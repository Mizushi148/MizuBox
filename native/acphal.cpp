// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox Series X|S Audio Co-Processor HAL & APU memory allocator (AcpHal.dll).

#include "include/xbox/acphal.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace {

constexpr GUID kClsidMMDeviceEnumerator = {
    0xBCDE0395u, 0xE52Fu, 0x467Cu, {0x8Eu, 0x3Du, 0xC4u, 0x57u, 0x92u, 0x91u, 0x69u, 0x2Eu}
};
constexpr GUID kIidIMMDeviceEnumerator = {
    0xA95664D2u, 0x9614u, 0x4F35u, {0xA7u, 0x46u, 0xDEu, 0x8Du, 0xB6u, 0x36u, 0x17u, 0xE6u}
};
constexpr GUID kIidIAudioClient = {
    0x1CB9AD4Cu, 0xDBFAu, 0x4C32u, {0xB1u, 0x78u, 0xC2u, 0xF5u, 0x68u, 0xA7u, 0x03u, 0xB2u}
};
constexpr GUID kIidIAudioRenderClient = {
    0xF294ACFCu, 0x3146u, 0x4483u, {0xA7u, 0xBFu, 0xADu, 0xDCu, 0xA7u, 0xC2u, 0x60u, 0xE2u}
};
constexpr GUID kSubFormatIeeeFloat = {
    0x00000003u, 0x0000u, 0x0010u, {0x80u, 0x00u, 0x00u, 0xAAu, 0x00u, 0x38u, 0x9Bu, 0x71u}
};
constexpr GUID kSubFormatPcm = {
    0x00000001u, 0x0000u, 0x0010u, {0x80u, 0x00u, 0x00u, 0xAAu, 0x00u, 0x38u, 0x9Bu, 0x71u}
};

constexpr int kMsAdpcmAdaptationTable[16] = {
    230, 230, 230, 230, 307, 409, 512, 614,
    768, 614, 512, 409, 307, 230, 230, 230
};
constexpr int kMsAdpcmCoeff1[7] = { 256, 512, 0, 192, 240, 460, 392 };
constexpr int kMsAdpcmCoeff2[7] = { 0, -256, 0, 64, 0, -208, -232 };

struct WsxAudioOutputStream {
    std::mutex mutex;
    UINT32 sample_rate = 48000u;
    UINT16 channels = 2u;
    UINT32 buffer_frames = 960u;

    // Live WASAPI objects (loaded dynamically from ole32.dll)
    bool is_wasapi = false;
    bool com_initialized = false;
    bool mix_is_float = true;
    UINT16 mix_channels = 2u;
    UINT16 mix_bits = 32u;
    UINT32 mix_buffer_frames = 0u;
    HMODULE ole32_mod = nullptr;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audio_client = nullptr;
    IAudioRenderClient* render_client = nullptr;

    // Silent software ring sink (always active as telemetry or fallback)
    std::vector<INT16> ring_pcm;
    size_t ring_write_pos = 0u;
    UINT64 total_frames_submitted = 0u;
    UINT64 underrun_count = 0u;
};

using CoInitializeExFn = HRESULT(WINAPI*)(LPVOID, DWORD);
using CoUninitializeFn = void(WINAPI*)();
using CoCreateInstanceFn = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
using CoTaskMemFreeFn = void(WINAPI*)(LPVOID);

bool init_wasapi_output(WsxAudioOutputStream* s) noexcept {
    s->ole32_mod = LoadLibraryW(L"ole32.dll");
    if (!s->ole32_mod) return false;

    const auto pCoInit = reinterpret_cast<CoInitializeExFn>(GetProcAddress(s->ole32_mod, "CoInitializeEx"));
    const auto pCoCreate = reinterpret_cast<CoCreateInstanceFn>(GetProcAddress(s->ole32_mod, "CoCreateInstance"));
    const auto pCoTaskMemFree = reinterpret_cast<CoTaskMemFreeFn>(GetProcAddress(s->ole32_mod, "CoTaskMemFree"));
    if (!pCoInit || !pCoCreate || !pCoTaskMemFree) return false;

    const HRESULT ci_hr = pCoInit(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(ci_hr)) {
        s->com_initialized = true;
    } else if (ci_hr != RPC_E_CHANGED_MODE) {
        return false;
    }

    if (FAILED(pCoCreate(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                         kIidIMMDeviceEnumerator, reinterpret_cast<void**>(&s->enumerator))) ||
        !s->enumerator) {
        return false;
    }
    if (FAILED(s->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &s->device)) || !s->device) {
        return false;
    }
    if (FAILED(s->device->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&s->audio_client))) ||
        !s->audio_client) {
        return false;
    }

    WAVEFORMATEX* mix_fmt = nullptr;
    if (FAILED(s->audio_client->GetMixFormat(&mix_fmt)) || !mix_fmt) {
        return false;
    }

    bool is_float = false;
    bool is_pcm16 = false;
    if (mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && mix_fmt->wBitsPerSample == 32u) {
        is_float = true;
    } else if (mix_fmt->wFormatTag == WAVE_FORMAT_PCM && mix_fmt->wBitsPerSample == 16u) {
        is_pcm16 = true;
    } else if (mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
               mix_fmt->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix_fmt);
        if (std::memcmp(&ext->SubFormat, &kSubFormatIeeeFloat, sizeof(GUID)) == 0 &&
            mix_fmt->wBitsPerSample == 32u) {
            is_float = true;
        } else if (std::memcmp(&ext->SubFormat, &kSubFormatPcm, sizeof(GUID)) == 0 &&
                   mix_fmt->wBitsPerSample == 16u) {
            is_pcm16 = true;
        }
    }

    if (!is_float && !is_pcm16) {
        pCoTaskMemFree(mix_fmt);
        return false;
    }

    const REFERENCE_TIME hns_duration = 500000; // 50 ms shared-mode buffer
    const HRESULT init_hr = s->audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED, 0u, hns_duration, 0, mix_fmt, nullptr);
    s->mix_channels = mix_fmt->nChannels;
    s->mix_bits = mix_fmt->wBitsPerSample;
    s->mix_is_float = is_float;
    pCoTaskMemFree(mix_fmt);

    if (FAILED(init_hr)) return false;
    if (FAILED(s->audio_client->GetBufferSize(&s->mix_buffer_frames)) || s->mix_buffer_frames == 0u) {
        return false;
    }
    if (FAILED(s->audio_client->GetService(kIidIAudioRenderClient,
                                           reinterpret_cast<void**>(&s->render_client))) ||
        !s->render_client) {
        return false;
    }
    if (FAILED(s->audio_client->Start())) {
        return false;
    }
    s->is_wasapi = true;
    return true;
}

void cleanup_wasapi_output(WsxAudioOutputStream* s) noexcept {
    if (!s) return;
    if (s->audio_client && s->is_wasapi) {
        s->audio_client->Stop();
    }
    if (s->render_client) {
        s->render_client->Release();
        s->render_client = nullptr;
    }
    if (s->audio_client) {
        s->audio_client->Release();
        s->audio_client = nullptr;
    }
    if (s->device) {
        s->device->Release();
        s->device = nullptr;
    }
    if (s->enumerator) {
        s->enumerator->Release();
        s->enumerator = nullptr;
    }
    if (s->com_initialized && s->ole32_mod) {
        const auto pCoUninit = reinterpret_cast<CoUninitializeFn>(
            GetProcAddress(s->ole32_mod, "CoUninitialize"));
        if (pCoUninit) pCoUninit();
        s->com_initialized = false;
    }
    if (s->ole32_mod) {
        FreeLibrary(s->ole32_mod);
        s->ole32_mod = nullptr;
    }
    s->is_wasapi = false;
}

// Standard IMA-ADPCM index adjustment & 89-entry step size tables
constexpr int kImaIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

constexpr int kImaStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

INT16 clamp_pcm16(int v) noexcept {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return static_cast<INT16>(v);
}

INT16 clamp_pcm16_float(float v) noexcept {
    if (v >= 32767.0f) return 32767;
    if (v <= -32768.0f) return -32768;
    return static_cast<INT16>(std::lrintf(v));
}

INT16 decode_ima_nibble(uint8_t nibble, int& predictor, int& step_index) noexcept {
    const int step = kImaStepTable[step_index];
    int diff = step >> 3;
    if (nibble & 1u) diff += step >> 2;
    if (nibble & 2u) diff += step >> 1;
    if (nibble & 4u) diff += step;
    if (nibble & 8u) {
        predictor -= diff;
    } else {
        predictor += diff;
    }
    predictor = static_cast<int>(clamp_pcm16(predictor));
    step_index = std::clamp(step_index + kImaIndexTable[nibble & 0x0Fu], 0, 88);
    return static_cast<INT16>(predictor);
}

uint8_t encode_ima_nibble(INT16 sample, int& predictor, int& step_index) noexcept {
    const int step = kImaStepTable[step_index];
    int diff = static_cast<int>(sample) - predictor;
    uint8_t nibble = 0u;
    if (diff < 0) {
        nibble = 8u;
        diff = -diff;
    }
    int vpdiff = step >> 3;
    if (diff >= step) {
        nibble |= 4u;
        diff -= step;
        vpdiff += step;
    }
    if (diff >= (step >> 1)) {
        nibble |= 2u;
        diff -= (step >> 1);
        vpdiff += (step >> 1);
    }
    if (diff >= (step >> 2)) {
        nibble |= 1u;
        vpdiff += (step >> 2);
    }
    if (nibble & 8u) {
        predictor -= vpdiff;
    } else {
        predictor += vpdiff;
    }
    predictor = static_cast<int>(clamp_pcm16(predictor));
    step_index = std::clamp(step_index + kImaIndexTable[nibble & 0x0Fu], 0, 88);
    return nibble;
}

