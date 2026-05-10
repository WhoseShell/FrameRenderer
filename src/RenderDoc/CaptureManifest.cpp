#include "RenderDoc/CaptureManifest.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

#include "Core/Win32.h"

namespace rdcimport
{
namespace
{
const FJsonValue* FindAny(const FJsonValue& object, std::initializer_list<std::string_view> keys)
{
    for (std::string_view key : keys)
    {
        if (const FJsonValue* value = object.Find(key))
        {
            return value;
        }
    }
    return nullptr;
}

bool ReadBool(const FJsonValue* value, bool fallback = false)
{
    return value && value->IsBool() ? value->AsBool() : fallback;
}

uint32_t ReadUInt(const FJsonValue* value, uint32_t fallback = 0)
{
    return value && value->IsNumber() ? static_cast<uint32_t>(value->AsNumber()) : fallback;
}

uint64_t ReadUInt64(const FJsonValue* value, uint64_t fallback = 0)
{
    return value && value->IsNumber() ? static_cast<uint64_t>(value->AsNumber()) : fallback;
}

std::string ReadString(const FJsonValue* value, std::string_view fallback = {})
{
    return value && value->IsString() ? std::string(value->AsString()) : std::string(fallback);
}

std::string ToUpper(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

std::filesystem::path ResolveManifestPath(const std::filesystem::path& baseDir, std::string_view pathText)
{
    if (pathText.empty())
    {
        return {};
    }

    const std::filesystem::path parsed = Utf8ToWide(pathText);
    if (parsed.is_absolute())
    {
        return parsed;
    }

    std::error_code ec;
    std::filesystem::path probeBase = baseDir;
    std::filesystem::path firstCandidate = baseDir / parsed;
    for (int i = 0; i < 6 && !probeBase.empty(); ++i)
    {
        std::filesystem::path candidate = probeBase / parsed;
        if (std::filesystem::is_regular_file(candidate, ec) || std::filesystem::is_directory(candidate, ec))
        {
            return candidate;
        }

        const std::filesystem::path parent = probeBase.parent_path();
        if (parent.empty() || parent == probeBase)
        {
            break;
        }
        probeBase = parent;
    }

    return firstCandidate;
}

D3D12_VIEWPORT ReadViewport(const FJsonValue* value)
{
    D3D12_VIEWPORT viewport{};
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    if (!value || !value->IsObject())
    {
        return viewport;
    }

    viewport.TopLeftX = static_cast<float>(value->Find("x") && value->Find("x")->IsNumber() ? value->Find("x")->AsNumber() : 0.0);
    viewport.TopLeftY = static_cast<float>(value->Find("y") && value->Find("y")->IsNumber() ? value->Find("y")->AsNumber() : 0.0);
    viewport.Width = static_cast<float>(value->Find("width") && value->Find("width")->IsNumber() ? value->Find("width")->AsNumber() : 0.0);
    viewport.Height = static_cast<float>(value->Find("height") && value->Find("height")->IsNumber() ? value->Find("height")->AsNumber() : 0.0);
    viewport.MinDepth = static_cast<float>(value->Find("min_depth") && value->Find("min_depth")->IsNumber() ? value->Find("min_depth")->AsNumber() : 0.0);
    viewport.MaxDepth = static_cast<float>(value->Find("max_depth") && value->Find("max_depth")->IsNumber() ? value->Find("max_depth")->AsNumber() : 1.0);
    return viewport;
}

ECaptureResourceKind ReadKind(const FJsonValue& object, const std::filesystem::path& path)
{
    const std::string kind = ToUpper(ReadString(FindAny(object, { "kind", "type" })));
    if (kind == "BUFFER")
    {
        return ECaptureResourceKind::Buffer;
    }
    if (kind == "TEXTURE")
    {
        return ECaptureResourceKind::Texture;
    }
    if (const FJsonValue* isTexture = object.Find("is_texture"); isTexture && isTexture->IsBool())
    {
        return isTexture->AsBool() ? ECaptureResourceKind::Texture : ECaptureResourceKind::Buffer;
    }
    return ToUpper(path.extension().string()) == ".DDS" ? ECaptureResourceKind::Texture : ECaptureResourceKind::Unknown;
}

FCaptureBoundResource ReadBoundResource(const FJsonValue& value, const std::filesystem::path& baseDir)
{
    FCaptureBoundResource resource;
    if (!value.IsObject())
    {
        return resource;
    }

    const auto& object = value.AsObject();
    auto find = [&](std::string_view key) -> const FJsonValue* {
        const auto found = object.find(key);
        return found != object.end() ? &found->second : nullptr;
    };

    const std::string pathText = ReadString(find("path"));
    resource.Path = ResolveManifestPath(baseDir, pathText);
    resource.Kind = ReadKind(value, resource.Path);
    resource.RootIndex = ReadUInt(find("root_index"));
    resource.BindPoint = ReadUInt(FindAny(value, { "bind_point", "bind", "slot" }));
    resource.ResourceId = ReadUInt64(find("resource_id"));
    resource.ResourceName = ReadString(find("resource_name"));
    resource.Format = ParseDxgiFormat(ReadString(FindAny(value, { "view_format", "format" })));
    resource.ViewType = ReadUInt(find("view_type"));
    resource.Slot = ReadUInt(find("slot"));
    resource.Stride = ReadUInt(find("stride"));
    resource.ByteOffset = ReadUInt(find("byte_offset"));
    resource.ByteSize = ReadUInt(find("byte_size"));
    resource.FirstMip = ReadUInt(find("first_mip"));
    resource.NumMips = ReadUInt(find("num_mips"), 1);
    resource.FirstSlice = ReadUInt(find("first_slice"));
    resource.NumSlices = ReadUInt(find("num_slices"), 1);
    resource.FirstElement = ReadUInt(find("first_element"));
    resource.NumElements = ReadUInt(find("num_elements"));
    resource.ElementByteSize = ReadUInt(FindAny(value, { "element_byte_size", "stride" }));
    return resource;
}

void AppendBoundResourceArray(
    const FJsonValue* arrayValue,
    const std::filesystem::path& baseDir,
    std::vector<FCaptureBoundResource>& out)
{
    if (!arrayValue || !arrayValue->IsArray())
    {
        return;
    }

    for (const FJsonValue& item : arrayValue->AsArray())
    {
        if (item.IsObject())
        {
            out.push_back(ReadBoundResource(item, baseDir));
        }
    }
}

FCaptureInputElement ReadInputElement(const FJsonValue& value)
{
    FCaptureInputElement element;
    if (!value.IsObject())
    {
        return element;
    }

    element.Semantic = ReadString(value.Find("semantic"));
    element.SemanticIndex = ReadUInt(value.Find("semantic_index"));
    element.Format = ParseDxgiFormat(ReadString(value.Find("format")));
    element.InputSlot = ReadUInt(FindAny(value, { "input_slot", "slot" }));
    element.ByteOffset = ReadUInt(FindAny(value, { "byte_offset", "offset" }));
    element.PerInstance = ReadBool(value.Find("per_instance"));
    element.InstanceStepRate = ReadUInt(value.Find("instance_step_rate"));
    return element;
}

void AppendInputLayout(const FJsonValue* arrayValue, std::vector<FCaptureInputElement>& out)
{
    if (!arrayValue || !arrayValue->IsArray())
    {
        return;
    }

    for (const FJsonValue& item : arrayValue->AsArray())
    {
        if (item.IsObject())
        {
            out.push_back(ReadInputElement(item));
        }
    }
}

void AppendShaderStage(
    std::string_view name,
    const FJsonValue* stageValue,
    const std::filesystem::path& baseDir,
    std::vector<FCaptureShaderStage>& out)
{
    if (!stageValue || !stageValue->IsObject())
    {
        return;
    }

    FCaptureShaderStage stage;
    stage.Name = std::string(name);
    stage.ShaderId = ReadUInt64(FindAny(*stageValue, { "shader_id", "resource_id" }));
    stage.EntryPoint = ReadString(FindAny(*stageValue, { "entry", "entry_point" }));

    stage.BytecodePath = ResolveManifestPath(baseDir, ReadString(FindAny(*stageValue, { "dxbc_path", "dxil_path", "bytecode_path", "path" })));
    stage.DisassemblyPath = ResolveManifestPath(baseDir, ReadString(FindAny(*stageValue, { "disassembly_path", "disasm_path" })));

    AppendBoundResourceArray(
        FindAny(*stageValue, { "bound_cbvs", "bound_constant_buffers", "constant_buffers" }),
        baseDir,
        stage.BoundConstantBuffers);
    AppendBoundResourceArray(
        FindAny(*stageValue, { "bound_srvs", "bound_read_only_resources", "read_only_resources" }),
        baseDir,
        stage.BoundReadOnlyResources);

    out.push_back(std::move(stage));
}

void AppendShaderStages(const FJsonValue& eventValue, const std::filesystem::path& baseDir, std::vector<FCaptureShaderStage>& out)
{
    static constexpr std::string_view kStageNames[] = { "vs", "hs", "ds", "gs", "ps", "cs" };

    if (const FJsonValue* stages = eventValue.Find("stages"); stages && stages->IsObject())
    {
        for (std::string_view name : kStageNames)
        {
            AppendShaderStage(name, stages->Find(name), baseDir, out);
        }
    }

    for (std::string_view name : kStageNames)
    {
        AppendShaderStage(name, eventValue.Find(name), baseDir, out);
    }
}
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
    {
        throw std::runtime_error("failed to open text file: " + WideToUtf8(path.wstring()));
    }

    const std::streamsize size = in.tellg();
    if (size < 0)
    {
        throw std::runtime_error("failed to measure text file: " + WideToUtf8(path.wstring()));
    }

    std::string text(static_cast<size_t>(size), '\0');
    in.seekg(0, std::ios::beg);
    if (size > 0 && !in.read(text.data(), size))
    {
        throw std::runtime_error("failed to read text file: " + WideToUtf8(path.wstring()));
    }
    return text;
}

std::vector<std::byte> ReadBinaryFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
    {
        throw std::runtime_error("failed to open binary file: " + WideToUtf8(path.wstring()));
    }

