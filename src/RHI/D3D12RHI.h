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

    void Init(HWND hwnd, uint32 width, uint32 height);
    void Shutdown();
    void Resize(uint32 width, uint32 height);
    void ExecuteImmediate(void(*record)(ID3D12GraphicsCommandList* cmd, void* user), void* user);

    FD3D12FrameContext BeginFrame();
    void EndFrame();
    void WaitForGPU();

    ID3D12Device* GetDevice() const { return Device.Get(); }
    IDXGIFactory7* GetFactory() const { return Factory.Get(); }
    const std::wstring& GetAdapterName() const { return AdapterName; }

    uint32 GetWidth() const { return Width; }
    uint32 GetHeight() const { return Height; }
    DXGI_FORMAT GetBackBufferFormat() const { return DXGI_FORMAT_R8G8B8A8_UNORM; }
    DXGI_FORMAT GetDepthFormat() const { return DXGI_FORMAT_D32_FLOAT; }

private:
    void CreateSwapchainResources();
    void CreateDepthResources();
    void MoveToNextFrame();

private:
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

    FD3D12FrameContext CurrentFrame{};
};
