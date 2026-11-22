// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// CTest #28: Extended User-Mode KernelX / XMem (XMemProtect, XMemMapPhysical,
// XMemGetPhysicalAddress, XMemFlushCache, XMemGetAllocationStatistics)
// + AcpHal.dll 48 kHz Stereo PCM Mixer, IMA-ADPCM Codec, and live IAcpHal::SubmitCommand.

#include "xmem.h"
#include "include/xbox/acphal.h"

#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

bool fail(const char* msg, HRESULT hr = S_OK) {
    if (hr != S_OK) {
        std::fprintf(stderr, "FAIL: %s (hr=0x%08lX)\n", msg, static_cast<unsigned long>(hr));
    } else {
        std::fprintf(stderr, "FAIL: %s\n", msg);
    }
    return false;
}

bool test_extended_xmem(HMODULE xmem_mod) {
    auto fnAlloc = reinterpret_cast<PVOID(WINAPI*)(SIZE_T, ULONGLONG)>(GetProcAddress(xmem_mod, "XMemAlloc"));
    auto fnFree = reinterpret_cast<VOID(WINAPI*)(PVOID, ULONGLONG)>(GetProcAddress(xmem_mod, "XMemFree"));
    auto fnQuery = reinterpret_cast<BOOL(WINAPI*)(HANDLE, LPCVOID, XMEM_MEMORY_BASIC_INFORMATION*)>(
        GetProcAddress(xmem_mod, "XMemVirtualQuery"));
    auto fnProtect = reinterpret_cast<BOOL(WINAPI*)(PVOID, SIZE_T, DWORD, PDWORD)>(
        GetProcAddress(xmem_mod, "XMemProtect"));
    auto fnMapPhys = reinterpret_cast<PVOID(WINAPI*)(SIZE_T, ULONG_PTR, DWORD, UINT64*)>(
        GetProcAddress(xmem_mod, "XMemMapPhysical"));
    auto fnUnmapPhys = reinterpret_cast<BOOL(WINAPI*)(PVOID)>(GetProcAddress(xmem_mod, "XMemUnmapPhysical"));
    auto fnGetPA = reinterpret_cast<UINT64(WINAPI*)(LPCVOID)>(GetProcAddress(xmem_mod, "XMemGetPhysicalAddress"));
    auto fnFlush = reinterpret_cast<VOID(WINAPI*)(LPCVOID, SIZE_T)>(GetProcAddress(xmem_mod, "XMemFlushCache"));
    auto fnStats = reinterpret_cast<BOOL(WINAPI*)(XMEM_ALLOCATION_STATISTICS*)>(
        GetProcAddress(xmem_mod, "XMemGetAllocationStatistics"));

    if (!fnAlloc || !fnFree || !fnQuery || !fnProtect || !fnMapPhys ||
        !fnUnmapPhys || !fnGetPA || !fnFlush || !fnStats) {
        return fail("Missing one or more extended XMem exports");
    }

    // 1. Standard XMemAlloc + interior physical address translation + XMemProtect RW -> RO -> RW
    void* mem = fnAlloc(131072u, 0);
    if (!mem) return fail("XMemAlloc failed");
    std::memset(mem, 0xAB, 131072u);

    const UINT64 pa0 = fnGetPA(mem);
    const UINT64 pa_off = fnGetPA(static_cast<const uint8_t*>(mem) + 1337u);
    if (pa0 == 0u || pa_off != pa0 + 1337u) {
        return fail("XMemGetPhysicalAddress interior offset translation mismatch");
    }

    DWORD oldProt = 0;
    if (!fnProtect(mem, 65536u, PAGE_READONLY, &oldProt) || oldProt != PAGE_READWRITE) {
        return fail("XMemProtect RW -> RO failed");
    }
    XMEM_MEMORY_BASIC_INFORMATION mbi{};
    if (!fnQuery(nullptr, mem, &mbi) || mbi.Protect != PAGE_READONLY || mbi.MemoryAttributes != 0x01u) {
        return fail("XMemVirtualQuery did not reflect PAGE_READONLY after XMemProtect");
    }
    if (!fnProtect(mem, 65536u, PAGE_READWRITE, &oldProt) || oldProt != PAGE_READONLY) {
        return fail("XMemProtect RO -> RW failed");
    }
    fnFlush(mem, 131072u);

    // 2. XMemMapPhysical (256 KiB aligned) + stats + unmap
    UINT64 phys_pa = 0;
    void* phys = fnMapPhys(131072u, 262144u, PAGE_READWRITE, &phys_pa);
    if (!phys || phys_pa == 0u || (reinterpret_cast<uintptr_t>(phys) & (262144u - 1u)) != 0u) {
        return fail("XMemMapPhysical failed or misalignment");
    }
    if (fnGetPA(static_cast<const uint8_t*>(phys) + 4096u) != phys_pa + 4096u) {
        return fail("XMemGetPhysicalAddress failed on XMemMapPhysical region");
    }
    fnFlush(phys, 131072u);

    XMEM_ALLOCATION_STATISTICS stats{};
    if (!fnStats(&stats) || stats.AllocationCount < 1u || stats.PhysicalMappingCount < 1u ||
        stats.PhysicalMappedBytes < 131072u || stats.PeakTrackedBytes < stats.TrackedBytes + stats.PhysicalMappedBytes) {
        return fail("XMemGetAllocationStatistics mismatch");
    }
    std::printf("  [XMem] pa=0x%llX phys_pa=0x%llX allocs=%llu phys_maps=%llu peak=%llu bytes OK\n",
                static_cast<unsigned long long>(pa0),
                static_cast<unsigned long long>(phys_pa),
                static_cast<unsigned long long>(stats.AllocationCount),
                static_cast<unsigned long long>(stats.PhysicalMappingCount),
                static_cast<unsigned long long>(stats.PeakTrackedBytes));

    if (!fnUnmapPhys(phys)) return fail("XMemUnmapPhysical failed");
    if (fnGetPA(phys) != 0u) return fail("Unmapped physical address still resolved");
    fnFree(mem, 0);
    return true;
}

