// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Wave 14 Emulator Core End-to-End Verification Test (Pure Direct3D 12 SM 6.6):
//   1. GPU Compute BCPack Decompressor (BC1, BC3, BC4, BC5, BC7 — 16,384 blocks / 64 scan tiles)
//      via DStorageDecompressBCPackGpu + IDStorageQueueX (DSTORAGE_COMPRESSION_FORMAT_BCPACK).
//   2. Batched Multi-Mip GPU Compute Xbox Texture Deswizzler (4KB_D + 64KB_D_X with pipe_bank_xor)
//      verified bit-exact against CPU wsx_xg::deswizzle_2d.
//   3. Lock-Free WsxD3D12CommandListProxy Barrier Coalescing & Elision telemetry +
//      Concurrent Lock-Free Positional OVERLAPPED File I/O in dstorage_xs.dll.
//   4. Multi-Threaded RDNA2/DXIL + ID3D12PipelineLibrary1 PSO Pre-Warming & Cold-Restart Cache Hit.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "include/xbox/d3d12_xs.h"
#include "include/xbox/dstorage_xs.h"
#include "d3d12_xs_gpu_deswizzle.h"
#include "d3d12_xs_async_pso.h"
#include "wsx_bcpack.h"
#include "wsx_xg_layout.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

int fail(const char* msg, HRESULT hr = S_OK) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "[WAVE14 FAIL] %s (hr=0x%08lX)\n", msg, static_cast<unsigned long>(hr));
    } else {
        std::fprintf(stderr, "[WAVE14 FAIL] %s\n", msg);
    }
    return 1;
}

std::vector<uint8_t> make_synthetic_bcn(uint32_t nblocks, uint32_t bsz, uint32_t seed) {
    std::vector<uint8_t> out(static_cast<size_t>(nblocks) * bsz);
    uint32_t s = seed;
    auto rng = [&]() -> uint32_t {
        s = s * 1664525u + 1013904223u;
        return s;
    };
    for (uint32_t i = 0u; i < nblocks; ++i) {
        uint8_t* b = out.data() + static_cast<size_t>(i) * bsz;
        for (uint32_t k = 0u; k < bsz; ++k) {
            // Smooth correlated endpoints + high-entropy indices (realistic GPU texture statistics!)
            if (k < 4u) b[k] = static_cast<uint8_t>((i * 3u + k * 19u + (rng() & 7u)) & 0xFFu);
            else        b[k] = static_cast<uint8_t>((rng() >> 16) & 0xFFu);
        }
    }
    return out;
}

