// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Synthetic x86-64 PE32+ fixture DLL used by wsx_pe_loader_test to verify:
// - Base relocations (DIR64 global pointers)
// - .pdata SEH / C++ try-catch unwinding via RtlAddFunctionTable
// - TLS callback execution on DLL_PROCESS_ATTACH
// - Static IAT binding to MizuBox shim xmem.dll
// - Dynamic LoadLibraryW("xgameruntime.dll") redirection away from C:\Windows\System32
// - Named & ordinal (@7) exports

#include "xmem.h"

extern "C" {
PVOID WINAPI XMemAlloc(SIZE_T size, ULONGLONG attributes);
VOID WINAPI XMemFree(PVOID address, ULONGLONG attributes);
}

#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace {

volatile LONG g_tls_attach_count = 0;
volatile LONG g_dll_attach_count = 0;

void NTAPI fixture_tls_callback(PVOID /*h*/, DWORD reason, PVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        InterlockedIncrement(&g_tls_attach_count);
    }
}

#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:wsx_fixture_tls_cb_ptr")
#pragma const_seg(".CRT$XLB")
extern "C" const PIMAGE_TLS_CALLBACK wsx_fixture_tls_cb_ptr = fixture_tls_callback;
#pragma const_seg()

// Global pointer array forces DIR64 base relocations when image moves from Preferred ImageBase
const char* g_reloc_strings[2] = {"WSX_RELOC_OK_A", "WSX_RELOC_OK_B"};

__declspec(noinline) void throw_value(int v) {
    throw v;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE /*hinst*/, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        InterlockedIncrement(&g_dll_attach_count);
    }
    return TRUE;
}

extern "C" {

int __cdecl wsx_fixture_run_checks(wchar_t* out_xgr_path, uint32_t path_cap) {
    if (g_tls_attach_count < 1 || g_dll_attach_count < 1) return -1;

    // 1. Verify DIR64-relocated global string pointers
    if (std::strcmp(g_reloc_strings[0], "WSX_RELOC_OK_A") != 0 ||
        std::strcmp(g_reloc_strings[1], "WSX_RELOC_OK_B") != 0) {
        return -2;
    }

    // 2. Verify C++ exception unwinding (.pdata RtlAddFunctionTable)
    int caught = 0;
    try {
        throw_value(1337);
    } catch (int v) {
        caught = v;
    }
    if (caught != 1337) return -3;

    // 3. Verify static IAT import into MizuBox xmem.dll
    void* mem = XMemAlloc(4096, 0);
    if (!mem) return -4;
    std::memset(mem, 0x5A, 4096);
    XMemFree(mem, 0);

    // 4. Verify dynamic LoadLibraryW("xgameruntime.dll") redirects to MizuBox shim (NOT System32!)
    HMODULE xgr = LoadLibraryW(L"xgameruntime.dll");
    if (!xgr) return -5;
    if (out_xgr_path && path_cap > 0) {
        out_xgr_path[0] = L'\0';
        GetModuleFileNameW(xgr, out_xgr_path, path_cap);
    }
    return 1337;
}

int __cdecl wsx_fixture_ordinal_fn(int x) {
    return x * 3 + 7;
}

} // extern "C"
