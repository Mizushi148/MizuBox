// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Pure Direct3D 12 (DXR Tier 1.1 + SM 6.6 RayQuery) Test for our clean-room XboxBvhBuilder.dll bridge:
// 1. Verifies all 12 exact MSVC mangled C++ exports of Microsoft's XboxBvhBuilder.dll are present.
// 2. Constructs XboxBvhBuilder in a 24-byte stack slot using d3d12_xs.dll's native Xbox ABI device
//    (WsxXboxDeviceAbiProxy) and Xbox ABI command list (WsxXboxCommandListAbiProxy).
// 3. Builds a real hardware BLAS (1 triangle), clones it via CopyRaytracingAccelerationStructure(CLONE),
//    and builds a 1-instance TLAS via BuildRaytracingAccelerationStructureBatched2X.
// 4. Compiles an SM 6.6 RayQuery<RAY_FLAG_NONE> compute shader at runtime via D3D12XboxCompileHlslToDxil
//    and traces HIT and MISS rays on the real GPU, asserting exact RayT, InstanceID, and barycentrics.
// 5. Asserts 0 D3D12 Debug Layer errors / corruptions.

#include "include/xbox/d3d12_xs.h"
#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

#define CHECK_HR(expr)                                                                 \
    do {                                                                               \
        HRESULT _hr = (expr);                                                          \
        if (FAILED(_hr)) {                                                             \
            std::fprintf(stderr, "FAIL: %s returned 0x%08lX at line %d\n",             \
                         #expr, static_cast<unsigned long>(_hr), __LINE__);            \
            return 1;                                                                  \
        }                                                                              \
    } while (0)

#define CHECK_COND(cond, msg)                                                          \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "FAIL: %s (%s) at line %d\n", msg, #cond, __LINE__);  \
            return 1;                                                                  \
        }                                                                              \
    } while (0)

namespace {

struct D3D12XBOX_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2 {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC Desc;
    UINT NumPostbuildInfoDescs;
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pPostbuildInfoDescs;
};

struct WsxXboxBvhBridgeStats {
    uint64_t InstancesCreated;
    uint64_t PrebuildInfoCalls;
    uint64_t BuildCalls;
    uint64_t BatchedBuildCalls;
    uint64_t CopyCalls;
    uint64_t RehydrateCalls;
    uint64_t ValidateCalls;
};

using FnBvhCtor = void*(__fastcall*)(void*, void*, bool);
using FnBvhDtor = void(__fastcall*)(void*);
using FnBvhPrebuild = void(__fastcall*)(
    void*,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS*,
    const void*,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO*);
using FnBvhBuild = void(__fastcall*)(
    void*,
    void*,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*,
    UINT,
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC*);
using FnBvhBatched2X = void(__fastcall*)(
    void*,
    void*,
    UINT,
    const D3D12XBOX_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2*);
using FnBvhCopy = void(__fastcall*)(
    void*,
    void*,
    D3D12_GPU_VIRTUAL_ADDRESS,
    D3D12_GPU_VIRTUAL_ADDRESS,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE);
using FnBvhValidate = HRESULT(__fastcall*)(void*, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*);
using FnBvhSetMorton = void(__fastcall*)(void*, bool);
using FnBvhStats = HRESULT(WINAPI*)(WsxXboxBvhBridgeStats*);

struct RayResult {
    uint32_t status;
    uint32_t inst_id;
    uint32_t prim_idx;
    float ray_t;
    float bary_u;
    float bary_v;
};

uint64_t align256(uint64_t n) noexcept {
    return (n < 256ull) ? 256ull : ((n + 255ull) & ~255ull);
}

HRESULT create_buffer(
    ID3D12Device* dev,
    D3D12_HEAP_TYPE heap_type,
    uint64_t bytes,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initial_state,
    ID3D12Resource** out) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap_type;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = align256(bytes);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initial_state,
                                        nullptr, IID_PPV_ARGS(out));
}

D3D12_RESOURCE_BARRIER uav_barrier(ID3D12Resource* res) noexcept {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = res;
    return b;
}

D3D12_RESOURCE_BARRIER transition_barrier(
    ID3D12Resource* res,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

} // namespace

