// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox Series X|S XAudio2_9.dll compatibility shim header.
// Exports XAudio2Create (ordinal #1), CreateAudioReverb, CreateAudioVolumeMeter, CreateFX,
// and MizuBox telemetry / fallback control helpers.

#pragma once

#include <Windows.h>
#include <cstdint>

struct IXAudio2;

#ifndef XAUDIO2_DEFAULT_PROCESSOR
#define XAUDIO2_DEFAULT_PROCESSOR 0x00000001u
#endif

struct WSX_XAUDIO2_STATS {
    uint32_t backend_mode;          // 0 = uninitialized, 1 = System32 XAudio2_9 bridge, 2 = Software clean-room fallback
    uint32_t force_fallback;        // 1 if forced software fallback is active
    uint64_t instances_created;
    uint64_t mastering_voices_created;
    uint64_t source_voices_created;
    uint64_t submix_voices_created;
    uint64_t buffers_submitted;
    uint64_t samples_processed;
    uint64_t xapo_effects_created;
    uint64_t fallback_mastering_rescues;
};

extern "C" {
#ifndef __XAUDIO2_INCLUDED__
HRESULT WINAPI XAudio2Create(
    IXAudio2** ppXAudio2,
    UINT32 Flags,
    UINT32 XAudio2Processor);

HRESULT WINAPI CreateAudioVolumeMeter(
    IUnknown** ppApo);

HRESULT WINAPI CreateAudioReverb(
    IUnknown** ppApo);

HRESULT WINAPI CreateFX(
    REFCLSID clsid,
    IUnknown** pEffect,
    const void* pInitDat,
    UINT32 InitDataByteSize);

HRESULT WINAPI XAudio2CreateWithVersionInfo(
    IXAudio2** ppXAudio2,
    UINT32 Flags,
    UINT32 XAudio2Processor,
    UINT32 ntddiVersion);
#endif

HRESULT WINAPI WsxXAudio2GetStats(
    WSX_XAUDIO2_STATS* pOutStats);

void WINAPI WsxXAudio2SetForceFallback(
    BOOL forceFallback);
}
