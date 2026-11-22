// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Automated unit & real-GPU integration test for d3d12_xs.dll's Xbox Series X|S
// Shader Container Inspector, FSSM Unpacker, Disk Cache, Translator Callback & Backend Switch:
// 1. Classifies all 5 shader container families (DesktopDxil, XboxDxilPure, XboxDxilIntrinsic,
//    XboxFssmRdna2 [XDAT+NOBC+XBCH->FSSM], XboxAcccRdna2 [ACCC/FSPX]).
// 2. Verifies built-in clean-room RFC 1950/1951 inflate unpacking XBCH -> FSSM + code offset + s_endpgm.
// 3. Verifies WSX_STRICT_SHADER_CHECK=1 pre-driver rejection (E_NOTIMPL) for untranslated RDNA2/@xbox.* shaders.
// 4. Verifies end-to-end GPU CreateComputePipelineState substitution of an untranslatable FSSM/NOBC shader via:
//    (a) WSX_SHADER_CACHE_DIR (<hash>.dxil disk cache)
//    (b) D3D12XboxRegisterShaderTranslator (runtime callback receiving unpacked FSSM bytes)
// 5. Verifies D3D12XboxGetGraphicsBackend (d3d12 / vulkan).

#include "include/xbox/d3d12_xs.h"
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

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

// Wrap arbitrary bytes in a valid RFC 1950 zlib stream using stored deflate block (type 0)
std::vector<uint8_t> make_zlib_stored(const std::vector<uint8_t>& plain) {
    std::vector<uint8_t> z;
    z.push_back(0x78u); // CMF: deflate, 32K window
    z.push_back(0x01u); // FLG: FCHECK valid (0x7801 % 31 == 0)
    z.push_back(0x01u); // BFINAL=1, BTYPE=00 (stored)
    const uint16_t len = static_cast<uint16_t>(plain.size());
    const uint16_t nlen = static_cast<uint16_t>(~len);
    append_u16(z, len);
    append_u16(z, nlen);
    append_bytes(z, plain.data(), plain.size());
    // Adler-32 trailer (big-endian)
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
    append_u32(out, 1u); // version
    append_u32(out, 0u); // total size patched below
    append_u32(out, static_cast<uint32_t>(chunks.size()));
    const size_t table_off = out.size();
    for (size_t i = 0; i < chunks.size(); ++i) append_u32(out, 0u);
    for (size_t i = 0; i < chunks.size(); ++i) {
        const uint32_t coff = static_cast<uint32_t>(out.size());
        std::memcpy(out.data() + table_off + i * 4, &coff, 4);
        append_bytes(out, chunks[i].fourcc, 4);
        append_u32(out, static_cast<uint32_t>(chunks[i].payload.size()));
        append_bytes(out, chunks[i].payload.data(), chunks[i].payload.size());
    }
    const uint32_t tot = static_cast<uint32_t>(out.size());
    std::memcpy(out.data() + 24, &tot, 4);
    return out;
}

std::vector<uint8_t> encode_char6_bytes(const char* s) {
    auto code = [](char ch) -> uint32_t {
        if (ch >= 'a' && ch <= 'z') return static_cast<uint32_t>(ch - 'a');
        if (ch >= 'A' && ch <= 'Z') return static_cast<uint32_t>(26 + ch - 'A');
        if (ch >= '0' && ch <= '9') return static_cast<uint32_t>(52 + ch - '0');
        if (ch == '.') return 62u;
        return 63u;
    };
    std::vector<uint8_t> out(16, 0u);
    uint64_t bitpos = 3; // deliberate non-byte-aligned bit offset to test bit-unaligned scanning!
    for (size_t i = 0; s[i] != '\0'; ++i) {
        const uint32_t c = code(s[i]);
        for (uint32_t b = 0; b < 6; ++b) {
            if ((c >> b) & 1u) {
                const size_t bi = static_cast<size_t>((bitpos + b) / 8);
                if (bi < out.size()) out[bi] |= static_cast<uint8_t>(1u << ((bitpos + b) % 8));
            }
        }
        bitpos += 6;
    }
    return out;
}

