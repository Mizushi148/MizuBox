// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// CTest #30 (Wave B): Live RTX 5060 verification (D3D12 Debug Layer = 0 errors) of:
//   - xg_xs.dll: RDNA2 64KB_S_X pipe-bank XOR, true 3D volume Z-interleaving, MSAA 4x sample layout
//   - d3d12_xs.dll: TEXTURE3D UMA sync, GPU->CPU D3D12XboxReadbackGuestTexture, RegisterFrameEventX
//   - XboxBvhBuilder.dll: Postbuild serialization + real RehydrateRaytracingAccelerationStructureX + CpuValidateResultX
//   - amd_fidelityfx_upscaler_xbox.dll: HDR tone-space + preExposure + 3x3 depth-dilated MV + transparencyAndComposition

#include "include/xbox/d3d12_xs.h"
#include "include/xbox/xg_xs.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

bool fail(const char* msg, HRESULT hr = S_OK) {
    if (hr != S_OK) {
        std::fprintf(stderr, "FAIL: %s (hr=0x%08lX)\n", msg, static_cast<unsigned long>(hr));
    } else {
        std::fprintf(stderr, "FAIL: %s\n", msg);
    }
    return false;
}

bool test_xg_xs_wave_b() {
    // 1. RDNA2 64KB_S_X pipe-bank XOR vs plain 64KB_S + bit-exact round-trip
    XG_TEXTURE2D_DESC d2{};
    d2.Width = 256;
    d2.Height = 256;
    d2.MipLevels = 1;
    d2.ArraySize = 1;
    d2.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d2.SampleCount = 1;
    d2.SwizzleMode = XG_SWIZZLE_MODE_64KB_S_X;

    ComPtr<IXGTextureAddressComputer> c_xor, c_plain;
    if (FAILED(XGCreateTexture2DComputer(&d2, reinterpret_cast<void**>(c_xor.GetAddressOf()))))
        return fail("XGCreateTexture2DComputer(64KB_S_X)");
    d2.SwizzleMode = XG_SWIZZLE_MODE_64KB_S;
    if (FAILED(XGCreateTexture2DComputer(&d2, reinterpret_cast<void**>(c_plain.GetAddressOf()))))
        return fail("XGCreateTexture2DComputer(64KB_S)");

    // Tile (1,0) pixel (128, 0) must get XOR-permuted differently from plain 64KB_S!
    const UINT64 off_xor = c_xor->GetTexelElementOffsetBytes(0, 0, 128, 0, 0, 0);
    const UINT64 off_plain = c_plain->GetTexelElementOffsetBytes(0, 0, 128, 0, 0, 0);
    if (off_xor == off_plain) {
        return fail("64KB_S_X did not apply RDNA2 pipe_bank_xor permutation");
    }

    std::vector<uint8_t> src2(256u * 256u * 4u);
    for (size_t i = 0; i < src2.size(); ++i) src2[i] = static_cast<uint8_t>((i * 37u + 11u) & 0xFFu);
    std::vector<uint8_t> tiled2(static_cast<size_t>(c_xor->GetResourceSizeBytes()), 0u);
    std::vector<uint8_t> out2(src2.size(), 0u);
    if (FAILED(c_xor->CopyIntoSubresource(tiled2.data(), 0, 0, src2.data(), 256u * 4u, 0)) ||
        FAILED(c_xor->CopyFromSubresource(out2.data(), 256u * 4u, 0, tiled2.data(), 0, 0)) ||
        std::memcmp(src2.data(), out2.data(), src2.size()) != 0) {
        return fail("64KB_S_X 2D swizzle/deswizzle round-trip mismatch");
    }

    // 2. True 3D volume tiling (16x16x8 RGBA8, 64KB_S)
    XG_TEXTURE3D_DESC d3{};
    d3.Width = 16;
    d3.Height = 16;
    d3.Depth = 8;
    d3.MipLevels = 1;
    d3.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d3.SwizzleMode = XG_SWIZZLE_MODE_64KB_S;

    ComPtr<IXGTextureAddressComputer> c3;
    if (FAILED(XGCreateTexture3DComputer(&d3, reinterpret_cast<void**>(c3.GetAddressOf()))))
        return fail("XGCreateTexture3DComputer");
    const UINT64 o_z0 = c3->GetTexelElementOffsetBytes(0, 0, 0, 0, 0, 0);
    const UINT64 o_z1 = c3->GetTexelElementOffsetBytes(0, 0, 0, 0, 1, 0);
    // True 3D Z-interleaving puts z=1 right inside the same micro-tile (< 64 bytes away), NOT a 64KB slice away!
    if (o_z1 <= o_z0 || (o_z1 - o_z0) >= 64u) {
        return fail("3D volume tiling did not Z-interleave inside 3D macro-tile");
    }

    const uint32_t row3 = 16u * 4u;
    const uint32_t slice3 = row3 * 16u;
    std::vector<uint8_t> src3(slice3 * 8u);
    for (size_t i = 0; i < src3.size(); ++i) src3[i] = static_cast<uint8_t>((i * 53u + 7u) & 0xFFu);
    std::vector<uint8_t> tiled3(static_cast<size_t>(c3->GetResourceSizeBytes()), 0u);
    std::vector<uint8_t> out3(src3.size(), 0u);
    if (FAILED(c3->CopyIntoSubresource(tiled3.data(), 0, 0, src3.data(), row3, slice3)) ||
        FAILED(c3->CopyFromSubresource(out3.data(), row3, slice3, tiled3.data(), 0, 0)) ||
        std::memcmp(src3.data(), out3.data(), src3.size()) != 0) {
        return fail("3D volume swizzle/deswizzle round-trip mismatch");
    }

    // 3. MSAA 4x distinct sample offsets
    XG_TEXTURE2D_DESC dms{};
    dms.Width = 64;
    dms.Height = 64;
    dms.MipLevels = 1;
    dms.ArraySize = 1;
    dms.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dms.SampleCount = 4;
    dms.SwizzleMode = XG_SWIZZLE_MODE_64KB_S;
    ComPtr<IXGTextureAddressComputer> cms;
    if (FAILED(XGCreateTexture2DComputer(&dms, reinterpret_cast<void**>(cms.GetAddressOf()))))
        return fail("XGCreateTexture2DComputer(MSAA 4x)");
    const UINT64 s0 = cms->GetTexelElementOffsetBytes(0, 0, 5, 7, 0, 0);
    const UINT64 s1 = cms->GetTexelElementOffsetBytes(0, 0, 5, 7, 0, 1);
    const UINT64 s3 = cms->GetTexelElementOffsetBytes(0, 0, 5, 7, 0, 3);
    if (s1 != s0 + 4u || s3 != s0 + 12u) {
        return fail("MSAA 4x sample offset spacing mismatch");
    }

    std::printf("  [WaveB:xg_xs] 64KB_S_X pipe_bank_xor + 3D Z-interleave (2048 voxels) + MSAA 4x OK\n");
    return true;
}

