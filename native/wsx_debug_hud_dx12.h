// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Pure Direct3D 12 SM 6.6 On-Screen Debug HUD Overlay (Zero External Dependencies):
// - Alpha-blends a crisp Xbox Scarlett Emerald performance overlay directly onto any
//   RTV-compatible backbuffer (RGBA8, BGRA8, R10G10B10A2, FP16) using a fullscreen VS+PS pass.
// - Built-in 5x7 pixel bitmap font + 60-frame frametime sparkline graph + live FPS, VRAM,
//   PSO cache, Draw/Dispatch/Barrier, UMA Write-Watch, Async Shader Queue, and XMem counters.

#pragma once

#include "d3d12_xs_sm66_runtime.h"
#include "wsx_config.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace wsx_hud {

using Microsoft::WRL::ComPtr;

// Zero-dependency clean-room lossless PNG writer (IEEE CRC32 + RFC 1950 stored zlib + Adler32)
inline bool write_png_rgb24(const std::filesystem::path& path, const uint8_t* rgba,
                            uint32_t width, uint32_t height, bool is_bgra) noexcept {
    if (!rgba || width == 0u || height == 0u) return false;
    try {
        if (path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
        }
        auto put_be32 = [](std::vector<uint8_t>& dst, uint32_t v) {
            dst.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
            dst.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
            dst.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
            dst.push_back(static_cast<uint8_t>(v & 0xFFu));
        };
        auto crc32_ieee = [](const uint8_t* p, size_t n) -> uint32_t {
            uint32_t c = 0xFFFFFFFFu;
            for (size_t i = 0; i < n; ++i) {
                c ^= p[i];
                for (int k = 0; k < 8; ++k) {
                    c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
                }
            }
            return c ^ 0xFFFFFFFFu;
        };
        auto append_chunk = [&](std::vector<uint8_t>& out, const char tag[4], const std::vector<uint8_t>& payload) {
            put_be32(out, static_cast<uint32_t>(payload.size()));
            const size_t tag_pos = out.size();
            out.insert(out.end(), tag, tag + 4);
            out.insert(out.end(), payload.begin(), payload.end());
            const uint32_t crc = crc32_ieee(out.data() + tag_pos, 4u + payload.size());
            put_be32(out, crc);
        };

        // Raw scanlines: filter byte (0 = None) + RGB24
        const size_t row_bytes = 1u + static_cast<size_t>(width) * 3u;
        std::vector<uint8_t> raw(row_bytes * height);
        for (uint32_t y = 0; y < height; ++y) {
            uint8_t* dst = raw.data() + static_cast<size_t>(y) * row_bytes;
            dst[0] = 0u;
            const uint8_t* src = rgba + static_cast<size_t>(y) * width * 4u;
            for (uint32_t x = 0; x < width; ++x) {
                const uint8_t c0 = src[x * 4u + 0u];
                const uint8_t c1 = src[x * 4u + 1u];
                const uint8_t c2 = src[x * 4u + 2u];
                dst[1u + x * 3u + 0u] = is_bgra ? c2 : c0;
                dst[1u + x * 3u + 1u] = c1;
                dst[1u + x * 3u + 2u] = is_bgra ? c0 : c2;
            }
        }

        // Wrap raw scanlines in valid RFC 1950 zlib stream using 65535-byte stored blocks + Adler32
        std::vector<uint8_t> zlib;
        zlib.reserve(raw.size() + (raw.size() / 65535u + 2u) * 5u + 8u);
        zlib.push_back(0x78u);
        zlib.push_back(0x01u);
        uint32_t a1 = 1u, a2 = 0u;
        for (size_t off = 0; off < raw.size(); ) {
            const size_t chunk = (std::min<size_t>)(65535u, raw.size() - off);
            const bool is_last = (off + chunk >= raw.size());
            zlib.push_back(is_last ? 0x01u : 0x00u);
            const uint16_t len16 = static_cast<uint16_t>(chunk);
            const uint16_t nlen16 = static_cast<uint16_t>(~len16);
            zlib.push_back(static_cast<uint8_t>(len16 & 0xFFu));
            zlib.push_back(static_cast<uint8_t>((len16 >> 8) & 0xFFu));
            zlib.push_back(static_cast<uint8_t>(nlen16 & 0xFFu));
            zlib.push_back(static_cast<uint8_t>((nlen16 >> 8) & 0xFFu));
            zlib.insert(zlib.end(), raw.data() + off, raw.data() + off + chunk);
            for (size_t k = 0; k < chunk; ++k) {
                a1 = (a1 + raw[off + k]) % 65521u;
                a2 = (a2 + a1) % 65521u;
            }
            off += chunk;
        }
        put_be32(zlib, (a2 << 16) | a1);

        std::vector<uint8_t> png;
        png.reserve(zlib.size() + 128u);
        static constexpr uint8_t kSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
        png.insert(png.end(), kSig, kSig + 8);

        std::vector<uint8_t> ihdr;
        put_be32(ihdr, width);
        put_be32(ihdr, height);
        ihdr.push_back(8u); // bit depth 8
        ihdr.push_back(2u); // color type RGB
        ihdr.push_back(0u); // compression
        ihdr.push_back(0u); // filter
        ihdr.push_back(0u); // interlace
        append_chunk(png, "IHDR", ihdr);
        append_chunk(png, "IDAT", zlib);
        append_chunk(png, "IEND", {});

        std::ofstream ofs(path, std::ios::binary);
        if (!ofs) return false;
        ofs.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        return ofs.good();
    } catch (...) {
        return false;
    }
}

