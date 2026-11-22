// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Wave 7 Verification Suite:
//  1. Clean-room Xbox BCPack Codec (BC1, BC3, BC4, BC5, BC7):
//     - Lossless round-trip compression & decompression (verifying real size reduction + bit-exact match)
//     - DirectStorage IDStorageQueueX async DSTORAGE_COMPRESSION_FORMAT_BCPACK upload into real D3D12 BC7 texture
//     - Resident mip-tail fallback upsampling (16x16 BC7 resident mip -> 64x64 target mip on corrupted stream)
//     - Neutral valid BCn fallback when no resident mip is supplied
//  2. Standalone wsx_gpu_check.exe execution + 408/408 CPU-vs-DXR Differential BVH + Markdown report check.

#include "include/xbox/dstorage_xs.h"
#include "include/xbox/d3d12_xs.h"
#include "wsx_bcpack.h"

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

std::vector<uint8_t> make_synthetic_bc_surface(DXGI_FORMAT fmt, uint32_t w, uint32_t h) {
    const auto kind = wsx_bcpack::classify_dxgi_bc_format(fmt);
    const uint32_t bsz = wsx_bcpack::block_bytes_for(kind);
    const uint32_t bw = (w + 3u) / 4u;
    const uint32_t bh = (h + 3u) / 4u;
    std::vector<uint8_t> out(static_cast<size_t>(bw) * bh * bsz, 0u);

    // Smooth gradient endpoints + structured indices (representative of real GPU textures)
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            uint8_t* b = out.data() + (static_cast<size_t>(by) * bw + bx) * bsz;
            if (kind == wsx_bcpack::BcKind::BC1) {
                const uint16_t c0 = static_cast<uint16_t>(((bx * 2u) & 31u) << 11 | ((by * 2u) & 63u) << 5 | 8u);
                const uint16_t c1 = static_cast<uint16_t>(c0 + 0x0841u);
                std::memcpy(b + 0, &c0, 2);
                std::memcpy(b + 2, &c1, 2);
                b[4] = 0x55u; b[5] = 0xAAu; b[6] = 0x55u; b[7] = 0xAAu;
            } else if (kind == wsx_bcpack::BcKind::BC4) {
                b[0] = static_cast<uint8_t>(40u + bx * 4u);
                b[1] = static_cast<uint8_t>(200u - by * 3u);
                for (int k = 2; k < 8; ++k) b[k] = static_cast<uint8_t>(0x24u);
            } else if (kind == wsx_bcpack::BcKind::BC3) {
                b[0] = 250u; b[1] = 200u;
                for (int k = 2; k < 8; ++k) b[k] = 0x12u;
                const uint16_t c0 = static_cast<uint16_t>(0x3186u + bx);
                const uint16_t c1 = static_cast<uint16_t>(0x630Cu + by);
                std::memcpy(b + 8, &c0, 2);
                std::memcpy(b + 10, &c1, 2);
                b[12] = 0x33u; b[13] = 0xCCu; b[14] = 0x33u; b[15] = 0xCCu;
            } else if (kind == wsx_bcpack::BcKind::BC5) {
                b[0] = static_cast<uint8_t>(128u + bx); b[1] = static_cast<uint8_t>(120u + bx);
                b[8] = static_cast<uint8_t>(128u + by); b[9] = static_cast<uint8_t>(120u + by);
                for (int k = 2; k < 8; ++k) { b[k] = 0x49u; b[8 + k] = 0x92u; }
            } else if (kind == wsx_bcpack::BcKind::BC7) {
                // Mode 6 smooth RGBA blocks
                b[0] = 0x40u;
                b[1] = static_cast<uint8_t>(0x40u + (bx & 0x3Fu));
                b[2] = static_cast<uint8_t>(0x50u + (by & 0x3Fu));
                b[3] = 0x70u; b[4] = 0x60u; b[5] = 0x70u; b[6] = 0xFEu; b[7] = 0x7Fu;
                for (int k = 8; k < 16; ++k) b[k] = static_cast<uint8_t>((k & 1) ? 0x22u : 0x44u);
            }
        }
    }
    return out;
}

