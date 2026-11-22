// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// CTest #27: Extended Offline XUser + Offline XStore + Local Persistent JSON XSAPI
// (Zero Microsoft cloud calls).

#include "include/xbox/XGameRuntimeInit.h"
#include "include/xbox/XUser.h"
#include "include/xbox/XStore.h"
#include "include/xbox/xsapi_c.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

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

bool test_extended_xuser(XUserHandle user) {
    // 1. UTF-8 Token & Signature
    {
        XAsyncBlock ab{};
        HRESULT hr = XUserGetTokenAndSignatureAsync(
            user, XUserGetTokenAndSignatureOptions::None,
            "GET", "https://offline.local/profile", 0, nullptr, 0, nullptr, &ab);
        if (FAILED(hr)) return fail("XUserGetTokenAndSignatureAsync failed", hr);
        hr = XAsyncGetStatus(&ab, true);
        if (FAILED(hr)) return fail("Token async wait failed", hr);

        size_t sz = 0;
        hr = XUserGetTokenAndSignatureResultSize(&ab, &sz);
        if (FAILED(hr) || sz < sizeof(XUserGetTokenAndSignatureData)) {
            return fail("XUserGetTokenAndSignatureResultSize failed", hr);
        }
        std::vector<uint8_t> buf(sz, 0u);
        XUserGetTokenAndSignatureData* data = nullptr;
        size_t used = 0;
        hr = XUserGetTokenAndSignatureResult(&ab, buf.size(), buf.data(), &data, &used);
        if (FAILED(hr) || !data || !data->token || !data->signature) {
            return fail("XUserGetTokenAndSignatureResult failed", hr);
        }
        if (std::strncmp(data->token, "XBL3.0 ", 7) != 0 ||
            std::strncmp(data->signature, "WSXSIG-", 7) != 0) {
            return fail("Unexpected offline XBL token/signature format");
        }
        std::printf("  [XUser:Token] %s | sig=%s\n", data->token, data->signature);
    }

    // 2. UTF-16 Token & Signature
    {
        XAsyncBlock ab{};
        HRESULT hr = XUserGetTokenAndSignatureUtf16Async(
            user, XUserGetTokenAndSignatureOptions::ForceRefresh,
            L"POST", L"https://offline.local/stats", 0, nullptr, 0, nullptr, &ab);
        if (FAILED(hr)) return fail("XUserGetTokenAndSignatureUtf16Async failed", hr);
        hr = XAsyncGetStatus(&ab, true);
        if (FAILED(hr)) return fail("UTF-16 token wait failed", hr);

        size_t sz = 0;
        hr = XUserGetTokenAndSignatureUtf16ResultSize(&ab, &sz);
        if (FAILED(hr) || sz == 0) return fail("UTF-16 token ResultSize failed", hr);
        std::vector<uint8_t> buf(sz, 0u);
        XUserGetTokenAndSignatureUtf16Data* wdata = nullptr;
        hr = XUserGetTokenAndSignatureUtf16Result(&ab, buf.size(), buf.data(), &wdata, nullptr);
        if (FAILED(hr) || !wdata || !wdata->token || std::wcsncmp(wdata->token, L"XBL3.0 ", 7) != 0) {
            return fail("XUserGetTokenAndSignatureUtf16Result invalid", hr);
        }
    }

    // 3. Valid PNG GamerPicture
    {
        XAsyncBlock ab{};
        HRESULT hr = XUserGetGamerPictureAsync(user, XUserGamerPictureSize::Medium, &ab);
        if (FAILED(hr)) return fail("XUserGetGamerPictureAsync failed", hr);
        hr = XAsyncGetStatus(&ab, true);
        if (FAILED(hr)) return fail("GamerPicture wait failed", hr);
        size_t sz = 0;
        hr = XUserGetGamerPictureResultSize(&ab, &sz);
        if (FAILED(hr) || sz < 32u) return fail("GamerPicture size invalid", hr);
        std::vector<uint8_t> png(sz, 0u);
        size_t used = 0;
        hr = XUserGetGamerPictureResult(&ab, png.size(), png.data(), &used);
        if (FAILED(hr) || used != sz) return fail("XUserGetGamerPictureResult failed", hr);
        static constexpr uint8_t kPngSig[8] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
        if (std::memcmp(png.data(), kPngSig, 8) != 0) {
            return fail("GamerPicture output is not a valid PNG");
        }
        std::printf("  [XUser:GamerPic] valid PNG (%zu bytes)\n", used);
    }

    // 4. Privilege/Issue UI Resolvers + SignOut Deferral
    {
        XAsyncBlock ab1{};
        if (FAILED(XUserResolvePrivilegeWithUiAsync(user, XUserPrivilegeOptions::None, XUserPrivilege::Multiplayer, &ab1)) ||
            FAILED(XUserResolvePrivilegeWithUiResult(&ab1))) {
            return fail("XUserResolvePrivilegeWithUiAsync failed");
        }
        XAsyncBlock ab2{};
        if (FAILED(XUserResolveIssueWithUiUtf16Async(user, L"https://offline.local/consent", &ab2)) ||
            FAILED(XUserResolveIssueWithUiUtf16Result(&ab2))) {
            return fail("XUserResolveIssueWithUiUtf16Async failed");
        }
        XUserSignOutDeferralHandle def = nullptr;
        if (FAILED(XUserGetSignOutDeferral(&def)) || !def) {
            return fail("XUserGetSignOutDeferral failed");
        }
        XUserCloseSignOutDeferralHandle(def);
    }
    return true;
}

