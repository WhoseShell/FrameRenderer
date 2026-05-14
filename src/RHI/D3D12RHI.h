#pragma once

#include <string>
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "Core/Types.h"

using Microsoft::WRL::ComPtr;

struct FD3D12FrameContext
{
    uint32 FrameIndex = 0;
    ID3D12GraphicsCommandList* CmdList = nullptr;
    ID3D12Resource* BackBuffer = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE RTV{};
    D3D12_CPU_DESCRIPTOR_HANDLE DSV{};
};

class FD3D12RHI
{
public:
    static constexpr uint32 kFrameCount = 2;

    /**
     * @brief 初始化 D3D12 设备、交换链与帧资源。
     * @param hwnd 交换链宿主窗口句柄。
     * @param width 渲染宽度（像素）。
     * @param height 渲染高度（像素）。
     * @return 无返回值；失败时抛出异常。
     * @note 阶段：渲染系统初始化阶段。
     */
    void Init(HWND hwnd, uint32 width, uint32 height);
    /**
     * @brief 关闭 RHI 并释放同步资源。
     * @param 无。
     * @return 无返回值。
     * @note 阶段：渲染系统销毁阶段。
     */
    void Shutdown();
    /**
     * @brief 处理窗口尺寸变化并重建交换链资源。
     * @param width 新宽度（像素）。
     * @param height 新高度（像素）。
     * @return 无返回值。
     * @note 阶段：渲染帧/窗口事件处理阶段。
     */
    void Resize(uint32 width, uint32 height);
    /**
     * @brief 执行一次性命令列表（立即提交并等待完成）。
     * @param record 命令录制回调。
     * @param user 用户数据指针，透传给回调。
     * @return 无返回值。
     * @note 阶段：资源上传/初始化的即时提交阶段。
     */
    void ExecuteImmediate(void(*record)(ID3D12GraphicsCommandList* cmd, void* user), void* user);

    /**
     * @brief 开始一帧渲染，返回当前帧上下文。
     * @param 无。
     * @return 当前帧上下文（命令列表/RTV/DSV 等）。
     * @note 阶段：渲染帧开始阶段。
     */
    FD3D12FrameContext BeginFrame();
    /**
     * @brief 结束当前帧渲染并提交到交换链。
     * @param 无。
     * @return 无返回值。
     * @note 阶段：渲染帧提交阶段。
     */
    void EndFrame();
    /**
     * @brief 等待 GPU 完成当前帧所有命令。
     * @param 无。
     * @return 无返回值。
     * @note 阶段：同步/销毁阶段或需要强制同步的场景。
     */
    void WaitForGPU();

    /**
     * @brief 获取 D3D12 设备指针。
     * @param 无。
     * @return ID3D12Device 指针。
     * @note 阶段：渲染资源创建阶段。
     */
    ID3D12Device* GetDevice() const { return Device.Get(); }
    ID3D12CommandQueue* GetCommandQueue() const { return Queue.Get(); }
    /**
     * @brief 获取 DXGI 工厂指针。
     * @param 无。
     * @return IDXGIFactory7 指针。
     * @note 阶段：适配器/交换链管理阶段。
     */
    IDXGIFactory7* GetFactory() const { return Factory.Get(); }
    /**
     * @brief 获取当前使用的适配器名称。
     * @param 无。
     * @return 适配器名称字符串。
     * @note 阶段：初始化后用于展示与日志。
     */
    const std::wstring& GetAdapterName() const { return AdapterName; }

    /**
     * @brief 获取当前渲染宽度。
     * @param 无。
     * @return 渲染宽度（像素）。
     * @note 阶段：渲染帧阶段。
     */
    uint32 GetWidth() const { return Width; }
    /**
     * @brief 获取当前渲染高度。
     * @param 无。
     * @return 渲染高度（像素）。
     * @note 阶段：渲染帧阶段。
     */
    uint32 GetHeight() const { return Height; }
    /**
     * @brief 获取后备缓冲格式。
     * @param 无。
     * @return DXGI_FORMAT 后备缓冲格式。
     * @note 阶段：交换链与RTV创建阶段。
     */
    DXGI_FORMAT GetBackBufferFormat() const { return DXGI_FORMAT_R8G8B8A8_UNORM; }
    /**
     * @brief 获取深度缓冲格式。
     * @param 无。
     * @return DXGI_FORMAT 深度缓冲格式。
     * @note 阶段：深度资源创建阶段。
     */
    DXGI_FORMAT GetDepthFormat() const { return DXGI_FORMAT_D32_FLOAT_S8X24_UINT; }

private:
    struct FPendingBackBufferCapture
    {
        ComPtr<ID3D12Resource> Readback;
        std::wstring Path;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
        uint32 Width = 0;
        uint32 Height = 0;
        bool Valid = false;
    };

    /**
     * @brief 创建交换链的 RTV 资源与视图。
     * @param 无。
     * @return 无返回值。
     * @note 阶段：交换链资源初始化/重建阶段。
     */
    void CreateSwapchainResources();
    /**
     * @brief 创建深度缓冲资源与 DSV。
     * @param 无。
     * @return 无返回值。
     * @note 阶段：深度资源初始化/重建阶段。
     */
    void CreateDepthResources();
    FPendingBackBufferCapture TryQueueBackBufferCapture();
    void SaveBackBufferCapture(const FPendingBackBufferCapture& capture);
    /**
     * @brief 翻转到下一帧并进行 GPU 同步。
     * @param 无。
     * @return 无返回值。
     * @note 阶段：帧提交后的同步阶段。
     */
    void MoveToNextFrame();

private:
    bool TryWaitForGPU(uint32 timeoutMs);

    HWND Hwnd = nullptr;
    uint32 Width = 0;
    uint32 Height = 0;

    ComPtr<IDXGIFactory7> Factory;
    ComPtr<ID3D12Device> Device;
    ComPtr<ID3D12CommandQueue> Queue;
    ComPtr<IDXGISwapChain3> Swapchain;
    std::wstring AdapterName;

    ComPtr<ID3D12DescriptorHeap> RTVHeap;
    uint32 RTVDescriptorSize = 0;
    ComPtr<ID3D12Resource> RenderTargets[kFrameCount];

    ComPtr<ID3D12DescriptorHeap> DSVHeap;
    ComPtr<ID3D12Resource> DepthBuffer;

    ComPtr<ID3D12CommandAllocator> CmdAlloc[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> CmdList;

    ComPtr<ID3D12Fence> Fence;
    uint64 FenceValue[kFrameCount] = {};
    HANDLE FenceEvent = nullptr;
    uint32 FrameIndex = 0;
    uint64 PresentedFrameCounter = 0;
    bool BackBufferCaptureDone = false;
    std::wstring BackBufferCaptureDonePath;

    FD3D12FrameContext CurrentFrame{};
};
