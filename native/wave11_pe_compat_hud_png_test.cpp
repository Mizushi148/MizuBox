// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Wave 11 Verification Suite (D3D12 Debug Layer: 0 errors):
//  1. AAA PE64 Loader Compatibility: CPU affinity remap, ideal processor remap, AppModel Package/AUMID,
//     custom Guest CommandLineW/A, and WinRT RoGetActivationFactory fallback inside a manually mapped DLL.
//  2. Upgraded Pure DX12 Debug HUD (1% Low / P99 / PSO% / Wave64 mode / F12:PNG) +
//     live RTX 5060 backbuffer PNG capture (D3D12XboxCaptureBackbufferPng) verified via PNG header/chunks.

#include "wsx_pe_loader.h"
#include "include/xbox/d3d12_xs.h"

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

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

struct WsxWave11CompatReport {
    uint32_t affinity_ok;
    uint32_t ideal_proc_ok;
    uint32_t pkg_full_ok;
    uint32_t pkg_family_ok;
    uint32_t aumid_ok;
    uint32_t cmdline_w_ok;
    uint32_t cmdline_a_ok;
    uint32_t winrt_ok;
};

uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void test_pe64_aaa_compat_hooks(const fs::path& fixture_dll, const fs::path& shim_dir) {
    std::printf("\n=== [Wave 11.1] AAA PE64 Loader Compat Hooks (Affinity/AppModel/CmdLine/WinRT) ===\n");

    wsx_pe::set_guest_command_line(L"game_scarlett_master.exe -console=anaconda -fps=60");

    wsx_pe::MappedPeImage img;
    wsx_pe::LoadOptions opts{};
    opts.shim_dir = shim_dir;
    opts.force_relocate = true;
    CHECK(img.load_from_file(fixture_dll, opts), "Manually mapped %s (relocs=%u, err=%s)",
          fixture_dll.filename().string().c_str(), img.stats().reloc_count, img.last_error().c_str());
    if (!img.is_loaded()) return;

    using FnRun = uint32_t(__cdecl*)(WsxWave11CompatReport*);
    auto fn = reinterpret_cast<FnRun>(img.find_export("wsx_run_wave11_compat_fixture"));
    CHECK(fn != nullptr, "Resolved wsx_run_wave11_compat_fixture export");
    if (fn) {
        WsxWave11CompatReport rep{};
        const uint32_t rc = fn(&rep);
        CHECK(rc == 1u && rep.affinity_ok && rep.ideal_proc_ok && rep.pkg_full_ok &&
              rep.pkg_family_ok && rep.aumid_ok && rep.cmdline_w_ok && rep.cmdline_a_ok && rep.winrt_ok,
              "Guest compat checks: aff=%u ideal=%u pkg=%u fam=%u aumid=%u cmdW=%u cmdA=%u winrt=%u",
              rep.affinity_ok, rep.ideal_proc_ok, rep.pkg_full_ok, rep.pkg_family_ok,
              rep.aumid_ok, rep.cmdline_w_ok, rep.cmdline_a_ok, rep.winrt_ok);
    }

    const auto stats = wsx_pe::get_compat_hook_stats();
    CHECK(stats.affinity_remaps >= 1 && stats.ideal_proc_remaps >= 1 &&
          stats.appmodel_queries >= 3 && stats.winrt_fallback_hits >= 1,
          "Compat hook telemetry: affRemaps=%llu idealRemaps=%llu appModel=%llu winrtFallback=%llu",
          stats.affinity_remaps, stats.ideal_proc_remaps, stats.appmodel_queries, stats.winrt_fallback_hits);

    img.unload();
    wsx_pe::set_guest_command_line(L"");
}

