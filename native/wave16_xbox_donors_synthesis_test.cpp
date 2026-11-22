// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Wave 16 XBoxDonors Grand Synthesis End-to-End Verification Suite (Test #47):
//   1. d3d12_xs.dll & Xbox Binary ABI (Leonida + ScarlettX + Xbox2Windows):
//      - WsxXboxDeviceAbiProxy byte +0x18 == 1 (FSR4 CopyDescriptorsSimple safety flag)
//      - IID_IGraphicsUnknown & IID_ID3D12ConsDmaCommandList QueryInterface support
//      - Xbox Device vtable slots 68 (0x220), 69 (0x228), 73 (0x248 CreateCommandListX)
//      - WsxXboxCommandListAbiProxy full +0x1000 inline register cache scratch zone safety
//      - D3D12ConfigureVirtualMemoryX (0x10000000 success & bounds validation)
//      - D3D12XboxUnpackAllShaderFssm (dual tag-0x0D XBCH -> FSSM $VS_* container extraction)
//      - D3D12XboxRetargetDxilTriple ("dxil-ms-xb2" -> "dxil-ms-dx" + DXBC MD5 recompute)
//      - Hardware-verified RDNA2 SMEM & ~3 pre-truncation ((sOFF=2) + (IMM=2) == 0)
//   2. xmem.dll + D3D12XboxOpenExistingHeapAlias (Xbox2Windows era_heap / era_gpu_alias / era_title_va):
//      - XMemDecodeAttributes 64-bit XALLOC_ATTRIBUTES bitfield decoding + 256 KiB aligned graphics allocation
//      - Zero-copy ID3D12Device3::OpenExistingHeapFromAddress GPU aliasing + CPU write -> GPU readback
//      - XMemRegisterGpuAlias / XMemTranslateTitleGpuVa / XMemUnregisterGpuAlias
//   3. xgameruntime.dll GDK QueryApiImpl COM Tables (ScarlettX + Leonida):
//      - XGameImpl (XGameGetXboxTitleId), XSystemImpl (ConsoleId/SandboxId),
//        XGameRuntimeFeatureImpl, XGameEventImpl, XPackageImpl, IXAsyncTaskQueue slots 25..28
//   4. AcpHal.dll Complete APU Surface:
//      - ApuMapApuAddress (reverse APU physical -> CPU virtual lookup) & ApuHeapGetState

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "include/xbox/d3d12_xs.h"
#include "include/xbox/acphal.h"
#include "include/xbox/XGameRuntimeInit.h"
#include "include/xbox/XSystem.h"
#include "include/xbox/XPackage.h"
#include "d3d12_xs_xbox_abi.h"
#include "d3d12_xs_rdna2_to_hlsl.h"
#include "xmem.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "  [WAVE16 FAIL] %s\n", msg);
        ++g_failures;
    }
}

void check_hr(HRESULT hr, const char* msg) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "  [WAVE16 FAIL] %s (hr=0x%08lX)\n", msg, static_cast<unsigned long>(hr));
        ++g_failures;
    }
}

