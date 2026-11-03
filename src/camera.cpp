// The fix for the third-person camera (research\docs\PLAN.md §3.6).
//
// The polar camera's update 0x007a2470 reads the virtual right stick, which for the mouse holds raw
// counts per frame, and always passes it through a gamepad response curve, 0x007a1240:
//     r' = pow(sqrt(x*x + y*y), 2.0),   x = cos(a) * r',   y = sin(a) * r'
// Its mouse branch then uses no frame time: x *= speed * sensitivity * 1e-5. So the turn per frame
// goes with counts squared: the turn per count is proportional to the mouse speed (the
// acceleration) and inversely proportional to the frame rate (the sensitivity change).
//
// The hook keeps that function for gamepads and camera keys. On the game's own mouse branch it
// replaces the stick with the counts since this camera's previous update, times MouseScale. Nothing
// is multiplied by a frame time, so the turn per count is constant. Speeds, sensitivity sliders,
// pitch limits and collision after this point are all native.
#include "common.h"
#include "mouse_model.h"

#include <MinHook.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace acb {
namespace {

constexpr uintptr_t kCurve = 0x007a1240;
constexpr uintptr_t kBranch = 0x007a2648;
constexpr uintptr_t kReaderOptions = 0x007a0be6;
constexpr uintptr_t kOptionsGetter = 0x00444e50;
constexpr uintptr_t kMouseSpeeds = 0x007a26d5;

// Camera fields.
constexpr size_t kStickX = 0x230;
constexpr size_t kStickY = 0x234;
constexpr size_t kBothAxes = 0x20d;   // 0: the reader keeps only the dominant axis
constexpr size_t kSpeedYaw = 0x1bc;
constexpr size_t kSpeedPitch = 0x204;
// Options fields.
constexpr size_t kInvertX = 0x13e;
constexpr size_t kInvertY = 0x13f;
constexpr size_t kSensitivityYaw = 0x118;
constexpr size_t kSensitivityPitch = 0x1c;

constexpr const char* kCurveBytes =
    "55 8B EC 83 EC 0C 56 8B F1 D9 86 30 02 00 00 D9 86 34 02 00 00 E8 ?? ?? ?? ?? D9 5D F8 D9 45 F8 D9 5D FC "
    "D9 86 34 02 00 00 D9 86 30 02 00 00 DC C8 D9 C1 DE CA DE C1 D9 5D F8 D9 45 F8 E8 ?? ?? ?? ?? D9 5D F8 "
    "D9 45 F8 DD 05 ?? ?? ?? ?? E8";
constexpr unsigned kCurveExponentOperand = 0x4A;

// mouse = deviceType([manager]) == 0 && [flag] != 0; otherwise gamepad remap; then the curve.
constexpr const char* kBranchBytes =
    "8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 75 0E 38 05 ?? ?? ?? ?? 74 06 C6 45 FE 01 EB 0B 8B CE "
    "C6 45 FE 00 E8 ?? ?? ?? ?? 8B CE E8 ?? ?? ?? ??";
constexpr unsigned kBranchManager = 2;
constexpr unsigned kBranchDeviceTypeCall = 6;
constexpr unsigned kBranchFlag = 17;
constexpr unsigned kBranchCurveCall = 42;

// The stick's reader stage: invert options and the dominant-axis rule, as the fix must repeat them.
constexpr const char* kReaderOptionsBytes =
    "E8 ?? ?? ?? ?? 80 B8 3E 01 00 00 00 F2 0F 10 05 ?? ?? ?? ?? 74 0F F3 0F 5A 4D 08 F2 0F 59 C8 F2 0F 5A C9 "
    "EB 05 F3 0F 10 4D 08 80 B8 3F 01 00 00 00 74 0F F3 0F 5A 55 FC F2 0F 59 D0 F2 0F 5A C2 EB 05 "
    "F3 0F 10 45 FC 80 BE 0D 02 00 00 00";
constexpr const char* kOptionsGetterBytes = "A1 ?? ?? ?? ?? C3";

constexpr const char* kMouseSpeedsBytes =
    "E8 ?? ?? ?? ?? F3 0F 10 80 18 01 00 00 F3 0F 11 45 EC E8 ?? ?? ?? ?? 80 BE 11 02 00 00 00 F3 0F 10 58 1C "
    "F3 0F 10 86 BC 01 00 00";

// First-person camera (EulerModifier) input, 0x008d83f0: thiscall (arg1, dt), ret 8. Its mouse branch
// is already linear and has no dt: delta[+0x184] = speed * reader[+0x14] * arg1 * sens * 0.001, and
// delta[+0x188] likewise from reader[+0x24]. Only the multipliers are applied here.
constexpr uintptr_t kFirstPersonInput = 0x008d83f0;
constexpr uintptr_t kFirstPersonReaders = 0x008d8432;
constexpr uintptr_t kFirstPersonBranch = 0x008d85e2;
constexpr uintptr_t kFirstPersonStoreA = 0x008d8635;
constexpr uintptr_t kFirstPersonStoreB = 0x008d86c9;
constexpr uintptr_t kPadAxisRead = 0x00708ba0;
constexpr size_t kFirstPersonDeltaA = 0x184;
constexpr size_t kFirstPersonDeltaB = 0x188;
constexpr size_t kFirstPersonReaderA = 0x14;
constexpr size_t kFirstPersonReaderB = 0x24;

constexpr const char* kFirstPersonInputBytes =
    "55 8B EC 83 EC 10 53 56 8B F1 E8 ?? ?? ?? ?? 33 DB 84 C0 74 2D 38 9E 90 00 00 00 75 25";
constexpr const char* kFirstPersonReadersBytes =
    "8B 4E 14 3B CB 74 0C 8B 01 8B 50 1C FF D2 D9 5D FC EB 08 0F 57 C0 F3 0F 11 45 FC "
    "8B 4E 24 3B CB 74 0C 8B 01 8B 50 1C FF D2 D9 5D F8";
constexpr const char* kFirstPersonBranchBytes =
    "8B 0D ?? ?? ?? ?? F3 0F 11 45 F4 E8 ?? ?? ?? ?? 85 C0 75 6D 38 1D ?? ?? ?? ?? 74 65 F3 0F 10 46 18";
constexpr const char* kFirstPersonStoreABytes = "F2 0F 5A D2 F3 0F 11 96 84 01 00 00";
constexpr const char* kFirstPersonStoreBBytes = "66 0F 5A C2 F3 0F 11 86 88 01 00 00";

// A NaN with a payload no computation produces: the deltas weren't written in this call.
constexpr DWORD kUnwritten = 0x7FC0ACB1;

using FirstPersonInputFn = void(__fastcall*)(BYTE* camera, void* edx, float arg1, float dt);
FirstPersonInputFn g_firstPersonInput = nullptr;
std::atomic<long> g_firstPersonUpdates{0};
const void* g_firstPersonLogged = nullptr;

using CurveFn = void(__fastcall*)(BYTE* camera, void* edx);
using DeviceTypeFn = int(__fastcall*)(void* manager);

CurveFn g_curve = nullptr;
DeviceTypeFn g_deviceType = nullptr;
void** g_manager = nullptr;
const volatile BYTE* g_mouseFlag = nullptr;
BYTE** g_options = nullptr;

std::atomic<bool> g_linear{false};
std::atomic<bool> g_installed{false};
std::atomic<float> g_scale{1.0f};        // MouseScale: base calibration of the third-person camera
std::atomic<float> g_sensitivity{1.0f};  // global multiplier, F9/F10
std::atomic<float> g_sensitivityX{1.0f};
std::atomic<float> g_sensitivityY{1.0f};
CameraCounts g_counts;

long long g_frequency = 1;
long long g_maxGap = 0;
long long g_minIdle = 0;
long long g_rawFallbackAge = 0;

bool g_f8Down = false;
bool g_f9Down = false;
bool g_f10Down = false;

long long Now() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

// Statistics, under a small lock: they are combined across camera threads.
struct Stats {
    long updates = 0;
    long delivered = 0;
    long long appliedX = 0;
    long long appliedY = 0;
    long long nativeX = 0;
    long long nativeY = 0;
    double stockWeighted = 0.0; // sum of r * |x|: the native turn in units of the speed factor
    long signDisagreeX = 0;
    long signDisagreeY = 0;
    long dominantOnly = 0;
    long diFallback = 0;
    long gapDrops = 0;
    long idleDrops = 0;
    float speedYaw = 0.0f;
    float speedPitch = 0.0f;
    float sensitivityYaw = 0.0f;
    float sensitivityPitch = 0.0f;
};
Stats g_stats;
std::atomic_flag g_statsLock = ATOMIC_FLAG_INIT;

void LockStats() {
    while (g_statsLock.test_and_set(std::memory_order_acquire)) {
    }
}
void UnlockStats() { g_statsLock.clear(std::memory_order_release); }

bool MouseBranch() {
    return g_deviceType(*g_manager) == 0 && *g_mouseFlag != 0;
}

long Magnitude(long value) { return value < 0 ? -value : value; }

void __fastcall CurveHook(BYTE* camera, void* edx) {
    if (!g_linear.load(std::memory_order_relaxed) || !MouseBranch()) {
        g_curve(camera, edx);
        return;
    }

    float* stickX = reinterpret_cast<float*>(camera + kStickX);
    float* stickY = reinterpret_cast<float*>(camera + kStickY);
    const float nativeX = *stickX;
    const float nativeY = *stickY;
    // Real input is whole counts. Anything smaller is what an earlier update left here when the
    // game skipped the read (input blocked): the game's own path handles that.
    const bool delivered = std::fabs(nativeX) >= 1.0f || std::fabs(nativeY) >= 1.0f;

    CountSnapshot totals;
    totals.raw = RawInputTotals();
    totals.dinput = DirectInputTotals();
    const long long now = Now();
    const CameraCounts::Taken taken = g_counts.Take(camera, totals, delivered, now, g_maxGap, g_minIdle);

    CountPair counts;
    bool usedFallback = false;
    if (taken.apply) {
        counts = taken.counts.raw;
        // DirectInput can move to a new raw-input window after a focus change until the next poll
        // subclasses it. Don't lose the camera meanwhile.
        const bool rawIsSilent = counts.x == 0 && counts.y == 0 && now - LastRawInputTime() > g_rawFallbackAge;
        if (!g_config.useRawInput || rawIsSilent) {
            usedFallback = g_config.useRawInput && (taken.counts.dinput.x != 0 || taken.counts.dinput.y != 0);
            counts = taken.counts.dinput;
        }
    }

    const BYTE* options = *g_options;
    bool dominantOnly = false;
    if (taken.apply) {
        // Natively x = -stick.x = -lX and y = -stick.y = lY, then the invert options.
        float x = -float(counts.x);
        float y = float(counts.y);
        if (options[kInvertX] != 0) x = -x;
        if (options[kInvertY] != 0) y = -y;
        if (camera[kBothAxes] == 0) {
            dominantOnly = true;
            if (std::fabs(y) >= std::fabs(x)) x = 0.0f;
            else y = 0.0f;
        }
        const float scale = g_scale.load(std::memory_order_relaxed) * g_sensitivity.load(std::memory_order_relaxed);
        *stickX = x * scale * g_sensitivityX.load(std::memory_order_relaxed);
        *stickY = y * scale * g_sensitivityY.load(std::memory_order_relaxed);
    } else {
        g_curve(camera, edx);
    }

    LockStats();
    Stats& s = g_stats;
    ++s.updates;
    s.gapDrops += taken.gapDrops;
    s.idleDrops += taken.idleDrops;
    if (taken.apply) {
        ++s.delivered;
        s.appliedX += Magnitude(counts.x);
        s.appliedY += Magnitude(counts.y);
        s.nativeX += long(std::fabs(nativeX));
        s.nativeY += long(std::fabs(nativeY));
        s.stockWeighted += std::sqrt(double(nativeX) * nativeX + double(nativeY) * nativeY) * std::fabs(nativeX);
        // The computed direction must agree with the game's when both moved the same way this update.
        if (nativeX != 0.0f && counts.x != 0 && !dominantOnly) {
            const float x = -float(counts.x) * (options[kInvertX] != 0 ? -1.0f : 1.0f);
            if ((x < 0.0f) != (nativeX < 0.0f)) ++s.signDisagreeX;
        }
        if (nativeY != 0.0f && counts.y != 0 && !dominantOnly) {
            const float y = float(counts.y) * (options[kInvertY] != 0 ? -1.0f : 1.0f);
            if ((y < 0.0f) != (nativeY < 0.0f)) ++s.signDisagreeY;
        }
        if (dominantOnly) ++s.dominantOnly;
        if (usedFallback) ++s.diFallback;
    }
    s.speedYaw = *reinterpret_cast<const float*>(camera + kSpeedYaw);
    s.speedPitch = *reinterpret_cast<const float*>(camera + kSpeedPitch);
    s.sensitivityYaw = *reinterpret_cast<const float*>(options + kSensitivityYaw);
    s.sensitivityPitch = *reinterpret_cast<const float*>(options + kSensitivityPitch);
    UnlockStats();
}

template <typename T>
T OperandAt(uintptr_t va, unsigned offset) {
    return *reinterpret_cast<const T*>(Resolve(va) + offset);
}

uintptr_t RelativeCallTarget(uintptr_t va, unsigned offset) {
    const uintptr_t site = Resolve(va) + offset;
    return site + 5 + OperandAt<int32_t>(va, offset + 1);
}

bool IsUnwritten(const float* value) {
    DWORD bits;
    memcpy(&bits, value, sizeof(bits));
    return bits == kUnwritten;
}

void MarkUnwritten(float* value) { memcpy(value, &kUnwritten, sizeof(kUnwritten)); }

// Which axis a delta turns: a PadAxisReader says so itself (stick index +8, axis +0xc: 0 horizontal).
// The first-person cameras seen in play use other reader nodes; for those the game's own code decides:
// it multiplies +0x184 by the Y look sensitivity (options +0x1c) and +0x188 by the X one (+0x118),
// the same options the third-person camera uses for pitch and yaw. So +0x184 is vertical.
float AxisMultiplier(const BYTE* camera, size_t readerField, int optionAxis, int* axis, bool* fromReader) {
    const BYTE* reader = *reinterpret_cast<const BYTE* const*>(camera + readerField);
    *axis = optionAxis;
    *fromReader = false;
    if (reader != nullptr) {
        const uintptr_t* vtable = *reinterpret_cast<const uintptr_t* const*>(reader);
        if (vtable != nullptr && vtable[0x1c / 4] == Resolve(kPadAxisRead)) {
            *axis = *reinterpret_cast<const int*>(reader + 0xc) == 0 ? 0 : 1;
            *fromReader = true;
        }
    }
    return *axis == 0 ? g_sensitivityX.load(std::memory_order_relaxed) : g_sensitivityY.load(std::memory_order_relaxed);
}

// Check of that mapping, in the log: in updates where the mouse moved only horizontally, only the
// horizontal delta should move, and the same for vertical.
struct MappingCheck {
    std::atomic<long> horizontalOnlyA{0}, horizontalOnlyB{0}, verticalOnlyA{0}, verticalOnlyB{0};
};
MappingCheck g_mappingCheck;
CountPair g_firstPersonLastTotals;

void __fastcall FirstPersonInputHook(BYTE* camera, void* edx, float arg1, float dt) {
    float* deltaA = reinterpret_cast<float*>(camera + kFirstPersonDeltaA);
    float* deltaB = reinterpret_cast<float*>(camera + kFirstPersonDeltaB);
    const float previousA = *deltaA;
    const float previousB = *deltaB;
    MarkUnwritten(deltaA);
    MarkUnwritten(deltaB);
    g_firstPersonInput(camera, edx, arg1, dt);
    // The function returns early without writing the deltas when input is blocked. Scaling the
    // leftover values every update would compound them, so restore those and leave them alone.
    const bool written = !IsUnwritten(deltaA) && !IsUnwritten(deltaB);
    if (IsUnwritten(deltaA)) *deltaA = previousA;
    if (IsUnwritten(deltaB)) *deltaB = previousB;
    if (!written || !g_linear.load(std::memory_order_relaxed) || !MouseBranch()) return;

    // The mouse counts since the previous first-person update, for the mapping check.
    const CountPair totals = DirectInputTotals();
    const long movedX = long(static_cast<unsigned long>(totals.x) - static_cast<unsigned long>(g_firstPersonLastTotals.x));
    const long movedY = long(static_cast<unsigned long>(totals.y) - static_cast<unsigned long>(g_firstPersonLastTotals.y));
    g_firstPersonLastTotals = totals;
    if (movedX != 0 && movedY == 0) {
        if (*deltaA != 0.0f) g_mappingCheck.horizontalOnlyA.fetch_add(1, std::memory_order_relaxed);
        if (*deltaB != 0.0f) g_mappingCheck.horizontalOnlyB.fetch_add(1, std::memory_order_relaxed);
    } else if (movedY != 0 && movedX == 0) {
        if (*deltaA != 0.0f) g_mappingCheck.verticalOnlyA.fetch_add(1, std::memory_order_relaxed);
        if (*deltaB != 0.0f) g_mappingCheck.verticalOnlyB.fetch_add(1, std::memory_order_relaxed);
    }

    int axisA = 1;
    int axisB = 0;
    bool readerA = false;
    bool readerB = false;
    const float global = g_sensitivity.load(std::memory_order_relaxed);
    *deltaA *= global * AxisMultiplier(camera, kFirstPersonReaderA, 1, &axisA, &readerA);
    *deltaB *= global * AxisMultiplier(camera, kFirstPersonReaderB, 0, &axisB, &readerB);
    g_firstPersonUpdates.fetch_add(1, std::memory_order_relaxed);
    if (g_firstPersonLogged != camera) {
        g_firstPersonLogged = camera;
        Log("first-person camera %p: delta +0x184 %s (%s), +0x188 %s (%s)", camera,
            axisA == 0 ? "horizontal" : "vertical", readerA ? "from its reader" : "from the game's sensitivity options",
            axisB == 0 ? "horizontal" : "vertical", readerB ? "from its reader" : "from the game's sensitivity options");
    }
}

bool InstallFirstPersonHook() {
    if (!CheckSignature("FirstPersonInput", kFirstPersonInput, kFirstPersonInputBytes) ||
        !CheckSignature("FirstPersonReaders", kFirstPersonReaders, kFirstPersonReadersBytes) ||
        !CheckSignature("FirstPersonBranch", kFirstPersonBranch, kFirstPersonBranchBytes) ||
        !CheckSignature("FirstPersonStoreA", kFirstPersonStoreA, kFirstPersonStoreABytes) ||
        !CheckSignature("FirstPersonStoreB", kFirstPersonStoreB, kFirstPersonStoreBBytes)) {
        return false;
    }
    // Its mouse condition must be the same one the third-person hook uses.
    if (OperandAt<uintptr_t>(kFirstPersonBranch, 2) != reinterpret_cast<uintptr_t>(g_manager) ||
        OperandAt<uintptr_t>(kFirstPersonBranch, 22) != reinterpret_cast<uintptr_t>(g_mouseFlag) ||
        RelativeCallTarget(kFirstPersonBranch, 11) != reinterpret_cast<uintptr_t>(g_deviceType)) {
        Log("signature FirstPersonBranch: a different mouse condition; hook skipped");
        return false;
    }
    void* target = reinterpret_cast<void*>(Resolve(kFirstPersonInput));
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&FirstPersonInputHook),
                                     reinterpret_cast<void**>(&g_firstPersonInput));
    if (status == MH_OK) status = MH_EnableHook(target);
    Log("hook FirstPersonInput: %s", status == MH_OK ? "installed" : "MinHook error");
    return status == MH_OK;
}

