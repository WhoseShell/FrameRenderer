#include "RenderDoc/CaptureRockRenderer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <iterator>
#include <sstream>
#include <stdexcept>

#include <d3dcompiler.h>

#include "Core/Diagnostics.h"
#include "RenderDoc/CaptureManifest.h"
#include "RenderDoc/DdsLoader.h"
#include "RenderDoc/Json.h"

namespace rdcimport
{
namespace
{
D3D12_RESOURCE_DESC MakeBufferDesc(uint64 byteSize)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (std::max<uint64>)(1, byteSize);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

uint32 Align256(uint32 size)
{
    return (size + 255u) & ~255u;
}

float ReadFloat(const std::vector<std::byte>& bytes, size_t offset)
{
    float value = 0.0f;
    std::memcpy(&value, bytes.data() + offset, sizeof(float));
    return value;
}

uint32 ReadUInt32(const std::vector<std::byte>& bytes, size_t offset)
{
    uint32 value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(uint32));
    return value;
}

const FJsonValue& RequireMember(const FJsonValue& object, std::string_view key)
{
    const FJsonValue* value = object.Find(key);
    if (!value)
    {
        throw std::runtime_error("missing JSON field: " + std::string(key));
    }
    return *value;
}

std::string ReadString(const FJsonValue& object, std::string_view key)
{
    const FJsonValue& value = RequireMember(object, key);
    if (!value.IsString())
    {
        throw std::runtime_error("JSON field is not a string: " + std::string(key));
    }
    return std::string(value.AsString());
}

double ReadNumber(const FJsonValue& object, std::string_view key)
{
    const FJsonValue& value = RequireMember(object, key);
    if (!value.IsNumber())
    {
        throw std::runtime_error("JSON field is not a number: " + std::string(key));
    }
    return value.AsNumber();
}

float ReadFloatMember(const FJsonValue& object, std::string_view key)
{
    return static_cast<float>(ReadNumber(object, key));
}

uint32 ReadUIntMember(const FJsonValue& object, std::string_view key)
{
    return static_cast<uint32>(ReadNumber(object, key));
}

uint64 ReadUInt64Member(const FJsonValue& object, std::string_view key)
{
    return static_cast<uint64>(ReadNumber(object, key));
}

DirectX::XMFLOAT3 ReadFloat3(const FJsonValue& object, std::string_view key)
{
    const FJsonValue& value = RequireMember(object, key);
    if (!value.IsArray() || value.AsArray().size() != 3)
    {
        throw std::runtime_error("JSON field is not a float3: " + std::string(key));
    }
    return DirectX::XMFLOAT3(
        static_cast<float>(value.AsArray()[0].AsNumber()),
        static_cast<float>(value.AsArray()[1].AsNumber()),
        static_cast<float>(value.AsArray()[2].AsNumber()));
}

DirectX::XMFLOAT3 NormalizeOrUp(const DirectX::XMFLOAT3& value)
{
    using namespace DirectX;
    XMVECTOR v = XMLoadFloat3(&value);
    const float lenSq = XMVectorGetX(XMVector3LengthSq(v));
    if (lenSq < 1.0e-8f)
    {
        return XMFLOAT3(0.0f, 1.0f, 0.0f);
    }
    XMFLOAT3 out{};
    XMStoreFloat3(&out, XMVector3Normalize(v));
    return out;
}

D3D12_RESOURCE_DESC MakeTextureDesc(const FDdsImage& image)
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

void CopyBytesToUploadBuffer(ID3D12Resource* resource, const void* source, size_t byteSize, const char* label)
{
    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, 0 };
    ThrowIfFailed(resource->Map(0, &readRange, &mapped), label);
    std::memcpy(mapped, source, byteSize);
    resource->Unmap(0, nullptr);
}

Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(const std::filesystem::path& path, const char* entry, const char* target)
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompileFromFile(
        path.wstring().c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry,
        target,
        flags,
        0,
        &bytecode,
        &errors);

    if (errors)
    {
        std::string message(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        DebugOutput(std::wstring(message.begin(), message.end()));
    }

    if (FAILED(hr))
    {
        throw std::runtime_error("failed to compile shader " + WideToUtf8(path.wstring()) + " entry " + entry);
    }
    return bytecode;
}
}

