// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room In-Process PE32+ (x86-64) Manual Mapper for Xbox Series X|S (Scarlett) Executables:
// - Maps PE64 sections into memory, applies DIR64/HIGHLOW/HIGH/LOW base relocations, sets exact
//   per-section VirtualProtect permissions, and flushes the instruction cache.
// - Resolves named and ordinal IAT imports with shim-first priority (MizuBox\bin\*.dll ALWAYS
//   takes precedence over C:\Windows\System32\xgameruntime.dll / GameInput.dll).
// - Redirects guest LoadLibraryA/W, LoadLibraryExA/W, GetModuleHandleA/W, and GetProcAddress calls
//   so dynamically loaded Xbox DLLs also bind to MizuBox shims instead of System32.
// - Registers .pdata unwind tables via RtlAddFunctionTable (full SEH & C++ try/catch support).
// - Initializes IMAGE_DIRECTORY_ENTRY_TLS index and dispatches TLS callbacks (DLL_PROCESS_ATTACH/DETACH).
// - Optional x86-64 auto-stub thunk generator for unknown/unimplemented non-critical Xbox imports.

#pragma once

#include "wsx_zen2_compat.h"
#include "wsx_package_config.h"

#include <Windows.h>
#include <unknwn.h>
#include <Psapi.h>
#include <intrin.h>
#include <malloc.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace wsx_pe {

struct LoadOptions {
    std::filesystem::path shim_dir;
    std::filesystem::path package_dir;
    bool allow_auto_stubs = false;
    bool force_relocate = false; // Useful in tests to guarantee delta != 0
    bool trace_imports = false;
};

struct LoadStats {
    uint64_t preferred_base = 0;
    uint64_t mapped_base = 0;
    uint32_t image_size = 0;
    uint32_t reloc_count = 0;
    uint32_t imported_dlls = 0;
    uint32_t resolved_imports = 0;
    uint32_t delay_imports_resolved = 0;
    uint32_t ordinal_imports = 0;
    uint32_t shim_dll_hits = 0;
    uint32_t auto_stub_count = 0;
    uint32_t pdata_functions = 0;
    uint32_t tls_callbacks_run = 0;
    uint32_t guest_tls_slot = 0;
    uint32_t guest_tls_block_bytes = 0;
    uint32_t guest_atexit_run = 0;
    bool security_cookie_initialized = false;
};

namespace detail {

inline std::string to_lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

inline std::wstring to_lower_wide(std::wstring s) {
    for (wchar_t& c : s) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return s;
}

inline std::wstring utf8_to_wide(const char* s) {
    if (!s || !*s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

struct StubSlot {
    std::string dll_name;
    std::string sym_name;
    std::atomic<uint64_t> hits{0};
};

struct MappedImageRange {
    uintptr_t base = 0;
    size_t size = 0;
    std::wstring path;
};

struct GuestTlsModule {
    uintptr_t image_base = 0;
    uint32_t slot_index = 0;
    const uint8_t* raw_data = nullptr;
    size_t raw_size = 0;
    size_t zero_fill = 0;
    size_t alignment = 16;
    std::vector<PIMAGE_TLS_CALLBACK> callbacks;
};

struct ThreadTlsBlock {
    uintptr_t image_base = 0;
    uint32_t slot_index = 0;
    void* ptr = nullptr;
};

struct ThreadGuestTlsState {
    void** orig_teb_tls_ptr = nullptr;
    void* ext_alloc_base = nullptr; // Real ProcessHeap base (ext_table - kWsxTlsVectorHeaderBytes)
    void** ext_table = nullptr;     // Points to TLS_VECTOR::ModuleTlsData[0] (+16 bytes)
    size_t orig_copy_slots = 0;
    std::vector<ThreadTlsBlock> blocks;
};

struct GuestAtExitEntry {
    uintptr_t owner_base = 0;
    void* table_tag = nullptr;
    int(__cdecl* fn_ret_int)() = nullptr;
    void(__cdecl* fn_void)() = nullptr;
};

// Windows 10/11 x64 ntdll!LdrpAllocateTls / LdrpHandleTlsData TLS_VECTOR layout:
//   [alloc_base + 0x00]: uint32_t Length (total ModuleTlsData slots), uint32_t Reserved
//   [alloc_base + 0x08]: void*    PreviousDeferredTlsVector
//   [alloc_base + 0x10]: void*    ModuleTlsData[Length]  <-- TEB->ThreadLocalStoragePointer (gs:[0x58])
constexpr size_t kWsxTlsVectorHeaderBytes = 16u;
constexpr size_t kWsxTlsTableSlots = 128u;
constexpr uint32_t kWsxGuestTlsSlotBase = 64u;

struct LoaderContext {
    std::mutex mtx;
    std::filesystem::path shim_dir;
    std::filesystem::path package_dir;
    std::unordered_map<std::wstring, HMODULE> loaded_by_lower_name;
    std::vector<MappedImageRange> mapped_images;
    std::vector<std::unique_ptr<StubSlot>> stubs;
    std::vector<GuestTlsModule> guest_tls_modules;
    std::unordered_map<DWORD, std::unique_ptr<ThreadGuestTlsState>> thread_tls_states;
    std::vector<GuestAtExitEntry> guest_atexit_entries;
    uint64_t guest_tls_slot_bitmap = 0; // 64 guest slots [64..127]
};

inline LoaderContext& global_ctx() noexcept {
    static LoaderContext ctx;
    return ctx;
}

inline uint64_t __cdecl auto_stub_dispatch(uint64_t slot_idx) noexcept {
    auto& ctx = global_ctx();
    StubSlot* slot = nullptr;
    {
        std::lock_guard<std::mutex> lk(ctx.mtx);
        if (slot_idx < ctx.stubs.size()) slot = ctx.stubs[static_cast<size_t>(slot_idx)].get();
    }
    if (slot) {
        const uint64_t prev = slot->hits.fetch_add(1, std::memory_order_relaxed);
        if (prev == 0) {
            std::fprintf(stderr, "[wsx_pe] AUTO-STUB invoked: %s!%s -> returning 0\n",
                         slot->dll_name.c_str(), slot->sym_name.c_str());
        }
    }
    return 0ull;
}

inline std::wstring base_filename_lower(const wchar_t* name) {
    if (!name || !*name) return {};
    std::filesystem::path p(name);
    std::wstring fn = to_lower_wide(p.filename().wstring());
    if (fn.find(L'.') == std::wstring::npos) fn += L".dll";
    return fn;
}

// Resolves a DLL with strict shim-first precedence over System32:
//   1) shim_dir / <filename>     (MizuBox\bin\*.dll — beats System32\xgameruntime.dll & GameInput.dll!)
//   2) package_dir / <filename>  (title-bundled DLLs such as Bink2Scarlett.dll, XCurl.dll, ...)
//   3) standard LoadLibraryExW   (Windows OS DLLs & API-sets)
inline HMODULE resolve_dll_shim_first(const wchar_t* raw_name, DWORD flags = 0, bool* out_was_shim = nullptr) noexcept {
    if (out_was_shim) *out_was_shim = false;
    if (!raw_name || !*raw_name) return nullptr;

    const std::wstring fn_low = base_filename_lower(raw_name);
    std::filesystem::path shim_dir;
    std::filesystem::path pkg_dir;
    {
        auto& ctx = global_ctx();
        std::lock_guard<std::mutex> lk(ctx.mtx);
        auto it = ctx.loaded_by_lower_name.find(fn_low);
        if (it != ctx.loaded_by_lower_name.end() && it->second != nullptr) {
            if (out_was_shim) {
                std::error_code ec;
                if (!ctx.shim_dir.empty() && std::filesystem::is_regular_file(ctx.shim_dir / fn_low, ec)) {
                    *out_was_shim = true;
                }
            }
            return it->second;
        }
        shim_dir = ctx.shim_dir;
        pkg_dir = ctx.package_dir;
    }

    auto try_load_full = [&](const std::filesystem::path& p, bool is_shim) -> HMODULE {
        std::error_code ec;
        if (p.empty() || !std::filesystem::is_regular_file(p, ec)) return nullptr;
        HMODULE hm = LoadLibraryExW(p.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!hm) hm = LoadLibraryW(p.c_str());
        if (hm) {
            auto& ctx = global_ctx();
            std::lock_guard<std::mutex> lk(ctx.mtx);
            ctx.loaded_by_lower_name[fn_low] = hm;
            if (out_was_shim) *out_was_shim = is_shim;
        }
        return hm;
    };

    if (!shim_dir.empty()) {
        if (HMODULE hm = try_load_full(shim_dir / fn_low, true)) return hm;
    }
    if (!pkg_dir.empty()) {
        if (HMODULE hm = try_load_full(pkg_dir / fn_low, false)) return hm;
    }

    HMODULE hm = flags ? LoadLibraryExW(raw_name, nullptr, flags) : LoadLibraryW(raw_name);
    if (hm) {
        auto& ctx = global_ctx();
        std::lock_guard<std::mutex> lk(ctx.mtx);
        ctx.loaded_by_lower_name[fn_low] = hm;
    }
    return hm;
}

inline void refresh_guest_tls_after_loadlibrary() noexcept;

inline HMODULE WINAPI hook_LoadLibraryW(LPCWSTR lpLibFileName) noexcept {
    HMODULE hm = resolve_dll_shim_first(lpLibFileName, 0);
    refresh_guest_tls_after_loadlibrary();
    return hm;
}

inline HMODULE WINAPI hook_LoadLibraryA(LPCSTR lpLibFileName) noexcept {
    if (!lpLibFileName) return nullptr;
    const std::wstring w = utf8_to_wide(lpLibFileName);
    HMODULE hm = resolve_dll_shim_first(w.c_str(), 0);
    refresh_guest_tls_after_loadlibrary();
    return hm;
}

inline HMODULE WINAPI hook_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) noexcept {
    HMODULE hm = hFile ? LoadLibraryExW(lpLibFileName, hFile, dwFlags)
                       : resolve_dll_shim_first(lpLibFileName, dwFlags);
    refresh_guest_tls_after_loadlibrary();
    return hm;
}

inline HMODULE WINAPI hook_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) noexcept {
    if (!lpLibFileName || hFile) {
        HMODULE hm = LoadLibraryExA(lpLibFileName, hFile, dwFlags);
        refresh_guest_tls_after_loadlibrary();
        return hm;
    }
    const std::wstring w = utf8_to_wide(lpLibFileName);
    HMODULE hm = resolve_dll_shim_first(w.c_str(), dwFlags);
    refresh_guest_tls_after_loadlibrary();
    return hm;
}

inline HMODULE WINAPI hook_GetModuleHandleW(LPCWSTR lpModuleName) noexcept {
    if (!lpModuleName) return GetModuleHandleW(nullptr);
    const std::wstring fn_low = base_filename_lower(lpModuleName);
    {
        auto& ctx = global_ctx();
        std::lock_guard<std::mutex> lk(ctx.mtx);
        auto it = ctx.loaded_by_lower_name.find(fn_low);
        if (it != ctx.loaded_by_lower_name.end() && it->second) return it->second;
    }
    return GetModuleHandleW(lpModuleName);
}

inline HMODULE WINAPI hook_GetModuleHandleA(LPCSTR lpModuleName) noexcept {
    if (!lpModuleName) return GetModuleHandleA(nullptr);
    const std::wstring w = utf8_to_wide(lpModuleName);
    return hook_GetModuleHandleW(w.c_str());
}

inline FARPROC WINAPI hook_GetProcAddress(HMODULE hModule, LPCSTR lpProcName) noexcept {
    return GetProcAddress(hModule, lpProcName);
}

inline PVOID WINAPI hook_RtlPcToFileHeader(PVOID PcValue, PVOID* BaseOfImage) noexcept {
    const uintptr_t pc = reinterpret_cast<uintptr_t>(PcValue);
    {
        auto& ctx = global_ctx();
        std::lock_guard<std::mutex> lk(ctx.mtx);
        for (const auto& img : ctx.mapped_images) {
            if (pc >= img.base && (pc - img.base) < img.size) {
                void* b = reinterpret_cast<void*>(img.base);
                if (BaseOfImage) *BaseOfImage = b;
                return b;
            }
        }
    }
    return RtlPcToFileHeader(PcValue, BaseOfImage);
}

inline BOOL WINAPI hook_GetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule) noexcept {
    if (!phModule) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if ((dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) != 0) {
        const uintptr_t pc = reinterpret_cast<uintptr_t>(lpModuleName);
        auto& ctx = global_ctx();
        std::lock_guard<std::mutex> lk(ctx.mtx);
        for (const auto& img : ctx.mapped_images) {
            if (pc >= img.base && (pc - img.base) < img.size) {
                *phModule = reinterpret_cast<HMODULE>(img.base);
                return TRUE;
            }
        }
    } else if (lpModuleName != nullptr) {
        const std::wstring fn_low = base_filename_lower(lpModuleName);
        auto& ctx = global_ctx();
        std::lock_guard<std::mutex> lk(ctx.mtx);
        auto it = ctx.loaded_by_lower_name.find(fn_low);
        if (it != ctx.loaded_by_lower_name.end() && it->second) {
            *phModule = it->second;
            return TRUE;
        }
    }
    return GetModuleHandleExW(dwFlags, lpModuleName, phModule);
}

