#pragma once

#include <string>
#include <wrl.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <DirectXMath.h>

#include "Core/Types.h"
#include "Math/Camera.h"
#include "RHI/D3D12RHI.h"
#include "Renderer/MeshGeneration.h"
#include "Scene/SceneTypes.h"

#include <vector>

using Microsoft::WRL::ComPtr;

class FRenderGraphBuilder;
class FSceneRenderer;
class FForwardShadingSceneRenderer;
class FDeferredShadingSceneRenderer;

struct FSkyAtmosphereSettings
{
    bool Enable = true;
    float AtmosphereHeight = 12.0f; // world units (Y up)
    float RayleighScale = 1.0f;
    float MieScale = 1.0f;
    float MieG = 0.8f;
    float GroundAlbedo = 0.2f;
};

class FSimpleSceneRenderer
{
public:
    enum class ERenderPath : uint8
    {
        Forward = 0,
        Deferred = 1,
    };

    void Init(FD3D12RHI& rhi);
    void Shutdown();
    bool IsRaytracingSupported() const { return bRaytracingSupported; }
    bool IsHWRTGIReady() const { return bHWRTGIReady; }
    const std::wstring& GetHWRTGIInitError() const { return HWRTGIInitError; }
    D3D12_RAYTRACING_TIER GetRaytracingTier() const { return RaytracingTier; }

    // Returns renderer texture slot (0 = built-in white).
    int CreateTextureRGBA8(FD3D12RHI& rhi, uint32 width, uint32 height, const uint8* rgba);
    int AllocateMaterialSRVBlock();
    void UpdateMaterialSRVBlock(int base, int albedoSlot, int normalSlot, int roughnessSlot, int metallicSlot, int aoSlot);

    void Render(
        FD3D12RHI& rhi,
        const FCamera& camera,
        float timeSeconds,
        ERenderPath renderPath,
        bool bEnableLumen,
        bool bEnableLumenSWRT,
        bool bEnableLumenHWRT,
        const std::vector<FSceneObject>& objects,
        int selectedIndex,
        bool bScaleGizmo,
        const DirectX::XMFLOAT3& lightDirWs,
        bool bEnableTonemap,
        float sunIntensity,
        const FSkyAtmosphereSettings& sky,
        int leftInsetPx,
        const DirectX::XMFLOAT3* previewPos,
        FSceneObject::EType previewType);

private:
    friend class FSceneRenderer;
    friend class FForwardShadingSceneRenderer;
    friend class FDeferredShadingSceneRenderer;

    struct FSceneCB
    {
        DirectX::XMFLOAT4X4 MVP;
        DirectX::XMFLOAT4X4 World;
        DirectX::XMFLOAT4X4 WorldInvTranspose;
        DirectX::XMFLOAT3 CameraPosWs;
        float _pad0 = 0.0f;
        DirectX::XMFLOAT3 LightDirWs;
        float _pad1 = 0.0f;
        DirectX::XMFLOAT3 LightColor;
        float LightIntensity = 1.0f;
        DirectX::XMFLOAT3 Albedo;
        float Metallic = 0.0f;
        float Roughness = 0.5f;
        float UseAlbedoTex = 0.0f;
        float UseNormalTex = 0.0f;
        float UseRoughnessTex = 0.0f;
        float UseMetallicTex = 0.0f;
        float UseAOTex = 0.0f;
        DirectX::XMFLOAT3 _pad2{};
    };

