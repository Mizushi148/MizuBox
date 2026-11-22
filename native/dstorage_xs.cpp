// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox Series X|S DirectStorage runtime (dstorage_xs.dll):
// - Real async worker-thread I/O queue (IDStorageQueueX)
// - Win32 file-to-memory & memory-to-memory transfers
// - RFC 1950 (zlib) & RFC 1951 (raw deflate) CPU decompression via wsx_inflate.h
// - IDStorageStatusArrayX completion & HRESULT reporting
// - Real ID3D12Fence::Signal via EnqueueSignal + Win32 HANDLE signaling via EnqueueSetEvent
// - Pre-submit cancellation via CancelRequestsWithTag(mask, value)

#include "include/xbox/dstorage_xs.h"
#include "wsx_inflate.h"
#include "wsx_gdeflate.h"
#include "wsx_gdeflate_gpu_dx12.h"
#include "wsx_bcpack.h"
#include "wsx_bcpack_gpu_dx12.h"
#include "wsx_xvd.h"

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

std::mutex g_config_mutex;
DSTORAGE_CONFIGURATION1 g_config{};

struct __declspec(uuid("51c3912b-3c0e-4d5e-b592-12a6b8c4e711")) IWsxDStorageFileInternal : IUnknown {
    virtual const std::wstring& path() const noexcept = 0;
    virtual HRESULT read_at(UINT64 offset, void* dst, UINT32 size, DWORD& out_read) noexcept = 0;
};

class WsxDStorageFile final : public IDStorageFileX, public IWsxDStorageFileInternal {
    std::atomic<ULONG> ref_{1};
    mutable std::shared_mutex io_mutex_; // Shared lock allows concurrent lock-free positional OVERLAPPED NVMe reads!
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::wstring path_;
    bool overlapped_ = false;

public:
    WsxDStorageFile(HANDLE h, std::wstring path, bool overlapped = false) noexcept
        : handle_(h), path_(std::move(path)), overlapped_(overlapped) {}

    ~WsxDStorageFile() {
        Close();
    }

    const std::wstring& path() const noexcept override { return path_; }

    HRESULT read_at(UINT64 offset, void* dst, UINT32 size, DWORD& out_read) noexcept override {
        out_read = 0;
        if (!dst && size > 0) return E_POINTER;
        if (size == 0) return S_OK;

        // Lock-free concurrent positional OVERLAPPED I/O path (zero exclusive mutex contention across threads!)
        if (overlapped_) {
            std::shared_lock<std::shared_mutex> lk(io_mutex_);
            if (handle_ == INVALID_HANDLE_VALUE) return E_HANDLE;

            HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!ev) return E_OUTOFMEMORY;

            uint8_t* out_ptr = static_cast<uint8_t*>(dst);
            UINT64 cur_off = offset;
            UINT32 remaining = size;
            HRESULT final_hr = S_OK;
            while (remaining > 0) {
                ResetEvent(ev);
                OVERLAPPED ov{};
                ov.Offset = static_cast<DWORD>(cur_off & 0xFFFFFFFFull);
                ov.OffsetHigh = static_cast<DWORD>((cur_off >> 32) & 0xFFFFFFFFull);
                ov.hEvent = ev;

                DWORD chunk = 0;
                if (!ReadFile(handle_, out_ptr, remaining, &chunk, &ov)) {
                    const DWORD err = GetLastError();
                    if (err == ERROR_IO_PENDING) {
                        if (!GetOverlappedResult(handle_, &ov, &chunk, TRUE)) {
                            const DWORD gerr = GetLastError();
                            final_hr = (gerr == ERROR_HANDLE_EOF)
                                ? HRESULT_FROM_WIN32(ERROR_HANDLE_EOF)
                                : HRESULT_FROM_WIN32(gerr);
                            break;
                        }
                    } else {
                        final_hr = HRESULT_FROM_WIN32(err);
                        break;
                    }
                }
                if (chunk == 0) {
                    final_hr = HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
                    break;
                }
                out_ptr += chunk;
                cur_off += chunk;
                remaining -= chunk;
                out_read += chunk;
            }
            CloseHandle(ev);
            return final_hr;
        }

