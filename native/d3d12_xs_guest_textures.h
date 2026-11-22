// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox Series X|S UMA Guest-Texture Registry & Pre-Submit Synchronization:
// - Wraps TEXTURE2D ID3D12Resource instances (both CreateCommittedResource & CreatePlacedResource)
//   so GetGPUVirtualAddress() lazily allocates & returns a CPU-writable UMA guest backing buffer.
// - Supports LINEAR, 4KB_S/4KB_D, and 64KB_S/64KB_D console tiled layouts via wsx_xg_layout.h
//   (identical byte layout to xg_xs.dll IXGTextureAddressComputer::CopyIntoSubresource).
// - Flushes dirty CPU-written guest textures via asynchronous upload staging slots IMMEDIATELY
//   before every ID3D12CommandQueue::ExecuteCommandLists (zero 1-frame lag, zero stalls,
//   exact submit-time D3D12 resource state barrier tracking, 0 D3D12 Debug Layer errors).

#pragma once

#include "d3d12_xs_gpu_deswizzle.h"
#include "wsx_config.h"
#include "wsx_xg_layout.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace wsx_d3d12 {

using Microsoft::WRL::ComPtr;

struct GuestTextureStats {
    uint64_t tracked_textures = 0;
    uint64_t backing_allocations = 0;
    uint64_t sync_flushes = 0;
    uint64_t textures_uploaded = 0;
    uint64_t deswizzled_uploads = 0;
    uint64_t bytes_uploaded = 0;
    uint64_t write_watch_skips = 0;
    uint64_t gpu_deswizzled_uploads = 0;
    uint64_t staging_trims = 0;
};

struct GuestTextureRecord;

struct RecordedStateTransition {
    std::weak_ptr<GuestTextureRecord> record;
    UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    D3D12_RESOURCE_STATES state_after = D3D12_RESOURCE_STATE_COMMON;
};

inline uint32_t infer_swizzle_mode_from_xbox_layout(D3D12_TEXTURE_LAYOUT layout) noexcept {
    const uint32_t raw = static_cast<uint32_t>(layout);
    switch (raw) {
    case 4u:
    case 0x104u:
        return wsx_xg::SWIZZLE_4KB_Z;
    case 5u:
    case 0x105u:
        return wsx_xg::SWIZZLE_4KB_S;
    case 6u:
    case 0x106u:
        return wsx_xg::SWIZZLE_4KB_D;
    case 8u:
    case 24u:
    case 0x108u:
        return wsx_xg::SWIZZLE_64KB_Z;
    case 9u:
    case 25u:
    case 0x109u:
        return wsx_xg::SWIZZLE_64KB_S;
    case 10u:
    case 26u:
    case 0x10Au:
        return wsx_xg::SWIZZLE_64KB_D;
    default:
        return wsx_xg::SWIZZLE_LINEAR;
    }
}

inline uint64_t fast_hash_bytes(const void* data, size_t size) noexcept {
    if (!data || size == 0u) return 0xcbf29ce484222325ull;
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xcbf29ce484222325ull ^ static_cast<uint64_t>(size);
    size_t i = 0;
    while (i + 8u <= size) {
        uint64_t w = 0;
        std::memcpy(&w, p + i, 8u);
        h ^= w;
        h *= 0x100000001b3ull;
        h ^= (h >> 33);
        i += 8u;
    }
    while (i < size) {
        h ^= p[i++];
        h *= 0x100000001b3ull;
    }
    h ^= (h >> 33);
    h *= 0xff51afd7ed558ccdull;
    h ^= (h >> 33);
    return h;
}

struct GuestTextureRecord {
    std::mutex record_mutex;
    ID3D12Resource* raw_resource = nullptr;
    D3D12_RESOURCE_DESC desc{};
    uint32_t swizzle_mode = wsx_xg::SWIZZLE_LINEAR;
    std::vector<wsx_xg::SubresourceLayout> mips;
    std::vector<wsx_xg::SubresourceLayout3D> mips3d;
    uint64_t slice_stride = 0;
    uint64_t total_bytes = 0;
    void* backing_mem = nullptr;
    bool has_write_watch = false;
    uint64_t last_uploaded_hash = 0;
    bool force_upload = false;
    D3D12_RESOURCE_STATES gpu_state = D3D12_RESOURCE_STATE_COMMON;
    std::atomic<bool> alive{true};

    ~GuestTextureRecord() {
        if (backing_mem) {
            VirtualFree(backing_mem, 0, MEM_RELEASE);
            backing_mem = nullptr;
        }
    }

    static void* alloc_guest_backing(SIZE_T bytes, bool& out_write_watch) noexcept {
        out_write_watch = false;
        void* p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
        if (p) {
            out_write_watch = true;
            ResetWriteWatch(p, bytes);
            return p;
        }
        return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }

    void rebuild_layout_locked() {
        const uint32_t w = std::max(1u, static_cast<uint32_t>(desc.Width));
        const uint32_t h = std::max(1u, desc.Height);
        const uint32_t mip_count = std::clamp(static_cast<uint32_t>(desc.MipLevels ? desc.MipLevels : 1u), 1u, 15u);
        const uint32_t depth_or_slices = std::max(1u, static_cast<uint32_t>(desc.DepthOrArraySize));
        const uint64_t base_align = wsx_xg::is_64kb_mode(swizzle_mode) ? 65536u
                                  : (wsx_xg::is_4kb_mode(swizzle_mode) ? 4096u : 256u);

        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
            mips.clear();
            mips3d.resize(mip_count);
            uint64_t cur = 0u;
            for (uint32_t m = 0; m < mip_count; ++m) {
                mips3d[m] = wsx_xg::compute_subresource_3d(w, h, depth_or_slices, m, desc.Format, swizzle_mode, cur);
                cur = mips3d[m].offset_bytes + mips3d[m].size_bytes;
            }
            slice_stride = wsx_xg::align_up(cur, base_align);
            total_bytes = slice_stride;
            return;
        }

