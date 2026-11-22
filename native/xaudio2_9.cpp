// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox Series X|S XAudio2_9.dll Compatibility Shim:
// - Exports XAudio2Create at ordinal #1 (required by Unity/Unreal/Wwise/Bink2 titles),
//   CreateAudioVolumeMeter, CreateAudioReverb, CreateFX, and XAudio2CreateWithVersionInfo.
// - Bridges cleanly to Windows System32\XAudio2_9.dll (loaded with LOAD_LIBRARY_SEARCH_SYSTEM32
//   + self-module guard so it can never recurse into itself), sanitizing Xbox-only flags/processors.
// - Provides a full clean-room COM software fallback (IXAudio2 / Mastering / Source / Submix / XAPO)
//   for headless environments, missing audio endpoints, or WSX_XAUDIO2_FALLBACK=1.

#include "wsx_config.h"

#include <Windows.h>
#include <objbase.h>
#include <xaudio2.h>
#include <xaudio2fx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

namespace {

struct WsxXAudio2StatsData {
    uint32_t backend_mode;
    uint32_t force_fallback;
    uint64_t instances_created;
    uint64_t mastering_voices_created;
    uint64_t source_voices_created;
    uint64_t submix_voices_created;
    uint64_t buffers_submitted;
    uint64_t samples_processed;
    uint64_t xapo_effects_created;
    uint64_t fallback_mastering_rescues;
};

struct GlobalAudioState {
    std::mutex sys_mtx;
    bool sys_probed = false;
    HMODULE self_module = nullptr;
    HMODULE sys_module = nullptr;

    using FnXAudio2Create = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
    using FnCreateAudioVolumeMeter = HRESULT(WINAPI*)(IUnknown**);
    using FnCreateAudioReverb = HRESULT(WINAPI*)(IUnknown**);
    using FnCreateFX = HRESULT(WINAPI*)(REFCLSID, IUnknown**, const void*, UINT32);

    FnXAudio2Create fn_create = nullptr;
    FnCreateAudioVolumeMeter fn_vol_meter = nullptr;
    FnCreateAudioReverb fn_reverb = nullptr;
    FnCreateFX fn_create_fx = nullptr;

    std::atomic<uint32_t> backend_mode{0u};
    std::atomic<uint32_t> force_fallback{0u};
    std::atomic<uint64_t> instances_created{0u};
    std::atomic<uint64_t> mastering_voices_created{0u};
    std::atomic<uint64_t> source_voices_created{0u};
    std::atomic<uint64_t> submix_voices_created{0u};
    std::atomic<uint64_t> buffers_submitted{0u};
    std::atomic<uint64_t> samples_processed{0u};
    std::atomic<uint64_t> xapo_effects_created{0u};
    std::atomic<uint64_t> fallback_mastering_rescues{0u};
};

GlobalAudioState& g_state() noexcept {
    static GlobalAudioState st;
    return st;
}

bool is_fallback_forced() noexcept {
    if (g_state().force_fallback.load(std::memory_order_relaxed) != 0u) return true;
    return wsx::get_config_bool("WSX_XAUDIO2_FALLBACK", "Audio", "xaudio2_software_fallback", false);
}

void ensure_system_xaudio2_loaded() noexcept {
    auto& st = g_state();
    std::lock_guard<std::mutex> lk(st.sys_mtx);
    if (st.sys_probed) return;
    st.sys_probed = true;

    // Strictly load real OS XAudio2_9.dll from System32 so we never recurse into MizuBox\bin\XAudio2_9.dll
    HMODULE mod = LoadLibraryExW(L"XAudio2_9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mod) {
        wchar_t sys_path[MAX_PATH]{};
        const UINT n = GetSystemDirectoryW(sys_path, MAX_PATH);
        if (n > 0 && n + 16 < MAX_PATH) {
            wcscat_s(sys_path, MAX_PATH, L"\\XAudio2_9.dll");
            mod = LoadLibraryW(sys_path);
        }
    }
    if (mod && mod != st.self_module) {
        st.sys_module = mod;
        st.fn_create = reinterpret_cast<GlobalAudioState::FnXAudio2Create>(GetProcAddress(mod, "XAudio2Create"));
        st.fn_vol_meter = reinterpret_cast<GlobalAudioState::FnCreateAudioVolumeMeter>(GetProcAddress(mod, "CreateAudioVolumeMeter"));
        st.fn_reverb = reinterpret_cast<GlobalAudioState::FnCreateAudioReverb>(GetProcAddress(mod, "CreateAudioReverb"));
        st.fn_create_fx = reinterpret_cast<GlobalAudioState::FnCreateFX>(GetProcAddress(mod, "CreateFX"));
    } else if (mod == st.self_module) {
        FreeLibrary(mod);
    }
}

// ============================================================================
// Clean-room Minimal XAPO (VolumeMeter / Reverb / CreateFX fallback)
// ============================================================================
class WsxSoftXapo final : public IUnknown {
    std::atomic<ULONG> ref_{1};
    CLSID clsid_{};
public:
    explicit WsxSoftXapo(REFCLSID id) noexcept : clsid_(id) {
        g_state().xapo_effects_created.fetch_add(1, std::memory_order_relaxed);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return n;
    }
};

// ============================================================================
// Clean-room Software IXAudio2 Voices (Mastering, Submix, Source)
// ============================================================================
struct SoftVoiceCommon {
    UINT32 creation_flags = 0;
    UINT32 input_channels = 2;
    UINT32 input_sample_rate = 48000;
    UINT32 processing_stage = 0;
    float volume = 1.0f;
    std::vector<float> channel_volumes;
    XAUDIO2_FILTER_PARAMETERS filter{LowPassFilter, 1.0f, 1.0f};
    std::mutex mtx;

