#include "renderer/taa_dlss.h"

#include <windows.h>

#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <cstring>

#include "core/log.h"
#include "game/symbols.h"
#include "renderer/state_backup.h"
#include "renderer/velocity_mask.h"
#include "upscaler/dlss.h"

namespace kc::taa_dlss {

static Settings g_settings;
static Status g_status;
static bool g_reset = true;

Settings& settings() { return g_settings; }
const Status& status() { return g_status; }
void request_reset() { g_reset = true; }

// ---------------------------------------------------------------------------------------------------------
// Shaders

// Mirrors PostAA_PS (PostAA.cfx, 2TX path) and DecodeMotionVector (Common.cfi:306).
static const char kPrepareCS[] = R"(
Texture2D<float4> InColour : register(t0);
Texture2D<float>  InDepth  : register(t1);
Texture2D<float2> InVel    : register(t2);
Texture2D<float>  VelDepth : register(t3);  // depth right after the moving objects (velocity_mask)
cbuffer Engine : register(b0) { float4 E[256]; };  // the engine's constant buffer with mReprojection
cbuffer Params : register(b1) {
    uint  ReprojReg;   // float4 register of mReprojection in Engine
    uint  RowMajor;
    float2 InvTarget;  // 1 / target texture size: the engine's baseTC is normalized to the whole texture
    uint2 RenderSize;
    float2 MvSign;
    float2 Scale;      // render size / target size: the sub-rect the scene occupies
    uint  VelMask;     // VelDepth is valid: object velocity only where the depth is still the moving object's
    float CutMv;       // camera-cut guard: count pixels moving more than this (UV units per frame); 0 = off
};
RWTexture2D<float4> OutColour : register(u0);
RWTexture2D<float>  OutDepth  : register(u1);
RWTexture2D<float2> OutMv     : register(u2);
RWByteAddressBuffer CutCount  : register(u3);  // [0] pixels over CutMv, [1] over CutMv / 2 (this frame)

float2 DecodeMotionVector(float2 e) {
    e = (e - 127.0 / 255.0) * 2.0;
    return (e * e) * float2(e.x > 0.0 ? 1.0 : -1.0, e.y > 0.0 ? 1.0 : -1.0);
}

groupshared uint g_cut;
groupshared uint g_fast;

float2 prepare(uint2 id);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi == 0) {
        g_cut = 0;
        g_fast = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (all(id < RenderSize)) {
        float2 mv = prepare(id);
        float m2 = dot(mv, mv);
        if (CutMv > 0.0 && m2 > 0.25 * CutMv * CutMv) {
            InterlockedAdd(g_fast, 1);
            if (m2 > CutMv * CutMv)
                InterlockedAdd(g_cut, 1);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0 && g_cut > 0)
        CutCount.InterlockedAdd(0, g_cut);
    if (gi == 0 && g_fast > 0)
        CutCount.InterlockedAdd(4, g_fast);
}

// Writes the outputs for one pixel and returns its motion vector (before MvSign).
float2 prepare(uint2 id) {
    int3 p = int3(id, 0);
    float depth = InDepth.Load(p);
    float2 tc = (float2(id) + 0.5) * InvTarget;
    // mReprojection is built for full-view UVs (CPostAA::Render has no HPosScale term), so camera motion is
    // reprojected at tc / Scale and brought back to sub-rect units. Object velocities are already in sub-rect
    // units (HPosToScreenTC scales them).
    float2 uv = tc / Scale;
    // Sky / far plane: the reprojection's w goes to 0 there. The engine's TAA shrugs off the resulting Inf/NaN
    // (treated as offscreen); DLSS spreads it through its history, so keep the depth off the exact limits and
    // drop any non-finite or absurd vector.
    float4 v = float4(uv, clamp(depth, 1e-6, 1.0 - 1e-6), 1.0);
    float4 h;
    if (RowMajor)
        h = v.x * E[ReprojReg] + v.y * E[ReprojReg + 1] + v.z * E[ReprojReg + 2] + v.w * E[ReprojReg + 3];
    else
        h = float4(dot(v, E[ReprojReg]), dot(v, E[ReprojReg + 1]), dot(v, E[ReprojReg + 2]), dot(v, E[ReprojReg + 3]));
    float2 obj = InVel.Load(p);
    if (VelMask && VelDepth.Load(p) != InDepth.Load(p))
        obj = 0.0;  // covered by something drawn later (static geometry never writes velocity)
    float2 mv = (obj.x == 0.0) ? (h.xy / h.w - uv) * Scale : DecodeMotionVector(obj);
    if (any(isnan(mv)) || any(isinf(mv)) || abs(h.w) < 1e-8 || any(abs(mv) > 2.0))
        mv = 0.0;
    OutMv[id] = mv * MvSign;
    OutDepth[id] = clamp(depth, 1e-6, 1.0 - 1e-6);  // reversed Z: the sky is exactly 0
    OutColour[id] = InColour.Load(p);
    return mv;
}
)";

