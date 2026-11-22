// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Persistent Native Direct3D 12 Driver PSO Binary Cache (ID3D12PipelineLibrary1):
// - Eliminates cold-start GPU driver shader compilation stutters (DXIL -> native SASS/ISA)
//   across Compute, Graphics, and Stream/Mesh PSOs.
// - Activated automatically when WSX_PSO_CACHE_DIR or WSX_SHADER_CACHE_DIR (or wsx.ini
//   [Graphics] pso_cache_dir / shader_cache_dir) is configured.
// - Keys are process-restart invariant: Root Signatures are hashed by serialized blob bytes
//   via ID3D12Object::SetPrivateData(kWsxRootSigHashGuid), never by raw pointer address.
// - Keeps the loaded library blob immutable for the lifetime of ID3D12PipelineLibrary per D3D12 spec,
//   and gracefully resets on driver/adapter version mismatch.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "wsx_config.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace wsx_pso_cache {

using Microsoft::WRL::ComPtr;

// Must stay layout-identical to D3D12XBOX_PSO_CACHE_STATS in include/xbox/d3d12_xs.h
struct PsoCacheStats {
    uint64_t hits;
    uint64_t misses;
    uint64_t stores;
    uint64_t flushes;
    uint64_t library_bytes;
};

struct GlobalPsoCacheCounters {
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> misses{0};
    std::atomic<uint64_t> stores{0};
    std::atomic<uint64_t> flushes{0};
    std::atomic<uint64_t> last_library_bytes{0};
};

inline GlobalPsoCacheCounters& global_counters() noexcept {
    static GlobalPsoCacheCounters c;
    return c;
}

// {6A81F4C2-9B31-4E7D-A155-2E7789C04419}
inline constexpr GUID kWsxRootSigHashGuid = {
    0x6A81F4C2u, 0x9B31u, 0x4E7Du, {0xA1u, 0x55u, 0x2Eu, 0x77u, 0x89u, 0xC0u, 0x44u, 0x19u}
};

struct Fnv1a64 {
    uint64_t h = 14695981039346656037ULL;

    void mix(const void* data, size_t bytes) noexcept {
        if (!data || bytes == 0u) return;
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i) {
            h ^= static_cast<uint64_t>(p[i]);
            h *= 1099511628211ULL;
        }
    }

    template <typename T>
    void mix_pod(const T& v) noexcept {
        mix(&v, sizeof(T));
    }

    void mix_str(const char* s) noexcept {
        if (!s) {
            const uint8_t z = 0u;
            mix_pod(z);
            return;
        }
        mix(s, std::strlen(s) + 1u);
    }

    void mix_shader(const D3D12_SHADER_BYTECODE& bc) noexcept {
        const uint64_t sz = static_cast<uint64_t>(bc.BytecodeLength);
        mix_pod(sz);
        if (bc.pShaderBytecode && bc.BytecodeLength > 0u) {
            mix(bc.pShaderBytecode, bc.BytecodeLength);
        }
    }
};

inline void attach_root_signature_hash(
    ID3D12RootSignature* rs,
    const void* blob,
    size_t blob_len) noexcept {
    if (!rs || !blob || blob_len == 0u) return;
    Fnv1a64 fnv;
    fnv.mix(blob, blob_len);
    uint64_t hash = fnv.h;
    if (hash == 0u) hash = 1u;
    (void)rs->SetPrivateData(kWsxRootSigHashGuid, sizeof(hash), &hash);
}

inline uint64_t get_root_signature_hash(ID3D12RootSignature* rs) noexcept {
    if (!rs) return 0u;
    uint64_t hash = 0u;
    UINT sz = sizeof(hash);
    if (SUCCEEDED(rs->GetPrivateData(kWsxRootSigHashGuid, &sz, &hash)) && sz == sizeof(hash)) {
        return hash;
    }
    return 0u;
}

inline std::wstring format_key(wchar_t prefix, uint64_t hash) {
    wchar_t buf[32]{};
    std::swprintf(buf, 32, L"%lc_%016llX", prefix, static_cast<unsigned long long>(hash));
    return std::wstring(buf);
}

