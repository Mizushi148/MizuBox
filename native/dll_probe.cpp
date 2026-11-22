// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Diagnostic probe that loads real Xbox Series DLLs from a build folder
// against MizuBox Windows x64 shims (d3d12_xs.dll + xmem.dll)
// and optionally runs safe runtime smoke checks on exported functions.

#include <Windows.h>
#include <d3d12.h>

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path executable_directory() {
    std::array<wchar_t, 32768> path{};
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) {
        return {};
    }
    return std::filesystem::path(path.data()).parent_path();
}

struct TargetDll {
    const wchar_t* filename;
    const char* probe_export;
};

constexpr TargetDll kTargets[] = {
    {L"Bink2Scarlett.dll", "BinkGetError"},
    {L"amd_fidelityfx_loader_xbox.dll", "ffxQuery"},
    {L"amd_fidelityfx_upscaler_xbox.dll", "ffxQuery"},
    {L"XboxBvhBuilder.dll", "??0XboxBvhBuilder@@QEAA@PEAUID3D12Device11@@_N@Z"},
    {L"XCurl.dll", nullptr},
    {L"libHttpClient.GDK.dll", nullptr},
};

// Public AMD FidelityFX API v2.0 query header & GetVersions descriptor
struct FfxApiHeader {
    uint64_t type;
    FfxApiHeader* pNext;
};

struct FfxApiQueryDescGetVersions {
    FfxApiHeader header;
    uint64_t createDescType;
    void* device;
    uint64_t* outputCount;
    uint64_t* versionIds;
    const char** versionNames;
};

using FfxQueryProc = uint32_t (*)(void** context, FfxApiHeader* desc);

// XboxBvhBuilder::GetRaytracingAccelerationStructurePrebuildInfo(this, inputs, streamDesc, outInfo)
using BvhPrebuildProc = void (*)(void* self,
                                 const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* inputs,
                                 const void* stream_desc,
                                 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* out_info);

void probe_ffx_versions(HMODULE mod, const char* label) {
    const auto fn = std::bit_cast<FfxQueryProc>(GetProcAddress(mod, "ffxQuery"));
    if (!fn) {
        return;
    }
    constexpr uint64_t kFfxApiQueryGetVersions = 0x00000004ULL;
    constexpr uint64_t kFfxApiCreateContextDescUpscale = 0x00010000ULL;
    uint64_t count = 0;
    FfxApiQueryDescGetVersions q{};
    q.header.type = kFfxApiQueryGetVersions;
    q.createDescType = kFfxApiCreateContextDescUpscale;
    q.outputCount = &count;

    uint32_t rc = 0xFFFFFFFFu;
    DWORD exc = 0;
    __try {
        rc = fn(nullptr, &q.header);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
    }
    if (exc != 0) {
        std::printf("    [RUNTIME] %s ffxQuery(GetVersions) exception 0x%08lX\n", label, exc);
        return;
    }
    std::printf("    [RUNTIME] %s ffxQuery(GetVersions) rc=%u count=%llu\n",
                label, rc, static_cast<unsigned long long>(count));
    if (rc == 0 && count > 0 && count <= 16) {
        std::array<uint64_t, 16> ids{};
        std::array<const char*, 16> names{};
        q.versionIds = ids.data();
        q.versionNames = names.data();
        __try {
            rc = fn(nullptr, &q.header);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            exc = GetExceptionCode();
        }
        if (exc == 0 && rc == 0) {
            for (uint64_t i = 0; i < count; ++i) {
                std::printf("      version[%llu]: id=0x%016llX name=%s\n",
                            static_cast<unsigned long long>(i),
                            static_cast<unsigned long long>(ids[i]),
                            names[i] ? names[i] : "(null)");
            }
        }
    }
}

