#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include <DirectXMath.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>

#include "Core/Types.h"
#include "RHI/D3D12RHI.h"

namespace rdcimport
{
struct FCaptureRockInstance
{
    DirectX::XMFLOAT3 Position{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 Scale{ 1.0f, 1.0f, 1.0f };
};

struct FCaptureRockFrameInputs
{
    D3D12_CPU_DESCRIPTOR_HANDLE TargetRTV{};
    D3D12_CPU_DESCRIPTOR_HANDLE DepthDSV{};
    D3D12_VIEWPORT Viewport{};
    D3D12_RECT Scissor{};
    DirectX::XMFLOAT4X4 ViewProj{};
    DirectX::XMFLOAT3 CameraPositionWs{};
    std::vector<FCaptureRockInstance> Instances;
    uint32 FrameIndex = 0;
};

class FCaptureRockRenderer
{
public:
    bool Initialize(
        FD3D12RHI& rhi,
        DXGI_FORMAT colorFormat,
        DXGI_FORMAT depthFormat,
        const std::filesystem::path& manifestPath,
        const std::filesystem::path& shaderPath,
        std::string* error = nullptr);

    void Reset();
    bool IsLoaded() const { return bLoaded_; }
    void Render(ID3D12GraphicsCommandList* cmd, const FCaptureRockFrameInputs& inputs);
    std::wstring FormatSummary() const;

private:
    static constexpr uint32 kTextureSlots = 8;
    static constexpr uint32 kMaxRockInstances = 64;

    struct FTextureSlot
    {
        uint32 Slot = 0;
        uint64 ResourceId = 0;
        uint32 OriginalBindPoint = 0;
        uint32 OriginalTableIndex = 0;
        std::string Role;
        std::filesystem::path RelativePath;
    };

    struct FTextureResource
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
        Microsoft::WRL::ComPtr<ID3D12Resource> Upload;
        DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
        D3D12_RESOURCE_DIMENSION Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        uint32 Width = 0;
        uint32 Height = 0;
        uint32 Depth = 1;
        uint32 MipCount = 1;
        uint32 ArraySize = 1;
    };

    struct FVertex
    {
        DirectX::XMFLOAT3 Position;
        DirectX::XMFLOAT3 Normal;
        DirectX::XMFLOAT2 UV;
        DirectX::XMFLOAT4 Color;
    };

    struct FConstants
    {
        DirectX::XMFLOAT4X4 ViewProj;
        DirectX::XMFLOAT4X4 RockWorld;
        DirectX::XMFLOAT4X4 RockWorldInvTranspose;
        DirectX::XMFLOAT4 CameraPosition;
        DirectX::XMFLOAT4 SunDirectionAndAmbient;
        DirectX::XMFLOAT4 SunColorAndIntensity;
        DirectX::XMFLOAT4 MaterialParams;
    };

    void LoadManifest(const std::filesystem::path& manifestPath);
    void LoadMesh(ID3D12Device* device);
    void LoadTextures(FD3D12RHI& rhi);
    void CreateRootSignature(ID3D12Device* device);
    void CreatePipelineState(ID3D12Device* device, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat);
    void CreateConstantBuffers(ID3D12Device* device);
    FConstants BuildConstants(const FCaptureRockFrameInputs& inputs, const FCaptureRockInstance& instance) const;

    std::filesystem::path ResolveAssetPath(const std::filesystem::path& relativePath) const;
    void CreateTextureSRV(ID3D12Device* device, const FTextureResource& texture, uint32 slot);

    Microsoft::WRL::ComPtr<ID3D12Device> Device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineState_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> SrvHeap_;
    uint32 SrvDescriptorSize_ = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> VertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer_;
    D3D12_VERTEX_BUFFER_VIEW VertexBufferView_{};
    D3D12_INDEX_BUFFER_VIEW IndexBufferView_{};

    Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffers_[FD3D12RHI::kFrameCount];
    uint8* ConstantBufferMapped_[FD3D12RHI::kFrameCount] = {};
    uint32 ConstantBufferSize_ = 0;

    std::array<FTextureSlot, kTextureSlots> TextureSlots_{};
    std::array<FTextureResource, kTextureSlots> Textures_{};
    uint32 LoadedTextureCount_ = 0;

    std::filesystem::path ManifestPath_;
    std::filesystem::path BaseDirectory_;
    std::filesystem::path ShaderPath_;
    std::filesystem::path PositionsPath_;
    std::filesystem::path NormalsPath_;
    std::filesystem::path Uv0Path_;
    std::filesystem::path Color0Path_;
    std::filesystem::path IndicesPath_;

    uint32 EventId_ = 0;
    uint32 DrawId_ = 0;
    uint32 VertexCount_ = 0;
    uint32 IndexCount_ = 0;
    DirectX::XMFLOAT3 BoundsMinCapture_{};
    DirectX::XMFLOAT3 BoundsMaxCapture_{};
    DirectX::XMFLOAT3 BoundsMinEngine_{};
    DirectX::XMFLOAT3 BoundsMaxEngine_{};
    float BoundsRadius_ = 1.0f;

    DirectX::XMFLOAT3 SunDirectionEngine_{ 0.0f, -1.0f, 0.0f };
    DirectX::XMFLOAT3 SunColor_{ 1.0f, 0.96f, 0.88f };
    float SunIntensity_ = 5.0f;
    float AmbientIntensity_ = 0.16f;
    float Roughness_ = 0.82f;
    float Metallic_ = 0.0f;
    float NormalStrength_ = 0.35f;
    float BaseColorBoost_ = 1.08f;
    DirectX::XMFLOAT3 BasePositionWorld_{ 0.0f, 0.0f, 0.0f };
    float UniformScale_ = 0.35f;
    bool bSitOnGround_ = true;
    bool bLoaded_ = false;
};
}
