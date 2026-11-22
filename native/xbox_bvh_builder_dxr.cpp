// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Native Direct3D 12 (DXR Tier 1.1) Bridge for XboxBvhBuilder.dll:
// - Exports all 12 exact MSVC x64 mangled C++ symbols of Microsoft's XboxBvhBuilder.dll.
// - Matches XboxBvhBuilder's 24-byte instance footprint (safe for stack/in-place construction).
// - Transparently unwraps d3d12_xs.dll's WsxXboxDeviceAbiProxy / WsxXboxCommandListAbiProxy /
//   WsxD3D12CommandListProxy down to desktop ID3D12Device5 / ID3D12GraphicsCommandList4.
// - Routes BLAS/TLAS PrebuildInfo, Build, Batched2X Build, and Copy directly to native
//   desktop GPU hardware DXR Tier 1.1 (no @xbox.* RDNA2 compute shaders, no Vulkan).

#include "d3d12_xs_proxies.h"
#include "wsx_config.h"

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>

using Microsoft::WRL::ComPtr;

// Forward-declared Xbox GDK structs matching exact XboxBvhBuilder.dll symbol mangling
struct D3D12XBOX_XDXR_STREAM_DESC {
    UINT Flags;
    UINT Reserved;
};

struct D3D12XBOX_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2 {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC Desc;
    UINT NumPostbuildInfoDescs;
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pPostbuildInfoDescs;
};

struct WsxXboxBvhBridgeStats {
    uint64_t InstancesCreated;
    uint64_t PrebuildInfoCalls;
    uint64_t BuildCalls;
    uint64_t BatchedBuildCalls;
    uint64_t CopyCalls;
    uint64_t RehydrateCalls;
    uint64_t ValidateCalls;
};

namespace {

constexpr uint64_t kBridgeMagic = 0x5844525F42564831ull; // "1HVB_RDX"
constexpr UINT kDesktopBuildFlagsMask = 0x3Fu; // Standard DXR 1.1 build flags

struct GlobalBridgeCounters {
    std::atomic<uint64_t> instances_created{0};
    std::atomic<uint64_t> prebuild_calls{0};
    std::atomic<uint64_t> build_calls{0};
    std::atomic<uint64_t> batched_build_calls{0};
    std::atomic<uint64_t> copy_calls{0};
    std::atomic<uint64_t> rehydrate_calls{0};
    std::atomic<uint64_t> validate_calls{0};
};

GlobalBridgeCounters& g_counters() noexcept {
    static GlobalBridgeCounters c;
    return c;
}

ComPtr<ID3D12Device5> unwrap_to_desktop_device5(IUnknown* pDevice) noexcept {
    if (!pDevice) return nullptr;
    IUnknown* base = pDevice;
    // 1. Unwrap d3d12_xs.dll's Xbox binary ABI proxy (WsxXboxDeviceAbiProxy -> WsxD3D12DeviceProxy)
    ID3D12Device* xbox_unwrapped = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(__uuidof(wsx_d3d12::IWsxXboxDeviceAbiMarker),
                                          reinterpret_cast<void**>(&xbox_unwrapped))) &&
        xbox_unwrapped) {
        base = xbox_unwrapped; // Non-AddRef internal unwrap marker
    }
    ComPtr<ID3D12Device5> dev5;
    base->QueryInterface(IID_PPV_ARGS(dev5.GetAddressOf()));
    return dev5;
}

ComPtr<ID3D12GraphicsCommandList4> unwrap_to_desktop_cmdlist4(IUnknown* pCmdList) noexcept {
    if (!pCmdList) return nullptr;
    IUnknown* base = pCmdList;
    // 1. Unwrap d3d12_xs.dll's Xbox binary ABI command list proxy
    ID3D12GraphicsCommandList* xbox_unwrapped = nullptr;
    if (SUCCEEDED(pCmdList->QueryInterface(__uuidof(wsx_d3d12::IWsxXboxCommandListAbiMarker),
                                           reinterpret_cast<void**>(&xbox_unwrapped))) &&
        xbox_unwrapped) {
        base = xbox_unwrapped; // Non-AddRef internal unwrap marker
    }
    // 2. Unwrap WsxD3D12CommandListProxy -> raw desktop ID3D12GraphicsCommandList
    ID3D12GraphicsCommandList* raw_cl = nullptr;
    if (SUCCEEDED(base->QueryInterface(__uuidof(wsx_d3d12::IWsxD3D12CommandListProxyMarker),
                                       reinterpret_cast<void**>(&raw_cl))) &&
        raw_cl) {
        base = raw_cl; // Non-AddRef internal unwrap marker
    }
    ComPtr<ID3D12GraphicsCommandList4> cl4;
    base->QueryInterface(IID_PPV_ARGS(cl4.GetAddressOf()));
    return cl4;
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS sanitize_inputs(
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& in) noexcept {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS out = in;
    out.Flags = static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
        static_cast<UINT>(in.Flags) & kDesktopBuildFlagsMask);
    return out;
}

