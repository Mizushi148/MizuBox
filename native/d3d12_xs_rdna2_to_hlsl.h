// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room AMD RDNA2 (GFX10.3 / Xbox Series X|S FSSM) -> HLSL Shader Model 6.6 Translator:
// - CFG-reachable BFS decoder covering all 14 RDNA2 (GFX10.3) instruction encodings with exact
//   32/64/96-bit literal, SDWA, DPP16/DPP8, and MIMG NSA word sizing (cleanly skipping inline
//   s_getpc_b64 constant pools placed after s_endpgm).
// - Extracts Compute Shader metadata ([numthreads(X,Y,Z)], code offset/size) from FSSM headers.
// - Emits pure Direct3D 12 SM 6.6 HLSL built on wsx_sm66::kSm66HlslPreamble:
//     * 40-DWORD Root Constants (user SGPRs s0..s31 + heap_base + code_base + slab/hash metadata)
//     * 64-bit Xbox GPU VA emulation via SM 6.6 ResourceDescriptorHeap[] RWByteAddressBuffer slabs
//     * Full GFX10.3 SOPP/SOPC/SOP1/SOPK/SOP2, VOP1/VOPC/VOP2/VOP3/VOP3P (including v_fma_mix_*
//       and v_pk_*), SDWA, DPP16/DPP8, SMEM, MUBUF/MTBUF, FLAT/GLOBAL/SCRATCH, DS (LDS + swizzle/
//       permute), and MIMG (FNV-1a T#/S# bindless lookup)
//     * Per-lane exec masking (v_cmpx_*, s_*_saveexec, s_*_wrexec, s_cbranch_execz/nz) + structured CFG
// - Feeds directly into D3D12XboxCompileHlslToDxil (dynamic Windows SDK dxcompiler.dll + dxil.dll)
//   for zero-Vulkan desktop Direct3D 12 execution.

#pragma once

#include "d3d12_xs_shader_pipeline.h"
#include "d3d12_xs_sm66_runtime.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <vector>

namespace wsx_rdna2 {

// Must stay layout-identical to D3D12XBOX_RDNA2_TRANSLATE_STATS in include/xbox/d3d12_xs.h
struct TranslateStats {
    uint32_t Stage;
    uint32_t NumThreadsX;
    uint32_t NumThreadsY;
    uint32_t NumThreadsZ;
    uint32_t CodeOffset;
    uint32_t CodeSize;
    uint32_t TotalInstructions;
    uint32_t TranslatedInstructions;
    uint32_t UnsupportedInstructions;
    uint32_t BasicBlockCount;
    uint32_t SgprCount;
    uint32_t VgprCount;
    uint32_t UsesLds;
    uint32_t UsesSm66Slabs;
    uint32_t UsesBindlessTextures;
};

enum class Enc : uint8_t {
    Unknown = 0,
    SOPP,
    SOPC,
    SOP1,
    SOPK,
    SOP2,
    VOP1,
    VOPC,
    VOP2,
    VOP3,
    VOP3P,
    SMEM,
    DS,
    FLAT,
    SCRATCH,
    GLOBAL,
    MUBUF,
    MTBUF,
    MIMG,
    EXP,
    VINTRP,
};

inline const char* enc_name(Enc e) noexcept {
    switch (e) {
    case Enc::SOPP: return "SOPP";
    case Enc::SOPC: return "SOPC";
    case Enc::SOP1: return "SOP1";
    case Enc::SOPK: return "SOPK";
    case Enc::SOP2: return "SOP2";
    case Enc::VOP1: return "VOP1";
    case Enc::VOPC: return "VOPC";
    case Enc::VOP2: return "VOP2";
    case Enc::VOP3: return "VOP3";
    case Enc::VOP3P: return "VOP3P";
    case Enc::SMEM: return "SMEM";
    case Enc::DS: return "DS";
    case Enc::FLAT: return "FLAT";
    case Enc::SCRATCH: return "SCRATCH";
    case Enc::GLOBAL: return "GLOBAL";
    case Enc::MUBUF: return "MUBUF";
    case Enc::MTBUF: return "MTBUF";
    case Enc::MIMG: return "MIMG";
    case Enc::EXP: return "EXP";
    case Enc::VINTRP: return "VINTRP";
    default: return "UNK";
    }
}

struct DecodedInsn {
    uint32_t pc = 0;         // Byte offset from start of RDNA2 code
    uint32_t size_bytes = 4; // 4, 8, 12, ...
    Enc enc = Enc::Unknown;
    uint32_t op = 0;
    uint32_t w0 = 0;
    uint32_t w1 = 0;
    uint32_t lit = 0;
    bool has_lit = false;
};

inline uint32_t read_u32_le(const uint8_t* p) noexcept {
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

inline bool src_has_extra_word_32(uint32_t s) noexcept {
    // 0xFF = 32-bit literal, 0xF9 = SDWA, 0xFA = DPP16, 0xE9/0xEA = DPP8
    return s == 0xFFu || s == 0xF9u || s == 0xFAu || s == 0xE9u || s == 0xEAu;
}

// ============================================================================
// Pass 1: GFX10.3 (RDNA2) Single-Instruction & CFG-Reachable Stream Decoder
// ============================================================================
inline DecodedInsn decode_one_insn(const uint32_t* words, size_t count, size_t i) noexcept {
    const uint32_t w0 = words[i];
    DecodedInsn d{};
    d.pc = static_cast<uint32_t>(i * 4u);
    d.w0 = w0;

    const uint32_t top2 = (w0 >> 30) & 3u;
    const uint32_t top4 = (w0 >> 28) & 0xFu;
    const uint32_t top6 = (w0 >> 26) & 0x3Fu;
    const uint32_t top7 = (w0 >> 25) & 0x7Fu;
    const uint32_t top9 = (w0 >> 23) & 0x1FFu;

    size_t nwords = 1;

    if (top9 == 0b101111111u) { // SOPP (0xBF80..)
        d.enc = Enc::SOPP;
        d.op = (w0 >> 16) & 0x7Fu;
        nwords = 1;
    } else if (top9 == 0b101111110u) { // SOPC (0xBF00..)
        d.enc = Enc::SOPC;
        d.op = (w0 >> 16) & 0x7Fu;
        const uint32_t s0 = w0 & 0xFFu;
        const uint32_t s1 = (w0 >> 8) & 0xFFu;
        if (s0 == 0xFFu || s1 == 0xFFu) {
            nwords = 2;
            d.has_lit = true;
            if (i + 1 < count) d.lit = words[i + 1];
        }
    } else if (top9 == 0b101111101u) { // SOP1 (0xBE80..)
        d.enc = Enc::SOP1;
        d.op = (w0 >> 8) & 0xFFu;
        const uint32_t s0 = w0 & 0xFFu;
        if (s0 == 0xFFu) {
            nwords = 2;
            d.has_lit = true;
            if (i + 1 < count) d.lit = words[i + 1];
        }
    } else if (top4 == 0xBu) { // SOPK (0xB0..0xBD)
        d.enc = Enc::SOPK;
        d.op = (w0 >> 23) & 0x1Fu;
        if (d.op == 0x15u) { // s_setreg_imm32_b32
            nwords = 2;
            d.has_lit = true;
            if (i + 1 < count) d.lit = words[i + 1];
        }
    } else if (top2 == 2u) { // SOP2 (0x80..0xAF)
        d.enc = Enc::SOP2;
        d.op = (w0 >> 23) & 0x7Fu;
        const uint32_t s0 = w0 & 0xFFu;
        const uint32_t s1 = (w0 >> 8) & 0xFFu;
        if (s0 == 0xFFu || s1 == 0xFFu) {
            nwords = 2;
            d.has_lit = true;
            if (i + 1 < count) d.lit = words[i + 1];
        }
    } else if (top7 == 0x3Fu) { // VOP1 (0x7E..0x7F)
        d.enc = Enc::VOP1;
        d.op = (w0 >> 9) & 0xFFu;
        const uint32_t s0 = w0 & 0x1FFu;
        if (src_has_extra_word_32(s0)) {
            nwords = 2;
            d.has_lit = (s0 == 0xFFu);
            if (i + 1 < count) d.lit = words[i + 1];
        }
    } else if (top7 == 0x3Eu) { // VOPC (0x7C..0x7D)
        d.enc = Enc::VOPC;
        d.op = (w0 >> 17) & 0xFFu;
        const uint32_t s0 = w0 & 0x1FFu;
        if (src_has_extra_word_32(s0)) {
            nwords = 2;
            d.has_lit = (s0 == 0xFFu);
            if (i + 1 < count) d.lit = words[i + 1];
        }
    } else if ((w0 & 0x80000000u) == 0u) { // VOP2
        d.enc = Enc::VOP2;
        d.op = (w0 >> 25) & 0x3Fu;
        const uint32_t s0 = w0 & 0x1FFu;
        // GFX10.3 VOP2 mandatory 32-bit literal opcodes:
        // 0x20=v_madmk_f32, 0x21=v_madak_f32, 0x2C=v_fmamk_f32, 0x2D=v_fmaak_f32,
        // 0x37=v_fmamk_f16, 0x38=v_fmaak_f16
        const bool mand_lit = (d.op == 0x20u || d.op == 0x21u ||
                               d.op == 0x2Cu || d.op == 0x2Du ||
                               d.op == 0x37u || d.op == 0x38u);
        if (src_has_extra_word_32(s0) || mand_lit) {
            nwords = 2;
            d.has_lit = (s0 == 0xFFu || mand_lit);
            if (i + 1 < count) d.lit = words[i + 1];
        }
    } else if (top6 == 0x35u) { // VOP3 (0xD4..0xD7)
        d.enc = Enc::VOP3;
        d.op = (w0 >> 16) & 0x3FFu;
        nwords = 2;
        if (i + 1 < count) {
            d.w1 = words[i + 1];
            const uint32_t s0 = d.w1 & 0x1FFu;
            const uint32_t s1 = (d.w1 >> 9) & 0x1FFu;
            const uint32_t s2 = (d.w1 >> 18) & 0x1FFu;
            // VOP3Interp packs {high[8], attrchan[7:6], attr[5:0]} in w1[8:0], NOT a src0 operand.
            const bool is_vop3_interp =
                (d.op == 0x200u || d.op == 0x201u || d.op == 0x202u ||
                 d.op == 0x342u || d.op == 0x343u || d.op == 0x35Au);
            if ((!is_vop3_interp && s0 == 0xFFu) || s1 == 0xFFu || s2 == 0xFFu) {
                nwords = 3;
                d.has_lit = true;
                if (i + 2 < count) d.lit = words[i + 2];
            }
        }
    } else if (top6 == 0x33u) { // VOP3P (0xCC..0xCF)
        d.enc = Enc::VOP3P;
        d.op = (w0 >> 16) & 0x7Fu;
        nwords = 2;
        if (i + 1 < count) {
            d.w1 = words[i + 1];
            const uint32_t s0 = d.w1 & 0x1FFu;
            const uint32_t s1 = (d.w1 >> 9) & 0x1FFu;
            const uint32_t s2 = (d.w1 >> 18) & 0x1FFu;
            if (s0 == 0xFFu || s1 == 0xFFu || s2 == 0xFFu) {
                nwords = 3;
                d.has_lit = true;
                if (i + 2 < count) d.lit = words[i + 2];
            }
        }
    } else if (top6 == 0x3Du) { // SMEM (0xF4..0xF7)
        d.enc = Enc::SMEM;
        d.op = (w0 >> 18) & 0xFFu;
        nwords = 2;
        if (i + 1 < count) d.w1 = words[i + 1];
    } else if (top6 == 0x36u) { // DS (0xD8..0xDB)
        d.enc = Enc::DS;
        d.op = (w0 >> 18) & 0xFFu;
        nwords = 2;
        if (i + 1 < count) d.w1 = words[i + 1];
    } else if (top6 == 0x37u) { // FLAT / SCRATCH / GLOBAL (0xDC..0xDF)
        const uint32_t seg = (w0 >> 14) & 3u;
        d.enc = (seg == 2u) ? Enc::GLOBAL : ((seg == 1u) ? Enc::SCRATCH : Enc::FLAT);
        d.op = (w0 >> 18) & 0x7Fu;
        nwords = 2;
        if (i + 1 < count) d.w1 = words[i + 1];
    } else if (top6 == 0x38u) { // MUBUF (0xE0..0xE3)
        d.enc = Enc::MUBUF;
        d.op = (w0 >> 18) & 0xFFu;
        nwords = 2;
        if (i + 1 < count) d.w1 = words[i + 1];
    } else if (top6 == 0x3Au) { // MTBUF (0xE8..0xEB)
        d.enc = Enc::MTBUF;
        d.op = (w0 >> 15) & 0xFu;
        nwords = 2;
        if (i + 1 < count) d.w1 = words[i + 1];
    } else if (top6 == 0x3Cu) { // MIMG (0xF0..0xF3)
        d.enc = Enc::MIMG;
        d.op = (w0 >> 18) & 0xFFu;
        const uint32_t nsize = (w0 >> 1) & 3u;
        nwords = 2u + nsize;
        if (i + 1 < count) d.w1 = words[i + 1];
    } else if (top6 == 0x3Eu) { // EXP (0xF8..0xFB)
        d.enc = Enc::EXP;
        nwords = 2;
        if (i + 1 < count) d.w1 = words[i + 1];
    } else if (top6 == 0x32u) { // VINTRP (0xC8..0xCB)
        d.enc = Enc::VINTRP;
        d.op = (w0 >> 16) & 3u;
        nwords = 1;
    } else {
        d.enc = Enc::Unknown;
        nwords = 1;
    }

    if (i + nwords > count) nwords = count - i;
    d.size_bytes = static_cast<uint32_t>(nwords * 4u);
    return d;
}

inline std::vector<DecodedInsn> decode_rdna2_stream(const uint32_t* words, size_t count) {
    if (!words || count == 0) return {};

    // CFG-reachable BFS from word index 0 cleanly skips unreachable inline s_getpc_b64
    // constant tables placed between s_endpgm and the trailing s_code_end padding.
    std::vector<uint8_t> visited(count, 0u);
    std::queue<size_t> worklist;
    worklist.push(0u);

    std::vector<DecodedInsn> out;
    out.reserve(count);

    while (!worklist.empty()) {
        size_t i = worklist.front();
        worklist.pop();
        while (i < count && !visited[i]) {
            visited[i] = 1u;
            const DecodedInsn d = decode_one_insn(words, count, i);
            out.push_back(d);
            const size_t next_i = i + (d.size_bytes / 4u);

            if (d.enc == Enc::SOPP) {
                if (d.op == 0x01u || d.op == 0x1Bu || d.op == 0x1Eu || d.op == 0x1Fu) {
                    // s_endpgm / s_endpgm_saved / s_endpgm_ordered_ps_done / s_code_end
                    break;
                }
                if (d.op == 0x02u) { // unconditional s_branch
                    const int16_t simm = static_cast<int16_t>(d.w0 & 0xFFFFu);
                    const int64_t tgt = static_cast<int64_t>(i) + 1 + static_cast<int64_t>(simm);
                    if (tgt >= 0 && static_cast<size_t>(tgt) < count && !visited[static_cast<size_t>(tgt)]) {
                        worklist.push(static_cast<size_t>(tgt));
                    }
                    break;
                }
                if ((d.op >= 0x04u && d.op <= 0x09u) || (d.op >= 0x17u && d.op <= 0x1Au)) {
                    // conditional s_cbranch_*
                    const int16_t simm = static_cast<int16_t>(d.w0 & 0xFFFFu);
                    const int64_t tgt = static_cast<int64_t>(i) + 1 + static_cast<int64_t>(simm);
                    if (tgt >= 0 && static_cast<size_t>(tgt) < count && !visited[static_cast<size_t>(tgt)]) {
                        worklist.push(static_cast<size_t>(tgt));
                    }
                }
            } else if (d.enc == Enc::SOPK && (d.op == 0x16u || d.op == 0x1Bu || d.op == 0x1Cu)) {
                // s_call_b64 / s_subvector_loop_begin / s_subvector_loop_end
                const int16_t simm = static_cast<int16_t>(d.w0 & 0xFFFFu);
                const int64_t tgt = static_cast<int64_t>(i) + 1 + static_cast<int64_t>(simm);
                if (tgt >= 0 && static_cast<size_t>(tgt) < count && !visited[static_cast<size_t>(tgt)]) {
                    worklist.push(static_cast<size_t>(tgt));
                }
            }
            i = next_i;
        }
    }

    std::sort(out.begin(), out.end(), [](const DecodedInsn& a, const DecodedInsn& b) {
        return a.pc < b.pc;
    });
    return out;
}

enum class CfgLoweringMode : uint32_t {
    StraightLine = 0u,
    Relooper     = 1u,
    TopoDag      = 2u,
    StateMachine = 3u
};

inline const char* cfg_mode_name(CfgLoweringMode m) noexcept {
    switch (m) {
    case CfgLoweringMode::StraightLine: return "StraightLine";
    case CfgLoweringMode::Relooper:     return "Relooper";
    case CfgLoweringMode::TopoDag:      return "TopoDag";
    case CfgLoweringMode::StateMachine: return "StateMachine";
    default:                            return "Unknown";
    }
}

enum class Wave64EmulationMode : uint8_t {
    Disabled = 0,
    Emulate = 1,
    Native64 = 2,
};

inline const char* wave64_mode_name(Wave64EmulationMode m) noexcept {
    switch (m) {
    case Wave64EmulationMode::Disabled: return "disabled";
    case Wave64EmulationMode::Emulate:  return "emulate_gs";
    case Wave64EmulationMode::Native64: return "native64";
    default:                            return "unknown";
    }
}

// ============================================================================
// Pass 2: HLSL SM 6.6 Emitter (with Relooper CFG, Exec-Guard Coalescing & Trimmed Preamble)
// ============================================================================
class HlslEmitter {
    std::string body_;
    std::vector<std::string>* line_sink_ = nullptr;
    std::set<uint32_t> sgprs_used_;
    std::set<uint32_t> vgprs_used_;
    std::set<uint32_t> ps_attrs_used_;
    std::set<uint32_t> ps_mrts_exported_;
    std::set<uint32_t> vs_params_exported_;
    bool ps_depth_exported_ = false;
    bool is_wave64_ = false;
    Wave64EmulationMode w64_mode_ = Wave64EmulationMode::Disabled;
    bool w64_force_mode_ = false;
    bool wave64_emulated_ = false;
    bool uses_lds_ = false;
    bool uses_slabs_ = false;
    bool uses_textures_ = false;
    bool uses_scratch_ = false;
    bool uses_vrel_ = false;
    uint32_t stage_ = 5u;
    uint32_t translated_count_ = 0;
    uint32_t unsupported_count_ = 0;
    uint32_t fssm_code_off_ = 0;
    CfgLoweringMode cfg_mode_ = CfgLoweringMode::StateMachine;
    bool exec_elided_ = false;
    uint32_t coalesced_guards_ = 0;
    uint32_t dead_scc_elided_ = 0;
    uint32_t dead_reg_elided_ = 0;
    uint32_t multi_exit_loops_structured_ = 0;
    uint32_t peephole_folds_ = 0;
    uint32_t branchless_selects_ = 0;
    uint32_t auto_lds_barriers_ = 0;
    uint32_t quad_grad_samples_ = 0;
    enum class LdsHazardState : uint8_t { None = 0, Read = 1, Write = 2 };
    LdsHazardState lds_hazard_ = LdsHazardState::None;
    bool auto_lds_barrier_enabled_ = true;
    bool clamp_finite_image_stores_ = true;
    bool ms_saw_alloc_req_ = false;
    bool as_saw_dispatch_req_ = false;
    bool use_native_fp16_ = false;
    std::vector<DecodedInsn>* unimpl_sink_ = nullptr;

public:
    bool is_wave64() const noexcept { return is_wave64_; }
    Wave64EmulationMode wave64_mode() const noexcept { return w64_mode_; }
    bool wave64_emulated() const noexcept { return wave64_emulated_; }
    CfgLoweringMode cfg_mode() const noexcept { return cfg_mode_; }
    bool exec_elided() const noexcept { return exec_elided_; }
    uint32_t coalesced_guards() const noexcept { return coalesced_guards_; }
    uint32_t dead_scc_elided() const noexcept { return dead_scc_elided_; }
    uint32_t dead_reg_elided() const noexcept { return dead_reg_elided_; }
    uint32_t multi_exit_loops_structured() const noexcept { return multi_exit_loops_structured_; }
    uint32_t peephole_folds() const noexcept { return peephole_folds_; }
    uint32_t branchless_selects() const noexcept { return branchless_selects_; }
    uint32_t auto_lds_barriers() const noexcept { return auto_lds_barriers_; }
    uint32_t quad_grad_samples() const noexcept { return quad_grad_samples_; }
    bool ms_dynamic_alloc_used() const noexcept { return ms_saw_alloc_req_; }
    bool native_fp16_used() const noexcept { return use_native_fp16_; }
    const std::set<uint32_t>& ps_attrs_used() const noexcept { return ps_attrs_used_; }
    const std::set<uint32_t>& ps_mrts_exported() const noexcept { return ps_mrts_exported_; }
    const std::set<uint32_t>& vs_params_exported() const noexcept { return vs_params_exported_; }
    bool ps_depth_exported() const noexcept { return ps_depth_exported_; }

private:

    bool stage_has_workgroup() const noexcept {
        return stage_ == 5u || stage_ == 12u || stage_ == 13u || stage_ == 0xFFFFu;
    }

    // Automatic Wave32-safe LDS RAW/WAR synchronization:
    // AMD RDNA2 Wave64 shaders rely on lockstep LDS access across lanes 0..63 without s_barrier.
    // On Wave32 GPUs (e.g. NVIDIA), lanes 0..31 and 32..63 run in separate warps and race unless synchronized.
    void mark_lds_access(bool is_write, bool is_read) {
        if (!stage_has_workgroup() || !auto_lds_barrier_enabled_) return;
        const bool raw_hazard = is_read && (lds_hazard_ == LdsHazardState::Write);
        const bool war_hazard = is_write && (lds_hazard_ == LdsHazardState::Read);
        if (raw_hazard || war_hazard) {
            line("GroupMemoryBarrierWithGroupSync();");
            ++auto_lds_barriers_;
            lds_hazard_ = LdsHazardState::None;
        }
        if (is_write) {
            lds_hazard_ = LdsHazardState::Write;
        } else if (is_read) {
            lds_hazard_ = LdsHazardState::Read;
        }
    }

    bool use_w64_emu() const noexcept {
        return w64_mode_ == Wave64EmulationMode::Emulate;
    }

    std::string expr_lane_id() const {
        return use_w64_emu() ? "wsx_w64_lane(wsx_gi)" : "WaveGetLaneIndex()";
    }

    std::string expr_lane_count() const {
        return use_w64_emu() ? "64u" : "WaveGetLaneCount()";
    }

    std::string expr_is_first_lane() const {
        return use_w64_emu() ? "wsx_w64_is_first(wsx_gi)" : "WaveIsFirstLane()";
    }

    std::string expr_mask_bit(const std::string& lo, const std::string& hi) const {
        return use_w64_emu()
            ? ("wsx_w64_mask_bit(" + lo + ", " + hi + ", wsx_gi)")
            : ("wsx_mask_bit(" + lo + ", " + hi + ")");
    }

    std::string expr_readlane(const std::string& val, const std::string& lane_expr) const {
        return use_w64_emu()
            ? ("wsx_w64_readlane(" + val + ", " + lane_expr + ", wsx_gi)")
            : ("WaveReadLaneAt(" + val + ", (" + lane_expr + ") & (WaveGetLaneCount() - 1u))");
    }

    std::string expr_readfirstlane(const std::string& val) const {
        return use_w64_emu()
            ? ("wsx_w64_readfirstlane(" + val + ", lane_active, wsx_gi)")
            : ("WaveReadLaneFirst(" + val + ")");
    }

    void emit_ballot64_assign(const std::string& pred, const std::string& dst_lo, const std::string& dst_hi) {
        if (use_w64_emu()) {
            line("{ uint2 _wb = wsx_w64_ballot(" + pred + ", wsx_gi); " + dst_lo + " = _wb.x; " + dst_hi + " = _wb.y; }");
        } else {
            line("{ uint4 _wb = WaveActiveBallot(" + pred + "); " + dst_lo + " = _wb.x; " + dst_hi + " = _wb.y; }");
        }
    }

    uint32_t lds_word_count() const noexcept {
        if (stage_ == 13u) return 2048u;         // 8 KB in MS (stays comfortably within 28 KB TGSM+payload limit)
        if (stage_has_workgroup()) {
            // Reserve headroom for Wave64 groupshared ballot/exchange scratch inside the 32 KB D3D12 CS TGSM cap.
            return use_w64_emu() ? 4096u : 8192u;
        }
        return 64u;                              // Thread-local fallback in VS / PS / GS (e.g. merged ES-GS)
    }

    uint32_t lds_word_mask() const noexcept {
        return lds_word_count() - 1u;
    }

    std::string ps_attr_expr(uint32_t attr, uint32_t chan) {
        if (stage_ != 0u) return "0.0f";
        const uint32_t slot = attr % 31u; // SV_Position + TEXCOORD0..30 = 32 max D3D12 input registers
        ps_attrs_used_.insert(slot);
        static constexpr const char* kCh[4] = {".x", ".y", ".z", ".w"};
        return "wsx_in.p" + std::to_string(slot) + kCh[chan & 3u];
    }

    std::string sreg(uint32_t idx) {
        if (idx < 106u) {
            sgprs_used_.insert(idx);
            return "s" + std::to_string(idx);
        }
        if (idx == 106u) return "vcc_lo";
        if (idx == 107u) return "vcc_hi";
        if (idx == 124u) return "m0";
        if (idx == 126u) return "exec_lo";
        if (idx == 127u) return "exec_hi";
        return "wsx_sink";
    }

    std::string sreg_hi(uint32_t idx) {
        if (idx < 105u) return sreg(idx + 1u);
        if (idx == 106u) return "vcc_hi";
        if (idx == 126u) return "exec_hi";
        return "wsx_sink";
    }

    std::string vreg(uint32_t idx) {
        idx &= 0xFFu;
        vgprs_used_.insert(idx);
        return "v" + std::to_string(idx);
    }

    std::string operand_u32(uint32_t code, uint32_t lit) {
        if (code < 106u) return sreg(code);
        if (code == 106u) return "vcc_lo";
        if (code == 107u) return "vcc_hi";
        if (code == 124u) return "m0";
        if (code == 125u) return "0u";
        if (code == 126u) return "exec_lo";
        if (code == 127u) return "exec_hi";
        if (code >= 128u && code <= 192u) return std::to_string(code - 128u) + "u";
        if (code >= 193u && code <= 208u) {
            const int32_t neg = -static_cast<int32_t>(code - 192u);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%08Xu", static_cast<uint32_t>(neg));
            return buf;
        }
        if (code == 240u) return "0x3F000000u"; // 0.5f
        if (code == 241u) return "0xBF000000u"; // -0.5f
        if (code == 242u) return "0x3F800000u"; // 1.0f
        if (code == 243u) return "0xBF800000u"; // -1.0f
        if (code == 244u) return "0x40000000u"; // 2.0f
        if (code == 245u) return "0xC0000000u"; // -2.0f
        if (code == 246u) return "0x40800000u"; // 4.0f
        if (code == 247u) return "0xC0800000u"; // -4.0f
        if (code == 248u) return "0x3E22F983u"; // 1/(2*pi)
        if (code == 251u) return "((vcc_lo | vcc_hi) == 0u ? 1u : 0u)";
        if (code == 252u) return "((exec_lo | exec_hi) == 0u ? 1u : 0u)";
        if (code == 253u) return "(scc ? 1u : 0u)";
        if (code == 255u) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%08Xu", lit);
            return buf;
        }
        if (code >= 256u && code < 512u) return vreg(code - 256u);
        return "0u";
    }

    std::string operand_u32_hi(uint32_t code, uint32_t lit) {
        if (code < 105u) return sreg(code + 1u);
        if (code == 106u) return "vcc_hi";
        if (code == 126u) return "exec_hi";
        if (code >= 256u && code < 511u) return vreg((code - 256u) + 1u);
        if (code >= 193u && code <= 208u) return "0xFFFFFFFFu";
        if (code == 240u) return "0x3FE00000u"; // 0.5
        if (code == 241u) return "0xBFE00000u"; // -0.5
        if (code == 242u) return "0x3FF00000u"; // 1.0
        if (code == 243u) return "0xBFF00000u"; // -1.0
        if (code == 244u) return "0x40000000u"; // 2.0
        if (code == 245u) return "0xC0000000u"; // -2.0
        if (code == 246u) return "0x40100000u"; // 4.0
        if (code == 247u) return "0xC0100000u"; // -4.0
        if (code == 255u) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%08Xu", lit);
            return buf;
        }
        return "0u";
    }

    std::string operand_u64(uint32_t code, uint32_t lit) {
        return "((uint64_t(" + operand_u32_hi(code, lit) + ") << 32) | uint64_t(" + operand_u32(code, lit) + "))";
    }

    std::string operand_f32(uint32_t code, uint32_t lit, bool abs_mod = false, bool neg_mod = false) {
        std::string e = "asfloat(" + operand_u32(code, lit) + ")";
        if (abs_mod) e = "abs(" + e + ")";
        if (neg_mod) e = "(-(" + e + "))";
        return e;
    }

    std::string operand_f64(uint32_t code, uint32_t lit, bool abs_mod = false, bool neg_mod = false) {
        std::string e = "asdouble(" + operand_u32(code, lit) + ", " + operand_u32_hi(code, lit) + ")";
        if (abs_mod) e = "abs(" + e + ")";
        if (neg_mod) e = "(-(" + e + "))";
        return e;
    }

    static std::string wrap_f32(const std::string& uexpr, bool abs_mod = false, bool neg_mod = false) {
        std::string e = "asfloat(" + uexpr + ")";
        if (abs_mod) e = "abs(" + e + ")";
        if (neg_mod) e = "(-(" + e + "))";
        return e;
    }

    std::string u64_from_s(uint32_t base) {
        return "((uint64_t(" + sreg_hi(base) + ") << 32) | uint64_t(" + sreg(base) + "))";
    }

    std::string vsharp_va_from_s(uint32_t base) {
        // RDNA2 V# buffer descriptor: 48-bit GPU VA lives in s[base] + (s[base+1] & 0xFFFF)
        return "((uint64_t(" + sreg(base + 1u) + " & 0xFFFFu) << 32) | uint64_t(" + sreg(base) + "))";
    }

    std::string vsharp_stride_from_s(uint32_t base) {
        // RDNA2 V# word 1 bits [29:16] = stride in bytes
        return "((" + sreg(base + 1u) + " >> 16u) & 0x3FFFu)";
    }

    void line(const std::string& s) {
        if (line_sink_) {
            line_sink_->push_back(s);
            return;
        }
        body_ += "            ";
        body_ += s;
        body_ += "\n";
    }

    // Optional SDWA destination modifier state active for the current VOP1/VOP2 instruction
    struct SdwaDstState {
        bool active = false;
        uint32_t dst_sel = 6u;
        uint32_t dst_unused = 2u;
    };
    SdwaDstState sdwa_dst_{};

    void emit_v_assign(uint32_t vd, const std::string& expr) {
        if (sdwa_dst_.active && sdwa_dst_.dst_sel != 6u) {
            line("if (lane_active) { " + vreg(vd) + " = wsx_sdwa_dst(" + vreg(vd) + ", " + expr + ", " +
                 std::to_string(sdwa_dst_.dst_sel) + "u, " + std::to_string(sdwa_dst_.dst_unused) + "u); }");
        } else {
            line("if (lane_active) { " + vreg(vd) + " = " + expr + "; }");
        }
    }

    void emit_v_assign_f32(uint32_t vd, const std::string& fexpr, bool clamp_mod = false, uint32_t omod = 0u) {
        std::string e = fexpr;
        if (omod == 1u) e = "(2.0f * (" + e + "))";
        else if (omod == 2u) e = "(4.0f * (" + e + "))";
        else if (omod == 3u) e = "(0.5f * (" + e + "))";
        if (clamp_mod) e = "saturate(" + e + ")";
        emit_v_assign(vd, "asuint(" + e + ")");
    }

    void emit_v_assign_f16_lo(uint32_t vd, const std::string& fexpr, bool clamp_mod = false) {
        std::string e = clamp_mod ? ("saturate(" + fexpr + ")") : fexpr;
        emit_v_assign(vd, "((" + vreg(vd) + " & 0xFFFF0000u) | (f32tof16(" + e + ") & 0xFFFFu))");
    }

    void emit_v_assign_f64(uint32_t vd, const std::string& dexpr) {
        line("if (lane_active) { uint _dlo = 0u, _dhi = 0u; asuint(double(" + dexpr + "), _dlo, _dhi); " +
             vreg(vd) + " = _dlo; " + vreg(vd + 1u) + " = _dhi; }");
    }

    // Resolve VOP1/VOP2/VOPC src0/src1 accounting for SDWA (0xF9), DPP16 (0xFA), and DPP8 (0xE9/0xEA)
    void resolve_vop_operands(
        uint32_t s0_code, uint32_t vs1, uint32_t lit, bool has_s1,
        std::string& out_s0u, std::string& out_s1u,
        std::string& out_s0f, std::string& out_s1f,
        bool& out_clamp, uint32_t& out_omod) {
        out_clamp = false;
        out_omod = 0u;
        sdwa_dst_ = {};

        if (s0_code == 0xF9u) { // SDWA
            const uint32_t src0_idx = lit & 0xFFu;
            const uint32_t s0_is_sgpr = (lit >> 23) & 1u;
            const uint32_t s1_is_sgpr = (lit >> 31) & 1u;
            const uint32_t s0_sel = (lit >> 16) & 7u;
            const uint32_t s0_sext = (lit >> 19) & 1u;
            const bool s0_neg = ((lit >> 20) & 1u) != 0u;
            const bool s0_abs = ((lit >> 21) & 1u) != 0u;
            const uint32_t s1_sel = (lit >> 24) & 7u;
            const uint32_t s1_sext = (lit >> 27) & 1u;
            const bool s1_neg = ((lit >> 28) & 1u) != 0u;
            const bool s1_abs = ((lit >> 29) & 1u) != 0u;

            sdwa_dst_.active = true;
            sdwa_dst_.dst_sel = (lit >> 8) & 7u;
            sdwa_dst_.dst_unused = (lit >> 11) & 3u;
            out_clamp = ((lit >> 13) & 1u) != 0u;
            out_omod = (lit >> 14) & 3u;

            const std::string raw0 = s0_is_sgpr ? operand_u32(src0_idx, 0u) : vreg(src0_idx);
            out_s0u = (s0_sel == 6u) ? raw0
                : ("wsx_sdwa_src(" + raw0 + ", " + std::to_string(s0_sel) + "u, " + std::to_string(s0_sext) + "u)");
            out_s0f = wrap_f32(out_s0u, s0_abs, s0_neg);

            if (has_s1) {
                const std::string raw1 = s1_is_sgpr ? operand_u32(vs1, 0u) : vreg(vs1);
                out_s1u = (s1_sel == 6u) ? raw1
                    : ("wsx_sdwa_src(" + raw1 + ", " + std::to_string(s1_sel) + "u, " + std::to_string(s1_sext) + "u)");
                out_s1f = wrap_f32(out_s1u, s1_abs, s1_neg);
            }
        } else if (s0_code == 0xFAu) { // DPP16
            const uint32_t src0_v = lit & 0xFFu;
            const uint32_t ctrl = (lit >> 8) & 0x1FFu;
            const bool s0_neg = ((lit >> 20) & 1u) != 0u;
            const bool s0_abs = ((lit >> 21) & 1u) != 0u;
            const bool s1_neg = ((lit >> 22) & 1u) != 0u;
            const bool s1_abs = ((lit >> 23) & 1u) != 0u;
            out_s0u = "wsx_dpp16(" + vreg(src0_v) + ", " + std::to_string(ctrl) + "u)";
            out_s0f = wrap_f32(out_s0u, s0_abs, s0_neg);
            if (has_s1) {
                out_s1u = vreg(vs1);
                out_s1f = wrap_f32(out_s1u, s1_abs, s1_neg);
            }
        } else if (s0_code == 0xE9u || s0_code == 0xEAu) { // DPP8
            const uint32_t src0_v = lit & 0xFFu;
            const uint32_t sel_word = (lit >> 8) & 0xFFFFFFu;
            out_s0u = "wsx_dpp8(" + vreg(src0_v) + ", " + std::to_string(sel_word) + "u)";
            out_s0f = "asfloat(" + out_s0u + ")";
            if (has_s1) {
                out_s1u = vreg(vs1);
                out_s1f = "asfloat(" + out_s1u + ")";
            }
        } else {
            out_s0u = operand_u32(s0_code, lit);
            out_s0f = operand_f32(s0_code, lit);
            if (has_s1) {
                out_s1u = vreg(vs1);
                out_s1f = "asfloat(" + out_s1u + ")";
            }
        }
    }

    void emit_vopc_op(uint32_t op, const std::string& a_u, const std::string& b_u,
                      const std::string& a_f, const std::string& b_f,
                      bool write_exec, uint32_t sdst_override = 106u) {
        const uint32_t family = op & 0xF0u;
        const uint32_t cmp = op & 0x0Fu;
        std::string cond = "false";

        auto int_cmp = [&](bool is_signed, const std::string& la, const std::string& rb, uint32_t c) -> std::string {
            const std::string lhs = is_signed ? ("int(" + la + ")") : la;
            const std::string rhs = is_signed ? ("int(" + rb + ")") : rb;
            switch (c) {
            case 0u: return "false";
            case 1u: return "(" + lhs + " < " + rhs + ")";
            case 2u: return "(" + lhs + " == " + rhs + ")";
            case 3u: return "(" + lhs + " <= " + rhs + ")";
            case 4u: return "(" + lhs + " > " + rhs + ")";
            case 5u: return "(" + lhs + " != " + rhs + ")";
            case 6u: return "(" + lhs + " >= " + rhs + ")";
            case 7u: return "true";
            default: return "false";
            }
        };

        auto flt_cmp = [&](const std::string& la, const std::string& rb, uint32_t c) -> std::string {
            switch (c) {
            case 0u:  return "false";
            case 1u:  return "(" + la + " < " + rb + ")";
            case 2u:  return "(" + la + " == " + rb + ")";
            case 3u:  return "(" + la + " <= " + rb + ")";
            case 4u:  return "(" + la + " > " + rb + ")";
            case 5u:  return "((" + la + " < " + rb + ") || (" + la + " > " + rb + "))"; // LG (ordered !=; false on NaN)
            case 6u:  return "(" + la + " >= " + rb + ")";
            case 7u:  return "(!isnan(" + la + ") && !isnan(" + rb + "))"; // O (ordered)
            case 8u:  return "(isnan(" + la + ") || isnan(" + rb + "))";   // U (unordered)
            case 9u:  return "(!(" + la + " >= " + rb + "))";              // NGE (unordered <)
            case 10u: return "(!((" + la + " < " + rb + ") || (" + la + " > " + rb + ")))"; // NLG (unordered ==; true on NaN)
            case 11u: return "(!(" + la + " > " + rb + "))";               // NGT (unordered <=)
            case 12u: return "(!(" + la + " <= " + rb + "))";              // NLE (unordered >)
            case 13u: return "(!(" + la + " == " + rb + "))";              // NEQ (unordered !=; true on NaN)
            case 14u: return "(!(" + la + " < " + rb + "))";               // NLT (unordered >=)
            case 15u: return "true";
            default:  return "false";
            }
        };

        // GFX10.3 VOPC:
        // 0x00..0x3F: F32/F64 (cmpx in 0x10..0x1F, 0x30..0x3F)
        // 0x80..0x9F: I32/I16 (0x88/98=CLASS_F32, 0x8F/9F=CLASS_F16), 0xA0..0xBF: I64/U16 (0xA8/B8=CLASS_F64)
        // 0xC0..0xDF: U32 (0..7) / F16 (8..F), 0xE0..0xFF: U64 (0..7) / F16 (8..F)
        if (family <= 0x30u) {
            cond = flt_cmp(a_f, b_f, cmp);
        } else if (op == 0x88u || op == 0x98u || op == 0xA8u || op == 0xB8u) {
            cond = "wsx_fp_class_f32(" + a_f + ", " + b_u + ")";
        } else if (op == 0x8Fu || op == 0x9Fu) {
            cond = "wsx_fp_class_f32(f16tof32(" + a_u + "), " + b_u + ")";
        } else if ((family == 0xC0u || family == 0xD0u) && cmp >= 8u) {
            cond = flt_cmp("f16tof32(" + a_u + ")", "f16tof32(" + b_u + ")", cmp - 8u);
        } else if ((family == 0xE0u || family == 0xF0u) && cmp >= 8u) {
            cond = flt_cmp("f16tof32(" + a_u + ")", "f16tof32(" + b_u + ")", cmp);
        } else if (family == 0x80u || family == 0x90u) {
            if (cmp < 8u) cond = int_cmp(true, a_u, b_u, cmp);
            else cond = int_cmp(true, "uint((int(" + a_u + ") << 16) >> 16)", "uint((int(" + b_u + ") << 16) >> 16)", cmp - 8u);
        } else if (family == 0xA0u || family == 0xB0u) {
            if (cmp < 8u) cond = int_cmp(true, a_u, b_u, cmp);
            else cond = int_cmp(false, "(" + a_u + " & 0xFFFFu)", "(" + b_u + " & 0xFFFFu)", cmp - 8u);
        } else if (family == 0xC0u || family == 0xD0u || family == 0xE0u || family == 0xF0u) {
            cond = int_cmp(false, a_u, b_u, cmp & 7u);
        }

        if (write_exec) {
            if (use_w64_emu()) {
                line("{ lane_active = lane_active && (" + cond + "); uint2 _wb = wsx_w64_ballot(lane_active, wsx_gi); exec_lo = _wb.x; exec_hi = _wb.y; }");
            } else {
                line("{ lane_active = lane_active && (" + cond + "); uint4 _wb = WaveActiveBallot(lane_active); exec_lo = _wb.x; exec_hi = _wb.y; }");
            }
        } else {
            line("vcc_lane = lane_active && (" + cond + ");");
            if (sdst_override == 106u) {
                emit_ballot64_assign("vcc_lane", "vcc_lo", "vcc_hi");
            } else if (is_wave64_) {
                emit_ballot64_assign("vcc_lane", sreg(sdst_override), sreg_hi(sdst_override));
            } else {
                line(sreg(sdst_override) + " = WaveActiveBallot(vcc_lane).x;");
            }
        }
    }

    // GFX10.3 (RDNA2) exact VOP1 opcode table
    bool emit_vop1_core(uint32_t op, uint32_t vd, const std::string& su, const std::string& sf,
                        uint32_t s0_code = 0u, uint32_t lit = 0u,
                        bool clamp_mod = false, uint32_t omod = 0u) {
        switch (op) {
        case 0x00u: return true; // v_nop
        case 0x01u: emit_v_assign(vd, su); return true; // v_mov_b32
        case 0x03u: emit_v_assign(vd, "uint(int(" + operand_f64(s0_code, lit) + "))"); return true; // v_cvt_i32_f64
        case 0x04u: emit_v_assign_f64(vd, "double(int(" + su + "))"); return true; // v_cvt_f64_i32
        case 0x05u: emit_v_assign_f32(vd, "float(int(" + su + "))", clamp_mod, omod); return true; // v_cvt_f32_i32
        case 0x06u: emit_v_assign_f32(vd, "float(" + su + ")", clamp_mod, omod); return true; // v_cvt_f32_u32
        case 0x07u: emit_v_assign(vd, "uint(max(" + sf + ", 0.0f))"); return true; // v_cvt_u32_f32
        case 0x08u: emit_v_assign(vd, "uint(int(" + sf + "))"); return true; // v_cvt_i32_f32
        case 0x0Au: emit_v_assign(vd, "f32tof16(" + sf + ")"); return true; // v_cvt_f16_f32
        case 0x0Bu: emit_v_assign_f32(vd, "f16tof32(" + su + ")", clamp_mod, omod); return true; // v_cvt_f32_f16
        case 0x0Cu: emit_v_assign(vd, "uint(int(floor(" + sf + " + 0.5f)))"); return true; // v_cvt_rpi_i32_f32
        case 0x0Du: emit_v_assign(vd, "uint(int(floor(" + sf + ")))"); return true; // v_cvt_flr_i32_f32
        case 0x0Eu: emit_v_assign_f32(vd, "(float((int(" + su + ") << 28) >> 28) * 0.0625f)", clamp_mod, omod); return true; // v_cvt_off_f32_i4
        case 0x0Fu: emit_v_assign_f32(vd, "float(" + operand_f64(s0_code, lit) + ")", clamp_mod, omod); return true; // v_cvt_f32_f64
        case 0x10u: emit_v_assign_f64(vd, "double(" + sf + ")"); return true; // v_cvt_f64_f32
        case 0x11u: emit_v_assign_f32(vd, "float((" + su + ") & 0xFFu)", clamp_mod, omod); return true; // v_cvt_f32_ubyte0
        case 0x12u: emit_v_assign_f32(vd, "float(((" + su + ") >> 8u) & 0xFFu)", clamp_mod, omod); return true; // v_cvt_f32_ubyte1
        case 0x13u: emit_v_assign_f32(vd, "float(((" + su + ") >> 16u) & 0xFFu)", clamp_mod, omod); return true; // v_cvt_f32_ubyte2
        case 0x14u: emit_v_assign_f32(vd, "float(((" + su + ") >> 24u) & 0xFFu)", clamp_mod, omod); return true; // v_cvt_f32_ubyte3
        case 0x15u: emit_v_assign(vd, "uint(max(" + operand_f64(s0_code, lit) + ", 0.0))"); return true; // v_cvt_u32_f64
        case 0x16u: emit_v_assign_f64(vd, "double(" + su + ")"); return true; // v_cvt_f64_u32
        case 0x17u: emit_v_assign_f64(vd, "double(trunc(float(" + operand_f64(s0_code, lit) + ")))"); return true; // v_trunc_f64
        case 0x18u: emit_v_assign_f64(vd, "double(ceil(float(" + operand_f64(s0_code, lit) + ")))"); return true; // v_ceil_f64
        case 0x19u: emit_v_assign_f64(vd, "double(round(float(" + operand_f64(s0_code, lit) + ")))"); return true; // v_rndne_f64
        case 0x1Au: emit_v_assign_f64(vd, "double(floor(float(" + operand_f64(s0_code, lit) + ")))"); return true; // v_floor_f64
        case 0x1Bu: return true; // v_pipeflush
        case 0x20u: emit_v_assign_f32(vd, "frac(" + sf + ")", clamp_mod, omod); return true; // v_fract_f32
        case 0x21u: emit_v_assign_f32(vd, "trunc(" + sf + ")", clamp_mod, omod); return true; // v_trunc_f32
        case 0x22u: emit_v_assign_f32(vd, "ceil(" + sf + ")", clamp_mod, omod); return true; // v_ceil_f32
        case 0x23u: emit_v_assign_f32(vd, "round(" + sf + ")", clamp_mod, omod); return true; // v_rndne_f32
        case 0x24u: emit_v_assign_f32(vd, "floor(" + sf + ")", clamp_mod, omod); return true; // v_floor_f32
        case 0x25u: emit_v_assign_f32(vd, "exp2(" + sf + ")", clamp_mod, omod); return true; // v_exp_f32
        case 0x27u: emit_v_assign_f32(vd, "log2(" + sf + ")", clamp_mod, omod); return true; // v_log_f32
        case 0x2Au:
        case 0x2Bu: emit_v_assign_f32(vd, "rcp(" + sf + ")", clamp_mod, omod); return true; // v_rcp_f32 / v_rcp_iflag_f32
        case 0x2Eu: emit_v_assign_f32(vd, "rsqrt(" + sf + ")", clamp_mod, omod); return true; // v_rsq_f32
        case 0x2Fu: emit_v_assign_f64(vd, "(1.0 / " + operand_f64(s0_code, lit) + ")"); return true; // v_rcp_f64
        case 0x31u: emit_v_assign_f64(vd, "double(rsqrt(float(" + operand_f64(s0_code, lit) + ")))"); return true; // v_rsq_f64
        case 0x33u: emit_v_assign_f32(vd, "sqrt(" + sf + ")", clamp_mod, omod); return true; // v_sqrt_f32
        case 0x34u: emit_v_assign_f64(vd, "double(sqrt(float(" + operand_f64(s0_code, lit) + ")))"); return true; // v_sqrt_f64
        case 0x35u: emit_v_assign_f32(vd, "sin(6.2831853f * (" + sf + "))", clamp_mod, omod); return true; // v_sin_f32
        case 0x36u: emit_v_assign_f32(vd, "cos(6.2831853f * (" + sf + "))", clamp_mod, omod); return true; // v_cos_f32
        case 0x37u: emit_v_assign(vd, "(~(" + su + "))"); return true; // v_not_b32
        case 0x38u: emit_v_assign(vd, "reversebits(" + su + ")"); return true; // v_bfrev_b32
        case 0x39u: emit_v_assign(vd, "((" + su + ") == 0u ? 0xFFFFFFFFu : (31u - firstbithigh(" + su + ")))"); return true; // v_ffbh_u32
        case 0x3Au: emit_v_assign(vd, "firstbitlow(" + su + ")"); return true; // v_ffbl_b32
        case 0x3Bu: emit_v_assign(vd, "firstbithigh(int(" + su + "))"); return true; // v_ffbh_i32
        case 0x3Cu: emit_v_assign(vd, "wsx_frexp_exp_f32(float(" + operand_f64(s0_code, lit) + "))"); return true; // v_frexp_exp_i32_f64
        case 0x3Du: emit_v_assign_f64(vd, "double(wsx_frexp_mant_f32(float(" + operand_f64(s0_code, lit) + ")))"); return true; // v_frexp_mant_f64
        case 0x3Eu: emit_v_assign_f64(vd, "double(frac(float(" + operand_f64(s0_code, lit) + ")))"); return true; // v_fract_f64
        case 0x3Fu: emit_v_assign(vd, "wsx_frexp_exp_f32(" + sf + ")"); return true; // v_frexp_exp_i32_f32
        case 0x40u: emit_v_assign_f32(vd, "wsx_frexp_mant_f32(" + sf + ")", clamp_mod, omod); return true; // v_frexp_mant_f32
        case 0x41u: return true; // v_clrexcp
        case 0x42u: // v_movreld_b32: v[vd + (m0 & 0xFF)] = su
            uses_vrel_ = true;
            (void)vreg(vd);
            line("if (lane_active) { WSX_VREL_WRITE(" + std::to_string(vd) + "u + (m0 & 255u), " + su + "); }");
            return true;
        case 0x43u: { // v_movrels_b32: vd = v[s0 + (m0 & 0xFF)]
            uses_vrel_ = true;
            const uint32_t sbase = (s0_code >= 256u) ? (s0_code - 256u) : 0u;
            (void)vreg(sbase);
            line("if (lane_active) { uint _vr = 0u; WSX_VREL_READ(_vr, " + std::to_string(sbase) + "u + (m0 & 255u)); " + vreg(vd) + " = _vr; }");
            return true;
        }
        case 0x44u: { // v_movrelsd_b32: v[vd + m0] = v[s0 + m0]
            uses_vrel_ = true;
            const uint32_t sbase = (s0_code >= 256u) ? (s0_code - 256u) : 0u;
            (void)vreg(vd); (void)vreg(sbase);
            line("if (lane_active) { uint _vr = 0u; WSX_VREL_READ(_vr, " + std::to_string(sbase) + "u + (m0 & 255u)); WSX_VREL_WRITE(" + std::to_string(vd) + "u + (m0 & 255u), _vr); }");
            return true;
        }
        case 0x48u: { // v_movrelsd_2_b32: v[vd + (m0 & 0xFF)] = v[s0 + ((m0 >> 16) & 0xFF)]
            uses_vrel_ = true;
            const uint32_t sbase = (s0_code >= 256u) ? (s0_code - 256u) : 0u;
            (void)vreg(vd); (void)vreg(sbase);
            line("if (lane_active) { uint _vr = 0u; WSX_VREL_READ(_vr, " + std::to_string(sbase) + "u + ((m0 >> 16u) & 255u)); WSX_VREL_WRITE(" + std::to_string(vd) + "u + (m0 & 255u), _vr); }");
            return true;
        }
        case 0x1Cu: emit_v_assign_f32(vd, "(float((int(" + su + ") << 28) >> 28) * 0.0625f)", clamp_mod, omod); return true; // v_cvt_off_f32_i4
        case 0x26u: emit_v_assign_f32(vd, "exp2(" + sf + ")", clamp_mod, omod); return true; // v_exp_legacy_f32
        case 0x28u: emit_v_assign_f32(vd, "log2(" + sf + ")", clamp_mod, omod); return true; // v_log_legacy_f32
        case 0x2Cu: emit_v_assign_f32(vd, "rcp(" + sf + ")", clamp_mod, omod); return true; // v_rcp_iflag_f32
        case 0x2Du: emit_v_assign_f32(vd, "clamp(rsqrt(" + sf + "), -3.402823466e38f, 3.402823466e38f)", clamp_mod, omod); return true; // v_rsq_clamp_f32
        case 0x45u: { // v_swap_b32
            if (s0_code >= 256u) {
                const uint32_t sv = s0_code - 256u;
                line("if (lane_active) { uint _sw = " + vreg(vd) + "; " + vreg(vd) + " = " + vreg(sv) + "; " + vreg(sv) + " = _sw; }");
            } else {
                emit_v_assign(vd, su);
            }
            return true;
        }
        case 0x46u: { // v_swaprel_b32
            uses_vrel_ = true;
            const uint32_t sv = (s0_code >= 256u) ? (s0_code - 256u) : 0u;
            (void)vreg(vd); (void)vreg(sv);
            line("if (lane_active) { uint _vr = 0u; WSX_VREL_READ(_vr, " + std::to_string(sv) + "u + (m0 & 255u)); uint _vd = " + vreg(vd) + "; " + vreg(vd) + " = _vr; WSX_VREL_WRITE(" + std::to_string(sv) + "u + (m0 & 255u), _vd); }");
            return true;
        }
        case 0x4Bu: return true; // v_pipeflush
        case 0x50u: emit_v_assign_f16_lo(vd, "float((" + su + ") & 0xFFFFu)", clamp_mod); return true; // v_cvt_f16_u16
        case 0x51u: emit_v_assign_f16_lo(vd, "float((int(" + su + ") << 16) >> 16)", clamp_mod); return true; // v_cvt_f16_i16
        case 0x52u: emit_v_assign(vd, "((" + vreg(vd) + " & 0xFFFF0000u) | (uint(clamp(f16tof32(" + su + "), 0.0f, 65535.0f)) & 0xFFFFu))"); return true; // v_cvt_u16_f16
        case 0x53u: emit_v_assign(vd, "((" + vreg(vd) + " & 0xFFFF0000u) | (uint(int(clamp(f16tof32(" + su + "), -32768.0f, 32767.0f))) & 0xFFFFu))"); return true; // v_cvt_i16_f16
        case 0x54u: emit_v_assign_f16_lo(vd, "rcp(f16tof32(" + su + "))", clamp_mod); return true; // v_rcp_f16
        case 0x55u: emit_v_assign_f16_lo(vd, "sqrt(f16tof32(" + su + "))", clamp_mod); return true; // v_sqrt_f16
        case 0x56u: emit_v_assign_f16_lo(vd, "rsqrt(f16tof32(" + su + "))", clamp_mod); return true; // v_rsq_f16
        case 0x57u: emit_v_assign_f16_lo(vd, "log2(f16tof32(" + su + "))", clamp_mod); return true; // v_log_f16
        case 0x58u: emit_v_assign_f16_lo(vd, "exp2(f16tof32(" + su + "))", clamp_mod); return true; // v_exp_f16
        case 0x59u: emit_v_assign_f16_lo(vd, "wsx_frexp_mant_f32(f16tof32(" + su + "))", clamp_mod); return true; // v_frexp_mant_f16
        case 0x5Au: emit_v_assign(vd, "((" + vreg(vd) + " & 0xFFFF0000u) | (wsx_frexp_exp_f32(f16tof32(" + su + ")) & 0xFFFFu))"); return true; // v_frexp_exp_i16_f16
        case 0x5Bu: emit_v_assign_f16_lo(vd, "floor(f16tof32(" + su + "))", clamp_mod); return true; // v_floor_f16
        case 0x5Cu: emit_v_assign_f16_lo(vd, "ceil(f16tof32(" + su + "))", clamp_mod); return true; // v_ceil_f16
        case 0x5Du: emit_v_assign_f16_lo(vd, "trunc(f16tof32(" + su + "))", clamp_mod); return true; // v_trunc_f16
        case 0x5Eu: emit_v_assign_f16_lo(vd, "round(f16tof32(" + su + "))", clamp_mod); return true; // v_rndne_f16
        case 0x5Fu: emit_v_assign_f16_lo(vd, "frac(f16tof32(" + su + "))", clamp_mod); return true; // v_fract_f16
        case 0x60u: emit_v_assign_f16_lo(vd, "sin(f16tof32(" + su + ") * 6.2831853f)", clamp_mod); return true; // v_sin_f16
        case 0x61u: emit_v_assign_f16_lo(vd, "cos(f16tof32(" + su + ") * 6.2831853f)", clamp_mod); return true; // v_cos_f16
        case 0x62u: emit_v_assign(vd, "(uint(clamp((int(" + su + ") << 16) >> 16, 0, 255)) | (uint(clamp(int(" + su + ") >> 16, 0, 255)) << 8u))"); return true; // v_sat_pk_u8_i16
        case 0x63u: emit_v_assign(vd, "((" + vreg(vd) + " & 0xFFFF0000u) | (uint(int(round(clamp(f16tof32(" + su + "), -1.0f, 1.0f) * 32767.0f))) & 0xFFFFu))"); return true; // v_cvt_norm_i16_f16
        case 0x64u: emit_v_assign(vd, "((" + vreg(vd) + " & 0xFFFF0000u) | (uint(round(saturate(f16tof32(" + su + ")) * 65535.0f)) & 0xFFFFu))"); return true; // v_cvt_norm_u16_f16
        default: return false;
        }
    }

    // GFX10.3 (RDNA2) exact VOP2 opcode table
    bool emit_vop2_core(uint32_t op, uint32_t vd, const std::string& s0u, const std::string& s1u,
                        const std::string& s0f, const std::string& s1f, uint32_t lit,
                        bool clamp_mod = false, uint32_t omod = 0u) {
        char lit_hex[32];
        std::snprintf(lit_hex, sizeof(lit_hex), "0x%08Xu", lit);
        const std::string lhex(lit_hex);
        switch (op) {
        case 0x01u: emit_v_assign(vd, "(vcc_lane ? (" + s1u + ") : (" + s0u + "))"); return true; // v_cndmask_b32
        case 0x02u: // v_dot2c_f32_f16
            emit_v_assign_f32(vd, "(f16tof32(" + s0u + ") * f16tof32(" + s1u + ") + f16tof32((" + s0u + ") >> 16u) * f16tof32((" + s1u + ") >> 16u) + asfloat(" + vreg(vd) + "))", clamp_mod, omod);
            return true;
        case 0x03u: emit_v_assign_f32(vd, "(" + s0f + " + " + s1f + ")", clamp_mod, omod); return true; // v_add_f32
        case 0x04u: emit_v_assign_f32(vd, "(" + s0f + " - " + s1f + ")", clamp_mod, omod); return true; // v_sub_f32
        case 0x05u: emit_v_assign_f32(vd, "(" + s1f + " - " + s0f + ")", clamp_mod, omod); return true; // v_subrev_f32
        case 0x06u: // v_fmac_legacy_f32 / v_mac_legacy_f32
        case 0x1Fu: // v_mac_f32
        case 0x2Bu: emit_v_assign_f32(vd, "mad(" + s0f + ", " + s1f + ", asfloat(" + vreg(vd) + "))", clamp_mod, omod); return true; // v_fmac_f32
        case 0x07u: // v_mul_legacy_f32
        case 0x08u: emit_v_assign_f32(vd, "(" + s0f + " * " + s1f + ")", clamp_mod, omod); return true; // v_mul_f32
        case 0x09u: emit_v_assign(vd, "uint(((int(" + s0u + ") << 8) >> 8) * ((int(" + s1u + ") << 8) >> 8))"); return true; // v_mul_i32_i24
        case 0x0Au: emit_v_assign(vd, "uint((int64_t((int(" + s0u + ") << 8) >> 8) * int64_t((int(" + s1u + ") << 8) >> 8)) >> 32)"); return true; // v_mul_hi_i32_i24
        case 0x0Bu: emit_v_assign(vd, "((" + s0u + " & 0xFFFFFFu) * (" + s1u + " & 0xFFFFFFu))"); return true; // v_mul_u32_u24
        case 0x0Cu: emit_v_assign(vd, "uint((uint64_t(" + s0u + " & 0xFFFFFFu) * uint64_t(" + s1u + " & 0xFFFFFFu)) >> 32)"); return true; // v_mul_hi_u32_u24
        case 0x0Du: // v_dot4c_i32_i8
            line("if (lane_active) { int _acc = int(" + vreg(vd) + "); [unroll] for (uint _k = 0u; _k < 4u; ++_k) { _acc += ((int((" + s0u + ") >> (_k*8u)) << 24) >> 24) * ((int((" + s1u + ") >> (_k*8u)) << 24) >> 24); } " + vreg(vd) + " = uint(_acc); }");
            return true;
        case 0x0Fu: emit_v_assign_f32(vd, "min(" + s0f + ", " + s1f + ")", clamp_mod, omod); return true; // v_min_f32
        case 0x10u: emit_v_assign_f32(vd, "max(" + s0f + ", " + s1f + ")", clamp_mod, omod); return true; // v_max_f32
        case 0x11u: emit_v_assign(vd, "uint(min(int(" + s0u + "), int(" + s1u + ")))"); return true; // v_min_i32
        case 0x12u: emit_v_assign(vd, "uint(max(int(" + s0u + "), int(" + s1u + ")))"); return true; // v_max_i32
        case 0x13u: emit_v_assign(vd, "min(" + s0u + ", " + s1u + ")"); return true; // v_min_u32
        case 0x14u: emit_v_assign(vd, "max(" + s0u + ", " + s1u + ")"); return true; // v_max_u32
        case 0x16u: emit_v_assign(vd, "((" + s1u + ") >> ((" + s0u + ") & 31u))"); return true; // v_lshrrev_b32
        case 0x18u: emit_v_assign(vd, "uint(int(" + s1u + ") >> ((" + s0u + ") & 31u))"); return true; // v_ashrrev_i32
        case 0x1Au: emit_v_assign(vd, "((" + s1u + ") << ((" + s0u + ") & 31u))"); return true; // v_lshlrev_b32
        case 0x1Bu: emit_v_assign(vd, "((" + s0u + ") & (" + s1u + "))"); return true; // v_and_b32
        case 0x1Cu: emit_v_assign(vd, "((" + s0u + ") | (" + s1u + "))"); return true; // v_or_b32
        case 0x1Du: emit_v_assign(vd, "((" + s0u + ") ^ (" + s1u + "))"); return true; // v_xor_b32
        case 0x1Eu: emit_v_assign(vd, "(~((" + s0u + ") ^ (" + s1u + ")))"); return true; // v_xnor_b32
        case 0x20u: // v_madmk_f32
        case 0x2Cu: emit_v_assign_f32(vd, "mad(" + s0f + ", asfloat(" + lhex + "), " + s1f + ")", clamp_mod, omod); return true; // v_fmamk_f32
        case 0x21u: // v_madak_f32
        case 0x2Du: emit_v_assign_f32(vd, "mad(" + s0f + ", " + s1f + ", asfloat(" + lhex + "))", clamp_mod, omod); return true; // v_fmaak_f32
        case 0x25u: emit_v_assign(vd, "((" + s0u + ") + (" + s1u + "))"); return true; // v_add_nc_u32
        case 0x26u: emit_v_assign(vd, "((" + s0u + ") - (" + s1u + "))"); return true; // v_sub_nc_u32
        case 0x27u: emit_v_assign(vd, "((" + s1u + ") - (" + s0u + "))"); return true; // v_subrev_nc_u32
        case 0x28u: // v_add_co_ci_u32 (VOP2: carry in/out via vcc_lane)
            line("if (lane_active) { uint _a = " + s0u + "; uint _b = " + s1u + "; uint _c = vcc_lane ? 1u : 0u; uint _r = _a + _b + _c; vcc_lane = (_r < _a) || (_c != 0u && _r == _a); " + vreg(vd) + " = _r; }");
            emit_ballot64_assign("vcc_lane", "vcc_lo", "vcc_hi");
            return true;
        case 0x29u: // v_sub_co_ci_u32
            line("if (lane_active) { uint _a = " + s0u + "; uint _b = " + s1u + "; uint _c = vcc_lane ? 1u : 0u; uint _r = _a - _b - _c; vcc_lane = (_b > _a) || (_c != 0u && _b == _a); " + vreg(vd) + " = _r; }");
            emit_ballot64_assign("vcc_lane", "vcc_lo", "vcc_hi");
            return true;
        case 0x2Au: // v_subrev_co_ci_u32
            line("if (lane_active) { uint _a = " + s1u + "; uint _b = " + s0u + "; uint _c = vcc_lane ? 1u : 0u; uint _r = _a - _b - _c; vcc_lane = (_b > _a) || (_c != 0u && _b == _a); " + vreg(vd) + " = _r; }");
            emit_ballot64_assign("vcc_lane", "vcc_lo", "vcc_hi");
            return true;
        case 0x2Fu: emit_v_assign(vd, "(f32tof16(" + s0f + ") | (f32tof16(" + s1f + ") << 16u))"); return true; // v_cvt_pkrtz_f16_f32
        case 0x32u: emit_v_assign_f16_lo(vd, "(f16tof32(" + s0u + ") + f16tof32(" + s1u + "))", clamp_mod); return true; // v_add_f16
        case 0x33u: emit_v_assign_f16_lo(vd, "(f16tof32(" + s0u + ") - f16tof32(" + s1u + "))", clamp_mod); return true; // v_sub_f16
        case 0x34u: emit_v_assign_f16_lo(vd, "(f16tof32(" + s1u + ") - f16tof32(" + s0u + "))", clamp_mod); return true; // v_subrev_f16
        case 0x35u: emit_v_assign_f16_lo(vd, "(f16tof32(" + s0u + ") * f16tof32(" + s1u + "))", clamp_mod); return true; // v_mul_f16
        case 0x36u: emit_v_assign_f16_lo(vd, "mad(f16tof32(" + s0u + "), f16tof32(" + s1u + "), f16tof32(" + vreg(vd) + "))", clamp_mod); return true; // v_fmac_f16
        case 0x37u: emit_v_assign_f16_lo(vd, "mad(f16tof32(" + s0u + "), f16tof32(" + lhex + "), f16tof32(" + s1u + "))", clamp_mod); return true; // v_fmamk_f16
        case 0x38u: emit_v_assign_f16_lo(vd, "mad(f16tof32(" + s0u + "), f16tof32(" + s1u + "), f16tof32(" + lhex + "))", clamp_mod); return true; // v_fmaak_f16
        case 0x39u: emit_v_assign_f16_lo(vd, "max(f16tof32(" + s0u + "), f16tof32(" + s1u + "))", clamp_mod); return true; // v_max_f16
        case 0x3Au: emit_v_assign_f16_lo(vd, "min(f16tof32(" + s0u + "), f16tof32(" + s1u + "))", clamp_mod); return true; // v_min_f16
        case 0x3Bu: emit_v_assign_f16_lo(vd, "ldexp(f16tof32(" + s0u + "), float((int(" + s1u + ") << 16) >> 16))", clamp_mod); return true; // v_ldexp_f16
        case 0x3Cu: // v_pk_fmac_f16
            line("if (lane_active) { float _lo = mad(f16tof32(" + s0u + "), f16tof32(" + s1u + "), f16tof32(" + vreg(vd) + ")); float _hi = mad(f16tof32((" + s0u + ") >> 16u), f16tof32((" + s1u + ") >> 16u), f16tof32((" + vreg(vd) + ") >> 16u)); " + vreg(vd) + " = (f32tof16(_lo) & 0xFFFFu) | ((f32tof16(_hi) & 0xFFFFu) << 16u); }");
            return true;
        default: return false;
        }
    }

    // Shared 32-bit atomic emitter for MUBUF / GLOBAL / FLAT (exact IEEE-754 FP32 & csub CAS)
    bool emit_va_atomic32(uint32_t op, const std::string& va_expr, uint32_t vdata, uint32_t vdst, bool glc) {
        int kind = -1;
        switch (op) {
        case 0x30u: kind = 0; break;  // swap
        case 0x31u: kind = 1; break;  // cmpswap
        case 0x32u: kind = 2; break;  // add
        case 0x33u: kind = 3; break;  // sub
        case 0x34u: kind = 17; break; // csub (clamped unsigned sub)
        case 0x35u: kind = 4; break;  // smin
        case 0x36u: kind = 5; break;  // umin
        case 0x37u: kind = 6; break;  // smax
        case 0x38u: kind = 7; break;  // umax
        case 0x39u: kind = 8; break;  // and
        case 0x3Au: kind = 9; break;  // or
        case 0x3Bu: kind = 10; break; // xor
        case 0x3Cu: kind = 11; break; // inc
        case 0x3Du: kind = 12; break; // dec
        case 0x3Eu: case 0x41u: kind = 13; break; // fcmpswap
        case 0x3Fu: case 0x42u: kind = 14; break; // fmin
        case 0x40u: case 0x43u: kind = 15; break; // fmax
        case 0x4Du: kind = 16; break;             // fadd (global_atomic_add_f32)
        default: return false;
        }
        uses_slabs_ = true;
        const std::string cmp_arg = (kind == 1 || kind == 13) ? vreg(vdata + 1u) : "0u";
        line("if (lane_active) { uint _orig = wsx_va_atomic_op32(" + va_expr + ", " +
             std::to_string(kind) + "u, " + vreg(vdata) + ", " + cmp_arg + ");" +
             (glc ? (" " + vreg(vdst) + " = _orig;") : "") + " }");
        return true;
    }

    // Shared 64-bit atomic emitter for MUBUF / GLOBAL / FLAT (SM 6.6 native Interlocked*64)
    bool emit_va_atomic64(uint32_t op, const std::string& va_expr, uint32_t vdata, uint32_t vdst, bool glc) {
        int kind = -1;
        switch (op) {
        case 0x50u: kind = 0; break;  // swap_x2
        case 0x51u: kind = 1; break;  // cmpswap_x2
        case 0x52u: kind = 2; break;  // add_x2
        case 0x53u: kind = 3; break;  // sub_x2
        case 0x55u: kind = 4; break;  // smin_x2
        case 0x56u: kind = 5; break;  // umin_x2
        case 0x57u: kind = 6; break;  // smax_x2
        case 0x58u: kind = 7; break;  // umax_x2
        case 0x59u: kind = 8; break;  // and_x2
        case 0x5Au: kind = 9; break;  // or_x2
        case 0x5Bu: kind = 10; break; // xor_x2
        case 0x5Cu: kind = 11; break; // inc_x2
        case 0x5Du: kind = 12; break; // dec_x2
        case 0x5Eu: kind = 13; break; // fcmpswap_x2
        case 0x5Fu: kind = 14; break; // fmin_x2
        case 0x60u: kind = 15; break; // fmax_x2
        default: return false;
        }
        uses_slabs_ = true;
        const std::string val64 = "((uint64_t(" + vreg(vdata + 1u) + ") << 32) | uint64_t(" + vreg(vdata) + "))";
        const std::string cmp64 = (kind == 1 || kind == 13)
            ? ("((uint64_t(" + vreg(vdata + 3u) + ") << 32) | uint64_t(" + vreg(vdata + 2u) + "))")
            : "0ull";
        line("if (lane_active) { uint64_t _o64 = wsx_va_atomic_op64(" + va_expr + ", " +
             std::to_string(kind) + "u, " + val64 + ", " + cmp64 + ");" +
             (glc ? (" " + vreg(vdst) + " = uint(_o64 & 0xFFFFFFFFull); " + vreg(vdst + 1u) + " = uint(_o64 >> 32);") : "") + " }");
        return true;
    }

    void emit_insn(const DecodedInsn& d, const std::map<uint32_t, uint32_t>& pc_to_bb) {
        auto resolve_branch_bb = [&](int16_t simm16) -> uint32_t {
            const int64_t target = static_cast<int64_t>(d.pc) + 4 + static_cast<int64_t>(simm16) * 4;
            auto it = pc_to_bb.find(static_cast<uint32_t>(target));
            return (it != pc_to_bb.end()) ? it->second : 0xFFFFFFFFu;
        };

        sdwa_dst_ = {};
        bool ok = false;

        switch (d.enc) {
        case Enc::SOPP: {
            const int16_t simm16 = static_cast<int16_t>(d.w0 & 0xFFFFu);
            switch (d.op) {
            case 0x00u: // s_nop
            case 0x03u: // s_wakeup
            case 0x0Cu: // s_waitcnt
            case 0x0Du: // s_sethalt
            case 0x0Eu: // s_sleep
            case 0x0Fu: // s_setprio
            case 0x10u: // s_sendmsg
            case 0x11u: { // s_sendmsghalt
                const uint32_t msg_id = static_cast<uint32_t>(simm16) & 0xFu;
                if (stage_ == 13u && msg_id == 10u) {
                    // GFX10.3 NGG / Mesh Shader MSG_GS_ALLOC_REQ: m0[10:0] = vert_cnt, m0[22:12] = prim_cnt
                    ms_saw_alloc_req_ = true;
                    line("if (wsx_gi == 0u) { _wsx_ms_counts[0] = (m0 & 0x7FFu); _wsx_ms_counts[1] = ((m0 >> 12u) & 0x7FFu); }");
                } else if (stage_ == 12u && msg_id == 10u) {
                    // Amplification Shader dynamic DispatchMesh counts packed in m0
                    as_saw_dispatch_req_ = true;
                    line("if (wsx_gi == 0u) { _wsx_as_dims[0] = max(1u, min(m0 & 0x3FFu, 64u)); _wsx_as_dims[1] = max(1u, min((m0 >> 10u) & 0x3FFu, 64u)); _wsx_as_dims[2] = max(1u, min((m0 >> 20u) & 0x3FFu, 64u)); }");
                }
                ok = true;
                break;
            }
            case 0x12u: // s_trap
            case 0x13u: // s_icache_inv
            case 0x14u: // s_incperflevel
            case 0x15u: // s_decperflevel
            case 0x16u: // s_ttracedata
            case 0x1Fu: // s_code_end
            case 0x20u: // s_inst_prefetch
            case 0x21u: // s_clause
            case 0x22u: // s_wait_idle
            case 0x23u: // s_waitcnt_depctr
            case 0x24u: // s_round_mode
            case 0x25u: // s_denorm_mode
            case 0x26u: // s_waitcnt_vscnt (alias)
            case 0x28u: // s_ttracedata_imm
                ok = true;
                break;
            case 0x01u: // s_endpgm
            case 0x1Bu: // s_endpgm_saved
            case 0x1Eu: // s_endpgm_ordered_ps_done
                line("wsx_pc = 0xFFFFFFFFu; break;");
                ok = true;
                break;
            case 0x02u: // s_branch
                line("wsx_pc = " + std::to_string(resolve_branch_bb(simm16)) + "u; break;");
                ok = true;
                break;
            case 0x04u: // s_cbranch_scc0
                line("if (!scc) { wsx_pc = " + std::to_string(resolve_branch_bb(simm16)) + "u; break; }");
                ok = true;
                break;
            case 0x05u: // s_cbranch_scc1
                line("if (scc) { wsx_pc = " + std::to_string(resolve_branch_bb(simm16)) + "u; break; }");
                ok = true;
                break;
            case 0x06u: // s_cbranch_vccz
                line("if ((vcc_lo | vcc_hi) == 0u) { wsx_pc = " + std::to_string(resolve_branch_bb(simm16)) + "u; break; }");
                ok = true;
                break;
            case 0x07u: // s_cbranch_vccnz
                line("if ((vcc_lo | vcc_hi) != 0u) { wsx_pc = " + std::to_string(resolve_branch_bb(simm16)) + "u; break; }");
                ok = true;
                break;
            case 0x08u: // s_cbranch_execz
                line("if (WaveActiveAllTrue(!lane_active)) { wsx_pc = " + std::to_string(resolve_branch_bb(simm16)) + "u; break; }");
                ok = true;
                break;
            case 0x09u: // s_cbranch_execnz
                line("if (!WaveActiveAllTrue(!lane_active)) { wsx_pc = " + std::to_string(resolve_branch_bb(simm16)) + "u; break; }");
                ok = true;
                break;
            case 0x0Au: // s_barrier
                if (stage_has_workgroup()) {
                    line("GroupMemoryBarrierWithGroupSync();");
                    lds_hazard_ = LdsHazardState::None;
                } else {
                    line("/* s_barrier (non-workgroup stage) */");
                }
                ok = true;
                break;
            case 0x0Bu: // s_setkill
                line("if ((" + std::to_string(static_cast<uint32_t>(d.w0 & 0xFFFFu)) + "u) != 0u) { lane_active = false; exec_lo = 0u; exec_hi = 0u; }");
                ok = true;
                break;
            case 0x17u:
            case 0x18u:
            case 0x19u:
            case 0x1Au: // s_cbranch_cdbg* (debug-only branches -> never taken)
                ok = true;
                break;
            default:
                break;
            }
            break;
        }
        case Enc::SOP1: {
            const uint32_t sd = (d.w0 >> 16) & 0x7Fu;
            const uint32_t s0 = d.w0 & 0xFFu;
            const std::string a = operand_u32(s0, d.lit);
            const std::string a_hi = operand_u32_hi(s0, d.lit);
            auto sync_exec = [&]() {
                if (sd == 126u || sd == 127u) line("lane_active = " + expr_mask_bit("exec_lo", "exec_hi") + ";");
            };
            auto emit_saveexec = [&](bool is64, const std::string& new_lo, const std::string& new_hi) {
                const std::string mb = expr_mask_bit("exec_lo", "exec_hi");
                if (is64) {
                    line("{ uint _ol = exec_lo; uint _oh = exec_hi; " + sreg(sd) + " = _ol; " + sreg_hi(sd) + " = _oh; exec_lo = " + new_lo + "; exec_hi = " + new_hi + "; lane_active = " + mb + "; scc = ((exec_lo | exec_hi) != 0u); }");
                } else {
                    line("{ uint _ol = exec_lo; " + sreg(sd) + " = _ol; exec_lo = " + new_lo + "; lane_active = " + mb + "; scc = (exec_lo != 0u); }");
                }
            };
            auto emit_wrexec = [&](bool is64, const std::string& new_lo, const std::string& new_hi) {
                const std::string mb = expr_mask_bit("exec_lo", "exec_hi");
                if (is64) {
                    line("{ uint _nl = " + new_lo + "; uint _nh = " + new_hi + "; " + sreg(sd) + " = _nl; " + sreg_hi(sd) + " = _nh; exec_lo = _nl; exec_hi = _nh; lane_active = " + mb + "; scc = ((exec_lo | exec_hi) != 0u); }");
                } else {
                    line("{ uint _nl = " + new_lo + "; " + sreg(sd) + " = _nl; exec_lo = _nl; lane_active = " + mb + "; scc = (exec_lo != 0u); }");
                }
            };
            switch (d.op) {
            case 0x03u: // s_mov_b32
                line(sreg(sd) + " = " + a + ";");
                sync_exec();
                ok = true; break;
            case 0x04u: // s_mov_b64
                line(sreg(sd) + " = " + a + "; " + sreg_hi(sd) + " = " + a_hi + ";");
                sync_exec();
                ok = true; break;
            case 0x05u: // s_cmov_b32
                line("if (scc) { " + sreg(sd) + " = " + a + "; }");
                sync_exec();
                ok = true; break;
            case 0x06u: // s_cmov_b64
                line("if (scc) { " + sreg(sd) + " = " + a + "; " + sreg_hi(sd) + " = " + a_hi + "; }");
                sync_exec();
                ok = true; break;
            case 0x07u: // s_not_b32
                line(sreg(sd) + " = ~(" + a + "); scc = (" + sreg(sd) + " != 0u);");
                sync_exec();
                ok = true; break;
            case 0x08u: // s_not_b64
                line(sreg(sd) + " = ~(" + a + "); " + sreg_hi(sd) + " = ~(" + a_hi + "); scc = ((" + sreg(sd) + " | " + sreg_hi(sd) + ") != 0u);");
                sync_exec();
                ok = true; break;
            case 0x09u: // s_wqm_b32
                line(sreg(sd) + " = wsx_wqm32(" + a + "); scc = (" + sreg(sd) + " != 0u);");
                sync_exec();
                ok = true; break;
            case 0x0Au: // s_wqm_b64
                line(sreg(sd) + " = wsx_wqm32(" + a + "); " + sreg_hi(sd) + " = wsx_wqm32(" + a_hi + "); scc = ((" + sreg(sd) + " | " + sreg_hi(sd) + ") != 0u);");
                sync_exec();
                ok = true; break;
            case 0x0Bu: // s_brev_b32
                line(sreg(sd) + " = reversebits(" + a + ");");
                ok = true; break;
            case 0x0Cu: // s_brev_b64
                line("{ uint _lo = reversebits(" + a_hi + "); uint _hi = reversebits(" + a + "); " + sreg(sd) + " = _lo; " + sreg_hi(sd) + " = _hi; }");
                ok = true; break;
            case 0x0Du: // s_bcnt0_i32_b32
                line(sreg(sd) + " = countbits(~(" + a + ")); scc = (" + sreg(sd) + " != 0u);");
                ok = true; break;
            case 0x0Eu: // s_bcnt0_i32_b64
                line(sreg(sd) + " = countbits(~(" + a + ")) + countbits(~(" + a_hi + ")); scc = (" + sreg(sd) + " != 0u);");
                ok = true; break;
            case 0x0Fu: // s_bcnt1_i32_b32
                line(sreg(sd) + " = countbits(" + a + "); scc = (" + sreg(sd) + " != 0u);");
                ok = true; break;
            case 0x10u: // s_bcnt1_i32_b64
                line(sreg(sd) + " = countbits(" + a + ") + countbits(" + a_hi + "); scc = (" + sreg(sd) + " != 0u);");
                ok = true; break;
            case 0x11u: // s_ff0_i32_b32
                line(sreg(sd) + " = firstbitlow(~(" + a + "));");
                ok = true; break;
            case 0x12u: // s_ff0_i32_b64
                line("{ uint _l = firstbitlow(~(" + a + ")); " + sreg(sd) + " = (_l != 0xFFFFFFFFu) ? _l : ((~(" + a_hi + ") != 0u) ? (32u + firstbitlow(~(" + a_hi + "))) : 0xFFFFFFFFu); }");
                ok = true; break;
            case 0x13u: // s_ff1_i32_b32
                line(sreg(sd) + " = firstbitlow(" + a + ");");
                ok = true; break;
            case 0x14u: // s_ff1_i32_b64
                line("{ uint _l = firstbitlow(" + a + "); " + sreg(sd) + " = (_l != 0xFFFFFFFFu) ? _l : ((" + a_hi + " != 0u) ? (32u + firstbitlow(" + a_hi + ")) : 0xFFFFFFFFu); }");
                ok = true; break;
            case 0x15u: // s_flbit_i32_b32
                line(sreg(sd) + " = ((" + a + ") == 0u ? 0xFFFFFFFFu : (31u - firstbithigh(" + a + ")));");
                ok = true; break;
            case 0x16u: // s_flbit_i32_b64
                line(sreg(sd) + " = ((" + a_hi + ") != 0u ? (31u - firstbithigh(" + a_hi + ")) : ((" + a + ") != 0u ? (63u - firstbithigh(" + a + ")) : 0xFFFFFFFFu));");
                ok = true; break;
            case 0x17u: // s_flbit_i32
            case 0x18u: // s_flbit_i32_i64
                line(sreg(sd) + " = firstbithigh(int(" + a + "));");
                ok = true; break;
            case 0x19u: // s_sext_i32_i8
                line(sreg(sd) + " = uint((int(" + a + ") << 24) >> 24);");
                ok = true; break;
            case 0x1Au: // s_sext_i32_i16
                line(sreg(sd) + " = uint((int(" + a + ") << 16) >> 16);");
                ok = true; break;
            case 0x1Bu: // s_bitset0_b32
                line(sreg(sd) + " &= ~(1u << ((" + a + ") & 31u));");
                ok = true; break;
            case 0x1Cu: // s_bitset0_b64
                line("{ uint _b = (" + a + ") & 63u; if (_b < 32u) " + sreg(sd) + " &= ~(1u << _b); else " + sreg_hi(sd) + " &= ~(1u << (_b - 32u)); }");
                ok = true; break;
            case 0x1Du: // s_bitset1_b32
                line(sreg(sd) + " |= (1u << ((" + a + ") & 31u));");
                ok = true; break;
            case 0x1Eu: // s_bitset1_b64
                line("{ uint _b = (" + a + ") & 63u; if (_b < 32u) " + sreg(sd) + " |= (1u << _b); else " + sreg_hi(sd) + " |= (1u << (_b - 32u)); }");
                ok = true; break;
            case 0x1Fu: { // s_getpc_b64
                uses_slabs_ = true;
                const uint32_t next_off = fssm_code_off_ + d.pc + d.size_bytes;
                line("{ uint64_t _pc = wsx_code_base() + " + std::to_string(next_off) + "ull; " +
                     sreg(sd) + " = uint(_pc & 0xFFFFFFFFull); " + sreg_hi(sd) + " = uint(_pc >> 32); }");
                ok = true; break;
            }
            case 0x20u: // s_setpc_b64 (subroutine return / indirect branch)
            case 0x21u: // s_swappc_b64
            case 0x22u: // s_rfe_b64
            case 0x23u: // s_cbranch_join
            case 0x30u: // s_rfe_restore_b64
            case 0x35u: // s_set_gpr_idx_on
            case 0x36u: // s_set_gpr_idx_off
                ok = true; break;
            case 0x2Eu: // s_set_gpr_idx_idx
                line("m0 = (m0 & ~0xFFu) | ((" + a + ") & 0xFFu);");
                ok = true; break;
            case 0x2Fu: // s_mov_fed_b32
                line(sreg(sd) + " = " + a + ";");
                sync_exec();
                ok = true; break;
            case 0x24u: emit_saveexec(true,  "(_ol & (" + a + "))", "(_oh & (" + a_hi + "))"); ok = true; break; // s_and_saveexec_b64
            case 0x3Cu: emit_saveexec(false, "(_ol & (" + a + "))", "0u"); ok = true; break; // s_and_saveexec_b32
            case 0x25u: emit_saveexec(true,  "(_ol | (" + a + "))", "(_oh | (" + a_hi + "))"); ok = true; break; // s_or_saveexec_b64
            case 0x3Du: emit_saveexec(false, "(_ol | (" + a + "))", "0u"); ok = true; break; // s_or_saveexec_b32
            case 0x26u: emit_saveexec(true,  "(_ol ^ (" + a + "))", "(_oh ^ (" + a_hi + "))"); ok = true; break; // s_xor_saveexec_b64
            case 0x3Eu: emit_saveexec(false, "(_ol ^ (" + a + "))", "0u"); ok = true; break; // s_xor_saveexec_b32
            case 0x27u: emit_saveexec(true,  "((" + a + ") & (~_ol))", "((" + a_hi + ") & (~_oh))"); ok = true; break; // s_andn2_saveexec_b64
            case 0x3Fu: emit_saveexec(false, "((" + a + ") & (~_ol))", "0u"); ok = true; break; // s_andn2_saveexec_b32
            case 0x28u: emit_saveexec(true,  "((" + a + ") | (~_ol))", "((" + a_hi + ") | (~_oh))"); ok = true; break; // s_orn2_saveexec_b64
            case 0x40u: emit_saveexec(false, "((" + a + ") | (~_ol))", "0u"); ok = true; break; // s_orn2_saveexec_b32
            case 0x29u: emit_saveexec(true,  "(~((" + a + ") & _ol))", "(~((" + a_hi + ") & _oh))"); ok = true; break; // s_nand_saveexec_b64
            case 0x41u: emit_saveexec(false, "(~((" + a + ") & _ol))", "0u"); ok = true; break; // s_nand_saveexec_b32
            case 0x2Au: emit_saveexec(true,  "(~((" + a + ") | _ol))", "(~((" + a_hi + ") | _oh))"); ok = true; break; // s_nor_saveexec_b64
            case 0x42u: emit_saveexec(false, "(~((" + a + ") | _ol))", "0u"); ok = true; break; // s_nor_saveexec_b32
            case 0x2Bu: emit_saveexec(true,  "(~((" + a + ") ^ _ol))", "(~((" + a_hi + ") ^ _oh))"); ok = true; break; // s_xnor_saveexec_b64
            case 0x43u: emit_saveexec(false, "(~((" + a + ") ^ _ol))", "0u"); ok = true; break; // s_xnor_saveexec_b32
            case 0x2Cu: // s_quadmask_b32
                line(sreg(sd) + " = wsx_quadmask32(" + a + "); scc = (" + sreg(sd) + " != 0u);");
                ok = true; break;
            case 0x2Du: // s_quadmask_b64
                line(sreg(sd) + " = wsx_quadmask32(" + a + ") | (wsx_quadmask32(" + a_hi + ") << 8u); " + sreg_hi(sd) + " = 0u; scc = (" + sreg(sd) + " != 0u);");
                ok = true; break;
            case 0x34u: // s_abs_i32
                line(sreg(sd) + " = uint(abs(int(" + a + "))); scc = (" + sreg(sd) + " != 0u);");
                ok = true; break;
            case 0x37u: emit_saveexec(true,  "((~(" + a + ")) & _ol)", "((~(" + a_hi + ")) & _oh)"); ok = true; break; // s_andn1_saveexec_b64
            case 0x44u: emit_saveexec(false, "((~(" + a + ")) & _ol)", "0u"); ok = true; break; // s_andn1_saveexec_b32
            case 0x38u: emit_saveexec(true,  "((~(" + a + ")) | _ol)", "((~(" + a_hi + ")) | _oh)"); ok = true; break; // s_orn1_saveexec_b64
            case 0x45u: emit_saveexec(false, "((~(" + a + ")) | _ol)", "0u"); ok = true; break; // s_orn1_saveexec_b32
            case 0x39u: emit_wrexec(true,  "((~(" + a + ")) & exec_lo)", "((~(" + a_hi + ")) & exec_hi)"); ok = true; break; // s_andn1_wrexec_b64
            case 0x46u: emit_wrexec(false, "((~(" + a + ")) & exec_lo)", "0u"); ok = true; break; // s_andn1_wrexec_b32
            case 0x3Au: emit_wrexec(true,  "((" + a + ") & (~exec_lo))", "((" + a_hi + ") & (~exec_hi))"); ok = true; break; // s_andn2_wrexec_b64
            case 0x47u: emit_wrexec(false, "((" + a + ") & (~exec_lo))", "0u"); ok = true; break; // s_andn2_wrexec_b32
            case 0x3Bu: // s_bitreplicate_b64_b32
                line(sreg(sd) + " = " + a + "; " + sreg_hi(sd) + " = " + a + ";");
                ok = true; break;
            default:
                break;
            }
            break;
        }
        case Enc::SOP2: {
            const uint32_t sd = (d.w0 >> 16) & 0x7Fu;
            const uint32_t s0 = d.w0 & 0xFFu;
            const uint32_t s1 = (d.w0 >> 8) & 0xFFu;
            const std::string a = operand_u32(s0, d.lit);
            const std::string b = operand_u32(s1, d.lit);
            const std::string a_hi = operand_u32_hi(s0, d.lit);
            const std::string b_hi = operand_u32_hi(s1, d.lit);
            auto set_exec_sync = [&]() {
                if (sd == 126u || sd == 127u) line("lane_active = " + expr_mask_bit("exec_lo", "exec_hi") + ";");
            };
            switch (d.op) {
            case 0x00u: // s_add_u32
                line("{ uint _a = " + a + "; uint _b = " + b + "; uint _r = _a + _b; " + sreg(sd) + " = _r; scc = (_r < _a); }");
                ok = true; break;
            case 0x01u: // s_sub_u32
                line("{ uint _a = " + a + "; uint _b = " + b + "; " + sreg(sd) + " = _a - _b; scc = (_b > _a); }");
                ok = true; break;
            case 0x02u: // s_add_i32
                line(sreg(sd) + " = uint(int(" + a + ") + int(" + b + ")); scc = false;");
                ok = true; break;
            case 0x03u: // s_sub_i32
                line(sreg(sd) + " = uint(int(" + a + ") - int(" + b + ")); scc = false;");
                ok = true; break;
            case 0x04u: // s_addc_u32
                line("{ uint _a = " + a + "; uint _b = " + b + "; uint _c = scc ? 1u : 0u; uint _r = _a + _b + _c; " + sreg(sd) + " = _r; scc = (_r < _a) || (_c != 0u && _r == _a); }");
                ok = true; break;
            case 0x05u: // s_subb_u32
                line("{ uint _a = " + a + "; uint _b = " + b + "; uint _c = scc ? 1u : 0u; " + sreg(sd) + " = _a - _b - _c; scc = (_b > _a) || (_c != 0u && _b == _a); }");
                ok = true; break;
            case 0x06u: line(sreg(sd) + " = uint(min(int(" + a + "), int(" + b + "))); scc = (int(" + a + ") < int(" + b + "));"); ok = true; break;
            case 0x07u: line(sreg(sd) + " = min(" + a + ", " + b + "); scc = (" + a + " < " + b + ");"); ok = true; break;
            case 0x08u: line(sreg(sd) + " = uint(max(int(" + a + "), int(" + b + "))); scc = (int(" + a + ") > int(" + b + "));"); ok = true; break;
            case 0x09u: line(sreg(sd) + " = max(" + a + ", " + b + "); scc = (" + a + " > " + b + ");"); ok = true; break;
            case 0x0Au: // s_cselect_b32
                line(sreg(sd) + " = scc ? (" + a + ") : (" + b + ");");
                set_exec_sync();
                ok = true; break;
            case 0x0Bu: // s_cselect_b64
                line(sreg(sd) + " = scc ? (" + a + ") : (" + b + "); " + sreg_hi(sd) + " = scc ? (" + a_hi + ") : (" + b_hi + ");");
                set_exec_sync();
                ok = true; break;
            case 0x0Eu: // s_and_b32
                line(sreg(sd) + " = (" + a + ") & (" + b + "); scc = (" + sreg(sd) + " != 0u);");
                set_exec_sync(); ok = true; break;
            case 0x0Fu: // s_and_b64
                line(sreg(sd) + " = (" + a + ") & (" + b + "); " + sreg_hi(sd) + " = (" + a_hi + ") & (" + b_hi + "); scc = ((" + sreg(sd) + " | " + sreg_hi(sd) + ") != 0u);");
                set_exec_sync(); ok = true; break;
            case 0x10u: // s_or_b32
                line(sreg(sd) + " = (" + a + ") | (" + b + "); scc = (" + sreg(sd) + " != 0u);");
                set_exec_sync(); ok = true; break;
            case 0x11u: // s_or_b64
                line(sreg(sd) + " = (" + a + ") | (" + b + "); " + sreg_hi(sd) + " = (" + a_hi + ") | (" + b_hi + "); scc = ((" + sreg(sd) + " | " + sreg_hi(sd) + ") != 0u);");
                set_exec_sync(); ok = true; break;
            case 0x12u: // s_xor_b32
                line(sreg(sd) + " = (" + a + ") ^ (" + b + "); scc = (" + sreg(sd) + " != 0u);");
                set_exec_sync(); ok = true; break;
            case 0x13u: // s_xor_b64
                line(sreg(sd) + " = (" + a + ") ^ (" + b + "); " + sreg_hi(sd) + " = (" + a_hi + ") ^ (" + b_hi + "); scc = ((" + sreg(sd) + " | " + sreg_hi(sd) + ") != 0u);");
                set_exec_sync(); ok = true; break;
            case 0x14u: // s_andn2_b32
                line(sreg(sd) + " = (" + a + ") & (~(" + b + ")); scc = (" + sreg(sd) + " != 0u);");
                set_exec_sync(); ok = true; break;
            case 0x15u: // s_andn2_b64
                line(sreg(sd) + " = (" + a + ") & (~(" + b + ")); " + sreg_hi(sd) + " = (" + a_hi + ") & (~(" + b_hi + ")); scc = ((" + sreg(sd) + " | " + sreg_hi(sd) + ") != 0u);");
                set_exec_sync(); ok = true; break;
            case 0x16u: // s_orn2_b32
                line(sreg(sd) + " = (" + a + ") | (~(" + b + ")); scc = (" + sreg(sd) + " != 0u);");
                set_exec_sync(); ok = true; break;
            case 0x17u: // s_orn2_b64
                line(sreg(sd) + " = (" + a + ") | (~(" + b + ")); " + sreg_hi(sd) + " = (" + a_hi + ") | (~(" + b_hi + ")); scc = ((" + sreg(sd) + " | " + sreg_hi(sd) + ") != 0u);");
                set_exec_sync(); ok = true; break;
            case 0x18u: // s_nand_b32
                line(sreg(sd) + " = ~((" + a + ") & (" + b + ")); scc = (" + sreg(sd) + " != 0u);");
                set_exec_sync(); ok = true; break;
            case 0x19u: // s_nand_b64
                line(sreg(sd) + " = ~((" + a + ") & (" + b + ")); " + sreg_hi(sd) + " = ~((" + a_hi + ") & (" + b_hi + ")); scc = ((" + sreg(sd) + " | " + sreg_hi(sd) + ") != 0u);");
                set_exec_sync(); ok = true; break;
            case 0x1Au: // s_nor_b32
                line(sreg(sd) + " = ~((" + a + ") | (" + b + ")); scc = (" + sreg(sd) + " != 0u);");
                set_exec_sync(); ok = true; break;
            case 0x1Bu: // s_nor_b64
                line(sreg(sd) + " = ~((" + a + ") | (" + b + ")); " + sreg_hi(sd) + " = ~((" + a_hi + ") | (" + b_hi + ")); scc = ((" + sreg(sd) + " | " + sreg_hi(sd) + ") != 0u);");
                set_exec_sync(); ok = true; break;
            case 0x1Cu: // s_xnor_b32
                line(sreg(sd) + " = ~((" + a + ") ^ (" + b + ")); scc = (" + sreg(sd) + " != 0u);");
                set_exec_sync(); ok = true; break;
            case 0x1Du: // s_xnor_b64
                line(sreg(sd) + " = ~((" + a + ") ^ (" + b + ")); " + sreg_hi(sd) + " = ~((" + a_hi + ") ^ (" + b_hi + ")); scc = ((" + sreg(sd) + " | " + sreg_hi(sd) + ") != 0u);");
                set_exec_sync(); ok = true; break;
            case 0x1Eu: line(sreg(sd) + " = (" + a + ") << ((" + b + ") & 31u); scc = (" + sreg(sd) + " != 0u);"); ok = true; break;
            case 0x1Fu: // s_lshl_b64
                line("{ uint64_t _r = " + operand_u64(s0, d.lit) + " << ((" + b + ") & 63u); " + sreg(sd) + " = uint(_r); " + sreg_hi(sd) + " = uint(_r >> 32); scc = (_r != 0ull); }");
                ok = true; break;
            case 0x20u: line(sreg(sd) + " = (" + a + ") >> ((" + b + ") & 31u); scc = (" + sreg(sd) + " != 0u);"); ok = true; break;
            case 0x21u: // s_lshr_b64
                line("{ uint64_t _r = " + operand_u64(s0, d.lit) + " >> ((" + b + ") & 63u); " + sreg(sd) + " = uint(_r); " + sreg_hi(sd) + " = uint(_r >> 32); scc = (_r != 0ull); }");
                ok = true; break;
            case 0x22u: line(sreg(sd) + " = uint(int(" + a + ") >> ((" + b + ") & 31u)); scc = (" + sreg(sd) + " != 0u);"); ok = true; break;
            case 0x23u: // s_ashr_i64
                line("{ int64_t _r = int64_t(" + operand_u64(s0, d.lit) + ") >> ((" + b + ") & 63u); " + sreg(sd) + " = uint(uint64_t(_r)); " + sreg_hi(sd) + " = uint(uint64_t(_r) >> 32); scc = (_r != 0); }");
                ok = true; break;
            case 0x24u: // s_bfm_b32
                line("{ uint _w = (" + a + ") & 31u; " + sreg(sd) + " = ((1u << _w) - 1u) << ((" + b + ") & 31u); }");
                ok = true; break;
            case 0x25u: // s_bfm_b64
                line("{ uint64_t _r = ((1ull << ((" + a + ") & 63u)) - 1ull) << ((" + b + ") & 63u); " + sreg(sd) + " = uint(_r); " + sreg_hi(sd) + " = uint(_r >> 32); }");
                ok = true; break;
            case 0x26u: line(sreg(sd) + " = uint(int(" + a + ") * int(" + b + "));"); ok = true; break;
            case 0x27u: // s_bfe_u32
                line("{ uint _sh = (" + b + ") & 31u; uint _w = ((" + b + ") >> 16u) & 0x7Fu; " + sreg(sd) + " = (_w == 0u) ? 0u : ((_w >= 32u) ? ((" + a + ") >> _sh) : (((" + a + ") >> _sh) & ((1u << _w) - 1u))); scc = (" + sreg(sd) + " != 0u); }");
                ok = true; break;
            case 0x28u: // s_bfe_i32
                line("{ uint _sh = (" + b + ") & 31u; uint _w = ((" + b + ") >> 16u) & 0x7Fu; " + sreg(sd) + " = (_w == 0u) ? 0u : ((_w >= 32u) ? uint(int(" + a + ") >> _sh) : uint(int(((" + a + ") >> _sh) << (32u - _w)) >> (32u - _w))); scc = (" + sreg(sd) + " != 0u); }");
                ok = true; break;
            case 0x29u: // s_bfe_u64
                line("{ uint _sh = (" + b + ") & 63u; uint _w = ((" + b + ") >> 16u) & 0x7Fu; uint64_t _r = (_w == 0u) ? 0ull : ((_w >= 64u) ? (" + operand_u64(s0, d.lit) + " >> _sh) : ((" + operand_u64(s0, d.lit) + " >> _sh) & ((1ull << _w) - 1ull))); " + sreg(sd) + " = uint(_r); " + sreg_hi(sd) + " = uint(_r >> 32); scc = (_r != 0ull); }");
                ok = true; break;
            case 0x2Au: // s_bfe_i64
                line("{ uint _sh = (" + b + ") & 63u; uint _w = ((" + b + ") >> 16u) & 0x7Fu; int64_t _r = (_w == 0u) ? 0 : ((_w >= 64u) ? (int64_t(" + operand_u64(s0, d.lit) + ") >> _sh) : ((int64_t(" + operand_u64(s0, d.lit) + " >> _sh) << (64u - _w)) >> (64u - _w))); " + sreg(sd) + " = uint(uint64_t(_r)); " + sreg_hi(sd) + " = uint(uint64_t(_r) >> 32); scc = (_r != 0); }");
                ok = true; break;
            case 0x2Cu: line(sreg(sd) + " = uint(abs(int(" + a + ") - int(" + b + "))); scc = (" + sreg(sd) + " != 0u);"); ok = true; break; // s_absdiff_i32
            case 0x2Eu: line(sreg(sd) + " = ((" + a + ") << 1u) + (" + b + ");"); ok = true; break;
            case 0x2Fu: line(sreg(sd) + " = ((" + a + ") << 2u) + (" + b + ");"); ok = true; break;
            case 0x30u: line(sreg(sd) + " = ((" + a + ") << 3u) + (" + b + ");"); ok = true; break;
            case 0x31u: line(sreg(sd) + " = ((" + a + ") << 4u) + (" + b + ");"); ok = true; break;
            case 0x32u: line(sreg(sd) + " = ((" + a + ") & 0xFFFFu) | (((" + b + ") & 0xFFFFu) << 16u);"); ok = true; break; // s_pack_ll_b32_b16
            case 0x33u: line(sreg(sd) + " = ((" + a + ") & 0xFFFFu) | ((" + b + ") & 0xFFFF0000u);"); ok = true; break; // s_pack_lh_b32_b16
            case 0x34u: line(sreg(sd) + " = (((" + a + ") >> 16u) & 0xFFFFu) | ((" + b + ") & 0xFFFF0000u);"); ok = true; break; // s_pack_hh_b32_b16
            case 0x35u: line(sreg(sd) + " = uint((uint64_t(" + a + ") * uint64_t(" + b + ")) >> 32);"); ok = true; break; // s_mul_hi_u32
            case 0x36u: line(sreg(sd) + " = uint(uint64_t(int64_t(int(" + a + ")) * int64_t(int(" + b + "))) >> 32);"); ok = true; break; // s_mul_hi_i32
            default:
                break;
            }
            break;
        }
        case Enc::SOPC: {
            const uint32_t s0 = d.w0 & 0xFFu;
            const uint32_t s1 = (d.w0 >> 8) & 0xFFu;
            const std::string a = operand_u32(s0, d.lit);
            const std::string b = operand_u32(s1, d.lit);
            switch (d.op) {
            case 0x00u: line("scc = (int(" + a + ") == int(" + b + "));"); ok = true; break;
            case 0x01u: line("scc = (int(" + a + ") != int(" + b + "));"); ok = true; break;
            case 0x02u: line("scc = (int(" + a + ") > int(" + b + "));");  ok = true; break;
            case 0x03u: line("scc = (int(" + a + ") >= int(" + b + "));"); ok = true; break;
            case 0x04u: line("scc = (int(" + a + ") < int(" + b + "));");  ok = true; break;
            case 0x05u: line("scc = (int(" + a + ") <= int(" + b + "));"); ok = true; break;
            case 0x06u: line("scc = (" + a + " == " + b + ");"); ok = true; break;
            case 0x07u: line("scc = (" + a + " != " + b + ");"); ok = true; break;
            case 0x08u: line("scc = (" + a + " > " + b + ");");  ok = true; break;
            case 0x09u: line("scc = (" + a + " >= " + b + ");"); ok = true; break;
            case 0x0Au: line("scc = (" + a + " < " + b + ");");  ok = true; break;
            case 0x0Bu: line("scc = (" + a + " <= " + b + ");"); ok = true; break;
            case 0x0Cu: line("scc = (((" + a + ") & (1u << ((" + b + ") & 31u))) == 0u);"); ok = true; break;
            case 0x0Du: line("scc = (((" + a + ") & (1u << ((" + b + ") & 31u))) != 0u);"); ok = true; break;
            case 0x0Eu: line("scc = ((" + operand_u64(s0, d.lit) + " & (1ull << ((" + b + ") & 63u))) == 0ull);"); ok = true; break;
            case 0x0Fu: line("scc = ((" + operand_u64(s0, d.lit) + " & (1ull << ((" + b + ") & 63u))) != 0ull);"); ok = true; break;
            case 0x12u: line("scc = (" + operand_u64(s0, d.lit) + " == " + operand_u64(s1, d.lit) + ");"); ok = true; break;
            case 0x13u: line("scc = (" + operand_u64(s0, d.lit) + " != " + operand_u64(s1, d.lit) + ");"); ok = true; break;
            default: break;
            }
            break;
        }
        case Enc::SOPK: {
            const uint32_t sd = (d.w0 >> 16) & 0x7Fu;
            const int32_t simm = static_cast<int16_t>(d.w0 & 0xFFFFu);
            const uint32_t uimm = d.w0 & 0xFFFFu;
            switch (d.op) {
            case 0x00u: line(sreg(sd) + " = uint(" + std::to_string(simm) + ");"); ok = true; break;
            case 0x01u: ok = true; break; // s_version
            case 0x02u: line("if (scc) { " + sreg(sd) + " = uint(" + std::to_string(simm) + "); }"); ok = true; break;
            case 0x03u: line("scc = (int(" + sreg(sd) + ") == " + std::to_string(simm) + ");"); ok = true; break;
            case 0x04u: line("scc = (int(" + sreg(sd) + ") != " + std::to_string(simm) + ");"); ok = true; break;
            case 0x05u: line("scc = (int(" + sreg(sd) + ") > " + std::to_string(simm) + ");");  ok = true; break;
            case 0x06u: line("scc = (int(" + sreg(sd) + ") >= " + std::to_string(simm) + ");"); ok = true; break;
            case 0x07u: line("scc = (int(" + sreg(sd) + ") < " + std::to_string(simm) + ");");  ok = true; break;
            case 0x08u: line("scc = (int(" + sreg(sd) + ") <= " + std::to_string(simm) + ");"); ok = true; break;
            case 0x09u: line("scc = (" + sreg(sd) + " == " + std::to_string(uimm) + "u);"); ok = true; break;
            case 0x0Au: line("scc = (" + sreg(sd) + " != " + std::to_string(uimm) + "u);"); ok = true; break;
            case 0x0Bu: line("scc = (" + sreg(sd) + " > " + std::to_string(uimm) + "u);");  ok = true; break;
            case 0x0Cu: line("scc = (" + sreg(sd) + " >= " + std::to_string(uimm) + "u);"); ok = true; break;
            case 0x0Du: line("scc = (" + sreg(sd) + " < " + std::to_string(uimm) + "u);");  ok = true; break;
            case 0x0Eu: line("scc = (" + sreg(sd) + " <= " + std::to_string(uimm) + "u);"); ok = true; break;
            case 0x0Fu: line(sreg(sd) + " = uint(int(" + sreg(sd) + ") + " + std::to_string(simm) + ");"); ok = true; break;
            case 0x10u: line(sreg(sd) + " = uint(int(" + sreg(sd) + ") * " + std::to_string(simm) + ");"); ok = true; break;
            case 0x12u: line(sreg(sd) + " = 0u;"); ok = true; break; // s_getreg_b32
            case 0x13u: // s_setreg_b32
            case 0x15u: // s_setreg_imm32_b32
            case 0x17u: // s_waitcnt_vscnt
            case 0x18u: // s_waitcnt_vmcnt
            case 0x19u: // s_waitcnt_expcnt
            case 0x1Au: // s_waitcnt_lgkmcnt
            case 0x1Bu: // s_subvector_loop_begin
            case 0x1Cu: // s_subvector_loop_end
                ok = true; break;
            case 0x16u: // s_call_b64
                line("wsx_pc = " + std::to_string(resolve_branch_bb(static_cast<int16_t>(simm))) + "u; break;");
                ok = true; break;
            default: break;
            }
            break;
        }
        case Enc::VOP1: {
            const uint32_t vd = (d.w0 >> 17) & 0xFFu;
            const uint32_t s0 = d.w0 & 0x1FFu;
            if (d.op == 0x02u) { // v_readfirstlane_b32
                line(sreg(vd & 0x7Fu) + " = " + expr_readfirstlane(operand_u32(s0, d.lit)) + ";");
                ok = true;
            } else if (d.op == 0x65u && s0 >= 256u) { // v_swap_b32
                const uint32_t vs = s0 - 256u;
                line("if (lane_active) { uint _sw = " + vreg(vd) + "; " + vreg(vd) + " = " + vreg(vs) + "; " + vreg(vs) + " = _sw; }");
                ok = true;
            } else {
                std::string s0u, s1u, s0f, s1f;
                bool cl = false;
                uint32_t om = 0u;
                resolve_vop_operands(s0, 0u, d.lit, false, s0u, s1u, s0f, s1f, cl, om);
                ok = emit_vop1_core(d.op, vd, s0u, s0f, s0, d.lit, cl, om);
            }
            break;
        }
        case Enc::VOP2: {
            const uint32_t vd = (d.w0 >> 17) & 0xFFu;
            const uint32_t s0 = d.w0 & 0x1FFu;
            const uint32_t vs1 = (d.w0 >> 9) & 0xFFu;
            std::string s0u, s1u, s0f, s1f;
            bool cl = false;
            uint32_t om = 0u;
            resolve_vop_operands(s0, vs1, d.lit, true, s0u, s1u, s0f, s1f, cl, om);
            ok = emit_vop2_core(d.op, vd, s0u, s1u, s0f, s1f, d.lit, cl, om);
            break;
        }
        case Enc::VOPC: {
            const uint32_t s0 = d.w0 & 0x1FFu;
            const uint32_t vs1 = (d.w0 >> 9) & 0xFFu;
            const uint32_t family = d.op & 0xF0u;
            const bool is_cmpx = (family == 0x10u || family == 0x30u || family == 0x90u ||
                                  family == 0xB0u || family == 0xD0u || family == 0xF0u);
            std::string s0u, s1u, s0f, s1f;
            bool cl = false;
            uint32_t om = 0u;
            resolve_vop_operands(s0, vs1, d.lit, true, s0u, s1u, s0f, s1f, cl, om);
            emit_vopc_op(d.op, s0u, s1u, s0f, s1f, is_cmpx);
            ok = true;
            break;
        }
        case Enc::VOP3: {
            const uint32_t vd = d.w0 & 0xFFu;
            const uint32_t abs_bits = (d.w0 >> 8) & 7u;
            const uint32_t opsel = (d.w0 >> 11) & 0xFu;
            const bool clamp_mod = ((d.w0 >> 15) & 1u) != 0u;
            const uint32_t s0 = d.w1 & 0x1FFu;
            const uint32_t s1 = (d.w1 >> 9) & 0x1FFu;
            const uint32_t s2 = (d.w1 >> 18) & 0x1FFu;
            const uint32_t omod = (d.w1 >> 27) & 3u;
            const uint32_t neg_bits = (d.w1 >> 29) & 7u;

            const std::string a_u = operand_u32(s0, d.lit);
            const std::string b_u = operand_u32(s1, d.lit);
            const std::string c_u = operand_u32(s2, d.lit);
            const std::string a_f = operand_f32(s0, d.lit, (abs_bits & 1u) != 0, (neg_bits & 1u) != 0);
            const std::string b_f = operand_f32(s1, d.lit, (abs_bits & 2u) != 0, (neg_bits & 2u) != 0);
            const std::string c_f = operand_f32(s2, d.lit, (abs_bits & 4u) != 0, (neg_bits & 4u) != 0);

            auto sel16 = [&](const std::string& u, uint32_t bit) -> std::string {
                return (opsel & (1u << bit)) ? ("((" + u + ") >> 16u)") : ("((" + u + ") & 0xFFFFu)");
            };
            auto sel16_i = [&](const std::string& u, uint32_t bit) -> std::string {
                return (opsel & (1u << bit)) ? ("(int(" + u + ") >> 16)") : ("((int(" + u + ") << 16) >> 16)");
            };
            auto sel16_f = [&](const std::string& u, uint32_t bit) -> std::string {
                std::string e = "f16tof32(" + sel16(u, bit) + ")";
                if (abs_bits & (1u << bit)) e = "abs(" + e + ")";
                if (neg_bits & (1u << bit)) e = "(-(" + e + "))";
                return e;
            };
            auto write16 = [&](const std::string& expr16) {
                if (opsel & 8u) emit_v_assign(vd, "((" + vreg(vd) + " & 0x0000FFFFu) | (((" + expr16 + ") & 0xFFFFu) << 16u))");
                else            emit_v_assign(vd, "((" + vreg(vd) + " & 0xFFFF0000u) | ((" + expr16 + ") & 0xFFFFu))");
            };

            if (d.op < 0x100u) {
                const uint32_t family = d.op & 0xF0u;
                const bool is_cmpx = (family == 0x10u || family == 0x30u || family == 0x90u ||
                                      family == 0xB0u || family == 0xD0u || family == 0xF0u);
                emit_vopc_op(d.op, a_u, b_u, a_f, b_f, is_cmpx, vd & 0x7Fu);
                ok = true;
            } else if (d.op >= 0x100u && d.op < 0x140u) {
                const std::string c_hi = is_wave64_ ? operand_u32_hi(s2, d.lit) : "0u";
                const std::string mb_c = expr_mask_bit(c_u, c_hi);
                auto emit_vop3_carry = [&](uint32_t sd) {
                    if (is_wave64_) {
                        emit_ballot64_assign("vcc_lane", sreg(sd), sreg_hi(sd));
                    } else {
                        line(sreg(sd) + " = WaveActiveBallot(vcc_lane).x;");
                    }
                };
                if (d.op == 0x101u) { // v_cndmask_b32 (VOP3 uses src2 scalar mask!)
                    emit_v_assign(vd, "(" + mb_c + " ? (" + b_u + ") : (" + a_u + "))");
                    ok = true;
                } else if (d.op == 0x128u) { // v_add_co_ci_u32 (VOP3B: carry-in = s2, carry-out = sdst)
                    const uint32_t sd = (d.w0 >> 8) & 0x7Fu;
                    line("if (lane_active) { uint _a = " + a_u + "; uint _b = " + b_u + "; uint _c = " + mb_c + " ? 1u : 0u; uint _r = _a + _b + _c; vcc_lane = (_r < _a) || (_c != 0u && _r == _a); " + vreg(vd) + " = _r; }");
                    emit_vop3_carry(sd);
                    ok = true;
                } else if (d.op == 0x129u) { // v_sub_co_ci_u32
                    const uint32_t sd = (d.w0 >> 8) & 0x7Fu;
                    line("if (lane_active) { uint _a = " + a_u + "; uint _b = " + b_u + "; uint _c = " + mb_c + " ? 1u : 0u; uint _r = _a - _b - _c; vcc_lane = (_b > _a) || (_c != 0u && _b == _a); " + vreg(vd) + " = _r; }");
                    emit_vop3_carry(sd);
                    ok = true;
                } else if (d.op == 0x12Au) { // v_subrev_co_ci_u32
                    const uint32_t sd = (d.w0 >> 8) & 0x7Fu;
                    line("if (lane_active) { uint _a = " + b_u + "; uint _b = " + a_u + "; uint _c = " + mb_c + " ? 1u : 0u; uint _r = _a - _b - _c; vcc_lane = (_b > _a) || (_c != 0u && _b == _a); " + vreg(vd) + " = _r; }");
                    emit_vop3_carry(sd);
                    ok = true;
                } else {
                    ok = emit_vop2_core(d.op - 0x100u, vd, a_u, b_u, a_f, b_f, d.lit, clamp_mod, omod);
                }
            } else if (d.op >= 0x180u && d.op < 0x200u) {
                ok = emit_vop1_core(d.op - 0x180u, vd, a_u, a_f, s0, d.lit, clamp_mod, omod);
            } else {
                switch (d.op) {
                case 0x140u:
                case 0x141u:
                case 0x14Bu: emit_v_assign_f32(vd, "mad(" + a_f + ", " + b_f + ", " + c_f + ")", clamp_mod, omod); ok = true; break;
                case 0x142u: emit_v_assign(vd, "uint(((int(" + a_u + ") << 8) >> 8) * ((int(" + b_u + ") << 8) >> 8) + int(" + c_u + "))"); ok = true; break;
                case 0x143u: emit_v_assign(vd, "((" + a_u + " & 0xFFFFFFu) * (" + b_u + " & 0xFFFFFFu) + (" + c_u + "))"); ok = true; break;
                case 0x144u: emit_v_assign_f32(vd, "wsx_cubeid(" + a_f + ", " + b_f + ", " + c_f + ")", clamp_mod, omod); ok = true; break;
                case 0x145u: emit_v_assign_f32(vd, "wsx_cubesc(" + a_f + ", " + b_f + ", " + c_f + ")", clamp_mod, omod); ok = true; break;
                case 0x146u: emit_v_assign_f32(vd, "wsx_cubetc(" + a_f + ", " + b_f + ", " + c_f + ")", clamp_mod, omod); ok = true; break;
                case 0x147u: emit_v_assign_f32(vd, "wsx_cubema(" + a_f + ", " + b_f + ", " + c_f + ")", clamp_mod, omod); ok = true; break;
                case 0x148u: emit_v_assign(vd, "wsx_bfe_u(" + a_u + ", " + b_u + ", " + c_u + ")"); ok = true; break;
                case 0x149u: emit_v_assign(vd, "wsx_bfe_i(" + a_u + ", " + b_u + ", " + c_u + ")"); ok = true; break;
                case 0x14Au: emit_v_assign(vd, "(((" + a_u + ") & (" + b_u + ")) | ((~(" + a_u + ")) & (" + c_u + ")))"); ok = true; break;
                case 0x14Cu: emit_v_assign_f64(vd, "fma(" + operand_f64(s0, d.lit, (abs_bits&1)!=0, (neg_bits&1)!=0) + ", " + operand_f64(s1, d.lit, (abs_bits&2)!=0, (neg_bits&2)!=0) + ", " + operand_f64(s2, d.lit, (abs_bits&4)!=0, (neg_bits&4)!=0) + ")"); ok = true; break;
                case 0x14Du: emit_v_assign(vd, "wsx_lerp_u8(" + a_u + ", " + b_u + ", " + c_u + ")"); ok = true; break;
                case 0x14Eu: emit_v_assign(vd, "wsx_alignbit(" + a_u + ", " + b_u + ", " + c_u + ")"); ok = true; break;
                case 0x14Fu: emit_v_assign(vd, "wsx_alignbyte(" + a_u + ", " + b_u + ", " + c_u + ")"); ok = true; break;
                case 0x150u: emit_v_assign_f32(vd, "(((" + a_f + ") > 0.0f && (" + b_f + ") > 0.0f) ? pow(" + b_f + ", clamp(" + c_f + ", -128.0f, 128.0f)) : 0.0f)", clamp_mod, omod); ok = true; break;
                case 0x151u: emit_v_assign_f32(vd, "min(min(" + a_f + ", " + b_f + "), " + c_f + ")", clamp_mod, omod); ok = true; break;
                case 0x152u: emit_v_assign(vd, "uint(min(min(int(" + a_u + "), int(" + b_u + ")), int(" + c_u + ")))"); ok = true; break;
                case 0x153u: emit_v_assign(vd, "min(min(" + a_u + ", " + b_u + "), " + c_u + ")"); ok = true; break;
                case 0x154u: emit_v_assign_f32(vd, "max(max(" + a_f + ", " + b_f + "), " + c_f + ")", clamp_mod, omod); ok = true; break;
                case 0x155u: emit_v_assign(vd, "uint(max(max(int(" + a_u + "), int(" + b_u + ")), int(" + c_u + ")))"); ok = true; break;
                case 0x156u: emit_v_assign(vd, "max(max(" + a_u + ", " + b_u + "), " + c_u + ")"); ok = true; break;
                case 0x157u: emit_v_assign_f32(vd, "wsx_med3_f(" + a_f + ", " + b_f + ", " + c_f + ")", clamp_mod, omod); ok = true; break;
                case 0x158u: emit_v_assign(vd, "wsx_med3_i(" + a_u + ", " + b_u + ", " + c_u + ")"); ok = true; break;
                case 0x159u: emit_v_assign(vd, "wsx_med3_u(" + a_u + ", " + b_u + ", " + c_u + ")"); ok = true; break;
                case 0x15Au: emit_v_assign(vd, "wsx_sad_u8(" + a_u + ", " + b_u + ", " + c_u + ")"); ok = true; break;
                case 0x15Bu: emit_v_assign(vd, "((wsx_sad_u8(" + a_u + ", " + b_u + ", 0u) << 16u) + (" + c_u + "))"); ok = true; break;
                case 0x15Cu: emit_v_assign(vd, "(uint(abs(int((" + a_u + ") & 0xFFFFu) - int((" + b_u + ") & 0xFFFFu))) + uint(abs(int((" + a_u + ") >> 16u) - int((" + b_u + ") >> 16u))) + (" + c_u + "))"); ok = true; break;
                case 0x15Du: emit_v_assign(vd, "((" + a_u + " > " + b_u + " ? (" + a_u + " - " + b_u + ") : (" + b_u + " - " + a_u + ")) + (" + c_u + "))"); ok = true; break;
                case 0x15Eu: emit_v_assign(vd, "(((" + c_u + ") & ~(0xFFu << (((" + b_u + ") & 3u) * 8u))) | (uint(clamp(" + a_f + ", 0.0f, 255.0f)) << (((" + b_u + ") & 3u) * 8u)))"); ok = true; break;
                case 0x15Fu: emit_v_assign_f32(vd, "((" + b_f + ") == 0.0f ? ((" + c_f + ") / (" + b_f + ")) : (" + a_f + "))", clamp_mod, omod); ok = true; break; // v_div_fixup_f32
                case 0x160u: emit_v_assign_f64(vd, operand_f64(s0, d.lit)); ok = true; break; // v_div_fixup_f64
                case 0x164u: emit_v_assign_f64(vd, "(" + operand_f64(s0, d.lit, (abs_bits&1)!=0, (neg_bits&1)!=0) + " + " + operand_f64(s1, d.lit, (abs_bits&2)!=0, (neg_bits&2)!=0) + ")"); ok = true; break;
                case 0x165u: emit_v_assign_f64(vd, "(" + operand_f64(s0, d.lit, (abs_bits&1)!=0, (neg_bits&1)!=0) + " * " + operand_f64(s1, d.lit, (abs_bits&2)!=0, (neg_bits&2)!=0) + ")"); ok = true; break;
                case 0x166u: emit_v_assign_f64(vd, "min(" + operand_f64(s0, d.lit, (abs_bits&1)!=0, (neg_bits&1)!=0) + ", " + operand_f64(s1, d.lit, (abs_bits&2)!=0, (neg_bits&2)!=0) + ")"); ok = true; break;
                case 0x167u: emit_v_assign_f64(vd, "max(" + operand_f64(s0, d.lit, (abs_bits&1)!=0, (neg_bits&1)!=0) + ", " + operand_f64(s1, d.lit, (abs_bits&2)!=0, (neg_bits&2)!=0) + ")"); ok = true; break;
                case 0x168u: emit_v_assign_f64(vd, "(" + operand_f64(s0, d.lit) + " * double(exp2(float(int(" + b_u + ")))))"); ok = true; break;
                case 0x169u: emit_v_assign(vd, "((" + a_u + ") * (" + b_u + "))"); ok = true; break;
                case 0x16Au: emit_v_assign(vd, "uint((uint64_t(" + a_u + ") * uint64_t(" + b_u + ")) >> 32)"); ok = true; break;
                case 0x16Bu: emit_v_assign(vd, "uint(int(" + a_u + ") * int(" + b_u + "))"); ok = true; break;
                case 0x16Cu: emit_v_assign(vd, "uint(uint64_t(int64_t(int(" + a_u + ")) * int64_t(int(" + b_u + "))) >> 32)"); ok = true; break;
                case 0x16Du: { // v_div_scale_f32 (VOP3B)
                    const uint32_t sd = (d.w0 >> 8) & 0x7Fu;
                    emit_v_assign_f32(vd, a_f, clamp_mod, omod);
                    line(sreg(sd) + " = 0u;");
                    ok = true; break;
                }
                case 0x16Eu: { // v_div_scale_f64
                    const uint32_t sd = (d.w0 >> 8) & 0x7Fu;
                    emit_v_assign_f64(vd, operand_f64(s0, d.lit));
                    line(sreg(sd) + " = 0u;");
                    ok = true; break;
                }
                case 0x16Fu: emit_v_assign_f32(vd, "mad(" + a_f + ", " + b_f + ", " + c_f + ")", clamp_mod, omod); ok = true; break;
                case 0x170u: emit_v_assign_f64(vd, "fma(" + operand_f64(s0, d.lit) + ", " + operand_f64(s1, d.lit) + ", " + operand_f64(s2, d.lit) + ")"); ok = true; break;
                case 0x176u: { // v_mad_u64_u32
                    const uint32_t sd = (d.w0 >> 8) & 0x7Fu;
                    line("if (lane_active) { uint64_t _r = uint64_t(" + a_u + ") * uint64_t(" + b_u + ") + " + operand_u64(s2, d.lit) + "; " + vreg(vd) + " = uint(_r); " + vreg(vd + 1u) + " = uint(_r >> 32); }");
                    line(sreg(sd) + " = 0u;");
                    ok = true; break;
                }
                case 0x177u: { // v_mad_i64_i32
                    const uint32_t sd = (d.w0 >> 8) & 0x7Fu;
                    line("if (lane_active) { int64_t _r = int64_t(int(" + a_u + ")) * int64_t(int(" + b_u + ")) + int64_t(" + operand_u64(s2, d.lit) + "); " + vreg(vd) + " = uint(uint64_t(_r)); " + vreg(vd + 1u) + " = uint(uint64_t(_r) >> 32); }");
                    line(sreg(sd) + " = 0u;");
                    ok = true; break;
                }
                case 0x178u: emit_v_assign(vd, "((" + a_u + ") ^ (" + b_u + ") ^ (" + c_u + "))"); ok = true; break; // v_xor3_b32
                case 0x200u: // v_interp_p1_f32_e64
                case 0x201u: // v_interp_p2_f32_e64
                case 0x202u: { // v_interp_mov_f32_e64
                    const uint32_t attr = d.w1 & 0x3Fu;
                    const uint32_t chan = (d.w1 >> 6) & 3u;
                    emit_v_assign_f32(vd, ps_attr_expr(attr, chan), clamp_mod, omod);
                    ok = true; break;
                }
                case 0x2FFu: // v_lshlrev_b64
                    line("if (lane_active) { uint64_t _r = " + operand_u64(s1, d.lit) + " << ((" + a_u + ") & 63u); " + vreg(vd) + " = uint(_r); " + vreg(vd + 1u) + " = uint(_r >> 32); }");
                    ok = true; break;
                case 0x300u: // v_lshrrev_b64
                    line("if (lane_active) { uint64_t _r = " + operand_u64(s1, d.lit) + " >> ((" + a_u + ") & 63u); " + vreg(vd) + " = uint(_r); " + vreg(vd + 1u) + " = uint(_r >> 32); }");
                    ok = true; break;
                case 0x301u: // v_ashrrev_i64
                    line("if (lane_active) { int64_t _r = int64_t(" + operand_u64(s1, d.lit) + ") >> ((" + a_u + ") & 63u); " + vreg(vd) + " = uint(uint64_t(_r)); " + vreg(vd + 1u) + " = uint(uint64_t(_r) >> 32); }");
                    ok = true; break;
                case 0x303u: write16(sel16(a_u, 0) + " + " + sel16(b_u, 1)); ok = true; break; // v_add_nc_u16
                case 0x304u: write16(sel16(a_u, 0) + " - " + sel16(b_u, 1)); ok = true; break; // v_sub_nc_u16
                case 0x305u: write16(sel16(a_u, 0) + " * " + sel16(b_u, 1)); ok = true; break; // v_mul_lo_u16
                case 0x307u: write16(sel16(b_u, 1) + " >> (" + sel16(a_u, 0) + " & 15u)"); ok = true; break; // v_lshrrev_b16
                case 0x308u: write16("uint(" + sel16_i(b_u, 1) + " >> (" + sel16(a_u, 0) + " & 15u))"); ok = true; break; // v_ashrrev_i16
                case 0x309u: write16("max(" + sel16(a_u, 0) + ", " + sel16(b_u, 1) + ")"); ok = true; break; // v_max_u16
                case 0x30Au: write16("uint(max(" + sel16_i(a_u, 0) + ", " + sel16_i(b_u, 1) + "))"); ok = true; break; // v_max_i16
                case 0x30Bu: write16("min(" + sel16(a_u, 0) + ", " + sel16(b_u, 1) + ")"); ok = true; break; // v_min_u16
                case 0x30Cu: write16("uint(min(" + sel16_i(a_u, 0) + ", " + sel16_i(b_u, 1) + "))"); ok = true; break; // v_min_i16
                case 0x30Fu: { // v_add_co_u32
                    const uint32_t sd = (d.w0 >> 8) & 0x7Fu;
                    line("if (lane_active) { uint _a = " + a_u + "; uint _b = " + b_u + "; uint _r = _a + _b; vcc_lane = (_r < _a); " + vreg(vd) + " = _r; }");
                    if (is_wave64_) emit_ballot64_assign("vcc_lane", sreg(sd), sreg_hi(sd));
                    else            line(sreg(sd) + " = WaveActiveBallot(vcc_lane).x;");
                    ok = true; break;
                }
                case 0x310u: { // v_sub_co_u32
                    const uint32_t sd = (d.w0 >> 8) & 0x7Fu;
                    line("if (lane_active) { uint _a = " + a_u + "; uint _b = " + b_u + "; vcc_lane = (_b > _a); " + vreg(vd) + " = _a - _b; }");
                    if (is_wave64_) emit_ballot64_assign("vcc_lane", sreg(sd), sreg_hi(sd));
                    else            line(sreg(sd) + " = WaveActiveBallot(vcc_lane).x;");
                    ok = true; break;
                }
                case 0x311u: // v_pack_b32_f16
                    emit_v_assign(vd, "((" + sel16(a_u, 0) + " & 0xFFFFu) | ((" + sel16(b_u, 1) + " & 0xFFFFu) << 16u))");
                    ok = true; break;
                case 0x312u: emit_v_assign(vd, "wsx_cvt_pknorm_i16(" + sel16_f(a_u, 0) + ", " + sel16_f(b_u, 1) + ")"); ok = true; break;
                case 0x313u: emit_v_assign(vd, "wsx_cvt_pknorm_u16(" + sel16_f(a_u, 0) + ", " + sel16_f(b_u, 1) + ")"); ok = true; break;
                case 0x314u: write16(sel16(b_u, 1) + " << (" + sel16(a_u, 0) + " & 15u)"); ok = true; break; // v_lshlrev_b16
                case 0x319u: { // v_subrev_co_u32
                    const uint32_t sd = (d.w0 >> 8) & 0x7Fu;
                    line("if (lane_active) { uint _a = " + b_u + "; uint _b = " + a_u + "; vcc_lane = (_b > _a); " + vreg(vd) + " = _a - _b; }");
                    if (is_wave64_) emit_ballot64_assign("vcc_lane", sreg(sd), sreg_hi(sd));
                    else            line(sreg(sd) + " = WaveActiveBallot(vcc_lane).x;");
                    ok = true; break;
                }
                case 0x340u: write16(sel16(a_u, 0) + " * " + sel16(b_u, 1) + " + " + sel16(c_u, 2)); ok = true; break; // v_mad_u16
                case 0x342u: // v_interp_p1ll_f16 (outputs FP32 intermediate)
                case 0x343u: { // v_interp_p1lv_f16 (outputs FP32 intermediate)
                    const uint32_t attr = d.w1 & 0x3Fu;
                    const uint32_t chan = (d.w1 >> 6) & 3u;
                    emit_v_assign_f32(vd, ps_attr_expr(attr, chan), clamp_mod, omod);
                    ok = true; break;
                }
                case 0x344u: emit_v_assign(vd, "wsx_perm_b32(" + a_u + ", " + b_u + ", " + c_u + ")"); ok = true; break; // v_perm_b32
                case 0x345u: emit_v_assign(vd, "(((" + a_u + ") ^ (" + b_u + ")) + (" + c_u + "))"); ok = true; break; // v_xad_u32
                case 0x346u: emit_v_assign(vd, "(((" + a_u + ") << ((" + b_u + ") & 31u)) + (" + c_u + "))"); ok = true; break; // v_lshl_add_u32
                case 0x347u: emit_v_assign(vd, "(((" + a_u + ") + (" + b_u + ")) << ((" + c_u + ") & 31u))"); ok = true; break; // v_add_lshl_u32
                case 0x34Bu: { // v_fma_f16
                    std::string f = "mad(" + sel16_f(a_u, 0) + ", " + sel16_f(b_u, 1) + ", " + sel16_f(c_u, 2) + ")";
                    if (clamp_mod) f = "saturate(" + f + ")";
                    write16("f32tof16(" + f + ")");
                    ok = true; break;
                }
                case 0x351u: write16("f32tof16(min(min(" + sel16_f(a_u, 0) + ", " + sel16_f(b_u, 1) + "), " + sel16_f(c_u, 2) + "))"); ok = true; break;
                case 0x352u: write16("uint(min(min(" + sel16_i(a_u, 0) + ", " + sel16_i(b_u, 1) + "), " + sel16_i(c_u, 2) + "))"); ok = true; break;
                case 0x353u: write16("min(min(" + sel16(a_u, 0) + ", " + sel16(b_u, 1) + "), " + sel16(c_u, 2) + ")"); ok = true; break;
                case 0x354u: write16("f32tof16(max(max(" + sel16_f(a_u, 0) + ", " + sel16_f(b_u, 1) + "), " + sel16_f(c_u, 2) + "))"); ok = true; break;
                case 0x355u: write16("uint(max(max(" + sel16_i(a_u, 0) + ", " + sel16_i(b_u, 1) + "), " + sel16_i(c_u, 2) + "))"); ok = true; break;
                case 0x356u: write16("max(max(" + sel16(a_u, 0) + ", " + sel16(b_u, 1) + "), " + sel16(c_u, 2) + ")"); ok = true; break;
                case 0x357u: write16("f32tof16(wsx_med3_f(" + sel16_f(a_u, 0) + ", " + sel16_f(b_u, 1) + ", " + sel16_f(c_u, 2) + "))"); ok = true; break;
                case 0x358u: write16("wsx_med3_i(uint(" + sel16_i(a_u, 0) + "), uint(" + sel16_i(b_u, 1) + "), uint(" + sel16_i(c_u, 2) + "))"); ok = true; break;
                case 0x359u: write16("wsx_med3_u(" + sel16(a_u, 0) + ", " + sel16(b_u, 1) + ", " + sel16(c_u, 2) + ")"); ok = true; break;
                case 0x35Au: { // v_interp_p2_f16 (outputs FP16 into lo/hi 16 bits selected by opsel[3])
                    const uint32_t attr = d.w1 & 0x3Fu;
                    const uint32_t chan = (d.w1 >> 6) & 3u;
                    std::string f = ps_attr_expr(attr, chan);
                    if (clamp_mod) f = "saturate(" + f + ")";
                    write16("f32tof16(" + f + ")");
                    ok = true; break;
                }
                case 0x35Eu: write16("uint(" + sel16_i(a_u, 0) + " * " + sel16_i(b_u, 1) + " + " + sel16_i(c_u, 2) + ")"); ok = true; break; // v_mad_i16
                case 0x35Fu: write16("f32tof16((" + sel16_f(b_u, 1) + ") == 0.0f ? ((" + sel16_f(c_u, 2) + ") / (" + sel16_f(b_u, 1) + ")) : (" + sel16_f(a_u, 0) + "))"); ok = true; break; // v_div_fixup_f16
                case 0x360u: line(sreg(vd & 0x7Fu) + " = " + expr_readlane(a_u, b_u) + ";"); ok = true; break;
                case 0x361u: line("if (" + expr_lane_id() + " == ((" + b_u + ") & (" + expr_lane_count() + " - 1u))) { " + vreg(vd) + " = " + a_u + "; }"); ok = true; break;
                case 0x362u: emit_v_assign_f32(vd, "ldexp(" + a_f + ", float(int(" + b_u + ")))", clamp_mod, omod); ok = true; break; // v_ldexp_f32
                case 0x363u: emit_v_assign(vd, "(((1u << ((" + a_u + ") & 31u)) - 1u) << ((" + b_u + ") & 31u))"); ok = true; break; // v_bfm_b32
                case 0x364u: emit_v_assign(vd, "(countbits(" + a_u + ") + (" + b_u + "))"); ok = true; break; // v_bcnt_u32_b32
                case 0x365u: emit_v_assign(vd, "(countbits((" + a_u + ") & (" + expr_lane_id() + " < 32u ? ((1u << " + expr_lane_id() + ") - 1u) : 0xFFFFFFFFu)) + (" + b_u + "))"); ok = true; break; // v_mbcnt_lo_u32_b32
                case 0x366u: emit_v_assign(vd, "((" + expr_lane_id() + " >= 32u ? countbits((" + a_u + ") & ((1u << (" + expr_lane_id() + " - 32u)) - 1u)) : 0u) + (" + b_u + "))"); ok = true; break; // v_mbcnt_hi_u32_b32 (exact on Wave32, native Wave64 & emulated Wave64)
                case 0x368u: emit_v_assign(vd, "wsx_cvt_pknorm_i16(" + a_f + ", " + b_f + ")"); ok = true; break; // v_cvt_pknorm_i16_f32
                case 0x369u: emit_v_assign(vd, "wsx_cvt_pknorm_u16(" + a_f + ", " + b_f + ")"); ok = true; break; // v_cvt_pknorm_u16_f32
                case 0x36Au: emit_v_assign(vd, "(min(" + a_u + ", 65535u) | (min(" + b_u + ", 65535u) << 16u))"); ok = true; break; // v_cvt_pk_u16_u32
                case 0x36Bu: emit_v_assign(vd, "((uint(clamp(int(" + a_u + "), -32768, 32767)) & 0xFFFFu) | ((uint(clamp(int(" + b_u + "), -32768, 32767)) & 0xFFFFu) << 16u))"); ok = true; break; // v_cvt_pk_i16_i32
                case 0x36Du: emit_v_assign(vd, "((" + a_u + ") + (" + b_u + ") + (" + c_u + "))"); ok = true; break; // v_add3_u32
                case 0x36Fu: emit_v_assign(vd, "(((" + a_u + ") << ((" + b_u + ") & 31u)) | (" + c_u + "))"); ok = true; break; // v_lshl_or_b32
                case 0x371u: emit_v_assign(vd, "(((" + a_u + ") & (" + b_u + ")) | (" + c_u + "))"); ok = true; break; // v_and_or_b32
                case 0x372u: emit_v_assign(vd, "((" + a_u + ") | (" + b_u + ") | (" + c_u + "))"); ok = true; break; // v_or3_b32
                case 0x373u: emit_v_assign(vd, "(" + sel16(a_u, 0) + " * " + sel16(b_u, 1) + " + (" + c_u + "))"); ok = true; break; // v_mad_u32_u16
                case 0x375u: emit_v_assign(vd, "uint(" + sel16_i(a_u, 0) + " * " + sel16_i(b_u, 1) + " + int(" + c_u + "))"); ok = true; break; // v_mad_i32_i16
                case 0x377u: emit_v_assign(vd, "wsx_permlane16(" + a_u + ", " + b_u + ", " + c_u + ", 0u)"); ok = true; break; // v_permlane16_b32
                case 0x378u: emit_v_assign(vd, "wsx_permlane16(" + a_u + ", " + b_u + ", " + c_u + ", 1u)"); ok = true; break; // v_permlanex16_b32
                default:
                    break;
                }
            }
            break;
        }
        case Enc::VOP3P: {
            const uint32_t vd = d.w0 & 0xFFu;
            const uint32_t neg_hi = (d.w0 >> 8) & 7u;
            const uint32_t opsel_lo = (d.w0 >> 11) & 7u;
            const uint32_t opsel_hi = (((d.w1 >> 27) & 3u) | (((d.w0 >> 14) & 1u) << 2u));
            const bool clamp_mod = ((d.w0 >> 15) & 1u) != 0u;
            const uint32_t s0 = d.w1 & 0x1FFu;
            const uint32_t s1 = (d.w1 >> 9) & 0x1FFu;
            const uint32_t s2 = (d.w1 >> 18) & 0x1FFu;
            const uint32_t neg_lo = (d.w1 >> 29) & 7u;

            const std::string au = operand_u32(s0, d.lit);
            const std::string bu = operand_u32(s1, d.lit);
            const std::string cu = operand_u32(s2, d.lit);

            auto mix_f = [&](const std::string& u, uint32_t k) -> std::string {
                std::string e;
                if (opsel_hi & (1u << k)) {
                    // FP16 input (opsel_lo[k] selects hi vs lo 16-bit half)
                    e = (opsel_lo & (1u << k)) ? ("f16tof32((" + u + ") >> 16u)") : ("f16tof32(" + u + ")");
                } else {
                    e = "asfloat(" + u + ")";
                }
                if (neg_hi & (1u << k)) e = "abs(" + e + ")"; // neg_hi = abs for v_fma_mix_*
                if (neg_lo & (1u << k)) e = "(-(" + e + "))";
                return e;
            };
            auto pk_u16 = [&](const std::string& u, uint32_t k, bool hi_lane) -> std::string {
                const bool take_hi = hi_lane ? ((opsel_hi & (1u << k)) != 0) : ((opsel_lo & (1u << k)) != 0);
                return take_hi ? ("((" + u + ") >> 16u)") : ("((" + u + ") & 0xFFFFu)");
            };
            auto pk_i16 = [&](const std::string& u, uint32_t k, bool hi_lane) -> std::string {
                const bool take_hi = hi_lane ? ((opsel_hi & (1u << k)) != 0) : ((opsel_lo & (1u << k)) != 0);
                return take_hi ? ("(int(" + u + ") >> 16)") : ("((int(" + u + ") << 16) >> 16)");
            };
            auto pk_f16 = [&](const std::string& u, uint32_t k, bool hi_lane) -> std::string {
                std::string e = "f16tof32(" + pk_u16(u, k, hi_lane) + ")";
                const bool neg = hi_lane ? ((neg_hi & (1u << k)) != 0) : ((neg_lo & (1u << k)) != 0);
                if (neg) e = "(-(" + e + "))";
                return e;
            };
            auto emit_pk_u16 = [&](const std::string& lo_e, const std::string& hi_e) {
                emit_v_assign(vd, "(((" + lo_e + ") & 0xFFFFu) | (((" + hi_e + ") & 0xFFFFu) << 16u))");
            };
            auto pk_f16_h2 = [&](const std::string& u, uint32_t k) -> std::string {
                std::string lo = "asfloat16(uint16_t(" + pk_u16(u, k, false) + "))";
                std::string hi = "asfloat16(uint16_t(" + pk_u16(u, k, true) + "))";
                if ((neg_lo & (1u << k)) != 0) lo = "(-(" + lo + "))";
                if ((neg_hi & (1u << k)) != 0) hi = "(-(" + hi + "))";
                return "float16_t2(" + lo + ", " + hi + ")";
            };
            auto emit_pk_f16_native = [&](const std::string& h2_expr) {
                const std::string clamped = clamp_mod ? ("saturate(" + h2_expr + ")") : h2_expr;
                emit_v_assign(vd, "wsx_pack_f16x2(" + clamped + ")");
            };
            auto emit_pk_f16 = [&](const std::string& lo_f, const std::string& hi_f) {
                const std::string lf = clamp_mod ? ("saturate(" + lo_f + ")") : lo_f;
                const std::string hf = clamp_mod ? ("saturate(" + hi_f + ")") : hi_f;
                emit_v_assign(vd, "((f32tof16(" + lf + ") & 0xFFFFu) | ((f32tof16(" + hf + ") & 0xFFFFu) << 16u))");
            };

            switch (d.op) {
            case 0x00u: emit_pk_u16("uint(" + pk_i16(au,0,false) + " * " + pk_i16(bu,1,false) + " + " + pk_i16(cu,2,false) + ")",
                                    "uint(" + pk_i16(au,0,true)  + " * " + pk_i16(bu,1,true)  + " + " + pk_i16(cu,2,true)  + ")"); ok = true; break;
            case 0x01u: emit_pk_u16(pk_u16(au,0,false) + " * " + pk_u16(bu,1,false), pk_u16(au,0,true) + " * " + pk_u16(bu,1,true)); ok = true; break;
            case 0x02u: emit_pk_u16("uint(" + pk_i16(au,0,false) + " + " + pk_i16(bu,1,false) + ")", "uint(" + pk_i16(au,0,true) + " + " + pk_i16(bu,1,true) + ")"); ok = true; break;
            case 0x03u: emit_pk_u16("uint(" + pk_i16(au,0,false) + " - " + pk_i16(bu,1,false) + ")", "uint(" + pk_i16(au,0,true) + " - " + pk_i16(bu,1,true) + ")"); ok = true; break;
            case 0x04u: emit_pk_u16("(" + pk_u16(bu,1,false) + " << (" + pk_u16(au,0,false) + " & 15u))", "(" + pk_u16(bu,1,true) + " << (" + pk_u16(au,0,true) + " & 15u))"); ok = true; break;
            case 0x05u: emit_pk_u16("(" + pk_u16(bu,1,false) + " >> (" + pk_u16(au,0,false) + " & 15u))", "(" + pk_u16(bu,1,true) + " >> (" + pk_u16(au,0,true) + " & 15u))"); ok = true; break;
            case 0x06u: emit_pk_u16("uint(" + pk_i16(bu,1,false) + " >> (" + pk_u16(au,0,false) + " & 15u))", "uint(" + pk_i16(bu,1,true) + " >> (" + pk_u16(au,0,true) + " & 15u))"); ok = true; break;
            case 0x07u: emit_pk_u16("uint(max(" + pk_i16(au,0,false) + ", " + pk_i16(bu,1,false) + "))", "uint(max(" + pk_i16(au,0,true) + ", " + pk_i16(bu,1,true) + "))"); ok = true; break;
            case 0x08u: emit_pk_u16("uint(min(" + pk_i16(au,0,false) + ", " + pk_i16(bu,1,false) + "))", "uint(min(" + pk_i16(au,0,true) + ", " + pk_i16(bu,1,true) + "))"); ok = true; break;
            case 0x09u: emit_pk_u16("(" + pk_u16(au,0,false) + " * " + pk_u16(bu,1,false) + " + " + pk_u16(cu,2,false) + ")",
                                    "(" + pk_u16(au,0,true)  + " * " + pk_u16(bu,1,true)  + " + " + pk_u16(cu,2,true)  + ")"); ok = true; break;
            case 0x0Au: emit_pk_u16("(" + pk_u16(au,0,false) + " + " + pk_u16(bu,1,false) + ")", "(" + pk_u16(au,0,true) + " + " + pk_u16(bu,1,true) + ")"); ok = true; break;
            case 0x0Bu: emit_pk_u16("(" + pk_u16(au,0,false) + " - " + pk_u16(bu,1,false) + ")", "(" + pk_u16(au,0,true) + " - " + pk_u16(bu,1,true) + ")"); ok = true; break;
            case 0x0Cu: emit_pk_u16("max(" + pk_u16(au,0,false) + ", " + pk_u16(bu,1,false) + ")", "max(" + pk_u16(au,0,true) + ", " + pk_u16(bu,1,true) + ")"); ok = true; break;
            case 0x0Du: emit_pk_u16("min(" + pk_u16(au,0,false) + ", " + pk_u16(bu,1,false) + ")", "min(" + pk_u16(au,0,true) + ", " + pk_u16(bu,1,true) + ")"); ok = true; break;
            case 0x0Eu:
                if (use_native_fp16_) emit_pk_f16_native("mad(" + pk_f16_h2(au,0) + ", " + pk_f16_h2(bu,1) + ", " + pk_f16_h2(cu,2) + ")");
                else emit_pk_f16("mad(" + pk_f16(au,0,false) + ", " + pk_f16(bu,1,false) + ", " + pk_f16(cu,2,false) + ")",
                                 "mad(" + pk_f16(au,0,true)  + ", " + pk_f16(bu,1,true)  + ", " + pk_f16(cu,2,true)  + ")");
                ok = true; break;
            case 0x0Fu:
                if (use_native_fp16_) emit_pk_f16_native("(" + pk_f16_h2(au,0) + " + " + pk_f16_h2(bu,1) + ")");
                else emit_pk_f16("(" + pk_f16(au,0,false) + " + " + pk_f16(bu,1,false) + ")", "(" + pk_f16(au,0,true) + " + " + pk_f16(bu,1,true) + ")");
                ok = true; break;
            case 0x10u:
                if (use_native_fp16_) emit_pk_f16_native("(" + pk_f16_h2(au,0) + " * " + pk_f16_h2(bu,1) + ")");
                else emit_pk_f16("(" + pk_f16(au,0,false) + " * " + pk_f16(bu,1,false) + ")", "(" + pk_f16(au,0,true) + " * " + pk_f16(bu,1,true) + ")");
                ok = true; break;
            case 0x11u:
                if (use_native_fp16_) emit_pk_f16_native("min(" + pk_f16_h2(au,0) + ", " + pk_f16_h2(bu,1) + ")");
                else emit_pk_f16("min(" + pk_f16(au,0,false) + ", " + pk_f16(bu,1,false) + ")", "min(" + pk_f16(au,0,true) + ", " + pk_f16(bu,1,true) + ")");
                ok = true; break;
            case 0x12u:
                if (use_native_fp16_) emit_pk_f16_native("max(" + pk_f16_h2(au,0) + ", " + pk_f16_h2(bu,1) + ")");
                else emit_pk_f16("max(" + pk_f16(au,0,false) + ", " + pk_f16(bu,1,false) + ")", "max(" + pk_f16(au,0,true) + ", " + pk_f16(bu,1,true) + ")");
                ok = true; break;
            case 0x13u: // v_dot2_f32_f16
                emit_v_assign_f32(vd, "(" + pk_f16(au,0,false) + " * " + pk_f16(bu,1,false) + " + " + pk_f16(au,0,true) + " * " + pk_f16(bu,1,true) + " + asfloat(" + cu + "))", clamp_mod);
                ok = true; break;
            case 0x14u: // v_dot2_i32_i16
                emit_v_assign(vd, "uint(" + pk_i16(au,0,false) + " * " + pk_i16(bu,1,false) + " + " + pk_i16(au,0,true) + " * " + pk_i16(bu,1,true) + " + int(" + cu + "))");
                ok = true; break;
            case 0x15u: // v_dot2_u32_u16
                emit_v_assign(vd, "(" + pk_u16(au,0,false) + " * " + pk_u16(bu,1,false) + " + " + pk_u16(au,0,true) + " * " + pk_u16(bu,1,true) + " + (" + cu + "))");
                ok = true; break;
            case 0x16u: // v_dot4_i32_i8
                line("if (lane_active) { int _acc = int(" + cu + "); [unroll] for (uint _k = 0u; _k < 4u; ++_k) { int _a8 = (int(((" + au + ") >> (_k*8u)) & 0xFFu) << 24) >> 24; int _b8 = (int(((" + bu + ") >> (_k*8u)) & 0xFFu) << 24) >> 24; _acc += _a8 * _b8; } " + vreg(vd) + " = uint(_acc); }");
                ok = true; break;
            case 0x17u: // v_dot4_u32_u8
                line("if (lane_active) { uint _acc = " + cu + "; [unroll] for (uint _k = 0u; _k < 4u; ++_k) { _acc += (((" + au + ") >> (_k*8u)) & 0xFFu) * (((" + bu + ") >> (_k*8u)) & 0xFFu); } " + vreg(vd) + " = _acc; }");
                ok = true; break;
            case 0x18u: // v_dot8_i32_i4
                line("if (lane_active) { int _acc = int(" + cu + "); [unroll] for (uint _k = 0u; _k < 8u; ++_k) { int _a4 = (int(((" + au + ") >> (_k*4u)) & 0xFu) << 28) >> 28; int _b4 = (int(((" + bu + ") >> (_k*4u)) & 0xFu) << 28) >> 28; _acc += _a4 * _b4; } " + vreg(vd) + " = uint(_acc); }");
                ok = true; break;
            case 0x19u: // v_dot8_u32_u4
                line("if (lane_active) { uint _acc = " + cu + "; [unroll] for (uint _k = 0u; _k < 8u; ++_k) { _acc += (((" + au + ") >> (_k*4u)) & 0xFu) * (((" + bu + ") >> (_k*4u)) & 0xFu); } " + vreg(vd) + " = _acc; }");
                ok = true; break;
            case 0x20u: // v_fma_mix_f32
                emit_v_assign_f32(vd, "mad(" + mix_f(au, 0) + ", " + mix_f(bu, 1) + ", " + mix_f(cu, 2) + ")", clamp_mod);
                ok = true; break;
            case 0x21u: // v_fma_mixlo_f16
                emit_v_assign_f16_lo(vd, "mad(" + mix_f(au, 0) + ", " + mix_f(bu, 1) + ", " + mix_f(cu, 2) + ")", clamp_mod);
                ok = true; break;
            case 0x22u: { // v_fma_mixhi_f16
                std::string r = "mad(" + mix_f(au, 0) + ", " + mix_f(bu, 1) + ", " + mix_f(cu, 2) + ")";
                if (clamp_mod) r = "saturate(" + r + ")";
                emit_v_assign(vd, "((" + vreg(vd) + " & 0x0000FFFFu) | ((f32tof16(" + r + ") & 0xFFFFu) << 16u))");
                ok = true; break;
            }
            default:
                break;
            }
            break;
        }
        case Enc::SMEM: {
            const uint32_t sbase = (d.w0 & 0x3Fu) << 1u;
            const uint32_t sdata = (d.w0 >> 6) & 0x7Fu;
            const uint32_t soff = (d.w1 >> 25) & 0x7Fu;
            int32_t imm_off = static_cast<int32_t>(d.w1 & 0x1FFFFFu);
            if (imm_off & 0x100000) imm_off |= ~0x1FFFFF;

            const bool glc = ((d.w0 >> 16) & 1u) != 0u;
            const bool is_raw_load = (d.op <= 0x04u);
            const bool is_scr_load = (d.op >= 0x05u && d.op <= 0x07u);
            const bool is_buf_load = (d.op >= 0x08u && d.op <= 0x0Cu);
            const bool is_raw_store = (d.op >= 0x10u && d.op <= 0x12u);
            const bool is_scr_store = (d.op >= 0x15u && d.op <= 0x17u);
            const bool is_buf_store = (d.op >= 0x18u && d.op <= 0x1Au);
            const bool is_raw_atomic = (d.op >= 0x40u && d.op <= 0x5Cu);
            const bool is_buf_atomic = (d.op >= 0x60u && d.op <= 0x7Cu);
            // Hardware-verified RDNA2 SMEM pre-truncation rule (Leonida RX 6800 gfx1030 measurement):
            // Each term (base, sOFF, IMM) has its low 2 bits cleared BEFORE addition:
            //   addr_i = (base & ~3) + (sOFF & ~3) + (IMM & ~3) + 4*i
            const int32_t imm_aligned = imm_off & ~3;
            auto make_smem_va = [&](bool is_buf) -> std::string {
                std::string va = "(" + (is_buf ? vsharp_va_from_s(sbase) : u64_from_s(sbase)) + " & ~3ull)";
                if (soff != 0x7Du) va = "(" + va + " + uint64_t((" + operand_u32(soff, 0u) + ") & ~3u))";
                if (imm_aligned != 0) va = "(" + va + " + uint64_t(int64_t(" + std::to_string(imm_aligned) + ")))";
                return va;
            };

            if (is_raw_load || is_buf_load) {
                uses_slabs_ = true;
                const uint32_t sub = is_buf_load ? (d.op - 0x08u) : d.op;
                const uint32_t ndw = 1u << sub;
                const std::string va_expr = make_smem_va(is_buf_load);

                line("{ uint64_t _va = " + va_expr + ";");
                for (uint32_t k = 0; k < ndw; ++k) {
                    if (sdata + k < 106u) {
                        line("  " + sreg(sdata + k) + " = wsx_va_load32(_va + " + std::to_string(k * 4u) + "ull);");
                    }
                }
                line("}");
                ok = true;
            } else if (is_scr_load) {
                uses_scratch_ = true;
                const uint32_t ndw = 1u << (d.op - 0x05u);
                std::string idx = "uint(" + std::to_string(imm_aligned) + ")";
                if (soff != 0x7Du) idx = "(((" + operand_u32(soff, 0u) + ") & ~3u) + " + idx + ")";
                line("{ uint _si = (" + idx + ") >> 2u;");
                for (uint32_t k = 0; k < ndw; ++k) {
                    if (sdata + k < 106u) line("  " + sreg(sdata + k) + " = wsx_scratch[(_si + " + std::to_string(k) + "u) & 255u];");
                }
                line("}");
                ok = true;
            } else if (is_raw_store || is_buf_store) {
                uses_slabs_ = true;
                const uint32_t sub = is_buf_store ? (d.op - 0x18u) : (d.op - 0x10u);
                const uint32_t ndw = 1u << sub;
                const std::string va_expr = make_smem_va(is_buf_store);
                line("if (" + expr_is_first_lane() + ") { uint64_t _va = " + va_expr + ";");
                for (uint32_t k = 0; k < ndw; ++k) {
                    if (sdata + k < 106u) {
                        line("  wsx_va_store32(_va + " + std::to_string(k * 4u) + "ull, " + sreg(sdata + k) + ");");
                    }
                }
                line("}");
                ok = true;
            } else if (is_scr_store) {
                uses_scratch_ = true;
                const uint32_t ndw = 1u << (d.op - 0x15u);
                std::string idx = "uint(" + std::to_string(imm_aligned) + ")";
                if (soff != 0x7Du) idx = "(((" + operand_u32(soff, 0u) + ") & ~3u) + " + idx + ")";
                line("{ uint _si = (" + idx + ") >> 2u;");
                for (uint32_t k = 0; k < ndw; ++k) {
                    if (sdata + k < 106u) line("  wsx_scratch[(_si + " + std::to_string(k) + "u) & 255u] = " + sreg(sdata + k) + ";");
                }
                line("}");
                ok = true;
            } else if (is_raw_atomic || is_buf_atomic) {
                uses_slabs_ = true;
                const uint32_t sub = is_buf_atomic ? (d.op - 0x60u) : (d.op - 0x40u);
                const bool is64 = (sub >= 0x10u);
                const uint32_t aop = is64 ? (sub - 0x10u) : sub;
                // Map GFX10.3 SMEM atomic index (0..12) directly to wsx_va_atomic_op32 / op64 kind
                if (aop <= 12u) {
                    const std::string va_expr = make_smem_va(is_buf_atomic);
                    if (!is64) {
                        const std::string cmp = (aop == 1u) ? sreg(sdata + 1u) : "0u";
                        const std::string bcast = use_w64_emu()
                            ? "wsx_w64_readlane(_so, 0u, wsx_gi)"
                            : "WaveReadLaneFirst(_so)";
                        line("{ uint _so = 0u; if (" + expr_is_first_lane() + ") { _so = wsx_va_atomic_op32(" + va_expr + ", " +
                             std::to_string(aop) + "u, " + sreg(sdata) + ", " + cmp + "); }" +
                             (glc ? (" " + sreg(sdata) + " = " + bcast + ";") : "") + " }");
                    } else {
                        const std::string val64 = "((uint64_t(" + sreg(sdata + 1u) + ") << 32) | uint64_t(" + sreg(sdata) + "))";
                        const std::string cmp64 = (aop == 1u)
                            ? ("((uint64_t(" + sreg(sdata + 3u) + ") << 32) | uint64_t(" + sreg(sdata + 2u) + "))")
                            : "0ull";
                        const std::string bcast_lo = use_w64_emu()
                            ? "wsx_w64_readlane(_slo, 0u, wsx_gi)"
                            : "WaveReadLaneFirst(_slo)";
                        const std::string bcast_hi = use_w64_emu()
                            ? "wsx_w64_readlane(_shi, 0u, wsx_gi)"
                            : "WaveReadLaneFirst(_shi)";
                        line("{ uint _slo = 0u, _shi = 0u; if (" + expr_is_first_lane() + ") { uint64_t _o64 = wsx_va_atomic_op64(" + va_expr + ", " +
                             std::to_string(aop) + "u, " + val64 + ", " + cmp64 + "); _slo = uint(_o64 & 0xFFFFFFFFull); _shi = uint(_o64 >> 32); }" +
                             (glc ? (" " + sreg(sdata) + " = " + bcast_lo + "; " + sreg(sdata + 1u) + " = " + bcast_hi + ";") : "") + " }");
                    }
                    ok = true;
                }
            } else if (d.op == 0x1Fu || d.op == 0x20u || d.op == 0x21u || d.op == 0x26u || d.op == 0x27u) { // cache inv/wb / s_atc_probe*
                ok = true;
            } else if (d.op == 0x24u || d.op == 0x25u) { // s_memtime / s_memrealtime
                line(sreg(sdata) + " = 0u; " + sreg_hi(sdata) + " = 0u;");
                ok = true;
            } else if (d.op == 0x2Au) { // s_get_waveid_in_workgroup
                if (stage_has_workgroup()) {
                    line(sreg(sdata) + " = (wsx_gi >> " + std::string(is_wave64_ ? "6u" : "5u") + ");");
                } else {
                    line(sreg(sdata) + " = 0u;");
                }
                ok = true;
            }
            break;
        }
        case Enc::MUBUF:
        case Enc::MTBUF: {
            const uint32_t imm_off = d.w0 & 0xFFFu;
            const bool offen = ((d.w0 >> 12) & 1u) != 0u;
            const bool idxen = ((d.w0 >> 13) & 1u) != 0u;
            const bool glc = ((d.w0 >> 14) & 1u) != 0u;
            const uint32_t vaddr = d.w1 & 0xFFu;
            const uint32_t vdata = (d.w1 >> 8) & 0xFFu;
            const uint32_t srsrc = ((d.w1 >> 16) & 0x1Fu) << 2u;
            const uint32_t soff = (d.w1 >> 24) & 0xFFu;

            auto make_va = [&](uint32_t elem_bytes) {
                std::string va = vsharp_va_from_s(srsrc);
                if (idxen && offen) {
                    va += " + uint64_t(" + vreg(vaddr) + ") * uint64_t(max(" + vsharp_stride_from_s(srsrc) + ", " + std::to_string(elem_bytes) + "u)) + uint64_t(" + vreg(vaddr + 1u) + ")";
                } else if (idxen) {
                    va += " + uint64_t(" + vreg(vaddr) + ") * uint64_t(max(" + vsharp_stride_from_s(srsrc) + ", " + std::to_string(elem_bytes) + "u))";
                } else if (offen) {
                    va += " + uint64_t(" + vreg(vaddr) + ")";
                }
                if (soff != 0x80u && soff != 0x7Du) va += " + uint64_t(" + operand_u32(soff, 0u) + ")";
                if (imm_off != 0u) va += " + " + std::to_string(imm_off) + "ull";
                return va;
            };

            if (d.enc == Enc::MTBUF) {
                // MTBUF: 0..3 = load_format x/xy/xyz/xyzw, 4..7 = store_format, 8..B = load_d16, C..F = store_d16
                const uint32_t comps = (d.op & 3u) + 1u;
                const bool is_store = ((d.op & 4u) != 0u);
                const bool is_d16 = ((d.op & 8u) != 0u);
                const uint32_t ndw = is_d16 ? ((comps + 1u) / 2u) : comps;
                uses_slabs_ = true;
                line("if (lane_active) { uint64_t _va = " + make_va(ndw * 4u) + ";");
                if (!is_store) {
                    for (uint32_t k = 0; k < ndw; ++k) {
                        line("  " + vreg(vdata + k) + " = wsx_va_load32(_va + " + std::to_string(k * 4u) + "ull);");
                    }
                } else {
                    for (uint32_t k = 0; k < ndw; ++k) {
                        line("  wsx_va_store32(_va + " + std::to_string(k * 4u) + "ull, " + vreg(vdata + k) + ");");
                    }
                }
                line("}");
                ok = true;
                break;
            }

            // MUBUF format loads/stores (0x00..0x07, 0x26..0x27, 0x80..0x87)
            if (d.op <= 0x07u || d.op == 0x26u || d.op == 0x27u || (d.op >= 0x80u && d.op <= 0x87u)) {
                uses_slabs_ = true;
                const bool is_d16 = (d.op >= 0x26u);
                const bool is_store = (d.op >= 0x04u && d.op <= 0x07u) || d.op == 0x27u || (d.op >= 0x84u && d.op <= 0x87u);
                const uint32_t comps = (d.op == 0x26u || d.op == 0x27u) ? 1u : ((d.op & 3u) + 1u);
                const uint32_t ndw = is_d16 ? ((comps + 1u) / 2u) : comps;
                line("if (lane_active) { uint64_t _va = " + make_va(ndw * 4u) + ";");
                if (!is_store) {
                    for (uint32_t k = 0; k < ndw; ++k) {
                        line("  " + vreg(vdata + k) + " = wsx_va_load32(_va + " + std::to_string(k * 4u) + "ull);");
                    }
                } else {
                    for (uint32_t k = 0; k < ndw; ++k) {
                        line("  wsx_va_store32(_va + " + std::to_string(k * 4u) + "ull, " + vreg(vdata + k) + ");");
                    }
                }
                line("}");
                ok = true;
                break;
            }

            // Sub-dword MUBUF loads/stores (0x08..0x0B, 0x18..0x1B, 0x20..0x25)
            if (d.op >= 0x08u && d.op <= 0x0Bu) {
                uses_slabs_ = true;
                const char* fn = (d.op == 0x08u) ? "wsx_va_load_u8" : (d.op == 0x09u) ? "wsx_va_load_i8"
                               : (d.op == 0x0Au) ? "wsx_va_load_u16" : "wsx_va_load_i16";
                const uint32_t eb = (d.op <= 0x09u) ? 1u : 2u;
                emit_v_assign(vdata, std::string(fn) + "(" + make_va(eb) + ")");
                ok = true; break;
            }
            if (d.op >= 0x18u && d.op <= 0x1Bu) {
                uses_slabs_ = true;
                const bool is_16 = (d.op >= 0x1Au);
                const bool is_hi = (d.op == 0x19u || d.op == 0x1Bu);
                const std::string val = is_hi ? ("(" + vreg(vdata) + " >> 16u)") : vreg(vdata);
                line("if (lane_active) { " + std::string(is_16 ? "wsx_va_store_u16(" : "wsx_va_store_u8(") +
                     make_va(is_16 ? 2u : 1u) + ", " + val + "); }");
                ok = true; break;
            }
            if (d.op >= 0x20u && d.op <= 0x25u) {
                uses_slabs_ = true;
                const bool is_hi = ((d.op & 1u) != 0u);
                const char* fn = (d.op <= 0x21u) ? "wsx_va_load_u8" : (d.op <= 0x23u) ? "wsx_va_load_i8" : "wsx_va_load_u16";
                const uint32_t eb = (d.op <= 0x23u) ? 1u : 2u;
                if (is_hi) emit_v_assign(vdata, "((" + vreg(vdata) + " & 0xFFFFu) | ((" + std::string(fn) + "(" + make_va(eb) + ") & 0xFFFFu) << 16u))");
                else       emit_v_assign(vdata, "((" + vreg(vdata) + " & 0xFFFF0000u) | (" + std::string(fn) + "(" + make_va(eb) + ") & 0xFFFFu))");
                ok = true; break;
            }

            // Dword MUBUF loads/stores: 0x0C=x1, 0x0D=x2, 0x0E=x4, 0x0F=x3 | 0x1C=x1, 0x1D=x2, 0x1E=x4, 0x1F=x3
            uint32_t load_dw = 0, store_dw = 0;
            if (d.op == 0x0Cu) load_dw = 1;
            else if (d.op == 0x0Du) load_dw = 2;
            else if (d.op == 0x0Eu) load_dw = 4;
            else if (d.op == 0x0Fu) load_dw = 3;
            else if (d.op == 0x1Cu) store_dw = 1;
            else if (d.op == 0x1Du) store_dw = 2;
            else if (d.op == 0x1Eu) store_dw = 4;
            else if (d.op == 0x1Fu) store_dw = 3;

            if (load_dw > 0u) {
                uses_slabs_ = true;
                line("if (lane_active) { uint64_t _va = " + make_va(load_dw * 4u) + ";");
                for (uint32_t k = 0; k < load_dw; ++k) {
                    line("  " + vreg(vdata + k) + " = wsx_va_load32(_va + " + std::to_string(k * 4u) + "ull);");
                }
                line("}");
                ok = true;
            } else if (store_dw > 0u) {
                uses_slabs_ = true;
                line("if (lane_active) { uint64_t _va = " + make_va(store_dw * 4u) + ";");
                if (store_dw == 4u) {
                    line("  wsx_va_store128(_va, uint4(" + vreg(vdata) + ", " + vreg(vdata + 1u) + ", " +
                         vreg(vdata + 2u) + ", " + vreg(vdata + 3u) + "));");
                } else {
                    for (uint32_t k = 0; k < store_dw; ++k) {
                        line("  wsx_va_store32(_va + " + std::to_string(k * 4u) + "ull, " + vreg(vdata + k) + ");");
                    }
                }
                line("}");
                ok = true;
            } else if ((d.op >= 0x30u && d.op <= 0x43u) || d.op == 0x4Du) {
                ok = emit_va_atomic32(d.op, make_va(4u), vdata, vdata, glc);
            } else if (d.op >= 0x50u && d.op <= 0x60u) {
                ok = emit_va_atomic64(d.op, make_va(8u), vdata, vdata, glc);
            } else if (d.op == 0x70u || d.op == 0x71u || d.op == 0x72u || d.op == 0x73u) { // buffer_wbinvl1 / gl0_inv / gl1_inv
                ok = true;
            }
            break;
        }
        case Enc::GLOBAL:
        case Enc::FLAT:
        case Enc::SCRATCH: {
            int32_t imm_off = static_cast<int32_t>(d.w0 & 0xFFFu);
            if (d.enc != Enc::FLAT && (imm_off & 0x800)) imm_off |= ~0xFFF;
            const bool glc = ((d.w0 >> 16) & 1u) != 0u;
            const uint32_t vaddr = d.w1 & 0xFFu;
            const uint32_t vdata = (d.w1 >> 8) & 0xFFu;
            const uint32_t saddr = (d.w1 >> 16) & 0x7Fu;
            const uint32_t vdst = (d.w1 >> 24) & 0xFFu;

            if (d.enc == Enc::SCRATCH) {
                uses_scratch_ = true;
                std::string idx = "(";
                if (saddr != 0x7Fu) idx += sreg(saddr) + " + ";
                idx += vreg(vaddr);
                if (imm_off != 0) idx += " + uint(" + std::to_string(imm_off) + ")";
                idx += ")";
                if (d.op >= 0x08u && d.op <= 0x0Fu) {
                    const uint32_t ndw = (d.op == 0x0Du) ? 2u : (d.op == 0x0Eu) ? 4u : (d.op == 0x0Fu) ? 3u : 1u;
                    line("if (lane_active) { uint _si = (" + idx + ") >> 2u;");
                    for (uint32_t k = 0; k < ndw; ++k) {
                        line("  " + vreg(vdst + k) + " = wsx_scratch[(_si + " + std::to_string(k) + "u) & 255u];");
                    }
                    line("}");
                    ok = true;
                } else if (d.op >= 0x18u && d.op <= 0x1Fu) {
                    const uint32_t ndw = (d.op == 0x1Du) ? 2u : (d.op == 0x1Eu) ? 4u : (d.op == 0x1Fu) ? 3u : 1u;
                    line("if (lane_active) { uint _si = (" + idx + ") >> 2u;");
                    for (uint32_t k = 0; k < ndw; ++k) {
                        line("  wsx_scratch[(_si + " + std::to_string(k) + "u) & 255u] = " + vreg(vdata + k) + ";");
                    }
                    line("}");
                    ok = true;
                } else if (d.op >= 0x20u && d.op <= 0x25u) {
                    line("if (lane_active) { " + vreg(vdst) + " = wsx_scratch[((" + idx + ") >> 2u) & 255u]; }");
                    ok = true;
                }
                break;
            }

            auto make_va = [&]() {
                std::string va;
                if (d.enc == Enc::GLOBAL && saddr != 0x7Fu) {
                    va = u64_from_s(saddr) + " + uint64_t(" + vreg(vaddr) + ")";
                } else {
                    va = "((uint64_t(" + vreg(vaddr + 1u) + ") << 32) | uint64_t(" + vreg(vaddr) + "))";
                }
                if (imm_off != 0) va += " + uint64_t(int64_t(" + std::to_string(imm_off) + "))";
                return va;
            };

            if (d.op >= 0x08u && d.op <= 0x0Bu) {
                uses_slabs_ = true;
                const char* fn = (d.op == 0x08u) ? "wsx_va_load_u8" : (d.op == 0x09u) ? "wsx_va_load_i8"
                               : (d.op == 0x0Au) ? "wsx_va_load_u16" : "wsx_va_load_i16";
                emit_v_assign(vdst, std::string(fn) + "(" + make_va() + ")");
                ok = true; break;
            }
            if (d.op >= 0x18u && d.op <= 0x1Bu) {
                uses_slabs_ = true;
                const bool is_16 = (d.op >= 0x1Au);
                const bool is_hi = (d.op == 0x19u || d.op == 0x1Bu);
                const std::string val = is_hi ? ("(" + vreg(vdata) + " >> 16u)") : vreg(vdata);
                line("if (lane_active) { " + std::string(is_16 ? "wsx_va_store_u16(" : "wsx_va_store_u8(") +
                     make_va() + ", " + val + "); }");
                ok = true; break;
            }
            if (d.op >= 0x20u && d.op <= 0x25u) {
                uses_slabs_ = true;
                const bool is_hi = ((d.op & 1u) != 0u);
                const char* fn = (d.op <= 0x21u) ? "wsx_va_load_u8" : (d.op <= 0x23u) ? "wsx_va_load_i8" : "wsx_va_load_u16";
                if (is_hi) emit_v_assign(vdst, "((" + vreg(vdst) + " & 0xFFFFu) | ((" + std::string(fn) + "(" + make_va() + ") & 0xFFFFu) << 16u))");
                else       emit_v_assign(vdst, "((" + vreg(vdst) + " & 0xFFFF0000u) | (" + std::string(fn) + "(" + make_va() + ") & 0xFFFFu))");
                ok = true; break;
            }
            if (d.op == 0x16u) { // global_load_dword_addtid
                uses_slabs_ = true;
                emit_v_assign(vdst, "wsx_va_load32(" + make_va() + " + uint64_t(" + expr_lane_id() + " * 4u))");
                ok = true; break;
            }
            if (d.op == 0x17u) { // global_store_dword_addtid
                uses_slabs_ = true;
                line("if (lane_active) { wsx_va_store32(" + make_va() + " + uint64_t(" + expr_lane_id() + " * 4u), " + vreg(vdata) + "); }");
                ok = true; break;
            }

            uint32_t load_dw = 0, store_dw = 0;
            if (d.op == 0x0Cu) load_dw = 1;
            else if (d.op == 0x0Du) load_dw = 2;
            else if (d.op == 0x0Eu) load_dw = 4;
            else if (d.op == 0x0Fu) load_dw = 3;
            else if (d.op == 0x1Cu) store_dw = 1;
            else if (d.op == 0x1Du) store_dw = 2;
            else if (d.op == 0x1Eu) store_dw = 4;
            else if (d.op == 0x1Fu) store_dw = 3;

            if (load_dw > 0u) {
                uses_slabs_ = true;
                line("if (lane_active) { uint64_t _va = " + make_va() + ";");
                for (uint32_t k = 0; k < load_dw; ++k) {
                    line("  " + vreg(vdst + k) + " = wsx_va_load32(_va + " + std::to_string(k * 4u) + "ull);");
                }
                line("}");
                ok = true;
            } else if (store_dw > 0u) {
                uses_slabs_ = true;
                line("if (lane_active) { uint64_t _va = " + make_va() + ";");
                for (uint32_t k = 0; k < store_dw; ++k) {
                    line("  wsx_va_store32(_va + " + std::to_string(k * 4u) + "ull, " + vreg(vdata + k) + ");");
                }
                line("}");
                ok = true;
            } else if ((d.op >= 0x30u && d.op <= 0x43u) || d.op == 0x4Du) {
                ok = emit_va_atomic32(d.op, make_va(), vdata, vdst, glc);
            } else if (d.op >= 0x50u && d.op <= 0x60u) {
                ok = emit_va_atomic64(d.op, make_va(), vdata, vdst, glc);
            } else if (d.op >= 0x26u && d.op <= 0x2Au) { // global_load_lds_* direct DMA to LDS
                uses_slabs_ = true;
                uses_lds_ = true;
                const std::string lmask = std::to_string(lds_word_mask()) + "u";
                line("if (lane_active) { wsx_lds[((m0 >> 2u) + " + expr_lane_id() + ") & " + lmask + "] = wsx_va_load32(" + make_va() + "); }");
                ok = true;
            }
            break;
        }
        case Enc::DS: {
            const uint32_t off0 = d.w0 & 0xFFu;
            const uint32_t off1 = (d.w0 >> 8) & 0xFFu;
            const uint32_t off16 = d.w0 & 0xFFFFu;
            const uint32_t vaddr = d.w1 & 0xFFu;
            const uint32_t vdata0 = (d.w1 >> 8) & 0xFFu;
            const uint32_t vdata1 = (d.w1 >> 16) & 0xFFu;
            const uint32_t vdst = (d.w1 >> 24) & 0xFFu;
            const std::string lmask = std::to_string(lds_word_mask()) + "u";
            const bool is_gs_lds = stage_has_workgroup();
            auto lds_idx = [&](uint32_t byte_off) {
                return "(((" + vreg(vaddr) + " + " + std::to_string(byte_off) + "u) >> 2u) & " + lmask + ")";
            };
            auto emit_lds_atomic = [&](const char* iop, const std::string& val_expr, bool rtn) {
                uses_lds_ = true;
                if (is_gs_lds) {
                    if (rtn) {
                        line("if (lane_active) { uint _o = 0u; " + std::string(iop) + "(wsx_lds[" + lds_idx(off16) + "], " + val_expr + ", _o); " + vreg(vdst) + " = _o; }");
                    } else {
                        line("if (lane_active) { " + std::string(iop) + "(wsx_lds[" + lds_idx(off16) + "], " + val_expr + "); }");
                    }
                } else {
                    std::string nv;
                    if (std::strcmp(iop, "InterlockedAdd") == 0) nv = "(_o + (" + val_expr + "))";
                    else if (std::strcmp(iop, "InterlockedAnd") == 0) nv = "(_o & (" + val_expr + "))";
                    else if (std::strcmp(iop, "InterlockedOr") == 0) nv = "(_o | (" + val_expr + "))";
                    else if (std::strcmp(iop, "InterlockedXor") == 0) nv = "(_o ^ (" + val_expr + "))";
                    else if (std::strcmp(iop, "InterlockedExchange") == 0) nv = "(" + val_expr + ")";
                    else if (std::strcmp(iop, "InterlockedMin") == 0) {
                        nv = (val_expr.rfind("int(", 0) == 0) ? ("uint(min(int(_o), " + val_expr + "))") : ("min(_o, " + val_expr + ")");
                    } else {
                        nv = (val_expr.rfind("int(", 0) == 0) ? ("uint(max(int(_o), " + val_expr + "))") : ("max(_o, " + val_expr + ")");
                    }
                    line("if (lane_active) { uint _i = " + lds_idx(off16) + "; uint _o = wsx_lds[_i]; wsx_lds[_i] = " + nv + ";" +
                         (rtn ? (" " + vreg(vdst) + " = _o;") : "") + " }");
                }
                ok = true;
            };

            auto emit_lds_f32_atomic = [&](const char* op_kind, bool rtn) {
                uses_lds_ = true;
                if (is_gs_lds) {
                    std::string expr = std::string(op_kind) == "add"
                        ? ("asuint(asfloat(_cur) + asfloat(" + vreg(vdata0) + "))")
                        : (std::string(op_kind) == "min"
                            ? ("asuint(min(asfloat(_cur), asfloat(" + vreg(vdata0) + ")))")
                            : ("asuint(max(asfloat(_cur), asfloat(" + vreg(vdata0) + ")))"));
                    line("if (lane_active) { uint _i = " + lds_idx(off16) + "; uint _cur = wsx_lds[_i]; uint _orig = _cur; [loop] for (uint _it = 0u; _it < 64u; ++_it) { uint _nxt = " + expr + "; uint _p = 0u; InterlockedCompareExchange(wsx_lds[_i], _cur, _nxt, _p); _orig = _p; if (_p == _cur) break; _cur = _p; }" +
                         (rtn ? (" " + vreg(vdst) + " = _orig;") : "") + " }");
                } else {
                    std::string expr = std::string(op_kind) == "add"
                        ? ("asuint(asfloat(_o) + asfloat(" + vreg(vdata0) + "))")
                        : (std::string(op_kind) == "min"
                            ? ("asuint(min(asfloat(_o), asfloat(" + vreg(vdata0) + ")))")
                            : ("asuint(max(asfloat(_o), asfloat(" + vreg(vdata0) + ")))"));
                    line("if (lane_active) { uint _i = " + lds_idx(off16) + "; uint _o = wsx_lds[_i]; wsx_lds[_i] = " + expr + ";" +
                         (rtn ? (" " + vreg(vdst) + " = _o;") : "") + " }");
                }
                ok = true;
            };

            auto emit_lds_u64_op = [&](const char* kind, bool rtn) {
                uses_lds_ = true;
                const std::string vlo = vreg(vdata0);
                const std::string vhi = vreg(vdata0 + 1u);
                const std::string rtn_assign = rtn ? (" " + vreg(vdst) + " = _olo; " + vreg(vdst + 1u) + " = _ohi;") : "";
                if (is_gs_lds && std::strcmp(kind, "add") == 0) {
                    line("if (lane_active) { uint _i = " + lds_idx(off16) + "; uint _i1 = (_i + 1u) & " + lmask +
                         "; uint _olo = 0u, _ohi = 0u; uint _vl = " + vlo + "; uint _vh = " + vhi +
                         "; InterlockedAdd(wsx_lds[_i], _vl, _olo); uint _c = ((_olo + _vl) < _olo) ? 1u : 0u; InterlockedAdd(wsx_lds[_i1], _vh + _c, _ohi);" + rtn_assign + " }");
                } else if (is_gs_lds && (std::strcmp(kind, "and") == 0 || std::strcmp(kind, "or") == 0 ||
                                         std::strcmp(kind, "xor") == 0 || std::strcmp(kind, "xchg") == 0)) {
                    const char* fn = (std::strcmp(kind, "and") == 0) ? "InterlockedAnd"
                                   : (std::strcmp(kind, "or") == 0)  ? "InterlockedOr"
                                   : (std::strcmp(kind, "xor") == 0) ? "InterlockedXor" : "InterlockedExchange";
                    line("if (lane_active) { uint _i = " + lds_idx(off16) + "; uint _i1 = (_i + 1u) & " + lmask +
                         "; uint _olo = 0u, _ohi = 0u; " + std::string(fn) + "(wsx_lds[_i], " + vlo + ", _olo); " +
                         std::string(fn) + "(wsx_lds[_i1], " + vhi + ", _ohi);" + rtn_assign + " }");
                } else {
                    std::string expr = "_o64";
                    if (std::strcmp(kind, "add") == 0)       expr = "(_o64 + _v64)";
                    else if (std::strcmp(kind, "sub") == 0)  expr = "(_o64 - _v64)";
                    else if (std::strcmp(kind, "rsub") == 0) expr = "(_v64 - _o64)";
                    else if (std::strcmp(kind, "inc") == 0)  expr = "(_o64 >= _v64 ? 0ull : (_o64 + 1ull))";
                    else if (std::strcmp(kind, "dec") == 0)  expr = "((_o64 == 0ull || _o64 > _v64) ? _v64 : (_o64 - 1ull))";
                    else if (std::strcmp(kind, "smin") == 0) expr = "uint64_t(min(int64_t(_o64), int64_t(_v64)))";
                    else if (std::strcmp(kind, "smax") == 0) expr = "uint64_t(max(int64_t(_o64), int64_t(_v64)))";
                    else if (std::strcmp(kind, "umin") == 0) expr = "min(_o64, _v64)";
                    else if (std::strcmp(kind, "umax") == 0) expr = "max(_o64, _v64)";
                    else if (std::strcmp(kind, "and") == 0)  expr = "(_o64 & _v64)";
                    else if (std::strcmp(kind, "or") == 0)   expr = "(_o64 | _v64)";
                    else if (std::strcmp(kind, "xor") == 0)  expr = "(_o64 ^ _v64)";
                    else if (std::strcmp(kind, "xchg") == 0) expr = "_v64";
                    else if (std::strcmp(kind, "mskor") == 0) expr = "((_o64 & ~_v64) | _v264)";
                    else if (std::strcmp(kind, "cmpst") == 0) expr = "((_o64 == _v64) ? _v264 : _o64)";
                    const std::string v2decl = (std::strcmp(kind, "mskor") == 0 || std::strcmp(kind, "cmpst") == 0)
                        ? (" uint64_t _v264 = (uint64_t(" + vreg(vdata1 + 1u) + ") << 32) | uint64_t(" + vreg(vdata1) + ");")
                        : "";
                    line("if (lane_active) { uint _i = " + lds_idx(off16) + "; uint _i1 = (_i + 1u) & " + lmask +
                         "; uint _olo = wsx_lds[_i]; uint _ohi = wsx_lds[_i1]; uint64_t _o64 = (uint64_t(_ohi) << 32) | uint64_t(_olo); uint64_t _v64 = (uint64_t(" +
                         vhi + ") << 32) | uint64_t(" + vlo + ");" + v2decl + " uint64_t _n64 = " + expr +
                         "; wsx_lds[_i] = uint(_n64 & 0xFFFFFFFFull); wsx_lds[_i1] = uint(_n64 >> 32);" + rtn_assign + " }");
                }
                ok = true;
            };

            // Classify LDS access kind for automatic Wave32-safe RAW/WAR barrier insertion
            if (is_gs_lds) {
                const bool is_non_lds = (d.op == 0x14u || d.op == 0x35u || d.op == 0xB2u || d.op == 0xB3u ||
                                         (d.op >= 0x80u && d.op <= 0x87u));
                if (!is_non_lds) {
                    const bool is_pure_read =
                        (d.op >= 0x36u && d.op <= 0x3Cu) ||
                        (d.op >= 0x76u && d.op <= 0x78u) ||
                        (d.op >= 0xA2u && d.op <= 0xA7u) ||
                        (d.op == 0xB1u) || (d.op == 0xFEu) || (d.op == 0xFFu);
                    const bool is_pure_write =
                        (d.op >= 0x0Du && d.op <= 0x0Fu) ||
                        (d.op == 0x1Eu || d.op == 0x1Fu) ||
                        (d.op >= 0x4Du && d.op <= 0x4Fu) ||
                        (d.op == 0xA0u || d.op == 0xA1u) ||
                        (d.op == 0xB0u) || (d.op == 0xDEu) || (d.op == 0xDFu);
                    if (is_pure_read) {
                        mark_lds_access(false, true);
                    } else if (is_pure_write) {
                        mark_lds_access(true, false);
                    } else {
                        mark_lds_access(true, true);
                    }
                }
            }

            switch (d.op) {
            case 0x00u: emit_lds_atomic("InterlockedAdd", vreg(vdata0), false); break; // ds_add_u32
            case 0x01u: emit_lds_atomic("InterlockedAdd", "uint(-int(" + vreg(vdata0) + "))", false); break; // ds_sub_u32
            case 0x02u: // ds_rsub_u32
                uses_lds_ = true;
                line("if (lane_active) { uint _i = " + lds_idx(off16) + "; wsx_lds[_i] = " + vreg(vdata0) + " - wsx_lds[_i]; }");
                ok = true; break;
            case 0x03u: emit_lds_atomic("InterlockedAdd", "1u", false); break; // ds_inc_u32
            case 0x04u: emit_lds_atomic("InterlockedAdd", "0xFFFFFFFFu", false); break; // ds_dec_u32
            case 0x05u: emit_lds_atomic("InterlockedMin", "int(" + vreg(vdata0) + ")", false); break; // ds_min_i32
            case 0x06u: emit_lds_atomic("InterlockedMax", "int(" + vreg(vdata0) + ")", false); break; // ds_max_i32
            case 0x07u: emit_lds_atomic("InterlockedMin", vreg(vdata0), false); break; // ds_min_u32
            case 0x08u: emit_lds_atomic("InterlockedMax", vreg(vdata0), false); break; // ds_max_u32
            case 0x09u: emit_lds_atomic("InterlockedAnd", vreg(vdata0), false); break; // ds_and_b32
            case 0x0Au: emit_lds_atomic("InterlockedOr",  vreg(vdata0), false); break; // ds_or_b32
            case 0x0Bu: emit_lds_atomic("InterlockedXor", vreg(vdata0), false); break; // ds_xor_b32
            case 0x0Cu: // ds_mskor_b32
                uses_lds_ = true;
                line("if (lane_active) { uint _i = " + lds_idx(off16) + "; wsx_lds[_i] = (wsx_lds[_i] & ~(" + vreg(vdata0) + ")) | " + vreg(vdata1) + "; }");
                ok = true; break;
            case 0x0Du: // ds_write_b32
                uses_lds_ = true;
                line("if (lane_active) { wsx_lds[" + lds_idx(off16) + "] = " + vreg(vdata0) + "; }");
                ok = true; break;
            case 0x0Eu: // ds_write2_b32
                uses_lds_ = true;
                line("if (lane_active) { wsx_lds[" + lds_idx(off0 * 4u) + "] = " + vreg(vdata0) + "; wsx_lds[" + lds_idx(off1 * 4u) + "] = " + vreg(vdata1) + "; }");
                ok = true; break;
            case 0x0Fu: // ds_write2st64_b32
                uses_lds_ = true;
                line("if (lane_active) { wsx_lds[" + lds_idx(off0 * 256u) + "] = " + vreg(vdata0) + "; wsx_lds[" + lds_idx(off1 * 256u) + "] = " + vreg(vdata1) + "; }");
                ok = true; break;
            case 0x10u: // ds_cmpst_b32
            case 0x11u: // ds_cmpst_f32
                uses_lds_ = true;
                if (is_gs_lds) {
                    line("if (lane_active) { uint _o = 0u; InterlockedCompareExchange(wsx_lds[" + lds_idx(off16) + "], " + vreg(vdata0) + ", " + vreg(vdata1) + ", _o); }");
                } else {
                    line("if (lane_active) { uint _i = " + lds_idx(off16) + "; if (wsx_lds[_i] == " + vreg(vdata0) + ") wsx_lds[_i] = " + vreg(vdata1) + "; }");
                }
                ok = true; break;
            case 0x12u: emit_lds_f32_atomic("min", false); break; // ds_min_f32
            case 0x13u: emit_lds_f32_atomic("max", false); break; // ds_max_f32
            case 0x14u: ok = true; break; // ds_nop
            case 0x15u: emit_lds_f32_atomic("add", false); break; // ds_add_f32
            case 0x16u: case 0x17u: case 0x18u: case 0x19u: case 0x1Au: case 0x1Bu: case 0x1Cu: case 0x1Du: { // ds_*_src2_b32
                uses_lds_ = true;
                line("if (lane_active) { uint _i0 = " + lds_idx(off0 * 4u) + "; uint _i1 = " + lds_idx(off1 * 4u) + "; wsx_lds[_i0] = wsx_lds[_i1]; }");
                ok = true; break;
            }
            case 0x1Eu: // ds_write_b8
                uses_lds_ = true;
                if (is_gs_lds) {
                    line("if (lane_active) { uint _ba = " + vreg(vaddr) + " + " + std::to_string(off16) + "u; uint _sh = (_ba & 3u) * 8u; uint _i = (_ba >> 2u) & " + lmask + "; InterlockedAnd(wsx_lds[_i], ~(0xFFu << _sh)); InterlockedOr(wsx_lds[_i], (" + vreg(vdata0) + " & 0xFFu) << _sh); }");
                } else {
                    line("if (lane_active) { uint _ba = " + vreg(vaddr) + " + " + std::to_string(off16) + "u; uint _sh = (_ba & 3u) * 8u; uint _i = (_ba >> 2u) & " + lmask + "; wsx_lds[_i] = (wsx_lds[_i] & ~(0xFFu << _sh)) | ((" + vreg(vdata0) + " & 0xFFu) << _sh); }");
                }
                ok = true; break;
            case 0x1Fu: // ds_write_b16
                uses_lds_ = true;
                if (is_gs_lds) {
                    line("if (lane_active) { uint _ba = " + vreg(vaddr) + " + " + std::to_string(off16) + "u; uint _sh = (_ba & 2u) * 8u; uint _i = (_ba >> 2u) & " + lmask + "; InterlockedAnd(wsx_lds[_i], ~(0xFFFFu << _sh)); InterlockedOr(wsx_lds[_i], (" + vreg(vdata0) + " & 0xFFFFu) << _sh); }");
                } else {
                    line("if (lane_active) { uint _ba = " + vreg(vaddr) + " + " + std::to_string(off16) + "u; uint _sh = (_ba & 2u) * 8u; uint _i = (_ba >> 2u) & " + lmask + "; wsx_lds[_i] = (wsx_lds[_i] & ~(0xFFFFu << _sh)) | ((" + vreg(vdata0) + " & 0xFFFFu) << _sh); }");
                }
                ok = true; break;
            case 0x20u: emit_lds_atomic("InterlockedAdd", vreg(vdata0), true); break; // ds_add_rtn_u32
            case 0x21u: emit_lds_atomic("InterlockedAdd", "uint(-int(" + vreg(vdata0) + "))", true); break;
            case 0x22u: // ds_rsub_rtn_u32
                uses_lds_ = true;
                line("if (lane_active) { uint _i = " + lds_idx(off16) + "; uint _o = wsx_lds[_i]; wsx_lds[_i] = " + vreg(vdata0) + " - _o; " + vreg(vdst) + " = _o; }");
                ok = true; break;
            case 0x23u: emit_lds_atomic("InterlockedAdd", "1u", true); break;
            case 0x24u: emit_lds_atomic("InterlockedAdd", "0xFFFFFFFFu", true); break;
            case 0x25u: emit_lds_atomic("InterlockedMin", "int(" + vreg(vdata0) + ")", true); break;
            case 0x26u: emit_lds_atomic("InterlockedMax", "int(" + vreg(vdata0) + ")", true); break;
            case 0x27u: emit_lds_atomic("InterlockedMin", vreg(vdata0), true); break;
            case 0x28u: emit_lds_atomic("InterlockedMax", vreg(vdata0), true); break;
            case 0x29u: emit_lds_atomic("InterlockedAnd", vreg(vdata0), true); break;
            case 0x2Au: emit_lds_atomic("InterlockedOr",  vreg(vdata0), true); break;
            case 0x2Bu: emit_lds_atomic("InterlockedXor", vreg(vdata0), true); break;
            case 0x2Cu: // ds_mskor_rtn_b32
                uses_lds_ = true;
                line("if (lane_active) { uint _i = " + lds_idx(off16) + "; uint _o = wsx_lds[_i]; wsx_lds[_i] = (_o & ~(" + vreg(vdata0) + ")) | " + vreg(vdata1) + "; " + vreg(vdst) + " = _o; }");
                ok = true; break;
            case 0x2Du: emit_lds_atomic("InterlockedExchange", vreg(vdata0), true); break; // ds_wrxchg_rtn_b32
            case 0x2Eu: // ds_wrxchg2_rtn_b32
            case 0x2Fu: { // ds_wrxchg2st64_rtn_b32
                uses_lds_ = true;
                const uint32_t mul = (d.op == 0x2Fu) ? 256u : 4u;
                line("if (lane_active) { uint _i0 = " + lds_idx(off0 * mul) + "; uint _i1 = " + lds_idx(off1 * mul) + "; uint _o0 = wsx_lds[_i0]; uint _o1 = wsx_lds[_i1]; wsx_lds[_i0] = " + vreg(vdata0) + "; wsx_lds[_i1] = " + vreg(vdata1) + "; " + vreg(vdst) + " = _o0; " + vreg(vdst + 1u) + " = _o1; }");
                ok = true; break;
            }
            case 0x30u: // ds_cmpst_rtn_b32
            case 0x31u: // ds_cmpst_rtn_f32
                uses_lds_ = true;
                if (is_gs_lds) {
                    line("if (lane_active) { uint _o = 0u; InterlockedCompareExchange(wsx_lds[" + lds_idx(off16) + "], " + vreg(vdata0) + ", " + vreg(vdata1) + ", _o); " + vreg(vdst) + " = _o; }");
                } else {
                    line("if (lane_active) { uint _i = " + lds_idx(off16) + "; uint _o = wsx_lds[_i]; if (_o == " + vreg(vdata0) + ") wsx_lds[_i] = " + vreg(vdata1) + "; " + vreg(vdst) + " = _o; }");
                }
                ok = true; break;
            case 0x32u: emit_lds_f32_atomic("min", true); break; // ds_min_rtn_f32
            case 0x33u: emit_lds_f32_atomic("max", true); break; // ds_max_rtn_f32
            case 0x34u: // ds_wrap_rtn_b32
                uses_lds_ = true;
                line("if (lane_active) { uint _i = " + lds_idx(off16) + "; uint _o = wsx_lds[_i]; wsx_lds[_i] = (_o >= " + vreg(vdata0) + ") ? (_o - " + vreg(vdata0) + ") : (_o + " + vreg(vdata1) + "); " + vreg(vdst) + " = _o; }");
                ok = true; break;
            case 0x35u: // ds_swizzle_b32 (wave lane swizzle, no LDS required!)
                emit_v_assign(vdst, "wsx_ds_swizzle(" + vreg(vaddr) + ", " + std::to_string(off16) + "u)");
                ok = true; break;
            case 0x36u: // ds_read_b32
                uses_lds_ = true;
                line("if (lane_active) { " + vreg(vdst) + " = wsx_lds[" + lds_idx(off16) + "]; }");
                ok = true; break;
            case 0x37u: // ds_read2_b32
                uses_lds_ = true;
                line("if (lane_active) { " + vreg(vdst) + " = wsx_lds[" + lds_idx(off0 * 4u) + "]; " + vreg(vdst + 1u) + " = wsx_lds[" + lds_idx(off1 * 4u) + "]; }");
                ok = true; break;
            case 0x38u: // ds_read2st64_b32
                uses_lds_ = true;
                line("if (lane_active) { " + vreg(vdst) + " = wsx_lds[" + lds_idx(off0 * 256u) + "]; " + vreg(vdst + 1u) + " = wsx_lds[" + lds_idx(off1 * 256u) + "]; }");
                ok = true; break;
            case 0x39u: // ds_read_i8
            case 0x3Au: // ds_read_u8
                uses_lds_ = true;
                line("if (lane_active) { uint _ba = " + vreg(vaddr) + " + " + std::to_string(off16) + "u; uint _b = (wsx_lds[(_ba >> 2u) & " + lmask + "] >> ((_ba & 3u) * 8u)) & 0xFFu; " +
                     vreg(vdst) + (d.op == 0x39u ? " = uint((int(_b) << 24) >> 24); }" : " = _b; }"));
                ok = true; break;
            case 0x3Bu: // ds_read_i16
            case 0x3Cu: // ds_read_u16
                uses_lds_ = true;
                line("if (lane_active) { uint _ba = " + vreg(vaddr) + " + " + std::to_string(off16) + "u; uint _h = (wsx_lds[(_ba >> 2u) & " + lmask + "] >> ((_ba & 2u) * 8u)) & 0xFFFFu; " +
                     vreg(vdst) + (d.op == 0x3Bu ? " = uint((int(_h) << 16) >> 16); }" : " = _h; }"));
                ok = true; break;
            case 0x3Du: // ds_consume (wave-uniform GDS/LDS atomic counter decrement)
            case 0x3Eu: // ds_append  (wave-uniform GDS/LDS atomic counter increment)
                if (is_gs_lds) {
                    uses_lds_ = true;
                    const bool is_consume = (d.op == 0x3Du);
                    const std::string delta = is_consume ? "uint(-int(_wcnt))" : "_wcnt";
                    const std::string lane_res = is_consume ? "(_wbase - 1u - _wpfx)" : "(_wbase + _wpfx)";
                    line("{ uint _wcnt = WaveActiveCountBits(lane_active); uint _wpfx = WavePrefixCountBits(lane_active); "
                         "if (lane_active) { uint _wi = " + lds_idx(off16) + "; uint _wbase = 0u; "
                         "if (WaveIsFirstLane()) { InterlockedAdd(wsx_lds[_wi], " + delta + ", _wbase); } "
                         "_wbase = WaveReadLaneFirst(_wbase); " + vreg(vdst) + " = " + lane_res + "; } }");
                    ok = true;
                } else {
                    emit_lds_atomic("InterlockedAdd", (d.op == 0x3Du) ? "0xFFFFFFFFu" : "1u", true);
                }
                break;
            case 0x3Fu: // ds_ordered_count
                emit_lds_atomic("InterlockedAdd", "1u", true);
                break;
            case 0x40u: emit_lds_u64_op("add", false); break;   // ds_add_u64
            case 0x41u: emit_lds_u64_op("sub", false); break;   // ds_sub_u64
            case 0x42u: emit_lds_u64_op("rsub", false); break;  // ds_rsub_u64
            case 0x43u: emit_lds_u64_op("inc", false); break;   // ds_inc_u64
            case 0x44u: emit_lds_u64_op("dec", false); break;   // ds_dec_u64
            case 0x45u: emit_lds_u64_op("smin", false); break;  // ds_min_i64
            case 0x46u: emit_lds_u64_op("smax", false); break;  // ds_max_i64
            case 0x47u: emit_lds_u64_op("umin", false); break;  // ds_min_u64
            case 0x48u: emit_lds_u64_op("umax", false); break;  // ds_max_u64
            case 0x49u: emit_lds_u64_op("and", false); break;   // ds_and_b64
            case 0x4Au: emit_lds_u64_op("or", false); break;    // ds_or_b64
            case 0x4Bu: emit_lds_u64_op("xor", false); break;   // ds_xor_b64
            case 0x4Cu: emit_lds_u64_op("mskor", false); break; // ds_mskor_b64
            case 0x4Du: // ds_write_b64
                uses_lds_ = true;
                line("if (lane_active) { uint _i = " + lds_idx(off16) + "; wsx_lds[_i] = " + vreg(vdata0) + "; wsx_lds[(_i + 1u) & " + lmask + "] = " + vreg(vdata0 + 1u) + "; }");
                ok = true; break;
            case 0x4Eu: // ds_write2_b64
                uses_lds_ = true;
                line("if (lane_active) { uint _i0 = " + lds_idx(off0 * 8u) + "; uint _i1 = " + lds_idx(off1 * 8u) + "; wsx_lds[_i0] = " + vreg(vdata0) + "; wsx_lds[(_i0+1u)&" + lmask + "] = " + vreg(vdata0+1u) + "; wsx_lds[_i1] = " + vreg(vdata1) + "; wsx_lds[(_i1+1u)&" + lmask + "] = " + vreg(vdata1+1u) + "; }");
                ok = true; break;
            case 0x4Fu: // ds_write2st64_b64
                uses_lds_ = true;
                line("if (lane_active) { uint _i0 = " + lds_idx(off0 * 512u) + "; uint _i1 = " + lds_idx(off1 * 512u) + "; wsx_lds[_i0] = " + vreg(vdata0) + "; wsx_lds[(_i0+1u)&" + lmask + "] = " + vreg(vdata0+1u) + "; wsx_lds[_i1] = " + vreg(vdata1) + "; wsx_lds[(_i1+1u)&" + lmask + "] = " + vreg(vdata1+1u) + "; }");
                ok = true; break;
            case 0x50u: case 0x51u: emit_lds_u64_op("cmpst", false); break; // ds_cmpst_b64 / f64
            case 0x52u: emit_lds_u64_op("umin", false); break; // ds_min_f64
            case 0x53u: emit_lds_u64_op("umax", false); break; // ds_max_f64
            case 0x54u: emit_lds_u64_op("add", false); break;  // ds_add_f64
            case 0x55u: emit_lds_f32_atomic("add", true); break; // ds_add_rtn_f32
            case 0x56u: case 0x57u: case 0x58u: case 0x59u: case 0x5Au: case 0x5Bu: case 0x5Cu: case 0x5Du: case 0x5Eu: case 0x5Fu: // ds_*_src2_b64
                uses_lds_ = true;
                line("if (lane_active) { uint _i0 = " + lds_idx(off0 * 8u) + "; uint _i1 = " + lds_idx(off1 * 8u) + "; wsx_lds[_i0] = wsx_lds[_i1]; wsx_lds[(_i0+1u)&" + lmask + "] = wsx_lds[(_i1+1u)&" + lmask + "]; }");
                ok = true; break;
            case 0x60u: emit_lds_u64_op("add", true); break;   // ds_add_rtn_u64
            case 0x61u: emit_lds_u64_op("sub", true); break;   // ds_sub_rtn_u64
            case 0x62u: emit_lds_u64_op("rsub", true); break;  // ds_rsub_rtn_u64
            case 0x63u: emit_lds_u64_op("inc", true); break;   // ds_inc_rtn_u64
            case 0x64u: emit_lds_u64_op("dec", true); break;   // ds_dec_rtn_u64
            case 0x65u: emit_lds_u64_op("smin", true); break;  // ds_min_rtn_i64
            case 0x66u: emit_lds_u64_op("smax", true); break;  // ds_max_rtn_i64
            case 0x67u: emit_lds_u64_op("umin", true); break;  // ds_min_rtn_u64
            case 0x68u: emit_lds_u64_op("umax", true); break;  // ds_max_rtn_u64
            case 0x69u: emit_lds_u64_op("and", true); break;   // ds_and_rtn_b64
            case 0x6Au: emit_lds_u64_op("or", true); break;    // ds_or_rtn_b64
            case 0x6Bu: emit_lds_u64_op("xor", true); break;   // ds_xor_rtn_b64
            case 0x6Cu: emit_lds_u64_op("mskor", true); break; // ds_mskor_rtn_b64
            case 0x6Du: emit_lds_u64_op("xchg", true); break;  // ds_wrxchg_rtn_b64
            case 0x6Eu: // ds_wrxchg2_rtn_b64
            case 0x6Fu: { // ds_wrxchg2st64_rtn_b64
                uses_lds_ = true;
                const uint32_t mul = (d.op == 0x6Fu) ? 512u : 8u;
                line("if (lane_active) { uint _i0 = " + lds_idx(off0 * mul) + "; uint _i1 = " + lds_idx(off1 * mul) + "; uint _a0 = wsx_lds[_i0]; uint _a1 = wsx_lds[(_i0+1u)&" + lmask + "]; uint _b0 = wsx_lds[_i1]; uint _b1 = wsx_lds[(_i1+1u)&" + lmask + "]; wsx_lds[_i0] = " + vreg(vdata0) + "; wsx_lds[(_i0+1u)&" + lmask + "] = " + vreg(vdata0+1u) + "; wsx_lds[_i1] = " + vreg(vdata1) + "; wsx_lds[(_i1+1u)&" + lmask + "] = " + vreg(vdata1+1u) + "; " + vreg(vdst) + " = _a0; " + vreg(vdst+1u) + " = _a1; " + vreg(vdst+2u) + " = _b0; " + vreg(vdst+3u) + " = _b1; }");
                ok = true; break;
            }
            case 0x70u: case 0x71u: emit_lds_u64_op("cmpst", true); break; // ds_cmpst_rtn_b64 / f64
            case 0x72u: emit_lds_u64_op("umin", true); break; // ds_min_rtn_f64
            case 0x73u: emit_lds_u64_op("umax", true); break; // ds_max_rtn_f64
            case 0x74u: emit_lds_u64_op("add", true); break;  // ds_add_rtn_f64
            case 0x75u: emit_lds_u64_op("xchg", true); break; // ds_condxchg32_rtn_b64
            case 0x76u: // ds_read_b64
                uses_lds_ = true;
                line("if (lane_active) { uint _i = " + lds_idx(off16) + "; " + vreg(vdst) + " = wsx_lds[_i]; " + vreg(vdst + 1u) + " = wsx_lds[(_i + 1u) & " + lmask + "]; }");
                ok = true; break;
            case 0x77u: // ds_read2_b64
                uses_lds_ = true;
                line("if (lane_active) { uint _i0 = " + lds_idx(off0 * 8u) + "; uint _i1 = " + lds_idx(off1 * 8u) + "; " + vreg(vdst) + " = wsx_lds[_i0]; " + vreg(vdst+1u) + " = wsx_lds[(_i0+1u)&" + lmask + "]; " + vreg(vdst+2u) + " = wsx_lds[_i1]; " + vreg(vdst+3u) + " = wsx_lds[(_i1+1u)&" + lmask + "]; }");
                ok = true; break;
            case 0x78u: // ds_read2st64_b64
                uses_lds_ = true;
                line("if (lane_active) { uint _i0 = " + lds_idx(off0 * 512u) + "; uint _i1 = " + lds_idx(off1 * 512u) + "; " + vreg(vdst) + " = wsx_lds[_i0]; " + vreg(vdst+1u) + " = wsx_lds[(_i0+1u)&" + lmask + "]; " + vreg(vdst+2u) + " = wsx_lds[_i1]; " + vreg(vdst+3u) + " = wsx_lds[(_i1+1u)&" + lmask + "]; }");
                ok = true; break;
            case 0x80u: case 0x81u: case 0x82u: case 0x83u: case 0x84u: case 0x85u: case 0x86u: case 0x87u: // ds_gws_*
                if (is_gs_lds) {
                    line("GroupMemoryBarrierWithGroupSync();");
                    lds_hazard_ = LdsHazardState::None;
                }
                ok = true; break;
            case 0xA0u: // ds_write_b8_d16_hi
            case 0xA1u: // ds_write_b16_d16_hi
                uses_lds_ = true;
                line("if (lane_active) { uint _ba = " + vreg(vaddr) + " + " + std::to_string(off16) + "u; uint _i = (_ba >> 2u) & " + lmask + "; wsx_lds[_i] = (" + vreg(vdata0) + " >> 16u); }");
                ok = true; break;
            case 0xA2u: case 0xA3u: case 0xA4u: case 0xA5u: case 0xA6u: case 0xA7u: // ds_read_*_d16*
                uses_lds_ = true;
                line("if (lane_active) { uint _ba = " + vreg(vaddr) + " + " + std::to_string(off16) + "u; uint _val = wsx_lds[(_ba >> 2u) & " + lmask + "] & 0xFFFFu; " +
                     vreg(vdst) + ((d.op & 1u) ? (" = (" + vreg(vdst) + " & 0xFFFFu) | (_val << 16u); }") : (" = (" + vreg(vdst) + " & 0xFFFF0000u) | _val; }")));
                ok = true; break;
            case 0xB0u: // ds_write_addtid_b32
                uses_lds_ = true;
                line("if (lane_active) { wsx_lds[((" + std::to_string(off16) + "u >> 2u) + " + expr_lane_id() + ") & " + lmask + "] = " + vreg(vdata0) + "; }");
                ok = true; break;
            case 0xB1u: // ds_read_addtid_b32
                uses_lds_ = true;
                line("if (lane_active) { " + vreg(vdst) + " = wsx_lds[((" + std::to_string(off16) + "u >> 2u) + " + expr_lane_id() + ") & " + lmask + "]; }");
                ok = true; break;
            case 0xB2u: // ds_permute_b32 (forward scatter approx via lane read)
            case 0xB3u: // ds_bpermute_b32 (backward gather: dst = lane[(addr >> 2) & (laneCount - 1)].data)
                if (use_w64_emu()) {
                    // Evaluate groupshared lane exchange OUTSIDE if (lane_active) so barriers stay wave-uniform!
                    line("{ uint _w64p = wsx_w64_readlane(" + vreg(vdata0) + ", (" + vreg(vaddr) + " >> 2u), wsx_gi); if (lane_active) { " + vreg(vdst) + " = _w64p; } }");
                } else {
                    emit_v_assign(vdst, "WaveReadLaneAt(" + vreg(vdata0) + ", (" + vreg(vaddr) + " >> 2u) & (WaveGetLaneCount() - 1u))");
                }
                ok = true; break;
            case 0xDEu: // ds_write_b96
            case 0xDFu: { // ds_write_b128
                uses_lds_ = true;
                const uint32_t ndw = (d.op == 0xDFu) ? 4u : 3u;
                line("if (lane_active) { uint _i = " + lds_idx(off16) + ";");
                for (uint32_t k = 0; k < ndw; ++k) {
                    line("  wsx_lds[(_i + " + std::to_string(k) + "u) & " + lmask + "] = " + vreg(vdata0 + k) + ";");
                }
                line("}");
                ok = true; break;
            }
            case 0xFEu: // ds_read_b96
            case 0xFFu: { // ds_read_b128
                uses_lds_ = true;
                const uint32_t ndw = (d.op == 0xFFu) ? 4u : 3u;
                line("if (lane_active) { uint _i = " + lds_idx(off16) + ";");
                for (uint32_t k = 0; k < ndw; ++k) {
                    line("  " + vreg(vdst + k) + " = wsx_lds[(_i + " + std::to_string(k) + "u) & " + lmask + "];");
                }
                line("}");
                ok = true; break;
            }
            default: break;
            }
            break;
        }
        case Enc::MIMG: {
            uses_textures_ = true;
            const uint32_t dmask = (d.w0 >> 8) & 0xFu;
            const bool glc = ((d.w0 >> 13) & 1u) != 0u;
            const uint32_t vaddr = d.w1 & 0xFFu;
            const uint32_t vdata = (d.w1 >> 8) & 0xFFu;
            const uint32_t srsrc = ((d.w1 >> 16) & 0x1Fu) << 2u;
            const uint32_t ssamp = ((d.w1 >> 21) & 0x1Fu) << 2u;
            const bool is_store = (d.op >= 0x08u && d.op <= 0x0Cu);
            const bool is_resinfo = (d.op == 0x0Eu);
            const bool is_atomic = (d.op >= 0x10u && d.op <= 0x1Cu);
            const bool is_bvh = (d.op == 0x65u || d.op == 0x66u);
            const bool is_lod = (d.op == 0x60u);
            const bool is_msaa = (d.op == 0x68u);
            const bool has_samp = (d.op >= 0x20u && d.op <= 0x5Fu) || (d.op >= 0x61u && d.op <= 0x7Fu && !is_bvh && !is_msaa);
            const char* comps[4] = {".x", ".y", ".z", ".w"};

            if (is_bvh) {
                uses_slabs_ = true;
                const bool is64 = (d.op == 0x66u);
                const uint32_t off = is64 ? 1u : 0u;
                const std::string nptr = is64
                    ? ("((uint64_t(" + vreg(vaddr + 1u) + ") << 32) | uint64_t(" + vreg(vaddr) + "))")
                    : ("uint64_t(" + vreg(vaddr) + ")");
                line("if (lane_active) { uint64_t _bbase = " + vsharp_va_from_s(srsrc) + ";");
                line("  uint4 _bh = wsx_bvh_intersect_ray(_bbase, " + nptr + ", asfloat(" + vreg(vaddr + 1u + off) + "), "
                     "float3(asfloat(" + vreg(vaddr + 2u + off) + "), asfloat(" + vreg(vaddr + 3u + off) + "), asfloat(" + vreg(vaddr + 4u + off) + ")), "
                     "float3(asfloat(" + vreg(vaddr + 5u + off) + "), asfloat(" + vreg(vaddr + 6u + off) + "), asfloat(" + vreg(vaddr + 7u + off) + ")), "
                     "float3(asfloat(" + vreg(vaddr + 8u + off) + "), asfloat(" + vreg(vaddr + 9u + off) + "), asfloat(" + vreg(vaddr + 10u + off) + ")));");
                line("  " + vreg(vdata) + " = _bh.x; " + vreg(vdata + 1u) + " = _bh.y; " + vreg(vdata + 2u) + " = _bh.z; " + vreg(vdata + 3u) + " = _bh.w; }");
                ok = true;
                break;
            }

            const bool is_ps_quad_grad =
                (stage_ == 0u) && has_samp &&
                (d.op == 0x20u || d.op == 0x25u || d.op == 0x28u || d.op == 0x2Du);
            const bool is_bias_sample = (d.op == 0x25u || d.op == 0x2Du);

            if (is_ps_quad_grad) {
                // SM 6.6 Quad-Gradient SampleGrad: compute derivatives OUTSIDE if (lane_active) so all 4
                // quad lanes participate even inside divergent branches or loops (no SampleLevel(0) blur).
                ++quad_grad_samples_;
                std::string qline = "{ float2 _quv = float2(asfloat(" + vreg(vaddr) + "), asfloat(" + vreg(vaddr + 1u) +
                                    ")); float2 _qdx = QuadReadAcrossX(_quv) - _quv; float2 _qdy = QuadReadAcrossY(_quv) - _quv;";
                if (is_bias_sample) {
                    qline += " float _qb = exp2(clamp(asfloat(" + vreg(vaddr + 2u) + "), -16.0f, 16.0f)); _qdx *= _qb; _qdy *= _qb;";
                }
                qline += " if (lane_active) { uint _ti = wsx_lookup_tsharp(uint4(" + sreg(srsrc) + "," + sreg(srsrc+1) + "," + sreg(srsrc+2) + "," + sreg(srsrc+3) +
                         "), uint4(" + sreg(srsrc+4) + "," + sreg(srsrc+5) + "," + sreg(srsrc+6) + "," + sreg(srsrc+7) + "));"
                         " Texture2D<float4> _tex = ResourceDescriptorHeap[NonUniformResourceIndex(_ti)];"
                         " uint _si = wsx_lookup_ssharp(uint4(" + sreg(ssamp) + "," + sreg(ssamp+1) + "," + sreg(ssamp+2) + "," + sreg(ssamp+3) + "));"
                         " SamplerState _smp = SamplerDescriptorHeap[NonUniformResourceIndex(_si)];"
                         " float4 _tx = _tex.SampleGrad(_smp, _quv, _qdx, _qdy);";
                uint32_t out_lane = 0;
                for (uint32_t c = 0; c < 4u; ++c) {
                    if (dmask & (1u << c)) {
                        qline += " " + vreg(vdata + out_lane) + " = asuint(_tx" + comps[c] + ");";
                        ++out_lane;
                    }
                }
                qline += " } }";
                line(qline);
                ok = true;
                break;
            }

            line("if (lane_active) {");
            line("  uint _ti = wsx_lookup_tsharp(uint4(" + sreg(srsrc) + "," + sreg(srsrc+1) + "," + sreg(srsrc+2) + "," + sreg(srsrc+3) + "), uint4(" + sreg(srsrc+4) + "," + sreg(srsrc+5) + "," + sreg(srsrc+6) + "," + sreg(srsrc+7) + "));");
            if (is_atomic) {
                const uint32_t kind = d.op - 0x10u; // 0=swap..12=dec
                const std::string cmp_arg = (kind == 1u) ? vreg(vdata + 1u) : "0u";
                line("  uint _to = wsx_tex_atomic_op32(_ti, uint2(" + vreg(vaddr) + ", " + vreg(vaddr + 1u) + "), " +
                     std::to_string(kind) + "u, " + vreg(vdata) + ", " + cmp_arg + ");" +
                     (glc ? (" " + vreg(vdata) + " = _to;") : ""));
            } else if (is_resinfo) {
                line("  Texture2D<float4> _tex = ResourceDescriptorHeap[NonUniformResourceIndex(_ti)];");
                line("  uint _tw = 0u, _th = 0u, _tm = 0u; _tex.GetDimensions(0u, _tw, _th, _tm);");
                line("  uint4 _ri = uint4(_tw, _th, 1u, _tm);");
                uint32_t out_lane = 0;
                for (uint32_t c = 0; c < 4u; ++c) {
                    if (dmask & (1u << c)) {
                        line("  " + vreg(vdata + out_lane) + " = _ri" + comps[c] + ";");
                        ++out_lane;
                    }
                }
            } else if (is_store) {
                line("  RWTexture2D<float4> _rwtex = ResourceDescriptorHeap[NonUniformResourceIndex(_ti)];");
                if (clamp_finite_image_stores_) {
                    line("  _rwtex[uint2(" + vreg(vaddr) + ", " + vreg(vaddr + 1u) + ")] = float4(wsx_clamp_finite_f32(asfloat(" + vreg(vdata) + ")), wsx_clamp_finite_f32(asfloat(" + vreg(vdata + 1u) + ")), wsx_clamp_finite_f32(asfloat(" + vreg(vdata + 2u) + ")), wsx_clamp_finite_f32(asfloat(" + vreg(vdata + 3u) + ")));");
                } else {
                    line("  _rwtex[uint2(" + vreg(vaddr) + ", " + vreg(vaddr + 1u) + ")] = float4(asfloat(" + vreg(vdata) + "), asfloat(" + vreg(vdata + 1u) + "), asfloat(" + vreg(vdata + 2u) + "), asfloat(" + vreg(vdata + 3u) + "));");
                }
            } else if (is_lod) {
                line("  float4 _tx = float4(0.0f, 0.0f, 0.0f, 0.0f);");
                uint32_t out_lane = 0;
                for (uint32_t c = 0; c < 4u; ++c) {
                    if (dmask & (1u << c)) {
                        line("  " + vreg(vdata + out_lane) + " = asuint(_tx" + comps[c] + ");");
                        ++out_lane;
                    }
                }
            } else {
                line("  Texture2D<float4> _tex = ResourceDescriptorHeap[NonUniformResourceIndex(_ti)];");
                if (has_samp) {
                    line("  uint _si = wsx_lookup_ssharp(uint4(" + sreg(ssamp) + "," + sreg(ssamp+1) + "," + sreg(ssamp+2) + "," + sreg(ssamp+3) + "));");
                    line("  SamplerState _smp = SamplerDescriptorHeap[NonUniformResourceIndex(_si)];");
                    line("  float4 _tx = _tex.SampleLevel(_smp, float2(asfloat(" + vreg(vaddr) + "), asfloat(" + vreg(vaddr + 1u) + ")), 0.0f);");
                } else {
                    line("  float4 _tx = _tex.Load(int3(int(" + vreg(vaddr) + "), int(" + vreg(vaddr + 1u) + "), 0));");
                }
                uint32_t out_lane = 0;
                for (uint32_t c = 0; c < 4u; ++c) {
                    if (dmask & (1u << c)) {
                        line("  " + vreg(vdata + out_lane) + " = asuint(_tx" + comps[c] + ");");
                        ++out_lane;
                    }
                }
            }
            line("}");
            ok = true;
            break;
        }
        case Enc::VINTRP: {
            // GFX10.3 VINTRP: op 0=v_interp_p1_f32, 1=v_interp_p2_f32, 2=v_interp_mov_f32
            const uint32_t chan = (d.w0 >> 8) & 3u;
            const uint32_t attr = (d.w0 >> 10) & 0x3Fu;
            const uint32_t vd = (d.w0 >> 18) & 0xFFu;
            if (d.op <= 2u) {
                // D3D12 rasterizer delivers hardware-interpolated attributes directly in wsx_in.p[slot].
                emit_v_assign_f32(vd, ps_attr_expr(attr, chan));
                ok = true;
            }
            break;
        }
        case Enc::EXP: {
            // GFX10.3 EXP: MRT0..7 (0..7), MRTZ (8), NULL (9), POS0..3 (12..15), PRIM (20), PARAM0..31 (32..63)
            const uint32_t en = d.w0 & 0xFu;
            const uint32_t tgt = (d.w0 >> 4) & 0x3Fu;
            const bool compr = ((d.w0 >> 10) & 1u) != 0u;
            const bool done = ((d.w0 >> 11) & 1u) != 0u;
            const bool vm = ((d.w0 >> 12) & 1u) != 0u;
            const uint32_t s[4] = {
                d.w1 & 0xFFu,
                (d.w1 >> 8) & 0xFFu,
                (d.w1 >> 16) & 0xFFu,
                (d.w1 >> 24) & 0xFFu
            };
            const bool ch_en[4] = {
                compr ? ((en & 3u) != 0u) : ((en & 1u) != 0u),
                compr ? ((en & 3u) != 0u) : ((en & 2u) != 0u),
                compr ? ((en & 12u) != 0u) : ((en & 4u) != 0u),
                compr ? ((en & 12u) != 0u) : ((en & 8u) != 0u)
            };
            static constexpr const char* kComp[4] = {".x", ".y", ".z", ".w"};
            auto ch_val = [&](uint32_t c) -> std::string {
                if (compr) {
                    const uint32_t reg = (c < 2u) ? s[0] : s[1];
                    return (c & 1u) ? ("f16tof32((" + vreg(reg) + ") >> 16u)") : ("f16tof32(" + vreg(reg) + ")");
                }
                return "asfloat(" + vreg(s[c]) + ")";
            };
            auto emit_vec4_export = [&](const std::string& dst_vec) {
                std::string stmts;
                for (uint32_t c = 0; c < 4u; ++c) {
                    if (ch_en[c]) {
                        stmts += dst_vec + kComp[c] + " = " + ch_val(c) + "; ";
                    }
                }
                if (!stmts.empty()) {
                    line("if (lane_active) { " + stmts + "}");
                }
            };

            if (tgt <= 7u) {
                if (stage_ == 0u) {
                    ps_mrts_exported_.insert(tgt);
                    emit_vec4_export("wsx_ps_out.mrt" + std::to_string(tgt));
                    if (vm && done) line("if (!lane_active) { discard; }");
                }
            } else if (tgt == 8u) {
                if (stage_ == 0u) {
                    ps_depth_exported_ = true;
                    if (ch_en[0]) line("if (lane_active) { wsx_ps_out.depth = " + ch_val(0) + "; }");
                    if (vm && done) line("if (!lane_active) { discard; }");
                }
            } else if (tgt == 9u) {
                if (stage_ == 0u && vm) {
                    line("if (!lane_active) { discard; }");
                }
            } else if (tgt == 12u) {
                if (stage_ == 1u || stage_ == 2u || stage_ == 13u) {
                    emit_vec4_export("wsx_out.pos");
                }
            } else if (tgt == 20u) {
                if (stage_ == 13u) {
                    line("if (lane_active) { uint _p = " + vreg(s[0]) +
                         "; if ((_p & 0x80000000u) == 0u) { wsx_prim = uint3(_p & 0x3FFu, (_p >> 10u) & 0x3FFu, (_p >> 20u) & 0x3FFu); } }");
                }
            } else if (tgt >= 32u && tgt <= 63u) {
                if (stage_ == 1u || stage_ == 2u || stage_ == 13u) {
                    const uint32_t cap = (stage_ == 13u) ? 16u : 31u;
                    const uint32_t slot = (tgt - 32u) % cap;
                    vs_params_exported_.insert(slot);
                    emit_vec4_export("wsx_out.p" + std::to_string(slot));
                }
            }
            ok = true;
            break;
        }
        default:
            break;
        }

        if (ok) {
            ++translated_count_;
        } else {
            ++unsupported_count_;
            if (unimpl_sink_) unimpl_sink_->push_back(d);
            char cmt[128];
            std::snprintf(cmt, sizeof(cmt), "/* unimpl pc=0x%X %s op=0x%X w0=0x%08X w1=0x%08X */",
                          d.pc, enc_name(d.enc), d.op, d.w0, d.w1);
            line(cmt);
        }
    }

public:
    std::string translate(
        const uint8_t* fssm,
        size_t fssm_size,
        uint32_t stage,
        TranslateStats& stats,
        std::vector<DecodedInsn>* out_unimpl = nullptr) {
        std::memset(&stats, 0, sizeof(stats));
        if (!fssm || fssm_size < 0xC8u || std::memcmp(fssm, "FSSM", 4) != 0) {
            return {};
        }

        const uint32_t raw_c0 = read_u32_le(fssm + 0xC0u);
        const uint32_t code_off = raw_c0 + 0x28u;
        if (code_off + 4u > fssm_size) return {};

        uint32_t code_sz = static_cast<uint32_t>(fssm_size - code_off);
        const uint32_t hdr_code_sz = read_u32_le(fssm + 0xC8u);
        if (hdr_code_sz >= 4u && code_off + hdr_code_sz <= fssm_size) {
            code_sz = hdr_code_sz;
        }

        uint32_t ntx = 1u, nty = 1u, ntz = 1u;
        if (stage == 5u || stage == 0xFFFFu) {
            ntx = 64u;
            if (fssm_size >= 0x148u) {
                const uint32_t hx = read_u32_le(fssm + 0x13Cu);
                const uint32_t hy = read_u32_le(fssm + 0x140u);
                const uint32_t hz = read_u32_le(fssm + 0x144u);
                if (hx >= 1u && hx <= 1024u && hy >= 1u && hy <= 1024u && hz >= 1u && hz <= 64u &&
                    static_cast<uint64_t>(hx) * hy * hz <= 1024ull) {
                    ntx = hx; nty = hy; ntz = hz;
                }
            }
        } else if (stage == 13u) {
            ntx = 64u;
        } else if (stage == 12u) {
            ntx = 32u;
        }

        const bool hdr_wave32 = (fssm_size >= 0xD8u) && (((read_u32_le(fssm + 0xD4u) >> 5) & 1u) != 0u);
        const size_t nwords = code_sz / 4u;
        std::vector<uint32_t> words(nwords);
        std::memcpy(words.data(), fssm + code_off, nwords * 4u);

        return translate_raw_rdna2(words.data(), words.size(), stage, ntx, nty, ntz,
                                   code_off, hdr_wave32, stats, out_unimpl);
    }

    std::string translate_raw_rdna2(
        const uint32_t* words,
        size_t nwords,
        uint32_t stage,
        uint32_t ntx,
        uint32_t nty,
        uint32_t ntz,
        uint32_t code_off,
        bool hdr_wave32,
        TranslateStats& stats,
        std::vector<DecodedInsn>* out_unimpl = nullptr) {
        std::memset(&stats, 0, sizeof(stats));
        body_.clear();
        line_sink_ = nullptr;
        sgprs_used_.clear();
        vgprs_used_.clear();
        ps_attrs_used_.clear();
        ps_mrts_exported_.clear();
        vs_params_exported_.clear();
        ps_depth_exported_ = false;
        is_wave64_ = false;
        uses_lds_ = false;
        uses_slabs_ = false;
        uses_textures_ = false;
        uses_scratch_ = false;
        uses_vrel_ = false;
        translated_count_ = 0;
        unsupported_count_ = 0;
        cfg_mode_ = CfgLoweringMode::StateMachine;
        exec_elided_ = false;
        coalesced_guards_ = 0;
        dead_scc_elided_ = 0;
        dead_reg_elided_ = 0;
        multi_exit_loops_structured_ = 0;
        peephole_folds_ = 0;
        branchless_selects_ = 0;
        auto_lds_barriers_ = 0;
        quad_grad_samples_ = 0;
        lds_hazard_ = LdsHazardState::None;
        auto_lds_barrier_enabled_ = wsx::get_config_bool("WSX_RDNA2_AUTO_LDS_BARRIER", "Graphics", "rdna2_auto_lds_barrier", true);
        clamp_finite_image_stores_ = wsx::get_config_bool("WSX_RDNA2_CLAMP_FINITE_STORES", "Graphics", "rdna2_clamp_finite_stores", true);
        ms_saw_alloc_req_ = false;
        as_saw_dispatch_req_ = false;
        use_native_fp16_ = wsx::get_config_bool("WSX_FP16_VOP3P", "Graphics", "fp16_vop3p", false);

        stage_ = stage;
        fssm_code_off_ = code_off;
        unimpl_sink_ = out_unimpl;
        stats.Stage = stage;
        stats.NumThreadsX = ntx;
        stats.NumThreadsY = nty;
        stats.NumThreadsZ = ntz;
        stats.CodeOffset = code_off;

        if (!words || nwords == 0u) return {};

        const std::vector<DecodedInsn> insns = decode_rdna2_stream(words, nwords);
        if (insns.empty()) return {};
        stats.CodeSize = insns.back().pc + insns.back().size_bytes;
        stats.TotalInstructions = static_cast<uint32_t>(insns.size());

        // Classify Wave64 vs Wave32 from both header flag and decoded EXEC instructions
        bool has_w64_exec = false;
        bool has_w32_exec = false;
        for (const auto& d : insns) {
            if (d.enc == Enc::SOP1) {
                const uint32_t sd = (d.w0 >> 16) & 0x7Fu;
                if ((d.op >= 0x24u && d.op <= 0x2Bu) || (d.op >= 0x37u && d.op <= 0x3Au) ||
                    (d.op == 0x04u && sd == 126u)) {
                    has_w64_exec = true;
                } else if ((d.op >= 0x3Cu && d.op <= 0x47u) || (d.op == 0x03u && sd == 126u)) {
                    has_w32_exec = true;
                }
            }
        }
        is_wave64_ = has_w64_exec ? true : (has_w32_exec ? false : !hdr_wave32);
        w64_mode_ = Wave64EmulationMode::Disabled;
        w64_force_mode_ = false;
        wave64_emulated_ = false;
        if (is_wave64_) {
            const bool legacy_ws64 = wsx::get_config_bool("WSX_EMIT_WAVESIZE64", "Graphics", "emit_wavesize64", false);
            std::string w64_cfg = wsx::get_config_string("WSX_WAVE64_EMULATE", "Graphics", "wave64_emulate", "auto");
            for (char& ch : w64_cfg) {
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            }
            if (legacy_ws64 || w64_cfg == "native64") {
                w64_mode_ = Wave64EmulationMode::Native64;
            } else if (w64_cfg == "0" || w64_cfg == "off" || w64_cfg == "false") {
                w64_mode_ = Wave64EmulationMode::Disabled;
            } else if (w64_cfg == "1" || w64_cfg == "force" || w64_cfg == "true" || w64_cfg == "on") {
                if (stage_has_workgroup()) {
                    w64_mode_ = Wave64EmulationMode::Emulate;
                    w64_force_mode_ = true;
                }
            } else {
                // auto: emit native [WaveSize(64)] on AMD (WaveLaneCountMax >= 64),
                // or groupshared 2x32-lane Wave64 emulation on 32-wide NVIDIA/Intel GPUs.
                if (wsx_sm66::get_host_wave_lane_max() >= 64u) {
                    w64_mode_ = Wave64EmulationMode::Native64;
                } else if (stage_has_workgroup()) {
                    w64_mode_ = Wave64EmulationMode::Emulate;
                }
            }
        }

        // Discover basic block leaders (PC = 0, every branch target, every fallthrough after branch/endpgm)
        std::set<uint32_t> leaders;
        leaders.insert(0u);
        for (size_t idx = 0; idx < insns.size(); ++idx) {
            const auto& d = insns[idx];
            const bool is_sopp_cf = (d.enc == Enc::SOPP &&
                (d.op == 0x01u || d.op == 0x02u || (d.op >= 0x04u && d.op <= 0x09u) ||
                 d.op == 0x1Bu || d.op == 0x1Eu));
            const bool is_sopk_cf = (d.enc == Enc::SOPK && d.op == 0x16u);
            if (is_sopp_cf || is_sopk_cf) {
                if (is_sopk_cf || (d.op != 0x01u && d.op != 0x1Bu && d.op != 0x1Eu)) {
                    const int16_t simm = static_cast<int16_t>(d.w0 & 0xFFFFu);
                    const int64_t tgt = static_cast<int64_t>(d.pc) + 4 + static_cast<int64_t>(simm) * 4;
                    if (tgt >= 0 && static_cast<uint64_t>(tgt) < stats.CodeSize) {
                        leaders.insert(static_cast<uint32_t>(tgt));
                    }
                }
                if (idx + 1 < insns.size()) {
                    leaders.insert(insns[idx + 1].pc);
                }
            }
        }

        std::map<uint32_t, uint32_t> pc_to_bb;
        uint32_t bb_next = 0;
        for (uint32_t lpc : leaders) pc_to_bb[lpc] = bb_next++;
        const uint32_t num_bbs = bb_next;
        const uint32_t kExitBB = num_bbs;
        const uint32_t kNoBB = 0xFFFFFFFFu;
        stats.BasicBlockCount = num_bbs;

        // Build explicit BasicBlock IR (body instructions + clean CFG terminator)
        enum class TermKind : uint8_t { EndPgm = 0, Fallthrough = 1, Jump = 2, CondBranch = 3 };
        struct BasicBlock {
            uint32_t id = 0;
            TermKind term = TermKind::EndPgm;
            std::string cond;
            uint32_t true_bb = 0;
            uint32_t false_bb = 0;
            std::vector<std::string> lines;
        };
        std::vector<BasicBlock> bbs(num_bbs);
        for (uint32_t b = 0; b < num_bbs; ++b) {
            bbs[b].id = b;
            bbs[b].true_bb = kExitBB;
            bbs[b].false_bb = kExitBB;
        }

        auto resolve_tgt = [&](const DecodedInsn& d) -> uint32_t {
            const int16_t simm = static_cast<int16_t>(d.w0 & 0xFFFFu);
            const int64_t tgt = static_cast<int64_t>(d.pc) + 4 + static_cast<int64_t>(simm) * 4;
            if (tgt < 0) return kExitBB;
            auto it = pc_to_bb.find(static_cast<uint32_t>(tgt));
            return (it != pc_to_bb.end()) ? it->second : kExitBB;
        };

        std::vector<size_t> bb_start_idx(num_bbs, insns.size());
        std::vector<size_t> bb_end_idx(num_bbs, insns.size());
        {
            uint32_t cur_bb = 0u;
            for (size_t idx = 0; idx < insns.size(); ++idx) {
                auto bit = pc_to_bb.find(insns[idx].pc);
                if (bit != pc_to_bb.end()) {
                    if (idx > 0) bb_end_idx[cur_bb] = idx;
                    cur_bb = bit->second;
                    bb_start_idx[cur_bb] = idx;
                }
            }
            bb_end_idx[cur_bb] = insns.size();
        }

        bool has_inline_pc_mut = false;
        for (uint32_t b = 0; b < num_bbs; ++b) {
            const size_t s_idx = bb_start_idx[b];
            const size_t e_idx = bb_end_idx[b];
            const uint32_t fall_bb = (b + 1u < num_bbs) ? (b + 1u) : kExitBB;
            if (s_idx >= e_idx) {
                bbs[b].term = (fall_bb < num_bbs) ? TermKind::Fallthrough : TermKind::EndPgm;
                bbs[b].true_bb = fall_bb;
                continue;
            }

            size_t body_end = e_idx;
            const auto& last = insns[e_idx - 1u];
            if (last.enc == Enc::SOPP && (last.op == 0x01u || last.op == 0x1Bu || last.op == 0x1Eu)) {
                bbs[b].term = TermKind::EndPgm;
                bbs[b].true_bb = kExitBB;
                ++translated_count_;
                body_end = e_idx - 1u;
            } else if (last.enc == Enc::SOPP && last.op == 0x02u) {
                bbs[b].term = TermKind::Jump;
                bbs[b].true_bb = resolve_tgt(last);
                ++translated_count_;
                body_end = e_idx - 1u;
            } else if (last.enc == Enc::SOPP && last.op >= 0x04u && last.op <= 0x09u) {
                bbs[b].term = TermKind::CondBranch;
                bbs[b].true_bb = resolve_tgt(last);
                bbs[b].false_bb = fall_bb;
                switch (last.op) {
                case 0x04u: bbs[b].cond = "!scc"; break;
                case 0x05u: bbs[b].cond = "scc"; break;
                case 0x06u: bbs[b].cond = "(vcc_lo | vcc_hi) == 0u"; break;
                case 0x07u: bbs[b].cond = "(vcc_lo | vcc_hi) != 0u"; break;
                case 0x08u: bbs[b].cond = "WaveActiveAllTrue(!lane_active)"; break;
                case 0x09u: bbs[b].cond = "!WaveActiveAllTrue(!lane_active)"; break;
                default: break;
                }
                ++translated_count_;
                body_end = e_idx - 1u;
            } else if (last.enc == Enc::SOPK && last.op == 0x16u) {
                bbs[b].term = TermKind::Jump;
                bbs[b].true_bb = resolve_tgt(last);
                ++translated_count_;
                body_end = e_idx - 1u;
            } else {
                bbs[b].term = (fall_bb < num_bbs) ? TermKind::Fallthrough : TermKind::EndPgm;
                bbs[b].true_bb = fall_bb;
            }

            line_sink_ = &bbs[b].lines;
            lds_hazard_ = LdsHazardState::None;
            for (size_t idx = s_idx; idx < body_end; ++idx) {
                emit_insn(insns[idx], pc_to_bb);
            }
            line_sink_ = nullptr;
            for (const auto& l : bbs[b].lines) {
                if (l.find("wsx_pc") != std::string::npos) {
                    has_inline_pc_mut = true;
                }
            }
        }

        // Optimization Pass 1: Static EXEC-mutability analysis & lane_active elision / block coalescing
        auto mutates_exec_line = [](const std::string& s) -> bool {
            return s.find("lane_active =") != std::string::npos ||
                   s.find("exec_lo =") != std::string::npos ||
                   s.find("exec_hi =") != std::string::npos;
        };
        auto unwrap_lane_guard = [](const std::string& s, std::string& inner) -> bool {
            static constexpr const char kPref[] = "if (lane_active) { ";
            static constexpr size_t kPrefLen = sizeof(kPref) - 1u;
            if (s.size() > kPrefLen + 2u &&
                s.compare(0u, kPrefLen, kPref) == 0 &&
                s.compare(s.size() - 2u, 2u, " }") == 0) {
                inner = s.substr(kPrefLen, s.size() - kPrefLen - 2u);
                return true;
            }
            return false;
        };
        auto has_local_var_decl = [](const std::string& s) -> bool {
            static constexpr const char* kDecls[] = {
                "uint ", "int ", "float ", "bool ", "double ",
                "uint2 ", "uint3 ", "uint4 ", "int2 ", "int4 ",
                "float2 ", "float3 ", "float4 ", "uint64_t ", "int64_t "
            };
            for (const char* d : kDecls) {
                if (s.find(d) != std::string::npos) return true;
            }
            return false;
        };

        bool shader_modifies_exec = false;
        for (const auto& bb : bbs) {
            for (const auto& l : bb.lines) {
                if (mutates_exec_line(l)) {
                    shader_modifies_exec = true;
                    break;
                }
            }
            if (shader_modifies_exec) break;
        }
        exec_elided_ = !shader_modifies_exec;

        // Pre-compute whether BB 0 has any incoming edges (so we know if entry prologue is unguarded)
        bool bb0_has_pred = false;
        for (const auto& bb : bbs) {
            if (bb.term == TermKind::Jump || bb.term == TermKind::Fallthrough) {
                if (bb.true_bb == 0u) bb0_has_pred = true;
            } else if (bb.term == TermKind::CondBranch) {
                if (bb.true_bb == 0u || bb.false_bb == 0u) bb0_has_pred = true;
            }
        }

        for (uint32_t b = 0; b < num_bbs; ++b) {
            std::vector<std::string> opt_lines;
            opt_lines.reserve(bbs[b].lines.size());
            if (!shader_modifies_exec) {
                for (const auto& l : bbs[b].lines) {
                    std::string inner;
                    if (unwrap_lane_guard(l, inner)) {
                        ++coalesced_guards_;
                        if (has_local_var_decl(inner)) {
                            opt_lines.push_back("{ " + inner + " }");
                        } else {
                            opt_lines.push_back(std::move(inner));
                        }
                    } else if (l.compare(0u, 18u, "if (lane_active) {") == 0) {
                        // Multi-line `if (lane_active) { ...` block -> plain lexical scope `{ ...`
                        ++coalesced_guards_;
                        opt_lines.push_back(l.substr(17u));
                    } else {
                        std::string cur = l;
                        const size_t pos = cur.find("lane_active && ");
                        if (pos != std::string::npos) {
                            cur.erase(pos, 15u);
                        }
                        const size_t rfl_pos = cur.find(", lane_active, wsx_gi)");
                        if (rfl_pos != std::string::npos) {
                            cur.replace(rfl_pos, 22u, ", true, wsx_gi)");
                        }
                        size_t wc_pos = 0u;
                        while ((wc_pos = cur.find("WaveActiveCountBits(lane_active)", wc_pos)) != std::string::npos) {
                            cur.replace(wc_pos, 32u, "WaveActiveCountBits(true)");
                            wc_pos += 25u;
                        }
                        size_t wp_pos = 0u;
                        while ((wp_pos = cur.find("WavePrefixCountBits(lane_active)", wp_pos)) != std::string::npos) {
                            cur.replace(wp_pos, 32u, "WavePrefixCountBits(true)");
                            wp_pos += 25u;
                        }
                        size_t ila_pos = 0u;
                        while ((ila_pos = cur.find("if (lane_active) {", ila_pos)) != std::string::npos) {
                            cur.replace(ila_pos, 18u, "{");
                            ila_pos += 1u;
                        }
                        opt_lines.push_back(std::move(cur));
                    }
                }
            } else {
                // Coalesce consecutive `if (lane_active) { ... }` statements (single & multi-line) into one block,
                // and lower isolated single-statement pure VALU writes (or all pure VALU writes in "all" mode)
                // to branchless ternary `vN = lane_active ? (expr) : vN;` to eliminate DXIL branch/phi explosion.
                const std::string branchless_mode = wsx::detail::to_lower_ascii(
                    wsx::get_config_string("WSX_RDNA2_BRANCHLESS_VALU", "Graphics", "rdna2_branchless_valu", "isolated"));
                const bool branchless_isolated = (branchless_mode != "off" && branchless_mode != "0" && branchless_mode != "false");
                const bool branchless_all = (branchless_mode == "all" || branchless_mode == "always");

                auto is_pure_vgpr_assign = [&](const std::string& stmt, std::string& out_lhs, std::string& out_rhs) -> bool {
                    if (stmt.size() < 7u || stmt[0] != 'v' || stmt.back() != ';') return false;
                    if (has_local_var_decl(stmt)) return false;
                    size_t i = 1u;
                    while (i < stmt.size() && stmt[i] >= '0' && stmt[i] <= '9') ++i;
                    if (i == 1u || i + 3u >= stmt.size()) return false;
                    if (stmt.compare(i, 3u, " = ") != 0) return false;
                    if (stmt.find(';') != stmt.size() - 1u) return false;
                    if (stmt.find('{') != std::string::npos || stmt.find('}') != std::string::npos) return false;
                    const std::string rhs = stmt.substr(i + 3u, stmt.size() - (i + 4u));
                    static constexpr const char* kImpure[] = {
                        "wsx_lds", "wsx_load", "wsx_store", "wsx_tex", "wsx_bvh", "wsx_w64_",
                        "Interlocked", "Wave", "Quad", "ResourceDescriptorHeap", "SamplerDescriptorHeap",
                        "GroupMemoryBarrier", "WSX_VREL"
                    };
                    for (const char* tok : kImpure) {
                        if (rhs.find(tok) != std::string::npos) return false;
                    }
                    out_lhs = stmt.substr(0u, i);
                    out_rhs = rhs;
                    return true;
                };

                bool prologue_full = (b == 0u && !bb0_has_pred);
                bool guard_open = false;
                uint32_t run_len = 0u;
                std::string single_pure_lhs;
                std::string single_pure_rhs;
                bool single_is_pure = false;
                int multi_brace_depth = 0;
                auto brace_delta = [](const std::string& s) -> int {
                    int d = 0;
                    for (char ch : s) {
                        if (ch == '{') ++d;
                        else if (ch == '}') --d;
                    }
                    return d;
                };
                auto close_guard = [&]() {
                    if (guard_open) {
                        if (branchless_isolated && run_len == 1u && single_is_pure && opt_lines.size() >= 2u) {
                            opt_lines.pop_back();
                            opt_lines.pop_back();
                            opt_lines.push_back(single_pure_lhs + " = lane_active ? (" + single_pure_rhs + ") : " + single_pure_lhs + ";");
                            ++branchless_selects_;
                            ++coalesced_guards_;
                        } else {
                            opt_lines.push_back("}");
                            if (run_len > 1u) coalesced_guards_ += (run_len - 1u);
                        }
                        guard_open = false;
                        run_len = 0u;
                        single_is_pure = false;
                        single_pure_lhs.clear();
                        single_pure_rhs.clear();
                    }
                };
                for (const auto& l : bbs[b].lines) {
                    if (multi_brace_depth > 0) {
                        opt_lines.push_back(guard_open ? ("    " + l) : l);
                        multi_brace_depth += brace_delta(l);
                        if (multi_brace_depth < 0) multi_brace_depth = 0;
                        single_is_pure = false;
                        continue;
                    }
                    std::string inner;
                    if (unwrap_lane_guard(l, inner)) {
                        if (prologue_full) {
                            ++coalesced_guards_;
                            if (has_local_var_decl(inner)) {
                                opt_lines.push_back("{ " + inner + " }");
                            } else {
                                opt_lines.push_back(std::move(inner));
                            }
                        } else {
                            std::string lhs, rhs;
                            const bool pure_assign = is_pure_vgpr_assign(inner, lhs, rhs);
                            if (branchless_all && pure_assign) {
                                close_guard();
                                opt_lines.push_back(lhs + " = lane_active ? (" + rhs + ") : " + lhs + ";");
                                ++branchless_selects_;
                                ++coalesced_guards_;
                                continue;
                            }
                            if (!guard_open) {
                                opt_lines.push_back("if (lane_active) {");
                                guard_open = true;
                                run_len = 0u;
                                single_is_pure = pure_assign;
                                if (pure_assign) {
                                    single_pure_lhs = std::move(lhs);
                                    single_pure_rhs = std::move(rhs);
                                }
                            } else {
                                single_is_pure = false;
                            }
                            ++run_len;
                            if (has_local_var_decl(inner)) {
                                opt_lines.push_back("    { " + inner + " }");
                            } else {
                                opt_lines.push_back("    " + inner);
                            }
                        }
                    } else if (l.compare(0u, 18u, "if (lane_active) {") == 0) {
                        const std::string rest = l.substr(17u);
                        multi_brace_depth = brace_delta(rest);
                        if (prologue_full) {
                            ++coalesced_guards_;
                            opt_lines.push_back(rest);
                        } else {
                            if (!guard_open) {
                                opt_lines.push_back("if (lane_active) {");
                                guard_open = true;
                                run_len = 0u;
                            }
                            single_is_pure = false;
                            ++run_len;
                            opt_lines.push_back("    " + rest);
                        }
                    } else {
                        close_guard();
                        if (mutates_exec_line(l)) prologue_full = false;
                        opt_lines.push_back(l);
                    }
                }
                close_guard();
            }
            bbs[b].lines = std::move(opt_lines);
        }

        // Optimization Pass 1B: Intra-Block Backward Dead-SCC Store Elimination & Peephole Folding
        auto fold_identity_casts = [&](std::string& line) {
            auto fold_pair = [&](const char* outer_open, size_t open_len) {
                size_t pos = 0u;
                while ((pos = line.find(outer_open, pos)) != std::string::npos) {
                    const size_t arg_start = pos + open_len;
                    size_t close_inner = arg_start;
                    bool simple = true;
                    while (close_inner < line.size() && line[close_inner] != ')') {
                        const char ch = line[close_inner];
                        if (ch == '(' || ch == ',' || ch == ';' || ch == ' ') {
                            simple = false;
                            break;
                        }
                        ++close_inner;
                    }
                    if (simple && close_inner > arg_start &&
                        close_inner + 1u < line.size() &&
                        line[close_inner] == ')' && line[close_inner + 1u] == ')') {
                        const std::string inner_tok = line.substr(arg_start, close_inner - arg_start);
                        line.replace(pos, (close_inner + 2u) - pos, inner_tok);
                        ++peephole_folds_;
                        pos += inner_tok.size();
                    } else {
                        pos = arg_start;
                    }
                }
            };
            fold_pair("asuint(asfloat(", 15u);
            fold_pair("asfloat(asuint(", 15u);
        };

        auto is_trivial_self_move = [](const std::string& trimmed) -> bool {
            const size_t eq = trimmed.find(" = ");
            if (eq == std::string::npos || trimmed.empty() || trimmed.back() != ';') return false;
            if (trimmed.find(';') != trimmed.size() - 1u) return false;
            const std::string lhs = trimmed.substr(0u, eq);
            const std::string rhs = trimmed.substr(eq + 3u, trimmed.size() - (eq + 4u));
            if (lhs != rhs || lhs.size() < 2u) return false;
            if (lhs[0] != 'v' && lhs[0] != 's') return false;
            for (size_t k = 1u; k < lhs.size(); ++k) {
                if (lhs[k] < '0' || lhs[k] > '9') return false;
            }
            return true;
        };

        for (uint32_t b = 0; b < num_bbs; ++b) {
            auto& lines = bbs[b].lines;
            // Forward pass: identity cast folding + pure comment/self-move pruning
            std::vector<std::string> cleaned;
            cleaned.reserve(lines.size());
            for (auto& l : lines) {
                fold_identity_casts(l);
                size_t first_nb = 0u;
                while (first_nb < l.size() && l[first_nb] == ' ') ++first_nb;
                const std::string tr = l.substr(first_nb);
                if (tr.size() >= 4u && tr[0] == '/' && tr[1] == '*' &&
                    tr[tr.size() - 2u] == '*' && tr.back() == '/') {
                    ++peephole_folds_;
                    continue;
                }
                if (is_trivial_self_move(tr)) {
                    ++peephole_folds_;
                    continue;
                }
                cleaned.push_back(std::move(l));
            }
            lines = std::move(cleaned);

            // Backward intra-block liveness pass for dead `scc = ...;` writes
            bool scc_live = (bbs[b].term != TermKind::EndPgm);
            std::vector<bool> keep(lines.size(), true);
            for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i) {
                std::string& l = lines[static_cast<size_t>(i)];
                if (l.find("scc") == std::string::npos) continue;

                size_t first_nb = 0u;
                while (first_nb < l.size() && l[first_nb] == ' ') ++first_nb;
                const std::string tr = l.substr(first_nb);

                // Standalone `scc = <expr>;`
                if (tr.compare(0u, 6u, "scc = ") == 0 && !tr.empty() && tr.back() == ';' &&
                    tr.find(';') == tr.size() - 1u) {
                    const bool rhs_reads_scc = (tr.find("scc", 6u) != std::string::npos);
                    if (!scc_live) {
                        keep[static_cast<size_t>(i)] = false;
                        ++dead_scc_elided_;
                    }
                    scc_live = rhs_reads_scc;
                    continue;
                }

                // Trailing `; scc = <expr>;` or `; scc = <expr>; }` on an unconditional scalar statement
                const size_t scc_pos = l.rfind("; scc = ");
                if (scc_pos != std::string::npos && l.find("if (") == std::string::npos) {
                    const std::string prefix = l.substr(0u, scc_pos + 1u);
                    const std::string tail = l.substr(scc_pos + 8u);
                    const bool ends_plain = (!tail.empty() && tail.back() == ';' && tail.find(';') == tail.size() - 1u);
                    const bool ends_brace = (tail.size() >= 3u &&
                                             tail.compare(tail.size() - 3u, 3u, "; }") == 0 &&
                                             tail.find(';') == tail.size() - 3u);
                    if (ends_plain || ends_brace) {
                        const bool reads_scc = (prefix.find("scc") != std::string::npos) ||
                                               (tail.find("scc") != std::string::npos);
                        if (!scc_live) {
                            l = ends_brace ? (prefix + " }") : prefix;
                            ++dead_scc_elided_;
                        }
                        scc_live = reads_scc;
                        continue;
                    }
                }

                // Any other mention (e.g., `if (scc)`, `s_cselect`, conditional write): conservatively live
                scc_live = true;
            }

            bool any_removed = false;
            for (bool k : keep) { if (!k) { any_removed = true; break; } }
            if (any_removed) {
                std::vector<std::string> compacted;
                compacted.reserve(lines.size());
                for (size_t i = 0; i < lines.size(); ++i) {
                    if (keep[i]) compacted.push_back(std::move(lines[i]));
                }
                lines = std::move(compacted);
            }

            // Optimization Pass 1C: Intra-Block Backward Dead VGPR/SGPR Store Elimination
            auto is_id_char = [](char c) noexcept -> bool {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_';
            };
            auto scan_reg_reads = [&](const std::string& s, size_t start_pos,
                                      uint64_t live_v[4], uint64_t live_s[2]) {
                const size_t n = s.size();
                for (size_t p = start_pos; p < n; ++p) {
                    const char ch = s[p];
                    if ((ch == 'v' || ch == 's') && (p == 0u || !is_id_char(s[p - 1u]))) {
                        size_t q = p + 1u;
                        if (q < n && s[q] >= '0' && s[q] <= '9') {
                            uint32_t idx = 0u;
                            while (q < n && s[q] >= '0' && s[q] <= '9') {
                                idx = idx * 10u + static_cast<uint32_t>(s[q] - '0');
                                ++q;
                            }
                            if (q == n || !is_id_char(s[q])) {
                                if (ch == 'v' && idx < 256u) live_v[idx >> 6] |= (1ull << (idx & 63u));
                                else if (ch == 's' && idx < 128u) live_s[idx >> 6] |= (1ull << (idx & 63u));
                            }
                            p = q - 1u;
                        }
                    }
                }
            };
            auto is_pure_reg_assign = [&](const std::string& tr, char& out_kind, uint32_t& out_idx, size_t& out_rhs_pos) -> bool {
                if (tr.size() < 6u || tr.back() != ';') return false;
                if (tr.find(';') != tr.size() - 1u) return false;
                const char k = tr[0];
                if (k != 'v' && k != 's') return false;
                size_t q = 1u;
                if (q >= tr.size() || tr[q] < '0' || tr[q] > '9') return false;
                uint32_t idx = 0u;
                while (q < tr.size() && tr[q] >= '0' && tr[q] <= '9') {
                    idx = idx * 10u + static_cast<uint32_t>(tr[q] - '0');
                    ++q;
                }
                if ((k == 'v' && idx >= 256u) || (k == 's' && idx >= 128u)) return false;
                if (q + 3u >= tr.size() || tr.compare(q, 3u, " = ") != 0) return false;
                const size_t rhs_pos = q + 3u;
                // Must have zero side effects (no memory/atomic/wave/barrier/discard/export)
                if (tr.find("wsx_va_", rhs_pos) != std::string::npos ||
                    tr.find("Interlocked", rhs_pos) != std::string::npos ||
                    tr.find("Wave", rhs_pos) != std::string::npos ||
                    tr.find("_wsx_w64_", rhs_pos) != std::string::npos ||
                    tr.find("GroupMemoryBarrier", rhs_pos) != std::string::npos ||
                    tr.find("wsx_lds", rhs_pos) != std::string::npos ||
                    tr.find("wsx_scratch", rhs_pos) != std::string::npos ||
                    tr.find("wsx_tex_", rhs_pos) != std::string::npos ||
                    tr.find("wsx_vrel_", rhs_pos) != std::string::npos) {
                    return false;
                }
                out_kind = k;
                out_idx = idx;
                out_rhs_pos = rhs_pos;
                return true;
            };

            uint64_t live_v[4]{};
            uint64_t live_s[2]{};
            const bool init_live = (bbs[b].term != TermKind::EndPgm) || uses_vrel_;
            if (init_live) {
                for (int w = 0; w < 4; ++w) live_v[w] = ~0ull;
                for (int w = 0; w < 2; ++w) live_s[w] = ~0ull;
            } else if (stage_ == 12u) {
                // AS epilogue may read s0..s3 / v0..v3 into WsxAsPayload when LDS is unused
                live_s[0] |= 0xFull;
                live_v[0] |= 0xFull;
            }

            std::vector<bool> keep_reg(lines.size(), true);
            bool any_reg_removed = false;
            for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i) {
                const std::string& l = lines[static_cast<size_t>(i)];
                size_t first_nb = 0u;
                while (first_nb < l.size() && l[first_nb] == ' ') ++first_nb;
                const std::string tr = l.substr(first_nb);

                char kind = 0;
                uint32_t ridx = 0u;
                size_t rhs_pos = 0u;
                if (is_pure_reg_assign(tr, kind, ridx, rhs_pos)) {
                    const bool is_live = (kind == 'v')
                        ? (((live_v[ridx >> 6] >> (ridx & 63u)) & 1ull) != 0ull)
                        : (((live_s[ridx >> 6] >> (ridx & 63u)) & 1ull) != 0ull);
                    if (!is_live) {
                        keep_reg[static_cast<size_t>(i)] = false;
                        any_reg_removed = true;
                        ++dead_reg_elided_;
                        continue;
                    }
                    // Overwritten unconditionally -> clear target liveness, then mark RHS reads live
                    if (kind == 'v') live_v[ridx >> 6] &= ~(1ull << (ridx & 63u));
                    else             live_s[ridx >> 6] &= ~(1ull << (ridx & 63u));
                    scan_reg_reads(tr, rhs_pos, live_v, live_s);
                } else {
                    scan_reg_reads(l, 0u, live_v, live_s);
                }
            }
            if (any_reg_removed) {
                std::vector<std::string> compacted;
                compacted.reserve(lines.size());
                for (size_t i = 0; i < lines.size(); ++i) {
                    if (keep_reg[i]) compacted.push_back(std::move(lines[i]));
                }
                lines = std::move(compacted);
            }
        }

        // Optimization Pass 2: CFG Reachability, Acyclicity, Dominators, Natural Loops & Post-Dominators
        std::vector<std::vector<uint32_t>> succs(num_bbs);
        for (uint32_t b = 0; b < num_bbs; ++b) {
            if (bbs[b].term == TermKind::Fallthrough || bbs[b].term == TermKind::Jump) {
                succs[b].push_back(bbs[b].true_bb);
            } else if (bbs[b].term == TermKind::CondBranch) {
                succs[b].push_back(bbs[b].true_bb);
                if (bbs[b].false_bb != bbs[b].true_bb) succs[b].push_back(bbs[b].false_bb);
            } else {
                succs[b].push_back(kExitBB);
            }
        }

        std::vector<bool> reachable(num_bbs, false);
        std::vector<uint32_t> reach_list;
        {
            std::queue<uint32_t> q;
            reachable[0] = true;
            q.push(0u);
            while (!q.empty()) {
                const uint32_t u = q.front();
                q.pop();
                reach_list.push_back(u);
                for (uint32_t v : succs[u]) {
                    if (v < num_bbs && !reachable[v]) {
                        reachable[v] = true;
                        q.push(v);
                    }
                }
            }
        }

        std::vector<std::vector<uint32_t>> preds(num_bbs + 1u);
        std::vector<uint32_t> in_deg(num_bbs, 0u);
        for (uint32_t u : reach_list) {
            for (uint32_t v : succs[u]) {
                preds[v].push_back(u);
                if (v < num_bbs) ++in_deg[v];
            }
        }

        // Kahn topological sort to test pure DAG acyclicity
        std::vector<uint32_t> topo_order;
        topo_order.reserve(reach_list.size());
        {
            std::vector<uint32_t> deg = in_deg;
            std::queue<uint32_t> q;
            for (uint32_t u : reach_list) {
                if (deg[u] == 0u) q.push(u);
            }
            while (!q.empty()) {
                const uint32_t u = q.front();
                q.pop();
                topo_order.push_back(u);
                for (uint32_t v : succs[u]) {
                    if (v < num_bbs && --deg[v] == 0u) q.push(v);
                }
            }
        }
        const bool is_dag = (topo_order.size() == reach_list.size());

        // Compute Dominators & Post-Dominators (for reasonable CFG sizes <= 512 BBs)
        const bool can_analyze_dom = (!has_inline_pc_mut && num_bbs <= 512u);
        const size_t bit_words = (num_bbs + 64u) / 64u;
        std::vector<std::vector<uint64_t>> dom;
        std::vector<uint32_t> ipdom(num_bbs, kNoBB);
        std::vector<bool> can_reach_exit(num_bbs + 1u, false);
        std::vector<std::vector<uint64_t>> pdom;
        std::vector<uint32_t> pdom_cnt(num_bbs + 1u, 0u);
        std::vector<bool> is_loop_hdr(num_bbs, false);
        std::vector<bool> loop_reducible(num_bbs, false);
        std::vector<std::vector<uint64_t>> loop_body;
        std::vector<uint32_t> loop_exit(num_bbs, kExitBB);
        std::vector<std::vector<uint32_t>> loop_exits(num_bbs);

        auto bit_get = [](const std::vector<uint64_t>& bs, uint32_t i) -> bool {
            return ((bs[i >> 6] >> (i & 63u)) & 1ull) != 0ull;
        };
        auto bit_set = [](std::vector<uint64_t>& bs, uint32_t i) {
            bs[i >> 6] |= (1ull << (i & 63u));
        };

        if (can_analyze_dom && num_bbs > 1u) {
            // Dominators
            dom.assign(num_bbs, std::vector<uint64_t>(bit_words, ~0ull));
            dom[0].assign(bit_words, 0ull);
            bit_set(dom[0], 0u);
            bool changed = true;
            while (changed) {
                changed = false;
                for (uint32_t u : reach_list) {
                    if (u == 0u || preds[u].empty()) continue;
                    std::vector<uint64_t> nd(bit_words, ~0ull);
                    for (uint32_t p : preds[u]) {
                        for (size_t w = 0; w < bit_words; ++w) nd[w] &= dom[p][w];
                    }
                    bit_set(nd, u);
                    if (nd != dom[u]) {
                        dom[u] = std::move(nd);
                        changed = true;
                    }
                }
            }

            // Natural Loops from back-edges (u -> h where h dominates u)
            loop_body.assign(num_bbs, std::vector<uint64_t>(bit_words, 0ull));
            for (uint32_t u : reach_list) {
                for (uint32_t h : succs[u]) {
                    if (h < num_bbs && bit_get(dom[u], h)) {
                        is_loop_hdr[h] = true;
                        bit_set(loop_body[h], h);
                        if (!bit_get(loop_body[h], u)) {
                            std::queue<uint32_t> q;
                            bit_set(loop_body[h], u);
                            q.push(u);
                            while (!q.empty()) {
                                const uint32_t cur = q.front();
                                q.pop();
                                for (uint32_t p : preds[cur]) {
                                    if (p < num_bbs && !bit_get(loop_body[h], p)) {
                                        bit_set(loop_body[h], p);
                                        q.push(p);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            for (uint32_t h : reach_list) {
                if (!is_loop_hdr[h]) continue;
                bool red = true;
                std::set<uint32_t> exits;
                for (uint32_t u : reach_list) {
                    if (!bit_get(loop_body[h], u)) continue;
                    if (u != h) {
                        for (uint32_t p : preds[u]) {
                            if (p < num_bbs && !bit_get(loop_body[h], p)) {
                                red = false;
                                break;
                            }
                        }
                    }
                    for (uint32_t v : succs[u]) {
                        if (v == kExitBB || !bit_get(loop_body[h], v)) {
                            exits.insert(v);
                        }
                    }
                }
                std::vector<uint32_t> non_exits;
                uint32_t chosen_exit = kExitBB;
                for (uint32_t e : exits) {
                    if (e != kExitBB) {
                        non_exits.push_back(e);
                        chosen_exit = e;
                    }
                }
                loop_reducible[h] = red && (non_exits.size() <= 8u);
                loop_exit[h] = chosen_exit;
                loop_exits[h] = std::move(non_exits);
            }

            // Post-Dominators from synthetic kExitBB
            {
                std::queue<uint32_t> q;
                can_reach_exit[kExitBB] = true;
                q.push(kExitBB);
                while (!q.empty()) {
                    const uint32_t v = q.front();
                    q.pop();
                    for (uint32_t p : preds[v]) {
                        if (!can_reach_exit[p]) {
                            can_reach_exit[p] = true;
                            q.push(p);
                        }
                    }
                }
            }
            pdom.assign(num_bbs + 1u, std::vector<uint64_t>(bit_words, ~0ull));
            pdom[kExitBB].assign(bit_words, 0ull);
            bit_set(pdom[kExitBB], kExitBB);
            changed = true;
            while (changed) {
                changed = false;
                for (uint32_t u : reach_list) {
                    if (!can_reach_exit[u]) continue;
                    std::vector<uint64_t> np(bit_words, ~0ull);
                    bool any_succ = false;
                    for (uint32_t v : succs[u]) {
                        if (!can_reach_exit[v]) continue;
                        any_succ = true;
                        for (size_t w = 0; w < bit_words; ++w) np[w] &= pdom[v][w];
                    }
                    if (!any_succ) continue;
                    bit_set(np, u);
                    if (np != pdom[u]) {
                        pdom[u] = std::move(np);
                        changed = true;
                    }
                }
            }
            auto popcount_bs = [](const std::vector<uint64_t>& bs) -> uint32_t {
                uint32_t c = 0u;
                for (uint64_t w : bs) {
                    while (w) { w &= (w - 1ull); ++c; }
                }
                return c;
            };
            for (uint32_t u = 0; u <= num_bbs; ++u) pdom_cnt[u] = popcount_bs(pdom[u]);
            for (uint32_t u : reach_list) {
                if (!can_reach_exit[u]) continue;
                uint32_t best = kNoBB;
                uint32_t best_cnt = 0u;
                for (uint32_t p = 0; p <= num_bbs; ++p) {
                    if (p == u || !bit_get(pdom[u], p)) continue;
                    if (pdom_cnt[p] > best_cnt) {
                        best_cnt = pdom_cnt[p];
                        best = p;
                    }
                }
                ipdom[u] = best;
            }
        }

        // Optimization Pass 3: Select CFG Lowering Tier (StraightLine -> Relooper -> TopoDag -> StateMachine)
        const std::string cfg_pref = wsx::detail::to_lower_ascii(
            wsx::get_config_string("WSX_RDNA2_CFG_MODE", "Graphics", "rdna2_cfg_mode", "auto"));
        const bool force_sm = (cfg_pref == "statemachine");
        const bool force_topo = (cfg_pref == "topodag");

        auto indent_str = [](int depth) -> std::string {
            return std::string(static_cast<size_t>(depth * 4), ' ');
        };
        auto append_bb_lines = [&](std::string& dst, uint32_t b, int depth) {
            const std::string ind = indent_str(depth);
            for (const auto& l : bbs[b].lines) {
                dst += ind;
                dst += l;
                dst += "\n";
            }
        };

        const bool ps_has_outputs = (stage_ == 0u) && (!ps_mrts_exported_.empty() || ps_depth_exported_);
        const uint32_t total_threads_est = std::clamp<uint32_t>(ntx * nty * ntz, 1u, 1024u);
        uint32_t max_vs_param_est = 0u;
        for (uint32_t p : vs_params_exported_) max_vs_param_est = std::max(max_vs_param_est, p + 1u);
        const uint32_t ms_vert_bytes = (1u + max_vs_param_est) * 16u;
        const uint32_t ms_cap_by_payload = std::clamp<uint32_t>(12288u / std::max(ms_vert_bytes, 16u), 32u, 128u);
        const uint32_t ms_max_verts = std::min<uint32_t>(ms_cap_by_payload, std::max<uint32_t>(total_threads_est, 64u));
        const uint32_t ms_max_prims = std::min<uint32_t>(128u, std::max<uint32_t>(total_threads_est, 64u));

        const bool stage_early_return_safe = (stage_ == 0u || stage_ == 1u || stage_ == 5u ||
                                              (stage_ == 13u && !ms_saw_alloc_req_) || stage_ == 0xFFFFu);
        auto stage_return_stmt = [&]() -> std::string {
            if (ps_has_outputs) return "return wsx_ps_out;";
            if (stage_ == 1u) return "return wsx_out;";
            if (stage_ == 13u) {
                return "{ if (wsx_gi < " + std::to_string(ms_max_verts) + "u) wsx_verts[wsx_gi] = wsx_out; "
                       "if (wsx_gi < " + std::to_string(ms_max_prims) + "u) wsx_tris[wsx_gi] = min(wsx_prim, uint3(" +
                       std::to_string(ms_max_verts - 1u) + "u, " + std::to_string(ms_max_verts - 1u) + "u, " +
                       std::to_string(ms_max_verts - 1u) + "u)); return; }";
            }
            return "return;";
        };

        std::string cfg_code;
        bool lowered = false;

        // Tier 0: Single Straight-Line Basic Block (0 branches, 0 loops, 0 wsx_pc)
        if (!force_sm && !force_topo && !has_inline_pc_mut &&
            num_bbs == 1u && bbs[0].term == TermKind::EndPgm) {
            append_bb_lines(cfg_code, 0u, 1);
            cfg_mode_ = CfgLoweringMode::StraightLine;
            lowered = true;
        }

        // Tier 1: Structured Relooper (if/else + single-exit & multi-exit reducible natural loops)
        if (!lowered && !force_sm && !force_topo && can_analyze_dom && stage_early_return_safe) {
            std::string reloop_buf;
            reloop_buf.reserve(4096u);
            size_t budget = static_cast<size_t>(num_bbs) * 4u + 16u;
            std::vector<uint8_t> in_stack(num_bbs, 0u);
            uint32_t local_multi_exit_cnt = 0u;

            // Compute lowest common post-dominator for a multi-exit loop exit set
            auto compute_multi_exit_merge = [&](const std::vector<uint32_t>& exs) -> uint32_t {
                if (exs.empty()) return kExitBB;
                if (exs.size() == 1u) return exs[0];
                std::vector<uint64_t> common(bit_words, ~0ull);
                bool any_reach = false;
                for (uint32_t e : exs) {
                    if (e <= num_bbs && can_reach_exit[e]) {
                        any_reach = true;
                        for (size_t w = 0; w < bit_words; ++w) common[w] &= pdom[e][w];
                    }
                }
                if (!any_reach) return kExitBB;
                uint32_t best = kExitBB;
                uint32_t best_cnt = 0u;
                for (uint32_t p = 0; p <= num_bbs; ++p) {
                    if (!bit_get(common, p)) continue;
                    if (pdom_cnt[p] > best_cnt) {
                        best_cnt = pdom_cnt[p];
                        best = p;
                    }
                }
                return best;
            };

            auto find_exit_slot = [](uint32_t target, const std::vector<uint32_t>* lexits, uint32_t cur_lexit) -> int {
                if (lexits && lexits->size() >= 2u) {
                    for (size_t k = 0; k < lexits->size(); ++k) {
                        if ((*lexits)[k] == target) return static_cast<int>(k) + 1;
                    }
                    return 0;
                }
                return (target == cur_lexit) ? -1 : 0;
            };

            std::function<bool(uint32_t, uint32_t, uint32_t, uint32_t, const std::vector<uint32_t>*, int, int, bool)> emit_region;
            emit_region = [&](uint32_t bb, uint32_t stop_bb,
                              uint32_t cur_lhdr, uint32_t cur_lexit,
                              const std::vector<uint32_t>* cur_lexits, int cur_ldepth,
                              int depth, bool is_top_tail) -> bool {
                if (depth > 32) return false;
                while (bb != stop_bb) {
                    if (bb == kExitBB) {
                        if (!is_top_tail || cur_lhdr != kNoBB || stop_bb != kExitBB) {
                            reloop_buf += indent_str(depth) + stage_return_stmt() + "\n";
                        }
                        return true;
                    }
                    if (bb == cur_lhdr) {
                        reloop_buf += indent_str(depth) + "continue;\n";
                        return true;
                    }
                    if (cur_lhdr != kNoBB) {
                        const int ex_slot = find_exit_slot(bb, cur_lexits, cur_lexit);
                        if (ex_slot < 0) {
                            reloop_buf += indent_str(depth) + "break;\n";
                            return true;
                        }
                        if (ex_slot > 0) {
                            reloop_buf += indent_str(depth) + "{ _wex" + std::to_string(cur_ldepth) +
                                          " = " + std::to_string(ex_slot) + "u; break; }\n";
                            return true;
                        }
                    }
                    if (bb >= num_bbs || in_stack[bb]) return false;
                    if (cur_lhdr != kNoBB && !bit_get(loop_body[cur_lhdr], bb)) return false;
                    if (budget == 0u) return false;
                    --budget;

                    // Check if `bb` starts a new reducible natural loop (single-exit or multi-exit)
                    if (is_loop_hdr[bb] && bb != cur_lhdr) {
                        if (!loop_reducible[bb]) return false;
                        const auto& ex_list = loop_exits[bb];
                        const bool is_multi_exit = (ex_list.size() >= 2u);
                        const uint32_t lex = is_multi_exit ? compute_multi_exit_merge(ex_list) : loop_exit[bb];
                        if (cur_lhdr != kNoBB) {
                            auto ok_in_outer = [&](uint32_t node) -> bool {
                                return node == kExitBB || node == cur_lhdr ||
                                       find_exit_slot(node, cur_lexits, cur_lexit) != 0 ||
                                       (node < num_bbs && bit_get(loop_body[cur_lhdr], node));
                            };
                            if (!ok_in_outer(lex)) return false;
                            for (uint32_t e : ex_list) {
                                if (!ok_in_outer(e)) return false;
                            }
                        }

                        in_stack[bb] = 1u;
                        if (is_multi_exit) {
                            reloop_buf += indent_str(depth) + "uint _wex" + std::to_string(depth) + " = 0u;\n";
                        }
                        reloop_buf += indent_str(depth) + "[loop]\n";
                        reloop_buf += indent_str(depth) + "for (uint _wlp" + std::to_string(depth) +
                                      " = 0u; _wlp" + std::to_string(depth) + " < 65536u; ++_wlp" +
                                      std::to_string(depth) + ") {\n";
                        append_bb_lines(reloop_buf, bb, depth + 1);

                        const std::vector<uint32_t>* next_lexits = is_multi_exit ? &ex_list : nullptr;
                        bool ok_loop = false;
                        const auto& blk = bbs[bb];
                        auto break_stmt_for = [&](int slot) -> std::string {
                            if (slot > 0) return "{ _wex" + std::to_string(depth) + " = " + std::to_string(slot) + "u; break; }";
                            return "break;";
                        };
                        if (blk.term == TermKind::EndPgm) {
                            reloop_buf += indent_str(depth + 1) + stage_return_stmt() + "\n";
                            ok_loop = true;
                        } else if (blk.term == TermKind::Fallthrough || blk.term == TermKind::Jump) {
                            ok_loop = emit_region(blk.true_bb, bb, bb, lex, next_lexits, depth, depth + 1, false);
                        } else if (blk.term == TermKind::CondBranch) {
                            const uint32_t T = blk.true_bb;
                            const uint32_t F = blk.false_bb;
                            const int t_ex = find_exit_slot(T, next_lexits, lex);
                            const int f_ex = find_exit_slot(F, next_lexits, lex);
                            if (t_ex != 0 && F == bb) {
                                reloop_buf += indent_str(depth + 1) + "if (" + blk.cond + ") " + break_stmt_for(t_ex) + "\n";
                                ok_loop = true;
                            } else if (f_ex != 0 && T == bb) {
                                reloop_buf += indent_str(depth + 1) + "if (!(" + blk.cond + ")) " + break_stmt_for(f_ex) + "\n";
                                ok_loop = true;
                            } else if (t_ex != 0 && f_ex == 0) {
                                reloop_buf += indent_str(depth + 1) + "if (" + blk.cond + ") " + break_stmt_for(t_ex) + "\n";
                                ok_loop = emit_region(F, bb, bb, lex, next_lexits, depth, depth + 1, false);
                            } else if (f_ex != 0 && t_ex == 0) {
                                reloop_buf += indent_str(depth + 1) + "if (!(" + blk.cond + ")) " + break_stmt_for(f_ex) + "\n";
                                ok_loop = emit_region(T, bb, bb, lex, next_lexits, depth, depth + 1, false);
                            } else {
                                uint32_t merge = ipdom[bb];
                                if (merge == kNoBB || (merge != bb && find_exit_slot(merge, next_lexits, lex) == 0 &&
                                                       !bit_get(loop_body[bb], merge))) {
                                    merge = bb;
                                }
                                reloop_buf += indent_str(depth + 1) + "if (" + blk.cond + ") {\n";
                                ok_loop = emit_region(T, merge, bb, lex, next_lexits, depth, depth + 2, false);
                                reloop_buf += indent_str(depth + 1) + "} else {\n";
                                if (ok_loop) ok_loop = emit_region(F, merge, bb, lex, next_lexits, depth, depth + 2, false);
                                reloop_buf += indent_str(depth + 1) + "}\n";
                                if (ok_loop && merge != bb) {
                                    ok_loop = emit_region(merge, bb, bb, lex, next_lexits, depth, depth + 1, false);
                                }
                            }
                        }
                        in_stack[bb] = 0u;
                        if (!ok_loop) return false;
                        reloop_buf += indent_str(depth) + "}\n";

                        if (is_multi_exit) {
                            ++local_multi_exit_cnt;
                            for (size_t k = 0; k < ex_list.size(); ++k) {
                                reloop_buf += indent_str(depth) + (k == 0u ? "if (" : "else if (") +
                                              "_wex" + std::to_string(depth) + " == " + std::to_string(k + 1u) + "u) {\n";
                                if (!emit_region(ex_list[k], lex, cur_lhdr, cur_lexit, cur_lexits, cur_ldepth, depth + 1, false)) {
                                    return false;
                                }
                                reloop_buf += indent_str(depth) + "}\n";
                            }
                        }
                        bb = lex;
                        continue;
                    }

                    // Normal non-loop block
                    in_stack[bb] = 1u;
                    append_bb_lines(reloop_buf, bb, depth);
                    const auto& blk = bbs[bb];
                    if (blk.term == TermKind::EndPgm) {
                        in_stack[bb] = 0u;
                        if (!is_top_tail || cur_lhdr != kNoBB || stop_bb != kExitBB) {
                            reloop_buf += indent_str(depth) + stage_return_stmt() + "\n";
                        }
                        return true;
                    }
                    if (blk.term == TermKind::Fallthrough || blk.term == TermKind::Jump) {
                        in_stack[bb] = 0u;
                        bb = blk.true_bb;
                        continue;
                    }
                    // Conditional branch: structure around immediate post-dominator `merge`
                    const uint32_t T = blk.true_bb;
                    const uint32_t F = blk.false_bb;
                    uint32_t merge = ipdom[bb];
                    if (merge == kNoBB) merge = stop_bb;
                    if (cur_lhdr != kNoBB && merge != cur_lhdr &&
                        find_exit_slot(merge, cur_lexits, cur_lexit) == 0 &&
                        merge != kExitBB && !bit_get(loop_body[cur_lhdr], merge)) {
                        merge = (cur_lexits && cur_lexits->size() >= 2u) ? stop_bb : cur_lexit;
                    }
                    if (merge == bb) {
                        in_stack[bb] = 0u;
                        return false;
                    }

                    bool ok_br = true;
                    if (T == merge && F != merge) {
                        reloop_buf += indent_str(depth) + "if (!(" + blk.cond + ")) {\n";
                        ok_br = emit_region(F, merge, cur_lhdr, cur_lexit, cur_lexits, cur_ldepth, depth + 1, false);
                        reloop_buf += indent_str(depth) + "}\n";
                    } else if (F == merge && T != merge) {
                        reloop_buf += indent_str(depth) + "if (" + blk.cond + ") {\n";
                        ok_br = emit_region(T, merge, cur_lhdr, cur_lexit, cur_lexits, cur_ldepth, depth + 1, false);
                        reloop_buf += indent_str(depth) + "}\n";
                    } else if (T != merge && F != merge) {
                        reloop_buf += indent_str(depth) + "if (" + blk.cond + ") {\n";
                        ok_br = emit_region(T, merge, cur_lhdr, cur_lexit, cur_lexits, cur_ldepth, depth + 1, false);
                        reloop_buf += indent_str(depth) + "} else {\n";
                        if (ok_br) ok_br = emit_region(F, merge, cur_lhdr, cur_lexit, cur_lexits, cur_ldepth, depth + 1, false);
                        reloop_buf += indent_str(depth) + "}\n";
                    }
                    in_stack[bb] = 0u;
                    if (!ok_br) return false;
                    bb = merge;
                }
                return true;
            };

            if (emit_region(0u, kExitBB, kNoBB, kNoBB, nullptr, 0, 1, true)) {
                cfg_code = std::move(reloop_buf);
                cfg_mode_ = CfgLoweringMode::Relooper;
                multi_exit_loops_structured_ = local_multi_exit_cnt;
                lowered = true;
            }
        }

        // Tier 1.5: Topological Acyclic DAG (0 back-edges, 0 `for` loop, 0 loop-carried PHIs!)
        if (!lowered && !force_sm && !has_inline_pc_mut && is_dag) {
            cfg_code += "    uint wsx_pc = 0u;\n";
            for (uint32_t b : topo_order) {
                const auto& blk = bbs[b];
                cfg_code += "    if (wsx_pc == " + std::to_string(b) + "u) {\n";
                append_bb_lines(cfg_code, b, 2);
                if (blk.term == TermKind::EndPgm) {
                    cfg_code += "        wsx_pc = 0xFFFFFFFFu;\n";
                } else if (blk.term == TermKind::Fallthrough || blk.term == TermKind::Jump) {
                    cfg_code += "        wsx_pc = " +
                        ((blk.true_bb < num_bbs) ? (std::to_string(blk.true_bb) + "u") : std::string("0xFFFFFFFFu")) + ";\n";
                } else if (blk.term == TermKind::CondBranch) {
                    const std::string t_str = (blk.true_bb < num_bbs) ? (std::to_string(blk.true_bb) + "u") : "0xFFFFFFFFu";
                    const std::string f_str = (blk.false_bb < num_bbs) ? (std::to_string(blk.false_bb) + "u") : "0xFFFFFFFFu";
                    cfg_code += "        wsx_pc = (" + blk.cond + ") ? " + t_str + " : " + f_str + ";\n";
                }
                cfg_code += "    }\n";
            }
            cfg_mode_ = CfgLoweringMode::TopoDag;
            lowered = true;
        }

        // Tier 2: State-Machine Fallback (irreducible cyclic CFGs only)
        if (!lowered) {
            cfg_code += "    uint wsx_pc = 0u;\n";
            cfg_code += "    [loop]\n";
            cfg_code += "    for (uint wsx_step = 0u; wsx_step < 65536u && wsx_pc != 0xFFFFFFFFu; ++wsx_step) {\n";
            cfg_code += "        switch (wsx_pc) {\n";
            for (uint32_t b = 0; b < num_bbs; ++b) {
                if (!reachable[b]) continue;
                const auto& blk = bbs[b];
                cfg_code += "        case " + std::to_string(b) + "u: {\n";
                append_bb_lines(cfg_code, b, 3);
                if (blk.term == TermKind::EndPgm) {
                    cfg_code += "            wsx_pc = 0xFFFFFFFFu; break;\n";
                } else if (blk.term == TermKind::Fallthrough || blk.term == TermKind::Jump) {
                    cfg_code += "            wsx_pc = " +
                        ((blk.true_bb < num_bbs) ? (std::to_string(blk.true_bb) + "u") : std::string("0xFFFFFFFFu")) + "; break;\n";
                } else if (blk.term == TermKind::CondBranch) {
                    const std::string t_str = (blk.true_bb < num_bbs) ? (std::to_string(blk.true_bb) + "u") : "0xFFFFFFFFu";
                    const std::string f_str = (blk.false_bb < num_bbs) ? (std::to_string(blk.false_bb) + "u") : "0xFFFFFFFFu";
                    cfg_code += "            wsx_pc = (" + blk.cond + ") ? " + t_str + " : " + f_str + "; break;\n";
                }
                cfg_code += "        }\n";
            }
            cfg_code += "        default: wsx_pc = 0xFFFFFFFFu; break;\n";
            cfg_code += "        }\n";
            cfg_code += "    }\n";
            cfg_mode_ = CfgLoweringMode::StateMachine;
        }

        // Optimization Pass 4: Assemble stage declarations (omitting unused state vars) + Trimmed SM 6.6 Preamble
        const bool need_scc  = (cfg_code.find("scc") != std::string::npos);
        const bool need_vcc  = (cfg_code.find("vcc_") != std::string::npos);
        const bool need_exec = (cfg_code.find("exec_lo") != std::string::npos ||
                                cfg_code.find("exec_hi") != std::string::npos ||
                                cfg_code.find("lane_active") != std::string::npos);
        const bool need_m0   = (cfg_code.find("m0") != std::string::npos);
        const bool need_sink = (cfg_code.find("wsx_sink") != std::string::npos);

        const uint32_t total_threads = std::clamp<uint32_t>(ntx * nty * ntz, 1u, 1024u);
        const uint32_t num_w64 = (total_threads + 63u) / 64u;
        const bool need_w64_rfl = use_w64_emu() && (cfg_code.find("wsx_w64_readfirstlane") != std::string::npos);
        const bool need_w64_ballot = use_w64_emu() &&
            (need_exec || need_w64_rfl || cfg_code.find("wsx_w64_ballot") != std::string::npos);
        const bool need_w64_xchg = use_w64_emu() &&
            (need_w64_rfl || cfg_code.find("wsx_w64_readlane") != std::string::npos);
        const bool need_w64_lane = use_w64_emu() &&
            (need_w64_ballot || need_w64_xchg ||
             cfg_code.find("wsx_w64_lane") != std::string::npos ||
             cfg_code.find("wsx_w64_is_first") != std::string::npos ||
             cfg_code.find("wsx_w64_mask_bit") != std::string::npos);
        // Single-wave64 workgroups (<= 64 threads) and StraightLine shaders are mathematically guaranteed
        // workgroup-uniform for GroupMemoryBarrierWithGroupSync(); Force mode enables sync unconditionally.
        const bool can_w64_gs_sync =
            w64_force_mode_ || (total_threads <= 64u) || (cfg_mode_ == CfgLoweringMode::StraightLine);
        wave64_emulated_ = need_w64_lane;

        std::string shader_main;
        shader_main.reserve(cfg_code.size() + 3072u);
        shader_main += "\n// Auto-translated from Xbox Series X|S RDNA2 by MizuBox (by Mizushi) DX12 SM 6.6 Optimizer (cfg=";
        shader_main += cfg_mode_name(cfg_mode_);
        if (is_wave64_) {
            shader_main += ", w64=";
            shader_main += wave64_mode_name(w64_mode_);
            if (need_w64_lane && can_w64_gs_sync) shader_main += "_sync";
        }
        shader_main += ")\n";
        if (uses_lds_ && stage_has_workgroup()) {
            shader_main += "groupshared uint wsx_lds[" + std::to_string(lds_word_count()) + "];\n";
        }
        if (cfg_code.find("wsx_pack_f16x2(") != std::string::npos) {
            shader_main += "uint wsx_pack_f16x2(float16_t2 h) { return uint(asuint16(h.x)) | (uint(asuint16(h.y)) << 16u); }\n";
        }
        if (cfg_code.find("wsx_clamp_finite_f32(") != std::string::npos) {
            shader_main += "float wsx_clamp_finite_f32(float x) { return isnan(x) ? 0.0f : clamp(x, -65024.0f, 65024.0f); }\n";
        }
        if (need_w64_lane) {
            shader_main += "uint wsx_w64_lane(uint gi) { return (WaveGetLaneCount() >= 64u) ? WaveGetLaneIndex() : (gi & 63u); }\n";
            shader_main += "bool wsx_w64_is_first(uint gi) { return (WaveGetLaneCount() >= 64u) ? WaveIsFirstLane() : ((gi & 63u) == 0u); }\n";
            shader_main += "bool wsx_w64_mask_bit(uint lo, uint hi, uint gi) { uint l = wsx_w64_lane(gi); return (l < 32u) ? (((lo >> l) & 1u) != 0u) : (((hi >> (l - 32u)) & 1u) != 0u); }\n";
        }
        if (need_w64_ballot) {
            if (can_w64_gs_sync) {
                shader_main += "groupshared uint _wsx_w64_ballot[" + std::to_string(num_w64) + "][2];\n";
                shader_main += "uint2 wsx_w64_ballot(bool pred, uint gi) {\n";
                shader_main += "    if (WaveGetLaneCount() >= 64u) { uint4 _b = WaveActiveBallot(pred); return uint2(_b.x, _b.y); }\n";
                shader_main += "    uint _w = gi >> 6u; uint _h = (gi >> 5u) & 1u; uint _b32 = WaveActiveBallot(pred).x;\n";
                shader_main += "    if (WaveIsFirstLane()) { _wsx_w64_ballot[_w][_h] = _b32; }\n";
                shader_main += "    GroupMemoryBarrierWithGroupSync();\n";
                shader_main += "    uint2 _r = uint2(_wsx_w64_ballot[_w][0], _wsx_w64_ballot[_w][1]);\n";
                shader_main += "    GroupMemoryBarrierWithGroupSync();\n";
                shader_main += "    return _r;\n";
                shader_main += "}\n";
            } else {
                shader_main += "uint2 wsx_w64_ballot(bool pred, uint gi) {\n";
                shader_main += "    uint4 _b = WaveActiveBallot(pred);\n";
                shader_main += "    if (WaveGetLaneCount() >= 64u) return uint2(_b.x, _b.y);\n";
                shader_main += "    return ((gi & 32u) == 0u) ? uint2(_b.x, 0u) : uint2(0u, _b.x);\n";
                shader_main += "}\n";
            }
        }
        if (need_w64_xchg) {
            if (can_w64_gs_sync) {
                shader_main += "groupshared uint _wsx_w64_xchg[" + std::to_string(total_threads) + "];\n";
                shader_main += "uint wsx_w64_readlane(uint val, uint src_lane, uint gi) {\n";
                shader_main += "    if (WaveGetLaneCount() >= 64u) return WaveReadLaneAt(val, src_lane & 63u);\n";
                shader_main += "    if (gi < " + std::to_string(total_threads) + "u) { _wsx_w64_xchg[gi] = val; }\n";
                shader_main += "    GroupMemoryBarrierWithGroupSync();\n";
                shader_main += "    uint _t = (gi & ~63u) | (src_lane & 63u);\n";
                shader_main += "    uint _r = (_t < " + std::to_string(total_threads) + "u) ? _wsx_w64_xchg[_t] : 0u;\n";
                shader_main += "    GroupMemoryBarrierWithGroupSync();\n";
                shader_main += "    return _r;\n";
                shader_main += "}\n";
            } else {
                shader_main += "uint wsx_w64_readlane(uint val, uint src_lane, uint gi) {\n";
                shader_main += "    return WaveReadLaneAt(val, src_lane & (WaveGetLaneCount() - 1u));\n";
                shader_main += "}\n";
            }
        }
        if (need_w64_rfl) {
            if (can_w64_gs_sync) {
                shader_main += "uint wsx_w64_readfirstlane(uint val, bool active, uint gi) {\n";
                shader_main += "    if (WaveGetLaneCount() >= 64u) return WaveReadLaneFirst(val);\n";
                shader_main += "    uint2 _b = wsx_w64_ballot(active, gi);\n";
                shader_main += "    uint _fl = (_b.x != 0u) ? firstbitlow(_b.x) : ((_b.y != 0u) ? (32u + firstbitlow(_b.y)) : 0u);\n";
                shader_main += "    return wsx_w64_readlane(val, _fl, gi);\n";
                shader_main += "}\n";
            } else {
                shader_main += "uint wsx_w64_readfirstlane(uint val, bool active, uint gi) {\n";
                shader_main += "    return WaveReadLaneFirst(val);\n";
                shader_main += "}\n";
            }
        }
        if (uses_vrel_) {
            shader_main += "#define WSX_VREL_READ(DST, IDX) { switch ((IDX) & 255u) { ";
            for (uint32_t vi : vgprs_used_) {
                shader_main += "case " + std::to_string(vi) + "u: (DST) = v" + std::to_string(vi) + "; break; ";
            }
            shader_main += "default: (DST) = 0u; break; } }\n";
            shader_main += "#define WSX_VREL_WRITE(IDX, VAL) { uint _wv = (VAL); switch ((IDX) & 255u) { ";
            for (uint32_t vi : vgprs_used_) {
                shader_main += "case " + std::to_string(vi) + "u: v" + std::to_string(vi) + " = _wv; break; ";
            }
            shader_main += "default: break; } }\n";
        }

        const bool emit_wavesize64 = (w64_mode_ == Wave64EmulationMode::Native64);
        const std::string ws64_attr = emit_wavesize64 ? "[WaveSize(64)]\n" : "";

        const uint32_t max_ps_attr = ps_attrs_used_.empty() ? 0u : (*ps_attrs_used_.rbegin() + 1u);
        const uint32_t max_vs_param = vs_params_exported_.empty() ? 0u : (*vs_params_exported_.rbegin() + 1u);
        if (stage_ == 0u) {
            shader_main += "\nstruct WsxPsIn {\n";
            shader_main += "    float4 pos : SV_Position;\n";
            for (uint32_t a = 0u; a < max_ps_attr; ++a) {
                shader_main += "    float4 p" + std::to_string(a) + " : TEXCOORD" + std::to_string(a) + ";\n";
            }
            shader_main += "};\n";
            if (ps_has_outputs) {
                shader_main += "struct WsxPsOut {\n";
                for (uint32_t m : ps_mrts_exported_) {
                    shader_main += "    float4 mrt" + std::to_string(m) + " : SV_Target" + std::to_string(m) + ";\n";
                }
                if (ps_depth_exported_) {
                    shader_main += "    float depth : SV_Depth;\n";
                }
                shader_main += "};\n";
                shader_main += "\nWsxPsOut main(WsxPsIn wsx_in) {\n";
                shader_main += "    WsxPsOut wsx_ps_out = (WsxPsOut)0;\n";
            } else {
                shader_main += "\nvoid main(WsxPsIn wsx_in) {\n";
            }
        } else if (stage_ == 1u) {
            shader_main += "\nstruct WsxVsOut {\n";
            shader_main += "    float4 pos : SV_Position;\n";
            for (uint32_t p = 0u; p < max_vs_param; ++p) {
                shader_main += "    float4 p" + std::to_string(p) + " : TEXCOORD" + std::to_string(p) + ";\n";
            }
            shader_main += "};\n";
            shader_main += "\nWsxVsOut main(uint wsx_vid : SV_VertexID, uint wsx_iid : SV_InstanceID) {\n";
            shader_main += "    WsxVsOut wsx_out = (WsxVsOut)0;\n";
            shader_main += "    wsx_out.pos = float4(0.0f, 0.0f, 0.0f, 1.0f);\n";
        } else if (stage_ == 2u) {
            shader_main += "\nstruct WsxGsIn {\n";
            shader_main += "    float4 pos : SV_Position;\n";
            shader_main += "};\n";
            shader_main += "struct WsxGsOut {\n";
            shader_main += "    float4 pos : SV_Position;\n";
            for (uint32_t p = 0u; p < max_vs_param; ++p) {
                shader_main += "    float4 p" + std::to_string(p) + " : TEXCOORD" + std::to_string(p) + ";\n";
            }
            shader_main += "};\n";
            shader_main += "\n[maxvertexcount(3)]\n";
            shader_main += "void main(triangle WsxGsIn wsx_in[3], inout TriangleStream<WsxGsOut> wsx_stream) {\n";
            shader_main += "    WsxGsOut wsx_out = (WsxGsOut)0;\n";
            shader_main += "    wsx_out.pos = wsx_in[0].pos;\n";
        } else if (stage_ == 12u) {
            shader_main += "\nstruct WsxAsPayload { uint data[16]; };\n";
            shader_main += "groupshared WsxAsPayload wsx_as_payload;\n";
            if (as_saw_dispatch_req_) {
                shader_main += "groupshared uint _wsx_as_dims[3];\n";
            }
            shader_main += "\n" + ws64_attr + "[numthreads(" + std::to_string(ntx) + ", " + std::to_string(nty) + ", " + std::to_string(ntz) + ")]\n";
            shader_main += "void main(uint3 wsx_gid : SV_GroupID, uint3 wsx_gtid : SV_GroupThreadID, uint wsx_gi : SV_GroupIndex) {\n";
            if (as_saw_dispatch_req_) {
                shader_main += "    if (wsx_gi == 0u) { _wsx_as_dims[0] = 1u; _wsx_as_dims[1] = 1u; _wsx_as_dims[2] = 1u; }\n";
                shader_main += "    GroupMemoryBarrierWithGroupSync();\n";
            }
        } else if (stage_ == 13u) {
            shader_main += "\nstruct WsxMsVert {\n";
            shader_main += "    float4 pos : SV_Position;\n";
            for (uint32_t p = 0u; p < max_vs_param; ++p) {
                shader_main += "    float4 p" + std::to_string(p) + " : TEXCOORD" + std::to_string(p) + ";\n";
            }
            shader_main += "};\n";
            if (ms_saw_alloc_req_) {
                shader_main += "groupshared uint _wsx_ms_counts[2];\n";
            }
            shader_main += "\n" + ws64_attr + "[outputtopology(\"triangle\")]\n";
            shader_main += "[numthreads(" + std::to_string(ntx) + ", " + std::to_string(nty) + ", " + std::to_string(ntz) + ")]\n";
            shader_main += "void main(uint3 wsx_gid : SV_GroupID, uint3 wsx_gtid : SV_GroupThreadID, uint wsx_gi : SV_GroupIndex,\n";
            shader_main += "          out vertices WsxMsVert wsx_verts[" + std::to_string(ms_max_verts) +
                           "], out indices uint3 wsx_tris[" + std::to_string(ms_max_prims) + "]) {\n";
            if (ms_saw_alloc_req_) {
                shader_main += "    if (wsx_gi == 0u) { _wsx_ms_counts[0] = " + std::to_string(ms_max_verts) +
                               "u; _wsx_ms_counts[1] = " + std::to_string(ms_max_prims) + "u; }\n";
                shader_main += "    GroupMemoryBarrierWithGroupSync();\n";
            } else {
                shader_main += "    SetMeshOutputCounts(" + std::to_string(ms_max_verts) + "u, " +
                               std::to_string(ms_max_prims) + "u);\n";
            }
            shader_main += "    WsxMsVert wsx_out = (WsxMsVert)0;\n";
            shader_main += "    wsx_out.pos = float4(0.0f, 0.0f, 0.0f, 1.0f);\n";
            shader_main += "    uint3 wsx_prim = uint3(0u, 1u, 2u);\n";
        } else {
            shader_main += "\n" + ws64_attr + "[numthreads(" + std::to_string(ntx) + ", " + std::to_string(nty) + ", " + std::to_string(ntz) + ")]\n";
            shader_main += "void main(uint3 wsx_gid : SV_GroupID, uint3 wsx_gtid : SV_GroupThreadID, uint wsx_gi : SV_GroupIndex) {\n";
        }

        if (need_w64_ballot && can_w64_gs_sync) {
            shader_main += "    if (wsx_gi < " + std::to_string(num_w64) + "u) { _wsx_w64_ballot[wsx_gi][0] = 0u; _wsx_w64_ballot[wsx_gi][1] = 0u; }\n";
            shader_main += "    GroupMemoryBarrierWithGroupSync();\n";
        }
        if (uses_lds_ && !stage_has_workgroup()) {
            shader_main += "    uint wsx_lds[64];\n";
            shader_main += "    [unroll] for (uint _li = 0u; _li < 64u; ++_li) { wsx_lds[_li] = 0u; }\n";
        }
        if (uses_scratch_) {
            shader_main += "    uint wsx_scratch[256];\n";
        }

        for (uint32_t si : sgprs_used_) {
            if (si < 32u) {
                if (stage_has_workgroup() && si == 4u) {
                    shader_main += "    uint s4 = (wsx_sgpr(4u) != 0u) ? wsx_sgpr(4u) : wsx_gid.x;\n";
                } else if (stage_has_workgroup() && si == 5u) {
                    shader_main += "    uint s5 = (wsx_sgpr(5u) != 0u) ? wsx_sgpr(5u) : wsx_gid.y;\n";
                } else if (stage_has_workgroup() && si == 6u) {
                    shader_main += "    uint s6 = (wsx_sgpr(6u) != 0u) ? wsx_sgpr(6u) : wsx_gid.z;\n";
                } else {
                    shader_main += "    uint s" + std::to_string(si) + " = wsx_sgpr(" + std::to_string(si) + "u);\n";
                }
            } else {
                shader_main += "    uint s" + std::to_string(si) + " = 0u;\n";
            }
        }
        for (uint32_t vi : vgprs_used_) {
            if (stage_has_workgroup()) {
                if (vi == 0u) shader_main += "    uint v0 = wsx_gtid.x;\n";
                else if (vi == 1u) shader_main += "    uint v1 = wsx_gtid.y;\n";
                else if (vi == 2u) shader_main += "    uint v2 = wsx_gtid.z;\n";
                else shader_main += "    uint v" + std::to_string(vi) + " = 0u;\n";
            } else if (stage_ == 0u) {
                if (vi == 0u) shader_main += "    uint v0 = asuint(wsx_in.pos.x);\n";
                else if (vi == 1u) shader_main += "    uint v1 = asuint(wsx_in.pos.y);\n";
                else if (vi == 2u) shader_main += "    uint v2 = asuint(wsx_in.pos.z);\n";
                else if (vi == 3u) shader_main += "    uint v3 = asuint(wsx_in.pos.w);\n";
                else shader_main += "    uint v" + std::to_string(vi) + " = 0u;\n";
            } else if (stage_ == 1u) {
                if (vi == 0u) shader_main += "    uint v0 = wsx_vid;\n";
                else if (vi == 3u) shader_main += "    uint v3 = wsx_iid;\n";
                else shader_main += "    uint v" + std::to_string(vi) + " = 0u;\n";
            } else if (stage_ == 2u) {
                if (vi == 0u) shader_main += "    uint v0 = asuint(wsx_in[0].pos.x);\n";
                else if (vi == 1u) shader_main += "    uint v1 = asuint(wsx_in[0].pos.y);\n";
                else if (vi == 2u) shader_main += "    uint v2 = asuint(wsx_in[0].pos.z);\n";
                else shader_main += "    uint v" + std::to_string(vi) + " = 0u;\n";
            } else {
                shader_main += "    uint v" + std::to_string(vi) + " = 0u;\n";
            }
        }

        if (need_scc)  shader_main += "    bool scc = false;\n";
        if (need_vcc) {
            shader_main += "    bool vcc_lane = false;\n";
            shader_main += "    uint vcc_lo = 0u;\n";
            shader_main += "    uint vcc_hi = 0u;\n";
        }
        if (need_exec) {
            shader_main += "    bool lane_active = true;\n";
            if (need_w64_ballot) {
                shader_main += "    uint2 wsx_init_exec = wsx_w64_ballot(true, wsx_gi);\n";
            } else {
                shader_main += "    uint4 wsx_init_exec = WaveActiveBallot(true);\n";
            }
            shader_main += "    uint exec_lo = wsx_init_exec.x;\n";
            shader_main += "    uint exec_hi = wsx_init_exec.y;\n";
        }
        if (need_m0)   shader_main += "    uint m0 = 0u;\n";
        if (need_sink) shader_main += "    uint wsx_sink = 0u;\n";

        shader_main += cfg_code;

        if (ps_has_outputs) {
            shader_main += "    return wsx_ps_out;\n";
        } else if (stage_ == 1u) {
            shader_main += "    return wsx_out;\n";
        } else if (stage_ == 2u) {
            shader_main += "    wsx_stream.Append(wsx_out);\n";
            shader_main += "    wsx_stream.Append(wsx_out);\n";
            shader_main += "    wsx_stream.Append(wsx_out);\n";
            shader_main += "    wsx_stream.RestartStrip();\n";
        } else if (stage_ == 12u) {
            if (uses_lds_) {
                shader_main += "    GroupMemoryBarrierWithGroupSync();\n";
                shader_main += "    if (wsx_gi < 16u) { wsx_as_payload.data[wsx_gi] = wsx_lds[wsx_gi]; }\n";
            } else {
                shader_main += "    if (wsx_gi == 0u) {\n";
                shader_main += "        [unroll] for (uint _pi = 0u; _pi < 16u; ++_pi) { wsx_as_payload.data[_pi] = 0u; }\n";
                for (uint32_t k = 0u; k < 4u; ++k) {
                    if (sgprs_used_.count(k)) {
                        shader_main += "        wsx_as_payload.data[" + std::to_string(k) + "] = s" + std::to_string(k) + ";\n";
                    }
                    if (vgprs_used_.count(k)) {
                        shader_main += "        wsx_as_payload.data[" + std::to_string(k + 4u) + "] = v" + std::to_string(k) + ";\n";
                    }
                }
                shader_main += "    }\n";
            }
            shader_main += "    GroupMemoryBarrierWithGroupSync();\n";
            if (as_saw_dispatch_req_) {
                shader_main += "    DispatchMesh(_wsx_as_dims[0], _wsx_as_dims[1], _wsx_as_dims[2], wsx_as_payload);\n";
            } else {
                shader_main += "    DispatchMesh(1u, 1u, 1u, wsx_as_payload);\n";
            }
        } else if (stage_ == 13u) {
            if (ms_saw_alloc_req_) {
                shader_main += "    GroupMemoryBarrierWithGroupSync();\n";
                shader_main += "    uint _wsx_vc = min(_wsx_ms_counts[0], " + std::to_string(ms_max_verts) + "u);\n";
                shader_main += "    uint _wsx_pc = min(_wsx_ms_counts[1], " + std::to_string(ms_max_prims) + "u);\n";
                shader_main += "    SetMeshOutputCounts(_wsx_vc, _wsx_pc);\n";
                shader_main += "    if (wsx_gi < _wsx_vc) { wsx_verts[wsx_gi] = wsx_out; }\n";
                shader_main += "    if (wsx_gi < _wsx_pc) {\n";
                shader_main += "        uint _wsx_vm = (_wsx_vc > 0u) ? (_wsx_vc - 1u) : 0u;\n";
                shader_main += "        wsx_tris[wsx_gi] = min(wsx_prim, uint3(_wsx_vm, _wsx_vm, _wsx_vm));\n";
                shader_main += "    }\n";
            } else {
                shader_main += "    if (wsx_gi < " + std::to_string(ms_max_verts) + "u) { wsx_verts[wsx_gi] = wsx_out; }\n";
                shader_main += "    if (wsx_gi < " + std::to_string(ms_max_prims) + "u) { wsx_tris[wsx_gi] = min(wsx_prim, uint3(" +
                               std::to_string(ms_max_verts - 1u) + "u, " + std::to_string(ms_max_verts - 1u) + "u, " +
                               std::to_string(ms_max_verts - 1u) + "u)); }\n";
            }
        }
        shader_main += "}\n";

        std::string hlsl = wsx_sm66::build_trimmed_sm66_preamble(shader_main);
        hlsl += shader_main;

        stats.TranslatedInstructions = translated_count_;
        stats.UnsupportedInstructions = unsupported_count_;
        stats.SgprCount = static_cast<uint32_t>(sgprs_used_.size());
        stats.VgprCount = static_cast<uint32_t>(vgprs_used_.size());
        stats.UsesLds = uses_lds_ ? 1u : 0u;
        stats.UsesSm66Slabs = uses_slabs_ ? 1u : 0u;
        stats.UsesBindlessTextures = uses_textures_ ? 1u : 0u;
        return hlsl;
    }
};

inline const wchar_t* target_profile_for_stage(uint32_t stage) noexcept {
    switch (stage) {
    case 0u:      return L"ps_6_6";
    case 1u:      return L"vs_6_6";
    case 2u:      return L"gs_6_6";
    case 5u:      return L"cs_6_6";
    case 12u:     return L"as_6_6";
    case 13u:     return L"ms_6_6";
    case 0xFFFFu: return L"cs_6_6";
    default:      return nullptr;
    }
}

inline HRESULT translate_fssm_to_hlsl_sm66(
    const void* fssm,
    size_t fssm_size,
    uint32_t stage,
    char* out_hlsl,
    size_t out_cap,
    size_t* out_size,
    TranslateStats* out_stats) noexcept {
    if (!fssm || fssm_size < 0xC8u || !out_size) return E_INVALIDARG;
    *out_size = 0;

    TranslateStats st{};
    HlslEmitter emitter;
    const std::string hlsl = emitter.translate(static_cast<const uint8_t*>(fssm), fssm_size, stage, st);
    if (out_stats) *out_stats = st;
    if (hlsl.empty()) return E_FAIL;

    *out_size = hlsl.size();
    if (!out_hlsl) return S_OK;
    if (out_cap < hlsl.size() + 1u) return E_NOT_SUFFICIENT_BUFFER;
    std::memcpy(out_hlsl, hlsl.data(), hlsl.size());
    out_hlsl[hlsl.size()] = '\0';
    return S_OK;
}

inline HRESULT translate_dxbc_fssm_to_dxil(
    const void* dxbc,
    size_t dxbc_size,
    std::vector<uint8_t>& out_dxil,
    char* out_errors,
    size_t err_cap,
    TranslateStats* out_stats) noexcept {
    out_dxil.clear();
    if (!dxbc || dxbc_size < 32u) return E_INVALIDARG;

    wsx_shader::ShaderInfo si{};
    std::vector<uint8_t> fssm;
    if (FAILED(wsx_shader::inspect_shader_bytecode(dxbc, dxbc_size, &si, &fssm))) {
        return E_NOTIMPL;
    }

    const bool is_accc = (static_cast<wsx_shader::ShaderKind>(si.kind) == wsx_shader::ShaderKind::XboxAcccRdna2);
    wsx_shader::AcccCodeSlice accc_slice{};
    if (is_accc) {
        if (!wsx_shader::extract_accc_fspx_code(dxbc, dxbc_size, accc_slice) || !accc_slice.code_ptr) {
            return E_NOTIMPL;
        }
        si.stage = accc_slice.stage;
    } else if (fssm.empty()) {
        return E_NOTIMPL;
    }

    const wchar_t* profile = target_profile_for_stage(si.stage);
    if (!profile) return E_NOTIMPL;

    auto run_emit = [&](TranslateStats& st) -> std::string {
        HlslEmitter emitter;
        if (is_accc) {
            const size_t nwords = accc_slice.code_size / 4u;
            std::vector<uint32_t> words(nwords);
            std::memcpy(words.data(), accc_slice.code_ptr, nwords * 4u);
            return emitter.translate_raw_rdna2(
                words.data(), words.size(), accc_slice.stage,
                accc_slice.numthreads_x, accc_slice.numthreads_y, accc_slice.numthreads_z,
                0u, false, st);
        }
        return emitter.translate(fssm.data(), fssm.size(), si.stage, st);
    };

    // Fast path: persistent disk cache hit when WSX_SHADER_CACHE_DIR / [Graphics] shader_cache_dir is configured
    if (wsx_shader::load_cached_dxil_by_hash(si, out_dxil)) {
        if (out_stats) {
            TranslateStats st{};
            (void)run_emit(st);
            *out_stats = st;
        }
        return S_OK;
    }

    TranslateStats st{};
    const std::string hlsl = run_emit(st);
    if (out_stats) *out_stats = st;
    if (hlsl.empty()) return E_FAIL;

    const HRESULT hr = wsx_sm66::compile_hlsl_to_dxil_vec(
        hlsl.c_str(), hlsl.size(), L"main", profile,
        out_dxil, out_errors, err_cap);
    if (FAILED(hr) || out_dxil.size() < 64u) {
        out_dxil.clear();
        return FAILED(hr) ? hr : E_FAIL;
    }
    (void)wsx_shader::save_cached_dxil_by_hash(si, out_dxil);
    return S_OK;
}

inline HRESULT builtin_fssm_to_dxil(
    const wsx_shader::ShaderInfo& si,
    const void* payload_data,
    size_t payload_size,
    std::vector<uint8_t>& out_dxil) noexcept {
    out_dxil.clear();
    if (!payload_data || payload_size < 32u) return E_INVALIDARG;

    const bool is_accc = (static_cast<wsx_shader::ShaderKind>(si.kind) == wsx_shader::ShaderKind::XboxAcccRdna2);
    uint32_t stage = si.stage;
    TranslateStats st{};
    HlslEmitter emitter;
    std::string hlsl;

    if (is_accc) {
        wsx_shader::AcccCodeSlice slice{};
        if (!wsx_shader::extract_accc_fspx_code(payload_data, payload_size, slice) || !slice.code_ptr) {
            return E_NOTIMPL;
        }
        stage = slice.stage;
        const size_t nwords = slice.code_size / 4u;
        std::vector<uint32_t> words(nwords);
        std::memcpy(words.data(), slice.code_ptr, nwords * 4u);
        hlsl = emitter.translate_raw_rdna2(
            words.data(), words.size(), slice.stage,
            slice.numthreads_x, slice.numthreads_y, slice.numthreads_z,
            0u, false, st);
    } else {
        if (payload_size < 0xC8u) return E_INVALIDARG;
        hlsl = emitter.translate(static_cast<const uint8_t*>(payload_data), payload_size, stage, st);
    }

    const wchar_t* profile = target_profile_for_stage(stage);
    if (!profile || hlsl.empty()) return E_FAIL;

    char err[512]{};
    const HRESULT hr = wsx_sm66::compile_hlsl_to_dxil_vec(
        hlsl.c_str(), hlsl.size(), L"main", profile,
        out_dxil, err, sizeof(err));
    if (FAILED(hr) || out_dxil.size() < 64u) {
        out_dxil.clear();
        wsx::trace_hr("d3d12_xs", "Rdna2AutoTranslate(DXC)", hr, "stage=%u cfg=%s err=%.200s",
                      stage, cfg_mode_name(emitter.cfg_mode()), err);
        return FAILED(hr) ? hr : E_FAIL;
    }
    wsx::trace_hr("d3d12_xs", "Rdna2AutoTranslate(SM6.6)", S_OK,
                  "kind=%s stage=%u cfg=%s insns=%u/%u bbs=%u execElide=%u dxil=%zuB",
                  is_accc ? "ACCC" : "FSSM", stage, cfg_mode_name(emitter.cfg_mode()),
                  st.TranslatedInstructions, st.TotalInstructions, st.BasicBlockCount,
                  emitter.exec_elided() ? 1u : 0u, out_dxil.size());
    return S_OK;
}

} // namespace wsx_rdna2