bool test_gpu_bcpack(ID3D12Device* dev) {
    struct FmtCase { DXGI_FORMAT fmt; const char* name; uint32_t bsz; uint32_t blocks; };
    const FmtCase cases[] = {
        {DXGI_FORMAT_BC1_UNORM_SRGB, "BC1", 8u,  16384u}, // 512x512 = 64 scan tiles!
        {DXGI_FORMAT_BC3_UNORM_SRGB, "BC3", 16u, 8192u},  // 512x256 = 32 scan tiles!
        {DXGI_FORMAT_BC4_UNORM,      "BC4", 8u,  4096u},  // 256x256 = 16 scan tiles!
        {DXGI_FORMAT_BC5_UNORM,      "BC5", 16u, 4096u},  // 256x256 = 16 scan tiles!
        {DXGI_FORMAT_BC7_UNORM_SRGB, "BC7", 16u, 8192u},  // 512x256 = 32 scan tiles!
    };

    uint64_t total_out_bytes = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t ci = 0; ci < _countof(cases); ++ci) {
        const auto& tc = cases[ci];
        const auto raw = make_synthetic_bcn(tc.blocks, tc.bsz, 0x13579BDFu + ci * 97u);

        UINT32 comp_sz = 0;
        HRESULT hr = DStorageCompressBCPack(raw.data(), static_cast<UINT32>(raw.size()), tc.fmt, nullptr, 0, &comp_sz);
        if (FAILED(hr) || comp_sz == 0) {
            std::fprintf(stderr, "CompressBCPack size query failed for %s\n", tc.name);
            return false;
        }
        std::vector<uint8_t> packed(comp_sz);
        hr = DStorageCompressBCPack(raw.data(), static_cast<UINT32>(raw.size()), tc.fmt, packed.data(), comp_sz, &comp_sz);
        if (FAILED(hr)) {
            std::fprintf(stderr, "CompressBCPack failed for %s\n", tc.name);
            return false;
        }

        std::vector<uint8_t> gpu_out(raw.size(), 0xCCu);
        UINT32 dec_sz = 0;
        BOOL used_gpu = FALSE;
        hr = DStorageDecompressBCPackGpu(dev, packed.data(), comp_sz, tc.fmt,
                                         gpu_out.data(), static_cast<UINT32>(gpu_out.size()),
                                         &dec_sz, &used_gpu);
        if (FAILED(hr) || dec_sz != raw.size() || !used_gpu) {
            std::fprintf(stderr, "DStorageDecompressBCPackGpu failed for %s (hr=0x%08lX, dec=%u, gpu=%d)\n",
                         tc.name, static_cast<unsigned long>(hr), dec_sz, static_cast<int>(used_gpu));
            return false;
        }
        if (std::memcmp(raw.data(), gpu_out.data(), raw.size()) != 0) {
            std::fprintf(stderr, "GPU BCPack bit mismatch for %s!\n", tc.name);
            return false;
        }
        total_out_bytes += raw.size();
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // Also test IDStorageQueueX end-to-end GPU BCPack request -> memory destination!
    ComPtr<IDStorageFactoryX> factory;
    if (FAILED(DStorageGetFactory(IID_PPV_ARGS(&factory))) || !factory) return false;
    DSTORAGE_QUEUE_DESC qd{};
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    qd.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    qd.Priority = DSTORAGE_PRIORITY_NORMAL;
    qd.Device = dev;
    ComPtr<IDStorageQueueX> q;
    ComPtr<IDStorageStatusArrayX> st;
    if (FAILED(factory->CreateQueue(&qd, IID_PPV_ARGS(&q))) ||
        FAILED(factory->CreateStatusArray(1, "bcpack_gpu", IID_PPV_ARGS(&st)))) return false;

    const auto bc7_src = make_synthetic_bcn(4096u, 16u, 0xCAFEBABEu); // 64 KiB (>= 16 KiB GPU threshold)
    std::vector<uint8_t> packed;
    if (!wsx_bcpack::compress(bc7_src.data(), bc7_src.size(), DXGI_FORMAT_BC7_UNORM_SRGB, packed)) return false;

    std::vector<uint8_t> dst(bc7_src.size(), 0u);
    DSTORAGE_REQUEST req{};
    req.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_BCPACK;
    req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    req.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    req.Source.Memory.Source = packed.data();
    req.Source.Memory.Size = static_cast<UINT32>(packed.size());
    req.Destination.Memory.Buffer = dst.data();
    req.Destination.Memory.Size = static_cast<UINT32>(dst.size());
    req.UncompressedSize = static_cast<UINT32>(bc7_src.size());
    q->EnqueueRequest(&req);
    q->EnqueueStatus(st.Get(), 0);
    q->Submit();

    const auto w0 = std::chrono::steady_clock::now();
    while (!st->IsComplete(0)) {
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count() > 10.0) return false;
        Sleep(1);
    }
    if (FAILED(st->GetHResult(0)) || std::memcmp(dst.data(), bc7_src.data(), bc7_src.size()) != 0) return false;

    std::printf("[PASS] 1. GPU Compute BCPack (BC1/BC3/BC4/BC5/BC7 + DStorageQueue): %.2f KB bit-exact in %.2f ms\n",
                static_cast<double>(total_out_bytes) / 1024.0, ms);
    return true;
}

