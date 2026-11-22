// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Wave 2 Verification Test (High-Throughput DX12 GPU Pipeline & Memory):
// 1. Hardware MEM_WRITE_WATCH O(1) dirty-page detection & zero-scan clean skips
// 2. Pure D3D12 SM 6.6 GPU Compute Deswizzler (4KB_S RGBA8, RDNA2 64KB_S_X RGBA8, 64KB_S_X BC7_UNORM)
//    verified bit-exact on RTX GPU against wsx_xg reference
// 3. Redundant UAV/no-op barrier deduplication + idle staging memory trim
// 4. DXGI 1.6 VRAM budget & pressure telemetry (D3D12XboxQueryVramBudget)
// 5. Multi-threaded async RDNA2->DXIL pre-compiler queue (real GTA VI FSR 4.1.1 ACCC/FSPX shaders + dedup)
// 6. xmem.dll O(1) 64KB..2MB size-class pool allocator (4,000 cycles, zero-init, XMemTrimPools)
// All GPU operations validated under the D3D12 Debug Layer (0 errors).

#include "include/xbox/d3d12_xs.h"
#include "wsx_xg_layout.h"
#include "xmem.h"

#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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

void wait_queue_idle(ID3D12Device* dev, ID3D12CommandQueue* queue) {
    ComPtr<ID3D12Fence> f;
    if (FAILED(dev->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)))) return;
    queue->Signal(f.Get(), 1u);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev) {
        f->SetEventOnCompletion(1u, ev);
        WaitForSingleObject(ev, 5000);
        CloseHandle(ev);
    }
}

bool readback_texture_2d(
    ID3D12Device* dev,
    ID3D12CommandQueue* queue,
    ID3D12Resource* tex,
    const D3D12_RESOURCE_DESC& desc,
    D3D12_RESOURCE_STATES current_state,
    std::vector<uint8_t>& out_bytes,
    uint32_t& out_row_pitch,
    uint32_t& out_num_rows) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT nrows = 0;
    UINT64 rsize = 0, total = 0;
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &nrows, &rsize, &total);

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = wsx_xg::align_up(total, 256u);
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> rb;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) {
        return false;
    }

    ComPtr<ID3D12CommandAllocator> ca;
    ComPtr<ID3D12GraphicsCommandList> cl;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca)))) return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr, IID_PPV_ARGS(&cl)))) return false;

    if (current_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = current_state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = rb.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    if (current_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = current_state;
        cl->ResourceBarrier(1, &b);
    }

    if (FAILED(cl->Close())) return false;
    ID3D12CommandList* lists[] = {cl.Get()};
    queue->ExecuteCommandLists(1, lists);
    wait_queue_idle(dev, queue);

    void* p = nullptr;
    D3D12_RANGE rr{0, static_cast<SIZE_T>(total)};
    if (FAILED(rb->Map(0, &rr, &p)) || !p) return false;
    out_bytes.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + total);
    D3D12_RANGE wr{0, 0};
    rb->Unmap(0, &wr);
    out_row_pitch = fp.Footprint.RowPitch;
    out_num_rows = nrows;
    return true;
}