bool CheckBindings() {
    const uintptr_t exponentAddress = OperandAt<uintptr_t>(kCurve, kCurveExponentOperand);
    if (!IsInImage(exponentAddress) || *reinterpret_cast<const double*>(exponentAddress) != 2.0) {
        Log("signature CameraCurve: the exponent isn't 2.0; hook skipped");
        return false;
    }
    if (RelativeCallTarget(kBranch, kBranchCurveCall) != Resolve(kCurve)) {
        Log("signature CameraBranch: the branch doesn't call the curve; hook skipped");
        return false;
    }
    const uintptr_t manager = OperandAt<uintptr_t>(kBranch, kBranchManager);
    const uintptr_t flag = OperandAt<uintptr_t>(kBranch, kBranchFlag);
    const uintptr_t deviceType = RelativeCallTarget(kBranch, kBranchDeviceTypeCall);
    const uintptr_t options = OperandAt<uintptr_t>(kOptionsGetter, 1);
    if (!IsInImage(manager) || !IsInImage(flag) || !IsInImage(deviceType) || !IsInImage(options)) {
        Log("signature CameraBranch: an operand points outside the image; hook skipped");
        return false;
    }
    if (RelativeCallTarget(kReaderOptions, 0) != Resolve(kOptionsGetter) ||
        RelativeCallTarget(kMouseSpeeds, 0) != Resolve(kOptionsGetter)) {
        Log("signature CameraOptions: the options getter differs; hook skipped");
        return false;
    }
    g_manager = reinterpret_cast<void**>(manager);
    g_mouseFlag = reinterpret_cast<const volatile BYTE*>(flag);
    g_deviceType = reinterpret_cast<DeviceTypeFn>(deviceType);
    g_options = reinterpret_cast<BYTE**>(options);
    Log("signature CameraBranch: mouse condition and options bound");
    return true;
}

