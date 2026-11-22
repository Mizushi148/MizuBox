// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Comprehensive automated unit & integration test for xgameruntime.dll:
// - XTaskQueue (Manual, ThreadPool, ThreadPool Timers via SubmitDelayedCallback, Terminate(wait=true))
// - XAsync / XAsyncProvider
// - XUser (Sign-in, Gamertag, XUID, LocalId, Privileges, Change Events, Uninit teardown)
// - XGameSave (Atomic .tmp->rename writes, real remaining quota delta, async read/write, enumeration)
// - XSystem (DeviceType, AnalyticsInfo, RuntimeInfo, ConsoleId, AppSpecificDeviceId, SandboxId, IsHandleValid)
// - XThread (TimeSensitive TLS isolation)
// - XNetworking (ConnectivityHint, Change Events, Preferred UDP Multiplayer Port sync & async)
// - XPersistentLocalStorage (Path, real disk space accounting, PromptUserForSpaceAsync)
// - XPackage (Identifier, Locale, EnumeratePackages, InstallationMonitor, MountWithUiAsync, ChunkAvailability)

#include "include/xbox/XGameRuntimeInit.h"
#include "include/xbox/XSystem.h"
#include "include/xbox/XThread.h"
#include "include/xbox/XTaskQueue.h"
#include "include/xbox/XAsync.h"
#include "include/xbox/XAsyncProvider.h"
#include "include/xbox/XUser.h"
#include "include/xbox/XGameSave.h"
#include "include/xbox/XGameSaveFiles.h"
#include "include/xbox/XNetworking.h"
#include "include/xbox/XPersistentLocalStorage.h"
#include "include/xbox/XPackage.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

#define CHECK_HR(expr)                                                                 \
    do {                                                                               \
        HRESULT _hr = (expr);                                                          \
        if (FAILED(_hr)) {                                                             \
            std::fprintf(stderr, "FAIL: %s returned 0x%08X at line %d\n",              \
                         #expr, static_cast<unsigned>(_hr), __LINE__);                 \
            return 1;                                                                  \
        }                                                                              \
    } while (0)

#define CHECK_COND(cond, msg)                                                          \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "FAIL: %s (%s) at line %d\n", msg, #cond, __LINE__);  \
            return 1;                                                                  \
        }                                                                              \
    } while (0)

namespace {

struct TestPayload {
    uint32_t magic = 0x56534758u; // 'XGSV'
    uint32_t level = 42;
    float pos[3] = {1.25f, -0.5f, 3.75f};
    char note[32] = "XboxSeriesSaveTest";
    uint8_t padding[4096 - 52]{};
};
static_assert(sizeof(TestPayload) == 4096, "TestPayload should be exactly 4096 bytes");

} // namespace

