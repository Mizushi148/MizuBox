// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Native Xbox Series X|S Direct3D 12.X Binary ABI COM Wrappers:
// - WsxXboxDescHeapAbiProxy:
//     * Implements Xbox D3D12.X RAX-return ABI at +0x48 (GetCPUDescriptorHandleForHeapStart)
//       and +0x50 (GetGPUDescriptorHandleForHeapStart) instead of desktop COM hidden out-ptr.
//     * Unwraps transparently via IWsxXboxDescHeapAbiMarker in SetDescriptorHeaps.
// - WsxXboxCommandListAbiProxy:
//     * Keeps Xbox inline PM4 buffer pointers (+0x18, +0x28, +0x30) at 0 so Scarlett binaries
//       always take the virtual-method slow path instead of writing raw PM4 packets.
//     * Reserves inline register cache padding (+0x038..+0x6FF) for Xbox inline state writes
//       (e.g. [cmdList+0x544]/[+0x554] in XboxBvhBuilder.dll).
//     * Maps Xbox ID3D12GraphicsCommandList vtable layout (including the +4 slot shift starting at
//       SetDescriptorHeaps=0x100 / SetComputeRootSignature=0x108 / SetComputeRootDescriptorTable=0x118,
//       plus Xbox DispatchX=0x280, inline barrier/sync=0x490, and AllocateGpuMemory=0x4A0 with
//       automatic UMA CPU-pointer -> desktop GPU VA translation in Set*RootSRV/CBV/UAV).
// - WsxXboxDeviceAbiProxy:
//     * Wraps WsxD3D12DeviceProxy with the Xbox Series X|S ID3D12Device vtable layout:
//       slot 62 (0x1F0) = 6-arg CreateComputePipelineStateX,
//       slot 76 (0x260) = 8-arg CreateCommittedResource,
//       slot 14 (0x070) = CreateDescriptorHeap (returns WsxXboxDescHeapAbiProxy),
//       slot 12 (0x060) = CreateCommandList (returns WsxXboxCommandListAbiProxy).

#pragma once

#include "d3d12_xs_proxies.h"

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wsx_d3d12 {

// Optional diagnostic counters queryable by tests via WSX_TRACE or internal helpers
struct WsxXboxAbiStats {
    std::atomic<uint64_t> desc_heaps_created{0};
    std::atomic<uint64_t> committed_resources_via_260{0};
    std::atomic<uint64_t> compute_pso_x_ok{0};
    std::atomic<uint64_t> compute_pso_x_fail{0};
    std::atomic<uint64_t> compute_pso_x_fallback{0};
    std::atomic<uint64_t> cmd_lists_wrapped{0};
    std::atomic<uint64_t> uma_alloc_gpu_mem_calls{0};
    std::atomic<uint64_t> uma_va_translations{0};
    std::atomic<uint64_t> unhandled_device_slots{0};
    std::atomic<uint64_t> unhandled_cmdlist_slots{0};
    // Wave 10 extended Xbox D3D12.X ABI telemetry
    std::atomic<uint64_t> graphics_pso_x_ok{0};
    std::atomic<uint64_t> placed_resources_x_ok{0};
    std::atomic<uint64_t> dispatch_mesh_calls{0};
    std::atomic<uint64_t> render_pass_calls{0};
    std::atomic<uint64_t> execute_indirect_calls{0};
    std::atomic<uint64_t> pix_markers_recorded{0};
};

inline WsxXboxAbiStats& xbox_abi_stats() noexcept {
    static WsxXboxAbiStats stats;
    return stats;
}

inline bool xbox_abi_env_flag(const wchar_t* name, bool default_val = false) noexcept {
    wchar_t buf[16]{};
    const DWORD len = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0) return default_val;
    return buf[0] == L'1' || buf[0] == L't' || buf[0] == L'T' || buf[0] == L'y' || buf[0] == L'Y';
}

// ============================================================================
// 1. WsxXboxDescHeapAbiProxy (RAX-return ABI at +0x48 and +0x50)
// ============================================================================

struct WsxXboxDescHeapAbiProxy {
    void** vtable;
    std::atomic<ULONG> ref{1};
    ComPtr<ID3D12DescriptorHeap> real;
    IUnknown* owner_xbox_dev = nullptr; // AddRef'd owner

    static WsxXboxDescHeapAbiProxy* create(ID3D12DescriptorHeap* raw_heap, IUnknown* owner_dev) noexcept;

    ~WsxXboxDescHeapAbiProxy() {
        if (owner_xbox_dev) {
            owner_xbox_dev->Release();
            owner_xbox_dev = nullptr;
        }
    }
};

namespace xbox_dh_detail {

inline HRESULT STDMETHODCALLTYPE qi(WsxXboxDescHeapAbiProxy* self, REFIID riid, void** ppv) noexcept {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IWsxXboxDescHeapAbiMarker)) {
        *ppv = self->real.Get();
        return S_OK; // Non-AddRef internal unwrap marker
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12DescriptorHeap)) {
        *ppv = self;
        self->ref.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    return self->real->QueryInterface(riid, ppv);
}

inline ULONG STDMETHODCALLTYPE addref(WsxXboxDescHeapAbiProxy* self) noexcept {
    return self->ref.fetch_add(1, std::memory_order_relaxed) + 1;
}

inline ULONG STDMETHODCALLTYPE release(WsxXboxDescHeapAbiProxy* self) noexcept {
    const ULONG r = self->ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (r == 0) {
        delete self;
    }
    return r;
}

inline HRESULT STDMETHODCALLTYPE get_priv(WsxXboxDescHeapAbiProxy* s, REFGUID g, UINT* sz, void* d) noexcept {
    return s->real->GetPrivateData(g, sz, d);
}
inline HRESULT STDMETHODCALLTYPE set_priv(WsxXboxDescHeapAbiProxy* s, REFGUID g, UINT sz, const void* d) noexcept {
    return s->real->SetPrivateData(g, sz, d);
}
inline HRESULT STDMETHODCALLTYPE set_priv_if(WsxXboxDescHeapAbiProxy* s, REFGUID g, const IUnknown* d) noexcept {
    return s->real->SetPrivateDataInterface(g, d);
}
inline HRESULT STDMETHODCALLTYPE set_name(WsxXboxDescHeapAbiProxy* s, LPCWSTR n) noexcept {
    return s->real->SetName(n);
}
inline HRESULT STDMETHODCALLTYPE get_dev(WsxXboxDescHeapAbiProxy* s, REFIID r, void** pp) noexcept {
    if (s->owner_xbox_dev) {
        return s->owner_xbox_dev->QueryInterface(r, pp);
    }
    return s->real->GetDevice(r, pp);
}
inline D3D12_DESCRIPTOR_HEAP_DESC* STDMETHODCALLTYPE get_desc(
    WsxXboxDescHeapAbiProxy* s, D3D12_DESCRIPTOR_HEAP_DESC* out) noexcept {
    *out = s->real->GetDesc();
    return out;
}
// Xbox D3D12.X ABI: returns 64-bit descriptor handle directly in RAX (1 arg: rcx=this)
inline SIZE_T STDMETHODCALLTYPE get_cpu_start_rax(WsxXboxDescHeapAbiProxy* s) noexcept {
    return s->real->GetCPUDescriptorHandleForHeapStart().ptr;
}
inline UINT64 STDMETHODCALLTYPE get_gpu_start_rax(WsxXboxDescHeapAbiProxy* s) noexcept {
    return s->real->GetGPUDescriptorHandleForHeapStart().ptr;
}

inline void** get_vtable() noexcept {
    static void* vt[11] = {
        reinterpret_cast<void*>(&qi),               // 0x00 (0)
        reinterpret_cast<void*>(&addref),           // 0x08 (1)
        reinterpret_cast<void*>(&release),          // 0x10 (2)
        reinterpret_cast<void*>(&get_priv),         // 0x18 (3)
        reinterpret_cast<void*>(&set_priv),         // 0x20 (4)
        reinterpret_cast<void*>(&set_priv_if),      // 0x28 (5)
        reinterpret_cast<void*>(&set_name),         // 0x30 (6)
        reinterpret_cast<void*>(&get_dev),          // 0x38 (7)
        reinterpret_cast<void*>(&get_desc),         // 0x40 (8)
        reinterpret_cast<void*>(&get_cpu_start_rax),// 0x48 (9) RAX return!
        reinterpret_cast<void*>(&get_gpu_start_rax),// 0x50 (10) RAX return!
    };
    return vt;
}

} // namespace xbox_dh_detail

inline WsxXboxDescHeapAbiProxy* WsxXboxDescHeapAbiProxy::create(
    ID3D12DescriptorHeap* raw_heap, IUnknown* owner_dev) noexcept {
    if (!raw_heap) return nullptr;
    auto* p = new (std::nothrow) WsxXboxDescHeapAbiProxy();
    if (!p) return nullptr;
    p->vtable = xbox_dh_detail::get_vtable();
    p->real = raw_heap;
    p->owner_xbox_dev = owner_dev;
    if (owner_dev) owner_dev->AddRef();
    xbox_abi_stats().desc_heaps_created.fetch_add(1, std::memory_order_relaxed);
    return p;
}

// ============================================================================
// 2. WsxXboxCommandListAbiProxy (Xbox ID3D12GraphicsCommandList binary ABI)
// ============================================================================

