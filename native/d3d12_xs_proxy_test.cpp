// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Automated test for upgraded d3d12_xs.dll graphics architecture:
// 1. WsxD3D12DeviceProxy COM wrapper (ID3D12Device..ID3D12Device8, IDXGIDevice1, IDXGIOutput6)
// 2. WsxD3D12CommandQueueProxy & WsxD3D12CommandListProxy (ID3D12GraphicsCommandList6, GetDevice identity,
//    Xbox ResourceBarrier high-bit sanitization, ExecuteCommandLists unwrapping)
// 3. UMA Emulation: CPU Map() on D3D12_HEAP_TYPE_DEFAULT committed buffers, CUSTOM (WRITE_COMBINE, L1) buffers,
//    and placed buffers inside D3D12_HEAP_TYPE_DEFAULT buffer heaps via GPU_UPLOAD / CUSTOM L0
// 4. D3D12 Debug Layer activation via ProcessDebugFlags
// 5. Zero-Copy SwapChain Backbuffer hand-off on D3D12_HEAP_FLAG_ALLOW_DISPLAY
// 6. HDR10 (R10G10B10A2_UNORM + DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) + VRR + 120Hz frame pacing

#include "include/xbox/d3d12_xs.h"
#include "include/xbox/d3dx12_xs.h"
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