        std::unique_lock<std::shared_mutex> lk(io_mutex_);
        if (handle_ == INVALID_HANDLE_VALUE) return E_HANDLE;

        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        uint8_t* out_ptr = static_cast<uint8_t*>(dst);
        UINT32 remaining = size;
        while (remaining > 0) {
            DWORD chunk = 0;
            if (!ReadFile(handle_, out_ptr, remaining, &chunk, nullptr)) {
                return HRESULT_FROM_WIN32(GetLastError());
            }
            if (chunk == 0) {
                return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
            }
            out_ptr += chunk;
            remaining -= chunk;
            out_read += chunk;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDStorageFileX)) {
            *ppv = static_cast<IDStorageFileX*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IWsxDStorageFileInternal)) {
            *ppv = static_cast<IWsxDStorageFileInternal*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    void STDMETHODCALLTYPE Close() override {
        std::unique_lock<std::shared_mutex> lk(io_mutex_);
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    HRESULT STDMETHODCALLTYPE GetFileInformation(BY_HANDLE_FILE_INFORMATION* info) override {
        if (!info) return E_POINTER;
        std::shared_lock<std::shared_mutex> lk(io_mutex_);
        if (handle_ == INVALID_HANDLE_VALUE) return E_HANDLE;
        if (!GetFileInformationByHandle(handle_, info)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        return S_OK;
    }
};

class WsxDStorageXvdFile final : public IDStorageFileX, public IWsxDStorageFileInternal {
    std::atomic<ULONG> ref_{1};
    mutable std::mutex io_mutex_;
    wsx_xvd::XvdVolumeReader reader_;

public:
    WsxDStorageXvdFile() = default;
    ~WsxDStorageXvdFile() { Close(); }

    HRESULT open_volume(const std::wstring& path) noexcept {
        std::lock_guard<std::mutex> lk(io_mutex_);
        const wsx_xvd::XvdStatus st = reader_.open(path);
        switch (st) {
        case wsx_xvd::XvdStatus::Ok: return S_OK;
        case wsx_xvd::XvdStatus::EncryptedUnsupported: return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        case wsx_xvd::XvdStatus::DynamicUnsupported: return E_NOTIMPL;
        case wsx_xvd::XvdStatus::InvalidHeader: return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        case wsx_xvd::XvdStatus::OutOfRange: return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        default: return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_OPEN_FAILED);
        }
    }

    const std::wstring& path() const noexcept override { return reader_.path(); }

    HRESULT read_at(UINT64 offset, void* dst, UINT32 size, DWORD& out_read) noexcept override {
        std::lock_guard<std::mutex> lk(io_mutex_);
        if (!reader_.is_open()) return E_HANDLE;
        const wsx_xvd::XvdStatus st = reader_.read_user_data(offset, dst, size, out_read);
        if (st == wsx_xvd::XvdStatus::Ok) return S_OK;
        if (st == wsx_xvd::XvdStatus::OutOfRange) return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        return E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDStorageFileX)) {
            *ppv = static_cast<IDStorageFileX*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IWsxDStorageFileInternal)) {
            *ppv = static_cast<IWsxDStorageFileInternal*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    void STDMETHODCALLTYPE Close() override {
        std::lock_guard<std::mutex> lk(io_mutex_);
        reader_.close();
    }

    HRESULT STDMETHODCALLTYPE GetFileInformation(BY_HANDLE_FILE_INFORMATION* info) override {
        if (!info) return E_POINTER;
        std::lock_guard<std::mutex> lk(io_mutex_);
        if (!reader_.is_open()) return E_HANDLE;
        if (!GetFileInformationByHandle(reader_.native_handle(), info)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        const uint64_t user_sz = reader_.readable_user_data_size();
        info->nFileSizeLow = static_cast<DWORD>(user_sz & 0xFFFFFFFFULL);
        info->nFileSizeHigh = static_cast<DWORD>((user_sz >> 32) & 0xFFFFFFFFULL);
        return S_OK;
    }
};

struct __declspec(uuid("2f9c74d1-62a0-4813-9a67-35b102f8c7a4")) IWsxDStorageStatusInternal : IUnknown {
    virtual void mark_pending(UINT32 index) noexcept = 0;
    virtual void mark_complete(UINT32 index, HRESULT hr) noexcept = 0;
};

class WsxDStorageStatusArray final : public IDStorageStatusArrayX, public IWsxDStorageStatusInternal {
    std::atomic<ULONG> ref_{1};
    mutable std::mutex mutex_;
    std::vector<bool> complete_;
    std::vector<HRESULT> results_;
    std::string name_;

public:
    WsxDStorageStatusArray(UINT32 capacity, const CHAR* name)
        : complete_(capacity, true),
          results_(capacity, S_OK),
          name_(name ? name : "") {}

    void mark_pending(UINT32 index) noexcept override {
        std::lock_guard<std::mutex> lk(mutex_);
        if (index < complete_.size()) {
            complete_[index] = false;
            results_[index] = E_PENDING;
        }
    }

    void mark_complete(UINT32 index, HRESULT hr) noexcept override {
        std::lock_guard<std::mutex> lk(mutex_);
        if (index < complete_.size()) {
            results_[index] = hr;
            complete_[index] = true;
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDStorageStatusArrayX)) {
            *ppv = static_cast<IDStorageStatusArrayX*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IWsxDStorageStatusInternal)) {
            *ppv = static_cast<IWsxDStorageStatusInternal*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    bool STDMETHODCALLTYPE IsComplete(UINT32 index) override {
        std::lock_guard<std::mutex> lk(mutex_);
        if (index >= complete_.size()) return true;
        return complete_[index];
    }

    HRESULT STDMETHODCALLTYPE GetHResult(UINT32 index) override {
        std::lock_guard<std::mutex> lk(mutex_);
        if (index >= results_.size()) return E_INVALIDARG;
        return results_[index];
    }
};

struct QueuedCommand {
    DSTORAGE_COMMAND_TYPE type = DSTORAGE_COMMAND_TYPE_NONE;
    DSTORAGE_REQUEST request{};
    ComPtr<IDStorageFileX> file_ref;
    std::string request_name;
    ComPtr<IDStorageStatusArrayX> status_array;
    UINT32 status_index = 0;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_value = 0;
    HANDLE event_handle = nullptr;
    bool cancelled = false;
};

class WsxDStorageQueue final : public IDStorageQueueX {
    std::atomic<ULONG> ref_{1};
    DSTORAGE_QUEUE_DESC desc_{};
    std::string name_;
    ComPtr<ID3D12Device> device_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<QueuedCommand> staged_;
    std::deque<QueuedCommand> submitted_;
    bool closing_ = false;
    bool worker_started_ = false;
    std::thread worker_;

    HANDLE error_event_ = nullptr;
    DSTORAGE_ERROR_RECORD error_record_{};
    HRESULT batch_error_ = S_OK;

    // Lazy D3D12 COPY queue & SM 6.6 GPU GDeflate decoder (used strictly on worker thread)
    ComPtr<ID3D12Device> copy_device_;
    ComPtr<ID3D12CommandQueue> copy_queue_;
    ComPtr<ID3D12CommandAllocator> copy_alloc_;
    ComPtr<ID3D12GraphicsCommandList> copy_list_;
    ComPtr<ID3D12Fence> copy_fence_;
    HANDLE copy_fence_event_ = nullptr;
    UINT64 copy_fence_val_ = 0u;
    wsx_gdeflate_gpu::WsxGDeflateGpuDecoder gpu_gdeflate_;
    wsx_bcpack_gpu::WsxBcPackGpuDecoder gpu_bcpack_;

    HRESULT ensure_copy_engine(ID3D12Device* dev) noexcept {
        if (!dev) return E_INVALIDARG;
        if (copy_queue_ && copy_device_.Get() == dev) return S_OK;

        copy_list_.Reset();
        copy_alloc_.Reset();
        copy_queue_.Reset();
        copy_fence_.Reset();
        copy_device_ = dev;

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        HRESULT hr = copy_device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&copy_queue_));
        if (FAILED(hr)) return hr;
        hr = copy_device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&copy_alloc_));
        if (FAILED(hr)) return hr;
        hr = copy_device_->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_COPY, copy_alloc_.Get(),
                                             nullptr, IID_PPV_ARGS(&copy_list_));
        if (FAILED(hr)) return hr;
        copy_list_->Close();

        hr = copy_device_->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_fence_));
        if (FAILED(hr)) return hr;
        if (!copy_fence_event_) {
            copy_fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!copy_fence_event_) return E_OUTOFMEMORY;
        }
        return S_OK;
    }

    HRESULT submit_copy_and_wait() noexcept {
        HRESULT hr = copy_list_->Close();
        if (FAILED(hr)) return hr;
        ID3D12CommandList* lists[] = { copy_list_.Get() };
        copy_queue_->ExecuteCommandLists(1u, lists);
        ++copy_fence_val_;
        hr = copy_queue_->Signal(copy_fence_.Get(), copy_fence_val_);
        if (FAILED(hr)) return hr;
        if (copy_fence_->GetCompletedValue() < copy_fence_val_) {
            hr = copy_fence_->SetEventOnCompletion(copy_fence_val_, copy_fence_event_);
            if (FAILED(hr)) return hr;
            WaitForSingleObject(copy_fence_event_, 10000u);
        }
        return S_OK;
    }

    HRESULT create_staging_upload(ID3D12Device* dev, UINT64 bytes, ComPtr<ID3D12Resource>& out_buf) noexcept {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (std::max<UINT64>(256u, bytes) + 255u) & ~255ull;
        rd.Height = 1u;
        rd.DepthOrArraySize = 1u;
        rd.MipLevels = 1u;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1u;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out_buf));
    }

    HRESULT upload_to_default_buffer(
        ID3D12Resource* dst_res,
        UINT64 dst_offset,
        const uint8_t* payload,
        size_t payload_size) noexcept {
        ComPtr<ID3D12Device> dev = device_;
        if (!dev) {
            const HRESULT dhr = dst_res->GetDevice(IID_PPV_ARGS(&dev));
            if (FAILED(dhr) || !dev) return FAILED(dhr) ? dhr : E_FAIL;
        }
        HRESULT hr = ensure_copy_engine(dev.Get());
        if (FAILED(hr)) return hr;

        ComPtr<ID3D12Resource> staging;
        hr = create_staging_upload(dev.Get(), payload_size, staging);
        if (FAILED(hr)) return hr;

        void* mp = nullptr;
        D3D12_RANGE rr{0, 0};
        hr = staging->Map(0, &rr, &mp);
        if (FAILED(hr)) return hr;
        if (payload_size > 0u) std::memcpy(mp, payload, payload_size);
        staging->Unmap(0, nullptr);

        hr = copy_alloc_->Reset();
        if (FAILED(hr)) return hr;
        hr = copy_list_->Reset(copy_alloc_.Get(), nullptr);
        if (FAILED(hr)) return hr;

        copy_list_->CopyBufferRegion(dst_res, dst_offset, staging.Get(), 0u, payload_size);
        return submit_copy_and_wait();
    }

    HRESULT upload_to_texture_region(
        const DSTORAGE_DESTINATION_TEXTURE_REGION& tex_dst,
        const uint8_t* payload,
        size_t payload_size) noexcept {
        if (!tex_dst.Resource) return E_INVALIDARG;
        ComPtr<ID3D12Device> dev = device_;
        if (!dev) {
            const HRESULT dhr = tex_dst.Resource->GetDevice(IID_PPV_ARGS(&dev));
            if (FAILED(dhr) || !dev) return FAILED(dhr) ? dhr : E_FAIL;
        }
        HRESULT hr = ensure_copy_engine(dev.Get());
        if (FAILED(hr)) return hr;

        const D3D12_RESOURCE_DESC rdesc = tex_dst.Resource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT full_fp{};
        UINT full_rows = 0u;
        UINT64 full_row_bytes = 0u;
        UINT64 full_total = 0u;
        dev->GetCopyableFootprints(&rdesc, tex_dst.SubresourceIndex, 1u, 0u,
                                   &full_fp, &full_rows, &full_row_bytes, &full_total);

        const bool has_box = (tex_dst.Region.right > tex_dst.Region.left &&
                              tex_dst.Region.bottom > tex_dst.Region.top);
        const UINT dst_x = has_box ? tex_dst.Region.left : 0u;
        const UINT dst_y = has_box ? tex_dst.Region.top : 0u;
        const UINT dst_z = has_box ? tex_dst.Region.front : 0u;
        const UINT reg_w = has_box ? (tex_dst.Region.right - tex_dst.Region.left) : full_fp.Footprint.Width;
        const UINT reg_h = has_box ? (tex_dst.Region.bottom - tex_dst.Region.top) : full_fp.Footprint.Height;
        const UINT reg_d = (has_box && tex_dst.Region.back > tex_dst.Region.front)
            ? (tex_dst.Region.back - tex_dst.Region.front)
            : full_fp.Footprint.Depth;

        D3D12_RESOURCE_DESC reg_desc = rdesc;
        reg_desc.Width = reg_w;
        reg_desc.Height = reg_h;
        reg_desc.DepthOrArraySize = static_cast<UINT16>(reg_d);
        reg_desc.MipLevels = 1u;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT stage_fp{};
        UINT stage_rows = 0u;
        UINT64 stage_row_bytes = 0u;
        UINT64 stage_total = 0u;
        dev->GetCopyableFootprints(&reg_desc, 0u, 1u, 0u,
                                   &stage_fp, &stage_rows, &stage_row_bytes, &stage_total);

        const UINT64 required_src_bytes = stage_row_bytes * static_cast<UINT64>(stage_rows) * static_cast<UINT64>(reg_d);
        if (payload_size < required_src_bytes) {
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        }

        ComPtr<ID3D12Resource> staging;
        hr = create_staging_upload(dev.Get(), stage_total, staging);
        if (FAILED(hr)) return hr;

        void* mp = nullptr;
        D3D12_RANGE rr{0, 0};
        hr = staging->Map(0, &rr, &mp);
        if (FAILED(hr)) return hr;
        auto* dst_base = static_cast<uint8_t*>(mp);
        const UINT64 dst_slice_pitch = static_cast<UINT64>(stage_fp.Footprint.RowPitch) * stage_rows;
        size_t src_off = 0u;
        for (UINT z = 0u; z < reg_d; ++z) {
            for (UINT y = 0u; y < stage_rows; ++y) {
                std::memcpy(
                    dst_base + stage_fp.Offset + z * dst_slice_pitch + static_cast<UINT64>(y) * stage_fp.Footprint.RowPitch,
                    payload + src_off,
                    static_cast<size_t>(stage_row_bytes));
                src_off += static_cast<size_t>(stage_row_bytes);
            }
        }
        staging->Unmap(0, nullptr);

        hr = copy_alloc_->Reset();
        if (FAILED(hr)) return hr;
        hr = copy_list_->Reset(copy_alloc_.Get(), nullptr);
        if (FAILED(hr)) return hr;

        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = tex_dst.Resource;
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_loc.SubresourceIndex = tex_dst.SubresourceIndex;

        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = staging.Get();
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_loc.PlacedFootprint = stage_fp;

        copy_list_->CopyTextureRegion(&dst_loc, dst_x, dst_y, dst_z, &src_loc, nullptr);
        return submit_copy_and_wait();
    }

    HRESULT upload_to_multiple_subresources(
        const DSTORAGE_DESTINATION_MULTIPLE_SUBRESOURCES& multi_dst,
        const uint8_t* payload,
        size_t payload_size) noexcept {
        if (!multi_dst.Resource) return E_INVALIDARG;
        ComPtr<ID3D12Device> dev = device_;
        if (!dev) {
            const HRESULT dhr = multi_dst.Resource->GetDevice(IID_PPV_ARGS(&dev));
            if (FAILED(dhr) || !dev) return FAILED(dhr) ? dhr : E_FAIL;
        }
        HRESULT hr = ensure_copy_engine(dev.Get());
        if (FAILED(hr)) return hr;

        const D3D12_RESOURCE_DESC rdesc = multi_dst.Resource->GetDesc();
        const UINT mips = rdesc.MipLevels ? rdesc.MipLevels : 1u;
        const UINT slices = (rdesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? 1u : rdesc.DepthOrArraySize;
        const UINT total_subs = mips * slices;
        if (multi_dst.FirstSubresource >= total_subs) return E_INVALIDARG;
        const UINT count = total_subs - multi_dst.FirstSubresource;

        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fps(count);
        std::vector<UINT> rows(count);
        std::vector<UINT64> row_bytes(count);
        UINT64 total_upload = 0u;
        dev->GetCopyableFootprints(&rdesc, multi_dst.FirstSubresource, count, 0u,
                                   fps.data(), rows.data(), row_bytes.data(), &total_upload);

        UINT64 required_src = 0u;
        for (UINT i = 0u; i < count; ++i) {
            required_src += row_bytes[i] * static_cast<UINT64>(rows[i]) * static_cast<UINT64>(fps[i].Footprint.Depth);
        }
        if (payload_size < required_src) {
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        }

        ComPtr<ID3D12Resource> staging;
        hr = create_staging_upload(dev.Get(), total_upload, staging);
        if (FAILED(hr)) return hr;

        void* mp = nullptr;
        D3D12_RANGE rr{0, 0};
        hr = staging->Map(0, &rr, &mp);
        if (FAILED(hr)) return hr;
        auto* dst_base = static_cast<uint8_t*>(mp);
        size_t src_off = 0u;
        for (UINT i = 0u; i < count; ++i) {
            const UINT depth = fps[i].Footprint.Depth;
            const UINT64 slice_pitch = static_cast<UINT64>(fps[i].Footprint.RowPitch) * rows[i];
            for (UINT z = 0u; z < depth; ++z) {
                for (UINT y = 0u; y < rows[i]; ++y) {
                    std::memcpy(
                        dst_base + fps[i].Offset + z * slice_pitch + static_cast<UINT64>(y) * fps[i].Footprint.RowPitch,
                        payload + src_off,
                        static_cast<size_t>(row_bytes[i]));
                    src_off += static_cast<size_t>(row_bytes[i]);
                }
            }
        }
        staging->Unmap(0, nullptr);

        hr = copy_alloc_->Reset();
        if (FAILED(hr)) return hr;
        hr = copy_list_->Reset(copy_alloc_.Get(), nullptr);
        if (FAILED(hr)) return hr;

        for (UINT i = 0u; i < count; ++i) {
            D3D12_TEXTURE_COPY_LOCATION dst_loc{};
            dst_loc.pResource = multi_dst.Resource;
            dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst_loc.SubresourceIndex = multi_dst.FirstSubresource + i;

            D3D12_TEXTURE_COPY_LOCATION src_loc{};
            src_loc.pResource = staging.Get();
            src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src_loc.PlacedFootprint = fps[i];

            copy_list_->CopyTextureRegion(&dst_loc, 0u, 0u, 0u, &src_loc, nullptr);
        }
        return submit_copy_and_wait();
    }

    void record_error_locked(HRESULT hr, const QueuedCommand& cmd) noexcept {
        if (error_record_.FailureCount == 0) {
            error_record_.FirstFailure.HResult = hr;
            error_record_.FirstFailure.CommandType = cmd.type;
            error_record_.FirstFailure.Request.Request = cmd.request;
            std::memset(error_record_.FirstFailure.Request.Filename, 0,
                        sizeof(error_record_.FirstFailure.Request.Filename));
            std::memset(error_record_.FirstFailure.Request.RequestName, 0,
                        sizeof(error_record_.FirstFailure.Request.RequestName));
            if (!cmd.request_name.empty()) {
                strncpy_s(error_record_.FirstFailure.Request.RequestName,
                          sizeof(error_record_.FirstFailure.Request.RequestName),
                          cmd.request_name.c_str(),
                          _TRUNCATE);
            }
            if (cmd.file_ref) {
                ComPtr<IWsxDStorageFileInternal> finternal;
                if (SUCCEEDED(cmd.file_ref.As(&finternal)) && finternal) {
                    wcsncpy_s(error_record_.FirstFailure.Request.Filename,
                              MAX_PATH,
                              finternal->path().c_str(),
                              _TRUNCATE);
                }
            }
        }
        ++error_record_.FailureCount;
        if (error_event_) {
            SetEvent(error_event_);
        }
    }

    HRESULT execute_request(const QueuedCommand& cmd) {
        if (cmd.cancelled) return S_OK;

        const DSTORAGE_REQUEST& req = cmd.request;
        std::vector<uint8_t> raw_storage;
        const uint8_t* src_bytes = nullptr;
        size_t src_size = 0;

        if (req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE) {
            if (!cmd.file_ref) return E_INVALIDARG;
            src_size = req.Source.File.Size;
            raw_storage.resize(src_size);
            ComPtr<IWsxDStorageFileInternal> finternal;
            if (FAILED(cmd.file_ref.As(&finternal)) || !finternal) return E_NOINTERFACE;
            DWORD bytes_read = 0;
            const HRESULT hr = finternal->read_at(
                req.Source.File.Offset,
                raw_storage.data(),
                req.Source.File.Size,
                bytes_read);
            if (FAILED(hr)) return hr;
            src_bytes = raw_storage.data();
        } else {
            if (!req.Source.Memory.Source && req.Source.Memory.Size > 0) return E_POINTER;
            src_bytes = static_cast<const uint8_t*>(req.Source.Memory.Source);
            src_size = req.Source.Memory.Size;
        }

        std::vector<uint8_t> inflated;
        const uint8_t* payload = src_bytes;
        size_t payload_size = src_size;

        const auto fmt = static_cast<DSTORAGE_COMPRESSION_FORMAT>(req.Options.CompressionFormat);
        if (fmt == DSTORAGE_COMPRESSION_FORMAT_ZLIB) {
            const size_t max_out = req.UncompressedSize > 0
                ? static_cast<size_t>(req.UncompressedSize) + 64u
                : (64u * 1024u * 1024u);
            if (!wsx_inflate::inflate_zlib(src_bytes, src_size, inflated, max_out)) {
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }
            payload = inflated.data();
            payload_size = inflated.size();
        } else if (fmt == DSTORAGE_COMPRESSION_FORMAT_GDEFLATE) {
            const size_t max_out = req.UncompressedSize > 0
                ? static_cast<size_t>(req.UncompressedSize) + 64u
                : (256u * 1024u * 1024u);
            bool gpu_disabled = false;
            {
                std::lock_guard<std::mutex> clk(g_config_mutex);
                gpu_disabled = (g_config.DisableGpuDecompression != FALSE);
            }
            if (device_ && !gpu_disabled && req.UncompressedSize > 0u) {
                inflated.resize(req.UncompressedSize);
                uint32_t out_sz = 0u;
                const HRESULT ghr = gpu_gdeflate_.decompress_to_memory(
                    device_.Get(), src_bytes, src_size, inflated.data(), inflated.size(), &out_sz, nullptr);
                if (FAILED(ghr)) {
                    return ghr;
                }
                inflated.resize(out_sz);
            } else {
                if (!wsx_gdeflate::decompress(src_bytes, src_size, inflated, max_out)) {
                    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                }
            }
            payload = inflated.data();
            payload_size = inflated.size();
        } else if (fmt == DSTORAGE_COMPRESSION_FORMAT_RAW_DEFLATE) {
            const size_t max_out = req.UncompressedSize > 0
                ? static_cast<size_t>(req.UncompressedSize) + 64u
                : (64u * 1024u * 1024u);
            if (!wsx_inflate::inflate_raw(src_bytes, src_size, inflated, max_out)) {
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }
            payload = inflated.data();
            payload_size = inflated.size();
        } else if (fmt == DSTORAGE_COMPRESSION_FORMAT_BCPACK) {
            const size_t max_out = req.UncompressedSize > 0
                ? static_cast<size_t>(req.UncompressedSize) + 64u
                : (256u * 1024u * 1024u);
            DXGI_FORMAT bc_fmt = DXGI_FORMAT_UNKNOWN;
            if (req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION &&
                req.Destination.Texture.Resource) {
                bc_fmt = req.Destination.Texture.Resource->GetDesc().Format;
            } else if (req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES &&
                       req.Destination.MultipleSubresources.Resource) {
                bc_fmt = req.Destination.MultipleSubresources.Resource->GetDesc().Format;
            }
            bool gpu_disabled = false;
            {
                std::lock_guard<std::mutex> clk(g_config_mutex);
                gpu_disabled = (g_config.DisableGpuDecompression != FALSE);
            }
            bool bc_ok = false;
            if (device_ && !gpu_disabled && (req.UncompressedSize >= 16384u || src_size >= 8192u)) {
                bc_ok = SUCCEEDED(gpu_bcpack_.decompress_to_memory(
                    device_.Get(), src_bytes, src_size, bc_fmt, inflated, max_out, nullptr));
            }
            if (!bc_ok) {
                bc_ok = wsx_bcpack::decompress(src_bytes, src_size, bc_fmt, inflated, max_out);
            }
            if (!bc_ok) {
                // Optional safe neutral mip-tail fallback for truncated/unsupported BCn texture streams
                if (req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION &&
                    req.Destination.Texture.Resource &&
                    wsx_bcpack::classify_dxgi_bc_format(bc_fmt) != wsx_bcpack::BcKind::Unknown) {
                    const D3D12_RESOURCE_DESC rdesc = req.Destination.Texture.Resource->GetDesc();
                    const UINT mip = req.Destination.Texture.SubresourceIndex % (rdesc.MipLevels ? rdesc.MipLevels : 1u);
                    const uint32_t mw = std::max<uint32_t>(1u, static_cast<uint32_t>(rdesc.Width >> mip));
                    const uint32_t mh = std::max<uint32_t>(1u, static_cast<uint32_t>(rdesc.Height >> mip));
                    if (!wsx_bcpack::synthesize_resident_mip_fallback(bc_fmt, mw, mh, nullptr, 0u, 0u, inflated)) {
                        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                    }
                } else {
                    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                }
            }
            payload = inflated.data();
            payload_size = inflated.size();
        } else if (fmt != DSTORAGE_COMPRESSION_FORMAT_NONE) {
            return E_NOTIMPL;
        }

        if (req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_MEMORY) {
            if (!req.Destination.Memory.Buffer && payload_size > 0) return E_POINTER;
            if (payload_size > req.Destination.Memory.Size) {
                return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            }
            if (payload_size > 0) {
                std::memcpy(req.Destination.Memory.Buffer, payload, payload_size);
            }
            return S_OK;
        }

        if (req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_BUFFER) {
            if (!req.Destination.Buffer.Resource) return E_INVALIDARG;
            if (payload_size > req.Destination.Buffer.Size) {
                return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            }
            void* mapped = nullptr;
            D3D12_RANGE read_range{0, 0};
            const HRESULT hr = req.Destination.Buffer.Resource->Map(0, &read_range, &mapped);
            if (SUCCEEDED(hr)) {
                if (payload_size > 0) {
                    std::memcpy(static_cast<uint8_t*>(mapped) + req.Destination.Buffer.Offset,
                                payload, payload_size);
                }
                D3D12_RANGE written{
                    static_cast<SIZE_T>(req.Destination.Buffer.Offset),
                    static_cast<SIZE_T>(req.Destination.Buffer.Offset + payload_size)};
                req.Destination.Buffer.Resource->Unmap(0, &written);
                return S_OK;
            }
            // DEFAULT heap D3D12 buffer fallback via COPY queue staging upload
            return upload_to_default_buffer(
                req.Destination.Buffer.Resource, req.Destination.Buffer.Offset, payload, payload_size);
        }

        if (req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION) {
            return upload_to_texture_region(req.Destination.Texture, payload, payload_size);
        }

        if (req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES) {
            return upload_to_multiple_subresources(req.Destination.MultipleSubresources, payload, payload_size);
        }

        return E_NOTIMPL;
    }

    void worker_loop() {
        for (;;) {
            QueuedCommand cmd{};
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [this] { return closing_ || !submitted_.empty(); });
                if (submitted_.empty() && closing_) break;
                cmd = std::move(submitted_.front());
                submitted_.pop_front();
            }

            if (cmd.type == DSTORAGE_COMMAND_TYPE_REQUEST) {
                const HRESULT hr = execute_request(cmd);
                if (FAILED(hr)) {
                    std::lock_guard<std::mutex> lk(mutex_);
                    if (SUCCEEDED(batch_error_)) {
                        batch_error_ = hr;
                    }
                    record_error_locked(hr, cmd);
                }
            } else if (cmd.type == DSTORAGE_COMMAND_TYPE_STATUS) {
                HRESULT status_hr = S_OK;
                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    status_hr = batch_error_;
                    batch_error_ = S_OK;
                }
                if (cmd.status_array) {
                    ComPtr<IWsxDStorageStatusInternal> internal;
                    if (SUCCEEDED(cmd.status_array.As(&internal)) && internal) {
                        internal->mark_complete(cmd.status_index, status_hr);
                    }
                }
            } else if (cmd.type == DSTORAGE_COMMAND_TYPE_SIGNAL) {
                if (cmd.fence) {
                    const HRESULT hr = cmd.fence->Signal(cmd.fence_value);
                    if (FAILED(hr)) {
                        std::lock_guard<std::mutex> lk(mutex_);
                        record_error_locked(hr, cmd);
                    }
                }
            } else if (cmd.type == DSTORAGE_COMMAND_TYPE_EVENT) {
                if (cmd.event_handle) {
                    SetEvent(cmd.event_handle);
                }
            }
        }
    }