struct HudMetricsSnapshot {
    float fps = 0.0f;
    float frame_ms = 0.0f;
    float one_percent_low_fps = 0.0f;
    float p99_frame_ms = 0.0f;
    float vram_used_mb = 0.0f;
    float vram_budget_mb = 0.0f;
    uint32_t vram_pressure = 0u;
    uint32_t wave64_emulated = 1u;
    uint64_t pso_hits = 0u;
    uint64_t pso_misses = 0u;
    uint64_t xmem_mb = 0u;
    uint64_t draw_calls = 0u;
    uint64_t dispatch_calls = 0u;
    uint64_t mesh_dispatches = 0u;
    uint64_t barriers_elided = 0u;
    uint64_t uma_uploads = 0u;
    uint64_t uma_ww_skips = 0u;
    uint64_t async_done = 0u;
    uint64_t async_total = 0u;
};

class WsxDebugHudOverlay {
    static constexpr uint32_t kCols = 44u;
    static constexpr uint32_t kRows = 5u;
    static constexpr uint32_t kHistory = 60u;

    // 256-byte aligned constant buffer matching HLSL cbuffer HudCB
#pragma warning(push)
#pragma warning(disable : 4324)
    struct alignas(256) HudConstantData {
        uint32_t target_w;
        uint32_t target_h;
        uint32_t panel_x;
        uint32_t panel_y;
        uint32_t panel_w;
        uint32_t panel_h;
        uint32_t history_head;
        uint32_t pressure_level;
        // 5 rows x 44 chars = 220 bytes packed into 55 uint32s (padded to 56)
        uint32_t text_packed[56];
        // 60 frame times in tenths of a millisecond (e.g. 167 = 16.7 ms)
        uint32_t history_ms10[60];
    };
#pragma warning(pop)

    std::mutex mtx_;
    bool init_attempted_ = false;
    bool available_ = false;
    ComPtr<ID3D12RootSignature> root_sig_;
    std::vector<uint8_t> vs_dxil_;
    std::vector<uint8_t> ps_dxil_;
    std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> pso_by_fmt_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12CommandAllocator> cmd_alloc_;
    ComPtr<ID3D12GraphicsCommandList> cmd_list_;
    ComPtr<ID3D12Resource> cb_buf_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    UINT64 fence_val_ = 0u;

    LARGE_INTEGER qpc_freq_{};
    LARGE_INTEGER last_present_qpc_{};
    bool has_last_qpc_ = false;
    std::array<float, kHistory> frame_ms_hist_{};
    uint32_t hist_cursor_ = 0u;
    bool prev_f11_down_ = false;
    bool prev_f12_down_ = false;
    std::atomic<bool> enabled_{false};

    // Compact 5x7 uppercase + digit + symbol glyph lookup encoded inside PS
    static constexpr char kHudVsHlsl[] = R"HLSL(
struct VSOut {
    float4 pos : SV_Position;
};

[RootSignature("RootFlags(0), CBV(b0)")]
VSOut main(uint vid : SV_VertexID) {
    float2 p = float2((vid == 2u) ? 3.0 : -1.0, (vid == 1u) ? -3.0 : 1.0);
    VSOut o;
    o.pos = float4(p, 0.0, 1.0);
    return o;
}
)HLSL";

    static constexpr char kHudPsHlsl[] = R"HLSL(
cbuffer HudCB : register(b0) {
    uint targetW;
    uint targetH;
    uint panelX;
    uint panelY;
    uint panelW;
    uint panelH;
    uint historyHead;
    uint pressureLevel;
    uint4 textPacked[14];    // 56 uint32s = 224 bytes (5 rows x 44 cols)
    uint4 historyMs10[15];   // 60 uint32s
};

