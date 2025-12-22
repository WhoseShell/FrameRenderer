#pragma once

#include <vector>

#include <DirectXMath.h>

#include "RHI/D3D12RHI.h"
#include "Scene/SceneTypes.h"

class FCamera;
class FRenderGraphBuilder;
class FSimpleSceneRenderer;
struct FSkyAtmosphereSettings;

struct FSceneViewFamily
{
    FD3D12RHI* RHI = nullptr;
    bool bEnableTonemap = true;
    bool bEnableLumen = false;
    bool bEnableLumenSWRT = false;
    bool bEnableLumenHWRT = false;
    float SunIntensity = 1.0f;
    const FSkyAtmosphereSettings* Sky = nullptr;
    int LeftInsetPx = 0;
};

struct FSceneView
{
    const FCamera* Camera = nullptr;
    DirectX::XMFLOAT3 LightDirWs{};
    float TimeSeconds = 0.0f;
};

struct FSceneRendererInputs
{
    const std::vector<FSceneObject>* Objects = nullptr;
    int SelectedIndex = -1;
    bool bScaleGizmo = false;
    const DirectX::XMFLOAT3* PreviewPos = nullptr;
    FSceneObject::EType PreviewType = FSceneObject::EType::Sphere;
};

class FSceneRenderer
{
public:
    FSceneRenderer(
        FSimpleSceneRenderer& renderer,
        const FSceneViewFamily& viewFamily,
        const FSceneView& view,
        const FSceneRendererInputs& inputs);
    virtual ~FSceneRenderer() = default;

    void Render();

protected:
    virtual void RenderPath(FRenderGraphBuilder& graph, D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv) = 0;
    virtual bool IsDeferredPath() const = 0;

    struct FViewInfo
    {
        DirectX::XMFLOAT4X4 ViewProj{};
        DirectX::XMFLOAT4X4 InvViewProj{};
        D3D12_VIEWPORT Viewport{};
        D3D12_RECT Scissor{};
    };

    FSimpleSceneRenderer& Renderer;
    const FSceneViewFamily& ViewFamily;
    const FSceneView& View;
    const FSceneRendererInputs& Inputs;

    FViewInfo ViewInfo{};
    FD3D12FrameContext Frame{};
    bool bUseLumen = false;
    bool bUseLumenSWRT = false;
    bool bUseHWRTGI = false;
    uint32 RtInstanceCount = 0;
    uint32 SwrtObjectCount = 0;
};

class FForwardShadingSceneRenderer : public FSceneRenderer
{
public:
    using FSceneRenderer::FSceneRenderer;

private:
    void RenderPath(FRenderGraphBuilder& graph, D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv) override;
    bool IsDeferredPath() const override { return false; }
};

class FDeferredShadingSceneRenderer : public FSceneRenderer
{
public:
    using FSceneRenderer::FSceneRenderer;

private:
    void RenderPath(FRenderGraphBuilder& graph, D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv) override;
    bool IsDeferredPath() const override { return true; }
};
