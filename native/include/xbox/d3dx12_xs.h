// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room CD3DX12 helper wrappers (<d3dx12_xs.h>).

#pragma once

#include "d3d12_xs.h"
#include <cstring>

struct D3DX12_DEFAULT {};
extern const __declspec(selectany) D3DX12_DEFAULT D3D12_DEFAULT;

inline DXGI_SAMPLE_DESC DefaultSampleDesc() noexcept {
    return DXGI_SAMPLE_DESC{1, 0};
}

struct CD3DX12_RASTERIZER_DESC : D3D12_RASTERIZER_DESC {
    CD3DX12_RASTERIZER_DESC() = default;
    explicit CD3DX12_RASTERIZER_DESC(const D3D12_RASTERIZER_DESC& o) noexcept : D3D12_RASTERIZER_DESC(o) {}
    explicit CD3DX12_RASTERIZER_DESC(D3DX12_DEFAULT) noexcept {
        FillMode = D3D12_FILL_MODE_SOLID;
        CullMode = D3D12_CULL_MODE_BACK;
        FrontCounterClockwise = FALSE;
        DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        DepthClipEnable = TRUE;
        MultisampleEnable = FALSE;
        AntialiasedLineEnable = FALSE;
        ForcedSampleCount = 0;
        ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    }
};

struct CD3DX12_BLEND_DESC : D3D12_BLEND_DESC {
    CD3DX12_BLEND_DESC() = default;
    explicit CD3DX12_BLEND_DESC(const D3D12_BLEND_DESC& o) noexcept : D3D12_BLEND_DESC(o) {}
    explicit CD3DX12_BLEND_DESC(D3DX12_DEFAULT) noexcept {
        AlphaToCoverageEnable = FALSE;
        IndependentBlendEnable = FALSE;
        const D3D12_RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc = {
            FALSE, FALSE,
            D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
            D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
            D3D12_LOGIC_OP_NOOP,
            D3D12_COLOR_WRITE_ENABLE_ALL,
        };
        for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            RenderTarget[i] = defaultRenderTargetBlendDesc;
        }
    }
};

struct CD3DX12_DEPTH_STENCIL_DESC : D3D12_DEPTH_STENCIL_DESC {
    CD3DX12_DEPTH_STENCIL_DESC() = default;
    explicit CD3DX12_DEPTH_STENCIL_DESC(const D3D12_DEPTH_STENCIL_DESC& o) noexcept : D3D12_DEPTH_STENCIL_DESC(o) {}
    explicit CD3DX12_DEPTH_STENCIL_DESC(D3DX12_DEFAULT) noexcept {
        DepthEnable = TRUE;
        DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        StencilEnable = FALSE;
        StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        const D3D12_DEPTH_STENCILOP_DESC defaultStencilOp = {
            D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
            D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS
        };
        FrontFace = defaultStencilOp;
        BackFace = defaultStencilOp;
    }
};

struct CD3DX12_HEAP_PROPERTIES : D3D12_HEAP_PROPERTIES {
    CD3DX12_HEAP_PROPERTIES() = default;
    explicit CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE type, UINT creationNodeMask = 1, UINT nodeMask = 1) noexcept {
        Type = type;
        CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        CreationNodeMask = creationNodeMask;
        VisibleNodeMask = nodeMask;
    }
};

struct CD3DX12_RESOURCE_DESC : D3D12_RESOURCE_DESC {
    CD3DX12_RESOURCE_DESC() = default;
    explicit CD3DX12_RESOURCE_DESC(const D3D12_RESOURCE_DESC& o) noexcept : D3D12_RESOURCE_DESC(o) {}

    static inline CD3DX12_RESOURCE_DESC Buffer(
        UINT64 width,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
        UINT64 alignment = 0) noexcept {
        CD3DX12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Alignment = alignment;
        d.Width = width;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.SampleDesc.Count = 1;
        d.SampleDesc.Quality = 0;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = flags;
        return d;
    }

    static inline CD3DX12_RESOURCE_DESC Tex2D(
        DXGI_FORMAT format,
        UINT64 width,
        UINT height,
        UINT16 arraySize = 1,
        UINT16 mipLevels = 0,
        UINT sampleCount = 1,
        UINT sampleQuality = 0,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
        D3D12_TEXTURE_LAYOUT layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        UINT64 alignment = 0) noexcept {
        CD3DX12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Alignment = alignment;
        d.Width = width;
        d.Height = height;
        d.DepthOrArraySize = arraySize;
        d.MipLevels = mipLevels;
        d.Format = format;
        d.SampleDesc.Count = sampleCount;
        d.SampleDesc.Quality = sampleQuality;
        d.Layout = layout;
        d.Flags = flags;
        return d;
    }
};

