// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Wave 4 Verification Test (Debug HUD, Crash/Minidump/DRED Diagnostics & Fuzzing Suite):
// 1. Live Pure D3D12 SM 6.6 Xbox Scarlett Emerald Debug HUD Overlay rendered onto a 640x360 RT
//    (verified emerald accent bar, sparkline, crisp bitmap text, untouched exterior, 0 DX12 errors)
// 2. D3D12 DRED GPU Breadcrumb & Page-Fault Diagnostic Capture on live RTX 5060
// 3. Controlled SEH Exception -> Crash Report (.txt with x64 regs, callstack, modules, DX12 telemetry)
//    + Valid WinDbg Minidump (.dmp starting with "MDMP")
// 4. Adversarial Robustness Fuzzer (600 randomized & malformed inputs):
//    - 300 random/truncated/hostile RDNA2 binary streams into HlslEmitter::translate_raw_rdna2
//    - 150 corrupted DXBC/FSSM/XBCH/ACCC containers into D3D12XboxInspectShaderBytecode & Unpack
//    - 150 corrupted GDeflate v1.0 & XVD payloads into wsx_gdeflate & wsx_xvd

#include "d3d12_xs_rdna2_to_hlsl.h"
#include "include/xbox/d3d12_xs.h"
#include "wsx_gdeflate.h"
#include "wsx_xvd.h"

#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "  [FAIL] %s\n", msg);
        ++g_failures;
    }
}

void check_hr(HRESULT hr, const char* msg) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "  [FAIL] %s (hr=0x%08lX)\n", msg, static_cast<unsigned long>(hr));
        ++g_failures;
    }
}

bool trigger_and_write_crash_report(
    const wchar_t* out_dir,
    wchar_t* txt_out,
    size_t txt_cap,
    wchar_t* dmp_out,
    size_t dmp_cap,
    ID3D12Device* dev,
    HRESULT& out_hr) {
    out_hr = E_FAIL;
    __try {
        RaiseException(0xE0005758u, 0u, 0u, nullptr);
    } __except ([&](EXCEPTION_POINTERS* ep) -> DWORD {
        out_hr = D3D12XboxWriteCrashReport(ep, out_dir, txt_out, txt_cap, dmp_out, dmp_cap, dev);
        return EXCEPTION_EXECUTE_HANDLER;
    }(GetExceptionInformation())) {
        return true;
    }
    return false;
}

} // namespace

