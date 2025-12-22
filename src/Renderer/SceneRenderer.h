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
    /**
     * @brief 构建场景渲染器并保存渲染参数引用。
     * @param renderer 渲染器实例。
     * @param viewFamily 视图族参数。
     * @param view 视图参数。
     * @param inputs 渲染输入（对象/选择/Gizmo）。
     * @return 无返回值（构造函数）。
     * @note 阶段：渲染帧构建阶段。
     */
    FSceneRenderer(
        FSimpleSceneRenderer& renderer,
        const FSceneViewFamily& viewFamily,
        const FSceneView& view,
        const FSceneRendererInputs& inputs);
    virtual ~FSceneRenderer() = default;

    /**
     * @brief 执行一帧场景渲染（构建渲染图并提交）。
     * @param 无。
     * @return 无返回值。
     * @note 阶段：渲染帧执行阶段。
     */
    void Render();

protected:
    /**
     * @brief 根据渲染路径填充渲染图。
     * @param graph 渲染图构建器。
     * @param hdrRtv HDR 目标 RTV。
     * @return 无返回值。
     * @note 阶段：渲染图构建阶段。
     */
    virtual void RenderPath(FRenderGraphBuilder& graph, D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv) = 0;
    /**
     * @brief 是否使用延迟渲染路径。
     * @param 无。
     * @return true 表示延迟渲染。
     * @note 阶段：渲染路径选择阶段。
     */
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
    /**
     * @brief 前向渲染路径的渲染图构建。
     * @param graph 渲染图构建器。
     * @param hdrRtv HDR 目标 RTV。
     * @return 无返回值。
     * @note 阶段：前向渲染路径构建阶段。
     */
    void RenderPath(FRenderGraphBuilder& graph, D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv) override;
    /**
     * @brief 指示当前为前向渲染路径。
     * @param 无。
     * @return false（非延迟）。
     * @note 阶段：渲染路径选择阶段。
     */
    bool IsDeferredPath() const override { return false; }
};

class FDeferredShadingSceneRenderer : public FSceneRenderer
{
public:
    using FSceneRenderer::FSceneRenderer;

private:
    /**
     * @brief 延迟渲染路径的渲染图构建。
     * @param graph 渲染图构建器。
     * @param hdrRtv HDR 目标 RTV。
     * @return 无返回值。
     * @note 阶段：延迟渲染路径构建阶段。
     */
    void RenderPath(FRenderGraphBuilder& graph, D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv) override;
    /**
     * @brief 指示当前为延迟渲染路径。
     * @param 无。
     * @return true（延迟渲染）。
     * @note 阶段：渲染路径选择阶段。
     */
    bool IsDeferredPath() const override { return true; }
};