bool test_offline_xstore(XUserHandle user) {
    SetEnvironmentVariableA("WSX_STORE_ADDONS", "DLC_PACK_01,EXPANSION_ALPHA");

    XStoreContextHandle store = nullptr;
    HRESULT hr = XStoreCreateContext(user, &store);
    if (FAILED(hr) || !store) return fail("XStoreCreateContext failed", hr);

    // 1. Game License
    {
        XAsyncBlock ab{};
        hr = XStoreQueryGameLicenseAsync(store, &ab);
        if (FAILED(hr)) return fail("XStoreQueryGameLicenseAsync failed", hr);
        XStoreGameLicense lic{};
        hr = XStoreQueryGameLicenseResult(&ab, &lic);
        if (FAILED(hr) || !lic.isActive || lic.isTrial) {
            return fail("Offline game license should be active & non-trial", hr);
        }
        std::printf("  [XStore:License] sku=%s active=%d trial=%d\n",
                    lic.skuStoreId, lic.isActive ? 1 : 0, lic.isTrial ? 1 : 0);
    }

    // 2. Add-On Licenses
    {
        XAsyncBlock ab{};
        hr = XStoreQueryAddOnLicensesAsync(store, &ab);
        if (FAILED(hr)) return fail("XStoreQueryAddOnLicensesAsync failed", hr);
        if (FAILED(XAsyncGetStatus(&ab, true))) return fail("AddOn wait failed");
        uint32_t count = 0;
        hr = XStoreQueryAddOnLicensesResultCount(&ab, &count);
        if (FAILED(hr) || count != 2u) return fail("Expected 2 configured add-on licenses", hr);
        std::vector<XStoreAddonLicense> addons(count);
        hr = XStoreQueryAddOnLicensesResult(&ab, count, addons.data());
        if (FAILED(hr) || !addons[0].isActive || std::strcmp(addons[1].skuStoreId, "EXPANSION_ALPHA") != 0) {
            return fail("XStoreQueryAddOnLicensesResult mismatch", hr);
        }
        std::printf("  [XStore:AddOns] count=%u (%s, %s)\n",
                    count, addons[0].skuStoreId, addons[1].skuStoreId);
    }

    // 3. Product + CanAcquire + Notification registration
    {
        XAsyncBlock ab{};
        XStoreProduct prod{};
        if (FAILED(XStoreQueryProductForCurrentGameAsync(store, &ab)) ||
            FAILED(XStoreQueryProductForCurrentGameResult(&ab, &prod)) ||
            !prod.isInUserCollection || !prod.storeId) {
            return fail("XStoreQueryProductForCurrentGame failed");
        }

        XAsyncBlock ab2{};
        XStoreCanAcquireLicenseResult canRes{};
        if (FAILED(XStoreCanAcquireLicenseForStoreIdAsync(store, "9WSXLOCAL0001", &ab2)) ||
            FAILED(XStoreCanAcquireLicenseForStoreIdResult(&ab2, &canRes)) ||
            canRes.status != XStoreCanLicenseStatus::Licensable) {
            return fail("XStoreCanAcquireLicenseForStoreId failed");
        }

        XTaskQueueRegistrationToken tok{};
        auto cb = [](void*) {};
        if (FAILED(XStoreRegisterGameLicenseChanged(store, nullptr, nullptr, cb, &tok)) || tok.token == 0) {
            return fail("XStoreRegisterGameLicenseChanged failed");
        }
        if (!XStoreUnregisterGameLicenseChanged(store, tok, true)) {
            return fail("XStoreUnregisterGameLicenseChanged failed");
        }
        XStoreAvailability avail{};
        avail.availabilityId = "AVAIL001";
        if (!XStoreIsAvailabilityPurchasable(avail)) {
            return fail("XStoreIsAvailabilityPurchasable returned false");
        }
    }

    XStoreCloseContextHandle(store);
    return true;
}

