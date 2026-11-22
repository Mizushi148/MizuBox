// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Wave 10 Live RTX 5060 Verification Suite (D3D12 Debug Layer: 0 errors):
//  1. Extended Xbox D3D12.X Binary ABI vtable (CreateGraphicsPipelineStateX @61, CreatePlacedResourceX @77,
//     PIX SetMarker/BeginEvent/EndEvent @60..62, BeginRenderPass/EndRenderPass @71..72,
//     ExecuteIndirect @63, DispatchMesh @83 -> D3D12XboxGetExtendedAbiStats).
//  2. SM 6.6 RDNA2 AutoBindContext (d3d12_xs_sm66_autobind.h): 1-call bind_compute() resolving a real
//     RGBA8 texture via FNV-1a T#/S# hash table + writing via 64-bit GPU VA slab on RTX 5060.
//  3. High-Precision Hybrid QPC Frame Pacer (D3D12XboxConfigureFramePacing(120, TRUE) +
//     D3D12XboxPresentX + D3D12XboxGetFramePacingStats P99 / 1% Low FPS).

#include "include/xbox/d3d12_xs.h"
#include "d3d12_xs_sm66_autobind.h"

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

bool exec_and_wait(ID3D12Device* dev, ID3D12CommandQueue* q, ID3D12GraphicsCommandList* cl) {
    cl->Close();
    ID3D12CommandList* lists[] = {cl};
    q->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> f;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)))) return false;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    q->Signal(f.Get(), 1);
    if (f->GetCompletedValue() < 1) {
        f->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, 5000);
    }
    CloseHandle(ev);
    return true;
}

