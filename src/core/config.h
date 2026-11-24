#pragma once
#include <windows.h>

#include <string>

namespace kc {

// kcd_clarity.ini next to the DLL. Missing keys get defaults; the file is written back on load
// so every setting is visible to the user.
struct Config {
    int mode = 1;              // [general] mode: 0 = off (stock game), 1 = feed motion vectors + depth through DLSS
    int output = 0;            // [general] output: 0 = keep the game's own AA image (SMAA 2TX) on screen,
                               //                   1 = show the DLAA image instead
    int dlss_preset = 0;       // [dlss] preset: NGX render preset, 0 = DLSS default, 10-13 = J-M
    bool velocity16 = true;    // [dlss] velocity16: object motion vectors at 16 bits (restart to change)
    bool velocity_cleanup = true;  // [dlss] velocity_cleanup: camera motion on occluders (velocity_mask)
    bool force_halton = true;  // [dlss] force_halton: r_AntialiasingTAAPattern 8, only while output = 1
    bool cut_guard = true;     // [dlss] cut_guard: zero the motion vectors on camera cuts
    bool map_fix = true;       // [dlss] map_fix: zero motion vectors to Encore on map/menu frames
};

Config& config();
void config_load(const std::wstring& path);
void config_save();

}  // namespace kc
