// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Windows x64 implementation of Xbox Series X|S Direct3D 12.X bridge (d3d12_xs.dll):
// 1. Full COM Proxy (WsxD3D12DeviceProxy : public ID3D12Device8) — zero VirtualProtect vtable patching.
// 2. Full Command Queue & Command List COM Proxies (WsxD3D12CommandQueueProxy, WsxD3D12CommandListProxy : ID3D12GraphicsCommandList6)
//    with Xbox ResourceBarrier state sanitization, proxy unwrapping, and Mesh Shader / DXR tracing.
// 3. UMA Emulation: transparent CPU-mappable promotion for DEFAULT / CUSTOM-L1 buffers & buffer heaps
//    via D3D12_HEAP_TYPE_GPU_UPLOAD (Resizable BAR) or D3D12_HEAP_TYPE_CUSTOM (WRITE_COMBINE, L0).
// 4. Zero-Copy SwapChain Backbuffer Hand-off + Non-Blocking Pipelined PresentX (no post-Present GPU wait stall).
// 5. Real VRR (DXGI_FEATURE_PRESENT_ALLOW_TEARING), HDR10/scRGB (IDXGISwapChain4::SetColorSpace1 + SetHDRMetaData + IDXGIOutput6),
//    and 60 Hz / 120 Hz high-resolution QPC + WaitableObject frame pacing (SetFrameIntervalX / ScheduleFrameEventX / WaitFrameEventX).
// 6. D3D12 Debug Layer (ID3D12Debug) and GPU-Based Validation (ID3D12Debug1) in Debug builds or via ProcessDebugFlags / WSX_D3D12_DEBUG.

#include "wsx_config.h"
#include "d3d12_xs_proxies.h"
#include "d3d12_xs_xbox_abi.h"
#include "d3d12_xs_shader_pipeline.h"
#include "d3d12_xs_sm66_runtime.h"
#include "d3d12_xs_rdna2_to_hlsl.h"
#include "d3d12_xs_pso_cache.h"
#include "d3d12_xs_async_pso.h"
#include "wsx_debug_hud_dx12.h"
#include "wsx_crash_diag.h"

#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <vector>

#if !defined(_M_X64)
#error The Xbox imports under test require AMD64.
#endif

using Microsoft::WRL::ComPtr;

extern "C" void wsx_query_frame_pacer_hud_metrics(float* one_pct_low, float* p99_ms) noexcept;

namespace {

constexpr char deserialize_name[] = "D3D12CreateRootSignatureDeserializer";
constexpr char serialize_name[] = "D3D12SerializeVersionedRootSignature";
constexpr char create_device_name[] = "D3D12CreateDevice";
constexpr char get_debug_name[] = "D3D12GetDebugInterface";

// XboxBvhBuilder sets these bits in the root signature flags.
constexpr UINT untranslated_xbox_flags = 0x03000000;

constexpr UINT kXboxHeapFlagsMask = 0x40000008u;     // D3D12_HEAP_FLAG_ALLOW_DISPLAY (0x8) | 0x40000000u
constexpr UINT kXboxResourceFlagsMask = 0x70000000u; // ALLOW_DCC | FORCE_TEXTURE_COMPATIBILITY | GAMEDVR
constexpr UINT kMaxSwapChainBuffers = 3u;

struct XboxCreateDeviceParameters {
    UINT Version;
    UINT ProcessDebugFlags;
    UINT GraphicsCommandQueueRingSizeBytes;
    BOOL DisableGeometryShaderAllocations;
    BOOL DisableTessellationShaderAllocations;
    BOOL DisableDXR;
    UINT CreateDeviceFlags;
    UINT AmplificationShaderIndirectArgsBufferSize;
    UINT AmplificationShaderPayloadBufferSize;
};

struct XboxPresentPlaneParameters {
    uint64_t Token;
    UINT ResourceCount;
    ID3D12Resource* const* ppResources;
    DXGI_COLOR_SPACE_TYPE ColorSpace;
};

struct XboxPresentParameters {
    UINT Flags;
};

struct DesktopExports {
    decltype(&D3D12CreateRootSignatureDeserializer) deserialize = nullptr;
    decltype(&D3D12SerializeVersionedRootSignature) serialize = nullptr;
    decltype(&D3D12CreateDevice) create_device = nullptr;
    decltype(&D3D12GetDebugInterface) get_debug = nullptr;
    HRESULT load_error = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
};

const DesktopExports& desktop_exports() noexcept {
    static const DesktopExports exports = []() noexcept {
        DesktopExports result;
        std::array<wchar_t, 32768> path{};
        constexpr wchar_t library[] = L"\\d3d12.dll";
        const UINT length = GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
        if (length == 0) {
            result.load_error = HRESULT_FROM_WIN32(GetLastError());
            return result;
        }
        if (length > path.size() - std::size(library)) {
            result.load_error = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            return result;
        }
        std::copy_n(library, std::size(library), path.data() + length);
        const HMODULE module = LoadLibraryExW(path.data(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            result.load_error = HRESULT_FROM_WIN32(GetLastError());
            return result;
        }
        result.deserialize = std::bit_cast<decltype(result.deserialize)>(
            GetProcAddress(module, deserialize_name));
        result.serialize = std::bit_cast<decltype(result.serialize)>(
            GetProcAddress(module, serialize_name));
        result.create_device = std::bit_cast<decltype(result.create_device)>(
            GetProcAddress(module, create_device_name));
        result.get_debug = std::bit_cast<decltype(result.get_debug)>(
            GetProcAddress(module, get_debug_name));
        return result;
    }();
    return exports;
}

HRESULT write_result(const char* function, HRESULT status,
                     std::optional<UINT> flags = std::nullopt) noexcept {
    std::array<wchar_t, 32768> path{};
    constexpr wchar_t log_name[] = L"shim_log.txt";
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return status;
    }
    wchar_t* filename = std::wcsrchr(path.data(), L'\\');
    if (!filename || filename + 1 > path.data() + path.size() - std::size(log_name)) {
        return status;
    }
    std::copy_n(log_name, std::size(log_name), filename + 1);

    std::array<char, 160> line{};
    const auto code = static_cast<unsigned long>(status);
    const int bytes = flags
        ? std::snprintf(line.data(), line.size(), "%s HRESULT=0x%08lX RootFlags=0x%08X\r\n",
                        function, code, *flags)
        : std::snprintf(line.data(), line.size(), "%s HRESULT=0x%08lX\r\n", function, code);
    if (bytes <= 0 || static_cast<std::size_t>(bytes) >= line.size()) {
        return status;
    }
    const HANDLE file = CreateFileW(path.data(), FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, line.data(), static_cast<DWORD>(bytes), &written, nullptr);
        CloseHandle(file);
    }
    if (flags) {
        wsx::trace_hr("d3d12_xs", function, status, "flags=0x%08X", *flags);
    } else {
        wsx::trace_hr("d3d12_xs", function, status);
    }
    return status;
}

bool env_flag_enabled(const wchar_t* name, bool default_val = false) noexcept {
    wchar_t buf[16]{};
    const DWORD len = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0) return default_val;
    return buf[0] == L'1' || buf[0] == L't' || buf[0] == L'T' || buf[0] == L'y' || buf[0] == L'Y';
}

std::optional<UINT> root_flags(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* description) noexcept {
    if (description) {
        switch (description->Version) {
        case D3D_ROOT_SIGNATURE_VERSION_1_0:
            return description->Desc_1_0.Flags;
        case D3D_ROOT_SIGNATURE_VERSION_1_1:
            return description->Desc_1_1.Flags;
        case D3D_ROOT_SIGNATURE_VERSION_1_2:
            return description->Desc_1_2.Flags;
        default:
            break;
        }
    }
    return std::nullopt;
}

bool xbox_abi_mode_enabled() noexcept {
    return wsx::get_config_bool("WSX_XBOX_D3D12_ABI", "Graphics", "xbox_abi", false);
}

bool strip_xbox_flags_enabled() noexcept {
    return env_flag_enabled(L"WSX_STRIP_XBOX_ROOT_FLAGS", false) || xbox_abi_mode_enabled();
}

D3D12_VERSIONED_ROOT_SIGNATURE_DESC strip_flags_copy(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& src) noexcept {
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC copy = src;
    switch (copy.Version) {
    case D3D_ROOT_SIGNATURE_VERSION_1_0:
        copy.Desc_1_0.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(
            copy.Desc_1_0.Flags & ~untranslated_xbox_flags);
        break;
    case D3D_ROOT_SIGNATURE_VERSION_1_1:
        copy.Desc_1_1.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(
            copy.Desc_1_1.Flags & ~untranslated_xbox_flags);
        break;
    case D3D_ROOT_SIGNATURE_VERSION_1_2:
        copy.Desc_1_2.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(
            copy.Desc_1_2.Flags & ~untranslated_xbox_flags);
        break;
    default:
        break;
    }
    return copy;
}

