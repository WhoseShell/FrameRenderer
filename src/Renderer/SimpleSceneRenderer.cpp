#include "Renderer/SimpleSceneRenderer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include <d3d12sdklayers.h>

#include "Core/Diagnostics.h"
#include "Core/Win32.h"
#include "RenderDoc/DdsLoader.h"
#include "Renderer/RenderGraph.h"
#include "Renderer/SceneRenderer.h"

#ifndef SHADER_DIR
#define SHADER_DIR L"."
#endif

#ifndef ASSET_DIR
#define ASSET_DIR L"assets"
#endif

namespace
{
/**
 * @brief 获取当前可执行文件所在目录。
 * @param 无。
 * @return 可执行文件目录路径；失败返回空路径。
 * @note 阶段：资源路径解析阶段。
 */
std::filesystem::path GetExecutableDir()
{
    wchar_t pathBuf[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, pathBuf, _countof(pathBuf));
    if (len == 0 || len >= _countof(pathBuf))
        return {};
    return std::filesystem::path(pathBuf).parent_path();
}

/**
 * @brief 将路径转换为绝对路径（失败则保持原样）。
 * @param path 输入路径。
 * @return 绝对路径或原始路径。
 * @note 阶段：资源路径解析阶段。
 */
std::filesystem::path MakeAbsoluteOrSame(const std::filesystem::path& path)
{
    std::error_code ec;
    auto abs = std::filesystem::absolute(path, ec);
    return ec ? path : abs;
}

/**
 * @brief 解析 shader 文件路径（支持多目录查找）。
 * @param file 输入文件名或路径。
 * @return 解析后的路径（可能仍为原始路径）。
 * @note 阶段：Shader 资源加载阶段。
 */
std::filesystem::path ResolveShaderPath(const std::filesystem::path& file)
{
    std::error_code ec;
    if (!file.empty() && std::filesystem::exists(file, ec))
        return MakeAbsoluteOrSame(file);

    const auto fileName = file.filename();
    if (fileName.empty())
        return file;

    const std::filesystem::path shaderDir = SHADER_DIR;
    if (!shaderDir.empty())
    {
        auto candidate = shaderDir / fileName;
        if (std::filesystem::exists(candidate, ec))
            return MakeAbsoluteOrSame(candidate);
    }

    const auto cwd = std::filesystem::current_path(ec);
    if (!cwd.empty())
    {
        auto candidate = cwd / L"shaders" / fileName;
        if (std::filesystem::exists(candidate, ec))
            return MakeAbsoluteOrSame(candidate);
    }

    const auto exeDir = GetExecutableDir();
    if (!exeDir.empty())
    {
        auto candidate = exeDir / L"shaders" / fileName;
        if (std::filesystem::exists(candidate, ec))
            return MakeAbsoluteOrSame(candidate);

        auto walk = exeDir;
        for (int i = 0; i < 5; ++i)
        {
            auto parent = walk.parent_path();
            if (parent.empty() || parent == walk)
                break;
            candidate = parent / L"shaders" / fileName;
            if (std::filesystem::exists(candidate, ec))
                return MakeAbsoluteOrSame(candidate);
            walk = parent;
        }
    }

    return file;
}

/**
 * @brief 将宽字符串转换为 UTF-8。
 * @param s 输入宽字符串。
 * @return UTF-8 字符串（不可转换字符会回退为 '?'）。
 * @note 阶段：日志/错误输出阶段。
 */
std::string NarrowUtf8(const std::wstring& s)
{
    if (s.empty())
        return {};

    const int needed = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
    {
        std::string fallback;
        fallback.reserve(s.size());
        for (wchar_t c : s)
            fallback.push_back((c >= 0 && c <= 0x7F) ? static_cast<char>(c) : '?');
        return fallback;
    }

    std::string out(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), needed, nullptr, nullptr);
    return out;
}

D3D12_RESOURCE_DESC MakeUploadBufferDesc(UINT64 byteSize)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (std::max<UINT64>)(1, byteSize);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

D3D12_RESOURCE_DESC MakeDdsTextureDesc(const rdcimport::FDdsImage& image)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = image.Dimension;
    desc.Width = image.Width;
    desc.Height = image.Height;
    desc.DepthOrArraySize = static_cast<UINT16>(
        image.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? image.Depth : image.ArraySize);
    desc.MipLevels = static_cast<UINT16>(image.MipCount);
    desc.Format = image.Format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return desc;
}

D3D12_SHADER_RESOURCE_VIEW_DESC MakeDdsSRVDesc(const rdcimport::FDdsImage& image)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = image.Format;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    if (image.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv.Texture3D.MipLevels = image.MipCount;
    }
    else if (image.ArraySize > 1)
    {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Texture2DArray.ArraySize = image.ArraySize;
        srv.Texture2DArray.MipLevels = image.MipCount;
    }
    else
    {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = image.MipCount;
    }
    return srv;
}

class FShaderInclude final : public ID3DInclude
{
public:
    /**
     * @brief 构造包含处理器，设置基础目录与附加搜索目录。
     * @param baseDir 基础包含目录。
     * @param extraDirs 额外搜索目录列表。
     * @return 无返回值（构造函数）。
     * @note 阶段：Shader 编译阶段。
     */
    FShaderInclude(const std::filesystem::path& baseDir, std::vector<std::filesystem::path> extraDirs)
        : BaseDir(baseDir), ExtraDirs(std::move(extraDirs))
    {
    }

    /**
     * @brief 打开 include 文件并返回其内容。
     * @param pFileName include 文件名。
     * @param pParentData 父级 include 数据指针。
     * @param ppData 输出文件内容缓冲区指针。
     * @param pBytes 输出内容字节数。
     * @return HRESULT 状态码。
     * @note 阶段：Shader 编译阶段。
     */
    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR pFileName, LPCVOID pParentData, LPCVOID* ppData, UINT* pBytes) override
    {
        if (!pFileName || !ppData || !pBytes)
            return E_INVALIDARG;

        std::filesystem::path requested = std::filesystem::path(pFileName);
        std::filesystem::path found;

        std::vector<std::filesystem::path> dirs;
        auto addDir = [&](const std::filesystem::path& dir)
        {
            if (!dir.empty())
                dirs.push_back(dir);
        };

        if (pParentData)
        {
            auto it = ParentDirs.find(pParentData);
            if (it != ParentDirs.end())
                addDir(it->second);
        }
        addDir(BaseDir);
        for (const auto& d : ExtraDirs)
            addDir(d);

        std::error_code ec;
        auto tryPath = [&](const std::filesystem::path& p) -> bool
        {
            if (p.empty())
                return false;
            if (std::filesystem::exists(p, ec))
            {
                found = p;
                return true;
            }
            return false;
        };

        if (requested.is_absolute())
            tryPath(requested);
        else
        {
            for (const auto& d : dirs)
            {
                if (tryPath(d / requested))
                    break;
            }
        }

        if (found.empty())
            return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);

        std::ifstream in(found, std::ios::binary | std::ios::ate);
        if (!in)
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

        const std::streamsize size = in.tellg();
        if (size < 0)
            return E_FAIL;
        in.seekg(0, std::ios::beg);

        char* data = new char[(size_t)size];
        if (size > 0 && !in.read(data, size))
        {
            delete[] data;
            return E_FAIL;
        }

        *ppData = data;
        *pBytes = (UINT)size;
        ParentDirs[data] = found.parent_path();
        return S_OK;
    }

    /**
     * @brief 关闭 include 文件并释放内存。
     * @param pData Open 返回的数据指针。
     * @return HRESULT 状态码。
     * @note 阶段：Shader 编译阶段。
     */
    HRESULT STDMETHODCALLTYPE Close(LPCVOID pData) override
    {
        ParentDirs.erase(pData);
        delete[] reinterpret_cast<const char*>(pData);
        return S_OK;
    }

private:
    std::filesystem::path BaseDir;
    std::vector<std::filesystem::path> ExtraDirs;
    std::unordered_map<const void*, std::filesystem::path> ParentDirs;
};
} // namespace

/**
 * @brief 将字节大小对齐到 256 字节。
 * @param size 输入字节数。
 * @return 256 字节对齐后的大小。
 * @note 阶段：常量缓冲/上传资源对齐阶段。
 */
uint32 FSimpleSceneRenderer::Align256(uint32 size)
{
    return (size + 255u) & ~255u;
}

/**
 * @brief 创建并上传 RGBA8 纹理到 GPU。
 * @param rhi 渲染硬件接口。
 * @param width 纹理宽度。
 * @param height 纹理高度。
 * @param rgba RGBA8 数据指针。
 * @return 纹理槽位（0 表示失败或白色默认纹理）。
 * @note 阶段：资源导入/上传阶段。
 */
int FSimpleSceneRenderer::CreateTextureRGBA8(FD3D12RHI& rhi, uint32 width, uint32 height, const uint8* rgba)
{
    if (!SRVHeap || !rgba || width == 0 || height == 0) return 0;
    // Reserve first 512 descriptors for textures.
    if ((uint32)Textures.size() >= 512) return 0;

    ID3D12Device* device = rhi.GetDevice();

    FTextureGPU tex{};
    tex.Width = width;
    tex.Height = height;

    // 创建默认堆上的纹理资源。
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES heapDefault{};
    heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

    ThrowIfFailed(device->CreateCommittedResource(
        &heapDefault,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&tex.Resource)), "CreateCommittedResource texture failed");

    // 创建上传缓冲并计算拷贝布局。
    // Upload buffer
    UINT64 uploadSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSize = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &numRows, &rowSize, &uploadSize);

    ComPtr<ID3D12Resource> upload;
    D3D12_HEAP_PROPERTIES heapUpload{};
    heapUpload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buf{};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width = uploadSize;
    buf.Height = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels = 1;
    buf.SampleDesc.Count = 1;
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(device->CreateCommittedResource(&heapUpload, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)),
                  "CreateCommittedResource upload failed");

    // 将 RGBA 数据按行拷贝到上传缓冲。
    // Copy rgba into upload with row pitch
    {
        uint8* mapped = nullptr;
        D3D12_RANGE rr{ 0,0 };
        ThrowIfFailed(upload->Map(0, &rr, reinterpret_cast<void**>(&mapped)), "Upload map failed");
        const uint32 srcPitch = width * 4;
        uint8* dst = mapped + footprint.Offset;
        for (uint32 y = 0; y < height; ++y)
            std::memcpy(dst + y * footprint.Footprint.RowPitch, rgba + y * srcPitch, srcPitch);
        upload->Unmap(0, nullptr);
    }

    struct FUploadCtx
    {
        ID3D12Resource* Dst = nullptr;
        ID3D12Resource* Upload = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
        uint32 Width = 0;
        uint32 Height = 0;
    } ctx;
    ctx.Dst = tex.Resource.Get();
    ctx.Upload = upload.Get();
    ctx.Footprint = footprint;
    ctx.Width = width;
    ctx.Height = height;

    // 录制一次性拷贝并转换资源状态。
    auto record = [](ID3D12GraphicsCommandList* cmd, void* user)
    {
        auto* c = reinterpret_cast<FUploadCtx*>(user);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = c->Dst;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = c->Upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = c->Footprint;

        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = c->Dst;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
    };

    rhi.ExecuteImmediate(record, &ctx);

    const int slot = (int)Textures.size();
    Textures.push_back(tex);

    // 创建 SRV。
    // Create SRV at slot
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = SRVHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(slot) * SRVDescriptorSize;
    device->CreateShaderResourceView(tex.Resource.Get(), &srv, cpu);

    return slot;
}