bool FCaptureRockRenderer::Initialize(
    FD3D12RHI& rhi,
    DXGI_FORMAT colorFormat,
    DXGI_FORMAT depthFormat,
    const std::filesystem::path& manifestPath,
    const std::filesystem::path& shaderPath,
    std::string* error)
{
    Reset();
    Device_ = rhi.GetDevice();
    ShaderPath_ = shaderPath;

    try
    {
        if (!Device_)
        {
            throw std::runtime_error("D3D12 device is null");
        }
        LoadManifest(manifestPath);
        LoadMesh(Device_.Get());
        LoadTextures(rhi);
        CreateRootSignature(Device_.Get());
        CreatePipelineState(Device_.Get(), colorFormat, depthFormat);
        CreateConstantBuffers(Device_.Get());
        bLoaded_ = true;
        return true;
    }
    catch (const std::exception& ex)
    {
        if (error)
        {
            *error = ex.what();
        }
        Reset();
        return false;
    }
}

void FCaptureRockRenderer::Reset()
{
    for (uint32 i = 0; i < FD3D12RHI::kFrameCount; ++i)
    {
        if (ConstantBuffers_[i] && ConstantBufferMapped_[i])
        {
            ConstantBuffers_[i]->Unmap(0, nullptr);
        }
        ConstantBufferMapped_[i] = nullptr;
        ConstantBuffers_[i].Reset();
    }

    VertexBuffer_.Reset();
    IndexBuffer_.Reset();
    RootSignature_.Reset();
    PipelineState_.Reset();
    SrvHeap_.Reset();
    for (FTextureResource& texture : Textures_)
    {
        texture = {};
    }
    LoadedTextureCount_ = 0;
    Device_.Reset();
    bLoaded_ = false;
}

void FCaptureRockRenderer::LoadManifest(const std::filesystem::path& manifestPath)
{
    ManifestPath_ = manifestPath;
    BaseDirectory_ = manifestPath.parent_path();

    const FJsonValue root = ParseJson(ReadTextFile(manifestPath));
    const FJsonValue& event = RequireMember(root, "event");
    EventId_ = ReadUIntMember(event, "event_id");
    DrawId_ = ReadUIntMember(event, "draw_id");

    const FJsonValue& mesh = RequireMember(root, "mesh");
    PositionsPath_ = Utf8ToWide(ReadString(mesh, "positions"));
    NormalsPath_ = Utf8ToWide(ReadString(mesh, "normals"));
    Uv0Path_ = Utf8ToWide(ReadString(mesh, "uv0"));
    Color0Path_ = Utf8ToWide(ReadString(mesh, "color0"));
    IndicesPath_ = Utf8ToWide(ReadString(mesh, "indices"));
    VertexCount_ = ReadUIntMember(mesh, "vertex_count");
    IndexCount_ = ReadUIntMember(mesh, "index_count");
    BoundsMinCapture_ = ReadFloat3(mesh, "bounds_min_capture");
    BoundsMaxCapture_ = ReadFloat3(mesh, "bounds_max_capture");

    if (const FJsonValue* placement = root.Find("placement"))
    {
        BasePositionWorld_ = ReadFloat3(*placement, "base_position_world");
        UniformScale_ = ReadFloatMember(*placement, "uniform_scale");
        if (const FJsonValue* sitOnGround = placement->Find("sit_on_ground"); sitOnGround && sitOnGround->IsBool())
        {
            bSitOnGround_ = sitOnGround->AsBool();
        }
    }

    const FJsonValue& lighting = RequireMember(root, "lighting");
    SunDirectionEngine_ = NormalizeOrUp(ReadFloat3(lighting, "sun_direction_engine"));
    SunColor_ = ReadFloat3(lighting, "sun_color");
    SunIntensity_ = ReadFloatMember(lighting, "sun_intensity");
    AmbientIntensity_ = ReadFloatMember(lighting, "ambient_intensity");

    const FJsonValue& material = RequireMember(root, "material");
    Roughness_ = ReadFloatMember(material, "roughness");
    Metallic_ = ReadFloatMember(material, "metallic");
    NormalStrength_ = ReadFloatMember(material, "normal_strength");
    BaseColorBoost_ = ReadFloatMember(material, "base_color_boost");

    const FJsonValue& slots = RequireMember(material, "texture_slots");
    if (!slots.IsArray())
    {
        throw std::runtime_error("material.texture_slots is not an array");
    }

    for (FTextureSlot& slot : TextureSlots_)
    {
        slot = {};
    }
    for (const FJsonValue& slotValue : slots.AsArray())
    {
        const uint32 slotIndex = ReadUIntMember(slotValue, "slot");
        if (slotIndex >= kTextureSlots)
        {
            throw std::runtime_error("texture slot out of range");
        }
        FTextureSlot& slot = TextureSlots_[slotIndex];
        slot.Slot = slotIndex;
        slot.ResourceId = ReadUInt64Member(slotValue, "resource_id");
        slot.OriginalBindPoint = ReadUIntMember(slotValue, "original_bind_point");
        slot.OriginalTableIndex = ReadUIntMember(slotValue, "original_table_index");
        slot.Role = ReadString(slotValue, "role");
        slot.RelativePath = Utf8ToWide(ReadString(slotValue, "path"));
    }
}

