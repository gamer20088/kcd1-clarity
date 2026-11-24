#include "core/config.h"

#include <string>

#include "core/log.h"

namespace kc {

static Config g_config;
static std::wstring g_path;

Config& config() { return g_config; }

void config_load(const std::wstring& path) {
    g_path = path;
    const wchar_t* f = g_path.c_str();
    g_config.mode = GetPrivateProfileIntW(L"general", L"mode", g_config.mode, f) ? 1 : 0;
    g_config.output = GetPrivateProfileIntW(L"general", L"output", g_config.output, f) ? 1 : 0;
    g_config.dlss_preset = GetPrivateProfileIntW(L"dlss", L"preset", g_config.dlss_preset, f);
    g_config.velocity16 = GetPrivateProfileIntW(L"dlss", L"velocity16", g_config.velocity16, f) != 0;
    g_config.velocity_cleanup = GetPrivateProfileIntW(L"dlss", L"velocity_cleanup", g_config.velocity_cleanup, f) != 0;
    g_config.force_halton = GetPrivateProfileIntW(L"dlss", L"force_halton", g_config.force_halton, f) != 0;
    g_config.cut_guard = GetPrivateProfileIntW(L"dlss", L"cut_guard", g_config.cut_guard, f) != 0;
    g_config.map_fix = GetPrivateProfileIntW(L"dlss", L"map_fix", g_config.map_fix, f) != 0;
    logf("config: %ls: mode %d, output %s, preset %d, velocity16 %d, velocity_cleanup %d, force_halton %d, cut_guard %d, map_fix %d", f,
         g_config.mode, g_config.output ? "dlaa" : "game", g_config.dlss_preset, g_config.velocity16,
         g_config.velocity_cleanup, g_config.force_halton, g_config.cut_guard, g_config.map_fix);
    config_save();
}

void config_save() {
    if (g_path.empty())
        return;
    const wchar_t* f = g_path.c_str();
    WritePrivateProfileStringW(L"general", L"mode", std::to_wstring(g_config.mode).c_str(), f);
    WritePrivateProfileStringW(L"general", L"output", std::to_wstring(g_config.output).c_str(), f);
    WritePrivateProfileStringW(L"dlss", L"preset", std::to_wstring(g_config.dlss_preset).c_str(), f);
    WritePrivateProfileStringW(L"dlss", L"velocity16", g_config.velocity16 ? L"1" : L"0", f);
    WritePrivateProfileStringW(L"dlss", L"velocity_cleanup", g_config.velocity_cleanup ? L"1" : L"0", f);
    WritePrivateProfileStringW(L"dlss", L"force_halton", g_config.force_halton ? L"1" : L"0", f);
    WritePrivateProfileStringW(L"dlss", L"cut_guard", g_config.cut_guard ? L"1" : L"0", f);
    WritePrivateProfileStringW(L"dlss", L"map_fix", g_config.map_fix ? L"1" : L"0", f);
}

}  // namespace kc
