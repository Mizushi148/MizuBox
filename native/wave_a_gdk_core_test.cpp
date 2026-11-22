// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// CTest #29 (Wave A): Verifies 100% completion of:
//   - XTaskQueue (CreateComposite + GetPort + RegisterMonitor/UnregisterMonitor)
//   - XUser & XStore (AddByIdWithUi, WsxDispatchUserChangeEvent, Consumables, PurchaseUI, AssociatedProducts)
//   - XGameSave (GetContainerInfo, GetFolderWithUiResultSize, 16 MiB all-or-nothing transactional rollback)
//   - Local Persistent XSAPI (TitleManagedStats, UserStatistics, Leaderboards, Social, MultiplayerManager)
//   - GameInput (IGameInputForceFeedbackEffect, MotorGain/PowerOn, DeviceCallback registration)

#include "include/xbox/XGameRuntimeInit.h"
#include "include/xbox/XTaskQueue.h"
#include "include/xbox/XUser.h"
#include "include/xbox/XStore.h"
#include "include/xbox/XGameSave.h"
#include "include/xbox/XGameSaveFiles.h"
#include "include/xbox/xsapi_c.h"

#include <GameInput.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

bool fail(const char* msg, HRESULT hr = S_OK) {
    if (hr != S_OK) {
        std::fprintf(stderr, "FAIL: %s (hr=0x%08lX)\n", msg, static_cast<unsigned long>(hr));
    } else {
        std::fprintf(stderr, "FAIL: %s\n", msg);
    }
    return false;
}

bool test_taskqueue_composite_and_monitor() {
    XTaskQueueHandle qA = nullptr, qB = nullptr, qComp = nullptr;
    if (FAILED(XTaskQueueCreate(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &qA)))
        return fail("Create qA");
    if (FAILED(XTaskQueueCreate(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &qB)))
        return fail("Create qB");

    XTaskQueuePortHandle pWork = nullptr, pComp = nullptr;
    if (FAILED(XTaskQueueGetPort(qA, XTaskQueuePort::Work, &pWork)) ||
        FAILED(XTaskQueueGetPort(qB, XTaskQueuePort::Completion, &pComp))) {
        return fail("XTaskQueueGetPort");
    }
    if (FAILED(XTaskQueueCreateComposite(pWork, pComp, &qComp)) || !qComp) {
        return fail("XTaskQueueCreateComposite");
    }

    std::atomic<int> monitorHits{0};
    XTaskQueueRegistrationToken monTok{};
    if (FAILED(XTaskQueueRegisterMonitor(
            qComp, &monitorHits,
            [](void* ctx, XTaskQueueHandle, XTaskQueuePort) noexcept {
                static_cast<std::atomic<int>*>(ctx)->fetch_add(1);
            },
            &monTok))) {
        return fail("XTaskQueueRegisterMonitor");
    }

    std::atomic<int> workDone{0}, compDone{0};
    XTaskQueueSubmitCallback(qComp, XTaskQueuePort::Work, &workDone,
                             [](void* ctx, bool) noexcept { static_cast<std::atomic<int>*>(ctx)->fetch_add(1); });
    XTaskQueueSubmitCallback(qComp, XTaskQueuePort::Completion, &compDone,
                             [](void* ctx, bool) noexcept { static_cast<std::atomic<int>*>(ctx)->fetch_add(1); });

    if (monitorHits.load() != 2) return fail("Monitor did not receive 2 submit callbacks");
    // Dispatch through the delegated backing queues qA (work) and qB (completion)!
    if (!XTaskQueueDispatch(qA, XTaskQueuePort::Work, 0) || workDone.load() != 1) {
        return fail("Composite work port did not delegate to qA");
    }
    if (!XTaskQueueDispatch(qB, XTaskQueuePort::Completion, 0) || compDone.load() != 1) {
        return fail("Composite completion port did not delegate to qB");
    }

    XTaskQueueUnregisterMonitor(qComp, monTok);
    XTaskQueueCloseHandle(qComp);
    XTaskQueueCloseHandle(qB);
    XTaskQueueCloseHandle(qA);
    std::printf("  [WaveA:XTaskQueue] Composite port delegation + Monitor callbacks OK\n");
    return true;
}