// ============================================================================
// DXGI Device -> Adapter -> Output6 (HDR10 & 120Hz capable) COM Chain
// ============================================================================
class WsxDxgiOutput6 final : public IDXGIOutput6 {
    std::atomic<ULONG> ref_{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
            riid == __uuidof(IDXGIOutput) || riid == __uuidof(IDXGIOutput1) ||
            riid == __uuidof(IDXGIOutput2) || riid == __uuidof(IDXGIOutput3) ||
            riid == __uuidof(IDXGIOutput4) || riid == __uuidof(IDXGIOutput5) ||
            riid == __uuidof(IDXGIOutput6)) {
            *ppv = static_cast<IDXGIOutput6*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return DXGI_ERROR_NOT_FOUND; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void** ppParent) override {
        if (ppParent) *ppParent = nullptr;
        return DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC* pDesc) override {
        if (!pDesc) return E_INVALIDARG;
        std::memset(pDesc, 0, sizeof(*pDesc));
        wcscpy_s(pDesc->DeviceName, L"\\\\.\\DISPLAY1");
        pDesc->DesktopCoordinates = {0, 0, 3840, 2160};
        pDesc->AttachedToDesktop = TRUE;
        pDesc->Rotation = DXGI_MODE_ROTATION_IDENTITY;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDisplayModeList(DXGI_FORMAT, UINT, UINT* pNumModes, DXGI_MODE_DESC*) override {
        if (pNumModes) *pNumModes = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(const DXGI_MODE_DESC* pModeToMatch,
                                                      DXGI_MODE_DESC* pClosestMatch, IUnknown*) override {
        if (!pModeToMatch || !pClosestMatch) return E_INVALIDARG;
        *pClosestMatch = *pModeToMatch;
        if (pClosestMatch->RefreshRate.Numerator == 0) {
            pClosestMatch->RefreshRate.Numerator = 120;
            pClosestMatch->RefreshRate.Denominator = 1;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE WaitForVBlank() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE TakeOwnership(IUnknown*, BOOL) override { return S_OK; }
    void STDMETHODCALLTYPE ReleaseOwnership() override {}
    HRESULT STDMETHODCALLTYPE GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetGammaControl(const DXGI_GAMMA_CONTROL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetGammaControl(DXGI_GAMMA_CONTROL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetDisplaySurface(IDXGISurface*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData(IDXGISurface*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override {
        if (!pStats) return E_INVALIDARG;
        std::memset(pStats, 0, sizeof(*pStats));
        return S_OK;
    }
    // IDXGIOutput1
    HRESULT STDMETHODCALLTYPE GetDisplayModeList1(DXGI_FORMAT, UINT, UINT* pNumModes, DXGI_MODE_DESC1*) override {
        if (pNumModes) *pNumModes = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode1(const DXGI_MODE_DESC1* pModeToMatch,
                                                       DXGI_MODE_DESC1* pClosestMatch, IUnknown*) override {
        if (!pModeToMatch || !pClosestMatch) return E_INVALIDARG;
        *pClosestMatch = *pModeToMatch;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData1(IDXGIResource*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DuplicateOutput(IUnknown*, IDXGIOutputDuplication**) override { return E_NOTIMPL; }
    // IDXGIOutput2
    BOOL STDMETHODCALLTYPE SupportsOverlays() override { return TRUE; }
    // IDXGIOutput3
    HRESULT STDMETHODCALLTYPE CheckOverlaySupport(DXGI_FORMAT, IUnknown*, UINT* pFlags) override {
        if (!pFlags) return E_INVALIDARG;
        *pFlags = DXGI_OVERLAY_SUPPORT_FLAG_DIRECT | DXGI_OVERLAY_SUPPORT_FLAG_SCALING;
        return S_OK;
    }
    // IDXGIOutput4
    HRESULT STDMETHODCALLTYPE CheckOverlayColorSpaceSupport(DXGI_FORMAT, DXGI_COLOR_SPACE_TYPE,
                                                            IUnknown*, UINT* pFlags) override {
        if (!pFlags) return E_INVALIDARG;
        *pFlags = DXGI_OVERLAY_COLOR_SPACE_SUPPORT_FLAG_PRESENT;
        return S_OK;
    }
    // IDXGIOutput5
    HRESULT STDMETHODCALLTYPE DuplicateOutput1(IUnknown*, UINT, UINT, const DXGI_FORMAT*,
                                               IDXGIOutputDuplication**) override {
        return E_NOTIMPL;
    }
    // IDXGIOutput6 (HDR10 / WCG metadata)
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_OUTPUT_DESC1* pDesc) override {
        if (!pDesc) return E_INVALIDARG;
        std::memset(pDesc, 0, sizeof(*pDesc));
        wcscpy_s(pDesc->DeviceName, L"\\\\.\\DISPLAY1");
        pDesc->DesktopCoordinates = {0, 0, 3840, 2160};
        pDesc->AttachedToDesktop = TRUE;
        pDesc->Rotation = DXGI_MODE_ROTATION_IDENTITY;
        pDesc->BitsPerColor = 10;
        pDesc->ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        // BT.2020 primaries & D65 white point
        pDesc->RedPrimary[0] = 0.708f;   pDesc->RedPrimary[1] = 0.292f;
        pDesc->GreenPrimary[0] = 0.170f; pDesc->GreenPrimary[1] = 0.797f;
        pDesc->BluePrimary[0] = 0.131f;  pDesc->BluePrimary[1] = 0.046f;
        pDesc->WhitePoint[0] = 0.3127f;  pDesc->WhitePoint[1] = 0.3290f;
        pDesc->MinLuminance = 0.001f;
        pDesc->MaxLuminance = 1000.0f;
        pDesc->MaxFullFrameLuminance = 400.0f;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CheckHardwareCompositionSupport(UINT* pFlags) override {
        if (!pFlags) return E_INVALIDARG;
        *pFlags = DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_FULLSCREEN |
                  DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_WINDOWED |
                  DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_CURSOR_STRETCHED;
        return S_OK;
    }
};

class WsxDxgiAdapter final : public IDXGIAdapter1 {
    std::atomic<ULONG> ref_{1};
    ComPtr<IDXGIAdapter1> real_adapter_;
public:
    explicit WsxDxgiAdapter(IDXGIAdapter1* real_adapter = nullptr) : real_adapter_(real_adapter) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
            riid == __uuidof(IDXGIAdapter) || riid == __uuidof(IDXGIAdapter1)) {
            *ppv = static_cast<IDXGIAdapter1*>(this);
            AddRef();
            return S_OK;
        }
        if (real_adapter_) {
            return real_adapter_->QueryInterface(riid, ppv);
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void* d) override {
        return real_adapter_ ? real_adapter_->SetPrivateData(g, s, d) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override {
        return real_adapter_ ? real_adapter_->SetPrivateDataInterface(g, u) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* s, void* d) override {
        return real_adapter_ ? real_adapter_->GetPrivateData(g, s, d) : DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override {
        if (real_adapter_) return real_adapter_->GetParent(riid, ppParent);
        return CreateDXGIFactory1(riid, ppParent);
    }
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT Output, IDXGIOutput** ppOutput) override {
        if (!ppOutput) return E_INVALIDARG;
        if (Output > 0) {
            *ppOutput = nullptr;
            return DXGI_ERROR_NOT_FOUND;
        }
        *ppOutput = new (std::nothrow) WsxDxgiOutput6();
        return *ppOutput ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC* pDesc) override {
        if (!pDesc) return E_INVALIDARG;
        if (real_adapter_ && SUCCEEDED(real_adapter_->GetDesc(pDesc))) {
            return S_OK;
        }
        std::memset(pDesc, 0, sizeof(*pDesc));
        wcscpy_s(pDesc->Description, L"MizuBox Scarlett Translation Adapter");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID g, LARGE_INTEGER* v) override {
        if (real_adapter_) return real_adapter_->CheckInterfaceSupport(g, v);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* pDesc) override {
        if (!pDesc) return E_INVALIDARG;
        if (real_adapter_ && SUCCEEDED(real_adapter_->GetDesc1(pDesc))) {
            return S_OK;
        }
        std::memset(pDesc, 0, sizeof(*pDesc));
        wcscpy_s(pDesc->Description, L"MizuBox Scarlett Translation Adapter");
        return S_OK;
    }
};

class WsxDxgiDevice1 final : public IDXGIDevice1 {
    std::atomic<ULONG> ref_{1};
    ComPtr<ID3D12Device> d3d_device_;
    ComPtr<IDXGIAdapter1> real_adapter_;
    UINT max_latency_ = 1;
public:
    WsxDxgiDevice1(ID3D12Device* dev, IDXGIAdapter1* adapter = nullptr)
        : d3d_device_(dev), real_adapter_(adapter) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
            riid == __uuidof(IDXGIDevice) || riid == __uuidof(IDXGIDevice1)) {
            *ppv = static_cast<IDXGIDevice1*>(this);
            AddRef();
            return S_OK;
        }
        if (d3d_device_) {
            return d3d_device_->QueryInterface(riid, ppv);
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return DXGI_ERROR_NOT_FOUND; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override {
        ComPtr<IDXGIAdapter> adapter;
        const HRESULT hr = GetAdapter(adapter.GetAddressOf());
        if (FAILED(hr)) return hr;
        return adapter->QueryInterface(riid, ppParent);
    }
    HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter** pAdapter) override {
        if (!pAdapter) return E_INVALIDARG;
        *pAdapter = new (std::nothrow) WsxDxgiAdapter(real_adapter_.Get());
        return *pAdapter ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE CreateSurface(const DXGI_SURFACE_DESC*, UINT, DXGI_USAGE,
                                            const DXGI_SHARED_RESOURCE*, IDXGISurface**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE QueryResourceResidency(IUnknown* const*, DXGI_RESIDENCY*, UINT) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* pPriority) override {
        if (pPriority) *pPriority = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override {
        max_latency_ = std::clamp(MaxLatency, 1u, kMaxSwapChainBuffers);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override {
        if (pMaxLatency) *pMaxLatency = max_latency_;
        return S_OK;
    }
};

// ============================================================================
// PresentContext: Zero-Copy SwapChain + Pipelined Non-Blocking PresentX + VRR/HDR/120Hz
// ============================================================================
struct PresentContext {
    HWND hwnd = nullptr;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT buffer_count = 2;
    UINT swapchain_flags = 0;
    UINT next_display_buffer_index = 0;
    bool tearing_supported = false;
    bool vrr_enabled = false;
    bool hdr_enabled = false;
    bool user_resized = false;
    DXGI_COLOR_SPACE_TYPE active_color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

    ComPtr<ID3D12Device> device;
    ComPtr<IDXGISwapChain4> swap_chain;
    HANDLE frame_latency_waitable_object = nullptr;
    ComPtr<ID3D12Resource> back_buffers[kMaxSwapChainBuffers];

    // Per-backbuffer command allocators & fence values for pipelined non-blocking fallback copy
    ComPtr<ID3D12CommandAllocator> allocators[kMaxSwapChainBuffers];
    UINT64 frame_fence_values[kMaxSwapChainBuffers]{};
    ComPtr<ID3D12CommandAllocator> capture_allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd_list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    UINT64 fence_value = 0;

    // Lazy bilinear scaled present blitter (used when window/fullscreen dims != game RT dims)
    bool blitter_attempted = false;
    bool blitter_ready = false;
    DXGI_FORMAT blitter_rtv_fmt = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12RootSignature> blitter_rs;
    ComPtr<ID3D12PipelineState> blitter_pso;
    ComPtr<ID3D12DescriptorHeap> blitter_srv_heap;
    ComPtr<ID3D12DescriptorHeap> blitter_rtv_heap;

    // Telemetry & testing
    UINT64 presented_frames = 0;
    UINT64 zero_copy_frames = 0;
    UINT64 resize_count = 0;
    UINT64 scaled_blit_frames = 0;
    bool frame_captured = false;

    ~PresentContext() {
        if (frame_latency_waitable_object) {
            CloseHandle(frame_latency_waitable_object);
        }
        if (fence_event) {
            CloseHandle(fence_event);
        }
    }
};

struct FramePacerState {
    UINT interval_enum = 1u; // 1 = 60Hz, 2 = 120Hz, 3 = 30Hz, 0 = Unlimited
    UINT max_latency = 1u;
    UINT flags = 0u;
    int64_t target_period_qpc = 0;
    int64_t next_deadline_qpc = 0;
    int64_t qpc_freq = 0;
    HANDLE high_res_timer = nullptr;
    HANDLE swapchain_waitable = nullptr;

    FramePacerState() noexcept {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        qpc_freq = freq.QuadPart;
        high_res_timer = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!high_res_timer) {
            high_res_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        }
        update_interval(1u, 1u, 0u);
    }

    ~FramePacerState() {
        if (high_res_timer) {
            CloseHandle(high_res_timer);
        }
    }

    void update_interval(UINT interval, UINT latency, UINT flg) noexcept {
        const std::string hz_str = wsx::get_config_string("WSX_FRAME_INTERVAL_HZ", "Graphics", "refresh_hz", "");
        if (!hz_str.empty()) {
            const int hz = std::atoi(hz_str.c_str());
            if (hz >= 120) interval = 2u;
            else if (hz == 60) interval = 1u;
            else if (hz == 30) interval = 3u;
            else if (hz == 0) interval = 0u;
        }
        interval_enum = interval;
        max_latency = std::clamp(latency, 1u, kMaxSwapChainBuffers - 1u);
        flags = flg;

        if (interval == 2u) {
            target_period_qpc = qpc_freq / 120; // 120 Hz (8.333 ms)
        } else if (interval == 1u) {
            target_period_qpc = qpc_freq / 60;  // 60 Hz (16.667 ms)
        } else if (interval == 3u) {
            target_period_qpc = qpc_freq / 30;  // 30 Hz (33.333 ms)
        } else {
            target_period_qpc = 0;              // Unlimited / VRR unlocked
        }
    }

    void schedule_origin() noexcept {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        next_deadline_qpc = now.QuadPart + target_period_qpc;
    }

    void wait_origin(DWORD timeout_ms) noexcept {
        // 1. Wait on DXGI SwapChain Frame Latency Waitable Object (non-blocking pipeline backpressure)
        if (swapchain_waitable) {
            WaitForSingleObjectEx(swapchain_waitable, std::min<DWORD>(timeout_ms, 100u), TRUE);
        }

        // Skip wall-clock sleep when running automated fast CTest exit-after-frames runs
        wchar_t exit_buf[16]{};
        if (GetEnvironmentVariableW(L"WSX_EXIT_AFTER_FRAMES", exit_buf, 16) > 0 &&
            !env_flag_enabled(L"WSX_FORCE_FRAME_PACING", false)) {
            return;
        }

        if (target_period_qpc <= 0) {
            return;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (next_deadline_qpc == 0 || now.QuadPart >= next_deadline_qpc) {
            next_deadline_qpc = now.QuadPart + target_period_qpc;
            return;
        }

        const int64_t remaining_qpc = next_deadline_qpc - now.QuadPart;
        const int64_t remaining_100ns = (remaining_qpc * 10000000LL) / qpc_freq;

        // Sleep via high-resolution waitable timer for bulk wait (> 1.2 ms), then QPC spin-tail
        if (high_res_timer && remaining_100ns > 12000LL) {
            LARGE_INTEGER due{};
            due.QuadPart = -(remaining_100ns - 8000LL);
            if (SetWaitableTimer(high_res_timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(high_res_timer, 50);
            }
        }
        for (;;) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= next_deadline_qpc) break;
            YieldProcessor();
        }
        next_deadline_qpc = now.QuadPart + target_period_qpc;
    }
};

std::mutex g_present_mutex;
std::unordered_map<ID3D12CommandQueue*, std::unique_ptr<PresentContext>> g_present_contexts;
std::atomic<uint64_t> g_frame_token{1};

// ============================================================================
// Window Subclassing: Borderless Fullscreen (Alt+Enter), WM_SIZE & WM_CLOSE
// ============================================================================
struct WindowHookState {
    WNDPROC orig_proc = nullptr;
    bool is_fullscreen = false;
    LONG_PTR saved_style = 0;
    LONG_PTR saved_exstyle = 0;
    RECT saved_rect{};
    std::atomic<UINT> pending_w{0};
    std::atomic<UINT> pending_h{0};
    std::atomic<bool> resize_pending{false};
    std::atomic<bool> close_requested{false};
};

std::mutex g_window_hook_mtx;
std::unordered_map<HWND, std::unique_ptr<WindowHookState>> g_window_hooks;

bool toggle_borderless_fullscreen_impl(HWND hwnd, bool* out_is_fs = nullptr) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return false;
    bool enter_fs = false;
    LONG_PTR restore_style = 0;
    LONG_PTR restore_exstyle = 0;
    RECT restore_rect{};
    {
        std::lock_guard<std::mutex> lk(g_window_hook_mtx);
        auto it = g_window_hooks.find(hwnd);
        if (it == g_window_hooks.end() || !it->second) return false;
        WindowHookState& st = *it->second;
        if (!st.is_fullscreen) {
            st.saved_style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            st.saved_exstyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            GetWindowRect(hwnd, &st.saved_rect);
            st.is_fullscreen = true;
            enter_fs = true;
        } else {
            restore_style = st.saved_style;
            restore_exstyle = st.saved_exstyle;
            restore_rect = st.saved_rect;
            st.is_fullscreen = false;
            enter_fs = false;
        }
    }

    if (enter_fs) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        RECT mr{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        if (mon && GetMonitorInfoW(mon, &mi)) {
            mr = mi.rcMonitor;
        }
        LONG_PTR cur_style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        cur_style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        SetWindowLongPtrW(hwnd, GWL_STYLE, cur_style | WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, mr.left, mr.top,
                     mr.right - mr.left, mr.bottom - mr.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
    } else {
        if (restore_style != 0) SetWindowLongPtrW(hwnd, GWL_STYLE, restore_style);
        if (restore_exstyle != 0) SetWindowLongPtrW(hwnd, GWL_EXSTYLE, restore_exstyle);
        const int rw = std::max<int>(64, restore_rect.right - restore_rect.left);
        const int rh = std::max<int>(64, restore_rect.bottom - restore_rect.top);
        SetWindowPos(hwnd, HWND_NOTOPMOST, restore_rect.left, restore_rect.top,
                     rw, rh, SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }

    RECT cr{};
    if (GetClientRect(hwnd, &cr)) {
        const UINT cw = static_cast<UINT>(std::max<LONG>(0, cr.right - cr.left));
        const UINT ch = static_cast<UINT>(std::max<LONG>(0, cr.bottom - cr.top));
        if (cw > 0 && ch > 0) {
            std::lock_guard<std::mutex> lk(g_window_hook_mtx);
            auto it = g_window_hooks.find(hwnd);
            if (it != g_window_hooks.end() && it->second) {
                it->second->pending_w.store(cw, std::memory_order_relaxed);
                it->second->pending_h.store(ch, std::memory_order_relaxed);
                it->second->resize_pending.store(true, std::memory_order_release);
            }
        }
    }
    if (out_is_fs) *out_is_fs = enter_fs;
    wsx::trace_void("d3d12_xs", "ToggleBorderlessFullscreen", "hwnd=%p fullscreen=%d",
                    static_cast<void*>(hwnd), enter_fs ? 1 : 0);
    return true;
}

LRESULT CALLBACK wsx_window_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WNDPROC orig = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_window_hook_mtx);
        auto it = g_window_hooks.find(hwnd);
        if (it != g_window_hooks.end() && it->second) {
            orig = it->second->orig_proc;
            if (msg == WM_SIZE && wParam != SIZE_MINIMIZED) {
                const UINT cw = LOWORD(lParam);
                const UINT ch = HIWORD(lParam);
                if (cw > 0 && ch > 0) {
                    it->second->pending_w.store(cw, std::memory_order_relaxed);
                    it->second->pending_h.store(ch, std::memory_order_relaxed);
                    it->second->resize_pending.store(true, std::memory_order_release);
                }
            } else if (msg == WM_CLOSE) {
                it->second->close_requested.store(true, std::memory_order_release);
            } else if (msg == WM_NCDESTROY) {
                if (orig) {
                    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
                }
                g_window_hooks.erase(it);
            }
        }
    }

    if (msg == WM_SYSKEYDOWN && wParam == VK_RETURN && ((lParam & (1LL << 29)) != 0)) {
        toggle_borderless_fullscreen_impl(hwnd, nullptr);
        return 0;
    }

    if (orig && orig != wsx_window_subclass_proc) {
        return CallWindowProcW(orig, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ensure_window_subclassed(HWND hwnd) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return;

    std::lock_guard<std::mutex> lk(g_window_hook_mtx);
    if (g_window_hooks.find(hwnd) != g_window_hooks.end()) return;

    WNDPROC prev = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wsx_window_subclass_proc)));
    if (!prev || prev == wsx_window_subclass_proc) return;

    auto st = std::make_unique<WindowHookState>();
    st->orig_proc = prev;
    g_window_hooks[hwnd] = std::move(st);
}

bool consume_pending_window_resize(HWND hwnd, UINT& out_w, UINT& out_h) noexcept {
    if (!hwnd) return false;
    std::lock_guard<std::mutex> lk(g_window_hook_mtx);
    auto it = g_window_hooks.find(hwnd);
    if (it == g_window_hooks.end() || !it->second) return false;
    if (!it->second->resize_pending.exchange(false, std::memory_order_acq_rel)) return false;
    out_w = it->second->pending_w.load(std::memory_order_relaxed);
    out_h = it->second->pending_h.load(std::memory_order_relaxed);
    return (out_w > 0 && out_h > 0);
}

bool query_window_hook_flags(HWND hwnd, bool& out_fs, bool& out_close) noexcept {
    out_fs = false;
    out_close = false;
    if (!hwnd) return false;
    std::lock_guard<std::mutex> lk(g_window_hook_mtx);
    auto it = g_window_hooks.find(hwnd);
    if (it == g_window_hooks.end() || !it->second) return false;
    out_fs = it->second->is_fullscreen;
    out_close = it->second->close_requested.load(std::memory_order_acquire);
    return true;
}

HWND find_process_window() noexcept {
    HWND found = nullptr;
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND hwnd, LPARAM lparam) -> BOOL {
            if (IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
                *reinterpret_cast<HWND*>(lparam) = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    if (found) {
        return found;
    }
    EnumWindows(
        [](HWND hwnd, LPARAM lparam) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
                *reinterpret_cast<HWND*>(lparam) = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

DXGI_FORMAT sanitize_swapchain_format(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

void wait_fence_value(PresentContext& ctx, UINT64 target_val) noexcept {
    if (!ctx.fence || !ctx.fence_event || target_val == 0) {
        return;
    }
    if (ctx.fence->GetCompletedValue() < target_val) {
        if (SUCCEEDED(ctx.fence->SetEventOnCompletion(target_val, ctx.fence_event))) {
            WaitForSingleObject(ctx.fence_event, 5000);
        }
    }
}

void wait_queue_idle(ID3D12CommandQueue* queue, PresentContext& ctx) noexcept {
    if (!queue || !ctx.fence || !ctx.fence_event) {
        return;
    }
    const UINT64 val = ++ctx.fence_value;
    if (SUCCEEDED(queue->Signal(ctx.fence.Get(), val))) {
        wait_fence_value(ctx, val);
    }
}

void configure_hdr_metadata(IDXGISwapChain4* sc4, DXGI_COLOR_SPACE_TYPE requested_cs,
                            DXGI_FORMAT format, PresentContext& ctx) noexcept {
    if (!sc4) return;

    DXGI_COLOR_SPACE_TYPE target_cs = requested_cs;
    if (target_cs == static_cast<DXGI_COLOR_SPACE_TYPE>(0)) {
        if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            target_cs = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709; // scRGB linear FP16 HDR
        } else if (format == DXGI_FORMAT_R10G10B10A2_UNORM &&
                   wsx::get_config_bool("WSX_HDR", "Graphics", "hdr", false)) {
            target_cs = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020; // HDR10 ST.2084
        } else {
            target_cs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        }
    }

    if (target_cs == ctx.active_color_space) {
        return;
    }

    UINT support = 0;
    if (SUCCEEDED(sc4->CheckColorSpaceSupport(target_cs, &support)) &&
        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0) {
        if (SUCCEEDED(sc4->SetColorSpace1(target_cs))) {
            ctx.active_color_space = target_cs;
            ctx.hdr_enabled =
                (target_cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                 target_cs == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
            if (ctx.hdr_enabled) {
                DXGI_HDR_METADATA_HDR10 hdr10{};
                // BT.2020 primaries in 1/50000 units
                hdr10.RedPrimary[0]   = 35400; hdr10.RedPrimary[1]   = 14600;
                hdr10.GreenPrimary[0] = 8500;  hdr10.GreenPrimary[1] = 39850;
                hdr10.BluePrimary[0]  = 6550;  hdr10.BluePrimary[1]  = 2300;
                hdr10.WhitePoint[0]   = 15635; hdr10.WhitePoint[1]   = 16450;
                hdr10.MaxMasteringLuminance = 1000;
                hdr10.MinMasteringLuminance = 10; // 0.001 nits
                hdr10.MaxContentLightLevel = 1000;
                hdr10.MaxFrameAverageLightLevel = 400;
                sc4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(hdr10), &hdr10);
            }
        }
    }
}

PresentContext* get_or_create_present_context_locked(
    ID3D12CommandQueue* pQueue, ID3D12Device* explicit_device = nullptr) noexcept {
    if (!pQueue) return nullptr;
    auto& ctx_ptr = g_present_contexts[pQueue];
    if (!ctx_ptr) {
        ctx_ptr = std::make_unique<PresentContext>();
        if (explicit_device) {
            ctx_ptr->device = explicit_device;
        } else if (FAILED(pQueue->GetDevice(IID_PPV_ARGS(ctx_ptr->device.GetAddressOf())))) {
            g_present_contexts.erase(pQueue);
            return nullptr;
        }
        for (UINT i = 0; i < kMaxSwapChainBuffers; ++i) {
            if (FAILED(ctx_ptr->device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(ctx_ptr->allocators[i].GetAddressOf())))) {
                g_present_contexts.erase(pQueue);
                return nullptr;
            }
        }
        if (FAILED(ctx_ptr->device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(ctx_ptr->capture_allocator.GetAddressOf())))) {
            g_present_contexts.erase(pQueue);
            return nullptr;
        }
        if (FAILED(ctx_ptr->device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, ctx_ptr->allocators[0].Get(), nullptr,
                IID_PPV_ARGS(ctx_ptr->cmd_list.GetAddressOf())))) {
            g_present_contexts.erase(pQueue);
            return nullptr;
        }
        ctx_ptr->cmd_list->Close();
        if (FAILED(ctx_ptr->device->CreateFence(
                0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(ctx_ptr->fence.GetAddressOf())))) {
            g_present_contexts.erase(pQueue);
            return nullptr;
        }
        ctx_ptr->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    return ctx_ptr.get();
}

bool resize_swapchain_locked(ID3D12CommandQueue* pQueue, PresentContext& ctx,
                             UINT new_w, UINT new_h) noexcept {
    if (!ctx.swap_chain || new_w == 0u || new_h == 0u) return false;
    if (ctx.width == new_w && ctx.height == new_h) return true;

    wait_queue_idle(pQueue, ctx);
    for (UINT i = 0; i < kMaxSwapChainBuffers; ++i) {
        ctx.back_buffers[i].Reset();
        ctx.frame_fence_values[i] = 0;
    }

    const HRESULT hr = ctx.swap_chain->ResizeBuffers(
        ctx.buffer_count, new_w, new_h, ctx.format, ctx.swapchain_flags);
    if (FAILED(hr)) {
        // Re-fetch existing buffers if driver rejected resize so PresentX never crashes
        for (UINT i = 0; i < ctx.buffer_count; ++i) {
            ctx.swap_chain->GetBuffer(i, IID_PPV_ARGS(ctx.back_buffers[i].GetAddressOf()));
        }
        return false;
    }

    for (UINT i = 0; i < ctx.buffer_count; ++i) {
        ctx.swap_chain->GetBuffer(i, IID_PPV_ARGS(ctx.back_buffers[i].GetAddressOf()));
    }
    ctx.width = new_w;
    ctx.height = new_h;
    ctx.user_resized = true;
    ++ctx.resize_count;
    ctx.next_display_buffer_index = 0;
    wsx::trace_void("d3d12_xs", "ResizeBuffers", "%ux%u count=%llu",
                    new_w, new_h, static_cast<unsigned long long>(ctx.resize_count));
    return true;
}

bool ensure_present_blitter_locked(PresentContext& ctx, DXGI_FORMAT rtv_fmt) noexcept {
    if (ctx.blitter_ready && ctx.blitter_rtv_fmt == rtv_fmt) return true;
    if (ctx.blitter_attempted && !ctx.blitter_ready) return false;
    ctx.blitter_attempted = true;
    ctx.blitter_ready = false;
    if (!ctx.device) return false;

    static constexpr const char kBlitHlsl[] = R"HLSL(
Texture2D<float4> g_src : register(t0);
SamplerState g_lin : register(s0);
struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VsOut vs_main(uint vid : SV_VertexID) {
    VsOut o;
    float2 uv = float2((vid << 1u) & 2u, vid & 2u);
    o.uv = uv;
    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}
float4 ps_main(VsOut i) : SV_Target {
    return g_src.Sample(g_lin, i.uv);
}
)HLSL";

    std::vector<uint8_t> vs_dxil, ps_dxil;
    char err_buf[256]{};
    if (FAILED(wsx_sm66::compile_hlsl_to_dxil_vec(
            kBlitHlsl, std::strlen(kBlitHlsl), L"vs_main", L"vs_6_0", vs_dxil, err_buf, sizeof(err_buf))) ||
        FAILED(wsx_sm66::compile_hlsl_to_dxil_vec(
            kBlitHlsl, std::strlen(kBlitHlsl), L"ps_main", L"ps_6_0", ps_dxil, err_buf, sizeof(err_buf)))) {
        return false;
    }

    if (!ctx.blitter_rs) {
        D3D12_DESCRIPTOR_RANGE1 rng{};
        rng.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        rng.NumDescriptors = 1;
        rng.BaseShaderRegister = 0;
        rng.RegisterSpace = 0;
        rng.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        rng.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER1 param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges = &rng;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samp.MaxLOD = D3D12_FLOAT32_MAX;
        samp.ShaderRegister = 0;
        samp.RegisterSpace = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rs_desc.Desc_1_1.NumParameters = 1;
        rs_desc.Desc_1_1.pParameters = &param;
        rs_desc.Desc_1_1.NumStaticSamplers = 1;
        rs_desc.Desc_1_1.pStaticSamplers = &samp;
        rs_desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> blob, err;
        auto fn_ser = desktop_exports().serialize;
        if (!fn_ser || FAILED(fn_ser(&rs_desc, blob.GetAddressOf(), err.GetAddressOf())) || !blob) {
            return false;
        }
        if (FAILED(ctx.device->CreateRootSignature(
                0, blob->GetBufferPointer(), blob->GetBufferSize(),
                IID_PPV_ARGS(ctx.blitter_rs.GetAddressOf())))) {
            return false;
        }
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = ctx.blitter_rs.Get();
    pso_desc.VS = {vs_dxil.data(), vs_dxil.size()};
    pso_desc.PS = {ps_dxil.data(), ps_dxil.size()};
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = rtv_fmt;
    pso_desc.SampleDesc.Count = 1;

    ctx.blitter_pso.Reset();
    if (FAILED(ctx.device->CreateGraphicsPipelineState(
            &pso_desc, IID_PPV_ARGS(ctx.blitter_pso.GetAddressOf())))) {
        return false;
    }

    if (!ctx.blitter_srv_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 1;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(ctx.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(ctx.blitter_srv_heap.GetAddressOf())))) {
            return false;
        }
    }
    if (!ctx.blitter_rtv_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(ctx.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(ctx.blitter_rtv_heap.GetAddressOf())))) {
            return false;
        }
    }

    ctx.blitter_rtv_fmt = rtv_fmt;
    ctx.blitter_ready = true;
    return true;
}

bool ensure_swapchain_locked(ID3D12CommandQueue* pQueue, PresentContext& ctx,
                             UINT w, UINT h, DXGI_FORMAT requested_fmt,
                             UINT buffer_count, FramePacerState* pacer = nullptr) noexcept {
    const DXGI_FORMAT fmt = sanitize_swapchain_format(requested_fmt);
    const UINT target_buffers = std::clamp(buffer_count, 2u, kMaxSwapChainBuffers);

    if (ctx.swap_chain && ctx.format == fmt && ctx.buffer_count == target_buffers &&
        (ctx.user_resized || (ctx.width == w && ctx.height == h))) {
        return true;
    }

    wait_queue_idle(pQueue, ctx);
    for (UINT i = 0; i < kMaxSwapChainBuffers; ++i) {
        ctx.back_buffers[i].Reset();
        ctx.frame_fence_values[i] = 0;
    }
    if (ctx.frame_latency_waitable_object) {
        CloseHandle(ctx.frame_latency_waitable_object);
        ctx.frame_latency_waitable_object = nullptr;
        if (pacer) pacer->swapchain_waitable = nullptr;
    }
    ctx.swap_chain.Reset();

    ctx.hwnd = find_process_window();
    if (!ctx.hwnd) {
        return false;
    }

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) {
        return false;
    }

    BOOL allow_tearing = FALSE;
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5))) {
        if (FAILED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing)))) {
            allow_tearing = FALSE;
        }
    }
    ctx.tearing_supported = (allow_tearing == TRUE);

    DXGI_SWAP_CHAIN_DESC1 sc_desc{};
    sc_desc.Width = w;
    sc_desc.Height = h;
    sc_desc.Format = fmt;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = target_buffers;
    sc_desc.Scaling = DXGI_SCALING_STRETCH;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sc_desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                    (ctx.tearing_supported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);

    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory->CreateSwapChainForHwnd(
            pQueue, ctx.hwnd, &sc_desc, nullptr, nullptr, sc1.GetAddressOf()))) {
        // Fallback without waitable object if driver/mode rejects it
        sc_desc.Flags = ctx.tearing_supported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
        if (FAILED(factory->CreateSwapChainForHwnd(
                pQueue, ctx.hwnd, &sc_desc, nullptr, nullptr, sc1.GetAddressOf()))) {
            return false;
        }
    }

    factory->MakeWindowAssociation(ctx.hwnd, DXGI_MWA_NO_ALT_ENTER);
    ensure_window_subclassed(ctx.hwnd);
    if (FAILED(sc1.As(&ctx.swap_chain))) {
        return false;
    }

    ctx.swapchain_flags = sc_desc.Flags;
    if ((sc_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0) {
        const UINT max_lat = pacer ? std::max(1u, pacer->max_latency) : (target_buffers - 1u);
        ctx.swap_chain->SetMaximumFrameLatency(max_lat);
        ctx.frame_latency_waitable_object = ctx.swap_chain->GetFrameLatencyWaitableObject();
        if (pacer) {
            pacer->swapchain_waitable = ctx.frame_latency_waitable_object;
        }
    }

    for (UINT i = 0; i < target_buffers; ++i) {
        ctx.swap_chain->GetBuffer(i, IID_PPV_ARGS(ctx.back_buffers[i].GetAddressOf()));
    }
    ctx.width = w;
    ctx.height = h;
    ctx.format = fmt;
    ctx.buffer_count = target_buffers;
    ctx.user_resized = false;
    ctx.next_display_buffer_index = 0;
    ctx.active_color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

    configure_hdr_metadata(ctx.swap_chain.Get(), static_cast<DXGI_COLOR_SPACE_TYPE>(0), fmt, ctx);
    return true;
}

void capture_resource_to_ppm(ID3D12CommandQueue* queue, PresentContext& ctx,
                             ID3D12Resource* src, const wchar_t* ppm_path) noexcept {
    if (!ctx.device || !ctx.capture_allocator || !ctx.cmd_list || !src) {
        return;
    }
    wait_queue_idle(queue, ctx);

    const D3D12_RESOURCE_DESC desc = src->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 total_bytes = 0;
    ctx.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &num_rows, &row_size, &total_bytes);

    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    readback_heap.CreationNodeMask = 1;
    readback_heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC buf_desc{};
    buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width = total_bytes;
    buf_desc.Height = 1;
    buf_desc.DepthOrArraySize = 1;
    buf_desc.MipLevels = 1;
    buf_desc.Format = DXGI_FORMAT_UNKNOWN;
    buf_desc.SampleDesc.Count = 1;
    buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> readback;
    if (FAILED(ctx.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(readback.GetAddressOf())))) {
        return;
    }

    if (FAILED(ctx.capture_allocator->Reset()) ||
        FAILED(ctx.cmd_list->Reset(ctx.capture_allocator.Get(), nullptr))) {
        return;
    }

    D3D12_RESOURCE_BARRIER to_copy{};
    to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy.Transition.pResource = src;
    to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ctx.cmd_list->ResourceBarrier(1, &to_copy);

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = readback.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = src;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;

    ctx.cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    D3D12_RESOURCE_BARRIER to_present = to_copy;
    to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    ctx.cmd_list->ResourceBarrier(1, &to_present);

    if (SUCCEEDED(ctx.cmd_list->Close())) {
        ID3D12CommandList* lists[] = {ctx.cmd_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        wait_queue_idle(queue, ctx);
    }

    void* mapped = nullptr;
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_bytes)};
    if (FAILED(readback->Map(0, &read_range, &mapped)) || !mapped) {
        return;
    }

    const UINT w = static_cast<UINT>(desc.Width);
    const UINT h = desc.Height;
    std::vector<unsigned char> rgb(static_cast<std::size_t>(w) * h * 3u);
    const auto* base = static_cast<const unsigned char*>(mapped) + footprint.Offset;
    for (UINT y = 0; y < h; ++y) {
        const auto* row = base + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch;
        auto* out = rgb.data() + static_cast<std::size_t>(y) * w * 3u;
        for (UINT x = 0; x < w; ++x) {
            if (desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                const uint32_t packed = reinterpret_cast<const uint32_t*>(row)[x];
                out[x * 3 + 0] = static_cast<unsigned char>(((packed >>  0) & 0x3FFu) >> 2);
                out[x * 3 + 1] = static_cast<unsigned char>(((packed >> 10) & 0x3FFu) >> 2);
                out[x * 3 + 2] = static_cast<unsigned char>(((packed >> 20) & 0x3FFu) >> 2);
            } else if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                       desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                out[x * 3 + 0] = row[x * 4 + 0];
                out[x * 3 + 1] = row[x * 4 + 1];
                out[x * 3 + 2] = row[x * 4 + 2];
            } else {
                // B8G8R8A8 -> R8G8B8
                out[x * 3 + 0] = row[x * 4 + 2];
                out[x * 3 + 1] = row[x * 4 + 1];
                out[x * 3 + 2] = row[x * 4 + 0];
            }
        }
    }
    D3D12_RANGE empty_range{0, 0};
    readback->Unmap(0, &empty_range);

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, ppm_path, L"wb") == 0 && fp) {
        std::fprintf(fp, "P6\n%u %u\n255\n", w, h);
        std::fwrite(rgb.data(), 1, rgb.size(), fp);
        std::fclose(fp);
    }
}

