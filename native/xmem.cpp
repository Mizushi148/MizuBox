// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Windows x64 implementation of Xbox Series X|S memory APIs (xmem.dll).

#include "xmem.h"
#include "wsx_config.h"

#include <Psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <unordered_map>
#include <vector>

#if !defined(_M_X64)
#error The Xbox xmem shim requires AMD64.
#endif

namespace {

constexpr SIZE_T kPageGranularity = 65536; // 64 KiB alignment covers CPU and GPU buffer requirements
constexpr size_t kNumPoolBuckets = 6;      // 64K, 128K, 256K, 512K, 1M, 2M
constexpr SIZE_T kPoolBucketSizes[kNumPoolBuckets] = {
    64u * 1024u,
    128u * 1024u,
    256u * 1024u,
    512u * 1024u,
    1024u * 1024u,
    2048u * 1024u,
};

inline int pool_bucket_for_size(SIZE_T commit_size) noexcept {
    for (size_t i = 0; i < kNumPoolBuckets; ++i) {
        if (commit_size == kPoolBucketSizes[i]) return static_cast<int>(i);
    }
    return -1;
}

struct PooledBlock {
    void* ptr = nullptr;
    UINT64 physical_address = 0;
};

struct AllocationRecord {
    void* raw_base = nullptr;
    SIZE_T requested_size = 0;
    SIZE_T committed_size = 0;
    SIZE_T alignment = kPageGranularity;
    ULONGLONG attributes = 0;
    UINT64 physical_address = 0;
    bool protection_modified = false;
    bool is_graphics = false;
};

struct PhysicalMappingRecord {
    void* raw_base = nullptr;
    uintptr_t aligned_va = 0;
    SIZE_T size = 0;
    ULONG_PTR alignment = 0;
    DWORD protect = PAGE_READWRITE;
    UINT64 physical_address = 0;
};

struct GpuAliasInterval {
    UINT64 cpu_base = 0;
    SIZE_T size = 0;
    UINT64 desktop_gpu_va = 0;
};

struct XMemState {
    std::mutex mutex;
    std::unordered_map<void*, AllocationRecord> allocations;
    std::unordered_map<void*, PhysicalMappingRecord> physical_mappings;
    std::unordered_map<UINT64, GpuAliasInterval> gpu_aliases;
    std::array<std::vector<PooledBlock>, kNumPoolBuckets> free_pools;
    UINT64 next_physical_address = 0x0000000100000000ULL;
    UINT64 max_pool_bytes = 64ULL * 1024ULL * 1024ULL;
    std::atomic<UINT64> tracked_bytes{0};
    std::atomic<UINT64> peak_tracked_bytes{0};
    std::atomic<UINT64> allocation_count{0};
    std::atomic<UINT64> physical_mapped_bytes{0};
    std::atomic<UINT64> physical_mapping_count{0};
    std::atomic<UINT64> pool_hits{0};
    std::atomic<UINT64> pool_misses{0};
    std::atomic<UINT64> pooled_block_count{0};
    std::atomic<UINT64> pooled_bytes{0};
    std::atomic<UINT64> trimmed_blocks{0};
    std::atomic<UINT64> trimmed_bytes{0};
    std::atomic<XMemAllocRoutine> alloc_hook{nullptr};
    std::atomic<XMemFreeRoutine> free_hook{nullptr};

