// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox Series X|S xg_xs.dll implementation (texture tiling/swizzle calculator).

#include "include/xbox/xg_xs.h"
#include "wsx_xg_layout.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace {

class WsxXgTextureComputer final : public IXGTextureAddressComputer {
    std::atomic<ULONG> refs_{1};
    XG_RESOURCE_LAYOUT layout_{};
    std::vector<wsx_xg::SubresourceLayout> mips_;
    std::vector<wsx_xg::SubresourceLayout3D> mips3d_;
    uint32_t array_size_ = 1u;
    uint32_t depth_ = 1u;
    uint32_t sample_count_ = 1u;
    bool is_3d_ = false;

public:
    WsxXgTextureComputer(uint32_t width, uint32_t height, uint32_t depth,
                         uint32_t mip_levels, uint32_t array_size,
                         DXGI_FORMAT fmt, uint32_t swizzle_mode,
                         uint32_t sample_count = 1u, bool is_3d = false)
        : array_size_(std::max(1u, array_size)),
          depth_(std::max(1u, depth)),
          sample_count_(std::clamp(sample_count ? sample_count : 1u, 1u, 8u)),
          is_3d_(is_3d) {
        const uint32_t mips = std::clamp(mip_levels ? mip_levels : 1u, 1u, 15u);
        const wsx_xg::FormatInfo fi = wsx_xg::get_format_info(fmt);
        const uint64_t base_align = wsx_xg::is_64kb_mode(swizzle_mode) ? 65536u
                                  : (wsx_xg::is_4kb_mode(swizzle_mode) ? 4096u : 256u);

        layout_.Planes = 1u;
        layout_.MipLevels = mips;
        layout_.ArraySize = array_size_;
        layout_.SwizzleMode = swizzle_mode;
        layout_.BaseAlignmentBytes = base_align;
        layout_.Plane[0].BaseOffsetBytes = 0u;
        layout_.Plane[0].BaseAlignmentBytes = base_align;
        layout_.Plane[0].BytesPerElement = fi.bytes_per_element;

        mips_.resize(mips);
        if (is_3d_) mips3d_.resize(mips);
        uint64_t cur = 0u;
        for (uint32_t m = 0; m < mips; ++m) {
            if (is_3d_) {
                mips3d_[m] = wsx_xg::compute_subresource_3d(width, height, depth_, m, fmt, swizzle_mode, cur);
                layout_.Plane[0].MipOffsetBytes[m] = static_cast<UINT>(mips3d_[m].offset_bytes);
                layout_.Plane[0].MipRowPitchBytes[m] = mips3d_[m].row_pitch_bytes;
                layout_.Plane[0].MipSlicePitchBytes[m] = mips3d_[m].slice_pitch_bytes;
                layout_.Plane[0].MipSizeBytes[m] = static_cast<UINT>(mips3d_[m].size_bytes);
                cur = mips3d_[m].offset_bytes + mips3d_[m].size_bytes;
            } else {
                mips_[m] = wsx_xg::compute_subresource_2d(width, height, m, fmt, swizzle_mode, cur);
                const uint64_t level_total = wsx_xg::align_up(mips_[m].size_bytes * sample_count_, base_align);
                layout_.Plane[0].MipOffsetBytes[m] = static_cast<UINT>(mips_[m].offset_bytes);
                layout_.Plane[0].MipRowPitchBytes[m] = mips_[m].row_pitch_bytes;
                layout_.Plane[0].MipSlicePitchBytes[m] = static_cast<UINT>(level_total);
                layout_.Plane[0].MipSizeBytes[m] = static_cast<UINT>(level_total);
                cur = mips_[m].offset_bytes + level_total;
            }
        }
        const uint64_t slice_total = wsx_xg::align_up(cur, base_align);
        layout_.Plane[0].SizeBytes = slice_total * array_size_;
        layout_.SizeBytes = layout_.Plane[0].SizeBytes;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown) {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE GetResourceLayout(XG_RESOURCE_LAYOUT* pLayout) override {
        if (!pLayout) return E_POINTER;
        *pLayout = layout_;
        return S_OK;
    }
    UINT64 STDMETHODCALLTYPE GetResourceSizeBytes() override { return layout_.SizeBytes; }
    UINT64 STDMETHODCALLTYPE GetResourceBaseAlignmentBytes() override { return layout_.BaseAlignmentBytes; }

    UINT64 STDMETHODCALLTYPE GetMipLevelOffsetBytes(UINT plane, UINT mipLevel) override {
        if (plane > 0u || mipLevel >= layout_.MipLevels) return 0u;
        return is_3d_ ? mips3d_[mipLevel].offset_bytes : mips_[mipLevel].offset_bytes;
    }

    UINT64 STDMETHODCALLTYPE GetTexelElementOffsetBytes(
        UINT plane, UINT mipLevel, UINT64 x, UINT y, UINT zOrSlice, UINT sample) override {
        if (plane > 0u || mipLevel >= layout_.MipLevels) return 0u;
        if (is_3d_) {
            const auto& sl3 = mips3d_[mipLevel];
            return sl3.offset_bytes +
                   wsx_xg::element_offset_3d(sl3, static_cast<uint32_t>(x), static_cast<uint32_t>(y), zOrSlice);
        }
        const auto& sl = mips_[mipLevel];
        const uint64_t slice_stride = layout_.Plane[0].SizeBytes / std::max(1u, array_size_);
        const uint64_t slice_base = static_cast<uint64_t>(zOrSlice) * slice_stride;
        return slice_base + sl.offset_bytes +
               wsx_xg::element_offset_msaa(sl, static_cast<uint32_t>(x), static_cast<uint32_t>(y), sample, sample_count_);
    }

    HRESULT STDMETHODCALLTYPE CopyIntoSubresource(
        void* pDstTiledBase, UINT plane, UINT subresource,
        const void* pSrcLinear, UINT srcRowPitch, UINT srcSlicePitch) override {
        if (!pDstTiledBase || !pSrcLinear || plane > 0u) return E_INVALIDARG;
        const uint32_t mips_cnt = layout_.MipLevels;
        const uint32_t mip = subresource % mips_cnt;
        const uint32_t slice = subresource / mips_cnt;
        if (is_3d_) {
            if (slice > 0u) return E_INVALIDARG;
            auto* dst = static_cast<uint8_t*>(pDstTiledBase) + mips3d_[mip].offset_bytes;
            const uint32_t eff_slice_pitch = srcSlicePitch ? srcSlicePitch : (srcRowPitch * mips3d_[mip].height_elems);
            wsx_xg::swizzle_3d(mips3d_[mip], static_cast<const uint8_t*>(pSrcLinear), srcRowPitch, eff_slice_pitch, dst);
            return S_OK;
        }
        if (slice >= array_size_) return E_INVALIDARG;
        const uint64_t slice_stride = layout_.Plane[0].SizeBytes / std::max(1u, array_size_);
        auto* dst = static_cast<uint8_t*>(pDstTiledBase) + slice * slice_stride + mips_[mip].offset_bytes;
        wsx_xg::swizzle_2d(mips_[mip], static_cast<const uint8_t*>(pSrcLinear), srcRowPitch, dst);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CopyFromSubresource(
        void* pDstLinear, UINT dstRowPitch, UINT dstSlicePitch,
        const void* pSrcTiledBase, UINT plane, UINT subresource) override {
        if (!pDstLinear || !pSrcTiledBase || plane > 0u) return E_INVALIDARG;
        const uint32_t mips_cnt = layout_.MipLevels;
        const uint32_t mip = subresource % mips_cnt;
        const uint32_t slice = subresource / mips_cnt;
        if (is_3d_) {
            if (slice > 0u) return E_INVALIDARG;
            const auto* src = static_cast<const uint8_t*>(pSrcTiledBase) + mips3d_[mip].offset_bytes;
            const uint32_t eff_slice_pitch = dstSlicePitch ? dstSlicePitch : (dstRowPitch * mips3d_[mip].height_elems);
            wsx_xg::deswizzle_3d(mips3d_[mip], src, static_cast<uint8_t*>(pDstLinear), dstRowPitch, eff_slice_pitch);
            return S_OK;
        }
        if (slice >= array_size_) return E_INVALIDARG;
        const uint64_t slice_stride = layout_.Plane[0].SizeBytes / std::max(1u, array_size_);
        const auto* src = static_cast<const uint8_t*>(pSrcTiledBase) + slice * slice_stride + mips_[mip].offset_bytes;
        wsx_xg::deswizzle_2d(mips_[mip], src, static_cast<uint8_t*>(pDstLinear), dstRowPitch);
        return S_OK;
    }
};

} // namespace

extern "C" {

HRESULT WINAPI XGCreateTexture1DComputer(const XG_TEXTURE1D_DESC* pDesc, void** ppComputer) {
    if (!pDesc || !ppComputer) return E_INVALIDARG;
    *ppComputer = new WsxXgTextureComputer(pDesc->Width, 1u, 1u, pDesc->MipLevels,
                                           pDesc->ArraySize, pDesc->Format, pDesc->SwizzleMode);
    return S_OK;
}

HRESULT WINAPI XGCreateTexture2DComputer(const XG_TEXTURE2D_DESC* pDesc, void** ppComputer) {
    if (!pDesc || !ppComputer) return E_INVALIDARG;
    *ppComputer = new WsxXgTextureComputer(pDesc->Width, pDesc->Height, 1u, pDesc->MipLevels,
                                           pDesc->ArraySize, pDesc->Format, pDesc->SwizzleMode,
                                           pDesc->SampleCount, false);
    return S_OK;
}

HRESULT WINAPI XGCreateTexture3DComputer(const XG_TEXTURE3D_DESC* pDesc, void** ppComputer) {
    if (!pDesc || !ppComputer) return E_INVALIDARG;
    *ppComputer = new WsxXgTextureComputer(pDesc->Width, pDesc->Height, pDesc->Depth,
                                           pDesc->MipLevels, 1u, pDesc->Format, pDesc->SwizzleMode,
                                           1u, true);
    return S_OK;
}

HRESULT WINAPI XGCreateBufferComputer(const XG_BUFFER_DESC* pDesc, void** ppComputer) {
    if (!pDesc || !ppComputer) return E_INVALIDARG;
    const uint32_t w = static_cast<uint32_t>(std::max<uint64_t>(1u, pDesc->SizeBytes));
    *ppComputer = new WsxXgTextureComputer(w, 1u, 1u, 1u, 1u, DXGI_FORMAT_R8_UINT, XG_SWIZZLE_MODE_LINEAR);
    return S_OK;
}

UINT WINAPI XGComputeOptimalSwizzleMode(UINT /*dimension*/, DXGI_FORMAT format, UINT width, UINT height, UINT /*depth*/, UINT /*bindFlags*/) {
    const auto fi = wsx_xg::get_format_info(format);
    const uint64_t raw = (static_cast<uint64_t>(width) / fi.block_dim) *
                         (static_cast<uint64_t>(height) / fi.block_dim) * fi.bytes_per_element;
    return (raw >= 65536ull) ? XG_SWIZZLE_MODE_64KB_S : XG_SWIZZLE_MODE_4KB_S;
}

UINT WINAPI XGComputeOptimalTileMode(UINT dimension, DXGI_FORMAT format, UINT width, UINT height, UINT depth, UINT bindFlags) {
    return XGComputeOptimalSwizzleMode(dimension, format, width, height, depth, bindFlags);
}

UINT WINAPI XGGetFormatBitsPerPixel(DXGI_FORMAT format) {
    const auto fi = wsx_xg::get_format_info(format);
    return fi.is_bc ? (fi.bytes_per_element * 8u / 16u) : (fi.bytes_per_element * 8u);
}

BOOL WINAPI XGIsBlockCompressedFormat(DXGI_FORMAT format) {
    return wsx_xg::get_format_info(format).is_bc ? TRUE : FALSE;
}

const char* WINAPI XGGetSwizzleModeString(UINT swizzleMode) {
    switch (swizzleMode) {
    case XG_SWIZZLE_MODE_LINEAR:   return "LINEAR";
    case XG_SWIZZLE_MODE_256B_S:   return "256B_S";
    case XG_SWIZZLE_MODE_256B_D:   return "256B_D";
    case XG_SWIZZLE_MODE_4KB_Z:    return "4KB_Z";
    case XG_SWIZZLE_MODE_4KB_S:    return "4KB_S";
    case XG_SWIZZLE_MODE_4KB_D:    return "4KB_D";
    case XG_SWIZZLE_MODE_64KB_Z:   return "64KB_Z";
    case XG_SWIZZLE_MODE_64KB_S:   return "64KB_S";
    case XG_SWIZZLE_MODE_64KB_D:   return "64KB_D";
    case XG_SWIZZLE_MODE_64KB_Z_X: return "64KB_Z_X";
    case XG_SWIZZLE_MODE_64KB_S_X: return "64KB_S_X";
    case XG_SWIZZLE_MODE_64KB_D_X: return "64KB_D_X";
    default:                       return "UNKNOWN";
    }
}

} // extern "C"
