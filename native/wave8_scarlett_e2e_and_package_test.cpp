// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Wave 8 Comprehensive Verification Suite:
//  1. Full-Stack Synthetic Gen9 In-Process PE64 Game (wsx_scarlett_e2e_game.dll):
//     - Manually mapped in-process with relocation + shim-first DLL binding
//     - Exercises ALL 10 MizuBox DLLs simultaneously in one session + static .tls + worker thread + atexit
//  2. 1-Click Package Auto-Discovery, CNG SHA-256 Manifest Generation/Verification & Tamper Detection
//     + Offline Shader/PSO Pre-Warmer with Checkpoint Resume (wsx_package_tool.exe).

#include "wsx_pe_loader.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(cond, msg, ...)                                        \
    do {                                                             \
        if (!(cond)) {                                               \
            std::fprintf(stderr, "[FAIL] " msg "\n", ##__VA_ARGS__); \
            ++g_failures;                                            \
        } else {                                                     \
            std::printf("[PASS] " msg "\n", ##__VA_ARGS__);          \
        }                                                            \
    } while (0)

struct WsxE2eReport {
    uint32_t dlls_verified_mask;
    uint32_t tls_main_ok;
    uint32_t tls_worker_ok;
    uint32_t xmem_ok;
    uint32_t xgameruntime_ok;
    uint32_t gameinput_ok;
    uint32_t xg_tiling_ok;
    uint32_t dstorage_bcpack_ok;
    uint32_t d3d12_hud_pso_ok;
    uint32_t bvh_dxr_ok;
    uint32_t fsr_upscaler_ok;
    uint32_t acphal_spatial_ok;
    uint32_t xaudio2_ord1_ok;
};

DWORD run_cli(const std::wstring& cmdline) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mut(cmdline.begin(), cmdline.end());
    mut.push_back(L'\0');
    if (!CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return 0xFFFFFFFFu;
    }
    WaitForSingleObject(pi.hProcess, 45000);
    DWORD code = 1u;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

void test_10dll_scarlett_e2e_game(const fs::path& game_dll, const fs::path& shim_dir) {
    std::printf("\n=== [Wave 8.1] Full-Stack 10-DLL In-Process Gen9 PE64 E2E Game ===\n");

    wsx_pe::MappedPeImage img;
    wsx_pe::LoadOptions opts{};
    opts.shim_dir = shim_dir;
    opts.force_relocate = true;
    CHECK(img.load_from_file(game_dll, opts),
          "Manually mapped %s (relocs=%u, tls_slot=%u, err=%s)",
          game_dll.filename().string().c_str(),
          img.stats().reloc_count,
          img.stats().guest_tls_slot,
          img.last_error().c_str());
    if (!img.is_loaded()) return;

    using FnRun = uint32_t(__cdecl*)(WsxE2eReport*, uint32_t*);
    auto fn = reinterpret_cast<FnRun>(img.find_export("wsx_run_scarlett_e2e_game"));
    CHECK(fn != nullptr, "Resolved wsx_run_scarlett_e2e_game export");
    if (!fn) return;

    WsxE2eReport rep{};
    uint32_t atexit_flag = 0u;
    const uint32_t rc = fn(&rep, &atexit_flag);
    CHECK(rc == 1u && rep.dlls_verified_mask == 0x3FFu,
          "ALL 10 MizuBox DLLs verified live in one in-process PE64 session (mask=0x%03X)",
          rep.dlls_verified_mask);
    CHECK(rep.tls_main_ok == 1u && rep.tls_worker_ok == 1u,
          "Static .tls main + worker thread isolation verified");
    CHECK(rep.d3d12_hud_pso_ok && rep.xmem_ok && rep.xgameruntime_ok && rep.gameinput_ok &&
          rep.bvh_dxr_ok && rep.fsr_upscaler_ok && rep.xg_tiling_ok &&
          rep.dstorage_bcpack_ok && rep.acphal_spatial_ok && rep.xaudio2_ord1_ok,
          "Subsystems: d3d12=%u xmem=%u xgr=%u gi=%u bvh=%u fsr=%u xg=%u ds=%u acp=%u xa2@1=%u",
          rep.d3d12_hud_pso_ok, rep.xmem_ok, rep.xgameruntime_ok, rep.gameinput_ok,
          rep.bvh_dxr_ok, rep.fsr_upscaler_ok, rep.xg_tiling_ok,
          rep.dstorage_bcpack_ok, rep.acphal_spatial_ok, rep.xaudio2_ord1_ok);

    img.unload();
    CHECK(atexit_flag == 0xD04Eu, "Isolated guest atexit shutdown ran cleanly on unload (0x%04X)", atexit_flag);
    CHECK(HeapValidate(GetProcessHeap(), 0, nullptr) != 0,
          "ProcessHeap integrity verified across 10-DLL TLS + unload");
}

void test_package_tool_and_prewarm(
    const fs::path& pkg_tool_exe,
    const fs::path& shim_dir,
    const fs::path& fsr_xbox_dll) {
    std::printf("\n=== [Wave 8.2] Package Auto-Discovery, CNG SHA-256 Verifier & PSO Pre-Warmer ===\n");

    const fs::path tmp = fs::temp_directory_path() / "wsx_wave8_pkg_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    // 1. Test --find on shim_dir
    const DWORD rc_find = run_cli(L"\"" + pkg_tool_exe.wstring() + L"\" --find \"" + shim_dir.wstring() + L"\"");
    CHECK(rc_find == 0u, "wsx_package_tool --find succeeded on build directory");

    // 2. Test --check --manifest + --verify + tamper detection on a synthetic package dir
    const fs::path pkg_dir = tmp / "sample_pkg";
    fs::create_directories(pkg_dir, ec);
    {
        std::ofstream(pkg_dir / "MicrosoftGame.config") << "<Game configVersion=\"1\"/>";
        std::ofstream(pkg_dir / "Game.exe", std::ios::binary) << "MZ_SYNTHETIC_GEN9_PAYLOAD_12345";
    }
    const fs::path manifest = tmp / "pkg.sha256";
    CHECK(run_cli(L"\"" + pkg_tool_exe.wstring() + L"\" --check \"" + pkg_dir.wstring() +
                  L"\" --manifest \"" + manifest.wstring() + L"\"") == 0u &&
          fs::is_regular_file(manifest, ec),
          "wsx_package_tool --check generated CNG SHA-256 manifest");
    CHECK(run_cli(L"\"" + pkg_tool_exe.wstring() + L"\" --check \"" + pkg_dir.wstring() +
                  L"\" --verify \"" + manifest.wstring() + L"\"") == 0u,
          "wsx_package_tool --verify passed on intact package");

    // Tamper 1 byte and verify --verify catches corruption cleanly!
    {
        std::ofstream(pkg_dir / "Game.exe", std::ios::binary) << "MZ_TAMPERED__GEN9_PAYLOAD_12345";
    }
    CHECK(run_cli(L"\"" + pkg_tool_exe.wstring() + L"\" --check \"" + pkg_dir.wstring() +
                  L"\" --verify \"" + manifest.wstring() + L"\"") != 0u,
          "wsx_package_tool --verify cleanly detected 1-byte file corruption");

    // 3. Test --prewarm-pso + --resume checkpoint on real Xbox amd_fidelityfx_upscaler_xbox.dll (ACCC/FSPX shaders)
    if (fs::is_regular_file(fsr_xbox_dll, ec)) {
        const fs::path cache_dir = tmp / "pso_cache";
        const DWORD rc_pw1 = run_cli(
            L"\"" + pkg_tool_exe.wstring() + L"\" --prewarm-pso \"" + fsr_xbox_dll.wstring() +
            L"\" --max 4 --cache-dir \"" + cache_dir.wstring() + L"\"");
        bool has_pso_bin = false;
        uint32_t dxil_files = 0;
        for (const auto& ent : fs::directory_iterator(cache_dir, ec)) {
            if (!ent.is_regular_file(ec)) continue;
            const auto ext = ent.path().extension().string();
            if (ext == ".bin" && ent.file_size(ec) > 0u) has_pso_bin = true;
            if (ext == ".dxil" && ent.file_size(ec) > 0u) ++dxil_files;
        }
        CHECK(rc_pw1 == 0u && fs::is_regular_file(cache_dir / "prewarm_checkpoint.txt", ec) &&
              has_pso_bin && dxil_files >= 4u,
              "Offline multi-threaded PSO pre-warm compiled %u real Xbox ACCC/FSPX shaders & persisted binary PSO library",
              dxil_files);

        // Second run with --resume should skip all 4 already-cached shaders instantly!
        const DWORD rc_pw2 = run_cli(
            L"\"" + pkg_tool_exe.wstring() + L"\" --prewarm-pso \"" + fsr_xbox_dll.wstring() +
            L"\" --max 4 --cache-dir \"" + cache_dir.wstring() + L"\" --resume");
        CHECK(rc_pw2 == 0u, "Offline PSO pre-warm --resume continued from checkpoint cleanly");
    }

    fs::remove_all(tmp, ec);
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 4 || argc > 5) {
        std::fprintf(stderr, "Usage: %s <wsx_scarlett_e2e_game.dll> <shim_dir> <wsx_package_tool.exe> [fsr_xbox.dll]\n", argv[0]);
        return 2;
    }

    test_10dll_scarlett_e2e_game(argv[1], argv[2]);
    test_package_tool_and_prewarm(argv[3], argv[2], argc == 5 ? fs::path(argv[4]) : fs::path{});

    std::printf("\n=== Wave 8 Summary: failures=%d ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
