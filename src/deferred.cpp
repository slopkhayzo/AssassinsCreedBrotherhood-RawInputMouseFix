// Steam build: ACBSP.exe is wrapped in SteamStub DRM 3.1 (x86). The .bind section's stub decrypts
// .text at start-up and then jumps to the game's entry point. The decrypted code is byte-identical
// to the Ubisoft Connect build (research\docs\PLAN.md §2), so every hook site is the same; but the
// ASI loader loads this plugin before the stub runs, when .text is still encrypted.
//
// So the hooks wait: the plugin patches a few of the exe's imports that the C runtime calls right
// after the entry point. The first such call that finds the code decrypted installs the hooks, once,
// and restores the imports. The stats thread checks too, in case none of those calls comes.
#include "common.h"

#include <cstring>

namespace acb {
namespace {

using GetSystemTimeAsFileTimeFn = void(WINAPI*)(LPFILETIME);
using GetCommandLineAFn = LPSTR(WINAPI*)();
using GetStartupInfoAFn = void(WINAPI*)(LPSTARTUPINFOA);

GetSystemTimeAsFileTimeFn g_getSystemTimeAsFileTime = nullptr;
GetCommandLineAFn g_getCommandLineA = nullptr;
GetStartupInfoAFn g_getStartupInfoA = nullptr;

void (*g_install)() = nullptr;
std::atomic<bool> g_waiting{false};
std::atomic<bool> g_done{false};
double g_waitStarted = 0;

void RestoreImports() {
    if (g_getSystemTimeAsFileTime != nullptr)
        PatchImport("kernel32.dll", "GetSystemTimeAsFileTime", reinterpret_cast<void*>(g_getSystemTimeAsFileTime));
    if (g_getCommandLineA != nullptr)
        PatchImport("kernel32.dll", "GetCommandLineA", reinterpret_cast<void*>(g_getCommandLineA));
    if (g_getStartupInfoA != nullptr)
        PatchImport("kernel32.dll", "GetStartupInfoA", reinterpret_cast<void*>(g_getStartupInfoA));
}

void InstallIfDecrypted(const char* by) {
    if (!g_waiting.load() || g_done.load() || !CodeLooksDecrypted()) return;
    if (g_done.exchange(true)) return;
    RestoreImports();
    Log("host: the code is decrypted (%s, %.3f s after load); installing the hooks", by,
        SecondsSinceStart() - g_waitStarted);
    g_install();
}

void WINAPI GetSystemTimeAsFileTimeTrigger(LPFILETIME time) {
    InstallIfDecrypted("GetSystemTimeAsFileTime");
    g_getSystemTimeAsFileTime(time);
}

LPSTR WINAPI GetCommandLineATrigger() {
    InstallIfDecrypted("GetCommandLineA");
    return g_getCommandLineA();
}

void WINAPI GetStartupInfoATrigger(LPSTARTUPINFOA info) {
    InstallIfDecrypted("GetStartupInfoA");
    g_getStartupInfoA(info);
}

} // namespace

bool CodeLooksDecrypted() {
    // The mouse map's prologue: plain code in the decrypted image, noise while encrypted.
    static const BYTE kPrologue[] = {0x55, 0x8B, 0xEC, 0x0F, 0x57, 0xC0, 0x53, 0x8B, 0x5D, 0x0C, 0x56, 0x8B, 0x75, 0x10};
    return memcmp(reinterpret_cast<const void*>(Resolve(0x00b95d30)), kPrologue, sizeof(kPrologue)) == 0;
}

bool WaitForDecryption(void (*install)()) {
    g_install = install;
    g_waitStarted = SecondsSinceStart();
    g_getSystemTimeAsFileTime = reinterpret_cast<GetSystemTimeAsFileTimeFn>(PatchImport(
        "kernel32.dll", "GetSystemTimeAsFileTime", reinterpret_cast<void*>(&GetSystemTimeAsFileTimeTrigger)));
    g_getCommandLineA = reinterpret_cast<GetCommandLineAFn>(
        PatchImport("kernel32.dll", "GetCommandLineA", reinterpret_cast<void*>(&GetCommandLineATrigger)));
    g_getStartupInfoA = reinterpret_cast<GetStartupInfoAFn>(
        PatchImport("kernel32.dll", "GetStartupInfoA", reinterpret_cast<void*>(&GetStartupInfoATrigger)));
    g_waiting.store(true);
    Log("host: SteamStub-encrypted code; the hooks wait for it to be decrypted (import triggers: %s %s %s)",
        g_getSystemTimeAsFileTime ? "GetSystemTimeAsFileTime" : "-", g_getCommandLineA ? "GetCommandLineA" : "-",
        g_getStartupInfoA ? "GetStartupInfoA" : "-");
    return true;
}

void DeferredTick() {
    if (!g_waiting.load() || g_done.load()) return;
    InstallIfDecrypted("stats thread");
    if (!g_done.load() && SecondsSinceStart() - g_waitStarted > 60.0) {
        // Something else is wrong: stop waiting, leave the game untouched.
        g_waiting.store(false);
        RestoreImports();
        Log("host: the code was still encrypted 60 s after load; no hooks installed");
    }
}

bool DeferredInstallDoneForTest() { return g_done.load(); }

void RunDeferredTriggerForTest() { InstallIfDecrypted("test"); }

} // namespace acb