bool is_valid_va_or_zero(D3D12_GPU_VIRTUAL_ADDRESS va) noexcept {
    return va != 0ull;
}

} // namespace

struct XboxBvhBuilderImpl {
    ComPtr<ID3D12Device5> dev5;
    bool use_64bit_morton = false;
    bool allow_async_compute = false;
};

class XboxBvhBuilder {
public:
    XboxBvhBuilder(ID3D12Device11* pDevice, bool bAllowAsyncCompute);
    ~XboxBvhBuilder();

    void GetRaytracingAccelerationStructurePrebuildInfo(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* pDesc,
        const D3D12XBOX_XDXR_STREAM_DESC* pStreamDesc,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* pInfo);

    void BuildRaytracingAccelerationStructure(
        ID3D12GraphicsCommandList4* pCommandList,
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc,
        UINT NumPostbuildInfoDescs,
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pPostbuildInfoDescs);

    void BuildRaytracingAccelerationStructureBatched2X(
        ID3D12GraphicsCommandList6* pCommandList,
        UINT Count,
        const D3D12XBOX_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2* pDescs);

    void CopyRaytracingAccelerationStructure(
        ID3D12GraphicsCommandList4* pCommandList,
        D3D12_GPU_VIRTUAL_ADDRESS DestAccelerationStructureData,
        D3D12_GPU_VIRTUAL_ADDRESS SourceAccelerationStructureData,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE Mode);

    void CpuValidateResultX(
        ID3D12GraphicsCommandList4* pCommandList,
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc);

    HRESULT ValidateRehydrateAccelerationStructureBuildDesc(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc);

    HRESULT RehydrateRaytracingAccelerationStructureX(
        ID3D12Device11* pDevice,
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc);

    HRESULT RehydrateRaytracingAccelerationStructureBatchedX(
        ID3D12GraphicsCommandList8* pCommandList,
        UINT Count,
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDescs);

    HRESULT RehydrateRaytracingAccelerationStructureBatched2X(
        ID3D12GraphicsCommandList8* pCommandList,
        UINT Count,
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDescs,
        const UINT64* pMaxSizes);

    void Set64BitMortonCodes(bool bEnable);

private:
    XboxBvhBuilderImpl* impl_ = nullptr;
    uint64_t magic_ = 0;
    uint64_t reserved_ = 0;
};

static_assert(sizeof(XboxBvhBuilder) == 24,
              "XboxBvhBuilder footprint must stay <= 24 bytes to match Microsoft XboxBvhBuilder.dll");

XboxBvhBuilder::XboxBvhBuilder(ID3D12Device11* pDevice, bool bAllowAsyncCompute)
    : impl_(new (std::nothrow) XboxBvhBuilderImpl{}),
      magic_(kBridgeMagic),
      reserved_(0) {
    g_counters().instances_created.fetch_add(1, std::memory_order_relaxed);
    if (impl_) {
        impl_->allow_async_compute = bAllowAsyncCompute;
        impl_->dev5 = unwrap_to_desktop_device5(reinterpret_cast<IUnknown*>(pDevice));
    }
    wsx::trace_void("XboxBvhBuilder", "ctor(DXR1.1)",
                    "dev=%p unwrapped_dev5=%p async=%u",
                    pDevice, impl_ ? impl_->dev5.Get() : nullptr, bAllowAsyncCompute ? 1u : 0u);
}

XboxBvhBuilder::~XboxBvhBuilder() {
    if (magic_ == kBridgeMagic) {
        delete impl_;
        impl_ = nullptr;
        magic_ = 0;
    }
    wsx::trace_void("XboxBvhBuilder", "dtor(DXR1.1)", "ok");
}

void XboxBvhBuilder::GetRaytracingAccelerationStructurePrebuildInfo(
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* pDesc,
    const D3D12XBOX_XDXR_STREAM_DESC* /*pStreamDesc*/,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* pInfo) {
    g_counters().prebuild_calls.fetch_add(1, std::memory_order_relaxed);
    if (!pInfo) return;
    std::memset(pInfo, 0, sizeof(*pInfo));
    if (!pDesc || magic_ != kBridgeMagic || !impl_ || !impl_->dev5) return;

    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS clean = sanitize_inputs(*pDesc);
    impl_->dev5->GetRaytracingAccelerationStructurePrebuildInfo(&clean, pInfo);
    wsx::trace_void("XboxBvhBuilder", "GetPrebuildInfo(DXR1.1)",
                    "type=%u numDescs=%u result=%llu scratch=%llu update=%llu",
                    static_cast<UINT>(clean.Type), clean.NumDescs,
                    static_cast<unsigned long long>(pInfo->ResultDataMaxSizeInBytes),
                    static_cast<unsigned long long>(pInfo->ScratchDataSizeInBytes),
                    static_cast<unsigned long long>(pInfo->UpdateScratchDataSizeInBytes));
}

