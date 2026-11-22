// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Direct3D 12 SM 6.6 Next-Gen GPU Video Frame Converter & HDR10 Tone Mapper:
// - Supports Planar YCbCr(A) 4:2:0 (8-bit Bink2/I420), Semi-Planar NV12 (8-bit), and
//   Semi-Planar P010 (10-bit HDR10) with bilinear chroma upsampling.
// - Supports BT.709 Limited/Full, BT.601 Limited, BT.2020 PQ HDR10 (native R10G10B10A2_UNORM PQ
//   or linear scRGB R16G16B16A16_FLOAT), and BT.2020 PQ -> ACES Filmic SDR tone mapping.
// - Uses a persistent double-buffered UPLOAD staging ring (zero per-frame CreateCommittedResource
//   allocations during continuous 60/120 FPS playback).

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wsx_video {

enum class YuvColorSpace : uint32_t {
    Bt709Limited      = 0u,
    Bt709Full         = 1u,
    Bt601Limited      = 2u,
    Bt2020PqHdr10     = 3u, // BT.2020 PQ -> HDR10 (PQ R10G10B10A2) or Linear scRGB (FP16 nits/80)
    Bt2020PqToSdrAces = 4u  // BT.2020 PQ -> BT.709 ACES Filmic HDR->SDR -> sRGB RGBA8
};

enum class YuvInputFormat : uint32_t {
    Planar420_8bit       = 0u, // Separate Y, Cb, Cr (+ optional Alpha) 8-bit planes
    SemiPlanarNV12_8bit  = 1u, // 8-bit Y plane + interleaved CbCr 8-bit UV plane
    SemiPlanarP010_10bit = 2u  // 16-bit LE Y plane + interleaved CbCr 16-bit LE UV plane (10-bit in bits [15:6])
};

class WsxYuvToRgbaConverter {
    using CompileHlslFn = HRESULT(WINAPI*)(
        const char*, size_t, const wchar_t*, const wchar_t*,
        void*, size_t, size_t*, char*, size_t);

    struct FrameUploadSlot {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> cmd_alloc;
        Microsoft::WRL::ComPtr<ID3D12Resource> upload_buf;
        uint64_t upload_capacity = 0u;
        UINT64 fence_val = 0u;
    };

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_sig_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> uav_heap_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmd_list_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    UINT64 fence_value_ = 0u;
    FrameUploadSlot slots_[2]{};
    uint32_t slot_cursor_ = 0u;
    uint64_t upload_allocations_ = 0u;
    uint64_t frames_converted_ = 0u;
    bool initialized_ = false;

    static CompileHlslFn resolve_dxil_compiler() noexcept {
        HMODULE mod = GetModuleHandleW(L"d3d12_xs.dll");
        if (!mod) mod = LoadLibraryW(L"d3d12_xs.dll");
        if (!mod) return nullptr;
        return reinterpret_cast<CompileHlslFn>(GetProcAddress(mod, "D3D12XboxCompileHlslToDxil"));
    }

    static uint64_t align_up(uint64_t v, uint64_t a) noexcept {
        return (v + a - 1u) & ~(a - 1u);
    }

    HRESULT ensure_slot_buffer(FrameUploadSlot& slot, uint64_t needed_bytes) noexcept {
        if (slot.upload_buf && slot.upload_capacity >= needed_bytes) {
            return S_OK;
        }
        const uint64_t cap = align_up((std::max<uint64_t>)(needed_bytes, 65536u), 65536u);
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = cap;
        rd.Height = 1u;
        rd.DepthOrArraySize = 1u;
        rd.MipLevels = 1u;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1u;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        Microsoft::WRL::ComPtr<ID3D12Resource> fresh;
        const HRESULT hr = device_->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&fresh));
        if (FAILED(hr)) return hr;
        slot.upload_buf = std::move(fresh);
        slot.upload_capacity = cap;
        ++upload_allocations_;
        return S_OK;
    }

    HRESULT wait_fence(UINT64 target) noexcept {
        if (!fence_ || target == 0u) return S_OK;
        if (fence_->GetCompletedValue() >= target) return S_OK;
        const HRESULT hr = fence_->SetEventOnCompletion(target, fence_event_);
        if (FAILED(hr)) return hr;
        WaitForSingleObject(fence_event_, 10000u);
        return S_OK;
    }