struct CD3DX12_RANGE : D3D12_RANGE {
    CD3DX12_RANGE() = default;
    CD3DX12_RANGE(SIZE_T begin, SIZE_T end) noexcept {
        Begin = begin;
        End = end;
    }
};

struct CD3DX12_CPU_DESCRIPTOR_HANDLE : D3D12_CPU_DESCRIPTOR_HANDLE {
    CD3DX12_CPU_DESCRIPTOR_HANDLE() = default;
    explicit CD3DX12_CPU_DESCRIPTOR_HANDLE(const D3D12_CPU_DESCRIPTOR_HANDLE& o) noexcept
        : D3D12_CPU_DESCRIPTOR_HANDLE(o) {}
    CD3DX12_CPU_DESCRIPTOR_HANDLE(const D3D12_CPU_DESCRIPTOR_HANDLE& other, INT offsetScaledByIncrementSize) noexcept {
        ptr = static_cast<SIZE_T>(other.ptr + offsetScaledByIncrementSize);
    }
    CD3DX12_CPU_DESCRIPTOR_HANDLE(const D3D12_CPU_DESCRIPTOR_HANDLE& other, INT offsetInDescriptors, UINT descriptorIncrementSize) noexcept {
        ptr = static_cast<SIZE_T>(other.ptr + static_cast<INT64>(offsetInDescriptors) * static_cast<INT64>(descriptorIncrementSize));
    }
};

struct CD3DX12_CLEAR_VALUE : D3D12_CLEAR_VALUE {
    CD3DX12_CLEAR_VALUE() = default;
    CD3DX12_CLEAR_VALUE(DXGI_FORMAT format, const FLOAT color[4]) noexcept {
        Format = format;
        std::memcpy(Color, color, sizeof(Color));
    }
    CD3DX12_CLEAR_VALUE(DXGI_FORMAT format, FLOAT depth, UINT8 stencil) noexcept {
        Format = format;
        std::memset(&Color, 0, sizeof(Color));
        DepthStencil.Depth = depth;
        DepthStencil.Stencil = stencil;
    }
};

struct CD3DX12_RESOURCE_BARRIER : D3D12_RESOURCE_BARRIER {
    CD3DX12_RESOURCE_BARRIER() = default;
    static inline CD3DX12_RESOURCE_BARRIER Transition(
        ID3D12Resource* pResource,
        D3D12_RESOURCE_STATES stateBefore,
        D3D12_RESOURCE_STATES stateAfter,
        UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_BARRIER_FLAGS flags = D3D12_RESOURCE_BARRIER_FLAG_NONE) noexcept {
        CD3DX12_RESOURCE_BARRIER result{};
        result.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        result.Flags = flags;
        result.D3D12_RESOURCE_BARRIER::Transition.pResource = pResource;
        result.D3D12_RESOURCE_BARRIER::Transition.StateBefore = stateBefore;
        result.D3D12_RESOURCE_BARRIER::Transition.StateAfter = stateAfter;
        result.D3D12_RESOURCE_BARRIER::Transition.Subresource = subresource;
        return result;
    }
};

// Texture upload helpers (GetRequiredIntermediateSize & UpdateSubresources)
inline UINT64 GetRequiredIntermediateSize(
    ID3D12Resource* pDestinationResource,
    UINT FirstSubresource,
    UINT NumSubresources) noexcept {
    if (!pDestinationResource || NumSubresources == 0) {
        return 0;
    }
    const D3D12_RESOURCE_DESC desc = pDestinationResource->GetDesc();
    UINT64 requiredSize = 0;
    ID3D12Device* device = nullptr;
    if (SUCCEEDED(pDestinationResource->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) && device) {
        device->GetCopyableFootprints(&desc, FirstSubresource, NumSubresources, 0,
                                      nullptr, nullptr, nullptr, &requiredSize);
        device->Release();
    }
    return requiredSize;
}

