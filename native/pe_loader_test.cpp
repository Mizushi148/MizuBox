// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Automated CTest for MizuBox In-Process PE32+ Manual Mapper (wsx_pe_loader.h):
// 1. Maps wsx_pe_fixture.dll at a FORCED relocated base (delta != 0).
// 2. Verifies DIR64 relocations, .pdata SEH/C++ try-catch unwinding (RtlAddFunctionTable),
//    TLS callback execution, static IAT binding to xmem.dll, ordinal export lookup (@7), and
//    dynamic LoadLibraryW("xgameruntime.dll") shim redirection beating C:\Windows\System32.
// 3. Mutates an in-memory copy of the PE to import a nonexistent symbol, maps it with
//    allow_auto_stubs=true, invokes the generated x86-64 auto-stub thunk, and verifies clean execution.

#include "wsx_pe_loader.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define CHECK_COND(cond, msg)                                                           \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::fprintf(stderr, "FAIL: %s (%s) at line %d\n", msg, #cond, __LINE__);   \
            return 1;                                                                   \
        }                                                                               \
    } while (0)

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    CHECK_COND(argc >= 3, "Usage: wsx_pe_loader_test <fixture.dll> <shim_dir>");
    const std::filesystem::path fixture_path = argv[1];
    const std::filesystem::path shim_dir = std::filesystem::absolute(argv[2]);

    // Test 1: Manual mapping with forced relocation, SEH, TLS, shim IAT & dynamic LoadLibrary redirection
    {
        wsx_pe::LoadOptions opts{};
        opts.shim_dir = shim_dir;
        opts.package_dir = fixture_path.parent_path();
        opts.force_relocate = true;
        opts.allow_auto_stubs = false;

        wsx_pe::MappedPeImage img;
        if (!img.load_from_file(fixture_path, opts)) {
            std::fprintf(stderr, "FAIL: load_from_file failed: %s\n", img.last_error().c_str());
            return 1;
        }

        const auto& st = img.stats();
        std::printf("[PE64] Mapped %s @0x%016llX (pref=0x%016llX, relocs=%u, dlls=%u, shims=%u, imports=%u, pdata=%u, tls=%u)\n",
                    fixture_path.filename().string().c_str(),
                    static_cast<unsigned long long>(st.mapped_base),
                    static_cast<unsigned long long>(st.preferred_base),
                    st.reloc_count, st.imported_dlls, st.shim_dll_hits,
                    st.resolved_imports, st.pdata_functions, st.tls_callbacks_run);

        CHECK_COND(st.mapped_base != st.preferred_base, "Forced relocation must move image base");
        CHECK_COND(st.reloc_count > 0u, "Image must apply DIR64 relocations");
        CHECK_COND(st.pdata_functions > 0u, ".pdata SEH unwind table must be registered");
        CHECK_COND(st.tls_callbacks_run >= 1u, "TLS callback must execute on attach");
        CHECK_COND(st.shim_dll_hits >= 1u, "Static import xmem.dll must hit shim_dir");

        using RunChecksFn = int(__cdecl*)(wchar_t*, uint32_t);
        auto run_checks = reinterpret_cast<RunChecksFn>(img.find_export("wsx_fixture_run_checks"));
        CHECK_COND(run_checks != nullptr, "Named export wsx_fixture_run_checks must resolve");

        wchar_t xgr_path[MAX_PATH]{};
        const int rc = run_checks(xgr_path, MAX_PATH);
        std::printf("[PE64] wsx_fixture_run_checks returned %d | guest xgameruntime.dll = %ls\n", rc, xgr_path);
        CHECK_COND(rc == 1337, "Fixture checks (reloc + C++ throw/catch + XMemAlloc + LoadLibrary) must return 1337");

        const std::wstring xgr_low = wsx_pe::detail::to_lower_wide(xgr_path);
        const std::wstring shim_low = wsx_pe::detail::to_lower_wide(shim_dir.wstring());
        CHECK_COND(xgr_low.find(L"\\system32\\") == std::wstring::npos,
                   "Guest LoadLibraryW(xgameruntime.dll) must NOT resolve to Windows System32");
        CHECK_COND(xgr_low.find(shim_low) == 0u,
                   "Guest LoadLibraryW(xgameruntime.dll) must resolve inside MizuBox shim_dir");

        using OrdFn = int(__cdecl*)(int);
        auto ord7 = reinterpret_cast<OrdFn>(img.find_export_ordinal(7));
        CHECK_COND(ord7 != nullptr && ord7(10) == 37, "Ordinal @7 export must resolve and compute 10*3+7=37");
    }

    // Test 2: Auto-stub x86-64 thunk generation for missing Xbox imports
    {
        std::ifstream f(fixture_path, std::ios::binary | std::ios::ate);
        CHECK_COND(static_cast<bool>(f), "Read fixture bytes");
        const size_t sz = static_cast<size_t>(f.tellg());
        f.seekg(0, std::ios::beg);
        std::vector<uint8_t> raw(sz);
        f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(sz));

        // Corrupt the ASCII import name "XMemFree" -> "XMemZZZZ" inside the raw PE image to force an auto-stub
        const char* needle = "XMemFree";
        bool patched = false;
        for (size_t i = 0; i + 8 <= raw.size(); ++i) {
            if (std::memcmp(raw.data() + i, needle, 8) == 0) {
                std::memcpy(raw.data() + i, "XMemZZZZ", 8);
                patched = true;
                break;
            }
        }
        CHECK_COND(patched, "Patch XMemFree -> XMemZZZZ in memory copy");

        wsx_pe::LoadOptions opts{};
        opts.shim_dir = shim_dir;
        opts.package_dir = fixture_path.parent_path();
        opts.force_relocate = true;
        opts.allow_auto_stubs = true;

        wsx_pe::MappedPeImage stub_img;
        if (!stub_img.load_from_memory(raw.data(), raw.size(), opts)) {
            std::fprintf(stderr, "FAIL: auto-stub load failed: %s\n", stub_img.last_error().c_str());
            return 1;
        }
        CHECK_COND(stub_img.stats().auto_stub_count == 1u, "Must generate exactly 1 x86-64 auto-stub thunk");

        using RunChecksFn = int(__cdecl*)(wchar_t*, uint32_t);
        auto run_checks = reinterpret_cast<RunChecksFn>(stub_img.find_export("wsx_fixture_run_checks"));
        CHECK_COND(run_checks != nullptr, "Find wsx_fixture_run_checks in auto-stubbed image");
        wchar_t dummy[MAX_PATH]{};
        CHECK_COND(run_checks(dummy, MAX_PATH) == 1337, "Auto-stubbed XMemZZZZ call must execute cleanly without crash");
        CHECK_COND(stub_img.total_auto_stub_hits() == 1u, "Auto-stub thunk hit counter must equal 1");
        std::printf("[PE64] Auto-stub x86-64 thunk verified: stubs=%u hits=%llu\n",
                    stub_img.stats().auto_stub_count,
                    static_cast<unsigned long long>(stub_img.total_auto_stub_hits()));
    }

    std::puts("PASS: pe_loader");
    return 0;
}