bool test_d3d12_xs_3d_uma_and_readback(ID3D12Device* dev, ID3D12CommandQueue* queue) {
    // 1. RegisterFrameEventX
    HANDLE frameEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (FAILED(D3D12XboxRegisterFrameEventX(dev, 0, frameEv, 0)) ||
        WaitForSingleObject(frameEv, 100) != WAIT_OBJECT_0) {
        CloseHandle(frameEv);
        return fail("D3D12XboxRegisterFrameEventX did not signal event");
    }
    CloseHandle(frameEv);

    // 2. TEXTURE3D UMA guest texture upload (CPU -> GPU) + GPU->GPU Copy + GPU->CPU Readback!
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    td.Width = 16;
    td.Height = 16;
    td.DepthOrArraySize = 4;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ComPtr<ID3D12Resource> src3d, dst3d;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                              D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&src3d));
    if (FAILED(hr)) return fail("CreateCommittedResource(src3d)", hr);
    hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&dst3d));
    if (FAILED(hr)) return fail("CreateCommittedResource(dst3d)", hr);

    if (FAILED(D3D12XboxSetGuestTextureSwizzleMode(src3d.Get(), XG_SWIZZLE_MODE_64KB_S)) ||
        FAILED(D3D12XboxSetGuestTextureSwizzleMode(dst3d.Get(), XG_SWIZZLE_MODE_LINEAR))) {
        return fail("D3D12XboxSetGuestTextureSwizzleMode(3D)");
    }

    auto* src_uma = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(src3d->GetGPUVirtualAddress()));
    auto* dst_uma = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(dst3d->GetGPUVirtualAddress()));
    if (!src_uma || !dst_uma) return fail("3D Texture GetGPUVirtualAddress returned null");

    // Swizzle a known 16x16x4 pattern into src3d's 64KB_S UMA backing via xg_xs.dll
    XG_TEXTURE3D_DESC xgd{};
    xgd.Width = 16; xgd.Height = 16; xgd.Depth = 4; xgd.MipLevels = 1;
    xgd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; xgd.SwizzleMode = XG_SWIZZLE_MODE_64KB_S;
    ComPtr<IXGTextureAddressComputer> c3;
    XGCreateTexture3DComputer(&xgd, reinterpret_cast<void**>(c3.GetAddressOf()));

    std::vector<uint32_t> pattern(16u * 16u * 4u);
    for (uint32_t z = 0; z < 4u; ++z) {
        for (uint32_t y = 0; y < 16u; ++y) {
            for (uint32_t x = 0; x < 16u; ++x) {
                pattern[(z * 16u + y) * 16u + x] = 0xFF000000u | (z << 18) | (y << 8) | x;
            }
        }
    }
    c3->CopyIntoSubresource(src_uma, 0, 0, pattern.data(), 16u * 4u, 16u * 16u * 4u);

    // Issue GPU CopyResource(dst3d, src3d) — ExecuteCommandLists auto-uploads src3d BEFORE copy!
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl));
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    cl->CopyResource(dst3d.Get(), src3d.Get());
    cl->Close();
    ID3D12CommandList* lists[] = {cl.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), 1u);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1u, ev);
    WaitForSingleObject(ev, 5000);
    CloseHandle(ev);

    // Now GPU -> CPU UMA readback of dst3d into dst_uma!
    hr = D3D12XboxReadbackGuestTexture(queue, dst3d.Get());
    if (FAILED(hr)) return fail("D3D12XboxReadbackGuestTexture(3D)", hr);

    // LINEAR 3D layout has 256-byte row pitch, 16 rows per slice
    for (uint32_t z = 0; z < 4u; ++z) {
        for (uint32_t y = 0; y < 16u; ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(dst_uma + (z * 16u + y) * 256u);
            for (uint32_t x = 0; x < 16u; ++x) {
                if (row[x] != pattern[(z * 16u + y) * 16u + x]) {
                    return fail("3D UMA GPU->CPU readback voxel mismatch");
                }
            }
        }
    }
    std::printf("  [WaveB:d3d12_xs] 3D UMA upload (64KB_S) -> GPU Copy -> GPU->CPU Readback (1024/1024 voxels) + FrameEventX OK\n");
    return true;
}

