#include "Renderer/SimpleSceneRenderer.h"

#include <algorithm>
#include <cstring>

#include "Core/Diagnostics.h"
#include "Renderer/RenderGraph.h"

void FSimpleSceneRenderer::InitSkyIBLPass(FD3D12RHI& rhi)
{
    ID3D12Device* device = rhi.GetDevice();
    if (!device) return;

    D3D12_ROOT_PARAMETER params[5]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].Descriptor.RegisterSpace = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1; // sky cubemap
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &srvRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE uavRange0{};
    uavRange0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange0.NumDescriptors = 1; // output texture
    uavRange0.BaseShaderRegister = 0;
    uavRange0.RegisterSpace = 0;
    uavRange0.OffsetInDescriptorsFromTableStart = 0;

    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &uavRange0;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE uavRange1{};
    uavRange1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange1.NumDescriptors = 1; // SH buffer
    uavRange1.BaseShaderRegister = 1;
    uavRange1.RegisterSpace = 0;
    uavRange1.OffsetInDescriptorsFromTableStart = 0;

    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 1;
    params[4].DescriptorTable.pDescriptorRanges = &uavRange1;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

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
    ThrowIfFailed(hr, "D3D12SerializeRootSignature (sky ibl) failed");
    ThrowIfFailed(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&SkyIBLRootSig)),
                  "CreateRootSignature (sky ibl) failed");

    std::wstring skyPath = std::wstring(SHADER_DIR) + L"/sky_ibl.hlsl";
    auto csGen = CompileShaderFromFile(skyPath, "CSGenerateSkyCube", "cs_5_0");
    auto csSH = CompileShaderFromFile(skyPath, "CSComputeSkySH", "cs_5_0");
    auto csPrefilter = CompileShaderFromFile(skyPath, "CSPrefilterSky", "cs_5_0");

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = SkyIBLRootSig.Get();
    pso.CS = { csGen->GetBufferPointer(), csGen->GetBufferSize() };
    ThrowIfFailed(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&SkyIBLGenPSO)), "CreateComputePipelineState (sky ibl gen) failed");
    pso.CS = { csSH->GetBufferPointer(), csSH->GetBufferSize() };
    ThrowIfFailed(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&SkyIBLSHPSO)), "CreateComputePipelineState (sky ibl sh) failed");
    pso.CS = { csPrefilter->GetBufferPointer(), csPrefilter->GetBufferSize() };
    ThrowIfFailed(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&SkyIBLPrefilterPSO)), "CreateComputePipelineState (sky ibl prefilter) failed");
}

