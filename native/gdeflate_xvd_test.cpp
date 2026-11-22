// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// CTest #26: Clean-room GDeflate v1.0 (32-stream tiled deflate) + Unencrypted .xvd
// container streaming through dstorage_xs.dll + real ID3D12Fence verification.

#include "include/xbox/dstorage_xs.h"
#include "wsx_gdeflate.h"
#include "wsx_xvd.h"

#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

bool fail(const char* msg, HRESULT hr = S_OK) {
    if (hr != S_OK) {
        std::fprintf(stderr, "FAIL: %s (hr=0x%08lX)\n", msg, static_cast<unsigned long>(hr));
    } else {
        std::fprintf(stderr, "FAIL: %s\n", msg);
    }
    return false;
}

std::vector<uint8_t> make_structured_payload(size_t size) {
    std::vector<uint8_t> buf(size, 0u);
    static const char* kBlock = "MizuBox-Scarlett-GDeflate-TileStream-RDNA2-SM66-";
    const size_t blen = std::strlen(kBlock);
    for (size_t i = 0; i < size; ++i) {
        if ((i & 0x3FFu) < 64u) {
            // Small pseudo-random high-entropy pockets mixed with repetitive asset headers
            buf[i] = static_cast<uint8_t>((i * 73u + (i >> 5) * 29u + 17u) & 0xFFu);
        } else {
            buf[i] = static_cast<uint8_t>(kBlock[(i + (i / 4096u)) % blen]);
        }
    }
    return buf;
}

bool test_gdeflate_modes() {
    const std::vector<uint8_t> orig = make_structured_payload(160000u); // 3 tiles: 65536 + 65536 + 28928

    const wsx_gdeflate::TileMode modes[] = {
        wsx_gdeflate::TileMode::ForceStored,
        wsx_gdeflate::TileMode::ForceFixed,
        wsx_gdeflate::TileMode::ForceDynamic,
        wsx_gdeflate::TileMode::Auto,
    };
    const char* names[] = {"Stored", "Fixed32", "Dynamic32", "Auto"};

    for (size_t m = 0; m < 4u; ++m) {
        std::vector<uint8_t> comp;
        if (!wsx_gdeflate::compress(orig.data(), orig.size(), comp, modes[m])) {
            return fail("wsx_gdeflate::compress failed");
        }
        if (comp.size() < 12u || comp[0] != 0x04u || comp[1] != 0xFBu) {
            return fail("GDeflate header magic mismatch");
        }

        std::vector<uint8_t> dec(orig.size(), 0u);
        UINT32 out_sz = 0;
        const HRESULT hr = DStorageDecompressGDeflate(
            comp.data(), static_cast<UINT32>(comp.size()),
            dec.data(), static_cast<UINT32>(dec.size()),
            &out_sz);
        if (FAILED(hr)) return fail("DStorageDecompressGDeflate failed", hr);
        if (out_sz != orig.size()) return fail("DStorageDecompressGDeflate size mismatch");
        if (std::memcmp(dec.data(), orig.data(), orig.size()) != 0) {
            return fail("GDeflate decompressed bytes mismatch");
        }
        if (modes[m] == wsx_gdeflate::TileMode::Auto && comp.size() >= orig.size() / 2u) {
            return fail("GDeflate Auto failed to achieve <50% ratio on structured payload");
        }
        std::printf("  [GDeflate:%s] orig=%zu comp=%zu ratio=%.1f%% OK\n",
                    names[m], orig.size(), comp.size(),
                    100.0 * static_cast<double>(comp.size()) / static_cast<double>(orig.size()));
    }
    return true;
}