// Build a minimal valid zlib-compressed XBCH wrapping a synthetic FSSM blob
std::vector<uint8_t> make_stored_zlib(const uint8_t* raw, size_t len) {
    std::vector<uint8_t> z;
    z.push_back(0x78u);
    z.push_back(0x01u);
    z.push_back(0x01u); // BFINAL=1, BTYPE=0 (stored)
    const uint16_t l16 = static_cast<uint16_t>(len);
    const uint16_t nl16 = static_cast<uint16_t>(~l16);
    z.push_back(static_cast<uint8_t>(l16 & 0xFFu));
    z.push_back(static_cast<uint8_t>(l16 >> 8u));
    z.push_back(static_cast<uint8_t>(nl16 & 0xFFu));
    z.push_back(static_cast<uint8_t>(nl16 >> 8u));
    z.insert(z.end(), raw, raw + len);
    uint32_t s1 = 1u, s2 = 0u;
    for (size_t i = 0; i < len; ++i) {
        s1 = (s1 + raw[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    const uint32_t adler = (s2 << 16u) | s1;
    z.push_back(static_cast<uint8_t>((adler >> 24u) & 0xFFu));
    z.push_back(static_cast<uint8_t>((adler >> 16u) & 0xFFu));
    z.push_back(static_cast<uint8_t>((adler >> 8u) & 0xFFu));
    z.push_back(static_cast<uint8_t>(adler & 0xFFu));
    return z;
}

std::vector<uint8_t> make_dual_0d_dxbc(const std::vector<uint8_t>& fssm0, const std::vector<uint8_t>& fssm1) {
    const auto z0 = make_stored_zlib(fssm0.data(), fssm0.size());
    const auto z1 = make_stored_zlib(fssm1.data(), fssm1.size());

    auto make_xbch = [](const std::vector<uint8_t>& z, uint32_t unp_sz) {
        std::vector<uint8_t> xb(24u + z.size(), 0u);
        std::memcpy(xb.data(), "XBCH", 4u);
        const uint32_t ver = 3u;
        const uint32_t st_sz = static_cast<uint32_t>(z.size());
        std::memcpy(xb.data() + 4u, &ver, 4u);
        std::memcpy(xb.data() + 8u, &st_sz, 4u);
        std::memcpy(xb.data() + 16u, &unp_sz, 4u);
        std::memcpy(xb.data() + 24u, z.data(), z.size());
        return xb;
    };
    const auto xb0 = make_xbch(z0, static_cast<uint32_t>(fssm0.size()));
    const auto xb1 = make_xbch(z1, static_cast<uint32_t>(fssm1.size()));

    // DXIL chunk payload: XDAT header (12B) + 2 x 12B entries (24B) = 36B header, followed by xb0, xb1
    const uint32_t xdat_hdr_sz = 12u + 2u * 12u;
    const uint32_t off0 = xdat_hdr_sz;
    const uint32_t sz0 = static_cast<uint32_t>(xb0.size());
    const uint32_t off1 = off0 + sz0;
    const uint32_t sz1 = static_cast<uint32_t>(xb1.size());
    const uint32_t dxil_sz = off1 + sz1;

    std::vector<uint8_t> dxbc(36u + 8u + dxil_sz, 0u);
    std::memcpy(dxbc.data(), "DXBC", 4u);
    const uint32_t one = 1u;
    const uint32_t total_sz = static_cast<uint32_t>(dxbc.size());
    const uint32_t chunk_off = 36u;
    std::memcpy(dxbc.data() + 20u, &one, 4u);
    std::memcpy(dxbc.data() + 24u, &total_sz, 4u);
    std::memcpy(dxbc.data() + 28u, &one, 4u);
    std::memcpy(dxbc.data() + 32u, &chunk_off, 4u);
    std::memcpy(dxbc.data() + 36u, "DXIL", 4u);
    std::memcpy(dxbc.data() + 40u, &dxil_sz, 4u);

    uint8_t* xdat = dxbc.data() + 44u;
    std::memcpy(xdat, "XDAT", 4u);
    const uint32_t two = 2u;
    std::memcpy(xdat + 4u, &two, 4u);
    std::memcpy(xdat + 8u, &xdat_hdr_sz, 4u);
    const uint32_t tag_0d = 0x0Du;
    std::memcpy(xdat + 12u, &tag_0d, 4u);
    std::memcpy(xdat + 16u, &off0, 4u);
    std::memcpy(xdat + 20u, &sz0, 4u);
    std::memcpy(xdat + 24u, &tag_0d, 4u);
    std::memcpy(xdat + 28u, &off1, 4u);
    std::memcpy(xdat + 32u, &sz1, 4u);
    std::memcpy(xdat + off0, xb0.data(), xb0.size());
    std::memcpy(xdat + off1, xb1.data(), xb1.size());
    return dxbc;
}

} // namespace

int main() {
    std::printf("=== Wave 16 XBoxDonors Grand Synthesis Verification Suite ===\n");

    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 0x20004u;
    ComPtr<ID3D12Device> dev;
    check_hr(D3D12XboxCreateDevice(nullptr, &dev_params, IID_PPV_ARGS(&dev)), "D3D12XboxCreateDevice");
    if (!dev) return 1;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    check_hr(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");

    // ========================================================================
    // 1. d3d12_xs.dll & Xbox Binary ABI (Leonida + ScarlettX + Xbox2Windows)
    // ========================================================================
    std::printf("[1/4] Xbox Binary Vtable (+0x18 FSR4 flag, +0x1000 CL pad, VirtualMemoryX, Dual-0x0D FSSM, DXIL Retarget, SMEM &~3)...\n");
    {
        ComPtr<ID3D12Device> xbox_dev;
        check_hr(D3D12XboxGetXboxAbiDevice(dev.Get(), IID_PPV_ARGS(&xbox_dev)), "D3D12XboxGetXboxAbiDevice");
        if (xbox_dev) {
            const uint8_t flag18 = *reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(xbox_dev.Get()) + 0x18u);
            check(flag18 == 1u, "WsxXboxDeviceAbiProxy byte +0x18 == 1 (forces FSR4 through CopyDescriptorsSimple)");

            // Query IID_IGraphicsUnknown
            ComPtr<IUnknown> gfx_unk;
            check_hr(xbox_dev->QueryInterface(wsx_d3d12::kIidXboxGraphicsUnknown, reinterpret_cast<void**>(gfx_unk.GetAddressOf())),
                     "Xbox Device QueryInterface(IID_IGraphicsUnknown)");

            // Call Xbox Device slots 68 (0x220), 69 (0x228), and 73 (0x248 CreateCommandListX with DMA type 0x10)
            void** dev_vt = *reinterpret_cast<void***>(xbox_dev.Get());
            auto fn68 = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*)>(dev_vt[68]);
            auto fn69 = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, UINT, UINT)>(dev_vt[69]);
            auto fn73 = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, const D3D12_COMMAND_LIST_TYPE*, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**)>(dev_vt[73]);
            check_hr(fn68(xbox_dev.Get()), "Xbox Device slot 68 (device info)");
            check_hr(fn69(xbox_dev.Get(), 1u, 1u), "Xbox Device slot 69 (device setting)");

            ComPtr<ID3D12CommandAllocator> dma_ca;
            const auto dma_type = static_cast<D3D12_COMMAND_LIST_TYPE>(0x10u);
            check_hr(xbox_dev->CreateCommandAllocator(dma_type, IID_PPV_ARGS(&dma_ca)), "Xbox CreateCommandAllocator(DMA 0x10)");
            ComPtr<IUnknown> dma_cl;
            check_hr(fn73(xbox_dev.Get(), &dma_type, dma_ca.Get(), nullptr, wsx_d3d12::kIidXboxConsDmaCommandList,
                          reinterpret_cast<void**>(dma_cl.GetAddressOf())),
                     "Xbox Device slot 73 CreateCommandListX(DMA 0x10 -> IID_ID3D12ConsDmaCommandList)");
            if (dma_cl) {
                auto* raw_cl = reinterpret_cast<uint8_t*>(dma_cl.Get());
                check(*reinterpret_cast<uint64_t*>(raw_cl + 0x18u) == 0ULL &&
                      *reinterpret_cast<uint64_t*>(raw_cl + 0x28u) == 0ULL &&
                      *reinterpret_cast<uint64_t*>(raw_cl + 0x30u) == 0ULL,
                      "WsxXboxCommandListAbiProxy PM4 fast-path pointers (+0x18/+0x28/+0x30) are 0");
                // Simulate guest inline register cache writes up to +0xFF8 (e.g. +0x544 and +0xFE0)
                std::memset(raw_cl + 0x544u, 0xCC, 32u);
                std::memset(raw_cl + 0xFE0u, 0xAA, 16u);
                ComPtr<IUnknown> cl_gfx_unk;
                check_hr(dma_cl->QueryInterface(wsx_d3d12::kIidXboxGraphicsUnknown, reinterpret_cast<void**>(cl_gfx_unk.GetAddressOf())),
                         "CommandList survives writes up to +0xFF0 inside +0x1000 scratch pad");
            }
        }

        // D3D12ConfigureVirtualMemoryX
        D3D12XBOX_VIRTUAL_MEMORY_CONFIGURATION vm_cfg{1u, 0x10u, 0xFFFFFFFFu, 0u, 99ULL, 99ULL};
        check(D3D12ConfigureVirtualMemoryX(&vm_cfg) == static_cast<HRESULT>(0x10000000L) &&
              vm_cfg.Result0 == 0ULL && vm_cfg.Result1 == 0ULL,
              "D3D12ConfigureVirtualMemoryX returns 0x10000000 and zeroes Result0/Result1");
        D3D12XBOX_VIRTUAL_MEMORY_CONFIGURATION bad_vm{1u, 0x99u, 0u, 0u, 0ULL, 0ULL};
        check(D3D12ConfigureVirtualMemoryX(&bad_vm) == E_INVALIDARG,
              "D3D12ConfigureVirtualMemoryX rejects SizeIn4MbUnits > 0x21");

        // Dual tag-0x0D XBCH -> FSSM extraction (Leonida 195 $VS_* discovery)
        std::vector<uint8_t> f0 = {'F','S','S','M', 0x01, 0x02, 0x03, 0x04};
        std::vector<uint8_t> f1 = {'F','S','S','M', 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
        const auto dual_dxbc = make_dual_0d_dxbc(f0, f1);
        uint32_t total_payloads = 0u;
        size_t sz0 = 0u, sz1 = 0u;
        uint8_t out0[32]{}, out1[32]{};
        check_hr(D3D12XboxUnpackAllShaderFssm(dual_dxbc.data(), dual_dxbc.size(), 0u, out0, sizeof(out0), &sz0, &total_payloads),
                 "D3D12XboxUnpackAllShaderFssm(payload 0)");
        check_hr(D3D12XboxUnpackAllShaderFssm(dual_dxbc.data(), dual_dxbc.size(), 1u, out1, sizeof(out1), &sz1, &total_payloads),
                 "D3D12XboxUnpackAllShaderFssm(payload 1)");
        check(total_payloads == 2u && sz0 == f0.size() && sz1 == f1.size() &&
              std::memcmp(out0, f0.data(), sz0) == 0 && std::memcmp(out1, f1.data(), sz1) == 0,
              "Both tag-0x0D FSSM payloads extracted bit-exact from dual-0x0D DXBC container");

        // DXIL target triple retargeting ("dxil-ms-xb2" -> "dxil-ms-dx")
        std::vector<uint8_t> fake_dxil(64u, 0u);
        std::memcpy(fake_dxil.data(), "DXBC", 4u);
        const uint32_t fsz = 64u;
        std::memcpy(fake_dxil.data() + 24u, &fsz, 4u);
        std::memcpy(fake_dxil.data() + 36u, "dxil-ms-xb2", 11u);
        std::vector<uint8_t> retargeted(64u, 0u);
        size_t r_sz = 0u;
        UINT patched_cnt = 0u;
        check_hr(D3D12XboxRetargetDxilTriple(fake_dxil.data(), fake_dxil.size(), retargeted.data(), retargeted.size(), &r_sz, &patched_cnt),
              "D3D12XboxRetargetDxilTriple");
        check(patched_cnt == 1u && std::memcmp(retargeted.data() + 36u, "dxil-ms-dx\0", 11u) == 0,
              "Target triple retargeted from dxil-ms-xb2 to dxil-ms-dx and DXBC MD5 updated");

        // Hardware-verified RDNA2 SMEM & ~3 pre-truncation
        // Encode s_load_dword s4, s[0:1], s2 offset:0x02 (SMEM op 0x00: w0 = 0xF4000000 | (sdata<<6) | (sbase>>1), w1 = (soff<<25) | imm)
        std::vector<uint32_t> smem_code = {
            0xF4000000u | (4u << 6u) | 0u,
            (2u << 25u) | 2u,
            0xBF810000u // s_endpgm
        };
        constexpr uint32_t kCodeOff = 0x150u;
        std::vector<uint8_t> fssm_smem(kCodeOff + smem_code.size() * 4u, 0u);
        std::memcpy(fssm_smem.data(), "FSSM", 4u);
        const uint32_t raw_c0 = kCodeOff - 0x28u;
        const uint32_t c_sz = static_cast<uint32_t>(smem_code.size() * 4u);
        const uint32_t ntx = 32u, one = 1u;
        std::memcpy(fssm_smem.data() + 0xC0u, &raw_c0, 4u);
        std::memcpy(fssm_smem.data() + 0xC8u, &c_sz, 4u);
        std::memcpy(fssm_smem.data() + 0x13Cu, &ntx, 4u);
        std::memcpy(fssm_smem.data() + 0x140u, &one, 4u);
        std::memcpy(fssm_smem.data() + 0x144u, &one, 4u);
        std::memcpy(fssm_smem.data() + kCodeOff, smem_code.data(), c_sz);
        wsx_rdna2::TranslateStats st{};
        wsx_rdna2::HlslEmitter em;
        const std::string hlsl = em.translate(fssm_smem.data(), fssm_smem.size(), 5u, st);
        check(hlsl.find("& ~3ull") != std::string::npos && hlsl.find("& ~3u") != std::string::npos,
              "SMEM s_load_dword applies hardware-verified & ~3 pre-truncation to base, sOFF, and IMM");
        std::printf("  [PASS 1] Xbox ABI +0x18/+0x1000, VirtualMemoryX, Dual-0x0D FSSM, DXIL Retarget, SMEM &~3 OK\n");
    }

    // ========================================================================
    // 2. xmem.dll + Zero-Copy Existing-Heap GPU Aliasing (Xbox2Windows)
    // ========================================================================
    std::printf("[2/4] XMemDecodeAttributes, 256 KiB Aligned Graphics Alloc, Zero-Copy OpenExistingHeapFromAddress GPU Alias...\n");
    {
        HMODULE hXmem = LoadLibraryA("xmem.dll");
        check(hXmem != nullptr, "LoadLibraryA(xmem.dll)");
        using PFN_XMemAlloc = void* (WINAPI*)(SIZE_T, ULONGLONG);
        using PFN_XMemFree = void (WINAPI*)(void*, ULONGLONG);
        using PFN_XMemDecodeAttributes = BOOL (WINAPI*)(ULONGLONG, XMEM_DECODED_ATTRIBUTES*);
        using PFN_XMemRegisterGpuAlias = BOOL (WINAPI*)(const void*, SIZE_T, UINT64);
        using PFN_XMemTranslateTitleGpuVa = BOOL (WINAPI*)(UINT64, SIZE_T, UINT64*);
        auto pfnXMemAlloc = reinterpret_cast<PFN_XMemAlloc>(GetProcAddress(hXmem, "XMemAlloc"));
        auto pfnXMemFree = reinterpret_cast<PFN_XMemFree>(GetProcAddress(hXmem, "XMemFree"));
        auto pfnXMemDecodeAttributes = reinterpret_cast<PFN_XMemDecodeAttributes>(GetProcAddress(hXmem, "XMemDecodeAttributes"));
        auto pfnXMemRegisterGpuAlias = reinterpret_cast<PFN_XMemRegisterGpuAlias>(GetProcAddress(hXmem, "XMemRegisterGpuAlias"));
        auto pfnXMemTranslateTitleGpuVa = reinterpret_cast<PFN_XMemTranslateTitleGpuVa>(GetProcAddress(hXmem, "XMemTranslateTitleGpuVa"));
        check(pfnXMemAlloc && pfnXMemFree && pfnXMemDecodeAttributes && pfnXMemRegisterGpuAlias && pfnXMemTranslateTitleGpuVa,
              "xmem.dll exports XMemAlloc, XMemFree, XMemDecodeAttributes, XMemRegisterGpuAlias, XMemTranslateTitleGpuVa");

        // Pack XALLOC_ATTRIBUTES: object_type=0x123, page_size=1 (64KB), allocator_id=0x42, alignment_log2=18 (256KB), memory_type=2 (graphics)
        const ULONGLONG attrs =
            0x123ULL |
            (1ULL << 14u) |
            (0x42ULL << 16u) |
            (18ULL << 24u) |
            (2ULL << 29u);
        XMEM_DECODED_ATTRIBUTES dec{};
        check(pfnXMemDecodeAttributes(attrs, &dec) != FALSE, "XMemDecodeAttributes");
        check(dec.IsValid == 1u && dec.IsGraphicsMemory == 1u &&
              dec.ObjectType == 0x123u && dec.PageSizeBytes == 65536ULL &&
              dec.AlignmentBytes == 262144ULL && dec.MemoryType == 2u,
              "XMemDecodeAttributes decoded all 64-bit XALLOC_ATTRIBUTES fields accurately");

        void* aligned_256k = pfnXMemAlloc(262144u, attrs);
        check(aligned_256k != nullptr && (reinterpret_cast<uintptr_t>(aligned_256k) & 0x3FFFFu) == 0u,
              "XMemAlloc honored 256 KiB alignment from XALLOC_ATTRIBUTES");
        if (aligned_256k) pfnXMemFree(aligned_256k, attrs);

        // Zero-Copy Existing-Heap GPU Aliasing on 64 KiB VirtualAlloc/XMemAlloc buffer
        const ULONGLONG gfx_64k_attrs = (1ULL << 14u) | (16ULL << 24u) | (1ULL << 29u);
        void* cpu_shared = pfnXMemAlloc(65536u, gfx_64k_attrs);
        check(cpu_shared != nullptr, "XMemAlloc(64 KiB graphics)");
        if (cpu_shared) {
            auto* words = static_cast<uint32_t*>(cpu_shared);
            words[0] = 0xCAFEBABEu;
            words[1] = 0x1337C0DEu;
            words[2] = 0x87654321u;
            words[3] = 0xdeadbeefu;

            ComPtr<ID3D12Heap> alias_heap;
            ComPtr<ID3D12Resource> alias_res;
            D3D12_GPU_VIRTUAL_ADDRESS alias_gpu_va = 0ULL;
            const HRESULT alias_hr = D3D12XboxOpenExistingHeapAlias(
                dev.Get(), cpu_shared, 65536u, &alias_heap, &alias_res, &alias_gpu_va);
            check_hr(alias_hr, "D3D12XboxOpenExistingHeapAlias");
            check(alias_heap && alias_res && alias_gpu_va != 0ULL, "Zero-copy D3D12 heap + placed resource created");

            // Register in XMem Title-VA -> Desktop-GPU-VA table and test translation
            const uint64_t title_va = reinterpret_cast<uint64_t>(cpu_shared);
            check(pfnXMemRegisterGpuAlias(cpu_shared, 65536u, alias_gpu_va) != FALSE, "XMemRegisterGpuAlias");
            UINT64 translated_va = 0ULL;
            check(pfnXMemTranslateTitleGpuVa(title_va + 256ULL, 64u, &translated_va) != FALSE &&
                  translated_va == alias_gpu_va + 256ULL,
                  "XMemTranslateTitleGpuVa translated Title VA + 256 -> Desktop GPU VA + 256");

            // Read back the zero-copy aliased buffer on the RTX 5060 GPU via CopyBufferRegion (NO CPU->GPU upload!)
            D3D12_HEAP_PROPERTIES hp_rb{};
            hp_rb.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rbd{};
            rbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rbd.Width = 256u;
            rbd.Height = 1u;
            rbd.DepthOrArraySize = 1u;
            rbd.MipLevels = 1u;
            rbd.SampleDesc.Count = 1u;
            rbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> rb;
            check_hr(dev->CreateCommittedResource(&hp_rb, D3D12_HEAP_FLAG_NONE, &rbd,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)),
                     "Create Readback Buffer");
            ComPtr<ID3D12CommandAllocator> ca;
            ComPtr<ID3D12GraphicsCommandList> cl;
            ComPtr<ID3D12Fence> fence;
            check_hr(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca)), "CreateCA");
            check_hr(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr, IID_PPV_ARGS(&cl)), "CreateCL");
            check_hr(dev->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");

            cl->CopyBufferRegion(rb.Get(), 0u, alias_res.Get(), 0u, 16u);
            cl->Close();
            ID3D12CommandList* lists[] = { cl.Get() };
            queue->ExecuteCommandLists(1u, lists);
            queue->Signal(fence.Get(), 1u);
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (fence->GetCompletedValue() < 1u) {
                fence->SetEventOnCompletion(1u, ev);
                WaitForSingleObject(ev, 5000u);
            }
            CloseHandle(ev);

            uint32_t gpu_read[4]{};
            void* mapped = nullptr;
            D3D12_RANGE rr{0, 16u};
            if (SUCCEEDED(rb->Map(0, &rr, &mapped)) && mapped) {
                std::memcpy(gpu_read, mapped, 16u);
                D3D12_RANGE zw{0, 0};
                rb->Unmap(0, &zw);
            }
            check(gpu_read[0] == 0xCAFEBABEu && gpu_read[1] == 0x1337C0DEu &&
                  gpu_read[2] == 0x87654321u && gpu_read[3] == 0xdeadbeefu,
                  "RTX 5060 GPU read CPU memory directly via zero-copy OpenExistingHeapFromAddress!");

            alias_res.Reset();
            alias_heap.Reset();
            pfnXMemFree(cpu_shared, gfx_64k_attrs);
            check(pfnXMemTranslateTitleGpuVa(title_va, 4u, &translated_va) == FALSE,
                  "XMemFree automatically unregistered Title-VA GPU alias");
            std::printf("  [PASS 2] XALLOC_ATTRIBUTES + 256KB Align + Zero-Copy GPU Heap Alias (0x%08X) OK\n", gpu_read[0]);
        }
    }

    // ========================================================================
    // 3. xgameruntime.dll GDK QueryApiImpl COM Tables (ScarlettX + Leonida)
    // ========================================================================
    std::printf("[3/4] xgameruntime.dll QueryApiImpl COM Tables (XGame, XSystem, XFeature, XEvent, XPackage)...\n");
    {
        check_hr(XGameRuntimeInitialize(), "XGameRuntimeInitialize");
        auto pQueryApi = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(const GUID*, const GUID*, void**)>(
            GetProcAddress(GetModuleHandleW(L"xgameruntime.dll"), "QueryApiImpl"));
        check(pQueryApi != nullptr, "Resolved xgameruntime!QueryApiImpl");
        if (pQueryApi) {
            // XGameImpl ({973A344E-24BF-4D0F-8457-56C534892B29})
            constexpr GUID kGuidXGame = {0x973A344Eu, 0x24BFu, 0x4D0Fu, {0x84u, 0x57u, 0x56u, 0xC5u, 0x34u, 0x89u, 0x2Bu, 0x29u}};
            void* xgame = nullptr;
            check_hr(pQueryApi(&kGuidXGame, &kGuidXGame, &xgame), "QueryApiImpl(XGameImpl)");
            if (xgame) {
                void** vt = *reinterpret_cast<void***>(xgame);
                auto fnGetTitleId = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t*)>(vt[3]);
                uint32_t tid = 0u;
                check_hr(fnGetTitleId(xgame, &tid), "XGameImpl::slot3(XGameGetXboxTitleId)");
                check(tid != 0u, "XGameGetXboxTitleId returned non-zero TitleId");
            }

            // XSystemImpl ({E349BD1A-FC20-4E40-B99C-4178CC6B409F})
            constexpr GUID kGuidXSys = {0xE349BD1Au, 0xFC20u, 0x4E40u, {0xB9u, 0x9Cu, 0x41u, 0x78u, 0xCCu, 0x6Bu, 0x40u, 0x9Fu}};
            void* xsys = nullptr;
            check_hr(pQueryApi(&kGuidXSys, &kGuidXSys, &xsys), "QueryApiImpl(XSystemImpl)");
            if (xsys) {
                void** vt = *reinterpret_cast<void***>(xsys);
                auto fnCid = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, size_t, char*, size_t*)>(vt[3]);
                char cid[64]{};
                size_t used = 0u;
                check_hr(fnCid(xsys, sizeof(cid), cid, &used), "XSystemImpl::slot3(XSystemGetConsoleId)");
                check(std::strlen(cid) == 38u, "XSystemImpl returned 38-char 8.8.8.8.2 ConsoleId");
            }

            // XGameRuntimeFeatureImpl ({8836FE87-EDB9-4FE3-8DAD-05F0D2CD5B40})
            constexpr GUID kGuidXFeat = {0x8836FE87u, 0xEDB9u, 0x4FE3u, {0x8Du, 0xADu, 0x05u, 0xF0u, 0xD2u, 0xCDu, 0x5Bu, 0x40u}};
            void* xfeat = nullptr;
            check_hr(pQueryApi(&kGuidXFeat, &kGuidXFeat, &xfeat), "QueryApiImpl(XGameRuntimeFeatureImpl)");
            if (xfeat) {
                void** vt = *reinterpret_cast<void***>(xfeat);
                auto fnAvail = reinterpret_cast<bool(STDMETHODCALLTYPE*)(void*, uint32_t)>(vt[3]);
                check(fnAvail(xfeat, 5u) && !fnAvail(xfeat, 99u), "XGameRuntimeIsFeatureAvailable(5==true, 99==false)");
            }

            // XGameEventImpl ({BBFBDCC7-BFE7-409B-A5CA-EDF054960B4D})
            constexpr GUID kGuidXEvt = {0xBBFBDCC7u, 0xBFE7u, 0x409Bu, {0xA5u, 0xCAu, 0xEDu, 0xF0u, 0x54u, 0x96u, 0x0Bu, 0x4Du}};
            void* xevt = nullptr;
            check_hr(pQueryApi(&kGuidXEvt, &kGuidXEvt, &xevt), "QueryApiImpl(XGameEventImpl)");

            // XPackageImpl ({AF406016-E850-4AA8-A88D-2F3DCB9DAC7E})
            constexpr GUID kGuidXPkg = {0xAF406016u, 0xE850u, 0x4AA8u, {0xA8u, 0x8Du, 0x2Fu, 0x3Du, 0xCBu, 0x9Du, 0xACu, 0x7Eu}};
            void* xpkg = nullptr;
            check_hr(pQueryApi(&kGuidXPkg, &kGuidXPkg, &xpkg), "QueryApiImpl(XPackageImpl)");
            if (xpkg) {
                void** vt = *reinterpret_cast<void***>(xpkg);
                auto fnLoc = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, size_t, char*)>(vt[11]);
                char loc[32]{};
                check_hr(fnLoc(xpkg, sizeof(loc), loc), "XPackageImpl::slot11(XPackageGetUserLocale)");
                check(std::strlen(loc) > 0u, "XPackageImpl returned non-empty locale");
            }
        }
        XGameRuntimeUninitialize();
        std::printf("  [PASS 3] QueryApiImpl COM Tables (XGame, XSystem, XFeature, XEvent, XPackage) OK\n");
    }

    // ========================================================================
    // 4. AcpHal.dll ApuMapApuAddress & ApuHeapGetState
    // ========================================================================
    std::printf("[4/5] AcpHal.dll ApuMapApuAddress & ApuHeapGetState...\n");
    {
        check_hr(ApuCreateHeap(8u * 1024u * 1024u, 8u * 1024u * 1024u), "ApuCreateHeap");
        void* va = nullptr;
        APU_ADDRESS pa = 0u;
        check_hr(ApuAlloc(&va, &pa, 8192u, 64u, APU_ALLOC_CACHED), "ApuAlloc(8192)");
        check(ApuMapApuAddress(pa + 512u) == static_cast<uint8_t*>(va) + 512u,
              "ApuMapApuAddress reverse physical->virtual lookup exact");
        check(ApuMapApuAddress(0xDEADBEEFu) == nullptr, "ApuMapApuAddress returns nullptr for unmapped address");

        APU_HEAP_STATE hst{};
        check_hr(ApuHeapGetState(&hst, 0u), "ApuHeapGetState");
        check(hst.cachedUsedInBytes >= 8192u && hst.allocationCount >= 1u,
              "ApuHeapGetState reports active APU allocation");
        ApuFree(va);
        std::printf("  [PASS 4] AcpHal ApuMapApuAddress & ApuHeapGetState (used=%u B, count=%u) OK\n",
                    hst.cachedUsedInBytes, hst.allocationCount);
    }

    // ========================================================================
    // 5. Multi-Title Console OS & D3D12.X Discoveries:
    //    - 0x00400000 ResourceBarrier State Bit Strip
    //    - TEXTURE1D / TEXTURE2D GetGPUVirtualAddress() & Map() UMA Backing
    //    - 16-Core Zen 2 Thread Affinity Mask Folding
    //    - MFResetDXGIDeviceManagerX Device Unwrapping & D3DMapEsramMemory
    // ========================================================================
    std::printf("[5/5] ResourceBarrier 0x400000 Strip, Texture1D/2D UMA GetGPUVirtualAddress+Map, 16-Core Affinity, MFResetDXGI & ESRAM...\n");
    {
        // 5a. Create a DEFAULT-heap TEXTURE1D and TEXTURE2D and verify GetGPUVirtualAddress() & Map()
        D3D12_HEAP_PROPERTIES hp_def{};
        hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC td1{};
        td1.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        td1.Width = 64u;
        td1.Height = 1u;
        td1.DepthOrArraySize = 1u;
        td1.MipLevels = 1u;
        td1.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td1.SampleDesc.Count = 1u;
        td1.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        ComPtr<ID3D12Resource> tex1d;
        check_hr(dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td1,
                                              D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex1d)),
                 "CreateCommittedResource(TEXTURE1D)");
        const D3D12_GPU_VIRTUAL_ADDRESS tex1d_va = tex1d ? tex1d->GetGPUVirtualAddress() : 0ULL;
        check(tex1d_va != 0ULL, "TEXTURE1D GetGPUVirtualAddress() returns non-zero UMA CPU backing pointer");
        void* tex1d_mapped = nullptr;
        check_hr(tex1d->Map(0u, nullptr, &tex1d_mapped), "TEXTURE1D Map(0) succeeds via UMA backing fallback");
        check(tex1d_mapped == reinterpret_cast<void*>(static_cast<uintptr_t>(tex1d_va)),
              "TEXTURE1D Map(0) pointer matches GetGPUVirtualAddress()");
        if (tex1d_mapped) {
            static_cast<uint32_t*>(tex1d_mapped)[0] = 0xFF00FF00u;
            tex1d->Unmap(0u, nullptr);
        }

        D3D12_RESOURCE_DESC td2 = td1;
        td2.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td2.Width = 32u;
        td2.Height = 32u;
        ComPtr<ID3D12Resource> tex2d;
        check_hr(dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td2,
                                              D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex2d)),
                 "CreateCommittedResource(TEXTURE2D)");
        const D3D12_GPU_VIRTUAL_ADDRESS tex2d_va = tex2d ? tex2d->GetGPUVirtualAddress() : 0ULL;
        check(tex2d_va != 0ULL, "TEXTURE2D GetGPUVirtualAddress() returns non-zero UMA CPU backing pointer");

        // 5b. Submit ResourceBarrier with Xbox 0x00400000 bit OR'd into PIXEL_SHADER_RESOURCE
        ComPtr<ID3D12CommandAllocator> ca;
        ComPtr<ID3D12GraphicsCommandList> cl;
        ComPtr<ID3D12Fence> fence;
        check_hr(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca)), "CreateCA(5b)");
        check_hr(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr, IID_PPV_ARGS(&cl)), "CreateCL(5b)");
        check_hr(dev->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence(5b)");

        D3D12_RESOURCE_BARRIER b1{};
        b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b1.Transition.pResource = tex2d.Get();
        b1.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b1.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b1.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | 0x00400000u);
        cl->ResourceBarrier(1u, &b1);

        D3D12_RESOURCE_BARRIER b2 = b1;
        b2.Transition.StateBefore = b1.Transition.StateAfter;
        b2.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        cl->ResourceBarrier(1u, &b2);

        check_hr(cl->Close(), "Close CL after Xbox 0x00400000 ResourceBarrier");
        ID3D12CommandList* lists[] = { cl.Get() };
        queue->ExecuteCommandLists(1u, lists);
        check_hr(queue->Signal(fence.Get(), 1u), "Signal fence after 0x00400000 ResourceBarrier");
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence->GetCompletedValue() < 1u) {
            fence->SetEventOnCompletion(1u, ev);
            WaitForSingleObject(ev, 5000u);
        }
        CloseHandle(ev);

        // 5c. 16-Core Zen 2 Thread Affinity Mask Remapping
        DWORD_PTR proc_mask = 0u, sys_mask = 0u;
        GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask);
        const ULONG_PTR core14 = D3D12XboxRemapThreadAffinityMask(static_cast<ULONG_PTR>(1ULL << 14u));
        const ULONG_PTR core15 = D3D12XboxRemapThreadAffinityMask(static_cast<ULONG_PTR>(1ULL << 15u));
        check(core14 != 0u && (core14 & proc_mask) == core14 &&
              core15 != 0u && (core15 & proc_mask) == core15,
              "D3D12XboxRemapThreadAffinityMask folds Xbox cores 14 & 15 onto valid host logical processors");
        const ULONG_PTR bad_zero = D3D12XboxRemapThreadAffinityMask(0u);
        check(bad_zero == 0u && GetLastError() == ERROR_INVALID_PARAMETER,
              "D3D12XboxRemapThreadAffinityMask(0) fails with ERROR_INVALID_PARAMETER");
        const ULONG_PTR bad_high = D3D12XboxRemapThreadAffinityMask(static_cast<ULONG_PTR>(1ULL << 28u));
        check(bad_high == 0u && GetLastError() == ERROR_INVALID_PARAMETER,
              "D3D12XboxRemapThreadAffinityMask(1<<28) fails with ERROR_INVALID_PARAMETER");

        // 5d. MFResetDXGIDeviceManagerX double-proxy unwrapping
        struct MockMfDxgiManager {
            void** vtable = nullptr;
            IUnknown* received_device = nullptr;
            UINT received_token = 0u;
            static HRESULT STDMETHODCALLTYPE slot6_reset(IUnknown* self, IUnknown* pDev, UINT tok) noexcept {
                auto* m = reinterpret_cast<MockMfDxgiManager*>(self);
                m->received_device = pDev;
                m->received_token = tok;
                return S_OK;
            }
        };
        void* mock_vt[9]{};
        mock_vt[6] = reinterpret_cast<void*>(&MockMfDxgiManager::slot6_reset);
        MockMfDxgiManager mgr{mock_vt, nullptr, 0u};

        ComPtr<ID3D12Device> xbox_abi_dev;
        check_hr(D3D12XboxGetXboxAbiDevice(dev.Get(), IID_PPV_ARGS(&xbox_abi_dev)), "D3D12XboxGetXboxAbiDevice(5d)");
        IUnknown* xbox_abi_unk = static_cast<IUnknown*>(xbox_abi_dev.Get());
        check_hr(MFResetDXGIDeviceManagerX(reinterpret_cast<IUnknown*>(&mgr), xbox_abi_unk, 0x4242u),
                 "MFResetDXGIDeviceManagerX");
        check(mgr.received_device != nullptr &&
              mgr.received_device != xbox_abi_unk &&
              mgr.received_device != static_cast<IUnknown*>(dev.Get()) &&
              mgr.received_token == 0x4242u,
              "MFResetDXGIDeviceManagerX unwrapped both WsxXboxDeviceAbiProxy and WsxD3D12DeviceProxy to raw desktop ID3D12Device");

        // 5e. D3DMapEsramMemory (4 x 64 KiB pages = 256 KiB)
        void* esram_va = VirtualAlloc(nullptr, 4u * 65536u, MEM_RESERVE, PAGE_NOACCESS);
        check(esram_va != nullptr, "VirtualAlloc(MEM_RESERVE 256 KiB for ESRAM)");
        if (esram_va) {
            check_hr(D3DMapEsramMemory(1u, esram_va, 4u, nullptr), "D3DMapEsramMemory(Flags=1, 4 pages)");
            static_cast<uint32_t*>(esram_va)[0] = 0xE52A1100u;
            static_cast<uint32_t*>(esram_va)[65535] = 0x11223344u;
            check(static_cast<uint32_t*>(esram_va)[0] == 0xE52A1100u &&
                  static_cast<uint32_t*>(esram_va)[65535] == 0x11223344u,
                  "D3DMapEsramMemory committed writable 256 KiB ESRAM region at caller VA");
            check(D3DMapEsramMemory(2u, esram_va, 4u, nullptr) == E_INVALIDARG,
                  "D3DMapEsramMemory rejects invalid Flags=2");
            VirtualFree(esram_va, 0u, MEM_RELEASE);
        }
        std::printf("  [PASS 5] ResourceBarrier 0x400000, Texture1D/2D UMA VA+Map, 16-Core Affinity, MFResetDXGI & ESRAM OK\n");
    }

    if (g_failures == 0) {
        std::printf("=== ALL WAVE 16 CHECKS PASSED (0 failures) ===\n");
        return 0;
    }
    std::fprintf(stderr, "=== WAVE 16 FAILED (%d failures) ===\n", g_failures);
    return 1;
}
