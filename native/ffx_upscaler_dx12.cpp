// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Native Direct3D 12 (SM 6.6) Bridge for amd_fidelityfx_upscaler_xbox.dll (FSR 4.1.1):
// - Implements all 5 exact FidelityFX API v2.0 C exports (ffxCreateContext, ffxDestroyContext,
//   ffxConfigure, ffxQuery, ffxDispatch) matching amd_fidelityfx_loader_xbox.dll's ABI.
// - Reports version 4.1.1 (0xF5A5CA1E01001001) and exact FSR quality ratios, render resolutions,
//   Halton jitter phase counts, Halton jitter offsets, and real allocated VRAM usage.
// - Transparently unwraps d3d12_xs.dll's native Xbox ABI device (WsxXboxDeviceAbiProxy) and
//   command list (WsxXboxCommandListAbiProxy / WsxD3D12CommandListProxy) to desktop D3D12.
// - Replaces the 29 Xbox RDNA2 ACCC/FSPX shaders with clean-room SM 6.6 HLSL compute passes
//   compiled at runtime via D3D12XboxCompileHlslToDxil:
//     1. Pass 1: Jitter-compensated 4x4 Lanczos-2 spatial reconstruction + motion-vector
//        temporal reprojection + 3x3 YCoCg variance clipping + reactive mask blending.
//     2. Pass 2: 5-tap Robust Contrast Adaptive Sharpening (RCAS) when enableSharpening != 0.
//     3. Reactive Mask Generator (FFX_API_DISPATCH_DESC_TYPE_UPSCALE_GENERATEREACTIVEMASK).

#include "d3d12_xs_proxies.h"
#include "include/xbox/d3d12_xs.h"
#include "wsx_config.h"

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// ============================================================================
// FidelityFX API v2.0 Binary-Compatible ABI Types (verified against FSR 4.1.1)
// ============================================================================

constexpr uint32_t FFX_API_RETURN_OK = 0u;
constexpr uint32_t FFX_API_RETURN_ERROR = 1u;
constexpr uint32_t FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE = 2u;
constexpr uint32_t FFX_API_RETURN_ERROR_RUNTIME_ERROR = 3u;
constexpr uint32_t FFX_API_RETURN_NO_PROVIDER = 4u;
constexpr uint32_t FFX_API_RETURN_ERROR_MEMORY = 5u;
constexpr uint32_t FFX_API_RETURN_ERROR_PARAMETER = 6u;

constexpr uint64_t FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1 = 0x00000001ull;
constexpr uint64_t FFX_API_QUERY_DESC_TYPE_GET_VERSIONS = 0x00000004ull;
constexpr uint64_t FFX_API_DESC_TYPE_OVERRIDE_VERSION = 0x00000005ull;
constexpr uint64_t FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION = 0x00000006ull;

constexpr uint64_t FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE = 0x00010000ull;
constexpr uint64_t FFX_API_DISPATCH_DESC_TYPE_UPSCALE = 0x00010001ull;
constexpr uint64_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE = 0x00010002ull;
constexpr uint64_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE = 0x00010003ull;
constexpr uint64_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT = 0x00010004ull;
constexpr uint64_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET = 0x00010005ull;
constexpr uint64_t FFX_API_DISPATCH_DESC_TYPE_UPSCALE_GENERATEREACTIVEMASK = 0x00010006ull;
constexpr uint64_t FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE = 0x00010007ull;
constexpr uint64_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE = 0x00010008ull;
constexpr uint64_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE_V2 = 0x00010009ull;
constexpr uint64_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_DEFAULT_PRESET = 0x0001000Aull;
constexpr uint64_t FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION = 0x0001000Bull;

constexpr uint64_t FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 = 0x01000002ull;

constexpr uint64_t kFsr411VersionId = 0xF5A5CA1E01001001ull;
constexpr const char* kFsr411VersionName = "4.1.1";

// Create flags
constexpr uint32_t FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE = (1u << 0);
constexpr uint32_t FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS = (1u << 1);
constexpr uint32_t FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION = (1u << 2);
constexpr uint32_t FFX_UPSCALE_ENABLE_DEPTH_INVERTED = (1u << 3);
constexpr uint32_t FFX_UPSCALE_ENABLE_DEPTH_INFINITE = (1u << 4);
constexpr uint32_t FFX_UPSCALE_ENABLE_AUTO_EXPOSURE = (1u << 5);

struct FfxApiHeader {
    uint64_t type;
    FfxApiHeader* pNext;
};

struct FfxApiDimensions2D {
    uint32_t width;
    uint32_t height;
};

struct FfxApiFloatCoords2D {
    float x;
    float y;
};

struct FfxApiResourceDescription {
    uint32_t type;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t mipCount;
    uint32_t flags;
    uint32_t usage;
};

struct FfxApiResource {
    void* resource;
    FfxApiResourceDescription description;
    uint32_t state;
    uint32_t _pad;
};
static_assert(sizeof(FfxApiResource) == 0x30, "FfxApiResource must be 48 bytes");

struct FfxApiEffectMemoryUsage {
    uint64_t totalUsageInBytes;
    uint64_t aliasableUsageInBytes;
};

using FfxApiMessage = void (*)(uint32_t type, const wchar_t* message);

struct FfxQueryGetVersions {
    FfxApiHeader header;
    uint64_t createDescType;
    void* device;
    uint64_t* outputCount;
    uint64_t* versionIds;
    const char** versionNames;
};

struct FfxQueryProviderVersion {
    FfxApiHeader header;
    uint64_t versionId;
    const char* versionName;
};

struct FfxQueryUpscaleRatio {
    FfxApiHeader header;
    uint32_t qualityMode;
    float* pOutUpscaleRatio;
};

struct FfxQueryRenderResolution {
    FfxApiHeader header;
    uint32_t displayWidth;
    uint32_t displayHeight;
    uint32_t qualityMode;
    uint32_t* pOutRenderWidth;
    uint32_t* pOutRenderHeight;
};

struct FfxQueryJitterPhaseCount {
    FfxApiHeader header;
    uint32_t renderWidth;
    uint32_t displayWidth;
    int32_t* pOutPhaseCount;
};

struct FfxQueryJitterOffset {
    FfxApiHeader header;
    int32_t index;
    int32_t phaseCount;
    float* pOutX;
    float* pOutY;
};

struct FfxQueryGpuMemoryUsage {
    FfxApiHeader header;
    FfxApiEffectMemoryUsage* gpuMemoryUsageUpscaler;
};

struct FfxQueryGpuMemoryUsageV2 {
    FfxApiHeader header;
    void* device;
    FfxApiDimensions2D maxRenderSize;
    FfxApiDimensions2D maxUpscaleSize;
    uint32_t flags;
    uint32_t _pad;
    FfxApiEffectMemoryUsage* gpuMemoryUsageUpscaler;
};

struct FfxCreateContextDescUpscale {
    FfxApiHeader header;
    uint32_t flags;
    FfxApiDimensions2D maxRenderSize;
    FfxApiDimensions2D maxUpscaleSize;
    FfxApiMessage fpMessage;
};

struct FfxCreateBackendDX12Desc {
    FfxApiHeader header;
    void* device;
};

struct FfxConfigureGlobalDebug1 {
    FfxApiHeader header;
    FfxApiMessage fpMessage;
    uint32_t debugLevel;
};

