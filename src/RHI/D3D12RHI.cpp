#include "RHI/D3D12RHI.h"

#include <stdexcept>

#include "Core/Diagnostics.h"

/**
 * @brief 构造资源状态转换屏障。
 * @param res 需要转换状态的资源。
 * @param before 转换前的资源状态。
 * @param after 转换后的资源状态。
 * @return D3D12_RESOURCE_BARRIER 屏障描述。
 * @note 阶段：渲染帧资源状态切换阶段。
 */
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

/**
 * @brief 初始化 D3D12 设备、交换链、命令与同步对象。
 * @param hwnd 交换链宿主窗口句柄。
 * @param width 渲染宽度（像素）。
 * @param height 渲染高度（像素）。
 * @return 无返回值；失败时抛出异常。
 * @note 阶段：渲染系统初始化阶段。
 */
void FD3D12RHI::Init(HWND hwnd, uint32 width, uint32 height)
{
    // 保存窗口与尺寸信息。
    Hwnd = hwnd;
    Width = width;
    Height = height;
    AdapterName.clear();

#if defined(_DEBUG)
    {
        // 开启 D3D12 调试层，便于开发期错误定位。
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            debug->EnableDebugLayer();
    }
#endif

    // 创建 DXGI 工厂。
    UINT factoryFlags = 0;
#if defined(_DEBUG)
    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&Factory)), "CreateDXGIFactory2 failed");

    ComPtr<IDXGIAdapter1> adapter;
    // 检测适配器是否支持 DXR。
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

    // 选择合适的硬件适配器，并保存名称。
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

    // 优先选择支持 DXR 的适配器，不行则降级到普通 D3D12 适配器。
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

    // 创建设备与命令队列。
    ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&Device)), "D3D12CreateDevice failed");

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(Device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&Queue)), "CreateCommandQueue failed");

    // 创建交换链。
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

    // 创建 RTV 描述符堆并初始化交换链 RT。
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = kFrameCount;
    ThrowIfFailed(Device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&RTVHeap)), "CreateDescriptorHeap RTV failed");
    RTVDescriptorSize = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    CreateSwapchainResources();
    CreateDepthResources();

    // 创建命令分配器与命令列表。
    for (uint32 i = 0; i < kFrameCount; ++i)
        ThrowIfFailed(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&CmdAlloc[i])), "CreateCommandAllocator failed");

    ThrowIfFailed(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, CmdAlloc[FrameIndex].Get(), nullptr, IID_PPV_ARGS(&CmdList)),
                  "CreateCommandList failed");
    ThrowIfFailed(CmdList->Close(), "Close cmd list failed");

    // 创建同步用 Fence 与事件。
    ThrowIfFailed(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)), "CreateFence failed");
    FenceValue[FrameIndex] = 1;

    FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!FenceEvent) throw std::runtime_error("CreateEvent failed");
}

/**
 * @brief 关闭 RHI 并清理同步对象。
 * @param 无。
 * @return 无返回值。
 * @note 阶段：渲染系统销毁阶段。
 */
void FD3D12RHI::Shutdown()
{
    // 确保 GPU 已完成，避免资源被使用时释放。
    WaitForGPU();
    if (FenceEvent) CloseHandle(FenceEvent);
    FenceEvent = nullptr;
}

/**
 * @brief 处理窗口尺寸变化并重建交换链/深度资源。
 * @param width 新渲染宽度（像素）。
 * @param height 新渲染高度（像素）。
 * @return 无返回值。
 * @note 阶段：运行时窗口尺寸变化阶段。
 */
void FD3D12RHI::Resize(uint32 width, uint32 height)
{
    if (!Swapchain) return;
    if (width == 0 || height == 0) return;
    if (width == Width && height == Height) return;

    // 等待 GPU 完成后再释放资源。
    WaitForGPU();

    for (uint32 i = 0; i < kFrameCount; ++i)
        RenderTargets[i].Reset();
    DepthBuffer.Reset();

    Width = width;
    Height = height;

    // 调整交换链缓冲区并重新创建 RTV/DSV。
    ThrowIfFailed(Swapchain->ResizeBuffers(kFrameCount, Width, Height, GetBackBufferFormat(), 0), "ResizeBuffers failed");
    FrameIndex = Swapchain->GetCurrentBackBufferIndex();

    CreateSwapchainResources();
    CreateDepthResources();
}

/**
 * @brief 执行一次性命令列表并等待完成。
 * @param record 录制命令的回调函数。
 * @param user 回调用户数据指针。
 * @return 无返回值。
 * @note 阶段：资源上传/初始化的即时提交阶段。
 */
void FD3D12RHI::ExecuteImmediate(void(*record)(ID3D12GraphicsCommandList* cmd, void* user), void* user)
{
    if (!Device || !Queue || !record) return;

    // 创建临时命令分配器与命令列表。
    ComPtr<ID3D12CommandAllocator> alloc;
    ThrowIfFailed(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "ExecuteImmediate: CreateCommandAllocator failed");

    ComPtr<ID3D12GraphicsCommandList> list;
    ThrowIfFailed(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)),
                  "ExecuteImmediate: CreateCommandList failed");

    // 记录用户提交的命令。
    record(list.Get(), user);

    // 提交并等待 GPU 完成。
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

