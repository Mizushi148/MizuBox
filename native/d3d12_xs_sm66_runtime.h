// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Pure Direct3D 12 (Shader Model 6.6) Runtime Contract for Translated Xbox Series X|S Shaders:
// - No Vulkan, no vendor extensions (works natively on NVIDIA, AMD, Intel, and WARP).
// - Emulates Xbox 64-bit GPU virtual address spaces (s_load*, buffer_*, global_load/store/atomic*)
//   via SM 6.6 Dynamic Resources (ResourceDescriptorHeap[]) over a contiguous RWByteAddressBuffer slab table.
// - Passes Xbox user SGPRs (s0..s31), heap_base, code_base (for s_getpc_b64 FSSM constant tables),
//   and FNV-1a bindless T#/S# descriptor hash table metadata in 40 DWORDS of Root Constants (limit: 64 DWORDS).
// - Dynamically loads Windows SDK dxcompiler.dll (+ dxil.dll) at runtime to compile SM 6.6 HLSL -> signed DXIL.

#pragma once

#include "wsx_config.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace wsx_sm66 {

using Microsoft::WRL::ComPtr;

// Must stay layout-identical to D3D12XBOX_SM66_CAPS in include/xbox/d3d12_xs.h
struct Sm66Caps {
    uint32_t HighestShaderModel;      // e.g. 0x66 for SM 6.6
    uint32_t ResourceBindingTier;     // 3 required for Dynamic Resources
    uint32_t SupportsDynamicResources;// 1 if SM 6.6 ResourceDescriptorHeap / SamplerDescriptorHeap available
    uint32_t SupportsInt64Atomics;    // 1 if 64-bit integer atomics supported in SM 6.6
    uint32_t WaveLaneCountMin;        // 32 on NVIDIA, 32/64 on AMD
    uint32_t WaveLaneCountMax;
    uint32_t MeshShaderTier;          // D3D12_MESH_SHADER_TIER (10 = Tier 1)
    uint32_t RaytracingTier;          // D3D12_RAYTRACING_TIER (11 = Tier 1.1)
};

// Must stay layout-identical to D3D12XBOX_SM66_USER_DATA (40 DWORDS = 160 bytes)
struct Sm66UserData {
    uint32_t s[32];             // Xbox user SGPRs s0..s31
    uint32_t heap_base_lo;      // 64-bit Xbox GPU heap base VA
    uint32_t heap_base_hi;
    uint32_t code_base_lo;      // 64-bit Xbox GPU VA of uploaded FSSM blob (s_getpc_b64)
    uint32_t code_base_hi;
    uint32_t slab_base_idx;     // First ResourceDescriptorHeap[] index of RWByteAddressBuffer slabs
    uint32_t slab_shift;        // log2(slabSizeBytes), e.g. 20 (1 MB) .. 28 (256 MB)
    uint32_t desc_table_srv_idx;// ResourceDescriptorHeap[] index of FNV-1a T#/S# StructuredBuffer
    uint32_t desc_table_mask;   // (tableCapacity - 1), power of 2
};
static_assert(sizeof(Sm66UserData) == 160, "Sm66UserData must be 40 DWORDS (160 bytes)");

// Must stay layout-identical to D3D12XBOX_SM66_DESC_HASH_ENTRY (40 bytes)
struct Sm66DescHashEntry {
    uint32_t key[8];    // Raw 8-dword T# OR 4-dword S# + {0xFFFFFFFF x 4}
    uint32_t heap_index;// Index into ResourceDescriptorHeap[] or SamplerDescriptorHeap[]
    uint32_t flags;     // 0 = empty, 1 = ResourceDescriptorHeap, 2 = SamplerDescriptorHeap
};
static_assert(sizeof(Sm66DescHashEntry) == 40, "Sm66DescHashEntry must be 40 bytes");

inline uint32_t fnv1a_8dw(const uint32_t key[8]) noexcept {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 8; ++i) {
        const uint32_t w = key[i];
        h = (h ^ (w & 0xFFu)) * 16777619u;
        h = (h ^ ((w >> 8) & 0xFFu)) * 16777619u;
        h = (h ^ ((w >> 16) & 0xFFu)) * 16777619u;
        h = (h ^ ((w >> 24) & 0xFFu)) * 16777619u;
    }
    return h;
}

// Reusable SM 6.6 HLSL runtime preamble for pure Direct3D 12 translated Xbox shaders
inline constexpr const char kSm66HlslPreamble[] = R"HLSL(
// MizuBox Direct3D 12 (SM 6.6) Xbox RDNA2 Runtime Preamble (by Mizushi — Fork of AnyXboxSeries)
// Contract: 40 DWORDS Root Constants (b0) + SM 6.6 ResourceDescriptorHeap / SamplerDescriptorHeap

cbuffer WsxUserData : register(b0, space0) {
    uint4 wsx_s_vec[8]; // 32 DWORDS tightly packed (HLSL packs scalar arrays to 16B per element!)
    uint4 wsx_meta0;    // x=heap_base_lo, y=heap_base_hi, z=code_base_lo, w=code_base_hi
    uint4 wsx_meta1;    // x=slab_base_idx, y=slab_shift, z=desc_table_srv_idx, w=desc_table_mask
};

struct WsxDescHashEntry {
    uint4 key0;
    uint4 key1;
    uint heap_index;
    uint flags;
};

uint wsx_sgpr(uint idx) {
    return wsx_s_vec[idx >> 2u][idx & 3u];
}

uint64_t wsx_heap_base() {
    return (uint64_t(wsx_meta0.y) << 32) | uint64_t(wsx_meta0.x);
}

uint64_t wsx_code_base() {
    return (uint64_t(wsx_meta0.w) << 32) | uint64_t(wsx_meta0.z);
}

uint64_t wsx_sgpr_u64(uint lo_idx) {
    return (uint64_t(wsx_sgpr(lo_idx + 1u)) << 32) | uint64_t(wsx_sgpr(lo_idx));
}

