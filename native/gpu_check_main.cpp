// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// MizuBox Standalone Portable GPU Compatibility & 408-Ray DXR Differential BVH Probe
// (Zero game files required — pure Direct3D 12 SM 6.6 + DXR Tier 1.1, works on NVIDIA/AMD/Intel/WARP).
//
// Probes:
//   1. Adapter capabilities (VRAM, Resizable BAR / GPU Upload Heap, SM 6.6+, Dynamic Resources,
//      WaveLaneCountMin/Max, Mesh Shader Tier 1, DXR Tier 1.1, Int64 Atomics, Enhanced Barriers).
//   2. Native [WaveSize(64)] hardware support probe + MizuBox groupshared Wave64 emulation check.
//   3. Mesh Shader Tier 1 (ms_6_6) compilation & PSO validation.
//   4. 408-Ray CPU Moller-Trumbore vs Hardware DXR Tier 1.1 (RayQuery) Differential BVH Suite:
//        - 128 primary multi-instance occlusion & miss rays
//        -  64 instance mask filtering rays (0x01, 0x02, 0x04, 0x08, 0x05, 0x0A, 0x00)
//        -  64 oblique / near-edge / tMin-tMax range-clamped rays
//        -  56 backface-culled vs double-sided rays
//        -  48 post-PERFORM_UPDATE dynamic BLAS refit rays
//        -  48 CopyRaytracingAccelerationStructure(CLONE) BLAS instance rays
//   5. Optional shareable Markdown compatibility report (--report <file.md>).

#include "d3d12_xs_sm66_runtime.h"
#include "include/xbox/d3d12_xs.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross3(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 normalize3(Vec3 v) {
    const double l = std::sqrt(dot3(v, v));
    return (l > 1e-12) ? Vec3{v.x / l, v.y / l, v.z / l} : Vec3{0.0, 0.0, 1.0};
}

struct Float3 { float x, y, z; };

struct CpuTriangle {
    Vec3 v0, v1, v2;
    uint32_t prim_idx;
};

struct CpuInstance {
    uint32_t instance_id = 0;
    uint32_t mask = 0xFFu;
    float xform[3][4]{};
    std::vector<CpuTriangle> tris;
};

struct alignas(16) GpuRayInput {
    float ox, oy, oz, tmin;
    float dx, dy, dz, tmax;
    uint32_t mask;
    uint32_t flags; // D3D12 ray flags (0 or RAY_FLAG_CULL_BACK_FACING_TRIANGLES = 0x10)
    uint32_t pad0, pad1;
};
static_assert(sizeof(GpuRayInput) == 48u, "GpuRayInput size");

struct alignas(16) GpuRayOutput {
    uint32_t hit;
    uint32_t instance_id;
    uint32_t prim_idx;
    float t;
    float bary_u;
    float bary_v;
    uint32_t pad0, pad1;
};
static_assert(sizeof(GpuRayOutput) == 32u, "GpuRayOutput size");

Vec3 xform_point(const float m[3][4], Vec3 p) {
    return {
        m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
        m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
        m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]
    };
}

GpuRayOutput trace_cpu_reference(const std::vector<CpuInstance>& scene, const GpuRayInput& r) {
    GpuRayOutput best{};
    best.hit = 0u;
    best.instance_id = 0xFFFFFFFFu;
    best.prim_idx = 0xFFFFFFFFu;
    best.t = r.tmax;
    best.bary_u = 0.0f;
    best.bary_v = 0.0f;

    const Vec3 ro{r.ox, r.oy, r.oz};
    const Vec3 rd{r.dx, r.dy, r.dz};
    const bool cull_back = (r.flags & 0x10u) != 0u;
    double best_t = static_cast<double>(r.tmax);

    for (const auto& inst : scene) {
        if ((inst.mask & r.mask) == 0u) continue;
        for (const auto& tri : inst.tris) {
            const Vec3 v0 = xform_point(inst.xform, tri.v0);
            const Vec3 v1 = xform_point(inst.xform, tri.v1);
            const Vec3 v2 = xform_point(inst.xform, tri.v2);
            const Vec3 e1 = v1 - v0;
            const Vec3 e2 = v2 - v0;
            const Vec3 pvec = cross3(rd, e2);
            const double det = dot3(e1, pvec);
            // In DXR default winding (clockwise front-face): det > 0 is front-facing, det < 0 is back-facing
            if (cull_back) {
                if (det <= 1e-12) continue;
            } else {
                if (std::fabs(det) <= 1e-12) continue;
            }
            const double inv_det = 1.0 / det;
            const Vec3 tvec = ro - v0;
            const double u = dot3(tvec, pvec) * inv_det;
            if (u < 0.0 || u > 1.0) continue;
            const Vec3 qvec = cross3(tvec, e1);
            const double v = dot3(rd, qvec) * inv_det;
            if (v < 0.0 || (u + v) > 1.0) continue;
            const double t = dot3(e2, qvec) * inv_det;
            if (t >= static_cast<double>(r.tmin) && t < best_t) {
                best_t = t;
                best.hit = 1u;
                best.instance_id = inst.instance_id;
                best.prim_idx = tri.prim_idx;
                best.t = static_cast<float>(t);
                best.bary_u = static_cast<float>(u);
                best.bary_v = static_cast<float>(v);
            }
        }
    }
    return best;
}

void add_cw_quad(std::vector<Float3>& verts, std::vector<CpuTriangle>& cpu_tris,
                 Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
    auto emit = [&](Vec3 p0, Vec3 p1, Vec3 p2) {
        const uint32_t pid = static_cast<uint32_t>(cpu_tris.size());
        verts.push_back({static_cast<float>(p0.x), static_cast<float>(p0.y), static_cast<float>(p0.z)});
        verts.push_back({static_cast<float>(p1.x), static_cast<float>(p1.y), static_cast<float>(p1.z)});
        verts.push_back({static_cast<float>(p2.x), static_cast<float>(p2.y), static_cast<float>(p2.z)});
        cpu_tris.push_back({p0, p1, p2, pid});
    };
    emit(a, b, c);
    emit(a, c, d);
}

