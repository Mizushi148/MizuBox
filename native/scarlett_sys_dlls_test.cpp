// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Integration CTest for Part 2 clean-room Xbox Series X|S system DLLs:
// - xg_xs.dll     (LINEAR / 4KB_S / 64KB_S / BC7 swizzle <-> deswizzle round-trip)
// - dstorage_xs.dll (async file + memory + zlib inflate + CancelRequestsWithTag + real ID3D12Fence)
// - AcpHal.dll    (ApuCreateHeap / ApuAlloc / ApuMapVirtualAddress / ApuIsVirtualAddressValid / IAcpHal / SHAPE)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include "include/xbox/xg_xs.h"
#include "include/xbox/dstorage_xs.h"
#include "include/xbox/acphal.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                                   \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "[scarlett_sys_dlls] FAIL (%s:%d): %s\n",                 \
                         __FILE__, __LINE__, (msg));                                       \
            ++g_failures;                                                                  \
        }                                                                                  \
    } while (0)

std::vector<uint8_t> make_zlib_stored_block(const uint8_t* data, uint16_t len) {
    // Valid RFC 1950 (zlib) stream wrapping an RFC 1951 stored block (BFINAL=1, BTYPE=00)
    std::vector<uint8_t> out;
    out.push_back(0x78u); // CMF = deflate, 32K window
    out.push_back(0x01u); // FLG = no dict, check bits so (0x7801 % 31 == 0)
    out.push_back(0x01u); // BFINAL=1, BTYPE=00
    out.push_back(static_cast<uint8_t>(len & 0xFFu));
    out.push_back(static_cast<uint8_t>((len >> 8) & 0xFFu));
    const uint16_t nlen = static_cast<uint16_t>(~len);
    out.push_back(static_cast<uint8_t>(nlen & 0xFFu));
    out.push_back(static_cast<uint8_t>((nlen >> 8) & 0xFFu));
    out.insert(out.end(), data, data + len);

    // Adler-32 trailer
    uint32_t s1 = 1u;
    uint32_t s2 = 0u;
    for (uint16_t i = 0; i < len; ++i) {
        s1 = (s1 + data[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    const uint32_t adler = (s2 << 16) | s1;
    out.push_back(static_cast<uint8_t>((adler >> 24) & 0xFFu));
    out.push_back(static_cast<uint8_t>((adler >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((adler >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(adler & 0xFFu));
    return out;
}

void test_xg_xs() {
    CHECK(XGGetFormatBitsPerPixel(DXGI_FORMAT_R8G8B8A8_UNORM) == 32u, "RGBA8 bpp == 32");
    CHECK(XGGetFormatBitsPerPixel(DXGI_FORMAT_BC7_UNORM) == 8u, "BC7 bpp == 8");
    CHECK(XGIsBlockCompressedFormat(DXGI_FORMAT_BC7_UNORM) == TRUE, "BC7 is block compressed");
    CHECK(XGIsBlockCompressedFormat(DXGI_FORMAT_R8G8B8A8_UNORM) == FALSE, "RGBA8 not block compressed");
    CHECK(XGComputeOptimalSwizzleMode(3, DXGI_FORMAT_R8G8B8A8_UNORM, 256, 256, 1, 0) == XG_SWIZZLE_MODE_64KB_S,
          "Optimal swizzle for 256x256 RGBA8 is 64KB_S");
    CHECK(std::strcmp(XGGetSwizzleModeString(XG_SWIZZLE_MODE_4KB_S), "4KB_S") == 0, "Swizzle name 4KB_S");

    auto run_2d_roundtrip = [](UINT w, UINT h, DXGI_FORMAT fmt, UINT swizzle, bool expect_different) {
        XG_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 2;
        desc.ArraySize = 1;
        desc.Format = fmt;
        desc.SampleCount = 1;
        desc.SwizzleMode = swizzle;

        ComPtr<IXGTextureAddressComputer> comp;
        const HRESULT hr = XGCreateTexture2DComputer(&desc, reinterpret_cast<void**>(comp.GetAddressOf()));
        CHECK(SUCCEEDED(hr) && comp, "XGCreateTexture2DComputer succeeded");
        if (!comp) return;

        XG_RESOURCE_LAYOUT layout{};
        CHECK(SUCCEEDED(comp->GetResourceLayout(&layout)), "GetResourceLayout succeeded");
        CHECK(layout.SizeBytes == comp->GetResourceSizeBytes(), "SizeBytes match");
        CHECK(layout.BaseAlignmentBytes == comp->GetResourceBaseAlignmentBytes(), "Alignment match");
        CHECK(layout.SizeBytes > 0, "Layout size > 0");
        if (swizzle == XG_SWIZZLE_MODE_64KB_S) {
            CHECK(layout.BaseAlignmentBytes >= 65536u, "64KB_S alignment >= 64KB");
        } else if (swizzle == XG_SWIZZLE_MODE_4KB_S) {
            CHECK(layout.BaseAlignmentBytes >= 4096u, "4KB_S alignment >= 4KB");
        }

        const bool is_bc = XGIsBlockCompressedFormat(fmt) != FALSE;
        const UINT row_elems = is_bc ? ((w + 3u) / 4u) : w;
        const UINT num_rows = is_bc ? ((h + 3u) / 4u) : h;
        const UINT elem_bytes = layout.Plane[0].BytesPerElement;
        const UINT linear_pitch = row_elems * elem_bytes;
        const size_t linear_bytes = static_cast<size_t>(linear_pitch) * num_rows;

        std::vector<uint8_t> src(linear_bytes);
        for (size_t i = 0; i < src.size(); ++i) {
            src[i] = static_cast<uint8_t>((i * 37u + 13u) & 0xFFu);
        }
        std::vector<uint8_t> tiled(static_cast<size_t>(layout.SizeBytes), 0xCDu);
        std::vector<uint8_t> dst(linear_bytes, 0x00u);

        CHECK(SUCCEEDED(comp->CopyIntoSubresource(
                  tiled.data(), 0, 0, src.data(), linear_pitch, static_cast<UINT>(linear_bytes))),
              "CopyIntoSubresource mip 0");
        if (expect_different) {
            const bool differs = std::memcmp(tiled.data(), src.data(), linear_bytes) != 0;
            CHECK(differs, "Swizzled memory differs from linear order");
        }
        CHECK(SUCCEEDED(comp->CopyFromSubresource(
                  dst.data(), linear_pitch, static_cast<UINT>(linear_bytes), tiled.data(), 0, 0)),
              "CopyFromSubresource mip 0");
        CHECK(std::memcmp(src.data(), dst.data(), linear_bytes) == 0,
              "2D swizzle <-> deswizzle bit-exact roundtrip");
    };

    run_2d_roundtrip(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, XG_SWIZZLE_MODE_LINEAR, false);
    run_2d_roundtrip(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, XG_SWIZZLE_MODE_4KB_S, true);
    run_2d_roundtrip(128, 128, DXGI_FORMAT_R8G8B8A8_UNORM, XG_SWIZZLE_MODE_64KB_S, true);
    run_2d_roundtrip(64, 64, DXGI_FORMAT_BC7_UNORM, XG_SWIZZLE_MODE_4KB_S, true);

    XG_BUFFER_DESC bdesc{65536u, 0u};
    ComPtr<IXGTextureAddressComputer> bcomp;
    CHECK(SUCCEEDED(XGCreateBufferComputer(&bdesc, reinterpret_cast<void**>(bcomp.GetAddressOf()))),
          "XGCreateBufferComputer succeeded");
    if (bcomp) {
        CHECK(bcomp->GetResourceSizeBytes() == 65536u, "Buffer computer size exact");
    }

    std::printf("[scarlett_sys_dlls] xg_xs.dll OK (LINEAR/4KB_S/64KB_S/BC7 round-trips verified)\n");
}

void test_dstorage_xs() {
    DSTORAGE_CONFIGURATION1 cfg{};
    cfg.NumSubmitThreads = 1;
    cfg.NumBuiltInCpuDecompressionThreads = 1;
    CHECK(SUCCEEDED(DStorageSetConfiguration1(&cfg)), "DStorageSetConfiguration1");

    ComPtr<IDStorageFactoryX> factory;
    CHECK(SUCCEEDED(DStorageGetFactory(IID_PPV_ARGS(&factory))) && factory, "DStorageGetFactory");
    if (!factory) return;

    // Prepare a real temp file on disk
    wchar_t temp_dir[MAX_PATH]{};
    wchar_t temp_file[MAX_PATH]{};
    CHECK(GetTempPathW(MAX_PATH, temp_dir) > 0, "GetTempPathW");
    CHECK(GetTempFileNameW(temp_dir, L"wsx", 0, temp_file) != 0, "GetTempFileNameW");

    std::vector<uint8_t> disk_payload(4096);
    for (size_t i = 0; i < disk_payload.size(); ++i) {
        disk_payload[i] = static_cast<uint8_t>((i * 19u + 7u) & 0xFFu);
    }
    {
        HANDLE wf = CreateFileW(temp_file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(wf != INVALID_HANDLE_VALUE, "Create temp test file");
        DWORD written = 0;
        if (wf != INVALID_HANDLE_VALUE) {
            CHECK(WriteFile(wf, disk_payload.data(), static_cast<DWORD>(disk_payload.size()), &written, nullptr) &&
                      written == disk_payload.size(),
                  "Write temp test file");
            CloseHandle(wf);
        }
    }

    ComPtr<IDStorageFileX> ds_file;
    CHECK(SUCCEEDED(factory->OpenFile(temp_file, IID_PPV_ARGS(&ds_file))) && ds_file, "OpenFile");

    BY_HANDLE_FILE_INFORMATION fi{};
    if (ds_file) {
        CHECK(SUCCEEDED(ds_file->GetFileInformation(&fi)) && fi.nFileSizeLow == 4096u,
              "GetFileInformation size == 4096");
    }

    // Create a real D3D12 device + fence to test EnqueueSignal end-to-end
    ComPtr<ID3D12Device> device;
    HRESULT dhr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    CHECK(SUCCEEDED(dhr) && device, "D3D12CreateDevice for ID3D12Fence");
    ComPtr<ID3D12Fence> fence;
    if (device) {
        CHECK(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && fence,
              "CreateFence");
    }

    DSTORAGE_QUEUE_DESC qdesc{};
    qdesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qdesc.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    qdesc.Priority = DSTORAGE_PRIORITY_NORMAL;
    qdesc.Name = "wsx_test_queue";
    qdesc.Device = device.Get();

    ComPtr<IDStorageQueueX> queue;
    CHECK(SUCCEEDED(factory->CreateQueue(&qdesc, IID_PPV_ARGS(&queue))) && queue, "CreateQueue");

    ComPtr<IDStorageStatusArrayX> status;
    CHECK(SUCCEEDED(factory->CreateStatusArray(4, "wsx_status", IID_PPV_ARGS(&status))) && status,
          "CreateStatusArray");

    if (queue && status && ds_file && fence) {
        std::vector<uint8_t> dst_file(1024, 0);
        std::vector<uint8_t> src_mem(512);
        for (size_t i = 0; i < src_mem.size(); ++i) src_mem[i] = static_cast<uint8_t>(0xA0u ^ (i & 0xFFu));
        std::vector<uint8_t> dst_mem(512, 0);

        const char* kZlibText = "MizuBox Clean-Room DirectStorage Zlib Payload 2026!";
        const uint16_t kZlibLen = static_cast<uint16_t>(std::strlen(kZlibText));
        const std::vector<uint8_t> zlib_blob = make_zlib_stored_block(
            reinterpret_cast<const uint8_t*>(kZlibText), kZlibLen);
        std::vector<uint8_t> dst_zlib(kZlibLen, 0);

        std::vector<uint8_t> dst_cancelled(128, 0x5Au);

        // 1. File-to-memory
        DSTORAGE_REQUEST r1{};
        r1.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        r1.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        r1.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
        r1.Source.File.Source = ds_file.Get();
        r1.Source.File.Offset = 512;
        r1.Source.File.Size = 1024;
        r1.Destination.Memory.Buffer = dst_file.data();
        r1.Destination.Memory.Size = 1024;
        r1.UncompressedSize = 1024;
        r1.CancellationTag = 0x11110001ull;
        r1.Name = "file_read";
        queue->EnqueueRequest(&r1);

        // 2. Memory-to-memory
        DSTORAGE_REQUEST r2{};
        r2.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        r2.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        r2.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
        r2.Source.Memory.Source = src_mem.data();
        r2.Source.Memory.Size = 512;
        r2.Destination.Memory.Buffer = dst_mem.data();
        r2.Destination.Memory.Size = 512;
        r2.UncompressedSize = 512;
        r2.CancellationTag = 0x11110002ull;
        r2.Name = "mem_copy";
        queue->EnqueueRequest(&r2);

        // 3. Zlib-decompressed memory-to-memory
        DSTORAGE_REQUEST r3{};
        r3.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        r3.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        r3.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_ZLIB;
        r3.Source.Memory.Source = zlib_blob.data();
        r3.Source.Memory.Size = static_cast<UINT32>(zlib_blob.size());
        r3.Destination.Memory.Buffer = dst_zlib.data();
        r3.Destination.Memory.Size = kZlibLen;
        r3.UncompressedSize = kZlibLen;
        r3.CancellationTag = 0x11110003ull;
        r3.Name = "zlib_inflate";
        queue->EnqueueRequest(&r3);

        // 4. Tagged request to be cancelled prior to Submit()
        DSTORAGE_REQUEST r4{};
        r4.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        r4.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        r4.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
        r4.Source.Memory.Source = src_mem.data();
        r4.Source.Memory.Size = 128;
        r4.Destination.Memory.Buffer = dst_cancelled.data();
        r4.Destination.Memory.Size = 128;
        r4.UncompressedSize = 128;
        r4.CancellationTag = 0xCAFE0042ull;
        r4.Name = "to_cancel";
        queue->EnqueueRequest(&r4);
        queue->CancelRequestsWithTag(0xFFFF0000ull, 0xCAFE0000ull);

        HANDLE fence_evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE ds_evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        CHECK(SUCCEEDED(fence->SetEventOnCompletion(7u, fence_evt)), "SetEventOnCompletion");

        queue->EnqueueStatus(status.Get(), 0);
        CHECK(!status->IsComplete(0), "Status slot 0 pending before Submit");
        queue->EnqueueSignal(fence.Get(), 7u);
        queue->EnqueueSetEvent(ds_evt);

        DSTORAGE_QUEUE_INFO qinfo{};
        queue->Query(&qinfo);
        CHECK(qinfo.EmptySlotCount < qdesc.Capacity, "Queue Query reflects enqueued commands");

        queue->Submit();

        CHECK(WaitForSingleObject(fence_evt, 5000) == WAIT_OBJECT_0, "ID3D12Fence signaled by dstorage_xs");
        CHECK(WaitForSingleObject(ds_evt, 5000) == WAIT_OBJECT_0, "Win32 event signaled by dstorage_xs");
        CloseHandle(fence_evt);
        CloseHandle(ds_evt);

        CHECK(fence->GetCompletedValue() == 7u, "ID3D12Fence value == 7");
        CHECK(status->IsComplete(0), "Status slot 0 complete");
        CHECK(status->GetHResult(0) == S_OK, "Status slot 0 S_OK");
        CHECK(std::memcmp(dst_file.data(), disk_payload.data() + 512, 1024) == 0, "File-to-mem bytes exact");
        CHECK(std::memcmp(dst_mem.data(), src_mem.data(), 512) == 0, "Mem-to-mem bytes exact");
        CHECK(std::memcmp(dst_zlib.data(), kZlibText, kZlibLen) == 0, "Zlib inflated bytes exact");

        bool untouched = true;
        for (uint8_t b : dst_cancelled) {
            if (b != 0x5Au) { untouched = false; break; }
        }
        CHECK(untouched, "CancelRequestsWithTag skipped cancelled request");

        queue->Close();
    }

    if (ds_file) ds_file->Close();
    DeleteFileW(temp_file);
    std::printf("[scarlett_sys_dlls] dstorage_xs.dll OK (file+mem+zlib+cancel+ID3D12Fence verified)\n");
}

void test_acphal() {
    CHECK(SUCCEEDED(ApuCreateHeap(64u * 1024u * 1024u, 64u * 1024u * 1024u)), "ApuCreateHeap");

    void* va = nullptr;
    APU_ADDRESS pa = APU_INVALID_ADDRESS;
    CHECK(SUCCEEDED(ApuAlloc(&va, &pa, 4096u, 256u, APU_ALLOC_CACHED)), "ApuAlloc 4KB align 256");
    CHECK(va != nullptr, "ApuAlloc va non-null");
    CHECK((reinterpret_cast<uintptr_t>(va) & 255u) == 0u, "ApuAlloc va 256B aligned");
    CHECK(pa != APU_INVALID_ADDRESS && (pa & 255u) == 0u, "ApuAlloc pa valid & 256B aligned");

    if (va) {
        auto* bytes = static_cast<uint8_t*>(va);
        std::memset(bytes, 0x3Cu, 4096u);
        CHECK(ApuIsVirtualAddressValid(bytes, 4096u) == TRUE, "Full span valid");
        CHECK(ApuIsVirtualAddressValid(bytes + 1024u, 3072u) == TRUE, "Interior tail valid");
        CHECK(ApuIsVirtualAddressValid(bytes + 1024u, 3073u) == FALSE, "Out-of-bounds rejected");
        CHECK(ApuIsVirtualAddressValid(nullptr, 1u) == FALSE, "Null VA rejected");
        CHECK(ApuMapVirtualAddress(bytes) == pa, "Base PA matches");
        CHECK(ApuMapVirtualAddress(bytes + 128u) == pa + 128u, "Interior PA offset exact");
        CHECK(SUCCEEDED(ApuFree(va)), "ApuFree");
        CHECK(ApuIsVirtualAddressValid(bytes, 1u) == FALSE, "Freed VA no longer valid");
        CHECK(ApuMapVirtualAddress(bytes) == APU_INVALID_ADDRESS, "Freed VA maps to 0");
    }

    SHAPE_CONTEXTS req{};
    req.numSrcContexts = 64;
    req.numEqContexts = 32;
    req.numFiltVolContexts = 64;
    req.numDmaContexts = 16;
    req.numXmaContexts = 32;
    req.numPcmContexts = 64;
    CHECK(SUCCEEDED(AcpHalAllocateShapeContexts(&req)), "AcpHalAllocateShapeContexts");

    ComPtr<IAcpHal> hal;
    CHECK(SUCCEEDED(AcpHalCreate(&hal)) && hal, "AcpHalCreate");
    if (hal) {
        CHECK(SUCCEEDED(hal->Connect(64, 64)), "IAcpHal::Connect");
        CHECK(SUCCEEDED(hal->SubmitCommand(ACP_COMMAND_TYPE_XMA_DECODE, 0x1234u, 0, nullptr, 0)),
              "IAcpHal::SubmitCommand");
        ACP_MESSAGE msg{};
        CHECK(hal->PopMessage(&msg) == S_OK && msg.payload == 0x1234u &&
                  msg.type == ACP_COMMAND_TYPE_XMA_DECODE,
              "IAcpHal::PopMessage returned submitted command");
        CHECK(hal->PopMessage(&msg) == S_FALSE, "IAcpHal::PopMessage S_FALSE when empty");
        CHECK(SUCCEEDED(hal->Disconnect()), "IAcpHal::Disconnect");
    }
    CHECK(SUCCEEDED(AcpHalReleaseShapeContexts()), "AcpHalReleaseShapeContexts");

    std::printf("[scarlett_sys_dlls] AcpHal.dll OK (APU alloc/map/validate + IAcpHal + SHAPE verified)\n");
}

} // namespace

int main() {
    test_xg_xs();
    test_dstorage_xs();
    test_acphal();

    if (g_failures != 0) {
        std::fprintf(stderr, "[scarlett_sys_dlls] FAILED with %d error(s)\n", g_failures);
        return 1;
    }
    std::printf("[scarlett_sys_dlls] ALL PASSED\n");
    return 0;
}
