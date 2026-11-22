// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Local Persistent Offline XSAPI (Xbox Live C API) header (<xsapi_c.h>).
// Zero Microsoft cloud calls: profiles, achievements, gamerscore, and rich presence
// are persisted atomically to local JSON next to XGameSave storage.

#pragma once

#include "XAsync.h"
#include "XTaskQueue.h"
#include "XUser.h"
#include <cstddef>
#include <cstdint>
#include <ctime>

constexpr size_t XBL_GAMERTAG_CHAR_SIZE         = 16;
constexpr size_t XBL_MODERN_GAMERTAG_CHAR_SIZE  = 100;
constexpr size_t XBL_MODERN_GAMERTAG_SUFFIX_CHAR_SIZE = 16;
constexpr size_t XBL_UNIQUE_MODERN_GAMERTAG_CHAR_SIZE = 104;
constexpr size_t XBL_DISPLAY_NAME_CHAR_SIZE     = 64;
constexpr size_t XBL_DISPLAY_PIC_URL_RAW_CHAR_SIZE = 225;
constexpr size_t XBL_GAMERSCORE_CHAR_SIZE       = 16;
constexpr size_t XBL_ACCOUNT_TIER_CHAR_SIZE     = 32;
constexpr size_t XBL_REPUTATION_CHAR_SIZE       = 32;
constexpr size_t XBL_SCID_LENGTH                = 40;

struct XblContext;
using XblContextHandle = XblContext*;

struct XblAchievementsResult;
using XblAchievementsResultHandle = XblAchievementsResult*;

struct XblInitArgs {
    XTaskQueueHandle queue;
    const char* scid;
};

struct XblUserProfile {
    uint64_t xboxUserId;
    char appDisplayName[XBL_DISPLAY_NAME_CHAR_SIZE];
    char appDisplayPictureResizeUri[XBL_DISPLAY_PIC_URL_RAW_CHAR_SIZE];
    char gameDisplayName[XBL_DISPLAY_NAME_CHAR_SIZE];
    char gameDisplayPictureResizeUri[XBL_DISPLAY_PIC_URL_RAW_CHAR_SIZE];
    char gamerscore[XBL_GAMERSCORE_CHAR_SIZE];
    char gamertag[XBL_GAMERTAG_CHAR_SIZE];
    char modernGamertag[XBL_MODERN_GAMERTAG_CHAR_SIZE];
    char modernGamertagSuffix[XBL_MODERN_GAMERTAG_SUFFIX_CHAR_SIZE];
    char uniqueModernGamertag[XBL_UNIQUE_MODERN_GAMERTAG_CHAR_SIZE];
    char accountTier[XBL_ACCOUNT_TIER_CHAR_SIZE];
    char reputation[XBL_REPUTATION_CHAR_SIZE];
};

enum class XblAchievementType : uint32_t {
    Unknown    = 0,
    All        = 1,
    Persistent = 2,
    Challenge  = 3
};

enum class XblAchievementOrderBy : uint32_t {
    DefaultOrder = 0,
    TitleId      = 1,
    UnlockTime   = 2
};

enum class XblAchievementProgressState : uint32_t {
    Unknown    = 0,
    Achieved   = 1,
    NotStarted = 2,
    InProgress = 3
};

struct XblAchievementProgression {
    uint32_t percentComplete;
    time_t timeUnlocked;
};

struct XblAchievement {
    const char* id;
    const char* serviceConfigurationId;
    const char* name;
    const char* unlockedDescription;
    const char* lockedDescription;
    uint32_t titleId;
    uint32_t gamerscore;
    XblAchievementProgressState progressState;
    XblAchievementProgression progression;
    bool isSecret;
};

enum class XblPresenceUserState : uint32_t {
    Unknown = 0,
    Online  = 1,
    Away    = 2,
    Offline = 3
};

struct XblPresenceRichPresenceIds {
    char scid[XBL_SCID_LENGTH];
    const char* presenceId;
    const char** presenceTokenIds;
    size_t presenceTokenIdsCount;
};

struct XblPresenceRecord {
    uint64_t xuid;
    XblPresenceUserState userState;
    uint32_t titleId;
    char richPresenceId[64];
    time_t lastUpdated;
};

enum class XblPermission : uint32_t {
    Unknown                      = 0,
    CommunicateUsingText         = 1,
    CommunicateUsingVideo        = 2,
    CommunicateUsingVoice        = 3,
    ViewTargetProfile            = 4,
    ViewTargetGameHistory        = 5,
    ViewTargetPresence           = 6,
    PlayMultiplayer              = 7,
    ViewTargetUserCreatedContent = 8
};