int main(int argc, char** argv) {
    const char* dll_path = (argc > 1 && argv[1]) ? argv[1] : "XboxBvhBuilder.dll";
    HMODULE mod = LoadLibraryA(dll_path);
    CHECK_COND(mod != nullptr, "Failed to load XboxBvhBuilder.dll");

    // 1. Verify all 12 exact Microsoft XboxBvhBuilder.dll mangled C++ exports exist
    const char* kRequiredExports[12] = {
        "??0XboxBvhBuilder@@QEAA@PEAUID3D12Device11@@_N@Z",
        "??1XboxBvhBuilder@@QEAA@XZ",
        "?BuildRaytracingAccelerationStructure@XboxBvhBuilder@@QEAAXPEAUID3D12GraphicsCommandList4@@PEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC@@IPEBUD3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC@@@Z",
        "?BuildRaytracingAccelerationStructureBatched2X@XboxBvhBuilder@@QEAAXPEAUID3D12GraphicsCommandList6@@IPEBUD3D12XBOX_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2@@@Z",
        "?CopyRaytracingAccelerationStructure@XboxBvhBuilder@@QEAAXPEAUID3D12GraphicsCommandList4@@_K1W4D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE@@@Z",
        "?CpuValidateResultX@XboxBvhBuilder@@QEAAXPEAUID3D12GraphicsCommandList4@@PEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC@@@Z",
        "?GetRaytracingAccelerationStructurePrebuildInfo@XboxBvhBuilder@@QEAAXPEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS@@PEBUD3D12XBOX_XDXR_STREAM_DESC@@PEAUD3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO@@@Z",
        "?RehydrateRaytracingAccelerationStructureBatched2X@XboxBvhBuilder@@QEAAJPEAUID3D12GraphicsCommandList8@@IPEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC@@PEB_K@Z",
        "?RehydrateRaytracingAccelerationStructureBatchedX@XboxBvhBuilder@@QEAAJPEAUID3D12GraphicsCommandList8@@IPEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC@@@Z",
        "?RehydrateRaytracingAccelerationStructureX@XboxBvhBuilder@@QEAAJPEAUID3D12Device11@@PEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC@@@Z",
        "?Set64BitMortonCodes@XboxBvhBuilder@@QEAAX_N@Z",
        "?ValidateRehydrateAccelerationStructureBuildDesc@XboxBvhBuilder@@QEAAJPEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC@@@Z",
    };
    for (const char* sym : kRequiredExports) {
        if (!GetProcAddress(mod, sym)) {
            std::fprintf(stderr, "FAIL: missing export %s\n", sym);
            FreeLibrary(mod);
            return 1;
        }
    }
    auto fn_ctor = reinterpret_cast<FnBvhCtor>(GetProcAddress(mod, kRequiredExports[0]));
    auto fn_dtor = reinterpret_cast<FnBvhDtor>(GetProcAddress(mod, kRequiredExports[1]));
    auto fn_build = reinterpret_cast<FnBvhBuild>(GetProcAddress(mod, kRequiredExports[2]));
    auto fn_batched = reinterpret_cast<FnBvhBatched2X>(GetProcAddress(mod, kRequiredExports[3]));
    auto fn_copy = reinterpret_cast<FnBvhCopy>(GetProcAddress(mod, kRequiredExports[4]));
    auto fn_prebuild = reinterpret_cast<FnBvhPrebuild>(GetProcAddress(mod, kRequiredExports[6]));
    auto fn_morton = reinterpret_cast<FnBvhSetMorton>(GetProcAddress(mod, kRequiredExports[10]));
    auto fn_validate = reinterpret_cast<FnBvhValidate>(GetProcAddress(mod, kRequiredExports[11]));
    auto fn_stats = reinterpret_cast<FnBvhStats>(GetProcAddress(mod, "WsxXboxBvhBuilderGetStats"));
    CHECK_COND(fn_stats != nullptr, "WsxXboxBvhBuilderGetStats missing");

    // 2. Enable D3D12 Debug Layer and create device via d3d12_xs.dll
    {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(dbg.GetAddressOf())))) {
            dbg->EnableDebugLayer();
        }
    }
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    params.ProcessDebugFlags = D3D12_PROCESS_DEBUG_FLAG_DEBUG_LAYER_ENABLED;
    ComPtr<ID3D12Device> dev;
    CHECK_HR(D3D12XboxCreateDevice(nullptr, &params, IID_PPV_ARGS(dev.GetAddressOf())));

    ComPtr<ID3D12InfoQueue> info_queue;
    dev.As(&info_queue);
    if (info_queue) info_queue->ClearStoredMessages();

    D3D12XBOX_SM66_CAPS caps{};
    CHECK_HR(D3D12XboxCheckSm66Support(dev.Get(), &caps));
    CHECK_COND(caps.RaytracingTier >= 11u, "GPU must support DXR Tier 1.1+ for hardware RayQuery");
    std::printf("[BVH-DXR] GPU RaytracingTier=%u (11=DXR1.1, 12=DXR1.2)\n", caps.RaytracingTier);

    // Wrap both device and command list in native Xbox binary ABI proxies!
    ComPtr<IUnknown> xbox_abi_dev;
    CHECK_HR(D3D12XboxGetXboxAbiDevice(dev.Get(), IID_PPV_ARGS(xbox_abi_dev.GetAddressOf())));

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    CHECK_HR(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(queue.GetAddressOf())));
    ComPtr<ID3D12CommandAllocator> alloc;
    CHECK_HR(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.GetAddressOf())));
    ComPtr<ID3D12GraphicsCommandList> cl;
    CHECK_HR(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(cl.GetAddressOf())));

    void* xbox_abi_cl = nullptr;
    CHECK_HR(D3D12XboxWrapCommandListForXboxAbi(cl.Get(), &xbox_abi_cl));
    CHECK_COND(xbox_abi_cl != nullptr, "Xbox ABI command list wrap failed");

    // 3. Construct XboxBvhBuilder in an exact 24-byte stack buffer
    alignas(16) uint8_t bvh_storage[24]{};
    fn_ctor(bvh_storage, xbox_abi_dev.Get(), true);
    fn_morton(bvh_storage, true);

    // 4. Upload 1 triangle: v0=(-1,-1,1), v1=(1,-1,1), v2=(0,1,1)
    const float verts[9] = {
        -1.0f, -1.0f, 1.0f,
         1.0f, -1.0f, 1.0f,
         0.0f,  1.0f, 1.0f,
    };
    ComPtr<ID3D12Resource> vb;
    CHECK_HR(create_buffer(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(verts),
                           D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                           vb.GetAddressOf()));
    {
        void* p = nullptr;
        CHECK_HR(vb->Map(0, nullptr, &p));
        std::memcpy(p, verts, sizeof(verts));
        vb->Unmap(0, nullptr);
    }

    D3D12_RAYTRACING_GEOMETRY_DESC geom{};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress();
    geom.Triangles.VertexBuffer.StrideInBytes = 12;
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.VertexCount = 3;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_in{};
    blas_in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    // Intentionally include an Xbox-only high flag bit (0x80000000) to test sanitization!
    blas_in.Flags = static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | 0x80000000u);
    blas_in.NumDescs = 1;
    blas_in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    blas_in.pGeometryDescs = &geom;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blas_info{};
    fn_prebuild(bvh_storage, &blas_in, nullptr, &blas_info);
    CHECK_COND(blas_info.ResultDataMaxSizeInBytes > 0 && blas_info.ScratchDataSizeInBytes > 0,
               "BLAS prebuild sizes must be > 0");
    std::printf("[BVH-DXR] BLAS PrebuildInfo: result=%llu scratch=%llu\n",
                static_cast<unsigned long long>(blas_info.ResultDataMaxSizeInBytes),
                static_cast<unsigned long long>(blas_info.ScratchDataSizeInBytes));

    ComPtr<ID3D12Resource> blas_buf, blas_clone_buf, blas_scratch;
    CHECK_HR(create_buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, blas_info.ResultDataMaxSizeInBytes,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, blas_buf.GetAddressOf()));
    CHECK_HR(create_buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, blas_info.ResultDataMaxSizeInBytes,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, blas_clone_buf.GetAddressOf()));
    CHECK_HR(create_buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, blas_info.ScratchDataSizeInBytes,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, blas_scratch.GetAddressOf()));

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blas_desc{};
    blas_desc.DestAccelerationStructureData = blas_buf->GetGPUVirtualAddress();
    blas_desc.Inputs = blas_in;
    blas_desc.ScratchAccelerationStructureData = blas_scratch->GetGPUVirtualAddress();
    CHECK_HR(fn_validate(bvh_storage, &blas_desc));

    // Build BLAS + UAV barrier + CLONE BLAS + UAV barrier
    fn_build(bvh_storage, xbox_abi_cl, &blas_desc, 0, nullptr);
    D3D12_RESOURCE_BARRIER b1 = uav_barrier(blas_buf.Get());
    cl->ResourceBarrier(1, &b1);
    fn_copy(bvh_storage, xbox_abi_cl,
            blas_clone_buf->GetGPUVirtualAddress(),
            blas_buf->GetGPUVirtualAddress(),
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);
    D3D12_RESOURCE_BARRIER b2 = uav_barrier(blas_clone_buf.Get());
    cl->ResourceBarrier(1, &b2);

    // 5. Build 1-instance TLAS referencing the CLONED BLAS via Batched2X
    D3D12_RAYTRACING_INSTANCE_DESC inst{};
    inst.Transform[0][0] = 1.0f;
    inst.Transform[1][1] = 1.0f;
    inst.Transform[2][2] = 1.0f;
    inst.InstanceID = 0x42u;
    inst.InstanceMask = 0xFFu;
    inst.AccelerationStructure = blas_clone_buf->GetGPUVirtualAddress();

    ComPtr<ID3D12Resource> inst_buf;
    CHECK_HR(create_buffer(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(inst),
                           D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                           inst_buf.GetAddressOf()));
    {
        void* p = nullptr;
        CHECK_HR(inst_buf->Map(0, nullptr, &p));
        std::memcpy(p, &inst, sizeof(inst));
        inst_buf->Unmap(0, nullptr);
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_in{};
    tlas_in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlas_in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tlas_in.NumDescs = 1;
    tlas_in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlas_in.InstanceDescs = inst_buf->GetGPUVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlas_info{};
    fn_prebuild(bvh_storage, &tlas_in, nullptr, &tlas_info);
    CHECK_COND(tlas_info.ResultDataMaxSizeInBytes > 0 && tlas_info.ScratchDataSizeInBytes > 0,
               "TLAS prebuild sizes must be > 0");

    ComPtr<ID3D12Resource> tlas_buf, tlas_scratch;
    CHECK_HR(create_buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, tlas_info.ResultDataMaxSizeInBytes,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, tlas_buf.GetAddressOf()));
    CHECK_HR(create_buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, tlas_info.ScratchDataSizeInBytes,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, tlas_scratch.GetAddressOf()));

    D3D12XBOX_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2 tlas_batch{};
    tlas_batch.Desc.DestAccelerationStructureData = tlas_buf->GetGPUVirtualAddress();
    tlas_batch.Desc.Inputs = tlas_in;
    tlas_batch.Desc.ScratchAccelerationStructureData = tlas_scratch->GetGPUVirtualAddress();
    fn_batched(bvh_storage, xbox_abi_cl, 1, &tlas_batch);
    D3D12_RESOURCE_BARRIER b3 = uav_barrier(tlas_buf.Get());
    cl->ResourceBarrier(1, &b3);

    // 6. Compile SM 6.6 RayQuery<RAY_FLAG_NONE> compute shader via D3D12XboxCompileHlslToDxil
    const char* rq_hlsl = R"HLSL(
