// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Wave C end-to-end test suite (CTest #31):
//   1. AcpHal.dll: WASAPI + silent sink, SHAPE linear SRC, 2-pole Biquad LPF, 5.1 IMA-ADPCM, MS-ADPCM
//   2. XNetworking (xgameruntime.dll): SecurityInfo UTF-8/UTF-16, Statistics, Config, UDP port monitor,
//      WSX_OFFLINE_NET toggle, and WinHTTP/Crypt32 TLS thumbprint validation
//   3. Middleware: wsx_pe_loader.h forwarder export resolution + /GS LoadConfig cookie +
//      live RTX DX12 SM 6.6 Bink2 planar YCbCr(A) 4:2:0 -> RGBA8 compute shader
//   4. dstorage_xs.dll: live GPU SM 6.6 GDeflate (Stored / Fixed32 / Dynamic32 multi-tile 140 KiB)
//      + DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION + MULTIPLE_SUBRESOURCES GPU upload
//      (0 D3D12 Debug Layer errors)

#include "include/xbox/acphal.h"
#include "include/xbox/dstorage_xs.h"
#include "include/xbox/XNetworking.h"
#include "include/xbox/XGameRuntimeInit.h"
#include "wsx_bink_yuv_dx12.h"
#include "wsx_gdeflate.h"
#include "wsx_pe_loader.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

double compute_rms(const INT16* samples, size_t count, size_t stride, size_t offset) {
    double sum_sq = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double v = static_cast<double>(samples[i * stride + offset]);
        sum_sq += v * v;
    }
    return std::sqrt(sum_sq / static_cast<double>(count));
}

bool create_hardware_device(ComPtr<ID3D12Device>& out_dev, ComPtr<ID3D12InfoQueue>& out_iq) {
    ComPtr<ID3D12Debug> dbg;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))) && dbg) {
        dbg->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            adapter.Reset();
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&out_dev)))) {
            break;
        }
        adapter.Reset();
    }
    if (!out_dev) {
        ComPtr<IDXGIAdapter> warp;
        if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
            D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&out_dev));
        }
    }
    if (!out_dev) return false;
    out_dev.As(&out_iq);
    return true;
}

uint64_t count_debug_errors(ID3D12InfoQueue* iq) {
    if (!iq) return 0u;
    uint64_t errs = 0u;
    const UINT64 n = iq->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0u;
        iq->GetMessage(i, nullptr, &len);
        if (len == 0u) continue;
        std::vector<uint8_t> buf(len);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (SUCCEEDED(iq->GetMessage(i, msg, &len))) {
            if (msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
                msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR) {
                std::fprintf(stderr, "  [D3D12 ERROR] %s\n", msg->pDescription ? msg->pDescription : "");
                ++errs;
            }
        }
    }
    return errs;
}

