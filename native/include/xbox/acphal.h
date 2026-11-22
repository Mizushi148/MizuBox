// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox Series X|S Audio Co-Processor HAL & APU memory allocator (AcpHal.dll).

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <unknwn.h>
#include <cstdint>

using APU_ADDRESS = UINT32;

constexpr APU_ADDRESS APU_INVALID_ADDRESS = 0u;

enum APU_ALLOC_FLAGS : UINT32 {
    APU_ALLOC_CACHED = 0u,
    APU_ALLOC_NONCACHED = 1u
};

struct APU_HEAP_STATE {
    UINT32 cachedSizeInBytes;
    UINT32 cachedUsedInBytes;
    UINT32 nonCachedSizeInBytes;
    UINT32 nonCachedUsedInBytes;
    UINT32 allocationCount;
    UINT32 reserved;
};

struct SHAPE_CONTEXTS {
    UINT32 numSrcContexts;
    UINT32 numEqContexts;
    UINT32 numFiltVolContexts;
    UINT32 numDmaContexts;
    UINT32 numXmaContexts;
    UINT32 numPcmContexts;
};

enum ACP_COMMAND_TYPE : UINT32 {
    ACP_COMMAND_TYPE_NONE = 0,
    ACP_COMMAND_TYPE_FLOWGRAPH = 1,
    ACP_COMMAND_TYPE_XMA_DECODE = 2,
    ACP_COMMAND_TYPE_DMA_TRANSFER = 3
};

constexpr UINT32 ACP_CMD_FLAG_ALLOW_HOST_PTR = 0x80000000u;
constexpr UINT32 ACP_CMD_FLAG_ASYNC          = 0x40000000u;

struct ACP_IMA_ADPCM_STATE {
    INT32 predictor;
    INT32 stepIndex;
};

struct ACP_MIX_VOICE {
    const INT16* pcmSamples;
    UINT32 frameCount;
    UINT16 channels;          // 1 = mono, 2 = interleaved stereo
    UINT16 reserved;
    float gain;               // linear gain (e.g. 1.0f)
    float pan;                // -1.0f (hard left) .. 0.0f (center) .. +1.0f (hard right)
    UINT32 outputFrameOffset; // starting stereo frame index in destination buffer
};

struct ACP_DMA_TRANSFER_DESC {
    const void* src;
    void* dst;
    UINT32 byteCount;
    UINT32 reserved;
};

struct ACP_XMA_DECODE_DESC {
    const UINT8* srcAdpcm;
    UINT32 srcByteCount;
    INT16* dstPcm;
    UINT32 maxDstSamples;
    ACP_IMA_ADPCM_STATE initialState;
};

struct ACP_FLOWGRAPH_MIX_DESC {
    const ACP_MIX_VOICE* voices;
    UINT32 voiceCount;
    INT16* dstStereoPcm;
    UINT32 dstFrameCount;
    BOOL clearOutput;
};

struct ACP_MESSAGE {
    UINT32 type;
    UINT32 flags;
    UINT64 timestamp;
    UINT64 payload;
};

enum ACP_BIQUAD_TYPE : UINT32 {
    ACP_BIQUAD_LOWPASS  = 0,
    ACP_BIQUAD_HIGHPASS = 1,
    ACP_BIQUAD_PEAKING  = 2
};

struct ACP_BIQUAD_PARAMS {
    ACP_BIQUAD_TYPE type;
    float sampleRate;
    float cutoffHz;
    float q;
    float gainDb;
};

struct ACP_BIQUAD_STATE {
    float z1;
    float z2;
};

struct ACP_AUDIO_OUTPUT_DESC {
    UINT32 sampleRate;           // Typically 48000
    UINT16 channels;             // 1 or 2
    UINT16 bitsPerSample;        // 16
    UINT32 bufferFrames;         // e.g. 960 (20 ms at 48 kHz)
    BOOL   preferSilentFallback; // If TRUE, bypass live speakers and use silent sink
};

struct ACP_AUDIO_OUTPUT_STATS {
    BOOL   isWasapi;
    UINT32 sampleRate;
    UINT16 channels;
    UINT16 reserved;
    UINT32 bufferFrames;
    UINT32 currentPaddingFrames;
    UINT64 totalFramesSubmitted;
    UINT64 underrunCount;
    float  estimatedLatencyMs;
    float  reservedF;
};

struct ACP_VEC3 {
    float x;
    float y;
    float z;
};

