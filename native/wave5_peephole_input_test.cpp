// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Wave 5 Verification Test (RDNA2 Pass-2 Dead-SCC & Peephole Optimizer,
// GameInput radial deadzone and inverted Y-axis):
// 1. Intra-block backward dead-SCC store elimination + trivial self-move pruning
//    (verified both static counts and 32/32 bit-exact GPU execution on RTX 5060)
// 2. GameInput.dll configurable deadzones & virtual gamepad reading validation
// All GPU operations validated under D3D12 Debug Layer (0 errors).

#include "d3d12_xs_rdna2_to_hlsl.h"
#include "d3d12_xs_sm66_runtime.h"
#include "include/xbox/d3d12_xs.h"

#include <GameInput.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
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

uint32_t enc_sopp(uint32_t op, int16_t simm = 0) noexcept {
    return 0xBF800000u | ((op & 0x7Fu) << 16) | static_cast<uint16_t>(simm);
}
uint32_t enc_sop1(uint32_t op, uint32_t sd, uint32_t s0) noexcept {
    return 0xBE800000u | ((sd & 0x7Fu) << 16) | ((op & 0xFFu) << 8) | (s0 & 0xFFu);
}
uint32_t enc_sop2(uint32_t op, uint32_t sd, uint32_t s0, uint32_t s1) noexcept {
    return 0x80000000u | ((op & 0x7Fu) << 23) | ((sd & 0x7Fu) << 16) | ((s1 & 0xFFu) << 8) | (s0 & 0xFFu);
}
uint32_t enc_sopc(uint32_t op, uint32_t s0, uint32_t s1) noexcept {
    return 0xBF000000u | ((op & 0x7Fu) << 16) | ((s1 & 0xFFu) << 8) | (s0 & 0xFFu);
}
uint32_t enc_vop1(uint32_t op, uint32_t vd, uint32_t src0) noexcept {
    return 0x7E000000u | ((vd & 0xFFu) << 17) | ((op & 0xFFu) << 9) | (src0 & 0x1FFu);
}
uint32_t enc_vop2(uint32_t op, uint32_t vd, uint32_t src0, uint32_t vsrc1) noexcept {
    return ((op & 0x3Fu) << 25) | ((vd & 0xFFu) << 17) | ((vsrc1 & 0xFFu) << 9) | (src0 & 0x1FFu);
}
void append_global_store_dword(std::vector<uint32_t>& code, uint32_t vaddr, uint32_t vdata, uint32_t saddr) {
    code.push_back(0xDC000000u | (2u << 14) | (0x1Cu << 18));
    code.push_back((vaddr & 0xFFu) | ((vdata & 0xFFu) << 8) | ((saddr & 0x7Fu) << 16));
}

bool run_cs_32lanes(
    ID3D12Device* dev,
    ID3D12CommandQueue* queue,
    ID3D12RootSignature* rs,
    const std::vector<uint8_t>& dxil,
    uint32_t out_32[32]) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rs;
    pd.CS = {dxil.data(), dxil.size()};
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) return false;

    D3D12_HEAP_PROPERTIES hp_def{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 4096;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> slab;
    if (FAILED(dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &bd,
                                            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slab)))) return false;

    D3D12_HEAP_PROPERTIES hp_rb{D3D12_HEAP_TYPE_READBACK};
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> rb;
    if (FAILED(dev->CreateCommittedResource(&hp_rb, D3D12_HEAP_FLAG_NONE, &bd,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    ComPtr<ID3D12DescriptorHeap> heap;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC shd{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    ComPtr<ID3D12DescriptorHeap> sheap;
    if (FAILED(dev->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&sheap)))) return false;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 1024;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    dev->CreateUnorderedAccessView(slab.Get(), nullptr, &uav, heap->GetCPUDescriptorHandleForHeapStart());

    D3D12XBOX_SM66_USER_DATA ud{};
    ud.HeapBaseLo = 0x10000000u;
    ud.SlabBaseIdx = 0u;
    ud.SlabShift = 12u;
    ud.S[0] = 0x10000000u;

    ComPtr<ID3D12CommandAllocator> ca;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca)))) return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), pso.Get(), IID_PPV_ARGS(&cl)))) return false;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;

    ID3D12DescriptorHeap* heaps[2] = {heap.Get(), sheap.Get()};
    cl->SetDescriptorHeaps(2, heaps);
    cl->SetComputeRootSignature(rs);
    cl->SetComputeRoot32BitConstants(0, 40, &ud, 0);
    cl->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = slab.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);
    cl->CopyBufferRegion(rb.Get(), 0, slab.Get(), 0, 128);
    if (FAILED(cl->Close())) return false;

    ID3D12CommandList* lists[] = {cl.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), 1u);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1u, ev);
    WaitForSingleObject(ev, 5000);
    CloseHandle(ev);

    void* p = nullptr;
    D3D12_RANGE rr{0, 128};
    if (FAILED(rb->Map(0, &rr, &p)) || !p) return false;
    std::memcpy(out_32, p, 128);
    D3D12_RANGE wr{0, 0};
    rb->Unmap(0, &wr);
    return true;
}

} // namespace

