// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room DirectXTK12 SimpleMath compatibility header.

#pragma once

#include <Windows.h>
#include <DirectXMath.h>

namespace DirectX {
namespace SimpleMath {

struct Matrix : public XMFLOAT4X4 {
    Matrix() noexcept
        : XMFLOAT4X4(1.f, 0.f, 0.f, 0.f,
                     0.f, 1.f, 0.f, 0.f,
                     0.f, 0.f, 1.f, 0.f,
                     0.f, 0.f, 0.f, 1.f) {}

    Matrix(const XMFLOAT4X4& m) noexcept : XMFLOAT4X4(m) {}
    Matrix(FXMMATRIX m) noexcept { XMStoreFloat4x4(this, m); }

    Matrix& operator=(const XMFLOAT4X4& m) noexcept {
        *static_cast<XMFLOAT4X4*>(this) = m;
        return *this;
    }
    Matrix& operator=(FXMMATRIX m) noexcept {
        XMStoreFloat4x4(this, m);
        return *this;
    }

    operator XMMATRIX() const noexcept { return XMLoadFloat4x4(this); }
};

struct Viewport {
    static RECT ComputeTitleSafeArea(UINT backBufferWidth, UINT backBufferHeight) noexcept {
        const float safew = (static_cast<float>(backBufferWidth) + 19.f) / 20.f;
        const float safeh = (static_cast<float>(backBufferHeight) + 19.f) / 20.f;
        return RECT{
            static_cast<LONG>(safew),
            static_cast<LONG>(safeh),
            static_cast<LONG>(static_cast<float>(backBufferWidth) - safew + 0.5f),
            static_cast<LONG>(static_cast<float>(backBufferHeight) - safeh + 0.5f)
        };
    }
};

} // namespace SimpleMath
} // namespace DirectX
