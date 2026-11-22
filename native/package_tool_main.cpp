// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// MizuBox 1-Click Package Auto-Discovery, CNG SHA-256 Verifier & Offline Shader/PSO Pre-Warmer:
//   wsx_package_tool --find <dir>
//   wsx_package_tool --check <dir> [--manifest <out.sha256>] [--verify <in.sha256>]
//   wsx_package_tool --prewarm-pso <shader_container> [--max N] [--cache-dir <dir>] [--resume]

#include "d3d12_xs_rdna2_to_hlsl.h"
#include "d3d12_xs_async_pso.h"
#include "wsx_inflate.h"
#include "include/xbox/d3d12_xs.h"

#include <Windows.h>
#include <bcrypt.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace {

bool compute_file_sha256(const fs::path& p, std::string& out_hex) {
    out_hex.clear();
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    DWORD obj_len = 0, cb_data = 0;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&obj_len), sizeof(obj_len), &cb_data, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    std::vector<uint8_t> hash_obj(obj_len);
    if (BCryptCreateHash(alg, &hash, hash_obj.data(), obj_len, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }

    std::ifstream f(p, std::ios::binary);
    if (!f) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    std::vector<uint8_t> buf(256u * 1024u);
    while (f) {
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = f.gcount();
        if (got > 0) {
            if (BCryptHashData(hash, buf.data(), static_cast<ULONG>(got), 0) < 0) {
                BCryptDestroyHash(hash);
                BCryptCloseAlgorithmProvider(alg, 0);
                return false;
            }
        }
    }

    uint8_t digest[32]{};
    const bool ok = (BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return false;

    static constexpr char kHex[] = "0123456789abcdef";
    out_hex.resize(64u);
    for (int i = 0; i < 32; ++i) {
        out_hex[i * 2 + 0] = kHex[(digest[i] >> 4) & 0xFu];
        out_hex[i * 2 + 1] = kHex[digest[i] & 0xFu];
    }
    return true;
}

std::string to_lower_ascii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

struct DiscoveredPackage {
    fs::path root;
    std::vector<fs::path> configs;
    std::vector<fs::path> exes;
    std::vector<fs::path> xbox_dlls;
    std::vector<fs::path> archives;
    uint64_t total_bytes = 0;
};

DiscoveredPackage discover_package(const fs::path& dir) {
    DiscoveredPackage pkg{};
    pkg.root = fs::absolute(dir);
    std::error_code ec;
    if (!fs::is_directory(pkg.root, ec)) return pkg;

    for (const auto& ent : fs::directory_iterator(pkg.root, ec)) {
        if (!ent.is_regular_file(ec)) continue;
        const fs::path p = ent.path();
        const std::string fn = to_lower_ascii(p.filename().string());
        const std::string ext = to_lower_ascii(p.extension().string());
        const uint64_t sz = ent.file_size(ec);
        if (fn == "microsoftgame.config" || fn == "appxmanifest.xml") {
            pkg.configs.push_back(p);
            pkg.total_bytes += sz;
        } else if (ext == ".exe") {
            pkg.exes.push_back(p);
            pkg.total_bytes += sz;
        } else if (ext == ".dll") {
            pkg.xbox_dlls.push_back(p);
            pkg.total_bytes += sz;
        } else if (ext == ".rpf" || ext == ".xvd" || ext == ".pak" || ext == ".ucas") {
            pkg.archives.push_back(p);
            pkg.total_bytes += sz;
        }
    }
    std::sort(pkg.configs.begin(), pkg.configs.end());
    std::sort(pkg.exes.begin(), pkg.exes.end());
    std::sort(pkg.xbox_dlls.begin(), pkg.xbox_dlls.end());
    std::sort(pkg.archives.begin(), pkg.archives.end());
    return pkg;
}

int cmd_find(const fs::path& dir) {
    const DiscoveredPackage pkg = discover_package(dir);
    std::printf("=== MizuBox Package Discovery ===\n");
    std::printf("Root      : %s\n", pkg.root.string().c_str());
    std::printf("Manifests : %zu\n", pkg.configs.size());
    for (const auto& p : pkg.configs) std::printf("  [CFG] %s\n", p.filename().string().c_str());
    std::printf("EXEs      : %zu\n", pkg.exes.size());
    for (const auto& p : pkg.exes) std::printf("  [EXE] %s\n", p.filename().string().c_str());
    std::printf("DLLs      : %zu\n", pkg.xbox_dlls.size());
    for (const auto& p : pkg.xbox_dlls) std::printf("  [DLL] %s\n", p.filename().string().c_str());
    std::printf("Archives  : %zu\n", pkg.archives.size());
    for (const auto& p : pkg.archives) {
        std::error_code ec;
        const double mb = static_cast<double>(fs::file_size(p, ec)) / (1024.0 * 1024.0);
        std::printf("  [ARC] %-32s (%.2f MB)\n", p.filename().string().c_str(), mb);
    }
    std::printf("Total size: %.2f MB\n", static_cast<double>(pkg.total_bytes) / (1024.0 * 1024.0));
    return (pkg.configs.empty() && pkg.exes.empty() && pkg.xbox_dlls.empty() && pkg.archives.empty()) ? 1 : 0;
}

int cmd_check(const fs::path& dir, const fs::path& out_manifest, const fs::path& verify_manifest) {
    if (!verify_manifest.empty()) {
        std::ifstream mf(verify_manifest);
        if (!mf) {
            std::fprintf(stderr, "[FAIL] Cannot open manifest: %s\n", verify_manifest.string().c_str());
            return 1;
        }
        uint32_t ok_cnt = 0, bad_cnt = 0;
        std::string line;
        while (std::getline(mf, line)) {
            if (line.size() < 66u) continue;
            const std::string expected = line.substr(0, 64);
            std::string rel = line.substr(64);
            while (!rel.empty() && (rel[0] == ' ' || rel[0] == '*' || rel[0] == '\t')) rel.erase(rel.begin());
            while (!rel.empty() && (rel.back() == '\r' || rel.back() == '\n')) rel.pop_back();
            std::string actual;
            const fs::path full = dir / rel;
            if (compute_file_sha256(full, actual) && actual == expected) {
                std::printf("[OK]   %s\n", rel.c_str());
                ++ok_cnt;
            } else {
                std::fprintf(stderr, "[FAIL] %s (expected %s, got %s)\n",
                             rel.c_str(), expected.c_str(), actual.empty() ? "MISSING" : actual.c_str());
                ++bad_cnt;
            }
        }
        std::printf("SHA-256 verification complete: ok=%u failed=%u\n", ok_cnt, bad_cnt);
        return (bad_cnt == 0 && ok_cnt > 0) ? 0 : 1;
    }

    const DiscoveredPackage pkg = discover_package(dir);
    std::vector<fs::path> targets;
    targets.insert(targets.end(), pkg.configs.begin(), pkg.configs.end());
    targets.insert(targets.end(), pkg.exes.begin(), pkg.exes.end());
    targets.insert(targets.end(), pkg.xbox_dlls.begin(), pkg.xbox_dlls.end());

    std::ofstream mf;
    if (!out_manifest.empty()) {
        mf.open(out_manifest, std::ios::binary);
        if (!mf) {
            std::fprintf(stderr, "[FAIL] Cannot create manifest: %s\n", out_manifest.string().c_str());
            return 1;
        }
    }

    uint32_t hashed = 0;
    for (const auto& p : targets) {
        std::string hex;
        if (compute_file_sha256(p, hex)) {
            const std::string fn = p.filename().string();
            std::printf("%s  %s\n", hex.c_str(), fn.c_str());
            if (mf) mf << hex << "  " << fn << "\n";
            ++hashed;
        }
    }
    std::printf("Hashed %u package files via Windows CNG SHA-256.\n", hashed);
    return hashed > 0 ? 0 : 1;
}

// Scan a binary container (.dll / .rpf) for embedded raw DXBC OR RPF _Wrapped (0x000DDCE7 raw-deflate) RDNA2 shaders
std::vector<std::vector<uint8_t>> extract_embedded_dxbc_blobs(const fs::path& container_path, uint32_t max_blobs) {
    std::vector<std::vector<uint8_t>> out;
    std::ifstream f(container_path, std::ios::binary | std::ios::ate);
    if (!f) return out;
    const size_t fsize = static_cast<size_t>(f.tellg());
    // Support full shaders.rpf (~250 MB) as well as Xbox DLLs
    const size_t scan_sz = std::min<size_t>(fsize, 512u * 1024u * 1024u);
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> raw(scan_sz);
    if (!f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(scan_sz))) return out;

    // 1. Scan for uncompressed embedded DXBC blobs (e.g. amd_fidelityfx_upscaler_xbox.dll, XboxBvhBuilder.dll)
    for (size_t i = 0; i + 32u <= raw.size() && out.size() < max_blobs; i += 4u) {
        if (raw[i] == 'D' && raw[i + 1] == 'X' && raw[i + 2] == 'B' && raw[i + 3] == 'C') {
            uint32_t total_sz = 0u;
            std::memcpy(&total_sz, raw.data() + i + 24u, 4u);
            if (total_sz >= 64u && total_sz <= 4u * 1024u * 1024u && i + total_sz <= raw.size()) {
                D3D12XBOX_SHADER_INFO insp{};
                if (SUCCEEDED(D3D12XboxInspectShaderBytecode(raw.data() + i, total_sz, &insp)) &&
                    (insp.Kind == D3D12XBOX_SHADER_KIND_XBOX_FSSM_RDNA2 ||
                     insp.Kind == D3D12XBOX_SHADER_KIND_XBOX_ACCC_RDNA2)) {
                    out.emplace_back(raw.data() + i, raw.data() + i + total_sz);
                    i += ((total_sz + 3u) & ~3u) - 4u;
                }
            }
        }
    }

    // 2. Scan for RPF _Wrapped raw-deflate DXBC entries marked by 0x000DDCE7 (e.g. GTA VI shaders.rpf)
    if (out.size() < max_blobs && raw.size() >= 16u) {
        std::vector<uint8_t> inflated;
        for (size_t i = 4u; i + 12u <= raw.size() && out.size() < max_blobs; ++i) {
            if (raw[i] == 0xE7u && raw[i + 1] == 0xDCu && raw[i + 2] == 0x0Du && raw[i + 3] == 0x00u) {
                uint32_t c1 = 0u, c2 = 0u, uncomp = 0u;
                std::memcpy(&c1, raw.data() + i - 4u, 4u);
                std::memcpy(&c2, raw.data() + i + 4u, 4u);
                std::memcpy(&uncomp, raw.data() + i + 8u, 4u);
                if (c1 == c2 && c1 >= 64u && c1 <= 2000000u && c1 <= uncomp + 1024u &&
                    i + 12u + c1 <= raw.size()) {
                    inflated.clear();
                    if (wsx_inflate::inflate_raw(raw.data() + i + 12u, c1, inflated, uncomp + 4096u) &&
                        inflated.size() >= 32u) {
                        D3D12XBOX_SHADER_INFO insp{};
                        if (SUCCEEDED(D3D12XboxInspectShaderBytecode(inflated.data(), inflated.size(), &insp)) &&
                            (insp.Kind == D3D12XBOX_SHADER_KIND_XBOX_FSSM_RDNA2 ||
                             insp.Kind == D3D12XBOX_SHADER_KIND_XBOX_ACCC_RDNA2)) {
                            out.push_back(std::move(inflated));
                            inflated.clear();
                        }
                    }
                    i += 11u;
                }
            }
        }
    }
    return out;
}

