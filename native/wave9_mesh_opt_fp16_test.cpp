// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Wave 9 Live RTX 5060 Verification Suite (D3D12 Debug Layer: 0 errors):
//  1. Dynamic RDNA2 Mesh Shader (ms_6_6): s_sendmsg(MSG_GS_ALLOC_REQ=10) + EXP POS0 + EXP PRIM
//     paired with RDNA2 Pixel Shader (ps_6_6) -> live DispatchMesh(1,1,1) rasterization on RTX 5060
//     with exact pixel readback!
//  2. Dynamic RDNA2 Amplification Shader (as_6_6): s_sendmsg(10) + 16-dword WsxAsPayload + signed DXIL!
//  3. Pass 1C Dead VGPR/SGPR Store Elimination (bit-exact 32/32 lanes on live GPU).
//  4. Multi-Exit Structured Loops in Relooper (2-exit loop -> Relooper, bit-exact vs StateMachine 32/32 lanes).
//  5. Native 16-bit FP16 VOP3P (WSX_FP16_VOP3P=1, DXC -enable-16bit-types, bit-exact packed half2 on GPU).

#include "d3d12_xs_rdna2_to_hlsl.h"
#include "d3d12_xs_sm66_runtime.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int g_failures = 0;

#define CHECK(cond, msg, ...)                                        \
    do {                                                             \
        if (!(cond)) {                                               \
            std::fprintf(stderr, "[FAIL] " msg "\n", ##__VA_ARGS__); \
            ++g_failures;                                            \
        } else {                                                     \
            std::printf("[PASS] " msg "\n", ##__VA_ARGS__);          \
        }                                                            \
    } while (0)

// RDNA2 (GFX10.3) instruction encoders
inline uint32_t enc_sopp(uint32_t op, int16_t simm16 = 0) {
    return 0xBF800000u | ((op & 0x7Fu) << 16) | (static_cast<uint32_t>(simm16) & 0xFFFFu);
}
inline uint32_t enc_sop1(uint32_t op, uint32_t sd, uint32_t ssrc0) {
    return 0xBE800000u | ((sd & 0x7Fu) << 16) | ((op & 0xFFu) << 8) | (ssrc0 & 0xFFu);
}
inline uint32_t enc_sop2(uint32_t op, uint32_t sd, uint32_t ssrc0, uint32_t ssrc1) {
    return 0x80000000u | ((op & 0x7Fu) << 23) | ((sd & 0x7Fu) << 16) | ((ssrc1 & 0xFFu) << 8) | (ssrc0 & 0xFFu);
}
inline uint32_t enc_sopc(uint32_t op, uint32_t ssrc0, uint32_t ssrc1) {
    return 0xBF000000u | ((op & 0x7Fu) << 16) | ((ssrc1 & 0xFFu) << 8) | (ssrc0 & 0xFFu);
}
inline uint32_t enc_vop1(uint32_t op, uint32_t vd, uint32_t src0) {
    return 0x7E000000u | ((vd & 0xFFu) << 17) | ((op & 0xFFu) << 9) | (src0 & 0x1FFu);
}
inline uint32_t enc_vop2(uint32_t op, uint32_t vd, uint32_t src0, uint32_t vsrc1) {
    return ((op & 0x3Fu) << 25) | ((vd & 0xFFu) << 17) | ((vsrc1 & 0xFFu) << 9) | (src0 & 0x1FFu);
}
inline uint32_t enc_vop3_w0(uint32_t op, uint32_t vd) {
    return (0x35u << 26) | ((op & 0x3FFu) << 16) | (vd & 0xFFu);
}
inline uint32_t enc_vop3_w1(uint32_t s0, uint32_t s1, uint32_t s2 = 0u) {
    return (s0 & 0x1FFu) | ((s1 & 0x1FFu) << 9) | ((s2 & 0x1FFu) << 18);
}
inline void push_vop3p_pk(std::vector<uint32_t>& w, uint32_t op, uint32_t vd, uint32_t s0, uint32_t s1, uint32_t s2 = 0u) {
    // opsel_lo = 0 (lo lane <- lo16), opsel_hi = 7 (hi lane <- hi16 -> bit14=1 in w0, bits[28:27]=3 in w1)
    w.push_back((0x33u << 26) | ((op & 0x7Fu) << 16) | (1u << 14) | (vd & 0xFFu));
    w.push_back((3u << 27) | ((s2 & 0x1FFu) << 18) | ((s1 & 0x1FFu) << 9) | (s0 & 0x1FFu));
}
inline void push_global_store_u32(std::vector<uint32_t>& w, uint32_t vaddr, uint32_t vdata, uint32_t saddr = 0u) {
    w.push_back((0x37u << 26) | (0x1Cu << 18) | (2u << 14));
    w.push_back(((saddr & 0x7Fu) << 16) | ((vdata & 0xFFu) << 8) | (vaddr & 0xFFu));
}
inline void push_exp(std::vector<uint32_t>& w, uint32_t tgt, uint32_t en,
                     uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3,
                     bool done = false, bool vm = false) {
    w.push_back((0x3Eu << 26) | (vm ? (1u << 12) : 0u) | (done ? (1u << 11) : 0u) | ((tgt & 0x3Fu) << 4) | (en & 0xFu));
    w.push_back((v0 & 0xFFu) | ((v1 & 0xFFu) << 8) | ((v2 & 0xFFu) << 16) | ((v3 & 0xFFu) << 24));
}
inline uint32_t f32_bits(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    return u;
}
inline uint16_t f32_to_f16(float f) {
    const uint32_t b = f32_bits(f);
    const uint32_t sign = (b >> 16) & 0x8000u;
    const int exp = static_cast<int>((b >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mant = (b >> 13) & 0x03FFu;
    if (exp <= 0) return static_cast<uint16_t>(sign);
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | mant);
}
inline uint32_t pack_f16x2(float lo, float hi) {
    return static_cast<uint32_t>(f32_to_f16(lo)) | (static_cast<uint32_t>(f32_to_f16(hi)) << 16);
}

struct GpuCtx {
    ComPtr<ID3D12Device8> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12DescriptorHeap> uav_heap;
    ComPtr<ID3D12DescriptorHeap> smp_heap;
    ComPtr<ID3D12Resource> slab_buf;
    ComPtr<ID3D12Resource> readback_buf;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList6> cl;
    ComPtr<ID3D12Fence> fence;
    HANDLE ev = nullptr;
    uint64_t fval = 0;

    bool init() {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))) && dbg) {
            dbg->EnableDebugLayer();
        }
        ComPtr<IDXGIFactory6> fac;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)))) return false;
        ComPtr<IDXGIAdapter1> ad;
        for (UINT i = 0; fac->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad)) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            ad->GetDesc1(&d);
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (SUCCEEDED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&dev)))) break;
            ad.Reset();
        }
        if (!dev) return false;

        wsx_sm66::Sm66Caps si{};
        wsx_sm66::check_sm66_support(dev.Get(), &si);
        if (!si.SupportsDynamicResources) return false;

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return false;
        if (FAILED(wsx_sm66::create_sm66_root_signature(
                dev.Get(), &D3D12SerializeVersionedRootSignature, IID_PPV_ARGS(&rs)))) return false;

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 4;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&uav_heap)))) return false;

        D3D12_DESCRIPTOR_HEAP_DESC shd{};
        shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        shd.NumDescriptors = 4;
        shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&smp_heap)))) return false;

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
        if (FAILED(dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &bd,
                                                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slab_buf)))) return false;

        D3D12_HEAP_PROPERTIES hp_rb{D3D12_HEAP_TYPE_READBACK};
        bd.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(dev->CreateCommittedResource(&hp_rb, D3D12_HEAP_FLAG_NONE, &bd,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_buf)))) return false;

        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 1024;
        ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        dev->CreateUnorderedAccessView(slab_buf.Get(), nullptr, &ud, uav_heap->GetCPUDescriptorHandleForHeapStart());

        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return false;
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl)))) return false;
        cl->Close();
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return ev != nullptr;
    }

    ~GpuCtx() { if (ev) CloseHandle(ev); }

    bool flush_and_wait() {
        cl->Close();
        ID3D12CommandList* lists[] = {cl.Get()};
        queue->ExecuteCommandLists(1, lists);
        ++fval;
        queue->Signal(fence.Get(), fval);
        if (fence->GetCompletedValue() < fval) {
            fence->SetEventOnCompletion(fval, ev);
            WaitForSingleObject(ev, 5000);
        }
        return true;
    }

    bool run_cs(const std::vector<uint8_t>& dxil, std::vector<uint32_t>& out_u32, uint32_t count = 32u) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rs.Get();
        pd.CS = {dxil.data(), dxil.size()};
        ComPtr<ID3D12PipelineState> pso;
        if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) return false;

        alloc->Reset();
        cl->Reset(alloc.Get(), pso.Get());
        ID3D12DescriptorHeap* heaps[] = {uav_heap.Get(), smp_heap.Get()};
        cl->SetDescriptorHeaps(2, heaps);
        cl->SetComputeRootSignature(rs.Get());

        wsx_sm66::Sm66UserData ud{};
        ud.s[0] = 0x10000000u;
        ud.heap_base_lo = 0x10000000u;
        ud.slab_base_idx = 0u;
        ud.slab_shift = 12u; // 4096 B slab
        cl->SetComputeRoot32BitConstants(0, 40, &ud, 0);

        D3D12_RESOURCE_BARRIER b0{};
        b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b0.Transition = {slab_buf.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        cl->ResourceBarrier(1, &b0);

        cl->Dispatch(1, 1, 1);

        D3D12_RESOURCE_BARRIER b1{};
        b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b1.Transition = {slab_buf.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cl->ResourceBarrier(1, &b1);
        cl->CopyBufferRegion(readback_buf.Get(), 0, slab_buf.Get(), 0, count * 4u);
        D3D12_RESOURCE_BARRIER b2{};
        b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b2.Transition = {slab_buf.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON};
        cl->ResourceBarrier(1, &b2);

        flush_and_wait();

        void* ptr = nullptr;
        if (FAILED(readback_buf->Map(0, nullptr, &ptr)) || !ptr) return false;
        out_u32.resize(count);
        std::memcpy(out_u32.data(), ptr, count * 4u);
        readback_buf->Unmap(0, nullptr);
        return true;
    }

    uint64_t debug_error_count() const {
        ComPtr<ID3D12InfoQueue> iq;
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&iq))) || !iq) return 0;
        uint64_t errs = 0;
        const UINT64 n = iq->GetNumStoredMessages();
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T len = 0;
            iq->GetMessage(i, nullptr, &len);
            std::vector<uint8_t> buf(len);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(iq->GetMessage(i, m, &len)) &&
                (m->Severity == D3D12_MESSAGE_SEVERITY_ERROR || m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)) {
                ++errs;
                std::fprintf(stderr, "  [D3D12 ERR] %s\n", m->pDescription);
            }
        }
        return errs;
    }
};