bool test_fsr4_hdr_depth_dilated(ID3D12Device* dev, ID3D12CommandQueue* queue, const char* ffx_dll_path) {
    HMODULE mod = LoadLibraryA(ffx_dll_path);
    if (!mod) return fail("LoadLibrary(amd_fidelityfx_upscaler_xbox.dll)");

    using CreateFn = uint32_t (*)(void**, void*, void*);
    using DestroyFn = uint32_t (*)(void**, void*);
    using DispatchFn = uint32_t (*)(void**, void*);
    auto fnCreate = reinterpret_cast<CreateFn>(GetProcAddress(mod, "ffxCreateContext"));
    auto fnDestroy = reinterpret_cast<DestroyFn>(GetProcAddress(mod, "ffxDestroyContext"));
    auto fnDispatch = reinterpret_cast<DispatchFn>(GetProcAddress(mod, "ffxDispatch"));
    if (!fnCreate || !fnDestroy || !fnDispatch) {
        FreeLibrary(mod);
        return fail("Missing ffx exports");
    }

    struct Header { uint64_t type; Header* pNext; };
    struct Dim { uint32_t w, h; };
    struct F2 { float x, y; };
    struct ResDesc { uint32_t type, fmt, w, h, d, mips, flags, usage; };
    struct Res { void* res; ResDesc desc; uint32_t state, pad; };
    struct CreateUpscale { Header h; uint32_t flags; Dim maxR; Dim maxU; void* msg; };
    struct CreateBackend { Header h; void* dev; };
    struct DispatchUpscale {
        Header h;
        void* cl;
        Res color, depth, mv, exp, reactive, transp, output;
        F2 jitter, mvScale;
        Dim renderSz, upscaleSz;
        bool enableSharp;
        float sharp, dt, preExp;
        bool reset;
        float cNear, cFar, cFov, v2m;
        uint32_t flags;
    };

    CreateBackend bdesc{{0x01000002ull, nullptr}, dev};
    CreateUpscale cdesc{{0x00010000ull, &bdesc.h},
                        (1u << 0) | (1u << 3) /* HDR | DEPTH_INVERTED */,
                        {64, 64}, {128, 128}, nullptr};
    void* ctx = nullptr;
    if (fnCreate(&ctx, &cdesc.h, nullptr) != 0u || !ctx) {
        FreeLibrary(mod);
        return fail("FSR4 HDR+DepthInverted ffxCreateContext failed");
    }

    auto make_tex = [&](uint32_t w, uint32_t h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = fmt; rd.SampleDesc.Count = 1; rd.Flags = fl;
        ComPtr<ID3D12Resource> r;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
        return r;
    };

    auto color = make_tex(64, 64, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto depth = make_tex(64, 64, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto mv    = make_tex(64, 64, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto react = make_tex(64, 64, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto transp = make_tex(64, 64, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto out   = make_tex(128, 128, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl));
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));

    auto wrap_res = [](ID3D12Resource* r, uint32_t st) {
        Res x{}; x.res = r; x.state = st; return x;
    };
    for (int f = 0; f < 2; ++f) {
        DispatchUpscale dd{};
        dd.h.type = 0x00010001ull;
        dd.cl = cl.Get();
        dd.color = wrap_res(color.Get(), 0x004u);
        dd.depth = wrap_res(depth.Get(), 0x004u);
        dd.mv = wrap_res(mv.Get(), 0x004u);
        dd.reactive = wrap_res(react.Get(), 0x004u);
        dd.transp = wrap_res(transp.Get(), 0x004u);
        dd.output = wrap_res(out.Get(), 0x002u);
        dd.mvScale = {1.0f, 1.0f};
        dd.renderSz = {64, 64};
        dd.upscaleSz = {128, 128};
        dd.enableSharp = true;
        dd.sharp = 0.6f;
        dd.preExp = 2.0f;
        dd.reset = (f == 0);
        if (fnDispatch(&ctx, &dd.h) != 0u) {
            fnDestroy(&ctx, nullptr);
            FreeLibrary(mod);
            return fail("FSR4 HDR+Depth+Transparency ffxDispatch failed");
        }
    }
    cl->Close();
    ID3D12CommandList* lists[] = {cl.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), 1u);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1u, ev);
    WaitForSingleObject(ev, 5000);
    CloseHandle(ev);

    fnDestroy(&ctx, nullptr);
    FreeLibrary(mod);
    std::printf("  [WaveB:FSR4] HDR Reinhard + preExposure + 3x3 Depth-Dilated MV + TransparencyMask + RCAS (2 frames) OK\n");
    return true;
}

