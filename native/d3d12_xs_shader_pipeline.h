// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox Series X|S Shader Container Inspector, FSSM Unpacker & Translation Pipeline:
// - Classifies all Xbox Series X|S shader binary layouts empirically observed in Scarlett titles:
//     1. DesktopDxil        : standard desktop DXBC/DXIL bytecode
//     2. XboxDxilPure       : dxil-ms-xb2 LLVM bitcode with 0 @xbox.* intrinsics (runs natively on desktop D3D12)
//     3. XboxDxilIntrinsics : dxil-ms-xb2 LLVM bitcode using proprietary @xbox.* RDNA2 intrinsics
//     4. XboxPrecompiledFssm: DXIL stub (NOBC) + XDAT -> XBCH (zlib) -> FSSM precompiled RDNA2 machine code
//     5. XboxPrecompiledAccc: ACCC (FSPX) precompiled RDNA2 machine code (e.g. FSR 4.1.1 Xbox DLL)
// - Built-in zero-dependency RFC 1951/1950 Inflate to unpack XBCH -> FSSM and locate RDNA2 ISA code slices.
// - Pluggable shader translation pipeline:
//     * Disk pre-translated cache lookup (WSX_SHADER_CACHE_DIR / [Graphics] shader_cache_dir): <hash>.dxil / <hash>.cso / <hash>.spv
//     * Runtime translator callback (D3D12XboxRegisterShaderTranslator)
//     * Opt-in strict pre-driver rejection (WSX_STRICT_SHADER_CHECK=1) to avoid driver crashes on untranslated RDNA2/@xbox.* PSOs.

#pragma once

#include "wsx_config.h"
#include "wsx_inflate.h"
#include "d3d12_xs_xbox_abi.h"

