// Loads the ASI into a host that is not the game and checks that it refuses to hook.
//   asi-load-test.exe <plugin.asi>                 host name check (this exe isn't ACBSP.exe)
//   ACBSP.exe <plugin.asi> --layout-host            a copy named ACBSP.exe: layout check must fail
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

std::string ReadLog(const wchar_t* pluginPath) {
    std::wstring path(pluginPath);
    const size_t slash = path.find_last_of(L"\\/");
    path = (slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash)) + L"\\acb-raw-mouse-fix.log";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::string text;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) text.append(buffer, read);
    CloseHandle(file);
    return text;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: %s <plugin.asi> [--layout-host]\n", argv[0]);
        return 2;
    }
    const bool layoutHost = argc > 2 && wcscmp(argv[2], L"--layout-host") == 0;
    wchar_t full[MAX_PATH];
    if (GetFullPathNameW(argv[1], MAX_PATH, full, nullptr) == 0) return 2;

    HMODULE plugin = LoadLibraryW(full);
    if (plugin == nullptr) {
        fwprintf(stderr, L"LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    int failures = 0;
    if (GetProcAddress(plugin, "DirectInput8Create") != nullptr) {
        fwprintf(stderr, L"FAIL: the plugin exports DirectInput8Create\n");
        ++failures;
    }
    const std::string log = ReadLog(full);
    const char* expected = layoutHost ? "does not match the researched layout" : "is not ACBSP.exe; no hooks installed";
    if (log.find(expected) == std::string::npos) {
        fwprintf(stderr, L"FAIL: the log does not say \"%hs\"\n%hs\n", expected, log.c_str());
        ++failures;
    }
    if (log.find("hook MouseMap: installed") != std::string::npos) {
        fwprintf(stderr, L"FAIL: a hook was installed into an unsupported host\n");
        ++failures;
    }
    FreeLibrary(plugin);
    if (failures == 0) wprintf(L"ok: %s rejected %s\n", layoutHost ? L"layout host" : L"unsupported host", full);
    return failures == 0 ? 0 : 1;
}
