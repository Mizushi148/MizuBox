// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Wave 13 package metadata, chunk availability, PLS and encrypted XVD rejection.

#include "wsx_package_config.h"
#include "wsx_xvd.h"
#include "include/xbox/XTaskQueue.h"
#include "include/xbox/XAsync.h"
#include "include/xbox/XPersistentLocalStorage.h"
#include "include/xbox/XPackage.h"

#include <Windows.h>
#include <unknwn.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

extern "C" {
HRESULT WINAPI DStorageOpenXvdVolume(const wchar_t* path, REFIID riid, void** outVolume);
}

namespace {

int g_failures = 0;

#define CHECK(cond, msg, ...)                                        \
    do {                                                             \
        if (!(cond)) {                                               \
            std::fprintf(stderr, "[FAIL] " msg "\n", ##__VA_ARGS__); \
            ++g_failures;                                            \
        } else {                                                     \
            std::printf("[PASS] " msg "\n", ##__VA_ARGS__);          \
        }                                                            \
    } while (0)

bool write_fixture(const fs::path& path, const char* contents, size_t size) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(contents, static_cast<std::streamsize>(size));
    file.close();
    return file.good();
}

bool write_fixture(const fs::path& path, std::string_view contents) {
    return write_fixture(path, contents.data(), contents.size());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: wsx_wave13_package_xvd_test <fixture directory>\n");
        return 2;
    }

    const fs::path build_root(argv[1]);
    constexpr std::string_view config = R"(<Game>
        <Identity Name="MizuBox.Fixture" Publisher="CN=MizuBox" Version="1.2.3.4" />
        <TitleId>1234ABCD</TitleId><StoreId>TEST00000001</StoreId><MSAAppId>0000000000000001</MSAAppId>
        <ShellVisuals DefaultDisplayName="MizuBox Test Title" />
        <Executable Name="pending.exe" Id="Game" TargetDeviceFamily="Scarlett" />
        <PersistentLocalStorage><SizeMB>64</SizeMB></PersistentLocalStorage>
        <Resources><Resource Language="en-US" /><Resource Language="ru-RU" /></Resources>
    </Game>)";
    constexpr std::string_view manifest = R"(<Package xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10">
        <uap:Protocol Name="ms-xbl-1234abcd" />
    </Package>)";
    constexpr std::string_view layout = R"(<Layout>
        <Chunk Id="1005"><FileGroup Include="installed.dat" /></Chunk>
        <Chunk Id="1001"><FileGroup Include="pending.exe" /></Chunk>
    </Layout>)";
    constexpr std::string_view payload = "MizuBox package fixture";
    const auto image = wsx_xvd::build_synthetic_xvd(nullptr, 0u, true, true);
    const fs::path xvd_path = build_root / "fixture.xvd";
    std::error_code ec;
    fs::create_directories(build_root, ec);
    if (ec || !write_fixture(build_root / "MicrosoftGame.Config", config) ||
        !write_fixture(build_root / "appxmanifest.xml", manifest) ||
        !write_fixture(build_root / "layout_fixture_xs.xml", layout) ||
        !write_fixture(build_root / "installed.dat", payload) ||
        !write_fixture(xvd_path, reinterpret_cast<const char*>(image.data()), image.size()) ||
        !SetEnvironmentVariableW(L"WSX_PACKAGE_DIR", build_root.c_str())) {
        std::fprintf(stderr, "Could not set up the MizuBox package fixture.\n");
        return 2;
    }

    // =========================================================================
    // 1. Live Hydration of MicrosoftGame.Config + appxmanifest.xml + layout_*_xs.xml
    // =========================================================================
    std::printf("\n=== [Wave 13.1] Package Fixture Hydration (MicrosoftGame.Config + appxmanifest + layout) ===\n");
    {
        const auto& cfg = wsx_pkg::get_hydrated_config(true);
        CHECK(cfg.hydrated_from_disk, "Hydrated package config directly from %s", cfg.config_path.string().c_str());
        CHECK(cfg.title_id == "1234ABCD" && cfg.title_id_u32 == 0x1234ABCDu,
              "Hydrated TitleId = %s (0x%08X)", cfg.title_id.c_str(), cfg.title_id_u32);
        CHECK(cfg.store_id == "TEST00000001" && cfg.msa_app_id == "0000000000000001",
              "Hydrated StoreId = %s, MSAAppId = %s", cfg.store_id.c_str(), cfg.msa_app_id.c_str());
        CHECK(cfg.identity_name == "MizuBox.Fixture" && cfg.version_str == "1.2.3.4",
              "Hydrated Identity = %s v%s", cfg.identity_name.c_str(), cfg.version_str.c_str());
        CHECK(cfg.exe_name == "pending.exe" && cfg.exe_id == "Game" && cfg.protocol == "ms-xbl-1234abcd",
              "Hydrated Executable = %s (Id=%s, Protocol=%s)", cfg.exe_name.c_str(), cfg.exe_id.c_str(), cfg.protocol.c_str());
        CHECK(cfg.pls_size_mb == 64u && cfg.locales.size() == 2u,
              "Hydrated PLS = %u MB, Locales = %zu languages", cfg.pls_size_mb, cfg.locales.size());
        CHECK(cfg.chunks.size() == 2u && cfg.chunks_ready == 1u &&
              cfg.total_installed_bytes == payload.size(), "Hydrated both chunks and counted installed bytes");

        // Verify xgameruntime.dll live XPackage & XPersistentLocalStorage exports
        char pkg_id[XPACKAGE_IDENTIFIER_MAX_LENGTH]{};
        CHECK(SUCCEEDED(XPackageGetCurrentProcessPackageIdentifier(sizeof(pkg_id), pkg_id)) &&
              std::strstr(pkg_id, "MizuBox.Fixture") != nullptr,
              "xgameruntime!XPackageGetCurrentProcessPackageIdentifier = %s", pkg_id);

        bool enum_ok = false;
        XPackageEnumeratePackages(
            XPackageKind::Game, XPackageEnumerationScope::ThisOnly, &enum_ok,
            [](void* ctx, const XPackageDetails* d) -> bool {
                if (d && std::strcmp(d->titleId, "1234ABCD") == 0 &&
                    std::strcmp(d->storeId, "TEST00000001") == 0 &&
                    std::strcmp(d->displayName, "MizuBox Test Title") == 0) {
                    *static_cast<bool*>(ctx) = true;
                }
                return true;
            });
        CHECK(enum_ok, "xgameruntime!XPackageEnumeratePackages hydrated TitleId=1234ABCD, StoreId=TEST00000001");

        XPackageChunkSelector sel_ready{};
        sel_ready.type = XPackageChunkSelectorType::Chunk;
        sel_ready.chunkId = 1005u; // installed.dat
        XPackageChunkSelector sel_missing{};
        sel_missing.type = XPackageChunkSelectorType::Chunk;
        sel_missing.chunkId = 1001u; // pending.exe
        XPackageChunkAvailability av_ready{}, av_missing{};
        CHECK(SUCCEEDED(XPackageFindChunkAvailability(pkg_id, 1, &sel_ready, &av_ready)) &&
              av_ready == XPackageChunkAvailability::Ready,
              "Chunk 1005 (installed.dat) -> XPackageChunkAvailability::Ready");
        CHECK(SUCCEEDED(XPackageFindChunkAvailability(pkg_id, 1, &sel_missing, &av_missing)) &&
              av_missing == XPackageChunkAvailability::Pending,
              "Chunk 1001 (missing pending.exe) -> XPackageChunkAvailability::Pending");

        XPersistentLocalStorageSpaceInfo pls{};
        CHECK(SUCCEEDED(XPersistentLocalStorageGetSpaceInfo(&pls)) &&
              pls.totalBytes == 64ULL * 1024ULL * 1024ULL,
              "xgameruntime!XPersistentLocalStorageGetSpaceInfo quota = %llu MB",
              static_cast<unsigned long long>(pls.totalBytes / (1024ULL * 1024ULL)));
    }

    // =========================================================================
    // 2. Synthetic encrypted XVD Inspection & Encrypted Retail Rejection
    // =========================================================================
    std::printf("\n=== [Wave 13.2] Synthetic encrypted XVD Header Validation (wsx_xvd.h + dstorage_xs.dll) ===\n");
    {
        std::ifstream xf(xvd_path, std::ios::binary);
        std::vector<uint8_t> hdr(wsx_xvd::kHeaderSize);
        const bool read_ok = xf && xf.read(reinterpret_cast<char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()));
        CHECK(read_ok, "Read 12 KiB XVD header from %s", xvd_path.string().c_str());

        wsx_xvd::XvdInfo xinfo{};
        const wsx_xvd::XvdStatus st = wsx_xvd::parse_header(hdr.data(), hdr.size(), xinfo);
        CHECK(st == wsx_xvd::XvdStatus::EncryptedUnsupported &&
              xinfo.format_version == 3u &&
              xinfo.volume_flags == 1u &&
              xinfo.drive_size == wsx_xvd::kPageSize &&
              xinfo.is_encrypted(),
              "Parsed fixture.xvd: magic='msft-xvd'@0x200, version=%u, flags=0x%X, driveSize=%llu bytes -> cleanly rejected as EncryptedUnsupported",
              xinfo.format_version, xinfo.volume_flags,
              static_cast<unsigned long long>(xinfo.drive_size));

        void* vol = nullptr;
        const HRESULT dhr = DStorageOpenXvdVolume(xvd_path.c_str(), __uuidof(IUnknown), &vol);
        CHECK(dhr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) && vol == nullptr,
              "dstorage_xs!DStorageOpenXvdVolume(fixture.xvd) returned ERROR_ACCESS_DENIED (0x%08lX)",
              static_cast<unsigned long>(dhr));
    }

    std::printf("\n=== Wave 13 Summary: failures=%d ===\n", g_failures);
    return (g_failures == 0) ? 0 : 1;
}
