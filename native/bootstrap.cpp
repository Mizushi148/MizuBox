// Copyright (c) 2026 edupode
// SPDX-License-Identifier: CPAL-1.0
//
// The contents of this file are subject to the Common Public Attribution License Version 1.0 (the "License"); you may not use this file except in compliance with the License. You may obtain a copy of the License at https://opensource.org/license/CPAL-1.0. The License is based on the Mozilla Public License Version 1.1 but Sections 14 and 15 have been added to cover use of software over a computer network and provide for limited attribution for the Original Developer. In addition, Exhibit A has been modified to be consistent with Exhibit B.
// Software distributed under the License is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License for the specific language governing rights and limitations under the License.
// The Original Code is AnyXboxSeries.
// The Original Developer is the Initial Developer.
// The Initial Developer of the Original Code is edupode. All portions of the code written by edupode are Copyright (c) 2026 edupode. All Rights Reserved.
//

#include "boot_api.h"
#include "graphics_runtime.h"
#include "wsx_config.h"
#include "wsx_crash_diag.h"
#include "wsx_pe_loader.h"

#include <wrl/client.h>

#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

namespace {
std::filesystem::path executable_directory() {
    std::array<wchar_t, 32768> path{};
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) {
        return {};
    }
    return std::filesystem::path(path.data()).parent_path();
}

std::wstring parse_manifest_executable(const std::filesystem::path& package_dir) {
    for (const wchar_t* name : {L"MicrosoftGame.Config", L"MicrosoftGameConfig.mgc"}) {
        const auto file = package_dir / name;
        if (!std::filesystem::is_regular_file(file)) {
            continue;
        }
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            continue;
        }
        std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::size_t search_from = 0;
        std::size_t exe_pos = std::string::npos;
        while ((exe_pos = xml.find("<Executable", search_from)) != std::string::npos) {
            const char next_ch = (exe_pos + 11 < xml.size()) ? xml[exe_pos + 11] : '\0';
            if (next_ch == ' ' || next_ch == '\t' || next_ch == '\r' || next_ch == '\n') {
                break;
            }
            search_from = exe_pos + 11;
        }
        if (exe_pos == std::string::npos) {
            continue;
        }
        const auto end_tag = xml.find('>', exe_pos);
        const auto name_pos = xml.find("Name=\"", exe_pos);
        if (name_pos != std::string::npos && (end_tag == std::string::npos || name_pos < end_tag)) {
            const auto start = name_pos + 6;
            const auto quote = xml.find('"', start);
            if (quote != std::string::npos) {
                const std::string utf8_name = xml.substr(start, quote - start);
                const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_name.data(),
                                                     static_cast<int>(utf8_name.size()), nullptr, 0);
                if (wlen > 0) {
                    std::wstring result(static_cast<std::size_t>(wlen), L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, utf8_name.data(),
                                        static_cast<int>(utf8_name.size()), result.data(), wlen);
                    return result;
                }
            }
        }
    }
    return {};
}

int launch_package_executable(const std::filesystem::path& package_dir,
                              const std::filesystem::path& exe_path,
                              AdapterMode adapter_mode,
                              bool in_process,
                              bool allow_auto_stubs) {
    const auto bin_dir = executable_directory();
    if (!bin_dir.empty()) {
        SetEnvironmentVariableW(L"WSX_BIN_DIR", bin_dir.c_str());
        wchar_t old_path[32768]{};
        GetEnvironmentVariableW(L"PATH", old_path, static_cast<DWORD>(std::size(old_path)));
        std::wstring new_path = bin_dir.wstring() + L";" + package_dir.wstring() + L";" + old_path;
        SetEnvironmentVariableW(L"PATH", new_path.c_str());
    }
    if (adapter_mode == AdapterMode::Warp) {
        SetEnvironmentVariableW(L"WSX_WARP", L"1");
    }
    const auto ini_path = wsx::ConfigStore::instance().loaded_path;
    if (!ini_path.empty()) {
        wchar_t existing_cfg[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"WSX_CONFIG_PATH", existing_cfg, MAX_PATH) == 0) {
            SetEnvironmentVariableW(L"WSX_CONFIG_PATH", ini_path.c_str());
        }
        std::printf("Config: %ls\n", ini_path.c_str());
    }

    if (in_process) {
        std::printf("Mapping Xbox Title PE64 In-Process via wsx_pe_loader: %ls\n", exe_path.c_str());
        std::fflush(stdout);
        SetCurrentDirectoryW(package_dir.c_str());
        wsx_pe::LoadOptions opts{};
        opts.shim_dir = bin_dir;
        opts.package_dir = package_dir;
        opts.allow_auto_stubs = allow_auto_stubs;
        opts.trace_imports = true;
        wsx_pe::MappedPeImage img;
        if (!img.load_from_file(exe_path, opts)) {
            std::fprintf(stderr, "In-process PE64 mapping failed: %s\n", img.last_error().c_str());
            return 3;
        }
        const auto& st = img.stats();
        std::printf("Mapped PE64 @0x%016llX (pref=0x%016llX, size=%u B, relocs=%u, dlls=%u, shims=%u, imports=%u, ord=%u, stubs=%u, pdata=%u, tls=%u)\n",
                    static_cast<unsigned long long>(st.mapped_base),
                    static_cast<unsigned long long>(st.preferred_base),
                    st.image_size, st.reloc_count, st.imported_dlls, st.shim_dll_hits,
                    st.resolved_imports, st.ordinal_imports, st.auto_stub_count,
                    st.pdata_functions, st.tls_callbacks_run);
        std::fflush(stdout);
        return img.run_exe_entry();
    }

    if (!bin_dir.empty()) {
        // For DLLs that also exist in Windows System32 (xgameruntime.dll on Win11 26100+ and GameInput.dll),
        // stage our shims into package_dir so the child process binds MizuBox shims before System32.
        std::error_code ec;
        for (const wchar_t* shim_name : {L"GameInput.dll", L"xgameruntime.dll"}) {
            const auto src = bin_dir / shim_name;
            const auto dst = package_dir / shim_name;
            if (std::filesystem::is_regular_file(src, ec)) {
                std::filesystem::copy_file(src, dst,
                                           std::filesystem::copy_options::overwrite_existing, ec);
            }
        }
    }

    std::printf("Launching Xbox Title Executable via MizuBox shims: %ls\n", exe_path.c_str());
    std::fflush(stdout);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe_path.wstring() + L"\"";
    if (!CreateProcessW(exe_path.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, package_dir.c_str(), &si, &pi)) {
        std::fprintf(stderr, "CreateProcessW failed with Win32 error %lu: %ls\n",
                     GetLastError(), exe_path.c_str());
        return 3;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::printf("Xbox Title Executable exited with code %lu (0x%08lX)\n",
                exit_code, exit_code);
    return static_cast<int>(exit_code);
}

