// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Full COM Proxies for Direct3D 12.X CommandQueue and GraphicsCommandList6:
// - WsxD3D12CommandListProxy : public ID3D12GraphicsCommandList6
//   * Sanitizes Xbox-specific resource state flags in ResourceBarrier & drops no-op barriers
//   * Returns WsxD3D12DeviceProxy from GetDevice()
//   * Unwraps bundle proxies in ExecuteBundle()
//   * Traces Mesh Shader / DXR calls when WSX_TRACE=1
// - WsxD3D12CommandQueueProxy : public ID3D12CommandQueue
//   * Unwraps WsxD3D12CommandListProxy arrays in ExecuteCommandLists()
//   * Returns WsxD3D12DeviceProxy from GetDevice()

#pragma once

#include "wsx_config.h"
#include "d3d12_xs_guest_textures.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace wsx_d3d12 {

using Microsoft::WRL::ComPtr;

struct CommandTelemetry {
    std::atomic<uint64_t> draw_calls{0};
    std::atomic<uint64_t> dispatch_calls{0};
    std::atomic<uint64_t> mesh_dispatches{0};
    std::atomic<uint64_t> ray_dispatches{0};
    std::atomic<uint64_t> barriers_submitted{0};
    std::atomic<uint64_t> barriers_elided{0};
    std::atomic<uint64_t> execute_command_lists{0};
    std::atomic<uint64_t> nested_occ_queries_unnested{0};
};

inline CommandTelemetry& global_command_telemetry() noexcept {
    static CommandTelemetry t;
    return t;
}

// Valid desktop D3D12_RESOURCE_STATES bitmask (excludes Xbox-only high state bits >= 0x10000000)
constexpr UINT kDesktopResourceStateMask = 0x03FFFFFFu;

// Xbox One / Xbox Series X|S shader-resource state modifier bit OR'd into SRV states in ResourceBarrier.
// On desktop D3D12, 0x00400000 is D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, which is
// illegal when combined with any other state bit or when used inside any transition barrier.
constexpr UINT kXboxShaderResourceStateBit = 0x00400000u;

// Xbox-only resource flags (ALLOW_DCC | FORCE_TEXTURE_COMPATIBILITY | ALLOW_AUTOMATIC_GAMEDVR_TONE_MAP)
constexpr UINT kXboxResourceFlagsMaskShared = 0x70000000u;

inline D3D12_RESOURCE_STATES sanitize_xbox_resource_state(D3D12_RESOURCE_STATES state) noexcept {
    UINT clean = static_cast<UINT>(state) & kDesktopResourceStateMask;
    if ((clean & kXboxShaderResourceStateBit) != 0u && (clean & ~kXboxShaderResourceStateBit) != 0u) {
        clean &= ~kXboxShaderResourceStateBit;
    }
    return static_cast<D3D12_RESOURCE_STATES>(clean);
}