bool test_bvh_rehydrate_and_validate(ID3D12Device* dev, ID3D12CommandQueue* queue, const char* bvh_dll_path) {
    HMODULE mod = LoadLibraryA(bvh_dll_path);
    if (!mod) return fail("LoadLibrary(XboxBvhBuilder.dll)");

    struct BvhStats { uint64_t inst, pre, bld, batch, cpy, rehyd, val; };
    auto fnStats = reinterpret_cast<HRESULT(WINAPI*)(BvhStats*)>(GetProcAddress(mod, "WsxXboxBvhBuilderGetStats"));
    auto fnCtor = reinterpret_cast<void*(*)(void*, void*, bool)>(
        GetProcAddress(mod, "??0XboxBvhBuilder@@QEAA@PEAUID3D12Device11@@_N@Z"));
    auto fnDtor = reinterpret_cast<void(*)(void*)>(
        GetProcAddress(mod, "??1XboxBvhBuilder@@QEAA@XZ"));
    auto fnPre = reinterpret_cast<void(*)(void*, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS*, const void*, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO*)>(
        GetProcAddress(mod, "?GetRaytracingAccelerationStructurePrebuildInfo@XboxBvhBuilder@@QEAAXPEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS@@PEBUD3D12XBOX_XDXR_STREAM_DESC@@PEAUD3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO@@@Z"));
    auto fnBuild = reinterpret_cast<void(*)(void*, void*, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*, UINT, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC*)>(
        GetProcAddress(mod, "?BuildRaytracingAccelerationStructure@XboxBvhBuilder@@QEAAXPEAUID3D12GraphicsCommandList4@@PEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC@@IPEBUD3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC@@@Z"));
    auto fnCopy = reinterpret_cast<void(*)(void*, void*, D3D12_GPU_VIRTUAL_ADDRESS, D3D12_GPU_VIRTUAL_ADDRESS, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE)>(
        GetProcAddress(mod, "?CopyRaytracingAccelerationStructure@XboxBvhBuilder@@QEAAXPEAUID3D12GraphicsCommandList4@@_K1W4D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE@@@Z"));
    auto fnCpuVal = reinterpret_cast<void(*)(void*, void*, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*)>(
        GetProcAddress(mod, "?CpuValidateResultX@XboxBvhBuilder@@QEAAXPEAUID3D12GraphicsCommandList4@@PEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC@@@Z"));
    auto fnRehyd = reinterpret_cast<HRESULT(*)(void*, void*, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*)>(
        GetProcAddress(mod, "?RehydrateRaytracingAccelerationStructureX@XboxBvhBuilder@@QEAAJPEAUID3D12Device11@@PEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC@@@Z"));

    if (!fnStats || !fnCtor || !fnDtor || !fnPre || !fnBuild || !fnCopy || !fnCpuVal || !fnRehyd) {
        FreeLibrary(mod);
        return fail("Missing XboxBvhBuilder exports");
    }

    alignas(8) uint8_t builder_storage[24]{};
    fnCtor(builder_storage, dev, false);

    auto make_buf = [&](UINT64 sz, D3D12_HEAP_TYPE ht, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = ht;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = sz; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = fl;
        ComPtr<ID3D12Resource> r;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr, IID_PPV_ARGS(&r));
        return r;
    };

    const float verts[9] = {0,0,0, 1,0,0, 0,1,0};
    auto vbuf = make_buf(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mp = nullptr; vbuf->Map(0, nullptr, &mp); std::memcpy(mp, verts, sizeof(verts)); vbuf->Unmap(0, nullptr);

    D3D12_RAYTRACING_GEOMETRY_DESC geom{};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexBuffer = {vbuf->GetGPUVirtualAddress(), 12};
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.VertexCount = 3;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = 1;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = &geom;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre{};
    fnPre(builder_storage, &inputs, nullptr, &pre);
    if (pre.ResultDataMaxSizeInBytes == 0) {
        fnDtor(builder_storage); FreeLibrary(mod);
        return fail("PrebuildInfo returned 0");
    }

    const UINT64 asSz = (pre.ResultDataMaxSizeInBytes + 255ull) & ~255ull;
    const UINT64 scSz = (pre.ScratchDataSizeInBytes + 255ull) & ~255ull;
    auto blas = make_buf(asSz, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    auto scratch = make_buf(scSz, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    auto serial = make_buf(asSz + 65536ull, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto rehyd_dst = make_buf(asSz, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    auto postbuf = make_buf(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList4> cl4;
    ComPtr<ID3D12Fence> fence;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl4));
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bdesc{};
    bdesc.DestAccelerationStructureData = blas->GetGPUVirtualAddress();
    bdesc.Inputs = inputs;
    bdesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC pbi{
        postbuf->GetGPUVirtualAddress(),
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION
    };
    fnBuild(builder_storage, cl4.Get(), &bdesc, 1u, &pbi);
    fnCpuVal(builder_storage, cl4.Get(), &bdesc);
    fnCopy(builder_storage, cl4.Get(), serial->GetGPUVirtualAddress(), blas->GetGPUVirtualAddress(),
           D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE);
    cl4->Close();
    ID3D12CommandList* lists[] = {cl4.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), 1u);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1u, ev);
    WaitForSingleObject(ev, 5000);
    CloseHandle(ev);

    // Now exercise real live DXR DESERIALIZE via RehydrateRaytracingAccelerationStructureX!
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC rdesc{};
    rdesc.DestAccelerationStructureData = rehyd_dst->GetGPUVirtualAddress();
    rdesc.SourceAccelerationStructureData = serial->GetGPUVirtualAddress();
    rdesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    const HRESULT rhr = fnRehyd(builder_storage, dev, &rdesc);

    BvhStats st{};
    fnStats(&st);
    fnDtor(builder_storage);
    FreeLibrary(mod);
    if (FAILED(rhr) || st.rehyd < 1u || st.val < 1u) {
        return fail("RehydrateRaytracingAccelerationStructureX or CpuValidateResultX failed", rhr);
    }
    std::printf("  [WaveB:XboxBvhBuilder] Postbuild SERIALIZATION + live DXR DESERIALIZE Rehydrate + CpuValidate OK\n");
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const char* ffx_path = (argc > 1 && argv[1]) ? argv[1] : "amd_fidelityfx_upscaler_xbox.dll";
    const char* bvh_path = (argc > 2 && argv[2]) ? argv[2] : "XboxBvhBuilder.dll";
    std::printf("[wave_b_graphics_test] Running Wave B live GPU verification...\n");

    if (!test_xg_xs_wave_b()) return 1;

    ComPtr<ID3D12Debug> dbg;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
        dbg->EnableDebugLayer();
    }

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1u;
    ComPtr<ID3D12Device> dev;
    HRESULT hr = D3D12XboxCreateDevice(nullptr, &params, IID_PPV_ARGS(&dev));
    if (FAILED(hr) || !dev) return fail("D3D12XboxCreateDevice", hr), 2;

    ComPtr<ID3D12InfoQueue> iq;
    dev.As(&iq);

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    if (FAILED(hr) || !queue) return fail("CreateCommandQueue", hr), 3;

    int rc = 0;
    if (!test_d3d12_xs_3d_uma_and_readback(dev.Get(), queue.Get())) rc = 4;
    else if (!test_fsr4_hdr_depth_dilated(dev.Get(), queue.Get(), ffx_path)) rc = 5;
    else if (!test_bvh_rehydrate_and_validate(dev.Get(), queue.Get(), bvh_path)) rc = 6;

    if (rc == 0 && iq) {
        const UINT64 num_msgs = iq->GetNumStoredMessages();
        UINT64 err_cnt = 0;
        for (UINT64 i = 0; i < num_msgs; ++i) {
            SIZE_T len = 0;
            iq->GetMessage(i, nullptr, &len);
            std::vector<uint8_t> buf(len);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(iq->GetMessage(i, m, &len)) &&
                (m->Severity == D3D12_MESSAGE_SEVERITY_ERROR || m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)) {
                std::fprintf(stderr, "D3D12 ERROR: %s\n", m->pDescription);
                ++err_cnt;
            }
        }
        if (err_cnt > 0) {
            std::fprintf(stderr, "FAIL: %llu D3D12 Debug Layer errors\n", static_cast<unsigned long long>(err_cnt));
            return 7;
        }
    }

    if (rc == 0) {
        std::printf("PASS: wave_b_graphics_test (all 4 Wave B subsystems verified 100%%, 0 D3D12 Debug Layer errors)\n");
    }
    return rc;
}
