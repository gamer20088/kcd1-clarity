#include "renderer/velocity_mask.h"
#include <cstdio>
#include <cstdlib>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace mask = kc::velocity_mask;

namespace kc {
void logf(const char*, ...) {}
}

static void require(bool result, const char* name) {
    if (!result) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        std::exit(1);
    }
}

static void STDMETHODCALLTYPE draw_during_present(ID3D11DeviceContext* context, UINT count, UINT start) {
    // Simulate Encore requesting the next frame concurrently with the cleanup draw.
    std::thread present(mask::on_present);
    present.join();
    context->Draw(count, start);
}

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                        D3D11_SDK_VERSION, &device, &level, &context)), "WARP device");

    D3D11_TEXTURE2D_DESC texture{640, 360, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                               D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    ComPtr<ID3D11Texture2D> color;
    ComPtr<ID3D11RenderTargetView> color_view;
    require(SUCCEEDED(device->CreateTexture2D(&texture, nullptr, &color)), "color texture");
    require(SUCCEEDED(device->CreateRenderTargetView(color.Get(), nullptr, &color_view)), "color view");

    texture.Format = DXGI_FORMAT_R16G16_SNORM;
    ComPtr<ID3D11Texture2D> velocity;
    ComPtr<ID3D11RenderTargetView> velocity_view;
    require(SUCCEEDED(device->CreateTexture2D(&texture, nullptr, &velocity)), "velocity texture");
    require(SUCCEEDED(device->CreateRenderTargetView(velocity.Get(), nullptr, &velocity_view)), "velocity view");

    auto stage_desc = texture;
    stage_desc.Usage = D3D11_USAGE_STAGING;
    stage_desc.BindFlags = 0;
    stage_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    require(SUCCEEDED(device->CreateTexture2D(&stage_desc, nullptr, &staging)), "velocity readback texture");

    texture.Format = DXGI_FORMAT_R24G8_TYPELESS;
    texture.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> depth_view;
    require(SUCCEEDED(device->CreateTexture2D(&texture, nullptr, &depth)), "depth texture");
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    require(SUCCEEDED(device->CreateDepthStencilView(depth.Get(), &dsv, &depth_view)), "depth view");

    ID3D11RenderTargetView* moving[] = {color_view.Get(), velocity_view.Get()};
    ID3D11RenderTargetView* fixed[] = {color_view.Get()};
    const float initial_velocity[] = {0.5f, 0.5f, 0.0f, 0.0f};
    for (unsigned i = 0; i < 8; ++i) {
        mask::on_present();
        context->ClearRenderTargetView(velocity_view.Get(), initial_velocity);
        context->ClearDepthStencilView(depth_view.Get(), D3D11_CLEAR_DEPTH, 0.75f, 0);
        mask::on_set_targets(context.Get(), 2, moving, depth_view.Get(), draw_during_present);
        mask::on_set_targets(context.Get(), 1, fixed, depth_view.Get(), draw_during_present);
        const auto held_depth = mask::depth_copy();
        require(held_depth.Get() != nullptr, "prepare-pass depth snapshot retained");
        context->ClearDepthStencilView(depth_view.Get(), D3D11_CLEAR_DEPTH, 0.25f, 0);
        mask::on_set_targets(context.Get(), 0, nullptr, nullptr, draw_during_present);
        require(mask::status().cleanups == i + 1, "cleanup ran with concurrent presentation");
        require(mask::depth_copy().Get() != nullptr, "present did not clear current draw's snapshot");
        context->CopyResource(staging.Get(), velocity.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "velocity readback");
        const auto* samples = static_cast<const std::int16_t*>(mapped.pData);
        const bool cleared = samples[0] == 0 && samples[1] == 0;
        context->Unmap(staging.Get(), 0);
        require(cleared, "cleanup actually cleared stale motion");
        mask::on_set_targets(context.Get(), 0, nullptr, nullptr, draw_during_present);
        require(mask::depth_copy().Get() == nullptr, "next target binding cleared stale snapshot");
    }

    mask::on_present();
    mask::on_set_targets(context.Get(), 2, moving, depth_view.Get(), draw_during_present);
    mask::on_set_targets(context.Get(), 1, fixed, depth_view.Get(), draw_during_present);
    mask::on_present();
    mask::on_set_targets(context.Get(), 0, nullptr, nullptr, draw_during_present);
    require(mask::status().cleanups == 8, "skipped interrupted cleanup without pending resources");
    std::puts("PASS: WARP cleanup, velocity readback, concurrent Present, and interrupted frame.");
}