std::filesystem::path FCaptureRockRenderer::ResolveAssetPath(const std::filesystem::path& relativePath) const
{
    if (relativePath.is_absolute())
    {
        return relativePath;
    }
    return BaseDirectory_ / relativePath;
}

void FCaptureRockRenderer::LoadMesh(ID3D12Device* device)
{
    const std::vector<std::byte> positions = ReadBinaryFile(ResolveAssetPath(PositionsPath_));
    const std::vector<std::byte> normals = ReadBinaryFile(ResolveAssetPath(NormalsPath_));
    const std::vector<std::byte> uv0 = ReadBinaryFile(ResolveAssetPath(Uv0Path_));
    const std::vector<std::byte> color0 = ReadBinaryFile(ResolveAssetPath(Color0Path_));
    const std::vector<std::byte> indices = ReadBinaryFile(ResolveAssetPath(IndicesPath_));

    if (positions.size() != static_cast<size_t>(VertexCount_) * sizeof(float) * 3
        || normals.size() != static_cast<size_t>(VertexCount_) * sizeof(float) * 3
        || uv0.size() != static_cast<size_t>(VertexCount_) * sizeof(float) * 2
        || color0.size() != static_cast<size_t>(VertexCount_) * sizeof(float) * 4
        || indices.size() != static_cast<size_t>(IndexCount_) * sizeof(uint32))
    {
        throw std::runtime_error("rock mesh buffer sizes do not match manifest counts");
    }

    const float centerX = 0.5f * (BoundsMinCapture_.x + BoundsMaxCapture_.x);
    const float centerY = 0.5f * (BoundsMinCapture_.y + BoundsMaxCapture_.y);
    const float centerZ = 0.5f * (BoundsMinCapture_.z + BoundsMaxCapture_.z);

    std::vector<FVertex> vertices(VertexCount_);
    BoundsMinEngine_ = DirectX::XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
    BoundsMaxEngine_ = DirectX::XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (uint32 i = 0; i < VertexCount_; ++i)
    {
        const size_t posOffset = static_cast<size_t>(i) * sizeof(float) * 3;
        const size_t uvOffset = static_cast<size_t>(i) * sizeof(float) * 2;
        const size_t colorOffset = static_cast<size_t>(i) * sizeof(float) * 4;

        const float px = ReadFloat(positions, posOffset + sizeof(float) * 0);
        const float py = ReadFloat(positions, posOffset + sizeof(float) * 1);
        const float pz = ReadFloat(positions, posOffset + sizeof(float) * 2);
        const float nx = ReadFloat(normals, posOffset + sizeof(float) * 0);
        const float ny = ReadFloat(normals, posOffset + sizeof(float) * 1);
        const float nz = ReadFloat(normals, posOffset + sizeof(float) * 2);

        FVertex& vertex = vertices[i];
        vertex.Position = DirectX::XMFLOAT3(px - centerX, pz - centerZ, py - centerY);
        vertex.Normal = NormalizeOrUp(DirectX::XMFLOAT3(nx, nz, ny));
        vertex.UV = DirectX::XMFLOAT2(
            ReadFloat(uv0, uvOffset + sizeof(float) * 0),
            ReadFloat(uv0, uvOffset + sizeof(float) * 1));
        vertex.Color = DirectX::XMFLOAT4(
            ReadFloat(color0, colorOffset + sizeof(float) * 0),
            ReadFloat(color0, colorOffset + sizeof(float) * 1),
            ReadFloat(color0, colorOffset + sizeof(float) * 2),
            ReadFloat(color0, colorOffset + sizeof(float) * 3));

        BoundsMinEngine_.x = (std::min)(BoundsMinEngine_.x, vertex.Position.x);
        BoundsMinEngine_.y = (std::min)(BoundsMinEngine_.y, vertex.Position.y);
        BoundsMinEngine_.z = (std::min)(BoundsMinEngine_.z, vertex.Position.z);
        BoundsMaxEngine_.x = (std::max)(BoundsMaxEngine_.x, vertex.Position.x);
        BoundsMaxEngine_.y = (std::max)(BoundsMaxEngine_.y, vertex.Position.y);
        BoundsMaxEngine_.z = (std::max)(BoundsMaxEngine_.z, vertex.Position.z);
    }

    const float extentX = BoundsMaxEngine_.x - BoundsMinEngine_.x;
    const float extentY = BoundsMaxEngine_.y - BoundsMinEngine_.y;
    const float extentZ = BoundsMaxEngine_.z - BoundsMinEngine_.z;
    BoundsRadius_ = (std::max)(0.5f, std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ) * 0.5f);

    std::vector<uint32> indexValues(IndexCount_);
    for (uint32 i = 0; i < IndexCount_; ++i)
    {
        indexValues[i] = ReadUInt32(indices, static_cast<size_t>(i) * sizeof(uint32));
    }

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    const uint64 vertexBytes = static_cast<uint64>(vertices.size() * sizeof(FVertex));
    const D3D12_RESOURCE_DESC vertexDesc = MakeBufferDesc(vertexBytes);
    ThrowIfFailed(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &vertexDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&VertexBuffer_)), "Create renderdoc rock vertex buffer failed");
    CopyBytesToUploadBuffer(VertexBuffer_.Get(), vertices.data(), static_cast<size_t>(vertexBytes), "Map renderdoc rock vertex buffer failed");

    const uint64 indexBytes = static_cast<uint64>(indexValues.size() * sizeof(uint32));
    const D3D12_RESOURCE_DESC indexDesc = MakeBufferDesc(indexBytes);
    ThrowIfFailed(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &indexDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&IndexBuffer_)), "Create renderdoc rock index buffer failed");
    CopyBytesToUploadBuffer(IndexBuffer_.Get(), indexValues.data(), static_cast<size_t>(indexBytes), "Map renderdoc rock index buffer failed");

    VertexBufferView_.BufferLocation = VertexBuffer_->GetGPUVirtualAddress();
    VertexBufferView_.SizeInBytes = static_cast<UINT>(vertexBytes);
    VertexBufferView_.StrideInBytes = sizeof(FVertex);
    IndexBufferView_.BufferLocation = IndexBuffer_->GetGPUVirtualAddress();
    IndexBufferView_.SizeInBytes = static_cast<UINT>(indexBytes);
    IndexBufferView_.Format = DXGI_FORMAT_R32_UINT;
}

