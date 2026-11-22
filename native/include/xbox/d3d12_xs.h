// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox Series X|S Direct3D 12.X compatibility header (<d3d12_xs.h>).

#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>

#ifndef IID_GRAPHICS_PPV_ARGS
#define IID_GRAPHICS_PPV_ARGS(ppType) IID_PPV_ARGS(ppType)
#endif

enum D3D12XBOX_CREATE_DEVICE_FLAGS : UINT {
    D3D12XBOX_CREATE_DEVICE_FLAG_NONE = 0x0,
};

enum D3D12XBOX_PROCESS_DEBUG_FLAGS : UINT {
    D3D12_PROCESS_DEBUG_FLAG_NONE = 0x0,
    D3D12_PROCESS_DEBUG_FLAG_DEBUG_LAYER_ENABLED = 0x1,
    D3D12XBOX_PROCESS_DEBUG_FLAG_INSTRUMENTED = 0x2,
};

constexpr UINT D3D12XBOX_DEFAULT_SIZE_BYTES = 0u;

#ifndef D3D12_ERROR_DRIVER_VERSION_MISMATCH
#define D3D12_ERROR_DRIVER_VERSION_MISMATCH static_cast<HRESULT>(0x887E0002L)
#endif

// Xbox-specific resource flags (stripped by d3d12_xs.dll along with D3D12_HEAP_FLAG_ALLOW_DISPLAY)
constexpr D3D12_RESOURCE_FLAGS D3D12XBOX_RESOURCE_FLAG_ALLOW_DCC =
    static_cast<D3D12_RESOURCE_FLAGS>(0x10000000u);
constexpr D3D12_RESOURCE_FLAGS D3D12XBOX_RESOURCE_FLAG_FORCE_TEXTURE_COMPATIBILITY =
    static_cast<D3D12_RESOURCE_FLAGS>(0x20000000u);
constexpr D3D12_RESOURCE_FLAGS D3D12XBOX_RESOURCE_FLAG_ALLOW_AUTOMATIC_GAMEDVR_TONE_MAP =
    static_cast<D3D12_RESOURCE_FLAGS>(0x40000000u);

using D3D12XBOX_FRAME_PIPELINE_TOKEN = uint64_t;
constexpr D3D12XBOX_FRAME_PIPELINE_TOKEN D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL = 0ULL;

enum D3D12XBOX_FRAME_EVENTS : UINT {
    D3D12XBOX_FRAME_EVENT_ORIGIN = 0u,
};

enum D3D12XBOX_WAIT_FRAME_EVENT_FLAGS : UINT {
    D3D12XBOX_WAIT_FRAME_EVENT_FLAG_NONE = 0u,
};

enum D3D12XBOX_SCHEDULE_FRAME_EVENT_FLAGS : UINT {
    D3D12XBOX_SCHEDULE_FRAME_EVENT_FLAG_NONE = 0u,
};

enum D3D12XBOX_FRAME_INTERVALS : UINT {
    D3D12XBOX_FRAME_INTERVAL_UNLIMITED = 0u,
    D3D12XBOX_FRAME_INTERVAL_60_HZ     = 1u,
    D3D12XBOX_FRAME_INTERVAL_120_HZ    = 2u,
    D3D12XBOX_FRAME_INTERVAL_30_HZ     = 3u,
};

enum D3D12XBOX_FRAME_INTERVAL_FLAGS : UINT {
    D3D12XBOX_FRAME_INTERVAL_FLAG_NONE = 0u,
    D3D12XBOX_FRAME_INTERVAL_FLAG_VRR  = 1u,
};

struct D3D12XBOX_CREATE_DEVICE_PARAMETERS {
    UINT Version;
    UINT ProcessDebugFlags;
    UINT GraphicsCommandQueueRingSizeBytes;
    BOOL DisableGeometryShaderAllocations;
    BOOL DisableTessellationShaderAllocations;
    BOOL DisableDXR;
    D3D12XBOX_CREATE_DEVICE_FLAGS CreateDeviceFlags;
    UINT AmplificationShaderIndirectArgsBufferSize;
    UINT AmplificationShaderPayloadBufferSize;
};

