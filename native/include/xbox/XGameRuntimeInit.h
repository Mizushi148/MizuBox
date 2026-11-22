// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0

#pragma once
#include <Windows.h>

extern "C" {
HRESULT WINAPI XGameRuntimeInitialize() noexcept;
VOID WINAPI XGameRuntimeUninitialize() noexcept;
}