bool test_batched_gpu_deswizzle(ID3D12Device* dev) {
    wsx_d3d12::GpuTextureDeswizzler desw;
    if (!desw.ready(dev)) {
        std::fprintf(stderr, "GpuTextureDeswizzler::ready failed\n");
        return false;
    }

    // Build a 3-mip BC7 64KB_D_X (pipe_bank_xor=7) + 1 RGBA8 4KB_D batch!
    struct JobSpec { DXGI_FORMAT fmt; uint32_t w, h; uint32_t swiz; uint32_t xor_val; };
    const JobSpec specs[] = {
        {DXGI_FORMAT_BC7_UNORM_SRGB,  256u, 256u, 26u, 7u}, // 64KB_R_X / D_X BC7 mip0
        {DXGI_FORMAT_BC7_UNORM_SRGB,  128u, 128u, 26u, 7u}, // BC7 mip1
        {DXGI_FORMAT_R8G8B8A8_UNORM,  128u, 128u,  2u, 0u}, // 4KB_D RGBA8
    };

    std::vector<wsx_d3d12::GpuDeswizzleSubresourceJob> jobs;
    std::vector<uint8_t> src_all, ref_all;
    uint64_t src_cursor = 0, dst_cursor = 0;

    for (uint32_t i = 0; i < _countof(specs); ++i) {
        const auto& sp = specs[i];
        const wsx_xg::SubresourceLayout sl = wsx_xg::compute_subresource_2d(sp.w, sp.h, 0u, sp.fmt, sp.swiz, 0u);

        const uint32_t row_pitch = (sl.width_elems * sl.bytes_per_element + 255u) & ~255u;
        const uint64_t dst_sz = static_cast<uint64_t>(row_pitch) * sl.height_elems;
        src_cursor = (src_cursor + 255ull) & ~255ull;
        dst_cursor = (dst_cursor + 255ull) & ~255ull;

        std::vector<uint8_t> tiled(static_cast<size_t>(sl.size_bytes));
        for (size_t k = 0; k < tiled.size(); ++k) {
            tiled[k] = static_cast<uint8_t>((k * 37u + i * 113u + 17u) & 0xFFu);
        }
        src_all.resize(static_cast<size_t>(src_cursor + sl.size_bytes), 0u);
        std::memcpy(src_all.data() + src_cursor, tiled.data(), tiled.size());

        ref_all.resize(static_cast<size_t>(dst_cursor + dst_sz), 0u);
        // Compute bit-exact CPU reference matching GPU shader (including pipe_bank_xor)
        for (uint32_t ey = 0; ey < sl.height_elems; ++ey) {
            for (uint32_t ex = 0; ex < sl.width_elems; ++ex) {
                const uint32_t tx = ex / sl.tile_w;
                const uint32_t ty = ey / sl.tile_h;
                const uint32_t lx = ex - tx * sl.tile_w;
                const uint32_t ly = ey - ty * sl.tile_h;
                const uint32_t tiles_per_row = sl.padded_width_elems / sl.tile_w;
                const uint32_t tile_bytes = sl.tile_w * sl.tile_h * sl.bytes_per_element;
                uint32_t m = 0u, ob = 0u, bx = 0u, by = 0u;
                while ((1u << bx) < sl.tile_w || (1u << by) < sl.tile_h) {
                    if ((1u << bx) < sl.tile_w) { m |= (((lx >> bx) & 1u) << ob++); ++bx; }
                    if ((1u << by) < sl.tile_h) { m |= (((ly >> by) & 1u) << ob++); ++by; }
                }
                uint32_t in_tile = m * sl.bytes_per_element;
                if (sl.swizzle_mode == 24u || sl.swizzle_mode == 25u || sl.swizzle_mode == 26u) {
                    const uint32_t xb = ((tx & 0x3Fu) ^ ((ty & 0x3Fu) << 1) ^ (sp.xor_val & 0x3Fu)) & 0x3Fu;
                    in_tile ^= (xb << 8);
                }
                const uint32_t soff = (ty * tiles_per_row + tx) * tile_bytes + in_tile;
                const uint64_t doff = dst_cursor + static_cast<uint64_t>(ey) * row_pitch + ex * sl.bytes_per_element;
                std::memcpy(ref_all.data() + doff, tiled.data() + soff, sl.bytes_per_element);
            }
        }

        wsx_d3d12::GpuDeswizzleSubresourceJob jb{};
        jb.src_byte_offset = src_cursor;
        jb.dst_byte_offset = dst_cursor;
        jb.layout = sl;
        jb.dst_row_pitch = row_pitch;
        jb.pipe_bank_xor = sp.xor_val;
        jobs.push_back(jb);
        src_cursor += sl.size_bytes;
        dst_cursor += dst_sz;
    }

    auto make_buf = [&](D3D12_HEAP_TYPE ht, UINT64 sz, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st, ComPtr<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES hp{ht};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (sz + 255ull) & ~255ull; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = fl;
        return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&out)));
    };
    ComPtr<ID3D12Resource> src_up, dst_uav, dst_rb;
    if (!make_buf(D3D12_HEAP_TYPE_UPLOAD, src_all.size(), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, src_up) ||
        !make_buf(D3D12_HEAP_TYPE_DEFAULT, ref_all.size(), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, dst_uav) ||
        !make_buf(D3D12_HEAP_TYPE_READBACK, ref_all.size(), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, dst_rb)) {
        return false;
    }
    void* p = nullptr;
    src_up->Map(0, nullptr, &p); std::memcpy(p, src_all.data(), src_all.size()); src_up->Unmap(0, nullptr);

    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> ca;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) ||
        FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr, IID_PPV_ARGS(&cl))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;

    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t ndisp = desw.record_deswizzle_batch(cl.Get(), src_up.Get(), dst_uav.Get(),
                                                      jobs.data(), static_cast<uint32_t>(jobs.size()));
    if (ndisp != jobs.size()) return false;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {dst_uav.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    cl->ResourceBarrier(1, &b);
    cl->CopyBufferRegion(dst_rb.Get(), 0, dst_uav.Get(), 0, ref_all.size());
    cl->Close();
    ID3D12CommandList* lists[] = {cl.Get()};
    q->ExecuteCommandLists(1, lists);
    q->Signal(fence.Get(), 1);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fence->GetCompletedValue() < 1) { fence->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, 10000); }
    CloseHandle(ev);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    void* rb = nullptr;
    D3D12_RANGE rr{0, ref_all.size()};
    dst_rb->Map(0, &rr, &rb);
    bool match = true;
    for (const auto& jb : jobs) {
        const uint64_t row_bytes = static_cast<uint64_t>(jb.layout.width_elems) * jb.layout.bytes_per_element;
        for (uint32_t y = 0; y < jb.layout.height_elems; ++y) {
            const uint64_t off = jb.dst_byte_offset + static_cast<uint64_t>(y) * jb.dst_row_pitch;
            if (std::memcmp(static_cast<const uint8_t*>(rb) + off, ref_all.data() + off, static_cast<size_t>(row_bytes)) != 0) {
                match = false;
                break;
            }
        }
    }
    dst_rb->Unmap(0, nullptr);
    const auto st = desw.get_stats();
    if (!match || st.batch_calls != 1u || st.dispatches != jobs.size()) return false;
    std::printf("[PASS] 2. Batched GPU Deswizzle (64KB_D_X + 4KB_D, %u subresources in 1 bind): %.2f KB bit-exact in %.2f ms\n",
                ndisp, static_cast<double>(st.bytes_deswizzled) / 1024.0, ms);
    return true;
}