void wsx_va_to_slab(uint64_t va, out uint slab_desc_idx, out uint byte_offset) {
    uint64_t rel = va - wsx_heap_base();
    uint slab_shift = wsx_meta1.y;
    uint slab_i = uint(rel >> slab_shift);
    uint mask = (1u << slab_shift) - 1u;
    slab_desc_idx = wsx_meta1.x + slab_i;
    byte_offset = uint(rel) & mask;
}

uint wsx_va_load32(uint64_t va) {
    uint sidx, boff;
    wsx_va_to_slab(va, sidx, boff);
    RWByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(sidx)];
    return slab.Load(boff);
}

uint2 wsx_va_load64(uint64_t va) {
    uint sidx, boff;
    wsx_va_to_slab(va, sidx, boff);
    RWByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(sidx)];
    return slab.Load2(boff);
}

uint4 wsx_va_load128(uint64_t va) {
    uint sidx, boff;
    wsx_va_to_slab(va, sidx, boff);
    RWByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(sidx)];
    return slab.Load4(boff);
}

void wsx_va_store32(uint64_t va, uint value) {
    uint sidx, boff;
    wsx_va_to_slab(va, sidx, boff);
    RWByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(sidx)];
    slab.Store(boff, value);
}

void wsx_va_store128(uint64_t va, uint4 value) {
    uint sidx, boff;
    wsx_va_to_slab(va, sidx, boff);
    RWByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(sidx)];
    slab.Store4(boff, value);
}

uint wsx_va_atomic_add32(uint64_t va, uint delta) {
    uint sidx, boff;
    wsx_va_to_slab(va, sidx, boff);
    RWByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(sidx)];
    uint orig = 0u;
    slab.InterlockedAdd(boff, delta, orig);
    return orig;
}

uint wsx_va_load_u8(uint64_t va) {
    uint w = wsx_va_load32(va & ~3ull);
    return (w >> ((uint(va) & 3u) * 8u)) & 0xFFu;
}

uint wsx_va_load_i8(uint64_t va) {
    uint b = wsx_va_load_u8(va);
    return uint((int(b) << 24) >> 24);
}

uint wsx_va_load_u16(uint64_t va) {
    uint w = wsx_va_load32(va & ~3ull);
    return (w >> ((uint(va) & 2u) * 8u)) & 0xFFFFu;
}

uint wsx_va_load_i16(uint64_t va) {
    uint h = wsx_va_load_u16(va);
    return uint((int(h) << 16) >> 16);
}

void wsx_va_store_u8(uint64_t va, uint v) {
    uint sidx, boff;
    wsx_va_to_slab(va & ~3ull, sidx, boff);
    uint sh = (uint(va) & 3u) * 8u;
    RWByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(sidx)];
    slab.InterlockedAnd(boff, ~(0xFFu << sh));
    slab.InterlockedOr(boff, (v & 0xFFu) << sh);
}

void wsx_va_store_u16(uint64_t va, uint v) {
    uint sidx, boff;
    wsx_va_to_slab(va & ~3ull, sidx, boff);
    uint sh = (uint(va) & 2u) * 8u;
    RWByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(sidx)];
    slab.InterlockedAnd(boff, ~(0xFFFFu << sh));
    slab.InterlockedOr(boff, (v & 0xFFFFu) << sh);
}

uint wsx_va_atomic_op32(uint64_t va, uint op_kind, uint val, uint cmp_val) {
    uint sidx, boff;
    wsx_va_to_slab(va, sidx, boff);
    RWByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(sidx)];
    uint orig = 0u;
    if (op_kind == 0u)      slab.InterlockedExchange(boff, val, orig);
    else if (op_kind == 1u) slab.InterlockedCompareExchange(boff, cmp_val, val, orig);
    else if (op_kind == 2u) slab.InterlockedAdd(boff, val, orig);
    else if (op_kind == 3u) slab.InterlockedAdd(boff, uint(-int(val)), orig);
    else if (op_kind == 4u) slab.InterlockedMin(boff, int(val), orig);
    else if (op_kind == 5u) slab.InterlockedMin(boff, val, orig);
    else if (op_kind == 6u) slab.InterlockedMax(boff, int(val), orig);
    else if (op_kind == 7u) slab.InterlockedMax(boff, val, orig);
    else if (op_kind == 8u) slab.InterlockedAnd(boff, val, orig);
    else if (op_kind == 9u) slab.InterlockedOr(boff, val, orig);
    else if (op_kind == 10u) slab.InterlockedXor(boff, val, orig);
    else if (op_kind == 11u) slab.InterlockedAdd(boff, 1u, orig);
    else if (op_kind == 12u) slab.InterlockedAdd(boff, 0xFFFFFFFFu, orig);
    else {
        uint cur = slab.Load(boff);
        [loop] for (uint _it = 0u; _it < 64u; ++_it) {
            uint nxt = cur;
            if (op_kind == 13u)      nxt = (asfloat(cur) == asfloat(cmp_val)) ? val : cur;
            else if (op_kind == 14u) nxt = asuint(min(asfloat(cur), asfloat(val)));
            else if (op_kind == 15u) nxt = asuint(max(asfloat(cur), asfloat(val)));
            else if (op_kind == 16u) nxt = asuint(asfloat(cur) + asfloat(val));
            else if (op_kind == 17u) nxt = (cur >= val) ? (cur - val) : 0u;
            uint prev = 0u;
            slab.InterlockedCompareExchange(boff, cur, nxt, prev);
            orig = prev;
            if (prev == cur) break;
            cur = prev;
        }
    }
    return orig;
}