void XboxBvhBuilder::BuildRaytracingAccelerationStructure(
    ID3D12GraphicsCommandList4* pCommandList,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc,
    UINT NumPostbuildInfoDescs,
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pPostbuildInfoDescs) {
    g_counters().build_calls.fetch_add(1, std::memory_order_relaxed);
    if (!pCommandList || !pDesc) return;
    ComPtr<ID3D12GraphicsCommandList4> cl4 = unwrap_to_desktop_cmdlist4(reinterpret_cast<IUnknown*>(pCommandList));
    if (!cl4) return;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC clean = *pDesc;
    clean.Inputs = sanitize_inputs(pDesc->Inputs);
    cl4->BuildRaytracingAccelerationStructure(&clean, NumPostbuildInfoDescs, pPostbuildInfoDescs);
    wsx::trace_void("XboxBvhBuilder", "BuildAS(DXR1.1)",
                    "cl4=%p type=%u numDescs=%u dst=0x%llX scratch=0x%llX post=%u",
                    cl4.Get(), static_cast<UINT>(clean.Inputs.Type), clean.Inputs.NumDescs,
                    static_cast<unsigned long long>(clean.DestAccelerationStructureData),
                    static_cast<unsigned long long>(clean.ScratchAccelerationStructureData),
                    NumPostbuildInfoDescs);
}

void XboxBvhBuilder::BuildRaytracingAccelerationStructureBatched2X(
    ID3D12GraphicsCommandList6* pCommandList,
    UINT Count,
    const D3D12XBOX_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2* pDescs) {
    g_counters().batched_build_calls.fetch_add(1, std::memory_order_relaxed);
    if (!pCommandList || !pDescs || Count == 0) return;
    ComPtr<ID3D12GraphicsCommandList4> cl4 = unwrap_to_desktop_cmdlist4(reinterpret_cast<IUnknown*>(pCommandList));
    if (!cl4) return;

    for (UINT i = 0; i < Count; ++i) {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC clean = pDescs[i].Desc;
        clean.Inputs = sanitize_inputs(clean.Inputs);
        cl4->BuildRaytracingAccelerationStructure(
            &clean, pDescs[i].NumPostbuildInfoDescs, pDescs[i].pPostbuildInfoDescs);
    }
    wsx::trace_void("XboxBvhBuilder", "BuildASBatched2X(DXR1.1)", "cl4=%p count=%u", cl4.Get(), Count);
}

void XboxBvhBuilder::CopyRaytracingAccelerationStructure(
    ID3D12GraphicsCommandList4* pCommandList,
    D3D12_GPU_VIRTUAL_ADDRESS DestAccelerationStructureData,
    D3D12_GPU_VIRTUAL_ADDRESS SourceAccelerationStructureData,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE Mode) {
    g_counters().copy_calls.fetch_add(1, std::memory_order_relaxed);
    if (!pCommandList) return;
    ComPtr<ID3D12GraphicsCommandList4> cl4 = unwrap_to_desktop_cmdlist4(reinterpret_cast<IUnknown*>(pCommandList));
    if (!cl4) return;

    cl4->CopyRaytracingAccelerationStructure(
        DestAccelerationStructureData, SourceAccelerationStructureData, Mode);
    wsx::trace_void("XboxBvhBuilder", "CopyAS(DXR1.1)",
                    "cl4=%p dst=0x%llX src=0x%llX mode=%u",
                    cl4.Get(),
                    static_cast<unsigned long long>(DestAccelerationStructureData),
                    static_cast<unsigned long long>(SourceAccelerationStructureData),
                    static_cast<UINT>(Mode));
}

void XboxBvhBuilder::CpuValidateResultX(
    ID3D12GraphicsCommandList4* pCommandList,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc) {
    g_counters().validate_calls.fetch_add(1, std::memory_order_relaxed);
    if (!pCommandList || !pDesc) return;
    ComPtr<ID3D12GraphicsCommandList4> cl4 = unwrap_to_desktop_cmdlist4(reinterpret_cast<IUnknown*>(pCommandList));
    if (!cl4) return;
    // Emit UAV barrier guaranteeing prior AS build is visible before subsequent validation/raytracing reads
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = nullptr;
    cl4->ResourceBarrier(1, &uav);
}

HRESULT XboxBvhBuilder::ValidateRehydrateAccelerationStructureBuildDesc(
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc) {
    g_counters().validate_calls.fetch_add(1, std::memory_order_relaxed);
    if (!pDesc) return E_INVALIDARG;
    if (!is_valid_va_or_zero(pDesc->DestAccelerationStructureData) ||
        !is_valid_va_or_zero(pDesc->ScratchAccelerationStructureData)) {
        return E_INVALIDARG;
    }
    if ((pDesc->DestAccelerationStructureData & 0xFFull) != 0ull) {
        return E_INVALIDARG; // DXR 256-byte AS alignment requirement
    }
    return S_OK;
}