struct D3D12XBOX_PRESENT_PLANE_PARAMETERS {
    D3D12XBOX_FRAME_PIPELINE_TOKEN Token;
    UINT ResourceCount;
    ID3D12Resource* const* ppResources;
    DXGI_COLOR_SPACE_TYPE ColorSpace;
};

struct D3D12XBOX_PRESENT_PARAMETERS {
    UINT Flags;
};

struct D3D12XBOX_ABI_TELEMETRY {
    uint64_t DescHeapsCreated;
    uint64_t CommittedResourcesVia260;
    uint64_t ComputePsoXOk;
    uint64_t ComputePsoXFail;
    uint64_t ComputePsoXFallback;
    uint64_t CmdListsWrapped;
    uint64_t UmaAllocGpuMemCalls;
    uint64_t UmaVaTranslations;
    uint64_t UnhandledDeviceSlots;
    uint64_t UnhandledCmdListSlots;
    uint64_t ShadersInspected;
    uint64_t ShadersPureDxil;
    uint64_t ShadersXboxIntrinsics;
    uint64_t ShadersFssmRdna2;
    uint64_t ShadersAcccRdna2;
    uint64_t ShadersCacheHits;
    uint64_t ShadersTranslatorHits;
    uint64_t ShadersRejectedStrict;
};

enum D3D12XBOX_SHADER_KIND : UINT {
    D3D12XBOX_SHADER_KIND_UNKNOWN             = 0u,
    D3D12XBOX_SHADER_KIND_DESKTOP_DXIL        = 1u, // Standard desktop DXBC/DXIL
    D3D12XBOX_SHADER_KIND_XBOX_DXIL_PURE      = 2u, // dxil-ms-xb2 LLVM bitcode, 0 @xbox.* intrinsics (runs natively on desktop D3D12)
    D3D12XBOX_SHADER_KIND_XBOX_DXIL_INTRINSIC = 3u, // dxil-ms-xb2 LLVM bitcode with proprietary @xbox.* RDNA2 intrinsics
    D3D12XBOX_SHADER_KIND_XBOX_FSSM_RDNA2     = 4u, // DXIL stub (NOBC) + XDAT -> XBCH (zlib) -> FSSM native RDNA2 ISA (shaders.rpf)
    D3D12XBOX_SHADER_KIND_XBOX_ACCC_RDNA2     = 5u, // ACCC (FSPX) native RDNA2 ISA (e.g. amd_fidelityfx_upscaler_xbox.dll)
};

enum D3D12XBOX_SHADER_STAGE : UINT {
    D3D12XBOX_SHADER_STAGE_PIXEL         = 0u,
    D3D12XBOX_SHADER_STAGE_VERTEX        = 1u,
    D3D12XBOX_SHADER_STAGE_GEOMETRY      = 2u,
    D3D12XBOX_SHADER_STAGE_HULL          = 3u,
    D3D12XBOX_SHADER_STAGE_DOMAIN        = 4u,
    D3D12XBOX_SHADER_STAGE_COMPUTE       = 5u,
    D3D12XBOX_SHADER_STAGE_LIBRARY       = 6u,
    D3D12XBOX_SHADER_STAGE_MESH          = 13u,
    D3D12XBOX_SHADER_STAGE_AMPLIFICATION = 14u,
    D3D12XBOX_SHADER_STAGE_UNKNOWN       = 0xFFFFu,
};

enum D3D12XBOX_SHADER_TARGET_FORMAT : UINT {
    D3D12XBOX_SHADER_TARGET_NONE   = 0u,
    D3D12XBOX_SHADER_TARGET_DXIL   = 1u, // Desktop D3D12 DXBC/DXIL bytecode
    D3D12XBOX_SHADER_TARGET_SPIRV  = 2u, // Vulkan SPIR-V bytecode (RDNA2 -> SPIR-V / PhysicalStorageBuffer64 contract)
};

