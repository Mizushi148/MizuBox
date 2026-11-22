// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Shared clean-room Xbox Series X|S (RDNA2 / Scarlett) Texture Tiling & Swizzle Layout Math:
// - Used by both xg_xs.dll (XGTextureAddressComputer) and d3d12_xs.dll (UMA guest texture upload).
// - Supports LINEAR, 4KB_S / 4KB_D (4096-byte micro-tiles), and 64KB_S / 64KB_D (65536-byte macro-tiles)
//   for uncompressed (1, 2, 4, 8, 16 Bpp) and block-compressed (BC1..BC7) DXGI formats.

#pragma once

#include <dxgiformat.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace wsx_xg {

enum SwizzleMode : uint32_t {
    SWIZZLE_LINEAR   = 0u,
    SWIZZLE_256B_S   = 1u,
    SWIZZLE_256B_D   = 2u,
    SWIZZLE_4KB_Z    = 4u,
    SWIZZLE_4KB_S    = 5u,
    SWIZZLE_4KB_D    = 6u,
    SWIZZLE_64KB_Z   = 8u,
    SWIZZLE_64KB_S   = 9u,
    SWIZZLE_64KB_D   = 10u,
    SWIZZLE_64KB_Z_T = 16u,
    SWIZZLE_64KB_S_T = 17u,
    SWIZZLE_64KB_D_T = 18u,
    SWIZZLE_64KB_Z_X = 24u,
    SWIZZLE_64KB_S_X = 25u,
    SWIZZLE_64KB_D_X = 26u,
};

struct FormatInfo {
    uint32_t bytes_per_element = 4u; // Bytes per pixel OR per 4x4 BC block
    uint32_t block_dim = 1u;         // 1 for uncompressed, 4 for BC1..BC7
    bool is_bc = false;
};

inline FormatInfo get_format_info(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM:
        return {1u, 1u, false};
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
        return {2u, 1u, false};
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
        return {8u, 1u, false};
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return {16u, 1u, false};
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        return {8u, 4u, true};
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return {16u, 4u, true};
    default:
        return {4u, 1u, false}; // RGBA8 / BGRA8 / R32F / D32F / R10G10B10A2 / R11G11B10F
    }
}

inline bool is_linear_mode(uint32_t mode) noexcept {
    return mode == SWIZZLE_LINEAR;
}

inline bool is_4kb_mode(uint32_t mode) noexcept {
    return mode == SWIZZLE_4KB_Z || mode == SWIZZLE_4KB_S || mode == SWIZZLE_4KB_D;
}

inline bool is_64kb_mode(uint32_t mode) noexcept {
    return mode == SWIZZLE_64KB_Z || mode == SWIZZLE_64KB_S || mode == SWIZZLE_64KB_D ||
           mode == SWIZZLE_64KB_Z_T || mode == SWIZZLE_64KB_S_T || mode == SWIZZLE_64KB_D_T ||
           mode == SWIZZLE_64KB_Z_X || mode == SWIZZLE_64KB_S_X || mode == SWIZZLE_64KB_D_X;
}

inline bool is_xor_mode(uint32_t mode) noexcept {
    return mode == SWIZZLE_64KB_Z_X || mode == SWIZZLE_64KB_S_X || mode == SWIZZLE_64KB_D_X;
}

// RDNA2 64KB_*_X pipe/bank XOR swizzle: bijectively permutes intra-tile 256-byte pipe-bank bits [8..13]
// based on macro-tile coordinates (tx, ty) + optional pipe_bank_xor seed.
inline uint64_t apply_rdna2_pipe_bank_xor(
    uint64_t in_tile_byte_offset,
    uint32_t tx,
    uint32_t ty,
    uint32_t mode,
    uint32_t pipe_bank_xor = 0u) noexcept {
    if (!is_xor_mode(mode)) return in_tile_byte_offset;
    const uint32_t xor_bits = ((tx & 0x3Fu) ^ ((ty & 0x3Fu) << 1) ^ (pipe_bank_xor & 0x3Fu)) & 0x3Fu;
    return in_tile_byte_offset ^ (static_cast<uint64_t>(xor_bits) << 8);
}