std::string hex16(const uint8_t h[16]) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s(32, '0');
    for (int i = 0; i < 16; ++i) {
        s[i * 2] = kHex[(h[i] >> 4) & 0xF];
        s[i * 2 + 1] = kHex[h[i] & 0xF];
    }
    return s;
}

struct TranslatorTestCtx {
    const void* replacement = nullptr;
    size_t replacement_size = 0;
    int calls = 0;
    bool saw_fssm = false;
};

HRESULT WINAPI test_translator_cb(
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
    auto* ctx = static_cast<TranslatorTestCtx*>(pUserContext);
    if (!ctx || !pInfo || !pOutTargetFormat || !pOutBufferSize) return E_INVALIDARG;
    if (pInfo->Kind != D3D12XBOX_SHADER_KIND_XBOX_FSSM_RDNA2) return E_NOTIMPL;
    if (pUnpackedFssm && unpackedFssmSize >= 4 && std::memcmp(pUnpackedFssm, "FSSM", 4) == 0) {
        ctx->saw_fssm = true;
    }
    *pOutTargetFormat = D3D12XBOX_SHADER_TARGET_DXIL;
    *pOutBufferSize = ctx->replacement_size;
    if (!pOutBuffer) return S_OK;
    if (outBufferCapacity < ctx->replacement_size) return E_NOT_SUFFICIENT_BUFFER;
    std::memcpy(pOutBuffer, ctx->replacement, ctx->replacement_size);
    ++ctx->calls;
    return S_OK;
}

} // namespace

