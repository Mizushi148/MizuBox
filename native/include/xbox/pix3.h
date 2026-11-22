// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0

#pragma once
#include <Windows.h>

constexpr UINT PIX_COLOR_DEFAULT = 0u;

template <typename... Args>
inline void PIXBeginEvent(Args&&...) noexcept {}

template <typename... Args>
inline void PIXEndEvent(Args&&...) noexcept {}

template <typename... Args>
inline void PIXScopedEvent(Args&&...) noexcept {}
