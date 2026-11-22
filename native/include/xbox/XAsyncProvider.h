// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox GDK Async Provider API header (<XAsyncProvider.h>).

#pragma once

#include "XAsync.h"

enum class XAsyncOp : uint32_t {
    Begin     = 0,
    DoWork    = 1,
    GetResult = 2,
    Cancel    = 3,
    Cleanup   = 4
};

struct XAsyncProviderData {
    XAsyncBlock* async;
    size_t bufferSize;
    void* buffer;
    void* context;
};

using XAsyncProvider = HRESULT(CALLBACK)(XAsyncOp op, const XAsyncProviderData* data);

#define XASYNC_IDENTITY(method) (#method)

extern "C" {

HRESULT STDAPICALLTYPE XAsyncBegin(
    XAsyncBlock* asyncBlock,
    void* context,
    const void* identity,
    const char* identityName,
    XAsyncProvider* provider) noexcept;

HRESULT STDAPICALLTYPE XAsyncSchedule(
    XAsyncBlock* asyncBlock,
    uint32_t delayInMs) noexcept;

void STDAPICALLTYPE XAsyncComplete(
    XAsyncBlock* asyncBlock,
    HRESULT result,
    size_t requiredBufferSize) noexcept;

HRESULT STDAPICALLTYPE XAsyncGetResult(
    XAsyncBlock* asyncBlock,
    const void* identity,
    size_t bufferSize,
    void* buffer,
    size_t* bufferUsed) noexcept;

} // extern "C"