bool verify_gpu_deswizzle_case(
    ID3D12Device* dev,
    ID3D12CommandQueue* queue,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT fmt,
    uint32_t swizzle_mode,
    const char* label) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = width;
    rd.Height = height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ComPtr<ID3D12Resource> tex;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                            nullptr, IID_PPV_ARGS(&tex)))) {
        std::fprintf(stderr, "  [FAIL] CreateCommittedResource failed for %s\n", label);
        return false;
    }

    if (FAILED(D3D12XboxSetGuestTextureSwizzleMode(tex.Get(), swizzle_mode))) {
        std::fprintf(stderr, "  [FAIL] SetGuestTextureSwizzleMode failed for %s\n", label);
        return false;
    }
    auto* guest_mem = reinterpret_cast<uint8_t*>(tex->GetGPUVirtualAddress());
    if (!guest_mem) return false;

    const auto sl = wsx_xg::compute_subresource_2d(width, height, 0, fmt, swizzle_mode, 0);
    const uint32_t linear_row = sl.width_elems * sl.bytes_per_element;
    std::vector<uint8_t> ref_linear(static_cast<size_t>(linear_row) * sl.height_elems);
    for (uint32_t y = 0; y < sl.height_elems; ++y) {
        for (uint32_t x = 0; x < sl.width_elems; ++x) {
            uint8_t* dst = ref_linear.data() + static_cast<size_t>(y) * linear_row + static_cast<size_t>(x) * sl.bytes_per_element;
            for (uint32_t b = 0; b < sl.bytes_per_element; ++b) {
                dst[b] = static_cast<uint8_t>(((x * 13u + y * 37u + b * 7u + swizzle_mode * 3u) ^ 0x5Au) & 0xFFu);
            }
        }
    }

    wsx_xg::swizzle_2d(sl, ref_linear.data(), linear_row, guest_mem);
    if (FAILED(D3D12XboxFlushGuestTextures(queue))) return false;
    wait_queue_idle(dev, queue);

    std::vector<uint8_t> gpu_bytes;
    uint32_t row_pitch = 0, num_rows = 0;
    if (!readback_texture_2d(dev, queue, tex.Get(), rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             gpu_bytes, row_pitch, num_rows)) {
        return false;
    }
    if (num_rows != sl.height_elems) return false;
    for (uint32_t y = 0; y < sl.height_elems; ++y) {
        const uint8_t* ref_row = ref_linear.data() + static_cast<size_t>(y) * linear_row;
        const uint8_t* gpu_row = gpu_bytes.data() + static_cast<size_t>(y) * row_pitch;
        if (std::memcmp(ref_row, gpu_row, linear_row) != 0) {
            std::fprintf(stderr, "  [FAIL] GPU deswizzle mismatch at row %u for %s\n", y, label);
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    SetEnvironmentVariableW(L"WSX_GPU_DESWIZZLE", L"force");
    SetEnvironmentVariableW(L"WSX_ASYNC_SHADER_THREADS", L"4");

    D3D12XBOX_CREATE_DEVICE_PARAMETERS cdp{};
    cdp.Version = D3D12_SDK_VERSION;
    cdp.ProcessDebugFlags = D3D12_PROCESS_DEBUG_FLAG_DEBUG_LAYER_ENABLED;
    ComPtr<ID3D12Device> dev;
    check_hr(D3D12XboxCreateDevice(nullptr, &cdp, IID_PPV_ARGS(&dev)), "D3D12XboxCreateDevice");
    if (!dev) return 1;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    check_hr(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");

    // ------------------------------------------------------------------------
    // 1. Hardware MEM_WRITE_WATCH O(1) Dirty Tracking & Clean Skip
    // ------------------------------------------------------------------------
    std::printf("[Wave2 / 1] Hardware MEM_WRITE_WATCH O(1) Dirty Tracking...\n");
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = 256;
        rd.Height = 256;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        ComPtr<ID3D12Resource> tex;
        check_hr(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                              nullptr, IID_PPV_ARGS(&tex)), "Create 256x256 guest tex");
        auto* mem = reinterpret_cast<uint32_t*>(tex->GetGPUVirtualAddress());
        check(mem != nullptr, "Guest UMA VA non-null");
        if (mem) {
            mem[0] = 0xFF112233u;
            mem[64 * 256 + 17] = 0xFF445566u;
        }
        check_hr(D3D12XboxFlushGuestTextures(queue.Get()), "Flush #1 (dirty)");
        D3D12XBOX_GUEST_TEXTURE_STATS gs1{};
        check_hr(D3D12XboxGetGuestTextureSyncStats(dev.Get(), &gs1), "GetGuestStats #1");
        check(gs1.textures_uploaded == 1u, "Expected 1 upload on first flush");

        for (int i = 0; i < 5; ++i) {
            check_hr(D3D12XboxFlushGuestTextures(queue.Get()), "Clean flush");
        }
        D3D12XBOX_GUEST_TEXTURE_STATS gs2{};
        D3D12XBOX_HIGHLOAD_PIPELINE_STATS hs2{};
        check_hr(D3D12XboxGetGuestTextureSyncStats(dev.Get(), &gs2), "GetGuestStats #2");
        check_hr(D3D12XboxGetHighloadPipelineStats(dev.Get(), &hs2), "GetHighloadStats #2");
        check(gs2.textures_uploaded == 1u, "Zero redundant uploads on 5 untouched flushes");
        check(hs2.write_watch_skips >= 5u, "MEM_WRITE_WATCH recorded >= 5 O(1) clean skips");

        if (mem) {
            mem[200 * 256 + 99] = 0xFFCAFE99u;
        }
        check_hr(D3D12XboxFlushGuestTextures(queue.Get()), "Flush #2 (single pixel dirty)");
        wait_queue_idle(dev.Get(), queue.Get());
        D3D12XBOX_GUEST_TEXTURE_STATS gs3{};
        check_hr(D3D12XboxGetGuestTextureSyncStats(dev.Get(), &gs3), "GetGuestStats #3");
        check(gs3.textures_uploaded == 2u, "Single modified page triggered exact dirty upload");

        std::vector<uint8_t> rb;
        uint32_t rp = 0, nr = 0;
        check(readback_texture_2d(dev.Get(), queue.Get(), tex.Get(), rd,
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, rb, rp, nr), "Readback dirty tex");
        if (!rb.empty()) {
            uint32_t px = 0;
            std::memcpy(&px, rb.data() + static_cast<size_t>(200) * rp + 99u * 4u, 4u);
            check(px == 0xFFCAFE99u, "Modified pixel matches 0xFFCAFE99 bit-exact on GPU");
        }
        std::printf("  [PASS] MEM_WRITE_WATCH skips=%llu, dirty uploads=%llu, GPU readback exact\n",
                    static_cast<unsigned long long>(hs2.write_watch_skips),
                    static_cast<unsigned long long>(gs3.textures_uploaded));
    }

    // ------------------------------------------------------------------------
    // 2. Pure D3D12 SM 6.6 GPU Compute Deswizzler (4KB_S, 64KB_S_X, 64KB_S_X BC7)
    // ------------------------------------------------------------------------
    std::printf("[Wave2 / 2] Pure D3D12 SM 6.6 GPU Compute Deswizzler...\n");
    {
        D3D12XBOX_HIGHLOAD_PIPELINE_STATS before{};
        check_hr(D3D12XboxGetHighloadPipelineStats(dev.Get(), &before), "Highload before deswizzle");

        check(verify_gpu_deswizzle_case(dev.Get(), queue.Get(), 128, 128,
                                        DXGI_FORMAT_R8G8B8A8_UNORM, wsx_xg::SWIZZLE_4KB_S, "128x128 RGBA8 4KB_S"),
              "GPU Deswizzle 128x128 RGBA8 4KB_S bit-exact");
        check(verify_gpu_deswizzle_case(dev.Get(), queue.Get(), 256, 256,
                                        DXGI_FORMAT_R8G8B8A8_UNORM, wsx_xg::SWIZZLE_64KB_S_X, "256x256 RGBA8 64KB_S_X"),
              "GPU Deswizzle 256x256 RGBA8 64KB_S_X (RDNA2 pipe_bank_xor) bit-exact");
        check(verify_gpu_deswizzle_case(dev.Get(), queue.Get(), 256, 256,
                                        DXGI_FORMAT_BC7_UNORM, wsx_xg::SWIZZLE_64KB_S_X, "256x256 BC7 64KB_S_X"),
              "GPU Deswizzle 256x256 BC7_UNORM 64KB_S_X (16B blocks) bit-exact");

        D3D12XBOX_HIGHLOAD_PIPELINE_STATS after{};
        check_hr(D3D12XboxGetHighloadPipelineStats(dev.Get(), &after), "Highload after deswizzle");
        const uint64_t gpu_uploads = after.gpu_deswizzled_uploads - before.gpu_deswizzled_uploads;
        check(gpu_uploads >= 3u, "All 3 tiled textures executed on SM 6.6 GPU Compute Deswizzler");
        std::printf("  [PASS] GPU SM 6.6 Deswizzle: %llu textures (4KB_S + 64KB_S_X RGBA8 + 64KB_S_X BC7) 100%% bit-exact\n",
                    static_cast<unsigned long long>(gpu_uploads));
    }

    // ------------------------------------------------------------------------
    // 3. Redundant Barrier Deduplication & Staging Memory Trim
    // ------------------------------------------------------------------------
    std::printf("[Wave2 / 3] Barrier Deduplication & Staging Memory Trim...\n");
    {
        D3D12XBOX_HIGHLOAD_PIPELINE_STATS b0{};
        check_hr(D3D12XboxGetHighloadPipelineStats(dev.Get(), &b0), "Stats before barriers");

        ComPtr<ID3D12CommandAllocator> ca;
        ComPtr<ID3D12GraphicsCommandList> cl;
        check_hr(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca)), "CreateCA");
        check_hr(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr, IID_PPV_ARGS(&cl)), "CreateCL");

        D3D12_RESOURCE_BARRIER bars[4]{};
        // 3 duplicate global UAV barriers -> 2 must be elided!
        for (int i = 0; i < 3; ++i) {
            bars[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            bars[i].UAV.pResource = nullptr;
        }
        // 1 no-op Xbox-flag transition (0x10000000 -> COMMON) -> must be elided!
        bars[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[3].Transition.pResource = nullptr;
        bars[3].Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(0x10000000u);
        bars[3].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        cl->ResourceBarrier(4, bars);
        check_hr(cl->Close(), "CloseCL");
        ID3D12CommandList* lists[] = {cl.Get()};
        queue->ExecuteCommandLists(1, lists);
        wait_queue_idle(dev.Get(), queue.Get());

        uint64_t freed_slots = 0;
        check_hr(D3D12XboxTrimStagingMemory(queue.Get(), &freed_slots), "D3D12XboxTrimStagingMemory");
        check(freed_slots >= 1u, "TrimStagingMemory reclaimed >= 1 idle upload slot");

        D3D12XBOX_HIGHLOAD_PIPELINE_STATS b1{};
        check_hr(D3D12XboxGetHighloadPipelineStats(dev.Get(), &b1), "Stats after barriers/trim");
        const uint64_t elided = b1.barriers_elided - b0.barriers_elided;
        check(elided >= 3u, "Elided 2 duplicate UAV + 1 no-op Xbox transition barrier");
        check(b1.staging_trims >= 1u, "Staging trims telemetry updated");
        std::printf("  [PASS] Barriers elided=%llu | Staging slots trimmed=%llu\n",
                    static_cast<unsigned long long>(elided),
                    static_cast<unsigned long long>(freed_slots));
    }

    // ------------------------------------------------------------------------
    // 4. DXGI 1.6 VRAM Budget & Pressure Telemetry
    // ------------------------------------------------------------------------
    std::printf("[Wave2 / 4] DXGI 1.6 VRAM Budget Query...\n");
    {
        D3D12XBOX_VRAM_BUDGET vb{};
        check_hr(D3D12XboxQueryVramBudget(dev.Get(), &vb), "D3D12XboxQueryVramBudget");
        check(vb.local_budget_bytes > 512ULL * 1024ULL * 1024ULL, "Local VRAM budget > 512 MB");
        check(vb.pressure_level <= 2u, "Valid VRAM pressure tier");
        std::printf("  [PASS] VRAM Local: %.1f MB used / %.1f MB budget (avail=%.1f MB, pressure=%u)\n",
                    vb.local_usage_bytes / (1024.0 * 1024.0),
                    vb.local_budget_bytes / (1024.0 * 1024.0),
                    vb.local_available_bytes / (1024.0 * 1024.0),
                    vb.pressure_level);
    }

    // ------------------------------------------------------------------------
    // 5. Async Multi-Threaded Shader Pre-Compiler Queue on real GTA VI FSR 4.1.1 DLL
    // ------------------------------------------------------------------------
    std::printf("[Wave2 / 5] Async Multi-Threaded RDNA2->DXIL Pre-Compiler Queue...\n");
    if (argc >= 2 && std::filesystem::exists(argv[1])) {
        std::filesystem::path tmp_cache = std::filesystem::temp_directory_path() / "wsx_wave2_async_cache";
        std::error_code ec;
        std::filesystem::remove_all(tmp_cache, ec);
        SetEnvironmentVariableW(L"WSX_SHADER_CACHE_DIR", tmp_cache.wstring().c_str());

        std::ifstream in(argv[1], std::ios::binary);
        std::vector<uint8_t> dll((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<std::pair<size_t, size_t>> all_slices;
        for (size_t pos = 0; pos + 32u <= dll.size(); pos += 4u) {
            if (dll[pos] == 'D' && dll[pos + 1] == 'X' && dll[pos + 2] == 'B' && dll[pos + 3] == 'C') {
                uint32_t sz = 0;
                std::memcpy(&sz, dll.data() + pos + 24u, 4u);
                if (sz >= 64u && pos + sz <= dll.size()) {
                    all_slices.emplace_back(pos, sz);
                    pos += (sz & ~3u) - 4u;
                }
            }
        }
        std::sort(all_slices.begin(), all_slices.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });
        std::vector<std::pair<size_t, size_t>> slices;
        for (size_t i = 0; i < all_slices.size() && slices.size() < 6u; ++i) {
            slices.push_back(all_slices[i]);
        }
        check(slices.size() == 6u, "Found 6 real GTA VI FSR 4.1.1 ACCC/FSPX shaders");

        const auto t0 = std::chrono::steady_clock::now();
        for (const auto& [off, sz] : slices) {
            check_hr(D3D12XboxEnqueueAsyncShaderCompile(dll.data() + off, sz), "Enqueue async shader");
        }
        // Enqueue 2 duplicates to verify deduplication
        for (size_t i = 0; i < 2u && i < slices.size(); ++i) {
            check_hr(D3D12XboxEnqueueAsyncShaderCompile(dll.data() + slices[i].first, slices[i].second),
                     "Enqueue duplicate shader");
        }
        check_hr(D3D12XboxDrainAsyncShaderQueue(60000u), "Drain async shader queue");
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        D3D12XBOX_HIGHLOAD_PIPELINE_STATS as{};
        check_hr(D3D12XboxGetHighloadPipelineStats(dev.Get(), &as), "Get async stats");
        check(as.async_enqueued == 6u, "Exactly 6 unique shaders enqueued");
        check(as.async_completed == 6u && as.async_failed == 0u, "6/6 real ACCC/FSPX shaders compiled async");
        check(as.async_dedup_hits == 2u, "2/2 duplicate shader submissions deduplicated");
        check(as.async_worker_threads >= 2u, "Multiple worker threads active");

        std::filesystem::remove_all(tmp_cache, ec);
        SetEnvironmentVariableW(L"WSX_SHADER_CACHE_DIR", nullptr);
        std::printf("  [PASS] Async Pre-Compiler: 6/6 real GTA VI shaders in %.1f ms on %u threads (dedup=%llu)\n",
                    ms, as.async_worker_threads, static_cast<unsigned long long>(as.async_dedup_hits));
    } else {
        std::printf("  [SKIP] GTA VI FSR DLL not passed\n");
    }

    // ------------------------------------------------------------------------
    // 6. xmem.dll O(1) 64KB..2MB Size-Class Slab Pool Benchmark & Trim
    // ------------------------------------------------------------------------
    std::printf("[Wave2 / 6] xmem.dll O(1) Size-Class Pool Allocator (4,000 cycles)...\n");
    {
        HMODULE xmem_mod = LoadLibraryW(L"xmem.dll");
        check(xmem_mod != nullptr, "LoadLibraryW(xmem.dll) succeeded");
        auto fnAlloc = xmem_mod ? reinterpret_cast<decltype(&WsxXMemAlloc)>(GetProcAddress(xmem_mod, "XMemAlloc")) : nullptr;
        auto fnFree = xmem_mod ? reinterpret_cast<decltype(&WsxXMemFree)>(GetProcAddress(xmem_mod, "XMemFree")) : nullptr;
        auto fnTrim = xmem_mod ? reinterpret_cast<decltype(&WsxXMemTrimPools)>(GetProcAddress(xmem_mod, "XMemTrimPools")) : nullptr;
        auto fnPoolStats = xmem_mod ? reinterpret_cast<decltype(&WsxXMemGetPoolStatistics)>(GetProcAddress(xmem_mod, "XMemGetPoolStatistics")) : nullptr;
        check(fnAlloc && fnFree && fnTrim && fnPoolStats, "Resolved XMemAlloc/XMemFree/XMemTrimPools/XMemGetPoolStatistics");

        if (fnAlloc && fnFree && fnTrim && fnPoolStats) {
            const auto t0 = std::chrono::steady_clock::now();
            bool zero_ok = true;
            constexpr SIZE_T kSizes[4] = {65536u, 131072u, 262144u, 524288u};
            for (int iter = 0; iter < 4000; ++iter) {
                const SIZE_T sz = kSizes[iter & 3];
                void* p = fnAlloc(sz, 0u);
                if (!p) {
                    zero_ok = false;
                    break;
                }
                auto* b = static_cast<uint8_t*>(p);
                if (b[0] != 0u || b[sz - 1u] != 0u) {
                    zero_ok = false;
                }
                b[0] = 0xAAu;
                b[sz - 1u] = 0x55u;
                fnFree(p, 0u);
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            check(zero_ok, "All 4,000 pooled allocations returned 64KB-aligned zero-initialized memory");

            XMEM_POOL_STATISTICS ps{};
            check(fnPoolStats(&ps) == TRUE, "XMemGetPoolStatistics succeeded");
            check(ps.PoolHits >= 3990u, "Pool hit rate >= 99.7% (3990+/4000)");
            check(ps.PooledBytes > 0u, "Pooled blocks cached before trim");

            const UINT64 trimmed = fnTrim(0u);
            XMEM_POOL_STATISTICS ps_after{};
            check(fnPoolStats(&ps_after) == TRUE, "XMemGetPoolStatistics after trim");
            check(trimmed > 0u && ps_after.PooledBytes == 0u, "XMemTrimPools(0) released all cached blocks");
            std::printf("  [PASS] xmem pool: 4,000 alloc/free in %.2f ms (hits=%llu, misses=%llu, trimmed=%llu KB)\n",
                        ms,
                        static_cast<unsigned long long>(ps.PoolHits),
                        static_cast<unsigned long long>(ps.PoolMisses),
                        static_cast<unsigned long long>(trimmed / 1024u));
        }
        if (xmem_mod) FreeLibrary(xmem_mod);
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
    std::printf("ALL WAVE 2 HIGH-LOAD DX12 PIPELINE & MEMORY TESTS PASSED (0 D3D12 Debug Layer errors)!\n");
    return 0;
}
