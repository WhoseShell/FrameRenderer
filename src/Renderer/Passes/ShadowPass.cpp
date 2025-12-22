#include "Renderer/SimpleSceneRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Core/Diagnostics.h"
#include "Renderer/RenderGraph.h"

void FSimpleSceneRenderer::InitShadowPass(
    FD3D12RHI& rhi,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& basePso,
    const D3D12_RASTERIZER_DESC& rast)
{
    ID3D12Device* device = rhi.GetDevice();
    if (!device) return;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].Descriptor.RegisterSpace = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = params;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigShadow, errShadow;
    HRESULT hrShadow = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigShadow, &errShadow);
    if (errShadow)
    {
        std::string err((const char*)errShadow->GetBufferPointer(), errShadow->GetBufferSize());
        DebugOutput(std::wstring(err.begin(), err.end()));
    }
    ThrowIfFailed(hrShadow, "D3D12SerializeRootSignature (shadow) failed");
    ThrowIfFailed(device->CreateRootSignature(0, sigShadow->GetBufferPointer(), sigShadow->GetBufferSize(), IID_PPV_ARGS(&ShadowRootSig)),
                  "CreateRootSignature (shadow) failed");

    std::wstring shadowPath = std::wstring(SHADER_DIR) + L"/shadow.hlsl";
    auto vsShadow = CompileShaderFromFile(shadowPath, "VSMain", "vs_5_0");
    auto psShadow = CompileShaderFromFile(shadowPath, "PSMain", "ps_5_0");

    D3D12_RASTERIZER_DESC rastShadow = rast;
    rastShadow.DepthBias = 1000;
    rastShadow.SlopeScaledDepthBias = 1.5f;
    rastShadow.DepthBiasClamp = 0.0f;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoShadow = basePso;
    psoShadow.pRootSignature = ShadowRootSig.Get();
    psoShadow.VS = { vsShadow->GetBufferPointer(), vsShadow->GetBufferSize() };
    psoShadow.PS = { psShadow->GetBufferPointer(), psShadow->GetBufferSize() };
    psoShadow.RasterizerState = rastShadow;
    psoShadow.NumRenderTargets = 0;
    for (int i = 0; i < 8; ++i)
        psoShadow.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
    psoShadow.DSVFormat = ShadowDepthFormat;
    psoShadow.DepthStencilState.DepthEnable = TRUE;
    psoShadow.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoShadow.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    psoShadow.DepthStencilState.StencilEnable = FALSE;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoShadow, IID_PPV_ARGS(&ShadowPSO)), "CreateGraphicsPipelineState (shadow) failed");
}

void FSimpleSceneRenderer::UpdateShadowCB(
    const DirectX::XMFLOAT3& cameraPosWs,
    const DirectX::XMFLOAT3& lightDirWs,
    uint32 frameIndex)
{
    if (!CBMappedShadow[frameIndex])
        return;

    using namespace DirectX;
    const float shadowDistance = 35.0f;
    const float shadowHalfSize = 18.0f;
    const float nearZ = 0.1f;
    const float farZ = shadowDistance * 2.0f + shadowHalfSize;

    XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&lightDirWs));
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    const float upDot = XMVectorGetX(XMVector3Dot(dir, up));
    if (std::fabs(upDot) > 0.98f)
        up = XMVectorSet(1, 0, 0, 0);

    XMVECTOR camPos = XMLoadFloat3(&cameraPosWs);
    XMVECTOR lightPos = camPos - dir * shadowDistance;
    XMMATRIX view = XMMatrixLookAtLH(lightPos, camPos, up);
    XMMATRIX proj = XMMatrixOrthographicLH(shadowHalfSize * 2.0f, shadowHalfSize * 2.0f, nearZ, farZ);

    FShadowCB cb{};
    XMStoreFloat4x4(&cb.LightViewProj, view * proj);
    cb.ShadowInvSize = { 1.0f / (float)std::max<uint32>(1u, ShadowMapSize), 1.0f / (float)std::max<uint32>(1u, ShadowMapSize) };
    cb.ShadowBias = 0.0015f;
    cb.ShadowStrength = 1.0f;
    std::memcpy(CBMappedShadow[frameIndex], &cb, sizeof(cb));
}

void FSimpleSceneRenderer::AddShadowPass(
    FRenderGraphBuilder& graph,
    const FD3D12FrameContext& frame,
    const std::vector<FSceneObject>& objects,
    const DirectX::XMFLOAT3* previewPos,
    FSceneObject::EType previewType)
{
    auto shadowMap = ShadowMap.Get();
    auto shadowSig = ShadowRootSig.Get();
    auto shadowPso = ShadowPSO.Get();
    const D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv = ShadowDSV;
    const uint32 shadowSize = ShadowMapSize;

    const D3D12_GPU_VIRTUAL_ADDRESS cbBase = ConstantBufferObjects[frame.FrameIndex]->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS shadowCB = ConstantBufferShadow[frame.FrameIndex]->GetGPUVirtualAddress();

    graph.AddPass("ShadowMap", [=, this](ID3D12GraphicsCommandList* cl)
    {
        if (!shadowMap || !shadowSig || !shadowPso)
            return;

        if (ShadowState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = shadowMap;
            b.Transition.StateBefore = ShadowState;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
            ShadowState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        }

        const float size = (float)std::max<uint32>(1u, shadowSize);
        const D3D12_VIEWPORT vp{ 0.0f, 0.0f, size, size, 0.0f, 1.0f };
        const D3D12_RECT sc{ 0, 0, (LONG)size, (LONG)size };
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        cl->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);
        cl->ClearDepthStencilView(shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        cl->SetPipelineState(shadowPso);
        cl->SetGraphicsRootSignature(shadowSig);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->SetGraphicsRootConstantBufferView(1, shadowCB);

        const uint32 drawCount = (uint32)std::min<size_t>(objects.size(), MaxObjects);
        for (uint32 i = 0; i < drawCount; ++i)
        {
            const auto& mesh = GetMesh(objects[i].Type);
            cl->SetGraphicsRootConstantBufferView(0, cbBase + (UINT64)CBSize * i);
            cl->IASetVertexBuffers(0, 1, &mesh.VBView);
            cl->IASetIndexBuffer(&mesh.IBView);
            cl->DrawIndexedInstanced(mesh.IndexCount, 1, 0, 0, 0);
        }

        if (previewPos && drawCount < MaxObjects)
        {
            const auto& mesh = GetMesh(previewType);
            cl->SetGraphicsRootConstantBufferView(0, cbBase + (UINT64)CBSize * drawCount);
            cl->IASetVertexBuffers(0, 1, &mesh.VBView);
            cl->IASetIndexBuffer(&mesh.IBView);
            cl->DrawIndexedInstanced(mesh.IndexCount, 1, 0, 0, 0);
        }

        if (ShadowState == D3D12_RESOURCE_STATE_DEPTH_WRITE)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = shadowMap;
            b.Transition.StateBefore = ShadowState;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
            ShadowState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
    });
}
