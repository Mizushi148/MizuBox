// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox Series X|S BCPack Texture Codec & Resident Mip-Tail Fallback:
// - Lossless planar endpoint/index splitting + scanline delta coding + deflate packing for
//   BC1 (DXT1, 8B), BC3 (DXT5, 16B), BC4 (8B), BC5 (16B), and BC7 (16B) GPU textures.
// - Auto-handles GDeflate / zlib-wrapped BCn payloads transparently.
// - Provides resident mip-tail upsampling fallback (BC1/BC3/BC4/BC5/BC7 block replication)
//   so truncated or unrecognized streaming mip chunks degrade smoothly instead of crashing
//   or showing uninitialized VRAM garbage.
// - Pure C++20, zero external dependencies (uses clean-room wsx_inflate.h & wsx_gdeflate.h).

#pragma once

#include "wsx_inflate.h"
#include "wsx_gdeflate.h"

#include <dxgiformat.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wsx_bcpack {

constexpr uint32_t kBcPackMagic = 0x4B504342u; // "BCPK"
constexpr uint16_t kBcPackVersion = 1u;

enum class BcKind : uint8_t {
    Unknown = 0,
    BC1 = 1, // 8 bytes/block  (4B color endpoints + 4B color indices)
    BC3 = 3, // 16 bytes/block (2B alpha ep + 6B alpha idx + 4B color ep + 4B color idx)
    BC4 = 4, // 8 bytes/block  (2B red ep + 6B red idx)
    BC5 = 5, // 16 bytes/block (2B r ep + 6B r idx + 2B g ep + 6B g idx)
    BC7 = 7, // 16 bytes/block (4x4 byte-transposed planar lanes for high entropy reduction)
};

inline BcKind classify_dxgi_bc_format(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
        return BcKind::BC1;
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
        return BcKind::BC3;
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        return BcKind::BC4;
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
        return BcKind::BC5;
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return BcKind::BC7;
    default:
        return BcKind::Unknown;
    }
}

inline uint32_t block_bytes_for(BcKind k) noexcept {
    switch (k) {
    case BcKind::BC1:
    case BcKind::BC4:
        return 8u;
    case BcKind::BC3:
    case BcKind::BC5:
    case BcKind::BC7:
        return 16u;
    default:
        return 0u;
    }
}

#pragma pack(push, 1)
struct BcPackHeader {
    uint32_t magic;            // kBcPackMagic ("BCPK")
    uint16_t version;          // kBcPackVersion (1)
    uint8_t bc_kind;           // BcKind
    uint8_t flags;             // bit 0: delta-coded endpoints, bit 1: RLE/LZ stream
    uint32_t block_count;      // Number of 4x4 BC blocks
    uint32_t uncompressed_size;// block_count * block_bytes
    uint32_t payload_size;     // Compressed planar stream byte count
    uint32_t crc32_uncomp;     // IEEE CRC32 integrity check on uncompressed BCn data
};
#pragma pack(pop)
static_assert(sizeof(BcPackHeader) == 24u, "BcPackHeader must be 24 bytes");