struct FfxDispatchDescUpscale {
    FfxApiHeader header;                       // +0x000
    void* commandList;                         // +0x010
    FfxApiResource color;                      // +0x018
    FfxApiResource depth;                      // +0x048
    FfxApiResource motionVectors;              // +0x078
    FfxApiResource exposure;                   // +0x0A8
    FfxApiResource reactive;                   // +0x0D8
    FfxApiResource transparencyAndComposition; // +0x108
    FfxApiResource output;                     // +0x138
    FfxApiFloatCoords2D jitterOffset;          // +0x168
    FfxApiFloatCoords2D motionVectorScale;     // +0x170
    FfxApiDimensions2D renderSize;             // +0x178
    FfxApiDimensions2D upscaleSize;            // +0x180
    bool enableSharpening;                     // +0x188
    float sharpness;                           // +0x18C
    float frameTimeDelta;                      // +0x190
    float preExposure;                         // +0x194
    bool reset;                                // +0x198
    float cameraNear;                          // +0x19C
    float cameraFar;                           // +0x1A0
    float cameraFovAngleVertical;              // +0x1A4
    float viewSpaceToMetersFactor;             // +0x1A8
    uint32_t flags;                            // +0x1AC
};
static_assert(offsetof(FfxDispatchDescUpscale, commandList) == 0x010);
static_assert(offsetof(FfxDispatchDescUpscale, color) == 0x018);
static_assert(offsetof(FfxDispatchDescUpscale, depth) == 0x048);
static_assert(offsetof(FfxDispatchDescUpscale, motionVectors) == 0x078);
static_assert(offsetof(FfxDispatchDescUpscale, output) == 0x138);
static_assert(offsetof(FfxDispatchDescUpscale, renderSize) == 0x178);
static_assert(offsetof(FfxDispatchDescUpscale, upscaleSize) == 0x180);
static_assert(offsetof(FfxDispatchDescUpscale, sharpness) == 0x18C);
static_assert(offsetof(FfxDispatchDescUpscale, preExposure) == 0x194);
static_assert(offsetof(FfxDispatchDescUpscale, cameraFovAngleVertical) == 0x1A4);

struct FfxDispatchDescGenerateReactiveMask {
    FfxApiHeader header;            // +0x000
    void* commandList;              // +0x010
    FfxApiResource colorOpaqueOnly; // +0x018
    FfxApiResource colorPreUpscale; // +0x048
    FfxApiResource outReactive;     // +0x078
    FfxApiDimensions2D renderSize;  // +0x0A8
    float scale;                    // +0x0B0
    float cutoffThreshold;          // +0x0B4
    float binaryValue;              // +0x0B8
    uint32_t flags;                 // +0x0BC
};

struct WsxFfxUpscalerStats {
    uint64_t ContextsCreated;
    uint64_t ContextsDestroyed;
    uint64_t QueryCalls;
    uint64_t ConfigureCalls;
    uint64_t DispatchUpscaleCalls;
    uint64_t DispatchRcasPasses;
    uint64_t DispatchReactiveCalls;
};

struct GlobalFfxCounters {
    std::atomic<uint64_t> contexts_created{0};
    std::atomic<uint64_t> contexts_destroyed{0};
    std::atomic<uint64_t> query_calls{0};
    std::atomic<uint64_t> configure_calls{0};
    std::atomic<uint64_t> dispatch_upscale{0};
    std::atomic<uint64_t> dispatch_rcas{0};
    std::atomic<uint64_t> dispatch_reactive{0};
};

GlobalFfxCounters& g_stats() noexcept {
    static GlobalFfxCounters c;
    return c;
}

// ============================================================================
// Pure Direct3D 12 SM 6.6 Compute Shaders (Lanczos-2 Temporal + RCAS + Reactive)
// ============================================================================

constexpr const char kTemporalUpscaleHlsl[] = R"HLSL(
cbuffer UpscaleParams : register(b0) {
    uint  render_w;
    uint  render_h;
    uint  upscale_w;
    uint  upscale_h;
    float jitter_x;
    float jitter_y;
    float mv_scale_x;
    float mv_scale_y;
    float pre_exposure;
    float sharpness;
    uint  reset_flag;
    uint  flags; // bit0=has_mv, bit1=has_reactive, bit2=mv_display_res, bit3=hdr, bit4=rcas_follows, bit5=has_depth, bit6=depth_inverted, bit7=has_transp
    uint  frame_idx;
    uint  pad0;
    uint  pad1;
    uint  pad2;
};

Texture2D<float4>   t_color        : register(t0);
Texture2D<float4>   t_history_prev : register(t1);
Texture2D<float2>   t_motion       : register(t2);
Texture2D<float4>   t_reactive     : register(t3);
Texture2D<float>    t_depth        : register(t4);
Texture2D<float4>   t_transparency : register(t5);
RWTexture2D<float4> u_history_curr : register(u0);
RWTexture2D<float4> u_final_out    : register(u1);
SamplerState        s_linear       : register(s0);

float3 tonemap_hdr(float3 c, float exp_scale, bool is_hdr) {
    if (!is_hdr) return c;
    float3 sc = c * exp_scale;
    float lum = max(dot(sc, float3(0.2126, 0.7152, 0.0722)), 0.0);
    return sc / (1.0 + lum);
}

float3 untonemap_hdr(float3 c, float exp_scale, bool is_hdr) {
    if (!is_hdr) return c;
    float lum = clamp(dot(c, float3(0.2126, 0.7152, 0.0722)), 0.0, 0.999);
    float3 sc = c / max(1.0 - lum, 1e-3);
    return sc / max(exp_scale, 1e-4);
}

float3 rgb_to_ycocg(float3 c) {
    float co = c.r - c.b;
    float t  = c.b + co * 0.5;
    float cg = c.g - t;
    float y  = t + cg * 0.5;
    return float3(y, co, cg);
}

float3 ycocg_to_rgb(float3 ycc) {
    float t = ycc.x - ycc.z * 0.5;
    float g = ycc.z + t;
    float b = t - ycc.y * 0.5;
    float r = b + ycc.y;
    return float3(r, g, b);
}

float lanczos2(float x) {
    float ax = abs(x);
    if (ax < 1e-5) return 1.0;
    if (ax >= 2.0) return 0.0;
    float px = 3.14159265 * ax;
    return (sin(px) / px) * (sin(px * 0.5) / (px * 0.5));
}