uint get_text_char(uint idx) {
    uint wordIdx = idx >> 2;
    uint vecIdx = wordIdx >> 2;
    uint comp = wordIdx & 3u;
    uint4 v = textPacked[min(vecIdx, 13u)];
    uint w = (comp == 0u) ? v.x : ((comp == 1u) ? v.y : ((comp == 2u) ? v.z : v.w));
    return (w >> ((idx & 3u) * 8u)) & 0xFFu;
}

uint get_hist_ms10(uint idx) {
    uint vecIdx = idx >> 2;
    uint comp = idx & 3u;
    uint4 v = historyMs10[min(vecIdx, 14u)];
    return (comp == 0u) ? v.x : ((comp == 1u) ? v.y : ((comp == 2u) ? v.z : v.w));
}

// Verified 5x7 pixel bitmap font packed into 35 bits -> uint2(low 20 bits = rows 0..3, high 15 bits = rows 4..6)
// Row r (0..6) has 5 bits: bit4=leftmost .. bit0=rightmost.
uint glyph_row_bits(uint ch, uint row) {
    uint lo = 0u, hi = 0u;
    switch (ch) {
        case  48u: lo = 0xACE2Eu; hi = 0x3A39u; break; // '0'
        case  49u: lo = 0x21184u; hi = 0x3884u; break; // '1'
        case  50u: lo = 0x3062Eu; hi = 0x7E08u; break; // '2'
        case  51u: lo = 0x3062Eu; hi = 0x3A21u; break; // '3'
        case  52u: lo = 0x928C2u; hi = 0x085Fu; break; // '4'
        case  53u: lo = 0x0FA1Fu; hi = 0x3A21u; break; // '5'
        case  54u: lo = 0x8FA0Eu; hi = 0x3A31u; break; // '6'
        case  55u: lo = 0x2083Fu; hi = 0x2108u; break; // '7'
        case  56u: lo = 0x7462Eu; hi = 0x3A31u; break; // '8'
        case  57u: lo = 0x7C62Eu; hi = 0x3041u; break; // '9'
        case  65u: lo = 0xFC62Eu; hi = 0x4631u; break; // 'A'
        case  66u: lo = 0xF463Eu; hi = 0x7A31u; break; // 'B'
        case  67u: lo = 0x8422Eu; hi = 0x3A30u; break; // 'C'
        case  68u: lo = 0x8C63Eu; hi = 0x7A31u; break; // 'D'
        case  69u: lo = 0xF421Fu; hi = 0x7E10u; break; // 'E'
        case  70u: lo = 0xF421Fu; hi = 0x4210u; break; // 'F'
        case  71u: lo = 0xBC22Eu; hi = 0x3A31u; break; // 'G'
        case  72u: lo = 0xFC631u; hi = 0x4631u; break; // 'H'
        case  73u: lo = 0x2108Eu; hi = 0x3884u; break; // 'I'
        case  74u: lo = 0x10847u; hi = 0x3252u; break; // 'J'
        case  75u: lo = 0xC5251u; hi = 0x4654u; break; // 'K'
        case  76u: lo = 0x84210u; hi = 0x7E10u; break; // 'L'
        case  77u: lo = 0xAD771u; hi = 0x4631u; break; // 'M'
        case  78u: lo = 0x9D731u; hi = 0x4631u; break; // 'N'
        case  79u: lo = 0x8C62Eu; hi = 0x3A31u; break; // 'O'
        case  80u: lo = 0xF463Eu; hi = 0x4210u; break; // 'P'
        case  81u: lo = 0x8C62Eu; hi = 0x3655u; break; // 'Q'
        case  82u: lo = 0xF463Eu; hi = 0x4654u; break; // 'R'
        case  83u: lo = 0x7420Fu; hi = 0x7821u; break; // 'S'
        case  84u: lo = 0x2109Fu; hi = 0x1084u; break; // 'T'
        case  85u: lo = 0x8C631u; hi = 0x3A31u; break; // 'U'
        case  86u: lo = 0x8C631u; hi = 0x1151u; break; // 'V'
        case  87u: lo = 0xAC631u; hi = 0x4775u; break; // 'W'
        case  88u: lo = 0x22A31u; hi = 0x462Au; break; // 'X'
        case  89u: lo = 0x22A31u; hi = 0x1084u; break; // 'Y'
        case  90u: lo = 0x2083Fu; hi = 0x7E08u; break; // 'Z'
        case  58u: lo = 0x01080u; hi = 0x0084u; break; // ':'
        case  46u: lo = 0x00000u; hi = 0x3180u; break; // '.'
        case  44u: lo = 0x00000u; hi = 0x2084u; break; // ','
        case  47u: lo = 0x20841u; hi = 0x4108u; break; // '/'
        case  45u: lo = 0xF8000u; hi = 0x0000u; break; // '-'
        case  61u: lo = 0x07C00u; hi = 0x001Fu; break; // '='
        case  40u: lo = 0x42082u; hi = 0x0888u; break; // '('
        case  41u: lo = 0x10888u; hi = 0x2082u; break; // ')'
        case  37u: lo = 0x20B59u; hi = 0x4D68u; break; // '%'
        case  43u: lo = 0xF9080u; hi = 0x0084u; break; // '+'
        case 124u: lo = 0x21084u; hi = 0x1084u; break; // '|'
        case  95u: lo = 0x00000u; hi = 0x7C00u; break; // '_'
        case  91u: lo = 0x4210Eu; hi = 0x3908u; break; // '['
        case  93u: lo = 0x1084Eu; hi = 0x3842u; break; // ']'
        default: return 0u;
    }

    if (row < 4u) return (lo >> (row * 5u)) & 0x1Fu;
    return (hi >> ((row - 4u) * 5u)) & 0x1Fu;
}