    XMemState() {
        wchar_t buf[32]{};
        const DWORD len = GetEnvironmentVariableW(L"WSX_XMEM_POOL_MAX_MB", buf, static_cast<DWORD>(std::size(buf)));
        if (len > 0 && len < std::size(buf)) {
            const unsigned long mb = std::wcstoul(buf, nullptr, 10);
            max_pool_bytes = static_cast<UINT64>(mb) * 1024ULL * 1024ULL;
        }
    }
};

void update_peak_tracked(XMemState& st, UINT64 current_total) noexcept {
    UINT64 prev = st.peak_tracked_bytes.load(std::memory_order_relaxed);
    while (current_total > prev &&
           !st.peak_tracked_bytes.compare_exchange_weak(prev, current_total, std::memory_order_relaxed)) {
    }
}

UINT64 allocate_guest_pa_locked(XMemState& st, SIZE_T span_bytes, ULONG_PTR align_bytes) noexcept {
    const UINT64 align = (align_bytes < kPageGranularity) ? static_cast<UINT64>(kPageGranularity)
                                                          : static_cast<UINT64>(align_bytes);
    const UINT64 pa = (st.next_physical_address + (align - 1u)) & ~(align - 1u);
    const UINT64 step = (static_cast<UINT64>(span_bytes) + (kPageGranularity - 1u)) &
                        ~(static_cast<UINT64>(kPageGranularity) - 1u);
    st.next_physical_address = pa + (step > 0u ? step : kPageGranularity);
    return pa;
}

XMemState& state() noexcept {
    static XMemState instance;
    return instance;
}

bool logging_enabled() noexcept {
    static const bool enabled = []() noexcept {
        wchar_t buf[8]{};
        const DWORD len = GetEnvironmentVariableW(L"WSX_XMEM_LOG", buf, static_cast<DWORD>(std::size(buf)));
        return len > 0 && buf[0] == L'1';
    }();
    return enabled;
}

void write_log(const char* message) noexcept {
    if (!logging_enabled() || !message) {
        return;
    }
    std::array<wchar_t, 32768> path{};
    constexpr wchar_t log_name[] = L"xmem_log.txt";
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return;
    }
    wchar_t* filename = std::wcsrchr(path.data(), L'\\');
    if (!filename || filename + 1 > path.data() + path.size() - std::size(log_name)) {
        return;
    }
    std::copy_n(log_name, std::size(log_name), filename + 1);

    const HANDLE file = CreateFileW(path.data(), FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, message, static_cast<DWORD>(std::strlen(message)), &written, nullptr);
        CloseHandle(file);
    }
}

SIZE_T align_up_page(SIZE_T size) noexcept {
    if (size == 0) {
        return kPageGranularity;
    }
    const SIZE_T remainder = size % kPageGranularity;
    if (remainder == 0) {
        return size;
    }
    if (size > (static_cast<SIZE_T>(-1) - kPageGranularity)) {
        return 0;
    }
    return size + (kPageGranularity - remainder);
}

DWORD compute_memory_attributes(const MEMORY_BASIC_INFORMATION& mbi) noexcept {
    if (mbi.State != MEM_COMMIT) {
        return 0;
    }
    const DWORD base_protect = mbi.Protect & 0xFFu;
    DWORD attrs = 0;
    switch (base_protect) {
    case PAGE_READONLY:
    case PAGE_EXECUTE_READ:
        attrs = 0x01u; // readable / GPU-read
        break;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        attrs = 0x03u; // readable + writable / GPU-read+write
        break;
    default:
        attrs = 0;
        break;
    }
    return attrs;
}