// Build BLAS A: unit cube [-1, 1]^3 (12 triangles) + floating tilted quad (2 tris) + pyramid roof (2 tris) = 16 tris
void build_mesh_a(std::vector<Float3>& verts, std::vector<CpuTriangle>& tris, Vec3 offset = {0, 0, 0}) {
    verts.clear();
    tris.clear();
    auto P = [&](double x, double y, double z) -> Vec3 {
        return {x + offset.x, y + offset.y, z + offset.z};
    };
    // Cube faces (outward-facing clockwise when viewed from outside)
    add_cw_quad(verts, tris, P(-1, -1, -1), P(-1, 1, -1), P(1, 1, -1), P(1, -1, -1)); // Front (-Z)
    add_cw_quad(verts, tris, P(1, -1, 1), P(1, 1, 1), P(-1, 1, 1), P(-1, -1, 1));     // Back (+Z)
    add_cw_quad(verts, tris, P(-1, -1, 1), P(-1, 1, 1), P(-1, 1, -1), P(-1, -1, -1)); // Left (-X)
    add_cw_quad(verts, tris, P(1, -1, -1), P(1, 1, -1), P(1, 1, 1), P(1, -1, 1));     // Right (+X)
    add_cw_quad(verts, tris, P(-1, 1, -1), P(-1, 1, 1), P(1, 1, 1), P(1, 1, -1));     // Top (+Y)
    add_cw_quad(verts, tris, P(-1, -1, 1), P(-1, -1, -1), P(1, -1, -1), P(1, -1, 1)); // Bottom (-Y)
    // Tilted diamond panel above cube
    add_cw_quad(verts, tris, P(-0.8, 1.3, -0.4), P(-0.6, 2.2, 0.3), P(0.7, 2.1, 0.5), P(0.8, 1.2, -0.3));
    // Two angled side fins
    auto emit_tri = [&](Vec3 a, Vec3 b, Vec3 c) {
        const uint32_t pid = static_cast<uint32_t>(tris.size());
        verts.push_back({static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(a.z)});
        verts.push_back({static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.z)});
        verts.push_back({static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z)});
        tris.push_back({a, b, c, pid});
    };
    emit_tri(P(1.1, -0.6, -0.5), P(1.8, 0.2, 0.0), P(1.1, -0.6, 0.6));
    emit_tri(P(-1.1, 0.4, -0.6), P(-1.9, -0.2, 0.1), P(-1.1, 0.4, 0.5));
}

// Build BLAS B: wide floor + sloped ramp + vertical wall = 8 triangles
void build_mesh_b(std::vector<Float3>& verts, std::vector<CpuTriangle>& tris) {
    verts.clear();
    tris.clear();
    add_cw_quad(verts, tris, {-6, 0, -6}, {-6, 0, 6}, {6, 0, 6}, {6, 0, -6});
    add_cw_quad(verts, tris, {-2, 0.1, 2.5}, {-2, 1.8, 5.2}, {2, 1.8, 5.2}, {2, 0.1, 2.5});
    add_cw_quad(verts, tris, {-4.5, 0.0, 4.5}, {-4.5, 2.5, 4.5}, {-2.2, 2.5, 4.5}, {-2.2, 0.0, 4.5});
    add_cw_quad(verts, tris, {2.3, 0.0, 3.8}, {2.3, 2.2, 3.8}, {4.6, 2.2, 3.8}, {4.6, 0.0, 3.8});
}

ComPtr<ID3D12Resource> create_buffer(
    ID3D12Device* dev, UINT64 size, D3D12_HEAP_TYPE heap,
    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES init_state) {
    D3D12_HEAP_PROPERTIES hp{heap};
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<UINT64>(size, 256ull);
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    ComPtr<ID3D12Resource> res;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, init_state, nullptr, IID_PPV_ARGS(&res)))) {
        return nullptr;
    }
    return res;
}

void upload_bytes(ID3D12Resource* upload_buf, const void* data, size_t n) {
    void* p = nullptr;
    D3D12_RANGE rr{0, 0};
    if (SUCCEEDED(upload_buf->Map(0, &rr, &p)) && p) {
        std::memcpy(p, data, n);
        upload_buf->Unmap(0, nullptr);
    }
}

void exec_and_wait(ID3D12CommandQueue* q, ID3D12GraphicsCommandList* cl,
                   ID3D12Fence* fence, HANDLE ev, UINT64& fval) {
    cl->Close();
    ID3D12CommandList* lists[] = {cl};
    q->ExecuteCommandLists(1, lists);
    const UINT64 target = ++fval;
    q->Signal(fence, target);
    if (fence->GetCompletedValue() < target) {
        fence->SetEventOnCompletion(target, ev);
        WaitForSingleObject(ev, 10000);
    }
}

struct ProbeReport {
    std::string adapter_name;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint64_t vram_mb = 0;
    uint64_t shared_mb = 0;
    bool gpu_upload_heap = false;
    bool enhanced_barriers = false;
    wsx_sm66::Sm66Caps sm66{};

    bool native_wave64_supported = false;
    uint32_t native_wave64_lane_count = 0;
    bool emulated_wave64_pass = false;
    bool mesh_shader_pso_pass = false;

    uint32_t dxr_total_rays = 0;
    uint32_t dxr_matched_rays = 0;
    uint32_t dxr_cat_matched[6]{};
    uint32_t dxr_cat_total[6]{128, 64, 64, 56, 48, 48};
    double dxr_max_dt = 0.0;
    double dxr_max_dbary = 0.0;
};