void test_bcpack_codecs_and_dstorage() {
    std::printf("\n=== [Wave 7.1] BCPack Codec (BC1/BC3/BC4/BC5/BC7) + DirectStorage + Mip Fallback ===\n");

    const struct { DXGI_FORMAT fmt; const char* name; } cases[] = {
        {DXGI_FORMAT_BC1_UNORM, "BC1"},
        {DXGI_FORMAT_BC3_UNORM, "BC3"},
        {DXGI_FORMAT_BC4_UNORM, "BC4"},
        {DXGI_FORMAT_BC5_UNORM, "BC5"},
        {DXGI_FORMAT_BC7_UNORM, "BC7"},
    };

    for (const auto& tc : cases) {
        const auto raw = make_synthetic_bc_surface(tc.fmt, 128u, 128u);
        std::vector<uint8_t> packed(raw.size() + 1024u);
        UINT32 packed_sz = 0u;
        HRESULT hr = DStorageCompressBCPack(
            raw.data(), static_cast<UINT32>(raw.size()), tc.fmt,
            packed.data(), static_cast<UINT32>(packed.size()), &packed_sz);
        CHECK(SUCCEEDED(hr) && packed_sz > 0u && packed_sz < raw.size(),
              "BCPack %s compressed %zu -> %u bytes (%.2fx ratio)",
              tc.name, raw.size(), packed_sz, static_cast<double>(raw.size()) / packed_sz);

        std::vector<uint8_t> restored(raw.size(), 0xEEu);
        UINT32 out_sz = 0u;
        BOOL used_fb = TRUE;
        hr = DStorageDecompressBCPack(
            packed.data(), packed_sz, tc.fmt,
            restored.data(), static_cast<UINT32>(restored.size()), &out_sz,
            nullptr, 0u, 0u, 128u, 128u, &used_fb);
        CHECK(SUCCEEDED(hr) && out_sz == raw.size() && used_fb == FALSE &&
              std::memcmp(raw.data(), restored.data(), raw.size()) == 0,
              "BCPack %s lossless bit-exact decode verified (%u bytes)", tc.name, out_sz);
    }

    // Resident mip-tail fallback: feed corrupted/truncated BC7 stream with a real 16x16 resident mip
    {
        const auto mip16 = make_synthetic_bc_surface(DXGI_FORMAT_BC7_UNORM, 16u, 16u);
        const uint8_t corrupt[12] = {0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4, 5, 6, 7, 8};
        std::vector<uint8_t> target64(16u * 16u * 16u, 0u); // 64x64 BC7 = 16x16 blocks * 16B = 4096 B
        UINT32 out_sz = 0u;
        BOOL used_fb = FALSE;
        const HRESULT hr = DStorageDecompressBCPack(
            corrupt, sizeof(corrupt), DXGI_FORMAT_BC7_UNORM,
            target64.data(), static_cast<UINT32>(target64.size()), &out_sz,
            mip16.data(), 16u, 16u, 64u, 64u, &used_fb);
        CHECK(SUCCEEDED(hr) && used_fb == TRUE && out_sz == target64.size(),
              "BC7 resident mip-tail fallback reconstructed 64x64 (%u bytes) from 16x16 mip", out_sz);
        // Block (0,0) and (3,3) of 64x64 must match block (0,0) of 16x16 resident mip!
        CHECK(std::memcmp(target64.data(), mip16.data(), 16u) == 0 &&
              std::memcmp(target64.data() + (3u * 16u + 3u) * 16u, mip16.data(), 16u) == 0,
              "Upsampled 64x64 BC7 blocks match resident 16x16 mip-tail");
    }

    // Async DirectStorage Queue BCPack upload directly into a real D3D12 BC7 GPU texture!
    {
        ComPtr<ID3D12Device8> dev;
        D3D12XBOX_CREATE_DEVICE_PARAMETERS cdp{};
        cdp.Version = D3D12_SDK_VERSION;
        CHECK(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &cdp, IID_PPV_ARGS(&dev))) && dev != nullptr,
              "Created d3d12_xs device for DirectStorage BCPack GPU upload");

        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 64;
        td.Height = 64;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_BC7_UNORM;
        td.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> bc7_tex;
        CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&bc7_tex))),
              "Created 64x64 BC7_UNORM GPU texture");

        const auto raw64 = make_synthetic_bc_surface(DXGI_FORMAT_BC7_UNORM, 64u, 64u);
        std::vector<uint8_t> packed64(raw64.size() + 512u);
        UINT32 packed_sz = 0u;
        DStorageCompressBCPack(raw64.data(), static_cast<UINT32>(raw64.size()),
                               DXGI_FORMAT_BC7_UNORM, packed64.data(),
                               static_cast<UINT32>(packed64.size()), &packed_sz);

        ComPtr<IDStorageFactoryX> factory;
        ComPtr<IDStorageQueueX> queue;
        ComPtr<IDStorageStatusArrayX> status;
        CHECK(SUCCEEDED(DStorageGetFactory(IID_PPV_ARGS(&factory))), "DStorageGetFactory succeeded");
        DSTORAGE_QUEUE_DESC qd{};
        qd.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        qd.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
        qd.Priority = DSTORAGE_PRIORITY_NORMAL;
        qd.Name = "Wave7BcPackQueue";
        qd.Device = dev.Get();
        CHECK(SUCCEEDED(factory->CreateQueue(&qd, IID_PPV_ARGS(&queue))), "Created DirectStorage BCPack queue");
        CHECK(SUCCEEDED(factory->CreateStatusArray(1, "bc7_status", IID_PPV_ARGS(&status))), "Created status array");

        DSTORAGE_REQUEST req{};
        req.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_BCPACK;
        req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        req.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION;
        req.Source.Memory.Source = packed64.data();
        req.Source.Memory.Size = packed_sz;
        req.Destination.Texture.Resource = bc7_tex.Get();
        req.Destination.Texture.SubresourceIndex = 0;
        req.Destination.Texture.Region = {0, 0, 0, 64, 64, 1};
        req.UncompressedSize = static_cast<UINT32>(raw64.size());
        req.Name = "BC7_StreamMip0";

        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        queue->EnqueueRequest(&req);
        queue->EnqueueStatus(status.Get(), 0);
        queue->EnqueueSetEvent(ev);
        queue->Submit();
        const DWORD wait_rc = WaitForSingleObject(ev, 5000);
        CloseHandle(ev);
        CHECK(wait_rc == WAIT_OBJECT_0 && status->IsComplete(0) && SUCCEEDED(status->GetHResult(0)),
              "DirectStorage async BCPack -> BC7 D3D12 Texture2D upload succeeded (hr=0x%08X)",
              static_cast<unsigned>(status->GetHResult(0)));
        queue->Close();
    }
}