    static uint32 Align256(uint32 size);
    static ComPtr<ID3DBlob> CompileShaderFromFile(const std::wstring& file, const std::string& entry, const std::string& target);
    void EnsureHDRTargets(FD3D12RHI& rhi);
    void EnsureDeferredTargets(FD3D12RHI& rhi);
    void EnsureShadowMap(FD3D12RHI& rhi);
    void EnsureSkyIBLTargets(FD3D12RHI& rhi);
    void InitRaytracing(FD3D12RHI& rhi);
    uint32 PrepareRaytracingScene(FD3D12RHI& rhi, uint32 frameIndex, const std::vector<FSceneObject>& objects, const DirectX::XMFLOAT3* previewPos, FSceneObject::EType previewType);
    void RecordBuildTLAS(ID3D12GraphicsCommandList* cmd, uint32 frameIndex, uint32 instanceCount);
    void InitSkyPass(FD3D12RHI& rhi, const D3D12_BLEND_DESC& blend, const D3D12_RASTERIZER_DESC& rast);
    void InitSkyIBLPass(FD3D12RHI& rhi);
    void InitTonemapPass(FD3D12RHI& rhi, const D3D12_BLEND_DESC& blend, const D3D12_RASTERIZER_DESC& rast);
    void InitDeferredPass(FD3D12RHI& rhi, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& basePso, const D3D12_BLEND_DESC& blend, const D3D12_RASTERIZER_DESC& rast);
    void InitLumenPass(FD3D12RHI& rhi, const D3D12_BLEND_DESC& blend, const D3D12_RASTERIZER_DESC& rast);
    void InitLumenSwrtPass(FD3D12RHI& rhi, const D3D12_BLEND_DESC& blend, const D3D12_RASTERIZER_DESC& rast);
    void InitHWRTGIPass(FD3D12RHI& rhi, const D3D12_BLEND_DESC& blend, const D3D12_RASTERIZER_DESC& rast);
    void InitShadowPass(FD3D12RHI& rhi, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& basePso, const D3D12_RASTERIZER_DESC& rast);

    void UpdateSkyCB(const DirectX::XMMATRIX& invViewProj, const DirectX::XMFLOAT3& cameraPosWs, const DirectX::XMFLOAT3& lightDirWs,
                     float sunIntensity, const FSkyAtmosphereSettings& sky, uint32 frameIndex);
    void UpdateTonemapCB(bool enableTonemap, uint32 frameIndex);
    void UpdateDeferredCB(const DirectX::XMFLOAT3& cameraPosWs, const DirectX::XMFLOAT3& lightDirWs, float sunIntensity, uint32 frameIndex);
    void UpdateShadowCB(const DirectX::XMFLOAT3& cameraPosWs, const DirectX::XMFLOAT3& lightDirWs, uint32 frameIndex);
    void UpdateLumenCB(FD3D12RHI& rhi, bool bUseLumen, const DirectX::XMFLOAT4X4& curViewProj, const DirectX::XMFLOAT3& cameraPosWs,
                       const DirectX::XMFLOAT3& lightDirWs, float sunIntensity, float timeSeconds, uint32 frameIndex);
    uint32 UpdateSWRTGICBAndObjects(FD3D12RHI& rhi, bool bUseSWRT, const DirectX::XMFLOAT4X4& curViewProj, const DirectX::XMFLOAT3& cameraPosWs,
                                   const DirectX::XMFLOAT3& lightDirWs, float sunIntensity, float timeSeconds, uint32 frameIndex,
                                   const std::vector<FSceneObject>& objects, const DirectX::XMFLOAT3* previewPos, FSceneObject::EType previewType);
    uint32 UpdateHWRTGICBAndScene(FD3D12RHI& rhi, bool bUseHWRTGI, const DirectX::XMFLOAT4X4& curViewProj, const DirectX::XMFLOAT3& cameraPosWs,
                                 const DirectX::XMFLOAT3& lightDirWs, float sunIntensity, float timeSeconds, uint32 frameIndex,
                                 const std::vector<FSceneObject>& objects, const DirectX::XMFLOAT3* previewPos, FSceneObject::EType previewType);