namespace detail {

inline uint32_t crc32_ieee(const uint8_t* data, size_t len) noexcept {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// Fast clean-room LZSS + RLE byte compressor (zero external libs, decodes in O(N))
// Token stream:
//   0x00..0x7F: literal run of (tok + 1) bytes (1..128)
//   0x80..0xBF: RLE repeat of next 1 byte, length = (tok - 0x80) + 3 (3..66)
//   0xC0..0xFF: LZ back-reference: len = (tok - 0xC0) + 4 (4..67), followed by uint16_t dist (1..65535)
inline void lz_rle_compress(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(n / 2u + 64u);
    std::vector<uint8_t> lits;
    lits.reserve(128u);

    auto flush_lits = [&]() {
        size_t off = 0u;
        while (off < lits.size()) {
            const size_t chunk = std::min<size_t>(128u, lits.size() - off);
            out.push_back(static_cast<uint8_t>(chunk - 1u));
            out.insert(out.end(), lits.begin() + static_cast<ptrdiff_t>(off),
                       lits.begin() + static_cast<ptrdiff_t>(off + chunk));
            off += chunk;
        }
        lits.clear();
    };

    // 4-byte rolling hash table for O(1) LZ match lookup
    std::vector<int32_t> head(16384u, -1);
    auto hash4 = [](const uint8_t* p) -> uint32_t {
        const uint32_t v = static_cast<uint32_t>(p[0]) |
                           (static_cast<uint32_t>(p[1]) << 8) |
                           (static_cast<uint32_t>(p[2]) << 16) |
                           (static_cast<uint32_t>(p[3]) << 24);
        return ((v * 2654435761u) >> 18) & 16383u;
    };

    size_t i = 0u;
    while (i < n) {
        // 1. Check RLE run >= 3
        size_t rle_len = 1u;
        const size_t max_rle = std::min<size_t>(66u, n - i);
        while (rle_len < max_rle && src[i + rle_len] == src[i]) {
            ++rle_len;
        }
        if (rle_len >= 3u) {
            flush_lits();
            out.push_back(static_cast<uint8_t>(0x80u + (rle_len - 3u)));
            out.push_back(src[i]);
            for (size_t k = 0; k < rle_len && i + k + 4u <= n; ++k) {
                head[hash4(src + i + k)] = static_cast<int32_t>(i + k);
            }
            i += rle_len;
            continue;
        }

        // 2. Check 4+ byte LZ back-reference within 65535 bytes
        if (i + 4u <= n) {
            const uint32_t h = hash4(src + i);
            const int32_t prev = head[h];
            head[h] = static_cast<int32_t>(i);
            if (prev >= 0 && (i - static_cast<size_t>(prev)) >= 1u &&
                (i - static_cast<size_t>(prev)) <= 65535u) {
                const size_t dist = i - static_cast<size_t>(prev);
                size_t mlen = 0u;
                const size_t max_m = std::min<size_t>(67u, n - i);
                while (mlen < max_m && src[static_cast<size_t>(prev) + mlen] == src[i + mlen]) {
                    ++mlen;
                }
                if (mlen >= 4u) {
                    flush_lits();
                    out.push_back(static_cast<uint8_t>(0xC0u + (mlen - 4u)));
                    out.push_back(static_cast<uint8_t>(dist & 0xFFu));
                    out.push_back(static_cast<uint8_t>((dist >> 8) & 0xFFu));
                    for (size_t k = 1u; k < mlen && i + k + 4u <= n; ++k) {
                        head[hash4(src + i + k)] = static_cast<int32_t>(i + k);
                    }
                    i += mlen;
                    continue;
                }
            }
        }

        lits.push_back(src[i]);
        if (lits.size() == 128u) flush_lits();
        ++i;
    }
    flush_lits();
}

inline bool lz_rle_decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t expected_len) noexcept {
    size_t sp = 0u;
    size_t dp = 0u;
    while (sp < src_len && dp < expected_len) {
        const uint8_t tok = src[sp++];
        if (tok < 0x80u) {
            const size_t len = static_cast<size_t>(tok) + 1u;
            if (sp + len > src_len || dp + len > expected_len) return false;
            std::memcpy(dst + dp, src + sp, len);
            sp += len;
            dp += len;
        } else if (tok < 0xC0u) {
            const size_t len = static_cast<size_t>(tok - 0x80u) + 3u;
            if (sp + 1u > src_len || dp + len > expected_len) return false;
            const uint8_t val = src[sp++];
            std::memset(dst + dp, val, len);
            dp += len;
        } else {
            const size_t len = static_cast<size_t>(tok - 0xC0u) + 4u;
            if (sp + 2u > src_len || dp + len > expected_len) return false;
            const size_t dist = static_cast<size_t>(src[sp]) | (static_cast<size_t>(src[sp + 1u]) << 8);
            sp += 2u;
            if (dist == 0u || dist > dp) return false;
            for (size_t k = 0; k < len; ++k) {
                dst[dp + k] = dst[dp + k - dist];
            }
            dp += len;
        }
    }
    return dp == expected_len && sp == src_len;
}

// Planar split + delta-coding of BCn blocks (mirrors hardware BCPack endpoint/index decorrelation)
inline void split_planes(const uint8_t* blocks, uint32_t nblocks, BcKind kind, std::vector<uint8_t>& planar) {
    const uint32_t bsz = block_bytes_for(kind);
    planar.resize(static_cast<size_t>(nblocks) * bsz);
    uint8_t* p = planar.data();

    if (kind == BcKind::BC1) {
        // Plane 0: delta-coded uint16_t color0, Plane 1: delta-coded uint16_t color1, Plane 2: 4B indices
        auto* ep0 = reinterpret_cast<uint16_t*>(p);
        auto* ep1 = reinterpret_cast<uint16_t*>(p + nblocks * 2u);
        uint8_t* idx = p + nblocks * 4u;
        uint16_t prev0 = 0u, prev1 = 0u;
        for (uint32_t i = 0; i < nblocks; ++i) {
            const uint8_t* b = blocks + static_cast<size_t>(i) * 8u;
            uint16_t c0 = 0, c1 = 0;
            std::memcpy(&c0, b + 0, 2);
            std::memcpy(&c1, b + 2, 2);
            ep0[i] = static_cast<uint16_t>(c0 - prev0);
            ep1[i] = static_cast<uint16_t>(c1 - prev1);
            prev0 = c0;
            prev1 = c1;
            std::memcpy(idx + static_cast<size_t>(i) * 4u, b + 4, 4);
        }
    } else if (kind == BcKind::BC4) {
        // Plane 0: delta-coded alpha/red ep0, Plane 1: delta-coded ep1, Plane 2: 6B indices
        uint8_t* ep0 = p;
        uint8_t* ep1 = p + nblocks;
        uint8_t* idx = p + nblocks * 2u;
        uint8_t prev0 = 0u, prev1 = 0u;
        for (uint32_t i = 0; i < nblocks; ++i) {
            const uint8_t* b = blocks + static_cast<size_t>(i) * 8u;
            ep0[i] = static_cast<uint8_t>(b[0] - prev0);
            ep1[i] = static_cast<uint8_t>(b[1] - prev1);
            prev0 = b[0];
            prev1 = b[1];
            std::memcpy(idx + static_cast<size_t>(i) * 6u, b + 2, 6);
        }
    } else if (kind == BcKind::BC3) {
        // Alpha endpoints (2B), Color endpoints (4B), Alpha indices (6B), Color indices (4B)
        uint8_t* a0 = p;
        uint8_t* a1 = p + nblocks;
        auto* c0p = reinterpret_cast<uint16_t*>(p + nblocks * 2u);
        auto* c1p = reinterpret_cast<uint16_t*>(p + nblocks * 4u);
        uint8_t* aidx = p + nblocks * 6u;
        uint8_t* cidx = p + nblocks * 12u;
        uint8_t pa0 = 0u, pa1 = 0u;
        uint16_t pc0 = 0u, pc1 = 0u;
        for (uint32_t i = 0; i < nblocks; ++i) {
            const uint8_t* b = blocks + static_cast<size_t>(i) * 16u;
            a0[i] = static_cast<uint8_t>(b[0] - pa0);
            a1[i] = static_cast<uint8_t>(b[1] - pa1);
            pa0 = b[0]; pa1 = b[1];
            uint16_t c0 = 0, c1 = 0;
            std::memcpy(&c0, b + 8, 2);
            std::memcpy(&c1, b + 10, 2);
            c0p[i] = static_cast<uint16_t>(c0 - pc0);
            c1p[i] = static_cast<uint16_t>(c1 - pc1);
            pc0 = c0; pc1 = c1;
            std::memcpy(aidx + static_cast<size_t>(i) * 6u, b + 2, 6);
            std::memcpy(cidx + static_cast<size_t>(i) * 4u, b + 12, 4);
        }
    } else if (kind == BcKind::BC5) {
        // Two BC4 channels (R endpoints, G endpoints, R indices, G indices)
        uint8_t* r0 = p;
        uint8_t* r1 = p + nblocks;
        uint8_t* g0 = p + nblocks * 2u;
        uint8_t* g1 = p + nblocks * 3u;
        uint8_t* ridx = p + nblocks * 4u;
        uint8_t* gidx = p + nblocks * 10u;
        uint8_t pr0 = 0u, pr1 = 0u, pg0 = 0u, pg1 = 0u;
        for (uint32_t i = 0; i < nblocks; ++i) {
            const uint8_t* b = blocks + static_cast<size_t>(i) * 16u;
            r0[i] = static_cast<uint8_t>(b[0] - pr0); pr0 = b[0];
            r1[i] = static_cast<uint8_t>(b[1] - pr1); pr1 = b[1];
            g0[i] = static_cast<uint8_t>(b[8] - pg0); pg0 = b[8];
            g1[i] = static_cast<uint8_t>(b[9] - pg1); pg1 = b[9];
            std::memcpy(ridx + static_cast<size_t>(i) * 6u, b + 2, 6);
            std::memcpy(gidx + static_cast<size_t>(i) * 6u, b + 10, 6);
        }
    } else {
        // BC7: 16 byte-transposed planes (plane 0 = mode+partition bytes, planes 1..15 = endpoints/indices)
        // with delta-coding on planes 0..7 (mode/partition/endpoints)
        for (uint32_t lane = 0; lane < 16u; ++lane) {
            uint8_t* dst_plane = p + static_cast<size_t>(lane) * nblocks;
            uint8_t prev = 0u;
            for (uint32_t i = 0; i < nblocks; ++i) {
                const uint8_t v = blocks[static_cast<size_t>(i) * 16u + lane];
                if (lane < 8u) {
                    dst_plane[i] = static_cast<uint8_t>(v - prev);
                    prev = v;
                } else {
                    dst_plane[i] = v;
                }
            }
        }
    }
}

inline void merge_planes(const uint8_t* planar, uint32_t nblocks, BcKind kind, uint8_t* blocks) noexcept {
    if (kind == BcKind::BC1) {
        const auto* ep0 = reinterpret_cast<const uint16_t*>(planar);
        const auto* ep1 = reinterpret_cast<const uint16_t*>(planar + nblocks * 2u);
        const uint8_t* idx = planar + nblocks * 4u;
        uint16_t prev0 = 0u, prev1 = 0u;
        for (uint32_t i = 0; i < nblocks; ++i) {
            uint8_t* b = blocks + static_cast<size_t>(i) * 8u;
            prev0 = static_cast<uint16_t>(prev0 + ep0[i]);
            prev1 = static_cast<uint16_t>(prev1 + ep1[i]);
            std::memcpy(b + 0, &prev0, 2);
            std::memcpy(b + 2, &prev1, 2);
            std::memcpy(b + 4, idx + static_cast<size_t>(i) * 4u, 4);
        }
    } else if (kind == BcKind::BC4) {
        const uint8_t* ep0 = planar;
        const uint8_t* ep1 = planar + nblocks;
        const uint8_t* idx = planar + nblocks * 2u;
        uint8_t prev0 = 0u, prev1 = 0u;
        for (uint32_t i = 0; i < nblocks; ++i) {
            uint8_t* b = blocks + static_cast<size_t>(i) * 8u;
            prev0 = static_cast<uint8_t>(prev0 + ep0[i]);
            prev1 = static_cast<uint8_t>(prev1 + ep1[i]);
            b[0] = prev0;
            b[1] = prev1;
            std::memcpy(b + 2, idx + static_cast<size_t>(i) * 6u, 6);
        }
    } else if (kind == BcKind::BC3) {
        const uint8_t* a0 = planar;
        const uint8_t* a1 = planar + nblocks;
        const auto* c0p = reinterpret_cast<const uint16_t*>(planar + nblocks * 2u);
        const auto* c1p = reinterpret_cast<const uint16_t*>(planar + nblocks * 4u);
        const uint8_t* aidx = planar + nblocks * 6u;
        const uint8_t* cidx = planar + nblocks * 12u;
        uint8_t pa0 = 0u, pa1 = 0u;
        uint16_t pc0 = 0u, pc1 = 0u;
        for (uint32_t i = 0; i < nblocks; ++i) {
            uint8_t* b = blocks + static_cast<size_t>(i) * 16u;
            pa0 = static_cast<uint8_t>(pa0 + a0[i]);
            pa1 = static_cast<uint8_t>(pa1 + a1[i]);
            pc0 = static_cast<uint16_t>(pc0 + c0p[i]);
            pc1 = static_cast<uint16_t>(pc1 + c1p[i]);
            b[0] = pa0; b[1] = pa1;
            std::memcpy(b + 2, aidx + static_cast<size_t>(i) * 6u, 6);
            std::memcpy(b + 8, &pc0, 2);
            std::memcpy(b + 10, &pc1, 2);
            std::memcpy(b + 12, cidx + static_cast<size_t>(i) * 4u, 4);
        }
    } else if (kind == BcKind::BC5) {
        const uint8_t* r0 = planar;
        const uint8_t* r1 = planar + nblocks;
        const uint8_t* g0 = planar + nblocks * 2u;
        const uint8_t* g1 = planar + nblocks * 3u;
        const uint8_t* ridx = planar + nblocks * 4u;
        const uint8_t* gidx = planar + nblocks * 10u;
        uint8_t pr0 = 0u, pr1 = 0u, pg0 = 0u, pg1 = 0u;
        for (uint32_t i = 0; i < nblocks; ++i) {
            uint8_t* b = blocks + static_cast<size_t>(i) * 16u;
            pr0 = static_cast<uint8_t>(pr0 + r0[i]);
            pr1 = static_cast<uint8_t>(pr1 + r1[i]);
            pg0 = static_cast<uint8_t>(pg0 + g0[i]);
            pg1 = static_cast<uint8_t>(pg1 + g1[i]);
            b[0] = pr0; b[1] = pr1;
            std::memcpy(b + 2, ridx + static_cast<size_t>(i) * 6u, 6);
            b[8] = pg0; b[9] = pg1;
            std::memcpy(b + 10, gidx + static_cast<size_t>(i) * 6u, 6);
        }
    } else {
        for (uint32_t lane = 0; lane < 16u; ++lane) {
            const uint8_t* src_plane = planar + static_cast<size_t>(lane) * nblocks;
            uint8_t prev = 0u;
            for (uint32_t i = 0; i < nblocks; ++i) {
                uint8_t v = src_plane[i];
                if (lane < 8u) {
                    prev = static_cast<uint8_t>(prev + v);
                    v = prev;
                }
                blocks[static_cast<size_t>(i) * 16u + lane] = v;
            }
        }
    }
}

} // namespace detail