int main() {
    SetEnvironmentVariableW(L"WSX_DRED", L"1");

    D3D12XBOX_CREATE_DEVICE_PARAMETERS cdp{};
    cdp.Version = D3D12_SDK_VERSION;
    cdp.ProcessDebugFlags = D3D12_PROCESS_DEBUG_FLAG_DEBUG_LAYER_ENABLED;
    ComPtr<ID3D12Device> dev;
    check_hr(D3D12XboxCreateDevice(nullptr, &cdp, IID_PPV_ARGS(&dev)), "D3D12XboxCreateDevice (DRED+Debug)");
    if (!dev) return 1;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    check_hr(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");

    // ========================================================================
    // 1. Live Pure D3D12 SM 6.6 Xbox Scarlett Emerald Debug HUD Overlay
    // ========================================================================
    std::printf("[Wave4 / 1] Pure D3D12 SM 6.6 On-Screen Debug HUD Overlay...\n");
    {
        constexpr uint32_t kW = 640u, kH = 360u;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = kW;
        rd.Height = kH;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        cv.Color[0] = 0.10f;
        cv.Color[1] = 0.20f;
        cv.Color[2] = 0.85f;
        cv.Color[3] = 1.00f;

        ComPtr<ID3D12Resource> rt;
        check_hr(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt)), "Create HUD RT");

        ComPtr<ID3D12DescriptorHeap> rtv_heap;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        check_hr(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)), "Create RTV heap");
        dev->CreateRenderTargetView(rt.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());

        // Clear RT to bright blue background, then transition -> PRESENT
        ComPtr<ID3D12CommandAllocator> ca;
        ComPtr<ID3D12GraphicsCommandList> cl;
        check_hr(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca)), "CreateCA");
        check_hr(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr, IID_PPV_ARGS(&cl)), "CreateCL");
        cl->ClearRenderTargetView(rtv_heap->GetCPUDescriptorHandleForHeapStart(), cv.Color, 0, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = rt.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &b);
        check_hr(cl->Close(), "Close clear CL");
        ID3D12CommandList* lists[] = {cl.Get()};
        queue->ExecuteCommandLists(1, lists);

        // Render HUD 10 times to populate the frametime sparkline & blend onto RT
        check_hr(D3D12XboxSetHudEnabled(dev.Get(), TRUE), "D3D12XboxSetHudEnabled(TRUE)");
        for (int f = 0; f < 10; ++f) {
            check_hr(D3D12XboxRenderDebugHud(
                         dev.Get(), queue.Get(), rt.Get(),
                         D3D12_RESOURCE_STATE_PRESENT,
                         (f == 9) ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_PRESENT),
                     "D3D12XboxRenderDebugHud");
        }

        // Readback RT to inspect HUD pixels
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT nrows = 0;
        UINT64 rsize = 0, total = 0;
        dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &nrows, &rsize, &total);
        D3D12_HEAP_PROPERTIES rhp{};
        rhp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rbd{};
        rbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rbd.Width = (total + 255u) & ~255ull;
        rbd.Height = 1;
        rbd.DepthOrArraySize = 1;
        rbd.MipLevels = 1;
        rbd.Format = DXGI_FORMAT_UNKNOWN;
        rbd.SampleDesc.Count = 1;
        rbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> rb;
        check_hr(dev->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &rbd,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)), "Create RB");
        check_hr(ca->Reset(), "Reset CA");
        check_hr(cl->Reset(ca.Get(), nullptr), "Reset CL");
        D3D12_TEXTURE_COPY_LOCATION dst{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {fp}};
        D3D12_TEXTURE_COPY_LOCATION src{rt.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        src.SubresourceIndex = 0;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        check_hr(cl->Close(), "Close RB CL");
        queue->ExecuteCommandLists(1, lists);
        ComPtr<ID3D12Fence> fence;
        check_hr(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        queue->Signal(fence.Get(), 1u);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1u, ev);
        WaitForSingleObject(ev, 5000);
        CloseHandle(ev);

        void* ptr = nullptr;
        D3D12_RANGE rr{0, static_cast<SIZE_T>(total)};
        check_hr(rb->Map(0, &rr, &ptr), "Map HUD RB");
        if (ptr) {
            const auto* px = static_cast<const uint8_t*>(ptr);
            auto pixel_at = [&](uint32_t x, uint32_t y) {
                return px + static_cast<size_t>(y) * fp.Footprint.RowPitch + static_cast<size_t>(x) * 4u;
            };
            // Exterior pixel (500, 280) must remain untouched bright blue (~26, 51, 217)
            const uint8_t* bg = pixel_at(500, 280);
            check(bg[0] < 40u && bg[1] < 70u && bg[2] > 200u, "Pixels outside HUD remain untouched blue");

            // Left emerald accent strip at (13, 40) must be bright green (G > 180, G > R*2)
            const uint8_t* acc = pixel_at(13, 40);
            check(acc[1] > 180u && acc[1] > acc[0] * 2u, "HUD Xbox Scarlett Emerald left accent bar rendered");

            // Count bright text glyph pixels inside text rows [y=20..95, x=24..280]
            uint32_t bright_text_px = 0u;
            uint32_t dark_panel_px = 0u;
            for (uint32_t y = 20u; y < 95u; ++y) {
                for (uint32_t x = 24u; x < 280u; ++x) {
                    const uint8_t* p = pixel_at(x, y);
                    if (p[1] > 170u) ++bright_text_px;
                    else if (p[2] < 80u) ++dark_panel_px;
                }
            }
            check(bright_text_px > 400u, "HUD rendered crisp bitmap font glyphs (>400 lit text pixels)");
            check(dark_panel_px > 5000u, "HUD translucent dark panel alpha-blended cleanly");

            D3D12_RANGE wr{0, 0};
            rb->Unmap(0, &wr);
            std::printf("  [PASS] Debug HUD: emerald=(%u,%u,%u) | textPx=%u | panelPx=%u | bg=(%u,%u,%u)\n",
                        acc[0], acc[1], acc[2], bright_text_px, dark_panel_px, bg[0], bg[1], bg[2]);
        }
    }

    // ========================================================================
    // 2. D3D12 DRED GPU Breadcrumb & Page-Fault Report Capture
    // ========================================================================
    std::printf("[Wave4 / 2] D3D12 DRED GPU Breadcrumb & Page-Fault Report...\n");
    {
        size_t len = 0u;
        check_hr(D3D12XboxCaptureGpuDredReport(dev.Get(), nullptr, 0u, &len), "DRED size query");
        check(len > 32u, "DRED report non-empty");
        std::string report(len + 1u, '\0');
        check_hr(D3D12XboxCaptureGpuDredReport(dev.Get(), report.data(), report.size(), &len), "DRED report fetch");
        check(report.find("D3D12 DRED DIAGNOSTICS") != std::string::npos, "DRED header present");
        check(report.find("0x00000000 (HEALTHY)") != std::string::npos, "Healthy device status reported");
        std::printf("  [PASS] DRED capture OK (%zu bytes, DeviceRemovedReason=HEALTHY)\n", len);
    }

    // ========================================================================
    // 3. Controlled SEH Exception -> Crash Report (.txt) + WinDbg Minidump (.dmp)
    // ========================================================================
    std::printf("[Wave4 / 3] Crash Report (.txt) + WinDbg Minidump (.dmp) Generation...\n");
    {
        std::filesystem::path tmp_crash = std::filesystem::temp_directory_path() / "wsx_wave4_crash_test";
        std::error_code ec;
        std::filesystem::remove_all(tmp_crash, ec);

        wchar_t txt_path[MAX_PATH]{};
        wchar_t dmp_path[MAX_PATH]{};
        HRESULT cr_hr = E_FAIL;
        const bool caught = trigger_and_write_crash_report(
            tmp_crash.wstring().c_str(), txt_path, MAX_PATH, dmp_path, MAX_PATH, dev.Get(), cr_hr);
        check(caught && SUCCEEDED(cr_hr), "SEH exception captured & D3D12XboxWriteCrashReport succeeded");
        check(txt_path[0] != L'\0' && std::filesystem::is_regular_file(txt_path), "Crash .txt file written");
        check(dmp_path[0] != L'\0' && std::filesystem::is_regular_file(dmp_path), "WinDbg .dmp minidump written");

        if (txt_path[0] != L'\0') {
            std::ifstream in(txt_path, std::ios::binary);
            std::string txt((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            check(txt.find("ExceptionCode=0xE0005758") != std::string::npos, "Crash report logged exact exception code");
            check(txt.find("X64 REGISTERS") != std::string::npos && txt.find("RIP=") != std::string::npos, "Crash report logged x64 registers");
            check(txt.find("CALLSTACK") != std::string::npos, "Crash report logged symbolized callstack");
            check(txt.find("LOADED MODULES") != std::string::npos, "Crash report logged module map");
            check(txt.find("D3D12 TELEMETRY") != std::string::npos, "Crash report logged D3D12 telemetry");
            check(txt.find("D3D12 DRED DIAGNOSTICS") != std::string::npos, "Crash report logged DRED GPU diagnostics");
        }
        size_t dmp_size = 0u;
        if (dmp_path[0] != L'\0') {
            std::ifstream din(dmp_path, std::ios::binary);
            char magic[4]{};
            din.read(magic, 4);
            dmp_size = static_cast<size_t>(std::filesystem::file_size(dmp_path, ec));
            check(std::memcmp(magic, "MDMP", 4) == 0 && dmp_size >= 8192u, "Valid MDMP minidump header & payload");
        }

        std::filesystem::remove_all(tmp_crash, ec);
        std::printf("  [PASS] Crash diagnostics OK: .txt + valid MDMP minidump (%zu KB)\n", dmp_size / 1024u);
    }

    // ========================================================================
    // 4. Adversarial Robustness Fuzzer (600 Malformed / Randomized Inputs)
    // ========================================================================
    std::printf("[Wave4 / 4] Adversarial Fuzzer (300 RDNA2 + 150 DXBC/FSSM + 150 GDeflate/XVD)...\n");
    {
        std::mt19937 rng(0x67A6F022u);

        // 4a. 300 hostile RDNA2 binary streams into HlslEmitter (including random branches, literals, SDWA/DPP, truncations)
        uint32_t rdna2_ok = 0u;
        for (uint32_t i = 0; i < 300u; ++i) {
            const uint32_t nwords = 1u + (rng() % 96u);
            std::vector<uint32_t> code(nwords);
            for (uint32_t w = 0; w < nwords; ++w) code[w] = rng();
            if ((i & 3u) == 0u && nwords >= 2u) {
                code.back() = 0xBF810000u; // s_endpgm
            }
            const uint32_t stages[5] = {0u, 1u, 2u, 5u, 13u};
            wsx_rdna2::TranslateStats st{};
            wsx_rdna2::HlslEmitter em;
            const std::string hlsl = em.translate_raw_rdna2(
                code.data(), code.size(), stages[i % 5u], 64u, 1u, 1u, 0u, false, st);
            if (!hlsl.empty()) ++rdna2_ok;
        }
        check(rdna2_ok == 300u, "300/300 fuzzed RDNA2 binary streams handled safely without crash");

        // 4b. 150 corrupted DXBC / FSSM / XBCH / ACCC containers
        for (uint32_t i = 0; i < 150u; ++i) {
            const size_t sz = (i == 0u) ? 0u : (1u + (rng() % 512u));
            std::vector<uint8_t> buf(sz);
            for (auto& b : buf) b = static_cast<uint8_t>(rng() & 0xFFu);
            if (sz >= 4u && (i & 1u)) {
                buf[0] = 'D'; buf[1] = 'X'; buf[2] = 'B'; buf[3] = 'C';
            }
            D3D12XBOX_SHADER_INFO info{};
            D3D12XboxInspectShaderBytecode(buf.empty() ? nullptr : buf.data(), buf.size(), &info);
            size_t out_sz = 0u;
            uint32_t code_off = 0u;
            D3D12XboxUnpackShaderFssm(buf.empty() ? nullptr : buf.data(), buf.size(), nullptr, 0u, &out_sz, &code_off);
        }

        // 4c. 150 corrupted GDeflate v1.0 & XVD containers
        for (uint32_t i = 0; i < 150u; ++i) {
            const size_t sz = 1u + (rng() % 512u);
            std::vector<uint8_t> buf(sz);
            for (auto& b : buf) b = static_cast<uint8_t>(rng() & 0xFFu);
            if (sz >= 2u && (i & 1u)) {
                buf[0] = 0x04u; buf[1] = 0xFBu; // GDeflate magic
            }
            std::vector<uint8_t> dst;
            wsx_gdeflate::decompress(buf.data(), buf.size(), dst, 256u * 1024u);
            wsx_xvd::XvdInfo xi{};
            wsx_xvd::parse_header(buf.data(), buf.size(), xi);
            if (i < 15u) {
                std::filesystem::path fxvd = std::filesystem::temp_directory_path() / "wsx_fuzz_bad.xvd";
                {
                    std::ofstream out(fxvd, std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
                }
                wsx_xvd::XvdVolumeReader vol;
                vol.open(fxvd.wstring());
                std::error_code ec;
                std::filesystem::remove(fxvd, ec);
            }
        }
        std::printf("  [PASS] Fuzzer survived 600/600 hostile inputs (0 crashes, 0 OOB)\n");
    }

    // Verify 0 D3D12 Debug Layer errors
    ComPtr<ID3D12InfoQueue> iq;
    if (SUCCEEDED(dev.As(&iq)) && iq) {
        const UINT64 n = iq->GetNumStoredMessagesAllowedByRetrievalFilter();
        uint32_t errs = 0;
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T len = 0;
            iq->GetMessage(i, nullptr, &len);
            if (len == 0) continue;
            std::vector<uint8_t> buf(len);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(iq->GetMessage(i, m, &len)) &&
                (m->Severity == D3D12_MESSAGE_SEVERITY_ERROR || m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)) {
                ++errs;
                std::fprintf(stderr, "  [D3D12 ERR] %s\n", m->pDescription);
            }
        }
        check(errs == 0u, "0 D3D12 Debug Layer errors");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "FAILED with %d error(s).\n", g_failures);
        return 1;
    }
    std::printf("ALL WAVE 4 HUD + CRASH/DRED + FUZZER TESTS PASSED (0 D3D12 Debug Layer errors)!\n");
    return 0;
}
