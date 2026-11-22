// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Pure Direct3D 12 (SM 6.6) Xbox RDNA2/FSSM Shader Runtime Contract Test:
// End-to-end real-GPU execution with D3D12 Debug Layer enabled (0 errors required):
// 1. Queries D3D12XboxCheckSm66Support (SM >= 6.6, Dynamic Resources, Mesh Shaders, DXR Tier 1.1+).
// 2. Creates the 40-DWORD Root Constants + Direct Heap Indexing Root Signature
//    via D3D12XboxCreateSm66RootSignature.
// 3. Compiles SM 6.6 HLSL (built on D3D12XboxGetSm66HlslPreamble) to signed DXIL at runtime
//    via D3D12XboxCompileHlslToDxil (dynamic Windows SDK dxcompiler.dll + dxil.dll).
// 4. Registers a translator callback via D3D12XboxRegisterShaderTranslator that intercepts a
//    synthetic stripped NOBC/FSSM Xbox shader in CreateComputePipelineState and returns the
//    compiled SM 6.6 DXIL.
// 5. Executes on real GPU (RTX 5060 / desktop D3D12) verifying:
//    - 64-bit Xbox GPU VA emulation across multiple RWByteAddressBuffer slabs (Slab 0 -> Slab 1)
//    - s_getpc_b64 FSSM inline constant table reads via wsx_code_load32()
//    - FNV-1a bindless T# (Texture2D) + S# (Sampler) lookup & live SampleLevel()
//    - Cross-slab 32-bit atomic accumulation via wsx_va_atomic_add32()
//    - 0 D3D12 Debug Layer errors / corruptions.

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

uint32_t fnv1a_8dw(const uint32_t key[8]) noexcept {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 8; ++i) {
        const uint32_t w = key[i];
        h = (h ^ (w & 0xFFu)) * 16777619u;
        h = (h ^ ((w >> 8) & 0xFFu)) * 16777619u;
        h = (h ^ ((w >> 16) & 0xFFu)) * 16777619u;
        h = (h ^ ((w >> 24) & 0xFFu)) * 16777619u;
    }
    return h;
}

void insert_desc_entry(
    std::vector<D3D12XBOX_SM66_DESC_HASH_ENTRY>& tbl,
    const uint32_t key[8],
    uint32_t heap_idx,
    uint32_t flags) {
    const uint32_t mask = static_cast<uint32_t>(tbl.size() - 1u);
    const uint32_t slot = fnv1a_8dw(key) & mask;
    for (uint32_t probe = 0; probe < 8u; ++probe) {
        auto& e = tbl[(slot + probe) & mask];
        if (e.Flags == 0u) {
            std::memcpy(e.Key, key, sizeof(e.Key));
            e.HeapIndex = heap_idx;
            e.Flags = flags;
            return;
        }
    }
}

void append_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFFu));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFFu));
}

void append_u32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFFu));
}

void append_bytes(std::vector<uint8_t>& v, const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    v.insert(v.end(), b, b + n);
}