int FSimpleSceneRenderer::CreateTextureDDS(FD3D12RHI& rhi, const std::filesystem::path& path)
{
    if (!SRVHeap || path.empty())
        return 0;
    if ((uint32)Textures.size() >= 512)
        return 0;

    ID3D12Device* device = rhi.GetDevice();
    if (!device)
        return 0;

    try
    {
        const rdcimport::FDdsImage image = rdcimport::LoadDdsImage(path);
        if (image.Format == DXGI_FORMAT_UNKNOWN || image.Subresources.empty())
            return 0;

        const D3D12_RESOURCE_DESC desc = MakeDdsTextureDesc(image);
        FTextureGPU tex{};
        tex.Width = image.Width;
        tex.Height = image.Height;

        D3D12_HEAP_PROPERTIES heapDefault{};
        heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;
        ThrowIfFailed(device->CreateCommittedResource(
            &heapDefault,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&tex.Resource)), "CreateCommittedResource DDS texture failed");

        const UINT subresourceCount = static_cast<UINT>(image.Subresources.size());
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresourceCount);
        std::vector<UINT> rowCounts(subresourceCount);
        std::vector<UINT64> rowSizes(subresourceCount);
        UINT64 uploadSize = 0;
        device->GetCopyableFootprints(
            &desc,
            0,
            subresourceCount,
            0,
            layouts.data(),
            rowCounts.data(),
            rowSizes.data(),
            &uploadSize);

        ComPtr<ID3D12Resource> upload;
        D3D12_HEAP_PROPERTIES heapUpload{};
        heapUpload.Type = D3D12_HEAP_TYPE_UPLOAD;
        const D3D12_RESOURCE_DESC uploadDesc = MakeUploadBufferDesc(uploadSize);
        ThrowIfFailed(device->CreateCommittedResource(
            &heapUpload,
            D3D12_HEAP_FLAG_NONE,
            &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&upload)), "CreateCommittedResource DDS upload failed");

        uint8* mapped = nullptr;
        D3D12_RANGE rr{ 0, 0 };
        ThrowIfFailed(upload->Map(0, &rr, reinterpret_cast<void**>(&mapped)), "DDS upload map failed");
        for (UINT i = 0; i < subresourceCount; ++i)
        {
            const rdcimport::FDdsSubresource& src = image.Subresources[i];
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout = layouts[i];
            uint8* dstSliceBase = mapped + layout.Offset;
            const uint8* srcSliceBase = reinterpret_cast<const uint8*>(src.Data.data());
            const UINT depth = (std::max)(1u, src.Depth);
            const UINT rowCount = rowCounts[i];
            const UINT64 rowSize = rowSizes[i];
            const UINT64 dstSlicePitch = static_cast<UINT64>(layout.Footprint.RowPitch) * rowCount;
            for (UINT z = 0; z < depth; ++z)
            {
                uint8* dstRow = dstSliceBase + z * dstSlicePitch;
                const uint8* srcRow = srcSliceBase + static_cast<size_t>(z) * src.SlicePitch;
                for (UINT y = 0; y < rowCount; ++y)
                {
                    std::memcpy(
                        dstRow + static_cast<size_t>(y) * layout.Footprint.RowPitch,
                        srcRow + static_cast<size_t>(y) * src.RowPitch,
                        static_cast<size_t>(rowSize));
                }
            }
        }
        upload->Unmap(0, nullptr);

        struct FUploadCtx
        {
            ID3D12Resource* Texture = nullptr;
            ID3D12Resource* Upload = nullptr;
            const std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>* Layouts = nullptr;
            UINT SubresourceCount = 0;
        } ctx;
        ctx.Texture = tex.Resource.Get();
        ctx.Upload = upload.Get();
        ctx.Layouts = &layouts;
        ctx.SubresourceCount = subresourceCount;

        rhi.ExecuteImmediate([](ID3D12GraphicsCommandList* cmd, void* user)
        {
            auto* c = reinterpret_cast<FUploadCtx*>(user);
            for (UINT i = 0; i < c->SubresourceCount; ++i)
            {
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = c->Texture;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = i;

                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = c->Upload;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = (*c->Layouts)[i];
                cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }

            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = c->Texture;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmd->ResourceBarrier(1, &barrier);
        }, &ctx);

        const int slot = (int)Textures.size();
        Textures.push_back(std::move(tex));

        const D3D12_SHADER_RESOURCE_VIEW_DESC srv = MakeDdsSRVDesc(image);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = SRVHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(slot) * SRVDescriptorSize;
        device->CreateShaderResourceView(Textures.back().Resource.Get(), &srv, cpu);
        return slot;
    }
    catch (const std::exception&)
    {
        DebugOutput(L"DDS load failed.");
        return 0;
    }
}

int FSimpleSceneRenderer::CreateStaticMesh(FD3D12RHI& rhi, const std::vector<FVertex>& vertices, const std::vector<uint32>& indices)
{
    if (vertices.empty() || indices.empty() || !rhi.GetDevice())
        return -1;

    ID3D12Device* device = rhi.GetDevice();
    FMeshGPU mesh{};
    const uint32 vbSize = (uint32)(vertices.size() * sizeof(FVertex));
    const uint32 ibSize = (uint32)(indices.size() * sizeof(uint32));

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC vbDesc{};
    vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vbDesc.Width = vbSize;
    vbDesc.Height = 1;
    vbDesc.DepthOrArraySize = 1;
    vbDesc.MipLevels = 1;
    vbDesc.SampleDesc.Count = 1;
    vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &vbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mesh.VertexBuffer)),
        "CreateCommittedResource static mesh vb failed");

    {
        void* mapped = nullptr;
        D3D12_RANGE readRange{ 0, 0 };
        ThrowIfFailed(mesh.VertexBuffer->Map(0, &readRange, &mapped), "Static mesh VB Map failed");
        std::memcpy(mapped, vertices.data(), vbSize);
        mesh.VertexBuffer->Unmap(0, nullptr);
    }

    D3D12_RESOURCE_DESC ibDesc = vbDesc;
    ibDesc.Width = ibSize;
    ThrowIfFailed(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &ibDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mesh.IndexBuffer)),
        "CreateCommittedResource static mesh ib failed");

    {
        void* mapped = nullptr;
        D3D12_RANGE readRange{ 0, 0 };
        ThrowIfFailed(mesh.IndexBuffer->Map(0, &readRange, &mapped), "Static mesh IB Map failed");
        std::memcpy(mapped, indices.data(), ibSize);
        mesh.IndexBuffer->Unmap(0, nullptr);
    }

    mesh.VBView.BufferLocation = mesh.VertexBuffer->GetGPUVirtualAddress();
    mesh.VBView.SizeInBytes = vbSize;
    mesh.VBView.StrideInBytes = sizeof(FVertex);
    mesh.IBView.BufferLocation = mesh.IndexBuffer->GetGPUVirtualAddress();
    mesh.IBView.SizeInBytes = ibSize;
    mesh.IBView.Format = DXGI_FORMAT_R32_UINT;
    mesh.IndexCount = (uint32)indices.size();

    const int meshIndex = (int)StaticMeshes.size();
    StaticMeshes.push_back(mesh);
    return meshIndex;
}

/**
 * @brief 分配材质 SRV 块（5 个连续槽位）。
 * @param 无。
 * @return 起始槽位索引。
 * @note 阶段：材质资源管理阶段。
 */
int FSimpleSceneRenderer::AllocateMaterialSRVBlock()
{
    if (!SRVHeap) return 0;
    // Reserve first 512 descriptors for textures, remaining for material blocks.
    static constexpr int kTextureRegionSize = 512;
    static constexpr int kBlockSize = 5;
    static constexpr int kMaxSlot = kShadowMapSRVSlot;
    static int nextBase = kTextureRegionSize;

    if (nextBase + kBlockSize >= kMaxSlot) return 0;
    const int base = nextBase;
    nextBase += kBlockSize;
    return base;
}

/**
 * @brief 更新材质 SRV 块的纹理槽位。
 * @param base SRV 块起始槽位。
 * @param albedoSlot Albedo 纹理槽位。
 * @param normalSlot Normal 纹理槽位。
 * @param roughnessSlot Roughness 纹理槽位。
 * @param metallicSlot Metallic 纹理槽位。
 * @param aoSlot AO 纹理槽位。
 * @return 无返回值。
 * @note 阶段：材质更新阶段。
 */
