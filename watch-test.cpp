// Offline test of the right-stick watchpoint (src\watch.cpp), no game needed: a fake pad
// is copied by a worker thread with the three copy forms the game uses (fld/fstp,
// movq, rep movsd) and the copies are read. The watch must record the accesses,
// follow the copies into further debug registers, end cleanly, and leave the
// process running.
#include "src/common.h"

#include <cstdio>
#include <cstring>

namespace {

__declspec(align(16)) BYTE g_pad[0x400];
__declspec(align(16)) BYTE g_copyFpu[0x400];
__declspec(align(16)) BYTE g_copyMovq[0x400];
__declspec(align(16)) BYTE g_copyString[0x400];
volatile LONG g_stop = 0;
volatile LONG g_sum = 0;
volatile LONG g_stickBits = 0;
volatile bool g_tight = false; // stress phase: the worker doesn't sleep

void WriteStick(LONG bits) {
    __asm {
        lea esi, g_pad
        mov eax, bits
        mov dword ptr [esi + 0x1a0], eax
        nop
    }
}

void CopyFpu() {
    __asm {
        lea esi, g_pad
        lea edi, g_copyFpu
        fld dword ptr [esi + 0x1a0]
        fstp dword ptr [edi + 0x1a0]
    }
}

void CopyMovq() {
    __asm {
        lea esi, g_pad
        lea edi, g_copyMovq
        movq xmm0, qword ptr [esi + 0x1a0]
        movq qword ptr [edi + 0x1a0], xmm0
    }
}

void CopyString() {
    __asm {
        push esi
        push edi
        lea esi, g_pad
        lea edi, g_copyString
        mov ecx, 0x100
        cld
        rep movsd
        pop edi
        pop esi
    }
}

LONG ReadCopies() {
    LONG a, b, c;
    __asm {
        lea esi, g_copyFpu
        mov eax, dword ptr [esi + 0x1a0]
        mov a, eax
        lea esi, g_copyMovq
        mov eax, dword ptr [esi + 0x1a0]
        mov b, eax
        lea esi, g_copyString
        mov eax, dword ptr [esi + 0x1a0]
        mov c, eax
    }
    return a ^ b ^ c;
}

DWORD WINAPI Worker(void*) {
    float value = 1.0f;
    while (!g_stop) {
        value += 1.0f; // never zero, so copies can be followed
        LONG bits;
        memcpy(&bits, &value, 4);
        InterlockedExchange(&g_stickBits, bits);
        WriteStick(bits);
        CopyFpu();
        CopyMovq();
        CopyString();
        InterlockedAdd(&g_sum, ReadCopies());
        if (!g_tight) Sleep(1);
    }
    return 0;
}

} // namespace

namespace acb {
void* PadObject() { return g_pad; }
DWORD LastRightStickBits() { return DWORD(g_stickBits); }
} // namespace acb

LONG WINAPI Unhandled(EXCEPTION_POINTERS* info) {
    const CONTEXT& c = *info->ContextRecord;
    fwprintf(stderr, L"FAIL: unhandled exception 0x%08lX at %p (Dr6 0x%08lX Dr7 0x%08lX EFlags 0x%08lX), "
             L"handler calls %ld\n", info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress,
             c.Dr6, c.Dr7, c.EFlags, acb::WatchHandlerCallsForTest());
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

int wmain(int argc, wchar_t** argv) {
    SetUnhandledExceptionFilter(&Unhandled);
    const bool noStep = argc > 1 && wcscmp(argv[1], L"--no-step") == 0;
    const bool noSwallow = argc > 1 && wcscmp(argv[1], L"--no-swallow") == 0;
    acb::WatchSetStepStoresForTest(!noStep);
    acb::WatchSetSwallowLostForTest(!noSwallow);
    acb::LogOpen(nullptr);
    acb::SetImageBaseForTest(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
    acb::g_config.watchRightStick = true;
    acb::g_config.watchSeconds = 3;

    HANDLE worker = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    Sleep(100);
    acb::WatchStartForTest();
    for (int i = 0; i < 25; ++i) {
        Sleep(100);
        if (i % 5 == 4) acb::WatchTick(true); // re-arms all threads with the followed slots
    }
    const int followed = acb::WatchFollowedSlotsForTest();
    const long hits = acb::WatchHitCountForTest();
    acb::WatchStopForTest();
    Sleep(200); // the worker keeps running with cleared debug registers

    // Stress: rewrite the debug registers of all threads as fast as possible during high-rate
    // hits. That is the race of the 2026-10-30 crash: a write can wipe DR6 between a trap and
    // the handler. The handler must survive it (--no-swallow shows the crash).
    g_tight = true;
    acb::g_config.watchSeconds = 60;
    acb::WatchStartForTest();
    const DWORD stressStart = GetTickCount();
    while (GetTickCount() - stressStart < 4000) acb::WatchStressRewriteForTest();
    acb::WatchStopForTest();
    g_tight = false;
    const long lost = acb::WatchLostStatusForTest();
    Sleep(200);
    InterlockedExchange(&g_stop, 1);
    WaitForSingleObject(worker, 5000);

    int failures = 0;
    if (hits < 10) {
        fwprintf(stderr, L"FAIL: only %ld watch hits\n", hits);
        ++failures;
    }
    if (followed < (noStep ? 1 : 3)) {
        fwprintf(stderr, L"FAIL: followed %d copies, expected 3 (fld/fstp, movq, rep movsd)\n", followed);
        ++failures;
    }
    if (failures == 0) {
        wprintf(L"ok: watch recorded %ld hits and followed %d copies; stress survived, %ld traps with DR6 lost\n",
                hits, followed, lost);
    }
    return failures == 0 ? 0 : 1;
}