// 1. Live Dynamic Mesh Shader (MSG_GS_ALLOC_REQ) + Pixel Shader Rasterization on RTX 5060
void test_dynamic_mesh_and_amplification_shaders(GpuCtx& gpu) {
    std::printf("\n=== [Wave 9.1] Dynamic RDNA2 Mesh (ms_6_6) & Amplification (as_6_6) Shaders ===\n");

    // RDNA2 Mesh Shader (stage=13, 32x1x1):
    //   m0 = 3 | (1 << 12)  (3 vertices, 1 triangle!)
    //   s_sendmsg(MSG_GS_ALLOC_REQ = 10)
    //   Compute fullscreen triangle from thread ID v0 (0,1,2):
    //     v1 = (v0 == 1) ? 3.0 : -1.0  -> X: (-1, 3, -1)
    //     v2 = (v0 == 2) ? 3.0 : -1.0  -> Y: (-1, -1, 3)
    //     v3 = 0.0, v4 = 1.0
    //   EXP POS0 (v1, v2, v3, v4)
    //   v5 = 0 | (1 << 10) | (2 << 20)
    //   EXP PRIM (v5)
    //   s_endpgm
    std::vector<uint32_t> ms_words;
    // s_mov_b32 m0(124), lit(3 | (1 << 12))
    ms_words.push_back(enc_sop1(0x03u, 124u, 0xFFu));
    ms_words.push_back(3u | (1u << 12));
    // s_sendmsg(10) -> MSG_GS_ALLOC_REQ
    ms_words.push_back(enc_sopp(0x10u, 10));
    // Default X = -1.0 (v1), Y = -1.0 (v2), Z = 0.0 (v3), W = 1.0 (v4)
    ms_words.push_back(enc_vop1(0x01u, 1u, 0xFFu)); ms_words.push_back(f32_bits(-1.0f));
    ms_words.push_back(enc_vop1(0x01u, 2u, 0xFFu)); ms_words.push_back(f32_bits(-1.0f));
    ms_words.push_back(enc_vop1(0x01u, 3u, 0xFFu)); ms_words.push_back(f32_bits(0.0f));
    ms_words.push_back(enc_vop1(0x01u, 4u, 0xFFu)); ms_words.push_back(f32_bits(1.0f));
    ms_words.push_back(enc_vop1(0x01u, 6u, 0xFFu)); ms_words.push_back(f32_bits(3.0f));
    // v_cmp_eq_u32_e64 vcc(106), v0(256), 1(129) -> op 0xC2 in VOP3
    ms_words.push_back(enc_vop3_w0(0xC2u, 106u)); ms_words.push_back(enc_vop3_w1(256u, 129u));
    // v_cndmask_b32 v1, v1, v6, vcc (VOP2 op 0x01: vd=1, src0=v1(257), vsrc1=6)
    ms_words.push_back(enc_vop2(0x01u, 1u, 257u, 6u));
    // v_cmp_eq_u32_e64 vcc(106), v0(256), 2(130)
    ms_words.push_back(enc_vop3_w0(0xC2u, 106u)); ms_words.push_back(enc_vop3_w1(256u, 130u));
    // v_cndmask_b32 v2, v2, v6, vcc
    ms_words.push_back(enc_vop2(0x01u, 2u, 258u, 6u));
    // EXP POS0 (tgt=12, en=0xF, v1,v2,v3,v4)
    push_exp(ms_words, 12u, 0xFu, 1u, 2u, 3u, 4u);
    // v5 = 0 | (1 << 10) | (2 << 20)
    ms_words.push_back(enc_vop1(0x01u, 5u, 0xFFu));
    ms_words.push_back(0u | (1u << 10) | (2u << 20));
    // EXP PRIM (tgt=20, en=0x1, v5)
    push_exp(ms_words, 20u, 0x1u, 5u, 0u, 0u, 0u, true);
    ms_words.push_back(enc_sopp(0x01u)); // s_endpgm

    wsx_rdna2::HlslEmitter em_ms;
    wsx_rdna2::TranslateStats st_ms{};
    const std::string hlsl_ms = em_ms.translate_raw_rdna2(ms_words.data(), ms_words.size(), 13u, 32u, 1u, 1u, 0u, true, st_ms);
    CHECK(em_ms.ms_dynamic_alloc_used() &&
          hlsl_ms.find("_wsx_ms_counts[0]") != std::string::npos &&
          hlsl_ms.find("SetMeshOutputCounts(_wsx_vc, _wsx_pc)") != std::string::npos,
          "RDNA2 MS decoded MSG_GS_ALLOC_REQ -> dynamic SetMeshOutputCounts(_wsx_vc, _wsx_pc)");

    // RDNA2 Pixel Shader (stage=0): output Xbox Scarlett Emerald (0.25, 0.72, 0.31, 1.0)
    std::vector<uint32_t> ps_words;
    ps_words.push_back(enc_vop1(0x01u, 0u, 0xFFu)); ps_words.push_back(f32_bits(0.25f));
    ps_words.push_back(enc_vop1(0x01u, 1u, 0xFFu)); ps_words.push_back(f32_bits(0.72f));
    ps_words.push_back(enc_vop1(0x01u, 2u, 0xFFu)); ps_words.push_back(f32_bits(0.31f));
    ps_words.push_back(enc_vop1(0x01u, 3u, 0xFFu)); ps_words.push_back(f32_bits(1.0f));
    push_exp(ps_words, 0u, 0xFu, 0u, 1u, 2u, 3u, true, true);
    ps_words.push_back(enc_sopp(0x01u));

    wsx_rdna2::HlslEmitter em_ps;
    wsx_rdna2::TranslateStats st_ps{};
    const std::string hlsl_ps = em_ps.translate_raw_rdna2(ps_words.data(), ps_words.size(), 0u, 1u, 1u, 1u, 0u, true, st_ps);

    std::vector<uint8_t> dxil_ms, dxil_ps;
    char err[1024]{};
    const HRESULT hr_ms = wsx_sm66::compile_hlsl_to_dxil_vec(hlsl_ms.c_str(), hlsl_ms.size(), L"main", L"ms_6_6", dxil_ms, err, sizeof(err));
    CHECK(SUCCEEDED(hr_ms) && !dxil_ms.empty(), "Compiled dynamic RDNA2 MS -> ms_6_6 signed DXIL (%zu B, err=%s)", dxil_ms.size(), err);
    err[0] = '\0';
    const HRESULT hr_ps = wsx_sm66::compile_hlsl_to_dxil_vec(hlsl_ps.c_str(), hlsl_ps.size(), L"main", L"ps_6_6", dxil_ps, err, sizeof(err));
    CHECK(SUCCEEDED(hr_ps) && !dxil_ps.empty(), "Compiled RDNA2 PS -> ps_6_6 signed DXIL (%zu B, err=%s)", dxil_ps.size(), err);

    if (SUCCEEDED(hr_ms) && SUCCEEDED(hr_ps)) {
        // Create live Mesh Shader Pipeline State via D3D12 pipeline state stream
        struct alignas(void*) MeshPsoStream {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t_rs = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
            ID3D12RootSignature* rs = nullptr;
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t_ms = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS;
            D3D12_SHADER_BYTECODE ms{};
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t_ps = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;
            D3D12_SHADER_BYTECODE ps{};
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t_rast = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
            D3D12_RASTERIZER_DESC rast{};
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t_rtv = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
            D3D12_RT_FORMAT_ARRAY rtv{};
        } stream{};
        stream.rs = gpu.rs.Get();
        stream.ms = {dxil_ms.data(), dxil_ms.size()};
        stream.ps = {dxil_ps.data(), dxil_ps.size()};
        stream.rast.FillMode = D3D12_FILL_MODE_SOLID;
        stream.rast.CullMode = D3D12_CULL_MODE_NONE;
        stream.rast.DepthClipEnable = FALSE;
        stream.rtv.NumRenderTargets = 1;
        stream.rtv.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

        D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof(stream), &stream};
        ComPtr<ID3D12PipelineState> pso;
        const HRESULT hr_pso = gpu.dev->CreatePipelineState(&sd, IID_PPV_ARGS(&pso));
        CHECK(SUCCEEDED(hr_pso) && pso, "Created live RTX 5060 Mesh+Pixel PSO (hr=0x%08lX)", static_cast<unsigned long>(hr_pso));

        if (SUCCEEDED(hr_pso)) {
            D3D12_HEAP_PROPERTIES hp_def{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC rtd{};
            rtd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rtd.Width = 64; rtd.Height = 64; rtd.DepthOrArraySize = 1; rtd.MipLevels = 1;
            rtd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rtd.SampleDesc.Count = 1;
            rtd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            ComPtr<ID3D12Resource> rt;
            gpu.dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &rtd,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt));

            D3D12_DESCRIPTOR_HEAP_DESC rhd{};
            rhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rhd.NumDescriptors = 1;
            ComPtr<ID3D12DescriptorHeap> rtv_heap;
            gpu.dev->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap));
            gpu.dev->CreateRenderTargetView(rt.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());

            D3D12_HEAP_PROPERTIES hp_rb{D3D12_HEAP_TYPE_READBACK};
            D3D12_RESOURCE_DESC rbd{};
            rbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rbd.Width = 256u * 64u; rbd.Height = 1; rbd.DepthOrArraySize = 1; rbd.MipLevels = 1;
            rbd.SampleDesc.Count = 1; rbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> rb;
            gpu.dev->CreateCommittedResource(&hp_rb, D3D12_HEAP_FLAG_NONE, &rbd,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));

            gpu.alloc->Reset();
            gpu.cl->Reset(gpu.alloc.Get(), pso.Get());
            const auto rtv_h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            gpu.cl->ClearRenderTargetView(rtv_h, clear, 0, nullptr);
            gpu.cl->OMSetRenderTargets(1, &rtv_h, FALSE, nullptr);
            D3D12_VIEWPORT vp{0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
            D3D12_RECT sc{0, 0, 64, 64};
            gpu.cl->RSSetViewports(1, &vp);
            gpu.cl->RSSetScissorRects(1, &sc);
            ID3D12DescriptorHeap* heaps[] = {gpu.uav_heap.Get(), gpu.smp_heap.Get()};
            gpu.cl->SetDescriptorHeaps(2, heaps);
            gpu.cl->SetGraphicsRootSignature(gpu.rs.Get());
            gpu.cl->DispatchMesh(1, 1, 1);

            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = {rt.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
            gpu.cl->ResourceBarrier(1, &b);

            D3D12_TEXTURE_COPY_LOCATION src{rt.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            D3D12_TEXTURE_COPY_LOCATION dst{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
            dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256};
            gpu.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            gpu.flush_and_wait();

            uint8_t* px = nullptr;
            rb->Map(0, nullptr, reinterpret_cast<void**>(&px));
            const uint8_t* c = px + 32u * 256u + 32u * 4u;
            const bool rgb_ok = (std::abs(int(c[0]) - 64) <= 2 &&
                                 std::abs(int(c[1]) - 184) <= 2 &&
                                 std::abs(int(c[2]) - 79) <= 2 &&
                                 c[3] == 255);
            CHECK(rgb_ok, "Live DispatchMesh(1,1,1) rasterized RDNA2 MS+PS -> pixel(32,32)=(%u,%u,%u,%u) expected ~(64,184,79,255)",
                  c[0], c[1], c[2], c[3]);
            rb->Unmap(0, nullptr);
        }
    }

    // Amplification Shader (stage=12): s_sendmsg(10) dynamic dispatch + 16-dword payload
    std::vector<uint32_t> as_words;
    as_words.push_back(enc_sop1(0x03u, 124u, 0xFFu));
    as_words.push_back(2u | (1u << 10) | (1u << 20)); // gx=2, gy=1, gz=1
    as_words.push_back(enc_sopp(0x10u, 10));
    as_words.push_back(enc_sopp(0x01u));
    wsx_rdna2::HlslEmitter em_as;
    wsx_rdna2::TranslateStats st_as{};
    const std::string hlsl_as = em_as.translate_raw_rdna2(as_words.data(), as_words.size(), 12u, 32u, 1u, 1u, 0u, true, st_as);
    std::vector<uint8_t> dxil_as;
    err[0] = '\0';
    const HRESULT hr_as = wsx_sm66::compile_hlsl_to_dxil_vec(hlsl_as.c_str(), hlsl_as.size(), L"main", L"as_6_6", dxil_as, err, sizeof(err));
    CHECK(SUCCEEDED(hr_as) && hlsl_as.find("uint data[16]") != std::string::npos &&
          hlsl_as.find("DispatchMesh(_wsx_as_dims[0], _wsx_as_dims[1], _wsx_as_dims[2]") != std::string::npos,
          "Compiled dynamic RDNA2 AS (16-dword WsxAsPayload + dynamic DispatchMesh) -> as_6_6 DXIL (%zu B)", dxil_as.size());
}

