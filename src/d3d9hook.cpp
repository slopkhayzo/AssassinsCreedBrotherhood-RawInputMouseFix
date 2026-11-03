// Frame statistics and the development frame limiter: exe import of
// d3d9!Direct3DCreate9 -> IDirect3D9::CreateDevice -> IDirect3DDevice9::Present.
#include "common.h"

#include <MinHook.h>
#include <cstring>
#include <d3d9.h>

namespace acb {
namespace {

using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND,
                                              const RGNDATA*);
using SwapChainPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND,
                                                       const RGNDATA*, DWORD);

Direct3DCreate9Fn g_create9 = nullptr;
CreateDeviceFn g_createDevice = nullptr;
PresentFn g_present = nullptr;
SwapChainPresentFn g_swapChainPresent = nullptr;
using EndSceneFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
EndSceneFn g_endScene = nullptr;
std::atomic<long> g_endScenes{0};
const BYTE* g_presentTarget = nullptr;
const BYTE* g_swapChainTarget = nullptr;
int g_silentReports = 0;
std::atomic<bool> g_createDeviceHooked{false};
std::atomic<bool> g_presentHooked{false};

LARGE_INTEGER g_frequency = {};
LONGLONG g_lastPresent = 0;
LONGLONG g_nextDeadline = 0;
HANDLE g_timer = nullptr;

// Frame-time histogram in 0.1 ms buckets up to 100 ms; written by the render thread only.
constexpr int kBuckets = 1000;
std::atomic<long> g_histogram[kBuckets + 1];
std::atomic<long> g_frames{0};
std::atomic<long> g_maxFrameUs{0};

void LimitFrame() {
    const int limit = g_config.frameLimit;
    if (limit <= 0) return;
    const LONGLONG period = g_frequency.QuadPart / limit;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_nextDeadline == 0 || now.QuadPart - g_nextDeadline > period) g_nextDeadline = now.QuadPart;
    const LONGLONG remaining = g_nextDeadline - now.QuadPart;
    const LONGLONG spinMargin = g_frequency.QuadPart / 2000; // 0.5 ms
    if (remaining > spinMargin && g_timer != nullptr) {
        LARGE_INTEGER due;
        due.QuadPart = -((remaining - spinMargin) * 10000000 / g_frequency.QuadPart);
        if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(g_timer, INFINITE);
    }
    do {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < g_nextDeadline);
    g_nextDeadline += period;
}

void CountFrame() {
    LimitFrame();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_lastPresent != 0) {
        const LONGLONG us = (now.QuadPart - g_lastPresent) * 1000000 / g_frequency.QuadPart;
        const int bucket = us / 100 < kBuckets ? int(us / 100) : kBuckets;
        g_histogram[bucket].fetch_add(1, std::memory_order_relaxed);
        g_frames.fetch_add(1, std::memory_order_relaxed);
        if (long(us) > g_maxFrameUs.load(std::memory_order_relaxed)) g_maxFrameUs.store(long(us));
    }
    g_lastPresent = now.QuadPart;
}

HRESULT STDMETHODCALLTYPE PresentHook(IDirect3DDevice9* device, const RECT* source, const RECT* dest,
                                      HWND window, const RGNDATA* dirty) {
    CountFrame();
    return g_present(device, source, dest, window, dirty);
}

// The game presents through its swap chain (the device's Present never ran in the first session).
HRESULT STDMETHODCALLTYPE SwapChainPresentHook(IDirect3DSwapChain9* chain, const RECT* source, const RECT* dest,
                                               HWND window, const RGNDATA* dirty, DWORD flags) {
    CountFrame();
    return g_swapChainPresent(chain, source, dest, window, dirty, flags);
}

// Diagnostics: the first session counted no Present at all. EndScene shows whether the device renders.
HRESULT STDMETHODCALLTYPE EndSceneHook(IDirect3DDevice9* device) {
    g_endScenes.fetch_add(1, std::memory_order_relaxed);
    return g_endScene(device);
}

void LogOwner(const char* what, const void* address) {
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module)) {
        GetModuleFileNameA(module, name, MAX_PATH);
    }
    const char* slash = strrchr(name, '\\');
    Log("d3d9: %s at %p in %s, first byte %02X", what, address, slash ? slash + 1 : name,
        *static_cast<const BYTE*>(address));
}