int boot(const wchar_t* build_path, const wchar_t* selected_module, bool play_sample,
         AdapterMode adapter_mode, bool in_process, bool allow_auto_stubs) {
    std::filesystem::path package;
    if (build_path) {
        package = std::filesystem::absolute(build_path);
        if (!std::filesystem::is_directory(package)) {
            std::fprintf(stderr, "Build directory unavailable: %ls\n", package.c_str());
            return 2;
        }
        std::printf("Build directory: %ls\n", package.c_str());
        if (!selected_module && !play_sample) {
            const std::wstring declared_exe = parse_manifest_executable(package);
            if (!declared_exe.empty()) {
                const auto candidate_exe = package / declared_exe;
                if (std::filesystem::is_regular_file(candidate_exe)) {
                    return launch_package_executable(package, candidate_exe, adapter_mode, in_process, allow_auto_stubs);
                }
                std::printf("Manifest declares %ls (not present in folder); running graphics probe.\n",
                            declared_exe.c_str());
            }
        }
    }

    std::filesystem::path module_path;
    if (selected_module) {
        module_path = std::filesystem::absolute(selected_module);
    } else {
        const auto directory = executable_directory();
        if (directory.empty()) {
            std::fprintf(stderr, "Could not determine the bootstrap directory\n");
            return 3;
        }
        module_path = directory / (play_sample ? L"wsx_sample_game.dll" : L"wsx_boot_graphics.dll");
    }
    constexpr DWORD search_order = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
    const std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)> module(
        LoadLibraryExW(module_path.c_str(), nullptr, search_order), FreeLibrary);
    if (!module) {
        std::fprintf(stderr, "LoadLibraryExW returned Win32 error %lu: %ls\n",
                     GetLastError(), module_path.c_str());
        return 3;
    }
    const auto start = std::bit_cast<WsxStartV1Proc>(GetProcAddress(module.get(), wsx_start_symbol));
    if (!start) {
        std::fprintf(stderr, "%s entry missing: %ls\n", wsx_start_symbol, module_path.c_str());
        return 3;
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(create_graphics_device(adapter_mode, device.GetAddressOf()))) {
        return 1;
    }
    std::printf("Calling %s in %ls\n", wsx_start_symbol, module_path.filename().c_str());
    std::fflush(stdout);
    const HRESULT status = start(device.Get(), package.empty() ? nullptr : package.c_str());
    std::printf("Module returned 0x%08lX\n", static_cast<unsigned long>(status));
    return FAILED(status) ? 1 : 0;
}
}

int wmain(int argc, wchar_t** argv) {
    if (wsx::get_config_bool("WSX_CRASH_DUMPS", "Diagnostics", "crash_dumps", true)) {
        wsx_diag::install_crash_handler();
    }
    const wchar_t* build_path = nullptr;
    const wchar_t* selected_module = nullptr;
    bool play_sample = false;
    bool in_process = false;
    bool allow_auto_stubs = false;
    AdapterMode adapter_mode = AdapterMode::Preferred;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg == L"--warp") {
            adapter_mode = AdapterMode::Warp;
        } else if (arg == L"--legacy-dxgi") {
            adapter_mode = AdapterMode::Standard;
        } else if (arg == L"--in-process") {
            in_process = true;
        } else if (arg == L"--auto-stubs") {
            allow_auto_stubs = true;
        } else if (arg == L"--build" && i + 1 < argc) {
            build_path = argv[++i];
        } else if (arg == L"--sample" && !selected_module) {
            play_sample = true;
        } else if (arg == L"--module" && !play_sample && i + 1 < argc) {
            selected_module = argv[++i];
        } else {
            std::fprintf(stderr, "Usage: wsx_bootstrap [--build directory] [--in-process] [--auto-stubs] [--sample | --module path.dll] [--warp | --legacy-dxgi]\n");
            return 2;
        }
    }
    try {
        return boot(build_path, selected_module, play_sample, adapter_mode, in_process, allow_auto_stubs);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