// Console-specific base & DMA interfaces discovered across Xbox Series X|S d3d12_xs.dll
// {aceeea63-e0a9-4a1c-bbec-71b2f485f758} — IID_IGraphicsUnknown (console base interface alongside IUnknown)
constexpr GUID kIidXboxGraphicsUnknown = {
    0xaceeea63u, 0xe0a9u, 0x4a1cu, {0xbbu, 0xecu, 0x71u, 0xb2u, 0xf4u, 0x85u, 0xf7u, 0x58u}
};
// {30bc77b9-8379-4b25-95de-b8588c42275d} — IID_ID3D12ConsDmaCommandList (console DMA copy command list)
constexpr GUID kIidXboxConsDmaCommandList = {
    0x30bc77b9u, 0x8379u, 0x4b25u, {0x95u, 0xdeu, 0xb8u, 0x58u, 0x8cu, 0x42u, 0x27u, 0x5du}
};

struct alignas(16) WsxXboxCommandListAbiProxy {
    void** vtable = nullptr;                // +0x000
    uint64_t reserved08 = 0;                // +0x008
    uint64_t reserved10 = 0;                // +0x010
    uint64_t pm4_current = 0;               // +0x018 (0 == pm4_limit -> force slow vtable path)
    uint64_t reserved20 = 0;                // +0x020
    uint64_t pm4_limit28 = 0;               // +0x028
    uint64_t pm4_limit30 = 0;               // +0x030
    uint8_t xbox_inline_state[0xFD0]{};     // +0x038..+0x1007 (full 0x1000-byte inline register cache scratch zone)

    std::atomic<ULONG> ref{1};
    IUnknown* owner_xbox_dev = nullptr;     // AddRef'd owning Xbox-ABI device (optional)
    ComPtr<ID3D12GraphicsCommandList> real0;
    ComPtr<ID3D12GraphicsCommandList1> real1;
    ComPtr<ID3D12GraphicsCommandList2> real2;
    ComPtr<ID3D12GraphicsCommandList3> real3;
    ComPtr<ID3D12GraphicsCommandList4> real4;
    ComPtr<ID3D12GraphicsCommandList5> real5;
    ComPtr<ID3D12GraphicsCommandList6> real6;

    // Lazily allocated UMA-compatible UPLOAD ring buffer for Xbox slot 0x4A0 (AllocateGpuMemory)
    ComPtr<ID3D12Resource> scratch_buf;
    uint8_t* scratch_cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS scratch_gpu_va = 0;
    size_t scratch_offset = 0;
    size_t scratch_capacity = 0;

    static WsxXboxCommandListAbiProxy* create(ID3D12GraphicsCommandList* list, IUnknown* owner_dev) noexcept;

    ~WsxXboxCommandListAbiProxy() {
        if (scratch_buf && scratch_cpu) {
            scratch_buf->Unmap(0, nullptr);
            scratch_cpu = nullptr;
        }
        if (owner_xbox_dev) {
            owner_xbox_dev->Release();
            owner_xbox_dev = nullptr;
        }
    }

    D3D12_GPU_VIRTUAL_ADDRESS translate_uma_va(D3D12_GPU_VIRTUAL_ADDRESS va) noexcept {
        if (scratch_cpu && scratch_capacity > 0) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(scratch_cpu);
            const uintptr_t addr = static_cast<uintptr_t>(va);
            if (addr >= base && addr < base + scratch_capacity) {
                xbox_abi_stats().uma_va_translations.fetch_add(1, std::memory_order_relaxed);
                return scratch_gpu_va + static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(addr - base);
            }
        }
        return va;
    }

    bool ensure_scratch_ring(size_t min_bytes) noexcept {
        if (scratch_cpu && scratch_capacity >= min_bytes) return true;
        if (scratch_buf) return false; // Already allocated
        ComPtr<ID3D12Device> dev;
        if (!real0 || FAILED(real0->GetDevice(IID_PPV_ARGS(dev.GetAddressOf()))) || !dev) {
            return false;
        }
        const size_t cap = std::max<size_t>(min_bytes, 1024u * 1024u); // 1 MB inline ring
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (cap + 255u) & ~255ULL;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(scratch_buf.GetAddressOf())))) {
            return false;
        }
        void* mapped = nullptr;
        if (FAILED(scratch_buf->Map(0, nullptr, &mapped)) || !mapped) {
            scratch_buf.Reset();
            return false;
        }
        scratch_cpu = static_cast<uint8_t*>(mapped);
        scratch_gpu_va = scratch_buf->GetGPUVirtualAddress();
        scratch_capacity = static_cast<size_t>(rd.Width);
        scratch_offset = 0;
        return true;
    }
};