HRESULT STDMETHODCALLTYPE CreateDeviceHook(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND window,
                                           DWORD flags, D3DPRESENT_PARAMETERS* parameters,
                                           IDirect3DDevice9** device) {
    HRESULT result = g_createDevice(d3d, adapter, type, window, flags, parameters, device);
    if (SUCCEEDED(result) && device != nullptr && *device != nullptr) {
        if (parameters != nullptr) {
            Log("d3d9: device created %ux%u windowed %d refresh %u interval 0x%X", parameters->BackBufferWidth,
                parameters->BackBufferHeight, parameters->Windowed, parameters->FullScreen_RefreshRateInHz,
                parameters->PresentationInterval);
        }
        if (!g_presentHooked.exchange(true)) {
            void** vtable = *reinterpret_cast<void***>(*device);
            LogOwner("device Present", vtable[17]);
            g_presentTarget = static_cast<const BYTE*>(vtable[17]);
            MH_STATUS status = MH_CreateHook(vtable[17], reinterpret_cast<void*>(&PresentHook),
                                             reinterpret_cast<void**>(&g_present));
            if (status == MH_OK) status = MH_EnableHook(vtable[17]);
            Log("d3d9: Present hook %s", status == MH_OK ? "installed" : "failed");

            IDirect3DSwapChain9* chain = nullptr;
            if (SUCCEEDED((*device)->GetSwapChain(0, &chain)) && chain != nullptr) {
                void** chainVtable = *reinterpret_cast<void***>(chain);
                LogOwner("swap chain Present", chainVtable[3]);
                g_swapChainTarget = static_cast<const BYTE*>(chainVtable[3]);
                status = MH_CreateHook(chainVtable[3], reinterpret_cast<void*>(&SwapChainPresentHook),
                                       reinterpret_cast<void**>(&g_swapChainPresent));
                if (status == MH_OK) status = MH_EnableHook(chainVtable[3]);
                Log("d3d9: swap chain Present hook %s", status == MH_OK ? "installed" : "failed");
                chain->Release();
            }
            status = MH_CreateHook(vtable[42], reinterpret_cast<void*>(&EndSceneHook),
                                   reinterpret_cast<void**>(&g_endScene));
            if (status == MH_OK) status = MH_EnableHook(vtable[42]);
            Log("d3d9: EndScene hook %s", status == MH_OK ? "installed" : "failed");
        }
    }
    return result;
}

IDirect3D9* WINAPI Direct3DCreate9Hook(UINT version) {
    IDirect3D9* d3d = g_create9(version);
    if (d3d != nullptr && !g_createDeviceHooked.exchange(true)) {
        void** vtable = *reinterpret_cast<void***>(d3d);
        MH_STATUS status = MH_CreateHook(vtable[16], reinterpret_cast<void*>(&CreateDeviceHook),
                                         reinterpret_cast<void**>(&g_createDevice));
        if (status == MH_OK) status = MH_EnableHook(vtable[16]);
        Log("d3d9: CreateDevice hook %s", status == MH_OK ? "installed" : "failed");
    }
    return d3d;
}

} // namespace

bool InstallD3D9Hooks() {
    if (!g_config.frameStats && g_config.frameLimit == 0) return false;
    QueryPerformanceFrequency(&g_frequency);
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (g_timer == nullptr) g_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    g_create9 = reinterpret_cast<Direct3DCreate9Fn>(
        PatchImport("d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&Direct3DCreate9Hook)));
    Log("d3d9: Direct3DCreate9 import %s%s", g_create9 ? "hooked" : "not found",
        g_config.frameLimit ? " (frame limiter on)" : "");
    return g_create9 != nullptr;
}

void ReportFrameStats(double seconds) {
    const long frames = g_frames.exchange(0);
    const long endScenes = g_endScenes.exchange(0);
    if (frames == 0) {
        // Say why, a few times: hooks overwritten by another module (first byte no longer E9), or
        // the game presents some other way.
        if (endScenes > 0 && g_silentReports < 5) {
            ++g_silentReports;
            Log("frames: no Present seen; %.0f EndScene/s; hook bytes: device Present %02X, swap chain %02X "
                "(E9 = still hooked)", endScenes / seconds, g_presentTarget ? *g_presentTarget : 0,
                g_swapChainTarget ? *g_swapChainTarget : 0);
        }
        return;
    }
    long counts[kBuckets + 1];
    for (int i = 0; i <= kBuckets; ++i) counts[i] = g_histogram[i].exchange(0);
    const long maxUs = g_maxFrameUs.exchange(0);
    auto percentile = [&](double p) {
        const long wanted = long(frames * p);
        long seen = 0;
        for (int i = 0; i <= kBuckets; ++i) {
            seen += counts[i];
            if (seen > wanted) return (i + 0.5) / 10.0;
        }
        return kBuckets / 10.0;
    };
    Log("frames: %.1f fps, frame time p50 %.1f ms p99 %.1f ms max %.1f ms%s", frames / seconds, percentile(0.5),
        percentile(0.99), maxUs / 1000.0, g_config.frameLimit ? " [limited]" : "");
}

} // namespace acb
