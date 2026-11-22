// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Microsoft GDeflate v1.0 (32-stream interleaved tiled DEFLATE)
// CPU decompressor and test compressor for dstorage_xs.dll.
//
// Format summary:
//   - 8-byte Tile Stream Header:
//       [0]     id              = 0x04
//       [1]     magic           = 0xFB (~id)
//       [2..3]  num_tiles       (uint16_t LE, >= 1)
//       [4..7]  flags           (uint32_t LE):
//                 bits [1:0]    tile_size_idx (1 => 64 KiB tiles)
//                 bits [19:2]   last_tile_size (0 => full tile size, else 1..65535)
//                 bits [31:20]  reserved (0)
//   - Tile Offset Table: num_tiles * uint32_t LE immediately after header:
//       offsets[0] = compressed byte size of the LAST tile (tile num_tiles - 1)
//       offsets[i] = byte offset of tile i (1 <= i < num_tiles) from start of tile payload
//       (tile 0 always starts at offset 0 of the tile payload)
//   - Each tile starts with an RFC 1951 block header (BFINAL:1, BTYPE:2):
//       BTYPE=0: stored uncompressed tile (byte-aligned LEN/NLEN + raw bytes)
//       BTYPE=1: fixed Huffman, 32-lane DWORD-interleaved bitstream
//       BTYPE=2: dynamic Huffman (standard RFC 1951 tree header), 32-lane DWORD-interleaved bitstream

#pragma once

#include "wsx_inflate.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <queue>
#include <vector>

