// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox Series X|S Game Streaming (XGameStreaming) offline compatibility header.

#pragma once

#include <Windows.h>
#include <cstdint>
#include "XTaskQueue.h"

#ifndef E_GAMESTREAMING_NOT_INITIALIZED
#define E_GAMESTREAMING_NOT_INITIALIZED      static_cast<HRESULT>(0x89245400L)
#endif
#ifndef E_GAMESTREAMING_CLIENT_NOT_CONNECTED
#define E_GAMESTREAMING_CLIENT_NOT_CONNECTED static_cast<HRESULT>(0x89245401L)
#endif

using XGameStreamingClientId = uint64_t;
constexpr XGameStreamingClientId XGameStreamingNullClientId = 0ULL;

enum class XGameStreamingConnectionState : uint32_t {
    Disconnected = 0,
    Connected = 1
};

using XGameStreamingConnectionStateChangedCallback = void CALLBACK(
    void* context,
    XGameStreamingClientId client,
    XGameStreamingConnectionState state);

extern "C" {

HRESULT STDAPICALLTYPE XGameStreamingInitialize() noexcept;
void STDAPICALLTYPE XGameStreamingUninitialize() noexcept;
bool STDAPICALLTYPE XGameStreamingIsStreaming() noexcept;
uint32_t STDAPICALLTYPE XGameStreamingGetClientCount() noexcept;
HRESULT STDAPICALLTYPE XGameStreamingGetClients(
    uint32_t clientCount,
    XGameStreamingClientId* clients,
    uint32_t* clientsUsed) noexcept;
void STDAPICALLTYPE XGameStreamingHideTouchControls() noexcept;
void STDAPICALLTYPE XGameStreamingShowTouchControlLayout(const char* layout) noexcept;
HRESULT STDAPICALLTYPE XGameStreamingGetTouchBundleVersion(
    XGameStreamingClientId client,
    XVersion* version,
    size_t versionNameSize,
    char* versionName) noexcept;
HRESULT STDAPICALLTYPE XGameStreamingRegisterConnectionStateChanged(
    XTaskQueueHandle queue,
    void* context,
    XGameStreamingConnectionStateChangedCallback* callback,
    XTaskQueueRegistrationToken* token) noexcept;
bool STDAPICALLTYPE XGameStreamingUnregisterConnectionStateChanged(
    XTaskQueueRegistrationToken token,
    bool wait) noexcept;

} // extern "C"
