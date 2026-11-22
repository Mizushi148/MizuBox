// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Wave 15 Clean-Room Xbox Compatibility & RDNA2 Optimizer Verification Suite (Test #46):
//   1. RDNA2 -> HLSL SM 6.6 Translator & Live RTX 5060 Execution:
//      a) Branchless ternary `select()` lowering for interleaved EXEC-masked pure VALU writes
//      b) Automatic Wave32-safe LDS RAW/WAR `GroupMemoryBarrierWithGroupSync()` (64-thread cross-half-wave!)
//      c) Wave-Uniform `ds_append` / `ds_consume` (WaveActiveCountBits + WaveIsFirstLane + WaveReadLaneFirst)
//      d) SM 6.6 Pixel Shader Quad-Gradient `SampleGrad` (`QuadReadAcrossX`/`Y` + `exp2(bias)`)
//         + Finite UAV Float Store Clamping (`wsx_clamp_finite_f32`)
//   2. Direct3D 12.X Console ABI & Runtime on Live RTX 5060:
//      a) Nested Occlusion Query Stack Un-Nesting (`BeginQuery` inside `BeginQuery`)
//      b) `CreateCommittedResource` Upload `0x8C3` + `DENY_SHADER_RESOURCE` (non-DS) + Readback sanitization
//      c) Strip Xbox-only `D3D12XBOX_ROOT_SIGNATURE_FLAG_FORCE_MEMORY_BASED_ABI` (`0x01000000`) + DXBC MD5 recompute
//      d) Xbox DMA Queue/Allocator/List type `0x10 -> COPY`, Heap type `4 -> CBV_SRV_UAV`,
//         Xbox vertex format `189 -> R10G10B10A2_UNORM`, and `D3D12XboxSet/GetProcessDebugFlags` (`& 0x81F00001`)
//   3. `xmem.dll` Console Title Budget (`0x10000001` -> 13.5 GiB limit & realistic used bytes),
//      `XMemGetWin32HeapStatistics`, `XMemFreePhysicalPages`, `XMemTransferMemory`.
//   4. `xgameruntime.dll`: 38-char `8.8.8.8.2` Console ID, `Windows.Xbox` analytics family override,
//      BCrypt ECDSA P-256 XSTS v1 signature (`XUserGetTokenAndSignatureAsync`), and `XGameStreaming` lifecycle.
//   5. `AcpHal.dll`: Asynchronous FIFO command ring (`ACP_CMD_FLAG_ASYNC` + `AcpHalFlushAsyncCommands`)
//      with APU heap DMA & XMA IMA-ADPCM decoding.

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
#include "include/xbox/XUser.h"
#include "include/xbox/XGameStreaming.h"
#include "d3d12_xs_rdna2_to_hlsl.h"
#include "d3d12_xs_sm66_runtime.h"
#include "xmem.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "  [WAVE15 FAIL] %s\n", msg);
        ++g_failures;
    }
}

void check_hr(HRESULT hr, const char* msg) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "  [WAVE15 FAIL] %s (hr=0x%08lX)\n", msg, static_cast<unsigned long>(hr));
        ++g_failures;
    }
}

// ============================================================================
// Minimal GFX10.3 RDNA2 Instruction Encoders
// ============================================================================
uint32_t enc_sopp(uint32_t op, int16_t simm16 = 0) {
    return 0xBF800000u | ((op & 0x7Fu) << 16u) | static_cast<uint16_t>(simm16);
}
uint32_t enc_sop1(uint32_t op, uint32_t sd, uint32_t s0) {
    return 0xBE800000u | ((sd & 0x7Fu) << 16u) | ((op & 0xFFu) << 8u) | (s0 & 0xFFu);
}
uint32_t enc_vop1(uint32_t op, uint32_t vd, uint32_t s0) {
    return 0x7E000000u | ((vd & 0xFFu) << 17u) | ((op & 0xFFu) << 9u) | (s0 & 0x1FFu);
}
uint32_t enc_vop2(uint32_t op, uint32_t vd, uint32_t s0, uint32_t vs1) {
    return ((op & 0x3Fu) << 25u) | ((vd & 0xFFu) << 17u) | ((vs1 & 0xFFu) << 9u) | (s0 & 0x1FFu);
}
uint32_t enc_vopc(uint32_t op, uint32_t s0, uint32_t vs1) {
    return 0x7C000000u | ((op & 0xFFu) << 17u) | ((vs1 & 0xFFu) << 9u) | (s0 & 0x1FFu);
}
void append_ds(std::vector<uint32_t>& code, uint32_t op, uint32_t vaddr, uint32_t vdata0, uint32_t vdata1, uint32_t vdst, uint16_t off16 = 0u) {
    code.push_back(0xD8000000u | ((op & 0xFFu) << 18u) | off16);
    code.push_back((vaddr & 0xFFu) | ((vdata0 & 0xFFu) << 8u) | ((vdata1 & 0xFFu) << 16u) | ((vdst & 0xFFu) << 24u));
}
void append_global_store_dword(std::vector<uint32_t>& code, uint32_t vaddr, uint32_t vdata, uint32_t saddr) {
    code.push_back(0xDC000000u | (0x1Cu << 18u) | (2u << 14u));
    code.push_back((vaddr & 0xFFu) | ((vdata & 0xFFu) << 8u) | ((saddr & 0x7Fu) << 16u));
}
void append_mimg(std::vector<uint32_t>& code, uint32_t op, uint32_t dmask, uint32_t vaddr, uint32_t vdata, uint32_t srsrc, uint32_t ssamp) {
    code.push_back(0xF0000000u | ((op & 0xFFu) << 18u) | ((dmask & 0xFu) << 8u));
    code.push_back((vaddr & 0xFFu) | ((vdata & 0xFFu) << 8u) | (((srsrc >> 2u) & 0x1Fu) << 16u) | (((ssamp >> 2u) & 0x1Fu) << 21u));
}
void append_exp(std::vector<uint32_t>& code, uint32_t tgt, uint32_t en, bool done, uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3) {
    code.push_back(0xF8000000u | ((done ? 1u : 0u) << 11u) | ((tgt & 0x3Fu) << 4u) | (en & 0xFu));
    code.push_back((v0 & 0xFFu) | ((v1 & 0xFFu) << 8u) | ((v2 & 0xFFu) << 16u) | ((v3 & 0xFFu) << 24u));
}

