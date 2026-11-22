// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Pure Direct3D 12 (SM 6.6) Test for our clean-room amd_fidelityfx_upscaler_xbox.dll (FSR 4.1.1) bridge:
// 1. Verifies all 5 exact FidelityFX API v2.0 C exports + version 4.1.1 + quality/jitter math.
// 2. Creates a 720p -> 1080p context directly on d3d12_xs.dll's native Xbox ABI device (WsxXboxDeviceAbiProxy).
// 3. Dispatches GENERATEREACTIVEMASK (0x10006) + 3 consecutive 720p->1080p frames (reset, temporal,
//    temporal + RCAS sharpening) through a wrapped native Xbox ABI command list (WsxXboxCommandListAbiProxy).
// 4. Reads back both the generated reactive mask and the final 1920x1080 upscaled texture on real GPU
//    and asserts exact color quadrant accuracy, reactive mask response, bridge counters, and 0 Debug Layer errors.

#include "include/xbox/d3d12_xs.h"
#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

#define CHECK_HR(expr)                                                                 \
    do {                                                                               \
        HRESULT _hr = (expr);                                                          \
        if (FAILED(_hr)) {                                                             \
            std::fprintf(stderr, "FAIL: %s returned 0x%08lX at line %d\n",             \
                         #expr, static_cast<unsigned long>(_hr), __LINE__);            \
            return 1;                                                                  \
        }                                                                              \
    } while (0)

#define CHECK_COND(cond, msg)                                                          \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "FAIL: %s (%s) at line %d\n", msg, #cond, __LINE__);  \
            return 1;                                                                  \
        }                                                                              \
    } while (0)

namespace {

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
struct FfxApiEffectMemoryUsage { uint64_t totalUsageInBytes; uint64_t aliasableUsageInBytes; };
struct FfxQueryGetVersions {
    FfxApiHeader header; uint64_t createDescType; void* device;
    uint64_t* outputCount; uint64_t* versionIds; const char** versionNames;
};
struct FfxQueryUpscaleRatio { FfxApiHeader header; uint32_t qualityMode; float* pOutUpscaleRatio; };
struct FfxQueryRenderResolution {
    FfxApiHeader header; uint32_t displayWidth; uint32_t displayHeight; uint32_t qualityMode;
    uint32_t* pOutRenderWidth; uint32_t* pOutRenderHeight;
};
struct FfxQueryJitterPhaseCount {
    FfxApiHeader header; uint32_t renderWidth; uint32_t displayWidth; int32_t* pOutPhaseCount;
};
struct FfxQueryJitterOffset {
    FfxApiHeader header; int32_t index; int32_t phaseCount; float* pOutX; float* pOutY;
};
struct FfxQueryGpuMemoryUsage { FfxApiHeader header; FfxApiEffectMemoryUsage* gpuMemoryUsageUpscaler; };
struct FfxCreateContextDescUpscale {
    FfxApiHeader header; uint32_t flags; FfxApiDimensions2D maxRenderSize;
    FfxApiDimensions2D maxUpscaleSize; void* fpMessage;
};
struct FfxCreateBackendDX12Desc { FfxApiHeader header; void* device; };
struct FfxConfigureKeyValue { FfxApiHeader header; uint64_t key; uint64_t u64; void* ptr; };
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
struct FfxDispatchDescGenerateReactiveMask {
    FfxApiHeader header;
    void* commandList;
    FfxApiResource colorOpaqueOnly;
    FfxApiResource colorPreUpscale;
    FfxApiResource outReactive;
    FfxApiDimensions2D renderSize;
    float scale;
    float cutoffThreshold;
    float binaryValue;
    uint32_t flags;
};
struct WsxFfxUpscalerStats {
    uint64_t ContextsCreated, ContextsDestroyed, QueryCalls, ConfigureCalls;
    uint64_t DispatchUpscaleCalls, DispatchRcasPasses, DispatchReactiveCalls;
};

using FnCreate = uint32_t(__cdecl*)(void**, FfxApiHeader*, const void*);
using FnDestroy = uint32_t(__cdecl*)(void**, const void*);
using FnConfigure = uint32_t(__cdecl*)(void**, const FfxApiHeader*);
using FnQuery = uint32_t(__cdecl*)(void**, FfxApiHeader*);
using FnDispatch = uint32_t(__cdecl*)(void**, const FfxApiHeader*);
using FnStats = HRESULT(WINAPI*)(WsxFfxUpscalerStats*);

FfxApiResource make_ffx_res(ID3D12Resource* r, uint32_t w, uint32_t h, uint32_t state) noexcept {
    FfxApiResource out{};
    out.resource = r;
    out.description.type = 2; // TEXTURE2D
    out.description.width = w;
    out.description.height = h;
    out.description.depth = 1;
    out.description.mipCount = 1;
    out.state = state;
    return out;
}

HRESULT create_tex2d(
    ID3D12Device* dev,
    uint32_t w,
    uint32_t h,
    DXGI_FORMAT fmt,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initial_state,
    ID3D12Resource** out) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initial_state,
                                        nullptr, IID_PPV_ARGS(out));
}