inline std::string resolve_cache_dir() {
    std::string dir = wsx::get_config_string("WSX_PSO_CACHE_DIR", "Graphics", "pso_cache_dir", "");
    if (dir.empty()) {
        dir = wsx::get_config_string("WSX_SHADER_CACHE_DIR", "Graphics", "shader_cache_dir", "");
    }
    return dir;
}

class PsoLibraryCache {
    std::mutex mtx_;
    std::filesystem::path cache_path_;
    std::vector<uint8_t> loaded_blob_; // Must remain alive & immutable while lib_ is alive!
    ComPtr<ID3D12PipelineLibrary> lib_;
    ComPtr<ID3D12PipelineLibrary1> lib1_;
    bool init_attempted_ = false;
    bool dirty_ = false;

    static size_t align_up(size_t v, size_t a) noexcept {
        return (v + (a - 1u)) & ~(a - 1u);
    }

    void ensure_initialized_locked(ID3D12Device1* dev1, IDXGIAdapter1* adapter) noexcept {
        const std::string dir = resolve_cache_dir();
        if (dir.empty()) {
            // If env var was cleared after init, keep current state; if never initialized, stay inactive
            return;
        }
        // Compute target file path for this adapter
        UINT vendor = 0u, device_id = 0u;
        if (adapter) {
            DXGI_ADAPTER_DESC1 ad{};
            if (SUCCEEDED(adapter->GetDesc1(&ad))) {
                vendor = ad.VendorId;
                device_id = ad.DeviceId;
            }
        }
        wchar_t fname[96]{};
        std::swprintf(fname, 96, L"wsx_pso_library_%04X_%04X.bin", vendor, device_id);
        const std::filesystem::path target_path = std::filesystem::path(dir) / fname;

        // Re-initialize if cache directory changed dynamically (e.g., tests switching WSX_PSO_CACHE_DIR)
        if (init_attempted_ && cache_path_ == target_path) {
            return;
        }
        if (init_attempted_ && cache_path_ != target_path) {
            flush_locked();
            lib1_.Reset();
            lib_.Reset();
            loaded_blob_.clear();
            init_attempted_ = false;
            dirty_ = false;
        }

        init_attempted_ = true;
        cache_path_ = target_path;
        if (!dev1) return;

        // Try loading persisted binary library blob from disk
        std::error_code ec;
        if (std::filesystem::is_regular_file(cache_path_, ec)) {
            std::ifstream f(cache_path_, std::ios::binary | std::ios::ate);
            if (f) {
                const std::streamoff sz = f.tellg();
                if (sz > 0 && sz <= static_cast<std::streamoff>(256u * 1024u * 1024u)) {
                    loaded_blob_.resize(static_cast<size_t>(sz));
                    f.seekg(0, std::ios::beg);
                    f.read(reinterpret_cast<char*>(loaded_blob_.data()), sz);
                    if (!f.good()) loaded_blob_.clear();
                }
            }
        }

        if (!loaded_blob_.empty()) {
            HRESULT hr = dev1->CreatePipelineLibrary(
                loaded_blob_.data(), loaded_blob_.size(), IID_PPV_ARGS(lib_.GetAddressOf()));
            if (FAILED(hr)) {
                // Driver version / adapter mismatch or stale cache -> cleanly recreate empty library
                loaded_blob_.clear();
                lib_.Reset();
            }
        }
        if (!lib_) {
            (void)dev1->CreatePipelineLibrary(nullptr, 0u, IID_PPV_ARGS(lib_.GetAddressOf()));
        }
        if (lib_) {
            (void)lib_.As(&lib1_);
            global_counters().last_library_bytes.store(
                static_cast<uint64_t>(lib_->GetSerializedSize()), std::memory_order_relaxed);
        }
    }

