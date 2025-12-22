#include "RHI/D3D12RHI.h"

#include <stdexcept>

#include "Core/Diagnostics.h"

static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

void FD3D12RHI::Init(HWND hwnd, uint32 width, uint32 height)
{
    Hwnd = hwnd;
    Width = width;
    Height = height;
    AdapterName.clear();

#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            debug->EnableDebugLayer();
    }
#endif

    UINT factoryFlags = 0;
#if defined(_DEBUG)
    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&Factory)), "CreateDXGIFactory2 failed");

    ComPtr<IDXGIAdapter1> adapter;
    auto supportsRaytracing = [&](IDXGIAdapter1* candidate) -> bool
    {
        if (!candidate)
            return false;

        ComPtr<ID3D12Device> testDevice;
        if (FAILED(D3D12CreateDevice(candidate, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&testDevice))))
            return false;

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
        if (FAILED(testDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5))))
            return false;

        return opt5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    };

    auto tryPick = [&](IDXGIAdapter1* candidate, bool requireRaytracing) -> bool
    {
        if (!candidate)
            return false;

        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            return false;

        if (requireRaytracing && !supportsRaytracing(candidate))
            return false;

        if (SUCCEEDED(D3D12CreateDevice(candidate, D3D_FEATURE_LEVEL_12_0, _uuidof(ID3D12Device), nullptr)))
        {
            AdapterName = desc.Description;
            return true;
        }
        return false;
    };

    ComPtr<IDXGIFactory6> factory6;
    const bool hasFactory6 = SUCCEEDED(Factory.As(&factory6));

    auto pickAdapter = [&](bool requireRaytracing) -> bool
    {
        // Prefer the high-performance adapter (avoids accidentally picking an iGPU without DXR).
        if (hasFactory6)
        {
            for (UINT i = 0;; ++i)
            {
                ComPtr<IDXGIAdapter1> candidate;
                if (factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND)
                    break;
                if (tryPick(candidate.Get(), requireRaytracing))
                {
                    adapter = candidate;
                    return true;
                }
            }
        }

        // Fallback: first hardware adapter that can create a D3D12 device.
        for (UINT i = 0; Factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            if (tryPick(adapter.Get(), requireRaytracing))
                return true;
            adapter.Reset();
        }
        return false;
    };

    if (!pickAdapter(true))
    {
        DebugOutput(L"DXR adapter not found; falling back to any D3D12 adapter.");
        pickAdapter(false);
    }

    if (!adapter)
    {
        ComPtr<IDXGIAdapter> warp;
        ThrowIfFailed(Factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "EnumWarpAdapter failed");
        ThrowIfFailed(warp.As(&adapter), "warp.As(adapter) failed");
        AdapterName = L"WARP";
    }

    if (!AdapterName.empty())
        DebugOutput(L"DX12 Adapter: " + AdapterName);

    ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&Device)), "D3D12CreateDevice failed");

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(Device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&Queue)), "CreateCommandQueue failed");

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.BufferCount = kFrameCount;
    scDesc.Width = Width;
    scDesc.Height = Height;
    scDesc.Format = GetBackBufferFormat();
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> sc1;
    ThrowIfFailed(Factory->CreateSwapChainForHwnd(Queue.Get(), Hwnd, &scDesc, nullptr, nullptr, &sc1),
                  "CreateSwapChainForHwnd failed");
    ThrowIfFailed(sc1.As(&Swapchain), "Swapchain cast failed");

    FrameIndex = Swapchain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = kFrameCount;
    ThrowIfFailed(Device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&RTVHeap)), "CreateDescriptorHeap RTV failed");
    RTVDescriptorSize = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    CreateSwapchainResources();
    CreateDepthResources();

    for (uint32 i = 0; i < kFrameCount; ++i)
        ThrowIfFailed(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&CmdAlloc[i])), "CreateCommandAllocator failed");

    ThrowIfFailed(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, CmdAlloc[FrameIndex].Get(), nullptr, IID_PPV_ARGS(&CmdList)),
                  "CreateCommandList failed");
    ThrowIfFailed(CmdList->Close(), "Close cmd list failed");

    ThrowIfFailed(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)), "CreateFence failed");
    FenceValue[FrameIndex] = 1;

    FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!FenceEvent) throw std::runtime_error("CreateEvent failed");
}

void FD3D12RHI::Shutdown()
{
    WaitForGPU();
    if (FenceEvent) CloseHandle(FenceEvent);
    FenceEvent = nullptr;
}

void FD3D12RHI::Resize(uint32 width, uint32 height)
{
    if (!Swapchain) return;
    if (width == 0 || height == 0) return;
    if (width == Width && height == Height) return;

    WaitForGPU();

    for (uint32 i = 0; i < kFrameCount; ++i)
        RenderTargets[i].Reset();
    DepthBuffer.Reset();

    Width = width;
    Height = height;

    ThrowIfFailed(Swapchain->ResizeBuffers(kFrameCount, Width, Height, GetBackBufferFormat(), 0), "ResizeBuffers failed");
    FrameIndex = Swapchain->GetCurrentBackBufferIndex();

    CreateSwapchainResources();
    CreateDepthResources();
}