namespace xbox_cl_detail {

template <uint32_t Slot>
inline void STDMETHODCALLTYPE unhandled(WsxXboxCommandListAbiProxy*) noexcept {
    xbox_abi_stats().unhandled_cmdlist_slots.fetch_add(1, std::memory_order_relaxed);
    wsx::trace_void("d3d12_xs", "WsxXboxCommandListAbiProxy::UnhandledSlot",
                    "slot=%u offset=0x%03X", Slot, Slot * 8u);
}

inline HRESULT STDMETHODCALLTYPE qi(WsxXboxCommandListAbiProxy* self, REFIID riid, void** ppv) noexcept {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IWsxXboxCommandListAbiMarker)) {
        *ppv = self->real0.Get();
        return S_OK; // Non-AddRef internal unwrap marker
    }
    if (riid == __uuidof(IUnknown) || riid == kIidXboxGraphicsUnknown ||
        riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12CommandList) ||
        riid == kIidXboxConsDmaCommandList ||
        riid == __uuidof(ID3D12GraphicsCommandList) ||
        (riid == __uuidof(ID3D12GraphicsCommandList1) && self->real1) ||
        (riid == __uuidof(ID3D12GraphicsCommandList2) && self->real2) ||
        (riid == __uuidof(ID3D12GraphicsCommandList3) && self->real3) ||
        (riid == __uuidof(ID3D12GraphicsCommandList4) && self->real4) ||
        (riid == __uuidof(ID3D12GraphicsCommandList5) && self->real5) ||
        (riid == __uuidof(ID3D12GraphicsCommandList6) && self->real6)) {
        *ppv = self;
        self->ref.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    return self->real0->QueryInterface(riid, ppv);
}
inline ULONG STDMETHODCALLTYPE addref(WsxXboxCommandListAbiProxy* s) noexcept {
    return s->ref.fetch_add(1, std::memory_order_relaxed) + 1;
}
inline ULONG STDMETHODCALLTYPE release(WsxXboxCommandListAbiProxy* s) noexcept {
    const ULONG r = s->ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (r == 0) delete s;
    return r;
}
inline HRESULT STDMETHODCALLTYPE get_priv(WsxXboxCommandListAbiProxy* s, REFGUID g, UINT* sz, void* d) noexcept { return s->real0->GetPrivateData(g, sz, d); }
inline HRESULT STDMETHODCALLTYPE set_priv(WsxXboxCommandListAbiProxy* s, REFGUID g, UINT sz, const void* d) noexcept { return s->real0->SetPrivateData(g, sz, d); }
inline HRESULT STDMETHODCALLTYPE set_priv_if(WsxXboxCommandListAbiProxy* s, REFGUID g, const IUnknown* d) noexcept { return s->real0->SetPrivateDataInterface(g, d); }
inline HRESULT STDMETHODCALLTYPE set_name(WsxXboxCommandListAbiProxy* s, LPCWSTR n) noexcept { return s->real0->SetName(n); }
inline HRESULT STDMETHODCALLTYPE get_dev(WsxXboxCommandListAbiProxy* s, REFIID r, void** pp) noexcept {
    if (s->owner_xbox_dev) return s->owner_xbox_dev->QueryInterface(r, pp);
    return s->real0->GetDevice(r, pp);
}
inline D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE get_type(WsxXboxCommandListAbiProxy* s) noexcept { return s->real0->GetType(); }
inline HRESULT STDMETHODCALLTYPE close_cl(WsxXboxCommandListAbiProxy* s) noexcept { return s->real0->Close(); }
inline HRESULT STDMETHODCALLTYPE reset_cl(WsxXboxCommandListAbiProxy* s, ID3D12CommandAllocator* a, ID3D12PipelineState* p) noexcept {
    s->scratch_offset = 0;
    return s->real0->Reset(a, p);
}
inline void STDMETHODCALLTYPE clear_state(WsxXboxCommandListAbiProxy* s, ID3D12PipelineState* p) noexcept { s->real0->ClearState(p); }
inline void STDMETHODCALLTYPE draw_inst(WsxXboxCommandListAbiProxy* s, UINT vc, UINT ic, UINT sv, UINT si) noexcept { s->real0->DrawInstanced(vc, ic, sv, si); }
inline void STDMETHODCALLTYPE draw_idx_inst(WsxXboxCommandListAbiProxy* s, UINT idx, UINT ic, UINT si, INT bv, UINT st) noexcept { s->real0->DrawIndexedInstanced(idx, ic, si, bv, st); }
inline void STDMETHODCALLTYPE dispatch(WsxXboxCommandListAbiProxy* s, UINT x, UINT y, UINT z) noexcept { s->real0->Dispatch(x, y, z); }
inline void STDMETHODCALLTYPE copy_buf(WsxXboxCommandListAbiProxy* s, ID3D12Resource* dst, UINT64 doff, ID3D12Resource* src, UINT64 soff, UINT64 n) noexcept { s->real0->CopyBufferRegion(dst, doff, src, soff, n); }
inline void STDMETHODCALLTYPE copy_tex(WsxXboxCommandListAbiProxy* s, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) noexcept { s->real0->CopyTextureRegion(dst, x, y, z, src, box); }
inline void STDMETHODCALLTYPE copy_res(WsxXboxCommandListAbiProxy* s, ID3D12Resource* dst, ID3D12Resource* src) noexcept { s->real0->CopyResource(dst, src); }
inline void STDMETHODCALLTYPE copy_tiles(WsxXboxCommandListAbiProxy* s, ID3D12Resource* r, const D3D12_TILED_RESOURCE_COORDINATE* c, const D3D12_TILE_REGION_SIZE* sz, ID3D12Resource* b, UINT64 off, D3D12_TILE_COPY_FLAGS f) noexcept { s->real0->CopyTiles(r, c, sz, b, off, f); }
inline void STDMETHODCALLTYPE resolve_sub(WsxXboxCommandListAbiProxy* s, ID3D12Resource* d, UINT ds, ID3D12Resource* src, UINT ss, DXGI_FORMAT f) noexcept { s->real0->ResolveSubresource(d, ds, src, ss, f); }
inline void STDMETHODCALLTYPE ia_topo(WsxXboxCommandListAbiProxy* s, D3D12_PRIMITIVE_TOPOLOGY t) noexcept { s->real0->IASetPrimitiveTopology(t); }
inline void STDMETHODCALLTYPE rs_vp(WsxXboxCommandListAbiProxy* s, UINT n, const D3D12_VIEWPORT* v) noexcept { s->real0->RSSetViewports(n, v); }
inline void STDMETHODCALLTYPE rs_sc(WsxXboxCommandListAbiProxy* s, UINT n, const D3D12_RECT* r) noexcept { s->real0->RSSetScissorRects(n, r); }
inline void STDMETHODCALLTYPE om_blend(WsxXboxCommandListAbiProxy* s, const FLOAT b[4]) noexcept { s->real0->OMSetBlendFactor(b); }
inline void STDMETHODCALLTYPE om_stencil(WsxXboxCommandListAbiProxy* s, UINT r) noexcept { s->real0->OMSetStencilRef(r); }
inline void STDMETHODCALLTYPE set_pso(WsxXboxCommandListAbiProxy* s, ID3D12PipelineState* p) noexcept {
    if (p) s->real0->SetPipelineState(p);
}
inline void STDMETHODCALLTYPE res_barrier(WsxXboxCommandListAbiProxy* s, UINT n, const D3D12_RESOURCE_BARRIER* b) noexcept {
    if (n == 0 || !b) return;
    D3D12_RESOURCE_BARRIER stack_buf[16];
    std::vector<D3D12_RESOURCE_BARRIER> heap_buf;
    D3D12_RESOURCE_BARRIER* out = stack_buf;
    if (n > 16) {
        heap_buf.resize(n);
        out = heap_buf.data();
    }
    UINT out_count = 0;
    for (UINT i = 0; i < n; ++i) {
        D3D12_RESOURCE_BARRIER cur = b[i];
        if (cur.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            cur.Transition.StateBefore = sanitize_xbox_transition_state(cur.Transition.StateBefore);
            cur.Transition.StateAfter = sanitize_xbox_transition_state(cur.Transition.StateAfter);
            if (auto* rp = WsxD3D12ResourceProxy::from_interface(cur.Transition.pResource)) {
                cur.Transition.pResource = rp->raw_resource();
                if ((cur.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) == 0 && rp->guest_record()) {
                    std::lock_guard<std::mutex> rlk(rp->guest_record()->record_mutex);
                    rp->guest_record()->gpu_state = cur.Transition.StateAfter;
                }
            } else {
                cur.Transition.pResource = unwrap_resource(cur.Transition.pResource);
            }
            if (cur.Transition.StateBefore == cur.Transition.StateAfter) continue;
        } else if (cur.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            cur.Aliasing.pResourceBefore = unwrap_resource(cur.Aliasing.pResourceBefore);
            cur.Aliasing.pResourceAfter = unwrap_resource(cur.Aliasing.pResourceAfter);
        } else if (cur.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
            cur.UAV.pResource = unwrap_resource(cur.UAV.pResource);
        }
        out[out_count++] = cur;
    }
    if (out_count > 0) {
        s->real0->ResourceBarrier(out_count, out);
    }
}
inline void STDMETHODCALLTYPE exec_bundle(WsxXboxCommandListAbiProxy* s, ID3D12GraphicsCommandList* b) noexcept {
    auto* raw_b = static_cast<ID3D12GraphicsCommandList*>(WsxD3D12CommandListProxy::unwrap(b));
    s->real0->ExecuteBundle(raw_b);
}
// Xbox CommandList +4 shifted slots (empirically verified in XboxBvhBuilder.dll & amd_fidelityfx_upscaler_xbox.dll):
inline void STDMETHODCALLTYPE set_desc_heaps(WsxXboxCommandListAbiProxy* s, UINT n, ID3D12DescriptorHeap* const* heaps) noexcept {
    if (n == 0 || !heaps) {
        s->real0->SetDescriptorHeaps(n, heaps);
        return;
    }
    ID3D12DescriptorHeap* unwrapped[8];
    if (n <= 8) {
        for (UINT i = 0; i < n; ++i) unwrapped[i] = unwrap_descriptor_heap(heaps[i]);
        s->real0->SetDescriptorHeaps(n, unwrapped);
    } else {
        std::vector<ID3D12DescriptorHeap*> vec(n);
        for (UINT i = 0; i < n; ++i) vec[i] = unwrap_descriptor_heap(heaps[i]);
        s->real0->SetDescriptorHeaps(n, vec.data());
    }
}
inline void STDMETHODCALLTYPE set_comp_rs(WsxXboxCommandListAbiProxy* s, ID3D12RootSignature* rs) noexcept { if (rs) s->real0->SetComputeRootSignature(rs); }
inline void STDMETHODCALLTYPE set_gfx_rs(WsxXboxCommandListAbiProxy* s, ID3D12RootSignature* rs) noexcept { if (rs) s->real0->SetGraphicsRootSignature(rs); }
inline void STDMETHODCALLTYPE set_comp_table(WsxXboxCommandListAbiProxy* s, UINT idx, D3D12_GPU_DESCRIPTOR_HANDLE h) noexcept { s->real0->SetComputeRootDescriptorTable(idx, h); }
inline void STDMETHODCALLTYPE set_gfx_table(WsxXboxCommandListAbiProxy* s, UINT idx, D3D12_GPU_DESCRIPTOR_HANDLE h) noexcept { s->real0->SetGraphicsRootDescriptorTable(idx, h); }
inline void STDMETHODCALLTYPE set_comp_const(WsxXboxCommandListAbiProxy* s, UINT idx, UINT v, UINT off) noexcept { s->real0->SetComputeRoot32BitConstant(idx, v, off); }
inline void STDMETHODCALLTYPE set_gfx_const(WsxXboxCommandListAbiProxy* s, UINT idx, UINT v, UINT off) noexcept { s->real0->SetGraphicsRoot32BitConstant(idx, v, off); }
inline void STDMETHODCALLTYPE set_comp_consts(WsxXboxCommandListAbiProxy* s, UINT idx, UINT cnt, const void* d, UINT off) noexcept { if (d) s->real0->SetComputeRoot32BitConstants(idx, cnt, d, off); }
inline void STDMETHODCALLTYPE set_gfx_consts(WsxXboxCommandListAbiProxy* s, UINT idx, UINT cnt, const void* d, UINT off) noexcept { if (d) s->real0->SetGraphicsRoot32BitConstants(idx, cnt, d, off); }
inline void STDMETHODCALLTYPE set_comp_cbv(WsxXboxCommandListAbiProxy* s, UINT idx, D3D12_GPU_VIRTUAL_ADDRESS va) noexcept { s->real0->SetComputeRootConstantBufferView(idx, s->translate_uma_va(va)); }
inline void STDMETHODCALLTYPE set_gfx_cbv(WsxXboxCommandListAbiProxy* s, UINT idx, D3D12_GPU_VIRTUAL_ADDRESS va) noexcept { s->real0->SetGraphicsRootConstantBufferView(idx, s->translate_uma_va(va)); }
inline void STDMETHODCALLTYPE set_comp_srv(WsxXboxCommandListAbiProxy* s, UINT idx, D3D12_GPU_VIRTUAL_ADDRESS va) noexcept { s->real0->SetComputeRootShaderResourceView(idx, s->translate_uma_va(va)); }
inline void STDMETHODCALLTYPE set_gfx_srv(WsxXboxCommandListAbiProxy* s, UINT idx, D3D12_GPU_VIRTUAL_ADDRESS va) noexcept { s->real0->SetGraphicsRootShaderResourceView(idx, s->translate_uma_va(va)); }
inline void STDMETHODCALLTYPE set_comp_uav(WsxXboxCommandListAbiProxy* s, UINT idx, D3D12_GPU_VIRTUAL_ADDRESS va) noexcept { s->real0->SetComputeRootUnorderedAccessView(idx, s->translate_uma_va(va)); }
inline void STDMETHODCALLTYPE set_gfx_uav(WsxXboxCommandListAbiProxy* s, UINT idx, D3D12_GPU_VIRTUAL_ADDRESS va) noexcept { s->real0->SetGraphicsRootUnorderedAccessView(idx, s->translate_uma_va(va)); }
inline void STDMETHODCALLTYPE ia_ib(WsxXboxCommandListAbiProxy* s, const D3D12_INDEX_BUFFER_VIEW* v) noexcept { s->real0->IASetIndexBuffer(v); }
inline void STDMETHODCALLTYPE ia_vb(WsxXboxCommandListAbiProxy* s, UINT st, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v) noexcept { s->real0->IASetVertexBuffers(st, n, v); }
inline void STDMETHODCALLTYPE so_targets(WsxXboxCommandListAbiProxy* s, UINT st, UINT n, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* v) noexcept { s->real0->SOSetTargets(st, n, v); }
inline void STDMETHODCALLTYPE om_rt(WsxXboxCommandListAbiProxy* s, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) noexcept { s->real0->OMSetRenderTargets(n, rts, single, dsv); }
inline void STDMETHODCALLTYPE clear_dsv(WsxXboxCommandListAbiProxy* s, D3D12_CPU_DESCRIPTOR_HANDLE h, D3D12_CLEAR_FLAGS f, FLOAT d, UINT8 st, UINT n, const D3D12_RECT* r) noexcept { s->real0->ClearDepthStencilView(h, f, d, st, n, r); }
inline void STDMETHODCALLTYPE clear_rtv(WsxXboxCommandListAbiProxy* s, D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT c[4], UINT n, const D3D12_RECT* r) noexcept { s->real0->ClearRenderTargetView(h, c, n, r); }
inline void STDMETHODCALLTYPE clear_uav_u(WsxXboxCommandListAbiProxy* s, D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource* res, const UINT v[4], UINT n, const D3D12_RECT* r) noexcept { s->real0->ClearUnorderedAccessViewUint(g, c, res, v, n, r); }
inline void STDMETHODCALLTYPE clear_uav_f(WsxXboxCommandListAbiProxy* s, D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource* res, const FLOAT v[4], UINT n, const D3D12_RECT* r) noexcept { s->real0->ClearUnorderedAccessViewFloat(g, c, res, v, n, r); }
inline void STDMETHODCALLTYPE discard_res(WsxXboxCommandListAbiProxy* s, ID3D12Resource* r, const D3D12_DISCARD_REGION* reg) noexcept { s->real0->DiscardResource(r, reg); }
inline void STDMETHODCALLTYPE begin_q(WsxXboxCommandListAbiProxy* s, ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT i) noexcept { s->real0->BeginQuery(h, t, i); }
inline void STDMETHODCALLTYPE end_q(WsxXboxCommandListAbiProxy* s, ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT i) noexcept { s->real0->EndQuery(h, t, i); }
inline void STDMETHODCALLTYPE resolve_q(WsxXboxCommandListAbiProxy* s, ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT si, UINT n, ID3D12Resource* d, UINT64 off) noexcept { s->real0->ResolveQueryData(h, t, si, n, d, off); }
inline void STDMETHODCALLTYPE set_pred(WsxXboxCommandListAbiProxy* s, ID3D12Resource* b, UINT64 off, D3D12_PREDICATION_OP op) noexcept { s->real0->SetPredication(b, off, op); }
inline void STDMETHODCALLTYPE set_marker(WsxXboxCommandListAbiProxy* s, UINT m, const void* d, UINT sz) noexcept {
    xbox_abi_stats().pix_markers_recorded.fetch_add(1, std::memory_order_relaxed);
    s->real0->SetMarker(m, d, sz);
}
inline void STDMETHODCALLTYPE begin_evt(WsxXboxCommandListAbiProxy* s, UINT m, const void* d, UINT sz) noexcept {
    xbox_abi_stats().pix_markers_recorded.fetch_add(1, std::memory_order_relaxed);
    s->real0->BeginEvent(m, d, sz);
}
inline void STDMETHODCALLTYPE end_evt(WsxXboxCommandListAbiProxy* s) noexcept {
    xbox_abi_stats().pix_markers_recorded.fetch_add(1, std::memory_order_relaxed);
    s->real0->EndEvent();
}
inline void STDMETHODCALLTYPE exec_indirect(WsxXboxCommandListAbiProxy* s, ID3D12CommandSignature* sig, UINT maxC, ID3D12Resource* argB, UINT64 argOff, ID3D12Resource* cntB, UINT64 cntOff) noexcept {
    xbox_abi_stats().execute_indirect_calls.fetch_add(1, std::memory_order_relaxed);
    s->real0->ExecuteIndirect(sig, maxC, unwrap_resource(argB), argOff, unwrap_resource(cntB), cntOff);
}
// Extended GraphicsCommandList1..6 Xbox ABI slots (+4 shifted)
inline void STDMETHODCALLTYPE om_depth_bounds(WsxXboxCommandListAbiProxy* s, FLOAT mn, FLOAT mx) noexcept {
    if (s->real1) s->real1->OMSetDepthBounds(mn, mx);
}
inline void STDMETHODCALLTYPE set_sample_pos(WsxXboxCommandListAbiProxy* s, UINT ns, UINT np, D3D12_SAMPLE_POSITION* p) noexcept {
    if (s->real1) s->real1->SetSamplePositions(ns, np, p);
}
inline void STDMETHODCALLTYPE resolve_sub_region(WsxXboxCommandListAbiProxy* s, ID3D12Resource* d, UINT ds, UINT dx, UINT dy, ID3D12Resource* src, UINT ss, D3D12_RECT* r, DXGI_FORMAT f, D3D12_RESOLVE_MODE m) noexcept {
    if (s->real1) s->real1->ResolveSubresourceRegion(unwrap_resource(d), ds, dx, dy, unwrap_resource(src), ss, r, f, m);
}
inline void STDMETHODCALLTYPE set_view_inst_mask(WsxXboxCommandListAbiProxy* s, UINT mask) noexcept {
    if (s->real1) s->real1->SetViewInstanceMask(mask);
}
inline void STDMETHODCALLTYPE write_buf_imm(WsxXboxCommandListAbiProxy* s, UINT count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* p, const D3D12_WRITEBUFFERIMMEDIATE_MODE* m) noexcept {
    if (!s->real2 || !p || count == 0) return;
    std::vector<D3D12_WRITEBUFFERIMMEDIATE_PARAMETER> translated(count);
    for (UINT i = 0; i < count; ++i) {
        translated[i] = p[i];
        translated[i].Dest = s->translate_uma_va(p[i].Dest);
    }
    s->real2->WriteBufferImmediate(count, translated.data(), m);
}
inline void STDMETHODCALLTYPE begin_render_pass(WsxXboxCommandListAbiProxy* s, UINT nrt, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rts, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds, D3D12_RENDER_PASS_FLAGS fl) noexcept {
    xbox_abi_stats().render_pass_calls.fetch_add(1, std::memory_order_relaxed);
    if (s->real4) s->real4->BeginRenderPass(nrt, rts, ds, fl);
}
inline void STDMETHODCALLTYPE end_render_pass(WsxXboxCommandListAbiProxy* s) noexcept {
    if (s->real4) s->real4->EndRenderPass();
}
inline void STDMETHODCALLTYPE build_rt_as(WsxXboxCommandListAbiProxy* s, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* d, UINT np, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* p) noexcept {
    if (!s->real4 || !d) return;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC copy = *d;
    copy.DestAccelerationStructureData = s->translate_uma_va(d->DestAccelerationStructureData);
    copy.SourceAccelerationStructureData = s->translate_uma_va(d->SourceAccelerationStructureData);
    copy.ScratchAccelerationStructureData = s->translate_uma_va(d->ScratchAccelerationStructureData);
    s->real4->BuildRaytracingAccelerationStructure(&copy, np, p);
}
inline void STDMETHODCALLTYPE emit_rt_postbuild(WsxXboxCommandListAbiProxy* s, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* d, UINT n, const D3D12_GPU_VIRTUAL_ADDRESS* src) noexcept {
    if (s->real4) s->real4->EmitRaytracingAccelerationStructurePostbuildInfo(d, n, src);
}
inline void STDMETHODCALLTYPE copy_rt_as(WsxXboxCommandListAbiProxy* s, D3D12_GPU_VIRTUAL_ADDRESS dst, D3D12_GPU_VIRTUAL_ADDRESS src, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE m) noexcept {
    if (s->real4) s->real4->CopyRaytracingAccelerationStructure(s->translate_uma_va(dst), s->translate_uma_va(src), m);
}
inline void STDMETHODCALLTYPE set_pso1(WsxXboxCommandListAbiProxy* s, ID3D12StateObject* so) noexcept {
    if (s->real4) s->real4->SetPipelineState1(so);
}
inline void STDMETHODCALLTYPE dispatch_rays(WsxXboxCommandListAbiProxy* s, const D3D12_DISPATCH_RAYS_DESC* d) noexcept {
    if (s->real4 && d) s->real4->DispatchRays(d);
}
inline void STDMETHODCALLTYPE rs_set_shading_rate(WsxXboxCommandListAbiProxy* s, D3D12_SHADING_RATE r, const D3D12_SHADING_RATE_COMBINER* c) noexcept {
    if (s->real5) s->real5->RSSetShadingRate(r, c);
}
inline void STDMETHODCALLTYPE rs_set_shading_rate_img(WsxXboxCommandListAbiProxy* s, ID3D12Resource* img) noexcept {
    if (s->real5) s->real5->RSSetShadingRateImage(unwrap_resource(img));
}
inline void STDMETHODCALLTYPE dispatch_mesh(WsxXboxCommandListAbiProxy* s, UINT x, UINT y, UINT z) noexcept {
    xbox_abi_stats().dispatch_mesh_calls.fetch_add(1, std::memory_order_relaxed);
    if (s->real6) s->real6->DispatchMesh(x, y, z);
}

