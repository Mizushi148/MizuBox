// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room unencrypted Xbox Virtual Disk (.xvd) container header parser & reader.
// STRICTLY supports unencrypted developer/loose XVD volumes (EncryptionDisabled flag set).
// Retail encrypted XVC/XVD volumes are cleanly rejected with XvdStatus::EncryptedUnsupported
// without attempting any DRM decryption or key extraction.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wsx_xvd {

constexpr uint32_t kPageSize = 0x1000u;     // 4 KiB XVD page
constexpr uint32_t kHeaderSize = 0x3000u;   // 12 KiB XVD header
constexpr uint32_t kMagicOffset = 0x200u;

constexpr uint32_t kVolFlagReadOnly              = 0x0001u;
constexpr uint32_t kVolFlagEncryptionDisabled    = 0x0002u;
constexpr uint32_t kVolFlagDataIntegrityDisabled = 0x0004u;
constexpr uint32_t kVolFlagLegacySectorSize      = 0x0008u;

enum class XvdStatus : uint32_t {
    Ok = 0,
    IoError = 1,
    InvalidHeader = 2,
    EncryptedUnsupported = 3,
    DynamicUnsupported = 4,
    OutOfRange = 5
};

struct XvdInfo {
    uint32_t volume_flags = 0;
    uint32_t format_version = 0;
    uint64_t drive_size = 0;
    uint32_t xvd_type = 0;       // 0 = Fixed, 1 = Dynamic
    uint32_t content_type = 0;
    uint32_t embedded_xvd_length = 0;
    uint32_t user_data_length = 0;
    uint32_t xvc_data_length = 0;
    uint32_t mutable_data_pages = 0;
    uint64_t user_data_offset = 0;
    uint64_t hash_tree_bytes = 0;
    std::array<uint8_t, 16> vduid{};
    std::array<uint8_t, 16> uduid{};

    bool is_encrypted() const noexcept {
        return (volume_flags & kVolFlagEncryptionDisabled) == 0u;
    }

    bool has_data_integrity() const noexcept {
        return (volume_flags & kVolFlagDataIntegrityDisabled) == 0u;
    }
};

namespace detail {

inline uint32_t read_u32_le(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t read_u64_le(const uint8_t* p) noexcept {
    return static_cast<uint64_t>(read_u32_le(p)) |
           (static_cast<uint64_t>(read_u32_le(p + 4)) << 32);
}

inline void write_u32_le(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

inline void write_u64_le(uint8_t* p, uint64_t v) noexcept {
    write_u32_le(p, static_cast<uint32_t>(v & 0xFFFFFFFFULL));
    write_u32_le(p + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFULL));
}

inline uint64_t align_up_u64(uint64_t v, uint64_t a) noexcept {
    return (v + a - 1u) & ~(a - 1u);
}

// Compute XVD integrity hash-tree byte length (170 x 24-byte hashes fit per 4 KiB hash page)
inline uint64_t compute_hash_tree_bytes(uint64_t data_pages) noexcept {
    constexpr uint64_t kHashesPerPage = 170u; // floor(4096 / 24)
    if (data_pages == 0u) return 0u;
    uint64_t total_hash_pages = 0u;
    uint64_t cur = data_pages;
    for (int level = 0; level < 4; ++level) {
        const uint64_t pages = (cur + kHashesPerPage - 1u) / kHashesPerPage;
        total_hash_pages += pages;
        if (pages <= 1u) break;
        cur = pages;
    }
    return total_hash_pages * kPageSize;
}

} // namespace detail

inline XvdStatus parse_header(const uint8_t* hdr, size_t hdr_size, XvdInfo& out) noexcept {
    out = {};
    if (!hdr || hdr_size < kHeaderSize) return XvdStatus::InvalidHeader;

    static constexpr char kMagic[8] = {'m', 's', 'f', 't', '-', 'x', 'v', 'd'};
    if (std::memcmp(hdr + kMagicOffset, kMagic, 8) != 0) {
        return XvdStatus::InvalidHeader;
    }

    out.volume_flags = detail::read_u32_le(hdr + 0x208u);
    out.format_version = detail::read_u32_le(hdr + 0x20Cu);
    out.drive_size = detail::read_u64_le(hdr + 0x218u);
    std::memcpy(out.vduid.data(), hdr + 0x220u, 16u);
    std::memcpy(out.uduid.data(), hdr + 0x230u, 16u);

    out.xvd_type = detail::read_u32_le(hdr + 0x3E4u);
    out.content_type = detail::read_u32_le(hdr + 0x3E8u);
    out.embedded_xvd_length = detail::read_u32_le(hdr + 0x3ECu);
    out.user_data_length = detail::read_u32_le(hdr + 0x3F0u);
    out.xvc_data_length = detail::read_u32_le(hdr + 0x3F4u);
    out.mutable_data_pages = detail::read_u32_le(hdr + 0x3FCu);

    if (out.format_version == 0u || out.format_version > 16u || out.drive_size == 0u) {
        return XvdStatus::InvalidHeader;
    }
    if (out.is_encrypted()) {
        return XvdStatus::EncryptedUnsupported;
    }
    if (out.xvd_type != 0u) {
        return XvdStatus::DynamicUnsupported;
    }

    const uint64_t embedded_bytes = detail::align_up_u64(out.embedded_xvd_length, kPageSize);
    const uint64_t user_meta_bytes = detail::align_up_u64(out.user_data_length, kPageSize);
    const uint64_t mutable_bytes = static_cast<uint64_t>(out.mutable_data_pages) * kPageSize;
    const uint64_t total_hashed_pages =
        (detail::align_up_u64(out.drive_size, kPageSize) + user_meta_bytes) / kPageSize;
    out.hash_tree_bytes = out.has_data_integrity()
        ? detail::compute_hash_tree_bytes(total_hashed_pages)
        : 0u;

    out.user_data_offset = kHeaderSize + embedded_bytes + mutable_bytes + out.hash_tree_bytes + user_meta_bytes;
    return XvdStatus::Ok;
}

class XvdVolumeReader {
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::wstring path_;
    XvdInfo info_{};
    uint64_t file_size_ = 0;

public:
    XvdVolumeReader() = default;
    ~XvdVolumeReader() { close(); }

