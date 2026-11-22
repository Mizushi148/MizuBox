// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room DirectXTK12 GraphicsMemory & RenderTargetState compatibility stubs.

#pragma once
#include "d3d12_xs.h"

namespace DirectX {

class GraphicsMemory {
public:
    explicit GraphicsMemory(::ID3D12Device* /*device*/) noexcept {}
    ~GraphicsMemory() = default;

    void Commit(::ID3D12CommandQueue* /*commandQueue*/) noexcept {}
};

struct RenderTargetState {
    DXGI_FORMAT rtFormats[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    UINT numRenderTargets = 0;
    DXGI_FORMAT dsFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC sampleDesc{1, 0};
    UINT sampleMask = UINT_MAX;
    UINT nodeMask = 0;

    RenderTargetState() noexcept = default;
    RenderTargetState(DXGI_FORMAT rtFormat, DXGI_FORMAT depthFormat) noexcept
        : numRenderTargets(1), dsFormat(depthFormat) {
        rtFormats[0] = rtFormat;
    }
};

} // namespace DirectX
