// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-Room Crash Diagnostics, Symbolized Stack Walk, WinDbg Minidump (.dmp) Writer
// & Direct3D 12 DRED (Device Removed Extended Data) Auto-Breadcrumb / Page-Fault Reporter.
// Dynamically loads DbgHelp.dll (zero static link dependency).

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "wsx_config.h"

#include <Windows.h>
#include <d3d12.h>
#include <Psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace wsx_diag {

using Microsoft::WRL::ComPtr;

namespace detail {

// Minimal DbgHelp ABI definitions (avoids requiring dbghelp.lib)
// Note: MINIDUMP_EXCEPTION_INFORMATION is 4-byte packed in <minidumpapiset.h> (16 bytes on x64).
#pragma pack(push, 4)
struct WsxMiniDumpExceptionInfo {
    DWORD ThreadId;
    PEXCEPTION_POINTERS ExceptionPointers;
    BOOL ClientPointers;
};
#pragma pack(pop)
static_assert(sizeof(WsxMiniDumpExceptionInfo) == 16u, "MINIDUMP_EXCEPTION_INFORMATION must be 16 bytes");

struct WsxSymbolInfo {
    ULONG SizeOfStruct;
    ULONG TypeIndex;
    ULONG64 Reserved[2];
    ULONG Index;
    ULONG Size;
    ULONG64 ModBase;
    ULONG Flags;
    ULONG64 Value;
    ULONG64 Address;
    ULONG Register;
    ULONG Scope;
    ULONG Tag;
    ULONG NameLen;
    ULONG MaxNameLen;
    char Name[256];
};

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, ULONG, void*, void*, void*);
using SymInitializeFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
using SymFromAddrFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, WsxSymbolInfo*);
using SymCleanupFn = BOOL(WINAPI*)(HANDLE);

inline const char* dred_op_name(D3D12_AUTO_BREADCRUMB_OP op) noexcept {
    switch (op) {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRTV";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDSV";
    case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "BuildRaytracingAS";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return "DispatchRays";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH: return "DispatchMesh";
    default: return "D3D12Op";
    }
}

inline std::string narrow_wstr(const wchar_t* ws) {
    if (!ws || !ws[0]) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, s.data(), n, nullptr, nullptr);
    return s;
}

inline std::wstring make_timestamp_tag() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[64]{};
    std::swprintf(buf, std::size(buf), L"%04u%02u%02u_%02u%02u%02u_%03u",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

} // namespace detail

// Enable D3D12 DRED Auto-Breadcrumbs & Page-Fault reporting BEFORE creating ID3D12Device
inline bool enable_d3d12_dred(HMODULE d3d12_mod = nullptr) noexcept {
    using GetDebugIfFn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE mod = d3d12_mod ? d3d12_mod : GetModuleHandleW(L"d3d12.dll");
    if (!mod) mod = LoadLibraryW(L"d3d12.dll");
    if (!mod) return false;
    const auto pGetDebug = reinterpret_cast<GetDebugIfFn>(GetProcAddress(mod, "D3D12GetDebugInterface"));
    if (!pGetDebug) return false;

    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
    if (FAILED(pGetDebug(IID_PPV_ARGS(&dred))) || !dred) {
        return false;
    }
    dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    return true;
}