float3 load_color_clamped(int2 p) {
    int2 c = clamp(p, int2(0, 0), int2(int(render_w) - 1, int(render_h) - 1));
    float3 rgb = t_color.Load(int3(c, 0)).rgb;
    return max(rgb, float3(0.0, 0.0, 0.0));
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= upscale_w || dtid.y >= upscale_h) return;

    float2 dst_size = float2(float(upscale_w), float(upscale_h));
    float2 src_size = float2(float(render_w), float(render_h));
    float2 uv = (float2(dtid.xy) + 0.5) / dst_size;

    // Jitter-compensated render-space center coordinate
    float2 src_pos = uv * src_size - float2(jitter_x, jitter_y) - 0.5;
    int2 base_p = int2(floor(src_pos));
    float2 f = src_pos - float2(base_p);

    bool is_hdr = ((flags & 8u) != 0u);
    float exp_scale = max(pre_exposure, 1e-4);

    // 4x4 Lanczos-2 spatial reconstruction in HDR tone-space with 2x2 anti-ringing clamp
    float3 sum_rgb = float3(0.0, 0.0, 0.0);
    float sum_w = 0.0;
    float3 ring_min = float3(1e30, 1e30, 1e30);
    float3 ring_max = float3(-1e30, -1e30, -1e30);

    [unroll]
    for (int dy = -1; dy <= 2; ++dy) {
        float wy = lanczos2(float(dy) - f.y);
        [unroll]
        for (int dx = -1; dx <= 2; ++dx) {
            float wx = lanczos2(float(dx) - f.x);
            float w = wx * wy;
            float3 s = tonemap_hdr(load_color_clamped(base_p + int2(dx, dy)), exp_scale, is_hdr);
            sum_rgb += s * w;
            sum_w += w;
            if (dx >= 0 && dx <= 1 && dy >= 0 && dy <= 1) {
                ring_min = min(ring_min, s);
                ring_max = max(ring_max, s);
            }
        }
    }
    float3 spatial = (sum_w > 1e-5) ? (sum_rgb / sum_w) : tonemap_hdr(load_color_clamped(base_p), exp_scale, is_hdr);
    spatial = clamp(spatial, ring_min, ring_max);

    float3 result_tm = spatial;

    // Temporal accumulation with 3x3 depth-dilated motion vectors + YCoCg variance clipping
    if (reset_flag == 0u && frame_idx > 0u) {
        int2 center_p = int2(clamp(floor(uv * src_size), float2(0.0, 0.0), src_size - 1.0));
        int2 src_max = int2(int(render_w) - 1, int(render_h) - 1);
        float3 m1 = float3(0.0, 0.0, 0.0);
        float3 m2 = float3(0.0, 0.0, 0.0);
        [unroll]
        for (int ny = -1; ny <= 1; ++ny) {
            [unroll]
            for (int nx = -1; nx <= 1; ++nx) {
                float3 tm_c = tonemap_hdr(load_color_clamped(center_p + int2(nx, ny)), exp_scale, is_hdr);
                float3 ycc = rgb_to_ycocg(tm_c);
                m1 += ycc;
                m2 += ycc * ycc;
            }
        }
        float3 mean = m1 / 9.0;
        float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, float3(0.0, 0.0, 0.0)));
        float3 box_min = mean - 1.25 * sigma;
        float3 box_max = mean + 1.25 * sigma;

        int2 dilated_p = center_p;
        if ((flags & 32u) != 0u) {
            bool inv_depth = ((flags & 64u) != 0u);
            float best_d = inv_depth ? -1.0 : 2.0;
            [unroll]
            for (int dy = -1; dy <= 1; ++dy) {
                [unroll]
                for (int dx = -1; dx <= 1; ++dx) {
                    int2 sp = clamp(center_p + int2(dx, dy), int2(0, 0), src_max);
                    float d = t_depth.Load(int3(sp, 0));
                    if (inv_depth ? (d > best_d) : (d < best_d)) {
                        best_d = d;
                        dilated_p = sp;
                    }
                }
            }
        }

        float2 mv_uv = float2(0.0, 0.0);
        if ((flags & 1u) != 0u) {
            int2 mv_p = ((flags & 4u) != 0u) ? int2(dtid.xy) : dilated_p;
            float2 mv_raw = t_motion.Load(int3(mv_p, 0)).xy;
            float2 mv_pix = mv_raw * float2(mv_scale_x, mv_scale_y);
            mv_uv = mv_pix / (((flags & 4u) != 0u) ? dst_size : src_size);
        }

        float2 prev_uv = uv + mv_uv;
        if (all(prev_uv >= float2(0.0, 0.0)) && all(prev_uv <= float2(1.0, 1.0))) {
            float3 hist_raw = max(t_history_prev.SampleLevel(s_linear, prev_uv, 0.0).rgb, float3(0.0, 0.0, 0.0));
            float3 hist_tm = tonemap_hdr(hist_raw, exp_scale, is_hdr);
            float3 hist_ycc = clamp(rgb_to_ycocg(hist_tm), box_min, box_max);
            float3 hist_clamped = max(ycocg_to_rgb(hist_ycc), float3(0.0, 0.0, 0.0));

            float reactive = 0.0;
            if ((flags & 2u) != 0u) {
                reactive = saturate(t_reactive.Load(int3(center_p, 0)).r);
            }
            if ((flags & 128u) != 0u) {
                float transp = saturate(t_transparency.Load(int3(center_p, 0)).r);
                reactive = max(reactive, transp);
            }
            float alpha = clamp(max(0.125, reactive), 0.125, 1.0);
            result_tm = lerp(hist_clamped, spatial, alpha);
        }
    }

    float3 result = untonemap_hdr(result_tm, exp_scale, is_hdr);
    u_history_curr[dtid.xy] = float4(result, 1.0);
    if ((flags & 16u) == 0u) {
        u_final_out[dtid.xy] = float4(result, 1.0);
    }
}
)HLSL";

constexpr const char kRcasHlsl[] = R"HLSL(
cbuffer RcasParams : register(b0) {
    uint  upscale_w;
    uint  upscale_h;
    float sharpness;
    uint  pad0;
};

Texture2D<float4>   t_input  : register(t0);
RWTexture2D<float4> u_output : register(u0);

float luma(float3 c) {
    return dot(c, float3(0.299, 0.587, 0.114));
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= upscale_w || dtid.y >= upscale_h) return;

    int2 p = int2(dtid.xy);
    int2 mx = int2(int(upscale_w) - 1, int(upscale_h) - 1);

    float3 e = t_input.Load(int3(p, 0)).rgb;
    float3 b = t_input.Load(int3(clamp(p + int2( 0, -1), int2(0, 0), mx), 0)).rgb;
    float3 d = t_input.Load(int3(clamp(p + int2(-1,  0), int2(0, 0), mx), 0)).rgb;
    float3 f = t_input.Load(int3(clamp(p + int2( 1,  0), int2(0, 0), mx), 0)).rgb;
    float3 h = t_input.Load(int3(clamp(p + int2( 0,  1), int2(0, 0), mx), 0)).rgb;

    float lb = luma(b), ld = luma(d), le = luma(e), lf = luma(f), lh = luma(h);
    float mn = min(min(lb, ld), min(lf, lh));
    float mx_l = max(max(lb, ld), max(lf, lh));

    float peak = lerp(0.0, 0.1875, saturate(sharpness));
    float peak_l = max(1.0, max(mx_l, le));
    float hit_min = min(mn, le) / max(4.0 * mx_l, 1e-5);
    float hit_max = (peak_l - max(mx_l, le)) / min(4.0 * mn - 4.0 * peak_l, -1e-5);
    float lobe = max(-hit_min, hit_max);
    float w = clamp(lobe, -peak, 0.0);

    float3 sharp = (e + w * (b + d + f + h)) / (1.0 + 4.0 * w);
    float3 cmin = min(e, min(min(b, d), min(f, h)));
    float3 cmax = max(e, max(max(b, d), max(f, h)));
    sharp = clamp(sharp, cmin, cmax);

    u_output[dtid.xy] = float4(max(sharp, float3(0.0, 0.0, 0.0)), 1.0);
}
)HLSL";

constexpr const char kGenReactiveHlsl[] = R"HLSL(
cbuffer ReactiveParams : register(b0) {
    uint  render_w;
    uint  render_h;
    float scale;
    float cutoff_threshold;
    float binary_value;
    uint  flags;
    uint  pad0;
    uint  pad1;
};

Texture2D<float4>   t_opaque     : register(t0);
Texture2D<float4>   t_preupscale : register(t1);
RWTexture2D<float>  u_reactive   : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= render_w || dtid.y >= render_h) return;

    float3 op = t_opaque.Load(int3(dtid.xy, 0)).rgb;
    float3 pre = t_preupscale.Load(int3(dtid.xy, 0)).rgb;
    float3 diff = abs(pre - op);
    float val = max(diff.r, max(diff.g, diff.b)) * scale;
    val = (val > cutoff_threshold) ? binary_value : val;
    u_reactive[dtid.xy] = saturate(val);
}
)HLSL";

// ============================================================================
// Helpers: Unwrap Xbox ABI proxies, format/state mapping, Halton math
// ============================================================================

ComPtr<ID3D12Device> unwrap_device(void* raw_dev) noexcept {
    if (!raw_dev) return nullptr;
    auto* unk = reinterpret_cast<IUnknown*>(raw_dev);
    IUnknown* base = unk;
    ID3D12Device* xbox_unwrapped = nullptr;
    if (SUCCEEDED(unk->QueryInterface(__uuidof(wsx_d3d12::IWsxXboxDeviceAbiMarker),
                                      reinterpret_cast<void**>(&xbox_unwrapped))) &&
        xbox_unwrapped) {
        base = xbox_unwrapped; // Non-AddRef internal unwrap marker
    }
    ComPtr<ID3D12Device> dev;
    base->QueryInterface(IID_PPV_ARGS(dev.GetAddressOf()));
    return dev;
}

ComPtr<ID3D12GraphicsCommandList> unwrap_cmdlist(void* raw_cl) noexcept {
    if (!raw_cl) return nullptr;
    auto* unk = reinterpret_cast<IUnknown*>(raw_cl);
    IUnknown* base = unk;
    ID3D12GraphicsCommandList* xbox_unwrapped = nullptr;
    if (SUCCEEDED(unk->QueryInterface(__uuidof(wsx_d3d12::IWsxXboxCommandListAbiMarker),
                                      reinterpret_cast<void**>(&xbox_unwrapped))) &&
        xbox_unwrapped) {
        base = xbox_unwrapped;
    }
    ID3D12GraphicsCommandList* raw = nullptr;
    if (SUCCEEDED(base->QueryInterface(__uuidof(wsx_d3d12::IWsxD3D12CommandListProxyMarker),
                                       reinterpret_cast<void**>(&raw))) &&
        raw) {
        base = raw;
    }
    ComPtr<ID3D12GraphicsCommandList> cl;
    base->QueryInterface(IID_PPV_ARGS(cl.GetAddressOf()));
    return cl;
}

