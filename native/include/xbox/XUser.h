// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox GDK User Profile & Identity API header (<XUser.h>).

#pragma once

#include "XAsync.h"
#include "XTaskQueue.h"
#include <cstddef>
#include <cstdint>

struct XUser;
using XUserHandle = XUser*;

struct XUserLocalId {
    uint64_t value;
};

constexpr XUserLocalId XUserNullUserLocalId = {0ULL};

constexpr size_t XUserMaxClassicGamertagBytes      = 16;
constexpr size_t XUserMaxModernGamertagBytes       = 97;
constexpr size_t XUserMaxModernGamertagSuffixBytes = 15;
constexpr size_t XUserMaxUniqueModernGamertagBytes = 101;

enum class XUserAddOptions : uint32_t {
    None                     = 0x00,
    AddDefaultUserSilently   = 0x01,
    AllowGuests              = 0x02,
    AddDefaultUserAllowingUI = 0x04
};
DEFINE_ENUM_FLAG_OPERATORS(XUserAddOptions);

enum class XUserState : uint32_t {
    SignedIn   = 0,
    SigningOut = 1,
    SignedOut  = 2
};

enum class XUserGamertagComponent : uint32_t {
    Classic      = 0,
    Modern       = 1,
    ModernSuffix = 2,
    UniqueModern = 3
};

enum class XUserAgeGroup : uint32_t {
    Unknown = 0,
    Child   = 1,
    Teen    = 2,
    Adult   = 3
};

enum class XUserPrivilege : uint32_t {
    CrossPlay            = 185,
    Sessions             = 189,
    Broadcast            = 190,
    ManageProfilePrivacy = 196,
    CloudGamingManageSession = 207,
    CloudGamingJoinSession   = 208,
    CloudSavedGames      = 209,
    SocialNetworkSharing = 220,
    UserGeneratedContent = 247,
    Communications       = 252,
    Multiplayer          = 254,
    AddFriends           = 255
};

enum class XUserPrivilegeOptions : uint32_t {
    None            = 0x00,
    AllUsers        = 0x01
};
DEFINE_ENUM_FLAG_OPERATORS(XUserPrivilegeOptions);

enum class XUserPrivilegeDenyReason : uint32_t {
    None                = 0,
    PurchaseRequired    = 1,
    Restricted          = 2,
    Banned              = 3,
    Unknown             = 0xFFFFFFFFu
};

enum class XUserChangeEvent : uint32_t {
    SignedInAgain = 0,
    SigningOut    = 1,
    SignedOut     = 2,
    Gamertag      = 3,
    Gamerpicture  = 4,
    Privileges    = 5
};

using XUserChangeEventCallback = void(CALLBACK)(
    void* context,
    XUserLocalId userLocalId,
    XUserChangeEvent event);

enum class XUserGetTokenAndSignatureOptions : uint32_t {
    None         = 0x00,
    ForceRefresh = 0x01,
    AllUsers     = 0x02
};
DEFINE_ENUM_FLAG_OPERATORS(XUserGetTokenAndSignatureOptions);

struct XUserGetTokenAndSignatureHttpHeader {
    const char* name;
    const char* value;
};

struct XUserGetTokenAndSignatureData {
    size_t tokenSize;
    size_t signatureSize;
    const char* token;
    const char* signature;
};

struct XUserGetTokenAndSignatureUtf16HttpHeader {
    const wchar_t* name;
    const wchar_t* value;
};

struct XUserGetTokenAndSignatureUtf16Data {
    size_t tokenCount;
    size_t signatureCount;
    const wchar_t* token;
    const wchar_t* signature;
};

enum class XUserGamerPictureSize : uint32_t {
    Small      = 0,
    Medium     = 1,
    Large      = 2,
    ExtraLarge = 3
};

struct XUserSignOutDeferral;
using XUserSignOutDeferralHandle = XUserSignOutDeferral*;