// Probe forced [WaveSize(64)] AND MizuBox groupshared Wave64 emulation on live GPU
void run_wave64_probes(ID3D12Device* dev, ID3D12CommandQueue* q,
                       ID3D12CommandAllocator* alloc, ID3D12GraphicsCommandList* cl,
                       ID3D12Fence* fence, HANDLE ev, UINT64& fval,
                       ProbeReport& rep) {
    // 1. Native [WaveSize(64)] probe
    static constexpr const char kNativeW64Hlsl[] = R"HLSL(
RWStructuredBuffer<uint> g_out : register(u0);
[WaveSize(64)]
[numthreads(64, 1, 1)]
void main(uint gi : SV_GroupIndex) {
    if (gi == 0u) g_out[0] = WaveGetLaneCount();
}
)HLSL";
    std::vector<uint8_t> dxil;
    char err[512]{};
    if (rep.sm66.WaveLaneCountMax >= 64u &&
        SUCCEEDED(wsx_sm66::compile_hlsl_to_dxil_vec(kNativeW64Hlsl, std::strlen(kNativeW64Hlsl), L"main", L"cs_6_6", dxil, err, sizeof(err)))) {
        D3D12_ROOT_PARAMETER1 rp{};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        rp.Descriptor.ShaderRegister = 0;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
        rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rsd.Desc_1_1.NumParameters = 1;
        rsd.Desc_1_1.pParameters = &rp;
        ComPtr<ID3DBlob> rsb, rse;
        ComPtr<ID3D12RootSignature> rs;
        ComPtr<ID3D12PipelineState> pso;
        if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&rsd, &rsb, &rse)) &&
            SUCCEEDED(dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&rs)))) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = rs.Get();
            pd.CS = {dxil.data(), dxil.size()};
            if (SUCCEEDED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) {
                auto uav = create_buffer(dev, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                auto rb = create_buffer(dev, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
                alloc->Reset();
                cl->Reset(alloc, pso.Get());
                cl->SetComputeRootSignature(rs.Get());
                cl->SetComputeRootUnorderedAccessView(0, uav->GetGPUVirtualAddress());
                cl->Dispatch(1, 1, 1);
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition = {uav.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
                cl->ResourceBarrier(1, &b);
                cl->CopyBufferRegion(rb.Get(), 0, uav.Get(), 0, 16);
                exec_and_wait(q, cl, fence, ev, fval);
                uint32_t* p = nullptr;
                D3D12_RANGE rr{0, 16};
                if (SUCCEEDED(rb->Map(0, &rr, reinterpret_cast<void**>(&p))) && p) {
                    rep.native_wave64_lane_count = p[0];
                    rep.native_wave64_supported = (p[0] == 64u);
                    rb->Unmap(0, nullptr);
                }
            }
        }
    }

    // 2. Emulated Wave64 (groupshared 2x32-lane) probe
    static constexpr const char kEmuW64Hlsl[] = R"HLSL(
RWStructuredBuffer<uint> g_out : register(u0);
groupshared uint _b[1][2];
groupshared uint _x[64];
uint2 emu_ballot(bool p, uint gi) {
    if (WaveGetLaneCount() >= 64u) { uint4 w = WaveActiveBallot(p); return uint2(w.x, w.y); }
    uint b32 = WaveActiveBallot(p).x;
    if (WaveIsFirstLane()) _b[0][(gi >> 5u) & 1u] = b32;
    GroupMemoryBarrierWithGroupSync();
    uint2 r = uint2(_b[0][0], _b[0][1]);
    GroupMemoryBarrierWithGroupSync();
    return r;
}
uint emu_readlane(uint v, uint src, uint gi) {
    if (WaveGetLaneCount() >= 64u) return WaveReadLaneAt(v, src & 63u);
    _x[gi] = v;
    GroupMemoryBarrierWithGroupSync();
    uint r = _x[src & 63u];
    GroupMemoryBarrierWithGroupSync();
    return r;
}
[numthreads(64, 1, 1)]
void main(uint gi : SV_GroupIndex) {
    uint2 m = emu_ballot(gi < 47u, gi);
    uint rl = emu_readlane(gi * 3u + 7u, 53u, gi);
    if (gi == 0u)  { g_out[0] = m.x; g_out[1] = m.y; g_out[2] = rl; }
    if (gi == 53u) { g_out[3] = m.x; g_out[4] = m.y; g_out[5] = rl; }
}
)HLSL";
    if (SUCCEEDED(wsx_sm66::compile_hlsl_to_dxil_vec(kEmuW64Hlsl, std::strlen(kEmuW64Hlsl), L"main", L"cs_6_6", dxil, err, sizeof(err)))) {
        D3D12_ROOT_PARAMETER1 rp{};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        rp.Descriptor.ShaderRegister = 0;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
        rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rsd.Desc_1_1.NumParameters = 1;
        rsd.Desc_1_1.pParameters = &rp;
        ComPtr<ID3DBlob> rsb, rse;
        ComPtr<ID3D12RootSignature> rs;
        ComPtr<ID3D12PipelineState> pso;
        if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&rsd, &rsb, &rse)) &&
            SUCCEEDED(dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&rs)))) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = rs.Get();
            pd.CS = {dxil.data(), dxil.size()};
            if (SUCCEEDED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) {
                auto uav = create_buffer(dev, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                auto rb = create_buffer(dev, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
                alloc->Reset();
                cl->Reset(alloc, pso.Get());
                cl->SetComputeRootSignature(rs.Get());
                cl->SetComputeRootUnorderedAccessView(0, uav->GetGPUVirtualAddress());
                cl->Dispatch(1, 1, 1);
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition = {uav.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
                cl->ResourceBarrier(1, &b);
                cl->CopyBufferRegion(rb.Get(), 0, uav.Get(), 0, 32);
                exec_and_wait(q, cl, fence, ev, fval);
                uint32_t* p = nullptr;
                D3D12_RANGE rr{0, 32};
                if (SUCCEEDED(rb->Map(0, &rr, reinterpret_cast<void**>(&p))) && p) {
                    rep.emulated_wave64_pass =
                        (p[0] == 0xFFFFFFFFu && p[1] == 0x00007FFFu && p[2] == 166u &&
                         p[3] == 0xFFFFFFFFu && p[4] == 0x00007FFFu && p[5] == 166u);
                    rb->Unmap(0, nullptr);
                }
            }
        }
    }
}