public:
    explicit WsxDStorageQueue(const DSTORAGE_QUEUE_DESC& desc)
        : desc_(desc),
          name_(desc.Name ? desc.Name : ""),
          device_(desc.Device) {
        if (desc_.Capacity < DSTORAGE_MIN_QUEUE_CAPACITY) {
            desc_.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
        } else if (desc_.Capacity > DSTORAGE_MAX_QUEUE_CAPACITY) {
            desc_.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
        }
        desc_.Name = name_.empty() ? nullptr : name_.c_str();
        error_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        worker_started_ = true;
        worker_ = std::thread([this] { worker_loop(); });
    }

    ~WsxDStorageQueue() {
        Close();
        if (error_event_) {
            CloseHandle(error_event_);
            error_event_ = nullptr;
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDStorageQueueX)) {
            *ppv = static_cast<IDStorageQueueX*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    void STDMETHODCALLTYPE EnqueueRequest(const DSTORAGE_REQUEST* request) override {
        if (!request) return;
        QueuedCommand cmd{};
        cmd.type = DSTORAGE_COMMAND_TYPE_REQUEST;
        cmd.request = *request;
        if (request->Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE && request->Source.File.Source) {
            cmd.file_ref = request->Source.File.Source;
        }
        if (request->Name) {
            cmd.request_name = request->Name;
            cmd.request.Name = cmd.request_name.c_str();
        }
        std::lock_guard<std::mutex> lk(mutex_);
        if (closing_) return;
        staged_.push_back(std::move(cmd));
    }

    void STDMETHODCALLTYPE EnqueueStatus(IDStorageStatusArrayX* statusArray, UINT32 index) override {
        if (!statusArray) return;
        ComPtr<IWsxDStorageStatusInternal> internal;
        if (SUCCEEDED(statusArray->QueryInterface(IID_PPV_ARGS(&internal))) && internal) {
            internal->mark_pending(index);
        }
        QueuedCommand cmd{};
        cmd.type = DSTORAGE_COMMAND_TYPE_STATUS;
        cmd.status_array = statusArray;
        cmd.status_index = index;
        std::lock_guard<std::mutex> lk(mutex_);
        if (closing_) return;
        staged_.push_back(std::move(cmd));
    }

    void STDMETHODCALLTYPE EnqueueSignal(ID3D12Fence* fence, UINT64 value) override {
        if (!fence) return;
        QueuedCommand cmd{};
        cmd.type = DSTORAGE_COMMAND_TYPE_SIGNAL;
        cmd.fence = fence;
        cmd.fence_value = value;
        std::lock_guard<std::mutex> lk(mutex_);
        if (closing_) return;
        staged_.push_back(std::move(cmd));
    }

    void STDMETHODCALLTYPE EnqueueSetEvent(HANDLE handle) override {
        if (!handle) return;
        QueuedCommand cmd{};
        cmd.type = DSTORAGE_COMMAND_TYPE_EVENT;
        cmd.event_handle = handle;
        std::lock_guard<std::mutex> lk(mutex_);
        if (closing_) return;
        staged_.push_back(std::move(cmd));
    }

    void STDMETHODCALLTYPE Submit() override {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (closing_ || staged_.empty()) return;
            while (!staged_.empty()) {
                submitted_.push_back(std::move(staged_.front()));
                staged_.pop_front();
            }
        }
        cv_.notify_one();
    }

    void STDMETHODCALLTYPE CancelRequestsWithTag(UINT64 mask, UINT64 value) override {
        std::lock_guard<std::mutex> lk(mutex_);
        auto cancel_in = [mask, value](std::deque<QueuedCommand>& q) {
            for (auto& cmd : q) {
                if (cmd.type == DSTORAGE_COMMAND_TYPE_REQUEST &&
                    (cmd.request.CancellationTag & mask) == (value & mask)) {
                    cmd.cancelled = true;
                }
            }
        };
        cancel_in(staged_);
        cancel_in(submitted_);
    }

    void STDMETHODCALLTYPE Close() override {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            closing_ = true;
            staged_.clear();
        }
        cv_.notify_all();
        if (worker_started_ && worker_.joinable()) {
            worker_.join();
            worker_started_ = false;
        }
        copy_list_.Reset();
        copy_alloc_.Reset();
        copy_queue_.Reset();
        copy_fence_.Reset();
        copy_device_.Reset();
        if (copy_fence_event_) {
            CloseHandle(copy_fence_event_);
            copy_fence_event_ = nullptr;
        }
    }

    HANDLE STDMETHODCALLTYPE GetErrorEvent() override {
        return error_event_;
    }

    void STDMETHODCALLTYPE RetrieveErrorRecord(DSTORAGE_ERROR_RECORD* record) override {
        if (!record) return;
        std::lock_guard<std::mutex> lk(mutex_);
        *record = error_record_;
        error_record_ = {};
        if (error_event_) {
            ResetEvent(error_event_);
        }
    }

    void STDMETHODCALLTYPE Query(DSTORAGE_QUEUE_INFO* info) override {
        if (!info) return;
        std::lock_guard<std::mutex> lk(mutex_);
        info->Desc = desc_;
        const size_t used = staged_.size() + submitted_.size();
        info->EmptySlotCount = (used >= desc_.Capacity)
            ? 0
            : static_cast<UINT16>(desc_.Capacity - used);
        const UINT16 half = static_cast<UINT16>(desc_.Capacity / 2u);
        info->RequestCountUntilAutoSubmit = (staged_.size() >= half)
            ? 0
            : static_cast<UINT16>(half - staged_.size());
    }
};

