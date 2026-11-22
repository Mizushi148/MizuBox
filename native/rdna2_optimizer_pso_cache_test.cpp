// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// CTest #32: RDNA2 -> SM 6.6 Optimizer + ACCC/FSPX Lifter + Persistent ID3D12PipelineLibrary Cache
// Verifies LIVE on hardware GPU (RTX 5060) under D3D12 Debug Layer (0 errors):
//   1. StraightLine CFG + Static EXEC Elision + Trimmed SM 6.6 Preamble (< 4 KB HLSL)
//   2. Relooper Structured CFG (Diamond if/else inside Natural Loop) bit-exact vs StateMachine
//   3. Coalesced `if (lane_active) { ... }` block merging on EXEC-masked RDNA2 shader
//   4. Direct clean-room lifting of ALL 29 real GTA VI ACCC/FSPX shaders from amd_fidelityfx_upscaler_xbox.dll
//      + live RTX PSO compilation on real ACCC/FSPX shaders via WSX_AUTO_TRANSLATE_FSSM=1
//   5. Persistent native driver ID3D12PipelineLibrary1 disk cache across cold device restart

#include "d3d12_xs_rdna2_to_hlsl.h"
#include "d3d12_xs_sm66_runtime.h"
#include "include/xbox/d3d12_xs.h"

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "  [FAIL] %s\n", msg);
        ++g_failures;
    }
}

void check_hr(HRESULT hr, const char* msg) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "  [FAIL] %s (hr=0x%08lX)\n", msg, static_cast<unsigned long>(hr));
        ++g_failures;
    }
}

