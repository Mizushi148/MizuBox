// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox Series X|S DirectStorage runtime (dstorage_xs.dll) public API.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d12.h>
#include <cstdint>

enum DSTORAGE_PRIORITY : INT8 {
    DSTORAGE_PRIORITY_LOW = -1,
    DSTORAGE_PRIORITY_NORMAL = 0,
    DSTORAGE_PRIORITY_HIGH = 1,
    DSTORAGE_PRIORITY_REALTIME = 2,
    DSTORAGE_PRIORITY_FIRST = DSTORAGE_PRIORITY_LOW,
    DSTORAGE_PRIORITY_LAST = DSTORAGE_PRIORITY_REALTIME,
    DSTORAGE_PRIORITY_COUNT = 4
};

constexpr UINT32 DSTORAGE_MIN_QUEUE_CAPACITY = 128;
constexpr UINT32 DSTORAGE_MAX_QUEUE_CAPACITY = 2000;

enum DSTORAGE_REQUEST_SOURCE_TYPE : UINT64 {
    DSTORAGE_REQUEST_SOURCE_FILE = 0,
    DSTORAGE_REQUEST_SOURCE_MEMORY = 1
};

enum DSTORAGE_REQUEST_DESTINATION_TYPE : UINT64 {
    DSTORAGE_REQUEST_DESTINATION_MEMORY = 0,
    DSTORAGE_REQUEST_DESTINATION_BUFFER = 1,
    DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION = 2,
    DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES = 3,
    DSTORAGE_REQUEST_DESTINATION_TILES = 4
};

enum DSTORAGE_COMPRESSION_FORMAT : UINT8 {
    DSTORAGE_COMPRESSION_FORMAT_NONE = 0,
    DSTORAGE_COMPRESSION_FORMAT_ZLIB = 1,
    DSTORAGE_COMPRESSION_FORMAT_GDEFLATE = 2,
    DSTORAGE_COMPRESSION_FORMAT_RAW_DEFLATE = 3,
    DSTORAGE_COMPRESSION_FORMAT_BCPACK = 4,
    DSTORAGE_CUSTOM_DECOMPRESSION_FLAG = 0x80
};

enum DSTORAGE_COMMAND_TYPE : INT32 {
    DSTORAGE_COMMAND_TYPE_NONE = -1,
    DSTORAGE_COMMAND_TYPE_REQUEST = 0,
    DSTORAGE_COMMAND_TYPE_STATUS = 1,
    DSTORAGE_COMMAND_TYPE_SIGNAL = 2,
    DSTORAGE_COMMAND_TYPE_EVENT = 3
};

struct DSTORAGE_REQUEST_OPTIONS {
    DSTORAGE_COMPRESSION_FORMAT CompressionFormat : 8;
    UINT64 SourceType : 1;
    UINT64 DestinationType : 7;
    UINT64 Reserved : 48;
};

struct IDStorageFileX;

struct DSTORAGE_SOURCE_FILE {
    IDStorageFileX* Source;
    UINT64 Offset;
    UINT32 Size;
};

struct DSTORAGE_SOURCE_MEMORY {
    void const* Source;
    UINT32 Size;
};

union DSTORAGE_SOURCE {
    DSTORAGE_SOURCE_MEMORY Memory;
    DSTORAGE_SOURCE_FILE File;
};

struct DSTORAGE_DESTINATION_MEMORY {
    void* Buffer;
    UINT32 Size;
};

struct DSTORAGE_DESTINATION_BUFFER {
    ID3D12Resource* Resource;
    UINT64 Offset;
    UINT32 Size;
};

struct DSTORAGE_DESTINATION_TEXTURE_REGION {
    ID3D12Resource* Resource;
    UINT SubresourceIndex;
    D3D12_BOX Region;
};

struct DSTORAGE_DESTINATION_MULTIPLE_SUBRESOURCES {
    ID3D12Resource* Resource;
    UINT FirstSubresource;
};

union DSTORAGE_DESTINATION {
    DSTORAGE_DESTINATION_MEMORY Memory;
    DSTORAGE_DESTINATION_BUFFER Buffer;
    DSTORAGE_DESTINATION_TEXTURE_REGION Texture;
    DSTORAGE_DESTINATION_MULTIPLE_SUBRESOURCES MultipleSubresources;
};

struct DSTORAGE_REQUEST {
    DSTORAGE_REQUEST_OPTIONS Options;
    DSTORAGE_SOURCE Source;
    DSTORAGE_DESTINATION Destination;
    UINT32 UncompressedSize;
    UINT64 CancellationTag;
    const CHAR* Name;
};

struct DSTORAGE_QUEUE_DESC {
    DSTORAGE_REQUEST_SOURCE_TYPE SourceType;
    UINT16 Capacity;
    DSTORAGE_PRIORITY Priority;
    const CHAR* Name;
    ID3D12Device* Device;
};

struct DSTORAGE_QUEUE_INFO {
    DSTORAGE_QUEUE_DESC Desc;
    UINT16 EmptySlotCount;
    UINT16 RequestCountUntilAutoSubmit;
};

struct DSTORAGE_ERROR_PARAMETERS_REQUEST {
    WCHAR Filename[MAX_PATH];
    CHAR RequestName[64];
    DSTORAGE_REQUEST Request;
};

struct DSTORAGE_ERROR_FIRST_FAILURE {
    HRESULT HResult;
    DSTORAGE_COMMAND_TYPE CommandType;
    DSTORAGE_ERROR_PARAMETERS_REQUEST Request;
};