#include <Windows.h>
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace wsx_shader {

enum class ShaderKind : uint32_t {
    Unknown           = 0u,
    DesktopDxil       = 1u,
    XboxDxilPure      = 2u,
    XboxDxilIntrinsic = 3u,
    XboxFssmRdna2     = 4u,
    XboxAcccRdna2     = 5u,
};

enum class TargetFormat : uint32_t {
    None  = 0u,
    Dxil  = 1u,
    Spirv = 2u,
};

enum class GraphicsBackend : uint32_t {
    D3D12  = 0u,
    Vulkan = 1u,
};

struct ShaderInfo {
    uint32_t kind = 0u;
    uint32_t stage = 0xFFFFu;
    uint16_t sm_major = 0u;
    uint16_t sm_minor = 0u;
    uint32_t root_sig_flags = 0u;
    uint32_t xbox_intrinsic_count = 0u;
    uint32_t has_xbox_typed_load = 0u;
    uint32_t has_nobc_stub = 0u;
    uint32_t has_rdna2_endpgm = 0u;
    uint32_t xbch_stored_size = 0u;
    uint32_t xbch_unpacked_size = 0u;
    uint32_t fssm_code_offset = 0u;
    uint32_t fssm_code_size = 0u;
    uint32_t accc_payload_size = 0u;
    uint8_t container_digest[16]{};
    uint8_t shader_hash[16]{};
    uint8_t xdat_hash[16]{};
};

using TranslateCallback = HRESULT(WINAPI*)(
    const ShaderInfo* pInfo,
    const void* pOriginalDxbc,
    size_t originalDxbcSize,
    const void* pUnpackedFssm,
    size_t unpackedFssmSize,
    uint32_t* pOutTargetFormat,
    void* pOutBuffer,
    size_t outBufferCapacity,
    size_t* pOutBufferSize,
    void* pUserContext);

struct ShaderPipelineStats {
    std::atomic<uint64_t> inspected{0};
    std::atomic<uint64_t> pure_dxil{0};
    std::atomic<uint64_t> xbox_intrinsics{0};
    std::atomic<uint64_t> fssm_rdna2{0};
    std::atomic<uint64_t> accc_rdna2{0};
    std::atomic<uint64_t> cache_hits{0};
    std::atomic<uint64_t> translator_hits{0};
    std::atomic<uint64_t> rejected_strict{0};
};

inline ShaderPipelineStats& shader_stats() noexcept {
    static ShaderPipelineStats s;
    return s;
}

// RFC 1951 / 1950 Inflate lives in wsx_inflate.h
namespace inflate_detail = ::wsx_inflate;

// ============================================================================
// 2. Zero-dependency LLVM 3.7 Bitcode Pattern Scanner (char6 & unaligned ASCII)
// ============================================================================
namespace bitcode_detail {

inline int char6_code(char ch) noexcept {
    if (ch >= 'a' && ch <= 'z') return ch - 'a';
    if (ch >= 'A' && ch <= 'Z') return 26 + (ch - 'A');
    if (ch >= '0' && ch <= '9') return 52 + (ch - '0');
    if (ch == '.') return 62;
    if (ch == '_') return 63;
    return -1;
}

// Counts occurrences of an arbitrary little-endian bit pattern (up to 64 bits) across all bit offsets.
inline uint32_t count_bit_pattern64(const uint8_t* data, size_t size, uint64_t pattern, uint32_t nbits) noexcept {
    if (!data || size == 0 || nbits == 0 || nbits > 64u) return 0u;
    const uint64_t total_bits = static_cast<uint64_t>(size) * 8ULL;
    if (total_bits < nbits) return 0u;
    const uint64_t mask = (nbits == 64u) ? ~0ULL : ((1ULL << nbits) - 1ULL);
    const uint64_t target = pattern & mask;
    uint32_t hits = 0u;
    const size_t min_bytes = (nbits + 7u) / 8u;
    for (size_t byte_i = 0; byte_i + min_bytes <= size; ++byte_i) {
        uint64_t w0 = 0;
        const size_t avail = std::min<size_t>(8u, size - byte_i);
        std::memcpy(&w0, data + byte_i, avail);
        const uint8_t next_b = (byte_i + 8u < size) ? data[byte_i + 8u] : 0u;
        const uint32_t max_boff = (byte_i + min_bytes == size)
            ? static_cast<uint32_t>(std::min<uint64_t>(7ULL, (size - byte_i) * 8ULL - nbits))
            : 7u;
        for (uint32_t boff = 0; boff <= max_boff; ++boff) {
            uint64_t window = w0 >> boff;
            if (boff > 0 && nbits + boff > 64u) {
                window |= static_cast<uint64_t>(next_b) << (64u - boff);
            }
            if ((window & mask) == target) {
                ++hits;
            }
        }
    }
    return hits;
}

// Counts occurrences of an unaligned ASCII string (any bit offset 0..7, length up to 16 chars).
inline uint32_t count_unaligned_ascii(const uint8_t* data, size_t size, const char* str) noexcept {
    if (!data || !str) return 0u;
    const size_t len = std::strlen(str);
    if (len == 0 || len > 16 || size < len) return 0u;
    uint32_t hits = 0u;
    for (uint32_t boff = 0; boff < 8u; ++boff) {
        const size_t limit = (boff == 0) ? (size - len + 1) : (size - len);
        for (size_t i = 0; i < limit; ++i) {
            bool match = true;
            for (size_t k = 0; k < len; ++k) {
                const uint8_t cur = (boff == 0)
                    ? data[i + k]
                    : static_cast<uint8_t>((data[i + k] >> boff) | (data[i + k + 1] << (8u - boff)));
                if (cur != static_cast<uint8_t>(str[k])) {
                    match = false;
                    break;
                }
            }
            if (match) ++hits;
        }
    }
    return hits;
}

inline uint32_t count_char6_string(const uint8_t* data, size_t size, const char* str) noexcept {
    if (!data || !str) return 0u;
    const size_t len = std::strlen(str);
    if (len == 0 || len > 10) return 0u; // 10 * 6 = 60 bits <= 64
    uint64_t pat = 0;
    for (size_t i = 0; i < len; ++i) {
        const int c = char6_code(str[i]);
        if (c < 0) return 0u;
        pat |= static_cast<uint64_t>(c) << (6u * i);
    }
    return count_bit_pattern64(data, size, pat, static_cast<uint32_t>(len * 6u));
}

} // namespace bitcode_detail

// ============================================================================
// 3. Xbox Series X|S Shader Container Inspector & FSSM Unpacker
// ============================================================================

inline uint32_t read_u32(const uint8_t* p) noexcept {
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

inline bool find_fourcc(const uint8_t* data, size_t size, const char fourcc[4], size_t& out_pos) noexcept {
    if (!data || size < 4) return false;
    for (size_t i = 0; i + 4 <= size; ++i) {
        if (data[i] == static_cast<uint8_t>(fourcc[0]) &&
            data[i + 1] == static_cast<uint8_t>(fourcc[1]) &&
            data[i + 2] == static_cast<uint8_t>(fourcc[2]) &&
            data[i + 3] == static_cast<uint8_t>(fourcc[3])) {
            out_pos = i;
            return true;
        }
    }
    return false;
}

inline bool find_last_s_endpgm(const uint8_t* data, size_t size, size_t start_off, size_t& out_end_pos) noexcept {
    if (!data || start_off + 4 > size) return false;
    bool found = false;
    for (size_t i = start_off; i + 4 <= size; i += 4) {
        if (read_u32(data + i) == 0xBF810000u) { // AMD RDNA2 s_endpgm
            out_end_pos = i + 4;
            found = true;
        }
    }
    if (!found) {
        // Fallback byte-aligned scan in case container payload is not 4-byte aligned
        for (size_t i = start_off; i + 4 <= size; ++i) {
            if (read_u32(data + i) == 0xBF810000u) {
                out_end_pos = i + 4;
                found = true;
            }
        }
    }
    return found;
}

inline bool unpack_fssm_from_dxbc(
    const void* dxbc_ptr,
    size_t dxbc_size,
    std::vector<uint8_t>& out_fssm,
    uint32_t* out_code_offset = nullptr,
    uint32_t* out_code_size = nullptr,
    uint8_t* out_xdat_hash16 = nullptr,
    uint32_t* out_stored_size = nullptr,
    uint32_t* out_unpacked_size = nullptr) noexcept {
    out_fssm.clear();
    if (!dxbc_ptr || dxbc_size < 36) return false;
    const auto* b = static_cast<const uint8_t*>(dxbc_ptr);
    if (std::memcmp(b, "DXBC", 4) != 0) return false;
    const uint32_t chunk_cnt = read_u32(b + 28);
    if (chunk_cnt == 0 || chunk_cnt > 64 || 32u + static_cast<size_t>(chunk_cnt) * 4u > dxbc_size) {
        return false;
    }
    for (uint32_t c = 0; c < chunk_cnt; ++c) {
        const uint32_t coff = read_u32(b + 32u + c * 4u);
        if (coff + 8u > dxbc_size) continue;
        const uint32_t csz = read_u32(b + coff + 4u);
        if (coff + 8u + csz > dxbc_size) continue;
        if (std::memcmp(b + coff, "DXIL", 4) != 0) continue;

        const uint8_t* dxil = b + coff + 8u;
        size_t xdat_pos = 0;
        if (!find_fourcc(dxil, csz, "XDAT", xdat_pos) || xdat_pos + 12u > csz) {
            return false;
        }
        const uint32_t xcnt = read_u32(dxil + xdat_pos + 4u);
        if (xcnt == 0 || xcnt > 64u || xdat_pos + 12u + static_cast<size_t>(xcnt) * 12u > csz) {
            return false;
        }
        bool unpacked_ok = false;
        for (uint32_t k = 0; k < xcnt; ++k) {
            const size_t epos = xdat_pos + 12u + static_cast<size_t>(k) * 12u;
            const uint32_t tag = read_u32(dxil + epos + 0u);
            const uint32_t off = read_u32(dxil + epos + 4u);
            const uint32_t sz  = read_u32(dxil + epos + 8u);
            if (static_cast<size_t>(off) + sz > csz) continue;
            if (tag == 0x12u && sz >= 16u && out_xdat_hash16) {
                std::memcpy(out_xdat_hash16, dxil + off, 16);
            }
            if (tag == 0x0Du && sz >= 24u && std::memcmp(dxil + off, "XBCH", 4) == 0) {
                const uint32_t stored = read_u32(dxil + off + 8u);
                const uint32_t unpacked = read_u32(dxil + off + 16u);
                if (out_stored_size) *out_stored_size = stored;
                if (out_unpacked_size) *out_unpacked_size = unpacked;
                if (24u + static_cast<size_t>(stored) <= sz && unpacked > 0 && unpacked <= 16u * 1024u * 1024u) {
                    if (inflate_detail::inflate_zlib(dxil + off + 24u, stored, out_fssm, unpacked + 4096u)) {
                        unpacked_ok = true;
                        if (out_fssm.size() >= 0xC4u && std::memcmp(out_fssm.data(), "FSSM", 4) == 0) {
                            const uint32_t code_off = read_u32(out_fssm.data() + 0xC0u) + 0x28u;
                            if (out_code_offset) *out_code_offset = code_off;
                            size_t end_pos = 0;
                            if (code_off < out_fssm.size() &&
                                find_last_s_endpgm(out_fssm.data(), out_fssm.size(), code_off, end_pos)) {
                                if (out_code_size) *out_code_size = static_cast<uint32_t>(end_pos - code_off);
                            }
                        }
                    }
                }
            }
        }
        return unpacked_ok;
    }
    return false;
}

// Leonida discovery: 195 $VS_* (VoxelizeGeneric) shader containers in shaders.rpf have TWO distinct
// tag-0x0D XBCH -> FSSM payloads inside the same XDAT directory!
// This helper extracts all tag-0x0D FSSM payloads in order into `out_payloads`.
inline size_t unpack_all_fssm_from_dxbc(
    const void* dxbc_ptr,
    size_t dxbc_size,
    std::vector<std::vector<uint8_t>>& out_payloads) noexcept {
    out_payloads.clear();
    if (!dxbc_ptr || dxbc_size < 36u) return 0u;
    const auto* b = static_cast<const uint8_t*>(dxbc_ptr);
    if (std::memcmp(b, "DXBC", 4) != 0) return 0u;
    const uint32_t chunk_cnt = read_u32(b + 28u);
    if (chunk_cnt == 0u || chunk_cnt > 64u || 32u + static_cast<size_t>(chunk_cnt) * 4u > dxbc_size) {
        return 0u;
    }
    for (uint32_t c = 0u; c < chunk_cnt; ++c) {
        const uint32_t coff = read_u32(b + 32u + c * 4u);
        if (coff + 8u > dxbc_size) continue;
        const uint32_t csz = read_u32(b + coff + 4u);
        if (coff + 8u + csz > dxbc_size) continue;
        if (std::memcmp(b + coff, "DXIL", 4) != 0) continue;

        const uint8_t* dxil = b + coff + 8u;
        size_t xdat_pos = 0u;
        if (!find_fourcc(dxil, csz, "XDAT", xdat_pos) || xdat_pos + 12u > csz) {
            return 0u;
        }
        const uint32_t xcnt = read_u32(dxil + xdat_pos + 4u);
        if (xcnt == 0u || xcnt > 64u || xdat_pos + 12u + static_cast<size_t>(xcnt) * 12u > csz) {
            return 0u;
        }
        for (uint32_t k = 0u; k < xcnt; ++k) {
            const size_t epos = xdat_pos + 12u + static_cast<size_t>(k) * 12u;
            const uint32_t tag = read_u32(dxil + epos + 0u);
            const uint32_t off = read_u32(dxil + epos + 4u);
            const uint32_t sz  = read_u32(dxil + epos + 8u);
            if (static_cast<size_t>(off) + sz > csz) continue;
            if (tag == 0x0Du && sz >= 24u && std::memcmp(dxil + off, "XBCH", 4) == 0) {
                const uint32_t stored = read_u32(dxil + off + 8u);
                const uint32_t unpacked = read_u32(dxil + off + 16u);
                if (24u + static_cast<size_t>(stored) <= sz && unpacked > 0u && unpacked <= 16u * 1024u * 1024u) {
                    std::vector<uint8_t> fssm;
                    if (inflate_detail::inflate_zlib(dxil + off + 24u, stored, fssm, unpacked + 4096u)) {
                        out_payloads.push_back(std::move(fssm));
                    }
                }
            }
        }
        return out_payloads.size();
    }
    return 0u;
}

// Xbox2Windows DXIL Retargeting:
// Rewrites LLVM 3.7 bitcode MODULE_CODE_TRIPLE / embedded target triple from "dxil-ms-xb2" to "dxil-ms-dx"
// (both raw ASCII and 6-bit VBR/unaligned bit-packed forms) inside a DXBC/DXIL container.
inline uint32_t retarget_xbox_dxil_triple_inplace(std::vector<uint8_t>& blob) noexcept {
    if (blob.size() < 36u) return 0u;
    uint32_t patched = 0u;

    // 1. Byte-aligned ASCII occurrences of "dxil-ms-xb2" (11 chars) -> "dxil-ms-dx\0" or length-preserving "dxil-ms-dx "
    static constexpr char kXb2[11] = {'d','x','i','l','-','m','s','-','x','b','2'};
    static constexpr char kDx[11]  = {'d','x','i','l','-','m','s','-','d','x', 0 };
    for (size_t i = 0u; i + 11u <= blob.size(); ++i) {
        if (std::memcmp(blob.data() + i, kXb2, 11u) == 0) {
            std::memcpy(blob.data() + i, kDx, 11u);
            ++patched;
        }
    }

    // 2. Arbitrary bit-aligned 8-bit VBR / literal stream inside LLVM 3.7 bitcode ("xb2" following "dxil-ms-"):
    //    In LLVM bitcode unabbreviated MODULE_CODE_TRIPLE (code 2), each character is emitted as a 6-bit VBR (8 bits for ASCII >= 32 or '-'/'2').
    //    Scan bit offsets 0..7 across the container for the exact 88-bit raw byte pattern of "dxil-ms-xb2".
    for (uint32_t bit_off = 1u; bit_off < 8u; ++bit_off) {
        for (size_t byte_pos = 20u; byte_pos + 12u < blob.size(); ++byte_pos) {
            bool match = true;
            for (size_t k = 0u; k < 11u; ++k) {
                const uint16_t w = static_cast<uint16_t>(blob[byte_pos + k]) |
                                   (static_cast<uint16_t>(blob[byte_pos + k + 1u]) << 8u);
                const uint8_t ch = static_cast<uint8_t>((w >> bit_off) & 0xFFu);
                if (ch != static_cast<uint8_t>(kXb2[k])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                for (size_t k = 0u; k < 11u; ++k) {
                    const uint16_t mask = static_cast<uint16_t>(0xFFu << bit_off);
                    uint16_t w = static_cast<uint16_t>(blob[byte_pos + k]) |
                                 (static_cast<uint16_t>(blob[byte_pos + k + 1u]) << 8u);
                    w = static_cast<uint16_t>((w & ~mask) | (static_cast<uint16_t>(static_cast<uint8_t>(kDx[k])) << bit_off));
                    blob[byte_pos + k]      = static_cast<uint8_t>(w & 0xFFu);
                    blob[byte_pos + k + 1u] = static_cast<uint8_t>((w >> 8u) & 0xFFu);
                }
                ++patched;
            }
        }
    }
    return patched;
}

struct AcccCodeSlice {
    const uint8_t* code_ptr = nullptr;
    uint32_t code_size = 0u;
    uint32_t stage = 5u; // Compute by default for FSPX compute shaders
    uint32_t numthreads_x = 64u;
    uint32_t numthreads_y = 1u;
    uint32_t numthreads_z = 1u;
};

// Parses Xbox ACCC (FSPX) precompiled RDNA2 shader containers (e.g., amd_fidelityfx_upscaler_xbox.dll).
inline bool extract_accc_fspx_code(const void* dxbc_ptr, size_t dxbc_size, AcccCodeSlice& out) noexcept {
    out = {};
    if (!dxbc_ptr || dxbc_size < 36u) return false;
    const auto* b = static_cast<const uint8_t*>(dxbc_ptr);
    if (std::memcmp(b, "DXBC", 4) != 0) return false;
    const uint32_t chunk_cnt = read_u32(b + 28u);
    if (chunk_cnt == 0u || chunk_cnt > 64u || 32u + static_cast<size_t>(chunk_cnt) * 4u > dxbc_size) {
        return false;
    }

    const uint8_t* accc = nullptr;
    uint32_t accc_sz = 0u;
    const uint8_t* psv0 = nullptr;
    uint32_t psv0_sz = 0u;

    for (uint32_t c = 0u; c < chunk_cnt; ++c) {
        const uint32_t coff = read_u32(b + 32u + c * 4u);
        if (coff + 8u > dxbc_size) continue;
        const uint32_t csz = read_u32(b + coff + 4u);
        if (coff + 8u + csz > dxbc_size) continue;
        if (std::memcmp(b + coff, "ACCC", 4) == 0) {
            accc = b + coff + 8u;
            accc_sz = csz;
        } else if (std::memcmp(b + coff, "PSV0", 4) == 0) {
            psv0 = b + coff + 8u;
            psv0_sz = csz;
        }
    }

    if (!accc || accc_sz < 32u || std::memcmp(accc, "FSPX", 4) != 0) return false;
    const uint32_t sec_cnt = read_u32(accc + 8u);
    if (sec_cnt == 0u || sec_cnt > 32u || 16u + static_cast<size_t>(sec_cnt) * 16u > accc_sz) {
        return false;
    }

    bool found_code = false;
    for (uint32_t s = 0u; s < sec_cnt; ++s) {
        const size_t e = 16u + static_cast<size_t>(s) * 16u;
        const uint32_t tag = read_u32(accc + e + 0u);
        const uint32_t off = read_u32(accc + e + 4u);
        const uint32_t sz  = read_u32(accc + e + 8u);
        if (static_cast<size_t>(off) + sz > accc_sz) continue;
        if (tag == 0u && sz >= 20u) {
            const uint32_t w3 = read_u32(accc + off + 12u);
            const uint32_t w4 = read_u32(accc + off + 16u);
            const uint32_t ntx = w3 & 0xFFFFu;
            const uint32_t nty = (w3 >> 16u) & 0xFFFFu;
            const uint32_t ntz = w4 & 0xFFFFu;
            if (ntx >= 1u && ntx <= 1024u && nty >= 1u && nty <= 1024u && ntz >= 1u && ntz <= 64u) {
                out.numthreads_x = ntx;
                out.numthreads_y = nty;
                out.numthreads_z = ntz;
            }
        } else if (tag == 6u && sz >= 4u) {
            out.code_ptr = accc + off;
            out.code_size = sz;
            found_code = true;
        }
    }

    if (!found_code) return false;
    if (psv0 && psv0_sz >= 32u) {
        const uint32_t psv_stage = read_u32(psv0 + 28u);
        if (psv_stage <= 13u) out.stage = psv_stage;
        if (psv0_sz >= 52u && out.stage == 5u) {
            const uint32_t pnx = read_u32(psv0 + 40u);
            const uint32_t pny = read_u32(psv0 + 44u);
            const uint32_t pnz = read_u32(psv0 + 48u);
            if (pnx >= 1u && pnx <= 1024u && pny >= 1u && pny <= 1024u && pnz >= 1u && pnz <= 64u) {
                out.numthreads_x = pnx;
                out.numthreads_y = pny;
                out.numthreads_z = pnz;
            }
        }
    }
    return true;
}

inline HRESULT inspect_shader_bytecode(const void* data, size_t size, ShaderInfo* out,
                                       std::vector<uint8_t>* optional_fssm_out = nullptr) noexcept {
    if (!out) return E_POINTER;
    std::memset(out, 0, sizeof(*out));
    out->stage = 0xFFFFu;
    if (!data || size < 32) return E_INVALIDARG;

    const auto* b = static_cast<const uint8_t*>(data);
    if (std::memcmp(b, "DXBC", 4) != 0) {
        out->kind = static_cast<uint32_t>(ShaderKind::Unknown);
        return S_OK;
    }

    std::memcpy(out->container_digest, b + 4, 16);
    std::memcpy(out->shader_hash, b + 4, 16); // Overridden by HASH chunk if present

    const uint32_t chunk_cnt = read_u32(b + 28);
    if (chunk_cnt == 0 || chunk_cnt > 64u || 32u + static_cast<size_t>(chunk_cnt) * 4u > size) {
        return E_INVALIDARG;
    }

    bool has_dxil = false;
    bool has_accc = false;
    bool has_xdat = false;
    const uint8_t* dxil_payload = nullptr;
    uint32_t dxil_size = 0u;
    const uint8_t* accc_payload = nullptr;
    uint32_t accc_size = 0u;

    for (uint32_t c = 0; c < chunk_cnt; ++c) {
        const uint32_t coff = read_u32(b + 32u + c * 4u);
        if (coff + 8u > size) continue;
        const uint32_t csz = read_u32(b + coff + 4u);
        if (coff + 8u + csz > size) continue;
        const uint8_t* cp = b + coff + 8u;

        if (std::memcmp(b + coff, "HASH", 4) == 0 && csz >= 20u) {
            std::memcpy(out->shader_hash, cp + 4u, 16);
        } else if (std::memcmp(b + coff, "RTS0", 4) == 0 && csz >= 24u) {
            out->root_sig_flags = read_u32(cp + 20u);
        } else if (std::memcmp(b + coff, "DXIL", 4) == 0 && csz >= 16u) {
            has_dxil = true;
            dxil_payload = cp;
            dxil_size = csz;
            const uint32_t prog_ver = read_u32(cp);
            out->stage = (prog_ver >> 16) & 0xFFFFu;
            out->sm_major = static_cast<uint16_t>((prog_ver >> 4) & 0xFu);
            out->sm_minor = static_cast<uint16_t>(prog_ver & 0xFu);
        } else if (std::memcmp(b + coff, "ACCC", 4) == 0) {
            has_accc = true;
            accc_payload = cp;
            accc_size = csz;
            out->accc_payload_size = csz;
        }
    }

    if (has_accc && !has_dxil) {
        out->kind = static_cast<uint32_t>(ShaderKind::XboxAcccRdna2);
        AcccCodeSlice slice{};
        if (extract_accc_fspx_code(data, size, slice)) {
            out->stage = slice.stage;
            out->fssm_code_offset = static_cast<uint32_t>(slice.code_ptr - accc_payload);
            out->fssm_code_size = slice.code_size;
        }
        size_t end_pos = 0;
        if (find_last_s_endpgm(accc_payload, accc_size, 0, end_pos)) {
            out->has_rdna2_endpgm = 1u;
        }
        return S_OK;
    }

    if (has_dxil && dxil_payload && dxil_size >= 24u) {
        size_t xdat_pos = 0;
        if (find_fourcc(dxil_payload, dxil_size, "XDAT", xdat_pos)) {
            has_xdat = true;
            size_t nobc_pos = 0;
            if (find_fourcc(dxil_payload, dxil_size, "NOBC", nobc_pos)) {
                out->has_nobc_stub = 1u;
            }
            // Unpack embedded XBCH -> FSSM metadata (present in both stripped NOBC game shaders
            // and dual-payload Xbox DXIL+FSSM middleware containers such as XboxBvhBuilder.dll).
            std::vector<uint8_t> local_fssm;
            auto& target_fssm = optional_fssm_out ? *optional_fssm_out : local_fssm;
            if (unpack_fssm_from_dxbc(data, size, target_fssm,
                                      &out->fssm_code_offset, &out->fssm_code_size,
                                      out->xdat_hash, &out->xbch_stored_size, &out->xbch_unpacked_size)) {
                out->has_rdna2_endpgm = (out->fssm_code_size > 0u) ? 1u : 0u;
            }
            // If LLVM bitcode was stripped (NOBC), only precompiled RDNA2 FSSM remains:
            if (out->has_nobc_stub != 0u) {
                out->kind = static_cast<uint32_t>(ShaderKind::XboxFssmRdna2);
                return S_OK;
            }
            // Dual-payload container (has both DXIL LLVM bitcode and XBCH/FSSM): fall through to
            // inspect the LLVM bitcode so pure dxil-ms-xb2 shaders run natively on desktop D3D12.
        }

        // Standard or Xbox LLVM bitcode DXIL: detect @xbox.* intrinsics (char6) and !xbox.compiler metadata
        const uint32_t intr_cnt = bitcode_detail::count_char6_string(dxil_payload, dxil_size, "xbox.");
        const uint32_t typed_cnt = bitcode_detail::count_char6_string(dxil_payload, dxil_size, "xbox.Typed");
        const uint32_t comp_meta = bitcode_detail::count_unaligned_ascii(dxil_payload, dxil_size, "xbox.compiler");
        out->xbox_intrinsic_count = intr_cnt;
        out->has_xbox_typed_load = (typed_cnt > 0u) ? 1u : 0u;

        if (intr_cnt > 0u) {
            out->kind = static_cast<uint32_t>(ShaderKind::XboxDxilIntrinsic);
        } else if (comp_meta > 0u || (out->root_sig_flags & 0x03000000u) != 0u) {
            out->kind = static_cast<uint32_t>(ShaderKind::XboxDxilPure);
        } else {
            out->kind = static_cast<uint32_t>(ShaderKind::DesktopDxil);
        }
        return S_OK;
    }

    // Legacy SM 5.x DXBC (SHEX/SHDR)
    if (!has_xdat) {
        out->kind = static_cast<uint32_t>(ShaderKind::DesktopDxil);
    }
    return S_OK;
}

// ============================================================================
// 4. Graphics Backend Selector, Disk Shader Cache & Pluggable Translator Hook
// ============================================================================

inline GraphicsBackend get_graphics_backend() noexcept {
    const std::string val = wsx::detail::to_lower_ascii(
        wsx::get_config_string("WSX_GRAPHICS_BACKEND", "Graphics", "backend", "d3d12"));
    if (val == "vulkan" || val == "vk") {
        return GraphicsBackend::Vulkan;
    }
    return GraphicsBackend::D3D12;
}

inline bool strict_shader_check_enabled() noexcept {
    return wsx::get_config_bool("WSX_STRICT_SHADER_CHECK", "Graphics", "strict_shader_check", false);
}

inline std::string hash16_to_hex(const uint8_t h[16]) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(32, '0');
    for (size_t i = 0; i < 16; ++i) {
        out[i * 2 + 0] = kHex[(h[i] >> 4) & 0xFu];
        out[i * 2 + 1] = kHex[h[i] & 0xFu];
    }
    return out;
}

using BuiltinFssmTranslatorFn = HRESULT (*)(
    const ShaderInfo& info,
    const void* fssm_data,
    size_t fssm_size,
    std::vector<uint8_t>& out_dxil) noexcept;

struct TranslatorRegistry {
    std::mutex mtx;
    TranslateCallback callback = nullptr;
    void* user_ctx = nullptr;
    BuiltinFssmTranslatorFn builtin_fssm = nullptr;
};

inline TranslatorRegistry& translator_registry() noexcept {
    static TranslatorRegistry reg;
    return reg;
}

inline void register_builtin_fssm_translator(BuiltinFssmTranslatorFn fn) noexcept {
    auto& reg = translator_registry();
    std::lock_guard<std::mutex> lock(reg.mtx);
    reg.builtin_fssm = fn;
}

inline bool auto_translate_fssm_enabled() noexcept {
    return wsx::get_config_bool("WSX_AUTO_TRANSLATE_FSSM", "Graphics", "auto_translate_fssm", false);
}

inline void register_shader_translator(TranslateCallback cb, void* ctx) noexcept {
    auto& reg = translator_registry();
    std::lock_guard<std::mutex> lock(reg.mtx);
    reg.callback = cb;
    reg.user_ctx = ctx;
}

inline bool hash16_is_nonzero(const uint8_t h[16]) noexcept {
    for (int i = 0; i < 16; ++i) {
        if (h[i] != 0u) return true;
    }
    return false;
}

inline std::string preferred_cache_key_hex(const ShaderInfo& info) {
    if (hash16_is_nonzero(info.shader_hash)) return hash16_to_hex(info.shader_hash);
    if (hash16_is_nonzero(info.xdat_hash)) return hash16_to_hex(info.xdat_hash);
    return hash16_to_hex(info.container_digest);
}

inline bool load_cached_dxil_by_hash(const ShaderInfo& info, std::vector<uint8_t>& out_bytecode) {
    const std::string dir = wsx::get_config_string("WSX_SHADER_CACHE_DIR", "Graphics", "shader_cache_dir", "");
    if (dir.empty()) return false;

    const std::filesystem::path root(dir);
    std::vector<std::filesystem::path> candidates;
    candidates.reserve(6);
    if (hash16_is_nonzero(info.shader_hash)) {
        const std::string h = hash16_to_hex(info.shader_hash);
        candidates.push_back(root / (h + ".dxil"));
        candidates.push_back(root / (h + ".cso"));
    }
    if (hash16_is_nonzero(info.xdat_hash)) {
        const std::string h = hash16_to_hex(info.xdat_hash);
        candidates.push_back(root / (h + ".dxil"));
        candidates.push_back(root / (h + ".cso"));
    }
    if (hash16_is_nonzero(info.container_digest)) {
        const std::string h = hash16_to_hex(info.container_digest);
        candidates.push_back(root / (h + ".dxil"));
        candidates.push_back(root / (h + ".cso"));
    }
    for (const auto& path : candidates) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) continue;
        std::ifstream f(path, std::ios::binary);
        if (!f) continue;
        out_bytecode.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (!out_bytecode.empty()) {
            return true;
        }
    }
    return false;
}

