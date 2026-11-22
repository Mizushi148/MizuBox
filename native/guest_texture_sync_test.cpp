// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Live RTX / D3D12 Debug Layer CTest for Part 3:
// Immediate UMA CPU-written guest-texture synchronization before EVERY ExecuteCommandLists:
// - Test A: CreateCommittedResource (DEFAULT Texture2D, initial state PIXEL_SHADER_RESOURCE):
//     Write Red to tex->GetGPUVirtualAddress() -> Submit #1 -> IMMEDIATELY overwrite same VA
//     with Green (same frame, no Present, no wait!) -> Submit #2 -> verify Readback #1 == Red
//     and Readback #2 == Green (proves zero 1-frame lag & zero pipelined race!).
// - Test B: CreateHeap + CreatePlacedResource (DEFAULT Texture2D with console 4KB_S swizzle):
//     Swizzle 64x64 pattern via xg_xs.dll CopyIntoSubresource directly into tex->GetGPUVirtualAddress()
//     -> Submit -> verify GPU readback matches original linear pattern 100% pixel-exact!
// - Verify D3D12XboxGetGuestTextureSyncStats telemetry & ZERO D3D12 Debug Layer errors.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "include/xbox/d3d12_xs.h"
#include "include/xbox/xg_xs.h"

// Undefine C++ macro helpers from d3d12_xs.h where we use raw COM interfaces cleanly
#undef ID3D12Device8
#undef ID3D12Device2
#undef ID3D12CommandQueue

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                                     \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "[guest_texture_sync] FAIL (%s:%d): %s\n",                  \
                         __FILE__, __LINE__, (msg));                                         \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

ComPtr<ID3D12Resource> create_readback_buffer(ID3D12Device* dev, UINT64 size) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buf;
    if (FAILED(dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&buf)))) {
        return nullptr;
    }
    return buf;
}

} // namespace