struct RayResult {
    uint status;
    uint inst_id;
    uint prim_idx;
    float ray_t;
    float bary_u;
    float bary_v;
};

RaytracingAccelerationStructure g_tlas : register(t0);
RWStructuredBuffer<RayResult> g_out : register(u0);

[numthreads(2, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    RayDesc ray;
    ray.Origin = float3(0.0, 0.0, 0.0);
    ray.Direction = (dtid.x == 0u) ? float3(0.0, 0.0, 1.0) : float3(0.0, 0.0, -1.0);
    ray.TMin = 0.001;
    ray.TMax = 100.0;

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(g_tlas, RAY_FLAG_NONE, 0xFFu, ray);
    q.Proceed();

    RayResult r;
    r.status = q.CommittedStatus();
    if (r.status == COMMITTED_TRIANGLE_HIT) {
        r.inst_id = q.CommittedInstanceID();
        r.prim_idx = q.CommittedPrimitiveIndex();
        r.ray_t = q.CommittedRayT();
        float2 bary = q.CommittedTriangleBarycentrics();
        r.bary_u = bary.x;
        r.bary_v = bary.y;
    } else {
        r.inst_id = 0u;
        r.prim_idx = 0u;
        r.ray_t = -1.0;
        r.bary_u = 0.0;
        r.bary_v = 0.0;
    }
    g_out[dtid.x] = r;
}
)HLSL";

    size_t rq_sz = 0;
    char err_buf[2048]{};
    HRESULT chr = D3D12XboxCompileHlslToDxil(rq_hlsl, std::strlen(rq_hlsl), L"main", L"cs_6_6",
                                             nullptr, 0, &rq_sz, err_buf, sizeof(err_buf));
    if (FAILED(chr)) {
        std::fprintf(stderr, "RayQuery DXC compile failed: 0x%08lX\n%s\n",
                     static_cast<unsigned long>(chr), err_buf);
        return 1;
    }
    std::vector<uint8_t> rq_dxil(rq_sz);
    CHECK_HR(D3D12XboxCompileHlslToDxil(rq_hlsl, std::strlen(rq_hlsl), L"main", L"cs_6_6",
                                        rq_dxil.data(), rq_dxil.size(), &rq_sz, err_buf, sizeof(err_buf)));

    D3D12_ROOT_PARAMETER1 rparams[2]{};
    rparams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rparams[0].Descriptor.ShaderRegister = 0;
    rparams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rparams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rparams[1].Descriptor.ShaderRegister = 0;
    rparams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rs_desc.Desc_1_1.NumParameters = 2;
    rs_desc.Desc_1_1.pParameters = rparams;
    ComPtr<ID3DBlob> rs_blob, rs_err;
    CHECK_HR(D3D12SerializeVersionedRootSignature(&rs_desc, rs_blob.GetAddressOf(), rs_err.GetAddressOf()));
    ComPtr<ID3D12RootSignature> rs;
    CHECK_HR(dev->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                      IID_PPV_ARGS(rs.GetAddressOf())));

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = rs.Get();
    pso_desc.CS.pShaderBytecode = rq_dxil.data();
    pso_desc.CS.BytecodeLength = rq_dxil.size();
    ComPtr<ID3D12PipelineState> pso;
    CHECK_HR(dev->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(pso.GetAddressOf())));

    ComPtr<ID3D12Resource> out_buf, out_rb;
    CHECK_HR(create_buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, sizeof(RayResult) * 2,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, out_buf.GetAddressOf()));
    CHECK_HR(create_buffer(dev.Get(), D3D12_HEAP_TYPE_READBACK, sizeof(RayResult) * 2,
                           D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, out_rb.GetAddressOf()));

    cl->SetComputeRootSignature(rs.Get());
    cl->SetPipelineState(pso.Get());
    cl->SetComputeRootShaderResourceView(0, tlas_buf->GetGPUVirtualAddress());
    cl->SetComputeRootUnorderedAccessView(1, out_buf->GetGPUVirtualAddress());
    cl->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER post[2] = {
        uav_barrier(out_buf.Get()),
        transition_barrier(out_buf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
    };
    cl->ResourceBarrier(2, post);
    cl->CopyBufferRegion(out_rb.Get(), 0, out_buf.Get(), 0, sizeof(RayResult) * 2);
    CHECK_HR(cl->Close());

    ID3D12CommandList* lists[1] = { cl.Get() };
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    CHECK_HR(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK_COND(ev != nullptr, "CreateEventW failed");
    CHECK_HR(queue->Signal(fence.Get(), 1));
    CHECK_HR(fence->SetEventOnCompletion(1, ev));
    CHECK_COND(WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0, "DXR GPU fence wait timed out");
    CloseHandle(ev);

    CHECK_HR(dev->GetDeviceRemovedReason());

    // 7. Read back RayQuery results and assert exact geometry hit + miss
    RayResult res[2]{};
    {
        void* p = nullptr;
        CHECK_HR(out_rb->Map(0, nullptr, &p));
        std::memcpy(res, p, sizeof(res));
        out_rb->Unmap(0, nullptr);
    }
    std::printf("[BVH-DXR] Ray0: status=%u inst=0x%X prim=%u t=%.4f bary=(%.4f, %.4f)\n",
                res[0].status, res[0].inst_id, res[0].prim_idx, res[0].ray_t, res[0].bary_u, res[0].bary_v);
    std::printf("[BVH-DXR] Ray1: status=%u t=%.4f\n", res[1].status, res[1].ray_t);

    CHECK_COND(res[0].status == 1u, "Ray 0 must hit cloned BLAS triangle (COMMITTED_TRIANGLE_HIT)");
    CHECK_COND(res[0].inst_id == 0x42u, "Ray 0 InstanceID must equal 0x42");
    CHECK_COND(res[0].prim_idx == 0u, "Ray 0 PrimitiveIndex must equal 0");
    CHECK_COND(std::fabs(res[0].ray_t - 1.0f) < 1e-4f, "Ray 0 RayT must equal 1.0");
    CHECK_COND(std::fabs(res[0].bary_u - 0.25f) < 1e-4f && std::fabs(res[0].bary_v - 0.50f) < 1e-4f,
               "Ray 0 barycentrics must equal (0.25, 0.50)");
    CHECK_COND(res[1].status == 0u, "Ray 1 must miss (COMMITTED_NOTHING)");

    // 8. Verify bridge telemetry and clean destruction
    WsxXboxBvhBridgeStats stats{};
    CHECK_HR(fn_stats(&stats));
    std::printf("[BVH-DXR] Bridge stats: inst=%llu prebuild=%llu build=%llu batched=%llu copy=%llu validate=%llu\n",
                static_cast<unsigned long long>(stats.InstancesCreated),
                static_cast<unsigned long long>(stats.PrebuildInfoCalls),
                static_cast<unsigned long long>(stats.BuildCalls),
                static_cast<unsigned long long>(stats.BatchedBuildCalls),
                static_cast<unsigned long long>(stats.CopyCalls),
                static_cast<unsigned long long>(stats.ValidateCalls));
    CHECK_COND(stats.InstancesCreated >= 1 && stats.PrebuildInfoCalls == 2 &&
               stats.BuildCalls == 1 && stats.BatchedBuildCalls == 1 && stats.CopyCalls == 1,
               "Bridge call counters mismatch");

    fn_dtor(bvh_storage);
    reinterpret_cast<IUnknown*>(xbox_abi_cl)->Release();

    // 9. Verify 0 D3D12 Debug Layer errors
    if (info_queue) {
        const UINT64 num_msgs = info_queue->GetNumStoredMessages();
        uint32_t err_count = 0;
        for (UINT64 i = 0; i < num_msgs; ++i) {
            SIZE_T len = 0;
            info_queue->GetMessage(i, nullptr, &len);
            if (!len) continue;
            std::vector<uint8_t> buf(len);
            auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(info_queue->GetMessage(i, msg, &len))) {
                if (msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
                    msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR) {
                    std::fprintf(stderr, "D3D12 DEBUG ERROR: %s\n", msg->pDescription);
                    ++err_count;
                }
            }
        }
        CHECK_COND(err_count == 0u, "D3D12 Debug Layer reported errors");
        std::printf("[BVH-DXR] D3D12 Debug Layer verified: 0 errors (%llu total messages)\n",
                    static_cast<unsigned long long>(num_msgs));
    }

    FreeLibrary(mod);
    std::printf("PASS: xbox_bvh_dxr\n");
    return 0;
}
