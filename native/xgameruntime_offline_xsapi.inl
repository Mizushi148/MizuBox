// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Included by xgameruntime.cpp:
//   - Extended Offline XUser (Tokens/Signatures UTF-8 & UTF-16, valid PNG GamerPicture,
//     UI Privilege/Issue Resolvers, SignOut Deferral)
//   - Offline XStore (Game License, Add-On Licenses, Current Game Product, CanAcquireLicense,
//     License Change Notifications)
//   - Local Persistent Offline XSAPI (XblInitialize/Cleanup, XblContext, XblProfile,
//     XblAchievements persisted atomically to local JSON, XblPresence, XblPrivacy, XblString)
// Zero Microsoft cloud calls.

struct XUserSignOutDeferral {
    uint64_t id = 0;
};

struct XStoreContext {
    std::atomic<int> refCount{1};
    XUserHandle user = nullptr;
    uint64_t xuid = 0;
};

struct XblContext {
    std::atomic<int> refCount{1};
    XUserHandle user = nullptr;
    uint64_t xuid = 0;
    std::string scid;
};

struct XblAchievementOwned {
    std::string id;
    std::string scid;
    std::string name;
    std::string unlockedDesc;
    std::string lockedDesc;
    uint32_t titleId = 0;
    uint32_t gamerscore = 0;
    XblAchievementProgressState progressState = XblAchievementProgressState::NotStarted;
    uint32_t percentComplete = 0;
    time_t timeUnlocked = 0;
    bool isSecret = false;
};

struct XblAchievementsResult {
    std::vector<XblAchievementOwned> storage;
    std::vector<XblAchievement> views;
    bool hasNext = false;
};

namespace {

// ----------------------------------------------------------------------------
// Deterministic Minimal Valid PNG Generator (1x1 RGBA #8957E5) for GamerPicture
// ----------------------------------------------------------------------------

uint32_t png_crc32(const uint8_t* data, size_t len) noexcept {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c ^= data[i];
        for (int b = 0; b < 8; ++b) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
    }
    return c ^ 0xFFFFFFFFu;
}

void png_append_be32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
}

void png_append_chunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t len) {
    png_append_be32(out, static_cast<uint32_t>(len));
    const size_t crc_start = out.size();
    out.insert(out.end(), type, type + 4);
    if (data && len > 0) {
        out.insert(out.end(), data, data + len);
    }
    const uint32_t crc = png_crc32(out.data() + crc_start, 4u + len);
    png_append_be32(out, crc);
}

const std::vector<uint8_t>& GetOfflineGamerPicturePng() {
    static const std::vector<uint8_t> kPng = []() {
        std::vector<uint8_t> out;
        static constexpr uint8_t kSig[8] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
        out.insert(out.end(), kSig, kSig + 8);

        // IHDR: 1x1, 8-bit RGBA (color type 6)
        uint8_t ihdr[13] = {
            0, 0, 0, 1, // width = 1
            0, 0, 0, 1, // height = 1
            8,          // bit depth
            6,          // RGBA
            0, 0, 0     // compression, filter, interlace
        };
        png_append_chunk(out, "IHDR", ihdr, sizeof(ihdr));

        // Raw scanline: filter=0 + RGBA(0x89, 0x57, 0xE5, 0xFF)
        const uint8_t raw[5] = {0x00u, 0x89u, 0x57u, 0xE5u, 0xFFu};
        uint32_t s1 = 1u, s2 = 0u;
        for (uint8_t b : raw) {
            s1 = (s1 + b) % 65521u;
            s2 = (s2 + s1) % 65521u;
        }
        const uint32_t adler = (s2 << 16) | s1;

        std::vector<uint8_t> zlib;
        zlib.push_back(0x78u);
        zlib.push_back(0x01u);
        zlib.push_back(0x01u); // BFINAL=1, BTYPE=0 (stored)
        zlib.push_back(5u);
        zlib.push_back(0u);
        zlib.push_back(static_cast<uint8_t>(~5u & 0xFFu));
        zlib.push_back(0xFFu);
        zlib.insert(zlib.end(), raw, raw + sizeof(raw));
        png_append_be32(zlib, adler);
        png_append_chunk(out, "IDAT", zlib.data(), zlib.size());
        png_append_chunk(out, "IEND", nullptr, 0u);
        return out;
    }();
    return kPng;
}

uint64_t fnv1a_64(const void* data, size_t len, uint64_t seed = 14695981039346656037ULL) noexcept {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

std::string MakeOfflineXblToken(uint64_t xuid) {
    char buf[128]{};
    const uint64_t uhs = fnv1a_64(&xuid, sizeof(xuid)) & 0xFFFFFFFFFFFFULL;
    std::snprintf(buf, sizeof(buf), "XBL3.0 x=%012llX;wsx-offline-%016llX",
                  static_cast<unsigned long long>(uhs),
                  static_cast<unsigned long long>(xuid));
    return std::string(buf);
}

std::string Base64EncodeBytes(const uint8_t* data, size_t len) {
    static constexpr char kTbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2u) / 3u) * 4u);
    size_t i = 0u;
    while (i + 3u <= len) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16u) |
                           (static_cast<uint32_t>(data[i + 1u]) << 8u) |
                           static_cast<uint32_t>(data[i + 2u]);
        out.push_back(kTbl[(v >> 18u) & 63u]);
        out.push_back(kTbl[(v >> 12u) & 63u]);
        out.push_back(kTbl[(v >> 6u) & 63u]);
        out.push_back(kTbl[v & 63u]);
        i += 3u;
    }
    const size_t rem = len - i;
    if (rem == 1u) {
        const uint32_t v = static_cast<uint32_t>(data[i]) << 16u;
        out.push_back(kTbl[(v >> 18u) & 63u]);
        out.push_back(kTbl[(v >> 12u) & 63u]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2u) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16u) |
                           (static_cast<uint32_t>(data[i + 1u]) << 8u);
        out.push_back(kTbl[(v >> 18u) & 63u]);
        out.push_back(kTbl[(v >> 12u) & 63u]);
        out.push_back(kTbl[(v >> 6u) & 63u]);
        out.push_back('=');
    }
    return out;
}

// Offline ephemeral Windows CNG (BCrypt) ECDSA P-256 + SHA-256 signer producing
// structurally valid Xbox Live XSTS v1 signatures: BE32(1) || BE64(FILETIME) || r(32) || s(32) (76 bytes -> 104-char Base64).
class OfflineXstsEcdsaSigner {
    std::mutex mtx_;
    BCRYPT_ALG_HANDLE ecdsa_alg_ = nullptr;
    BCRYPT_ALG_HANDLE sha256_alg_ = nullptr;
    BCRYPT_KEY_HANDLE key_ = nullptr;
    bool initialized_ = false;

    void ensure_init_locked() noexcept {
        if (initialized_) return;
        initialized_ = true;
        if (BCryptOpenAlgorithmProvider(&sha256_alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
            sha256_alg_ = nullptr;
        }
        if (BCryptOpenAlgorithmProvider(&ecdsa_alg_, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) == 0 && ecdsa_alg_) {
            if (BCryptGenerateKeyPair(ecdsa_alg_, &key_, 256u, 0) == 0 && key_) {
                if (BCryptFinalizeKeyPair(key_, 0) != 0) {
                    BCryptDestroyKey(key_);
                    key_ = nullptr;
                }
            }
        }
    }

public:
    ~OfflineXstsEcdsaSigner() noexcept {
        if (key_) BCryptDestroyKey(key_);
        if (ecdsa_alg_) BCryptCloseAlgorithmProvider(ecdsa_alg_, 0);
        if (sha256_alg_) BCryptCloseAlgorithmProvider(sha256_alg_, 0);
    }

    std::string sign_xsts_v1_base64(uint64_t xuid, const std::string& method, const std::string& url) {
        std::lock_guard<std::mutex> lk(mtx_);
        ensure_init_locked();

        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        const uint64_t ft64 = (static_cast<uint64_t>(ft.dwHighDateTime) << 32u) |
                              static_cast<uint64_t>(ft.dwLowDateTime);

        uint8_t header[12]{};
        // Version = 1 (big-endian u32)
        header[0] = 0u; header[1] = 0u; header[2] = 0u; header[3] = 1u;
        // FILETIME timestamp (big-endian u64)
        for (int i = 0; i < 8; ++i) {
            header[4 + i] = static_cast<uint8_t>((ft64 >> (56 - i * 8)) & 0xFFu);
        }

        std::vector<uint8_t> msg;
        msg.reserve(12u + 1u + method.size() + 1u + url.size() + 1u + sizeof(xuid));
        msg.insert(msg.end(), header, header + 12u);
        msg.push_back(0u);
        msg.insert(msg.end(), method.begin(), method.end());
        msg.push_back(0u);
        msg.insert(msg.end(), url.begin(), url.end());
        msg.push_back(0u);
        const auto* xp = reinterpret_cast<const uint8_t*>(&xuid);
        msg.insert(msg.end(), xp, xp + sizeof(xuid));

        uint8_t digest[32]{};
        bool hashed = false;
        if (sha256_alg_) {
            BCRYPT_HASH_HANDLE hh = nullptr;
            if (BCryptCreateHash(sha256_alg_, &hh, nullptr, 0, nullptr, 0, 0) == 0 && hh) {
                if (BCryptHashData(hh, msg.data(), static_cast<ULONG>(msg.size()), 0) == 0 &&
                    BCryptFinishHash(hh, digest, sizeof(digest), 0) == 0) {
                    hashed = true;
                }
                BCryptDestroyHash(hh);
            }
        }

        uint8_t sig_raw[64]{};
        ULONG sig_len = 0u;
        bool signed_ok = false;
        if (hashed && key_) {
            if (BCryptSignHash(key_, nullptr, digest, sizeof(digest),
                               sig_raw, sizeof(sig_raw), &sig_len, 0) == 0 &&
                sig_len == 64u) {
                signed_ok = true;
            }
        }
        if (!signed_ok) {
            // Deterministic fallback if CNG provider is unavailable
            uint64_t h = fnv1a_64(msg.data(), msg.size());
            for (size_t k = 0u; k < 8u; ++k) {
                h = fnv1a_64(&k, sizeof(k), h);
                std::memcpy(sig_raw + k * 8u, &h, 8u);
            }
        }

        uint8_t packet[76]{};
        std::memcpy(packet, header, 12u);
        std::memcpy(packet + 12u, sig_raw, 64u);
        return Base64EncodeBytes(packet, sizeof(packet));
    }
};

OfflineXstsEcdsaSigner& GetOfflineEcdsaSigner() noexcept {
    static OfflineXstsEcdsaSigner s;
    return s;
}

std::string MakeOfflineXblSignature(uint64_t xuid, const std::string& method, const std::string& url) {
    const std::string b64 = GetOfflineEcdsaSigner().sign_xsts_v1_base64(xuid, method, url);
    const std::string mode = wsx::detail::to_lower_ascii(
        wsx::get_config_string("WSX_XBL_SIG_MODE", "Console", "xbl_sig_mode", "wsx"));
    if (mode == "xsts" || mode == "raw" || mode == "ecdsa") {
        return b64;
    }
    return "WSXSIG-" + b64;
}

std::wstring Utf8ToWideStr(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string WideToUtf8Str(const wchar_t* s) {
    if (!s || !s[0]) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// ----------------------------------------------------------------------------
// XStore Local State
// ----------------------------------------------------------------------------

struct StoreLicenseChangeReg {
    uint64_t token = 0;
    XStoreContextHandle ctx = nullptr;
    XTaskQueueHandle queue = nullptr;
    void* context = nullptr;
    XStoreGameLicenseChangedCallback* callback = nullptr;
};

std::mutex g_store_mtx;
std::vector<StoreLicenseChangeReg> g_store_license_regs;

std::vector<std::string> GetConfiguredStoreAddons() {
    std::vector<std::string> out;
    const std::string raw = wsx::get_config_string("WSX_STORE_ADDONS", "Store", "addons", "");
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t comma = raw.find(',', pos);
        if (comma == std::string::npos) comma = raw.size();
        std::string item = wsx::detail::trim_ascii(raw.substr(pos, comma - pos));
        if (!item.empty()) out.push_back(std::move(item));
        pos = comma + 1u;
    }
    return out;
}

// ----------------------------------------------------------------------------
// Local Persistent XSAPI Store (JSON next to saves — ZERO network I/O)
// ----------------------------------------------------------------------------

struct LocalXsapiState {
    std::mutex mtx;
    bool initialized = false;
    std::string scid = "00000000-0000-0000-0000-000076029b4d";
    uint32_t defaultTitleId = 0x76029B4Du;
    fs::path storageDir;
    // Per-(xuid, titleId) achievements and per-xuid presence cached in memory & persisted to JSON
    std::unordered_map<uint64_t, std::vector<XblAchievementOwned>> achievementsByKey;
    std::unordered_map<uint64_t, XblPresenceRecord> presenceByXuid;
};

LocalXsapiState& XsapiState() noexcept {
    static LocalXsapiState st;
    return st;
}

fs::path ResolveXsapiDirectory() {
    const std::string cfg = wsx::get_config_string("WSX_XSAPI_DIR", "Storage", "xsapi_dir", "");
    fs::path dir;
    if (!cfg.empty()) {
        dir = Utf8ToPath(cfg);
    } else {
        dir = GetBaseSaveDirectory().parent_path() / L"xsapi_local";
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::string JsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8u);
    for (char c : in) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) >= 0x20u) out.push_back(c);
            break;
        }
    }
    return out;
}