inline bool save_cached_dxil_by_hash(const ShaderInfo& info, const std::vector<uint8_t>& bytecode) noexcept {
    if (bytecode.size() < 32u) return false;
    try {
        const std::string dir = wsx::get_config_string("WSX_SHADER_CACHE_DIR", "Graphics", "shader_cache_dir", "");
        if (dir.empty()) return false;

        const std::filesystem::path root(dir);
        std::error_code ec;
        std::filesystem::create_directories(root, ec);

        const std::string hex = preferred_cache_key_hex(info);
        const std::filesystem::path dst = root / (hex + ".dxil");
        char tmp_suffix[64];
        std::snprintf(tmp_suffix, sizeof(tmp_suffix), ".dxil.%lu.%lu.tmp",
                      static_cast<unsigned long>(GetCurrentProcessId()),
                      static_cast<unsigned long>(GetCurrentThreadId()));
        const std::filesystem::path tmp = root / (hex + tmp_suffix);

        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return false;
            f.write(reinterpret_cast<const char*>(bytecode.data()), static_cast<std::streamsize>(bytecode.size()));
            f.flush();
            if (!f.good()) {
                f.close();
                std::filesystem::remove(tmp, ec);
                return false;
            }
        }
        if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::filesystem::remove(tmp, ec);
            return false;
        }
        wsx::trace_hr("d3d12_xs", "ShaderPipeline::CacheSave", S_OK,
                      "stage=%u hash=%s bytes=%zu", info.stage, hex.c_str(), bytecode.size());
        return true;
    } catch (...) {
        return false;
    }
}