// Xbox D3D12.X slot 80 (0x280): DispatchX(x, y, z, flags)
inline void STDMETHODCALLTYPE dispatch_x(WsxXboxCommandListAbiProxy* s, UINT x, UINT y, UINT z, UINT /*flags*/) noexcept {
    s->real0->Dispatch(x, y, z);
}

// Xbox D3D12.X slot 146 (0x490): inline GPU barrier / WriteBufferImmediate sync
inline void STDMETHODCALLTYPE xbox_inline_sync_490(WsxXboxCommandListAbiProxy* s) noexcept {
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = nullptr;
    s->real0->ResourceBarrier(1, &uav);
}

// Xbox D3D12.X slot 148 (0x4A0): AllocateGpuMemory(sizeBytes, alignment) -> UMA CPU/GPU ptr
inline void* STDMETHODCALLTYPE xbox_alloc_gpu_mem_4a0(
    WsxXboxCommandListAbiProxy* s, UINT size_bytes, UINT align) noexcept {
    xbox_abi_stats().uma_alloc_gpu_mem_calls.fetch_add(1, std::memory_order_relaxed);
    if (size_bytes == 0) return nullptr;
    const size_t a = std::max<size_t>(align ? align : 16u, 16u);
    if (!s->ensure_scratch_ring(size_bytes + a)) return nullptr;
    const size_t aligned = (s->scratch_offset + a - 1u) & ~(a - 1u);
    if (aligned + size_bytes > s->scratch_capacity) return nullptr;
    s->scratch_offset = aligned + size_bytes;
    return s->scratch_cpu + aligned;
}