    void AddSkyPass(FRenderGraphBuilder& graph, const FD3D12FrameContext& frame, const D3D12_VIEWPORT& vp, const D3D12_RECT& sc,
                    D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv, bool enable);
    void AddSkyIBLPasses(FRenderGraphBuilder& graph, const FD3D12FrameContext& frame, bool enable);
    void AddShadowPass(FRenderGraphBuilder& graph, const FD3D12FrameContext& frame, const std::vector<FSceneObject>& objects,
                       const DirectX::XMFLOAT3* previewPos, FSceneObject::EType previewType);
    void AddGBufferPasses(FRenderGraphBuilder& graph, const FD3D12FrameContext& frame, const D3D12_VIEWPORT& vp, const D3D12_RECT& sc,
                          const std::vector<FSceneObject>& objects, const DirectX::XMFLOAT3* previewPos, FSceneObject::EType previewType);
    void AddDeferredLightingPass(FRenderGraphBuilder& graph, const FD3D12FrameContext& frame, const D3D12_VIEWPORT& vp, const D3D12_RECT& sc,
                                 D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv);
    void AddBuildTLASPass(FRenderGraphBuilder& graph, uint32 frameIndex, uint32 instanceCount);
    void AddHWRTGIPasses(FRenderGraphBuilder& graph, const FD3D12FrameContext& frame, const D3D12_VIEWPORT& vp, const D3D12_RECT& sc,
                         D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv, uint32 instanceCount);
    void AddLumenSwrtPasses(FRenderGraphBuilder& graph, const FD3D12FrameContext& frame, const D3D12_VIEWPORT& vp, const D3D12_RECT& sc,
                            D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv, uint32 objectCount);
    void AddLumenPass(FRenderGraphBuilder& graph, const FD3D12FrameContext& frame, const D3D12_VIEWPORT& vp, const D3D12_RECT& sc,
                      D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv);
    void AddHDRToSRVPass(FRenderGraphBuilder& graph);
    void AddTonemapPass(FRenderGraphBuilder& graph, const FD3D12FrameContext& frame, const D3D12_VIEWPORT& vp, const D3D12_RECT& sc);

private:
    ComPtr<ID3D12RootSignature> RootSig;
    ComPtr<ID3D12PipelineState> PSO;
    ComPtr<ID3D12PipelineState> PSO_Lines;
    ComPtr<ID3D12DescriptorHeap> SRVHeap;
    uint32 SRVDescriptorSize = 0;
    static constexpr int kShadowMapSRVSlot = 2000;
    static constexpr int kSkyPrefilterSRVSlot = kShadowMapSRVSlot + 1;
    static constexpr int kSkySHSRVSlot = kShadowMapSRVSlot + 2;

    // SkyAtmosphere (fullscreen) pass
    ComPtr<ID3D12RootSignature> SkyRootSig;
    ComPtr<ID3D12PipelineState> SkyPSO;
    struct FSkyCB
    {
        DirectX::XMFLOAT4X4 InvViewProj;
        DirectX::XMFLOAT3 CameraPosWs;
        float AtmosphereHeight = 12.0f;

        DirectX::XMFLOAT3 SunDirWs;
        float SunIntensity = 1.0f;

        DirectX::XMFLOAT3 SunColor{ 1.0f, 0.98f, 0.92f };
        float RayleighScale = 1.0f;

        float MieScale = 1.0f;
        float MieG = 0.8f;
        float GroundAlbedo = 0.2f;
        float _pad0 = 0.0f;
    };
    ComPtr<ID3D12Resource> ConstantBufferSky[FD3D12RHI::kFrameCount];
    uint8* CBMappedSky[FD3D12RHI::kFrameCount] = {};
    uint32 SkyCBSize = 0;

    // Sky IBL (cubemap + SH)
    static constexpr uint32 kSkyIBLSize = 64;
    static constexpr uint32 kSkyIBLMipCount = 7;
    static constexpr uint32 kSkyIBLConstantCount = kSkyIBLMipCount + 2;
    DXGI_FORMAT SkyIBLFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    ComPtr<ID3D12RootSignature> SkyIBLRootSig;
    ComPtr<ID3D12PipelineState> SkyIBLGenPSO;
    ComPtr<ID3D12PipelineState> SkyIBLSHPSO;
    ComPtr<ID3D12PipelineState> SkyIBLPrefilterPSO;

    ComPtr<ID3D12Resource> SkyCube;
    ComPtr<ID3D12Resource> SkyPrefilter;
    ComPtr<ID3D12Resource> SkySH;
    D3D12_RESOURCE_STATES SkyCubeState = D3D12_RESOURCE_STATE_GENERIC_READ;
    D3D12_RESOURCE_STATES SkyPrefilterState = D3D12_RESOURCE_STATE_GENERIC_READ;
    D3D12_RESOURCE_STATES SkySHState = D3D12_RESOURCE_STATE_GENERIC_READ;