int cmd_prewarm_pso(const fs::path& shader_container, uint32_t max_shaders,
                    const fs::path& cache_dir, bool resume) {
    const auto t0 = std::chrono::steady_clock::now();
    std::error_code ec;
    fs::create_directories(cache_dir, ec);
    const fs::path ckpt_path = cache_dir / "prewarm_checkpoint.txt";

    std::set<std::string> done_hashes;
    if (resume && fs::is_regular_file(ckpt_path, ec)) {
        std::ifstream cf(ckpt_path);
        std::string h;
        while (std::getline(cf, h)) {
            while (!h.empty() && (h.back() == '\r' || h.back() == '\n')) h.pop_back();
            if (!h.empty()) done_hashes.insert(h);
        }
    }

    auto blobs = extract_embedded_dxbc_blobs(shader_container, max_shaders);
    std::printf("=== MizuBox Offline Shader + PSO Pre-Warmer ===\n");
    std::printf("Source     : %s (%zu RDNA2 DXBC shaders found)\n",
                shader_container.string().c_str(), blobs.size());
    std::printf("Cache Dir  : %s (resume=%d, checkpoint_entries=%zu)\n",
                cache_dir.string().c_str(), resume ? 1 : 0, done_hashes.size());
    if (blobs.empty()) {
        std::fprintf(stderr, "[FAIL] No FSSM/ACCC DXBC shaders found in %s\n", shader_container.string().c_str());
        return 1;
    }

    const fs::path pso_lib_path = cache_dir / "wsx_pso_cache.bin";
    SetEnvironmentVariableW(L"WSX_PSO_CACHE_PATH", pso_lib_path.c_str());
    SetEnvironmentVariableW(L"WSX_SHADER_CACHE_DIR", cache_dir.c_str());
    SetEnvironmentVariableW(L"WSX_AUTO_TRANSLATE_FSSM", L"1");

    ComPtr<ID3D12Device8> dev;
    D3D12XBOX_CREATE_DEVICE_PARAMETERS cdp{};
    cdp.Version = D3D12_SDK_VERSION;
    if (FAILED(D3D12XboxCreateDevice(nullptr, &cdp, IID_PPV_ARGS(&dev))) || !dev) {
        std::fprintf(stderr, "[FAIL] D3D12XboxCreateDevice failed\n");
        return 1;
    }

    ComPtr<ID3D12RootSignature> sm66_rs;
    if (FAILED(D3D12XboxCreateSm66RootSignature(dev.Get(), IID_PPV_ARGS(&sm66_rs))) || !sm66_rs) {
        std::fprintf(stderr, "[FAIL] Failed to create SM 6.6 root signature\n");
        return 1;
    }

    struct WorkItem {
        std::string key;
        const std::vector<uint8_t>* blob = nullptr;
        std::vector<uint8_t> dxil;
        uint32_t stage = 0;
        bool ok = false;
    };

    std::vector<WorkItem> work;
    uint32_t skipped_resume = 0;
    for (const auto& blob : blobs) {
        char key[33]{};
        static constexpr char kHex[] = "0123456789abcdef";
        for (int b = 0; b < 16; ++b) {
            key[b * 2 + 0] = kHex[(blob[4 + b] >> 4) & 0xFu];
            key[b * 2 + 1] = kHex[blob[4 + b] & 0xFu];
        }
        std::string key_str(key);
        if (resume && done_hashes.count(key_str) != 0u) {
            ++skipped_resume;
            continue;
        }
        done_hashes.insert(key_str);
        work.push_back(WorkItem{std::move(key_str), &blob, {}, 0u, false});
    }

    // Parallel RDNA2 -> SM 6.6 DXIL compilation across worker threads
    if (!work.empty()) {
        std::atomic<size_t> next_idx{0};
        const unsigned nthreads = std::clamp<unsigned>(
            std::min<unsigned>(static_cast<unsigned>(work.size()),
                               std::max<unsigned>(1u, std::thread::hardware_concurrency())),
            1u, 6u);
        std::vector<std::thread> workers;
        workers.reserve(nthreads);
        for (unsigned t = 0; t < nthreads; ++t) {
            workers.emplace_back([&]() {
                for (;;) {
                    const size_t i = next_idx.fetch_add(1u);
                    if (i >= work.size()) break;
                    auto& item = work[i];
                    size_t dxil_sz = 0;
                    std::vector<uint8_t> dxil(256u * 1024u);
                    D3D12XBOX_RDNA2_TRANSLATE_STATS tst{};
                    char err[256]{};
                    HRESULT hr = D3D12XboxTranslateDxbcFssmToDxil(
                        item.blob->data(), item.blob->size(), dxil.data(), dxil.size(), &dxil_sz, err, sizeof(err), &tst);
                    if (hr == E_NOT_SUFFICIENT_BUFFER && dxil_sz > dxil.size()) {
                        dxil.resize(dxil_sz);
                        hr = D3D12XboxTranslateDxbcFssmToDxil(
                            item.blob->data(), item.blob->size(), dxil.data(), dxil.size(), &dxil_sz, err, sizeof(err), &tst);
                    }
                    if (SUCCEEDED(hr) && dxil_sz > 0u) {
                        dxil.resize(dxil_sz);
                        item.dxil = std::move(dxil);
                        item.stage = tst.Stage;
                        item.ok = true;
                    }
                }
            });
        }
        for (auto& w : workers) w.join();
    }

    // Compile tiny SM 6.6 partner shaders once so VS/PS/GS/MS shaders also pre-warm real driver PSOs
    std::vector<uint8_t> pass_ps_dxil, pass_vs_all_dxil;
    {
        const char* kPassPs = "float4 main(float4 pos : SV_Position) : SV_Target0 { return float4(1,0,1,1); }";
        std::string pass_vs_all = "struct Out {\n float4 pos : SV_Position;\n";
        for (uint32_t i = 0; i < 31u; ++i) {
            pass_vs_all += " float4 p" + std::to_string(i) + " : TEXCOORD" + std::to_string(i) + ";\n";
        }
        pass_vs_all += "};\nOut main(uint vid : SV_VertexID) { Out o = (Out)0; o.pos = float4(0,0,0,1); return o; }\n";
        char perr[256]{};
        wsx_sm66::compile_hlsl_to_dxil_vec(kPassPs, std::strlen(kPassPs), L"main", L"ps_6_6", pass_ps_dxil, perr, sizeof(perr));
        wsx_sm66::compile_hlsl_to_dxil_vec(pass_vs_all.c_str(), pass_vs_all.size(), L"main", L"vs_6_6", pass_vs_all_dxil, perr, sizeof(perr));
    }

    uint32_t compiled = 0, pso_created = 0, failed = 0;
    uint32_t st_ps = 0, st_vs = 0, st_gs = 0, st_cs = 0, st_ms = 0;
    std::ofstream ckpt_out(ckpt_path, resume ? (std::ios::app | std::ios::binary) : std::ios::binary);
    for (const auto& item : work) {
        if (!item.ok) {
            ++failed;
            continue;
        }
        ++compiled;
        if (item.stage == 0u) ++st_ps;
        else if (item.stage == 1u) ++st_vs;
        else if (item.stage == 2u) ++st_gs;
        else if (item.stage == 5u) ++st_cs;
        else if (item.stage == 13u) ++st_ms;

        const fs::path dxil_file = cache_dir / (item.key + ".dxil");
        std::ofstream df(dxil_file, std::ios::binary);
        if (df) df.write(reinterpret_cast<const char*>(item.dxil.data()), static_cast<std::streamsize>(item.dxil.size()));

        if (item.stage == 5u) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = sm66_rs.Get();
            pd.CS = {item.dxil.data(), item.dxil.size()};
            ComPtr<ID3D12PipelineState> pso;
            if (SUCCEEDED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso))) && pso) {
                ++pso_created;
            }
        } else if (item.stage == 13u && !pass_ps_dxil.empty()) {
            struct alignas(void*) MeshStream {
                D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t_rs = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
                ID3D12RootSignature* rs = nullptr;
                D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t_ms = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS;
                D3D12_SHADER_BYTECODE ms{};
                D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t_ps = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;
                D3D12_SHADER_BYTECODE ps{};
                D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t_rtv = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
                D3D12_RT_FORMAT_ARRAY rtv{};
            } ms_stream{};
            ms_stream.rs = sm66_rs.Get();
            ms_stream.ms = {item.dxil.data(), item.dxil.size()};
            ms_stream.ps = {pass_ps_dxil.data(), pass_ps_dxil.size()};
            ms_stream.rtv.NumRenderTargets = 1u;
            ms_stream.rtv.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof(ms_stream), &ms_stream};
            ComPtr<ID3D12PipelineState> pso;
            if (SUCCEEDED(dev->CreatePipelineState(&sd, IID_PPV_ARGS(&pso))) && pso) {
                ++pso_created;
            }
        } else if ((item.stage == 0u || item.stage == 1u || item.stage == 2u) &&
                   !pass_ps_dxil.empty() && !pass_vs_all_dxil.empty()) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC gd{};
            gd.pRootSignature = sm66_rs.Get();
            gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            gd.SampleMask = UINT_MAX;
            gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            gd.NumRenderTargets = (item.stage == 0u) ? 8u : 1u;
            for (UINT r = 0; r < gd.NumRenderTargets; ++r) gd.RTVFormats[r] = DXGI_FORMAT_R8G8B8A8_UNORM;
            gd.SampleDesc.Count = 1u;
            if (item.stage == 0u) {
                gd.VS = {pass_vs_all_dxil.data(), pass_vs_all_dxil.size()};
                gd.PS = {item.dxil.data(), item.dxil.size()};
            } else if (item.stage == 1u) {
                gd.VS = {item.dxil.data(), item.dxil.size()};
                gd.PS = {pass_ps_dxil.data(), pass_ps_dxil.size()};
            } else {
                gd.VS = {pass_vs_all_dxil.data(), pass_vs_all_dxil.size()};
                gd.GS = {item.dxil.data(), item.dxil.size()};
                gd.PS = {pass_ps_dxil.data(), pass_ps_dxil.size()};
            }
            ComPtr<ID3D12PipelineState> pso;
            if (SUCCEEDED(dev->CreateGraphicsPipelineState(&gd, IID_PPV_ARGS(&pso))) && pso) {
                ++pso_created;
            }
        }
        if (ckpt_out) {
            ckpt_out << item.key << "\n";
            ckpt_out.flush();
        }
    }

    D3D12XboxFlushPsoCache(dev.Get());
    D3D12XBOX_PSO_CACHE_STATS pstats{};
    D3D12XboxGetPsoCacheStats(&pstats);

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("Pre-warm complete in %.1f ms: compiled=%u (PS=%u VS=%u GS=%u CS=%u MS=%u) pso_created=%u skipped_resume=%u failed=%u\n",
                ms, compiled, st_ps, st_vs, st_gs, st_cs, st_ms, pso_created, skipped_resume, failed);
    std::printf("Binary PSO Cache: hits=%llu stores=%llu flushes=%llu libBytes=%llu\n",
                static_cast<unsigned long long>(pstats.hits),
                static_cast<unsigned long long>(pstats.stores),
                static_cast<unsigned long long>(pstats.flushes),
                static_cast<unsigned long long>(pstats.library_bytes));
    return (failed == 0 && (compiled + skipped_resume) > 0) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("Usage:\n"
                    "  wsx_package_tool --find <dir>\n"
                    "  wsx_package_tool --check <dir> [--manifest <out.sha256>] [--verify <in.sha256>]\n"
                    "  wsx_package_tool --prewarm-pso <dll_or_rpf> [--max N] [--cache-dir <dir>] [--resume]\n");
        return 2;
    }

    std::string mode = argv[1];
    if (mode == "--find") {
        const fs::path dir = (argc >= 3) ? fs::path(argv[2]) : fs::current_path();
        return cmd_find(dir);
    }
    if (mode == "--check") {
        fs::path dir = (argc >= 3 && argv[2][0] != '-') ? fs::path(argv[2]) : fs::current_path();
        fs::path out_m, ver_m;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--manifest") == 0 && i + 1 < argc) out_m = argv[++i];
            else if (std::strcmp(argv[i], "--verify") == 0 && i + 1 < argc) ver_m = argv[++i];
        }
        return cmd_check(dir, out_m, ver_m);
    }
    if (mode == "--prewarm-pso") {
        if (argc < 3) return 2;
        fs::path src = argv[2];
        uint32_t max_n = 16u;
        fs::path cache_dir = fs::current_path() / "wsx_shader_cache";
        bool resume = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--max") == 0 && i + 1 < argc) max_n = static_cast<uint32_t>(std::atoi(argv[++i]));
            else if (std::strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc) cache_dir = argv[++i];
            else if (std::strcmp(argv[i], "--resume") == 0) resume = true;
        }
        return cmd_prewarm_pso(src, max_n, cache_dir, resume);
    }
    return 2;
}