    HRESULT flush_locked() noexcept {
        if (!lib_ || cache_path_.empty() || !dirty_) return S_OK;
        try {
            const SIZE_T sz = lib_->GetSerializedSize();
            if (sz == 0u) return S_OK;
            std::vector<uint8_t> out(sz);
            const HRESULT hr = lib_->Serialize(out.data(), out.size());
            if (FAILED(hr)) return hr;

            std::error_code ec;
            std::filesystem::create_directories(cache_path_.parent_path(), ec);

            wchar_t tmp_suffix[64]{};
            std::swprintf(tmp_suffix, 64, L".%lu.%lu.tmp",
                          static_cast<unsigned long>(GetCurrentProcessId()),
                          static_cast<unsigned long>(GetCurrentThreadId()));
            std::filesystem::path tmp = cache_path_;
            tmp += tmp_suffix;

            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f) return E_FAIL;
                f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
                f.flush();
                if (!f.good()) {
                    f.close();
                    std::filesystem::remove(tmp, ec);
                    return E_FAIL;
                }
            }
            if (!MoveFileExW(tmp.c_str(), cache_path_.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                std::filesystem::remove(tmp, ec);
                return HRESULT_FROM_WIN32(GetLastError());
            }
            dirty_ = false;
            global_counters().flushes.fetch_add(1u, std::memory_order_relaxed);
            global_counters().last_library_bytes.store(static_cast<uint64_t>(sz), std::memory_order_relaxed);
            wsx::trace_hr("d3d12_xs", "PsoCache::Flush", S_OK, "bytes=%zu", static_cast<size_t>(sz));
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }

    static bool hash_compute_desc(const D3D12_COMPUTE_PIPELINE_STATE_DESC& d, uint64_t& out_hash) noexcept {
        const uint64_t rs_h = get_root_signature_hash(d.pRootSignature);
        if (d.pRootSignature && rs_h == 0u) return false;
        Fnv1a64 fnv;
        fnv.mix_pod(rs_h);
        fnv.mix_shader(d.CS);
        fnv.mix_pod(d.NodeMask);
        fnv.mix_pod(d.Flags);
        out_hash = fnv.h;
        return true;
    }

    static bool hash_graphics_desc(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, uint64_t& out_hash) noexcept {
        const uint64_t rs_h = get_root_signature_hash(d.pRootSignature);
        if (d.pRootSignature && rs_h == 0u) return false;
        Fnv1a64 fnv;
        fnv.mix_pod(rs_h);
        fnv.mix_shader(d.VS);
        fnv.mix_shader(d.PS);
        fnv.mix_shader(d.DS);
        fnv.mix_shader(d.HS);
        fnv.mix_shader(d.GS);
        fnv.mix_pod(d.BlendState);
        fnv.mix_pod(d.SampleMask);
        fnv.mix_pod(d.RasterizerState);
        fnv.mix_pod(d.DepthStencilState);
        fnv.mix_pod(d.InputLayout.NumElements);
        for (UINT i = 0; i < d.InputLayout.NumElements && d.InputLayout.pInputElementDescs; ++i) {
            const auto& e = d.InputLayout.pInputElementDescs[i];
            fnv.mix_str(e.SemanticName);
            fnv.mix_pod(e.SemanticIndex);
            fnv.mix_pod(e.Format);
            fnv.mix_pod(e.InputSlot);
            fnv.mix_pod(e.AlignedByteOffset);
            fnv.mix_pod(e.InputSlotClass);
            fnv.mix_pod(e.InstanceDataStepRate);
        }
        fnv.mix_pod(d.IBStripCutValue);
        fnv.mix_pod(d.PrimitiveTopologyType);
        fnv.mix_pod(d.NumRenderTargets);
        fnv.mix_pod(d.RTVFormats);
        fnv.mix_pod(d.DSVFormat);
        fnv.mix_pod(d.SampleDesc);
        fnv.mix_pod(d.NodeMask);
        fnv.mix_pod(d.Flags);
        out_hash = fnv.h;
        return true;
    }