    ComPtr<ID3D12DescriptorHeap> SkyIBLHeap;
    uint32 SkyIBLDescriptorSize = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE SkyIBLCubeSrvGPU{};
    D3D12_GPU_DESCRIPTOR_HANDLE SkyIBLCubeUavGPU{};
    D3D12_GPU_DESCRIPTOR_HANDLE SkyIBLSHUavGPU{};
    std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> SkyIBLPrefilterUavGPU;

    struct FSkyIBLCB
    {
        float Roughness = 0.0f;
        float MipLevel = 0.0f;
        float MaxMip = 0.0f;
        float _pad0 = 0.0f;
    };
    ComPtr<ID3D12Resource> ConstantBufferSkyIBL[FD3D12RHI::kFrameCount];
    uint8* CBMappedSkyIBL[FD3D12RHI::kFrameCount] = {};
    uint32 SkyIBLCBSize = 0;

    struct FTextureGPU
    {
        ComPtr<ID3D12Resource> Resource;
        uint32 Width = 0;
        uint32 Height = 0;
    };
    std::vector<FTextureGPU> Textures;

    struct FMeshGPU
    {
        ComPtr<ID3D12Resource> VertexBuffer;
        D3D12_VERTEX_BUFFER_VIEW VBView{};
        ComPtr<ID3D12Resource> IndexBuffer;
        D3D12_INDEX_BUFFER_VIEW IBView{};
        uint32 IndexCount = 0;
    };

    const FMeshGPU& GetMesh(FSceneObject::EType type) const;

    FMeshGPU MeshSphere;
    FMeshGPU MeshBox;
    FMeshGPU MeshCone;

    static constexpr uint32 MaxObjects = 256;

    ComPtr<ID3D12Resource> ConstantBufferObjects[FD3D12RHI::kFrameCount];
    uint8* CBMappedObjects[FD3D12RHI::kFrameCount] = {};

    ComPtr<ID3D12Resource> ConstantBufferGizmo[FD3D12RHI::kFrameCount];
    uint8* CBMappedGizmo[FD3D12RHI::kFrameCount] = {};
    uint32 CBSize = 0;

    // Shadow map (directional)
    uint32 ShadowMapSize = 2048;
    DXGI_FORMAT ShadowDepthFormat = DXGI_FORMAT_D32_FLOAT;
    ComPtr<ID3D12Resource> ShadowMap;
    ComPtr<ID3D12DescriptorHeap> ShadowDSVHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE ShadowDSV{};
    D3D12_RESOURCE_STATES ShadowState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    ComPtr<ID3D12RootSignature> ShadowRootSig;
    ComPtr<ID3D12PipelineState> ShadowPSO;
    struct FShadowCB
    {
        DirectX::XMFLOAT4X4 LightViewProj;
        DirectX::XMFLOAT2 ShadowInvSize;
        float ShadowBias = 0.0015f;
        float ShadowStrength = 1.0f;
    };
    ComPtr<ID3D12Resource> ConstantBufferShadow[FD3D12RHI::kFrameCount];
    uint8* CBMappedShadow[FD3D12RHI::kFrameCount] = {};
    uint32 ShadowCBSize = 0;

    // HDR scene target + tonemap pass
    DXGI_FORMAT HDRFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uint32 HDRWidth = 0;
    uint32 HDRHeight = 0;
    ComPtr<ID3D12Resource> HDRColor;
    ComPtr<ID3D12DescriptorHeap> HDRRTVHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE HDRRTV{};
    D3D12_RESOURCE_STATES HDRState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    ComPtr<ID3D12DescriptorHeap> TonemapSRVHeap;
    uint32 TonemapSRVDescriptorSize = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE TonemapHDRSRVGPU{};

    ComPtr<ID3D12RootSignature> TonemapRootSig;
    ComPtr<ID3D12PipelineState> TonemapPSO;