std::string ExtractJsonStringField(const std::string& obj, const char* key) {
    const std::string pat = std::string("\"") + key + "\":\"";
    const size_t p = obj.find(pat);
    if (p == std::string::npos) return {};
    size_t i = p + pat.size();
    std::string out;
    while (i < obj.size()) {
        if (obj[i] == '\\' && i + 1u < obj.size()) {
            const char nxt = obj[i + 1u];
            if (nxt == 'n') out.push_back('\n');
            else if (nxt == 'r') out.push_back('\r');
            else if (nxt == 't') out.push_back('\t');
            else out.push_back(nxt);
            i += 2u;
        } else if (obj[i] == '"') {
            break;
        } else {
            out.push_back(obj[i++]);
        }
    }
    return out;
}

uint64_t ExtractJsonUintField(const std::string& obj, const char* key, uint64_t fallback = 0) noexcept {
    const std::string pat = std::string("\"") + key + "\":";
    const size_t p = obj.find(pat);
    if (p == std::string::npos) return fallback;
    const char* s = obj.c_str() + p + pat.size();
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    return (end && end != s) ? static_cast<uint64_t>(v) : fallback;
}

fs::path AchievementsJsonPath(const fs::path& dir, uint64_t xuid, uint32_t titleId) {
    wchar_t name[128]{};
    std::swprintf(name, std::size(name), L"achievements_%016llX_%08X.json",
                  static_cast<unsigned long long>(xuid), titleId);
    return dir / name;
}

fs::path PresenceJsonPath(const fs::path& dir, uint64_t xuid) {
    wchar_t name[96]{};
    std::swprintf(name, std::size(name), L"presence_%016llX.json",
                  static_cast<unsigned long long>(xuid));
    return dir / name;
}

fs::path ProfileJsonPath(const fs::path& dir, uint64_t xuid) {
    wchar_t name[96]{};
    std::swprintf(name, std::size(name), L"profile_%016llX.json",
                  static_cast<unsigned long long>(xuid));
    return dir / name;
}

uint64_t MakeAchMapKey(uint64_t xuid, uint32_t titleId) noexcept {
    return (xuid * 1315423911ULL) ^ static_cast<uint64_t>(titleId);
}

std::vector<XblAchievementOwned> LoadAchievementsLocked(const fs::path& dir, uint64_t xuid, uint32_t titleId) {
    std::vector<XblAchievementOwned> list;
    const fs::path p = AchievementsJsonPath(dir, xuid, titleId);
    std::ifstream in(p, std::ios::binary);
    if (!in) return list;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    while (pos < content.size()) {
        const size_t open = content.find('{', pos);
        if (open == std::string::npos) break;
        const size_t close = content.find('}', open);
        if (close == std::string::npos) break;
        const std::string obj = content.substr(open, close - open + 1u);
        const std::string id = ExtractJsonStringField(obj, "id");
        if (!id.empty()) {
            XblAchievementOwned a{};
            a.id = id;
            a.scid = ExtractJsonStringField(obj, "scid");
            a.name = ExtractJsonStringField(obj, "name");
            a.unlockedDesc = ExtractJsonStringField(obj, "unlockedDesc");
            a.lockedDesc = ExtractJsonStringField(obj, "lockedDesc");
            a.titleId = static_cast<uint32_t>(ExtractJsonUintField(obj, "titleId", titleId));
            a.gamerscore = static_cast<uint32_t>(ExtractJsonUintField(obj, "gamerscore", 15));
            a.percentComplete = static_cast<uint32_t>(ExtractJsonUintField(obj, "percentComplete", 0));
            a.timeUnlocked = static_cast<time_t>(ExtractJsonUintField(obj, "timeUnlocked", 0));
            a.progressState = static_cast<XblAchievementProgressState>(
                ExtractJsonUintField(obj, "progressState", static_cast<uint64_t>(XblAchievementProgressState::NotStarted)));
            a.isSecret = ExtractJsonUintField(obj, "isSecret", 0) != 0;
            list.push_back(std::move(a));
        }
        pos = close + 1u;
    }
    return list;
}

void SaveAchievementsLocked(const fs::path& dir, uint64_t xuid, uint32_t titleId, const std::vector<XblAchievementOwned>& list) {
    std::string json = "[\n";
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& a = list[i];
        char numbuf[256]{};
        std::snprintf(
            numbuf, sizeof(numbuf),
            "\"titleId\":%u,\"gamerscore\":%u,\"percentComplete\":%u,\"progressState\":%u,\"timeUnlocked\":%llu,\"isSecret\":%u",
            a.titleId, a.gamerscore, a.percentComplete,
            static_cast<unsigned>(a.progressState),
            static_cast<unsigned long long>(a.timeUnlocked),
            a.isSecret ? 1u : 0u);
        json += "  {\"id\":\"" + JsonEscape(a.id) +
                "\",\"scid\":\"" + JsonEscape(a.scid) +
                "\",\"name\":\"" + JsonEscape(a.name) +
                "\",\"unlockedDesc\":\"" + JsonEscape(a.unlockedDesc) +
                "\",\"lockedDesc\":\"" + JsonEscape(a.lockedDesc) + "\"," +
                numbuf + "}";
        if (i + 1u < list.size()) json += ",";
        json += "\n";
    }
    json += "]\n";
    AtomicWriteBinaryFile(AchievementsJsonPath(dir, xuid, titleId),
                          reinterpret_cast<const uint8_t*>(json.data()), json.size());
}

void SavePresenceLocked(const fs::path& dir, const XblPresenceRecord& rec) {
    char buf[512]{};
    std::snprintf(
        buf, sizeof(buf),
        "{\"xuid\":%llu,\"userState\":%u,\"titleId\":%u,\"richPresenceId\":\"%s\",\"lastUpdated\":%llu}\n",
        static_cast<unsigned long long>(rec.xuid),
        static_cast<unsigned>(rec.userState),
        rec.titleId,
        JsonEscape(rec.richPresenceId).c_str(),
        static_cast<unsigned long long>(rec.lastUpdated));
    const std::string s(buf);
    AtomicWriteBinaryFile(PresenceJsonPath(dir, rec.xuid),
                          reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

bool LoadPresenceLocked(const fs::path& dir, uint64_t xuid, XblPresenceRecord& out) {
    std::ifstream in(PresenceJsonPath(dir, xuid), std::ios::binary);
    if (!in) return false;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    out = {};
    out.xuid = xuid;
    out.userState = static_cast<XblPresenceUserState>(
        ExtractJsonUintField(content, "userState", static_cast<uint64_t>(XblPresenceUserState::Online)));
    out.titleId = static_cast<uint32_t>(ExtractJsonUintField(content, "titleId", 0));
    out.lastUpdated = static_cast<time_t>(ExtractJsonUintField(content, "lastUpdated", 0));
    const std::string rp = ExtractJsonStringField(content, "richPresenceId");
    strncpy_s(out.richPresenceId, sizeof(out.richPresenceId), rp.c_str(), _TRUNCATE);
    return true;
}

uint32_t ComputeTotalGamerscoreLocked(const fs::path& dir, uint64_t xuid) {
    uint32_t total = 0;
    std::error_code ec;
    wchar_t prefix[64]{};
    std::swprintf(prefix, std::size(prefix), L"achievements_%016llX_", static_cast<unsigned long long>(xuid));
    const std::wstring pref(prefix);
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec || !entry.is_regular_file(ec)) continue;
        const std::wstring fn = entry.path().filename().wstring();
        if (fn.rfind(pref, 0) == 0 && entry.path().extension() == L".json") {
            std::ifstream in(entry.path(), std::ios::binary);
            if (!in) continue;
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            size_t pos = 0;
            while (pos < content.size()) {
                const size_t open = content.find('{', pos);
                if (open == std::string::npos) break;
                const size_t close = content.find('}', open);
                if (close == std::string::npos) break;
                const std::string obj = content.substr(open, close - open + 1u);
                const uint64_t st = ExtractJsonUintField(obj, "progressState", 0);
                if (st == static_cast<uint64_t>(XblAchievementProgressState::Achieved)) {
                    total += static_cast<uint32_t>(ExtractJsonUintField(obj, "gamerscore", 15));
                }
                pos = close + 1u;
            }
        }
    }
    return total;
}

void SaveProfileSnapshotLocked(const fs::path& dir, const XblUserProfile& p) {
    std::string json = "{";
    json += "\"xuid\":" + std::to_string(p.xboxUserId);
    json += ",\"gamertag\":\"" + JsonEscape(p.gamertag) + "\"";
    json += ",\"modernGamertag\":\"" + JsonEscape(p.modernGamertag) + "\"";
    json += ",\"gamerscore\":\"" + JsonEscape(p.gamerscore) + "\"";
    json += ",\"accountTier\":\"" + JsonEscape(p.accountTier) + "\"";
    json += ",\"reputation\":\"" + JsonEscape(p.reputation) + "\"}\n";
    AtomicWriteBinaryFile(ProfileJsonPath(dir, p.xboxUserId),
                          reinterpret_cast<const uint8_t*>(json.data()), json.size());
}

void FillOfflineProfile(uint64_t xuid, const fs::path& dir, XblUserProfile& out) {
    std::memset(&out, 0, sizeof(out));
    out.xboxUserId = xuid;

    XUserHandle u = GetOrCreateDefaultUser();
    std::string classic = "OfflinePlayer";
    std::string modern = "OfflinePlayer";
    std::string suffix;
    if (u) {
        if (xuid == 0 || xuid == u->xuid) {
            out.xboxUserId = u->xuid;
            classic = u->classicGamertag;
            modern = u->modernGamertag;
            suffix = u->suffix;
        } else {
            char tmp[32]{};
            std::snprintf(tmp, sizeof(tmp), "Player_%04llX", static_cast<unsigned long long>(xuid & 0xFFFFULL));
            classic = tmp;
            modern = tmp;
        }
        XUserCloseHandle(u);
    }

    const uint32_t gs = 1000u + ComputeTotalGamerscoreLocked(dir, out.xboxUserId);
    char gsStr[16]{};
    std::snprintf(gsStr, sizeof(gsStr), "%u", gs);

    strncpy_s(out.gamertag, sizeof(out.gamertag), classic.c_str(), _TRUNCATE);
    strncpy_s(out.modernGamertag, sizeof(out.modernGamertag), modern.c_str(), _TRUNCATE);
    strncpy_s(out.modernGamertagSuffix, sizeof(out.modernGamertagSuffix), suffix.c_str(), _TRUNCATE);
    strncpy_s(out.uniqueModernGamertag, sizeof(out.uniqueModernGamertag), modern.c_str(), _TRUNCATE);
    strncpy_s(out.appDisplayName, sizeof(out.appDisplayName), classic.c_str(), _TRUNCATE);
    strncpy_s(out.gameDisplayName, sizeof(out.gameDisplayName), modern.c_str(), _TRUNCATE);
    strncpy_s(out.appDisplayPictureResizeUri, sizeof(out.appDisplayPictureResizeUri),
              "local://mizubox/gamerpic.png", _TRUNCATE);
    strncpy_s(out.gameDisplayPictureResizeUri, sizeof(out.gameDisplayPictureResizeUri),
              "local://mizubox/gamerpic.png", _TRUNCATE);
    strncpy_s(out.gamerscore, sizeof(out.gamerscore), gsStr, _TRUNCATE);
    strncpy_s(out.accountTier, sizeof(out.accountTier), "Gold", _TRUNCATE);
    strncpy_s(out.reputation, sizeof(out.reputation), "GoodPlayer", _TRUNCATE);

    SaveProfileSnapshotLocked(dir, out);
}

} // namespace

