# Third-party components

None of these are committed to this repository. CMake downloads them at configure time and checks their SHA-256
hashes (pinned in `CMakeLists.txt` and `cmake/dlss-sdk-*.sha256`).

| Component | Version | License | Source |
|---|---|---|---|
| MinHook | v1.3.3 | BSD 2-Clause | https://github.com/TsudaKageyu/minhook |
| NVIDIA DLSS SDK (NGX) | v310.9.1 | NVIDIA RTX SDKs license (`LICENSE.txt` in the SDK) | https://github.com/NVIDIA/DLSS |

The DLSS SDK headers and `nvsdk_ngx_s.lib` are used at build time only. `nvngx_dlss.dll` is NVIDIA's runtime and is
covered by NVIDIA's license, not by this project's MIT license.

Kingdom Come: Deliverance and its files are © Warhorse Studios / Deep Silver. This repository contains none of the
game's code, assets or shaders. It references addresses and byte patterns in `WHGame.dll`.