bool test_persistent_offline_xsapi(XUserHandle user) {
    const fs::path xsapi_dir = fs::temp_directory_path() / L"wsx_offline_xsapi_test";
    std::error_code ec;
    fs::remove_all(xsapi_dir, ec);
    fs::create_directories(xsapi_dir, ec);
    SetEnvironmentVariableW(L"WSX_XSAPI_DIR", xsapi_dir.c_str());

    XblInitArgs initArgs{};
    initArgs.scid = "00000000-0000-0000-0000-000076029b4d";
    if (FAILED(XblInitialize(&initArgs))) return fail("XblInitialize failed");

    XblContextHandle ctx = nullptr;
    if (FAILED(XblContextCreateHandle(user, &ctx)) || !ctx) {
        return fail("XblContextCreateHandle failed");
    }
    uint64_t xuid = 0;
    if (FAILED(XblContextGetXboxUserId(ctx, &xuid)) || xuid == 0) {
        return fail("XblContextGetXboxUserId failed");
    }

    // Update two achievements: "1" -> 50% (InProgress), "2" -> 100% (Achieved, +25G)
    {
        XAsyncBlock ab1{}, ab2{};
        if (FAILED(XblAchievementsUpdateAchievementAsync(ctx, xuid, "1", 50u, &ab1)) ||
            FAILED(XAsyncGetStatus(&ab1, true))) {
            return fail("XblAchievementsUpdateAchievementAsync(1, 50%) failed");
        }
        if (FAILED(XblAchievementsUpdateAchievementAsync(ctx, xuid, "2", 100u, &ab2)) ||
            FAILED(XAsyncGetStatus(&ab2, true))) {
            return fail("XblAchievementsUpdateAchievementAsync(2, 100%) failed");
        }
    }

    // Set rich presence (persisted to JSON)
    {
        XblPresenceRichPresenceIds rp{};
        strncpy_s(rp.scid, sizeof(rp.scid), initArgs.scid, _TRUNCATE);
        rp.presenceId = "ExploringViceCity";
        XAsyncBlock ab{};
        if (FAILED(XblPresenceSetPresenceAsync(ctx, true, &rp, &ab)) ||
            FAILED(XAsyncGetStatus(&ab, true))) {
            return fail("XblPresenceSetPresenceAsync failed");
        }
    }

    // Tear down completely (clears in-memory cache) and reload strictly from local JSON!
    XblContextCloseHandle(ctx);
    ctx = nullptr;
    XblCleanup();

    if (FAILED(XblInitialize(&initArgs))) return fail("Second XblInitialize failed");
    if (FAILED(XblContextCreateHandle(user, &ctx)) || !ctx) {
        return fail("Second XblContextCreateHandle failed");
    }

    // Verify persisted achievements loaded from disk
    {
        XAsyncBlock ab{};
        if (FAILED(XblAchievementsGetAchievementsForTitleIdAsync(
                ctx, xuid, 0, XblAchievementType::All, false,
                XblAchievementOrderBy::DefaultOrder, 0, 100, &ab))) {
            return fail("XblAchievementsGetAchievementsForTitleIdAsync failed");
        }
        XblAchievementsResultHandle res = nullptr;
        if (FAILED(XblAchievementsGetAchievementsForTitleIdResult(&ab, &res)) || !res) {
            return fail("XblAchievementsGetAchievementsForTitleIdResult failed");
        }
        const XblAchievement* list = nullptr;
        size_t count = 0;
        if (FAILED(XblAchievementsResultGetAchievements(res, &list, &count)) || count != 2u) {
            XblAchievementsResultCloseHandle(res);
            return fail("Expected 2 persisted achievements reloaded from JSON");
        }
        bool found1 = false, found2 = false;
        for (size_t i = 0; i < count; ++i) {
            if (std::strcmp(list[i].id, "1") == 0 &&
                list[i].progressState == XblAchievementProgressState::InProgress &&
                list[i].progression.percentComplete == 50u) {
                found1 = true;
            }
            if (std::strcmp(list[i].id, "2") == 0 &&
                list[i].progressState == XblAchievementProgressState::Achieved &&
                list[i].progression.percentComplete == 100u &&
                list[i].progression.timeUnlocked > 0) {
                found2 = true;
            }
        }
        XblAchievementsResultCloseHandle(res);
        if (!found1 || !found2) return fail("Persisted achievement states did not match");
        std::printf("  [XSAPI:Achievements] reloaded 2/2 persisted entries from JSON OK\n");
    }

    // Verify profile gamerscore accumulated (+25G on base 1000G => 1025G)
    {
        XAsyncBlock ab{};
        XblUserProfile prof{};
        if (FAILED(XblProfileGetUserProfileAsync(ctx, xuid, &ab)) ||
            FAILED(XblProfileGetUserProfileResult(&ab, &prof))) {
            return fail("XblProfileGetUserProfileAsync failed");
        }
        if (std::strcmp(prof.gamerscore, "1025") != 0) {
            std::fprintf(stderr, "Unexpected gamerscore: %s (expected 1025)\n", prof.gamerscore);
            return fail("Gamerscore did not accumulate from persisted unlocked achievement");
        }
        std::printf("  [XSAPI:Profile] gamertag=%s gamerscore=%s tier=%s\n",
                    prof.modernGamertag, prof.gamerscore, prof.accountTier);
    }

    // Verify persisted presence reloaded from JSON
    {
        XAsyncBlock ab{};
        XblPresenceRecord rec{};
        if (FAILED(XblPresenceGetPresenceAsync(ctx, xuid, &ab)) ||
            FAILED(XblPresenceGetPresenceResult(&ab, &rec))) {
            return fail("XblPresenceGetPresenceAsync failed");
        }
        if (rec.userState != XblPresenceUserState::Online ||
            std::strcmp(rec.richPresenceId, "ExploringViceCity") != 0) {
            return fail("Persisted presence did not match");
        }
        std::printf("  [XSAPI:Presence] state=Online richPresence=%s\n", rec.richPresenceId);
    }

    // Verify Privacy + String verification
    {
        XAsyncBlock ab1{};
        XblPermissionCheckResult perm{};
        if (FAILED(XblPrivacyCheckPermissionAsync(ctx, XblPermission::PlayMultiplayer, xuid, &ab1)) ||
            FAILED(XblPrivacyCheckPermissionResult(&ab1, &perm)) || !perm.isAllowed) {
            return fail("XblPrivacyCheckPermission failed");
        }

        XAsyncBlock ab2{};
        if (FAILED(XblStringVerifyStringAsync(ctx, "MizuBox-CleanRoom", &ab2)) ||
            FAILED(XAsyncGetStatus(&ab2, true))) {
            return fail("XblStringVerifyStringAsync failed");
        }
        size_t sz = 0;
        if (FAILED(XblStringVerifyStringResultSize(&ab2, &sz)) || sz < sizeof(XblVerifyStringResult)) {
            return fail("XblStringVerifyStringResultSize failed");
        }
        std::vector<uint8_t> buf(sz, 0u);
        XblVerifyStringResult* vres = nullptr;
        if (FAILED(XblStringVerifyStringResult(&ab2, buf.size(), buf.data(), &vres, nullptr)) ||
            !vres || vres->resultCode != XblVerifyStringResultCode::Success) {
            return fail("XblStringVerifyStringResult failed");
        }
    }

    XblContextCloseHandle(ctx);
    {
        XAsyncBlock cleanupAb{};
        if (FAILED(XblCleanupAsync(&cleanupAb)) || FAILED(XAsyncGetStatus(&cleanupAb, true))) {
            return fail("XblCleanupAsync failed");
        }
    }
    fs::remove_all(xsapi_dir, ec);
    return true;
}

} // namespace

int main() {
    std::printf("[offline_xsapi_test] Running Offline XUser + XStore + Persistent XSAPI tests...\n");
    if (FAILED(XGameRuntimeInitialize())) return 1;

    XUserHandle user = nullptr;
    {
        XAsyncBlock ab{};
        if (FAILED(XUserAddAsync(XUserAddOptions::AddDefaultUserSilently, &ab)) ||
            FAILED(XUserAddResult(&ab, &user)) || !user) {
            std::fprintf(stderr, "FAIL: XUserAddAsync failed\n");
            return 2;
        }
    }

    int rc = 0;
    if (!test_extended_xuser(user)) rc = 3;
    else if (!test_offline_xstore(user)) rc = 4;
    else if (!test_persistent_offline_xsapi(user)) rc = 5;

    XUserCloseHandle(user);
    XGameRuntimeUninitialize();

    if (rc == 0) {
        std::printf("PASS: offline_xsapi_test (XUser tokens/PNG + XStore licenses/addons + JSON-persisted XSAPI)\n");
    }
    return rc;
}