inline BOOL WINAPI hook_GetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule) noexcept {
    if ((dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) != 0) {
        return hook_GetModuleHandleExW(dwFlags, reinterpret_cast<LPCWSTR>(lpModuleName), phModule);
    }
    if (!lpModuleName) return GetModuleHandleExA(dwFlags, lpModuleName, phModule);
    const std::wstring w = utf8_to_wide(lpModuleName);
    return hook_GetModuleHandleExW(dwFlags, w.c_str(), phModule);
}

inline DWORD WINAPI hook_GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize) noexcept {
    if (hModule && lpFilename && nSize > 0) {
        const uintptr_t mod_addr = reinterpret_cast<uintptr_t>(hModule);
        auto& ctx = global_ctx();
        std::lock_guard<std::mutex> lk(ctx.mtx);
        for (const auto& img : ctx.mapped_images) {
            if (img.base == mod_addr && !img.path.empty()) {
                const DWORD copy_len = static_cast<DWORD>(std::min<size_t>(img.path.size(), nSize - 1u));
                std::memcpy(lpFilename, img.path.data(), copy_len * sizeof(wchar_t));
                lpFilename[copy_len] = L'\0';
                return copy_len;
            }
        }
    }
    return GetModuleFileNameW(hModule, lpFilename, nSize);
}

inline uintptr_t find_owning_image_base_locked(const LoaderContext& ctx, uintptr_t addr) noexcept {
    for (const auto& img : ctx.mapped_images) {
        if (addr >= img.base && (addr - img.base) < img.size) {
            return img.base;
        }
    }
    return 0;
}

inline uint32_t alloc_guest_tls_slot_locked(LoaderContext& ctx) noexcept {
    for (uint32_t bit = 0; bit < 64u; ++bit) {
        const uint64_t mask = 1ull << bit;
        if ((ctx.guest_tls_slot_bitmap & mask) == 0ull) {
            ctx.guest_tls_slot_bitmap |= mask;
            return kWsxGuestTlsSlotBase + bit;
        }
    }
    return TLS_OUT_OF_INDEXES;
}

inline void free_guest_tls_slot_locked(LoaderContext& ctx, uint32_t slot) noexcept {
    if (slot >= kWsxGuestTlsSlotBase && slot < kWsxGuestTlsSlotBase + 64u) {
        ctx.guest_tls_slot_bitmap &= ~(1ull << (slot - kWsxGuestTlsSlotBase));
    }
}

// Compute exact count of OS-loaded static TLS slots by scanning loaded modules' IMAGE_DIRECTORY_ENTRY_TLS
// (avoids both unsafe HeapSize() probes and copying past the end of ntdll's ThreadLocalStoragePointer array!).
inline size_t count_os_tls_slots() noexcept {
    HMODULE mods[1024]{};
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        return 0;
    }
    const size_t nmods = std::min<size_t>(needed / sizeof(HMODULE), std::size(mods));
    size_t max_slots = 0;
    for (size_t i = 0; i < nmods; ++i) {
        const auto* base = reinterpret_cast<const uint8_t*>(mods[i]);
        if (!base) continue;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) continue;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) continue;
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        if (dir.VirtualAddress == 0 || dir.Size < sizeof(IMAGE_TLS_DIRECTORY64)) continue;
        const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(base + dir.VirtualAddress);
        if (tls->AddressOfIndex == 0) continue;
        const DWORD idx = *reinterpret_cast<const DWORD*>(tls->AddressOfIndex);
        if (idx < kWsxGuestTlsSlotBase) {
            max_slots = std::max<size_t>(max_slots, static_cast<size_t>(idx) + 1u);
        }
    }
    return max_slots;
}

inline void* alloc_tls_vector_table(void*** out_module_data) noexcept {
    if (!out_module_data) return nullptr;
    *out_module_data = nullptr;
    void* raw_alloc = HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY,
        kWsxTlsVectorHeaderBytes + kWsxTlsTableSlots * sizeof(void*));
    if (!raw_alloc) return nullptr;
    auto* hdr = static_cast<uint64_t*>(raw_alloc);
    hdr[0] = static_cast<uint64_t>(kWsxTlsTableSlots); // TLS_VECTOR::Length = 128
    hdr[1] = 0ull;                                     // TLS_VECTOR::PreviousDeferredTlsVector = nullptr
    *out_module_data = reinterpret_cast<void**>(
        static_cast<uint8_t*>(raw_alloc) + kWsxTlsVectorHeaderBytes);
    return raw_alloc;
}

inline size_t detect_os_tls_slot_count(void** os_tbl) noexcept {
    size_t slots = count_os_tls_slots();
    if (os_tbl) {
        const auto* base16 = reinterpret_cast<const uint8_t*>(os_tbl) - kWsxTlsVectorHeaderBytes;
        if (HeapValidate(GetProcessHeap(), 0, base16)) {
            const SIZE_T blk_sz = HeapSize(GetProcessHeap(), 0, base16);
            if (blk_sz != static_cast<SIZE_T>(-1) && blk_sz > kWsxTlsVectorHeaderBytes) {
                const size_t cap_slots = (blk_sz - kWsxTlsVectorHeaderBytes) / sizeof(void*);
                const uint32_t hdr_len = *reinterpret_cast<const uint32_t*>(base16);
                if (hdr_len > 0u && hdr_len <= cap_slots) {
                    slots = std::max<size_t>(slots, hdr_len);
                }
                slots = std::min<size_t>(slots, cap_slots);
            }
        }
    }
    return std::min<size_t>(slots, kWsxGuestTlsSlotBase);
}

inline ThreadGuestTlsState* ensure_thread_tls_table_locked(LoaderContext& ctx) noexcept {
    const DWORD tid = GetCurrentThreadId();
    auto it = ctx.thread_tls_states.find(tid);
    if (it != ctx.thread_tls_states.end() && it->second) {
        return it->second.get();
    }
    auto st = std::make_unique<ThreadGuestTlsState>();
    void** orig_tbl = reinterpret_cast<void**>(__readgsqword(0x58));
    void** ext_tbl = nullptr;
    void* raw_alloc = alloc_tls_vector_table(&ext_tbl);
    if (!raw_alloc || !ext_tbl) return nullptr;

    size_t copy_slots = 0;
    if (orig_tbl) {
        copy_slots = detect_os_tls_slot_count(orig_tbl);
        if (copy_slots > 0) {
            std::memcpy(ext_tbl, orig_tbl, copy_slots * sizeof(void*));
        }
    }
    st->orig_teb_tls_ptr = orig_tbl;
    st->ext_alloc_base = raw_alloc;
    st->ext_table = ext_tbl;
    st->orig_copy_slots = copy_slots;
    __writegsqword(0x58, reinterpret_cast<uint64_t>(ext_tbl));

    ThreadGuestTlsState* raw = st.get();
    ctx.thread_tls_states[tid] = std::move(st);
    return raw;
}