/**
 * @brief 创建交换链的后备缓冲 RTV。
 * @param 无。
 * @return 无返回值。
 * @note 阶段：交换链初始化/重建阶段。
 */
void FD3D12RHI::CreateSwapchainResources()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = RTVHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32 i = 0; i < kFrameCount; ++i)
    {
        // 获取后备缓冲并创建对应 RTV。
        ThrowIfFailed(Swapchain->GetBuffer(i, IID_PPV_ARGS(&RenderTargets[i])), "GetBuffer failed");
        Device->CreateRenderTargetView(RenderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += RTVDescriptorSize;
    }
}

/**
 * @brief 创建深度缓冲资源与 DSV。
 * @param 无。
 * @return 无返回值。
 * @note 阶段：深度资源初始化/重建阶段。
 */
void FD3D12RHI::CreateDepthResources()
{
    // 创建 DSV 描述符堆。
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

    // 创建深度纹理资源。
    ThrowIfFailed(Device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clearValue,
        IID_PPV_ARGS(&DepthBuffer)),
        "CreateCommittedResource depth failed");

    // 创建 DSV 视图。
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvView{};
    dsvView.Format = GetDepthFormat();
    dsvView.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvView.Flags = D3D12_DSV_FLAG_NONE;
    Device->CreateDepthStencilView(DepthBuffer.Get(), &dsvView, DSVHeap->GetCPUDescriptorHandleForHeapStart());
}

/**
 * @brief 开始一帧渲染，重置命令列表并切换到渲染目标状态。
 * @param 无。
 * @return 当前帧上下文（含 RT/DSV/命令列表）。
 * @note 阶段：渲染帧开始阶段。
 */
FD3D12FrameContext FD3D12RHI::BeginFrame()
{
    // 重置命令分配器与命令列表。
    ThrowIfFailed(CmdAlloc[FrameIndex]->Reset(), "CmdAlloc Reset failed");
    ThrowIfFailed(CmdList->Reset(CmdAlloc[FrameIndex].Get(), nullptr), "CmdList Reset failed");

    // 填充当前帧上下文数据。
    CurrentFrame = {};
    CurrentFrame.FrameIndex = FrameIndex;
    CurrentFrame.CmdList = CmdList.Get();
    CurrentFrame.BackBuffer = RenderTargets[FrameIndex].Get();

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = RTVHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(FrameIndex) * RTVDescriptorSize;
    CurrentFrame.RTV = rtv;
    CurrentFrame.DSV = DSVHeap->GetCPUDescriptorHandleForHeapStart();

    // 将后备缓冲从 Present 状态切换为 RenderTarget 状态。
    auto toRT = Transition(CurrentFrame.BackBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    CmdList->ResourceBarrier(1, &toRT);

    return CurrentFrame;
}

/**
 * @brief 结束当前帧，提交命令并执行 Present。
 * @param 无。
 * @return 无返回值。
 * @note 阶段：渲染帧提交阶段。
 */
void FD3D12RHI::EndFrame()
{
    // 切回 Present 状态。
    auto toPresent = Transition(CurrentFrame.BackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    CmdList->ResourceBarrier(1, &toPresent);

    // 提交命令列表并呈现。
    ThrowIfFailed(CmdList->Close(), "CmdList Close failed");
    ID3D12CommandList* lists[] = { CmdList.Get() };
    Queue->ExecuteCommandLists(1, lists);

    ThrowIfFailed(Swapchain->Present(1, 0), "Present failed");
    MoveToNextFrame();
}

/**
 * @brief 等待 GPU 完成当前帧命令。
 * @param 无。
 * @return 无返回值。
 * @note 阶段：同步/资源安全释放阶段。
 */
void FD3D12RHI::WaitForGPU()
{
    // 通过 fence 信号和事件等待 GPU 完成。
    const uint64 fenceToWait = FenceValue[FrameIndex];
    ThrowIfFailed(Queue->Signal(Fence.Get(), fenceToWait), "Queue Signal failed");
    ThrowIfFailed(Fence->SetEventOnCompletion(fenceToWait, FenceEvent), "SetEventOnCompletion failed");
    WaitForSingleObject(FenceEvent, INFINITE);
    FenceValue[FrameIndex]++;
}

/**
 * @brief 翻转到下一帧并同步 fence。
 * @param 无。
 * @return 无返回值。
 * @note 阶段：帧切换阶段。
 */
void FD3D12RHI::MoveToNextFrame()
{
    // 先对当前帧打信号，推进 fence。
    const uint64 currentFence = FenceValue[FrameIndex];
    ThrowIfFailed(Queue->Signal(Fence.Get(), currentFence), "Queue Signal failed");

    FrameIndex = Swapchain->GetCurrentBackBufferIndex();

    // 若下一帧未完成，等待 GPU。
    if (Fence->GetCompletedValue() < FenceValue[FrameIndex])
    {
        ThrowIfFailed(Fence->SetEventOnCompletion(FenceValue[FrameIndex], FenceEvent), "SetEventOnCompletion failed");
        WaitForSingleObject(FenceEvent, INFINITE);
    }

    FenceValue[FrameIndex] = currentFence + 1;
}
