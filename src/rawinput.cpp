// Raw Input source, Dead Space 3 method (research\docs\PLAN.md §5.2): exclusive
// DirectInput owns the process' mouse registration, so instead of registering again we
// find the window DirectInput registered and read WM_INPUT on its way through.
#include "common.h"

#include <cstdlib>

namespace acb {
namespace {

struct Totals {
    std::atomic<long> packets{0};
    std::atomic<long> absolute{0}; // MOUSE_MOVE_ABSOLUTE packets (ignored)
    std::atomic<long long> x{0};
    std::atomic<long long> y{0};
    std::atomic<long long> absX{0};
    std::atomic<long long> absY{0};
};
Totals g_raw;      // since the last report
Totals g_rawAll;   // since the window was subclassed
std::atomic<long long> g_diX{0}, g_diY{0};       // since the last report
std::atomic<long long> g_diAllX{0}, g_diAllY{0}; // since the window was subclassed

// Running totals for the camera; never reset, see mouse_model.h.
std::atomic<long> g_totalRawX{0}, g_totalRawY{0};
std::atomic<long> g_totalDiX{0}, g_totalDiY{0};
std::atomic<long long> g_lastRawTime{0};

HWND g_window = nullptr;
WNDPROC g_original = nullptr;
std::atomic<bool> g_subclassed{false};

RAWINPUTDEVICE g_lastDevices[16] = {};
UINT g_lastCount = UINT(-1);

LRESULT CALLBACK SniffWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INPUT) {
        RAWINPUT input;
        UINT size = sizeof(input);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &input, &size,
                            sizeof(RAWINPUTHEADER)) != UINT(-1) &&
            input.header.dwType == RIM_TYPEMOUSE) {
            const RAWMOUSE& m = input.data.mouse;
            if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
                g_raw.absolute.fetch_add(1, std::memory_order_relaxed);
            } else if (m.lLastX != 0 || m.lLastY != 0) {
                g_totalRawX.fetch_add(m.lLastX, std::memory_order_relaxed);
                g_totalRawY.fetch_add(m.lLastY, std::memory_order_relaxed);
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                g_lastRawTime.store(now.QuadPart, std::memory_order_relaxed);
                Totals* const totals[] = {&g_raw, &g_rawAll};
                for (Totals* t : totals) {
                    t->packets.fetch_add(1, std::memory_order_relaxed);
                    t->x.fetch_add(m.lLastX, std::memory_order_relaxed);
                    t->y.fetch_add(m.lLastY, std::memory_order_relaxed);
                    t->absX.fetch_add(std::labs(m.lLastX), std::memory_order_relaxed);
                    t->absY.fetch_add(std::labs(m.lLastY), std::memory_order_relaxed);
                }
            }
        }
    }
    WNDPROC original = g_original;
    return original != nullptr ? CallWindowProcW(original, window, message, wParam, lParam)
                               : DefWindowProcW(window, message, wParam, lParam);
}

void Subclass(HWND window) {
    DWORD process = 0;
    DWORD thread = GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId()) {
        Log("raw: mouse target window %p belongs to another process", window);
        return;
    }
    wchar_t className[128] = L"";
    GetClassNameW(window, className, 128);
    // Publish the original before our procedure can receive the window's first message.
    g_original = reinterpret_cast<WNDPROC>(GetWindowLongW(window, GWL_WNDPROC));
    SetLastError(ERROR_SUCCESS);
    auto previous = reinterpret_cast<WNDPROC>(
        SetWindowLongW(window, GWL_WNDPROC, reinterpret_cast<LONG>(&SniffWindowProc)));
    if (previous == nullptr && GetLastError() != ERROR_SUCCESS) {
        Log("raw: subclassing window %p failed (error %lu)", window, GetLastError());
        g_original = nullptr;
        return;
    }
    g_original = previous;
    g_window = window;
    g_rawAll.packets = 0;
    g_rawAll.x = 0;
    g_rawAll.y = 0;
    g_rawAll.absX = 0;
    g_rawAll.absY = 0;
    g_diAllX = 0;
    g_diAllY = 0;
    g_subclassed.store(true, std::memory_order_release);
    Log("raw: subclassed DirectInput's raw-input window %p (class \"%ls\", thread %lu, unicode %d)", window,
        className, thread, IsWindowUnicode(window));
}

} // namespace