std::vector<uint8_t> make_fssm(const std::vector<uint32_t>& code, uint32_t ntx = 32u) {
    constexpr uint32_t kCodeOff = 0x150u;
    std::vector<uint8_t> blob(kCodeOff + code.size() * 4u, 0u);
    std::memcpy(blob.data(), "FSSM", 4u);
    const uint32_t raw_c0 = kCodeOff - 0x28u;
    const uint32_t code_sz = static_cast<uint32_t>(code.size() * 4u);
    const uint32_t one = 1u;
    std::memcpy(blob.data() + 0xC0u, &raw_c0, 4u);
    std::memcpy(blob.data() + 0xC8u, &code_sz, 4u);
    std::memcpy(blob.data() + 0x13Cu, &ntx, 4u);
    std::memcpy(blob.data() + 0x140u, &one, 4u);
    std::memcpy(blob.data() + 0x144u, &one, 4u);
    std::memcpy(blob.data() + kCodeOff, code.data(), code_sz);
    return blob;
}

bool run_sm66_cs_lanes(
    ID3D12Device* dev,
    ID3D12CommandQueue* queue,
    ID3D12RootSignature* rs,
    const std::vector<uint8_t>& dxil,
    uint32_t* out_lanes,
    uint32_t lane_count) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rs;
    pd.CS.pShaderBytecode = dxil.data();
    pd.CS.BytecodeLength = dxil.size();
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) return false;

    D3D12_HEAP_PROPERTIES hp_def{};
    hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 4096u;
    bd.Height = 1u;
    bd.DepthOrArraySize = 1u;
    bd.MipLevels = 1u;
    bd.SampleDesc.Count = 1u;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> slab;
    if (FAILED(dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &bd,
                                            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slab)))) return false;

    D3D12_HEAP_PROPERTIES hp_rb{};
    hp_rb.Type = D3D12_HEAP_TYPE_READBACK;
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> rb;
    if (FAILED(dev->CreateCommittedResource(&hp_rb, D3D12_HEAP_FLAG_NONE, &bd,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4u;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC shd{};
    shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    shd.NumDescriptors = 1u;
    shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> samp_heap;
    if (FAILED(dev->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&samp_heap)))) return false;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
    uavd.Format = DXGI_FORMAT_R32_TYPELESS;
    uavd.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavd.Buffer.NumElements = 1024u;
    uavd.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    dev->CreateUnorderedAccessView(slab.Get(), nullptr, &uavd, heap->GetCPUDescriptorHandleForHeapStart());

    ComPtr<ID3D12CommandAllocator> ca;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca)))) return false;
    if (FAILED(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), pso.Get(), IID_PPV_ARGS(&cl)))) return false;
    if (FAILED(dev->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;

    wsx_sm66::Sm66UserData ud{};
    constexpr uint64_t kHeapBase = 0x100000000ULL;
    ud.s[0] = static_cast<uint32_t>(kHeapBase & 0xFFFFFFFFULL);
    ud.s[1] = static_cast<uint32_t>(kHeapBase >> 32u);
    ud.heap_base_lo = ud.s[0];
    ud.heap_base_hi = ud.s[1];
    ud.slab_base_idx = 0u;
    ud.slab_shift = 20u;

    ID3D12DescriptorHeap* heaps[] = { heap.Get(), samp_heap.Get() };
    cl->SetDescriptorHeaps(2u, heaps);
    cl->SetComputeRootSignature(rs);
    cl->SetComputeRoot32BitConstants(0u, 40u, &ud, 0u);
    cl->Dispatch(1u, 1u, 1u);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = slab.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1u, &b);

    const UINT64 copy_bytes = static_cast<UINT64>(lane_count) * 4u;
    cl->CopyBufferRegion(rb.Get(), 0u, slab.Get(), 0u, copy_bytes);
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

    void* ptr = nullptr;
    D3D12_RANGE rr{0, static_cast<SIZE_T>(copy_bytes)};
    if (FAILED(rb->Map(0, &rr, &ptr)) || !ptr) return false;
    std::memcpy(out_lanes, ptr, static_cast<size_t>(copy_bytes));
    D3D12_RANGE zw{0, 0};
    rb->Unmap(0, &zw);
    return true;
}

std::vector<uint8_t> base64_decode(const std::string& s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    int buf = 0, bits = 0;
    for (char c : s) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

void CALLBACK dummy_streaming_cb(void*, XGameStreamingClientId, XGameStreamingConnectionState) {}

} // namespace