template <size_t... Is>
inline std::array<void*, 180> make_vtable_impl(std::index_sequence<Is...>) noexcept {
    std::array<void*, 180> vt{reinterpret_cast<void*>(&unhandled<static_cast<uint32_t>(Is)>)...};
    vt[0]   = reinterpret_cast<void*>(&qi);
    vt[1]   = reinterpret_cast<void*>(&addref);
    vt[2]   = reinterpret_cast<void*>(&release);
    vt[3]   = reinterpret_cast<void*>(&get_priv);
    vt[4]   = reinterpret_cast<void*>(&set_priv);
    vt[5]   = reinterpret_cast<void*>(&set_priv_if);
    vt[6]   = reinterpret_cast<void*>(&set_name);
    vt[7]   = reinterpret_cast<void*>(&get_dev);
    vt[8]   = reinterpret_cast<void*>(&get_type);
    vt[9]   = reinterpret_cast<void*>(&close_cl);
    vt[10]  = reinterpret_cast<void*>(&reset_cl);
    vt[11]  = reinterpret_cast<void*>(&clear_state);
    vt[12]  = reinterpret_cast<void*>(&draw_inst);
    vt[13]  = reinterpret_cast<void*>(&draw_idx_inst);
    vt[14]  = reinterpret_cast<void*>(&dispatch);        // 0x070
    vt[15]  = reinterpret_cast<void*>(&copy_buf);
    vt[16]  = reinterpret_cast<void*>(&copy_tex);
    vt[17]  = reinterpret_cast<void*>(&copy_res);
    vt[18]  = reinterpret_cast<void*>(&copy_tiles);
    vt[19]  = reinterpret_cast<void*>(&resolve_sub);
    vt[20]  = reinterpret_cast<void*>(&ia_topo);
    vt[21]  = reinterpret_cast<void*>(&rs_vp);
    vt[22]  = reinterpret_cast<void*>(&rs_sc);
    vt[23]  = reinterpret_cast<void*>(&om_blend);
    vt[24]  = reinterpret_cast<void*>(&om_stencil);
    vt[25]  = reinterpret_cast<void*>(&set_pso);         // 0x0C8
    vt[26]  = reinterpret_cast<void*>(&res_barrier);     // 0x0D0
    vt[27]  = reinterpret_cast<void*>(&exec_bundle);     // 0x0D8
    // Xbox D3D12.X +4 shifted slots (slots 28..31 are Xbox-inserted)
    vt[32]  = reinterpret_cast<void*>(&set_desc_heaps);  // 0x100 (FSR4 verified)
    vt[33]  = reinterpret_cast<void*>(&set_comp_rs);     // 0x108 (XboxBvhBuilder + FSR4 verified)
    vt[34]  = reinterpret_cast<void*>(&set_gfx_rs);      // 0x110
    vt[35]  = reinterpret_cast<void*>(&set_comp_table);  // 0x118 (FSR4 verified)
    vt[36]  = reinterpret_cast<void*>(&set_gfx_table);   // 0x120
    vt[37]  = reinterpret_cast<void*>(&set_comp_const);  // 0x128 (XboxBvhBuilder verified)
    vt[38]  = reinterpret_cast<void*>(&set_gfx_const);   // 0x130
    vt[39]  = reinterpret_cast<void*>(&set_comp_consts); // 0x138 (XboxBvhBuilder verified)
    vt[40]  = reinterpret_cast<void*>(&set_gfx_consts);  // 0x140
    vt[41]  = reinterpret_cast<void*>(&set_comp_cbv);    // 0x148
    vt[42]  = reinterpret_cast<void*>(&set_gfx_cbv);     // 0x150
    vt[43]  = reinterpret_cast<void*>(&set_comp_srv);    // 0x158 (XboxBvhBuilder verified)
    vt[44]  = reinterpret_cast<void*>(&set_gfx_srv);     // 0x160
    vt[45]  = reinterpret_cast<void*>(&set_comp_uav);    // 0x168 (XboxBvhBuilder verified)
    vt[46]  = reinterpret_cast<void*>(&set_gfx_uav);     // 0x170
    vt[47]  = reinterpret_cast<void*>(&ia_ib);
    vt[48]  = reinterpret_cast<void*>(&ia_vb);
    vt[49]  = reinterpret_cast<void*>(&so_targets);
    vt[50]  = reinterpret_cast<void*>(&om_rt);
    vt[51]  = reinterpret_cast<void*>(&clear_dsv);
    vt[52]  = reinterpret_cast<void*>(&clear_rtv);
    vt[53]  = reinterpret_cast<void*>(&clear_uav_u);
    vt[54]  = reinterpret_cast<void*>(&clear_uav_f);
    vt[55]  = reinterpret_cast<void*>(&discard_res);
    vt[56]  = reinterpret_cast<void*>(&begin_q);
    vt[57]  = reinterpret_cast<void*>(&end_q);
    vt[58]  = reinterpret_cast<void*>(&resolve_q);
    vt[59]  = reinterpret_cast<void*>(&set_pred);
    vt[60]  = reinterpret_cast<void*>(&set_marker);
    vt[61]  = reinterpret_cast<void*>(&begin_evt);
    vt[62]  = reinterpret_cast<void*>(&end_evt);
    vt[63]  = reinterpret_cast<void*>(&exec_indirect);
    vt[65]  = reinterpret_cast<void*>(&om_depth_bounds);
    vt[66]  = reinterpret_cast<void*>(&set_sample_pos);
    vt[67]  = reinterpret_cast<void*>(&resolve_sub_region);
    vt[68]  = reinterpret_cast<void*>(&set_view_inst_mask);
    vt[69]  = reinterpret_cast<void*>(&write_buf_imm);
    vt[71]  = reinterpret_cast<void*>(&begin_render_pass);      // 0x238
    vt[72]  = reinterpret_cast<void*>(&end_render_pass);        // 0x240
    vt[75]  = reinterpret_cast<void*>(&build_rt_as);            // 0x258
    vt[76]  = reinterpret_cast<void*>(&emit_rt_postbuild);      // 0x260
    vt[77]  = reinterpret_cast<void*>(&copy_rt_as);             // 0x268
    vt[78]  = reinterpret_cast<void*>(&set_pso1);               // 0x270
    vt[79]  = reinterpret_cast<void*>(&dispatch_rays);          // 0x278
    vt[80]  = reinterpret_cast<void*>(&dispatch_x);             // 0x280
    vt[81]  = reinterpret_cast<void*>(&rs_set_shading_rate);    // 0x288
    vt[82]  = reinterpret_cast<void*>(&rs_set_shading_rate_img);// 0x290
    vt[83]  = reinterpret_cast<void*>(&dispatch_mesh);          // 0x298
    vt[146] = reinterpret_cast<void*>(&xbox_inline_sync_490);   // 0x490
    vt[148] = reinterpret_cast<void*>(&xbox_alloc_gpu_mem_4a0); // 0x4A0
    return vt;
}

inline void** get_vtable() noexcept {
    static auto vt = make_vtable_impl(std::make_index_sequence<180>{});
    return vt.data();
}

} // namespace xbox_cl_detail

inline WsxXboxCommandListAbiProxy* WsxXboxCommandListAbiProxy::create(
    ID3D12GraphicsCommandList* list, IUnknown* owner_dev) noexcept {
    if (!list) return nullptr;
    auto* unwrapped = static_cast<ID3D12GraphicsCommandList*>(WsxD3D12CommandListProxy::unwrap(list));
    auto* p = new (std::nothrow) WsxXboxCommandListAbiProxy();
    if (!p) return nullptr;
    p->vtable = xbox_cl_detail::get_vtable();
    p->real0 = unwrapped;
    p->real0.As(&p->real1);
    p->real0.As(&p->real2);
    p->real0.As(&p->real3);
    p->real0.As(&p->real4);
    p->real0.As(&p->real5);
    p->real0.As(&p->real6);
    p->owner_xbox_dev = owner_dev;
    if (owner_dev) owner_dev->AddRef();
    xbox_abi_stats().cmd_lists_wrapped.fetch_add(1, std::memory_order_relaxed);
    return p;
}

// ============================================================================
// 3. WsxXboxDeviceAbiProxy (Xbox ID3D12Device / ID3D12Device11 binary ABI)
// ============================================================================

struct WsxXboxDeviceAbiProxy {
    void** vtable = nullptr;                // +0x00
    std::atomic<ULONG> ref{1};              // +0x08
    uint32_t reserved0C = 0u;               // +0x0C
    uint64_t reserved10 = 0ULL;             // +0x10
    // Leonida discovery: amd_fidelityfx_upscaler_xbox.dll reads *(uint8_t*)((uintptr_t)pDevice + 0x18).
    // If 0, it bypasses CopyDescriptorsSimple and performs raw 32-byte memcpy between CPU handles!
    // Setting byte +0x18 to 1 forces it through vtable slot 24 (CopyDescriptorsSimple).
    uint8_t copy_desc_vtable_flag_18 = 1u;  // +0x18
    uint8_t pad19[7]{};                     // +0x19..+0x1F
    ComPtr<ID3D12Device> dev0;
    ComPtr<ID3D12Device1> dev1;
    ComPtr<ID3D12Device2> dev2;
    ComPtr<ID3D12Device4> dev4;
    ComPtr<ID3D12Device8> dev8;