extern "C" {

HRESULT STDAPICALLTYPE XUserAddAsync(
    XUserAddOptions options,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XUserAddResult(
    XAsyncBlock* async,
    XUserHandle* newUser) noexcept;

HRESULT STDAPICALLTYPE XUserDuplicateHandle(
    XUserHandle handle,
    XUserHandle* duplicatedHandle) noexcept;

void STDAPICALLTYPE XUserCloseHandle(
    XUserHandle user) noexcept;

int32_t STDAPICALLTYPE XUserCompare(
    XUserHandle user1,
    XUserHandle user2) noexcept;

HRESULT STDAPICALLTYPE XUserGetMaxUsers(
    uint32_t* maxUsers) noexcept;

HRESULT STDAPICALLTYPE XUserGetId(
    XUserHandle user,
    uint64_t* userId) noexcept;

HRESULT STDAPICALLTYPE XUserGetLocalId(
    XUserHandle user,
    XUserLocalId* userLocalId) noexcept;

HRESULT STDAPICALLTYPE XUserFindUserById(
    uint64_t userId,
    XUserHandle* handle) noexcept;

HRESULT STDAPICALLTYPE XUserFindUserByLocalId(
    XUserLocalId userLocalId,
    XUserHandle* handle) noexcept;

HRESULT STDAPICALLTYPE XUserGetState(
    XUserHandle user,
    XUserState* state) noexcept;

HRESULT STDAPICALLTYPE XUserGetGamertag(
    XUserHandle user,
    XUserGamertagComponent gamertagComponent,
    size_t gamertagSize,
    char* gamertag,
    size_t* gamertagUsed) noexcept;

HRESULT STDAPICALLTYPE XUserGetAgeGroup(
    XUserHandle user,
    XUserAgeGroup* ageGroup) noexcept;

HRESULT STDAPICALLTYPE XUserCheckPrivilege(
    XUserHandle user,
    XUserPrivilegeOptions options,
    XUserPrivilege privilege,
    bool* hasPrivilege,
    XUserPrivilegeDenyReason* reason) noexcept;

HRESULT STDAPICALLTYPE XUserIsStoreUser(
    XUserHandle user,
    bool* isStoreUser) noexcept;

HRESULT STDAPICALLTYPE XUserRegisterForChangeEvent(
    XTaskQueueHandle queue,
    void* context,
    XUserChangeEventCallback* callback,
    XTaskQueueRegistrationToken* token) noexcept;

bool STDAPICALLTYPE XUserUnregisterForChangeEvent(
    XTaskQueueRegistrationToken token,
    bool wait) noexcept;

HRESULT STDAPICALLTYPE XUserGetTokenAndSignatureAsync(
    XUserHandle user,
    XUserGetTokenAndSignatureOptions options,
    const char* method,
    const char* url,
    size_t headerCount,
    const XUserGetTokenAndSignatureHttpHeader* headers,
    size_t bodySize,
    const void* bodyBuffer,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XUserGetTokenAndSignatureResultSize(
    XAsyncBlock* async,
    size_t* bufferSize) noexcept;

HRESULT STDAPICALLTYPE XUserGetTokenAndSignatureResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XUserGetTokenAndSignatureData** ptrToBuffer,
    size_t* bufferUsed) noexcept;

HRESULT STDAPICALLTYPE XUserGetTokenAndSignatureUtf16Async(
    XUserHandle user,
    XUserGetTokenAndSignatureOptions options,
    const wchar_t* method,
    const wchar_t* url,
    size_t headerCount,
    const XUserGetTokenAndSignatureUtf16HttpHeader* headers,
    size_t bodySize,
    const void* bodyBuffer,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XUserGetTokenAndSignatureUtf16ResultSize(
    XAsyncBlock* async,
    size_t* bufferSize) noexcept;

HRESULT STDAPICALLTYPE XUserGetTokenAndSignatureUtf16Result(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XUserGetTokenAndSignatureUtf16Data** ptrToBuffer,
    size_t* bufferUsed) noexcept;

HRESULT STDAPICALLTYPE XUserGetGamerPictureAsync(
    XUserHandle user,
    XUserGamerPictureSize pictureSize,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XUserGetGamerPictureResultSize(
    XAsyncBlock* async,
    size_t* bufferSize) noexcept;

HRESULT STDAPICALLTYPE XUserGetGamerPictureResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    size_t* bufferUsed) noexcept;

HRESULT STDAPICALLTYPE XUserResolvePrivilegeWithUiAsync(
    XUserHandle user,
    XUserPrivilegeOptions options,
    XUserPrivilege privilege,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XUserResolvePrivilegeWithUiResult(
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XUserResolveIssueWithUiUtf16Async(
    XUserHandle user,
    const wchar_t* url,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XUserResolveIssueWithUiUtf16Result(
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XUserGetSignOutDeferral(
    XUserSignOutDeferralHandle* deferral) noexcept;

void STDAPICALLTYPE XUserCloseSignOutDeferralHandle(
    XUserSignOutDeferralHandle deferral) noexcept;

HRESULT STDAPICALLTYPE XUserAddByIdWithUiAsync(
    uint64_t userId,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XUserAddByIdWithUiResult(
    XAsyncBlock* async,
    XUserHandle* newUser) noexcept;

HRESULT STDAPICALLTYPE WsxDispatchUserChangeEvent(
    XUserLocalId userLocalId,
    XUserChangeEvent event) noexcept;

} // extern "C"