enum D3D12XBOX_GRAPHICS_BACKEND : UINT {
    D3D12XBOX_GRAPHICS_BACKEND_D3D12  = 0u, // Native Desktop D3D12.X COM proxy (default)
    D3D12XBOX_GRAPHICS_BACKEND_VULKAN = 1u, // Planned Vulkan backend for precompiled FSSM RDNA2 -> SPIR-V shaders
};

struct D3D12XBOX_SHADER_INFO {
    D3D12XBOX_SHADER_KIND Kind;
    D3D12XBOX_SHADER_STAGE Stage;
    uint16_t ShaderModelMajor;
    uint16_t ShaderModelMinor;
    uint32_t RootSignatureFlags;
    uint32_t XboxIntrinsicCount;
    uint32_t HasXboxTypedLoad;
    uint32_t HasNobcStub;
    uint32_t HasRdna2EndPgm;
    uint32_t XbchStoredSize;
    uint32_t XbchUnpackedSize;
    uint32_t FssmCodeOffset;
    uint32_t FssmCodeSize;
    uint32_t AcccPayloadSize;
    uint8_t ContainerDigest[16]; // DXBC header digest (+0x04..+0x13)
    uint8_t ShaderHash[16];      // HASH chunk digest (or ContainerDigest fallback)
    uint8_t XdatHash[16];        // XDAT tag 0x12 hash (if present)
};

struct D3D12XBOX_SM66_CAPS {
    uint32_t HighestShaderModel;      // e.g. 0x66 for SM 6.6
    uint32_t ResourceBindingTier;     // 3 required for Dynamic Resources
    uint32_t SupportsDynamicResources;// 1 if SM 6.6 ResourceDescriptorHeap / SamplerDescriptorHeap available
    uint32_t SupportsInt64Atomics;    // 1 if 64-bit integer atomics supported
    uint32_t WaveLaneCountMin;        // 32 on NVIDIA, 32/64 on AMD
    uint32_t WaveLaneCountMax;
    uint32_t MeshShaderTier;          // D3D12_MESH_SHADER_TIER (10 = Tier 1)
    uint32_t RaytracingTier;          // D3D12_RAYTRACING_TIER (11 = Tier 1.1)
};

// Pure Direct3D 12 SM 6.6 User Data Root Constants contract (40 DWORDS = 160 bytes)
struct D3D12XBOX_SM66_USER_DATA {
    uint32_t S[32];             // Xbox user SGPRs s0..s31
    uint32_t HeapBaseLo;        // 64-bit Xbox GPU slab heap base VA
    uint32_t HeapBaseHi;
    uint32_t CodeBaseLo;        // 64-bit Xbox GPU VA of uploaded FSSM blob (s_getpc_b64)
    uint32_t CodeBaseHi;
    uint32_t SlabBaseIdx;       // First ResourceDescriptorHeap[] index of RWByteAddressBuffer slabs
    uint32_t SlabShift;         // log2(slabSizeBytes)
    uint32_t DescTableSrvIdx;   // ResourceDescriptorHeap[] index of FNV-1a T#/S# StructuredBuffer
    uint32_t DescTableMask;     // (tableCapacity - 1), power of 2
};

// Pure Direct3D 12 SM 6.6 Bindless FNV-1a T#/S# Hash Table Entry (40 bytes)
struct D3D12XBOX_SM66_DESC_HASH_ENTRY {
    uint32_t Key[8];     // Raw 8-dword T# OR 4-dword S# + {0xFFFFFFFF x 4}
    uint32_t HeapIndex;  // Index into ResourceDescriptorHeap[] or SamplerDescriptorHeap[]
    uint32_t Flags;      // 0 = empty, 1 = ResourceDescriptorHeap, 2 = SamplerDescriptorHeap
};