    struct FTonemapCB
    {
        float EnableTonemap = 1.0f;
        float Exposure = 1.0f;
        float Gamma = 2.2f;
        float _pad0 = 0.0f;
    };
    ComPtr<ID3D12Resource> ConstantBufferTonemap[FD3D12RHI::kFrameCount];
    uint8* CBMappedTonemap[FD3D12RHI::kFrameCount] = {};
    uint32 TonemapCBSize = 0;

    // Deferred pipeline: GBuffer + fullscreen lighting
    DXGI_FORMAT GBuffer0Format = DXGI_FORMAT_R16G16B16A16_FLOAT;      // albedo.rgb + metallic.a (higher precision to match forward)
    DXGI_FORMAT GBuffer1Format = DXGI_FORMAT_R16G16B16A16_FLOAT;      // normal.xyz + roughness.a (roughness < 0 => empty)
    DXGI_FORMAT GBuffer2Format = DXGI_FORMAT_R16G16B16A16_FLOAT;      // posW.xyz + ao.a
    uint32 GBufferWidth = 0;
    uint32 GBufferHeight = 0;
    ComPtr<ID3D12Resource> GBuffer0;
    ComPtr<ID3D12Resource> GBuffer1;
    ComPtr<ID3D12Resource> GBuffer2;
    ComPtr<ID3D12DescriptorHeap> GBufferRTVHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE GBufferRTVs[3] = {};
    D3D12_RESOURCE_STATES GBufferStates[3] = {};

    ComPtr<ID3D12DescriptorHeap> GBufferSRVHeap;
    uint32 GBufferSRVDescriptorSize = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE GBufferSRVBaseGPU{};

    // Deferred SRV/UAV heap layout (shared by deferred lighting + Lumen passes).
    static constexpr uint32 kGBufferHeapNumDescriptors = 64;
    static constexpr uint32 kDesc_GBuffer0 = 0;
    static constexpr uint32 kDesc_GBuffer1 = 1;
    static constexpr uint32 kDesc_GBuffer2 = 2;
    static constexpr uint32 kDesc_LumenHistory0 = 3;
    static constexpr uint32 kDesc_LumenHistory1 = 4;
    static constexpr uint32 kDesc_TLAS0 = 5;
    static constexpr uint32 kDesc_TLAS1 = 6;
    static constexpr uint32 kDesc_RTObjects0 = 7;
    static constexpr uint32 kDesc_RTObjects1 = 8;
    static constexpr uint32 kDesc_SphereVB = 9;
    static constexpr uint32 kDesc_SphereIB = 10;
    static constexpr uint32 kDesc_BoxVB = 11;
    static constexpr uint32 kDesc_BoxIB = 12;
    static constexpr uint32 kDesc_ConeVB = 13;
    static constexpr uint32 kDesc_ConeIB = 14;
    static constexpr uint32 kDesc_HWRTGIHistory0 = 15;
    static constexpr uint32 kDesc_HWRTGIHistory1 = 16;
    static constexpr uint32 kDesc_HWRTGIMeta0 = 17;
    static constexpr uint32 kDesc_HWRTGIMeta1 = 18;
    static constexpr uint32 kDesc_HWRTGIFiltered = 19;
    static constexpr uint32 kDesc_LumenSWRTSurface0 = 20;
    static constexpr uint32 kDesc_LumenSWRTSurface1 = 21;
    static constexpr uint32 kDesc_ShadowMap = 25;
    static constexpr uint32 kDesc_SkyPrefilter = 26;
    static constexpr uint32 kDesc_SkySH = 27;

    static constexpr uint32 kDescUAVBase = 32;
    static constexpr uint32 kUav_HWRTGIHistory0 = 32;
    static constexpr uint32 kUav_HWRTGIHistory1 = 33;
    static constexpr uint32 kUav_HWRTGIMeta0 = 34;
    static constexpr uint32 kUav_HWRTGIMeta1 = 35;
    static constexpr uint32 kUav_HWRTGIFiltered = 36;
    static constexpr uint32 kUav_LumenSWRTSurface0 = 37;
    static constexpr uint32 kUav_LumenSWRTSurface1 = 38;