struct ApuAllocation {
    void* raw_base = nullptr;
    uintptr_t aligned_va = 0;
    UINT32 size = 0;
    UINT32 alignment = 0;
    UINT32 flags = 0;
    APU_ADDRESS phys_addr = APU_INVALID_ADDRESS;
};

struct ApuState {
    std::mutex mutex;
    UINT64 cached_budget = 128ull * 1024ull * 1024ull;
    UINT64 noncached_budget = 128ull * 1024ull * 1024ull;
    UINT64 cached_used = 0;
    UINT64 noncached_used = 0;
    APU_ADDRESS next_phys = 0x10000000u;
    std::vector<ApuAllocation> allocs;
    SHAPE_CONTEXTS active_contexts{};
    bool contexts_allocated = false;
};

ApuState& apu_state() {
    static ApuState s;
    return s;
}

constexpr bool is_power_of_two(UINT32 v) noexcept {
    return v != 0u && (v & (v - 1u)) == 0u;
}

UINT32 align_up_u32(UINT32 value, UINT32 align) noexcept {
    return (value + align - 1u) & ~(align - 1u);
}

struct __declspec(uuid("7e4a2b91-5c38-4f10-8a7d-92c1e6b3f042")) IWsxAcpHalInternal : IUnknown {};

class WsxAcpHal final : public IAcpHal {
    struct PendingAsyncCmd {
        ACP_COMMAND_TYPE type = ACP_COMMAND_TYPE_NONE;
        UINT32 command_id = 0u;
        UINT32 flags = 0u;
        std::vector<uint8_t> desc_bytes;
        std::vector<ACP_MIX_VOICE> copied_voices;
    };

    std::atomic<ULONG> ref_{1};
    std::mutex mutex_;
    std::condition_variable cmd_cv_;
    std::condition_variable idle_cv_;
    bool connected_ = false;
    bool worker_stop_ = false;
    bool worker_busy_ = false;
    UINT32 max_messages_ = 256u;
    std::deque<ACP_MESSAGE> messages_;
    std::deque<PendingAsyncCmd> pending_cmds_;
    std::thread worker_;
    LARGE_INTEGER start_qpc_{};
    LARGE_INTEGER qpc_freq_{};

    static HRESULT execute_cmd_payload(
        ACP_COMMAND_TYPE type,
        UINT32 commandId,
        UINT32 flags,
        const void* data,
        UINT32 dataSize,
        const ACP_MIX_VOICE* copied_voices,
        UINT64& out_result_payload) noexcept {
        out_result_payload = commandId;
        const bool allow_host = (flags & ACP_CMD_FLAG_ALLOW_HOST_PTR) != 0u;

        if (data != nullptr && dataSize > 0u) {
            if (type == ACP_COMMAND_TYPE_DMA_TRANSFER) {
                if (dataSize < sizeof(ACP_DMA_TRANSFER_DESC)) return E_INVALIDARG;
                const auto* desc = static_cast<const ACP_DMA_TRANSFER_DESC*>(data);
                if (!desc->src || !desc->dst) return E_POINTER;
                if (!allow_host) {
                    if (!ApuIsVirtualAddressValid(desc->src, desc->byteCount) ||
                        !ApuIsVirtualAddressValid(desc->dst, desc->byteCount)) {
                        return E_INVALIDARG;
                    }
                }
                if (desc->byteCount > 0u) {
                    std::memmove(desc->dst, desc->src, desc->byteCount);
                }
                out_result_payload = desc->byteCount;
            } else if (type == ACP_COMMAND_TYPE_XMA_DECODE) {
                if (dataSize < sizeof(ACP_XMA_DECODE_DESC)) return E_INVALIDARG;
                const auto* desc = static_cast<const ACP_XMA_DECODE_DESC*>(data);
                if (!desc->srcAdpcm || !desc->dstPcm) return E_POINTER;
                if (!allow_host) {
                    const UINT32 dst_bytes = desc->maxDstSamples * static_cast<UINT32>(sizeof(INT16));
                    if (!ApuIsVirtualAddressValid(desc->srcAdpcm, desc->srcByteCount) ||
                        !ApuIsVirtualAddressValid(desc->dstPcm, dst_bytes)) {
                        return E_INVALIDARG;
                    }
                }
                ACP_IMA_ADPCM_STATE st = desc->initialState;
                UINT32 decoded = 0u;
                const HRESULT hr = AcpHalDecodeImaAdpcm(
                    desc->srcAdpcm, desc->srcByteCount,
                    desc->dstPcm, desc->maxDstSamples,
                    &st, &decoded);
                if (FAILED(hr)) return hr;
                out_result_payload = decoded;
            } else if (type == ACP_COMMAND_TYPE_FLOWGRAPH) {
                if (dataSize < sizeof(ACP_FLOWGRAPH_MIX_DESC)) return E_INVALIDARG;
                const auto* desc = static_cast<const ACP_FLOWGRAPH_MIX_DESC*>(data);
                if (!desc->dstStereoPcm) return E_POINTER;
                if (!allow_host) {
                    const UINT32 dst_bytes = desc->dstFrameCount * 2u * static_cast<UINT32>(sizeof(INT16));
                    if (!ApuIsVirtualAddressValid(desc->dstStereoPcm, dst_bytes)) {
                        return E_INVALIDARG;
                    }
                }
                const ACP_MIX_VOICE* voices_ptr = copied_voices ? copied_voices : desc->voices;
                const HRESULT hr = AcpHalMixVoices(
                    voices_ptr, desc->voiceCount,
                    desc->dstStereoPcm, desc->dstFrameCount,
                    desc->clearOutput);
                if (FAILED(hr)) return hr;
                out_result_payload = desc->dstFrameCount;
            }
        }
        return S_OK;
    }

    void worker_loop() noexcept {
        for (;;) {
            PendingAsyncCmd cmd;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cmd_cv_.wait(lk, [this]() { return worker_stop_ || !pending_cmds_.empty(); });
                if (worker_stop_ && pending_cmds_.empty()) break;
                cmd = std::move(pending_cmds_.front());
                pending_cmds_.pop_front();
                worker_busy_ = true;
            }

            UINT64 payload = cmd.command_id;
            const void* data_ptr = cmd.desc_bytes.empty() ? nullptr : cmd.desc_bytes.data();
            const UINT32 data_sz = static_cast<UINT32>(cmd.desc_bytes.size());
            const ACP_MIX_VOICE* voices_ptr = cmd.copied_voices.empty() ? nullptr : cmd.copied_voices.data();
            const HRESULT hr = execute_cmd_payload(
                cmd.type, cmd.command_id, cmd.flags, data_ptr, data_sz, voices_ptr, payload);

            {
                std::lock_guard<std::mutex> lk(mutex_);
                if (SUCCEEDED(hr) && connected_ && messages_.size() < max_messages_) {
                    ACP_MESSAGE msg{};
                    msg.type = static_cast<UINT32>(cmd.type);
                    msg.flags = cmd.flags;
                    msg.timestamp = GetAudioFrameCount();
                    msg.payload = payload;
                    messages_.push_back(msg);
                }
                worker_busy_ = false;
                if (pending_cmds_.empty()) {
                    idle_cv_.notify_all();
                }
            }
        }
    }

    void stop_worker() noexcept {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            worker_stop_ = true;
            pending_cmds_.clear();
        }
        cmd_cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void ensure_worker_locked() {
        if (!worker_.joinable()) {
            worker_stop_ = false;
            worker_ = std::thread([this]() { worker_loop(); });
        }
    }