bool test_acphal_audio_dsp() {
    HRESULT hr = ApuCreateHeap(32u * 1024u * 1024u, 32u * 1024u * 1024u);
    if (FAILED(hr)) return fail("ApuCreateHeap failed", hr);

    ComPtr<IAcpHal> hal;
    hr = AcpHalCreate(&hal);
    if (FAILED(hr) || !hal) return fail("AcpHalCreate failed", hr);
    hr = hal->Connect(64u, 64u);
    if (FAILED(hr)) return fail("IAcpHal::Connect failed", hr);

    // 1. Synthesize 1024-sample 440 Hz sine wave at 48 kHz and encode to IMA-ADPCM
    constexpr UINT32 kFrames = 1024u;
    std::vector<INT16> sine(kFrames, 0);
    for (UINT32 i = 0u; i < kFrames; ++i) {
        const double t = static_cast<double>(i) / 48000.0;
        sine[i] = static_cast<INT16>(std::lrint(16000.0 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * t)));
    }

    void* apu_adpcm = nullptr;
    void* apu_decoded = nullptr;
    void* apu_mix_out = nullptr;
    void* apu_dma_dst = nullptr;
    APU_ADDRESS pa1 = 0, pa2 = 0, pa3 = 0, pa4 = 0;

    if (FAILED(ApuAlloc(&apu_adpcm, &pa1, kFrames, 64u, APU_ALLOC_CACHED)) ||
        FAILED(ApuAlloc(&apu_decoded, &pa2, kFrames * sizeof(INT16), 64u, APU_ALLOC_CACHED)) ||
        FAILED(ApuAlloc(&apu_mix_out, &pa3, kFrames * 2u * sizeof(INT16), 64u, APU_ALLOC_CACHED)) ||
        FAILED(ApuAlloc(&apu_dma_dst, &pa4, kFrames * 2u * sizeof(INT16), 64u, APU_ALLOC_NONCACHED))) {
        return fail("ApuAlloc failed");
    }

    ACP_IMA_ADPCM_STATE enc_state{0, 0};
    UINT32 enc_bytes = 0u;
    hr = AcpHalEncodeImaAdpcm(sine.data(), kFrames, static_cast<UINT8*>(apu_adpcm), kFrames, &enc_state, &enc_bytes);
    if (FAILED(hr) || enc_bytes != kFrames / 2u) {
        return fail("AcpHalEncodeImaAdpcm failed", hr);
    }

    // 2. Submit real ACP_COMMAND_TYPE_XMA_DECODE via IAcpHal::SubmitCommand into APU buffer
    ACP_XMA_DECODE_DESC dec_desc{};
    dec_desc.srcAdpcm = static_cast<const UINT8*>(apu_adpcm);
    dec_desc.srcByteCount = enc_bytes;
    dec_desc.dstPcm = static_cast<INT16*>(apu_decoded);
    dec_desc.maxDstSamples = kFrames;
    dec_desc.initialState = {0, 0};

    hr = hal->SubmitCommand(ACP_COMMAND_TYPE_XMA_DECODE, 101u, 0u, &dec_desc, sizeof(dec_desc));
    if (FAILED(hr)) return fail("SubmitCommand(XMA_DECODE) failed", hr);

    ACP_MESSAGE msg{};
    hr = hal->PopMessage(&msg);
    if (hr != S_OK || msg.type != ACP_COMMAND_TYPE_XMA_DECODE || msg.payload != kFrames) {
        return fail("PopMessage(XMA_DECODE) mismatch", hr);
    }

    // Verify IMA-ADPCM waveform accuracy (RMS error well under 2% of peak amplitude)
    const auto* decoded_pcm = static_cast<const INT16*>(apu_decoded);
    double sum_sq_err = 0.0;
    for (UINT32 i = 0u; i < kFrames; ++i) {
        const double d = static_cast<double>(decoded_pcm[i]) - static_cast<double>(sine[i]);
        sum_sq_err += d * d;
    }
    const double rmse = std::sqrt(sum_sq_err / static_cast<double>(kFrames));
    if (rmse > 400.0) {
        std::fprintf(stderr, "IMA-ADPCM RMSE too high: %.2f\n", rmse);
        return fail("IMA-ADPCM decode quality failed");
    }
    std::printf("  [AcpHal:IMA-ADPCM] 4:1 encode/decode (%u -> %u bytes) RMSE=%.2f OK\n",
                kFrames * 2u, enc_bytes, rmse);

    // 3. Submit real ACP_COMMAND_TYPE_FLOWGRAPH mixing 2 hard-panned voices + clipping saturation
    std::vector<INT16> right_voice(kFrames, 20000);
    std::vector<INT16> boost_voice(kFrames, 25000);
    ACP_MIX_VOICE voices[3]{};
    // Voice 0: decoded sine hard-left (pan = -1.0)
    voices[0] = {decoded_pcm, kFrames, 1u, 0u, 1.0f, -1.0f, 0u};
    // Voice 1 + Voice 2: hard-right (pan = +1.0), sum = 45000 -> must clamp cleanly to +32767!
    voices[1] = {right_voice.data(), kFrames, 1u, 0u, 1.0f, +1.0f, 0u};
    voices[2] = {boost_voice.data(), kFrames, 1u, 0u, 1.0f, +1.0f, 0u};

    ACP_FLOWGRAPH_MIX_DESC mix_desc{};
    mix_desc.voices = voices;
    mix_desc.voiceCount = 3u;
    mix_desc.dstStereoPcm = static_cast<INT16*>(apu_mix_out);
    mix_desc.dstFrameCount = kFrames;
    mix_desc.clearOutput = TRUE;

    hr = hal->SubmitCommand(ACP_COMMAND_TYPE_FLOWGRAPH, 102u, 0u, &mix_desc, sizeof(mix_desc));
    if (FAILED(hr)) return fail("SubmitCommand(FLOWGRAPH) failed", hr);
    hr = hal->PopMessage(&msg);
    if (hr != S_OK || msg.type != ACP_COMMAND_TYPE_FLOWGRAPH || msg.payload != kFrames) {
        return fail("PopMessage(FLOWGRAPH) mismatch", hr);
    }

    const auto* stereo = static_cast<const INT16*>(apu_mix_out);
    // Hard-left channel should match decoded_pcm[i], hard-right channel should saturate to 32767
    for (UINT32 i = 0u; i < kFrames; ++i) {
        if (std::abs(static_cast<int>(stereo[i * 2u + 0u]) - static_cast<int>(decoded_pcm[i])) > 1) {
            return fail("Flowgraph left-channel pan mismatch");
        }
        if (stereo[i * 2u + 1u] != 32767) {
            return fail("Flowgraph right-channel clipping saturation mismatch");
        }
    }
    std::printf("  [AcpHal:Flowgraph] 48kHz stereo 3-voice pan+saturation mix (%u frames) OK\n", kFrames);

    // 4. Submit real ACP_COMMAND_TYPE_DMA_TRANSFER between APU heaps + verify invalid host ptr rejection
    const UINT32 mix_bytes = kFrames * 2u * static_cast<UINT32>(sizeof(INT16));
    ACP_DMA_TRANSFER_DESC dma_desc{apu_mix_out, apu_dma_dst, mix_bytes, 0u};
    hr = hal->SubmitCommand(ACP_COMMAND_TYPE_DMA_TRANSFER, 103u, 0u, &dma_desc, sizeof(dma_desc));
    if (FAILED(hr)) return fail("SubmitCommand(DMA_TRANSFER) failed", hr);
    hr = hal->PopMessage(&msg);
    if (hr != S_OK || msg.type != ACP_COMMAND_TYPE_DMA_TRANSFER || msg.payload != mix_bytes) {
        return fail("PopMessage(DMA_TRANSFER) mismatch", hr);
    }
    if (std::memcmp(apu_dma_dst, apu_mix_out, mix_bytes) != 0) {
        return fail("APU DMA transfer bytes mismatch");
    }

    // Verify invalid non-APU pointer is rejected when ACP_CMD_FLAG_ALLOW_HOST_PTR is omitted
    uint8_t stack_buf[64]{};
    ACP_DMA_TRANSFER_DESC bad_dma{stack_buf, apu_dma_dst, sizeof(stack_buf), 0u};
    const HRESULT bad_hr = hal->SubmitCommand(ACP_COMMAND_TYPE_DMA_TRANSFER, 104u, 0u, &bad_dma, sizeof(bad_dma));
    if (bad_hr != E_INVALIDARG) {
        return fail("Expected E_INVALIDARG for non-APU DMA source pointer", bad_hr);
    }
    std::printf("  [AcpHal:DMA] APU-to-APU transfer (%u bytes) + invalid-ptr guard OK\n", mix_bytes);

    hal->Disconnect();
    ApuFree(apu_dma_dst);
    ApuFree(apu_mix_out);
    ApuFree(apu_decoded);
    ApuFree(apu_adpcm);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const char* xmem_path = (argc > 1 && argv[1]) ? argv[1] : "xmem.dll";
    std::printf("[xmem_acphal_audio_test] Loading %s + testing AcpHal DSP/Mixer...\n", xmem_path);

    HMODULE xmem_mod = LoadLibraryA(xmem_path);
    if (!xmem_mod) {
        std::fprintf(stderr, "FAIL: Could not load %s (err=%lu)\n", xmem_path, GetLastError());
        return 1;
    }

    int rc = 0;
    if (!test_extended_xmem(xmem_mod)) rc = 2;
    else if (!test_acphal_audio_dsp()) rc = 3;

    FreeLibrary(xmem_mod);
    if (rc == 0) {
        std::printf("PASS: xmem_acphal_audio_test (XMem KernelX extensions + AcpHal IMA-ADPCM/48kHz mixer/DMA)\n");
    }
    return rc;
}