// 2. Pass 1C Dead VGPR/SGPR Store Elimination (verified bit-exact on live GPU)
void test_dead_vgpr_sgpr_elimination(GpuCtx& gpu) {
    std::printf("\n=== [Wave 9.2] Pass 1C Dead VGPR/SGPR Store Elimination ===\n");
    std::vector<uint32_t> words;
    // Deliberate dead writes before overwrite / endpgm:
    words.push_back(enc_vop1(0x01u, 10u, 0xFFu)); words.push_back(0xDEADBEEFu); // DEAD (overwritten below)
    words.push_back(enc_vop1(0x01u, 11u, 0xFFu)); words.push_back(0xBADF00Du);  // DEAD (never read)
    words.push_back(enc_sop1(0x03u, 12u, 0xFFu)); words.push_back(0x11111111u); // DEAD (overwritten)
    words.push_back(enc_sop1(0x03u, 12u, 0xFFu)); words.push_back(0x00000100u); // LIVE (used in add)
    // Live calculation: v10 = v0 + s12 (v0 + 0x100)
    words.push_back(enc_vop2(0x25u, 10u, 12u, 0u)); // v_add_nc_u32 v10, s12, v0
    // v1 = v0 << 2 (byte offset)
    words.push_back(enc_vop2(0x1Au, 1u, 130u, 0u)); // v_lshlrev_b32 v1, 2, v0
    push_global_store_u32(words, 1u, 10u, 0u);
    // Dead write AFTER store before s_endpgm:
    words.push_back(enc_vop1(0x01u, 10u, 0xFFu)); words.push_back(0x99999999u); // DEAD
    words.push_back(enc_sopp(0x01u));

    wsx_rdna2::HlslEmitter em;
    wsx_rdna2::TranslateStats st{};
    const std::string hlsl = em.translate_raw_rdna2(words.data(), words.size(), 5u, 32u, 1u, 1u, 0u, true, st);
    CHECK(em.dead_reg_elided() >= 4u &&
          hlsl.find("DEADBEEF") == std::string::npos &&
          hlsl.find("BADF00D") == std::string::npos &&
          hlsl.find("99999999") == std::string::npos,
          "Pass 1C pruned %u dead VGPR/SGPR stores cleanly", em.dead_reg_elided());

    std::vector<uint8_t> dxil;
    char err[1024]{};
    CHECK(SUCCEEDED(wsx_sm66::compile_hlsl_to_dxil_vec(hlsl.c_str(), hlsl.size(), L"main", L"cs_6_6", dxil, err, sizeof(err))),
          "Compiled Pass 1C optimized CS -> cs_6_6 DXIL (%zu B)", dxil.size());
    std::vector<uint32_t> out;
    bool ok = gpu.run_cs(dxil, out, 32u);
    uint32_t matched = 0;
    for (uint32_t i = 0; ok && i < 32u; ++i) {
        if (out[i] == (i + 0x100u)) ++matched;
    }
    CHECK(ok && matched == 32u, "Live RTX 5060 Pass 1C GPU dispatch bit-exact (%u/32 lanes)", matched);
}