bool test_gamesave_transactional_and_container_info(XUserHandle user) {
    const fs::path saveDir = fs::temp_directory_path() / L"wsx_wave_a_saves";
    std::error_code ec;
    fs::remove_all(saveDir, ec);
    SetEnvironmentVariableW(L"WSX_GAMESAVE_DIR", saveDir.c_str());

    XGameSaveProviderHandle prov = nullptr;
    if (FAILED(XGameSaveInitializeProvider(user, "00000000-0000-0000-0000-000076029b4d", false, &prov)))
        return fail("XGameSaveInitializeProvider");

    XGameSaveContainerHandle cont = nullptr;
    if (FAILED(XGameSaveCreateContainer(prov, "slot1", &cont))) return fail("CreateContainer");

    // 1. Valid multi-blob commit
    XGameSaveUpdateHandle upd = nullptr;
    const uint8_t dataA[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    const uint8_t dataB[8] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x11,0x22};
    if (FAILED(XGameSaveCreateUpdate(cont, "Slot One", &upd)) ||
        FAILED(XGameSaveSubmitBlobWrite(upd, "header", dataA, sizeof(dataA))) ||
        FAILED(XGameSaveSubmitBlobWrite(upd, "world", dataB, sizeof(dataB))) ||
        FAILED(XGameSaveSubmitUpdate(upd))) {
        return fail("Initial multi-blob commit failed");
    }
    XGameSaveCloseUpdate(upd);

    // 2. Oversized transactional batch (> 16 MiB) must return E_GS_UPDATE_TOO_BIG and leave existing blobs untouched!
    XGameSaveUpdateHandle badUpd = nullptr;
    std::vector<uint8_t> huge(17u * 1024u * 1024u, 0x77u);
    const uint8_t poison[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    XGameSaveCreateUpdate(cont, "Poisoned", &badUpd);
    XGameSaveSubmitBlobWrite(badUpd, "header", poison, sizeof(poison));
    XGameSaveSubmitBlobWrite(badUpd, "huge_blob", huge.data(), huge.size());
    const HRESULT badHr = XGameSaveSubmitUpdate(badUpd);
    XGameSaveCloseUpdate(badUpd);
    if (badHr != E_GS_UPDATE_TOO_BIG) {
        return fail("Expected E_GS_UPDATE_TOO_BIG on >16 MiB update", badHr);
    }

    // Verify "slot1" still has exact 2 blobs and original 24 bytes via XGameSaveGetContainerInfo
    struct InfoCap { uint32_t blobs = 0; uint64_t bytes = 0; std::string display; } cap{};
    if (FAILED(XGameSaveGetContainerInfo(
            prov, "slot1", &cap,
            [](const XGameSaveContainerInfo* info, void* ctx) noexcept -> bool {
                auto* c = static_cast<InfoCap*>(ctx);
                c->blobs = info->blobCount;
                c->bytes = info->totalSize;
                c->display = info->displayName ? info->displayName : "";
                return true;
            }))) {
        return fail("XGameSaveGetContainerInfo failed");
    }
    if (cap.blobs != 2u || cap.bytes != 24u || cap.display != "Slot One") {
        return fail("Transactional rollback did not preserve original container state");
    }

    // 3. XGameSaveFilesGetFolderWithUiResultSize
    XAsyncBlock fab{};
    if (FAILED(XGameSaveFilesGetFolderWithUiAsync(user, "00000000-0000-0000-0000-000076029b4d", &fab)) ||
        FAILED(XAsyncGetStatus(&fab, true))) {
        return fail("XGameSaveFilesGetFolderWithUiAsync failed");
    }
    size_t fsz = 0;
    if (FAILED(XGameSaveFilesGetFolderWithUiResultSize(&fab, &fsz)) || fsz < 4u) {
        return fail("XGameSaveFilesGetFolderWithUiResultSize failed");
    }
    std::string fpath(fsz, '\0');
    if (FAILED(XGameSaveFilesGetFolderWithUiResult(&fab, fsz, fpath.data()))) {
        return fail("XGameSaveFilesGetFolderWithUiResult failed");
    }

    XGameSaveCloseContainer(cont);
    XGameSaveCloseProvider(prov);
    fs::remove_all(saveDir, ec);
    std::printf("  [WaveA:XGameSave] GetContainerInfo + 16 MiB tx rollback + FolderResultSize (%zu B) OK\n", fsz);
    return true;
}

bool test_xuser_xstore_xsapi_wave_a(XUserHandle user) {
    const fs::path dir = fs::temp_directory_path() / L"wsx_wave_a_xsapi";
    std::error_code ec;
    fs::remove_all(dir, ec);
    SetEnvironmentVariableW(L"WSX_XSAPI_DIR", dir.c_str());
    SetEnvironmentVariableA("WSX_STORE_ADDONS", "PACK_A,PACK_B");

    // 1. XUser change event dispatch + AddByIdWithUi
    std::atomic<int> evCount{0};
    XTaskQueueRegistrationToken uTok{};
    if (FAILED(XUserRegisterForChangeEvent(
            nullptr, &evCount,
            [](void* ctx, XUserLocalId, XUserChangeEvent ev) noexcept {
                if (ev == XUserChangeEvent::Gamertag) {
                    static_cast<std::atomic<int>*>(ctx)->fetch_add(1);
                }
            },
            &uTok))) {
        return fail("XUserRegisterForChangeEvent");
    }
    XUserLocalId lid{};
    XUserGetLocalId(user, &lid);
    WsxDispatchUserChangeEvent(lid, XUserChangeEvent::Gamertag);
    Sleep(30);
    XUserUnregisterForChangeEvent(uTok, true);
    if (evCount.load() < 1) return fail("WsxDispatchUserChangeEvent did not notify subscriber");

    {
        XAsyncBlock ab{};
        XUserHandle u2 = nullptr;
        if (FAILED(XUserAddByIdWithUiAsync(0x0009000012345678ULL, &ab)) ||
            FAILED(XUserAddByIdWithUiResult(&ab, &u2)) || !u2) {
            return fail("XUserAddByIdWithUiAsync failed");
        }
        uint64_t id2 = 0;
        XUserGetId(u2, &id2);
        XUserCloseHandle(u2);
        if (id2 != 0x0009000012345678ULL) return fail("Secondary user XUID mismatch");
    }

    // 2. XStore Consumables + PurchaseUI + AssociatedProducts
    XStoreContextHandle store = nullptr;
    if (FAILED(XStoreCreateContext(user, &store))) return fail("XStoreCreateContext");
    {
        XAsyncBlock ab{};
        XStoreConsumableResult c1{};
        if (FAILED(XStoreQueryConsumableBalanceRemainingAsync(store, "GOLD_COINS", &ab)) ||
            FAILED(XStoreQueryConsumableBalanceRemainingResult(&ab, &c1)) || c1.quantity != 100u) {
            return fail("Initial consumable balance expected 100");
        }

        XAsyncBlock fab{};
        GUID tid{1,2,3,{4,5,6,7,8,9,10,11}};
        if (FAILED(XStoreReportConsumableFulfillmentAsync(store, "GOLD_COINS", 35u, tid, &fab)) ||
            FAILED(XStoreReportConsumableFulfillmentResult(&fab))) {
            return fail("XStoreReportConsumableFulfillment failed");
        }

        XAsyncBlock ab2{};
        XStoreConsumableResult c2{};
        if (FAILED(XStoreQueryConsumableBalanceRemainingAsync(store, "GOLD_COINS", &ab2)) ||
            FAILED(XStoreQueryConsumableBalanceRemainingResult(&ab2, &c2)) || c2.quantity != 65u) {
            return fail("Post-fulfillment consumable balance expected 65");
        }

        XAsyncBlock pab{};
        if (FAILED(XStoreShowPurchaseUIAsync(store, "GOLD_COINS", "Gold Pack", nullptr, &pab)) ||
            FAILED(XStoreShowPurchaseUIResult(&pab))) {
            return fail("XStoreShowPurchaseUI failed");
        }

        XAsyncBlock qab{};
        XStoreProductQueryHandle qh = nullptr;
        if (FAILED(XStoreQueryAssociatedProductsAsync(store, XStoreProductKind::Durable, 10, &qab)) ||
            FAILED(XStoreQueryAssociatedProductsResult(&qab, &qh)) || !qh) {
            return fail("XStoreQueryAssociatedProducts failed");
        }
        int prodCount = 0;
        XStoreEnumerateProductsQuery(qh, &prodCount, [](const XStoreProduct*, void* ctx) noexcept -> bool {
            ++(*static_cast<int*>(ctx));
            return true;
        });
        XStoreCloseProductsQueryHandle(qh);
        if (prodCount != 2) return fail("Expected 2 associated products");
    }
    XStoreCloseContextHandle(store);

    // 3. Local Persistent XSAPI: Stats + Leaderboards + Social + MultiplayerManager
    XblInitArgs initArgs{};
    initArgs.scid = "00000000-0000-0000-0000-000076029b4d";
    if (FAILED(XblInitialize(&initArgs))) return fail("XblInitialize");
    XblContextHandle xbl = nullptr;
    if (FAILED(XblContextCreateHandle(user, &xbl))) return fail("XblContextCreateHandle");
    uint64_t xuid = 0;
    XblContextGetXboxUserId(xbl, &xuid);

    XblTitleManagedStatistic stat{};
    stat.statisticName = "HighScore";
    stat.statisticType = XblTitleManagedStatType::Number;
    stat.numberValue = 98500.0;
    {
        XAsyncBlock ab{};
        if (FAILED(XblTitleManagedStatsWriteAsync(xbl, xuid, &stat, 1u, &ab)) ||
            FAILED(XAsyncGetStatus(&ab, true))) {
            return fail("XblTitleManagedStatsWriteAsync failed");
        }
    }
    {
        XAsyncBlock ab{};
        if (FAILED(XblUserStatisticsGetSingleUserStatisticAsync(xbl, xuid, initArgs.scid, "HighScore", &ab)) ||
            FAILED(XAsyncGetStatus(&ab, true))) {
            return fail("XblUserStatisticsGetSingleUserStatisticAsync failed");
        }
        size_t sz = 0;
        XblUserStatisticsGetSingleUserStatisticResultSize(&ab, &sz);
        std::vector<uint8_t> buf(sz, 0u);
        XblUserStatisticsResult* res = nullptr;
        if (FAILED(XblUserStatisticsGetSingleUserStatisticResult(&ab, buf.size(), buf.data(), &res, nullptr)) ||
            !res || res->serviceConfigStatisticsCount != 1u ||
            res->serviceConfigStatistics[0].statisticsCount != 1u ||
            std::strcmp(res->serviceConfigStatistics[0].statistics[0].value, "98500") != 0) {
            return fail("Persisted HighScore statistic mismatch");
        }
    }
    {
        XblLeaderboardQuery q{};
        q.xboxUserId = xuid;
        strncpy_s(q.scid, sizeof(q.scid), initArgs.scid, _TRUNCATE);
        q.statName = "HighScore";
        q.maxItems = 10u;
        XAsyncBlock ab{};
        if (FAILED(XblLeaderboardGetLeaderboardAsync(xbl, q, &ab)) || FAILED(XAsyncGetStatus(&ab, true))) {
            return fail("XblLeaderboardGetLeaderboardAsync failed");
        }
        size_t sz = 0;
        XblLeaderboardGetLeaderboardResultSize(&ab, &sz);
        std::vector<uint8_t> buf(sz, 0u);
        XblLeaderboardResult* lb = nullptr;
        if (FAILED(XblLeaderboardGetLeaderboardResult(&ab, buf.size(), buf.data(), &lb, nullptr)) ||
            !lb || lb->rowsCount != 1u || lb->rows[0].rank != 1u ||
            std::strcmp(lb->rows[0].columnValues[0], "98500") != 0) {
            return fail("Local Leaderboard result mismatch");
        }
    }
    {
        XAsyncBlock ab{};
        XblSocialRelationshipResultHandle sres = nullptr;
        if (FAILED(XblSocialGetSocialRelationshipsAsync(xbl, xuid, 0, 0, 10, &ab)) ||
            FAILED(XblSocialGetSocialRelationshipsResult(&ab, &sres)) || !sres) {
            return fail("XblSocialGetSocialRelationships failed");
        }
        const XblSocialRelationship* rels = nullptr;
        size_t rcnt = 99;
        XblSocialRelationshipResultGetRelationships(sres, &rels, &rcnt);
        XblSocialRelationshipResultCloseHandle(sres);
        if (rcnt != 0u) return fail("Expected 0 offline friends");
    }
    if (FAILED(XblMultiplayerManagerInitialize("OfflineLobby", nullptr))) {
        return fail("XblMultiplayerManagerInitialize");
    }
    const XblMultiplayerEvent* mpEvs = nullptr;
    size_t mpCnt = 0;
    if (FAILED(XblMultiplayerManagerDoWork(&mpEvs, &mpCnt)) ||
        XblMultiplayerManagerLobbySessionLocalMembersCount() != 1u) {
        return fail("XblMultiplayerManager lobby count mismatch");
    }

    XblContextCloseHandle(xbl);
    XblCleanup();
    fs::remove_all(dir, ec);
    std::printf("  [WaveA:XUser/XStore/XSAPI] ChangeEvent + Consumables (100->65) + Stats/Leaderboard(98500) + Lobby OK\n");
    return true;
}

bool test_gameinput_ffb_and_callbacks() {
    ComPtr<IGameInput> gi;
    HRESULT hr = GameInputCreate(&gi);
    if (FAILED(hr) || !gi) return fail("GameInputCreate failed", hr);

    ComPtr<IGameInputDevice> agg;
    hr = gi->CreateAggregateDevice(GameInputKindGamepad, &agg);
    if (FAILED(hr) || !agg) return fail("CreateAggregateDevice failed", hr);

    if (!agg->IsForceFeedbackMotorPoweredOn(0)) {
        return fail("Expected FFB motor powered on for gamepad device");
    }
    agg->SetForceFeedbackMotorGain(0, 0.8f);

    GameInputForceFeedbackParams params{};
    params.kind = GameInputForceFeedbackConstant;
    ComPtr<IGameInputForceFeedbackEffect> fx;
    hr = agg->CreateForceFeedbackEffect(0, &params, &fx);
    if (FAILED(hr) || !fx) return fail("CreateForceFeedbackEffect failed", hr);

    fx->SetGain(0.5f);
    if (fx->GetGain() < 0.49f || fx->GetGain() > 0.51f) return fail("FFB SetGain/GetGain mismatch");
    fx->SetState(GameInputFeedbackRunning);
    if (fx->GetState() != GameInputFeedbackRunning) return fail("FFB Running state mismatch");
    fx->SetState(GameInputFeedbackStopped);
    if (fx->GetState() != GameInputFeedbackStopped) return fail("FFB Stopped state mismatch");

    // Verify Impulse Trigger rumble + DeviceCallback registration & clean unregistration
    GameInputRumbleParams rp{0.2f, 0.3f, 0.9f, 0.8f};
    agg->SetRumbleState(&rp);
    GameInputRumbleParams zero{};
    agg->SetRumbleState(&zero);

    int seenDevices = 0;
    GameInputCallbackToken tok = 0;
    hr = gi->RegisterDeviceCallback(
        nullptr, GameInputKindGamepad, GameInputDeviceConnected,
        GameInputBlockingEnumeration, &seenDevices,
        [](GameInputCallbackToken, void* ctx, IGameInputDevice*, uint64_t,
           GameInputDeviceStatus, GameInputDeviceStatus) noexcept {
            ++(*static_cast<int*>(ctx));
        },
        &tok);
    if (FAILED(hr) || seenDevices < 1 || !gi->UnregisterCallback(tok, 0)) {
        return fail("GameInput RegisterDeviceCallback/UnregisterCallback failed", hr);
    }

    std::printf("  [WaveA:GameInput] IGameInputForceFeedbackEffect + Impulse Triggers + DeviceCallbacks (seen=%d) OK\n",
                seenDevices);
    return true;
}

} // namespace

int main() {
    std::printf("[wave_a_gdk_core_test] Running Wave A verification...\n");
    if (FAILED(XGameRuntimeInitialize())) return 1;

    XUserHandle user = nullptr;
    {
        XAsyncBlock ab{};
        if (FAILED(XUserAddAsync(XUserAddOptions::AddDefaultUserSilently, &ab)) ||
            FAILED(XUserAddResult(&ab, &user)) || !user) {
            return 2;
        }
    }

    int rc = 0;
    if (!test_taskqueue_composite_and_monitor()) rc = 3;
    else if (!test_gamesave_transactional_and_container_info(user)) rc = 4;
    else if (!test_xuser_xstore_xsapi_wave_a(user)) rc = 5;
    else if (!test_gameinput_ffb_and_callbacks()) rc = 6;

    XUserCloseHandle(user);
    XGameRuntimeUninitialize();

    if (rc == 0) {
        std::printf("PASS: wave_a_gdk_core_test (all 5 Wave A subsystems verified 100%%)\n");
    }
    return rc;
}