// Diagnostics returned by D3D12XboxTranslateFssmToHlslSm66 / D3D12XboxTranslateDxbcFssmToDxil
struct D3D12XBOX_RDNA2_TRANSLATE_STATS {
    uint32_t Stage;
    uint32_t NumThreadsX;
    uint32_t NumThreadsY;
    uint32_t NumThreadsZ;
    uint32_t CodeOffset;
    uint32_t CodeSize;
    uint32_t TotalInstructions;
    uint32_t TranslatedInstructions;
    uint32_t UnsupportedInstructions;
    uint32_t BasicBlockCount;
    uint32_t SgprCount;
    uint32_t VgprCount;
    uint32_t UsesLds;
    uint32_t UsesSm66Slabs;
    uint32_t UsesBindlessTextures;
};

// Pluggable shader translator callback:
// Called by d3d12_xs.dll when Create*PipelineState encounters a non-desktop Xbox shader
// (FSSM RDNA2, ACCC/FSPX RDNA2, or @xbox.* DXIL). The callback writes translated desktop
// DXIL (or SPIR-V) into outBuffer (or queries required size when outBuffer == nullptr) and
// sets *outTargetFormat + *outSize, returning S_OK on success.
using PFN_D3D12XBOX_TRANSLATE_SHADER = HRESULT(WINAPI*)(
    const D3D12XBOX_SHADER_INFO* pInfo,
    const void* pOriginalDxbc,
    size_t originalDxbcSize,
    const void* pUnpackedFssm,
    size_t unpackedFssmSize,
    D3D12XBOX_SHADER_TARGET_FORMAT* pOutTargetFormat,
    void* pOutBuffer,
    size_t outBufferCapacity,
    size_t* pOutBufferSize,
    void* pUserContext);