    const std::streamsize size = in.tellg();
    if (size < 0)
    {
        throw std::runtime_error("failed to measure binary file: " + WideToUtf8(path.wstring()));
    }

    std::vector<std::byte> bytes(static_cast<size_t>(size));
    in.seekg(0, std::ios::beg);
    if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size))
    {
        throw std::runtime_error("failed to read binary file: " + WideToUtf8(path.wstring()));
    }
    return bytes;
}

std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty())
    {
        return {};
    }

    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0)
    {
        return std::wstring(value.begin(), value.end());
    }

    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed);
    return out;
}

std::string WideToUtf8(std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }

    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
    {
        std::string fallback;
        fallback.reserve(value.size());
        for (wchar_t c : value)
        {
            fallback.push_back(c >= 0 && c <= 0x7F ? static_cast<char>(c) : '?');
        }
        return fallback;
    }

    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

DXGI_FORMAT ParseDxgiFormat(std::string_view value)
{
    std::string key = ToUpper(std::string(value));
    if (key.rfind("DXGI_FORMAT_", 0) == 0)
    {
        key.erase(0, 12);
    }

    if (key == "UNKNOWN" || key.empty()) return DXGI_FORMAT_UNKNOWN;
    if (key == "R8G8B8A8_UNORM") return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (key == "R8G8B8A8_SNORM") return DXGI_FORMAT_R8G8B8A8_SNORM;
    if (key == "R16_UINT") return DXGI_FORMAT_R16_UINT;
    if (key == "R32_UINT") return DXGI_FORMAT_R32_UINT;
    if (key == "R16_FLOAT") return DXGI_FORMAT_R16_FLOAT;
    if (key == "R16_UNORM") return DXGI_FORMAT_R16_UNORM;
    if (key == "R32_FLOAT") return DXGI_FORMAT_R32_FLOAT;
    if (key == "R32G32_FLOAT") return DXGI_FORMAT_R32G32_FLOAT;
    if (key == "R16G16_FLOAT") return DXGI_FORMAT_R16G16_FLOAT;
    if (key == "R16G16_UNORM") return DXGI_FORMAT_R16G16_UNORM;
    if (key == "R8G8_UNORM") return DXGI_FORMAT_R8G8_UNORM;
    if (key == "R32G32B32_FLOAT") return DXGI_FORMAT_R32G32B32_FLOAT;
    if (key == "R32G32B32A32_FLOAT") return DXGI_FORMAT_R32G32B32A32_FLOAT;
    if (key == "R16G16B16A16_FLOAT") return DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (key == "R11G11B10_FLOAT") return DXGI_FORMAT_R11G11B10_FLOAT;
    if (key == "BC1_UNORM") return DXGI_FORMAT_BC1_UNORM;
    if (key == "BC3_UNORM") return DXGI_FORMAT_BC3_UNORM;
    if (key == "BC4_UNORM") return DXGI_FORMAT_BC4_UNORM;
    if (key == "BC5_UNORM") return DXGI_FORMAT_BC5_UNORM;
    if (key == "BC7_UNORM") return DXGI_FORMAT_BC7_UNORM;
    if (key == "D32_FLOAT") return DXGI_FORMAT_D32_FLOAT;
    if (key == "D32_FLOAT_S8X24_UINT") return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    if (key == "D32S8_TYPELESS") return DXGI_FORMAT_R32G8X24_TYPELESS;
    return DXGI_FORMAT_UNKNOWN;
}

