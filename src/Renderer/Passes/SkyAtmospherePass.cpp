#include "Renderer/SimpleSceneRenderer.h"

#include <algorithm>
#include <cstring>

#include "Core/Diagnostics.h"
#include "Renderer/RenderGraph.h"

/**
 * @brief 初始化天空大气渲染通道（RootSig/PSO）。
 * @param rhi 渲染硬件接口。
 * @param blend 混合状态描述。
 * @param rast 光栅化状态描述。
 * @return 无返回值。
 * @note 阶段：天空通道初始化阶段。
 */
void FSimpleSceneRenderer::InitSkyPass(FD3D12RHI& rhi, const D3D12_BLEND_DESC& blend, const D3D12_RASTERIZER_DESC& rast)
{
    ID3D12Device* device = rhi.GetDevice();
    if (!device) return;

    // RootSignature：仅包含天空常量缓冲。
    D3D12_ROOT_PARAMETER paramsSky[1]{};
    paramsSky[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    paramsSky[0].Descriptor.ShaderRegister = 0;
    paramsSky[0].Descriptor.RegisterSpace = 0;
    paramsSky[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsSky{};
    rsSky.NumParameters = 1;
    rsSky.pParameters = paramsSky;
    rsSky.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigSky, errSky;
    HRESULT hrSky = D3D12SerializeRootSignature(&rsSky, D3D_ROOT_SIGNATURE_VERSION_1, &sigSky, &errSky);
    if (errSky)
    {
        std::string err((const char*)errSky->GetBufferPointer(), errSky->GetBufferSize());
        DebugOutput(std::wstring(err.begin(), err.end()));
    }
    ThrowIfFailed(hrSky, "D3D12SerializeRootSignature (sky) failed");
    ThrowIfFailed(device->CreateRootSignature(0, sigSky->GetBufferPointer(), sigSky->GetBufferSize(), IID_PPV_ARGS(&SkyRootSig)),
                  "CreateRootSignature (sky) failed");

    // 编译天空 Shader 并创建 PSO。
    std::wstring skyPath = std::wstring(SHADER_DIR) + L"/sky_atmosphere.hlsl";
    auto vsSky = CompileShaderFromFile(skyPath, "VSMain", "vs_5_0");
    auto psSky = CompileShaderFromFile(skyPath, "PSMain", "ps_5_0");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSky{};
    psoSky.pRootSignature = SkyRootSig.Get();
    psoSky.VS = { vsSky->GetBufferPointer(), vsSky->GetBufferSize() };
    psoSky.PS = { psSky->GetBufferPointer(), psSky->GetBufferSize() };
    psoSky.BlendState = blend;
    psoSky.RasterizerState = rast;
    psoSky.DepthStencilState.DepthEnable = FALSE;
    psoSky.DepthStencilState.StencilEnable = FALSE;
    psoSky.SampleMask = UINT_MAX;
    psoSky.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoSky.NumRenderTargets = 1;
    psoSky.RTVFormats[0] = HDRFormat;
    psoSky.SampleDesc.Count = 1;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoSky, IID_PPV_ARGS(&SkyPSO)), "CreateGraphicsPipelineState (sky) failed");
}

/**
 * @brief 更新天空常量缓冲参数。
 * @param invViewProj 视图投影逆矩阵。
 * @param cameraPosWs 相机世界位置。
 * @param lightDirWs 太阳方向。
 * @param sunIntensity 太阳强度。
 * @param sky 天空参数。
 * @param frameIndex 帧索引。
 * @return 无返回值。
 * @note 阶段：天空参数更新阶段。
 */
void FSimpleSceneRenderer::UpdateSkyCB(
    const DirectX::XMMATRIX& invViewProj,
    const DirectX::XMFLOAT3& cameraPosWs,
    const DirectX::XMFLOAT3& lightDirWs,
    float sunIntensity,
    const FSkyAtmosphereSettings& sky,
    uint32 frameIndex)
{
    if (!CBMappedSky[frameIndex])
        return;

    // 填充天空常量缓冲。
    FSkyCB skycb{};
    DirectX::XMStoreFloat4x4(&skycb.InvViewProj, invViewProj);
    skycb.CameraPosWs = cameraPosWs;
    skycb.AtmosphereHeight = std::max(0.5f, sky.AtmosphereHeight);
    skycb.SunDirWs = lightDirWs;
    skycb.SunIntensity = sunIntensity;
    skycb.SunColor = { 1.0f, 0.98f, 0.92f };
    skycb.RayleighScale = sky.RayleighScale;
    skycb.MieScale = sky.MieScale;
    skycb.MieG = sky.MieG;
    skycb.GroundAlbedo = sky.GroundAlbedo;
    std::memcpy(CBMappedSky[frameIndex], &skycb, sizeof(skycb));
}

/**
 * @brief 添加天空渲染 Pass（全屏三角形）。
 * @param graph 渲染图构建器。
 * @param frame 当前帧上下文。
 * @param vp 视口。
 * @param sc 裁剪区域。
 * @param hdrRtv HDR 目标 RTV。
 * @param enable 是否启用天空渲染。
 * @return 无返回值。
 * @note 阶段：天空渲染阶段。
 */
void FSimpleSceneRenderer::AddSkyPass(
    FRenderGraphBuilder& graph,
    const FD3D12FrameContext& frame,
    const D3D12_VIEWPORT& vp,
    const D3D12_RECT& sc,
    D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv,
    bool enable)
{
    auto skySig = SkyRootSig.Get();
    auto skyPso = SkyPSO.Get();
    const D3D12_GPU_VIRTUAL_ADDRESS skyCB = ConstantBufferSky[frame.FrameIndex]->GetGPUVirtualAddress();

    graph.AddPass("DrawSky", [=](ID3D12GraphicsCommandList* cl)
    {
        // 禁用时跳过。
        if (!enable) return;
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        cl->OMSetRenderTargets(1, &hdrRtv, FALSE, nullptr);
        cl->SetPipelineState(skyPso);
        cl->SetGraphicsRootSignature(skySig);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->SetGraphicsRootConstantBufferView(0, skyCB);
        cl->DrawInstanced(3, 1, 0, 0);
    });
}