void probe_bvh_prebuild(HMODULE mod) {
    const auto fn = std::bit_cast<BvhPrebuildProc>(GetProcAddress(
        mod,
        "?GetRaytracingAccelerationStructurePrebuildInfo@XboxBvhBuilder@@QEAAXPEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS@@PEBUD3D12XBOX_XDXR_STREAM_DESC@@PEAUD3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO@@@Z"));
    if (!fn) {
        return;
    }
    D3D12_RAYTRACING_GEOMETRY_DESC geom{};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.VertexCount = 300;
    geom.Triangles.VertexBuffer.StrideInBytes = 12;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = 1;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = &geom;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    alignas(16) unsigned char dummy_self[256]{};
    DWORD exc = 0;
    __try {
        fn(dummy_self, &inputs, nullptr, &info);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
    }
    if (exc != 0) {
        std::printf("    [RUNTIME] XboxBvhBuilder::GetPrebuildInfo exception 0x%08lX\n", exc);
        return;
    }
    std::printf("    [RUNTIME] XboxBvhBuilder::GetPrebuildInfo(300 verts): Result=%llu B, Scratch=%llu B, Update=%llu B\n",
                static_cast<unsigned long long>(info.ResultDataMaxSizeInBytes),
                static_cast<unsigned long long>(info.ScratchDataSizeInBytes),
                static_cast<unsigned long long>(info.UpdateScratchDataSizeInBytes));
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: wsx_dll_probe <build_directory> [shim_directory] [--runtime-test]\n");
        return 2;
    }

    const std::filesystem::path build_dir = std::filesystem::absolute(argv[1]);
    std::filesystem::path shim_dir = executable_directory();
    bool runtime_test = false;
    for (int i = 2; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg == L"--runtime-test") {
            runtime_test = true;
        } else {
            shim_dir = std::filesystem::absolute(argv[i]);
        }
    }

    if (!std::filesystem::is_directory(build_dir)) {
        std::fprintf(stderr, "Build directory not found: %ls\n", build_dir.c_str());
        return 2;
    }
    if (!std::filesystem::is_directory(shim_dir)) {
        std::fprintf(stderr, "Shim directory not found: %ls\n", shim_dir.c_str());
        return 2;
    }

    std::printf("Build dir : %ls\n", build_dir.c_str());
    std::printf("Shim dir  : %ls\n", shim_dir.c_str());

    const auto d3d12_shim_path = shim_dir / L"d3d12_xs.dll";
    const auto xmem_shim_path = shim_dir / L"xmem.dll";
    const HMODULE h_d3d12 = LoadLibraryExW(d3d12_shim_path.c_str(), nullptr,
                                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    const HMODULE h_xmem = LoadLibraryExW(xmem_shim_path.c_str(), nullptr,
                                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h_d3d12 || !h_xmem) {
        std::fprintf(stderr, "Failed to preload shims (d3d12_xs=%p, xmem=%p, err=%lu)\n",
                     static_cast<void*>(h_d3d12), static_cast<void*>(h_xmem), GetLastError());
        return 1;
    }
    std::puts("Preloaded shims: d3d12_xs.dll + xmem.dll");

    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    AddDllDirectory(shim_dir.c_str());
    AddDllDirectory(build_dir.c_str());

    int loaded = 0;
    int skipped = 0;
    int failed = 0;

    for (const auto& target : kTargets) {
        const auto full = build_dir / target.filename;
        if (!std::filesystem::is_regular_file(full)) {
            std::printf("  [SKIP] %-36ls (not present)\n", target.filename);
            ++skipped;
            continue;
        }
        const HMODULE mod = LoadLibraryExW(full.c_str(), nullptr,
                                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                               LOAD_LIBRARY_SEARCH_USER_DIRS |
                                               LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!mod) {
            std::printf("  [FAIL] %-36ls Win32 error=%lu\n", target.filename, GetLastError());
            ++failed;
            continue;
        }
        bool export_ok = true;
        if (target.probe_export) {
            export_ok = (GetProcAddress(mod, target.probe_export) != nullptr);
        }
        std::printf("  [ OK ] %-36ls base=%p export=%s\n",
                    target.filename,
                    static_cast<void*>(mod),
                    target.probe_export ? (export_ok ? "OK" : "MISSING") : "n/a");

        if (runtime_test && export_ok) {
            const std::wstring_view fname(target.filename);
            if (fname == L"amd_fidelityfx_upscaler_xbox.dll") {
                probe_ffx_versions(mod, "amd_fidelityfx_upscaler_xbox.dll");
            } else if (fname == L"XboxBvhBuilder.dll") {
                probe_bvh_prebuild(mod);
            }
        }

        if (!export_ok) {
            ++failed;
        } else {
            ++loaded;
        }
        FreeLibrary(mod);
    }

    FreeLibrary(h_xmem);
    FreeLibrary(h_d3d12);

    std::printf("Summary: loaded=%d skipped=%d failed=%d\n", loaded, skipped, failed);
    return failed == 0 ? 0 : 1;
}
