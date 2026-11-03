// Development: find who reads the pad's right stick (research\docs\PLAN.md §3.4).
//
// F11 starts a watch window of WatchSeconds: a hardware data breakpoint (DR0, read/write,
// 4 bytes) on pad+0x1a0 on every thread. A vectored handler records each instruction
// that touches it. When that instruction is directly followed by a store to the heap
// (a struct copy: fld [a] / fstp [b], movss, mov), the copy's destination is watched too
// (DR1..DR3), so the reader of a copied pad is found as well. Nothing is changed.
#include "common.h"

#include <TlHelp32.h>
#include <algorithm>
#include <cstring>

namespace acb {
namespace {

constexpr uintptr_t kMouseMapBegin = 0x00b95d30;
constexpr uintptr_t kMouseMapEnd = 0x00b960cc;

struct Slot {
    std::atomic<uintptr_t> address{0};
    int level = 0;
    uintptr_t copiedBy = 0; // preferred VA of the store that created this slot
};
Slot g_slots[4];

struct Hit {
    std::atomic<uintptr_t> eip{0}; // runtime address after the access (data breakpoints trap after)
    std::atomic<long> count{0};
    uintptr_t returns[3] = {};
    int slot = 0;
    DWORD thread = 0;
    int window = 0;
    bool reported = false;
};
constexpr int kMaxHits = 256;
Hit g_hits[kMaxHits];
std::atomic<long> g_hitOverflow{0};

std::atomic<bool> g_active{false};
std::atomic<long> g_lock{0};
PVOID g_handler = nullptr;
double g_windowEnd = 0;
int g_window = 0;
bool g_f11Down = false;

void Lock() {
    while (g_lock.exchange(1, std::memory_order_acquire) != 0) YieldProcessor();
}
void Unlock() { g_lock.store(0, std::memory_order_release); }

DWORD BuildDr7() {
    DWORD dr7 = 0;
    for (int i = 0; i < 4; ++i) {
        if (g_slots[i].address.load() == 0) continue;
        dr7 |= 1u << (2 * i);          // local enable
        dr7 |= 3u << (16 + 4 * i);     // RW = read or write
        dr7 |= 3u << (18 + 4 * i);     // LEN = 4 bytes
    }
    return dr7;
}

void FillDebugRegisters(CONTEXT& context, bool armed) {
    context.Dr0 = armed ? g_slots[0].address.load() : 0;
    context.Dr1 = armed ? g_slots[1].address.load() : 0;
    context.Dr2 = armed ? g_slots[2].address.load() : 0;
    context.Dr3 = armed ? g_slots[3].address.load() : 0;
    context.Dr7 = armed ? BuildDr7() : 0;
}

uintptr_t SafeRead(uintptr_t address) {
    __try {
        return *reinterpret_cast<const uintptr_t*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool SafeReadBytes(uintptr_t address, BYTE* out, size_t size) {
    __try {
        memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsImageCode(uintptr_t address) {
    return IsInImage(address) && address >= Resolve(0x00401000) && address < Resolve(0x00401000 + 0x01f7caa5);
}

DWORD RegisterValue(const CONTEXT& c, int index) {
    switch (index) {
    case 0: return c.Eax;
    case 1: return c.Ecx;
    case 2: return c.Edx;
    case 3: return c.Ebx;
    case 4: return c.Esp;
    case 5: return c.Ebp;
    case 6: return c.Esi;
    default: return c.Edi;
    }
}

// Memory operand of a ModRM byte (32-bit addressing, SIB included); false for a register.
bool DecodeMemoryOperand(const CONTEXT& c, const BYTE* modrmBytes, uintptr_t& address) {
    const BYTE modrm = modrmBytes[0];
    const int mod = modrm >> 6;
    const int rm = modrm & 7;
    if (mod == 3) return false;
    const BYTE* next = modrmBytes + 1;
    DWORD base = 0;
    if (rm == 4) {
        const BYTE sib = *next++;
        const int scale = 1 << (sib >> 6);
        const int index = (sib >> 3) & 7;
        const int baseRegister = sib & 7;
        if (index != 4) base += RegisterValue(c, index) * scale;
        if (baseRegister == 5 && mod == 0) {
            LONG absolute;
            memcpy(&absolute, next, 4);
            next += 4;
            base += absolute;
        } else {
            base += RegisterValue(c, baseRegister);
        }
    } else if (rm == 5 && mod == 0) {
        LONG absolute;
        memcpy(&absolute, next, 4);
        address = uintptr_t(DWORD(absolute));
        return true;
    } else {
        base = RegisterValue(c, rm);
    }
    LONG displacement = 0;
    if (mod == 1) displacement = static_cast<signed char>(*next);
    else if (mod == 2) memcpy(&displacement, next, 4);
    address = uintptr_t(base + displacement);
    return true;
}

// Destination and size of a store at eip (the instruction after the watched access), or false.
// Covers the copy forms MSVC emits: mov, fst/fstp, movss/movsd/movq/movlps, movups/movaps/movdqu/movdqa.
bool StoreAt(const CONTEXT& c, uintptr_t eip, uintptr_t& destination, int& size) {
    BYTE code[16] = {};
    if (!SafeReadBytes(eip, code, sizeof(code))) return false;
    const BYTE* p = code;
    BYTE prefix = 0;
    if (*p == 0x66 || *p == 0xF2 || *p == 0xF3) prefix = *p++;
    const BYTE* modrm = nullptr;
    if (prefix == 0 && p[0] == 0x89) {
        modrm = p + 1, size = 4;
    } else if (prefix == 0 && (p[0] == 0xD9 || p[0] == 0xDD) && (((p[1] >> 3) & 7) == 2 || ((p[1] >> 3) & 7) == 3)) {
        modrm = p + 1, size = p[0] == 0xD9 ? 4 : 8;
    } else if (p[0] == 0x0F) {
        const BYTE op = p[1];
        if (op == 0x11 && prefix == 0xF3) size = 4;                       // movss
        else if (op == 0x11 && prefix == 0xF2) size = 8;                  // movsd
        else if (op == 0x11 || op == 0x29) size = 16;                     // movups/movupd/movaps/movapd
        else if (op == 0xD6 && prefix == 0x66) size = 8;                  // movq
        else if (op == 0x13) size = 8;                                    // movlps/movlpd
        else if (op == 0x7F && (prefix == 0x66 || prefix == 0xF3)) size = 16; // movdqa/movdqu
        else return false;
        modrm = p + 2;
    } else {
        return false;
    }
    return DecodeMemoryOperand(c, modrm, destination);
}

// A store we single-step through to see where the value just read lands.
struct PendingStore {
    bool active;
    uintptr_t destination;
    int size;
    DWORD value;
    int slot;
};
thread_local PendingStore t_pending = {};
bool g_stepStores = true; // trap-flag step over a store to follow wide copies

bool OnCurrentStack(uintptr_t address) {
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    return address >= uintptr_t(tib->StackLimit) && address < uintptr_t(tib->StackBase);
}

void Follow(CONTEXT& c, uintptr_t destination, int fromSlot, uintptr_t byPreferredEip);

void Record(CONTEXT& c, int slot) {
    const uintptr_t eip = c.Eip;
    Hit* hit = nullptr;
    Lock();
    for (Hit& h : g_hits) {
        uintptr_t seen = h.eip.load(std::memory_order_relaxed);
        if (seen == eip) {
            hit = &h;
            break;
        }
        if (seen == 0) {
            h.eip.store(eip, std::memory_order_relaxed);
            h.slot = slot;
            h.thread = GetCurrentThreadId();
            h.window = g_window;
            // Return-address candidates: code addresses near the top of the stack.
            int found = 0;
            for (int i = 0; i < 64 && found < 3; ++i) {
                uintptr_t value = SafeRead(c.Esp + 4 * i);
                if (IsImageCode(value)) h.returns[found++] = value;
            }
            hit = &h;
            break;
        }
    }
    if (hit == nullptr) g_hitOverflow.fetch_add(1, std::memory_order_relaxed);
    else hit->count.fetch_add(1, std::memory_order_relaxed);
    Unlock();

    // Follow a struct copy (never stack temporaries, never the mapper, never code outside the
    // game such as our own hook). Only a non-zero value can be found again unambiguously.
    // Never read a watched address here: the debug registers are still armed while this
    // handler runs, so the read would trap again, recursively. The value comes from the
    // mouse-map hook instead, which reads the stick right after the game writes it.
    if (!IsInImage(eip)) return;
    const uintptr_t preferredEip = ToPreferred(eip);
    if (preferredEip >= kMouseMapBegin && preferredEip < kMouseMapEnd) return;
    const uintptr_t watched = g_slots[slot].address.load();
    const DWORD value = LastRightStickBits();
    if (value == 0) return;

    BYTE around[4] = {};
    SafeReadBytes(eip - 2, around, sizeof(around)); // bytes eip-2 .. eip+1
    const bool stringCopy = (around[2] == 0xA5) || (around[2] == 0xF3 && around[3] == 0xA5) || // still repeating
                            (around[1] == 0xA5);                                               // just finished
    // With fast strings the trap can come up to 64 iterations late: use the distance ESI moved
    // past the watched dword (forward copies only).
    const uintptr_t distance = uintptr_t(c.Esi) - watched;
    if (stringCopy && (c.EFlags & 0x400) == 0 && uintptr_t(c.Esi) > watched && distance <= 256 &&
        (distance & 3) == 0) {
        Follow(c, uintptr_t(c.Edi) - distance, slot, preferredEip);
        return;
    }
    uintptr_t destination = 0;
    int size = 0;
    if (g_stepStores && StoreAt(c, eip, destination, size) && !OnCurrentStack(destination)) {
        t_pending = {true, destination, size, value, slot};
        c.EFlags |= 0x100; // single-step the store, then look for the value in what it wrote
    }
}

void Follow(CONTEXT& c, uintptr_t destination, int fromSlot, uintptr_t byPreferredEip) {
    if (destination == 0 || (destination & 3) != 0 || OnCurrentStack(destination)) return;
    Lock();
    bool known = false;
    // Within 16 bytes of a watched field is the same stick vector, not a new copy.
    for (int i = 0; !known && i < 4; ++i) {
        const uintptr_t slotAddress = g_slots[i].address.load();
        known = slotAddress != 0 && destination + 16 > slotAddress && destination < slotAddress + 16;
    }
    for (int i = 1; !known && i < 4; ++i) {
        if (g_slots[i].address.load() != 0) continue;
        g_slots[i].level = g_slots[fromSlot].level + 1;
        g_slots[i].copiedBy = byPreferredEip;
        g_slots[i].address.store(destination);
        FillDebugRegisters(c, true); // this thread now; the others on the next tick
        break;
    }
    Unlock();
}

void FinishPendingStore(CONTEXT& c) {
    PendingStore pending = t_pending;
    t_pending.active = false;
    c.EFlags &= ~DWORD(0x100);
    uintptr_t match = 0;
    int matches = 0;
    for (int offset = 0; offset + 4 <= pending.size; offset += 4) {
        const uintptr_t address = pending.destination + offset;
        bool watched = false;
        for (int i = 0; !watched && i < 4; ++i) watched = g_slots[i].address.load() == address;
        if (watched) continue; // already followed, and reading it would trap again
        if (DWORD(SafeRead(address)) == pending.value) {
            match = address;
            ++matches;
        }
    }
    if (matches == 1) {
        const uintptr_t storeEip = c.Eip; // after the store; the copy is reported by the read before it
        Follow(c, match, pending.slot, IsInImage(storeEip) ? ToPreferred(storeEip) : 0);
    }
}

// A 32-bit process under WOW64 can receive its debug-register and trap-flag exceptions as
// STATUS_WX86_SINGLE_STEP instead of EXCEPTION_SINGLE_STEP (seen in watch-test).
constexpr DWORD kWx86SingleStep = 0x4000001E;

std::atomic<long> g_handlerCalls{0};
std::atomic<long> g_lostStatus{0};
bool g_swallowLost = true;
bool g_forceWrite = false; // watch-test: always rewrite the debug registers, to stress the race

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* info) {
    g_handlerCalls.fetch_add(1, std::memory_order_relaxed);
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_SINGLE_STEP && code != kWx86SingleStep) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT& c = *info->ContextRecord;
    bool handled = false;
    if (t_pending.active) {
        FinishPendingStore(c); // our trap-flag step over a store
        handled = true;
    }
    const DWORD status = c.Dr6 & 0xF;
    if (status != 0) {
        if (g_active.load(std::memory_order_relaxed)) {
            int slot = 0;
            while (slot < 4 && (status & (1u << slot)) == 0) ++slot;
            Record(c, slot);
            FillDebugRegisters(c, true);
        } else {
            FillDebugRegisters(c, false);
        }
        handled = true;
    } else if (!handled && g_swallowLost) {
        // A trap whose DR6 got lost: re-arming with SetThreadContext can overwrite the saved DR6
        // while the thread is on its way from the trap to this handler (the crash of 2026-10-30:
        // Dr6 = 0 in the dump, after thousands of good hits). This handler only exists after F11
        // and nothing else in the process uses debug registers or the trap flag, so it is ours.
        g_lostStatus.fetch_add(1, std::memory_order_relaxed);
        FillDebugRegisters(c, g_active.load(std::memory_order_relaxed));
        c.EFlags &= ~DWORD(0x100);
        handled = true;
    }
    if (!handled) return EXCEPTION_CONTINUE_SEARCH;
    c.Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void ApplyToAllThreads(bool armed) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    const DWORD process = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    THREADENTRY32 entry = {sizeof(entry)};
    int applied = 0;
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE,
                                   entry.th32ThreadID);
        if (thread == nullptr) continue;
        if (SuspendThread(thread) != DWORD(-1)) {
            CONTEXT context = {};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context)) {
                const CONTEXT before = context;
                FillDebugRegisters(context, armed);
                // Only write when something changes: every write can overwrite a DR6 in flight.
                const bool same = before.Dr0 == context.Dr0 && before.Dr1 == context.Dr1 &&
                                  before.Dr2 == context.Dr2 && before.Dr3 == context.Dr3 &&
                                  before.Dr7 == context.Dr7;
                if ((same && !g_forceWrite) || SetThreadContext(thread, &context)) ++applied;
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    static int lastApplied = -1;
    if (applied != lastApplied) {
        Log("watch: debug registers %s on %d threads", armed ? "armed" : "cleared", applied);
        lastApplied = applied;
    }
}

void FormatAddress(char* out, size_t size, uintptr_t address) {
    if (address == 0) {
        strcpy_s(out, size, "-");
    } else if (IsInImage(address)) {
        sprintf_s(out, size, "0x%08X", unsigned(ToPreferred(address)));
    } else {
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(address), &module)) {
            GetModuleFileNameA(module, name, MAX_PATH);
            const char* slash = strrchr(name, '\\');
            sprintf_s(out, size, "%s+0x%X", slash ? slash + 1 : name, unsigned(address - uintptr_t(module)));
        } else {
            sprintf_s(out, size, "%p", reinterpret_cast<void*>(address));
        }
    }
}

void ReportHits(bool all) {
    struct Row {
        uintptr_t eip;
        long count;
        Hit* hit;
    };
    Row rows[kMaxHits];
    int n = 0;
    for (Hit& h : g_hits) {
        uintptr_t eip = h.eip.load();
        if (eip == 0) break;
        if (!all && h.reported) continue;
        rows[n++] = {eip, h.count.load(), &h};
    }
    if (all) std::sort(rows, rows + n, [](const Row& a, const Row& b) { return a.count > b.count; });
    for (int i = 0; i < n; ++i) {
        char eip[64], r0[64], r1[64], r2[64];
        FormatAddress(eip, sizeof(eip), rows[i].eip);
        FormatAddress(r0, sizeof(r0), rows[i].hit->returns[0]);
        FormatAddress(r1, sizeof(r1), rows[i].hit->returns[1]);
        FormatAddress(r2, sizeof(r2), rows[i].hit->returns[2]);
        Log("watch%s: after %s (slot %d, level %d) x%ld tid %lu window %d | stack %s %s %s", all ? " summary" : "",
            eip, rows[i].hit->slot, g_slots[rows[i].hit->slot].level, rows[i].count, rows[i].hit->thread,
            rows[i].hit->window, r0, r1, r2);
        rows[i].hit->reported = true;
    }
}

void Start() {
    void* pad = PadObject();
    if (pad == nullptr) {
        Log("watch: F11 ignored, the keyboard+mouse pad hasn't been seen yet");
        return;
    }
    if (g_handler == nullptr) g_handler = AddVectoredExceptionHandler(1, &WatchHandler);
    Lock();
    for (int i = 1; i < 4; ++i) {
        g_slots[i].address.store(0);
        g_slots[i].level = 0;
        g_slots[i].copiedBy = 0;
    }
    g_slots[0].address.store(reinterpret_cast<uintptr_t>(pad) + 0x1a0);
    g_slots[0].level = 0;
    Unlock();
    ++g_window;
    g_windowEnd = SecondsSinceStart() + g_config.watchSeconds;
    g_active.store(true);
    Log("watch: window %d started for %d s on pad %p + 0x1a0 (right stick X)", g_window, g_config.watchSeconds, pad);
    ApplyToAllThreads(true);
}

void Stop() {
    g_active.store(false);
    ApplyToAllThreads(false);
    for (int i = 1; i < 4; ++i) {
        if (g_slots[i].address.load() == 0) continue;
        char by[64];
        FormatAddress(by, sizeof(by), Resolve(g_slots[i].copiedBy));
        Log("watch: slot %d followed a copy to %p (level %d) made by the instruction just before %s", i,
            reinterpret_cast<void*>(g_slots[i].address.load()), g_slots[i].level, by);
    }
    Log("watch: window %d ended (%ld hits not recorded: table full; %ld traps with DR6 lost)", g_window,
        g_hitOverflow.exchange(0), g_lostStatus.load());
    ReportHits(true);
}

} // namespace