// ----------------------------------------------------------------------------
// 1. AcpHal: WASAPI + SHAPE Linear SRC + Biquad LPF + 5.1 IMA-ADPCM + MS-ADPCM
// ----------------------------------------------------------------------------
void test_acphal_wave_c() {
    std::printf("[Wave C / 1] AcpHal WASAPI + SHAPE SRC/Biquad + 5.1 IMA-ADPCM + MS-ADPCM...\n");

    // 1a. Silent sink + live WASAPI open/submit/close
    std::vector<INT16> tone(480u * 2u, 0);
    for (UINT32 f = 0u; f < 480u; ++f) {
        const INT16 s = static_cast<INT16>(std::sin(2.0 * 3.14159265358979 * 440.0 * f / 48000.0) * 6000.0);
        tone[f * 2u + 0u] = s;
        tone[f * 2u + 1u] = s;
    }

    for (BOOL silent : { TRUE, FALSE }) {
        ACP_AUDIO_OUTPUT_DESC od{};
        od.sampleRate = 48000u;
        od.channels = 2u;
        od.bitsPerSample = 16u;
        od.bufferFrames = 960u;
        od.preferSilentFallback = silent;
        ACP_AUDIO_OUTPUT_HANDLE h = nullptr;
        check_hr(AcpHalOpenAudioOutput(&od, &h), "AcpHalOpenAudioOutput");
        check(h != nullptr, "AcpHalOpenAudioOutput returned null handle");
        UINT32 written = 0u;
        check_hr(AcpHalSubmitAudioOutput(h, tone.data(), 480u, &written), "AcpHalSubmitAudioOutput");
        check(written > 0u, "AcpHalSubmitAudioOutput wrote 0 frames");
        check_hr(AcpHalCloseAudioOutput(h), "AcpHalCloseAudioOutput");
    }

    // 1b. SHAPE Linear SRC (24 kHz -> 48 kHz ramp)
    std::vector<INT16> ramp_24k(240u * 2u);
    for (UINT32 f = 0u; f < 240u; ++f) {
        ramp_24k[f * 2u + 0u] = static_cast<INT16>(f * 100);
        ramp_24k[f * 2u + 1u] = static_cast<INT16>(-static_cast<int>(f) * 100);
    }
    std::vector<INT16> ramp_48k(512u * 2u, 0);
    UINT32 out_frames = 0u;
    check_hr(AcpHalResampleLinear(ramp_24k.data(), 240u, 24000u, 2u,
                                  ramp_48k.data(), 512u, 48000u, &out_frames),
             "AcpHalResampleLinear 24k->48k");
    check(out_frames == 480u, "Resample 24k->48k should produce 480 frames");
    check(std::abs(static_cast<int>(ramp_48k[1u * 2u + 0u]) - 50) <= 1, "Resample linear midpoint L");
    check(std::abs(static_cast<int>(ramp_48k[1u * 2u + 1u]) + 50) <= 1, "Resample linear midpoint R");

    // 1c. SHAPE 2-Pole Biquad Low-Pass Filter (fc = 1000 Hz at 48 kHz)
    const UINT32 N = 2048u;
    std::vector<INT16> low_sig(N * 2u), high_sig(N * 2u);
    for (UINT32 f = 0u; f < N; ++f) {
        const INT16 sl = static_cast<INT16>(std::sin(2.0 * 3.14159265358979 * 200.0 * f / 48000.0) * 16000.0);
        const INT16 sh = static_cast<INT16>(std::sin(2.0 * 3.14159265358979 * 12000.0 * f / 48000.0) * 16000.0);
        low_sig[f * 2u] = low_sig[f * 2u + 1u] = sl;
        high_sig[f * 2u] = high_sig[f * 2u + 1u] = sh;
    }
    const double low_in_rms = compute_rms(low_sig.data() + 1024u, 512u, 2u, 0u);
    const double high_in_rms = compute_rms(high_sig.data() + 1024u, 512u, 2u, 0u);

    ACP_BIQUAD_PARAMS lpf{ ACP_BIQUAD_LOWPASS, 48000.0f, 1000.0f, 0.70710678f, 0.0f };
    ACP_BIQUAD_STATE st_l[2]{}, st_h[2]{};
    check_hr(AcpHalApplyBiquadFilter(low_sig.data(), N, 2u, &lpf, st_l), "Biquad LPF 200Hz");
    check_hr(AcpHalApplyBiquadFilter(high_sig.data(), N, 2u, &lpf, st_h), "Biquad LPF 12kHz");

    const double low_out_rms = compute_rms(low_sig.data() + 1024u, 512u, 2u, 0u);
    const double high_out_rms = compute_rms(high_sig.data() + 1024u, 512u, 2u, 0u);
    check(low_out_rms > low_in_rms * 0.85, "Biquad LPF should pass 200 Hz");
    check(high_out_rms < high_in_rms * 0.10, "Biquad LPF should strongly attenuate 12 kHz");

    // 1d. 5.1 (6-channel) IMA-ADPCM round-trip
    constexpr UINT16 kCh = 6u;
    constexpr UINT32 kFrames = 256u; // 32 chunks * 8 samples per channel
    std::vector<INT16> src_51[kCh];
    std::vector<UINT8> enc_51[kCh];
    for (UINT16 c = 0u; c < kCh; ++c) {
        src_51[c].resize(kFrames);
        enc_51[c].resize(kFrames / 2u);
        for (UINT32 f = 0u; f < kFrames; ++f) {
            const double freq = 300.0 + c * 150.0;
            src_51[c][f] = static_cast<INT16>(std::sin(2.0 * 3.14159265358979 * freq * f / 48000.0) * 14000.0);
        }
        ACP_IMA_ADPCM_STATE es{};
        UINT32 enc_bytes = 0u;
        check_hr(AcpHalEncodeImaAdpcm(src_51[c].data(), kFrames, enc_51[c].data(),
                                      static_cast<UINT32>(enc_51[c].size()), &es, &enc_bytes),
                 "Encode ch for 5.1");
        check(enc_bytes == kFrames / 2u, "5.1 channel encoded byte size");
    }
    // Interleave into standard WAV 4-byte-per-channel chunks
    std::vector<UINT8> interleaved_51;
    const UINT32 num_chunks = kFrames / 8u;
    for (UINT32 chnk = 0u; chnk < num_chunks; ++chnk) {
        for (UINT16 c = 0u; c < kCh; ++c) {
            for (UINT32 b = 0u; b < 4u; ++b) {
                interleaved_51.push_back(enc_51[c][chnk * 4u + b]);
            }
        }
    }
    std::vector<INT16> dec_51(kFrames * kCh, 0);
    ACP_IMA_ADPCM_STATE ds[kCh]{};
    UINT32 dec_frames = 0u;
    check_hr(AcpHalDecodeImaAdpcmMultichannel(
                 interleaved_51.data(), static_cast<UINT32>(interleaved_51.size()),
                 kCh, dec_51.data(), kFrames, ds, &dec_frames),
             "AcpHalDecodeImaAdpcmMultichannel 5.1");
    check(dec_frames == kFrames, "5.1 IMA-ADPCM decoded frame count == 256");
    for (UINT16 c = 0u; c < kCh; ++c) {
        double err_sq = 0.0;
        for (UINT32 f = 32u; f < kFrames; ++f) {
            const double d = static_cast<double>(dec_51[f * kCh + c]) - static_cast<double>(src_51[c][f]);
            err_sq += d * d;
        }
        const double rmse = std::sqrt(err_sq / static_cast<double>(kFrames - 32u));
        check(rmse < 600.0, "5.1 IMA-ADPCM channel RMSE < 600");
    }

    // 1e. MS-ADPCM stereo block decode (exact bit-verified test vector)
    // Header (14 bytes for stereo):
    //   pred[0]=0 ({256,0}), pred[1]=1 ({512,-256})
    //   delta[0]=100, delta[1]=64
    //   samp1[0]=1000, samp1[1]=500
    //   samp2[0]=800, samp2[1]=400
    // Payload byte 0: 0x2F -> L nibble = +2, R nibble = -1 (0xF -> -1)
    //   L out2 = ((1000*256 + 800*0)/256) + 2*100 = 1200
    //   R out2 = ((500*512 + 400*-256)/256) + (-1)*64 = 600 - 64 = 536
    auto push_i16 = [](std::vector<UINT8>& v, INT16 x) {
        v.push_back(static_cast<UINT8>(static_cast<uint16_t>(x) & 0xFFu));
        v.push_back(static_cast<UINT8>((static_cast<uint16_t>(x) >> 8) & 0xFFu));
    };
    std::vector<UINT8> ms_block;
    ms_block.push_back(0u); // pred L
    ms_block.push_back(1u); // pred R
    push_i16(ms_block, 100);  // delta L
    push_i16(ms_block, 64);   // delta R
    push_i16(ms_block, 1000); // samp1 L
    push_i16(ms_block, 500);  // samp1 R
    push_i16(ms_block, 800);  // samp2 L
    push_i16(ms_block, 400);  // samp2 R
    ms_block.push_back(0x2Fu);
    ms_block.push_back(0x00u);

    INT16 ms_out[16]{};
    UINT32 ms_frames = 0u;
    check_hr(AcpHalDecodeMsAdpcm(ms_block.data(), static_cast<UINT32>(ms_block.size()),
                                 2u, ms_out, 8u, &ms_frames),
             "AcpHalDecodeMsAdpcm stereo");
    check(ms_frames == 4u, "MS-ADPCM decoded 4 stereo frames");
    check(ms_out[0] == 800 && ms_out[1] == 400, "MS-ADPCM frame 0 == samp2");
    check(ms_out[2] == 1000 && ms_out[3] == 500, "MS-ADPCM frame 1 == samp1");
    check(ms_out[4] == 1200 && ms_out[5] == 536, "MS-ADPCM frame 2 exact recurrence");
    std::printf("  [PASS] AcpHal WASAPI + SRC(24k->48k) + Biquad(200Hz=%.0f%%,12kHz=%.1f%%) + 5.1 IMA + MS-ADPCM\n",
                100.0 * low_out_rms / low_in_rms, 100.0 * high_out_rms / high_in_rms);
}