void FSimpleSceneRenderer::UpdateMaterialSRVBlock(int base, int albedoSlot, int normalSlot, int roughnessSlot, int metallicSlot, int aoSlot)
{
    if (!SRVHeap) return;
    ID3D12Device* device = nullptr;
    SRVHeap->GetDevice(IID_PPV_ARGS(&device));
    if (!device) return;

    // 将非法槽位夹取到默认纹理。
    auto clampSlot = [](int s) { return (s < 0) ? 0 : (s > 511 ? 0 : s); };
    albedoSlot = clampSlot(albedoSlot);
    normalSlot = clampSlot(normalSlot);
    roughnessSlot = clampSlot(roughnessSlot);
    metallicSlot = clampSlot(metallicSlot);
    aoSlot = clampSlot(aoSlot);

    const int slots[5] = { albedoSlot, normalSlot, roughnessSlot, metallicSlot, aoSlot };
    for (int i = 0; i < 5; ++i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE src = SRVHeap->GetCPUDescriptorHandleForHeapStart();
        src.ptr += SIZE_T(slots[i]) * SRVDescriptorSize;
        D3D12_CPU_DESCRIPTOR_HANDLE dst = SRVHeap->GetCPUDescriptorHandleForHeapStart();
        dst.ptr += SIZE_T(base + i) * SRVDescriptorSize;
        device->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    device->Release();
}

/**
 * @brief 根据物体类型获取对应网格。
 * @param type 物体类型。
 * @return 对应的 GPU 网格引用。
 * @note 阶段：渲染绘制阶段。
 */
const FSimpleSceneRenderer::FMeshGPU& FSimpleSceneRenderer::GetMesh(FSceneObject::EType type) const
{
    switch (type)
    {
    case FSceneObject::EType::Sphere: return MeshSphere;
    case FSceneObject::EType::Box: return MeshBox;
    case FSceneObject::EType::Cone: return MeshCone;
    default: return MeshSphere;
    }
}

const FSimpleSceneRenderer::FMeshGPU* FSimpleSceneRenderer::GetMeshForObject(const FSceneObject& object) const
{
    if (IsProceduralSceneObject(object.Type))
        return &GetMesh(object.Type);
    if (object.Type == FSceneObject::EType::StaticMesh
        && object.StaticMeshIndex >= 0
        && object.StaticMeshIndex < (int)StaticMeshes.size())
    {
        return &StaticMeshes[(size_t)object.StaticMeshIndex];
    }
    return nullptr;
}

/**
 * @brief 编译 HLSL Shader 文件并返回字节码。
 * @param file shader 文件路径。
 * @param entry 入口函数名。
 * @param target 编译目标（如 vs_5_1）。
 * @return 编译后的字节码。
 * @note 阶段：渲染初始化/Shader 编译阶段。
 */
ComPtr<ID3DBlob> FSimpleSceneRenderer::CompileShaderFromFile(const std::wstring& file, const std::string& entry, const std::string& target)
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    // 解析 shader 路径并准备 include 搜索目录。
    const std::filesystem::path resolvedPath = ResolveShaderPath(file);
    const std::wstring resolvedFile = resolvedPath.wstring();

    std::vector<std::filesystem::path> includeDirs;
    includeDirs.push_back(std::filesystem::path(SHADER_DIR));
    {
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        if (!cwd.empty())
            includeDirs.push_back(cwd);
    }
    const auto exeDir = GetExecutableDir();
    if (!exeDir.empty())
        includeDirs.push_back(exeDir / L"shaders");

    FShaderInclude includeHandler(resolvedPath.parent_path(), includeDirs);

    // 编译 shader。
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompileFromFile(
        resolvedFile.c_str(),
        nullptr,
        &includeHandler,
        entry.c_str(),
        target.c_str(),
        flags,
        0,
        &bytecode,
        &errors);

    if (errors)
    {
        // 输出编译错误信息。
        std::string err((const char*)errors->GetBufferPointer(), errors->GetBufferSize());
        DebugOutput(std::wstring(err.begin(), err.end()));
    }
    std::error_code ec;
    if (FAILED(hr))
    {
        DebugOutput(L"Shader compile failed: " + resolvedFile);
        if (!std::filesystem::exists(resolvedPath, ec))
            DebugOutput(L"Shader file not found: " + resolvedFile);
        const auto cwd = std::filesystem::current_path(ec);
        if (!cwd.empty())
            DebugOutput(L"CWD: " + cwd.wstring());
        DebugOutput(L"SHADER_DIR: " + std::wstring(SHADER_DIR));
    }
    std::string failMsg = "D3DCompileFromFile failed: ";
    failMsg += NarrowUtf8(resolvedFile);
    ThrowIfFailed(hr, failMsg.c_str());
    return bytecode;
}

/**
 * @brief 初始化硬件光线追踪（构建 BLAS）。
 * @param rhi 渲染硬件接口。
 * @return 无返回值。
 * @note 阶段：渲染初始化阶段（DXR）。
 */
void FSimpleSceneRenderer::InitRaytracing(FD3D12RHI& rhi)
{
    bRaytracingSupported = false;
    RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;

    ID3D12Device* device = rhi.GetDevice();
    if (!device) return;

    ComPtr<ID3D12Device5> device5;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5))))
        return;

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5))))
        return;

    RaytracingTier = opt5.RaytracingTier;
    if (RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
        return;

    bRaytracingSupported = true;

    // 构建每个基础网格的 BLAS。
    auto buildBLAS = [&](const FMeshGPU& mesh, FRTMeshAS& out, const char* name)
    {
        if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount == 0)
            return;

        const uint32 vertexCount = (mesh.VBView.StrideInBytes > 0) ? (mesh.VBView.SizeInBytes / mesh.VBView.StrideInBytes) : 0u;
        if (vertexCount == 0)
            return;

        D3D12_RAYTRACING_GEOMETRY_DESC geom{};
        geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geom.Triangles.VertexBuffer.StartAddress = mesh.VertexBuffer->GetGPUVirtualAddress();
        geom.Triangles.VertexBuffer.StrideInBytes = sizeof(FVertex);
        geom.Triangles.VertexCount = vertexCount;
        geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geom.Triangles.IndexBuffer = mesh.IndexBuffer->GetGPUVirtualAddress();
        geom.Triangles.IndexCount = mesh.IndexCount;
        geom.Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
        geom.Triangles.Transform3x4 = 0;

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        inputs.NumDescs = 1;
        inputs.pGeometryDescs = &geom;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
        if (info.ResultDataMaxSizeInBytes == 0 || info.ScratchDataSizeInBytes == 0)
            return;

        D3D12_HEAP_PROPERTIES heapDefault{};
        heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

        auto makeBuffer = [&](UINT64 size, D3D12_RESOURCE_STATES initial, ComPtr<ID3D12Resource>& outRes)
        {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = size;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ThrowIfFailed(device->CreateCommittedResource(
                &heapDefault,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                initial,
                nullptr,
                IID_PPV_ARGS(&outRes)),
                name);
        };

        ComPtr<ID3D12Resource> scratch;
        makeBuffer(info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, scratch);
        makeBuffer(info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, out.BLAS);

        struct FBuildCtx
        {
            ID3D12Resource* Scratch = nullptr;
            ID3D12Resource* Dest = nullptr;
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs{};
        } ctx;
        ctx.Scratch = scratch.Get();
        ctx.Dest = out.BLAS.Get();
        ctx.Inputs = inputs;

        auto record = [](ID3D12GraphicsCommandList* cmd, void* user)
        {
            auto* c = reinterpret_cast<FBuildCtx*>(user);
            if (!c || !c->Scratch || !c->Dest) return;

            ComPtr<ID3D12GraphicsCommandList4> cl4;
            if (FAILED(cmd->QueryInterface(IID_PPV_ARGS(&cl4))))
                return;

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
            build.Inputs = c->Inputs;
            build.ScratchAccelerationStructureData = c->Scratch->GetGPUVirtualAddress();
            build.DestAccelerationStructureData = c->Dest->GetGPUVirtualAddress();
            build.SourceAccelerationStructureData = 0;
            cl4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

            D3D12_RESOURCE_BARRIER uav{};
            uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav.UAV.pResource = c->Dest;
            cl4->ResourceBarrier(1, &uav);
        };

        rhi.ExecuteImmediate(record, &ctx);
    };

    buildBLAS(MeshSphere, RTMeshSphere, "Build BLAS (Sphere) failed");
    buildBLAS(MeshBox, RTMeshBox, "Build BLAS (Box) failed");
    buildBLAS(MeshCone, RTMeshCone, "Build BLAS (Cone) failed");
}

/**
 * @brief 初始化渲染器资源（根签名、PSO、网格、常量缓冲等）。
 * @param rhi 渲染硬件接口。
 * @return 无返回值。
 * @note 阶段：渲染初始化阶段。
 */