HRESULT XboxBvhBuilder::RehydrateRaytracingAccelerationStructureX(
    ID3D12Device11* pDevice,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc) {
    g_counters().rehydrate_calls.fetch_add(1, std::memory_order_relaxed);
    if (!pDesc) return E_INVALIDARG;
    if (pDesc->DestAccelerationStructureData == 0ull || (pDesc->DestAccelerationStructureData & 0xFFull) != 0ull) {
        return E_INVALIDARG;
    }
    if (pDesc->SourceAccelerationStructureData == 0ull) {
        return S_OK;
    }
    ComPtr<ID3D12Device5> dev5 = pDevice
        ? unwrap_to_desktop_device5(reinterpret_cast<IUnknown*>(pDevice))
        : (impl_ ? impl_->dev5 : nullptr);
    if (!dev5) return E_NOINTERFACE;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList4> cl4;
    ComPtr<ID3D12Fence> fence;
    HRESULT hr = dev5->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) return hr;
    if (FAILED(hr = dev5->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&alloc)))) return hr;
    if (FAILED(hr = dev5->CreateCommandList(0, qd.Type, alloc.Get(), nullptr, IID_PPV_ARGS(&cl4)))) return hr;
    if (FAILED(hr = dev5->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return hr;

    cl4->CopyRaytracingAccelerationStructure(
        pDesc->DestAccelerationStructureData,
        pDesc->SourceAccelerationStructureData,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE);
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = nullptr;
    cl4->ResourceBarrier(1, &uav);
    if (FAILED(hr = cl4->Close())) return hr;

    ID3D12CommandList* lists[] = {cl4.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(hr = queue->Signal(fence.Get(), 1u))) return hr;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev) {
        fence->SetEventOnCompletion(1u, ev);
        WaitForSingleObject(ev, 5000);
        CloseHandle(ev);
    }
    return S_OK;
}

HRESULT XboxBvhBuilder::RehydrateRaytracingAccelerationStructureBatchedX(
    ID3D12GraphicsCommandList8* pCommandList,
    UINT Count,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDescs) {
    g_counters().rehydrate_calls.fetch_add(1, std::memory_order_relaxed);
    if (!pCommandList || (!pDescs && Count > 0)) return E_INVALIDARG;
    ComPtr<ID3D12GraphicsCommandList4> cl4 = unwrap_to_desktop_cmdlist4(reinterpret_cast<IUnknown*>(pCommandList));
    if (!cl4) return E_NOINTERFACE;
    bool issued = false;
    for (UINT i = 0; i < Count; ++i) {
        if (pDescs[i].SourceAccelerationStructureData != 0ull &&
            pDescs[i].DestAccelerationStructureData != 0ull) {
            cl4->CopyRaytracingAccelerationStructure(
                pDescs[i].DestAccelerationStructureData,
                pDescs[i].SourceAccelerationStructureData,
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE);
            issued = true;
        }
    }
    if (issued) {
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = nullptr;
        cl4->ResourceBarrier(1, &uav);
    }
    return S_OK;
}

HRESULT XboxBvhBuilder::RehydrateRaytracingAccelerationStructureBatched2X(
    ID3D12GraphicsCommandList8* pCommandList,
    UINT Count,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDescs,
    const UINT64* /*pMaxSizes*/) {
    return RehydrateRaytracingAccelerationStructureBatchedX(pCommandList, Count, pDescs);
}

void XboxBvhBuilder::Set64BitMortonCodes(bool bEnable) {
    if (magic_ == kBridgeMagic && impl_) {
        impl_->use_64bit_morton = bEnable;
    }
}

extern "C" HRESULT WINAPI WsxXboxBvhBuilderGetStats(WsxXboxBvhBridgeStats* pOut) {
    if (!pOut) return E_POINTER;
    auto& c = g_counters();
    pOut->InstancesCreated = c.instances_created.load(std::memory_order_relaxed);
    pOut->PrebuildInfoCalls = c.prebuild_calls.load(std::memory_order_relaxed);
    pOut->BuildCalls = c.build_calls.load(std::memory_order_relaxed);
    pOut->BatchedBuildCalls = c.batched_build_calls.load(std::memory_order_relaxed);
    pOut->CopyCalls = c.copy_calls.load(std::memory_order_relaxed);
    pOut->RehydrateCalls = c.rehydrate_calls.load(std::memory_order_relaxed);
    pOut->ValidateCalls = c.validate_calls.load(std::memory_order_relaxed);
    return S_OK;
}
