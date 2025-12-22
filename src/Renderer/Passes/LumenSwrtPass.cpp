#include "Renderer/SimpleSceneRenderer.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "Core/Diagnostics.h"
#include "Renderer/RenderGraph.h"

/**
 * @brief 初始化 Lumen SWRT 通道（计算/合成 PSO）。
 * @param rhi 渲染硬件接口。
 * @param blend 混合状态描述。
 * @param rast 光栅化状态描述。
 * @return 无返回值。
 * @note 阶段：SWRT GI 通道初始化阶段。
 */
void FSimpleSceneRenderer::InitLumenSwrtPass(FD3D12RHI& rhi, const D3D12_BLEND_DESC& blend, const D3D12_RASTERIZER_DESC& rast)
{
    ID3D12Device* device = rhi.GetDevice();
    if (!device)
        return;

    // Compute root signature: b0 + SRV table (t0..tN) + UAV table (u0..u6)
    // 计算通道 RootSignature：CBV + SRV + UAV。
    {
        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].Descriptor.RegisterSpace = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = kDesc_SkySH + 1; // t0..t27
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = 0;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srvRange;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_DESCRIPTOR_RANGE uavRange{};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = (kUav_LumenSWRTSurface1 - kDescUAVBase + 1); // u0..u6
        uavRange.BaseShaderRegister = 0;
        uavRange.RegisterSpace = 0;
        uavRange.OffsetInDescriptorsFromTableStart = 0;

        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &uavRange;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.MipLODBias = 0.0f;
        samp.MaxAnisotropy = 1;
        samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samp.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        samp.MinLOD = 0.0f;
        samp.MaxLOD = D3D12_FLOAT32_MAX;
        samp.ShaderRegister = 0;
        samp.RegisterSpace = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = _countof(params);
        rs.pParameters = params;
        rs.NumStaticSamplers = 1;
        rs.pStaticSamplers = &samp;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> sig, err;
        HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (err)
        {
            std::string e((const char*)err->GetBufferPointer(), err->GetBufferSize());
            DebugOutput(std::wstring(e.begin(), e.end()));
        }
        ThrowIfFailed(hr, "D3D12SerializeRootSignature (lumen swrt) failed");
        ThrowIfFailed(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&LumenSwrtRootSig)),
                      "CreateRootSignature (lumen swrt) failed");
    }

    // Compute PSOs: surface cache, GI, filter.
    // 计算 PSO：表面缓存、GI、滤波。
    {
        std::wstring swrtPath = std::wstring(SHADER_DIR) + L"/lumen_swrt.hlsl";
        auto csSurface = CompileShaderFromFile(swrtPath, "CSSWRTSurfaceCache", "cs_5_0");
        auto csGI = CompileShaderFromFile(swrtPath, "CSSWRTGI", "cs_5_0");
        auto csFilter = CompileShaderFromFile(swrtPath, "CSSWRTFilter", "cs_5_0");

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = LumenSwrtRootSig.Get();
        pso.CS = { csSurface->GetBufferPointer(), csSurface->GetBufferSize() };
        ThrowIfFailed(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&LumenSwrtSurfacePSO)), "CreateComputePipelineState (lumen swrt surface) failed");

        pso.CS = { csGI->GetBufferPointer(), csGI->GetBufferSize() };
        ThrowIfFailed(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&LumenSwrtGIPSO)), "CreateComputePipelineState (lumen swrt gi) failed");

        pso.CS = { csFilter->GetBufferPointer(), csFilter->GetBufferSize() };
        ThrowIfFailed(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&LumenSwrtFilterPSO)), "CreateComputePipelineState (lumen swrt filter) failed");
    }

    // Additive composite (filtered GI -> HDR)
    // 合成 PSO：将滤波后的 GI 叠加到 HDR。
    {
        D3D12_ROOT_PARAMETER paramsAdd[2]{};
        paramsAdd[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        paramsAdd[0].Descriptor.ShaderRegister = 0;
        paramsAdd[0].Descriptor.RegisterSpace = 0;
        paramsAdd[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = kDesc_SkySH + 1; // t0..t27
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = 0;

        paramsAdd[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        paramsAdd[1].DescriptorTable.NumDescriptorRanges = 1;
        paramsAdd[1].DescriptorTable.pDescriptorRanges = &srvRange;
        paramsAdd[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampAdd{};
        sampAdd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampAdd.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampAdd.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampAdd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampAdd.MipLODBias = 0.0f;
        sampAdd.MaxAnisotropy = 1;
        sampAdd.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampAdd.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        sampAdd.MinLOD = 0.0f;
        sampAdd.MaxLOD = D3D12_FLOAT32_MAX;
        sampAdd.ShaderRegister = 0;
        sampAdd.RegisterSpace = 0;
        sampAdd.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = _countof(paramsAdd);
        rs.pParameters = paramsAdd;
        rs.NumStaticSamplers = 1;
        rs.pStaticSamplers = &sampAdd;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> sig, err;
        HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (err)
        {
            std::string e((const char*)err->GetBufferPointer(), err->GetBufferSize());
            DebugOutput(std::wstring(e.begin(), e.end()));
        }
        ThrowIfFailed(hr, "D3D12SerializeRootSignature (lumen swrt add) failed");
        ThrowIfFailed(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&LumenSwrtAddRootSig)),
                      "CreateRootSignature (lumen swrt add) failed");

        std::wstring addPath = std::wstring(SHADER_DIR) + L"/hwrt_gi_add.hlsl";
        auto vsAdd = CompileShaderFromFile(addPath, "VSFullscreen", "vs_5_0");
        auto psAdd = CompileShaderFromFile(addPath, "PSAddGI", "ps_5_0");

        D3D12_BLEND_DESC blendAdd = blend;
        blendAdd.RenderTarget[0].BlendEnable = TRUE;
        blendAdd.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        blendAdd.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        blendAdd.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        blendAdd.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        blendAdd.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        blendAdd.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = LumenSwrtAddRootSig.Get();
        pso.VS = { vsAdd->GetBufferPointer(), vsAdd->GetBufferSize() };
        pso.PS = { psAdd->GetBufferPointer(), psAdd->GetBufferSize() };
        pso.BlendState = blendAdd;
        pso.RasterizerState = rast;
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DepthStencilState.StencilEnable = FALSE;
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = HDRFormat;
        pso.SampleDesc.Count = 1;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&LumenSwrtAddPSO)), "CreateGraphicsPipelineState (lumen swrt add) failed");
    }
}