// Compress raw BC1/BC3/BC4/BC5/BC7 blocks into clean-room BCPack ("BCPK") stream
inline bool compress(
    const uint8_t* src,
    size_t src_size,
    DXGI_FORMAT bc_format,
    std::vector<uint8_t>& out_packed) {
    out_packed.clear();
    const BcKind kind = classify_dxgi_bc_format(bc_format);
    const uint32_t bsz = block_bytes_for(kind);
    if (!src || src_size == 0u || bsz == 0u || (src_size % bsz) != 0u) return false;

    const uint32_t nblocks = static_cast<uint32_t>(src_size / bsz);
    std::vector<uint8_t> planar;
    detail::split_planes(src, nblocks, kind, planar);

    std::vector<uint8_t> compressed;
    detail::lz_rle_compress(planar.data(), planar.size(), compressed);

    const bool use_lz = compressed.size() < planar.size();
    const std::vector<uint8_t>& chosen = use_lz ? compressed : planar;

    BcPackHeader hdr{};
    hdr.magic = kBcPackMagic;
    hdr.version = kBcPackVersion;
    hdr.bc_kind = static_cast<uint8_t>(kind);
    hdr.flags = static_cast<uint8_t>(1u | (use_lz ? 2u : 0u));
    hdr.block_count = nblocks;
    hdr.uncompressed_size = static_cast<uint32_t>(src_size);
    hdr.payload_size = static_cast<uint32_t>(chosen.size());
    hdr.crc32_uncomp = detail::crc32_ieee(src, src_size);

    out_packed.resize(sizeof(BcPackHeader) + chosen.size());
    std::memcpy(out_packed.data(), &hdr, sizeof(BcPackHeader));
    if (!chosen.empty()) {
        std::memcpy(out_packed.data() + sizeof(BcPackHeader), chosen.data(), chosen.size());
    }
    return true;
}

