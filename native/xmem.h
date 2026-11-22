// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Windows x64 implementation of Xbox Series X|S memory APIs (xmem.dll).

#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstdint>

extern "C" {

// 64-byte structure observed by XMemVirtualQuery callers (e.g. XboxBvhBuilder.dll):
//   +0x00..+0x2F : Standard x64 MEMORY_BASIC_INFORMATION (48 bytes)
//   +0x30        : Extended memory attributes (bit 0 = readable/GPU-read, bit 1 = writable/GPU-write)
//   +0x34        : Page size in bytes
//   +0x38        : Reserved
struct XMEM_MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress;
    PVOID AllocationBase;
    DWORD AllocationProtect;
    WORD PartitionId;
    WORD Reserved0;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
    DWORD Reserved1;
    DWORD MemoryAttributes;
    DWORD PageSize;
    UINT64 Reserved2;
};

static_assert(sizeof(XMEM_MEMORY_BASIC_INFORMATION) == 0x40,
              "XMEM_MEMORY_BASIC_INFORMATION must be 64 bytes");
static_assert(offsetof(XMEM_MEMORY_BASIC_INFORMATION, State) == 0x20,
              "State offset must match Win32 MEMORY_BASIC_INFORMATION");
static_assert(offsetof(XMEM_MEMORY_BASIC_INFORMATION, Protect) == 0x24,
              "Protect offset must match Win32 MEMORY_BASIC_INFORMATION");
static_assert(offsetof(XMEM_MEMORY_BASIC_INFORMATION, MemoryAttributes) == 0x30,
              "MemoryAttributes offset must be 0x30");

// 128-byte structure observed by XMemGetWorkingSetStatistics callers
// (e.g. amd_fidelityfx_upscaler_xbox.dll passes 0x80-byte buffer and reads +0x08).
struct XMEM_WORKING_SET_STATISTICS {
    UINT64 PageCount;            // +0x00
    UINT64 WorkingSetBytes;      // +0x08 (commit/working set bytes used for allocation deltas)
    UINT64 PeakWorkingSetBytes;  // +0x10
    UINT64 PrivateBytes;         // +0x18
    UINT64 XMemTrackedBytes;     // +0x20
    UINT64 XMemAllocationCount;  // +0x28
    UINT64 Reserved[10];         // +0x30..+0x7F
};

static_assert(sizeof(XMEM_WORKING_SET_STATISTICS) == 0x80,
              "XMEM_WORKING_SET_STATISTICS must be 128 bytes");
static_assert(offsetof(XMEM_WORKING_SET_STATISTICS, WorkingSetBytes) == 0x08,
              "WorkingSetBytes offset must be 0x08");

struct XMEM_ALLOCATION_STATISTICS {
    UINT64 AllocationCount;
    UINT64 TrackedBytes;
    UINT64 PhysicalMappingCount;
    UINT64 PhysicalMappedBytes;
    UINT64 PeakTrackedBytes;
    UINT64 Reserved[3];
};

static_assert(sizeof(XMEM_ALLOCATION_STATISTICS) == 0x40,
              "XMEM_ALLOCATION_STATISTICS must be 64 bytes");

struct XMEM_POOL_STATISTICS {
    UINT64 PoolHits;
    UINT64 PoolMisses;
    UINT64 PooledBlockCount;
    UINT64 PooledBytes;
    UINT64 TrimmedBlocks;
    UINT64 TrimmedBytes;
    UINT64 MaxPoolBytes;
    UINT64 Reserved;
};

static_assert(sizeof(XMEM_POOL_STATISTICS) == 0x40,
              "XMEM_POOL_STATISTICS must be 64 bytes");

struct XMEM_WIN32_HEAP_STATISTICS {
    UINT64 TitleMemoryBudgetBytes; // +0x00 (e.g. 13.5 GiB on Series X)
    UINT64 ProcessPrivateBytes;    // +0x08
    UINT64 WorkingSetBytes;        // +0x10
    UINT64 XMemTrackedBytes;       // +0x18
    UINT64 PhysicalMappedBytes;    // +0x20
    UINT64 AllocationCount;        // +0x28
    UINT64 Reserved[2];            // +0x30..+0x3F
};

static_assert(sizeof(XMEM_WIN32_HEAP_STATISTICS) == 0x40,
              "XMEM_WIN32_HEAP_STATISTICS must be 64 bytes");

