// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Wave 12 end-to-end D3D12 render regression and performance benchmark.
//       a) Hand-assembled RDNA2 Mesh Shader (MSG_GS_ALLOC_REQ -> ms_6_6) + RDNA2 Pixel Shader (ps_6_6)
//          rasterized live via DispatchMesh(1,1,1) onto 320x180 RGBA8 render target,
//       b) Live amd_fidelityfx_upscaler_xbox.dll (FSR 4.1.1 SM 6.6) upscaling 320x180 -> 640x360
//          (reset + temporal + RCAS sharpening) through native Xbox ABI Device & CommandList proxies,
//       c) Back-to-back determinism PSNR (> 60 dB) + analytical golden-region PSNR (> 45 dB),
//       d) Upgraded Pure DX12 Debug HUD overlay + D3D12XboxCaptureBackbufferPng -> golden_wave12_render.png.

#include "d3d12_xs_rdna2_to_hlsl.h"
#include "d3d12_xs_sm66_runtime.h"
#include "include/xbox/d3d12_xs.h"

#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(cond, msg, ...)                                        \
    do {                                                             \
        if (!(cond)) {                                               \
            std::fprintf(stderr, "[FAIL] " msg "\n", ##__VA_ARGS__); \
            ++g_failures;                                            \
        } else {                                                     \
            std::printf("[PASS] " msg "\n", ##__VA_ARGS__);          \
        }                                                            \
    } while (0)

// RDNA2 (GFX10.3) encoders
inline uint32_t enc_sopp(uint32_t op, int16_t simm16 = 0) {
    return 0xBF800000u | ((op & 0x7Fu) << 16) | (static_cast<uint32_t>(simm16) & 0xFFFFu);
}
inline uint32_t enc_sop1(uint32_t op, uint32_t sd, uint32_t ssrc0) {
    return 0xBE800000u | ((sd & 0x7Fu) << 16) | ((op & 0xFFu) << 8) | (ssrc0 & 0xFFu);
}
inline uint32_t enc_vop1(uint32_t op, uint32_t vd, uint32_t src0) {
    return 0x7E000000u | ((vd & 0xFFu) << 17) | ((op & 0xFFu) << 9) | (src0 & 0x1FFu);
}
inline uint32_t enc_vop2(uint32_t op, uint32_t vd, uint32_t src0, uint32_t vsrc1) {
    return ((op & 0x3Fu) << 25) | ((vd & 0xFFu) << 17) | ((vsrc1 & 0xFFu) << 9) | (src0 & 0x1FFu);
}
inline uint32_t enc_vop3_w0(uint32_t op, uint32_t vd) {
    return (0x35u << 26) | ((op & 0x3FFu) << 16) | (vd & 0xFFu);
}
inline uint32_t enc_vop3_w1(uint32_t s0, uint32_t s1, uint32_t s2 = 0u) {
    return (s0 & 0x1FFu) | ((s1 & 0x1FFu) << 9) | ((s2 & 0x1FFu) << 18);
}
inline void push_exp(std::vector<uint32_t>& w, uint32_t tgt, uint32_t en,
                     uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3,
                     bool done = false, bool vm = false) {
    w.push_back((0x3Eu << 26) | (vm ? (1u << 12) : 0u) | (done ? (1u << 11) : 0u) | ((tgt & 0x3Fu) << 4) | (en & 0xFu));
    w.push_back((v0 & 0xFFu) | ((v1 & 0xFFu) << 8) | ((v2 & 0xFFu) << 16) | ((v3 & 0xFFu) << 24));
}
inline uint32_t f32_bits(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    return u;
}

// FidelityFX API v2.0 C ABI structures matching amd_fidelityfx_upscaler_xbox.dll
struct FfxApiHeader { uint64_t type; FfxApiHeader* pNext; };
struct FfxApiDimensions2D { uint32_t width; uint32_t height; };
struct FfxApiFloatCoords2D { float x; float y; };
struct FfxApiResourceDescription {
    uint32_t type, format, width, height, depth, mipCount, flags, usage;
};
struct FfxApiResource {
    void* resource;
    FfxApiResourceDescription description;
    uint32_t state;
    uint32_t _pad;
};
struct FfxCreateContextDescUpscale {
    FfxApiHeader header; uint32_t flags; FfxApiDimensions2D maxRenderSize;
    FfxApiDimensions2D maxUpscaleSize; void* fpMessage;
};
struct FfxCreateBackendDX12Desc { FfxApiHeader header; void* device; };
struct FfxDispatchDescUpscale {
    FfxApiHeader header;
    void* commandList;
    FfxApiResource color;
    FfxApiResource depth;
    FfxApiResource motionVectors;
    FfxApiResource exposure;
    FfxApiResource reactive;
    FfxApiResource transparencyAndComposition;
    FfxApiResource output;
    FfxApiFloatCoords2D jitterOffset;
    FfxApiFloatCoords2D motionVectorScale;
    FfxApiDimensions2D renderSize;
    FfxApiDimensions2D upscaleSize;
    bool enableSharpening;
    float sharpness;
    float frameTimeDelta;
    float preExposure;
    bool reset;
    float cameraNear;
    float cameraFar;
    float cameraFovAngleVertical;
    float viewSpaceToMetersFactor;
    uint32_t flags;
};

using FnFfxCreate = uint32_t(__cdecl*)(void**, FfxApiHeader*, const void*);
using FnFfxDestroy = uint32_t(__cdecl*)(void**, const void*);
using FnFfxDispatch = uint32_t(__cdecl*)(void**, const FfxApiHeader*);

FfxApiResource make_ffx_res(ID3D12Resource* r, uint32_t w, uint32_t h, uint32_t state) noexcept {
    FfxApiResource out{};
    out.resource = r;
    out.description.type = 2u; // TEXTURE2D
    out.description.width = w;
    out.description.height = h;
    out.description.depth = 1u;
    out.description.mipCount = 1u;
    out.state = state;
    return out;
}

D3D12_RESOURCE_BARRIER make_barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

double compute_psnr_rgb(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    const size_t pixels = a.size() / 4u;
    double sse = 0.0;
    for (size_t i = 0; i < pixels; ++i) {
        for (size_t c = 0; c < 3u; ++c) {
            const double d = static_cast<double>(a[i * 4u + c]) - static_cast<double>(b[i * 4u + c]);
            sse += d * d;
        }
    }
    const double mse = sse / static_cast<double>(pixels * 3u);
    if (mse <= 1e-12) return 99.99; // Bit-exact (infinite PSNR capped for reporting)
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

double compute_region_psnr(const std::vector<uint8_t>& img, uint32_t w,
                           uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                           uint8_t exp_r, uint8_t exp_g, uint8_t exp_b) {
    double sse = 0.0;
    uint64_t count = 0;
    for (uint32_t y = y0; y < y1; ++y) {
        for (uint32_t x = x0; x < x1; ++x) {
            const size_t idx = (static_cast<size_t>(y) * w + x) * 4u;
            const double dr = static_cast<double>(img[idx + 0u]) - static_cast<double>(exp_r);
            const double dg = static_cast<double>(img[idx + 1u]) - static_cast<double>(exp_g);
            const double db = static_cast<double>(img[idx + 2u]) - static_cast<double>(exp_b);
            sse += dr * dr + dg * dg + db * db;
            count += 3u;
        }
    }
    const double mse = (count > 0) ? (sse / static_cast<double>(count)) : 1e9;
    if (mse <= 1e-12) return 99.99;
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

uint64_t fnv1a_64(const std::vector<uint8_t>& data) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint8_t b : data) {
        h ^= b;
        h *= 0x100000001B3ull;
    }
    return h;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: wsx_wave12_golden_render_test <amd_fidelityfx_upscaler_xbox.dll> <out_png>\n");
        return 2;
    }

    const fs::path ffx_dll_path(argv[1]);
    const fs::path out_png_path(argv[2]);
    std::printf("\n=== [Wave 12] Golden Render (RDNA2 MS+PS -> FSR 4.1.1 -> Debug HUD -> PNG) ===\n");
    {
        // Enable D3D12 Debug Layer
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))) && dbg) {
            dbg->EnableDebugLayer();
        }
        D3D12XBOX_CREATE_DEVICE_PARAMETERS cdp{};
        cdp.Version = 1u;
        cdp.ProcessDebugFlags = D3D12_PROCESS_DEBUG_FLAG_DEBUG_LAYER_ENABLED;
        ComPtr<ID3D12Device> dev;
        CHECK(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &cdp, IID_PPV_ARGS(&dev))) && dev,
              "Created d3d12_xs device under D3D12 Debug Layer");
        if (!dev) return 1;

        ComPtr<ID3D12InfoQueue> iq;
        dev.As(&iq);
        if (iq) iq->ClearStoredMessages();

        ComPtr<ID3D12Device2> dev2;
        CHECK(SUCCEEDED(dev.As(&dev2)) && dev2, "Acquired ID3D12Device2 for Mesh Shader PSO stream");

        ComPtr<IUnknown> xbox_abi_dev;
        CHECK(SUCCEEDED(D3D12XboxGetXboxAbiDevice(dev.Get(), IID_PPV_ARGS(&xbox_abi_dev))) && xbox_abi_dev,
              "Acquired native Xbox ABI Device proxy for FSR 4.1.1");

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        CHECK(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) && queue,
              "Created Direct CommandQueue");

        ComPtr<ID3D12RootSignature> rs;
        CHECK(SUCCEEDED(D3D12XboxCreateSm66RootSignature(dev.Get(), IID_PPV_ARGS(&rs))) && rs,
              "Created SM 6.6 Dynamic Resource Root Signature");

        // 2a. Translate & compile RDNA2 Mesh Shader (centered Scarlett Emerald triangle) + RDNA2 Pixel Shader
        const auto t_compile_start = std::chrono::steady_clock::now();
        std::vector<uint32_t> ms_words;
        // m0 = 3 verts | (1 prim << 12), s_sendmsg(MSG_GS_ALLOC_REQ = 10)
        ms_words.push_back(enc_sop1(0x03u, 124u, 0xFFu));
        ms_words.push_back(3u | (1u << 12));
        ms_words.push_back(enc_sopp(0x10u, 10));
        // Vertex 0 default: (0.0, 0.72, 0.0, 1.0)
        ms_words.push_back(enc_vop1(0x01u, 1u, 0xFFu)); ms_words.push_back(f32_bits(0.0f));
        ms_words.push_back(enc_vop1(0x01u, 2u, 0xFFu)); ms_words.push_back(f32_bits(0.72f));
        ms_words.push_back(enc_vop1(0x01u, 3u, 0xFFu)); ms_words.push_back(f32_bits(0.0f));
        ms_words.push_back(enc_vop1(0x01u, 4u, 0xFFu)); ms_words.push_back(f32_bits(1.0f));
        ms_words.push_back(enc_vop1(0x01u, 6u, 0xFFu)); ms_words.push_back(f32_bits(0.68f));
        ms_words.push_back(enc_vop1(0x01u, 7u, 0xFFu)); ms_words.push_back(f32_bits(-0.68f));
        ms_words.push_back(enc_vop1(0x01u, 8u, 0xFFu)); ms_words.push_back(f32_bits(-0.65f));
        // if (v0 == 1): X = +0.68, Y = -0.65
        ms_words.push_back(enc_vop3_w0(0xC2u, 106u)); ms_words.push_back(enc_vop3_w1(256u, 129u));
        ms_words.push_back(enc_vop2(0x01u, 1u, 257u, 6u));
        ms_words.push_back(enc_vop2(0x01u, 2u, 258u, 8u));
        // if (v0 == 2): X = -0.68, Y = -0.65
        ms_words.push_back(enc_vop3_w0(0xC2u, 106u)); ms_words.push_back(enc_vop3_w1(256u, 130u));
        ms_words.push_back(enc_vop2(0x01u, 1u, 257u, 7u));
        ms_words.push_back(enc_vop2(0x01u, 2u, 258u, 8u));
        push_exp(ms_words, 12u, 0xFu, 1u, 2u, 3u, 4u);
        ms_words.push_back(enc_vop1(0x01u, 5u, 0xFFu));
        ms_words.push_back(0u | (1u << 10) | (2u << 20));
        push_exp(ms_words, 20u, 0x1u, 5u, 0u, 0u, 0u, true);
        ms_words.push_back(enc_sopp(0x01u));

        std::vector<uint32_t> ps_words;
        // Scarlett Emerald (57/255, 211/255, 83/255, 1.0)
        ps_words.push_back(enc_vop1(0x01u, 0u, 0xFFu)); ps_words.push_back(f32_bits(57.0f / 255.0f));
        ps_words.push_back(enc_vop1(0x01u, 1u, 0xFFu)); ps_words.push_back(f32_bits(211.0f / 255.0f));
        ps_words.push_back(enc_vop1(0x01u, 2u, 0xFFu)); ps_words.push_back(f32_bits(83.0f / 255.0f));
        ps_words.push_back(enc_vop1(0x01u, 3u, 0xFFu)); ps_words.push_back(f32_bits(1.0f));
        push_exp(ps_words, 0u, 0xFu, 0u, 1u, 2u, 3u, true, true);
        ps_words.push_back(enc_sopp(0x01u));

        wsx_rdna2::HlslEmitter em_ms, em_ps;
        wsx_rdna2::TranslateStats st_ms{}, st_ps{};
        const std::string hlsl_ms = em_ms.translate_raw_rdna2(ms_words.data(), ms_words.size(), 13u, 32u, 1u, 1u, 0u, true, st_ms);
        const std::string hlsl_ps = em_ps.translate_raw_rdna2(ps_words.data(), ps_words.size(), 0u, 1u, 1u, 1u, 0u, true, st_ps);

        std::vector<uint8_t> dxil_ms, dxil_ps;
        char err[1024]{};
        const HRESULT hr_ms = wsx_sm66::compile_hlsl_to_dxil_vec(hlsl_ms.c_str(), hlsl_ms.size(), L"main", L"ms_6_6", dxil_ms, err, sizeof(err));
        const HRESULT hr_ps = wsx_sm66::compile_hlsl_to_dxil_vec(hlsl_ps.c_str(), hlsl_ps.size(), L"main", L"ps_6_6", dxil_ps, err, sizeof(err));
        const double compile_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_compile_start).count();
        CHECK(SUCCEEDED(hr_ms) && SUCCEEDED(hr_ps) && !dxil_ms.empty() && !dxil_ps.empty(),
              "Translated & DXC-compiled RDNA2 MS+PS -> signed SM 6.6 DXIL in %.2f ms", compile_ms);

        // Create Mesh+Pixel PSO
        struct alignas(void*) MeshPsoStream {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t_rs = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
            ID3D12RootSignature* rs = nullptr;
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t_ms = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS;
            D3D12_SHADER_BYTECODE ms{};
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t_ps = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;
            D3D12_SHADER_BYTECODE ps{};
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t_rast = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
            D3D12_RASTERIZER_DESC rast{};
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t_rtv = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
            D3D12_RT_FORMAT_ARRAY rtv{};
        } stream{};
        stream.rs = rs.Get();
        stream.ms = {dxil_ms.data(), dxil_ms.size()};
        stream.ps = {dxil_ps.data(), dxil_ps.size()};
        stream.rast.FillMode = D3D12_FILL_MODE_SOLID;
        stream.rast.CullMode = D3D12_CULL_MODE_NONE;
        stream.rast.DepthClipEnable = FALSE;
        stream.rtv.NumRenderTargets = 1;
        stream.rtv.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

        D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof(stream), &stream};
        ComPtr<ID3D12PipelineState> mesh_pso;
        CHECK(SUCCEEDED(dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&mesh_pso))) && mesh_pso,
              "Created live RTX 5060 RDNA2 Mesh+Pixel Pipeline State");

        // 2b. Load FSR 4.1.1 bridge & create 320x180 -> 640x360 context
        HMODULE h_ffx = LoadLibraryW(ffx_dll_path.c_str());
        CHECK(h_ffx != nullptr, "Loaded amd_fidelityfx_upscaler_xbox.dll");
        if (!h_ffx) return 1;
        auto fn_create = reinterpret_cast<FnFfxCreate>(GetProcAddress(h_ffx, "ffxCreateContext"));
        auto fn_destroy = reinterpret_cast<FnFfxDestroy>(GetProcAddress(h_ffx, "ffxDestroyContext"));
        auto fn_dispatch = reinterpret_cast<FnFfxDispatch>(GetProcAddress(h_ffx, "ffxDispatch"));
        CHECK(fn_create && fn_destroy && fn_dispatch, "Resolved FSR 4.1.1 C exports");

        constexpr uint32_t kRW = 320u, kRH = 180u;
        constexpr uint32_t kUW = 640u, kUH = 360u;

        FfxCreateBackendDX12Desc dx12_desc{{0x01000002ull, nullptr}, xbox_abi_dev.Get()};
        FfxCreateContextDescUpscale up_desc{{0x00010000ull, &dx12_desc.header}, 0x01u, {kRW, kRH}, {kUW, kUH}, nullptr};
        void* ffx_ctx = nullptr;
        CHECK(fn_create(&ffx_ctx, &up_desc.header, nullptr) == 0u && ffx_ctx != nullptr,
              "Created FSR 4.1.1 SM 6.6 context (320x180 -> 640x360) on Xbox ABI device");

        // Allocate render target (320x180), motion vector tex (320x180), reactive (320x180), upscaled output (640x360)
        D3D12_HEAP_PROPERTIES hp_def{D3D12_HEAP_TYPE_DEFAULT};
        auto mk_tex = [&](uint32_t w, uint32_t h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st, ComPtr<ID3D12Resource>& out) {
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = fmt; td.SampleDesc.Count = 1; td.Flags = fl;
            return dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td, st, nullptr, IID_PPV_ARGS(&out));
        };

        ComPtr<ID3D12Resource> tex_scene, tex_mv, tex_reactive, tex_out;
        CHECK(SUCCEEDED(mk_tex(kRW, kRH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                               D3D12_RESOURCE_STATE_RENDER_TARGET, tex_scene)), "Created 320x180 scene RT");
        CHECK(SUCCEEDED(mk_tex(kRW, kRH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                               D3D12_RESOURCE_STATE_RENDER_TARGET, tex_mv)), "Created 320x180 MV tex");
        CHECK(SUCCEEDED(mk_tex(kRW, kRH, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, tex_reactive)), "Created 320x180 reactive tex");
        CHECK(SUCCEEDED(mk_tex(kUW, kUH, DXGI_FORMAT_R8G8B8A8_UNORM,
                               D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, tex_out)), "Created 640x360 FSR+HUD backbuffer");

        // RTV + Shader-visible heaps for Mesh Shader
        D3D12_DESCRIPTOR_HEAP_DESC rhd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        ComPtr<ID3D12DescriptorHeap> rtv_heap;
        dev->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap));
        const UINT rtv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_scene = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_mv{rtv_scene.ptr + rtv_inc};
        dev->CreateRenderTargetView(tex_scene.Get(), nullptr, rtv_scene);
        dev->CreateRenderTargetView(tex_mv.Get(), nullptr, rtv_mv);

        D3D12_DESCRIPTOR_HEAP_DESC shd_cbv{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        D3D12_DESCRIPTOR_HEAP_DESC shd_smp{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        ComPtr<ID3D12DescriptorHeap> cbv_heap, smp_heap;
        dev->CreateDescriptorHeap(&shd_cbv, IID_PPV_ARGS(&cbv_heap));
        dev->CreateDescriptorHeap(&shd_smp, IID_PPV_ARGS(&smp_heap));

        // Readback buffer for 640x360 RGBA8 PSNR check
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT nrows = 0; UINT64 row_sz = 0, total_sz = 0;
        const D3D12_RESOURCE_DESC out_rd = tex_out->GetDesc();
        dev->GetCopyableFootprints(&out_rd, 0, 1, 0, &fp, &nrows, &row_sz, &total_sz);
        D3D12_HEAP_PROPERTIES hp_rb{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rbd{};
        rbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rbd.Width = total_sz; rbd.Height = 1; rbd.DepthOrArraySize = 1; rbd.MipLevels = 1;
        rbd.SampleDesc.Count = 1; rbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> rb_buf;
        dev->CreateCommittedResource(&hp_rb, D3D12_HEAP_FLAG_NONE, &rbd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb_buf));

        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList6> cl;
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl));
        cl->Close();

        void* xbox_abi_cl = nullptr;
        CHECK(SUCCEEDED(D3D12XboxWrapCommandListForXboxAbi(cl.Get(), &xbox_abi_cl)) && xbox_abi_cl,
              "Wrapped GraphicsCommandList6 in native Xbox ABI proxy");

        ComPtr<ID3D12Fence> fence;
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        HANDLE fev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        UINT64 fval = 0;

        auto render_golden_pass = [&](std::vector<uint8_t>& out_rgba, double& elapsed_ms) -> bool {
            const auto t0 = std::chrono::steady_clock::now();
            alloc->Reset();
            cl->Reset(alloc.Get(), nullptr);

            // Clear 320x180 scene to Xbox Navy (#0D1117) and MV to (0,0)
            const float navy[4] = {13.0f / 255.0f, 17.0f / 255.0f, 23.0f / 255.0f, 1.0f};
            const float zero4[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            cl->ClearRenderTargetView(rtv_scene, navy, 0, nullptr);
            cl->ClearRenderTargetView(rtv_mv, zero4, 0, nullptr);

            // Stage A: DispatchMesh(1,1,1) -> RDNA2 MS+PS centered Scarlett Emerald triangle
            ID3D12DescriptorHeap* heaps[2] = {cbv_heap.Get(), smp_heap.Get()};
            cl->SetDescriptorHeaps(2, heaps);
            cl->SetGraphicsRootSignature(rs.Get());
            cl->SetPipelineState(mesh_pso.Get());
            D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kRW), static_cast<float>(kRH), 0.0f, 1.0f};
            D3D12_RECT sc{0, 0, static_cast<LONG>(kRW), static_cast<LONG>(kRH)};
            cl->RSSetViewports(1, &vp);
            cl->RSSetScissorRects(1, &sc);
            cl->OMSetRenderTargets(1, &rtv_scene, FALSE, nullptr);
            cl->DispatchMesh(1, 1, 1);

            // Transition scene + MV -> NON_PIXEL_SHADER_RESOURCE for FSR 4.1.1
            D3D12_RESOURCE_BARRIER to_srv[2] = {
                make_barrier(tex_scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                make_barrier(tex_mv.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            };
            cl->ResourceBarrier(2, to_srv);

            // Stage B: FSR 4.1.1 320x180 -> 640x360 (reset + temporal + RCAS sharpening)
            for (int f = 0; f < 2; ++f) {
                FfxDispatchDescUpscale dd{};
                dd.header.type = 0x00010001ull;
                dd.commandList = xbox_abi_cl;
                dd.color = make_ffx_res(tex_scene.Get(), kRW, kRH, 0x004u);
                dd.motionVectors = make_ffx_res(tex_mv.Get(), kRW, kRH, 0x004u);
                dd.reactive = make_ffx_res(tex_reactive.Get(), kRW, kRH, 0x004u);
                dd.output = make_ffx_res(tex_out.Get(), kUW, kUH, 0x002u);
                dd.motionVectorScale = {static_cast<float>(kRW), static_cast<float>(kRH)};
                dd.renderSize = {kRW, kRH};
                dd.upscaleSize = {kUW, kUH};
                dd.enableSharpening = (f == 1);
                dd.sharpness = (f == 1) ? 0.75f : 0.0f;
                dd.frameTimeDelta = 16.666f;
                dd.preExposure = 1.0f;
                dd.reset = (f == 0);
                dd.cameraNear = 0.1f;
                dd.cameraFar = 1000.0f;
                dd.cameraFovAngleVertical = 1.0472f;
                dd.viewSpaceToMetersFactor = 1.0f;
                if (fn_dispatch(&ffx_ctx, &dd.header) != 0u) return false;
            }

            // Copy upscaled 640x360 output to readback buffer & restore states
            D3D12_RESOURCE_BARRIER b_cp = make_barrier(tex_out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cl->ResourceBarrier(1, &b_cp);
            D3D12_TEXTURE_COPY_LOCATION dst_loc{rb_buf.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
            dst_loc.PlacedFootprint = fp;
            D3D12_TEXTURE_COPY_LOCATION src_loc{tex_out.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
            cl->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

            D3D12_RESOURCE_BARRIER restore[3] = {
                make_barrier(tex_out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                make_barrier(tex_scene.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                make_barrier(tex_mv.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            };
            cl->ResourceBarrier(3, restore);
            cl->Close();

            ID3D12CommandList* lists[1] = {cl.Get()};
            queue->ExecuteCommandLists(1, lists);
            ++fval;
            queue->Signal(fence.Get(), fval);
            fence->SetEventOnCompletion(fval, fev);
            WaitForSingleObject(fev, 10000);
            elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

            void* mapped = nullptr;
            if (FAILED(rb_buf->Map(0, nullptr, &mapped)) || !mapped) return false;
            out_rgba.resize(static_cast<size_t>(kUW) * kUH * 4u);
            for (uint32_t y = 0; y < kUH; ++y) {
                std::memcpy(out_rgba.data() + static_cast<size_t>(y) * kUW * 4u,
                            static_cast<const uint8_t*>(mapped) + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch,
                            static_cast<size_t>(kUW) * 4u);
            }
            rb_buf->Unmap(0, nullptr);
            return true;
        };

        std::vector<uint8_t> frame_a, frame_b;
        double gpu_ms_a = 0.0, gpu_ms_b = 0.0;
        CHECK(render_golden_pass(frame_a, gpu_ms_a), "Rendered Golden Pass A (RDNA2 MS+PS -> FSR 4.1.1 640x360) in %.2f ms", gpu_ms_a);
        CHECK(render_golden_pass(frame_b, gpu_ms_b), "Rendered Golden Pass B (RDNA2 MS+PS -> FSR 4.1.1 640x360) in %.2f ms", gpu_ms_b);

        const double det_psnr = compute_psnr_rgb(frame_a, frame_b);
        const double emerald_psnr = compute_region_psnr(frame_a, kUW, 260, 150, 380, 230, 57, 211, 83);
        const double navy_psnr = compute_region_psnr(frame_a, kUW, 580, 300, 632, 352, 13, 17, 23);
        const uint64_t hash_a = fnv1a_64(frame_a);

        CHECK(det_psnr >= 60.0, "Back-to-back GPU determinism PSNR = %.2f dB (>= 60.0 dB, FNV1a=0x%016llX)",
              det_psnr, static_cast<unsigned long long>(hash_a));
        CHECK(emerald_psnr >= 45.0, "Golden Scarlett Emerald triangle interior PSNR = %.2f dB (>= 45.0 dB)", emerald_psnr);
        CHECK(navy_psnr >= 45.0, "Golden Xbox Navy background PSNR = %.2f dB (>= 45.0 dB)", navy_psnr);

        // Stage C: Composite upgraded Pure DX12 Debug HUD + live lossless PNG capture
        CHECK(SUCCEEDED(D3D12XboxSetHudEnabled(dev.Get(), TRUE)), "Enabled upgraded SM 6.6 Debug HUD");
        CHECK(SUCCEEDED(D3D12XboxRenderDebugHud(dev.Get(), queue.Get(), tex_out.Get(),
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                D3D12_RESOURCE_STATE_COMMON)),
              "Composited upgraded Debug HUD onto 640x360 FSR-upscaled backbuffer");

        std::error_code ec;
        fs::remove(out_png_path, ec);
        CHECK(SUCCEEDED(D3D12XboxCaptureBackbufferPng(dev.Get(), queue.Get(), tex_out.Get(),
                                                      D3D12_RESOURCE_STATE_COMMON,
                                                      D3D12_RESOURCE_STATE_COMMON,
                                                      out_png_path.c_str())),
              "Captured final golden backbuffer PNG -> %s", out_png_path.string().c_str());

        bool png_ok = false;
        uintmax_t png_sz = 0;
        if (fs::exists(out_png_path)) {
            png_sz = fs::file_size(out_png_path);
            std::ifstream f(out_png_path, std::ios::binary);
            std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
            if (b.size() > 64 && std::memcmp(b.data(), kSig, 8) == 0) {
                const uint32_t pw = (uint32_t(b[16]) << 24) | (uint32_t(b[17]) << 16) | (uint32_t(b[18]) << 8) | uint32_t(b[19]);
                const uint32_t ph = (uint32_t(b[20]) << 24) | (uint32_t(b[21]) << 16) | (uint32_t(b[22]) << 8) | uint32_t(b[23]);
                png_ok = (pw == kUW && ph == kUH);
            }
        }
        CHECK(png_ok, "Validated golden_wave12_render.png (640x360, %llu bytes)", static_cast<unsigned long long>(png_sz));

        // Cleanup FFX + Xbox ABI wrapper
        fn_destroy(&ffx_ctx, nullptr);
        reinterpret_cast<IUnknown*>(xbox_abi_cl)->Release();
        CloseHandle(fev);
        FreeLibrary(h_ffx);

        // Verify 0 D3D12 Debug Layer errors
        uint64_t dbg_errs = 0;
        if (iq) {
            const UINT64 n = iq->GetNumStoredMessages();
            for (UINT64 i = 0; i < n; ++i) {
                SIZE_T len = 0;
                iq->GetMessage(i, nullptr, &len);
                std::vector<uint8_t> buf(len);
                auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                if (SUCCEEDED(iq->GetMessage(i, m, &len)) &&
                    (m->Severity == D3D12_MESSAGE_SEVERITY_ERROR || m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)) {
                    ++dbg_errs;
                    std::fprintf(stderr, "  [D3D12 ERR] %s\n", m->pDescription);
                }
            }
        }
        CHECK(dbg_errs == 0u, "D3D12 Debug Layer reported 0 errors across Wave 12 Golden Pipeline");
    }

    std::printf("\n=== Wave 12 Summary: failures=%d ===\n", g_failures);
    return (g_failures == 0) ? 0 : 1;
}