std::string DxgiFormatName(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "DXGI_FORMAT_R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_SNORM: return "DXGI_FORMAT_R8G8B8A8_SNORM";
    case DXGI_FORMAT_R16_UINT: return "DXGI_FORMAT_R16_UINT";
    case DXGI_FORMAT_R32_UINT: return "DXGI_FORMAT_R32_UINT";
    case DXGI_FORMAT_R16_FLOAT: return "DXGI_FORMAT_R16_FLOAT";
    case DXGI_FORMAT_R16_UNORM: return "DXGI_FORMAT_R16_UNORM";
    case DXGI_FORMAT_R32_FLOAT: return "DXGI_FORMAT_R32_FLOAT";
    case DXGI_FORMAT_R32G32_FLOAT: return "DXGI_FORMAT_R32G32_FLOAT";
    case DXGI_FORMAT_R16G16_FLOAT: return "DXGI_FORMAT_R16G16_FLOAT";
    case DXGI_FORMAT_R16G16_UNORM: return "DXGI_FORMAT_R16G16_UNORM";
    case DXGI_FORMAT_R8G8_UNORM: return "DXGI_FORMAT_R8G8_UNORM";
    case DXGI_FORMAT_R32G32B32_FLOAT: return "DXGI_FORMAT_R32G32B32_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "DXGI_FORMAT_R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "DXGI_FORMAT_R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "DXGI_FORMAT_R11G11B10_FLOAT";
    case DXGI_FORMAT_BC1_UNORM: return "DXGI_FORMAT_BC1_UNORM";
    case DXGI_FORMAT_BC3_UNORM: return "DXGI_FORMAT_BC3_UNORM";
    case DXGI_FORMAT_BC4_UNORM: return "DXGI_FORMAT_BC4_UNORM";
    case DXGI_FORMAT_BC5_UNORM: return "DXGI_FORMAT_BC5_UNORM";
    case DXGI_FORMAT_BC7_UNORM: return "DXGI_FORMAT_BC7_UNORM";
    case DXGI_FORMAT_D32_FLOAT: return "DXGI_FORMAT_D32_FLOAT";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "DXGI_FORMAT_D32_FLOAT_S8X24_UINT";
    case DXGI_FORMAT_R32G8X24_TYPELESS: return "DXGI_FORMAT_R32G8X24_TYPELESS";
    default: return "DXGI_FORMAT_UNKNOWN";
    }
}