uint64_t wsx_va_atomic_op64(uint64_t va, uint op_kind, uint64_t val, uint64_t cmp_val) {
    uint sidx, boff;
    wsx_va_to_slab(va & ~7ull, sidx, boff);
    RWByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(sidx)];
    uint64_t orig = 0ull;
    if (op_kind == 0u)      slab.InterlockedExchange64(boff, val, orig);
    else if (op_kind == 1u) slab.InterlockedCompareExchange64(boff, cmp_val, val, orig);
    else if (op_kind == 2u) slab.InterlockedAdd64(boff, val, orig);
    else if (op_kind == 3u) slab.InterlockedAdd64(boff, uint64_t(-int64_t(val)), orig);
    else if (op_kind == 4u) { int64_t _o = 0; slab.InterlockedMin64(boff, int64_t(val), _o); orig = uint64_t(_o); }
    else if (op_kind == 5u) slab.InterlockedMin64(boff, val, orig);
    else if (op_kind == 6u) { int64_t _o = 0; slab.InterlockedMax64(boff, int64_t(val), _o); orig = uint64_t(_o); }
    else if (op_kind == 7u) slab.InterlockedMax64(boff, val, orig);
    else if (op_kind == 8u) slab.InterlockedAnd64(boff, val, orig);
    else if (op_kind == 9u) slab.InterlockedOr64(boff, val, orig);
    else if (op_kind == 10u) slab.InterlockedXor64(boff, val, orig);
    else if (op_kind == 11u) slab.InterlockedAdd64(boff, 1ull, orig);
    else if (op_kind == 12u) slab.InterlockedAdd64(boff, 0xFFFFFFFFFFFFFFFFull, orig);
    else {
        uint64_t cur = slab.Load<uint64_t>(boff);
        [loop] for (uint _it = 0u; _it < 64u; ++_it) {
            uint64_t nxt = cur;
            if (op_kind == 13u)      nxt = (cur == cmp_val) ? val : cur;
            else if (op_kind == 14u) nxt = (cur < val) ? cur : val;
            else if (op_kind == 15u) nxt = (cur > val) ? cur : val;
            uint64_t prev = 0ull;
            slab.InterlockedCompareExchange64(boff, cur, nxt, prev);
            orig = prev;
            if (prev == cur) break;
            cur = prev;
        }
    }
    return orig;
}

uint wsx_tex_atomic_op32(uint ti, uint2 coord, uint op_kind, uint val, uint cmp_val) {
    RWTexture2D<uint> tex = ResourceDescriptorHeap[NonUniformResourceIndex(ti)];
    uint orig = 0u;
    if (op_kind == 0u)      InterlockedExchange(tex[coord], val, orig);
    else if (op_kind == 1u) InterlockedCompareExchange(tex[coord], cmp_val, val, orig);
    else if (op_kind == 2u) InterlockedAdd(tex[coord], val, orig);
    else if (op_kind == 3u) InterlockedAdd(tex[coord], uint(-int(val)), orig);
    else if (op_kind == 4u) InterlockedMin(tex[coord], int(val), orig);
    else if (op_kind == 5u) InterlockedMin(tex[coord], val, orig);
    else if (op_kind == 6u) InterlockedMax(tex[coord], int(val), orig);
    else if (op_kind == 7u) InterlockedMax(tex[coord], val, orig);
    else if (op_kind == 8u) InterlockedAnd(tex[coord], val, orig);
    else if (op_kind == 9u) InterlockedOr(tex[coord], val, orig);
    else if (op_kind == 10u) InterlockedXor(tex[coord], val, orig);
    else if (op_kind == 11u) InterlockedAdd(tex[coord], 1u, orig);
    else if (op_kind == 12u) InterlockedAdd(tex[coord], 0xFFFFFFFFu, orig);
    return orig;
}