int main() {
    std::puts("=== MizuBox Xbox Shader Pipeline & FSSM Verification Test ===");

    // 1. Test synthetic FSSM (XDAT + NOBC + XBCH zlib -> FSSM + RDNA2 s_endpgm)
    std::vector<uint8_t> synthetic_fssm(0x140, 0u);
    std::memcpy(synthetic_fssm.data(), "FSSM", 4);
    const uint32_t raw_c0 = 0xE0u; // code_offset = 0xE0 + 0x28 = 0x108
    std::memcpy(synthetic_fssm.data() + 0xC0, &raw_c0, 4);
    const uint32_t rdna_nop = 0xBF800000u;    // s_nop 0
    const uint32_t rdna_end = 0xBF810000u;    // s_endpgm
    std::memcpy(synthetic_fssm.data() + 0x108, &rdna_nop, 4);
    std::memcpy(synthetic_fssm.data() + 0x10C, &rdna_end, 4);

    const std::vector<uint8_t> z_fssm = make_zlib_stored(synthetic_fssm);
    std::vector<uint8_t> xbch;
    append_bytes(xbch, "XBCH", 4);
    append_u32(xbch, 1u);
    append_u32(xbch, static_cast<uint32_t>(z_fssm.size()));
    append_u32(xbch, 0u);
    append_u32(xbch, static_cast<uint32_t>(synthetic_fssm.size()));
    append_u32(xbch, 0u);
    append_bytes(xbch, z_fssm.data(), z_fssm.size());

    std::vector<uint8_t> nobc_ent(28, 0u);
    std::memcpy(nobc_ent.data() + 8, "NOBC", 4);

    const uint8_t xdat_hash[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    std::vector<uint8_t> dxil_fssm;
    append_u32(dxil_fssm, 0x000D0066u); // Mesh Shader SM 6.6
    append_u32(dxil_fssm, 0u);
    append_bytes(dxil_fssm, "DXIL", 4);
    append_u32(dxil_fssm, 0x106u);
    append_u32(dxil_fssm, 0u);
    append_u32(dxil_fssm, 0u);
    // XDAT starts at offset 24
    const uint32_t xdat_header_end = 24u + 12u + 3u * 12u;
    const uint32_t off_hash = xdat_header_end;
    const uint32_t off_xbch = off_hash + 16u;
    const uint32_t off_nobc = off_xbch + static_cast<uint32_t>(xbch.size());
    append_bytes(dxil_fssm, "XDAT", 4);
    append_u32(dxil_fssm, 3u);
    append_u32(dxil_fssm, 12u + 3u * 12u);
    append_u32(dxil_fssm, 0x12u); append_u32(dxil_fssm, off_hash); append_u32(dxil_fssm, 16u);
    append_u32(dxil_fssm, 0x0Du); append_u32(dxil_fssm, off_xbch); append_u32(dxil_fssm, static_cast<uint32_t>(xbch.size()));
    append_u32(dxil_fssm, 0x07u); append_u32(dxil_fssm, off_nobc); append_u32(dxil_fssm, static_cast<uint32_t>(nobc_ent.size()));
    append_bytes(dxil_fssm, xdat_hash, 16);
    append_bytes(dxil_fssm, xbch.data(), xbch.size());
    append_bytes(dxil_fssm, nobc_ent.data(), nobc_ent.size());

    const uint8_t fssm_digest[16] = {0xAA,0xBB,0xCC,0xDD,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0x00,0xFE,0xED};
    std::vector<uint8_t> hash_chunk(20, 0u);
    std::memcpy(hash_chunk.data() + 4, fssm_digest, 16);
    const std::vector<uint8_t> dxbc_fssm = build_dxbc(
        fssm_digest, {{{'H','A','S','H'}, hash_chunk}, {{'D','X','I','L'}, dxil_fssm}});

    D3D12XBOX_SHADER_INFO info{};
    CHECK_HR(D3D12XboxInspectShaderBytecode(dxbc_fssm.data(), dxbc_fssm.size(), &info));
    CHECK_COND(info.Kind == D3D12XBOX_SHADER_KIND_XBOX_FSSM_RDNA2, "Expected XBOX_FSSM_RDNA2");
    CHECK_COND(info.Stage == D3D12XBOX_SHADER_STAGE_MESH, "Expected Mesh Shader stage (13)");
    CHECK_COND(info.ShaderModelMajor == 6 && info.ShaderModelMinor == 6, "Expected SM 6.6");
    CHECK_COND(info.HasNobcStub == 1 && info.HasRdna2EndPgm == 1, "Expected NOBC + s_endpgm");
    CHECK_COND(info.FssmCodeOffset == 0x108u && info.FssmCodeSize == 8u, "Expected FSSM code range 0x108..+8");
    CHECK_COND(std::memcmp(info.XdatHash, xdat_hash, 16) == 0, "Expected XDAT hash match");

    std::vector<uint8_t> unpacked_fssm(512, 0u);
    size_t unpacked_sz = 0;
    uint32_t unpacked_code_off = 0;
    CHECK_HR(D3D12XboxUnpackShaderFssm(dxbc_fssm.data(), dxbc_fssm.size(),
                                       unpacked_fssm.data(), unpacked_fssm.size(),
                                       &unpacked_sz, &unpacked_code_off));
    CHECK_COND(unpacked_sz == synthetic_fssm.size() && unpacked_code_off == 0x108u &&
               std::memcmp(unpacked_fssm.data(), synthetic_fssm.data(), unpacked_sz) == 0,
               "Unpacked FSSM must match original byte-for-byte");

    // 2. Test ACCC/FSPX (FSR 4.1.1 Xbox format)
    std::vector<uint8_t> accc_payload(64, 0u);
    std::memcpy(accc_payload.data(), "FSPX", 4);
    std::memcpy(accc_payload.data() + 48, &rdna_end, 4);
    const std::vector<uint8_t> dxbc_accc = build_dxbc(fssm_digest, {{{'A','C','C','C'}, accc_payload}});
    CHECK_HR(D3D12XboxInspectShaderBytecode(dxbc_accc.data(), dxbc_accc.size(), &info));
    CHECK_COND(info.Kind == D3D12XBOX_SHADER_KIND_XBOX_ACCC_RDNA2, "Expected XBOX_ACCC_RDNA2");
    CHECK_COND(info.HasRdna2EndPgm == 1 && info.AcccPayloadSize == 64u, "Expected ACCC s_endpgm + size");

    // 3. Test XboxDxilPure vs XboxDxilIntrinsic (with xbox.TypedLoad)
    std::vector<uint8_t> dxil_pure(64, 0u);
    append_u32(dxil_pure, 0x00050065u); // CS 6.5 (put at offset 0)
    std::memset(dxil_pure.data(), 0, dxil_pure.size());
    const uint32_t cs65 = 0x00050065u;
    std::memcpy(dxil_pure.data(), &cs65, 4);
    std::memcpy(dxil_pure.data() + 24, "xbox.compiler.options", 21);
    const std::vector<uint8_t> dxbc_pure = build_dxbc(fssm_digest, {{{'D','X','I','L'}, dxil_pure}});
    CHECK_HR(D3D12XboxInspectShaderBytecode(dxbc_pure.data(), dxbc_pure.size(), &info));
    CHECK_COND(info.Kind == D3D12XBOX_SHADER_KIND_XBOX_DXIL_PURE && info.XboxIntrinsicCount == 0,
               "Expected XBOX_DXIL_PURE with 0 intrinsics");

    std::vector<uint8_t> dxil_intr = dxil_pure;
    const auto c6_typed = encode_char6_bytes("xbox.TypedLoad");
    append_bytes(dxil_intr, c6_typed.data(), c6_typed.size());
    const std::vector<uint8_t> dxbc_intr = build_dxbc(fssm_digest, {{{'D','X','I','L'}, dxil_intr}});
    CHECK_HR(D3D12XboxInspectShaderBytecode(dxbc_intr.data(), dxbc_intr.size(), &info));
    CHECK_COND(info.Kind == D3D12XBOX_SHADER_KIND_XBOX_DXIL_INTRINSIC &&
               info.XboxIntrinsicCount >= 1 && info.HasXboxTypedLoad == 1,
               "Expected XBOX_DXIL_INTRINSIC with xbox.TypedLoad");

    // 4. Test Graphics Backend selector
    SetEnvironmentVariableW(L"WSX_GRAPHICS_BACKEND", nullptr);
    CHECK_COND(D3D12XboxGetGraphicsBackend() == D3D12XBOX_GRAPHICS_BACKEND_D3D12, "Default backend must be D3D12");
    SetEnvironmentVariableW(L"WSX_GRAPHICS_BACKEND", L"vulkan");
    CHECK_COND(D3D12XboxGetGraphicsBackend() == D3D12XBOX_GRAPHICS_BACKEND_VULKAN, "WSX_GRAPHICS_BACKEND=vulkan");
    SetEnvironmentVariableW(L"WSX_GRAPHICS_BACKEND", nullptr);

    // 5. Real GPU end-to-end test: strict rejection, disk shader cache substitution, and translator callback!
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    ComPtr<ID3D12Device8> dev;
    CHECK_HR(D3D12XboxCreateDevice(nullptr, &params, IID_GRAPHICS_PPV_ARGS(dev.GetAddressOf())));

    // Create empty desktop root signature + compile valid desktop CS_5_0 replacement bytecode
    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    ComPtr<ID3DBlob> rsBlob, rsErr, csBlob, csErr;
    CHECK_HR(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf()));
    ComPtr<ID3D12RootSignature> rs;
    CHECK_HR(dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(rs.GetAddressOf())));

    const char* hlsl = "[numthreads(1,1,1)] void main() {}";
    CHECK_HR(D3DCompile(hlsl, std::strlen(hlsl), "wsx_stub.hlsl", nullptr, nullptr, "main", "cs_5_0",
                        0, 0, csBlob.GetAddressOf(), csErr.GetAddressOf()));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rs.Get();
    psoDesc.CS = {dxbc_fssm.data(), dxbc_fssm.size()};

    // 5a. Strict rejection of untranslated FSSM shader (must return E_NOTIMPL before touching GPU driver)
    D3D12XBOX_ABI_TELEMETRY t0{}, t1{};
    D3D12XboxGetAbiTelemetry(&t0);
    SetEnvironmentVariableW(L"WSX_STRICT_SHADER_CHECK", L"1");
    ComPtr<ID3D12PipelineState> rejectedPso;
    const HRESULT strictHr = dev->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(rejectedPso.GetAddressOf()));
    CHECK_COND(strictHr == E_NOTIMPL && !rejectedPso, "Strict shader check must reject untranslated FSSM with E_NOTIMPL");
    D3D12XboxGetAbiTelemetry(&t1);
    CHECK_COND(t1.ShadersRejectedStrict == t0.ShadersRejectedStrict + 1 &&
               t1.ShadersFssmRdna2 == t0.ShadersFssmRdna2 + 1, "Telemetry must record FSSM strict rejection");

    // 5b. Disk pre-translated shader cache override (<cache>/<hash>.dxil) succeeds on real GPU!
    const fs::path cacheDir = fs::temp_directory_path() / "wsx_shader_cache_test";
    std::error_code ec;
    fs::create_directories(cacheDir, ec);
    const fs::path cachedFile = cacheDir / (hex16(fssm_digest) + ".dxil");
    {
        std::ofstream cf(cachedFile, std::ios::binary | std::ios::trunc);
        cf.write(static_cast<const char*>(csBlob->GetBufferPointer()),
                 static_cast<std::streamsize>(csBlob->GetBufferSize()));
    }
    SetEnvironmentVariableW(L"WSX_SHADER_CACHE_DIR", cacheDir.wstring().c_str());
    D3D12XboxGetAbiTelemetry(&t0);
    ComPtr<ID3D12PipelineState> cachedPso;
    CHECK_HR(dev->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(cachedPso.GetAddressOf())));
    CHECK_COND(cachedPso != nullptr, "Disk shader cache substitution must produce real ID3D12PipelineState");
    D3D12XboxGetAbiTelemetry(&t1);
    CHECK_COND(t1.ShadersCacheHits == t0.ShadersCacheHits + 1, "Telemetry must record disk cache hit");
    SetEnvironmentVariableW(L"WSX_SHADER_CACHE_DIR", nullptr);
    fs::remove_all(cacheDir, ec);

    // 5c. Runtime pluggable translator callback (D3D12XboxRegisterShaderTranslator) succeeds on real GPU!
    TranslatorTestCtx tctx{};
    tctx.replacement = csBlob->GetBufferPointer();
    tctx.replacement_size = csBlob->GetBufferSize();
    CHECK_HR(D3D12XboxRegisterShaderTranslator(&test_translator_cb, &tctx));
    D3D12XboxGetAbiTelemetry(&t0);
    ComPtr<ID3D12PipelineState> translatedPso;
    CHECK_HR(dev->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(translatedPso.GetAddressOf())));
    CHECK_COND(translatedPso != nullptr && tctx.calls == 1 && tctx.saw_fssm,
               "Pluggable translator callback must receive unpacked FSSM and produce real PSO");
    D3D12XboxGetAbiTelemetry(&t1);
    CHECK_COND(t1.ShadersTranslatorHits == t0.ShadersTranslatorHits + 1, "Telemetry must record translator hit");

    D3D12XboxRegisterShaderTranslator(nullptr, nullptr);
    SetEnvironmentVariableW(L"WSX_STRICT_SHADER_CHECK", nullptr);

    std::puts("PASS: shader_pipeline (FSSM unpack + ACCC + @xbox.* bitcode + strict check + disk cache + translator callback)");
    return 0;
}