extern "C" {
HRESULT WINAPI D3D12XboxCreateDevice(
    IUnknown* pAdapter,
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* pParams,
    REFIID riid,
    void** ppDevice);

HRESULT WINAPI D3D12XboxQueryInterface(
    ID3D12Device* pDevice,
    REFIID riid,
    void** ppvObject);

HRESULT WINAPI D3D12XboxCreateCommittedResource(
    ID3D12Device* pDevice,
    const D3D12_HEAP_PROPERTIES* pHeapProperties,
    D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    REFIID riidResource,
    void** ppvResource);

HRESULT WINAPI D3D12XboxSetFrameIntervalX(
    ID3D12Device* pDevice,
    IDXGIOutput* pOutput,
    UINT Interval,
    UINT MaxLatency,
    UINT Flags);

HRESULT WINAPI D3D12XboxScheduleFrameEventX(
    ID3D12Device* pDevice,
    UINT EventType,
    UINT64 Time,
    void* pData,
    UINT Flags);

HRESULT WINAPI D3D12XboxWaitFrameEventX(
    ID3D12Device* pDevice,
    UINT EventType,
    DWORD TimeoutMs,
    void* pData,
    UINT Flags,
    D3D12XBOX_FRAME_PIPELINE_TOKEN* pToken);

HRESULT WINAPI D3D12XboxPresentX(
    ID3D12CommandQueue* pQueue,
    UINT PlaneCount,
    const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters,
    const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters);

HRESULT WINAPI D3D12XboxSuspendCommandQueueX(
    ID3D12CommandQueue* pQueue,
    UINT Flags);

HRESULT WINAPI D3D12XboxResumeCommandQueueX(
    ID3D12CommandQueue* pQueue);

HRESULT WINAPI D3D12XboxGetXboxAbiDevice(
    ID3D12Device* pDevice,
    REFIID riid,
    void** ppXboxAbiDevice);

HRESULT WINAPI D3D12XboxWrapCommandListForXboxAbi(
    ID3D12GraphicsCommandList* pCmdList,
    void** ppXboxAbiCmdList);

HRESULT WINAPI D3D12XboxGetAbiTelemetry(
    D3D12XBOX_ABI_TELEMETRY* pOut);

HRESULT WINAPI D3D12XboxInspectShaderBytecode(
    const void* pBytecode,
    size_t bytecodeLength,
    D3D12XBOX_SHADER_INFO* pOutInfo);

HRESULT WINAPI D3D12XboxUnpackShaderFssm(
    const void* pDxbc,
    size_t dxbcSize,
    void* pOutFssm,
    size_t outCapacity,
    size_t* pOutSize,
    uint32_t* pOutCodeOffset);

HRESULT WINAPI D3D12XboxRegisterShaderTranslator(
    PFN_D3D12XBOX_TRANSLATE_SHADER pCallback,
    void* pUserContext);

D3D12XBOX_GRAPHICS_BACKEND WINAPI D3D12XboxGetGraphicsBackend();

HRESULT WINAPI D3D12XboxCheckSm66Support(
    ID3D12Device* pDevice,
    D3D12XBOX_SM66_CAPS* pOutCaps);

HRESULT WINAPI D3D12XboxCreateSm66RootSignature(
    ID3D12Device* pDevice,
    REFIID riid,
    void** ppRootSignature);

HRESULT WINAPI D3D12XboxGetSm66HlslPreamble(
    const char** ppOutPreamble,
    size_t* pOutLength);

HRESULT WINAPI D3D12XboxCompileHlslToDxil(
    const char* pHlslSource,
    size_t sourceLength,
    const wchar_t* pEntryPoint,
    const wchar_t* pTargetProfile,
    void* pOutDxil,
    size_t outCapacity,
    size_t* pOutSize,
    char* pOutErrors,
    size_t errorCapacity);

HRESULT WINAPI D3D12XboxTranslateFssmToHlslSm66(
    const void* pFssm,
    size_t fssmSize,
    D3D12XBOX_SHADER_STAGE stage,
    char* pOutHlsl,
    size_t outCapacity,
    size_t* pOutSize,
    D3D12XBOX_RDNA2_TRANSLATE_STATS* pOutStats);

typedef struct D3D12XBOX_GUEST_TEXTURE_STATS {
    uint64_t tracked_textures;
    uint64_t backing_allocations;
    uint64_t sync_flushes;
    uint64_t textures_uploaded;
    uint64_t deswizzled_uploads;
    uint64_t bytes_uploaded;
} D3D12XBOX_GUEST_TEXTURE_STATS;

HRESULT WINAPI D3D12XboxTranslateDxbcFssmToDxil(
    const void* pDxbc,
    size_t dxbcSize,
    void* pOutDxil,
    size_t outCapacity,
    size_t* pOutSize,
    char* pOutErrors,
    size_t errorCapacity,
    D3D12XBOX_RDNA2_TRANSLATE_STATS* pOutStats);

HRESULT WINAPI D3D12XboxSetGuestTextureSwizzleMode(
    ID3D12Resource* pResource,
    UINT swizzleMode);

HRESULT WINAPI D3D12XboxFlushGuestTextures(
    ID3D12CommandQueue* pQueue);

HRESULT WINAPI D3D12XboxGetGuestTextureSyncStats(
    ID3D12Device* pDevice,
    D3D12XBOX_GUEST_TEXTURE_STATS* pOutStats);

HRESULT WINAPI D3D12XboxReadbackGuestTexture(
    ID3D12CommandQueue* pQueue,
    ID3D12Resource* pResource);

typedef struct D3D12XBOX_PSO_CACHE_STATS {
    uint64_t hits;
    uint64_t misses;
    uint64_t stores;
    uint64_t flushes;
    uint64_t library_bytes;
} D3D12XBOX_PSO_CACHE_STATS;

typedef struct D3D12XBOX_VRAM_BUDGET {
    uint64_t local_budget_bytes;
    uint64_t local_usage_bytes;
    uint64_t local_available_bytes;
    uint64_t non_local_budget_bytes;
    uint64_t non_local_usage_bytes;
    uint32_t pressure_level; // 0 = Normal (<80%), 1 = Warning (80..92%), 2 = Critical (>=92%)
    uint32_t reserved;
} D3D12XBOX_VRAM_BUDGET;

typedef struct D3D12XBOX_HIGHLOAD_PIPELINE_STATS {
    uint64_t write_watch_skips;
    uint64_t gpu_deswizzled_uploads;
    uint64_t staging_trims;
    uint64_t draw_calls;
    uint64_t dispatch_calls;
    uint64_t mesh_dispatches;
    uint64_t ray_dispatches;
    uint64_t barriers_submitted;
    uint64_t barriers_elided;
    uint64_t execute_command_lists;
    uint64_t async_enqueued;
    uint64_t async_completed;
    uint64_t async_failed;
    uint64_t async_dedup_hits;
    uint64_t async_in_flight;
    uint32_t async_worker_threads;
    uint32_t reserved;
} D3D12XBOX_HIGHLOAD_PIPELINE_STATS;

HRESULT WINAPI D3D12XboxRegisterFrameEventX(
    ID3D12Device* pDevice,
    UINT EventType,
    HANDLE hEvent,
    UINT Flags);

HRESULT WINAPI D3D12XboxFlushPsoCache(
    ID3D12Device* pDevice);

HRESULT WINAPI D3D12XboxGetPsoCacheStats(
    D3D12XBOX_PSO_CACHE_STATS* pOutStats);

HRESULT WINAPI D3D12XboxQueryVramBudget(
    ID3D12Device* pDevice,
    D3D12XBOX_VRAM_BUDGET* pOutBudget);

HRESULT WINAPI D3D12XboxEnqueueAsyncShaderCompile(
    const void* pDxbc,
    size_t dxbcSize);

HRESULT WINAPI D3D12XboxDrainAsyncShaderQueue(
    DWORD timeoutMs);

HRESULT WINAPI D3D12XboxGetHighloadPipelineStats(
    ID3D12Device* pDevice,
    D3D12XBOX_HIGHLOAD_PIPELINE_STATS* pOutStats);

HRESULT WINAPI D3D12XboxTrimStagingMemory(
    ID3D12CommandQueue* pQueue,
    uint64_t* pOutSlotsFreed);

HRESULT WINAPI D3D12XboxSetHudEnabled(
    ID3D12Device* pDevice,
    BOOL enabled);

HRESULT WINAPI D3D12XboxRenderDebugHud(
    ID3D12Device* pDevice,
    ID3D12CommandQueue* pQueue,
    ID3D12Resource* pBackbuffer,
    D3D12_RESOURCE_STATES stateBefore,
    D3D12_RESOURCE_STATES stateAfter);

HRESULT WINAPI D3D12XboxCaptureGpuDredReport(
    ID3D12Device* pDevice,
    char* pOutBuf,
    size_t outCapacity,
    size_t* pOutLength);

typedef struct D3D12XBOX_WINDOW_DISPLAY_STATS {
    uint32_t width;
    uint32_t height;
    uint32_t is_fullscreen;
    uint32_t close_requested;
    uint64_t resize_count;
    uint64_t scaled_blit_frames;
} D3D12XBOX_WINDOW_DISPLAY_STATS;

HRESULT WINAPI D3D12XboxWriteCrashReport(
    void* pExceptionPointers,
    const wchar_t* pOutDir,
    wchar_t* pOutTxtPath,
    size_t txtCap,
    wchar_t* pOutDmpPath,
    size_t dmpCap,
    ID3D12Device* pOptionalDevice);

HRESULT WINAPI D3D12XboxToggleBorderlessFullscreen(
    HWND hwnd,
    BOOL* pOutIsFullscreen);

HRESULT WINAPI D3D12XboxResizeSwapChain(
    ID3D12CommandQueue* pQueue,
    UINT width,
    UINT height);

HRESULT WINAPI D3D12XboxGetWindowDisplayStats(
    ID3D12CommandQueue* pQueue,
    D3D12XBOX_WINDOW_DISPLAY_STATS* pOutStats);

typedef struct D3D12XBOX_FRAME_PACING_STATS {
    uint32_t target_fps;
    uint32_t low_latency_enabled;
    uint64_t paced_frames;
    double last_frame_ms;
    double avg_fps;
    double one_percent_low_fps;
    double p99_frame_ms;
} D3D12XBOX_FRAME_PACING_STATS;

typedef struct D3D12XBOX_EXTENDED_ABI_STATS {
    uint64_t graphics_pso_x_ok;
    uint64_t placed_resources_x_ok;
    uint64_t dispatch_mesh_calls;
    uint64_t render_pass_calls;
    uint64_t execute_indirect_calls;
    uint64_t pix_markers_recorded;
    uint64_t unhandled_device_slots;
    uint64_t unhandled_cmdlist_slots;
} D3D12XBOX_EXTENDED_ABI_STATS;

HRESULT WINAPI D3D12XboxConfigureFramePacing(
    UINT maxFps,
    BOOL lowLatencyWait);

HRESULT WINAPI D3D12XboxGetFramePacingStats(
    D3D12XBOX_FRAME_PACING_STATS* pOutStats);

HRESULT WINAPI D3D12XboxGetExtendedAbiStats(
    D3D12XBOX_EXTENDED_ABI_STATS* pOutStats);

HRESULT WINAPI D3D12XboxCaptureBackbufferPng(
    ID3D12Device* pDevice,
    ID3D12CommandQueue* pQueue,
    ID3D12Resource* pBackbuffer,
    D3D12_RESOURCE_STATES stateBefore,
    D3D12_RESOURCE_STATES stateAfter,
    const wchar_t* pOutPngPath);

HRESULT WINAPI D3D12XboxSetProcessDebugFlags(UINT flags);
UINT WINAPI D3D12XboxGetProcessDebugFlags();

struct D3D12XBOX_VIRTUAL_MEMORY_CONFIGURATION {
    UINT32 Flags;
    UINT32 SizeIn4MbUnits;
    UINT32 Option;
    UINT32 Reserved;
    UINT64 Result0;
    UINT64 Result1;
};
static_assert(sizeof(D3D12XBOX_VIRTUAL_MEMORY_CONFIGURATION) == 0x20,
              "D3D12XBOX_VIRTUAL_MEMORY_CONFIGURATION must be 32 bytes");

HRESULT WINAPI D3D12ConfigureVirtualMemoryX(
    D3D12XBOX_VIRTUAL_MEMORY_CONFIGURATION* pConfig);

// Zero-Copy Existing-Heap GPU Aliasing (Xbox2Windows era_gpu_alias architecture):
// Wraps a 64-KiB-aligned VirtualAlloc/XMemAlloc CPU region directly into a desktop ID3D12Heap + placed raw buffer
// via ID3D12Device3::OpenExistingHeapFromAddress so CPU and GPU share physical RAM with zero copies.
HRESULT WINAPI D3D12XboxOpenExistingHeapAlias(
    ID3D12Device* pDevice,
    void* pCpuBase,
    SIZE_T sizeInBytes,
    ID3D12Heap** ppOutHeap,
    ID3D12Resource** ppOutResource,
    D3D12_GPU_VIRTUAL_ADDRESS* pOutDesktopGpuVa);

// Extract all tag-0x0D XBCH -> FSSM RDNA2 payloads from a DXBC container (supports dual-0x0D $VS_* VoxelizeGeneric containers).
HRESULT WINAPI D3D12XboxUnpackAllShaderFssm(
    const void* pDxbc,
    size_t dxbcSize,
    uint32_t payloadIndex,
    void* pOutFssmBuffer,
    size_t fssmBufferCapacity,
    size_t* pOutFssmSize,
    uint32_t* pOutTotalPayloads);

// Retarget Xbox DXIL triple ("dxil-ms-xb2" -> "dxil-ms-dx") and recompute DXBC container MD5 hash.
HRESULT WINAPI D3D12XboxRetargetDxilTriple(
    const void* pInDxbc,
    size_t inDxbcSize,
    void* pOutDxbc,
    size_t outCapacity,
    size_t* pOutSize,
    UINT* pOutTriplesPatched);

// Remap 16-core Zen 2 (Xbox Series X|S) thread affinity masks (0..15) onto valid host CPU logical cores.
// Rejects 0 or masks with bits outside the 16 console cores (and outside host cores) with ERROR_INVALID_PARAMETER.
ULONG_PTR WINAPI D3D12XboxRemapThreadAffinityMask(ULONG_PTR xboxAffinityMask);

// Unwraps WsxXboxDeviceAbiProxy / WsxD3D12DeviceProxy to the raw desktop ID3D12Device and calls
// IMFDXGIDeviceManager::ResetDevice (slot 6) for Xbox Media Foundation video playback.
HRESULT WINAPI MFResetDXGIDeviceManagerX(
    IUnknown* pManager,
    IUnknown* pUnkDevice,
    UINT resetToken);

// Console d3d12_x / d3d12_xs ESRAM memory mapper: accepts Flags 1 or 3 and commits NumPages * 64 KiB at VirtualAddress.
HRESULT WINAPI D3DMapEsramMemory(
    UINT Flags,
    void* VirtualAddress,
    UINT NumPages,
    const UINT* PageList);
}