void unregister_gpu_aliases_locked(XMemState& st, UINT64 base, SIZE_T span) noexcept {
    st.gpu_aliases.erase(base);
    if (span == 0) {
        return;
    }
    for (auto it = st.gpu_aliases.begin(); it != st.gpu_aliases.end();) {
        const UINT64 alias_base = it->second.cpu_base;
        if (alias_base >= base && (alias_base - base) < span) {
            it = st.gpu_aliases.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace

extern "C" {

BOOL WINAPI WsxXMemDecodeAttributes(ULONGLONG attributes, XMEM_DECODED_ATTRIBUTES* outDecoded) {
    if (!outDecoded) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::memset(outDecoded, 0, sizeof(*outDecoded));

    const UINT16 object_type = static_cast<UINT16>(attributes & 0x3FFFu);
    const UINT8 page_code = static_cast<UINT8>((attributes >> 14) & 0x3u);
    const UINT8 allocator_id = static_cast<UINT8>((attributes >> 16) & 0xFFu);
    const UINT8 raw_align_log2 = static_cast<UINT8>((attributes >> 24) & 0x1Fu);
    const UINT8 clamped_align_log2 = (raw_align_log2 < 4u) ? 4u : raw_align_log2;
    const UINT8 mem_type = static_cast<UINT8>((attributes >> 29) & 0xFu);
    const UINT64 reserved_bits = (attributes >> 33);

    UINT64 page_bytes = 0;
    if (page_code == 0u) {
        page_bytes = 4096ULL;
    } else if (page_code == 1u) {
        page_bytes = 65536ULL;
    } else if (page_code == 2u) {
        page_bytes = 4194304ULL;
    }

    const bool is_graphics = (mem_type >= 1u && mem_type <= 6u);
    const bool is_valid = (reserved_bits == 0ULL && page_code != 3u && mem_type <= 9u);

    outDecoded->ObjectType = object_type;
    outDecoded->PageSizeCode = page_code;
    outDecoded->AllocatorId = allocator_id;
    outDecoded->AlignmentLog2 = clamped_align_log2;
    outDecoded->MemoryType = mem_type;
    outDecoded->IsGraphicsMemory = is_graphics ? 1u : 0u;
    outDecoded->IsValid = is_valid ? 1u : 0u;
    outDecoded->PageSizeBytes = page_bytes;
    outDecoded->AlignmentBytes = 1ULL << clamped_align_log2;
    outDecoded->ReservedBits = reserved_bits;
    return TRUE;
}

PVOID WINAPI WsxXMemAllocDefault(SIZE_T size, ULONGLONG attributes) {
    const SIZE_T commit_size = align_up_page(size);
    if (commit_size == 0) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }

    XMEM_DECODED_ATTRIBUTES dec{};
    WsxXMemDecodeAttributes(attributes, &dec);
    const bool is_graphics = (dec.IsGraphicsMemory != 0u);
    SIZE_T align_bytes = kPageGranularity;
    if (dec.IsValid != 0u &&
        dec.AlignmentBytes > kPageGranularity &&
        dec.AlignmentBytes <= (16ULL * 1024ULL * 1024ULL)) {
        align_bytes = static_cast<SIZE_T>(dec.AlignmentBytes);
    }

    auto& st = state();
    const int bucket = (align_bytes <= kPageGranularity) ? pool_bucket_for_size(commit_size) : -1;
    void* ptr = nullptr;
    void* raw_base = nullptr;
    UINT64 pa = 0;
    bool from_pool = false;

    if (bucket >= 0 && st.max_pool_bytes > 0u) {
        std::lock_guard<std::mutex> lock(st.mutex);
        auto& vec = st.free_pools[static_cast<size_t>(bucket)];
        if (!vec.empty()) {
            const PooledBlock blk = vec.back();
            vec.pop_back();
            ptr = blk.ptr;
            raw_base = blk.ptr;
            pa = blk.physical_address;
            from_pool = true;
            st.allocations[ptr] = AllocationRecord{
                raw_base, size, commit_size, align_bytes, attributes, pa, false, is_graphics
            };
        }
    }

    if (from_pool) {
        st.pool_hits.fetch_add(1u, std::memory_order_relaxed);
        st.pooled_block_count.fetch_sub(1u, std::memory_order_relaxed);
        st.pooled_bytes.fetch_sub(commit_size, std::memory_order_relaxed);
        std::memset(ptr, 0, commit_size); // Preserve Xbox VirtualAlloc zero-initialized guarantee
    } else {
        if (bucket >= 0) {
            st.pool_misses.fetch_add(1u, std::memory_order_relaxed);
        }
        const SIZE_T extra = (align_bytes > kPageGranularity) ? align_bytes : 0u;
        if (commit_size > (static_cast<SIZE_T>(-1) - extra)) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        // VirtualAlloc guarantees 64 KiB base alignment and zero-initialized committed pages.
        raw_base = VirtualAlloc(nullptr, commit_size + extra, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!raw_base) {
            if (logging_enabled()) {
                char line[160]{};
                std::snprintf(line, sizeof(line),
                              "XMemAllocDefault FAILED size=%zu attrs=0x%016llX err=%lu\r\n",
                              size, static_cast<unsigned long long>(attributes), GetLastError());
                write_log(line);
            }
            return nullptr;
        }
        if (extra > 0u) {
            const uintptr_t raw_addr = reinterpret_cast<uintptr_t>(raw_base);
            const uintptr_t aligned_va =
                (raw_addr + (align_bytes - 1u)) & ~static_cast<uintptr_t>(align_bytes - 1u);
            ptr = reinterpret_cast<void*>(aligned_va);
        } else {
            ptr = raw_base;
        }
        std::lock_guard<std::mutex> lock(st.mutex);
        pa = allocate_guest_pa_locked(st, commit_size, align_bytes);
        st.allocations[ptr] = AllocationRecord{
            raw_base, size, commit_size, align_bytes, attributes, pa, false, is_graphics
        };
    }

    const UINT64 cur_tracked = st.tracked_bytes.fetch_add(commit_size, std::memory_order_relaxed) + commit_size;
    st.allocation_count.fetch_add(1, std::memory_order_relaxed);
    update_peak_tracked(st, cur_tracked + st.physical_mapped_bytes.load(std::memory_order_relaxed));

    if (logging_enabled()) {
        char line[160]{};
        std::snprintf(line, sizeof(line),
                      "XMemAllocDefault size=%zu commit=%zu pool=%d attrs=0x%016llX -> %p\r\n",
                      size, commit_size, from_pool ? 1 : 0, static_cast<unsigned long long>(attributes), ptr);
        write_log(line);
    }
    return ptr;
}

VOID WINAPI WsxXMemFreeDefault(PVOID address, ULONGLONG attributes) {
    if (!address) {
        return;
    }
    AllocationRecord rec{};
    bool found = false;
    bool pooled = false;
    auto& st = state();
    {
        std::lock_guard<std::mutex> lock(st.mutex);
        const auto it = st.allocations.find(address);
        if (it != st.allocations.end()) {
            rec = it->second;
            st.allocations.erase(it);
            found = true;

            unregister_gpu_aliases_locked(st, reinterpret_cast<UINT64>(address), rec.committed_size);

            const int bucket = (rec.alignment <= kPageGranularity && rec.raw_base == address)
                ? pool_bucket_for_size(rec.committed_size)
                : -1;
            if (bucket >= 0 && !rec.protection_modified && st.max_pool_bytes > 0u) {
                const UINT64 cur_pooled = st.pooled_bytes.load(std::memory_order_relaxed);
                if (cur_pooled + rec.committed_size <= st.max_pool_bytes) {
                    st.free_pools[static_cast<size_t>(bucket)].push_back(PooledBlock{address, rec.physical_address});
                    st.pooled_block_count.fetch_add(1u, std::memory_order_relaxed);
                    st.pooled_bytes.fetch_add(rec.committed_size, std::memory_order_relaxed);
                    pooled = true;
                }
            }
        } else {
            unregister_gpu_aliases_locked(st, reinterpret_cast<UINT64>(address), 0);
        }
    }
    if (found) {
        st.tracked_bytes.fetch_sub(rec.committed_size, std::memory_order_relaxed);
        st.allocation_count.fetch_sub(1, std::memory_order_relaxed);
    }
    if (!pooled) {
        VirtualFree((found && rec.raw_base) ? rec.raw_base : address, 0, MEM_RELEASE);
    }

    if (logging_enabled()) {
        char line[160]{};
        std::snprintf(line, sizeof(line),
                      "XMemFreeDefault ptr=%p attrs=0x%016llX tracked=%d pooled=%d\r\n",
                      address, static_cast<unsigned long long>(attributes), found ? 1 : 0, pooled ? 1 : 0);
        write_log(line);
    }
}

PVOID WINAPI WsxXMemAlloc(SIZE_T size, ULONGLONG attributes) {
    if (const auto hook = state().alloc_hook.load(std::memory_order_acquire)) {
        return hook(size, attributes);
    }
    return WsxXMemAllocDefault(size, attributes);
}

VOID WINAPI WsxXMemFree(PVOID address, ULONGLONG attributes) {
    if (!address) {
        return;
    }
    if (const auto hook = state().free_hook.load(std::memory_order_acquire)) {
        hook(address, attributes);
        return;
    }
    WsxXMemFreeDefault(address, attributes);
}

SIZE_T WINAPI WsxXMemSizeDefault(PVOID address, ULONGLONG /*attributes*/) {
    if (!address) {
        return 0;
    }
    auto& st = state();
    {
        std::lock_guard<std::mutex> lock(st.mutex);
        const auto it = st.allocations.find(address);
        if (it != st.allocations.end()) {
            return it->second.requested_size;
        }
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT) {
        return mbi.RegionSize;
    }
    return 0;
}

SIZE_T WINAPI WsxXMemSize(PVOID address, ULONGLONG attributes) {
    return WsxXMemSizeDefault(address, attributes);
}

VOID WINAPI WsxXMemSetAllocationHooks(XMemAllocRoutine alloc_hook, XMemFreeRoutine free_hook) {
    auto& st = state();
    st.alloc_hook.store(alloc_hook, std::memory_order_release);
    st.free_hook.store(free_hook, std::memory_order_release);
}

BOOL WINAPI WsxXMemVirtualQuery(HANDLE process, LPCVOID address, XMEM_MEMORY_BASIC_INFORMATION* info) {
    if (!info) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::memset(info, 0, sizeof(*info));
    const HANDLE target = process ? process : GetCurrentProcess();

    MEMORY_BASIC_INFORMATION mbi{};
    const SIZE_T queried = VirtualQueryEx(target, address, &mbi, sizeof(mbi));
    if (queried == 0) {
        return FALSE;
    }

    info->BaseAddress = mbi.BaseAddress;
    info->AllocationBase = mbi.AllocationBase;
    info->AllocationProtect = mbi.AllocationProtect;
    info->PartitionId = mbi.PartitionId;
    info->RegionSize = mbi.RegionSize;
    info->State = mbi.State;
    info->Protect = mbi.Protect;
    info->Type = mbi.Type;
    info->MemoryAttributes = compute_memory_attributes(mbi);

    bool is_xmem = false;
    {
        auto& st = state();
        std::lock_guard<std::mutex> lock(st.mutex);
        is_xmem = (st.allocations.find(mbi.AllocationBase) != st.allocations.end()) ||
                  (st.physical_mappings.find(mbi.AllocationBase) != st.physical_mappings.end());
        if (!is_xmem) {
            for (const auto& [aptr, rec] : st.allocations) {
                if (rec.raw_base == mbi.AllocationBase) {
                    is_xmem = true;
                    break;
                }
            }
        }
        if (!is_xmem) {
            for (const auto& [pptr, rec] : st.physical_mappings) {
                if (rec.raw_base == mbi.AllocationBase) {
                    is_xmem = true;
                    break;
                }
            }
        }
    }
    info->PageSize = is_xmem ? static_cast<DWORD>(kPageGranularity) : 4096u;
    return TRUE;
}

BOOL WINAPI WsxXMemGetWorkingSetStatistics(DWORD flags, XMEM_WORKING_SET_STATISTICS* stats) {
    if (!stats) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::memset(stats, 0, sizeof(*stats));

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                 sizeof(pmc))) {
        return FALSE;
    }

    const auto& st = state();
    const UINT64 tracked = st.tracked_bytes.load(std::memory_order_relaxed);
    const UINT64 phys_mapped = st.physical_mapped_bytes.load(std::memory_order_relaxed);
    const UINT64 count = st.allocation_count.load(std::memory_order_relaxed);
    // PrivateUsage (committed private bytes) updates deterministically on VirtualAlloc/VirtualFree
    // and D3D12 allocations without waiting for working-set trimming, matching delta-measurement callers.
    const UINT64 reported_bytes = (std::max)(static_cast<UINT64>(pmc.PrivateUsage), tracked + phys_mapped);

    // Console Title Memory Budget by configured console profile:
    // Xbox Series X (Anaconda): 13.5 GiB | Series S (Lockhart): 8.0 GiB | Xbox One: 5.0 GiB | Devkit: 36.0 GiB
    const std::string ctype = wsx::detail::to_lower_ascii(
        wsx::get_config_string("WSX_CONSOLE_TYPE", "Console", "type", "anaconda"));
    UINT64 title_budget_bytes = 0x360000000ULL; // 13.5 GiB (Xbox Series X default)
    if (ctype == "lockhart" || ctype == "seriess") {
        title_budget_bytes = 0x200000000ULL;    // 8.0 GiB
    } else if (ctype == "xboxone" || ctype == "1080p") {
        title_budget_bytes = 0x140000000ULL;    // 5.0 GiB
    } else if (ctype == "devkit") {
        title_budget_bytes = 0x900000000ULL;    // 36.0 GiB
    }

    // Console title budget query (flags == 0x10000001):
    //   +0x00 = Title Memory Budget Limit (13.5 GiB on Series X)
    //   +0x08 = Title Memory USED Bytes (process private + XMem tracked, NOT host PC free RAM!)
    // Legacy/delta callers (e.g. 0x20000001):
    //   +0x00 = PageCount, +0x08 = WorkingSetBytes, Reserved[0] = TitleMemoryBudgetBytes.
    stats->PageCount = (flags == kXMemWorkingSetFlagTitleBudget)
        ? title_budget_bytes
        : (reported_bytes / 4096u);
    stats->WorkingSetBytes = reported_bytes;
    stats->PeakWorkingSetBytes = static_cast<UINT64>(pmc.PeakWorkingSetSize);
    stats->PrivateBytes = static_cast<UINT64>(pmc.PrivateUsage);
    stats->XMemTrackedBytes = tracked;
    stats->XMemAllocationCount = count;
    stats->Reserved[0] = title_budget_bytes;

    if (logging_enabled()) {
        char line[160]{};
        std::snprintf(line, sizeof(line),
                      "XMemGetWorkingSetStatistics flags=0x%08lX bytes=%llu tracked=%llu\r\n",
                      static_cast<unsigned long>(flags),
                      static_cast<unsigned long long>(reported_bytes),
                      static_cast<unsigned long long>(tracked));
        write_log(line);
    }
    return TRUE;
}

BOOL WINAPI WsxXMemProtect(PVOID address, SIZE_T size, DWORD new_protect, PDWORD old_protect) {
    if (!address || size == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    DWORD local_old = 0;
    const BOOL ok = VirtualProtect(address, size, new_protect, old_protect ? old_protect : &local_old);
    if (ok) {
        auto& st = state();
        std::lock_guard<std::mutex> lock(st.mutex);
        const auto it = st.physical_mappings.find(address);
        if (it != st.physical_mappings.end()) {
            it->second.protect = new_protect;
        }
        const auto ait = st.allocations.find(address);
        if (ait != st.allocations.end()) {
            ait->second.protection_modified = true;
        }
    }
    return ok;
}

PVOID WINAPI WsxXMemMapPhysical(SIZE_T size, ULONG_PTR alignment, DWORD protect, UINT64* out_physical_address) {
    if (out_physical_address) *out_physical_address = 0;
    if (size == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const ULONG_PTR align = (alignment == 0) ? kPageGranularity : alignment;
    if ((align & (align - 1u)) != 0u || align > (16u * 1024u * 1024u)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const SIZE_T commit_size = align_up_page(size);
    if (commit_size == 0) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    const SIZE_T extra = (align > kPageGranularity) ? static_cast<SIZE_T>(align) : 0u;
    const DWORD effective_protect = protect ? protect : PAGE_READWRITE;
    void* raw = VirtualAlloc(nullptr, commit_size + extra, MEM_RESERVE | MEM_COMMIT, effective_protect);
    if (!raw) {
        return nullptr;
    }
    const uintptr_t raw_addr = reinterpret_cast<uintptr_t>(raw);
    const uintptr_t aligned_va = (raw_addr + (align - 1u)) & ~static_cast<uintptr_t>(align - 1u);
    void* aligned_ptr = reinterpret_cast<void*>(aligned_va);

    auto& st = state();
    UINT64 pa = 0;
    {
        std::lock_guard<std::mutex> lock(st.mutex);
        pa = allocate_guest_pa_locked(st, commit_size, align);
        st.physical_mappings[aligned_ptr] = PhysicalMappingRecord{
            raw, aligned_va, commit_size, align, effective_protect, pa
        };
    }
    const UINT64 cur_phys = st.physical_mapped_bytes.fetch_add(commit_size, std::memory_order_relaxed) + commit_size;
    st.physical_mapping_count.fetch_add(1, std::memory_order_relaxed);
    update_peak_tracked(st, st.tracked_bytes.load(std::memory_order_relaxed) + cur_phys);

    if (out_physical_address) *out_physical_address = pa;
    return aligned_ptr;
}

BOOL WINAPI WsxXMemUnmapPhysical(PVOID address) {
    if (!address) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    PhysicalMappingRecord rec{};
    bool found = false;
    auto& st = state();
    {
        std::lock_guard<std::mutex> lock(st.mutex);
        const auto it = st.physical_mappings.find(address);
        if (it != st.physical_mappings.end()) {
            rec = it->second;
            st.physical_mappings.erase(it);
            found = true;
            unregister_gpu_aliases_locked(st, reinterpret_cast<UINT64>(address), rec.size);
        }
    }
    if (!found) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return FALSE;
    }
    st.physical_mapped_bytes.fetch_sub(rec.size, std::memory_order_relaxed);
    st.physical_mapping_count.fetch_sub(1, std::memory_order_relaxed);
    return VirtualFree(rec.raw_base, 0, MEM_RELEASE);
}

UINT64 WINAPI WsxXMemGetPhysicalAddress(LPCVOID address) {
    if (!address) return 0;
    const uintptr_t target = reinterpret_cast<uintptr_t>(address);
    auto& st = state();
    std::lock_guard<std::mutex> lock(st.mutex);

    for (const auto& [ptr, rec] : st.physical_mappings) {
        if (target >= rec.aligned_va && (target - rec.aligned_va) < rec.size) {
            return rec.physical_address + static_cast<UINT64>(target - rec.aligned_va);
        }
    }
    for (const auto& [ptr, rec] : st.allocations) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(ptr);
        if (target >= base && (target - base) < rec.committed_size) {
            return rec.physical_address + static_cast<UINT64>(target - base);
        }
    }
    return 0;
}

VOID WINAPI WsxXMemFlushCache(LPCVOID address, SIZE_T size) {
    MemoryBarrier();
    if (address && size > 0) {
        FlushInstructionCache(GetCurrentProcess(), address, size);
    }
}

BOOL WINAPI WsxXMemGetAllocationStatistics(XMEM_ALLOCATION_STATISTICS* stats) {
    if (!stats) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::memset(stats, 0, sizeof(*stats));
    const auto& st = state();
    stats->AllocationCount = st.allocation_count.load(std::memory_order_relaxed);
    stats->TrackedBytes = st.tracked_bytes.load(std::memory_order_relaxed);
    stats->PhysicalMappingCount = st.physical_mapping_count.load(std::memory_order_relaxed);
    stats->PhysicalMappedBytes = st.physical_mapped_bytes.load(std::memory_order_relaxed);
    stats->PeakTrackedBytes = st.peak_tracked_bytes.load(std::memory_order_relaxed);
    return TRUE;
}

UINT64 WINAPI WsxXMemTrimPools(UINT64 target_free_bytes) {
    auto& st = state();
    std::vector<void*> to_release;
    UINT64 freed_bytes = 0u;
    UINT64 freed_blocks = 0u;
    {
        std::lock_guard<std::mutex> lock(st.mutex);
        // Trim largest buckets first to reclaim VRAM/RAM fastest under pressure
        for (int b = static_cast<int>(kNumPoolBuckets) - 1; b >= 0; --b) {
            auto& vec = st.free_pools[static_cast<size_t>(b)];
            const SIZE_T blk_sz = kPoolBucketSizes[static_cast<size_t>(b)];
            while (!vec.empty()) {
                if (target_free_bytes != 0u && freed_bytes >= target_free_bytes) {
                    break;
                }
                to_release.push_back(vec.back().ptr);
                vec.pop_back();
                freed_bytes += blk_sz;
                ++freed_blocks;
            }
            if (target_free_bytes != 0u && freed_bytes >= target_free_bytes) {
                break;
            }
        }
    }
    for (void* p : to_release) {
        VirtualFree(p, 0, MEM_RELEASE);
    }
    if (freed_blocks > 0u) {
        st.pooled_block_count.fetch_sub(freed_blocks, std::memory_order_relaxed);
        st.pooled_bytes.fetch_sub(freed_bytes, std::memory_order_relaxed);
        st.trimmed_blocks.fetch_add(freed_blocks, std::memory_order_relaxed);
        st.trimmed_bytes.fetch_add(freed_bytes, std::memory_order_relaxed);
    }
    return freed_bytes;
}

BOOL WINAPI WsxXMemGetPoolStatistics(XMEM_POOL_STATISTICS* stats) {
    if (!stats) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::memset(stats, 0, sizeof(*stats));
    const auto& st = state();
    stats->PoolHits = st.pool_hits.load(std::memory_order_relaxed);
    stats->PoolMisses = st.pool_misses.load(std::memory_order_relaxed);
    stats->PooledBlockCount = st.pooled_block_count.load(std::memory_order_relaxed);
    stats->PooledBytes = st.pooled_bytes.load(std::memory_order_relaxed);
    stats->TrimmedBlocks = st.trimmed_blocks.load(std::memory_order_relaxed);
    stats->TrimmedBytes = st.trimmed_bytes.load(std::memory_order_relaxed);
    stats->MaxPoolBytes = st.max_pool_bytes;
    return TRUE;
}

BOOL WINAPI WsxXMemGetWin32HeapStatistics(XMEM_WIN32_HEAP_STATISTICS* stats) {
    if (!stats) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::memset(stats, 0, sizeof(*stats));
    XMEM_WORKING_SET_STATISTICS ws{};
    if (!WsxXMemGetWorkingSetStatistics(kXMemWorkingSetFlagTitleBudget, &ws)) {
        return FALSE;
    }
    const auto& st = state();
    stats->TitleMemoryBudgetBytes = ws.PageCount;
    stats->ProcessPrivateBytes = ws.PrivateBytes;
    stats->WorkingSetBytes = ws.WorkingSetBytes;
    stats->XMemTrackedBytes = ws.XMemTrackedBytes;
    stats->PhysicalMappedBytes = st.physical_mapped_bytes.load(std::memory_order_relaxed);
    stats->AllocationCount = ws.XMemAllocationCount;
    return TRUE;
}

BOOL WINAPI WsxXMemFreePhysicalPages(PVOID address, SIZE_T /*size*/) {
    if (!address) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (WsxXMemUnmapPhysical(address)) {
        return TRUE;
    }
    return VirtualFree(address, 0, MEM_RELEASE);
}

HRESULT WINAPI WsxXMemTransferMemory(PVOID dst, LPCVOID src, SIZE_T size, ULONGLONG flags) {
    if (size == 0u) return S_OK;
    if (!dst || !src) return E_POINTER;
    std::memmove(dst, src, size);
    if ((flags & 1ULL) != 0ULL) {
        WsxXMemFlushCache(dst, size);
    }
    return S_OK;
}

BOOL WINAPI WsxXMemRegisterGpuAlias(LPCVOID cpuBase, SIZE_T size, UINT64 desktopGpuVa) {
    if (!cpuBase || size == 0u || desktopGpuVa == 0ULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const UINT64 base = reinterpret_cast<UINT64>(cpuBase);
    const UINT64 span = static_cast<UINT64>(size);
    if (base + span < base) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    auto& st = state();
    std::lock_guard<std::mutex> lock(st.mutex);
    st.gpu_aliases[base] = GpuAliasInterval{base, size, desktopGpuVa};
    return TRUE;
}

BOOL WINAPI WsxXMemUnregisterGpuAlias(LPCVOID cpuBase) {
    if (!cpuBase) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const UINT64 base = reinterpret_cast<UINT64>(cpuBase);
    auto& st = state();
    std::lock_guard<std::mutex> lock(st.mutex);
    const auto it = st.gpu_aliases.find(base);
    if (it == st.gpu_aliases.end()) {
        SetLastError(ERROR_NOT_FOUND);
        return FALSE;
    }
    st.gpu_aliases.erase(it);
    return TRUE;
}

BOOL WINAPI WsxXMemTranslateTitleGpuVa(UINT64 titleVa, SIZE_T byteCount, UINT64* outDesktopGpuVa) {
    if (!outDesktopGpuVa) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *outDesktopGpuVa = 0ULL;
    if (titleVa == 0ULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const UINT64 req_bytes = static_cast<UINT64>(byteCount);
    if (titleVa + req_bytes < titleVa) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    auto& st = state();
    std::lock_guard<std::mutex> lock(st.mutex);
    for (const auto& [base, interval] : st.gpu_aliases) {
        if (titleVa >= interval.cpu_base) {
            const UINT64 offset = titleVa - interval.cpu_base;
            const UINT64 span = static_cast<UINT64>(interval.size);
            if (offset < span && req_bytes <= (span - offset)) {
                *outDesktopGpuVa = interval.desktop_gpu_va + offset;
                return TRUE;
            }
        }
    }
    SetLastError(ERROR_NOT_FOUND);
    return FALSE;
}

} // extern "C"