// Capture a structured DRED diagnostic report (safe to call on healthy OR removed devices)
inline HRESULT capture_dred_report(ID3D12Device* raw_device, std::string& out_report) {
    if (!raw_device) return E_INVALIDARG;
    std::ostringstream os;
    const HRESULT removed_reason = raw_device->GetDeviceRemovedReason();
    char hex[32]{};
    std::snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(removed_reason));
    os << "=== D3D12 DRED DIAGNOSTICS ===\n";
    os << "DeviceRemovedReason: " << hex << (removed_reason == S_OK ? " (HEALTHY)" : " (DEVICE_REMOVED)") << "\n";

    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(raw_device->QueryInterface(IID_PPV_ARGS(&dred))) || !dred) {
        os << "DRED Interface     : Unavailable (enable WSX_DRED=1 before device creation)\n";
        out_report = os.str();
        return S_OK;
    }

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT ab{};
    const HRESULT hr_ab = dred->GetAutoBreadcrumbsOutput(&ab);
    if (SUCCEEDED(hr_ab)) {
        uint32_t node_count = 0u;
        for (const D3D12_AUTO_BREADCRUMB_NODE* node = ab.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
            ++node_count;
            const std::string cl_name = node->pCommandListDebugNameW
                ? detail::narrow_wstr(node->pCommandListDebugNameW)
                : (node->pCommandListDebugNameA ? std::string(node->pCommandListDebugNameA) : "<unnamed>");
            const UINT completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0u;
            os << "  BreadcrumbNode[" << node_count << "] CmdList='" << cl_name
               << "' completed=" << completed << "/" << node->BreadcrumbCount << "\n";
            if (node->pCommandHistory && node->BreadcrumbCount > 0u) {
                const UINT start = (completed > 4u) ? (completed - 4u) : 0u;
                const UINT end = (std::min)(node->BreadcrumbCount, completed + 4u);
                for (UINT i = start; i < end; ++i) {
                    os << "    [" << i << "] " << detail::dred_op_name(node->pCommandHistory[i])
                       << (i == completed ? "  <-- LAST/FAULT" : "") << "\n";
                }
            }
        }
        if (node_count == 0u) {
            os << "AutoBreadcrumbs    : Enabled (0 fault nodes recorded)\n";
        }
    } else {
        os << "AutoBreadcrumbs    : Not active (hr=0x" << std::hex << static_cast<unsigned long>(hr_ab) << std::dec << ")\n";
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
    const HRESULT hr_pf = dred->GetPageFaultAllocationOutput(&pf);
    if (SUCCEEDED(hr_pf)) {
        std::snprintf(hex, sizeof(hex), "0x%016llX", static_cast<unsigned long long>(pf.PageFaultVA));
        os << "PageFaultVA        : " << hex << "\n";
        uint32_t alloc_idx = 0u;
        for (const D3D12_DRED_ALLOCATION_NODE* n = pf.pHeadExistingAllocationNode; n && alloc_idx < 8u; n = n->pNext, ++alloc_idx) {
            const std::string name = n->ObjectNameW ? detail::narrow_wstr(n->ObjectNameW)
                                   : (n->ObjectNameA ? std::string(n->ObjectNameA) : "<unnamed>");
            os << "  ExistingAlloc[" << alloc_idx << "]: type=" << static_cast<int>(n->AllocationType)
               << " name='" << name << "'\n";
        }
        alloc_idx = 0u;
        for (const D3D12_DRED_ALLOCATION_NODE* n = pf.pHeadRecentFreedAllocationNode; n && alloc_idx < 8u; n = n->pNext, ++alloc_idx) {
            const std::string name = n->ObjectNameW ? detail::narrow_wstr(n->ObjectNameW)
                                   : (n->ObjectNameA ? std::string(n->ObjectNameA) : "<unnamed>");
            os << "  RecentFreed[" << alloc_idx << "]: type=" << static_cast<int>(n->AllocationType)
               << " name='" << name << "'\n";
        }
    } else {
        os << "PageFaultOutput    : Not active\n";
    }

    out_report = os.str();
    return S_OK;
}