        mips3d.clear();
        mips.resize(mip_count);
        uint64_t cur = 0u;
        for (uint32_t m = 0; m < mip_count; ++m) {
            mips[m] = wsx_xg::compute_subresource_2d(w, h, m, desc.Format, swizzle_mode, cur);
            cur = mips[m].offset_bytes + mips[m].size_bytes;
        }
        slice_stride = wsx_xg::align_up(cur, base_align);
        total_bytes = slice_stride * depth_or_slices;
    }

    void* ensure_backing_locked(bool& newly_allocated) {
        newly_allocated = false;
        if (backing_mem) return backing_mem;
        rebuild_layout_locked();
        if (total_bytes == 0u) return nullptr;
        backing_mem = alloc_guest_backing(static_cast<SIZE_T>(total_bytes), has_write_watch);
        if (!backing_mem) return nullptr;
        last_uploaded_hash = fast_hash_bytes(backing_mem, static_cast<size_t>(total_bytes));
        force_upload = false;
        newly_allocated = true;
        return backing_mem;
    }
};

class GuestTextureManager {
    std::mutex mutex_;
    std::unordered_map<ID3D12Resource*, std::shared_ptr<GuestTextureRecord>> active_;
    std::atomic<uint32_t> active_count_{0};
    std::atomic<uint64_t> backing_allocations_{0};
    std::atomic<uint64_t> sync_flushes_{0};
    std::atomic<uint64_t> textures_uploaded_{0};
    std::atomic<uint64_t> deswizzled_uploads_{0};
    std::atomic<uint64_t> bytes_uploaded_{0};
    std::atomic<uint64_t> write_watch_skips_{0};
    std::atomic<uint64_t> gpu_deswizzled_uploads_{0};
    std::atomic<uint64_t> staging_trims_{0};

public:
    bool has_active_textures() const noexcept {
        return active_count_.load(std::memory_order_acquire) != 0u;
    }

    void register_active(const std::shared_ptr<GuestTextureRecord>& rec, bool newly_allocated) {
        if (!rec || !rec->raw_resource) return;
        std::lock_guard<std::mutex> lk(mutex_);
        auto [it, inserted] = active_.emplace(rec->raw_resource, rec);
        if (inserted) {
            active_count_.store(static_cast<uint32_t>(active_.size()), std::memory_order_release);
        }
        if (newly_allocated) {
            backing_allocations_.fetch_add(1u, std::memory_order_relaxed);
        }
    }

    void unregister_resource(ID3D12Resource* raw_res) {
        if (!raw_res) return;
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = active_.find(raw_res);
        if (it != active_.end()) {
            it->second->alive.store(false, std::memory_order_release);
            active_.erase(it);
            active_count_.store(static_cast<uint32_t>(active_.size()), std::memory_order_release);
        }
    }

    static void commit_state_transitions(const std::vector<RecordedStateTransition>& transitions) {
        for (const auto& t : transitions) {
            if (auto rec = t.record.lock()) {
                std::lock_guard<std::mutex> rlk(rec->record_mutex);
                rec->gpu_state = t.state_after;
            }
        }
    }

    std::vector<std::shared_ptr<GuestTextureRecord>> snapshot_active() {
        std::vector<std::shared_ptr<GuestTextureRecord>> out;
        if (!has_active_textures()) return out;
        std::lock_guard<std::mutex> lk(mutex_);
        out.reserve(active_.size());
        for (const auto& kv : active_) {
            if (kv.second && kv.second->alive.load(std::memory_order_acquire)) {
                out.push_back(kv.second);
            }
        }
        return out;
    }

    void record_write_watch_skips(uint64_t skips) noexcept {
        if (skips > 0u) write_watch_skips_.fetch_add(skips, std::memory_order_relaxed);
    }

    void record_staging_trim(uint64_t freed_slots) noexcept {
        if (freed_slots > 0u) staging_trims_.fetch_add(freed_slots, std::memory_order_relaxed);
    }

    void record_flush_stats(uint64_t uploaded, uint64_t deswizzled, uint64_t gpu_deswizzled, uint64_t bytes) noexcept {
        sync_flushes_.fetch_add(1u, std::memory_order_relaxed);
        textures_uploaded_.fetch_add(uploaded, std::memory_order_relaxed);
        deswizzled_uploads_.fetch_add(deswizzled, std::memory_order_relaxed);
        gpu_deswizzled_uploads_.fetch_add(gpu_deswizzled, std::memory_order_relaxed);
        bytes_uploaded_.fetch_add(bytes, std::memory_order_relaxed);
    }