std::vector<uint8_t> make_zlib_stored(const std::vector<uint8_t>& plain) {
    std::vector<uint8_t> z;
    z.push_back(0x78u);
    z.push_back(0x01u);
    z.push_back(0x01u);
    const uint16_t len = static_cast<uint16_t>(plain.size());
    const uint16_t nlen = static_cast<uint16_t>(~len);
    append_u16(z, len);
    append_u16(z, nlen);
    append_bytes(z, plain.data(), plain.size());
    uint32_t s1 = 1u, s2 = 0u;
    for (uint8_t b : plain) {
        s1 = (s1 + b) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    const uint32_t adler = (s2 << 16) | s1;
    z.push_back(static_cast<uint8_t>((adler >> 24) & 0xFFu));
    z.push_back(static_cast<uint8_t>((adler >> 16) & 0xFFu));
    z.push_back(static_cast<uint8_t>((adler >> 8) & 0xFFu));
    z.push_back(static_cast<uint8_t>(adler & 0xFFu));
    return z;
}

struct ChunkSpec {
    char fourcc[4];
    std::vector<uint8_t> payload;
};

std::vector<uint8_t> build_dxbc(const uint8_t digest[16], const std::vector<ChunkSpec>& chunks) {
    std::vector<uint8_t> out;
    append_bytes(out, "DXBC", 4);
    append_bytes(out, digest, 16);
    append_u32(out, 1u);
    append_u32(out, 0u);
    append_u32(out, static_cast<uint32_t>(chunks.size()));
    const size_t off_table = out.size();
    for (size_t i = 0; i < chunks.size(); ++i) append_u32(out, 0u);
    for (size_t i = 0; i < chunks.size(); ++i) {
        const uint32_t chunk_off = static_cast<uint32_t>(out.size());
        std::memcpy(out.data() + off_table + i * 4, &chunk_off, 4);
        append_bytes(out, chunks[i].fourcc, 4);
        append_u32(out, static_cast<uint32_t>(chunks[i].payload.size()));
        append_bytes(out, chunks[i].payload.data(), chunks[i].payload.size());
    }
    const uint32_t total = static_cast<uint32_t>(out.size());
    std::memcpy(out.data() + 24, &total, 4);
    return out;
}

// Synthetic stripped NOBC + XDAT(XBCH->FSSM) compute shader container matching shaders.rpf layout
std::vector<uint8_t> make_synthetic_fssm_cs() {
    std::vector<uint8_t> fssm(0x140, 0x00u);
    std::memcpy(fssm.data(), "FSSM", 4);
    const uint32_t raw_c0 = 0xD8u; // code_offset = 0xD8 + 0x28 = 0x100
    std::memcpy(fssm.data() + 0xC0, &raw_c0, 4);
    const uint32_t s_nop = 0xBF800000u;
    const uint32_t s_endpgm = 0xBF810000u;
    std::memcpy(fssm.data() + 0x100, &s_nop, 4);
    std::memcpy(fssm.data() + 0x104, &s_endpgm, 4);
    // Inline constant dword at offset 0x108 (for s_getpc_b64 test)
    const uint32_t inline_const = 0x00001337u;
    std::memcpy(fssm.data() + 0x108, &inline_const, 4);

    const std::vector<uint8_t> z = make_zlib_stored(fssm);
    std::vector<uint8_t> xbch;
    append_bytes(xbch, "XBCH", 4);
    append_u32(xbch, 1u);
    append_u32(xbch, static_cast<uint32_t>(z.size()));
    append_u32(xbch, 0u);
    append_u32(xbch, static_cast<uint32_t>(fssm.size()));
    append_u32(xbch, 0u);
    append_bytes(xbch, z.data(), z.size());

    std::vector<uint8_t> nobc_ent(28, 0u);
    std::memcpy(nobc_ent.data() + 8, "NOBC", 4);

    const uint8_t xdat_hash[16] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x66
    };
    std::vector<uint8_t> dxil_chunk;
    append_u32(dxil_chunk, 0x00050066u); // Compute Shader (5), SM 6.6
    append_u32(dxil_chunk, 0u);
    append_bytes(dxil_chunk, "DXIL", 4);
    append_u32(dxil_chunk, 0x106u);
    append_u32(dxil_chunk, 0u);
    append_u32(dxil_chunk, 0u);
    // XDAT starts at offset 24 inside the DXIL chunk
    const uint32_t xdat_header_end = 24u + 12u + 3u * 12u;
    const uint32_t off_hash = xdat_header_end;
    const uint32_t off_xbch = off_hash + 16u;
    const uint32_t off_nobc = off_xbch + static_cast<uint32_t>(xbch.size());
    append_bytes(dxil_chunk, "XDAT", 4);
    append_u32(dxil_chunk, 3u);
    append_u32(dxil_chunk, 12u + 3u * 12u);
    append_u32(dxil_chunk, 0x12u); append_u32(dxil_chunk, off_hash); append_u32(dxil_chunk, 16u);
    append_u32(dxil_chunk, 0x0du); append_u32(dxil_chunk, off_xbch); append_u32(dxil_chunk, static_cast<uint32_t>(xbch.size()));
    append_u32(dxil_chunk, 0x07u); append_u32(dxil_chunk, off_nobc); append_u32(dxil_chunk, static_cast<uint32_t>(nobc_ent.size()));
    append_bytes(dxil_chunk, xdat_hash, 16u);
    append_bytes(dxil_chunk, xbch.data(), xbch.size());
    append_bytes(dxil_chunk, nobc_ent.data(), nobc_ent.size());

    const uint8_t digest[16] = {
        0x66, 0x53, 0x4D, 0x36, 0x12, 0x34, 0x56, 0x78,
        0x9A, 0xBC, 0xDE, 0xF0, 0xAA, 0xBB, 0xCC, 0xDD
    };
    return build_dxbc(digest, {
        { {'D','X','I','L'}, dxil_chunk },
    });
}