inline UINT64 UpdateSubresources(
    ID3D12GraphicsCommandList* pCmdList,
    ID3D12Resource* pDestinationResource,
    ID3D12Resource* pIntermediate,
    UINT64 IntermediateOffset,
    UINT FirstSubresource,
    UINT NumSubresources,
    const D3D12_SUBRESOURCE_DATA* pSrcData) noexcept {
    if (!pCmdList || !pDestinationResource || !pIntermediate || !pSrcData || NumSubresources == 0 || NumSubresources > 16) {
        return 0;
    }
    const D3D12_RESOURCE_DESC desc = pDestinationResource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[16]{};
    UINT numRows[16]{};
    UINT64 rowSizes[16]{};
    UINT64 requiredSize = 0;

    ID3D12Device* device = nullptr;
    if (FAILED(pDestinationResource->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device) {
        return 0;
    }
    device->GetCopyableFootprints(&desc, FirstSubresource, NumSubresources, IntermediateOffset,
                                  layouts, numRows, rowSizes, &requiredSize);
    device->Release();

    BYTE* pData = nullptr;
    if (FAILED(pIntermediate->Map(0, nullptr, reinterpret_cast<void**>(&pData))) || !pData) {
        return 0;
    }

    for (UINT i = 0; i < NumSubresources; ++i) {
        const auto& layout = layouts[i];
        BYTE* dstSlice = pData + layout.Offset;
        const BYTE* srcSlice = static_cast<const BYTE*>(pSrcData[i].pData);
        for (UINT z = 0; z < layout.Footprint.Depth; ++z) {
            BYTE* dstRows = dstSlice + static_cast<SIZE_T>(z) * layout.Footprint.RowPitch * numRows[i];
            const BYTE* srcRows = srcSlice + static_cast<SIZE_T>(z) * pSrcData[i].SlicePitch;
            for (UINT y = 0; y < numRows[i]; ++y) {
                std::memcpy(dstRows + static_cast<SIZE_T>(y) * layout.Footprint.RowPitch,
                            srcRows + static_cast<SIZE_T>(y) * pSrcData[i].RowPitch,
                            static_cast<SIZE_T>(rowSizes[i]));
            }
        }
    }
    pIntermediate->Unmap(0, nullptr);

    for (UINT i = 0; i < NumSubresources; ++i) {
        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = pDestinationResource;
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = i + FirstSubresource;

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = pIntermediate;
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = layouts[i];

        pCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    }
    return requiredSize;
}

// Mesh shader pipeline state stream helpers (used by SimpleMeshShader)
template <typename InnerStructType, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename DefaultArg = InnerStructType>
class alignas(void*) CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE pssType;
    InnerStructType pssInner;
public:
    CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT() noexcept : pssType(Type), pssInner(DefaultArg()) {}
    CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT(InnerStructType const& i) noexcept : pssType(Type), pssInner(i) {}
    CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT& operator=(InnerStructType const& i) noexcept { pssType = Type; pssInner = i; return *this; }
    operator InnerStructType const&() const noexcept { return pssInner; }
    operator InnerStructType&() noexcept { return pssInner; }
};

struct D3DX12_MESH_SHADER_PIPELINE_STATE_DESC {
    ID3D12RootSignature*          pRootSignature;
    D3D12_SHADER_BYTECODE         AS;
    D3D12_SHADER_BYTECODE         MS;
    D3D12_SHADER_BYTECODE         PS;
    D3D12_BLEND_DESC              BlendState;
    UINT                          SampleMask;
    D3D12_RASTERIZER_DESC         RasterizerState;
    D3D12_DEPTH_STENCIL_DESC      DepthStencilState;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE PrimitiveTopologyType;
    UINT                          NumRenderTargets;
    DXGI_FORMAT                   RTVFormats[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
    DXGI_FORMAT                   DSVFormat;
    DXGI_SAMPLE_DESC              SampleDesc;
    UINT                          NodeMask;
    D3D12_CACHED_PIPELINE_STATE   CachedPSO;
    D3D12_PIPELINE_STATE_FLAGS    Flags;
};

struct CD3DX12_PIPELINE_MESH_STATE_STREAM {
    CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> pRootSignature;
    CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS> AS;
    CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS> MS;
    CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> PS;
    CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> BlendState;
    CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT<UINT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK> SampleMask;
    CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> RasterizerState;
    CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT<D3D12_DEPTH_STENCIL_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL> DepthStencilState;
    CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT<D3D12_PRIMITIVE_TOPOLOGY_TYPE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY> PrimitiveTopologyType;
    CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT<D3D12_RT_FORMAT_ARRAY, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS> RTVFormats;
    CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT<DXGI_FORMAT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT> DSVFormat;
    CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> SampleDesc;

    CD3DX12_PIPELINE_MESH_STATE_STREAM() = default;
    explicit CD3DX12_PIPELINE_MESH_STATE_STREAM(const D3DX12_MESH_SHADER_PIPELINE_STATE_DESC& d) noexcept
        : pRootSignature(d.pRootSignature)
        , AS(d.AS)
        , MS(d.MS)
        , PS(d.PS)
        , BlendState(d.BlendState)
        , SampleMask(d.SampleMask)
        , RasterizerState(d.RasterizerState)
        , DepthStencilState(d.DepthStencilState)
        , PrimitiveTopologyType(d.PrimitiveTopologyType)
        , DSVFormat(d.DSVFormat)
        , SampleDesc(d.SampleDesc) {
        D3D12_RT_FORMAT_ARRAY rts{};
        rts.NumRenderTargets = d.NumRenderTargets;
        for (UINT i = 0; i < d.NumRenderTargets && i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            rts.RTFormats[i] = d.RTVFormats[i];
        }
        RTVFormats = rts;
    }
};
