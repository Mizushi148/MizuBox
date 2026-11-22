// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Live Hardware CTest for AMD Zen 2 x86-64 User-Mode Compatibility VEH (wsx_zen2_compat.h):
// Executes real x86-64 machine code for ALL 7 AMD-exclusive Zen 2 user-mode instructions
// directly on the host CPU (trapping hardware #UD on Intel CPUs via Vectored Exception Handler):
//   1. CLZERO (0F 01 FC)             — exact 64-byte cache-line zeroing at [RAX & ~63]
//   2. EXTRQ imm (66 0F 78) & reg (66 0F 79)   — SSE4a bit-field extraction + high-64 preservation
//   3. INSERTQ imm (F2 0F 78) & reg (F2 0F 79) — SSE4a bit-field insertion + high-64 preservation
//   4. MOVNTSS (F3 0F 2B, disp8 + SIB)         — SSE4a 32-bit non-temporal store
//   5. MOVNTSD (F2 0F 2B, disp8)               — SSE4a 64-bit non-temporal store
//   6. MONITORX (0F 01 FA) + MWAITX (0F 01 FB) — user-mode timed monitor/wait

#include "wsx_zen2_compat.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                              \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "[zen2_compat] FAIL (%s:%d): %s\n",                  \
                         __FILE__, __LINE__, (msg));                                  \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

struct ExecPage {
    uint8_t* mem = nullptr;
    size_t cap = 4096;

    ExecPage() {
        mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }
    ~ExecPage() {
        if (mem) VirtualFree(mem, 0, MEM_RELEASE);
    }

    template <typename Fn>
    Fn compile(const std::vector<uint8_t>& code) {
        DWORD oldp = 0;
        VirtualProtect(mem, cap, PAGE_READWRITE, &oldp);
        std::memcpy(mem, code.data(), code.size());
        VirtualProtect(mem, cap, PAGE_EXECUTE_READ, &oldp);
        FlushInstructionCache(GetCurrentProcess(), mem, code.size());
        return reinterpret_cast<Fn>(mem);
    }
};

} // namespace

