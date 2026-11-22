// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Direct3D 12 SM 6.6 GPU GDeflate v1.0 Compute Decompressor.
// Dispatches 1 threadgroup [numthreads(32, 1, 1)] per 64 KiB GDeflate tile:
//   - BTYPE=0 (Stored): 32-lane parallel block copy
//   - BTYPE=1 (Fixed Huffman, 32-stream interleaved): 32-lane parallel symbol decode + warp LZ77 emit
//   - BTYPE=2 (Dynamic Huffman, 32-stream interleaved): on-GPU dynamic tree decode + 32-lane decode + LZ77 emit

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "wsx_gdeflate.h"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace wsx_gdeflate_gpu {

struct GpuTileDesc {
    uint32_t src_byte_offset;
    uint32_t src_byte_size;
    uint32_t dst_byte_offset;
    uint32_t expected_out_size;
};

class WsxGDeflateGpuDecoder {
    using CompileHlslFn = HRESULT(WINAPI*)(
        const char*, size_t, const wchar_t*, const wchar_t*,
        void*, size_t, size_t*, char*, size_t);

    std::mutex mutex_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> cmd_alloc_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmd_list_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_sig_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
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
        rd.Width = (std::max<UINT64>(4u, size_bytes) + 3u) & ~3ull;
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
        pso_.Reset();
        root_sig_.Reset();
        cmd_list_.Reset();
        cmd_alloc_.Reset();
        queue_.Reset();
        device_.Reset();
    }

public:
    WsxGDeflateGpuDecoder() = default;
    ~WsxGDeflateGpuDecoder() {
        std::lock_guard<std::mutex> lk(mutex_);
        shutdown_locked();
    }

    WsxGDeflateGpuDecoder(const WsxGDeflateGpuDecoder&) = delete;
    WsxGDeflateGpuDecoder& operator=(const WsxGDeflateGpuDecoder&) = delete;

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

        static constexpr char kGDeflateCsHlsl[] = R"HLSL(
cbuffer Params : register(b0) {
    uint numTiles;
    uint totalSrcBytes;
    uint totalDstBytes;
    uint reserved;
};

struct TileDesc {
    uint srcOffset;
    uint srcSize;
    uint dstOffset;
    uint expectedOutSize;
};

ByteAddressBuffer srcBuf          : register(t0);
StructuredBuffer<TileDesc> tiles  : register(t1);
RWByteAddressBuffer dstBuf        : register(u0);
RWByteAddressBuffer statusBuf     : register(u1);

static const uint kLenBase[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint kLenExtra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint kDistBase[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint kDistExtra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13
};
static const uint kCodeOrder[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

groupshared uint g_ll_count[16];
groupshared uint g_ll_syms[288];
groupshared uint g_d_count[16];
groupshared uint g_d_syms[32];
groupshared uint g_tmp_lens[320];
groupshared uint g_cl_lens[19];
groupshared uint g_cl_count[8];
groupshared uint g_cl_syms[19];
groupshared uint g_offs[16];

groupshared uint g_lo[32];
groupshared uint g_hi[32];
groupshared uint g_bc[32];
groupshared uint g_fin[32];
groupshared uint g_sym[32];
groupshared uint g_len[32];

groupshared uint g_word_pos;
groupshared uint g_out_pos;
groupshared uint g_active;
groupshared uint g_err;
groupshared uint g_btype;
groupshared uint g_stored_src;
groupshared uint g_stored_dst;
groupshared uint g_stored_len;
groupshared uint g_stored_done;

uint load_src_u8(uint pos) {
    uint w = srcBuf.Load(pos & ~3u);
    return (w >> ((pos & 3u) * 8u)) & 0xFFu;
}

uint load_src_u32_clamped(uint pos, uint limit) {
    if (pos + 4u <= limit) {
        uint align = pos & 3u;
        if (align == 0u) return srcBuf.Load(pos);
        uint w0 = srcBuf.Load(pos & ~3u);
        uint w1 = srcBuf.Load((pos & ~3u) + 4u);
        return (w0 >> (align * 8u)) | (w1 << ((4u - align) * 8u));
    }
    uint w = 0u;
    uint rem = (pos < limit) ? (limit - pos) : 0u;
    for (uint i = 0u; i < rem; ++i) {
        w |= load_src_u8(pos + i) << (i * 8u);
    }
    return w;
}

uint load_dst_u8(uint pos) {
    uint w = dstBuf.Load(pos & ~3u);
    return (w >> ((pos & 3u) * 8u)) & 0xFFu;
}

void store_dst_u8_lane0(uint pos, uint val) {
    uint addr = pos & ~3u;
    uint shift = (pos & 3u) * 8u;
    uint w = dstBuf.Load(addr);
    w = (w & ~(0xFFu << shift)) | ((val & 0xFFu) << shift);
    dstBuf.Store(addr, w);
}

void refill_lane(uint k, uint tileBase, uint tileSize) {
    if (g_bc[k] <= 32u) {
        uint limit = tileBase + tileSize;
        uint cur = tileBase + g_word_pos;
        uint w = load_src_u32_clamped(cur, limit);
        uint step = (g_word_pos < tileSize) ? min(4u, tileSize - g_word_pos) : 0u;
        g_word_pos += step;
        uint bc = g_bc[k];
        if (bc == 0u) {
            g_lo[k] = w;
            g_hi[k] = 0u;
        } else if (bc < 32u) {
            g_lo[k] |= (w << bc);
            g_hi[k] |= (w >> (32u - bc));
        } else {
            g_hi[k] = w;
        }
        g_bc[k] = bc + 32u;
    }
}

uint read_bits_lane(uint k, uint n) {
    if (n == 0u) return 0u;
    uint mask = (n == 32u) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    uint lo = g_lo[k];
    uint hi = g_hi[k];
    uint val = lo & mask;
    if (n < 32u) {
        g_lo[k] = (lo >> n) | (hi << (32u - n));
        g_hi[k] = hi >> n;
    } else {
        g_lo[k] = hi;
        g_hi[k] = 0u;
    }
    g_bc[k] -= n;
    return val;
}

uint decode_huff_ll(uint k) {
    int code = 0;
    int first = 0;
    int index = 0;
    [unroll]
    for (int len = 1; len <= 15; ++len) {
        uint bit = read_bits_lane(k, 1u);
        code = (code << 1) | int(bit);
        int c = int(g_ll_count[len]);
        if (code - c < first) {
            return g_ll_syms[uint(index + (code - first))];
        }
        index += c;
        first = (first + c) << 1;
    }
    return 0xFFFFu;
}

uint decode_huff_dist(uint k) {
    int code = 0;
    int first = 0;
    int index = 0;
    [unroll]
    for (int len = 1; len <= 15; ++len) {
        uint bit = read_bits_lane(k, 1u);
        code = (code << 1) | int(bit);
        int c = int(g_d_count[len]);
        if (code - c < first) {
            return g_d_syms[uint(index + (code - first))];
        }
        index += c;
        first = (first + c) << 1;
    }
    return 0xFFFFu;
}

uint hdr_read_bits(inout uint bitBuf, inout uint bitCnt, inout uint bytePos, uint tileBase, uint tileSize, uint n) {
    while (bitCnt < n && bytePos < tileSize) {
        bitBuf |= load_src_u8(tileBase + bytePos) << bitCnt;
        bitCnt += 8u;
        ++bytePos;
    }
    uint val = bitBuf & ((1u << n) - 1u);
    bitBuf >>= n;
    bitCnt = (bitCnt >= n) ? (bitCnt - n) : 0u;
    return val;
}

void hdr_align_byte(inout uint bitBuf, inout uint bitCnt) {
    uint rem = bitCnt & 7u;
    if (rem != 0u) {
        bitBuf >>= rem;
        bitCnt -= rem;
    }
}

uint hdr_read_u8_aligned(inout uint bitBuf, inout uint bitCnt, inout uint bytePos, uint tileBase, uint tileSize) {
    if (bitCnt >= 8u) {
        uint v = bitBuf & 0xFFu;
        bitBuf >>= 8u;
        bitCnt -= 8u;
        return v;
    }
    if (bytePos < tileSize) {
        uint v = load_src_u8(tileBase + bytePos);
        ++bytePos;
        return v;
    }
    return 0u;
}

[RootSignature("RootFlags(0), RootConstants(num32BitConstants=4, b0), SRV(t0), SRV(t1), UAV(u0), UAV(u1)")]
[numthreads(32, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    uint tileIdx = gid.x;
    uint lane = gtid.x;
    if (tileIdx >= numTiles) return;

    TileDesc td = tiles[tileIdx];

    // 1. All 32 lanes clear this tile's output words in parallel
    uint tileWords = (td.expectedOutSize + 3u) >> 2u;
    for (uint w = lane; w < tileWords; w += 32u) {
        dstBuf.Store(td.dstOffset + w * 4u, 0u);
    }
    if (lane == 0u) {
        g_err = 0u;
        g_out_pos = 0u;
        g_active = 32u;
        g_stored_done = 0u;
        g_btype = (td.srcSize > 0u) ? ((load_src_u8(td.srcOffset) >> 1u) & 3u) : 3u;
    }
    GroupMemoryBarrierWithGroupSync();

    // 2. BTYPE == 0 (Stored uncompressed tile, 32-lane parallel copy per stored block)
    if (g_btype == 0u) {
        uint hBitBuf = 0u, hBitCnt = 0u, hBytePos = 0u;
        uint written = 0u;
        while (g_stored_done == 0u) {
            if (lane == 0u) {
                uint bfinal = hdr_read_bits(hBitBuf, hBitCnt, hBytePos, td.srcOffset, td.srcSize, 1u);
                uint bt = hdr_read_bits(hBitBuf, hBitCnt, hBytePos, td.srcOffset, td.srcSize, 2u);
                if (bt != 0u) {
                    g_err = 1u;
                    g_stored_done = 1u;
                    g_stored_len = 0u;
                } else {
                    hdr_align_byte(hBitBuf, hBitCnt);
                    uint b0 = hdr_read_u8_aligned(hBitBuf, hBitCnt, hBytePos, td.srcOffset, td.srcSize);
                    uint b1 = hdr_read_u8_aligned(hBitBuf, hBitCnt, hBytePos, td.srcOffset, td.srcSize);
                    uint b2 = hdr_read_u8_aligned(hBitBuf, hBitCnt, hBytePos, td.srcOffset, td.srcSize);
                    uint b3 = hdr_read_u8_aligned(hBitBuf, hBitCnt, hBytePos, td.srcOffset, td.srcSize);
                    uint len = b0 | (b1 << 8u);
                    uint nlen = b2 | (b3 << 8u);
                    if (((~len) & 0xFFFFu) != nlen || written + len > td.expectedOutSize) {
                        g_err = 2u;
                        g_stored_done = 1u;
                        g_stored_len = 0u;
                    } else {
                        // Rewind bytePos by buffered whole bytes so all 32 lanes read directly from srcBuf
                        uint rawStart = hBytePos - (hBitCnt >> 3u);
                        hBitBuf = 0u;
                        hBitCnt = 0u;
                        hBytePos = rawStart + len;
                        g_stored_src = td.srcOffset + rawStart;
                        g_stored_dst = td.dstOffset + written;
                        g_stored_len = len;
                        written += len;
                        if (bfinal != 0u || written >= td.expectedOutSize) {
                            g_stored_done = 1u;
                        }
                    }
                }
            }
            GroupMemoryBarrierWithGroupSync();
            uint copyLen = g_stored_len;
            uint sBase = g_stored_src;
            uint dBase = g_stored_dst;
            for (uint i = lane; i < copyLen; i += 32u) {
                uint b = load_src_u8(sBase + i);
                uint dp = dBase + i;
                dstBuf.InterlockedOr(dp & ~3u, b << ((dp & 3u) * 8u));
            }
            GroupMemoryBarrierWithGroupSync();
        }
        if (lane == 0u) {
            statusBuf.Store(tileIdx * 4u, (g_err == 0u && written == td.expectedOutSize) ? 1u : (0xE0000000u | g_err));
        }
        return;
    }

    // 3. BTYPE == 1 (Fixed Huffman) or BTYPE == 2 (Dynamic Huffman) — build tables on lane 0
    if (lane == 0u) {
        for (uint i = 0u; i < 16u; ++i) {
            g_ll_count[i] = 0u;
            g_d_count[i] = 0u;
        }
        uint hBitBuf = 0u, hBitCnt = 0u, hBytePos = 0u;
        hdr_read_bits(hBitBuf, hBitCnt, hBytePos, td.srcOffset, td.srcSize, 1u); // bfinal
        uint bt = hdr_read_bits(hBitBuf, hBitCnt, hBytePos, td.srcOffset, td.srcSize, 2u);

        if (bt == 1u) {
            for (uint i = 0u; i <= 143u; ++i) g_tmp_lens[i] = 8u;
            for (uint i = 144u; i <= 255u; ++i) g_tmp_lens[i] = 9u;
            for (uint i = 256u; i <= 279u; ++i) g_tmp_lens[i] = 7u;
            for (uint i = 280u; i <= 287u; ++i) g_tmp_lens[i] = 8u;

            for (uint s = 0u; s < 288u; ++s) g_ll_count[g_tmp_lens[s]] += 1u;
            g_offs[0] = 0u;
            for (uint l = 1u; l < 16u; ++l) g_offs[l] = g_offs[l - 1u] + g_ll_count[l - 1u];
            for (uint s = 0u; s < 288u; ++s) {
                uint ln = g_tmp_lens[s];
                g_ll_syms[g_offs[ln]] = s;
                g_offs[ln] += 1u;
            }
            g_d_count[5] = 32u;
            for (uint s = 0u; s < 32u; ++s) g_d_syms[s] = s;
        } else if (bt == 2u) {
            uint hlit = hdr_read_bits(hBitBuf, hBitCnt, hBytePos, td.srcOffset, td.srcSize, 5u) + 257u;
            uint hdist = hdr_read_bits(hBitBuf, hBitCnt, hBytePos, td.srcOffset, td.srcSize, 5u) + 1u;
            uint hclen = hdr_read_bits(hBitBuf, hBitCnt, hBytePos, td.srcOffset, td.srcSize, 4u) + 4u;
            if (hlit > 286u || hdist > 30u) {
                g_err = 3u;
            } else {
                for (uint i = 0u; i < 19u; ++i) g_cl_lens[i] = 0u;
                for (uint i = 0u; i < hclen; ++i) {
                    g_cl_lens[kCodeOrder[i]] = hdr_read_bits(hBitBuf, hBitCnt, hBytePos, td.srcOffset, td.srcSize, 3u);
                }
                for (uint i = 0u; i < 8u; ++i) g_cl_count[i] = 0u;
                for (uint i = 0u; i < 19u; ++i) {
                    if (g_cl_lens[i] > 0u) g_cl_count[g_cl_lens[i]] += 1u;
                }
                g_offs[0] = 0u;
                for (uint l = 1u; l < 8u; ++l) g_offs[l] = g_offs[l - 1u] + g_cl_count[l - 1u];
                for (uint i = 0u; i < 19u; ++i) {
                    uint ln = g_cl_lens[i];
                    if (ln > 0u) {
                        g_cl_syms[g_offs[ln]] = i;
                        g_offs[ln] += 1u;
                    }
                }

                uint totalLens = hlit + hdist;
                uint idx = 0u;
                while (idx < totalLens && g_err == 0u) {
                    int code = 0, first = 0, cidx = 0;
                    uint csym = 0xFFFFu;
                    for (int l = 1; l <= 7; ++l) {
                        uint bit = hdr_read_bits(hBitBuf, hBitCnt, hBytePos, td.srcOffset, td.srcSize, 1u);
                        code = (code << 1) | int(bit);
                        int c = int(g_cl_count[l]);
                        if (code - c < first) {
                            csym = g_cl_syms[uint(cidx + (code - first))];
                            break;
                        }
                        cidx += c;
                        first = (first + c) << 1;
                    }
                    if (csym < 16u) {
                        g_tmp_lens[idx++] = csym;
                    } else if (csym == 16u && idx > 0u) {
                        uint rep = hdr_read_bits(hBitBuf, hBitCnt, hBytePos, td.srcOffset, td.srcSize, 2u) + 3u;
                        uint prev = g_tmp_lens[idx - 1u];
                        for (uint r = 0u; r < rep && idx < totalLens; ++r) g_tmp_lens[idx++] = prev;
                    } else if (csym == 17u) {
                        uint rep = hdr_read_bits(hBitBuf, hBitCnt, hBytePos, td.srcOffset, td.srcSize, 3u) + 3u;
                        for (uint r = 0u; r < rep && idx < totalLens; ++r) g_tmp_lens[idx++] = 0u;
                    } else if (csym == 18u) {
                        uint rep = hdr_read_bits(hBitBuf, hBitCnt, hBytePos, td.srcOffset, td.srcSize, 7u) + 11u;
                        for (uint r = 0u; r < rep && idx < totalLens; ++r) g_tmp_lens[idx++] = 0u;
                    } else {
                        g_err = 4u;
                    }
                }

                if (g_err == 0u) {
                    for (uint s = 0u; s < hlit; ++s) {
                        if (g_tmp_lens[s] > 0u) g_ll_count[g_tmp_lens[s]] += 1u;
                    }
                    g_offs[0] = 0u;
                    for (uint l = 1u; l < 16u; ++l) g_offs[l] = g_offs[l - 1u] + g_ll_count[l - 1u];
                    for (uint s = 0u; s < hlit; ++s) {
                        uint ln = g_tmp_lens[s];
                        if (ln > 0u) {
                            g_ll_syms[g_offs[ln]] = s;
                            g_offs[ln] += 1u;
                        }
                    }

                    for (uint s = 0u; s < hdist; ++s) {
                        if (g_tmp_lens[hlit + s] > 0u) g_d_count[g_tmp_lens[hlit + s]] += 1u;
                    }
                    g_offs[0] = 0u;
                    for (uint l = 1u; l < 16u; ++l) g_offs[l] = g_offs[l - 1u] + g_d_count[l - 1u];
                    for (uint s = 0u; s < hdist; ++s) {
                        uint ln = g_tmp_lens[hlit + s];
                        if (ln > 0u) {
                            g_d_syms[g_offs[ln]] = s;
                            g_offs[ln] += 1u;
                        }
                    }
                }
            }
        } else {
            g_err = 5u;
        }

        uint bitsConsumed = hBytePos * 8u - hBitCnt;
        g_word_pos = ((bitsConsumed + 31u) / 32u) * 4u;
        if (g_word_pos > td.srcSize) g_err = 6u;
    }
    GroupMemoryBarrierWithGroupSync();

    if (g_err != 0u) {
        if (lane == 0u) statusBuf.Store(tileIdx * 4u, 0xE0000000u | g_err);
        return;
    }

    // 4. All 32 lanes initialize & prime their initial 64-bit buffer in parallel (2 DWORDs per lane)
    {
        uint primePos = g_word_pos + lane * 8u;
        uint limit = td.srcOffset + td.srcSize;
        g_lo[lane] = load_src_u32_clamped(td.srcOffset + primePos, limit);
        g_hi[lane] = load_src_u32_clamped(td.srcOffset + primePos + 4u, limit);
        g_bc[lane] = 64u;
        g_fin[lane] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0u) {
        g_word_pos = min(td.srcSize, g_word_pos + 256u);
    }
    GroupMemoryBarrierWithGroupSync();

    // 5. Main 32-stream decode loop:
    //    Phase 1: ALL 32 active lanes decode their Huffman litlen symbol + length extra bits IN PARALLEL!
    //    Phase 2: Lane 0 commits literals/matches in exact stream order & refills DWORDs into lanes.
    while (g_active > 0u && g_err == 0u) {
        if (g_fin[lane] == 0u) {
            uint sym = decode_huff_ll(lane);
            g_sym[lane] = sym;
            if (sym > 256u && sym <= 285u) {
                uint li = sym - 257u;
                uint lextra = read_bits_lane(lane, kLenExtra[li]);
                g_len[lane] = kLenBase[li] + lextra;
            } else {
                g_len[lane] = 0u;
            }
        }
        GroupMemoryBarrierWithGroupSync();

        if (lane == 0u) {
            uint outPos = g_out_pos;
            for (uint k = 0u; k < 32u; ++k) {
                if (g_fin[k] != 0u) continue;
                uint sym = g_sym[k];
                if (sym < 256u) {
                    if (outPos >= td.expectedOutSize) { g_err = 7u; break; }
                    store_dst_u8_lane0(td.dstOffset + outPos, sym);
                    ++outPos;
                } else if (sym == 256u) {
                    g_fin[k] = 1u;
                    g_active -= 1u;
                } else if (sym <= 285u) {
                    uint matchLen = g_len[k];
                    refill_lane(k, td.srcOffset, td.srcSize);
                    uint dsym = decode_huff_dist(k);
                    if (dsym >= 30u) { g_err = 8u; break; }
                    uint dextra = read_bits_lane(k, kDistExtra[dsym]);
                    uint dist = kDistBase[dsym] + dextra;
                    if (dist == 0u || dist > outPos || outPos + matchLen > td.expectedOutSize) {
                        g_err = 9u;
                        break;
                    }
                    for (uint m = 0u; m < matchLen; ++m) {
                        uint b = load_dst_u8(td.dstOffset + outPos - dist);
                        store_dst_u8_lane0(td.dstOffset + outPos, b);
                        ++outPos;
                    }
                } else {
                    g_err = 10u;
                    break;
                }
                if (g_fin[k] == 0u) {
                    refill_lane(k, td.srcOffset, td.srcSize);
                }
            }
            g_out_pos = outPos;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0u) {
        uint ok = (g_err == 0u && g_out_pos == td.expectedOutSize) ? 1u : (0xE0000000u | g_err);
        statusBuf.Store(tileIdx * 4u, ok);
    }
}
)HLSL";

        const auto get_dxil = [&]() -> const std::vector<uint8_t>& {
            static std::mutex dxil_mtx;
            static std::vector<uint8_t> cached_dxil;
            std::lock_guard<std::mutex> dlk(dxil_mtx);
            if (!cached_dxil.empty()) return cached_dxil;
            size_t dxil_sz = 0u;
            char err_buf[1024]{};
            const size_t src_len = std::strlen(kGDeflateCsHlsl);
            HRESULT chr = compiler(kGDeflateCsHlsl, src_len, L"main", L"cs_6_6",
                                   nullptr, 0u, &dxil_sz, err_buf, sizeof(err_buf));
            if (FAILED(chr) || dxil_sz < 64u) {
                if (out_err) *out_err = std::string("GDeflate DXIL size query failed: ") + err_buf;
                return cached_dxil;
            }
            std::vector<uint8_t> tmp(dxil_sz);
            chr = compiler(kGDeflateCsHlsl, src_len, L"main", L"cs_6_6",
                           tmp.data(), tmp.size(), &dxil_sz, err_buf, sizeof(err_buf));
            if (FAILED(chr)) {
                if (out_err) *out_err = std::string("GDeflate DXIL compile failed: ") + err_buf;
                return cached_dxil;
            }
            tmp.resize(dxil_sz);
            cached_dxil = std::move(tmp);
            return cached_dxil;
        };

        const std::vector<uint8_t>& dxil = get_dxil();
        if (dxil.empty()) return false;

        device_ = device;
        HRESULT hr = S_OK;
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        hr = device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
        if (FAILED(hr)) {
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            hr = device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
            if (FAILED(hr)) { shutdown_locked(); return false; }
        }
        const D3D12_COMMAND_LIST_TYPE list_type = qd.Type;

        hr = device_->CreateCommandAllocator(list_type, IID_PPV_ARGS(&cmd_alloc_));
        if (FAILED(hr)) { shutdown_locked(); return false; }
        hr = device_->CreateRootSignature(0u, dxil.data(), dxil.size(), IID_PPV_ARGS(&root_sig_));
        if (FAILED(hr)) { shutdown_locked(); return false; }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root_sig_.Get();
        pd.CS.pShaderBytecode = dxil.data();
        pd.CS.BytecodeLength = dxil.size();
        hr = device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_));
        if (FAILED(hr)) { shutdown_locked(); return false; }

        hr = device_->CreateCommandList(0u, list_type, cmd_alloc_.Get(), pso_.Get(), IID_PPV_ARGS(&cmd_list_));
        if (FAILED(hr)) { shutdown_locked(); return false; }
        cmd_list_->Close();

        hr = device_->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        if (FAILED(hr)) { shutdown_locked(); return false; }
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event_) { shutdown_locked(); return false; }

        initialized_ = true;
        return true;
    }

    HRESULT decompress_to_memory(
        ID3D12Device* device,
        const uint8_t* src,
        size_t src_size,
        void* dst,
        size_t dst_capacity,
        uint32_t* out_decompressed_size,
        bool* out_used_gpu = nullptr) {
        if (out_decompressed_size) *out_decompressed_size = 0u;
        if (out_used_gpu) *out_used_gpu = false;
        if (!src || src_size < 12u) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);

        // Parse 8-byte GDeflate v1.0 header & tile offset table
        if (src[0] != wsx_gdeflate::kStreamId || src[1] != wsx_gdeflate::kStreamMagic) {
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
        const uint16_t num_tiles = wsx_gdeflate::detail::read_u16_le(src + 2u);
        if (num_tiles == 0u) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        const uint32_t flags = wsx_gdeflate::detail::read_u32_le(src + 4u);
        const uint32_t tile_size_idx = flags & 0x3u;
        const uint32_t last_tile_sz = (flags >> 2u) & 0x3FFFFu;
        if (tile_size_idx != 1u) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);

        const size_t table_end = 8u + static_cast<size_t>(num_tiles) * 4u;
        if (table_end > src_size) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);

        const size_t tile_payload_bytes = src_size - table_end;
        const uint32_t full_tile = wsx_gdeflate::kDefaultTileSize;
        const uint32_t final_tile_out = (last_tile_sz == 0u) ? full_tile : last_tile_sz;
        const size_t total_out = static_cast<size_t>(num_tiles - 1u) * full_tile + final_tile_out;
        if (total_out > dst_capacity) {
            if (out_decompressed_size) *out_decompressed_size = static_cast<uint32_t>(total_out);
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        }
        if (!dst && total_out > 0u) return E_POINTER;

        std::vector<GpuTileDesc> tile_descs(num_tiles);
        const uint32_t last_tile_comp_sz = wsx_gdeflate::detail::read_u32_le(src + 8u);
        for (uint32_t i = 0u; i < num_tiles; ++i) {
            const uint32_t start = (i == 0u)
                ? 0u
                : wsx_gdeflate::detail::read_u32_le(src + 8u + static_cast<size_t>(i) * 4u);
            const uint32_t comp_sz = (i + 1u < num_tiles)
                ? (wsx_gdeflate::detail::read_u32_le(src + 8u + static_cast<size_t>(i + 1u) * 4u) - start)
                : last_tile_comp_sz;
            if (static_cast<size_t>(start) + comp_sz > tile_payload_bytes) {
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }
            tile_descs[i].src_byte_offset = start;
            tile_descs[i].src_byte_size = comp_sz;
            tile_descs[i].dst_byte_offset = i * full_tile;
            tile_descs[i].expected_out_size = (i + 1u < num_tiles) ? full_tile : final_tile_out;
        }

        // If device is provided and GPU pipeline initializes, run native SM 6.6 compute decompression!
        if (device && ensure_initialized(device)) {
            std::lock_guard<std::mutex> lk(mutex_);
            const UINT64 src_buf_sz = (std::max<UINT64>(4u, tile_payload_bytes) + 3u) & ~3ull;
            const UINT64 desc_buf_sz = static_cast<UINT64>(num_tiles) * sizeof(GpuTileDesc);
            const UINT64 dst_buf_sz = (std::max<UINT64>(4u, total_out) + 3u) & ~3ull;
            const UINT64 stat_buf_sz = static_cast<UINT64>(num_tiles) * sizeof(uint32_t);

            Microsoft::WRL::ComPtr<ID3D12Resource> src_res, desc_res, dst_res, stat_res, rb_dst, rb_stat;
            HRESULT hr = create_buffer(device_.Get(), D3D12_HEAP_TYPE_UPLOAD, src_buf_sz,
                                       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, src_res);
            if (SUCCEEDED(hr)) hr = create_buffer(device_.Get(), D3D12_HEAP_TYPE_UPLOAD, desc_buf_sz,
                                                  D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, desc_res);
            if (SUCCEEDED(hr)) hr = create_buffer(device_.Get(), D3D12_HEAP_TYPE_DEFAULT, dst_buf_sz,
                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, dst_res);
            if (SUCCEEDED(hr)) hr = create_buffer(device_.Get(), D3D12_HEAP_TYPE_DEFAULT, stat_buf_sz,
                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, stat_res);
            if (SUCCEEDED(hr)) hr = create_buffer(device_.Get(), D3D12_HEAP_TYPE_READBACK, dst_buf_sz,
                                                  D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, rb_dst);
            if (SUCCEEDED(hr)) hr = create_buffer(device_.Get(), D3D12_HEAP_TYPE_READBACK, stat_buf_sz,
                                                  D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, rb_stat);

            if (SUCCEEDED(hr)) {
                void* mp = nullptr;
                D3D12_RANGE rr{0, 0};
                if (SUCCEEDED(src_res->Map(0, &rr, &mp))) {
                    std::memset(mp, 0, static_cast<size_t>(src_buf_sz));
                    if (tile_payload_bytes > 0u) std::memcpy(mp, src + table_end, tile_payload_bytes);
                    src_res->Unmap(0, nullptr);
                }
                if (SUCCEEDED(desc_res->Map(0, &rr, &mp))) {
                    std::memcpy(mp, tile_descs.data(), static_cast<size_t>(desc_buf_sz));
                    desc_res->Unmap(0, nullptr);
                }

                hr = cmd_alloc_->Reset();
                if (SUCCEEDED(hr)) hr = cmd_list_->Reset(cmd_alloc_.Get(), pso_.Get());
                if (SUCCEEDED(hr)) {
                    cmd_list_->SetComputeRootSignature(root_sig_.Get());
                    const uint32_t params[4] = {
                        static_cast<uint32_t>(num_tiles),
                        static_cast<uint32_t>(tile_payload_bytes),
                        static_cast<uint32_t>(total_out),
                        0u
                    };
                    cmd_list_->SetComputeRoot32BitConstants(0u, 4u, params, 0u);
                    cmd_list_->SetComputeRootShaderResourceView(1u, src_res->GetGPUVirtualAddress());
                    cmd_list_->SetComputeRootShaderResourceView(2u, desc_res->GetGPUVirtualAddress());
                    cmd_list_->SetComputeRootUnorderedAccessView(3u, dst_res->GetGPUVirtualAddress());
                    cmd_list_->SetComputeRootUnorderedAccessView(4u, stat_res->GetGPUVirtualAddress());

                    cmd_list_->Dispatch(num_tiles, 1u, 1u);

                    D3D12_RESOURCE_BARRIER b[2]{};
                    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b[0].Transition.pResource = dst_res.Get();
                    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b[1].Transition.pResource = stat_res.Get();
                    b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    cmd_list_->ResourceBarrier(2u, b);

                    cmd_list_->CopyBufferRegion(rb_dst.Get(), 0u, dst_res.Get(), 0u, dst_buf_sz);
                    cmd_list_->CopyBufferRegion(rb_stat.Get(), 0u, stat_res.Get(), 0u, stat_buf_sz);
                    hr = cmd_list_->Close();
                }

                if (SUCCEEDED(hr)) {
                    ID3D12CommandList* lists[] = { cmd_list_.Get() };
                    queue_->ExecuteCommandLists(1u, lists);
                    ++fence_value_;
                    hr = queue_->Signal(fence_.Get(), fence_value_);
                    if (SUCCEEDED(hr) && fence_->GetCompletedValue() < fence_value_) {
                        fence_->SetEventOnCompletion(fence_value_, fence_event_);
                        WaitForSingleObject(fence_event_, 10000u);
                    }
                }

                if (SUCCEEDED(hr)) {
                    bool all_tiles_ok = true;
                    void* stat_ptr = nullptr;
                    D3D12_RANGE stat_rr{0, static_cast<SIZE_T>(stat_buf_sz)};
                    if (SUCCEEDED(rb_stat->Map(0, &stat_rr, &stat_ptr)) && stat_ptr) {
                        const auto* statuses = static_cast<const uint32_t*>(stat_ptr);
                        for (uint32_t i = 0u; i < num_tiles; ++i) {
                            if (statuses[i] != 1u) {
                                all_tiles_ok = false;
                                break;
                            }
                        }
                        D3D12_RANGE zero_wr{0, 0};
                        rb_stat->Unmap(0, &zero_wr);
                    } else {
                        all_tiles_ok = false;
                    }

                    if (all_tiles_ok) {
                        void* dst_ptr = nullptr;
                        D3D12_RANGE dst_rr{0, static_cast<SIZE_T>(total_out)};
                        if (SUCCEEDED(rb_dst->Map(0, &dst_rr, &dst_ptr)) && dst_ptr) {
                            if (total_out > 0u) std::memcpy(dst, dst_ptr, total_out);
                            D3D12_RANGE zero_wr{0, 0};
                            rb_dst->Unmap(0, &zero_wr);
                            if (out_decompressed_size) *out_decompressed_size = static_cast<uint32_t>(total_out);
                            if (out_used_gpu) *out_used_gpu = true;
                            return S_OK;
                        }
                    }
                }
            }
        }

        // Clean CPU fallback if device is nullptr or GPU shader unavailable
        std::vector<uint8_t> inflated;
        if (!wsx_gdeflate::decompress(src, src_size, inflated, dst_capacity)) {
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
        if (inflated.size() > dst_capacity) {
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        }
        if (!inflated.empty()) {
            std::memcpy(dst, inflated.data(), inflated.size());
        }
        if (out_decompressed_size) *out_decompressed_size = static_cast<uint32_t>(inflated.size());
        return S_OK;
    }
};

} // namespace wsx_gdeflate_gpu