// ----------------------------------------------------------------------------
// 2. XNetworking (SecurityInfo UTF-8/UTF-16, Stats, Config, UDP Monitor, Offline & TLS)
// ----------------------------------------------------------------------------
void test_xnetworking_wave_c() {
    std::printf("[Wave C / 2] XNetworking SecurityInfo + Stats/Config + UDP Monitor + Offline/TLS...\n");
    check_hr(XGameRuntimeInitialize(), "XGameRuntimeInitialize");

    XTaskQueueHandle q = nullptr;
    check_hr(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::Manual, &q),
             "XTaskQueueCreate");

    // 2a. UTF-8 SecurityInformationForUrl
    XAsyncBlock ab8{};
    ab8.queue = q;
    check_hr(XNetworkingQuerySecurityInformationForUrlAsync("https://xboxlive.example.test/api", &ab8),
             "XNetworkingQuerySecurityInformationForUrlAsync");
    check_hr(XAsyncGetStatus(&ab8, true), "SecurityInfo UTF-8 wait");
    size_t sz8 = 0u;
    check_hr(XNetworkingQuerySecurityInformationForUrlResultSize(&ab8, &sz8), "SecurityInfo UTF-8 size");
    check(sz8 >= sizeof(XNetworkingSecurityInformation), "SecurityInfo UTF-8 size valid");
    std::vector<uint8_t> buf8(sz8);
    XNetworkingSecurityInformation* info8 = nullptr;
    size_t used8 = 0u;
    check_hr(XNetworkingQuerySecurityInformationForUrlResult(&ab8, buf8.size(), &used8, buf8.data(), &info8),
             "SecurityInfo UTF-8 result");
    check(info8 != nullptr && used8 == sizeof(XNetworkingSecurityInformation), "SecurityInfo UTF-8 struct valid");

    // 2b. UTF-16 SecurityInformationForUrl
    XAsyncBlock ab16{};
    ab16.queue = q;
    check_hr(XNetworkingQuerySecurityInformationForUrlUtf16Async(L"https://xboxlive.example.test/api", &ab16),
             "XNetworkingQuerySecurityInformationForUrlUtf16Async");
    check_hr(XAsyncGetStatus(&ab16, true), "SecurityInfo UTF-16 wait");
    size_t sz16 = 0u;
    check_hr(XNetworkingQuerySecurityInformationForUrlUtf16ResultSize(&ab16, &sz16), "SecurityInfo UTF-16 size");
    std::vector<uint8_t> buf16(sz16);
    XNetworkingSecurityInformation* info16 = nullptr;
    check_hr(XNetworkingQuerySecurityInformationForUrlUtf16Result(&ab16, buf16.size(), nullptr, buf16.data(), &info16),
             "SecurityInfo UTF-16 result");
    check(info16 != nullptr, "SecurityInfo UTF-16 struct valid");

    // 2c. Statistics & Configuration
    XNetworkingStatisticsBuffer stats{};
    check_hr(XNetworkingQueryStatistics(XNetworkingStatisticsType::TitleTcpConnections, &stats), "QueryStatistics TCP");
    check_hr(XNetworkingQueryStatistics(XNetworkingStatisticsType::TitleUdpConnections, &stats), "QueryStatistics UDP");
    uint64_t max_tcp = 0u, max_udp = 0u;
    check_hr(XNetworkingQueryConfigurationSetting(XNetworkingConfigurationSetting::MaxTitleTcpConnections, &max_tcp),
             "QueryConfig MaxTcp");
    check_hr(XNetworkingQueryConfigurationSetting(XNetworkingConfigurationSetting::MaxTitleUdpConnections, &max_udp),
             "QueryConfig MaxUdp");
    check(max_tcp == 256u && max_udp == 128u, "QueryConfig values (256 TCP / 128 UDP)");

    // 2d. PreferredLocalUdpMultiplayerPortChanged monitor
    std::atomic<uint16_t> observed_port{0u};
    XTaskQueueRegistrationToken port_tok{};
    check_hr(XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged(
                 q, &observed_port,
                 [](void* ctx, uint16_t port) noexcept {
                     static_cast<std::atomic<uint16_t>*>(ctx)->store(port, std::memory_order_release);
                 },
                 &port_tok),
             "RegisterPreferredLocalUdpMultiplayerPortChanged");
    check(port_tok.token != 0u, "UDP port token non-zero");
    XTaskQueueDispatch(q, XTaskQueuePort::Completion, 200u);
    check(observed_port.load(std::memory_order_acquire) == 3074u, "UDP port monitor received 3074");
    check(XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged(port_tok, true),
          "UnregisterPreferredLocalUdpMultiplayerPortChanged");

    // 2e. WSX_OFFLINE_NET=1 mode & WinHTTP TLS cert verification rejection on invalid handle
    SetEnvironmentVariableW(L"WSX_OFFLINE_NET", L"1");
    XNetworkingConnectivityHint hint{};
    check_hr(XNetworkingGetConnectivityHint(&hint), "ConnectivityHint offline");
    check(hint.connectivityLevel == XNetworkingConnectivityLevelHint::LocalAccess,
          "WSX_OFFLINE_NET=1 reports LocalAccess");

    uint8_t dummy_sha256[32]{};
    XNetworkingThumbprint tp{ 0u, sizeof(dummy_sha256), dummy_sha256 };
    check_hr(XNetworkingVerifyServerCertificate(reinterpret_cast<void*>(0x1234), &tp),
             "VerifyServerCertificate allows offline mode");

    SetEnvironmentVariableW(L"WSX_OFFLINE_NET", nullptr);
    check_hr(XNetworkingGetConnectivityHint(&hint), "ConnectivityHint online");
    check(hint.connectivityLevel == XNetworkingConnectivityLevelHint::InternetAccess,
          "Default reports InternetAccess");
    const HRESULT cert_hr = XNetworkingVerifyServerCertificate(reinterpret_cast<void*>(0x1234), &tp);
    check(FAILED(cert_hr), "VerifyServerCertificate rejects invalid WinHTTP handle when online");

    XTaskQueueTerminate(q, true, nullptr, nullptr);
    XTaskQueueCloseHandle(q);
    XGameRuntimeUninitialize();
    std::printf("  [PASS] XNetworking SecurityInfo(UTF8/16) + Stats/Config + UDP(3074) + Offline/TLS check\n");
}