public:
    WsxYuvToRgbaConverter() = default;
    ~WsxYuvToRgbaConverter() { shutdown(); }

    WsxYuvToRgbaConverter(const WsxYuvToRgbaConverter&) = delete;
    WsxYuvToRgbaConverter& operator=(const WsxYuvToRgbaConverter&) = delete;

    void shutdown() noexcept {
        if (initialized_ && fence_ && fence_value_ > 0u) {
            wait_fence(fence_value_);
        }
        initialized_ = false;
        for (auto& s : slots_) {
            s.upload_buf.Reset();
            s.cmd_alloc.Reset();
            s.upload_capacity = 0u;
            s.fence_val = 0u;
        }
        if (fence_event_) {
            CloseHandle(fence_event_);
            fence_event_ = nullptr;
        }
        fence_.Reset();
        cmd_list_.Reset();
        uav_heap_.Reset();
        pso_.Reset();
        root_sig_.Reset();
        device_.Reset();
        slot_cursor_ = 0u;
        upload_allocations_ = 0u;
        frames_converted_ = 0u;
    }

    bool is_initialized() const noexcept { return initialized_; }
    uint64_t upload_allocations() const noexcept { return upload_allocations_; }
    uint64_t frames_converted() const noexcept { return frames_converted_; }

    bool initialize(ID3D12Device* device, std::string* out_err = nullptr) {
        shutdown();
        if (!device) {
            if (out_err) *out_err = "null ID3D12Device";
            return false;
        }
        const CompileHlslFn compiler = resolve_dxil_compiler();
        if (!compiler) {
            if (out_err) *out_err = "D3D12XboxCompileHlslToDxil not found in d3d12_xs.dll";
            return false;
        }

        static constexpr char kYuvCsHlsl[] = R"HLSL(
cbuffer Params : register(b0) {
    uint width;
    uint height;
    uint pitchY;
    uint pitchC;
    uint pitchA;
    uint hasAlpha;
    uint colorSpace;
    uint inputFormat;      // 0=Planar420_8, 1=NV12_8, 2=P010_10
    uint offY;
    uint offCb;
    uint offCr;
    uint offA;
    uint outputLinearScRgb; // 1 = FP16 scRGB (nits/80), 0 = normalized UNORM/PQ
    uint bilinearChroma;    // 1 = smooth bilinear chroma upsampling
    uint reserved0;
    uint reserved1;
};

ByteAddressBuffer srcBuf : register(t0);
RWTexture2D<float4> outRgba : register(u0);

float load_u8(uint byteOffset) {
    uint word = srcBuf.Load(byteOffset & ~3u);
    uint shift = (byteOffset & 3u) * 8u;
    return float((word >> shift) & 0xFFu) / 255.0;
}

float load_p010(uint byteOffset) {
    uint word = srcBuf.Load(byteOffset & ~3u);
    uint shift = (byteOffset & 2u) * 8u;
    uint val16 = (word >> shift) & 0xFFFFu;
    return float(val16 >> 6) / 1023.0;
}

float2 sample_chroma_point(uint cx, uint cy, uint cw, uint ch) {
    cx = min(cx, cw - 1u);
    cy = min(cy, ch - 1u);
    if (inputFormat == 1u) {
        uint off = offCb + cy * pitchC + cx * 2u;
        return float2(load_u8(off), load_u8(off + 1u));
    } else if (inputFormat == 2u) {
        uint off = offCb + cy * pitchC + cx * 4u;
        return float2(load_p010(off), load_p010(off + 2u));
    } else {
        uint off0 = offCb + cy * pitchC + cx;
        uint off1 = offCr + cy * pitchC + cx;
        return float2(load_u8(off0), load_u8(off1));
    }
}

float2 sample_chroma(uint2 px) {
    uint cw = max(1u, (width + 1u) >> 1);
    uint ch = max(1u, (height + 1u) >> 1);
    if (bilinearChroma == 0u || cw <= 1u || ch <= 1u) {
        return sample_chroma_point(px.x >> 1, px.y >> 1, cw, ch);
    }
    float fx = (float(px.x) + 0.5) * 0.5 - 0.5;
    float fy = (float(px.y) + 0.5) * 0.5 - 0.5;
    float cx0f = floor(max(0.0, fx));
    float cy0f = floor(max(0.0, fy));
    uint cx0 = uint(cx0f);
    uint cy0 = uint(cy0f);
    uint cx1 = min(cx0 + 1u, cw - 1u);
    uint cy1 = min(cy0 + 1u, ch - 1u);
    float tx = saturate(fx - cx0f);
    float ty = saturate(fy - cy0f);
    float2 c00 = sample_chroma_point(cx0, cy0, cw, ch);
    float2 c10 = sample_chroma_point(cx1, cy0, cw, ch);
    float2 c01 = sample_chroma_point(cx0, cy1, cw, ch);
    float2 c11 = sample_chroma_point(cx1, cy1, cw, ch);
    return lerp(lerp(c00, c10, tx), lerp(c01, c11, tx), ty);
}

// SMPTE ST 2084 PQ EOTF -> linear luminance in nits (0..10000)
float3 pq_eotf_nits(float3 pq) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 p = pow(max(pq, 0.0), 1.0 / m2);
    float3 num = max(p - c1, 0.0);
    float3 den = max(c2 - c3 * p, 1e-6);
    return pow(num / den, 1.0 / m1) * 10000.0;
}