struct XblPermissionCheckResult {
    bool isAllowed;
    uint64_t targetXuid;
    XblPermission permissionRequested;
    uint32_t reasonsCount;
};

enum class XblVerifyStringResultCode : uint32_t {
    Success          = 0,
    Offensive        = 1,
    TooLong          = 2,
    UnknownError     = 3
};

struct XblVerifyStringResult {
    XblVerifyStringResultCode resultCode;
    const char* firstOffendingSubstring;
};

enum class XblTitleManagedStatType : uint32_t {
    Number = 0,
    String = 1
};

struct XblTitleManagedStatistic {
    const char* statisticName;
    XblTitleManagedStatType statisticType;
    double numberValue;
    const char* stringValue;
};

struct XblStatistic {
    const char* statisticName;
    const char* statisticType;
    const char* value;
};

struct XblServiceConfigurationStatistic {
    char serviceConfigurationId[XBL_SCID_LENGTH];
    XblStatistic* statistics;
    uint32_t statisticsCount;
};

struct XblUserStatisticsResult {
    uint64_t xboxUserId;
    XblServiceConfigurationStatistic* serviceConfigStatistics;
    uint32_t serviceConfigStatisticsCount;
};

struct XblLeaderboardQuery {
    uint64_t xboxUserId;
    char scid[XBL_SCID_LENGTH];
    const char* leaderboardName;
    const char* statName;
    uint32_t maxItems;
    uint32_t skipResultToRank;
};

struct XblLeaderboardRow {
    char gamertag[XBL_GAMERTAG_CHAR_SIZE];
    char modernGamertag[XBL_MODERN_GAMERTAG_CHAR_SIZE];
    uint64_t xboxUserId;
    double percentile;
    uint32_t rank;
    const char** columnValues;
    size_t columnValuesCount;
};

struct XblLeaderboardResult {
    uint32_t totalRowCount;
    XblLeaderboardRow* rows;
    size_t rowsCount;
    bool hasNext;
};

struct XblSocialRelationship {
    uint64_t xboxUserId;
    bool isFavorite;
    bool isFollowingCaller;
    bool isFriend;
};

struct XblSocialRelationshipResult;
using XblSocialRelationshipResultHandle = XblSocialRelationshipResult*;

struct XblMultiplayerEvent {
    HRESULT Result;
    uint32_t EventType;
    uint32_t SessionType;
};

