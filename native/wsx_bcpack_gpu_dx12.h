// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Direct3D 12 SM 6.6 GPU Compute BCPack Texture Decompressor:
// - Accelerates planar delta endpoint reconstruction + BC1/BC3/BC4/BC5/BC7 block interleaving
//   directly on the GPU using a 3-pass work-efficient parallel prefix-sum & coalesced Store2/Store4 pipeline:
//     Pass 1 (BcPackScanTilesCS)   : 256-block threadgroup parallel inclusive scan per tile
//     Pass 2 (BcPackScanOffsetsCS) : Exclusive scan across per-tile delta sums
//     Pass 3 (BcPackMergeBlocksCS) : Coalesced parallel BC1/BC3/BC4/BC5/BC7 block assembly -> VRAM
// - 100% bit-exact with wsx_bcpack::decompress (verified by IEEE CRC32 integrity check).
// - Zero external dependencies; compiles SM 6.6 DXIL in-process via D3D12XboxCompileHlslToDxil.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "wsx_bcpack.h"

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace wsx_bcpack_gpu {

struct GpuBcPackStats {
    uint64_t dispatches = 0;
    uint64_t blocks_decoded = 0;
    uint64_t bytes_decoded = 0;
};

inline std::atomic<uint64_t>& global_dispatches() noexcept { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& global_blocks() noexcept     { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& global_bytes() noexcept      { static std::atomic<uint64_t> v{0}; return v; }

inline GpuBcPackStats get_global_stats() noexcept {
    return GpuBcPackStats{
        global_dispatches().load(std::memory_order_relaxed),
        global_blocks().load(std::memory_order_relaxed),
        global_bytes().load(std::memory_order_relaxed)
    };
}

class WsxBcPackGpuDecoder {
    using CompileHlslFn = HRESULT(WINAPI*)(
        const char*, size_t, const wchar_t*, const wchar_t*,
        void*, size_t, size_t*, char*, size_t);

    std::mutex mutex_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> cmd_alloc_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmd_list_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_sig_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_scan_tiles_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_scan_offsets_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_merge_blocks_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    UINT64 fence_value_ = 0u;
    bool initialized_ = false;

    static CompileHlslFn resolve_dxil_compiler() noexcept {
        HMODULE mod = GetModuleHandleW(L"d3d12_xs.dll");
        if (!mod) mod = LoadLibraryW(L"d3d12_xs.dll");
        if (!mod) return nullptr;
        return reinterpret_cast<CompileHlslFn>(GetProcAddress(mod, "D3D12XboxCompileHlslToDxil"));
    }

    static HRESULT create_buffer(
        ID3D12Device* dev,
        D3D12_HEAP_TYPE heap_type,
        UINT64 size_bytes,
        D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES initial_state,
        Microsoft::WRL::ComPtr<ID3D12Resource>& out_res) noexcept {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap_type;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (std::max<UINT64>(16u, size_bytes) + 15u) & ~15ull;
        rd.Height = 1u;
        rd.DepthOrArraySize = 1u;
        rd.MipLevels = 1u;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1u;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        return dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, initial_state, nullptr, IID_PPV_ARGS(&out_res));
    }

    void shutdown_locked() noexcept {
        initialized_ = false;
        if (fence_event_) {
            CloseHandle(fence_event_);
            fence_event_ = nullptr;
        }
        fence_.Reset();
        pso_merge_blocks_.Reset();
        pso_scan_offsets_.Reset();
        pso_scan_tiles_.Reset();
        root_sig_.Reset();
        cmd_list_.Reset();
        cmd_alloc_.Reset();
        queue_.Reset();
        device_.Reset();
    }

public:
    WsxBcPackGpuDecoder() = default;
    ~WsxBcPackGpuDecoder() {
        std::lock_guard<std::mutex> lk(mutex_);
        shutdown_locked();
    }

    WsxBcPackGpuDecoder(const WsxBcPackGpuDecoder&) = delete;
    WsxBcPackGpuDecoder& operator=(const WsxBcPackGpuDecoder&) = delete;

    bool ensure_initialized(ID3D12Device* device, std::string* out_err = nullptr) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (initialized_ && device_.Get() == device) return true;
        shutdown_locked();
        if (!device) {
            if (out_err) *out_err = "null ID3D12Device";
            return false;
        }
        const CompileHlslFn compiler = resolve_dxil_compiler();
        if (!compiler) {
            if (out_err) *out_err = "D3D12XboxCompileHlslToDxil not found";
            return false;
        }

        // Pure Direct3D 12 SM 6.6 Parallel BCPack Planar Prefix-Sum + Block Interleaver
        // Supports BC1 (kind=1), BC3 (kind=3), BC4 (kind=4), BC5 (kind=5), BC7 (kind=7)
        // Each block has up to 8 delta lanes stored in two uint4s (lanes 0..7):
        //   BC1: lane0=ep0(u16), lane1=ep1(u16)
        //   BC4: lane0=ep0(u8),  lane1=ep1(u8)
        //   BC3: lane0=a0(u8),   lane1=a1(u8), lane2=c0(u16), lane3=c1(u16)
        //   BC5: lane0=r0(u8),   lane1=r1(u8), lane2=g0(u8),  lane3=g1(u8)
        //   BC7: lane0..7 = byte planes 0..7 (u8)
        static constexpr char kBcPackCsHlsl[] = R"HLSL(
cbuffer Params : register(b0) {
    uint numBlocks;
    uint bcKind;
    uint numTiles;
    uint numLanes;
};

ByteAddressBuffer     planarBuf   : register(t0);
RWByteAddressBuffer   scanBuf     : register(u0); // Per-block scanned delta lanes (numBlocks * 8 * 4 bytes)
RWByteAddressBuffer   tileSumBuf  : register(u1); // Per-tile total / exclusive offset (numTiles * 8 * 4 bytes)
RWByteAddressBuffer   dstBlockBuf : register(u2); // Final linear BC1/BC3/BC4/BC5/BC7 blocks

uint load_u8(uint off) {
    uint w = planarBuf.Load(off & ~3u);
    return (w >> ((off & 3u) * 8u)) & 0xFFu;
}

uint load_u16(uint off) {
    uint lo = load_u8(off);
    uint hi = load_u8(off + 1u);
    return lo | (hi << 8);
}

uint load_delta_lane(uint blk, uint lane) {
    if (bcKind == 1u) { // BC1: 2x u16 endpoints
        if (lane == 0u) return load_u16(blk * 2u);
        if (lane == 1u) return load_u16(numBlocks * 2u + blk * 2u);
    } else if (bcKind == 4u) { // BC4: 2x u8 endpoints
        if (lane == 0u) return load_u8(blk);
        if (lane == 1u) return load_u8(numBlocks + blk);
    } else if (bcKind == 3u) { // BC3: 2x u8 alpha + 2x u16 color
        if (lane == 0u) return load_u8(blk);
        if (lane == 1u) return load_u8(numBlocks + blk);
        if (lane == 2u) return load_u16(numBlocks * 2u + blk * 2u);
        if (lane == 3u) return load_u16(numBlocks * 4u + blk * 2u);
    } else if (bcKind == 5u) { // BC5: 4x u8 endpoints
        if (lane < 4u) return load_u8(lane * numBlocks + blk);
    } else if (bcKind == 7u) { // BC7: 8x u8 delta planes 0..7
        if (lane < 8u) return load_u8(lane * numBlocks + blk);
    }
    return 0u;
}

groupshared uint gs_val[256];

[RootSignature("RootFlags(0), RootConstants(num32BitConstants=4, b0), SRV(t0), UAV(u0), UAV(u1), UAV(u2)")]
[numthreads(256, 1, 1)]
void BcPackScanTilesCS(uint3 gid : SV_GroupID, uint gtid : SV_GroupThreadID) {
    uint blk = gid.x * 256u + gtid;
    for (uint lane = 0u; lane < numLanes; ++lane) {
        uint v = (blk < numBlocks) ? load_delta_lane(blk, lane) : 0u;
        gs_val[gtid] = v;
        GroupMemoryBarrierWithGroupSync();

        // Work-efficient inclusive Hillis-Steele prefix scan inside 256-block tile
        [unroll]
        for (uint step = 1u; step < 256u; step <<= 1u) {
            uint addv = (gtid >= step) ? gs_val[gtid - step] : 0u;
            GroupMemoryBarrierWithGroupSync();
            gs_val[gtid] += addv;
            GroupMemoryBarrierWithGroupSync();
        }

        if (blk < numBlocks) {
            scanBuf.Store((blk * 8u + lane) * 4u, gs_val[gtid]);
        }
        if (gtid == 255u) {
            tileSumBuf.Store((gid.x * 8u + lane) * 4u, gs_val[255]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

[RootSignature("RootFlags(0), RootConstants(num32BitConstants=4, b0), SRV(t0), UAV(u0), UAV(u1), UAV(u2)")]
[numthreads(8, 1, 1)]
void BcPackScanOffsetsCS(uint3 tid : SV_DispatchThreadID) {
    uint lane = tid.x;
    if (lane >= numLanes) return;
    uint running = 0u;
    for (uint t = 0u; t < numTiles; ++t) {
        uint off = (t * 8u + lane) * 4u;
        uint cur = tileSumBuf.Load(off);
        tileSumBuf.Store(off, running);
        running += cur;
    }
}

[RootSignature("RootFlags(0), RootConstants(num32BitConstants=4, b0), SRV(t0), UAV(u0), UAV(u1), UAV(u2)")]
[numthreads(64, 1, 1)]
void BcPackMergeBlocksCS(uint3 dtid : SV_DispatchThreadID) {
    uint blk = dtid.x;
    if (blk >= numBlocks) return;
    uint tile = blk >> 8u;
    uint sbase = blk * 32u;
    uint tbase = tile * 32u;

    if (bcKind == 1u) {
        // BC1 (8B): u16 c0 | (u16 c1 << 16), 4B indices
        uint c0 = (scanBuf.Load(sbase + 0u) + tileSumBuf.Load(tbase + 0u)) & 0xFFFFu;
        uint c1 = (scanBuf.Load(sbase + 4u) + tileSumBuf.Load(tbase + 4u)) & 0xFFFFu;
        uint ioff = numBlocks * 4u + blk * 4u;
        uint idx = load_u8(ioff) | (load_u8(ioff + 1u) << 8) | (load_u8(ioff + 2u) << 16) | (load_u8(ioff + 3u) << 24);
        dstBlockBuf.Store2(blk * 8u, uint2(c0 | (c1 << 16), idx));
    } else if (bcKind == 4u) {
        // BC4 (8B): u8 ep0, u8 ep1, 6B indices
        uint e0 = (scanBuf.Load(sbase + 0u) + tileSumBuf.Load(tbase + 0u)) & 0xFFu;
        uint e1 = (scanBuf.Load(sbase + 4u) + tileSumBuf.Load(tbase + 4u)) & 0xFFu;
        uint ioff = numBlocks * 2u + blk * 6u;
        uint w0 = e0 | (e1 << 8) | (load_u8(ioff + 0u) << 16) | (load_u8(ioff + 1u) << 24);
        uint w1 = load_u8(ioff + 2u) | (load_u8(ioff + 3u) << 8) | (load_u8(ioff + 4u) << 16) | (load_u8(ioff + 5u) << 24);
        dstBlockBuf.Store2(blk * 8u, uint2(w0, w1));
    } else if (bcKind == 3u) {
        // BC3 (16B): u8 a0, u8 a1, 6B alpha idx, u16 c0, u16 c1, 4B color idx
        uint a0 = (scanBuf.Load(sbase + 0u)  + tileSumBuf.Load(tbase + 0u))  & 0xFFu;
        uint a1 = (scanBuf.Load(sbase + 4u)  + tileSumBuf.Load(tbase + 4u))  & 0xFFu;
        uint c0 = (scanBuf.Load(sbase + 8u)  + tileSumBuf.Load(tbase + 8u))  & 0xFFFFu;
        uint c1 = (scanBuf.Load(sbase + 12u) + tileSumBuf.Load(tbase + 12u)) & 0xFFFFu;
        uint aoff = numBlocks * 6u + blk * 6u;
        uint coff = numBlocks * 12u + blk * 4u;
        uint w0 = a0 | (a1 << 8) | (load_u8(aoff + 0u) << 16) | (load_u8(aoff + 1u) << 24);
        uint w1 = load_u8(aoff + 2u) | (load_u8(aoff + 3u) << 8) | (load_u8(aoff + 4u) << 16) | (load_u8(aoff + 5u) << 24);
        uint w2 = c0 | (c1 << 16);
        uint w3 = load_u8(coff + 0u) | (load_u8(coff + 1u) << 8) | (load_u8(coff + 2u) << 16) | (load_u8(coff + 3u) << 24);
        dstBlockBuf.Store4(blk * 16u, uint4(w0, w1, w2, w3));
    } else if (bcKind == 5u) {
        // BC5 (16B): u8 r0, u8 r1, 6B ridx, u8 g0, u8 g1, 6B gidx
        uint r0 = (scanBuf.Load(sbase + 0u)  + tileSumBuf.Load(tbase + 0u))  & 0xFFu;
        uint r1 = (scanBuf.Load(sbase + 4u)  + tileSumBuf.Load(tbase + 4u))  & 0xFFu;
        uint g0 = (scanBuf.Load(sbase + 8u)  + tileSumBuf.Load(tbase + 8u))  & 0xFFu;
        uint g1 = (scanBuf.Load(sbase + 12u) + tileSumBuf.Load(tbase + 12u)) & 0xFFu;
        uint roff = numBlocks * 4u + blk * 6u;
        uint goff = numBlocks * 10u + blk * 6u;
        uint w0 = r0 | (r1 << 8) | (load_u8(roff + 0u) << 16) | (load_u8(roff + 1u) << 24);
        uint w1 = load_u8(roff + 2u) | (load_u8(roff + 3u) << 8) | (load_u8(roff + 4u) << 16) | (load_u8(roff + 5u) << 24);
        uint w2 = g0 | (g1 << 8) | (load_u8(goff + 0u) << 16) | (load_u8(goff + 1u) << 24);
        uint w3 = load_u8(goff + 2u) | (load_u8(goff + 3u) << 8) | (load_u8(goff + 4u) << 16) | (load_u8(goff + 5u) << 24);
        dstBlockBuf.Store4(blk * 16u, uint4(w0, w1, w2, w3));
    } else if (bcKind == 7u) {
        // BC7 (16B): planes 0..7 delta-reconstructed, planes 8..15 raw byte transpose
        uint b0 = (scanBuf.Load(sbase + 0u)  + tileSumBuf.Load(tbase + 0u))  & 0xFFu;
        uint b1 = (scanBuf.Load(sbase + 4u)  + tileSumBuf.Load(tbase + 4u))  & 0xFFu;
        uint b2 = (scanBuf.Load(sbase + 8u)  + tileSumBuf.Load(tbase + 8u))  & 0xFFu;
        uint b3 = (scanBuf.Load(sbase + 12u) + tileSumBuf.Load(tbase + 12u)) & 0xFFu;
        uint b4 = (scanBuf.Load(sbase + 16u) + tileSumBuf.Load(tbase + 16u)) & 0xFFu;
        uint b5 = (scanBuf.Load(sbase + 20u) + tileSumBuf.Load(tbase + 20u)) & 0xFFu;
        uint b6 = (scanBuf.Load(sbase + 24u) + tileSumBuf.Load(tbase + 24u)) & 0xFFu;
        uint b7 = (scanBuf.Load(sbase + 28u) + tileSumBuf.Load(tbase + 28u)) & 0xFFu;
        uint w0 = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
        uint w1 = b4 | (b5 << 8) | (b6 << 16) | (b7 << 24);
        uint w2 = load_u8(8u  * numBlocks + blk) | (load_u8(9u  * numBlocks + blk) << 8) |
                  (load_u8(10u * numBlocks + blk) << 16) | (load_u8(11u * numBlocks + blk) << 24);
        uint w3 = load_u8(12u * numBlocks + blk) | (load_u8(13u * numBlocks + blk) << 8) |
                  (load_u8(14u * numBlocks + blk) << 16) | (load_u8(15u * numBlocks + blk) << 24);
        dstBlockBuf.Store4(blk * 16u, uint4(w0, w1, w2, w3));
    }
}
)HLSL";

        auto compile_cs = [&](const wchar_t* entry, std::vector<uint8_t>& out_dxil) -> bool {
            out_dxil.resize(512u * 1024u);
            size_t out_sz = 0u;
            char err_buf[1024]{};
            const HRESULT hr = compiler(kBcPackCsHlsl, std::strlen(kBcPackCsHlsl), entry, L"cs_6_6",
                                        out_dxil.data(), out_dxil.size(), &out_sz, err_buf, sizeof(err_buf));
            if (FAILED(hr) || out_sz < 64u) {
                if (out_err) *out_err = err_buf[0] ? err_buf : "SM 6.6 BCPack CS compile failed";
                return false;
            }
            out_dxil.resize(out_sz);
            return true;
        };

        std::vector<uint8_t> dxil_tiles, dxil_offs, dxil_merge;
        if (!compile_cs(L"BcPackScanTilesCS", dxil_tiles) ||
            !compile_cs(L"BcPackScanOffsetsCS", dxil_offs) ||
            !compile_cs(L"BcPackMergeBlocksCS", dxil_merge)) {
            return false;
        }

        if (FAILED(device->CreateRootSignature(0u, dxil_tiles.data(), dxil_tiles.size(), IID_PPV_ARGS(&root_sig_)))) {
            if (out_err) *out_err = "CreateRootSignature failed";
            return false;
        }
        auto make_pso = [&](const std::vector<uint8_t>& dxil, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out_pso) -> bool {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = root_sig_.Get();
            pd.CS = {dxil.data(), dxil.size()};
            return SUCCEEDED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&out_pso)));
        };
        if (!make_pso(dxil_tiles, pso_scan_tiles_) ||
            !make_pso(dxil_offs, pso_scan_offsets_) ||
            !make_pso(dxil_merge, pso_merge_blocks_)) {
            if (out_err) *out_err = "CreateComputePipelineState failed";
            shutdown_locked();
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)))) {
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)))) {
                shutdown_locked();
                return false;
            }
        }
        if (FAILED(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&cmd_alloc_))) ||
            FAILED(device->CreateCommandList(0u, qd.Type, cmd_alloc_.Get(), nullptr, IID_PPV_ARGS(&cmd_list_))) ||
            FAILED(cmd_list_->Close()) ||
            FAILED(device->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
            shutdown_locked();
            return false;
        }
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event_) {
            shutdown_locked();
            return false;
        }
        device_ = device;
        initialized_ = true;
        return true;
    }

    // GPU-accelerated BCPack ("BCPK") decompression -> bit-exact BC1/BC3/BC4/BC5/BC7 blocks + CRC32 check!
    HRESULT decompress_to_memory(
        ID3D12Device* device,
        const uint8_t* src,
        size_t src_size,
        DXGI_FORMAT expected_bc_format,
        std::vector<uint8_t>& out_blocks,
        size_t max_output_bytes = 256u * 1024u * 1024u,
        std::string* out_err = nullptr) {
        out_blocks.clear();
        if (!src || src_size < sizeof(wsx_bcpack::BcPackHeader)) return E_INVALIDARG;

        uint32_t magic = 0u;
        std::memcpy(&magic, src, 4u);
        if (magic != wsx_bcpack::kBcPackMagic) {
            return wsx_bcpack::decompress(src, src_size, expected_bc_format, out_blocks, max_output_bytes)
                ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }

        wsx_bcpack::BcPackHeader hdr{};
        std::memcpy(&hdr, src, sizeof(hdr));
        if (hdr.version != wsx_bcpack::kBcPackVersion) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        const auto kind = static_cast<wsx_bcpack::BcKind>(hdr.bc_kind);
        const uint32_t bsz = wsx_bcpack::block_bytes_for(kind);
        if (bsz == 0u || hdr.block_count == 0u) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        const uint64_t expected_sz = static_cast<uint64_t>(hdr.block_count) * bsz;
        if (expected_sz != hdr.uncompressed_size || expected_sz > max_output_bytes) {
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
        if (sizeof(wsx_bcpack::BcPackHeader) + hdr.payload_size > src_size) {
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
        if (expected_bc_format != DXGI_FORMAT_UNKNOWN) {
            const auto exp_k = wsx_bcpack::classify_dxgi_bc_format(expected_bc_format);
            if (exp_k != wsx_bcpack::BcKind::Unknown && exp_k != kind) {
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }
        }

        // Unpack planar LZ/RLE stream first
        const uint8_t* payload = src + sizeof(wsx_bcpack::BcPackHeader);
        std::vector<uint8_t> planar(static_cast<size_t>(expected_sz));
        if ((hdr.flags & 2u) != 0u) {
            if (!wsx_bcpack::detail::lz_rle_decompress(payload, hdr.payload_size, planar.data(), planar.size())) {
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }
        } else {
            if (hdr.payload_size != planar.size()) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            std::memcpy(planar.data(), payload, planar.size());
        }

        // Execute 3-pass SM 6.6 parallel GPU prefix-sum + block interleave!
        if (!ensure_initialized(device, out_err)) {
            out_blocks.resize(static_cast<size_t>(expected_sz));
            wsx_bcpack::detail::merge_planes(planar.data(), hdr.block_count, kind, out_blocks.data());
            if (wsx_bcpack::detail::crc32_ieee(out_blocks.data(), out_blocks.size()) != hdr.crc32_uncomp) {
                out_blocks.clear();
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }
            return S_OK;
        }

        std::lock_guard<std::mutex> lk(mutex_);
        const uint32_t nblocks = hdr.block_count;
        const uint32_t num_tiles = (nblocks + 255u) / 256u;
        uint32_t num_lanes = 2u;
        if (kind == wsx_bcpack::BcKind::BC3 || kind == wsx_bcpack::BcKind::BC5) num_lanes = 4u;
        else if (kind == wsx_bcpack::BcKind::BC7) num_lanes = 8u;

        Microsoft::WRL::ComPtr<ID3D12Resource> src_up, scan_uav, tile_uav, dst_uav, dst_rb;
        const UINT64 scan_bytes = static_cast<UINT64>(nblocks) * 32ull;
        const UINT64 tile_bytes = static_cast<UINT64>(std::max<uint32_t>(1u, num_tiles)) * 32ull;
        if (FAILED(create_buffer(device_.Get(), D3D12_HEAP_TYPE_UPLOAD, planar.size(),
                                 D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, src_up)) ||
            FAILED(create_buffer(device_.Get(), D3D12_HEAP_TYPE_DEFAULT, scan_bytes,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, scan_uav)) ||
            FAILED(create_buffer(device_.Get(), D3D12_HEAP_TYPE_DEFAULT, tile_bytes,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, tile_uav)) ||
            FAILED(create_buffer(device_.Get(), D3D12_HEAP_TYPE_DEFAULT, expected_sz,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, dst_uav)) ||
            FAILED(create_buffer(device_.Get(), D3D12_HEAP_TYPE_READBACK, expected_sz,
                                 D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, dst_rb))) {
            return E_OUTOFMEMORY;
        }

        void* p_up = nullptr;
        if (FAILED(src_up->Map(0u, nullptr, &p_up)) || !p_up) return E_FAIL;
        std::memcpy(p_up, planar.data(), planar.size());
        src_up->Unmap(0u, nullptr);

        if (FAILED(cmd_alloc_->Reset()) || FAILED(cmd_list_->Reset(cmd_alloc_.Get(), pso_scan_tiles_.Get()))) {
            return E_FAIL;
        }
        const uint32_t params[4] = {nblocks, static_cast<uint32_t>(kind), num_tiles, num_lanes};
        cmd_list_->SetComputeRootSignature(root_sig_.Get());
        cmd_list_->SetComputeRoot32BitConstants(0u, 4u, params, 0u);
        cmd_list_->SetComputeRootShaderResourceView(1u, src_up->GetGPUVirtualAddress());
        cmd_list_->SetComputeRootUnorderedAccessView(2u, scan_uav->GetGPUVirtualAddress());
        cmd_list_->SetComputeRootUnorderedAccessView(3u, tile_uav->GetGPUVirtualAddress());
        cmd_list_->SetComputeRootUnorderedAccessView(4u, dst_uav->GetGPUVirtualAddress());

        // Pass 1: Parallel 256-block tile inclusive scan
        cmd_list_->Dispatch(num_tiles, 1u, 1u);

        D3D12_RESOURCE_BARRIER uav_b[2]{};
        uav_b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uav_b[0].UAV.pResource = scan_uav.Get();
        uav_b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uav_b[1].UAV.pResource = tile_uav.Get();
        cmd_list_->ResourceBarrier(2u, uav_b);

        // Pass 2: Parallel per-lane exclusive scan over tile totals
        cmd_list_->SetPipelineState(pso_scan_offsets_.Get());
        cmd_list_->Dispatch(1u, 1u, 1u);
        cmd_list_->ResourceBarrier(1u, &uav_b[1]);

        // Pass 3: Coalesced parallel BC1/BC3/BC4/BC5/BC7 block assembly
        cmd_list_->SetPipelineState(pso_merge_blocks_.Get());
        cmd_list_->Dispatch((nblocks + 63u) / 64u, 1u, 1u);

        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition = {dst_uav.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd_list_->ResourceBarrier(1u, &to_copy);
        cmd_list_->CopyBufferRegion(dst_rb.Get(), 0u, dst_uav.Get(), 0u, expected_sz);
        if (FAILED(cmd_list_->Close())) return E_FAIL;

        ID3D12CommandList* lists[] = {cmd_list_.Get()};
        queue_->ExecuteCommandLists(1u, lists);
        ++fence_value_;
        if (FAILED(queue_->Signal(fence_.Get(), fence_value_))) return E_FAIL;
        if (fence_->GetCompletedValue() < fence_value_) {
            fence_->SetEventOnCompletion(fence_value_, fence_event_);
            WaitForSingleObject(fence_event_, 10000u);
        }

        void* p_rb = nullptr;
        D3D12_RANGE rr{0u, static_cast<SIZE_T>(expected_sz)};
        if (FAILED(dst_rb->Map(0u, &rr, &p_rb)) || !p_rb) return E_FAIL;
        out_blocks.resize(static_cast<size_t>(expected_sz));
        std::memcpy(out_blocks.data(), p_rb, static_cast<size_t>(expected_sz));
        D3D12_RANGE empty_w{0u, 0u};
        dst_rb->Unmap(0u, &empty_w);

        if (wsx_bcpack::detail::crc32_ieee(out_blocks.data(), out_blocks.size()) != hdr.crc32_uncomp) {
            out_blocks.clear();
            if (out_err) *out_err = "GPU BCPack CRC32 mismatch";
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }

        global_dispatches().fetch_add(1u, std::memory_order_relaxed);
        global_blocks().fetch_add(nblocks, std::memory_order_relaxed);
        global_bytes().fetch_add(expected_sz, std::memory_order_relaxed);
        return S_OK;
    }
};

} // namespace wsx_bcpack_gpu
