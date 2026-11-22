// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room DirectXTK12 Keyboard, DescriptorPile, SpriteBatch, SpriteFont, ResourceUploadBatch, CommonStates, FindMedia compatibility headers.

#pragma once

#include "d3d12_xs.h"
#include "GraphicsMemory.h"
#include <DirectXMath.h>
#include <wrl/client.h>

namespace DirectX {

class Keyboard {
public:
    struct State {
        bool Escape = false;
    };
    struct KeyboardStateTracker {
        void Update(const State&) noexcept {}
        void Reset() noexcept {}
    };
    State GetState() const noexcept {
        State st{};
        st.Escape = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        return st;
    }
};

class DescriptorPile {
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    UINT inc_ = 0;
public:
    DescriptorPile(::ID3D12Device* device,
                   D3D12_DESCRIPTOR_HEAP_TYPE type,
                   D3D12_DESCRIPTOR_HEAP_FLAGS flags,
                   size_t capacity,
                   size_t /*reserve*/ = 0) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.NumDescriptors = static_cast<UINT>(capacity);
        desc.Type = type;
        desc.Flags = flags;
        if (device) {
            device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap_.GetAddressOf()));
            inc_ = device->GetDescriptorHandleIncrementSize(type);
        }
    }
    ID3D12DescriptorHeap* Heap() const noexcept { return heap_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle(size_t index) const noexcept {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap_ ? heap_->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{0};
        h.ptr += index * inc_;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(size_t index) const noexcept {
        D3D12_GPU_DESCRIPTOR_HANDLE h = heap_ ? heap_->GetGPUDescriptorHandleForHeapStart() : D3D12_GPU_DESCRIPTOR_HANDLE{0};
        h.ptr += index * inc_;
        return h;
    }
};

struct CommonStates {
    static inline const D3D12_BLEND_DESC AlphaBlend{};
};

struct SpriteBatchPipelineStateDescription {
    explicit SpriteBatchPipelineStateDescription(const RenderTargetState&, const D3D12_BLEND_DESC* = nullptr) noexcept {}
};

class ResourceUploadBatch {
public:
    explicit ResourceUploadBatch(::ID3D12Device*) noexcept {}
    void Begin(D3D12_COMMAND_LIST_TYPE = D3D12_COMMAND_LIST_TYPE_DIRECT) noexcept {}
    void End(::ID3D12CommandQueue*) noexcept {}
};

class SpriteBatch {
public:
    SpriteBatch(::ID3D12Device*, ResourceUploadBatch&, const SpriteBatchPipelineStateDescription&) noexcept {}
    void Begin(ID3D12GraphicsCommandList*) noexcept {}
    void End() noexcept {}
    void SetViewport(const D3D12_VIEWPORT&) noexcept {}
};

class SpriteFont {
public:
    SpriteFont(::ID3D12Device*, ResourceUploadBatch&, const wchar_t*,
               D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) noexcept {}
    void DrawString(SpriteBatch*, const wchar_t*, const XMFLOAT2&, FXMVECTOR) const noexcept {}
    float GetLineSpacing() const noexcept { return 24.0f; }
};

} // namespace DirectX

namespace DX {
inline void FindMediaFile(wchar_t* strDestPath, int cchDest, const wchar_t* strFilename) noexcept {
    if (strDestPath && cchDest > 0 && strFilename) {
        wcsncpy_s(strDestPath, static_cast<size_t>(cchDest), strFilename, _TRUNCATE);
    }
}
} // namespace DX
