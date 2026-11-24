#pragma once

namespace kc {
// Hooks IDXGISwapChain::Present / ResizeBuffers (dxgi.dll code, so every swapchain goes through
// them). Needs d3d11.dll loaded. Returns false if the dummy device or MinHook fails.
bool install_present_hooks();
// Hooks the engine's CD3D9Renderer::RT_EndFrame (after sym::resolve_all), for taa_dlss::on_end_frame.
bool install_end_frame_hook();
}