inline void tile_dims_3d_elements(uint32_t mode, uint32_t bpe, uint32_t& out_tw, uint32_t& out_th, uint32_t& out_td) noexcept {
    if (is_linear_mode(mode)) {
        out_tw = 1u;
        out_th = 1u;
        out_td = 1u;
        return;
    }
    const uint32_t tile_bytes = is_64kb_mode(mode) ? 65536u : (is_4kb_mode(mode) ? 4096u : 256u);
    const uint32_t elems = std::max(1u, tile_bytes / std::max(1u, bpe));
    uint32_t log2_e = 0u;
    while ((1u << log2_e) < elems && log2_e < 20u) ++log2_e;
    // Distribute log2_e bits evenly across (X, Y, Z) for 3D macro-tiles (e.g. 64KB @ 4Bpp -> 16x16x16? 14 bits -> 5,5,4 => 32x32x16)
    const uint32_t log2_z = log2_e / 3u;
    const uint32_t log2_y = (log2_e - log2_z) / 2u;
    const uint32_t log2_x = log2_e - log2_z - log2_y;
    out_tw = 1u << log2_x;
    out_th = 1u << log2_y;
    out_td = 1u << log2_z;
}

inline uint32_t morton_index_3d(uint32_t lx, uint32_t ly, uint32_t lz, uint32_t tw, uint32_t th, uint32_t td) noexcept {
    uint32_t idx = 0u;
    uint32_t out_bit = 0u;
    uint32_t bx = 0u, by = 0u, bz = 0u;
    while ((1u << bx) < tw || (1u << by) < th || (1u << bz) < td) {
        if ((1u << bx) < tw) {
            idx |= (((lx >> bx) & 1u) << out_bit++);
            ++bx;
        }
        if ((1u << by) < th) {
            idx |= (((ly >> by) & 1u) << out_bit++);
            ++by;
        }
        if ((1u << bz) < td) {
            idx |= (((lz >> bz) & 1u) << out_bit++);
            ++bz;
        }
    }
    return idx;
}

inline void tile_dims_elements(uint32_t mode, uint32_t bpe, uint32_t& out_tw, uint32_t& out_th) noexcept {
    if (is_linear_mode(mode)) {
        out_tw = 1u;
        out_th = 1u;
        return;
    }
    const uint32_t tile_bytes = is_64kb_mode(mode) ? 65536u : (is_4kb_mode(mode) ? 4096u : 256u);
    const uint32_t elems = std::max(1u, tile_bytes / std::max(1u, bpe));
    uint32_t log2_e = 0;
    while ((1u << log2_e) < elems && log2_e < 20u) ++log2_e;
    const uint32_t log2_w = (log2_e + 1u) / 2u;
    const uint32_t log2_h = log2_e - log2_w;
    out_tw = 1u << log2_w;
    out_th = 1u << log2_h;
}

inline uint32_t morton_index_rect(uint32_t lx, uint32_t ly, uint32_t tw, uint32_t th) noexcept {
    uint32_t idx = 0u;
    uint32_t out_bit = 0u;
    uint32_t bx = 0u, by = 0u;
    while ((1u << bx) < tw || (1u << by) < th) {
        if ((1u << bx) < tw) {
            idx |= (((lx >> bx) & 1u) << out_bit++);
            ++bx;
        }
        if ((1u << by) < th) {
            idx |= (((ly >> by) & 1u) << out_bit++);
            ++by;
        }
    }
    return idx;
}

inline uint64_t align_up(uint64_t v, uint64_t a) noexcept {
    return (a <= 1u) ? v : ((v + a - 1u) & ~(a - 1u));
}

struct SubresourceLayout {
    uint64_t offset_bytes = 0;
    uint64_t size_bytes = 0;
    uint32_t width_elems = 1;
    uint32_t height_elems = 1;
    uint32_t padded_width_elems = 1;
    uint32_t padded_height_elems = 1;
    uint32_t row_pitch_bytes = 0;
    uint32_t bytes_per_element = 4;
    uint32_t tile_w = 1;
    uint32_t tile_h = 1;
    uint32_t swizzle_mode = SWIZZLE_LINEAR;
};

