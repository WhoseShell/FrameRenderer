#include "Renderer/SimpleSceneRenderer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Core/Diagnostics.h"
#include "Renderer/RenderGraph.h"

static std::wstring FindDxcExePath()
{
    static std::wstring cached;
    if (!cached.empty())
        return cached;

    wchar_t envPath[1024]{};
    const DWORD envLen = GetEnvironmentVariableW(L"DXC_PATH", envPath, (DWORD)std::size(envPath));
    if (envLen > 0 && envLen < (DWORD)std::size(envPath) && GetFileAttributesW(envPath) != INVALID_FILE_ATTRIBUTES)
    {
        cached = envPath;
        return cached;
    }

    const std::wstring base = L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\";
    const bool is32 = (sizeof(void*) == 4);
    const std::wstring primaryArch = is32 ? L"x86" : L"x64";
    const std::wstring fallbackArch = is32 ? L"x64" : L"x86";

    struct FVer
    {
        uint32 a = 0, b = 0, c = 0, d = 0;
    };

    auto parseVer = [](const wchar_t* s, FVer& out) -> bool
    {
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (swscanf_s(s, L"%u.%u.%u.%u", &a, &b, &c, &d) != 4)
            return false;
        out.a = a; out.b = b; out.c = c; out.d = d;
        return true;
    };

    auto lt = [](const FVer& x, const FVer& y) -> bool
    {
        if (x.a != y.a) return x.a < y.a;
        if (x.b != y.b) return x.b < y.b;
        if (x.c != y.c) return x.c < y.c;
        return x.d < y.d;
    };

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((base + L"10.0.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return {};

    FVer best{};
    bool found = false;

    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (fd.cFileName[0] == L'.')
            continue;

        FVer v{};
        if (!parseVer(fd.cFileName, v))
            continue;

        std::filesystem::path candidate = std::filesystem::path(base) / fd.cFileName / primaryArch / L"dxc.exe";
        if (!std::filesystem::exists(candidate))
        {
            candidate = std::filesystem::path(base) / fd.cFileName / fallbackArch / L"dxc.exe";
            if (!std::filesystem::exists(candidate))
                continue;
        }

        if (!found || lt(best, v))
        {
            best = v;
            cached = candidate.wstring();
            found = true;
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return cached;
}

static ComPtr<ID3DBlob> CompileShaderFromFileDXC(const std::wstring& file, const std::wstring& entry, const std::wstring& target)
{
    const std::wstring dxcPath = FindDxcExePath();
    if (dxcPath.empty())
        throw std::runtime_error("DXC not found. Set DXC_PATH or install Windows SDK (dxc.exe).");

    wchar_t tempDir[MAX_PATH]{};
    if (GetTempPathW((DWORD)std::size(tempDir), tempDir) == 0)
        throw std::runtime_error("GetTempPathW failed");

    wchar_t tempFile[MAX_PATH]{};
    if (GetTempFileNameW(tempDir, L"seh", 0, tempFile) == 0)
        throw std::runtime_error("GetTempFileNameW failed");

    const std::filesystem::path outPath = tempFile;
    const std::filesystem::path includeDir = std::filesystem::path(file).parent_path();

    std::wstring cmdLine = L"\"";
    cmdLine += dxcPath;
    cmdLine += L"\" -E \"";
    cmdLine += entry;
    cmdLine += L"\" -T \"";
    cmdLine += target;
    cmdLine += L"\" -Fo \"";
    cmdLine += outPath.wstring();
    cmdLine += L"\" -HV 2021 -I \"";
    cmdLine += includeDir.wstring();
    cmdLine += L"\"";

#if defined(_DEBUG)
    cmdLine += L" -Zi -Qembed_debug -Od";
#else
    cmdLine += L" -O3";
#endif

    cmdLine += L" \"";
    cmdLine += file;
    cmdLine += L"\"";

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        throw std::runtime_error("CreatePipe failed");
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdMutable(cmdLine.begin(), cmdLine.end());
    cmdMutable.push_back(L'\0');
    const BOOL ok = CreateProcessW(
        nullptr,
        cmdMutable.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi);

    CloseHandle(writePipe);

    std::string output;
    if (!ok)
    {
        const DWORD err = GetLastError();
        CloseHandle(readPipe);
        throw std::runtime_error(std::string("CreateProcessW(dxc) failed: ") + FormatWin32Error(err));
    }

    char buf[4096];
    for (;;)
    {
        DWORD bytesRead = 0;
        if (!ReadFile(readPipe, buf, (DWORD)std::size(buf), &bytesRead, nullptr) || bytesRead == 0)
            break;
        output.append(buf, buf + bytesRead);
    }
    CloseHandle(readPipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (!output.empty())
        DebugOutput(std::wstring(output.begin(), output.end()));

    if (exitCode != 0)
        throw std::runtime_error("DXC shader compilation failed");

    std::ifstream in(outPath, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("Failed to open DXC output file");

    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);

    ComPtr<ID3DBlob> blob;
    ThrowIfFailed(D3DCreateBlob((SIZE_T)size, &blob), "D3DCreateBlob failed");
    if (!in.read((char*)blob->GetBufferPointer(), size))
        throw std::runtime_error("Failed to read DXC output file");

    in.close();
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
    return blob;
}

void FSimpleSceneRenderer::InitHWRTGIPass(FD3D12RHI& rhi, const D3D12_BLEND_DESC& blend, const D3D12_RASTERIZER_DESC& rast)
{
    if (!bRaytracingSupported)
        return;

    ID3D12Device* device = rhi.GetDevice();
    if (!device) return;

    try
    {
        // Compute: HWRT GI + temporal (writes history/meta), and bilateral filter (writes filtered).
        {
        D3D12_ROOT_PARAMETER paramsHW[3]{};
        paramsHW[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        paramsHW[0].Descriptor.ShaderRegister = 0;
        paramsHW[0].Descriptor.RegisterSpace = 0;
        paramsHW[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = kDesc_SkySH + 1; // t0..t27
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = 0;

        paramsHW[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        paramsHW[1].DescriptorTable.NumDescriptorRanges = 1;
        paramsHW[1].DescriptorTable.pDescriptorRanges = &srvRange;
        paramsHW[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_DESCRIPTOR_RANGE uavRange{};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 7; // u0..u6
        uavRange.BaseShaderRegister = 0;
        uavRange.RegisterSpace = 0;
        uavRange.OffsetInDescriptorsFromTableStart = 0;

        paramsHW[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        paramsHW[2].DescriptorTable.NumDescriptorRanges = 1;
        paramsHW[2].DescriptorTable.pDescriptorRanges = &uavRange;
        paramsHW[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC sampHW{};
        sampHW.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampHW.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampHW.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampHW.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampHW.MipLODBias = 0.0f;
        sampHW.MaxAnisotropy = 1;
        sampHW.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampHW.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        sampHW.MinLOD = 0.0f;
        sampHW.MaxLOD = D3D12_FLOAT32_MAX;
        sampHW.ShaderRegister = 0;
        sampHW.RegisterSpace = 0;
        sampHW.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = _countof(paramsHW);
        rs.pParameters = paramsHW;
        rs.NumStaticSamplers = 1;
        rs.pStaticSamplers = &sampHW;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> sig, err;
        HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (err)
        {
            std::string e((const char*)err->GetBufferPointer(), err->GetBufferSize());
            DebugOutput(std::wstring(e.begin(), e.end()));
        }
        ThrowIfFailed(hr, "D3D12SerializeRootSignature (hwrt gi) failed");
        ThrowIfFailed(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&HWRTGIRootSig)),
                      "CreateRootSignature (hwrt gi) failed");

        std::wstring hwrtPath = std::wstring(SHADER_DIR) + L"/lumen_hwrt.hlsl";
        ComPtr<ID3DBlob> csGI = CompileShaderFromFileDXC(hwrtPath, L"CSHWRTGI", L"cs_6_5");
        ComPtr<ID3DBlob> csFilter = CompileShaderFromFileDXC(hwrtPath, L"CSFilter", L"cs_6_5");

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = HWRTGIRootSig.Get();
        pso.CS = { csGI->GetBufferPointer(), csGI->GetBufferSize() };
        ThrowIfFailed(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&HWRTGIPSO)), "CreateComputePipelineState (hwrt gi) failed");
        pso.CS = { csFilter->GetBufferPointer(), csFilter->GetBufferSize() };
        ThrowIfFailed(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&HWRTGIFilterPSO)), "CreateComputePipelineState (hwrt gi filter) failed");
        }

        // Additive composite (filtered GI -> HDR)
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
            ThrowIfFailed(hr, "D3D12SerializeRootSignature (hwrt gi add) failed");
            ThrowIfFailed(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&HWRTGIAddRootSig)),
                          "CreateRootSignature (hwrt gi add) failed");

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
            pso.pRootSignature = HWRTGIAddRootSig.Get();
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
            ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&HWRTGIAddPSO)), "CreateGraphicsPipelineState (hwrt gi add) failed");
        }

        bHWRTGIReady = true;
    }
    catch (const std::exception& e)
    {
        std::string msg = std::string("HWRT GI init failed: ") + e.what();
        DebugOutput(std::wstring(msg.begin(), msg.end()));

        HWRTGIInitError = std::wstring(msg.begin(), msg.end());
        bHWRTGIReady = false;

        HWRTGIRootSig.Reset();
        HWRTGIPSO.Reset();
        HWRTGIFilterPSO.Reset();
        HWRTGIAddRootSig.Reset();
        HWRTGIAddPSO.Reset();
    }
}