void ApplySensitivityKey(float factor) {
    const float sensitivity = g_sensitivity.load() * factor;
    g_sensitivity.store(sensitivity);
    g_config.sensitivity = sensitivity;
    const bool saved = SaveSensitivity(sensitivity);
    Log("Sensitivity %.3f%s", sensitivity, saved ? "" : " (not saved: the ini isn't writable)");
}

} // namespace

bool InstallCameraHooks() {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_frequency = frequency.QuadPart;
    g_maxGap = g_frequency / 4;         // 250 ms: the camera wasn't running
    g_minIdle = g_frequency / 20;       // 50 ms: input not delivered
    g_rawFallbackAge = g_frequency / 10; // 100 ms without a raw packet while DirectInput has counts
    g_scale.store(g_config.mouseScale);
    g_sensitivity.store(g_config.sensitivity);
    g_sensitivityX.store(g_config.sensitivityX);
    g_sensitivityY.store(g_config.sensitivityY);
    g_linear.store(g_config.linearCamera);

    if (!CheckSignature("CameraCurve", kCurve, kCurveBytes) || !CheckSignature("CameraBranch", kBranch, kBranchBytes) ||
        !CheckSignature("CameraReaderOptions", kReaderOptions, kReaderOptionsBytes) ||
        !CheckSignature("OptionsGetter", kOptionsGetter, kOptionsGetterBytes) ||
        !CheckSignature("CameraMouseSpeeds", kMouseSpeeds, kMouseSpeedsBytes) || !CheckBindings()) {
        g_linear.store(false);
        return false;
    }
    void* target = reinterpret_cast<void*>(Resolve(kCurve));
    MH_STATUS status =
        MH_CreateHook(target, reinterpret_cast<void*>(&CurveHook), reinterpret_cast<void**>(&g_curve));
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK) {
        Log("hook CameraCurve: MinHook error %d", int(status));
        g_linear.store(false);
        return false;
    }
    g_installed.store(true);
    Log("hook CameraCurve: installed (LinearCamera=%d MouseScale=%.2f Sensitivity=%.3f X %.3f Y %.3f)",
        g_linear.load(), g_scale.load(), g_sensitivity.load(), g_sensitivityX.load(), g_sensitivityY.load());
    InstallFirstPersonHook();
    return true;
}

