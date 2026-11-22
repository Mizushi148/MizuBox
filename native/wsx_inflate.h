// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Zero-dependency clean-room RFC 1951 (raw deflate) & RFC 1950 (zlib) inflater.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wsx_inflate {

namespace detail {

struct BitReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t byte_pos = 0;
    uint64_t bit_buf = 0;
    uint32_t bit_count = 0;

    bool ensure(uint32_t n) noexcept {
        while (bit_count < n) {
            if (byte_pos >= size) return false;
            bit_buf |= static_cast<uint64_t>(data[byte_pos++]) << bit_count;
            bit_count += 8u;
        }
        return true;
    }

    bool read_bits(uint32_t n, uint32_t& out) noexcept {
        if (n == 0) { out = 0; return true; }
        if (!ensure(n)) return false;
        out = static_cast<uint32_t>(bit_buf & ((1ULL << n) - 1ULL));
        bit_buf >>= n;
        bit_count -= n;
        return true;
    }

    void align_byte() noexcept {
        const uint32_t drop = bit_count & 7u;
        bit_buf >>= drop;
        bit_count -= drop;
    }

    bool read_bytes_aligned(uint8_t* dst, size_t n) noexcept {
        align_byte();
        while (n > 0 && bit_count >= 8u) {
            *dst++ = static_cast<uint8_t>(bit_buf & 0xFFu);
            bit_buf >>= 8u;
            bit_count -= 8u;
            --n;
        }
        if (byte_pos + n > size) return false;
        if (n > 0) {
            std::memcpy(dst, data + byte_pos, n);
            byte_pos += n;
        }
        return true;
    }
};

struct HuffTable {
    static constexpr int kMaxBits = 15;
    std::array<int, kMaxBits + 1> count{};
    std::vector<uint16_t> symbols;

    bool build(const uint8_t* lengths, size_t num_syms) {
        count.fill(0);
        for (size_t i = 0; i < num_syms; ++i) {
            if (lengths[i] > kMaxBits) return false;
            ++count[lengths[i]];
        }
        if (count[0] == static_cast<int>(num_syms)) {
            symbols.clear();
            return true;
        }
        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left = (left << 1) - count[len];
            if (left < 0) return false;
        }
        std::array<int, kMaxBits + 1> offs{};
        int total = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            offs[len] = total;
            total += count[len];
        }
        symbols.assign(static_cast<size_t>(total), 0u);
        for (size_t sym = 0; sym < num_syms; ++sym) {
            const uint8_t len = lengths[sym];
            if (len != 0) {
                symbols[static_cast<size_t>(offs[len]++)] = static_cast<uint16_t>(sym);
            }
        }
        return true;
    }

    bool decode(BitReader& br, uint16_t& out_sym) const noexcept {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            uint32_t bit = 0;
            if (!br.read_bits(1, bit)) return false;
            code = (code << 1) | static_cast<int>(bit);
            const int c = count[len];
            if (code - c < first) {
                const size_t idx = static_cast<size_t>(index + (code - first));
                if (idx >= symbols.size()) return false;
                out_sym = symbols[idx];
                return true;
            }
            index += c;
            first = (first + c) << 1;
        }
        return false;
    }
};

inline bool decode_block(BitReader& br, const HuffTable& litlen, const HuffTable& dist,
                         std::vector<uint8_t>& out, size_t max_out) {
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

    for (;;) {
        uint16_t sym = 0;
        if (!litlen.decode(br, sym)) return false;
        if (sym < 256u) {
            if (out.size() >= max_out) return false;
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256u) {
            return true;
        } else {
            const uint32_t li = static_cast<uint32_t>(sym - 257u);
            if (li >= 29u) return false;
            uint32_t extra = 0;
            if (!br.read_bits(kLenExtra[li], extra)) return false;
            const size_t length = static_cast<size_t>(kLenBase[li] + extra);

            uint16_t dsym = 0;
            if (!dist.decode(br, dsym) || dsym >= 30u) return false;
            uint32_t dextra = 0;
            if (!br.read_bits(kDistExtra[dsym], dextra)) return false;
            const size_t distance = static_cast<size_t>(kDistBase[dsym] + dextra);
            if (distance == 0 || distance > out.size()) {
                return false;
            }
            if (out.size() + length > max_out) {
                const size_t avail = max_out - out.size();
                for (size_t i = 0; i < avail; ++i) {
                    out.push_back(out[out.size() - distance]);
                }
                return false;
            }
            for (size_t i = 0; i < length; ++i) {
                out.push_back(out[out.size() - distance]);
            }
        }
    }
}

} // namespace detail

