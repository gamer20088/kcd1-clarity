# KCD1 Clarity with RTX Encore

This branch is based on [KCD1 Clarity 0.2.0](https://github.com/eonflux7/kcd1-clarity/tree/v0.2.0). Its C++ changes fix a crash in Clarity's optional velocity cleanup when used alongside [RTX Encore 2.5.0](https://github.com/SilyNoMeta/rtx-encore/releases/tag/v2.5.0). The fix is in Clarity. Encore is a separately installed dependency.

## How the two mods work together

- **Clarity** hooks KCD1's Direct3D 11 temporal anti-aliasing pass. It builds full-screen motion vectors from the game's camera and object data and supplies them, along with depth, to DLSS. At native resolution it runs DLAA.
- **Encore** supplies DLSS Frame Generation. Keep Encore's own display-image upscaler disabled when Clarity owns DLSS. With **Game Motion Vectors** selected, Encore uses compatible data when it captures it and can fall back to motion estimation. Seeing **Using Game Vectors** in gameplay is the check for the active input source. Estimated motion during loading was observed on the test system.

This branch adds no new injection path or private Encore API. The successful test used Clarity's `dinput8.dll` and Encore's `version.dll` beside `KingdomCome.exe`.

## Setup used for the test

Tested on the Steam build recognized by Clarity as 1.9.8, on DirectX 11, with an RTX 3080 Ti and 2560×1440 fullscreen.

1. Build the source branch using the [upstream build instructions](../README.md#building-from-source). With the game closed, copy the resulting `build\dinput8.dll` to `Bin\Win64` in place of the existing Clarity loader. Install Clarity's and Encore's runtime dependencies through their original distributions. If both mods are already installed, keep the existing runtime DLLs.
2. Set KCD1's anti-aliasing to **SMAA 2TX**, its output to **2560×1440**, and its display mode to **Fullscreen**. In Clarity, choose **DLAA**. In `kcd_clarity.ini`, that is `[general] mode=1` and `[dlss] quality=0`.
3. Merge these keys into the corresponding objects in the existing `rtx-encore.jsonc`. Preserve the other entries. The 1× setting arms the engine while pausing generated frames during startup.

   ```json
   {
     "renderingEngines": { "frameGenerationEngine": "dlss-fg" },
     "frameGeneration": {
       "mode": "fixed",
       "multiplier": 1,
       "frameGenerationWithoutGameFg": true
     },
     "dlssSuperResolution": {
       "presentOpticalFlow": "game",
       "dlssWithoutGameDlss": false
     }
   }
   ```

4. Launch through Steam, load a save, then switch to **2×** in Encore. Confirm **Using Game Vectors**. A live change to 2× may be saved for the next launch. Reapply `multiplier: 1` with the game closed if you want to repeat the staged startup.

## Patches and evidence

| Change | What it does | Status |
| --- | --- | --- |
| Velocity cleanup source fix | Retains texture references throughout cleanup, defers presentation-triggered reset to the render path, and skips cleanup when required resources are missing. | Built and passed the offline D3D11 WARP regression. One in-game run passed the previously crashing horse scene with DLAA and 2× FG. |
| [Early cleanup bypass](historical/velocity-cleanup-bypass.patch) | Source equivalent of the early five-byte DLL workaround. Skips the optional in-engine velocity cleanup and retains the DLSS prepare-pass mask. | Historical. Use only as an alternative on an unmodified v0.2.0 checkout. |
| Staged Encore startup | Starts at 1×, then switches to 2× after a save loads. | Worked with DLAA and **Using Game Vectors**. |
| Classic protection trial | Set `protectionTuning` to `"classic"` under Encore's `imageQuality` object, with `frameProtection` enabled. The same map and dialogue transitions are the comparison cases. | Saved and awaiting a controlled 1× versus 2× retest. |

Original crash signature: `0xc0000005` reading address zero in Clarity 0.2.0's `dinput8.dll` at RVA `0x3c200`, with Encore on the stack. The fault maps to a texture reference in `velocity_mask.cpp` after render-target view creation. The WARP test simulates a presentation callback during cleanup, reads back the corrected velocity, and checks that an interrupted frame skips cleanup safely.

DLSS **Quality** with 2× at startup reached gameplay and reported game vectors, but that session later hit `0xc0000374` (heap corruption). That was a different crash signature. The source fix here has not been validated against that failure. Map and dialogue text may still warp during abrupt cuts. Compare the same scene at 1× and 2× before attributing it to DLAA or FG.

## Offline regression test

From an x64 Visual Studio developer prompt at the repository root:

```bat
mkdir build
pushd build
cl /nologo /std:c++20 /EHsc /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I..\src ..\tests\velocity_mask_warp.cpp ..\src\renderer\velocity_mask.cpp ..\src\renderer\state_backup.cpp /Fe:velocity_mask_warp.exe /link d3d11.lib d3dcompiler.lib
velocity_mask_warp.exe
popd
```

This builds and runs a standalone software-rendered D3D11 test. It does not start the game. Clarity's source retains its [MIT license](../LICENSE). Obtain Encore and the NVIDIA runtime files from their respective upstream releases.