// Clean-room RDNA2 image_bvh_intersect_ray / image_bvh64_intersect_ray helper
// Decodes triangle (node_type 0..3) via Moller-Trumbore or box32 (node_type 5) via 4-AABB slab test
uint4 wsx_bvh_intersect_ray(uint64_t base_va, uint64_t node_ptr, float tmax, float3 ro, float3 rd, float3 inv_rd) {
    uint node_type = uint(node_ptr & 7ull);
    uint64_t addr = base_va + ((node_ptr & ~7ull) << 3u);
    if (node_type <= 3u) {
        float3 v0 = float3(asfloat(wsx_va_load32(addr)), asfloat(wsx_va_load32(addr + 4ull)), asfloat(wsx_va_load32(addr + 8ull)));
        float3 v1 = float3(asfloat(wsx_va_load32(addr + 12ull)), asfloat(wsx_va_load32(addr + 16ull)), asfloat(wsx_va_load32(addr + 20ull)));
        float3 v2 = float3(asfloat(wsx_va_load32(addr + 24ull)), asfloat(wsx_va_load32(addr + 28ull)), asfloat(wsx_va_load32(addr + 32ull)));
        float3 e1 = v1 - v0;
        float3 e2 = v2 - v0;
        float3 pvec = cross(rd, e2);
        float det = dot(e1, pvec);
        float3 tvec = ro - v0;
        float u = dot(tvec, pvec);
        float3 qvec = cross(tvec, e1);
        float v = dot(rd, qvec);
        float t = dot(e2, qvec);
        return uint4(asuint(t), asuint(det), asuint(u), asuint(v));
    }
    uint4 hits = uint4(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
    uint out_i = 0u;
    [unroll] for (uint c = 0u; c < 4u; ++c) {
        uint child = wsx_va_load32(addr + uint64_t(c * 4u));
        if (child == 0xFFFFFFFFu) continue;
        uint64_t bo = addr + 16ull + uint64_t(c * 24u);
        float3 bmin = float3(asfloat(wsx_va_load32(bo)), asfloat(wsx_va_load32(bo + 4ull)), asfloat(wsx_va_load32(bo + 8ull)));
        float3 bmax = float3(asfloat(wsx_va_load32(bo + 12ull)), asfloat(wsx_va_load32(bo + 16ull)), asfloat(wsx_va_load32(bo + 20ull)));
        float3 t0 = (bmin - ro) * inv_rd;
        float3 t1 = (bmax - ro) * inv_rd;
        float3 tlo = min(t0, t1);
        float3 thi = max(t0, t1);
        float tnear = max(max(tlo.x, tlo.y), max(tlo.z, 0.0f));
        float tfar  = min(min(thi.x, thi.y), min(thi.z, tmax));
        if (tnear <= tfar) {
            if (out_i == 0u) hits.x = child;
            else if (out_i == 1u) hits.y = child;
            else if (out_i == 2u) hits.z = child;
            else hits.w = child;
            ++out_i;
        }
    }
    return hits;
}

// Compact RDNA2 math / bit / wave helpers
float wsx_med3_f(float a, float b, float c) { return max(min(a, b), min(max(a, b), c)); }
uint wsx_med3_i(uint a, uint b, uint c) { int ia = int(a), ib = int(b), ic = int(c); return uint(max(min(ia, ib), min(max(ia, ib), ic))); }
uint wsx_med3_u(uint a, uint b, uint c) { return max(min(a, b), min(max(a, b), c)); }

uint wsx_bfe_u(uint a, uint b, uint c) { uint w = c & 31u; return (w == 0u) ? 0u : ((a >> (b & 31u)) & ((1u << w) - 1u)); }
uint wsx_bfe_i(uint a, uint b, uint c) { uint w = c & 31u; if (w == 0u) return 0u; uint sh = 32u - w; return uint(int((a >> (b & 31u)) << sh) >> sh); }
uint wsx_alignbit(uint hi, uint lo, uint sh) { uint s = sh & 31u; return (s == 0u) ? lo : ((lo >> s) | (hi << (32u - s))); }
uint wsx_alignbyte(uint hi, uint lo, uint sel) { uint s = (sel & 3u) * 8u; return (s == 0u) ? lo : ((lo >> s) | (hi << (32u - s))); }

uint wsx_perm_b32(uint hi, uint lo, uint sel) {
    uint r = 0u;
    [unroll]
    for (uint k = 0u; k < 4u; ++k) {
        uint s = (sel >> (k * 8u)) & 0xFFu;
        uint b = 0u;
        if (s < 4u)       b = (lo >> (s * 8u)) & 0xFFu;
        else if (s < 8u)  b = (hi >> ((s - 4u) * 8u)) & 0xFFu;
        else if (s == 8u) b = ((lo & 0x80u) != 0u) ? 0xFFu : 0u;
        else if (s == 9u) b = ((lo & 0x8000u) != 0u) ? 0xFFu : 0u;
        else if (s == 10u) b = ((lo & 0x800000u) != 0u) ? 0xFFu : 0u;
        else if (s == 11u) b = ((lo & 0x80000000u) != 0u) ? 0xFFu : 0u;
        else if (s == 12u) b = 0u;
        else               b = 0xFFu;
        r |= (b << (k * 8u));
    }
    return r;
}

float wsx_cubeid(float x, float y, float z) {
    float ax = abs(x), ay = abs(y), az = abs(z);
    if (az >= ax && az >= ay) return (z < 0.0f) ? 5.0f : 4.0f;
    if (ay >= ax) return (y < 0.0f) ? 3.0f : 2.0f;
    return (x < 0.0f) ? 1.0f : 0.0f;
}
float wsx_cubesc(float x, float y, float z) {
    float ax = abs(x), ay = abs(y), az = abs(z);
    if (az >= ax && az >= ay) return (z < 0.0f) ? -x : x;
    if (ay >= ax) return x;
    return (x < 0.0f) ? z : -z;
}
float wsx_cubetc(float x, float y, float z) {
    float ax = abs(x), ay = abs(y), az = abs(z);
    if (az >= ax && az >= ay) return -y;
    if (ay >= ax) return (y < 0.0f) ? -z : z;
    return -y;
}
float wsx_cubema(float x, float y, float z) {
    return 2.0f * max(max(abs(x), abs(y)), abs(z));
}

uint wsx_wqm32(uint m) {
    uint r = 0u;
    [unroll]
    for (uint q = 0u; q < 8u; ++q) {
        uint sh = q * 4u;
        if ((m & (0xFu << sh)) != 0u) r |= (0xFu << sh);
    }
    return r;
}

uint wsx_quadmask32(uint m) {
    uint r = 0u;
    [unroll]
    for (uint q = 0u; q < 8u; ++q) {
        if ((m & (0xFu << (q * 4u))) != 0u) r |= (1u << q);
    }
    return r;
}

uint wsx_frexp_exp_f32(float f) {
    uint b = asuint(f);
    uint e = (b >> 23u) & 0xFFu;
    return (e == 0u || e == 0xFFu) ? 0u : uint(int(e) - 126);
}
float wsx_frexp_mant_f32(float f) {
    uint b = asuint(f);
    uint e = (b >> 23u) & 0xFFu;
    if (e == 0u || e == 0xFFu) return f;
    return asfloat((b & 0x807FFFFFu) | 0x3F000000u);
}

uint wsx_sdwa_src(uint v, uint sel, uint sext) {
    uint r = v;
    if (sel < 4u) {
        r = (v >> (sel * 8u)) & 0xFFu;
        if (sext != 0u) r = uint((int(r) << 24) >> 24);
    } else if (sel == 4u) {
        r = v & 0xFFFFu;
        if (sext != 0u) r = uint((int(r) << 16) >> 16);
    } else if (sel == 5u) {
        r = (v >> 16u) & 0xFFFFu;
        if (sext != 0u) r = uint((int(r) << 16) >> 16);
    }
    return r;
}

uint wsx_sdwa_dst(uint old_v, uint v, uint dst_sel, uint dst_unused) {
    if (dst_sel == 6u) return v;
    uint base = (dst_unused == 2u) ? old_v : 0u;
    if (dst_sel < 4u) {
        uint sh = dst_sel * 8u;
        uint b = v & 0xFFu;
        if (dst_unused == 1u) return uint((int(b) << 24) >> 24);
        return (base & ~(0xFFu << sh)) | (b << sh);
    }
    uint sh = (dst_sel == 5u) ? 16u : 0u;
    uint h = v & 0xFFFFu;
    if (dst_unused == 1u) return uint((int(h) << 16) >> 16);
    return (base & ~(0xFFFFu << sh)) | (h << sh);
}

uint wsx_dpp16(uint v, uint ctrl) {
    uint lane = WaveGetLaneIndex();
    uint lmask = WaveGetLaneCount() - 1u;
    uint src = lane;
    if (ctrl < 0x100u) {
        uint q = (ctrl >> ((lane & 3u) * 2u)) & 3u;
        src = (lane & ~3u) | q;
    } else if (ctrl >= 0x101u && ctrl <= 0x10Fu) {
        uint sh = ctrl - 0x100u;
        src = ((lane & 15u) + sh < 16u) ? (lane + sh) : lane;
    } else if (ctrl >= 0x111u && ctrl <= 0x11Fu) {
        uint sh = ctrl - 0x110u;
        src = ((lane & 15u) >= sh) ? (lane - sh) : lane;
    } else if (ctrl >= 0x121u && ctrl <= 0x12Fu) {
        uint sh = ctrl - 0x120u;
        src = (lane & ~15u) | (((lane & 15u) + sh) & 15u);
    } else if (ctrl == 0x130u) { src = (lane > 0u) ? (lane - 1u) : 0u;
    } else if (ctrl == 0x134u) { src = (lane + lmask) & lmask;
    } else if (ctrl == 0x138u) { src = (lane < lmask) ? (lane + 1u) : lmask;
    } else if (ctrl == 0x13Cu) { src = (lane + 1u) & lmask;
    } else if (ctrl == 0x140u) { src = lane ^ 15u;
    } else if (ctrl == 0x141u) { src = lane ^ 7u;
    } else if (ctrl == 0x142u) { src = (lane >= 16u) ? ((lane & ~15u) - 1u) : lane;
    } else if (ctrl == 0x143u) { src = (lane >= 32u) ? 31u : lmask;
    } else if (ctrl >= 0x150u && ctrl <= 0x15Fu) { src = (lane & ~15u) | (ctrl & 15u);
    } else if (ctrl >= 0x160u && ctrl <= 0x16Fu) { src = lane ^ (ctrl & 15u); }
    return WaveReadLaneAt(v, src & lmask);
}

uint wsx_dpp8(uint v, uint sel_word) {
    uint lane = WaveGetLaneIndex();
    uint sub = (sel_word >> ((lane & 7u) * 3u)) & 7u;
    return WaveReadLaneAt(v, ((lane & ~7u) | sub) & (WaveGetLaneCount() - 1u));
}

uint wsx_ds_swizzle(uint v, uint off16) {
    uint lane = WaveGetLaneIndex();
    uint src = lane;
    if ((off16 & 0x8000u) != 0u) {
        uint q = (off16 >> ((lane & 3u) * 2u)) & 3u;
        src = (lane & ~3u) | q;
    } else {
        uint and_m = off16 & 0x1Fu;
        uint or_m = (off16 >> 5u) & 0x1Fu;
        uint xor_m = (off16 >> 10u) & 0x1Fu;
        src = (lane & ~31u) | ((((lane & 31u) & and_m) | or_m) ^ xor_m);
    }
    return WaveReadLaneAt(v, src & (WaveGetLaneCount() - 1u));
}

uint wsx_permlane16(uint src0, uint sel_lo, uint sel_hi, uint x16) {
    uint lane = WaveGetLaneIndex();
    uint sub = lane & 15u;
    uint sel_w = (sub < 8u) ? sel_lo : sel_hi;
    uint idx = (sel_w >> ((sub & 7u) * 4u)) & 15u;
    uint base = (lane & ~15u) ^ (x16 != 0u ? 16u : 0u);
    return WaveReadLaneAt(src0, (base | idx) & (WaveGetLaneCount() - 1u));
}

bool wsx_mask_bit(uint lo, uint hi) {
    uint lane = WaveGetLaneIndex();
    return (lane < 32u) ? (((lo >> lane) & 1u) != 0u) : (((hi >> (lane - 32u)) & 1u) != 0u);
}

bool wsx_fp_class_f32(float x, uint mask) {
    uint u = asuint(x);
    uint sign = u >> 31u;
    uint exp = (u >> 23u) & 0xFFu;
    uint mant = u & 0x7FFFFFu;
    uint bit = 0u;
    if (exp == 0xFFu) {
        if (mant == 0u)      bit = (sign != 0u) ? 2u : 9u; // -Inf : +Inf
        else                 bit = ((mant & 0x400000u) != 0u) ? 1u : 0u; // QNaN : SNaN
    } else if (exp == 0u) {
        if (mant == 0u)      bit = (sign != 0u) ? 5u : 6u; // -0 : +0
        else                 bit = (sign != 0u) ? 4u : 7u; // -Subnormal : +Subnormal
    } else {
        bit = (sign != 0u) ? 3u : 8u; // -Normal : +Normal
    }
    return ((mask >> bit) & 1u) != 0u;
}

uint wsx_lerp_u8(uint a, uint b, uint c) {
    uint r = 0u;
    [unroll]
    for (uint k = 0u; k < 4u; ++k) {
        uint sh = k * 8u;
        uint v = (((a >> sh) & 0xFFu) + ((b >> sh) & 0xFFu) + ((c >> sh) & 1u)) >> 1u;
        r |= ((v & 0xFFu) << sh);
    }
    return r;
}

uint wsx_sad_u8(uint a, uint b, uint c) {
    uint acc = c;
    [unroll]
    for (uint k = 0u; k < 4u; ++k) {
        uint sh = k * 8u;
        int d = int((a >> sh) & 0xFFu) - int((b >> sh) & 0xFFu);
        acc += uint(abs(d));
    }
    return acc;
}

uint wsx_cvt_pknorm_i16(float a, float b) {
    int ia = int(round(clamp(a, -1.0f, 1.0f) * 32767.0f));
    int ib = int(round(clamp(b, -1.0f, 1.0f) * 32767.0f));
    return (uint(ia) & 0xFFFFu) | ((uint(ib) & 0xFFFFu) << 16u);
}

uint wsx_cvt_pknorm_u16(float a, float b) {
    uint ua = uint(round(saturate(a) * 65535.0f));
    uint ub = uint(round(saturate(b) * 65535.0f));
    return (ua & 0xFFFFu) | ((ub & 0xFFFFu) << 16u);
}

// Reads an inline constant table dword from the shader's FSSM blob (s_getpc_b64 + offset)
uint wsx_code_load32(uint fssm_byte_offset) {
    return wsx_va_load32(wsx_code_base() + uint64_t(fssm_byte_offset));
}

uint wsx_fnv1a_word(uint h, uint w) {
    h = (h ^ (w & 0xFFu)) * 16777619u;
    h = (h ^ ((w >> 8) & 0xFFu)) * 16777619u;
    h = (h ^ ((w >> 16) & 0xFFu)) * 16777619u;
    h = (h ^ ((w >> 24) & 0xFFu)) * 16777619u;
    return h;
}

uint wsx_lookup_descriptor(uint4 k0, uint4 k1) {
    uint h = 2166136261u;
    h = wsx_fnv1a_word(h, k0.x); h = wsx_fnv1a_word(h, k0.y);
    h = wsx_fnv1a_word(h, k0.z); h = wsx_fnv1a_word(h, k0.w);
    h = wsx_fnv1a_word(h, k1.x); h = wsx_fnv1a_word(h, k1.y);
    h = wsx_fnv1a_word(h, k1.z); h = wsx_fnv1a_word(h, k1.w);
    StructuredBuffer<WsxDescHashEntry> tbl = ResourceDescriptorHeap[wsx_meta1.z];
    uint tbl_mask = wsx_meta1.w;
    uint slot = h & tbl_mask;
    [unroll]
    for (uint probe = 0; probe < 8u; ++probe) {
        WsxDescHashEntry e = tbl[(slot + probe) & tbl_mask];
        if (e.flags == 0u) return 0u; // fallback descriptor index 0
        if (all(e.key0 == k0) && all(e.key1 == k1)) {
            return e.heap_index;
        }
    }
    return 0u;
}

uint wsx_lookup_tsharp(uint4 t0, uint4 t1) {
    return wsx_lookup_descriptor(t0, t1);
}

uint wsx_lookup_ssharp(uint4 s0) {
    return wsx_lookup_descriptor(s0, uint4(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu));
}
)HLSL";

