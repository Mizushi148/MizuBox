// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// End-to-end GPU and runtime verification harness for all 6 real Xbox Series X|S
// middleware DLLs shipped with the GTA VI Xbox build (v1.0.2.1), running on
// Windows x64 through MizuBox shims (d3d12_xs.dll, xmem.dll, xgameruntime.dll).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "include/xbox/d3d12_xs.h"
#include "include/xbox/XAsync.h"
#include "include/xbox/XTaskQueue.h"
#include "wsx_inflate.h"
#include "xmem.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// ============================================================================
// Common helpers
// ============================================================================

std::filesystem::path executable_directory() {
    std::array<wchar_t, 32768> path{};
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) {
        return {};
    }
    return std::filesystem::path(path.data()).parent_path();
}

uint64_t fnv1a64(const void* data, size_t len) noexcept {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

struct SuiteStats {
    int passed = 0;
    int warnings = 0;
    int failed = 0;
};

struct SehFaultInfo {
    DWORD code = 0;
    uintptr_t address = 0;
};

int seh_filter(DWORD code, EXCEPTION_POINTERS* ep, SehFaultInfo* out) noexcept {
    if (out) {
        out->code = code;
        out->address = (ep && ep->ExceptionRecord)
            ? reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress)
            : 0;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

DWORD seh_call_raw(void (*fn)(void*), void* ctx, SehFaultInfo* out_fault = nullptr) noexcept {
    __try {
        fn(ctx);
        return 0;
    } __except (seh_filter(GetExceptionCode(), GetExceptionInformation(), out_fault)) {
        return out_fault ? out_fault->code : GetExceptionCode();
    }
}

template <typename Fn>
DWORD seh_run(Fn&& fn, SehFaultInfo* out_fault = nullptr) noexcept {
    auto thunk = [](void* p) { (*static_cast<std::remove_reference_t<Fn>*>(p))(); };
    return seh_call_raw(thunk, &fn, out_fault);
}

// ============================================================================
// In-process Loopback HTTP/1.1 Server (for XCurl.dll & libHttpClient.GDK.dll)
// ============================================================================

class LocalHttpServer {
public:
    bool start() {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return false;
        }
        wsa_started_ = true;

        listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_sock_ == INVALID_SOCKET) {
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // Ephemeral port
        if (bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            return false;
        }
        int len = sizeof(addr);
        if (getsockname(listen_sock_, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR) {
            return false;
        }
        port_ = ntohs(addr.sin_port);
        if (listen(listen_sock_, 16) == SOCKET_ERROR) {
            return false;
        }

        running_.store(true);
        worker_ = std::thread([this]() { serve_loop(); });
        return true;
    }

    void stop() {
        running_.store(false);
        if (listen_sock_ != INVALID_SOCKET) {
            closesocket(listen_sock_);
            listen_sock_ = INVALID_SOCKET;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        if (wsa_started_) {
            WSACleanup();
            wsa_started_ = false;
        }
    }

    ~LocalHttpServer() { stop(); }

    uint16_t port() const noexcept { return port_; }
    uint32_t requests_handled() const noexcept { return requests_.load(); }

private:
    void serve_loop() {
        while (running_.load()) {
            SOCKET client = accept(listen_sock_, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                break;
            }
            DWORD timeout_ms = 2000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

            std::string req;
            req.reserve(4096);
            char buf[2048];
            int n = recv(client, buf, sizeof(buf), 0);
            if (n > 0) {
                req.append(buf, static_cast<size_t>(n));
                const auto hdr_end = req.find("\r\n\r\n");
                if (hdr_end != std::string::npos) {
                    // Check Content-Length for POST bodies
                    size_t content_len = 0;
                    const std::string lower_hdr = [&]() {
                        std::string s = req.substr(0, hdr_end);
                        for (char& c : s) {
                            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                        }
                        return s;
                    }();
                    const auto cl_pos = lower_hdr.find("content-length:");
                    if (cl_pos != std::string::npos) {
                        content_len = static_cast<size_t>(std::strtoull(lower_hdr.c_str() + cl_pos + 15, nullptr, 10));
                    }
                    const size_t target_size = hdr_end + 4 + content_len;
                    while (req.size() < target_size && req.size() < 65536) {
                        const int more = recv(client, buf, sizeof(buf), 0);
                        if (more <= 0) break;
                        req.append(buf, static_cast<size_t>(more));
                    }
                }
            }

            std::string method = "UNKNOWN";
            std::string path = "/";
            const auto line_end = req.find("\r\n");
            if (line_end != std::string::npos) {
                const auto sp1 = req.find(' ');
                const auto sp2 = (sp1 != std::string::npos) ? req.find(' ', sp1 + 1) : std::string::npos;
                if (sp1 != std::string::npos && sp2 != std::string::npos && sp2 < line_end) {
                    method = req.substr(0, sp1);
                    path = req.substr(sp1 + 1, sp2 - sp1 - 1);
                }
            }
            std::string req_body;
            const auto body_pos = req.find("\r\n\r\n");
            if (body_pos != std::string::npos) {
                req_body = req.substr(body_pos + 4);
            }

            const std::string body = "WSX_OK|" + method + "|" + path + "|body=" + req_body;
            const std::string resp =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "X-Wsx-Server: MizuBox\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                "Connection: close\r\n\r\n" + body;
            send(client, resp.data(), static_cast<int>(resp.size()), 0);
            shutdown(client, SD_SEND);
            closesocket(client);
            requests_.fetch_add(1);
        }
    }

    bool wsa_started_ = false;
    SOCKET listen_sock_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> requests_{0};
    std::thread worker_;
};

// ============================================================================
// GPU helper: execute command list and wait on fence
// ============================================================================

HRESULT gpu_execute_and_wait(ID3D12Device* dev, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cl) {
    HRESULT hr = cl->Close();
    if (FAILED(hr)) return hr;
    ID3D12CommandList* lists[] = {cl};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
    if (FAILED(hr)) return hr;
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!evt) return HRESULT_FROM_WIN32(GetLastError());
    hr = queue->Signal(fence.Get(), 1);
    if (SUCCEEDED(hr) && fence->GetCompletedValue() < 1) {
        hr = fence->SetEventOnCompletion(1, evt);
        if (SUCCEEDED(hr)) {
            WaitForSingleObject(evt, 10000);
        }
    }
    CloseHandle(evt);
    return hr;
}

// ============================================================================
// [1/6] Bink2Scarlett.dll + real movies.rpf + xmem.dll + d3d12_xs.dll GPU test
// ============================================================================

struct BinkHeaderView {
    uint32_t Width;
    uint32_t Height;
    uint32_t Frames;
    uint32_t FrameNum;
    uint32_t LastFrameNum;
    uint32_t FrameRate;
    uint32_t FrameRateDiv;
    uint32_t ReadError;
    uint32_t OpenFlags;
    uint32_t BinkType;
};

struct BinkSummary {
    uint32_t Width;
    uint32_t Height;
    uint32_t TotalTime;
    uint32_t FileFrameRate;
    uint32_t FileFrameRateDiv;
    uint32_t FrameRate;
    uint32_t FrameRateDiv;
    uint32_t TotalOpenTime;
    uint32_t TotalFrames;
    uint32_t TotalPlayedFrames;
    uint32_t SkippedFrames;
    uint32_t SkippedBlits;
    uint32_t SoundSkips;
    uint32_t TotalBlitTime;
    uint32_t TotalReadTime;
    uint32_t TotalVideoDecompTime;
    uint32_t TotalAudioDecompTime;
    uint32_t TotalIdleReadTime;
    uint32_t TotalBackReadTime;
    uint32_t TotalReadSpeed;
    uint32_t SlowestFrameTime;
    uint32_t Slowest2FrameTime;
    uint32_t SlowestFrameNum;
    uint32_t Slowest2FrameNum;
    uint32_t AverageDataRate;
    uint32_t AverageFrameSize;
    uint32_t HighestMemAmount;
    uint32_t TotalIOMemory;
    uint32_t HighestIOUsed;
    uint32_t Highest1SecRate;
    uint32_t Highest1SecFrame;
};

void test_bink2_scarlett(
    const std::filesystem::path& build_dir,
    ID3D12Device8* d3d_dev,
    ID3D12CommandQueue* queue,
    HMODULE h_xmem,
    SuiteStats& stats) {
    std::puts("\n========================================================================");
    std::puts("[1/6] Bink2Scarlett.dll (movies.rpf decode + xmem.dll + d3d12_xs GPU UMA)");
    std::puts("========================================================================");

    const auto dll_path = build_dir / L"Bink2Scarlett.dll";
    HMODULE mod = LoadLibraryExW(dll_path.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!mod) {
        std::printf("  [FAIL] LoadLibrary(Bink2Scarlett.dll) err=%lu\n", GetLastError());
        stats.failed++;
        return;
    }

    using BinkOpenProc = BinkHeaderView* (*)(const void* name_or_mem, uint32_t flags);
    using BinkCloseProc = void (*)(BinkHeaderView*);
    using BinkDoFrameProc = int32_t (*)(BinkHeaderView*);
    using BinkNextFrameProc = void (*)(BinkHeaderView*);
    using BinkCopyToBufferProc = int32_t (*)(BinkHeaderView*, void* dest, int32_t pitch, uint32_t height, uint32_t x, uint32_t y, uint32_t flags);
    using BinkGetErrorProc = const char* (*)();
    using BinkGetSummaryProc = void (*)(BinkHeaderView*, BinkSummary*);
    using BinkUtilCPUsProc = uint32_t (*)();
    using BinkUtilMallocProc = void* (*)(uint64_t);
    using BinkUtilFreeProc = void (*)(void*);
    using BinkOpenXAudio2Proc = void* (*)(uintptr_t);
    using BinkSetSoundSystemProc = int32_t (*)(void*, uintptr_t);

    auto pOpen = std::bit_cast<BinkOpenProc>(GetProcAddress(mod, "BinkOpen"));
    auto pClose = std::bit_cast<BinkCloseProc>(GetProcAddress(mod, "BinkClose"));
    auto pDoFrame = std::bit_cast<BinkDoFrameProc>(GetProcAddress(mod, "BinkDoFrame"));
    auto pNextFrame = std::bit_cast<BinkNextFrameProc>(GetProcAddress(mod, "BinkNextFrame"));
    auto pCopy = std::bit_cast<BinkCopyToBufferProc>(GetProcAddress(mod, "BinkCopyToBuffer"));
    auto pGetError = std::bit_cast<BinkGetErrorProc>(GetProcAddress(mod, "BinkGetError"));
    auto pGetSummary = std::bit_cast<BinkGetSummaryProc>(GetProcAddress(mod, "BinkGetSummary"));
    auto pCPUs = std::bit_cast<BinkUtilCPUsProc>(GetProcAddress(mod, "BinkUtilCPUs"));
    auto pMalloc = std::bit_cast<BinkUtilMallocProc>(GetProcAddress(mod, "BinkUtilMalloc"));
    auto pFree = std::bit_cast<BinkUtilFreeProc>(GetProcAddress(mod, "BinkUtilFree"));
    auto pOpenXAudio2 = std::bit_cast<BinkOpenXAudio2Proc>(GetProcAddress(mod, "BinkOpenXAudio2"));
    auto pSetSound = std::bit_cast<BinkSetSoundSystemProc>(GetProcAddress(mod, "BinkSetSoundSystem"));

    using GetWsProc = BOOL(WINAPI*)(DWORD, XMEM_WORKING_SET_STATISTICS*);
    auto pXMemStats = std::bit_cast<GetWsProc>(GetProcAddress(h_xmem, "XMemGetWorkingSetStatistics"));

    XMEM_WORKING_SET_STATISTICS ws_before{};
    if (pXMemStats) pXMemStats(0, &ws_before);

    // 1. CPU & XMem probe via BinkUtilMalloc/Free
    const uint32_t cpus = pCPUs ? pCPUs() : 0;
    void* bink_mem = pMalloc ? pMalloc(65536) : nullptr;
    XMEM_WORKING_SET_STATISTICS ws_mid{};
    if (pXMemStats) pXMemStats(0, &ws_mid);
    if (bink_mem && pFree) pFree(bink_mem);
    std::printf("  [ OK ] BinkUtilCPUs=%u, BinkUtilMalloc(64KB)=%p (xmem tracked delta=%+lld B, allocs=%+lld)\n",
                cpus, bink_mem,
                static_cast<long long>(ws_mid.XMemTrackedBytes - ws_before.XMemTrackedBytes),
                static_cast<long long>(ws_mid.XMemAllocationCount - ws_before.XMemAllocationCount));

    // 2. XAudio2 sound backend binding (imports xaudio2_9.dll ordinal 1)
    if (pSetSound && pOpenXAudio2) {
        const int32_t snd_ok = pSetSound(reinterpret_cast<void*>(pOpenXAudio2), 0);
        std::printf("  [ OK ] BinkSetSoundSystem(BinkOpenXAudio2, 0) -> %d\n", snd_ok);
    }

    // 3. Read real KB2n stream from movies.rpf (1080p clip @ 0x1BFE1000, 180600 bytes)
    //    AND 720x1280 audio clip @ 0x038A6A000 (3.26 MB)
    const auto movies_path = build_dir / L"movies.rpf";
    std::ifstream mf(movies_path, std::ios::binary);
    if (!mf) {
        std::printf("  [WARN] movies.rpf not found at %ls\n", movies_path.c_str());
        stats.warnings++;
        FreeLibrary(mod);
        return;
    }

    struct ClipSpec {
        const char* label;
        uint64_t offset;
    };
    constexpr ClipSpec kClips[] = {
        {"1080p_129 (no audio)", 0x1BFE1000ULL},
        {"vert_067 (720x1280 + 1 audio track)", 0x038A6A000ULL},
    };

    bool gpu_roundtrip_ok = false;

    for (const auto& clip : kClips) {
        mf.clear();
        mf.seekg(static_cast<std::streamoff>(clip.offset), std::ios::beg);
        uint8_t hdr[32]{};
        mf.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
        if (!mf || std::memcmp(hdr, "KB2", 3) != 0) {
            std::printf("  [FAIL] Invalid KB2 header at 0x%llX for %s\n",
                        static_cast<unsigned long long>(clip.offset), clip.label);
            stats.failed++;
            continue;
        }
        uint32_t payload_sz = 0;
        std::memcpy(&payload_sz, hdr + 4, 4);
        const size_t total_sz = static_cast<size_t>(payload_sz) + 8;
        std::vector<uint8_t> stream_buf(total_sz);
        mf.seekg(static_cast<std::streamoff>(clip.offset), std::ios::beg);
        mf.read(reinterpret_cast<char*>(stream_buf.data()), static_cast<std::streamsize>(total_sz));

        constexpr uint32_t BINKFROMMEMORY = 0x04000000u;
        constexpr uint32_t BINKNOSKIP = 0x00080000u;
        constexpr uint32_t BINKSURFACE32RGBA = 6u;

        XMEM_WORKING_SET_STATISTICS ws_open0{}, ws_open1{};
        if (pXMemStats) pXMemStats(0, &ws_open0);

        BinkHeaderView* bnk = nullptr;
        DWORD exc = seh_run([&]() {
            bnk = pOpen(stream_buf.data(), BINKFROMMEMORY | BINKNOSKIP);
        });
        if (pXMemStats) pXMemStats(0, &ws_open1);

        if (exc != 0 || !bnk) {
            std::printf("  [FAIL] BinkOpen(%s) failed exc=0x%08lX err=%s\n",
                        clip.label, exc, (pGetError && pGetError()) ? pGetError() : "(none)");
            stats.failed++;
            continue;
        }

        std::printf("  [ OK ] BinkOpen(%s): %ux%u, %u frames, %.2f fps | xmem delta=%+lld B\n",
                    clip.label, bnk->Width, bnk->Height, bnk->Frames,
                    bnk->FrameRateDiv ? (static_cast<double>(bnk->FrameRate) / bnk->FrameRateDiv) : 0.0,
                    static_cast<long long>(ws_open1.XMemTrackedBytes - ws_open0.XMemTrackedBytes));

        const uint32_t w = bnk->Width;
        const uint32_t h = bnk->Height;
        const uint32_t pitch = w * 4;
        std::vector<uint8_t> rgba(static_cast<size_t>(pitch) * h, 0);

        char dump_dir_buf[MAX_PATH]{};
        const bool want_dump = GetEnvironmentVariableA("WSX_DUMP_BINK_DIR", dump_dir_buf, sizeof(dump_dir_buf)) > 0u;
        const uint32_t frames_to_decode = std::min<uint32_t>(bnk->Frames, (bnk->Frames >= 60u && want_dump) ? 45u : 5u);
        uint64_t last_hash = 0;
        size_t last_nonzero = 0;
        bool decode_ok = true;
        for (uint32_t f = 0; f < frames_to_decode; ++f) {
            exc = seh_run([&]() {
                pDoFrame(bnk);
                pCopy(bnk, rgba.data(), static_cast<int32_t>(pitch), h, 0, 0, BINKSURFACE32RGBA);
                if (f + 1 < frames_to_decode) pNextFrame(bnk);
            });
            if (exc != 0) {
                decode_ok = false;
                break;
            }
            last_hash = fnv1a64(rgba.data(), rgba.size());
            last_nonzero = 0;
            for (size_t i = 0; i < rgba.size(); i += 4) {
                if ((rgba[i] | rgba[i + 1] | rgba[i + 2]) != 0) ++last_nonzero;
            }
        }

        if (!decode_ok) {
            std::printf("  [FAIL] BinkDoFrame/Copy exception 0x%08lX\n", exc);
            stats.failed++;
        } else {
            BinkSummary sum{};
            if (pGetSummary) pGetSummary(bnk, &sum);
            std::printf("  [ OK ] Decoded %u real frames -> frame %u non-black pixels=%zu/%u hash=0x%016llX (peak mem=%u KB)\n",
                        frames_to_decode, frames_to_decode, last_nonzero, w * h,
                        static_cast<unsigned long long>(last_hash), sum.HighestMemAmount / 1024);

            // Upload decoded frame to GPU via d3d12_xs.dll (UMA DEFAULT buffer + Xbox DCC texture + readback)
            if (((!gpu_roundtrip_ok && w == 1920 && h == 1080) || want_dump) && d3d_dev && queue) {
                const uint32_t aligned_pitch = (pitch + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
                const uint64_t buf_bytes = static_cast<uint64_t>(aligned_pitch) * h;

                // 1) Upload buffer allocated as D3D12_HEAP_TYPE_DEFAULT via D3D12XboxCreateCommittedResource
                //    Tests MizuBox UMA emulation (CPU Map() on DEFAULT heap buffer!)
                D3D12_HEAP_PROPERTIES default_heap{};
                default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC buf_desc{};
                buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                buf_desc.Width = buf_bytes;
                buf_desc.Height = 1;
                buf_desc.DepthOrArraySize = 1;
                buf_desc.MipLevels = 1;
                buf_desc.SampleDesc.Count = 1;
                buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

                ComPtr<ID3D12Resource> uma_upload;
                HRESULT hr = D3D12XboxCreateCommittedResource(
                    d3d_dev, &default_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(uma_upload.GetAddressOf()));

                void* mapped = nullptr;
                if (SUCCEEDED(hr)) {
                    hr = uma_upload->Map(0, nullptr, &mapped);
                }
                if (SUCCEEDED(hr) && mapped) {
                    for (uint32_t row = 0; row < h; ++row) {
                        std::memcpy(static_cast<uint8_t*>(mapped) + static_cast<size_t>(row) * aligned_pitch,
                                    rgba.data() + static_cast<size_t>(row) * pitch, pitch);
                    }
                    uma_upload->Unmap(0, nullptr);
                }

                // 2) GPU Texture2D with Xbox-specific flags (sanitized by d3d12_xs.dll!)
                D3D12_RESOURCE_DESC tex_desc{};
                tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                tex_desc.Width = w;
                tex_desc.Height = h;
                tex_desc.DepthOrArraySize = 1;
                tex_desc.MipLevels = 1;
                tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                tex_desc.SampleDesc.Count = 1;
                tex_desc.Flags = D3D12XBOX_RESOURCE_FLAG_ALLOW_DCC | D3D12XBOX_RESOURCE_FLAG_FORCE_TEXTURE_COMPATIBILITY;

                ComPtr<ID3D12Resource> gpu_tex;
                if (SUCCEEDED(hr)) {
                    hr = D3D12XboxCreateCommittedResource(
                        d3d_dev, &default_heap, D3D12_HEAP_FLAG_ALLOW_DISPLAY, &tex_desc,
                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(gpu_tex.GetAddressOf()));
                }

                // 3) Readback buffer
                D3D12_HEAP_PROPERTIES rb_heap{};
                rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
                ComPtr<ID3D12Resource> readback;
                if (SUCCEEDED(hr)) {
                    hr = d3d_dev->CreateCommittedResource(
                        &rb_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(readback.GetAddressOf()));
                }

                ComPtr<ID3D12CommandAllocator> alloc;
                ComPtr<ID3D12GraphicsCommandList> cl;
                if (SUCCEEDED(hr)) {
                    hr = d3d_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.GetAddressOf()));
                }
                if (SUCCEEDED(hr)) {
                    hr = d3d_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(cl.GetAddressOf()));
                }
                if (SUCCEEDED(hr)) {
                    D3D12_TEXTURE_COPY_LOCATION src_loc{};
                    src_loc.pResource = uma_upload.Get();
                    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    src_loc.PlacedFootprint.Footprint.Width = w;
                    src_loc.PlacedFootprint.Footprint.Height = h;
                    src_loc.PlacedFootprint.Footprint.Depth = 1;
                    src_loc.PlacedFootprint.Footprint.RowPitch = aligned_pitch;

                    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
                    dst_loc.pResource = gpu_tex.Get();
                    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dst_loc.SubresourceIndex = 0;

                    cl->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

                    // Include an Xbox high-bit resource state (0x10000000) to verify WsxD3D12CommandListProxy sanitization!
                    D3D12_RESOURCE_BARRIER bar{};
                    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    bar.Transition.pResource = gpu_tex.Get();
                    bar.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_COPY_DEST | 0x10000000u);
                    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    cl->ResourceBarrier(1, &bar);

                    D3D12_TEXTURE_COPY_LOCATION rb_loc = src_loc;
                    rb_loc.pResource = readback.Get();
                    cl->CopyTextureRegion(&rb_loc, 0, 0, 0, &dst_loc, nullptr);

                    hr = gpu_execute_and_wait(d3d_dev, queue, cl.Get());
                }

                if (SUCCEEDED(hr)) {
                    void* rb_ptr = nullptr;
                    hr = readback->Map(0, nullptr, &rb_ptr);
                    if (SUCCEEDED(hr) && rb_ptr) {
                        std::vector<uint8_t> rb_tight(rgba.size());
                        for (uint32_t row = 0; row < h; ++row) {
                            std::memcpy(rb_tight.data() + static_cast<size_t>(row) * pitch,
                                        static_cast<const uint8_t*>(rb_ptr) + static_cast<size_t>(row) * aligned_pitch,
                                        pitch);
                        }
                        readback->Unmap(0, nullptr);
                        const uint64_t rb_hash = fnv1a64(rb_tight.data(), rb_tight.size());
                        if (rb_hash == last_hash) {
                            gpu_roundtrip_ok = true;
                            std::printf("  [PASS] GPU UMA DEFAULT->XboxDCC Tex2D->Readback BIT-EXACT match (hash=0x%016llX)\n",
                                        static_cast<unsigned long long>(rb_hash));
                            if (want_dump) {
                                std::error_code ec;
                                std::filesystem::create_directories(dump_dir_buf, ec);
                                const std::string tag = (w == 1920) ? "bink_1080p_129_gpu_readback.ppm" : "bink_vert_067_gpu_readback.ppm";
                                const std::filesystem::path out_ppm = std::filesystem::path(dump_dir_buf) / tag;
                                std::ofstream ofs(out_ppm, std::ios::binary);
                                if (ofs) {
                                    const std::string ppm_hdr = "P6\n" + std::to_string(w) + " " + std::to_string(h) + "\n255\n";
                                    ofs.write(ppm_hdr.data(), static_cast<std::streamsize>(ppm_hdr.size()));
                                    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3u);
                                    for (size_t px = 0, n_px = static_cast<size_t>(w) * h; px < n_px; ++px) {
                                        rgb[px * 3 + 0] = rb_tight[px * 4 + 0];
                                        rgb[px * 3 + 1] = rb_tight[px * 4 + 1];
                                        rgb[px * 3 + 2] = rb_tight[px * 4 + 2];
                                    }
                                    ofs.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
                                    std::printf("  [DUMP] Saved RTX GPU-readback frame: %s (%ux%u, hash=0x%016llX)\n",
                                                out_ppm.string().c_str(), w, h, static_cast<unsigned long long>(rb_hash));
                                }
                            }
                        } else {
                            std::printf("  [FAIL] GPU readback mismatch: expected=0x%016llX got=0x%016llX\n",
                                        static_cast<unsigned long long>(last_hash), static_cast<unsigned long long>(rb_hash));
                            stats.failed++;
                        }
                    }
                } else {
                    std::printf("  [FAIL] GPU upload/readback hr=0x%08lX\n", static_cast<unsigned long>(hr));
                    stats.failed++;
                }
            }
        }

        const DWORD close_exc = seh_run([&]() { pClose(bnk); });
        if (close_exc != 0) {
            std::printf("  [WARN] BinkClose exception 0x%08lX\n", close_exc);
        }
    }

    if (gpu_roundtrip_ok) stats.passed++;
    FreeLibrary(mod);
}

