// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0

#include "xmem.h"

#include <Windows.h>

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

struct XMemApi {
    decltype(&WsxXMemAlloc) alloc = nullptr;
    decltype(&WsxXMemFree) free_fn = nullptr;
    decltype(&WsxXMemAllocDefault) alloc_default = nullptr;
    decltype(&WsxXMemFreeDefault) free_default = nullptr;
    decltype(&WsxXMemSize) size_fn = nullptr;
    decltype(&WsxXMemSetAllocationHooks) set_hooks = nullptr;
    decltype(&WsxXMemVirtualQuery) virtual_query = nullptr;
    decltype(&WsxXMemGetWorkingSetStatistics) working_set = nullptr;
};

XMemApi g_api{};
std::atomic<int> g_hook_allocs{0};
std::atomic<int> g_hook_frees{0};

PVOID WINAPI test_alloc_hook(SIZE_T size, ULONGLONG attrs) {
    g_hook_allocs.fetch_add(1, std::memory_order_relaxed);
    return g_api.alloc_default(size, attrs);
}

VOID WINAPI test_free_hook(PVOID ptr, ULONGLONG attrs) {
    g_hook_frees.fetch_add(1, std::memory_order_relaxed);
    g_api.free_default(ptr, attrs);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: wsx_xmem_test <absolute path to xmem.dll>\n");
        return 2;
    }

    const HMODULE mod = LoadLibraryExW(argv[1], nullptr,
                                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mod) {
        std::fprintf(stderr, "LoadLibraryExW failed with Win32 error %lu\n", GetLastError());
        return 1;
    }

    g_api.alloc = std::bit_cast<decltype(g_api.alloc)>(GetProcAddress(mod, "XMemAlloc"));
    g_api.free_fn = std::bit_cast<decltype(g_api.free_fn)>(GetProcAddress(mod, "XMemFree"));
    g_api.alloc_default = std::bit_cast<decltype(g_api.alloc_default)>(GetProcAddress(mod, "XMemAllocDefault"));
    g_api.free_default = std::bit_cast<decltype(g_api.free_default)>(GetProcAddress(mod, "XMemFreeDefault"));
    g_api.size_fn = std::bit_cast<decltype(g_api.size_fn)>(GetProcAddress(mod, "XMemSize"));
    g_api.set_hooks = std::bit_cast<decltype(g_api.set_hooks)>(GetProcAddress(mod, "XMemSetAllocationHooks"));
    g_api.virtual_query = std::bit_cast<decltype(g_api.virtual_query)>(GetProcAddress(mod, "XMemVirtualQuery"));
    g_api.working_set = std::bit_cast<decltype(g_api.working_set)>(GetProcAddress(mod, "XMemGetWorkingSetStatistics"));

    if (!g_api.alloc || !g_api.free_fn || !g_api.alloc_default || !g_api.free_default ||
        !g_api.size_fn || !g_api.set_hooks || !g_api.virtual_query || !g_api.working_set) {
        std::fprintf(stderr, "One or more expected xmem.dll exports are missing\n");
        return 1;
    }

    // 1. Null error handling
    if (g_api.virtual_query(GetCurrentProcess(), nullptr, nullptr) != FALSE) {
        std::fprintf(stderr, "XMemVirtualQuery must reject null output buffer\n");
        return 1;
    }
    if (g_api.working_set(0x20000001u, nullptr) != FALSE) {
        std::fprintf(stderr, "XMemGetWorkingSetStatistics must reject null output buffer\n");
        return 1;
    }
    g_api.free_fn(nullptr, 0);

    // 2. Working set baseline
    XMEM_WORKING_SET_STATISTICS before{};
    if (!g_api.working_set(0x20000001u, &before) || before.WorkingSetBytes == 0) {
        std::fprintf(stderr, "XMemGetWorkingSetStatistics baseline failed\n");
        return 1;
    }

    // 3. Allocate 4 MiB, verify 64 KiB alignment, zero-init, read/write, XMemSize, XMemVirtualQuery
    constexpr SIZE_T kTestSize = 4u * 1024u * 1024u;
    void* buf = g_api.alloc(kTestSize, 0x12345678ULL);
    if (!buf) {
        std::fprintf(stderr, "XMemAlloc(4 MiB) returned null\n");
        return 1;
    }
    if ((reinterpret_cast<std::uintptr_t>(buf) & 0xFFFFu) != 0) {
        std::fprintf(stderr, "XMemAlloc buffer is not 64 KiB aligned: %p\n", buf);
        return 1;
    }
    auto* bytes = static_cast<unsigned char*>(buf);
    if (bytes[0] != 0 || bytes[kTestSize - 1] != 0) {
        std::fprintf(stderr, "XMemAlloc buffer is not zero-initialized\n");
        return 1;
    }
    std::memset(bytes, 0x5A, kTestSize);

    if (g_api.size_fn(buf, 0) != kTestSize) {
        std::fprintf(stderr, "XMemSize mismatch: got %zu expected %zu\n",
                     g_api.size_fn(buf, 0), kTestSize);
        return 1;
    }

    // Verify exact fields checked by XboxBvhBuilder.dll!
    XMEM_MEMORY_BASIC_INFORMATION mbi{};
    if (!g_api.virtual_query(GetCurrentProcess(), bytes + 4096, &mbi)) {
        std::fprintf(stderr, "XMemVirtualQuery failed on live allocation\n");
        return 1;
    }
    if ((mbi.State & MEM_COMMIT) == 0 || (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE)) == 0 ||
        (mbi.MemoryAttributes & 0x03u) != 0x03u || mbi.PageSize != 65536u) {
        std::fprintf(stderr,
                     "XMemVirtualQuery unexpected fields: State=0x%lX Protect=0x%lX Attrs=0x%lX Page=%lu\n",
                     mbi.State, mbi.Protect, mbi.MemoryAttributes, mbi.PageSize);
        return 1;
    }

    // 4. Verify working set delta reflects the 4 MiB allocation!
    XMEM_WORKING_SET_STATISTICS after{};
    if (!g_api.working_set(0x20000001u, &after) ||
        after.WorkingSetBytes < before.WorkingSetBytes + kTestSize ||
        after.XMemTrackedBytes < kTestSize ||
        after.XMemAllocationCount != 1) {
        std::fprintf(stderr,
                     "WorkingSet delta check failed: before=%llu after=%llu tracked=%llu count=%llu\n",
                     before.WorkingSetBytes, after.WorkingSetBytes,
                     after.XMemTrackedBytes, after.XMemAllocationCount);
        return 1;
    }

    // 5. Free and verify released state
    g_api.free_fn(buf, 0x12345678ULL);
    XMEM_MEMORY_BASIC_INFORMATION freed_mbi{};
    if (g_api.virtual_query(GetCurrentProcess(), buf, &freed_mbi) &&
        (freed_mbi.State & MEM_COMMIT) != 0) {
        std::fprintf(stderr, "Buffer still appears MEM_COMMIT after XMemFree\n");
        return 1;
    }
    XMEM_WORKING_SET_STATISTICS post_free{};
    if (!g_api.working_set(0x20000001u, &post_free) || post_free.XMemAllocationCount != 0) {
        std::fprintf(stderr, "Allocation count did not return to 0 after XMemFree\n");
        return 1;
    }

    // 6. Allocation hooks test
    g_api.set_hooks(&test_alloc_hook, &test_free_hook);
    void* hooked = g_api.alloc(1024, 0);
    g_api.free_fn(hooked, 0);
    g_api.set_hooks(nullptr, nullptr);
    if (!hooked || g_hook_allocs.load() != 1 || g_hook_frees.load() != 1) {
        std::fprintf(stderr, "XMemSetAllocationHooks failed\n");
        return 1;
    }

    // 7. Multithreaded stress test
    std::vector<std::thread> threads;
    std::atomic<bool> mt_ok{true};
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 64; ++i) {
                void* p = g_api.alloc(4096u * static_cast<SIZE_T>((i % 4) + 1), 0);
                if (!p) {
                    mt_ok.store(false);
                    return;
                }
                static_cast<unsigned char*>(p)[0] = 0xAA;
                g_api.free_fn(p, 0);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    if (!mt_ok.load()) {
        std::fprintf(stderr, "Multithreaded XMemAlloc/XMemFree stress test failed\n");
        return 1;
    }

    std::puts("xmem.dll tests passed: alignment, zero-init, VirtualQuery, WorkingSet delta, hooks, MT stress.");
    return 0;
}
