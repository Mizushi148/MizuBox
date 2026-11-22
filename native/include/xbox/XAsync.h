// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox GDK Asynchronous Block API header (<XAsync.h>).

#pragma once

#include "XTaskQueue.h"
#include <cstddef>
#include <cstdint>

struct XAsyncBlock;
using XAsyncCompletionRoutine = void(CALLBACK)(XAsyncBlock* asyncBlock);
using XAsyncWork = HRESULT(CALLBACK)(XAsyncBlock* asyncBlock);

struct XAsyncBlock {
    XTaskQueueHandle queue;
    void* context;
    XAsyncCompletionRoutine* callback;
    unsigned char internal[sizeof(void*) * 4];
};

extern "C" {

HRESULT STDAPICALLTYPE XAsyncGetStatus(
    XAsyncBlock* asyncBlock,
    bool wait) noexcept;

HRESULT STDAPICALLTYPE XAsyncGetResultSize(
    XAsyncBlock* asyncBlock,
    size_t* bufferSize) noexcept;

void STDAPICALLTYPE XAsyncCancel(
    XAsyncBlock* asyncBlock) noexcept;

HRESULT STDAPICALLTYPE XAsyncRun(
    XAsyncBlock* asyncBlock,
    XAsyncWork* work) noexcept;

} // extern "C"
