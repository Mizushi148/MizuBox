// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// PE64 fixture compiled with real __declspec(thread) static .tls RawData + ZeroFill,
// IMAGE_TLS_CALLBACK, CreateThread worker thread, and guest atexit() handlers.

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>

// Force linker to keep .tls directory and TLS callback table
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:wsx_tls_cb_entry")

namespace {

// Real initialized static TLS variable (.tls RawData) + zero-initialized TLS array (SizeOfZeroFill)
__declspec(thread) uint32_t g_tls_magic = 0xCAFEBABEu;
__declspec(thread) uint32_t g_tls_zero[16];

std::atomic<uint32_t> g_cb_proc_attach{0};
std::atomic<uint32_t> g_cb_thread_attach{0};
std::atomic<uint32_t> g_cb_thread_detach{0};
std::atomic<uint32_t> g_cb_proc_detach{0};

uint32_t* g_host_atexit_log = nullptr;
uint32_t* g_host_atexit_count = nullptr;

void __cdecl guest_atexit_first() {
    if (g_host_atexit_log && g_host_atexit_count) {
        const uint32_t idx = (*g_host_atexit_count)++;
        if (idx < 8u) g_host_atexit_log[idx] = 0xA1u;
    }
}

void __cdecl guest_atexit_second() {
    if (g_host_atexit_log && g_host_atexit_count) {
        const uint32_t idx = (*g_host_atexit_count)++;
        if (idx < 8u) g_host_atexit_log[idx] = 0xB2u;
    }
}

void NTAPI wsx_tls_callback(PVOID /*DllHandle*/, DWORD Reason, PVOID /*Reserved*/) {
    if (Reason == DLL_PROCESS_ATTACH) {
        g_cb_proc_attach.fetch_add(1, std::memory_order_relaxed);
    } else if (Reason == DLL_THREAD_ATTACH) {
        g_cb_thread_attach.fetch_add(1, std::memory_order_relaxed);
    } else if (Reason == DLL_THREAD_DETACH) {
        g_cb_thread_detach.fetch_add(1, std::memory_order_relaxed);
    } else if (Reason == DLL_PROCESS_DETACH) {
        g_cb_proc_detach.fetch_add(1, std::memory_order_relaxed);
    }
}

struct WorkerResult {
    uint32_t initial_magic = 0;
    uint32_t initial_zero = 0xFFFFFFFFu;
    uint32_t mutated_magic = 0;
};

DWORD WINAPI worker_thread_fn(LPVOID param) {
    auto* out = static_cast<WorkerResult*>(param);
    out->initial_magic = g_tls_magic;
    out->initial_zero = g_tls_zero[7];
    g_tls_magic = 0x33334444u;
    g_tls_zero[7] = 0x77778888u;
    out->mutated_magic = g_tls_magic;
    return 0;
}

} // namespace

#pragma const_seg(".CRT$XLB")
extern "C" const PIMAGE_TLS_CALLBACK wsx_tls_cb_entry = wsx_tls_callback;
#pragma const_seg()

struct WsxTlsFixtureReport {
    uint32_t main_initial_magic;
    uint32_t main_initial_zero;
    uint32_t main_after_worker_magic;
    uint32_t worker_initial_magic;
    uint32_t worker_initial_zero;
    uint32_t worker_mutated_magic;
    uint32_t cb_proc_attach;
    uint32_t cb_thread_attach;
    uint32_t cb_thread_detach;
};

extern "C" uint32_t __cdecl run_tls_and_atexit_test(
    WsxTlsFixtureReport* out_rep,
    uint32_t* host_atexit_log,
    uint32_t* host_atexit_count) {
    if (!out_rep) return 0u;
    out_rep->main_initial_magic = g_tls_magic;
    out_rep->main_initial_zero = g_tls_zero[7];

    // Mutate main thread's static TLS before spawning worker thread
    g_tls_magic = 0x11112222u;
    g_tls_zero[7] = 0x55556666u;

    WorkerResult wres{};
    HANDLE th = CreateThread(nullptr, 0, &worker_thread_fn, &wres, 0, nullptr);
    if (th) {
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
    }

    out_rep->main_after_worker_magic = g_tls_magic;
    out_rep->worker_initial_magic = wres.initial_magic;
    out_rep->worker_initial_zero = wres.initial_zero;
    out_rep->worker_mutated_magic = wres.mutated_magic;
    out_rep->cb_proc_attach = g_cb_proc_attach.load(std::memory_order_relaxed);
    out_rep->cb_thread_attach = g_cb_thread_attach.load(std::memory_order_relaxed);
    out_rep->cb_thread_detach = g_cb_thread_detach.load(std::memory_order_relaxed);

    // Register two atexit handlers to verify isolated LIFO execution on guest unload
    g_host_atexit_log = host_atexit_log;
    g_host_atexit_count = host_atexit_count;
    std::atexit(&guest_atexit_first);
    std::atexit(&guest_atexit_second);
    return 1u;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
