// Offline check of the plugin against a real ACBSP.exe, without running the game:
// maps the exe into this process (no imports resolved, nothing executed), then runs the
// plugin's own layout validation, signature checks and hook installation against that
// private mapping, and checks that every hook site now jumps. The file is not modified.
//   layout-profile-test.exe <path\to\ACBSP.exe>
#include "src/common.h"

#include <MinHook.h>
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: %s <ACBSP.exe>\n", argv[0]);
        return 2;
    }
    HMODULE image = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (image == nullptr) {
        fwprintf(stderr, L"cannot map %s: %lu\n", argv[1], GetLastError());
        return 1;
    }
    acb::LogOpen(nullptr); // log beside this test exe
    acb::Log("layout-profile-test: %ls mapped at %p", argv[1], image);
    acb::SetImageBaseForTest(reinterpret_cast<uintptr_t>(image));

    int failures = 0;
    if (!acb::ValidateHostLayout()) ++failures;
    if (failures == 0) {
        if (MH_Initialize() != MH_OK) ++failures;
        if (!acb::InstallInputHooks()) ++failures;
        const BYTE* mouseMap = reinterpret_cast<const BYTE*>(acb::Resolve(0x00b95d30));
        if (mouseMap[0] != 0xE9) {
            fwprintf(stderr, L"FAIL: MouseMap does not jump after installation\n");
            ++failures;
        }
        if (!acb::InstallCameraHooks()) ++failures;
        const BYTE* curve = reinterpret_cast<const BYTE*>(acb::Resolve(0x007a1240));
        if (curve[0] != 0xE9) {
            fwprintf(stderr, L"FAIL: the camera curve does not jump after installation\n");
            ++failures;
        }
        const BYTE* firstPerson = reinterpret_cast<const BYTE*>(acb::Resolve(0x008d83f0));
        if (firstPerson[0] != 0xE9) {
            fwprintf(stderr, L"FAIL: the first-person camera input does not jump after installation\n");
            ++failures;
        }
        acb::g_config.frameStats = true;          // off by default; still check its import patch
        if (!acb::InstallD3D9Hooks()) ++failures; // patches the mapping's d3d9 import
        MH_Uninitialize();
    }
    acb::Log("layout-profile-test: %s", failures == 0 ? "passed" : "FAILED");
    if (failures == 0) wprintf(L"ok: layout, signatures and hooks match %s\n", argv[1]);
    else fwprintf(stderr, L"FAIL: %d check(s) failed; see acb-raw-mouse-fix.log beside the test\n", failures);
    return failures == 0 ? 0 : 1;
}