float3 aces_filmic(float3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 linear_to_srgb(float3 lin) {
    float3 lo = lin * 12.92;
    float3 hi = 1.055 * pow(max(lin, 0.0), 1.0 / 2.4) - 0.055;
    return lerp(lo, hi, step(0.0031308, lin));
}

[RootSignature("RootFlags(0), RootConstants(num32BitConstants=16, b0), SRV(t0), DescriptorTable(UAV(u0, numDescriptors=1))")]
[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= width || dtid.y >= height) return;

    float yRaw = (inputFormat == 2u)
        ? load_p010(offY + dtid.y * pitchY + dtid.x * 2u)
        : load_u8(offY + dtid.y * pitchY + dtid.x);
    float2 cbcr = sample_chroma(dtid.xy);
    float cbRaw = cbcr.x;
    float crRaw = cbcr.y;
    float aVal = (hasAlpha != 0u) ? load_u8(offA + dtid.y * pitchA + dtid.x) : 1.0;

    float y, cb, cr;
    float3 rgb;
    if (colorSpace == 1u) {
        // BT.709 Full Range
        y  = yRaw;
        cb = cbRaw - (128.0 / 255.0);
        cr = crRaw - (128.0 / 255.0);
        rgb.r = y + 1.5748 * cr;
        rgb.g = y - 0.187324 * cb - 0.468124 * cr;
        rgb.b = y + 1.8556 * cb;
        rgb = saturate(rgb);
    } else if (colorSpace == 2u) {
        // BT.601 Limited Range
        y  = (yRaw  - (16.0  / 255.0)) * (255.0 / 219.0);
        cb = (cbRaw - (128.0 / 255.0)) * (255.0 / 224.0);
        cr = (crRaw - (128.0 / 255.0)) * (255.0 / 224.0);
        rgb.r = y + 1.402 * cr;
        rgb.g = y - 0.344136 * cb - 0.714136 * cr;
        rgb.b = y + 1.772 * cb;
        rgb = saturate(rgb);
    } else if (colorSpace == 3u || colorSpace == 4u) {
        // ITU-R BT.2020 NCL Limited Range (10-bit or 8-bit)
        float yBias = (inputFormat == 2u) ? (64.0 / 1023.0) : (16.0 / 255.0);
        float yScale = (inputFormat == 2u) ? (1023.0 / 876.0) : (255.0 / 219.0);
        float cBias = (inputFormat == 2u) ? (512.0 / 1023.0) : (128.0 / 255.0);
        float cScale = (inputFormat == 2u) ? (1023.0 / 896.0) : (255.0 / 224.0);
        y  = (yRaw  - yBias) * yScale;
        cb = (cbRaw - cBias) * cScale;
        cr = (crRaw - cBias) * cScale;
        float3 pq2020;
        pq2020.r = y + 1.4746 * cr;
        pq2020.g = y - 0.164553 * cb - 0.571353 * cr;
        pq2020.b = y + 1.8814 * cb;
        pq2020 = saturate(pq2020);

        if (colorSpace == 3u && outputLinearScRgb == 0u) {
            // Native HDR10 PQ passthrough (R10G10B10A2_UNORM)
            rgb = pq2020;
        } else {
            float3 nits2020 = pq_eotf_nits(pq2020);
            if (colorSpace == 3u) {
                // Linear scRGB output in 80-nit reference white units (R16G16B16A16_FLOAT)
                rgb = nits2020 / 80.0;
            } else {
                // BT.2020 -> BT.709 gamut matrix, then ACES Filmic SDR tone mapping (203-nit ref)
                float3 lin709;
                lin709.r =  1.6605 * nits2020.r - 0.5876 * nits2020.g - 0.0728 * nits2020.b;
                lin709.g = -0.1246 * nits2020.r + 1.1329 * nits2020.g - 0.0083 * nits2020.b;
                lin709.b = -0.0182 * nits2020.r - 0.1006 * nits2020.g + 1.1187 * nits2020.b;
                lin709 = max(lin709, 0.0) / 203.0;
                rgb = linear_to_srgb(aces_filmic(lin709));
            }
        }
    } else {
        // BT.709 Limited Range (Standard HD/4K Console Video / Bink2)
        y  = (yRaw  - (16.0  / 255.0)) * (255.0 / 219.0);
        cb = (cbRaw - (128.0 / 255.0)) * (255.0 / 224.0);
        cr = (crRaw - (128.0 / 255.0)) * (255.0 / 224.0);
        rgb.r = y + 1.5748 * cr;
        rgb.g = y - 0.187324 * cb - 0.468124 * cr;
        rgb.b = y + 1.8556 * cb;
        rgb = saturate(rgb);
    }

    outRgba[dtid.xy] = float4(rgb, saturate(aVal));
}
)HLSL";

        size_t dxil_sz = 0u;
        char err_buf[1024]{};
        const size_t src_len = std::strlen(kYuvCsHlsl);
        HRESULT hr = compiler(kYuvCsHlsl, src_len, L"main", L"cs_6_6",
                              nullptr, 0u, &dxil_sz, err_buf, sizeof(err_buf));
        if (FAILED(hr) || dxil_sz < 64u) {
            if (out_err) *out_err = std::string("DXIL size query failed: ") + err_buf;
            return false;
        }
        std::vector<uint8_t> dxil(dxil_sz);
        hr = compiler(kYuvCsHlsl, src_len, L"main", L"cs_6_6",
                      dxil.data(), dxil.size(), &dxil_sz, err_buf, sizeof(err_buf));
        if (FAILED(hr)) {
            if (out_err) *out_err = std::string("DXIL compile failed: ") + err_buf;
            return false;
        }
        dxil.resize(dxil_sz);

        device_ = device;
        hr = device_->CreateRootSignature(0u, dxil.data(), dxil.size(), IID_PPV_ARGS(&root_sig_));
        if (FAILED(hr)) {
            if (out_err) *out_err = "CreateRootSignature failed";
            shutdown();
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root_sig_.Get();
        pso_desc.CS.pShaderBytecode = dxil.data();
        pso_desc.CS.BytecodeLength = dxil.size();
        hr = device_->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso_));
        if (FAILED(hr)) {
            if (out_err) *out_err = "CreateComputePipelineState failed";
            shutdown();
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2u; // 1 descriptor per double-buffered slot
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&uav_heap_));
        if (FAILED(hr)) {
            if (out_err) *out_err = "CreateDescriptorHeap failed";
            shutdown();
            return false;
        }

        for (auto& s : slots_) {
            hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.cmd_alloc));
            if (FAILED(hr)) { shutdown(); return false; }
        }
        hr = device_->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, slots_[0].cmd_alloc.Get(),
                                        pso_.Get(), IID_PPV_ARGS(&cmd_list_));
        if (FAILED(hr)) { shutdown(); return false; }
        cmd_list_->Close();

        hr = device_->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        if (FAILED(hr)) { shutdown(); return false; }
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event_) { shutdown(); return false; }

        initialized_ = true;
        return true;
    }

    HRESULT convert_frame(
        ID3D12CommandQueue* queue,
        uint32_t width,
        uint32_t height,
        YuvInputFormat input_format,
        const void* y_plane,
        uint32_t pitch_y,
        const void* cb_or_uv_plane,
        const void* cr_plane,
        uint32_t pitch_c,
        const void* alpha_plane,
        uint32_t pitch_a,
        YuvColorSpace color_space,
        ID3D12Resource* dst_rgba_uav,
        D3D12_RESOURCE_STATES dst_state_before,
        D3D12_RESOURCE_STATES dst_state_after,
        bool bilinear_chroma = true,
        bool sync_wait = true) {
        if (!initialized_) return E_NOT_VALID_STATE;
        if (!queue || !y_plane || !cb_or_uv_plane || !dst_rgba_uav) return E_POINTER;
        if (input_format == YuvInputFormat::Planar420_8bit && !cr_plane) return E_POINTER;
        if (width == 0u || height == 0u) return E_INVALIDARG;

        const uint32_t ch_rows = (height + 1u) / 2u;
        const size_t y_bytes = static_cast<size_t>(pitch_y) * height;
        const size_t c_bytes = static_cast<size_t>(pitch_c) * ch_rows;
        const bool has_alpha = (alpha_plane != nullptr && pitch_a >= width);
        const size_t a_bytes = has_alpha ? (static_cast<size_t>(pitch_a) * height) : 4u;

        const uint64_t off_y = 0u;
        const uint64_t off_cb = align_up(off_y + y_bytes, 256u);
        const uint64_t off_cr = (input_format == YuvInputFormat::Planar420_8bit)
            ? align_up(off_cb + c_bytes, 256u)
            : off_cb;
        const uint64_t off_a = (input_format == YuvInputFormat::Planar420_8bit)
            ? align_up(off_cr + c_bytes, 256u)
            : align_up(off_cb + c_bytes, 256u);
        const uint64_t total_bytes = align_up(off_a + a_bytes, 256u);
        if (total_bytes > 0xFFFFFFFFull) return E_OUTOFMEMORY;

        const uint32_t sidx = slot_cursor_ & 1u;
        slot_cursor_ ^= 1u;
        FrameUploadSlot& slot = slots_[sidx];

        // Wait only on THIS double-buffered slot's previous GPU submission
        HRESULT hr = wait_fence(slot.fence_val);
        if (FAILED(hr)) return hr;

        hr = ensure_slot_buffer(slot, total_bytes);
        if (FAILED(hr)) return hr;

        void* mapped = nullptr;
        D3D12_RANGE rr{0, 0};
        hr = slot.upload_buf->Map(0, &rr, &mapped);
        if (FAILED(hr) || !mapped) return FAILED(hr) ? hr : E_FAIL;
        auto* base = static_cast<uint8_t*>(mapped);
        std::memcpy(base + off_y, y_plane, y_bytes);
        std::memcpy(base + off_cb, cb_or_uv_plane, c_bytes);
        if (input_format == YuvInputFormat::Planar420_8bit) {
            std::memcpy(base + off_cr, cr_plane, c_bytes);
        }
        if (has_alpha) {
            std::memcpy(base + off_a, alpha_plane, a_bytes);
        } else {
            const uint32_t opaque = 0xFFFFFFFFu;
            std::memcpy(base + off_a, &opaque, 4u);
        }
        D3D12_RANGE wr{0, static_cast<SIZE_T>(total_bytes)};
        slot.upload_buf->Unmap(0, &wr);

        const D3D12_RESOURCE_DESC dst_desc = dst_rgba_uav->GetDesc();
        const DXGI_FORMAT uav_fmt = (dst_desc.Format != DXGI_FORMAT_UNKNOWN)
            ? dst_desc.Format
            : DXGI_FORMAT_R8G8B8A8_UNORM;
        const bool is_scrgb_fp16 = (uav_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                                    uav_fmt == DXGI_FORMAT_R32G32B32A32_FLOAT);

        const UINT inc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav = uav_heap_->GetCPUDescriptorHandleForHeapStart();
        cpu_uav.ptr += static_cast<SIZE_T>(sidx) * inc;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = uav_heap_->GetGPUDescriptorHandleForHeapStart();
        gpu_uav.ptr += static_cast<UINT64>(sidx) * inc;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
        uavd.Format = uav_fmt;
        uavd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavd.Texture2D.MipSlice = 0u;
        device_->CreateUnorderedAccessView(dst_rgba_uav, nullptr, &uavd, cpu_uav);

        hr = slot.cmd_alloc->Reset();
        if (FAILED(hr)) return hr;
        hr = cmd_list_->Reset(slot.cmd_alloc.Get(), pso_.Get());
        if (FAILED(hr)) return hr;

        if (dst_state_before != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = dst_rgba_uav;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = dst_state_before;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            cmd_list_->ResourceBarrier(1u, &b);
        }

        ID3D12DescriptorHeap* heaps[] = { uav_heap_.Get() };
        cmd_list_->SetDescriptorHeaps(1u, heaps);
        cmd_list_->SetComputeRootSignature(root_sig_.Get());

        const uint32_t params[16] = {
            width, height, pitch_y, pitch_c,
            has_alpha ? pitch_a : 0u,
            has_alpha ? 1u : 0u,
            static_cast<uint32_t>(color_space),
            static_cast<uint32_t>(input_format),
            static_cast<uint32_t>(off_y),
            static_cast<uint32_t>(off_cb),
            static_cast<uint32_t>(off_cr),
            static_cast<uint32_t>(off_a),
            is_scrgb_fp16 ? 1u : 0u,
            bilinear_chroma ? 1u : 0u,
            0u, 0u
        };
        cmd_list_->SetComputeRoot32BitConstants(0u, 16u, params, 0u);
        cmd_list_->SetComputeRootShaderResourceView(1u, slot.upload_buf->GetGPUVirtualAddress());
        cmd_list_->SetComputeRootDescriptorTable(2u, gpu_uav);

        cmd_list_->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1u);

        D3D12_RESOURCE_BARRIER post[2]{};
        UINT num_post = 0u;
        post[num_post].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        post[num_post].UAV.pResource = dst_rgba_uav;
        ++num_post;
        if (dst_state_after != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            post[num_post].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            post[num_post].Transition.pResource = dst_rgba_uav;
            post[num_post].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            post[num_post].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            post[num_post].Transition.StateAfter = dst_state_after;
            ++num_post;
        }
        cmd_list_->ResourceBarrier(num_post, post);

        hr = cmd_list_->Close();
        if (FAILED(hr)) return hr;

        ID3D12CommandList* lists[] = { cmd_list_.Get() };
        queue->ExecuteCommandLists(1u, lists);
        ++fence_value_;
        slot.fence_val = fence_value_;
        hr = queue->Signal(fence_.Get(), fence_value_);
        if (FAILED(hr)) return hr;
        ++frames_converted_;
        if (sync_wait) {
            return wait_fence(fence_value_);
        }
        return S_OK;
    }

    HRESULT flush_async() noexcept {
        return wait_fence(fence_value_);
    }

    // Backward-compatible wrapper for existing planar 4:2:0 callers
    HRESULT convert_planar_420(
        ID3D12CommandQueue* queue,
        uint32_t width,
        uint32_t height,
        const uint8_t* y_plane,
        uint32_t pitch_y,
        const uint8_t* cb_plane,
        const uint8_t* cr_plane,
        uint32_t pitch_c,
        const uint8_t* alpha_plane,
        uint32_t pitch_a,
        YuvColorSpace color_space,
        ID3D12Resource* dst_rgba_uav,
        D3D12_RESOURCE_STATES dst_state_before,
        D3D12_RESOURCE_STATES dst_state_after) {
        return convert_frame(
            queue, width, height, YuvInputFormat::Planar420_8bit,
            y_plane, pitch_y, cb_plane, cr_plane, pitch_c, alpha_plane, pitch_a,
            color_space, dst_rgba_uav, dst_state_before, dst_state_after,
            true, true);
    }

    HRESULT convert_nv12(
        ID3D12CommandQueue* queue,
        uint32_t width,
        uint32_t height,
        const uint8_t* y_plane,
        uint32_t pitch_y,
        const uint8_t* uv_plane,
        uint32_t pitch_uv,
        YuvColorSpace color_space,
        ID3D12Resource* dst_rgba_uav,
        D3D12_RESOURCE_STATES dst_state_before,
        D3D12_RESOURCE_STATES dst_state_after,
        bool sync_wait = true) {
        return convert_frame(
            queue, width, height, YuvInputFormat::SemiPlanarNV12_8bit,
            y_plane, pitch_y, uv_plane, nullptr, pitch_uv, nullptr, 0u,
            color_space, dst_rgba_uav, dst_state_before, dst_state_after,
            true, sync_wait);
    }

    HRESULT convert_p010(
        ID3D12CommandQueue* queue,
        uint32_t width,
        uint32_t height,
        const uint16_t* y_plane,
        uint32_t pitch_y_bytes,
        const uint16_t* uv_plane,
        uint32_t pitch_uv_bytes,
        YuvColorSpace color_space,
        ID3D12Resource* dst_rgba_uav,
        D3D12_RESOURCE_STATES dst_state_before,
        D3D12_RESOURCE_STATES dst_state_after,
        bool sync_wait = true) {
        return convert_frame(
            queue, width, height, YuvInputFormat::SemiPlanarP010_10bit,
            y_plane, pitch_y_bytes, uv_plane, nullptr, pitch_uv_bytes, nullptr, 0u,
            color_space, dst_rgba_uav, dst_state_before, dst_state_after,
            true, sync_wait);
    }
};

} // namespace wsx_video