public:
    WsxAcpHal() noexcept {
        QueryPerformanceFrequency(&qpc_freq_);
        QueryPerformanceCounter(&start_qpc_);
    }

    ~WsxAcpHal() noexcept {
        stop_worker();
    }

    HRESULT flush_async(DWORD timeout_ms) noexcept {
        std::unique_lock<std::mutex> lk(mutex_);
        auto done = [this]() { return pending_cmds_.empty() && !worker_busy_; };
        if (done()) return S_OK;
        if (timeout_ms == INFINITE) {
            idle_cv_.wait(lk, done);
            return S_OK;
        }
        const bool ok = idle_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), done);
        return ok ? S_OK : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IWsxAcpHalInternal)) {
            *ppv = this;
            return S_OK; // Non-AddRef internal unwrap
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAcpHal)) {
            *ppv = static_cast<IAcpHal*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE Connect(UINT32 /*commandQueueLength*/, UINT32 messageQueueLength) override {
        std::lock_guard<std::mutex> lk(mutex_);
        connected_ = true;
        max_messages_ = messageQueueLength > 0 ? messageQueueLength : 256u;
        messages_.clear();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Disconnect() override {
        stop_worker();
        std::lock_guard<std::mutex> lk(mutex_);
        connected_ = false;
        messages_.clear();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SubmitCommand(
        ACP_COMMAND_TYPE type,
        UINT32 commandId,
        UINT32 flags,
        const void* data,
        UINT32 dataSize) override {
        if ((flags & ACP_CMD_FLAG_ASYNC) != 0u) {
            // Asynchronous ACP hardware command ring path: validate upfront, enqueue FIFO for worker thread.
            const bool allow_host = (flags & ACP_CMD_FLAG_ALLOW_HOST_PTR) != 0u;
            PendingAsyncCmd cmd;
            cmd.type = type;
            cmd.command_id = commandId;
            cmd.flags = flags;
            if (data != nullptr && dataSize > 0u) {
                if (type == ACP_COMMAND_TYPE_DMA_TRANSFER) {
                    if (dataSize < sizeof(ACP_DMA_TRANSFER_DESC)) return E_INVALIDARG;
                    const auto* desc = static_cast<const ACP_DMA_TRANSFER_DESC*>(data);
                    if (!desc->src || !desc->dst) return E_POINTER;
                    if (!allow_host &&
                        (!ApuIsVirtualAddressValid(desc->src, desc->byteCount) ||
                         !ApuIsVirtualAddressValid(desc->dst, desc->byteCount))) {
                        return E_INVALIDARG;
                    }
                } else if (type == ACP_COMMAND_TYPE_XMA_DECODE) {
                    if (dataSize < sizeof(ACP_XMA_DECODE_DESC)) return E_INVALIDARG;
                    const auto* desc = static_cast<const ACP_XMA_DECODE_DESC*>(data);
                    if (!desc->srcAdpcm || !desc->dstPcm) return E_POINTER;
                    if (!allow_host) {
                        const UINT32 dst_bytes = desc->maxDstSamples * static_cast<UINT32>(sizeof(INT16));
                        if (!ApuIsVirtualAddressValid(desc->srcAdpcm, desc->srcByteCount) ||
                            !ApuIsVirtualAddressValid(desc->dstPcm, dst_bytes)) {
                            return E_INVALIDARG;
                        }
                    }
                } else if (type == ACP_COMMAND_TYPE_FLOWGRAPH) {
                    if (dataSize < sizeof(ACP_FLOWGRAPH_MIX_DESC)) return E_INVALIDARG;
                    const auto* desc = static_cast<const ACP_FLOWGRAPH_MIX_DESC*>(data);
                    if (!desc->dstStereoPcm) return E_POINTER;
                    if (!allow_host) {
                        const UINT32 dst_bytes = desc->dstFrameCount * 2u * static_cast<UINT32>(sizeof(INT16));
                        if (!ApuIsVirtualAddressValid(desc->dstStereoPcm, dst_bytes)) {
                            return E_INVALIDARG;
                        }
                    }
                    if (desc->voices && desc->voiceCount > 0u) {
                        cmd.copied_voices.assign(desc->voices, desc->voices + desc->voiceCount);
                    }
                }
                const auto* bp = static_cast<const uint8_t*>(data);
                cmd.desc_bytes.assign(bp, bp + dataSize);
            }
            {
                std::lock_guard<std::mutex> lk(mutex_);
                if (!connected_) return E_FAIL;
                ensure_worker_locked();
                pending_cmds_.push_back(std::move(cmd));
            }
            cmd_cv_.notify_one();
            return S_OK;
        }

        // Preserve strict FIFO ordering if async jobs are in-flight before a synchronous command
        flush_async(INFINITE);

        UINT64 result_payload = commandId;
        const HRESULT exec_hr = execute_cmd_payload(type, commandId, flags, data, dataSize, nullptr, result_payload);
        if (FAILED(exec_hr)) return exec_hr;

        std::lock_guard<std::mutex> lk(mutex_);
        if (!connected_) return E_FAIL;
        if (messages_.size() < max_messages_) {
            ACP_MESSAGE msg{};
            msg.type = static_cast<UINT32>(type);
            msg.flags = flags;
            msg.timestamp = GetAudioFrameCount();
            msg.payload = result_payload;
            messages_.push_back(msg);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE PopMessage(ACP_MESSAGE* outMessage) override {
        if (!outMessage) return E_POINTER;
        std::lock_guard<std::mutex> lk(mutex_);
        if (messages_.empty()) return S_FALSE;
        *outMessage = messages_.front();
        messages_.pop_front();
        return S_OK;
    }

    UINT64 STDMETHODCALLTYPE GetAudioFrameCount() override {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (qpc_freq_.QuadPart <= 0) return 0;
        const UINT64 elapsed_us = static_cast<UINT64>(
            (now.QuadPart - start_qpc_.QuadPart) * 1000000LL / qpc_freq_.QuadPart);
        // 48 kHz audio clock = 48 frames per millisecond
        return (elapsed_us * 48ull) / 1000ull;
    }
};

} // namespace

extern "C" {

HRESULT WINAPI ApuCreateHeap(UINT32 cachedSizeInBytes, UINT32 nonCachedSizeInBytes) {
    auto& st = apu_state();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (cachedSizeInBytes > 0) st.cached_budget = cachedSizeInBytes;
    if (nonCachedSizeInBytes > 0) st.noncached_budget = nonCachedSizeInBytes;
    return S_OK;
}

HRESULT WINAPI ApuAlloc(
    void** virtualAddress,
    APU_ADDRESS* physicalAddress,
    UINT32 sizeInBytes,
    UINT32 alignmentInBytes,
    UINT32 flags) {
    if (!virtualAddress) return E_POINTER;
    *virtualAddress = nullptr;
    if (physicalAddress) *physicalAddress = APU_INVALID_ADDRESS;
    if (sizeInBytes == 0u) return E_INVALIDARG;

    const UINT32 align = (alignmentInBytes == 0u) ? 64u : alignmentInBytes;
    if (!is_power_of_two(align) || align > (16u * 1024u * 1024u)) {
        return E_INVALIDARG;
    }

    auto& st = apu_state();
    std::lock_guard<std::mutex> lk(st.mutex);

    const bool non_cached = (flags & APU_ALLOC_NONCACHED) != 0u;
    const UINT64 budget = non_cached ? st.noncached_budget : st.cached_budget;
    UINT64& used = non_cached ? st.noncached_used : st.cached_used;
    if (used + sizeInBytes > budget) {
        return E_OUTOFMEMORY;
    }

    // VirtualAlloc always returns 64 KB aligned pages; over-allocate if align > 64 KB
    const SIZE_T extra = (align > 65536u) ? static_cast<SIZE_T>(align) : 0u;
    const SIZE_T alloc_bytes = static_cast<SIZE_T>(sizeInBytes) + extra;
    void* raw = VirtualAlloc(nullptr, alloc_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!raw) return E_OUTOFMEMORY;

    const uintptr_t raw_addr = reinterpret_cast<uintptr_t>(raw);
    const uintptr_t aligned = (raw_addr + (align - 1u)) & ~static_cast<uintptr_t>(align - 1u);

    const UINT32 pa_align = (align < 64u) ? 64u : align;
    const APU_ADDRESS pa = align_up_u32(st.next_phys, pa_align);
    const UINT32 aligned_span = align_up_u32(sizeInBytes, 64u);
    if (static_cast<UINT64>(pa) + aligned_span >= 0xF0000000ull) {
        VirtualFree(raw, 0, MEM_RELEASE);
        return E_OUTOFMEMORY;
    }
    st.next_phys = pa + aligned_span;
    used += sizeInBytes;

    ApuAllocation entry{};
    entry.raw_base = raw;
    entry.aligned_va = aligned;
    entry.size = sizeInBytes;
    entry.alignment = align;
    entry.flags = flags;
    entry.phys_addr = pa;
    st.allocs.push_back(entry);

    *virtualAddress = reinterpret_cast<void*>(aligned);
    if (physicalAddress) *physicalAddress = pa;
    return S_OK;
}

HRESULT WINAPI ApuFree(void* virtualAddress) {
    if (!virtualAddress) return S_OK;
    const uintptr_t target = reinterpret_cast<uintptr_t>(virtualAddress);

    auto& st = apu_state();
    std::lock_guard<std::mutex> lk(st.mutex);
    for (auto it = st.allocs.begin(); it != st.allocs.end(); ++it) {
        if (it->aligned_va == target) {
            const bool non_cached = (it->flags & APU_ALLOC_NONCACHED) != 0u;
            UINT64& used = non_cached ? st.noncached_used : st.cached_used;
            used = (used >= it->size) ? (used - it->size) : 0u;
            VirtualFree(it->raw_base, 0, MEM_RELEASE);
            st.allocs.erase(it);
            return S_OK;
        }
    }
    return E_INVALIDARG;
}

APU_ADDRESS WINAPI ApuMapVirtualAddress(const void* virtualAddress) {
    if (!virtualAddress) return APU_INVALID_ADDRESS;
    const uintptr_t target = reinterpret_cast<uintptr_t>(virtualAddress);

    auto& st = apu_state();
    std::lock_guard<std::mutex> lk(st.mutex);
    for (const auto& a : st.allocs) {
        if (target >= a.aligned_va && (target - a.aligned_va) < a.size) {
            return static_cast<APU_ADDRESS>(a.phys_addr + static_cast<UINT32>(target - a.aligned_va));
        }
    }
    return APU_INVALID_ADDRESS;
}

void* WINAPI ApuMapApuAddress(APU_ADDRESS physicalAddress) {
    if (physicalAddress == APU_INVALID_ADDRESS) return nullptr;

    auto& st = apu_state();
    std::lock_guard<std::mutex> lk(st.mutex);
    for (const auto& a : st.allocs) {
        if (physicalAddress >= a.phys_addr && (physicalAddress - a.phys_addr) < a.size) {
            return reinterpret_cast<void*>(a.aligned_va + static_cast<uintptr_t>(physicalAddress - a.phys_addr));
        }
    }
    return nullptr;
}

BOOL WINAPI ApuIsVirtualAddressValid(const void* virtualAddress, UINT32 sizeInBytes) {
    if (!virtualAddress) return FALSE;
    const uintptr_t target = reinterpret_cast<uintptr_t>(virtualAddress);

    auto& st = apu_state();
    std::lock_guard<std::mutex> lk(st.mutex);
    for (const auto& a : st.allocs) {
        if (target >= a.aligned_va) {
            const uintptr_t offset = target - a.aligned_va;
            if (offset < a.size && static_cast<UINT64>(offset) + sizeInBytes <= a.size) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

HRESULT WINAPI ApuHeapGetState(APU_HEAP_STATE* outState, UINT32 /*flags*/) {
    if (!outState) return E_POINTER;

    auto& st = apu_state();
    std::lock_guard<std::mutex> lk(st.mutex);
    outState->cachedSizeInBytes = static_cast<UINT32>(st.cached_budget);
    outState->cachedUsedInBytes = static_cast<UINT32>(st.cached_used);
    outState->nonCachedSizeInBytes = static_cast<UINT32>(st.noncached_budget);
    outState->nonCachedUsedInBytes = static_cast<UINT32>(st.noncached_used);
    outState->allocationCount = static_cast<UINT32>(st.allocs.size());
    outState->reserved = 0u;
    return S_OK;
}

HRESULT WINAPI AcpHalCreate(IAcpHal** ppAcpHal) {
    if (!ppAcpHal) return E_POINTER;
    *ppAcpHal = new (std::nothrow) WsxAcpHal();
    return *ppAcpHal ? S_OK : E_OUTOFMEMORY;
}

HRESULT WINAPI AcpHalAllocateShapeContexts(const SHAPE_CONTEXTS* pRequestedContexts) {
    if (!pRequestedContexts) return E_POINTER;
    // Enforce sane Xbox Series X SHAPE hardware ceilings
    if (pRequestedContexts->numSrcContexts > 2048u ||
        pRequestedContexts->numEqContexts > 2048u ||
        pRequestedContexts->numFiltVolContexts > 2048u ||
        pRequestedContexts->numDmaContexts > 512u ||
        pRequestedContexts->numXmaContexts > 1024u ||
        pRequestedContexts->numPcmContexts > 2048u) {
        return E_INVALIDARG;
    }
    auto& st = apu_state();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.active_contexts = *pRequestedContexts;
    st.contexts_allocated = true;
    return S_OK;
}

HRESULT WINAPI AcpHalReleaseShapeContexts() {
    auto& st = apu_state();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.active_contexts = {};
    st.contexts_allocated = false;
    return S_OK;
}

HRESULT WINAPI AcpHalEncodeImaAdpcm(
    const INT16* srcPcm,
    UINT32 sampleCount,
    UINT8* dstAdpcm,
    UINT32 dstCapacityBytes,
    ACP_IMA_ADPCM_STATE* ioState,
    UINT32* outEncodedBytes) {
    if (outEncodedBytes) *outEncodedBytes = 0u;
    if (!srcPcm || !dstAdpcm) return E_POINTER;
    if (sampleCount == 0u) return S_OK;

    const UINT32 needed = (sampleCount + 1u) / 2u;
    if (dstCapacityBytes < needed) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

    int pred = ioState ? std::clamp(ioState->predictor, -32768, 32767) : 0;
    int step_idx = ioState ? std::clamp(ioState->stepIndex, 0, 88) : 0;

    UINT32 out_i = 0u;
    for (UINT32 i = 0u; i < sampleCount; i += 2u) {
        const uint8_t lo = encode_ima_nibble(srcPcm[i], pred, step_idx);
        const uint8_t hi = (i + 1u < sampleCount)
            ? encode_ima_nibble(srcPcm[i + 1u], pred, step_idx)
            : 0u;
        dstAdpcm[out_i++] = static_cast<uint8_t>((lo & 0x0Fu) | ((hi & 0x0Fu) << 4));
    }

    if (ioState) {
        ioState->predictor = pred;
        ioState->stepIndex = step_idx;
    }
    if (outEncodedBytes) *outEncodedBytes = out_i;
    return S_OK;
}

HRESULT WINAPI AcpHalDecodeImaAdpcm(
    const UINT8* srcAdpcm,
    UINT32 srcBytes,
    INT16* dstPcm,
    UINT32 maxSamples,
    ACP_IMA_ADPCM_STATE* ioState,
    UINT32* outDecodedSamples) {
    if (outDecodedSamples) *outDecodedSamples = 0u;
    if (!srcAdpcm || !dstPcm) return E_POINTER;
    if (srcBytes == 0u || maxSamples == 0u) return S_OK;

    int pred = ioState ? std::clamp(ioState->predictor, -32768, 32767) : 0;
    int step_idx = ioState ? std::clamp(ioState->stepIndex, 0, 88) : 0;

    UINT32 produced = 0u;
    for (UINT32 b = 0u; b < srcBytes && produced < maxSamples; ++b) {
        const uint8_t byte = srcAdpcm[b];
        dstPcm[produced++] = decode_ima_nibble(static_cast<uint8_t>(byte & 0x0Fu), pred, step_idx);
        if (produced < maxSamples) {
            dstPcm[produced++] = decode_ima_nibble(static_cast<uint8_t>((byte >> 4) & 0x0Fu), pred, step_idx);
        }
    }

    if (ioState) {
        ioState->predictor = pred;
        ioState->stepIndex = step_idx;
    }
    if (outDecodedSamples) *outDecodedSamples = produced;
    return S_OK;
}

HRESULT WINAPI AcpHalMixVoices(
    const ACP_MIX_VOICE* voices,
    UINT32 voiceCount,
    INT16* dstStereoPcm,
    UINT32 dstFrameCount,
    BOOL clearOutput) {
    if (!dstStereoPcm) return E_POINTER;
    if (voiceCount > 0u && !voices) return E_POINTER;
    if (dstFrameCount == 0u) return S_OK;

    std::vector<float> accum(static_cast<size_t>(dstFrameCount) * 2u, 0.0f);
    if (!clearOutput) {
        for (UINT32 i = 0u; i < dstFrameCount * 2u; ++i) {
            accum[i] = static_cast<float>(dstStereoPcm[i]);
        }
    }

    for (UINT32 vi = 0u; vi < voiceCount; ++vi) {
        const ACP_MIX_VOICE& v = voices[vi];
        if (!v.pcmSamples || v.frameCount == 0u || v.outputFrameOffset >= dstFrameCount) continue;
        if (v.channels != 1u && v.channels != 2u) return E_INVALIDARG;

        const float pan = std::clamp(v.pan, -1.0f, 1.0f);
        const float gain_l = v.gain * std::sqrt(0.5f * (1.0f - pan));
        const float gain_r = v.gain * std::sqrt(0.5f * (1.0f + pan));
        const UINT32 avail = dstFrameCount - v.outputFrameOffset;
        const UINT32 frames = (std::min)(v.frameCount, avail);

        for (UINT32 f = 0u; f < frames; ++f) {
            const size_t dst_idx = static_cast<size_t>(v.outputFrameOffset + f) * 2u;
            if (v.channels == 1u) {
                const float s = static_cast<float>(v.pcmSamples[f]);
                accum[dst_idx + 0u] += s * gain_l;
                accum[dst_idx + 1u] += s * gain_r;
            } else {
                const float sl = static_cast<float>(v.pcmSamples[static_cast<size_t>(f) * 2u + 0u]);
                const float sr = static_cast<float>(v.pcmSamples[static_cast<size_t>(f) * 2u + 1u]);
                accum[dst_idx + 0u] += sl * gain_l * 1.41421356f;
                accum[dst_idx + 1u] += sr * gain_r * 1.41421356f;
            }
        }
    }

    for (UINT32 i = 0u; i < dstFrameCount * 2u; ++i) {
        dstStereoPcm[i] = clamp_pcm16_float(accum[i]);
    }
    return S_OK;
}

HRESULT WINAPI AcpHalOpenAudioOutput(
    const ACP_AUDIO_OUTPUT_DESC* desc,
    ACP_AUDIO_OUTPUT_HANDLE* outHandle) {
    if (!outHandle) return E_POINTER;
    *outHandle = nullptr;

    auto* stream = new (std::nothrow) WsxAudioOutputStream();
    if (!stream) return E_OUTOFMEMORY;

    stream->sample_rate = (desc && desc->sampleRate > 0u) ? desc->sampleRate : 48000u;
    stream->channels = (desc && (desc->channels == 1u || desc->channels == 2u)) ? desc->channels : 2u;
    stream->buffer_frames = (desc && desc->bufferFrames > 0u) ? desc->bufferFrames : 960u;
    stream->ring_pcm.assign(static_cast<size_t>(stream->buffer_frames) * stream->channels * 4u, 0);

    const bool silent = (desc && desc->preferSilentFallback != FALSE);
    if (!silent) {
        if (!init_wasapi_output(stream)) {
            cleanup_wasapi_output(stream);
        }
    }

    *outHandle = static_cast<ACP_AUDIO_OUTPUT_HANDLE>(stream);
    return S_OK;
}

HRESULT WINAPI AcpHalSubmitAudioOutput(
    ACP_AUDIO_OUTPUT_HANDLE handle,
    const INT16* interleavedPcm,
    UINT32 frameCount,
    UINT32* outFramesWritten) {
    if (outFramesWritten) *outFramesWritten = 0u;
    if (!handle) return E_INVALIDARG;
    if (!interleavedPcm && frameCount > 0u) return E_POINTER;
    if (frameCount == 0u) return S_OK;

    auto* stream = static_cast<WsxAudioOutputStream*>(handle);
    std::lock_guard<std::mutex> lk(stream->mutex);

    // Always mirror into the software ring buffer for deterministic telemetry / fallback
    const size_t ring_cap = stream->ring_pcm.size();
    if (ring_cap > 0u) {
        const size_t samples = static_cast<size_t>(frameCount) * stream->channels;
        for (size_t i = 0u; i < samples; ++i) {
            stream->ring_pcm[stream->ring_write_pos] = interleavedPcm[i];
            stream->ring_write_pos = (stream->ring_write_pos + 1u) % ring_cap;
        }
    }

    UINT32 written = frameCount;
    if (stream->is_wasapi && stream->audio_client && stream->render_client) {
        UINT32 padding = 0u;
        if (SUCCEEDED(stream->audio_client->GetCurrentPadding(&padding)) &&
            stream->mix_buffer_frames > padding) {
            if (padding == 0u && stream->total_frames_submitted > 0u) {
                ++stream->underrun_count;
            }
            const UINT32 avail = stream->mix_buffer_frames - padding;
            const UINT32 to_write = (std::min)(frameCount, avail);
            if (to_write > 0u) {
                BYTE* dst_bytes = nullptr;
                if (SUCCEEDED(stream->render_client->GetBuffer(to_write, &dst_bytes)) && dst_bytes) {
                    const UINT16 src_ch = stream->channels;
                    const UINT16 dst_ch = stream->mix_channels;
                    if (stream->mix_is_float) {
                        auto* out_f = reinterpret_cast<float*>(dst_bytes);
                        constexpr float kInv32768 = 1.0f / 32768.0f;
                        for (UINT32 f = 0u; f < to_write; ++f) {
                            const float l = static_cast<float>(interleavedPcm[static_cast<size_t>(f) * src_ch]) * kInv32768;
                            const float r = (src_ch >= 2u)
                                ? static_cast<float>(interleavedPcm[static_cast<size_t>(f) * src_ch + 1u]) * kInv32768
                                : l;
                            for (UINT16 c = 0u; c < dst_ch; ++c) {
                                out_f[static_cast<size_t>(f) * dst_ch + c] =
                                    (c == 0u) ? l : ((c == 1u) ? r : 0.0f);
                            }
                        }
                    } else {
                        auto* out_i16 = reinterpret_cast<INT16*>(dst_bytes);
                        for (UINT32 f = 0u; f < to_write; ++f) {
                            const INT16 l = interleavedPcm[static_cast<size_t>(f) * src_ch];
                            const INT16 r = (src_ch >= 2u)
                                ? interleavedPcm[static_cast<size_t>(f) * src_ch + 1u]
                                : l;
                            for (UINT16 c = 0u; c < dst_ch; ++c) {
                                out_i16[static_cast<size_t>(f) * dst_ch + c] =
                                    (c == 0u) ? l : ((c == 1u) ? r : static_cast<INT16>(0));
                            }
                        }
                    }
                    stream->render_client->ReleaseBuffer(to_write, 0u);
                    written = to_write;
                }
            }
        }
    }

    stream->total_frames_submitted += written;
    if (outFramesWritten) *outFramesWritten = written;
    return S_OK;
}

HRESULT WINAPI AcpHalCloseAudioOutput(ACP_AUDIO_OUTPUT_HANDLE handle) {
    if (!handle) return S_OK;
    auto* stream = static_cast<WsxAudioOutputStream*>(handle);
    {
        std::lock_guard<std::mutex> lk(stream->mutex);
        cleanup_wasapi_output(stream);
    }
    delete stream;
    return S_OK;
}

HRESULT WINAPI AcpHalResampleLinear(
    const INT16* srcPcm,
    UINT32 srcFrames,
    UINT32 srcSampleRate,
    UINT16 channels,
    INT16* dstPcm,
    UINT32 dstCapacityFrames,
    UINT32 dstSampleRate,
    UINT32* outDstFrames) {
    if (outDstFrames) *outDstFrames = 0u;
    if (!srcPcm || !dstPcm) return E_POINTER;
    if (channels == 0u || channels > 8u || srcSampleRate == 0u || dstSampleRate == 0u) {
        return E_INVALIDARG;
    }
    if (srcFrames == 0u) return S_OK;

    const UINT64 expected_u64 = (static_cast<UINT64>(srcFrames) * dstSampleRate + (srcSampleRate / 2u)) / srcSampleRate;
    const UINT32 out_frames = static_cast<UINT32>((std::max)(static_cast<UINT64>(1u), expected_u64));
    if (dstCapacityFrames < out_frames) {
        if (outDstFrames) *outDstFrames = out_frames;
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    const double step = static_cast<double>(srcSampleRate) / static_cast<double>(dstSampleRate);
    for (UINT32 f = 0u; f < out_frames; ++f) {
        const double src_pos = static_cast<double>(f) * step;
        const UINT32 idx0 = (std::min)(static_cast<UINT32>(src_pos), srcFrames - 1u);
        const UINT32 idx1 = (std::min)(idx0 + 1u, srcFrames - 1u);
        const float frac = static_cast<float>(src_pos - static_cast<double>(idx0));
        for (UINT16 c = 0u; c < channels; ++c) {
            const float s0 = static_cast<float>(srcPcm[static_cast<size_t>(idx0) * channels + c]);
            const float s1 = static_cast<float>(srcPcm[static_cast<size_t>(idx1) * channels + c]);
            dstPcm[static_cast<size_t>(f) * channels + c] =
                clamp_pcm16_float(s0 + (s1 - s0) * frac);
        }
    }

    if (outDstFrames) *outDstFrames = out_frames;
    return S_OK;
}

HRESULT WINAPI AcpHalApplyBiquadFilter(
    INT16* ioPcm,
    UINT32 frameCount,
    UINT16 channels,
    const ACP_BIQUAD_PARAMS* params,
    ACP_BIQUAD_STATE* ioChannelStates) {
    if (!ioPcm || !params) return E_POINTER;
    if (channels == 0u || channels > 8u || params->sampleRate <= 0.0f ||
        params->cutoffHz <= 0.0f || params->cutoffHz >= 0.495f * params->sampleRate) {
        return E_INVALIDARG;
    }
    if (frameCount == 0u) return S_OK;

    const float q = (params->q > 0.05f) ? params->q : 0.70710678f;
    constexpr float kTwoPi = 6.28318530717958647692f;
    const float w0 = kTwoPi * params->cutoffHz / params->sampleRate;
    const float cos_w0 = std::cos(w0);
    const float sin_w0 = std::sin(w0);
    const float alpha = sin_w0 / (2.0f * q);

    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;

    if (params->type == ACP_BIQUAD_LOWPASS) {
        b0 = (1.0f - cos_w0) * 0.5f;
        b1 = 1.0f - cos_w0;
        b2 = b0;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cos_w0;
        a2 = 1.0f - alpha;
    } else if (params->type == ACP_BIQUAD_HIGHPASS) {
        b0 = (1.0f + cos_w0) * 0.5f;
        b1 = -(1.0f + cos_w0);
        b2 = b0;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cos_w0;
        a2 = 1.0f - alpha;
    } else if (params->type == ACP_BIQUAD_PEAKING) {
        const float a_gain = std::pow(10.0f, params->gainDb / 40.0f);
        b0 = 1.0f + alpha * a_gain;
        b1 = -2.0f * cos_w0;
        b2 = 1.0f - alpha * a_gain;
        a0 = 1.0f + alpha / a_gain;
        a1 = -2.0f * cos_w0;
        a2 = 1.0f - alpha / a_gain;
    } else {
        return E_INVALIDARG;
    }

    const float inv_a0 = 1.0f / a0;
    b0 *= inv_a0;
    b1 *= inv_a0;
    b2 *= inv_a0;
    a1 *= inv_a0;
    a2 *= inv_a0;

    ACP_BIQUAD_STATE local_states[8]{};
    ACP_BIQUAD_STATE* st = ioChannelStates ? ioChannelStates : local_states;

    for (UINT32 f = 0u; f < frameCount; ++f) {
        for (UINT16 c = 0u; c < channels; ++c) {
            const size_t idx = static_cast<size_t>(f) * channels + c;
            const float x = static_cast<float>(ioPcm[idx]);
            const float y = b0 * x + st[c].z1;
            st[c].z1 = b1 * x - a1 * y + st[c].z2;
            st[c].z2 = b2 * x - a2 * y;
            ioPcm[idx] = clamp_pcm16_float(y);
        }
    }
    return S_OK;
}

HRESULT WINAPI AcpHalDecodeImaAdpcmMultichannel(
    const UINT8* srcAdpcm,
    UINT32 srcBytes,
    UINT16 channels,
    INT16* dstInterleavedPcm,
    UINT32 maxFrames,
    ACP_IMA_ADPCM_STATE* ioChannelStates,
    UINT32* outDecodedFrames) {
    if (outDecodedFrames) *outDecodedFrames = 0u;
    if (!srcAdpcm || !dstInterleavedPcm) return E_POINTER;
    if (channels == 0u || channels > 8u) return E_INVALIDARG;
    if (srcBytes == 0u || maxFrames == 0u) return S_OK;

    if (channels == 1u) {
        return AcpHalDecodeImaAdpcm(
            srcAdpcm, srcBytes, dstInterleavedPcm, maxFrames,
            ioChannelStates, outDecodedFrames);
    }

    int pred[8]{};
    int step_idx[8]{};
    for (UINT16 c = 0u; c < channels; ++c) {
        if (ioChannelStates) {
            pred[c] = std::clamp(ioChannelStates[c].predictor, -32768, 32767);
            step_idx[c] = std::clamp(ioChannelStates[c].stepIndex, 0, 88);
        }
    }

    // Standard WAV/Xbox multichannel IMA-ADPCM: 4-byte (8-sample) chunk per channel in round-robin order.
    const UINT32 chunk_bytes = static_cast<UINT32>(channels) * 4u;
    UINT32 pos = 0u;
    UINT32 frames_out = 0u;

    while (pos + chunk_bytes <= srcBytes && frames_out < maxFrames) {
        const UINT32 frames_this_chunk = (std::min)(8u, maxFrames - frames_out);
        for (UINT16 c = 0u; c < channels; ++c) {
            const UINT8* ch_src = srcAdpcm + pos + static_cast<size_t>(c) * 4u;
            for (UINT32 s = 0u; s < frames_this_chunk; ++s) {
                const uint8_t b = ch_src[s >> 1];
                const uint8_t nib = (s & 1u)
                    ? static_cast<uint8_t>((b >> 4) & 0x0Fu)
                    : static_cast<uint8_t>(b & 0x0Fu);
                dstInterleavedPcm[static_cast<size_t>(frames_out + s) * channels + c] =
                    decode_ima_nibble(nib, pred[c], step_idx[c]);
            }
        }
        pos += chunk_bytes;
        frames_out += frames_this_chunk;
    }

    if (ioChannelStates) {
        for (UINT16 c = 0u; c < channels; ++c) {
            ioChannelStates[c].predictor = pred[c];
            ioChannelStates[c].stepIndex = step_idx[c];
        }
    }
    if (outDecodedFrames) *outDecodedFrames = frames_out;
    return S_OK;
}

HRESULT WINAPI AcpHalDecodeMsAdpcm(
    const UINT8* srcBlock,
    UINT32 srcBytes,
    UINT16 channels,
    INT16* dstInterleavedPcm,
    UINT32 maxFrames,
    UINT32* outDecodedFrames) {
    if (outDecodedFrames) *outDecodedFrames = 0u;
    if (!srcBlock || !dstInterleavedPcm) return E_POINTER;
    if (channels == 0u || channels > 2u) return E_INVALIDARG;

    const UINT32 header_bytes = static_cast<UINT32>(channels) * 7u;
    if (srcBytes < header_bytes || maxFrames < 2u) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    auto read_i16_le = [](const UINT8* p) noexcept -> INT16 {
        return static_cast<INT16>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
    };

    int coeff1[2]{};
    int coeff2[2]{};
    int delta[2]{};
    int samp1[2]{};
    int samp2[2]{};

    UINT32 off = 0u;
    for (UINT16 c = 0u; c < channels; ++c) {
        const uint8_t p_idx = srcBlock[off++];
        if (p_idx >= 7u) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        coeff1[c] = kMsAdpcmCoeff1[p_idx];
        coeff2[c] = kMsAdpcmCoeff2[p_idx];
    }
    for (UINT16 c = 0u; c < channels; ++c) {
        delta[c] = read_i16_le(srcBlock + off);
        if (delta[c] < 16) delta[c] = 16;
        off += 2u;
    }
    for (UINT16 c = 0u; c < channels; ++c) {
        samp1[c] = read_i16_le(srcBlock + off);
        off += 2u;
    }
    for (UINT16 c = 0u; c < channels; ++c) {
        samp2[c] = read_i16_le(srcBlock + off);
        off += 2u;
    }

    // Standard MS-ADPCM emits samp2 first, then samp1
    for (UINT16 c = 0u; c < channels; ++c) {
        dstInterleavedPcm[c] = clamp_pcm16(samp2[c]);
        dstInterleavedPcm[channels + c] = clamp_pcm16(samp1[c]);
    }
    UINT32 frames_out = 2u;

    auto decode_one = [&](UINT16 c, uint8_t raw_nib) noexcept -> INT16 {
        const int signed_nib = (raw_nib & 0x08u) ? (static_cast<int>(raw_nib) - 16) : static_cast<int>(raw_nib);
        int pred = ((samp1[c] * coeff1[c]) + (samp2[c] * coeff2[c])) / 256;
        pred += signed_nib * delta[c];
        const INT16 out_s = clamp_pcm16(pred);
        samp2[c] = samp1[c];
        samp1[c] = static_cast<int>(out_s);
        delta[c] = (kMsAdpcmAdaptationTable[raw_nib & 0x0Fu] * delta[c]) / 256;
        if (delta[c] < 16) delta[c] = 16;
        return out_s;
    };

    if (channels == 1u) {
        for (; off < srcBytes && frames_out < maxFrames; ++off) {
            const uint8_t b = srcBlock[off];
            dstInterleavedPcm[frames_out++] = decode_one(0u, static_cast<uint8_t>((b >> 4) & 0x0Fu));
            if (frames_out < maxFrames) {
                dstInterleavedPcm[frames_out++] = decode_one(0u, static_cast<uint8_t>(b & 0x0Fu));
            }
        }
    } else {
        // Stereo MS-ADPCM: high nibble = left channel (0), low nibble = right channel (1)
        for (; off < srcBytes && frames_out < maxFrames; ++off) {
            const uint8_t b = srcBlock[off];
            dstInterleavedPcm[static_cast<size_t>(frames_out) * 2u + 0u] =
                decode_one(0u, static_cast<uint8_t>((b >> 4) & 0x0Fu));
            dstInterleavedPcm[static_cast<size_t>(frames_out) * 2u + 1u] =
                decode_one(1u, static_cast<uint8_t>(b & 0x0Fu));
            ++frames_out;
        }
    }

    if (outDecodedFrames) *outDecodedFrames = frames_out;
    return S_OK;
}

// ============================================================================
// Wave 3 Next-Gen Audio DSP: 3D Spatial (HRTF ITD/ILD/Head-Shadow/Doppler),
// Schroeder-Moorer Stereo Reverb, Cubic Catmull-Rom SRC, Soft Limiter,
// ITU-R BS.775 5.1/7.1 Downmix & WASAPI Output Telemetry
// ============================================================================

namespace {

struct WsxCombFilter {
    std::vector<float> buf;
    size_t idx = 0u;
    float filter_store = 0.0f;

    void init(size_t delay_samples) {
        buf.assign((std::max<size_t>)(16u, delay_samples), 0.0f);
        idx = 0u;
        filter_store = 0.0f;
    }

    float process(float in, float feedback, float damp1, float damp2) noexcept {
        const float out = buf[idx];
        filter_store = (out * damp2) + (filter_store * damp1);
        buf[idx] = in + (filter_store * feedback);
        if (++idx >= buf.size()) idx = 0u;
        return out;
    }
};

struct WsxAllpassFilter {
    std::vector<float> buf;
    size_t idx = 0u;

    void init(size_t delay_samples) {
        buf.assign((std::max<size_t>)(8u, delay_samples), 0.0f);
        idx = 0u;
    }

    float process(float in) noexcept {
        const float bufout = buf[idx];
        const float out = -in + bufout;
        buf[idx] = in + (bufout * 0.5f);
        if (++idx >= buf.size()) idx = 0u;
        return out;
    }
};

struct WsxReverbInstance {
    std::mutex mutex;
    WsxCombFilter comb_l[4];
    WsxCombFilter comb_r[4];
    WsxAllpassFilter ap_l[2];
    WsxAllpassFilter ap_r[2];
    float feedback = 0.80f;
    float damp1 = 0.45f;
    float damp2 = 0.55f;
    float wet1 = 0.35f;
    float wet2 = 0.0f;
    float dry = 0.85f;
};

inline float vec3_dot(const ACP_VEC3& a, const ACP_VEC3& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline ACP_VEC3 vec3_sub(const ACP_VEC3& a, const ACP_VEC3& b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline ACP_VEC3 vec3_cross(const ACP_VEC3& a, const ACP_VEC3& b) noexcept {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

inline ACP_VEC3 vec3_norm(const ACP_VEC3& v, const ACP_VEC3& fallback) noexcept {
    const float len2 = vec3_dot(v, v);
    if (len2 < 1e-12f) return fallback;
    const float inv = 1.0f / std::sqrt(len2);
    return {v.x * inv, v.y * inv, v.z * inv};
}

inline float sample_delay_ring(const float ring[64], UINT32 write_pos, float delay_samples) noexcept {
    const float clamped = std::clamp(delay_samples, 0.0f, 62.0f);
    const UINT32 d0 = static_cast<UINT32>(clamped);
    const float frac = clamped - static_cast<float>(d0);
    const UINT32 i0 = (write_pos - 1u - d0) & 63u;
    const UINT32 i1 = (write_pos - 2u - d0) & 63u;
    return ring[i0] + (ring[i1] - ring[i0]) * frac;
}

inline float catmull_rom(float y0, float y1, float y2, float y3, float t) noexcept {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5f * ((2.0f * y1) +
                   (-y0 + y2) * t +
                   (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3) * t2 +
                   (-y0 + 3.0f * y1 - 3.0f * y2 + y3) * t3);
}

} // namespace

HRESULT WINAPI AcpHalProcessSpatialVoice(
    const INT16* srcMonoPcm,
    UINT32 srcFrames,
    UINT32 sampleRate,
    const ACP_SPATIAL_LISTENER* listener,
    const ACP_SPATIAL_EMITTER* emitter,
    ACP_SPATIAL_STATE* ioState,
    INT16* dstStereoPcm,
    UINT32 dstCapacityFrames,
    UINT32* outDstFrames) {
    if (outDstFrames) *outDstFrames = 0u;
    if (!srcMonoPcm || !listener || !emitter || !dstStereoPcm) return E_POINTER;
    if (sampleRate == 0u) return E_INVALIDARG;
    if (srcFrames == 0u || dstCapacityFrames == 0u) return S_OK;

    ACP_SPATIAL_STATE local_state{};
    ACP_SPATIAL_STATE& st = ioState ? *ioState : local_state;

    const ACP_VEC3 fwd = vec3_norm(listener->forward, {0.0f, 0.0f, 1.0f});
    const ACP_VEC3 up = vec3_norm(listener->up, {0.0f, 1.0f, 0.0f});
    // Right-handed basis: right = normalize(forward x up)
    const ACP_VEC3 right = vec3_norm(vec3_cross(fwd, up), {1.0f, 0.0f, 0.0f});

    const ACP_VEC3 delta = vec3_sub(emitter->position, listener->position);
    const float dist = std::sqrt((std::max)(0.0f, vec3_dot(delta, delta)));
    const ACP_VEC3 dir = (dist > 1e-5f)
        ? ACP_VEC3{delta.x / dist, delta.y / dist, delta.z / dist}
        : fwd;

    // Azimuth sine: -1.0 = hard left, 0.0 = center, +1.0 = hard right
    const float sin_az = std::clamp(vec3_dot(dir, right), -1.0f, 1.0f);

    // Inverse-distance rolloff attenuation
    const float min_d = (std::max)(0.1f, emitter->minDistance);
    const float max_d = (emitter->maxDistance > min_d) ? emitter->maxDistance : 10000.0f;
    const float rolloff = (std::max)(0.0f, emitter->rolloffFactor);
    const float clamped_d = std::clamp(dist, min_d, max_d);
    const float atten = min_d / (min_d + rolloff * (clamped_d - min_d));

    // Doppler pitch shift: radial velocities along listener -> emitter direction
    constexpr float kSpeedOfSound = 343.0f;
    const float doppler_scale = std::clamp(emitter->dopplerScale, 0.0f, 4.0f);
    const float v_listener = vec3_dot(listener->velocity, dir) * doppler_scale;
    const float v_emitter = vec3_dot(emitter->velocity, dir) * doppler_scale;
    const float num = (std::max)(34.3f, kSpeedOfSound + v_listener);
    const float den = (std::max)(34.3f, kSpeedOfSound + v_emitter);
    const float doppler_ratio = std::clamp(num / den, 0.5f, 2.0f);

    // Equal-power + ILD head-shadow gain per ear
    const float pan_angle = (sin_az * 0.5f + 0.5f) * 1.57079632679f; // 0 .. pi/2
    const float gain_l = atten * std::cos(pan_angle) * (1.0f - 0.25f * (std::max)(0.0f, sin_az));
    const float gain_r = atten * std::sin(pan_angle) * (1.0f - 0.25f * (std::max)(0.0f, -sin_az));

    // Woodworth ITD: up to 0.65 ms across human head (~31.2 samples at 48 kHz)
    const float max_itd_samples = (std::min)(48.0f, 0.00065f * static_cast<float>(sampleRate));
    const float delay_l = (sin_az > 0.0f) ? (sin_az * max_itd_samples) : 0.0f;
    const float delay_r = (sin_az < 0.0f) ? (-sin_az * max_itd_samples) : 0.0f;

    // One-pole low-pass coefficient for far-ear head-shadow occlusion (1.0 = bypassed, lower = muffled)
    const float lp_alpha_l = (sin_az > 0.0f) ? std::clamp(1.0f - 0.72f * sin_az, 0.22f, 1.0f) : 1.0f;
    const float lp_alpha_r = (sin_az < 0.0f) ? std::clamp(1.0f + 0.72f * sin_az, 0.22f, 1.0f) : 1.0f;

    const UINT32 est_out = static_cast<UINT32>(static_cast<double>(srcFrames) / static_cast<double>(doppler_ratio));
    const UINT32 out_frames = (std::min)(dstCapacityFrames, (std::max)(1u, est_out));

    double phase = st.dopplerPhase;
    if (phase < 0.0 || phase >= static_cast<double>(srcFrames)) phase = 0.0;

    for (UINT32 f = 0u; f < out_frames; ++f) {
        const double p = phase;
        const UINT32 idx0 = static_cast<UINT32>(p);
        const UINT32 idx1 = (idx0 + 1u < srcFrames) ? (idx0 + 1u) : (srcFrames - 1u);
        const float frac = static_cast<float>(p - static_cast<double>(idx0));
        const float s0 = (idx0 < srcFrames) ? static_cast<float>(srcMonoPcm[idx0]) : st.lastSample;
        const float s1 = (idx1 < srcFrames) ? static_cast<float>(srcMonoPcm[idx1]) : s0;
        const float mono = s0 + (s1 - s0) * frac;

        st.delayRing[st.delayWritePos & 63u] = mono;
        st.delayWritePos = (st.delayWritePos + 1u) & 63u;

        const float raw_l = sample_delay_ring(st.delayRing, st.delayWritePos, delay_l);
        const float raw_r = sample_delay_ring(st.delayRing, st.delayWritePos, delay_r);

        st.headShadowLpL += lp_alpha_l * (raw_l - st.headShadowLpL);
        st.headShadowLpR += lp_alpha_r * (raw_r - st.headShadowLpR);

        dstStereoPcm[static_cast<size_t>(f) * 2u + 0u] =
            clamp_pcm16(static_cast<int>(std::lround(st.headShadowLpL * gain_l)));
        dstStereoPcm[static_cast<size_t>(f) * 2u + 1u] =
            clamp_pcm16(static_cast<int>(std::lround(st.headShadowLpR * gain_r)));

        phase += static_cast<double>(doppler_ratio);
        if (phase >= static_cast<double>(srcFrames)) {
            if (outDstFrames) *outDstFrames = f + 1u;
            st.dopplerPhase = phase - static_cast<double>(srcFrames);
            st.lastSample = static_cast<float>(srcMonoPcm[srcFrames - 1u]);
            st.lastGain = atten;
            st.lastDopplerRatio = doppler_ratio;
            return S_OK;
        }
    }

    st.dopplerPhase = phase - static_cast<double>(srcFrames);
    if (st.dopplerPhase < 0.0) st.dopplerPhase = 0.0;
    st.lastSample = static_cast<float>(srcMonoPcm[srcFrames - 1u]);
    st.lastGain = atten;
    st.lastDopplerRatio = doppler_ratio;
    if (outDstFrames) *outDstFrames = out_frames;
    return S_OK;
}

HRESULT WINAPI AcpHalCreateReverb(
    const ACP_REVERB_PARAMS* params,
    ACP_REVERB_HANDLE* outHandle) {
    if (!outHandle) return E_POINTER;
    *outHandle = nullptr;

    ACP_REVERB_PARAMS cfg{};
    if (params) {
        cfg = *params;
    } else {
        cfg.roomSize = 0.75f;
        cfg.damping = 0.45f;
        cfg.wetMix = 0.35f;
        cfg.dryMix = 0.85f;
        cfg.stereoWidth = 1.0f;
        cfg.sampleRate = 48000u;
    }
    const float sr_scale = static_cast<float>(cfg.sampleRate ? cfg.sampleRate : 48000u) / 44100.0f;

    auto* rev = new (std::nothrow) WsxReverbInstance();
    if (!rev) return E_OUTOFMEMORY;

    constexpr size_t kCombBase[4] = {1116u, 1188u, 1277u, 1356u};
    constexpr size_t kApBase[2] = {556u, 441u};
    const size_t spread = static_cast<size_t>((std::max)(1.0f, 23.0f * sr_scale));
    for (int i = 0; i < 4; ++i) {
        const size_t d = static_cast<size_t>(static_cast<float>(kCombBase[i]) * sr_scale);
        rev->comb_l[i].init(d);
        rev->comb_r[i].init(d + spread);
    }
    for (int i = 0; i < 2; ++i) {
        const size_t d = static_cast<size_t>(static_cast<float>(kApBase[i]) * sr_scale);
        rev->ap_l[i].init(d);
        rev->ap_r[i].init(d + spread);
    }

    const float room = std::clamp(cfg.roomSize, 0.0f, 0.98f);
    const float damp = std::clamp(cfg.damping, 0.0f, 0.95f);
    const float wet = std::clamp(cfg.wetMix, 0.0f, 2.0f);
    const float width = std::clamp(cfg.stereoWidth, 0.0f, 1.0f);
    rev->feedback = 0.70f + room * 0.28f;
    rev->damp1 = damp * 0.4f;
    rev->damp2 = 1.0f - rev->damp1;
    rev->wet1 = wet * (width * 0.5f + 0.5f);
    rev->wet2 = wet * ((1.0f - width) * 0.5f);
    rev->dry = std::clamp(cfg.dryMix, 0.0f, 2.0f);

    *outHandle = static_cast<ACP_REVERB_HANDLE>(rev);
    return S_OK;
}

HRESULT WINAPI AcpHalApplyReverb(
    ACP_REVERB_HANDLE handle,
    INT16* ioStereoPcm,
    UINT32 frameCount) {
    if (!handle) return E_INVALIDARG;
    if (!ioStereoPcm && frameCount > 0u) return E_POINTER;
    if (frameCount == 0u) return S_OK;

    auto* rev = static_cast<WsxReverbInstance*>(handle);
    std::lock_guard<std::mutex> lk(rev->mutex);
    constexpr float kInGain = 0.015f;
    constexpr float kInv32768 = 1.0f / 32768.0f;

    for (UINT32 f = 0u; f < frameCount; ++f) {
        const float in_l = static_cast<float>(ioStereoPcm[static_cast<size_t>(f) * 2u + 0u]) * kInv32768;
        const float in_r = static_cast<float>(ioStereoPcm[static_cast<size_t>(f) * 2u + 1u]) * kInv32768;
        const float mono_in = (in_l + in_r) * kInGain;

        float acc_l = 0.0f;
        float acc_r = 0.0f;
        for (int i = 0; i < 4; ++i) {
            acc_l += rev->comb_l[i].process(mono_in, rev->feedback, rev->damp1, rev->damp2);
            acc_r += rev->comb_r[i].process(mono_in, rev->feedback, rev->damp1, rev->damp2);
        }
        for (int i = 0; i < 2; ++i) {
            acc_l = rev->ap_l[i].process(acc_l);
            acc_r = rev->ap_r[i].process(acc_r);
        }

        const float out_l = (acc_l * rev->wet1 + acc_r * rev->wet2) * 4.0f + in_l * rev->dry;
        const float out_r = (acc_r * rev->wet1 + acc_l * rev->wet2) * 4.0f + in_r * rev->dry;
        ioStereoPcm[static_cast<size_t>(f) * 2u + 0u] =
            clamp_pcm16(static_cast<int>(std::lround(out_l * 32767.0f)));
        ioStereoPcm[static_cast<size_t>(f) * 2u + 1u] =
            clamp_pcm16(static_cast<int>(std::lround(out_r * 32767.0f)));
    }
    return S_OK;
}

HRESULT WINAPI AcpHalDestroyReverb(ACP_REVERB_HANDLE handle) {
    if (!handle) return S_OK;
    delete static_cast<WsxReverbInstance*>(handle);
    return S_OK;
}

HRESULT WINAPI AcpHalResampleCubic(
    const INT16* srcPcm,
    UINT32 srcFrames,
    UINT32 srcSampleRate,
    UINT16 channels,
    INT16* dstPcm,
    UINT32 dstCapacityFrames,
    UINT32 dstSampleRate,
    UINT32* outDstFrames) {
    if (outDstFrames) *outDstFrames = 0u;
    if (!srcPcm || !dstPcm) return E_POINTER;
    if (srcSampleRate == 0u || dstSampleRate == 0u || channels == 0u || channels > 8u) {
        return E_INVALIDARG;
    }
    if (srcFrames == 0u || dstCapacityFrames == 0u) return S_OK;

    if (srcSampleRate == dstSampleRate) {
        const UINT32 copy_frames = (std::min)(srcFrames, dstCapacityFrames);
        std::memcpy(dstPcm, srcPcm, static_cast<size_t>(copy_frames) * channels * sizeof(INT16));
        if (outDstFrames) *outDstFrames = copy_frames;
        return S_OK;
    }

    const UINT64 scaled = (static_cast<UINT64>(srcFrames) * dstSampleRate + (srcSampleRate / 2u)) / srcSampleRate;
    const UINT32 out_frames = static_cast<UINT32>((std::min<UINT64>)(dstCapacityFrames, (std::max<UINT64>)(1u, scaled)));
    const double step = static_cast<double>(srcSampleRate) / static_cast<double>(dstSampleRate);

    auto fetch = [&](int idx, UINT16 ch) noexcept -> float {
        const int clamped = std::clamp(idx, 0, static_cast<int>(srcFrames) - 1);
        return static_cast<float>(srcPcm[static_cast<size_t>(clamped) * channels + ch]);
    };

    for (UINT32 f = 0u; f < out_frames; ++f) {
        const double pos = static_cast<double>(f) * step;
        const int i1 = static_cast<int>(pos);
        const float frac = static_cast<float>(pos - static_cast<double>(i1));
        for (UINT16 c = 0u; c < channels; ++c) {
            const float v = catmull_rom(
                fetch(i1 - 1, c),
                fetch(i1, c),
                fetch(i1 + 1, c),
                fetch(i1 + 2, c),
                frac);
            dstPcm[static_cast<size_t>(f) * channels + c] =
                clamp_pcm16(static_cast<int>(std::lround(v)));
        }
    }

    if (outDstFrames) *outDstFrames = out_frames;
    return S_OK;
}

HRESULT WINAPI AcpHalApplySoftLimiter(
    INT16* ioPcm,
    UINT32 frameCount,
    UINT16 channels,
    const ACP_LIMITER_PARAMS* params,
    ACP_LIMITER_STATE* ioState) {
    if (!ioPcm && frameCount > 0u) return E_POINTER;
    if (channels == 0u || channels > 8u) return E_INVALIDARG;
    if (frameCount == 0u) return S_OK;

    const float threshold = params ? std::clamp(params->thresholdLinear, 0.1f, 0.99f) : 0.80f;
    const float ceiling = params ? std::clamp(params->ceilingLinear, threshold, 1.0f) : 0.98f;
    const float sr = static_cast<float>((params && params->sampleRate > 0u) ? params->sampleRate : 48000u);
    const float release_ms = params ? std::clamp(params->releaseMs, 1.0f, 1000.0f) : 50.0f;
    const float rel_coeff = std::exp(-1.0f / (sr * release_ms * 0.001f));

    ACP_LIMITER_STATE local{};
    ACP_LIMITER_STATE& st = ioState ? *ioState : local;
    constexpr float kInv32768 = 1.0f / 32768.0f;
    float min_gain = 1.0f;

    for (UINT32 f = 0u; f < frameCount; ++f) {
        float peak = 0.0f;
        for (UINT16 c = 0u; c < channels; ++c) {
            const float v = std::fabs(static_cast<float>(ioPcm[static_cast<size_t>(f) * channels + c]) * kInv32768);
            if (v > peak) peak = v;
        }
        // Instant attack, smooth exponential release
        if (peak > st.envelope) {
            st.envelope = peak;
        } else {
            st.envelope = peak + rel_coeff * (st.envelope - peak);
        }

        float gain = 1.0f;
        if (st.envelope > threshold) {
            // Smooth hyperbolic soft-knee compression asymptotically capped at `ceiling`
            const float excess = st.envelope - threshold;
            const float headroom = (std::max)(1e-4f, ceiling - threshold);
            const float shaped = threshold + headroom * std::tanh(excess / headroom);
            gain = shaped / st.envelope;
        }
        if (gain < min_gain) min_gain = gain;

        for (UINT16 c = 0u; c < channels; ++c) {
            const float s = static_cast<float>(ioPcm[static_cast<size_t>(f) * channels + c]) * gain;
            ioPcm[static_cast<size_t>(f) * channels + c] =
                clamp_pcm16(static_cast<int>(std::lround(s)));
        }
    }

    st.gainReductionDb = (min_gain < 1.0f) ? (20.0f * std::log10((std::max)(1e-5f, min_gain))) : 0.0f;
    return S_OK;
}

HRESULT WINAPI AcpHalDownmixToStereo(
    const INT16* srcMultiPcm,
    UINT32 frameCount,
    UINT16 srcChannels,
    INT16* dstStereoPcm) {
    if (!srcMultiPcm || !dstStereoPcm) return E_POINTER;
    if (srcChannels == 0u || srcChannels > 8u) return E_INVALIDARG;
    if (frameCount == 0u) return S_OK;

    constexpr float kCenter = 0.70710678f;
    constexpr float kLfe = 0.50f;
    constexpr float kSurround = 0.70710678f;
    constexpr float kBack = 0.50f;

    for (UINT32 f = 0u; f < frameCount; ++f) {
        const INT16* s = srcMultiPcm + static_cast<size_t>(f) * srcChannels;
        float out_l = 0.0f;
        float out_r = 0.0f;
        if (srcChannels == 1u) {
            out_l = out_r = static_cast<float>(s[0]);
        } else if (srcChannels == 2u) {
            out_l = static_cast<float>(s[0]);
            out_r = static_cast<float>(s[1]);
        } else if (srcChannels == 6u) {
            // Standard 5.1 channel order: FL, FR, FC, LFE, BL/SL, BR/SR
            const float norm = 1.0f / (1.0f + kCenter + kSurround);
            out_l = (static_cast<float>(s[0]) + kCenter * static_cast<float>(s[2]) +
                     kLfe * static_cast<float>(s[3]) + kSurround * static_cast<float>(s[4])) * norm;
            out_r = (static_cast<float>(s[1]) + kCenter * static_cast<float>(s[2]) +
                     kLfe * static_cast<float>(s[3]) + kSurround * static_cast<float>(s[5])) * norm;
        } else if (srcChannels == 8u) {
            // Standard 7.1 channel order: FL, FR, FC, LFE, BL, BR, SL, SR
            const float norm = 1.0f / (1.0f + kCenter + kSurround + kBack);
            out_l = (static_cast<float>(s[0]) + kCenter * static_cast<float>(s[2]) +
                     kLfe * static_cast<float>(s[3]) + kBack * static_cast<float>(s[4]) +
                     kSurround * static_cast<float>(s[6])) * norm;
            out_r = (static_cast<float>(s[1]) + kCenter * static_cast<float>(s[2]) +
                     kLfe * static_cast<float>(s[3]) + kBack * static_cast<float>(s[5]) +
                     kSurround * static_cast<float>(s[7])) * norm;
        } else {
            // Generic N-channel folddown: even -> L, odd -> R
            const float inv = 2.0f / static_cast<float>(srcChannels);
            for (UINT16 c = 0u; c < srcChannels; ++c) {
                if ((c & 1u) == 0u) out_l += static_cast<float>(s[c]) * inv;
                else out_r += static_cast<float>(s[c]) * inv;
            }
        }
        dstStereoPcm[static_cast<size_t>(f) * 2u + 0u] = clamp_pcm16(static_cast<int>(std::lround(out_l)));
        dstStereoPcm[static_cast<size_t>(f) * 2u + 1u] = clamp_pcm16(static_cast<int>(std::lround(out_r)));
    }
    return S_OK;
}

HRESULT WINAPI AcpHalGetAudioOutputStats(
    ACP_AUDIO_OUTPUT_HANDLE handle,
    ACP_AUDIO_OUTPUT_STATS* outStats) {
    if (!outStats) return E_POINTER;
    std::memset(outStats, 0, sizeof(*outStats));
    if (!handle) return E_INVALIDARG;

    auto* stream = static_cast<WsxAudioOutputStream*>(handle);
    std::lock_guard<std::mutex> lk(stream->mutex);

    UINT32 padding = 0u;
    if (stream->is_wasapi && stream->audio_client) {
        stream->audio_client->GetCurrentPadding(&padding);
    }
    const UINT32 buf_frames = stream->is_wasapi ? stream->mix_buffer_frames : stream->buffer_frames;
    outStats->isWasapi = stream->is_wasapi ? TRUE : FALSE;
    outStats->sampleRate = stream->sample_rate;
    outStats->channels = stream->channels;
    outStats->bufferFrames = buf_frames;
    outStats->currentPaddingFrames = padding;
    outStats->totalFramesSubmitted = stream->total_frames_submitted;
    outStats->underrunCount = stream->underrun_count;
    outStats->estimatedLatencyMs = (stream->sample_rate > 0u)
        ? (static_cast<float>(stream->is_wasapi ? padding : buf_frames) * 1000.0f / static_cast<float>(stream->sample_rate))
        : 0.0f;
    return S_OK;
}

HRESULT WINAPI AcpHalFlushAsyncCommands(
    IAcpHal* pAcpHal,
    DWORD timeoutMs) {
    if (!pAcpHal) return E_POINTER;
    WsxAcpHal* impl = nullptr;
    if (FAILED(pAcpHal->QueryInterface(__uuidof(IWsxAcpHalInternal), reinterpret_cast<void**>(&impl))) || !impl) {
        return E_INVALIDARG;
    }
    return impl->flush_async(timeoutMs);
}

} // extern "C"