// Camera-cut guard, second pass: when most of the screen "moved" more than CutMv in one frame, the engine's
// reprojection points at the previous shot (dialogue cut, teleport, map or menu transition). Those vectors are
// garbage and make frame generation warp the whole image, text included; zero vectors make it blend instead.
// A cut comes out of nowhere: the frame before it was calm. A fast turn (shaking the head, riding) builds up over
// frames, so when the previous frame already moved fast, the vectors are real and are kept.
static const char kCutCS[] = R"(
ByteAddressBuffer CutCount : register(t0);  // [0] pixels over CutMv, [2] pixels over CutMv / 2 in the previous frame
RWTexture2D<float2> Mv : register(u0);
cbuffer Params : register(b0) { uint2 Size; uint Threshold; uint PrevMax; };
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= Size))
        return;
    if (CutCount.Load(0) > Threshold && CutCount.Load(8) <= PrevMax)
        Mv[id] = float2(0.0, 0.0);
}
)";

// Full-screen triangle; output re-encoded to sRGB like the end of PostAA_PS. Alpha 0 like PostAA_PS (pixout zeroed).
static const char kWritebackVS[] = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";
static const char kWritebackPS[] = R"(
Texture2D<float4> Src : register(t0);  // DLSS output
float4 main(float4 pos : SV_Position) : SV_Target {
    float4 o = Src.Load(int3(pos.xy, 0));
    if (any(isnan(o)) || any(isinf(o))) return float4(1, 0, 1, 0);  // NaN/Inf magenta
    float3 c = saturate(o.rgb);
    c = (c < 0.0031308) ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return float4(c, 0.0);
}
)";

static ID3D11ComputeShader* g_prepare_cs;
static ID3D11VertexShader* g_writeback_vs;
static ID3D11PixelShader* g_writeback_ps;
static ID3D11Buffer* g_params;
static bool g_shaders_failed;

// Camera-cut guard resources.
constexpr float kCutMotion = 0.25f;   // UV per frame: a quarter of the screen in one frame
constexpr float kCutFraction = 0.5f;   // share of the screen that must exceed it (real cuts: 68-100% in game)
constexpr float kCutPrevMax = 0.3f;    // and at most this share moved over half of kCutMotion the frame before
                                       // (fast turns and riding reached 50-74% while already moving fast)
constexpr int kCutRing = 3;           // staging copies, read back a couple of frames late (log only)
static ID3D11ComputeShader* g_cut_cs;
static ID3D11Buffer* g_cut_params;
static ID3D11Buffer* g_cut_buf;
static ID3D11Buffer* g_cut_prev;  // [0] = g_cut_buf[1] of the previous frame
static ID3D11ShaderResourceView* g_cut_srv;
static ID3D11UnorderedAccessView* g_cut_uav;
static ID3D11Buffer* g_cut_staging[kCutRing];
static bool g_cut_staged[kCutRing];
static int g_cut_next;
static uint64_t g_cut_frames, g_cut_logged_at, g_cut_kept, g_cut_kept_logged_at;