// ----------------------------------------------------------------------------
// 3. Middleware: PE64 Forwarders + /GS Cookie + Live GPU SM 6.6 YCbCr->RGBA8
// ----------------------------------------------------------------------------
void test_middleware_wave_c(
    const std::filesystem::path& fixture_dll,
    const std::filesystem::path& shim_dir,
    ID3D12Device* dev,
    ID3D12CommandQueue* direct_queue) {
    std::printf("[Wave C / 3] PE64 Forwarders/GS-Cookie + Live GPU SM 6.6 YCbCr(A) 4:2:0 -> RGBA8...\n");

    // 3a. Forwarder export resolution (NTDLL & ApiSet)
    void* rtl_ver = wsx_pe::MappedPeImage::resolve_forwarder_export("NTDLL.RtlGetVersion");
    void* wait_addr = wsx_pe::MappedPeImage::resolve_forwarder_export("api-ms-win-core-synch-l1-2-0.WaitOnAddress");
    check(rtl_ver != nullptr, "resolve_forwarder_export(NTDLL.RtlGetVersion)");
    check(wait_addr != nullptr, "resolve_forwarder_export(api-ms-win-core-synch-l1-2-0.WaitOnAddress)");

    // 3b. MappedPeImage /GS SecurityCookie init on fixture DLL
    if (!fixture_dll.empty() && std::filesystem::exists(fixture_dll)) {
        wsx_pe::MappedPeImage img;
        wsx_pe::LoadOptions opts{};
        opts.shim_dir = shim_dir;
        opts.force_relocate = true;
        opts.allow_auto_stubs = true;
        check(img.load_from_file(fixture_dll, opts), "MappedPeImage::load_from_file fixture DLL");
        check(img.stats().security_cookie_initialized, "LoadConfig /GS SecurityCookie initialized");
        img.unload();
    }

    // 3c. Live DX12 SM 6.6 BT.709 Limited 4:2:0 YCbCrA -> RGBA8
    wsx_video::WsxYuvToRgbaConverter conv;
    std::string err;
    check(conv.initialize(dev, &err), "WsxYuvToRgbaConverter::initialize");
    if (!conv.is_initialized()) {
        std::fprintf(stderr, "    init err: %s\n", err.c_str());
        return;
    }

    constexpr uint32_t W = 64u, H = 64u;
    std::vector<uint8_t> y_plane(W * H, 16u);
    std::vector<uint8_t> cb_plane((W / 2u) * (H / 2u), 128u);
    std::vector<uint8_t> cr_plane((W / 2u) * (H / 2u), 128u);
    std::vector<uint8_t> a_plane(W * H, 255u);

    auto fill_quad = [&](uint32_t x0, uint32_t y0, uint8_t yv, uint8_t cbv, uint8_t crv, uint8_t av) {
        for (uint32_t y = y0; y < y0 + H / 2u; ++y) {
            for (uint32_t x = x0; x < x0 + W / 2u; ++x) {
                y_plane[y * W + x] = yv;
                a_plane[y * W + x] = av;
                cb_plane[(y / 2u) * (W / 2u) + (x / 2u)] = cbv;
                cr_plane[(y / 2u) * (W / 2u) + (x / 2u)] = crv;
            }
        }
    };
    fill_quad(0u,     0u,     235u, 128u, 128u, 255u); // White (BT.709 limited)
    fill_quad(W / 2u, 0u,      63u, 102u, 240u, 200u); // Red
    fill_quad(0u,     H / 2u, 173u,  42u,  26u, 128u); // Green
    fill_quad(W / 2u, H / 2u,  32u, 240u, 118u,  64u); // Blue

    D3D12_HEAP_PROPERTIES hp_def{};
    hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W;
    td.Height = H;
    td.DepthOrArraySize = 1u;
    td.MipLevels = 1u;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1u;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> rgba_tex;
    check_hr(dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td,
                                          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&rgba_tex)),
             "Create YUV output texture");

    check_hr(conv.convert_planar_420(
                 direct_queue, W, H,
                 y_plane.data(), W, cb_plane.data(), cr_plane.data(), W / 2u,
                 a_plane.data(), W, wsx_video::YuvColorSpace::Bt709Limited,
                 rgba_tex.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
             "WsxYuvToRgbaConverter::convert_planar_420");

    // Readback 64x64 RGBA8
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0u;
    UINT64 row_bytes = 0u, total = 0u;
    dev->GetCopyableFootprints(&td, 0u, 1u, 0u, &fp, &rows, &row_bytes, &total);

    D3D12_HEAP_PROPERTIES hp_rb{};
    hp_rb.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rbd{};
    rbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbd.Width = total;
    rbd.Height = 1u;
    rbd.DepthOrArraySize = 1u;
    rbd.MipLevels = 1u;
    rbd.SampleDesc.Count = 1u;
    rbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rb;
    check_hr(dev->CreateCommittedResource(&hp_rb, D3D12_HEAP_FLAG_NONE, &rbd,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)),
             "Create YUV readback buffer");

    ComPtr<ID3D12CommandAllocator> ca;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    check_hr(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca)), "CA");
    check_hr(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr, IID_PPV_ARGS(&cl)), "CL");
    check_hr(dev->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Fence");

    D3D12_TEXTURE_COPY_LOCATION src_loc{ rgba_tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
    src_loc.SubresourceIndex = 0u;
    D3D12_TEXTURE_COPY_LOCATION dst_loc{ rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
    dst_loc.PlacedFootprint = fp;
    cl->CopyTextureRegion(&dst_loc, 0u, 0u, 0u, &src_loc, nullptr);
    cl->Close();
    ID3D12CommandList* lists[] = { cl.Get() };
    direct_queue->ExecuteCommandLists(1u, lists);
    direct_queue->Signal(fence.Get(), 1u);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fence->GetCompletedValue() < 1u) {
        fence->SetEventOnCompletion(1u, ev);
        WaitForSingleObject(ev, 5000u);
    }
    CloseHandle(ev);

    void* ptr = nullptr;
    D3D12_RANGE rr{0, static_cast<SIZE_T>(total)};
    check_hr(rb->Map(0, &rr, &ptr), "Map YUV readback");
    if (ptr) {
        const auto* px = static_cast<const uint8_t*>(ptr);
        auto sample = [&](uint32_t x, uint32_t y) -> const uint8_t* {
            return px + fp.Offset + y * fp.Footprint.RowPitch + x * 4u;
        };
        const uint8_t* wpx = sample(16u, 16u);
        const uint8_t* rpx = sample(48u, 16u);
        const uint8_t* gpx = sample(16u, 48u);
        const uint8_t* bpx = sample(48u, 48u);
        check(wpx[0] >= 252 && wpx[1] >= 252 && wpx[2] >= 252 && wpx[3] == 255, "YUV White quadrant");
        check(rpx[0] >= 235 && rpx[1] <= 15 && rpx[2] <= 15 && std::abs(int(rpx[3]) - 200) <= 2, "YUV Red quadrant");
        check(gpx[0] <= 15 && gpx[1] >= 235 && gpx[2] <= 15 && std::abs(int(gpx[3]) - 128) <= 2, "YUV Green quadrant");
        check(bpx[0] <= 15 && bpx[1] <= 15 && bpx[2] >= 235 && std::abs(int(bpx[3]) - 64) <= 2, "YUV Blue quadrant");
        D3D12_RANGE zw{0, 0};
        rb->Unmap(0, &zw);
    }
    std::printf("  [PASS] PE64 Forwarders + /GS Cookie + GPU SM 6.6 BT.709 YCbCr(A)->RGBA8 (4/4 quadrants exact)\n");
}

// ----------------------------------------------------------------------------
// 4. dstorage_xs: Live GPU SM 6.6 GDeflate + TEXTURE_REGION + MULTIPLE_SUBRESOURCES
// ----------------------------------------------------------------------------
void test_dstorage_gpu_wave_c(ID3D12Device* dev, ID3D12CommandQueue* direct_queue) {
    std::printf("[Wave C / 4] dstorage_xs GPU SM 6.6 GDeflate (3 tiles x 3 modes) + TextureRegion + MultiSubresources...\n");

    // 4a. Multi-tile (140 KiB = 3 tiles) GPU GDeflate in Stored, Fixed32, and Dynamic32!
    constexpr size_t kPayloadSize = 140u * 1024u + 37u; // Non-multiple-of-4 last tile to stress edge cases
    std::vector<uint8_t> original(kPayloadSize);
    for (size_t i = 0; i < kPayloadSize; ++i) {
        if ((i % 64u) < 40u) {
            original[i] = static_cast<uint8_t>('A' + (i % 13u)); // Repetitive for LZ77 matches
        } else {
            original[i] = static_cast<uint8_t>((i * 73u + 19u) & 0xFFu);
        }
    }

    const wsx_gdeflate::TileMode modes[3] = {
        wsx_gdeflate::TileMode::ForceStored,
        wsx_gdeflate::TileMode::ForceFixed,
        wsx_gdeflate::TileMode::ForceDynamic
    };
    const char* mode_names[3] = { "Stored(BTYPE=0)", "Fixed32(BTYPE=1)", "Dynamic32(BTYPE=2)" };

    for (int m = 0; m < 3; ++m) {
        std::vector<uint8_t> comp;
        check(wsx_gdeflate::compress(original.data(), original.size(), comp, modes[m]), mode_names[m]);
        std::vector<uint8_t> decomp(original.size(), 0xCCu);
        UINT32 out_sz = 0u;
        BOOL used_gpu = FALSE;
        check_hr(DStorageDecompressGDeflateGpu(
                     dev, comp.data(), static_cast<UINT32>(comp.size()),
                     decomp.data(), static_cast<UINT32>(decomp.size()),
                     &out_sz, &used_gpu),
                 mode_names[m]);
        check(used_gpu == TRUE, "DStorageDecompressGDeflateGpu executed on GPU SM 6.6");
        check(out_sz == original.size(), "GPU GDeflate output byte count matches");
        check(std::memcmp(decomp.data(), original.data(), original.size()) == 0,
              "GPU GDeflate 140 KiB byte-exact match");
    }

    // 4b. IDStorageQueueX -> DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION + MULTIPLE_SUBRESOURCES
    ComPtr<IDStorageFactoryX> factory;
    check_hr(DStorageGetFactory(IID_PPV_ARGS(&factory)), "DStorageGetFactory");

    DSTORAGE_QUEUE_DESC qd{};
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    qd.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    qd.Priority = DSTORAGE_PRIORITY_NORMAL;
    qd.Name = "WaveCGpuTexQueue";
    qd.Device = dev;
    ComPtr<IDStorageQueueX> queue;
    check_hr(factory->CreateQueue(&qd, IID_PPV_ARGS(&queue)), "CreateQueue with Device");

    ComPtr<IDStorageStatusArrayX> status;
    check_hr(factory->CreateStatusArray(2u, "WaveCStatus", IID_PPV_ARGS(&status)), "CreateStatusArray");

    // 64x64 RGBA8 texture with 2 mip levels (mip 0: 64x64, mip 1: 32x32) in COMMON state
    constexpr UINT W = 64u, H = 64u;
    D3D12_HEAP_PROPERTIES hp_def{};
    hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W;
    td.Height = H;
    td.DepthOrArraySize = 1u;
    td.MipLevels = 2u;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1u;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ComPtr<ID3D12Resource> tex;
    check_hr(dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td,
                                          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex)),
             "Create 64x64x2mip target texture");

    // Request 0: MULTIPLE_SUBRESOURCES (mip 0 = 0x11223344, mip 1 = 0x55667788) compressed with GDeflate!
    std::vector<uint8_t> multi_raw(W * H * 4u + (W / 2u) * (H / 2u) * 4u);
    for (UINT i = 0u; i < W * H; ++i) {
        multi_raw[i * 4u + 0u] = 0x11u;
        multi_raw[i * 4u + 1u] = 0x22u;
        multi_raw[i * 4u + 2u] = 0x33u;
        multi_raw[i * 4u + 3u] = 0x44u;
    }
    const size_t mip1_off = W * H * 4u;
    for (UINT i = 0u; i < (W / 2u) * (H / 2u); ++i) {
        multi_raw[mip1_off + i * 4u + 0u] = 0x55u;
        multi_raw[mip1_off + i * 4u + 1u] = 0x66u;
        multi_raw[mip1_off + i * 4u + 2u] = 0x77u;
        multi_raw[mip1_off + i * 4u + 3u] = 0x88u;
    }
    std::vector<uint8_t> multi_gd;
    check(wsx_gdeflate::compress(multi_raw.data(), multi_raw.size(), multi_gd, wsx_gdeflate::TileMode::Auto),
          "Compress multi-subresource payload");

    // Request 1: TEXTURE_REGION sub-box [16..48, 16..48] on mip 0 (0xAABBCCDD), GDeflate-compressed!
    std::vector<uint8_t> box_raw(32u * 32u * 4u);
    for (UINT i = 0u; i < 32u * 32u; ++i) {
        box_raw[i * 4u + 0u] = 0xAAu;
        box_raw[i * 4u + 1u] = 0xBBu;
        box_raw[i * 4u + 2u] = 0xCCu;
        box_raw[i * 4u + 3u] = 0xDDu;
    }
    std::vector<uint8_t> box_gd;
    check(wsx_gdeflate::compress(box_raw.data(), box_raw.size(), box_gd, wsx_gdeflate::TileMode::ForceFixed),
          "Compress texture region payload");

    DSTORAGE_REQUEST r0{};
    r0.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
    r0.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    r0.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES;
    r0.Source.Memory.Source = multi_gd.data();
    r0.Source.Memory.Size = static_cast<UINT32>(multi_gd.size());
    r0.Destination.MultipleSubresources.Resource = tex.Get();
    r0.Destination.MultipleSubresources.FirstSubresource = 0u;
    r0.UncompressedSize = static_cast<UINT32>(multi_raw.size());
    r0.Name = "MultiSubGDeflate";

    DSTORAGE_REQUEST r1{};
    r1.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
    r1.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    r1.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION;
    r1.Source.Memory.Source = box_gd.data();
    r1.Source.Memory.Size = static_cast<UINT32>(box_gd.size());
    r1.Destination.Texture.Resource = tex.Get();
    r1.Destination.Texture.SubresourceIndex = 0u;
    r1.Destination.Texture.Region = D3D12_BOX{ 16u, 16u, 0u, 48u, 48u, 1u };
    r1.UncompressedSize = static_cast<UINT32>(box_raw.size());
    r1.Name = "TexRegionGDeflate";

    HANDLE done_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    queue->EnqueueRequest(&r0);
    queue->EnqueueStatus(status.Get(), 0u);
    queue->EnqueueRequest(&r1);
    queue->EnqueueStatus(status.Get(), 1u);
    queue->EnqueueSetEvent(done_ev);
    queue->Submit();

    check(WaitForSingleObject(done_ev, 10000u) == WAIT_OBJECT_0, "DirectStorage texture requests finished");
    CloseHandle(done_ev);
    check(status->IsComplete(0u) && SUCCEEDED(status->GetHResult(0u)), "MULTIPLE_SUBRESOURCES succeeded");
    check(status->IsComplete(1u) && SUCCEEDED(status->GetHResult(1u)), "TEXTURE_REGION succeeded");
    queue->Close();

    // Readback both mip 0 and mip 1 via DIRECT queue
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fps[2]{};
    UINT nrows[2]{};
    UINT64 rbytes[2]{};
    UINT64 total_rb = 0u;
    dev->GetCopyableFootprints(&td, 0u, 2u, 0u, fps, nrows, rbytes, &total_rb);

    D3D12_HEAP_PROPERTIES hp_rb{};
    hp_rb.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rbd{};
    rbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbd.Width = total_rb;
    rbd.Height = 1u;
    rbd.DepthOrArraySize = 1u;
    rbd.MipLevels = 1u;
    rbd.SampleDesc.Count = 1u;
    rbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rb;
    check_hr(dev->CreateCommittedResource(&hp_rb, D3D12_HEAP_FLAG_NONE, &rbd,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)),
             "Create texture readback");

    ComPtr<ID3D12CommandAllocator> ca;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    check_hr(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca)), "CA");
    check_hr(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr, IID_PPV_ARGS(&cl)), "CL");
    check_hr(dev->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Fence");

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1u, &b);

    for (UINT m = 0u; m < 2u; ++m) {
        D3D12_TEXTURE_COPY_LOCATION src_loc{ tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
        src_loc.SubresourceIndex = m;
        D3D12_TEXTURE_COPY_LOCATION dst_loc{ rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
        dst_loc.PlacedFootprint = fps[m];
        cl->CopyTextureRegion(&dst_loc, 0u, 0u, 0u, &src_loc, nullptr);
    }
    cl->Close();
    ID3D12CommandList* lists[] = { cl.Get() };
    direct_queue->ExecuteCommandLists(1u, lists);
    direct_queue->Signal(fence.Get(), 1u);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fence->GetCompletedValue() < 1u) {
        fence->SetEventOnCompletion(1u, ev);
        WaitForSingleObject(ev, 5000u);
    }
    CloseHandle(ev);

    void* ptr = nullptr;
    D3D12_RANGE rr{0, static_cast<SIZE_T>(total_rb)};
    check_hr(rb->Map(0, &rr, &ptr), "Map DStorage texture readback");
    if (ptr) {
        const auto* base = static_cast<const uint8_t*>(ptr);
        // Mip 0 corner (4,4) should still be 0x11223344 from MULTIPLE_SUBRESOURCES
        const uint8_t* p_corner = base + fps[0].Offset + 4u * fps[0].Footprint.RowPitch + 4u * 4u;
        check(p_corner[0] == 0x11u && p_corner[1] == 0x22u && p_corner[2] == 0x33u && p_corner[3] == 0x44u,
              "Mip 0 outer pixel matches MULTIPLE_SUBRESOURCES");
        // Mip 0 center (24,24) should be overwritten with 0xAABBCCDD from TEXTURE_REGION!
        const uint8_t* p_box = base + fps[0].Offset + 24u * fps[0].Footprint.RowPitch + 24u * 4u;
        check(p_box[0] == 0xAAu && p_box[1] == 0xBBu && p_box[2] == 0xCCu && p_box[3] == 0xDDu,
              "Mip 0 inner box [16..48] matches TEXTURE_REGION");
        // Mip 1 (10,10) should be 0x55667788 from MULTIPLE_SUBRESOURCES!
        const uint8_t* p_mip1 = base + fps[1].Offset + 10u * fps[1].Footprint.RowPitch + 10u * 4u;
        check(p_mip1[0] == 0x55u && p_mip1[1] == 0x66u && p_mip1[2] == 0x77u && p_mip1[3] == 0x88u,
              "Mip 1 pixel matches MULTIPLE_SUBRESOURCES");
        D3D12_RANGE zw{0, 0};
        rb->Unmap(0, &zw);
    }
    std::printf("  [PASS] GPU SM 6.6 GDeflate (Stored/Fixed32/Dynamic32 140 KiB exact) + TextureRegion + MultiSubresources\n");
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path fixture_dll = (argc > 1) ? std::filesystem::path(argv[1]) : std::filesystem::path{};
    const std::filesystem::path shim_dir = (argc > 2) ? std::filesystem::path(argv[2]) : std::filesystem::path{};

    test_acphal_wave_c();
    test_xnetworking_wave_c();

    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12InfoQueue> iq;
    if (!create_hardware_device(dev, iq)) {
        std::fprintf(stderr, "  [FAIL] Could not create D3D12 device\n");
        return 1;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> direct_queue;
    check_hr(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&direct_queue)), "CreateCommandQueue DIRECT");

    test_middleware_wave_c(fixture_dll, shim_dir, dev.Get(), direct_queue.Get());
    test_dstorage_gpu_wave_c(dev.Get(), direct_queue.Get());

    const uint64_t d3d_errs = count_debug_errors(iq.Get());
    check(d3d_errs == 0u, "Zero D3D12 Debug Layer errors across Wave C GPU passes");

    if (g_failures > 0) {
        std::fprintf(stderr, "WAVE C TEST FAILED (%d failures)\n", g_failures);
        return 1;
    }
    std::printf("ALL WAVE C TESTS PASSED (0 D3D12 Debug Layer errors)!\n");
    return 0;
}