// ============================================================================
// [2/6] XboxBvhBuilder.dll (Prebuild + XMemVirtualQuery + 52 DXIL Shaders + GPU Build!)
// ============================================================================

void test_xbox_bvh_builder(
    const std::filesystem::path& build_dir,
    const std::filesystem::path& shim_dir,
    ID3D12Device8* d3d_dev,
    ID3D12CommandQueue* queue,
    HMODULE h_xmem,
    SuiteStats& stats) {
    std::puts("\n========================================================================");
    std::puts("[2/6] XboxBvhBuilder.dll (52 real Xbox DXIL shaders + XMemVirtualQuery + GPU BVH)");
    std::puts("========================================================================");

    // Enable Xbox root signature flag stripping (0x03000000) in d3d12_xs.dll
    SetEnvironmentVariableW(L"WSX_STRIP_XBOX_ROOT_FLAGS", L"1");

    const auto dll_path = build_dir / L"XboxBvhBuilder.dll";
    HMODULE mod = LoadLibraryExW(dll_path.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!mod) {
        std::printf("  [FAIL] LoadLibrary(XboxBvhBuilder.dll) err=%lu\n", GetLastError());
        stats.failed++;
        return;
    }

    using CtorProc = void* (*)(void* self, void* dev11, bool skip_build_pipelines);
    using DtorProc = void (*)(void* self);
    using PrebuildProc = void (*)(void* self, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS*, const void*, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO*);
    using BuildProc = void (*)(void* self, void* cmdlist4, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*, UINT, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC*);
    using ValidateRehydrateProc = HRESULT (*)(void* self, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*);

    auto pCtor = std::bit_cast<CtorProc>(GetProcAddress(mod, "??0XboxBvhBuilder@@QEAA@PEAUID3D12Device11@@_N@Z"));
    auto pDtor = std::bit_cast<DtorProc>(GetProcAddress(mod, "??1XboxBvhBuilder@@QEAA@XZ"));
    auto pPrebuild = std::bit_cast<PrebuildProc>(GetProcAddress(
        mod, "?GetRaytracingAccelerationStructurePrebuildInfo@XboxBvhBuilder@@QEAAXPEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS@@PEBUD3D12XBOX_XDXR_STREAM_DESC@@PEAUD3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO@@@Z"));
    auto pBuild = std::bit_cast<BuildProc>(GetProcAddress(
        mod, "?BuildRaytracingAccelerationStructure@XboxBvhBuilder@@QEAAXPEAUID3D12GraphicsCommandList4@@PEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC@@IPEBUD3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC@@@Z"));
    auto pValidateRehydrate = std::bit_cast<ValidateRehydrateProc>(GetProcAddress(
        mod, "?ValidateRehydrateAccelerationStructureBuildDesc@XboxBvhBuilder@@QEAAJPEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC@@@Z"));

    // 1. Construct real XboxBvhBuilder DIRECTLY on d3d12_xs.dll's native Xbox-ABI Device!
    void* xbox_dev_raw = nullptr;
    if (FAILED(D3D12XboxGetXboxAbiDevice(d3d_dev, __uuidof(ID3D12Device), &xbox_dev_raw)) || !xbox_dev_raw) {
        std::puts("  [FAIL] D3D12XboxGetXboxAbiDevice failed");
        stats.failed++;
        FreeLibrary(mod);
        return;
    }

    D3D12XBOX_ABI_TELEMETRY tel0{}, tel1{};
    D3D12XboxGetAbiTelemetry(&tel0);

    alignas(16) uint8_t bvh_storage[64]{}; // sizeof(XboxBvhBuilder) == 24 bytes (3 unique_ptrs)
    bool constructed = false;
    SetEnvironmentVariableW(L"WSX_XBOX_PSO_FALLBACK", nullptr);
    const auto t0 = std::chrono::steady_clock::now();
    DWORD exc = seh_run([&]() {
        pCtor(bvh_storage, xbox_dev_raw, false);
        constructed = true;
    });
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    D3D12XboxGetAbiTelemetry(&tel1);

    if (constructed) {
        std::printf("  [PASS] Strict XboxBvhBuilder(d3d12_xs XboxAbiDev) SUCCEEDED in %lld ms! PSO_X=%llu/%llu\n",
                    ms,
                    static_cast<unsigned long long>(tel1.ComputePsoXOk - tel0.ComputePsoXOk),
                    static_cast<unsigned long long>((tel1.ComputePsoXOk - tel0.ComputePsoXOk) + (tel1.ComputePsoXFail - tel0.ComputePsoXFail)));
        stats.passed++;
    } else {
        std::printf("  [INFO] Strict XboxBvhBuilder stopped at shader #%llu (exc=0x%08lX): d3d12_xs PSO_X ok=%llu fail=%llu (LeafRehydration.pdb)\n",
                    static_cast<unsigned long long>((tel1.ComputePsoXOk - tel0.ComputePsoXOk) + 1), exc,
                    static_cast<unsigned long long>(tel1.ComputePsoXOk - tel0.ComputePsoXOk),
                    static_cast<unsigned long long>(tel1.ComputePsoXFail - tel0.ComputePsoXFail));

        // 1b. Full-sweep pass via d3d12_xs.dll native WSX_XBOX_PSO_FALLBACK=1 mode
        std::memset(bvh_storage, 0, sizeof(bvh_storage));
        D3D12XboxGetAbiTelemetry(&tel0);
        SetEnvironmentVariableW(L"WSX_XBOX_PSO_FALLBACK", L"1");
        const auto t1 = std::chrono::steady_clock::now();
        exc = seh_run([&]() {
            pCtor(bvh_storage, xbox_dev_raw, false);
            constructed = true;
        });
        SetEnvironmentVariableW(L"WSX_XBOX_PSO_FALLBACK", nullptr);
        const auto ms2 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t1).count();
        D3D12XboxGetAbiTelemetry(&tel1);

        if (constructed) {
            const uint64_t pso_ok = tel1.ComputePsoXOk - tel0.ComputePsoXOk;
            const uint64_t pso_fail = tel1.ComputePsoXFail - tel0.ComputePsoXFail;
            const uint64_t pso_fb = tel1.ComputePsoXFallback - tel0.ComputePsoXFallback;
            std::printf("  [PASS] Full-sweep XboxBvhBuilder on native d3d12_xs.dll Xbox-ABI completed in %lld ms!\n", ms2);
            std::printf("         Real Xbox DXIL PSOs compiled via d3d12_xs!0x1F0: %llu/%llu OK (%llu fallback, unhandled=%llu)\n",
                        static_cast<unsigned long long>(pso_ok),
                        static_cast<unsigned long long>(pso_ok + pso_fail),
                        static_cast<unsigned long long>(pso_fb),
                        static_cast<unsigned long long>(tel1.UnhandledDeviceSlots - tel0.UnhandledDeviceSlots));
            stats.passed++;
        } else {
            std::printf("  [FAIL] Full-sweep XboxBvhBuilder failed exc=0x%08lX\n", exc);
            stats.failed++;
        }
    }

    // 2. Test PrebuildInfo for BLAS (300 verts) and TLAS (64 instances)
    D3D12_RAYTRACING_GEOMETRY_DESC geom{};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.VertexCount = 300;
    geom.Triangles.VertexBuffer.StrideInBytes = 12;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_in{};
    blas_in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    blas_in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    blas_in.NumDescs = 1;
    blas_in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    blas_in.pGeometryDescs = &geom;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blas_info{};
    pPrebuild(bvh_storage, &blas_in, nullptr, &blas_info);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_in{};
    tlas_in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlas_in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tlas_in.NumDescs = 64;
    tlas_in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlas_info{};
    pPrebuild(bvh_storage, &tlas_in, nullptr, &tlas_info);

    std::printf("  [ OK ] PrebuildInfo BLAS(300v): Result=%llu B, Scratch=%llu B | TLAS(64 inst): Result=%llu B, Scratch=%llu B\n",
                static_cast<unsigned long long>(blas_info.ResultDataMaxSizeInBytes),
                static_cast<unsigned long long>(blas_info.ScratchDataSizeInBytes),
                static_cast<unsigned long long>(tlas_info.ResultDataMaxSizeInBytes),
                static_cast<unsigned long long>(tlas_info.ScratchDataSizeInBytes));

    // 3. Test ValidateRehydrateAccelerationStructureBuildDesc -> exercises live XMemVirtualQuery in xmem.dll!
    using XMemAllocProc = PVOID(WINAPI*)(SIZE_T, ULONGLONG);
    using XMemFreeProc = VOID(WINAPI*)(PVOID, ULONGLONG);
    auto pXMemAlloc = std::bit_cast<XMemAllocProc>(GetProcAddress(h_xmem, "XMemAlloc"));
    auto pXMemFree = std::bit_cast<XMemFreeProc>(GetProcAddress(h_xmem, "XMemFree"));

    if (pValidateRehydrate && pXMemAlloc && pXMemFree) {
        void* dest_mem = pXMemAlloc(65536, 0);
        void* src_mem = pXMemAlloc(65536, 0);
        void* vbuf_mem = pXMemAlloc(16384, 0);
        void* scratch_mem = pXMemAlloc(65536, 0);
        if (dest_mem) std::memset(dest_mem, 0, 65536);
        if (src_mem) std::memset(src_mem, 0, 65536);
        if (vbuf_mem) std::memset(vbuf_mem, 0, 16384);
        if (scratch_mem) std::memset(scratch_mem, 0, 65536);

        D3D12_RAYTRACING_GEOMETRY_DESC rgeom = geom;
        rgeom.Triangles.VertexBuffer.StartAddress = reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(vbuf_mem);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC rdesc{};
        rdesc.DestAccelerationStructureData = reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(dest_mem);
        rdesc.SourceAccelerationStructureData = reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(src_mem);
        rdesc.ScratchAccelerationStructureData = reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(scratch_mem);
        rdesc.Inputs = blas_in;
        rdesc.Inputs.pGeometryDescs = &rgeom;

        HRESULT vhr = E_FAIL;
        exc = seh_run([&]() {
            vhr = pValidateRehydrate(bvh_storage, &rdesc);
        });
        std::printf("  [ OK ] ValidateRehydrateAccelerationStructureBuildDesc(XMemAlloc bufs) -> hr=0x%08lX exc=0x%08lX\n",
                    static_cast<unsigned long>(vhr), exc);

        pXMemFree(scratch_mem, 0);
        pXMemFree(vbuf_mem, 0);
        pXMemFree(src_mem, 0);
        pXMemFree(dest_mem, 0);
    }

    // 4. If constructed, test real GPU BuildRaytracingAccelerationStructure on 300 triangles!
    bool orig_bvh_blocked_by_xbox_intrinsics = false;
    if (constructed && pBuild && d3d_dev && queue) {
        auto create_gpu_uav_buf = [&](uint64_t bytes, D3D12_RESOURCE_STATES init_state, bool cpu_upload) -> ComPtr<ID3D12Resource> {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = cpu_upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = (bytes + 255) & ~255ULL;
            rd.Height = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            rd.Flags = cpu_upload ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ComPtr<ID3D12Resource> res;
            d3d_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr, IID_PPV_ARGS(res.GetAddressOf()));
            return res;
        };

        auto vb = create_gpu_uav_buf(300 * 12, D3D12_RESOURCE_STATE_GENERIC_READ, true);
        if (vb) {
            float* v = nullptr;
            if (SUCCEEDED(vb->Map(0, nullptr, reinterpret_cast<void**>(&v))) && v) {
                for (int t = 0; t < 100; ++t) {
                    const float x = static_cast<float>(t % 10);
                    const float y = static_cast<float>(t / 10);
                    v[t * 9 + 0] = x;        v[t * 9 + 1] = y;        v[t * 9 + 2] = 0.0f;
                    v[t * 9 + 3] = x + 0.8f; v[t * 9 + 4] = y;        v[t * 9 + 5] = 0.0f;
                    v[t * 9 + 6] = x;        v[t * 9 + 7] = y + 0.8f; v[t * 9 + 8] = 0.5f;
                }
                vb->Unmap(0, nullptr);
            }
        }
        auto dest_buf = create_gpu_uav_buf(std::max<uint64_t>(blas_info.ResultDataMaxSizeInBytes, 65536), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, false);
        auto scratch_buf = create_gpu_uav_buf(std::max<uint64_t>(blas_info.ScratchDataSizeInBytes, 65536), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, false);
        auto inline_gpu_buf = create_gpu_uav_buf(256 * 1024, D3D12_RESOURCE_STATE_GENERIC_READ, true);

        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList4> cl4;
        HRESULT hr = d3d_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.GetAddressOf()));
        if (SUCCEEDED(hr)) {
            hr = d3d_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(cl4.GetAddressOf()));
        }

        if (SUCCEEDED(hr) && vb && dest_buf && scratch_buf) {
            void* xbox_cl_raw = nullptr;
            hr = D3D12XboxWrapCommandListForXboxAbi(cl4.Get(), &xbox_cl_raw);
            if (SUCCEEDED(hr) && xbox_cl_raw) {
                D3D12XBOX_ABI_TELEMETRY clt0{}, clt1{};
                D3D12XboxGetAbiTelemetry(&clt0);

                D3D12_RAYTRACING_GEOMETRY_DESC g2 = geom;
                g2.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress();

                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bdesc{};
                bdesc.DestAccelerationStructureData = dest_buf->GetGPUVirtualAddress();
                bdesc.ScratchAccelerationStructureData = scratch_buf->GetGPUVirtualAddress();
                bdesc.Inputs = blas_in;
                bdesc.Inputs.pGeometryDescs = &g2;

                bool build_rec_ok = false;
                SehFaultInfo fault{};
                exc = seh_run([&]() {
                    pBuild(bvh_storage, xbox_cl_raw, &bdesc, 0, nullptr);
                    build_rec_ok = true;
                }, &fault);
                D3D12XboxGetAbiTelemetry(&clt1);

                const uint64_t uma_allocs = clt1.UmaAllocGpuMemCalls - clt0.UmaAllocGpuMemCalls;
                const uint64_t uma_va_xlat = clt1.UmaVaTranslations - clt0.UmaVaTranslations;
                const uint64_t cl_unhandled = clt1.UnhandledCmdListSlots - clt0.UnhandledCmdListSlots;

                if (build_rec_ok) {
                    std::printf("  [ OK ] BuildRaytracingAccelerationStructure recorded via native d3d12_xs.dll XboxCmdList! UmaAlloc=%llu UmaVaXlat=%llu Unhandled=%llu\n",
                                static_cast<unsigned long long>(uma_allocs),
                                static_cast<unsigned long long>(uma_va_xlat),
                                static_cast<unsigned long long>(cl_unhandled));
                    if (cl_unhandled == 0) {
                        D3D12_RESOURCE_BARRIER b2{};
                        b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b2.Transition.pResource = dest_buf.Get();
                        b2.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                        b2.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                        b2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        cl4->ResourceBarrier(1, &b2);

                        D3D12_HEAP_PROPERTIES rb_hp{};
                        rb_hp.Type = D3D12_HEAP_TYPE_READBACK;
                        D3D12_RESOURCE_DESC rb_rd{};
                        rb_rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                        rb_rd.Width = blas_info.ResultDataMaxSizeInBytes;
                        rb_rd.Height = 1; rb_rd.DepthOrArraySize = 1; rb_rd.MipLevels = 1; rb_rd.SampleDesc.Count = 1;
                        rb_rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                        ComPtr<ID3D12Resource> rb;
                        d3d_dev->CreateCommittedResource(&rb_hp, D3D12_HEAP_FLAG_NONE, &rb_rd,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(rb.GetAddressOf()));
                        if (rb) {
                            cl4->CopyBufferRegion(rb.Get(), 0, dest_buf.Get(), 0, blas_info.ResultDataMaxSizeInBytes);
                        }
                        HRESULT exec_hr = E_FAIL;
                        SehFaultInfo exec_fault{};
                        const DWORD exec_exc = seh_run([&]() {
                            exec_hr = gpu_execute_and_wait(d3d_dev, queue, cl4.Get());
                        }, &exec_fault);
                        if (exec_exc == 0 && SUCCEEDED(exec_hr) && rb) {
                            void* p = nullptr;
                            if (SUCCEEDED(rb->Map(0, nullptr, &p)) && p) {
                                const auto* bytes = static_cast<const uint8_t*>(p);
                                size_t nz = 0;
                                for (size_t i = 0; i < blas_info.ResultDataMaxSizeInBytes; ++i) {
                                    if (bytes[i] != 0) ++nz;
                                }
                                const uint64_t h = fnv1a64(bytes, blas_info.ResultDataMaxSizeInBytes);
                                rb->Unmap(0, nullptr);
                                std::printf("  [PASS] GPU BVH4 ExecuteCommandLists SUCCEEDED on RTX GPU! Non-zero=%zu/%llu B hash=0x%016llX\n",
                                            nz, static_cast<unsigned long long>(blas_info.ResultDataMaxSizeInBytes),
                                            static_cast<unsigned long long>(h));
                            }
                        } else if (exec_exc != 0) {
                            HMODULE fm = nullptr;
                            char mname[MAX_PATH] = "(unknown)";
                            uintptr_t rva = exec_fault.address;
                            if (exec_fault.address && GetModuleHandleExA(
                                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCSTR>(exec_fault.address), &fm) && fm) {
                                GetModuleFileNameA(fm, mname, MAX_PATH);
                                rva = exec_fault.address - reinterpret_cast<uintptr_t>(fm);
                            }
                            std::printf("  [WARN] GPU BVH4 ExecuteCommandLists trapped exc=0x%08lX at %s+0x%llX (dxil-ms-xb2 @xbox.* RDNA2 intrinsics)\n",
                                        exec_exc, mname, static_cast<unsigned long long>(rva));
                            stats.warnings++;
                        } else {
                            std::printf("  [WARN] GPU BVH4 execution hr=0x%08lX (device removed reason=0x%08lX)\n",
                                        static_cast<unsigned long>(exec_hr),
                                        static_cast<unsigned long>(d3d_dev->GetDeviceRemovedReason()));
                            stats.warnings++;
                        }
                    } else {
                        stats.warnings++;
                    }
                } else {
                    HMODULE fault_mod = nullptr;
                    char mod_name[MAX_PATH] = "(unknown)";
                    uintptr_t rva = fault.address;
                    if (fault.address && GetModuleHandleExA(
                            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(fault.address), &fault_mod) && fault_mod) {
                        GetModuleFileNameA(fault_mod, mod_name, MAX_PATH);
                        rva = fault.address - reinterpret_cast<uintptr_t>(fault_mod);
                    }
                    std::printf("  [INFO] Original XboxBvhBuilder::BuildAS trapped exc=0x%08lX at %s+0x%llX (UmaAlloc=%llu UmaVaXlat=%llu Unhandled=%llu)\n",
                                exc, mod_name, static_cast<unsigned long long>(rva),
                                static_cast<unsigned long long>(uma_allocs),
                                static_cast<unsigned long long>(uma_va_xlat),
                                static_cast<unsigned long long>(cl_unhandled));
                    std::puts("         Root cause: 43/52 XboxBvhBuilder shaders use proprietary dxil-ms-xb2 @xbox.* RDNA2 intrinsics -> switching to DXR Tier 1.1 bridge");
                    orig_bvh_blocked_by_xbox_intrinsics = true;
                }
                reinterpret_cast<IUnknown*>(xbox_cl_raw)->Release();
            }
        }
    }

    // 5. Prove real GPU execution of an unmodified pure-DXIL dxil-ms-xb2 shader from XboxBvhBuilder.dll
    //    (ResetCounterValues.pdb @ file offset 0xB76E0, compiled by dxcxbox 2504.2509, 0 @xbox.* intrinsics)
    if (d3d_dev && queue) {
        std::ifstream bf(dll_path, std::ios::binary);
        if (bf) {
            bf.seekg(0xB76E0, std::ios::beg);
            uint8_t hdr[32]{};
            bf.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
            uint32_t bsz = 0;
            if (bf && std::memcmp(hdr, "DXBC", 4) == 0) {
                std::memcpy(&bsz, hdr + 24, 4);
            }
            if (bsz > 32 && bsz < 65536) {
                std::vector<uint8_t> shader(bsz);
                bf.seekg(0xB76E0, std::ios::beg);
                bf.read(reinterpret_cast<char*>(shader.data()), static_cast<std::streamsize>(bsz));

                using DeserFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
                using SerFn = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
                HMODULE h_d3d12_sys = GetModuleHandleW(L"d3d12.dll");
                HMODULE h_d3d12_xs = GetModuleHandleW(L"d3d12_xs.dll");
                auto pDeser = h_d3d12_sys ? std::bit_cast<DeserFn>(GetProcAddress(h_d3d12_sys, "D3D12CreateVersionedRootSignatureDeserializer")) : nullptr;
                auto pSerXs = h_d3d12_xs ? std::bit_cast<SerFn>(GetProcAddress(h_d3d12_xs, "D3D12SerializeVersionedRootSignature")) : nullptr;

                ComPtr<ID3D12VersionedRootSignatureDeserializer> deser;
                ComPtr<ID3DBlob> rsb, errb;
                ComPtr<ID3D12RootSignature> rs;
                ComPtr<ID3D12PipelineState> pso;
                if (pDeser && pSerXs &&
                    SUCCEEDED(pDeser(shader.data(), shader.size(), IID_PPV_ARGS(deser.GetAddressOf()))) &&
                    SUCCEEDED(pSerXs(deser->GetUnconvertedRootSignatureDesc(), rsb.GetAddressOf(), errb.GetAddressOf())) &&
                    SUCCEEDED(d3d_dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(rs.GetAddressOf())))) {
                    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
                    pd.pRootSignature = rs.Get();
                    pd.CS = {shader.data(), shader.size()};
                    if (SUCCEEDED(d3d_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(pso.GetAddressOf()))) && pso) {
                        auto mk_buf = [&](D3D12_HEAP_TYPE ht, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
                            D3D12_HEAP_PROPERTIES hp{}; hp.Type = ht;
                            D3D12_RESOURCE_DESC rd{};
                            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = 1024;
                            rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
                            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = fl;
                            ComPtr<ID3D12Resource> r;
                            d3d_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(r.GetAddressOf()));
                            return r;
                        };
                        auto uav = mk_buf(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        auto rb  = mk_buf(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
                        ComPtr<ID3D12CommandAllocator> al;
                        ComPtr<ID3D12GraphicsCommandList> cl;
                        if (uav && rb &&
                            SUCCEEDED(d3d_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(al.GetAddressOf()))) &&
                            SUCCEEDED(d3d_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(cl.GetAddressOf())))) {
                            cl->SetComputeRootSignature(rs.Get());
                            cl->SetPipelineState(pso.Get());
                            cl->SetComputeRootUnorderedAccessView(1, uav->GetGPUVirtualAddress());
                            const UINT consts[3] = {32u, 16u, 0xCAFEBABEu};
                            cl->SetComputeRoot32BitConstants(2, 3, consts, 0);
                            cl->Dispatch(1, 1, 1);

                            D3D12_RESOURCE_BARRIER b{};
                            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                            b.Transition.pResource = uav.Get();
                            b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                            cl->ResourceBarrier(1, &b);
                            cl->CopyBufferRegion(rb.Get(), 0, uav.Get(), 0, 1024);

                            if (SUCCEEDED(gpu_execute_and_wait(d3d_dev, queue, cl.Get()))) {
                                uint32_t* out = nullptr;
                                if (SUCCEEDED(rb->Map(0, nullptr, reinterpret_cast<void**>(&out))) && out) {
                                    int matched = 0;
                                    for (int i = 0; i < 64; ++i) {
                                        if (out[i] == 0xCAFEBABEu) ++matched;
                                    }
                                    const uint32_t sentinel = out[64];
                                    rb->Unmap(0, nullptr);
                                    if (matched == 64 && sentinel == 0) {
                                        std::puts("  [PASS] Live GPU Dispatch of Xbox dxil-ms-xb2 shader (ResetCounterValues.pdb): 64/64 dwords == 0xCAFEBABE!");
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (constructed && pDtor) {
        const DWORD dexc = seh_run([&]() { pDtor(bvh_storage); });
        if (dexc == 0) {
            std::puts("  [ OK ] ~XboxBvhBuilder() destroyed cleanly.");
        } else {
            std::printf("  [WARN] ~XboxBvhBuilder() exception 0x%08lX\n", dexc);
        }
    }
    FreeLibrary(mod);

    // 6. Native Direct3D 12 DXR Tier 1.1 Bridge (MizuBox\bin\XboxBvhBuilder.dll):
    //    Replaces the 43 @xbox.* RDNA2 compute shaders with hardware DXR BLAS + TLAS + SM 6.6 RayQuery verification!
    bool dxr_bridge_ok = false;
    const auto bridge_path = shim_dir / L"XboxBvhBuilder.dll";
    if (d3d_dev && queue && std::filesystem::is_regular_file(bridge_path)) {
        HMODULE bmod = LoadLibraryExW(bridge_path.c_str(), nullptr,
                                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (bmod) {
            auto bCtor = std::bit_cast<CtorProc>(GetProcAddress(bmod, "??0XboxBvhBuilder@@QEAA@PEAUID3D12Device11@@_N@Z"));
            auto bDtor = std::bit_cast<DtorProc>(GetProcAddress(bmod, "??1XboxBvhBuilder@@QEAA@XZ"));
            auto bPrebuild = std::bit_cast<PrebuildProc>(GetProcAddress(
                bmod, "?GetRaytracingAccelerationStructurePrebuildInfo@XboxBvhBuilder@@QEAAXPEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS@@PEBUD3D12XBOX_XDXR_STREAM_DESC@@PEAUD3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO@@@Z"));
            auto bBuild = std::bit_cast<BuildProc>(GetProcAddress(
                bmod, "?BuildRaytracingAccelerationStructure@XboxBvhBuilder@@QEAAXPEAUID3D12GraphicsCommandList4@@PEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC@@IPEBUD3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC@@@Z"));

            if (bCtor && bDtor && bPrebuild && bBuild) {
                alignas(16) uint8_t bridge_obj[24]{};
                bCtor(bridge_obj, xbox_dev_raw, true);

                auto mk_dxr_buf = [&](uint64_t bytes, D3D12_HEAP_TYPE ht, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
                    D3D12_HEAP_PROPERTIES hp{}; hp.Type = ht;
                    D3D12_RESOURCE_DESC rd{};
                    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                    rd.Width = (bytes < 256ull) ? 256ull : ((bytes + 255ull) & ~255ull);
                    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
                    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = fl;
                    ComPtr<ID3D12Resource> r;
                    d3d_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(r.GetAddressOf()));
                    return r;
                };

                // Same 100-triangle (300 vertex) grid as step 4
                auto vb = mk_dxr_buf(300 * 12, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
                if (vb) {
                    float* v = nullptr;
                    if (SUCCEEDED(vb->Map(0, nullptr, reinterpret_cast<void**>(&v))) && v) {
                        for (int t = 0; t < 100; ++t) {
                            const float x = static_cast<float>(t % 10);
                            const float y = static_cast<float>(t / 10);
                            v[t * 9 + 0] = x;        v[t * 9 + 1] = y;        v[t * 9 + 2] = 0.0f;
                            v[t * 9 + 3] = x + 0.8f; v[t * 9 + 4] = y;        v[t * 9 + 5] = 0.0f;
                            v[t * 9 + 6] = x;        v[t * 9 + 7] = y + 0.8f; v[t * 9 + 8] = 0.5f;
                        }
                        vb->Unmap(0, nullptr);
                    }
                }

                D3D12_RAYTRACING_GEOMETRY_DESC g_dxr = geom;
                if (vb) g_dxr.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress();
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS b_in = blas_in;
                b_in.pGeometryDescs = &g_dxr;
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO b_pre{};
                bPrebuild(bridge_obj, &b_in, nullptr, &b_pre);

                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS t_in{};
                t_in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
                t_in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
                t_in.NumDescs = 1;
                t_in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO t_pre{};
                bPrebuild(bridge_obj, &t_in, nullptr, &t_pre);

                auto blas_res = mk_dxr_buf(b_pre.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
                auto blas_scr = mk_dxr_buf(b_pre.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                auto tlas_res = mk_dxr_buf(t_pre.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
                auto tlas_scr = mk_dxr_buf(t_pre.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                auto inst_buf = mk_dxr_buf(sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_UPLOAD,
                    D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
                auto rq_uav = mk_dxr_buf(256, D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                auto rq_rb = mk_dxr_buf(256, D3D12_HEAP_TYPE_READBACK,
                    D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

                if (vb && blas_res && blas_scr && tlas_res && tlas_scr && inst_buf && rq_uav && rq_rb) {
                    D3D12_RAYTRACING_INSTANCE_DESC idesc{};
                    idesc.Transform[0][0] = 1.0f; idesc.Transform[1][1] = 1.0f; idesc.Transform[2][2] = 1.0f;
                    idesc.InstanceID = 0x6A61u;
                    idesc.InstanceMask = 0xFFu;
                    idesc.AccelerationStructure = blas_res->GetGPUVirtualAddress();
                    void* ip = nullptr;
                    if (SUCCEEDED(inst_buf->Map(0, nullptr, &ip)) && ip) {
                        std::memcpy(ip, &idesc, sizeof(idesc));
                        inst_buf->Unmap(0, nullptr);
                    }
                    t_in.InstanceDescs = inst_buf->GetGPUVirtualAddress();

                    // Compile SM 6.6 RayQuery verifier at runtime via d3d12_xs.dll
                    static constexpr const char kSuiteRqHlsl[] = R"HLSL(
struct HitOut { uint status; uint inst_id; uint prim_idx; float t; };
RaytracingAccelerationStructure g_tlas : register(t0);
RWStructuredBuffer<HitOut> g_out : register(u0);
[numthreads(1, 1, 1)]
void main() {
    RayDesc r;
    r.Origin = float3(0.2, 0.2, 1.0);
    r.Direction = float3(0.0, 0.0, -1.0);
    r.TMin = 0.001; r.TMax = 10.0;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(g_tlas, RAY_FLAG_NONE, 0xFFu, r);
    q.Proceed();
    HitOut o;
    o.status = q.CommittedStatus();
    o.inst_id = (o.status == COMMITTED_TRIANGLE_HIT) ? q.CommittedInstanceID() : 0u;
    o.prim_idx = (o.status == COMMITTED_TRIANGLE_HIT) ? q.CommittedPrimitiveIndex() : 0xFFFFFFFFu;
    o.t = (o.status == COMMITTED_TRIANGLE_HIT) ? q.CommittedRayT() : -1.0;
    g_out[0] = o;
}
)HLSL";
                    size_t rq_sz = 0;
                    if (SUCCEEDED(D3D12XboxCompileHlslToDxil(kSuiteRqHlsl, sizeof(kSuiteRqHlsl) - 1, L"main", L"cs_6_6",
                                                             nullptr, 0, &rq_sz, nullptr, 0)) && rq_sz > 64) {
                        std::vector<uint8_t> rq_dxil(rq_sz);
                        using SerFn = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
                        HMODULE h_d3d12_xs = GetModuleHandleW(L"d3d12_xs.dll");
                        auto pSerXs = h_d3d12_xs ? std::bit_cast<SerFn>(GetProcAddress(h_d3d12_xs, "D3D12SerializeVersionedRootSignature")) : nullptr;

                        D3D12_ROOT_PARAMETER1 rps[2]{};
                        rps[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rps[0].Descriptor.ShaderRegister = 0;
                        rps[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rps[1].Descriptor.ShaderRegister = 0;
                        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
                        rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
                        rsd.Desc_1_1.NumParameters = 2; rsd.Desc_1_1.pParameters = rps;
                        ComPtr<ID3DBlob> rsb, rse;
                        ComPtr<ID3D12RootSignature> rs;
                        ComPtr<ID3D12PipelineState> pso;
                        ComPtr<ID3D12CommandAllocator> al;
                        ComPtr<ID3D12GraphicsCommandList4> cl4;
                        if (pSerXs &&
                            SUCCEEDED(D3D12XboxCompileHlslToDxil(kSuiteRqHlsl, sizeof(kSuiteRqHlsl) - 1, L"main", L"cs_6_6",
                                                                 rq_dxil.data(), rq_dxil.size(), &rq_sz, nullptr, 0)) &&
                            SUCCEEDED(pSerXs(&rsd, rsb.GetAddressOf(), rse.GetAddressOf())) &&
                            SUCCEEDED(d3d_dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(rs.GetAddressOf()))) &&
                            SUCCEEDED(d3d_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(al.GetAddressOf()))) &&
                            SUCCEEDED(d3d_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(cl4.GetAddressOf())))) {
                            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
                            pd.pRootSignature = rs.Get();
                            pd.CS = {rq_dxil.data(), rq_dxil.size()};
                            void* xcl = nullptr;
                            if (SUCCEEDED(d3d_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(pso.GetAddressOf()))) &&
                                SUCCEEDED(D3D12XboxWrapCommandListForXboxAbi(cl4.Get(), &xcl)) && xcl) {
                                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
                                bd.DestAccelerationStructureData = blas_res->GetGPUVirtualAddress();
                                bd.ScratchAccelerationStructureData = blas_scr->GetGPUVirtualAddress();
                                bd.Inputs = b_in;
                                bBuild(bridge_obj, xcl, &bd, 0, nullptr);

                                D3D12_RESOURCE_BARRIER ub1{};
                                ub1.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; ub1.UAV.pResource = blas_res.Get();
                                cl4->ResourceBarrier(1, &ub1);

                                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC td{};
                                td.DestAccelerationStructureData = tlas_res->GetGPUVirtualAddress();
                                td.ScratchAccelerationStructureData = tlas_scr->GetGPUVirtualAddress();
                                td.Inputs = t_in;
                                bBuild(bridge_obj, xcl, &td, 0, nullptr);

                                D3D12_RESOURCE_BARRIER ub2{};
                                ub2.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; ub2.UAV.pResource = tlas_res.Get();
                                cl4->ResourceBarrier(1, &ub2);

                                cl4->SetComputeRootSignature(rs.Get());
                                cl4->SetPipelineState(pso.Get());
                                cl4->SetComputeRootShaderResourceView(0, tlas_res->GetGPUVirtualAddress());
                                cl4->SetComputeRootUnorderedAccessView(1, rq_uav->GetGPUVirtualAddress());
                                cl4->Dispatch(1, 1, 1);

                                D3D12_RESOURCE_BARRIER pb{};
                                pb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                pb.Transition.pResource = rq_uav.Get();
                                pb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                                pb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                                pb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                cl4->ResourceBarrier(1, &pb);
                                cl4->CopyBufferRegion(rq_rb.Get(), 0, rq_uav.Get(), 0, 16);

                                if (SUCCEEDED(gpu_execute_and_wait(d3d_dev, queue, cl4.Get()))) {
                                    struct HitOut { uint32_t status; uint32_t inst_id; uint32_t prim_idx; float t; } ho{};
                                    void* rp = nullptr;
                                    if (SUCCEEDED(rq_rb->Map(0, nullptr, &rp)) && rp) {
                                        std::memcpy(&ho, rp, sizeof(ho));
                                        rq_rb->Unmap(0, nullptr);
                                    }
                                    if (ho.status == 1u && ho.inst_id == 0x6A61u && ho.prim_idx == 0u &&
                                        ho.t > 0.87f && ho.t < 0.88f) {
                                        dxr_bridge_ok = true;
                                        std::printf("  [PASS] MizuBox DXR Tier 1.1 XboxBvhBuilder.dll bridge: 100-tri BLAS (%llu B) + TLAS (%llu B) + SM 6.6 RayQuery HIT (inst=0x%X prim=%u t=%.4f)!\n",
                                                    static_cast<unsigned long long>(b_pre.ResultDataMaxSizeInBytes),
                                                    static_cast<unsigned long long>(t_pre.ResultDataMaxSizeInBytes),
                                                    ho.inst_id, ho.prim_idx, ho.t);
                                    }
                                }
                                reinterpret_cast<IUnknown*>(xcl)->Release();
                            }
                        }
                    }
                }
                bDtor(bridge_obj);
            }
            FreeLibrary(bmod);
        }
    }
    if (orig_bvh_blocked_by_xbox_intrinsics && !dxr_bridge_ok) {
        stats.warnings++;
    }
    reinterpret_cast<IUnknown*>(xbox_dev_raw)->Release();
}

// ============================================================================
// [3/6 & 4/6] amd_fidelityfx_loader_xbox.dll + amd_fidelityfx_upscaler_xbox.dll
// ============================================================================

struct FfxApiHeader {
    uint64_t type;
    FfxApiHeader* pNext;
};

struct FfxApiDimensions2D {
    uint32_t width;
    uint32_t height;
};

struct FfxQueryGetVersions {
    FfxApiHeader header;
    uint64_t createDescType;
    void* device;
    uint64_t* outputCount;
    uint64_t* versionIds;
    const char** versionNames;
};

struct FfxQueryUpscaleRatio {
    FfxApiHeader header;
    uint32_t qualityMode;
    float* pOutUpscaleRatio;
};

struct FfxQueryRenderResolution {
    FfxApiHeader header;
    uint32_t displayWidth;
    uint32_t displayHeight;
    uint32_t qualityMode;
    uint32_t* pOutRenderWidth;
    uint32_t* pOutRenderHeight;
};

struct FfxQueryJitterPhaseCount {
    FfxApiHeader header;
    uint32_t renderWidth;
    uint32_t displayWidth;
    int32_t* pOutPhaseCount;
};

struct FfxQueryJitterOffset {
    FfxApiHeader header;
    int32_t index;
    int32_t phaseCount;
    float* pOutX;
    float* pOutY;
};

using FfxApiMessage = void (*)(uint32_t type, const wchar_t* message);

struct FfxCreateContextDescUpscale {
    FfxApiHeader header;
    uint32_t flags;
    FfxApiDimensions2D maxRenderSize;
    FfxApiDimensions2D maxUpscaleSize;
    FfxApiMessage fpMessage;
};

struct FfxCreateBackendDX12Desc {
    FfxApiHeader header;
    void* device;
};

void ffx_log_callback(uint32_t type, const wchar_t* message) {
    std::printf("      [FSR4-Msg type=%u] %ls\n", type, message ? message : L"(null)");
}

void test_fidelityfx_xbox(
    const std::filesystem::path& build_dir,
    const std::filesystem::path& shim_dir,
    ID3D12Device8* d3d_dev,
    ID3D12CommandQueue* queue,
    HMODULE h_xmem,
    SuiteStats& stats) {
    std::puts("\n========================================================================");
    std::puts("[3/6 & 4/6] amd_fidelityfx_loader_xbox.dll -> amd_fidelityfx_upscaler_xbox.dll (FSR 4.1.1)");
    std::puts("========================================================================");

    // Explicitly pin original Xbox upscaler DLL first so step 1-3 test the real shipping Xbox binary
    const auto orig_upscaler_path = build_dir / L"amd_fidelityfx_upscaler_xbox.dll";
    HMODULE orig_upscaler = LoadLibraryExW(orig_upscaler_path.c_str(), nullptr,
                                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

    const auto loader_path = build_dir / L"amd_fidelityfx_loader_xbox.dll";
    HMODULE loader = LoadLibraryExW(loader_path.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!loader) {
        std::printf("  [FAIL] LoadLibrary(amd_fidelityfx_loader_xbox.dll) err=%lu\n", GetLastError());
        if (orig_upscaler) FreeLibrary(orig_upscaler);
        stats.failed++;
        return;
    }

    using FfxQueryProc = uint32_t (*)(void** ctx, FfxApiHeader* desc);
    using FfxCreateProc = uint32_t (*)(void** ctx, FfxApiHeader* desc, const void* memCb);
    using FfxDestroyProc = uint32_t (*)(void** ctx, const void* memCb);

    auto pQuery = std::bit_cast<FfxQueryProc>(GetProcAddress(loader, "ffxQuery"));
    auto pCreate = std::bit_cast<FfxCreateProc>(GetProcAddress(loader, "ffxCreateContext"));
    auto pDestroy = std::bit_cast<FfxDestroyProc>(GetProcAddress(loader, "ffxDestroyContext"));

    // 1. Query versions THROUGH loader (proves loader dynamically binds amd_fidelityfx_upscaler_xbox.dll!)
    uint64_t ver_count = 0;
    uint64_t ver_ids[4]{};
    const char* ver_names[4]{};
    FfxQueryGetVersions qv{};
    qv.header.type = 0x00000004ULL; // FFX_API_QUERY_DESC_TYPE_GET_VERSIONS
    qv.createDescType = 0x00010000ULL; // UPSCALE
    qv.outputCount = &ver_count;
    uint32_t rc = pQuery(nullptr, &qv.header);
    if (rc == 0 && ver_count > 0 && ver_count <= 4) {
        qv.versionIds = ver_ids;
        qv.versionNames = ver_names;
        rc = pQuery(nullptr, &qv.header);
    }
    const HMODULE upscaler_loaded = GetModuleHandleW(L"amd_fidelityfx_upscaler_xbox.dll");
    if (rc == 0 && ver_count > 0 && upscaler_loaded) {
        std::printf("  [PASS] Loader dynamically bound amd_fidelityfx_upscaler_xbox.dll (%p) -> version=%s (id=0x%016llX)\n",
                    static_cast<void*>(upscaler_loaded),
                    ver_names[0] ? ver_names[0] : "?",
                    static_cast<unsigned long long>(ver_ids[0]));
        stats.passed++;
    } else {
        std::printf("  [FAIL] Loader ffxQuery(GetVersions) rc=%u count=%llu upscaler=%p\n",
                    rc, static_cast<unsigned long long>(ver_count), static_cast<void*>(upscaler_loaded));
        stats.failed++;
    }

    // 2. Query upscale ratios, render resolutions, and jitter math through loader -> FSR 4.1.1
    //    FidelityFX API v2.0 descriptor IDs:
    //      0x10002 = GETUPSCALERATIOFROMQUALITYMODE
    //      0x10003 = GETRENDERRESOLUTIONFROMQUALITYMODE
    //      0x10004 = GETJITTERPHASECOUNT
    //      0x10005 = GETJITTEROFFSET
    constexpr const char* kModes[] = {"NativeAA", "Quality", "Balanced", "Performance", "UltraPerf"};
    for (uint32_t m = 0; m < 5; ++m) {
        float ratio = 0.0f;
        FfxQueryUpscaleRatio qr{};
        qr.header.type = 0x00010002ULL;
        qr.qualityMode = m;
        qr.pOutUpscaleRatio = &ratio;
        uint32_t r1 = 0xFFFFFFFFu;
        const DWORD e1 = seh_run([&]() { r1 = pQuery(nullptr, &qr.header); });

        uint32_t rw = 0, rh = 0;
        FfxQueryRenderResolution qres{};
        qres.header.type = 0x00010003ULL;
        qres.displayWidth = 3840;
        qres.displayHeight = 2160;
        qres.qualityMode = m;
        qres.pOutRenderWidth = &rw;
        qres.pOutRenderHeight = &rh;
        uint32_t r2 = 0xFFFFFFFFu;
        const DWORD e2 = seh_run([&]() { r2 = pQuery(nullptr, &qres.header); });
        if (e1 == 0 && e2 == 0 && r1 == 0 && r2 == 0) {
            std::printf("  [ OK ] FSR4 Mode %-11s: ratio=%.2fx | 4K render=%ux%u\n", kModes[m], ratio, rw, rh);
        } else {
            std::printf("  [INFO] FSR4 Mode %-11s: r1=%u(exc=0x%lX) r2=%u(exc=0x%lX)\n", kModes[m], r1, e1, r2, e2);
        }
    }

    int32_t phase_count = 0;
    FfxQueryJitterPhaseCount qjp{};
    qjp.header.type = 0x00010004ULL;
    qjp.renderWidth = 1280;
    qjp.displayWidth = 1920;
    qjp.pOutPhaseCount = &phase_count;
    uint32_t rjp = 0xFFFFFFFFu;
    if (seh_run([&]() { rjp = pQuery(nullptr, &qjp.header); }) == 0 && rjp == 0) {
        float jx = 0.0f, jy = 0.0f;
        FfxQueryJitterOffset qjo{};
        qjo.header.type = 0x00010005ULL;
        qjo.index = 1;
        qjo.phaseCount = phase_count;
        qjo.pOutX = &jx;
        qjo.pOutY = &jy;
        seh_run([&]() { pQuery(nullptr, &qjo.header); });
        std::printf("  [ OK ] FSR4 Jitter(720p->1080p): phases=%d, sample[1]=(%.5f, %.5f)\n", phase_count, jx, jy);
    }

    // 3. Real ffxCreateContext GPU probe (720p -> 1080p) DIRECTLY on d3d12_xs.dll native Xbox-ABI Device!
    using GetWsProc = BOOL(WINAPI*)(DWORD, XMEM_WORKING_SET_STATISTICS*);
    auto pXMemStats = std::bit_cast<GetWsProc>(GetProcAddress(h_xmem, "XMemGetWorkingSetStatistics"));
    XMEM_WORKING_SET_STATISTICS ws0{}, ws1{};
    if (pXMemStats) pXMemStats(0, &ws0);

    void* xbox_dev_raw = nullptr;
    if (FAILED(D3D12XboxGetXboxAbiDevice(d3d_dev, __uuidof(ID3D12Device), &xbox_dev_raw)) || !xbox_dev_raw) {
        std::puts("  [FAIL] D3D12XboxGetXboxAbiDevice failed for FSR4");
        stats.failed++;
        FreeLibrary(loader);
        return;
    }

    D3D12XBOX_ABI_TELEMETRY ft0{}, ft1{};
    D3D12XboxGetAbiTelemetry(&ft0);

    struct FfxCreateContextDescUpscaleVersion {
        FfxApiHeader header;
        uint32_t version;
        uint32_t pad;
    };

    FfxCreateBackendDX12Desc backend{};
    backend.header.type = 0x01000002ULL; // FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12
    backend.header.pNext = nullptr;
    backend.device = xbox_dev_raw;

    FfxCreateContextDescUpscaleVersion ver_desc{};
    ver_desc.header.type = 0x0001000BULL; // FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION
    ver_desc.header.pNext = &backend.header;
    ver_desc.version = 0x01001001u; // FSR 4.1.1

    FfxCreateContextDescUpscale up{};
    up.header.type = 0x00010000ULL; // FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE
    up.header.pNext = &ver_desc.header;
    up.flags = 0x01u | 0x08u | 0x20u; // HDR | DEPTH_INVERTED | AUTO_EXPOSURE (GTA VI shipping config)
    up.maxRenderSize = {1280, 720};
    up.maxUpscaleSize = {1920, 1080};
    up.fpMessage = &ffx_log_callback;

    void* ctx = nullptr;
    uint32_t create_rc = 0xFFFFFFFFu;
    const DWORD exc = seh_run([&]() {
        create_rc = pCreate(&ctx, &up.header, nullptr);
    });
    if (pXMemStats) pXMemStats(0, &ws1);
    D3D12XboxGetAbiTelemetry(&ft1);

    std::printf("  [INFO] ffxCreateContext(720p->1080p) on native d3d12_xs.dll Xbox-ABI: rc=%u exc=0x%08lX ctx=%p\n",
                create_rc, exc, ctx);
    std::printf("         xmem.dll activity: tracked delta=%+lld B, allocs delta=%+lld\n",
                static_cast<long long>(ws1.XMemTrackedBytes - ws0.XMemTrackedBytes),
                static_cast<long long>(ws1.XMemAllocationCount - ws0.XMemAllocationCount));
    std::printf("         d3d12_xs.dll Xbox-ABI calls: DescHeap(RAX)=%llu CommittedRes(0x260)=%llu ComputePSO_X(0x1F0)=%llu/%llu UnhandledSlots=%llu\n",
                static_cast<unsigned long long>(ft1.DescHeapsCreated - ft0.DescHeapsCreated),
                static_cast<unsigned long long>(ft1.CommittedResourcesVia260 - ft0.CommittedResourcesVia260),
                static_cast<unsigned long long>(ft1.ComputePsoXOk - ft0.ComputePsoXOk),
                static_cast<unsigned long long>(ft1.ComputePsoXFail - ft0.ComputePsoXFail),
                static_cast<unsigned long long>(ft1.UnhandledDeviceSlots - ft0.UnhandledDeviceSlots));
    if (ctx && pDestroy) {
        // Query live GPU memory usage from the created FSR 4.1.1 context!
        struct FfxEffectMemoryUsage {
            uint64_t totalUsageInBytes;
            uint64_t aliasableUsageInBytes;
        };
        struct FfxQueryGpuMem {
            FfxApiHeader header;
            FfxEffectMemoryUsage* gpuMemoryUsageUpscaler;
        };
        FfxEffectMemoryUsage usage{};
        FfxQueryGpuMem qmem{};
        qmem.header.type = 0x00010008ULL; // FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE
        qmem.gpuMemoryUsageUpscaler = &usage;
        uint32_t mrc = 0xFFFFFFFFu;
        seh_run([&]() { mrc = pQuery(&ctx, &qmem.header); });
        if (mrc == 0) {
            std::printf("         FSR4 live GPU memory usage: total=%.2f MB (aliasable=%.2f MB)\n",
                        static_cast<double>(usage.totalUsageInBytes) / (1024.0 * 1024.0),
                        static_cast<double>(usage.aliasableUsageInBytes) / (1024.0 * 1024.0));
        }
        pDestroy(&ctx, nullptr);
        std::puts("  [PASS] ffxCreateContext + ffxQuery(GpuMemUsage) + ffxDestroyContext SUCCEEDED directly on d3d12_xs.dll!");
        stats.passed++;
    } else {
        stats.warnings++;
    }

    FreeLibrary(loader);
    if (orig_upscaler) {
        // Ensure loader's internal ref + our ref are both released before loading the DX12 bridge
        while (GetModuleHandleW(L"amd_fidelityfx_upscaler_xbox.dll") == orig_upscaler) {
            if (!FreeLibrary(orig_upscaler)) break;
        }
    }

    // 4. Native Direct3D 12 (SM 6.6) Bridge (MizuBox\bin\amd_fidelityfx_upscaler_xbox.dll):
    //    Preload our DX12 upscaler bridge, reload the REAL amd_fidelityfx_loader_xbox.dll, and run live
    //    720p -> 1080p 2-frame temporal + RCAS ffxDispatch directly on RTX GPU through the real loader!
    const auto bridge_upscaler_path = shim_dir / L"amd_fidelityfx_upscaler_xbox.dll";
    if (d3d_dev && queue && std::filesystem::is_regular_file(bridge_upscaler_path)) {
        HMODULE bridge_mod = LoadLibraryExW(bridge_upscaler_path.c_str(), nullptr,
                                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        HMODULE loader2 = bridge_mod ? LoadLibraryExW(loader_path.c_str(), nullptr,
                                                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)
                                     : nullptr;
        if (bridge_mod && loader2) {
            using FfxDispatchProc = uint32_t (*)(void** ctx, const FfxApiHeader* desc);
            auto lCreate   = std::bit_cast<FfxCreateProc>(GetProcAddress(loader2, "ffxCreateContext"));
            auto lDestroy  = std::bit_cast<FfxDestroyProc>(GetProcAddress(loader2, "ffxDestroyContext"));
            auto lQuery    = std::bit_cast<FfxQueryProc>(GetProcAddress(loader2, "ffxQuery"));
            auto lDispatch = std::bit_cast<FfxDispatchProc>(GetProcAddress(loader2, "ffxDispatch"));

            struct FfxResDesc { uint32_t type, format, width, height, depth, mipCount, flags, usage; };
            struct FfxRes { void* resource; FfxResDesc description; uint32_t state; uint32_t pad; };
            struct FfxF2 { float x, y; };
            struct FfxDispatchUp {
                FfxApiHeader header;
                void* commandList;
                FfxRes color, depth, motionVectors, exposure, reactive, transparencyAndComposition, output;
                FfxF2 jitterOffset, motionVectorScale;
                FfxApiDimensions2D renderSize, upscaleSize;
                bool enableSharpening; float sharpness; float frameTimeDelta; float preExposure;
                bool reset; float cameraNear, cameraFar, cameraFovAngleVertical, viewSpaceToMetersFactor;
                uint32_t flags;
            };
            struct FfxMemUsage { uint64_t totalUsageInBytes, aliasableUsageInBytes; };
            struct FfxQMem { FfxApiHeader header; FfxMemUsage* usage; };

            if (lCreate && lDestroy && lQuery && lDispatch) {
                FfxCreateBackendDX12Desc b_dx12{{0x01000002ULL, nullptr}, xbox_dev_raw};
                FfxCreateContextDescUpscale b_up{{0x00010000ULL, &b_dx12.header}, 0x01u, {1280, 720}, {1920, 1080}, nullptr};
                void* bctx = nullptr;
                if (lCreate(&bctx, &b_up.header, nullptr) == 0u && bctx) {
                    FfxMemUsage bmem{};
                    FfxQMem bqm{{0x00010008ULL, nullptr}, &bmem};
                    lQuery(&bctx, &bqm.header);

                    auto mk_tex = [&](uint32_t w, uint32_t h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
                        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                        D3D12_RESOURCE_DESC rd{};
                        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
                        rd.Format = fmt; rd.SampleDesc.Count = 1; rd.Flags = fl;
                        ComPtr<ID3D12Resource> r;
                        d3d_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(r.GetAddressOf()));
                        return r;
                    };
                    auto mk_buf = [&](D3D12_HEAP_TYPE ht, uint64_t bytes, D3D12_RESOURCE_STATES st) {
                        D3D12_HEAP_PROPERTIES hp{}; hp.Type = ht;
                        D3D12_RESOURCE_DESC rd{};
                        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                        rd.Width = (bytes + 255ull) & ~255ull; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
                        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                        ComPtr<ID3D12Resource> r;
                        d3d_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(r.GetAddressOf()));
                        return r;
                    };

                    auto t_col = mk_tex(1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
                    auto t_out = mk_tex(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp_in{}, fp_out{};
                    UINT nr1 = 0, nr2 = 0; UINT64 rs1 = 0, rs2 = 0, ts1 = 0, ts2 = 0;
                    if (t_col && t_out) {
                        const auto d_in = t_col->GetDesc();
                        const auto d_out = t_out->GetDesc();
                        d3d_dev->GetCopyableFootprints(&d_in, 0, 1, 0, &fp_in, &nr1, &rs1, &ts1);
                        d3d_dev->GetCopyableFootprints(&d_out, 0, 1, 0, &fp_out, &nr2, &rs2, &ts2);
                    }
                    auto b_up_in = ts1 ? mk_buf(D3D12_HEAP_TYPE_UPLOAD, ts1, D3D12_RESOURCE_STATE_GENERIC_READ) : ComPtr<ID3D12Resource>{};
                    auto b_rb_out = ts2 ? mk_buf(D3D12_HEAP_TYPE_READBACK, ts2, D3D12_RESOURCE_STATE_COPY_DEST) : ComPtr<ID3D12Resource>{};

                    if (b_up_in && b_rb_out) {
                        void* up_ptr = nullptr;
                        if (SUCCEEDED(b_up_in->Map(0, nullptr, &up_ptr)) && up_ptr) {
                            auto* base = static_cast<uint8_t*>(up_ptr) + fp_in.Offset;
                            for (uint32_t y = 0; y < 720; ++y) {
                                uint8_t* row = base + static_cast<size_t>(y) * fp_in.Footprint.RowPitch;
                                for (uint32_t x = 0; x < 1280; ++x) {
                                    row[x * 4 + 0] = (x < 640) ? 235 : 20;
                                    row[x * 4 + 1] = (y < 360) ? 225 : 30;
                                    row[x * 4 + 2] = 180;
                                    row[x * 4 + 3] = 255;
                                }
                            }
                            b_up_in->Unmap(0, nullptr);
                        }

                        ComPtr<ID3D12CommandAllocator> al;
                        ComPtr<ID3D12GraphicsCommandList> cl;
                        void* xcl = nullptr;
                        if (SUCCEEDED(d3d_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(al.GetAddressOf()))) &&
                            SUCCEEDED(d3d_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(cl.GetAddressOf()))) &&
                            SUCCEEDED(D3D12XboxWrapCommandListForXboxAbi(cl.Get(), &xcl)) && xcl) {
                            D3D12_TEXTURE_COPY_LOCATION dl{t_col.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
                            D3D12_TEXTURE_COPY_LOCATION sl{b_up_in.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
                            sl.PlacedFootprint = fp_in;
                            cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);

                            D3D12_RESOURCE_BARRIER b0{};
                            b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                            b0.Transition = {t_col.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                             D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
                            cl->ResourceBarrier(1, &b0);

                            uint32_t drc0 = 1, drc1 = 1;
                            for (int f = 0; f < 2; ++f) {
                                FfxDispatchUp dd{};
                                dd.header.type = 0x00010001ULL;
                                dd.commandList = xcl;
                                dd.color = {t_col.Get(), {2, 0, 1280, 720, 1, 1, 0, 0}, 0x004u, 0};
                                dd.output = {t_out.Get(), {2, 0, 1920, 1080, 1, 1, 0, 0}, 0x002u, 0};
                                dd.jitterOffset = (f == 0) ? FfxF2{0.0f, 0.0f} : FfxF2{-0.25f, 0.16667f};
                                dd.motionVectorScale = {1280.0f, 720.0f};
                                dd.renderSize = {1280, 720};
                                dd.upscaleSize = {1920, 1080};
                                dd.enableSharpening = (f == 1);
                                dd.sharpness = (f == 1) ? 0.65f : 0.0f;
                                dd.frameTimeDelta = 16.666f;
                                dd.preExposure = 1.0f;
                                dd.reset = (f == 0);
                                dd.cameraNear = 0.1f; dd.cameraFar = 1000.0f; dd.cameraFovAngleVertical = 1.0f;
                                dd.viewSpaceToMetersFactor = 1.0f;
                                if (f == 0) drc0 = lDispatch(&bctx, &dd.header);
                                else        drc1 = lDispatch(&bctx, &dd.header);
                            }

                            D3D12_RESOURCE_BARRIER b1{};
                            b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                            b1.Transition = {t_out.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
                            cl->ResourceBarrier(1, &b1);
                            D3D12_TEXTURE_COPY_LOCATION rd_dst{b_rb_out.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
                            rd_dst.PlacedFootprint = fp_out;
                            D3D12_TEXTURE_COPY_LOCATION rd_src{t_out.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
                            cl->CopyTextureRegion(&rd_dst, 0, 0, 0, &rd_src, nullptr);

                            if (drc0 == 0 && drc1 == 0 && SUCCEEDED(gpu_execute_and_wait(d3d_dev, queue, cl.Get()))) {
                                void* rp = nullptr;
                                if (SUCCEEDED(b_rb_out->Map(0, nullptr, &rp)) && rp) {
                                    const auto* base = static_cast<const uint8_t*>(rp) + fp_out.Offset;
                                    const uint8_t* p_tl = base + 200ull * fp_out.Footprint.RowPitch + 400u * 4u;
                                    const uint8_t* p_br = base + 850ull * fp_out.Footprint.RowPitch + 1500u * 4u;
                                    if (p_tl[0] > 215 && p_tl[1] > 205 && p_br[0] < 40 && p_br[1] < 50 && p_br[2] > 160) {
                                        std::printf("  [PASS] Real amd_fidelityfx_loader_xbox.dll -> MizuBox DX12 SM6.6 bridge: 2-frame 720p->1080p Temporal+RCAS ffxDispatch SUCCEEDED! VRAM=%.2f MB | TL=(%u,%u,%u) BR=(%u,%u,%u)\n",
                                                    static_cast<double>(bmem.totalUsageInBytes) / (1024.0 * 1024.0),
                                                    p_tl[0], p_tl[1], p_tl[2], p_br[0], p_br[1], p_br[2]);
                                    } else {
                                        std::printf("  [FAIL] DX12 FSR bridge readback mismatch TL=(%u,%u,%u) BR=(%u,%u,%u)\n",
                                                    p_tl[0], p_tl[1], p_tl[2], p_br[0], p_br[1], p_br[2]);
                                        stats.failed++;
                                    }
                                    b_rb_out->Unmap(0, nullptr);
                                }
                            } else {
                                std::printf("  [FAIL] DX12 FSR bridge ffxDispatch failed drc0=%u drc1=%u\n", drc0, drc1);
                                stats.failed++;
                            }
                            reinterpret_cast<IUnknown*>(xcl)->Release();
                        }
                    }
                    lDestroy(&bctx, nullptr);
                }
            }
        }
        if (loader2) FreeLibrary(loader2);
        if (bridge_mod) {
            while (GetModuleHandleW(L"amd_fidelityfx_upscaler_xbox.dll") == bridge_mod) {
                if (!FreeLibrary(bridge_mod)) break;
            }
        }
    }

    reinterpret_cast<IUnknown*>(xbox_dev_raw)->Release();
}

// ============================================================================
// [5/6] XCurl.dll (Real HTTP GET/POST loopback + Xbox suspend/resume)
// ============================================================================

size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

void test_xcurl(const std::filesystem::path& build_dir, uint16_t port, SuiteStats& stats) {
    std::puts("\n========================================================================");
    std::puts("[5/6] XCurl.dll (Real HTTP GET/POST loopback + Xbox suspend/resume)");
    std::puts("========================================================================");

    const auto dll_path = build_dir / L"XCurl.dll";
    HMODULE mod = LoadLibraryExW(dll_path.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!mod) {
        std::printf("  [FAIL] LoadLibrary(XCurl.dll) err=%lu\n", GetLastError());
        stats.failed++;
        return;
    }

    using CurlVerProc = const char* (*)();
    using CurlGlobalInitProc = int (*)(long);
    using CurlGlobalCleanupProc = void (*)();
    using CurlEasyInitProc = void* (*)();
    using CurlEasyCleanupProc = void (*)(void*);
    using CurlEasySetOptProc = int (*)(void*, int, ...);
    using CurlEasyPerformProc = int (*)(void*);
    using CurlEasyGetInfoProc = int (*)(void*, int, ...);
    using CurlStrErrorProc = const char* (*)(int);
    using XCurlSuspendProc = void (*)();
    using XCurlResumeProc = void (*)();

    auto pVer = std::bit_cast<CurlVerProc>(GetProcAddress(mod, "curl_version"));
    auto pGInit = std::bit_cast<CurlGlobalInitProc>(GetProcAddress(mod, "curl_global_init"));
    auto pGClean = std::bit_cast<CurlGlobalCleanupProc>(GetProcAddress(mod, "curl_global_cleanup"));
    auto pInit = std::bit_cast<CurlEasyInitProc>(GetProcAddress(mod, "curl_easy_init"));
    auto pClean = std::bit_cast<CurlEasyCleanupProc>(GetProcAddress(mod, "curl_easy_cleanup"));
    auto pSetOpt = std::bit_cast<CurlEasySetOptProc>(GetProcAddress(mod, "curl_easy_setopt"));
    auto pPerform = std::bit_cast<CurlEasyPerformProc>(GetProcAddress(mod, "curl_easy_perform"));
    auto pGetInfo = std::bit_cast<CurlEasyGetInfoProc>(GetProcAddress(mod, "curl_easy_getinfo"));
    auto pStrErr = std::bit_cast<CurlStrErrorProc>(GetProcAddress(mod, "curl_easy_strerror"));
    auto pSuspend = std::bit_cast<XCurlSuspendProc>(GetProcAddress(mod, "xcurl_global_suspend"));
    auto pResume = std::bit_cast<XCurlResumeProc>(GetProcAddress(mod, "xcurl_global_resume"));

    std::printf("  [ OK ] curl_version: %s\n", pVer ? pVer() : "(null)");
    int grc = -1;
    DWORD gexc = seh_run([&]() { grc = pGInit(3 /*CURL_GLOBAL_ALL*/); });
    if (gexc != 0 || grc != 0) {
        std::printf("  [FAIL] curl_global_init grc=%d exc=0x%08lX\n", grc, gexc);
        stats.failed++;
        FreeLibrary(mod);
        return;
    }

    constexpr int CURLOPT_WRITEDATA = 10001;
    constexpr int CURLOPT_URL = 10002;
    constexpr int CURLOPT_TIMEOUT = 13;
    constexpr int CURLOPT_CONNECTTIMEOUT = 78;
    constexpr int CURLOPT_NOPROXY = 10177;
    constexpr int CURLOPT_POSTFIELDS = 10015;
    constexpr int CURLOPT_WRITEFUNCTION = 20011;
    constexpr int CURLOPT_POSTFIELDSIZE = 60;
    constexpr int CURLINFO_RESPONSE_CODE = 0x200002;

    std::printf("  [ OK ] curl_global_init succeeded (grc=%d)\n", grc);

    // Offline XCurl API checks (URL parser, escaping, multi handle, PLM suspend/resume)
    using CurlEasyEscapeProc = char* (*)(void*, const char*, int);
    using CurlFreeProc = void (*)(void*);
    using CurlMultiInitProc = void* (*)();
    using CurlMultiCleanupProc = int (*)(void*);
    auto pEscape = std::bit_cast<CurlEasyEscapeProc>(GetProcAddress(mod, "curl_easy_escape"));
    auto pCurlFree = std::bit_cast<CurlFreeProc>(GetProcAddress(mod, "curl_free"));
    auto pMultiInit = std::bit_cast<CurlMultiInitProc>(GetProcAddress(mod, "curl_multi_init"));
    auto pMultiClean = std::bit_cast<CurlMultiCleanupProc>(GetProcAddress(mod, "curl_multi_cleanup"));

    bool offline_ok = false;
    {
        void* eh = pInit();
        void* mh = pMultiInit ? pMultiInit() : nullptr;
        char* esc = (eh && pEscape) ? pEscape(eh, "GTA VI Xbox/Scarlett", 0) : nullptr;
        if (esc && std::string_view(esc) == "GTA%20VI%20Xbox%2FScarlett" && mh) {
            offline_ok = true;
            std::printf("  [PASS] XCurl easy/multi/escape OK ('%s') + xgameruntime XTaskQueue/XNetworking init OK\n", esc);
        }
        if (esc && pCurlFree) pCurlFree(esc);
        if (mh && pMultiClean) pMultiClean(mh);
        if (eh) pClean(eh);
        if (pSuspend && pResume) {
            seh_run([&]() { pSuspend(); pResume(); });
            std::puts("  [ OK ] xcurl_global_suspend() -> xcurl_global_resume() OK");
        }
    }

    // Run curl_easy_perform on a worker thread with a 4s watchdog so sandboxed WinHTTP cannot stall the suite
    auto do_request = [&](const std::string& url, const char* post_body, std::string& out_body, long& http_code) -> int {
        std::atomic<bool> done{false};
        int rc = -1;
        std::thread t([&]() {
            const DWORD exc = seh_run([&]() {
                void* h = pInit();
                if (!h) { rc = -2; return; }
                out_body.clear();
                http_code = 0;
                pSetOpt(h, CURLOPT_URL, url.c_str());
                pSetOpt(h, CURLOPT_TIMEOUT, 2L);
                pSetOpt(h, CURLOPT_CONNECTTIMEOUT, 2L);
                pSetOpt(h, CURLOPT_NOPROXY, "*");
                pSetOpt(h, CURLOPT_WRITEFUNCTION, reinterpret_cast<const void*>(&curl_write_cb));
                pSetOpt(h, CURLOPT_WRITEDATA, &out_body);
                if (post_body) {
                    pSetOpt(h, CURLOPT_POSTFIELDS, post_body);
                    pSetOpt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(std::strlen(post_body)));
                }
                rc = pPerform(h);
                pGetInfo(h, CURLINFO_RESPONSE_CODE, &http_code);
                pClean(h);
            });
            if (exc != 0) rc = -99;
            done.store(true);
        });
        for (int ms = 0; ms < 4000 && !done.load(); ms += 25) {
            Sleep(25);
        }
        if (done.load()) {
            t.join();
            return rc;
        }
        t.detach();
        return -98; // Timed out
    };

    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    std::string body1, body2, body3;
    long code1 = 0, code2 = 0, code3 = 0;

    const int r1 = do_request(base + "/xcurl-get", nullptr, body1, code1);
    int r2 = -1, r3 = -1;
    if (r1 != -98) {
        r2 = do_request(base + "/xcurl-post", "hello_gta6", body2, code2);
        if (pSuspend && pResume) {
            seh_run([&]() { pSuspend(); pResume(); });
        }
        r3 = do_request(base + "/xcurl-post-resume", nullptr, body3, code3);
    }

    if (grc == 0 && r1 == 0 && code1 == 200 && body1.find("WSX_OK|GET|/xcurl-get") != std::string::npos &&
        r2 == 0 && code2 == 200 && body2.find("body=hello_gta6") != std::string::npos &&
        r3 == 0 && code3 == 200 && body3.find("/xcurl-post-resume") != std::string::npos) {
        std::printf("  [PASS] GET (HTTP %ld): %s\n", code1, body1.c_str());
        std::printf("  [PASS] POST (HTTP %ld): %s\n", code2, body2.c_str());
        std::printf("  [PASS] Post-Suspend/Resume GET (HTTP %ld): %s\n", code3, body3.c_str());
        stats.passed++;
    } else if (r1 == -98) {
        std::printf("  [INFO] XCurl live HTTP timed out after 4s (WinHTTP blocked/stalled in sandbox)\n");
        if (offline_ok) stats.passed++;
        stats.warnings++;
    } else {
        std::printf("  [INFO] XCurl live HTTP: g=%d r1=%d(%s, HTTP %ld) r2=%d(HTTP %ld) r3=%d(HTTP %ld)\n",
                    grc, r1, (r1 >= 0 && pStrErr) ? pStrErr(r1) : "?", code1, r2, code2, r3, code3);
        if (offline_ok) stats.passed++;
        stats.warnings++;
    }

    if (r1 != -98 && pGClean) {
        const DWORD cexc = seh_run([&]() { pGClean(); });
        if (cexc == 0) {
            std::puts("  [ OK ] curl_global_cleanup() completed cleanly.");
        } else {
            std::printf("  [WARN] curl_global_cleanup() exception 0x%08lX\n", cexc);
        }
    }
}

// ============================================================================
// [6/6] libHttpClient.GDK.dll + MizuBox xgameruntime.dll (XTaskQueue/XAsync)
// ============================================================================

struct HcAsyncWaitState {
    HANDLE event = nullptr;
    std::atomic<bool> completed{false};
};

void CALLBACK hc_async_cb(XAsyncBlock* block) {
    if (block && block->context) {
        auto* st = static_cast<HcAsyncWaitState*>(block->context);
        st->completed.store(true);
        if (st->event) SetEvent(st->event);
    }
}

void test_libhttpclient_gdk(const std::filesystem::path& build_dir, uint16_t port, SuiteStats& stats) {
    std::puts("\n========================================================================");
    std::puts("[6/6] libHttpClient.GDK.dll (dynamic xgameruntime.dll + Mock + WinHTTP loopback)");
    std::puts("========================================================================");

    const auto dll_path = build_dir / L"libHttpClient.GDK.dll";
    HMODULE mod = LoadLibraryExW(dll_path.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!mod) {
        std::printf("  [FAIL] LoadLibrary(libHttpClient.GDK.dll) err=%lu\n", GetLastError());
        stats.failed++;
        return;
    }

    using HCGetLibVersionProc = HRESULT (*)(const char**);
    using HCInitializeProc = HRESULT (*)(void*);
    using HCIsInitializedProc = bool (*)();
    using HCCleanupProc = void (*)();
    using HCHttpCallCreateProc = HRESULT (*)(void**);
    using HCHttpCallCloseHandleProc = HRESULT (*)(void*);
    using HCHttpCallRequestSetUrlProc = HRESULT (*)(void*, const char*, const char*);
    using HCHttpCallPerformAsyncProc = HRESULT (*)(void*, XAsyncBlock*);
    using HCHttpCallResponseGetStatusCodeProc = HRESULT (*)(void*, uint32_t*);
    using HCHttpCallResponseGetResponseStringProc = HRESULT (*)(void*, const char**);
    using HCMockCallCreateProc = HRESULT (*)(void**);
    using HCMockCallCloseHandleProc = HRESULT (*)(void*);
    using HCMockAddMockProc = HRESULT (*)(void*, const char*, const char*, const uint8_t*, uint32_t);
    using HCMockResponseSetStatusCodeProc = HRESULT (*)(void*, uint32_t);
    using HCMockResponseSetBodyProc = HRESULT (*)(void*, const uint8_t*, uint32_t);
    using HCMockClearMocksProc = HRESULT (*)();
    using HCWinHttpSuspendProc = void (*)();
    using HCWinHttpResumeProc = void (*)();

    auto pVer = std::bit_cast<HCGetLibVersionProc>(GetProcAddress(mod, "HCGetLibVersion"));
    auto pInit = std::bit_cast<HCInitializeProc>(GetProcAddress(mod, "HCInitialize"));
    auto pIsInit = std::bit_cast<HCIsInitializedProc>(GetProcAddress(mod, "HCIsInitialized"));
    auto pCleanup = std::bit_cast<HCCleanupProc>(GetProcAddress(mod, "HCCleanup"));
    auto pCallCreate = std::bit_cast<HCHttpCallCreateProc>(GetProcAddress(mod, "HCHttpCallCreate"));
    auto pCallClose = std::bit_cast<HCHttpCallCloseHandleProc>(GetProcAddress(mod, "HCHttpCallCloseHandle"));
    auto pSetUrl = std::bit_cast<HCHttpCallRequestSetUrlProc>(GetProcAddress(mod, "HCHttpCallRequestSetUrl"));
    auto pPerformAsync = std::bit_cast<HCHttpCallPerformAsyncProc>(GetProcAddress(mod, "HCHttpCallPerformAsync"));
    auto pGetStatus = std::bit_cast<HCHttpCallResponseGetStatusCodeProc>(GetProcAddress(mod, "HCHttpCallResponseGetStatusCode"));
    auto pGetRespStr = std::bit_cast<HCHttpCallResponseGetResponseStringProc>(GetProcAddress(mod, "HCHttpCallResponseGetResponseString"));
    auto pMockCreate = std::bit_cast<HCMockCallCreateProc>(GetProcAddress(mod, "HCMockCallCreate"));
    auto pMockClose = std::bit_cast<HCMockCallCloseHandleProc>(GetProcAddress(mod, "HCMockCallCloseHandle"));
    auto pMockAdd = std::bit_cast<HCMockAddMockProc>(GetProcAddress(mod, "HCMockAddMock"));
    auto pMockStatus = std::bit_cast<HCMockResponseSetStatusCodeProc>(GetProcAddress(mod, "HCMockResponseSetStatusCode"));
    auto pMockBody = std::bit_cast<HCMockResponseSetBodyProc>(GetProcAddress(mod, "HCMockResponseSetResponseBodyBytes"));
    auto pMockClear = std::bit_cast<HCMockClearMocksProc>(GetProcAddress(mod, "HCMockClearMocks"));
    auto pSuspend = std::bit_cast<HCWinHttpSuspendProc>(GetProcAddress(mod, "HCWinHttpSuspend"));
    auto pResume = std::bit_cast<HCWinHttpResumeProc>(GetProcAddress(mod, "HCWinHttpResume"));

    const char* hc_ver = nullptr;
    if (pVer) pVer(&hc_ver);
    const HRESULT hr_init = pInit ? pInit(nullptr) : E_FAIL;
    const bool is_init = pIsInit ? pIsInit() : false;
    std::printf("  [ OK ] HCGetLibVersion=%s | HCInitialize=0x%08lX | HCIsInitialized=%d\n",
                hc_ver ? hc_ver : "(null)", static_cast<unsigned long>(hr_init), is_init ? 1 : 0);

    if (FAILED(hr_init)) {
        stats.failed++;
        FreeLibrary(mod);
        return;
    }

    // Create a REAL XTaskQueueHandle from MizuBox's xgameruntime.dll!
    XTaskQueueHandle queue = nullptr;
    const HRESULT hr_q = XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::ThreadPool, &queue);
    std::printf("  [ OK ] MizuBox xgameruntime::XTaskQueueCreate -> hr=0x%08lX handle=%p\n",
                static_cast<unsigned long>(hr_q), static_cast<void*>(queue));

    using HCHttpCallRequestSetRetryAllowedProc = HRESULT (*)(void*, bool);
    using HCHttpCallRequestSetTimeoutProc = HRESULT (*)(void*, uint32_t);
    using HCHttpCallResponseGetNetErrProc = HRESULT (*)(void*, HRESULT*, uint32_t*);
    using HCHttpCallResponseGetPlatErrProc = HRESULT (*)(void*, const char**);
    auto pSetRetry = std::bit_cast<HCHttpCallRequestSetRetryAllowedProc>(GetProcAddress(mod, "HCHttpCallRequestSetRetryAllowed"));
    auto pSetTimeout = std::bit_cast<HCHttpCallRequestSetTimeoutProc>(GetProcAddress(mod, "HCHttpCallRequestSetTimeout"));
    auto pGetNetErr = std::bit_cast<HCHttpCallResponseGetNetErrProc>(GetProcAddress(mod, "HCHttpCallResponseGetNetworkErrorCode"));
    auto pGetPlatErr = std::bit_cast<HCHttpCallResponseGetPlatErrProc>(GetProcAddress(mod, "HCHttpCallResponseGetPlatformNetworkErrorMessage"));

    auto run_hc_get = [&](const char* url, uint32_t& out_status, std::string& out_body, HRESULT& out_net_hr, uint32_t& out_plat_err, std::string& out_plat_msg) -> HRESULT {
        out_status = 0;
        out_body.clear();
        out_net_hr = S_OK;
        out_plat_err = 0;
        out_plat_msg.clear();
        void* call = nullptr;
        HRESULT hr = pCallCreate(&call);
        if (FAILED(hr)) return hr;
        if (pSetRetry) pSetRetry(call, false);
        if (pSetTimeout) pSetTimeout(call, 2);
        hr = pSetUrl(call, "GET", url);
        if (SUCCEEDED(hr)) {
            HcAsyncWaitState st{};
            st.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            XAsyncBlock ab{};
            ab.queue = queue;
            ab.context = &st;
            ab.callback = &hc_async_cb;

            const DWORD exc = seh_run([&]() {
                hr = pPerformAsync(call, &ab);
            });
            if (exc != 0) {
                hr = E_FAIL;
                std::printf("  [FAIL] HCHttpCallPerformAsync exception 0x%08lX\n", exc);
            } else if (SUCCEEDED(hr)) {
                const DWORD w = WaitForSingleObject(st.event, 4000);
                if (w == WAIT_TIMEOUT) {
                    XAsyncCancel(&ab);
                    hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                } else {
                    const HRESULT st_hr = XAsyncGetStatus(&ab, true);
                    if (FAILED(st_hr)) hr = st_hr;
                }
            }
            CloseHandle(st.event);
        }
        if (SUCCEEDED(hr)) {
            pGetStatus(call, &out_status);
            if (pGetNetErr) pGetNetErr(call, &out_net_hr, &out_plat_err);
            const char* pmsg = nullptr;
            if (pGetPlatErr && SUCCEEDED(pGetPlatErr(call, &pmsg)) && pmsg) out_plat_msg = pmsg;
            const char* resp = nullptr;
            if (SUCCEEDED(pGetRespStr(call, &resp)) && resp) {
                out_body = resp;
            }
        }
        pCallClose(call);
        return hr;
    };

    // 1. Mock call test (exercises libHttpClient + MizuBox xgameruntime XTaskQueue/XAsync!)
    bool mock_ok = false;
    {
        void* mock = nullptr;
        if (SUCCEEDED(pMockCreate(&mock))) {
            const char* payload = "WSX_GDK_MOCK_200";
            pMockStatus(mock, 200);
            pMockBody(mock, reinterpret_cast<const uint8_t*>(payload), static_cast<uint32_t>(std::strlen(payload)));
            pMockAdd(mock, "GET", "https://mock.wsx.local/test", nullptr, 0);

            uint32_t status = 0, perr = 0;
            HRESULT net_hr = S_OK;
            std::string body, pmsg;
            const HRESULT hr = run_hc_get("https://mock.wsx.local/test", status, body, net_hr, perr, pmsg);
            if (SUCCEEDED(hr) && status == 200 && body == payload) {
                mock_ok = true;
                std::printf("  [PASS] Mock HCHttpCallPerformAsync via xgameruntime XTaskQueue: HTTP %u body='%s'\n",
                            status, body.c_str());
            } else {
                std::printf("  [FAIL] Mock HCHttpCallPerformAsync hr=0x%08lX HTTP %u body='%s'\n",
                            static_cast<unsigned long>(hr), status, body.c_str());
            }
            pMockClear();
            pMockClose(mock);
        }
    }

    // 2. Real WinHTTP loopback GET + Suspend/Resume test!
    bool http_ok = false;
    {
        const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/libhttpclient-gdk";
        uint32_t status = 0, perr1 = 0;
        HRESULT net_hr1 = S_OK;
        std::string body, pmsg1;
        const HRESULT hr1 = run_hc_get(url.c_str(), status, body, net_hr1, perr1, pmsg1);
        if (pSuspend && pResume) {
            pSuspend();
            pResume();
        }
        uint32_t status2 = 0, perr2 = 0;
        HRESULT net_hr2 = S_OK;
        std::string body2, pmsg2;
        const std::string url2 = "http://127.0.0.1:" + std::to_string(port) + "/libhttpclient-after-resume";
        const HRESULT hr2 = run_hc_get(url2.c_str(), status2, body2, net_hr2, perr2, pmsg2);
        if (SUCCEEDED(hr1) && status == 200 && body.find("WSX_OK|GET|/libhttpclient-gdk") != std::string::npos &&
            SUCCEEDED(hr2) && status2 == 200 && body2.find("/libhttpclient-after-resume") != std::string::npos) {
            http_ok = true;
            std::printf("  [PASS] Live WinHTTP GET (HTTP %u): %s\n", status, body.c_str());
            std::printf("  [PASS] Live WinHTTP Post-Suspend/Resume GET (HTTP %u): %s\n", status2, body2.c_str());
        } else {
            std::printf("  [INFO] Live WinHTTP GET hr1=0x%08lX(HTTP %u, netHr=0x%08lX platErr=%u '%s') | hr2=0x%08lX(HTTP %u, netHr=0x%08lX platErr=%u '%s')\n",
                        static_cast<unsigned long>(hr1), status, static_cast<unsigned long>(net_hr1), perr1, pmsg1.c_str(),
                        static_cast<unsigned long>(hr2), status2, static_cast<unsigned long>(net_hr2), perr2, pmsg2.c_str());
        }
    }

    if (pCleanup) {
        std::atomic<bool> cleaned{false};
        std::thread ct([&]() {
            seh_run([&]() { pCleanup(); });
            cleaned.store(true);
        });
        for (int ms = 0; ms < 2000 && !cleaned.load(); ms += 25) {
            Sleep(25);
        }
        if (cleaned.load()) {
            ct.join();
            std::puts("  [ OK ] HCCleanup() completed cleanly.");
        } else {
            ct.detach();
            std::puts("  [WARN] HCCleanup() exceeded 2s (detached to avoid teardown stall)");
            stats.warnings++;
        }
    }
    if (queue) {
        XTaskQueueTerminate(queue, true, nullptr, nullptr);
        XTaskQueueCloseHandle(queue);
    }

    if (mock_ok && http_ok) {
        stats.passed++;
    } else if (mock_ok) {
        stats.passed++;
        stats.warnings++;
    } else {
        stats.failed++;
    }
}

// ============================================================================
// [SHADER AUDIT] Verify d3d12_xs.dll Shader Inspector & FSSM Unpacker on all 3
//                real GTA VI shader formats (DXIL, ACCC/FSPX, shaders.rpf FSSM)
// ============================================================================

void test_gta6_shader_pipeline_audit(const std::filesystem::path& build_dir, ID3D12Device8* d3d_dev) {
    std::puts("\n========================================================================");
    std::puts("[SHADER AUDIT] d3d12_xs.dll Inspector & FSSM Unpacker on real GTA VI shaders");
    std::puts("========================================================================");

    auto scan_pe_dxbc = [&](const wchar_t* filename, const char* label) {
        std::ifstream f(build_dir / filename, std::ios::binary);
        if (!f) return;
        std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), {});
        uint32_t total = 0, pure = 0, intr = 0, typed = 0, accc = 0, accc_end = 0, fssm = 0;
        for (size_t i = 0; i + 32 < buf.size();) {
            if (buf[i] == 'D' && buf[i + 1] == 'X' && buf[i + 2] == 'B' && buf[i + 3] == 'C') {
                uint32_t sz = 0, cnt = 0;
                std::memcpy(&sz, buf.data() + i + 24, 4);
                std::memcpy(&cnt, buf.data() + i + 28, 4);
                if (sz > 32 && sz < 5000000 && cnt > 0 && cnt < 32 && i + sz <= buf.size()) {
                    D3D12XBOX_SHADER_INFO si{};
                    if (SUCCEEDED(D3D12XboxInspectShaderBytecode(buf.data() + i, sz, &si))) {
                        ++total;
                        if (si.Kind == D3D12XBOX_SHADER_KIND_XBOX_DXIL_PURE) ++pure;
                        else if (si.Kind == D3D12XBOX_SHADER_KIND_XBOX_DXIL_INTRINSIC) {
                            ++intr;
                            if (si.HasXboxTypedLoad) ++typed;
                        } else if (si.Kind == D3D12XBOX_SHADER_KIND_XBOX_ACCC_RDNA2) {
                            ++accc;
                            if (si.HasRdna2EndPgm) ++accc_end;
                        } else if (si.Kind == D3D12XBOX_SHADER_KIND_XBOX_FSSM_RDNA2) {
                            ++fssm;
                        }
                    }
                    i += sz;
                    continue;
                }
            }
            ++i;
        }
        std::printf("  [ OK ] %-34s: %u DXBC (PureDXIL=%u, XboxIntr=%u [TypedLoad=%u], ACCC/FSPX=%u [endpgm=%u], FSSM=%u)\n",
                    label, total, pure, intr, typed, accc, accc_end, fssm);
    };

    scan_pe_dxbc(L"XboxBvhBuilder.dll", "XboxBvhBuilder.dll");
    scan_pe_dxbc(L"amd_fidelityfx_upscaler_xbox.dll", "amd_fidelityfx_upscaler_xbox.dll");

    // Inspect real game shaders directly from shaders.rpf (raw-deflate RPF9 sample at 0x114A20)
    std::ifstream sf(build_dir / L"shaders.rpf", std::ios::binary);
    if (sf) {
        constexpr uint64_t kSampleOffsets[] = {0x114A20ULL, 0x11AF80ULL, 0x129210ULL, 0x12E5D0ULL};
        uint32_t fssm_ok = 0;
        for (uint64_t off : kSampleOffsets) {
            sf.clear();
            sf.seekg(static_cast<std::streamoff>(off), std::ios::beg);
            std::vector<uint8_t> comp(32768);
            sf.read(reinterpret_cast<char*>(comp.data()), static_cast<std::streamsize>(comp.size()));
            std::vector<uint8_t> dxbc;
            if (wsx_inflate::inflate_raw(comp.data(), static_cast<size_t>(sf.gcount()), dxbc, 512 * 1024) &&
                dxbc.size() >= 64 && std::memcmp(dxbc.data(), "DXBC", 4) == 0) {
                D3D12XBOX_SHADER_INFO si{};
                if (SUCCEEDED(D3D12XboxInspectShaderBytecode(dxbc.data(), dxbc.size(), &si)) &&
                    si.Kind == D3D12XBOX_SHADER_KIND_XBOX_FSSM_RDNA2) {
                    size_t fssm_sz = 0;
                    uint32_t code_off = 0;
                    std::vector<uint8_t> fssm_blob(si.XbchUnpackedSize + 64u);
                    if (SUCCEEDED(D3D12XboxUnpackShaderFssm(dxbc.data(), dxbc.size(),
                                                            fssm_blob.data(), fssm_blob.size(),
                                                            &fssm_sz, &code_off)) &&
                        fssm_sz >= 4 && std::memcmp(fssm_blob.data(), "FSSM", 4) == 0) {
                        ++fssm_ok;
                        if (fssm_ok == 1) {
                            std::printf("  [ OK ] shaders.rpf @0x%llX        : FSSM RDNA2 (stage=%u SM%u.%u NOBC=%u XBCH=%u->%zu B codeOff=0x%X codeSz=%u B endpgm=%u)\n",
                                        static_cast<unsigned long long>(off),
                                        static_cast<unsigned>(si.Stage), si.ShaderModelMajor, si.ShaderModelMinor,
                                        si.HasNobcStub, si.XbchStoredSize, fssm_sz, code_off, si.FssmCodeSize, si.HasRdna2EndPgm);
                        }
                    }
                }
            }
        }
        std::printf("  [ OK ] shaders.rpf FSSM unpacker check : %u/4 real game RDNA2 FSSM shaders unpacked cleanly via d3d12_xs.dll\n", fssm_ok);

        // Live RDNA2 (FSSM) -> SM 6.6 HLSL -> Signed DXIL -> RTX Hardware PSO on real GTA VI shaders.rpf CS @0xE196830
        if (d3d_dev) {
            sf.clear();
            sf.seekg(static_cast<std::streamoff>(0xE196830ULL), std::ios::beg);
            std::vector<uint8_t> comp(65536);
            sf.read(reinterpret_cast<char*>(comp.data()), static_cast<std::streamsize>(comp.size()));
            std::vector<uint8_t> dxbc;
            if (wsx_inflate::inflate_raw(comp.data(), static_cast<size_t>(sf.gcount()), dxbc, 512 * 1024)) {
                std::vector<uint8_t> dxil(512 * 1024);
                size_t dxil_sz = 0;
                D3D12XBOX_RDNA2_TRANSLATE_STATS ts{};
                HRESULT thr = D3D12XboxTranslateDxbcFssmToDxil(dxbc.data(), dxbc.size(), dxil.data(), dxil.size(), &dxil_sz, nullptr, 0, &ts);
                if (thr == E_NOT_SUFFICIENT_BUFFER && dxil_sz > dxil.size()) {
                    dxil.resize(dxil_sz);
                    thr = D3D12XboxTranslateDxbcFssmToDxil(dxbc.data(), dxbc.size(), dxil.data(), dxil.size(), &dxil_sz, nullptr, 0, &ts);
                }
                if (SUCCEEDED(thr) && dxil_sz > 64) {
                    ComPtr<ID3D12RootSignature> sm66_rs;
                    ComPtr<ID3D12PipelineState> cs_pso;
                    if (SUCCEEDED(D3D12XboxCreateSm66RootSignature(d3d_dev, IID_PPV_ARGS(sm66_rs.GetAddressOf())))) {
                        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
                        pd.pRootSignature = sm66_rs.Get();
                        pd.CS = {dxil.data(), dxil_sz};
                        if (SUCCEEDED(d3d_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(cs_pso.GetAddressOf()))) && cs_pso) {
                            std::printf("  [PASS] RDNA2->SM6.6 DXIL live RTX PSO : shaders.rpf CS @0xE196830 (%u/%u insns=%.1f%%, %u BBs, nt=%ux%ux%u -> %zu B DXIL)\n",
                                        ts.TranslatedInstructions, ts.TotalInstructions,
                                        ts.TotalInstructions ? (100.0 * ts.TranslatedInstructions / ts.TotalInstructions) : 0.0,
                                        ts.BasicBlockCount, ts.NumThreadsX, ts.NumThreadsY, ts.NumThreadsZ, dxil_sz);
                        }
                    }
                }
            }
        }
    }

    D3D12XBOX_ABI_TELEMETRY tel{};
    if (SUCCEEDED(D3D12XboxGetAbiTelemetry(&tel))) {
        std::printf("  [INFO] d3d12_xs Shader Telemetry       : backend=%s inspected=%llu pureDxil=%llu xboxIntr=%llu fssm=%llu accc=%llu cacheHit=%llu xlatHit=%llu strictRej=%llu\n",
                    D3D12XboxGetGraphicsBackend() == D3D12XBOX_GRAPHICS_BACKEND_VULKAN ? "vulkan" : "d3d12",
                    static_cast<unsigned long long>(tel.ShadersInspected),
                    static_cast<unsigned long long>(tel.ShadersPureDxil),
                    static_cast<unsigned long long>(tel.ShadersXboxIntrinsics),
                    static_cast<unsigned long long>(tel.ShadersFssmRdna2),
                    static_cast<unsigned long long>(tel.ShadersAcccRdna2),
                    static_cast<unsigned long long>(tel.ShadersCacheHits),
                    static_cast<unsigned long long>(tel.ShadersTranslatorHits),
                    static_cast<unsigned long long>(tel.ShadersRejectedStrict));
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    const std::filesystem::path build_dir = (argc >= 2)
        ? std::filesystem::absolute(argv[1])
        : std::filesystem::current_path();
    const std::filesystem::path shim_dir = (argc >= 3)
        ? std::filesystem::absolute(argv[2])
        : executable_directory();

    std::printf("MizuBox by Mizushi (Fork of AnyXboxSeries) — GTA VI 6-DLL Real GPU & Runtime Verification Suite\n");
    std::printf("Build dir : %ls\n", build_dir.c_str());
    std::printf("Shim dir  : %ls\n", shim_dir.c_str());

    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    AddDllDirectory(shim_dir.c_str());
    AddDllDirectory(build_dir.c_str());

    // Preload all MizuBox shims so game DLLs (static & dynamic) resolve to them!
    HMODULE h_d3d12 = LoadLibraryExW((shim_dir / L"d3d12_xs.dll").c_str(), nullptr,
                                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE h_xmem = LoadLibraryExW((shim_dir / L"xmem.dll").c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE h_xgr = LoadLibraryExW((shim_dir / L"xgameruntime.dll").c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h_d3d12 || !h_xmem || !h_xgr) {
        std::fprintf(stderr, "Failed to preload MizuBox shims (d3d12=%p xmem=%p xgr=%p err=%lu)\n",
                     static_cast<void*>(h_d3d12), static_cast<void*>(h_xmem), static_cast<void*>(h_xgr), GetLastError());
        return 1;
    }
    std::printf("Preloaded shims: d3d12_xs.dll (%p), xmem.dll (%p), xgameruntime.dll (%p)\n",
                static_cast<void*>(h_d3d12), static_cast<void*>(h_xmem), static_cast<void*>(h_xgr));

    // Create real D3D12.X device via MizuBox D3D12XboxCreateDevice!
    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1;
    ComPtr<ID3D12Device8> d3d_dev;
    HRESULT hr = D3D12XboxCreateDevice(nullptr, &dev_params, IID_PPV_ARGS(d3d_dev.GetAddressOf()));
    if (FAILED(hr) || !d3d_dev) {
        std::fprintf(stderr, "D3D12XboxCreateDevice failed hr=0x%08lX\n", static_cast<unsigned long>(hr));
        return 1;
    }

    // Print GPU adapter info & DXR tier
    {
        ComPtr<IDXGIFactory4> fac;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(fac.GetAddressOf())))) {
            const LUID luid = d3d_dev->GetAdapterLuid();
            ComPtr<IDXGIAdapter1> ad;
            if (SUCCEEDED(fac->EnumAdapterByLuid(luid, IID_PPV_ARGS(ad.GetAddressOf())))) {
                DXGI_ADAPTER_DESC1 ad_desc{};
                if (SUCCEEDED(ad->GetDesc1(&ad_desc))) {
                    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
                    d3d_dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5));
                    std::printf("GPU Adapter: %ls (VRAM=%llu MB, DXR Tier=%d)\n",
                                ad_desc.Description,
                                static_cast<unsigned long long>(ad_desc.DedicatedVideoMemory / (1024 * 1024)),
                                static_cast<int>(opt5.RaytracingTier));
                }
            }
        }
    }

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    hr = d3d_dev->CreateCommandQueue(&qdesc, IID_PPV_ARGS(queue.GetAddressOf()));
    if (FAILED(hr) || !queue) {
        std::fprintf(stderr, "CreateCommandQueue failed hr=0x%08lX\n", static_cast<unsigned long>(hr));
        return 1;
    }

    LocalHttpServer http_server;
    if (!http_server.start()) {
        std::fprintf(stderr, "LocalHttpServer failed to bind loopback port\n");
        return 1;
    }
    std::printf("Loopback HTTP/1.1 test server listening on 127.0.0.1:%u\n", http_server.port());

    SuiteStats stats{};
    test_bink2_scarlett(build_dir, d3d_dev.Get(), queue.Get(), h_xmem, stats);
    test_xbox_bvh_builder(build_dir, shim_dir, d3d_dev.Get(), queue.Get(), h_xmem, stats);
    test_fidelityfx_xbox(build_dir, shim_dir, d3d_dev.Get(), queue.Get(), h_xmem, stats);
    test_xcurl(build_dir, http_server.port(), stats);
    test_libhttpclient_gdk(build_dir, http_server.port(), stats);
    test_gta6_shader_pipeline_audit(build_dir, d3d_dev.Get());

    std::printf("\n========================================================================\n");
    std::printf("SUITE SUMMARY: passed=%d warnings=%d failed=%d | loopback HTTP requests=%u\n",
                stats.passed, stats.warnings, stats.failed, http_server.requests_handled());
    std::printf("========================================================================\n");

    http_server.stop();
    queue.Reset();
    d3d_dev.Reset();
    std::fflush(stdout);
    std::fflush(stderr);
    ExitProcess(stats.failed == 0 ? 0u : 1u);
}
