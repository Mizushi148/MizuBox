// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Wave 10.2: Automatic SM 6.6 RDNA2 Bindless Slab & T#/S# Descriptor Hash-Table Runtime.
// Manages the shader-visible CBV_SRV_UAV + SAMPLER descriptor heaps, SM 6.6 root signature,
// 64-bit Xbox GPU VA -> RWByteAddressBuffer slab table, and GPU-resident FNV-1a open-addressing
// T#/S# descriptor hash table so translated RDNA2 shaders automatically resolve real GPU buffers,
// textures, and samplers on Draw*/Dispatch*/DispatchMesh with a single bind_*() call.

#pragma once

#include "d3d12_xs_sm66_runtime.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wsx_sm66 {

using Microsoft::WRL::ComPtr;

class AutoBindContext {
    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12RootSignature> root_sig_;
    ComPtr<ID3D12DescriptorHeap> srv_uav_heap_;
    ComPtr<ID3D12DescriptorHeap> sampler_heap_;
    UINT srv_inc_ = 0;
    UINT smp_inc_ = 0;

    // Slab layout: descriptors [slab_base_idx_ .. slab_base_idx_ + max_slabs_ - 1]
    uint64_t heap_base_va_ = 0x10000000ull;
    uint32_t slab_shift_ = 20u;     // Default 1 MB per slab
    uint32_t slab_base_idx_ = 0u;
    uint32_t max_slabs_ = 16u;