inline bool attach_guest_tls_module_on_current_thread_locked(
    LoaderContext& ctx, const GuestTlsModule& mod) noexcept {
    if (mod.slot_index >= kWsxTlsTableSlots) return false;
    ThreadGuestTlsState* st = ensure_thread_tls_table_locked(ctx);
    if (!st || !st->ext_table) return false;

    for (const auto& existing : st->blocks) {
        if (existing.image_base == mod.image_base && existing.slot_index == mod.slot_index) {
            return true;
        }
    }

    const size_t total_bytes = std::max<size_t>(mod.raw_size + mod.zero_fill, 16u);
    const size_t align = std::clamp<size_t>(mod.alignment, 16u, 4096u);
    void* blk = _aligned_malloc(total_bytes, align);
    if (!blk) return false;
    if (mod.raw_size > 0 && mod.raw_data) {
        std::memcpy(blk, mod.raw_data, mod.raw_size);
    }
    if (total_bytes > mod.raw_size) {
        std::memset(static_cast<uint8_t*>(blk) + mod.raw_size, 0, total_bytes - mod.raw_size);
    }
    st->ext_table[mod.slot_index] = blk;
    st->blocks.push_back(ThreadTlsBlock{mod.image_base, mod.slot_index, blk});
    return true;
}

inline void detach_guest_tls_on_current_thread(uintptr_t only_image_base = 0) noexcept {
    auto& ctx = global_ctx();
    std::lock_guard<std::mutex> lk(ctx.mtx);
    const DWORD tid = GetCurrentThreadId();
    auto it = ctx.thread_tls_states.find(tid);
    if (it == ctx.thread_tls_states.end() || !it->second) return;
    ThreadGuestTlsState* st = it->second.get();
    void** cur_teb_tbl = reinterpret_cast<void**>(__readgsqword(0x58));
    const bool teb_still_ours = (cur_teb_tbl == st->ext_table);

    auto bit = st->blocks.begin();
    while (bit != st->blocks.end()) {
        if (only_image_base == 0 || bit->image_base == only_image_base) {
            if (teb_still_ours && st->ext_table && bit->slot_index < kWsxTlsTableSlots) {
                st->ext_table[bit->slot_index] = nullptr;
            }
            if (bit->ptr) {
                _aligned_free(bit->ptr);
            }
            bit = st->blocks.erase(bit);
        } else {
            ++bit;
        }
    }
    if (st->blocks.empty()) {
        // Restore orig_teb_tls_ptr ONLY if ntdll!LdrpHandleTlsData neither replaced gs:[0x58] nor
        // mutated any OS TLS slots [0..63] inside st->ext_table during guest LoadLibrary calls!
        if (teb_still_ours && st->ext_table) {
            bool os_slots_mutated = false;
            if (st->orig_teb_tls_ptr && st->orig_copy_slots > 0) {
                if (std::memcmp(st->ext_table, st->orig_teb_tls_ptr, st->orig_copy_slots * sizeof(void*)) != 0) {
                    os_slots_mutated = true;
                }
            }
            for (size_t s = st->orig_copy_slots; s < kWsxGuestTlsSlotBase && !os_slots_mutated; ++s) {
                if (st->ext_table[s] != nullptr) os_slots_mutated = true;
            }
            if (!os_slots_mutated) {
                __writegsqword(0x58, reinterpret_cast<uint64_t>(st->orig_teb_tls_ptr));
                if (st->ext_alloc_base) {
                    HeapFree(GetProcessHeap(), 0, st->ext_alloc_base);
                }
            }
            // If OS mutated slots [0..63] in-place, leave st->ext_table (backed by a valid
            // ProcessHeap TLS_VECTOR starting at st->ext_alloc_base = ext_table - 16) live at
            // gs:[0x58] so ntdll!LdrShutdownThread cleanly owns and frees it at thread exit.
        }
        st->ext_alloc_base = nullptr;
        st->ext_table = nullptr;
        ctx.thread_tls_states.erase(it);
    }
}

// If a dynamic LoadLibrary triggered ntdll!LdrpHandleTlsData and replaced gs:[0x58],
// re-mirror the new OS TLS table and re-install all active guest static TLS slots [64..127].
inline void refresh_guest_tls_after_loadlibrary() noexcept {
    auto& ctx = global_ctx();
    std::lock_guard<std::mutex> lk(ctx.mtx);
    const DWORD tid = GetCurrentThreadId();
    auto it = ctx.thread_tls_states.find(tid);
    if (it == ctx.thread_tls_states.end() || !it->second || it->second->blocks.empty()) return;
    ThreadGuestTlsState* st = it->second.get();
    void** cur_teb_tbl = reinterpret_cast<void**>(__readgsqword(0x58));
    if (cur_teb_tbl == st->ext_table) return;

    void** ext_tbl = nullptr;
    void* raw_alloc = alloc_tls_vector_table(&ext_tbl);
    if (!raw_alloc || !ext_tbl) return;
    size_t copy_slots = 0;
    if (cur_teb_tbl) {
        copy_slots = detect_os_tls_slot_count(cur_teb_tbl);
        if (copy_slots > 0) std::memcpy(ext_tbl, cur_teb_tbl, copy_slots * sizeof(void*));
    }
    for (const auto& blk : st->blocks) {
        if (blk.slot_index < kWsxTlsTableSlots) {
            ext_tbl[blk.slot_index] = blk.ptr;
        }
    }
    st->orig_teb_tls_ptr = cur_teb_tbl;
    st->ext_alloc_base = raw_alloc;
    st->ext_table = ext_tbl;
    st->orig_copy_slots = copy_slots;
    __writegsqword(0x58, reinterpret_cast<uint64_t>(ext_tbl));
}

struct GuestThreadTrampolineArgs {
    LPTHREAD_START_ROUTINE orig_start = nullptr;
    LPVOID orig_param = nullptr;
};

inline void run_guest_thread_tls_detach_callbacks() noexcept {
    std::vector<std::pair<void*, PIMAGE_TLS_CALLBACK>> cbs;
    {
        auto& ctx = global_ctx();
        std::lock_guard<std::mutex> lk(ctx.mtx);
        for (auto mit = ctx.guest_tls_modules.rbegin(); mit != ctx.guest_tls_modules.rend(); ++mit) {
            for (auto cit = mit->callbacks.rbegin(); cit != mit->callbacks.rend(); ++cit) {
                cbs.emplace_back(reinterpret_cast<void*>(mit->image_base), *cit);
            }
        }
    }
    for (const auto& pair : cbs) {
        pair.second(pair.first, DLL_THREAD_DETACH, nullptr);
    }
    detach_guest_tls_on_current_thread(0);
}

inline DWORD WINAPI guest_thread_trampoline(LPVOID raw_args) noexcept {
    std::unique_ptr<GuestThreadTrampolineArgs> args(static_cast<GuestThreadTrampolineArgs*>(raw_args));
    if (!args || !args->orig_start) return 0;

    std::vector<std::pair<void*, PIMAGE_TLS_CALLBACK>> attach_cbs;
    {
        auto& ctx = global_ctx();
        std::lock_guard<std::mutex> lk(ctx.mtx);
        for (const auto& mod : ctx.guest_tls_modules) {
            attach_guest_tls_module_on_current_thread_locked(ctx, mod);
            for (PIMAGE_TLS_CALLBACK cb : mod.callbacks) {
                attach_cbs.emplace_back(reinterpret_cast<void*>(mod.image_base), cb);
            }
        }
    }
    for (const auto& pair : attach_cbs) {
        pair.second(pair.first, DLL_THREAD_ATTACH, nullptr);
    }

    const DWORD rc = args->orig_start(args->orig_param);
    run_guest_thread_tls_detach_callbacks();
    return rc;
}

inline HANDLE WINAPI hook_CreateThread(
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    SIZE_T dwStackSize,
    LPTHREAD_START_ROUTINE lpStartAddress,
    LPVOID lpParameter,
    DWORD dwCreationFlags,
    LPDWORD lpThreadId) noexcept {
    if (!lpStartAddress) {
        return CreateThread(lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, lpThreadId);
    }
    auto* args = new (std::nothrow) GuestThreadTrampolineArgs{lpStartAddress, lpParameter};
    if (!args) {
        SetLastError(ERROR_OUTOFMEMORY);
        return nullptr;
    }
    HANDLE h = CreateThread(
        lpThreadAttributes, dwStackSize, &guest_thread_trampoline, args, dwCreationFlags, lpThreadId);
    if (!h) {
        delete args;
    }
    return h;
}

inline VOID WINAPI hook_ExitThread(DWORD dwExitCode) noexcept {
    run_guest_thread_tls_detach_callbacks();
    ExitThread(dwExitCode);
}

inline int __cdecl hook_guest_atexit(void(__cdecl* fn)()) noexcept {
    if (!fn) return -1;
    auto& ctx = global_ctx();
    std::lock_guard<std::mutex> lk(ctx.mtx);
    const uintptr_t owner = find_owning_image_base_locked(ctx, reinterpret_cast<uintptr_t>(fn));
    ctx.guest_atexit_entries.push_back(GuestAtExitEntry{owner, nullptr, nullptr, fn});
    return 0;
}

using GuestOnExitFn = int(__cdecl*)();

inline GuestOnExitFn __cdecl hook_guest_onexit(GuestOnExitFn fn) noexcept {
    if (!fn) return nullptr;
    auto& ctx = global_ctx();
    std::lock_guard<std::mutex> lk(ctx.mtx);
    const uintptr_t owner = find_owning_image_base_locked(ctx, reinterpret_cast<uintptr_t>(fn));
    ctx.guest_atexit_entries.push_back(GuestAtExitEntry{owner, nullptr, fn, nullptr});
    return fn;
}