void test_extended_xbox_abi(ID3D12Device* dev, ID3D12CommandQueue* q) {
    std::printf("\n=== [Wave 10.1] Extended Xbox D3D12.X Binary ABI (MS/Indirect/RenderPass/PIX) ===\n");

    ComPtr<IUnknown> xdev_unk;
    CHECK(SUCCEEDED(D3D12XboxGetXboxAbiDevice(dev, IID_PPV_ARGS(&xdev_unk))) && xdev_unk,
          "Acquired WsxXboxDeviceAbiProxy");
    if (!xdev_unk) return;

    void** dvt = *reinterpret_cast<void***>(xdev_unk.Get());

    // Compile minimal VS + PS + CS + MS via DXC
    const char* vs_src = "float4 main(uint id : SV_VertexID) : SV_Position { return float4(0,0,0,1); }";
    const char* ps_src = "float4 main() : SV_Target { return float4(0,1,0,1); }";
    const char* cs_src = "[numthreads(1,1,1)] void main() {}";
    const char* ms_src =
        "struct V { float4 p : SV_Position; };\n"
        "[outputtopology(\"triangle\")] [numthreads(1,1,1)]\n"
        "void main(out vertices V v[3], out indices uint3 t[1]) {\n"
        "  SetMeshOutputCounts(3, 1);\n"
        "  v[0].p = float4(-1,-1,0,1); v[1].p = float4(3,-1,0,1); v[2].p = float4(-1,3,0,1);\n"
        "  t[0] = uint3(0,1,2);\n"
        "}\n";
    std::vector<uint8_t> vs_dxil, ps_dxil, cs_dxil, ms_dxil;
    char err[512]{};
    CHECK(SUCCEEDED(wsx_sm66::compile_hlsl_to_dxil_vec(vs_src, std::strlen(vs_src), L"main", L"vs_6_6", vs_dxil, err, sizeof(err))) &&
          SUCCEEDED(wsx_sm66::compile_hlsl_to_dxil_vec(ps_src, std::strlen(ps_src), L"main", L"ps_6_6", ps_dxil, err, sizeof(err))) &&
          SUCCEEDED(wsx_sm66::compile_hlsl_to_dxil_vec(cs_src, std::strlen(cs_src), L"main", L"cs_6_6", cs_dxil, err, sizeof(err))) &&
          SUCCEEDED(wsx_sm66::compile_hlsl_to_dxil_vec(ms_src, std::strlen(ms_src), L"main", L"ms_6_6", ms_dxil, err, sizeof(err))),
          "Compiled VS/PS/CS/MS shaders for Xbox ABI verification");

    // Empty root signature
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
    rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    ComPtr<ID3DBlob> rsb, rse;
    D3D12SerializeVersionedRootSignature(&rsd, &rsb, &rse);
    ComPtr<ID3D12RootSignature> rs;
    dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&rs));

    // 1) Slot 61 (0x1E8): CreateGraphicsPipelineStateX
    using FnCreateGfxPsoX = HRESULT(STDMETHODCALLTYPE*)(void*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, const void*, const void*, REFIID, void**);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gpd{};
    gpd.pRootSignature = rs.Get();
    gpd.VS = {vs_dxil.data(), vs_dxil.size()};
    gpd.PS = {ps_dxil.data(), ps_dxil.size()};
    gpd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gpd.SampleMask = UINT_MAX;
    gpd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gpd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gpd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gpd.NumRenderTargets = 1;
    gpd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gpd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> gfx_pso;
    const HRESULT hr_gpso = reinterpret_cast<FnCreateGfxPsoX>(dvt[61])(
        xdev_unk.Get(), &gpd, nullptr, nullptr, IID_PPV_ARGS(&gfx_pso));
    CHECK(SUCCEEDED(hr_gpso) && gfx_pso, "Xbox ABI slot 61 (0x1E8) CreateGraphicsPipelineStateX succeeded");

    // 2) Slot 77 (0x268): CreatePlacedResourceX
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = 65536;
    hd.Properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ComPtr<ID3D12Heap> heap;
    dev->CreateHeap(&hd, IID_PPV_ARGS(&heap));

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    using FnCreatePlacedX = HRESULT(STDMETHODCALLTYPE*)(void*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
    ComPtr<ID3D12Resource> indirect_args;
    const HRESULT hr_pl = reinterpret_cast<FnCreatePlacedX>(dvt[77])(
        xdev_unk.Get(), heap.Get(), 0, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&indirect_args));
    CHECK(SUCCEEDED(hr_pl) && indirect_args, "Xbox ABI slot 77 (0x268) CreatePlacedResourceX succeeded");

    // Populate D3D12_DISPATCH_ARGUMENTS {1, 1, 1} into placed indirect buffer
    if (indirect_args) {
        D3D12_DISPATCH_ARGUMENTS* da = nullptr;
        indirect_args->Map(0, nullptr, reinterpret_cast<void**>(&da));
        da->ThreadGroupCountX = 1; da->ThreadGroupCountY = 1; da->ThreadGroupCountZ = 1;
        indirect_args->Unmap(0, nullptr);
    }

    // Create compute PSO + command signature for ExecuteIndirect + Mesh PSO for DispatchMesh
    D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
    cpd.pRootSignature = rs.Get();
    cpd.CS = {cs_dxil.data(), cs_dxil.size()};
    ComPtr<ID3D12PipelineState> cs_pso;
    dev->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&cs_pso));

    D3D12_INDIRECT_ARGUMENT_DESC iad{};
    iad.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC csd{};
    csd.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    csd.NumArgumentDescs = 1;
    csd.pArgumentDescs = &iad;
    ComPtr<ID3D12CommandSignature> cmd_sig;
    dev->CreateCommandSignature(&csd, nullptr, IID_PPV_ARGS(&cmd_sig));

    struct alignas(void*) MsStream {
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
    } ms_stream{};
    ms_stream.rs = rs.Get();
    ms_stream.ms = {ms_dxil.data(), ms_dxil.size()};
    ms_stream.ps = {ps_dxil.data(), ps_dxil.size()};
    ms_stream.rast.FillMode = D3D12_FILL_MODE_SOLID;
    ms_stream.rast.CullMode = D3D12_CULL_MODE_NONE;
    ms_stream.rtv.NumRenderTargets = 1;
    ms_stream.rtv.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D12Device2> dev2;
    dev->QueryInterface(IID_PPV_ARGS(&dev2));
    D3D12_PIPELINE_STATE_STREAM_DESC mssd{sizeof(ms_stream), &ms_stream};
    ComPtr<ID3D12PipelineState> ms_pso;
    dev2->CreatePipelineState(&mssd, IID_PPV_ARGS(&ms_pso));

    // RenderTarget for BeginRenderPass / DispatchMesh
    D3D12_HEAP_PROPERTIES hp_def{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rtd{};
    rtd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtd.Width = 32; rtd.Height = 32; rtd.DepthOrArraySize = 1; rtd.MipLevels = 1;
    rtd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rtd.SampleDesc.Count = 1;
    rtd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> rt;
    dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &rtd,
                                 D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt));
    D3D12_DESCRIPTOR_HEAP_DESC rhd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    dev->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap));
    dev->CreateRenderTargetView(rt.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());

    // Create Xbox-ABI wrapped command list via slot 12 (0x060)
    ComPtr<ID3D12CommandAllocator> alloc;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    using FnCreateCl = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
    ComPtr<IUnknown> xcl_unk;
    CHECK(SUCCEEDED(reinterpret_cast<FnCreateCl>(dvt[12])(
              xdev_unk.Get(), 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), cs_pso.Get(), IID_PPV_ARGS(&xcl_unk))) && xcl_unk,
          "Created WsxXboxCommandListAbiProxy via Xbox ABI slot 12");

    if (xcl_unk) {
        void** cvt = *reinterpret_cast<void***>(xcl_unk.Get());
        // PIX markers @ 60, 61, 62
        using FnMarker = void(STDMETHODCALLTYPE*)(void*, UINT, const void*, UINT);
        using FnEndEvt = void(STDMETHODCALLTYPE*)(void*);
        reinterpret_cast<FnMarker>(cvt[61])(xcl_unk.Get(), 1u, "Wave10_Frame", 13u);
        reinterpret_cast<FnMarker>(cvt[60])(xcl_unk.Get(), 1u, "Wave10_Mark", 12u);

        // SetComputeRootSignature @ 33 + ExecuteIndirect @ 63
        using FnSetRs = void(STDMETHODCALLTYPE*)(void*, ID3D12RootSignature*);
        using FnExecInd = void(STDMETHODCALLTYPE*)(void*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
        reinterpret_cast<FnSetRs>(cvt[33])(xcl_unk.Get(), rs.Get());
        reinterpret_cast<FnExecInd>(cvt[63])(xcl_unk.Get(), cmd_sig.Get(), 1u, indirect_args.Get(), 0, nullptr, 0);

        // BeginRenderPass @ 71 + SetGraphicsRootSignature @ 34 + SetPSO @ 25 + DispatchMesh @ 83 + EndRenderPass @ 72
        D3D12_RENDER_PASS_RENDER_TARGET_DESC rp_rt{};
        rp_rt.cpuDescriptor = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rp_rt.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
        rp_rt.BeginningAccess.Clear.ClearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rp_rt.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;

        using FnBeginRP = void(STDMETHODCALLTYPE*)(void*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS);
        using FnEndRP = void(STDMETHODCALLTYPE*)(void*);
        using FnSetPso = void(STDMETHODCALLTYPE*)(void*, ID3D12PipelineState*);
        using FnSetVp = void(STDMETHODCALLTYPE*)(void*, UINT, const D3D12_VIEWPORT*);
        using FnSetSc = void(STDMETHODCALLTYPE*)(void*, UINT, const D3D12_RECT*);
        using FnDispMesh = void(STDMETHODCALLTYPE*)(void*, UINT, UINT, UINT);
        using FnClose = HRESULT(STDMETHODCALLTYPE*)(void*);

        D3D12_VIEWPORT vp{0, 0, 32, 32, 0, 1};
        D3D12_RECT sc{0, 0, 32, 32};
        reinterpret_cast<FnBeginRP>(cvt[71])(xcl_unk.Get(), 1u, &rp_rt, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
        reinterpret_cast<FnSetVp>(cvt[21])(xcl_unk.Get(), 1u, &vp);
        reinterpret_cast<FnSetSc>(cvt[22])(xcl_unk.Get(), 1u, &sc);
        reinterpret_cast<FnSetRs>(cvt[34])(xcl_unk.Get(), rs.Get());
        reinterpret_cast<FnSetPso>(cvt[25])(xcl_unk.Get(), ms_pso.Get());
        reinterpret_cast<FnDispMesh>(cvt[83])(xcl_unk.Get(), 1u, 1u, 1u);
        reinterpret_cast<FnEndRP>(cvt[72])(xcl_unk.Get());
        reinterpret_cast<FnEndEvt>(cvt[62])(xcl_unk.Get());
        reinterpret_cast<FnClose>(cvt[9])(xcl_unk.Get());

        // Execute the underlying unwrapped command list on RTX 5060!
        ComPtr<ID3D12GraphicsCommandList> raw_cl;
        // QueryInterface for our internal marker or cast through d3d12_xs queue execution
        ID3D12CommandList* raw_list = nullptr;
        static const GUID kXboxClMarker = {0x9521a390, 0x3b6e, 0x45df, {0x8e, 0x60, 0x2c, 0x79, 0x43, 0x9b, 0x51, 0x0b}};
        if (SUCCEEDED(xcl_unk->QueryInterface(kXboxClMarker, reinterpret_cast<void**>(&raw_list))) && raw_list) {
            q->ExecuteCommandLists(1, &raw_list);
            ComPtr<ID3D12Fence> f;
            dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f));
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            q->Signal(f.Get(), 1);
            if (f->GetCompletedValue() < 1) { f->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, 5000); }
            CloseHandle(ev);
        }
    }

    D3D12XBOX_EXTENDED_ABI_STATS st{};
    CHECK(SUCCEEDED(D3D12XboxGetExtendedAbiStats(&st)) &&
          st.graphics_pso_x_ok >= 1 && st.placed_resources_x_ok >= 1 &&
          st.dispatch_mesh_calls >= 1 && st.render_pass_calls >= 1 &&
          st.execute_indirect_calls >= 1 && st.pix_markers_recorded >= 3 &&
          st.unhandled_device_slots == 0 && st.unhandled_cmdlist_slots == 0,
          "Extended Xbox ABI verified live: gpsoX=%llu placedX=%llu mesh=%llu rp=%llu indirect=%llu pix=%llu unhandled=%llu/%llu",
          st.graphics_pso_x_ok, st.placed_resources_x_ok, st.dispatch_mesh_calls, st.render_pass_calls,
          st.execute_indirect_calls, st.pix_markers_recorded, st.unhandled_device_slots, st.unhandled_cmdlist_slots);
}