extern "C" {

HRESULT STDAPICALLTYPE XblInitialize(
    const XblInitArgs* args) noexcept;

void STDAPICALLTYPE XblCleanup() noexcept;

HRESULT STDAPICALLTYPE XblCleanupAsync(
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XblContextCreateHandle(
    XUserHandle user,
    XblContextHandle* context) noexcept;

void STDAPICALLTYPE XblContextCloseHandle(
    XblContextHandle xboxLiveContextHandle) noexcept;

HRESULT STDAPICALLTYPE XblContextDuplicateHandle(
    XblContextHandle xboxLiveContextHandle,
    XblContextHandle* duplicatedHandle) noexcept;

HRESULT STDAPICALLTYPE XblContextGetXboxUserId(
    XblContextHandle xboxLiveContextHandle,
    uint64_t* xuid) noexcept;

HRESULT STDAPICALLTYPE XblProfileGetUserProfileAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XblProfileGetUserProfileResult(
    XAsyncBlock* async,
    XblUserProfile* profile) noexcept;

HRESULT STDAPICALLTYPE XblProfileGetUserProfilesAsync(
    XblContextHandle xboxLiveContext,
    const uint64_t* xboxUserIds,
    size_t xboxUserIdsCount,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XblProfileGetUserProfilesResultCount(
    XAsyncBlock* async,
    size_t* profileCount) noexcept;

HRESULT STDAPICALLTYPE XblProfileGetUserProfilesResult(
    XAsyncBlock* async,
    size_t profilesCount,
    XblUserProfile* profiles) noexcept;

HRESULT STDAPICALLTYPE XblAchievementsGetAchievementsForTitleIdAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    uint32_t titleId,
    XblAchievementType type,
    bool unlockedOnly,
    XblAchievementOrderBy orderBy,
    uint32_t skipItems,
    uint32_t maxItems,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XblAchievementsGetAchievementsForTitleIdResult(
    XAsyncBlock* async,
    XblAchievementsResultHandle* result) noexcept;

HRESULT STDAPICALLTYPE XblAchievementsResultGetAchievements(
    XblAchievementsResultHandle resultHandle,
    const XblAchievement** achievements,
    size_t* achievementsCount) noexcept;

HRESULT STDAPICALLTYPE XblAchievementsResultHasNext(
    XblAchievementsResultHandle resultHandle,
    bool* hasNext) noexcept;

void STDAPICALLTYPE XblAchievementsResultCloseHandle(
    XblAchievementsResultHandle resultHandle) noexcept;

HRESULT STDAPICALLTYPE XblAchievementsUpdateAchievementAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const char* achievementId,
    uint32_t percentComplete,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XblPresenceSetPresenceAsync(
    XblContextHandle xboxLiveContext,
    bool isUserActiveInTitle,
    const XblPresenceRichPresenceIds* richPresenceIds,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XblPresenceGetPresenceAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xuid,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XblPresenceGetPresenceResult(
    XAsyncBlock* async,
    XblPresenceRecord* presence) noexcept;

HRESULT STDAPICALLTYPE XblPrivacyCheckPermissionAsync(
    XblContextHandle xboxLiveContext,
    XblPermission permissionToCheck,
    uint64_t targetXuid,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XblPrivacyCheckPermissionResult(
    XAsyncBlock* async,
    XblPermissionCheckResult* result) noexcept;

HRESULT STDAPICALLTYPE XblStringVerifyStringAsync(
    XblContextHandle xboxLiveContext,
    const char* stringToVerify,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XblStringVerifyStringResultSize(
    XAsyncBlock* async,
    size_t* resultSize) noexcept;

HRESULT STDAPICALLTYPE XblStringVerifyStringResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblVerifyStringResult** ptrToBuffer,
    size_t* bufferUsed) noexcept;

HRESULT STDAPICALLTYPE XblTitleManagedStatsWriteAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const XblTitleManagedStatistic* statistics,
    size_t statisticsCount,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XblTitleManagedStatsUpdateStatsAsync(
    XblContextHandle xboxLiveContext,
    const XblTitleManagedStatistic* statistics,
    size_t statisticsCount,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XblTitleManagedStatsDeleteStatsAsync(
    XblContextHandle xboxLiveContext,
    const char** statisticNames,
    size_t statisticNamesCount,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XblUserStatisticsGetSingleUserStatisticAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const char* serviceConfigurationId,
    const char* statisticName,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XblUserStatisticsGetSingleUserStatisticResultSize(
    XAsyncBlock* async,
    size_t* resultSizeInBytes) noexcept;

HRESULT STDAPICALLTYPE XblUserStatisticsGetSingleUserStatisticResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblUserStatisticsResult** ptrToBuffer,
    size_t* bufferUsed) noexcept;

HRESULT STDAPICALLTYPE XblLeaderboardGetLeaderboardAsync(
    XblContextHandle xboxLiveContext,
    XblLeaderboardQuery leaderboardQuery,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XblLeaderboardGetLeaderboardResultSize(
    XAsyncBlock* async,
    size_t* resultSizeInBytes) noexcept;

HRESULT STDAPICALLTYPE XblLeaderboardGetLeaderboardResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblLeaderboardResult** ptrToBuffer,
    size_t* bufferUsed) noexcept;

HRESULT STDAPICALLTYPE XblSocialGetSocialRelationshipsAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    uint32_t socialRelationshipFilter,
    size_t startIndex,
    size_t maxItems,
    XAsyncBlock* async) noexcept;

HRESULT STDAPICALLTYPE XblSocialGetSocialRelationshipsResult(
    XAsyncBlock* async,
    XblSocialRelationshipResultHandle* handle) noexcept;

HRESULT STDAPICALLTYPE XblSocialRelationshipResultGetRelationships(
    XblSocialRelationshipResultHandle resultHandle,
    const XblSocialRelationship** relationships,
    size_t* relationshipsCount) noexcept;

void STDAPICALLTYPE XblSocialRelationshipResultCloseHandle(
    XblSocialRelationshipResultHandle resultHandle) noexcept;

HRESULT STDAPICALLTYPE XblMultiplayerManagerInitialize(
    const char* lobbySessionTemplateName,
    XTaskQueueHandle asyncQueue) noexcept;

HRESULT STDAPICALLTYPE XblMultiplayerManagerDoWork(
    const XblMultiplayerEvent** multiplayerEvents,
    size_t* multiplayerEventsCount) noexcept;

size_t STDAPICALLTYPE XblMultiplayerManagerLobbySessionLocalMembersCount() noexcept;

} // extern "C"