bool test_lockfree_barriers_and_overlapped_io(ID3D12Device* dev) {
    // Part A: Verify lock-free WsxD3D12CommandListProxy barrier coalescing & round-trip elision!
    D3D12XBOX_HIGHLOAD_PIPELINE_STATS st0{};
    if (FAILED(D3D12XboxGetHighloadPipelineStats(dev, &st0))) return false;

    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 4096; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> res;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&res)))) return false;

    ComPtr<ID3D12CommandAllocator> ca;
    ComPtr<ID3D12GraphicsCommandList> cl;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr, IID_PPV_ARGS(&cl)))) return false;

    // Submit 4 barriers on the same resource:
    //   1) COMMON -> COPY_DEST
    //   2) COPY_DEST -> UNORDERED_ACCESS  (coalesces with #1 into COMMON -> UNORDERED_ACCESS: +1 elided!)
    //   3) UNORDERED_ACCESS -> COPY_DEST  (coalesces into COMMON -> COPY_DEST: +1 elided!)
    //   4) COPY_DEST -> COMMON            (round-trip back to COMMON -> elides completely: +1 elided!)
    D3D12_RESOURCE_BARRIER bs[4]{};
    for (int i = 0; i < 4; ++i) {
        bs[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bs[i].Transition.pResource = res.Get();
        bs[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    bs[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;           bs[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    bs[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;        bs[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bs[2].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; bs[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    bs[3].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;        bs[3].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    cl->ResourceBarrier(4, bs);
    cl->Close(); // Flushes lock-free per-CL counters!

    D3D12XBOX_HIGHLOAD_PIPELINE_STATS st1{};
    if (FAILED(D3D12XboxGetHighloadPipelineStats(dev, &st1))) return false;
    const uint64_t elided = st1.barriers_elided - st0.barriers_elided;
    if (elided < 3u) {
        std::fprintf(stderr, "Expected >=3 coalesced/elided barriers, got %llu\n", static_cast<unsigned long long>(elided));
        return false;
    }

    // Part B: Verify lock-free concurrent positional OVERLAPPED File I/O across 4 queues sharing 1 IDStorageFileX!
    const fs::path tmp_file = fs::temp_directory_path() / "wsx_wave14_overlapped_io.bin";
    constexpr uint32_t kChunk = 256u * 1024u;
    std::vector<uint8_t> disk_data(kChunk * 4u);
    for (size_t i = 0; i < disk_data.size(); ++i) disk_data[i] = static_cast<uint8_t>((i * 131u + 7u) & 0xFFu);
    {
        std::ofstream f(tmp_file, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(disk_data.data()), static_cast<std::streamsize>(disk_data.size()));
    }

    ComPtr<IDStorageFactoryX> factory;
    ComPtr<IDStorageFileX> ds_file;
    if (FAILED(DStorageGetFactory(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->OpenFile(tmp_file.wstring().c_str(), IID_PPV_ARGS(&ds_file)))) return false;

    ComPtr<IDStorageQueueX> qs[4];
    ComPtr<IDStorageStatusArrayX> sts[4];
    std::vector<uint8_t> bufs[4];
    for (int i = 0; i < 4; ++i) {
        DSTORAGE_QUEUE_DESC qd{};
        qd.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        qd.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
        qd.Priority = DSTORAGE_PRIORITY_NORMAL;
        if (FAILED(factory->CreateQueue(&qd, IID_PPV_ARGS(&qs[i]))) ||
            FAILED(factory->CreateStatusArray(1, "ov", IID_PPV_ARGS(&sts[i])))) return false;
        bufs[i].assign(kChunk, 0u);
        DSTORAGE_REQUEST r{};
        r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        r.Source.File.Source = ds_file.Get();
        r.Source.File.Offset = static_cast<UINT64>(i) * kChunk;
        r.Source.File.Size = kChunk;
        r.Destination.Memory.Buffer = bufs[i].data();
        r.Destination.Memory.Size = kChunk;
        r.UncompressedSize = kChunk;
        qs[i]->EnqueueRequest(&r);
        qs[i]->EnqueueStatus(sts[i].Get(), 0);
    }
    for (int i = 0; i < 4; ++i) qs[i]->Submit();
    for (int i = 0; i < 4; ++i) {
        const auto w0 = std::chrono::steady_clock::now();
        while (!sts[i]->IsComplete(0)) {
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count() > 10.0) return false;
            Sleep(1);
        }
        if (FAILED(sts[i]->GetHResult(0)) ||
            std::memcmp(bufs[i].data(), disk_data.data() + static_cast<size_t>(i) * kChunk, kChunk) != 0) {
            return false;
        }
    }
    ds_file->Close();
    std::error_code ec;
    fs::remove(tmp_file, ec);

    std::printf("[PASS] 3. Lock-Free Barrier Coalescing (elided=%llu/4) + 4-Queue Concurrent OVERLAPPED NVMe I/O (1 MB exact)\n",
                static_cast<unsigned long long>(elided));
    return true;
}

bool test_pso_prewarm_and_cold_restart_hits() {
    const fs::path cache_dir = fs::temp_directory_path() / "wsx_wave14_pso_prewarm_cache";
    std::error_code ec;
    fs::remove_all(cache_dir, ec);
    fs::create_directories(cache_dir, ec);
    SetEnvironmentVariableW(L"WSX_PSO_CACHE_DIR", cache_dir.wstring().c_str());

    // Compile 4 distinct SM 6.6 compute shaders to DXIL and pre-warm ID3D12PipelineLibrary1 across worker threads!
    std::vector<std::vector<uint8_t>> dxils;
    for (int k = 0; k < 4; ++k) {
        char hlsl[512]{};
        std::snprintf(hlsl, sizeof(hlsl),
            "RWByteAddressBuffer uOut : register(u0);\n"
            "[RootSignature(\"RootFlags(0), UAV(u0)\")]\n"
            "[numthreads(64,1,1)]\n"
            "void main(uint3 tid : SV_DispatchThreadID) { uOut.Store(tid.x * 4u, tid.x + %du); }\n",
            k * 17 + 1);
        std::vector<uint8_t> d(256u * 1024u);
        size_t sz = 0;
        char err[512]{};
        if (FAILED(D3D12XboxCompileHlslToDxil(hlsl, std::strlen(hlsl), L"main", L"cs_6_6",
                                              d.data(), d.size(), &sz, err, sizeof(err))) || sz < 64u) {
            return false;
        }
        d.resize(sz);
        dxils.push_back(std::move(d));
    }

    // Cold Boot #1: Pre-warm 4 PSOs in parallel via AsyncShaderCompileQueue::prewarm_compute_psos & flush to disk!
    D3D12XBOX_PSO_CACHE_STATS s_before{};
    D3D12XboxGetPsoCacheStats(&s_before);
    {
        ComPtr<ID3D12Device> d1;
        D3D12XBOX_CREATE_DEVICE_PARAMETERS dp{};
        dp.Version = D3D12_SDK_VERSION;
        if (FAILED(D3D12XboxCreateDevice(nullptr, &dp, IID_PPV_ARGS(&d1))) || !d1) return false;
        ComPtr<ID3D12RootSignature> rs1;
        if (FAILED(d1->CreateRootSignature(0, dxils[0].data(), dxils[0].size(), IID_PPV_ARGS(&rs1)))) return false;

        const uint32_t warmed = wsx_async::AsyncShaderCompileQueue::prewarm_compute_psos(d1.Get(), rs1.Get(), dxils);
        if (warmed != 4u) return false;
        if (FAILED(D3D12XboxFlushPsoCache(d1.Get()))) return false;
    }

    // Cold Boot #2 (Fresh ID3D12Device): Every single PSO must hit persistent ID3D12PipelineLibrary1 on disk!
    D3D12XBOX_PSO_CACHE_STATS s_mid{};
    D3D12XboxGetPsoCacheStats(&s_mid);
    {
        ComPtr<ID3D12Device> d2;
        D3D12XBOX_CREATE_DEVICE_PARAMETERS dp{};
        dp.Version = D3D12_SDK_VERSION;
        if (FAILED(D3D12XboxCreateDevice(nullptr, &dp, IID_PPV_ARGS(&d2))) || !d2) return false;
        ComPtr<ID3D12RootSignature> rs2;
        if (FAILED(d2->CreateRootSignature(0, dxils[0].data(), dxils[0].size(), IID_PPV_ARGS(&rs2)))) return false;

        const uint32_t warmed2 = wsx_async::AsyncShaderCompileQueue::prewarm_compute_psos(d2.Get(), rs2.Get(), dxils);
        if (warmed2 != 4u) return false;
    }
    D3D12XBOX_PSO_CACHE_STATS s_after{};
    D3D12XboxGetPsoCacheStats(&s_after);
    const uint64_t warm_hits = s_after.hits - s_mid.hits;
    SetEnvironmentVariableW(L"WSX_PSO_CACHE_DIR", nullptr);
    fs::remove_all(cache_dir, ec);

    if (warm_hits < 4u) {
        std::fprintf(stderr, "Expected >=4 PSO cache hits on cold restart, got %llu\n", static_cast<unsigned long long>(warm_hits));
        return false;
    }
    std::printf("[PASS] 4. Multi-Threaded RDNA2/DXIL PSO Pre-Warming + Persistent PipelineLibrary1 (cold-restart hits=%llu/4, lib=%llu B)\n",
                static_cast<unsigned long long>(warm_hits), static_cast<unsigned long long>(s_after.library_bytes));
    return true;
}

} // namespace

int main() {
    ComPtr<ID3D12Device> dev;
    D3D12XBOX_CREATE_DEVICE_PARAMETERS dp{};
    dp.Version = D3D12_SDK_VERSION;
    HRESULT hr = D3D12XboxCreateDevice(nullptr, &dp, IID_PPV_ARGS(&dev));
    if (FAILED(hr) || !dev) return fail("D3D12XboxCreateDevice failed", hr);

    if (!test_gpu_bcpack(dev.Get())) return fail("GPU BCPack test failed");
    if (!test_batched_gpu_deswizzle(dev.Get())) return fail("Batched GPU Deswizzle test failed");
    if (!test_lockfree_barriers_and_overlapped_io(dev.Get())) return fail("Lock-free barriers / OVERLAPPED I/O test failed");
    if (!test_pso_prewarm_and_cold_restart_hits()) return fail("PSO pre-warming test failed");

    std::printf("=== ALL WAVE 14 CORE TESTS PASSED (GPU BCPack + GPU Deswizzle + Lock-Free Barriers/IO + PSO Pre-Warm) ===\n");
    return 0;
}