int main() {
    // Enable D3D12 Debug Layer before device creation
    ComPtr<ID3D12Debug> debug_ctrl;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_ctrl))) && debug_ctrl) {
        debug_ctrl->EnableDebugLayer();
    }

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = D3D12_SDK_VERSION;
    ComPtr<ID3D12Device> device;
    HRESULT hr = D3D12XboxCreateDevice(nullptr, &params, IID_PPV_ARGS(&device));
    CHECK(SUCCEEDED(hr) && device, "D3D12XboxCreateDevice succeeded");
    if (!device) return 1;

    ComPtr<ID3D12InfoQueue> info_queue;
    device.As(&info_queue);

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    hr = device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue));
    CHECK(SUCCEEDED(hr) && queue, "CreateCommandQueue succeeded");
    if (!queue) return 1;

    ComPtr<ID3D12Fence> fence;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    CHECK(SUCCEEDED(hr) && fence, "CreateFence succeeded");
    HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(fence_event != nullptr, "CreateEventW succeeded");
    UINT64 fence_val = 0;

    auto wait_gpu = [&]() {
        ++fence_val;
        CHECK(SUCCEEDED(queue->Signal(fence.Get(), fence_val)), "queue->Signal");
        if (fence->GetCompletedValue() < fence_val) {
            CHECK(SUCCEEDED(fence->SetEventOnCompletion(fence_val, fence_event)), "SetEventOnCompletion");
            CHECK(WaitForSingleObject(fence_event, 5000) == WAIT_OBJECT_0, "GPU fence wait");
        }
    };

    constexpr UINT kW = 64;
    constexpr UINT kH = 64;
    constexpr UINT kRowPitch = 256; // 64 * 4 = 256 (already 256B aligned)
    constexpr UINT64 kTexBytes = static_cast<UINT64>(kRowPitch) * kH;

    // =========================================================================
    // TEST A: CreateCommittedResource DEFAULT Texture2D (LINEAR UMA write)
    // Back-to-back submits within the SAME frame (Red -> Submit 1 -> Green -> Submit 2)
    // =========================================================================
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kW;
        td.Height = kH;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        ComPtr<ID3D12Resource> tex;
        hr = device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            nullptr, IID_PPV_ARGS(&tex));
        CHECK(SUCCEEDED(hr) && tex, "Test A: CreateCommittedResource Texture2D");

        const D3D12_GPU_VIRTUAL_ADDRESS guest_va = tex ? tex->GetGPUVirtualAddress() : 0;
        CHECK(guest_va != 0, "Test A: Texture2D::GetGPUVirtualAddress returned CPU-writable UMA VA");

        auto rb1 = create_readback_buffer(device.Get(), kTexBytes);
        auto rb2 = create_readback_buffer(device.Get(), kTexBytes);
        CHECK(rb1 && rb2, "Test A: Created readback buffers");

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, nullptr);

        ComPtr<ID3D12CommandAllocator> alloc1, alloc2;
        ComPtr<ID3D12GraphicsCommandList> cmd1, cmd2;
        CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc1))), "alloc1");
        CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc2))), "alloc2");
        CHECK(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc1.Get(), nullptr, IID_PPV_ARGS(&cmd1))), "cmd1");
        CHECK(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc2.Get(), nullptr, IID_PPV_ARGS(&cmd2))), "cmd2");

        auto record_copy = [&](ID3D12GraphicsCommandList* cl, ID3D12Resource* dst_rb) {
            D3D12_RESOURCE_BARRIER b1{};
            b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b1.Transition.pResource = tex.Get();
            b1.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b1.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            b1.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            cl->ResourceBarrier(1, &b1);

            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = dst_rb;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = fp;

            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = tex.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;

            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

            D3D12_RESOURCE_BARRIER b2 = b1;
            b2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b2.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            cl->ResourceBarrier(1, &b2);
            CHECK(SUCCEEDED(cl->Close()), "cmd->Close");
        };

        record_copy(cmd1.Get(), rb1.Get());
        record_copy(cmd2.Get(), rb2.Get());

        if (guest_va != 0) {
            auto* pixels = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(guest_va));

            // CPU write #1: Red (0xFF0000FF = R:255, G:0, B:0, A:255) -> Submit #1
            for (UINT i = 0; i < kW * kH; ++i) pixels[i] = 0xFF0000FFu;
            ID3D12CommandList* l1[] = {cmd1.Get()};
            queue->ExecuteCommandLists(1, l1);

            // IMMEDIATELY inside the same frame (NO Present, NO GPU wait):
            // CPU write #2: Green (0xFF00FF00 = R:0, G:255, B:0, A:255) -> Submit #2
            for (UINT i = 0; i < kW * kH; ++i) pixels[i] = 0xFF00FF00u;
            ID3D12CommandList* l2[] = {cmd2.Get()};
            queue->ExecuteCommandLists(1, l2);

            wait_gpu();

            auto verify_solid = [&](ID3D12Resource* rb, uint32_t expected, const char* label) {
                void* p = nullptr;
                D3D12_RANGE rr{0, static_cast<SIZE_T>(kTexBytes)};
                CHECK(SUCCEEDED(rb->Map(0, &rr, &p)) && p, label);
                if (p) {
                    const auto* got = static_cast<const uint32_t*>(p);
                    UINT mismatches = 0;
                    for (UINT i = 0; i < kW * kH; ++i) {
                        if (got[i] != expected) ++mismatches;
                    }
                    if (mismatches != 0) {
                        std::fprintf(stderr, "[guest_texture_sync] %s: %u mismatches (first=0x%08X expected=0x%08X)\n",
                                     label, mismatches, got[0], expected);
                    }
                    CHECK(mismatches == 0, label);
                    D3D12_RANGE wr{0, 0};
                    rb->Unmap(0, &wr);
                }
            };

            verify_solid(rb1.Get(), 0xFF0000FFu, "Test A Submit #1 readback == Red (immediate)");
            verify_solid(rb2.Get(), 0xFF00FF00u, "Test A Submit #2 readback == Green (same-frame update)");
        }
    }

    // =========================================================================
    // TEST B: CreateHeap + CreatePlacedResource with console 4KB_S swizzle via xg_xs.dll
    // =========================================================================
    {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kW;
        td.Height = kH;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        // Pass Xbox console swizzle layout 4KB_S directly in Layout!
        td.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(XG_SWIZZLE_MODE_4KB_S);

        const D3D12_RESOURCE_ALLOCATION_INFO alloc_info = device->GetResourceAllocationInfo(0, 1, &td);
        D3D12_HEAP_DESC hdesc{};
        hdesc.SizeInBytes = (alloc_info.SizeInBytes < 65536u) ? 65536u : alloc_info.SizeInBytes;
        hdesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hdesc.Alignment = alloc_info.Alignment;
        hdesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;

        ComPtr<ID3D12Heap> heap;
        hr = device->CreateHeap(&hdesc, IID_PPV_ARGS(&heap));
        CHECK(SUCCEEDED(hr) && heap, "Test B: CreateHeap succeeded");

        ComPtr<ID3D12Resource> placed_tex;
        if (heap) {
            hr = device->CreatePlacedResource(
                heap.Get(), 0, &td, D3D12_RESOURCE_STATE_COMMON,
                nullptr, IID_PPV_ARGS(&placed_tex));
            CHECK(SUCCEEDED(hr) && placed_tex, "Test B: CreatePlacedResource (4KB_S) succeeded");
        }

        const D3D12_GPU_VIRTUAL_ADDRESS placed_va = placed_tex ? placed_tex->GetGPUVirtualAddress() : 0;
        CHECK(placed_va != 0, "Test B: Placed Texture2D GetGPUVirtualAddress non-zero");

        // Prepare deterministic 64x64 RGBA8 gradient+checker pattern
        std::vector<uint32_t> linear_src(kW * kH);
        for (UINT y = 0; y < kH; ++y) {
            for (UINT x = 0; x < kW; ++x) {
                const uint8_t r = static_cast<uint8_t>((x * 4u) & 0xFFu);
                const uint8_t g = static_cast<uint8_t>((y * 4u) & 0xFFu);
                const uint8_t b = static_cast<uint8_t>((((x >> 3) ^ (y >> 3)) & 1u) ? 0xE0u : 0x20u);
                const uint8_t a = 0xFFu;
                linear_src[y * kW + x] =
                    static_cast<uint32_t>(r) |
                    (static_cast<uint32_t>(g) << 8) |
                    (static_cast<uint32_t>(b) << 16) |
                    (static_cast<uint32_t>(a) << 24);
            }
        }

        // Swizzle into guest UMA VA using xg_xs.dll (exact Xbox title workflow!)
        XG_TEXTURE2D_DESC xg_desc{};
        xg_desc.Width = kW;
        xg_desc.Height = kH;
        xg_desc.MipLevels = 1;
        xg_desc.ArraySize = 1;
        xg_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        xg_desc.SampleCount = 1;
        xg_desc.SwizzleMode = XG_SWIZZLE_MODE_4KB_S;

        ComPtr<IXGTextureAddressComputer> xg_comp;
        CHECK(SUCCEEDED(XGCreateTexture2DComputer(&xg_desc, reinterpret_cast<void**>(xg_comp.GetAddressOf()))) && xg_comp,
              "Test B: XGCreateTexture2DComputer(4KB_S)");

        if (placed_va != 0 && xg_comp) {
            void* guest_ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(placed_va));
            CHECK(SUCCEEDED(xg_comp->CopyIntoSubresource(
                      guest_ptr, 0, 0, linear_src.data(), kRowPitch, static_cast<UINT>(kTexBytes))),
                  "Test B: xg_xs CopyIntoSubresource(4KB_S) -> guest VA");
            CHECK(std::memcmp(guest_ptr, linear_src.data(), static_cast<size_t>(kTexBytes)) != 0,
                  "Test B: Guest VA contains genuine 4KB_S swizzled memory");

            auto rb3 = create_readback_buffer(device.Get(), kTexBytes);
            CHECK(rb3 != nullptr, "Test B: Readback buffer");

            D3D12_RESOURCE_DESC clean_td = td;
            clean_td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
            device->GetCopyableFootprints(&clean_td, 0, 1, 0, &fp, nullptr, nullptr, nullptr);

            ComPtr<ID3D12CommandAllocator> alloc3;
            ComPtr<ID3D12GraphicsCommandList> cmd3;
            CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc3))), "alloc3");
            CHECK(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc3.Get(), nullptr, IID_PPV_ARGS(&cmd3))), "cmd3");

            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = rb3.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = fp;

            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = placed_tex.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;

            cmd3->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            CHECK(SUCCEEDED(cmd3->Close()), "cmd3->Close");

            ID3D12CommandList* l3[] = {cmd3.Get()};
            queue->ExecuteCommandLists(1, l3);
            wait_gpu();

            void* p = nullptr;
            D3D12_RANGE rr{0, static_cast<SIZE_T>(kTexBytes)};
            CHECK(SUCCEEDED(rb3->Map(0, &rr, &p)) && p, "Test B: Map readback");
            if (p) {
                const auto* got = static_cast<const uint32_t*>(p);
                UINT mismatches = 0;
                for (UINT i = 0; i < kW * kH; ++i) {
                    if (got[i] != linear_src[i]) ++mismatches;
                }
                if (mismatches != 0) {
                    std::fprintf(stderr, "[guest_texture_sync] Test B: %u pixel mismatches after 4KB_S deswizzle!\n",
                                 mismatches);
                }
                CHECK(mismatches == 0, "Test B: Placed 4KB_S GPU readback 100% pixel-exact across 4096 pixels");
                D3D12_RANGE wr{0, 0};
                rb3->Unmap(0, &wr);
            }
        }
    }

    // Check telemetry stats
    D3D12XBOX_GUEST_TEXTURE_STATS stats{};
    CHECK(SUCCEEDED(D3D12XboxGetGuestTextureSyncStats(device.Get(), &stats)), "D3D12XboxGetGuestTextureSyncStats");
    CHECK(stats.backing_allocations >= 2, "stats.backing_allocations >= 2");
    CHECK(stats.sync_flushes >= 3, "stats.sync_flushes >= 3");
    CHECK(stats.textures_uploaded >= 3, "stats.textures_uploaded >= 3");
    CHECK(stats.deswizzled_uploads >= 1, "stats.deswizzled_uploads >= 1");
    CHECK(stats.bytes_uploaded >= kTexBytes * 3u, "stats.bytes_uploaded >= 49152");

    // Verify ZERO D3D12 Debug Layer errors or corruption messages
    UINT debug_errors = 0;
    if (info_queue) {
        const UINT64 num = info_queue->GetNumStoredMessages();
        for (UINT64 i = 0; i < num; ++i) {
            SIZE_T len = 0;
            info_queue->GetMessage(i, nullptr, &len);
            if (len == 0) continue;
            std::vector<uint8_t> buf(len);
            auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(info_queue->GetMessage(i, msg, &len))) {
                if (msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
                    msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR) {
                    std::fprintf(stderr, "[D3D12 Debug ERROR #%d]: %s\n",
                                 static_cast<int>(msg->ID), msg->pDescription);
                    ++debug_errors;
                }
            }
        }
    }
    CHECK(debug_errors == 0, "ZERO D3D12 Debug Layer errors");

    CloseHandle(fence_event);

    if (g_failures != 0) {
        std::fprintf(stderr, "[guest_texture_sync] FAILED with %d error(s)\n", g_failures);
        return 1;
    }

    std::printf(
        "[guest_texture_sync] ALL PASSED (flushes=%llu uploads=%llu deswizzled=%llu bytes=%llu d3d12_errors=%u)\n",
        static_cast<unsigned long long>(stats.sync_flushes),
        static_cast<unsigned long long>(stats.textures_uploaded),
        static_cast<unsigned long long>(stats.deswizzled_uploads),
        static_cast<unsigned long long>(stats.bytes_uploaded),
        debug_errors);
    return 0;
}