void FSimpleSceneRenderer::EnsureSkyIBLTargets(FD3D12RHI& rhi)
{
    ID3D12Device* device = rhi.GetDevice();
    if (!device || !SRVHeap)
        return;

    const uint32 size = kSkyIBLSize;
    const uint32 mipCount = kSkyIBLMipCount;

    if (!SkyCube || !SkyPrefilter || !SkySH)
    {
        SkyCube.Reset();
        SkyPrefilter.Reset();
        SkySH.Reset();

        D3D12_HEAP_PROPERTIES heapDefault{};
        heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC cubeDesc{};
        cubeDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        cubeDesc.Width = size;
        cubeDesc.Height = size;
        cubeDesc.DepthOrArraySize = 6;
        cubeDesc.MipLevels = 1;
        cubeDesc.Format = SkyIBLFormat;
        cubeDesc.SampleDesc.Count = 1;
        cubeDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        cubeDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ThrowIfFailed(device->CreateCommittedResource(
            &heapDefault,
            D3D12_HEAP_FLAG_NONE,
            &cubeDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&SkyCube)), "CreateCommittedResource SkyCube failed");

        D3D12_RESOURCE_DESC preDesc = cubeDesc;
        preDesc.MipLevels = (uint16)mipCount;
        ThrowIfFailed(device->CreateCommittedResource(
            &heapDefault,
            D3D12_HEAP_FLAG_NONE,
            &preDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&SkyPrefilter)), "CreateCommittedResource SkyPrefilter failed");

        D3D12_RESOURCE_DESC shDesc{};
        shDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        shDesc.Width = UINT64(sizeof(float) * 4) * 9;
        shDesc.Height = 1;
        shDesc.DepthOrArraySize = 1;
        shDesc.MipLevels = 1;
        shDesc.SampleDesc.Count = 1;
        shDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        shDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ThrowIfFailed(device->CreateCommittedResource(
            &heapDefault,
            D3D12_HEAP_FLAG_NONE,
            &shDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&SkySH)), "CreateCommittedResource SkySH failed");

        SkyCubeState = D3D12_RESOURCE_STATE_GENERIC_READ;
        SkyPrefilterState = D3D12_RESOURCE_STATE_GENERIC_READ;
        SkySHState = D3D12_RESOURCE_STATE_GENERIC_READ;
    }

    if (!SkyIBLHeap)
    {
        const uint32 heapCount = 3 + mipCount;
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = heapCount;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&SkyIBLHeap)), "CreateDescriptorHeap (sky ibl) failed");
        SkyIBLDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = SkyIBLHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = SkyIBLHeap->GetGPUDescriptorHandleForHeapStart();

        // t0: sky cube SRV
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = SkyIBLFormat;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            srv.TextureCube.MipLevels = 1;
            device->CreateShaderResourceView(SkyCube.Get(), &srv, cpu);
            SkyIBLCubeSrvGPU = gpu;
        }

        // u0: sky cube UAV
        cpu.ptr += SIZE_T(SkyIBLDescriptorSize);
        gpu.ptr += SIZE_T(SkyIBLDescriptorSize);
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = SkyIBLFormat;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uav.Texture2DArray.MipSlice = 0;
            uav.Texture2DArray.FirstArraySlice = 0;
            uav.Texture2DArray.ArraySize = 6;
            device->CreateUnorderedAccessView(SkyCube.Get(), nullptr, &uav, cpu);
            SkyIBLCubeUavGPU = gpu;
        }

        // u1: SH buffer UAV
        cpu.ptr += SIZE_T(SkyIBLDescriptorSize);
        gpu.ptr += SIZE_T(SkyIBLDescriptorSize);
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = DXGI_FORMAT_UNKNOWN;
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.FirstElement = 0;
            uav.Buffer.NumElements = 9;
            uav.Buffer.StructureByteStride = sizeof(float) * 4;
            uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
            device->CreateUnorderedAccessView(SkySH.Get(), nullptr, &uav, cpu);
            SkyIBLSHUavGPU = gpu;
        }

        SkyIBLPrefilterUavGPU.resize(mipCount);
        for (uint32 mip = 0; mip < mipCount; ++mip)
        {
            cpu.ptr += SIZE_T(SkyIBLDescriptorSize);
            gpu.ptr += SIZE_T(SkyIBLDescriptorSize);

            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = SkyIBLFormat;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uav.Texture2DArray.MipSlice = (UINT)mip;
            uav.Texture2DArray.FirstArraySlice = 0;
            uav.Texture2DArray.ArraySize = 6;
            device->CreateUnorderedAccessView(SkyPrefilter.Get(), nullptr, &uav, cpu);
            SkyIBLPrefilterUavGPU[mip] = gpu;
        }
    }

    // Forward SRV heap: shadow map table also carries sky IBL.
    if (SkyPrefilter && SRVHeap && SRVDescriptorSize > 0)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = SkyIBLFormat;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MipLevels = kSkyIBLMipCount;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = SRVHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(kSkyPrefilterSRVSlot) * SRVDescriptorSize;
        device->CreateShaderResourceView(SkyPrefilter.Get(), &srv, cpu);
    }

    if (SkySH && SRVHeap && SRVDescriptorSize > 0)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Buffer.FirstElement = 0;
        srv.Buffer.NumElements = 9;
        srv.Buffer.StructureByteStride = sizeof(float) * 4;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = SRVHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(kSkySHSRVSlot) * SRVDescriptorSize;
        device->CreateShaderResourceView(SkySH.Get(), &srv, cpu);
    }

    // Deferred SRV heap: create sky IBL entries if available.
    if (GBufferSRVHeap && GBufferSRVDescriptorSize > 0 && SkyPrefilter)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = SkyIBLFormat;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MipLevels = kSkyIBLMipCount;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = GBufferSRVHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(kDesc_SkyPrefilter) * GBufferSRVDescriptorSize;
        device->CreateShaderResourceView(SkyPrefilter.Get(), &srv, cpu);
    }

    if (GBufferSRVHeap && GBufferSRVDescriptorSize > 0 && SkySH)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Buffer.FirstElement = 0;
        srv.Buffer.NumElements = 9;
        srv.Buffer.StructureByteStride = sizeof(float) * 4;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = GBufferSRVHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(kDesc_SkySH) * GBufferSRVDescriptorSize;
        device->CreateShaderResourceView(SkySH.Get(), &srv, cpu);
    }
}