void FSimpleSceneRenderer::Init(FD3D12RHI& rhi)
{
    ID3D12Device* device = rhi.GetDevice();

    // 创建主渲染根签名。
    // Root signature (CBV b0 + material SRVs + shadow data)
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 5;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = 0;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[2].Descriptor.ShaderRegister = 1;
    params[2].Descriptor.RegisterSpace = 0;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE rangeShadow{};
    rangeShadow.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rangeShadow.NumDescriptors = 3; // shadow map + sky prefilter + sky SH
    rangeShadow.BaseShaderRegister = 5;
    rangeShadow.RegisterSpace = 0;
    rangeShadow.OffsetInDescriptorsFromTableStart = 0;

    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &rangeShadow;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 4;
    rsDesc.pParameters = params;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].MipLODBias = 0.0f;
    samplers[0].MaxAnisotropy = 1;
    samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    samplers[0].MinLOD = 0.0f;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;
    samplers[0].RegisterSpace = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].MipLODBias = 0.0f;
    samplers[1].MaxAnisotropy = 1;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[1].MinLOD = 0.0f;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].ShaderRegister = 1;
    samplers[1].RegisterSpace = 0;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    rsDesc.NumStaticSamplers = 2;
    rsDesc.pStaticSamplers = samplers;

    ComPtr<ID3DBlob> sigBlob, errBlob;
    HRESULT rsHr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (errBlob)
    {
        std::string err((const char*)errBlob->GetBufferPointer(), errBlob->GetBufferSize());
        DebugOutput(std::wstring(err.begin(), err.end()));
    }
    ThrowIfFailed(rsHr, "D3D12SerializeRootSignature failed");
    ThrowIfFailed(device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&RootSig)),
                  "CreateRootSignature failed");

    // 编译主渲染 Shader。
    // Shaders
    std::wstring pbrPath = std::wstring(SHADER_DIR) + L"/pbr.hlsl";
    auto vs = CompileShaderFromFile(pbrPath, "VSMain", "vs_5_0");
    auto ps = CompileShaderFromFile(pbrPath, "PSMain", "ps_5_0");

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_BLEND_DESC blend{};
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = FALSE;
    {
        D3D12_RENDER_TARGET_BLEND_DESC rt{};
        rt.BlendEnable = FALSE;
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ZERO;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        for (int i = 0; i < 8; ++i) blend.RenderTarget[i] = rt;
    }

    D3D12_RASTERIZER_DESC rast{};
    rast.FillMode = D3D12_FILL_MODE_SOLID;
    rast.CullMode = D3D12_CULL_MODE_BACK;
    rast.FrontCounterClockwise = FALSE;
    rast.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rast.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rast.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rast.DepthClipEnable = TRUE;
    rast.MultisampleEnable = FALSE;
    rast.AntialiasedLineEnable = FALSE;
    rast.ForcedSampleCount = 0;
    rast.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    ds.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = RootSig.Get();
    psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    psoDesc.BlendState = blend;
    psoDesc.RasterizerState = rast;
    psoDesc.DepthStencilState = ds;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.InputLayout = { layout, _countof(layout) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = HDRFormat;
    psoDesc.DSVFormat = rhi.GetDepthFormat();
    psoDesc.SampleDesc.Count = 1;

    {
        ComPtr<ID3D12InfoQueue> infoQueue;
        UINT64 beforeCount = 0;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))) && infoQueue)
            beforeCount = infoQueue->GetNumStoredMessages();

        HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&PSO));
        if (FAILED(hr))
        {
            std::string extra;
            if (infoQueue)
            {
                const UINT64 afterCount = infoQueue->GetNumStoredMessages();
                for (UINT64 i = beforeCount; i < afterCount; ++i)
                {
                    SIZE_T len = 0;
                    if (FAILED(infoQueue->GetMessage((UINT64)i, nullptr, &len)) || len == 0) continue;
                    std::vector<uint8_t> bytes(len);
                    auto* msg = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                    if (SUCCEEDED(infoQueue->GetMessage((UINT64)i, msg, &len)) && msg->pDescription)
                    {
                        extra += msg->pDescription;
                        extra += "\n";
                    }
                }
            }
            ThrowIfFailed(hr, extra.empty() ? "CreateGraphicsPipelineState failed" : ("CreateGraphicsPipelineState failed:\n" + extra).c_str());
        }
    }

    // Gizmo 线框渲染 PSO。
    // Line PSO for gizmo
    {
        std::wstring linesPath = std::wstring(SHADER_DIR) + L"/lines.hlsl";
        auto lvs = CompileShaderFromFile(linesPath, "VSMain", "vs_5_0");
        auto lps = CompileShaderFromFile(linesPath, "PSMain", "ps_5_0");

        D3D12_INPUT_ELEMENT_DESC lineLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC linePso = psoDesc;
        linePso.VS = { lvs->GetBufferPointer(), lvs->GetBufferSize() };
        linePso.PS = { lps->GetBufferPointer(), lps->GetBufferSize() };
        linePso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        linePso.InputLayout = { lineLayout, _countof(lineLayout) };
        // Gizmo should render on top without depth testing.
        linePso.DepthStencilState.DepthEnable = FALSE;
        linePso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        linePso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;

        ThrowIfFailed(device->CreateGraphicsPipelineState(&linePso, IID_PPV_ARGS(&PSO_Lines)), "CreateGraphicsPipelineState (lines) failed");
    }

    // 初始化各渲染通道的 PSO/RootSignature。
    InitSkyPass(rhi, blend, rast);
    InitSkyIBLPass(rhi);
    InitTonemapPass(rhi, blend, rast);
    InitDeferredPass(rhi, psoDesc, blend, rast);
    InitShadowPass(rhi, psoDesc, rast);
    InitLumenPass(rhi, blend, rast);
    InitLumenSwrtPass(rhi, blend, rast);

    auto createMesh = [&](FMeshGPU& mesh, const std::vector<FVertex>& verts, const std::vector<uint16>& indices, const char* name)
    {
        const uint32 vbSize = (uint32)(verts.size() * sizeof(FVertex));
        const uint32 ibSize = (uint32)(indices.size() * sizeof(uint16));

        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC vbDesc{};
        vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        vbDesc.Width = vbSize;
        vbDesc.Height = 1;
        vbDesc.DepthOrArraySize = 1;
        vbDesc.MipLevels = 1;
        vbDesc.SampleDesc.Count = 1;
        vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&mesh.VertexBuffer)),
                      (std::string("CreateCommittedResource vb failed: ") + name).c_str());
        {
            void* mapped = nullptr;
            D3D12_RANGE readRange{ 0,0 };
            ThrowIfFailed(mesh.VertexBuffer->Map(0, &readRange, &mapped), "VB Map failed");
            std::memcpy(mapped, verts.data(), vbSize);
            mesh.VertexBuffer->Unmap(0, nullptr);
        }

        D3D12_RESOURCE_DESC ibDesc = vbDesc;
        ibDesc.Width = ibSize;
        ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &ibDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&mesh.IndexBuffer)),
                      (std::string("CreateCommittedResource ib failed: ") + name).c_str());
        {
            void* mapped = nullptr;
            D3D12_RANGE readRange{ 0,0 };
            ThrowIfFailed(mesh.IndexBuffer->Map(0, &readRange, &mapped), "IB Map failed");
            std::memcpy(mapped, indices.data(), ibSize);
            mesh.IndexBuffer->Unmap(0, nullptr);
        }

        mesh.VBView.BufferLocation = mesh.VertexBuffer->GetGPUVirtualAddress();
        mesh.VBView.SizeInBytes = vbSize;
        mesh.VBView.StrideInBytes = sizeof(FVertex);

        mesh.IBView.BufferLocation = mesh.IndexBuffer->GetGPUVirtualAddress();
        mesh.IBView.SizeInBytes = ibSize;
        mesh.IBView.Format = DXGI_FORMAT_R16_UINT;

        mesh.IndexCount = (uint32)indices.size();
    };

    {
        std::vector<FVertex> verts;
        std::vector<uint16> indices;
        GenerateSphereMesh(48, 24, 0.75f, verts, indices);
        createMesh(MeshSphere, verts, indices, "Sphere");
    }
    {
        std::vector<FVertex> verts;
        std::vector<uint16> indices;
        GenerateBoxMesh(0.6f, verts, indices);
        createMesh(MeshBox, verts, indices, "Box");
    }
    {
        std::vector<FVertex> verts;
        std::vector<uint16> indices;
        GenerateConeMesh(48, 0.6f, 1.4f, verts, indices);
        createMesh(MeshCone, verts, indices, "Cone");
    }

    InitRaytracing(rhi);

    bHWRTGIReady = false;
    HWRTGIInitError.clear();
    InitHWRTGIPass(rhi, blend, rast);

    // Constant buffers
    CBSize = Align256((uint32)sizeof(FSceneCB));
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC cbDesc{};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = CBSize * MaxObjects;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (uint32 i = 0; i < FD3D12RHI::kFrameCount; ++i)
    {
        ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&ConstantBufferObjects[i])),
                      "CreateCommittedResource cb objects failed");
        void* mapped = nullptr;
        D3D12_RANGE readRange{ 0,0 };
        ThrowIfFailed(ConstantBufferObjects[i]->Map(0, &readRange, &mapped), "CB objects Map failed");
        CBMappedObjects[i] = static_cast<uint8*>(mapped);
        std::memset(CBMappedObjects[i], 0, CBSize * MaxObjects);

        mapped = nullptr;
        D3D12_RESOURCE_DESC gizmoCbDesc = cbDesc;
        gizmoCbDesc.Width = CBSize;
        ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &gizmoCbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&ConstantBufferGizmo[i])),
                      "CreateCommittedResource cb gizmo failed");
        ThrowIfFailed(ConstantBufferGizmo[i]->Map(0, &readRange, &mapped), "CB gizmo Map failed");
        CBMappedGizmo[i] = static_cast<uint8*>(mapped);
        std::memset(CBMappedGizmo[i], 0, CBSize);
    }

    // Sky constant buffers (per frame)
    SkyCBSize = Align256((uint32)sizeof(FSkyCB));
    {
        D3D12_RESOURCE_DESC skyDesc = cbDesc;
        skyDesc.Width = SkyCBSize;
        for (uint32 i = 0; i < FD3D12RHI::kFrameCount; ++i)
        {
            ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &skyDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&ConstantBufferSky[i])),
                          "CreateCommittedResource cb sky failed");
            void* mapped = nullptr;
            D3D12_RANGE readRange{ 0,0 };
            ThrowIfFailed(ConstantBufferSky[i]->Map(0, &readRange, &mapped), "CB sky Map failed");
            CBMappedSky[i] = static_cast<uint8*>(mapped);
            std::memset(CBMappedSky[i], 0, SkyCBSize);
        }
    }

    // Sky IBL constant buffers (per frame)
    SkyIBLCBSize = Align256((uint32)sizeof(FSkyIBLCB));
    {
        D3D12_RESOURCE_DESC iblDesc = cbDesc;
        iblDesc.Width = SkyIBLCBSize * kSkyIBLConstantCount;
        for (uint32 i = 0; i < FD3D12RHI::kFrameCount; ++i)
        {
            ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &iblDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&ConstantBufferSkyIBL[i])),
                          "CreateCommittedResource cb sky ibl failed");
            void* mapped = nullptr;
            D3D12_RANGE readRange{ 0,0 };
            ThrowIfFailed(ConstantBufferSkyIBL[i]->Map(0, &readRange, &mapped), "CB sky ibl Map failed");
            CBMappedSkyIBL[i] = static_cast<uint8*>(mapped);
            std::memset(CBMappedSkyIBL[i], 0, SkyIBLCBSize * kSkyIBLConstantCount);
        }
    }

    // Tonemap constant buffers (per frame)
    TonemapCBSize = Align256((uint32)sizeof(FTonemapCB));
    {
        D3D12_RESOURCE_DESC tmDesc = cbDesc;
        tmDesc.Width = TonemapCBSize;

        for (uint32 i = 0; i < FD3D12RHI::kFrameCount; ++i)
        {
            ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &tmDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&ConstantBufferTonemap[i])),
                          "CreateCommittedResource cb tonemap failed");
            void* mapped = nullptr;
            D3D12_RANGE readRange{ 0,0 };
            ThrowIfFailed(ConstantBufferTonemap[i]->Map(0, &readRange, &mapped), "CB tonemap Map failed");
            CBMappedTonemap[i] = static_cast<uint8*>(mapped);
            std::memset(CBMappedTonemap[i], 0, TonemapCBSize);
        }
    }

    // Deferred lighting constant buffers (per frame)
    DeferredCBSize = Align256((uint32)sizeof(FDeferredLightCB));
    {
        D3D12_RESOURCE_DESC dlDesc = cbDesc;
        dlDesc.Width = DeferredCBSize;

        for (uint32 i = 0; i < FD3D12RHI::kFrameCount; ++i)
        {
            ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &dlDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&ConstantBufferDeferred[i])),
                          "CreateCommittedResource cb deferred failed");
            void* mapped = nullptr;
            D3D12_RANGE readRange{ 0,0 };
            ThrowIfFailed(ConstantBufferDeferred[i]->Map(0, &readRange, &mapped), "CB deferred Map failed");
            CBMappedDeferred[i] = static_cast<uint8*>(mapped);
            std::memset(CBMappedDeferred[i], 0, DeferredCBSize);
        }
    }

    // Shadow constant buffers (per frame)
    ShadowCBSize = Align256((uint32)sizeof(FShadowCB));
    {
        D3D12_RESOURCE_DESC shDesc = cbDesc;
        shDesc.Width = ShadowCBSize;

        for (uint32 i = 0; i < FD3D12RHI::kFrameCount; ++i)
        {
            ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &shDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&ConstantBufferShadow[i])),
                          "CreateCommittedResource cb shadow failed");
            void* mapped = nullptr;
            D3D12_RANGE readRange{ 0,0 };
            ThrowIfFailed(ConstantBufferShadow[i]->Map(0, &readRange, &mapped), "CB shadow Map failed");
            CBMappedShadow[i] = static_cast<uint8*>(mapped);
            std::memset(CBMappedShadow[i], 0, ShadowCBSize);
        }
    }

    // Lumen constant buffers (per frame)
    LumenCBSize = Align256((uint32)sizeof(FLumenCB));
    {
        D3D12_RESOURCE_DESC lumenDesc = cbDesc;
        lumenDesc.Width = LumenCBSize;

        for (uint32 i = 0; i < FD3D12RHI::kFrameCount; ++i)
        {
            ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &lumenDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&ConstantBufferLumen[i])),
                          "CreateCommittedResource cb lumen failed");
            void* mapped = nullptr;
            D3D12_RANGE readRange{ 0,0 };
            ThrowIfFailed(ConstantBufferLumen[i]->Map(0, &readRange, &mapped), "CB lumen Map failed");
            CBMappedLumen[i] = static_cast<uint8*>(mapped);
            std::memset(CBMappedLumen[i], 0, LumenCBSize);
        }
    }

    // Lumen HWRT GI constant buffers (per frame)
    HWRTGICBSize = Align256((uint32)sizeof(FHWRTGICB));
    {
        D3D12_RESOURCE_DESC hwDesc = cbDesc;
        hwDesc.Width = HWRTGICBSize;

        for (uint32 i = 0; i < FD3D12RHI::kFrameCount; ++i)
        {
            ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &hwDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&ConstantBufferHWRTGI[i])),
                          "CreateCommittedResource cb hwrt gi failed");
            void* mapped = nullptr;
            D3D12_RANGE readRange{ 0,0 };
            ThrowIfFailed(ConstantBufferHWRTGI[i]->Map(0, &readRange, &mapped), "CB hwrt gi Map failed");
            CBMappedHWRTGI[i] = static_cast<uint8*>(mapped);
            std::memset(CBMappedHWRTGI[i], 0, HWRTGICBSize);
        }
    }

    // Gizmo vertex buffer (supports translate/scale handle lines), dynamic upload
    {
        D3D12_HEAP_PROPERTIES gizmoUploadHeap{};
        gizmoUploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC gizmoVBDesc{};
        gizmoVBDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        gizmoVBDesc.Width = sizeof(FVertex) * 64;
        gizmoVBDesc.Height = 1;
        gizmoVBDesc.DepthOrArraySize = 1;
        gizmoVBDesc.MipLevels = 1;
        gizmoVBDesc.SampleDesc.Count = 1;
        gizmoVBDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(device->CreateCommittedResource(&gizmoUploadHeap, D3D12_HEAP_FLAG_NONE, &gizmoVBDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&GizmoVB)),
                      "CreateCommittedResource gizmo vb failed");

        void* mapped = nullptr;
        D3D12_RANGE readRange{ 0,0 };
        ThrowIfFailed(GizmoVB->Map(0, &readRange, &mapped), "GizmoVB Map failed");
        GizmoMapped = static_cast<FVertex*>(mapped);
        std::memset(GizmoMapped, 0, sizeof(FVertex) * 64);

        GizmoVBView.BufferLocation = GizmoVB->GetGPUVirtualAddress();
        GizmoVBView.SizeInBytes = sizeof(FVertex) * 64;
        GizmoVBView.StrideInBytes = sizeof(FVertex);
    }

    // SRV heap + built-in textures at slots 0..4 and default material block
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 2048;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&SRVHeap)), "CreateDescriptorHeap SRV failed");
        SRVDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        Textures.clear();
        Textures.reserve(512);

        const uint8 white[4] = { 255, 255, 255, 255 };      // albedo/rough/ao
        const uint8 normal[4] = { 128, 128, 255, 255 };     // flat normal
        const uint8 black[4] = { 0, 0, 0, 255 };            // metallic
        CreateTextureRGBA8(rhi, 1, 1, white);   // 0
        CreateTextureRGBA8(rhi, 1, 1, normal);  // 1
        CreateTextureRGBA8(rhi, 1, 1, white);   // 2 roughness neutral
        CreateTextureRGBA8(rhi, 1, 1, black);   // 3 metallic neutral
        CreateTextureRGBA8(rhi, 1, 1, white);   // 4 ao neutral
    }

    // Tonemap SRV heap (single SRV for HDRColor)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 1;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&TonemapSRVHeap)), "CreateDescriptorHeap SRV (tonemap) failed");
        TonemapSRVDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    EnsureHDRTargets(rhi);
    InitRenderDocRockRenderer(rhi);
}