void test_gpu_check_408_dxr(const std::filesystem::path& gpu_check_exe) {
    std::printf("\n=== [Wave 7.2] Standalone wsx_gpu_check.exe + 408-Ray CPU-vs-DXR Differential BVH ===\n");
    const std::filesystem::path rep_path = std::filesystem::temp_directory_path() / "wsx_wave7_gpu_report.md";
    std::error_code ec;
    std::filesystem::remove(rep_path, ec);

    std::wstring cmd = L"\"" + gpu_check_exe.wstring() + L"\" --report \"" + rep_path.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd_mut(cmd.begin(), cmd.end());
    cmd_mut.push_back(L'\0');
    const BOOL ok = CreateProcessW(nullptr, cmd_mut.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    CHECK(ok != FALSE, "Launched wsx_gpu_check.exe");
    if (!ok) return;

    WaitForSingleObject(pi.hProcess, 30000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    CHECK(exit_code == 0u, "wsx_gpu_check.exe exited cleanly (code=%lu)", static_cast<unsigned long>(exit_code));

    std::ifstream f(rep_path);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(!content.empty() && content.find("408 / 408") != std::string::npos,
          "Markdown report contains verified 408 / 408 CPU-vs-DXR Differential BVH score");
    CHECK(content.find("Bit-Exact 64-lane") != std::string::npos,
          "Markdown report confirms Bit-Exact 64-lane Wave64 emulation");
    std::filesystem::remove(rep_path, ec);
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <wsx_gpu_check.exe>\n", argv[0]);
        return 2;
    }

    test_bcpack_codecs_and_dstorage();
    test_gpu_check_408_dxr(argv[1]);

    std::printf("\n=== Wave 7 Summary: failures=%d ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
