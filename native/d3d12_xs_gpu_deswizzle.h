// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Pure Direct3D 12 SM 6.6 GPU Compute Deswizzler for Xbox Series X|S Console Tiled Textures:
// - Converts raw 4KB_Z/S/D, 64KB_Z/S/D, 64KB_*_T, and RDNA2 64KB_*_X (with pipe_bank_xor)
//   guest texture uploads directly on the GPU into D3D12 CopyTextureRegion linear footprints.
// - Supports 1, 2, 4, 8, and 16 bytes-per-element (including all BC1..BC7 block-compressed formats
//   and non-UAV formats such as *_SRGB / *_TYPELESS) with zero thread write races.

#pragma once

#include "d3d12_xs_sm66_runtime.h"
#include "wsx_config.h"
#include "wsx_xg_layout.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace wsx_d3d12 {

using Microsoft::WRL::ComPtr;

struct GpuDeswizzleStats {
    uint64_t dispatches = 0;
    uint64_t batch_calls = 0;
    uint64_t bytes_deswizzled = 0;
};

struct GpuDeswizzleSubresourceJob {
    uint64_t src_byte_offset = 0;
    uint64_t dst_byte_offset = 0;
    wsx_xg::SubresourceLayout layout{};
    uint32_t dst_row_pitch = 0;
    uint32_t pipe_bank_xor = 0;
};

class GpuTextureDeswizzler {
    std::mutex init_mtx_;
    bool init_attempted_ = false;
    bool available_ = false;
    ComPtr<ID3D12RootSignature> root_sig_;
    ComPtr<ID3D12PipelineState> pso_;
    std::atomic<uint64_t> dispatches_{0};
    std::atomic<uint64_t> batch_calls_{0};
    std::atomic<uint64_t> bytes_deswizzled_{0};

    static bool ensure_initialized(GpuTextureDeswizzler& self, ID3D12Device* raw_device) noexcept {
        if (!raw_device) return false;
        std::lock_guard<std::mutex> lk(self.init_mtx_);
        if (self.init_attempted_) return self.available_;
        self.init_attempted_ = true;

        static constexpr char kDeswizzleCs[] = R"HLSL(
cbuffer Params : register(b0) {
    uint widthElems;
    uint heightElems;
    uint paddedWidthElems;
    uint tileW;
    uint tileH;
    uint bpe;
    uint swizzleMode;
    uint dstRowPitch;
    uint srcBaseBytes;
    uint dstBaseBytes;
    uint pipeBankXor;
    uint reserved;
};

ByteAddressBuffer srcBuf : register(t0);
RWByteAddressBuffer dstBuf : register(u0);

uint morton_rect(uint lx, uint ly, uint tw, uint th) {
    uint idx = 0u;
    uint outBit = 0u;
    uint bx = 0u;
    uint by = 0u;
    [loop]
    while ((1u << bx) < tw || (1u << by) < th) {
        if ((1u << bx) < tw) {
            idx |= (((lx >> bx) & 1u) << outBit);
            outBit++;
            bx++;
        }
        if ((1u << by) < th) {
            idx |= (((ly >> by) & 1u) << outBit);
            outBit++;
            by++;
        }
    }
    return idx;
}

uint elem_src_offset(uint ex, uint ey) {
    uint tx = ex / tileW;
    uint ty = ey / tileH;
    uint lx = ex - tx * tileW;
    uint ly = ey - ty * tileH;
    uint tilesPerRow = paddedWidthElems / tileW;
    uint tileBytes = tileW * tileH * bpe;
    uint tileIdx = ty * tilesPerRow + tx;
    uint inTile = morton_rect(lx, ly, tileW, tileH) * bpe;
    if (swizzleMode == 24u || swizzleMode == 25u || swizzleMode == 26u) {
        uint xorBits = ((tx & 0x3Fu) ^ ((ty & 0x3Fu) << 1) ^ (pipeBankXor & 0x3Fu)) & 0x3Fu;
        inTile ^= (xorBits << 8);
    }
    return srcBaseBytes + tileIdx * tileBytes + inTile;
}

uint load_byte(uint off) {
    uint w = srcBuf.Load(off & ~3u);
    return (w >> ((off & 3u) * 8u)) & 0xFFu;
}

uint load_u16(uint off) {
    uint w = srcBuf.Load(off & ~3u);
    return (w >> ((off & 2u) * 8u)) & 0xFFFFu;
}

[RootSignature("RootFlags(0), RootConstants(num32BitConstants=12, b0), SRV(t0), UAV(u0)")]
[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint ey = dtid.y;
    if (ey >= heightElems) return;

    if (bpe == 4u) {
        uint ex = dtid.x;
        if (ex >= widthElems) return;
        uint s = elem_src_offset(ex, ey);
        uint d = dstBaseBytes + ey * dstRowPitch + ex * 4u;
        dstBuf.Store(d, srcBuf.Load(s));
    } else if (bpe == 8u) {
        uint ex = dtid.x;
        if (ex >= widthElems) return;
        uint s = elem_src_offset(ex, ey);
        uint d = dstBaseBytes + ey * dstRowPitch + ex * 8u;
        dstBuf.Store2(d, srcBuf.Load2(s));
    } else if (bpe == 16u) {
        uint ex = dtid.x;
        if (ex >= widthElems) return;
        uint s = elem_src_offset(ex, ey);
        uint d = dstBaseBytes + ey * dstRowPitch + ex * 16u;
        dstBuf.Store4(d, srcBuf.Load4(s));
    } else if (bpe == 2u) {
        uint ex0 = dtid.x * 2u;
        if (ex0 >= widthElems) return;
        uint v0 = load_u16(elem_src_offset(ex0, ey));
        uint v1 = (ex0 + 1u < widthElems) ? load_u16(elem_src_offset(ex0 + 1u, ey)) : 0u;
        uint d = dstBaseBytes + ey * dstRowPitch + dtid.x * 4u;
        dstBuf.Store(d, v0 | (v1 << 16));
    } else if (bpe == 1u) {
        uint ex0 = dtid.x * 4u;
        if (ex0 >= widthElems) return;
        uint b0 = load_byte(elem_src_offset(ex0, ey));
        uint b1 = (ex0 + 1u < widthElems) ? load_byte(elem_src_offset(ex0 + 1u, ey)) : 0u;
        uint b2 = (ex0 + 2u < widthElems) ? load_byte(elem_src_offset(ex0 + 2u, ey)) : 0u;
        uint b3 = (ex0 + 3u < widthElems) ? load_byte(elem_src_offset(ex0 + 3u, ey)) : 0u;
        uint d = dstBaseBytes + ey * dstRowPitch + dtid.x * 4u;
        dstBuf.Store(d, b0 | (b1 << 8) | (b2 << 16) | (b3 << 24));
    }
}
)HLSL";