inline int __cdecl hook_guest_register_onexit_function(void* table, GuestOnExitFn fn) noexcept {
    if (!fn) return -1;
    auto& ctx = global_ctx();
    std::lock_guard<std::mutex> lk(ctx.mtx);
    uintptr_t owner = find_owning_image_base_locked(ctx, reinterpret_cast<uintptr_t>(fn));
    if (owner == 0 && table) {
        owner = find_owning_image_base_locked(ctx, reinterpret_cast<uintptr_t>(table));
    }
    ctx.guest_atexit_entries.push_back(GuestAtExitEntry{owner, table, fn, nullptr});
    return 0;
}

inline uint32_t flush_guest_atexit_entries(uintptr_t owner_base, void* table_tag = nullptr) noexcept {
    std::vector<GuestAtExitEntry> to_run;
    {
        auto& ctx = global_ctx();
        std::lock_guard<std::mutex> lk(ctx.mtx);
        auto it = ctx.guest_atexit_entries.begin();
        while (it != ctx.guest_atexit_entries.end()) {
            const bool match = table_tag
                ? (it->table_tag == table_tag)
                : (owner_base != 0 && it->owner_base == owner_base);
            if (match) {
                to_run.push_back(*it);
                it = ctx.guest_atexit_entries.erase(it);
            } else {
                ++it;
            }
        }
    }
    uint32_t count = 0;
    for (auto it = to_run.rbegin(); it != to_run.rend(); ++it) {
        if (it->fn_void) {
            it->fn_void();
            ++count;
        } else if (it->fn_ret_int) {
            it->fn_ret_int();
            ++count;
        }
    }
    return count;
}

inline int __cdecl hook_guest_execute_onexit_table(void* table) noexcept {
    if (table) {
        flush_guest_atexit_entries(0, table);
    }
    return 0;
}

// ============================================================================
// Wave 11.1: AAA PE64 Compatibility Hooks (CPU Affinity Remap, AppModel IDs,
//            Guest CommandLine, and WinRT RoGetActivationFactory Bridge)
// ============================================================================
struct CompatHookStats {
    std::atomic<uint64_t> affinity_remaps{0};
    std::atomic<uint64_t> ideal_proc_remaps{0};
    std::atomic<uint64_t> appmodel_queries{0};
    std::atomic<uint64_t> winrt_fallback_hits{0};
};

inline CompatHookStats& compat_hook_stats() noexcept {
    static CompatHookStats s;
    return s;
}

inline std::mutex& guest_cmdline_mtx() noexcept {
    static std::mutex m;
    return m;
}
inline std::wstring& guest_cmdline_w() noexcept {
    static std::wstring w;
    return w;
}
inline std::string& guest_cmdline_a() noexcept {
    static std::string a;
    return a;
}

inline DWORD_PTR remap_affinity_mask_to_host(DWORD_PTR requested) noexcept {
    if (requested == 0u) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0u;
    }
    DWORD_PTR proc_mask = 0, sys_mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask) || proc_mask == 0) {
        return requested;
    }
    // Fold any out-of-range Scarlett / high core bits (e.g., cores 12..15 on 6c/12t host) into valid host bits
    std::vector<DWORD_PTR> valid_bits;
    for (unsigned b = 0; b < 64u; ++b) {
        const DWORD_PTR bit = (DWORD_PTR(1u) << b);
        if (proc_mask & bit) valid_bits.push_back(bit);
    }
    if (valid_bits.empty()) return proc_mask;
    DWORD_PTR remapped = 0;
    for (unsigned b = 0; b < 64u; ++b) {
        if (requested & (DWORD_PTR(1u) << b)) {
            remapped |= valid_bits[b % valid_bits.size()];
        }
    }
    if ((requested & ~proc_mask) != 0u) {
        compat_hook_stats().affinity_remaps.fetch_add(1, std::memory_order_relaxed);
    }
    return remapped ? remapped : valid_bits[0];
}

inline DWORD_PTR WINAPI hook_SetThreadAffinityMask(HANDLE hThread, DWORD_PTR dwThreadAffinityMask) noexcept {
    const DWORD_PTR safe_mask = remap_affinity_mask_to_host(dwThreadAffinityMask);
    if (safe_mask == 0u) {
        return 0u;
    }
    return SetThreadAffinityMask(hThread, safe_mask);
}

inline BOOL WINAPI hook_GetProcessAffinityMask(
    HANDLE hProcess, PDWORD_PTR lpProcessAffinityMask, PDWORD_PTR lpSystemAffinityMask) noexcept {
    DWORD_PTR proc_m = 0u, sys_m = 0u;
    const BOOL ok = GetProcessAffinityMask(hProcess, &proc_m, &sys_m);
    if (!ok) return FALSE;
    if (hProcess == GetCurrentProcess() || GetProcessId(hProcess) == GetCurrentProcessId()) {
        proc_m |= static_cast<DWORD_PTR>(0xFFFFULL);
        sys_m  |= static_cast<DWORD_PTR>(0xFFFFULL);
    }
    if (lpProcessAffinityMask) *lpProcessAffinityMask = proc_m;
    if (lpSystemAffinityMask)  *lpSystemAffinityMask  = sys_m;
    return TRUE;
}

inline DWORD WINAPI hook_SetThreadIdealProcessor(HANDLE hThread, DWORD dwIdealProcessor) noexcept {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const DWORD ncpu = std::max<DWORD>(1u, si.dwNumberOfProcessors);
    DWORD safe = dwIdealProcessor;
    if (dwIdealProcessor != MAXIMUM_PROCESSORS && dwIdealProcessor >= ncpu) {
        safe = dwIdealProcessor % ncpu;
        compat_hook_stats().ideal_proc_remaps.fetch_add(1, std::memory_order_relaxed);
    }
    return SetThreadIdealProcessor(hThread, safe);
}

inline BOOL WINAPI hook_SetThreadIdealProcessorEx(
    HANDLE hThread, PPROCESSOR_NUMBER lpIdealProcessor, PPROCESSOR_NUMBER lpPreviousIdealProcessor) noexcept {
    if (!lpIdealProcessor) return SetThreadIdealProcessorEx(hThread, lpIdealProcessor, lpPreviousIdealProcessor);
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const DWORD ncpu = std::max<DWORD>(1u, si.dwNumberOfProcessors);
    PROCESSOR_NUMBER safe = *lpIdealProcessor;
    if (safe.Number >= ncpu) {
        safe.Group = 0;
        safe.Number = static_cast<BYTE>(safe.Number % ncpu);
        compat_hook_stats().ideal_proc_remaps.fetch_add(1, std::memory_order_relaxed);
    }
    return SetThreadIdealProcessorEx(hThread, &safe, lpPreviousIdealProcessor);
}

