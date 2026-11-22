// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Mass RDNA2 (FSSM) -> HLSL SM 6.6 -> Signed DXIL Audit Tool across all shaders in shaders.rpf.
// Scans every _Wrapped raw-deflate DXBC entry (0x000DDCE7 marker), unpacks XBCH -> FSSM,
// measures exact instruction translation coverage across all Compute Shaders & all stages,
// prints ranked top unimplemented (Enc, op) opcodes, and optionally verifies live DXC + RTX PSO
// compilation across N sampled real Compute Shaders under D3D12 Debug Layer.

#include "d3d12_xs_rdna2_to_hlsl.h"
#include "include/xbox/d3d12_xs.h"
#include "wsx_inflate.h"

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

const char* stage_name(uint32_t s) noexcept {
    switch (s) {
    case 0u: return "PS";
    case 1u: return "VS";
    case 2u: return "GS";
    case 3u: return "HS";
    case 4u: return "DS";
    case 5u: return "CS";
    case 12u: return "AS";
    case 13u: return "MS";
    default: return "OTHER";
    }
}

struct UnimplStat {
    uint64_t count = 0;
    uint32_t sample_w0 = 0;
    uint32_t sample_w1 = 0;
    uint64_t sample_off = 0;
    std::string sample_name;
};

struct ShaderCandidate {
    uint32_t stage = 0;
    uint64_t file_off = 0;
    uint32_t comp_size = 0;
    uint32_t uncomp_size = 0;
    uint32_t insn_count = 0;
    uint32_t unimpl_count = 0;
    std::string name;
};

struct StageCoverage {
    uint64_t shaders = 0;
    uint64_t insns = 0;
    uint64_t xlat = 0;
    uint64_t unimpl = 0;
    uint64_t full_100 = 0;
    uint64_t wave64 = 0;
    uint64_t wave32 = 0;
};

struct CacheJobEntry {
    uint32_t stage = 0;
    uint64_t file_off = 0;
    uint32_t comp_size = 0;
    uint32_t uncomp_size = 0;
    std::string hash_hex;
};

std::string extract_shader_name(const uint8_t* base, size_t marker_pos) {
    if (marker_pos < 8) return "?";
    size_t end = marker_pos - 4; // before comp_size
    while (end > 0 && (base[end - 1] == 0 || base[end - 1] < 0x20 || base[end - 1] > 0x7E)) {
        --end;
    }
    size_t start = end;
    while (start > 0 && (end - start) < 96 && base[start - 1] >= 0x20 && base[start - 1] <= 0x7E) {
        --start;
    }
    if (end > start) {
        return std::string(reinterpret_cast<const char*>(base + start), end - start);
    }
    return "?";
}