void FCaptureRockRenderer::LoadTextures(FD3D12RHI& rhi)
{
    ID3D12Device* device = rhi.GetDevice();

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = kTextureSlots;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&SrvHeap_)), "Create renderdoc rock SRV heap failed");
    SrvDescriptorSize_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    LoadedTextureCount_ = 0;
    for (uint32 slotIndex = 0; slotIndex < kTextureSlots; ++slotIndex)
    {
        const FTextureSlot& slot = TextureSlots_[slotIndex];
        if (slot.RelativePath.empty())
        {
            throw std::runtime_error("missing manifest texture slot " + std::to_string(slotIndex));
        }

        const FDdsImage image = LoadDdsImage(ResolveAssetPath(slot.RelativePath));
        const D3D12_RESOURCE_DESC textureDesc = MakeTextureDesc(image);

        FTextureResource texture;
        texture.Format = image.Format;
        texture.Dimension = image.Dimension;
        texture.Width = image.Width;
        texture.Height = image.Height;
        texture.Depth = image.Depth;
        texture.MipCount = image.MipCount;
        texture.ArraySize = image.ArraySize;

        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ThrowIfFailed(device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&texture.Resource)), "Create renderdoc rock texture failed");

        const UINT subresourceCount = static_cast<UINT>(image.Subresources.size());
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresourceCount);
        std::vector<UINT> rowCounts(subresourceCount);
        std::vector<UINT64> rowSizes(subresourceCount);
        UINT64 uploadSize = 0;
        device->GetCopyableFootprints(
            &textureDesc,
            0,
            subresourceCount,
            0,
            layouts.data(),
            rowCounts.data(),
            rowSizes.data(),
            &uploadSize);

        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        const D3D12_RESOURCE_DESC uploadDesc = MakeBufferDesc(uploadSize);
        ThrowIfFailed(device->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&texture.Upload)), "Create renderdoc rock texture upload failed");

        uint8* mapped = nullptr;
        ThrowIfFailed(texture.Upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map renderdoc rock texture upload failed");
        for (UINT i = 0; i < subresourceCount; ++i)
        {
            const FDdsSubresource& src = image.Subresources[i];
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
        texture.Upload->Unmap(0, nullptr);

        struct FUploadContext
        {
            ID3D12Resource* Texture = nullptr;
            ID3D12Resource* Upload = nullptr;
            const std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>* Layouts = nullptr;
            UINT SubresourceCount = 0;
        } ctx;
        ctx.Texture = texture.Resource.Get();
        ctx.Upload = texture.Upload.Get();
        ctx.Layouts = &layouts;
        ctx.SubresourceCount = subresourceCount;

        rhi.ExecuteImmediate([](ID3D12GraphicsCommandList* cmd, void* user)
        {
            auto* c = reinterpret_cast<FUploadContext*>(user);
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

        Textures_[slotIndex] = std::move(texture);
        CreateTextureSRV(device, Textures_[slotIndex], slotIndex);
        ++LoadedTextureCount_;
    }
}

void FCaptureRockRenderer::CreateTextureSRV(ID3D12Device* device, const FTextureResource& texture, uint32 slot)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = texture.Format;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    if (texture.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv.Texture3D.MipLevels = texture.MipCount;
    }
    else if (texture.ArraySize > 1)
    {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Texture2DArray.ArraySize = texture.ArraySize;
        srv.Texture2DArray.MipLevels = texture.MipCount;
    }
    else
    {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = texture.MipCount;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = SrvHeap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot) * SrvDescriptorSize_;
    device->CreateShaderResourceView(texture.Resource.Get(), &srv, handle);
}

void FCaptureRockRenderer::CreateRootSignature(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = kTextureSlots;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MipLODBias = 0.0f;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 2;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (error)
    {
        std::string message(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize());
        DebugOutput(std::wstring(message.begin(), message.end()));
    }
    ThrowIfFailed(hr, "Serialize renderdoc rock root signature failed");
    ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&RootSignature_)),
        "Create renderdoc rock root signature failed");
}