void test_sm66_autobind_runtime(ID3D12Device* dev, ID3D12CommandQueue* q) {
    std::printf("\n=== [Wave 10.2] Automatic SM 6.6 RDNA2 Bindless Slab + T#/S# AutoBindContext ===\n");

    wsx_sm66::AutoBindContext binder;
    CHECK(binder.initialize(dev, &D3D12SerializeVersionedRootSignature, 0x10000000ull, 12u, 4u, 64u),
          "Initialized WsxSm66::AutoBindContext (4 slabs, 64-entry FNV-1a T#/S# hash table)");

    // Create slab0 buffer + readback buffer
    D3D12_HEAP_PROPERTIES hp_def{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_HEAP_PROPERTIES hp_up{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_HEAP_PROPERTIES hp_rb{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 4096, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    ComPtr<ID3D12Resource> slab0, rb;
    dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slab0));
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;
    dev->CreateCommittedResource(&hp_rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));
    CHECK(binder.register_slab(0u, slab0.Get(), 4096u), "Registered 64-bit GPU VA slab0 @ 0x10000000");

    // Create 4x4 bright cyan RGBA8 texture (R=0, G=255, B=255, A=255 -> 0xFFFFFF00)
    D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    ComPtr<ID3D12Resource> tex, tex_up;
    dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex));
    D3D12_RESOURCE_DESC ubd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 1024, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    dev->CreateCommittedResource(&hp_up, D3D12_HEAP_FLAG_NONE, &ubd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tex_up));
    {
        uint8_t* p = nullptr;
        tex_up->Map(0, nullptr, reinterpret_cast<void**>(&p));
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                uint8_t* px = p + y * 256 + x * 4;
                px[0] = 0x10; px[1] = 0xE0; px[2] = 0xF0; px[3] = 0xFF;
            }
        }
        tex_up->Unmap(0, nullptr);
    }

    const uint32_t t_key[8] = {0xAA01, 0xAA02, 0xAA03, 0xAA04, 0xAA05, 0xAA06, 0xAA07, 0xAA08};
    const uint32_t s_key[4] = {0xBB01, 0xBB02, 0xBB03, 0xBB04};
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Texture2D.MipLevels = 1;
    D3D12_SAMPLER_DESC smpd{};
    smpd.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    smpd.AddressU = smpd.AddressV = smpd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    CHECK(binder.register_texture_srv(t_key, tex.Get(), &srvd) && binder.register_sampler(s_key, &smpd),
          "Registered Xbox T#/S# keys into AutoBindContext FNV-1a table (descs=%u)", binder.registered_descriptors());

    // Compile SM 6.6 shader using preamble's wsx_lookup_tsharp/ssharp + wsx_va_store32
    std::string body = R"HLSL(