void FCaptureAssetManifest::LoadFromFile(const std::filesystem::path& path)
{
    SourcePath_ = path;
    BaseDirectory_ = path.parent_path();
    Entries_.clear();

    const FJsonValue root = ParseJson(ReadTextFile(path));
    const FJsonValue* entries = root.Find("entries");
    if (!entries || !entries->IsArray())
    {
        throw std::runtime_error("asset manifest requires an entries array");
    }

    for (const FJsonValue& value : entries->AsArray())
    {
        if (!value.IsObject())
        {
            continue;
        }

        FCaptureAssetEntry entry;
        entry.LogicalName = ReadString(value.Find("logical_name"));
        entry.Id = ReadUInt64(value.Find("id"));
        entry.Type = ReadString(value.Find("type"));
        const std::string pathText = ReadString(value.Find("path"));
        entry.Path = ResolveManifestPath(BaseDirectory_, pathText);
        entry.Format = ParseDxgiFormat(ReadString(value.Find("format")));
        entry.Width = ReadUInt(value.Find("width"));
        entry.Height = ReadUInt(value.Find("height"));
        entry.Depth = ReadUInt(value.Find("depth"), 1);
        entry.Mips = ReadUInt(value.Find("mips"), 1);
        entry.Stride = ReadUInt(value.Find("stride"));
        entry.ByteOffset = ReadUInt64(value.Find("byte_offset"));
        entry.ByteLength = ReadUInt64(value.Find("byte_length"));
        entry.FirstElement = ReadUInt(value.Find("first_element"));
        entry.NumElements = ReadUInt(value.Find("num_elements"));
        entry.SourceDraw = ReadUInt(value.Find("source_draw"));
        Entries_.push_back(std::move(entry));
    }
}

std::filesystem::path FCapturePassManifest::ResolvePath(std::string_view path) const
{
    return ResolveManifestPath(BaseDirectory_, path);
}