    std::mutex fallback_mtx;
    ComPtr<ID3D12PipelineState> fallback_compute_pso;

    static WsxXboxDeviceAbiProxy* create(ID3D12Device* base_device) noexcept;
};
static_assert(offsetof(WsxXboxDeviceAbiProxy, copy_desc_vtable_flag_18) == 0x18,
              "WsxXboxDeviceAbiProxy byte +0x18 flag must sit at exact offset 0x18");

namespace xbox_dev_detail {

template <uint32_t Slot>
inline HRESULT STDMETHODCALLTYPE unhandled(WsxXboxDeviceAbiProxy*) noexcept {
    xbox_abi_stats().unhandled_device_slots.fetch_add(1, std::memory_order_relaxed);
    wsx::trace_hr("d3d12_xs", "WsxXboxDeviceAbiProxy::UnhandledSlot", E_NOTIMPL,
                  "slot=%u offset=0x%03X", Slot, Slot * 8u);
    return E_NOTIMPL;
}

inline HRESULT STDMETHODCALLTYPE qi(WsxXboxDeviceAbiProxy* self, REFIID riid, void** ppv) noexcept {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IWsxXboxDeviceAbiMarker)) {
        *ppv = self->dev0.Get();
        return S_OK; // Non-AddRef internal unwrap marker
    }
    if (riid == __uuidof(IUnknown) || riid == kIidXboxGraphicsUnknown ||
        riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12Device) || riid == __uuidof(ID3D12Device1) ||
        riid == __uuidof(ID3D12Device2) || riid == __uuidof(ID3D12Device3) ||
        riid == __uuidof(ID3D12Device4) || riid == __uuidof(ID3D12Device5) ||
        riid == __uuidof(ID3D12Device6) || riid == __uuidof(ID3D12Device7) ||
        riid == __uuidof(ID3D12Device8)) {
        *ppv = self;
        self->ref.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    return self->dev0->QueryInterface(riid, ppv);
}