    static bool hash_stream_desc(const D3D12_PIPELINE_STATE_STREAM_DESC& d, uint64_t& out_hash) noexcept {
        if (!d.pPipelineStateSubobjectStream || d.SizeInBytes < sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)) {
            return false;
        }
        const auto* base = static_cast<const uint8_t*>(d.pPipelineStateSubobjectStream);
        size_t off = 0u;
        Fnv1a64 fnv;
        while (off + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= d.SizeInBytes) {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE st{};
            std::memcpy(&st, base + off, sizeof(st));
            fnv.mix_pod(st);
            size_t entry_sz = 0u;
            switch (st) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: {
                struct S { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; ID3D12RootSignature* v; };
                entry_sz = sizeof(S);
                if (off + entry_sz > d.SizeInBytes) return false;
                const auto* s = reinterpret_cast<const S*>(base + off);
                const uint64_t rs_h = get_root_signature_hash(s->v);
                if (s->v && rs_h == 0u) return false;
                fnv.mix_pod(rs_h);
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: {
                struct S { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_SHADER_BYTECODE v; };
                entry_sz = sizeof(S);
                if (off + entry_sz > d.SizeInBytes) return false;
                const auto* s = reinterpret_cast<const S*>(base + off);
                fnv.mix_shader(s->v);
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: {
                struct S { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_INPUT_LAYOUT_DESC v; };
                entry_sz = sizeof(S);
                if (off + entry_sz > d.SizeInBytes) return false;
                const auto* s = reinterpret_cast<const S*>(base + off);
                fnv.mix_pod(s->v.NumElements);
                for (UINT i = 0; i < s->v.NumElements && s->v.pInputElementDescs; ++i) {
                    const auto& e = s->v.pInputElementDescs[i];
                    fnv.mix_str(e.SemanticName);
                    fnv.mix_pod(e.SemanticIndex);
                    fnv.mix_pod(e.Format);
                    fnv.mix_pod(e.InputSlot);
                    fnv.mix_pod(e.AlignedByteOffset);
                    fnv.mix_pod(e.InputSlotClass);
                    fnv.mix_pod(e.InstanceDataStepRate);
                }
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
                entry_sz = sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) + sizeof(D3D12_BLEND_DESC); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
                entry_sz = sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) + sizeof(D3D12_DEPTH_STENCIL_DESC); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
                entry_sz = sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) + sizeof(D3D12_DEPTH_STENCIL_DESC1); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
                entry_sz = sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) + sizeof(DXGI_FORMAT); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
                entry_sz = sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) + sizeof(D3D12_RASTERIZER_DESC); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
                entry_sz = sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) + sizeof(D3D12_RT_FORMAT_ARRAY); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
                entry_sz = sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) + sizeof(DXGI_SAMPLE_DESC); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
                entry_sz = sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) + sizeof(UINT); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
                entry_sz = sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) + sizeof(D3D12_PRIMITIVE_TOPOLOGY_TYPE); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
                entry_sz = sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) + sizeof(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
                entry_sz = sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) + sizeof(D3D12_PIPELINE_STATE_FLAGS); break;
            default:
                return false; // Unknown stream subobject -> skip PSO library cache cleanly
            }
            if (off + entry_sz > d.SizeInBytes) return false;
            if (st != D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE &&
                st != D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT &&
                !(st >= D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS && st <= D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS) &&
                st != D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS &&
                st != D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS) {
                fnv.mix(base + off + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE),
                        entry_sz - sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE));
            }
            off = align_up(off + entry_sz, sizeof(void*));
        }
        out_hash = fnv.h;
        return true;
    }