D3D12_RESOURCE_STATES ffx_state_to_d3d12(uint32_t s) noexcept {
    switch (s) {
    case 0x000u:
    case 0x001u: return D3D12_RESOURCE_STATE_COMMON;
    case 0x002u: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case 0x004u: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case 0x008u: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case 0x00Cu: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case 0x010u: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case 0x014u: return D3D12_RESOURCE_STATE_GENERIC_READ;
    case 0x020u: return D3D12_RESOURCE_STATE_COPY_DEST;
    case 0x040u: return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    case 0x080u: return D3D12_RESOURCE_STATE_PRESENT;
    case 0x100u: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case 0x200u: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    default:     return D3D12_RESOURCE_STATE_COMMON;
    }
}

DXGI_FORMAT resolve_view_format(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:       return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS:          return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:          return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R8_TYPELESS:           return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_D32_FLOAT:             return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:     return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:             return DXGI_FORMAT_R16_UNORM;
    default: return fmt;
    }
}

float quality_mode_ratio(uint32_t mode) noexcept {
    switch (mode) {
    case 0u: return 1.0f; // NativeAA
    case 1u: return 1.5f; // Quality
    case 2u: return 1.7f; // Balanced
    case 3u: return 2.0f; // Performance
    case 4u: return 3.0f; // Ultra Performance
    default: return 0.0f;
    }
}

float halton(int32_t index, int32_t base) noexcept {
    float f = 1.0f;
    float r = 0.0f;
    int32_t cur = index;
    while (cur > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(cur % base);
        cur /= base;
    }
    return r;
}

// ============================================================================
// Cached compiled SM 6.6 DXIL shaders (compiled once per process via d3d12_xs)
// ============================================================================

struct CompiledShaders {
    std::mutex mtx;
    bool attempted = false;
    bool ok = false;
    std::vector<uint8_t> temporal_dxil;
    std::vector<uint8_t> rcas_dxil;
    std::vector<uint8_t> reactive_dxil;
};

CompiledShaders& g_shaders() noexcept {
    static CompiledShaders s;
    return s;
}

bool compile_one_cs(const char* src, std::vector<uint8_t>& out) noexcept {
    size_t sz = 0;
    char err[1024]{};
    const size_t len = std::strlen(src);
    HRESULT hr = D3D12XboxCompileHlslToDxil(src, len, L"main", L"cs_6_6", nullptr, 0, &sz, err, sizeof(err));
    if (FAILED(hr) || sz < 64u) {
        wsx::trace_hr("FfxUpscaler", "CompileCS(size)", hr, "err=%.200s", err);
        return false;
    }
    out.resize(sz);
    hr = D3D12XboxCompileHlslToDxil(src, len, L"main", L"cs_6_6", out.data(), out.size(), &sz, err, sizeof(err));
    if (FAILED(hr)) {
        out.clear();
        return false;
    }
    out.resize(sz);
    return true;
}

bool ensure_shaders_compiled() noexcept {
    auto& s = g_shaders();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (s.attempted) return s.ok;
    s.attempted = true;
    s.ok = compile_one_cs(kTemporalUpscaleHlsl, s.temporal_dxil) &&
           compile_one_cs(kRcasHlsl, s.rcas_dxil) &&
           compile_one_cs(kGenReactiveHlsl, s.reactive_dxil);
    return s.ok;
}

// ============================================================================
// Upscaler Context State
// ============================================================================

constexpr uint32_t kDescriptorRingSlots = 64u;
constexpr uint32_t kDescriptorsPerSlot = 10u; // 6 SRVs + 2 UAVs + 2 spare (RCAS/Reactive)

struct FfxUpscalerContext {
    uint64_t magic = 0x465352345F445831ull; // "1XD_4RSF"
    ComPtr<ID3D12Device> dev;
    uint32_t create_flags = 0;
    FfxApiDimensions2D max_render{1280, 720};
    FfxApiDimensions2D max_upscale{1920, 1080};
    FfxApiMessage log_cb = nullptr;
    uint32_t debug_level = 0;

    // Pipelines
    ComPtr<ID3D12RootSignature> rs_temporal;
    ComPtr<ID3D12RootSignature> rs_rcas;
    ComPtr<ID3D12RootSignature> rs_reactive;
    ComPtr<ID3D12PipelineState> pso_temporal;
    ComPtr<ID3D12PipelineState> pso_rcas;
    ComPtr<ID3D12PipelineState> pso_reactive;

    // Shader-visible CBV_SRV_UAV descriptor ring
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    UINT srv_inc = 0;

    // Internal GPU textures
    ComPtr<ID3D12Resource> history[2];
    D3D12_RESOURCE_STATES history_state[2] = {
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_COMMON,
    };
    ComPtr<ID3D12Resource> dummy_zero_tex; // 1x1 R16G16B16A16_FLOAT for optional null inputs
    D3D12_RESOURCE_STATES dummy_state = D3D12_RESOURCE_STATE_COMMON;

    uint64_t vram_bytes = 0;
    uint32_t frame_index = 0;
    uint32_t ring_cursor = 0;
};

HRESULT serialize_root_sig(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc, ComPtr<ID3DBlob>& out_blob) noexcept {
    using SerFn = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
    HMODULE h_d3d12_xs = GetModuleHandleW(L"d3d12_xs.dll");
    if (!h_d3d12_xs) h_d3d12_xs = LoadLibraryW(L"d3d12_xs.dll");
    auto fn = h_d3d12_xs
        ? reinterpret_cast<SerFn>(GetProcAddress(h_d3d12_xs, "D3D12SerializeVersionedRootSignature"))
        : nullptr;
    if (!fn) return E_FAIL;
    ComPtr<ID3DBlob> err;
    return fn(&desc, out_blob.GetAddressOf(), err.GetAddressOf());
}