void print_top_unimpl(const char* title,
                      const std::unordered_map<uint32_t, UnimplStat>& hist,
                      uint64_t total_unimpl,
                      size_t top_n) {
    struct Row {
        uint32_t key;
        UnimplStat st;
    };
    std::vector<Row> rows;
    rows.reserve(hist.size());
    for (const auto& kv : hist) rows.push_back({kv.first, kv.second});
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.st.count > b.st.count;
    });

    std::printf("\n=== %s (unique=%zu, total_unimpl=%llu) ===\n",
                title, rows.size(), static_cast<unsigned long long>(total_unimpl));
    const size_t limit = std::min(top_n, rows.size());
    double cum = 0.0;
    for (size_t i = 0; i < limit; ++i) {
        const auto enc = static_cast<wsx_rdna2::Enc>(rows[i].key >> 16);
        const uint32_t op = rows[i].key & 0xFFFFu;
        const double pct = total_unimpl ? (100.0 * static_cast<double>(rows[i].st.count) / static_cast<double>(total_unimpl)) : 0.0;
        cum += pct;
        std::printf("  %2zu. %-7s op=0x%03X : %8llu (%5.2f%%, cum %6.2f%%) | w0=0x%08X w1=0x%08X @0x%llX (%s)\n",
                    i + 1, wsx_rdna2::enc_name(enc), op,
                    static_cast<unsigned long long>(rows[i].st.count), pct, cum,
                    rows[i].st.sample_w0, rows[i].st.sample_w1,
                    static_cast<unsigned long long>(rows[i].st.sample_off),
                    rows[i].st.sample_name.c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    const std::filesystem::path rpf_path = (argc >= 2) ? argv[1] : "shaders.rpf";
    size_t dxc_test_count = 0;
    size_t dxc_test_stages = 0;
    uint32_t dxc_max_insns = 2000u;
    size_t max_shaders = 0; // 0 = all
    bool cs_only_scan = false;
    std::filesystem::path export_index_path;
    std::filesystem::path build_cache_dir;
    uint32_t cache_jobs = std::max(1u, std::thread::hardware_concurrency() / 2u);

    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dxc-test") == 0 && i + 1 < argc) {
            dxc_test_count = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--dxc-test-stages") == 0 && i + 1 < argc) {
            dxc_test_stages = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--dxc-max-insns") == 0 && i + 1 < argc) {
            dxc_max_insns = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--max") == 0 && i + 1 < argc) {
            max_shaders = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--cs-only") == 0) {
            cs_only_scan = true;
        } else if (std::strcmp(argv[i], "--export-index") == 0 && i + 1 < argc) {
            export_index_path = argv[++i];
        } else if (std::strcmp(argv[i], "--build-cache") == 0 && i + 1 < argc) {
            build_cache_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--jobs") == 0 && i + 1 < argc) {
            cache_jobs = std::max(1u, static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10)));
        }
    }

    const auto t0 = std::chrono::steady_clock::now();
    std::ifstream f(rpf_path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::fprintf(stderr, "Failed to open %s\n", rpf_path.string().c_str());
        return 1;
    }
    const size_t file_size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(file_size);
    if (!f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(file_size))) {
        std::fprintf(stderr, "Failed to read %s\n", rpf_path.string().c_str());
        return 1;
    }
    f.close();

    // Find all _Wrapped raw-deflate DXBC entries via 0x000DDCE7 header marker
    struct BlobEntry {
        size_t marker_pos;
        size_t data_off;
        uint32_t comp_size;
        uint32_t uncomp_size;
    };
    std::vector<BlobEntry> entries;
    entries.reserve(30000);
    for (size_t i = 4; i + 12 <= file_size; ++i) {
        if (data[i] == 0xE7u && data[i + 1] == 0xDCu && data[i + 2] == 0x0Du && data[i + 3] == 0x00u) {
            uint32_t c1 = 0, c2 = 0, u = 0;
            std::memcpy(&c1, data.data() + i - 4, 4);
            std::memcpy(&c2, data.data() + i + 4, 4);
            std::memcpy(&u, data.data() + i + 8, 4);
            if (c1 == c2 && c1 >= 64u && c1 <= 2000000u && c1 <= u + 1024u && i + 12u + c1 <= file_size) {
                entries.push_back({i, i + 12u, c1, u});
                i += 11u;
            }
        }
    }

    std::printf("Loaded %s (%.2f MB) -> found %zu wrapped shader entries\n",
                rpf_path.string().c_str(), static_cast<double>(file_size) / (1024.0 * 1024.0), entries.size());

    std::map<uint32_t, StageCoverage> stage_cov;
    uint64_t fssm_total = 0, fssm_cs = 0;
    uint64_t cfg_counts[4]{};
    uint64_t exec_elided_total = 0, coalesced_guards_total = 0, dead_scc_total = 0, peephole_total = 0, total_hlsl_bytes = 0;
    uint64_t all_insns = 0, all_xlat = 0, all_unimpl = 0, all_unknown_enc = 0;
    uint64_t cs_insns = 0, cs_xlat = 0, cs_unimpl = 0, cs_unknown_enc = 0;
    uint64_t cs_100 = 0, cs_99 = 0, cs_95 = 0, cs_lt95 = 0;
    uint64_t cs_sdwa_ops = 0, cs_dpp_ops = 0;

    std::unordered_map<uint32_t, UnimplStat> cs_hist;
    std::unordered_map<uint32_t, UnimplStat> all_hist;
    std::vector<ShaderCandidate> cs_list;
    std::map<uint32_t, std::vector<ShaderCandidate>> stage_lists;
    std::vector<CacheJobEntry> cache_jobs_list;
    cs_list.reserve(8192);

    std::ofstream idx_out;
    bool idx_first = true;
    if (!export_index_path.empty()) {
        std::error_code iec;
        if (export_index_path.has_parent_path()) std::filesystem::create_directories(export_index_path.parent_path(), iec);
        idx_out.open(export_index_path, std::ios::binary | std::ios::trunc);
        if (idx_out) {
            idx_out << "{\n  \"format\": \"mizubox-rdna2-sm66-index-v1\",\n  \"shaders\": [\n";
        }
    }

    std::vector<uint8_t> dxbc;
    std::vector<uint8_t> fssm;
    std::vector<wsx_rdna2::DecodedInsn> unimpl_vec;

    const size_t scan_limit = (max_shaders > 0 && max_shaders < entries.size()) ? max_shaders : entries.size();
    for (size_t idx = 0; idx < scan_limit; ++idx) {
        const auto& e = entries[idx];
        dxbc.clear();
        if (!wsx_inflate::inflate_raw(data.data() + e.data_off, e.comp_size, dxbc, e.uncomp_size + 4096u)) {
            continue;
        }
        wsx_shader::ShaderInfo si{};
        fssm.clear();
        if (FAILED(wsx_shader::inspect_shader_bytecode(dxbc.data(), dxbc.size(), &si, &fssm)) || fssm.empty()) {
            continue;
        }
        ++fssm_total;
        auto& sc = stage_cov[si.stage];
        ++sc.shaders;

        const bool is_cs = (si.stage == 5u);
        if (cs_only_scan && !is_cs) continue;

        wsx_rdna2::TranslateStats st{};
        wsx_rdna2::HlslEmitter emitter;
        unimpl_vec.clear();
        const std::string hlsl_out = emitter.translate(fssm.data(), fssm.size(), si.stage, st, &unimpl_vec);
        const uint32_t cm = static_cast<uint32_t>(emitter.cfg_mode());
        if (cm < 4u) ++cfg_counts[cm];
        if (emitter.exec_elided()) ++exec_elided_total;
        coalesced_guards_total += emitter.coalesced_guards();
        dead_scc_total += emitter.dead_scc_elided();
        peephole_total += emitter.peephole_folds();
        total_hlsl_bytes += hlsl_out.size();

        all_insns += st.TotalInstructions;
        all_xlat += st.TranslatedInstructions;
        all_unimpl += st.UnsupportedInstructions;
        sc.insns += st.TotalInstructions;
        sc.xlat += st.TranslatedInstructions;
        sc.unimpl += st.UnsupportedInstructions;
        if (st.UnsupportedInstructions == 0u) ++sc.full_100;
        if (emitter.is_wave64()) ++sc.wave64;
        else ++sc.wave32;

        if (idx_out) {
            const std::string hhex = wsx_shader::preferred_cache_key_hex(si);
            if (!idx_first) idx_out << ",\n";
            idx_first = false;
            auto write_set = [&](const std::set<uint32_t>& s) {
                idx_out << "[";
                bool first_el = true;
                for (uint32_t v : s) {
                    if (!first_el) idx_out << ",";
                    first_el = false;
                    idx_out << v;
                }
                idx_out << "]";
            };
            idx_out << "    {\"hash\":\"" << hhex << "\",\"stage\":\"" << stage_name(si.stage)
                    << "\",\"stage_id\":" << si.stage
                    << ",\"wave64\":" << (emitter.is_wave64() ? "true" : "false")
                    << ",\"offset\":" << e.data_off
                    << ",\"insns\":" << st.TotalInstructions
                    << ",\"bbs\":" << st.BasicBlockCount
                    << ",\"sgprs\":" << st.SgprCount
                    << ",\"vgprs\":" << st.VgprCount
                    << ",\"nt\":[" << st.NumThreadsX << "," << st.NumThreadsY << "," << st.NumThreadsZ << "]"
                    << ",\"lds\":" << (st.UsesLds ? "true" : "false")
                    << ",\"slabs\":" << (st.UsesSm66Slabs ? "true" : "false")
                    << ",\"tex\":" << (st.UsesBindlessTextures ? "true" : "false")
                    << ",\"ps_attrs\":"; write_set(emitter.ps_attrs_used());
            idx_out << ",\"ps_mrts\":"; write_set(emitter.ps_mrts_exported());
            idx_out << ",\"ps_depth\":" << (emitter.ps_depth_exported() ? "true" : "false")
                    << ",\"vs_params\":"; write_set(emitter.vs_params_exported());
            idx_out << "}";
        }

        if (!build_cache_dir.empty()) {
            cache_jobs_list.push_back({si.stage, e.data_off, e.comp_size, e.uncomp_size,
                                       wsx_shader::preferred_cache_key_hex(si)});
        }

        if (dxc_test_stages > 0) {
            const std::string name = extract_shader_name(data.data(), e.marker_pos);
            stage_lists[si.stage].push_back({si.stage, e.data_off, e.comp_size, e.uncomp_size, st.TotalInstructions, st.UnsupportedInstructions, name});
        }

        // Also scan SDWA/DPP counts on CS
        if (is_cs) {
            ++fssm_cs;
            cs_insns += st.TotalInstructions;
            cs_xlat += st.TranslatedInstructions;
            cs_unimpl += st.UnsupportedInstructions;

            const double cov = st.TotalInstructions
                ? (100.0 * static_cast<double>(st.TranslatedInstructions) / static_cast<double>(st.TotalInstructions))
                : 100.0;
            if (st.UnsupportedInstructions == 0u) ++cs_100;
            if (cov >= 99.0) ++cs_99;
            if (cov >= 95.0) ++cs_95;
            else ++cs_lt95;

            const std::string name = extract_shader_name(data.data(), e.marker_pos);
            cs_list.push_back({si.stage, e.data_off, e.comp_size, e.uncomp_size, st.TotalInstructions, st.UnsupportedInstructions, name});

            // Count SDWA / DPP operand encodings in CS
            if (fssm.size() >= st.CodeOffset + 4u && st.CodeSize >= 4u) {
                const size_t nw = st.CodeSize / 4u;
                std::vector<uint32_t> words(nw);
                std::memcpy(words.data(), fssm.data() + st.CodeOffset, nw * 4u);
                const auto dec = wsx_rdna2::decode_rdna2_stream(words.data(), words.size());
                for (const auto& ins : dec) {
                    if (ins.enc == wsx_rdna2::Enc::VOP1 || ins.enc == wsx_rdna2::Enc::VOP2 || ins.enc == wsx_rdna2::Enc::VOPC) {
                        const uint32_t s0 = ins.w0 & 0x1FFu;
                        if (s0 == 0xF9u) ++cs_sdwa_ops;
                        else if (s0 == 0xFAu || s0 == 0xE9u || s0 == 0xEAu) ++cs_dpp_ops;
                    }
                }
            }

            for (const auto& u : unimpl_vec) {
                if (u.enc == wsx_rdna2::Enc::Unknown) ++cs_unknown_enc;
                const uint32_t key = (static_cast<uint32_t>(u.enc) << 16) | (u.op & 0xFFFFu);
                auto& s = cs_hist[key];
                if (s.count == 0) {
                    s.sample_w0 = u.w0;
                    s.sample_w1 = u.w1;
                    s.sample_off = e.data_off;
                    s.sample_name = name;
                }
                ++s.count;
            }
        }

        for (const auto& u : unimpl_vec) {
            if (u.enc == wsx_rdna2::Enc::Unknown) ++all_unknown_enc;
            const uint32_t key = (static_cast<uint32_t>(u.enc) << 16) | (u.op & 0xFFFFu);
            auto& s = all_hist[key];
            if (s.count == 0) {
                s.sample_w0 = u.w0;
                s.sample_w1 = u.w1;
                s.sample_off = e.data_off;
                s.sample_name = extract_shader_name(data.data(), e.marker_pos);
            }
            ++s.count;
        }
    }

    if (idx_out) {
        idx_out << "\n  ]\n}\n";
        idx_out.close();
        std::printf("\nExported shader metadata index -> %s (%llu entries)\n",
                    export_index_path.string().c_str(), static_cast<unsigned long long>(fssm_total));
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();

    uint64_t total_w64 = 0, total_w32 = 0;
    std::printf("\n=== SHADER ARCHIVE BREAKDOWN (%.2f s) ===\n", sec);
    std::printf("  Total unpacked FSSM shaders: %llu\n", static_cast<unsigned long long>(fssm_total));
    for (const auto& kv : stage_cov) {
        const auto& sc = kv.second;
        total_w64 += sc.wave64;
        total_w32 += sc.wave32;
        const double spct = sc.insns ? (100.0 * static_cast<double>(sc.xlat) / static_cast<double>(sc.insns)) : 0.0;
        std::printf("    Stage %2u (%-5s): %6llu shaders | %10llu / %10llu insns = %7.3f%% (100%%=%llu, unimpl=%llu, W64=%llu, W32=%llu)\n",
                    kv.first, stage_name(kv.first),
                    static_cast<unsigned long long>(sc.shaders),
                    static_cast<unsigned long long>(sc.xlat),
                    static_cast<unsigned long long>(sc.insns),
                    spct,
                    static_cast<unsigned long long>(sc.full_100),
                    static_cast<unsigned long long>(sc.unimpl),
                    static_cast<unsigned long long>(sc.wave64),
                    static_cast<unsigned long long>(sc.wave32));
    }
    std::printf("  Wave width summary         : Wave64=%llu (%.1f%%) | Wave32=%llu (%.1f%%)\n",
                static_cast<unsigned long long>(total_w64), fssm_total ? (100.0 * total_w64 / fssm_total) : 0.0,
                static_cast<unsigned long long>(total_w32), fssm_total ? (100.0 * total_w32 / fssm_total) : 0.0);
    const uint64_t structured_total = cfg_counts[0] + cfg_counts[1] + cfg_counts[2];
    std::printf("  CFG Optimizer summary      : StraightLine=%llu | Relooper=%llu | TopoDag=%llu | StateMachine=%llu (loop-free/structured=%.1f%%)\n",
                static_cast<unsigned long long>(cfg_counts[0]),
                static_cast<unsigned long long>(cfg_counts[1]),
                static_cast<unsigned long long>(cfg_counts[2]),
                static_cast<unsigned long long>(cfg_counts[3]),
                fssm_total ? (100.0 * structured_total / fssm_total) : 0.0);
    std::printf("  Exec/Guard & HLSL summary  : execElided=%llu (%.1f%%) | guardsCoalesced=%llu | deadScc=%llu | peephole=%llu | avgHlsl=%.1f KB\n",
                static_cast<unsigned long long>(exec_elided_total),
                fssm_total ? (100.0 * exec_elided_total / fssm_total) : 0.0,
                static_cast<unsigned long long>(coalesced_guards_total),
                static_cast<unsigned long long>(dead_scc_total),
                static_cast<unsigned long long>(peephole_total),
                fssm_total ? (static_cast<double>(total_hlsl_bytes) / (1024.0 * fssm_total)) : 0.0);

    const double cs_pct = cs_insns ? (100.0 * static_cast<double>(cs_xlat) / static_cast<double>(cs_insns)) : 0.0;
    const double all_pct = all_insns ? (100.0 * static_cast<double>(all_xlat) / static_cast<double>(all_insns)) : 0.0;
    std::printf("\n=== TRANSLATION COVERAGE ===\n");
    std::printf("  COMPUTE SHADERS (%llu shaders): %llu / %llu insns = %.3f%% (unimpl=%llu, unk_enc=%llu)\n",
                static_cast<unsigned long long>(fssm_cs),
                static_cast<unsigned long long>(cs_xlat),
                static_cast<unsigned long long>(cs_insns),
                cs_pct,
                static_cast<unsigned long long>(cs_unimpl),
                static_cast<unsigned long long>(cs_unknown_enc));
    std::printf("    CS coverage buckets: 100%%=%llu (%.1f%%) | >=99%%=%llu (%.1f%%) | >=95%%=%llu (%.1f%%) | <95%%=%llu (%.1f%%)\n",
                static_cast<unsigned long long>(cs_100), fssm_cs ? 100.0 * cs_100 / fssm_cs : 0.0,
                static_cast<unsigned long long>(cs_99),  fssm_cs ? 100.0 * cs_99  / fssm_cs : 0.0,
                static_cast<unsigned long long>(cs_95),  fssm_cs ? 100.0 * cs_95  / fssm_cs : 0.0,
                static_cast<unsigned long long>(cs_lt95), fssm_cs ? 100.0 * cs_lt95 / fssm_cs : 0.0);
    std::printf("    CS VOP modifiers   : SDWA(0xF9)=%llu, DPP(0xFA/0xE9/0xEA)=%llu\n",
                static_cast<unsigned long long>(cs_sdwa_ops),
                static_cast<unsigned long long>(cs_dpp_ops));
    if (!cs_only_scan) {
        std::printf("  ALL STAGES      (%llu shaders): %llu / %llu insns = %.3f%% (unimpl=%llu, unk_enc=%llu)\n",
                    static_cast<unsigned long long>(fssm_total),
                    static_cast<unsigned long long>(all_xlat),
                    static_cast<unsigned long long>(all_insns),
                    all_pct,
                    static_cast<unsigned long long>(all_unimpl),
                    static_cast<unsigned long long>(all_unknown_enc));
    }

    print_top_unimpl("TOP 50 UNIMPLEMENTED OPCODES (COMPUTE SHADERS)", cs_hist, cs_unimpl, 50);
    if (!cs_only_scan) {
        print_top_unimpl("TOP 30 UNIMPLEMENTED OPCODES (ALL STAGES)", all_hist, all_unimpl, 30);
    }

    // Optional live DXC + RTX PSO stress test across N diverse real Compute Shaders!
    if (dxc_test_count > 0 && !cs_list.empty()) {
        std::vector<ShaderCandidate> eligible;
        eligible.reserve(cs_list.size());
        for (const auto& c : cs_list) {
            if (dxc_max_insns == 0u || c.insn_count <= dxc_max_insns) eligible.push_back(c);
        }
        std::sort(eligible.begin(), eligible.end(), [](const ShaderCandidate& a, const ShaderCandidate& b) {
            return a.insn_count < b.insn_count;
        });
        const size_t n = std::min(dxc_test_count, eligible.size());
        std::printf("\n=== LIVE DXC + RTX PSO STRESS TEST (%zu / %zu eligible CS <= %u insns) ===\n",
                    n, eligible.size(), dxc_max_insns);

        {
            ComPtr<ID3D12Debug> dbg;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(dbg.GetAddressOf())))) {
                dbg->EnableDebugLayer();
            }
        }
        D3D12XBOX_CREATE_DEVICE_PARAMETERS dp{};
        dp.Version = 1;
        dp.ProcessDebugFlags = D3D12_PROCESS_DEBUG_FLAG_DEBUG_LAYER_ENABLED;
        ComPtr<ID3D12Device> dev;
        if (FAILED(D3D12XboxCreateDevice(nullptr, &dp, IID_PPV_ARGS(dev.GetAddressOf())))) {
            std::fprintf(stderr, "D3D12XboxCreateDevice failed\n");
            return 1;
        }
        ComPtr<ID3D12InfoQueue> iq;
        dev.As(&iq);
        if (iq) iq->ClearStoredMessages();

        ComPtr<ID3D12RootSignature> sm66_rs;
        if (FAILED(D3D12XboxCreateSm66RootSignature(dev.Get(), IID_PPV_ARGS(sm66_rs.GetAddressOf())))) {
            std::fprintf(stderr, "D3D12XboxCreateSm66RootSignature failed\n");
            return 1;
        }

        size_t dxc_ok = 0, pso_ok = 0, dxc_fail = 0, pso_fail = 0;
        const auto dt0 = std::chrono::steady_clock::now();

        for (size_t k = 0; k < n; ++k) {
            const size_t ci = (n == 1) ? 0 : (k * (eligible.size() - 1)) / (n - 1);
            const auto& c = eligible[ci];
            dxbc.clear();
            if (!wsx_inflate::inflate_raw(data.data() + c.file_off, c.comp_size, dxbc, c.uncomp_size + 4096u)) {
                continue;
            }
            std::vector<uint8_t> dxil(2 * 1024 * 1024);
            size_t dxil_sz = 0;
            char err[1024]{};
            D3D12XBOX_RDNA2_TRANSLATE_STATS ts{};
            HRESULT hr = D3D12XboxTranslateDxbcFssmToDxil(
                dxbc.data(), dxbc.size(), dxil.data(), dxil.size(), &dxil_sz, err, sizeof(err), &ts);
            if (hr == E_NOT_SUFFICIENT_BUFFER && dxil_sz > dxil.size()) {
                dxil.resize(dxil_sz);
                hr = D3D12XboxTranslateDxbcFssmToDxil(
                    dxbc.data(), dxbc.size(), dxil.data(), dxil.size(), &dxil_sz, err, sizeof(err), &ts);
            }
            if (FAILED(hr) || dxil_sz < 64u) {
                ++dxc_fail;
                if (dxc_fail <= 10) {
                    std::printf("  [DXC FAIL] @0x%llX (%s, %u insns) hr=0x%08lX: %.240s\n",
                                static_cast<unsigned long long>(c.file_off), c.name.c_str(),
                                c.insn_count, static_cast<unsigned long>(hr), err);
                }
                continue;
            }
            ++dxc_ok;

            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = sm66_rs.Get();
            pd.CS = {dxil.data(), dxil_sz};
            ComPtr<ID3D12PipelineState> pso;
            const HRESULT phr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(pso.GetAddressOf()));
            if (SUCCEEDED(phr) && pso) {
                ++pso_ok;
            } else {
                ++pso_fail;
                if (pso_fail <= 10) {
                    std::printf("  [PSO FAIL] @0x%llX (%s, %u insns, %zu B DXIL) hr=0x%08lX\n",
                                static_cast<unsigned long long>(c.file_off), c.name.c_str(),
                                c.insn_count, dxil_sz, static_cast<unsigned long>(phr));
                }
            }
            if ((k + 1) % 20 == 0 || k + 1 == n) {
                std::printf("  [PROGRESS] %zu/%zu: DXC=%zu OK (fail=%zu), RTX PSO=%zu OK (last: %u insns, %zu B DXIL)\n",
                            k + 1, n, dxc_ok, dxc_fail, pso_ok, c.insn_count, dxil_sz);
            }
        }

        uint32_t dbg_errs = 0;
        UINT64 dbg_msgs = 0;
        if (iq) {
            dbg_msgs = iq->GetNumStoredMessages();
            for (UINT64 i = 0; i < dbg_msgs; ++i) {
                SIZE_T len = 0;
                iq->GetMessage(i, nullptr, &len);
                if (!len) continue;
                std::vector<uint8_t> mb(len);
                auto* m = reinterpret_cast<D3D12_MESSAGE*>(mb.data());
                if (SUCCEEDED(iq->GetMessage(i, m, &len)) &&
                    (m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
                     m->Severity == D3D12_MESSAGE_SEVERITY_ERROR)) {
                    ++dbg_errs;
                    if (dbg_errs <= 5) std::printf("  [D3D12 ERR] %s\n", m->pDescription);
                }
            }
        }
        const double dsec = std::chrono::duration<double>(std::chrono::steady_clock::now() - dt0).count();
        std::printf("  STRESS SUMMARY (%.2f s): DXC=%zu/%zu OK (fail=%zu) | RTX PSO=%zu/%zu OK (fail=%zu) | DebugLayer errs=%u (msgs=%llu)\n",
                    dsec, dxc_ok, n, dxc_fail, pso_ok, n, pso_fail, dbg_errs, static_cast<unsigned long long>(dbg_msgs));
        if (dxc_fail > 0 || pso_fail > 0 || dbg_errs > 0) return 2;
    }

    // Optional live multi-stage DXC + RTX PSO stress test across ALL stages (PS, VS, GS, MS, CS)!
    if (dxc_test_stages > 0 && !stage_lists.empty()) {
        std::printf("\n=== LIVE MULTI-STAGE DXC + RTX PSO STRESS TEST (%zu shaders per stage, <= %u insns) ===\n",
                    dxc_test_stages, dxc_max_insns);
        {
            ComPtr<ID3D12Debug> dbg;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(dbg.GetAddressOf())))) {
                dbg->EnableDebugLayer();
            }
        }
        D3D12XBOX_CREATE_DEVICE_PARAMETERS dp{};
        dp.Version = 1;
        dp.ProcessDebugFlags = D3D12_PROCESS_DEBUG_FLAG_DEBUG_LAYER_ENABLED;
        ComPtr<ID3D12Device> dev;
        if (FAILED(D3D12XboxCreateDevice(nullptr, &dp, IID_PPV_ARGS(dev.GetAddressOf())))) {
            std::fprintf(stderr, "D3D12XboxCreateDevice failed\n");
            return 1;
        }
        ComPtr<ID3D12Device2> dev2;
        dev.As(&dev2);
        ComPtr<ID3D12InfoQueue> iq;
        dev.As(&iq);
        if (iq) iq->ClearStoredMessages();

        ComPtr<ID3D12RootSignature> sm66_rs;
        if (FAILED(D3D12XboxCreateSm66RootSignature(dev.Get(), IID_PPV_ARGS(sm66_rs.GetAddressOf())))) {
            std::fprintf(stderr, "D3D12XboxCreateSm66RootSignature failed\n");
            return 1;
        }

        // Compile tiny SM 6.6 partner shaders for graphics/mesh PSO validation
        const char* kPassPs = "float4 main(float4 pos : SV_Position) : SV_Target0 { return float4(1,0,1,1); }";
        const char* kPassVsSimple = "float4 main(uint vid : SV_VertexID) : SV_Position { return float4(0,0,0,1); }";
        std::string pass_vs_all = "struct Out {\n float4 pos : SV_Position;\n";
        for (uint32_t i = 0; i < 31u; ++i) {
            pass_vs_all += " float4 p" + std::to_string(i) + " : TEXCOORD" + std::to_string(i) + ";\n";
        }
        pass_vs_all += "};\nOut main(uint vid : SV_VertexID) { Out o = (Out)0; o.pos = float4(0,0,0,1); return o; }\n";

        std::vector<uint8_t> pass_ps_dxil, pass_vs_simple_dxil, pass_vs_all_dxil;
        char perr[512]{};
        if (FAILED(wsx_sm66::compile_hlsl_to_dxil_vec(kPassPs, std::strlen(kPassPs), L"main", L"ps_6_6", pass_ps_dxil, perr, sizeof(perr))) ||
            FAILED(wsx_sm66::compile_hlsl_to_dxil_vec(kPassVsSimple, std::strlen(kPassVsSimple), L"main", L"vs_6_6", pass_vs_simple_dxil, perr, sizeof(perr))) ||
            FAILED(wsx_sm66::compile_hlsl_to_dxil_vec(pass_vs_all.c_str(), pass_vs_all.size(), L"main", L"vs_6_6", pass_vs_all_dxil, perr, sizeof(perr)))) {
            std::fprintf(stderr, "Partner SM 6.6 shader compile failed: %s\n", perr);
            return 1;
        }

        auto make_base_gpso = [&](D3D12_GRAPHICS_PIPELINE_STATE_DESC& g) {
            std::memset(&g, 0, sizeof(g));
            g.pRootSignature = sm66_rs.Get();
            g.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            g.SampleMask = UINT_MAX;
            g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            g.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            g.RasterizerState.DepthClipEnable = TRUE;
            g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            g.NumRenderTargets = 1;
            g.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            g.SampleDesc.Count = 1;
        };