inline HRESULT write_crash_report(
    EXCEPTION_POINTERS* ep,
    const wchar_t* out_dir = nullptr,
    std::wstring* out_txt_path = nullptr,
    std::wstring* out_dmp_path = nullptr,
    ID3D12Device* optional_raw_device = nullptr,
    const char* extra_telemetry = nullptr) noexcept {
    try {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path dir;
        if (out_dir && out_dir[0] != L'\0') {
            dir = out_dir;
        } else {
            const std::string cfg = wsx::get_config_string("WSX_CRASH_DIR", "Diagnostics", "crash_dir", "");
            if (!cfg.empty()) {
                dir = fs::path(cfg);
            } else {
                wchar_t exe[MAX_PATH]{};
                if (GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0u) {
                    dir = fs::path(exe).parent_path() / L"crashes";
                } else {
                    dir = fs::temp_directory_path(ec) / L"wsx_crashes";
                }
            }
        }
        fs::create_directories(dir, ec);

        const std::wstring tag = detail::make_timestamp_tag();
        const fs::path txt_path = dir / (L"wsx_crash_" + tag + L".txt");
        const fs::path dmp_path = dir / (L"wsx_crash_" + tag + L".dmp");

        std::ostringstream os;
        os << "========================================================================\n";
        os << "MizuBox by Mizushi (Enhanced Fork of AnyXboxSeries) — Crash & Diagnostic Report\n";
        os << "========================================================================\n";
        os << "PID=" << GetCurrentProcessId() << " TID=" << GetCurrentThreadId() << "\n";

        char buf[256]{};
        if (ep && ep->ExceptionRecord) {
            const auto* er = ep->ExceptionRecord;
            std::snprintf(buf, sizeof(buf),
                          "ExceptionCode=0x%08lX Flags=0x%08lX Address=0x%016llX Params=%lu\n",
                          static_cast<unsigned long>(er->ExceptionCode),
                          static_cast<unsigned long>(er->ExceptionFlags),
                          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(er->ExceptionAddress)),
                          static_cast<unsigned long>(er->NumberParameters));
            os << buf;
            if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
                std::snprintf(buf, sizeof(buf), "AccessViolation: %s @ VA=0x%016llX\n",
                              er->ExceptionInformation[0] == 1 ? "WRITE" : (er->ExceptionInformation[0] == 8 ? "DEP" : "READ"),
                              static_cast<unsigned long long>(er->ExceptionInformation[1]));
                os << buf;
            }
        }

        if (ep && ep->ContextRecord) {
            const CONTEXT& c = *ep->ContextRecord;
            os << "\n=== X64 REGISTERS ===\n";
            std::snprintf(buf, sizeof(buf), "RIP=%016llX RSP=%016llX RBP=%016llX EFLAGS=%08lX\n",
                          c.Rip, c.Rsp, c.Rbp, static_cast<unsigned long>(c.EFlags));
            os << buf;
            std::snprintf(buf, sizeof(buf), "RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\n",
                          c.Rax, c.Rbx, c.Rcx, c.Rdx);
            os << buf;
            std::snprintf(buf, sizeof(buf), "RSI=%016llX RDI=%016llX R8 =%016llX R9 =%016llX\n",
                          c.Rsi, c.Rdi, c.R8, c.R9);
            os << buf;
            std::snprintf(buf, sizeof(buf), "R10=%016llX R11=%016llX R12=%016llX R13=%016llX\n",
                          c.R10, c.R11, c.R12, c.R13);
            os << buf;
            std::snprintf(buf, sizeof(buf), "R14=%016llX R15=%016llX\n", c.R14, c.R15);
            os << buf;

            // Safely dump up to 16 instruction bytes at RIP if readable
            MEMORY_BASIC_INFORMATION mbi{};
            if (c.Rip != 0 && VirtualQuery(reinterpret_cast<LPCVOID>(c.Rip), &mbi, sizeof(mbi)) == sizeof(mbi) &&
                mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0) {
                os << "Bytes@RIP: ";
                const auto* code = reinterpret_cast<const uint8_t*>(c.Rip);
                for (int i = 0; i < 16; ++i) {
                    std::snprintf(buf, sizeof(buf), "%02X ", code[i]);
                    os << buf;
                }
                os << "\n";
            }
        }

        // Symbolized callstack via dynamic DbgHelp.dll
        os << "\n=== CALLSTACK ===\n";
        HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
        detail::SymInitializeFn pSymInit = nullptr;
        detail::SymFromAddrFn pSymFromAddr = nullptr;
        detail::SymCleanupFn pSymCleanup = nullptr;
        detail::MiniDumpWriteDumpFn pMiniDump = nullptr;
        bool sym_ready = false;
        if (dbghelp) {
            pSymInit = reinterpret_cast<detail::SymInitializeFn>(GetProcAddress(dbghelp, "SymInitialize"));
            pSymFromAddr = reinterpret_cast<detail::SymFromAddrFn>(GetProcAddress(dbghelp, "SymFromAddr"));
            pSymCleanup = reinterpret_cast<detail::SymCleanupFn>(GetProcAddress(dbghelp, "SymCleanup"));
            pMiniDump = reinterpret_cast<detail::MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
            if (pSymInit && pSymFromAddr) {
                sym_ready = (pSymInit(GetCurrentProcess(), nullptr, TRUE) != FALSE);
            }
        }

        void* frames[32]{};
        const USHORT nframes = CaptureStackBackTrace(0u, 32u, frames, nullptr);
        for (USHORT i = 0; i < nframes; ++i) {
            const uintptr_t addr = reinterpret_cast<uintptr_t>(frames[i]);
            HMODULE mod = nullptr;
            char mod_name[MAX_PATH] = "<unknown>";
            uintptr_t mod_base = 0;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(addr), &mod) && mod) {
                mod_base = reinterpret_cast<uintptr_t>(mod);
                GetModuleBaseNameA(GetCurrentProcess(), mod, mod_name, MAX_PATH);
            }
            std::string sym_str;
            if (sym_ready && pSymFromAddr) {
                detail::WsxSymbolInfo si{};
                si.SizeOfStruct = offsetof(detail::WsxSymbolInfo, Name);
                si.MaxNameLen = 255u;
                DWORD64 disp = 0;
                if (pSymFromAddr(GetCurrentProcess(), static_cast<DWORD64>(addr), &disp, &si)) {
                    std::snprintf(buf, sizeof(buf), " %s+0x%llX", si.Name, static_cast<unsigned long long>(disp));
                    sym_str = buf;
                }
            }
            std::snprintf(buf, sizeof(buf), "  [%2u] 0x%016llX %s+0x%llX%s\n",
                          static_cast<unsigned>(i),
                          static_cast<unsigned long long>(addr),
                          mod_name,
                          static_cast<unsigned long long>(mod_base ? (addr - mod_base) : 0u),
                          sym_str.c_str());
            os << buf;
        }
        if (sym_ready && pSymCleanup) {
            pSymCleanup(GetCurrentProcess());
        }

        // Loaded modules table
        os << "\n=== LOADED MODULES ===\n";
        HMODULE mods[256]{};
        DWORD cb_needed = 0;
        if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &cb_needed)) {
            const DWORD count = (std::min<DWORD>)(cb_needed / sizeof(HMODULE), 256u);
            for (DWORD i = 0; i < count; ++i) {
                MODULEINFO mi{};
                char path_a[MAX_PATH]{};
                GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi));
                GetModuleFileNameExA(GetCurrentProcess(), mods[i], path_a, MAX_PATH);
                std::snprintf(buf, sizeof(buf), "  0x%016llX [size=0x%08lX] %s\n",
                              static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mi.lpBaseOfDll)),
                              static_cast<unsigned long>(mi.SizeOfImage),
                              path_a);
                os << buf;
            }
        }

        if (extra_telemetry && extra_telemetry[0] != '\0') {
            os << "\n=== D3D12 TELEMETRY ===\n" << extra_telemetry << "\n";
        }

        if (optional_raw_device) {
            std::string dred_txt;
            if (SUCCEEDED(capture_dred_report(optional_raw_device, dred_txt))) {
                os << "\n" << dred_txt;
            }
        }

        // Write text report
        {
            std::ofstream out(txt_path, std::ios::binary | std::ios::trunc);
            const std::string body = os.str();
            out.write(body.data(), static_cast<std::streamsize>(body.size()));
        }
        if (out_txt_path) *out_txt_path = txt_path.wstring();

        // Write WinDbg minidump (.dmp)
        if (pMiniDump) {
            HANDLE f = CreateFileW(dmp_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                detail::WsxMiniDumpExceptionInfo mei{};
                mei.ThreadId = GetCurrentThreadId();
                mei.ExceptionPointers = ep;
                mei.ClientPointers = FALSE;
                // MiniDumpWithDataSegs (0x1) | MiniDumpWithThreadInfo (0x1000)
                const BOOL ok = pMiniDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                                          0x00001001u, ep ? &mei : nullptr, nullptr, nullptr);
                CloseHandle(f);
                if (ok && out_dmp_path) {
                    *out_dmp_path = dmp_path.wstring();
                }
            }
        }

        if (dbghelp) FreeLibrary(dbghelp);
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

namespace detail {
inline std::atomic<bool>& crash_handler_installed() noexcept {
    static std::atomic<bool> installed{false};
    return installed;
}
inline std::wstring& crash_dump_dir() {
    static std::wstring dir;
    return dir;
}
inline LONG WINAPI wsx_unhandled_exception_filter(EXCEPTION_POINTERS* ep) noexcept {
    std::wstring txt, dmp;
    write_crash_report(ep, crash_dump_dir().empty() ? nullptr : crash_dump_dir().c_str(), &txt, &dmp);
    if (!txt.empty()) {
        std::fprintf(stderr, "\n[MizuBox] Crash report written: %ls\n", txt.c_str());
    }
    if (!dmp.empty()) {
        std::fprintf(stderr, "[MizuBox] Minidump written    : %ls\n", dmp.c_str());
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace detail

inline void install_crash_handler(const wchar_t* dump_dir = nullptr) noexcept {
    if (dump_dir && dump_dir[0] != L'\0') {
        detail::crash_dump_dir() = dump_dir;
    }
    if (!detail::crash_handler_installed().exchange(true, std::memory_order_acq_rel)) {
        SetUnhandledExceptionFilter(&detail::wsx_unhandled_exception_filter);
    }
}

} // namespace wsx_diag