extern "C" {

// ============================================================================
// Extended XUser Exports
// ============================================================================

HRESULT STDAPICALLTYPE XUserGetTokenAndSignatureAsync(
    XUserHandle user,
    XUserGetTokenAndSignatureOptions /*options*/,
    const char* method,
    const char* url,
    size_t /*headerCount*/,
    const XUserGetTokenAndSignatureHttpHeader* /*headers*/,
    size_t /*bodySize*/,
    const void* /*bodyBuffer*/,
    XAsyncBlock* async) noexcept {
    if (!IsValidUser(user) || !async) return E_INVALIDARG;

    struct TokenCtx {
        std::string token;
        std::string signature;
    };
    auto* ctx = new (std::nothrow) TokenCtx{
        MakeOfflineXblToken(user->xuid),
        MakeOfflineXblSignature(user->xuid, method ? method : "GET", url ? url : "")
    };
    if (!ctx) return E_OUTOFMEMORY;

    const HRESULT hr = XAsyncBegin(
        async, ctx, reinterpret_cast<const void*>(&XUserGetTokenAndSignatureAsync),
        "XUserGetTokenAndSignatureAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<TokenCtx*>(data->context);
            if (op == XAsyncOp::DoWork) {
                const size_t req = sizeof(XUserGetTokenAndSignatureData) +
                                   c->token.size() + 1u + c->signature.size() + 1u;
                XAsyncComplete(data->async, S_OK, req);
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                const size_t req = sizeof(XUserGetTokenAndSignatureData) +
                                   c->token.size() + 1u + c->signature.size() + 1u;
                if (!data->buffer || data->bufferSize < req) return E_NOT_SUFFICIENT_BUFFER;
                auto* out = static_cast<XUserGetTokenAndSignatureData*>(data->buffer);
                char* dst = reinterpret_cast<char*>(out + 1);
                std::memcpy(dst, c->token.c_str(), c->token.size() + 1u);
                out->token = dst;
                out->tokenSize = c->token.size() + 1u;
                dst += out->tokenSize;
                std::memcpy(dst, c->signature.c_str(), c->signature.size() + 1u);
                out->signature = dst;
                out->signatureSize = c->signature.size() + 1u;
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) {
                delete c;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return wsx::trace_hr(kMod, "XUserGetTokenAndSignatureAsync", XAsyncSchedule(async, 0));
}

HRESULT STDAPICALLTYPE XUserGetTokenAndSignatureResultSize(
    XAsyncBlock* async,
    size_t* bufferSize) noexcept {
    return XAsyncGetResultSize(async, bufferSize);
}

HRESULT STDAPICALLTYPE XUserGetTokenAndSignatureResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XUserGetTokenAndSignatureData** ptrToBuffer,
    size_t* bufferUsed) noexcept {
    if (!async || !buffer || !ptrToBuffer) return E_INVALIDARG;
    *ptrToBuffer = nullptr;
    const HRESULT hr = XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XUserGetTokenAndSignatureAsync),
        bufferSize, buffer, bufferUsed);
    if (SUCCEEDED(hr)) {
        *ptrToBuffer = static_cast<XUserGetTokenAndSignatureData*>(buffer);
    }
    return hr;
}

HRESULT STDAPICALLTYPE XUserGetTokenAndSignatureUtf16Async(
    XUserHandle user,
    XUserGetTokenAndSignatureOptions /*options*/,
    const wchar_t* method,
    const wchar_t* url,
    size_t /*headerCount*/,
    const XUserGetTokenAndSignatureUtf16HttpHeader* /*headers*/,
    size_t /*bodySize*/,
    const void* /*bodyBuffer*/,
    XAsyncBlock* async) noexcept {
    if (!IsValidUser(user) || !async) return E_INVALIDARG;

    struct TokenWCtx {
        std::wstring token;
        std::wstring signature;
    };
    auto* ctx = new (std::nothrow) TokenWCtx{
        Utf8ToWideStr(MakeOfflineXblToken(user->xuid)),
        Utf8ToWideStr(MakeOfflineXblSignature(user->xuid, WideToUtf8Str(method), WideToUtf8Str(url)))
    };
    if (!ctx) return E_OUTOFMEMORY;

    const HRESULT hr = XAsyncBegin(
        async, ctx, reinterpret_cast<const void*>(&XUserGetTokenAndSignatureUtf16Async),
        "XUserGetTokenAndSignatureUtf16Async",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<TokenWCtx*>(data->context);
            if (op == XAsyncOp::DoWork) {
                const size_t req = sizeof(XUserGetTokenAndSignatureUtf16Data) +
                                   (c->token.size() + 1u + c->signature.size() + 1u) * sizeof(wchar_t);
                XAsyncComplete(data->async, S_OK, req);
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                const size_t req = sizeof(XUserGetTokenAndSignatureUtf16Data) +
                                   (c->token.size() + 1u + c->signature.size() + 1u) * sizeof(wchar_t);
                if (!data->buffer || data->bufferSize < req) return E_NOT_SUFFICIENT_BUFFER;
                auto* out = static_cast<XUserGetTokenAndSignatureUtf16Data*>(data->buffer);
                auto* dst = reinterpret_cast<wchar_t*>(out + 1);
                std::memcpy(dst, c->token.c_str(), (c->token.size() + 1u) * sizeof(wchar_t));
                out->token = dst;
                out->tokenCount = c->token.size() + 1u;
                dst += out->tokenCount;
                std::memcpy(dst, c->signature.c_str(), (c->signature.size() + 1u) * sizeof(wchar_t));
                out->signature = dst;
                out->signatureCount = c->signature.size() + 1u;
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) {
                delete c;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return wsx::trace_hr(kMod, "XUserGetTokenAndSignatureUtf16Async", XAsyncSchedule(async, 0));
}

HRESULT STDAPICALLTYPE XUserGetTokenAndSignatureUtf16ResultSize(
    XAsyncBlock* async,
    size_t* bufferSize) noexcept {
    return XAsyncGetResultSize(async, bufferSize);
}

HRESULT STDAPICALLTYPE XUserGetTokenAndSignatureUtf16Result(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XUserGetTokenAndSignatureUtf16Data** ptrToBuffer,
    size_t* bufferUsed) noexcept {
    if (!async || !buffer || !ptrToBuffer) return E_INVALIDARG;
    *ptrToBuffer = nullptr;
    const HRESULT hr = XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XUserGetTokenAndSignatureUtf16Async),
        bufferSize, buffer, bufferUsed);
    if (SUCCEEDED(hr)) {
        *ptrToBuffer = static_cast<XUserGetTokenAndSignatureUtf16Data*>(buffer);
    }
    return hr;
}

HRESULT STDAPICALLTYPE XUserGetGamerPictureAsync(
    XUserHandle user,
    XUserGamerPictureSize /*pictureSize*/,
    XAsyncBlock* async) noexcept {
    if (!IsValidUser(user) || !async) return E_INVALIDARG;
    const HRESULT hr = XAsyncBegin(
        async, nullptr, reinterpret_cast<const void*>(&XUserGetGamerPictureAsync),
        "XUserGetGamerPictureAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            const auto& png = GetOfflineGamerPicturePng();
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, png.size());
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                if (!data->buffer || data->bufferSize < png.size()) return E_NOT_SUFFICIENT_BUFFER;
                std::memcpy(data->buffer, png.data(), png.size());
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) return hr;
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XUserGetGamerPictureResultSize(
    XAsyncBlock* async,
    size_t* bufferSize) noexcept {
    return XAsyncGetResultSize(async, bufferSize);
}

HRESULT STDAPICALLTYPE XUserGetGamerPictureResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    size_t* bufferUsed) noexcept {
    if (!async || !buffer) return E_INVALIDARG;
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XUserGetGamerPictureAsync),
        bufferSize, buffer, bufferUsed);
}

HRESULT STDAPICALLTYPE XUserResolvePrivilegeWithUiAsync(
    XUserHandle user,
    XUserPrivilegeOptions /*options*/,
    XUserPrivilege /*privilege*/,
    XAsyncBlock* async) noexcept {
    if (!IsValidUser(user) || !async) return E_INVALIDARG;
    const HRESULT hr = XAsyncBegin(
        async, nullptr, reinterpret_cast<const void*>(&XUserResolvePrivilegeWithUiAsync),
        "XUserResolvePrivilegeWithUiAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, 0);
            }
            return S_OK;
        });
    if (FAILED(hr)) return hr;
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XUserResolvePrivilegeWithUiResult(
    XAsyncBlock* async) noexcept {
    return XAsyncGetResult(async, reinterpret_cast<const void*>(&XUserResolvePrivilegeWithUiAsync), 0, nullptr, nullptr);
}

HRESULT STDAPICALLTYPE XUserResolveIssueWithUiUtf16Async(
    XUserHandle user,
    const wchar_t* /*url*/,
    XAsyncBlock* async) noexcept {
    if (!IsValidUser(user) || !async) return E_INVALIDARG;
    const HRESULT hr = XAsyncBegin(
        async, nullptr, reinterpret_cast<const void*>(&XUserResolveIssueWithUiUtf16Async),
        "XUserResolveIssueWithUiUtf16Async",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, 0);
            }
            return S_OK;
        });
    if (FAILED(hr)) return hr;
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XUserResolveIssueWithUiUtf16Result(
    XAsyncBlock* async) noexcept {
    return XAsyncGetResult(async, reinterpret_cast<const void*>(&XUserResolveIssueWithUiUtf16Async), 0, nullptr, nullptr);
}

HRESULT STDAPICALLTYPE XUserGetSignOutDeferral(
    XUserSignOutDeferralHandle* deferral) noexcept {
    if (!deferral) return E_INVALIDARG;
    auto* d = new (std::nothrow) XUserSignOutDeferral{g_next_token.fetch_add(1, std::memory_order_relaxed)};
    if (!d) return E_OUTOFMEMORY;
    *deferral = d;
    return S_OK;
}

void STDAPICALLTYPE XUserCloseSignOutDeferralHandle(
    XUserSignOutDeferralHandle deferral) noexcept {
    delete deferral;
}

// ============================================================================
// Offline XStore Exports
// ============================================================================

HRESULT STDAPICALLTYPE XStoreCreateContext(
    XUserHandle user,
    XStoreContextHandle* storeContextHandle) noexcept {
    if (!storeContextHandle) return E_INVALIDARG;
    *storeContextHandle = nullptr;
    auto* ctx = new (std::nothrow) XStoreContext();
    if (!ctx) return E_OUTOFMEMORY;
    if (IsValidUser(user)) {
        XUserDuplicateHandle(user, &ctx->user);
        ctx->xuid = user->xuid;
    } else {
        ctx->user = GetOrCreateDefaultUser();
        ctx->xuid = ctx->user ? ctx->user->xuid : 0ULL;
    }
    *storeContextHandle = ctx;
    return wsx::trace_hr(kMod, "XStoreCreateContext", S_OK, "ctx=%p", static_cast<void*>(ctx));
}

void STDAPICALLTYPE XStoreCloseContextHandle(
    XStoreContextHandle storeContextHandle) noexcept {
    if (!storeContextHandle) return;
    if (storeContextHandle->refCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (storeContextHandle->user) XUserCloseHandle(storeContextHandle->user);
        delete storeContextHandle;
    }
}

HRESULT STDAPICALLTYPE XStoreQueryGameLicenseAsync(
    XStoreContextHandle storeContextHandle,
    XAsyncBlock* async) noexcept {
    if (!storeContextHandle || !async) return E_INVALIDARG;
    const HRESULT hr = XAsyncBegin(
        async, nullptr, reinterpret_cast<const void*>(&XStoreQueryGameLicenseAsync),
        "XStoreQueryGameLicenseAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, sizeof(XStoreGameLicense));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                if (!data->buffer || data->bufferSize < sizeof(XStoreGameLicense)) return E_NOT_SUFFICIENT_BUFFER;
                auto* lic = static_cast<XStoreGameLicense*>(data->buffer);
                std::memset(lic, 0, sizeof(*lic));
                const std::string sku = wsx::get_config_string("WSX_STORE_SKU", "Store", "sku_id", "0010");
                strncpy_s(lic->skuStoreId, sizeof(lic->skuStoreId), sku.c_str(), _TRUNCATE);
                lic->isActive = true;
                lic->isTrial = false;
                lic->isTrialOwnedByThisUser = false;
                lic->isDiscLicense = false;
                lic->trialTimeRemainingInSeconds = 0;
                lic->expirationDate = 0;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) return hr;
    return wsx::trace_hr(kMod, "XStoreQueryGameLicenseAsync", XAsyncSchedule(async, 0));
}

HRESULT STDAPICALLTYPE XStoreQueryGameLicenseResult(
    XAsyncBlock* async,
    XStoreGameLicense* license) noexcept {
    if (!async || !license) return E_INVALIDARG;
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XStoreQueryGameLicenseAsync),
        sizeof(XStoreGameLicense), license, nullptr);
}

