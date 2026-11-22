// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox Series X|S xg_xs.dll public ABI header (texture swizzle/tiling layout calculator).

#pragma once

#include <Windows.h>
#include <unknwn.h>
#include <dxgiformat.h>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XG_SWIZZLE_MODE {
    XG_SWIZZLE_MODE_LINEAR   = 0,
    XG_SWIZZLE_MODE_256B_S   = 1,
    XG_SWIZZLE_MODE_256B_D   = 2,
    XG_SWIZZLE_MODE_4KB_Z    = 4,
    XG_SWIZZLE_MODE_4KB_S    = 5,
    XG_SWIZZLE_MODE_4KB_D    = 6,
    XG_SWIZZLE_MODE_64KB_Z   = 8,
    XG_SWIZZLE_MODE_64KB_S   = 9,
    XG_SWIZZLE_MODE_64KB_D   = 10,
    XG_SWIZZLE_MODE_64KB_Z_X = 24,
    XG_SWIZZLE_MODE_64KB_S_X = 25,
    XG_SWIZZLE_MODE_64KB_D_X = 26,
} XG_SWIZZLE_MODE;

typedef struct XG_TEXTURE2D_DESC {
    UINT Width;
    UINT Height;
    UINT MipLevels;
    UINT ArraySize;
    DXGI_FORMAT Format;
    UINT SampleCount;
    UINT SampleQuality;
    UINT Usage;
    UINT BindFlags;
    UINT CPUAccessFlags;
    UINT MiscFlags;
    UINT SwizzleMode;
    UINT Pitch;
} XG_TEXTURE2D_DESC;

typedef struct XG_TEXTURE1D_DESC {
    UINT Width;
    UINT MipLevels;
    UINT ArraySize;
    DXGI_FORMAT Format;
    UINT SwizzleMode;
} XG_TEXTURE1D_DESC;

typedef struct XG_TEXTURE3D_DESC {
    UINT Width;
    UINT Height;
    UINT Depth;
    UINT MipLevels;
    DXGI_FORMAT Format;
    UINT SwizzleMode;
} XG_TEXTURE3D_DESC;

typedef struct XG_BUFFER_DESC {
    UINT64 SizeBytes;
    UINT Flags;
} XG_BUFFER_DESC;

typedef struct XG_PLANE_LAYOUT {
    UINT64 BaseOffsetBytes;
    UINT64 SizeBytes;
    UINT64 BaseAlignmentBytes;
    UINT BytesPerElement;
    UINT MipOffsetBytes[15];
    UINT MipSizeBytes[15];
    UINT MipRowPitchBytes[15];
    UINT MipSlicePitchBytes[15];
} XG_PLANE_LAYOUT;

typedef struct XG_RESOURCE_LAYOUT {
    UINT64 SizeBytes;
    UINT64 BaseAlignmentBytes;
    UINT Planes;
    UINT MipLevels;
    UINT ArraySize;
    UINT SwizzleMode;
    XG_PLANE_LAYOUT Plane[2];
} XG_RESOURCE_LAYOUT;

#ifdef __cplusplus
} // extern "C"

struct __declspec(novtable) IXGTextureAddressComputer : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetResourceLayout(XG_RESOURCE_LAYOUT* pLayout) = 0;
    virtual UINT64  STDMETHODCALLTYPE GetResourceSizeBytes() = 0;
    virtual UINT64  STDMETHODCALLTYPE GetResourceBaseAlignmentBytes() = 0;
    virtual UINT64  STDMETHODCALLTYPE GetMipLevelOffsetBytes(UINT plane, UINT mipLevel) = 0;
    virtual UINT64  STDMETHODCALLTYPE GetTexelElementOffsetBytes(
        UINT plane, UINT mipLevel, UINT64 x, UINT y, UINT zOrSlice, UINT sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE CopyIntoSubresource(
        void* pDstTiledBase, UINT plane, UINT subresource,
        const void* pSrcLinear, UINT srcRowPitch, UINT srcSlicePitch) = 0;
    virtual HRESULT STDMETHODCALLTYPE CopyFromSubresource(
        void* pDstLinear, UINT dstRowPitch, UINT dstSlicePitch,
        const void* pSrcTiledBase, UINT plane, UINT subresource) = 0;
};

extern "C" {
#endif

HRESULT WINAPI XGCreateTexture1DComputer(const XG_TEXTURE1D_DESC* pDesc, void** ppComputer);
HRESULT WINAPI XGCreateTexture2DComputer(const XG_TEXTURE2D_DESC* pDesc, void** ppComputer);
HRESULT WINAPI XGCreateTexture3DComputer(const XG_TEXTURE3D_DESC* pDesc, void** ppComputer);
HRESULT WINAPI XGCreateBufferComputer(const XG_BUFFER_DESC* pDesc, void** ppComputer);
UINT    WINAPI XGComputeOptimalSwizzleMode(UINT dimension, DXGI_FORMAT format, UINT width, UINT height, UINT depth, UINT bindFlags);
UINT    WINAPI XGComputeOptimalTileMode(UINT dimension, DXGI_FORMAT format, UINT width, UINT height, UINT depth, UINT bindFlags);
UINT    WINAPI XGGetFormatBitsPerPixel(DXGI_FORMAT format);
BOOL    WINAPI XGIsBlockCompressedFormat(DXGI_FORMAT format);
const char* WINAPI XGGetSwizzleModeString(UINT swizzleMode);

#ifdef __cplusplus
}
#endif
