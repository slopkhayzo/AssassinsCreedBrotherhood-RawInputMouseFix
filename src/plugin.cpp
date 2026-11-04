// ACBRawMouseFix.asi entry point. Loaded by an ASI loader (Ultimate ASI Loader x86 as
// dinput8.dll). On a host that isn't the researched ACBSP.exe layout it logs why and
// stays inert. It exports nothing and never forwards DirectInput8Create.
#include "common.h"

#include <MinHook.h>
#include <cstdio>

namespace acb {
namespace {

HANDLE g_instanceMutex = nullptr;

bool AcquireInstanceGuard() {
    wchar_t name[64];
    swprintf_s(name, L"Local\\ACBRawMouseFix-%lu", GetCurrentProcessId());
    g_instanceMutex = CreateMutexW(nullptr, FALSE, name);
    if (g_instanceMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        Log("another copy of the plugin is already active in this process; this one stays inert");
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
        return false;
    }
    return g_instanceMutex != nullptr;
}

DWORD WINAPI StatsThread(void*) {
    char hash[65];
    HashHostFile(hash);

    double last = SecondsSinceStart();
    for (;;) {
        Sleep(50);
        const double now = SecondsSinceStart();
        const bool second = now - last >= 1.0;
        DeferredTick();
        WatchTick(second);
        CameraKeys();
        if (!second) continue;
        const double elapsed = now - last;
        last = now;
        PollRawInputRegistration();
        if (g_config.statsLog) {
            ReportInputStats(elapsed);
            ReportCameraStats(elapsed);
            ReportRawStats(elapsed);
            ReportFrameStats(elapsed);
        }
    }
}

void InstallHooks() {
    InstallInputHooks();
    InstallCameraHooks();
    InstallD3D9Hooks();
    if (WatchInstall()) Log("watch: press F11 in game to record the right-stick readers for %d s", g_config.watchSeconds);
}

void Initialize(HMODULE self) {
    LogOpen(self);
    Log("ACBRawMouseFix %s", kVersion);
    if (!AcquireInstanceGuard()) return;
    LoadConfig();
    if (!ValidateHost()) return;

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        Log("MinHook initialization failed (%d); no hooks installed", int(status));
        return;
    }
    // The Steam build's code is still encrypted by SteamStub at this point (src\deferred.cpp).
    if (HostHasSection(".bind") && !CodeLooksDecrypted()) WaitForDecryption(&InstallHooks);
    else InstallHooks();

    HANDLE thread = CreateThread(nullptr, 0, &StatsThread, nullptr, 0, nullptr);
    if (thread != nullptr) CloseHandle(thread);
}

} // namespace
} // namespace acb

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        acb::Initialize(instance);
    }
    return TRUE;
}