void FSimpleSceneRenderer::InitRenderDocRockRenderer(FD3D12RHI& rhi)
{
    std::string error;
    const std::filesystem::path manifestPath = std::filesystem::path(ASSET_DIR) / L"renderdoc_rock" / L"CaptureRockManifest.json";
    const std::filesystem::path shaderPath = std::filesystem::path(SHADER_DIR) / L"renderdoc_rock.hlsl";
    if (RenderDocRockRenderer.Initialize(rhi, HDRFormat, rhi.GetDepthFormat(), manifestPath, shaderPath, &error))
    {
        DebugOutput(L"RenderDoc rock renderer loaded: " + RenderDocRockRenderer.FormatSummary());
    }
    else
    {
        DebugOutput(L"RenderDoc rock renderer not loaded: " + std::wstring(error.begin(), error.end()));
    }
}

/**
 * @brief 释放渲染器资源并解除映射。
 * @param 无。
 * @return 无返回值。
 * @note 阶段：渲染销毁阶段。
 */
void FSimpleSceneRenderer::AddRenderDocRockPass(
    FRenderGraphBuilder& graph,
    const FD3D12FrameContext& frame,
    const D3D12_VIEWPORT& vp,
    const D3D12_RECT& sc,
    D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv,
    const DirectX::XMFLOAT4X4& viewProj,
    const DirectX::XMFLOAT3& cameraPositionWs,
    const std::vector<FSceneObject>& objects,
    const DirectX::XMFLOAT3* previewPos,
    FSceneObject::EType previewType)
{
    if (!RenderDocRockRenderer.IsLoaded())
    {
        return;
    }

    rdcimport::FCaptureRockFrameInputs inputs{};
    inputs.TargetRTV = hdrRtv;
    inputs.DepthDSV = frame.DSV;
    inputs.Viewport = vp;
    inputs.Scissor = sc;
    inputs.ViewProj = viewProj;
    inputs.CameraPositionWs = cameraPositionWs;
    inputs.FrameIndex = frame.FrameIndex;
    const uint32 drawCount = (uint32)std::min<size_t>(objects.size(), MaxObjects);
    for (uint32 i = 0; i < drawCount; ++i)
    {
        if (IsRenderDocRockObject(objects[i].Type))
        {
            inputs.Instances.push_back({ objects[i].Position, objects[i].Scale });
        }
    }
    if (previewPos && IsRenderDocRockObject(previewType))
    {
        DirectX::XMFLOAT3 previewPosition = *previewPos;
        previewPosition.y += 0.39f;
        inputs.Instances.push_back({ previewPosition, { 0.35f, 0.35f, 0.35f } });
    }
    if (inputs.Instances.empty())
    {
        return;
    }

    auto* rock = &RenderDocRockRenderer;
    graph.AddPass("RenderDocRock", [rock, inputs](ID3D12GraphicsCommandList* cl)
    {
        rock->Render(cl, inputs);
    });
}

void FSimpleSceneRenderer::Shutdown()
{
    RenderDocRockRenderer.Reset();

    // 解除 Gizmo 顶点缓冲映射。
    if (GizmoVB && GizmoMapped)
        GizmoVB->Unmap(0, nullptr);
    GizmoMapped = nullptr;

    // 解除常量缓冲映射并清理 RT 相关数据。
    for (uint32 i = 0; i < FD3D12RHI::kFrameCount; ++i)
    {
        if (ConstantBufferObjects[i] && CBMappedObjects[i])
            ConstantBufferObjects[i]->Unmap(0, nullptr);
        CBMappedObjects[i] = nullptr;

        if (ConstantBufferGizmo[i] && CBMappedGizmo[i])
            ConstantBufferGizmo[i]->Unmap(0, nullptr);
        CBMappedGizmo[i] = nullptr;

        if (ConstantBufferTonemap[i] && CBMappedTonemap[i])
            ConstantBufferTonemap[i]->Unmap(0, nullptr);
        CBMappedTonemap[i] = nullptr;

        if (ConstantBufferDeferred[i] && CBMappedDeferred[i])
            ConstantBufferDeferred[i]->Unmap(0, nullptr);
        CBMappedDeferred[i] = nullptr;

        if (ConstantBufferShadow[i] && CBMappedShadow[i])
            ConstantBufferShadow[i]->Unmap(0, nullptr);
        CBMappedShadow[i] = nullptr;

        if (ConstantBufferLumen[i] && CBMappedLumen[i])
            ConstantBufferLumen[i]->Unmap(0, nullptr);
        CBMappedLumen[i] = nullptr;

        if (ConstantBufferHWRTGI[i] && CBMappedHWRTGI[i])
            ConstantBufferHWRTGI[i]->Unmap(0, nullptr);
        CBMappedHWRTGI[i] = nullptr;

        if (ConstantBufferSky[i] && CBMappedSky[i])
            ConstantBufferSky[i]->Unmap(0, nullptr);
        CBMappedSky[i] = nullptr;

        if (ConstantBufferSkyIBL[i] && CBMappedSkyIBL[i])
            ConstantBufferSkyIBL[i]->Unmap(0, nullptr);
        CBMappedSkyIBL[i] = nullptr;

        if (RTObjectBuffer[i] && RTObjectMapped[i])
            RTObjectBuffer[i]->Unmap(0, nullptr);
        RTObjectMapped[i] = nullptr;
        RTObjectInstanceCount[i] = 0;

        if (RTFrame[i].InstanceDescsUpload && RTFrame[i].InstanceDescsMapped)
            RTFrame[i].InstanceDescsUpload->Unmap(0, nullptr);
        RTFrame[i].InstanceDescsMapped = nullptr;
    }

    HDRColor.Reset();
    HDRRTVHeap.Reset();
    TonemapSRVHeap.Reset();
    ShadowMap.Reset();
    ShadowDSVHeap.Reset();
    ShadowRootSig.Reset();
    ShadowPSO.Reset();
    SkyCube.Reset();
    SkyPrefilter.Reset();
    SkySH.Reset();
    SkyIBLHeap.Reset();
    SkyIBLRootSig.Reset();
    SkyIBLGenPSO.Reset();
    SkyIBLSHPSO.Reset();
    SkyIBLPrefilterPSO.Reset();
    SkyIBLPrefilterUavGPU.clear();

    GBuffer0.Reset();
    GBuffer1.Reset();
    GBuffer2.Reset();
    GBufferRTVHeap.Reset();
    GBufferSRVHeap.Reset();

    LumenHistory[0].Reset();
    LumenHistory[1].Reset();
    LumenRTVHeap.Reset();
    LumenSwrtSurface[0].Reset();
    LumenSwrtSurface[1].Reset();
    LumenSwrtRootSig.Reset();
    LumenSwrtSurfacePSO.Reset();
    LumenSwrtGIPSO.Reset();
    LumenSwrtFilterPSO.Reset();
    LumenSwrtAddRootSig.Reset();
    LumenSwrtAddPSO.Reset();

    HWRTGIHistory[0].Reset();
    HWRTGIHistory[1].Reset();
    HWRTGIMeta[0].Reset();
    HWRTGIMeta[1].Reset();
    HWRTGIFiltered.Reset();
    HWRTGIRootSig.Reset();
    HWRTGIPSO.Reset();
    HWRTGIFilterPSO.Reset();
    HWRTGIAddRootSig.Reset();
    HWRTGIAddPSO.Reset();

    RTMeshSphere.BLAS.Reset();
    RTMeshBox.BLAS.Reset();
    RTMeshCone.BLAS.Reset();
    StaticMeshes.clear();
    for (uint32 i = 0; i < FD3D12RHI::kFrameCount; ++i)
    {
        RTFrame[i].TLAS.Reset();
        RTFrame[i].Scratch.Reset();
        RTFrame[i].InstanceDescsUpload.Reset();
        RTFrame[i].MaxInstances = 0;
        RTObjectBuffer[i].Reset();
    }

    bRaytracingSupported = false;
    RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    bHWRTGIReady = false;
    HWRTGIInitError.clear();
    bSWRTGIHistoryValid = false;
}