struct XMEM_DECODED_ATTRIBUTES {
    UINT16 ObjectType;         // bits 0..13
    UINT8  PageSizeCode;       // bits 14..15 (0=4KB, 1=64KB, 2=4MB, 3=invalid)
    UINT8  AllocatorId;        // bits 16..23
    UINT8  AlignmentLog2;      // bits 24..28 (clamped >= 4 -> 16B)
    UINT8  MemoryType;         // bits 29..32 (0=heap, 1..6=graphics, 7..9=physical)
    UINT8  IsGraphicsMemory;   // 1 if MemoryType in 1..6
    UINT8  IsValid;            // 1 if Reserved(bits 33..63)==0 && PageSizeCode!=3 && MemoryType<=9
    UINT64 PageSizeBytes;      // 4096, 65536, or 4194304
    UINT64 AlignmentBytes;     // 1ULL << max(AlignmentLog2, 4)
    UINT64 ReservedBits;       // bits 33..63
};

static_assert(sizeof(XMEM_DECODED_ATTRIBUTES) == 0x20, "XMEM_DECODED_ATTRIBUTES must be 32 bytes");

constexpr DWORD kXMemWorkingSetFlagTitleBudget = 0x10000001u;

using XMemAllocRoutine = PVOID(WINAPI*)(SIZE_T size, ULONGLONG attributes);
using XMemFreeRoutine = VOID(WINAPI*)(PVOID address, ULONGLONG attributes);

PVOID WINAPI WsxXMemAlloc(SIZE_T size, ULONGLONG attributes);
VOID WINAPI WsxXMemFree(PVOID address, ULONGLONG attributes);
PVOID WINAPI WsxXMemAllocDefault(SIZE_T size, ULONGLONG attributes);
VOID WINAPI WsxXMemFreeDefault(PVOID address, ULONGLONG attributes);
SIZE_T WINAPI WsxXMemSize(PVOID address, ULONGLONG attributes);
SIZE_T WINAPI WsxXMemSizeDefault(PVOID address, ULONGLONG attributes);
VOID WINAPI WsxXMemSetAllocationHooks(XMemAllocRoutine alloc_hook, XMemFreeRoutine free_hook);
BOOL WINAPI WsxXMemVirtualQuery(HANDLE process, LPCVOID address, XMEM_MEMORY_BASIC_INFORMATION* info);
BOOL WINAPI WsxXMemGetWorkingSetStatistics(DWORD flags, XMEM_WORKING_SET_STATISTICS* stats);
BOOL WINAPI WsxXMemProtect(PVOID address, SIZE_T size, DWORD new_protect, PDWORD old_protect);
PVOID WINAPI WsxXMemMapPhysical(SIZE_T size, ULONG_PTR alignment, DWORD protect, UINT64* out_physical_address);
BOOL WINAPI WsxXMemUnmapPhysical(PVOID address);
UINT64 WINAPI WsxXMemGetPhysicalAddress(LPCVOID address);
VOID WINAPI WsxXMemFlushCache(LPCVOID address, SIZE_T size);
BOOL WINAPI WsxXMemGetAllocationStatistics(XMEM_ALLOCATION_STATISTICS* stats);
UINT64 WINAPI WsxXMemTrimPools(UINT64 target_free_bytes);
BOOL WINAPI WsxXMemGetPoolStatistics(XMEM_POOL_STATISTICS* stats);
BOOL WINAPI WsxXMemGetWin32HeapStatistics(XMEM_WIN32_HEAP_STATISTICS* stats);
BOOL WINAPI WsxXMemFreePhysicalPages(PVOID address, SIZE_T size);
HRESULT WINAPI WsxXMemTransferMemory(PVOID dst, LPCVOID src, SIZE_T size, ULONGLONG flags);
BOOL WINAPI WsxXMemDecodeAttributes(ULONGLONG attributes, XMEM_DECODED_ATTRIBUTES* outDecoded);
BOOL WINAPI WsxXMemRegisterGpuAlias(LPCVOID cpuBase, SIZE_T size, UINT64 desktopGpuVa);
BOOL WINAPI WsxXMemUnregisterGpuAlias(LPCVOID cpuBase);
BOOL WINAPI WsxXMemTranslateTitleGpuVa(UINT64 titleVa, SIZE_T byteCount, UINT64* outDesktopGpuVa);

} // extern "C"