struct ACP_SPATIAL_LISTENER {
    ACP_VEC3 position;
    ACP_VEC3 forward;
    ACP_VEC3 up;
    ACP_VEC3 velocity;
};

struct ACP_SPATIAL_EMITTER {
    ACP_VEC3 position;
    ACP_VEC3 velocity;
    float minDistance;
    float maxDistance;
    float rolloffFactor;
    float dopplerScale;
};

struct ACP_SPATIAL_STATE {
    float delayRing[64];
    UINT32 delayWritePos;
    float headShadowLpL;
    float headShadowLpR;
    double dopplerPhase;
    float lastSample;
    float lastGain;
    float lastDopplerRatio;
};

struct ACP_REVERB_PARAMS {
    float roomSize;    // 0.0 .. 1.0 (default ~0.75)
    float damping;     // 0.0 .. 1.0 (default ~0.45)
    float wetMix;      // 0.0 .. 1.0 (default ~0.35)
    float dryMix;      // 0.0 .. 1.0 (default ~0.85)
    float stereoWidth; // 0.0 .. 1.0 (default ~1.0)
    UINT32 sampleRate; // e.g. 48000
};

struct ACP_LIMITER_PARAMS {
    float thresholdLinear; // e.g. 0.80f
    float ceilingLinear;   // e.g. 0.98f
    float releaseMs;       // e.g. 50.0f
    UINT32 sampleRate;     // e.g. 48000
};

struct ACP_LIMITER_STATE {
    float envelope;
    float gainReductionDb;
};

using ACP_AUDIO_OUTPUT_HANDLE = void*;
using ACP_REVERB_HANDLE = void*;

struct __declspec(uuid("b3714d94-3a57-420e-9e6a-81c9f4e2a501")) IAcpHal : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Connect(UINT32 commandQueueLength, UINT32 messageQueueLength) = 0;
    virtual HRESULT STDMETHODCALLTYPE Disconnect() = 0;
    virtual HRESULT STDMETHODCALLTYPE SubmitCommand(ACP_COMMAND_TYPE type, UINT32 commandId, UINT32 flags, const void* data, UINT32 dataSize) = 0;
    virtual HRESULT STDMETHODCALLTYPE PopMessage(ACP_MESSAGE* outMessage) = 0;
    virtual UINT64 STDMETHODCALLTYPE GetAudioFrameCount() = 0;
};