// ============================================================================
// Console Process Debug Flags & DXBC RTS0 Xbox Root Signature Flag Sanitizer
// ============================================================================
// Console-exact mask for D3D12XboxSetProcessDebugFlags (bits 0, 20..24, 31)
constexpr UINT kXboxProcessDebugFlagsMask = 0x81F00001u;
std::atomic<UINT> g_xbox_process_debug_flags{0u};

inline uint32_t dxbc_md5_rotl(uint32_t x, uint32_t c) noexcept {
    return (x << c) | (x >> (32u - c));
}

void dxbc_md5_transform(uint32_t state[4], const uint8_t block[64]) noexcept {
    static constexpr uint32_t kS[64] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
    };
    static constexpr uint32_t kK[64] = {
        0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,
        0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,
        0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau,0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
        0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,
        0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,
        0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u,0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
        0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,
        0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u
    };
    uint32_t m[16]{};
    std::memcpy(m, block, 64);
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (uint32_t i = 0; i < 64u; ++i) {
        uint32_t f = 0u, g = 0u;
        if (i < 16u)      { f = (b & c) | ((~b) & d); g = i; }
        else if (i < 32u) { f = (d & b) | ((~d) & c); g = (5u * i + 1u) & 15u; }
        else if (i < 48u) { f = b ^ c ^ d;            g = (3u * i + 5u) & 15u; }
        else              { f = c ^ (b | (~d));       g = (7u * i) & 15u; }
        const uint32_t tmp = d;
        d = c;
        c = b;
        b = b + dxbc_md5_rotl(a + f + kK[i] + m[g], kS[i]);
        a = tmp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

// Recompute the public DXBC container 16-byte checksum at bytes [4..19] over bytes [20..totalSize].
void recompute_dxbc_hash_inplace(std::vector<uint8_t>& blob) noexcept {
    if (blob.size() < 32u) return;
    const uint8_t* payload = blob.data() + 20u;
    const uint32_t payload_len = static_cast<uint32_t>(blob.size() - 20u);
    uint32_t state[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    uint32_t offset = 0u;
    while (offset + 64u <= payload_len) {
        dxbc_md5_transform(state, payload + offset);
        offset += 64u;
    }
    const uint32_t rem = payload_len - offset;
    const uint32_t bit_len_lo = payload_len << 3u;
    // Microsoft DXBC modified MD5: bit_len_lo at [0..3], bit_len_hi = (payload_len << 1) | 1 at [60..63]
    const uint32_t bit_len_hi = (payload_len << 1u) | 1u;
    uint8_t tail[64]{};
    if (rem >= 56u) {
        std::memcpy(tail, payload + offset, rem);
        tail[rem] = 0x80u;
        dxbc_md5_transform(state, tail);
        std::memset(tail, 0, sizeof(tail));
        std::memcpy(tail + 0u, &bit_len_lo, 4u);
        std::memcpy(tail + 60u, &bit_len_hi, 4u);
        dxbc_md5_transform(state, tail);
    } else {
        std::memcpy(tail + 0u, &bit_len_lo, 4u);
        if (rem > 0u) std::memcpy(tail + 4u, payload + offset, rem);
        tail[4u + rem] = 0x80u;
        std::memcpy(tail + 60u, &bit_len_hi, 4u);
        dxbc_md5_transform(state, tail);
    }
    std::memcpy(blob.data() + 4u, state, 16u);
}

// Strip Xbox-only root signature flags (>= 0x01000000, e.g. D3D12XBOX_ROOT_SIGNATURE_FLAG_FORCE_MEMORY_BASED_ABI)
// from a serialized DXBC RTS0 container and recompute its DXBC checksum so desktop D3D12 accepts it.
bool sanitize_xbox_root_signature_blob(
    const void* src,
    SIZE_T src_len,
    std::vector<uint8_t>& out_patched,
    uint32_t* out_stripped_flags = nullptr) {
    if (out_stripped_flags) *out_stripped_flags = 0u;
    if (!src || src_len < 36u) return false;
    const auto* bytes = static_cast<const uint8_t*>(src);
    if (std::memcmp(bytes, "DXBC", 4u) != 0) return false;

    uint32_t total_size = 0u, chunk_count = 0u;
    std::memcpy(&total_size, bytes + 24u, 4u);
    std::memcpy(&chunk_count, bytes + 28u, 4u);
    if (total_size < 36u || total_size > src_len || chunk_count == 0u || chunk_count > 64u) {
        return false;
    }
    if (32u + static_cast<size_t>(chunk_count) * 4u > total_size) return false;

    uint32_t rts0_flags_off = 0u;
    uint32_t xbox_flags = 0u;
    for (uint32_t i = 0u; i < chunk_count; ++i) {
        uint32_t coff = 0u;
        std::memcpy(&coff, bytes + 32u + static_cast<size_t>(i) * 4u, 4u);
        if (coff + 32u > total_size) continue;
        if (std::memcmp(bytes + coff, "RTS0", 4u) != 0) continue;
        uint32_t csize = 0u;
        std::memcpy(&csize, bytes + coff + 4u, 4u);
        if (csize < 24u || coff + 8u + csize > total_size) continue;
        uint32_t version = 0u, flags = 0u;
        std::memcpy(&version, bytes + coff + 8u, 4u);
        std::memcpy(&flags, bytes + coff + 8u + 20u, 4u);
        if ((version == 1u || version == 2u) && (flags & 0xFF000000u) != 0u) {
            rts0_flags_off = coff + 8u + 20u;
            xbox_flags = flags & 0xFF000000u;
            break;
        }
    }
    if (xbox_flags == 0u) return false;

    out_patched.assign(bytes, bytes + total_size);
    uint32_t flags = 0u;
    std::memcpy(&flags, out_patched.data() + rts0_flags_off, 4u);
    flags &= ~0xFF000000u;
    std::memcpy(out_patched.data() + rts0_flags_off, &flags, 4u);
    recompute_dxbc_hash_inplace(out_patched);
    if (out_stripped_flags) *out_stripped_flags = xbox_flags;
    return true;
}

// ============================================================================
// Full COM Proxy: WsxD3D12DeviceProxy : public ID3D12Device8
// (Replaces fragile VirtualProtect vtable slot 0/27 patching with a clean COM wrapper)
// ============================================================================
// Private UUID to unwrap WsxD3D12DeviceProxy when needed internally
struct __declspec(uuid("b9d7402a-6e19-4c91-9b34-8150649e27f1")) IWsxD3D12DeviceProxyMarker : IUnknown {};

class WsxD3D12DeviceProxy final : public ID3D12Device8 {
    std::atomic<ULONG> ref_{1};
    ComPtr<ID3D12Device> real0_;
    ComPtr<ID3D12Device1> real1_;
    ComPtr<ID3D12Device2> real2_;
    ComPtr<ID3D12Device3> real3_;
    ComPtr<ID3D12Device4> real4_;
    ComPtr<ID3D12Device5> real5_;
    ComPtr<ID3D12Device6> real6_;
    ComPtr<ID3D12Device7> real7_;
    ComPtr<ID3D12Device8> real8_;
    ComPtr<IDXGIAdapter1> adapter_;

    std::mutex proxy_mtx_;
    ComPtr<ID3D12CommandQueue> direct_queue_; // Always stores unwrapped raw ID3D12CommandQueue (avoids COM cycles & DXGI issues)
    FramePacerState pacer_;
    bool native_uma_ = false;
    bool gpu_upload_heap_supported_ = false;
    bool uma_emulation_enabled_ = true;
    std::atomic<bool> traced_uma_promote_{false};
    std::shared_ptr<wsx_d3d12::GuestTextureManager> guest_tex_mgr_ =
        std::make_shared<wsx_d3d12::GuestTextureManager>();
    wsx_pso_cache::PsoLibraryCache pso_cache_;
    wsx_hud::WsxDebugHudOverlay hud_overlay_;

    template <typename CreateFn>
    HRESULT wrap_texture2d_if_needed(
        D3D12_RESOURCE_DIMENSION dim,
        UINT sample_count,
        D3D12_TEXTURE_LAYOUT orig_layout,
        D3D12_RESOURCE_STATES initial_state,
        REFIID riid,
        void** ppvResource,
        CreateFn&& create_fn) {
        if ((dim != D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
             dim != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
             dim != D3D12_RESOURCE_DIMENSION_TEXTURE3D) ||
            sample_count > 1u || !ppvResource) {
            return create_fn(riid, ppvResource);
        }
        *ppvResource = nullptr;
        ComPtr<ID3D12Resource> raw_res;
        const HRESULT hr = create_fn(__uuidof(ID3D12Resource), reinterpret_cast<void**>(raw_res.GetAddressOf()));
        if (FAILED(hr) || !raw_res) {
            return hr;
        }
        const uint32_t swizzle = wsx_d3d12::infer_swizzle_mode_from_xbox_layout(orig_layout);
        auto* proxy = new (std::nothrow) wsx_d3d12::WsxD3D12ResourceProxy(
            this, raw_res.Get(), guest_tex_mgr_, swizzle, initial_state);
        if (!proxy) {
            return E_OUTOFMEMORY;
        }
        const HRESULT qi_hr = proxy->QueryInterface(riid, ppvResource);
        proxy->Release();
        return qi_hr;
    }

    // Build UMA-compatible candidate heap properties for Xbox DEFAULT / CUSTOM-L1 buffers.
    // Returns number of candidate heap descriptors to try before falling back to original.
    UINT build_uma_candidates(
        const D3D12_HEAP_PROPERTIES& src,
        D3D12_HEAP_FLAGS clean_heap_flags,
        D3D12_RESOURCE_DIMENSION dim,
        D3D12_RESOURCE_FLAGS clean_res_flags,
        D3D12_RESOURCE_STATES clean_initial_state,
        D3D12_HEAP_PROPERTIES out_candidates[2]) const noexcept {
        UINT count = 0;

        // 1. Xbox CUSTOM heap on L1 with CPU access (or L1 on native UMA where L1 is invalid):
        //    Desktop D3D12 forbids CPU-visible L1; promote to GPU_UPLOAD (for WRITE_COMBINE) or CUSTOM L0.
        if (src.Type == D3D12_HEAP_TYPE_CUSTOM && src.MemoryPoolPreference == D3D12_MEMORY_POOL_L1) {
            const bool cpu_visible =
                src.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE ||
                src.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
            if (cpu_visible && uma_emulation_enabled_) {
                if (gpu_upload_heap_supported_ &&
                    src.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE &&
                    (clean_res_flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) == 0 &&
                    clean_initial_state != D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) {
                    D3D12_HEAP_PROPERTIES gpu_up{};
                    gpu_up.Type = D3D12_HEAP_TYPE_GPU_UPLOAD;
                    gpu_up.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
                    gpu_up.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
                    gpu_up.CreationNodeMask = src.CreationNodeMask;
                    gpu_up.VisibleNodeMask = src.VisibleNodeMask;
                    out_candidates[count++] = gpu_up;
                }
                D3D12_HEAP_PROPERTIES l0 = src;
                l0.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
                out_candidates[count++] = l0;
                return count;
            }
            if (native_uma_) {
                D3D12_HEAP_PROPERTIES l0 = src;
                l0.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
                out_candidates[count++] = l0;
                return count;
            }
            return 0;
        }

        // 2. Xbox DEFAULT buffers: on Xbox Series X|S UMA, DEFAULT buffers can be CPU-mapped directly.
        //    Promote regular DEFAULT buffers to GPU_UPLOAD (Resizable BAR) or CUSTOM (WRITE_COMBINE, L0)
        //    so direct Map()/Unmap() works on desktop Windows while keeping all DEFAULT buffer states valid.
        if (uma_emulation_enabled_ &&
            src.Type == D3D12_HEAP_TYPE_DEFAULT &&
            dim == D3D12_RESOURCE_DIMENSION_BUFFER &&
            (clean_res_flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)) == 0 &&
            clean_initial_state != D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE &&
            (clean_heap_flags & (D3D12_HEAP_FLAG_SHARED |
                                 D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER |
                                 D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT)) == 0) {
            if (gpu_upload_heap_supported_) {
                D3D12_HEAP_PROPERTIES gpu_up{};
                gpu_up.Type = D3D12_HEAP_TYPE_GPU_UPLOAD;
                gpu_up.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
                gpu_up.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
                gpu_up.CreationNodeMask = src.CreationNodeMask;
                gpu_up.VisibleNodeMask = src.VisibleNodeMask;
                out_candidates[count++] = gpu_up;
            }
            D3D12_HEAP_PROPERTIES custom_l0{};
            custom_l0.Type = D3D12_HEAP_TYPE_CUSTOM;
            custom_l0.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
            custom_l0.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
            custom_l0.CreationNodeMask = src.CreationNodeMask;
            custom_l0.VisibleNodeMask = src.VisibleNodeMask;
            out_candidates[count++] = custom_l0;
        }
        return count;
    }

    template <typename CreateFn>
    HRESULT try_uma_committed_create(
        const D3D12_HEAP_PROPERTIES* pHeapProperties,
        D3D12_HEAP_FLAGS clean_heap_flags,
        D3D12_RESOURCE_DIMENSION dim,
        D3D12_RESOURCE_FLAGS clean_res_flags,
        D3D12_RESOURCE_STATES clean_initial_state,
        CreateFn&& fn) noexcept {
        if (pHeapProperties) {
            D3D12_HEAP_PROPERTIES candidates[2]{};
            const UINT count = build_uma_candidates(
                *pHeapProperties, clean_heap_flags, dim, clean_res_flags, clean_initial_state, candidates);
            for (UINT i = 0; i < count; ++i) {
                const HRESULT hr = fn(&candidates[i]);
                if (SUCCEEDED(hr)) {
                    if (!traced_uma_promote_.exchange(true, std::memory_order_relaxed)) {
                        wsx::trace_void("d3d12_xs", "WsxD3D12DeviceProxy::UMA_Promote",
                                        "srcType=%u -> dstType=%u pool=%u cpuPage=%u",
                                        static_cast<unsigned>(pHeapProperties->Type),
                                        static_cast<unsigned>(candidates[i].Type),
                                        static_cast<unsigned>(candidates[i].MemoryPoolPreference),
                                        static_cast<unsigned>(candidates[i].CPUPageProperty));
                    }
                    return hr;
                }
            }
        }
        return fn(pHeapProperties);
    }

    template <typename CreateHeapFn>
    HRESULT try_uma_heap_create(D3D12_HEAP_DESC clean_desc, CreateHeapFn&& fn) noexcept {
        const D3D12_HEAP_PROPERTIES src = clean_desc.Properties;
        const bool is_buffer_only_heap =
            (clean_desc.Flags & D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS) == D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        D3D12_HEAP_PROPERTIES candidates[2]{};
        const UINT count = build_uma_candidates(
            src,
            clean_desc.Flags,
            is_buffer_only_heap ? D3D12_RESOURCE_DIMENSION_BUFFER : D3D12_RESOURCE_DIMENSION_UNKNOWN,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COMMON,
            candidates);
        for (UINT i = 0; i < count; ++i) {
            D3D12_HEAP_DESC cand_desc = clean_desc;
            cand_desc.Properties = candidates[i];
            const HRESULT hr = fn(&cand_desc);
            if (SUCCEEDED(hr)) {
                return hr;
            }
        }
        return fn(&clean_desc);
    }

public:
    WsxD3D12DeviceProxy(ID3D12Device* raw_dev, IDXGIAdapter1* adapter) noexcept
        : real0_(raw_dev), adapter_(adapter) {
        real0_.As(&real1_);
        real0_.As(&real2_);
        real0_.As(&real3_);
        real0_.As(&real4_);
        real0_.As(&real5_);
        real0_.As(&real6_);
        real0_.As(&real7_);
        real0_.As(&real8_);

        D3D12_FEATURE_DATA_ARCHITECTURE1 arch{};
        arch.NodeIndex = 0;
        if (SUCCEEDED(real0_->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &arch, sizeof(arch)))) {
            native_uma_ = (arch.UMA != FALSE);
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 opts1{};
        if (SUCCEEDED(real0_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opts1, sizeof(opts1)))) {
            wsx_sm66::set_host_wave_lane_max(opts1.WaveLaneCountMax);
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS16 opts16{};
        if (SUCCEEDED(real0_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &opts16, sizeof(opts16)))) {
            gpu_upload_heap_supported_ = (opts16.GPUUploadHeapSupported != FALSE);
        }
        uma_emulation_enabled_ = wsx::get_config_bool("WSX_UMA_EMULATION", "Graphics", "uma_emulation", true);
        wsx::trace_void("d3d12_xs", "WsxD3D12DeviceProxy::ctor",
                        "nativeUMA=%d gpuUploadHeap=%d umaEmulation=%d",
                        native_uma_ ? 1 : 0,
                        gpu_upload_heap_supported_ ? 1 : 0,
                        uma_emulation_enabled_ ? 1 : 0);
    }

    ~WsxD3D12DeviceProxy() noexcept {
        // Detach and destroy any PresentContext entries bound to this device BEFORE DLL_PROCESS_DETACH,
        // preventing D3D12Core.dll/DXGI COM Release() calls under the OS Loader Lock at process exit.
        std::vector<std::unique_ptr<PresentContext>> to_destroy;
        {
            std::lock_guard<std::mutex> plock(g_present_mutex);
            for (auto it = g_present_contexts.begin(); it != g_present_contexts.end();) {
                if (it->second && it->second->device.Get() == real0_.Get()) {
                    to_destroy.push_back(std::move(it->second));
                    it = g_present_contexts.erase(it);
                } else {
                    ++it;
                }
            }
        }
        to_destroy.clear();
    }

    ID3D12Device* raw_device() const noexcept { return real0_.Get(); }
    FramePacerState& pacer() noexcept { return pacer_; }
    const std::shared_ptr<wsx_d3d12::GuestTextureManager>& guest_texture_manager() const noexcept {
        return guest_tex_mgr_;
    }

    void set_frame_interval(UINT interval, UINT max_latency, UINT flags) noexcept {
        std::lock_guard<std::mutex> lock(proxy_mtx_);
        pacer_.update_interval(interval, max_latency, flags);
        if (direct_queue_) {
            std::lock_guard<std::mutex> plock(g_present_mutex);
            auto it = g_present_contexts.find(direct_queue_.Get());
            if (it != g_present_contexts.end() && it->second) {
                it->second->vrr_enabled =
                    (flags & 1u) != 0 || interval == 0u ||
                    wsx::get_config_bool("WSX_VRR", "Graphics", "vrr", false);
                if (it->second->swap_chain && it->second->frame_latency_waitable_object) {
                    it->second->swap_chain->SetMaximumFrameLatency(pacer_.max_latency);
                }
            }
        }
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IWsxD3D12DeviceProxyMarker)) {
            *ppvObject = this;
            return S_OK; // Non-AddRef internal marker query
        }
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(ID3D12Object) ||
            riid == __uuidof(ID3D12Device) ||
            (riid == __uuidof(ID3D12Device1) && real1_) ||
            (riid == __uuidof(ID3D12Device2) && real2_) ||
            (riid == __uuidof(ID3D12Device3) && real3_) ||
            (riid == __uuidof(ID3D12Device4) && real4_) ||
            (riid == __uuidof(ID3D12Device5) && real5_) ||
            (riid == __uuidof(ID3D12Device6) && real6_) ||
            (riid == __uuidof(ID3D12Device7) && real7_) ||
            (riid == __uuidof(ID3D12Device8) && real8_)) {
            *ppvObject = static_cast<ID3D12Device8*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IDXGIDevice1) || riid == __uuidof(IDXGIDevice)) {
            auto* wrapper = new (std::nothrow) WsxDxgiDevice1(this, adapter_.Get());
            if (!wrapper) {
                *ppvObject = nullptr;
                return write_result("WsxD3D12DeviceProxy::QueryInterface(IDXGIDevice1)", E_OUTOFMEMORY);
            }
            *ppvObject = static_cast<IDXGIDevice1*>(wrapper);
            return write_result("WsxD3D12DeviceProxy::QueryInterface(IDXGIDevice1)", S_OK);
        }
        return real0_->QueryInterface(riid, ppvObject);
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++ref_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) {
            delete this;
        }
        return r;
    }

    // ID3D12Object
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
        return real0_->GetPrivateData(guid, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
        return real0_->SetPrivateData(guid, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
        return real0_->SetPrivateDataInterface(guid, pData);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override {
        return real0_->SetName(Name);
    }

    // ID3D12Device
    UINT STDMETHODCALLTYPE GetNodeCount() override {
        return real0_->GetNodeCount();
    }

    HRESULT STDMETHODCALLTYPE CreateCommandQueue(
        const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid, void** ppCommandQueue) override {
        if (!ppCommandQueue) return E_POINTER;
        *ppCommandQueue = nullptr;

        D3D12_COMMAND_QUEUE_DESC clean_desc{};
        const D3D12_COMMAND_QUEUE_DESC* use_desc = pDesc;
        if (pDesc) {
            clean_desc = *pDesc;
            clean_desc.Type = wsx_d3d12::remap_xbox_command_list_type(clean_desc.Type);
            use_desc = &clean_desc;
        }

        ComPtr<ID3D12CommandQueue> raw_q;
        const HRESULT hr = real0_->CreateCommandQueue(use_desc, IID_PPV_ARGS(raw_q.GetAddressOf()));
        if (FAILED(hr)) {
            return hr;
        }
        if (use_desc && use_desc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            std::lock_guard<std::mutex> lock(proxy_mtx_);
            direct_queue_ = raw_q;
            std::lock_guard<std::mutex> plock(g_present_mutex);
            PresentContext* ctx = get_or_create_present_context_locked(raw_q.Get(), real0_.Get());
            if (ctx) {
                ctx->vrr_enabled =
                    (pacer_.flags & 1u) != 0 || pacer_.interval_enum == 0u ||
                    wsx::get_config_bool("WSX_VRR", "Graphics", "vrr", false);
            }
        }
        auto* proxy = new (std::nothrow) wsx_d3d12::WsxD3D12CommandQueueProxy(
            this, real0_.Get(), raw_q.Get(), guest_tex_mgr_);
        if (!proxy) {
            return E_OUTOFMEMORY;
        }
        const HRESULT qi_hr = proxy->QueryInterface(riid, ppCommandQueue);
        proxy->Release();
        return qi_hr;
    }

    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** ppCommandAllocator) override {
        return real0_->CreateCommandAllocator(wsx_d3d12::remap_xbox_command_list_type(type), riid, ppCommandAllocator);
    }

    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) override {
        if (!pDesc) return real0_->CreateGraphicsPipelineState(pDesc, riid, ppPipelineState);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC local = *pDesc;
        std::vector<D3D12_INPUT_ELEMENT_DESC> patched_ia;
        if (local.InputLayout.NumElements > 0u && local.InputLayout.pInputElementDescs) {
            bool needs_ia_patch = false;
            for (UINT i = 0u; i < local.InputLayout.NumElements; ++i) {
                if (wsx_d3d12::remap_xbox_vertex_format(local.InputLayout.pInputElementDescs[i].Format) !=
                    local.InputLayout.pInputElementDescs[i].Format) {
                    needs_ia_patch = true;
                    break;
                }
            }
            if (needs_ia_patch) {
                patched_ia.assign(local.InputLayout.pInputElementDescs,
                                  local.InputLayout.pInputElementDescs + local.InputLayout.NumElements);
                for (auto& el : patched_ia) {
                    el.Format = wsx_d3d12::remap_xbox_vertex_format(el.Format);
                }
                local.InputLayout.pInputElementDescs = patched_ia.data();
            }
        }
        std::vector<uint8_t> s_vs, s_ps, s_ds, s_hs, s_gs;
        HRESULT hr = wsx_shader::prepare_shader_for_desktop_d3d12(pDesc->VS, local.VS, s_vs);
        if (FAILED(hr)) return hr;
        hr = wsx_shader::prepare_shader_for_desktop_d3d12(pDesc->PS, local.PS, s_ps);
        if (FAILED(hr)) return hr;
        hr = wsx_shader::prepare_shader_for_desktop_d3d12(pDesc->DS, local.DS, s_ds);
        if (FAILED(hr)) return hr;
        hr = wsx_shader::prepare_shader_for_desktop_d3d12(pDesc->HS, local.HS, s_hs);
        if (FAILED(hr)) return hr;
        hr = wsx_shader::prepare_shader_for_desktop_d3d12(pDesc->GS, local.GS, s_gs);
        if (FAILED(hr)) return hr;
        return pso_cache_.load_or_create_graphics(
            real0_.Get(), real1_.Get(), adapter_.Get(), local, riid, ppPipelineState);
    }

    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(
        const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) override {
        if (!pDesc) return real0_->CreateComputePipelineState(pDesc, riid, ppPipelineState);
        D3D12_COMPUTE_PIPELINE_STATE_DESC local = *pDesc;
        std::vector<uint8_t> s_cs;
        const HRESULT hr = wsx_shader::prepare_shader_for_desktop_d3d12(pDesc->CS, local.CS, s_cs);
        if (FAILED(hr)) return hr;
        return pso_cache_.load_or_create_compute(
            real0_.Get(), real1_.Get(), adapter_.Get(), local, riid, ppPipelineState);
    }

    HRESULT STDMETHODCALLTYPE CreateCommandList(
        UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pCommandAllocator,
        ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList) override {
        if (!ppCommandList) return E_POINTER;
        *ppCommandList = nullptr;

        const D3D12_COMMAND_LIST_TYPE clean_type = wsx_d3d12::remap_xbox_command_list_type(type);
        ComPtr<ID3D12GraphicsCommandList> raw_list;
        const HRESULT hr = real0_->CreateCommandList(
            nodeMask, clean_type, pCommandAllocator, pInitialState, IID_PPV_ARGS(raw_list.GetAddressOf()));
        if (FAILED(hr)) {
            return real0_->CreateCommandList(nodeMask, clean_type, pCommandAllocator, pInitialState, riid, ppCommandList);
        }
        auto* proxy = new (std::nothrow) wsx_d3d12::WsxD3D12CommandListProxy(this, raw_list.Get());
        if (!proxy) {
            return E_OUTOFMEMORY;
        }
        const HRESULT qi_hr = proxy->QueryInterface(riid, ppCommandList);
        proxy->Release();
        return qi_hr;
    }

    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(
        D3D12_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) override {
        return real0_->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize);
    }

    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(
        const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc, REFIID riid, void** ppvHeap) override {
        if (pDescriptorHeapDesc &&
            wsx_d3d12::remap_xbox_descriptor_heap_type(pDescriptorHeapDesc->Type) != pDescriptorHeapDesc->Type) {
            D3D12_DESCRIPTOR_HEAP_DESC clean = *pDescriptorHeapDesc;
            clean.Type = wsx_d3d12::remap_xbox_descriptor_heap_type(clean.Type);
            return real0_->CreateDescriptorHeap(&clean, riid, ppvHeap);
        }
        return real0_->CreateDescriptorHeap(pDescriptorHeapDesc, riid, ppvHeap);
    }

    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) override {
        return real0_->GetDescriptorHandleIncrementSize(
            wsx_d3d12::remap_xbox_descriptor_heap_type(DescriptorHeapType));
    }

    HRESULT STDMETHODCALLTYPE CreateRootSignature(
        UINT nodeMask, const void* pBlobWithRootSignature, SIZE_T blobLengthInBytes,
        REFIID riid, void** ppvRootSignature) override {
        std::vector<uint8_t> patched_blob;
        const void* use_blob = pBlobWithRootSignature;
        SIZE_T use_len = blobLengthInBytes;
        uint32_t stripped_xbox_flags = 0u;
        if (sanitize_xbox_root_signature_blob(pBlobWithRootSignature, blobLengthInBytes, patched_blob, &stripped_xbox_flags)) {
            use_blob = patched_blob.data();
            use_len = patched_blob.size();
        }
        const HRESULT hr = real0_->CreateRootSignature(
            nodeMask, use_blob, use_len, riid, ppvRootSignature);
        if (SUCCEEDED(hr) && ppvRootSignature && *ppvRootSignature &&
            use_blob && use_len > 0u) {
            ComPtr<ID3D12RootSignature> rs;
            if (SUCCEEDED(static_cast<IUnknown*>(*ppvRootSignature)->QueryInterface(IID_PPV_ARGS(&rs))) && rs) {
                wsx_pso_cache::attach_root_signature_hash(rs.Get(), use_blob, use_len);
            }
        }
        return hr;
    }

    void STDMETHODCALLTYPE CreateConstantBufferView(
        const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        real0_->CreateConstantBufferView(pDesc, DestDescriptor);
    }

    void STDMETHODCALLTYPE CreateShaderResourceView(
        ID3D12Resource* pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        ID3D12Resource* raw = wsx_d3d12::unwrap_resource(pResource);
        if (raw && pDesc && pDesc->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP &&
            raw->GetDesc().Format == DXGI_FORMAT_R32_TYPELESS) {
            D3D12_SHADER_RESOURCE_VIEW_DESC clean = *pDesc;
            clean.Format = DXGI_FORMAT_R32_UINT;
            real0_->CreateShaderResourceView(raw, &clean, DestDescriptor);
            return;
        }
        real0_->CreateShaderResourceView(raw, pDesc, DestDescriptor);
    }

    void STDMETHODCALLTYPE CreateUnorderedAccessView(
        ID3D12Resource* pResource, ID3D12Resource* pCounterResource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        ID3D12Resource* raw = wsx_d3d12::unwrap_resource(pResource);
        ID3D12Resource* raw_cnt = wsx_d3d12::unwrap_resource(pCounterResource);
        if (pDesc && pDesc->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC clean = *pDesc;
            clean.Format = DXGI_FORMAT_R32_UINT;
            real0_->CreateUnorderedAccessView(raw, raw_cnt, &clean, DestDescriptor);
            return;
        }
        real0_->CreateUnorderedAccessView(raw, raw_cnt, pDesc, DestDescriptor);
    }

    void STDMETHODCALLTYPE CreateRenderTargetView(
        ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        real0_->CreateRenderTargetView(wsx_d3d12::unwrap_resource(pResource), pDesc, DestDescriptor);
    }

    void STDMETHODCALLTYPE CreateDepthStencilView(
        ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        real0_->CreateDepthStencilView(wsx_d3d12::unwrap_resource(pResource), pDesc, DestDescriptor);
    }

    void STDMETHODCALLTYPE CreateSampler(
        const D3D12_SAMPLER_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        real0_->CreateSampler(pDesc, DestDescriptor);
    }

    void STDMETHODCALLTYPE CopyDescriptors(
        UINT NumDestDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
        const UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
        const UINT* pSrcDescriptorRangeSizes, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override {
        real0_->CopyDescriptors(NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
                                NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes,
                                DescriptorHeapsType);
    }

    void STDMETHODCALLTYPE CopyDescriptorsSimple(
        UINT NumDescriptors, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
        D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
        D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override {
        real0_->CopyDescriptorsSimple(NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart,
                                      DescriptorHeapsType);
    }

    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(
        UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs) override {
        if (!pResourceDescs || numResourceDescs == 0) {
            return real0_->GetResourceAllocationInfo(visibleMask, numResourceDescs, pResourceDescs);
        }
        std::vector<D3D12_RESOURCE_DESC> clean(pResourceDescs, pResourceDescs + numResourceDescs);
        for (auto& d : clean) wsx_d3d12::sanitize_xbox_resource_desc_inplace(d);
        return real0_->GetResourceAllocationInfo(visibleMask, numResourceDescs, clean.data());
    }

    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(
        UINT nodeMask, D3D12_HEAP_TYPE heapType) override {
        return real0_->GetCustomHeapProperties(nodeMask, heapType);
    }

    HRESULT STDMETHODCALLTYPE CreateCommittedResource(
        const D3D12_HEAP_PROPERTIES* pHeapProperties,
        D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC* pDesc,
        D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        REFIID riidResource,
        void** ppvResource) override {
        if (!pDesc) return E_INVALIDARG;

        const D3D12_HEAP_FLAGS clean_heap_flags =
            static_cast<D3D12_HEAP_FLAGS>(HeapFlags & ~kXboxHeapFlagsMask);
        D3D12_RESOURCE_DESC clean_desc = *pDesc;
        wsx_d3d12::sanitize_xbox_resource_desc_inplace(clean_desc);

        // Zero-Copy SwapChain Backbuffer Hand-off:
        // When an Xbox Series X|S title allocates a displayable render target with D3D12_HEAP_FLAG_ALLOW_DISPLAY,
        // hand out the DXGI SwapChain's own backbuffer resource directly so PresentX requires zero CopyResource!
        if ((HeapFlags & D3D12_HEAP_FLAG_ALLOW_DISPLAY) != 0 &&
            (clean_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 &&
            clean_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            ppvResource && !env_flag_enabled(L"WSX_DISABLE_ZERO_COPY", false) &&
            wsx::get_config_bool("", "Graphics", "zero_copy", true)) {
            ID3D12CommandQueue* q = nullptr;
            UINT target_buffers = 2u;
            {
                std::lock_guard<std::mutex> lock(proxy_mtx_);
                q = direct_queue_.Get();
                target_buffers = std::clamp(pacer_.max_latency + 1u, 2u, kMaxSwapChainBuffers);
            }
            if (q) {
                std::lock_guard<std::mutex> plock(g_present_mutex);
                PresentContext* ctx = get_or_create_present_context_locked(q, real0_.Get());
                if (ctx && ensure_swapchain_locked(
                        q, *ctx, static_cast<UINT>(clean_desc.Width), clean_desc.Height,
                        clean_desc.Format, target_buffers, &pacer_)) {
                    if (ctx->next_display_buffer_index < ctx->buffer_count) {
                        const UINT idx = ctx->next_display_buffer_index++;
                        const HRESULT sc_hr = ctx->swap_chain->GetBuffer(idx, riidResource, ppvResource);
                        if (SUCCEEDED(sc_hr)) {
                            return write_result("WsxD3D12DeviceProxy::CreateCommittedResource(ZeroCopySwapChain)",
                                                sc_hr, static_cast<UINT>(HeapFlags));
                        }
                    }
                }
            }
        }

        const D3D12_RESOURCE_STATES clean_initial_state =
            wsx_d3d12::sanitize_xbox_initial_state_for_heap(pHeapProperties, InitialResourceState);

        const HRESULT hr = wrap_texture2d_if_needed(
            clean_desc.Dimension, clean_desc.SampleDesc.Count, pDesc->Layout,
            clean_initial_state, riidResource, ppvResource,
            [&](REFIID target_riid, void** target_ppv) {
                return try_uma_committed_create(
                    pHeapProperties, clean_heap_flags, clean_desc.Dimension, clean_desc.Flags, clean_initial_state,
                    [&](const D3D12_HEAP_PROPERTIES* hp) {
                        return real0_->CreateCommittedResource(
                            hp, clean_heap_flags, &clean_desc,
                            clean_initial_state, pOptimizedClearValue, target_riid, target_ppv);
                    });
            });
        return write_result("WsxD3D12DeviceProxy::CreateCommittedResource", hr, static_cast<UINT>(HeapFlags));
    }

    HRESULT STDMETHODCALLTYPE CreateHeap(
        const D3D12_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override {
        if (!pDesc) return E_INVALIDARG;
        D3D12_HEAP_DESC clean_desc = *pDesc;
        clean_desc.Flags = static_cast<D3D12_HEAP_FLAGS>(clean_desc.Flags & ~kXboxHeapFlagsMask);
        return try_uma_heap_create(clean_desc, [&](const D3D12_HEAP_DESC* d) {
            return real0_->CreateHeap(d, riid, ppvHeap);
        });
    }

    HRESULT STDMETHODCALLTYPE CreatePlacedResource(
        ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC* pDesc,
        D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        REFIID riid, void** ppvResource) override {
        if (!pDesc) return E_INVALIDARG;
        D3D12_RESOURCE_DESC clean_desc = *pDesc;
        wsx_d3d12::sanitize_xbox_resource_desc_inplace(clean_desc);
        D3D12_HEAP_DESC hdesc{};
        const D3D12_HEAP_PROPERTIES* hp_ptr = nullptr;
        if (pHeap) {
            hdesc = pHeap->GetDesc();
            hp_ptr = &hdesc.Properties;
        }
        const D3D12_RESOURCE_STATES clean_state =
            wsx_d3d12::sanitize_xbox_initial_state_for_heap(hp_ptr, InitialState);
        return wrap_texture2d_if_needed(
            clean_desc.Dimension, clean_desc.SampleDesc.Count, pDesc->Layout,
            clean_state, riid, ppvResource,
            [&](REFIID target_riid, void** target_ppv) {
                return real0_->CreatePlacedResource(pHeap, HeapOffset, &clean_desc, clean_state,
                                                    pOptimizedClearValue, target_riid, target_ppv);
            });
    }

    HRESULT STDMETHODCALLTYPE CreateReservedResource(
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) override {
        if (!pDesc) return E_INVALIDARG;
        D3D12_RESOURCE_DESC clean_desc = *pDesc;
        wsx_d3d12::sanitize_xbox_resource_desc_inplace(clean_desc);
        const D3D12_RESOURCE_STATES clean_state = wsx_d3d12::sanitize_xbox_resource_state(InitialState);
        return real0_->CreateReservedResource(&clean_desc, clean_state, pOptimizedClearValue, riid, ppvResource);
    }

    HRESULT STDMETHODCALLTYPE CreateSharedHandle(
        ID3D12DeviceChild* pObject, const SECURITY_ATTRIBUTES* pAttributes,
        DWORD Access, LPCWSTR Name, HANDLE* pHandle) override {
        if (auto* rp = wsx_d3d12::WsxD3D12ResourceProxy::from_interface(pObject)) {
            pObject = rp->raw_resource();
        }
        return real0_->CreateSharedHandle(pObject, pAttributes, Access, Name, pHandle);
    }

    HRESULT STDMETHODCALLTYPE OpenSharedHandle(
        HANDLE NTHandle, REFIID riid, void** ppvObj) override {
        return real0_->OpenSharedHandle(NTHandle, riid, ppvObj);
    }

    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(
        LPCWSTR Name, DWORD Access, HANDLE* pNTHandle) override {
        return real0_->OpenSharedHandleByName(Name, Access, pNTHandle);
    }

    HRESULT STDMETHODCALLTYPE MakeResident(
        UINT NumObjects, ID3D12Pageable* const* ppObjects) override {
        if (NumObjects == 0 || !ppObjects) return real0_->MakeResident(NumObjects, ppObjects);
        std::vector<ID3D12Pageable*> unwrapped(NumObjects);
        for (UINT i = 0; i < NumObjects; ++i) unwrapped[i] = wsx_d3d12::unwrap_pageable(ppObjects[i]);
        return real0_->MakeResident(NumObjects, unwrapped.data());
    }

    HRESULT STDMETHODCALLTYPE Evict(
        UINT NumObjects, ID3D12Pageable* const* ppObjects) override {
        if (NumObjects == 0 || !ppObjects) return real0_->Evict(NumObjects, ppObjects);
        std::vector<ID3D12Pageable*> unwrapped(NumObjects);
        for (UINT i = 0; i < NumObjects; ++i) unwrapped[i] = wsx_d3d12::unwrap_pageable(ppObjects[i]);
        return real0_->Evict(NumObjects, unwrapped.data());
    }

    HRESULT STDMETHODCALLTYPE CreateFence(
        UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void** ppFence) override {
        return real0_->CreateFence(InitialValue, Flags, riid, ppFence);
    }

    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override {
        return real0_->GetDeviceRemovedReason();
    }

    void STDMETHODCALLTYPE GetCopyableFootprints(
        const D3D12_RESOURCE_DESC* pResourceDesc, UINT FirstSubresource, UINT NumSubresources,
        UINT64 BaseOffset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts, UINT* pNumRows,
        UINT64* pRowSizeInBytes, UINT64* pTotalBytes) override {
        if (pResourceDesc) {
            D3D12_RESOURCE_DESC clean = *pResourceDesc;
            wsx_d3d12::sanitize_xbox_resource_desc_inplace(clean);
            real0_->GetCopyableFootprints(&clean, FirstSubresource, NumSubresources, BaseOffset,
                                          pLayouts, pNumRows, pRowSizeInBytes, pTotalBytes);
            return;
        }
        real0_->GetCopyableFootprints(pResourceDesc, FirstSubresource, NumSubresources, BaseOffset,
                                      pLayouts, pNumRows, pRowSizeInBytes, pTotalBytes);
    }

    HRESULT STDMETHODCALLTYPE CreateQueryHeap(
        const D3D12_QUERY_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override {
        return real0_->CreateQueryHeap(pDesc, riid, ppvHeap);
    }

    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL Enable) override {
        return real0_->SetStablePowerState(Enable);
    }

    HRESULT STDMETHODCALLTYPE CreateCommandSignature(
        const D3D12_COMMAND_SIGNATURE_DESC* pDesc, ID3D12RootSignature* pRootSignature,
        REFIID riid, void** ppvCommandSignature) override {
        return real0_->CreateCommandSignature(pDesc, pRootSignature, riid, ppvCommandSignature);
    }

    void STDMETHODCALLTYPE GetResourceTiling(
        ID3D12Resource* pTiledResource, UINT* pNumTilesForEntireResource,
        D3D12_PACKED_MIP_INFO* pPackedMipDesc, D3D12_TILE_SHAPE* pStandardTileShapeForNonPackedMips,
        UINT* pNumSubresourceTilings, UINT FirstSubresourceTilingToGet,
        D3D12_SUBRESOURCE_TILING* pSubresourceTilingsForNonPackedMips) override {
        real0_->GetResourceTiling(wsx_d3d12::unwrap_resource(pTiledResource), pNumTilesForEntireResource, pPackedMipDesc,
                                  pStandardTileShapeForNonPackedMips, pNumSubresourceTilings,
                                  FirstSubresourceTilingToGet, pSubresourceTilingsForNonPackedMips);
    }

    LUID STDMETHODCALLTYPE GetAdapterLuid() override {
        return real0_->GetAdapterLuid();
    }

    // ID3D12Device1
    HRESULT STDMETHODCALLTYPE CreatePipelineLibrary(
        const void* pLibraryBlob, SIZE_T BlobLength, REFIID riid, void** ppPipelineLibrary) override {
        return real1_ ? real1_->CreatePipelineLibrary(pLibraryBlob, BlobLength, riid, ppPipelineLibrary) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE SetEventOnMultipleFenceCompletion(
        ID3D12Fence* const* ppFences, const UINT64* pFenceValues, UINT NumFences,
        D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags, HANDLE hEvent) override {
        return real1_ ? real1_->SetEventOnMultipleFenceCompletion(ppFences, pFenceValues, NumFences, Flags, hEvent) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE SetResidencyPriority(
        UINT NumObjects, ID3D12Pageable* const* ppObjects, const D3D12_RESIDENCY_PRIORITY* pPriorities) override {
        if (!real1_) return E_NOINTERFACE;
        if (NumObjects == 0 || !ppObjects) return real1_->SetResidencyPriority(NumObjects, ppObjects, pPriorities);
        std::vector<ID3D12Pageable*> unwrapped(NumObjects);
        for (UINT i = 0; i < NumObjects; ++i) unwrapped[i] = wsx_d3d12::unwrap_pageable(ppObjects[i]);
        return real1_->SetResidencyPriority(NumObjects, unwrapped.data(), pPriorities);
    }

    // ID3D12Device2
    HRESULT STDMETHODCALLTYPE CreatePipelineState(
        const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc, REFIID riid, void** ppPipelineState) override {
        if (!real2_) return E_NOINTERFACE;
        if (!pDesc || !pDesc->pPipelineStateSubobjectStream || pDesc->SizeInBytes < sizeof(void*)) {
            return real2_->CreatePipelineState(pDesc, riid, ppPipelineState);
        }

        // Inspect & optionally substitute VS/PS/DS/HS/GS/CS/AS/MS subobjects in the stream
        std::vector<uint8_t> stream_copy(
            static_cast<const uint8_t*>(pDesc->pPipelineStateSubobjectStream),
            static_cast<const uint8_t*>(pDesc->pPipelineStateSubobjectStream) + pDesc->SizeInBytes);
        std::vector<std::vector<uint8_t>> owned_shaders;
        bool modified = false;

        auto align_ptr = [](size_t off) noexcept -> size_t {
            return (off + sizeof(void*) - 1u) & ~(sizeof(void*) - 1u);
        };
        auto subobj_stride = [](D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t) noexcept -> size_t {
#pragma warning(push)
#pragma warning(disable : 4324)
            struct alignas(void*) S_RS { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; ID3D12RootSignature* v; };
            struct alignas(void*) S_BC { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_SHADER_BYTECODE v; };
            struct alignas(void*) S_SO { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_STREAM_OUTPUT_DESC v; };
            struct alignas(void*) S_BL { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_BLEND_DESC v; };
            struct alignas(void*) S_U  { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; UINT v; };
            struct alignas(void*) S_RA { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_RASTERIZER_DESC v; };
            struct alignas(void*) S_DS { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_DEPTH_STENCIL_DESC v; };
            struct alignas(void*) S_IL { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_INPUT_LAYOUT_DESC v; };
            struct alignas(void*) S_RT { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_RT_FORMAT_ARRAY v; };
            struct alignas(void*) S_SD { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; DXGI_SAMPLE_DESC v; };
            struct alignas(void*) S_CP { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_CACHED_PIPELINE_STATE v; };
            struct alignas(void*) S_D1 { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_DEPTH_STENCIL_DESC1 v; };
            struct alignas(void*) S_VI { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_VIEW_INSTANCING_DESC v; };
#pragma warning(pop)
            switch (t) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return sizeof(S_RS);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:             return sizeof(S_BC);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:  return sizeof(S_SO);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:          return sizeof(S_BL);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:          return sizeof(S_U);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:     return sizeof(S_RA);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:  return sizeof(S_DS);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:   return sizeof(S_IL);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return sizeof(S_RT);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:    return sizeof(S_SD);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:     return sizeof(S_CP);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: return sizeof(S_D1);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:return sizeof(S_VI);
            default: return 0u;
            }
        };

        size_t pos = 0;
        while (pos + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= stream_copy.size()) {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE stype{};
            std::memcpy(&stype, stream_copy.data() + pos, sizeof(stype));
            const size_t stride = subobj_stride(stype);
            if (stride == 0u || pos + stride > stream_copy.size()) break;

            if (stype == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS ||
                stype == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS ||
                stype == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS ||
                stype == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS ||
                stype == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS ||
                stype == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS ||
                stype == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS ||
                stype == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS) {
                struct alignas(void*) S_BC { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_SHADER_BYTECODE v; };
                auto* entry = reinterpret_cast<S_BC*>(stream_copy.data() + pos);
                std::vector<uint8_t> storage;
                D3D12_SHADER_BYTECODE replaced{};
                const HRESULT shr = wsx_shader::prepare_shader_for_desktop_d3d12(entry->v, replaced, storage);
                if (FAILED(shr)) return shr;
                if (!storage.empty()) {
                    owned_shaders.push_back(std::move(storage));
                    entry->v.pShaderBytecode = owned_shaders.back().data();
                    entry->v.BytecodeLength = owned_shaders.back().size();
                    modified = true;
                }
            }
            pos = align_ptr(pos + stride);
        }

        if (modified) {
            D3D12_PIPELINE_STATE_STREAM_DESC mod_desc{};
            mod_desc.SizeInBytes = stream_copy.size();
            mod_desc.pPipelineStateSubobjectStream = stream_copy.data();
            return pso_cache_.load_or_create_stream(
                real2_.Get(), real1_.Get(), adapter_.Get(), mod_desc, riid, ppPipelineState);
        }
        return pso_cache_.load_or_create_stream(
            real2_.Get(), real1_.Get(), adapter_.Get(), *pDesc, riid, ppPipelineState);
    }

    HRESULT flush_pso_cache() noexcept { return pso_cache_.flush(); }
    IDXGIAdapter1* adapter() const noexcept { return adapter_.Get(); }
    wsx_hud::WsxDebugHudOverlay& hud_overlay() noexcept { return hud_overlay_; }

    wsx_hud::HudMetricsSnapshot build_hud_metrics() noexcept {
        wsx_hud::HudMetricsSnapshot m{};
        const float avg_ms = hud_overlay_.current_avg_frame_ms();
        m.frame_ms = avg_ms;
        m.fps = (avg_ms > 0.01f) ? (1000.0f / avg_ms) : 0.0f;
        wsx_query_frame_pacer_hud_metrics(&m.one_percent_low_fps, &m.p99_frame_ms);
        m.wave64_emulated = (wsx_sm66::get_host_wave_lane_max() < 64u) ? 1u : 0u;

        ComPtr<IDXGIAdapter3> ad3;
        if (adapter_) adapter_->QueryInterface(IID_PPV_ARGS(&ad3));
        if (ad3) {
            DXGI_QUERY_VIDEO_MEMORY_INFO linfo{};
            if (SUCCEEDED(ad3->QueryVideoMemoryInfo(0u, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &linfo))) {
                m.vram_used_mb = static_cast<float>(linfo.CurrentUsage) / (1024.0f * 1024.0f);
                m.vram_budget_mb = static_cast<float>(linfo.Budget) / (1024.0f * 1024.0f);
                if (linfo.Budget > 0u) {
                    const double r = static_cast<double>(linfo.CurrentUsage) / static_cast<double>(linfo.Budget);
                    m.vram_pressure = (r >= 0.92) ? 2u : ((r >= 0.80) ? 1u : 0u);
                }
            }
        }
        const auto& pc = wsx_pso_cache::global_counters();
        m.pso_hits = pc.hits.load(std::memory_order_relaxed);
        m.pso_misses = pc.misses.load(std::memory_order_relaxed);

        // Query xmem.dll tracked bytes dynamically if loaded
        if (HMODULE xm = GetModuleHandleW(L"xmem.dll")) {
            struct XMemStatsMini { UINT64 allocs, tracked, pmc, pmb, peak, r[3]; };
            using GetXMemStatsFn = BOOL(WINAPI*)(XMemStatsMini*);
            if (auto fn = reinterpret_cast<GetXMemStatsFn>(GetProcAddress(xm, "XMemGetAllocationStatistics"))) {
                XMemStatsMini xs{};
                if (fn(&xs)) {
                    m.xmem_mb = (xs.tracked + xs.pmb) / (1024ULL * 1024ULL);
                }
            }
        }

        const auto& ct = wsx_d3d12::global_command_telemetry();
        m.draw_calls = ct.draw_calls.load(std::memory_order_relaxed);
        m.dispatch_calls = ct.dispatch_calls.load(std::memory_order_relaxed);
        m.mesh_dispatches = ct.mesh_dispatches.load(std::memory_order_relaxed);
        m.barriers_elided = ct.barriers_elided.load(std::memory_order_relaxed);
        if (guest_tex_mgr_) {
            const auto gs = guest_tex_mgr_->get_stats();
            m.uma_uploads = gs.textures_uploaded;
            m.uma_ww_skips = gs.write_watch_skips;
        }
        const auto as = wsx_async::AsyncShaderCompileQueue::instance().get_stats();
        m.async_done = as.completed;
        m.async_total = as.enqueued;
        return m;
    }

    // ID3D12Device3
    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress(
        const void* pAddress, REFIID riid, void** ppvHeap) override {
        return real3_ ? real3_->OpenExistingHeapFromAddress(pAddress, riid, ppvHeap) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromFileMapping(
        HANDLE hFileMapping, REFIID riid, void** ppvHeap) override {
        return real3_ ? real3_->OpenExistingHeapFromFileMapping(hFileMapping, riid, ppvHeap) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE EnqueueMakeResident(
        D3D12_RESIDENCY_FLAGS Flags, UINT NumObjects, ID3D12Pageable* const* ppObjects,
        ID3D12Fence* pFenceToSignal, UINT64 FenceValueToSignal) override {
        if (!real3_) return E_NOINTERFACE;
        if (NumObjects == 0 || !ppObjects) {
            return real3_->EnqueueMakeResident(Flags, NumObjects, ppObjects, pFenceToSignal, FenceValueToSignal);
        }
        std::vector<ID3D12Pageable*> unwrapped(NumObjects);
        for (UINT i = 0; i < NumObjects; ++i) unwrapped[i] = wsx_d3d12::unwrap_pageable(ppObjects[i]);
        return real3_->EnqueueMakeResident(Flags, NumObjects, unwrapped.data(), pFenceToSignal, FenceValueToSignal);
    }

    // ID3D12Device4
    HRESULT STDMETHODCALLTYPE CreateCommandList1(
        UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags,
        REFIID riid, void** ppCommandList) override {
        if (!real4_) return E_NOINTERFACE;
        if (!ppCommandList) return E_POINTER;
        *ppCommandList = nullptr;

        const D3D12_COMMAND_LIST_TYPE clean_type = wsx_d3d12::remap_xbox_command_list_type(type);
        ComPtr<ID3D12GraphicsCommandList> raw_list;
        const HRESULT hr = real4_->CreateCommandList1(
            nodeMask, clean_type, flags, IID_PPV_ARGS(raw_list.GetAddressOf()));
        if (FAILED(hr)) {
            return real4_->CreateCommandList1(nodeMask, clean_type, flags, riid, ppCommandList);
        }
        auto* proxy = new (std::nothrow) wsx_d3d12::WsxD3D12CommandListProxy(this, raw_list.Get());
        if (!proxy) {
            return E_OUTOFMEMORY;
        }
        const HRESULT qi_hr = proxy->QueryInterface(riid, ppCommandList);
        proxy->Release();
        return qi_hr;
    }

    HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession(
        const D3D12_PROTECTED_RESOURCE_SESSION_DESC* pDesc, REFIID riid, void** ppSession) override {
        return real4_ ? real4_->CreateProtectedResourceSession(pDesc, riid, ppSession) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE CreateCommittedResource1(
        const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        ID3D12ProtectedResourceSession* pProtectedSession,
        REFIID riidResource, void** ppvResource) override {
        if (!real4_) return E_NOINTERFACE;
        if (!pDesc) return E_INVALIDARG;
        const D3D12_HEAP_FLAGS clean_heap_flags =
            static_cast<D3D12_HEAP_FLAGS>(HeapFlags & ~kXboxHeapFlagsMask);
        D3D12_RESOURCE_DESC clean_desc = *pDesc;
        wsx_d3d12::sanitize_xbox_resource_desc_inplace(clean_desc);
        const D3D12_RESOURCE_STATES clean_initial_state =
            wsx_d3d12::sanitize_xbox_initial_state_for_heap(pHeapProperties, InitialResourceState);
        return wrap_texture2d_if_needed(
            clean_desc.Dimension, clean_desc.SampleDesc.Count, pDesc->Layout,
            clean_initial_state, riidResource, ppvResource,
            [&](REFIID target_riid, void** target_ppv) {
                return try_uma_committed_create(
                    pHeapProperties, clean_heap_flags, clean_desc.Dimension, clean_desc.Flags, clean_initial_state,
                    [&](const D3D12_HEAP_PROPERTIES* hp) {
                        return real4_->CreateCommittedResource1(
                            hp, clean_heap_flags, &clean_desc, clean_initial_state,
                            pOptimizedClearValue, pProtectedSession, target_riid, target_ppv);
                    });
            });
    }

    HRESULT STDMETHODCALLTYPE CreateHeap1(
        const D3D12_HEAP_DESC* pDesc, ID3D12ProtectedResourceSession* pProtectedSession,
        REFIID riid, void** ppvHeap) override {
        if (!real4_) return E_NOINTERFACE;
        if (!pDesc) return E_INVALIDARG;
        D3D12_HEAP_DESC clean_desc = *pDesc;
        clean_desc.Flags = static_cast<D3D12_HEAP_FLAGS>(clean_desc.Flags & ~kXboxHeapFlagsMask);
        return try_uma_heap_create(clean_desc, [&](const D3D12_HEAP_DESC* d) {
            return real4_->CreateHeap1(d, pProtectedSession, riid, ppvHeap);
        });
    }

    HRESULT STDMETHODCALLTYPE CreateReservedResource1(
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        ID3D12ProtectedResourceSession* pProtectedSession,
        REFIID riid, void** ppvResource) override {
        if (!real4_) return E_NOINTERFACE;
        if (!pDesc) return E_INVALIDARG;
        D3D12_RESOURCE_DESC clean_desc = *pDesc;
        wsx_d3d12::sanitize_xbox_resource_desc_inplace(clean_desc);
        const D3D12_RESOURCE_STATES clean_state = wsx_d3d12::sanitize_xbox_resource_state(InitialState);
        return real4_->CreateReservedResource1(&clean_desc, clean_state, pOptimizedClearValue,
                                               pProtectedSession, riid, ppvResource);
    }

    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo1(
        UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs,
        D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) override {
        if (!real4_) return D3D12_RESOURCE_ALLOCATION_INFO{};
        if (!pResourceDescs || numResourceDescs == 0) {
            return real4_->GetResourceAllocationInfo1(visibleMask, numResourceDescs, pResourceDescs,
                                                      pResourceAllocationInfo1);
        }
        std::vector<D3D12_RESOURCE_DESC> clean(pResourceDescs, pResourceDescs + numResourceDescs);
        for (auto& d : clean) wsx_d3d12::sanitize_xbox_resource_desc_inplace(d);
        return real4_->GetResourceAllocationInfo1(visibleMask, numResourceDescs, clean.data(),
                                                  pResourceAllocationInfo1);
    }

    // ID3D12Device5
    HRESULT STDMETHODCALLTYPE CreateLifetimeTracker(
        ID3D12LifetimeOwner* pOwner, REFIID riid, void** ppvTracker) override {
        return real5_ ? real5_->CreateLifetimeTracker(pOwner, riid, ppvTracker) : E_NOINTERFACE;
    }

    void STDMETHODCALLTYPE RemoveDevice() override {
        if (real5_) real5_->RemoveDevice();
    }

    HRESULT STDMETHODCALLTYPE EnumerateMetaCommands(
        UINT* pNumMetaCommands, D3D12_META_COMMAND_DESC* pDescs) override {
        return real5_ ? real5_->EnumerateMetaCommands(pNumMetaCommands, pDescs) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE EnumerateMetaCommandParameters(
        REFGUID CommandId, D3D12_META_COMMAND_PARAMETER_STAGE Stage,
        UINT* pTotalStructureSizeInBytes, UINT* pParameterCount,
        D3D12_META_COMMAND_PARAMETER_DESC* pParameterDescs) override {
        return real5_ ? real5_->EnumerateMetaCommandParameters(
            CommandId, Stage, pTotalStructureSizeInBytes, pParameterCount, pParameterDescs) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE CreateMetaCommand(
        REFGUID CommandId, UINT NodeMask, const void* pCreationParametersData,
        SIZE_T CreationParametersDataSizeInBytes, REFIID riid, void** ppMetaCommand) override {
        return real5_ ? real5_->CreateMetaCommand(
            CommandId, NodeMask, pCreationParametersData, CreationParametersDataSizeInBytes, riid, ppMetaCommand) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE CreateStateObject(
        const D3D12_STATE_OBJECT_DESC* pDesc, REFIID riid, void** ppStateObject) override {
        return real5_ ? real5_->CreateStateObject(pDesc, riid, ppStateObject) : E_NOINTERFACE;
    }

    void STDMETHODCALLTYPE GetRaytracingAccelerationStructurePrebuildInfo(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* pDesc,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* pInfo) override {
        if (real5_) real5_->GetRaytracingAccelerationStructurePrebuildInfo(pDesc, pInfo);
    }

    D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE CheckDriverMatchingIdentifier(
        D3D12_SERIALIZED_DATA_TYPE SerializedDataType,
        const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER* pIdentifierToCheck) override {
        if (!real5_) return D3D12_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_VERSION;
        return real5_->CheckDriverMatchingIdentifier(SerializedDataType, pIdentifierToCheck);
    }

    // ID3D12Device6
    HRESULT STDMETHODCALLTYPE SetBackgroundProcessingMode(
        D3D12_BACKGROUND_PROCESSING_MODE Mode, D3D12_MEASUREMENTS_ACTION MeasurementsAction,
        HANDLE hEventToSignalUponCompletion, BOOL* pbFurtherMeasurementsDesired) override {
        return real6_ ? real6_->SetBackgroundProcessingMode(
            Mode, MeasurementsAction, hEventToSignalUponCompletion, pbFurtherMeasurementsDesired) : E_NOINTERFACE;
    }

    // ID3D12Device7
    HRESULT STDMETHODCALLTYPE AddToStateObject(
        const D3D12_STATE_OBJECT_DESC* pAddition, ID3D12StateObject* pStateObjectToGrowFrom,
        REFIID riid, void** ppNewStateObject) override {
        return real7_ ? real7_->AddToStateObject(pAddition, pStateObjectToGrowFrom, riid, ppNewStateObject) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession1(
        const D3D12_PROTECTED_RESOURCE_SESSION_DESC1* pDesc, REFIID riid, void** ppSession) override {
        return real7_ ? real7_->CreateProtectedResourceSession1(pDesc, riid, ppSession) : E_NOINTERFACE;
    }

    // ID3D12Device8
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo2(
        UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC1* pResourceDescs,
        D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) override {
        if (!real8_) return D3D12_RESOURCE_ALLOCATION_INFO{};
        if (!pResourceDescs || numResourceDescs == 0) {
            return real8_->GetResourceAllocationInfo2(visibleMask, numResourceDescs, pResourceDescs,
                                                      pResourceAllocationInfo1);
        }
        std::vector<D3D12_RESOURCE_DESC1> clean(pResourceDescs, pResourceDescs + numResourceDescs);
        for (auto& d : clean) wsx_d3d12::sanitize_xbox_resource_desc_inplace(d);
        return real8_->GetResourceAllocationInfo2(visibleMask, numResourceDescs, clean.data(),
                                                  pResourceAllocationInfo1);
    }

    HRESULT STDMETHODCALLTYPE CreateCommittedResource2(
        const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC1* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        ID3D12ProtectedResourceSession* pProtectedSession,
        REFIID riidResource, void** ppvResource) override {
        if (!real8_) return E_NOINTERFACE;
        if (!pDesc) return E_INVALIDARG;
        const D3D12_HEAP_FLAGS clean_heap_flags =
            static_cast<D3D12_HEAP_FLAGS>(HeapFlags & ~kXboxHeapFlagsMask);
        D3D12_RESOURCE_DESC1 clean_desc = *pDesc;
        wsx_d3d12::sanitize_xbox_resource_desc_inplace(clean_desc);
        const D3D12_RESOURCE_STATES clean_initial_state =
            wsx_d3d12::sanitize_xbox_initial_state_for_heap(pHeapProperties, InitialResourceState);
        return wrap_texture2d_if_needed(
            clean_desc.Dimension, clean_desc.SampleDesc.Count, pDesc->Layout,
            clean_initial_state, riidResource, ppvResource,
            [&](REFIID target_riid, void** target_ppv) {
                return try_uma_committed_create(
                    pHeapProperties, clean_heap_flags, clean_desc.Dimension, clean_desc.Flags, clean_initial_state,
                    [&](const D3D12_HEAP_PROPERTIES* hp) {
                        return real8_->CreateCommittedResource2(
                            hp, clean_heap_flags, &clean_desc, clean_initial_state,
                            pOptimizedClearValue, pProtectedSession, target_riid, target_ppv);
                    });
            });
    }

    HRESULT STDMETHODCALLTYPE CreatePlacedResource1(
        ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC1* pDesc,
        D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        REFIID riid, void** ppvResource) override {
        if (!real8_) return E_NOINTERFACE;
        if (!pDesc) return E_INVALIDARG;
        D3D12_RESOURCE_DESC1 clean_desc = *pDesc;
        wsx_d3d12::sanitize_xbox_resource_desc_inplace(clean_desc);
        D3D12_HEAP_DESC hdesc{};
        const D3D12_HEAP_PROPERTIES* hp_ptr = nullptr;
        if (pHeap) {
            hdesc = pHeap->GetDesc();
            hp_ptr = &hdesc.Properties;
        }
        const D3D12_RESOURCE_STATES clean_state =
            wsx_d3d12::sanitize_xbox_initial_state_for_heap(hp_ptr, InitialState);
        return wrap_texture2d_if_needed(
            clean_desc.Dimension, clean_desc.SampleDesc.Count, pDesc->Layout,
            clean_state, riid, ppvResource,
            [&](REFIID target_riid, void** target_ppv) {
                return real8_->CreatePlacedResource1(
                    pHeap, HeapOffset, &clean_desc, clean_state, pOptimizedClearValue, target_riid, target_ppv);
            });
    }

    void STDMETHODCALLTYPE CreateSamplerFeedbackUnorderedAccessView(
        ID3D12Resource* pTargetedResource, ID3D12Resource* pFeedbackResource,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        if (real8_) {
            real8_->CreateSamplerFeedbackUnorderedAccessView(
                wsx_d3d12::unwrap_resource(pTargetedResource),
                wsx_d3d12::unwrap_resource(pFeedbackResource),
                DestDescriptor);
        }
    }

    void STDMETHODCALLTYPE GetCopyableFootprints1(
        const D3D12_RESOURCE_DESC1* pResourceDesc, UINT FirstSubresource, UINT NumSubresources,
        UINT64 BaseOffset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts, UINT* pNumRows,
        UINT64* pRowSizeInBytes, UINT64* pTotalBytes) override {
        if (real8_) {
            if (pResourceDesc) {
                D3D12_RESOURCE_DESC1 clean = *pResourceDesc;
                wsx_d3d12::sanitize_xbox_resource_desc_inplace(clean);
                real8_->GetCopyableFootprints1(&clean, FirstSubresource, NumSubresources, BaseOffset,
                                               pLayouts, pNumRows, pRowSizeInBytes, pTotalBytes);
                return;
            }
            real8_->GetCopyableFootprints1(pResourceDesc, FirstSubresource, NumSubresources, BaseOffset,
                                           pLayouts, pNumRows, pRowSizeInBytes, pTotalBytes);
        }
    }
};

WsxD3D12DeviceProxy* as_device_proxy(ID3D12Device* dev) noexcept {
    if (!dev) return nullptr;
    ID3D12Device* unwrapped = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(wsx_d3d12::IWsxXboxDeviceAbiMarker),
                                      reinterpret_cast<void**>(&unwrapped))) && unwrapped) {
        dev = unwrapped;
    }
    WsxD3D12DeviceProxy* proxy = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IWsxD3D12DeviceProxyMarker),
                                      reinterpret_cast<void**>(&proxy)))) {
        return proxy;
    }
    return nullptr;
}

void enable_d3d12_debug_layer_if_requested(const void* pRawParams) noexcept {
    const auto* params = static_cast<const XboxCreateDeviceParameters*>(pRawParams);
#if defined(_DEBUG)
    bool enable_debug = true;
    bool enable_gbv = true;
#else
    bool enable_debug = false;
    bool enable_gbv = false;
#endif
    if (params) {
        if ((params->ProcessDebugFlags & 0x1u) != 0) enable_debug = true;
        if ((params->ProcessDebugFlags & 0x2u) != 0) {
            enable_debug = true;
            enable_gbv = true;
        }
    }
    if (wsx::get_config_bool("WSX_D3D12_DEBUG", "Graphics", "debug_layer", false)) enable_debug = true;
    if (wsx::get_config_bool("WSX_D3D12_GBV", "Graphics", "gpu_validation", false)) {
        enable_debug = true;
        enable_gbv = true;
    }
    if (!enable_debug) return;

    const auto& desktop = desktop_exports();
    if (!desktop.get_debug) return;

    ComPtr<ID3D12Debug> debug0;
    if (SUCCEEDED(desktop.get_debug(IID_PPV_ARGS(debug0.GetAddressOf())))) {
        debug0->EnableDebugLayer();
        if (enable_gbv) {
            ComPtr<ID3D12Debug1> debug1;
            if (SUCCEEDED(debug0.As(&debug1))) {
                debug1->SetEnableGPUBasedValidation(TRUE);
                debug1->SetEnableSynchronizedCommandQueueValidation(TRUE);
            }
        }
        write_result(enable_gbv ? "D3D12GetDebugInterface(DebugLayer+GBV)" : "D3D12GetDebugInterface(DebugLayer)", S_OK);
    }
}

void configure_debug_info_queue(ID3D12Device* raw_device) noexcept {
    if (!raw_device) return;
    ComPtr<ID3D12InfoQueue> info_queue;
    if (SUCCEEDED(raw_device->QueryInterface(IID_PPV_ARGS(info_queue.GetAddressOf())))) {
        D3D12_MESSAGE_ID hide_ids[] = {
            D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
        };
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumIDs = static_cast<UINT>(std::size(hide_ids));
        filter.DenyList.pIDList = hide_ids;
        info_queue->AddStorageFilterEntries(&filter);
    }
}

HRESULT wrap_created_device(ComPtr<ID3D12Device>& raw_dev, IDXGIAdapter1* adapter,
                            REFIID riid, void** ppDevice) noexcept {
    configure_debug_info_queue(raw_dev.Get());
    if (!ppDevice) {
        return S_OK;
    }
    auto* proxy = new (std::nothrow) WsxD3D12DeviceProxy(raw_dev.Get(), adapter);
    if (!proxy) {
        *ppDevice = nullptr;
        return E_OUTOFMEMORY;
    }
    if (xbox_abi_mode_enabled()) {
        auto* xbox_abi = wsx_d3d12::WsxXboxDeviceAbiProxy::create(proxy);
        proxy->Release();
        if (!xbox_abi) {
            *ppDevice = nullptr;
            return E_OUTOFMEMORY;
        }
        const HRESULT hr = reinterpret_cast<IUnknown*>(xbox_abi)->QueryInterface(riid, ppDevice);
        reinterpret_cast<IUnknown*>(xbox_abi)->Release();
        return hr;
    }
    const HRESULT hr = proxy->QueryInterface(riid, ppDevice);
    proxy->Release();
    return hr;
}

} // namespace

extern "C" {

HRESULT WINAPI WsxCreateRootSignatureDeserializer(
    LPCVOID data, SIZE_T size, REFIID iid, void** deserializer) {
    const auto& desktop = desktop_exports();
    if (!desktop.deserialize) {
        if (deserializer) {
            *deserializer = nullptr;
        }
        return write_result(deserialize_name, desktop.load_error);
    }
    std::vector<uint8_t> patched;
    if (sanitize_xbox_root_signature_blob(data, size, patched)) {
        return write_result(deserialize_name, desktop.deserialize(patched.data(), patched.size(), iid, deserializer));
    }
    return write_result(deserialize_name, desktop.deserialize(data, size, iid, deserializer));
}

HRESULT WINAPI WsxSerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* description, ID3DBlob** blob, ID3DBlob** error_blob) {
    const auto flags = root_flags(description);
    const bool has_xbox_flags = flags && (*flags & untranslated_xbox_flags) != 0;
    const auto& desktop = desktop_exports();
    if (!desktop.serialize || (has_xbox_flags && !strip_xbox_flags_enabled())) {
        if (blob) {
            *blob = nullptr;
        }
        if (error_blob) {
            *error_blob = nullptr;
        }
        return write_result(serialize_name, has_xbox_flags ? E_NOTIMPL : desktop.load_error, flags);
    }
    if (has_xbox_flags && description) {
        const auto sanitized = strip_flags_copy(*description);
        return write_result(serialize_name, desktop.serialize(&sanitized, blob, error_blob), flags);
    }
    return write_result(serialize_name, desktop.serialize(description, blob, error_blob), flags);
}

HRESULT WINAPI D3D12XboxCreateDevice(
    IUnknown* pAdapter,
    const void* pParams,
    REFIID riid,
    void** ppDevice) {
    const auto& desktop = desktop_exports();
    if (!desktop.create_device) {
        if (ppDevice) *ppDevice = nullptr;
        return write_result("D3D12XboxCreateDevice", desktop.load_error);
    }

    enable_d3d12_debug_layer_if_requested(pParams);
    if (wsx::get_config_bool("WSX_DRED", "Diagnostics", "dred", false)) {
        wsx_diag::enable_d3d12_dred();
    }
    if (wsx::get_config_bool("WSX_CRASH_DUMPS", "Diagnostics", "crash_dumps", false)) {
        wsx_diag::install_crash_handler();
    }

    const bool force_warp = env_flag_enabled(L"WSX_WARP", false);

    ComPtr<IDXGIFactory4> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) {
        if (!pAdapter && !force_warp) {
            ComPtr<IDXGIFactory6> factory6;
            if (SUCCEEDED(factory.As(&factory6))) {
                for (UINT i = 0;; ++i) {
                    ComPtr<IDXGIAdapter1> candidate;
                    if (FAILED(factory6->EnumAdapterByGpuPreference(
                            i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                            IID_PPV_ARGS(candidate.GetAddressOf())))) {
                        break;
                    }
                    DXGI_ADAPTER_DESC1 desc{};
                    if (SUCCEEDED(candidate->GetDesc1(&desc)) &&
                        (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                        continue;
                    }
                    ComPtr<ID3D12Device> raw_dev;
                    const HRESULT hr = desktop.create_device(
                        candidate.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(raw_dev.GetAddressOf()));
                    if (SUCCEEDED(hr)) {
                        const HRESULT wrap_hr = wrap_created_device(raw_dev, candidate.Get(), riid, ppDevice);
                        return write_result("D3D12XboxCreateDevice", wrap_hr);
                    }
                }
            }
        }
        if (!pAdapter && !force_warp) {
            ComPtr<ID3D12Device> raw_dev;
            const HRESULT hr = desktop.create_device(
                nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(raw_dev.GetAddressOf()));
            if (SUCCEEDED(hr)) {
                const HRESULT wrap_hr = wrap_created_device(raw_dev, nullptr, riid, ppDevice);
                return write_result("D3D12XboxCreateDevice", wrap_hr);
            }
        }
        ComPtr<IDXGIAdapter1> warp_adapter;
        if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(warp_adapter.GetAddressOf())))) {
            ComPtr<ID3D12Device> raw_dev;
            const HRESULT hr = desktop.create_device(
                warp_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(raw_dev.GetAddressOf()));
            if (SUCCEEDED(hr)) {
                const HRESULT wrap_hr = wrap_created_device(raw_dev, warp_adapter.Get(), riid, ppDevice);
                return write_result("D3D12XboxCreateDevice(WARP)", wrap_hr);
            }
        }
    }

    ComPtr<ID3D12Device> raw_dev;
    const HRESULT hr = desktop.create_device(
        pAdapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(raw_dev.GetAddressOf()));
    if (SUCCEEDED(hr)) {
        ComPtr<IDXGIAdapter1> ad1;
        if (pAdapter) pAdapter->QueryInterface(IID_PPV_ARGS(ad1.GetAddressOf()));
        const HRESULT wrap_hr = wrap_created_device(raw_dev, ad1.Get(), riid, ppDevice);
        return write_result("D3D12XboxCreateDevice", wrap_hr);
    }
    if (ppDevice) *ppDevice = nullptr;
    return write_result("D3D12XboxCreateDevice", hr);
}