    GuestTextureStats get_stats() const noexcept {
        GuestTextureStats s{};
        s.tracked_textures = active_count_.load(std::memory_order_acquire);
        s.backing_allocations = backing_allocations_.load(std::memory_order_relaxed);
        s.sync_flushes = sync_flushes_.load(std::memory_order_relaxed);
        s.textures_uploaded = textures_uploaded_.load(std::memory_order_relaxed);
        s.deswizzled_uploads = deswizzled_uploads_.load(std::memory_order_relaxed);
        s.bytes_uploaded = bytes_uploaded_.load(std::memory_order_relaxed);
        s.write_watch_skips = write_watch_skips_.load(std::memory_order_relaxed);
        s.gpu_deswizzled_uploads = gpu_deswizzled_uploads_.load(std::memory_order_relaxed);
        s.staging_trims = staging_trims_.load(std::memory_order_relaxed);
        return s;
    }
};

struct GuestUploadContext {
    struct UploadSlot {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> cmd_list;
        ComPtr<ID3D12Resource> staging_buf;
        UINT64 staging_capacity = 0;
        ComPtr<ID3D12Resource> gpu_linear_buf;
        UINT64 gpu_linear_capacity = 0;
        D3D12_RESOURCE_STATES gpu_linear_state = D3D12_RESOURCE_STATE_COMMON;
        UINT64 fence_val = 0;
    };

    std::mutex mutex;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    UINT64 next_fence_val = 1;
    std::vector<UploadSlot> slots;
    GpuTextureDeswizzler gpu_deswizzler;

    ~GuestUploadContext() {
        drain();
        gpu_deswizzler.reset();
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
    }

    void drain() noexcept {
        std::lock_guard<std::mutex> lk(mutex);
        if (!fence || next_fence_val <= 1u) return;
        const UINT64 target = next_fence_val - 1u;
        if (fence->GetCompletedValue() < target && fence_event) {
            if (SUCCEEDED(fence->SetEventOnCompletion(target, fence_event))) {
                WaitForSingleObject(fence_event, 5000);
            }
        }
        slots.clear();
    }

    uint64_t trim_idle_slots() noexcept {
        std::lock_guard<std::mutex> lk(mutex);
        if (!fence || slots.empty()) return 0u;
        const UINT64 completed = fence->GetCompletedValue();
        uint64_t freed = 0u;
        for (auto& s : slots) {
            if (s.fence_val <= completed) {
                bool had = false;
                if (s.staging_buf) {
                    s.staging_buf.Reset();
                    s.staging_capacity = 0u;
                    had = true;
                }
                if (s.gpu_linear_buf) {
                    s.gpu_linear_buf.Reset();
                    s.gpu_linear_capacity = 0u;
                    s.gpu_linear_state = D3D12_RESOURCE_STATE_COMMON;
                    had = true;
                }
                if (had) ++freed;
            }
        }
        return freed;
    }