inline SubresourceLayout compute_subresource_2d(
    uint32_t width_px,
    uint32_t height_px,
    uint32_t mip_level,
    DXGI_FORMAT fmt,
    uint32_t swizzle_mode,
    uint64_t base_offset = 0) noexcept {
    const FormatInfo fi = get_format_info(fmt);
    const uint32_t mw = std::max(1u, width_px >> mip_level);
    const uint32_t mh = std::max(1u, height_px >> mip_level);
    const uint32_t ew = (mw + fi.block_dim - 1u) / fi.block_dim;
    const uint32_t eh = (mh + fi.block_dim - 1u) / fi.block_dim;

    SubresourceLayout sl{};
    sl.width_elems = ew;
    sl.height_elems = eh;
    sl.bytes_per_element = fi.bytes_per_element;
    sl.swizzle_mode = swizzle_mode;
    tile_dims_elements(swizzle_mode, fi.bytes_per_element, sl.tile_w, sl.tile_h);

    if (is_linear_mode(swizzle_mode)) {
        sl.row_pitch_bytes = static_cast<uint32_t>(align_up(static_cast<uint64_t>(ew) * fi.bytes_per_element, 256u));
        sl.padded_width_elems = sl.row_pitch_bytes / fi.bytes_per_element;
        sl.padded_height_elems = eh;
        sl.offset_bytes = align_up(base_offset, 256u);
        sl.size_bytes = static_cast<uint64_t>(sl.row_pitch_bytes) * eh;
    } else {
        const uint64_t tile_align = is_64kb_mode(swizzle_mode) ? 65536u : (is_4kb_mode(swizzle_mode) ? 4096u : 256u);
        sl.padded_width_elems = static_cast<uint32_t>(align_up(ew, sl.tile_w));
        sl.padded_height_elems = static_cast<uint32_t>(align_up(eh, sl.tile_h));
        sl.row_pitch_bytes = sl.padded_width_elems * fi.bytes_per_element;
        sl.offset_bytes = align_up(base_offset, tile_align);
        sl.size_bytes = align_up(
            static_cast<uint64_t>(sl.padded_width_elems) * sl.padded_height_elems * fi.bytes_per_element,
            tile_align);
    }
    return sl;
}

inline uint64_t element_offset_in_subresource(const SubresourceLayout& sl, uint32_t ex, uint32_t ey) noexcept {
    if (is_linear_mode(sl.swizzle_mode)) {
        return static_cast<uint64_t>(ey) * sl.row_pitch_bytes + static_cast<uint64_t>(ex) * sl.bytes_per_element;
    }
    const uint32_t tx = ex / sl.tile_w;
    const uint32_t ty = ey / sl.tile_h;
    const uint32_t lx = ex % sl.tile_w;
    const uint32_t ly = ey % sl.tile_h;
    const uint32_t tiles_per_row = sl.padded_width_elems / sl.tile_w;
    const uint64_t tile_bytes = static_cast<uint64_t>(sl.tile_w) * sl.tile_h * sl.bytes_per_element;
    const uint64_t tile_idx = static_cast<uint64_t>(ty) * tiles_per_row + tx;
    const uint64_t in_tile_raw = static_cast<uint64_t>(morton_index_rect(lx, ly, sl.tile_w, sl.tile_h)) * sl.bytes_per_element;
    const uint64_t in_tile = apply_rdna2_pipe_bank_xor(in_tile_raw, tx, ty, sl.swizzle_mode);
    return tile_idx * tile_bytes + in_tile;
}

// MSAA sample-interleaved offset (2x/4x/8x): groups samples inside micro-tile blocks
inline uint64_t element_offset_msaa(
    const SubresourceLayout& sl,
    uint32_t ex,
    uint32_t ey,
    uint32_t sample_idx,
    uint32_t sample_count) noexcept {
    const uint32_t sc = std::max(1u, sample_count);
    const uint32_t s = std::min(sample_idx, sc - 1u);
    const uint64_t base = element_offset_in_subresource(sl, ex, ey);
    if (sc <= 1u) return base;
    return base * sc + static_cast<uint64_t>(s) * sl.bytes_per_element;
}

struct SubresourceLayout3D {
    uint64_t offset_bytes = 0;
    uint64_t size_bytes = 0;
    uint32_t width_elems = 1;
    uint32_t height_elems = 1;
    uint32_t depth_elems = 1;
    uint32_t padded_width_elems = 1;
    uint32_t padded_height_elems = 1;
    uint32_t padded_depth_elems = 1;
    uint32_t row_pitch_bytes = 0;
    uint32_t slice_pitch_bytes = 0;
    uint32_t bytes_per_element = 4;
    uint32_t tile_w = 1;
    uint32_t tile_h = 1;
    uint32_t tile_d = 1;
    uint32_t swizzle_mode = SWIZZLE_LINEAR;
};

