# KCD1 Clarity encore-lite

Gives **RTX Encore**'s frame generation the real motion vectors and depth from **Kingdom Come: Deliverance** (the
first game), so Encore reports **Using Game Vectors** instead of estimating motion from the image.

A stripped-down build of [KCD1 Clarity](https://github.com/eonflux7/kcd1-clarity) 0.2.0, kept to that single job.
Unofficial mod, not affiliated with Warhorse Studios, NVIDIA or the RTX Encore project.

> **Status:** builds cleanly and passes the offline D3D11 (WARP) regression test. In-game testing is still in
> progress. Please report what you see, with `kcd_clarity.log` and `rtx-encore.log`.

## How it works

RTX Encore 2.5.0 only uses game motion vectors on DirectX 11 when it can read them from the game's DLSS call. KCD1 has
no DLSS, so this mod provides one:

1. At the game's SMAA 2TX resolve draw, it builds full-screen motion vectors from the engine's reprojection matrix and
   object velocity, plus a copy of the depth buffer.
2. It runs a DLAA evaluation with those inputs. Encore reads the vectors and depth from that call.
3. With `output=0` (the default), the DLAA image is discarded and **the game's own SMAA 2TX image is what you see**.
   With `output=1`, the DLAA image replaces it.

The DLAA pass runs either way, so it costs some GPU time even when its image isn't shown.

## Requirements

- NVIDIA RTX graphics card.
- Kingdom Come: Deliverance, Steam version **1.9.8**, DirectX 11, at **native resolution**.
- Anti-aliasing set to **SMAA 2TX** in the game's video options. The mod looks for that shader.
- [RTX Encore](https://github.com/SilyNoMeta/rtx-encore) 2.5.0 with DLSS frame generation enabled, motion estimation
  set to **Game motion vectors**, and Encore's own upscaling off.
- `nvngx_dlss.dll` in `Bin\Win64`. Encore's install usually provides it; otherwise use the one from the build.

## Install

1. Close the game.
2. Copy `dinput8.dll` into `Bin\Win64` (next to `KingdomCome.exe`).
3. Start the game and load a save. Encore's overlay should show **Using Game Vectors** at 2×.

If Encore shows estimated motion during startup, start Encore at 1× and switch to 2× after the save loads.

Any other mod that installs a `dinput8.dll` (Ultimate ASI Loader, full KCD1 Clarity) conflicts with this one. Only one
can be installed.

## Settings

`Bin\Win64\kcd_clarity.ini` is created on first start:

```ini
[general]
mode=1             ; 0 = off (stock game), 1 = on
output=0           ; 0 = game's SMAA 2TX on screen, 1 = DLAA on screen
[dlss]
preset=0           ; NGX render preset, 0 = DLSS default
velocity16=1       ; object motion vectors at 16 bits (restart to change)
velocity_cleanup=1 ; camera motion on occluders in the engine's velocity target
force_halton=1     ; Halton jitter pattern, applied only while output=1
cut_guard=1        ; zero the motion vectors on camera cuts
```

`velocity16` and `velocity_cleanup` also change the velocity data the game's own SMAA 2TX reads. Set both to 0 for a
completely stock game image. That combination hasn't been tested with Encore yet.

## Camera cuts and UI

On a camera cut (a dialogue shot change, a teleport, a map or menu transition) the game's reprojection still points at
the previous shot for one frame. Those vectors would make frame generation warp the whole image, text included. With
`cut_guard=1`, when more than half the screen moves over a quarter of its width in one frame, the vectors for that
frame are zeroed on the GPU, so Encore blends the two frames instead of warping. Each detected cut is logged in
`kcd_clarity.log`. The log also records when the game stops and resumes drawing its 3D scene (map, menus,
loading); those frames have no DLSS call, so Encore uses its own motion estimation.

Text over a moving scene can still ghost slightly. Encore's fix for that, UI recomposition, needs the scene and the
interface as separate layers, which Encore 2.5.0 only accepts on DirectX 12 or through DLSS frame-generation inputs.
In Encore, **Frame rate > Protection tuning > Classic** may also reduce artifacts that the default *Refined* leaves.

## Uninstall

Delete `dinput8.dll` from `Bin\Win64`. You can also delete `kcd_clarity.ini` and `kcd_clarity.log`.

## Building from source

Needs Visual Studio 2022 (or Build Tools) with the C++ x64 workload; CMake and Ninja come with it. Run `build.cmd`
from the repository root. The output is `build\dinput8.dll`; NVIDIA's runtime is copied to `build\ngx\rel`.
`build.cmd dist` also packages a zip. CMake downloads MinHook and the needed DLSS SDK files and checks their hashes.

Offline regression tests (software D3D11, they do not start the game), from an x64 developer prompt:

```bat
mkdir build\test
cd build\test
cl /nologo /std:c++20 /EHsc /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I..\..\src ..\..\tests\velocity_mask_warp.cpp ..\..\src\renderer\velocity_mask.cpp ..\..\src\renderer\state_backup.cpp /Fe:velocity_mask_warp.exe /link d3d11.lib d3dcompiler.lib
cl /nologo /std:c++20 /EHsc /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN ..\..\tests\cut_guard_warp.cpp /Fe:cut_guard_warp.exe /link d3d11.lib d3dcompiler.lib
.\velocity_mask_warp.exe
.\cut_guard_warp.exe ..\..\src\renderer\taa_dlss.cpp
```

`velocity_mask_warp` covers the velocity cleanup crash fix. `cut_guard_warp` compiles the motion-vector shaders and
checks that vectors are kept on normal and fast camera motion and zeroed on a cut.

## Changes from KCD1 Clarity 0.2.0

- **Crash fix:** stock 0.2.0 crashed at `dinput8.dll` RVA `0x3c200` in the optional velocity cleanup while Encore was
  generating frames. The velocity cleanup now holds its texture references for the whole pass, defers the reset that
  Present triggers to the render path, and skips cleanup when resources are missing.
- **Game AA output:** new `output=0` mode keeps the game's SMAA 2TX image on screen.
- **Camera-cut guard:** zeroes the motion vectors on frames where the camera jumps (see above).
- **Removed:**
  - The F8/F3 menu and Dear ImGui
  - The quality modes below native, with all their sub-resolution fixes
  - The debug canary, frame dumps, mip bias, shadow noise and the D3D debug-layer option
- **Fewer hooks:** the D3D11 hooks are reduced to seven: CreatePixelShader, PSSetShader, Draw, CreateTexture2D,
  CreateShaderResourceView, CreateRenderTargetView and OMSetRenderTargets.

For full DLSS with upscaling, use upstream [KCD1 Clarity](https://github.com/eonflux7/kcd1-clarity). The previous
`main` of this fork (Clarity 0.3.0) is kept under the tag `clarity-0.3.0-main`.

## Credits and license

Based on KCD1 Clarity by its contributors; engine research from their kcd-re notes, which the source comments cite. MIT
licensed: see [LICENSE](LICENSE). Third-party components keep their own licenses: see [THIRD_PARTY.md](THIRD_PARTY.md).