// Modular trimmed SM 6.6 preamble builder: emits ONLY helper groups referenced in `shader_body`
// (slashes per-shader HLSL from ~20.5 KB down to ~1.5-4 KB so DXC parses & optimizes far faster).
inline std::string build_trimmed_sm66_preamble(const std::string& shader_body) {
    auto has_any = [&](std::initializer_list<const char*> needles) -> bool {
        for (const char* n : needles) {
            if (shader_body.find(n) != std::string::npos) return true;
        }
        return false;
    };

    const bool need_bvh      = has_any({"wsx_bvh_intersect_ray"});
    const bool need_va_a32   = has_any({"wsx_va_atomic_op32"});
    const bool need_va_a64   = has_any({"wsx_va_atomic_op64"});
    const bool need_va_sub   = has_any({"wsx_va_load_u8", "wsx_va_load_i8", "wsx_va_load_u16",
                                        "wsx_va_load_i16", "wsx_va_store_u8", "wsx_va_store_u16"});
    const bool need_va_mem   = need_bvh || need_va_a32 || need_va_a64 || need_va_sub ||
                               has_any({"wsx_va_", "wsx_code_load32"});
    const bool need_tex_a32  = has_any({"wsx_tex_atomic_op32"});
    const bool need_desc     = has_any({"wsx_lookup_"});
    const bool need_mask_bit = has_any({"wsx_mask_bit"});
    const bool need_sdwa_dpp = has_any({"wsx_sdwa_", "wsx_dpp", "wsx_ds_swizzle", "wsx_permlane16"});
    const bool need_bit_math = has_any({
        "wsx_med3_", "wsx_bfe_", "wsx_align", "wsx_perm_b32",
        "wsx_cube", "wsx_wqm32", "wsx_quadmask32", "wsx_frexp_",
        "wsx_fp_class_f32", "wsx_lerp_u8", "wsx_sad_u8", "wsx_cvt_pknorm_"
    });

    const std::string_view full(kSm66HlslPreamble);
    auto slice = [&](const char* start_tok, const char* end_tok) -> std::string_view {
        const size_t s = full.find(start_tok);
        if (s == std::string_view::npos) return {};
        const size_t e = end_tok ? full.find(end_tok, s) : full.size();
        if (e == std::string_view::npos || e <= s) return {};
        return full.substr(s, e - s);
    };

    std::string out;
    out.reserve(4096u);
    out += slice("// MizuBox Direct3D 12", "struct WsxDescHashEntry");
    if (need_desc) {
        out += slice("struct WsxDescHashEntry", "uint wsx_sgpr(");
    }
    out += slice("uint wsx_sgpr(", "void wsx_va_to_slab(");
    if (need_va_mem) {
        out += slice("void wsx_va_to_slab(", "uint wsx_va_load_u8(");
    }
    if (need_va_sub) {
        out += slice("uint wsx_va_load_u8(", "uint wsx_va_atomic_op32(");
    }
    if (need_va_a32) {
        out += slice("uint wsx_va_atomic_op32(", "uint64_t wsx_va_atomic_op64(");
    }
    if (need_va_a64) {
        out += slice("uint64_t wsx_va_atomic_op64(", "uint wsx_tex_atomic_op32(");
    }
    if (need_tex_a32) {
        out += slice("uint wsx_tex_atomic_op32(", "// Clean-room RDNA2 image_bvh_intersect_ray");
    }
    if (need_bvh) {
        out += slice("// Clean-room RDNA2 image_bvh_intersect_ray", "// Compact RDNA2 math / bit / wave helpers");
    }
    if (need_bit_math) {
        out += slice("// Compact RDNA2 math / bit / wave helpers", "uint wsx_sdwa_src(");
    }
    if (need_sdwa_dpp) {
        out += slice("uint wsx_sdwa_src(", "bool wsx_mask_bit(");
    }
    if (need_mask_bit) {
        out += slice("bool wsx_mask_bit(", "bool wsx_fp_class_f32(");
    }
    if (need_bit_math) {
        out += slice("bool wsx_fp_class_f32(", "// Reads an inline constant table dword");
    }
    if (need_va_mem) {
        out += slice("// Reads an inline constant table dword", "uint wsx_fnv1a_word(");
    }
    if (need_desc) {
        out += slice("uint wsx_fnv1a_word(", nullptr);
    }
    return out;
}