    ComPtr<ID3D12PipelineState> PSO_GBuffer;
    ComPtr<ID3D12RootSignature> DeferredLightRootSig;
    ComPtr<ID3D12PipelineState> DeferredLightPSO;
    struct FDeferredLightCB
    {
        DirectX::XMFLOAT3 CameraPosWs;
        float _pad0 = 0.0f;
        DirectX::XMFLOAT3 LightDirWs;
        float _pad1 = 0.0f;
        DirectX::XMFLOAT3 LightColor;
        float LightIntensity = 1.0f;
    };
    ComPtr<ID3D12Resource> ConstantBufferDeferred[FD3D12RHI::kFrameCount];
    uint8* CBMappedDeferred[FD3D12RHI::kFrameCount] = {};
    uint32 DeferredCBSize = 0;

    // Lumen (Lite): screen-trace GI + reflections with temporal accumulation (deferred-only)
    DXGI_FORMAT LumenHistoryFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uint32 LumenWidth = 0;
    uint32 LumenHeight = 0;
    ComPtr<ID3D12Resource> LumenHistory[2];
    ComPtr<ID3D12DescriptorHeap> LumenRTVHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE LumenRTVs[2] = {};
    D3D12_RESOURCE_STATES LumenHistoryStates[2] = {};
    int LumenHistoryWriteIndex = 0;
    bool bLumenHistoryValid = false;
    DirectX::XMFLOAT4X4 PrevViewProj{};

    ComPtr<ID3D12RootSignature> LumenRootSig;
    ComPtr<ID3D12PipelineState> LumenPSO;
    struct FLumenCB
    {
        DirectX::XMFLOAT4X4 ViewProj;
        DirectX::XMFLOAT4X4 PrevViewProj;
        DirectX::XMFLOAT3 CameraPosWs;
        float TemporalWeight = 0.9f;
        DirectX::XMFLOAT3 LightDirWs;
        float MaxTraceDistance = 8.0f;
        DirectX::XMFLOAT3 LightColor;
        float LightIntensity = 1.0f;
        DirectX::XMFLOAT2 InvResolution;
        float StepSize = 0.25f;
        float Intensity = 1.0f;
        float FrameIndex = 0.0f;
        float PrevHistoryIndex = 0.0f;
        float HistoryValid = 0.0f;
        float _pad0 = 0.0f;
    };
    ComPtr<ID3D12Resource> ConstantBufferLumen[FD3D12RHI::kFrameCount];
    uint8* CBMappedLumen[FD3D12RHI::kFrameCount] = {};
    uint32 LumenCBSize = 0;

    // Lumen SWRT GI: software ray tracing + surface cache + temporal history + bilateral denoise.
    DXGI_FORMAT LumenSwrtSurfaceFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uint32 LumenSwrtWidth = 0;
    uint32 LumenSwrtHeight = 0;
    ComPtr<ID3D12Resource> LumenSwrtSurface[2];
    D3D12_RESOURCE_STATES LumenSwrtSurfaceStates[2] = {};

    ComPtr<ID3D12RootSignature> LumenSwrtRootSig;
    ComPtr<ID3D12PipelineState> LumenSwrtSurfacePSO;
    ComPtr<ID3D12PipelineState> LumenSwrtGIPSO;
    ComPtr<ID3D12PipelineState> LumenSwrtFilterPSO;
    ComPtr<ID3D12RootSignature> LumenSwrtAddRootSig;
    ComPtr<ID3D12PipelineState> LumenSwrtAddPSO;
    bool bSWRTGIHistoryValid = false;

    // Lumen HWRT GI: DXR ray query + screen-probe cache + temporal reprojection + bilateral denoise.
    uint32 HWRTGIWidth = 0;
    uint32 HWRTGIHeight = 0;
    ComPtr<ID3D12Resource> HWRTGIHistory[2];
    ComPtr<ID3D12Resource> HWRTGIMeta[2];
    ComPtr<ID3D12Resource> HWRTGIFiltered;
    D3D12_RESOURCE_STATES HWRTGIHistoryStates[2] = {};
    D3D12_RESOURCE_STATES HWRTGIMetaStates[2] = {};
    D3D12_RESOURCE_STATES HWRTGIFilteredState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bool bHWRTGIHistoryValid = false;