void run_mesh_shader_probe(ID3D12Device2* dev2, ProbeReport& rep) {
    if (!dev2 || rep.sm66.MeshShaderTier < 10u) return;
    static constexpr const char kMsHlsl[] = R"HLSL(
struct V { float4 p : SV_Position; };
[outputtopology("triangle")]
[numthreads(3, 1, 1)]
void ms_main(uint gi : SV_GroupIndex, out vertices V v[3], out indices uint3 t[1]) {
    SetMeshOutputCounts(3u, 1u);
    v[gi].p = float4(gi == 1u ? 0.5f : -0.5f, gi == 2u ? 0.5f : -0.5f, 0.0f, 1.0f);
    if (gi == 0u) t[0] = uint3(0u, 1u, 2u);
}
float4 ps_main(V i) : SV_Target { return float4(0.1f, 0.8f, 0.3f, 1.0f); }
)HLSL";
    std::vector<uint8_t> ms_dxil, ps_dxil;
    char err[512]{};
    if (FAILED(wsx_sm66::compile_hlsl_to_dxil_vec(kMsHlsl, std::strlen(kMsHlsl), L"ms_main", L"ms_6_6", ms_dxil, err, sizeof(err))) ||
        FAILED(wsx_sm66::compile_hlsl_to_dxil_vec(kMsHlsl, std::strlen(kMsHlsl), L"ps_main", L"ps_6_6", ps_dxil, err, sizeof(err)))) {
        return;
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
    rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    ComPtr<ID3DBlob> rsb, rse;
    ComPtr<ID3D12RootSignature> rs;
    if (FAILED(D3D12SerializeVersionedRootSignature(&rsd, &rsb, &rse)) ||
        FAILED(dev2->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&rs)))) {
        return;
    }
    struct alignas(8) MeshStream {
        struct alignas(8) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE; ID3D12RootSignature* v; } rs_so;
        struct alignas(8) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS; D3D12_SHADER_BYTECODE v; } ms_so;
        struct alignas(8) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS; D3D12_SHADER_BYTECODE v; } ps_so;
        struct alignas(8) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS; D3D12_RT_FORMAT_ARRAY v; } rtv_so;
    } stream{};
    stream.rs_so.v = rs.Get();
    stream.ms_so.v = {ms_dxil.data(), ms_dxil.size()};
    stream.ps_so.v = {ps_dxil.data(), ps_dxil.size()};
    stream.rtv_so.v.NumRenderTargets = 1;
    stream.rtv_so.v.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

    D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof(stream), &stream};
    ComPtr<ID3D12PipelineState> pso;
    rep.mesh_shader_pso_pass = SUCCEEDED(dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&pso))) && pso != nullptr;
}