static bool make_cut_resources(ID3D11Device* device) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &g_cut_buf)))
        return false;
    D3D11_BUFFER_DESC pd{};
    pd.ByteWidth = 16;
    pd.Usage = D3D11_USAGE_DEFAULT;
    pd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateBuffer(&pd, nullptr, &g_cut_prev)))
        return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 4;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.NumElements = 4;
    sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    if (FAILED(device->CreateUnorderedAccessView(g_cut_buf, &ud, &g_cut_uav)) ||
        FAILED(device->CreateShaderResourceView(g_cut_buf, &sd, &g_cut_srv)))
        return false;
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 16;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &g_cut_params)))
        return false;
    D3D11_BUFFER_DESC st{};
    st.ByteWidth = 16;
    st.Usage = D3D11_USAGE_STAGING;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (ID3D11Buffer*& s : g_cut_staging)
        if (FAILED(device->CreateBuffer(&st, nullptr, &s)))
            return false;
    return true;
}

static ID3DBlob* compile(const char* src, size_t len, const char* target, const char* name) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(src, len, name, nullptr, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &code, &errors);
    if (FAILED(hr))
        logf("taa_dlss: compile %s failed 0x%08lx: %s", name, hr,
             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
    if (errors)
        errors->Release();
    return SUCCEEDED(hr) ? code : nullptr;
}

static bool ensure_shaders(ID3D11Device* device) {
    if (g_status.shaders_ready || g_shaders_failed)
        return g_status.shaders_ready;
    d3d11::ScopedInside inside;
    ID3DBlob* cs = compile(kPrepareCS, sizeof(kPrepareCS) - 1, "cs_5_0", "kcd_prepare_cs");
    ID3DBlob* vs = compile(kWritebackVS, sizeof(kWritebackVS) - 1, "vs_5_0", "kcd_writeback_vs");
    ID3DBlob* ps = compile(kWritebackPS, sizeof(kWritebackPS) - 1, "ps_5_0", "kcd_writeback_ps");
    ID3DBlob* cut = compile(kCutCS, sizeof(kCutCS) - 1, "cs_5_0", "kcd_cut_cs");
    bool ok = cs && vs && ps && cut &&
              SUCCEEDED(device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &g_prepare_cs)) &&
              SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_writeback_vs)) &&
              SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_writeback_ps)) &&
              SUCCEEDED(device->CreateComputeShader(cut->GetBufferPointer(), cut->GetBufferSize(), nullptr, &g_cut_cs));
    for (ID3DBlob* b : {cs, vs, ps, cut})
        if (b)
            b->Release();
    ok = ok && make_cut_resources(device);

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 48;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(device->CreateBuffer(&bd, nullptr, &g_params));
    g_status.shaders_ready = ok;
    g_shaders_failed = !ok;
    logf("taa_dlss: shaders and state %s", ok ? "ready" : "FAILED");
    return ok;
}

// ---------------------------------------------------------------------------------------------------------
// Working textures (target size)

struct Tex {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    void release() {
        if (uav) uav->Release();
        if (srv) srv->Release();
        if (tex) tex->Release();
        *this = Tex{};
    }
};
static Tex g_colour, g_depth, g_mv, g_out;
static UINT g_tex_w, g_tex_h;

static bool make(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format, Tex* t) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    return SUCCEEDED(device->CreateTexture2D(&d, nullptr, &t->tex)) &&
           SUCCEEDED(device->CreateShaderResourceView(t->tex, nullptr, &t->srv)) &&
           SUCCEEDED(device->CreateUnorderedAccessView(t->tex, nullptr, &t->uav));
}