bool LinearCameraActive() { return g_installed.load() && g_linear.load(std::memory_order_relaxed); }

void CameraKeys() {
    if (!g_installed.load()) return;
    DWORD foregroundProcess = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
    const bool foreground = foregroundProcess == GetCurrentProcessId();
    const bool f8 = foreground && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    const bool f9 = foreground && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    const bool f10 = foreground && (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f8 && !g_f8Down) {
        const bool linear = !g_linear.load();
        g_linear.store(linear);
        Log("F8: %s", linear ? "fix ON (linear camera)" : "fix OFF (stock camera)");
    }
    if (f9 && !g_f9Down) ApplySensitivityKey(1.0f / 1.05f);
    if (f10 && !g_f10Down) ApplySensitivityKey(1.05f);
    g_f8Down = f8;
    g_f9Down = f9;
    g_f10Down = f10;
}

void ReportCameraStats(double seconds) {
    LockStats();
    const Stats s = g_stats;
    g_stats = Stats{};
    UnlockStats();
    if (s.updates == 0 && g_firstPersonUpdates.load() == 0) return;
    const float scale = g_scale.load() * g_sensitivity.load();
    // Yaw units are radians if the constants mean what they appear to; turn per count = scale * K.
    const double yawPerCount =
        std::fabs(double(s.speedYaw)) * s.sensitivityYaw * 1e-5 * scale * g_sensitivityX.load();
    const double cmPer360 = yawPerCount > 0.0 ? (2.0 * 3.14159265358979 / yawPerCount) / (800.0 / 2.54) : 0.0;
    Log("camera: %.0f updates/s on %d camera(s), delivered %ld | applied |x| %lld |y| %lld, game's stick |x| %lld |y| "
        "%lld | stock multiplier here %.1f | sign disagree %ld/%ld, dominant-only %ld, DI fallback %ld, dropped gap %ld "
        "idle %ld | %s scale %.2f (x %.3f y %.3f) speed %.2f/%.2f game sens %.3f/%.3f = %.1f cm/360 at 800 DPI | "
        "first person %ld updates",
        s.updates / seconds, g_counts.ActiveCameras(Now(), g_maxGap), s.delivered, s.appliedX, s.appliedY, s.nativeX,
        s.nativeY, s.nativeX > 0 ? s.stockWeighted / double(s.nativeX) : 0.0, s.signDisagreeX, s.signDisagreeY,
        s.dominantOnly, s.diFallback, s.gapDrops, s.idleDrops, g_linear.load() ? "ON" : "OFF", scale,
        g_sensitivityX.load(), g_sensitivityY.load(), s.speedYaw, s.speedPitch, s.sensitivityYaw, s.sensitivityPitch,
        cmPer360, g_firstPersonUpdates.exchange(0));
    MappingCheck& m = g_mappingCheck;
    const long hA = m.horizontalOnlyA.exchange(0), hB = m.horizontalOnlyB.exchange(0);
    const long vA = m.verticalOnlyA.exchange(0), vB = m.verticalOnlyB.exchange(0);
    if (hA + hB + vA + vB > 0) {
        Log("first person: horizontal-only mouse moved +0x184 %ld times, +0x188 %ld; vertical-only moved +0x184 %ld, "
            "+0x188 %ld (expected: +0x188 horizontal, +0x184 vertical)", hA, hB, vA, vB);
    }
}

} // namespace acb
