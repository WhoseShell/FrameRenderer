#include "Renderer/SimpleSceneRenderer.h"

#include <algorithm>
#include <cstring>

#include "Core/Diagnostics.h"
#include "Renderer/RenderGraph.h"

/**
 * @brief 初始化 Tonemap 通道（RootSig/PSO）。
 * @param rhi 渲染硬件接口。
 * @param blend 混合状态描述。
 * @param rast 光栅化状态描述。
 * @return 无返回值。
 * @note 阶段：后处理通道初始化阶段。
 */
void FSimpleSceneRenderer::InitTonemapPass(FD3D12RHI& rhi, const D3D12_BLEND_DESC& blend, const D3D12_RASTERIZER_DESC& rast)
{
    ID3D12Device* device = rhi.GetDevice();
    if (!device) return;

    // RootSignature：CBV + HDR SRV。
    D3D12_ROOT_PARAMETER paramsTM[2]{};
    paramsTM[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    paramsTM[0].Descriptor.ShaderRegister = 0;
    paramsTM[0].Descriptor.RegisterSpace = 0;
    paramsTM[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE rangeTM{};
    rangeTM.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rangeTM.NumDescriptors = 1;
    rangeTM.BaseShaderRegister = 0;
    rangeTM.RegisterSpace = 0;
    rangeTM.OffsetInDescriptorsFromTableStart = 0;

    paramsTM[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    paramsTM[1].DescriptorTable.NumDescriptorRanges = 1;
    paramsTM[1].DescriptorTable.pDescriptorRanges = &rangeTM;
    paramsTM[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampTM{};
    sampTM.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampTM.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampTM.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampTM.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampTM.MipLODBias = 0.0f;
    sampTM.MaxAnisotropy = 1;
    sampTM.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampTM.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampTM.MinLOD = 0.0f;
    sampTM.MaxLOD = D3D12_FLOAT32_MAX;
    sampTM.ShaderRegister = 0;
    sampTM.RegisterSpace = 0;
    sampTM.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsTM{};
    rsTM.NumParameters = 2;
    rsTM.pParameters = paramsTM;
    rsTM.NumStaticSamplers = 1;
    rsTM.pStaticSamplers = &sampTM;
    rsTM.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigTM, errTM;
    HRESULT hrTM = D3D12SerializeRootSignature(&rsTM, D3D_ROOT_SIGNATURE_VERSION_1, &sigTM, &errTM);
    if (errTM)
    {
        std::string err((const char*)errTM->GetBufferPointer(), errTM->GetBufferSize());
        DebugOutput(std::wstring(err.begin(), err.end()));
    }
    ThrowIfFailed(hrTM, "D3D12SerializeRootSignature (tonemap) failed");
    ThrowIfFailed(device->CreateRootSignature(0, sigTM->GetBufferPointer(), sigTM->GetBufferSize(), IID_PPV_ARGS(&TonemapRootSig)),
                  "CreateRootSignature (tonemap) failed");

    // 编译 Tonemap Shader 并创建 PSO。
    std::wstring tonemapPath = std::wstring(SHADER_DIR) + L"/tonemap.hlsl";
    auto vsTM = CompileShaderFromFile(tonemapPath, "VSMain", "vs_5_0");
    auto psTM = CompileShaderFromFile(tonemapPath, "PSMain", "ps_5_0");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTM{};
    psoTM.pRootSignature = TonemapRootSig.Get();
    psoTM.VS = { vsTM->GetBufferPointer(), vsTM->GetBufferSize() };
    psoTM.PS = { psTM->GetBufferPointer(), psTM->GetBufferSize() };
    psoTM.BlendState = blend;
    psoTM.RasterizerState = rast;
    psoTM.DepthStencilState.DepthEnable = FALSE;
    psoTM.DepthStencilState.StencilEnable = FALSE;
    psoTM.SampleMask = UINT_MAX;
    psoTM.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoTM.NumRenderTargets = 1;
    psoTM.RTVFormats[0] = rhi.GetBackBufferFormat();
    psoTM.SampleDesc.Count = 1;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoTM, IID_PPV_ARGS(&TonemapPSO)), "CreateGraphicsPipelineState (tonemap) failed");
}

/**
 * @brief 更新 Tonemap 常量缓冲。
 * @param tonemapOperator Tonemap operator, or None for gamma-only output.
 * @param frameIndex 帧索引。
 * @return 无返回值。
 * @note 阶段：后处理参数更新阶段。
 */
void FSimpleSceneRenderer::UpdateTonemapCB(ETonemapOperator tonemapOperator, uint32 frameIndex, float targetWidth, float targetHeight)
{
    if (!CBMappedTonemap[frameIndex])
        return;

    // 填充 Tonemap 参数。
    FTonemapCB tcb{};
    tcb.EnableTonemap = (tonemapOperator != ETonemapOperator::None) ? 1.0f : 0.0f;
    tcb.Exposure = 0.6f;
    tcb.Gamma = 2.2f;
    tcb.TonemapOperator = static_cast<float>(static_cast<uint32>(tonemapOperator));
    tcb.TargetWidth = std::max(1.0f, targetWidth);
    tcb.TargetHeight = std::max(1.0f, targetHeight);
    tcb.InvTargetWidth = 1.0f / tcb.TargetWidth;
    tcb.InvTargetHeight = 1.0f / tcb.TargetHeight;
    std::memcpy(CBMappedTonemap[frameIndex], &tcb, sizeof(tcb));
}

/**
 * @brief 将 HDR 目标转换为 SRV 可读状态。
 * @param graph 渲染图构建器。
 * @return 无返回值。
 * @note 阶段：后处理前的资源状态转换阶段。
 */
void FSimpleSceneRenderer::AddHDRToSRVPass(FRenderGraphBuilder& graph)
{
    auto hdr = HDRColor.Get();
    graph.AddPass("HDRToSRV", [=, this](ID3D12GraphicsCommandList* cl)
    {
        if (hdr && HDRState == D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = hdr;
            b.Transition.StateBefore = HDRState;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
            HDRState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
    });
}

/**
 * @brief 添加 Tonemap Pass（HDR -> BackBuffer）。
 * @param graph 渲染图构建器。
 * @param frame 当前帧上下文。
 * @param vp 视口。
 * @param sc 裁剪区域。
 * @return 无返回值。
 * @note 阶段：后处理 Tonemap 阶段。
 */
void FSimpleSceneRenderer::AddTonemapPass(
    FRenderGraphBuilder& graph,
    const FD3D12FrameContext& frame,
    const D3D12_VIEWPORT& vp,
    const D3D12_RECT& sc)
{
    auto tonemapSig = TonemapRootSig.Get();
    auto tonemapPso = TonemapPSO.Get();
    auto tonemapHeap = TonemapSRVHeap.Get();
    const D3D12_GPU_VIRTUAL_ADDRESS tonemapCB = ConstantBufferTonemap[frame.FrameIndex]->GetGPUVirtualAddress();
    const D3D12_GPU_DESCRIPTOR_HANDLE tonemapSrv = TonemapHDRSRVGPU;

    graph.AddPass("Tonemap", [=](ID3D12GraphicsCommandList* cl)
    {
        // 全屏三角形输出到后备缓冲。
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        cl->OMSetRenderTargets(1, &frame.RTV, FALSE, nullptr);

        cl->SetPipelineState(tonemapPso);
        cl->SetGraphicsRootSignature(tonemapSig);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        if (tonemapHeap)
        {
            ID3D12DescriptorHeap* heaps[] = { tonemapHeap };
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetGraphicsRootDescriptorTable(1, tonemapSrv);
        }
        cl->SetGraphicsRootConstantBufferView(0, tonemapCB);
        cl->DrawInstanced(3, 1, 0, 0);
    });
}
