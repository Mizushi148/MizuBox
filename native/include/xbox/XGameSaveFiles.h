// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox GDK GameSaveFiles Win32 IO API header (<XGameSaveFiles.h>).

#pragma once

#include "XAsync.h"
#include "XUser.h"
#include <cstdint>

extern "C" {

HRESULT STDAPICALLTYPE XGameSaveFilesGetFolderWithUiAsync(
    XUserHandle requestingUser,
    const char* configurationId,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XGameSaveFilesGetFolderWithUiResultSize(
    XAsyncBlock* async,
    size_t* folderSize) noexcept;

HRESULT STDAPICALLTYPE XGameSaveFilesGetFolderWithUiResult(
    XAsyncBlock* async,
    size_t folderSize,
    char* folderResult) noexcept;

HRESULT STDAPICALLTYPE XGameSaveFilesGetRemainingQuota(
    XUserHandle userContext,
    const char* configurationId,
    int64_t* remainingQuota) noexcept;

} // extern "C"