// Xbox Device slots 68 (0x220) and 69 (0x228): device info query / device setting
inline HRESULT STDMETHODCALLTYPE xbox_dev_info_68(WsxXboxDeviceAbiProxy*) noexcept {
    return S_OK;
}
inline HRESULT STDMETHODCALLTYPE xbox_dev_setting_69(WsxXboxDeviceAbiProxy*, UINT, UINT) noexcept {
    return S_OK;
}
inline ULONG STDMETHODCALLTYPE addref(WsxXboxDeviceAbiProxy* s) noexcept {
    return s->ref.fetch_add(1, std::memory_order_relaxed) + 1;
}
inline ULONG STDMETHODCALLTYPE release(WsxXboxDeviceAbiProxy* s) noexcept {
    const ULONG r = s->ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (r == 0) delete s;
    return r;
}
inline HRESULT STDMETHODCALLTYPE get_priv(WsxXboxDeviceAbiProxy* s, REFGUID g, UINT* sz, void* d) noexcept { return s->dev0->GetPrivateData(g, sz, d); }
inline HRESULT STDMETHODCALLTYPE set_priv(WsxXboxDeviceAbiProxy* s, REFGUID g, UINT sz, const void* d) noexcept { return s->dev0->SetPrivateData(g, sz, d); }
inline HRESULT STDMETHODCALLTYPE set_priv_if(WsxXboxDeviceAbiProxy* s, REFGUID g, const IUnknown* d) noexcept { return s->dev0->SetPrivateDataInterface(g, d); }
inline HRESULT STDMETHODCALLTYPE set_name(WsxXboxDeviceAbiProxy* s, LPCWSTR n) noexcept { return s->dev0->SetName(n); }
inline UINT STDMETHODCALLTYPE get_node_count(WsxXboxDeviceAbiProxy* s) noexcept { return s->dev0->GetNodeCount(); }
inline HRESULT STDMETHODCALLTYPE create_cmd_queue(WsxXboxDeviceAbiProxy* s, const D3D12_COMMAND_QUEUE_DESC* d, REFIID r, void** pp) noexcept { return s->dev0->CreateCommandQueue(d, r, pp); }
inline HRESULT STDMETHODCALLTYPE create_cmd_alloc(WsxXboxDeviceAbiProxy* s, D3D12_COMMAND_LIST_TYPE t, REFIID r, void** pp) noexcept { return s->dev0->CreateCommandAllocator(t, r, pp); }
inline HRESULT STDMETHODCALLTYPE create_gfx_pso(WsxXboxDeviceAbiProxy* s, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, REFIID r, void** pp) noexcept { return s->dev0->CreateGraphicsPipelineState(d, r, pp); }
inline HRESULT STDMETHODCALLTYPE create_comp_pso(WsxXboxDeviceAbiProxy* s, const D3D12_COMPUTE_PIPELINE_STATE_DESC* d, REFIID r, void** pp) noexcept { return s->dev0->CreateComputePipelineState(d, r, pp); }
inline HRESULT STDMETHODCALLTYPE create_cmd_list(WsxXboxDeviceAbiProxy* s, UINT nm, D3D12_COMMAND_LIST_TYPE t, ID3D12CommandAllocator* a, ID3D12PipelineState* p, REFIID r, void** pp) noexcept {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    const D3D12_COMMAND_LIST_TYPE remapped_t = remap_xbox_command_list_type(t);
    ComPtr<ID3D12GraphicsCommandList> base_list;
    const HRESULT hr = s->dev0->CreateCommandList(nm, remapped_t, a, p, IID_PPV_ARGS(base_list.GetAddressOf()));
    if (FAILED(hr)) return hr;
    auto* wrapped = WsxXboxCommandListAbiProxy::create(base_list.Get(), reinterpret_cast<IUnknown*>(s));
    if (!wrapped) return E_OUTOFMEMORY;
    const HRESULT qhr = reinterpret_cast<IUnknown*>(wrapped)->QueryInterface(r, pp);
    reinterpret_cast<IUnknown*>(wrapped)->Release();
    return qhr;
}
// Xbox Device slot 73 (0x248): CreateCommandListX(this, pDesc, pAllocator, pInitialState, riid, ppList)
inline HRESULT STDMETHODCALLTYPE create_cmd_list_x_73(
    WsxXboxDeviceAbiProxy* s,
    const D3D12_COMMAND_LIST_TYPE* pTypeDesc,
    ID3D12CommandAllocator* a,
    ID3D12PipelineState* p,
    REFIID r,
    void** pp) noexcept {
    const D3D12_COMMAND_LIST_TYPE t = pTypeDesc ? remap_xbox_command_list_type(*pTypeDesc) : D3D12_COMMAND_LIST_TYPE_DIRECT;
    return create_cmd_list(s, 0u, t, a, p, r, pp);
}
inline HRESULT STDMETHODCALLTYPE check_feature(WsxXboxDeviceAbiProxy* s, D3D12_FEATURE f, void* d, UINT sz) noexcept { return s->dev0->CheckFeatureSupport(f, d, sz); }
inline HRESULT STDMETHODCALLTYPE create_desc_heap(WsxXboxDeviceAbiProxy* s, const D3D12_DESCRIPTOR_HEAP_DESC* d, REFIID r, void** pp) noexcept {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    ComPtr<ID3D12DescriptorHeap> raw_heap;
    const HRESULT hr = s->dev0->CreateDescriptorHeap(d, IID_PPV_ARGS(raw_heap.GetAddressOf()));
    if (FAILED(hr)) return hr;
    auto* wrapped = WsxXboxDescHeapAbiProxy::create(raw_heap.Get(), reinterpret_cast<IUnknown*>(s));
    if (!wrapped) return E_OUTOFMEMORY;
    const HRESULT qhr = reinterpret_cast<IUnknown*>(wrapped)->QueryInterface(r, pp);
    reinterpret_cast<IUnknown*>(wrapped)->Release();
    return qhr;
}
inline UINT STDMETHODCALLTYPE get_desc_inc(WsxXboxDeviceAbiProxy* s, D3D12_DESCRIPTOR_HEAP_TYPE t) noexcept { return s->dev0->GetDescriptorHandleIncrementSize(t); }
inline HRESULT STDMETHODCALLTYPE create_root_sig(WsxXboxDeviceAbiProxy* s, UINT nm, const void* blob, SIZE_T sz, REFIID r, void** pp) noexcept { return s->dev0->CreateRootSignature(nm, blob, sz, r, pp); }
inline void STDMETHODCALLTYPE create_cbv(WsxXboxDeviceAbiProxy* s, const D3D12_CONSTANT_BUFFER_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) noexcept { s->dev0->CreateConstantBufferView(d, h); }
inline void STDMETHODCALLTYPE create_srv(WsxXboxDeviceAbiProxy* s, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) noexcept { s->dev0->CreateShaderResourceView(r, d, h); }
inline void STDMETHODCALLTYPE create_uav(WsxXboxDeviceAbiProxy* s, ID3D12Resource* r, ID3D12Resource* c, const D3D12_UNORDERED_ACCESS_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) noexcept { s->dev0->CreateUnorderedAccessView(r, c, d, h); }
inline void STDMETHODCALLTYPE create_rtv(WsxXboxDeviceAbiProxy* s, ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) noexcept { s->dev0->CreateRenderTargetView(r, d, h); }
inline void STDMETHODCALLTYPE create_dsv(WsxXboxDeviceAbiProxy* s, ID3D12Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) noexcept { s->dev0->CreateDepthStencilView(r, d, h); }
inline void STDMETHODCALLTYPE create_sampler(WsxXboxDeviceAbiProxy* s, const D3D12_SAMPLER_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) noexcept { s->dev0->CreateSampler(d, h); }
inline void STDMETHODCALLTYPE copy_descs(WsxXboxDeviceAbiProxy* s, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* ds, const UINT* dsz, UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* ss, const UINT* ssz, D3D12_DESCRIPTOR_HEAP_TYPE t) noexcept { s->dev0->CopyDescriptors(nd, ds, dsz, ns, ss, ssz, t); }
inline void STDMETHODCALLTYPE copy_descs_simple(WsxXboxDeviceAbiProxy* s, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE d, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE t) noexcept { s->dev0->CopyDescriptorsSimple(n, d, src, t); }
inline D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE get_alloc_info(WsxXboxDeviceAbiProxy* s, D3D12_RESOURCE_ALLOCATION_INFO* out, UINT m, UINT n, const D3D12_RESOURCE_DESC* d) noexcept {
    *out = s->dev0->GetResourceAllocationInfo(m, n, d);
    return out;
}
inline D3D12_HEAP_PROPERTIES* STDMETHODCALLTYPE get_custom_heap(WsxXboxDeviceAbiProxy* s, D3D12_HEAP_PROPERTIES* out, UINT m, D3D12_HEAP_TYPE t) noexcept {
    *out = s->dev0->GetCustomHeapProperties(m, t);
    return out;
}
inline HRESULT STDMETHODCALLTYPE create_committed(WsxXboxDeviceAbiProxy* s, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC* rd, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID r, void** pp) noexcept {
    return s->dev0->CreateCommittedResource(hp, hf, rd, st, cv, r, pp);
}
inline HRESULT STDMETHODCALLTYPE create_committed_xbox_260(WsxXboxDeviceAbiProxy* s, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC* rd, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID r, void** pp) noexcept {
    xbox_abi_stats().committed_resources_via_260.fetch_add(1, std::memory_order_relaxed);
    return s->dev0->CreateCommittedResource(hp, hf, rd, st, cv, r, pp);
}
inline HRESULT STDMETHODCALLTYPE create_heap(WsxXboxDeviceAbiProxy* s, const D3D12_HEAP_DESC* d, REFIID r, void** pp) noexcept { return s->dev0->CreateHeap(d, r, pp); }
inline HRESULT STDMETHODCALLTYPE create_placed(WsxXboxDeviceAbiProxy* s, ID3D12Heap* h, UINT64 off, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID r, void** pp) noexcept { return s->dev0->CreatePlacedResource(h, off, d, st, cv, r, pp); }
inline HRESULT STDMETHODCALLTYPE create_reserved(WsxXboxDeviceAbiProxy* s, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID r, void** pp) noexcept { return s->dev0->CreateReservedResource(d, st, cv, r, pp); }
inline HRESULT STDMETHODCALLTYPE create_shared_handle(WsxXboxDeviceAbiProxy* s, ID3D12DeviceChild* o, const SECURITY_ATTRIBUTES* sa, DWORD a, LPCWSTR n, HANDLE* h) noexcept { return s->dev0->CreateSharedHandle(o, sa, a, n, h); }
inline HRESULT STDMETHODCALLTYPE open_shared_handle(WsxXboxDeviceAbiProxy* s, HANDLE h, REFIID r, void** pp) noexcept { return s->dev0->OpenSharedHandle(h, r, pp); }
inline HRESULT STDMETHODCALLTYPE open_shared_name(WsxXboxDeviceAbiProxy* s, LPCWSTR n, DWORD a, HANDLE* h) noexcept { return s->dev0->OpenSharedHandleByName(n, a, h); }
inline HRESULT STDMETHODCALLTYPE make_resident(WsxXboxDeviceAbiProxy* s, UINT n, ID3D12Pageable* const* o) noexcept { return s->dev0->MakeResident(n, o); }
inline HRESULT STDMETHODCALLTYPE evict(WsxXboxDeviceAbiProxy* s, UINT n, ID3D12Pageable* const* o) noexcept { return s->dev0->Evict(n, o); }
inline HRESULT STDMETHODCALLTYPE create_fence(WsxXboxDeviceAbiProxy* s, UINT64 init, D3D12_FENCE_FLAGS f, REFIID r, void** pp) noexcept { return s->dev0->CreateFence(init, f, r, pp); }
inline HRESULT STDMETHODCALLTYPE get_removed_reason(WsxXboxDeviceAbiProxy* s) noexcept { return s->dev0->GetDeviceRemovedReason(); }
inline void STDMETHODCALLTYPE get_copyable_footprints(WsxXboxDeviceAbiProxy* s, const D3D12_RESOURCE_DESC* d, UINT f, UINT n, UINT64 b, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* l, UINT* nr, UINT64* rs, UINT64* tb) noexcept { s->dev0->GetCopyableFootprints(d, f, n, b, l, nr, rs, tb); }
inline HRESULT STDMETHODCALLTYPE create_query_heap(WsxXboxDeviceAbiProxy* s, const D3D12_QUERY_HEAP_DESC* d, REFIID r, void** pp) noexcept { return s->dev0->CreateQueryHeap(d, r, pp); }
inline HRESULT STDMETHODCALLTYPE set_stable_power(WsxXboxDeviceAbiProxy* s, BOOL e) noexcept { return s->dev0->SetStablePowerState(e); }
inline HRESULT STDMETHODCALLTYPE create_cmd_sig(WsxXboxDeviceAbiProxy* s, const D3D12_COMMAND_SIGNATURE_DESC* d, ID3D12RootSignature* rs, REFIID r, void** pp) noexcept { return s->dev0->CreateCommandSignature(d, rs, r, pp); }
inline void STDMETHODCALLTYPE get_res_tiling(WsxXboxDeviceAbiProxy* s, ID3D12Resource* r, UINT* nt, D3D12_PACKED_MIP_INFO* pm, D3D12_TILE_SHAPE* ts, UINT* ns, UINT fs, D3D12_SUBRESOURCE_TILING* st) noexcept { s->dev0->GetResourceTiling(r, nt, pm, ts, ns, fs, st); }
inline LUID* STDMETHODCALLTYPE get_adapter_luid(WsxXboxDeviceAbiProxy* s, LUID* out) noexcept {
    *out = s->dev0->GetAdapterLuid();
    return out;
}
inline HRESULT STDMETHODCALLTYPE create_pipe_lib(WsxXboxDeviceAbiProxy* s, const void* b, SIZE_T sz, REFIID r, void** pp) noexcept { return s->dev1 ? s->dev1->CreatePipelineLibrary(b, sz, r, pp) : E_NOINTERFACE; }
inline HRESULT STDMETHODCALLTYPE set_evt_multi(WsxXboxDeviceAbiProxy* s, ID3D12Fence* const* f, const UINT64* v, UINT n, D3D12_MULTIPLE_FENCE_WAIT_FLAGS fl, HANDLE h) noexcept { return s->dev1 ? s->dev1->SetEventOnMultipleFenceCompletion(f, v, n, fl, h) : E_NOINTERFACE; }
inline HRESULT STDMETHODCALLTYPE set_res_prio(WsxXboxDeviceAbiProxy* s, UINT n, ID3D12Pageable* const* o, const D3D12_RESIDENCY_PRIORITY* p) noexcept { return s->dev1 ? s->dev1->SetResidencyPriority(n, o, p) : E_NOINTERFACE; }
inline HRESULT STDMETHODCALLTYPE create_pipe_state(WsxXboxDeviceAbiProxy* s, const D3D12_PIPELINE_STATE_STREAM_DESC* d, REFIID r, void** pp) noexcept { return s->dev2 ? s->dev2->CreatePipelineState(d, r, pp) : E_NOINTERFACE; }
inline HRESULT STDMETHODCALLTYPE create_cmd_list1(WsxXboxDeviceAbiProxy* s, UINT nm, D3D12_COMMAND_LIST_TYPE t, D3D12_COMMAND_LIST_FLAGS fl, REFIID r, void** pp) noexcept {
    if (!s->dev4) return E_NOINTERFACE;
    if (!pp) return E_POINTER;
    *pp = nullptr;
    ComPtr<ID3D12GraphicsCommandList> base_list;
    const HRESULT hr = s->dev4->CreateCommandList1(nm, t, fl, IID_PPV_ARGS(base_list.GetAddressOf()));
    if (FAILED(hr)) return hr;
    auto* wrapped = WsxXboxCommandListAbiProxy::create(base_list.Get(), reinterpret_cast<IUnknown*>(s));
    if (!wrapped) return E_OUTOFMEMORY;
    const HRESULT qhr = reinterpret_cast<IUnknown*>(wrapped)->QueryInterface(r, pp);
    reinterpret_cast<IUnknown*>(wrapped)->Release();
    return qhr;
}

// Xbox Series X|S D3D12.X slot 61 (offset 0x1E8): 6-arg CreateGraphicsPipelineStateX
inline HRESULT STDMETHODCALLTYPE create_graphics_pso_x_1e8(
    WsxXboxDeviceAbiProxy* self,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
    const void* /*pExt1*/,
    const void* /*pExt2*/,
    REFIID riid,
    void** ppPipelineState) noexcept {
    const HRESULT hr = self->dev0->CreateGraphicsPipelineState(pDesc, riid, ppPipelineState);
    if (SUCCEEDED(hr)) {
        xbox_abi_stats().graphics_pso_x_ok.fetch_add(1, std::memory_order_relaxed);
    }
    return hr;
}