// Decompress BCPack ("BCPK"), GDeflate-wrapped BCn, or zlib-wrapped BCn stream
inline bool decompress(
    const uint8_t* src,
    size_t src_size,
    DXGI_FORMAT expected_bc_format,
    std::vector<uint8_t>& out_blocks,
    size_t max_output_bytes = 256u * 1024u * 1024u) {
    out_blocks.clear();
    if (!src || src_size < 4u) return false;

    uint32_t magic = 0u;
    std::memcpy(&magic, src, 4u);

    // 1. Clean-room BCPK planar stream
    if (magic == kBcPackMagic) {
        if (src_size < sizeof(BcPackHeader)) return false;
        BcPackHeader hdr{};
        std::memcpy(&hdr, src, sizeof(BcPackHeader));
        if (hdr.version != kBcPackVersion) return false;
        const BcKind kind = static_cast<BcKind>(hdr.bc_kind);
        const uint32_t bsz = block_bytes_for(kind);
        if (bsz == 0u || hdr.block_count == 0u) return false;
        const uint64_t expected_sz = static_cast<uint64_t>(hdr.block_count) * bsz;
        if (expected_sz != hdr.uncompressed_size || expected_sz > max_output_bytes) return false;
        if (sizeof(BcPackHeader) + hdr.payload_size > src_size) return false;
        if (expected_bc_format != DXGI_FORMAT_UNKNOWN) {
            const BcKind exp_k = classify_dxgi_bc_format(expected_bc_format);
            if (exp_k != BcKind::Unknown && exp_k != kind) return false;
        }

        const uint8_t* payload = src + sizeof(BcPackHeader);
        std::vector<uint8_t> planar(static_cast<size_t>(expected_sz));
        if ((hdr.flags & 2u) != 0u) {
            if (!detail::lz_rle_decompress(payload, hdr.payload_size, planar.data(), planar.size())) {
                return false;
            }
        } else {
            if (hdr.payload_size != planar.size()) return false;
            std::memcpy(planar.data(), payload, planar.size());
        }

        out_blocks.resize(static_cast<size_t>(expected_sz));
        detail::merge_planes(planar.data(), hdr.block_count, kind, out_blocks.data());
        if (detail::crc32_ieee(out_blocks.data(), out_blocks.size()) != hdr.crc32_uncomp) {
            out_blocks.clear();
            return false;
        }
        return true;
    }

    // 2. GDeflate-wrapped BCn stream (magic 0x04 0xFB)
    if (src[0] == 0x04u && src[1] == 0xFBu) {
        return wsx_gdeflate::decompress(src, src_size, out_blocks, max_output_bytes);
    }

    // 3. Zlib-wrapped BCn stream
    if (src[0] == 0x78u && (src[1] == 0x01u || src[1] == 0x5Eu || src[1] == 0x9Cu || src[1] == 0xDAu)) {
        return wsx_inflate::inflate_zlib(src, src_size, out_blocks, max_output_bytes);
    }

    return false;
}