void FCaptureRockRenderer::CreatePipelineState(ID3D12Device* device, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat)
{
    Microsoft::WRL::ComPtr<ID3DBlob> vs = CompileShader(ShaderPath_, "VSMain", "vs_5_0");
    Microsoft::WRL::ComPtr<ID3DBlob> ps = CompileShader(ShaderPath_, "PSMain", "ps_5_0");

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.FrontCounterClockwise = FALSE;
    rasterizer.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = RootSignature_.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.BlendState = blend;
    pso.RasterizerState = rasterizer;
    pso.DepthStencilState = depth;
    pso.SampleMask = UINT_MAX;
    pso.InputLayout = { inputLayout, static_cast<UINT>(std::size(inputLayout)) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = colorFormat;
    pso.DSVFormat = depthFormat;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&PipelineState_)),
        "Create renderdoc rock PSO failed");
}

void FCaptureRockRenderer::CreateConstantBuffers(ID3D12Device* device)
{
    ConstantBufferSize_ = Align256(sizeof(FConstants));
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const D3D12_RESOURCE_DESC desc = MakeBufferDesc(ConstantBufferSize_);

    for (uint32 i = 0; i < FD3D12RHI::kFrameCount; ++i)
    {
        ThrowIfFailed(device->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&ConstantBuffers_[i])), "Create renderdoc rock constant buffer failed");

        D3D12_RANGE readRange{ 0, 0 };
        void* mapped = nullptr;
        ThrowIfFailed(ConstantBuffers_[i]->Map(0, &readRange, &mapped), "Map renderdoc rock constant buffer failed");
        ConstantBufferMapped_[i] = static_cast<uint8*>(mapped);
    }
}