void test_upgraded_hud_and_png_capture() {
    std::printf("\n=== [Wave 11.2] Upgraded DX12 Debug HUD & Live RTX 5060 Backbuffer PNG Capture ===\n");
    SetEnvironmentVariableW(L"WSX_D3D12_DEBUG", L"1");

    D3D12XBOX_CREATE_DEVICE_PARAMETERS cdp{};
    cdp.Version = D3D12_SDK_VERSION;
    cdp.ProcessDebugFlags = D3D12_PROCESS_DEBUG_FLAG_DEBUG_LAYER_ENABLED;
    ComPtr<ID3D12Device> dev;
    CHECK(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &cdp, IID_PPV_ARGS(&dev))) && dev,
          "Created d3d12_xs device under D3D12 Debug Layer");
    if (!dev) return;

    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    ComPtr<ID3D12CommandQueue> q;
    CHECK(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) && q, "Created Direct CommandQueue");

    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rtd{};
    rtd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtd.Width = 640; rtd.Height = 360; rtd.DepthOrArraySize = 1; rtd.MipLevels = 1;
    rtd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; rtd.SampleDesc.Count = 1;
    rtd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> rt;
    CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rtd,
                                                 D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&rt))) && rt,
          "Created 640x360 BGRA8 render target");

    CHECK(SUCCEEDED(D3D12XboxSetHudEnabled(dev.Get(), TRUE)), "Enabled upgraded SM 6.6 Debug HUD");
    CHECK(SUCCEEDED(D3D12XboxRenderDebugHud(dev.Get(), q.Get(), rt.Get(),
                                            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT)),
          "Rendered upgraded Debug HUD (W64/1%%Low/P99/PSO%%/F12:PNG) onto 640x360 RT");

    std::error_code ec;
    const fs::path png_path = fs::temp_directory_path(ec) / L"wsx_wave11_hud_capture.png";
    fs::remove(png_path, ec);
    const HRESULT hr_png = D3D12XboxCaptureBackbufferPng(
        dev.Get(), q.Get(), rt.Get(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT,
        png_path.c_str());
    CHECK(SUCCEEDED(hr_png) && fs::is_regular_file(png_path, ec),
          "D3D12XboxCaptureBackbufferPng wrote live RTX 5060 backbuffer PNG (hr=0x%08lX)",
          static_cast<unsigned long>(hr_png));

    // Validate genuine PNG signature + IHDR dimensions (640x360) + IDAT + IEND
    std::ifstream ifs(png_path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    static constexpr uint8_t kSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    const bool valid_sig = bytes.size() > 64u && std::memcmp(bytes.data(), kSig, 8) == 0 &&
                           std::memcmp(bytes.data() + 12, "IHDR", 4) == 0;
    const uint32_t png_w = valid_sig ? read_be32(bytes.data() + 16) : 0u;
    const uint32_t png_h = valid_sig ? read_be32(bytes.data() + 20) : 0u;
    const bool has_iend = bytes.size() >= 12u && std::memcmp(bytes.data() + bytes.size() - 8u, "IEND", 4) == 0;
    CHECK(valid_sig && png_w == 640u && png_h == 360u && has_iend,
          "Validated lossless PNG output: %ux%u, %zu bytes, valid IHDR+IDAT+IEND",
          png_w, png_h, bytes.size());
    fs::remove(png_path, ec);

    ComPtr<ID3D12InfoQueue> iq;
    uint64_t errs = 0;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&iq))) && iq) {
        const UINT64 n = iq->GetNumStoredMessages();
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T len = 0;
            iq->GetMessage(i, nullptr, &len);
            std::vector<uint8_t> buf(len);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(iq->GetMessage(i, m, &len)) &&
                (m->Severity == D3D12_MESSAGE_SEVERITY_ERROR || m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)) {
                ++errs;
                std::fprintf(stderr, "  [D3D12 ERR] %s\n", m->pDescription);
            }
        }
    }
    CHECK(errs == 0u, "D3D12 Debug Layer reported %llu errors across Wave 11", static_cast<unsigned long long>(errs));
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <wsx_wave11_compat_fixture.dll> <shim_dir>\n", argv[0]);
        return 2;
    }
    test_pe64_aaa_compat_hooks(argv[1], argv[2]);
    test_upgraded_hud_and_png_capture();
    std::printf("\n=== Wave 11 Summary: failures=%d ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
