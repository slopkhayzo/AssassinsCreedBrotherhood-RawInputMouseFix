// Assassin's Creed Brotherhood Raw Input Mouse Fix: shared declarations.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>

#include "mouse_model.h"

namespace acb {

constexpr const char* kVersion = "1.0.1";

// Addresses below are VAs at the exe's preferred base; Resolve() maps them to the
// running image (the exe has ASLR).
constexpr uintptr_t kPreferredBase = 0x00400000;

// ---- log.cpp ---------------------------------------------------------------
void LogOpen(HMODULE self);
void Log(const char* format, ...);
const wchar_t* PluginDirectory();
double SecondsSinceStart();

// ---- config.cpp ------------------------------------------------------------
struct Config {
    // [Mouse]
    bool linearCamera = true;     // the third-person camera turns by raw counts (src\camera.cpp)
    float mouseScale = 25.0f;     // base: stick units per count on the camera's mouse branch
    float sensitivity = 1.0f;     // global multiplier (F9/F10), for every mouse camera
    float sensitivityX = 1.0f;    // horizontal multiplier
    float sensitivityY = 1.0f;    // vertical multiplier
    bool useRawInput = true;      // counts from WM_INPUT; otherwise from DirectInput's polls
    bool removeClamp = true;      // other cameras get the unclamped counts of each poll
    // [Diagnostics]
    bool statsLog = false;        // per-second counters
    bool rawSniff = false;        // compare WM_INPUT with DirectInput even when UseRawInput=0
    bool frameStats = false;      // development: D3D9 Present hooks (frame times; the game's Present isn't seen)
    int frameLimit = 0;           // development frame limiter (FPS), 0 = off
    bool watchRightStick = false; // hardware watchpoint on the pad's right stick
    int watchSeconds = 30;
};
extern Config g_config;
void LoadConfig();
bool SaveSensitivity(float sensitivity);

// ---- layout.cpp ------------------------------------------------------------
uintptr_t ImageBase();
inline uintptr_t Resolve(uintptr_t va) { return ImageBase() + (va - kPreferredBase); }
inline uintptr_t ToPreferred(uintptr_t address) { return address - ImageBase() + kPreferredBase; }
bool IsInImage(uintptr_t address);
// Validates that the host is the expected x86 ACBSP.exe layout. Logs every failure.
bool ValidateHost();
bool ValidateHostLayout();                 // the same without the module-name check
void SetImageBaseForTest(uintptr_t base);  // layout-profile-test: a private mapping of the exe
// Compares the bytes at a VA with a pattern like "55 8B EC ?? ?? 0F". Logs a mismatch.
bool CheckSignature(const char* name, uintptr_t va, const char* pattern);
bool HostHasSection(const char* name);
// Patches the exe's import of dll!function; returns the previous pointer or nullptr.
void* PatchImport(const char* dll, const char* function, void* replacement);
// SHA-256 of the exe on disk, as upper-case hex (empty on failure). Slow: background only.
bool HashHostFile(char (&hex)[65]);

// ---- input.cpp -------------------------------------------------------------
bool InstallInputHooks();
void ReportInputStats(double seconds);
void* PadObject(); // the keyboard+mouse device / virtual pad, once seen
DWORD LastRightStickBits(); // the right stick X the game last wrote (float bits), for the watch

// ---- rawinput.cpp ----------------------------------------------------------
void PollRawInputRegistration();
void ReportRawStats(double seconds);
// Signed DirectInput sums seen by the mouse map, for the raw-vs-DirectInput comparison.
void AddDirectInputCounts(long x, long y);
CountPair RawInputTotals();       // running totals, see mouse_model.h
CountPair DirectInputTotals();
long long LastRawInputTime();     // QueryPerformanceCounter time of the last raw packet

// ---- camera.cpp ------------------------------------------------------------
bool InstallCameraHooks();
bool LinearCameraActive();        // the fix is installed and not switched off with F8
void CameraKeys();                // F8 A/B, F9/F10 scale
void ReportCameraStats(double seconds);

// ---- deferred.cpp ----------------------------------------------------------
bool CodeLooksDecrypted();                  // false while SteamStub hasn't decrypted .text
bool WaitForDecryption(void (*install)());  // installs once the code is decrypted
void DeferredTick();                        // stats thread: fallback check, 60 s give-up
bool DeferredInstallDoneForTest();
void RunDeferredTriggerForTest();

// ---- d3d9hook.cpp ----------------------------------------------------------
bool InstallD3D9Hooks();
void ReportFrameStats(double seconds);

// ---- watch.cpp -------------------------------------------------------------
// F11 starts a watch window; once a second it re-arms all threads, reports, and disarms at the end.
void WatchTick(bool secondElapsed);
bool WatchInstall();
// watch-test only
void WatchStartForTest();
void WatchStopForTest();
int WatchFollowedSlotsForTest();
long WatchHitCountForTest();
long WatchHandlerCallsForTest();
void WatchSetStepStoresForTest(bool enabled);
void WatchSetSwallowLostForTest(bool enabled);
void WatchStressRewriteForTest();
long WatchLostStatusForTest();

} // namespace acb