// Inspects a D3D12_SHADER_BYTECODE before submitting to desktop D3D12:
// - Records shader-kind telemetry.
// - For non-desktop Xbox shaders (FSSM, ACCC, @xbox.* DXIL):
//     1) Tries disk pre-translated DXIL cache (<WSX_SHADER_CACHE_DIR>/<hash>.dxil|.cso).
//     2) Tries registered runtime translator callback (D3D12XboxRegisterShaderTranslator).
//     3) If still untranslated and WSX_STRICT_SHADER_CHECK=1, returns E_NOTIMPL so we fail
//        gracefully before triggering a vendor driver crash (e.g. nvwgf2umx.dll+0x4AE373).
inline HRESULT prepare_shader_for_desktop_d3d12(
    const D3D12_SHADER_BYTECODE& in_bc,
    D3D12_SHADER_BYTECODE& out_bc,
    std::vector<uint8_t>& owned_storage) noexcept {
    out_bc = in_bc;
    if (!in_bc.pShaderBytecode || in_bc.BytecodeLength < 32u) {
        return S_OK;
    }

    ShaderInfo info{};
    std::vector<uint8_t> fssm_buf;
    if (FAILED(inspect_shader_bytecode(in_bc.pShaderBytecode, in_bc.BytecodeLength, &info, &fssm_buf))) {
        return S_OK;
    }

    auto& st = shader_stats();
    st.inspected.fetch_add(1, std::memory_order_relaxed);
    const auto kind = static_cast<ShaderKind>(info.kind);
    switch (kind) {
    case ShaderKind::XboxDxilPure:
        st.pure_dxil.fetch_add(1, std::memory_order_relaxed);
        return S_OK; // Runs natively on desktop D3D12
    case ShaderKind::XboxDxilIntrinsic:
        st.xbox_intrinsics.fetch_add(1, std::memory_order_relaxed);
        break;
    case ShaderKind::XboxFssmRdna2:
        st.fssm_rdna2.fetch_add(1, std::memory_order_relaxed);
        break;
    case ShaderKind::XboxAcccRdna2:
        st.accc_rdna2.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        return S_OK;
    }

    // 1. Check on-disk pre-translated DXIL shader cache
    if (load_cached_dxil_by_hash(info, owned_storage)) {
        st.cache_hits.fetch_add(1, std::memory_order_relaxed);
        out_bc.pShaderBytecode = owned_storage.data();
        out_bc.BytecodeLength = owned_storage.size();
        wsx::trace_hr("d3d12_xs", "ShaderPipeline::CacheHit", S_OK,
                      "kind=%u stage=%u hash=%s bytes=%zu",
                      info.kind, info.stage, hash16_to_hex(info.shader_hash).c_str(), owned_storage.size());
        return S_OK;
    }

    // 2. Try registered runtime translator callback
    TranslateCallback cb = nullptr;
    void* ctx = nullptr;
    BuiltinFssmTranslatorFn builtin_fn = nullptr;
    {
        auto& reg = translator_registry();
        std::lock_guard<std::mutex> lock(reg.mtx);
        cb = reg.callback;
        ctx = reg.user_ctx;
        builtin_fn = reg.builtin_fssm;
    }
    if (cb) {
        uint32_t target_fmt = 0u;
        size_t req_size = 0u;
        const HRESULT qhr = cb(&info, in_bc.pShaderBytecode, in_bc.BytecodeLength,
                               fssm_buf.empty() ? nullptr : fssm_buf.data(), fssm_buf.size(),
                               &target_fmt, nullptr, 0, &req_size, ctx);
        if (SUCCEEDED(qhr) && req_size > 0 && target_fmt == static_cast<uint32_t>(TargetFormat::Dxil)) {
            owned_storage.resize(req_size);
            size_t written = 0u;
            const HRESULT thr = cb(&info, in_bc.pShaderBytecode, in_bc.BytecodeLength,
                                   fssm_buf.empty() ? nullptr : fssm_buf.data(), fssm_buf.size(),
                                   &target_fmt, owned_storage.data(), owned_storage.size(), &written, ctx);
            if (SUCCEEDED(thr) && written > 0 && target_fmt == static_cast<uint32_t>(TargetFormat::Dxil)) {
                owned_storage.resize(written);
                (void)save_cached_dxil_by_hash(info, owned_storage);
                st.translator_hits.fetch_add(1, std::memory_order_relaxed);
                out_bc.pShaderBytecode = owned_storage.data();
                out_bc.BytecodeLength = owned_storage.size();
                wsx::trace_hr("d3d12_xs", "ShaderPipeline::TranslatorHit", S_OK,
                              "kind=%u stage=%u bytes=%zu", info.kind, info.stage, written);
                return S_OK;
            }
            owned_storage.clear();
        }
    }

    // 2b. Optional built-in pure Direct3D 12 RDNA2 (FSSM & ACCC/FSPX) -> HLSL SM 6.6 -> signed DXIL translator
    //     Enabled via WSX_AUTO_TRANSLATE_FSSM=1 or [Graphics] auto_translate_fssm = 1 in wsx.ini.
    //     Automatically writes compiled DXIL to WSX_SHADER_CACHE_DIR when configured.
    if (builtin_fn && auto_translate_fssm_enabled()) {
        if (kind == ShaderKind::XboxFssmRdna2 && !fssm_buf.empty()) {
            if (SUCCEEDED(builtin_fn(info, fssm_buf.data(), fssm_buf.size(), owned_storage)) &&
                !owned_storage.empty()) {
                (void)save_cached_dxil_by_hash(info, owned_storage);
                st.translator_hits.fetch_add(1, std::memory_order_relaxed);
                out_bc.pShaderBytecode = owned_storage.data();
                out_bc.BytecodeLength = owned_storage.size();
                return S_OK;
            }
            owned_storage.clear();
        } else if (kind == ShaderKind::XboxAcccRdna2) {
            if (SUCCEEDED(builtin_fn(info, in_bc.pShaderBytecode, in_bc.BytecodeLength, owned_storage)) &&
                !owned_storage.empty()) {
                (void)save_cached_dxil_by_hash(info, owned_storage);
                st.translator_hits.fetch_add(1, std::memory_order_relaxed);
                out_bc.pShaderBytecode = owned_storage.data();
                out_bc.BytecodeLength = owned_storage.size();
                return S_OK;
            }
            owned_storage.clear();
        }
    }

    // 3. Untranslated non-desktop Xbox shader (FSSM/ACCC RDNA2 or @xbox.* DXIL):
    //    Under WSX_STRICT_SHADER_CHECK=1, reject cleanly before handing to desktop GPU driver.
    if (strict_shader_check_enabled()) {
        st.rejected_strict.fetch_add(1, std::memory_order_relaxed);
        return wsx::trace_hr("d3d12_xs", "ShaderPipeline::RejectUntranslatedXboxShader", E_NOTIMPL,
                             "kind=%u stage=%u xboxIntr=%u fssmCode=%u hash=%s",
                             info.kind, info.stage, info.xbox_intrinsic_count,
                             info.fssm_code_size, hash16_to_hex(info.shader_hash).c_str());
    }
    return S_OK;
}

} // namespace wsx_shader
