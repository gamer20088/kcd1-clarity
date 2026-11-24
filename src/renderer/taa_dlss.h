#pragma once
#include <d3d11.h>

#include <cstdint>

#include "hooks/d3d11_hooks.h"

// Runs at the engine's TAA resolve draw (PostAA_PS, SMAA 2TX), native resolution only.
//
// At that draw the engine has bound everything DLSS needs (kcd-re db/notes/10-taa.md, 30-frame.md):
//   _tex4 current frame (after SMAA), PostAA_DepthTex device depth, _tex3 object velocity,
//   mReprojection (current uv+depth -> previous clip) in the PS constant buffer, RTV = TAA output.
// We build full-screen motion vectors exactly like PostAA_PS does (camera reprojection where no object velocity)
// and evaluate DLAA with them. RTX Encore reads the motion vectors and depth from that NGX call for its frame
// generation ("Using Game Vectors").
//
// Output::Game (default): the DLAA result is discarded and the engine's own TAA draw runs as usual, so the image on
//                         screen is the game's SMAA 2TX.
// Output::Dlaa:           the DLAA result replaces the engine's TAA output.
namespace kc::taa_dlss {

enum class Mode { Off = 0, On = 1 };
enum class Output { Game = 0, Dlaa = 1 };

struct Settings {
    Mode mode = Mode::On;
    Output output = Output::Game;
    bool force_halton = true;   // r_AntialiasingTAAPattern 8 (Halton 2,3) while output = Dlaa
    bool cut_guard = true;      // zero the motion vectors on camera cuts (most of the screen jumps at once)
    bool map_fix = true;        // zero motion vectors to Encore on frames without a 3D scene (map, menus)
    bool jitter_flip_x = true;  // confirmed in game 2026-10-15: DLSS jitter = (-x, +y) of gRenDev+0xF20
    bool jitter_flip_y = false;
    bool mv_flip_x = false;
    bool mv_flip_y = false;
    int depth_mode = 0;         // 0 = from r_ReverseDepth, 1 = normal, 2 = inverted
};

struct Status {
    bool shaders_ready = false;
    bool resources_ready = false;
    bool last_ok = false;
    const char* last_error = "";
    UINT width = 0, height = 0;
    float jitter_x = 0, jitter_y = 0;  // pixels, as passed to DLSS
    int reverse_depth = -1;            // CV_r_ReverseDepth
    uint64_t frames = 0;
};

Settings& settings();
const Status& status();
void request_reset();

// Render thread, from the Draw hook while the TAA shader is bound. Returns true if the engine's draw must be
// skipped (our DLAA image was written instead).
bool on_taa_draw(ID3D11DeviceContext* ctx, const d3d11::TaaShader& taa);
// Render thread, every Present: keeps the engine on the jitter pattern DLAA output needs.
void on_present();
// Render thread, at the start of the engine's CD3D9Renderer::RT_EndFrame: the map fix.
void on_end_frame();

}  // namespace kc::taa_dlss