// 408-Ray CPU Moller-Trumbore vs Hardware DXR Tier 1.1 Differential BVH Suite
void run_dxr_408_differential_suite(
    ID3D12Device5* dev5, ID3D12CommandQueue* q,
    ID3D12CommandAllocator* alloc, ID3D12GraphicsCommandList4* cl4,
    ID3D12Fence* fence, HANDLE ev, UINT64& fval,
    ProbeReport& rep) {
    if (!dev5 || !cl4 || rep.sm66.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) return;

    // Build geometry buffers
    std::vector<Float3> verts_a, verts_a_upd, verts_b;
    std::vector<CpuTriangle> tris_a, tris_a_upd, tris_b;
    build_mesh_a(verts_a, tris_a, {0.0, 0.0, 0.0});
    build_mesh_a(verts_a_upd, tris_a_upd, {0.5, 0.75, 0.0}); // dynamic refit target!
    build_mesh_b(verts_b, tris_b);

    auto vb_a = create_buffer(dev5, verts_a.size() * sizeof(Float3), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto vb_a_upd = create_buffer(dev5, verts_a_upd.size() * sizeof(Float3), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto vb_b = create_buffer(dev5, verts_b.size() * sizeof(Float3), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    upload_bytes(vb_a.Get(), verts_a.data(), verts_a.size() * sizeof(Float3));
    upload_bytes(vb_a_upd.Get(), verts_a_upd.data(), verts_a_upd.size() * sizeof(Float3));
    upload_bytes(vb_b.Get(), verts_b.data(), verts_b.size() * sizeof(Float3));

    auto make_geom_desc = [](ID3D12Resource* vb, UINT vcount) {
        D3D12_RAYTRACING_GEOMETRY_DESC g{};
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        g.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress();
        g.Triangles.VertexBuffer.StrideInBytes = sizeof(Float3);
        g.Triangles.VertexCount = vcount;
        g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        return g;
    };

    D3D12_RAYTRACING_GEOMETRY_DESC gd_a = make_geom_desc(vb_a.Get(), static_cast<UINT>(verts_a.size()));
    D3D12_RAYTRACING_GEOMETRY_DESC gd_b = make_geom_desc(vb_b.Get(), static_cast<UINT>(verts_b.size()));

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in_a{};
    in_a.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in_a.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
                 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    in_a.NumDescs = 1;
    in_a.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in_a.pGeometryDescs = &gd_a;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in_b = in_a;
    in_b.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    in_b.pGeometryDescs = &gd_b;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info_a{}, info_b{}, info_tlas{};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&in_a, &info_a);
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&in_b, &info_b);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in_tlas{};
    in_tlas.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in_tlas.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    in_tlas.NumDescs = 4;
    in_tlas.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&in_tlas, &info_tlas);

    const UINT64 scratch_sz = std::max({
        info_a.ScratchDataSizeInBytes, info_a.UpdateScratchDataSizeInBytes,
        info_b.ScratchDataSizeInBytes, info_tlas.ScratchDataSizeInBytes,
        info_tlas.UpdateScratchDataSizeInBytes}) + 4096ull;

    auto blas_a = create_buffer(dev5, info_a.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    auto blas_a_clone = create_buffer(dev5, info_a.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    auto blas_b = create_buffer(dev5, info_b.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    auto tlas = create_buffer(dev5, info_tlas.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    auto scratch = create_buffer(dev5, scratch_sz, D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

    // 4 Instances:
    //   Inst 0 (ID=10, Mask=0x01): BLAS A at identity
    //   Inst 1 (ID=20, Mask=0x02): BLAS A rotated 45 deg Y, scaled (1.2, 0.8, 1.0), translated (+4, +0.5, -1)
    //   Inst 2 (ID=30, Mask=0x04): BLAS B translated (0, -2, 0)
    //   Inst 3 (ID=40, Mask=0x08): CLONED BLAS A translated (-4, 0, +2)
    const float c45 = 0.70710678f;
    const float s45 = 0.70710678f;
    std::array<D3D12_RAYTRACING_INSTANCE_DESC, 4> insts{};
    auto set_mat = [](float dst[3][4],
                      float m00, float m01, float m02, float m03,
                      float m10, float m11, float m12, float m13,
                      float m20, float m21, float m22, float m23) {
        dst[0][0] = m00; dst[0][1] = m01; dst[0][2] = m02; dst[0][3] = m03;
        dst[1][0] = m10; dst[1][1] = m11; dst[1][2] = m12; dst[1][3] = m13;
        dst[2][0] = m20; dst[2][1] = m21; dst[2][2] = m22; dst[2][3] = m23;
    };

    set_mat(insts[0].Transform, 1, 0, 0, 0,   0, 1, 0, 0,   0, 0, 1, 0);
    insts[0].InstanceID = 10; insts[0].InstanceMask = 0x01;
    insts[0].AccelerationStructure = blas_a->GetGPUVirtualAddress();

    set_mat(insts[1].Transform, 1.2f * c45, 0, 1.0f * s45, 4.0f,
                                0, 0.8f, 0, 0.5f,
                                -1.2f * s45, 0, 1.0f * c45, -1.0f);
    insts[1].InstanceID = 20; insts[1].InstanceMask = 0x02;
    insts[1].AccelerationStructure = blas_a->GetGPUVirtualAddress();

    set_mat(insts[2].Transform, 1, 0, 0, 0,   0, 1, 0, -2.0f,   0, 0, 1, 0);
    insts[2].InstanceID = 30; insts[2].InstanceMask = 0x04;
    insts[2].AccelerationStructure = blas_b->GetGPUVirtualAddress();

    set_mat(insts[3].Transform, 1, 0, 0, -4.0f,   0, 1, 0, 0,   0, 0, 1, 2.0f);
    insts[3].InstanceID = 40; insts[3].InstanceMask = 0x08;
    insts[3].AccelerationStructure = blas_a_clone->GetGPUVirtualAddress();

    auto inst_buf = create_buffer(dev5, sizeof(insts), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    upload_bytes(inst_buf.Get(), insts.data(), sizeof(insts));
    in_tlas.InstanceDescs = inst_buf->GetGPUVirtualAddress();

    // Build BLAS A, BLAS B, CLONE BLAS A -> blas_a_clone, then build TLAS
    alloc->Reset();
    cl4->Reset(alloc, nullptr);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd_a{};
    bd_a.DestAccelerationStructureData = blas_a->GetGPUVirtualAddress();
    bd_a.Inputs = in_a;
    bd_a.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    cl4->BuildRaytracingAccelerationStructure(&bd_a, 0, nullptr);

    D3D12_RESOURCE_BARRIER uav_b{};
    uav_b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cl4->ResourceBarrier(1, &uav_b);

    cl4->CopyRaytracingAccelerationStructure(
        blas_a_clone->GetGPUVirtualAddress(),
        blas_a->GetGPUVirtualAddress(),
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd_b{};
    bd_b.DestAccelerationStructureData = blas_b->GetGPUVirtualAddress();
    bd_b.Inputs = in_b;
    bd_b.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    cl4->ResourceBarrier(1, &uav_b);
    cl4->BuildRaytracingAccelerationStructure(&bd_b, 0, nullptr);
    cl4->ResourceBarrier(1, &uav_b);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd_tlas{};
    bd_tlas.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
    bd_tlas.Inputs = in_tlas;
    bd_tlas.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    cl4->BuildRaytracingAccelerationStructure(&bd_tlas, 0, nullptr);
    cl4->ResourceBarrier(1, &uav_b);
    exec_and_wait(q, cl4, fence, ev, fval);

    // Compile SM 6.6 DXR Tier 1.1 RayQuery CS
    static constexpr const char kDxrHlsl[] = R"HLSL(
cbuffer Params : register(b0) { uint g_rayCount; };
struct RayIn {
    float3 o; float tmin;
    float3 d; float tmax;
    uint mask; uint flags; uint p0; uint p1;
};
struct RayOut {
    uint hit; uint inst_id; uint prim_idx; float t;
    float bary_u; float bary_v; uint p0; uint p1;
};
RaytracingAccelerationStructure g_tlas : register(t0);
StructuredBuffer<RayIn> g_rays : register(t1);
RWStructuredBuffer<RayOut> g_out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint idx = dtid.x;
    if (idx >= g_rayCount) return;
    RayIn r = g_rays[idx];
    RayDesc rd;
    rd.Origin = r.o;
    rd.TMin = r.tmin;
    rd.Direction = r.d;
    rd.TMax = r.tmax;

    RayOut o;
    o.hit = 0u; o.inst_id = 0xFFFFFFFFu; o.prim_idx = 0xFFFFFFFFu;
    o.t = r.tmax; o.bary_u = 0.0f; o.bary_v = 0.0f; o.p0 = 0u; o.p1 = 0u;

    if ((r.flags & 0x10u) != 0u) {
        RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
        q.TraceRayInline(g_tlas, 0u, r.mask, rd);
        q.Proceed();
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            o.hit = 1u;
            o.inst_id = q.CommittedInstanceID();
            o.prim_idx = q.CommittedPrimitiveIndex();
            o.t = q.CommittedRayT();
            float2 b = q.CommittedTriangleBarycentrics();
            o.bary_u = b.x; o.bary_v = b.y;
        }
    } else {
        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(g_tlas, 0u, r.mask, rd);
        q.Proceed();
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            o.hit = 1u;
            o.inst_id = q.CommittedInstanceID();
            o.prim_idx = q.CommittedPrimitiveIndex();
            o.t = q.CommittedRayT();
            float2 b = q.CommittedTriangleBarycentrics();
            o.bary_u = b.x; o.bary_v = b.y;
        }
    }
    g_out[idx] = o;
}
)HLSL";
    std::vector<uint8_t> dxil;
    char err[1024]{};
    if (FAILED(wsx_sm66::compile_hlsl_to_dxil_vec(kDxrHlsl, std::strlen(kDxrHlsl), L"main", L"cs_6_6", dxil, err, sizeof(err)))) {
        std::fprintf(stderr, "[ERROR] DXR RayQuery HLSL compile failed: %s\n", err);
        return;
    }

    D3D12_ROOT_PARAMETER1 rps[4]{};
    rps[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rps[0].Constants = {0, 0, 1};
    rps[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rps[1].Descriptor = {0, 0};
    rps[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rps[2].Descriptor = {1, 0};
    rps[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rps[3].Descriptor = {0, 0};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
    rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rsd.Desc_1_1.NumParameters = 4;
    rsd.Desc_1_1.pParameters = rps;
    ComPtr<ID3DBlob> rsb, rse;
    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(D3D12SerializeVersionedRootSignature(&rsd, &rsb, &rse)) ||
        FAILED(dev5->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&rs)))) {
        return;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rs.Get();
    pd.CS = {dxil.data(), dxil.size()};
    if (FAILED(dev5->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) return;

    // Helper to build CPU reference scene snapshot
    auto make_cpu_scene = [&](bool updated_a) {
        std::vector<CpuInstance> sc(4);
        for (int i = 0; i < 4; ++i) {
            sc[i].instance_id = insts[i].InstanceID;
            sc[i].mask = insts[i].InstanceMask;
            std::memcpy(sc[i].xform, insts[i].Transform, sizeof(sc[i].xform));
        }
        sc[0].tris = updated_a ? tris_a_upd : tris_a;
        sc[1].tris = updated_a ? tris_a_upd : tris_a;
        sc[2].tris = tris_b;
        sc[3].tris = tris_a; // CLONED BLAS A was cloned BEFORE update — remains original!
        return sc;
    };

    auto make_ray = [](Vec3 o, Vec3 dir, float tmin, float tmax, uint32_t mask, uint32_t flags) -> GpuRayInput {
        const Vec3 nd = normalize3(dir);
        return GpuRayInput{
            static_cast<float>(o.x), static_cast<float>(o.y), static_cast<float>(o.z), tmin,
            static_cast<float>(nd.x), static_cast<float>(nd.y), static_cast<float>(nd.z), tmax,
            mask, flags, 0u, 0u
        };
    };

    // Build Batch 1 (360 rays: Categories 0, 1, 2, 3, 5 — BEFORE dynamic BLAS update)
    std::vector<GpuRayInput> batch1;
    std::vector<int> batch1_cat;

    // Category 0 (128 rays): 16x8 pinhole grid covering all 4 instances + background misses
    for (int iy = 0; iy < 8; ++iy) {
        for (int ix = 0; ix < 16; ++ix) {
            const double u = -6.23 + (12.46 * (ix + 0.37)) / 16.0;
            const double v = -2.41 + (5.13 * (iy + 0.41)) / 8.0;
            batch1.push_back(make_ray({0.13, 0.71, -9.5}, {u - 0.13, v - 0.71, 9.5}, 0.001f, 50.0f, 0xFFu, 0u));
            batch1_cat.push_back(0);
        }
    }
    // Category 1 (64 rays): Instance mask filtering across all 7 mask combos
    const uint32_t masks[8] = {0x01u, 0x02u, 0x04u, 0x08u, 0x05u, 0x0Au, 0x03u, 0x00u};
    for (int i = 0; i < 64; ++i) {
        const double x = -4.8 + (9.6 * ((i % 8) + 0.31)) / 8.0;
        const double y = -1.6 + (3.4 * ((i / 8) + 0.27)) / 8.0;
        batch1.push_back(make_ray({x, y, -8.0}, {0.03, -0.07, 1.0}, 0.001f, 40.0f, masks[i % 8], 0u));
        batch1_cat.push_back(1);
    }
    // Category 2 (64 rays): Oblique angles + tMin/tMax range clamping
    for (int i = 0; i < 64; ++i) {
        const double ang = 0.17 + i * (6.283185307 / 64.0);
        const Vec3 ro{std::cos(ang) * 7.5, -0.8 + (i % 7) * 0.47, std::sin(ang) * 7.5};
        const Vec3 tgt{(i % 3 - 1) * 2.13, -0.43 + (i % 5) * 0.33, (i % 2) * 0.77};
        const float tmin = (i % 4 == 0) ? 5.5f : 0.01f;
        const float tmax = (i % 4 == 1) ? 4.2f : 30.0f;
        batch1.push_back(make_ray(ro, tgt - ro, tmin, tmax, 0xFFu, 0u));
        batch1_cat.push_back(2);
    }
    // Category 3 (56 rays): Backface culling (rays from INSIDE cube & behind floor/walls!)
    for (int i = 0; i < 56; ++i) {
        const double ang = 0.29 + i * (6.283185307 / 28.0);
        const uint32_t flg = (i < 28) ? 0x10u : 0u; // 28 culled + 28 double-sided from inside cube!
        const Vec3 dir{std::cos(ang), -0.61 + (i % 7) * 0.21, std::sin(ang)};
        batch1.push_back(make_ray({0.07, 0.11, -0.05}, dir, 0.001f, 25.0f, 0xFFu, flg));
        batch1_cat.push_back(3);
    }
    // Category 5 (48 rays): Focused on CLONED BLAS Instance 3 at (-4, 0, +2)
    for (int iy = 0; iy < 6; ++iy) {
        for (int ix = 0; ix < 8; ++ix) {
            const double x = -5.41 + (2.82 * (ix + 0.39)) / 8.0;
            const double y = -1.23 + (3.61 * (iy + 0.43)) / 6.0;
            batch1.push_back(make_ray({-4.0, 0.2, -5.0}, {x + 4.0, y - 0.2, 7.0}, 0.001f, 30.0f, 0x08u, 0u));
            batch1_cat.push_back(5);
        }
    }

    auto dispatch_and_compare = [&](const std::vector<GpuRayInput>& rays,
                                    const std::vector<int>& cats,
                                    const std::vector<CpuInstance>& cpu_scene) {
        const size_t n = rays.size();
        auto ray_buf = create_buffer(dev5, n * sizeof(GpuRayInput), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto out_buf = create_buffer(dev5, n * sizeof(GpuRayOutput), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        auto rb_buf  = create_buffer(dev5, n * sizeof(GpuRayOutput), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        upload_bytes(ray_buf.Get(), rays.data(), n * sizeof(GpuRayInput));

        alloc->Reset();
        cl4->Reset(alloc, pso.Get());
        cl4->SetComputeRootSignature(rs.Get());
        const UINT n_u32 = static_cast<UINT>(n);
        cl4->SetComputeRoot32BitConstant(0, n_u32, 0);
        cl4->SetComputeRootShaderResourceView(1, tlas->GetGPUVirtualAddress());
        cl4->SetComputeRootShaderResourceView(2, ray_buf->GetGPUVirtualAddress());
        cl4->SetComputeRootUnorderedAccessView(3, out_buf->GetGPUVirtualAddress());
        cl4->Dispatch(static_cast<UINT>((n + 63u) / 64u), 1, 1);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {out_buf.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cl4->ResourceBarrier(1, &b);
        cl4->CopyBufferRegion(rb_buf.Get(), 0, out_buf.Get(), 0, n * sizeof(GpuRayOutput));
        exec_and_wait(q, cl4, fence, ev, fval);

        GpuRayOutput* gpu = nullptr;
        D3D12_RANGE rr{0, n * sizeof(GpuRayOutput)};
        if (SUCCEEDED(rb_buf->Map(0, &rr, reinterpret_cast<void**>(&gpu))) && gpu) {
            for (size_t i = 0; i < n; ++i) {
                const GpuRayOutput ref = trace_cpu_reference(cpu_scene, rays[i]);
                ++rep.dxr_total_rays;
                bool ok = (gpu[i].hit == ref.hit);
                if (ok && ref.hit) {
                    const double dt = std::fabs(static_cast<double>(gpu[i].t) - static_cast<double>(ref.t));
                    const double du = std::fabs(static_cast<double>(gpu[i].bary_u) - static_cast<double>(ref.bary_u));
                    const double dv = std::fabs(static_cast<double>(gpu[i].bary_v) - static_cast<double>(ref.bary_v));
                    rep.dxr_max_dt = std::max(rep.dxr_max_dt, dt);
                    rep.dxr_max_dbary = std::max(rep.dxr_max_dbary, std::max(du, dv));
                    ok = (gpu[i].instance_id == ref.instance_id &&
                          gpu[i].prim_idx == ref.prim_idx &&
                          dt < 1e-3 && du < 1e-3 && dv < 1e-3);
                }
                if (ok) {
                    ++rep.dxr_matched_rays;
                    ++rep.dxr_cat_matched[cats[i]];
                }
            }
            rb_buf->Unmap(0, nullptr);
        }
    };

    dispatch_and_compare(batch1, batch1_cat, make_cpu_scene(false));

    // Perform in-place PERFORM_UPDATE on BLAS A (vertices shifted by +0.5, +0.75, 0) + TLAS update!
    D3D12_RAYTRACING_GEOMETRY_DESC gd_a_upd = make_geom_desc(vb_a_upd.Get(), static_cast<UINT>(verts_a_upd.size()));
    in_a.pGeometryDescs = &gd_a_upd;
    in_a.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
                 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE |
                 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    in_tlas.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE |
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    alloc->Reset();
    cl4->Reset(alloc, nullptr);
    bd_a.SourceAccelerationStructureData = blas_a->GetGPUVirtualAddress();
    bd_a.DestAccelerationStructureData = blas_a->GetGPUVirtualAddress();
    bd_a.Inputs = in_a;
    cl4->BuildRaytracingAccelerationStructure(&bd_a, 0, nullptr);
    cl4->ResourceBarrier(1, &uav_b);
    bd_tlas.SourceAccelerationStructureData = tlas->GetGPUVirtualAddress();
    bd_tlas.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
    bd_tlas.Inputs = in_tlas;
    cl4->BuildRaytracingAccelerationStructure(&bd_tlas, 0, nullptr);
    cl4->ResourceBarrier(1, &uav_b);
    exec_and_wait(q, cl4, fence, ev, fval);

    // Category 4 (48 rays): Post-PERFORM_UPDATE refitted scene (checks updated BLAS A + untouched cloned BLAS A!)
    std::vector<GpuRayInput> batch2;
    std::vector<int> batch2_cat;
    for (int iy = 0; iy < 6; ++iy) {
        for (int ix = 0; ix < 8; ++ix) {
            const double x = -1.6 + (4.2 * (ix + 0.37)) / 8.0;
            const double y = -0.9 + (4.1 * (iy + 0.41)) / 6.0;
            batch2.push_back(make_ray({0.41, 0.67, -7.5}, {x - 0.41, y - 0.67, 7.5}, 0.001f, 35.0f, 0xFFu, 0u));
            batch2_cat.push_back(4);
        }
    }
    dispatch_and_compare(batch2, batch2_cat, make_cpu_scene(true));
}

std::string sanitize_filename(std::string s) {
    for (char& c : s) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) c = '_';
    }
    return s;
}

std::string format_markdown_report(const ProbeReport& r) {
    std::ostringstream os;
    os << "# MizuBox (by Mizushi — Enhanced Fork of AnyXboxSeries) GPU Compatibility Report\n\n";
    os << "**Adapter:** `" << r.adapter_name << "` (`0x" << std::hex << r.vendor_id
       << ":0x" << r.device_id << std::dec << "`)\n\n";
    os << "## 1. Direct3D 12 Hardware Capabilities\n\n";
    os << "| Capability | Value | Status |\n";
    os << "| :--- | :--- | :--- |\n";
    os << "| Dedicated VRAM / Shared RAM | " << r.vram_mb << " MB / " << r.shared_mb << " MB | " << (r.vram_mb >= 4096 ? "PASS" : "WARN") << " |\n";
    os << "| Resizable BAR (`GPU_UPLOAD` Heap) | " << (r.gpu_upload_heap ? "Supported" : "Fallback L0") << " | " << (r.gpu_upload_heap ? "PASS" : "INFO") << " |\n";
    os << "| Highest Shader Model | `0x" << std::hex << r.sm66.HighestShaderModel << std::dec << "` | " << (r.sm66.HighestShaderModel >= 0x66u ? "PASS" : "FAIL") << " |\n";
    os << "| SM 6.6 Dynamic Resources (Tier 3) | " << (r.sm66.SupportsDynamicResources ? "Yes" : "No") << " | " << (r.sm66.SupportsDynamicResources ? "PASS" : "FAIL") << " |\n";
    os << "| Hardware Wave Width (Min..Max) | " << r.sm66.WaveLaneCountMin << ".." << r.sm66.WaveLaneCountMax << " lanes | INFO |\n";
    os << "| Native `[WaveSize(64)]` CS | " << (r.native_wave64_supported ? "Native 64-lane" : "32-wide (auto-emulated)") << " | " << (r.native_wave64_supported ? "NATIVE" : "EMULATED") << " |\n";
    os << "| Emulated Wave64 (`groupshared`) | " << (r.emulated_wave64_pass ? "Bit-Exact 64-lane" : "FAIL") << " | " << (r.emulated_wave64_pass ? "PASS" : "FAIL") << " |\n";
    os << "| Mesh Shaders Tier 1 (`ms_6_6`) | Tier " << (r.sm66.MeshShaderTier >= 10 ? 1 : 0) << " / PSO " << (r.mesh_shader_pso_pass ? "OK" : "FAIL") << " | " << (r.mesh_shader_pso_pass ? "PASS" : "FAIL") << " |\n";
    os << "| DXR Raytracing Tier | " << (r.sm66.RaytracingTier >= 11 ? "Tier 1.1" : (r.sm66.RaytracingTier >= 10 ? "Tier 1.0" : "None")) << " | " << (r.sm66.RaytracingTier >= 11 ? "PASS" : "FAIL") << " |\n";
    os << "| Int64 Typed Atomics | " << (r.sm66.SupportsInt64Atomics ? "Yes" : "No") << " | " << (r.sm66.SupportsInt64Atomics ? "PASS" : "INFO") << " |\n";
    os << "| Enhanced Barriers (Options12) | " << (r.enhanced_barriers ? "Yes" : "No") << " | INFO |\n\n";

    os << "## 2. 408-Ray CPU-vs-DXR Tier 1.1 Differential BVH Verification\n\n";
    os << "**Total Score:** `" << r.dxr_matched_rays << " / " << r.dxr_total_rays << "`"
       << " | **Max `|Δt|`:** `" << r.dxr_max_dt << "`"
       << " | **Max `|Δbary|`:** `" << r.dxr_max_dbary << "`\n\n";
    static constexpr const char* kCatNames[6] = {
        "Primary Multi-Instance Grid (Occlusion & Misses)",
        "Instance Mask Filtering (0x00..0x0A)",
        "Oblique Angles & tMin/tMax Range Clamping",
        "Backface Culling vs Double-Sided Rays",
        "Post-PERFORM_UPDATE Dynamic BLAS/TLAS Refit",
        "Cloned BLAS Precision (CopyRaytracingAS::CLONE)"
    };
    os << "| Category | Matched | Status |\n";
    os << "| :--- | :--- | :--- |\n";
    for (int i = 0; i < 6; ++i) {
        os << "| " << kCatNames[i] << " | " << r.dxr_cat_matched[i] << " / " << r.dxr_cat_total[i]
           << " | " << (r.dxr_cat_matched[i] == r.dxr_cat_total[i] ? "PASS" : "FAIL") << " |\n";
    }
    return os.str();
}

} // namespace

int main(int argc, char** argv) {
    bool use_warp = false;
    std::string report_path;
    bool auto_report = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--warp") == 0) {
            use_warp = true;
        } else if (std::strcmp(argv[i], "--report") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                report_path = argv[++i];
            } else {
                auto_report = true;
            }
        }
    }

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        std::fprintf(stderr, "[FAIL] CreateDXGIFactory2 failed\n");
        return 1;
    }

    ComPtr<IDXGIAdapter1> adapter;
    if (use_warp) {
        factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    } else {
        for (UINT i = 0; SUCCEEDED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))); ++i) {
            DXGI_ADAPTER_DESC1 ad{};
            adapter->GetDesc1(&ad);
            if ((ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) break;
            adapter.Reset();
        }
    }
    if (!adapter) {
        std::fprintf(stderr, "[FAIL] No suitable DXGI adapter found\n");
        return 1;
    }

    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    ProbeReport rep{};
    char name_utf8[256]{};
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name_utf8, sizeof(name_utf8), nullptr, nullptr);
    rep.adapter_name = name_utf8;
    rep.vendor_id = ad.VendorId;
    rep.device_id = ad.DeviceId;
    rep.vram_mb = ad.DedicatedVideoMemory / (1024ull * 1024ull);
    rep.shared_mb = ad.SharedSystemMemory / (1024ull * 1024ull);

    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) {
        std::fprintf(stderr, "[FAIL] D3D12CreateDevice failed on %s\n", rep.adapter_name.c_str());
        return 1;
    }

    wsx_sm66::check_sm66_support(dev.Get(), &rep.sm66);
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &o16, sizeof(o16)))) {
        rep.gpu_upload_heap = (o16.GPUUploadHeapSupported != FALSE);
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof(o12)))) {
        rep.enhanced_barriers = (o12.EnhancedBarriersSupported != FALSE);
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl));
    cl->Close();
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fval = 0;

    run_wave64_probes(dev.Get(), queue.Get(), alloc.Get(), cl.Get(), fence.Get(), ev, fval, rep);

    ComPtr<ID3D12Device2> dev2;
    dev.As(&dev2);
    run_mesh_shader_probe(dev2.Get(), rep);

    ComPtr<ID3D12Device5> dev5;
    ComPtr<ID3D12GraphicsCommandList4> cl4;
    if (SUCCEEDED(dev.As(&dev5)) && SUCCEEDED(cl.As(&cl4))) {
        run_dxr_408_differential_suite(dev5.Get(), queue.Get(), alloc.Get(), cl4.Get(), fence.Get(), ev, fval, rep);
    }

    CloseHandle(ev);

    const std::string md = format_markdown_report(rep);
    std::printf("%s\n", md.c_str());

    if (auto_report) {
        report_path = "gpu-report-" + sanitize_filename(rep.adapter_name) + ".md";
    }
    if (!report_path.empty()) {
        std::ofstream f(report_path, std::ios::binary);
        if (f) {
            f.write(md.data(), static_cast<std::streamsize>(md.size()));
            std::printf("[INFO] Wrote compatibility report to %s\n", report_path.c_str());
        }
    }

    const bool core_ok =
        rep.sm66.SupportsDynamicResources != 0u &&
        rep.emulated_wave64_pass &&
        (rep.sm66.RaytracingTier < D3D12_RAYTRACING_TIER_1_1 ||
         (rep.dxr_total_rays == 408u && rep.dxr_matched_rays == 408u));
    return core_ok ? 0 : 1;
}