namespace wsx_gdeflate {

constexpr uint8_t kStreamId = 0x04u;
constexpr uint8_t kStreamMagic = 0xFBu; // ~kStreamId
constexpr uint32_t kDefaultTileSize = 65536u;
constexpr uint32_t kNumStreams = 32u;

enum class TileMode : uint8_t {
    Auto = 0,          // Pick best of Dynamic / Fixed / Stored per tile
    ForceStored = 1,   // BTYPE = 0
    ForceFixed = 2,    // BTYPE = 1 (32-stream interleaved)
    ForceDynamic = 3   // BTYPE = 2 (32-stream interleaved)
};

namespace detail {

inline uint16_t read_u16_le(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t read_u32_le(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline void write_u16_le(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

inline void write_u32_le(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

static constexpr uint16_t kLenBase[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static constexpr uint8_t kLenExtra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};
static constexpr uint16_t kDistBase[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static constexpr uint8_t kDistExtra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13
};
static constexpr uint8_t kCodeOrder[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

inline void build_fixed_lengths(uint8_t ll[288], uint8_t dl[32]) noexcept {
    for (int i = 0; i <= 143; ++i) ll[i] = 8;
    for (int i = 144; i <= 255; ++i) ll[i] = 9;
    for (int i = 256; i <= 279; ++i) ll[i] = 7;
    for (int i = 280; i <= 287; ++i) ll[i] = 8;
    for (int i = 0; i < 32; ++i) dl[i] = 5;
}

struct LaneBitReader {
    uint64_t bit_buf = 0;
    uint32_t bit_count = 0;
    bool finished = false;

    void refill_if_needed(const uint8_t* tile, size_t tile_size, size_t& word_byte_pos) noexcept {
        if (bit_count <= 32u) {
            uint32_t w = 0;
            if (word_byte_pos + 4u <= tile_size) {
                w = read_u32_le(tile + word_byte_pos);
                word_byte_pos += 4u;
            } else if (word_byte_pos < tile_size) {
                const size_t rem = tile_size - word_byte_pos;
                for (size_t i = 0; i < rem; ++i) {
                    w |= static_cast<uint32_t>(tile[word_byte_pos + i]) << (i * 8u);
                }
                word_byte_pos = tile_size;
            }
            bit_buf |= static_cast<uint64_t>(w) << bit_count;
            bit_count += 32u;
        }
    }

    bool read_bits(uint32_t n, uint32_t& out) noexcept {
        if (n == 0u) {
            out = 0u;
            return true;
        }
        if (bit_count < n) return false;
        out = static_cast<uint32_t>(bit_buf & ((1ULL << n) - 1ULL));
        bit_buf >>= n;
        bit_count -= n;
        return true;
    }

    bool decode_huff(const wsx_inflate::detail::HuffTable& tab, uint16_t& out_sym) noexcept {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= wsx_inflate::detail::HuffTable::kMaxBits; ++len) {
            uint32_t bit = 0;
            if (!read_bits(1u, bit)) return false;
            code = (code << 1) | static_cast<int>(bit);
            const int c = tab.count[len];
            if (code - c < first) {
                const size_t idx = static_cast<size_t>(index + (code - first));
                if (idx >= tab.symbols.size()) return false;
                out_sym = tab.symbols[idx];
                return true;
            }
            index += c;
            first = (first + c) << 1;
        }
        return false;
    }
};

inline bool decode_tile(
    const uint8_t* tile,
    size_t tile_size,
    uint8_t* dst,
    size_t expected_out_size) {
    if (!tile || tile_size == 0u) return false;

    wsx_inflate::detail::BitReader hdr_br{tile, tile_size, 0, 0, 0};
    uint32_t bfinal = 0;
    uint32_t btype = 0;
    if (!hdr_br.read_bits(1u, bfinal) || !hdr_br.read_bits(2u, btype)) {
        return false;
    }

    if (btype == 0u) {
        // Uncompressed stored tile
        uint8_t lh[4]{};
        if (!hdr_br.read_bytes_aligned(lh, 4u)) return false;
        const uint16_t len = read_u16_le(lh);
        const uint16_t nlen = read_u16_le(lh + 2);
        if (static_cast<uint16_t>(~len) != nlen) return false;
        // Note: a full 65536-byte stored tile is split across stored blocks or uses raw copy if len == expected.
        size_t written = 0;
        if (len > expected_out_size) return false;
        if (len > 0u) {
            if (!hdr_br.read_bytes_aligned(dst, len)) return false;
            written += len;
        }
        while (bfinal == 0u && written < expected_out_size) {
            if (!hdr_br.read_bits(1u, bfinal) || !hdr_br.read_bits(2u, btype)) return false;
            if (btype != 0u) return false;
            if (!hdr_br.read_bytes_aligned(lh, 4u)) return false;
            const uint16_t next_len = read_u16_le(lh);
            const uint16_t next_nlen = read_u16_le(lh + 2);
            if (static_cast<uint16_t>(~next_len) != next_nlen) return false;
            if (written + next_len > expected_out_size) return false;
            if (next_len > 0u) {
                if (!hdr_br.read_bytes_aligned(dst + written, next_len)) return false;
                written += next_len;
            }
        }
        return written == expected_out_size;
    }

    wsx_inflate::detail::HuffTable litlen_tab;
    wsx_inflate::detail::HuffTable dist_tab;

    if (btype == 1u) {
        uint8_t ll[288]{};
        uint8_t dl[32]{};
        build_fixed_lengths(ll, dl);
        if (!litlen_tab.build(ll, 288u) || !dist_tab.build(dl, 32u)) return false;
    } else if (btype == 2u) {
        uint32_t hlit = 0, hdist = 0, hclen = 0;
        if (!hdr_br.read_bits(5u, hlit) || !hdr_br.read_bits(5u, hdist) || !hdr_br.read_bits(4u, hclen)) {
            return false;
        }
        hlit += 257u;
        hdist += 1u;
        hclen += 4u;
        if (hlit > 286u || hdist > 30u) return false;

        uint8_t cl_lens[19]{};
        for (uint32_t i = 0; i < hclen; ++i) {
            uint32_t v = 0;
            if (!hdr_br.read_bits(3u, v)) return false;
            cl_lens[kCodeOrder[i]] = static_cast<uint8_t>(v);
        }
        wsx_inflate::detail::HuffTable cl_tab;
        if (!cl_tab.build(cl_lens, 19u)) return false;

        std::vector<uint8_t> lengths(hlit + hdist, 0u);
        size_t idx = 0;
        while (idx < lengths.size()) {
            uint16_t sym = 0;
            if (!cl_tab.decode(hdr_br, sym)) return false;
            if (sym < 16u) {
                lengths[idx++] = static_cast<uint8_t>(sym);
            } else if (sym == 16u) {
                if (idx == 0u) return false;
                uint32_t rep = 0;
                if (!hdr_br.read_bits(2u, rep)) return false;
                rep += 3u;
                const uint8_t prev = lengths[idx - 1u];
                if (idx + rep > lengths.size()) return false;
                while (rep--) lengths[idx++] = prev;
            } else if (sym == 17u) {
                uint32_t rep = 0;
                if (!hdr_br.read_bits(3u, rep)) return false;
                rep += 3u;
                if (idx + rep > lengths.size()) return false;
                while (rep--) lengths[idx++] = 0u;
            } else if (sym == 18u) {
                uint32_t rep = 0;
                if (!hdr_br.read_bits(7u, rep)) return false;
                rep += 11u;
                if (idx + rep > lengths.size()) return false;
                while (rep--) lengths[idx++] = 0u;
            } else {
                return false;
            }
        }
        if (!litlen_tab.build(lengths.data(), hlit) ||
            !dist_tab.build(lengths.data() + hlit, hdist)) {
            return false;
        }
    } else {
        return false;
    }

    // Align header end to next 32-bit DWORD boundary before 32-stream interleaved payload
    const size_t bits_consumed = hdr_br.byte_pos * 8u - hdr_br.bit_count;
    size_t word_byte_pos = ((bits_consumed + 31u) / 32u) * 4u;
    if (word_byte_pos > tile_size) return false;

    std::array<LaneBitReader, kNumStreams> lanes{};
    // Initial 64-bit prime for all 32 lanes (2 uint32 words per lane)
    for (uint32_t k = 0; k < kNumStreams; ++k) {
        lanes[k].refill_if_needed(tile, tile_size, word_byte_pos);
        lanes[k].refill_if_needed(tile, tile_size, word_byte_pos);
    }

    size_t out_pos = 0;
    uint32_t active_lanes = kNumStreams;
    while (active_lanes > 0u) {
        for (uint32_t k = 0; k < kNumStreams; ++k) {
            auto& lane = lanes[k];
            if (lane.finished) continue;

            uint16_t sym = 0;
            if (!lane.decode_huff(litlen_tab, sym)) return false;

            if (sym < 256u) {
                if (out_pos >= expected_out_size) return false;
                dst[out_pos++] = static_cast<uint8_t>(sym);
            } else if (sym == 256u) {
                lane.finished = true;
                --active_lanes;
            } else {
                const uint32_t li = static_cast<uint32_t>(sym - 257u);
                if (li >= 29u) return false;
                uint32_t lextra = 0;
                if (!lane.read_bits(kLenExtra[li], lextra)) return false;
                const size_t length = static_cast<size_t>(kLenBase[li] + lextra);
                lane.refill_if_needed(tile, tile_size, word_byte_pos);

                uint16_t dsym = 0;
                if (!lane.decode_huff(dist_tab, dsym) || dsym >= 30u) return false;
                uint32_t dextra = 0;
                if (!lane.read_bits(kDistExtra[dsym], dextra)) return false;
                const size_t distance = static_cast<size_t>(kDistBase[dsym] + dextra);
                if (distance == 0u || distance > out_pos || out_pos + length > expected_out_size) {
                    return false;
                }
                for (size_t i = 0; i < length; ++i) {
                    dst[out_pos] = dst[out_pos - distance];
                    ++out_pos;
                }
            }

            if (!lane.finished) {
                lane.refill_if_needed(tile, tile_size, word_byte_pos);
            }
        }
    }

    return out_pos == expected_out_size;
}

// ============================================================================
// Clean-Room GDeflate v1.0 Compressor (Stored + 32-Stream Fixed & Dynamic)
// ============================================================================

struct BitWriter {
    std::vector<uint8_t> bytes;
    uint64_t bit_buf = 0;
    uint32_t bit_count = 0;

    void write_bits(uint32_t val, uint32_t n) {
        if (n == 0u) return;
        bit_buf |= (static_cast<uint64_t>(val) & ((1ULL << n) - 1ULL)) << bit_count;
        bit_count += n;
        while (bit_count >= 8u) {
            bytes.push_back(static_cast<uint8_t>(bit_buf & 0xFFu));
            bit_buf >>= 8u;
            bit_count -= 8u;
        }
    }

    void align_byte() {
        if (bit_count > 0u) {
            bytes.push_back(static_cast<uint8_t>(bit_buf & 0xFFu));
            bit_buf = 0;
            bit_count = 0;
        }
    }

    void align_u32() {
        align_byte();
        while ((bytes.size() & 3u) != 0u) {
            bytes.push_back(0u);
        }
    }
};

struct WordBitWriter {
    std::vector<uint32_t> words;
    uint64_t bit_buf = 0;
    uint32_t bit_count = 0;

    void write_bits(uint32_t val, uint32_t n) {
        if (n == 0u) return;
        bit_buf |= (static_cast<uint64_t>(val) & ((1ULL << n) - 1ULL)) << bit_count;
        bit_count += n;
        while (bit_count >= 32u) {
            words.push_back(static_cast<uint32_t>(bit_buf & 0xFFFFFFFFULL));
            bit_buf >>= 32u;
            bit_count -= 32u;
        }
    }

    void finish() {
        if (bit_count > 0u) {
            words.push_back(static_cast<uint32_t>(bit_buf & 0xFFFFFFFFULL));
            bit_buf = 0;
            bit_count = 0;
        }
        while (words.size() < 2u) {
            words.push_back(0u);
        }
    }
};

struct HuffEncTable {
    std::vector<uint32_t> rev_code; // bit-reversed canonical code ready for LSB-first writing
    std::vector<uint8_t> len;

    static uint32_t bit_reverse(uint32_t code, uint8_t bits) noexcept {
        uint32_t r = 0;
        for (uint8_t i = 0; i < bits; ++i) {
            r = (r << 1) | (code & 1u);
            code >>= 1;
        }
        return r;
    }

    void build(const uint8_t* lengths, size_t num_syms) {
        rev_code.assign(num_syms, 0u);
        len.assign(lengths, lengths + num_syms);
        std::array<uint32_t, 16> bl_count{};
        for (size_t i = 0; i < num_syms; ++i) {
            if (lengths[i] > 0u && lengths[i] <= 15u) {
                ++bl_count[lengths[i]];
            }
        }
        std::array<uint32_t, 16> next_code{};
        uint32_t code = 0;
        for (int bits = 1; bits <= 15; ++bits) {
            code = (code + bl_count[bits - 1]) << 1;
            next_code[bits] = code;
        }
        for (size_t sym = 0; sym < num_syms; ++sym) {
            const uint8_t l = lengths[sym];
            if (l > 0u) {
                rev_code[sym] = bit_reverse(next_code[l]++, l);
            }
        }
    }
};

// Build Kraft-valid canonical Huffman code lengths capped at max_bits (<= 15)
inline void build_code_lengths(const uint32_t* freqs, size_t num_syms, uint8_t max_bits, std::vector<uint8_t>& out_lens) {
    out_lens.assign(num_syms, 0u);
    std::vector<size_t> active;
    active.reserve(num_syms);
    for (size_t i = 0; i < num_syms; ++i) {
        if (freqs[i] > 0u) active.push_back(i);
    }
    if (active.empty()) {
        out_lens[0] = 1u;
        return;
    }
    if (active.size() == 1u) {
        out_lens[active[0]] = 1u;
        return;
    }

    struct Node {
        uint64_t weight = 0;
        int left = -1;
        int right = -1;
        int sym = -1;
    };
    std::vector<Node> nodes;
    nodes.reserve(active.size() * 2u);
    using PQItem = std::pair<uint64_t, int>;
    std::priority_queue<PQItem, std::vector<PQItem>, std::greater<PQItem>> pq;
    for (size_t s : active) {
        const int id = static_cast<int>(nodes.size());
        nodes.push_back(Node{freqs[s], -1, -1, static_cast<int>(s)});
        pq.push({freqs[s], id});
    }
    while (pq.size() > 1u) {
        const auto a = pq.top(); pq.pop();
        const auto b = pq.top(); pq.pop();
        const int id = static_cast<int>(nodes.size());
        nodes.push_back(Node{a.first + b.first, a.second, b.second, -1});
        pq.push({a.first + b.first, id});
    }

    const int root = pq.top().second;
    std::vector<std::pair<int, uint8_t>> stack;
    stack.push_back({root, static_cast<uint8_t>(0u)});
    bool overflow = false;
    while (!stack.empty()) {
        const auto [nid, depth] = stack.back();
        stack.pop_back();
        if (nodes[nid].sym >= 0) {
            const uint8_t d = (depth == 0u) ? 1u : depth;
            if (d > max_bits) {
                overflow = true;
                out_lens[static_cast<size_t>(nodes[nid].sym)] = max_bits;
            } else {
                out_lens[static_cast<size_t>(nodes[nid].sym)] = d;
            }
        } else {
            stack.push_back({nodes[nid].left, static_cast<uint8_t>(depth + 1u)});
            stack.push_back({nodes[nid].right, static_cast<uint8_t>(depth + 1u)});
        }
    }

    if (overflow) {
        // Rebalance Kraft sum so sum(2^(max_bits - len)) <= 2^max_bits
        const uint32_t limit = 1u << max_bits;
        auto kraft_sum = [&]() -> uint32_t {
            uint32_t s = 0;
            for (size_t sym : active) s += (1u << (max_bits - out_lens[sym]));
            return s;
        };
        std::sort(active.begin(), active.end(), [&](size_t a, size_t b) {
            return freqs[a] < freqs[b]; // lowest frequency first
        });
        while (kraft_sum() > limit) {
            for (size_t sym : active) {
                if (out_lens[sym] < max_bits) {
                    ++out_lens[sym];
                    if (kraft_sum() <= limit) break;
                }
            }
        }
        // Tighten spare Kraft budget on highest-frequency symbols
        for (auto it = active.rbegin(); it != active.rend(); ++it) {
            const size_t sym = *it;
            while (out_lens[sym] > 1u) {
                const uint32_t delta = 1u << (max_bits - out_lens[sym]);
                if (kraft_sum() + delta <= limit) {
                    --out_lens[sym];
                } else {
                    break;
                }
            }
        }
    }
}

struct LzToken {
    uint16_t litlen = 0;  // 0..255 literal, 256 EOB, 257..285 match length sym
    uint8_t len_ebits = 0;
    uint16_t len_eval = 0;
    uint8_t dist_sym = 0;
    uint8_t dist_ebits = 0;
    uint16_t dist_eval = 0;
};

inline void encode_length_sym(uint32_t length, uint16_t& sym, uint8_t& ebits, uint16_t& eval) noexcept {
    if (length == 258u) {
        sym = 285u;
        ebits = 0u;
        eval = 0u;
        return;
    }
    for (uint32_t i = 0; i < 28u; ++i) {
        const uint32_t next_base = kLenBase[i + 1u];
        if (length < next_base) {
            sym = static_cast<uint16_t>(257u + i);
            ebits = kLenExtra[i];
            eval = static_cast<uint16_t>(length - kLenBase[i]);
            return;
        }
    }
    sym = 285u;
    ebits = 0u;
    eval = 0u;
}

inline void encode_dist_sym(uint32_t dist, uint8_t& sym, uint8_t& ebits, uint16_t& eval) noexcept {
    for (uint32_t i = 0; i < 29u; ++i) {
        if (dist < kDistBase[i + 1u]) {
            sym = static_cast<uint8_t>(i);
            ebits = kDistExtra[i];
            eval = static_cast<uint16_t>(dist - kDistBase[i]);
            return;
        }
    }
    sym = 29u;
    ebits = kDistExtra[29];
    eval = static_cast<uint16_t>(dist - kDistBase[29]);
}

inline std::vector<LzToken> lz77_tokenize(const uint8_t* data, size_t size) {
    std::vector<LzToken> tokens;
    tokens.reserve(size / 2u + 32u);
    constexpr size_t kHashSize = 1u << 14;
    std::vector<int32_t> head(kHashSize, -1);

    auto hash3 = [](const uint8_t* p) noexcept -> size_t {
        const uint32_t v = (static_cast<uint32_t>(p[0]) << 16) ^
                           (static_cast<uint32_t>(p[1]) << 8) ^
                           static_cast<uint32_t>(p[2]);
        return ((v * 2654435761u) >> 18) & (kHashSize - 1u);
    };

    size_t pos = 0;
    while (pos < size) {
        uint32_t best_len = 0;
        uint32_t best_dist = 0;
        if (pos + 3u <= size) {
            const size_t h = hash3(data + pos);
            const int32_t prev = head[h];
            head[h] = static_cast<int32_t>(pos);
            if (prev >= 0) {
                const size_t dist = pos - static_cast<size_t>(prev);
                if (dist >= 1u && dist <= 32768u) {
                    const size_t max_len = (std::min<size_t>)(258u, size - pos);
                    size_t m = 0;
                    while (m < max_len && data[static_cast<size_t>(prev) + m] == data[pos + m]) {
                        ++m;
                    }
                    if (m >= 3u) {
                        best_len = static_cast<uint32_t>(m);
                        best_dist = static_cast<uint32_t>(dist);
                    }
                }
            }
        }
        if (best_len >= 3u) {
            LzToken tok{};
            encode_length_sym(best_len, tok.litlen, tok.len_ebits, tok.len_eval);
            encode_dist_sym(best_dist, tok.dist_sym, tok.dist_ebits, tok.dist_eval);
            tokens.push_back(tok);
            const size_t end_pos = pos + best_len;
            for (size_t p = pos + 1u; p < end_pos && p + 3u <= size; ++p) {
                head[hash3(data + p)] = static_cast<int32_t>(p);
            }
            pos = end_pos;
        } else {
            LzToken tok{};
            tok.litlen = data[pos++];
            tokens.push_back(tok);
        }
    }
    return tokens;
}

inline std::vector<uint8_t> encode_stored_tile(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    size_t offset = 0;
    if (size == 0u) {
        out.push_back(0x01u); // BFINAL=1, BTYPE=0
        write_u16_le(out, 0u);
        write_u16_le(out, 0xFFFFu);
        return out;
    }
    while (offset < size) {
        const size_t chunk = (std::min<size_t>)(65535u, size - offset);
        const bool is_final = (offset + chunk == size);
        out.push_back(is_final ? 0x01u : 0x00u); // BFINAL + BTYPE=0
        const uint16_t len = static_cast<uint16_t>(chunk);
        write_u16_le(out, len);
        write_u16_le(out, static_cast<uint16_t>(~len));
        out.insert(out.end(), data + offset, data + offset + chunk);
        offset += chunk;
    }
    return out;
}

inline std::vector<uint8_t> encode_huffman_tile(
    const std::vector<LzToken>& tokens,
    bool dynamic_mode) {
    // Distribute tokens round-robin across 32 lanes and append EOB (256) to each lane
    std::array<std::vector<LzToken>, kNumStreams> lane_tokens;
    for (size_t i = 0; i < tokens.size(); ++i) {
        lane_tokens[i & (kNumStreams - 1u)].push_back(tokens[i]);
    }
    LzToken eob{};
    eob.litlen = 256u;
    for (uint32_t k = 0; k < kNumStreams; ++k) {
        lane_tokens[k].push_back(eob);
    }

    std::vector<uint8_t> ll_lens(288u, 0u);
    std::vector<uint8_t> dist_lens(32u, 0u);
    BitWriter hdr;
    hdr.write_bits(1u, 1u); // BFINAL = 1

    if (!dynamic_mode) {
        hdr.write_bits(1u, 2u); // BTYPE = 1 (Fixed Huffman)
        build_fixed_lengths(ll_lens.data(), dist_lens.data());
    } else {
        hdr.write_bits(2u, 2u); // BTYPE = 2 (Dynamic Huffman)
        std::array<uint32_t, 286> ll_freq{};
        std::array<uint32_t, 30> dist_freq{};
        for (uint32_t k = 0; k < kNumStreams; ++k) {
            for (const auto& t : lane_tokens[k]) {
                ++ll_freq[t.litlen];
                if (t.litlen > 256u) {
                    ++dist_freq[t.dist_sym];
                }
            }
        }
        build_code_lengths(ll_freq.data(), 286u, 15u, ll_lens);
        build_code_lengths(dist_freq.data(), 30u, 15u, dist_lens);
        ll_lens.resize(286u);
        dist_lens.resize(30u);

        size_t hlit = 286u;
        while (hlit > 257u && ll_lens[hlit - 1u] == 0u) --hlit;
        size_t hdist = 30u;
        while (hdist > 1u && dist_lens[hdist - 1u] == 0u) --hdist;

        std::vector<uint8_t> combined;
        combined.reserve(hlit + hdist);
        combined.insert(combined.end(), ll_lens.begin(), ll_lens.begin() + static_cast<std::ptrdiff_t>(hlit));
        combined.insert(combined.end(), dist_lens.begin(), dist_lens.begin() + static_cast<std::ptrdiff_t>(hdist));

        std::array<uint32_t, 19> cl_freq{};
        for (uint8_t v : combined) ++cl_freq[v];
        std::vector<uint8_t> cl_lens;
        build_code_lengths(cl_freq.data(), 19u, 7u, cl_lens);

        size_t hclen = 19u;
        while (hclen > 4u && cl_lens[kCodeOrder[hclen - 1u]] == 0u) --hclen;

        hdr.write_bits(static_cast<uint32_t>(hlit - 257u), 5u);
        hdr.write_bits(static_cast<uint32_t>(hdist - 1u), 5u);
        hdr.write_bits(static_cast<uint32_t>(hclen - 4u), 4u);
        for (size_t i = 0; i < hclen; ++i) {
            hdr.write_bits(cl_lens[kCodeOrder[i]], 3u);
        }

        HuffEncTable cl_enc;
        cl_enc.build(cl_lens.data(), 19u);
        for (uint8_t v : combined) {
            hdr.write_bits(cl_enc.rev_code[v], cl_enc.len[v]);
        }
    }

    hdr.align_u32();

    HuffEncTable ll_enc, dist_enc;
    ll_enc.build(ll_lens.data(), ll_lens.size());
    dist_enc.build(dist_lens.data(), dist_lens.size());

    // Encode each lane independently and record exact split bit lengths for lockstep interleaving
    struct TokBitCost {
        uint8_t part1_bits = 0; // litlen + len_extra (<= 20)
        uint8_t part2_bits = 0; // dist + dist_extra (<= 28, or 0 for literal/EOB)
    };
    std::array<WordBitWriter, kNumStreams> writers;
    std::array<std::vector<TokBitCost>, kNumStreams> tok_bits;
    for (uint32_t k = 0; k < kNumStreams; ++k) {
        tok_bits[k].reserve(lane_tokens[k].size());
        for (const auto& t : lane_tokens[k]) {
            TokBitCost cost{};
            writers[k].write_bits(ll_enc.rev_code[t.litlen], ll_enc.len[t.litlen]);
            cost.part1_bits = ll_enc.len[t.litlen];
            if (t.litlen > 256u) {
                writers[k].write_bits(t.len_eval, t.len_ebits);
                cost.part1_bits = static_cast<uint8_t>(cost.part1_bits + t.len_ebits);
                writers[k].write_bits(dist_enc.rev_code[t.dist_sym], dist_enc.len[t.dist_sym]);
                writers[k].write_bits(t.dist_eval, t.dist_ebits);
                cost.part2_bits = static_cast<uint8_t>(dist_enc.len[t.dist_sym] + t.dist_ebits);
            }
            tok_bits[k].push_back(cost);
        }
        writers[k].finish();
    }

    // Interleave 32-bit words in exact decoder refill order
    std::vector<uint8_t> tile = std::move(hdr.bytes);
    std::array<size_t, kNumStreams> word_idx{};
    std::array<uint32_t, kNumStreams> sim_bits{};
    std::array<size_t, kNumStreams> tok_idx{};
    uint32_t active = kNumStreams;

    // Initial 64-bit prime (2 words per lane)
    for (uint32_t k = 0; k < kNumStreams; ++k) {
        write_u32_le(tile, writers[k].words[0]);
        write_u32_le(tile, writers[k].words[1]);
        word_idx[k] = 2u;
        sim_bits[k] = 64u;
    }

    auto emit_refill = [&](uint32_t k) {
        if (sim_bits[k] <= 32u) {
            const uint32_t next_w = (word_idx[k] < writers[k].words.size())
                ? writers[k].words[word_idx[k]++]
                : 0u;
            write_u32_le(tile, next_w);
            sim_bits[k] += 32u;
        }
    };

    while (active > 0u) {
        for (uint32_t k = 0; k < kNumStreams; ++k) {
            if (tok_idx[k] >= tok_bits[k].size()) continue;
            const TokBitCost cost = tok_bits[k][tok_idx[k]++];
            sim_bits[k] -= cost.part1_bits;
            if (cost.part2_bits > 0u) {
                emit_refill(k);
                sim_bits[k] -= cost.part2_bits;
            }
            if (tok_idx[k] >= tok_bits[k].size()) {
                --active;
            } else {
                emit_refill(k);
            }
        }
    }

    return tile;
}

} // namespace detail

inline bool decompress(
    const uint8_t* src,
    size_t src_size,
    std::vector<uint8_t>& out,
    size_t max_out = 256u * 1024u * 1024u) {
    out.clear();
    if (!src || src_size < 12u) return false;
    if (src[0] != kStreamId || src[1] != kStreamMagic) return false;

    const uint16_t num_tiles = detail::read_u16_le(src + 2);
    const uint32_t flags = detail::read_u32_le(src + 4);
    const uint32_t tile_size_idx = flags & 0x3u;
    const uint32_t last_tile_raw = (flags >> 2) & 0x3FFFFu;
    const uint32_t reserved = flags >> 20;
    if (num_tiles == 0u || reserved != 0u || tile_size_idx > 1u) return false;

    const uint32_t tile_size = kDefaultTileSize;
    const uint32_t last_tile_size = (last_tile_raw == 0u) ? tile_size : last_tile_raw;
    if (last_tile_size > tile_size) return false;

    const size_t table_bytes = static_cast<size_t>(num_tiles) * sizeof(uint32_t);
    const size_t payload_base = 8u + table_bytes;
    if (payload_base > src_size) return false;
    const size_t payload_total = src_size - payload_base;

    const uint64_t total_uncompressed =
        static_cast<uint64_t>(num_tiles - 1u) * tile_size + last_tile_size;
    if (total_uncompressed > max_out) return false;

    std::vector<uint32_t> offsets(num_tiles, 0u);
    for (uint16_t t = 0; t < num_tiles; ++t) {
        offsets[t] = detail::read_u32_le(src + 8u + static_cast<size_t>(t) * 4u);
    }

    out.resize(static_cast<size_t>(total_uncompressed));

    for (uint16_t t = 0; t < num_tiles; ++t) {
        const size_t tile_start = (t == 0u) ? 0u : static_cast<size_t>(offsets[t]);
        size_t tile_comp_size = 0;
        if (t + 1u < num_tiles) {
            const size_t next_start = static_cast<size_t>(offsets[t + 1u]);
            if (next_start < tile_start) return false;
            tile_comp_size = next_start - tile_start;
        } else {
            tile_comp_size = static_cast<size_t>(offsets[0]);
        }
        if (tile_start + tile_comp_size > payload_total) return false;

        const size_t expected_len = (t + 1u < num_tiles) ? tile_size : last_tile_size;
        const size_t dst_off = static_cast<size_t>(t) * tile_size;
        if (!detail::decode_tile(
                src + payload_base + tile_start,
                tile_comp_size,
                out.data() + dst_off,
                expected_len)) {
            out.clear();
            return false;
        }
    }

    return true;
}

inline bool compress(
    const uint8_t* src,
    size_t src_size,
    std::vector<uint8_t>& out,
    TileMode mode = TileMode::Auto) {
    out.clear();
    if (!src && src_size > 0u) return false;
    if (src_size == 0u) return false;

    const uint32_t tile_size = kDefaultTileSize;
    const size_t num_tiles_sz = (src_size + tile_size - 1u) / tile_size;
    if (num_tiles_sz == 0u || num_tiles_sz > 65535u) return false;
    const uint16_t num_tiles = static_cast<uint16_t>(num_tiles_sz);
    const uint32_t last_tile_size = static_cast<uint32_t>(src_size - static_cast<size_t>(num_tiles - 1u) * tile_size);

    std::vector<std::vector<uint8_t>> tiles(num_tiles);
    for (uint16_t t = 0; t < num_tiles; ++t) {
        const size_t off = static_cast<size_t>(t) * tile_size;
        const size_t len = (t + 1u < num_tiles) ? tile_size : last_tile_size;

        if (mode == TileMode::ForceStored) {
            tiles[t] = detail::encode_stored_tile(src + off, len);
        } else if (mode == TileMode::ForceFixed) {
            const auto toks = detail::lz77_tokenize(src + off, len);
            tiles[t] = detail::encode_huffman_tile(toks, false);
        } else if (mode == TileMode::ForceDynamic) {
            const auto toks = detail::lz77_tokenize(src + off, len);
            tiles[t] = detail::encode_huffman_tile(toks, true);
        } else {
            const auto toks = detail::lz77_tokenize(src + off, len);
            auto dyn = detail::encode_huffman_tile(toks, true);
            auto fix = detail::encode_huffman_tile(toks, false);
            auto sto = detail::encode_stored_tile(src + off, len);
            auto* best = &dyn;
            if (fix.size() < best->size()) best = &fix;
            if (sto.size() < best->size()) best = &sto;
            tiles[t] = std::move(*best);
        }
    }

    // Emit 8-byte header
    out.push_back(kStreamId);
    out.push_back(kStreamMagic);
    detail::write_u16_le(out, num_tiles);
    const uint32_t last_field = (last_tile_size == tile_size) ? 0u : (last_tile_size & 0x3FFFFu);
    const uint32_t flags = (1u & 0x3u) | (last_field << 2);
    detail::write_u32_le(out, flags);

    // Emit tile offset table: offsets[0] = last tile compressed size; offsets[1..N-1] = payload offsets
    std::vector<uint32_t> offsets(num_tiles, 0u);
    uint32_t cursor = 0;
    for (uint16_t t = 0; t < num_tiles; ++t) {
        if (t > 0u) {
            offsets[t] = cursor;
        }
        cursor += static_cast<uint32_t>(tiles[t].size());
    }
    offsets[0] = static_cast<uint32_t>(tiles[num_tiles - 1u].size());
    for (uint16_t t = 0; t < num_tiles; ++t) {
        detail::write_u32_le(out, offsets[t]);
    }

    // Emit tile payloads
    for (uint16_t t = 0; t < num_tiles; ++t) {
        out.insert(out.end(), tiles[t].begin(), tiles[t].end());
    }
    return true;
}

} // namespace wsx_gdeflate
