// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox GDK Networking API header (<XNetworking.h>).

#pragma once

#include "XAsync.h"
#include "XTaskQueue.h"
#include <cstdint>

enum class XNetworkingConnectivityLevelHint : uint32_t {
    Unknown                   = 0,
    None                      = 1,
    LocalAccess               = 2,
    InternetAccess            = 3,
    ConstrainedInternetAccess = 4
};

enum class XNetworkingConnectivityCostHint : uint32_t {
    Unknown      = 0,
    Unrestricted = 1,
    Fixed        = 2,
    Variable     = 3
};

struct XNetworkingConnectivityHint {
    XNetworkingConnectivityLevelHint connectivityLevel;
    XNetworkingConnectivityCostHint  connectivityCost;
    uint32_t                         ianaInterfaceType;
    bool                             networkInitialized;
    bool                             approachingDataLimit;
    bool                             overDataLimit;
    bool                             roaming;
};

enum class XNetworkingThumbprintType : uint32_t {
    Leaf   = 0,
    Issuer = 1,
    Root   = 2
};

struct XNetworkingThumbprint {
    uint32_t thumbprintType;
    size_t   thumbprintBufferByteCount;
    uint8_t* thumbprintBuffer;
};

struct XNetworkingSecurityInformation {
    uint32_t               enabledHttpSecurityProtocolFlags;
    size_t                 thumbprintCount;
    XNetworkingThumbprint* thumbprints;
};

enum class XNetworkingStatisticsType : uint32_t {
    TitleTcpConnections = 0,
    TitleUdpConnections = 1
};

union XNetworkingStatisticsBuffer {
    struct {
        uint64_t bytesSent;
        uint64_t bytesReceived;
        uint64_t packetsSent;
        uint64_t packetsReceived;
    } titleTcpConnections;
    struct {
        uint64_t bytesSent;
        uint64_t bytesReceived;
        uint64_t packetsSent;
        uint64_t packetsReceived;
    } titleUdpConnections;
    uint8_t raw[64];
};

enum class XNetworkingConfigurationSetting : uint32_t {
    MaxTitleTcpConnections = 0,
    MaxTitleUdpConnections = 1
};

using XNetworkingConnectivityHintChangedCallback = void(CALLBACK)(
    void* context,
    const XNetworkingConnectivityHint* connectivityHint);

using XNetworkingPreferredLocalUdpMultiplayerPortChangedCallback = void(CALLBACK)(
    void* context,
    uint16_t preferredLocalUdpMultiplayerPort);

extern "C" {

HRESULT STDAPICALLTYPE XNetworkingGetConnectivityHint(
    XNetworkingConnectivityHint* connectivityHint) noexcept;

HRESULT STDAPICALLTYPE XNetworkingRegisterConnectivityHintChanged(
    XTaskQueueHandle queue,
    void* context,
    XNetworkingConnectivityHintChangedCallback* callback,
    XTaskQueueRegistrationToken* token) noexcept;

bool STDAPICALLTYPE XNetworkingUnregisterConnectivityHintChanged(
    XTaskQueueRegistrationToken token,
    bool wait) noexcept;

HRESULT STDAPICALLTYPE XNetworkingQueryPreferredLocalUdpMultiplayerPort(
    uint16_t* preferredLocalUdpMultiplayerPort) noexcept;

HRESULT STDAPICALLTYPE XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync(
    XAsyncBlock* asyncBlock) noexcept;

HRESULT STDAPICALLTYPE XNetworkingQueryPreferredLocalUdpMultiplayerPortResult(
    XAsyncBlock* asyncBlock,
    uint16_t* preferredLocalUdpMultiplayerPort) noexcept;

HRESULT STDAPICALLTYPE XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged(
    XTaskQueueHandle queue,
    void* context,
    XNetworkingPreferredLocalUdpMultiplayerPortChangedCallback* callback,
    XTaskQueueRegistrationToken* token) noexcept;

bool STDAPICALLTYPE XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged(
    XTaskQueueRegistrationToken token,
    bool wait) noexcept;

HRESULT STDAPICALLTYPE XNetworkingQuerySecurityInformationForUrlAsync(
    const char* url,
    XAsyncBlock* asyncBlock) noexcept;

HRESULT STDAPICALLTYPE XNetworkingQuerySecurityInformationForUrlResultSize(
    XAsyncBlock* asyncBlock,
    size_t* securityInformationBufferByteCount) noexcept;

HRESULT STDAPICALLTYPE XNetworkingQuerySecurityInformationForUrlResult(
    XAsyncBlock* asyncBlock,
    size_t securityInformationBufferByteCount,
    size_t* securityInformationBufferByteCountUsed,
    uint8_t* securityInformationBuffer,
    XNetworkingSecurityInformation** securityInformation) noexcept;

HRESULT STDAPICALLTYPE XNetworkingQuerySecurityInformationForUrlUtf16Async(
    const wchar_t* url,
    XAsyncBlock* asyncBlock) noexcept;

HRESULT STDAPICALLTYPE XNetworkingQuerySecurityInformationForUrlUtf16ResultSize(
    XAsyncBlock* asyncBlock,
    size_t* securityInformationBufferByteCount) noexcept;

HRESULT STDAPICALLTYPE XNetworkingQuerySecurityInformationForUrlUtf16Result(
    XAsyncBlock* asyncBlock,
    size_t securityInformationBufferByteCount,
    size_t* securityInformationBufferByteCountUsed,
    uint8_t* securityInformationBuffer,
    XNetworkingSecurityInformation** securityInformation) noexcept;

HRESULT STDAPICALLTYPE XNetworkingVerifyServerCertificate(
    void* requestHandle,
    const XNetworkingThumbprint* thumbprint) noexcept;

HRESULT STDAPICALLTYPE XNetworkingQueryStatistics(
    XNetworkingStatisticsType statisticsType,
    XNetworkingStatisticsBuffer* statisticsBuffer) noexcept;

HRESULT STDAPICALLTYPE XNetworkingQueryConfigurationSetting(
    XNetworkingConfigurationSetting configuration,
    uint64_t* value) noexcept;

} // extern "C"
