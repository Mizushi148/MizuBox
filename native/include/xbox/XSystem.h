// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox GDK System Information API header (<XSystem.h>).

#pragma once

#include "XGameRuntimeTypes.h"
#include <cstddef>
#include <cstdint>

enum class XSystemDeviceType : uint32_t {
    Unknown              = 0x00,
    Pc                   = 0x01,
    XboxOne              = 0x02,
    XboxOneS             = 0x03,
    XboxOneX             = 0x04,
    XboxOneXDevkit       = 0x05,
    XboxScarlettLockhart = 0x06,
    XboxScarlettAnaconda = 0x07,
    XboxScarlettDevkit   = 0x08
};

struct XSystemAnalyticsInfo {
    XVersion osVersion;
    XVersion hostingOsVersion;
    char     family[64];
    char     form[64];
};

struct XSystemRuntimeInfo {
    XVersion runtimeVersion;
    XVersion availableVersion;
};

constexpr size_t XSystemConsoleIdBytes            = 40;
constexpr size_t XSystemAppSpecificDeviceIdBytes  = 45;
constexpr size_t XSystemXboxLiveSandboxIdMaxBytes = 16;

extern "C" {

XSystemDeviceType WINAPI XSystemGetDeviceType() noexcept;

XSystemAnalyticsInfo STDAPICALLTYPE XSystemGetAnalyticsInfo() noexcept;

XSystemRuntimeInfo STDAPICALLTYPE XSystemGetRuntimeInfo() noexcept;

HRESULT STDAPICALLTYPE XSystemGetConsoleId(
    size_t consoleIdSize,
    char* consoleId,
    size_t* consoleIdUsed) noexcept;

HRESULT STDAPICALLTYPE XSystemGetAppSpecificDeviceId(
    size_t appSpecificDeviceIdSize,
    char* appSpecificDeviceId,
    size_t* appSpecificDeviceIdUsed) noexcept;

HRESULT STDAPICALLTYPE XSystemGetXboxLiveSandboxId(
    size_t sandboxIdSize,
    char* sandboxId,
    size_t* sandboxIdUsed) noexcept;

bool STDAPICALLTYPE XSystemIsHandleValid(
    HANDLE handle) noexcept;

} // extern "C"