// Thin zero-overhead C++ extensions on ID3D12Device8 and ID3D12CommandQueue
// that preserve the exact desktop COM vtable (backed by WsxD3D12DeviceProxy in d3d12_xs.dll)
// while routing Xbox "*X" methods into d3d12_xs.dll.
struct __declspec(uuid("9218e6bb-f944-4f7e-a75c-b1b2c7b701f3")) WsxID3D12Device8 : ::ID3D12Device8 {
    HRESULT SetFrameIntervalX(IDXGIOutput* pOutput, UINT Interval, UINT MaxLatency, UINT Flags) {
        return D3D12XboxSetFrameIntervalX(this, pOutput, Interval, MaxLatency, Flags);
    }
    HRESULT ScheduleFrameEventX(UINT EventType, UINT64 Time, void* pData, UINT Flags) {
        return D3D12XboxScheduleFrameEventX(this, EventType, Time, pData, Flags);
    }
    HRESULT WaitFrameEventX(UINT EventType, DWORD TimeoutMs, void* pData, UINT Flags,
                            D3D12XBOX_FRAME_PIPELINE_TOKEN* pToken) {
        return D3D12XboxWaitFrameEventX(this, EventType, TimeoutMs, pData, Flags, pToken);
    }
};

struct __declspec(uuid("0ec870a6-5d7e-4c22-8cfc-5baae07616ed")) WsxID3D12CommandQueue : ::ID3D12CommandQueue {
    HRESULT PresentX(UINT PlaneCount,
                     const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters,
                     const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters) {
        return D3D12XboxPresentX(this, PlaneCount, pPlaneParameters, pPresentParameters);
    }
    HRESULT SuspendX(UINT Flags) {
        return D3D12XboxSuspendCommandQueueX(this, Flags);
    }
    HRESULT ResumeX() {
        return D3D12XboxResumeCommandQueueX(this);
    }
};

template <typename T>
inline ID3D12CommandList* const* CommandListCast(T* const* pp) noexcept {
    return reinterpret_cast<ID3D12CommandList* const*>(pp);
}

#define ID3D12Device8 WsxID3D12Device8
#define ID3D12Device2 WsxID3D12Device8
#define ID3D12CommandQueue WsxID3D12CommandQueue
