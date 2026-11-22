// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room offline Xbox GDK Store & License API header (<XStore.h>).
// Zero Microsoft cloud calls: resolves game/add-on entitlements strictly from local config.

#pragma once

#include "XAsync.h"
#include "XTaskQueue.h"
#include "XUser.h"
#include <cstddef>
#include <cstdint>
#include <ctime>

constexpr size_t XSTORE_SKU_ID_SIZE = 18;
constexpr size_t XSTORE_IN_APP_OFFER_TOKEN_MAX_SIZE = 64;
constexpr size_t XSTORE_PRICE_CURRCODE_SIZE = 16;

struct XStoreContext;
using XStoreContextHandle = XStoreContext*;

enum class XStoreProductKind : uint32_t {
    None       = 0x00,
    Consumable = 0x01,
    Durable    = 0x02,
    Game       = 0x04,
    Pass       = 0x08,
    UnmanagedConsumable = 0x10
};
DEFINE_ENUM_FLAG_OPERATORS(XStoreProductKind);

enum class XStoreCanLicenseStatus : uint32_t {
    NotLicensableToUser     = 0,
    Licensable              = 1,
    LicenseActionNotApplicableToProduct = 2
};

struct XStoreGameLicense {
    char skuStoreId[XSTORE_SKU_ID_SIZE];
    bool isActive;
    bool isTrialOwnedByThisUser;
    bool isDiscLicense;
    bool isTrial;
    uint32_t trialTimeRemainingInSeconds;
    char trialUniqueId[64];
    time_t expirationDate;
};

struct XStoreAddonLicense {
    char skuStoreId[XSTORE_SKU_ID_SIZE];
    char inAppOfferToken[XSTORE_IN_APP_OFFER_TOKEN_MAX_SIZE];
    bool isActive;
    time_t expirationDate;
};

struct XStorePrice {
    float basePrice;
    float price;
    float recurrencePrice;
    char currencyCode[XSTORE_PRICE_CURRCODE_SIZE];
    const char* formattedBasePrice;
    const char* formattedPrice;
    const char* formattedRecurrencePrice;
    bool isOnSale;
    time_t saleEndDate;
};

struct XStoreProduct {
    const char* storeId;
    const char* title;
    const char* description;
    const char* language;
    const char* inAppOfferToken;
    const char* linkUri;
    XStoreProductKind productKind;
    XStorePrice price;
    bool hasDigitalDownload;
    bool isInUserCollection;
    uint32_t keywordsCount;
    const char** keywords;
    uint32_t skusCount;
    void* skus;
    uint32_t imagesCount;
    void* images;
    uint32_t videosCount;
    void* videos;
};

struct XStoreCanAcquireLicenseResult {
    char licensableSku[XSTORE_SKU_ID_SIZE];
    XStoreCanLicenseStatus status;
};

struct XStoreAvailability {
    const char* availabilityId;
    XStorePrice price;
    time_t endDate;
};

using XStoreGameLicenseChangedCallback = void(CALLBACK)(void* context);

extern "C" {

HRESULT STDAPICALLTYPE XStoreCreateContext(
    XUserHandle user,
    XStoreContextHandle* storeContextHandle) noexcept;

void STDAPICALLTYPE XStoreCloseContextHandle(
    XStoreContextHandle storeContextHandle) noexcept;

HRESULT STDAPICALLTYPE XStoreQueryGameLicenseAsync(
    XStoreContextHandle storeContextHandle,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XStoreQueryGameLicenseResult(
    XAsyncBlock* async,
    XStoreGameLicense* license) noexcept;

HRESULT STDAPICALLTYPE XStoreQueryAddOnLicensesAsync(
    XStoreContextHandle storeContextHandle,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XStoreQueryAddOnLicensesResultCount(
    XAsyncBlock* async,
    uint32_t* count) noexcept;

HRESULT STDAPICALLTYPE XStoreQueryAddOnLicensesResult(
    XAsyncBlock* async,
    uint32_t count,
    XStoreAddonLicense* addOnLicenses) noexcept;

HRESULT STDAPICALLTYPE XStoreQueryProductForCurrentGameAsync(
    XStoreContextHandle storeContextHandle,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XStoreQueryProductForCurrentGameResult(
    XAsyncBlock* async,
    XStoreProduct* product) noexcept;

HRESULT STDAPICALLTYPE XStoreCanAcquireLicenseForStoreIdAsync(
    XStoreContextHandle storeContextHandle,
    const char* storeProductId,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XStoreCanAcquireLicenseForStoreIdResult(
    XAsyncBlock* async,
    XStoreCanAcquireLicenseResult* storeCanAcquireLicenseResult) noexcept;

HRESULT STDAPICALLTYPE XStoreRegisterGameLicenseChanged(
    XStoreContextHandle storeContextHandle,
    XTaskQueueHandle queue,
    void* context,
    XStoreGameLicenseChangedCallback* callback,
    XTaskQueueRegistrationToken* token) noexcept;

bool STDAPICALLTYPE XStoreUnregisterGameLicenseChanged(
    XStoreContextHandle storeContextHandle,
    XTaskQueueRegistrationToken token,
    bool wait) noexcept;

struct XStoreConsumableResult {
    uint32_t quantity;
};

struct XStoreProductQuery;
using XStoreProductQueryHandle = XStoreProductQuery*;

using XStoreProductQueryCallback = bool(CALLBACK)(const XStoreProduct* product, void* context);

bool STDAPICALLTYPE XStoreIsAvailabilityPurchasable(
    XStoreAvailability availability) noexcept;

HRESULT STDAPICALLTYPE XStoreQueryConsumableBalanceRemainingAsync(
    XStoreContextHandle storeContextHandle,
    const char* storeProductId,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XStoreQueryConsumableBalanceRemainingResult(
    XAsyncBlock* async,
    XStoreConsumableResult* consumableResult) noexcept;

HRESULT STDAPICALLTYPE XStoreReportConsumableFulfillmentAsync(
    XStoreContextHandle storeContextHandle,
    const char* productStoreId,
    uint32_t quantity,
    GUID trackingId,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XStoreReportConsumableFulfillmentResult(
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XStoreShowPurchaseUIAsync(
    XStoreContextHandle storeContextHandle,
    const char* storeId,
    const char* name,
    const char* extendedJsonData,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XStoreShowPurchaseUIResult(
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XStoreQueryAssociatedProductsAsync(
    XStoreContextHandle storeContextHandle,
    XStoreProductKind productKinds,
    uint32_t maxItemsToRetrievePerPage,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XStoreQueryAssociatedProductsResult(
    XAsyncBlock* async,
    XStoreProductQueryHandle* productQueryHandle) noexcept;

HRESULT STDAPICALLTYPE XStoreEnumerateProductsQuery(
    XStoreProductQueryHandle productQueryHandle,
    void* context,
    XStoreProductQueryCallback* callback) noexcept;

void STDAPICALLTYPE XStoreCloseProductsQueryHandle(
    XStoreProductQueryHandle productQueryHandle) noexcept;

} // extern "C"