class WsxDStorageFactory final : public IDStorageFactoryX {
    std::atomic<ULONG> ref_{1};
    std::atomic<UINT32> debug_flags_{0};
    std::atomic<UINT32> staging_size_{32u * 1024u * 1024u};

public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDStorageFactoryX)) {
            *ppv = static_cast<IDStorageFactoryX*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE CreateQueue(const DSTORAGE_QUEUE_DESC* desc, REFIID riid, void** ppv) override {
        if (!desc || !ppv) return E_POINTER;
        *ppv = nullptr;
        auto* q = new (std::nothrow) WsxDStorageQueue(*desc);
        if (!q) return E_OUTOFMEMORY;
        const HRESULT hr = q->QueryInterface(riid, ppv);
        q->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE OpenFile(const WCHAR* path, REFIID riid, void** ppv) override {
        if (!path || !ppv) return E_POINTER;
        *ppv = nullptr;
        HANDLE h = CreateFileW(
            path,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        auto* f = new (std::nothrow) WsxDStorageFile(h, path, true);
        if (!f) {
            CloseHandle(h);
            return E_OUTOFMEMORY;
        }
        const HRESULT hr = f->QueryInterface(riid, ppv);
        f->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateStatusArray(UINT32 capacity, const CHAR* name, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (capacity == 0) return E_INVALIDARG;
        auto* arr = new (std::nothrow) WsxDStorageStatusArray(capacity, name);
        if (!arr) return E_OUTOFMEMORY;
        const HRESULT hr = arr->QueryInterface(riid, ppv);
        arr->Release();
        return hr;
    }

    void STDMETHODCALLTYPE SetDebugFlags(UINT32 flags) override {
        debug_flags_.store(flags, std::memory_order_relaxed);
    }

    HRESULT STDMETHODCALLTYPE SetStagingBufferSize(UINT32 size) override {
        if (size < 64u * 1024u) return E_INVALIDARG;
        staging_size_.store(size, std::memory_order_relaxed);
        return S_OK;
    }
};

} // namespace

extern "C" {

HRESULT WINAPI DStorageGetFactory(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    auto* factory = new (std::nothrow) WsxDStorageFactory();
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

HRESULT WINAPI DStorageCreateFactory(REFIID riid, void** ppv) {
    return DStorageGetFactory(riid, ppv);
}

HRESULT WINAPI DStorageSetConfiguration(const DSTORAGE_CONFIGURATION* configuration) {
    std::lock_guard<std::mutex> lk(g_config_mutex);
    if (!configuration) {
        g_config = {};
        return S_OK;
    }
    g_config.NumSubmitThreads = configuration->NumSubmitThreads;
    g_config.NumBuiltInCpuDecompressionThreads = configuration->NumBuiltInCpuDecompressionThreads;
    g_config.ForceMappingLayer = configuration->ForceMappingLayer;
    g_config.DisableBypassIO = configuration->DisableBypassIO;
    g_config.DisableTelemetry = configuration->DisableTelemetry;
    g_config.DisableGpuDecompressionMetacommand = configuration->DisableGpuDecompressionMetacommand;
    g_config.DisableGpuDecompression = configuration->DisableGpuDecompression;
    g_config.ForceFileBuffering = FALSE;
    return S_OK;
}

HRESULT WINAPI DStorageSetConfiguration1(const DSTORAGE_CONFIGURATION1* configuration) {
    std::lock_guard<std::mutex> lk(g_config_mutex);
    if (!configuration) {
        g_config = {};
        return S_OK;
    }
    g_config = *configuration;
    return S_OK;
}

HRESULT WINAPI DStorageOpenXvdVolume(const WCHAR* xvdPath, REFIID riid, void** ppv) {
    if (!xvdPath || !ppv) return E_POINTER;
    *ppv = nullptr;
    auto* xvd = new (std::nothrow) WsxDStorageXvdFile();
    if (!xvd) return E_OUTOFMEMORY;
    HRESULT hr = xvd->open_volume(xvdPath);
    if (FAILED(hr)) {
        xvd->Release();
        return hr;
    }
    hr = xvd->QueryInterface(riid, ppv);
    xvd->Release();
    return hr;
}

HRESULT WINAPI DStorageDecompressGDeflate(
    const void* src,
    UINT32 srcSize,
    void* dst,
    UINT32 dstSize,
    UINT32* outDecompressedSize) {
    if (outDecompressedSize) *outDecompressedSize = 0u;
    if (!src || srcSize == 0u) return E_INVALIDARG;
    if (!dst && dstSize > 0u) return E_POINTER;

    std::vector<uint8_t> inflated;
    if (!wsx_gdeflate::decompress(
            static_cast<const uint8_t*>(src),
            static_cast<size_t>(srcSize),
            inflated,
            dstSize > 0u ? static_cast<size_t>(dstSize) : (256u * 1024u * 1024u))) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    if (outDecompressedSize) {
        *outDecompressedSize = static_cast<UINT32>(inflated.size());
    }
    if (inflated.size() > dstSize) {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    if (!inflated.empty()) {
        std::memcpy(dst, inflated.data(), inflated.size());
    }
    return S_OK;
}

HRESULT WINAPI DStorageDecompressGDeflateGpu(
    ID3D12Device* device,
    const void* src,
    UINT32 srcSize,
    void* dst,
    UINT32 dstSize,
    UINT32* outDecompressedSize,
    BOOL* outUsedGpu) {
    if (outDecompressedSize) *outDecompressedSize = 0u;
    if (outUsedGpu) *outUsedGpu = FALSE;
    if (!src || srcSize == 0u) return E_INVALIDARG;
    if (!dst && dstSize > 0u) return E_POINTER;

    wsx_gdeflate_gpu::WsxGDeflateGpuDecoder decoder;
    uint32_t out_sz = 0u;
    bool used_gpu = false;
    const HRESULT hr = decoder.decompress_to_memory(
        device,
        static_cast<const uint8_t*>(src),
        static_cast<size_t>(srcSize),
        dst,
        static_cast<size_t>(dstSize),
        &out_sz,
        &used_gpu);
    if (outDecompressedSize) *outDecompressedSize = out_sz;
    if (outUsedGpu) *outUsedGpu = used_gpu ? TRUE : FALSE;
    return hr;
}

HRESULT WINAPI DStorageCompressBCPack(
    const void* src,
    UINT32 srcSize,
    DXGI_FORMAT bcFormat,
    void* dst,
    UINT32 dstSize,
    UINT32* outCompressedSize) {
    if (outCompressedSize) *outCompressedSize = 0u;
    if (!src || srcSize == 0u) return E_INVALIDARG;
    if (!dst && dstSize > 0u) return E_POINTER;

    std::vector<uint8_t> packed;
    if (!wsx_bcpack::compress(static_cast<const uint8_t*>(src), static_cast<size_t>(srcSize), bcFormat, packed)) {
        return E_INVALIDARG;
    }
    if (outCompressedSize) *outCompressedSize = static_cast<UINT32>(packed.size());
    if (!dst) return S_OK;
    if (packed.size() > dstSize) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    if (!packed.empty()) std::memcpy(dst, packed.data(), packed.size());
    return S_OK;
}

HRESULT WINAPI DStorageDecompressBCPack(
    const void* src,
    UINT32 srcSize,
    DXGI_FORMAT bcFormat,
    void* dst,
    UINT32 dstSize,
    UINT32* outDecompressedSize,
    const void* optionalResidentMip,
    UINT32 residentMipWidth,
    UINT32 residentMipHeight,
    UINT32 targetWidth,
    UINT32 targetHeight,
    BOOL* outUsedMipFallback) {
    if (outDecompressedSize) *outDecompressedSize = 0u;
    if (outUsedMipFallback) *outUsedMipFallback = FALSE;
    if (!dst && dstSize > 0u) return E_POINTER;

    std::vector<uint8_t> out_blocks;
    bool ok = false;
    if (src && srcSize > 0u) {
        ok = wsx_bcpack::decompress(
            static_cast<const uint8_t*>(src),
            static_cast<size_t>(srcSize),
            bcFormat,
            out_blocks,
            dstSize > 0u ? static_cast<size_t>(dstSize) : (256u * 1024u * 1024u));
    }
    if (!ok) {
        if (targetWidth > 0u && targetHeight > 0u &&
            wsx_bcpack::synthesize_resident_mip_fallback(
                bcFormat, targetWidth, targetHeight,
                static_cast<const uint8_t*>(optionalResidentMip),
                residentMipWidth, residentMipHeight,
                out_blocks)) {
            if (outUsedMipFallback) *outUsedMipFallback = TRUE;
        } else {
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
    }
    if (outDecompressedSize) *outDecompressedSize = static_cast<UINT32>(out_blocks.size());
    if (!dst) return S_OK;
    if (out_blocks.size() > dstSize) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    if (!out_blocks.empty()) std::memcpy(dst, out_blocks.data(), out_blocks.size());
    return S_OK;
}

HRESULT WINAPI DStorageDecompressBCPackGpu(
    ID3D12Device* device,
    const void* src,
    UINT32 srcSize,
    DXGI_FORMAT bcFormat,
    void* dst,
    UINT32 dstSize,
    UINT32* outDecompressedSize,
    BOOL* outUsedGpu) {
    if (outDecompressedSize) *outDecompressedSize = 0u;
    if (outUsedGpu) *outUsedGpu = FALSE;
    if (!src || srcSize == 0u) return E_INVALIDARG;
    if (!dst && dstSize > 0u) return E_POINTER;

    const uint64_t before_disp = wsx_bcpack_gpu::get_global_stats().dispatches;
    static wsx_bcpack_gpu::WsxBcPackGpuDecoder g_bcpack_gpu_decoder;
    std::vector<uint8_t> out_blocks;
    const HRESULT hr = g_bcpack_gpu_decoder.decompress_to_memory(
        device,
        static_cast<const uint8_t*>(src),
        static_cast<size_t>(srcSize),
        bcFormat,
        out_blocks,
        dstSize > 0u ? static_cast<size_t>(dstSize) : (256u * 1024u * 1024u),
        nullptr);
    if (FAILED(hr)) return hr;
    const uint64_t after_disp = wsx_bcpack_gpu::get_global_stats().dispatches;
    if (outUsedGpu) *outUsedGpu = (after_disp > before_disp) ? TRUE : FALSE;
    if (outDecompressedSize) *outDecompressedSize = static_cast<UINT32>(out_blocks.size());
    if (!dst) return S_OK;
    if (out_blocks.size() > dstSize) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    if (!out_blocks.empty()) std::memcpy(dst, out_blocks.data(), out_blocks.size());
    return S_OK;
}

} // extern "C"