static bool ensure_textures(ID3D11Device* device, UINT w, UINT h) {
    if (g_status.resources_ready && w == g_tex_w && h == g_tex_h)
        return true;
    for (Tex* t : {&g_colour, &g_depth, &g_mv, &g_out})
        t->release();
    bool ok = make(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, &g_colour) &&
              make(device, w, h, DXGI_FORMAT_R32_FLOAT, &g_depth) &&
              make(device, w, h, DXGI_FORMAT_R16G16_FLOAT, &g_mv) &&
              make(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, &g_out);
    g_tex_w = w;
    g_tex_h = h;
    g_status.resources_ready = ok;
    g_reset = true;
    logf("taa_dlss: working textures %ux%u %s", w, h, ok ? "ready" : "FAILED");
    return ok;
}

// ---------------------------------------------------------------------------------------------------------

static uint64_t g_taa_draws_seen;  // TAA draws since start (render thread)

// Map fix (map_fix): on frames where the game draws no 3D scene (map, inventory, menus), there is no TAA draw and so
// no DLSS call, and Encore falls back to its own motion estimation for them. At the engine's RT_EndFrame (render
// thread, after the frame is drawn, before it is presented) such frames get a DLSS call with zero motion vectors, so
// Encore keeps using game vectors and treats the screen as still. Frames with a 3D scene are not touched.
static DWORD g_render_thread;               // thread of the TAA draws
static ID3D11DeviceContext* g_ctx;          // the immediate context of the TAA draws (AddRef'd once, kept)
static dlss::EvalInputs g_last_in;          // inputs of the last 3D frame
static bool g_have_inputs;
static bool g_taa_this_frame;
static uint64_t g_map_evals;

void on_end_frame() {
    if (GetCurrentThreadId() != g_render_thread)
        return;
    if (g_taa_this_frame) {
        g_taa_this_frame = false;
        return;
    }
    if (!g_settings.map_fix || g_settings.mode != Mode::On || !g_have_inputs || !g_ctx)
        return;
    bool ok;
    {
        d3d11::ScopedInside inside;
        StateBackup backup(g_ctx);
        const float zero[4] = {};
        g_ctx->ClearUnorderedAccessViewFloat(g_mv.uav, zero);
        dlss::EvalInputs in = g_last_in;
        in.jitter_x = in.jitter_y = 0.0f;
        in.reset = false;
        ok = dlss::evaluate(g_ctx, in);
    }
    if (++g_map_evals == 1 || !ok)
        logf("taa_dlss: map fix: no 3D scene, zero motion vectors handed to Encore (%s)",
             ok ? "ok" : "DLSS evaluate failed");
}

void on_present() {
    // Log when the game stops drawing its 3D scene (map, menus, loading) and when it resumes. On those frames there
    // is no DLSS call, so Encore has no game vectors and falls back to its own motion estimation.
    static uint64_t last_draws, presents_without;
    static bool paused;
    if (g_taa_draws_seen != last_draws) {
        if (paused)
            logf("taa_dlss: 3D scene resumed after %llu presents without it", presents_without);
        last_draws = g_taa_draws_seen;
        presents_without = 0;
        paused = false;
    } else if (++presents_without == 6 && g_settings.mode == Mode::On) {
        paused = true;  // 6 presents: about 3 real frames at 2x frame generation
        logf("taa_dlss: 3D scene stopped (map, menu or loading?); no game vectors for Encore until it resumes");
    }

    // Halton only matters when the DLAA image is what you see; with the game's SMAA 2TX on screen keep its pattern.
    static int saved_pattern = -1;
    int* p = sym::ptr<int>(sym::Id::CV_r_AntialiasingTAAPattern);
    if (!p)
        return;
    bool force = g_settings.mode == Mode::On && g_settings.output == Output::Dlaa && g_settings.force_halton;
    if (force) {
        if (saved_pattern < 0 && *p != 8) {
            saved_pattern = *p;
            logf("taa_dlss: r_AntialiasingTAAPattern %d -> 8", saved_pattern);
        }
        *p = 8;
    } else if (saved_pattern >= 0) {
        *p = saved_pattern;
        logf("taa_dlss: r_AntialiasingTAAPattern restored to %d", saved_pattern);
        saved_pattern = -1;
    }
}

