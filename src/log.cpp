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
    // A launch can start several ACBSP.exe in a row (Steam, SteamStub, Ubisoft Connect hand-offs):
    // append, so the last one doesn't erase the others, and start over for a new launch (the log
    // untouched for 10 minutes) or past 4 MB.
    bool fresh = true;
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &attributes)) {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER written, current;
        written.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
        written.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
        current.LowPart = now.dwLowDateTime;
        current.HighPart = now.dwHighDateTime;
        const ULONGLONG age = current.QuadPart > written.QuadPart ? current.QuadPart - written.QuadPart : 0;
        fresh = age > 10ull * 60 * 10000000 || attributes.nFileSizeHigh != 0 || attributes.nFileSizeLow > 4u << 20;
    }
    g_logFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            fresh ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    char header[160];
    SYSTEMTIME local;
    GetLocalTime(&local);
    const int length2 = _snprintf_s(header, _TRUNCATE, "==== process %lu, %04u-%02u-%02u %02u:%02u:%02u ====\r\n",
                                    GetCurrentProcessId(), local.wYear, local.wMonth, local.wDay, local.wHour,
                                    local.wMinute, local.wSecond);
    DWORD written2 = 0;
    if (length2 > 0) WriteFile(g_logFile, header, DWORD(length2), &written2, nullptr);
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