extern "C" {

HRESULT WINAPI ApuCreateHeap(UINT32 cachedSizeInBytes, UINT32 nonCachedSizeInBytes);
HRESULT WINAPI ApuAlloc(
    void** virtualAddress,
    APU_ADDRESS* physicalAddress,
    UINT32 sizeInBytes,
    UINT32 alignmentInBytes,
    UINT32 flags);
HRESULT WINAPI ApuFree(void* virtualAddress);
APU_ADDRESS WINAPI ApuMapVirtualAddress(const void* virtualAddress);
void* WINAPI ApuMapApuAddress(APU_ADDRESS physicalAddress);
BOOL WINAPI ApuIsVirtualAddressValid(const void* virtualAddress, UINT32 sizeInBytes);
HRESULT WINAPI ApuHeapGetState(APU_HEAP_STATE* outState, UINT32 flags);

HRESULT WINAPI AcpHalCreate(IAcpHal** ppAcpHal);
HRESULT WINAPI AcpHalAllocateShapeContexts(const SHAPE_CONTEXTS* pRequestedContexts);
HRESULT WINAPI AcpHalReleaseShapeContexts();

HRESULT WINAPI AcpHalEncodeImaAdpcm(
    const INT16* srcPcm,
    UINT32 sampleCount,
    UINT8* dstAdpcm,
    UINT32 dstCapacityBytes,
    ACP_IMA_ADPCM_STATE* ioState,
    UINT32* outEncodedBytes);

HRESULT WINAPI AcpHalDecodeImaAdpcm(
    const UINT8* srcAdpcm,
    UINT32 srcBytes,
    INT16* dstPcm,
    UINT32 maxSamples,
    ACP_IMA_ADPCM_STATE* ioState,
    UINT32* outDecodedSamples);

HRESULT WINAPI AcpHalMixVoices(
    const ACP_MIX_VOICE* voices,
    UINT32 voiceCount,
    INT16* dstStereoPcm,
    UINT32 dstFrameCount,
    BOOL clearOutput);

HRESULT WINAPI AcpHalOpenAudioOutput(
    const ACP_AUDIO_OUTPUT_DESC* desc,
    ACP_AUDIO_OUTPUT_HANDLE* outHandle);

HRESULT WINAPI AcpHalSubmitAudioOutput(
    ACP_AUDIO_OUTPUT_HANDLE handle,
    const INT16* interleavedPcm,
    UINT32 frameCount,
    UINT32* outFramesWritten);

HRESULT WINAPI AcpHalCloseAudioOutput(
    ACP_AUDIO_OUTPUT_HANDLE handle);

HRESULT WINAPI AcpHalResampleLinear(
    const INT16* srcPcm,
    UINT32 srcFrames,
    UINT32 srcSampleRate,
    UINT16 channels,
    INT16* dstPcm,
    UINT32 dstCapacityFrames,
    UINT32 dstSampleRate,
    UINT32* outDstFrames);

HRESULT WINAPI AcpHalApplyBiquadFilter(
    INT16* ioPcm,
    UINT32 frameCount,
    UINT16 channels,
    const ACP_BIQUAD_PARAMS* params,
    ACP_BIQUAD_STATE* ioChannelStates);

// Standard WAV/Xbox multichannel IMA-ADPCM: 4-byte (8-sample) chunks interleaved per channel 0..(channels-1), 1..8 channels.
HRESULT WINAPI AcpHalDecodeImaAdpcmMultichannel(
    const UINT8* srcAdpcm,
    UINT32 srcBytes,
    UINT16 channels,
    INT16* dstInterleavedPcm,
    UINT32 maxFrames,
    ACP_IMA_ADPCM_STATE* ioChannelStates,
    UINT32* outDecodedFrames);

// Standard Microsoft 4-bit ADPCM block decoder (1..2 channels, 7-predictor table, high-nibble first).
HRESULT WINAPI AcpHalDecodeMsAdpcm(
    const UINT8* srcBlock,
    UINT32 srcBytes,
    UINT16 channels,
    INT16* dstInterleavedPcm,
    UINT32 maxFrames,
    UINT32* outDecodedFrames);

// Wave 3 Next-Gen Audio DSP & Spatialization
HRESULT WINAPI AcpHalProcessSpatialVoice(
    const INT16* srcMonoPcm,
    UINT32 srcFrames,
    UINT32 sampleRate,
    const ACP_SPATIAL_LISTENER* listener,
    const ACP_SPATIAL_EMITTER* emitter,
    ACP_SPATIAL_STATE* ioState,
    INT16* dstStereoPcm,
    UINT32 dstCapacityFrames,
    UINT32* outDstFrames);

HRESULT WINAPI AcpHalCreateReverb(
    const ACP_REVERB_PARAMS* params,
    ACP_REVERB_HANDLE* outHandle);

HRESULT WINAPI AcpHalApplyReverb(
    ACP_REVERB_HANDLE handle,
    INT16* ioStereoPcm,
    UINT32 frameCount);

HRESULT WINAPI AcpHalDestroyReverb(
    ACP_REVERB_HANDLE handle);

HRESULT WINAPI AcpHalResampleCubic(
    const INT16* srcPcm,
    UINT32 srcFrames,
    UINT32 srcSampleRate,
    UINT16 channels,
    INT16* dstPcm,
    UINT32 dstCapacityFrames,
    UINT32 dstSampleRate,
    UINT32* outDstFrames);

HRESULT WINAPI AcpHalApplySoftLimiter(
    INT16* ioPcm,
    UINT32 frameCount,
    UINT16 channels,
    const ACP_LIMITER_PARAMS* params,
    ACP_LIMITER_STATE* ioState);

HRESULT WINAPI AcpHalDownmixToStereo(
    const INT16* srcMultiPcm,
    UINT32 frameCount,
    UINT16 srcChannels,
    INT16* dstStereoPcm);

HRESULT WINAPI AcpHalGetAudioOutputStats(
    ACP_AUDIO_OUTPUT_HANDLE handle,
    ACP_AUDIO_OUTPUT_STATS* outStats);

// Drain all asynchronous ACP command ring jobs submitted with ACP_CMD_FLAG_ASYNC in strict FIFO order.
HRESULT WINAPI AcpHalFlushAsyncCommands(
    IAcpHal* pAcpHal,
    DWORD timeoutMs);

} // extern "C"