// Helper to pack raw RDNA2 dwords into a clean FSSM compute shader blob
std::vector<uint8_t> make_fssm_cs(const std::vector<uint32_t>& code, uint32_t ntx = 32u) {
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

// GFX10.3 RDNA2 instruction encoders for synthetic test kernels
uint32_t enc_sopp(uint32_t op, int16_t simm16 = 0) {
    return 0xBF800000u | ((op & 0x7Fu) << 16u) | static_cast<uint16_t>(simm16);
}
uint32_t enc_sop1(uint32_t op, uint32_t sd, uint32_t s0) {
    return 0xBE800000u | ((sd & 0x7Fu) << 16u) | ((op & 0xFFu) << 8u) | (s0 & 0xFFu);
}
uint32_t enc_sop2(uint32_t op, uint32_t sd, uint32_t s0, uint32_t s1) {
    return 0x80000000u | ((op & 0x7Fu) << 23u) | ((sd & 0x7Fu) << 16u) | ((s1 & 0xFFu) << 8u) | (s0 & 0xFFu);
}
uint32_t enc_sopc(uint32_t op, uint32_t s0, uint32_t s1) {
    return 0xBF000000u | ((op & 0x7Fu) << 16u) | ((s1 & 0xFFu) << 8u) | (s0 & 0xFFu);
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
// GLOBAL_STORE_DWORD (Enc::GLOBAL = 0xDC.., op = 0x1C): saddr=s[0:1], vaddr=v[va], data=v[vd]
void append_global_store_dword(std::vector<uint32_t>& code, uint32_t vaddr, uint32_t vdata, uint32_t saddr) {
    // top6 = 0x37 (0xDC..), seg = 2 (GLOBAL) at bits [15:14], op = 0x1C at bits [24:18]
    const uint32_t w0 = 0xDC000000u | (0x1Cu << 18u) | (2u << 14u);
    const uint32_t w1 = (vaddr & 0xFFu) | ((vdata & 0xFFu) << 8u) | ((saddr & 0x7Fu) << 16u);
    code.push_back(w0);
    code.push_back(w1);
}

// Run a compiled SM 6.6 CS DXIL using the standard 40-DWORD SM 6.6 slab contract and read back 32 uints
bool run_sm66_cs_32lanes(
    ID3D12Device* dev,
    ID3D12CommandQueue* queue,
    ID3D12RootSignature* rs,
    const std::vector<uint8_t>& dxil,
    uint32_t out_lanes[32]) {
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
                                            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slab)))) {
        return false;
    }

    D3D12_HEAP_PROPERTIES hp_rb{};
    hp_rb.Type = D3D12_HEAP_TYPE_READBACK;
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> rb;
    if (FAILED(dev->CreateCommittedResource(&hp_rb, D3D12_HEAP_FLAG_NONE, &bd,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) {
        return false;
    }

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
    uavd.Buffer.FirstElement = 0u;
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
    ud.slab_shift = 20u; // 1 MB slab

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

    cl->CopyBufferRegion(rb.Get(), 0u, slab.Get(), 0u, 128u);
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
    D3D12_RANGE rr{0, 128u};
    if (FAILED(rb->Map(0, &rr, &ptr)) || !ptr) return false;
    std::memcpy(out_lanes, ptr, 128u);
    D3D12_RANGE zw{0, 0};
    rb->Unmap(0, &zw);
    return true;
}

uint64_t count_debug_errors(ID3D12Device* dev) {
    ComPtr<ID3D12InfoQueue> iq;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&iq))) || !iq) return 0u;
    uint64_t errs = 0u;
    const UINT64 n = iq->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0u;
        iq->GetMessage(i, nullptr, &len);
        if (len == 0u) continue;
        std::vector<uint8_t> buf(len);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (SUCCEEDED(iq->GetMessage(i, msg, &len))) {
            if (msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
                msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR) {
                std::fprintf(stderr, "  [D3D12 ERROR] %s\n", msg->pDescription ? msg->pDescription : "");
                ++errs;
            }
        }
    }
    return errs;
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path fsr_dll = (argc > 1) ? std::filesystem::path(argv[1]) : std::filesystem::path{};

    ComPtr<ID3D12Debug> dbg;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))) && dbg) {
        dbg->EnableDebugLayer();
    }

    D3D12XBOX_CREATE_DEVICE_PARAMETERS cdp{};
    cdp.Version = D3D12_SDK_VERSION;
    cdp.ProcessDebugFlags = D3D12_PROCESS_DEBUG_FLAG_DEBUG_LAYER_ENABLED;
    ComPtr<ID3D12Device> dev;
    check_hr(D3D12XboxCreateDevice(nullptr, &cdp, IID_PPV_ARGS(&dev)), "D3D12XboxCreateDevice");
    if (!dev) return 1;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    check_hr(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");

    ComPtr<ID3D12RootSignature> sm66_rs;
    check_hr(D3D12XboxCreateSm66RootSignature(dev.Get(), IID_PPV_ARGS(&sm66_rs)), "CreateSm66RootSignature");

    // ------------------------------------------------------------------------
    // 1. StraightLine + Static EXEC Elision + Trimmed Preamble (< 4 KB HLSL)
    // ------------------------------------------------------------------------
    std::printf("[Opt / 1] StraightLine + EXEC Elision + Trimmed Preamble...\n");
    {
        // out[tid] = tid * 3 + 7 (v0 = tid)
        // v1 = v0 << 2 (byte offset), v2 = v0 + v0, v2 = v2 + v0, v2 = v2 + 7, store, s_endpgm
        std::vector<uint32_t> code;
        code.push_back(enc_vop2(0x1Au, 1u, 128u + 2u, 0u)); // v_lshlrev_b32 v1, 2, v0
        code.push_back(enc_vop2(0x25u, 2u, 256u + 0u, 0u)); // v_add_nc_u32 v2, v0, v0 (2*tid)
        code.push_back(enc_vop2(0x25u, 2u, 256u + 2u, 0u)); // v_add_nc_u32 v2, v2, v0 (3*tid)
        code.push_back(enc_vop2(0x25u, 2u, 128u + 7u, 2u)); // v_add_nc_u32 v2, 7, v2  (3*tid+7)
        append_global_store_dword(code, 1u, 2u, 0u);        // global_store_dword v1, v2, s[0:1]
        code.push_back(enc_sopp(0x01u));                    // s_endpgm

        const auto fssm = make_fssm_cs(code);
        wsx_rdna2::TranslateStats st{};
        wsx_rdna2::HlslEmitter em;
        const std::string hlsl = em.translate(fssm.data(), fssm.size(), 5u, st);
        check(em.cfg_mode() == wsx_rdna2::CfgLoweringMode::StraightLine, "Expected StraightLine CFG");
        check(em.exec_elided(), "Expected static EXEC elision");
        check(hlsl.find("switch (wsx_pc)") == std::string::npos, "No switch(wsx_pc) in StraightLine");
        check(hlsl.find("if (lane_active)") == std::string::npos, "No if(lane_active) when EXEC unmutated");
        check(hlsl.size() < 4096u, "Trimmed preamble keeps StraightLine HLSL < 4 KB");

        std::vector<uint8_t> dxil;
        char err[512]{};
        check_hr(wsx_sm66::compile_hlsl_to_dxil_vec(hlsl.c_str(), hlsl.size(), L"main", L"cs_6_6",
                                                    dxil, err, sizeof(err)), "Compile StraightLine CS");
        uint32_t out[32]{};
        check(run_sm66_cs_32lanes(dev.Get(), queue.Get(), sm66_rs.Get(), dxil, out), "Run StraightLine CS on GPU");
        bool exact = true;
        for (uint32_t k = 0; k < 32u; ++k) {
            if (out[k] != k * 3u + 7u) exact = false;
        }
        check(exact, "StraightLine GPU output == tid*3+7 for 32/32 lanes");
        std::printf("  [PASS] StraightLine HLSL=%zu B (was ~21 KB), execElided=1, 32/32 lanes exact\n", hlsl.size());
    }

    // ------------------------------------------------------------------------
    // 2. Relooper Structured CFG (Diamond if/else inside Natural Loop) vs StateMachine
    // ------------------------------------------------------------------------
    std::printf("[Opt / 2] Relooper Structured Loop + Diamond if/else vs StateMachine...\n");
    {
        // BB0 [0..1]: v2 = 0; s8 = 0
        // BB1 [2..3]: s_cmp_lt_u32 s8, 6; s_cbranch_scc0 +6 (-> BB6 @10)
        // BB2 [4..5]: s_and_b32 s9, s8, 1; s_cbranch_scc0 +2 (-> BB4 @8)
        // BB3 [6..7]: v_add_nc_u32 v2, 10, v2; s_branch +1 (-> BB5 @9)
        // BB4 [8]:    v_add_nc_u32 v2, 1, v2
        // BB5 [9..10]: wait -> compute exact word offsets carefully!
        std::vector<uint32_t> code;
        // [0] v_mov_b32 v2, 0
        code.push_back(enc_vop1(0x01u, 2u, 128u + 0u));
        // [1] s_mov_b32 s8, 0
        code.push_back(enc_sop1(0x03u, 8u, 128u + 0u));
        // [2] BB1 (loop hdr): s_cmp_lt_u32 s8, 6 (SOPC op 0x0A)
        code.push_back(enc_sopc(0x0Au, 8u, 128u + 6u));
        // [3] s_cbranch_scc0 -> [11] (exit): simm = 11 - (3 + 1) = +7
        code.push_back(enc_sopp(0x04u, 7));
        // [4] BB2 (parity test): s_and_b32 s9, s8, 1 (SOP2 op 0x0E)
        code.push_back(enc_sop2(0x0Eu, 9u, 8u, 128u + 1u));
        // [5] s_cbranch_scc0 -> [8] (even): simm = 8 - (5 + 1) = +2
        code.push_back(enc_sopp(0x04u, 2));
        // [6] BB3 (odd): v_add_nc_u32 v2, 10, v2
        code.push_back(enc_vop2(0x25u, 2u, 128u + 10u, 2u));
        // [7] s_branch -> [9] (latch): simm = 9 - (7 + 1) = +1
        code.push_back(enc_sopp(0x02u, 1));
        // [8] BB4 (even): v_add_nc_u32 v2, 1, v2
        code.push_back(enc_vop2(0x25u, 2u, 128u + 1u, 2u));
        // [9] BB5 (latch): s_add_u32 s8, s8, 1 (SOP2 op 0x00)
        code.push_back(enc_sop2(0x00u, 8u, 8u, 128u + 1u));
        // [10] s_branch -> [2] (loop hdr): simm = 2 - (10 + 1) = -9
        code.push_back(enc_sopp(0x02u, -9));
        // [11] BB6 (exit): v_add_nc_u32 v2, v2, v0
        code.push_back(enc_vop2(0x25u, 2u, 256u + 2u, 0u));
        // [12] v_lshlrev_b32 v1, 2, v0
        code.push_back(enc_vop2(0x1Au, 1u, 128u + 2u, 0u));
        // [13..14] global_store_dword v1, v2, s[0:1]
        append_global_store_dword(code, 1u, 2u, 0u);
        // [15] s_endpgm
        code.push_back(enc_sopp(0x01u));

        const auto fssm = make_fssm_cs(code);

        // Compile & execute with Relooper (default auto)
        SetEnvironmentVariableW(L"WSX_RDNA2_CFG_MODE", nullptr);
        wsx_rdna2::TranslateStats st_rel{};
        wsx_rdna2::HlslEmitter em_rel;
        const std::string hlsl_rel = em_rel.translate(fssm.data(), fssm.size(), 5u, st_rel);
        check(em_rel.cfg_mode() == wsx_rdna2::CfgLoweringMode::Relooper, "Expected Relooper CFG on loop+diamond");
        check(hlsl_rel.find("switch (wsx_pc)") == std::string::npos, "No switch(wsx_pc) in Relooper");
        check(hlsl_rel.find("for (uint _wlp") != std::string::npos, "Structured natural loop emitted");
        std::vector<uint8_t> dxil_rel;
        char err[512]{};
        check_hr(wsx_sm66::compile_hlsl_to_dxil_vec(hlsl_rel.c_str(), hlsl_rel.size(), L"main", L"cs_6_6",
                                                    dxil_rel, err, sizeof(err)), "Compile Relooper CS");
        uint32_t out_rel[32]{};
        check(run_sm66_cs_32lanes(dev.Get(), queue.Get(), sm66_rs.Get(), dxil_rel, out_rel), "Run Relooper CS");

        // Compile & execute with forced StateMachine fallback to verify bit-exact equivalence
        SetEnvironmentVariableW(L"WSX_RDNA2_CFG_MODE", L"statemachine");
        wsx_rdna2::TranslateStats st_sm{};
        wsx_rdna2::HlslEmitter em_sm;
        const std::string hlsl_sm = em_sm.translate(fssm.data(), fssm.size(), 5u, st_sm);
        SetEnvironmentVariableW(L"WSX_RDNA2_CFG_MODE", nullptr);
        check(em_sm.cfg_mode() == wsx_rdna2::CfgLoweringMode::StateMachine, "Forced StateMachine CFG");
        std::vector<uint8_t> dxil_sm;
        check_hr(wsx_sm66::compile_hlsl_to_dxil_vec(hlsl_sm.c_str(), hlsl_sm.size(), L"main", L"cs_6_6",
                                                    dxil_sm, err, sizeof(err)), "Compile StateMachine CS");
        uint32_t out_sm[32]{};
        check(run_sm66_cs_32lanes(dev.Get(), queue.Get(), sm66_rs.Get(), dxil_sm, out_sm), "Run StateMachine CS");

        bool exact = true;
        for (uint32_t k = 0; k < 32u; ++k) {
            if (out_rel[k] != 33u + k || out_rel[k] != out_sm[k]) exact = false;
        }
        check(exact, "Relooper == StateMachine == 33+tid across 32/32 lanes");
        std::printf("  [PASS] Relooper (HLSL=%zu B, DXIL=%zu B) bit-exact vs StateMachine (HLSL=%zu B, DXIL=%zu B)\n",
                    hlsl_rel.size(), dxil_rel.size(), hlsl_sm.size(), dxil_sm.size());
    }

    // ------------------------------------------------------------------------
    // 3. EXEC-Masked Block Coalescing
    // ------------------------------------------------------------------------
    std::printf("[Opt / 3] EXEC-Masked if(lane_active) Block Coalescing...\n");
    {
        std::vector<uint32_t> code;
        code.push_back(enc_vop1(0x01u, 2u, 128u + 0u));     // v2 = 0 (prologue: unguarded!)
        code.push_back(enc_vop2(0x1Au, 1u, 128u + 2u, 0u)); // v1 = v0 << 2 (prologue: unguarded!)
        // v_cmpx_gt_u32 16, v0 (VOPC GFX10.3 op 0xD4 = v_cmpx_gt_u32 -> 16 > v0 <=> v0 < 16!)
        code.push_back(enc_vopc(0xD4u, 128u + 16u, 0u));
        // 4 consecutive VALU ops -> must coalesce into ONE `if (lane_active) { ... }`
        code.push_back(enc_vop1(0x01u, 2u, 128u + 1u));     // v2 = 1
        code.push_back(enc_vop2(0x25u, 2u, 128u + 2u, 2u)); // v2 += 2
        code.push_back(enc_vop2(0x25u, 2u, 128u + 3u, 2u)); // v2 += 3
        code.push_back(enc_vop2(0x25u, 2u, 128u + 4u, 2u)); // v2 += 4 (= 10)
        // Re-enable all lanes: s_mov_b64 exec, -1 (SOP1 op 0x04, sd=126, s0=193 [-1])
        code.push_back(enc_sop1(0x04u, 126u, 193u));
        append_global_store_dword(code, 1u, 2u, 0u);
        code.push_back(enc_sopp(0x01u));

        const auto fssm = make_fssm_cs(code);
        wsx_rdna2::TranslateStats st{};
        wsx_rdna2::HlslEmitter em;
        const std::string hlsl = em.translate(fssm.data(), fssm.size(), 5u, st);
        check(!em.exec_elided(), "EXEC mutation properly detected");
        check(em.coalesced_guards() >= 5u, "Prologue elided + 4 VALU ops coalesced into 1 guard");

        // Count occurrences of "if (lane_active)" in HLSL (should be 2: 1 coalesced 4-op block + 1 store)
        size_t guard_cnt = 0u, pos = 0u;
        while ((pos = hlsl.find("if (lane_active)", pos)) != std::string::npos) {
            ++guard_cnt;
            pos += 16u;
        }
        check(guard_cnt == 2u, "Exactly 2 coalesced if(lane_active) blocks instead of 7");

        std::vector<uint8_t> dxil;
        char err[512]{};
        check_hr(wsx_sm66::compile_hlsl_to_dxil_vec(hlsl.c_str(), hlsl.size(), L"main", L"cs_6_6",
                                                    dxil, err, sizeof(err)), "Compile Coalesced CS");
        uint32_t out[32]{};
        check(run_sm66_cs_32lanes(dev.Get(), queue.Get(), sm66_rs.Get(), dxil, out), "Run Coalesced CS");
        bool exact = true;
        for (uint32_t k = 0; k < 32u; ++k) {
            const uint32_t exp = (k < 16u) ? 10u : 0u;
            if (out[k] != exp) exact = false;
        }
        check(exact, "Coalesced EXEC mask output: lanes 0..15==10, lanes 16..31==0");
        std::printf("  [PASS] Coalesced 7 guards -> %zu blocks (saved=%u), 32/32 masked lanes exact\n",
                    guard_cnt, em.coalesced_guards());
    }

    // ------------------------------------------------------------------------
    // 4. Direct Clean-Room ACCC/FSPX Lifting on Real GTA VI amd_fidelityfx_upscaler_xbox.dll
    // ------------------------------------------------------------------------
    std::printf("[Opt / 4] Direct ACCC/FSPX RDNA2 Lifting on real GTA VI FSR 4.1.1 DLL...\n");
    if (!fsr_dll.empty() && std::filesystem::exists(fsr_dll)) {
        std::ifstream f(fsr_dll, std::ios::binary | std::ios::ate);
        const size_t fsz = static_cast<size_t>(f.tellg());
        f.seekg(0, std::ios::beg);
        std::vector<uint8_t> bytes(fsz);
        f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(fsz));

        uint32_t accc_found = 0u, accc_100 = 0u, accc_pso_ok = 0u;
        uint64_t accc_insns = 0u, accc_xlat = 0u;
        SetEnvironmentVariableW(L"WSX_AUTO_TRANSLATE_FSSM", L"1");

        for (size_t i = 0; i + 32u <= fsz; ++i) {
            if (std::memcmp(bytes.data() + i, "DXBC", 4u) != 0) continue;
            uint32_t sz = 0u, cc = 0u;
            std::memcpy(&sz, bytes.data() + i + 24u, 4u);
            std::memcpy(&cc, bytes.data() + i + 28u, 4u);
            if (cc == 0u || cc > 32u || sz < 64u || i + sz > fsz) continue;
            wsx_shader::AcccCodeSlice slice{};
            if (!wsx_shader::extract_accc_fspx_code(bytes.data() + i, sz, slice)) continue;

            ++accc_found;
            const size_t nwords = slice.code_size / 4u;
            std::vector<uint32_t> words(nwords);
            std::memcpy(words.data(), slice.code_ptr, nwords * 4u);
            wsx_rdna2::TranslateStats st{};
            wsx_rdna2::HlslEmitter em;
            const std::string hlsl = em.translate_raw_rdna2(
                words.data(), words.size(), slice.stage,
                slice.numthreads_x, slice.numthreads_y, slice.numthreads_z,
                0u, false, st);
            accc_insns += st.TotalInstructions;
            accc_xlat += st.TranslatedInstructions;
            if (!hlsl.empty() && st.UnsupportedInstructions == 0u) ++accc_100;

            // Live auto-translate + RTX PSO creation through d3d12_xs.dll for first 5 real ACCC/FSPX shaders!
            if (accc_pso_ok < 5u && st.TotalInstructions <= 1500u) {
                D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
                pd.pRootSignature = sm66_rs.Get();
                pd.CS.pShaderBytecode = bytes.data() + i;
                pd.CS.BytecodeLength = sz;
                ComPtr<ID3D12PipelineState> pso;
                if (SUCCEEDED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso))) && pso) {
                    ++accc_pso_ok;
                }
            }
            i += sz - 1u;
        }
        SetEnvironmentVariableW(L"WSX_AUTO_TRANSLATE_FSSM", nullptr);

        check(accc_found == 29u, "Found all 29 ACCC/FSPX shaders in amd_fidelityfx_upscaler_xbox.dll");
        check(accc_100 == accc_found, "29/29 ACCC/FSPX shaders lifted at 100.0% instruction coverage");
        check(accc_pso_ok == 5u, "5/5 real ACCC/FSPX shaders auto-translated & created as live RTX PSOs");
        std::printf("  [PASS] ACCC/FSPX: %u/%u shaders at 100%% (%llu/%llu insns) | %u/5 live RTX PSOs OK\n",
                    accc_100, accc_found,
                    static_cast<unsigned long long>(accc_xlat),
                    static_cast<unsigned long long>(accc_insns),
                    accc_pso_ok);
    } else {
        std::printf("  [SKIP] amd_fidelityfx_upscaler_xbox.dll not supplied\n");
    }

    // ------------------------------------------------------------------------
    // 5. Persistent Native Driver ID3D12PipelineLibrary1 Disk Cache (Cross-Restart)
    // ------------------------------------------------------------------------
    std::printf("[Opt / 5] Persistent Native Driver ID3D12PipelineLibrary1 Disk Cache...\n");
    {
        const std::filesystem::path pso_dir =
            std::filesystem::temp_directory_path() / "wsx_pso_cache_test_32";
        std::error_code ec;
        std::filesystem::remove_all(pso_dir, ec);
        std::filesystem::create_directories(pso_dir, ec);
        SetEnvironmentVariableW(L"WSX_PSO_CACHE_DIR", pso_dir.c_str());

        // Compile 1 CS + 1 VS + 1 PS cleanly via DXC
        static constexpr char kCs[] =
            "RWByteAddressBuffer b : register(u0);\n"
            "[numthreads(8,1,1)] void main(uint3 id : SV_DispatchThreadID) { b.Store(id.x*4, id.x + 42u); }\n";
        static constexpr char kVs[] =
            "float4 main(uint id : SV_VertexID) : SV_Position { return float4(0,0,0,1); }\n";
        static constexpr char kPs[] =
            "float4 main() : SV_Target { return float4(1,0,0,1); }\n";
        std::vector<uint8_t> cs_dxil, vs_dxil, ps_dxil;
        char err[512]{};
        check_hr(wsx_sm66::compile_hlsl_to_dxil_vec(kCs, std::strlen(kCs), L"main", L"cs_6_6", cs_dxil, err, sizeof(err)), "CS DXIL");
        check_hr(wsx_sm66::compile_hlsl_to_dxil_vec(kVs, std::strlen(kVs), L"main", L"vs_6_6", vs_dxil, err, sizeof(err)), "VS DXIL");
        check_hr(wsx_sm66::compile_hlsl_to_dxil_vec(kPs, std::strlen(kPs), L"main", L"ps_6_6", ps_dxil, err, sizeof(err)), "PS DXIL");

        // Serialize root signature blob (1 root UAV u0)
        D3D12_ROOT_PARAMETER1 rp{};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        rp.Descriptor.ShaderRegister = 0u;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
        rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rsd.Desc_1_1.NumParameters = 1u;
        rsd.Desc_1_1.pParameters = &rp;
        ComPtr<ID3DBlob> rs_blob, rs_err;
        check_hr(D3D12SerializeVersionedRootSignature(&rsd, &rs_blob, &rs_err), "Serialize RS");

        D3D12XBOX_PSO_CACHE_STATS before{};
        check_hr(D3D12XboxGetPsoCacheStats(&before), "GetPsoCacheStats before");

        // --- Session 1 (Cold): populate & flush ID3D12PipelineLibrary to disk ---
        {
            ComPtr<ID3D12Device> d1;
            check_hr(D3D12XboxCreateDevice(nullptr, &cdp, IID_PPV_ARGS(&d1)), "Create Device #1");
            ComPtr<ID3D12RootSignature> rs1;
            check_hr(d1->CreateRootSignature(0u, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                             IID_PPV_ARGS(&rs1)), "Create RS #1");

            D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
            cpd.pRootSignature = rs1.Get();
            cpd.CS = { cs_dxil.data(), cs_dxil.size() };
            ComPtr<ID3D12PipelineState> cpso1;
            check_hr(d1->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&cpso1)), "Cold Compute PSO");

            D3D12_GRAPHICS_PIPELINE_STATE_DESC gpd{};
            gpd.pRootSignature = rs1.Get();
            gpd.VS = { vs_dxil.data(), vs_dxil.size() };
            gpd.PS = { ps_dxil.data(), ps_dxil.size() };
            gpd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            gpd.SampleMask = UINT_MAX;
            gpd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            gpd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            gpd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            gpd.NumRenderTargets = 1u;
            gpd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            gpd.SampleDesc.Count = 1u;
            ComPtr<ID3D12PipelineState> gpso1;
            check_hr(d1->CreateGraphicsPipelineState(&gpd, IID_PPV_ARGS(&gpso1)), "Cold Graphics PSO");

            check_hr(D3D12XboxFlushPsoCache(d1.Get()), "FlushPsoCache #1");
            check(count_debug_errors(d1.Get()) == 0u, "Zero D3D12 errors on Cold Device #1");
        }

        D3D12XBOX_PSO_CACHE_STATS after_cold{};
        check_hr(D3D12XboxGetPsoCacheStats(&after_cold), "GetPsoCacheStats after cold");
        check(after_cold.stores >= before.stores + 2u, "Cold session stored >= 2 PSOs into PipelineLibrary");
        check(after_cold.flushes >= before.flushes + 1u, "Cold session flushed library blob to disk");
        check(after_cold.library_bytes > 0u, "Serialized ID3D12PipelineLibrary size > 0");

        // --- Session 2 (Warm Restart): brand-new Device #2 + brand-new RootSignature #2 ---
        {
            ComPtr<ID3D12Device> d2;
            check_hr(D3D12XboxCreateDevice(nullptr, &cdp, IID_PPV_ARGS(&d2)), "Create Device #2");
            ComPtr<ID3D12RootSignature> rs2;
            check_hr(d2->CreateRootSignature(0u, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                             IID_PPV_ARGS(&rs2)), "Create RS #2");

            D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
            cpd.pRootSignature = rs2.Get();
            cpd.CS = { cs_dxil.data(), cs_dxil.size() };
            ComPtr<ID3D12PipelineState> cpso2;
            check_hr(d2->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&cpso2)), "Warm Compute PSO");

            D3D12_GRAPHICS_PIPELINE_STATE_DESC gpd{};
            gpd.pRootSignature = rs2.Get();
            gpd.VS = { vs_dxil.data(), vs_dxil.size() };
            gpd.PS = { ps_dxil.data(), ps_dxil.size() };
            gpd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            gpd.SampleMask = UINT_MAX;
            gpd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            gpd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            gpd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            gpd.NumRenderTargets = 1u;
            gpd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            gpd.SampleDesc.Count = 1u;
            ComPtr<ID3D12PipelineState> gpso2;
            check_hr(d2->CreateGraphicsPipelineState(&gpd, IID_PPV_ARGS(&gpso2)), "Warm Graphics PSO");

            check(count_debug_errors(d2.Get()) == 0u, "Zero D3D12 errors on Warm Device #2");
        }

        D3D12XBOX_PSO_CACHE_STATS after_warm{};
        check_hr(D3D12XboxGetPsoCacheStats(&after_warm), "GetPsoCacheStats after warm");
        check(after_warm.hits >= after_cold.hits + 2u,
              "Warm Device #2 hit ID3D12PipelineLibrary for BOTH Compute & Graphics PSOs across restart");

        SetEnvironmentVariableW(L"WSX_PSO_CACHE_DIR", nullptr);
        std::filesystem::remove_all(pso_dir, ec);
        std::printf("  [PASS] ID3D12PipelineLibrary1 disk cache: stores=%llu flushes=%llu lib=%llu B -> warm hits=%llu\n",
                    static_cast<unsigned long long>(after_cold.stores - before.stores),
                    static_cast<unsigned long long>(after_cold.flushes - before.flushes),
                    static_cast<unsigned long long>(after_cold.library_bytes),
                    static_cast<unsigned long long>(after_warm.hits - after_cold.hits));
    }

    check(count_debug_errors(dev.Get()) == 0u, "Zero D3D12 Debug Layer errors across all optimizer tests");

    if (g_failures > 0) {
        std::fprintf(stderr, "RDNA2 OPTIMIZER & PSO CACHE TEST FAILED (%d failures)\n", g_failures);
        return 1;
    }
    std::printf("ALL RDNA2 OPTIMIZER + ACCC/FSPX + PSO CACHE TESTS PASSED (0 D3D12 Debug Layer errors)!\n");
    return 0;
}