HRESULT WINAPI D3D12XboxQueryInterface(
    ID3D12Device* pDevice,
    REFIID riid,
    void** ppvObject) {
    if (!pDevice || !ppvObject) {
        return E_POINTER;
    }
    return pDevice->QueryInterface(riid, ppvObject);
}

HRESULT WINAPI D3D12XboxCreateCommittedResource(
    ID3D12Device* pDevice,
    const D3D12_HEAP_PROPERTIES* pHeapProperties,
    D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    REFIID riidResource,
    void** ppvResource) {
    if (!pDevice || !pDesc) {
        return E_INVALIDARG;
    }
    return pDevice->CreateCommittedResource(
        pHeapProperties, HeapFlags, pDesc,
        InitialResourceState, pOptimizedClearValue, riidResource, ppvResource);
}

HRESULT WINAPI D3D12XboxSetFrameIntervalX(
    ID3D12Device* pDevice,
    IDXGIOutput* /*pOutput*/,
    UINT Interval,
    UINT MaxLatency,
    UINT Flags) {
    if (auto* proxy = as_device_proxy(pDevice)) {
        proxy->set_frame_interval(Interval, MaxLatency, Flags);
    }
    return wsx::trace_hr("d3d12_xs", "D3D12XboxSetFrameIntervalX", S_OK,
                         "interval=%u latency=%u flags=0x%X", Interval, MaxLatency, Flags);
}