bool sample_hud_glyph(uint lx, uint ly, uint scale, uint textLeft, uint textTop, uint cellW, uint rowPitch, out uint outRowIdx) {
    outRowIdx = 0u;
    if (lx < textLeft || ly < textTop) return false;
    uint relY = ly - textTop;
    if (relY >= 5u * rowPitch) return false;
    uint rowIdx = relY / rowPitch;
    uint py = relY - rowIdx * rowPitch;
    uint relX = lx - textLeft;
    uint colIdx = relX / cellW;
    uint px = relX - colIdx * cellW;
    if (rowIdx >= 5u || colIdx >= 44u || px >= 5u * scale || py >= 7u * scale) return false;
    uint ch = get_text_char(rowIdx * 44u + colIdx);
    if (ch <= 32u) return false;
    uint gx = px / scale;
    uint gy = py / scale;
    uint bits = glyph_row_bits(ch, gy);
    outRowIdx = rowIdx;
    return (((bits >> (4u - gx)) & 1u) != 0u);
}

[RootSignature("RootFlags(0), CBV(b0)")]
float4 main(float4 pos : SV_Position) : SV_Target0 {
    uint px = uint(pos.x);
    uint py = uint(pos.y);
    if (px < panelX || py < panelY || px >= panelX + panelW || py >= panelY + panelH) {
        discard;
    }

    uint lx = px - panelX;
    uint ly = py - panelY;
    uint scale = (panelW >= 480u) ? 2u : 1u;

    // Xbox Scarlett Emerald accent bar on left edge
    float3 accent = (pressureLevel >= 2u) ? float3(0.95, 0.25, 0.22)
                  : ((pressureLevel == 1u) ? float3(0.92, 0.72, 0.18)
                                           : float3(0.22, 0.86, 0.36));
    uint accentW = (scale == 2u) ? 6u : 4u;
    if (lx < accentW) {
        return float4(accent, 0.98);
    }

    // Crisp 1px outer border
    if (lx == panelW - 1u || ly == 0u || ly == panelH - 1u) {
        return float4(0.24, 0.30, 0.36, 0.96);
    }

    // Translucent dark obsidian panel background (high readability over bright sky/city)
    float4 color = float4(0.04, 0.06, 0.08, 0.90);

    // Header separator line below row 0
    uint sepY = (scale == 2u) ? 31u : 20u;
    if (ly == sepY) {
        return float4(0.18, 0.24, 0.30, 0.95);
    }

    // Crisp integer-scaled bitmap text (5 rows x 44 cols) with 1px dark drop shadow at 2x
    uint textTop  = (scale == 2u) ? 12u : 7u;
    uint textLeft = (scale == 2u) ? 18u : 11u;
    uint cellW    = (scale == 2u) ? 12u : 6u;   // 5*scale glyph + 2px (or 1px) crisp gap
    uint rowPitch = (scale == 2u) ? 24u : 15u;  // 7*scale glyph + generous line spacing
    uint rowIdx = 0u;
    if (sample_hud_glyph(lx, ly, scale, textLeft, textTop, cellW, rowPitch, rowIdx)) {
        float3 textCol = (rowIdx == 0u) ? accent : float3(0.95, 0.97, 0.99);
        return float4(textCol, 0.99);
    }
    if (scale == 2u && lx > 0u && ly > 0u &&
        sample_hud_glyph(lx - 1u, ly - 1u, scale, textLeft, textTop, cellW, rowPitch, rowIdx)) {
        return float4(0.0, 0.0, 0.0, 0.95);
    }

    // Bottom frametime sparkline graph (60 bars, scaled 0..33.3 ms)
    uint graphLeft = textLeft;
    uint graphH = (scale == 2u) ? 44u : 30u;
    uint barStep = (scale == 2u) ? 8u : 4u;
    uint graphW = 60u * barStep;
    uint graphTop = panelH - graphH - ((scale == 2u) ? 14u : 10u);
    if (lx >= graphLeft && lx < graphLeft + graphW && ly >= graphTop && ly < graphTop + graphH) {
        uint bar = (lx - graphLeft) / barStep;
        uint subX = (lx - graphLeft) - bar * barStep;
        uint histIdx = (historyHead + bar) % 60u;
        uint ms10 = get_hist_ms10(histIdx); // e.g. 167 = 16.7 ms
        uint barH = min(graphH, (ms10 * graphH) / 333u);
        uint yFromBot = (graphTop + graphH - 1u) - ly;

        // Subtle graph well background + 60 FPS (16.7ms) dashed midline
        color = float4(0.02, 0.03, 0.05, 0.94);
        if (yFromBot == graphH / 2u && (lx & 3u) < 2u) {
            color = float4(0.28, 0.35, 0.42, 0.92);
        }
        if (subX + 1u < barStep && yFromBot < barH && ms10 > 0u) {
            float3 barCol = (ms10 > 250u) ? float3(0.94, 0.32, 0.25)
                          : ((ms10 > 175u) ? float3(0.94, 0.74, 0.20) : accent);
            color = float4(barCol, 0.96);
        }
    }

    return color;
}
)HLSL";

    bool ensure_init_locked(ID3D12Device* raw_device) noexcept {
        if (!raw_device) return false;
        if (init_attempted_) return available_;
        init_attempted_ = true;

        char err[512]{};
        if (FAILED(wsx_sm66::compile_hlsl_to_dxil_vec(
                kHudVsHlsl, std::strlen(kHudVsHlsl), L"main", L"vs_6_6", vs_dxil_, err, sizeof(err)))) {
            return false;
        }
        if (FAILED(wsx_sm66::compile_hlsl_to_dxil_vec(
                kHudPsHlsl, std::strlen(kHudPsHlsl), L"main", L"ps_6_6", ps_dxil_, err, sizeof(err)))) {
            return false;
        }
        if (FAILED(raw_device->CreateRootSignature(0u, vs_dxil_.data(), vs_dxil_.size(), IID_PPV_ARGS(&root_sig_)))) {
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1u;
        if (FAILED(raw_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_)))) return false;

        if (FAILED(raw_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmd_alloc_)))) return false;
        if (FAILED(raw_device->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_alloc_.Get(), nullptr, IID_PPV_ARGS(&cmd_list_)))) return false;
        cmd_list_->Close();

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = sizeof(HudConstantData);
        bd.Height = 1u;
        bd.DepthOrArraySize = 1u;
        bd.MipLevels = 1u;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1u;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(raw_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cb_buf_)))) {
            return false;
        }

        if (FAILED(raw_device->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event_) return false;

        available_ = true;
        return true;
    }

    ID3D12PipelineState* get_or_create_pso_locked(ID3D12Device* raw_device, DXGI_FORMAT rtv_fmt) noexcept {
        const uint32_t key = static_cast<uint32_t>(rtv_fmt);
        auto it = pso_by_fmt_.find(key);
        if (it != pso_by_fmt_.end()) return it->second.Get();

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root_sig_.Get();
        pd.VS = {vs_dxil_.data(), vs_dxil_.size()};
        pd.PS = {ps_dxil_.data(), ps_dxil_.size()};
        pd.BlendState.RenderTarget[0].BlendEnable = TRUE;
        pd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        pd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1u;
        pd.RTVFormats[0] = rtv_fmt;
        pd.SampleDesc.Count = 1u;

        ComPtr<ID3D12PipelineState> pso;
        if (FAILED(raw_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) {
            return nullptr;
        }
        ID3D12PipelineState* ptr = pso.Get();
        pso_by_fmt_.emplace(key, std::move(pso));
        return ptr;
    }

    static void pack_row_ascii(uint32_t text_packed[56], uint32_t row, const char* line) noexcept {
        if (row >= kRows) return;
        for (uint32_t col = 0u; col < kCols; ++col) {
            char ch = ' ';
            if (line && line[col] != '\0') {
                ch = line[col];
                if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
            } else {
                line = nullptr;
            }
            const uint32_t idx = row * kCols + col;
            const uint32_t w = idx >> 2;
            const uint32_t sh = (idx & 3u) * 8u;
            text_packed[w] = (text_packed[w] & ~(0xFFu << sh)) | (static_cast<uint32_t>(static_cast<uint8_t>(ch)) << sh);
        }
    }

public:
    WsxDebugHudOverlay() {
        QueryPerformanceFrequency(&qpc_freq_);
        if (qpc_freq_.QuadPart <= 0) qpc_freq_.QuadPart = 1;
        enabled_.store(wsx::get_config_bool("WSX_HUD", "Diagnostics", "hud", false), std::memory_order_relaxed);
    }

    ~WsxDebugHudOverlay() {
        shutdown();
    }

    void shutdown() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        if (fence_ && fence_val_ > 0u && fence_->GetCompletedValue() < fence_val_ && fence_event_) {
            fence_->SetEventOnCompletion(fence_val_, fence_event_);
            WaitForSingleObject(fence_event_, 3000);
        }
        if (fence_event_) {
            CloseHandle(fence_event_);
            fence_event_ = nullptr;
        }
        fence_.Reset();
        cb_buf_.Reset();
        cmd_list_.Reset();
        cmd_alloc_.Reset();
        rtv_heap_.Reset();
        pso_by_fmt_.clear();
        root_sig_.Reset();
        available_ = false;
        init_attempted_ = false;
    }

    void set_enabled(bool on) noexcept {
        enabled_.store(on, std::memory_order_relaxed);
    }

    bool is_enabled() const noexcept {
        return enabled_.load(std::memory_order_relaxed);
    }

    void poll_hotkey_and_record_frame(float override_frame_ms = -1.0f) noexcept {
        const bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (f11 && !prev_f11_down_) {
            enabled_.store(!enabled_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        prev_f11_down_ = f11;

        float ms = override_frame_ms;
        if (ms < 0.0f) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            if (has_last_qpc_) {
                ms = static_cast<float>(static_cast<double>(now.QuadPart - last_present_qpc_.QuadPart) * 1000.0 /
                                        static_cast<double>(qpc_freq_.QuadPart));
                ms = std::clamp(ms, 0.1f, 250.0f);
            } else {
                ms = 16.67f;
            }
            last_present_qpc_ = now;
            has_last_qpc_ = true;
        }
        std::lock_guard<std::mutex> lk(mtx_);
        frame_ms_hist_[hist_cursor_ % kHistory] = ms;
        hist_cursor_ = (hist_cursor_ + 1u) % kHistory;
    }

    bool poll_f12_screenshot_requested() noexcept {
        const bool f12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        const bool triggered = (f12 && !prev_f12_down_);
        prev_f12_down_ = f12;
        return triggered;
    }

    HRESULT capture_backbuffer_png(
        ID3D12Device* raw_device,
        ID3D12CommandQueue* raw_queue,
        ID3D12Resource* raw_backbuffer,
        D3D12_RESOURCE_STATES state_before,
        D3D12_RESOURCE_STATES state_after,
        const std::filesystem::path& out_png) noexcept {
        if (!raw_device || !raw_queue || !raw_backbuffer || out_png.empty()) return E_INVALIDARG;
        std::lock_guard<std::mutex> lk(mtx_);
        if (!ensure_init_locked(raw_device)) return E_FAIL;

        if (fence_val_ > 0u && fence_->GetCompletedValue() < fence_val_) {
            fence_->SetEventOnCompletion(fence_val_, fence_event_);
            WaitForSingleObject(fence_event_, 5000);
        }

        const D3D12_RESOURCE_DESC rd = raw_backbuffer->GetDesc();
        if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.Width == 0u || rd.Height == 0u) {
            return E_INVALIDARG;
        }
        const uint32_t w = static_cast<uint32_t>(rd.Width);
        const uint32_t h = rd.Height;
        const uint32_t row_pitch = ((w * 4u) + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
                                   ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);

        D3D12_HEAP_PROPERTIES hp_rb{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rbd{};
        rbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rbd.Width = static_cast<UINT64>(row_pitch) * h;
        rbd.Height = 1;
        rbd.DepthOrArraySize = 1;
        rbd.MipLevels = 1;
        rbd.SampleDesc.Count = 1;
        rbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> rb;
        HRESULT hr = raw_device->CreateCommittedResource(
            &hp_rb, D3D12_HEAP_FLAG_NONE, &rbd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));
        if (FAILED(hr)) return hr;

        if (FAILED(hr = cmd_alloc_->Reset())) return hr;
        if (FAILED(hr = cmd_list_->Reset(cmd_alloc_.Get(), nullptr))) return hr;

        if (state_before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = {raw_backbuffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                            state_before, D3D12_RESOURCE_STATE_COPY_SOURCE};
            cmd_list_->ResourceBarrier(1u, &b);
        }

        D3D12_TEXTURE_COPY_LOCATION src_loc{raw_backbuffer, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        D3D12_TEXTURE_COPY_LOCATION dst_loc{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dst_loc.PlacedFootprint.Footprint = {rd.Format, w, h, 1, row_pitch};
        cmd_list_->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

        if (state_after != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = {raw_backbuffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                            D3D12_RESOURCE_STATE_COPY_SOURCE, state_after};
            cmd_list_->ResourceBarrier(1u, &b);
        }

        if (FAILED(hr = cmd_list_->Close())) return hr;
        ID3D12CommandList* lists[] = {cmd_list_.Get()};
        raw_queue->ExecuteCommandLists(1u, lists);
        ++fence_val_;
        if (FAILED(hr = raw_queue->Signal(fence_.Get(), fence_val_))) return hr;
        if (fence_->GetCompletedValue() < fence_val_) {
            fence_->SetEventOnCompletion(fence_val_, fence_event_);
            WaitForSingleObject(fence_event_, 5000);
        }

        void* mapped = nullptr;
        hr = rb->Map(0u, nullptr, &mapped);
        if (FAILED(hr) || !mapped) return FAILED(hr) ? hr : E_FAIL;
        std::vector<uint8_t> tight(static_cast<size_t>(w) * h * 4u);
        for (uint32_t y = 0; y < h; ++y) {
            std::memcpy(tight.data() + static_cast<size_t>(y) * w * 4u,
                        static_cast<const uint8_t*>(mapped) + static_cast<size_t>(y) * row_pitch,
                        static_cast<size_t>(w) * 4u);
        }
        rb->Unmap(0u, nullptr);

        const bool is_bgra = (rd.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                              rd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                              rd.Format == DXGI_FORMAT_B8G8R8X8_UNORM);
        return write_png_rgb24(out_png, tight.data(), w, h, is_bgra) ? S_OK : E_FAIL;
    }

    float current_avg_frame_ms() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        float sum = 0.0f;
        uint32_t cnt = 0u;
        for (float v : frame_ms_hist_) {
            if (v > 0.0f) { sum += v; ++cnt; }
        }
        return cnt > 0u ? (sum / static_cast<float>(cnt)) : 16.67f;
    }

    HRESULT render(
        ID3D12Device* raw_device,
        ID3D12CommandQueue* raw_queue,
        ID3D12Resource* raw_backbuffer,
        D3D12_RESOURCE_STATES state_before,
        D3D12_RESOURCE_STATES state_after,
        const HudMetricsSnapshot& metrics) noexcept {
        if (!raw_device || !raw_queue || !raw_backbuffer) return E_INVALIDARG;

        std::lock_guard<std::mutex> lk(mtx_);
        if (!ensure_init_locked(raw_device)) return E_FAIL;

        if (fence_val_ > 0u && fence_->GetCompletedValue() < fence_val_) {
            fence_->SetEventOnCompletion(fence_val_, fence_event_);
            WaitForSingleObject(fence_event_, 5000);
        }

        const D3D12_RESOURCE_DESC rd = raw_backbuffer->GetDesc();
        if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0u) {
            return E_INVALIDARG;
        }
        const DXGI_FORMAT rtv_fmt = (rd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) ? DXGI_FORMAT_R8G8B8A8_UNORM
                                  : (rd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) ? DXGI_FORMAT_B8G8R8A8_UNORM
                                  : rd.Format;
        ID3D12PipelineState* pso = get_or_create_pso_locked(raw_device, rtv_fmt);
        if (!pso) return E_FAIL;

        HudConstantData cb{};
        cb.target_w = static_cast<uint32_t>(rd.Width);
        cb.target_h = rd.Height;
        const bool hd_scale = (cb.target_w >= 960u && cb.target_h >= 540u);
        cb.panel_x = hd_scale ? 16u : 12u;
        cb.panel_y = hd_scale ? 16u : 12u;
        const uint32_t want_w = hd_scale ? 560u : 286u;
        const uint32_t want_h = hd_scale ? 196u : 132u;
        cb.panel_w = (std::min<uint32_t>)(want_w, cb.target_w > 32u ? cb.target_w - 32u : cb.target_w);
        cb.panel_h = (std::min<uint32_t>)(want_h, cb.target_h > 32u ? cb.target_h - 32u : cb.target_h);
        cb.history_head = hist_cursor_;
        cb.pressure_level = metrics.vram_pressure;

        const double pso_total = static_cast<double>(metrics.pso_hits + metrics.pso_misses);
        const double pso_pct = (pso_total > 0.0) ? (100.0 * static_cast<double>(metrics.pso_hits) / pso_total) : 100.0;
        const float low_fps = (metrics.one_percent_low_fps > 0.0f) ? metrics.one_percent_low_fps : metrics.fps;
        const float p99_ms  = (metrics.p99_frame_ms > 0.0f) ? metrics.p99_frame_ms : metrics.frame_ms;

        char lines[kRows][64]{};
        std::snprintf(lines[0], sizeof(lines[0]), "MIZUBOX DX12 SM6.6 HUD W64:%s",
                      metrics.wave64_emulated ? "EMU" : "NAT");
        std::snprintf(lines[1], sizeof(lines[1]), "FPS:%5.1f 1L:%4.0f P99:%4.1fMS",
                      metrics.fps, low_fps, p99_ms);
        std::snprintf(lines[2], sizeof(lines[2]), "VRAM:%4.0f/%4.0fMB PSO:%3.0f%% (%llu/%llu)",
                      metrics.vram_used_mb, metrics.vram_budget_mb, pso_pct,
                      static_cast<unsigned long long>(metrics.pso_hits),
                      static_cast<unsigned long long>(metrics.pso_hits + metrics.pso_misses));
        std::snprintf(lines[3], sizeof(lines[3]), "DRW:%llu DSP:%llu MSH:%llu BAR-:%llu",
                      static_cast<unsigned long long>(metrics.draw_calls),
                      static_cast<unsigned long long>(metrics.dispatch_calls),
                      static_cast<unsigned long long>(metrics.mesh_dispatches),
                      static_cast<unsigned long long>(metrics.barriers_elided));
        std::snprintf(lines[4], sizeof(lines[4]), "UMA:%llu WW:%llu ASYNC:%llu/%llu F12:PNG",
                      static_cast<unsigned long long>(metrics.uma_uploads),
                      static_cast<unsigned long long>(metrics.uma_ww_skips),
                      static_cast<unsigned long long>(metrics.async_done),
                      static_cast<unsigned long long>(metrics.async_total));

        for (uint32_t r = 0u; r < kRows; ++r) {
            pack_row_ascii(cb.text_packed, r, lines[r]);
        }
        for (uint32_t i = 0u; i < kHistory; ++i) {
            cb.history_ms10[i] = static_cast<uint32_t>(std::clamp(frame_ms_hist_[i] * 10.0f, 0.0f, 1000.0f));
        }

        void* mapped = nullptr;
        D3D12_RANGE rr{0, 0};
        HRESULT hr = cb_buf_->Map(0u, &rr, &mapped);
        if (FAILED(hr) || !mapped) return FAILED(hr) ? hr : E_FAIL;
        std::memcpy(mapped, &cb, sizeof(cb));
        D3D12_RANGE wr{0, sizeof(cb)};
        cb_buf_->Unmap(0u, &wr);

        D3D12_RENDER_TARGET_VIEW_DESC rtvd{};
        rtvd.Format = rtv_fmt;
        rtvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        raw_device->CreateRenderTargetView(raw_backbuffer, &rtvd, rtv_h);

        if (FAILED(hr = cmd_alloc_->Reset())) return hr;
        if (FAILED(hr = cmd_list_->Reset(cmd_alloc_.Get(), pso))) return hr;

        if (state_before != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = raw_backbuffer;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = state_before;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            cmd_list_->ResourceBarrier(1u, &b);
        }

        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(rd.Width), static_cast<float>(rd.Height), 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, static_cast<LONG>(rd.Width), static_cast<LONG>(rd.Height)};
        cmd_list_->RSSetViewports(1u, &vp);
        cmd_list_->RSSetScissorRects(1u, &sc);
        cmd_list_->OMSetRenderTargets(1u, &rtv_h, FALSE, nullptr);
        cmd_list_->SetGraphicsRootSignature(root_sig_.Get());
        cmd_list_->SetGraphicsRootConstantBufferView(0u, cb_buf_->GetGPUVirtualAddress());
        cmd_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd_list_->DrawInstanced(3u, 1u, 0u, 0u);

        if (state_after != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = raw_backbuffer;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = state_after;
            cmd_list_->ResourceBarrier(1u, &b);
        }

        if (FAILED(hr = cmd_list_->Close())) return hr;
        ID3D12CommandList* lists[] = {cmd_list_.Get()};
        raw_queue->ExecuteCommandLists(1u, lists);
        ++fence_val_;
        if (FAILED(hr = raw_queue->Signal(fence_.Get(), fence_val_))) return hr;
        if (fence_->GetCompletedValue() < fence_val_) {
            fence_->SetEventOnCompletion(fence_val_, fence_event_);
            WaitForSingleObject(fence_event_, 5000);
        }
        return S_OK;
    }
};

} // namespace wsx_hud
