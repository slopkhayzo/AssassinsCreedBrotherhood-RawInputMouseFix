// Host checks: the running exe must be the x86 ACBSP.exe layout this plugin was
// researched on. The SHA-256 is only logged; compatibility comes from the section
// layout and the byte signature of every hook site, checked once before hooking.
#include "common.h"

#include <bcrypt.h>
#include <cstdio>
#include <cstring>

namespace acb {
namespace {

struct ExpectedSection {
    const char* name;
    DWORD rva;
    DWORD minimumSize;
    DWORD requiredFlags;
    DWORD forbiddenFlags;
};

// From research\analysis\pe_info.py on the 2022-10-27 Ubisoft Connect build.
constexpr ExpectedSection kSections[] = {
    {".text", 0x00001000, 0x01f7caa5, IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ, IMAGE_SCN_MEM_WRITE},
    {".rdata", 0x01f7e000, 0x003f303d, IMAGE_SCN_MEM_READ, IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE},
    {".data", 0x02372000, 0x00291f84, IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE, IMAGE_SCN_MEM_EXECUTE},
};

struct ReferenceBuild {
    const char* sha256;
    const char* name;
};
// The Steam file is the Ubisoft Connect build wrapped in SteamStub 3.1: decrypted, its code is
// byte-identical, so both use the same hook sites.
constexpr ReferenceBuild kReferenceBuilds[] = {
    {"C0F6074082C99FAB4AA47715591966C727F569F32DBD3B0ACC34E43278ED157A", "reference build: Ubisoft Connect"},
    {"72FE53EB40CAE0F17D8EA457A6E12694B32A3B41D0D88B8BF79169750556C174", "reference build: Steam (SteamStub)"},
};

uintptr_t g_base = 0;
DWORD g_imageSize = 0;

const IMAGE_NT_HEADERS32* Headers() {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    return reinterpret_cast<const IMAGE_NT_HEADERS32*>(g_base + dos->e_lfanew);
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

uintptr_t ImageBase() {
    if (g_base == 0) g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return g_base;
}

void SetImageBaseForTest(uintptr_t base) {
    g_base = base;
    g_imageSize = Headers()->OptionalHeader.SizeOfImage;
}

bool IsInImage(uintptr_t address) {
    return address >= ImageBase() && address < ImageBase() + g_imageSize;
}

bool HostHasSection(const char* name) {
    const IMAGE_NT_HEADERS32* nt = Headers();
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (strncmp(reinterpret_cast<const char*>(sections[i].Name), name, IMAGE_SIZEOF_SHORT_NAME) == 0) return true;
    }
    return false;
}

bool ValidateHost() {
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = path;
    if (length > 0 && length < MAX_PATH) {
        const wchar_t* slash = wcsrchr(path, L'\\');
        if (slash != nullptr) name = slash + 1;
    }
    if (_wcsicmp(name, L"ACBSP.exe") != 0) {
        Log("host: %ls is not ACBSP.exe; no hooks installed", name);
        return false;
    }
    return ValidateHostLayout();
}

bool ValidateHostLayout() {
    ImageBase();
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        Log("host: no DOS header; no hooks installed");
        return false;
    }
    const IMAGE_NT_HEADERS32* nt = Headers();
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        Log("host: not a PE32 x86 image; no hooks installed");
        return false;
    }
    g_imageSize = nt->OptionalHeader.SizeOfImage;
    Log("host: ACBSP.exe base 0x%08X size 0x%08X timestamp %u", unsigned(g_base), unsigned(g_imageSize),
        unsigned(nt->FileHeader.TimeDateStamp));

    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (const ExpectedSection& expected : kSections) {
        const IMAGE_SECTION_HEADER* found = nullptr;
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (strncmp(reinterpret_cast<const char*>(sections[i].Name), expected.name,
                        IMAGE_SIZEOF_SHORT_NAME) == 0) {
                found = &sections[i];
                break;
            }
        }
        if (found == nullptr) {
            Log("host: section %s missing; no hooks installed", expected.name);
            return false;
        }
        if (found->VirtualAddress != expected.rva || found->Misc.VirtualSize < expected.minimumSize ||
            (found->Characteristics & expected.requiredFlags) != expected.requiredFlags ||
            (found->Characteristics & expected.forbiddenFlags) != 0) {
            Log("host: section %s at RVA 0x%08X size 0x%08X flags 0x%08X does not match the "
                "researched layout (RVA 0x%08X, size >= 0x%08X); no hooks installed",
                expected.name, unsigned(found->VirtualAddress), unsigned(found->Misc.VirtualSize),
                unsigned(found->Characteristics), unsigned(expected.rva), unsigned(expected.minimumSize));
            return false;
        }
    }
    Log("host: section layout matches");
    return true;
}

bool CheckSignature(const char* name, uintptr_t va, const char* pattern) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(Resolve(va));
    size_t index = 0;
    for (const char* p = pattern; *p != '\0';) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        if (p[0] == '?' && p[1] == '?') {
            p += 2;
            ++index;
            continue;
        }
        int high = HexValue(p[0]);
        int low = p[1] != '\0' ? HexValue(p[1]) : -1;
        if (high < 0 || low < 0) {
            Log("signature %s: bad pattern", name);
            return false;
        }
        uint8_t wanted = uint8_t(high * 16 + low);
        if (!IsInImage(uintptr_t(bytes + index)) || bytes[index] != wanted) {
            Log("signature %s at 0x%08X: byte %u is %02X, expected %02X; hook skipped", name,
                unsigned(va), unsigned(index), IsInImage(uintptr_t(bytes + index)) ? bytes[index] : 0,
                wanted);
            return false;
        }
        p += 2;
        ++index;
    }
    Log("signature %s at 0x%08X: ok (%u bytes)", name, unsigned(va), unsigned(index));
    return true;
}

void* PatchImport(const char* dll, const char* function, void* replacement) {
    const IMAGE_NT_HEADERS32* nt = Headers();
    const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) return nullptr;
    auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(g_base + directory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        const char* name = reinterpret_cast<const char*>(g_base + descriptor->Name);
        if (_stricmp(name, dll) != 0) continue;
        auto* names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(
            g_base + (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk : descriptor->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA32*>(g_base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(g_base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byName->Name), function) != 0) continue;
            DWORD oldProtect = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(DWORD), PAGE_READWRITE, &oldProtect)) return nullptr;
            void* previous = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<DWORD>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(DWORD), oldProtect, &oldProtect);
            return previous;
        }
    }
    return nullptr;
}

bool HashHostFile(char (&hex)[65]) {
    hex[0] = '\0';
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    bool ok = false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    static uint8_t buffer[1 << 16];
    uint8_t digest[32];
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        ok = true;
        DWORD read = 0;
        while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            if (BCryptHashData(hash, buffer, read, 0) != 0) {
                ok = false;
                break;
            }
        }
        ok = ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    }
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    if (!ok) return false;
    for (int i = 0; i < 32; ++i) sprintf_s(hex + i * 2, 3, "%02X", digest[i]);
    const char* name = "unknown build, layout matched";
    for (const ReferenceBuild& build : kReferenceBuilds) {
        if (strcmp(hex, build.sha256) == 0) name = build.name;
    }
    Log("host: SHA-256 %s (%s)", hex, name);
    return true;
}

} // namespace acb