/**
 * @brief 更新 SWRT GI 常量缓冲并准备对象数据。
 * @param rhi 渲染硬件接口。
 * @param bUseSWRT 是否启用 SWRT。
 * @param curViewProj 当前视图投影矩阵。
 * @param cameraPosWs 相机世界位置。
 * @param lightDirWs 光源方向。
 * @param sunIntensity 太阳强度。
 * @param timeSeconds 当前时间（秒）。
 * @param frameIndex 帧索引。
 * @param objects 场景对象列表。
 * @param previewPos 预览位置（可空）。
 * @param previewType 预览对象类型。
 * @return 对象实例数量。
 * @note 阶段：SWRT GI 参数更新阶段。
 */
uint32 FSimpleSceneRenderer::UpdateSWRTGICBAndObjects(
    FD3D12RHI& rhi,
    bool bUseSWRT,
    const DirectX::XMFLOAT4X4& curViewProj,
    const DirectX::XMFLOAT3& cameraPosWs,
    const DirectX::XMFLOAT3& lightDirWs,
    float sunIntensity,
    float timeSeconds,
    uint32 frameIndex,
    const std::vector<FSceneObject>& objects,
    const DirectX::XMFLOAT3* previewPos,
    FSceneObject::EType previewType)
{
    if (!bUseSWRT || !CBMappedHWRTGI[frameIndex] || !GBufferSRVHeap)
        return 0;

    ID3D12Device* device = rhi.GetDevice();
    if (!device)
        return 0;

    const int prevIdx = 1 - (int(frameIndex) & 1);

    const uint32 drawCount = (uint32)std::min<size_t>(objects.size(), MaxObjects);
    const bool bHasPreview = (previewPos && drawCount < MaxObjects);
    const uint32 instanceCount = drawCount + (bHasPreview ? 1u : 0u);
    if (instanceCount == 0)
        return 0;

    // 填充 GI 常量缓冲。
    FHWRTGICB hcb{};
    hcb.ViewProj = curViewProj;
    hcb.PrevViewProj = (bSWRTGIHistoryValid ? PrevViewProj : curViewProj);
    hcb.CameraPosWs = cameraPosWs;
    hcb.TemporalWeight = 0.94f;
    hcb.LightDirWs = lightDirWs;
    hcb.MaxTraceDistance = 20.0f;
    hcb.LightColor = { 1.0f, 0.98f, 0.92f };
    hcb.LightIntensity = sunIntensity;
    hcb.InvFullResolution = { 1.0f / std::max(1.0f, (float)rhi.GetWidth()), 1.0f / std::max(1.0f, (float)rhi.GetHeight()) };
    hcb.InvGIResolution = { 1.0f / std::max(1.0f, (float)LumenSwrtWidth), 1.0f / std::max(1.0f, (float)LumenSwrtHeight) };
    hcb.FrameIndex = timeSeconds * 60.0f;
    hcb.FrameParity = (float)(frameIndex & 1);
    hcb.PrevHistoryIndex = (float)prevIdx;
    hcb.HistoryValid = bSWRTGIHistoryValid ? 1.0f : 0.0f;
    hcb.GIIntensity = 1.0f;
    hcb.DepthReject = 0.25f;
    hcb.NormalReject = 0.35f;
    hcb.RaysPerPixel = 1.0f;
    hcb.ObjectCount = (float)instanceCount;
    std::memcpy(CBMappedHWRTGI[frameIndex], &hcb, sizeof(hcb));

    // Ensure per-frame object data buffer (upload, persistently mapped).
    // 确保对象数据缓冲存在。
    {
        const uint32 capacity = MaxObjects + 1;
        const UINT64 bytes = UINT64(sizeof(FRTObjectData)) * UINT64(capacity);
        if (!RTObjectBuffer[frameIndex])
        {
            D3D12_HEAP_PROPERTIES heapUpload{};
            heapUpload.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = bytes;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            ThrowIfFailed(device->CreateCommittedResource(
                &heapUpload,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&RTObjectBuffer[frameIndex])),
                "CreateCommittedResource SWRT RTObjectBuffer failed");

            void* mapped = nullptr;
            D3D12_RANGE readRange{ 0, 0 };
            ThrowIfFailed(RTObjectBuffer[frameIndex]->Map(0, &readRange, &mapped), "SWRT RTObjectBuffer Map failed");
            RTObjectMapped[frameIndex] = static_cast<uint8*>(mapped);
            std::memset(RTObjectMapped[frameIndex], 0, (size_t)bytes);
        }
    }

    auto meshTypeToIndex = [](FSceneObject::EType t) -> uint32
    {
        switch (t)
        {
        case FSceneObject::EType::Sphere: return 0;
        case FSceneObject::EType::Box: return 1;
        case FSceneObject::EType::Cone: return 2;
        default: return 0;
        }
    };

    auto writeObject = [&](uint32 instanceIndex, const FSceneObject& obj)
    {
        // 写入对象数据用于 SWRT 追踪。
        const uint32 meshType = meshTypeToIndex(obj.Type);

        FRTObjectData o{};
        o.Albedo = obj.Albedo;
        o.Metallic = obj.Metallic;
        o.Roughness = obj.Roughness;
        o.AO = 1.0f;
        o.MeshType = meshType;
        o.Position = obj.Position;
        o.Radius = obj.Radius;
        o.Scale = obj.Scale;
        std::memcpy(RTObjectMapped[frameIndex] + size_t(instanceIndex) * sizeof(FRTObjectData), &o, sizeof(o));
    };

    for (uint32 i = 0; i < drawCount; ++i)
        writeObject(i, objects[i]);

    if (bHasPreview)
    {
        // 追加预览对象。
        FSceneObject preview{};
        preview.Type = previewType;
        preview.Position = *previewPos;
        preview.Albedo = { 0.35f, 0.9f, 0.35f };
        preview.Metallic = 0.0f;
        preview.Roughness = 0.6f;
        writeObject(drawCount, preview);
    }

    RTObjectInstanceCount[frameIndex] = instanceCount;

    // Update per-frame object buffer SRV in the deferred SRV heap.
    // 更新对象数据 SRV 到延迟堆。
    if (device && RTObjectBuffer[frameIndex] && GBufferSRVDescriptorSize > 0)
    {
        const uint32 objIndex = (frameIndex & 1) ? kDesc_RTObjects1 : kDesc_RTObjects0;

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Buffer.FirstElement = 0;
        srv.Buffer.NumElements = MaxObjects + 1;
        srv.Buffer.StructureByteStride = sizeof(FRTObjectData);
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = GBufferSRVHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(objIndex) * GBufferSRVDescriptorSize;
        device->CreateShaderResourceView(RTObjectBuffer[frameIndex].Get(), &srv, cpu);
    }

    return instanceCount;
}

