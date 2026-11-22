// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Live Package Hydrator for MicrosoftGame.Config, appxmanifest.xml & layout_*_xs.xml:
// - Automatically discovers and parses the active Xbox Series X|S build's MicrosoftGame.Config,
//   appxmanifest.xml, and layout_*_xs.xml (e.g. GTA VI TitleId=7952987A, StoreId=9NL3WWNZLZZN,
//   Identity=RockstarGamesInc.28872443D20A7, Executable=game_scarlett_master.exe Id=GTAO,
//   PLS=2048 MB, 39 locales, and per-chunk file presence/byte sizes).
// - Feeds hydrated metadata into xgameruntime.dll (XPackage*, XSystem*, XPersistentLocalStorage*,
//   XGameSave*) and wsx_pe_loader.h (GetCurrentPackageFullName / FamilyName / AUMID).

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace wsx_pkg {

struct HydratedChunkInfo {
    uint32_t chunk_id = 0;
    uint32_t files_total = 0;
    uint32_t files_present = 0;
    uint64_t bytes_present = 0;
    bool all_present = false;
};

struct HydratedPackageConfig {
    bool hydrated_from_disk = false;
    std::filesystem::path root_dir;
    std::filesystem::path config_path;
    std::filesystem::path manifest_path;
    std::filesystem::path layout_path;

    std::string title_id = "00007777";
    uint32_t title_id_u32 = 0x00007777u;
    std::string msa_app_id = "0000000000007777";
    std::string store_id = "9NBLGGH4WSX0";
    std::string identity_name = "MizuBox.Title";
    std::string publisher = "CN=Mizushi";
    std::string version_str = "1.0.0.0";
    uint16_t ver_major = 1, ver_minor = 0, ver_build = 0, ver_rev = 0;

    std::string display_name = "MizuBox Title";
    std::string publisher_display_name = "Mizushi (MizuBox)";
    std::string exe_name = "game_scarlett_master.exe";
    std::string exe_id = "Game";
    std::string target_device_family = "Scarlett";
    std::string protocol = "ms-xbl-00007777";

    uint32_t pls_size_mb = 256u;
    bool smt_enabled = true;
    std::string anaconda_memory = "Advanced";
    std::string lockhart_memory = "Advanced";

    std::string package_full_name = "MizuBox.Title_1.0.0.0_x64__8wekyb3d8bbwe";
    std::string package_family_name = "MizuBox.Title_8wekyb3d8bbwe";
    std::string aumid = "MizuBox.Title_8wekyb3d8bbwe!Game";

    std::wstring package_full_name_w = L"MizuBox.Title_1.0.0.0_x64__8wekyb3d8bbwe";
    std::wstring package_family_name_w = L"MizuBox.Title_8wekyb3d8bbwe";
    std::wstring aumid_w = L"MizuBox.Title_8wekyb3d8bbwe!Game";

    std::vector<std::string> locales;
    std::vector<HydratedChunkInfo> chunks;
    uint64_t total_installed_bytes = 0;
    uint32_t chunks_ready = 0;
};

namespace detail {

inline std::wstring utf8_to_wide_pkg(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

inline std::string read_text_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

inline std::string extract_xml_tag(const std::string& xml, const char* tag) {
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    const size_t p0 = xml.find(open);
    if (p0 == std::string::npos) return {};
    const size_t s = p0 + open.size();
    const size_t p1 = xml.find(close, s);
    if (p1 == std::string::npos) return {};
    return xml.substr(s, p1 - s);
}

inline std::string extract_xml_attr_in_tag(const std::string& xml, const char* tag, const char* attr, size_t start_from = 0) {
    const std::string open = std::string("<") + tag + " ";
    const size_t t0 = xml.find(open, start_from);
    if (t0 == std::string::npos) return {};
    const size_t t1 = xml.find('>', t0);
    if (t1 == std::string::npos) return {};
    const std::string needle = std::string(attr) + "=\"";
    const size_t a0 = xml.find(needle, t0);
    if (a0 == std::string::npos || a0 >= t1) return {};
    const size_t v0 = a0 + needle.size();
    const size_t v1 = xml.find('"', v0);
    if (v1 == std::string::npos || v1 > t1) return {};
    return xml.substr(v0, v1 - v0);
}

inline std::filesystem::path discover_package_root() {
    namespace fs = std::filesystem;
    std::error_code ec;

    wchar_t env_dir[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"WSX_PACKAGE_DIR", env_dir, MAX_PATH) > 0) {
        fs::path p(env_dir);
        if (fs::is_regular_file(p / L"MicrosoftGame.Config", ec)) return fs::weakly_canonical(p, ec);
    }

    std::vector<fs::path> bases;
    const fs::path cwd = fs::current_path(ec);
    if (!cwd.empty()) {
        bases.push_back(cwd);
        bases.push_back(cwd.parent_path());
        bases.push_back(cwd.parent_path().parent_path());
    }
    wchar_t exe_buf[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe_buf, MAX_PATH) > 0) {
        fs::path ed = fs::path(exe_buf).parent_path();
        bases.push_back(ed);
        bases.push_back(ed.parent_path());
        bases.push_back(ed.parent_path().parent_path());
        bases.push_back(ed.parent_path().parent_path().parent_path());
    }

    for (const auto& b : bases) {
        if (b.empty()) continue;
        if (fs::is_regular_file(b / L"MicrosoftGame.Config", ec)) {
            return fs::weakly_canonical(b, ec);
        }
    }
    return {};
}

inline void parse_layout_xml(const std::filesystem::path& root,
                             const std::filesystem::path& layout_path,
                             HydratedPackageConfig& cfg) {
    namespace fs = std::filesystem;
    const std::string xml = read_text_file(layout_path);
    if (xml.empty()) return;

    size_t pos = 0;
    while (pos < xml.size()) {
        const size_t c0 = xml.find("<Chunk", pos);
        if (c0 == std::string::npos) break;
        const size_t c_end_tag = xml.find('>', c0);
        const size_t c1 = xml.find("</Chunk>", c0);
        if (c_end_tag == std::string::npos || c1 == std::string::npos) break;

        HydratedChunkInfo ci{};
        const std::string id_str = extract_xml_attr_in_tag(xml, "Chunk", "Id", c0);
        if (!id_str.empty()) {
            ci.chunk_id = static_cast<uint32_t>(std::strtoul(id_str.c_str(), nullptr, 10));
        }

        size_t fpos = c_end_tag + 1;
        std::error_code ec;
        while (fpos < c1) {
            const size_t fg = xml.find("<FileGroup", fpos);
            if (fg == std::string::npos || fg >= c1) break;
            const std::string dst = extract_xml_attr_in_tag(xml, "FileGroup", "DestinationPath", fg);
            const std::string inc = extract_xml_attr_in_tag(xml, "FileGroup", "Include", fg);
            if (!inc.empty()) {
                ++ci.files_total;
                std::string rel = dst;
                while (!rel.empty() && (rel.front() == '\\' || rel.front() == '/')) rel.erase(rel.begin());
                fs::path fpath = rel.empty() ? (root / inc) : (root / rel / inc);
                if (fs::is_regular_file(fpath, ec)) {
                    ++ci.files_present;
                    ci.bytes_present += fs::file_size(fpath, ec);
                }
            }
            const size_t fg_end = xml.find('>', fg);
            if (fg_end == std::string::npos) break;
            fpos = fg_end + 1;
        }
        ci.all_present = (ci.files_total > 0u && ci.files_present == ci.files_total);
        cfg.total_installed_bytes += ci.bytes_present;
        if (ci.all_present) ++cfg.chunks_ready;
        cfg.chunks.push_back(ci);
        pos = c1 + 8;
    }
}

inline HydratedPackageConfig load_package_config_from_root(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    HydratedPackageConfig cfg{};
    if (root.empty()) return cfg;

    std::error_code ec;
    const fs::path mgc = root / L"MicrosoftGame.Config";
    if (!fs::is_regular_file(mgc, ec)) return cfg;

    const std::string xml = read_text_file(mgc);
    if (xml.empty()) return cfg;

    cfg.hydrated_from_disk = true;
    cfg.root_dir = root;
    cfg.config_path = mgc;

    const std::string tid = extract_xml_tag(xml, "TitleId");
    if (!tid.empty()) {
        cfg.title_id = tid;
        cfg.title_id_u32 = static_cast<uint32_t>(std::strtoul(tid.c_str(), nullptr, 16));
    }
    const std::string msa = extract_xml_tag(xml, "MSAAppId");
    if (!msa.empty()) cfg.msa_app_id = msa;
    const std::string sid = extract_xml_tag(xml, "StoreId");
    if (!sid.empty()) cfg.store_id = sid;

    const std::string id_name = extract_xml_attr_in_tag(xml, "Identity", "Name");
    if (!id_name.empty()) cfg.identity_name = id_name;
    const std::string id_pub = extract_xml_attr_in_tag(xml, "Identity", "Publisher");
    if (!id_pub.empty()) cfg.publisher = id_pub;
    const std::string id_ver = extract_xml_attr_in_tag(xml, "Identity", "Version");
    if (!id_ver.empty()) {
        cfg.version_str = id_ver;
        unsigned a = 1, b = 0, c = 0, d = 0;
        if (sscanf_s(id_ver.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) >= 2) {
            cfg.ver_major = static_cast<uint16_t>(a);
            cfg.ver_minor = static_cast<uint16_t>(b);
            cfg.ver_build = static_cast<uint16_t>(c);
            cfg.ver_rev   = static_cast<uint16_t>(d);
        }
    }

    const std::string disp = extract_xml_attr_in_tag(xml, "ShellVisuals", "DefaultDisplayName");
    if (!disp.empty()) cfg.display_name = disp;
    const std::string pdisp = extract_xml_attr_in_tag(xml, "ShellVisuals", "PublisherDisplayName");
    if (!pdisp.empty()) cfg.publisher_display_name = pdisp;

    const std::string exe_n = extract_xml_attr_in_tag(xml, "Executable", "Name");
    if (!exe_n.empty()) cfg.exe_name = exe_n;
    const std::string exe_i = extract_xml_attr_in_tag(xml, "Executable", "Id");
    if (!exe_i.empty()) cfg.exe_id = exe_i;
    const std::string dev_f = extract_xml_attr_in_tag(xml, "Executable", "TargetDeviceFamily");
    if (!dev_f.empty()) cfg.target_device_family = dev_f;

    const std::string pls_s = extract_xml_tag(xml, "SizeMB");
    if (!pls_s.empty()) {
        const uint32_t mb = static_cast<uint32_t>(std::strtoul(pls_s.c_str(), nullptr, 10));
        if (mb > 0u) cfg.pls_size_mb = mb;
    }
    const std::string smt_s = extract_xml_tag(xml, "SMTEnabled");
    if (!smt_s.empty()) {
        cfg.smt_enabled = (smt_s == "true" || smt_s == "1");
    }
    const std::string ana_m = extract_xml_tag(xml, "XboxAnacondaTitleMemory");
    if (!ana_m.empty()) cfg.anaconda_memory = ana_m;
    const std::string lck_m = extract_xml_tag(xml, "XboxLockhartTitleMemory");
    if (!lck_m.empty()) cfg.lockhart_memory = lck_m;

    // Parse all <Resource Language="..." /> entries
    size_t rpos = 0;
    while (rpos < xml.size()) {
        const size_t r0 = xml.find("<Resource ", rpos);
        if (r0 == std::string::npos) break;
        const std::string lang = extract_xml_attr_in_tag(xml, "Resource", "Language", r0);
        if (!lang.empty()) cfg.locales.push_back(lang);
        const size_t r1 = xml.find('>', r0);
        if (r1 == std::string::npos) break;
        rpos = r1 + 1;
    }

    // Parse appxmanifest.xml if present
    const fs::path appx = root / L"appxmanifest.xml";
    if (fs::is_regular_file(appx, ec)) {
        cfg.manifest_path = appx;
        const std::string axml = read_text_file(appx);
        const std::string proto = extract_xml_attr_in_tag(axml, "uap:Protocol", "Name");
        if (!proto.empty()) cfg.protocol = proto;
    }

    // Build canonical AppModel identity strings
    cfg.package_full_name = cfg.identity_name + "_" + cfg.version_str + "_x64__8wekyb3d8bbwe";
    cfg.package_family_name = cfg.identity_name + "_8wekyb3d8bbwe";
    cfg.aumid = cfg.package_family_name + "!" + cfg.exe_id;
    cfg.package_full_name_w = utf8_to_wide_pkg(cfg.package_full_name);
    cfg.package_family_name_w = utf8_to_wide_pkg(cfg.package_family_name);
    cfg.aumid_w = utf8_to_wide_pkg(cfg.aumid);

    // Discover and parse layout_*_xs.xml (or layout*.xml)
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string fn = entry.path().filename().string();
        if (fn.rfind("layout", 0) == 0 && entry.path().extension() == ".xml") {
            cfg.layout_path = entry.path();
            parse_layout_xml(root, entry.path(), cfg);
            break;
        }
    }
    return cfg;
}

} // namespace detail

inline const HydratedPackageConfig& get_hydrated_config(bool force_reload = false) {
    static std::mutex mtx;
    static bool initialized = false;
    static std::wstring last_env_dir;
    static HydratedPackageConfig cached;

    std::lock_guard<std::mutex> lk(mtx);
    wchar_t env_buf[MAX_PATH]{};
    GetEnvironmentVariableW(L"WSX_PACKAGE_DIR", env_buf, MAX_PATH);
    const std::wstring cur_env(env_buf);
    if (!initialized || force_reload || cur_env != last_env_dir) {
        initialized = true;
        last_env_dir = cur_env;
        cached = detail::load_package_config_from_root(detail::discover_package_root());
    }
    return cached;
}

} // namespace wsx_pkg