        std::vector<uint8_t> dxil;
        char err[512]{};
        const HRESULT chr = wsx_sm66::compile_hlsl_to_dxil_vec(
            kDeswizzleCs, std::strlen(kDeswizzleCs), L"main", L"cs_6_6", dxil, err, sizeof(err));
        if (FAILED(chr) || dxil.size() < 64u) {
            return false;
        }
        if (FAILED(raw_device->CreateRootSignature(0u, dxil.data(), dxil.size(), IID_PPV_ARGS(&self.root_sig_)))) {
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = self.root_sig_.Get();
        pd.CS.pShaderBytecode = dxil.data();
        pd.CS.BytecodeLength = dxil.size();
        if (FAILED(raw_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&self.pso_)))) {
            self.root_sig_.Reset();
            return false;
        }
        self.available_ = true;
        return true;
    }

public:
    static bool mode_enabled_for_size(uint64_t total_bytes) noexcept {
        const std::string cfg = wsx::get_config_string("WSX_GPU_DESWIZZLE", "Graphics", "gpu_deswizzle", "1");
        if (cfg == "0" || cfg == "false" || cfg == "off") return false;
        if (cfg == "force" || cfg == "always" || cfg == "2") return true;
        // Default: accelerate textures >= 16 KiB on GPU; tiny mips/icons stay zero-overhead on CPU
        return total_bytes >= 16384u;
    }

    static bool supports_layout(const wsx_xg::SubresourceLayout& sl) noexcept {
        if (wsx_xg::is_linear_mode(sl.swizzle_mode)) return false;
        if (sl.bytes_per_element != 1u && sl.bytes_per_element != 2u &&
            sl.bytes_per_element != 4u && sl.bytes_per_element != 8u &&
            sl.bytes_per_element != 16u) {
            return false;
        }
        return sl.tile_w > 0u && sl.tile_h > 0u &&
               sl.padded_width_elems >= sl.tile_w &&
               (sl.padded_width_elems % sl.tile_w) == 0u;
    }

    bool ready(ID3D12Device* raw_device) noexcept {
        return ensure_initialized(*this, raw_device);
    }

    void reset() noexcept {
        std::lock_guard<std::mutex> lk(init_mtx_);
        pso_.Reset();
        root_sig_.Reset();
        init_attempted_ = false;
        available_ = false;
    }

    // Assumes `cmd` is an open DIRECT command list, `src_upload_buf` is in GENERIC_READ,
    // and `dst_linear_buf` is in UNORDERED_ACCESS. Caller transitions `dst_linear_buf` -> COPY_SOURCE after batch.
    bool record_deswizzle_2d(
        ID3D12GraphicsCommandList* cmd,
        ID3D12Resource* src_upload_buf,
        uint64_t src_byte_offset,
        ID3D12Resource* dst_linear_buf,
        uint64_t dst_byte_offset,
        const wsx_xg::SubresourceLayout& sl,
        uint32_t dst_row_pitch,
        uint32_t pipe_bank_xor = 0u) noexcept {
        if (!available_ || !cmd || !src_upload_buf || !dst_linear_buf) return false;
        if (!supports_layout(sl)) return false;
        if ((src_byte_offset & 3u) != 0u || (dst_byte_offset & 3u) != 0u || (dst_row_pitch & 3u) != 0u) {
            return false;
        }
        if (src_byte_offset > 0xFFFFFFFFull || dst_byte_offset > 0xFFFFFFFFull) {
            return false;
        }

        const uint32_t params[12] = {
            sl.width_elems,
            sl.height_elems,
            sl.padded_width_elems,
            sl.tile_w,
            sl.tile_h,
            sl.bytes_per_element,
            sl.swizzle_mode,
            dst_row_pitch,
            static_cast<uint32_t>(src_byte_offset),
            static_cast<uint32_t>(dst_byte_offset),
            pipe_bank_xor,
            0u
        };

        uint32_t work_x = sl.width_elems;
        if (sl.bytes_per_element == 1u) work_x = (sl.width_elems + 3u) / 4u;
        else if (sl.bytes_per_element == 2u) work_x = (sl.width_elems + 1u) / 2u;

        cmd->SetComputeRootSignature(root_sig_.Get());
        cmd->SetPipelineState(pso_.Get());
        cmd->SetComputeRoot32BitConstants(0u, 12u, params, 0u);
        cmd->SetComputeRootShaderResourceView(1u, src_upload_buf->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(2u, dst_linear_buf->GetGPUVirtualAddress());
        cmd->Dispatch((work_x + 7u) / 8u, (sl.height_elems + 7u) / 8u, 1u);
        dispatches_.fetch_add(1u, std::memory_order_relaxed);
        bytes_deswizzled_.fetch_add(static_cast<uint64_t>(sl.width_elems) * sl.height_elems * sl.bytes_per_element,
                                    std::memory_order_relaxed);
        return true;
    }

    // Batched multi-mip / multi-subresource GPU deswizzle: binds RootSignature, PSO & buffer SRV/UAV ONCE!
    uint32_t record_deswizzle_batch(
        ID3D12GraphicsCommandList* cmd,
        ID3D12Resource* src_upload_buf,
        ID3D12Resource* dst_linear_buf,
        const GpuDeswizzleSubresourceJob* jobs,
        uint32_t job_count) noexcept {
        if (!available_ || !cmd || !src_upload_buf || !dst_linear_buf || !jobs || job_count == 0u) return 0u;

        bool bound = false;
        uint32_t dispatched = 0u;
        uint64_t total_bytes = 0u;
        const D3D12_GPU_VIRTUAL_ADDRESS src_va = src_upload_buf->GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS dst_va = dst_linear_buf->GetGPUVirtualAddress();

        for (uint32_t i = 0u; i < job_count; ++i) {
            const auto& j = jobs[i];
            const auto& sl = j.layout;
            if (!supports_layout(sl)) continue;
            if ((j.src_byte_offset & 3u) != 0u || (j.dst_byte_offset & 3u) != 0u || (j.dst_row_pitch & 3u) != 0u) continue;
            if (j.src_byte_offset > 0xFFFFFFFFull || j.dst_byte_offset > 0xFFFFFFFFull) continue;

            if (!bound) {
                cmd->SetComputeRootSignature(root_sig_.Get());
                cmd->SetPipelineState(pso_.Get());
                cmd->SetComputeRootShaderResourceView(1u, src_va);
                cmd->SetComputeRootUnorderedAccessView(2u, dst_va);
                bound = true;
            }
            const uint32_t params[12] = {
                sl.width_elems,
                sl.height_elems,
                sl.padded_width_elems,
                sl.tile_w,
                sl.tile_h,
                sl.bytes_per_element,
                sl.swizzle_mode,
                j.dst_row_pitch,
                static_cast<uint32_t>(j.src_byte_offset),
                static_cast<uint32_t>(j.dst_byte_offset),
                j.pipe_bank_xor,
                0u
            };
            uint32_t work_x = sl.width_elems;
            if (sl.bytes_per_element == 1u) work_x = (sl.width_elems + 3u) / 4u;
            else if (sl.bytes_per_element == 2u) work_x = (sl.width_elems + 1u) / 2u;

            cmd->SetComputeRoot32BitConstants(0u, 12u, params, 0u);
            cmd->Dispatch((work_x + 7u) / 8u, (sl.height_elems + 7u) / 8u, 1u);
            ++dispatched;
            total_bytes += static_cast<uint64_t>(sl.width_elems) * sl.height_elems * sl.bytes_per_element;
        }
        if (dispatched > 0u) {
            dispatches_.fetch_add(dispatched, std::memory_order_relaxed);
            batch_calls_.fetch_add(1u, std::memory_order_relaxed);
            bytes_deswizzled_.fetch_add(total_bytes, std::memory_order_relaxed);
        }
        return dispatched;
    }

    GpuDeswizzleStats get_stats() const noexcept {
        return GpuDeswizzleStats{
            dispatches_.load(std::memory_order_relaxed),
            batch_calls_.load(std::memory_order_relaxed),
            bytes_deswizzled_.load(std::memory_order_relaxed)
        };
    }
};

} // namespace wsx_d3d12
