// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox GDK Task Queue API header (<XTaskQueue.h>).

#pragma once

#include <Windows.h>
#include <cstdint>

struct XTaskQueueObject;
using XTaskQueueHandle = XTaskQueueObject*;

struct XTaskQueuePortObject;
using XTaskQueuePortHandle = XTaskQueuePortObject*;

struct XTaskQueueRegistrationToken {
    uint64_t token;
};

enum class XTaskQueuePort : uint32_t {
    Work       = 0,
    Completion = 1
};

enum class XTaskQueueDispatchMode : uint32_t {
    Manual               = 0,
    ThreadPool           = 1,
    SerializedThreadPool = 2,
    Immediate            = 3
};

using XTaskQueueCallback = void(CALLBACK)(void* context, bool canceled);
using XTaskQueueTerminatedCallback = void(CALLBACK)(void* context);
using XTaskQueueMonitorCallback = void(CALLBACK)(void* context, XTaskQueueHandle queue, XTaskQueuePort port);

extern "C" {

HRESULT STDAPICALLTYPE XTaskQueueCreate(
    XTaskQueueDispatchMode workDispatchMode,
    XTaskQueueDispatchMode completionDispatchMode,
    XTaskQueueHandle* queue) noexcept;

void STDAPICALLTYPE XTaskQueueCloseHandle(
    XTaskQueueHandle queue) noexcept;

HRESULT STDAPICALLTYPE XTaskQueueDuplicateHandle(
    XTaskQueueHandle queueHandle,
    XTaskQueueHandle* duplicatedHandle) noexcept;

bool STDAPICALLTYPE XTaskQueueDispatch(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    uint32_t timeoutInMs) noexcept;

HRESULT STDAPICALLTYPE XTaskQueueSubmitCallback(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    void* callbackContext,
    XTaskQueueCallback* callback) noexcept;

HRESULT STDAPICALLTYPE XTaskQueueSubmitDelayedCallback(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    uint32_t delayMs,
    void* callbackContext,
    XTaskQueueCallback* callback) noexcept;

HRESULT STDAPICALLTYPE XTaskQueueTerminate(
    XTaskQueueHandle queue,
    bool wait,
    void* callbackContext,
    XTaskQueueTerminatedCallback* callback) noexcept;

bool STDAPICALLTYPE XTaskQueueGetCurrentProcessTaskQueue(
    XTaskQueueHandle* queue) noexcept;

void STDAPICALLTYPE XTaskQueueSetCurrentProcessTaskQueue(
    XTaskQueueHandle queue) noexcept;

HRESULT STDAPICALLTYPE XTaskQueueGetPort(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    XTaskQueuePortHandle* portHandle) noexcept;

HRESULT STDAPICALLTYPE XTaskQueueCreateComposite(
    XTaskQueuePortHandle workPort,
    XTaskQueuePortHandle completionPort,
    XTaskQueueHandle* queue) noexcept;

HRESULT STDAPICALLTYPE XTaskQueueRegisterMonitor(
    XTaskQueueHandle queue,
    void* callbackContext,
    XTaskQueueMonitorCallback* callback,
    XTaskQueueRegistrationToken* token) noexcept;

void STDAPICALLTYPE XTaskQueueUnregisterMonitor(
    XTaskQueueHandle queue,
    XTaskQueueRegistrationToken token) noexcept;

} // extern "C"