void FSimpleSceneRenderer::AddSkyIBLPasses(
    FRenderGraphBuilder& graph,
    const FD3D12FrameContext& frame,
    bool enable)
{
    (void)enable;

    if (!SkyIBLRootSig || !SkyIBLGenPSO || !SkyIBLSHPSO || !SkyIBLPrefilterPSO ||
        !SkyCube || !SkyPrefilter || !SkySH || !SkyIBLHeap ||
        !ConstantBufferSky[frame.FrameIndex] || !ConstantBufferSkyIBL[frame.FrameIndex] || !CBMappedSkyIBL[frame.FrameIndex])
        return;

    const uint32 size = kSkyIBLSize;
    const uint32 mipCount = kSkyIBLMipCount;
    const float maxMip = (mipCount > 0) ? float(mipCount - 1) : 0.0f;

    auto skySig = SkyIBLRootSig.Get();
    auto psoGen = SkyIBLGenPSO.Get();
    auto psoSH = SkyIBLSHPSO.Get();
    auto psoPrefilter = SkyIBLPrefilterPSO.Get();
    const D3D12_GPU_VIRTUAL_ADDRESS skyCB = ConstantBufferSky[frame.FrameIndex]->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS skyIblCBBase = ConstantBufferSkyIBL[frame.FrameIndex]->GetGPUVirtualAddress();
    uint8* cbMapped = CBMappedSkyIBL[frame.FrameIndex];
    const uint32 cbStride = SkyIBLCBSize;

    auto heap = SkyIBLHeap.Get();
    const D3D12_GPU_DESCRIPTOR_HANDLE skySrv = SkyIBLCubeSrvGPU;
    const D3D12_GPU_DESCRIPTOR_HANDLE skyUav = SkyIBLCubeUavGPU;
    const D3D12_GPU_DESCRIPTOR_HANDLE shUav = SkyIBLSHUavGPU;

    const uint32 cbIndexGen = 0;
    graph.AddPass("SkyIBLGen", [=, this](ID3D12GraphicsCommandList* cl)
    {
        if (SkyCubeState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = SkyCube.Get();
            b.Transition.StateBefore = SkyCubeState;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
            SkyCubeState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        FSkyIBLCB cb{};
        cb.MaxMip = maxMip;
        std::memcpy(cbMapped + size_t(cbIndexGen) * cbStride, &cb, sizeof(cb));

        cl->SetPipelineState(psoGen);
        cl->SetComputeRootSignature(skySig);
        if (heap)
        {
            ID3D12DescriptorHeap* heaps[] = { heap };
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetComputeRootDescriptorTable(2, skySrv);
            cl->SetComputeRootDescriptorTable(3, skyUav);
            cl->SetComputeRootDescriptorTable(4, shUav);
        }
        cl->SetComputeRootConstantBufferView(0, skyCB);
        cl->SetComputeRootConstantBufferView(1, skyIblCBBase + UINT64(cbIndexGen) * cbStride);
        cl->Dispatch((size + 7u) / 8u, (size + 7u) / 8u, 6u);

        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = SkyCube.Get();
        cl->ResourceBarrier(1, &uav);

        if (SkyCubeState != D3D12_RESOURCE_STATE_GENERIC_READ)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = SkyCube.Get();
            b.Transition.StateBefore = SkyCubeState;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
            SkyCubeState = D3D12_RESOURCE_STATE_GENERIC_READ;
        }
    });

    const uint32 cbIndexSH = 1;
    graph.AddPass("SkyIBLSH", [=, this](ID3D12GraphicsCommandList* cl)
    {
        if (SkySHState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = SkySH.Get();
            b.Transition.StateBefore = SkySHState;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
            SkySHState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        FSkyIBLCB cb{};
        cb.MaxMip = maxMip;
        std::memcpy(cbMapped + size_t(cbIndexSH) * cbStride, &cb, sizeof(cb));

        cl->SetPipelineState(psoSH);
        cl->SetComputeRootSignature(skySig);
        if (heap)
        {
            ID3D12DescriptorHeap* heaps[] = { heap };
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetComputeRootDescriptorTable(2, skySrv);
            cl->SetComputeRootDescriptorTable(3, skyUav);
            cl->SetComputeRootDescriptorTable(4, shUav);
        }
        cl->SetComputeRootConstantBufferView(0, skyCB);
        cl->SetComputeRootConstantBufferView(1, skyIblCBBase + UINT64(cbIndexSH) * cbStride);
        cl->Dispatch(1u, 1u, 1u);

        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = SkySH.Get();
        cl->ResourceBarrier(1, &uav);

        if (SkySHState != D3D12_RESOURCE_STATE_GENERIC_READ)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = SkySH.Get();
            b.Transition.StateBefore = SkySHState;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
            SkySHState = D3D12_RESOURCE_STATE_GENERIC_READ;
        }
    });

    for (uint32 mip = 0; mip < mipCount; ++mip)
    {
        const D3D12_GPU_DESCRIPTOR_HANDLE mipUav = SkyIBLPrefilterUavGPU[mip];
        const uint32 cbIndex = 2 + mip;
        const uint32 mipSize = std::max(1u, size >> mip);
        const float roughness = (maxMip > 0.0f) ? (float(mip) / maxMip) : 0.0f;

        graph.AddPass("SkyIBLPrefilter", [=, this](ID3D12GraphicsCommandList* cl)
        {
            if (SkyPrefilterState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = SkyPrefilter.Get();
                b.Transition.StateBefore = SkyPrefilterState;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                cl->ResourceBarrier(1, &b);
                SkyPrefilterState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }

            FSkyIBLCB cb{};
            cb.Roughness = roughness;
            cb.MipLevel = (float)mip;
            cb.MaxMip = maxMip;
            std::memcpy(cbMapped + size_t(cbIndex) * cbStride, &cb, sizeof(cb));

            cl->SetPipelineState(psoPrefilter);
            cl->SetComputeRootSignature(skySig);
            if (heap)
            {
                ID3D12DescriptorHeap* heaps[] = { heap };
                cl->SetDescriptorHeaps(1, heaps);
                cl->SetComputeRootDescriptorTable(2, skySrv);
                cl->SetComputeRootDescriptorTable(3, mipUav);
                cl->SetComputeRootDescriptorTable(4, shUav);
            }
            cl->SetComputeRootConstantBufferView(0, skyCB);
            cl->SetComputeRootConstantBufferView(1, skyIblCBBase + UINT64(cbIndex) * cbStride);
            cl->Dispatch((mipSize + 7u) / 8u, (mipSize + 7u) / 8u, 6u);

            D3D12_RESOURCE_BARRIER uav{};
            uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav.UAV.pResource = SkyPrefilter.Get();
            cl->ResourceBarrier(1, &uav);
        });
    }

    graph.AddPass("SkyIBLPrefilterToSRV", [=, this](ID3D12GraphicsCommandList* cl)
    {
        if (SkyPrefilterState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = SkyPrefilter.Get();
            b.Transition.StateBefore = SkyPrefilterState;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
            SkyPrefilterState = D3D12_RESOURCE_STATE_GENERIC_READ;
        }
    });
}
