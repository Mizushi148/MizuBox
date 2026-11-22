// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Wave 11 Guest PE64 Compatibility Fixture: exercises CPU affinity remapping,
// ideal processor remapping, AppModel Package/AUMID queries, custom CommandLine,
// and WinRT RoGetActivationFactory fallback inside a manually mapped image.

#include <Windows.h>
#include <appmodel.h>
#include <roapi.h>

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>

struct WsxWave11CompatReport {
    uint32_t affinity_ok;
    uint32_t ideal_proc_ok;
    uint32_t pkg_full_ok;
    uint32_t pkg_family_ok;
    uint32_t aumid_ok;
    uint32_t cmdline_w_ok;
    uint32_t cmdline_a_ok;
    uint32_t winrt_ok;
};

extern "C" uint32_t __cdecl wsx_run_wave11_compat_fixture(WsxWave11CompatReport* out) {
    if (!out) return 0u;
    std::memset(out, 0, sizeof(*out));

    // 1. Out-of-range thread affinity bit (bit 60) -> remapped safely to host core!
    const DWORD_PTR prev_aff = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR(1u) << 60));
    if (prev_aff != 0) {
        SetThreadAffinityMask(GetCurrentThread(), prev_aff);
        out->affinity_ok = 1u;
    }

    // 2. Out-of-range ideal processor (core 31 on 12-thread host) -> remapped modulo host CPUs!
    const DWORD prev_ideal = SetThreadIdealProcessor(GetCurrentThread(), 31u);
    if (prev_ideal != static_cast<DWORD>(-1)) {
        out->ideal_proc_ok = 1u;
    }

    // 3. AppModel identity APIs (PackageFullName, PackageFamilyName, AUMID — hydrated from MicrosoftGame.Config or fallback)
    wchar_t buf[256]{};
    UINT32 len = static_cast<UINT32>(std::size(buf));
    if (GetCurrentPackageFullName(&len, buf) == ERROR_SUCCESS &&
        (std::wcsstr(buf, L"RockstarGamesInc.") != nullptr || std::wcsstr(buf, L"MizuBox.Title") != nullptr)) {
        out->pkg_full_ok = 1u;
    }
    len = static_cast<UINT32>(std::size(buf));
    if (GetCurrentPackageFamilyName(&len, buf) == ERROR_SUCCESS &&
        (std::wcsstr(buf, L"RockstarGamesInc.") != nullptr || std::wcsstr(buf, L"MizuBox.Title") != nullptr)) {
        out->pkg_family_ok = 1u;
    }
    len = static_cast<UINT32>(std::size(buf));
    if (GetCurrentApplicationUserModelId(&len, buf) == ERROR_SUCCESS &&
        (std::wcsstr(buf, L"!GTAO") != nullptr || std::wcsstr(buf, L"!Game") != nullptr)) {
        out->aumid_ok = 1u;
    }

    // 4. Guest CommandLineW / A
    const wchar_t* cmdw = GetCommandLineW();
    if (cmdw && std::wcsstr(cmdw, L"-console=anaconda") != nullptr) {
        out->cmdline_w_ok = 1u;
    }
    const char* cmda = GetCommandLineA();
    if (cmda && std::strstr(cmda, "-console=anaconda") != nullptr) {
        out->cmdline_a_ok = 1u;
    }

    // 5. WinRT RoGetActivationFactory fallback for Xbox-only namespace
    IUnknown* fac = nullptr;
    if (SUCCEEDED(RoGetActivationFactory(nullptr, __uuidof(IUnknown), reinterpret_cast<void**>(&fac))) && fac) {
        fac->Release();
        out->winrt_ok = 1u;
    }

    return (out->affinity_ok && out->ideal_proc_ok && out->pkg_full_ok &&
            out->pkg_family_ok && out->aumid_ok && out->cmdline_w_ok &&
            out->cmdline_a_ok && out->winrt_ok) ? 1u : 0u;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