// 3. Multi-Exit Structured Loop in Relooper (bit-exact vs StateMachine on RTX 5060)
void test_multi_exit_relooper_loop(GpuCtx& gpu) {
    std::printf("\n=== [Wave 9.3] Multi-Exit Natural Loop Structuring in Relooper ===\n");
    // Build a 2-exit natural loop:
    //   BB0 (init): s10 = 0 (counter), v5 = 0
    //   BB1 (loop hdr): s10 = s10 + 1; s_cmp_lt_u32 s10, 4; if (!scc) goto BB3 (Exit #1: normal finish)
    //   BB2 (loop body): s_cmp_eq_u32 s10, 2; if (scc) goto BB4 (Exit #2: early break) -> else goto BB1 (back-edge!)
    //   BB3 (Exit #1): v5 = 1000; goto BB5
    //   BB4 (Exit #2): v5 = 2000; goto BB5
    //   BB5 (Merge): v6 = v5 + v0; store v6 at [s0:s1 + v0*4]; s_endpgm
    // Note: s10 starts at 0, increments to 1,2 -> hits s10==2 -> takes Exit #2 (v5=2000) -> outputs 2000 + lane!
    std::vector<uint32_t> w;
    // BB0: s10 = 0 (128), v5 = 0 (128)
    w.push_back(enc_sop1(0x03u, 10u, 128u));           // pc 0 (idx 0)
    w.push_back(enc_vop1(0x01u, 5u, 128u));            // pc 4 (idx 1)
    // BB1 (idx 2):
    w.push_back(enc_sop2(0x00u, 10u, 10u, 129u));      // idx 2: s10 = s10 + 1
    w.push_back(enc_sopc(0x0Au, 10u, 132u));           // idx 3: s_cmp_lt_u32 s10, 4 (128+4=132)
    w.push_back(enc_sopp(0x04u, 3));                   // idx 4: s_cbranch_scc0 +3 -> idx 4+1+3 = idx 8 (BB3)
    // BB2 (idx 5):
    w.push_back(enc_sopc(0x00u, 10u, 130u));           // idx 5: s_cmp_eq_i32 s10, 2 (128+2=130)
    w.push_back(enc_sopp(0x05u, 4));                   // idx 6: s_cbranch_scc1 +4 -> idx 6+1+4 = idx 11 (BB4)
    w.push_back(enc_sopp(0x02u, -6));                  // idx 7: s_branch -6 -> idx 7+1-6 = idx 2 (BB1)
    // BB3 (idx 8): Exit #1
    w.push_back(enc_vop1(0x01u, 5u, 0xFFu));           // idx 8: v5 = 1000
    w.push_back(1000u);                                // idx 9
    w.push_back(enc_sopp(0x02u, 2));                   // idx 10: s_branch +2 -> idx 10+1+2 = idx 13 (BB5)
    // BB4 (idx 11): Exit #2
    w.push_back(enc_vop1(0x01u, 5u, 0xFFu));           // idx 11: v5 = 2000
    w.push_back(2000u);                                // idx 12
    // BB5 (idx 13): Merge + Store
    w.push_back(enc_vop2(0x25u, 6u, 256u + 5u, 0u));   // idx 13: v6 = v5 + v0
    w.push_back(enc_vop2(0x1Au, 1u, 130u, 0u));        // idx 14: v1 = v0 << 2
    push_global_store_u32(w, 1u, 6u, 0u);              // idx 15..16
    w.push_back(enc_sopp(0x01u));                      // idx 17: s_endpgm

    SetEnvironmentVariableW(L"WSX_RDNA2_CFG_MODE", L"auto");
    wsx_rdna2::HlslEmitter em_rel;
    wsx_rdna2::TranslateStats st_rel{};
    const std::string hlsl_rel = em_rel.translate_raw_rdna2(w.data(), w.size(), 5u, 32u, 1u, 1u, 0u, true, st_rel);
    CHECK(em_rel.cfg_mode() == wsx_rdna2::CfgLoweringMode::Relooper &&
          em_rel.multi_exit_loops_structured() >= 1u &&
          hlsl_rel.find("_wex") != std::string::npos,
          "2-exit natural loop structured into Relooper (cfg=%s, multi_exit=%u, zero StateMachine!)",
          wsx_rdna2::cfg_mode_name(em_rel.cfg_mode()), em_rel.multi_exit_loops_structured());

    SetEnvironmentVariableW(L"WSX_RDNA2_CFG_MODE", L"statemachine");
    wsx_rdna2::HlslEmitter em_sm;
    wsx_rdna2::TranslateStats st_sm{};
    const std::string hlsl_sm = em_sm.translate_raw_rdna2(w.data(), w.size(), 5u, 32u, 1u, 1u, 0u, true, st_sm);
    SetEnvironmentVariableW(L"WSX_RDNA2_CFG_MODE", nullptr);

    std::vector<uint8_t> dxil_rel, dxil_sm;
    char err[1024]{};
    CHECK(SUCCEEDED(wsx_sm66::compile_hlsl_to_dxil_vec(hlsl_rel.c_str(), hlsl_rel.size(), L"main", L"cs_6_6", dxil_rel, err, sizeof(err))),
          "Compiled Multi-Exit Relooper CS -> cs_6_6 DXIL (%zu B, err=%s)", dxil_rel.size(), err);
    err[0] = '\0';
    CHECK(SUCCEEDED(wsx_sm66::compile_hlsl_to_dxil_vec(hlsl_sm.c_str(), hlsl_sm.size(), L"main", L"cs_6_6", dxil_sm, err, sizeof(err))),
          "Compiled StateMachine reference CS -> cs_6_6 DXIL (%zu B)", dxil_sm.size());

    std::vector<uint32_t> out_rel, out_sm;
    const bool ok1 = gpu.run_cs(dxil_rel, out_rel, 32u);
    const bool ok2 = gpu.run_cs(dxil_sm, out_sm, 32u);
    uint32_t matched = 0;
    for (uint32_t i = 0; ok1 && ok2 && i < 32u; ++i) {
        if (out_rel[i] == (2000u + i) && out_rel[i] == out_sm[i]) ++matched;
    }
    CHECK(ok1 && ok2 && matched == 32u,
          "Live RTX 5060 Multi-Exit Relooper vs StateMachine bit-exact (%u/32 lanes, lane0=%u)",
          matched, out_rel.empty() ? 0u : out_rel[0]);
}