public:
    ~PsoLibraryCache() {
        flush_and_shutdown();
    }

    HRESULT flush() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        return flush_locked();
    }

    void flush_and_shutdown() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        (void)flush_locked();
        lib1_.Reset();
        lib_.Reset();
        loaded_blob_.clear();
        init_attempted_ = false;
        dirty_ = false;
    }

    HRESULT load_or_create_compute(
        ID3D12Device* dev0,
        ID3D12Device1* dev1,
        IDXGIAdapter1* adapter,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc,
        REFIID riid,
        void** ppPso) noexcept {
        uint64_t hash = 0u;
        if (!dev1 || resolve_cache_dir().empty() || !hash_compute_desc(desc, hash)) {
            return dev0->CreateComputePipelineState(&desc, riid, ppPso);
        }
        const std::wstring key = format_key(L'C', hash);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ensure_initialized_locked(dev1, adapter);
            if (lib_) {
                if (SUCCEEDED(lib_->LoadComputePipeline(key.c_str(), &desc, riid, ppPso))) {
                    global_counters().hits.fetch_add(1u, std::memory_order_relaxed);
                    return S_OK;
                }
            }
        }
        global_counters().misses.fetch_add(1u, std::memory_order_relaxed);
        const HRESULT hr = dev0->CreateComputePipelineState(&desc, riid, ppPso);
        if (SUCCEEDED(hr) && ppPso && *ppPso) {
            ComPtr<ID3D12PipelineState> pso;
            if (SUCCEEDED(static_cast<IUnknown*>(*ppPso)->QueryInterface(IID_PPV_ARGS(&pso))) && pso) {
                std::lock_guard<std::mutex> lk(mtx_);
                if (lib_ && SUCCEEDED(lib_->StorePipeline(key.c_str(), pso.Get()))) {
                    dirty_ = true;
                    global_counters().stores.fetch_add(1u, std::memory_order_relaxed);
                    global_counters().last_library_bytes.store(
                        static_cast<uint64_t>(lib_->GetSerializedSize()), std::memory_order_relaxed);
                }
            }
        }
        return hr;
    }

    HRESULT load_or_create_graphics(
        ID3D12Device* dev0,
        ID3D12Device1* dev1,
        IDXGIAdapter1* adapter,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
        REFIID riid,
        void** ppPso) noexcept {
        uint64_t hash = 0u;
        if (!dev1 || resolve_cache_dir().empty() || !hash_graphics_desc(desc, hash)) {
            return dev0->CreateGraphicsPipelineState(&desc, riid, ppPso);
        }
        const std::wstring key = format_key(L'G', hash);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ensure_initialized_locked(dev1, adapter);
            if (lib_) {
                if (SUCCEEDED(lib_->LoadGraphicsPipeline(key.c_str(), &desc, riid, ppPso))) {
                    global_counters().hits.fetch_add(1u, std::memory_order_relaxed);
                    return S_OK;
                }
            }
        }
        global_counters().misses.fetch_add(1u, std::memory_order_relaxed);
        const HRESULT hr = dev0->CreateGraphicsPipelineState(&desc, riid, ppPso);
        if (SUCCEEDED(hr) && ppPso && *ppPso) {
            ComPtr<ID3D12PipelineState> pso;
            if (SUCCEEDED(static_cast<IUnknown*>(*ppPso)->QueryInterface(IID_PPV_ARGS(&pso))) && pso) {
                std::lock_guard<std::mutex> lk(mtx_);
                if (lib_ && SUCCEEDED(lib_->StorePipeline(key.c_str(), pso.Get()))) {
                    dirty_ = true;
                    global_counters().stores.fetch_add(1u, std::memory_order_relaxed);
                    global_counters().last_library_bytes.store(
                        static_cast<uint64_t>(lib_->GetSerializedSize()), std::memory_order_relaxed);
                }
            }
        }
        return hr;
    }

    HRESULT load_or_create_stream(
        ID3D12Device2* dev2,
        ID3D12Device1* dev1,
        IDXGIAdapter1* adapter,
        const D3D12_PIPELINE_STATE_STREAM_DESC& desc,
        REFIID riid,
        void** ppPso) noexcept {
        uint64_t hash = 0u;
        if (!dev1 || resolve_cache_dir().empty() || !hash_stream_desc(desc, hash)) {
            return dev2->CreatePipelineState(&desc, riid, ppPso);
        }
        const std::wstring key = format_key(L'S', hash);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ensure_initialized_locked(dev1, adapter);
            if (lib1_) {
                if (SUCCEEDED(lib1_->LoadPipeline(key.c_str(), &desc, riid, ppPso))) {
                    global_counters().hits.fetch_add(1u, std::memory_order_relaxed);
                    return S_OK;
                }
            }
        }
        global_counters().misses.fetch_add(1u, std::memory_order_relaxed);
        const HRESULT hr = dev2->CreatePipelineState(&desc, riid, ppPso);
        if (SUCCEEDED(hr) && ppPso && *ppPso) {
            ComPtr<ID3D12PipelineState> pso;
            if (SUCCEEDED(static_cast<IUnknown*>(*ppPso)->QueryInterface(IID_PPV_ARGS(&pso))) && pso) {
                std::lock_guard<std::mutex> lk(mtx_);
                if (lib_ && SUCCEEDED(lib_->StorePipeline(key.c_str(), pso.Get()))) {
                    dirty_ = true;
                    global_counters().stores.fetch_add(1u, std::memory_order_relaxed);
                    global_counters().last_library_bytes.store(
                        static_cast<uint64_t>(lib_->GetSerializedSize()), std::memory_order_relaxed);
                }
            }
        }
        return hr;
    }
};

} // namespace wsx_pso_cache