    ComPtr<ID3D12RootSignature> HWRTGIRootSig;
    ComPtr<ID3D12PipelineState> HWRTGIPSO;
    ComPtr<ID3D12PipelineState> HWRTGIFilterPSO;
    ComPtr<ID3D12RootSignature> HWRTGIAddRootSig;
    ComPtr<ID3D12PipelineState> HWRTGIAddPSO;

    struct FHWRTGICB
    {
        DirectX::XMFLOAT4X4 ViewProj;
        DirectX::XMFLOAT4X4 PrevViewProj;
        DirectX::XMFLOAT3 CameraPosWs;
        float TemporalWeight = 0.9f;
        DirectX::XMFLOAT3 LightDirWs;
        float MaxTraceDistance = 30.0f;
        DirectX::XMFLOAT3 LightColor;
        float LightIntensity = 1.0f;
        DirectX::XMFLOAT2 InvFullResolution;
        DirectX::XMFLOAT2 InvGIResolution;
        float FrameIndex = 0.0f;
        float FrameParity = 0.0f;
        float PrevHistoryIndex = 0.0f;
        float HistoryValid = 0.0f;
        float GIIntensity = 1.0f;
        float DepthReject = 0.25f;   // relative depth rejection (|dz| / z)
        float NormalReject = 0.35f;  // 1 - dot(N,Nprev)
        float RaysPerPixel = 1.0f;
        float ObjectCount = 0.0f;
        float _pad0 = 0.0f;
        float _pad1 = 0.0f;
        float _pad2 = 0.0f;
    };
    ComPtr<ID3D12Resource> ConstantBufferHWRTGI[FD3D12RHI::kFrameCount];
    uint8* CBMappedHWRTGI[FD3D12RHI::kFrameCount] = {};
    uint32 HWRTGICBSize = 0;
    bool bHWRTGIReady = false;
    std::wstring HWRTGIInitError;

    ComPtr<ID3D12Resource> GizmoVB;
    D3D12_VERTEX_BUFFER_VIEW GizmoVBView{};
    FVertex* GizmoMapped = nullptr;

    // Hardware ray tracing (DXR): BLAS per primitive + per-frame TLAS.
    bool bRaytracingSupported = false;
    D3D12_RAYTRACING_TIER RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;

    struct FRTMeshAS
    {
        ComPtr<ID3D12Resource> BLAS;
    };

    FRTMeshAS RTMeshSphere;
    FRTMeshAS RTMeshBox;
    FRTMeshAS RTMeshCone;

    struct FRTFrame
    {
        ComPtr<ID3D12Resource> TLAS;
        ComPtr<ID3D12Resource> Scratch;
        ComPtr<ID3D12Resource> InstanceDescsUpload;
        uint8* InstanceDescsMapped = nullptr;
        uint32 MaxInstances = 0;
    };
    FRTFrame RTFrame[FD3D12RHI::kFrameCount];

    struct FRTObjectData
    {
        DirectX::XMFLOAT3 Albedo{ 0.8f, 0.8f, 0.8f };
        float Metallic = 0.0f;
        float Roughness = 0.5f;
        float AO = 1.0f;
        uint32 MeshType = 0;
        uint32 _pad0 = 0;
        DirectX::XMFLOAT3 Position{ 0.0f, 0.0f, 0.0f };
        float Radius = 0.75f;
        DirectX::XMFLOAT3 Scale{ 1.0f, 1.0f, 1.0f };
        float _pad1 = 0.0f;
    };
    static_assert(sizeof(FRTObjectData) % 16 == 0, "FRTObjectData should be 16-byte aligned");

    ComPtr<ID3D12Resource> RTObjectBuffer[FD3D12RHI::kFrameCount];
    uint8* RTObjectMapped[FD3D12RHI::kFrameCount] = {};
    uint32 RTObjectInstanceCount[FD3D12RHI::kFrameCount] = {};
};
