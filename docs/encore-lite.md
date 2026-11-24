# KCD1 Clarity encore-lite

A stripped build of the `encore-compatibility` branch whose only job is to give RTX Encore the game's motion vectors
and depth, so Encore's frame generation reports **Using Game Vectors**. Native resolution only.

## How it works

At KCD1's SMAA 2TX resolve draw, Clarity builds full-screen motion vectors and depth (same shader as 0.2.0) and runs a
DLAA evaluation with them. Encore reads the vectors and depth from that NGX call. Encore has no other DX11 input route,
so the DLAA evaluation still has to run.

- `output=0` (default): the DLAA result is thrown away and the game's own SMAA 2TX draw runs as normal. What you see
  is the game's anti-aliasing. DLAA still costs GPU time.
- `output=1`: the DLAA result replaces the game's TAA output (the 0.2.0 behaviour at quality 0).

## Removed compared with 0.2.0

The ImGui menu and input (F8/F3), the DLSS quality modes below native and all sub-resolution fixes (screen_size_fix,
edge-clamp canary, DrawFullScreenTri/sun-shaft hooks, mip bias, shadow noise), frame dumps, and the D3D debug layer
option. The D3D11 hooks are reduced to CreatePixelShader, PSSetShader, Draw, CreateTexture2D, CreateShaderResourceView,
CreateRenderTargetView and OMSetRenderTargets.

Kept: the TAA hook and motion/depth preparation, DLAA through NGX, the velocity cleanup with the fix from 415fd14,
16-bit object velocity, the crash logger, and the dinput8 proxy.

## kcd_clarity.ini

```ini
[general]
mode=1            ; 0 = off (stock game), 1 = on
output=0          ; 0 = game's SMAA 2TX on screen, 1 = DLAA on screen
[dlss]
preset=0
velocity16=1      ; object motion vectors at 16 bits (restart to change)
velocity_cleanup=1 ; camera motion on occluders in the engine's velocity target
force_halton=1    ; Halton jitter pattern, applied only while output=1
```

Keys left over from 0.2.0 are ignored. Keep KCD1's anti-aliasing on **SMAA 2TX**, because the hook looks for that
shader.
