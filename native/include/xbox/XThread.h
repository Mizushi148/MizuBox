// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox GDK Threading API header (<XThread.h>).

#pragma once

#include <Windows.h>

extern "C" {

bool STDAPICALLTYPE XThreadIsTimeSensitive() noexcept;

HRESULT STDAPICALLTYPE XThreadSetTimeSensitive(
    bool isTimeSensitive) noexcept;

void STDAPICALLTYPE XThreadAssertNotTimeSensitive() noexcept;

} // extern "C"