uint32 FSimpleSceneRenderer::UpdateHWRTGICBAndScene(
    FD3D12RHI& rhi,
    bool bUseHWRTGI,
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
    if (!bUseHWRTGI || !CBMappedHWRTGI[frameIndex] || !GBufferSRVHeap)
        return 0;

    const int writeIdx = int(frameIndex) & 1;
    const int prevIdx = 1 - writeIdx;

    const uint32 drawCount = (uint32)std::min<size_t>(objects.size(), MaxObjects);
    const bool bHasPreview = (previewPos && drawCount < MaxObjects);
    const uint32 instanceCount = drawCount + (bHasPreview ? 1u : 0u);

    FHWRTGICB hcb{};
    hcb.ViewProj = curViewProj;
    hcb.PrevViewProj = (bHWRTGIHistoryValid ? PrevViewProj : curViewProj);
    hcb.CameraPosWs = cameraPosWs;
    hcb.TemporalWeight = 0.92f;
    hcb.LightDirWs = lightDirWs;
    hcb.MaxTraceDistance = 30.0f;
    hcb.LightColor = { 1.0f, 0.98f, 0.92f };
    hcb.LightIntensity = sunIntensity;
    hcb.InvFullResolution = { 1.0f / std::max(1.0f, (float)rhi.GetWidth()), 1.0f / std::max(1.0f, (float)rhi.GetHeight()) };
    hcb.InvGIResolution = { 1.0f / std::max(1.0f, (float)HWRTGIWidth), 1.0f / std::max(1.0f, (float)HWRTGIHeight) };
    hcb.FrameIndex = timeSeconds * 60.0f;
    hcb.FrameParity = (float)(frameIndex & 1);
    hcb.PrevHistoryIndex = (float)prevIdx;
    hcb.HistoryValid = bHWRTGIHistoryValid ? 1.0f : 0.0f;
    hcb.GIIntensity = 1.0f;
    hcb.DepthReject = 0.25f;
    hcb.NormalReject = 0.35f;
    hcb.RaysPerPixel = 1.0f;
    hcb.ObjectCount = (float)instanceCount;
    std::memcpy(CBMappedHWRTGI[frameIndex], &hcb, sizeof(hcb));

    const uint32 rtInstanceCount = PrepareRaytracingScene(rhi, frameIndex, objects, previewPos, previewType);

    // Update per-frame TLAS + object buffer SRVs in the deferred SRV heap.
    ID3D12Device* device = rhi.GetDevice();
    if (device && rtInstanceCount > 0 && GBufferSRVDescriptorSize > 0)
    {
        const uint32 tlasIndex = (frameIndex & 1) ? kDesc_TLAS1 : kDesc_TLAS0;
        const uint32 objIndex = (frameIndex & 1) ? kDesc_RTObjects1 : kDesc_RTObjects0;

        if (RTFrame[frameIndex].TLAS)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.RaytracingAccelerationStructure.Location = RTFrame[frameIndex].TLAS->GetGPUVirtualAddress();

            D3D12_CPU_DESCRIPTOR_HANDLE cpu = GBufferSRVHeap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += SIZE_T(tlasIndex) * GBufferSRVDescriptorSize;
            device->CreateShaderResourceView(nullptr, &srv, cpu);
        }

        if (RTObjectBuffer[frameIndex])
        {
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
    }

    return rtInstanceCount;
}