// ============================================================================
// Cached host GPU wave lane width (used by RDNA2 Wave64 auto-emulation)
// ============================================================================
inline std::atomic<uint32_t>& host_wave_lane_max_storage() noexcept {
    static std::atomic<uint32_t> v{0u};
    return v;
}

inline void set_host_wave_lane_max(uint32_t max_lanes) noexcept {
    if (max_lanes >= 16u) {
        host_wave_lane_max_storage().store(max_lanes, std::memory_order_relaxed);
    }
}

inline uint32_t get_host_wave_lane_max() noexcept {
    return host_wave_lane_max_storage().load(std::memory_order_relaxed);
}

// ============================================================================
// Query SM 6.6 / Dynamic Resources / Mesh Shader / DXR capabilities
// ============================================================================
inline HRESULT check_sm66_support(ID3D12Device* dev, Sm66Caps* out) noexcept {
    if (!dev || !out) return E_INVALIDARG;
    std::memset(out, 0, sizeof(*out));

    D3D12_FEATURE_DATA_SHADER_MODEL sm{};
    sm.HighestShaderModel = D3D_SHADER_MODEL_6_8;
    if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
        sm.HighestShaderModel = D3D_SHADER_MODEL_6_6;
        dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
    }
    out->HighestShaderModel = static_cast<uint32_t>(sm.HighestShaderModel);

    D3D12_FEATURE_DATA_D3D12_OPTIONS opts0{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts0, sizeof(opts0)))) {
        out->ResourceBindingTier = static_cast<uint32_t>(opts0.ResourceBindingTier);
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS1 opts1{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opts1, sizeof(opts1)))) {
        out->WaveLaneCountMin = opts1.WaveLaneCountMin;
        out->WaveLaneCountMax = opts1.WaveLaneCountMax;
        set_host_wave_lane_max(opts1.WaveLaneCountMax);
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5)))) {
        out->RaytracingTier = static_cast<uint32_t>(opts5.RaytracingTier);
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 opts7{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opts7, sizeof(opts7)))) {
        out->MeshShaderTier = static_cast<uint32_t>(opts7.MeshShaderTier);
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS9 opts9{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &opts9, sizeof(opts9)))) {
        out->SupportsInt64Atomics = opts9.AtomicInt64OnTypedResourceSupported ? 1u : 0u;
    }

    out->SupportsDynamicResources =
        (out->HighestShaderModel >= static_cast<uint32_t>(D3D_SHADER_MODEL_6_6) &&
         out->ResourceBindingTier >= static_cast<uint32_t>(D3D12_RESOURCE_BINDING_TIER_3))
            ? 1u : 0u;
    return S_OK;
}