    bool flush_dirty_textures(
        ID3D12Device* raw_device,
        ID3D12CommandQueue* raw_queue,
        D3D12_COMMAND_LIST_TYPE queue_type,
        GuestTextureManager& mgr) {
        if (!raw_device || !raw_queue || !mgr.has_active_textures()) return false;
        if (queue_type != D3D12_COMMAND_LIST_TYPE_DIRECT &&
            queue_type != D3D12_COMMAND_LIST_TYPE_COPY) {
            return false;
        }

        const auto candidates = mgr.snapshot_active();
        if (candidates.empty()) return false;

        const bool is_direct = (queue_type == D3D12_COMMAND_LIST_TYPE_DIRECT);

        struct DirtyItem {
            std::shared_ptr<GuestTextureRecord> rec;
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
            UINT subresource_count = 0;
            bool use_gpu_deswizzle = false;
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
            std::vector<UINT> num_rows;
            std::vector<UINT64> row_sizes;
            std::vector<UINT64> raw_src_offsets;
            UINT64 upload_base_offset = 0;
        };

        std::vector<DirtyItem> dirty;
        UINT64 total_staging_bytes = 0u;
        UINT64 total_gpu_linear_bytes = 0u;
        uint64_t ww_skipped = 0u;

        for (const auto& rec : candidates) {
            std::lock_guard<std::mutex> rlk(rec->record_mutex);
            if (!rec->alive.load(std::memory_order_acquire) || !rec->backing_mem || rec->total_bytes == 0u) {
                continue;
            }
            if (rec->has_write_watch && !rec->force_upload) {
                void* dirty_page[1]{};
                ULONG_PTR page_count = 1u;
                DWORD granularity = 0u;
                if (GetWriteWatch(0u, rec->backing_mem, static_cast<SIZE_T>(rec->total_bytes),
                                  dirty_page, &page_count, &granularity) == 0 && page_count == 0u) {
                    ++ww_skipped;
                    continue;
                }
            }
            const uint64_t cur_hash = fast_hash_bytes(rec->backing_mem, static_cast<size_t>(rec->total_bytes));
            if (cur_hash == rec->last_uploaded_hash && !rec->force_upload) {
                if (rec->has_write_watch) {
                    ResetWriteWatch(rec->backing_mem, static_cast<SIZE_T>(rec->total_bytes));
                }
                continue;
            }

            const bool is_3d = (rec->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D);
            const UINT mips = std::max(1u, static_cast<UINT>(is_3d ? rec->mips3d.size() : rec->mips.size()));
            const UINT slices = is_3d ? 1u : std::max(1u, static_cast<UINT>(rec->desc.DepthOrArraySize));
            const UINT sub_count = mips * slices;

            bool can_gpu_deswizzle = false;
            if (is_direct && !is_3d && !wsx_xg::is_linear_mode(rec->swizzle_mode) &&
                GpuTextureDeswizzler::mode_enabled_for_size(rec->total_bytes)) {
                can_gpu_deswizzle = true;
                for (UINT m = 0; m < mips; ++m) {
                    if (!GpuTextureDeswizzler::supports_layout(rec->mips[m])) {
                        can_gpu_deswizzle = false;
                        break;
                    }
                }
                if (can_gpu_deswizzle && !gpu_deswizzler.ready(raw_device)) {
                    can_gpu_deswizzle = false;
                }
            }

            DirtyItem item{};
            item.rec = rec;
            item.state = rec->gpu_state;
            item.subresource_count = sub_count;
            item.use_gpu_deswizzle = can_gpu_deswizzle;
            item.footprints.resize(sub_count);
            item.num_rows.resize(sub_count);
            item.row_sizes.resize(sub_count);

            UINT64 req_bytes = 0u;
            if (can_gpu_deswizzle) {
                total_gpu_linear_bytes = wsx_xg::align_up(total_gpu_linear_bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
                item.upload_base_offset = total_gpu_linear_bytes;
                raw_device->GetCopyableFootprints(
                    &rec->desc, 0, sub_count, item.upload_base_offset,
                    item.footprints.data(), item.num_rows.data(), item.row_sizes.data(), &req_bytes);
                if (req_bytes == 0u) continue;
                total_gpu_linear_bytes += req_bytes;

                item.raw_src_offsets.resize(sub_count);
                for (UINT s = 0; s < sub_count; ++s) {
                    const UINT mip = s % mips;
                    total_staging_bytes = wsx_xg::align_up(total_staging_bytes, 256u);
                    item.raw_src_offsets[s] = total_staging_bytes;
                    total_staging_bytes += rec->mips[mip].size_bytes;
                }
            } else {
                total_staging_bytes = wsx_xg::align_up(total_staging_bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
                item.upload_base_offset = total_staging_bytes;
                raw_device->GetCopyableFootprints(
                    &rec->desc, 0, sub_count, item.upload_base_offset,
                    item.footprints.data(), item.num_rows.data(), item.row_sizes.data(), &req_bytes);
                if (req_bytes == 0u) continue;
                total_staging_bytes += req_bytes;
            }
            dirty.push_back(std::move(item));
        }

        if (ww_skipped > 0u) {
            mgr.record_write_watch_skips(ww_skipped);
        }
        if (dirty.empty() || total_staging_bytes == 0u) return false;

        std::lock_guard<std::mutex> qlk(mutex);
        if (!fence) {
            if (FAILED(raw_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
                return false;
            }
            fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }

        const UINT64 completed = fence->GetCompletedValue();
        UploadSlot* slot = nullptr;
        for (auto& s : slots) {
            if (s.fence_val <= completed) {
                slot = &s;
                break;
            }
        }
        if (!slot) {
            UploadSlot fresh{};
            if (FAILED(raw_device->CreateCommandAllocator(queue_type, IID_PPV_ARGS(&fresh.allocator)))) {
                return false;
            }
            if (FAILED(raw_device->CreateCommandList(
                    0, queue_type, fresh.allocator.Get(), nullptr, IID_PPV_ARGS(&fresh.cmd_list)))) {
                return false;
            }
            fresh.cmd_list->Close();
            slots.push_back(std::move(fresh));
            slot = &slots.back();
        }

        if (!slot->staging_buf || slot->staging_capacity < total_staging_bytes) {
            const UINT64 new_cap = wsx_xg::align_up(std::max<UINT64>(total_staging_bytes, 65536u), 65536u);
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = new_cap;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.Format = DXGI_FORMAT_UNKNOWN;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            ComPtr<ID3D12Resource> new_buf;
            if (FAILED(raw_device->CreateCommittedResource(
                    &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr, IID_PPV_ARGS(&new_buf)))) {
                return false;
            }
            slot->staging_buf = std::move(new_buf);
            slot->staging_capacity = new_cap;
        }

        if (total_gpu_linear_bytes > 0u &&
            (!slot->gpu_linear_buf || slot->gpu_linear_capacity < total_gpu_linear_bytes)) {
            const UINT64 new_cap = wsx_xg::align_up(std::max<UINT64>(total_gpu_linear_bytes, 65536u), 65536u);
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = new_cap;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.Format = DXGI_FORMAT_UNKNOWN;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            ComPtr<ID3D12Resource> new_buf;
            if (FAILED(raw_device->CreateCommittedResource(
                    &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON,
                    nullptr, IID_PPV_ARGS(&new_buf)))) {
                return false;
            }
            slot->gpu_linear_buf = std::move(new_buf);
            slot->gpu_linear_capacity = new_cap;
            slot->gpu_linear_state = D3D12_RESOURCE_STATE_COMMON;
        }

        if (FAILED(slot->allocator->Reset()) ||
            FAILED(slot->cmd_list->Reset(slot->allocator.Get(), nullptr))) {
            return false;
        }

        void* mapped_ptr = nullptr;
        D3D12_RANGE read_none{0, 0};
        if (FAILED(slot->staging_buf->Map(0, &read_none, &mapped_ptr)) || !mapped_ptr) {
            slot->cmd_list->Close();
            return false;
        }
        auto* staging_base = static_cast<uint8_t*>(mapped_ptr);

        uint64_t uploaded_count = 0;
        uint64_t deswizzled_count = 0;
        uint64_t gpu_deswizzled_count = 0;
        uint64_t uploaded_bytes = 0;
        bool any_gpu_deswizzle = false;

        // Pass 1: Copy guest memory into UPLOAD staging buffer (raw tiled for GPU deswizzle, linear for CPU path)
        for (auto& item : dirty) {
            auto& rec = *item.rec;
            std::lock_guard<std::mutex> rlk(rec.record_mutex);
            if (!rec.alive.load(std::memory_order_acquire) || !rec.backing_mem) {
                item.subresource_count = 0u;
                continue;
            }
            // Reset write-watch BEFORE copying so any concurrent write marks dirty again
            if (rec.has_write_watch) {
                ResetWriteWatch(rec.backing_mem, static_cast<SIZE_T>(rec.total_bytes));
            }
            const auto* guest_base = static_cast<const uint8_t*>(rec.backing_mem);
            const bool is_3d = (rec.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D);
            const UINT mips = std::max(1u, static_cast<UINT>(is_3d ? rec.mips3d.size() : rec.mips.size()));
            const bool is_tiled = !wsx_xg::is_linear_mode(rec.swizzle_mode);

            if (item.use_gpu_deswizzle) {
                for (UINT s = 0; s < item.subresource_count; ++s) {
                    const UINT mip = s % mips;
                    const UINT slice = s / mips;
                    const auto& sl = rec.mips[mip];
                    const uint8_t* src_sub = guest_base + slice * rec.slice_stride + sl.offset_bytes;
                    std::memcpy(staging_base + item.raw_src_offsets[s], src_sub, static_cast<size_t>(sl.size_bytes));
                }
                any_gpu_deswizzle = true;
                ++gpu_deswizzled_count;
            } else {
                for (UINT s = 0; s < item.subresource_count; ++s) {
                    const UINT mip = s % mips;
                    const UINT slice = s / mips;
                    const auto& fp = item.footprints[s];
                    uint8_t* dst_sub = staging_base + fp.Offset;
                    if (is_3d) {
                        const auto& sl3 = rec.mips3d[mip];
                        const uint8_t* src_sub = guest_base + sl3.offset_bytes;
                        const uint32_t linear_slice_pitch = fp.Footprint.RowPitch * item.num_rows[s];
                        wsx_xg::deswizzle_3d(sl3, src_sub, dst_sub, fp.Footprint.RowPitch, linear_slice_pitch);
                    } else {
                        const auto& sl = rec.mips[mip];
                        const uint8_t* src_sub = guest_base + slice * rec.slice_stride + sl.offset_bytes;
                        wsx_xg::deswizzle_2d(sl, src_sub, dst_sub, fp.Footprint.RowPitch);
                    }
                }
            }

            item.state = rec.gpu_state;
            rec.last_uploaded_hash = fast_hash_bytes(rec.backing_mem, static_cast<size_t>(rec.total_bytes));
            rec.force_upload = false;
            ++uploaded_count;
            if (is_tiled) ++deswizzled_count;
            uploaded_bytes += rec.total_bytes;
        }

        D3D12_RANGE written_range{0, static_cast<SIZE_T>(total_staging_bytes)};
        slot->staging_buf->Unmap(0, &written_range);

        if (uploaded_count == 0u) {
            slot->cmd_list->Close();
            return false;
        }

        // Pass 2: GPU SM 6.6 Compute Deswizzle for all tiled 2D items -> slot->gpu_linear_buf
        if (any_gpu_deswizzle && slot->gpu_linear_buf) {
            if (slot->gpu_linear_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = slot->gpu_linear_buf.Get();
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = slot->gpu_linear_state;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                slot->cmd_list->ResourceBarrier(1, &b);
                slot->gpu_linear_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }

            std::vector<GpuDeswizzleSubresourceJob> batch_jobs;
            for (const auto& item : dirty) {
                if (!item.use_gpu_deswizzle || item.subresource_count == 0u) continue;
                auto& rec = *item.rec;
                std::lock_guard<std::mutex> rlk(rec.record_mutex);
                const UINT mips = std::max(1u, static_cast<UINT>(rec.mips.size()));
                for (UINT s = 0; s < item.subresource_count; ++s) {
                    const UINT mip = s % mips;
                    const auto& fp = item.footprints[s];
                    GpuDeswizzleSubresourceJob jb{};
                    jb.src_byte_offset = item.raw_src_offsets[s];
                    jb.dst_byte_offset = fp.Offset;
                    jb.layout = rec.mips[mip];
                    jb.dst_row_pitch = fp.Footprint.RowPitch;
                    jb.pipe_bank_xor = 0u;
                    batch_jobs.push_back(jb);
                }
            }
            if (!batch_jobs.empty()) {
                gpu_deswizzler.record_deswizzle_batch(
                    slot->cmd_list.Get(),
                    slot->staging_buf.Get(),
                    slot->gpu_linear_buf.Get(),
                    batch_jobs.data(),
                    static_cast<uint32_t>(batch_jobs.size()));
            }

            D3D12_RESOURCE_BARRIER to_copy{};
            to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            to_copy.Transition.pResource = slot->gpu_linear_buf.Get();
            to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            slot->cmd_list->ResourceBarrier(1, &to_copy);
            slot->gpu_linear_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        }

        // Pass 3: CopyTextureRegion from linear source (gpu_linear_buf or staging_buf) -> guest texture
        for (const auto& item : dirty) {
            if (item.subresource_count == 0u) continue;
            auto& rec = *item.rec;
            const D3D12_RESOURCE_STATES prev_state = item.state;
            const bool need_barrier = is_direct && (prev_state != D3D12_RESOURCE_STATE_COPY_DEST);

            if (need_barrier) {
                D3D12_RESOURCE_BARRIER pre{};
                pre.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                pre.Transition.pResource = rec.raw_resource;
                pre.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                pre.Transition.StateBefore = prev_state;
                pre.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                slot->cmd_list->ResourceBarrier(1, &pre);
            }

            ID3D12Resource* src_buf = item.use_gpu_deswizzle
                ? slot->gpu_linear_buf.Get()
                : slot->staging_buf.Get();
            for (UINT s = 0; s < item.subresource_count; ++s) {
                D3D12_TEXTURE_COPY_LOCATION dst_loc{};
                dst_loc.pResource = rec.raw_resource;
                dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst_loc.SubresourceIndex = s;

                D3D12_TEXTURE_COPY_LOCATION src_loc{};
                src_loc.pResource = src_buf;
                src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src_loc.PlacedFootprint = item.footprints[s];

                slot->cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
            }

            if (need_barrier) {
                D3D12_RESOURCE_BARRIER post{};
                post.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                post.Transition.pResource = rec.raw_resource;
                post.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                post.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                post.Transition.StateAfter = prev_state;
                slot->cmd_list->ResourceBarrier(1, &post);
            }
        }

        if (FAILED(slot->cmd_list->Close())) {
            return false;
        }

        ID3D12CommandList* ul = slot->cmd_list.Get();
        raw_queue->ExecuteCommandLists(1, &ul);
        slot->fence_val = next_fence_val++;
        raw_queue->Signal(fence.Get(), slot->fence_val);

        mgr.record_flush_stats(uploaded_count, deswizzled_count, gpu_deswizzled_count, uploaded_bytes);
        wsx::trace_void("d3d12_xs", "GuestUploadContext::flush_dirty_textures",
                        "uploaded=%llu deswizzled=%llu gpu_deswizzled=%llu ww_skips=%llu bytes=%llu",
                        static_cast<unsigned long long>(uploaded_count),
                        static_cast<unsigned long long>(deswizzled_count),
                        static_cast<unsigned long long>(gpu_deswizzled_count),
                        static_cast<unsigned long long>(ww_skipped),
                        static_cast<unsigned long long>(uploaded_bytes));
        return true;
    }
};

struct __declspec(uuid("6b38f1c2-9d4e-47a1-8c35-2e71a9b04f18")) IWsxD3D12ResourceProxyMarker : IUnknown {};

class WsxD3D12ResourceProxy final : public ID3D12Resource2 {
    std::atomic<ULONG> ref_{1};
    ComPtr<ID3D12Device> owner_device_;
    ComPtr<ID3D12Resource> real0_;
    ComPtr<ID3D12Resource1> real1_;
    ComPtr<ID3D12Resource2> real2_;
    std::shared_ptr<GuestTextureManager> guest_mgr_;
    std::shared_ptr<GuestTextureRecord> record_;

public:
    WsxD3D12ResourceProxy(
        ID3D12Device* owner_device,
        ID3D12Resource* raw_res,
        std::shared_ptr<GuestTextureManager> guest_mgr,
        uint32_t initial_swizzle_mode,
        D3D12_RESOURCE_STATES initial_state) noexcept
        : owner_device_(owner_device),
          real0_(raw_res),
          guest_mgr_(std::move(guest_mgr)) {
        real0_.As(&real1_);
        real0_.As(&real2_);

        record_ = std::make_shared<GuestTextureRecord>();
        record_->raw_resource = real0_.Get();
        record_->desc = real0_->GetDesc();
        record_->swizzle_mode = initial_swizzle_mode;
        record_->gpu_state = initial_state;
    }

    ~WsxD3D12ResourceProxy() {
        if (guest_mgr_ && record_) {
            guest_mgr_->unregister_resource(record_->raw_resource);
        }
    }

    ID3D12Resource* raw_resource() const noexcept { return real0_.Get(); }
    const std::shared_ptr<GuestTextureRecord>& guest_record() const noexcept { return record_; }

    HRESULT set_swizzle_mode(uint32_t swizzle_mode) {
        if (!record_ || !guest_mgr_) return E_FAIL;
        bool newly_allocated = false;
        {
            std::lock_guard<std::mutex> lk(record_->record_mutex);
            if (record_->swizzle_mode != swizzle_mode) {
                record_->swizzle_mode = swizzle_mode;
                if (record_->backing_mem) {
                    const uint64_t old_size = record_->total_bytes;
                    record_->rebuild_layout_locked();
                    if (record_->total_bytes > old_size) {
                        bool new_ww = false;
                        void* fresh = GuestTextureRecord::alloc_guest_backing(
                            static_cast<SIZE_T>(record_->total_bytes), new_ww);
                        if (!fresh) return E_OUTOFMEMORY;
                        std::memcpy(fresh, record_->backing_mem, static_cast<size_t>(old_size));
                        VirtualFree(record_->backing_mem, 0, MEM_RELEASE);
                        record_->backing_mem = fresh;
                        record_->has_write_watch = new_ww;
                    }
                    record_->force_upload = true;
                }
            }
            if (!record_->backing_mem) {
                if (!record_->ensure_backing_locked(newly_allocated)) return E_OUTOFMEMORY;
            }
        }
        guest_mgr_->register_active(record_, newly_allocated);
        return S_OK;
    }

    static WsxD3D12ResourceProxy* from_interface(IUnknown* unk) noexcept {
        if (!unk) return nullptr;
        WsxD3D12ResourceProxy* proxy = nullptr;
        if (SUCCEEDED(unk->QueryInterface(__uuidof(IWsxD3D12ResourceProxyMarker),
                                          reinterpret_cast<void**>(&proxy))) && proxy) {
            return proxy;
        }
        return nullptr;
    }

    static ID3D12Resource* unwrap(ID3D12Resource* res) noexcept {
        if (!res) return nullptr;
        if (auto* p = from_interface(res)) return p->raw_resource();
        return res;
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IWsxD3D12ResourceProxyMarker)) {
            *ppvObject = this;
            return S_OK; // Non-AddRef internal unwrap marker
        }
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(ID3D12Object) ||
            riid == __uuidof(ID3D12DeviceChild) ||
            riid == __uuidof(ID3D12Pageable) ||
            riid == __uuidof(ID3D12Resource) ||
            (riid == __uuidof(ID3D12Resource1) && real1_) ||
            (riid == __uuidof(ID3D12Resource2) && real2_)) {
            *ppvObject = static_cast<ID3D12Resource2*>(this);
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
        if (owner_device_) return owner_device_->QueryInterface(riid, ppvDevice);
        return real0_->GetDevice(riid, ppvDevice);
    }

    // ID3D12Resource
    HRESULT STDMETHODCALLTYPE Map(UINT Subresource, const D3D12_RANGE* pReadRange, void** ppData) override {
        const HRESULT hr = real0_->Map(Subresource, pReadRange, ppData);
        if (SUCCEEDED(hr)) return hr;
        if (record_ && guest_mgr_ &&
            (record_->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
             record_->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
             record_->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)) {
            void* ptr = nullptr;
            bool newly_allocated = false;
            uint64_t sub_off = 0ULL;
            {
                std::lock_guard<std::mutex> lk(record_->record_mutex);
                ptr = record_->ensure_backing_locked(newly_allocated);
                if (ptr) {
                    const bool is_3d = (record_->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D);
                    const UINT mips = std::max(1u, static_cast<UINT>(is_3d ? record_->mips3d.size() : record_->mips.size()));
                    const UINT mip = Subresource % mips;
                    const UINT slice = Subresource / mips;
                    if (is_3d && mip < record_->mips3d.size()) {
                        sub_off = record_->mips3d[mip].offset_bytes;
                    } else if (!is_3d && mip < record_->mips.size()) {
                        sub_off = static_cast<uint64_t>(slice) * record_->slice_stride + record_->mips[mip].offset_bytes;
                    }
                    if (sub_off >= record_->total_bytes) sub_off = 0ULL;
                }
            }
            if (ptr) {
                guest_mgr_->register_active(record_, newly_allocated);
                if (ppData) {
                    *ppData = static_cast<uint8_t*>(ptr) + sub_off;
                }
                return S_OK;
            }
        }
        return hr;
    }
    void STDMETHODCALLTYPE Unmap(UINT Subresource, const D3D12_RANGE* pWrittenRange) override {
        if (record_ &&
            (record_->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
             record_->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
             record_->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)) {
            bool has_backing = false;
            {
                std::lock_guard<std::mutex> lk(record_->record_mutex);
                if (record_->backing_mem) {
                    has_backing = true;
                    if (!pWrittenRange || pWrittenRange->End > pWrittenRange->Begin) {
                        record_->force_upload = true;
                    }
                }
            }
            if (has_backing) return;
        }
        real0_->Unmap(Subresource, pWrittenRange);
    }
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override {
        return real0_->GetDesc();
    }
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override {
        if (record_ && guest_mgr_ &&
            (record_->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
             record_->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
             record_->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)) {
            void* ptr = nullptr;
            bool newly_allocated = false;
            {
                std::lock_guard<std::mutex> lk(record_->record_mutex);
                ptr = record_->ensure_backing_locked(newly_allocated);
            }
            if (ptr) {
                guest_mgr_->register_active(record_, newly_allocated);
                return static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(reinterpret_cast<uintptr_t>(ptr));
            }
        }
        return real0_->GetGPUVirtualAddress();
    }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(
        UINT DstSubresource, const D3D12_BOX* pDstBox, const void* pSrcData,
        UINT SrcRowPitch, UINT SrcDepthPitch) override {
        return real0_->WriteToSubresource(DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
    }
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(
        void* pDstData, UINT DstRowPitch, UINT DstDepthPitch,
        UINT SrcSubresource, const D3D12_BOX* pSrcBox) override {
        return real0_->ReadFromSubresource(pDstData, DstRowPitch, DstDepthPitch, SrcSubresource, pSrcBox);
    }
    HRESULT STDMETHODCALLTYPE GetHeapProperties(
        D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS* pHeapFlags) override {
        return real0_->GetHeapProperties(pHeapProperties, pHeapFlags);
    }

    // ID3D12Resource1
    HRESULT STDMETHODCALLTYPE GetProtectedResourceSession(REFIID riid, void** ppProtectedSession) override {
        return real1_ ? real1_->GetProtectedResourceSession(riid, ppProtectedSession) : E_NOINTERFACE;
    }

    // ID3D12Resource2
    D3D12_RESOURCE_DESC1 STDMETHODCALLTYPE GetDesc1() override {
        return real2_ ? real2_->GetDesc1() : D3D12_RESOURCE_DESC1{};
    }
};

inline HRESULT readback_guest_texture(
    ID3D12Device* raw_device,
    ID3D12CommandQueue* raw_queue,
    D3D12_COMMAND_LIST_TYPE queue_type,
    const std::shared_ptr<GuestTextureRecord>& rec) noexcept {
    if (!raw_device || !raw_queue || !rec) return E_INVALIDARG;
    std::lock_guard<std::mutex> rlk(rec->record_mutex);
    if (!rec->alive.load(std::memory_order_acquire) || !rec->raw_resource) return E_HANDLE;
    bool newly = false;
    if (!rec->ensure_backing_locked(newly)) return E_OUTOFMEMORY;

    const bool is_3d = (rec->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D);
    const UINT mips = std::max(1u, static_cast<UINT>(is_3d ? rec->mips3d.size() : rec->mips.size()));
    const UINT slices = is_3d ? 1u : std::max(1u, static_cast<UINT>(rec->desc.DepthOrArraySize));
    const UINT sub_count = mips * slices;

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fps(sub_count);
    std::vector<UINT> nrows(sub_count);
    std::vector<UINT64> rsizes(sub_count);
    UINT64 req_bytes = 0;
    raw_device->GetCopyableFootprints(&rec->desc, 0, sub_count, 0, fps.data(), nrows.data(), rsizes.data(), &req_bytes);
    if (req_bytes == 0u) return S_OK;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = wsx_xg::align_up(req_bytes, 256u);
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> rb;
    HRESULT hr = raw_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));
    if (FAILED(hr)) return hr;

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(hr = raw_device->CreateCommandAllocator(queue_type, IID_PPV_ARGS(&alloc)))) return hr;
    if (FAILED(hr = raw_device->CreateCommandList(0, queue_type, alloc.Get(), nullptr, IID_PPV_ARGS(&cl)))) return hr;
    if (FAILED(hr = raw_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return hr;

    const bool is_direct = (queue_type == D3D12_COMMAND_LIST_TYPE_DIRECT);
    const D3D12_RESOURCE_STATES prev_state = rec->gpu_state;
    const bool need_bar = is_direct && (prev_state != D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (need_bar) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = rec->raw_resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = prev_state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
    }

    for (UINT s = 0; s < sub_count; ++s) {
        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = rb.Get();
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_loc.PlacedFootprint = fps[s];
        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = rec->raw_resource;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_loc.SubresourceIndex = s;
        cl->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
    }

    if (need_bar) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = rec->raw_resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = prev_state;
        cl->ResourceBarrier(1, &b);
    }

    if (FAILED(hr = cl->Close())) return hr;
    ID3D12CommandList* lists[] = {cl.Get()};
    raw_queue->ExecuteCommandLists(1, lists);
    if (FAILED(hr = raw_queue->Signal(fence.Get(), 1u))) return hr;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) return E_OUTOFMEMORY;
    fence->SetEventOnCompletion(1u, ev);
    WaitForSingleObject(ev, 5000);
    CloseHandle(ev);

    void* mapped = nullptr;
    D3D12_RANGE rr{0, static_cast<SIZE_T>(req_bytes)};
    if (FAILED(hr = rb->Map(0, &rr, &mapped)) || !mapped) return FAILED(hr) ? hr : E_FAIL;
    const auto* src_base = static_cast<const uint8_t*>(mapped);
    auto* guest_base = static_cast<uint8_t*>(rec->backing_mem);

    for (UINT s = 0; s < sub_count; ++s) {
        const UINT mip = s % mips;
        const UINT slice = s / mips;
        const auto& fp = fps[s];
        const uint8_t* src_sub = src_base + fp.Offset;
        if (is_3d) {
            const auto& sl3 = rec->mips3d[mip];
            const uint32_t linear_slice_pitch = fp.Footprint.RowPitch * nrows[s];
            wsx_xg::swizzle_3d(sl3, src_sub, fp.Footprint.RowPitch, linear_slice_pitch, guest_base + sl3.offset_bytes);
        } else {
            const auto& sl = rec->mips[mip];
            wsx_xg::swizzle_2d(sl, src_sub, fp.Footprint.RowPitch, guest_base + slice * rec->slice_stride + sl.offset_bytes);
        }
    }
    D3D12_RANGE wr{0, 0};
    rb->Unmap(0, &wr);
    if (rec->has_write_watch) {
        ResetWriteWatch(rec->backing_mem, static_cast<SIZE_T>(rec->total_bytes));
    }
    rec->last_uploaded_hash = fast_hash_bytes(rec->backing_mem, static_cast<size_t>(rec->total_bytes));
    rec->force_upload = false;
    return S_OK;
}

inline ID3D12Resource* unwrap_resource(ID3D12Resource* res) noexcept {
    return WsxD3D12ResourceProxy::unwrap(res);
}

inline ID3D12Pageable* unwrap_pageable(ID3D12Pageable* obj) noexcept {
    if (!obj) return nullptr;
    if (auto* rp = WsxD3D12ResourceProxy::from_interface(obj)) {
        return rp->raw_resource();
    }
    return obj;
}

} // namespace wsx_d3d12