HRESULT STDAPICALLTYPE XStoreQueryAddOnLicensesAsync(
    XStoreContextHandle storeContextHandle,
    XAsyncBlock* async) noexcept {
    if (!storeContextHandle || !async) return E_INVALIDARG;

    struct AddonCtx {
        std::vector<XStoreAddonLicense> addons;
    };
    auto* ctx = new (std::nothrow) AddonCtx();
    if (!ctx) return E_OUTOFMEMORY;
    for (const auto& id : GetConfiguredStoreAddons()) {
        XStoreAddonLicense a{};
        strncpy_s(a.skuStoreId, sizeof(a.skuStoreId), id.c_str(), _TRUNCATE);
        strncpy_s(a.inAppOfferToken, sizeof(a.inAppOfferToken), id.c_str(), _TRUNCATE);
        a.isActive = true;
        a.expirationDate = 0;
        ctx->addons.push_back(a);
    }

    const HRESULT hr = XAsyncBegin(
        async, ctx, reinterpret_cast<const void*>(&XStoreQueryAddOnLicensesAsync),
        "XStoreQueryAddOnLicensesAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<AddonCtx*>(data->context);
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, c->addons.size() * sizeof(XStoreAddonLicense));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                const size_t bytes = c->addons.size() * sizeof(XStoreAddonLicense);
                if (bytes > 0u) {
                    if (!data->buffer || data->bufferSize < bytes) return E_NOT_SUFFICIENT_BUFFER;
                    std::memcpy(data->buffer, c->addons.data(), bytes);
                }
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) {
                delete c;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XStoreQueryAddOnLicensesResultCount(
    XAsyncBlock* async,
    uint32_t* count) noexcept {
    if (!async || !count) return E_INVALIDARG;
    size_t sz = 0;
    const HRESULT hr = XAsyncGetResultSize(async, &sz);
    if (FAILED(hr)) return hr;
    *count = static_cast<uint32_t>(sz / sizeof(XStoreAddonLicense));
    return S_OK;
}

HRESULT STDAPICALLTYPE XStoreQueryAddOnLicensesResult(
    XAsyncBlock* async,
    uint32_t count,
    XStoreAddonLicense* addOnLicenses) noexcept {
    if (!async) return E_INVALIDARG;
    if (count > 0u && !addOnLicenses) return E_INVALIDARG;
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XStoreQueryAddOnLicensesAsync),
        static_cast<size_t>(count) * sizeof(XStoreAddonLicense), addOnLicenses, nullptr);
}

HRESULT STDAPICALLTYPE XStoreQueryProductForCurrentGameAsync(
    XStoreContextHandle storeContextHandle,
    XAsyncBlock* async) noexcept {
    if (!storeContextHandle || !async) return E_INVALIDARG;
    const HRESULT hr = XAsyncBegin(
        async, nullptr, reinterpret_cast<const void*>(&XStoreQueryProductForCurrentGameAsync),
        "XStoreQueryProductForCurrentGameAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, sizeof(XStoreProduct));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                if (!data->buffer || data->bufferSize < sizeof(XStoreProduct)) return E_NOT_SUFFICIENT_BUFFER;
                static const std::string kStoreId = wsx::get_config_string("WSX_STORE_ID", "Store", "product_id", "9WSXLOCAL0001");
                static const std::string kTitle = wsx::get_config_string("WSX_TITLE_NAME", "Game", "title_name", "MizuBox Title");
                auto* prod = static_cast<XStoreProduct*>(data->buffer);
                std::memset(prod, 0, sizeof(*prod));
                prod->storeId = kStoreId.c_str();
                prod->title = kTitle.c_str();
                prod->description = "Local offline title license";
                prod->language = "en-US";
                prod->inAppOfferToken = "";
                prod->linkUri = "ms-windows-store://pdp/?productid=9WSXLOCAL0001";
                prod->productKind = XStoreProductKind::Game;
                strncpy_s(prod->price.currencyCode, sizeof(prod->price.currencyCode), "USD", _TRUNCATE);
                prod->price.formattedBasePrice = "$0.00";
                prod->price.formattedPrice = "$0.00";
                prod->price.formattedRecurrencePrice = "";
                prod->hasDigitalDownload = true;
                prod->isInUserCollection = true;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) return hr;
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XStoreQueryProductForCurrentGameResult(
    XAsyncBlock* async,
    XStoreProduct* product) noexcept {
    if (!async || !product) return E_INVALIDARG;
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XStoreQueryProductForCurrentGameAsync),
        sizeof(XStoreProduct), product, nullptr);
}

HRESULT STDAPICALLTYPE XStoreCanAcquireLicenseForStoreIdAsync(
    XStoreContextHandle storeContextHandle,
    const char* storeProductId,
    XAsyncBlock* async) noexcept {
    if (!storeContextHandle || !storeProductId || !async) return E_INVALIDARG;
    const HRESULT hr = XAsyncBegin(
        async, nullptr, reinterpret_cast<const void*>(&XStoreCanAcquireLicenseForStoreIdAsync),
        "XStoreCanAcquireLicenseForStoreIdAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, sizeof(XStoreCanAcquireLicenseResult));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                if (!data->buffer || data->bufferSize < sizeof(XStoreCanAcquireLicenseResult)) return E_NOT_SUFFICIENT_BUFFER;
                auto* res = static_cast<XStoreCanAcquireLicenseResult*>(data->buffer);
                std::memset(res, 0, sizeof(*res));
                strncpy_s(res->licensableSku, sizeof(res->licensableSku), "0010", _TRUNCATE);
                res->status = XStoreCanLicenseStatus::Licensable;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) return hr;
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XStoreCanAcquireLicenseForStoreIdResult(
    XAsyncBlock* async,
    XStoreCanAcquireLicenseResult* storeCanAcquireLicenseResult) noexcept {
    if (!async || !storeCanAcquireLicenseResult) return E_INVALIDARG;
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XStoreCanAcquireLicenseForStoreIdAsync),
        sizeof(XStoreCanAcquireLicenseResult), storeCanAcquireLicenseResult, nullptr);
}

HRESULT STDAPICALLTYPE XStoreRegisterGameLicenseChanged(
    XStoreContextHandle storeContextHandle,
    XTaskQueueHandle queue,
    void* context,
    XStoreGameLicenseChangedCallback* callback,
    XTaskQueueRegistrationToken* token) noexcept {
    if (!storeContextHandle || !callback || !token) return E_INVALIDARG;
    const uint64_t tok = g_next_token.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_store_mtx);
    g_store_license_regs.push_back(StoreLicenseChangeReg{tok, storeContextHandle, queue, context, callback});
    token->token = tok;
    return S_OK;
}

bool STDAPICALLTYPE XStoreUnregisterGameLicenseChanged(
    XStoreContextHandle storeContextHandle,
    XTaskQueueRegistrationToken token,
    bool /*wait*/) noexcept {
    std::lock_guard<std::mutex> lk(g_store_mtx);
    for (auto it = g_store_license_regs.begin(); it != g_store_license_regs.end(); ++it) {
        if (it->token == token.token && (!storeContextHandle || it->ctx == storeContextHandle)) {
            g_store_license_regs.erase(it);
            return true;
        }
    }
    return false;
}

bool STDAPICALLTYPE XStoreIsAvailabilityPurchasable(
    XStoreAvailability availability) noexcept {
    return availability.availabilityId != nullptr && availability.availabilityId[0] != '\0';
}

// ============================================================================
// Local Persistent Offline XSAPI Exports
// ============================================================================

HRESULT STDAPICALLTYPE XblInitialize(const XblInitArgs* args) noexcept {
    auto& st = XsapiState();
    std::lock_guard<std::mutex> lk(st.mtx);
    st.storageDir = ResolveXsapiDirectory();
    if (args && args->scid && args->scid[0] != '\0') {
        st.scid = args->scid;
    }
    st.achievementsByKey.clear();
    st.presenceByXuid.clear();
    st.initialized = true;
    return wsx::trace_hr(kMod, "XblInitialize", S_OK, "scid=%s", st.scid.c_str());
}

void STDAPICALLTYPE XblCleanup() noexcept {
    auto& st = XsapiState();
    std::lock_guard<std::mutex> lk(st.mtx);
    st.achievementsByKey.clear();
    st.presenceByXuid.clear();
    st.initialized = false;
    wsx::trace_void(kMod, "XblCleanup", "ok");
}

HRESULT STDAPICALLTYPE XblCleanupAsync(XAsyncBlock* async) noexcept {
    if (!async) return E_INVALIDARG;
    const HRESULT hr = XAsyncBegin(
        async, nullptr, reinterpret_cast<const void*>(&XblCleanupAsync), "XblCleanupAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            if (op == XAsyncOp::DoWork) {
                XblCleanup();
                XAsyncComplete(data->async, S_OK, 0);
            }
            return S_OK;
        });
    if (FAILED(hr)) return hr;
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XblContextCreateHandle(
    XUserHandle user,
    XblContextHandle* context) noexcept {
    if (!IsValidUser(user) || !context) return E_INVALIDARG;
    *context = nullptr;
    auto* ctx = new (std::nothrow) XblContext();
    if (!ctx) return E_OUTOFMEMORY;
    XUserDuplicateHandle(user, &ctx->user);
    ctx->xuid = user->xuid;
    {
        auto& st = XsapiState();
        std::lock_guard<std::mutex> lk(st.mtx);
        if (st.storageDir.empty()) st.storageDir = ResolveXsapiDirectory();
        ctx->scid = st.scid;
    }
    *context = ctx;
    return wsx::trace_hr(kMod, "XblContextCreateHandle", S_OK, "xuid=0x%016llX",
                         static_cast<unsigned long long>(ctx->xuid));
}

void STDAPICALLTYPE XblContextCloseHandle(XblContextHandle xboxLiveContextHandle) noexcept {
    if (!xboxLiveContextHandle) return;
    if (xboxLiveContextHandle->refCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (xboxLiveContextHandle->user) XUserCloseHandle(xboxLiveContextHandle->user);
        delete xboxLiveContextHandle;
    }
}

HRESULT STDAPICALLTYPE XblContextDuplicateHandle(
    XblContextHandle xboxLiveContextHandle,
    XblContextHandle* duplicatedHandle) noexcept {
    if (!xboxLiveContextHandle || !duplicatedHandle) return E_INVALIDARG;
    xboxLiveContextHandle->refCount.fetch_add(1, std::memory_order_relaxed);
    *duplicatedHandle = xboxLiveContextHandle;
    return S_OK;
}

HRESULT STDAPICALLTYPE XblContextGetXboxUserId(
    XblContextHandle xboxLiveContextHandle,
    uint64_t* xuid) noexcept {
    if (!xboxLiveContextHandle || !xuid) return E_INVALIDARG;
    *xuid = xboxLiveContextHandle->xuid;
    return S_OK;
}

HRESULT STDAPICALLTYPE XblProfileGetUserProfileAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    XAsyncBlock* async) noexcept {
    if (!xboxLiveContext || !async) return E_INVALIDARG;

    struct ProfCtx {
        uint64_t xuid = 0;
    };
    auto* ctx = new (std::nothrow) ProfCtx{xboxUserId ? xboxUserId : xboxLiveContext->xuid};
    if (!ctx) return E_OUTOFMEMORY;

    const HRESULT hr = XAsyncBegin(
        async, ctx, reinterpret_cast<const void*>(&XblProfileGetUserProfileAsync),
        "XblProfileGetUserProfileAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<ProfCtx*>(data->context);
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, sizeof(XblUserProfile));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                if (!data->buffer || data->bufferSize < sizeof(XblUserProfile)) return E_NOT_SUFFICIENT_BUFFER;
                auto& st = XsapiState();
                std::lock_guard<std::mutex> lk(st.mtx);
                if (st.storageDir.empty()) st.storageDir = ResolveXsapiDirectory();
                FillOfflineProfile(c->xuid, st.storageDir, *static_cast<XblUserProfile*>(data->buffer));
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) {
                delete c;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XblProfileGetUserProfileResult(
    XAsyncBlock* async,
    XblUserProfile* profile) noexcept {
    if (!async || !profile) return E_INVALIDARG;
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XblProfileGetUserProfileAsync),
        sizeof(XblUserProfile), profile, nullptr);
}

