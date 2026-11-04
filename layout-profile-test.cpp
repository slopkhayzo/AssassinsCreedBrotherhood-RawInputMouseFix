// Offline check of the plugin against a real ACBSP.exe, without running the game:
// maps the exe into this process (no imports resolved, nothing executed), then runs the
// plugin's own layout validation, signature checks and hook installation against that
// private mapping, and checks that every hook site now jumps. The file is not modified.
//   layout-profile-test.exe <path\to\ACBSP.exe>
//   layout-profile-test.exe <path\to\Steam\ACBSP.exe> --decrypt-with <Steamless-unpacked copy>
// The Steam file's code is SteamStub-encrypted: the test checks that nothing is hooked while it
// is, then writes the decrypted code into the mapping (relocated, as at run time) and checks that
// the plugin's deferred install then hooks everything, as it does when the stub has run.
#include "src/common.h"

#include <MinHook.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;
int g_hooksInstalled = 0;

void Fail(const wchar_t* what) {
    fwprintf(stderr, L"FAIL: %s\n", what);
    ++g_failures;
}

void InstallAndCheck() {
    if (!acb::InstallInputHooks()) Fail(L"input hooks");
    if (reinterpret_cast<const BYTE*>(acb::Resolve(0x00b95d30))[0] != 0xE9) Fail(L"MouseMap does not jump");
    if (!acb::InstallCameraHooks()) Fail(L"camera hooks");
    if (reinterpret_cast<const BYTE*>(acb::Resolve(0x007a1240))[0] != 0xE9) Fail(L"the camera curve does not jump");
    if (reinterpret_cast<const BYTE*>(acb::Resolve(0x008d83f0))[0] != 0xE9) Fail(L"the first-person input does not jump");
    acb::g_config.frameStats = true;  // off by default; still check its import patch
    if (!acb::InstallD3D9Hooks()) Fail(L"the d3d9 import patch");
    ++g_hooksInstalled;
}

// What SteamStub does at start-up: replace .text with the decrypted code, relocated to the base.
bool WriteDecryptedText(HMODULE image, const wchar_t* unpackedPath) {
    HANDLE file = CreateFileW(unpackedPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::vector<BYTE> data(GetFileSize(file, nullptr));
    DWORD read = 0;
    const bool ok = ReadFile(file, data.data(), DWORD(data.size()), &read, nullptr) && read == data.size();
    CloseHandle(file);
    if (!ok) return false;

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(data.data());
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(data.data() + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    const IMAGE_SECTION_HEADER* text = nullptr;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sections[i].Name, ".text", 6) == 0) text = &sections[i];
    }
    if (text == nullptr) return false;

    BYTE* base = reinterpret_cast<BYTE*>(image);
    BYTE* destination = base + text->VirtualAddress;
    DWORD oldProtect = 0;
    if (!VirtualProtect(destination, text->Misc.VirtualSize, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    memcpy(destination, data.data() + text->PointerToRawData, text->Misc.VirtualSize);

    // Relocations inside .text, from the mapped image's own table. The delta comes from the file's
    // preferred base: the loader rewrites ImageBase in the mapped headers to the actual base.
    auto* mappedNt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const DWORD delta = DWORD(reinterpret_cast<uintptr_t>(base) - nt->OptionalHeader.ImageBase);
    const IMAGE_DATA_DIRECTORY& directory = mappedNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    const BYTE* block = base + directory.VirtualAddress;
    const BYTE* end = block + directory.Size;
    DWORD relocated = 0;
    while (block < end) {
        auto* header = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(block);
        if (header->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;
        const WORD* entries = reinterpret_cast<const WORD*>(block + sizeof(IMAGE_BASE_RELOCATION));
        const DWORD count = (header->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        for (DWORD i = 0; i < count; ++i) {
            if ((entries[i] >> 12) != IMAGE_REL_BASED_HIGHLOW) continue;
            const DWORD rva = header->VirtualAddress + (entries[i] & 0xFFF);
            if (rva < text->VirtualAddress || rva + 4 > text->VirtualAddress + text->Misc.VirtualSize) continue;
            *reinterpret_cast<DWORD*>(base + rva) += delta;
            ++relocated;
        }
        block += header->SizeOfBlock;
    }
    VirtualProtect(destination, text->Misc.VirtualSize, oldProtect, &oldProtect);
    acb::Log("layout-profile-test: wrote the decrypted .text (%u bytes, %u relocations)",
             unsigned(text->Misc.VirtualSize), unsigned(relocated));
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: %s <ACBSP.exe> [--decrypt-with <unpacked ACBSP.exe>]\n", argv[0]);
        return 2;
    }
    const wchar_t* unpacked = argc > 3 && wcscmp(argv[2], L"--decrypt-with") == 0 ? argv[3] : nullptr;
    HMODULE image = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (image == nullptr) {
        fwprintf(stderr, L"cannot map %s: %lu\n", argv[1], GetLastError());
        return 1;
    }
    acb::LogOpen(nullptr); // log beside this test exe
    acb::Log("layout-profile-test: %ls mapped at %p", argv[1], image);
    acb::SetImageBaseForTest(reinterpret_cast<uintptr_t>(image));

    if (!acb::ValidateHostLayout()) Fail(L"layout");
    if (g_failures == 0 && MH_Initialize() != MH_OK) Fail(L"MinHook");
    bool steamStub = false;
    if (g_failures == 0) {
        steamStub = acb::HostHasSection(".bind") && !acb::CodeLooksDecrypted();
        if (!steamStub) {
            InstallAndCheck();
        } else if (unpacked == nullptr) {
            Fail(L"the code is SteamStub-encrypted: pass --decrypt-with <unpacked copy>");
        } else {
            acb::WaitForDecryption(&InstallAndCheck);
            acb::RunDeferredTriggerForTest(); // a trigger while still encrypted must do nothing
            if (acb::DeferredInstallDoneForTest() || g_hooksInstalled != 0) Fail(L"hooked while encrypted");
            if (!WriteDecryptedText(image, unpacked)) Fail(L"writing the decrypted .text");
            if (!acb::CodeLooksDecrypted()) Fail(L"the decrypted .text isn't recognized");
            acb::RunDeferredTriggerForTest();
            if (!acb::DeferredInstallDoneForTest() || g_hooksInstalled != 1) Fail(L"no deferred install after decryption");
            acb::RunDeferredTriggerForTest(); // later triggers must not install twice
            if (g_hooksInstalled != 1) Fail(L"installed twice");
        }
        MH_Uninitialize();
    }
    acb::Log("layout-profile-test: %s", g_failures == 0 ? "passed" : "FAILED");
    if (g_failures == 0) {
        wprintf(L"ok: layout, signatures and hooks match %s%s\n", argv[1],
                steamStub ? L" (SteamStub: nothing hooked while encrypted, all hooks after decryption)" : L"");
    } else {
        fwprintf(stderr, L"FAIL: %d check(s) failed; see acb-raw-mouse-fix.log beside the test\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