// ============================================================================
// Standard SM 6.6 Xbox-contract Root Signature (40 DWORDS Root Constants + Direct Heap Indexing)
// ============================================================================
inline HRESULT create_sm66_root_signature(
    ID3D12Device* dev,
    decltype(&D3D12SerializeVersionedRootSignature) fn_serialize,
    REFIID riid,
    void** ppRootSignature) noexcept {
    if (!dev || !fn_serialize || !ppRootSignature) return E_INVALIDARG;
    *ppRootSignature = nullptr;

    D3D12_ROOT_PARAMETER1 param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.ShaderRegister = 0;
    param.Constants.RegisterSpace = 0;
    param.Constants.Num32BitValues = static_cast<UINT>(sizeof(Sm66UserData) / 4u); // 40 DWORDS
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = 1;
    desc.Desc_1_1.pParameters = &param;
    desc.Desc_1_1.NumStaticSamplers = 0;
    desc.Desc_1_1.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> blob, err;
    const HRESULT shr = fn_serialize(&desc, blob.GetAddressOf(), err.GetAddressOf());
    if (FAILED(shr) || !blob) return FAILED(shr) ? shr : E_FAIL;

    return dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), riid, ppRootSignature);
}

// ============================================================================
// Dynamic runtime dxcompiler.dll loader & SM 6.6 HLSL -> signed DXIL compiler
// ============================================================================
namespace dxc_detail {

struct DxcRuntime {
    std::mutex mtx;
    bool attempted = false;
    HMODULE mod_dxil = nullptr;
    HMODULE mod_dxc = nullptr;
    DxcCreateInstanceProc create_instance = nullptr;
};

inline DxcRuntime& dxc_runtime() noexcept {
    static DxcRuntime rt;
    return rt;
}

inline HMODULE try_load_from_dir(const std::filesystem::path& dir, HMODULE& out_dxil) noexcept {
    std::error_code ec;
    const auto dxc_path = dir / L"dxcompiler.dll";
    if (!std::filesystem::is_regular_file(dxc_path, ec)) return nullptr;
    const auto dxil_path = dir / L"dxil.dll";
    if (std::filesystem::is_regular_file(dxil_path, ec)) {
        out_dxil = LoadLibraryExW(dxil_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    return LoadLibraryExW(dxc_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

inline DxcCreateInstanceProc ensure_dxc_loaded() noexcept {
    auto& rt = dxc_runtime();
    std::lock_guard<std::mutex> lock(rt.mtx);
    if (rt.attempted) return rt.create_instance;
    rt.attempted = true;

    // 1. Optional WSX_DXC_DIR override
    wchar_t env_dir[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"WSX_DXC_DIR", env_dir, MAX_PATH) > 0) {
        rt.mod_dxc = try_load_from_dir(env_dir, rt.mod_dxil);
    }

    // 2. Alongside d3d12_xs.dll or current executable
    if (!rt.mod_dxc) {
        HMODULE self_mod = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&ensure_dxc_loaded), &self_mod) && self_mod) {
            wchar_t dll_path[MAX_PATH]{};
            if (GetModuleFileNameW(self_mod, dll_path, MAX_PATH) > 0) {
                rt.mod_dxc = try_load_from_dir(std::filesystem::path(dll_path).parent_path(), rt.mod_dxil);
            }
        }
    }
    if (!rt.mod_dxc) {
        wchar_t exe_path[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
            rt.mod_dxc = try_load_from_dir(std::filesystem::path(exe_path).parent_path(), rt.mod_dxil);
        }
    }

    // 3. Default PATH / System32
    if (!rt.mod_dxc) {
        rt.mod_dxil = LoadLibraryW(L"dxil.dll");
        rt.mod_dxc = LoadLibraryW(L"dxcompiler.dll");
    }

    // 4. Windows 10/11 SDK x64 bin directory (newest installed version first)
    if (!rt.mod_dxc) {
        const std::filesystem::path kits_root(L"C:\\Program Files (x86)\\Windows Kits\\10\\bin");
        std::error_code ec;
        if (std::filesystem::is_directory(kits_root, ec)) {
            std::vector<std::filesystem::path> versions;
            for (const auto& entry : std::filesystem::directory_iterator(kits_root, ec)) {
                if (entry.is_directory(ec)) {
                    const auto candidate = entry.path() / L"x64";
                    if (std::filesystem::is_regular_file(candidate / L"dxcompiler.dll", ec)) {
                        versions.push_back(candidate);
                    }
                }
            }
            std::sort(versions.begin(), versions.end(), std::greater<std::filesystem::path>());
            for (const auto& dir : versions) {
                rt.mod_dxc = try_load_from_dir(dir, rt.mod_dxil);
                if (rt.mod_dxc) break;
            }
        }
    }

    if (rt.mod_dxc) {
        rt.create_instance = reinterpret_cast<DxcCreateInstanceProc>(
            GetProcAddress(rt.mod_dxc, "DxcCreateInstance"));
    }
    return rt.create_instance;
}

} // namespace dxc_detail