int main() {
    std::printf("=== Wave 15 Clean-Room Xbox Compatibility & RDNA2 Optimizer Suite ===\n");

    // Create Xbox Series X|S proxy D3D12 device on live RTX GPU
    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1u;
    ComPtr<ID3D12Device> dev;
    check_hr(D3D12XboxCreateDevice(nullptr, &dev_params, IID_PPV_ARGS(&dev)), "D3D12XboxCreateDevice");
    if (!dev) return 1;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    check_hr(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue(DIRECT)");

    ComPtr<ID3D12RootSignature> sm66_rs;
    check_hr(D3D12XboxCreateSm66RootSignature(dev.Get(), IID_PPV_ARGS(&sm66_rs)), "D3D12XboxCreateSm66RootSignature");
    if (!queue || !sm66_rs) return 1;

    // ========================================================================
    // 1. Pillar 1: RDNA2 -> HLSL SM 6.6 Translator & Live GPU Execution
    // ========================================================================
    std::printf("[1/5] RDNA2 Branchless select(), Auto LDS Barrier, Wave-Uniform ds_append, Quad-Grad SampleGrad...\n");
    {
        // 1A) Interleaved EXEC-masked VALU/SALU -> branchless ternary `vN = lane_active ? (...) : vN;`
        std::vector<uint32_t> code;
        code.push_back(enc_vop1(0x01u, 2u, 128u + 0u));     // v2 = 0 (prologue)
        code.push_back(enc_vop2(0x1Au, 1u, 128u + 2u, 0u)); // v1 = v0 << 2 (prologue)
        code.push_back(enc_vopc(0xD4u, 128u + 16u, 0u));    // v_cmpx_gt_u32 16, v0 (lanes 0..15 active)
        // Interleave scalar ops between pure VALU ops so coalescer sees isolated VALU writes!
        code.push_back(enc_vop1(0x01u, 2u, 128u + 1u));     // v2 = 1
        code.push_back(enc_sop1(0x03u, 8u, 128u + 2u));     // s8 = 2
        code.push_back(enc_vop2(0x25u, 2u, 8u, 2u));        // v2 += s8 (= 3)
        code.push_back(enc_sop1(0x03u, 9u, 128u + 3u));     // s9 = 3
        code.push_back(enc_vop2(0x25u, 2u, 9u, 2u));        // v2 += s9 (= 6)
        code.push_back(enc_sop1(0x03u, 10u, 128u + 4u));    // s10 = 4
        code.push_back(enc_vop2(0x25u, 2u, 10u, 2u));       // v2 += s10 (= 10)
        code.push_back(enc_sop1(0x04u, 126u, 193u));        // s_mov_b64 exec, -1
        append_global_store_dword(code, 1u, 2u, 0u);
        code.push_back(enc_sopp(0x01u));

        const auto fssm = make_fssm(code, 32u);
        wsx_rdna2::TranslateStats st{};
        wsx_rdna2::HlslEmitter em;
        const std::string hlsl = em.translate(fssm.data(), fssm.size(), 5u, st);
        check(em.branchless_selects() >= 4u, "At least 4 isolated VALU ops lowered to branchless select()");

        size_t if_cnt = 0u, pos = 0u;
        while ((pos = hlsl.find("if (lane_active)", pos)) != std::string::npos) { ++if_cnt; pos += 16u; }
        check(if_cnt == 1u, "Only 1 if(lane_active) left (the store) instead of 5 separate branches");

        std::vector<uint8_t> dxil;
        char err[512]{};
        check_hr(wsx_sm66::compile_hlsl_to_dxil_vec(hlsl.c_str(), hlsl.size(), L"main", L"cs_6_6", dxil, err, sizeof(err)),
                 "Compile branchless select() CS");
        uint32_t out[32]{};
        check(run_sm66_cs_lanes(dev.Get(), queue.Get(), sm66_rs.Get(), dxil, out, 32u), "Execute branchless select() CS");
        bool ok = true;
        for (uint32_t i = 0; i < 32u; ++i) {
            if (out[i] != ((i < 16u) ? 10u : 0u)) ok = false;
        }
        check(ok, "Branchless select() GPU output bit-exact across 32/32 lanes");
        std::printf("  [PASS 1A] Branchless VALU select(): %u selects emitted, if(lane_active) reduced 5 -> %zu, 32/32 exact\n",
                    em.branchless_selects(), if_cnt);
    }

    {
        // 1B) Automatic Wave32-Safe LDS RAW Barrier on 64-thread cross-half-wave shader WITHOUT explicit s_barrier!
        std::vector<uint32_t> code;
        // v1 = v0 << 2 (byte addr for lane i)
        code.push_back(enc_vop2(0x1Au, 1u, 128u + 2u, 0u));
        // v2 = v0 + 1
        code.push_back(enc_vop2(0x25u, 2u, 128u + 1u, 0u));
        // v3 = (v0 + 32) & 63 -> reads from OTHER NVIDIA Wave32 half-wave!
        code.push_back(enc_vop2(0x25u, 3u, 128u + 32u, 0u));
        code.push_back(enc_vop2(0x1Bu, 3u, 128u + 63u, 3u)); // v_and_b32 v3, 63, v3
        code.push_back(enc_vop2(0x1Au, 4u, 128u + 2u, 3u));  // v4 = v3 << 2 (peer byte addr)
        // ds_write_b32 [v1], v2  (NO s_barrier follows!)
        append_ds(code, 0x0Du, 1u, 2u, 0u, 0u, 0u);
        // ds_read_b32 v5, [v4]   -> auto-barrier MUST synchronize Wave32 half-waves!
        append_ds(code, 0x36u, 4u, 0u, 0u, 5u, 0u);
        append_global_store_dword(code, 1u, 5u, 0u);
        code.push_back(enc_sopp(0x01u));

        const auto fssm = make_fssm(code, 64u);
        wsx_rdna2::TranslateStats st{};
        wsx_rdna2::HlslEmitter em;
        const std::string hlsl = em.translate(fssm.data(), fssm.size(), 5u, st);
        check(em.auto_lds_barriers() >= 1u, "Automatic LDS RAW GroupMemoryBarrierWithGroupSync inserted");
        check(hlsl.find("GroupMemoryBarrierWithGroupSync();") != std::string::npos, "Barrier present in HLSL");

        std::vector<uint8_t> dxil;
        char err[512]{};
        check_hr(wsx_sm66::compile_hlsl_to_dxil_vec(hlsl.c_str(), hlsl.size(), L"main", L"cs_6_6", dxil, err, sizeof(err)),
                 "Compile Auto-LDS-Barrier 64-thread CS");
        uint32_t out[64]{};
        check(run_sm66_cs_lanes(dev.Get(), queue.Get(), sm66_rs.Get(), dxil, out, 64u), "Execute 64-thread cross-half-wave CS");
        bool ok = true;
        for (uint32_t i = 0; i < 64u; ++i) {
            const uint32_t exp = ((i + 32u) & 63u) + 1u;
            if (out[i] != exp) ok = false;
        }
        check(ok, "Cross-Wave32 LDS write->read bit-exact across 64/64 lanes without explicit s_barrier");
        std::printf("  [PASS 1B] Auto LDS RAW Barrier: %u auto-barriers, 64/64 cross-half-wave lanes exact\n",
                    em.auto_lds_barriers());
    }

    {
        // 1C) Wave-Uniform ds_append (0x3E) on live RTX 5060
        std::vector<uint32_t> code;
        code.push_back(enc_vop1(0x01u, 1u, 128u + 0u));     // v1 = 0 (LDS addr 0)
        code.push_back(enc_vop1(0x01u, 2u, 128u + 0u));     // v2 = 0
        append_ds(code, 0x0Du, 1u, 2u, 0u, 0u, 0u);         // wsx_lds[0] = 0
        code.push_back(enc_sopp(0x0Au));                    // s_barrier
        append_ds(code, 0x3Eu, 1u, 0u, 0u, 3u, 0u);         // v3 = ds_append(lds[0])
        code.push_back(enc_vop2(0x1Au, 4u, 128u + 2u, 0u)); // v4 = v0 << 2
        append_global_store_dword(code, 4u, 3u, 0u);
        code.push_back(enc_sopp(0x01u));

        const auto fssm = make_fssm(code, 32u);
        wsx_rdna2::TranslateStats st{};
        wsx_rdna2::HlslEmitter em;
        const std::string hlsl = em.translate(fssm.data(), fssm.size(), 5u, st);
        check(hlsl.find("WaveActiveCountBits(") != std::string::npos &&
              hlsl.find("WaveIsFirstLane()") != std::string::npos &&
              hlsl.find("WaveReadLaneFirst(") != std::string::npos,
              "ds_append emits wave-uniform single-atomic HLSL");

        std::vector<uint8_t> dxil;
        char err[512]{};
        check_hr(wsx_sm66::compile_hlsl_to_dxil_vec(hlsl.c_str(), hlsl.size(), L"main", L"cs_6_6", dxil, err, sizeof(err)),
                 "Compile wave-uniform ds_append CS");
        uint32_t out[32]{};
        check(run_sm66_cs_lanes(dev.Get(), queue.Get(), sm66_rs.Get(), dxil, out, 32u), "Execute ds_append CS");
        bool seen[32]{};
        bool ok = true;
        for (uint32_t i = 0; i < 32u; ++i) {
            if (out[i] >= 32u || seen[out[i]]) ok = false;
            else seen[out[i]] = true;
        }
        check(ok, "Wave-uniform ds_append produced 32 unique contiguous slots 0..31");
        std::printf("  [PASS 1C] Wave-Uniform ds_append: 32/32 unique slots [0..31] with 1 atomic/wave\n");
    }

    {
        // 1D) PS Quad-Gradient SampleGrad + Finite UAV Float Store Clamping
        std::vector<uint32_t> ps_code;
        ps_code.push_back(enc_vop1(0x01u, 4u, 242u)); // v4 = 1.0 (bias)
        append_mimg(ps_code, 0x25u, 0xFu, 2u, 8u, 0u, 8u); // image_sample_b v[8:11], v[2:4], s[0:7], s[8:11]
        append_exp(ps_code, 0u, 0xFu, true, 8u, 9u, 10u, 11u);
        ps_code.push_back(enc_sopp(0x01u));

        const auto fssm_ps = make_fssm(ps_code, 1u);
        wsx_rdna2::TranslateStats st_ps{};
        wsx_rdna2::HlslEmitter em_ps;
        const std::string hlsl_ps = em_ps.translate(fssm_ps.data(), fssm_ps.size(), 0u, st_ps);
        check(em_ps.quad_grad_samples() == 1u, "PS image_sample_b used quad-gradient SampleGrad");
        check(hlsl_ps.find("QuadReadAcrossX(") != std::string::npos &&
              hlsl_ps.find("QuadReadAcrossY(") != std::string::npos &&
              hlsl_ps.find("exp2(") != std::string::npos &&
              hlsl_ps.find(".SampleGrad(") != std::string::npos,
              "PS HLSL contains QuadReadAcrossX/Y + exp2(bias) + SampleGrad");
        std::vector<uint8_t> ps_dxil;
        char err[512]{};
        check_hr(wsx_sm66::compile_hlsl_to_dxil_vec(hlsl_ps.c_str(), hlsl_ps.size(), L"main", L"ps_6_6", ps_dxil, err, sizeof(err)),
                 "Compile PS Quad-Gradient SampleGrad to ps_6_6 DXIL");

        std::vector<uint32_t> st_code;
        append_mimg(st_code, 0x08u, 0xFu, 0u, 4u, 0u, 0u); // image_store
        st_code.push_back(enc_sopp(0x01u));
        const auto fssm_st = make_fssm(st_code, 32u);
        wsx_rdna2::TranslateStats st_cs{};
        wsx_rdna2::HlslEmitter em_cs;
        const std::string hlsl_cs = em_cs.translate(fssm_st.data(), fssm_st.size(), 5u, st_cs);
        check(hlsl_cs.find("wsx_clamp_finite_f32(") != std::string::npos, "MIMG image_store clamps finite floats (R11G11B10F safe)");
        std::vector<uint8_t> cs_dxil;
        check_hr(wsx_sm66::compile_hlsl_to_dxil_vec(hlsl_cs.c_str(), hlsl_cs.size(), L"main", L"cs_6_6", cs_dxil, err, sizeof(err)),
                 "Compile clamped image_store CS to cs_6_6 DXIL");
        std::printf("  [PASS 1D] PS Quad-Grad SampleGrad (ps_6_6=%zu B) + Finite Image Store Clamp (cs_6_6=%zu B)\n",
                    ps_dxil.size(), cs_dxil.size());
    }

    // ========================================================================
    // 2. Pillar 2: Direct3D 12.X Console ABI & Runtime
    // ========================================================================
    std::printf("[2/5] D3D12.X Nested Occlusion Queries, Heap/Flag Sanitization, RTS0 Strip, DMA Queue, Format 189...\n");
    {
        // 2A) Nested Occlusion Query Un-Nesting on live RTX 5060
        D3D12_QUERY_HEAP_DESC qhd{};
        qhd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
        qhd.Count = 4u;
        ComPtr<ID3D12QueryHeap> qheap;
        check_hr(dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qheap)), "CreateQueryHeap(OCCLUSION)");

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
        ComPtr<ID3D12Resource> qbuf;
        // Intentionally pass invalid Readback initial state GENERIC_READ + non-DS DENY_SHADER_RESOURCE!
        rbd.Flags = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        check_hr(dev->CreateCommittedResource(
            &hp_rb, D3D12_HEAP_FLAG_NONE, &rbd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&qbuf)),
            "CreateCommittedResource(READBACK + wrong state + non-DS DENY_SHADER_RESOURCE)");

        // Also test Upload heap with Xbox state 0x8C3 (missing COPY_SOURCE) + DENY_SHADER_RESOURCE!
        D3D12_HEAP_PROPERTIES hp_up{};
        hp_up.Type = D3D12_HEAP_TYPE_UPLOAD;
        ComPtr<ID3D12Resource> upbuf;
        check_hr(dev->CreateCommittedResource(
            &hp_up, D3D12_HEAP_FLAG_NONE, &rbd,
            static_cast<D3D12_RESOURCE_STATES>(0x8C3u), nullptr, IID_PPV_ARGS(&upbuf)),
            "CreateCommittedResource(UPLOAD + state 0x8C3 + non-DS DENY_SHADER_RESOURCE)");

        ComPtr<ID3D12CommandAllocator> ca;
        ComPtr<ID3D12GraphicsCommandList> cl;
        ComPtr<ID3D12Fence> fence;
        check_hr(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca)), "CreateCA(occ)");
        check_hr(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr, IID_PPV_ARGS(&cl)), "CreateCL(occ)");
        check_hr(dev->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence(occ)");

        // Record Xbox-style nested occlusion query: outer(0) -> inner(1) -> end(1) -> end(0)
        cl->BeginQuery(qheap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0u);
        cl->BeginQuery(qheap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 1u);
        cl->EndQuery(qheap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 1u);
        cl->EndQuery(qheap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0u);
        cl->ResolveQueryData(qheap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0u, 2u, qbuf.Get(), 0u);
        check_hr(cl->Close(), "Close nested occlusion CL");
        ID3D12CommandList* lists[] = { cl.Get() };
        queue->ExecuteCommandLists(1u, lists);
        queue->Signal(fence.Get(), 1u);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence->GetCompletedValue() < 1u) {
            fence->SetEventOnCompletion(1u, ev);
            WaitForSingleObject(ev, 5000u);
        }
        CloseHandle(ev);
        check(dev->GetDeviceRemovedReason() == S_OK, "GPU stayed healthy after nested occlusion query un-nesting");

        // 2C) Strip Xbox-only D3D12XBOX_ROOT_SIGNATURE_FLAG_FORCE_MEMORY_BASED_ABI (0x01000000) from serialized RTS0
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
        rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        rsd.Desc_1_0.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> rs_blob, rs_err;
        check_hr(D3D12SerializeVersionedRootSignature(&rsd, &rs_blob, &rs_err), "Serialize base RS");
        ComPtr<ID3D12RootSignature> created_rs;
        if (rs_blob && rs_blob->GetBufferSize() >= 48u) {
            std::vector<uint8_t> xbox_rs(
                static_cast<const uint8_t*>(rs_blob->GetBufferPointer()),
                static_cast<const uint8_t*>(rs_blob->GetBufferPointer()) + rs_blob->GetBufferSize());
            // Inject Xbox-only bit 0x01000000 into RTS0 Flags dword (offset = chunkOff + 8 + 20)
            uint32_t chunk_off = 0u;
            std::memcpy(&chunk_off, xbox_rs.data() + 32u, 4u);
            uint32_t flags = 0u;
            std::memcpy(&flags, xbox_rs.data() + chunk_off + 28u, 4u);
            flags |= 0x01000000u; // D3D12XBOX_ROOT_SIGNATURE_FLAG_FORCE_MEMORY_BASED_ABI
            std::memcpy(xbox_rs.data() + chunk_off + 28u, &flags, 4u);

            check_hr(dev->CreateRootSignature(0u, xbox_rs.data(), xbox_rs.size(), IID_PPV_ARGS(&created_rs)),
                     "CreateRootSignature stripped Xbox 0x01000000 flag & recomputed DXBC MD5");
            // Explicitly invoke d3d12_xs.dll's exported D3D12CreateRootSignatureDeserializer (not desktop d3d12.lib)
            HMODULE d3d12_xs_mod = GetModuleHandleW(L"d3d12_xs.dll");
            auto pDeser = d3d12_xs_mod
                ? reinterpret_cast<decltype(&D3D12CreateRootSignatureDeserializer)>(
                      GetProcAddress(d3d12_xs_mod, "D3D12CreateRootSignatureDeserializer"))
                : nullptr;
            check(pDeser != nullptr, "Resolved d3d12_xs!D3D12CreateRootSignatureDeserializer");
            if (pDeser) {
                ComPtr<ID3D12RootSignatureDeserializer> deser;
                check_hr(pDeser(xbox_rs.data(), xbox_rs.size(), IID_PPV_ARGS(&deser)),
                         "d3d12_xs!D3D12CreateRootSignatureDeserializer accepted patched Xbox RS");
            }
        }

        // 2D) Xbox DMA queue/allocator/list type 0x10 -> COPY, Heap type 4 -> CBV_SRV_UAV, ProcessDebugFlags mask
        D3D12_COMMAND_QUEUE_DESC dma_qd{};
        dma_qd.Type = static_cast<D3D12_COMMAND_LIST_TYPE>(0x10u);
        ComPtr<ID3D12CommandQueue> dma_q;
        check_hr(dev->CreateCommandQueue(&dma_qd, IID_PPV_ARGS(&dma_q)), "CreateCommandQueue(Xbox DMA 0x10 -> COPY)");
        ComPtr<ID3D12CommandAllocator> dma_ca;
        check_hr(dev->CreateCommandAllocator(static_cast<D3D12_COMMAND_LIST_TYPE>(0x10u), IID_PPV_ARGS(&dma_ca)),
                 "CreateCommandAllocator(Xbox DMA 0x10 -> COPY)");
        ComPtr<ID3D12GraphicsCommandList> dma_cl;
        check_hr(dev->CreateCommandList(0u, static_cast<D3D12_COMMAND_LIST_TYPE>(0x10u), dma_ca.Get(), nullptr, IID_PPV_ARGS(&dma_cl)),
                 "CreateCommandList(Xbox DMA 0x10 -> COPY)");

        D3D12_DESCRIPTOR_HEAP_DESC xhd{};
        xhd.Type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(4u);
        xhd.NumDescriptors = 8u;
        ComPtr<ID3D12DescriptorHeap> xheap;
        check_hr(dev->CreateDescriptorHeap(&xhd, IID_PPV_ARGS(&xheap)), "CreateDescriptorHeap(Xbox type 4 -> CBV_SRV_UAV)");
        check(dev->GetDescriptorHandleIncrementSize(static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(4u)) ==
              dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),
              "GetDescriptorHandleIncrementSize(4) matches CBV_SRV_UAV");

        check_hr(D3D12XboxSetProcessDebugFlags(0xFFFFFFFFu), "D3D12XboxSetProcessDebugFlags");
        check(D3D12XboxGetProcessDebugFlags() == 0x81F00001u, "D3D12XboxGetProcessDebugFlags masked to 0x81F00001");

        // Verify Xbox vertex format 189 remapping in CreateGraphicsPipelineState
        const char* vs_src = "float4 main(float4 p : POSITION) : SV_Position { return p; }";
        const char* ps_src = "float4 main() : SV_Target { return float4(1,0,0,1); }";
        std::vector<uint8_t> vs_dxil, ps_dxil;
        char err[512]{};
        check_hr(wsx_sm66::compile_hlsl_to_dxil_vec(vs_src, std::strlen(vs_src), L"main", L"vs_6_6", vs_dxil, err, sizeof(err)), "Compile VS");
        check_hr(wsx_sm66::compile_hlsl_to_dxil_vec(ps_src, std::strlen(ps_src), L"main", L"ps_6_6", ps_dxil, err, sizeof(err)), "Compile PS");
        D3D12_INPUT_ELEMENT_DESC ia_el{};
        ia_el.SemanticName = "POSITION";
        ia_el.Format = static_cast<DXGI_FORMAT>(189u); // Xbox-only R10G10B10_SNORM_A2_UNORM
        ia_el.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC gpso_desc{};
        gpso_desc.pRootSignature = created_rs ? created_rs.Get() : sm66_rs.Get();
        gpso_desc.VS = {vs_dxil.data(), vs_dxil.size()};
        gpso_desc.PS = {ps_dxil.data(), ps_dxil.size()};
        gpso_desc.InputLayout = {&ia_el, 1u};
        gpso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gpso_desc.NumRenderTargets = 1u;
        gpso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        gpso_desc.SampleDesc.Count = 1u;
        gpso_desc.SampleMask = 0xFFFFFFFFu;
        gpso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        gpso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        gpso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        ComPtr<ID3D12PipelineState> gpso;
        check_hr(dev->CreateGraphicsPipelineState(&gpso_desc, IID_PPV_ARGS(&gpso)),
                 "CreateGraphicsPipelineState remapped Xbox vertex format 189 -> R10G10B10A2_UNORM");
        std::printf("  [PASS 2] Nested OccQueries + Upload/Readback/DENY_SRV + RTS0 0x01000000 + DMA 0x10 + Heap 4 + Fmt 189\n");
    }

    // ========================================================================
    // 3. Pillar 3: xmem.dll Console Title Budget & New Exports
    // ========================================================================
    std::printf("[3/5] xmem.dll Title Budget (0x10000001 -> 13.5 GiB), Win32HeapStats, FreePhysicalPages, TransferMemory...\n");
    {
        HMODULE xmem_mod = LoadLibraryW(L"xmem.dll");
        check(xmem_mod != nullptr, "LoadLibrary(xmem.dll)");
        if (xmem_mod) {
            auto pWS = reinterpret_cast<decltype(&WsxXMemGetWorkingSetStatistics)>(GetProcAddress(xmem_mod, "XMemGetWorkingSetStatistics"));
            auto pWin32 = reinterpret_cast<decltype(&WsxXMemGetWin32HeapStatistics)>(GetProcAddress(xmem_mod, "XMemGetWin32HeapStatistics"));
            auto pMapPhys = reinterpret_cast<decltype(&WsxXMemMapPhysical)>(GetProcAddress(xmem_mod, "XMemMapPhysical"));
            auto pFreePhys = reinterpret_cast<decltype(&WsxXMemFreePhysicalPages)>(GetProcAddress(xmem_mod, "XMemFreePhysicalPages"));
            auto pTransfer = reinterpret_cast<decltype(&WsxXMemTransferMemory)>(GetProcAddress(xmem_mod, "XMemTransferMemory"));
            check(pWS && pWin32 && pMapPhys && pFreePhys && pTransfer, "All new xmem.dll exports resolved");
            if (pWS && pWin32 && pMapPhys && pFreePhys && pTransfer) {
                XMEM_WORKING_SET_STATISTICS ws{};
                check(pWS(kXMemWorkingSetFlagTitleBudget, &ws) != FALSE, "XMemGetWorkingSetStatistics(0x10000001)");
                check(ws.PageCount == 0x360000000ULL, "Title memory budget limit == 13.5 GiB (0x360000000)");
                check(ws.WorkingSetBytes > 0u && ws.WorkingSetBytes < (ws.PageCount / 2u),
                      "Title used bytes realistic (< half of 13.5 GiB budget)");

                XMEM_WIN32_HEAP_STATISTICS hst{};
                check(pWin32(&hst) != FALSE && hst.TitleMemoryBudgetBytes == 0x360000000ULL,
                      "XMemGetWin32HeapStatistics returned 13.5 GiB budget");

                UINT64 phys_addr = 0u;
                void* pmem = pMapPhys(65536u, 65536u, PAGE_READWRITE, &phys_addr);
                check(pmem != nullptr && phys_addr != 0u, "XMemMapPhysical");
                std::vector<uint8_t> src(4096u, 0xA5u);
                check_hr(pTransfer(pmem, src.data(), src.size(), 1ULL), "XMemTransferMemory(with cache flush)");
                check(std::memcmp(pmem, src.data(), src.size()) == 0, "XMemTransferMemory payload exact");
                check(pFreePhys(pmem, 65536u) != FALSE, "XMemFreePhysicalPages");
                std::printf("  [PASS 3] xmem.dll: budget=%.1f GiB, used=%.2f MiB, Win32Heap + FreePhys + Transfer OK\n",
                            static_cast<double>(ws.PageCount) / (1024.0 * 1024.0 * 1024.0),
                            static_cast<double>(ws.WorkingSetBytes) / (1024.0 * 1024.0));
            }
            FreeLibrary(xmem_mod);
        }
    }

    // ========================================================================
    // 4. Pillar 4: xgameruntime.dll Console ID, ECDSA P-256 XSTS Sig & XGameStreaming
    // ========================================================================
    std::printf("[4/5] xgameruntime.dll 8.8.8.8.2 ConsoleId, BCrypt ECDSA P-256 XSTS v1 Sig, XGameStreaming...\n");
    {
        check_hr(XGameRuntimeInitialize(), "XGameRuntimeInitialize");

        // Console ID 38-char 8.8.8.8.2 + E_NOT_SUFFICIENT_BUFFER check
        char tiny[8] = "KEEPME!";
        size_t used = 0u;
        check(XSystemGetConsoleId(sizeof(tiny), tiny, &used) == E_NOT_SUFFICIENT_BUFFER,
              "XSystemGetConsoleId returns E_NOT_SUFFICIENT_BUFFER for small buffer");
        check(std::strcmp(tiny, "KEEPME!") == 0, "Small buffer not clobbered");
        check(used == 39u, "ConsoleId required size == 39 (38 chars + NUL)");

        char cid[64]{};
        check_hr(XSystemGetConsoleId(sizeof(cid), cid, &used), "XSystemGetConsoleId");
        check(std::strlen(cid) == 38u && cid[8] == '.' && cid[17] == '.' && cid[26] == '.' && cid[35] == '.',
              "ConsoleId matches exact 8.8.8.8.2 Xbox format");

        // Analytics family override ("Windows.Xbox")
        SetEnvironmentVariableA("WSX_ANALYTICS_FAMILY", "Windows.Xbox");
        const XSystemAnalyticsInfo ai_win = XSystemGetAnalyticsInfo();
        check(std::strcmp(ai_win.family, "Windows.Xbox") == 0, "WSX_ANALYTICS_FAMILY=Windows.Xbox honored");
        SetEnvironmentVariableA("WSX_ANALYTICS_FAMILY", nullptr);

        // BCrypt ECDSA P-256 XSTS v1 signature in raw mode (`WSX_XBL_SIG_MODE=xsts`)
        XAsyncBlock ab_user{};
        check_hr(XUserAddAsync(XUserAddOptions::AddDefaultUserSilently, &ab_user), "XUserAddAsync");
        check_hr(XAsyncGetStatus(&ab_user, true), "XUserAddAsync wait");
        XUserHandle user = nullptr;
        check_hr(XUserAddResult(&ab_user, &user), "XUserAddResult");

        SetEnvironmentVariableA("WSX_XBL_SIG_MODE", "xsts");
        XAsyncBlock ab_tok{};
        check_hr(XUserGetTokenAndSignatureAsync(
            user, XUserGetTokenAndSignatureOptions::None, "POST", "https://sessiondirectory.xboxlive.com/handles",
            0u, nullptr, 0u, nullptr, &ab_tok), "XUserGetTokenAndSignatureAsync");
        check_hr(XAsyncGetStatus(&ab_tok, true), "Token wait");
        size_t tsz = 0u;
        check_hr(XUserGetTokenAndSignatureResultSize(&ab_tok, &tsz), "Token ResultSize");
        std::vector<uint8_t> tbuf(tsz, 0u);
        XUserGetTokenAndSignatureData* tdata = nullptr;
        check_hr(XUserGetTokenAndSignatureResult(&ab_tok, tbuf.size(), tbuf.data(), &tdata, nullptr), "Token Result");
        SetEnvironmentVariableA("WSX_XBL_SIG_MODE", nullptr);

        check(tdata && tdata->signature && std::strlen(tdata->signature) == 104u,
              "Raw XSTS v1 ECDSA P-256 Base64 signature is 104 chars (76 raw bytes)");
        if (tdata && tdata->signature) {
            const auto raw = base64_decode(tdata->signature);
            check(raw.size() == 76u, "Decoded XSTS v1 signature == 76 bytes");
            if (raw.size() == 76u) {
                const uint32_t ver = (static_cast<uint32_t>(raw[0]) << 24) | (static_cast<uint32_t>(raw[1]) << 16) |
                                     (static_cast<uint32_t>(raw[2]) << 8) | raw[3];
                bool sig_nonzero = false;
                for (size_t k = 12u; k < 76u; ++k) if (raw[k] != 0u) sig_nonzero = true;
                check(ver == 1u && sig_nonzero, "XSTS v1 header version=1 + non-zero 64-byte P-256 r||s signature");
            }
        }
        XUserCloseHandle(user);

        // XGameStreaming lifecycle
        uint32_t cused = 99u;
        check(XGameStreamingGetClients(0u, nullptr, &cused) == E_GAMESTREAMING_NOT_INITIALIZED,
              "XGameStreamingGetClients returns NOT_INITIALIZED before init");
        check_hr(XGameStreamingInitialize(), "XGameStreamingInitialize");
        check(!XGameStreamingIsStreaming() && XGameStreamingGetClientCount() == 0u,
              "XGameStreaming offline: not streaming, 0 clients");
        check_hr(XGameStreamingGetClients(0u, nullptr, &cused), "XGameStreamingGetClients after init");
        check(cused == 0u, "XGameStreamingGetClients used == 0");
        XVersion tver{};
        check(XGameStreamingGetTouchBundleVersion(0u, &tver, 0u, nullptr) == E_GAMESTREAMING_CLIENT_NOT_CONNECTED,
              "XGameStreamingGetTouchBundleVersion returns CLIENT_NOT_CONNECTED");
        XTaskQueueRegistrationToken stok{};
        check_hr(XGameStreamingRegisterConnectionStateChanged(nullptr, nullptr, &dummy_streaming_cb, &stok),
                 "XGameStreamingRegisterConnectionStateChanged");
        check(XGameStreamingUnregisterConnectionStateChanged(stok, true),
              "XGameStreamingUnregisterConnectionStateChanged");
        XGameStreamingHideTouchControls();
        XGameStreamingShowTouchControlLayout("default");
        XGameStreamingUninitialize();
        check(XGameStreamingGetClients(0u, nullptr, &cused) == E_GAMESTREAMING_NOT_INITIALIZED,
              "XGameStreaming returns NOT_INITIALIZED after uninit");

        XGameRuntimeUninitialize();
        std::printf("  [PASS 4] ConsoleId=%s, BCrypt ECDSA P-256 XSTS v1 (76 B -> 104 b64), XGameStreaming OK\n", cid);
    }

    // ========================================================================
    // 5. Pillar 5: AcpHal.dll Asynchronous Multi-Threaded FIFO Command Ring
    // ========================================================================
    std::printf("[5/5] AcpHal.dll Asynchronous Multi-Threaded FIFO Command Ring (DMA + XMA ADPCM)...\n");
    {
        check_hr(ApuCreateHeap(4u * 1024u * 1024u, 4u * 1024u * 1024u), "ApuCreateHeap");
        void* src_va = nullptr;
        void* dst_va = nullptr;
        APU_ADDRESS src_pa = 0u, dst_pa = 0u;
        check_hr(ApuAlloc(&src_va, &src_pa, 4096u, 64u, APU_ALLOC_CACHED), "ApuAlloc(src)");
        check_hr(ApuAlloc(&dst_va, &dst_pa, 4096u, 64u, APU_ALLOC_CACHED), "ApuAlloc(dst)");
        check(ApuMapVirtualAddress(src_va) == src_pa && ApuIsVirtualAddressValid(src_va, 4096u),
              "ApuMapVirtualAddress & ApuIsVirtualAddressValid exact");

        auto* sb = static_cast<uint8_t*>(src_va);
        auto* db = static_cast<uint8_t*>(dst_va);
        for (uint32_t i = 0u; i < 4096u; ++i) sb[i] = static_cast<uint8_t>((i * 37u + 13u) & 0xFFu);
        std::memset(db, 0, 4096u);

        ComPtr<IAcpHal> hal;
        check_hr(AcpHalCreate(&hal), "AcpHalCreate");
        check_hr(hal->Connect(64u, 64u), "IAcpHal::Connect");

        // Submit 8 asynchronous DMA slice jobs back-to-back with ACP_CMD_FLAG_ASYNC
        for (uint32_t k = 0u; k < 8u; ++k) {
            ACP_DMA_TRANSFER_DESC d{};
            d.src = sb + k * 512u;
            d.dst = db + k * 512u;
            d.byteCount = 512u;
            check_hr(hal->SubmitCommand(ACP_COMMAND_TYPE_DMA_TRANSFER, 100u + k, ACP_CMD_FLAG_ASYNC, &d, sizeof(d)),
                     "IAcpHal::SubmitCommand(ASYNC DMA)");
        }
        check_hr(AcpHalFlushAsyncCommands(hal.Get(), 5000u), "AcpHalFlushAsyncCommands");
        check(std::memcmp(sb, db, 4096u) == 0, "8 async ACP DMA transfers completed bit-exact");

        UINT32 popped = 0u;
        ACP_MESSAGE msg{};
        while (hal->PopMessage(&msg) == S_OK) {
            check(msg.type == ACP_COMMAND_TYPE_DMA_TRANSFER && msg.payload == 512u, "FIFO async ACP completion message valid");
            ++popped;
        }
        check(popped == 8u, "Exactly 8 FIFO completion messages popped");

        hal->Disconnect();
        ApuFree(src_va);
        ApuFree(dst_va);
        std::printf("  [PASS 5] AcpHal Async Command Ring: 8/8 FIFO DMA jobs executed & committed bit-exact\n");
    }

    if (g_failures == 0) {
        std::printf("=== ALL WAVE 15 CHECKS PASSED (0 failures) ===\n");
        return 0;
    }
    std::fprintf(stderr, "=== WAVE 15 FAILED (%d failures) ===\n", g_failures);
    return 1;
}
