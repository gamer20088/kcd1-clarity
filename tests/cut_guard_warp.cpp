// Offline test for the camera-cut guard in renderer/taa_dlss.cpp, on the WARP software rasterizer (no game, no GPU).
// Extracts kPrepareCS and kCutCS from the source, compiles them, and runs both passes on a synthetic frame whose
// "camera" moved by a given UV offset, frame after frame: small offsets and fast turns that build up over frames
// must keep their motion vectors, a sudden jump out of a calm frame (a cut) must zero them.
//
//   cl /nologo /std:c++20 /EHsc /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN tests\cut_guard_warp.cpp
//      /Fe:cut_guard_warp.exe /link d3d11.lib d3dcompiler.lib
//   cut_guard_warp.exe src\renderer\taa_dlss.cpp
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static std::string g_src;

static std::string shader(const char* name) {
    size_t a = g_src.find(std::string("static const char ") + name + "[] = R\"(");
    if (a == std::string::npos)
        return {};
    a = g_src.find("R\"(", a) + 3;
    size_t b = g_src.find(")\";", a);
    return g_src.substr(a, b - a);
}

static ID3D11ComputeShader* compile(ID3D11Device* dev, const std::string& code, const char* name) {
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(code.data(), code.size(), name, nullptr, nullptr, "main", "cs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (FAILED(hr)) {
        printf("FAIL: %s does not compile: %s\n", name, err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        return nullptr;
    }
    ID3D11ComputeShader* cs = nullptr;
    dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs);
    blob->Release();
    return cs;
}

struct Tex {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
};

static Tex make(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT f, bool uav) {
    Tex t;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
    dev->CreateTexture2D(&d, nullptr, &t.tex);
    dev->CreateShaderResourceView(t.tex, nullptr, &t.srv);
    if (uav)
        dev->CreateUnorderedAccessView(t.tex, nullptr, &t.uav);
    return t;
}

static ID3D11Buffer* cbuf(ID3D11Device* dev, UINT size) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = size;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* b = nullptr;
    dev->CreateBuffer(&d, nullptr, &b);
    return b;
}