    // FNV-1a T#/S# descriptor hash table StructuredBuffer at desc_table_srv_idx_
    uint32_t desc_table_srv_idx_ = 16u;
    uint32_t desc_table_cap_ = 256u; // Power of 2
    uint32_t next_res_idx_ = 17u;
    uint32_t next_smp_idx_ = 0u;
    ComPtr<ID3D12Resource> desc_table_buf_;
    Sm66DescHashEntry* desc_table_cpu_ = nullptr;
    uint64_t code_base_va_ = 0ull;
    uint32_t registered_slabs_ = 0u;
    uint32_t registered_descs_ = 0u;

public:
    bool initialize(
        ID3D12Device* dev,
        decltype(&D3D12SerializeVersionedRootSignature) fn_serialize = &D3D12SerializeVersionedRootSignature,
        uint64_t heap_base_va = 0x10000000ull,
        uint32_t slab_shift = 20u,
        uint32_t max_slabs = 16u,
        uint32_t desc_table_cap = 256u) noexcept {
        if (!dev || !fn_serialize || max_slabs == 0u || desc_table_cap < 16u) return false;
        dev_ = dev;
        heap_base_va_ = heap_base_va;
        slab_shift_ = std::clamp<uint32_t>(slab_shift, 12u, 28u);
        max_slabs_ = max_slabs;
        // Round desc_table_cap up to power of 2
        uint32_t cap = 16u;
        while (cap < desc_table_cap && cap < 16384u) cap <<= 1u;
        desc_table_cap_ = cap;
        slab_base_idx_ = 0u;
        desc_table_srv_idx_ = max_slabs_;
        next_res_idx_ = max_slabs_ + 1u;
        next_smp_idx_ = 0u;

        if (FAILED(create_sm66_root_signature(dev, fn_serialize, IID_PPV_ARGS(&root_sig_)))) {
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = max_slabs_ + 1u + 256u;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_uav_heap_)))) return false;

        D3D12_DESCRIPTOR_HEAP_DESC shd{};
        shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        shd.NumDescriptors = 64u;
        shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&sampler_heap_)))) return false;

        srv_inc_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        smp_inc_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

        // Allocate persistently mapped UPLOAD buffer for FNV-1a T#/S# hash table
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = static_cast<UINT64>(desc_table_cap_) * sizeof(Sm66DescHashEntry);
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&desc_table_buf_)))) {
            return false;
        }
        void* mapped = nullptr;
        if (FAILED(desc_table_buf_->Map(0, nullptr, &mapped)) || !mapped) return false;
        desc_table_cpu_ = static_cast<Sm66DescHashEntry*>(mapped);
        std::memset(desc_table_cpu_, 0, static_cast<size_t>(bd.Width));

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.Buffer.NumElements = desc_table_cap_;
        srv.Buffer.StructureByteStride = sizeof(Sm66DescHashEntry);
        dev->CreateShaderResourceView(desc_table_buf_.Get(), &srv, cpu_res_handle(desc_table_srv_idx_));
        return true;
    }

    ~AutoBindContext() {
        if (desc_table_buf_ && desc_table_cpu_) {
            desc_table_buf_->Unmap(0, nullptr);
            desc_table_cpu_ = nullptr;
        }
    }

    ID3D12RootSignature* root_signature() const noexcept { return root_sig_.Get(); }
    ID3D12DescriptorHeap* resource_heap() const noexcept { return srv_uav_heap_.Get(); }
    ID3D12DescriptorHeap* sampler_heap() const noexcept { return sampler_heap_.Get(); }
    uint32_t registered_slabs() const noexcept { return registered_slabs_; }
    uint32_t registered_descriptors() const noexcept { return registered_descs_; }
    uint64_t heap_base_va() const noexcept { return heap_base_va_; }
    uint32_t slab_shift() const noexcept { return slab_shift_; }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_res_handle(uint32_t idx) const noexcept {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srv_uav_heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(idx) * srv_inc_;
        return h;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_smp_handle(uint32_t idx) const noexcept {
        D3D12_CPU_DESCRIPTOR_HANDLE h = sampler_heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(idx) * smp_inc_;
        return h;
    }

    bool register_slab(uint32_t slab_index, ID3D12Resource* buf, uint32_t byte_size) noexcept {
        if (!dev_ || !buf || slab_index >= max_slabs_ || byte_size < 4u) return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = byte_size / 4u;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        dev_->CreateUnorderedAccessView(buf, nullptr, &uav, cpu_res_handle(slab_base_idx_ + slab_index));
        ++registered_slabs_;
        return true;
    }

    bool register_texture_srv(const uint32_t t_key[8], ID3D12Resource* tex,
                              const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, uint32_t* out_heap_idx = nullptr) noexcept {
        if (!dev_ || !t_key || !tex || !desc_table_cpu_) return false;
        const uint32_t idx = next_res_idx_++;
        dev_->CreateShaderResourceView(tex, desc, cpu_res_handle(idx));
        if (out_heap_idx) *out_heap_idx = idx;
        return insert_hash_entry(t_key, idx, 1u);
    }

    bool register_sampler(const uint32_t s_key[4], const D3D12_SAMPLER_DESC* desc, uint32_t* out_heap_idx = nullptr) noexcept {
        if (!dev_ || !s_key || !desc || !desc_table_cpu_ || next_smp_idx_ >= 64u) return false;
        const uint32_t idx = next_smp_idx_++;
        dev_->CreateSampler(desc, cpu_smp_handle(idx));
        uint32_t full_key[8] = {s_key[0], s_key[1], s_key[2], s_key[3],
                                0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
        if (out_heap_idx) *out_heap_idx = idx;
        return insert_hash_entry(full_key, idx, 2u);
    }

    void set_code_base_va(uint64_t va) noexcept { code_base_va_ = va; }

    Sm66UserData build_user_data(const uint32_t* user_sgprs = nullptr, uint32_t sgpr_count = 0u) const noexcept {
        Sm66UserData ud{};
        const uint32_t copy_n = std::min<uint32_t>(sgpr_count, 32u);
        if (user_sgprs && copy_n > 0u) {
            std::memcpy(ud.s, user_sgprs, copy_n * sizeof(uint32_t));
        }
        ud.heap_base_lo = static_cast<uint32_t>(heap_base_va_ & 0xFFFFFFFFull);
        ud.heap_base_hi = static_cast<uint32_t>(heap_base_va_ >> 32);
        ud.code_base_lo = static_cast<uint32_t>(code_base_va_ & 0xFFFFFFFFull);
        ud.code_base_hi = static_cast<uint32_t>(code_base_va_ >> 32);
        ud.slab_base_idx = slab_base_idx_;
        ud.slab_shift = slab_shift_;
        ud.desc_table_srv_idx = desc_table_srv_idx_;
        ud.desc_table_mask = desc_table_cap_ - 1u;
        return ud;
    }

    void bind_compute(ID3D12GraphicsCommandList* cl, const uint32_t* user_sgprs = nullptr, uint32_t sgpr_count = 0u) const noexcept {
        if (!cl || !root_sig_) return;
        ID3D12DescriptorHeap* heaps[2] = {srv_uav_heap_.Get(), sampler_heap_.Get()};
        cl->SetDescriptorHeaps(2, heaps);
        cl->SetComputeRootSignature(root_sig_.Get());
        const Sm66UserData ud = build_user_data(user_sgprs, sgpr_count);
        cl->SetComputeRoot32BitConstants(0, 40, &ud, 0);
    }

    void bind_graphics(ID3D12GraphicsCommandList* cl, const uint32_t* user_sgprs = nullptr, uint32_t sgpr_count = 0u) const noexcept {
        if (!cl || !root_sig_) return;
        ID3D12DescriptorHeap* heaps[2] = {srv_uav_heap_.Get(), sampler_heap_.Get()};
        cl->SetDescriptorHeaps(2, heaps);
        cl->SetGraphicsRootSignature(root_sig_.Get());
        const Sm66UserData ud = build_user_data(user_sgprs, sgpr_count);
        cl->SetGraphicsRoot32BitConstants(0, 40, &ud, 0);
    }

private:
    bool insert_hash_entry(const uint32_t key[8], uint32_t heap_idx, uint32_t flags) noexcept {
        const uint32_t mask = desc_table_cap_ - 1u;
        uint32_t slot = fnv1a_8dw(key) & mask;
        for (uint32_t probe = 0; probe < desc_table_cap_; ++probe) {
            Sm66DescHashEntry& ent = desc_table_cpu_[slot];
            if (ent.flags == 0u ||
                (ent.flags == flags && std::memcmp(ent.key, key, 8u * sizeof(uint32_t)) == 0)) {
                const bool was_empty = (ent.flags == 0u);
                std::memcpy(ent.key, key, 8u * sizeof(uint32_t));
                ent.heap_index = heap_idx;
                ent.flags = flags;
                if (was_empty) ++registered_descs_;
                return true;
            }
            slot = (slot + 1u) & mask;
        }
        return false;
    }
};

} // namespace wsx_sm66
