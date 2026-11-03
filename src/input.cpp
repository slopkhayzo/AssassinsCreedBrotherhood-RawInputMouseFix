// The game's keyboard+mouse "virtual pad" mapping, 0x00b95d30 (research\docs\PLAN.md §3.3).
//
// thiscall: ecx = keyboard+mouse device (it is also the pad state), stack args: the
// DIMOUSESTATE2 just read, the keyboard state, the bindings. Callee cleans 12 bytes.
// It writes the pad's right stick at +0x1a0/+0x1a4: the arrow keys give +-1, and only
// when no arrow key is held the mouse gives clamp(lX, +-150) and -clamp(lY, +-100).
#include "common.h"

#include <MinHook.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace acb {
namespace {

constexpr uintptr_t kMouseMap = 0x00b95d30;
constexpr uintptr_t kMouseClampTail = 0x00b96033;
constexpr uintptr_t kRightStickOffset = 0x1a0;

constexpr const char* kMouseMapPrologue =
    "55 8B EC 0F 57 C0 53 8B 5D 0C 56 8B 75 10 57 8B F9 F3 0F 11 87 94 01 00 00 F3 0F 11 87 90 01 00 00";
// The clamp; ?? are relocated absolute operands, checked by value below.
constexpr const char* kMouseClampTailBytes =
    "F3 0F 2A 02 F2 0F 10 15 ?? ?? ?? ?? 0F 5A C8 66 0F 2F D1 72 08 F3 0F 10 05 ?? ?? ?? ?? "
    "0F 5A C8 66 0F 2F 0D ?? ?? ?? ?? 72 08 F3 0F 10 05 ?? ?? ?? ?? F2 0F 10 15 ?? ?? ?? ?? "
    "F3 0F 11 87 A0 01 00 00 F3 0F 2A 42 04 0F 5A C8 66 0F 2F D1 72 08 F3 0F 10 05 ?? ?? ?? ?? "
    "0F 5A C8 66 0F 2F 0D ?? ?? ?? ?? 72 08 F3 0F 10 05 ?? ?? ?? ?? 0F 5A C0 F2 0F 59 05 ?? ?? ?? ?? "
    "66 0F 5A C0 F3 0F 11 87 A4 01 00 00 5F 5E 5B 5D";

struct ConstantOperand {
    unsigned offset;
    bool isDouble;
    double value;
};
constexpr ConstantOperand kClampConstants[] = {
    {8, true, -150.0},  {25, false, -150.0}, {36, true, 150.0},  {46, false, 150.0}, {54, true, -100.0},
    {84, false, -100.0}, {95, true, 100.0},  {105, false, 100.0}, {116, true, -1.0},
};
constexpr float kClampX = 150.0f;
constexpr float kClampY = 100.0f;

struct MouseState2 {
    LONG x;
    LONG y;
    LONG z;
    BYTE buttons[8];
};

using MouseMapFn = void(__fastcall*)(void* pad, void* edx, const MouseState2* mouse, const BYTE* keyboard,
                                     void* bindings);
MouseMapFn g_mouseMapOriginal = nullptr;

std::atomic<void*> g_pad{nullptr};
std::atomic<DWORD> g_mapThread{0};
std::atomic<DWORD> g_lastStickBits{0};

struct Counters {
    std::atomic<long> calls{0};
    std::atomic<long> moving{0};       // calls with lX or lY != 0
    std::atomic<long> keysOverride{0}; // mouse moved but the arrow keys own the stick
    std::atomic<long long> absX{0};
    std::atomic<long long> absY{0};
    std::atomic<long> clampHitsX{0};
    std::atomic<long> clampHitsY{0};
    std::atomic<long long> lostX{0}; // counts above the clamp
    std::atomic<long long> lostY{0};
    std::atomic<long> maxX{0};
    std::atomic<long> maxY{0};
};
Counters g_counters;

void StoreMax(std::atomic<long>& slot, long value) {
    long seen = slot.load(std::memory_order_relaxed);
    while (value > seen && !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}

float Clamp(float value, float limit) {
    if (value < -limit) return -limit;
    if (value > limit) return limit;
    return value;
}

void __fastcall MouseMapHook(void* pad, void* edx, const MouseState2* mouse, const BYTE* keyboard,
                             void* bindings) {
    g_mouseMapOriginal(pad, edx, mouse, keyboard, bindings);

    if (g_pad.load(std::memory_order_relaxed) != pad) {
        g_pad.store(pad, std::memory_order_release);
        g_mapThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    }

    float* stick = reinterpret_cast<float*>(static_cast<BYTE*>(pad) + kRightStickOffset);
    DWORD stickBits;
    memcpy(&stickBits, &stick[0], sizeof(stickBits));
    g_lastStickBits.store(stickBits, std::memory_order_relaxed);

    Counters& c = g_counters;
    c.calls.fetch_add(1, std::memory_order_relaxed);
    if (mouse == nullptr) return;
    const long x = mouse->x;
    const long y = mouse->y;
    if (x == 0 && y == 0) return;

    AddDirectInputCounts(x, y);
    c.moving.fetch_add(1, std::memory_order_relaxed);
    const long ax = std::labs(x);
    const long ay = std::labs(y);
    c.absX.fetch_add(ax, std::memory_order_relaxed);
    c.absY.fetch_add(ay, std::memory_order_relaxed);
    StoreMax(c.maxX, ax);
    StoreMax(c.maxY, ay);

    const float clampedX = Clamp(float(x), kClampX);
    const float clampedY = -Clamp(float(y), kClampY);
    // The mouse block ran iff the stick holds exactly the clamped mouse values. Arrow
    // keys only produce small integers, which can't be confused with a clamped value.
    if (stick[0] != clampedX || stick[1] != clampedY) {
        c.keysOverride.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (ax > long(kClampX)) {
        c.clampHitsX.fetch_add(1, std::memory_order_relaxed);
        c.lostX.fetch_add(ax - long(kClampX), std::memory_order_relaxed);
    }
    if (ay > long(kClampY)) {
        c.clampHitsY.fetch_add(1, std::memory_order_relaxed);
        c.lostY.fetch_add(ay - long(kClampY), std::memory_order_relaxed);
    }
    if (g_config.removeClamp && LinearCameraActive()) {
        stick[0] = float(x);
        stick[1] = -float(y);
    }
}

bool CheckClampConstants() {
    const BYTE* tail = reinterpret_cast<const BYTE*>(Resolve(kMouseClampTail));
    for (const ConstantOperand& operand : kClampConstants) {
        uintptr_t address = *reinterpret_cast<const uintptr_t*>(tail + operand.offset);
        if (!IsInImage(address)) {
            Log("signature MouseClampTail: operand +%u points outside the image; hook skipped", operand.offset);
            return false;
        }
        double value = operand.isDouble ? *reinterpret_cast<const double*>(address)
                                        : double(*reinterpret_cast<const float*>(address));
        if (value != operand.value) {
            Log("signature MouseClampTail: operand +%u is %g, expected %g; hook skipped", operand.offset, value,
                operand.value);
            return false;
        }
    }
    Log("signature MouseClampTail: clamp constants ok (X +-150, Y +-100, Y negated)");
    return true;
}

} // namespace

void* PadObject() { return g_pad.load(std::memory_order_acquire); }

DWORD LastRightStickBits() { return g_lastStickBits.load(std::memory_order_relaxed); }

bool InstallInputHooks() {
    if (!CheckSignature("MouseMap", kMouseMap, kMouseMapPrologue) ||
        !CheckSignature("MouseClampTail", kMouseClampTail, kMouseClampTailBytes) || !CheckClampConstants()) {
        return false;
    }
    void* target = reinterpret_cast<void*>(Resolve(kMouseMap));
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&MouseMapHook),
                                     reinterpret_cast<void**>(&g_mouseMapOriginal));
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK) {
        Log("hook MouseMap: MinHook error %d", int(status));
        return false;
    }
    Log("hook MouseMap: installed");
    return true;
}

void ReportInputStats(double seconds) {
    Counters& c = g_counters;
    const long calls = c.calls.exchange(0);
    const long moving = c.moving.exchange(0);
    const long keys = c.keysOverride.exchange(0);
    const long long ax = c.absX.exchange(0);
    const long long ay = c.absY.exchange(0);
    const long hx = c.clampHitsX.exchange(0);
    const long hy = c.clampHitsY.exchange(0);
    const long long lx = c.lostX.exchange(0);
    const long long ly = c.lostY.exchange(0);
    const long mx = c.maxX.exchange(0);
    const long my = c.maxY.exchange(0);
    if (moving == 0) return;
    Log("mouse map: %.0f calls/s, moving %ld, keys-override %ld | DI |x| %lld |y| %lld, max %ld/%ld | "
        "clamp hits x %ld y %ld, lost x %lld (%.1f%%) y %lld (%.1f%%)%s | pad %p tid %lu",
        calls / seconds, moving, keys, ax, ay, mx, my, hx, hy, lx, ax ? 100.0 * lx / ax : 0.0, ly,
        ay ? 100.0 * ly / ay : 0.0, g_config.removeClamp && LinearCameraActive() ? " [restored]" : "", PadObject(),
        g_mapThread.load());
}

} // namespace acb