struct DSTORAGE_ERROR_RECORD {
    UINT32 FailureCount;
    DSTORAGE_ERROR_FIRST_FAILURE FirstFailure;
};

struct DSTORAGE_CONFIGURATION {
    UINT32 NumSubmitThreads;
    INT32 NumBuiltInCpuDecompressionThreads;
    BOOL ForceMappingLayer;
    BOOL DisableBypassIO;
    BOOL DisableTelemetry;
    BOOL DisableGpuDecompressionMetacommand;
    BOOL DisableGpuDecompression;
};

struct DSTORAGE_CONFIGURATION1 {
    UINT32 NumSubmitThreads;
    INT32 NumBuiltInCpuDecompressionThreads;
    BOOL ForceMappingLayer;
    BOOL DisableBypassIO;
    BOOL DisableTelemetry;
    BOOL DisableGpuDecompressionMetacommand;
    BOOL DisableGpuDecompression;
    BOOL ForceFileBuffering;
};

struct __declspec(uuid("5de95e7b-955a-4868-a73c-243b29f4b8da")) IDStorageFileX : public IUnknown {
    virtual void STDMETHODCALLTYPE Close() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFileInformation(BY_HANDLE_FILE_INFORMATION* info) = 0;
};

struct __declspec(uuid("82397587-7cd5-453b-a02e-31379bd64656")) IDStorageStatusArrayX : public IUnknown {
    virtual bool STDMETHODCALLTYPE IsComplete(UINT32 index) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetHResult(UINT32 index) = 0;
};

struct __declspec(uuid("cfdbd83f-9e06-4fda-8ea5-69042137f49b")) IDStorageQueueX : public IUnknown {
    virtual void STDMETHODCALLTYPE EnqueueRequest(const DSTORAGE_REQUEST* request) = 0;
    virtual void STDMETHODCALLTYPE EnqueueStatus(IDStorageStatusArrayX* statusArray, UINT32 index) = 0;
    virtual void STDMETHODCALLTYPE EnqueueSignal(ID3D12Fence* fence, UINT64 value) = 0;
    virtual void STDMETHODCALLTYPE EnqueueSetEvent(HANDLE handle) = 0;
    virtual void STDMETHODCALLTYPE Submit() = 0;
    virtual void STDMETHODCALLTYPE CancelRequestsWithTag(UINT64 mask, UINT64 value) = 0;
    virtual void STDMETHODCALLTYPE Close() = 0;
    virtual HANDLE STDMETHODCALLTYPE GetErrorEvent() = 0;
    virtual void STDMETHODCALLTYPE RetrieveErrorRecord(DSTORAGE_ERROR_RECORD* record) = 0;
    virtual void STDMETHODCALLTYPE Query(DSTORAGE_QUEUE_INFO* info) = 0;
};

struct __declspec(uuid("6924ea0c-c3cd-4826-b10a-f64f4ed927c1")) IDStorageFactoryX : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateQueue(const DSTORAGE_QUEUE_DESC* desc, REFIID riid, void** ppv) = 0;
    virtual HRESULT STDMETHODCALLTYPE OpenFile(const WCHAR* path, REFIID riid, void** ppv) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateStatusArray(UINT32 capacity, const CHAR* name, REFIID riid, void** ppv) = 0;
    virtual void STDMETHODCALLTYPE SetDebugFlags(UINT32 flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetStagingBufferSize(UINT32 size) = 0;
};

extern "C" {

HRESULT WINAPI DStorageGetFactory(REFIID riid, void** ppv);
HRESULT WINAPI DStorageCreateFactory(REFIID riid, void** ppv);
HRESULT WINAPI DStorageSetConfiguration(const DSTORAGE_CONFIGURATION* configuration);
HRESULT WINAPI DStorageSetConfiguration1(const DSTORAGE_CONFIGURATION1* configuration);
HRESULT WINAPI DStorageOpenXvdVolume(const WCHAR* xvdPath, REFIID riid, void** ppv);
HRESULT WINAPI DStorageDecompressGDeflate(
    const void* src,
    UINT32 srcSize,
    void* dst,
    UINT32 dstSize,
    UINT32* outDecompressedSize);

HRESULT WINAPI DStorageDecompressGDeflateGpu(
    ID3D12Device* device,
    const void* src,
    UINT32 srcSize,
    void* dst,
    UINT32 dstSize,
    UINT32* outDecompressedSize,
    BOOL* outUsedGpu);

HRESULT WINAPI DStorageCompressBCPack(
    const void* src,
    UINT32 srcSize,
    DXGI_FORMAT bcFormat,
    void* dst,
    UINT32 dstSize,
    UINT32* outCompressedSize);

HRESULT WINAPI DStorageDecompressBCPack(
    const void* src,
    UINT32 srcSize,
    DXGI_FORMAT bcFormat,
    void* dst,
    UINT32 dstSize,
    UINT32* outDecompressedSize,
    const void* optionalResidentMip,
    UINT32 residentMipWidth,
    UINT32 residentMipHeight,
    UINT32 targetWidth,
    UINT32 targetHeight,
    BOOL* outUsedMipFallback);

HRESULT WINAPI DStorageDecompressBCPackGpu(
    ID3D12Device* device,
    const void* src,
    UINT32 srcSize,
    DXGI_FORMAT bcFormat,
    void* dst,
    UINT32 dstSize,
    UINT32* outDecompressedSize,
    BOOL* outUsedGpu);

} // extern "C"