bool WatchInstall() { return g_config.watchRightStick; }

void WatchStartForTest() { Start(); }

void WatchStopForTest() {
    if (g_active.load()) Stop();
}

int WatchFollowedSlotsForTest() {
    int followed = 0;
    for (int i = 1; i < 4; ++i) followed += g_slots[i].address.load() != 0;
    return followed;
}

long WatchHandlerCallsForTest() { return g_handlerCalls.load(); }

void WatchSetStepStoresForTest(bool enabled) { g_stepStores = enabled; }

void WatchSetSwallowLostForTest(bool enabled) { g_swallowLost = enabled; }

void WatchStressRewriteForTest() {
    g_forceWrite = true;
    ApplyToAllThreads(true);
    g_forceWrite = false;
}

long WatchLostStatusForTest() { return g_lostStatus.load(); }

long WatchHitCountForTest() {
    long total = 0;
    for (Hit& h : g_hits) total += h.count.load();
    return total;
}

void WatchTick(bool secondElapsed) {
    if (!g_config.watchRightStick) return;
    DWORD foregroundProcess = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
    const bool down = foregroundProcess == GetCurrentProcessId() && (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    const bool pressed = down && !g_f11Down;
    g_f11Down = down;
    if (g_active.load()) {
        if (!secondElapsed) return;
        ReportHits(false);
        if (SecondsSinceStart() >= g_windowEnd) Stop();
        else ApplyToAllThreads(true); // new threads, and copies followed since the last tick
    } else if (pressed) {
        Start();
    }
}

} // namespace acb