void FD3D12RHI::ExecuteImmediate(void(*record)(ID3D12GraphicsCommandList* cmd, void* user), void* user)
{
    if (!Device || !Queue || !record) return;

    ComPtr<ID3D12CommandAllocator> alloc;
    ThrowIfFailed(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "ExecuteImmediate: CreateCommandAllocator failed");

    ComPtr<ID3D12GraphicsCommandList> list;
    ThrowIfFailed(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)),
                  "ExecuteImmediate: CreateCommandList failed");

    record(list.Get(), user);

    ThrowIfFailed(list->Close(), "ExecuteImmediate: Close failed");
    ID3D12CommandList* lists[] = { list.Get() };
    Queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    ThrowIfFailed(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "ExecuteImmediate: CreateFence failed");
    HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!evt) throw std::runtime_error("ExecuteImmediate: CreateEvent failed");

    ThrowIfFailed(Queue->Signal(fence.Get(), 1), "ExecuteImmediate: Signal failed");
    ThrowIfFailed(fence->SetEventOnCompletion(1, evt), "ExecuteImmediate: SetEventOnCompletion failed");
    WaitForSingleObject(evt, INFINITE);
    CloseHandle(evt);
}

void FD3D12RHI::CreateSwapchainResources()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = RTVHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32 i = 0; i < kFrameCount; ++i)
    {
        ThrowIfFailed(Swapchain->GetBuffer(i, IID_PPV_ARGS(&RenderTargets[i])), "GetBuffer failed");
        Device->CreateRenderTargetView(RenderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += RTVDescriptorSize;
    }
}

void FD3D12RHI::CreateDepthResources()
{
    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDesc.NumDescriptors = 1;
    ThrowIfFailed(Device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&DSVHeap)), "CreateDescriptorHeap DSV failed");

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depthDesc{};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = Width;
    depthDesc.Height = Height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = GetDepthFormat();
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = GetDepthFormat();
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    ThrowIfFailed(Device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clearValue,
        IID_PPV_ARGS(&DepthBuffer)),
        "CreateCommittedResource depth failed");

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvView{};
    dsvView.Format = GetDepthFormat();
    dsvView.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvView.Flags = D3D12_DSV_FLAG_NONE;
    Device->CreateDepthStencilView(DepthBuffer.Get(), &dsvView, DSVHeap->GetCPUDescriptorHandleForHeapStart());
}

FD3D12FrameContext FD3D12RHI::BeginFrame()
{
    ThrowIfFailed(CmdAlloc[FrameIndex]->Reset(), "CmdAlloc Reset failed");
    ThrowIfFailed(CmdList->Reset(CmdAlloc[FrameIndex].Get(), nullptr), "CmdList Reset failed");

    CurrentFrame = {};
    CurrentFrame.FrameIndex = FrameIndex;
    CurrentFrame.CmdList = CmdList.Get();
    CurrentFrame.BackBuffer = RenderTargets[FrameIndex].Get();

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = RTVHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(FrameIndex) * RTVDescriptorSize;
    CurrentFrame.RTV = rtv;
    CurrentFrame.DSV = DSVHeap->GetCPUDescriptorHandleForHeapStart();

    auto toRT = Transition(CurrentFrame.BackBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    CmdList->ResourceBarrier(1, &toRT);

    return CurrentFrame;
}

void FD3D12RHI::EndFrame()
{
    auto toPresent = Transition(CurrentFrame.BackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    CmdList->ResourceBarrier(1, &toPresent);

    ThrowIfFailed(CmdList->Close(), "CmdList Close failed");
    ID3D12CommandList* lists[] = { CmdList.Get() };
    Queue->ExecuteCommandLists(1, lists);

    ThrowIfFailed(Swapchain->Present(1, 0), "Present failed");
    MoveToNextFrame();
}

void FD3D12RHI::WaitForGPU()
{
    const uint64 fenceToWait = FenceValue[FrameIndex];
    ThrowIfFailed(Queue->Signal(Fence.Get(), fenceToWait), "Queue Signal failed");
    ThrowIfFailed(Fence->SetEventOnCompletion(fenceToWait, FenceEvent), "SetEventOnCompletion failed");
    WaitForSingleObject(FenceEvent, INFINITE);
    FenceValue[FrameIndex]++;
}

void FD3D12RHI::MoveToNextFrame()
{
    const uint64 currentFence = FenceValue[FrameIndex];
    ThrowIfFailed(Queue->Signal(Fence.Get(), currentFence), "Queue Signal failed");

    FrameIndex = Swapchain->GetCurrentBackBufferIndex();

    if (Fence->GetCompletedValue() < FenceValue[FrameIndex])
    {
        ThrowIfFailed(Fence->SetEventOnCompletion(FenceValue[FrameIndex], FenceEvent), "SetEventOnCompletion failed");
        WaitForSingleObject(FenceEvent, INFINITE);
    }

    FenceValue[FrameIndex] = currentFence + 1;
}