/**
 * @brief 确保 HDR 渲染目标与当前分辨率匹配。
 * @param rhi 渲染硬件接口。
 * @return 无返回值。
 * @note 阶段：渲染帧资源准备阶段。
 */
void FSimpleSceneRenderer::EnsureHDRTargets(FD3D12RHI& rhi)
{
    ID3D12Device* device = rhi.GetDevice();
    if (!device || !TonemapSRVHeap) return;

    const uint32 w = std::max<uint32>(1u, rhi.GetWidth());
    const uint32 h = std::max<uint32>(1u, rhi.GetHeight());
    if (HDRColor && w == HDRWidth && h == HDRHeight)
        return;

    // 尺寸发生变化时重建 HDR 资源与 RTV。
    HDRWidth = w;
    HDRHeight = h;
    HDRColor.Reset();
    HDRRTVHeap.Reset();

    D3D12_HEAP_PROPERTIES heapDefault{};
    heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = HDRWidth;
    desc.Height = HDRHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = HDRFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = HDRFormat;
    clear.Color[0] = 0.06f;
    clear.Color[1] = 0.06f;
    clear.Color[2] = 0.08f;
    clear.Color[3] = 1.0f;

    ThrowIfFailed(device->CreateCommittedResource(
        &heapDefault,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &clear,
        IID_PPV_ARGS(&HDRColor)), "CreateCommittedResource HDR color failed");

    // RTV heap (1 descriptor)
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = 1;
        ThrowIfFailed(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&HDRRTVHeap)), "CreateDescriptorHeap HDR RTV failed");
        HDRRTV = HDRRTVHeap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(HDRColor.Get(), nullptr, HDRRTV);
    }

    // SRV into tonemap heap
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = HDRFormat;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(HDRColor.Get(), &srv, TonemapSRVHeap->GetCPUDescriptorHandleForHeapStart());
        TonemapHDRSRVGPU = TonemapSRVHeap->GetGPUDescriptorHandleForHeapStart();
    }

    HDRState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

/**
 * @brief 确保阴影贴图资源存在并完成初始化。
 * @param rhi 渲染硬件接口。
 * @return 无返回值。
 * @note 阶段：阴影资源准备阶段。
 */
void FSimpleSceneRenderer::EnsureShadowMap(FD3D12RHI& rhi)
{
    ID3D12Device* device = rhi.GetDevice();
    if (!device || !SRVHeap) return;
    if (ShadowMap)
        return;

    // 创建深度纹理与 DSV/SRV 资源。
    const uint32 size = std::max<uint32>(1u, ShadowMapSize);

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = size;
    desc.Height = size;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = ShadowDepthFormat;
    clear.DepthStencil.Depth = 1.0f;
    clear.DepthStencil.Stencil = 0;

    D3D12_HEAP_PROPERTIES heapDefault{};
    heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

    ThrowIfFailed(device->CreateCommittedResource(
        &heapDefault,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &clear,
        IID_PPV_ARGS(&ShadowMap)), "CreateCommittedResource ShadowMap failed");

    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDesc.NumDescriptors = 1;
    ThrowIfFailed(device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&ShadowDSVHeap)), "CreateDescriptorHeap Shadow DSV failed");

    ShadowDSV = ShadowDSVHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = ShadowDepthFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsv.Flags = D3D12_DSV_FLAG_NONE;
    device->CreateDepthStencilView(ShadowMap.Get(), &dsv, ShadowDSV);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = SRVHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(kShadowMapSRVSlot) * SRVDescriptorSize;
    device->CreateShaderResourceView(ShadowMap.Get(), &srv, cpu);

    ShadowState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

/**
 * @brief 确保延迟渲染相关的 GBuffer 与 GI 资源存在。
 * @param rhi 渲染硬件接口。
 * @return 无返回值。
 * @note 阶段：延迟渲染资源准备阶段。
 */