inline SubresourceLayout3D compute_subresource_3d(
    uint32_t width_px,
    uint32_t height_px,
    uint32_t depth_px,
    uint32_t mip_level,
    DXGI_FORMAT fmt,
    uint32_t swizzle_mode,
    uint64_t base_offset = 0) noexcept {
    const FormatInfo fi = get_format_info(fmt);
    const uint32_t mw = std::max(1u, width_px >> mip_level);
    const uint32_t mh = std::max(1u, height_px >> mip_level);
    const uint32_t md = std::max(1u, depth_px >> mip_level);
    const uint32_t ew = (mw + fi.block_dim - 1u) / fi.block_dim;
    const uint32_t eh = (mh + fi.block_dim - 1u) / fi.block_dim;

    SubresourceLayout3D sl{};
    sl.width_elems = ew;
    sl.height_elems = eh;
    sl.depth_elems = md;
    sl.bytes_per_element = fi.bytes_per_element;
    sl.swizzle_mode = swizzle_mode;
    tile_dims_3d_elements(swizzle_mode, fi.bytes_per_element, sl.tile_w, sl.tile_h, sl.tile_d);

    if (is_linear_mode(swizzle_mode)) {
        sl.row_pitch_bytes = static_cast<uint32_t>(align_up(static_cast<uint64_t>(ew) * fi.bytes_per_element, 256u));
        sl.padded_width_elems = sl.row_pitch_bytes / fi.bytes_per_element;
        sl.padded_height_elems = eh;
        sl.padded_depth_elems = md;
        sl.slice_pitch_bytes = sl.row_pitch_bytes * eh;
        sl.offset_bytes = align_up(base_offset, 256u);
        sl.size_bytes = static_cast<uint64_t>(sl.slice_pitch_bytes) * md;
    } else {
        const uint64_t tile_align = is_64kb_mode(swizzle_mode) ? 65536u : (is_4kb_mode(swizzle_mode) ? 4096u : 256u);
        sl.padded_width_elems = static_cast<uint32_t>(align_up(ew, sl.tile_w));
        sl.padded_height_elems = static_cast<uint32_t>(align_up(eh, sl.tile_h));
        sl.padded_depth_elems = static_cast<uint32_t>(align_up(md, sl.tile_d));
        sl.row_pitch_bytes = sl.padded_width_elems * fi.bytes_per_element;
        sl.slice_pitch_bytes = sl.row_pitch_bytes * sl.padded_height_elems;
        sl.offset_bytes = align_up(base_offset, tile_align);
        sl.size_bytes = align_up(
            static_cast<uint64_t>(sl.padded_width_elems) * sl.padded_height_elems * sl.padded_depth_elems * fi.bytes_per_element,
            tile_align);
    }
    return sl;
}

inline uint64_t element_offset_3d(const SubresourceLayout3D& sl, uint32_t ex, uint32_t ey, uint32_t ez) noexcept {
    if (is_linear_mode(sl.swizzle_mode)) {
        return static_cast<uint64_t>(ez) * sl.slice_pitch_bytes +
               static_cast<uint64_t>(ey) * sl.row_pitch_bytes +
               static_cast<uint64_t>(ex) * sl.bytes_per_element;
    }
    const uint32_t tx = ex / sl.tile_w;
    const uint32_t ty = ey / sl.tile_h;
    const uint32_t tz = ez / sl.tile_d;
    const uint32_t lx = ex % sl.tile_w;
    const uint32_t ly = ey % sl.tile_h;
    const uint32_t lz = ez % sl.tile_d;
    const uint32_t tiles_x = sl.padded_width_elems / sl.tile_w;
    const uint32_t tiles_y = sl.padded_height_elems / sl.tile_h;
    const uint64_t tile_bytes = static_cast<uint64_t>(sl.tile_w) * sl.tile_h * sl.tile_d * sl.bytes_per_element;
    const uint64_t tile_idx = (static_cast<uint64_t>(tz) * tiles_y + ty) * tiles_x + tx;
    const uint64_t in_tile_raw = static_cast<uint64_t>(morton_index_3d(lx, ly, lz, sl.tile_w, sl.tile_h, sl.tile_d)) * sl.bytes_per_element;
    const uint64_t in_tile = apply_rdna2_pipe_bank_xor(in_tile_raw, tx ^ tz, ty, sl.swizzle_mode);
    return tile_idx * tile_bytes + in_tile;
}

