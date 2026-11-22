// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Shared configuration loader (wsx.ini + WSX_* env var overrides) and API Call Tracer (WSX_TRACE=1)
// for d3d12_xs.dll, xgameruntime.dll, xmem.dll, and wsx_bootstrap.exe.

#pragma once

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace wsx {

namespace detail {

inline std::string trim_ascii(std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

inline std::string to_lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

inline std::filesystem::path get_module_dir(HMODULE mod) noexcept {
    wchar_t buf[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(mod, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::filesystem::path(buf).parent_path();
}

inline HMODULE current_image_module() noexcept {
    HMODULE mod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&current_image_module),
        &mod);
    return mod;
}

} // namespace detail

struct ConfigStore {
    std::filesystem::path loaded_path;
    std::unordered_map<std::string, std::string> values; // "section.key" (lowercased) -> value

    static const ConfigStore& instance() noexcept {
        static const ConfigStore store = load();
        return store;
    }

    static std::filesystem::path find_ini_path() {
        namespace fs = std::filesystem;
        std::error_code ec;

        wchar_t env_cfg[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"WSX_CONFIG_PATH", env_cfg, MAX_PATH) > 0 && env_cfg[0] != L'\0') {
            fs::path p(env_cfg);
            if (fs::is_regular_file(p, ec)) return p;
        }

        // 1. Current working directory
        {
            fs::path cwd = fs::current_path(ec);
            if (!ec) {
                if (fs::is_regular_file(cwd / L"wsx.ini", ec)) return cwd / L"wsx.ini";
            }
        }

        // 2. Executable directory and parent
        {
            fs::path exe_dir = detail::get_module_dir(nullptr);
            if (!exe_dir.empty()) {
                if (fs::is_regular_file(exe_dir / L"wsx.ini", ec)) return exe_dir / L"wsx.ini";
                if (exe_dir.has_parent_path()) {
                    fs::path parent = exe_dir.parent_path();
                    if (fs::is_regular_file(parent / L"wsx.ini", ec)) return parent / L"wsx.ini";
                    if (parent.has_parent_path() &&
                        fs::is_regular_file(parent.parent_path() / L"wsx.ini", ec)) {
                        return parent.parent_path() / L"wsx.ini";
                    }
                }
            }
        }

        // 3. Current DLL directory (e.g. bin\d3d12_xs.dll -> ..\wsx.ini)
        {
            fs::path dll_dir = detail::get_module_dir(detail::current_image_module());
            if (!dll_dir.empty()) {
                if (fs::is_regular_file(dll_dir / L"wsx.ini", ec)) return dll_dir / L"wsx.ini";
                if (dll_dir.has_parent_path() &&
                    fs::is_regular_file(dll_dir.parent_path() / L"wsx.ini", ec)) {
                    return dll_dir.parent_path() / L"wsx.ini";
                }
            }
        }

        return {};
    }

    static ConfigStore load() {
        ConfigStore result;
        result.loaded_path = find_ini_path();
        if (result.loaded_path.empty()) {
            return result;
        }
        std::ifstream in(result.loaded_path);
        if (!in) {
            return result;
        }
        std::string section;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // Strip inline comments starting with ';' or '#' when not inside quotes
            // (keep '#' inside gamertags like LocalPlayer#0001 by only treating ';' or leading '#' as comments)
            const std::string trimmed = detail::trim_ascii(line);
            if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') continue;
            if (trimmed.front() == '[' && trimmed.back() == ']') {
                section = detail::to_lower_ascii(detail::trim_ascii(trimmed.substr(1, trimmed.size() - 2)));
                continue;
            }
            const auto eq = trimmed.find('=');
            if (eq == std::string::npos) continue;
            std::string key = detail::to_lower_ascii(detail::trim_ascii(trimmed.substr(0, eq)));
            std::string val = trimmed.substr(eq + 1);
            const auto semi = val.find(';');
            if (semi != std::string::npos) val = val.substr(0, semi);
            val = detail::trim_ascii(val);
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                val = val.substr(1, val.size() - 2);
            }
            if (!key.empty()) {
                std::string full_key = section.empty() ? key : (section + "." + key);
                result.values[full_key] = val;
            }
        }
        return result;
    }

    std::string get_raw(const char* section, const char* key) const {
        std::string full_key = detail::to_lower_ascii(section) + "." + detail::to_lower_ascii(key);
        auto it = values.find(full_key);
        return (it != values.end()) ? it->second : std::string{};
    }
};

// Priority: Environment Variable (env_name) > wsx.ini ([section] key) > default_val
inline std::string get_config_string(
    const char* env_name,
    const char* ini_section,
    const char* ini_key,
    const char* default_val = "") {
    if (env_name && env_name[0] != '\0') {
        char buf[1024]{};
        const DWORD len = GetEnvironmentVariableA(env_name, buf, static_cast<DWORD>(sizeof(buf)));
        if (len > 0 && len < sizeof(buf)) {
            return std::string(buf, len);
        }
    }
    if (ini_section && ini_key) {
        std::string v = ConfigStore::instance().get_raw(ini_section, ini_key);
        if (!v.empty()) {
            return v;
        }
    }
    return default_val ? std::string(default_val) : std::string{};
}

