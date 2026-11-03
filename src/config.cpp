// acb-raw-mouse-fix.ini beside the ASI. Missing keys keep their defaults.
#include "common.h"

#include <cstdio>
#include <cstdlib>

namespace acb {

Config g_config;

namespace {

wchar_t g_iniPath[MAX_PATH] = L"";

bool ReadBool(const wchar_t* section, const wchar_t* key, bool fallback) {
    return GetPrivateProfileIntW(section, key, fallback ? 1 : 0, g_iniPath) != 0;
}

int ReadInt(const wchar_t* section, const wchar_t* key, int fallback, int low, int high) {
    int value = int(GetPrivateProfileIntW(section, key, fallback, g_iniPath));
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

float ReadFloat(const wchar_t* section, const wchar_t* key, float fallback, float low, float high) {
    wchar_t text[64] = L"";
    GetPrivateProfileStringW(section, key, L"", text, 64, g_iniPath);
    if (text[0] == L'\0') return fallback;
    const float value = float(_wtof(text));
    if (!(value >= low)) return low;
    if (value > high) return high;
    return value;
}

} // namespace

bool SaveSensitivity(float sensitivity) {
    wchar_t text[64];
    if (swprintf_s(text, L"%.3f", double(sensitivity)) < 0) return false;
    return WritePrivateProfileStringW(L"Mouse", L"Sensitivity", text, g_iniPath) != FALSE;
}

void LoadConfig() {
    if (swprintf_s(g_iniPath, L"%s\\acb-raw-mouse-fix.ini", PluginDirectory()) < 0) return;
    if (GetFileAttributesW(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        Log("config: %ls not found, using defaults", g_iniPath);
    }

    Config& c = g_config;
    c.linearCamera = ReadBool(L"Mouse", L"LinearCamera", c.linearCamera);
    c.mouseScale = ReadFloat(L"Mouse", L"MouseScale", c.mouseScale, 0.1f, 1000.0f);
    c.sensitivity = ReadFloat(L"Mouse", L"Sensitivity", c.sensitivity, 0.01f, 100.0f);
    c.sensitivityX = ReadFloat(L"Mouse", L"SensitivityX", c.sensitivityX, 0.01f, 100.0f);
    c.sensitivityY = ReadFloat(L"Mouse", L"SensitivityY", c.sensitivityY, 0.01f, 100.0f);
    c.useRawInput = ReadBool(L"Mouse", L"UseRawInput", c.useRawInput);
    c.removeClamp = ReadBool(L"Mouse", L"RemoveClamp", c.removeClamp);
    c.statsLog = ReadBool(L"Diagnostics", L"StatsLog", c.statsLog);
    c.rawSniff = ReadBool(L"Diagnostics", L"RawSniff", c.rawSniff);
    c.frameStats = ReadBool(L"Diagnostics", L"FrameStats", c.frameStats);
    c.frameLimit = ReadInt(L"Diagnostics", L"FrameLimit", c.frameLimit, 0, 1000);
    c.watchRightStick = ReadBool(L"Diagnostics", L"WatchRightStick", c.watchRightStick);
    c.watchSeconds = ReadInt(L"Diagnostics", L"WatchSeconds", c.watchSeconds, 1, 600);

    Log("config: LinearCamera=%d MouseScale=%.2f Sensitivity=%.3f SensitivityX=%.3f SensitivityY=%.3f UseRawInput=%d RemoveClamp=%d StatsLog=%d RawSniff=%d FrameStats=%d FrameLimit=%d "
        "WatchRightStick=%d WatchSeconds=%d",
        c.linearCamera, c.mouseScale, c.sensitivity, c.sensitivityX, c.sensitivityY, c.useRawInput, c.removeClamp, c.statsLog, c.rawSniff, c.frameStats, c.frameLimit,
        c.watchRightStick, c.watchSeconds);
}

} // namespace acb
