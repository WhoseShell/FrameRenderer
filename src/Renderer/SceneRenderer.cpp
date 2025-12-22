#include "Renderer/SceneRenderer.h"

#include <algorithm>
#include <cstring>

#include "Renderer/RenderGraph.h"
#include "Renderer/SimpleSceneRenderer.h"

FSceneRenderer::FSceneRenderer(
    FSimpleSceneRenderer& renderer,
    const FSceneViewFamily& viewFamily,
    const FSceneView& view,
    const FSceneRendererInputs& inputs)
    : Renderer(renderer)
    , ViewFamily(viewFamily)
    , View(view)
    , Inputs(inputs)
{
}

void FSceneRenderer::Render()
{
    if (!ViewFamily.RHI || !View.Camera || !Inputs.Objects)
        return;

    FD3D12RHI& rhi = *ViewFamily.RHI;
    Renderer.EnsureHDRTargets(rhi);
    Renderer.EnsureShadowMap(rhi);

    const bool bDeferred = IsDeferredPath();
    bUseLumen = (bDeferred && ViewFamily.bEnableLumen);
    bUseLumenSWRT = (bDeferred && ViewFamily.bEnableLumen && ViewFamily.bEnableLumenSWRT);
    if (bDeferred)
        Renderer.EnsureDeferredTargets(rhi);
    Renderer.EnsureSkyIBLTargets(rhi);

    Frame = rhi.BeginFrame();
    ID3D12GraphicsCommandList* cmd = Frame.CmdList;

    DirectX::XMMATRIX invVP{};
    {
        using namespace DirectX;
        const float aspect = (rhi.GetHeight() == 0) ? 1.0f : (float)rhi.GetWidth() / (float)rhi.GetHeight();

        const XMMATRIX view = View.Camera->GetViewMatrix();
        const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, 0.1f, 100.0f);
        const XMMATRIX vp = view * proj;
        invVP = XMMatrixInverse(nullptr, vp);
        XMStoreFloat4x4(&ViewInfo.ViewProj, vp);
        XMStoreFloat4x4(&ViewInfo.InvViewProj, invVP);
    }

    const auto& objects = *Inputs.Objects;
    const bool hasSelection = (Inputs.SelectedIndex >= 0 && Inputs.SelectedIndex < (int)objects.size());

    // Update constant buffers (per-frame)
    {
        using namespace DirectX;
        const XMMATRIX viewProj = XMLoadFloat4x4(&ViewInfo.ViewProj);
        const uint32 drawCount = (uint32)std::min<size_t>(objects.size(), FSimpleSceneRenderer::MaxObjects);
        for (uint32 i = 0; i < drawCount; ++i)
        {
            const auto& obj = objects[i];
            const XMMATRIX scale = XMMatrixScaling(obj.Scale.x, obj.Scale.y, obj.Scale.z);
            const XMMATRIX world = scale * XMMatrixTranslation(obj.Position.x, obj.Position.y, obj.Position.z);
            const XMMATRIX worldInvT = XMMatrixTranspose(XMMatrixInverse(nullptr, world));

            FSimpleSceneRenderer::FSceneCB cb{};
            XMStoreFloat4x4(&cb.MVP, world * viewProj);
            XMStoreFloat4x4(&cb.World, world);
            XMStoreFloat4x4(&cb.WorldInvTranspose, worldInvT);
            cb.CameraPosWs = View.Camera->Position;
            cb.LightDirWs = View.LightDirWs;
            cb.LightColor = { 1.0f, 0.98f, 0.92f };
            cb.LightIntensity = ViewFamily.SunIntensity;
            cb.Albedo = obj.Albedo;
            cb.Metallic = obj.Metallic;
            cb.Roughness = obj.Roughness;
            cb.UseAlbedoTex = obj.UseAlbedoTex;
            cb.UseNormalTex = obj.UseNormalTex;
            cb.UseRoughnessTex = obj.UseRoughnessTex;
            cb.UseMetallicTex = obj.UseMetallicTex;
            cb.UseAOTex = obj.UseAOTex;

            std::memcpy(Renderer.CBMappedObjects[Frame.FrameIndex] + (size_t)Renderer.CBSize * i, &cb, sizeof(cb));
        }

        uint32 previewSlot = drawCount;
        if (Inputs.PreviewPos && previewSlot < FSimpleSceneRenderer::MaxObjects)
        {
            const XMMATRIX world = XMMatrixTranslation(Inputs.PreviewPos->x, Inputs.PreviewPos->y, Inputs.PreviewPos->z);
            const XMMATRIX worldInvT = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
            FSimpleSceneRenderer::FSceneCB cb{};
            XMStoreFloat4x4(&cb.MVP, world * viewProj);
            XMStoreFloat4x4(&cb.World, world);
            XMStoreFloat4x4(&cb.WorldInvTranspose, worldInvT);
            cb.CameraPosWs = View.Camera->Position;
            cb.LightDirWs = View.LightDirWs;
            cb.LightColor = { 1.0f, 0.98f, 0.92f };
            cb.LightIntensity = ViewFamily.SunIntensity * 0.5f;
            cb.Albedo = { 0.35f, 0.9f, 0.35f };
            cb.Metallic = 0.0f;
            cb.Roughness = 0.6f;
            std::memcpy(Renderer.CBMappedObjects[Frame.FrameIndex] + (size_t)Renderer.CBSize * previewSlot, &cb, sizeof(cb));
        }

        if (hasSelection)
        {
            const auto& obj = objects[Inputs.SelectedIndex];
            const XMMATRIX gizmoWorld = XMMatrixTranslation(obj.Position.x, obj.Position.y, obj.Position.z);
            const XMMATRIX gizmoInvT = XMMatrixIdentity();

            FSimpleSceneRenderer::FSceneCB cb{};
            XMStoreFloat4x4(&cb.MVP, gizmoWorld * viewProj);
            XMStoreFloat4x4(&cb.World, gizmoWorld);
            XMStoreFloat4x4(&cb.WorldInvTranspose, gizmoInvT);
            std::memcpy(Renderer.CBMappedGizmo[Frame.FrameIndex], &cb, sizeof(cb));
        }

        if (ViewFamily.Sky)
            Renderer.UpdateSkyCB(invVP, View.Camera->Position, View.LightDirWs, ViewFamily.SunIntensity, *ViewFamily.Sky, Frame.FrameIndex);
        Renderer.UpdateShadowCB(View.Camera->Position, View.LightDirWs, Frame.FrameIndex);
    }

    Renderer.UpdateTonemapCB(ViewFamily.bEnableTonemap, Frame.FrameIndex);

    if (bDeferred)
        Renderer.UpdateDeferredCB(View.Camera->Position, View.LightDirWs, ViewFamily.SunIntensity, Frame.FrameIndex);

    Renderer.UpdateLumenCB(rhi, bUseLumen, ViewInfo.ViewProj, View.Camera->Position, View.LightDirWs,
                           ViewFamily.SunIntensity, View.TimeSeconds, Frame.FrameIndex);

    bUseHWRTGI = (bUseLumen && ViewFamily.bEnableLumenHWRT && Renderer.bRaytracingSupported && Renderer.HWRTGIPSO &&
                  Renderer.HWRTGIFilterPSO && Renderer.HWRTGIAddPSO && Renderer.HWRTGIRootSig && Renderer.HWRTGIAddRootSig);

    RtInstanceCount = Renderer.UpdateHWRTGICBAndScene(
        rhi, bUseHWRTGI, ViewInfo.ViewProj, View.Camera->Position, View.LightDirWs, ViewFamily.SunIntensity, View.TimeSeconds,
        Frame.FrameIndex, objects, Inputs.PreviewPos, Inputs.PreviewType);

    bUseLumenSWRT = (bUseLumenSWRT && !bUseHWRTGI && Renderer.LumenSwrtRootSig && Renderer.LumenSwrtSurfacePSO &&
                     Renderer.LumenSwrtGIPSO && Renderer.LumenSwrtFilterPSO && Renderer.LumenSwrtAddPSO);

    SwrtObjectCount = Renderer.UpdateSWRTGICBAndObjects(
        rhi, bUseLumenSWRT, ViewInfo.ViewProj, View.Camera->Position, View.LightDirWs, ViewFamily.SunIntensity, View.TimeSeconds,
        Frame.FrameIndex, objects, Inputs.PreviewPos, Inputs.PreviewType);

    const float x0 = (float)std::max(0, ViewFamily.LeftInsetPx);
    const float w3d = (float)rhi.GetWidth() - x0;
    ViewInfo.Viewport = { x0, 0.0f, w3d, (float)rhi.GetHeight(), 0.0f, 1.0f };
    ViewInfo.Scissor = { (LONG)x0, 0, (LONG)rhi.GetWidth(), (LONG)rhi.GetHeight() };

    FRenderGraphBuilder graph(cmd);
    auto hdr = Renderer.HDRColor.Get();
    const D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv = Renderer.HDRRTV;

    graph.AddPass("ClearHDR", [=, this](ID3D12GraphicsCommandList* cl)
    {
        if (hdr && Renderer.HDRState != D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = hdr;
            b.Transition.StateBefore = Renderer.HDRState;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
            Renderer.HDRState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }

        cl->RSSetViewports(1, &ViewInfo.Viewport);
        cl->RSSetScissorRects(1, &ViewInfo.Scissor);
        cl->OMSetRenderTargets(1, &hdrRtv, FALSE, &Frame.DSV);

        const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
        cl->ClearRenderTargetView(hdrRtv, clearColor, 0, nullptr);
        cl->ClearDepthStencilView(Frame.DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    });

    const bool skyEnabled = (ViewFamily.Sky && ViewFamily.Sky->Enable);
    Renderer.AddSkyIBLPasses(graph, Frame, skyEnabled);
    Renderer.AddShadowPass(graph, Frame, objects, Inputs.PreviewPos, Inputs.PreviewType);
    Renderer.AddSkyPass(graph, Frame, ViewInfo.Viewport, ViewInfo.Scissor, hdrRtv, skyEnabled);

    RenderPath(graph, hdrRtv);

    if (hasSelection && Renderer.GizmoMapped)
    {
        // Gizmo vertices are in local-space around origin; world-aligned via gizmo CB (translation only).
        constexpr float L = 1.5f;
        uint32 gizmoVertCount = 0;

        if (!Inputs.bScaleGizmo)
        {
            constexpr float AH = 0.22f;   // arrowhead length
            constexpr float AW = 0.10f;   // arrowhead half-width

            const FVertex v[] = {
                // X axis (red)
                { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
                { { L,    0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
                { { L,    0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
                { { L-AH, +AW,  0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
                { { L,    0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
                { { L-AH, -AW,  0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },

                // Y axis (green)
                { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
                { { 0.0f, L,    0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
                { { 0.0f, L,    0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
                { { +AW,  L-AH, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
                { { 0.0f, L,    0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
                { { -AW,  L-AH, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },

                // Z axis (blue-ish)
                { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.5f, 1.0f }, { 0.0f, 1.0f, 0.0f } },
                { { 0.0f, 0.0f, L    }, { 0.0f, 0.5f, 1.0f }, { 0.0f, 1.0f, 0.0f } },
                { { 0.0f, 0.0f, L    }, { 0.0f, 0.5f, 1.0f }, { 0.0f, 1.0f, 0.0f } },
                { { 0.0f, +AW,  L-AH }, { 0.0f, 0.5f, 1.0f }, { 0.0f, 1.0f, 0.0f } },
                { { 0.0f, 0.0f, L    }, { 0.0f, 0.5f, 1.0f }, { 0.0f, 1.0f, 0.0f } },
                { { 0.0f, -AW,  L-AH }, { 0.0f, 0.5f, 1.0f }, { 0.0f, 1.0f, 0.0f } },
            };
            std::memcpy(Renderer.GizmoMapped, v, sizeof(v));
            gizmoVertCount = (uint32)_countof(v);
        }
        else
        {
            constexpr float B = 0.12f; // box half-size for scale handles
            FVertex v[64]{};
            uint32 count = 0;
            const auto pushLine = [&](float ax, float ay, float az, float bx, float by, float bz, float r, float g, float b)
            {
                v[count++] = { { ax, ay, az }, { r, g, b }, { 0.0f, 1.0f, 0.0f } };
                v[count++] = { { bx, by, bz }, { r, g, b }, { 0.0f, 1.0f, 0.0f } };
            };

            // X axis (red) + box at end (YZ plane)
            pushLine(0.0f, 0.0f, 0.0f, L, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
            pushLine(L,  B,  B, L,  B, -B, 1.0f, 0.0f, 0.0f);
            pushLine(L,  B, -B, L, -B, -B, 1.0f, 0.0f, 0.0f);
            pushLine(L, -B, -B, L, -B,  B, 1.0f, 0.0f, 0.0f);
            pushLine(L, -B,  B, L,  B,  B, 1.0f, 0.0f, 0.0f);

            // Y axis (green) + box at end (XZ plane)
            pushLine(0.0f, 0.0f, 0.0f, 0.0f, L, 0.0f, 0.0f, 1.0f, 0.0f);
            pushLine( B, L,  B,  B, L, -B, 0.0f, 1.0f, 0.0f);
            pushLine( B, L, -B, -B, L, -B, 0.0f, 1.0f, 0.0f);
            pushLine(-B, L, -B, -B, L,  B, 0.0f, 1.0f, 0.0f);
            pushLine(-B, L,  B,  B, L,  B, 0.0f, 1.0f, 0.0f);

            // Z axis (blue-ish) + box at end (XY plane)
            pushLine(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, L, 0.0f, 0.5f, 1.0f);
            pushLine( B,  B, L,  B, -B, L, 0.0f, 0.5f, 1.0f);
            pushLine( B, -B, L, -B, -B, L, 0.0f, 0.5f, 1.0f);
            pushLine(-B, -B, L, -B,  B, L, 0.0f, 0.5f, 1.0f);
            pushLine(-B,  B, L,  B,  B, L, 0.0f, 0.5f, 1.0f);

            std::memcpy(Renderer.GizmoMapped, v, sizeof(FVertex) * count);
            gizmoVertCount = count;
        }

        auto psoLines = Renderer.PSO_Lines.Get();
        auto rootSig = Renderer.RootSig.Get();
        auto gizmoVBV = Renderer.GizmoVBView;
        auto srvHeap = Renderer.SRVHeap.Get();
        const uint32 srvStride = Renderer.SRVDescriptorSize;
        D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvGpu{};
        if (srvHeap)
        {
            shadowSrvGpu = srvHeap->GetGPUDescriptorHandleForHeapStart();
            shadowSrvGpu.ptr += SIZE_T(FSimpleSceneRenderer::kShadowMapSRVSlot) * srvStride;
        }
        D3D12_GPU_VIRTUAL_ADDRESS cbAddrGizmo = Renderer.ConstantBufferGizmo[Frame.FrameIndex]->GetGPUVirtualAddress();
        D3D12_GPU_VIRTUAL_ADDRESS shadowCB = Renderer.ConstantBufferShadow[Frame.FrameIndex]->GetGPUVirtualAddress();

        graph.AddPass("DrawGizmo", [=, this](ID3D12GraphicsCommandList* cl)
        {
            cl->RSSetViewports(1, &ViewInfo.Viewport);
            cl->RSSetScissorRects(1, &ViewInfo.Scissor);
            cl->OMSetRenderTargets(1, &hdrRtv, FALSE, &Frame.DSV);

            cl->SetPipelineState(psoLines);
            cl->SetGraphicsRootSignature(rootSig);
            cl->SetGraphicsRootConstantBufferView(0, cbAddrGizmo);
            cl->SetGraphicsRootConstantBufferView(2, shadowCB);
            if (srvHeap)
            {
                ID3D12DescriptorHeap* heaps[] = { srvHeap };
                cl->SetDescriptorHeaps(1, heaps);
                D3D12_GPU_DESCRIPTOR_HANDLE gpu = srvHeap->GetGPUDescriptorHandleForHeapStart();
                cl->SetGraphicsRootDescriptorTable(1, gpu);
                cl->SetGraphicsRootDescriptorTable(3, shadowSrvGpu);
            }
            cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
            cl->IASetVertexBuffers(0, 1, &gizmoVBV);
            cl->DrawInstanced(gizmoVertCount, 1, 0, 0);
        });
    }

    Renderer.AddHDRToSRVPass(graph);
    Renderer.AddTonemapPass(graph, Frame, ViewInfo.Viewport, ViewInfo.Scissor);

    graph.Execute();
    rhi.EndFrame();

    Renderer.PrevViewProj = ViewInfo.ViewProj;
    Renderer.bLumenHistoryValid = (bUseLumen && !bUseHWRTGI && !bUseLumenSWRT);
    Renderer.bHWRTGIHistoryValid = (bUseHWRTGI && RtInstanceCount > 0);
    Renderer.bSWRTGIHistoryValid = (bUseLumenSWRT && SwrtObjectCount > 0);
}

void FForwardShadingSceneRenderer::RenderPath(FRenderGraphBuilder& graph, D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv)
{
    auto rootSig = Renderer.RootSig.Get();
    auto pso = Renderer.PSO.Get();
    const D3D12_GPU_VIRTUAL_ADDRESS cbBase = Renderer.ConstantBufferObjects[Frame.FrameIndex]->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS shadowCB = Renderer.ConstantBufferShadow[Frame.FrameIndex]->GetGPUVirtualAddress();
    auto srvHeap = Renderer.SRVHeap.Get();
    const uint32 srvStride = Renderer.SRVDescriptorSize;
    D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvGpu{};
    if (srvHeap)
    {
        shadowSrvGpu = srvHeap->GetGPUDescriptorHandleForHeapStart();
        shadowSrvGpu.ptr += SIZE_T(FSimpleSceneRenderer::kShadowMapSRVSlot) * srvStride;
    }

    graph.AddPass("DrawScene", [=, this](ID3D12GraphicsCommandList* cl)
    {
        cl->RSSetViewports(1, &ViewInfo.Viewport);
        cl->RSSetScissorRects(1, &ViewInfo.Scissor);
        cl->OMSetRenderTargets(1, &hdrRtv, FALSE, &Frame.DSV);

        cl->SetPipelineState(pso);
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

        const uint32 drawCount = (uint32)std::min<size_t>(Inputs.Objects->size(), FSimpleSceneRenderer::MaxObjects);
        for (uint32 i = 0; i < drawCount; ++i)
        {
            const auto& mesh = Renderer.GetMesh((*Inputs.Objects)[i].Type);
            cl->SetGraphicsRootConstantBufferView(0, cbBase + (UINT64)Renderer.CBSize * i);
            if (srvHeap)
            {
                D3D12_GPU_DESCRIPTOR_HANDLE gpu = srvHeap->GetGPUDescriptorHandleForHeapStart();
                const uint32 base = ((*Inputs.Objects)[i].MaterialSRVBase >= 0) ? (uint32)(*Inputs.Objects)[i].MaterialSRVBase : 0u;
                gpu.ptr += SIZE_T(std::min<uint32>(base, 2047u)) * srvStride;
                cl->SetGraphicsRootDescriptorTable(1, gpu);
            }
            cl->IASetVertexBuffers(0, 1, &mesh.VBView);
            cl->IASetIndexBuffer(&mesh.IBView);
            cl->DrawIndexedInstanced(mesh.IndexCount, 1, 0, 0, 0);
        }

        if (Inputs.PreviewPos && drawCount < FSimpleSceneRenderer::MaxObjects)
        {
            const auto& mesh = Renderer.GetMesh(Inputs.PreviewType);
            cl->SetGraphicsRootConstantBufferView(0, cbBase + (UINT64)Renderer.CBSize * drawCount);
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
}

void FDeferredShadingSceneRenderer::RenderPath(FRenderGraphBuilder& graph, D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv)
{
    Renderer.AddGBufferPasses(graph, Frame, ViewInfo.Viewport, ViewInfo.Scissor, *Inputs.Objects, Inputs.PreviewPos, Inputs.PreviewType);

    if (bUseHWRTGI && RtInstanceCount > 0)
        Renderer.AddBuildTLASPass(graph, Frame.FrameIndex, RtInstanceCount);

    Renderer.AddDeferredLightingPass(graph, Frame, ViewInfo.Viewport, ViewInfo.Scissor, hdrRtv);

    if (bUseHWRTGI && RtInstanceCount > 0)
        Renderer.AddHWRTGIPasses(graph, Frame, ViewInfo.Viewport, ViewInfo.Scissor, hdrRtv, RtInstanceCount);
    else if (bUseLumenSWRT && SwrtObjectCount > 0)
        Renderer.AddLumenSwrtPasses(graph, Frame, ViewInfo.Viewport, ViewInfo.Scissor, hdrRtv, SwrtObjectCount);
    else if (bUseLumen && Renderer.LumenPSO && Renderer.LumenRootSig && Renderer.LumenRTVHeap && Renderer.ConstantBufferLumen[Frame.FrameIndex])
        Renderer.AddLumenPass(graph, Frame, ViewInfo.Viewport, ViewInfo.Scissor, hdrRtv);
}
