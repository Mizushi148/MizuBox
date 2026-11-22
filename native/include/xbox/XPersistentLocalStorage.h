// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox GDK Persistent Local Storage API header (<XPersistentLocalStorage.h>).

#pragma once

#include "XAsync.h"
#include <cstddef>
#include <cstdint>

struct XPersistentLocalStorageSpaceInfo {
    uint64_t availableFreeBytes;
    uint64_t totalFreeBytes;
    uint64_t usedBytes;
    uint64_t totalBytes;
};

extern "C" {

HRESULT STDAPICALLTYPE XPersistentLocalStorageGetPathSize(
    size_t* pathSize) noexcept;

HRESULT STDAPICALLTYPE XPersistentLocalStorageGetPath(
    size_t pathSize,
    char* path,
    size_t* pathUsed) noexcept;

HRESULT STDAPICALLTYPE XPersistentLocalStorageGetSpaceInfo(
    XPersistentLocalStorageSpaceInfo* spaceInfo) noexcept;

HRESULT STDAPICALLTYPE XPersistentLocalStoragePromptUserForSpaceAsync(
    uint64_t requestedBytes,
    XAsyncBlock* asyncBlock) noexcept;

HRESULT STDAPICALLTYPE XPersistentLocalStoragePromptUserForSpaceResult(
    XAsyncBlock* asyncBlock) noexcept;

} // extern "C"
