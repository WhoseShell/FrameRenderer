#include "Renderer/SimpleSceneRenderer.h"

#include <algorithm>
#include <cstring>

#include "Core/Diagnostics.h"
#include "Renderer/RenderGraph.h"

void FSimpleSceneRenderer::InitLumenPass(FD3D12RHI& rhi, const D3D12_BLEND_DESC& blend, const D3D12_RASTERIZER_DESC& rast)
{
    ID3D12Device* device = rhi.GetDevice();
    if (!device) return;

    std::wstring lumenPath = std::wstring(SHADER_DIR) + L"/lumen.hlsl";
    auto vsL = CompileShaderFromFile(lumenPath, "VSFullscreen", "vs_5_0");
    auto psL = CompileShaderFromFile(lumenPath, "PSLumen", "ps_5_0");

    D3D12_ROOT_PARAMETER paramsL[2]{};
    paramsL[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    paramsL[0].Descriptor.ShaderRegister = 0;
    paramsL[0].Descriptor.RegisterSpace = 0;
    paramsL[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE rangeL{};
    rangeL.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rangeL.NumDescriptors = kDesc_SkySH + 1; // gbuffer + history + sky IBL + extra slots
    rangeL.BaseShaderRegister = 0;
    rangeL.RegisterSpace = 0;
    rangeL.OffsetInDescriptorsFromTableStart = 0;

    paramsL[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    paramsL[1].DescriptorTable.NumDescriptorRanges = 1;
    paramsL[1].DescriptorTable.pDescriptorRanges = &rangeL;
    paramsL[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampL{};
    sampL.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampL.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampL.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampL.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampL.MipLODBias = 0.0f;
    sampL.MaxAnisotropy = 1;
    sampL.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampL.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampL.MinLOD = 0.0f;
    sampL.MaxLOD = D3D12_FLOAT32_MAX;
    sampL.ShaderRegister = 0;
    sampL.RegisterSpace = 0;
    sampL.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsL{};
    rsL.NumParameters = 2;
    rsL.pParameters = paramsL;
    rsL.NumStaticSamplers = 1;
    rsL.pStaticSamplers = &sampL;
    rsL.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigL, errL;
    HRESULT hrL = D3D12SerializeRootSignature(&rsL, D3D_ROOT_SIGNATURE_VERSION_1, &sigL, &errL);
    if (errL)
    {
        std::string err((const char*)errL->GetBufferPointer(), errL->GetBufferSize());
        DebugOutput(std::wstring(err.begin(), err.end()));
    }
    ThrowIfFailed(hrL, "D3D12SerializeRootSignature (lumen) failed");
    ThrowIfFailed(device->CreateRootSignature(0, sigL->GetBufferPointer(), sigL->GetBufferSize(), IID_PPV_ARGS(&LumenRootSig)),
                  "CreateRootSignature (lumen) failed");

    D3D12_BLEND_DESC blendLumen = blend;
    blendLumen.IndependentBlendEnable = TRUE;
    // RT0 (HDR): additive
    blendLumen.RenderTarget[0].BlendEnable = TRUE;
    blendLumen.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blendLumen.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    blendLumen.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendLumen.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blendLumen.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    blendLumen.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    // RT1 (History): overwrite
    blendLumen.RenderTarget[1].BlendEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoL{};
    psoL.pRootSignature = LumenRootSig.Get();
    psoL.VS = { vsL->GetBufferPointer(), vsL->GetBufferSize() };
    psoL.PS = { psL->GetBufferPointer(), psL->GetBufferSize() };
    psoL.BlendState = blendLumen;
    psoL.RasterizerState = rast;
    psoL.DepthStencilState.DepthEnable = FALSE;
    psoL.DepthStencilState.StencilEnable = FALSE;
    psoL.SampleMask = UINT_MAX;
    psoL.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoL.NumRenderTargets = 2;
    psoL.RTVFormats[0] = HDRFormat;
    psoL.RTVFormats[1] = LumenHistoryFormat;
    psoL.SampleDesc.Count = 1;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoL, IID_PPV_ARGS(&LumenPSO)), "CreateGraphicsPipelineState (lumen) failed");
}

void FSimpleSceneRenderer::UpdateLumenCB(
    FD3D12RHI& rhi,
    bool bUseLumen,
    const DirectX::XMFLOAT4X4& curViewProj,
    const DirectX::XMFLOAT3& cameraPosWs,
    const DirectX::XMFLOAT3& lightDirWs,
    float sunIntensity,
    float timeSeconds,
    uint32 frameIndex)
{
    if (!bUseLumen || !CBMappedLumen[frameIndex])
        return;

    const int writeIdx = int(frameIndex) & 1;
    const int prevIdx = 1 - writeIdx;

    FLumenCB lcb{};
    lcb.ViewProj = curViewProj;
    lcb.PrevViewProj = (bLumenHistoryValid ? PrevViewProj : curViewProj);
    lcb.CameraPosWs = cameraPosWs;
    lcb.TemporalWeight = 0.92f;
    lcb.LightDirWs = lightDirWs;
    lcb.MaxTraceDistance = 12.0f;
    lcb.LightColor = { 1.0f, 0.98f, 0.92f };
    lcb.LightIntensity = sunIntensity;
    lcb.InvResolution = { 1.0f / std::max(1.0f, (float)rhi.GetWidth()), 1.0f / std::max(1.0f, (float)rhi.GetHeight()) };
    lcb.StepSize = 0.25f;
    lcb.Intensity = 1.0f;
    lcb.FrameIndex = timeSeconds * 60.0f;
    lcb.PrevHistoryIndex = (float)prevIdx;
    lcb.HistoryValid = bLumenHistoryValid ? 1.0f : 0.0f;
    std::memcpy(CBMappedLumen[frameIndex], &lcb, sizeof(lcb));
}

void FSimpleSceneRenderer::AddLumenPass(
    FRenderGraphBuilder& graph,
    const FD3D12FrameContext& frame,
    const D3D12_VIEWPORT& vp,
    const D3D12_RECT& sc,
    D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv)
{
    const int writeIdx = int(frame.FrameIndex) & 1;
    ID3D12Resource* histWrite = LumenHistory[writeIdx].Get();
    const D3D12_CPU_DESCRIPTOR_HANDLE histRtv = LumenRTVs[writeIdx];
    auto lumenSig = LumenRootSig.Get();
    auto lumenPso = LumenPSO.Get();
    const D3D12_GPU_VIRTUAL_ADDRESS lumenCB = ConstantBufferLumen[frame.FrameIndex]->GetGPUVirtualAddress();
    auto gbufSrvHeap = GBufferSRVHeap.Get();
    const D3D12_GPU_DESCRIPTOR_HANDLE gbufSrvBase = GBufferSRVBaseGPU;

    graph.AddPass("Lumen", [=, this](ID3D12GraphicsCommandList* cl)
    {
        if (!histWrite) return;

        if (LumenHistoryStates[writeIdx] != D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = histWrite;
            b.Transition.StateBefore = LumenHistoryStates[writeIdx];
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
            LumenHistoryStates[writeIdx] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = { hdrRtv, histRtv };
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        cl->OMSetRenderTargets(2, rtvs, FALSE, nullptr);

        cl->SetPipelineState(lumenPso);
        cl->SetGraphicsRootSignature(lumenSig);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        if (gbufSrvHeap)
        {
            ID3D12DescriptorHeap* heaps[] = { gbufSrvHeap };
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetGraphicsRootDescriptorTable(1, gbufSrvBase);
        }
        cl->SetGraphicsRootConstantBufferView(0, lumenCB);
        cl->DrawInstanced(3, 1, 0, 0);
    });

    graph.AddPass("LumenHistoryToSRV", [=, this](ID3D12GraphicsCommandList* cl)
    {
        if (!histWrite) return;
        if (LumenHistoryStates[writeIdx] == D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = histWrite;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
            LumenHistoryStates[writeIdx] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
    });
}
