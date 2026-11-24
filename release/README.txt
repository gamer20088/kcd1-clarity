KCD1 Clarity encore-lite - game motion vectors for RTX Encore
==============================================================

Unofficial mod. Not affiliated with Warhorse Studios, NVIDIA or RTX Encore. Uses NVIDIA DLSS.

Gives RTX Encore's frame generation Kingdom Come: Deliverance's real motion vectors and
depth, so Encore shows "Using Game Vectors". By default you still see the game's own
SMAA 2TX image; a hidden DLAA pass carries the vectors to Encore.

Requirements
------------
- NVIDIA RTX graphics card.
- Kingdom Come: Deliverance (the first game), Steam 1.9.8, DirectX 11, native resolution.
- Options > Video > Anti-aliasing: SMAA 2TX.
- RTX Encore 2.5.0 with DLSS frame generation on, motion estimation "Game motion vectors",
  and Encore's own upscaling off.

Install
-------
1. Close the game. Open the game folder (Steam: right-click the game > Manage > Browse
   local files).
2. Copy dinput8.dll into Bin\Win64 (next to KingdomCome.exe). Copy nvngx_dlss.dll too
   only if Bin\Win64 doesn't already have one.
3. Start the game and load a save. Encore's overlay should show "Using Game Vectors".

Settings (Bin\Win64\kcd_clarity.ini)
------------------------------------
[general] mode=1    0 = off, 1 = on
[general] output=0  0 = game's SMAA 2TX on screen, 1 = DLAA on screen

Conflicts
---------
Any other mod that installs a dinput8.dll (Ultimate ASI Loader, full KCD1 Clarity)
conflicts with this one. Only one can be installed.

Uninstall
---------
Delete dinput8.dll from Bin\Win64. Also delete nvngx_dlss.dll if you copied it and
nothing else uses it, plus kcd_clarity.ini and kcd_clarity.log.

Licenses
--------
MIT licensed. nvngx_dlss.dll is NVIDIA's and covered by the NVIDIA license in
kcd_clarity_licenses. Source and bug reports: https://github.com/gamer20088/kcd1-clarity