#pragma warning(push)
#pragma warning(disable : 4324)
        struct MsPsoStream {
            struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; ID3D12RootSignature* v; } rs;
            struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_SHADER_BYTECODE v; } ms;
            struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_SHADER_BYTECODE v; } ps;
            struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_RT_FORMAT_ARRAY v; } rtv;
            struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; DXGI_SAMPLE_DESC v; } sd;
        };
#pragma warning(pop)

        size_t total_tested = 0, total_dxc_ok = 0, total_pso_ok = 0, total_dxc_fail = 0, total_pso_fail = 0;
        const auto mt0 = std::chrono::steady_clock::now();

        for (auto& kv : stage_lists) {
            const uint32_t stg = kv.first;
            std::vector<ShaderCandidate> eligible;
            for (const auto& c : kv.second) {
                if (dxc_max_insns == 0u || c.insn_count <= dxc_max_insns) eligible.push_back(c);
            }
            if (eligible.empty()) continue;
            std::sort(eligible.begin(), eligible.end(), [](const ShaderCandidate& a, const ShaderCandidate& b) {
                return a.insn_count < b.insn_count;
            });
            const size_t n = std::min(dxc_test_stages, eligible.size());
            size_t s_dxc_ok = 0, s_pso_ok = 0, s_dxc_fail = 0, s_pso_fail = 0;

            for (size_t k = 0; k < n; ++k) {
                const size_t ci = (n == 1) ? 0 : (k * (eligible.size() - 1)) / (n - 1);
                const auto& c = eligible[ci];
                ++total_tested;
                dxbc.clear();
                if (!wsx_inflate::inflate_raw(data.data() + c.file_off, c.comp_size, dxbc, c.uncomp_size + 4096u)) {
                    ++s_dxc_fail; ++total_dxc_fail;
                    continue;
                }
                std::vector<uint8_t> dxil(2 * 1024 * 1024);
                size_t dxil_sz = 0;
                char err[1024]{};
                D3D12XBOX_RDNA2_TRANSLATE_STATS ts{};
                HRESULT hr = D3D12XboxTranslateDxbcFssmToDxil(
                    dxbc.data(), dxbc.size(), dxil.data(), dxil.size(), &dxil_sz, err, sizeof(err), &ts);
                if (hr == E_NOT_SUFFICIENT_BUFFER && dxil_sz > dxil.size()) {
                    dxil.resize(dxil_sz);
                    hr = D3D12XboxTranslateDxbcFssmToDxil(
                        dxbc.data(), dxbc.size(), dxil.data(), dxil.size(), &dxil_sz, err, sizeof(err), &ts);
                }
                if (FAILED(hr) || dxil_sz < 64u) {
                    ++s_dxc_fail; ++total_dxc_fail;
                    if (s_dxc_fail <= 5) {
                        std::printf("  [%s DXC FAIL] @0x%llX (%s, %u insns) hr=0x%08lX: %.240s\n",
                                    stage_name(stg), static_cast<unsigned long long>(c.file_off), c.name.c_str(),
                                    c.insn_count, static_cast<unsigned long>(hr), err);
                    }
                    continue;
                }
                ++s_dxc_ok; ++total_dxc_ok;

                ComPtr<ID3D12PipelineState> pso;
                HRESULT phr = E_NOTIMPL;
                if (stg == 5u) {
                    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
                    pd.pRootSignature = sm66_rs.Get();
                    pd.CS = {dxil.data(), dxil_sz};
                    phr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(pso.GetAddressOf()));
                } else if (stg == 0u) {
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC g{};
                    make_base_gpso(g);
                    g.VS = {pass_vs_all_dxil.data(), pass_vs_all_dxil.size()};
                    g.PS = {dxil.data(), dxil_sz};
                    g.NumRenderTargets = 8;
                    for (int r = 0; r < 8; ++r) {
                        g.RTVFormats[r] = DXGI_FORMAT_R8G8B8A8_UNORM;
                        g.BlendState.RenderTarget[r].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                    }
                    g.DSVFormat = DXGI_FORMAT_D32_FLOAT;
                    g.DepthStencilState.DepthEnable = TRUE;
                    g.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
                    g.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
                    phr = dev->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(pso.GetAddressOf()));
                } else if (stg == 1u) {
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC g{};
                    make_base_gpso(g);
                    g.VS = {dxil.data(), dxil_sz};
                    g.PS = {pass_ps_dxil.data(), pass_ps_dxil.size()};
                    phr = dev->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(pso.GetAddressOf()));
                } else if (stg == 2u) {
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC g{};
                    make_base_gpso(g);
                    g.VS = {pass_vs_simple_dxil.data(), pass_vs_simple_dxil.size()};
                    g.GS = {dxil.data(), dxil_sz};
                    g.PS = {pass_ps_dxil.data(), pass_ps_dxil.size()};
                    phr = dev->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(pso.GetAddressOf()));
                } else if (stg == 13u && dev2) {
                    MsPsoStream ms{};
                    ms.rs = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, sm66_rs.Get()};
                    ms.ms = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, {dxil.data(), dxil_sz}};
                    ms.ps = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, {pass_ps_dxil.data(), pass_ps_dxil.size()}};
                    D3D12_RT_FORMAT_ARRAY rta{};
                    rta.NumRenderTargets = 1;
                    rta.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                    ms.rtv = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, rta};
                    ms.sd = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, {1, 0}};
                    D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof(ms), &ms};
                    phr = dev2->CreatePipelineState(&sd, IID_PPV_ARGS(pso.GetAddressOf()));
                }
                if (SUCCEEDED(phr) && pso) {
                    ++s_pso_ok; ++total_pso_ok;
                } else {
                    ++s_pso_fail; ++total_pso_fail;
                    if (s_pso_fail <= 5) {
                        std::printf("  [%s PSO FAIL] @0x%llX (%s, %u insns, %zu B DXIL) hr=0x%08lX\n",
                                    stage_name(stg), static_cast<unsigned long long>(c.file_off), c.name.c_str(),
                                    c.insn_count, dxil_sz, static_cast<unsigned long>(phr));
                    }
                }
            }
            std::printf("  STAGE %-2s: DXC=%zu/%zu OK (fail=%zu) | RTX PSO=%zu/%zu OK (fail=%zu)\n",
                        stage_name(stg), s_dxc_ok, n, s_dxc_fail, s_pso_ok, n, s_pso_fail);
        }

        uint32_t dbg_errs = 0;
        UINT64 dbg_msgs = 0;
        if (iq) {
            dbg_msgs = iq->GetNumStoredMessages();
            for (UINT64 i = 0; i < dbg_msgs; ++i) {
                SIZE_T len = 0;
                iq->GetMessage(i, nullptr, &len);
                if (!len) continue;
                std::vector<uint8_t> mb(len);
                auto* m = reinterpret_cast<D3D12_MESSAGE*>(mb.data());
                if (SUCCEEDED(iq->GetMessage(i, m, &len)) &&
                    (m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
                     m->Severity == D3D12_MESSAGE_SEVERITY_ERROR)) {
                    ++dbg_errs;
                    if (dbg_errs <= 10) std::printf("  [D3D12 ERR] %s\n", m->pDescription);
                }
            }
        }
        const double msec = std::chrono::duration<double>(std::chrono::steady_clock::now() - mt0).count();
        std::printf("  MULTI-STAGE SUMMARY (%.2f s): DXC=%zu/%zu OK (fail=%zu) | RTX PSO=%zu/%zu OK (fail=%zu) | DebugLayer errs=%u (msgs=%llu)\n",
                    msec, total_dxc_ok, total_tested, total_dxc_fail, total_pso_ok, total_tested, total_pso_fail,
                    dbg_errs, static_cast<unsigned long long>(dbg_msgs));
        if (total_dxc_fail > 0 || total_pso_fail > 0 || dbg_errs > 0) return 3;
    }

    // Optional multithreaded resumable offline SM 6.6 DXIL shader cache pre-builder (--build-cache <dir>)
    if (!build_cache_dir.empty() && !cache_jobs_list.empty()) {
        std::error_code bec;
        std::filesystem::create_directories(build_cache_dir, bec);
        SetEnvironmentVariableW(L"WSX_SHADER_CACHE_DIR", build_cache_dir.c_str());

        std::printf("\n=== OFFLINE SM 6.6 DXIL SHADER CACHE BUILDER (%zu shaders, %u threads -> %s) ===\n",
                    cache_jobs_list.size(), cache_jobs, build_cache_dir.string().c_str());
        const auto bt0 = std::chrono::steady_clock::now();
        std::atomic<size_t> next_idx{0};
        std::atomic<size_t> built_cnt{0};
        std::atomic<size_t> skipped_cnt{0};
        std::atomic<size_t> failed_cnt{0};
        std::mutex log_mtx;

        auto worker = [&]() {
            std::vector<uint8_t> w_dxbc;
            std::vector<uint8_t> w_dxil(2 * 1024 * 1024);
            while (true) {
                const size_t i = next_idx.fetch_add(1, std::memory_order_relaxed);
                if (i >= cache_jobs_list.size()) break;
                const auto& job = cache_jobs_list[i];
                const std::filesystem::path dst = build_cache_dir / (job.hash_hex + ".dxil");
                std::error_code fec;
                if (std::filesystem::is_regular_file(dst, fec) && std::filesystem::file_size(dst, fec) >= 64u) {
                    skipped_cnt.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                w_dxbc.clear();
                if (!wsx_inflate::inflate_raw(data.data() + job.file_off, job.comp_size, w_dxbc, job.uncomp_size + 4096u)) {
                    failed_cnt.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                size_t dxil_sz = 0;
                char err[512]{};
                HRESULT hr = D3D12XboxTranslateDxbcFssmToDxil(
                    w_dxbc.data(), w_dxbc.size(), w_dxil.data(), w_dxil.size(), &dxil_sz, err, sizeof(err), nullptr);
                if (hr == E_NOT_SUFFICIENT_BUFFER && dxil_sz > w_dxil.size()) {
                    w_dxil.resize(dxil_sz);
                    hr = D3D12XboxTranslateDxbcFssmToDxil(
                        w_dxbc.data(), w_dxbc.size(), w_dxil.data(), w_dxil.size(), &dxil_sz, err, sizeof(err), nullptr);
                }
                if (SUCCEEDED(hr) && dxil_sz >= 64u) {
                    const size_t done = built_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (done % 100 == 0) {
                        std::lock_guard<std::mutex> lk(log_mtx);
                        std::printf("  [CACHE] built=%zu skipped=%zu failed=%zu / %zu\n",
                                    done, skipped_cnt.load(), failed_cnt.load(), cache_jobs_list.size());
                    }
                } else {
                    const size_t fl = failed_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (fl <= 5) {
                        std::lock_guard<std::mutex> lk(log_mtx);
                        std::printf("  [CACHE FAIL] %s @0x%llX (%s) hr=0x%08lX: %.200s\n",
                                    stage_name(job.stage), static_cast<unsigned long long>(job.file_off),
                                    job.hash_hex.c_str(), static_cast<unsigned long>(hr), err);
                    }
                }
            }
        };

        std::vector<std::thread> pool;
        pool.reserve(cache_jobs);
        for (uint32_t t = 0; t < cache_jobs; ++t) pool.emplace_back(worker);
        for (auto& th : pool) th.join();

        const double bsec = std::chrono::duration<double>(std::chrono::steady_clock::now() - bt0).count();
        std::printf("  CACHE BUILD SUMMARY (%.2f s): built=%zu | resumed_skip=%zu | failed=%zu | total=%zu\n",
                    bsec, built_cnt.load(), skipped_cnt.load(), failed_cnt.load(), cache_jobs_list.size());
        SetEnvironmentVariableW(L"WSX_SHADER_CACHE_DIR", nullptr);
        if (failed_cnt.load() > 0) return 4;
    }

    return 0;
}