HRESULT create_buf(
    ID3D12Device* dev,
    D3D12_HEAP_TYPE ht,
    uint64_t bytes,
    D3D12_RESOURCE_STATES st,
    ID3D12Resource** out) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = ht;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (bytes + 255ull) & ~255ull;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st,
                                        nullptr, IID_PPV_ARGS(out));
}

D3D12_RESOURCE_BARRIER make_barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) noexcept {
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = r;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = a;
    bar.Transition.StateAfter = b;
    return bar;
}

} // namespace

int main(int argc, char** argv) {
    const char* dll_path = (argc > 1 && argv[1]) ? argv[1] : "amd_fidelityfx_upscaler_xbox.dll";
    HMODULE mod = LoadLibraryA(dll_path);
    CHECK_COND(mod != nullptr, "Failed to load amd_fidelityfx_upscaler_xbox.dll");

    auto fn_create = reinterpret_cast<FnCreate>(GetProcAddress(mod, "ffxCreateContext"));
    auto fn_destroy = reinterpret_cast<FnDestroy>(GetProcAddress(mod, "ffxDestroyContext"));
    auto fn_configure = reinterpret_cast<FnConfigure>(GetProcAddress(mod, "ffxConfigure"));
    auto fn_query = reinterpret_cast<FnQuery>(GetProcAddress(mod, "ffxQuery"));
    auto fn_dispatch = reinterpret_cast<FnDispatch>(GetProcAddress(mod, "ffxDispatch"));
    auto fn_stats = reinterpret_cast<FnStats>(GetProcAddress(mod, "WsxFfxUpscalerGetStats"));
    CHECK_COND(fn_create && fn_destroy && fn_configure && fn_query && fn_dispatch && fn_stats,
               "Missing FidelityFX API v2.0 exports");

    // 1. Verify version, quality modes, and Halton jitter queries match FSR 4.1.1
    uint64_t ver_cnt = 1;
    uint64_t ver_id = 0;
    const char* ver_name = nullptr;
    FfxQueryGetVersions qv{};
    qv.header.type = 0x00000004ull;
    qv.createDescType = 0x00010000ull;
    qv.outputCount = &ver_cnt;
    qv.versionIds = &ver_id;
    qv.versionNames = &ver_name;
    CHECK_COND(fn_query(nullptr, &qv.header) == 0u, "GET_VERSIONS failed");
    CHECK_COND(ver_cnt == 1u && ver_id == 0xF5A5CA1E01001001ull && ver_name && std::strcmp(ver_name, "4.1.1") == 0,
               "Version mismatch");

    uint32_t rw = 0, rh = 0;
    FfxQueryRenderResolution qres{{0x00010003ull, nullptr}, 3840, 2160, 2, &rw, &rh}; // Balanced 1.7x
    CHECK_COND(fn_query(nullptr, &qres.header) == 0u && rw == 2258u && rh == 1270u, "Balanced 4K resolution mismatch");

    int32_t phases = 0;
    FfxQueryJitterPhaseCount qjp{{0x00010004ull, nullptr}, 1280, 1920, &phases};
    CHECK_COND(fn_query(nullptr, &qjp.header) == 0u && phases == 18, "720p->1080p jitter phases must be 18");

    float jx = 0.0f, jy = 0.0f;
    FfxQueryJitterOffset qjo{{0x00010005ull, nullptr}, 1, phases, &jx, &jy};
    CHECK_COND(fn_query(nullptr, &qjo.header) == 0u && std::fabs(jx + 0.25f) < 1e-4f && std::fabs(jy - 0.166667f) < 1e-4f,
               "Halton jitter sample[1] mismatch");
    std::printf("[FFX-DX12] Version=%s (0x%016llX) | 4K Balanced=%ux%u | Jitter720p->1080p phases=%d sample[1]=(%.5f, %.5f)\n",
                ver_name, static_cast<unsigned long long>(ver_id), rw, rh, phases, jx, jy);

    // 2. Enable D3D12 Debug Layer and create device via d3d12_xs.dll
    {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(dbg.GetAddressOf())))) {
            dbg->EnableDebugLayer();
        }
    }
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    params.ProcessDebugFlags = D3D12_PROCESS_DEBUG_FLAG_DEBUG_LAYER_ENABLED;
    ComPtr<ID3D12Device> dev;
    CHECK_HR(D3D12XboxCreateDevice(nullptr, &params, IID_PPV_ARGS(dev.GetAddressOf())));

    ComPtr<ID3D12InfoQueue> info_queue;
    dev.As(&info_queue);
    if (info_queue) info_queue->ClearStoredMessages();

    ComPtr<IUnknown> xbox_abi_dev;
    CHECK_HR(D3D12XboxGetXboxAbiDevice(dev.Get(), IID_PPV_ARGS(xbox_abi_dev.GetAddressOf())));

    // 3. Create 720p -> 1080p upscaler context on native Xbox ABI device
    constexpr uint32_t kRW = 1280u, kRH = 720u;
    constexpr uint32_t kUW = 1920u, kUH = 1080u;

    FfxCreateBackendDX12Desc dx12_desc{{0x01000002ull, nullptr}, xbox_abi_dev.Get()};
    FfxCreateContextDescUpscale up_desc{{0x00010000ull, &dx12_desc.header}, 0x01u, {kRW, kRH}, {kUW, kUH}, nullptr};
    void* ctx = nullptr;
    CHECK_COND(fn_create(&ctx, &up_desc.header, nullptr) == 0u && ctx != nullptr, "ffxCreateContext failed");

    FfxApiEffectMemoryUsage mem_usage{};
    FfxQueryGpuMemoryUsage qmem{{0x00010008ull, nullptr}, &mem_usage};
    CHECK_COND(fn_query(&ctx, &qmem.header) == 0u && mem_usage.totalUsageInBytes > 20ull * 1024 * 1024,
               "GPU memory usage must reflect real RGBA16F ping-pong history");
    std::printf("[FFX-DX12] Context created (720p->1080p), real VRAM=%.2f MB\n",
                static_cast<double>(mem_usage.totalUsageInBytes) / (1024.0 * 1024.0));

    FfxConfigureKeyValue kv{{0x00010007ull, nullptr}, 1u, 1u, nullptr};
    CHECK_COND(fn_configure(&ctx, &kv.header) == 0u, "ffxConfigure(KEYVALUE) failed");

    // 4. Allocate test textures (colorOpaque, colorPreUpscale, motionVectors, reactive, output1080p)
    ComPtr<ID3D12Resource> tex_opaque, tex_color, tex_mv, tex_reactive, tex_out;
    CHECK_HR(create_tex2d(dev.Get(), kRW, kRH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_COMMON, tex_opaque.GetAddressOf()));
    CHECK_HR(create_tex2d(dev.Get(), kRW, kRH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_COMMON, tex_color.GetAddressOf()));
    CHECK_HR(create_tex2d(dev.Get(), kRW, kRH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_COMMON, tex_mv.GetAddressOf()));
    CHECK_HR(create_tex2d(dev.Get(), kRW, kRH, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, tex_reactive.GetAddressOf()));
    CHECK_HR(create_tex2d(dev.Get(), kUW, kUH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, tex_out.GetAddressOf()));

    // Upload buffers for opaque (4 solid color quadrants), color (opaque + bright translucent center box), and zero MV
    auto upload_tex = [&](ID3D12Resource* dst, auto fill_fn, ComPtr<ID3D12Resource>& up_out,
                          D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp_out) -> HRESULT {
        const D3D12_RESOURCE_DESC rd = dst->GetDesc();
        UINT nrows = 0; UINT64 row_sz = 0, total_sz = 0;
        dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp_out, &nrows, &row_sz, &total_sz);
        HRESULT hr = create_buf(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, total_sz,
                                D3D12_RESOURCE_STATE_GENERIC_READ, up_out.GetAddressOf());
        if (FAILED(hr)) return hr;
        void* p = nullptr;
        hr = up_out->Map(0, nullptr, &p);
        if (FAILED(hr)) return hr;
        std::memset(p, 0, static_cast<size_t>(total_sz));
        fill_fn(static_cast<uint8_t*>(p) + fp_out.Offset, fp_out.Footprint.RowPitch);
        up_out->Unmap(0, nullptr);
        return S_OK;
    };

    ComPtr<ID3D12Resource> up_opaque, up_color, up_mv;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp_opaque{}, fp_color{}, fp_mv{};
    auto fill_quadrants = [&](uint8_t* base, UINT pitch, bool add_particle) {
        for (uint32_t y = 0; y < kRH; ++y) {
            uint8_t* row = base + static_cast<size_t>(y) * pitch;
            for (uint32_t x = 0; x < kRW; ++x) {
                uint8_t r = 0, g = 0, b = 0;
                if (x < kRW / 2 && y < kRH / 2) { r = 240; g = 10; b = 10; }       // TL: Red
                else if (x >= kRW / 2 && y < kRH / 2) { r = 10; g = 240; b = 10; } // TR: Green
                else if (x < kRW / 2 && y >= kRH / 2) { r = 10; g = 10; b = 240; } // BL: Blue
                else { const uint8_t v = ((x / 16 + y / 16) & 1) ? 230 : 40; r = g = b = v; } // BR: Checker
                if (add_particle && x >= 600 && x < 680 && y >= 320 && y < 400) {
                    r = 255; g = 255; b = 255;
                }
                row[x * 4 + 0] = r; row[x * 4 + 1] = g; row[x * 4 + 2] = b; row[x * 4 + 3] = 255;
            }
        }
    };
    CHECK_HR(upload_tex(tex_opaque.Get(), [&](uint8_t* b, UINT p) { fill_quadrants(b, p, false); }, up_opaque, fp_opaque));
    CHECK_HR(upload_tex(tex_color.Get(),  [&](uint8_t* b, UINT p) { fill_quadrants(b, p, true); },  up_color,  fp_color));
    CHECK_HR(upload_tex(tex_mv.Get(),     [&](uint8_t*, UINT) {},                                    up_mv,     fp_mv));

    // Readback buffers for reactive (720p R8) and output (1080p RGBA8)
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp_react_rb{}, fp_out_rb{};
    UINT nr1 = 0, nr2 = 0; UINT64 rs1 = 0, rs2 = 0, ts1 = 0, ts2 = 0;
    const D3D12_RESOURCE_DESC rd_react = tex_reactive->GetDesc();
    const D3D12_RESOURCE_DESC rd_out = tex_out->GetDesc();
    dev->GetCopyableFootprints(&rd_react, 0, 1, 0, &fp_react_rb, &nr1, &rs1, &ts1);
    dev->GetCopyableFootprints(&rd_out,   0, 1, 0, &fp_out_rb,   &nr2, &rs2, &ts2);
    ComPtr<ID3D12Resource> rb_react, rb_out;
    CHECK_HR(create_buf(dev.Get(), D3D12_HEAP_TYPE_READBACK, ts1, D3D12_RESOURCE_STATE_COPY_DEST, rb_react.GetAddressOf()));
    CHECK_HR(create_buf(dev.Get(), D3D12_HEAP_TYPE_READBACK, ts2, D3D12_RESOURCE_STATE_COPY_DEST, rb_out.GetAddressOf()));

    // Command queue, allocator, list & Xbox ABI wrapper
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    CHECK_HR(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(queue.GetAddressOf())));
    ComPtr<ID3D12CommandAllocator> alloc;
    CHECK_HR(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.GetAddressOf())));
    ComPtr<ID3D12GraphicsCommandList> cl;
    CHECK_HR(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(cl.GetAddressOf())));

    void* xbox_abi_cl = nullptr;
    CHECK_HR(D3D12XboxWrapCommandListForXboxAbi(cl.Get(), &xbox_abi_cl));

    auto copy_up = [&](ID3D12Resource* dst, ID3D12Resource* src, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp) {
        D3D12_TEXTURE_COPY_LOCATION dl{dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        D3D12_TEXTURE_COPY_LOCATION sl{src, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        sl.PlacedFootprint = fp;
        cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    };
    copy_up(tex_opaque.Get(), up_opaque.Get(), fp_opaque);
    copy_up(tex_color.Get(),  up_color.Get(),  fp_color);
    copy_up(tex_mv.Get(),     up_mv.Get(),     fp_mv);

    D3D12_RESOURCE_BARRIER init_bars[3] = {
        make_barrier(tex_opaque.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        make_barrier(tex_color.Get(),  D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        make_barrier(tex_mv.Get(),     D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    cl->ResourceBarrier(3, init_bars);

    // 5a. Dispatch GENERATEREACTIVEMASK (0x10006)
    FfxDispatchDescGenerateReactiveMask grm{};
    grm.header.type = 0x00010006ull;
    grm.commandList = xbox_abi_cl;
    grm.colorOpaqueOnly = make_ffx_res(tex_opaque.Get(), kRW, kRH, 0x004u); // COMPUTE_READ
    grm.colorPreUpscale = make_ffx_res(tex_color.Get(),  kRW, kRH, 0x004u);
    grm.outReactive     = make_ffx_res(tex_reactive.Get(), kRW, kRH, 0x002u); // UAV
    grm.renderSize = {kRW, kRH};
    grm.scale = 1.0f;
    grm.cutoffThreshold = 0.2f;
    grm.binaryValue = 0.9f;
    CHECK_COND(fn_dispatch(&ctx, &grm.header) == 0u, "ffxDispatch(GENERATEREACTIVEMASK) failed");

    // 5b. Dispatch 3 consecutive 720p -> 1080p upscale frames (reset -> temporal -> temporal + RCAS)
    for (int f = 0; f < 3; ++f) {
        float fx = 0.0f, fy = 0.0f;
        FfxQueryJitterOffset jq{{0x00010005ull, nullptr}, f, phases, &fx, &fy};
        fn_query(nullptr, &jq.header);

        FfxDispatchDescUpscale dd{};
        dd.header.type = 0x00010001ull;
        dd.commandList = xbox_abi_cl;
        dd.color         = make_ffx_res(tex_color.Get(),    kRW, kRH, 0x004u); // COMPUTE_READ
        dd.motionVectors = make_ffx_res(tex_mv.Get(),       kRW, kRH, 0x004u);
        dd.reactive      = make_ffx_res(tex_reactive.Get(), kRW, kRH, 0x002u); // UAV (bridge transitions to SRV & restores!)
        dd.output        = make_ffx_res(tex_out.Get(),      kUW, kUH, 0x002u); // UAV
        dd.jitterOffset = {fx, fy};
        dd.motionVectorScale = {static_cast<float>(kRW), static_cast<float>(kRH)};
        dd.renderSize = {kRW, kRH};
        dd.upscaleSize = {kUW, kUH};
        dd.enableSharpening = (f == 2);
        dd.sharpness = (f == 2) ? 0.75f : 0.0f;
        dd.frameTimeDelta = 16.666f;
        dd.preExposure = 1.0f;
        dd.reset = (f == 0);
        dd.cameraNear = 0.1f;
        dd.cameraFar = 1000.0f;
        dd.cameraFovAngleVertical = 1.0472f;
        dd.viewSpaceToMetersFactor = 1.0f;
        CHECK_COND(fn_dispatch(&ctx, &dd.header) == 0u, "ffxDispatch(UPSCALE) failed");
    }

    // Copy reactive mask & final 1080p output to readback buffers
    D3D12_RESOURCE_BARRIER rb_bars[2] = {
        make_barrier(tex_reactive.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        make_barrier(tex_out.Get(),      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
    };
    cl->ResourceBarrier(2, rb_bars);
    {
        D3D12_TEXTURE_COPY_LOCATION d1{rb_react.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        d1.PlacedFootprint = fp_react_rb;
        D3D12_TEXTURE_COPY_LOCATION s1{tex_reactive.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        cl->CopyTextureRegion(&d1, 0, 0, 0, &s1, nullptr);

        D3D12_TEXTURE_COPY_LOCATION d2{rb_out.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        d2.PlacedFootprint = fp_out_rb;
        D3D12_TEXTURE_COPY_LOCATION s2{tex_out.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        cl->CopyTextureRegion(&d2, 0, 0, 0, &s2, nullptr);
    }
    CHECK_HR(cl->Close());

    ID3D12CommandList* lists[1] = {cl.Get()};
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    CHECK_HR(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK_COND(ev != nullptr, "CreateEventW failed");
    CHECK_HR(queue->Signal(fence.Get(), 1));
    CHECK_HR(fence->SetEventOnCompletion(1, ev));
    CHECK_COND(WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0, "GPU fence wait timed out");
    CloseHandle(ev);
    CHECK_HR(dev->GetDeviceRemovedReason());

    // 6. Verify reactive mask readback (opaque pixel = 0, translucent particle center > 200)
    {
        void* p = nullptr;
        CHECK_HR(rb_react->Map(0, nullptr, &p));
        const auto* base = static_cast<const uint8_t*>(p) + fp_react_rb.Offset;
        const uint8_t opaque_r = base[100 * fp_react_rb.Footprint.RowPitch + 100];
        const uint8_t particle_r = base[360 * fp_react_rb.Footprint.RowPitch + 640];
        rb_react->Unmap(0, nullptr);
        std::printf("[FFX-DX12] Reactive mask verified: opaque=%u, translucent_patch=%u\n", opaque_r, particle_r);
        CHECK_COND(opaque_r == 0u, "Opaque region reactive mask must be 0");
        CHECK_COND(particle_r >= 200u, "Translucent patch reactive mask must be >= 200");
    }

    // 7. Verify 1920x1080 upscaled + RCAS output pixels across all 4 quadrants + particle center
    {
        void* p = nullptr;
        CHECK_HR(rb_out->Map(0, nullptr, &p));
        const auto* base = static_cast<const uint8_t*>(p) + fp_out_rb.Offset;
        auto sample_px = [&](uint32_t x, uint32_t y) {
            return base + static_cast<size_t>(y) * fp_out_rb.Footprint.RowPitch + x * 4u;
        };
        const uint8_t* tl = sample_px(400, 200);  // Red quadrant
        const uint8_t* tr = sample_px(1500, 200); // Green quadrant
        const uint8_t* bl = sample_px(400, 850);  // Blue quadrant
        const uint8_t* ctr = sample_px(960, 540); // White particle center
        std::printf("[FFX-DX12] 1080p Output readback: TL=(%u,%u,%u) TR=(%u,%u,%u) BL=(%u,%u,%u) CTR=(%u,%u,%u)\n",
                    tl[0], tl[1], tl[2], tr[0], tr[1], tr[2], bl[0], bl[1], bl[2], ctr[0], ctr[1], ctr[2]);
        CHECK_COND(tl[0] > 220 && tl[1] < 30 && tl[2] < 30, "TL red quadrant mismatch");
        CHECK_COND(tr[1] > 220 && tr[0] < 30 && tr[2] < 30, "TR green quadrant mismatch");
        CHECK_COND(bl[2] > 220 && bl[0] < 30 && bl[1] < 30, "BL blue quadrant mismatch");
        CHECK_COND(ctr[0] > 230 && ctr[1] > 230 && ctr[2] > 230, "Center white patch mismatch");
        rb_out->Unmap(0, nullptr);
    }

    // 8. Verify stats, clean destroy, and 0 D3D12 Debug Layer errors
    WsxFfxUpscalerStats st{};
    CHECK_HR(fn_stats(&st));
    std::printf("[FFX-DX12] Bridge stats: ctx=%llu/%llu queries=%llu cfg=%llu upscale=%llu rcas=%llu reactive=%llu\n",
                static_cast<unsigned long long>(st.ContextsCreated),
                static_cast<unsigned long long>(st.ContextsDestroyed),
                static_cast<unsigned long long>(st.QueryCalls),
                static_cast<unsigned long long>(st.ConfigureCalls),
                static_cast<unsigned long long>(st.DispatchUpscaleCalls),
                static_cast<unsigned long long>(st.DispatchRcasPasses),
                static_cast<unsigned long long>(st.DispatchReactiveCalls));
    CHECK_COND(st.ContextsCreated == 1 && st.DispatchUpscaleCalls == 3 &&
               st.DispatchRcasPasses == 1 && st.DispatchReactiveCalls == 1,
               "Bridge dispatch counters mismatch");

    CHECK_COND(fn_destroy(&ctx, nullptr) == 0u && ctx == nullptr, "ffxDestroyContext failed");
    reinterpret_cast<IUnknown*>(xbox_abi_cl)->Release();

    if (info_queue) {
        const UINT64 n = info_queue->GetNumStoredMessages();
        uint32_t errs = 0;
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T len = 0;
            info_queue->GetMessage(i, nullptr, &len);
            if (!len) continue;
            std::vector<uint8_t> buf(len);
            auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(info_queue->GetMessage(i, msg, &len))) {
                if (msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
                    msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR) {
                    std::fprintf(stderr, "D3D12 DEBUG ERROR: %s\n", msg->pDescription);
                    ++errs;
                }
            }
        }
        CHECK_COND(errs == 0u, "D3D12 Debug Layer reported errors");
        std::printf("[FFX-DX12] D3D12 Debug Layer verified: 0 errors (%llu total messages)\n",
                    static_cast<unsigned long long>(n));
    }

    FreeLibrary(mod);
    std::printf("PASS: ffx_upscaler_dx12\n");
    return 0;
}