void FSimpleSceneRenderer::EnsureDeferredTargets(FD3D12RHI& rhi)
{
    ID3D12Device* device = rhi.GetDevice();
    if (!device) return;

    const uint32 w = std::max<uint32>(1u, rhi.GetWidth());
    const uint32 h = std::max<uint32>(1u, rhi.GetHeight());
    if (GBuffer0 && GBuffer1 && GBuffer2 && GBufferRTVHeap && GBufferSRVHeap && LumenHistory[0] && LumenHistory[1] && LumenRTVHeap &&
        LumenSwrtSurface[0] && LumenSwrtSurface[1] &&
        HWRTGIHistory[0] && HWRTGIHistory[1] && HWRTGIMeta[0] && HWRTGIMeta[1] && HWRTGIFiltered &&
        w == GBufferWidth && h == GBufferHeight)
        return;

    // 资源重建：清空旧资源并重置状态。
    GBufferWidth = w;
    GBufferHeight = h;
    GBuffer0.Reset();
    GBuffer1.Reset();
    GBuffer2.Reset();
    GBufferRTVHeap.Reset();
    GBufferSRVHeap.Reset();
    std::memset(GBufferRTVs, 0, sizeof(GBufferRTVs));

    LumenWidth = w;
    LumenHeight = h;
    LumenHistory[0].Reset();
    LumenHistory[1].Reset();
    LumenRTVHeap.Reset();
    std::memset(LumenRTVs, 0, sizeof(LumenRTVs));
    LumenHistoryWriteIndex = 0;
    bLumenHistoryValid = false;

    HWRTGIWidth = (w + 1u) / 2u;
    HWRTGIHeight = (h + 1u) / 2u;
    LumenSwrtWidth = HWRTGIWidth;
    LumenSwrtHeight = HWRTGIHeight;
    LumenSwrtSurface[0].Reset();
    LumenSwrtSurface[1].Reset();
    LumenSwrtSurfaceStates[0] = D3D12_RESOURCE_STATE_GENERIC_READ;
    LumenSwrtSurfaceStates[1] = D3D12_RESOURCE_STATE_GENERIC_READ;
    bSWRTGIHistoryValid = false;

    HWRTGIHistory[0].Reset();
    HWRTGIHistory[1].Reset();
    HWRTGIMeta[0].Reset();
    HWRTGIMeta[1].Reset();
    HWRTGIFiltered.Reset();
    HWRTGIHistoryStates[0] = D3D12_RESOURCE_STATE_GENERIC_READ;
    HWRTGIHistoryStates[1] = D3D12_RESOURCE_STATE_GENERIC_READ;
    HWRTGIMetaStates[0] = D3D12_RESOURCE_STATE_GENERIC_READ;
    HWRTGIMetaStates[1] = D3D12_RESOURCE_STATE_GENERIC_READ;
    HWRTGIFilteredState = D3D12_RESOURCE_STATE_GENERIC_READ;
    bHWRTGIHistoryValid = false;

    D3D12_HEAP_PROPERTIES heapDefault{};
    heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto createRT = [&](ComPtr<ID3D12Resource>& out, DXGI_FORMAT format, const float clearColor[4], const char* debugName)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = GBufferWidth;
        desc.Height = GBufferHeight;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clear{};
        clear.Format = format;
        clear.Color[0] = clearColor[0];
        clear.Color[1] = clearColor[1];
        clear.Color[2] = clearColor[2];
        clear.Color[3] = clearColor[3];

        ThrowIfFailed(device->CreateCommittedResource(
            &heapDefault,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clear,
            IID_PPV_ARGS(&out)), debugName);
    };

    {
        const float c0[4] = { 0, 0, 0, 0 };
        const float c1[4] = { 0, 0, 1, 0 }; // alpha=0 => empty pixel (roughness+1 encoding)
        const float c2[4] = { 0, 0, 0, 1 };
        createRT(GBuffer0, GBuffer0Format, c0, "CreateCommittedResource GBuffer0 failed");
        createRT(GBuffer1, GBuffer1Format, c1, "CreateCommittedResource GBuffer1 failed");
        createRT(GBuffer2, GBuffer2Format, c2, "CreateCommittedResource GBuffer2 failed");
    }

    // RTV heap (3 descriptors)
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = 3;
        ThrowIfFailed(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&GBufferRTVHeap)), "CreateDescriptorHeap GBuffer RTV failed");

        const uint32 inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE h0 = GBufferRTVHeap->GetCPUDescriptorHandleForHeapStart();
        GBufferRTVs[0] = h0;
        GBufferRTVs[1] = { h0.ptr + SIZE_T(inc) };
        GBufferRTVs[2] = { h0.ptr + SIZE_T(inc) * 2 };
        device->CreateRenderTargetView(GBuffer0.Get(), nullptr, GBufferRTVs[0]);
        device->CreateRenderTargetView(GBuffer1.Get(), nullptr, GBufferRTVs[1]);
        device->CreateRenderTargetView(GBuffer2.Get(), nullptr, GBufferRTVs[2]);
    }

    // Lumen history textures + RTV heap (2 descriptors)
    {
        const float c[4] = { 0, 0, 0, 0 };
        createRT(LumenHistory[0], LumenHistoryFormat, c, "CreateCommittedResource LumenHistory0 failed");
        createRT(LumenHistory[1], LumenHistoryFormat, c, "CreateCommittedResource LumenHistory1 failed");

        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = 2;
        ThrowIfFailed(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&LumenRTVHeap)), "CreateDescriptorHeap Lumen RTV failed");

        const uint32 inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE h0 = LumenRTVHeap->GetCPUDescriptorHandleForHeapStart();
        LumenRTVs[0] = h0;
        LumenRTVs[1] = { h0.ptr + SIZE_T(inc) };
        device->CreateRenderTargetView(LumenHistory[0].Get(), nullptr, LumenRTVs[0]);
        device->CreateRenderTargetView(LumenHistory[1].Get(), nullptr, LumenRTVs[1]);
    }

    // HWRT GI: history + meta + filtered textures (UAV/SRV only, half res)
    {
        auto createUAVTex = [&](ComPtr<ID3D12Resource>& out, DXGI_FORMAT format, const char* debugName)
        {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = HWRTGIWidth;
            desc.Height = HWRTGIHeight;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            ThrowIfFailed(device->CreateCommittedResource(
                &heapDefault,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&out)), debugName);
        };

        createUAVTex(HWRTGIHistory[0], DXGI_FORMAT_R16G16B16A16_FLOAT, "CreateCommittedResource HWRTGIHistory0 failed");
        createUAVTex(HWRTGIHistory[1], DXGI_FORMAT_R16G16B16A16_FLOAT, "CreateCommittedResource HWRTGIHistory1 failed");
        createUAVTex(HWRTGIMeta[0], DXGI_FORMAT_R16G16B16A16_FLOAT, "CreateCommittedResource HWRTGIMeta0 failed");
        createUAVTex(HWRTGIMeta[1], DXGI_FORMAT_R16G16B16A16_FLOAT, "CreateCommittedResource HWRTGIMeta1 failed");
        createUAVTex(HWRTGIFiltered, DXGI_FORMAT_R16G16B16A16_FLOAT, "CreateCommittedResource HWRTGIFiltered failed");
    }

    // Lumen SWRT: surface cache textures (UAV/SRV only, half res)
    {
        auto createSwrtTex = [&](ComPtr<ID3D12Resource>& out, DXGI_FORMAT format, const char* debugName)
        {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = LumenSwrtWidth;
            desc.Height = LumenSwrtHeight;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            ThrowIfFailed(device->CreateCommittedResource(
                &heapDefault,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&out)), debugName);
        };

        createSwrtTex(LumenSwrtSurface[0], LumenSwrtSurfaceFormat, "CreateCommittedResource LumenSWRTSurface0 failed");
        createSwrtTex(LumenSwrtSurface[1], LumenSwrtSurfaceFormat, "CreateCommittedResource LumenSWRTSurface1 failed");
    }

    // SRV heap for GBuffer sampling in deferred lighting
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = kGBufferHeapNumDescriptors;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&GBufferSRVHeap)), "CreateDescriptorHeap SRV (gbuffer) failed");
        GBufferSRVDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        auto createSRV = [&](ID3D12Resource* res, DXGI_FORMAT format, uint32 index)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;

            D3D12_CPU_DESCRIPTOR_HANDLE cpu = GBufferSRVHeap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += SIZE_T(index) * GBufferSRVDescriptorSize;
            device->CreateShaderResourceView(res, &srv, cpu);
        };

        auto createCubeSRV = [&](ID3D12Resource* res, DXGI_FORMAT format, uint32 mipLevels, uint32 index)
        {
            if (!res) return;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            srv.TextureCube.MipLevels = mipLevels;
            srv.TextureCube.MostDetailedMip = 0;
            srv.TextureCube.ResourceMinLODClamp = 0.0f;

            D3D12_CPU_DESCRIPTOR_HANDLE cpu = GBufferSRVHeap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += SIZE_T(index) * GBufferSRVDescriptorSize;
            device->CreateShaderResourceView(res, &srv, cpu);
        };

        createSRV(GBuffer0.Get(), GBuffer0Format, 0);
        createSRV(GBuffer1.Get(), GBuffer1Format, 1);
        createSRV(GBuffer2.Get(), GBuffer2Format, 2);
        createSRV(LumenHistory[0].Get(), LumenHistoryFormat, 3);
        createSRV(LumenHistory[1].Get(), LumenHistoryFormat, 4);
        if (ShadowMap)
            createSRV(ShadowMap.Get(), DXGI_FORMAT_R32_FLOAT, kDesc_ShadowMap);
        if (SkyPrefilter)
            createCubeSRV(SkyPrefilter.Get(), SkyIBLFormat, kSkyIBLMipCount, kDesc_SkyPrefilter);

        createSRV(HWRTGIHistory[0].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kDesc_HWRTGIHistory0);
        createSRV(HWRTGIHistory[1].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kDesc_HWRTGIHistory1);
        createSRV(HWRTGIMeta[0].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kDesc_HWRTGIMeta0);
        createSRV(HWRTGIMeta[1].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kDesc_HWRTGIMeta1);
        createSRV(HWRTGIFiltered.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kDesc_HWRTGIFiltered);
        createSRV(LumenSwrtSurface[0].Get(), LumenSwrtSurfaceFormat, kDesc_LumenSWRTSurface0);
        createSRV(LumenSwrtSurface[1].Get(), LumenSwrtSurfaceFormat, kDesc_LumenSWRTSurface1);

        auto createStructuredBufferSRV = [&](ID3D12Resource* res, uint32 elementCount, uint32 strideBytes, uint32 index)
        {
            if (!res) return;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = DXGI_FORMAT_UNKNOWN;
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Buffer.FirstElement = 0;
            srv.Buffer.NumElements = elementCount;
            srv.Buffer.StructureByteStride = strideBytes;
            srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

            D3D12_CPU_DESCRIPTOR_HANDLE cpu = GBufferSRVHeap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += SIZE_T(index) * GBufferSRVDescriptorSize;
            device->CreateShaderResourceView(res, &srv, cpu);
        };

        auto createRawBufferSRV = [&](ID3D12Resource* res, uint32 numDwords, uint32 index)
        {
            if (!res) return;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = DXGI_FORMAT_R32_TYPELESS;
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Buffer.FirstElement = 0;
            srv.Buffer.NumElements = numDwords;
            srv.Buffer.StructureByteStride = 0;
            srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;

            D3D12_CPU_DESCRIPTOR_HANDLE cpu = GBufferSRVHeap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += SIZE_T(index) * GBufferSRVDescriptorSize;
            device->CreateShaderResourceView(res, &srv, cpu);
        };

        if (SkySH)
            createStructuredBufferSRV(SkySH.Get(), 9, sizeof(DirectX::XMFLOAT4), kDesc_SkySH);

        // Mesh buffer SRVs for HWRT shading (static).
        createStructuredBufferSRV(MeshSphere.VertexBuffer.Get(), MeshSphere.VBView.SizeInBytes / sizeof(FVertex), sizeof(FVertex), kDesc_SphereVB);
        createRawBufferSRV(MeshSphere.IndexBuffer.Get(), (MeshSphere.IBView.SizeInBytes + 3) / 4, kDesc_SphereIB);

        createStructuredBufferSRV(MeshBox.VertexBuffer.Get(), MeshBox.VBView.SizeInBytes / sizeof(FVertex), sizeof(FVertex), kDesc_BoxVB);
        createRawBufferSRV(MeshBox.IndexBuffer.Get(), (MeshBox.IBView.SizeInBytes + 3) / 4, kDesc_BoxIB);

        createStructuredBufferSRV(MeshCone.VertexBuffer.Get(), MeshCone.VBView.SizeInBytes / sizeof(FVertex), sizeof(FVertex), kDesc_ConeVB);
        createRawBufferSRV(MeshCone.IndexBuffer.Get(), (MeshCone.IBView.SizeInBytes + 3) / 4, kDesc_ConeIB);

        // UAVs for HWRT GI compute.
        auto createUAV = [&](ID3D12Resource* res, DXGI_FORMAT format, uint32 index)
        {
            if (!res) return;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = format;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Texture2D.MipSlice = 0;
            uav.Texture2D.PlaneSlice = 0;

            D3D12_CPU_DESCRIPTOR_HANDLE cpu = GBufferSRVHeap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += SIZE_T(index) * GBufferSRVDescriptorSize;
            device->CreateUnorderedAccessView(res, nullptr, &uav, cpu);
        };

        createUAV(HWRTGIHistory[0].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kUav_HWRTGIHistory0);
        createUAV(HWRTGIHistory[1].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kUav_HWRTGIHistory1);
        createUAV(HWRTGIMeta[0].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kUav_HWRTGIMeta0);
        createUAV(HWRTGIMeta[1].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kUav_HWRTGIMeta1);
        createUAV(HWRTGIFiltered.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kUav_HWRTGIFiltered);
        createUAV(LumenSwrtSurface[0].Get(), LumenSwrtSurfaceFormat, kUav_LumenSWRTSurface0);
        createUAV(LumenSwrtSurface[1].Get(), LumenSwrtSurfaceFormat, kUav_LumenSWRTSurface1);

        GBufferSRVBaseGPU = GBufferSRVHeap->GetGPUDescriptorHandleForHeapStart();
    }

    GBufferStates[0] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    GBufferStates[1] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    GBufferStates[2] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    LumenHistoryStates[0] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    LumenHistoryStates[1] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    LumenSwrtSurfaceStates[0] = D3D12_RESOURCE_STATE_GENERIC_READ;
    LumenSwrtSurfaceStates[1] = D3D12_RESOURCE_STATE_GENERIC_READ;
}

/**
 * @brief 准备 HWRT 场景数据（TLAS/实例描述与对象数据）。
 * @param rhi 渲染硬件接口。
 * @param frameIndex 当前帧索引。
 * @param objects 场景对象列表。
 * @param previewPos 预览对象位置（可空）。
 * @param previewType 预览对象类型。
 * @return 实例数量（0 表示未启用或无实例）。
 * @note 阶段：HWRT GI 准备阶段。
 */