HRESULT init_context_gpu(FfxUpscalerContext& ctx) noexcept {
    if (!ensure_shaders_compiled()) return E_FAIL;
    const auto& sh = g_shaders();

    // 1. Static linear clamp sampler
    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    samp.MaxLOD = D3D12_FLOAT32_MAX;

    // Temporal Root Signature: 16 DWORDs b0 + 6 SRVs t0..t5 + 2 UAVs u0..u1 + static sampler s0
    {
        D3D12_DESCRIPTOR_RANGE1 r_srv{};
        r_srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r_srv.NumDescriptors = 6;
        r_srv.BaseShaderRegister = 0;
        r_srv.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        D3D12_DESCRIPTOR_RANGE1 r_uav{};
        r_uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        r_uav.NumDescriptors = 2;
        r_uav.BaseShaderRegister = 0;
        r_uav.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

        D3D12_ROOT_PARAMETER1 params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 16};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &r_srv};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = {1, &r_uav};
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
        rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rsd.Desc_1_1 = {3, params, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob;
        HRESULT hr = serialize_root_sig(rsd, blob);
        if (FAILED(hr)) return hr;
        hr = ctx.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(ctx.rs_temporal.GetAddressOf()));
        if (FAILED(hr)) return hr;
    }

    // RCAS Root Signature: 4 DWORDs b0 + 1 SRV t0 + 1 UAV u0
    {
        D3D12_DESCRIPTOR_RANGE1 r_srv{};
        r_srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r_srv.NumDescriptors = 1;
        r_srv.BaseShaderRegister = 0;
        r_srv.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        D3D12_DESCRIPTOR_RANGE1 r_uav{};
        r_uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        r_uav.NumDescriptors = 1;
        r_uav.BaseShaderRegister = 0;
        r_uav.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

        D3D12_ROOT_PARAMETER1 params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 4};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &r_srv};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = {1, &r_uav};
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
        rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rsd.Desc_1_1 = {3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob;
        HRESULT hr = serialize_root_sig(rsd, blob);
        if (FAILED(hr)) return hr;
        hr = ctx.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(ctx.rs_rcas.GetAddressOf()));
        if (FAILED(hr)) return hr;
    }

    // Reactive Mask Root Signature: 8 DWORDs b0 + 2 SRVs t0..t1 + 1 UAV u0
    {
        D3D12_DESCRIPTOR_RANGE1 r_srv{};
        r_srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r_srv.NumDescriptors = 2;
        r_srv.BaseShaderRegister = 0;
        r_srv.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        D3D12_DESCRIPTOR_RANGE1 r_uav{};
        r_uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        r_uav.NumDescriptors = 1;
        r_uav.BaseShaderRegister = 0;
        r_uav.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

        D3D12_ROOT_PARAMETER1 params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 8};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &r_srv};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = {1, &r_uav};
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
        rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rsd.Desc_1_1 = {3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob;
        HRESULT hr = serialize_root_sig(rsd, blob);
        if (FAILED(hr)) return hr;
        hr = ctx.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(ctx.rs_reactive.GetAddressOf()));
        if (FAILED(hr)) return hr;
    }

    // 2. Compute PSOs
    auto make_pso = [&](ID3D12RootSignature* rs, const std::vector<uint8_t>& dxil, ComPtr<ID3D12PipelineState>& out) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rs;
        pd.CS = {dxil.data(), dxil.size()};
        return ctx.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(out.GetAddressOf()));
    };
    HRESULT hr = make_pso(ctx.rs_temporal.Get(), sh.temporal_dxil, ctx.pso_temporal);
    if (FAILED(hr)) return hr;
    hr = make_pso(ctx.rs_rcas.Get(), sh.rcas_dxil, ctx.pso_rcas);
    if (FAILED(hr)) return hr;
    hr = make_pso(ctx.rs_reactive.Get(), sh.reactive_dxil, ctx.pso_reactive);
    if (FAILED(hr)) return hr;

    // 3. Descriptor heap ring (64 slots x 8 descriptors)
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kDescriptorRingSlots * kDescriptorsPerSlot;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ctx.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(ctx.srv_heap.GetAddressOf()));
    if (FAILED(hr)) return hr;
    ctx.srv_inc = ctx.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // 4. Ping-pong high-resolution RGBA16F history textures + 1x1 dummy texture
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC hdesc{};
    hdesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    hdesc.Width = std::max<uint32_t>(ctx.max_upscale.width, 1u);
    hdesc.Height = std::max<uint32_t>(ctx.max_upscale.height, 1u);
    hdesc.DepthOrArraySize = 1;
    hdesc.MipLevels = 1;
    hdesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    hdesc.SampleDesc.Count = 1;
    hdesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    const D3D12_RESOURCE_ALLOCATION_INFO h_alloc = ctx.dev->GetResourceAllocationInfo(0, 1, &hdesc);
    ctx.vram_bytes = h_alloc.SizeInBytes * 2ull;

    for (int i = 0; i < 2; ++i) {
        hr = ctx.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &hdesc,
                                              D3D12_RESOURCE_STATE_COMMON, nullptr,
                                              IID_PPV_ARGS(ctx.history[i].GetAddressOf()));
        if (FAILED(hr)) return hr;
        ctx.history_state[i] = D3D12_RESOURCE_STATE_COMMON;
    }

    D3D12_RESOURCE_DESC ddesc = hdesc;
    ddesc.Width = 1;
    ddesc.Height = 1;
    ddesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    hr = ctx.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ddesc,
                                          D3D12_RESOURCE_STATE_COMMON, nullptr,
                                          IID_PPV_ARGS(ctx.dummy_zero_tex.GetAddressOf()));
    if (FAILED(hr)) return hr;
    ctx.dummy_state = D3D12_RESOURCE_STATE_COMMON;
    ctx.vram_bytes += ctx.dev->GetResourceAllocationInfo(0, 1, &ddesc).SizeInBytes;

    return S_OK;
}