inline D3D12_RESOURCE_STATES sanitize_xbox_transition_state(D3D12_RESOURCE_STATES state) noexcept {
    UINT clean = static_cast<UINT>(state) & kDesktopResourceStateMask;
    if ((clean & kXboxShaderResourceStateBit) != 0u) {
        clean &= ~kXboxShaderResourceStateBit;
        if (clean == 0u) {
            clean = static_cast<UINT>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }
    return static_cast<D3D12_RESOURCE_STATES>(clean);
}

// Heap-aware initial state sanitization for desktop D3D12:
// Real Xbox titles (Unity/UE) pass 0x8C3 (missing COPY_SOURCE) on UPLOAD heaps or non-COPY_DEST on READBACK heaps.
inline D3D12_RESOURCE_STATES sanitize_xbox_initial_state_for_heap(
    const D3D12_HEAP_PROPERTIES* hp,
    D3D12_RESOURCE_STATES state) noexcept {
    if (hp) {
        if (hp->Type == D3D12_HEAP_TYPE_UPLOAD) {
            return D3D12_RESOURCE_STATE_GENERIC_READ;
        }
        if (hp->Type == D3D12_HEAP_TYPE_READBACK) {
            return D3D12_RESOURCE_STATE_COPY_DEST;
        }
    }
    return sanitize_xbox_resource_state(state);
}

// Xbox One/Series DMA command queue/allocator/list type 0x10 -> PC D3D12_COMMAND_LIST_TYPE_COPY
inline D3D12_COMMAND_LIST_TYPE remap_xbox_command_list_type(D3D12_COMMAND_LIST_TYPE type) noexcept {
    if (static_cast<UINT>(type) == 0x10u) {
        return D3D12_COMMAND_LIST_TYPE_COPY;
    }
    return type;
}

// Xbox descriptor heap type 4 (console CBV/SRV/UAV alias) -> PC D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
inline D3D12_DESCRIPTOR_HEAP_TYPE remap_xbox_descriptor_heap_type(D3D12_DESCRIPTOR_HEAP_TYPE type) noexcept {
    if (static_cast<UINT>(type) == 4u) {
        return D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    }
    return type;
}

// Xbox-only vertex format 189 (R10G10B10_SNORM_A2_UNORM) -> PC DXGI_FORMAT_R10G10B10A2_UNORM
inline DXGI_FORMAT remap_xbox_vertex_format(DXGI_FORMAT fmt) noexcept {
    if (static_cast<UINT>(fmt) == 189u) {
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    return fmt;
}

// Sanitize Xbox Series X|S resource descriptors for desktop D3D12:
// 1. Strip Xbox-only resource flags (0x70000000u)
// 2. Strip D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE when ALLOW_DEPTH_STENCIL is not set (desktop returns E_INVALIDARG)
// 3. Map Xbox-only tile layouts (>= 2, e.g. 0x101 used by FSR 4.1.1) -> ROW_MAJOR / UNKNOWN
// 4. Map DXGI_FORMAT_R9G9B9E5_SHAREDEXP + ALLOW_UNORDERED_ACCESS -> DXGI_FORMAT_R32_TYPELESS
//    (Xbox RDNA2 allows UAV directly on R9G9B9E5_SHAREDEXP; desktop D3D12 requires R32_TYPELESS + R32_UINT UAV)
template <typename ResourceDescT>
inline void sanitize_xbox_resource_desc_inplace(ResourceDescT& desc) noexcept {
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<UINT>(desc.Flags) & ~kXboxResourceFlagsMaskShared);
    if ((desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0 &&
        (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) {
        desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
            static_cast<UINT>(desc.Flags) & ~static_cast<UINT>(D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE));
    }
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (static_cast<UINT>(desc.Layout) >= 2u) {
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        }
    } else if (static_cast<UINT>(desc.Layout) >= 2u) {
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }
    if (desc.Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP &&
        (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
    }
}

struct __declspec(uuid("9c2a50d1-4a7e-4b92-8e1f-61d34c9a20e1")) IWsxD3D12CommandListProxyMarker : IUnknown {};
struct __declspec(uuid("4e7f12b8-9c3d-41a6-b52e-80f1d3e76a94")) IWsxD3D12CommandQueueProxyMarker : IUnknown {};
struct __declspec(uuid("e1b72094-5c38-4d61-9a14-7b23c04f81a2")) IWsxXboxDescHeapAbiMarker : IUnknown {};
struct __declspec(uuid("f2c831a5-6d49-4e72-8b25-8c34d15f92b3")) IWsxXboxCommandListAbiMarker : IUnknown {};
struct __declspec(uuid("a3d942b6-7e5a-4f83-9c36-9d45e260a3c4")) IWsxXboxDeviceAbiMarker : IUnknown {};

inline ID3D12DescriptorHeap* unwrap_descriptor_heap(ID3D12DescriptorHeap* heap) noexcept {
    if (!heap) return nullptr;
    ID3D12DescriptorHeap* raw = nullptr;
    if (SUCCEEDED(heap->QueryInterface(__uuidof(IWsxXboxDescHeapAbiMarker),
                                       reinterpret_cast<void**>(&raw))) && raw) {
        return raw;
    }
    return heap;
}

class WsxD3D12CommandListProxy final : public ID3D12GraphicsCommandList6 {
    std::atomic<ULONG> ref_{1};
    ComPtr<ID3D12Device> owner_device_;
    ComPtr<ID3D12GraphicsCommandList> real0_;
    ComPtr<ID3D12GraphicsCommandList1> real1_;
    ComPtr<ID3D12GraphicsCommandList2> real2_;
    ComPtr<ID3D12GraphicsCommandList3> real3_;
    ComPtr<ID3D12GraphicsCommandList4> real4_;
    ComPtr<ID3D12GraphicsCommandList5> real5_;
    ComPtr<ID3D12GraphicsCommandList6> real6_;
    bool traced_mesh_ = false;
    bool traced_dxr_ = false;
    // Lock-free per-command-list state tracking & batched telemetry (D3D12 CL recording is single-threaded by spec!)
    struct ActiveOccQuery {
        ID3D12QueryHeap* heap = nullptr;
        D3D12_QUERY_TYPE type = D3D12_QUERY_TYPE_OCCLUSION;
        UINT index = 0u;
    };
    std::vector<ActiveOccQuery> occ_query_stack_;
    std::vector<RecordedStateTransition> recorded_transitions_;
    uint64_t local_draw_calls_ = 0u;
    uint64_t local_dispatch_calls_ = 0u;
    uint64_t local_mesh_dispatches_ = 0u;
    uint64_t local_ray_dispatches_ = 0u;
    uint64_t local_barriers_submitted_ = 0u;
    uint64_t local_barriers_elided_ = 0u;
    uint64_t local_nested_occ_unnested_ = 0u;

    static bool is_occlusion_query_type(D3D12_QUERY_TYPE type) noexcept {
        return type == D3D12_QUERY_TYPE_OCCLUSION || type == D3D12_QUERY_TYPE_BINARY_OCCLUSION;
    }

    void flush_local_telemetry() noexcept {
        auto& gt = global_command_telemetry();
        if (local_draw_calls_)          { gt.draw_calls.fetch_add(local_draw_calls_, std::memory_order_relaxed); local_draw_calls_ = 0u; }
        if (local_dispatch_calls_)      { gt.dispatch_calls.fetch_add(local_dispatch_calls_, std::memory_order_relaxed); local_dispatch_calls_ = 0u; }
        if (local_mesh_dispatches_)     { gt.mesh_dispatches.fetch_add(local_mesh_dispatches_, std::memory_order_relaxed); local_mesh_dispatches_ = 0u; }
        if (local_ray_dispatches_)      { gt.ray_dispatches.fetch_add(local_ray_dispatches_, std::memory_order_relaxed); local_ray_dispatches_ = 0u; }
        if (local_barriers_submitted_)  { gt.barriers_submitted.fetch_add(local_barriers_submitted_, std::memory_order_relaxed); local_barriers_submitted_ = 0u; }
        if (local_barriers_elided_)     { gt.barriers_elided.fetch_add(local_barriers_elided_, std::memory_order_relaxed); local_barriers_elided_ = 0u; }
        if (local_nested_occ_unnested_) { gt.nested_occ_queries_unnested.fetch_add(local_nested_occ_unnested_, std::memory_order_relaxed); local_nested_occ_unnested_ = 0u; }
    }

public:
    WsxD3D12CommandListProxy(ID3D12Device* owner_device, ID3D12GraphicsCommandList* raw_list) noexcept
        : owner_device_(owner_device), real0_(raw_list) {
        real0_.As(&real1_);
        real0_.As(&real2_);
        real0_.As(&real3_);
        real0_.As(&real4_);
        real0_.As(&real5_);
        real0_.As(&real6_);
        recorded_transitions_.reserve(64u);
    }

    ~WsxD3D12CommandListProxy() noexcept {
        flush_local_telemetry();
    }

    ID3D12GraphicsCommandList* raw_list() const noexcept { return real0_.Get(); }

    void commit_recorded_transitions() const {
        if (!recorded_transitions_.empty()) {
            GuestTextureManager::commit_state_transitions(recorded_transitions_);
        }
    }

    static WsxD3D12CommandListProxy* from_interface(ID3D12CommandList* list) noexcept {
        if (!list) return nullptr;
        ID3D12GraphicsCommandList* xbox_raw = nullptr;
        if (SUCCEEDED(list->QueryInterface(__uuidof(IWsxXboxCommandListAbiMarker),
                                           reinterpret_cast<void**>(&xbox_raw))) && xbox_raw) {
            list = xbox_raw;
        }
        WsxD3D12CommandListProxy* proxy = nullptr;
        if (SUCCEEDED(list->QueryInterface(__uuidof(IWsxD3D12CommandListProxyMarker),
                                           reinterpret_cast<void**>(&proxy))) && proxy) {
            return proxy;
        }
        return nullptr;
    }

    static ID3D12CommandList* unwrap(ID3D12CommandList* list) noexcept {
        if (!list) return nullptr;
        ID3D12GraphicsCommandList* xbox_raw = nullptr;
        if (SUCCEEDED(list->QueryInterface(__uuidof(IWsxXboxCommandListAbiMarker),
                                           reinterpret_cast<void**>(&xbox_raw))) && xbox_raw) {
            list = xbox_raw;
        }
        WsxD3D12CommandListProxy* proxy = nullptr;
        if (SUCCEEDED(list->QueryInterface(__uuidof(IWsxD3D12CommandListProxyMarker),
                                           reinterpret_cast<void**>(&proxy))) && proxy) {
            return proxy->raw_list();
        }
        return list;
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IWsxD3D12CommandListProxyMarker)) {
            *ppvObject = this;
            return S_OK; // Non-AddRef internal unwrap marker
        }
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(ID3D12Object) ||
            riid == __uuidof(ID3D12DeviceChild) ||
            riid == __uuidof(ID3D12CommandList) ||
            riid == __uuidof(ID3D12GraphicsCommandList) ||
            (riid == __uuidof(ID3D12GraphicsCommandList1) && real1_) ||
            (riid == __uuidof(ID3D12GraphicsCommandList2) && real2_) ||
            (riid == __uuidof(ID3D12GraphicsCommandList3) && real3_) ||
            (riid == __uuidof(ID3D12GraphicsCommandList4) && real4_) ||
            (riid == __uuidof(ID3D12GraphicsCommandList5) && real5_) ||
            (riid == __uuidof(ID3D12GraphicsCommandList6) && real6_)) {
            *ppvObject = static_cast<ID3D12GraphicsCommandList6*>(this);
            AddRef();
            return S_OK;
        }
        return real0_->QueryInterface(riid, ppvObject);
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    // ID3D12Object
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
        return real0_->GetPrivateData(guid, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
        return real0_->SetPrivateData(guid, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
        return real0_->SetPrivateDataInterface(guid, pData);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override {
        return real0_->SetName(Name);
    }

    // ID3D12DeviceChild
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override {
        if (owner_device_) {
            return owner_device_->QueryInterface(riid, ppvDevice);
        }
        return real0_->GetDevice(riid, ppvDevice);
    }

    // ID3D12CommandList
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override {
        return real0_->GetType();
    }

    // ID3D12GraphicsCommandList
    HRESULT STDMETHODCALLTYPE Close() override {
        flush_local_telemetry();
        return real0_->Close();
    }
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState) override {
        occ_query_stack_.clear();
        recorded_transitions_.clear();
        local_draw_calls_ = local_dispatch_calls_ = local_mesh_dispatches_ = local_ray_dispatches_ = 0u;
        local_barriers_submitted_ = local_barriers_elided_ = local_nested_occ_unnested_ = 0u;
        return real0_->Reset(pAllocator, pInitialState);
    }
    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState* pPipelineState) override {
        real0_->ClearState(pPipelineState);
    }
    void STDMETHODCALLTYPE DrawInstanced(
        UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) override {
        ++local_draw_calls_;
        real0_->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
    }
    void STDMETHODCALLTYPE DrawIndexedInstanced(
        UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation,
        INT BaseVertexLocation, UINT StartInstanceLocation) override {
        ++local_draw_calls_;
        real0_->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation,
                                     BaseVertexLocation, StartInstanceLocation);
    }
    void STDMETHODCALLTYPE Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override {
        ++local_dispatch_calls_;
        real0_->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    }
    void STDMETHODCALLTYPE CopyBufferRegion(
        ID3D12Resource* pDstBuffer, UINT64 DstOffset, ID3D12Resource* pSrcBuffer,
        UINT64 SrcOffset, UINT64 NumBytes) override {
        real0_->CopyBufferRegion(unwrap_resource(pDstBuffer), DstOffset,
                                 unwrap_resource(pSrcBuffer), SrcOffset, NumBytes);
    }
    void STDMETHODCALLTYPE CopyTextureRegion(
        const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ,
        const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox) override {
        D3D12_TEXTURE_COPY_LOCATION dst_local{};
        D3D12_TEXTURE_COPY_LOCATION src_local{};
        const D3D12_TEXTURE_COPY_LOCATION* dst_ptr = pDst;
        const D3D12_TEXTURE_COPY_LOCATION* src_ptr = pSrc;
        if (pDst) {
            dst_local = *pDst;
            dst_local.pResource = unwrap_resource(dst_local.pResource);
            dst_ptr = &dst_local;
        }
        if (pSrc) {
            src_local = *pSrc;
            src_local.pResource = unwrap_resource(src_local.pResource);
            src_ptr = &src_local;
        }
        real0_->CopyTextureRegion(dst_ptr, DstX, DstY, DstZ, src_ptr, pSrcBox);
    }
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource* pDstResource, ID3D12Resource* pSrcResource) override {
        real0_->CopyResource(unwrap_resource(pDstResource), unwrap_resource(pSrcResource));
    }
    void STDMETHODCALLTYPE CopyTiles(
        ID3D12Resource* pTiledResource, const D3D12_TILED_RESOURCE_COORDINATE* pTileRegionStartCoordinate,
        const D3D12_TILE_REGION_SIZE* pTileRegionSize, ID3D12Resource* pBuffer,
        UINT64 BufferStartOffsetInBytes, D3D12_TILE_COPY_FLAGS Flags) override {
        real0_->CopyTiles(unwrap_resource(pTiledResource), pTileRegionStartCoordinate, pTileRegionSize,
                          unwrap_resource(pBuffer), BufferStartOffsetInBytes, Flags);
    }
    void STDMETHODCALLTYPE ResolveSubresource(
        ID3D12Resource* pDstResource, UINT DstSubresource, ID3D12Resource* pSrcResource,
        UINT SrcSubresource, DXGI_FORMAT Format) override {
        real0_->ResolveSubresource(unwrap_resource(pDstResource), DstSubresource,
                                   unwrap_resource(pSrcResource), SrcSubresource, Format);
    }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) override {
        real0_->IASetPrimitiveTopology(PrimitiveTopology);
    }
    void STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D12_VIEWPORT* pViewports) override {
        real0_->RSSetViewports(NumViewports, pViewports);
    }
    void STDMETHODCALLTYPE RSSetScissorRects(UINT NumRects, const D3D12_RECT* pRects) override {
        real0_->RSSetScissorRects(NumRects, pRects);
    }
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT BlendFactor[4]) override {
        real0_->OMSetBlendFactor(BlendFactor);
    }
    void STDMETHODCALLTYPE OMSetStencilRef(UINT StencilRef) override {
        real0_->OMSetStencilRef(StencilRef);
    }
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* pPipelineState) override {
        real0_->SetPipelineState(pPipelineState);
    }

    void STDMETHODCALLTYPE ResourceBarrier(UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) override {
        if (NumBarriers == 0 || !pBarriers) return;

        D3D12_RESOURCE_BARRIER stack_buf[16];
        std::vector<D3D12_RESOURCE_BARRIER> heap_buf;
        D3D12_RESOURCE_BARRIER* out = stack_buf;
        if (NumBarriers > 16) {
            heap_buf.resize(NumBarriers);
            out = heap_buf.data();
        }

        UINT out_count = 0;
        UINT elided = 0;
        for (UINT i = 0; i < NumBarriers; ++i) {
            D3D12_RESOURCE_BARRIER b = pBarriers[i];
            if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                if (auto* rp = WsxD3D12ResourceProxy::from_interface(b.Transition.pResource)) {
                    b.Transition.pResource = rp->raw_resource();
                    const D3D12_RESOURCE_STATES clean_before = sanitize_xbox_transition_state(b.Transition.StateBefore);
                    const D3D12_RESOURCE_STATES clean_after = sanitize_xbox_transition_state(b.Transition.StateAfter);
                    b.Transition.StateBefore = clean_before;
                    b.Transition.StateAfter = clean_after;
                    if (clean_before == clean_after) {
                        ++elided;
                        continue;
                    }
                    if ((b.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) == 0 && rp->guest_record()) {
                        recorded_transitions_.push_back(RecordedStateTransition{
                            rp->guest_record(), b.Transition.Subresource, clean_after});
                    }
                } else {
                    b.Transition.StateBefore = sanitize_xbox_transition_state(b.Transition.StateBefore);
                    b.Transition.StateAfter = sanitize_xbox_transition_state(b.Transition.StateAfter);
                    if (b.Transition.StateBefore == b.Transition.StateAfter) {
                        ++elided;
                        continue; // Drop no-op Xbox-only state transition
                    }
                }
                // Coalesce back-to-back transitions on the exact same resource & subresource within this batch
                if (b.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE) {
                    bool coalesced = false;
                    for (UINT k = out_count; k > 0u; --k) {
                        auto& prev = out[k - 1u];
                        if (prev.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
                            prev.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE &&
                            prev.Transition.pResource == b.Transition.pResource &&
                            prev.Transition.Subresource == b.Transition.Subresource &&
                            prev.Transition.StateAfter == b.Transition.StateBefore) {
                            prev.Transition.StateAfter = b.Transition.StateAfter;
                            ++elided;
                            if (prev.Transition.StateBefore == prev.Transition.StateAfter) {
                                // Round-trip A->B->A cancels completely
                                for (UINT j = k - 1u; j + 1u < out_count; ++j) out[j] = out[j + 1u];
                                --out_count;
                                ++elided;
                            }
                            coalesced = true;
                            break;
                        }
                    }
                    if (coalesced) continue;
                }
            } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
                b.Aliasing.pResourceBefore = unwrap_resource(b.Aliasing.pResourceBefore);
                b.Aliasing.pResourceAfter = unwrap_resource(b.Aliasing.pResourceAfter);
            } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
                b.UAV.pResource = unwrap_resource(b.UAV.pResource);
                // Deduplicate identical UAV barriers within the same ResourceBarrier batch
                bool dup_uav = false;
                for (UINT k = 0; k < out_count; ++k) {
                    if (out[k].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV &&
                        (out[k].UAV.pResource == nullptr || out[k].UAV.pResource == b.UAV.pResource)) {
                        dup_uav = true;
                        break;
                    }
                }
                if (dup_uav) {
                    ++elided;
                    continue;
                }
            }
            out[out_count++] = b;
        }
        local_barriers_elided_ += elided;
        if (out_count > 0) {
            local_barriers_submitted_ += out_count;
            real0_->ResourceBarrier(out_count, out);
        }
    }

    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList* pCommandList) override {
        auto* raw_bundle = static_cast<ID3D12GraphicsCommandList*>(unwrap(pCommandList));
        real0_->ExecuteBundle(raw_bundle);
    }

    void STDMETHODCALLTYPE SetDescriptorHeaps(
        UINT NumDescriptorHeaps, ID3D12DescriptorHeap* const* ppDescriptorHeaps) override {
        if (NumDescriptorHeaps == 0 || !ppDescriptorHeaps) {
            real0_->SetDescriptorHeaps(NumDescriptorHeaps, ppDescriptorHeaps);
            return;
        }
        if (NumDescriptorHeaps <= 8) {
            ID3D12DescriptorHeap* unwrapped[8];
            for (UINT i = 0; i < NumDescriptorHeaps; ++i) {
                unwrapped[i] = unwrap_descriptor_heap(ppDescriptorHeaps[i]);
            }
            real0_->SetDescriptorHeaps(NumDescriptorHeaps, unwrapped);
        } else {
            std::vector<ID3D12DescriptorHeap*> unwrapped(NumDescriptorHeaps);
            for (UINT i = 0; i < NumDescriptorHeaps; ++i) {
                unwrapped[i] = unwrap_descriptor_heap(ppDescriptorHeaps[i]);
            }
            real0_->SetDescriptorHeaps(NumDescriptorHeaps, unwrapped.data());
        }
    }
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature* pRootSignature) override {
        real0_->SetComputeRootSignature(pRootSignature);
    }
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature* pRootSignature) override {
        real0_->SetGraphicsRootSignature(pRootSignature);
    }
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(
        UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override {
        real0_->SetComputeRootDescriptorTable(RootParameterIndex, BaseDescriptor);
    }
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(
        UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override {
        real0_->SetGraphicsRootDescriptorTable(RootParameterIndex, BaseDescriptor);
    }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(
        UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override {
        real0_->SetComputeRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
    }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(
        UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override {
        real0_->SetGraphicsRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
    }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(
        UINT RootParameterIndex, UINT Num32BitValuesToSet, const void* pSrcData, UINT DestOffsetIn32BitValues) override {
        real0_->SetComputeRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
    }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(
        UINT RootParameterIndex, UINT Num32BitValuesToSet, const void* pSrcData, UINT DestOffsetIn32BitValues) override {
        real0_->SetGraphicsRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
    }
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override {
        real0_->SetComputeRootConstantBufferView(RootParameterIndex, BufferLocation);
    }
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override {
        real0_->SetGraphicsRootConstantBufferView(RootParameterIndex, BufferLocation);
    }
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override {
        real0_->SetComputeRootShaderResourceView(RootParameterIndex, BufferLocation);
    }
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override {
        real0_->SetGraphicsRootShaderResourceView(RootParameterIndex, BufferLocation);
    }
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override {
        real0_->SetComputeRootUnorderedAccessView(RootParameterIndex, BufferLocation);
    }
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override {
        real0_->SetGraphicsRootUnorderedAccessView(RootParameterIndex, BufferLocation);
    }
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* pView) override {
        real0_->IASetIndexBuffer(pView);
    }
    void STDMETHODCALLTYPE IASetVertexBuffers(
        UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW* pViews) override {
        real0_->IASetVertexBuffers(StartSlot, NumViews, pViews);
    }
    void STDMETHODCALLTYPE SOSetTargets(
        UINT StartSlot, UINT NumViews, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* pViews) override {
        real0_->SOSetTargets(StartSlot, NumViews, pViews);
    }
    void STDMETHODCALLTYPE OMSetRenderTargets(
        UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
        BOOL RTsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) override {
        real0_->OMSetRenderTargets(NumRenderTargetDescriptors, pRenderTargetDescriptors,
                                   RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
    }
    void STDMETHODCALLTYPE ClearDepthStencilView(
        D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags,
        FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects) override {
        real0_->ClearDepthStencilView(DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);
    }
    void STDMETHODCALLTYPE ClearRenderTargetView(
        D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[4],
        UINT NumRects, const D3D12_RECT* pRects) override {
        real0_->ClearRenderTargetView(RenderTargetView, ColorRGBA, NumRects, pRects);
    }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(
        D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle,
        ID3D12Resource* pResource, const UINT Values[4], UINT NumRects, const D3D12_RECT* pRects) override {
        real0_->ClearUnorderedAccessViewUint(ViewGPUHandleInCurrentHeap, ViewCPUHandle, unwrap_resource(pResource),
                                             Values, NumRects, pRects);
    }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(
        D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle,
        ID3D12Resource* pResource, const FLOAT Values[4], UINT NumRects, const D3D12_RECT* pRects) override {
        real0_->ClearUnorderedAccessViewFloat(ViewGPUHandleInCurrentHeap, ViewCPUHandle, unwrap_resource(pResource),
                                              Values, NumRects, pRects);
    }
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource* pResource, const D3D12_DISCARD_REGION* pRegion) override {
        real0_->DiscardResource(unwrap_resource(pResource), pRegion);
    }
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) override {
        // Xbox allows nested occlusion queries (e.g. UE4/UE5 outer pass dummy query around per-object queries).
        // Desktop D3D12 allows only ONE active occlusion query per command list: suspend outer, run inner, resume on EndQuery!
        if (is_occlusion_query_type(Type)) {
            if (!occ_query_stack_.empty()) {
                const auto& outer = occ_query_stack_.back();
                real0_->EndQuery(outer.heap, outer.type, outer.index);
                ++local_nested_occ_unnested_;
            }
            occ_query_stack_.push_back(ActiveOccQuery{pQueryHeap, Type, Index});
        }
        real0_->BeginQuery(pQueryHeap, Type, Index);
    }
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) override {
        real0_->EndQuery(pQueryHeap, Type, Index);
        if (is_occlusion_query_type(Type) && !occ_query_stack_.empty()) {
            const auto& top = occ_query_stack_.back();
            if (top.heap == pQueryHeap && top.type == Type && top.index == Index) {
                occ_query_stack_.pop_back();
                if (!occ_query_stack_.empty()) {
                    const auto& resume = occ_query_stack_.back();
                    real0_->BeginQuery(resume.heap, resume.type, resume.index);
                }
            } else {
                for (auto it = occ_query_stack_.rbegin(); it != occ_query_stack_.rend(); ++it) {
                    if (it->heap == pQueryHeap && it->type == Type && it->index == Index) {
                        occ_query_stack_.erase(std::next(it).base());
                        break;
                    }
                }
            }
        }
    }
    void STDMETHODCALLTYPE ResolveQueryData(
        ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT NumQueries,
        ID3D12Resource* pDestinationBuffer, UINT64 AlignedDestinationBufferOffset) override {
        real0_->ResolveQueryData(pQueryHeap, Type, StartIndex, NumQueries,
                                 unwrap_resource(pDestinationBuffer), AlignedDestinationBufferOffset);
    }
    void STDMETHODCALLTYPE SetPredication(
        ID3D12Resource* pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Operation) override {
        real0_->SetPredication(unwrap_resource(pBuffer), AlignedBufferOffset, Operation);
    }
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void* pData, UINT Size) override {
        real0_->SetMarker(Metadata, pData, Size);
    }
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void* pData, UINT Size) override {
        real0_->BeginEvent(Metadata, pData, Size);
    }
    void STDMETHODCALLTYPE EndEvent() override {
        real0_->EndEvent();
    }
    void STDMETHODCALLTYPE ExecuteIndirect(
        ID3D12CommandSignature* pCommandSignature, UINT MaxCommandCount, ID3D12Resource* pArgumentBuffer,
        UINT64 ArgumentBufferOffset, ID3D12Resource* pCountBuffer, UINT64 CountBufferOffset) override {
        ++local_draw_calls_;
        real0_->ExecuteIndirect(pCommandSignature, MaxCommandCount, unwrap_resource(pArgumentBuffer),
                                ArgumentBufferOffset, unwrap_resource(pCountBuffer), CountBufferOffset);
    }

    // ID3D12GraphicsCommandList1
    void STDMETHODCALLTYPE AtomicCopyBufferUINT(
        ID3D12Resource* pDstBuffer, UINT64 DstOffset, ID3D12Resource* pSrcBuffer, UINT64 SrcOffset,
        UINT Dependencies, ID3D12Resource* const* ppDependentResources,
        const D3D12_SUBRESOURCE_RANGE_UINT64* pDependentSubresourceRanges) override {
        if (real1_) {
            real1_->AtomicCopyBufferUINT(unwrap_resource(pDstBuffer), DstOffset, unwrap_resource(pSrcBuffer), SrcOffset,
                                         Dependencies, ppDependentResources, pDependentSubresourceRanges);
        }
    }
    void STDMETHODCALLTYPE AtomicCopyBufferUINT64(
        ID3D12Resource* pDstBuffer, UINT64 DstOffset, ID3D12Resource* pSrcBuffer, UINT64 SrcOffset,
        UINT Dependencies, ID3D12Resource* const* ppDependentResources,
        const D3D12_SUBRESOURCE_RANGE_UINT64* pDependentSubresourceRanges) override {
        if (real1_) {
            real1_->AtomicCopyBufferUINT64(unwrap_resource(pDstBuffer), DstOffset, unwrap_resource(pSrcBuffer), SrcOffset,
                                           Dependencies, ppDependentResources, pDependentSubresourceRanges);
        }
    }
    void STDMETHODCALLTYPE OMSetDepthBounds(FLOAT Min, FLOAT Max) override {
        if (real1_) real1_->OMSetDepthBounds(Min, Max);
    }
    void STDMETHODCALLTYPE SetSamplePositions(
        UINT NumSamplesPerPixel, UINT NumPixels, D3D12_SAMPLE_POSITION* pSamplePositions) override {
        if (real1_) real1_->SetSamplePositions(NumSamplesPerPixel, NumPixels, pSamplePositions);
    }
    void STDMETHODCALLTYPE ResolveSubresourceRegion(
        ID3D12Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY,
        ID3D12Resource* pSrcResource, UINT SrcSubresource, D3D12_RECT* pSrcRect,
        DXGI_FORMAT Format, D3D12_RESOLVE_MODE ResolveMode) override {
        if (real1_) {
            real1_->ResolveSubresourceRegion(unwrap_resource(pDstResource), DstSubresource, DstX, DstY,
                                             unwrap_resource(pSrcResource), SrcSubresource, pSrcRect, Format, ResolveMode);
        }
    }
    void STDMETHODCALLTYPE SetViewInstanceMask(UINT Mask) override {
        if (real1_) real1_->SetViewInstanceMask(Mask);
    }

    // ID3D12GraphicsCommandList2
    void STDMETHODCALLTYPE WriteBufferImmediate(
        UINT Count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* pParams,
        const D3D12_WRITEBUFFERIMMEDIATE_MODE* pModes) override {
        if (real2_) real2_->WriteBufferImmediate(Count, pParams, pModes);
    }

    // ID3D12GraphicsCommandList3
    void STDMETHODCALLTYPE SetProtectedResourceSession(
        ID3D12ProtectedResourceSession* pProtectedResourceSession) override {
        if (real3_) real3_->SetProtectedResourceSession(pProtectedResourceSession);
    }

    // ID3D12GraphicsCommandList4
    void STDMETHODCALLTYPE BeginRenderPass(
        UINT NumRenderTargets, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* pRenderTargets,
        const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* pDepthStencil, D3D12_RENDER_PASS_FLAGS Flags) override {
        if (real4_) real4_->BeginRenderPass(NumRenderTargets, pRenderTargets, pDepthStencil, Flags);
    }
    void STDMETHODCALLTYPE EndRenderPass() override {
        if (real4_) real4_->EndRenderPass();
    }
    void STDMETHODCALLTYPE InitializeMetaCommand(
        ID3D12MetaCommand* pMetaCommand, const void* pInitializationParametersData,
        SIZE_T InitializationParametersDataSizeInBytes) override {
        if (real4_) {
            real4_->InitializeMetaCommand(pMetaCommand, pInitializationParametersData,
                                          InitializationParametersDataSizeInBytes);
        }
    }
    void STDMETHODCALLTYPE ExecuteMetaCommand(
        ID3D12MetaCommand* pMetaCommand, const void* pExecutionParametersData,
        SIZE_T ExecutionParametersDataSizeInBytes) override {
        if (real4_) {
            real4_->ExecuteMetaCommand(pMetaCommand, pExecutionParametersData,
                                       ExecutionParametersDataSizeInBytes);
        }
    }
    void STDMETHODCALLTYPE BuildRaytracingAccelerationStructure(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc,
        UINT NumPostbuildInfoDescs,
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pPostbuildInfoDescs) override {
        if (!traced_dxr_) {
            traced_dxr_ = true;
            wsx::trace_void("d3d12_xs", "WsxD3D12CommandListProxy::BuildRaytracingAccelerationStructure",
                            "postbuild=%u", NumPostbuildInfoDescs);
        }
        if (real4_) {
            real4_->BuildRaytracingAccelerationStructure(pDesc, NumPostbuildInfoDescs, pPostbuildInfoDescs);
        }
    }
    void STDMETHODCALLTYPE EmitRaytracingAccelerationStructurePostbuildInfo(
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pDesc,
        UINT NumSourceAccelerationStructures,
        const D3D12_GPU_VIRTUAL_ADDRESS* pSourceAccelerationStructureData) override {
        if (real4_) {
            real4_->EmitRaytracingAccelerationStructurePostbuildInfo(
                pDesc, NumSourceAccelerationStructures, pSourceAccelerationStructureData);
        }
    }
    void STDMETHODCALLTYPE CopyRaytracingAccelerationStructure(
        D3D12_GPU_VIRTUAL_ADDRESS DestAccelerationStructureData,
        D3D12_GPU_VIRTUAL_ADDRESS SourceAccelerationStructureData,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE Mode) override {
        if (real4_) {
            real4_->CopyRaytracingAccelerationStructure(
                DestAccelerationStructureData, SourceAccelerationStructureData, Mode);
        }
    }
    void STDMETHODCALLTYPE SetPipelineState1(ID3D12StateObject* pStateObject) override {
        if (real4_) real4_->SetPipelineState1(pStateObject);
    }
    void STDMETHODCALLTYPE DispatchRays(const D3D12_DISPATCH_RAYS_DESC* pDesc) override {
        ++local_ray_dispatches_;
        if (real4_) real4_->DispatchRays(pDesc);
    }

    // ID3D12GraphicsCommandList5
    void STDMETHODCALLTYPE RSSetShadingRate(
        D3D12_SHADING_RATE baseShadingRate, const D3D12_SHADING_RATE_COMBINER* combiners) override {
        if (real5_) real5_->RSSetShadingRate(baseShadingRate, combiners);
    }
    void STDMETHODCALLTYPE RSSetShadingRateImage(ID3D12Resource* shadingRateImage) override {
        if (real5_) real5_->RSSetShadingRateImage(unwrap_resource(shadingRateImage));
    }

    // ID3D12GraphicsCommandList6
    void STDMETHODCALLTYPE DispatchMesh(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override {
        ++local_mesh_dispatches_;
        if (!traced_mesh_) {
            traced_mesh_ = true;
            wsx::trace_void("d3d12_xs", "WsxD3D12CommandListProxy::DispatchMesh",
                            "groups=(%u,%u,%u)", ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
        }
        if (real6_) real6_->DispatchMesh(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    }
};

class WsxD3D12CommandQueueProxy final : public ID3D12CommandQueue {
    std::atomic<ULONG> ref_{1};
    ComPtr<ID3D12Device> owner_device_;
    ComPtr<ID3D12Device> raw_device_;
    ComPtr<ID3D12CommandQueue> real_;
    std::shared_ptr<GuestTextureManager> guest_mgr_;
    D3D12_COMMAND_LIST_TYPE queue_type_ = D3D12_COMMAND_LIST_TYPE_DIRECT;
    GuestUploadContext upload_ctx_;

public:
    WsxD3D12CommandQueueProxy(
        ID3D12Device* owner_device,
        ID3D12Device* raw_device,
        ID3D12CommandQueue* raw_queue,
        std::shared_ptr<GuestTextureManager> guest_mgr = nullptr) noexcept
        : owner_device_(owner_device),
          raw_device_(raw_device),
          real_(raw_queue),
          guest_mgr_(std::move(guest_mgr)) {
        if (real_) {
            queue_type_ = real_->GetDesc().Type;
        }
    }

    ~WsxD3D12CommandQueueProxy() {
        upload_ctx_.drain();
    }

    ID3D12CommandQueue* raw_queue() const noexcept { return real_.Get(); }

    bool flush_guest_textures() {
        if (!guest_mgr_ || !raw_device_ || !real_) return false;
        return upload_ctx_.flush_dirty_textures(raw_device_.Get(), real_.Get(), queue_type_, *guest_mgr_);
    }

    HRESULT readback_guest_texture(ID3D12Resource* res) {
        if (!res || !raw_device_ || !real_) return E_INVALIDARG;
        auto* rp = WsxD3D12ResourceProxy::from_interface(res);
        if (!rp || !rp->guest_record()) return E_NOINTERFACE;
        return wsx_d3d12::readback_guest_texture(raw_device_.Get(), real_.Get(), queue_type_, rp->guest_record());
    }

    uint64_t trim_staging_memory() noexcept {
        const uint64_t freed = upload_ctx_.trim_idle_slots();
        if (guest_mgr_ && freed > 0u) {
            guest_mgr_->record_staging_trim(freed);
        }
        return freed;
    }

    static WsxD3D12CommandQueueProxy* from_interface(ID3D12CommandQueue* q) noexcept {
        if (!q) return nullptr;
        WsxD3D12CommandQueueProxy* proxy = nullptr;
        if (SUCCEEDED(q->QueryInterface(__uuidof(IWsxD3D12CommandQueueProxyMarker),
                                        reinterpret_cast<void**>(&proxy))) && proxy) {
            return proxy;
        }
        return nullptr;
    }

    static ID3D12CommandQueue* unwrap(ID3D12CommandQueue* q) noexcept {
        if (!q) return nullptr;
        if (auto* proxy = from_interface(q)) {
            return proxy->raw_queue();
        }
        return q;
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IWsxD3D12CommandQueueProxyMarker)) {
            *ppvObject = this;
            return S_OK; // Non-AddRef internal unwrap marker
        }
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(ID3D12Object) ||
            riid == __uuidof(ID3D12DeviceChild) ||
            riid == __uuidof(ID3D12Pageable) ||
            riid == __uuidof(ID3D12CommandQueue)) {
            *ppvObject = static_cast<ID3D12CommandQueue*>(this);
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(riid, ppvObject);
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    // ID3D12Object
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
        return real_->GetPrivateData(guid, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
        return real_->SetPrivateData(guid, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
        return real_->SetPrivateDataInterface(guid, pData);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override {
        return real_->SetName(Name);
    }

    // ID3D12DeviceChild
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override {
        if (owner_device_) {
            return owner_device_->QueryInterface(riid, ppvDevice);
        }
        return real_->GetDevice(riid, ppvDevice);
    }

    // ID3D12CommandQueue
    void STDMETHODCALLTYPE UpdateTileMappings(
        ID3D12Resource* pResource, UINT NumResourceRegions,
        const D3D12_TILED_RESOURCE_COORDINATE* pResourceRegionStartCoordinates,
        const D3D12_TILE_REGION_SIZE* pResourceRegionSizes, ID3D12Heap* pHeap,
        UINT NumRanges, const D3D12_TILE_RANGE_FLAGS* pRangeFlags,
        const UINT* pHeapRangeStartOffsets, const UINT* pRangeTileCounts,
        D3D12_TILE_MAPPING_FLAGS Flags) override {
        real_->UpdateTileMappings(unwrap_resource(pResource), NumResourceRegions, pResourceRegionStartCoordinates,
                                  pResourceRegionSizes, pHeap, NumRanges, pRangeFlags,
                                  pHeapRangeStartOffsets, pRangeTileCounts, Flags);
    }

    void STDMETHODCALLTYPE CopyTileMappings(
        ID3D12Resource* pDstResource, const D3D12_TILED_RESOURCE_COORDINATE* pDstRegionStartCoordinate,
        ID3D12Resource* pSrcResource, const D3D12_TILED_RESOURCE_COORDINATE* pSrcRegionStartCoordinate,
        const D3D12_TILE_REGION_SIZE* pRegionSize, D3D12_TILE_MAPPING_FLAGS Flags) override {
        real_->CopyTileMappings(unwrap_resource(pDstResource), pDstRegionStartCoordinate,
                                unwrap_resource(pSrcResource), pSrcRegionStartCoordinate, pRegionSize, Flags);
    }

    void STDMETHODCALLTYPE ExecuteCommandLists(
        UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) override {
        global_command_telemetry().execute_command_lists.fetch_add(1u, std::memory_order_relaxed);
        // Immediate UMA guest-texture upload BEFORE every ExecuteCommandLists submission
        if (guest_mgr_ && guest_mgr_->has_active_textures()) {
            flush_guest_textures();
        }
        if (NumCommandLists == 0 || !ppCommandLists) {
            real_->ExecuteCommandLists(NumCommandLists, ppCommandLists);
            return;
        }
        if (NumCommandLists <= 32) {
            ID3D12CommandList* unwrapped[32];
            WsxD3D12CommandListProxy* proxies[32];
            for (UINT i = 0; i < NumCommandLists; ++i) {
                proxies[i] = WsxD3D12CommandListProxy::from_interface(ppCommandLists[i]);
                unwrapped[i] = proxies[i] ? proxies[i]->raw_list() : WsxD3D12CommandListProxy::unwrap(ppCommandLists[i]);
            }
            real_->ExecuteCommandLists(NumCommandLists, unwrapped);
            for (UINT i = 0; i < NumCommandLists; ++i) {
                if (proxies[i]) proxies[i]->commit_recorded_transitions();
            }
        } else {
            std::vector<ID3D12CommandList*> unwrapped(NumCommandLists);
            std::vector<WsxD3D12CommandListProxy*> proxies(NumCommandLists);
            for (UINT i = 0; i < NumCommandLists; ++i) {
                proxies[i] = WsxD3D12CommandListProxy::from_interface(ppCommandLists[i]);
                unwrapped[i] = proxies[i] ? proxies[i]->raw_list() : WsxD3D12CommandListProxy::unwrap(ppCommandLists[i]);
            }
            real_->ExecuteCommandLists(NumCommandLists, unwrapped.data());
            for (UINT i = 0; i < NumCommandLists; ++i) {
                if (proxies[i]) proxies[i]->commit_recorded_transitions();
            }
        }
    }

    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void* pData, UINT Size) override {
        real_->SetMarker(Metadata, pData, Size);
    }
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void* pData, UINT Size) override {
        real_->BeginEvent(Metadata, pData, Size);
    }
    void STDMETHODCALLTYPE EndEvent() override {
        real_->EndEvent();
    }
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* pFence, UINT64 Value) override {
        return real_->Signal(pFence, Value);
    }
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* pFence, UINT64 Value) override {
        return real_->Wait(pFence, Value);
    }
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* pFrequency) override {
        return real_->GetTimestampFrequency(pFrequency);
    }
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* pGpuTimestamp, UINT64* pCpuTimestamp) override {
        return real_->GetClockCalibration(pGpuTimestamp, pCpuTimestamp);
    }
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override {
        return real_->GetDesc();
    }
};

} // namespace wsx_d3d12