[numthreads(1,1,1)]
void main() {
    uint4 tk0 = uint4(0xAA01u, 0xAA02u, 0xAA03u, 0xAA04u);
    uint4 tk1 = uint4(0xAA05u, 0xAA06u, 0xAA07u, 0xAA08u);
    uint4 sk  = uint4(0xBB01u, 0xBB02u, 0xBB03u, 0xBB04u);
    uint ti = wsx_lookup_tsharp(tk0, tk1);
    uint si = wsx_lookup_ssharp(sk);
    Texture2D<float4> tex = ResourceDescriptorHeap[NonUniformResourceIndex(ti)];
    SamplerState smp = SamplerDescriptorHeap[NonUniformResourceIndex(si)];
    float4 c = tex.SampleLevel(smp, float2(0.5f, 0.5f), 0.0f);
    uint packed = (uint(round(c.x * 255.0f)) & 0xFFu) |
                  ((uint(round(c.y * 255.0f)) & 0xFFu) << 8u) |
                  ((uint(round(c.z * 255.0f)) & 0xFFu) << 16u) |
                  ((uint(round(c.w * 255.0f)) & 0xFFu) << 24u);
    uint64_t va = uint64_t(wsx_sgpr(0u)) | (uint64_t(wsx_sgpr(1u)) << 32ull);
    wsx_va_store32(va, packed);
}
)HLSL";
    const std::string hlsl = wsx_sm66::build_trimmed_sm66_preamble(body) + body;
    std::vector<uint8_t> dxil;
    char err[1024]{};
    const HRESULT hr_c = wsx_sm66::compile_hlsl_to_dxil_vec(hlsl.c_str(), hlsl.size(), L"main", L"cs_6_6", dxil, err, sizeof(err));
    CHECK(SUCCEEDED(hr_c) && !dxil.empty(), "Compiled AutoBindContext SM 6.6 test shader (%zu B, err=%s)", dxil.size(), err);

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = binder.root_signature();
    pd.CS = {dxil.data(), dxil.size()};
    ComPtr<ID3D12PipelineState> pso;
    dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cl;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso.Get(), IID_PPV_ARGS(&cl));

    // Upload texture
    D3D12_TEXTURE_COPY_LOCATION src_l{tex_up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    src_l.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 256};
    D3D12_TEXTURE_COPY_LOCATION dst_l{tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    cl->CopyTextureRegion(&dst_l, 0, 0, 0, &src_l, nullptr);
    D3D12_RESOURCE_BARRIER bars[2]{};
    bars[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bars[0].Transition = {tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    bars[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bars[1].Transition = {slab0.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    cl->ResourceBarrier(2, bars);

    // ONE-CALL bind_compute()!
    const uint32_t sgprs[2] = {0x10000000u, 0u};
    binder.bind_compute(cl.Get(), sgprs, 2u);
    cl->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER b2{};
    b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b2.Transition = {slab0.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    cl->ResourceBarrier(1, &b2);
    cl->CopyBufferRegion(rb.Get(), 0, slab0.Get(), 0, 4);
    exec_and_wait(dev, q, cl.Get());

    uint32_t* out = nullptr;
    rb->Map(0, nullptr, reinterpret_cast<void**>(&out));
    const uint32_t val = out ? out[0] : 0u;
    rb->Unmap(0, nullptr);
    CHECK(val == 0xFFF0E010u,
          "Live RTX 5060 AutoBindContext bind_compute() sampled T#/S# -> slab0 exact (got=0x%08X expected=0xFFF0E010)", val);
}

void test_hybrid_frame_pacer(ID3D12Device* dev, ID3D12CommandQueue* q) {
    std::printf("\n=== [Wave 10.3] High-Precision Hybrid QPC Frame Pacer (max_fps + P99/1%% Low) ===\n");

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WsxWave10PacerWin";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"WSX Pacer Test", WS_OVERLAPPEDWINDOW,
                                0, 0, 160, 120, nullptr, nullptr, wc.hInstance, nullptr);

    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = 160; rd.Height = 120; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> rt;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                 D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&rt));

    CHECK(SUCCEEDED(D3D12XboxConfigureFramePacing(120u, TRUE)), "Configured hybrid frame pacer to 120 FPS (8.33 ms)");

    ID3D12Resource* res_arr[1] = {rt.Get()};
    D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
    plane.Token = 1;
    plane.ResourceCount = 1;
    plane.ppResources = res_arr;
    D3D12XBOX_PRESENT_PARAMETERS pp{};
    pp.Flags = 1u; // tearing / unlocked swapchain so pacer governs timing cleanly

    for (int f = 0; f < 10; ++f) {
        D3D12XboxPresentX(q, 1, &plane, &pp);
    }
    D3D12XboxPresentX(q, 0, nullptr, nullptr); // flush swapchain cleanly

    D3D12XBOX_FRAME_PACING_STATS fps{};
    CHECK(SUCCEEDED(D3D12XboxGetFramePacingStats(&fps)) &&
          fps.target_fps == 120u && fps.paced_frames >= 10u &&
          fps.avg_fps >= 40.0 && fps.avg_fps <= 145.0 &&
          fps.p99_frame_ms > 0.0 && fps.one_percent_low_fps > 0.0,
          "Frame pacer telemetry: target=%u paced=%llu avg=%.1f FPS 1%%Low=%.1f FPS p99=%.2f ms",
          fps.target_fps, fps.paced_frames, fps.avg_fps, fps.one_percent_low_fps, fps.p99_frame_ms);

    D3D12XboxConfigureFramePacing(0u, TRUE);
    if (hwnd) DestroyWindow(hwnd);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetEnvironmentVariableW(L"WSX_DEBUG_LAYER", L"1");

    D3D12XBOX_CREATE_DEVICE_PARAMETERS cdp{};
    cdp.Version = D3D12_SDK_VERSION;
    ComPtr<ID3D12Device> dev;
    CHECK(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &cdp, IID_PPV_ARGS(&dev))) && dev,
          "Created d3d12_xs device under D3D12 Debug Layer");
    if (!dev) return 1;

    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    ComPtr<ID3D12CommandQueue> q;
    CHECK(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) && q, "Created Direct CommandQueue");

    test_extended_xbox_abi(dev.Get(), q.Get());
    test_sm66_autobind_runtime(dev.Get(), q.Get());
    test_hybrid_frame_pacer(dev.Get(), q.Get());

    ComPtr<ID3D12InfoQueue> iq;
    uint64_t errs = 0;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&iq))) && iq) {
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
    }
    CHECK(errs == 0u, "D3D12 Debug Layer reported %llu errors across all Wave 10 tests", static_cast<unsigned long long>(errs));

    std::printf("\n=== Wave 10 Summary: failures=%d ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