// 4. Native 16-bit FP16 VOP3P (WSX_FP16_VOP3P=1, DXC -enable-16bit-types)
void test_native_fp16_vop3p(GpuCtx& gpu) {
    std::printf("\n=== [Wave 9.4] Native 16-bit FP16 VOP3P (-enable-16bit-types) ===\n");
    const uint32_t pk_a = pack_f16x2(1.5f, -0.5f);
    const uint32_t pk_b = pack_f16x2(2.0f,  4.0f);
    const uint32_t pk_c = pack_f16x2(2.0f,  3.0f);
    const uint32_t expected = pack_f16x2(7.0f, 10.5f); // (1.5+2.0)*2.0 = 7.0, (-0.5+4.0)*3.0 = 10.5

    std::vector<uint32_t> w;
    w.push_back(enc_vop1(0x01u, 2u, 0xFFu)); w.push_back(pk_a);
    w.push_back(enc_vop1(0x01u, 3u, 0xFFu)); w.push_back(pk_b);
    w.push_back(enc_vop1(0x01u, 4u, 0xFFu)); w.push_back(pk_c);
    // v_pk_add_f16 v5, v2, v3 (op 0x0F)
    push_vop3p_pk(w, 0x0Fu, 5u, 256u + 2u, 256u + 3u);
    // v_pk_mul_f16 v6, v5, v4 (op 0x10)
    push_vop3p_pk(w, 0x10u, 6u, 256u + 5u, 256u + 4u);
    // Store v6 at [s0:s1 + v0*4]
    w.push_back(enc_vop2(0x1Au, 1u, 130u, 0u));
    push_global_store_u32(w, 1u, 6u, 0u);
    w.push_back(enc_sopp(0x01u));

    SetEnvironmentVariableW(L"WSX_FP16_VOP3P", L"1");
    wsx_rdna2::HlslEmitter em;
    wsx_rdna2::TranslateStats st{};
    const std::string hlsl = em.translate_raw_rdna2(w.data(), w.size(), 5u, 32u, 1u, 1u, 0u, true, st);
    SetEnvironmentVariableW(L"WSX_FP16_VOP3P", nullptr);

    CHECK(em.native_fp16_used() && hlsl.find("float16_t2") != std::string::npos,
          "WSX_FP16_VOP3P=1 emitted native SM 6.6 float16_t2 packed arithmetic");

    std::vector<uint8_t> dxil;
    char err[1024]{};
    const HRESULT hr = wsx_sm66::compile_hlsl_to_dxil_vec(hlsl.c_str(), hlsl.size(), L"main", L"cs_6_6", dxil, err, sizeof(err));
    CHECK(SUCCEEDED(hr) && !dxil.empty(),
          "DXC auto-enabled -enable-16bit-types & compiled native FP16 DXIL (%zu B, err=%s)", dxil.size(), err);

    std::vector<uint32_t> out;
    const bool ok = SUCCEEDED(hr) && gpu.run_cs(dxil, out, 32u);
    uint32_t matched = 0;
    for (uint32_t i = 0; ok && i < 32u; ++i) {
        if (out[i] == expected) ++matched;
    }
    CHECK(ok && matched == 32u,
          "Live RTX 5060 native FP16 VOP3P bit-exact (%u/32 lanes, got=0x%08X expected=0x%08X)",
          matched, out.empty() ? 0u : out[0], expected);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    GpuCtx gpu;
    CHECK(gpu.init(), "Initialized live D3D12 SM 6.6 GPU context with Debug Layer");
    if (!gpu.dev) return 1;

    test_dynamic_mesh_and_amplification_shaders(gpu);
    test_dead_vgpr_sgpr_elimination(gpu);
    test_multi_exit_relooper_loop(gpu);
    test_native_fp16_vop3p(gpu);

    const uint64_t dbg_errs = gpu.debug_error_count();
    CHECK(dbg_errs == 0u, "D3D12 Debug Layer reported %llu errors across all Wave 9 GPU tests",
          static_cast<unsigned long long>(dbg_errs));

    std::printf("\n=== Wave 9 Summary: failures=%d ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