/**
 * @brief 添加 SWRT GI 的一系列 Pass（Surface/GI/Filter/Add）。
 * @param graph 渲染图构建器。
 * @param frame 当前帧上下文。
 * @param vp 视口。
 * @param sc 裁剪区域。
 * @param hdrRtv HDR 目标 RTV。
 * @param objectCount 对象数量。
 * @return 无返回值。
 * @note 阶段：SWRT GI 渲染阶段。
 */
void FSimpleSceneRenderer::AddLumenSwrtPasses(
    FRenderGraphBuilder& graph,
    const FD3D12FrameContext& frame,
    const D3D12_VIEWPORT& vp,
    const D3D12_RECT& sc,
    D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv,
    uint32 objectCount)
{
    if (objectCount == 0 || !LumenSwrtRootSig || !LumenSwrtSurfacePSO || !LumenSwrtGIPSO || !LumenSwrtFilterPSO ||
        !LumenSwrtAddRootSig || !LumenSwrtAddPSO || !ConstantBufferHWRTGI[frame.FrameIndex] ||
        !LumenSwrtSurface[0] || !LumenSwrtSurface[1] || !HWRTGIHistory[0] || !HWRTGIHistory[1] || !HWRTGIMeta[0] ||
        !HWRTGIMeta[1] || !HWRTGIFiltered)
        return;

    const int writeIdx = int(frame.FrameIndex) & 1;
    ID3D12Resource* giHistWrite = HWRTGIHistory[writeIdx].Get();
    ID3D12Resource* giMetaWrite = HWRTGIMeta[writeIdx].Get();
    ID3D12Resource* giFiltered = HWRTGIFiltered.Get();
    ID3D12Resource* surf0 = LumenSwrtSurface[0].Get();
    ID3D12Resource* surf1 = LumenSwrtSurface[1].Get();
    const uint32 giW = LumenSwrtWidth;
    const uint32 giH = LumenSwrtHeight;

    auto swrtSig = LumenSwrtRootSig.Get();
    auto swrtSurfacePso = LumenSwrtSurfacePSO.Get();
    auto swrtGiPso = LumenSwrtGIPSO.Get();
    auto swrtFilterPso = LumenSwrtFilterPSO.Get();
    auto swrtAddSig = LumenSwrtAddRootSig.Get();
    auto swrtAddPso = LumenSwrtAddPSO.Get();
    const D3D12_GPU_VIRTUAL_ADDRESS swrtCB = ConstantBufferHWRTGI[frame.FrameIndex]->GetGPUVirtualAddress();
    auto gbufSrvHeap = GBufferSRVHeap.Get();
    const D3D12_GPU_DESCRIPTOR_HANDLE gbufSrvBase = GBufferSRVBaseGPU;

    graph.AddPass("LumenSWRTSurface", [=, this](ID3D12GraphicsCommandList* cl)
    {
        if (!surf0 || !surf1) return;

        // Surface 缓存写入（UAV）。
        ID3D12Resource* surfRes[2] = { surf0, surf1 };
        for (int i = 0; i < 2; ++i)
        {
            if (LumenSwrtSurfaceStates[i] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = surfRes[i];
                b.Transition.StateBefore = LumenSwrtSurfaceStates[i];
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                cl->ResourceBarrier(1, &b);
                LumenSwrtSurfaceStates[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
        }

        cl->SetPipelineState(swrtSurfacePso);
        cl->SetComputeRootSignature(swrtSig);
        if (gbufSrvHeap)
        {
            ID3D12DescriptorHeap* heaps[] = { gbufSrvHeap };
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetComputeRootDescriptorTable(1, gbufSrvBase);
            D3D12_GPU_DESCRIPTOR_HANDLE uavBase = gbufSrvBase;
            uavBase.ptr += SIZE_T(kDescUAVBase) * GBufferSRVDescriptorSize;
            cl->SetComputeRootDescriptorTable(2, uavBase);
        }
        cl->SetComputeRootConstantBufferView(0, swrtCB);
        cl->Dispatch((giW + 7u) / 8u, (giH + 7u) / 8u, 1u);

        D3D12_RESOURCE_BARRIER uav[2]{};
        uav[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav[0].UAV.pResource = surf0;
        uav[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav[1].UAV.pResource = surf1;
        cl->ResourceBarrier(2, uav);
    });

    graph.AddPass("LumenSWRTSurfaceToSRV", [=, this](ID3D12GraphicsCommandList* cl)
    {
        // Surface 缓存切回 SRV。
        if (!surf0 || !surf1) return;
        ID3D12Resource* surfRes[2] = { surf0, surf1 };
        for (int i = 0; i < 2; ++i)
        {
            if (LumenSwrtSurfaceStates[i] == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = surfRes[i];
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                cl->ResourceBarrier(1, &b);
                LumenSwrtSurfaceStates[i] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            }
        }
    });

    graph.AddPass("LumenSWRTGI", [=, this](ID3D12GraphicsCommandList* cl)
    {
        if (!giHistWrite || !giMetaWrite) return;

        // GI 计算写入历史与元数据。
        if (HWRTGIHistoryStates[writeIdx] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = giHistWrite;
            b.Transition.StateBefore = HWRTGIHistoryStates[writeIdx];
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
            HWRTGIHistoryStates[writeIdx] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        if (HWRTGIMetaStates[writeIdx] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = giMetaWrite;
            b.Transition.StateBefore = HWRTGIMetaStates[writeIdx];
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
            HWRTGIMetaStates[writeIdx] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        cl->SetPipelineState(swrtGiPso);
        cl->SetComputeRootSignature(swrtSig);
        if (gbufSrvHeap)
        {
            ID3D12DescriptorHeap* heaps[] = { gbufSrvHeap };
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetComputeRootDescriptorTable(1, gbufSrvBase);
            D3D12_GPU_DESCRIPTOR_HANDLE uavBase = gbufSrvBase;
            uavBase.ptr += SIZE_T(kDescUAVBase) * GBufferSRVDescriptorSize;
            cl->SetComputeRootDescriptorTable(2, uavBase);
        }
        cl->SetComputeRootConstantBufferView(0, swrtCB);
        cl->Dispatch((giW + 7u) / 8u, (giH + 7u) / 8u, 1u);

        D3D12_RESOURCE_BARRIER uav[2]{};
        uav[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav[0].UAV.pResource = giHistWrite;
        uav[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav[1].UAV.pResource = giMetaWrite;
        cl->ResourceBarrier(2, uav);
    });

    graph.AddPass("LumenSWRTGIToSRV", [=, this](ID3D12GraphicsCommandList* cl)
    {
        // GI 历史与元数据切回 SRV。
        if (!giHistWrite || !giMetaWrite) return;
        if (HWRTGIHistoryStates[writeIdx] == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = giHistWrite;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
            HWRTGIHistoryStates[writeIdx] = D3D12_RESOURCE_STATE_GENERIC_READ;
        }
        if (HWRTGIMetaStates[writeIdx] == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = giMetaWrite;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
            HWRTGIMetaStates[writeIdx] = D3D12_RESOURCE_STATE_GENERIC_READ;
        }
    });

    graph.AddPass("LumenSWRTFilter", [=, this](ID3D12GraphicsCommandList* cl)
    {
        if (!giFiltered) return;
        // 对 GI 结果做滤波。
        if (HWRTGIFilteredState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = giFiltered;
            b.Transition.StateBefore = HWRTGIFilteredState;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
            HWRTGIFilteredState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        cl->SetPipelineState(swrtFilterPso);
        cl->SetComputeRootSignature(swrtSig);
        if (gbufSrvHeap)
        {
            ID3D12DescriptorHeap* heaps[] = { gbufSrvHeap };
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetComputeRootDescriptorTable(1, gbufSrvBase);
            D3D12_GPU_DESCRIPTOR_HANDLE uavBase = gbufSrvBase;
            uavBase.ptr += SIZE_T(kDescUAVBase) * GBufferSRVDescriptorSize;
            cl->SetComputeRootDescriptorTable(2, uavBase);
        }
        cl->SetComputeRootConstantBufferView(0, swrtCB);
        cl->Dispatch((giW + 7u) / 8u, (giH + 7u) / 8u, 1u);

        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = giFiltered;
        cl->ResourceBarrier(1, &uav);
    });

    graph.AddPass("LumenSWRTFilterToSRV", [=, this](ID3D12GraphicsCommandList* cl)
    {
        // 过滤结果切回 SRV。
        if (!giFiltered) return;
        if (HWRTGIFilteredState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = giFiltered;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
            HWRTGIFilteredState = D3D12_RESOURCE_STATE_GENERIC_READ;
        }
    });

    graph.AddPass("LumenSWRTAdd", [=](ID3D12GraphicsCommandList* cl)
    {
        // 将 GI 结果叠加到 HDR。
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        cl->OMSetRenderTargets(1, &hdrRtv, FALSE, nullptr);

        cl->SetPipelineState(swrtAddPso);
        cl->SetGraphicsRootSignature(swrtAddSig);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        if (gbufSrvHeap)
        {
            ID3D12DescriptorHeap* heaps[] = { gbufSrvHeap };
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetGraphicsRootDescriptorTable(1, gbufSrvBase);
        }
        cl->SetGraphicsRootConstantBufferView(0, swrtCB);
        cl->DrawInstanced(3, 1, 0, 0);
    });
}