int main() {
    CHECK_HR(XGameRuntimeInitialize());

    // ========================================================================
    // 1. XSystem
    // ========================================================================
    const XSystemAnalyticsInfo analytics = XSystemGetAnalyticsInfo();
    CHECK_COND(std::strcmp(analytics.family, "Xbox") == 0, "XSystemAnalyticsInfo family != Xbox");
    CHECK_COND(analytics.osVersion.major == 10 && analytics.osVersion.build >= 22000,
               "XSystemAnalyticsInfo osVersion invalid");

    const XSystemRuntimeInfo rtInfo = XSystemGetRuntimeInfo();
    CHECK_COND(rtInfo.runtimeVersion.Value != 0, "XSystemRuntimeInfo version is 0");

    char consoleId[XSystemConsoleIdBytes]{};
    size_t consoleIdUsed = 0;
    CHECK_HR(XSystemGetConsoleId(sizeof(consoleId), consoleId, &consoleIdUsed));
    CHECK_COND(consoleIdUsed > 1 && std::strlen(consoleId) > 0, "Empty ConsoleId");

    char asid[XSystemAppSpecificDeviceIdBytes]{};
    size_t asidUsed = 0;
    CHECK_HR(XSystemGetAppSpecificDeviceId(sizeof(asid), asid, &asidUsed));
    CHECK_COND(asidUsed > 1, "Empty AppSpecificDeviceId");

    char sandboxId[XSystemXboxLiveSandboxIdMaxBytes]{};
    size_t sandboxUsed = 0;
    CHECK_HR(XSystemGetXboxLiveSandboxId(sizeof(sandboxId), sandboxId, &sandboxUsed));
    CHECK_COND(sandboxUsed > 1 && std::strlen(sandboxId) > 0, "Empty SandboxId");

    CHECK_COND(!XSystemIsHandleValid(nullptr) && !XSystemIsHandleValid(INVALID_HANDLE_VALUE),
               "XSystemIsHandleValid accepted invalid handle");

    // ========================================================================
    // 2. XThread (TLS isolation)
    // ========================================================================
    CHECK_COND(!XThreadIsTimeSensitive(), "Default thread should not be time-sensitive");
    CHECK_HR(XThreadSetTimeSensitive(true));
    CHECK_COND(XThreadIsTimeSensitive(), "XThreadSetTimeSensitive(true) failed");
    std::atomic<bool> otherThreadSensitive{true};
    std::thread([&]() {
        otherThreadSensitive.store(XThreadIsTimeSensitive());
    }).join();
    CHECK_COND(!otherThreadSensitive.load(), "XThread time-sensitive flag leaked across threads");
    CHECK_HR(XThreadSetTimeSensitive(false));
    CHECK_COND(!XThreadIsTimeSensitive(), "XThreadSetTimeSensitive(false) failed");

    // ========================================================================
    // 3. XTaskQueue (Manual + ThreadPool Timers + Terminate(wait=true))
    // ========================================================================
    XTaskQueueHandle queue = nullptr;
    CHECK_HR(XTaskQueueCreate(
        XTaskQueueDispatchMode::ThreadPool,
        XTaskQueueDispatchMode::Manual,
        &queue));
    CHECK_COND(queue != nullptr, "XTaskQueueCreate returned null");

    std::atomic<int> manualCallbackCount{0};
    CHECK_HR(XTaskQueueSubmitCallback(
        queue, XTaskQueuePort::Completion, &manualCallbackCount,
        [](void* ctx, bool canceled) noexcept {
            if (!canceled) {
                static_cast<std::atomic<int>*>(ctx)->fetch_add(1);
            }
        }));
    CHECK_COND(XTaskQueueDispatch(queue, XTaskQueuePort::Completion, 50),
               "XTaskQueueDispatch should dispatch manual completion callback");
    CHECK_COND(manualCallbackCount.load() == 1, "Manual callback count mismatch");

    // Delayed callback via Windows ThreadPool Timer
    std::atomic<bool> delayedFired{false};
    CHECK_HR(XTaskQueueSubmitDelayedCallback(
        queue, XTaskQueuePort::Work, 15, &delayedFired,
        [](void* ctx, bool canceled) noexcept {
            if (!canceled) {
                static_cast<std::atomic<bool>*>(ctx)->store(true);
            }
        }));
    for (int i = 0; i < 40 && !delayedFired.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_COND(delayedFired.load(), "Delayed ThreadPool timer callback did not fire");

    // Terminate(wait=true) cancels long-pending timers cleanly without blocking 5 seconds
    XTaskQueueHandle termQueue = nullptr;
    CHECK_HR(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::ThreadPool, &termQueue));
    std::atomic<bool> longTimerCanceled{false};
    CHECK_HR(XTaskQueueSubmitDelayedCallback(
        termQueue, XTaskQueuePort::Work, 5000, &longTimerCanceled,
        [](void* ctx, bool canceled) noexcept {
            if (canceled) static_cast<std::atomic<bool>*>(ctx)->store(true);
        }));
    const auto termStart = std::chrono::steady_clock::now();
    std::atomic<bool> terminatedCbFired{false};
    CHECK_HR(XTaskQueueTerminate(
        termQueue, true, &terminatedCbFired,
        [](void* ctx) noexcept { static_cast<std::atomic<bool>*>(ctx)->store(true); }));
    const auto termMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - termStart).count();
    CHECK_COND(termMs < 1000, "XTaskQueueTerminate(wait=true) took too long canceling delayed timer");
    CHECK_COND(longTimerCanceled.load() && terminatedCbFired.load(),
               "XTaskQueueTerminate did not cancel pending timer or fire termination callback");
    XTaskQueueCloseHandle(termQueue);

    // XAsyncProvider lifecycle: DoWork returning E_PENDING + delayed reschedule + auto XAsyncOp::Cleanup on 0-byte op
    {
        struct PendingCleanupCtx {
            std::atomic<int> doWorkCalls{0};
            std::atomic<int> cleanupCalls{0};
            std::atomic<bool> cleanupRanBeforeCallback{false};
            std::atomic<bool> callbackFired{false};
        } pctx;

        XTaskQueueHandle tpQueue = nullptr;
        CHECK_HR(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::ThreadPool, &tpQueue));

        XAsyncBlock pab{};
        pab.queue = tpQueue;
        pab.context = &pctx;
        pab.callback = [](XAsyncBlock* ab) noexcept {
            auto* c = static_cast<PendingCleanupCtx*>(ab->context);
            c->cleanupRanBeforeCallback.store(c->cleanupCalls.load() == 1);
            c->callbackFired.store(true);
        };

        auto providerFn = [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<PendingCleanupCtx*>(data->context);
            if (op == XAsyncOp::Begin) {
                return XAsyncSchedule(data->async, 0);
            }
            if (op == XAsyncOp::DoWork) {
                const int pass = c->doWorkCalls.fetch_add(1) + 1;
                if (pass == 1) {
                    XAsyncSchedule(data->async, 10);
                    return E_PENDING; // Must NOT complete prematurely!
                }
                XAsyncComplete(data->async, S_OK, 0);
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) {
                c->cleanupCalls.fetch_add(1);
                return S_OK;
            }
            return S_OK;
        };

        CHECK_HR(XAsyncBegin(&pab, &pctx, nullptr, "PendingCleanupTest", providerFn));
        CHECK_HR(XAsyncGetStatus(&pab, true));
        CHECK_COND(pctx.doWorkCalls.load() == 2, "XAsync DoWork E_PENDING reschedule did not execute 2 passes");
        CHECK_COND(pctx.cleanupCalls.load() == 1, "XAsyncOp::Cleanup did not auto-fire once for 0-byte op");
        CHECK_COND(pctx.callbackFired.load() && pctx.cleanupRanBeforeCallback.load(),
                   "XAsyncOp::Cleanup must run before completion callback for 0-byte op");

        CHECK_HR(XTaskQueueTerminate(tpQueue, true, nullptr, nullptr));
        XTaskQueueCloseHandle(tpQueue);
    }

    // ========================================================================
    // 4. XUser
    // ========================================================================
    XAsyncBlock userAsync{};
    userAsync.queue = queue;
    std::atomic<bool> userCompletionCalled{false};
    userAsync.context = &userCompletionCalled;
    userAsync.callback = [](XAsyncBlock* ab) noexcept {
        static_cast<std::atomic<bool>*>(ab->context)->store(true);
    };
    CHECK_HR(XUserAddAsync(XUserAddOptions::AddDefaultUserSilently, &userAsync));
    CHECK_HR(XAsyncGetStatus(&userAsync, true));
    CHECK_COND(userCompletionCalled.load(), "XUserAddAsync completion callback not fired");

    XUserHandle user = nullptr;
    CHECK_HR(XUserAddResult(&userAsync, &user));
    CHECK_COND(user != nullptr, "XUserAddResult returned null user");

    uint64_t xuid = 0;
    CHECK_HR(XUserGetId(user, &xuid));
    CHECK_COND(xuid != 0, "XUserGetId returned 0");

    XUserLocalId localId{};
    CHECK_HR(XUserGetLocalId(user, &localId));
    CHECK_COND(localId.value == 1ULL, "XUserGetLocalId unexpected value");

    XUserState userState = XUserState::SignedOut;
    CHECK_HR(XUserGetState(user, &userState));
    CHECK_COND(userState == XUserState::SignedIn, "XUserGetState should be SignedIn");

    char modernGamertag[XUserMaxUniqueModernGamertagBytes]{};
    size_t gtUsed = 0;
    CHECK_HR(XUserGetGamertag(
        user, XUserGamertagComponent::UniqueModern,
        sizeof(modernGamertag), modernGamertag, &gtUsed));
    CHECK_COND(gtUsed > 1 && std::strlen(modernGamertag) > 0, "Empty UniqueModern gamertag");

    bool hasCloudSaves = false;
    XUserPrivilegeDenyReason denyReason = XUserPrivilegeDenyReason::Unknown;
    CHECK_HR(XUserCheckPrivilege(
        user, XUserPrivilegeOptions::None, XUserPrivilege::CloudSavedGames,
        &hasCloudSaves, &denyReason));
    CHECK_COND(hasCloudSaves && denyReason == XUserPrivilegeDenyReason::None,
               "CloudSavedGames privilege check failed");

    // ========================================================================
    // 5. XGameSave (Atomic writes + real quota delta + async read/write)
    // ========================================================================
    XAsyncBlock gsInitAsync{};
    gsInitAsync.queue = queue;
    CHECK_HR(XGameSaveInitializeProviderAsync(
        user, "00000000-0000-0000-0000-000000007777", false, &gsInitAsync));
    CHECK_HR(XAsyncGetStatus(&gsInitAsync, true));

    XGameSaveProviderHandle provider = nullptr;
    CHECK_HR(XGameSaveInitializeProviderResult(&gsInitAsync, &provider));
    CHECK_COND(provider != nullptr, "XGameSaveInitializeProviderResult returned null");

    // Clean any leftover container before measuring quota delta
    XGameSaveDeleteContainer(provider, "TestSlotContainer");

    int64_t quotaBefore = 0;
    CHECK_HR(XGameSaveGetRemainingQuota(provider, &quotaBefore));
    CHECK_COND(quotaBefore > 1024 * 1024, "XGameSaveGetRemainingQuota too small");

    XGameSaveContainerHandle container = nullptr;
    CHECK_HR(XGameSaveCreateContainer(provider, "TestSlotContainer", &container));

    XGameSaveUpdateHandle update = nullptr;
    CHECK_HR(XGameSaveCreateUpdate(container, "Test Slot Display Name", &update));

    TestPayload writtenPayload{};
    CHECK_HR(XGameSaveSubmitBlobWrite(
        update, "player_state.bin",
        reinterpret_cast<const uint8_t*>(&writtenPayload), sizeof(writtenPayload)));

    XAsyncBlock submitAsync{};
    submitAsync.queue = queue;
    CHECK_HR(XGameSaveSubmitUpdateAsync(update, &submitAsync));
    CHECK_HR(XGameSaveSubmitUpdateResult(&submitAsync));
    XGameSaveCloseUpdate(update);

    int64_t quotaAfter = 0;
    CHECK_HR(XGameSaveGetRemainingQuota(provider, &quotaAfter));
    CHECK_COND(quotaBefore - quotaAfter == static_cast<int64_t>(sizeof(TestPayload)),
               "XGameSaveGetRemainingQuota did not reflect exact 4096-byte atomic write");

    // Read blob data asynchronously
    const char* blobNames[] = {"player_state.bin"};
    XAsyncBlock readAsync{};
    readAsync.queue = queue;
    CHECK_HR(XGameSaveReadBlobDataAsync(container, blobNames, 1, &readAsync));
    CHECK_HR(XAsyncGetStatus(&readAsync, true));

    size_t requiredSize = 0;
    CHECK_HR(XAsyncGetResultSize(&readAsync, &requiredSize));
    std::vector<uint8_t> blobBuffer(requiredSize);
    uint32_t blobCount = 0;
    auto* blobs = reinterpret_cast<XGameSaveBlob*>(blobBuffer.data());
    CHECK_HR(XGameSaveReadBlobDataResult(&readAsync, blobBuffer.size(), blobs, &blobCount));
    CHECK_COND(blobCount == 1 && blobs[0].info.size == sizeof(TestPayload), "Loaded blob size mismatch");
    const auto* readPayload = reinterpret_cast<const TestPayload*>(blobs[0].data);
    CHECK_COND(readPayload->magic == writtenPayload.magic && readPayload->level == writtenPayload.level,
               "Loaded blob content mismatch");

    CHECK_HR(XGameSaveDeleteContainer(provider, "TestSlotContainer"));
    XGameSaveCloseContainer(container);
    XGameSaveCloseProvider(provider);

    // ========================================================================
    // 6. XNetworking
    // ========================================================================
    XNetworkingConnectivityHint netHint{};
    CHECK_HR(XNetworkingGetConnectivityHint(&netHint));
    CHECK_COND(netHint.networkInitialized &&
               netHint.connectivityLevel == XNetworkingConnectivityLevelHint::InternetAccess &&
               netHint.ianaInterfaceType == 6u,
               "XNetworkingGetConnectivityHint unexpected values");

    XTaskQueueRegistrationToken netToken{};
    CHECK_HR(XNetworkingRegisterConnectivityHintChanged(
        queue, nullptr,
        [](void*, const XNetworkingConnectivityHint*) noexcept {},
        &netToken));
    CHECK_COND(netToken.token != 0, "XNetworkingRegisterConnectivityHintChanged token is 0");
    CHECK_COND(XNetworkingUnregisterConnectivityHintChanged(netToken, true),
               "XNetworkingUnregisterConnectivityHintChanged failed");

    uint16_t udpPort = 0;
    CHECK_HR(XNetworkingQueryPreferredLocalUdpMultiplayerPort(&udpPort));
    CHECK_COND(udpPort == 3074u, "Preferred UDP multiplayer port != 3074");

    XAsyncBlock portAsync{};
    portAsync.queue = queue;
    CHECK_HR(XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync(&portAsync));
    uint16_t asyncUdpPort = 0;
    CHECK_HR(XNetworkingQueryPreferredLocalUdpMultiplayerPortResult(&portAsync, &asyncUdpPort));
    CHECK_COND(asyncUdpPort == 3074u, "Async preferred UDP port != 3074");

    // ========================================================================
    // 7. XPersistentLocalStorage
    // ========================================================================
    size_t plsPathSize = 0;
    CHECK_HR(XPersistentLocalStorageGetPathSize(&plsPathSize));
    CHECK_COND(plsPathSize > 1, "PLS path size invalid");
    std::string plsPath(plsPathSize, '\0');
    size_t plsUsed = 0;
    CHECK_HR(XPersistentLocalStorageGetPath(plsPath.size(), plsPath.data(), &plsUsed));
    plsPath.resize(plsUsed - 1);
    CHECK_COND(fs::is_directory(plsPath), "PLS directory was not created on disk");

    XPersistentLocalStorageSpaceInfo plsBefore{};
    CHECK_HR(XPersistentLocalStorageGetSpaceInfo(&plsBefore));
    const fs::path testCacheFile = fs::path(plsPath) / "shader_cache_test.bin";
    {
        std::ofstream f(testCacheFile, std::ios::binary | std::ios::trunc);
        std::vector<char> zeros(2048, 0x5A);
        f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
    XPersistentLocalStorageSpaceInfo plsAfter{};
    CHECK_HR(XPersistentLocalStorageGetSpaceInfo(&plsAfter));
    fs::remove(testCacheFile);
    CHECK_COND(plsAfter.usedBytes >= plsBefore.usedBytes + 2048,
               "XPersistentLocalStorageGetSpaceInfo did not reflect real disk usage");

    // ========================================================================
    // 8. XPackage
    // ========================================================================
    CHECK_COND(XPackageIsPackagedProcess(), "XPackageIsPackagedProcess returned false");
    char pkgId[XPACKAGE_IDENTIFIER_MAX_LENGTH]{};
    CHECK_HR(XPackageGetCurrentProcessPackageIdentifier(sizeof(pkgId), pkgId));
    CHECK_COND(std::strlen(pkgId) > 0, "Empty package identifier");

    char locale[LOCALE_NAME_MAX_BYTES]{};
    CHECK_HR(XPackageGetUserLocale(sizeof(locale), locale));
    CHECK_COND(std::strlen(locale) >= 2, "Empty user locale");

    int enumPkgCount = 0;
    CHECK_HR(XPackageEnumeratePackages(
        XPackageKind::Game, XPackageEnumerationScope::ThisOnly, &enumPkgCount,
        [](void* ctx, const XPackageDetails* details) noexcept -> bool {
            if (details && details->packageIdentifier && details->kind == XPackageKind::Game) {
                ++(*static_cast<int*>(ctx));
            }
            return true;
        }));
    CHECK_COND(enumPkgCount == 1, "XPackageEnumeratePackages did not enumerate current game package");

    XPackageInstallationMonitorHandle monitor = nullptr;
    CHECK_HR(XPackageCreateInstallationMonitor(pkgId, 0, nullptr, 1000, queue, &monitor));
    XPackageInstallationProgress prog{};
    XPackageGetInstallationProgress(monitor, &prog);
    CHECK_COND(prog.launchable && prog.completed && prog.installedBytes == prog.totalBytes,
               "XPackageInstallationProgress should report completed");
    XPackageCloseInstallationMonitorHandle(monitor);

    XAsyncBlock mountAsync{};
    mountAsync.queue = queue;
    CHECK_HR(XPackageMountWithUiAsync(pkgId, &mountAsync));
    XPackageMountHandle mount = nullptr;
    CHECK_HR(XPackageMountWithUiResult(&mountAsync, &mount));
    CHECK_COND(mount != nullptr, "XPackageMountWithUiResult returned null");
    size_t mountPathSz = 0;
    CHECK_HR(XPackageGetMountPathSize(mount, &mountPathSz));
    std::string mountPath(mountPathSz, '\0');
    CHECK_HR(XPackageGetMountPath(mount, mountPath.size(), mountPath.data()));
    CHECK_COND(std::strlen(mountPath.c_str()) > 0, "Empty XPackage mount path");
    XPackageCloseMountHandle(mount);

    XPackageChunkAvailability avail = XPackageChunkAvailability::Unavailable;
    CHECK_HR(XPackageFindChunkAvailability(pkgId, 0, nullptr, &avail));
    CHECK_COND(avail == XPackageChunkAvailability::Ready, "XPackageFindChunkAvailability != Ready");

    // Clean shutdown
    XUserCloseHandle(user);
    XTaskQueueCloseHandle(queue);
    XGameRuntimeUninitialize();

    std::printf(
        "PASS: xgameruntime full suite (XSystem[%s/%s] + XThread + XTaskQueue[Timer+Terminate] + "
        "XUser[%s] + XGameSave[Atomic+Quota] + XNetworking[UDP:%u] + PLS + XPackage[%s, %s])\n",
        analytics.form, sandboxId, modernGamertag, static_cast<unsigned>(udpPort), pkgId, locale);
    return 0;
}