HRESULT WINAPI D3D12XboxScheduleFrameEventX(
    ID3D12Device* pDevice,
    UINT /*EventType*/,
    UINT64 /*Time*/,
    void* /*pData*/,
    UINT /*Flags*/) {
    if (auto* proxy = as_device_proxy(pDevice)) {
        proxy->pacer().schedule_origin();
    }
    return S_OK;
}

HRESULT WINAPI D3D12XboxWaitFrameEventX(
    ID3D12Device* pDevice,
    UINT /*EventType*/,
    DWORD TimeoutMs,
    void* /*pData*/,
    UINT /*Flags*/,
    uint64_t* pToken) {
    if (auto* proxy = as_device_proxy(pDevice)) {
        proxy->pacer().wait_origin(TimeoutMs);
    }
    if (pToken) {
        *pToken = g_frame_token.fetch_add(1, std::memory_order_relaxed);
    }
    return S_OK;
}

HRESULT WINAPI D3D12XboxPresentX(
    ID3D12CommandQueue* pQueue,
    UINT PlaneCount,
    const void* pRawPlaneParameters,
    const void* pRawPresentParameters) {
    if (!pQueue) {
        return E_INVALIDARG;
    }
    // Optional Debug HUD overlay hook on PresentX (WSX_HUD=1 or live F11 toggle)
    WsxD3D12DeviceProxy* present_dev_proxy = nullptr;
    if (auto* qp_hud = wsx_d3d12::WsxD3D12CommandQueueProxy::from_interface(pQueue)) {
        ComPtr<ID3D12Device> owner_dev;
        if (SUCCEEDED(qp_hud->GetDevice(IID_PPV_ARGS(&owner_dev))) && owner_dev) {
            present_dev_proxy = as_device_proxy(owner_dev.Get());
        }
    }
    pQueue = wsx_d3d12::WsxD3D12CommandQueueProxy::unwrap(pQueue);

    std::lock_guard<std::mutex> lock(g_present_mutex);

    // Blank screen flush / resize reset
    if (PlaneCount == 0 || !pRawPlaneParameters) {
        auto it = g_present_contexts.find(pQueue);
        if (it != g_present_contexts.end() && it->second) {
            wait_queue_idle(pQueue, *it->second);
            for (UINT i = 0; i < kMaxSwapChainBuffers; ++i) {
                it->second->back_buffers[i].Reset();
                it->second->frame_fence_values[i] = 0;
            }
            if (it->second->frame_latency_waitable_object) {
                CloseHandle(it->second->frame_latency_waitable_object);
                it->second->frame_latency_waitable_object = nullptr;
            }
            it->second->swap_chain.Reset();
            it->second->next_display_buffer_index = 0;
        }
        return S_OK;
    }

    const auto* plane = static_cast<const XboxPresentPlaneParameters*>(pRawPlaneParameters);
    if (plane->ResourceCount == 0 || !plane->ppResources || !plane->ppResources[0]) {
        return S_OK;
    }
    ID3D12Resource* src = wsx_d3d12::unwrap_resource(plane->ppResources[0]);
    const D3D12_RESOURCE_DESC src_desc = src->GetDesc();
    const UINT w = static_cast<UINT>(src_desc.Width);
    const UINT h = src_desc.Height;

    PresentContext* ctx_ptr = get_or_create_present_context_locked(pQueue);
    if (!ctx_ptr) {
        return E_FAIL;
    }
    PresentContext& ctx = *ctx_ptr;

    ensure_swapchain_locked(pQueue, ctx, w, h, src_desc.Format, ctx.buffer_count);

    // Apply any pending WM_SIZE / Alt+Enter resize live via DXGI ResizeBuffers!
    if (ctx.swap_chain && ctx.hwnd) {
        UINT pend_w = 0, pend_h = 0;
        if (consume_pending_window_resize(ctx.hwnd, pend_w, pend_h)) {
            resize_swapchain_locked(pQueue, ctx, pend_w, pend_h);
        }
    }

    // Update HDR color space &HDR10 mastering metadata if requested by plane parameters
    if (ctx.swap_chain) {
        configure_hdr_metadata(ctx.swap_chain.Get(), plane->ColorSpace, src_desc.Format, ctx);
    }

    // Optional frame capture for automated testing (WSX_CAPTURE_FRAME=path.ppm)
    // Conducted BEFORE swap_chain->Present() so zero-copy swapchain backbuffers are read before FLIP_DISCARD flips them.
    UINT64 max_frames = 0;
    wchar_t exit_buf[32]{};
    if (GetEnvironmentVariableW(L"WSX_EXIT_AFTER_FRAMES", exit_buf, 32) > 0) {
        max_frames = static_cast<UINT64>(_wtoi64(exit_buf));
    }

    const UINT64 upcoming_frame = ctx.presented_frames + 1;
    const UINT64 capture_target_frame = (max_frames > 2) ? max_frames : 2;
    if (!ctx.frame_captured && upcoming_frame >= capture_target_frame) {
        wchar_t capture_path[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"WSX_CAPTURE_FRAME", capture_path, MAX_PATH) > 0) {
            capture_resource_to_ppm(pQueue, ctx, src, capture_path);
            ctx.frame_captured = true;
        }
    }

    if (ctx.swap_chain && ctx.back_buffers[0]) {
        const UINT back_idx = ctx.swap_chain->GetCurrentBackBufferIndex();
        ID3D12Resource* dst = ctx.back_buffers[back_idx].Get();

        if (src == dst) {
            // ================================================================
            // ZERO-COPY FAST PATH:
            // The game rendered directly into the DXGI SwapChain's backbuffer
            // (leased via CreateCommittedResource with D3D12_HEAP_FLAG_ALLOW_DISPLAY).
            // Zero CopyResource, zero extra command list, zero post-Present wait!
            // ================================================================
            ++ctx.zero_copy_frames;
        } else if (dst) {
            // ================================================================
            // PIPELINED NON-BLOCKING COPY FALLBACK:
            // Wait only for this specific back_idx slot from BufferCount frames ago,
            // never stalling the CPU after Present!
            // ================================================================
            wait_fence_value(ctx, ctx.frame_fence_values[back_idx]);

            if (SUCCEEDED(ctx.allocators[back_idx]->Reset()) &&
                SUCCEEDED(ctx.cmd_list->Reset(ctx.allocators[back_idx].Get(), nullptr))) {
                const D3D12_RESOURCE_DESC dst_desc = dst->GetDesc();
                const bool same_dims = (src_desc.Width == dst_desc.Width && src_desc.Height == dst_desc.Height);
                bool blitted = false;
                if (!same_dims &&
                    (src_desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0 &&
                    ensure_present_blitter_locked(ctx, dst_desc.Format)) {
                    // Hardware bilinear scale blit when user resized window or toggled borderless fullscreen
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
                    srv_desc.Format = src_desc.Format;
                    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srv_desc.Texture2D.MipLevels = 1;
                    ctx.device->CreateShaderResourceView(
                        src, &srv_desc, ctx.blitter_srv_heap->GetCPUDescriptorHandleForHeapStart());

                    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
                    rtv_desc.Format = dst_desc.Format;
                    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                    ctx.device->CreateRenderTargetView(
                        dst, &rtv_desc, ctx.blitter_rtv_heap->GetCPUDescriptorHandleForHeapStart());

                    D3D12_RESOURCE_BARRIER barriers_in[2]{};
                    barriers_in[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barriers_in[0].Transition.pResource = src;
                    barriers_in[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                    barriers_in[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    barriers_in[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

                    barriers_in[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barriers_in[1].Transition.pResource = dst;
                    barriers_in[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                    barriers_in[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    barriers_in[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    ctx.cmd_list->ResourceBarrier(2, barriers_in);

                    ID3D12DescriptorHeap* heaps[] = {ctx.blitter_srv_heap.Get()};
                    ctx.cmd_list->SetDescriptorHeaps(1, heaps);
                    ctx.cmd_list->SetGraphicsRootSignature(ctx.blitter_rs.Get());
                    ctx.cmd_list->SetPipelineState(ctx.blitter_pso.Get());
                    ctx.cmd_list->SetGraphicsRootDescriptorTable(
                        0, ctx.blitter_srv_heap->GetGPUDescriptorHandleForHeapStart());

                    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_h = ctx.blitter_rtv_heap->GetCPUDescriptorHandleForHeapStart();
                    ctx.cmd_list->OMSetRenderTargets(1, &rtv_h, FALSE, nullptr);
                    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(dst_desc.Width), static_cast<float>(dst_desc.Height), 0.0f, 1.0f};
                    D3D12_RECT sc{0, 0, static_cast<LONG>(dst_desc.Width), static_cast<LONG>(dst_desc.Height)};
                    ctx.cmd_list->RSSetViewports(1, &vp);
                    ctx.cmd_list->RSSetScissorRects(1, &sc);
                    ctx.cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    ctx.cmd_list->DrawInstanced(3, 1, 0, 0);

                    D3D12_RESOURCE_BARRIER barriers_out[2] = {barriers_in[0], barriers_in[1]};
                    barriers_out[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    barriers_out[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                    barriers_out[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    barriers_out[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                    ctx.cmd_list->ResourceBarrier(2, barriers_out);

                    ++ctx.scaled_blit_frames;
                    blitted = true;
                }
                if (!blitted) {
                    D3D12_RESOURCE_BARRIER barriers_in[2]{};
                    barriers_in[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barriers_in[0].Transition.pResource = src;
                    barriers_in[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                    barriers_in[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    barriers_in[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

                    barriers_in[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barriers_in[1].Transition.pResource = dst;
                    barriers_in[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                    barriers_in[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                    barriers_in[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

                    ctx.cmd_list->ResourceBarrier(2, barriers_in);
                    if (same_dims) {
                        ctx.cmd_list->CopyResource(dst, src);
                    } else {
                        D3D12_TEXTURE_COPY_LOCATION dloc{};
                        dloc.pResource = dst;
                        dloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        dloc.SubresourceIndex = 0;
                        D3D12_TEXTURE_COPY_LOCATION sloc{};
                        sloc.pResource = src;
                        sloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        sloc.SubresourceIndex = 0;
                        D3D12_BOX box{0, 0, 0,
                                      static_cast<UINT>((std::min)(src_desc.Width, dst_desc.Width)),
                                      (std::min)(src_desc.Height, dst_desc.Height), 1};
                        ctx.cmd_list->CopyTextureRegion(&dloc, 0, 0, 0, &sloc, &box);
                        ++ctx.scaled_blit_frames;
                    }

                    D3D12_RESOURCE_BARRIER barriers_out[2] = {barriers_in[0], barriers_in[1]};
                    barriers_out[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    barriers_out[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                    barriers_out[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                    barriers_out[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

                    ctx.cmd_list->ResourceBarrier(2, barriers_out);
                }
                if (SUCCEEDED(ctx.cmd_list->Close())) {
                    ID3D12CommandList* lists[] = {ctx.cmd_list.Get()};
                    pQueue->ExecuteCommandLists(1, lists);
                }
            }
        }

        if (present_dev_proxy && dst) {
            present_dev_proxy->hud_overlay().poll_hotkey_and_record_frame();
            if (present_dev_proxy->hud_overlay().is_enabled()) {
                const auto metrics = present_dev_proxy->build_hud_metrics();
                present_dev_proxy->hud_overlay().render(
                    present_dev_proxy->raw_device(), pQueue, dst,
                    D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, metrics);
            }
        }

        const auto* present_params = static_cast<const XboxPresentParameters*>(pRawPresentParameters);
        const bool use_tearing =
            ctx.tearing_supported &&
            (ctx.vrr_enabled || (present_params && (present_params->Flags & 1u) != 0));
        const UINT sync_interval = use_tearing ? 0u : 1u;
        const UINT present_flags = use_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0u;

        ctx.swap_chain->Present(sync_interval, present_flags);

        // Signal fence asynchronously for this backbuffer slot without blocking CPU!
        const UINT64 signal_val = ++ctx.fence_value;
        if (SUCCEEDED(pQueue->Signal(ctx.fence.Get(), signal_val))) {
            ctx.frame_fence_values[back_idx] = signal_val;
        }
    }

    ctx.presented_frames = upcoming_frame;

    // Wave 10.3: High-precision hybrid QPC frame pacing & rolling 120-frame P99 / 1% Low telemetry
    extern void wsx_record_and_pace_frame() noexcept;
    wsx_record_and_pace_frame();

    if (ctx.presented_frames == 1) {
        wsx::trace_hr("d3d12_xs", "D3D12XboxPresentX", S_OK,
                      "frame=1 %ux%u fmt=%u zeroCopy=%d vrr=%d hdr=%d",
                      w, h, static_cast<unsigned>(src_desc.Format),
                      ctx.zero_copy_frames > 0 ? 1 : 0,
                      ctx.vrr_enabled ? 1 : 0,
                      ctx.hdr_enabled ? 1 : 0);
    }

    bool win_fs = false, win_close = false;
    query_window_hook_flags(ctx.hwnd, win_fs, win_close);
    if (max_frames > 0 && ctx.presented_frames >= max_frames) {
        PostQuitMessage(0);
    } else if (win_close || (ctx.hwnd && !IsWindow(ctx.hwnd)) || (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
        PostQuitMessage(0);
    }

    return S_OK;
}

HRESULT WINAPI D3D12XboxSuspendCommandQueueX(
    ID3D12CommandQueue* pQueue,
    UINT /*Flags*/) {
    if (pQueue) {
        pQueue = wsx_d3d12::WsxD3D12CommandQueueProxy::unwrap(pQueue);
        std::lock_guard<std::mutex> lock(g_present_mutex);
        auto it = g_present_contexts.find(pQueue);
        if (it != g_present_contexts.end() && it->second) {
            wait_queue_idle(pQueue, *it->second);
        }
    }
    return S_OK;
}

HRESULT WINAPI D3D12XboxResumeCommandQueueX(
    ID3D12CommandQueue* /*pQueue*/) {
    return S_OK;
}

struct XboxAbiTelemetryOut {
    uint64_t desc_heaps_created;
    uint64_t committed_resources_via_260;
    uint64_t compute_pso_x_ok;
    uint64_t compute_pso_x_fail;
    uint64_t compute_pso_x_fallback;
    uint64_t cmd_lists_wrapped;
    uint64_t uma_alloc_gpu_mem_calls;
    uint64_t uma_va_translations;
    uint64_t unhandled_device_slots;
    uint64_t unhandled_cmdlist_slots;
    uint64_t shaders_inspected;
    uint64_t shaders_pure_dxil;
    uint64_t shaders_xbox_intrinsics;
    uint64_t shaders_fssm_rdna2;
    uint64_t shaders_accc_rdna2;
    uint64_t shaders_cache_hits;
    uint64_t shaders_translator_hits;
    uint64_t shaders_rejected_strict;
};

HRESULT WINAPI D3D12XboxGetXboxAbiDevice(
    ID3D12Device* pDevice,
    REFIID riid,
    void** ppXboxAbiDevice) {
    if (!ppXboxAbiDevice) return E_POINTER;
    *ppXboxAbiDevice = nullptr;
    if (!pDevice) return E_INVALIDARG;
    auto* wrapped = wsx_d3d12::WsxXboxDeviceAbiProxy::create(pDevice);
    if (!wrapped) return E_OUTOFMEMORY;
    const HRESULT hr = reinterpret_cast<IUnknown*>(wrapped)->QueryInterface(riid, ppXboxAbiDevice);
    reinterpret_cast<IUnknown*>(wrapped)->Release();
    return hr;
}

HRESULT WINAPI D3D12XboxWrapCommandListForXboxAbi(
    ID3D12GraphicsCommandList* pCmdList,
    void** ppXboxAbiCmdList) {
    if (!ppXboxAbiCmdList) return E_POINTER;
    *ppXboxAbiCmdList = nullptr;
    if (!pCmdList) return E_INVALIDARG;
    auto* wrapped = wsx_d3d12::WsxXboxCommandListAbiProxy::create(pCmdList, nullptr);
    if (!wrapped) return E_OUTOFMEMORY;
    *ppXboxAbiCmdList = wrapped;
    return S_OK;
}

HRESULT WINAPI D3D12XboxGetAbiTelemetry(
    XboxAbiTelemetryOut* pOut) {
    if (!pOut) return E_POINTER;
    const auto& s = wsx_d3d12::xbox_abi_stats();
    const auto& sh = wsx_shader::shader_stats();
    pOut->desc_heaps_created = s.desc_heaps_created.load(std::memory_order_relaxed);
    pOut->committed_resources_via_260 = s.committed_resources_via_260.load(std::memory_order_relaxed);
    pOut->compute_pso_x_ok = s.compute_pso_x_ok.load(std::memory_order_relaxed);
    pOut->compute_pso_x_fail = s.compute_pso_x_fail.load(std::memory_order_relaxed);
    pOut->compute_pso_x_fallback = s.compute_pso_x_fallback.load(std::memory_order_relaxed);
    pOut->cmd_lists_wrapped = s.cmd_lists_wrapped.load(std::memory_order_relaxed);
    pOut->uma_alloc_gpu_mem_calls = s.uma_alloc_gpu_mem_calls.load(std::memory_order_relaxed);
    pOut->uma_va_translations = s.uma_va_translations.load(std::memory_order_relaxed);
    pOut->unhandled_device_slots = s.unhandled_device_slots.load(std::memory_order_relaxed);
    pOut->unhandled_cmdlist_slots = s.unhandled_cmdlist_slots.load(std::memory_order_relaxed);
    pOut->shaders_inspected = sh.inspected.load(std::memory_order_relaxed);
    pOut->shaders_pure_dxil = sh.pure_dxil.load(std::memory_order_relaxed);
    pOut->shaders_xbox_intrinsics = sh.xbox_intrinsics.load(std::memory_order_relaxed);
    pOut->shaders_fssm_rdna2 = sh.fssm_rdna2.load(std::memory_order_relaxed);
    pOut->shaders_accc_rdna2 = sh.accc_rdna2.load(std::memory_order_relaxed);
    pOut->shaders_cache_hits = sh.cache_hits.load(std::memory_order_relaxed);
    pOut->shaders_translator_hits = sh.translator_hits.load(std::memory_order_relaxed);
    pOut->shaders_rejected_strict = sh.rejected_strict.load(std::memory_order_relaxed);
    return S_OK;
}

HRESULT WINAPI D3D12XboxInspectShaderBytecode(
    const void* pBytecode,
    size_t bytecodeLength,
    wsx_shader::ShaderInfo* pOutInfo) {
    return wsx_shader::inspect_shader_bytecode(pBytecode, bytecodeLength, pOutInfo);
}

HRESULT WINAPI D3D12XboxUnpackShaderFssm(
    const void* pDxbc,
    size_t dxbcSize,
    void* pOutFssm,
    size_t outCapacity,
    size_t* pOutSize,
    uint32_t* pOutCodeOffset) {
    if (!pDxbc || dxbcSize < 36) return E_INVALIDARG;
    std::vector<uint8_t> fssm;
    uint32_t code_off = 0u;
    if (!wsx_shader::unpack_fssm_from_dxbc(pDxbc, dxbcSize, fssm, &code_off)) {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    if (pOutSize) *pOutSize = fssm.size();
    if (pOutCodeOffset) *pOutCodeOffset = code_off;
    if (!pOutFssm) return S_OK;
    if (outCapacity < fssm.size()) return E_NOT_SUFFICIENT_BUFFER;
    std::memcpy(pOutFssm, fssm.data(), fssm.size());
    return S_OK;
}

HRESULT WINAPI D3D12XboxRegisterShaderTranslator(
    wsx_shader::TranslateCallback pCallback,
    void* pUserContext) {
    wsx_shader::register_shader_translator(pCallback, pUserContext);
    return S_OK;
}

UINT WINAPI D3D12XboxGetGraphicsBackend() {
    return static_cast<UINT>(wsx_shader::get_graphics_backend());
}

HRESULT WINAPI D3D12XboxCheckSm66Support(
    ID3D12Device* pDevice,
    wsx_sm66::Sm66Caps* pOutCaps) {
    return wsx_sm66::check_sm66_support(pDevice, pOutCaps);
}

HRESULT WINAPI D3D12XboxCreateSm66RootSignature(
    ID3D12Device* pDevice,
    REFIID riid,
    void** ppRootSignature) {
    const auto& desktop = desktop_exports();
    if (!desktop.serialize) return desktop.load_error;
    return wsx_sm66::create_sm66_root_signature(pDevice, desktop.serialize, riid, ppRootSignature);
}

HRESULT WINAPI D3D12XboxGetSm66HlslPreamble(
    const char** ppOutPreamble,
    size_t* pOutLength) {
    if (!ppOutPreamble) return E_POINTER;
    *ppOutPreamble = wsx_sm66::kSm66HlslPreamble;
    if (pOutLength) *pOutLength = sizeof(wsx_sm66::kSm66HlslPreamble) - 1u;
    return S_OK;
}

HRESULT WINAPI D3D12XboxCompileHlslToDxil(
    const char* pHlslSource,
    size_t sourceLength,
    const wchar_t* pEntryPoint,
    const wchar_t* pTargetProfile,
    void* pOutDxil,
    size_t outCapacity,
    size_t* pOutSize,
    char* pOutErrors,
    size_t errorCapacity) {
    return wsx_sm66::compile_hlsl_to_dxil(
        pHlslSource, sourceLength, pEntryPoint, pTargetProfile,
        pOutDxil, outCapacity, pOutSize, pOutErrors, errorCapacity);
}

HRESULT WINAPI D3D12XboxTranslateFssmToHlslSm66(
    const void* pFssm,
    size_t fssmSize,
    UINT stage,
    char* pOutHlsl,
    size_t outCapacity,
    size_t* pOutSize,
    wsx_rdna2::TranslateStats* pOutStats) {
    return wsx_rdna2::translate_fssm_to_hlsl_sm66(
        pFssm, fssmSize, stage, pOutHlsl, outCapacity, pOutSize, pOutStats);
}

HRESULT WINAPI D3D12XboxTranslateDxbcFssmToDxil(
    const void* pDxbc,
    size_t dxbcSize,
    void* pOutDxil,
    size_t outCapacity,
    size_t* pOutSize,
    char* pOutErrors,
    size_t errorCapacity,
    wsx_rdna2::TranslateStats* pOutStats) {
    if (!pOutSize) return E_INVALIDARG;
    *pOutSize = 0;
    std::vector<uint8_t> dxil;
    const HRESULT hr = wsx_rdna2::translate_dxbc_fssm_to_dxil(
        pDxbc, dxbcSize, dxil, pOutErrors, errorCapacity, pOutStats);
    if (FAILED(hr)) return hr;
    *pOutSize = dxil.size();
    if (!pOutDxil) return S_OK;
    if (outCapacity < dxil.size()) return E_NOT_SUFFICIENT_BUFFER;
    std::memcpy(pOutDxil, dxil.data(), dxil.size());
    return S_OK;
}

HRESULT WINAPI D3D12XboxSetGuestTextureSwizzleMode(
    ID3D12Resource* pResource,
    UINT swizzleMode) {
    if (!pResource) return E_INVALIDARG;
    auto* rp = wsx_d3d12::WsxD3D12ResourceProxy::from_interface(pResource);
    if (!rp) return E_NOINTERFACE;
    return rp->set_swizzle_mode(swizzleMode);
}

HRESULT WINAPI D3D12XboxFlushGuestTextures(
    ID3D12CommandQueue* pQueue) {
    if (!pQueue) return E_INVALIDARG;
    auto* qp = wsx_d3d12::WsxD3D12CommandQueueProxy::from_interface(pQueue);
    if (!qp) return E_NOINTERFACE;
    qp->flush_guest_textures();
    return S_OK;
}

struct WsxGuestTextureStatsPublic {
    uint64_t tracked_textures;
    uint64_t backing_allocations;
    uint64_t sync_flushes;
    uint64_t textures_uploaded;
    uint64_t deswizzled_uploads;
    uint64_t bytes_uploaded;
};

struct WsxVramBudgetPublic {
    uint64_t local_budget_bytes;
    uint64_t local_usage_bytes;
    uint64_t local_available_bytes;
    uint64_t non_local_budget_bytes;
    uint64_t non_local_usage_bytes;
    uint32_t pressure_level;
    uint32_t reserved;
};

struct WsxHighloadPipelineStatsPublic {
    uint64_t write_watch_skips;
    uint64_t gpu_deswizzled_uploads;
    uint64_t staging_trims;
    uint64_t draw_calls;
    uint64_t dispatch_calls;
    uint64_t mesh_dispatches;
    uint64_t ray_dispatches;
    uint64_t barriers_submitted;
    uint64_t barriers_elided;
    uint64_t execute_command_lists;
    uint64_t async_enqueued;
    uint64_t async_completed;
    uint64_t async_failed;
    uint64_t async_dedup_hits;
    uint64_t async_in_flight;
    uint32_t async_worker_threads;
    uint32_t reserved;
};

HRESULT WINAPI D3D12XboxGetGuestTextureSyncStats(
    ID3D12Device* pDevice,
    WsxGuestTextureStatsPublic* pOutStats) {
    if (!pDevice || !pOutStats) return E_POINTER;
    WsxD3D12DeviceProxy* dp = nullptr;
    if (FAILED(pDevice->QueryInterface(__uuidof(IWsxD3D12DeviceProxyMarker),
                                       reinterpret_cast<void**>(&dp))) || !dp) {
        return E_NOINTERFACE;
    }
    const auto& mgr = dp->guest_texture_manager();
    if (!mgr) {
        *pOutStats = {};
        return S_OK;
    }
    const auto full = mgr->get_stats();
    pOutStats->tracked_textures = full.tracked_textures;
    pOutStats->backing_allocations = full.backing_allocations;
    pOutStats->sync_flushes = full.sync_flushes;
    pOutStats->textures_uploaded = full.textures_uploaded;
    pOutStats->deswizzled_uploads = full.deswizzled_uploads;
    pOutStats->bytes_uploaded = full.bytes_uploaded;
    return S_OK;
}

HRESULT WINAPI D3D12XboxReadbackGuestTexture(
    ID3D12CommandQueue* pQueue,
    ID3D12Resource* pResource) {
    if (!pQueue || !pResource) return E_INVALIDARG;
    auto* qp = wsx_d3d12::WsxD3D12CommandQueueProxy::from_interface(pQueue);
    if (!qp) return E_NOINTERFACE;
    return qp->readback_guest_texture(pResource);
}

HRESULT WINAPI D3D12XboxRegisterFrameEventX(
    ID3D12Device* pDevice,
    UINT /*EventType*/,
    HANDLE hEvent,
    UINT /*Flags*/) {
    if (!pDevice) return E_INVALIDARG;
    if (hEvent && hEvent != INVALID_HANDLE_VALUE) {
        SetEvent(hEvent);
    }
    return S_OK;
}

HRESULT WINAPI D3D12XboxFlushPsoCache(ID3D12Device* pDevice) {
    if (!pDevice) return E_INVALIDARG;
    WsxD3D12DeviceProxy* dp = nullptr;
    if (FAILED(pDevice->QueryInterface(__uuidof(IWsxD3D12DeviceProxyMarker),
                                       reinterpret_cast<void**>(&dp))) || !dp) {
        return E_NOINTERFACE;
    }
    return dp->flush_pso_cache();
}

HRESULT WINAPI D3D12XboxGetPsoCacheStats(wsx_pso_cache::PsoCacheStats* pOutStats) {
    if (!pOutStats) return E_POINTER;
    const auto& c = wsx_pso_cache::global_counters();
    pOutStats->hits = c.hits.load(std::memory_order_relaxed);
    pOutStats->misses = c.misses.load(std::memory_order_relaxed);
    pOutStats->stores = c.stores.load(std::memory_order_relaxed);
    pOutStats->flushes = c.flushes.load(std::memory_order_relaxed);
    pOutStats->library_bytes = c.last_library_bytes.load(std::memory_order_relaxed);
    return S_OK;
}

HRESULT WINAPI D3D12XboxQueryVramBudget(
    ID3D12Device* pDevice,
    WsxVramBudgetPublic* pOutBudget) {
    if (!pOutBudget) return E_POINTER;
    *pOutBudget = {};
    if (!pDevice) return E_INVALIDARG;

    ComPtr<IDXGIAdapter3> adapter3;
    WsxD3D12DeviceProxy* dp = nullptr;
    ID3D12Device* raw_dev = pDevice;
    if (SUCCEEDED(pDevice->QueryInterface(__uuidof(IWsxD3D12DeviceProxyMarker),
                                          reinterpret_cast<void**>(&dp))) && dp) {
        if (dp->adapter()) {
            dp->adapter()->QueryInterface(IID_PPV_ARGS(&adapter3));
        }
        if (dp->raw_device()) raw_dev = dp->raw_device();
    }
    if (!adapter3 && raw_dev) {
        const LUID luid = raw_dev->GetAdapterLuid();
        ComPtr<IDXGIFactory4> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter3));
        }
    }
    if (!adapter3) return E_NOINTERFACE;

    DXGI_QUERY_VIDEO_MEMORY_INFO local_info{};
    DXGI_QUERY_VIDEO_MEMORY_INFO non_local_info{};
    const HRESULT hr = adapter3->QueryVideoMemoryInfo(0u, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local_info);
    if (FAILED(hr)) return hr;
    adapter3->QueryVideoMemoryInfo(0u, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non_local_info);

    pOutBudget->local_budget_bytes = local_info.Budget;
    pOutBudget->local_usage_bytes = local_info.CurrentUsage;
    pOutBudget->local_available_bytes =
        (local_info.Budget > local_info.CurrentUsage) ? (local_info.Budget - local_info.CurrentUsage) : 0u;
    pOutBudget->non_local_budget_bytes = non_local_info.Budget;
    pOutBudget->non_local_usage_bytes = non_local_info.CurrentUsage;

    uint32_t pressure = 0u;
    if (local_info.Budget > 0u) {
        const double ratio = static_cast<double>(local_info.CurrentUsage) / static_cast<double>(local_info.Budget);
        if (ratio >= 0.92) pressure = 2u;
        else if (ratio >= 0.80) pressure = 1u;
    }
    pOutBudget->pressure_level = pressure;
    return S_OK;
}

HRESULT WINAPI D3D12XboxEnqueueAsyncShaderCompile(
    const void* pDxbc,
    size_t dxbcSize) {
    return wsx_async::AsyncShaderCompileQueue::instance().enqueue(pDxbc, dxbcSize);
}

HRESULT WINAPI D3D12XboxDrainAsyncShaderQueue(DWORD timeoutMs) {
    const bool done = wsx_async::AsyncShaderCompileQueue::instance().drain(timeoutMs);
    return done ? S_OK : HRESULT_FROM_WIN32(WAIT_TIMEOUT);
}

HRESULT WINAPI D3D12XboxGetHighloadPipelineStats(
    ID3D12Device* pDevice,
    WsxHighloadPipelineStatsPublic* pOutStats) {
    if (!pOutStats) return E_POINTER;
    *pOutStats = {};
    if (pDevice) {
        WsxD3D12DeviceProxy* dp = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(__uuidof(IWsxD3D12DeviceProxyMarker),
                                              reinterpret_cast<void**>(&dp))) && dp) {
            if (const auto& mgr = dp->guest_texture_manager()) {
                const auto gs = mgr->get_stats();
                pOutStats->write_watch_skips = gs.write_watch_skips;
                pOutStats->gpu_deswizzled_uploads = gs.gpu_deswizzled_uploads;
                pOutStats->staging_trims = gs.staging_trims;
            }
        }
    }
    const auto& ct = wsx_d3d12::global_command_telemetry();
    pOutStats->draw_calls = ct.draw_calls.load(std::memory_order_relaxed);
    pOutStats->dispatch_calls = ct.dispatch_calls.load(std::memory_order_relaxed);
    pOutStats->mesh_dispatches = ct.mesh_dispatches.load(std::memory_order_relaxed);
    pOutStats->ray_dispatches = ct.ray_dispatches.load(std::memory_order_relaxed);
    pOutStats->barriers_submitted = ct.barriers_submitted.load(std::memory_order_relaxed);
    pOutStats->barriers_elided = ct.barriers_elided.load(std::memory_order_relaxed);
    pOutStats->execute_command_lists = ct.execute_command_lists.load(std::memory_order_relaxed);

    const auto as = wsx_async::AsyncShaderCompileQueue::instance().get_stats();
    pOutStats->async_enqueued = as.enqueued;
    pOutStats->async_completed = as.completed;
    pOutStats->async_failed = as.failed;
    pOutStats->async_dedup_hits = as.dedup_hits;
    pOutStats->async_in_flight = as.in_flight;
    pOutStats->async_worker_threads = as.worker_threads;
    return S_OK;
}

HRESULT WINAPI D3D12XboxTrimStagingMemory(
    ID3D12CommandQueue* pQueue,
    uint64_t* pOutSlotsFreed) {
    if (pOutSlotsFreed) *pOutSlotsFreed = 0u;
    if (!pQueue) return E_INVALIDARG;
    auto* qp = wsx_d3d12::WsxD3D12CommandQueueProxy::from_interface(pQueue);
    if (!qp) return E_NOINTERFACE;
    const uint64_t freed = qp->trim_staging_memory();
    if (pOutSlotsFreed) *pOutSlotsFreed = freed;
    return S_OK;
}

HRESULT WINAPI D3D12XboxSetHudEnabled(
    ID3D12Device* pDevice,
    BOOL enabled) {
    if (!pDevice) return E_INVALIDARG;
    auto* dp = as_device_proxy(pDevice);
    if (!dp) return E_NOINTERFACE;
    dp->hud_overlay().set_enabled(enabled != FALSE);
    return S_OK;
}

HRESULT WINAPI D3D12XboxRenderDebugHud(
    ID3D12Device* pDevice,
    ID3D12CommandQueue* pQueue,
    ID3D12Resource* pBackbuffer,
    D3D12_RESOURCE_STATES stateBefore,
    D3D12_RESOURCE_STATES stateAfter) {
    if (!pDevice || !pQueue || !pBackbuffer) return E_INVALIDARG;
    auto* dp = as_device_proxy(pDevice);
    if (!dp) return E_NOINTERFACE;
    dp->hud_overlay().poll_hotkey_and_record_frame();
    auto metrics = dp->build_hud_metrics();
    metrics.wave64_emulated = (wsx_sm66::get_host_wave_lane_max() < 64u) ? 1u : 0u;
    const HRESULT hr = dp->hud_overlay().render(
        dp->raw_device(),
        wsx_d3d12::WsxD3D12CommandQueueProxy::unwrap(pQueue),
        wsx_d3d12::unwrap_resource(pBackbuffer),
        wsx_d3d12::sanitize_xbox_resource_state(stateBefore),
        wsx_d3d12::sanitize_xbox_resource_state(stateAfter),
        metrics);
    if (SUCCEEDED(hr) && dp->hud_overlay().poll_f12_screenshot_requested()) {
        wchar_t fname[128]{};
        SYSTEMTIME st{};
        GetLocalTime(&st);
        std::swprintf(fname, std::size(fname), L"wsx_shot_%04u%02u%02u_%02u%02u%02u.png",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        dp->hud_overlay().capture_backbuffer_png(
            dp->raw_device(),
            wsx_d3d12::WsxD3D12CommandQueueProxy::unwrap(pQueue),
            wsx_d3d12::unwrap_resource(pBackbuffer),
            wsx_d3d12::sanitize_xbox_resource_state(stateAfter),
            wsx_d3d12::sanitize_xbox_resource_state(stateAfter),
            std::filesystem::path(L"captures") / fname);
    }
    return hr;
}

HRESULT WINAPI D3D12XboxCaptureBackbufferPng(
    ID3D12Device* pDevice,
    ID3D12CommandQueue* pQueue,
    ID3D12Resource* pBackbuffer,
    D3D12_RESOURCE_STATES stateBefore,
    D3D12_RESOURCE_STATES stateAfter,
    const wchar_t* pOutPngPath) {
    if (!pDevice || !pQueue || !pBackbuffer || !pOutPngPath || pOutPngPath[0] == L'\0') {
        return E_INVALIDARG;
    }
    auto* dp = as_device_proxy(pDevice);
    if (!dp) return E_NOINTERFACE;
    return dp->hud_overlay().capture_backbuffer_png(
        dp->raw_device(),
        wsx_d3d12::WsxD3D12CommandQueueProxy::unwrap(pQueue),
        wsx_d3d12::unwrap_resource(pBackbuffer),
        wsx_d3d12::sanitize_xbox_resource_state(stateBefore),
        wsx_d3d12::sanitize_xbox_resource_state(stateAfter),
        std::filesystem::path(pOutPngPath));
}

HRESULT WINAPI D3D12XboxCaptureGpuDredReport(
    ID3D12Device* pDevice,
    char* pOutBuf,
    size_t outCapacity,
    size_t* pOutLength) {
    if (!pDevice) return E_INVALIDARG;
    auto* dp = as_device_proxy(pDevice);
    ID3D12Device* raw = dp ? dp->raw_device() : pDevice;
    std::string report;
    const HRESULT hr = wsx_diag::capture_dred_report(raw, report);
    if (FAILED(hr)) return hr;
    if (pOutLength) *pOutLength = report.size();
    if (!pOutBuf) return S_OK;
    if (outCapacity <= report.size()) return E_NOT_SUFFICIENT_BUFFER;
    std::memcpy(pOutBuf, report.data(), report.size());
    pOutBuf[report.size()] = '\0';
    return S_OK;
}

HRESULT WINAPI D3D12XboxWriteCrashReport(
    void* pExceptionPointers,
    const wchar_t* pOutDir,
    wchar_t* pOutTxtPath,
    size_t txtCap,
    wchar_t* pOutDmpPath,
    size_t dmpCap,
    ID3D12Device* pOptionalDevice) {
    ID3D12Device* raw_dev = nullptr;
    if (pOptionalDevice) {
        auto* dp = as_device_proxy(pOptionalDevice);
        raw_dev = dp ? dp->raw_device() : pOptionalDevice;
    }
    const auto& pc = wsx_pso_cache::global_counters();
    const auto& ct = wsx_d3d12::global_command_telemetry();
    const auto as = wsx_async::AsyncShaderCompileQueue::instance().get_stats();
    char telem[512]{};
    std::snprintf(telem, sizeof(telem),
                  "PSO Cache : hits=%llu misses=%llu stores=%llu flushes=%llu libBytes=%llu\n"
                  "Commands  : draws=%llu dispatches=%llu mesh=%llu rays=%llu barriersSub=%llu elided=%llu execCL=%llu\n"
                  "AsyncQ    : enqueued=%llu completed=%llu failed=%llu dedup=%llu workers=%u",
                  static_cast<unsigned long long>(pc.hits.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(pc.misses.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(pc.stores.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(pc.flushes.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(pc.last_library_bytes.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(ct.draw_calls.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(ct.dispatch_calls.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(ct.mesh_dispatches.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(ct.ray_dispatches.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(ct.barriers_submitted.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(ct.barriers_elided.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(ct.execute_command_lists.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(as.enqueued),
                  static_cast<unsigned long long>(as.completed),
                  static_cast<unsigned long long>(as.failed),
                  static_cast<unsigned long long>(as.dedup_hits),
                  as.worker_threads);

    std::wstring txt_path, dmp_path;
    const HRESULT hr = wsx_diag::write_crash_report(
        static_cast<EXCEPTION_POINTERS*>(pExceptionPointers),
        pOutDir, &txt_path, &dmp_path, raw_dev, telem);
    if (FAILED(hr)) return hr;
    if (pOutTxtPath && txtCap > 0u) {
        const size_t n = (std::min)(txt_path.size(), txtCap - 1u);
        std::memcpy(pOutTxtPath, txt_path.data(), n * sizeof(wchar_t));
        pOutTxtPath[n] = L'\0';
    }
    if (pOutDmpPath && dmpCap > 0u) {
        const size_t n = (std::min)(dmp_path.size(), dmpCap - 1u);
        std::memcpy(pOutDmpPath, dmp_path.data(), n * sizeof(wchar_t));
        pOutDmpPath[n] = L'\0';
    }
    return S_OK;
}

struct WsxWindowDisplayStatsOut {
    uint32_t width;
    uint32_t height;
    uint32_t is_fullscreen;
    uint32_t close_requested;
    uint64_t resize_count;
    uint64_t scaled_blit_frames;
};

HRESULT WINAPI D3D12XboxToggleBorderlessFullscreen(
    HWND hwnd,
    BOOL* pOutIsFullscreen) {
    if (!hwnd) hwnd = find_process_window();
    if (!hwnd || !IsWindow(hwnd)) return E_INVALIDARG;
    ensure_window_subclassed(hwnd);
    bool is_fs = false;
    if (!toggle_borderless_fullscreen_impl(hwnd, &is_fs)) {
        return E_FAIL;
    }
    if (pOutIsFullscreen) *pOutIsFullscreen = is_fs ? TRUE : FALSE;
    return S_OK;
}

HRESULT WINAPI D3D12XboxResizeSwapChain(
    ID3D12CommandQueue* pQueue,
    UINT width,
    UINT height) {
    if (!pQueue || width == 0u || height == 0u) return E_INVALIDARG;
    pQueue = wsx_d3d12::WsxD3D12CommandQueueProxy::unwrap(pQueue);
    std::lock_guard<std::mutex> lock(g_present_mutex);
    auto it = g_present_contexts.find(pQueue);
    if (it == g_present_contexts.end() || !it->second || !it->second->swap_chain) {
        return E_NOT_VALID_STATE;
    }
    return resize_swapchain_locked(pQueue, *it->second, width, height) ? S_OK : E_FAIL;
}

HRESULT WINAPI D3D12XboxGetWindowDisplayStats(
    ID3D12CommandQueue* pQueue,
    WsxWindowDisplayStatsOut* pOutStats) {
    if (!pQueue || !pOutStats) return E_INVALIDARG;
    std::memset(pOutStats, 0, sizeof(*pOutStats));
    pQueue = wsx_d3d12::WsxD3D12CommandQueueProxy::unwrap(pQueue);
    std::lock_guard<std::mutex> lock(g_present_mutex);
    auto it = g_present_contexts.find(pQueue);
    if (it == g_present_contexts.end() || !it->second) {
        return S_FALSE;
    }
    const PresentContext& ctx = *it->second;
    bool is_fs = false, close_req = false;
    query_window_hook_flags(ctx.hwnd, is_fs, close_req);
    pOutStats->width = ctx.width;
    pOutStats->height = ctx.height;
    pOutStats->is_fullscreen = is_fs ? 1u : 0u;
    pOutStats->close_requested = close_req ? 1u : 0u;
    pOutStats->resize_count = ctx.resize_count;
    pOutStats->scaled_blit_frames = ctx.scaled_blit_frames;
    return S_OK;
}

// ============================================================================
// Wave 10.3: High-Precision Hybrid QPC Frame Pacer + P99 / 1% Low Telemetry
// ============================================================================
struct WsxFramePacerState {
    std::mutex mtx;
    bool initialized = false;
    uint32_t target_fps = 0u;
    bool low_latency = true;
    int64_t qpc_freq = 1;
    int64_t last_qpc = 0;
    uint64_t paced_frames = 0;
    double last_frame_ms = 0.0;
    double history_ms[120]{};
    uint32_t history_count = 0;
    uint32_t history_cursor = 0;

    void ensure_init_locked() noexcept {
        if (initialized) return;
        initialized = true;
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        qpc_freq = (f.QuadPart > 0) ? f.QuadPart : 10000000LL;
        const int cfg_fps = wsx::get_config_int("WSX_MAX_FPS", "Graphics", "max_fps", 0);
        if (cfg_fps > 0) target_fps = static_cast<uint32_t>(std::clamp(cfg_fps, 1, 1000));
    }
};

WsxFramePacerState& frame_pacer_state() noexcept {
    static WsxFramePacerState st;
    return st;
}

void wsx_record_and_pace_frame() noexcept {
    auto& st = frame_pacer_state();
    std::lock_guard<std::mutex> lock(st.mtx);
    st.ensure_init_locked();

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (st.last_qpc != 0) {
        if (st.target_fps > 0u) {
            const double target_sec = 1.0 / static_cast<double>(st.target_fps);
            const int64_t target_ticks = static_cast<int64_t>(target_sec * static_cast<double>(st.qpc_freq));
            const int64_t deadline = st.last_qpc + target_ticks;
            // Coarse Sleep(1) while > 1.5 ms remains, then sub-millisecond spin-wait
            while (now.QuadPart < deadline) {
                const double rem_ms = static_cast<double>(deadline - now.QuadPart) * 1000.0 / static_cast<double>(st.qpc_freq);
                if (rem_ms > 1.5) {
                    Sleep(1);
                } else {
                    YieldProcessor();
                }
                QueryPerformanceCounter(&now);
            }
        }
        const double dt_ms = static_cast<double>(now.QuadPart - st.last_qpc) * 1000.0 / static_cast<double>(st.qpc_freq);
        st.last_frame_ms = dt_ms;
        st.history_ms[st.history_cursor] = dt_ms;
        st.history_cursor = (st.history_cursor + 1u) % 120u;
        if (st.history_count < 120u) ++st.history_count;
    }
    st.last_qpc = now.QuadPart;
    ++st.paced_frames;
}

struct WsxFramePacingStatsOut {
    uint32_t target_fps;
    uint32_t low_latency_enabled;
    uint64_t paced_frames;
    double last_frame_ms;
    double avg_fps;
    double one_percent_low_fps;
    double p99_frame_ms;
};

struct WsxExtendedAbiStatsOut {
    uint64_t graphics_pso_x_ok;
    uint64_t placed_resources_x_ok;
    uint64_t dispatch_mesh_calls;
    uint64_t render_pass_calls;
    uint64_t execute_indirect_calls;
    uint64_t pix_markers_recorded;
    uint64_t unhandled_device_slots;
    uint64_t unhandled_cmdlist_slots;
};

HRESULT WINAPI D3D12XboxConfigureFramePacing(
    UINT maxFps,
    BOOL lowLatencyWait) {
    auto& st = frame_pacer_state();
    std::lock_guard<std::mutex> lock(st.mtx);
    st.ensure_init_locked();
    st.target_fps = (maxFps > 1000u) ? 1000u : maxFps;
    st.low_latency = (lowLatencyWait != FALSE);
    st.last_qpc = 0;
    st.history_count = 0;
    st.history_cursor = 0;
    return wsx::trace_hr("d3d12_xs", "D3D12XboxConfigureFramePacing", S_OK,
                         "maxFps=%u lowLatency=%d", st.target_fps, st.low_latency ? 1 : 0);
}

HRESULT WINAPI D3D12XboxGetFramePacingStats(
    WsxFramePacingStatsOut* pOutStats) {
    if (!pOutStats) return E_POINTER;
    std::memset(pOutStats, 0, sizeof(*pOutStats));
    auto& st = frame_pacer_state();
    std::lock_guard<std::mutex> lock(st.mtx);
    st.ensure_init_locked();
    pOutStats->target_fps = st.target_fps;
    pOutStats->low_latency_enabled = st.low_latency ? 1u : 0u;
    pOutStats->paced_frames = st.paced_frames;
    pOutStats->last_frame_ms = st.last_frame_ms;
    if (st.history_count > 0u) {
        double sorted[120]{};
        double sum = 0.0;
        for (uint32_t i = 0; i < st.history_count; ++i) {
            sorted[i] = st.history_ms[i];
            sum += sorted[i];
        }
        std::sort(sorted, sorted + st.history_count);
        const double avg_ms = sum / static_cast<double>(st.history_count);
        const uint32_t p99_idx = std::min<uint32_t>(
            st.history_count - 1u,
            static_cast<uint32_t>(std::ceil(0.99 * static_cast<double>(st.history_count))) - 1u);
        const double p99_ms = sorted[p99_idx];
        pOutStats->avg_fps = (avg_ms > 0.001) ? (1000.0 / avg_ms) : 0.0;
        pOutStats->p99_frame_ms = p99_ms;
        pOutStats->one_percent_low_fps = (p99_ms > 0.001) ? (1000.0 / p99_ms) : 0.0;
    }
    return S_OK;
}

void wsx_query_frame_pacer_hud_metrics(float* one_pct_low, float* p99_ms) noexcept {
    WsxFramePacingStatsOut st{};
    if (SUCCEEDED(D3D12XboxGetFramePacingStats(&st))) {
        if (one_pct_low) *one_pct_low = static_cast<float>(st.one_percent_low_fps);
        if (p99_ms) *p99_ms = static_cast<float>(st.p99_frame_ms);
    }
}

HRESULT WINAPI D3D12XboxGetExtendedAbiStats(
    WsxExtendedAbiStatsOut* pOutStats) {
    if (!pOutStats) return E_POINTER;
    const auto& s = wsx_d3d12::xbox_abi_stats();
    pOutStats->graphics_pso_x_ok = s.graphics_pso_x_ok.load(std::memory_order_relaxed);
    pOutStats->placed_resources_x_ok = s.placed_resources_x_ok.load(std::memory_order_relaxed);
    pOutStats->dispatch_mesh_calls = s.dispatch_mesh_calls.load(std::memory_order_relaxed);
    pOutStats->render_pass_calls = s.render_pass_calls.load(std::memory_order_relaxed);
    pOutStats->execute_indirect_calls = s.execute_indirect_calls.load(std::memory_order_relaxed);
    pOutStats->pix_markers_recorded = s.pix_markers_recorded.load(std::memory_order_relaxed);
    pOutStats->unhandled_device_slots = s.unhandled_device_slots.load(std::memory_order_relaxed);
    pOutStats->unhandled_cmdlist_slots = s.unhandled_cmdlist_slots.load(std::memory_order_relaxed);
    return S_OK;
}

HRESULT WINAPI D3D12XboxSetProcessDebugFlags(UINT flags) {
    const UINT masked = flags & kXboxProcessDebugFlagsMask;
    g_xbox_process_debug_flags.store(masked, std::memory_order_release);
    return S_OK;
}

UINT WINAPI D3D12XboxGetProcessDebugFlags() {
    return g_xbox_process_debug_flags.load(std::memory_order_acquire);
}

struct D3D12XBOX_VIRTUAL_MEMORY_CONFIGURATION {
    UINT     Flags;
    UINT     SizeIn4MbUnits;
    UINT     Option;
    UINT     Reserved;
    UINT64   Result0;
    UINT64   Result1;
};

HRESULT WINAPI D3D12ConfigureVirtualMemoryX(
    D3D12XBOX_VIRTUAL_MEMORY_CONFIGURATION* pConfig) {
    if (!pConfig) return E_INVALIDARG;
    if (pConfig->Flags > 1u ||
        pConfig->SizeIn4MbUnits > 0x21u ||
        (pConfig->Option != 0xFFFFFFFFu && pConfig->Option >= 0x10u)) {
        return E_INVALIDARG;
    }
    pConfig->Result0 = 0ULL;
    pConfig->Result1 = 0ULL;
    return static_cast<HRESULT>(0x10000000L);
}

HRESULT WINAPI D3D12XboxOpenExistingHeapAlias(
    ID3D12Device* pDevice,
    void* pCpuBase,
    SIZE_T sizeInBytes,
    ID3D12Heap** ppOutHeap,
    ID3D12Resource** ppOutResource,
    D3D12_GPU_VIRTUAL_ADDRESS* pOutDesktopGpuVa) {
    if (ppOutHeap) *ppOutHeap = nullptr;
    if (ppOutResource) *ppOutResource = nullptr;
    if (pOutDesktopGpuVa) *pOutDesktopGpuVa = 0ULL;
    if (!pDevice || !pCpuBase || sizeInBytes == 0u) return E_INVALIDARG;

    // Validate via VirtualQuery that pCpuBase is the exact AllocationBase of a committed region (prevents D3D12 error 1088)
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(pCpuBase, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT ||
        mbi.AllocationBase != pCpuBase ||
        mbi.RegionSize < sizeInBytes) {
        return E_INVALIDARG;
    }

    ComPtr<ID3D12Device3> dev3;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&dev3))) || !dev3) {
        return E_NOINTERFACE;
    }

    ComPtr<ID3D12Heap> heap;
    HRESULT hr = dev3->OpenExistingHeapFromAddress(pCpuBase, IID_PPV_ARGS(&heap));
    if (FAILED(hr) || !heap) return hr;

    const D3D12_HEAP_DESC hd = heap->GetDesc();
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Alignment = 0u;
    rd.Width = (std::min<UINT64>)(hd.SizeInBytes, (static_cast<UINT64>(sizeInBytes) + 65535ULL) & ~65535ULL);
    rd.Height = 1u;
    rd.DepthOrArraySize = 1u;
    rd.MipLevels = 1u;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1u;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

    ComPtr<ID3D12Resource> res;
    hr = pDevice->CreatePlacedResource(
        heap.Get(), 0ULL, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&res));
    if (FAILED(hr) || !res) return hr;

    if (pOutDesktopGpuVa) *pOutDesktopGpuVa = res->GetGPUVirtualAddress();
    if (ppOutHeap) *ppOutHeap = heap.Detach();
    if (ppOutResource) *ppOutResource = res.Detach();
    return S_OK;
}

HRESULT WINAPI D3D12XboxUnpackAllShaderFssm(
    const void* pDxbc,
    size_t dxbcSize,
    uint32_t payloadIndex,
    void* pOutFssmBuffer,
    size_t fssmBufferCapacity,
    size_t* pOutFssmSize,
    uint32_t* pOutTotalPayloads) {
    if (pOutFssmSize) *pOutFssmSize = 0u;
    if (pOutTotalPayloads) *pOutTotalPayloads = 0u;
    if (!pDxbc || dxbcSize < 36u) return E_INVALIDARG;

    std::vector<std::vector<uint8_t>> payloads;
    const size_t total = wsx_shader::unpack_all_fssm_from_dxbc(pDxbc, dxbcSize, payloads);
    if (pOutTotalPayloads) *pOutTotalPayloads = static_cast<uint32_t>(total);
    if (total == 0u || payloadIndex >= total) return E_INVALIDARG;

    const auto& chosen = payloads[payloadIndex];
    if (pOutFssmSize) *pOutFssmSize = chosen.size();
    if (!pOutFssmBuffer) return S_OK;
    if (fssmBufferCapacity < chosen.size()) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::memcpy(pOutFssmBuffer, chosen.data(), chosen.size());
    return S_OK;
}

HRESULT WINAPI D3D12XboxRetargetDxilTriple(
    const void* pInDxbc,
    size_t inDxbcSize,
    void* pOutDxbc,
    size_t outCapacity,
    size_t* pOutSize,
    UINT* pOutTriplesPatched) {
    if (pOutSize) *pOutSize = inDxbcSize;
    if (pOutTriplesPatched) *pOutTriplesPatched = 0u;
    if (!pInDxbc || inDxbcSize < 36u) return E_INVALIDARG;
    const auto* bytes = static_cast<const uint8_t*>(pInDxbc);
    if (std::memcmp(bytes, "DXBC", 4u) != 0) return E_INVALIDARG;

    std::vector<uint8_t> work(bytes, bytes + inDxbcSize);
    const uint32_t patched = wsx_shader::retarget_xbox_dxil_triple_inplace(work);
    if (patched > 0u) {
        recompute_dxbc_hash_inplace(work);
    }
    if (pOutTriplesPatched) *pOutTriplesPatched = patched;
    if (!pOutDxbc) return S_OK;
    if (outCapacity < work.size()) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::memcpy(pOutDxbc, work.data(), work.size());
    return S_OK;
}

ULONG_PTR WINAPI D3D12XboxRemapThreadAffinityMask(ULONG_PTR xboxAffinityMask) {
    // Xbox Series X|S has 8 cores / 16 logical threads (bits 0..15 = 0xFFFF).
    // Any empty mask or mask containing bits outside the 16 console cores (and outside host cores)
    // is rejected with ERROR_INVALID_PARAMETER, matching Xbox OS kernel32legacy behavior.
    DWORD_PTR proc_mask = 0u, sys_mask = 0u;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask) || proc_mask == 0u) {
        proc_mask = 0xFFFu;
    }
    const ULONG_PTR allowed_universe = static_cast<ULONG_PTR>(0xFFFFULL) | static_cast<ULONG_PTR>(proc_mask);
    if (xboxAffinityMask == 0u || (xboxAffinityMask & ~allowed_universe) != 0u) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0u;
    }

    // Collect available logical processor bit masks on the host CPU (e.g. 12 threads on i5-11400F -> 0x0FFF)
    ULONG_PTR valid_bits[64]{};
    uint32_t valid_count = 0u;
    for (uint32_t b = 0u; b < 64u; ++b) {
        const ULONG_PTR bit = (static_cast<ULONG_PTR>(1u) << b);
        if ((proc_mask & bit) != 0u) {
            valid_bits[valid_count++] = bit;
        }
    }
    if (valid_count == 0u) return static_cast<ULONG_PTR>(proc_mask);

    // Fold every set Xbox core bit b (0..15) onto valid_bits[b % valid_count] so cores 12..15 never fail on <16t CPUs
    ULONG_PTR remapped = 0u;
    for (uint32_t b = 0u; b < 64u; ++b) {
        const ULONG_PTR bit = (static_cast<ULONG_PTR>(1u) << b);
        if ((xboxAffinityMask & bit) != 0u) {
            remapped |= valid_bits[b % valid_count];
        }
    }
    return remapped ? remapped : valid_bits[0];
}

HRESULT WINAPI MFResetDXGIDeviceManagerX(
    IUnknown* pManager,
    IUnknown* pUnkDevice,
    UINT resetToken) {
    if (!pManager || !pUnkDevice) return E_INVALIDARG;

    // 1. Unwrap WsxXboxDeviceAbiProxy -> WsxD3D12DeviceProxy if wrapped in raw Xbox binary vtable
    IUnknown* cur_dev = pUnkDevice;
    ID3D12Device* abi_unwrapped = nullptr;
    if (SUCCEEDED(cur_dev->QueryInterface(__uuidof(wsx_d3d12::IWsxXboxDeviceAbiMarker),
                                          reinterpret_cast<void**>(&abi_unwrapped))) && abi_unwrapped) {
        cur_dev = abi_unwrapped;
    }

    // 2. Unwrap WsxD3D12DeviceProxy -> raw desktop ID3D12Device
    WsxD3D12DeviceProxy* dev_proxy = nullptr;
    if (SUCCEEDED(cur_dev->QueryInterface(__uuidof(IWsxD3D12DeviceProxyMarker),
                                          reinterpret_cast<void**>(&dev_proxy))) && dev_proxy) {
        cur_dev = dev_proxy->raw_device();
    }

    // 3. Invoke IMFDXGIDeviceManager::ResetDevice (vtable slot 6 on IMFDXGIDeviceManager)
    //    IMFDXGIDeviceManager vtable: 0=QI, 1=AddRef, 2=Release, 3=CloseDeviceHandle, 4=GetVideoService,
    //    5=LockDevice, 6=ResetDevice(IUnknown* pUnkDevice, UINT resetToken), 7=TestDevice, 8=UnlockDevice
    auto** vt = *reinterpret_cast<void***>(pManager);
    if (!vt || !vt[6]) return E_POINTER;
    using PfnResetDevice = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, IUnknown*, UINT);
    auto fn_reset = reinterpret_cast<PfnResetDevice>(vt[6]);
    return fn_reset(pManager, cur_dev, resetToken);
}

HRESULT WINAPI D3DMapEsramMemory(
    UINT Flags,
    void* VirtualAddress,
    UINT NumPages,
    const UINT* PageList) {
    (void)PageList;
    // Console d3d12_x / d3d12_xs ESRAM mapper accepts Flags 1 or 3 and maps NumPages * 64 KiB at VirtualAddress
    if ((Flags != 1u && Flags != 3u) || !VirtualAddress || NumPages == 0u) {
        return E_INVALIDARG;
    }
    const SIZE_T bytes = static_cast<SIZE_T>(NumPages) * 65536ULL;
    if (VirtualAlloc(VirtualAddress, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr) {
        return S_OK;
    }
    if (VirtualAlloc(VirtualAddress, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != nullptr) {
        return S_OK;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(VirtualAddress, &mbi, sizeof(mbi)) == sizeof(mbi) &&
        mbi.State == MEM_COMMIT && mbi.RegionSize >= bytes) {
        DWORD old_prot = 0u;
        VirtualProtect(VirtualAddress, bytes, PAGE_READWRITE, &old_prot);
        return S_OK;
    }
    return E_OUTOFMEMORY;
}

} // extern "C"

namespace {
struct BuiltinRdna2Registrar {
    BuiltinRdna2Registrar() noexcept {
        wsx_shader::register_builtin_fssm_translator(&wsx_rdna2::builtin_fssm_to_dxil);
    }
};
const BuiltinRdna2Registrar g_builtin_rdna2_registrar{};
} // namespace

static_assert(std::is_same_v<decltype(&WsxCreateRootSignatureDeserializer),
                             decltype(&D3D12CreateRootSignatureDeserializer)>);
static_assert(std::is_same_v<decltype(&WsxSerializeVersionedRootSignature),
                             decltype(&D3D12SerializeVersionedRootSignature)>);