bool test_xvd_and_dstorage_pipeline() {
    const std::vector<uint8_t> orig = make_structured_payload(160000u);
    std::vector<uint8_t> gdef;
    if (!wsx_gdeflate::compress(orig.data(), orig.size(), gdef, wsx_gdeflate::TileMode::Auto)) {
        return fail("compress for XVD payload failed");
    }

    // Build an unencrypted XVD with integrity hash-tree padding enabled (exercises real offset math)
    // and embed a 256-byte prefix before the GDeflate stream inside user data.
    std::vector<uint8_t> user_data(256u + gdef.size(), 0x5Au);
    std::memcpy(user_data.data() + 256u, gdef.data(), gdef.size());

    const std::vector<uint8_t> plain_xvd = wsx_xvd::build_synthetic_xvd(
        user_data.data(), user_data.size(), false /*mark_encrypted*/, true /*include_integrity_padding*/);
    const std::vector<uint8_t> enc_xvd = wsx_xvd::build_synthetic_xvd(
        user_data.data(), user_data.size(), true /*mark_encrypted*/, true);

    const fs::path temp_dir = fs::temp_directory_path() / L"wsx_gdeflate_xvd_test";
    std::error_code ec;
    fs::create_directories(temp_dir, ec);
    const fs::path plain_path = temp_dir / L"dev_unencrypted.xvd";
    const fs::path enc_path = temp_dir / L"retail_encrypted.xvc";

    {
        std::ofstream f(plain_path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(plain_xvd.data()), static_cast<std::streamsize>(plain_xvd.size()));
    }
    {
        std::ofstream f(enc_path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(enc_xvd.data()), static_cast<std::streamsize>(enc_xvd.size()));
    }

    // 1. Verify encrypted XVC is cleanly refused (no DRM bypass!)
    {
        ComPtr<IDStorageFileX> refused;
        const HRESULT hr = DStorageOpenXvdVolume(enc_path.c_str(), IID_PPV_ARGS(&refused));
        if (hr != HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) || refused != nullptr) {
            return fail("Encrypted XVC was not cleanly rejected with ERROR_ACCESS_DENIED", hr);
        }
        std::printf("  [XVD:Encrypted] cleanly rejected with 0x%08lX (ERROR_ACCESS_DENIED)\n",
                    static_cast<unsigned long>(hr));
    }

    // 2. Open unencrypted XVD and stream GDeflate payload through real IDStorageQueueX + ID3D12Fence
    ComPtr<IDStorageFileX> xvd_file;
    HRESULT hr = DStorageOpenXvdVolume(plain_path.c_str(), IID_PPV_ARGS(&xvd_file));
    if (FAILED(hr) || !xvd_file) return fail("DStorageOpenXvdVolume failed on unencrypted XVD", hr);

    BY_HANDLE_FILE_INFORMATION fi{};
    hr = xvd_file->GetFileInformation(&fi);
    if (FAILED(hr)) return fail("XVD GetFileInformation failed", hr);
    const uint64_t reported_user_sz =
        static_cast<uint64_t>(fi.nFileSizeLow) | (static_cast<uint64_t>(fi.nFileSizeHigh) << 32);
    if (reported_user_sz < user_data.size()) {
        return fail("XVD reported user data size too small");
    }

    ComPtr<ID3D12Device> device;
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) return fail("D3D12CreateDevice failed", hr);

    ComPtr<ID3D12Fence> fence;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) return fail("CreateFence failed", hr);

    ComPtr<IDStorageFactoryX> factory;
    hr = DStorageGetFactory(IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) return fail("DStorageGetFactory failed", hr);

    DSTORAGE_QUEUE_DESC qdesc{};
    qdesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qdesc.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    qdesc.Priority = DSTORAGE_PRIORITY_NORMAL;
    qdesc.Name = "gdeflate_xvd_queue";
    qdesc.Device = device.Get();

    ComPtr<IDStorageQueueX> queue;
    hr = factory->CreateQueue(&qdesc, IID_PPV_ARGS(&queue));
    if (FAILED(hr) || !queue) return fail("CreateQueue failed", hr);

    ComPtr<IDStorageStatusArrayX> status;
    hr = factory->CreateStatusArray(2, "gdeflate_xvd_status", IID_PPV_ARGS(&status));
    if (FAILED(hr) || !status) return fail("CreateStatusArray failed", hr);

    std::vector<uint8_t> streamed_out(orig.size(), 0u);
    DSTORAGE_REQUEST req{};
    req.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
    req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    req.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    req.Source.File.Source = xvd_file.Get();
    req.Source.File.Offset = 256u;
    req.Source.File.Size = static_cast<UINT32>(gdef.size());
    req.Destination.Memory.Buffer = streamed_out.data();
    req.Destination.Memory.Size = static_cast<UINT32>(streamed_out.size());
    req.UncompressedSize = static_cast<UINT32>(orig.size());
    req.Name = "xvd_gdeflate_stream";

    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    queue->EnqueueRequest(&req);
    queue->EnqueueStatus(status.Get(), 0);
    queue->EnqueueSignal(fence.Get(), 77u);
    queue->EnqueueSetEvent(ev);
    queue->Submit();

    const DWORD wait_rc = WaitForSingleObject(ev, 5000);
    CloseHandle(ev);
    if (wait_rc != WAIT_OBJECT_0) return fail("DirectStorage queue timed out");

    if (!status->IsComplete(0) || FAILED(status->GetHResult(0))) {
        return fail("DirectStorage XVD+GDeflate request failed", status->GetHResult(0));
    }
    if (fence->GetCompletedValue() != 77u) {
        return fail("ID3D12Fence did not reach expected value 77");
    }
    if (std::memcmp(streamed_out.data(), orig.data(), orig.size()) != 0) {
        return fail("Streamed XVD+GDeflate output mismatch");
    }

    queue->Close();
    xvd_file->Close();
    fs::remove_all(temp_dir, ec);

    std::printf("  [XVD+DStorage+GDeflate] user_size=%llu streamed=%zu fence=%llu OK\n",
                static_cast<unsigned long long>(reported_user_sz),
                streamed_out.size(),
                static_cast<unsigned long long>(fence->GetCompletedValue()));
    return true;
}

} // namespace

int main() {
    std::printf("[gdeflate_xvd_test] Running GDeflate v1.0 & Unencrypted XVD tests...\n");
    if (!test_gdeflate_modes()) return 1;
    if (!test_xvd_and_dstorage_pipeline()) return 2;
    std::printf("PASS: gdeflate_xvd_test (4 GDeflate modes + unencrypted XVD stream + encrypted XVC rejection)\n");
    return 0;
}