// Resident mip-tail fallback: reconstructs target BC1/BC3/BC4/BC5/BC7 mip level by block-upsampling
// a resident lower-resolution mip (or synthesizing valid neutral-gray BCn blocks if no mip-tail exists).
inline bool synthesize_resident_mip_fallback(
    DXGI_FORMAT bc_format,
    uint32_t target_w,
    uint32_t target_h,
    const uint8_t* resident_mip,
    uint32_t resident_w,
    uint32_t resident_h,
    std::vector<uint8_t>& out_blocks) {
    out_blocks.clear();
    const BcKind kind = classify_dxgi_bc_format(bc_format);
    const uint32_t bsz = block_bytes_for(kind);
    if (bsz == 0u || target_w == 0u || target_h == 0u) return false;

    const uint32_t dst_bw = std::max<uint32_t>(1u, (target_w + 3u) / 4u);
    const uint32_t dst_bh = std::max<uint32_t>(1u, (target_h + 3u) / 4u);
    out_blocks.resize(static_cast<size_t>(dst_bw) * dst_bh * bsz);

    if (resident_mip && resident_w > 0u && resident_h > 0u) {
        const uint32_t src_bw = std::max<uint32_t>(1u, (resident_w + 3u) / 4u);
        const uint32_t src_bh = std::max<uint32_t>(1u, (resident_h + 3u) / 4u);
        for (uint32_t by = 0; by < dst_bh; ++by) {
            const uint32_t sy = std::min<uint32_t>(src_bh - 1u, (by * src_bh) / dst_bh);
            for (uint32_t bx = 0; bx < dst_bw; ++bx) {
                const uint32_t sx = std::min<uint32_t>(src_bw - 1u, (bx * src_bw) / dst_bw);
                std::memcpy(
                    out_blocks.data() + (static_cast<size_t>(by) * dst_bw + bx) * bsz,
                    resident_mip + (static_cast<size_t>(sy) * src_bw + sx) * bsz,
                    bsz);
            }
        }
        return true;
    }

    // Valid neutral mid-gray BCn block fallback (zero artifacts, hardware-valid)
    uint8_t neutral[16]{};
    if (kind == BcKind::BC1) {
        // RGB565 mid-gray 0x8410, indices = 0
        neutral[0] = 0x10u; neutral[1] = 0x84u;
        neutral[2] = 0x10u; neutral[3] = 0x84u;
    } else if (kind == BcKind::BC3) {
        neutral[0] = 0xFFu; neutral[1] = 0xFFu; // opaque alpha
        neutral[8] = 0x10u; neutral[9] = 0x84u;
        neutral[10] = 0x10u; neutral[11] = 0x84u;
    } else if (kind == BcKind::BC4) {
        neutral[0] = 0x80u; neutral[1] = 0x80u;
    } else if (kind == BcKind::BC5) {
        neutral[0] = 0x80u; neutral[1] = 0x80u;
        neutral[8] = 0x80u; neutral[9] = 0x80u;
    } else if (kind == BcKind::BC7) {
        // BC7 Mode 6 (bit 6 set = 0x40): 1 subset, 7-bit RGBA endpoints all 0x40 (mid-gray, opaque)
        neutral[0] = 0x40u;
        neutral[1] = 0x80u; neutral[2] = 0x40u; neutral[3] = 0x80u;
        neutral[4] = 0x40u; neutral[5] = 0x80u; neutral[6] = 0xFEu; neutral[7] = 0x7Fu;
    }
    for (size_t i = 0; i < static_cast<size_t>(dst_bw) * dst_bh; ++i) {
        std::memcpy(out_blocks.data() + i * bsz, neutral, bsz);
    }
    return true;
}

} // namespace wsx_bcpack