    XvdVolumeReader(const XvdVolumeReader&) = delete;
    XvdVolumeReader& operator=(const XvdVolumeReader&) = delete;

    XvdStatus open(const std::wstring& path) noexcept {
        close();
        HANDLE h = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
            nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return XvdStatus::IoError;
        }

        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(h, &sz) || sz.QuadPart < static_cast<LONGLONG>(kHeaderSize)) {
            CloseHandle(h);
            return XvdStatus::InvalidHeader;
        }

        std::vector<uint8_t> hdr(kHeaderSize, 0u);
        DWORD read_bytes = 0;
        if (!ReadFile(h, hdr.data(), kHeaderSize, &read_bytes, nullptr) || read_bytes < kHeaderSize) {
            CloseHandle(h);
            return XvdStatus::IoError;
        }

        XvdInfo parsed{};
        const XvdStatus st = parse_header(hdr.data(), hdr.size(), parsed);
        if (st != XvdStatus::Ok) {
            CloseHandle(h);
            return st;
        }

        if (parsed.user_data_offset > static_cast<uint64_t>(sz.QuadPart)) {
            CloseHandle(h);
            return XvdStatus::InvalidHeader;
        }

        handle_ = h;
        path_ = path;
        info_ = parsed;
        file_size_ = static_cast<uint64_t>(sz.QuadPart);
        return XvdStatus::Ok;
    }

    void close() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        info_ = {};
        file_size_ = 0;
    }

    bool is_open() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
    const XvdInfo& info() const noexcept { return info_; }
    const std::wstring& path() const noexcept { return path_; }
    HANDLE native_handle() const noexcept { return handle_; }

    uint64_t readable_user_data_size() const noexcept {
        if (!is_open() || file_size_ < info_.user_data_offset) return 0u;
        const uint64_t avail = file_size_ - info_.user_data_offset;
        return (std::min)(info_.drive_size, avail);
    }

    XvdStatus read_user_data(uint64_t offset, void* dst, uint32_t size, DWORD& out_read) const noexcept {
        out_read = 0;
        if (!is_open()) return XvdStatus::IoError;
        if (size == 0u) return XvdStatus::Ok;
        if (!dst) return XvdStatus::OutOfRange;

        const uint64_t max_bytes = readable_user_data_size();
        if (offset > max_bytes || static_cast<uint64_t>(size) > (max_bytes - offset)) {
            return XvdStatus::OutOfRange;
        }

        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(info_.user_data_offset + offset);
        if (!SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) {
            return XvdStatus::IoError;
        }

        uint8_t* ptr = static_cast<uint8_t*>(dst);
        uint32_t rem = size;
        while (rem > 0u) {
            DWORD chunk = 0;
            if (!ReadFile(handle_, ptr, rem, &chunk, nullptr)) {
                return XvdStatus::IoError;
            }
            if (chunk == 0u) return XvdStatus::OutOfRange;
            ptr += chunk;
            rem -= chunk;
            out_read += chunk;
        }
        return XvdStatus::Ok;
    }
};

// Clean-room synthetic unencrypted XVD image builder for unit & integration tests
inline std::vector<uint8_t> build_synthetic_xvd(
    const uint8_t* user_payload,
    size_t payload_size,
    bool mark_encrypted = false,
    bool include_integrity_padding = false) {
    const uint64_t drive_size = detail::align_up_u64((std::max<size_t>)(payload_size, 1u), kPageSize);
    uint32_t flags = kVolFlagReadOnly;
    if (!mark_encrypted) flags |= kVolFlagEncryptionDisabled;
    if (!include_integrity_padding) flags |= kVolFlagDataIntegrityDisabled;

    const uint64_t hash_bytes = include_integrity_padding
        ? detail::compute_hash_tree_bytes(drive_size / kPageSize)
        : 0u;
    const uint64_t user_off = kHeaderSize + hash_bytes;

    std::vector<uint8_t> img(static_cast<size_t>(user_off + drive_size), 0u);
    static constexpr char kMagic[8] = {'m', 's', 'f', 't', '-', 'x', 'v', 'd'};
    std::memcpy(img.data() + kMagicOffset, kMagic, 8u);
    detail::write_u32_le(img.data() + 0x208u, flags);
    detail::write_u32_le(img.data() + 0x20Cu, 3u); // format_version = 3
    detail::write_u64_le(img.data() + 0x218u, drive_size);
    detail::write_u32_le(img.data() + 0x3E4u, 0u); // Fixed XVD
    detail::write_u32_le(img.data() + 0x3E8u, 1u); // Title content

    if (user_payload && payload_size > 0u) {
        std::memcpy(img.data() + user_off, user_payload, payload_size);
    }
    return img;
}

} // namespace wsx_xvd