struct TranslatorCtx {
    const std::vector<uint8_t>* compiled_sm66_dxil = nullptr;
    uint32_t invoke_count = 0;
    size_t received_fssm_size = 0;
    uint32_t received_fssm_inline_const = 0;
};

HRESULT WINAPI sm66_test_translator(
    const D3D12XBOX_SHADER_INFO* pInfo,
    const void* /*pOriginalDxbc*/,
    size_t /*originalDxbcSize*/,
    const void* pUnpackedFssm,
    size_t unpackedFssmSize,
    D3D12XBOX_SHADER_TARGET_FORMAT* pOutTargetFormat,
    void* pOutBuffer,
    size_t outBufferCapacity,
    size_t* pOutBufferSize,
    void* pUserContext) {
    auto* ctx = static_cast<TranslatorCtx*>(pUserContext);
    if (!ctx || !ctx->compiled_sm66_dxil || !pInfo || !pOutTargetFormat || !pOutBufferSize) {
        return E_INVALIDARG;
    }
    if (pInfo->Kind != D3D12XBOX_SHADER_KIND_XBOX_FSSM_RDNA2) {
        return E_NOTIMPL;
    }
    ctx->received_fssm_size = unpackedFssmSize;
    if (pUnpackedFssm && unpackedFssmSize >= 0x10Cu) {
        std::memcpy(&ctx->received_fssm_inline_const,
                    static_cast<const uint8_t*>(pUnpackedFssm) + 0x108, 4);
    }
    *pOutTargetFormat = D3D12XBOX_SHADER_TARGET_DXIL;
    *pOutBufferSize = ctx->compiled_sm66_dxil->size();
    if (!pOutBuffer) return S_OK;
    if (outBufferCapacity < ctx->compiled_sm66_dxil->size()) return E_NOT_SUFFICIENT_BUFFER;
    std::memcpy(pOutBuffer, ctx->compiled_sm66_dxil->data(), ctx->compiled_sm66_dxil->size());
    ++ctx->invoke_count;
    return S_OK;
}

D3D12_RESOURCE_BARRIER make_transition(
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

D3D12_RESOURCE_BARRIER make_uav_barrier(ID3D12Resource* res) noexcept {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = res;
    return b;
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
    rd.Width = bytes;
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

} // namespace

int main() {
    // Enable D3D12 Debug Layer cleanly before device creation
    {
        ComPtr<ID3D12Debug> debug_iface;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug_iface.GetAddressOf())))) {
            debug_iface->EnableDebugLayer();
        }
    }

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    params.ProcessDebugFlags = D3D12_PROCESS_DEBUG_FLAG_DEBUG_LAYER_ENABLED;

    ComPtr<ID3D12Device> dev;
    CHECK_HR(D3D12XboxCreateDevice(nullptr, &params, IID_PPV_ARGS(dev.GetAddressOf())));

    ComPtr<ID3D12InfoQueue> info_queue;
    dev.As(&info_queue);
    if (info_queue) {
        info_queue->ClearStoredMessages();
    }

    // 1. Check SM 6.6 / Dynamic Resources / Mesh Shader / DXR capabilities
    D3D12XBOX_SM66_CAPS caps{};
    CHECK_HR(D3D12XboxCheckSm66Support(dev.Get(), &caps));
    std::printf("[SM66] HighestSM=0x%X BindingTier=%u DynamicRes=%u Int64Atomics=%u Wave=%u..%u MeshTier=%u RtxTier=%u\n",
                caps.HighestShaderModel, caps.ResourceBindingTier, caps.SupportsDynamicResources,
                caps.SupportsInt64Atomics, caps.WaveLaneCountMin, caps.WaveLaneCountMax,
                caps.MeshShaderTier, caps.RaytracingTier);
    CHECK_COND(caps.HighestShaderModel >= 0x66u, "GPU must support Shader Model 6.6+");
    CHECK_COND(caps.SupportsDynamicResources == 1u, "GPU must support SM 6.6 Dynamic Resources");

    // 2. Create the standard 40-DWORD SM 6.6 Xbox-contract Root Signature
    ComPtr<ID3D12RootSignature> root_sig;
    CHECK_HR(D3D12XboxCreateSm66RootSignature(dev.Get(), IID_PPV_ARGS(root_sig.GetAddressOf())));

    // 3. Build & runtime-compile SM 6.6 HLSL using the d3d12_xs.dll preamble + dxcompiler.dll
    const char* preamble = nullptr;
    size_t preamble_len = 0;
    CHECK_HR(D3D12XboxGetSm66HlslPreamble(&preamble, &preamble_len));
    CHECK_COND(preamble && preamble_len > 256u, "SM 6.6 HLSL preamble must be non-empty");

    const char* kernel_src = R"HLSL(
