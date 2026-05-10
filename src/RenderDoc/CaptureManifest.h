#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>

#include <filesystem>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "RenderDoc/Json.h"

namespace rdcimport
{
enum class ECaptureResourceKind
{
    Unknown,
    Buffer,
    Texture
};

struct FCaptureAssetEntry
{
    std::string LogicalName;
    uint64_t Id = 0;
    std::string Type;
    std::filesystem::path Path;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t Depth = 1;
    uint32_t Mips = 1;
    uint32_t Stride = 0;
    uint64_t ByteOffset = 0;
    uint64_t ByteLength = 0;
    uint32_t FirstElement = 0;
    uint32_t NumElements = 0;
    uint32_t SourceDraw = 0;
};

struct FCaptureInputElement
{
    std::string Semantic;
    uint32_t SemanticIndex = 0;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    uint32_t InputSlot = 0;
    uint32_t ByteOffset = 0;
    bool PerInstance = false;
    uint32_t InstanceStepRate = 0;
};

struct FCaptureBoundResource
{
    ECaptureResourceKind Kind = ECaptureResourceKind::Unknown;
    uint32_t RootIndex = 0;
    uint32_t BindPoint = 0;
    uint64_t ResourceId = 0;
    std::string ResourceName;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    std::filesystem::path Path;
    uint32_t ViewType = 0;
    uint32_t Slot = 0;
    uint32_t Stride = 0;
    uint32_t ByteOffset = 0;
    uint32_t ByteSize = 0;
    uint32_t FirstMip = 0;
    uint32_t NumMips = 1;
    uint32_t FirstSlice = 0;
    uint32_t NumSlices = 1;
    uint32_t FirstElement = 0;
    uint32_t NumElements = 0;
    uint32_t ElementByteSize = 0;
};

struct FCaptureShaderStage
{
    std::string Name;
    uint64_t ShaderId = 0;
    std::string EntryPoint;
    std::filesystem::path BytecodePath;
    std::filesystem::path DisassemblyPath;
    std::vector<FCaptureBoundResource> BoundConstantBuffers;
    std::vector<FCaptureBoundResource> BoundReadOnlyResources;
};

struct FCapturePassEvent
{
    uint32_t EventId = 0;
    uint32_t ActionId = 0;
    std::string Label;
    uint64_t PipelineStateId = 0;
    uint64_t RootSignatureId = 0;
    std::string TopologyName;
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    D3D12_VIEWPORT Viewport{};
    uint32_t DrawIndexInCommandList = 0;
    uint32_t CommandList = 0;
    uint32_t NumIndices = 0;
    uint32_t NumInstances = 1;
    uint32_t RenderTargetCount = 1;
    uint32_t BaseVertex = 0;
    uint32_t IndexOffset = 0;
    uint32_t VertexOffset = 0;
    uint32_t VertexCount = 0;
    std::string DrawChunkName;
    std::vector<FCaptureInputElement> InputLayout;
    std::vector<FCaptureBoundResource> VertexBuffers;
    std::optional<FCaptureBoundResource> IndexBuffer;
    std::vector<FCaptureBoundResource> RootViews;
    std::vector<FCaptureBoundResource> RootConstantBuffers;
    std::vector<FCaptureShaderStage> ShaderStages;
};

class FCaptureAssetManifest
{
public:
    void LoadFromFile(const std::filesystem::path& path);
    const std::vector<FCaptureAssetEntry>& Entries() const { return Entries_; }
    const std::filesystem::path& SourcePath() const { return SourcePath_; }
    const std::filesystem::path& BaseDirectory() const { return BaseDirectory_; }

private:
    std::filesystem::path SourcePath_;
    std::filesystem::path BaseDirectory_;
    std::vector<FCaptureAssetEntry> Entries_;
};

class FCapturePassManifest
{
public:
    void LoadFromFile(const std::filesystem::path& path);
    const std::vector<FCapturePassEvent>& Events() const { return Events_; }
    const std::filesystem::path& SourcePath() const { return SourcePath_; }
    const std::filesystem::path& BaseDirectory() const { return BaseDirectory_; }
    const std::filesystem::path& SourceCapture() const { return SourceCapture_; }
    bool Available() const { return Available_; }

private:
    std::filesystem::path ResolvePath(std::string_view path) const;

    std::filesystem::path SourcePath_;
    std::filesystem::path BaseDirectory_;
    std::filesystem::path SourceCapture_;
    bool Available_ = true;
    std::vector<FCapturePassEvent> Events_;
};

std::string ReadTextFile(const std::filesystem::path& path);
std::vector<std::byte> ReadBinaryFile(const std::filesystem::path& path);
std::wstring Utf8ToWide(std::string_view value);
std::string WideToUtf8(std::wstring_view value);
DXGI_FORMAT ParseDxgiFormat(std::string_view value);
std::string DxgiFormatName(DXGI_FORMAT format);
}