CountPair RawInputTotals() {
    CountPair totals;
    totals.x = g_totalRawX.load(std::memory_order_relaxed);
    totals.y = g_totalRawY.load(std::memory_order_relaxed);
    return totals;
}

CountPair DirectInputTotals() {
    CountPair totals;
    totals.x = g_totalDiX.load(std::memory_order_relaxed);
    totals.y = g_totalDiY.load(std::memory_order_relaxed);
    return totals;
}

long long LastRawInputTime() { return g_lastRawTime.load(std::memory_order_relaxed); }

void AddDirectInputCounts(long x, long y) {
    g_totalDiX.fetch_add(x, std::memory_order_relaxed);
    g_totalDiY.fetch_add(y, std::memory_order_relaxed);
    g_diX.fetch_add(x, std::memory_order_relaxed);
    g_diY.fetch_add(y, std::memory_order_relaxed);
    if (g_subclassed.load(std::memory_order_relaxed)) {
        g_diAllX.fetch_add(x, std::memory_order_relaxed);
        g_diAllY.fetch_add(y, std::memory_order_relaxed);
    }
}

void PollRawInputRegistration() {
    // The fix reads its counts here (UseRawInput); RawSniff only adds it for comparisons.
    if (!g_config.rawSniff && !g_config.useRawInput) return;
    RAWINPUTDEVICE devices[16];
    UINT count = 16;
    UINT result = GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE));
    if (result == UINT(-1)) {
        Log("raw: GetRegisteredRawInputDevices failed (error %lu, need %u)", GetLastError(), count);
        return;
    }
    count = result;
    bool changed = count != g_lastCount;
    for (UINT i = 0; !changed && i < count; ++i) {
        changed = memcmp(&devices[i], &g_lastDevices[i], sizeof(RAWINPUTDEVICE)) != 0;
    }
    if (changed) {
        if (count == 0) Log("raw: no raw-input registrations in the process");
        for (UINT i = 0; i < count; ++i) {
            Log("raw: registration %u: usage %04X:%04X flags 0x%08lX target %p", i, devices[i].usUsagePage,
                devices[i].usUsage, devices[i].dwFlags, devices[i].hwndTarget);
        }
        memcpy(g_lastDevices, devices, sizeof(RAWINPUTDEVICE) * count);
        g_lastCount = count;
    }

    for (UINT i = 0; i < count; ++i) {
        if (devices[i].usUsagePage != 1 || devices[i].usUsage != 2) continue;
        HWND target = devices[i].hwndTarget;
        if (target == nullptr) {
            if (changed) Log("raw: the mouse registration follows keyboard focus (no target window)");
            continue;
        }
        // One subclass per window: re-subclassing after someone chained on top of us would loop.
        if (target != g_window && IsWindow(target)) Subclass(target);
    }
}

void ReportRawStats(double seconds) {
    const long packets = g_raw.packets.exchange(0);
    const long absolute = g_raw.absolute.exchange(0);
    const long long x = g_raw.x.exchange(0);
    const long long y = g_raw.y.exchange(0);
    const long long ax = g_raw.absX.exchange(0);
    const long long ay = g_raw.absY.exchange(0);
    const long long dx = g_diX.exchange(0);
    const long long dy = g_diY.exchange(0);
    if (packets == 0 && absolute == 0 && dx == 0 && dy == 0) return;
    if (!g_subclassed.load()) {
        Log("raw: (not sniffing) DI sum x %lld y %lld", dx, dy);
        return;
    }
    Log("raw: %.0f packets/s%s, sum x %lld y %lld (|x| %lld |y| %lld) | DI sum x %lld y %lld | since start: "
        "raw %lld/%lld DI %lld/%lld", packets / seconds, absolute ? " (+absolute)" : "", x, y, ax, ay, dx, dy,
        g_rawAll.x.load(), g_rawAll.y.load(), g_diAllX.load(), g_diAllY.load());
}

} // namespace acb
