// Log file beside the ASI: acb-raw-mouse-fix.log (truncated at each launch).
#include "common.h"

#include <cstdarg>
#include <cstdio>

namespace acb {
namespace {

CRITICAL_SECTION g_logLock;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
wchar_t g_directory[MAX_PATH] = L"";
LARGE_INTEGER g_start = {};
LARGE_INTEGER g_frequency = {};

} // namespace

const wchar_t* PluginDirectory() { return g_directory; }

double SecondsSinceStart() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - g_start.QuadPart) / double(g_frequency.QuadPart);
}

void LogOpen(HMODULE self) {
    InitializeCriticalSection(&g_logLock);
    QueryPerformanceFrequency(&g_frequency);
    QueryPerformanceCounter(&g_start);

    DWORD length = GetModuleFileNameW(self, g_directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        g_directory[0] = L'\0';
        return;
    }
    wchar_t* slash = wcsrchr(g_directory, L'\\');
    if (slash != nullptr) *slash = L'\0';

    wchar_t path[MAX_PATH];
    if (swprintf_s(path, L"%s\\acb-raw-mouse-fix.log", g_directory) < 0) return;
    g_logFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
}

void Log(const char* format, ...) {
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    char line[1024];
    int prefix = _snprintf_s(line, _TRUNCATE, "[%9.3f] ", SecondsSinceStart());
    if (prefix < 0) prefix = 0;
    va_list args;
    va_start(args, format);
    int body = _vsnprintf_s(line + prefix, sizeof(line) - prefix - 2, _TRUNCATE, format, args);
    va_end(args);
    size_t used = body < 0 ? sizeof(line) - 3 : size_t(prefix + body);
    line[used++] = '\r';
    line[used++] = '\n';

    EnterCriticalSection(&g_logLock);
    DWORD written = 0;
    WriteFile(g_logFile, line, DWORD(used), &written, nullptr);
    LeaveCriticalSection(&g_logLock);
}

} // namespace acb