inline void swizzle_3d(
    const SubresourceLayout3D& sl,
    const uint8_t* linear_src,
    uint32_t linear_row_pitch,
    uint32_t linear_slice_pitch,
    uint8_t* tiled_dst) noexcept {
    if (!linear_src || !tiled_dst) return;
    for (uint32_t z = 0; z < sl.depth_elems; ++z) {
        const uint8_t* src_slice = linear_src + static_cast<size_t>(z) * linear_slice_pitch;
        for (uint32_t y = 0; y < sl.height_elems; ++y) {
            const uint8_t* src_row = src_slice + static_cast<size_t>(y) * linear_row_pitch;
            for (uint32_t x = 0; x < sl.width_elems; ++x) {
                const uint64_t dst_off = element_offset_3d(sl, x, y, z);
                std::memcpy(tiled_dst + dst_off, src_row + static_cast<size_t>(x) * sl.bytes_per_element, sl.bytes_per_element);
            }
        }
    }
}

inline void deswizzle_3d(
    const SubresourceLayout3D& sl,
    const uint8_t* tiled_src,
    uint8_t* linear_dst,
    uint32_t linear_row_pitch,
    uint32_t linear_slice_pitch) noexcept {
    if (!tiled_src || !linear_dst) return;
    for (uint32_t z = 0; z < sl.depth_elems; ++z) {
        uint8_t* dst_slice = linear_dst + static_cast<size_t>(z) * linear_slice_pitch;
        for (uint32_t y = 0; y < sl.height_elems; ++y) {
            uint8_t* dst_row = dst_slice + static_cast<size_t>(y) * linear_row_pitch;
            for (uint32_t x = 0; x < sl.width_elems; ++x) {
                const uint64_t src_off = element_offset_3d(sl, x, y, z);
                std::memcpy(dst_row + static_cast<size_t>(x) * sl.bytes_per_element, tiled_src + src_off, sl.bytes_per_element);
            }
        }
    }
}

// Swizzle linear row-major source into console tiled destination
inline void swizzle_2d(
    const SubresourceLayout& sl,
    const uint8_t* linear_src,
    uint32_t linear_row_pitch,
    uint8_t* tiled_dst) noexcept {
    if (!linear_src || !tiled_dst) return;
    if (is_linear_mode(sl.swizzle_mode)) {
        const size_t row_bytes = static_cast<size_t>(sl.width_elems) * sl.bytes_per_element;
        for (uint32_t y = 0; y < sl.height_elems; ++y) {
            std::memcpy(tiled_dst + static_cast<size_t>(y) * sl.row_pitch_bytes,
                        linear_src + static_cast<size_t>(y) * linear_row_pitch, row_bytes);
        }
        return;
    }
    for (uint32_t y = 0; y < sl.height_elems; ++y) {
        const uint8_t* src_row = linear_src + static_cast<size_t>(y) * linear_row_pitch;
        for (uint32_t x = 0; x < sl.width_elems; ++x) {
            const uint64_t dst_off = element_offset_in_subresource(sl, x, y);
            std::memcpy(tiled_dst + dst_off, src_row + static_cast<size_t>(x) * sl.bytes_per_element, sl.bytes_per_element);
        }
    }
}

// Deswizzle console tiled source into desktop D3D12 linear upload destination
inline void deswizzle_2d(
    const SubresourceLayout& sl,
    const uint8_t* tiled_src,
    uint8_t* linear_dst,
    uint32_t linear_row_pitch) noexcept {
    if (!tiled_src || !linear_dst) return;
    if (is_linear_mode(sl.swizzle_mode)) {
        const size_t row_bytes = static_cast<size_t>(sl.width_elems) * sl.bytes_per_element;
        for (uint32_t y = 0; y < sl.height_elems; ++y) {
            std::memcpy(linear_dst + static_cast<size_t>(y) * linear_row_pitch,
                        tiled_src + static_cast<size_t>(y) * sl.row_pitch_bytes, row_bytes);
        }
        return;
    }
    for (uint32_t y = 0; y < sl.height_elems; ++y) {
        uint8_t* dst_row = linear_dst + static_cast<size_t>(y) * linear_row_pitch;
        for (uint32_t x = 0; x < sl.width_elems; ++x) {
            const uint64_t src_off = element_offset_in_subresource(sl, x, y);
            std::memcpy(dst_row + static_cast<size_t>(x) * sl.bytes_per_element, tiled_src + src_off, sl.bytes_per_element);
        }
    }
}

} // namespace wsx_xg