void push_transition(
    std::vector<D3D12_RESOURCE_BARRIER>& bars,
    ID3D12Resource* res,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
    res = wsx_d3d12::unwrap_resource(res);
    if (!res || before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    bars.push_back(b);
}

void create_tex2d_srv(ID3D12Device* dev, ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE dst) noexcept {
    res = wsx_d3d12::unwrap_resource(res);
    const D3D12_RESOURCE_DESC rd = res->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = resolve_view_format(rd.Format);
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(res, &srv, dst);
}

void create_tex2d_uav(ID3D12Device* dev, ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE dst) noexcept {
    res = wsx_d3d12::unwrap_resource(res);
    const D3D12_RESOURCE_DESC rd = res->GetDesc();
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = resolve_view_format(rd.Format);
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Texture2D.MipSlice = 0;
    dev->CreateUnorderedAccessView(res, nullptr, &uav, dst);
}

uint32_t dispatch_upscale_impl(FfxUpscalerContext& ctx, const FfxDispatchDescUpscale& desc) noexcept {
    if (!desc.commandList || !desc.color.resource || !desc.output.resource) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    ComPtr<ID3D12GraphicsCommandList> cl = unwrap_cmdlist(desc.commandList);
    if (!cl) return FFX_API_RETURN_ERROR_PARAMETER;

    auto* res_color = wsx_d3d12::unwrap_resource(reinterpret_cast<ID3D12Resource*>(desc.color.resource));
    auto* res_output = wsx_d3d12::unwrap_resource(reinterpret_cast<ID3D12Resource*>(desc.output.resource));
    auto* res_mv = wsx_d3d12::unwrap_resource(reinterpret_cast<ID3D12Resource*>(desc.motionVectors.resource));
    auto* res_reactive = wsx_d3d12::unwrap_resource(reinterpret_cast<ID3D12Resource*>(desc.reactive.resource));
    auto* res_depth = wsx_d3d12::unwrap_resource(reinterpret_cast<ID3D12Resource*>(desc.depth.resource));
    auto* res_transp = wsx_d3d12::unwrap_resource(reinterpret_cast<ID3D12Resource*>(desc.transparencyAndComposition.resource));

    const D3D12_RESOURCE_DESC color_rd = res_color->GetDesc();
    const D3D12_RESOURCE_DESC out_rd = res_output->GetDesc();

    const uint32_t rw = desc.renderSize.width ? desc.renderSize.width : static_cast<uint32_t>(color_rd.Width);
    const uint32_t rh = desc.renderSize.height ? desc.renderSize.height : color_rd.Height;
    const uint32_t uw = desc.upscaleSize.width ? desc.upscaleSize.width : std::min<uint32_t>(ctx.max_upscale.width, static_cast<uint32_t>(out_rd.Width));
    const uint32_t uh = desc.upscaleSize.height ? desc.upscaleSize.height : std::min<uint32_t>(ctx.max_upscale.height, out_rd.Height);
    if (rw == 0 || rh == 0 || uw == 0 || uh == 0) return FFX_API_RETURN_ERROR_PARAMETER;

    const bool do_rcas = desc.enableSharpening && (desc.sharpness > 1e-4f);
    const uint32_t read_hist = ctx.frame_index & 1u;
    const uint32_t write_hist = read_hist ^ 1u;

    const D3D12_RESOURCE_STATES st_color_orig = ffx_state_to_d3d12(desc.color.state);
    const D3D12_RESOURCE_STATES st_out_orig = ffx_state_to_d3d12(desc.output.state);
    const D3D12_RESOURCE_STATES st_mv_orig = res_mv ? ffx_state_to_d3d12(desc.motionVectors.state) : D3D12_RESOURCE_STATE_COMMON;
    const D3D12_RESOURCE_STATES st_react_orig = res_reactive ? ffx_state_to_d3d12(desc.reactive.state) : D3D12_RESOURCE_STATE_COMMON;
    const D3D12_RESOURCE_STATES st_depth_orig = res_depth ? ffx_state_to_d3d12(desc.depth.state) : D3D12_RESOURCE_STATE_COMMON;
    const D3D12_RESOURCE_STATES st_transp_orig = res_transp ? ffx_state_to_d3d12(desc.transparencyAndComposition.state) : D3D12_RESOURCE_STATE_COMMON;

    // 1. Pre-pass resource state transitions
    std::vector<D3D12_RESOURCE_BARRIER> pre_bars;
    pre_bars.reserve(10);
    push_transition(pre_bars, res_color, st_color_orig, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (res_mv && res_mv != res_color) {
        push_transition(pre_bars, res_mv, st_mv_orig, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (res_reactive && res_reactive != res_color && res_reactive != res_mv) {
        push_transition(pre_bars, res_reactive, st_react_orig, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (res_depth && res_depth != res_color && res_depth != res_mv && res_depth != res_reactive) {
        push_transition(pre_bars, res_depth, st_depth_orig, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (res_transp && res_transp != res_color && res_transp != res_mv && res_transp != res_reactive && res_transp != res_depth) {
        push_transition(pre_bars, res_transp, st_transp_orig, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    push_transition(pre_bars, res_output, st_out_orig, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    push_transition(pre_bars, ctx.history[read_hist].Get(), ctx.history_state[read_hist], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ctx.history_state[read_hist] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    push_transition(pre_bars, ctx.history[write_hist].Get(), ctx.history_state[write_hist], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.history_state[write_hist] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    push_transition(pre_bars, ctx.dummy_zero_tex.Get(), ctx.dummy_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ctx.dummy_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (!pre_bars.empty()) {
        cl->ResourceBarrier(static_cast<UINT>(pre_bars.size()), pre_bars.data());
    }

    // 2. Populate descriptor ring slot for Pass 1 (t0..t5, u0..u1) & optional Pass 2 (t0, u0)
    const uint32_t slot = ctx.ring_cursor % kDescriptorRingSlots;
    ctx.ring_cursor++;
    const UINT base_idx = slot * kDescriptorsPerSlot;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = ctx.srv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu0 = ctx.srv_heap->GetGPUDescriptorHandleForHeapStart();
    auto cpu_at = [&](UINT i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu0.ptr + SIZE_T(base_idx + i) * ctx.srv_inc}; };
    auto gpu_at = [&](UINT i) { return D3D12_GPU_DESCRIPTOR_HANDLE{gpu0.ptr + UINT64(base_idx + i) * ctx.srv_inc}; };

    create_tex2d_srv(ctx.dev.Get(), res_color, cpu_at(0));
    create_tex2d_srv(ctx.dev.Get(), ctx.history[read_hist].Get(), cpu_at(1));
    create_tex2d_srv(ctx.dev.Get(), res_mv ? res_mv : ctx.dummy_zero_tex.Get(), cpu_at(2));
    create_tex2d_srv(ctx.dev.Get(), res_reactive ? res_reactive : ctx.dummy_zero_tex.Get(), cpu_at(3));
    create_tex2d_srv(ctx.dev.Get(), res_depth ? res_depth : ctx.dummy_zero_tex.Get(), cpu_at(4));
    create_tex2d_srv(ctx.dev.Get(), res_transp ? res_transp : ctx.dummy_zero_tex.Get(), cpu_at(5));
    create_tex2d_uav(ctx.dev.Get(), ctx.history[write_hist].Get(), cpu_at(6));
    create_tex2d_uav(ctx.dev.Get(), res_output, cpu_at(7));

    // 3. Pass 1: Temporal Lanczos-2 Upscale (HDR tone-space + 3x3 depth-dilated MV + transparency mask)
    struct TemporalCB {
        uint32_t render_w, render_h, upscale_w, upscale_h;
        float jitter_x, jitter_y, mv_scale_x, mv_scale_y;
        float pre_exposure, sharpness;
        uint32_t reset_flag, flags;
        uint32_t frame_idx, pad0, pad1, pad2;
    } cb{};
    cb.render_w = rw;
    cb.render_h = rh;
    cb.upscale_w = uw;
    cb.upscale_h = uh;
    cb.jitter_x = desc.jitterOffset.x;
    cb.jitter_y = desc.jitterOffset.y;
    cb.mv_scale_x = (desc.motionVectorScale.x != 0.0f) ? desc.motionVectorScale.x : 1.0f;
    cb.mv_scale_y = (desc.motionVectorScale.y != 0.0f) ? desc.motionVectorScale.y : 1.0f;
    cb.pre_exposure = (desc.preExposure > 0.0f) ? desc.preExposure : 1.0f;
    cb.sharpness = std::clamp(desc.sharpness, 0.0f, 1.0f);
    cb.reset_flag = desc.reset ? 1u : 0u;
    cb.flags = (res_mv ? 1u : 0u) |
               (res_reactive ? 2u : 0u) |
               ((ctx.create_flags & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) ? 4u : 0u) |
               ((ctx.create_flags & FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE) ? 8u : 0u) |
               (do_rcas ? 16u : 0u) |
               (res_depth ? 32u : 0u) |
               ((ctx.create_flags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) ? 64u : 0u) |
               (res_transp ? 128u : 0u);
    cb.frame_idx = desc.reset ? 0u : ctx.frame_index;

    ID3D12DescriptorHeap* heaps[1] = {ctx.srv_heap.Get()};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(ctx.rs_temporal.Get());
    cl->SetPipelineState(ctx.pso_temporal.Get());
    cl->SetComputeRoot32BitConstants(0, 16, &cb, 0);
    cl->SetComputeRootDescriptorTable(1, gpu_at(0));
    cl->SetComputeRootDescriptorTable(2, gpu_at(6));
    cl->Dispatch((uw + 7u) / 8u, (uh + 7u) / 8u, 1u);

    // 4. Optional Pass 2: RCAS Sharpening (history[write_hist] -> output)
    if (do_rcas) {
        g_stats().dispatch_rcas.fetch_add(1, std::memory_order_relaxed);

        D3D12_RESOURCE_BARRIER rcas_bar{};
        rcas_bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rcas_bar.Transition.pResource = wsx_d3d12::unwrap_resource(ctx.history[write_hist].Get());
        rcas_bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        rcas_bar.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        rcas_bar.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cl->ResourceBarrier(1, &rcas_bar);
        ctx.history_state[write_hist] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        create_tex2d_srv(ctx.dev.Get(), ctx.history[write_hist].Get(), cpu_at(8));
        create_tex2d_uav(ctx.dev.Get(), res_output, cpu_at(9));

        struct RcasCB {
            uint32_t upscale_w, upscale_h;
            float sharpness;
            uint32_t pad0;
        } rcb{uw, uh, std::clamp(desc.sharpness, 0.0f, 1.0f), 0u};

        cl->SetComputeRootSignature(ctx.rs_rcas.Get());
        cl->SetPipelineState(ctx.pso_rcas.Get());
        cl->SetComputeRoot32BitConstants(0, 4, &rcb, 0);
        cl->SetComputeRootDescriptorTable(1, gpu_at(8));
        cl->SetComputeRootDescriptorTable(2, gpu_at(9));
        cl->Dispatch((uw + 7u) / 8u, (uh + 7u) / 8u, 1u);
    }

    // 5. UAV barrier on output + restore caller resource states
    std::vector<D3D12_RESOURCE_BARRIER> post_bars;
    post_bars.reserve(8);
    {
        D3D12_RESOURCE_BARRIER uavb{};
        uavb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavb.UAV.pResource = res_output;
        post_bars.push_back(uavb);
    }
    push_transition(post_bars, res_color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, st_color_orig);
    if (res_mv && res_mv != res_color) {
        push_transition(post_bars, res_mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, st_mv_orig);
    }
    if (res_reactive && res_reactive != res_color && res_reactive != res_mv) {
        push_transition(post_bars, res_reactive, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, st_react_orig);
    }
    if (res_depth && res_depth != res_color && res_depth != res_mv && res_depth != res_reactive) {
        push_transition(post_bars, res_depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, st_depth_orig);
    }
    if (res_transp && res_transp != res_color && res_transp != res_mv && res_transp != res_reactive && res_transp != res_depth) {
        push_transition(post_bars, res_transp, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, st_transp_orig);
    }
    push_transition(post_bars, res_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, st_out_orig);
    cl->ResourceBarrier(static_cast<UINT>(post_bars.size()), post_bars.data());

    ctx.frame_index = desc.reset ? 1u : (ctx.frame_index + 1u);
    g_stats().dispatch_upscale.fetch_add(1, std::memory_order_relaxed);
    wsx::trace_void("FfxUpscaler", "ffxDispatch(DX12-SM66)",
                    "render=%ux%u upscale=%ux%u rcas=%u reset=%u frame=%u",
                    rw, rh, uw, uh, do_rcas ? 1u : 0u, desc.reset ? 1u : 0u, ctx.frame_index);
    return FFX_API_RETURN_OK;
}

uint32_t dispatch_reactive_impl(FfxUpscalerContext& ctx, const FfxDispatchDescGenerateReactiveMask& desc) noexcept {
    if (!desc.commandList || !desc.colorOpaqueOnly.resource ||
        !desc.colorPreUpscale.resource || !desc.outReactive.resource) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    ComPtr<ID3D12GraphicsCommandList> cl = unwrap_cmdlist(desc.commandList);
    if (!cl) return FFX_API_RETURN_ERROR_PARAMETER;

    auto* res_op = wsx_d3d12::unwrap_resource(reinterpret_cast<ID3D12Resource*>(desc.colorOpaqueOnly.resource));
    auto* res_pre = wsx_d3d12::unwrap_resource(reinterpret_cast<ID3D12Resource*>(desc.colorPreUpscale.resource));
    auto* res_out = wsx_d3d12::unwrap_resource(reinterpret_cast<ID3D12Resource*>(desc.outReactive.resource));

    const D3D12_RESOURCE_DESC rd = res_out->GetDesc();
    const uint32_t rw = desc.renderSize.width ? desc.renderSize.width : static_cast<uint32_t>(rd.Width);
    const uint32_t rh = desc.renderSize.height ? desc.renderSize.height : rd.Height;

    const D3D12_RESOURCE_STATES st_op = ffx_state_to_d3d12(desc.colorOpaqueOnly.state);
    const D3D12_RESOURCE_STATES st_pre = ffx_state_to_d3d12(desc.colorPreUpscale.state);
    const D3D12_RESOURCE_STATES st_out = ffx_state_to_d3d12(desc.outReactive.state);

    std::vector<D3D12_RESOURCE_BARRIER> pre_bars;
    push_transition(pre_bars, res_op, st_op, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (res_pre != res_op) {
        push_transition(pre_bars, res_pre, st_pre, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    push_transition(pre_bars, res_out, st_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!pre_bars.empty()) cl->ResourceBarrier(static_cast<UINT>(pre_bars.size()), pre_bars.data());

    const uint32_t slot = ctx.ring_cursor % kDescriptorRingSlots;
    ctx.ring_cursor++;
    const UINT base_idx = slot * kDescriptorsPerSlot;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = ctx.srv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu0 = ctx.srv_heap->GetGPUDescriptorHandleForHeapStart();
    auto cpu_at = [&](UINT i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu0.ptr + SIZE_T(base_idx + i) * ctx.srv_inc}; };
    auto gpu_at = [&](UINT i) { return D3D12_GPU_DESCRIPTOR_HANDLE{gpu0.ptr + UINT64(base_idx + i) * ctx.srv_inc}; };

    create_tex2d_srv(ctx.dev.Get(), res_op, cpu_at(0));
    create_tex2d_srv(ctx.dev.Get(), res_pre, cpu_at(1));
    create_tex2d_uav(ctx.dev.Get(), res_out, cpu_at(2));

    struct ReactCB {
        uint32_t render_w, render_h;
        float scale, cutoff, binary;
        uint32_t flags, pad0, pad1;
    } rcb{rw, rh, desc.scale > 0.0f ? desc.scale : 1.0f, desc.cutoffThreshold,
          desc.binaryValue > 0.0f ? desc.binaryValue : 0.9f, desc.flags, 0u, 0u};

    ID3D12DescriptorHeap* heaps[1] = {ctx.srv_heap.Get()};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(ctx.rs_reactive.Get());
    cl->SetPipelineState(ctx.pso_reactive.Get());
    cl->SetComputeRoot32BitConstants(0, 8, &rcb, 0);
    cl->SetComputeRootDescriptorTable(1, gpu_at(0));
    cl->SetComputeRootDescriptorTable(2, gpu_at(2));
    cl->Dispatch((rw + 7u) / 8u, (rh + 7u) / 8u, 1u);

    std::vector<D3D12_RESOURCE_BARRIER> post_bars;
    {
        D3D12_RESOURCE_BARRIER uavb{};
        uavb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavb.UAV.pResource = res_out;
        post_bars.push_back(uavb);
    }
    push_transition(post_bars, res_op, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, st_op);
    if (res_pre != res_op) {
        push_transition(post_bars, res_pre, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, st_pre);
    }
    push_transition(post_bars, res_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, st_out);
    cl->ResourceBarrier(static_cast<UINT>(post_bars.size()), post_bars.data());

    g_stats().dispatch_reactive.fetch_add(1, std::memory_order_relaxed);
    return FFX_API_RETURN_OK;
}

} // namespace

// ============================================================================
// Public FidelityFX API v2.0 Exports (C ABI)
// ============================================================================

extern "C" {

uint32_t __cdecl ffxCreateContext(void** context, FfxApiHeader* desc, const void* /*memCb*/) {
    if (!context || !desc) return FFX_API_RETURN_ERROR_PARAMETER;
    *context = nullptr;

    const FfxCreateContextDescUpscale* up_desc = nullptr;
    const FfxCreateBackendDX12Desc* dx12_desc = nullptr;

    for (const FfxApiHeader* cur = desc; cur != nullptr; cur = cur->pNext) {
        if (cur->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) {
            up_desc = reinterpret_cast<const FfxCreateContextDescUpscale*>(cur);
        } else if (cur->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12) {
            dx12_desc = reinterpret_cast<const FfxCreateBackendDX12Desc*>(cur);
        }
    }
    if (!up_desc || !dx12_desc || !dx12_desc->device) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }

    ComPtr<ID3D12Device> dev = unwrap_device(dx12_desc->device);
    if (!dev) return FFX_API_RETURN_ERROR_PARAMETER;

    auto* ctx = new (std::nothrow) FfxUpscalerContext{};
    if (!ctx) return FFX_API_RETURN_ERROR_MEMORY;

    ctx->dev = dev;
    ctx->create_flags = up_desc->flags;
    ctx->max_render = up_desc->maxRenderSize;
    ctx->max_upscale = up_desc->maxUpscaleSize;
    ctx->log_cb = up_desc->fpMessage;

    const HRESULT hr = init_context_gpu(*ctx);
    if (FAILED(hr)) {
        delete ctx;
        wsx::trace_hr("FfxUpscaler", "ffxCreateContext(DX12-SM66)", hr, "init_context_gpu failed");
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    }

    *context = ctx;
    g_stats().contexts_created.fetch_add(1, std::memory_order_relaxed);
    wsx::trace_void("FfxUpscaler", "ffxCreateContext(DX12-SM66)",
                    "ctx=%p render=%ux%u upscale=%ux%u vram=%llu B",
                    ctx, ctx->max_render.width, ctx->max_render.height,
                    ctx->max_upscale.width, ctx->max_upscale.height,
                    static_cast<unsigned long long>(ctx->vram_bytes));
    return FFX_API_RETURN_OK;
}

uint32_t __cdecl ffxDestroyContext(void** context, const void* /*memCb*/) {
    if (!context || !*context) return FFX_API_RETURN_ERROR_PARAMETER;
    auto* ctx = static_cast<FfxUpscalerContext*>(*context);
    if (ctx->magic != 0x465352345F445831ull) return FFX_API_RETURN_ERROR_PARAMETER;
    ctx->magic = 0;
    delete ctx;
    *context = nullptr;
    g_stats().contexts_destroyed.fetch_add(1, std::memory_order_relaxed);
    return FFX_API_RETURN_OK;
}

uint32_t __cdecl ffxConfigure(void** context, const FfxApiHeader* desc) {
    g_stats().configure_calls.fetch_add(1, std::memory_order_relaxed);
    if (!desc) return FFX_API_RETURN_ERROR_PARAMETER;
    for (const FfxApiHeader* cur = desc; cur != nullptr; cur = cur->pNext) {
        if (cur->type == FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1) {
            const auto* dbg = reinterpret_cast<const FfxConfigureGlobalDebug1*>(cur);
            if (context && *context) {
                auto* ctx = static_cast<FfxUpscalerContext*>(*context);
                if (ctx->magic == 0x465352345F445831ull) {
                    ctx->log_cb = dbg->fpMessage;
                    ctx->debug_level = dbg->debugLevel;
                }
            }
        } else if (cur->type == FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE) {
            // Runtime key-value overrides accepted cleanly
        } else {
            return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
        }
    }
    return FFX_API_RETURN_OK;
}

uint32_t __cdecl ffxQuery(void** context, FfxApiHeader* desc) {
    g_stats().query_calls.fetch_add(1, std::memory_order_relaxed);
    if (!desc) return FFX_API_RETURN_ERROR_PARAMETER;

    for (FfxApiHeader* cur = desc; cur != nullptr; cur = cur->pNext) {
        switch (cur->type) {
        case FFX_API_QUERY_DESC_TYPE_GET_VERSIONS: {
            auto* q = reinterpret_cast<FfxQueryGetVersions*>(cur);
            if (!q->outputCount) return FFX_API_RETURN_ERROR_PARAMETER;
            if (q->createDescType != FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE && q->createDescType != 0ull) {
                *q->outputCount = 0;
                return FFX_API_RETURN_OK;
            }
            const uint64_t cap = *q->outputCount;
            *q->outputCount = 1ull;
            if (q->versionIds && q->versionNames && cap >= 1ull) {
                q->versionIds[0] = kFsr411VersionId;
                q->versionNames[0] = kFsr411VersionName;
            }
            break;
        }
        case FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION: {
            auto* q = reinterpret_cast<FfxQueryProviderVersion*>(cur);
            q->versionId = kFsr411VersionId;
            q->versionName = kFsr411VersionName;
            break;
        }
        case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE: {
            auto* q = reinterpret_cast<FfxQueryUpscaleRatio*>(cur);
            if (!q->pOutUpscaleRatio) return FFX_API_RETURN_ERROR_PARAMETER;
            const float ratio = quality_mode_ratio(q->qualityMode);
            if (ratio <= 0.0f) return FFX_API_RETURN_ERROR_PARAMETER;
            *q->pOutUpscaleRatio = ratio;
            break;
        }
        case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE: {
            auto* q = reinterpret_cast<FfxQueryRenderResolution*>(cur);
            const float ratio = quality_mode_ratio(q->qualityMode);
            if (ratio <= 0.0f || !q->pOutRenderWidth || !q->pOutRenderHeight) {
                return FFX_API_RETURN_ERROR_PARAMETER;
            }
            *q->pOutRenderWidth = std::max<uint32_t>(1u, static_cast<uint32_t>(static_cast<float>(q->displayWidth) / ratio));
            *q->pOutRenderHeight = std::max<uint32_t>(1u, static_cast<uint32_t>(static_cast<float>(q->displayHeight) / ratio));
            break;
        }
        case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT: {
            auto* q = reinterpret_cast<FfxQueryJitterPhaseCount*>(cur);
            if (!q->pOutPhaseCount || q->renderWidth == 0u || q->displayWidth == 0u) {
                return FFX_API_RETURN_ERROR_PARAMETER;
            }
            const float r = static_cast<float>(q->displayWidth) / static_cast<float>(q->renderWidth);
            *q->pOutPhaseCount = static_cast<int32_t>(std::ceil(8.0f * r * r));
            break;
        }
        case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET: {
            auto* q = reinterpret_cast<FfxQueryJitterOffset*>(cur);
            if (!q->pOutX || !q->pOutY || q->phaseCount <= 0) {
                return FFX_API_RETURN_ERROR_PARAMETER;
            }
            const int32_t idx = (q->index % q->phaseCount) + 1;
            *q->pOutX = halton(idx, 2) - 0.5f;
            *q->pOutY = halton(idx, 3) - 0.5f;
            break;
        }
        case FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE: {
            auto* q = reinterpret_cast<FfxQueryGpuMemoryUsage*>(cur);
            if (!q->gpuMemoryUsageUpscaler || !context || !*context) {
                return FFX_API_RETURN_ERROR_PARAMETER;
            }
            auto* ctx = static_cast<FfxUpscalerContext*>(*context);
            if (ctx->magic != 0x465352345F445831ull) return FFX_API_RETURN_ERROR_PARAMETER;
            q->gpuMemoryUsageUpscaler->totalUsageInBytes = ctx->vram_bytes;
            q->gpuMemoryUsageUpscaler->aliasableUsageInBytes = 0ull;
            break;
        }
        case FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE_V2: {
            auto* q = reinterpret_cast<FfxQueryGpuMemoryUsageV2*>(cur);
            if (!q->gpuMemoryUsageUpscaler) return FFX_API_RETURN_ERROR_PARAMETER;
            const uint64_t pixels = static_cast<uint64_t>(std::max<uint32_t>(q->maxUpscaleSize.width, 1u)) *
                                    static_cast<uint64_t>(std::max<uint32_t>(q->maxUpscaleSize.height, 1u));
            q->gpuMemoryUsageUpscaler->totalUsageInBytes = pixels * 8ull * 2ull; // 2x RGBA16F history
            q->gpuMemoryUsageUpscaler->aliasableUsageInBytes = 0ull;
            break;
        }
        case FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_DEFAULT_PRESET: {
            auto* words = reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(cur) + 0x10);
            words[0] = 0ull;
            words[1] = 0ull;
            break;
        }
        default:
            return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
        }
    }
    return FFX_API_RETURN_OK;
}

uint32_t __cdecl ffxDispatch(void** context, const FfxApiHeader* desc) {
    if (!context || !*context || !desc) return FFX_API_RETURN_ERROR_PARAMETER;
    auto* ctx = static_cast<FfxUpscalerContext*>(*context);
    if (ctx->magic != 0x465352345F445831ull) return FFX_API_RETURN_ERROR_PARAMETER;

    for (const FfxApiHeader* cur = desc; cur != nullptr; cur = cur->pNext) {
        if (cur->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
            const uint32_t rc = dispatch_upscale_impl(*ctx, *reinterpret_cast<const FfxDispatchDescUpscale*>(cur));
            if (rc != FFX_API_RETURN_OK) return rc;
        } else if (cur->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE_GENERATEREACTIVEMASK) {
            const uint32_t rc = dispatch_reactive_impl(*ctx, *reinterpret_cast<const FfxDispatchDescGenerateReactiveMask*>(cur));
            if (rc != FFX_API_RETURN_OK) return rc;
        } else {
            return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
        }
    }
    return FFX_API_RETURN_OK;
}

HRESULT WINAPI WsxFfxUpscalerGetStats(WsxFfxUpscalerStats* pOut) {
    if (!pOut) return E_POINTER;
    auto& s = g_stats();
    pOut->ContextsCreated = s.contexts_created.load(std::memory_order_relaxed);
    pOut->ContextsDestroyed = s.contexts_destroyed.load(std::memory_order_relaxed);
    pOut->QueryCalls = s.query_calls.load(std::memory_order_relaxed);
    pOut->ConfigureCalls = s.configure_calls.load(std::memory_order_relaxed);
    pOut->DispatchUpscaleCalls = s.dispatch_upscale.load(std::memory_order_relaxed);
    pOut->DispatchRcasPasses = s.dispatch_rcas.load(std::memory_order_relaxed);
    pOut->DispatchReactiveCalls = s.dispatch_reactive.load(std::memory_order_relaxed);
    return S_OK;
}

} // extern "C"