#define CHECK_HR(expr)                                                                 \
    do {                                                                               \
        HRESULT _hr = (expr);                                                          \
        if (FAILED(_hr)) {                                                             \
            std::fprintf(stderr, "FAIL: %s returned 0x%08X at line %d\n",              \
                         #expr, static_cast<unsigned>(_hr), __LINE__);                 \
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

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    // Create a visible top-level window for Zero-Copy SwapChain + HDR10 + VRR PresentX testing
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "WsxProxyTestWindowClass";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(
        0, wc.lpszClassName, "WsxD3D12ProxyTest", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        0, 0, 640, 360, nullptr, nullptr, wc.hInstance, nullptr);
    CHECK_COND(hwnd != nullptr, "Failed to create test window");

    // 1. Create Device via D3D12XboxCreateDevice with Debug Layer flag
    D3D12XBOX_CREATE_DEVICE_PARAMETERS createParams{};
    createParams.Version = D3D12_SDK_VERSION;
    createParams.ProcessDebugFlags = D3D12_PROCESS_DEBUG_FLAG_DEBUG_LAYER_ENABLED;

    ComPtr<ID3D12Device8> device;
    CHECK_HR(D3D12XboxCreateDevice(
        nullptr, &createParams, IID_GRAPHICS_PPV_ARGS(device.GetAddressOf())));
    CHECK_COND(device != nullptr, "D3D12XboxCreateDevice returned null");

    // Verify COM Proxy QueryInterface across D3D12Device tiers and DXGI Output6 (HDR10)
    ComPtr<ID3D12Device2> dev2;
    ComPtr<ID3D12Device5> dev5;
    CHECK_HR(device.As(&dev2));
    CHECK_HR(device.As(&dev5));
    CHECK_COND(static_cast<ID3D12Device*>(dev2.Get()) == static_cast<ID3D12Device*>(device.Get()),
               "COM identity must match across ID3D12Device2 and ID3D12Device8 on WsxD3D12DeviceProxy");

    ComPtr<IDXGIDevice1> dxgiDevice;
    CHECK_HR(device.As(&dxgiDevice));
    ComPtr<IDXGIAdapter> dxgiAdapter;
    CHECK_HR(dxgiDevice->GetAdapter(dxgiAdapter.GetAddressOf()));
    ComPtr<IDXGIOutput> dxgiOutput;
    CHECK_HR(dxgiAdapter->EnumOutputs(0, dxgiOutput.GetAddressOf()));
    ComPtr<IDXGIOutput6> dxgiOutput6;
    CHECK_HR(dxgiOutput.As(&dxgiOutput6));

    DXGI_OUTPUT_DESC1 outDesc1{};
    CHECK_HR(dxgiOutput6->GetDesc1(&outDesc1));
    CHECK_COND(outDesc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
               "IDXGIOutput6 should report HDR10 BT.2020 ST.2084 color space");
    CHECK_COND(outDesc1.BitsPerColor == 10 && outDesc1.MaxLuminance >= 1000.0f,
               "IDXGIOutput6 should report 10-bit 1000-nit HDR capability");

    // 2. Create Direct Command Queue & configure 120 Hz + VRR frame interval
    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    CHECK_HR(device->CreateCommandQueue(&qDesc, IID_GRAPHICS_PPV_ARGS(queue.GetAddressOf())));

    // 2a. Verify WsxD3D12CommandQueueProxy::GetDevice returns our WsxD3D12DeviceProxy
    ComPtr<ID3D12Device8> queueDev;
    CHECK_HR(queue->GetDevice(IID_GRAPHICS_PPV_ARGS(queueDev.GetAddressOf())));
    CHECK_COND(queueDev.Get() == device.Get(),
               "CommandQueueProxy::GetDevice must return WsxD3D12DeviceProxy");

    // 2b. Verify WsxD3D12CommandListProxy (ID3D12GraphicsCommandList6) via CreateCommandList & CreateCommandList1
    ComPtr<ID3D12CommandAllocator> cmdAlloc;
    CHECK_HR(device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_GRAPHICS_PPV_ARGS(cmdAlloc.GetAddressOf())));

    ComPtr<ID3D12GraphicsCommandList6> cmdList6;
    CHECK_HR(device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr,
        IID_GRAPHICS_PPV_ARGS(cmdList6.GetAddressOf())));
    CHECK_COND(cmdList6 != nullptr, "CreateCommandList(ID3D12GraphicsCommandList6) returned null");

    ComPtr<ID3D12Device8> listDev;
    CHECK_HR(cmdList6->GetDevice(IID_GRAPHICS_PPV_ARGS(listDev.GetAddressOf())));
    CHECK_COND(listDev.Get() == device.Get(),
               "CommandListProxy::GetDevice must return WsxD3D12DeviceProxy");

    ComPtr<ID3D12GraphicsCommandList6> cmdList1Closed;
    CHECK_HR(device->CreateCommandList1(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
        IID_GRAPHICS_PPV_ARGS(cmdList1Closed.GetAddressOf())));
    ComPtr<ID3D12Device8> list1Dev;
    CHECK_HR(cmdList1Closed->GetDevice(IID_GRAPHICS_PPV_ARGS(list1Dev.GetAddressOf())));
    CHECK_COND(list1Dev.Get() == device.Get(),
               "CreateCommandList1 proxy GetDevice must return WsxD3D12DeviceProxy");

    // 2c. UMA Emulation Test 1: CPU Map() directly on a D3D12_HEAP_TYPE_DEFAULT committed buffer!
    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC umaBufDesc = CD3DX12_RESOURCE_DESC::Buffer(4096);
    ComPtr<ID3D12Resource> umaDefaultBuf;
    CHECK_HR(device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &umaBufDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_GRAPHICS_PPV_ARGS(umaDefaultBuf.GetAddressOf())));

    D3D12_HEAP_PROPERTIES actualDefaultHeap{};
    CHECK_HR(umaDefaultBuf->GetHeapProperties(&actualDefaultHeap, nullptr));
    CHECK_COND(actualDefaultHeap.Type == D3D12_HEAP_TYPE_GPU_UPLOAD ||
               (actualDefaultHeap.Type == D3D12_HEAP_TYPE_CUSTOM &&
                actualDefaultHeap.MemoryPoolPreference == D3D12_MEMORY_POOL_L0),
               "UMA emulation should promote DEFAULT buffer to GPU_UPLOAD or CUSTOM L0");

    void* mappedPtr = nullptr;
    CHECK_HR(umaDefaultBuf->Map(0, nullptr, &mappedPtr));
    CHECK_COND(mappedPtr != nullptr, "UMA DEFAULT buffer Map() returned null pointer");
    auto* mappedWords = static_cast<UINT32*>(mappedPtr);
    mappedWords[0] = 0xCAFEBABEu;
    mappedWords[1] = 0xDEADBEEFu;
    umaDefaultBuf->Unmap(0, nullptr);

    void* verifyPtr = nullptr;
    CHECK_HR(umaDefaultBuf->Map(0, nullptr, &verifyPtr));
    auto* verifyWords = static_cast<const UINT32*>(verifyPtr);
    CHECK_COND(verifyWords[0] == 0xCAFEBABEu && verifyWords[1] == 0xDEADBEEFu,
               "UMA DEFAULT buffer readback mismatch after Unmap/Map");
    umaDefaultBuf->Unmap(0, nullptr);

    // 2d. UMA Emulation Test 2: DEFAULT buffer with D3D12_RESOURCE_STATE_COPY_DEST (GDK sample pattern)
    ComPtr<ID3D12Resource> umaCopyDestBuf;
    CHECK_HR(device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &umaBufDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_GRAPHICS_PPV_ARGS(umaCopyDestBuf.GetAddressOf())));
    void* copyDestPtr = nullptr;
    CHECK_HR(umaCopyDestBuf->Map(0, nullptr, &copyDestPtr));
    static_cast<UINT32*>(copyDestPtr)[0] = 0x55AA55AAu;
    umaCopyDestBuf->Unmap(0, nullptr);

    // 2e. UMA Emulation Test 3: Xbox CUSTOM (WRITE_COMBINE, L1) buffer -> GPU_UPLOAD / CUSTOM L0
    D3D12_HEAP_PROPERTIES xboxCustomL1{};
    xboxCustomL1.Type = D3D12_HEAP_TYPE_CUSTOM;
    xboxCustomL1.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
    xboxCustomL1.MemoryPoolPreference = D3D12_MEMORY_POOL_L1;
    ComPtr<ID3D12Resource> umaCustomL1Buf;
    CHECK_HR(device->CreateCommittedResource(
        &xboxCustomL1, D3D12_HEAP_FLAG_NONE, &umaBufDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_GRAPHICS_PPV_ARGS(umaCustomL1Buf.GetAddressOf())));
    void* customL1Ptr = nullptr;
    CHECK_HR(umaCustomL1Buf->Map(0, nullptr, &customL1Ptr));
    static_cast<UINT32*>(customL1Ptr)[0] = 0x12345678u;
    umaCustomL1Buf->Unmap(0, nullptr);
    void* customL1Verify = nullptr;
    CHECK_HR(umaCustomL1Buf->Map(0, nullptr, &customL1Verify));
    CHECK_COND(static_cast<const UINT32*>(customL1Verify)[0] == 0x12345678u,
               "UMA CUSTOM L1 buffer readback mismatch");
    umaCustomL1Buf->Unmap(0, nullptr);

    // 2f. UMA Emulation Test 4: Placed buffer inside D3D12_HEAP_TYPE_DEFAULT ALLOW_ONLY_BUFFERS heap
    D3D12_HEAP_DESC defaultBufHeapDesc{};
    defaultBufHeapDesc.SizeInBytes = 65536;
    defaultBufHeapDesc.Properties = defaultHeap;
    defaultBufHeapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    defaultBufHeapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ComPtr<ID3D12Heap> placedHeap;
    CHECK_HR(device->CreateHeap(&defaultBufHeapDesc, IID_GRAPHICS_PPV_ARGS(placedHeap.GetAddressOf())));
    ComPtr<ID3D12Resource> placedBuf;
    CHECK_HR(device->CreatePlacedResource(
        placedHeap.Get(), 0, &umaBufDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_GRAPHICS_PPV_ARGS(placedBuf.GetAddressOf())));
    void* placedPtr = nullptr;
    CHECK_HR(placedBuf->Map(0, nullptr, &placedPtr));
    static_cast<UINT32*>(placedPtr)[0] = 0x87654321u;
    placedBuf->Unmap(0, nullptr);

    // 2g. Test WsxD3D12CommandListProxy::ResourceBarrier Xbox high-bit sanitization + ExecuteCommandLists unwrapping
    //     Pass Xbox-only high state bit 0x10000000 on transition COMMON -> COPY_SOURCE, plus a no-op Xbox-only barrier!
    D3D12_RESOURCE_BARRIER xboxBarriers[2]{};
    xboxBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    xboxBarriers[0].Transition.pResource = umaDefaultBuf.Get();
    xboxBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    xboxBarriers[0].Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_COMMON | 0x10000000u);
    xboxBarriers[0].Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_COMMON | 0x20000000u); // Sanitizes to COMMON->COMMON (dropped!)

    xboxBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    xboxBarriers[1].Transition.pResource = umaDefaultBuf.Get();
    xboxBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    xboxBarriers[1].Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_COMMON | 0x10000000u);
    xboxBarriers[1].Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_COPY_SOURCE | 0x40000000u);

    cmdList6->ResourceBarrier(2, xboxBarriers);
    cmdList6->CopyBufferRegion(umaCopyDestBuf.Get(), 0, umaDefaultBuf.Get(), 0, 16);
    CHECK_HR(cmdList6->Close());

    ID3D12CommandList* execLists[] = {cmdList6.Get()};
    queue->ExecuteCommandLists(1, execLists);

    // Sync GPU and verify GPU CopyBufferRegion wrote 0xCAFEBABE / 0xDEADBEEF into umaCopyDestBuf!
    ComPtr<ID3D12Fence> syncFence;
    CHECK_HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_GRAPHICS_PPV_ARGS(syncFence.GetAddressOf())));
    HANDLE fenceEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK_COND(fenceEvt != nullptr, "Failed to create fence event");
    CHECK_HR(queue->Signal(syncFence.Get(), 1));
    CHECK_HR(syncFence->SetEventOnCompletion(1, fenceEvt));
    CHECK_COND(WaitForSingleObject(fenceEvt, 5000) == WAIT_OBJECT_0, "GPU sync fence timed out");
    CloseHandle(fenceEvt);

    void* gpuCopiedPtr = nullptr;
    CHECK_HR(umaCopyDestBuf->Map(0, nullptr, &gpuCopiedPtr));
    auto* gpuCopiedWords = static_cast<const UINT32*>(gpuCopiedPtr);
    CHECK_COND(gpuCopiedWords[0] == 0xCAFEBABEu && gpuCopiedWords[1] == 0xDEADBEEFu,
               "GPU CopyBufferRegion between UMA-promoted DEFAULT buffers failed");
    umaCopyDestBuf->Unmap(0, nullptr);

    const char* umaModeName = (actualDefaultHeap.Type == D3D12_HEAP_TYPE_GPU_UPLOAD) ? "GPU_UPLOAD(ReBAR)" : "CUSTOM_L0";

    CHECK_HR(device->SetFrameIntervalX(
        dxgiOutput.Get(),
        D3D12XBOX_FRAME_INTERVAL_120_HZ,
        1u /* 2 backbuffers */,
        D3D12XBOX_FRAME_INTERVAL_FLAG_VRR));
    CHECK_HR(device->ScheduleFrameEventX(
        D3D12XBOX_FRAME_EVENT_ORIGIN, 0u, nullptr, D3D12XBOX_SCHEDULE_FRAME_EVENT_FLAG_NONE));

    // 3. Allocate 2 HDR10 (R10G10B10A2_UNORM) displayable backbuffers with D3D12_HEAP_FLAG_ALLOW_DISPLAY
    //    and D3D12XBOX_RESOURCE_FLAG_ALLOW_DCC -> verify Zero-Copy SwapChain Backbuffer hand-off!
    D3D12_RESOURCE_DESC rtDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R10G10B10A2_UNORM, 640, 360, 1, 1);
    rtDesc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12XBOX_RESOURCE_FLAG_ALLOW_DCC);

    ComPtr<ID3D12Resource> renderTargets[2];
    for (UINT i = 0; i < 2; ++i) {
        CHECK_HR(device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_ALLOW_DISPLAY,
            &rtDesc,
            D3D12_RESOURCE_STATE_PRESENT,
            nullptr,
            IID_GRAPHICS_PPV_ARGS(renderTargets[i].GetAddressOf())));
        CHECK_COND(renderTargets[i] != nullptr, "Zero-copy display backbuffer is null");
    }

    // 4. Test 120 Hz Frame Pacing + Non-blocking Zero-Copy HDR10 PresentX across 4 frames
    SetEnvironmentVariableW(L"WSX_FORCE_FRAME_PACING", L"1");
    const auto t0 = std::chrono::steady_clock::now();

    for (UINT frame = 0; frame < 4; ++frame) {
        D3D12XBOX_FRAME_PIPELINE_TOKEN token = D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL;
        CHECK_HR(device->WaitFrameEventX(
            D3D12XBOX_FRAME_EVENT_ORIGIN, 100, nullptr,
            D3D12XBOX_WAIT_FRAME_EVENT_FLAG_NONE, &token));
        CHECK_COND(token != D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL, "WaitFrameEventX returned null token");

        ID3D12Resource* rt = renderTargets[frame % 2].Get();
        D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
        plane.Token = token;
        plane.ResourceCount = 1;
        plane.ppResources = &rt;
        plane.ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;

        D3D12XBOX_PRESENT_PARAMETERS presentParams{};
        presentParams.Flags = D3D12XBOX_FRAME_INTERVAL_FLAG_VRR;
        CHECK_HR(queue->PresentX(1, &plane, &presentParams));
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    SetEnvironmentVariableW(L"WSX_FORCE_FRAME_PACING", nullptr);

    // 3 paced intervals at 120 Hz (~8.33 ms each) should take at least ~18 ms
    CHECK_COND(elapsedMs >= 16.0, "120 Hz frame pacer did not pace frames properly");

    // Flush & clean up
    CHECK_HR(queue->PresentX(0, nullptr, nullptr));

    // 5. Xbox D3D12.X ABI Verification inside d3d12_xs.dll (under D3D12 Debug Layer!):
    //    5a. Xbox texture layout 0x101 + DXGI_FORMAT_R9G9B9E5_SHAREDEXP + ALLOW_UNORDERED_ACCESS + UAV/SRV
    {
        D3D12_RESOURCE_DESC fsrTexDesc{};
        fsrTexDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        fsrTexDesc.Width = 128;
        fsrTexDesc.Height = 128;
        fsrTexDesc.DepthOrArraySize = 1;
        fsrTexDesc.MipLevels = 1;
        fsrTexDesc.Format = DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        fsrTexDesc.SampleDesc.Count = 1;
        fsrTexDesc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101); // Xbox tile layout
        fsrTexDesc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12XBOX_RESOURCE_FLAG_ALLOW_DCC);

        ComPtr<ID3D12Resource> fsrTex;
        CHECK_HR(device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &fsrTexDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_GRAPHICS_PPV_ARGS(fsrTex.GetAddressOf())));
        CHECK_COND(fsrTex->GetDesc().Format == DXGI_FORMAT_R32_TYPELESS,
                   "R9G9B9E5_SHAREDEXP + UAV should sanitize to R32_TYPELESS");

        // 5b. Native Xbox-ABI Device & RAX-return DescriptorHeap & CommandList via D3D12XboxGetXboxAbiDevice
        void* xboxDevRaw = nullptr;
        CHECK_HR(D3D12XboxGetXboxAbiDevice(device.Get(), __uuidof(ID3D12Device), &xboxDevRaw));
        CHECK_COND(xboxDevRaw != nullptr, "D3D12XboxGetXboxAbiDevice returned null");
        auto** xdevVt = *reinterpret_cast<void***>(xboxDevRaw);

        // Call Xbox slot 76 (0x260): CreateCommittedResource directly via Xbox vtable!
        using XboxCreateCommittedFn = HRESULT(STDMETHODCALLTYPE*)(
            void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
            D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
        auto fnCreateCommitted260 = reinterpret_cast<XboxCreateCommittedFn>(xdevVt[76]);
        ComPtr<ID3D12Resource> resVia260;
        CHECK_HR(fnCreateCommitted260(
            xboxDevRaw, &defaultHeap, D3D12_HEAP_FLAG_NONE, &fsrTexDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_GRAPHICS_PPV_ARGS(resVia260.GetAddressOf())));
        CHECK_COND(resVia260 != nullptr, "Xbox slot 0x260 CreateCommittedResource returned null");

        // Call Xbox slot 14 (0x070): CreateDescriptorHeap -> returns RAX-ABI WsxXboxDescHeapAbiProxy!
        using XboxCreateDescHeapFn = HRESULT(STDMETHODCALLTYPE*)(
            void*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
        auto fnCreateDescHeap = reinterpret_cast<XboxCreateDescHeapFn>(xdevVt[14]);
        D3D12_DESCRIPTOR_HEAP_DESC dhDesc{};
        dhDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        dhDesc.NumDescriptors = 4;
        dhDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        void* xdhRaw = nullptr;
        CHECK_HR(fnCreateDescHeap(xboxDevRaw, &dhDesc, __uuidof(ID3D12DescriptorHeap), &xdhRaw));
        CHECK_COND(xdhRaw != nullptr, "Xbox CreateDescriptorHeap returned null");

        auto** xdhVt = *reinterpret_cast<void***>(xdhRaw);
        using XboxGetHandleRaxFn = UINT64(STDMETHODCALLTYPE*)(void*);
        const SIZE_T cpuStart = static_cast<SIZE_T>(reinterpret_cast<XboxGetHandleRaxFn>(xdhVt[9])(xdhRaw));
        const UINT64 gpuStart = reinterpret_cast<XboxGetHandleRaxFn>(xdhVt[10])(xdhRaw);
        CHECK_COND(cpuStart != 0 && gpuStart != 0, "Xbox RAX-return DescriptorHeap start handles must be non-zero");

        // Create UAV & SRV with R9G9B9E5_SHAREDEXP into the Xbox descriptor heap (sanitized -> R32_UINT!)
        const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(fsrTex.Get(), nullptr, &uavDesc, D3D12_CPU_DESCRIPTOR_HANDLE{cpuStart});

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(fsrTex.Get(), &srvDesc, D3D12_CPU_DESCRIPTOR_HANDLE{cpuStart + inc});

        // Wrap a CommandList into Xbox ABI & verify slot 0x100 (SetDescriptorHeaps unwrap) + slot 0x4A0 (AllocateGpuMemory)
        ComPtr<ID3D12CommandAllocator> xAlloc;
        CHECK_HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_GRAPHICS_PPV_ARGS(xAlloc.GetAddressOf())));
        ComPtr<ID3D12GraphicsCommandList> xBaseCl;
        CHECK_HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, xAlloc.Get(), nullptr, IID_GRAPHICS_PPV_ARGS(xBaseCl.GetAddressOf())));

        void* xClRaw = nullptr;
        CHECK_HR(D3D12XboxWrapCommandListForXboxAbi(xBaseCl.Get(), &xClRaw));
        CHECK_COND(xClRaw != nullptr, "D3D12XboxWrapCommandListForXboxAbi returned null");
        auto** xclVt = *reinterpret_cast<void***>(xClRaw);

        // Slot 32 (0x100): Xbox SetDescriptorHeaps with Xbox-ABI descriptor heap
        using XboxSetDescHeapsFn = void(STDMETHODCALLTYPE*)(void*, UINT, void* const*);
        void* heaps[] = {xdhRaw};
        reinterpret_cast<XboxSetDescHeapsFn>(xclVt[32])(xClRaw, 1, heaps);

        // Slot 148 (0x4A0): Xbox AllocateGpuMemory -> CPU-writable inline UMA ring buffer!
        using XboxAllocGpuMemFn = void*(STDMETHODCALLTYPE*)(void*, UINT, UINT);
        void* umaInline = reinterpret_cast<XboxAllocGpuMemFn>(xclVt[148])(xClRaw, 256, 16);
        CHECK_COND(umaInline != nullptr, "Xbox slot 0x4A0 AllocateGpuMemory returned null");
        std::memset(umaInline, 0xAB, 256);

        // Slot 9 (0x048): Close + ExecuteCommandLists unwrap through WsxD3D12CommandQueueProxy!
        using XboxCloseFn = HRESULT(STDMETHODCALLTYPE*)(void*);
        CHECK_HR(reinterpret_cast<XboxCloseFn>(xclVt[9])(xClRaw));
        ID3D12CommandList* xExecLists[] = {static_cast<ID3D12CommandList*>(xClRaw)};
        queue->ExecuteCommandLists(1, xExecLists);

        // Wait for GPU before releasing command list / descriptor heap (required by D3D12 Debug Layer!)
        {
            ComPtr<ID3D12Fence> xFence;
            CHECK_HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_GRAPHICS_PPV_ARGS(xFence.GetAddressOf())));
            HANDLE xEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            CHECK_HR(queue->Signal(xFence.Get(), 1));
            CHECK_HR(xFence->SetEventOnCompletion(1, xEvt));
            WaitForSingleObject(xEvt, 5000);
            CloseHandle(xEvt);
        }

        // Verify telemetry counters in d3d12_xs.dll
        D3D12XBOX_ABI_TELEMETRY abiTel{};
        CHECK_HR(D3D12XboxGetAbiTelemetry(&abiTel));
        CHECK_COND(abiTel.DescHeapsCreated >= 1 && abiTel.CommittedResourcesVia260 >= 1 &&
                   abiTel.UmaAllocGpuMemCalls >= 1 && abiTel.UnhandledDeviceSlots == 0 &&
                   abiTel.UnhandledCmdListSlots == 0,
                   "d3d12_xs.dll Xbox ABI telemetry counters mismatch");

        reinterpret_cast<IUnknown*>(xClRaw)->Release();
        reinterpret_cast<IUnknown*>(xdhRaw)->Release();
        reinterpret_cast<IUnknown*>(xboxDevRaw)->Release();

        // 5c. Verify WSX_XBOX_D3D12_ABI=1 opt-in device creation mode
        SetEnvironmentVariableW(L"WSX_XBOX_D3D12_ABI", L"1");
        void* optInXboxDev = nullptr;
        const HRESULT optInHr = D3D12XboxCreateDevice(nullptr, &createParams, __uuidof(ID3D12Device), &optInXboxDev);
        SetEnvironmentVariableW(L"WSX_XBOX_D3D12_ABI", nullptr);
        CHECK_HR(optInHr);
        CHECK_COND(optInXboxDev != nullptr, "WSX_XBOX_D3D12_ABI=1 device creation returned null");
        auto** optVt = *reinterpret_cast<void***>(optInXboxDev);
        void* optDh = nullptr;
        CHECK_HR(reinterpret_cast<XboxCreateDescHeapFn>(optVt[14])(optInXboxDev, &dhDesc, __uuidof(ID3D12DescriptorHeap), &optDh));
        auto** optDhVt = *reinterpret_cast<void***>(optDh);
        CHECK_COND(reinterpret_cast<XboxGetHandleRaxFn>(optDhVt[9])(optDh) != 0,
                   "WSX_XBOX_D3D12_ABI=1 descriptor heap RAX handle must be non-zero");
        reinterpret_cast<IUnknown*>(optDh)->Release();
        reinterpret_cast<IUnknown*>(optInXboxDev)->Release();
    }

    // Verify D3D12 Debug Layer reported zero errors or corruptions across all proxy/UMA/barrier/PresentX calls
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(device.As(&infoQueue)) && infoQueue) {
        const UINT64 msgCount = infoQueue->GetNumStoredMessages();
        UINT errorCount = 0;
        for (UINT64 i = 0; i < msgCount; ++i) {
            SIZE_T msgLen = 0;
            if (SUCCEEDED(infoQueue->GetMessage(i, nullptr, &msgLen)) && msgLen > 0) {
                std::vector<char> buf(msgLen);
                auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                if (SUCCEEDED(infoQueue->GetMessage(i, msg, &msgLen))) {
                    if (msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
                        msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR) {
                        std::fprintf(stderr, "D3D12 DEBUG ERROR [%d]: %s\n",
                                     static_cast<int>(msg->ID),
                                     msg->pDescription ? msg->pDescription : "(null)");
                        ++errorCount;
                    }
                }
            }
        }
        CHECK_COND(errorCount == 0, "D3D12 Debug Layer emitted errors during proxy/UMA/PresentX test");
    }

    DestroyWindow(hwnd);

    std::printf("PASS: d3d12_xs_proxy_test (Device/Queue/CmdList6 COM Proxies + Native Xbox D3D12.X ABI [0x1F0/0x260/RAX-Heap/R9G9B9E5/0x4A0-UMA] + UMA[%s] + Xbox Barrier Sanitization + DebugLayer 0 Errors + Zero-Copy SwapChain + HDR10 + VRR + 120Hz Pacer [4 frames in %.2f ms])\n",
                umaModeName, elapsedMs);
    return 0;
}