inline bool inflate_raw(const uint8_t* src, size_t src_size, std::vector<uint8_t>& out,
                        size_t max_out = 16u * 1024u * 1024u, size_t* out_consumed = nullptr) {
    out.clear();
    if (!src || src_size == 0) return false;
    detail::BitReader br{src, src_size, 0, 0, 0};

    detail::HuffTable fixed_ll, fixed_dist;
    {
        uint8_t ll[288]{};
        for (int i = 0; i <= 143; ++i) ll[i] = 8;
        for (int i = 144; i <= 255; ++i) ll[i] = 9;
        for (int i = 256; i <= 279; ++i) ll[i] = 7;
        for (int i = 280; i <= 287; ++i) ll[i] = 8;
        uint8_t dl[32]{};
        for (int i = 0; i < 32; ++i) dl[i] = 5;
        if (!fixed_ll.build(ll, 288) || !fixed_dist.build(dl, 32)) return false;
    }

    for (;;) {
        uint32_t bfinal = 0, btype = 0;
        if (!br.read_bits(1, bfinal) || !br.read_bits(2, btype)) return false;
        if (btype == 0u) {
            uint8_t hdr[4]{};
            if (!br.read_bytes_aligned(hdr, 4)) return false;
            const uint16_t len = static_cast<uint16_t>(hdr[0] | (hdr[1] << 8));
            const uint16_t nlen = static_cast<uint16_t>(hdr[2] | (hdr[3] << 8));
            if (static_cast<uint16_t>(~len) != nlen) return false;
            if (out.size() + len > max_out) {
                const size_t avail = max_out - out.size();
                const size_t base = out.size();
                out.resize(base + avail);
                br.read_bytes_aligned(out.data() + base, avail);
                return false;
            }
            const size_t base = out.size();
            out.resize(base + len);
            if (!br.read_bytes_aligned(out.data() + base, len)) return false;
        } else if (btype == 1u) {
            if (!detail::decode_block(br, fixed_ll, fixed_dist, out, max_out)) return false;
        } else if (btype == 2u) {
            uint32_t hlit = 0, hdist = 0, hclen = 0;
            if (!br.read_bits(5, hlit) || !br.read_bits(5, hdist) || !br.read_bits(4, hclen)) return false;
            hlit += 257u;
            hdist += 1u;
            hclen += 4u;
            if (hlit > 286u || hdist > 30u) return false;

            static constexpr uint8_t kCodeOrder[19] = {
                16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
            };
            uint8_t cl_lens[19]{};
            for (uint32_t i = 0; i < hclen; ++i) {
                uint32_t v = 0;
                if (!br.read_bits(3, v)) return false;
                cl_lens[kCodeOrder[i]] = static_cast<uint8_t>(v);
            }
            detail::HuffTable cl_tab;
            if (!cl_tab.build(cl_lens, 19)) return false;

            std::vector<uint8_t> lengths(hlit + hdist, 0u);
            size_t idx = 0;
            while (idx < lengths.size()) {
                uint16_t sym = 0;
                if (!cl_tab.decode(br, sym)) return false;
                if (sym < 16u) {
                    lengths[idx++] = static_cast<uint8_t>(sym);
                } else if (sym == 16u) {
                    if (idx == 0) return false;
                    uint32_t rep = 0;
                    if (!br.read_bits(2, rep)) return false;
                    rep += 3u;
                    const uint8_t prev = lengths[idx - 1];
                    if (idx + rep > lengths.size()) return false;
                    while (rep--) lengths[idx++] = prev;
                } else if (sym == 17u) {
                    uint32_t rep = 0;
                    if (!br.read_bits(3, rep)) return false;
                    rep += 3u;
                    if (idx + rep > lengths.size()) return false;
                    while (rep--) lengths[idx++] = 0u;
                } else if (sym == 18u) {
                    uint32_t rep = 0;
                    if (!br.read_bits(7, rep)) return false;
                    rep += 11u;
                    if (idx + rep > lengths.size()) return false;
                    while (rep--) lengths[idx++] = 0u;
                } else {
                    return false;
                }
            }
            detail::HuffTable dyn_ll, dyn_dist;
            if (!dyn_ll.build(lengths.data(), hlit) ||
                !dyn_dist.build(lengths.data() + hlit, hdist)) {
                return false;
            }
            if (!detail::decode_block(br, dyn_ll, dyn_dist, out, max_out)) return false;
        } else {
            return false;
        }
        if (bfinal != 0u) {
            if (out_consumed) {
                br.align_byte();
                *out_consumed = br.byte_pos - (br.bit_count / 8u);
            }
            return true;
        }
    }
}

inline bool inflate_zlib(const uint8_t* src, size_t src_size, std::vector<uint8_t>& out,
                         size_t max_out = 16u * 1024u * 1024u, size_t* out_consumed = nullptr) {
    if (!src || src_size < 6) return false;
    const uint8_t cmf = src[0];
    const uint8_t flg = src[1];
    if ((cmf & 0x0Fu) != 8u) return false;
    if (((static_cast<uint32_t>(cmf) * 256u + flg) % 31u) != 0u) return false;
    size_t hdr = 2;
    if ((flg & 0x20u) != 0u) {
        if (src_size < 10) return false;
        hdr += 4;
    }
    size_t raw_consumed = 0;
    const bool ok = inflate_raw(src + hdr, src_size - hdr, out, max_out, &raw_consumed);
    if (ok && out_consumed) {
        *out_consumed = std::min(src_size, hdr + raw_consumed + 4u);
    }
    return ok;
}

} // namespace wsx_inflate