void FCaptureRockRenderer::UpdateConstants(const FCaptureRockFrameInputs& inputs)
{
    using namespace DirectX;
    const float scale = (std::max)(UniformScale_, 0.001f);
    XMFLOAT3 rockWorld = BasePositionWorld_;
    if (bSitOnGround_)
    {
        rockWorld.y -= BoundsMinEngine_.y * scale;
    }

    FConstants constants{};
    constants.ViewProj = inputs.ViewProj;
    constants.CameraPosition = XMFLOAT4(inputs.CameraPositionWs.x, inputs.CameraPositionWs.y, inputs.CameraPositionWs.z, 1.0f);
    constants.SunDirectionAndAmbient = XMFLOAT4(SunDirectionEngine_.x, SunDirectionEngine_.y, SunDirectionEngine_.z, AmbientIntensity_);
    constants.SunColorAndIntensity = XMFLOAT4(SunColor_.x, SunColor_.y, SunColor_.z, SunIntensity_);
    constants.MaterialParams = XMFLOAT4(Roughness_, Metallic_, NormalStrength_, BaseColorBoost_);
    constants.RockWorld = XMFLOAT4(rockWorld.x, rockWorld.y, rockWorld.z, scale);

    const uint32 frameIndex = inputs.FrameIndex % FD3D12RHI::kFrameCount;
    std::memcpy(ConstantBufferMapped_[frameIndex], &constants, sizeof(constants));
}

void FCaptureRockRenderer::Render(ID3D12GraphicsCommandList* cmd, const FCaptureRockFrameInputs& inputs)
{
    if (!bLoaded_ || !cmd)
    {
        return;
    }

    UpdateConstants(inputs);
    const uint32 frameIndex = inputs.FrameIndex % FD3D12RHI::kFrameCount;

    ID3D12DescriptorHeap* heaps[] = { SrvHeap_.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(RootSignature_.Get());
    cmd->SetPipelineState(PipelineState_.Get());
    cmd->RSSetViewports(1, &inputs.Viewport);
    cmd->RSSetScissorRects(1, &inputs.Scissor);
    cmd->OMSetRenderTargets(1, &inputs.TargetRTV, FALSE, &inputs.DepthDSV);
    cmd->SetGraphicsRootConstantBufferView(0, ConstantBuffers_[frameIndex]->GetGPUVirtualAddress());
    cmd->SetGraphicsRootDescriptorTable(1, SrvHeap_->GetGPUDescriptorHandleForHeapStart());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &VertexBufferView_);
    cmd->IASetIndexBuffer(&IndexBufferView_);
    cmd->DrawIndexedInstanced(IndexCount_, 1, 0, 0, 0);
}

std::wstring FCaptureRockRenderer::FormatSummary() const
{
    std::wstringstream out;
    out << L"event=" << EventId_
        << L" draw=" << DrawId_
        << L" vertices=" << VertexCount_
        << L" indices=" << IndexCount_
        << L" textures=" << LoadedTextureCount_
        << L" radius=" << BoundsRadius_;
    return out.str();
}
}