HRESULT STDAPICALLTYPE XblProfileGetUserProfilesAsync(
    XblContextHandle xboxLiveContext,
    const uint64_t* xboxUserIds,
    size_t xboxUserIdsCount,
    XAsyncBlock* async) noexcept {
    if (!xboxLiveContext || !async || (xboxUserIdsCount > 0u && !xboxUserIds)) return E_INVALIDARG;

    struct ProfsCtx {
        std::vector<uint64_t> xuids;
    };
    auto* ctx = new (std::nothrow) ProfsCtx();
    if (!ctx) return E_OUTOFMEMORY;
    ctx->xuids.assign(xboxUserIds, xboxUserIds + xboxUserIdsCount);

    const HRESULT hr = XAsyncBegin(
        async, ctx, reinterpret_cast<const void*>(&XblProfileGetUserProfilesAsync),
        "XblProfileGetUserProfilesAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<ProfsCtx*>(data->context);
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, c->xuids.size() * sizeof(XblUserProfile));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                const size_t bytes = c->xuids.size() * sizeof(XblUserProfile);
                if (bytes > 0u) {
                    if (!data->buffer || data->bufferSize < bytes) return E_NOT_SUFFICIENT_BUFFER;
                    auto& st = XsapiState();
                    std::lock_guard<std::mutex> lk(st.mtx);
                    if (st.storageDir.empty()) st.storageDir = ResolveXsapiDirectory();
                    auto* dst = static_cast<XblUserProfile*>(data->buffer);
                    for (size_t i = 0; i < c->xuids.size(); ++i) {
                        FillOfflineProfile(c->xuids[i], st.storageDir, dst[i]);
                    }
                }
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) {
                delete c;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XblProfileGetUserProfilesResultCount(
    XAsyncBlock* async,
    size_t* profileCount) noexcept {
    if (!async || !profileCount) return E_INVALIDARG;
    size_t sz = 0;
    const HRESULT hr = XAsyncGetResultSize(async, &sz);
    if (FAILED(hr)) return hr;
    *profileCount = sz / sizeof(XblUserProfile);
    return S_OK;
}

HRESULT STDAPICALLTYPE XblProfileGetUserProfilesResult(
    XAsyncBlock* async,
    size_t profilesCount,
    XblUserProfile* profiles) noexcept {
    if (!async || (profilesCount > 0u && !profiles)) return E_INVALIDARG;
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XblProfileGetUserProfilesAsync),
        profilesCount * sizeof(XblUserProfile), profiles, nullptr);
}

HRESULT STDAPICALLTYPE XblAchievementsGetAchievementsForTitleIdAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    uint32_t titleId,
    XblAchievementType /*type*/,
    bool unlockedOnly,
    XblAchievementOrderBy /*orderBy*/,
    uint32_t skipItems,
    uint32_t maxItems,
    XAsyncBlock* async) noexcept {
    if (!xboxLiveContext || !async) return E_INVALIDARG;

    struct AchQueryCtx {
        uint64_t xuid = 0;
        uint32_t titleId = 0;
        bool unlockedOnly = false;
        uint32_t skipItems = 0;
        uint32_t maxItems = 0;
    };
    auto* ctx = new (std::nothrow) AchQueryCtx{
        xboxUserId ? xboxUserId : xboxLiveContext->xuid,
        titleId ? titleId : XsapiState().defaultTitleId,
        unlockedOnly, skipItems, maxItems
    };
    if (!ctx) return E_OUTOFMEMORY;

    const HRESULT hr = XAsyncBegin(
        async, ctx, reinterpret_cast<const void*>(&XblAchievementsGetAchievementsForTitleIdAsync),
        "XblAchievementsGetAchievementsForTitleIdAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<AchQueryCtx*>(data->context);
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, sizeof(XblAchievementsResultHandle));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                if (!data->buffer || data->bufferSize < sizeof(XblAchievementsResultHandle)) {
                    return E_NOT_SUFFICIENT_BUFFER;
                }
                auto& st = XsapiState();
                std::vector<XblAchievementOwned> all;
                {
                    std::lock_guard<std::mutex> lk(st.mtx);
                    if (st.storageDir.empty()) st.storageDir = ResolveXsapiDirectory();
                    const uint64_t key = MakeAchMapKey(c->xuid, c->titleId);
                    auto it = st.achievementsByKey.find(key);
                    if (it == st.achievementsByKey.end()) {
                        auto loaded = LoadAchievementsLocked(st.storageDir, c->xuid, c->titleId);
                        it = st.achievementsByKey.emplace(key, std::move(loaded)).first;
                    }
                    all = it->second;
                }

                auto* res = new (std::nothrow) XblAchievementsResult();
                if (!res) return E_OUTOFMEMORY;
                uint32_t seen = 0;
                for (const auto& a : all) {
                    if (c->unlockedOnly && a.progressState != XblAchievementProgressState::Achieved) continue;
                    if (seen++ < c->skipItems) continue;
                    if (c->maxItems > 0u && res->storage.size() >= c->maxItems) {
                        res->hasNext = true;
                        break;
                    }
                    res->storage.push_back(a);
                }
                res->views.reserve(res->storage.size());
                for (const auto& s : res->storage) {
                    XblAchievement v{};
                    v.id = s.id.c_str();
                    v.serviceConfigurationId = s.scid.c_str();
                    v.name = s.name.c_str();
                    v.unlockedDescription = s.unlockedDesc.c_str();
                    v.lockedDescription = s.lockedDesc.c_str();
                    v.titleId = s.titleId;
                    v.gamerscore = s.gamerscore;
                    v.progressState = s.progressState;
                    v.progression.percentComplete = s.percentComplete;
                    v.progression.timeUnlocked = s.timeUnlocked;
                    v.isSecret = s.isSecret;
                    res->views.push_back(v);
                }
                *static_cast<XblAchievementsResultHandle*>(data->buffer) = res;
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) {
                delete c;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XblAchievementsGetAchievementsForTitleIdResult(
    XAsyncBlock* async,
    XblAchievementsResultHandle* result) noexcept {
    if (!async || !result) return E_INVALIDARG;
    *result = nullptr;
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XblAchievementsGetAchievementsForTitleIdAsync),
        sizeof(XblAchievementsResultHandle), result, nullptr);
}

HRESULT STDAPICALLTYPE XblAchievementsResultGetAchievements(
    XblAchievementsResultHandle resultHandle,
    const XblAchievement** achievements,
    size_t* achievementsCount) noexcept {
    if (!resultHandle || !achievements || !achievementsCount) return E_INVALIDARG;
    *achievements = resultHandle->views.empty() ? nullptr : resultHandle->views.data();
    *achievementsCount = resultHandle->views.size();
    return S_OK;
}

HRESULT STDAPICALLTYPE XblAchievementsResultHasNext(
    XblAchievementsResultHandle resultHandle,
    bool* hasNext) noexcept {
    if (!resultHandle || !hasNext) return E_INVALIDARG;
    *hasNext = resultHandle->hasNext;
    return S_OK;
}

void STDAPICALLTYPE XblAchievementsResultCloseHandle(
    XblAchievementsResultHandle resultHandle) noexcept {
    delete resultHandle;
}

HRESULT STDAPICALLTYPE XblAchievementsUpdateAchievementAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const char* achievementId,
    uint32_t percentComplete,
    XAsyncBlock* async) noexcept {
    if (!xboxLiveContext || !achievementId || !achievementId[0] || !async) return E_INVALIDARG;

    struct AchUpdCtx {
        uint64_t xuid = 0;
        std::string scid;
        std::string achId;
        uint32_t percent = 0;
    };
    auto* ctx = new (std::nothrow) AchUpdCtx{
        xboxUserId ? xboxUserId : xboxLiveContext->xuid,
        xboxLiveContext->scid,
        achievementId,
        (std::min<uint32_t>)(percentComplete, 100u)
    };
    if (!ctx) return E_OUTOFMEMORY;

    const HRESULT hr = XAsyncBegin(
        async, ctx, reinterpret_cast<const void*>(&XblAchievementsUpdateAchievementAsync),
        "XblAchievementsUpdateAchievementAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<AchUpdCtx*>(data->context);
            if (op == XAsyncOp::DoWork) {
                auto& st = XsapiState();
                std::lock_guard<std::mutex> lk(st.mtx);
                if (st.storageDir.empty()) st.storageDir = ResolveXsapiDirectory();
                const uint32_t tid = st.defaultTitleId;
                const uint64_t key = MakeAchMapKey(c->xuid, tid);
                auto it = st.achievementsByKey.find(key);
                if (it == st.achievementsByKey.end()) {
                    auto loaded = LoadAchievementsLocked(st.storageDir, c->xuid, tid);
                    it = st.achievementsByKey.emplace(key, std::move(loaded)).first;
                }
                auto& list = it->second;
                XblAchievementOwned* entry = nullptr;
                for (auto& existing : list) {
                    if (existing.id == c->achId) {
                        entry = &existing;
                        break;
                    }
                }
                if (!entry) {
                    XblAchievementOwned created{};
                    created.id = c->achId;
                    created.scid = c->scid;
                    created.name = std::string("Achievement ") + c->achId;
                    created.unlockedDesc = "Unlocked locally in MizuBox";
                    created.lockedDesc = "In progress";
                    created.titleId = tid;
                    created.gamerscore = 25u;
                    list.push_back(std::move(created));
                    entry = &list.back();
                }
                if (c->percent > entry->percentComplete) {
                    entry->percentComplete = c->percent;
                }
                if (entry->percentComplete >= 100u) {
                    entry->progressState = XblAchievementProgressState::Achieved;
                    if (entry->timeUnlocked == 0) {
                        entry->timeUnlocked = std::time(nullptr);
                    }
                } else if (entry->percentComplete > 0u) {
                    entry->progressState = XblAchievementProgressState::InProgress;
                }
                SaveAchievementsLocked(st.storageDir, c->xuid, tid, list);
                XAsyncComplete(data->async, S_OK, 0);
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) {
                delete c;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return wsx::trace_hr(kMod, "XblAchievementsUpdateAchievementAsync", XAsyncSchedule(async, 0),
                         "id=%s pct=%u", achievementId, percentComplete);
}

HRESULT STDAPICALLTYPE XblPresenceSetPresenceAsync(
    XblContextHandle xboxLiveContext,
    bool isUserActiveInTitle,
    const XblPresenceRichPresenceIds* richPresenceIds,
    XAsyncBlock* async) noexcept {
    if (!xboxLiveContext || !async) return E_INVALIDARG;

    struct PresSetCtx {
        uint64_t xuid = 0;
        bool active = false;
        std::string rpId;
    };
    auto* ctx = new (std::nothrow) PresSetCtx{
        xboxLiveContext->xuid,
        isUserActiveInTitle,
        (richPresenceIds && richPresenceIds->presenceId) ? richPresenceIds->presenceId : ""
    };
    if (!ctx) return E_OUTOFMEMORY;

    const HRESULT hr = XAsyncBegin(
        async, ctx, reinterpret_cast<const void*>(&XblPresenceSetPresenceAsync),
        "XblPresenceSetPresenceAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<PresSetCtx*>(data->context);
            if (op == XAsyncOp::DoWork) {
                auto& st = XsapiState();
                std::lock_guard<std::mutex> lk(st.mtx);
                if (st.storageDir.empty()) st.storageDir = ResolveXsapiDirectory();
                XblPresenceRecord rec{};
                rec.xuid = c->xuid;
                rec.userState = c->active ? XblPresenceUserState::Online : XblPresenceUserState::Away;
                rec.titleId = st.defaultTitleId;
                strncpy_s(rec.richPresenceId, sizeof(rec.richPresenceId), c->rpId.c_str(), _TRUNCATE);
                rec.lastUpdated = std::time(nullptr);
                st.presenceByXuid[c->xuid] = rec;
                SavePresenceLocked(st.storageDir, rec);
                XAsyncComplete(data->async, S_OK, 0);
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) {
                delete c;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XblPresenceGetPresenceAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xuid,
    XAsyncBlock* async) noexcept {
    if (!xboxLiveContext || !async) return E_INVALIDARG;

    struct PresGetCtx {
        uint64_t xuid = 0;
    };
    auto* ctx = new (std::nothrow) PresGetCtx{xuid ? xuid : xboxLiveContext->xuid};
    if (!ctx) return E_OUTOFMEMORY;

    const HRESULT hr = XAsyncBegin(
        async, ctx, reinterpret_cast<const void*>(&XblPresenceGetPresenceAsync),
        "XblPresenceGetPresenceAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<PresGetCtx*>(data->context);
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, sizeof(XblPresenceRecord));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                if (!data->buffer || data->bufferSize < sizeof(XblPresenceRecord)) return E_NOT_SUFFICIENT_BUFFER;
                auto& st = XsapiState();
                std::lock_guard<std::mutex> lk(st.mtx);
                if (st.storageDir.empty()) st.storageDir = ResolveXsapiDirectory();
                XblPresenceRecord rec{};
                auto it = st.presenceByXuid.find(c->xuid);
                if (it != st.presenceByXuid.end()) {
                    rec = it->second;
                } else if (LoadPresenceLocked(st.storageDir, c->xuid, rec)) {
                    st.presenceByXuid[c->xuid] = rec;
                } else {
                    rec.xuid = c->xuid;
                    rec.userState = XblPresenceUserState::Online;
                    rec.titleId = st.defaultTitleId;
                    rec.lastUpdated = std::time(nullptr);
                }
                *static_cast<XblPresenceRecord*>(data->buffer) = rec;
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) {
                delete c;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XblPresenceGetPresenceResult(
    XAsyncBlock* async,
    XblPresenceRecord* presence) noexcept {
    if (!async || !presence) return E_INVALIDARG;
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XblPresenceGetPresenceAsync),
        sizeof(XblPresenceRecord), presence, nullptr);
}

HRESULT STDAPICALLTYPE XblPrivacyCheckPermissionAsync(
    XblContextHandle xboxLiveContext,
    XblPermission permissionToCheck,
    uint64_t targetXuid,
    XAsyncBlock* async) noexcept {
    if (!xboxLiveContext || !async) return E_INVALIDARG;

    struct PrivCtx {
        XblPermission perm = XblPermission::Unknown;
        uint64_t target = 0;
    };
    auto* ctx = new (std::nothrow) PrivCtx{permissionToCheck, targetXuid};
    if (!ctx) return E_OUTOFMEMORY;

    const HRESULT hr = XAsyncBegin(
        async, ctx, reinterpret_cast<const void*>(&XblPrivacyCheckPermissionAsync),
        "XblPrivacyCheckPermissionAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<PrivCtx*>(data->context);
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, sizeof(XblPermissionCheckResult));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                if (!data->buffer || data->bufferSize < sizeof(XblPermissionCheckResult)) return E_NOT_SUFFICIENT_BUFFER;
                auto* res = static_cast<XblPermissionCheckResult*>(data->buffer);
                res->isAllowed = true;
                res->targetXuid = c->target;
                res->permissionRequested = c->perm;
                res->reasonsCount = 0u;
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) {
                delete c;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XblPrivacyCheckPermissionResult(
    XAsyncBlock* async,
    XblPermissionCheckResult* result) noexcept {
    if (!async || !result) return E_INVALIDARG;
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XblPrivacyCheckPermissionAsync),
        sizeof(XblPermissionCheckResult), result, nullptr);
}

HRESULT STDAPICALLTYPE XblStringVerifyStringAsync(
    XblContextHandle xboxLiveContext,
    const char* stringToVerify,
    XAsyncBlock* async) noexcept {
    if (!xboxLiveContext || !stringToVerify || !async) return E_INVALIDARG;

    struct StrCtx {
        bool tooLong = false;
    };
    auto* ctx = new (std::nothrow) StrCtx{std::strlen(stringToVerify) > 4096u};
    if (!ctx) return E_OUTOFMEMORY;

    const HRESULT hr = XAsyncBegin(
        async, ctx, reinterpret_cast<const void*>(&XblStringVerifyStringAsync),
        "XblStringVerifyStringAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<StrCtx*>(data->context);
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, sizeof(XblVerifyStringResult));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                if (!data->buffer || data->bufferSize < sizeof(XblVerifyStringResult)) return E_NOT_SUFFICIENT_BUFFER;
                auto* out = static_cast<XblVerifyStringResult*>(data->buffer);
                out->resultCode = c->tooLong ? XblVerifyStringResultCode::TooLong : XblVerifyStringResultCode::Success;
                out->firstOffendingSubstring = nullptr;
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) {
                delete c;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XblStringVerifyStringResultSize(
    XAsyncBlock* async,
    size_t* resultSize) noexcept {
    return XAsyncGetResultSize(async, resultSize);
}

HRESULT STDAPICALLTYPE XblStringVerifyStringResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblVerifyStringResult** ptrToBuffer,
    size_t* bufferUsed) noexcept {
    if (!async || !buffer || !ptrToBuffer) return E_INVALIDARG;
    *ptrToBuffer = nullptr;
    const HRESULT hr = XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XblStringVerifyStringAsync),
        bufferSize, buffer, bufferUsed);
    if (SUCCEEDED(hr)) {
        *ptrToBuffer = static_cast<XblVerifyStringResult*>(buffer);
    }
    return hr;
}

} // extern "C"