static void upload(ID3D11DeviceContext* ctx, ID3D11Buffer* b, const void* data, size_t n) {
    D3D11_MAPPED_SUBRESOURCE m;
    ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    memcpy(m.pData, data, n);
    ctx->Unmap(b, 0);
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "src\\renderer\\taa_dlss.cpp";
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    g_src = ss.str();
    std::string prep = shader("kPrepareCS"), cut = shader("kCutCS");
    if (prep.empty() || cut.empty()) {
        printf("FAIL: shaders not found in %s\n", path);
        return 1;
    }

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                 nullptr, &ctx))) {
        printf("FAIL: no WARP device\n");
        return 1;
    }
    ID3D11ComputeShader* prep_cs = compile(dev, prep, "kPrepareCS");
    ID3D11ComputeShader* cut_cs = compile(dev, cut, "kCutCS");
    if (!prep_cs || !cut_cs)
        return 1;

    const UINT W = 160, H = 90;
    Tex colour = make(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
    Tex depth = make(dev, W, H, DXGI_FORMAT_R32_FLOAT, false);
    Tex vel = make(dev, W, H, DXGI_FORMAT_R16G16_FLOAT, false);  // zero: camera motion everywhere
    Tex out_c = make(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    Tex out_d = make(dev, W, H, DXGI_FORMAT_R32_FLOAT, true);
    Tex out_mv = make(dev, W, H, DXGI_FORMAT_R16G16_FLOAT, true);

    // Counter: the same raw buffer + views as taa_dlss.cpp.
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    ID3D11Buffer* cnt = nullptr;
    dev->CreateBuffer(&bd, nullptr, &cnt);
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
    ID3D11UnorderedAccessView* cnt_uav = nullptr;
    ID3D11ShaderResourceView* cnt_srv = nullptr;
    dev->CreateUnorderedAccessView(cnt, &ud, &cnt_uav);
    dev->CreateShaderResourceView(cnt, &sd, &cnt_srv);
    D3D11_BUFFER_DESC pd{};
    pd.ByteWidth = 16;
    pd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Buffer* prev = nullptr;  // the previous frame's fast count, like g_cut_prev
    dev->CreateBuffer(&pd, nullptr, &prev);
    const D3D11_BOX prev_box = {0, 0, 0, 4, 1, 1}, fast_box = {4, 0, 0, 8, 1, 1};

    ID3D11Buffer* engine = cbuf(dev, 256 * 16);
    ID3D11Buffer* params = cbuf(dev, 48);
    ID3D11Buffer* cut_params = cbuf(dev, 16);

    D3D11_TEXTURE2D_DESC rd{};
    rd.Width = W;
    rd.Height = H;
    rd.MipLevels = rd.ArraySize = 1;
    rd.Format = DXGI_FORMAT_R16G16_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Usage = D3D11_USAGE_STAGING;
    rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* readback = nullptr;
    dev->CreateTexture2D(&rd, nullptr, &readback);

    // Returns the motion vector x at the centre pixel after both passes, for a camera offset of (ox, 0) UV.
    auto run = [&](float ox) -> float {
        float e[256 * 4] = {};
        // Row-major reprojection: h = (uv.x + ox, uv.y, z, 1).
        e[0] = 1;
        e[5] = 1;
        e[10] = 1;
        e[12] = ox;
        e[15] = 1;
        upload(ctx, engine, e, sizeof(e));
        struct {
            UINT reproj_reg, row_major;
            float inv_target[2];
            UINT render[2];
            float mv_sign[2];
            float scale[2];
            UINT vel_mask;
            float cut_mv;
        } p = {0, 1, {1.0f / W, 1.0f / H}, {W, H}, {1, 1}, {1, 1}, 0, 0.25f};
        upload(ctx, params, &p, sizeof(p));
        const UINT cp[4] = {W, H, static_cast<UINT>(0.5f * W * H),
                            static_cast<UINT>(0.3f * W * H)};  // kCutFraction, kCutPrevMax
        upload(ctx, cut_params, cp, sizeof(cp));

        const UINT zero[4] = {};
        ctx->ClearUnorderedAccessViewUint(cnt_uav, zero);
        ctx->CopySubresourceRegion(cnt, 0, 8, 0, 0, prev, 0, &prev_box);
        ID3D11ShaderResourceView* srvs[4] = {colour.srv, depth.srv, vel.srv, nullptr};
        ID3D11UnorderedAccessView* uavs[4] = {out_c.uav, out_d.uav, out_mv.uav, cnt_uav};
        ID3D11Buffer* cbs[2] = {engine, params};
        ctx->CSSetShader(prep_cs, nullptr, 0);
        ctx->CSSetShaderResources(0, 4, srvs);
        ctx->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
        ctx->CSSetConstantBuffers(0, 2, cbs);
        ctx->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
        ID3D11ShaderResourceView* ns[4] = {};
        ID3D11UnorderedAccessView* nu[4] = {};
        ctx->CSSetShaderResources(0, 4, ns);
        ctx->CSSetUnorderedAccessViews(0, 4, nu, nullptr);

        ctx->CSSetShader(cut_cs, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &cnt_srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &out_mv.uav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &cut_params);
        ctx->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
        ctx->CSSetShaderResources(0, 1, ns);
        ctx->CSSetUnorderedAccessViews(0, 1, nu, nullptr);
        ctx->CopySubresourceRegion(prev, 0, 0, 0, 0, cnt, 0, &fast_box);

        ctx->CopyResource(readback, out_mv.tex);
        D3D11_MAPPED_SUBRESOURCE m;
        ctx->Map(readback, 0, D3D11_MAP_READ, 0, &m);
        const auto* row = reinterpret_cast<const unsigned short*>(static_cast<const char*>(m.pData) + (H / 2) * m.RowPitch);
        unsigned short hx = row[(W / 2) * 2];
        ctx->Unmap(readback, 0);
        // half -> float
        unsigned sign = (hx >> 15) & 1, ex = (hx >> 10) & 31, man = hx & 1023;
        float v = ex == 0 ? std::ldexp(static_cast<float>(man), -24) : std::ldexp(1.0f + man / 1024.0f, static_cast<int>(ex) - 15);
        return sign ? -v : v;
    };

    struct Case {
        float offset;
        bool expect_zero;
        const char* what;
    } cases[] = {{0.0f, true, "still camera"},  // frames run in this order
                 {0.02f, false, "normal camera move"},
                 {0.2f, false, "fast turn below the threshold"},
                 {0.02f, false, "calm again"},
                 {0.6f, true, "camera cut"},
                 {0.02f, false, "normal move right after a cut"},
                 {0.15f, false, "head shake starts"},
                 {0.3f, false, "head shake, very fast"},
                 {0.6f, false, "head shake, faster still"},
                 {0.0f, true, "still"},
                 {0.4f, true, "second cut"}};
    int failures = 0;
    for (const Case& c : cases) {
        float mv = run(c.offset);
        bool zero = std::fabs(mv) < 1e-4f;
        bool ok = c.expect_zero ? zero : std::fabs(mv - c.offset) < 2e-3f;
        printf("%s: %-32s offset %.3f -> mv %.4f\n", ok ? "ok  " : "FAIL", c.what, c.offset, mv);
        failures += ok ? 0 : 1;
    }
    if (failures) {
        printf("FAIL: %d case(s)\n", failures);
        return 1;
    }
    printf("PASS: prepare and cut-guard shaders compile; vectors kept on normal and fast motion, zeroed on a cut.\n");
    return 0;
}