    SoftVoiceCommon(UINT32 flags, UINT32 ch, UINT32 sr, UINT32 stage)
        : creation_flags(flags),
          input_channels(std::max<UINT32>(1u, ch)),
          input_sample_rate(std::max<UINT32>(1000u, sr)),
          processing_stage(stage),
          channel_volumes(std::max<UINT32>(1u, ch), 1.0f) {}
};

class WsxSoftMasteringVoice final : public IXAudio2MasteringVoice {
    SoftVoiceCommon c_;
    DWORD channel_mask_ = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
public:
    WsxSoftMasteringVoice(UINT32 ch, UINT32 sr)
        : c_(0u, ch ? ch : 2u, sr ? sr : 48000u, 0u) {
        if (c_.input_channels == 6u) {
            channel_mask_ = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                            SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
        } else if (c_.input_channels == 8u) {
            channel_mask_ = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                            SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |
                            SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
        }
    }

    void STDMETHODCALLTYPE GetVoiceDetails(XAUDIO2_VOICE_DETAILS* pVoiceDetails) override {
        if (!pVoiceDetails) return;
        pVoiceDetails->CreationFlags = c_.creation_flags;
        pVoiceDetails->ActiveFlags = c_.creation_flags;
        pVoiceDetails->InputChannels = c_.input_channels;
        pVoiceDetails->InputSampleRate = c_.input_sample_rate;
    }
    HRESULT STDMETHODCALLTYPE SetOutputVoices(const XAUDIO2_VOICE_SENDS*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetEffectChain(const XAUDIO2_EFFECT_CHAIN*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EnableEffect(UINT32, UINT32) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DisableEffect(UINT32, UINT32) override { return S_OK; }
    void STDMETHODCALLTYPE GetEffectState(UINT32, BOOL* pEnabled) override { if (pEnabled) *pEnabled = FALSE; }
    HRESULT STDMETHODCALLTYPE SetEffectParameters(UINT32, const void*, UINT32, UINT32) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetEffectParameters(UINT32, void* pParams, UINT32 sz) override {
        if (pParams && sz) std::memset(pParams, 0, sz);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFilterParameters(const XAUDIO2_FILTER_PARAMETERS* p, UINT32) override {
        if (p) { std::lock_guard<std::mutex> lk(c_.mtx); c_.filter = *p; }
        return S_OK;
    }
    void STDMETHODCALLTYPE GetFilterParameters(XAUDIO2_FILTER_PARAMETERS* p) override {
        if (p) { std::lock_guard<std::mutex> lk(c_.mtx); *p = c_.filter; }
    }
    HRESULT STDMETHODCALLTYPE SetOutputFilterParameters(IXAudio2Voice*, const XAUDIO2_FILTER_PARAMETERS*, UINT32) override { return S_OK; }
    void STDMETHODCALLTYPE GetOutputFilterParameters(IXAudio2Voice*, XAUDIO2_FILTER_PARAMETERS* p) override {
        if (p) *p = {LowPassFilter, 1.0f, 1.0f};
    }
    HRESULT STDMETHODCALLTYPE SetVolume(float Volume, UINT32) override {
        std::lock_guard<std::mutex> lk(c_.mtx);
        c_.volume = Volume;
        return S_OK;
    }
    void STDMETHODCALLTYPE GetVolume(float* pVolume) override {
        if (pVolume) { std::lock_guard<std::mutex> lk(c_.mtx); *pVolume = c_.volume; }
    }
    HRESULT STDMETHODCALLTYPE SetChannelVolumes(UINT32 Channels, const float* pVolumes, UINT32) override {
        if (!pVolumes) return E_INVALIDARG;
        std::lock_guard<std::mutex> lk(c_.mtx);
        const UINT32 n = std::min<UINT32>(Channels, static_cast<UINT32>(c_.channel_volumes.size()));
        for (UINT32 i = 0; i < n; ++i) c_.channel_volumes[i] = pVolumes[i];
        return S_OK;
    }
    void STDMETHODCALLTYPE GetChannelVolumes(UINT32 Channels, float* pVolumes) override {
        if (!pVolumes) return;
        std::lock_guard<std::mutex> lk(c_.mtx);
        const UINT32 n = std::min<UINT32>(Channels, static_cast<UINT32>(c_.channel_volumes.size()));
        for (UINT32 i = 0; i < n; ++i) pVolumes[i] = c_.channel_volumes[i];
    }
    HRESULT STDMETHODCALLTYPE SetOutputMatrix(IXAudio2Voice*, UINT32, UINT32, const float*, UINT32) override { return S_OK; }
    void STDMETHODCALLTYPE GetOutputMatrix(IXAudio2Voice*, UINT32 srcCh, UINT32 dstCh, float* pMatrix) override {
        if (pMatrix) std::memset(pMatrix, 0, static_cast<size_t>(srcCh) * dstCh * sizeof(float));
    }
    void STDMETHODCALLTYPE DestroyVoice() override { delete this; }
    HRESULT STDMETHODCALLTYPE GetChannelMask(DWORD* pChannelmask) override {
        if (!pChannelmask) return E_INVALIDARG;
        *pChannelmask = channel_mask_;
        return S_OK;
    }
};

class WsxSoftSubmixVoice final : public IXAudio2SubmixVoice {
    SoftVoiceCommon c_;
public:
    WsxSoftSubmixVoice(UINT32 ch, UINT32 sr, UINT32 flags, UINT32 stage)
        : c_(flags, ch, sr, stage) {}

    void STDMETHODCALLTYPE GetVoiceDetails(XAUDIO2_VOICE_DETAILS* pVoiceDetails) override {
        if (!pVoiceDetails) return;
        pVoiceDetails->CreationFlags = c_.creation_flags;
        pVoiceDetails->ActiveFlags = c_.creation_flags;
        pVoiceDetails->InputChannels = c_.input_channels;
        pVoiceDetails->InputSampleRate = c_.input_sample_rate;
    }
    HRESULT STDMETHODCALLTYPE SetOutputVoices(const XAUDIO2_VOICE_SENDS*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetEffectChain(const XAUDIO2_EFFECT_CHAIN*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EnableEffect(UINT32, UINT32) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DisableEffect(UINT32, UINT32) override { return S_OK; }
    void STDMETHODCALLTYPE GetEffectState(UINT32, BOOL* pEnabled) override { if (pEnabled) *pEnabled = FALSE; }
    HRESULT STDMETHODCALLTYPE SetEffectParameters(UINT32, const void*, UINT32, UINT32) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetEffectParameters(UINT32, void* pParams, UINT32 sz) override {
        if (pParams && sz) std::memset(pParams, 0, sz);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFilterParameters(const XAUDIO2_FILTER_PARAMETERS* p, UINT32) override {
        if (p) { std::lock_guard<std::mutex> lk(c_.mtx); c_.filter = *p; }
        return S_OK;
    }
    void STDMETHODCALLTYPE GetFilterParameters(XAUDIO2_FILTER_PARAMETERS* p) override {
        if (p) { std::lock_guard<std::mutex> lk(c_.mtx); *p = c_.filter; }
    }
    HRESULT STDMETHODCALLTYPE SetOutputFilterParameters(IXAudio2Voice*, const XAUDIO2_FILTER_PARAMETERS*, UINT32) override { return S_OK; }
    void STDMETHODCALLTYPE GetOutputFilterParameters(IXAudio2Voice*, XAUDIO2_FILTER_PARAMETERS* p) override {
        if (p) *p = {LowPassFilter, 1.0f, 1.0f};
    }
    HRESULT STDMETHODCALLTYPE SetVolume(float Volume, UINT32) override {
        std::lock_guard<std::mutex> lk(c_.mtx);
        c_.volume = Volume;
        return S_OK;
    }
    void STDMETHODCALLTYPE GetVolume(float* pVolume) override {
        if (pVolume) { std::lock_guard<std::mutex> lk(c_.mtx); *pVolume = c_.volume; }
    }
    HRESULT STDMETHODCALLTYPE SetChannelVolumes(UINT32 Channels, const float* pVolumes, UINT32) override {
        if (!pVolumes) return E_INVALIDARG;
        std::lock_guard<std::mutex> lk(c_.mtx);
        const UINT32 n = std::min<UINT32>(Channels, static_cast<UINT32>(c_.channel_volumes.size()));
        for (UINT32 i = 0; i < n; ++i) c_.channel_volumes[i] = pVolumes[i];
        return S_OK;
    }
    void STDMETHODCALLTYPE GetChannelVolumes(UINT32 Channels, float* pVolumes) override {
        if (!pVolumes) return;
        std::lock_guard<std::mutex> lk(c_.mtx);
        const UINT32 n = std::min<UINT32>(Channels, static_cast<UINT32>(c_.channel_volumes.size()));
        for (UINT32 i = 0; i < n; ++i) pVolumes[i] = c_.channel_volumes[i];
    }
    HRESULT STDMETHODCALLTYPE SetOutputMatrix(IXAudio2Voice*, UINT32, UINT32, const float*, UINT32) override { return S_OK; }
    void STDMETHODCALLTYPE GetOutputMatrix(IXAudio2Voice*, UINT32 srcCh, UINT32 dstCh, float* pMatrix) override {
        if (pMatrix) std::memset(pMatrix, 0, static_cast<size_t>(srcCh) * dstCh * sizeof(float));
    }
    void STDMETHODCALLTYPE DestroyVoice() override { delete this; }
};

class WsxSoftSourceVoice final : public IXAudio2SourceVoice {
    SoftVoiceCommon c_;
    UINT32 block_align_ = 4u;
    float freq_ratio_ = 1.0f;
    float max_freq_ratio_ = 2.0f;
    bool playing_ = false;
    IXAudio2VoiceCallback* cb_ = nullptr;
    UINT64 samples_played_ = 0u;
    struct QueuedBuf {
        void* ctx = nullptr;
        UINT32 samples = 0;
        UINT32 flags = 0;
    };
    std::vector<QueuedBuf> queue_;

    void drain_one_locked() {
        if (queue_.empty()) return;
        const QueuedBuf b = queue_.front();
        queue_.erase(queue_.begin());
        samples_played_ += b.samples;
        g_state().samples_processed.fetch_add(b.samples, std::memory_order_relaxed);
        if (cb_) {
            cb_->OnBufferStart(b.ctx);
            cb_->OnBufferEnd(b.ctx);
            if ((b.flags & XAUDIO2_END_OF_STREAM) != 0u) {
                cb_->OnStreamEnd();
            }
        }
    }

public:
    WsxSoftSourceVoice(const WAVEFORMATEX* fmt, UINT32 flags, float max_ratio, IXAudio2VoiceCallback* cb)
        : c_(flags,
             fmt ? fmt->nChannels : 2u,
             fmt ? fmt->nSamplesPerSec : 48000u,
             0u),
          block_align_(fmt && fmt->nBlockAlign ? fmt->nBlockAlign : 4u),
          max_freq_ratio_(std::clamp(max_ratio, 1.0f, 1024.0f)),
          cb_(cb) {}

    void STDMETHODCALLTYPE GetVoiceDetails(XAUDIO2_VOICE_DETAILS* pVoiceDetails) override {
        if (!pVoiceDetails) return;
        pVoiceDetails->CreationFlags = c_.creation_flags;
        pVoiceDetails->ActiveFlags = c_.creation_flags;
        pVoiceDetails->InputChannels = c_.input_channels;
        pVoiceDetails->InputSampleRate = c_.input_sample_rate;
    }
    HRESULT STDMETHODCALLTYPE SetOutputVoices(const XAUDIO2_VOICE_SENDS*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetEffectChain(const XAUDIO2_EFFECT_CHAIN*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EnableEffect(UINT32, UINT32) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DisableEffect(UINT32, UINT32) override { return S_OK; }
    void STDMETHODCALLTYPE GetEffectState(UINT32, BOOL* pEnabled) override { if (pEnabled) *pEnabled = FALSE; }
    HRESULT STDMETHODCALLTYPE SetEffectParameters(UINT32, const void*, UINT32, UINT32) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetEffectParameters(UINT32, void* pParams, UINT32 sz) override {
        if (pParams && sz) std::memset(pParams, 0, sz);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFilterParameters(const XAUDIO2_FILTER_PARAMETERS* p, UINT32) override {
        if (p) { std::lock_guard<std::mutex> lk(c_.mtx); c_.filter = *p; }
        return S_OK;
    }
    void STDMETHODCALLTYPE GetFilterParameters(XAUDIO2_FILTER_PARAMETERS* p) override {
        if (p) { std::lock_guard<std::mutex> lk(c_.mtx); *p = c_.filter; }
    }
    HRESULT STDMETHODCALLTYPE SetOutputFilterParameters(IXAudio2Voice*, const XAUDIO2_FILTER_PARAMETERS*, UINT32) override { return S_OK; }
    void STDMETHODCALLTYPE GetOutputFilterParameters(IXAudio2Voice*, XAUDIO2_FILTER_PARAMETERS* p) override {
        if (p) *p = {LowPassFilter, 1.0f, 1.0f};
    }
    HRESULT STDMETHODCALLTYPE SetVolume(float Volume, UINT32) override {
        std::lock_guard<std::mutex> lk(c_.mtx);
        c_.volume = Volume;
        return S_OK;
    }
    void STDMETHODCALLTYPE GetVolume(float* pVolume) override {
        if (pVolume) { std::lock_guard<std::mutex> lk(c_.mtx); *pVolume = c_.volume; }
    }
    HRESULT STDMETHODCALLTYPE SetChannelVolumes(UINT32 Channels, const float* pVolumes, UINT32) override {
        if (!pVolumes) return E_INVALIDARG;
        std::lock_guard<std::mutex> lk(c_.mtx);
        const UINT32 n = std::min<UINT32>(Channels, static_cast<UINT32>(c_.channel_volumes.size()));
        for (UINT32 i = 0; i < n; ++i) c_.channel_volumes[i] = pVolumes[i];
        return S_OK;
    }
    void STDMETHODCALLTYPE GetChannelVolumes(UINT32 Channels, float* pVolumes) override {
        if (!pVolumes) return;
        std::lock_guard<std::mutex> lk(c_.mtx);
        const UINT32 n = std::min<UINT32>(Channels, static_cast<UINT32>(c_.channel_volumes.size()));
        for (UINT32 i = 0; i < n; ++i) pVolumes[i] = c_.channel_volumes[i];
    }
    HRESULT STDMETHODCALLTYPE SetOutputMatrix(IXAudio2Voice*, UINT32, UINT32, const float*, UINT32) override { return S_OK; }
    void STDMETHODCALLTYPE GetOutputMatrix(IXAudio2Voice*, UINT32 srcCh, UINT32 dstCh, float* pMatrix) override {
        if (pMatrix) std::memset(pMatrix, 0, static_cast<size_t>(srcCh) * dstCh * sizeof(float));
    }
    void STDMETHODCALLTYPE DestroyVoice() override { delete this; }

    HRESULT STDMETHODCALLTYPE Start(UINT32, UINT32) override {
        std::lock_guard<std::mutex> lk(c_.mtx);
        playing_ = true;
        while (!queue_.empty()) drain_one_locked();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop(UINT32, UINT32) override {
        std::lock_guard<std::mutex> lk(c_.mtx);
        playing_ = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SubmitSourceBuffer(const XAUDIO2_BUFFER* pBuffer, const XAUDIO2_BUFFER_WMA*) override {
        if (!pBuffer) return E_INVALIDARG;
        g_state().buffers_submitted.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(c_.mtx);
        const UINT32 nsamples = (pBuffer->PlayLength > 0u)
            ? pBuffer->PlayLength
            : (block_align_ > 0u ? (pBuffer->AudioBytes / block_align_) : 0u);
        queue_.push_back(QueuedBuf{pBuffer->pContext, nsamples, pBuffer->Flags});
        if (playing_) {
            drain_one_locked();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FlushSourceBuffers() override {
        std::lock_guard<std::mutex> lk(c_.mtx);
        while (!queue_.empty()) {
            void* ctx = queue_.front().ctx;
            queue_.erase(queue_.begin());
            if (cb_) cb_->OnBufferEnd(ctx);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Discontinuity() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ExitLoop(UINT32) override { return S_OK; }
    void STDMETHODCALLTYPE GetState(XAUDIO2_VOICE_STATE* pVoiceState, UINT32 Flags) override {
        if (!pVoiceState) return;
        std::lock_guard<std::mutex> lk(c_.mtx);
        pVoiceState->pCurrentBufferContext = queue_.empty() ? nullptr : queue_.front().ctx;
        pVoiceState->BuffersQueued = static_cast<UINT32>(queue_.size());
        pVoiceState->SamplesPlayed = ((Flags & XAUDIO2_VOICE_NOSAMPLESPLAYED) != 0u) ? 0u : samples_played_;
    }
    HRESULT STDMETHODCALLTYPE SetFrequencyRatio(float Ratio, UINT32) override {
        if (Ratio < XAUDIO2_MIN_FREQ_RATIO || Ratio > max_freq_ratio_) return E_INVALIDARG;
        std::lock_guard<std::mutex> lk(c_.mtx);
        freq_ratio_ = Ratio;
        return S_OK;
    }
    void STDMETHODCALLTYPE GetFrequencyRatio(float* pRatio) override {
        if (pRatio) { std::lock_guard<std::mutex> lk(c_.mtx); *pRatio = freq_ratio_; }
    }
    HRESULT STDMETHODCALLTYPE SetSourceSampleRate(UINT32 NewSourceSampleRate) override {
        if (NewSourceSampleRate < XAUDIO2_MIN_SAMPLE_RATE || NewSourceSampleRate > XAUDIO2_MAX_SAMPLE_RATE) {
            return E_INVALIDARG;
        }
        std::lock_guard<std::mutex> lk(c_.mtx);
        c_.input_sample_rate = NewSourceSampleRate;
        return S_OK;
    }
};

// ============================================================================
// Clean-room Software IXAudio2 Engine (also used as headless rescue)
// ============================================================================
class WsxSoftXAudio2 final : public IXAudio2 {
    std::atomic<ULONG> ref_{1};
    bool running_ = true;
public:
    WsxSoftXAudio2() noexcept {
        g_state().backend_mode.store(2u, std::memory_order_relaxed);
        g_state().instances_created.fetch_add(1, std::memory_order_relaxed);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IXAudio2)) {
            *ppv = static_cast<IXAudio2*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE RegisterForCallbacks(IXAudio2EngineCallback*) override { return S_OK; }
    void STDMETHODCALLTYPE UnregisterForCallbacks(IXAudio2EngineCallback*) override {}
    HRESULT STDMETHODCALLTYPE CreateSourceVoice(
        IXAudio2SourceVoice** ppSourceVoice,
        const WAVEFORMATEX* pSourceFormat,
        UINT32 Flags,
        float MaxFrequencyRatio,
        IXAudio2VoiceCallback* pCallback,
        const XAUDIO2_VOICE_SENDS*,
        const XAUDIO2_EFFECT_CHAIN*) override {
        if (!ppSourceVoice || !pSourceFormat) return E_INVALIDARG;
        auto* v = new (std::nothrow) WsxSoftSourceVoice(pSourceFormat, Flags, MaxFrequencyRatio, pCallback);
        if (!v) return E_OUTOFMEMORY;
        *ppSourceVoice = v;
        g_state().source_voices_created.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateSubmixVoice(
        IXAudio2SubmixVoice** ppSubmixVoice,
        UINT32 InputChannels,
        UINT32 InputSampleRate,
        UINT32 Flags,
        UINT32 ProcessingStage,
        const XAUDIO2_VOICE_SENDS*,
        const XAUDIO2_EFFECT_CHAIN*) override {
        if (!ppSubmixVoice) return E_INVALIDARG;
        auto* v = new (std::nothrow) WsxSoftSubmixVoice(InputChannels, InputSampleRate, Flags, ProcessingStage);
        if (!v) return E_OUTOFMEMORY;
        *ppSubmixVoice = v;
        g_state().submix_voices_created.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateMasteringVoice(
        IXAudio2MasteringVoice** ppMasteringVoice,
        UINT32 InputChannels,
        UINT32 InputSampleRate,
        UINT32,
        LPCWSTR,
        const XAUDIO2_EFFECT_CHAIN*,
        AUDIO_STREAM_CATEGORY) override {
        if (!ppMasteringVoice) return E_INVALIDARG;
        auto* v = new (std::nothrow) WsxSoftMasteringVoice(InputChannels, InputSampleRate);
        if (!v) return E_OUTOFMEMORY;
        *ppMasteringVoice = v;
        g_state().mastering_voices_created.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE StartEngine() override { running_ = true; return S_OK; }
    void STDMETHODCALLTYPE StopEngine() override { running_ = false; }
    HRESULT STDMETHODCALLTYPE CommitChanges(UINT32) override { return S_OK; }
    void STDMETHODCALLTYPE GetPerformanceData(XAUDIO2_PERFORMANCE_DATA* pPerfData) override {
        if (!pPerfData) return;
        std::memset(pPerfData, 0, sizeof(*pPerfData));
        pPerfData->ActiveSourceVoiceCount = static_cast<UINT32>(g_state().source_voices_created.load(std::memory_order_relaxed));
        pPerfData->TotalSourceVoiceCount = pPerfData->ActiveSourceVoiceCount;
        pPerfData->ActiveSubmixVoiceCount = static_cast<UINT32>(g_state().submix_voices_created.load(std::memory_order_relaxed));
        pPerfData->ActiveMatrixMixCount = 1u;
        pPerfData->MemoryUsageInBytes = 65536u;
    }
    void STDMETHODCALLTYPE SetDebugConfiguration(const XAUDIO2_DEBUG_CONFIGURATION*, void*) override {}
};

// ============================================================================
// System32 XAudio2_9 Bridge Proxy (sanitizes Xbox params & rescues headless endpoints)
// ============================================================================
class WsxHostXAudio2Proxy final : public IXAudio2 {
    std::atomic<ULONG> ref_{1};
    IXAudio2* real_ = nullptr;
    bool using_headless_rescue_ = false;
public:
    explicit WsxHostXAudio2Proxy(IXAudio2* real_eng) noexcept : real_(real_eng) {
        g_state().backend_mode.store(1u, std::memory_order_relaxed);
        g_state().instances_created.fetch_add(1, std::memory_order_relaxed);
    }
    ~WsxHostXAudio2Proxy() {
        if (real_) real_->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IXAudio2)) {
            *ppv = static_cast<IXAudio2*>(this);
            AddRef();
            return S_OK;
        }
        return real_ ? real_->QueryInterface(riid, ppv) : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE RegisterForCallbacks(IXAudio2EngineCallback* cb) override {
        return real_->RegisterForCallbacks(cb);
    }
    void STDMETHODCALLTYPE UnregisterForCallbacks(IXAudio2EngineCallback* cb) override {
        real_->UnregisterForCallbacks(cb);
    }
    HRESULT STDMETHODCALLTYPE CreateSourceVoice(
        IXAudio2SourceVoice** ppSourceVoice,
        const WAVEFORMATEX* pSourceFormat,
        UINT32 Flags,
        float MaxFrequencyRatio,
        IXAudio2VoiceCallback* pCallback,
        const XAUDIO2_VOICE_SENDS* pSendList,
        const XAUDIO2_EFFECT_CHAIN* pEffectChain) override {
        if (using_headless_rescue_) {
            if (!ppSourceVoice || !pSourceFormat) return E_INVALIDARG;
            *ppSourceVoice = new (std::nothrow) WsxSoftSourceVoice(pSourceFormat, Flags, MaxFrequencyRatio, pCallback);
            if (!*ppSourceVoice) return E_OUTOFMEMORY;
            g_state().source_voices_created.fetch_add(1, std::memory_order_relaxed);
            return S_OK;
        }
        const HRESULT hr = real_->CreateSourceVoice(
            ppSourceVoice, pSourceFormat, Flags, MaxFrequencyRatio, pCallback, pSendList, pEffectChain);
        if (SUCCEEDED(hr)) g_state().source_voices_created.fetch_add(1, std::memory_order_relaxed);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSubmixVoice(
        IXAudio2SubmixVoice** ppSubmixVoice,
        UINT32 InputChannels,
        UINT32 InputSampleRate,
        UINT32 Flags,
        UINT32 ProcessingStage,
        const XAUDIO2_VOICE_SENDS* pSendList,
        const XAUDIO2_EFFECT_CHAIN* pEffectChain) override {
        if (using_headless_rescue_) {
            if (!ppSubmixVoice) return E_INVALIDARG;
            *ppSubmixVoice = new (std::nothrow) WsxSoftSubmixVoice(InputChannels, InputSampleRate, Flags, ProcessingStage);
            if (!*ppSubmixVoice) return E_OUTOFMEMORY;
            g_state().submix_voices_created.fetch_add(1, std::memory_order_relaxed);
            return S_OK;
        }
        const HRESULT hr = real_->CreateSubmixVoice(
            ppSubmixVoice, InputChannels, InputSampleRate, Flags, ProcessingStage, pSendList, pEffectChain);
        if (SUCCEEDED(hr)) g_state().submix_voices_created.fetch_add(1, std::memory_order_relaxed);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateMasteringVoice(
        IXAudio2MasteringVoice** ppMasteringVoice,
        UINT32 InputChannels,
        UINT32 InputSampleRate,
        UINT32 Flags,
        LPCWSTR szDeviceId,
        const XAUDIO2_EFFECT_CHAIN* pEffectChain,
        AUDIO_STREAM_CATEGORY StreamCategory) override {
        HRESULT hr = real_->CreateMasteringVoice(
            ppMasteringVoice, InputChannels, InputSampleRate, Flags, szDeviceId, pEffectChain, StreamCategory);
        if (FAILED(hr) && szDeviceId != nullptr) {
            // Retry with default Windows endpoint if Xbox passed a console-specific endpoint ID
            hr = real_->CreateMasteringVoice(
                ppMasteringVoice, InputChannels, InputSampleRate, 0u, nullptr, pEffectChain, AudioCategory_GameEffects);
        }
        if (FAILED(hr) && ppMasteringVoice) {
            // Headless / no-speaker rescue: hand out clean-room software mastering voice so title never crashes
            auto* soft = new (std::nothrow) WsxSoftMasteringVoice(InputChannels, InputSampleRate);
            if (soft) {
                *ppMasteringVoice = soft;
                using_headless_rescue_ = true;
                g_state().fallback_mastering_rescues.fetch_add(1, std::memory_order_relaxed);
                g_state().mastering_voices_created.fetch_add(1, std::memory_order_relaxed);
                return S_OK;
            }
        }
        if (SUCCEEDED(hr)) g_state().mastering_voices_created.fetch_add(1, std::memory_order_relaxed);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE StartEngine() override { return using_headless_rescue_ ? S_OK : real_->StartEngine(); }
    void STDMETHODCALLTYPE StopEngine() override { if (!using_headless_rescue_) real_->StopEngine(); }
    HRESULT STDMETHODCALLTYPE CommitChanges(UINT32 OperationSet) override {
        return using_headless_rescue_ ? S_OK : real_->CommitChanges(OperationSet);
    }
    void STDMETHODCALLTYPE GetPerformanceData(XAUDIO2_PERFORMANCE_DATA* pPerfData) override {
        if (!pPerfData) return;
        if (using_headless_rescue_) {
            std::memset(pPerfData, 0, sizeof(*pPerfData));
            return;
        }
        real_->GetPerformanceData(pPerfData);
    }
    void STDMETHODCALLTYPE SetDebugConfiguration(const XAUDIO2_DEBUG_CONFIGURATION* pDebugConfig, void* pReserved) override {
        if (!using_headless_rescue_) real_->SetDebugConfiguration(pDebugConfig, pReserved);
    }
};

} // namespace

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        g_state().self_module = hinstDLL;
    }
    return TRUE;
}

extern "C" {

HRESULT WINAPI WsxExport_XAudio2Create(
    IXAudio2** ppXAudio2,
    UINT32 Flags,
    XAUDIO2_PROCESSOR XAudio2Processor) {
    if (!ppXAudio2) return E_INVALIDARG;
    *ppXAudio2 = nullptr;

    if (!is_fallback_forced()) {
        ensure_system_xaudio2_loaded();
        auto& st = g_state();
        if (st.fn_create) {
            const UINT32 clean_flags = Flags & ~(XAUDIO2_DEBUG_ENGINE | 0xFFFF0000u);
            const XAUDIO2_PROCESSOR clean_proc = (XAudio2Processor == 0u) ? XAUDIO2_DEFAULT_PROCESSOR : XAudio2Processor;
            IXAudio2* real_eng = nullptr;
            HRESULT hr = st.fn_create(&real_eng, clean_flags, clean_proc);
            if (FAILED(hr) && clean_proc != XAUDIO2_DEFAULT_PROCESSOR) {
                hr = st.fn_create(&real_eng, 0u, XAUDIO2_DEFAULT_PROCESSOR);
            }
            if (SUCCEEDED(hr) && real_eng) {
                auto* proxy = new (std::nothrow) WsxHostXAudio2Proxy(real_eng);
                if (!proxy) {
                    real_eng->Release();
                    return E_OUTOFMEMORY;
                }
                *ppXAudio2 = proxy;
                wsx::trace_hr("XAudio2_9", "XAudio2Create", S_OK, "backend=host_sys32 flags=0x%X proc=%u",
                              clean_flags, static_cast<unsigned>(clean_proc));
                return S_OK;
            }
        }
    }

    auto* soft = new (std::nothrow) WsxSoftXAudio2();
    if (!soft) return E_OUTOFMEMORY;
    *ppXAudio2 = soft;
    wsx::trace_hr("XAudio2_9", "XAudio2Create", S_OK, "backend=software_fallback flags=0x%X", Flags);
    return S_OK;
}

HRESULT WINAPI WsxExport_XAudio2CreateWithVersionInfo(
    IXAudio2** ppXAudio2,
    UINT32 Flags,
    XAUDIO2_PROCESSOR XAudio2Processor,
    UINT32 /*ntddiVersion*/) {
    return WsxExport_XAudio2Create(ppXAudio2, Flags, XAudio2Processor);
}

HRESULT WINAPI WsxExport_CreateAudioVolumeMeter(IUnknown** ppApo) {
    if (!ppApo) return E_INVALIDARG;
    *ppApo = nullptr;
    if (!is_fallback_forced()) {
        ensure_system_xaudio2_loaded();
        if (g_state().fn_vol_meter) {
            const HRESULT hr = g_state().fn_vol_meter(ppApo);
            if (SUCCEEDED(hr) && *ppApo) {
                g_state().xapo_effects_created.fetch_add(1, std::memory_order_relaxed);
                return hr;
            }
        }
    }
    *ppApo = new (std::nothrow) WsxSoftXapo(__uuidof(AudioVolumeMeter));
    return *ppApo ? S_OK : E_OUTOFMEMORY;
}

HRESULT WINAPI WsxExport_CreateAudioReverb(IUnknown** ppApo) {
    if (!ppApo) return E_INVALIDARG;
    *ppApo = nullptr;
    if (!is_fallback_forced()) {
        ensure_system_xaudio2_loaded();
        if (g_state().fn_reverb) {
            const HRESULT hr = g_state().fn_reverb(ppApo);
            if (SUCCEEDED(hr) && *ppApo) {
                g_state().xapo_effects_created.fetch_add(1, std::memory_order_relaxed);
                return hr;
            }
        }
    }
    *ppApo = new (std::nothrow) WsxSoftXapo(__uuidof(AudioReverb));
    return *ppApo ? S_OK : E_OUTOFMEMORY;
}

HRESULT WINAPI WsxExport_CreateFX(
    REFCLSID clsid,
    IUnknown** pEffect,
    const void* pInitDat,
    UINT32 InitDataByteSize) {
    if (!pEffect) return E_INVALIDARG;
    *pEffect = nullptr;
    if (!is_fallback_forced()) {
        ensure_system_xaudio2_loaded();
        if (g_state().fn_create_fx) {
            const HRESULT hr = g_state().fn_create_fx(clsid, pEffect, pInitDat, InitDataByteSize);
            if (SUCCEEDED(hr) && *pEffect) {
                g_state().xapo_effects_created.fetch_add(1, std::memory_order_relaxed);
                return hr;
            }
        }
    }
    *pEffect = new (std::nothrow) WsxSoftXapo(clsid);
    return *pEffect ? S_OK : E_OUTOFMEMORY;
}

HRESULT WINAPI WsxXAudio2GetStats(WsxXAudio2StatsData* pOutStats) {
    if (!pOutStats) return E_INVALIDARG;
    auto& st = g_state();
    pOutStats->backend_mode = st.backend_mode.load(std::memory_order_relaxed);
    pOutStats->force_fallback = is_fallback_forced() ? 1u : 0u;
    pOutStats->instances_created = st.instances_created.load(std::memory_order_relaxed);
    pOutStats->mastering_voices_created = st.mastering_voices_created.load(std::memory_order_relaxed);
    pOutStats->source_voices_created = st.source_voices_created.load(std::memory_order_relaxed);
    pOutStats->submix_voices_created = st.submix_voices_created.load(std::memory_order_relaxed);
    pOutStats->buffers_submitted = st.buffers_submitted.load(std::memory_order_relaxed);
    pOutStats->samples_processed = st.samples_processed.load(std::memory_order_relaxed);
    pOutStats->xapo_effects_created = st.xapo_effects_created.load(std::memory_order_relaxed);
    pOutStats->fallback_mastering_rescues = st.fallback_mastering_rescues.load(std::memory_order_relaxed);
    return S_OK;
}

void WINAPI WsxXAudio2SetForceFallback(BOOL forceFallback) {
    g_state().force_fallback.store(forceFallback ? 1u : 0u, std::memory_order_relaxed);
}

} // extern "C"