inline HRESULT STDMETHODCALLTYPE create_placed_xbox_268(
    WsxXboxDeviceAbiProxy* s,
    ID3D12Heap* h,
    UINT64 off,
    const D3D12_RESOURCE_DESC* rd,
    D3D12_RESOURCE_STATES st,
    const D3D12_CLEAR_VALUE* cv,
    REFIID r,
    void** pp) noexcept {
    const HRESULT hr = s->dev0->CreatePlacedResource(h, off, rd, st, cv, r, pp);
    if (SUCCEEDED(hr)) {
        xbox_abi_stats().placed_resources_x_ok.fetch_add(1, std::memory_order_relaxed);
    }
    return hr;
}

inline HRESULT STDMETHODCALLTYPE create_reserved_xbox_270(
    WsxXboxDeviceAbiProxy* s,
    const D3D12_RESOURCE_DESC* rd,
    D3D12_RESOURCE_STATES st,
    const D3D12_CLEAR_VALUE* cv,
    REFIID r,
    void** pp) noexcept {
    return s->dev0->CreateReservedResource(rd, st, cv, r, pp);
}

// Xbox Series X|S D3D12.X slot 62 (offset 0x1F0): 6-arg CreateComputePipelineStateX
// Observed in XboxBvhBuilder.dll (0x18000C4BE) and amd_fidelityfx_upscaler_xbox.dll (0x180005825):
//   HRESULT CreateComputePipelineStateX(const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
//                                       const void* pExt1, const void* pExt2,
//                                       REFIID riid, void** ppPipelineState)
inline HRESULT STDMETHODCALLTYPE create_compute_pso_x_1f0(
    WsxXboxDeviceAbiProxy* self,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
    const void* /*pExt1*/,
    const void* /*pExt2*/,
    REFIID riid,
    void** ppPipelineState) noexcept {
    const HRESULT hr = self->dev0->CreateComputePipelineState(pDesc, riid, ppPipelineState);
    if (SUCCEEDED(hr)) {
        xbox_abi_stats().compute_pso_x_ok.fetch_add(1, std::memory_order_relaxed);
        if (ppPipelineState && *ppPipelineState && xbox_abi_env_flag(L"WSX_XBOX_PSO_FALLBACK", false)) {
            std::lock_guard<std::mutex> lock(self->fallback_mtx);
            if (!self->fallback_compute_pso) {
                self->fallback_compute_pso = static_cast<ID3D12PipelineState*>(*ppPipelineState);
            }
        }
        return hr;
    }
    xbox_abi_stats().compute_pso_x_fail.fetch_add(1, std::memory_order_relaxed);
    wsx::trace_hr("d3d12_xs", "WsxXboxDeviceAbiProxy::CreateComputePipelineStateX(0x1F0)", hr,
                  "bytecodeBytes=%zu", pDesc ? pDesc->CS.BytecodeLength : 0u);
    if (ppPipelineState && xbox_abi_env_flag(L"WSX_XBOX_PSO_FALLBACK", false)) {
        std::lock_guard<std::mutex> lock(self->fallback_mtx);
        if (self->fallback_compute_pso) {
            self->fallback_compute_pso->AddRef();
            *ppPipelineState = self->fallback_compute_pso.Get();
            xbox_abi_stats().compute_pso_x_fallback.fetch_add(1, std::memory_order_relaxed);
            return S_OK;
        }
    }
    return hr;
}

template <size_t... Is>
inline std::array<void*, 160> make_vtable_impl(std::index_sequence<Is...>) noexcept {
    std::array<void*, 160> vt{reinterpret_cast<void*>(&unhandled<static_cast<uint32_t>(Is)>)...};
    vt[0]  = reinterpret_cast<void*>(&qi);
    vt[1]  = reinterpret_cast<void*>(&addref);
    vt[2]  = reinterpret_cast<void*>(&release);
    vt[3]  = reinterpret_cast<void*>(&get_priv);
    vt[4]  = reinterpret_cast<void*>(&set_priv);
    vt[5]  = reinterpret_cast<void*>(&set_priv_if);
    vt[6]  = reinterpret_cast<void*>(&set_name);
    vt[7]  = reinterpret_cast<void*>(&get_node_count);
    vt[8]  = reinterpret_cast<void*>(&create_cmd_queue);
    vt[9]  = reinterpret_cast<void*>(&create_cmd_alloc);
    vt[10] = reinterpret_cast<void*>(&create_gfx_pso);
    vt[11] = reinterpret_cast<void*>(&create_comp_pso);
    vt[12] = reinterpret_cast<void*>(&create_cmd_list);          // 0x060 (returns WsxXboxCommandListAbiProxy)
    vt[13] = reinterpret_cast<void*>(&check_feature);
    vt[14] = reinterpret_cast<void*>(&create_desc_heap);         // 0x070 (returns WsxXboxDescHeapAbiProxy)
    vt[15] = reinterpret_cast<void*>(&get_desc_inc);
    vt[16] = reinterpret_cast<void*>(&create_root_sig);          // 0x080
    vt[17] = reinterpret_cast<void*>(&create_cbv);
    vt[18] = reinterpret_cast<void*>(&create_srv);
    vt[19] = reinterpret_cast<void*>(&create_uav);
    vt[20] = reinterpret_cast<void*>(&create_rtv);
    vt[21] = reinterpret_cast<void*>(&create_dsv);
    vt[22] = reinterpret_cast<void*>(&create_sampler);
    vt[23] = reinterpret_cast<void*>(&copy_descs);
    vt[24] = reinterpret_cast<void*>(&copy_descs_simple);
    vt[25] = reinterpret_cast<void*>(&get_alloc_info);
    vt[26] = reinterpret_cast<void*>(&get_custom_heap);
    vt[27] = reinterpret_cast<void*>(&create_committed);         // 0x0D8
    vt[28] = reinterpret_cast<void*>(&create_heap);
    vt[29] = reinterpret_cast<void*>(&create_placed);
    vt[30] = reinterpret_cast<void*>(&create_reserved);
    vt[31] = reinterpret_cast<void*>(&create_shared_handle);
    vt[32] = reinterpret_cast<void*>(&open_shared_handle);
    vt[33] = reinterpret_cast<void*>(&open_shared_name);
    vt[34] = reinterpret_cast<void*>(&make_resident);
    vt[35] = reinterpret_cast<void*>(&evict);
    vt[36] = reinterpret_cast<void*>(&create_fence);
    vt[37] = reinterpret_cast<void*>(&get_removed_reason);
    vt[38] = reinterpret_cast<void*>(&get_copyable_footprints);
    vt[39] = reinterpret_cast<void*>(&create_query_heap);
    vt[40] = reinterpret_cast<void*>(&set_stable_power);
    vt[41] = reinterpret_cast<void*>(&create_cmd_sig);
    vt[42] = reinterpret_cast<void*>(&get_res_tiling);
    vt[43] = reinterpret_cast<void*>(&get_adapter_luid);
    vt[44] = reinterpret_cast<void*>(&create_pipe_lib);
    vt[45] = reinterpret_cast<void*>(&set_evt_multi);
    vt[46] = reinterpret_cast<void*>(&set_res_prio);
    vt[47] = reinterpret_cast<void*>(&create_pipe_state);
    vt[48] = reinterpret_cast<void*>(&create_cmd_queue);          // 0x180 (CreateCommandQueueX)
    vt[51] = reinterpret_cast<void*>(&create_cmd_list1);
    vt[61] = reinterpret_cast<void*>(&create_graphics_pso_x_1e8); // 0x1E8 (CreateGraphicsPipelineStateX)
    vt[62] = reinterpret_cast<void*>(&create_compute_pso_x_1f0);  // 0x1F0 (XboxBvhBuilder + FSR4)
    vt[68] = reinterpret_cast<void*>(&xbox_dev_info_68);          // 0x220 (Xbox device info query)
    vt[69] = reinterpret_cast<void*>(&xbox_dev_setting_69);       // 0x228 (Xbox device setting)
    vt[73] = reinterpret_cast<void*>(&create_cmd_list_x_73);      // 0x248 (CreateCommandListX)
    vt[76] = reinterpret_cast<void*>(&create_committed_xbox_260); // 0x260 (FSR4 CreateCommittedResource)
    vt[77] = reinterpret_cast<void*>(&create_placed_xbox_268);    // 0x268 (CreatePlacedResourceX)
    vt[78] = reinterpret_cast<void*>(&create_reserved_xbox_270);  // 0x270 (CreateReservedResourceX)
    return vt;
}

inline void** get_vtable() noexcept {
    static auto vt = make_vtable_impl(std::make_index_sequence<160>{});
    return vt.data();
}

} // namespace xbox_dev_detail

inline WsxXboxDeviceAbiProxy* WsxXboxDeviceAbiProxy::create(ID3D12Device* base_device) noexcept {
    if (!base_device) return nullptr;
    // Avoid double-wrapping if caller already passed an Xbox-ABI device
    ID3D12Device* unwrapped = nullptr;
    if (SUCCEEDED(base_device->QueryInterface(__uuidof(IWsxXboxDeviceAbiMarker),
                                              reinterpret_cast<void**>(&unwrapped))) && unwrapped) {
        base_device = unwrapped;
    }
    auto* p = new (std::nothrow) WsxXboxDeviceAbiProxy();
    if (!p) return nullptr;
    p->vtable = xbox_dev_detail::get_vtable();
    p->dev0 = base_device;
    p->dev0.As(&p->dev1);
    p->dev0.As(&p->dev2);
    p->dev0.As(&p->dev4);
    p->dev0.As(&p->dev8);
    return p;
}

} // namespace wsx_d3d12
