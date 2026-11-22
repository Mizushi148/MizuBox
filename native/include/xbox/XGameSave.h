// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox GDK Connected Storage / Game Save API header (<XGameSave.h>).

#pragma once

#include "XAsync.h"
#include "XUser.h"
#include <cstddef>
#include <cstdint>
#include <ctime>

struct XGameSaveProvider;
using XGameSaveProviderHandle = XGameSaveProvider*;

struct XGameSaveContainer;
using XGameSaveContainerHandle = XGameSaveContainer*;

struct XGameSaveUpdate;
using XGameSaveUpdateHandle = XGameSaveUpdate*;

constexpr size_t X_GS_MAX_CONTAINER_NAME_SIZE = 64;
constexpr size_t X_GS_MAX_BLOB_NAME_SIZE      = 64;

constexpr HRESULT E_GS_INVALID_CONTAINER_NAME = static_cast<HRESULT>(0x80830001L);
constexpr HRESULT E_GS_NO_ACCESS              = static_cast<HRESULT>(0x80830002L);
constexpr HRESULT E_GS_OUT_OF_LOCAL_STORAGE   = static_cast<HRESULT>(0x80830003L);
constexpr HRESULT E_GS_USER_CANCELED          = static_cast<HRESULT>(0x80830004L);
constexpr HRESULT E_GS_UPDATE_TOO_BIG         = static_cast<HRESULT>(0x80830005L);
constexpr HRESULT E_GS_QUOTA_EXCEEDED         = static_cast<HRESULT>(0x80830006L);
constexpr HRESULT E_GS_PROVIDED_BUFFER_TOO_SMALL = static_cast<HRESULT>(0x80830007L);
constexpr HRESULT E_GS_BLOB_NOT_FOUND         = static_cast<HRESULT>(0x80830008L);
constexpr HRESULT E_GS_NO_SERVICE_CONFIGURATION = static_cast<HRESULT>(0x80830009L);
constexpr HRESULT E_GS_CONTAINER_NOT_IN_SYNC  = static_cast<HRESULT>(0x8083000AL);
constexpr HRESULT E_GS_CONTAINER_SYNC_FAILED  = static_cast<HRESULT>(0x8083000BL);
constexpr HRESULT E_GS_USER_NOT_REGISTERED_IN_SERVICE = static_cast<HRESULT>(0x8083000CL);
constexpr HRESULT E_GS_HANDLE_EXPIRED         = static_cast<HRESULT>(0x8083000DL);

struct XGameSaveContainerInfo {
    const char* name;
    const char* displayName;
    uint32_t blobCount;
    uint64_t totalSize;
    time_t lastModifiedTime;
    bool needsSync;
};

struct XGameSaveBlobInfo {
    const char* name;
    uint32_t size;
};

struct XGameSaveBlob {
    XGameSaveBlobInfo info;
    uint8_t* data;
};

using XGameSaveContainerInfoCallback = bool(CALLBACK)(
    const XGameSaveContainerInfo* info,
    void* context);

using XGameSaveBlobInfoCallback = bool(CALLBACK)(
    const XGameSaveBlobInfo* info,
    void* context);

extern "C" {

HRESULT STDAPICALLTYPE XGameSaveInitializeProvider(
    XUserHandle requestingUser,
    const char* configurationId,
    bool syncOnDemand,
    XGameSaveProviderHandle* provider) noexcept;

HRESULT STDAPICALLTYPE XGameSaveInitializeProviderAsync(
    XUserHandle requestingUser,
    const char* configurationId,
    bool syncOnDemand,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XGameSaveInitializeProviderResult(
    XAsyncBlock* async,
    XGameSaveProviderHandle* provider) noexcept;

void STDAPICALLTYPE XGameSaveCloseProvider(
    XGameSaveProviderHandle provider) noexcept;

HRESULT STDAPICALLTYPE XGameSaveGetRemainingQuota(
    XGameSaveProviderHandle provider,
    int64_t* remainingQuota) noexcept;

HRESULT STDAPICALLTYPE XGameSaveGetRemainingQuotaAsync(
    XGameSaveProviderHandle provider,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XGameSaveGetRemainingQuotaResult(
    XAsyncBlock* async,
    int64_t* remainingQuota) noexcept;

HRESULT STDAPICALLTYPE XGameSaveCreateContainer(
    XGameSaveProviderHandle provider,
    const char* containerName,
    XGameSaveContainerHandle* containerContext) noexcept;

void STDAPICALLTYPE XGameSaveCloseContainer(
    XGameSaveContainerHandle containerContext) noexcept;

HRESULT STDAPICALLTYPE XGameSaveDeleteContainer(
    XGameSaveProviderHandle provider,
    const char* containerName) noexcept;

HRESULT STDAPICALLTYPE XGameSaveDeleteContainerAsync(
    XGameSaveProviderHandle provider,
    const char* containerName,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XGameSaveDeleteContainerResult(
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XGameSaveCreateUpdate(
    XGameSaveContainerHandle container,
    const char* containerDisplayName,
    XGameSaveUpdateHandle* updateContext) noexcept;

void STDAPICALLTYPE XGameSaveCloseUpdate(
    XGameSaveUpdateHandle updateContext) noexcept;

HRESULT STDAPICALLTYPE XGameSaveSubmitBlobWrite(
    XGameSaveUpdateHandle updateContext,
    const char* blobName,
    const uint8_t* data,
    size_t dataSize) noexcept;

HRESULT STDAPICALLTYPE XGameSaveSubmitBlobDelete(
    XGameSaveUpdateHandle updateContext,
    const char* blobName) noexcept;

HRESULT STDAPICALLTYPE XGameSaveSubmitUpdate(
    XGameSaveUpdateHandle updateContext) noexcept;

HRESULT STDAPICALLTYPE XGameSaveSubmitUpdateAsync(
    XGameSaveUpdateHandle updateContext,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XGameSaveSubmitUpdateResult(
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XGameSaveEnumerateContainerInfo(
    XGameSaveProviderHandle provider,
    void* context,
    XGameSaveContainerInfoCallback* callback) noexcept;

HRESULT STDAPICALLTYPE XGameSaveEnumerateContainerInfoByName(
    XGameSaveProviderHandle provider,
    const char* containerNamePrefix,
    void* context,
    XGameSaveContainerInfoCallback* callback) noexcept;

HRESULT STDAPICALLTYPE XGameSaveGetContainerInfo(
    XGameSaveProviderHandle provider,
    const char* containerName,
    void* context,
    XGameSaveContainerInfoCallback* callback) noexcept;

HRESULT STDAPICALLTYPE XGameSaveEnumerateBlobInfo(
    XGameSaveContainerHandle container,
    void* context,
    XGameSaveBlobInfoCallback* callback) noexcept;

HRESULT STDAPICALLTYPE XGameSaveEnumerateBlobInfoByName(
    XGameSaveContainerHandle container,
    const char* blobName,
    void* context,
    XGameSaveBlobInfoCallback* callback) noexcept;

HRESULT STDAPICALLTYPE XGameSaveReadBlobData(
    XGameSaveContainerHandle container,
    const char* const* blobNames,
    uint32_t* countOfBlobs,
    size_t blobsSize,
    XGameSaveBlob* blobData) noexcept;

HRESULT STDAPICALLTYPE XGameSaveReadBlobDataAsync(
    XGameSaveContainerHandle container,
    const char* const* blobNames,
    uint32_t countOfBlobs,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XGameSaveReadBlobDataResult(
    XAsyncBlock* async,
    size_t blobsSize,
    XGameSaveBlob* blobData,
    uint32_t* countOfBlobs) noexcept;

} // extern "C"