inline bool parse_bool_str(const std::string& v, bool default_val) noexcept {
    if (v.empty()) return default_val;
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(v[0])));
    if (c == '1' || c == 't' || c == 'y') return true;
    if (c == '0' || c == 'f' || c == 'n') return false;
    return default_val;
}

inline bool get_config_bool(
    const char* env_name,
    const char* ini_section,
    const char* ini_key,
    bool default_val = false) {
    const std::string v = get_config_string(env_name, ini_section, ini_key, "");
    return parse_bool_str(v, default_val);
}

inline int64_t get_config_int(
    const char* env_name,
    const char* ini_section,
    const char* ini_key,
    int64_t default_val = 0) {
    const std::string v = get_config_string(env_name, ini_section, ini_key, "");
    if (v.empty()) return default_val;
    char* end = nullptr;
    const long long parsed = std::strtoll(v.c_str(), &end, 0);
    return (end && end != v.c_str()) ? static_cast<int64_t>(parsed) : default_val;
}

inline uint64_t get_config_uint64(
    const char* env_name,
    const char* ini_section,
    const char* ini_key,
    uint64_t default_val = 0) {
    const std::string v = get_config_string(env_name, ini_section, ini_key, "");
    if (v.empty()) return default_val;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(v.c_str(), &end, 0);
    return (end && end != v.c_str()) ? static_cast<uint64_t>(parsed) : default_val;
}

// ============================================================================
// Structured API Call Tracer (WSX_TRACE=1 or [Diagnostics] trace=1)
// ============================================================================
class ApiTracer {
    bool enabled_ = false;
    std::wstring log_path_;
    int64_t qpc_start_ = 0;
    int64_t qpc_freq_ = 1;
    std::mutex mtx_;

    ApiTracer() {
        LARGE_INTEGER freq{}, now{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        qpc_freq_ = (freq.QuadPart > 0) ? freq.QuadPart : 1;
        qpc_start_ = now.QuadPart;

        enabled_ = get_config_bool("WSX_TRACE", "Diagnostics", "trace", false);
        if (enabled_) {
            const std::string custom = get_config_string("WSX_TRACE_FILE", "Diagnostics", "trace_file", "");
            if (!custom.empty()) {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, custom.c_str(), -1, nullptr, 0);
                if (wlen > 1) {
                    std::wstring wp(static_cast<size_t>(wlen - 1), L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, custom.c_str(), -1, wp.data(), wlen);
                    std::filesystem::path p(wp);
                    if (p.is_relative()) {
                        p = detail::get_module_dir(nullptr) / p;
                    }
                    log_path_ = p.wstring();
                }
            }
            if (log_path_.empty()) {
                log_path_ = (detail::get_module_dir(nullptr) / L"wsx_trace.log").wstring();
            }
        }
    }

public:
    static ApiTracer& instance() noexcept {
        static ApiTracer tracer;
        return tracer;
    }

    bool enabled() const noexcept { return enabled_; }

    void write_line(const char* module_name, const char* func_name, HRESULT hr,
                    const char* detail_fmt, va_list ap) noexcept {
        if (!enabled_) return;

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double ms = static_cast<double>(now.QuadPart - qpc_start_) * 1000.0 /
                          static_cast<double>(qpc_freq_);
        const DWORD tid = GetCurrentThreadId();

        char details[512]{};
        if (detail_fmt && detail_fmt[0] != '\0') {
            std::vsnprintf(details, sizeof(details), detail_fmt, ap);
        }

        char line[768]{};
        const int n = std::snprintf(
            line, sizeof(line), "[%9.2f ms][TID:%5lu][%-12s] %s(%s) -> 0x%08lX\r\n",
            ms, static_cast<unsigned long>(tid), module_name ? module_name : "wsx",
            func_name ? func_name : "?", details, static_cast<unsigned long>(hr));
        if (n <= 0) return;

        std::lock_guard<std::mutex> lock(mtx_);
        const HANDLE h = CreateFileW(
            log_path_.c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, line, static_cast<DWORD>(std::min<int>(n, static_cast<int>(sizeof(line) - 1))),
                      &written, nullptr);
            CloseHandle(h);
        }
    }
};

inline HRESULT trace_hr(const char* module_name, const char* func_name, HRESULT hr,
                        const char* detail_fmt = "", ...) noexcept {
    auto& tracer = ApiTracer::instance();
    if (!tracer.enabled()) return hr;
    va_list ap;
    va_start(ap, detail_fmt);
    tracer.write_line(module_name, func_name, hr, detail_fmt, ap);
    va_end(ap);
    return hr;
}

inline void trace_void(const char* module_name, const char* func_name,
                       const char* detail_fmt = "", ...) noexcept {
    auto& tracer = ApiTracer::instance();
    if (!tracer.enabled()) return;
    va_list ap;
    va_start(ap, detail_fmt);
    tracer.write_line(module_name, func_name, S_OK, detail_fmt, ap);
    va_end(ap);
}

} // namespace wsx