uint32 FSimpleSceneRenderer::PrepareRaytracingScene(
    FD3D12RHI& rhi,
    uint32 frameIndex,
    const std::vector<FSceneObject>& objects,
    const DirectX::XMFLOAT3* previewPos,
    FSceneObject::EType previewType)
{
    if (!bRaytracingSupported)
        return 0;

    ID3D12Device* device = rhi.GetDevice();
    if (!device)
        return 0;

    ComPtr<ID3D12Device5> device5;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5))))
        return 0;

    const uint32 drawCount = (uint32)std::min<size_t>(objects.size(), MaxObjects);
    const bool bHasPreview = (previewPos && drawCount < MaxObjects && IsProceduralSceneObject(previewType));
    uint32 instanceCount = bHasPreview ? 1u : 0u;
    for (uint32 i = 0; i < drawCount; ++i)
    {
        if (IsProceduralSceneObject(objects[i].Type))
        {
            ++instanceCount;
        }
    }
    if (instanceCount == 0)
        return 0;

    const uint32 capacity = MaxObjects + 1; // include a potential preview instance

    // 确保每帧对象数据缓冲（上传堆，常驻映射）。
    {
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
                "CreateCommittedResource RTObjectBuffer failed");

            void* mapped = nullptr;
            D3D12_RANGE readRange{ 0, 0 };
            ThrowIfFailed(RTObjectBuffer[frameIndex]->Map(0, &readRange, &mapped), "RTObjectBuffer Map failed");
            RTObjectMapped[frameIndex] = static_cast<uint8*>(mapped);
            std::memset(RTObjectMapped[frameIndex], 0, (size_t)bytes);
        }
    }

    // 确保每帧实例描述缓冲（上传堆，常驻映射）。
    {
        const UINT64 bytes = UINT64(sizeof(D3D12_RAYTRACING_INSTANCE_DESC)) * UINT64(capacity);
        auto& rf = RTFrame[frameIndex];
        if (!rf.InstanceDescsUpload)
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
                IID_PPV_ARGS(&rf.InstanceDescsUpload)),
                "CreateCommittedResource TLAS InstanceDescsUpload failed");

            void* mapped = nullptr;
            D3D12_RANGE rr{ 0, 0 };
            ThrowIfFailed(rf.InstanceDescsUpload->Map(0, &rr, &mapped), "TLAS InstanceDescsUpload Map failed");
            rf.InstanceDescsMapped = static_cast<uint8*>(mapped);
            std::memset(rf.InstanceDescsMapped, 0, (size_t)bytes);

            rf.MaxInstances = capacity;
        }
    }

    // 确保每帧 TLAS 与 Scratch 缓冲（默认堆）。
    {
        auto& rf = RTFrame[frameIndex];
        if (!rf.TLAS || !rf.Scratch)
        {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            inputs.NumDescs = rf.MaxInstances;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
            device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
            if (info.ResultDataMaxSizeInBytes == 0 || info.ScratchDataSizeInBytes == 0)
                return 0;

            D3D12_HEAP_PROPERTIES heapDefault{};
            heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

            auto makeBuffer = [&](UINT64 size, D3D12_RESOURCE_STATES initial, ComPtr<ID3D12Resource>& outRes, const char* debugName)
            {
                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                desc.Width = size;
                desc.Height = 1;
                desc.DepthOrArraySize = 1;
                desc.MipLevels = 1;
                desc.SampleDesc.Count = 1;
                desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                ThrowIfFailed(device->CreateCommittedResource(
                    &heapDefault,
                    D3D12_HEAP_FLAG_NONE,
                    &desc,
                    initial,
                    nullptr,
                    IID_PPV_ARGS(&outRes)),
                    debugName);
            };

            makeBuffer(info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, rf.Scratch, "CreateCommittedResource TLAS scratch failed");
            makeBuffer(info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, rf.TLAS, "CreateCommittedResource TLAS failed");
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

    auto getBLAS = [&](FSceneObject::EType t) -> ID3D12Resource*
    {
        switch (t)
        {
        case FSceneObject::EType::Sphere: return RTMeshSphere.BLAS.Get();
        case FSceneObject::EType::Box: return RTMeshBox.BLAS.Get();
        case FSceneObject::EType::Cone: return RTMeshCone.BLAS.Get();
        default: return RTMeshSphere.BLAS.Get();
        }
    };

    // 填充实例数据与 TLAS 实例描述。
    auto* inst = reinterpret_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(RTFrame[frameIndex].InstanceDescsMapped);
    if (!inst || !RTObjectMapped[frameIndex])
        return 0;

    auto writeInstance = [&](uint32 instanceIndex, const FSceneObject& obj)
    {
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

        D3D12_RAYTRACING_INSTANCE_DESC id{};
        id.InstanceID = instanceIndex;
        id.InstanceContributionToHitGroupIndex = 0;
        id.InstanceMask = 0xFF;
        id.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;

        const float tx = obj.Position.x;
        const float ty = obj.Position.y;
        const float tz = obj.Position.z;
        const float sx = obj.Scale.x;
        const float sy = obj.Scale.y;
        const float sz = obj.Scale.z;
        const float m[3][4] = {
            { sx,   0.0f, 0.0f, tx },
            { 0.0f, sy,   0.0f, ty },
            { 0.0f, 0.0f, sz,   tz },
        };
        std::memcpy(id.Transform, m, sizeof(id.Transform));

        ID3D12Resource* blas = getBLAS(obj.Type);
        id.AccelerationStructure = blas ? blas->GetGPUVirtualAddress() : 0;
        inst[instanceIndex] = id;
    };

    uint32 instanceIndex = 0;
    for (uint32 i = 0; i < drawCount; ++i)
    {
        if (IsProceduralSceneObject(objects[i].Type))
        {
            writeInstance(instanceIndex++, objects[i]);
        }
    }

    if (bHasPreview)
    {
        // 追加预览对象实例。
        FSceneObject preview{};
        preview.Type = previewType;
        preview.Position = *previewPos;
        preview.Albedo = { 0.35f, 0.9f, 0.35f };
        preview.Metallic = 0.0f;
        preview.Roughness = 0.6f;
        writeInstance(instanceIndex++, preview);
    }

    RTObjectInstanceCount[frameIndex] = instanceIndex;
    return instanceIndex;
}

/**
 * @brief 录制 TLAS 构建命令。
 * @param cmd 命令列表。
 * @param frameIndex 帧索引。
 * @param instanceCount 实例数量。
 * @return 无返回值。
 * @note 阶段：HWRT GI 构建阶段。
 */
void FSimpleSceneRenderer::RecordBuildTLAS(ID3D12GraphicsCommandList* cmd, uint32 frameIndex, uint32 instanceCount)
{
    if (!bRaytracingSupported || instanceCount == 0)
        return;

    auto& rf = RTFrame[frameIndex];
    if (!rf.TLAS || !rf.Scratch || !rf.InstanceDescsUpload)
        return;

    ComPtr<ID3D12GraphicsCommandList4> cl4;
    if (FAILED(cmd->QueryInterface(IID_PPV_ARGS(&cl4))))
        return;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = instanceCount;
    inputs.InstanceDescs = rf.InstanceDescsUpload->GetGPUVirtualAddress();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = inputs;
    build.ScratchAccelerationStructureData = rf.Scratch->GetGPUVirtualAddress();
    build.DestAccelerationStructureData = rf.TLAS->GetGPUVirtualAddress();
    build.SourceAccelerationStructureData = 0;

    cl4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = rf.TLAS.Get();
    cl4->ResourceBarrier(1, &uav);
}

/**
 * @brief 渲染一帧场景（创建视图并选择渲染路径）。
 * @param rhi 渲染硬件接口。
 * @param camera 相机参数。
 * @param timeSeconds 当前时间（秒）。
 * @param renderPath 渲染路径。
 * @param bEnableLumen 是否启用 Lumen。
 * @param bEnableLumenSWRT 是否启用 SWRT GI。
 * @param bEnableLumenHWRT 是否启用 HWRT GI。
 * @param objects 场景对象列表。
 * @param selectedIndex 选中对象索引。
 * @param bScaleGizmo Gizmo 是否为缩放模式。
 * @param lightDirWs 光源方向。
 * @param bEnableTonemap 是否启用 Tonemap。
 * @param sunIntensity 太阳强度。
 * @param sky 天空参数。
 * @param leftInsetPx 左侧 UI 预留像素。
 * @param previewPos 预览位置（可空）。
 * @param previewType 预览对象类型。
 * @return 无返回值。
 * @note 阶段：渲染帧执行阶段。
 */
void FSimpleSceneRenderer::Render(
    FD3D12RHI& rhi,
    const FCamera& camera,
    float timeSeconds,
    ERenderPath renderPath,
    bool bEnableLumen,
    bool bEnableLumenSWRT,
    bool bEnableLumenHWRT,
    const std::vector<FSceneObject>& objects,
    int selectedIndex,
    bool bScaleGizmo,
    const DirectX::XMFLOAT3& lightDirWs,
    bool bEnableTonemap,
    float sunIntensity,
    const FSkyAtmosphereSettings& sky,
    int leftInsetPx,
    const DirectX::XMFLOAT3* previewPos,
    FSceneObject::EType previewType,
    int viewportX,
    int viewportY,
    int viewportWidth,
    int viewportHeight,
    std::function<void(ID3D12GraphicsCommandList*)> postSceneUiPass)
{
    // 构建视图族参数。
    FSceneViewFamily viewFamily{};
    viewFamily.RHI = &rhi;
    viewFamily.bEnableTonemap = bEnableTonemap;
    viewFamily.bEnableLumen = bEnableLumen;
    viewFamily.bEnableLumenSWRT = bEnableLumenSWRT;
    viewFamily.bEnableLumenHWRT = bEnableLumenHWRT;
    viewFamily.SunIntensity = sunIntensity;
    viewFamily.Sky = &sky;
    viewFamily.LeftInsetPx = leftInsetPx;
    viewFamily.ViewportX = viewportX;
    viewFamily.ViewportY = viewportY;
    viewFamily.ViewportWidth = viewportWidth;
    viewFamily.ViewportHeight = viewportHeight;
    viewFamily.PostSceneUiPass = std::move(postSceneUiPass);

    // 构建单视图参数。
    FSceneView view{};
    view.Camera = &camera;
    view.LightDirWs = lightDirWs;
    view.TimeSeconds = timeSeconds;

    // 构建渲染输入。
    FSceneRendererInputs inputs{};
    inputs.Objects = &objects;
    inputs.SelectedIndex = selectedIndex;
    inputs.bScaleGizmo = bScaleGizmo;
    inputs.PreviewPos = previewPos;
    inputs.PreviewType = previewType;

    // 根据渲染路径选择场景渲染器。
    if (renderPath == ERenderPath::Deferred)
    {
        FDeferredShadingSceneRenderer sceneRenderer(*this, viewFamily, view, inputs);
        sceneRenderer.Render();
    }
    else
    {
        FForwardShadingSceneRenderer sceneRenderer(*this, viewFamily, view, inputs);
        sceneRenderer.Render();
    }
}