inline HRESULT compile_hlsl_to_dxil_vec(
    const char* hlsl_source,
    size_t source_len,
    const wchar_t* entry_point,
    const wchar_t* target_profile,
    std::vector<uint8_t>& out_dxil,
    char* out_errors,
    size_t err_cap) noexcept {
    out_dxil.clear();
    if (!hlsl_source || source_len == 0 || !entry_point || !target_profile) {
        return E_INVALIDARG;
    }
    if (out_errors && err_cap > 0) out_errors[0] = '\0';

    auto fn_create = dxc_detail::ensure_dxc_loaded();
    if (!fn_create) {
        if (out_errors && err_cap > 0) {
            std::snprintf(out_errors, err_cap, "dxcompiler.dll not found (set WSX_DXC_DIR or install Windows 10/11 SDK)");
        }
        return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    }

    ComPtr<IDxcCompiler3> compiler;
    HRESULT hr = fn_create(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.GetAddressOf()));
    if (FAILED(hr)) return hr;

    DxcBuffer src_buf{};
    src_buf.Ptr = hlsl_source;
    src_buf.Size = source_len;
    src_buf.Encoding = DXC_CP_UTF8;

    // Keep -O3 for structured/compact HLSL (up to 96 KB) and fall back to -O1 only on huge shaders.
    // Optional override: WSX_DXC_OPT_LEVEL=0|1|2|3.
    const wchar_t* opt_flag = (source_len > 98304u) ? L"-O1" : L"-O3";
    wchar_t opt_env[8]{};
    if (GetEnvironmentVariableW(L"WSX_DXC_OPT_LEVEL", opt_env, 8) > 0) {
        if (opt_env[0] == L'0') opt_flag = L"-Od";
        else if (opt_env[0] == L'1') opt_flag = L"-O1";
        else if (opt_env[0] == L'2') opt_flag = L"-O2";
        else if (opt_env[0] == L'3') opt_flag = L"-O3";
    }
    std::vector<const wchar_t*> args = {
        L"wsx_sm66_shader.hlsl",
        L"-E", entry_point,
        L"-T", target_profile,
        L"-HV", L"2021",
        opt_flag,
    };
    const std::string_view src_sv(hlsl_source, source_len);
    if (src_sv.find("float16_t") != std::string_view::npos ||
        src_sv.find("uint16_t") != std::string_view::npos) {
        args.push_back(L"-enable-16bit-types");
    }

    ComPtr<IDxcResult> result;
    hr = compiler->Compile(&src_buf, args.data(), static_cast<UINT32>(args.size()),
                           nullptr, IID_PPV_ARGS(result.GetAddressOf()));
    if (FAILED(hr) || !result) return FAILED(hr) ? hr : E_FAIL;

    ComPtr<IDxcBlobUtf8> errors;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(errors.GetAddressOf()), nullptr)) &&
        errors && errors->GetStringLength() > 0 && out_errors && err_cap > 0) {
        const size_t copy_n = std::min(err_cap - 1u, errors->GetStringLength());
        std::memcpy(out_errors, errors->GetStringPointer(), copy_n);
        out_errors[copy_n] = '\0';
    }

    HRESULT compile_status = E_FAIL;
    if (FAILED(result->GetStatus(&compile_status)) || FAILED(compile_status)) {
        return FAILED(compile_status) ? compile_status : E_FAIL;
    }

    ComPtr<IDxcBlob> obj;
    hr = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(obj.GetAddressOf()), nullptr);
    if (FAILED(hr) || !obj) return FAILED(hr) ? hr : E_FAIL;

    const size_t obj_size = obj->GetBufferSize();
    out_dxil.resize(obj_size);
    if (obj_size > 0) {
        std::memcpy(out_dxil.data(), obj->GetBufferPointer(), obj_size);
    }
    return S_OK;
}

inline HRESULT compile_hlsl_to_dxil(
    const char* hlsl_source,
    size_t source_len,
    const wchar_t* entry_point,
    const wchar_t* target_profile,
    void* out_dxil,
    size_t out_cap,
    size_t* out_size,
    char* out_errors,
    size_t err_cap) noexcept {
    if (!out_size) return E_INVALIDARG;
    *out_size = 0;
    std::vector<uint8_t> compiled;
    const HRESULT hr = compile_hlsl_to_dxil_vec(
        hlsl_source, source_len, entry_point, target_profile, compiled, out_errors, err_cap);
    if (FAILED(hr)) return hr;
    *out_size = compiled.size();
    if (!out_dxil) return S_OK;
    if (out_cap < compiled.size()) return E_NOT_SUFFICIENT_BUFFER;
    if (!compiled.empty()) {
        std::memcpy(out_dxil, compiled.data(), compiled.size());
    }
    return S_OK;
}

} // namespace wsx_sm66