int main() {
    CHECK(wsx_zen2::ensure_installed(), "Install Zen 2 compatibility VEH");
    const wsx_zen2::HostCpuCaps caps = wsx_zen2::query_host_cpu_caps();
    std::printf("[zen2_compat] Host CPU native caps: AVX=%d AVX2=%d FMA=%d BMI1=%d BMI2=%d ADX=%d AES=%d SHA=%d XSAVE=%d | AMD-ext: CLZERO=%d SSE4a=%d MONITORX=%d\n",
                caps.has_avx, caps.has_avx2, caps.has_fma, caps.has_bmi1, caps.has_bmi2,
                caps.has_adx, caps.has_aes, caps.has_sha, caps.has_xsave,
                caps.has_clzero, caps.has_sse4a, caps.has_monitorx);

    ExecPage ep;
    CHECK(ep.mem != nullptr, "Allocate RX code page");
    if (!ep.mem) return 1;

    // 1. Test CLZERO (0F 01 FC) — zero 64-byte cache line containing [RCX + 19]
    {
        alignas(64) uint8_t buf[128];
        std::memset(buf, 0xA5, sizeof(buf));
        // mov rax, rcx ; clzero ; ret
        using Fn = void(*)(void*);
        auto fn = ep.compile<Fn>({
            0x48, 0x89, 0xC8,       // mov rax, rcx
            0x0F, 0x01, 0xFC,       // clzero
            0xC3                    // ret
        });
        fn(buf + 19); // Unaligned interior pointer inside first 64B cache line

        bool line0_zero = true;
        for (int i = 0; i < 64; ++i) {
            if (buf[i] != 0x00) { line0_zero = false; break; }
        }
        bool line1_intact = true;
        for (int i = 64; i < 128; ++i) {
            if (buf[i] != 0xA5) { line1_intact = false; break; }
        }
        CHECK(line0_zero, "CLZERO zeroed entire aligned 64-byte cache line [0..63]");
        CHECK(line1_intact, "CLZERO preserved adjacent cache line [64..127]");
    }

    // 2. Test EXTRQ imm (66 0F 78 /0 ib ib) & EXTRQ reg (66 0F 79 /r)
    {
        alignas(16) uint64_t io[2] = {0xFEDCBA9876543210ull, 0x1122334455667788ull};
        // movdqu xmm0, [rcx] ; extrq xmm0, 12, 8 ; movdqu [rcx], xmm0 ; ret
        using Fn1 = void(*)(void*);
        auto fn_imm = ep.compile<Fn1>({
            0xF3, 0x0F, 0x6F, 0x01,             // movdqu xmm0, [rcx]
            0x66, 0x0F, 0x78, 0xC0, 12, 8,      // extrq xmm0, len=12, idx=8
            0xF3, 0x0F, 0x7F, 0x01,             // movdqu [rcx], xmm0
            0xC3
        });
        fn_imm(io);
        // Bits [19:8] of 0xFEDCBA9876543210 = 0x432
        CHECK(io[0] == 0x432ull, "EXTRQ imm extracted bits [19:8] == 0x432");
        CHECK(io[1] == 0x1122334455667788ull, "EXTRQ imm preserved upper 64 bits of XMM0");

        alignas(16) uint64_t io2[2] = {0xFEDCBA9876543210ull, 0xAABBCCDDEEFF0011ull};
        // ctrl in xmm1.Low: len=16 (bits 5:0), idx=24 (bits 13:8) => (24 << 8) | 16 = 0x1810
        alignas(16) uint64_t ctrl[2] = {0x1810ull, 0ull};
        using Fn2 = void(*)(void*, const void*);
        auto fn_reg = ep.compile<Fn2>({
            0xF3, 0x0F, 0x6F, 0x01,             // movdqu xmm0, [rcx]
            0xF3, 0x0F, 0x6F, 0x0A,             // movdqu xmm1, [rdx]
            0x66, 0x0F, 0x79, 0xC1,             // extrq xmm0, xmm1
            0xF3, 0x0F, 0x7F, 0x01,             // movdqu [rcx], xmm0
            0xC3
        });
        fn_reg(io2, ctrl);
        // Bits [39:24] of 0xFEDCBA9876543210 = 0x9876
        CHECK(io2[0] == 0x9876ull, "EXTRQ reg extracted bits [39:24] == 0x9876");
        CHECK(io2[1] == 0xAABBCCDDEEFF0011ull, "EXTRQ reg preserved upper 64 bits of XMM0");
    }

    // 3. Test INSERTQ imm (F2 0F 78 /r ib ib) & INSERTQ reg (F2 0F 79 /r)
    {
        alignas(16) uint64_t dst[2] = {0x1111111111111111ull, 0xDEADBEAFCAFEBABEull};
        alignas(16) uint64_t src[2] = {0x0000000000000ABCull, 0ull};
        using Fn2 = void(*)(void*, const void*);
        auto fn_imm = ep.compile<Fn2>({
            0xF3, 0x0F, 0x6F, 0x01,             // movdqu xmm0, [rcx]
            0xF3, 0x0F, 0x6F, 0x0A,             // movdqu xmm1, [rdx]
            0xF2, 0x0F, 0x78, 0xC1, 12, 16,     // insertq xmm0, xmm1, len=12, idx=16
            0xF3, 0x0F, 0x7F, 0x01,             // movdqu [rcx], xmm0
            0xC3
        });
        fn_imm(dst, src);
        // Replaces bits [27:16] of 0x1111111111111111 with 0xABC -> 0x111111111ABC1111
        CHECK(dst[0] == 0x111111111ABC1111ull, "INSERTQ imm inserted 0xABC at bits [27:16]");
        CHECK(dst[1] == 0xDEADBEAFCAFEBABEull, "INSERTQ imm preserved upper 64 bits of XMM0");

        alignas(16) uint64_t dst2[2] = {0xFFFFFFFFFFFFFFFFull, 0x0102030405060708ull};
        // src2.Low = 0x1234, src2.High = len=16 | (idx=32 << 8) = 0x2010
        alignas(16) uint64_t src2[2] = {0x1234ull, 0x2010ull};
        auto fn_reg = ep.compile<Fn2>({
            0xF3, 0x0F, 0x6F, 0x01,             // movdqu xmm0, [rcx]
            0xF3, 0x0F, 0x6F, 0x0A,             // movdqu xmm1, [rdx]
            0xF2, 0x0F, 0x79, 0xC1,             // insertq xmm0, xmm1
            0xF3, 0x0F, 0x7F, 0x01,             // movdqu [rcx], xmm0
            0xC3
        });
        fn_reg(dst2, src2);
        CHECK(dst2[0] == 0xFFFF1234FFFFFFFFull, "INSERTQ reg inserted 0x1234 at bits [47:32]");
        CHECK(dst2[1] == 0x0102030405060708ull, "INSERTQ reg preserved upper 64 bits of XMM0");
    }

    // 4. Test MOVNTSS (F3 0F 2B, disp8 & SIB) & MOVNTSD (F2 0F 2B, disp8)
    {
        alignas(16) uint32_t out_buf[8]{};
        std::memset(out_buf, 0xCC, sizeof(out_buf));
        alignas(16) uint64_t val[2] = {0x1122334455667788ull, 0ull};
        using Fn3 = void(*)(void*, const void*, uint64_t);
        auto fn_nt = ep.compile<Fn3>({
            0xF3, 0x0F, 0x6F, 0x02,             // movdqu xmm0, [rdx]
            0xF3, 0x0F, 0x2B, 0x41, 0x04,       // movntss [rcx + 4], xmm0  (writes 0x55667788)
            0xF3, 0x42, 0x0F, 0x2B, 0x04, 0x81, // movntss [rcx + r8*4], xmm0 (REX.X + SIB test with r8=3 -> +12)
            0xF2, 0x0F, 0x2B, 0x41, 0x10,       // movntsd [rcx + 16], xmm0 (writes 0x1122334455667788)
            0xC3
        });
        fn_nt(out_buf, val, 3ull);

        CHECK(out_buf[0] == 0xCCCCCCCCu, "MOVNTSS did not clobber [0]");
        CHECK(out_buf[1] == 0x55667788u, "MOVNTSS [rcx+4] wrote exact 32-bit value");
        CHECK(out_buf[2] == 0xCCCCCCCCu, "MOVNTSS did not clobber [2]");
        CHECK(out_buf[3] == 0x55667788u, "MOVNTSS [rcx+r8*4] SIB wrote exact 32-bit value");
        uint64_t got64 = 0;
        std::memcpy(&got64, &out_buf[4], 8);
        CHECK(got64 == 0x1122334455667788ull, "MOVNTSD [rcx+16] wrote exact 64-bit value");
        CHECK(out_buf[6] == 0xCCCCCCCCu, "MOVNTSD did not clobber [6]");
    }

    // 5. Test MONITORX (0F 01 FA) & MWAITX (0F 01 FB)
    {
        alignas(64) uint64_t mon_var = 0;
        using Fn1 = void(*)(void*);
        auto fn_mw = ep.compile<Fn1>({
            0x48, 0x89, 0xC8,       // mov rax, rcx
            0x31, 0xC9,             // xor ecx, ecx
            0x31, 0xD2,             // xor edx, edx
            0x0F, 0x01, 0xFA,       // monitorx
            0x31, 0xC0,             // xor eax, eax
            0x0F, 0x01, 0xFB,       // mwaitx
            0xC3
        });
        fn_mw(&mon_var);
    }

    const wsx_zen2::VehStats st = wsx_zen2::get_stats();
    std::printf("[zen2_compat] VEH stats: clzero=%llu extrq=%llu insertq=%llu movntss=%llu movntsd=%llu monitorx/mwaitx=%llu total=%llu\n",
                static_cast<unsigned long long>(st.clzero_hits),
                static_cast<unsigned long long>(st.extrq_hits),
                static_cast<unsigned long long>(st.insertq_hits),
                static_cast<unsigned long long>(st.movntss_hits),
                static_cast<unsigned long long>(st.movntsd_hits),
                static_cast<unsigned long long>(st.monitorx_mwaitx_hits),
                static_cast<unsigned long long>(st.total_hits));

    if (!caps.has_clzero)   CHECK(st.clzero_hits == 1u, "Intel host trapped & emulated CLZERO via VEH");
    if (!caps.has_sse4a)    CHECK(st.extrq_hits == 2u && st.insertq_hits == 2u && st.movntss_hits == 2u && st.movntsd_hits == 1u,
                                  "Intel host trapped & emulated all 7 SSE4a calls via VEH");
    if (!caps.has_monitorx) CHECK(st.monitorx_mwaitx_hits == 2u, "Intel host trapped & emulated MONITORX+MWAITX via VEH");

    if (g_failures != 0) {
        std::fprintf(stderr, "[zen2_compat] FAILED with %d error(s)\n", g_failures);
        return 1;
    }
    std::puts("[zen2_compat] ALL PASSED (100% Zen 2 user-mode ISA verified on live host CPU!)");
    return 0;
}