// ============================================================================
// Wave A Extensions: XUser Events, XStore Consumables/Products, Local XSAPI
// Stats / Leaderboards / Social / Offline Multiplayer Manager
// ============================================================================

struct XStoreProductQuery {
    std::vector<std::string> ids;
    std::vector<std::string> titles;
    std::vector<XStoreProduct> products;
};

struct XblSocialRelationshipResult {
    std::vector<XblSocialRelationship> items;
};

namespace {

struct LocalStatValue {
    std::string name;
    XblTitleManagedStatType type = XblTitleManagedStatType::Number;
    double numberValue = 0.0;
    std::string stringValue;
};

struct LocalConsumableStore {
    std::mutex mtx;
    std::unordered_map<std::string, uint32_t> balances; // key = "<xuid>:<productId>"
    bool mpInitialized = false;
    std::string mpLobbyTemplate;
    std::vector<XblMultiplayerEvent> mpEvents;
};

LocalConsumableStore& WaveAStore() noexcept {
    static LocalConsumableStore s;
    return s;
}

fs::path ConsumablesJsonPath(const fs::path& dir, uint64_t xuid) {
    wchar_t name[96]{};
    std::swprintf(name, std::size(name), L"consumables_%016llX.json",
                  static_cast<unsigned long long>(xuid));
    return dir / name;
}

fs::path StatsJsonPath(const fs::path& dir, uint64_t xuid) {
    wchar_t name[96]{};
    std::swprintf(name, std::size(name), L"stats_%016llX.json",
                  static_cast<unsigned long long>(xuid));
    return dir / name;
}

std::unordered_map<std::string, uint32_t> LoadConsumablesFile(const fs::path& dir, uint64_t xuid) {
    std::unordered_map<std::string, uint32_t> m;
    std::ifstream in(ConsumablesJsonPath(dir, xuid), std::ios::binary);
    if (!in) return m;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    while (pos < content.size()) {
        const size_t open = content.find('{', pos);
        if (open == std::string::npos) break;
        const size_t close = content.find('}', open);
        if (close == std::string::npos) break;
        const std::string obj = content.substr(open, close - open + 1u);
        const std::string id = ExtractJsonStringField(obj, "id");
        if (!id.empty()) {
            m[id] = static_cast<uint32_t>(ExtractJsonUintField(obj, "qty", 0));
        }
        pos = close + 1u;
    }
    return m;
}

void SaveConsumablesFile(const fs::path& dir, uint64_t xuid, const std::unordered_map<std::string, uint32_t>& m) {
    std::string json = "[\n";
    size_t i = 0;
    for (const auto& [id, qty] : m) {
        json += "  {\"id\":\"" + JsonEscape(id) + "\",\"qty\":" + std::to_string(qty) + "}";
        if (++i < m.size()) json += ",";
        json += "\n";
    }
    json += "]\n";
    AtomicWriteBinaryFile(ConsumablesJsonPath(dir, xuid),
                          reinterpret_cast<const uint8_t*>(json.data()), json.size());
}

std::vector<LocalStatValue> LoadStatsFile(const fs::path& dir, uint64_t xuid) {
    std::vector<LocalStatValue> list;
    std::ifstream in(StatsJsonPath(dir, xuid), std::ios::binary);
    if (!in) return list;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    while (pos < content.size()) {
        const size_t open = content.find('{', pos);
        if (open == std::string::npos) break;
        const size_t close = content.find('}', open);
        if (close == std::string::npos) break;
        const std::string obj = content.substr(open, close - open + 1u);
        const std::string name = ExtractJsonStringField(obj, "name");
        if (!name.empty()) {
            LocalStatValue v{};
            v.name = name;
            v.type = static_cast<XblTitleManagedStatType>(ExtractJsonUintField(obj, "type", 0));
            const std::string numStr = ExtractJsonStringField(obj, "num");
            v.numberValue = numStr.empty() ? 0.0 : std::strtod(numStr.c_str(), nullptr);
            v.stringValue = ExtractJsonStringField(obj, "str");
            list.push_back(std::move(v));
        }
        pos = close + 1u;
    }
    return list;
}

void SaveStatsFile(const fs::path& dir, uint64_t xuid, const std::vector<LocalStatValue>& list) {
    std::string json = "[\n";
    for (size_t i = 0; i < list.size(); ++i) {
        char numBuf[64]{};
        std::snprintf(numBuf, sizeof(numBuf), "%.6g", list[i].numberValue);
        json += "  {\"name\":\"" + JsonEscape(list[i].name) +
                "\",\"type\":" + std::to_string(static_cast<unsigned>(list[i].type)) +
                ",\"num\":\"" + numBuf +
                "\",\"str\":\"" + JsonEscape(list[i].stringValue) + "\"}";
        if (i + 1u < list.size()) json += ",";
        json += "\n";
    }
    json += "]\n";
    AtomicWriteBinaryFile(StatsJsonPath(dir, xuid),
                          reinterpret_cast<const uint8_t*>(json.data()), json.size());
}

} // namespace

extern "C" {

// ----------------------------------------------------------------------------
// XUser Wave A Extensions
// ----------------------------------------------------------------------------

HRESULT STDAPICALLTYPE XUserAddByIdWithUiAsync(
    uint64_t userId,
    XAsyncBlock* async) noexcept {
    if (!async) return E_INVALIDARG;
    auto* uidPtr = new (std::nothrow) uint64_t(userId);
    if (!uidPtr) return E_OUTOFMEMORY;
    const HRESULT hr = XAsyncBegin(
        async, uidPtr, reinterpret_cast<const void*>(&XUserAddByIdWithUiAsync),
        "XUserAddByIdWithUiAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* uid = static_cast<uint64_t*>(data->context);
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, sizeof(XUserHandle));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                if (!data->buffer || data->bufferSize < sizeof(XUserHandle)) return E_NOT_SUFFICIENT_BUFFER;
                XUserHandle u = GetOrCreateDefaultUser();
                if (!u) return E_OUTOFMEMORY;
                if (*uid != 0ULL && *uid != u->xuid) {
                    XUserCloseHandle(u);
                    auto* custom = new (std::nothrow) XUser();
                    if (!custom) return E_OUTOFMEMORY;
                    custom->xuid = *uid;
                    custom->localId.value = (*uid & 0xFFFFULL) + 2ULL;
                    char gt[32]{};
                    std::snprintf(gt, sizeof(gt), "Player_%04llX", static_cast<unsigned long long>(*uid & 0xFFFFULL));
                    custom->classicGamertag = gt;
                    custom->modernGamertag = gt;
                    custom->suffix = "";
                    u = custom;
                }
                *static_cast<XUserHandle*>(data->buffer) = u;
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) {
                delete uid;
            }
            return S_OK;
        });
    if (FAILED(hr)) {
        delete uidPtr;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XUserAddByIdWithUiResult(
    XAsyncBlock* async,
    XUserHandle* newUser) noexcept {
    if (!async || !newUser) return E_INVALIDARG;
    *newUser = nullptr;
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XUserAddByIdWithUiAsync),
        sizeof(XUserHandle), newUser, nullptr);
}

HRESULT STDAPICALLTYPE WsxDispatchUserChangeEvent(
    XUserLocalId userLocalId,
    XUserChangeEvent event) noexcept {
    std::vector<UserChangeReg> regs;
    {
        std::lock_guard<std::mutex> lock(g_user_mtx);
        regs = g_user_change_regs;
    }
    for (const auto& r : regs) {
        if (!r.callback) continue;
        struct DispatchArg {
            XUserChangeEventCallback* cb;
            void* ctx;
            XUserLocalId id;
            XUserChangeEvent ev;
        };
        if (r.queue) {
            auto* arg = new (std::nothrow) DispatchArg{r.callback, r.context, userLocalId, event};
            if (arg) {
                XTaskQueueSubmitCallback(
                    r.queue, XTaskQueuePort::Completion, arg,
                    [](void* p, bool canceled) noexcept {
                        auto* a = static_cast<DispatchArg*>(p);
                        if (!canceled && a->cb) a->cb(a->ctx, a->id, a->ev);
                        delete a;
                    });
            }
        } else {
            r.callback(r.context, userLocalId, event);
        }
    }
    return S_OK;
}

// ----------------------------------------------------------------------------
// XStore Wave A Extensions (Consumables + Purchase UI + Associated Products)
// ----------------------------------------------------------------------------

HRESULT STDAPICALLTYPE XStoreQueryConsumableBalanceRemainingAsync(
    XStoreContextHandle storeContextHandle,
    const char* storeProductId,
    XAsyncBlock* async) noexcept {
    if (!storeContextHandle || !storeProductId || !storeProductId[0] || !async) return E_INVALIDARG;
    struct ConsQueryCtx {
        uint64_t xuid;
        std::string productId;
    };
    auto* ctx = new (std::nothrow) ConsQueryCtx{storeContextHandle->xuid, storeProductId};
    if (!ctx) return E_OUTOFMEMORY;
    const HRESULT hr = XAsyncBegin(
        async, ctx, reinterpret_cast<const void*>(&XStoreQueryConsumableBalanceRemainingAsync),
        "XStoreQueryConsumableBalanceRemainingAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<ConsQueryCtx*>(data->context);
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, sizeof(XStoreConsumableResult));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                if (!data->buffer || data->bufferSize < sizeof(XStoreConsumableResult)) return E_NOT_SUFFICIENT_BUFFER;
                auto& ws = WaveAStore();
                std::lock_guard<std::mutex> lk(ws.mtx);
                const fs::path dir = ResolveXsapiDirectory();
                auto map = LoadConsumablesFile(dir, c->xuid);
                uint32_t qty = 0;
                auto it = map.find(c->productId);
                if (it != map.end()) {
                    qty = it->second;
                } else {
                    qty = static_cast<uint32_t>(wsx::get_config_int("WSX_CONSUMABLE_DEFAULT", "Store", "default_consumable_qty", 100));
                }
                static_cast<XStoreConsumableResult*>(data->buffer)->quantity = qty;
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) delete c;
            return S_OK;
        });
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XStoreQueryConsumableBalanceRemainingResult(
    XAsyncBlock* async,
    XStoreConsumableResult* consumableResult) noexcept {
    if (!async || !consumableResult) return E_INVALIDARG;
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XStoreQueryConsumableBalanceRemainingAsync),
        sizeof(XStoreConsumableResult), consumableResult, nullptr);
}