[numthreads(32, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint tid = dtid.x;
    uint64_t in_va  = wsx_sgpr_u64(0) + uint64_t(tid) * 4ull;   // Slab 0
    uint64_t out_va = wsx_sgpr_u64(2) + uint64_t(tid) * 4ull;   // Slab 1
    uint64_t sum_va = wsx_sgpr_u64(16);                         // Slab 0 atomic accumulator

    uint in_val     = wsx_va_load32(in_va);
    uint fssm_const = wsx_code_load32(0x108u);                  // s_getpc_b64 -> Slab 1

    uint4 t0 = uint4(wsx_sgpr(4),  wsx_sgpr(5),  wsx_sgpr(6),  wsx_sgpr(7));
    uint4 t1 = uint4(wsx_sgpr(8),  wsx_sgpr(9),  wsx_sgpr(10), wsx_sgpr(11));
    uint4 s0 = uint4(wsx_sgpr(12), wsx_sgpr(13), wsx_sgpr(14), wsx_sgpr(15));

    uint tex_idx = wsx_lookup_tsharp(t0, t1);
    uint smp_idx = wsx_lookup_ssharp(s0);

    Texture2D<float4> tex = ResourceDescriptorHeap[NonUniformResourceIndex(tex_idx)];
    SamplerState smp      = SamplerDescriptorHeap[NonUniformResourceIndex(smp_idx)];
    float4 sample_val     = tex.SampleLevel(smp, float2(0.5, 0.5), 0.0);
    uint tex_scaled       = uint(sample_val.r * 1000.0 + 0.5);

    wsx_va_store32(out_va, in_val + fssm_const + tex_scaled);
    wsx_va_atomic_add32(sum_va, tid + 1u);
}
)HLSL";

    std::string full_hlsl(preamble, preamble_len);
    full_hlsl += "\n";
    full_hlsl += kernel_src;

    size_t dxil_size = 0;
    char dxc_errors[2048]{};
    HRESULT hr = D3D12XboxCompileHlslToDxil(
        full_hlsl.c_str(), full_hlsl.size(),
        L"main", L"cs_6_6",
        nullptr, 0, &dxil_size,
        dxc_errors, sizeof(dxc_errors));
    if (FAILED(hr)) {
        std::fprintf(stderr, "DXC size query failed: hr=0x%08lX errors:\n%s\n",
                     static_cast<unsigned long>(hr), dxc_errors);
        return 1;
    }
    CHECK_COND(dxil_size > 64u, "Compiled SM 6.6 DXIL must be non-empty");

    std::vector<uint8_t> compiled_dxil(dxil_size);
    CHECK_HR(D3D12XboxCompileHlslToDxil(
        full_hlsl.c_str(), full_hlsl.size(),
        L"main", L"cs_6_6",
        compiled_dxil.data(), compiled_dxil.size(), &dxil_size,
        dxc_errors, sizeof(dxc_errors)));

    D3D12XBOX_SHADER_INFO dxil_info{};
    CHECK_HR(D3D12XboxInspectShaderBytecode(compiled_dxil.data(), compiled_dxil.size(), &dxil_info));
    CHECK_COND(dxil_info.Kind == D3D12XBOX_SHADER_KIND_DESKTOP_DXIL, "Compiled DXIL must be desktop DXIL");
    CHECK_COND(dxil_info.XboxIntrinsicCount == 0u, "Compiled DXIL must have 0 @xbox.* intrinsics");
    std::printf("[SM66] Runtime DXC compiled cs_6_6 -> %zu bytes signed DXIL (SM %u.%u)\n",
                dxil_size, dxil_info.ShaderModelMajor, dxil_info.ShaderModelMinor);

    // 4. Feed a stripped NOBC/FSSM Xbox shader through CreateComputePipelineState via translator hook
    const std::vector<uint8_t> fssm_dxbc = make_synthetic_fssm_cs();
    TranslatorCtx tctx{};
    tctx.compiled_sm66_dxil = &compiled_dxil;
    CHECK_HR(D3D12XboxRegisterShaderTranslator(&sm66_test_translator, &tctx));

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root_sig.Get();
    pso_desc.CS.pShaderBytecode = fssm_dxbc.data();
    pso_desc.CS.BytecodeLength = fssm_dxbc.size();
    ComPtr<ID3D12PipelineState> pso;
    const HRESULT pso_hr = dev->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(pso.GetAddressOf()));
    D3D12XboxRegisterShaderTranslator(nullptr, nullptr);
    if (FAILED(pso_hr) && info_queue) {
        const UINT64 n = info_queue->GetNumStoredMessages();
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T len = 0;
            info_queue->GetMessage(i, nullptr, &len);
            if (!len) continue;
            std::vector<uint8_t> buf(len);
            auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(info_queue->GetMessage(i, msg, &len))) {
                std::fprintf(stderr, "D3D12 MSG[%llu]: %s\n", static_cast<unsigned long long>(i), msg->pDescription);
            }
        }
    }
    CHECK_HR(pso_hr);
    CHECK_COND(tctx.invoke_count == 1u, "Translator callback must fire once for FSSM/NOBC shader");
    CHECK_COND(tctx.received_fssm_size == 0x140u && tctx.received_fssm_inline_const == 0x00001337u,
               "Translator must receive unpacked FSSM blob with intact inline constant");

    // 5. Allocate GPU slabs, Texture2D, FNV-1a T#/S# table, descriptor heaps & execute on real GPU
    constexpr uint32_t kSlabShift = 16u; // 64 KB per slab
    constexpr uint64_t kSlabBytes = 1ull << kSlabShift;
    constexpr uint64_t kHeapBase = 0x0000001000000000ull;
    constexpr uint64_t kCodeBase = kHeapBase + kSlabBytes + 0x200ull; // inside Slab 1
    constexpr uint32_t kNumLanes = 32u;

    ComPtr<ID3D12Resource> slab0, slab1, slab0_up, slab1_up, slab0_rb, slab1_rb;
    CHECK_HR(create_buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, kSlabBytes,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON,
                           slab0.GetAddressOf()));
    CHECK_HR(create_buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, kSlabBytes,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON,
                           slab1.GetAddressOf()));
    CHECK_HR(create_buffer(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, kSlabBytes,
                           D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                           slab0_up.GetAddressOf()));
    CHECK_HR(create_buffer(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, kSlabBytes,
                           D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                           slab1_up.GetAddressOf()));
    CHECK_HR(create_buffer(dev.Get(), D3D12_HEAP_TYPE_READBACK, kSlabBytes,
                           D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                           slab0_rb.GetAddressOf()));
    CHECK_HR(create_buffer(dev.Get(), D3D12_HEAP_TYPE_READBACK, kSlabBytes,
                           D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                           slab1_rb.GetAddressOf()));

    // Populate Slab 0 upload: 32 input dwords at 0x0000, atomic sum = 0 at 0x0400
    {
        void* p = nullptr;
        CHECK_HR(slab0_up->Map(0, nullptr, &p));
        std::memset(p, 0, static_cast<size_t>(kSlabBytes));
        auto* d = static_cast<uint32_t*>(p);
        for (uint32_t i = 0; i < kNumLanes; ++i) d[i] = 100u + i;
        slab0_up->Unmap(0, nullptr);
    }

    // Populate Slab 1 upload: unpacked FSSM blob at 0x200 (inline constant 0x1337 at +0x108 -> 0x308)
    {
        size_t fssm_sz = 0;
        uint32_t fssm_code_off = 0;
        CHECK_HR(D3D12XboxUnpackShaderFssm(fssm_dxbc.data(), fssm_dxbc.size(),
                                           nullptr, 0, &fssm_sz, &fssm_code_off));
        std::vector<uint8_t> fssm_bytes(fssm_sz);
        CHECK_HR(D3D12XboxUnpackShaderFssm(fssm_dxbc.data(), fssm_dxbc.size(),
                                           fssm_bytes.data(), fssm_bytes.size(), &fssm_sz, &fssm_code_off));
        void* p = nullptr;
        CHECK_HR(slab1_up->Map(0, nullptr, &p));
        std::memset(p, 0, static_cast<size_t>(kSlabBytes));
        std::memcpy(static_cast<uint8_t*>(p) + 0x200, fssm_bytes.data(), fssm_bytes.size());
        slab1_up->Unmap(0, nullptr);
    }

    // Create 4x4 RGBA32F Texture2D filled with (0.25, 0.5, 0.75, 1.0)
    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = 4;
    tex_desc.Height = 4;
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = 1;
    tex_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES default_hp{};
    default_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> tex2d;
    CHECK_HR(dev->CreateCommittedResource(&default_hp, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                          D3D12_RESOURCE_STATE_COMMON, nullptr,
                                          IID_PPV_ARGS(tex2d.GetAddressOf())));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT tex_fp{};
    UINT tex_num_rows = 0;
    UINT64 tex_row_bytes = 0, tex_total_bytes = 0;
    dev->GetCopyableFootprints(&tex_desc, 0, 1, 0, &tex_fp, &tex_num_rows, &tex_row_bytes, &tex_total_bytes);
    ComPtr<ID3D12Resource> tex_up;
    CHECK_HR(create_buffer(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, tex_total_bytes,
                           D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                           tex_up.GetAddressOf()));
    {
        void* p = nullptr;
        CHECK_HR(tex_up->Map(0, nullptr, &p));
        std::memset(p, 0, static_cast<size_t>(tex_total_bytes));
        for (UINT y = 0; y < 4; ++y) {
            auto* row = reinterpret_cast<float*>(static_cast<uint8_t*>(p) + tex_fp.Offset + y * tex_fp.Footprint.RowPitch);
            for (UINT x = 0; x < 4; ++x) {
                row[x * 4 + 0] = 0.25f;
                row[x * 4 + 1] = 0.50f;
                row[x * 4 + 2] = 0.75f;
                row[x * 4 + 3] = 1.00f;
            }
        }
        tex_up->Unmap(0, nullptr);
    }

    // Create FNV-1a T#/S# descriptor hash table (64 entries)
    constexpr uint32_t kHashCap = 64u;
    const uint32_t tsharp_key[8] = {
        0xDEAD0001u, 0xBEEF0002u, 0xCAFE0003u, 0xBABE0004u,
        0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u
    };
    const uint32_t ssharp_key[8] = {
        0x5A5A1111u, 0xA5A52222u, 0x13573333u, 0x24684444u,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu
    };
    std::vector<D3D12XBOX_SM66_DESC_HASH_ENTRY> hash_entries(kHashCap);
    insert_desc_entry(hash_entries, tsharp_key, 2u, 1u); // ResourceDescriptorHeap[2] = tex2d
    insert_desc_entry(hash_entries, ssharp_key, 1u, 2u); // SamplerDescriptorHeap[1] = linear clamp

    const uint64_t hash_bytes = hash_entries.size() * sizeof(D3D12XBOX_SM66_DESC_HASH_ENTRY);
    ComPtr<ID3D12Resource> hash_buf, hash_up;
    CHECK_HR(create_buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, hash_bytes,
                           D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON,
                           hash_buf.GetAddressOf()));
    CHECK_HR(create_buffer(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, hash_bytes,
                           D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                           hash_up.GetAddressOf()));
    {
        void* p = nullptr;
        CHECK_HR(hash_up->Map(0, nullptr, &p));
        std::memcpy(p, hash_entries.data(), static_cast<size_t>(hash_bytes));
        hash_up->Unmap(0, nullptr);
    }

    // Create shader-visible CBV_SRV_UAV heap (4 descriptors: Slab0 UAV, Slab1 UAV, Tex2D SRV, HashTable SRV)
    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc{};
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.NumDescriptors = 8;
    srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    CHECK_HR(dev->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(srv_heap.GetAddressOf())));
    const UINT srv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = srv_heap->GetCPUDescriptorHandleForHeapStart();

    D3D12_UNORDERED_ACCESS_VIEW_DESC raw_uav{};
    raw_uav.Format = DXGI_FORMAT_R32_TYPELESS;
    raw_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    raw_uav.Buffer.FirstElement = 0;
    raw_uav.Buffer.NumElements = static_cast<UINT>(kSlabBytes / 4u);
    raw_uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    dev->CreateUnorderedAccessView(slab0.Get(), nullptr, &raw_uav, srv_cpu);
    srv_cpu.ptr += srv_inc;
    dev->CreateUnorderedAccessView(slab1.Get(), nullptr, &raw_uav, srv_cpu);
    srv_cpu.ptr += srv_inc;

    D3D12_SHADER_RESOURCE_VIEW_DESC tex_srv{};
    tex_srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    tex_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    tex_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    tex_srv.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(tex2d.Get(), &tex_srv, srv_cpu);
    srv_cpu.ptr += srv_inc;

    D3D12_SHADER_RESOURCE_VIEW_DESC hash_srv{};
    hash_srv.Format = DXGI_FORMAT_UNKNOWN;
    hash_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    hash_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    hash_srv.Buffer.FirstElement = 0;
    hash_srv.Buffer.NumElements = kHashCap;
    hash_srv.Buffer.StructureByteStride = sizeof(D3D12XBOX_SM66_DESC_HASH_ENTRY);
    dev->CreateShaderResourceView(hash_buf.Get(), &hash_srv, srv_cpu);

    // Create shader-visible SAMPLER heap (slot 1 = linear clamp)
    D3D12_DESCRIPTOR_HEAP_DESC smp_heap_desc{};
    smp_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    smp_heap_desc.NumDescriptors = 4;
    smp_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> smp_heap;
    CHECK_HR(dev->CreateDescriptorHeap(&smp_heap_desc, IID_PPV_ARGS(smp_heap.GetAddressOf())));
    const UINT smp_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    D3D12_CPU_DESCRIPTOR_HANDLE smp_cpu = smp_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SAMPLER_DESC smp_desc{};
    smp_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp_desc.MaxLOD = D3D12_FLOAT32_MAX;
    dev->CreateSampler(&smp_desc, smp_cpu);
    smp_cpu.ptr += smp_inc;
    dev->CreateSampler(&smp_desc, smp_cpu);

    // Command queue, allocator, list & fence
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    CHECK_HR(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(queue.GetAddressOf())));
    ComPtr<ID3D12CommandAllocator> alloc;
    CHECK_HR(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.GetAddressOf())));
    ComPtr<ID3D12GraphicsCommandList> cl;
    CHECK_HR(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(cl.GetAddressOf())));

    // Upload all buffers & texture
    cl->CopyBufferRegion(slab0.Get(), 0, slab0_up.Get(), 0, kSlabBytes);
    cl->CopyBufferRegion(slab1.Get(), 0, slab1_up.Get(), 0, kSlabBytes);
    cl->CopyBufferRegion(hash_buf.Get(), 0, hash_up.Get(), 0, hash_bytes);
    D3D12_TEXTURE_COPY_LOCATION tex_dst{};
    tex_dst.pResource = tex2d.Get();
    tex_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    tex_dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION tex_src{};
    tex_src.pResource = tex_up.Get();
    tex_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    tex_src.PlacedFootprint = tex_fp;
    cl->CopyTextureRegion(&tex_dst, 0, 0, 0, &tex_src, nullptr);

    D3D12_RESOURCE_BARRIER pre_barriers[4] = {
        make_transition(slab0.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        make_transition(slab1.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        make_transition(tex2d.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        make_transition(hash_buf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    cl->ResourceBarrier(4, pre_barriers);

    // Fill 40-DWORD SM 6.6 User Data Root Constants
    D3D12XBOX_SM66_USER_DATA ud{};
    const uint64_t in_va  = kHeapBase + 0x0000ull;                // Slab 0
    const uint64_t out_va = kHeapBase + kSlabBytes + 0x1000ull;   // Slab 1
    const uint64_t sum_va = kHeapBase + 0x0400ull;                // Slab 0 atomic
    ud.S[0] = static_cast<uint32_t>(in_va & 0xFFFFFFFFull);
    ud.S[1] = static_cast<uint32_t>(in_va >> 32);
    ud.S[2] = static_cast<uint32_t>(out_va & 0xFFFFFFFFull);
    ud.S[3] = static_cast<uint32_t>(out_va >> 32);
    for (int i = 0; i < 8; ++i) ud.S[4 + i] = tsharp_key[i];
    for (int i = 0; i < 4; ++i) ud.S[12 + i] = ssharp_key[i];
    ud.S[16] = static_cast<uint32_t>(sum_va & 0xFFFFFFFFull);
    ud.S[17] = static_cast<uint32_t>(sum_va >> 32);
    ud.HeapBaseLo = static_cast<uint32_t>(kHeapBase & 0xFFFFFFFFull);
    ud.HeapBaseHi = static_cast<uint32_t>(kHeapBase >> 32);
    ud.CodeBaseLo = static_cast<uint32_t>(kCodeBase & 0xFFFFFFFFull);
    ud.CodeBaseHi = static_cast<uint32_t>(kCodeBase >> 32);
    ud.SlabBaseIdx = 0u;
    ud.SlabShift = kSlabShift;
    ud.DescTableSrvIdx = 3u;
    ud.DescTableMask = kHashCap - 1u;

    ID3D12DescriptorHeap* heaps[2] = { srv_heap.Get(), smp_heap.Get() };
    cl->SetDescriptorHeaps(2, heaps);
    cl->SetComputeRootSignature(root_sig.Get());
    cl->SetPipelineState(pso.Get());
    cl->SetComputeRoot32BitConstants(0, 40u, &ud, 0u);
    cl->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER post_barriers[4] = {
        make_uav_barrier(slab0.Get()),
        make_uav_barrier(slab1.Get()),
        make_transition(slab0.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        make_transition(slab1.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
    };
    cl->ResourceBarrier(4, post_barriers);
    cl->CopyBufferRegion(slab0_rb.Get(), 0, slab0.Get(), 0, kSlabBytes);
    cl->CopyBufferRegion(slab1_rb.Get(), 0, slab1.Get(), 0, kSlabBytes);
    CHECK_HR(cl->Close());

    ID3D12CommandList* lists[1] = { cl.Get() };
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    CHECK_HR(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK_COND(ev != nullptr, "CreateEventW failed");
    CHECK_HR(queue->Signal(fence.Get(), 1));
    CHECK_HR(fence->SetEventOnCompletion(1, ev));
    CHECK_COND(WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0, "GPU fence wait timed out");
    CloseHandle(ev);

    CHECK_HR(dev->GetDeviceRemovedReason());

    // Verify Slab 1 outputs (0x1000): (100 + i) + 0x1337 + 250
    {
        void* p = nullptr;
        CHECK_HR(slab1_rb->Map(0, nullptr, &p));
        const auto* out_d = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(p) + 0x1000);
        for (uint32_t i = 0; i < kNumLanes; ++i) {
            const uint32_t expected = (100u + i) + 0x00001337u + 250u;
            if (out_d[i] != expected) {
                std::fprintf(stderr, "FAIL: lane %u got 0x%X expected 0x%X\n", i, out_d[i], expected);
                slab1_rb->Unmap(0, nullptr);
                return 1;
            }
        }
        slab1_rb->Unmap(0, nullptr);
    }

    // Verify Slab 0 atomic sum (0x0400): sum(1..32) = 528
    {
        void* p = nullptr;
        CHECK_HR(slab0_rb->Map(0, nullptr, &p));
        uint32_t atomic_sum = 0;
        std::memcpy(&atomic_sum, static_cast<const uint8_t*>(p) + 0x0400, 4);
        slab0_rb->Unmap(0, nullptr);
        CHECK_COND(atomic_sum == 528u, "Cross-slab atomic sum must equal 528");
        std::printf("[SM66] GPU readback verified: 32/32 lanes bit-exact (out[0]=0x%X, atomic_sum=%u)\n",
                    100u + 0x1337u + 250u, atomic_sum);
    }

    // Verify 0 D3D12 Debug Layer errors / corruptions
    if (info_queue) {
        const UINT64 num_msgs = info_queue->GetNumStoredMessages();
        uint32_t err_count = 0;
        for (UINT64 i = 0; i < num_msgs; ++i) {
            SIZE_T len = 0;
            info_queue->GetMessage(i, nullptr, &len);
            if (len == 0) continue;
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
        std::printf("[SM66] D3D12 Debug Layer verified: 0 errors (%llu total messages)\n",
                    static_cast<unsigned long long>(num_msgs));
    }

    std::printf("PASS: sm66_rdna_runtime\n");
    return 0;
}
