// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox GDK Packaging & Content Mount API header (<XPackage.h>).

#pragma once

#include "XGameRuntimeTypes.h"
#include "XAsync.h"
#include "XTaskQueue.h"
#include <cstddef>
#include <cstdint>

constexpr size_t XPACKAGE_IDENTIFIER_MAX_LENGTH = 33;
constexpr size_t LOCALE_NAME_MAX_BYTES          = 85;

struct XPackageMount;
using XPackageMountHandle = XPackageMount*;

struct XPackageInstallationMonitor;
using XPackageInstallationMonitorHandle = XPackageInstallationMonitor*;

enum class XPackageKind : uint32_t {
    Game    = 0,
    Content = 1
};

enum class XPackageEnumerationScope : uint32_t {
    ThisOnly    = 0,
    ThisAndRelated = 1
};

enum class XPackageChunkAvailability : uint32_t {
    Ready       = 0,
    Pending     = 1,
    Installable = 2,
    Unavailable = 3
};

enum class XPackageChunkSelectorType : uint32_t {
    Language = 1,
    Tag      = 2,
    Feature  = 3,
    Chunk    = 4
};

struct XPackageChunkSelector {
    XPackageChunkSelectorType type;
    union {
        const char* language;
        const char* tag;
        const char* feature;
        uint32_t    chunkId;
    };
};

struct XPackageDetails {
    const char*  packageIdentifier;
    XVersion     version;
    XPackageKind kind;
    const char*  displayName;
    const char*  description;
    const char*  publisher;
    const char*  storeId;
    bool         installing;
    uint32_t     index;
    uint32_t     count;
    bool         ageRestricted;
    const char*  titleId;
};

struct XPackageInstallationProgress {
    uint64_t totalBytes;
    uint64_t installedBytes;
    uint64_t launchBytes;
    bool     launchable;
    bool     completed;
};

using XPackageEnumerationCallback = bool(CALLBACK)(
    void* context,
    const XPackageDetails* details);

using XPackageChunkAvailabilityCallback = bool(CALLBACK)(
    void* context,
    const XPackageChunkSelector* selector,
    XPackageChunkAvailability availability);

extern "C" {

HRESULT STDAPICALLTYPE XPackageGetCurrentProcessPackageIdentifier(
    size_t bufferSize,
    char* buffer) noexcept;

bool STDAPICALLTYPE XPackageIsPackagedProcess() noexcept;

HRESULT STDAPICALLTYPE XPackageGetUserLocale(
    size_t localeSize,
    char* locale) noexcept;

HRESULT STDAPICALLTYPE XPackageEnumeratePackages(
    XPackageKind kind,
    XPackageEnumerationScope scope,
    void* context,
    XPackageEnumerationCallback* callback) noexcept;

HRESULT STDAPICALLTYPE XPackageCreateInstallationMonitor(
    const char* packageIdentifier,
    uint32_t selectorCount,
    const XPackageChunkSelector* selectors,
    uint32_t minimumUpdateIntervalMs,
    XTaskQueueHandle queue,
    XPackageInstallationMonitorHandle* installationMonitor) noexcept;

void STDAPICALLTYPE XPackageGetInstallationProgress(
    XPackageInstallationMonitorHandle installationMonitor,
    XPackageInstallationProgress* progress) noexcept;

void STDAPICALLTYPE XPackageCloseInstallationMonitorHandle(
    XPackageInstallationMonitorHandle installationMonitor) noexcept;

HRESULT STDAPICALLTYPE XPackageMountWithUiAsync(
    const char* packageIdentifier,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XPackageMountWithUiResult(
    XAsyncBlock* async,
    XPackageMountHandle* mount) noexcept;

HRESULT STDAPICALLTYPE XPackageGetMountPathSize(
    XPackageMountHandle mount,
    size_t* pathSize) noexcept;

HRESULT STDAPICALLTYPE XPackageGetMountPath(
    XPackageMountHandle mount,
    size_t pathSize,
    char* path) noexcept;

void STDAPICALLTYPE XPackageCloseMountHandle(
    XPackageMountHandle mount) noexcept;

HRESULT STDAPICALLTYPE XPackageFindChunkAvailability(
    const char* packageIdentifier,
    uint32_t selectorCount,
    const XPackageChunkSelector* selectors,
    XPackageChunkAvailability* availability) noexcept;

HRESULT STDAPICALLTYPE XPackageEnumerateChunkAvailability(
    const char* packageIdentifier,
    XPackageChunkSelectorType type,
    void* context,
    XPackageChunkAvailabilityCallback* callback) noexcept;

} // extern "C"