HRESULT STDAPICALLTYPE XStoreReportConsumableFulfillmentAsync(
    XStoreContextHandle storeContextHandle,
    const char* productStoreId,
    uint32_t quantity,
    GUID /*trackingId*/,
    XAsyncBlock* async) noexcept {
    if (!storeContextHandle || !productStoreId || !productStoreId[0] || !async) return E_INVALIDARG;
    struct FulfillCtx {
        uint64_t xuid;
        std::string productId;
        uint32_t qty;
    };
    auto* ctx = new (std::nothrow) FulfillCtx{storeContextHandle->xuid, productStoreId, quantity};
    if (!ctx) return E_OUTOFMEMORY;
    const HRESULT hr = XAsyncBegin(
        async, ctx, reinterpret_cast<const void*>(&XStoreReportConsumableFulfillmentAsync),
        "XStoreReportConsumableFulfillmentAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<FulfillCtx*>(data->context);
            if (op == XAsyncOp::DoWork) {
                auto& ws = WaveAStore();
                std::lock_guard<std::mutex> lk(ws.mtx);
                const fs::path dir = ResolveXsapiDirectory();
                auto map = LoadConsumablesFile(dir, c->xuid);
                uint32_t cur = 0;
                auto it = map.find(c->productId);
                if (it != map.end()) {
                    cur = it->second;
                } else {
                    cur = static_cast<uint32_t>(wsx::get_config_int("WSX_CONSUMABLE_DEFAULT", "Store", "default_consumable_qty", 100));
                }
                if (c->qty > cur) {
                    XAsyncComplete(data->async, E_FAIL, 0);
                    return S_OK;
                }
                map[c->productId] = cur - c->qty;
                SaveConsumablesFile(dir, c->xuid, map);
                XAsyncComplete(data->async, S_OK, 0);
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) delete c;
            return S_OK;
        });
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XStoreReportConsumableFulfillmentResult(XAsyncBlock* async) noexcept {
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XStoreReportConsumableFulfillmentAsync),
        0, nullptr, nullptr);
}

HRESULT STDAPICALLTYPE XStoreShowPurchaseUIAsync(
    XStoreContextHandle storeContextHandle,
    const char* storeId,
    const char* /*name*/,
    const char* /*extendedJsonData*/,
    XAsyncBlock* async) noexcept {
    if (!storeContextHandle || !storeId || !storeId[0] || !async) return E_INVALIDARG;
    struct PurchaseCtx {
        uint64_t xuid;
        std::string storeId;
    };
    auto* ctx = new (std::nothrow) PurchaseCtx{storeContextHandle->xuid, storeId};
    if (!ctx) return E_OUTOFMEMORY;
    const HRESULT hr = XAsyncBegin(
        async, ctx, reinterpret_cast<const void*>(&XStoreShowPurchaseUIAsync),
        "XStoreShowPurchaseUIAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<PurchaseCtx*>(data->context);
            if (op == XAsyncOp::DoWork) {
                auto& ws = WaveAStore();
                std::lock_guard<std::mutex> lk(ws.mtx);
                const fs::path dir = ResolveXsapiDirectory();
                auto map = LoadConsumablesFile(dir, c->xuid);
                map[c->storeId] = map[c->storeId] + 100u;
                SaveConsumablesFile(dir, c->xuid, map);
                XAsyncComplete(data->async, S_OK, 0);
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) delete c;
            return S_OK;
        });
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XStoreShowPurchaseUIResult(XAsyncBlock* async) noexcept {
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XStoreShowPurchaseUIAsync),
        0, nullptr, nullptr);
}

HRESULT STDAPICALLTYPE XStoreQueryAssociatedProductsAsync(
    XStoreContextHandle storeContextHandle,
    XStoreProductKind productKinds,
    uint32_t maxItemsToRetrievePerPage,
    XAsyncBlock* async) noexcept {
    if (!storeContextHandle || !async) return E_INVALIDARG;
    struct AssocCtx {
        XStoreProductKind kinds;
        uint32_t maxItems;
    };
    auto* ctx = new (std::nothrow) AssocCtx{productKinds, maxItemsToRetrievePerPage};
    if (!ctx) return E_OUTOFMEMORY;
    const HRESULT hr = XAsyncBegin(
        async, ctx, reinterpret_cast<const void*>(&XStoreQueryAssociatedProductsAsync),
        "XStoreQueryAssociatedProductsAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<AssocCtx*>(data->context);
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, sizeof(XStoreProductQueryHandle));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                if (!data->buffer || data->bufferSize < sizeof(XStoreProductQueryHandle)) return E_NOT_SUFFICIENT_BUFFER;
                auto* q = new (std::nothrow) XStoreProductQuery();
                if (!q) return E_OUTOFMEMORY;
                const auto addons = GetConfiguredStoreAddons();
                for (const auto& id : addons) {
                    if (c->maxItems > 0u && q->ids.size() >= c->maxItems) break;
                    q->ids.push_back(id);
                    q->titles.push_back(std::string("Add-On ") + id);
                }
                q->products.reserve(q->ids.size());
                for (size_t i = 0; i < q->ids.size(); ++i) {
                    XStoreProduct p{};
                    p.storeId = q->ids[i].c_str();
                    p.title = q->titles[i].c_str();
                    p.description = "Offline local add-on";
                    p.language = "en-US";
                    p.inAppOfferToken = q->ids[i].c_str();
                    p.linkUri = "";
                    p.productKind = XStoreProductKind::Durable;
                    strncpy_s(p.price.currencyCode, sizeof(p.price.currencyCode), "USD", _TRUNCATE);
                    p.price.formattedBasePrice = "$0.00";
                    p.price.formattedPrice = "$0.00";
                    p.price.formattedRecurrencePrice = "";
                    p.hasDigitalDownload = true;
                    p.isInUserCollection = true;
                    q->products.push_back(p);
                }
                *static_cast<XStoreProductQueryHandle*>(data->buffer) = q;
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) delete c;
            return S_OK;
        });
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XStoreQueryAssociatedProductsResult(
    XAsyncBlock* async,
    XStoreProductQueryHandle* productQueryHandle) noexcept {
    if (!async || !productQueryHandle) return E_INVALIDARG;
    *productQueryHandle = nullptr;
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XStoreQueryAssociatedProductsAsync),
        sizeof(XStoreProductQueryHandle), productQueryHandle, nullptr);
}

HRESULT STDAPICALLTYPE XStoreEnumerateProductsQuery(
    XStoreProductQueryHandle productQueryHandle,
    void* context,
    XStoreProductQueryCallback* callback) noexcept {
    if (!productQueryHandle || !callback) return E_INVALIDARG;
    for (const auto& p : productQueryHandle->products) {
        if (!callback(&p, context)) break;
    }
    return S_OK;
}

void STDAPICALLTYPE XStoreCloseProductsQueryHandle(
    XStoreProductQueryHandle productQueryHandle) noexcept {
    delete productQueryHandle;
}

// ----------------------------------------------------------------------------
// XSAPI Wave A Extensions: Local JSON Stats, Leaderboards, Social & Lobby
// ----------------------------------------------------------------------------

HRESULT STDAPICALLTYPE XblTitleManagedStatsWriteAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const XblTitleManagedStatistic* statistics,
    size_t statisticsCount,
    XAsyncBlock* async) noexcept {
    if (!xboxLiveContext || !async || (statisticsCount > 0u && !statistics)) return E_INVALIDARG;
    struct StatWriteCtx {
        uint64_t xuid;
        std::vector<LocalStatValue> entries;
    };
    auto* ctx = new (std::nothrow) StatWriteCtx();
    if (!ctx) return E_OUTOFMEMORY;
    ctx->xuid = xboxUserId ? xboxUserId : xboxLiveContext->xuid;
    for (size_t i = 0; i < statisticsCount; ++i) {
        if (!statistics[i].statisticName) continue;
        LocalStatValue v{};
        v.name = statistics[i].statisticName;
        v.type = statistics[i].statisticType;
        v.numberValue = statistics[i].numberValue;
        v.stringValue = statistics[i].stringValue ? statistics[i].stringValue : "";
        ctx->entries.push_back(std::move(v));
    }

    const HRESULT hr = XAsyncBegin(
        async, ctx, reinterpret_cast<const void*>(&XblTitleManagedStatsWriteAsync),
        "XblTitleManagedStatsWriteAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<StatWriteCtx*>(data->context);
            if (op == XAsyncOp::DoWork) {
                auto& st = XsapiState();
                std::lock_guard<std::mutex> lk(st.mtx);
                if (st.storageDir.empty()) st.storageDir = ResolveXsapiDirectory();
                auto list = LoadStatsFile(st.storageDir, c->xuid);
                for (const auto& upd : c->entries) {
                    bool replaced = false;
                    for (auto& existing : list) {
                        if (existing.name == upd.name) {
                            existing = upd;
                            replaced = true;
                            break;
                        }
                    }
                    if (!replaced) list.push_back(upd);
                }
                SaveStatsFile(st.storageDir, c->xuid, list);
                XAsyncComplete(data->async, S_OK, 0);
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) delete c;
            return S_OK;
        });
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XblTitleManagedStatsUpdateStatsAsync(
    XblContextHandle xboxLiveContext,
    const XblTitleManagedStatistic* statistics,
    size_t statisticsCount,
    XAsyncBlock* async) noexcept {
    if (!xboxLiveContext) return E_INVALIDARG;
    return XblTitleManagedStatsWriteAsync(xboxLiveContext, xboxLiveContext->xuid, statistics, statisticsCount, async);
}