// Engine bindings at the TAA draw, AddRef'd by the Get calls.
struct Bound {
    ID3D11ShaderResourceView* colour = nullptr;
    ID3D11ShaderResourceView* depth = nullptr;
    ID3D11ShaderResourceView* velocity = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    D3D11_VIEWPORT viewport{};
    ~Bound() {
        for (IUnknown* u : {static_cast<IUnknown*>(colour), static_cast<IUnknown*>(depth),
                            static_cast<IUnknown*>(velocity), static_cast<IUnknown*>(cb), static_cast<IUnknown*>(rtv)})
            if (u)
                u->Release();
    }
};

static bool fail(const char* why) {
    if (strcmp(g_status.last_error, why) != 0)
        logf("taa_dlss: falling back to engine TAA: %s", why);
    g_status.last_error = why;
    g_status.last_ok = false;
    return false;
}

static float read_float(uintptr_t base, uintptr_t offset) {
    float v;
    memcpy(&v, reinterpret_cast<const void*>(base + offset), sizeof(v));
    return v;
}

bool on_taa_draw(ID3D11DeviceContext* ctx, const d3d11::TaaShader& taa) {
    ++g_taa_draws_seen;
    if (g_settings.mode != Mode::On)
        return false;

    Bound b;
    ctx->PSGetShaderResources(taa.slot_colour, 1, &b.colour);
    ctx->PSGetShaderResources(taa.slot_depth, 1, &b.depth);
    ctx->PSGetShaderResources(taa.slot_velocity, 1, &b.velocity);
    ctx->PSGetConstantBuffers(taa.cb_slot, 1, &b.cb);
    ctx->OMGetRenderTargets(1, &b.rtv, nullptr);
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &b.viewport);
    if (!b.colour || !b.depth || !b.velocity || !b.cb || !b.rtv || nvp == 0)
        return fail("TAA draw without the expected bindings");

    ID3D11Resource* target = nullptr;
    b.rtv->GetResource(&target);
    D3D11_TEXTURE2D_DESC td;
    static_cast<ID3D11Texture2D*>(target)->GetDesc(&td);
    target->Release();
    // Native resolution only: the TAA draw must cover the whole target.
    const UINT w = td.Width, h = td.Height;
    if (b.viewport.TopLeftX != 0 || b.viewport.TopLeftY != 0 || static_cast<UINT>(b.viewport.Width) != w ||
        static_cast<UINT>(b.viewport.Height) != h)
        return fail("TAA viewport is not the full target (not native resolution)");

    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    bool ready = ensure_shaders(device) && ensure_textures(device, w, h);
    device->Release();
    if (!ready)
        return fail("shader or texture setup failed");

    // Jitter: RT_RenderScene stores 2*j/(render size) in clip space at gRenDev+0xF20 (kcd-re notes/40-jitter.md).
    float jx = 0, jy = 0;
    if (uintptr_t r = sym::addr(sym::Id::gRenDev)) {
        jx = read_float(r, 0xF20) * w * 0.5f;
        jy = read_float(r, 0xF24) * h * 0.5f;
    }
    if (g_settings.jitter_flip_x)
        jx = -jx;
    if (g_settings.jitter_flip_y)
        jy = -jy;
    g_status.jitter_x = jx;
    g_status.jitter_y = jy;

    int* reverse = sym::ptr<int>(sym::Id::CV_r_ReverseDepth);
    g_status.reverse_depth = reverse ? *reverse : -1;
    dlss::Settings& ds = dlss::settings();
    ds.is_hdr = true;  // colour is linear (sRGB view of the SMAA output), in [0, 1]
    ds.depth_inverted = g_settings.depth_mode == 2 || (g_settings.depth_mode == 0 && g_status.reverse_depth > 0);

    // Owns the depth view for the whole pass, even if a generated frame presents meanwhile (crash fix 415fd14).
    const auto vel_depth = velocity_mask::depth_copy();
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_params, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return fail("Map failed");
    struct {
        UINT reproj_reg, row_major;
        float inv_target[2];
        UINT render[2];
        float mv_sign[2];
        float scale[2];
        UINT vel_mask;
        float cut_mv;
    } params = {taa.reproj_offset / 16, taa.reproj_row_major ? 1u : 0u,
                {1.0f / w, 1.0f / h}, {w, h},
                {g_settings.mv_flip_x ? -1.0f : 1.0f, g_settings.mv_flip_y ? -1.0f : 1.0f},
                {1.0f, 1.0f},
                vel_depth ? 1u : 0u, g_settings.cut_guard ? kCutMotion : 0.0f};
    memcpy(m.pData, &params, sizeof(params));
    ctx->Unmap(g_params, 0);

    // Everything from here, NGX included, must leave the engine's bindings (and its cached view of them) as they
    // were: the engine's own TAA draw may run right after us.
    bool ok = false;
    {
        StateBackup backup(ctx);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->IASetInputLayout(nullptr);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(nullptr, 0);
        ctx->RSSetState(nullptr);

        bool recreated = false;
        if (!dlss::ensure(ctx, w, h, w, h, &recreated))
            return fail("DLSS not available (see log)");

        // 1. Colour/depth copies and full-screen motion vectors.
        ID3D11ShaderResourceView* srvs[4] = {b.colour, b.depth, b.velocity, vel_depth.Get()};
        ID3D11UnorderedAccessView* uavs[4] = {g_colour.uav, g_depth.uav, g_mv.uav, g_cut_uav};
        ID3D11Buffer* cbs[2] = {b.cb, g_params};
        const UINT zero[4] = {};
        ctx->ClearUnorderedAccessViewUint(g_cut_uav, zero);
        const D3D11_BOX prev_box = {0, 0, 0, 4, 1, 1}, fast_box = {4, 0, 0, 8, 1, 1};
        ctx->CopySubresourceRegion(g_cut_buf, 0, 8, 0, 0, g_cut_prev, 0, &prev_box);
        ctx->CSSetShader(g_prepare_cs, nullptr, 0);
        ctx->CSSetShaderResources(0, 4, srvs);
        ctx->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
        ctx->CSSetConstantBuffers(0, 2, cbs);
        ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        ID3D11ShaderResourceView* null_srvs[4] = {};
        ID3D11UnorderedAccessView* null_uavs[4] = {};
        ID3D11Buffer* null_cbs[2] = {};
        ctx->CSSetShaderResources(0, 4, null_srvs);
        ctx->CSSetUnorderedAccessViews(0, 4, null_uavs, nullptr);
        ctx->CSSetConstantBuffers(0, 2, null_cbs);

        // 1b. Camera-cut guard: zero the vectors when most of the screen jumped (decided on the GPU, same frame).
        if (g_settings.cut_guard && SUCCEEDED(ctx->Map(g_cut_params, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            const UINT cp[4] = {w, h, static_cast<UINT>(kCutFraction * w * h),
                                static_cast<UINT>(kCutPrevMax * w * h)};
            memcpy(m.pData, cp, sizeof(cp));
            ctx->Unmap(g_cut_params, 0);
            ctx->CSSetShader(g_cut_cs, nullptr, 0);
            ctx->CSSetShaderResources(0, 1, &g_cut_srv);
            ctx->CSSetUnorderedAccessViews(0, 1, &g_mv.uav, nullptr);
            ctx->CSSetConstantBuffers(0, 1, &g_cut_params);
            ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            ctx->CSSetShaderResources(0, 1, null_srvs);
            ctx->CSSetUnorderedAccessViews(0, 1, null_uavs, nullptr);
            ctx->CSSetConstantBuffers(0, 1, null_cbs);
            ctx->CopySubresourceRegion(g_cut_prev, 0, 0, 0, 0, g_cut_buf, 0, &fast_box);
            // Log only: read the count back a couple of frames late, never waiting on the GPU.
            ctx->CopyResource(g_cut_staging[g_cut_next], g_cut_buf);
            g_cut_staged[g_cut_next] = true;
            g_cut_next = (g_cut_next + 1) % kCutRing;
            if (g_cut_staged[g_cut_next]) {
                D3D11_MAPPED_SUBRESOURCE rb;
                if (SUCCEEDED(ctx->Map(g_cut_staging[g_cut_next], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &rb))) {
                    const UINT* c = static_cast<const UINT*>(rb.pData);
                    const UINT count = c[0], prev_fast = c[2];
                    ctx->Unmap(g_cut_staging[g_cut_next], 0);
                    g_cut_staged[g_cut_next] = false;
                    const double px = static_cast<double>(w) * h;
                    if (count > cp[2] && prev_fast > cp[3]) {
                        ++g_cut_kept;
                        if (g_status.frames - g_cut_kept_logged_at > 300 || g_cut_kept_logged_at == 0) {
                            logf("taa_dlss: fast camera motion, not a cut, vectors kept (%.0f%% jumped, %.0f%% already "
                                 "fast the frame before; %llu so far)",
                                 100.0 * count / px, 100.0 * prev_fast / px, g_cut_kept);
                            g_cut_kept_logged_at = g_status.frames;
                        }
                    } else if (count > cp[2]) {
                        ++g_cut_frames;
                        if (g_status.frames - g_cut_logged_at > 30 || g_cut_logged_at == 0) {
                            logf("taa_dlss: camera cut, motion vectors zeroed (%.0f%% of the screen jumped, %.0f%% "
                                 "fast the frame before; %llu so far)",
                                 100.0 * count / px, 100.0 * prev_fast / px, g_cut_frames);
                            g_cut_logged_at = g_status.frames;
                        }
                    }
                }
            }
        }

        // 2. DLAA. This NGX call is what RTX Encore reads the motion vectors and depth from.
        dlss::EvalInputs in;
        in.colour = g_colour.tex;
        in.depth = g_depth.tex;
        in.motion = g_mv.tex;
        in.output = g_out.tex;
        in.render_width = w;
        in.render_height = h;
        in.jitter_x = jx;
        in.jitter_y = jy;
        in.mv_scale_x = static_cast<float>(w);  // MVs are in texture UV units
        in.mv_scale_y = static_cast<float>(h);
        in.reset = g_reset || recreated;
        ok = dlss::evaluate(ctx, in);
        if (ok) {
            if (!g_ctx) {
                g_ctx = ctx;
                g_ctx->AddRef();
            }
            g_render_thread = GetCurrentThreadId();
            g_last_in = in;
            g_have_inputs = true;
            g_taa_this_frame = true;
        }
        g_reset = false;

        // 3. Output = Dlaa only: into the engine's TAA output, encoded like PostAA_PS.
        if (ok && g_settings.output == Output::Dlaa) {
            D3D11_VIEWPORT full = {0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
            ctx->OMSetRenderTargets(1, &b.rtv, nullptr);
            ctx->RSSetViewports(1, &full);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(g_writeback_vs, nullptr, 0);
            ctx->PSSetShader(g_writeback_ps, nullptr, 0);
            ctx->PSSetShaderResources(0, 1, &g_out.srv);
            ctx->Draw(3, 0);
            ID3D11ShaderResourceView* null_srv = nullptr;
            ID3D11RenderTargetView* null_rtv = nullptr;
            ctx->PSSetShaderResources(0, 1, &null_srv);
            ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
        }
    }  // engine state restored

    g_status.width = w;
    g_status.height = h;
    ++g_status.frames;
    if (!ok)
        return fail("DLSS evaluate failed");
    if (!g_status.last_ok)
        logf("taa_dlss: running at %ux%u, output %s", w, h, g_settings.output == Output::Dlaa ? "DLAA" : "game AA");
    g_status.last_ok = true;
    g_status.last_error = "";
    // Game output: let the engine's SMAA 2TX draw run as normal.
    return g_settings.output == Output::Dlaa;
}

}  // namespace kc::taa_dlss
