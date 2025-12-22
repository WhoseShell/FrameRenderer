#include "Renderer/SimpleSceneRenderer.h"

#include <algorithm>
#include <cstring>

#include "Core/Diagnostics.h"
#include "Renderer/RenderGraph.h"

void FSimpleSceneRenderer::InitDeferredPass(
    FD3D12RHI& rhi,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& basePso,
    const D3D12_BLEND_DESC& blend,
    const D3D12_RASTERIZER_DESC& rast)
{
    ID3D12Device* device = rhi.GetDevice();
    if (!device) return;

    std::wstring deferredPath = std::wstring(SHADER_DIR) + L"/deferred.hlsl";
    auto psGBuffer = CompileShaderFromFile(deferredPath, "PSGBuffer", "ps_5_0");
    auto vsLight = CompileShaderFromFile(deferredPath, "VSFullscreen", "vs_5_0");
    auto psLight = CompileShaderFromFile(deferredPath, "PSDeferredLighting", "ps_5_0");

    // GBuffer PSO: same root signature + VS as forward, different PS + MRT outputs.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gbufPso = basePso;
    gbufPso.PS = { psGBuffer->GetBufferPointer(), psGBuffer->GetBufferSize() };
    gbufPso.NumRenderTargets = 3;
    gbufPso.RTVFormats[0] = GBuffer0Format;
    gbufPso.RTVFormats[1] = GBuffer1Format;
    gbufPso.RTVFormats[2] = GBuffer2Format;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&gbufPso, IID_PPV_ARGS(&PSO_GBuffer)), "CreateGraphicsPipelineState (gbuffer) failed");

    // Deferred lighting root signature: b0 + gbuffer SRVs + shadow CB + shadow SRV
    D3D12_ROOT_PARAMETER paramsDL[4]{};
    paramsDL[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    paramsDL[0].Descriptor.ShaderRegister = 0;
    paramsDL[0].Descriptor.RegisterSpace = 0;
    paramsDL[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE rangeDL{};
    rangeDL.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rangeDL.NumDescriptors = 3;
    rangeDL.BaseShaderRegister = 0;
    rangeDL.RegisterSpace = 0;
    rangeDL.OffsetInDescriptorsFromTableStart = 0;

    paramsDL[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    paramsDL[1].DescriptorTable.NumDescriptorRanges = 1;
    paramsDL[1].DescriptorTable.pDescriptorRanges = &rangeDL;
    paramsDL[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    paramsDL[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    paramsDL[2].Descriptor.ShaderRegister = 1;
    paramsDL[2].Descriptor.RegisterSpace = 0;
    paramsDL[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE rangeShadow{};
    rangeShadow.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rangeShadow.NumDescriptors = 3; // shadow map + sky prefilter + sky SH
    rangeShadow.BaseShaderRegister = 5;
    rangeShadow.RegisterSpace = 0;
    rangeShadow.OffsetInDescriptorsFromTableStart = 0;

    paramsDL[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    paramsDL[3].DescriptorTable.NumDescriptorRanges = 1;
    paramsDL[3].DescriptorTable.pDescriptorRanges = &rangeShadow;
    paramsDL[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplersDL[2]{};
    samplersDL[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplersDL[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplersDL[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplersDL[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplersDL[0].MipLODBias = 0.0f;
    samplersDL[0].MaxAnisotropy = 1;
    samplersDL[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samplersDL[0].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    samplersDL[0].MinLOD = 0.0f;
    samplersDL[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplersDL[0].ShaderRegister = 0;
    samplersDL[0].RegisterSpace = 0;
    samplersDL[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    samplersDL[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplersDL[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplersDL[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplersDL[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplersDL[1].MipLODBias = 0.0f;
    samplersDL[1].MaxAnisotropy = 1;
    samplersDL[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplersDL[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplersDL[1].MinLOD = 0.0f;
    samplersDL[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplersDL[1].ShaderRegister = 1;
    samplersDL[1].RegisterSpace = 0;
    samplersDL[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDL{};
    rsDL.NumParameters = 4;
    rsDL.pParameters = paramsDL;
    rsDL.NumStaticSamplers = 2;
    rsDL.pStaticSamplers = samplersDL;
    rsDL.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigDL, errDL;
    HRESULT hrDL = D3D12SerializeRootSignature(&rsDL, D3D_ROOT_SIGNATURE_VERSION_1, &sigDL, &errDL);
    if (errDL)
    {
        std::string err((const char*)errDL->GetBufferPointer(), errDL->GetBufferSize());
        DebugOutput(std::wstring(err.begin(), err.end()));
    }
    ThrowIfFailed(hrDL, "D3D12SerializeRootSignature (deferred lighting) failed");
    ThrowIfFailed(device->CreateRootSignature(0, sigDL->GetBufferPointer(), sigDL->GetBufferSize(), IID_PPV_ARGS(&DeferredLightRootSig)),
                  "CreateRootSignature (deferred lighting) failed");

    D3D12_BLEND_DESC blendLight = blend;
    blendLight.RenderTarget[0].BlendEnable = TRUE;
    blendLight.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blendLight.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blendLight.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendLight.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blendLight.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blendLight.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDL{};
    psoDL.pRootSignature = DeferredLightRootSig.Get();
    psoDL.VS = { vsLight->GetBufferPointer(), vsLight->GetBufferSize() };
    psoDL.PS = { psLight->GetBufferPointer(), psLight->GetBufferSize() };
    psoDL.BlendState = blendLight;
    psoDL.RasterizerState = rast;
    psoDL.DepthStencilState.DepthEnable = FALSE;
    psoDL.DepthStencilState.StencilEnable = FALSE;
    psoDL.SampleMask = UINT_MAX;
    psoDL.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDL.NumRenderTargets = 1;
    psoDL.RTVFormats[0] = HDRFormat;
    psoDL.SampleDesc.Count = 1;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDL, IID_PPV_ARGS(&DeferredLightPSO)), "CreateGraphicsPipelineState (deferred lighting) failed");
}

void FSimpleSceneRenderer::UpdateDeferredCB(
    const DirectX::XMFLOAT3& cameraPosWs,
    const DirectX::XMFLOAT3& lightDirWs,
    float sunIntensity,
    uint32 frameIndex)
{
    if (!CBMappedDeferred[frameIndex])
        return;

    FDeferredLightCB dcb{};
    dcb.CameraPosWs = cameraPosWs;
    dcb.LightDirWs = lightDirWs;
    dcb.LightColor = { 1.0f, 0.98f, 0.92f };
    dcb.LightIntensity = sunIntensity;
    std::memcpy(CBMappedDeferred[frameIndex], &dcb, sizeof(dcb));
}

void FSimpleSceneRenderer::AddGBufferPasses(
    FRenderGraphBuilder& graph,
    const FD3D12FrameContext& frame,
    const D3D12_VIEWPORT& vp,
    const D3D12_RECT& sc,
    const std::vector<FSceneObject>& objects,
    const DirectX::XMFLOAT3* previewPos,
    FSceneObject::EType previewType)
{
    auto gbuf0 = GBuffer0.Get();
    auto gbuf1 = GBuffer1.Get();
    auto gbuf2 = GBuffer2.Get();
    const D3D12_CPU_DESCRIPTOR_HANDLE gbufRtv0 = GBufferRTVs[0];
    const D3D12_CPU_DESCRIPTOR_HANDLE gbufRtv1 = GBufferRTVs[1];
    const D3D12_CPU_DESCRIPTOR_HANDLE gbufRtv2 = GBufferRTVs[2];

    auto rootSig = RootSig.Get();
    auto psoGBuffer = PSO_GBuffer.Get();
    auto srvHeap = SRVHeap.Get();
    const D3D12_GPU_VIRTUAL_ADDRESS cbBase = ConstantBufferObjects[frame.FrameIndex]->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS shadowCB = ConstantBufferShadow[frame.FrameIndex]->GetGPUVirtualAddress();
    const uint32 srvStride = SRVDescriptorSize;
    D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvGpu{};
    if (srvHeap)
    {
        shadowSrvGpu = srvHeap->GetGPUDescriptorHandleForHeapStart();
        shadowSrvGpu.ptr += SIZE_T(kShadowMapSRVSlot) * srvStride;
    }

    graph.AddPass("ClearGBuffer", [=, this](ID3D12GraphicsCommandList* cl)
    {
        ID3D12Resource* res[3] = { gbuf0, gbuf1, gbuf2 };
        for (int i = 0; i < 3; ++i)
        {
            if (!res[i]) continue;
            if (GBufferStates[i] != D3D12_RESOURCE_STATE_RENDER_TARGET)
            {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = res[i];
                b.Transition.StateBefore = GBufferStates[i];
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                cl->ResourceBarrier(1, &b);
                GBufferStates[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
        }

        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);

        const float clear0[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const float clear1[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
        const float clear2[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        cl->ClearRenderTargetView(gbufRtv0, clear0, 0, nullptr);
        cl->ClearRenderTargetView(gbufRtv1, clear1, 0, nullptr);
        cl->ClearRenderTargetView(gbufRtv2, clear2, 0, nullptr);
    });

    graph.AddPass("GBuffer", [=, this](ID3D12GraphicsCommandList* cl)
    {
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = { gbufRtv0, gbufRtv1, gbufRtv2 };
        cl->OMSetRenderTargets(3, rtvs, FALSE, &frame.DSV);

        cl->SetPipelineState(psoGBuffer);
        cl->SetGraphicsRootSignature(rootSig);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (srvHeap)
        {
            ID3D12DescriptorHeap* heaps[] = { srvHeap };
            cl->SetDescriptorHeaps(1, heaps);
        }
        cl->SetGraphicsRootConstantBufferView(2, shadowCB);
        if (srvHeap)
            cl->SetGraphicsRootDescriptorTable(3, shadowSrvGpu);

        const uint32 drawCount = (uint32)std::min<size_t>(objects.size(), MaxObjects);
        for (uint32 i = 0; i < drawCount; ++i)
        {
            const auto& mesh = GetMesh(objects[i].Type);
            cl->SetGraphicsRootConstantBufferView(0, cbBase + (UINT64)CBSize * i);
            if (srvHeap)
            {
                D3D12_GPU_DESCRIPTOR_HANDLE gpu = srvHeap->GetGPUDescriptorHandleForHeapStart();
                const uint32 base = (objects[i].MaterialSRVBase >= 0) ? (uint32)objects[i].MaterialSRVBase : 0u;
                gpu.ptr += SIZE_T(std::min<uint32>(base, 2047u)) * srvStride;
                cl->SetGraphicsRootDescriptorTable(1, gpu);
            }
            cl->IASetVertexBuffers(0, 1, &mesh.VBView);
            cl->IASetIndexBuffer(&mesh.IBView);
            cl->DrawIndexedInstanced(mesh.IndexCount, 1, 0, 0, 0);
        }

        if (previewPos && drawCount < MaxObjects)
        {
            const auto& mesh = GetMesh(previewType);
            cl->SetGraphicsRootConstantBufferView(0, cbBase + (UINT64)CBSize * drawCount);
            if (srvHeap)
            {
                D3D12_GPU_DESCRIPTOR_HANDLE gpu = srvHeap->GetGPUDescriptorHandleForHeapStart();
                gpu.ptr += SIZE_T(0) * srvStride;
                cl->SetGraphicsRootDescriptorTable(1, gpu);
            }
            cl->IASetVertexBuffers(0, 1, &mesh.VBView);
            cl->IASetIndexBuffer(&mesh.IBView);
            cl->DrawIndexedInstanced(mesh.IndexCount, 1, 0, 0, 0);
        }
    });

    graph.AddPass("GBufferToSRV", [=, this](ID3D12GraphicsCommandList* cl)
    {
        ID3D12Resource* res[3] = { gbuf0, gbuf1, gbuf2 };
        for (int i = 0; i < 3; ++i)
        {
            if (!res[i]) continue;
            if (GBufferStates[i] == D3D12_RESOURCE_STATE_RENDER_TARGET)
            {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = res[i];
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                cl->ResourceBarrier(1, &b);
                GBufferStates[i] = D3D12_RESOURCE_STATE_GENERIC_READ;
            }
        }
    });
}

void FSimpleSceneRenderer::AddDeferredLightingPass(
    FRenderGraphBuilder& graph,
    const FD3D12FrameContext& frame,
    const D3D12_VIEWPORT& vp,
    const D3D12_RECT& sc,
    D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv)
{
    auto lightSig = DeferredLightRootSig.Get();
    auto lightPso = DeferredLightPSO.Get();
    auto gbufSrvHeap = GBufferSRVHeap.Get();
    const D3D12_GPU_DESCRIPTOR_HANDLE gbufSrvBase = GBufferSRVBaseGPU;
    const D3D12_GPU_VIRTUAL_ADDRESS deferredCB = ConstantBufferDeferred[frame.FrameIndex]->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS shadowCB = ConstantBufferShadow[frame.FrameIndex]->GetGPUVirtualAddress();
    D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvGpu = gbufSrvBase;
    shadowSrvGpu.ptr += SIZE_T(kDesc_ShadowMap) * GBufferSRVDescriptorSize;

    graph.AddPass("DeferredLighting", [=](ID3D12GraphicsCommandList* cl)
    {
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        cl->OMSetRenderTargets(1, &hdrRtv, FALSE, nullptr);

        cl->SetPipelineState(lightPso);
        cl->SetGraphicsRootSignature(lightSig);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        if (gbufSrvHeap)
        {
            ID3D12DescriptorHeap* heaps[] = { gbufSrvHeap };
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetGraphicsRootDescriptorTable(1, gbufSrvBase);
            cl->SetGraphicsRootDescriptorTable(3, shadowSrvGpu);
        }
        cl->SetGraphicsRootConstantBufferView(0, deferredCB);
        cl->SetGraphicsRootConstantBufferView(2, shadowCB);
        cl->DrawInstanced(3, 1, 0, 0);
    });
}