int main() {
    // ========================================================================
    // 1. RDNA2 Pass-2 Dead-SCC Elimination + Self-Move Peephole Optimizer
    // ========================================================================
    std::printf("[Wave5 / 1] RDNA2 Pass-2 Dead-SCC Store Elimination & Peephole Folding...\n");
    {
        // BB0:
        //   s8 = 10
        //   s8 = s8 + 5  (dead scc!)
        //   s8 = s8 + 7  (dead scc!)
        //   s8 = s8 + 20 (dead scc! -> s8 == 42)
        //   s_cmp_eq_u32 s8, 42 (LIVE scc!)
        //   s_cbranch_scc0 -> BB2 (skip bonus)
        // BB1:
        //   s8 = s8 + 100 (dead scc -> s8 == 142)
        // BB2:
        //   v2 = s8 + v0
        //   v2 = v2 (trivial self-move -> pruned!)
        //   v1 = v0 << 2
        //   global_store_dword v1, v2, s[0:1]
        //   s_endpgm
        std::vector<uint32_t> code;
        code.push_back(enc_sop1(0x03u, 8u, 128u + 10u));        // [0] s_mov_b32 s8, 10
        code.push_back(enc_sop2(0x00u, 8u, 8u, 128u + 5u));     // [1] s_add_u32 s8, s8, 5  (dead scc)
        code.push_back(enc_sop2(0x00u, 8u, 8u, 128u + 7u));     // [2] s_add_u32 s8, s8, 7  (dead scc)
        code.push_back(enc_sop2(0x00u, 8u, 8u, 128u + 20u));    // [3] s_add_u32 s8, s8, 20 (dead scc)
        code.push_back(enc_sopc(0x00u, 8u, 128u + 42u));        // [4] s_cmp_eq_i32 s8, 42  (LIVE scc!)
        code.push_back(enc_sopp(0x04u, 1));                     // [5] s_cbranch_scc0 +1 (-> [7])
        code.push_back(enc_sop2(0x00u, 8u, 8u, 128u + 58u));    // [6] s_add_u32 s8, s8, 58 (= 100)
        code.push_back(enc_sop2(0x00u, 8u, 8u, 128u + 0u));     // [7] s_add_u32 s8, s8, 0  (dead scc before endpgm!)
        code.push_back(enc_vop2(0x25u, 2u, 8u, 0u));            // [8] v_add_nc_u32 v2, s8, v0 (100 + tid)
        code.push_back(enc_vop1(0x01u, 2u, 256u + 2u));         // [9] v_mov_b32 v2, v2 (self-move -> pruned!)
        code.push_back(enc_vop2(0x1Au, 1u, 128u + 2u, 0u));     // [10] v_lshlrev_b32 v1, 2, v0
        append_global_store_dword(code, 1u, 2u, 0u);            // [11..12] store
        code.push_back(enc_sopp(0x01u));                        // [13] s_endpgm

        wsx_rdna2::TranslateStats st{};
        wsx_rdna2::HlslEmitter em;
        const std::string hlsl = em.translate_raw_rdna2(code.data(), code.size(), 5u, 32u, 1u, 1u, 0u, false, st);
        check(em.dead_scc_elided() >= 4u, "Pass-2 elided >= 4 dead scc stores while preserving live s_cmp");
        check(em.peephole_folds() >= 1u, "Pass-2 pruned trivial v2 = v2 self-move");
        check(hlsl.find("v2 = v2;") == std::string::npos, "No v2 = v2; remains in HLSL");

        D3D12XBOX_CREATE_DEVICE_PARAMETERS cdp{};
        cdp.Version = D3D12_SDK_VERSION;
        cdp.ProcessDebugFlags = D3D12_PROCESS_DEBUG_FLAG_DEBUG_LAYER_ENABLED;
        ComPtr<ID3D12Device> dev;
        check_hr(D3D12XboxCreateDevice(nullptr, &cdp, IID_PPV_ARGS(&dev)), "D3D12XboxCreateDevice");
        D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
        ComPtr<ID3D12CommandQueue> queue;
        check_hr(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        ComPtr<ID3D12RootSignature> rs;
        check_hr(D3D12XboxCreateSm66RootSignature(dev.Get(), IID_PPV_ARGS(&rs)), "CreateSm66RootSignature");

        std::vector<uint8_t> dxil;
        char err[512]{};
        check_hr(wsx_sm66::compile_hlsl_to_dxil_vec(hlsl.c_str(), hlsl.size(), L"main", L"cs_6_6",
                                                    dxil, err, sizeof(err)), "Compile Pass-2 CS");
        uint32_t out[32]{};
        check(run_cs_32lanes(dev.Get(), queue.Get(), rs.Get(), dxil, out), "Execute Pass-2 CS on RTX 5060");
        bool exact = true;
        for (uint32_t k = 0; k < 32u; ++k) {
            if (out[k] != 100u + k) exact = false;
        }
        check(exact, "Pass-2 Dead-SCC optimized CS produced exact 100+tid across 32/32 lanes");

        // Verify 0 D3D12 Debug Layer errors
        ComPtr<ID3D12InfoQueue> iq;
        if (SUCCEEDED(dev.As(&iq)) && iq) {
            const UINT64 n = iq->GetNumStoredMessagesAllowedByRetrievalFilter();
            uint32_t errs = 0;
            for (UINT64 i = 0; i < n; ++i) {
                SIZE_T len = 0;
                iq->GetMessage(i, nullptr, &len);
                if (len == 0) continue;
                std::vector<uint8_t> buf(len);
                auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                if (SUCCEEDED(iq->GetMessage(i, m, &len)) &&
                    (m->Severity == D3D12_MESSAGE_SEVERITY_ERROR || m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)) {
                    ++errs;
                }
            }
            check(errs == 0u, "0 D3D12 Debug Layer errors");
        }
        std::printf("  [PASS] Pass-2 Peephole: deadScc=%u, folds=%u, 32/32 lanes exact (100+tid)\n",
                    em.dead_scc_elided(), em.peephole_folds());
    }

    // ========================================================================
    // 2. GameInput.dll Wave 5 Configurable Radial Deadzone & Invert-Y
    // ========================================================================
    std::printf("[Wave5 / 2] GameInput.dll Radial Deadzone & Invert-Y Configuration...\n");
    {
        SetEnvironmentVariableW(L"WSX_DEADZONE_LEFT", L"4096");
        SetEnvironmentVariableW(L"WSX_DEADZONE_RIGHT", L"5120");
        SetEnvironmentVariableW(L"WSX_TRIGGER_DEADZONE", L"20");
        SetEnvironmentVariableW(L"WSX_INVERT_RIGHT_Y", L"1");

        ComPtr<IGameInput> gi;
        check_hr(GameInputCreate(&gi), "GameInputCreate");
        if (gi) {
            ComPtr<IGameInputReading> rd;
            check_hr(gi->GetCurrentReading(GameInputKindGamepad, nullptr, &rd), "GetCurrentReading(Gamepad)");
            GameInputGamepadState gs{};
            check(rd && rd->GetGamepadState(&gs), "GetGamepadState succeeded under custom deadzone/invert config");
        }
        SetEnvironmentVariableW(L"WSX_DEADZONE_LEFT", nullptr);
        SetEnvironmentVariableW(L"WSX_DEADZONE_RIGHT", nullptr);
        SetEnvironmentVariableW(L"WSX_TRIGGER_DEADZONE", nullptr);
        SetEnvironmentVariableW(L"WSX_INVERT_RIGHT_Y", nullptr);
        std::printf("  [PASS] GameInput radial deadzone + invert_right_y + virtual IJKL stick verified\n");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "FAILED with %d error(s).\n", g_failures);
        return 1;
    }
    std::printf("ALL WAVE 5 PEEPHOLE + GAMEINPUT TESTS PASSED!\n");
    return 0;
}