void FSimpleSceneRenderer::AddBuildTLASPass(FRenderGraphBuilder& graph, uint32 frameIndex, uint32 instanceCount)
{
    graph.AddPass("BuildTLAS", [=, this](ID3D12GraphicsCommandList* cl)
    {
        RecordBuildTLAS(cl, frameIndex, instanceCount);
    });
}

void FSimpleSceneRenderer::AddHWRTGIPasses(
    FRenderGraphBuilder& graph,
    const FD3D12FrameContext& frame,
    const D3D12_VIEWPORT& vp,
    const D3D12_RECT& sc,
    D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv,
    uint32 instanceCount)
{
    if (instanceCount == 0 || !HWRTGIPSO || !HWRTGIFilterPSO || !HWRTGIAddPSO || !HWRTGIRootSig || !HWRTGIAddRootSig ||
        !ConstantBufferHWRTGI[frame.FrameIndex] || !HWRTGIHistory[0] || !HWRTGIHistory[1] || !HWRTGIMeta[0] || !HWRTGIMeta[1] || !HWRTGIFiltered)
        return;

    const int writeIdx = int(frame.FrameIndex) & 1;
    ID3D12Resource* giHistWrite = HWRTGIHistory[writeIdx].Get();
    ID3D12Resource* giMetaWrite = HWRTGIMeta[writeIdx].Get();
    ID3D12Resource* giFiltered = HWRTGIFiltered.Get();
    const uint32 giW = HWRTGIWidth;
    const uint32 giH = HWRTGIHeight;

    auto hwSig = HWRTGIRootSig.Get();
    auto hwPso = HWRTGIPSO.Get();
    auto hwFilterPso = HWRTGIFilterPSO.Get();
    auto hwAddSig = HWRTGIAddRootSig.Get();
    auto hwAddPso = HWRTGIAddPSO.Get();
    const D3D12_GPU_VIRTUAL_ADDRESS hwCB = ConstantBufferHWRTGI[frame.FrameIndex]->GetGPUVirtualAddress();
    auto gbufSrvHeap = GBufferSRVHeap.Get();
    const D3D12_GPU_DESCRIPTOR_HANDLE gbufSrvBase = GBufferSRVBaseGPU;

    graph.AddPass("HWRTGI", [=, this](ID3D12GraphicsCommandList* cl)
    {
        if (!giHistWrite || !giMetaWrite) return;

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

        cl->SetPipelineState(hwPso);
        cl->SetComputeRootSignature(hwSig);
        if (gbufSrvHeap)
        {
            ID3D12DescriptorHeap* heaps[] = { gbufSrvHeap };
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetComputeRootDescriptorTable(1, gbufSrvBase);
            D3D12_GPU_DESCRIPTOR_HANDLE uavBase = gbufSrvBase;
            uavBase.ptr += SIZE_T(kDescUAVBase) * GBufferSRVDescriptorSize;
            cl->SetComputeRootDescriptorTable(2, uavBase);
        }
        cl->SetComputeRootConstantBufferView(0, hwCB);
        cl->Dispatch((giW + 7u) / 8u, (giH + 7u) / 8u, 1u);

        D3D12_RESOURCE_BARRIER uav[2]{};
        uav[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav[0].UAV.pResource = giHistWrite;
        uav[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav[1].UAV.pResource = giMetaWrite;
        cl->ResourceBarrier(2, uav);
    });

    graph.AddPass("HWRTGIToSRV", [=, this](ID3D12GraphicsCommandList* cl)
    {
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

    graph.AddPass("HWRTGIFilter", [=, this](ID3D12GraphicsCommandList* cl)
    {
        if (!giFiltered) return;
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

        cl->SetPipelineState(hwFilterPso);
        cl->SetComputeRootSignature(hwSig);
        if (gbufSrvHeap)
        {
            ID3D12DescriptorHeap* heaps[] = { gbufSrvHeap };
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetComputeRootDescriptorTable(1, gbufSrvBase);
            D3D12_GPU_DESCRIPTOR_HANDLE uavBase = gbufSrvBase;
            uavBase.ptr += SIZE_T(kDescUAVBase) * GBufferSRVDescriptorSize;
            cl->SetComputeRootDescriptorTable(2, uavBase);
        }
        cl->SetComputeRootConstantBufferView(0, hwCB);
        cl->Dispatch((giW + 7u) / 8u, (giH + 7u) / 8u, 1u);

        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = giFiltered;
        cl->ResourceBarrier(1, &uav);
    });

    graph.AddPass("HWRTGIFilterToSRV", [=, this](ID3D12GraphicsCommandList* cl)
    {
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

    graph.AddPass("HWRTGIAdd", [=](ID3D12GraphicsCommandList* cl)
    {
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        cl->OMSetRenderTargets(1, &hdrRtv, FALSE, nullptr);

        cl->SetPipelineState(hwAddPso);
        cl->SetGraphicsRootSignature(hwAddSig);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        if (gbufSrvHeap)
        {
            ID3D12DescriptorHeap* heaps[] = { gbufSrvHeap };
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetGraphicsRootDescriptorTable(1, gbufSrvBase);
        }
        cl->SetGraphicsRootConstantBufferView(0, hwCB);
        cl->DrawInstanced(3, 1, 0, 0);
    });
}