inline LONG copy_appmodel_string(const wchar_t* src, UINT32* length, PWSTR buf) noexcept {
    compat_hook_stats().appmodel_queries.fetch_add(1, std::memory_order_relaxed);
    if (!length) return ERROR_INVALID_PARAMETER;
    const UINT32 needed = static_cast<UINT32>(std::wcslen(src) + 1u);
    if (!buf || *length < needed) {
        *length = needed;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    std::memcpy(buf, src, needed * sizeof(wchar_t));
    *length = needed;
    return ERROR_SUCCESS;
}

inline LONG WINAPI hook_GetCurrentPackageFullName(UINT32* packageFullNameLength, PWSTR packageFullName) noexcept {
    const auto& cfg = wsx_pkg::get_hydrated_config();
    return copy_appmodel_string(cfg.package_full_name_w.c_str(),
                                packageFullNameLength, packageFullName);
}

inline LONG WINAPI hook_GetCurrentPackageFamilyName(UINT32* packageFamilyNameLength, PWSTR packageFamilyName) noexcept {
    const auto& cfg = wsx_pkg::get_hydrated_config();
    return copy_appmodel_string(cfg.package_family_name_w.c_str(),
                                packageFamilyNameLength, packageFamilyName);
}

inline LONG WINAPI hook_GetCurrentApplicationUserModelId(UINT32* applicationUserModelIdLength, PWSTR applicationUserModelId) noexcept {
    const auto& cfg = wsx_pkg::get_hydrated_config();
    return copy_appmodel_string(cfg.aumid_w.c_str(),
                                applicationUserModelIdLength, applicationUserModelId);
}

inline LPWSTR WINAPI hook_GetCommandLineW() noexcept {
    std::lock_guard<std::mutex> lk(guest_cmdline_mtx());
    if (!guest_cmdline_w().empty()) {
        return guest_cmdline_w().data();
    }
    return GetCommandLineW();
}

inline LPSTR WINAPI hook_GetCommandLineA() noexcept {
    std::lock_guard<std::mutex> lk(guest_cmdline_mtx());
    if (!guest_cmdline_a().empty()) {
        return guest_cmdline_a().data();
    }
    return GetCommandLineA();
}

struct WsxWinRtFallbackFactory final : IUnknown {
    std::atomic<ULONG> ref{1};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        // Accept IUnknown / IInspectable / IActivationFactory queries cleanly
        static const GUID kIInspectable = {0xAF86E2E0, 0xB12D, 0x4c6a, {0x9C, 0x5A, 0xD7, 0xAA, 0x65, 0x10, 0x1E, 0x90}};
        static const GUID kIActFactory  = {0x00000035, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
        if (riid == __uuidof(IUnknown) || riid == kIInspectable || riid == kIActFactory) {
            *ppv = this;
            ref.fetch_add(1, std::memory_order_relaxed);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ref.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (r == 0) delete this;
        return r;
    }
    // IInspectable slots (3..5) + IActivationFactory::ActivateInstance (6)
    virtual HRESULT STDMETHODCALLTYPE GetIids(ULONG* c, IID** iids) {
        if (c) *c = 0;
        if (iids) *iids = nullptr;
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE GetRuntimeClassName(void** name) {
        if (name) *name = nullptr;
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(int* lvl) {
        if (lvl) *lvl = 0;
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE ActivateInstance(void** inst) {
        if (!inst) return E_POINTER;
        *inst = new (std::nothrow) WsxWinRtFallbackFactory();
        return *inst ? S_OK : E_OUTOFMEMORY;
    }
};

inline HRESULT WINAPI hook_RoGetActivationFactory(void* activatableClassId, REFIID iid, void** factory) noexcept {
    if (!factory) return E_POINTER;
    *factory = nullptr;
    using FnRealRo = HRESULT(WINAPI*)(void*, REFIID, void**);
    HMODULE combase = GetModuleHandleW(L"combase.dll");
    if (!combase) combase = LoadLibraryW(L"combase.dll");
    if (combase) {
        auto fn_real = reinterpret_cast<FnRealRo>(GetProcAddress(combase, "RoGetActivationFactory"));
        if (fn_real) {
            const HRESULT hr = fn_real(activatableClassId, iid, factory);
            if (SUCCEEDED(hr)) return hr;
        }
    }
    compat_hook_stats().winrt_fallback_hits.fetch_add(1, std::memory_order_relaxed);
    auto* stub = new (std::nothrow) WsxWinRtFallbackFactory();
    if (!stub) return E_OUTOFMEMORY;
    const HRESULT qhr = stub->QueryInterface(iid, factory);
    stub->Release();
    return qhr;
}

inline FARPROC maybe_hook_loader_import(const std::string& dll_low, const char* sym_name) noexcept {
    if (!sym_name) return nullptr;
    const bool is_crt_dll =
        dll_low == "ucrtbase.dll" ||
        dll_low == "msvcrt.dll" ||
        dll_low.rfind("api-ms-win-crt-runtime-", 0) == 0 ||
        dll_low.rfind("vcruntime", 0) == 0;
    if (is_crt_dll) {
        if (std::strcmp(sym_name, "atexit") == 0 || std::strcmp(sym_name, "_crt_atexit") == 0) {
            return reinterpret_cast<FARPROC>(&hook_guest_atexit);
        }
        if (std::strcmp(sym_name, "_onexit") == 0) {
            return reinterpret_cast<FARPROC>(&hook_guest_onexit);
        }
        if (std::strcmp(sym_name, "_register_onexit_function") == 0) {
            return reinterpret_cast<FARPROC>(&hook_guest_register_onexit_function);
        }
        if (std::strcmp(sym_name, "_execute_onexit_table") == 0) {
            return reinterpret_cast<FARPROC>(&hook_guest_execute_onexit_table);
        }
    }
    if (dll_low == "combase.dll" || dll_low.rfind("api-ms-win-core-winrt-", 0) == 0) {
        if (std::strcmp(sym_name, "RoGetActivationFactory") == 0) {
            return reinterpret_cast<FARPROC>(&hook_RoGetActivationFactory);
        }
    }
    if (dll_low == "mfplat.dll" && std::strcmp(sym_name, "MFResetDXGIDeviceManagerX") == 0) {
        if (HMODULE hd3d = resolve_dll_shim_first(L"d3d12_xs.dll", 0)) {
            if (FARPROC fp = GetProcAddress(hd3d, "MFResetDXGIDeviceManagerX")) return fp;
        }
    }
    if ((dll_low == "d3d12_x.dll" || dll_low == "d3d12_xs.dll") &&
        std::strcmp(sym_name, "D3DMapEsramMemory") == 0) {
        if (HMODULE hd3d = resolve_dll_shim_first(L"d3d12_xs.dll", 0)) {
            if (FARPROC fp = GetProcAddress(hd3d, "D3DMapEsramMemory")) return fp;
        }
    }
    const bool is_loader_dll =
        dll_low == "kernel32.dll" ||
        dll_low == "kernel32legacy.dll" ||
        dll_low == "kernelbase.dll" ||
        dll_low == "ntdll.dll" ||
        dll_low.rfind("api-ms-win-core-libraryloader-", 0) == 0 ||
        dll_low.rfind("api-ms-win-core-processthreads-", 0) == 0 ||
        dll_low.rfind("api-ms-win-core-processtopology-", 0) == 0 ||
        dll_low.rfind("api-ms-win-core-processenvironment-", 0) == 0 ||
        dll_low.rfind("api-ms-win-appmodel-runtime-", 0) == 0;
    if (!is_loader_dll) return nullptr;
    if (std::strcmp(sym_name, "LoadLibraryW") == 0)        return reinterpret_cast<FARPROC>(&hook_LoadLibraryW);
    if (std::strcmp(sym_name, "LoadLibraryA") == 0)        return reinterpret_cast<FARPROC>(&hook_LoadLibraryA);
    if (std::strcmp(sym_name, "LoadLibraryExW") == 0)      return reinterpret_cast<FARPROC>(&hook_LoadLibraryExW);
    if (std::strcmp(sym_name, "LoadLibraryExA") == 0)      return reinterpret_cast<FARPROC>(&hook_LoadLibraryExA);
    if (std::strcmp(sym_name, "GetModuleHandleW") == 0)    return reinterpret_cast<FARPROC>(&hook_GetModuleHandleW);
    if (std::strcmp(sym_name, "GetModuleHandleA") == 0)    return reinterpret_cast<FARPROC>(&hook_GetModuleHandleA);
    if (std::strcmp(sym_name, "GetModuleHandleExW") == 0)  return reinterpret_cast<FARPROC>(&hook_GetModuleHandleExW);
    if (std::strcmp(sym_name, "GetModuleHandleExA") == 0)  return reinterpret_cast<FARPROC>(&hook_GetModuleHandleExA);
    if (std::strcmp(sym_name, "GetModuleFileNameW") == 0)  return reinterpret_cast<FARPROC>(&hook_GetModuleFileNameW);
    if (std::strcmp(sym_name, "GetProcAddress") == 0)      return reinterpret_cast<FARPROC>(&hook_GetProcAddress);
    if (std::strcmp(sym_name, "RtlPcToFileHeader") == 0)   return reinterpret_cast<FARPROC>(&hook_RtlPcToFileHeader);
    if (std::strcmp(sym_name, "CreateThread") == 0)        return reinterpret_cast<FARPROC>(&hook_CreateThread);
    if (std::strcmp(sym_name, "ExitThread") == 0)          return reinterpret_cast<FARPROC>(&hook_ExitThread);
    if (std::strcmp(sym_name, "SetThreadAffinityMask") == 0)            return reinterpret_cast<FARPROC>(&hook_SetThreadAffinityMask);
    if (std::strcmp(sym_name, "GetProcessAffinityMask") == 0)           return reinterpret_cast<FARPROC>(&hook_GetProcessAffinityMask);
    if (std::strcmp(sym_name, "SetThreadIdealProcessor") == 0)          return reinterpret_cast<FARPROC>(&hook_SetThreadIdealProcessor);
    if (std::strcmp(sym_name, "SetThreadIdealProcessorEx") == 0)        return reinterpret_cast<FARPROC>(&hook_SetThreadIdealProcessorEx);
    if (std::strcmp(sym_name, "GetCurrentPackageFullName") == 0)        return reinterpret_cast<FARPROC>(&hook_GetCurrentPackageFullName);
    if (std::strcmp(sym_name, "GetCurrentPackageFamilyName") == 0)      return reinterpret_cast<FARPROC>(&hook_GetCurrentPackageFamilyName);
    if (std::strcmp(sym_name, "GetCurrentApplicationUserModelId") == 0) return reinterpret_cast<FARPROC>(&hook_GetCurrentApplicationUserModelId);
    if (std::strcmp(sym_name, "GetCommandLineW") == 0)                  return reinterpret_cast<FARPROC>(&hook_GetCommandLineW);
    if (std::strcmp(sym_name, "GetCommandLineA") == 0)                  return reinterpret_cast<FARPROC>(&hook_GetCommandLineA);
    return nullptr;
}

inline DWORD section_protect(DWORD ch) noexcept {
    const bool x = (ch & IMAGE_SCN_MEM_EXECUTE) != 0;
    const bool r = (ch & IMAGE_SCN_MEM_READ) != 0;
    const bool w = (ch & IMAGE_SCN_MEM_WRITE) != 0;
    if (x && w) return PAGE_EXECUTE_READWRITE;
    if (x && r) return PAGE_EXECUTE_READ;
    if (x)      return PAGE_EXECUTE;
    if (w)      return PAGE_READWRITE;
    if (r)      return PAGE_READONLY;
    return PAGE_NOACCESS;
}

} // namespace detail

struct CompatHookSnapshot {
    uint64_t affinity_remaps = 0;
    uint64_t ideal_proc_remaps = 0;
    uint64_t appmodel_queries = 0;
    uint64_t winrt_fallback_hits = 0;
};

inline CompatHookSnapshot get_compat_hook_stats() noexcept {
    const auto& s = detail::compat_hook_stats();
    return CompatHookSnapshot{
        s.affinity_remaps.load(std::memory_order_relaxed),
        s.ideal_proc_remaps.load(std::memory_order_relaxed),
        s.appmodel_queries.load(std::memory_order_relaxed),
        s.winrt_fallback_hits.load(std::memory_order_relaxed)
    };
}

inline void set_guest_command_line(const std::wstring& cmdline) noexcept {
    std::lock_guard<std::mutex> lk(detail::guest_cmdline_mtx());
    detail::guest_cmdline_w() = cmdline;
    if (cmdline.empty()) {
        detail::guest_cmdline_a().clear();
    } else {
        const int n = WideCharToMultiByte(CP_UTF8, 0, cmdline.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            detail::guest_cmdline_a().assign(static_cast<size_t>(n - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, cmdline.c_str(), -1, detail::guest_cmdline_a().data(), n, nullptr, nullptr);
        }
    }
}

class MappedPeImage {
    uint8_t* base_ = nullptr;
    size_t image_size_ = 0;
    uint32_t entry_rva_ = 0;
    bool is_dll_ = false;
    bool dll_attached_ = false;

    PRUNTIME_FUNCTION pdata_table_ = nullptr;
    DWORD tls_index_ = TLS_OUT_OF_INDEXES;
    std::vector<PIMAGE_TLS_CALLBACK> tls_callbacks_;

    uint8_t* stub_page_ = nullptr;
    size_t stub_page_cap_ = 0;
    size_t stub_page_used_ = 0;
    std::vector<uint64_t> owned_stub_slots_;

    LoadStats stats_{};
    std::string last_error_;
    std::wstring source_path_;

    FARPROC alloc_auto_stub(const std::string& dll_name, const std::string& sym_name) {
        constexpr size_t kThunkSize = 32u;
        if (!stub_page_) {
            stub_page_cap_ = 65536u;
            stub_page_ = static_cast<uint8_t*>(
                VirtualAlloc(nullptr, stub_page_cap_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (!stub_page_) return nullptr;
        }
        if (stub_page_used_ + kThunkSize > stub_page_cap_) return nullptr;

        uint64_t slot_idx = 0;
        {
            auto& ctx = detail::global_ctx();
            std::lock_guard<std::mutex> lk(ctx.mtx);
            slot_idx = static_cast<uint64_t>(ctx.stubs.size());
            auto s = std::make_unique<detail::StubSlot>();
            s->dll_name = dll_name;
            s->sym_name = sym_name;
            ctx.stubs.push_back(std::move(s));
        }
        owned_stub_slots_.push_back(slot_idx);

        // Emit x86-64 thunk:
        //   48 B9 <slot_idx:8>          ; mov rcx, slot_idx
        //   48 B8 <&auto_stub_dispatch> ; mov rax, &auto_stub_dispatch
        //   FF E0                       ; jmp rax
        uint8_t* p = stub_page_ + stub_page_used_;
        stub_page_used_ += kThunkSize;
        p[0] = 0x48; p[1] = 0xB9;
        std::memcpy(p + 2, &slot_idx, 8);
        const uint64_t target = reinterpret_cast<uint64_t>(&detail::auto_stub_dispatch);
        p[10] = 0x48; p[11] = 0xB8;
        std::memcpy(p + 12, &target, 8);
        p[20] = 0xFF; p[21] = 0xE0;
        ++stats_.auto_stub_count;
        return reinterpret_cast<FARPROC>(p);
    }

public:
    MappedPeImage() = default;
    ~MappedPeImage() { unload(); }
    MappedPeImage(const MappedPeImage&) = delete;
    MappedPeImage& operator=(const MappedPeImage&) = delete;

    const LoadStats& stats() const noexcept { return stats_; }
    const std::string& last_error() const noexcept { return last_error_; }
    void* base() const noexcept { return base_; }
    bool is_loaded() const noexcept { return base_ != nullptr; }
    bool is_dll() const noexcept { return is_dll_; }

    uint64_t total_auto_stub_hits() const noexcept {
        auto& ctx = detail::global_ctx();
        std::lock_guard<std::mutex> lk(ctx.mtx);
        uint64_t sum = 0;
        for (uint64_t idx : owned_stub_slots_) {
            if (idx < ctx.stubs.size()) sum += ctx.stubs[static_cast<size_t>(idx)]->hits.load(std::memory_order_relaxed);
        }
        return sum;
    }

    bool load_from_file(const std::filesystem::path& pe_path, const LoadOptions& opts) {
        std::ifstream f(pe_path, std::ios::binary | std::ios::ate);
        if (!f) {
            last_error_ = "Failed to open PE file: " + pe_path.string();
            return false;
        }
        const size_t sz = static_cast<size_t>(f.tellg());
        f.seekg(0, std::ios::beg);
        std::vector<uint8_t> raw(sz);
        if (!f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(sz))) {
            last_error_ = "Failed to read PE file: " + pe_path.string();
            return false;
        }
        LoadOptions eff = opts;
        if (eff.package_dir.empty()) eff.package_dir = pe_path.parent_path();
        source_path_ = std::filesystem::absolute(pe_path).wstring();
        return load_from_memory(raw.data(), raw.size(), eff);
    }

    bool load_from_memory(const uint8_t* raw, size_t raw_size, const LoadOptions& opts) {
        unload();
        stats_ = {};
        last_error_.clear();

        if (!raw || raw_size < sizeof(IMAGE_DOS_HEADER)) {
            last_error_ = "Input buffer smaller than IMAGE_DOS_HEADER";
            return false;
        }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
            static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > raw_size) {
            last_error_ = "Invalid DOS/NT header";
            return false;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            last_error_ = "Not an x86-64 PE32+ image";
            return false;
        }

        // Ensure AMD Zen 2 #UD compatibility VEH (CLZERO / SSE4a / MONITORX / MWAITX) is active on Intel hosts
        wsx_zen2::ensure_installed();

        // Configure global shim-first search paths
        {
            auto& ctx = detail::global_ctx();
            std::lock_guard<std::mutex> lk(ctx.mtx);
            if (!opts.shim_dir.empty()) ctx.shim_dir = opts.shim_dir;
            if (!opts.package_dir.empty()) ctx.package_dir = opts.package_dir;
        }

        const size_t img_size = nt->OptionalHeader.SizeOfImage;
        const uint64_t pref_base = nt->OptionalHeader.ImageBase;
        stats_.preferred_base = pref_base;
        stats_.image_size = static_cast<uint32_t>(img_size);
        entry_rva_ = nt->OptionalHeader.AddressOfEntryPoint;
        is_dll_ = (nt->FileHeader.Characteristics & IMAGE_FILE_DLL) != 0;

        // 1. Reserve & commit image memory
        void* preferred_hint = opts.force_relocate ? nullptr : reinterpret_cast<void*>(pref_base);
        base_ = static_cast<uint8_t*>(VirtualAlloc(preferred_hint, img_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!base_) {
            base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, img_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        }
        if (!base_) {
            last_error_ = "VirtualAlloc failed for SizeOfImage";
            return false;
        }
        if (opts.force_relocate && reinterpret_cast<uint64_t>(base_) == pref_base) {
            // Allocate a second block to guarantee a non-zero relocation delta
            uint8_t* alt = static_cast<uint8_t*>(VirtualAlloc(nullptr, img_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (alt) {
                VirtualFree(base_, 0, MEM_RELEASE);
                base_ = alt;
            }
        }
        image_size_ = img_size;
        stats_.mapped_base = reinterpret_cast<uint64_t>(base_);

        // 2. Copy PE headers and sections
        const size_t hdr_copy = std::min<size_t>(nt->OptionalHeader.SizeOfHeaders, std::min(raw_size, img_size));
        std::memcpy(base_, raw, hdr_copy);

        const auto* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (sec[i].SizeOfRawData == 0) continue;
            if (static_cast<size_t>(sec[i].PointerToRawData) + sec[i].SizeOfRawData > raw_size ||
                static_cast<size_t>(sec[i].VirtualAddress) + sec[i].SizeOfRawData > img_size) {
                last_error_ = "Section out of bounds";
                unload();
                return false;
            }
            std::memcpy(base_ + sec[i].VirtualAddress, raw + sec[i].PointerToRawData, sec[i].SizeOfRawData);
        }

        // 3. Apply base relocations (DIR64 / HIGHLOW / HIGH / LOW)
        const int64_t delta = static_cast<int64_t>(stats_.mapped_base) - static_cast<int64_t>(pref_base);
        const auto& rel_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (delta != 0 && rel_dir.VirtualAddress != 0 && rel_dir.Size >= sizeof(IMAGE_BASE_RELOCATION)) {
            size_t off = 0;
            while (off + sizeof(IMAGE_BASE_RELOCATION) <= rel_dir.Size) {
                const auto* blk = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(base_ + rel_dir.VirtualAddress + off);
                if (blk->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;
                const size_t nentries = (blk->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(uint16_t);
                const auto* entries = reinterpret_cast<const uint16_t*>(blk + 1);
                for (size_t k = 0; k < nentries; ++k) {
                    const uint16_t type = entries[k] >> 12;
                    const uint16_t r_off = entries[k] & 0x0FFFu;
                    const size_t target_rva = static_cast<size_t>(blk->VirtualAddress) + r_off;
                    if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
                    if (type == IMAGE_REL_BASED_DIR64 && target_rva + 8u <= img_size) {
                        uint64_t v = 0;
                        std::memcpy(&v, base_ + target_rva, 8);
                        v = static_cast<uint64_t>(static_cast<int64_t>(v) + delta);
                        std::memcpy(base_ + target_rva, &v, 8);
                        ++stats_.reloc_count;
                    } else if (type == IMAGE_REL_BASED_HIGHLOW && target_rva + 4u <= img_size) {
                        uint32_t v = 0;
                        std::memcpy(&v, base_ + target_rva, 4);
                        v = static_cast<uint32_t>(static_cast<int64_t>(v) + delta);
                        std::memcpy(base_ + target_rva, &v, 4);
                        ++stats_.reloc_count;
                    } else if (type == IMAGE_REL_BASED_HIGH && target_rva + 2u <= img_size) {
                        uint16_t v = 0;
                        std::memcpy(&v, base_ + target_rva, 2);
                        v = static_cast<uint16_t>(v + HIWORD(static_cast<DWORD>(delta)));
                        std::memcpy(base_ + target_rva, &v, 2);
                        ++stats_.reloc_count;
                    } else if (type == IMAGE_REL_BASED_LOW && target_rva + 2u <= img_size) {
                        uint16_t v = 0;
                        std::memcpy(&v, base_ + target_rva, 2);
                        v = static_cast<uint16_t>(v + LOWORD(static_cast<DWORD>(delta)));
                        std::memcpy(base_ + target_rva, &v, 2);
                        ++stats_.reloc_count;
                    }
                }
                off += blk->SizeOfBlock;
            }
        }

        // 4. Resolve IAT imports (named & ordinal, shim-first over System32, LoadLibrary hooks, auto-stubs)
        const auto& imp_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (imp_dir.VirtualAddress != 0 && imp_dir.Size >= sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base_ + imp_dir.VirtualAddress);
            for (; desc->Name != 0; ++desc) {
                const char* dll_cstr = reinterpret_cast<const char*>(base_ + desc->Name);
                const std::string dll_name(dll_cstr);
                const std::string dll_low = detail::to_lower_ascii(dll_name);
                const std::wstring dll_w = detail::utf8_to_wide(dll_cstr);
                ++stats_.imported_dlls;

                bool was_shim = false;
                HMODULE mod = detail::resolve_dll_shim_first(dll_w.c_str(), 0, &was_shim);
                if (was_shim) ++stats_.shim_dll_hits;
                if (!mod && !opts.allow_auto_stubs) {
                    last_error_ = "Missing imported DLL: " + dll_name;
                    unload();
                    return false;
                }

                const auto* orig_thunk = reinterpret_cast<const IMAGE_THUNK_DATA64*>(
                    base_ + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
                auto* iat_thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base_ + desc->FirstThunk);

                for (; orig_thunk->u1.AddressOfData != 0; ++orig_thunk, ++iat_thunk) {
                    FARPROC proc = nullptr;
                    std::string sym_label;
                    if (IMAGE_SNAP_BY_ORDINAL64(orig_thunk->u1.Ordinal)) {
                        const WORD ord = static_cast<WORD>(IMAGE_ORDINAL64(orig_thunk->u1.Ordinal));
                        sym_label = "#" + std::to_string(ord);
                        ++stats_.ordinal_imports;
                        if (mod) proc = GetProcAddress(mod, MAKEINTRESOURCEA(ord));
                    } else {
                        const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                            base_ + orig_thunk->u1.AddressOfData);
                        sym_label = ibn->Name;
                        proc = detail::maybe_hook_loader_import(dll_low, ibn->Name);
                        if (!proc && mod) proc = GetProcAddress(mod, ibn->Name);
                    }
                    if (!proc) {
                        if (opts.allow_auto_stubs) {
                            proc = alloc_auto_stub(dll_name, sym_label);
                            if (opts.trace_imports) {
                                std::printf("[wsx_pe] stubbed %s!%s\n", dll_name.c_str(), sym_label.c_str());
                            }
                        } else {
                            last_error_ = "Unresolved import: " + dll_name + "!" + sym_label;
                            unload();
                            return false;
                        }
                    }
                    iat_thunk->u1.Function = reinterpret_cast<ULONGLONG>(proc);
                    ++stats_.resolved_imports;
                }
            }
        }

        // 4b. Eagerly bind IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT (Delay-Load IAT)
        const auto& dly_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        if (dly_dir.VirtualAddress != 0 && dly_dir.Size >= sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)) {
            const auto* ddesc = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(
                base_ + dly_dir.VirtualAddress);
            for (; ddesc->DllNameRVA != 0; ++ddesc) {
                const bool rva_based = (ddesc->Attributes.AllAttributes & 1u) != 0u;
                auto ptr_from = [&](DWORD v) -> uint8_t* {
                    if (v == 0) return nullptr;
                    if (rva_based) return base_ + v;
                    return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(v));
                };

                const char* dll_cstr = reinterpret_cast<const char*>(ptr_from(ddesc->DllNameRVA));
                if (!dll_cstr || !*dll_cstr) continue;
                const std::string dll_name(dll_cstr);
                const std::string dll_low = detail::to_lower_ascii(dll_name);
                const std::wstring dll_w = detail::utf8_to_wide(dll_cstr);
                ++stats_.imported_dlls;

                bool was_shim = false;
                HMODULE mod = detail::resolve_dll_shim_first(dll_w.c_str(), 0, &was_shim);
                if (was_shim) ++stats_.shim_dll_hits;

                const auto* int_thunk = reinterpret_cast<const IMAGE_THUNK_DATA64*>(
                    ptr_from(ddesc->ImportNameTableRVA));
                auto* iat_thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(
                    ptr_from(ddesc->ImportAddressTableRVA));
                if (!int_thunk || !iat_thunk) continue;

                for (; int_thunk->u1.AddressOfData != 0; ++int_thunk, ++iat_thunk) {
                    FARPROC proc = nullptr;
                    std::string sym_label;
                    if (IMAGE_SNAP_BY_ORDINAL64(int_thunk->u1.Ordinal)) {
                        const WORD ord = static_cast<WORD>(IMAGE_ORDINAL64(int_thunk->u1.Ordinal));
                        sym_label = "#" + std::to_string(ord);
                        ++stats_.ordinal_imports;
                        if (mod) proc = GetProcAddress(mod, MAKEINTRESOURCEA(ord));
                    } else {
                        const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                            rva_based
                                ? (base_ + int_thunk->u1.AddressOfData)
                                : reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(int_thunk->u1.AddressOfData)));
                        sym_label = ibn->Name;
                        proc = detail::maybe_hook_loader_import(dll_low, ibn->Name);
                        if (!proc && mod) proc = GetProcAddress(mod, ibn->Name);
                    }
                    if (!proc && opts.allow_auto_stubs) {
                        proc = alloc_auto_stub(dll_name, sym_label);
                    }
                    if (proc) {
                        iat_thunk->u1.Function = reinterpret_cast<ULONGLONG>(proc);
                        ++stats_.delay_imports_resolved;
                        ++stats_.resolved_imports;
                    }
                }
            }
        }

        // 4c. Initialize /GS Security Cookie from IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG
        const auto& lc_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
        if (lc_dir.VirtualAddress != 0 &&
            lc_dir.Size >= offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, SecurityCookie) + sizeof(ULONGLONG)) {
            const auto* lc = reinterpret_cast<const IMAGE_LOAD_CONFIG_DIRECTORY64*>(
                base_ + lc_dir.VirtualAddress);
            if (lc->Size >= offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, SecurityCookie) + sizeof(ULONGLONG) &&
                lc->SecurityCookie != 0) {
                const uintptr_t cookie_addr = static_cast<uintptr_t>(lc->SecurityCookie);
                const uintptr_t img_lo = reinterpret_cast<uintptr_t>(base_);
                const uintptr_t img_hi = img_lo + img_size;
                if (cookie_addr >= img_lo && cookie_addr + sizeof(ULONGLONG) <= img_hi) {
                    auto* cookie_ptr = reinterpret_cast<ULONGLONG*>(cookie_addr);
                    constexpr ULONGLONG kDefaultGsCookie = 0x00002B992DDFA232ULL;
                    if (*cookie_ptr == 0ULL || *cookie_ptr == kDefaultGsCookie) {
                        FILETIME ft{};
                        GetSystemTimeAsFileTime(&ft);
                        LARGE_INTEGER qpc{};
                        QueryPerformanceCounter(&qpc);
                        ULONGLONG cookie =
                            (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) ^
                            static_cast<ULONGLONG>(ft.dwLowDateTime) ^
                            static_cast<ULONGLONG>(qpc.QuadPart) ^
                            (static_cast<ULONGLONG>(GetCurrentProcessId()) << 16) ^
                            static_cast<ULONGLONG>(GetCurrentThreadId()) ^
                            static_cast<ULONGLONG>(cookie_addr);
                        cookie &= 0x0000FFFFFFFFFFFFULL;
                        if (cookie == 0ULL || cookie == kDefaultGsCookie) {
                            cookie = 0x00002B992DDFA233ULL;
                        }
                        *cookie_ptr = cookie;
                    }
                    stats_.security_cookie_initialized = true;
                }
            }
        }

        // Make auto-stub thunk page RX before executing any guest code
        if (stub_page_ && stub_page_used_ > 0) {
            DWORD oldp = 0;
            VirtualProtect(stub_page_, stub_page_cap_, PAGE_EXECUTE_READ, &oldp);
            FlushInstructionCache(GetCurrentProcess(), stub_page_, stub_page_used_);
        }

        // 5. Apply per-section VirtualProtect permissions
        {
            DWORD oldp = 0;
            VirtualProtect(base_, hdr_copy, PAGE_READONLY, &oldp);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
                const size_t vsz = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
                if (vsz == 0) continue;
                VirtualProtect(base_ + sec[i].VirtualAddress, vsz,
                               detail::section_protect(sec[i].Characteristics), &oldp);
            }
            FlushInstructionCache(GetCurrentProcess(), base_, img_size);
        }

        // 6. Register .pdata SEH unwind table via RtlAddFunctionTable + mapped range for RtlPcToFileHeader
        {
            auto& ctx = detail::global_ctx();
            std::lock_guard<std::mutex> lk(ctx.mtx);
            ctx.mapped_images.push_back(detail::MappedImageRange{
                reinterpret_cast<uintptr_t>(base_), image_size_, source_path_});
        }
        const auto& exc_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (exc_dir.VirtualAddress != 0 && exc_dir.Size >= sizeof(RUNTIME_FUNCTION)) {
            const DWORD fn_count = exc_dir.Size / sizeof(RUNTIME_FUNCTION);
            auto* table = reinterpret_cast<PRUNTIME_FUNCTION>(base_ + exc_dir.VirtualAddress);
            if (RtlAddFunctionTable(table, fn_count, stats_.mapped_base)) {
                pdata_table_ = table;
                stats_.pdata_functions = fn_count;
            }
        }

        // 7. Initialize true static TLS (TEB::ThreadLocalStoragePointer + .tls RawData + ZeroFill) & execute TLS callbacks
        const auto& tls_dir_ent = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        if (tls_dir_ent.VirtualAddress != 0 && tls_dir_ent.Size >= sizeof(IMAGE_TLS_DIRECTORY64)) {
            const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(base_ + tls_dir_ent.VirtualAddress);
            const uintptr_t img_lo = reinterpret_cast<uintptr_t>(base_);
            const uintptr_t img_hi = img_lo + img_size;

            std::vector<PIMAGE_TLS_CALLBACK> cbs;
            if (tls->AddressOfCallBacks != 0) {
                const auto* cb = reinterpret_cast<const PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks);
                while (*cb) {
                    cbs.push_back(*cb);
                    ++cb;
                }
            }

            if (tls->AddressOfIndex != 0) {
                detail::GuestTlsModule gmod{};
                gmod.image_base = img_lo;
                if (tls->StartAddressOfRawData >= img_lo &&
                    tls->EndAddressOfRawData >= tls->StartAddressOfRawData &&
                    tls->EndAddressOfRawData <= img_hi) {
                    gmod.raw_data = reinterpret_cast<const uint8_t*>(tls->StartAddressOfRawData);
                    gmod.raw_size = static_cast<size_t>(tls->EndAddressOfRawData - tls->StartAddressOfRawData);
                }
                gmod.zero_fill = static_cast<size_t>(tls->SizeOfZeroFill);
                const DWORD align_enc = (tls->Characteristics >> 20) & 0xFu;
                gmod.alignment = (align_enc >= 1u && align_enc <= 13u) ? (size_t(1u) << (align_enc - 1u)) : 16u;
                gmod.callbacks = cbs;

                {
                    auto& ctx = detail::global_ctx();
                    std::lock_guard<std::mutex> lk(ctx.mtx);
                    tls_index_ = detail::alloc_guest_tls_slot_locked(ctx);
                    if (tls_index_ != TLS_OUT_OF_INDEXES) {
                        gmod.slot_index = tls_index_;
                        if (detail::attach_guest_tls_module_on_current_thread_locked(ctx, gmod)) {
                            ctx.guest_tls_modules.push_back(gmod);
                            stats_.guest_tls_slot = tls_index_;
                            stats_.guest_tls_block_bytes = static_cast<uint32_t>(
                                std::max<size_t>(gmod.raw_size + gmod.zero_fill, 16u));
                        } else {
                            detail::free_guest_tls_slot_locked(ctx, tls_index_);
                            tls_index_ = TLS_OUT_OF_INDEXES;
                        }
                    }
                }
                if (tls_index_ != TLS_OUT_OF_INDEXES) {
                    auto* idx_ptr = reinterpret_cast<DWORD*>(tls->AddressOfIndex);
                    DWORD old_p = 0;
                    const bool changed_p = VirtualProtect(idx_ptr, sizeof(DWORD), PAGE_READWRITE, &old_p) != 0;
                    *idx_ptr = tls_index_;
                    if (changed_p) VirtualProtect(idx_ptr, sizeof(DWORD), old_p, &old_p);
                }
            }

            // Dispatch IMAGE_TLS_CALLBACK(DLL_PROCESS_ATTACH) AFTER .tls block is live on TEB!
            for (PIMAGE_TLS_CALLBACK cb : cbs) {
                tls_callbacks_.push_back(cb);
                cb(base_, DLL_PROCESS_ATTACH, nullptr);
                ++stats_.tls_callbacks_run;
            }
        }

        // 8. For DLL images, run DllMain(DLL_PROCESS_ATTACH)
        if (is_dll_ && entry_rva_ != 0) {
            using DllMainFn = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
            const auto fn = reinterpret_cast<DllMainFn>(base_ + entry_rva_);
            if (!fn(reinterpret_cast<HINSTANCE>(base_), DLL_PROCESS_ATTACH, nullptr)) {
                last_error_ = "Guest DllMain(DLL_PROCESS_ATTACH) returned FALSE";
                unload();
                return false;
            }
            dll_attached_ = true;
        }
        return true;
    }

    int run_exe_entry() const {
        if (!base_ || entry_rva_ == 0 || is_dll_) return -1;
        using ExeEntryFn = int(*)();
        const auto entry = reinterpret_cast<ExeEntryFn>(base_ + entry_rva_);
        return entry();
    }

    static void* resolve_forwarder_export(const char* fwd_str) noexcept {
        if (!fwd_str || !*fwd_str) return nullptr;
        const char* dot = std::strrchr(fwd_str, '.');
        if (!dot || dot == fwd_str || *(dot + 1) == '\0') return nullptr;

        std::string dll_part(fwd_str, static_cast<size_t>(dot - fwd_str));
        const std::string sym_part(dot + 1);
        const std::string dll_low = detail::to_lower_ascii(dll_part);
        if (dll_low.size() < 4 || dll_low.substr(dll_low.size() - 4) != ".dll") {
            dll_part += ".dll";
        }
        const std::wstring dll_w = detail::utf8_to_wide(dll_part.c_str());
        HMODULE mod = detail::resolve_dll_shim_first(dll_w.c_str(), 0, nullptr);
        if (!mod) return nullptr;

        if (sym_part[0] == '#') {
            const long ord = std::strtol(sym_part.c_str() + 1, nullptr, 10);
            if (ord <= 0 || ord > 0xFFFF) return nullptr;
            return reinterpret_cast<void*>(GetProcAddress(mod, MAKEINTRESOURCEA(static_cast<WORD>(ord))));
        }
        if (FARPROC hooked = detail::maybe_hook_loader_import(detail::to_lower_ascii(dll_part), sym_part.c_str())) {
            return reinterpret_cast<void*>(hooked);
        }
        return reinterpret_cast<void*>(GetProcAddress(mod, sym_part.c_str()));
    }

    void* find_export(const char* name) const noexcept {
        if (!base_ || !name) return nullptr;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base_);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base_ + dos->e_lfanew);
        const auto& exp_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (exp_dir.VirtualAddress == 0 || exp_dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) return nullptr;
        const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base_ + exp_dir.VirtualAddress);
        const auto* names = reinterpret_cast<const uint32_t*>(base_ + exp->AddressOfNames);
        const auto* ords = reinterpret_cast<const uint16_t*>(base_ + exp->AddressOfNameOrdinals);
        const auto* funcs = reinterpret_cast<const uint32_t*>(base_ + exp->AddressOfFunctions);
        for (uint32_t i = 0; i < exp->NumberOfNames; ++i) {
            const char* ename = reinterpret_cast<const char*>(base_ + names[i]);
            if (std::strcmp(ename, name) == 0) {
                const uint32_t rva = funcs[ords[i]];
                if (rva == 0) return nullptr;
                if (rva >= exp_dir.VirtualAddress && rva < exp_dir.VirtualAddress + exp_dir.Size) {
                    return resolve_forwarder_export(reinterpret_cast<const char*>(base_ + rva));
                }
                return base_ + rva;
            }
        }
        return nullptr;
    }

    void* find_export_ordinal(uint16_t ordinal) const noexcept {
        if (!base_) return nullptr;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base_);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base_ + dos->e_lfanew);
        const auto& exp_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (exp_dir.VirtualAddress == 0 || exp_dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) return nullptr;
        const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base_ + exp_dir.VirtualAddress);
        if (ordinal < exp->Base) return nullptr;
        const uint32_t idx = static_cast<uint32_t>(ordinal - exp->Base);
        if (idx >= exp->NumberOfFunctions) return nullptr;
        const auto* funcs = reinterpret_cast<const uint32_t*>(base_ + exp->AddressOfFunctions);
        const uint32_t rva = funcs[idx];
        if (rva == 0) return nullptr;
        if (rva >= exp_dir.VirtualAddress && rva < exp_dir.VirtualAddress + exp_dir.Size) {
            return resolve_forwarder_export(reinterpret_cast<const char*>(base_ + rva));
        }
        return base_ + rva;
    }

    void unload() noexcept {
        if (base_) {
            const uintptr_t b = reinterpret_cast<uintptr_t>(base_);
            // Flush isolated guest atexit/_onexit destructors in LIFO order BEFORE detaching/freeing guest code!
            stats_.guest_atexit_run += detail::flush_guest_atexit_entries(b, nullptr);

            if (is_dll_ && dll_attached_ && entry_rva_ != 0) {
                using DllMainFn = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
                const auto fn = reinterpret_cast<DllMainFn>(base_ + entry_rva_);
                fn(reinterpret_cast<HINSTANCE>(base_), DLL_PROCESS_DETACH, nullptr);
                dll_attached_ = false;
            }
            for (auto it = tls_callbacks_.rbegin(); it != tls_callbacks_.rend(); ++it) {
                (*it)(base_, DLL_PROCESS_DETACH, nullptr);
            }
            tls_callbacks_.clear();
            if (tls_index_ != TLS_OUT_OF_INDEXES) {
                detail::detach_guest_tls_on_current_thread(b);
                auto& ctx = detail::global_ctx();
                std::lock_guard<std::mutex> lk(ctx.mtx);
                ctx.guest_tls_modules.erase(
                    std::remove_if(ctx.guest_tls_modules.begin(), ctx.guest_tls_modules.end(),
                                   [b](const detail::GuestTlsModule& m) { return m.image_base == b; }),
                    ctx.guest_tls_modules.end());
                detail::free_guest_tls_slot_locked(ctx, tls_index_);
                tls_index_ = TLS_OUT_OF_INDEXES;
            }
            if (pdata_table_) {
                RtlDeleteFunctionTable(pdata_table_);
                pdata_table_ = nullptr;
            }
            {
                auto& ctx = detail::global_ctx();
                std::lock_guard<std::mutex> lk(ctx.mtx);
                ctx.mapped_images.erase(
                    std::remove_if(ctx.mapped_images.begin(), ctx.mapped_images.end(),
                                   [b](const detail::MappedImageRange& r) { return r.base == b; }),
                    ctx.mapped_images.end());
            }
            VirtualFree(base_, 0, MEM_RELEASE);
            base_ = nullptr;
            image_size_ = 0;
        }
        if (stub_page_) {
            VirtualFree(stub_page_, 0, MEM_RELEASE);
            stub_page_ = nullptr;
            stub_page_cap_ = 0;
            stub_page_used_ = 0;
        }
        owned_stub_slots_.clear();
    }
};

} // namespace wsx_pe