void FCapturePassManifest::LoadFromFile(const std::filesystem::path& path)
{
    SourcePath_ = path;
    BaseDirectory_ = path.parent_path();
    SourceCapture_.clear();
    Available_ = true;
    Events_.clear();

    const FJsonValue root = ParseJson(ReadTextFile(path));
    if (!root.IsObject())
    {
        throw std::runtime_error("pass manifest root must be an object");
    }

    Available_ = root.Find("available") == nullptr || ReadBool(root.Find("available"), true);
    SourceCapture_ = ResolvePath(ReadString(FindAny(root, { "capture", "source_capture" })));

    const FJsonValue* events = root.Find("events");
    if (!events || !events->IsArray())
    {
        throw std::runtime_error("pass manifest requires an events array");
    }

    for (const FJsonValue& eventValue : events->AsArray())
    {
        if (!eventValue.IsObject())
        {
            continue;
        }

        FCapturePassEvent event;
        event.EventId = ReadUInt(eventValue.Find("event_id"));
        event.ActionId = ReadUInt(eventValue.Find("action_id"));
        event.Label = ReadString(eventValue.Find("label"));
        if (event.Label.empty() && event.EventId != 0)
        {
            event.Label = "event_" + std::to_string(event.EventId);
        }

        event.PipelineStateId = ReadUInt64(FindAny(eventValue, { "pso", "pipeline_state_id" }));
        event.RootSignatureId = ReadUInt64(FindAny(eventValue, { "root_signature", "root_signature_id" }));
        event.TopologyName = ReadString(eventValue.Find("topology"));
        event.PrimitiveTopology = static_cast<D3D12_PRIMITIVE_TOPOLOGY>(ReadUInt(eventValue.Find("primitive_topology")));
        event.Viewport = ReadViewport(eventValue.Find("viewport"));

        if (const FJsonValue* pipeline = eventValue.Find("pipeline_state"); pipeline && pipeline->IsObject())
        {
            if (event.PipelineStateId == 0)
            {
                event.PipelineStateId = ReadUInt64(FindAny(*pipeline, { "pso_id", "pipeline_state_id" }));
            }
            if (event.RootSignatureId == 0)
            {
                event.RootSignatureId = ReadUInt64(FindAny(*pipeline, { "root_signature", "root_signature_id" }));
            }
            AppendInputLayout(pipeline->Find("input_layout"), event.InputLayout);
        }

        if (const FJsonValue* draw = eventValue.Find("draw"); draw && draw->IsObject())
        {
            event.DrawIndexInCommandList = ReadUInt(draw->Find("draw_index_in_cmdlist"));
            event.CommandList = ReadUInt(draw->Find("command_list"));
            event.NumIndices = ReadUInt(draw->Find("num_indices"));
            event.NumInstances = ReadUInt(draw->Find("num_instances"), 1);
            event.BaseVertex = ReadUInt(draw->Find("base_vertex"));
            event.IndexOffset = ReadUInt(draw->Find("index_offset"));
            event.VertexOffset = ReadUInt(draw->Find("vertex_offset"));
            event.VertexCount = ReadUInt(draw->Find("vertex_count"));
            event.DrawChunkName = ReadString(draw->Find("chunk_name"));
        }
        else
        {
            event.NumIndices = ReadUInt(eventValue.Find("num_indices"));
        event.NumInstances = ReadUInt(eventValue.Find("num_instances"), 1);
        event.BaseVertex = ReadUInt(eventValue.Find("base_vertex"));
        event.IndexOffset = ReadUInt(eventValue.Find("index_offset"));
        event.VertexOffset = ReadUInt(eventValue.Find("vertex_offset"));
        }

        if (const FJsonValue* renderTargets = eventValue.Find("render_targets"); renderTargets && renderTargets->IsArray())
        {
            event.RenderTargetCount = std::max<uint32_t>(1, std::min<uint32_t>(8, static_cast<uint32_t>(renderTargets->AsArray().size())));
        }

        AppendInputLayout(eventValue.Find("input_layout"), event.InputLayout);
        AppendBoundResourceArray(eventValue.Find("vertex_buffers"), BaseDirectory_, event.VertexBuffers);
        if (const FJsonValue* indexBuffer = eventValue.Find("index_buffer"); indexBuffer && indexBuffer->IsObject())
        {
            FCaptureBoundResource resource = ReadBoundResource(*indexBuffer, BaseDirectory_);
            resource.Kind = ECaptureResourceKind::Buffer;
            event.IndexBuffer = std::move(resource);
        }
        AppendBoundResourceArray(eventValue.Find("root_views"), BaseDirectory_, event.RootViews);
        AppendBoundResourceArray(eventValue.Find("root_cbvs"), BaseDirectory_, event.RootConstantBuffers);
        AppendShaderStages(eventValue, BaseDirectory_, event.ShaderStages);

        if (event.EventId != 0)
        {
            Events_.push_back(std::move(event));
        }
    }
}
}