HRESULT STDAPICALLTYPE XblTitleManagedStatsDeleteStatsAsync(
    XblContextHandle xboxLiveContext,
    const char** statisticNames,
    size_t statisticNamesCount,
    XAsyncBlock* async) noexcept {
    if (!xboxLiveContext || !async || (statisticNamesCount > 0u && !statisticNames)) return E_INVALIDARG;
    struct StatDelCtx {
        uint64_t xuid;
        std::vector<std::string> names;
    };
    auto* ctx = new (std::nothrow) StatDelCtx();
    if (!ctx) return E_OUTOFMEMORY;
    ctx->xuid = xboxLiveContext->xuid;
    for (size_t i = 0; i < statisticNamesCount; ++i) {
        if (statisticNames[i]) ctx->names.emplace_back(statisticNames[i]);
    }
    const HRESULT hr = XAsyncBegin(
        async, ctx, reinterpret_cast<const void*>(&XblTitleManagedStatsDeleteStatsAsync),
        "XblTitleManagedStatsDeleteStatsAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<StatDelCtx*>(data->context);
            if (op == XAsyncOp::DoWork) {
                auto& st = XsapiState();
                std::lock_guard<std::mutex> lk(st.mtx);
                if (st.storageDir.empty()) st.storageDir = ResolveXsapiDirectory();
                auto list = LoadStatsFile(st.storageDir, c->xuid);
                list.erase(std::remove_if(list.begin(), list.end(), [&](const LocalStatValue& v) {
                    return std::find(c->names.begin(), c->names.end(), v.name) != c->names.end();
                }), list.end());
                SaveStatsFile(st.storageDir, c->xuid, list);
                XAsyncComplete(data->async, S_OK, 0);
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) delete c;
            return S_OK;
        });
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XblUserStatisticsGetSingleUserStatisticAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const char* serviceConfigurationId,
    const char* statisticName,
    XAsyncBlock* async) noexcept {
    if (!xboxLiveContext || !statisticName || !statisticName[0] || !async) return E_INVALIDARG;
    struct GetStatCtx {
        uint64_t xuid;
        std::string scid;
        std::string statName;
        std::string statType;
        std::string statVal;
        bool found = false;
    };
    auto* ctx = new (std::nothrow) GetStatCtx();
    if (!ctx) return E_OUTOFMEMORY;
    ctx->xuid = xboxUserId ? xboxUserId : xboxLiveContext->xuid;
    ctx->scid = (serviceConfigurationId && serviceConfigurationId[0]) ? serviceConfigurationId : xboxLiveContext->scid;
    ctx->statName = statisticName;

    const HRESULT hr = XAsyncBegin(
        async, ctx, reinterpret_cast<const void*>(&XblUserStatisticsGetSingleUserStatisticAsync),
        "XblUserStatisticsGetSingleUserStatisticAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<GetStatCtx*>(data->context);
            if (op == XAsyncOp::DoWork) {
                auto& st = XsapiState();
                std::lock_guard<std::mutex> lk(st.mtx);
                if (st.storageDir.empty()) st.storageDir = ResolveXsapiDirectory();
                const auto list = LoadStatsFile(st.storageDir, c->xuid);
                for (const auto& item : list) {
                    if (item.name == c->statName) {
                        c->found = true;
                        if (item.type == XblTitleManagedStatType::Number) {
                            c->statType = "Double";
                            char nb[64]{};
                            std::snprintf(nb, sizeof(nb), "%.6g", item.numberValue);
                            c->statVal = nb;
                        } else {
                            c->statType = "String";
                            c->statVal = item.stringValue;
                        }
                        break;
                    }
                }
                const size_t req = sizeof(XblUserStatisticsResult) +
                                   sizeof(XblServiceConfigurationStatistic) +
                                   (c->found ? (sizeof(XblStatistic) + c->statName.size() + 1u +
                                                c->statType.size() + 1u + c->statVal.size() + 1u)
                                             : 0u);
                XAsyncComplete(data->async, S_OK, req);
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                auto* out = static_cast<XblUserStatisticsResult*>(data->buffer);
                auto* sc = reinterpret_cast<XblServiceConfigurationStatistic*>(out + 1);
                std::memset(out, 0, sizeof(*out));
                std::memset(sc, 0, sizeof(*sc));
                out->xboxUserId = c->xuid;
                out->serviceConfigStatistics = sc;
                out->serviceConfigStatisticsCount = 1u;
                strncpy_s(sc->serviceConfigurationId, sizeof(sc->serviceConfigurationId), c->scid.c_str(), _TRUNCATE);
                if (c->found) {
                    auto* stPtr = reinterpret_cast<XblStatistic*>(sc + 1);
                    char* strDst = reinterpret_cast<char*>(stPtr + 1);
                    std::memcpy(strDst, c->statName.c_str(), c->statName.size() + 1u);
                    stPtr->statisticName = strDst;
                    strDst += c->statName.size() + 1u;
                    std::memcpy(strDst, c->statType.c_str(), c->statType.size() + 1u);
                    stPtr->statisticType = strDst;
                    strDst += c->statType.size() + 1u;
                    std::memcpy(strDst, c->statVal.c_str(), c->statVal.size() + 1u);
                    stPtr->value = strDst;
                    sc->statistics = stPtr;
                    sc->statisticsCount = 1u;
                }
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) delete c;
            return S_OK;
        });
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XblUserStatisticsGetSingleUserStatisticResultSize(
    XAsyncBlock* async,
    size_t* resultSizeInBytes) noexcept {
    return XAsyncGetResultSize(async, resultSizeInBytes);
}

HRESULT STDAPICALLTYPE XblUserStatisticsGetSingleUserStatisticResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblUserStatisticsResult** ptrToBuffer,
    size_t* bufferUsed) noexcept {
    if (!async || !buffer || !ptrToBuffer) return E_INVALIDARG;
    *ptrToBuffer = nullptr;
    const HRESULT hr = XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XblUserStatisticsGetSingleUserStatisticAsync),
        bufferSize, buffer, bufferUsed);
    if (SUCCEEDED(hr)) {
        *ptrToBuffer = static_cast<XblUserStatisticsResult*>(buffer);
    }
    return hr;
}

HRESULT STDAPICALLTYPE XblLeaderboardGetLeaderboardAsync(
    XblContextHandle xboxLiveContext,
    XblLeaderboardQuery leaderboardQuery,
    XAsyncBlock* async) noexcept {
    if (!xboxLiveContext || !async) return E_INVALIDARG;
    struct LbRowOwned {
        uint64_t xuid = 0;
        std::string gt;
        std::string mgt;
        double score = 0.0;
        std::string valStr;
    };
    struct LbCtx {
        std::string statName;
        uint32_t maxItems = 100u;
        std::vector<LbRowOwned> rows;
    };
    auto* ctx = new (std::nothrow) LbCtx();
    if (!ctx) return E_OUTOFMEMORY;
    ctx->statName = leaderboardQuery.statName ? leaderboardQuery.statName
                  : (leaderboardQuery.leaderboardName ? leaderboardQuery.leaderboardName : "");
    ctx->maxItems = leaderboardQuery.maxItems ? leaderboardQuery.maxItems : 100u;

    const HRESULT hr = XAsyncBegin(
        async, ctx, reinterpret_cast<const void*>(&XblLeaderboardGetLeaderboardAsync),
        "XblLeaderboardGetLeaderboardAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* c = static_cast<LbCtx*>(data->context);
            if (op == XAsyncOp::DoWork) {
                auto& st = XsapiState();
                std::lock_guard<std::mutex> lk(st.mtx);
                if (st.storageDir.empty()) st.storageDir = ResolveXsapiDirectory();
                std::error_code ec;
                for (const auto& entry : fs::directory_iterator(st.storageDir, ec)) {
                    if (ec || !entry.is_regular_file(ec)) continue;
                    const std::string fn = entry.path().filename().string();
                    if (fn.rfind("stats_", 0) != 0 || entry.path().extension() != L".json") continue;
                    const std::string hexStr = fn.substr(6, fn.size() - 11);
                    const uint64_t uid = std::strtoull(hexStr.c_str(), nullptr, 16);
                    const auto stats = LoadStatsFile(st.storageDir, uid);
                    for (const auto& sv : stats) {
                        if (c->statName.empty() || sv.name == c->statName) {
                            XblUserProfile prof{};
                            FillOfflineProfile(uid, st.storageDir, prof);
                            LbRowOwned r{};
                            r.xuid = uid;
                            r.gt = prof.gamertag;
                            r.mgt = prof.modernGamertag;
                            r.score = sv.numberValue;
                            if (sv.type == XblTitleManagedStatType::Number) {
                                char nb[64]{};
                                std::snprintf(nb, sizeof(nb), "%.6g", sv.numberValue);
                                r.valStr = nb;
                            } else {
                                r.valStr = sv.stringValue;
                            }
                            c->rows.push_back(std::move(r));
                            break;
                        }
                    }
                }
                std::sort(c->rows.begin(), c->rows.end(), [](const LbRowOwned& a, const LbRowOwned& b) {
                    return a.score > b.score;
                });
                if (c->rows.size() > c->maxItems) c->rows.resize(c->maxItems);
                size_t req = sizeof(XblLeaderboardResult) +
                             c->rows.size() * (sizeof(XblLeaderboardRow) + sizeof(const char*));
                for (const auto& r : c->rows) req += r.valStr.size() + 1u;
                XAsyncComplete(data->async, S_OK, req);
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                auto* out = static_cast<XblLeaderboardResult*>(data->buffer);
                std::memset(out, 0, sizeof(*out));
                out->totalRowCount = static_cast<uint32_t>(c->rows.size());
                out->rowsCount = c->rows.size();
                out->hasNext = false;
                if (!c->rows.empty()) {
                    auto* rowArr = reinterpret_cast<XblLeaderboardRow*>(out + 1);
                    auto** colPtrArr = reinterpret_cast<const char**>(rowArr + c->rows.size());
                    char* strDst = reinterpret_cast<char*>(colPtrArr + c->rows.size());
                    out->rows = rowArr;
                    for (size_t i = 0; i < c->rows.size(); ++i) {
                        std::memset(&rowArr[i], 0, sizeof(XblLeaderboardRow));
                        strncpy_s(rowArr[i].gamertag, sizeof(rowArr[i].gamertag), c->rows[i].gt.c_str(), _TRUNCATE);
                        strncpy_s(rowArr[i].modernGamertag, sizeof(rowArr[i].modernGamertag), c->rows[i].mgt.c_str(), _TRUNCATE);
                        rowArr[i].xboxUserId = c->rows[i].xuid;
                        rowArr[i].rank = static_cast<uint32_t>(i + 1u);
                        rowArr[i].percentile = 1.0 - (static_cast<double>(i) / static_cast<double>(c->rows.size()));
                        std::memcpy(strDst, c->rows[i].valStr.c_str(), c->rows[i].valStr.size() + 1u);
                        colPtrArr[i] = strDst;
                        strDst += c->rows[i].valStr.size() + 1u;
                        rowArr[i].columnValues = &colPtrArr[i];
                        rowArr[i].columnValuesCount = 1u;
                    }
                }
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) delete c;
            return S_OK;
        });
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XblLeaderboardGetLeaderboardResultSize(
    XAsyncBlock* async,
    size_t* resultSizeInBytes) noexcept {
    return XAsyncGetResultSize(async, resultSizeInBytes);
}

HRESULT STDAPICALLTYPE XblLeaderboardGetLeaderboardResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblLeaderboardResult** ptrToBuffer,
    size_t* bufferUsed) noexcept {
    if (!async || !buffer || !ptrToBuffer) return E_INVALIDARG;
    *ptrToBuffer = nullptr;
    const HRESULT hr = XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XblLeaderboardGetLeaderboardAsync),
        bufferSize, buffer, bufferUsed);
    if (SUCCEEDED(hr)) {
        *ptrToBuffer = static_cast<XblLeaderboardResult*>(buffer);
    }
    return hr;
}

HRESULT STDAPICALLTYPE XblSocialGetSocialRelationshipsAsync(
    XblContextHandle xboxLiveContext,
    uint64_t /*xboxUserId*/,
    uint32_t /*socialRelationshipFilter*/,
    size_t /*startIndex*/,
    size_t /*maxItems*/,
    XAsyncBlock* async) noexcept {
    if (!xboxLiveContext || !async) return E_INVALIDARG;
    const HRESULT hr = XAsyncBegin(
        async, nullptr, reinterpret_cast<const void*>(&XblSocialGetSocialRelationshipsAsync),
        "XblSocialGetSocialRelationshipsAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, sizeof(XblSocialRelationshipResultHandle));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                if (!data->buffer || data->bufferSize < sizeof(XblSocialRelationshipResultHandle)) {
                    return E_NOT_SUFFICIENT_BUFFER;
                }
                auto* res = new (std::nothrow) XblSocialRelationshipResult();
                if (!res) return E_OUTOFMEMORY;
                *static_cast<XblSocialRelationshipResultHandle*>(data->buffer) = res;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) return hr;
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XblSocialGetSocialRelationshipsResult(
    XAsyncBlock* async,
    XblSocialRelationshipResultHandle* handle) noexcept {
    if (!async || !handle) return E_INVALIDARG;
    *handle = nullptr;
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XblSocialGetSocialRelationshipsAsync),
        sizeof(XblSocialRelationshipResultHandle), handle, nullptr);
}

HRESULT STDAPICALLTYPE XblSocialRelationshipResultGetRelationships(
    XblSocialRelationshipResultHandle resultHandle,
    const XblSocialRelationship** relationships,
    size_t* relationshipsCount) noexcept {
    if (!resultHandle || !relationships || !relationshipsCount) return E_INVALIDARG;
    *relationships = resultHandle->items.empty() ? nullptr : resultHandle->items.data();
    *relationshipsCount = resultHandle->items.size();
    return S_OK;
}

void STDAPICALLTYPE XblSocialRelationshipResultCloseHandle(
    XblSocialRelationshipResultHandle resultHandle) noexcept {
    delete resultHandle;
}

HRESULT STDAPICALLTYPE XblMultiplayerManagerInitialize(
    const char* lobbySessionTemplateName,
    XTaskQueueHandle /*asyncQueue*/) noexcept {
    auto& ws = WaveAStore();
    std::lock_guard<std::mutex> lk(ws.mtx);
    ws.mpInitialized = true;
    ws.mpLobbyTemplate = lobbySessionTemplateName ? lobbySessionTemplateName : "OfflineLobby";
    ws.mpEvents.clear();
    return S_OK;
}

HRESULT STDAPICALLTYPE XblMultiplayerManagerDoWork(
    const XblMultiplayerEvent** multiplayerEvents,
    size_t* multiplayerEventsCount) noexcept {
    if (!multiplayerEvents || !multiplayerEventsCount) return E_INVALIDARG;
    auto& ws = WaveAStore();
    std::lock_guard<std::mutex> lk(ws.mtx);
    *multiplayerEvents = ws.mpEvents.empty() ? nullptr : ws.mpEvents.data();
    *multiplayerEventsCount = ws.mpEvents.size();
    return S_OK;
}

size_t STDAPICALLTYPE XblMultiplayerManagerLobbySessionLocalMembersCount() noexcept {
    auto& ws = WaveAStore();
    std::lock_guard<std::mutex> lk(ws.mtx);
    return ws.mpInitialized ? 1u : 0u;
}

} // extern "C"
