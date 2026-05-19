#include "Editor/EditorAssets.h"

#include "RenderDoc/Json.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace editor
{
namespace
{
bool HasExt(const std::filesystem::path& path, std::initializer_list<const wchar_t*> exts)
{
    std::wstring name = path.filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    for (const wchar_t* candidate : exts)
    {
        std::wstring suffix = candidate;
        if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            return true;
    }
    return false;
}

std::wstring FileStem(const std::filesystem::path& path)
{
    return path.stem().wstring();
}

std::string JsonEscape(const std::wstring& value)
{
    const std::string utf8 = ToUtf8(value);
    std::string out;
    out.reserve(utf8.size() + 8);
    for (const char c : utf8)
    {
        switch (c)
        {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

std::wstring GetString(const rdcimport::FJsonValue& obj, const char* key, const std::wstring& fallback = L"")
{
    if (const rdcimport::FJsonValue* v = obj.Find(key); v && v->IsString())
        return FromUtf8(std::string(v->AsString()));
    return fallback;
}

float GetFloat(const rdcimport::FJsonValue& obj, const char* key, float fallback)
{
    if (const rdcimport::FJsonValue* v = obj.Find(key); v && v->IsNumber())
        return (float)v->AsNumber();
    return fallback;
}

uint32 GetUInt(const rdcimport::FJsonValue& obj, const char* key, uint32 fallback)
{
    if (const rdcimport::FJsonValue* v = obj.Find(key); v && v->IsNumber())
        return (uint32)std::max(0.0, v->AsNumber());
    return fallback;
}

bool GetBool(const rdcimport::FJsonValue& obj, const char* key, bool fallback)
{
    if (const rdcimport::FJsonValue* v = obj.Find(key); v && v->IsBool())
        return v->AsBool();
    return fallback;
}

DirectX::XMFLOAT3 GetFloat3(const rdcimport::FJsonValue& obj, const char* key, const DirectX::XMFLOAT3& fallback)
{
    const rdcimport::FJsonValue* v = obj.Find(key);
    if (!v || !v->IsArray())
        return fallback;
    const auto& a = v->AsArray();
    if (a.size() < 3 || !a[0].IsNumber() || !a[1].IsNumber() || !a[2].IsNumber())
        return fallback;
    return { (float)a[0].AsNumber(), (float)a[1].AsNumber(), (float)a[2].AsNumber() };
}

std::wstring NormalizeShadingMode(std::wstring mode)
{
    std::transform(mode.begin(), mode.end(), mode.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    if (mode == L"unlit")
        return L"Unlit";
    if (mode == L"rdr2rock" || mode == L"rdr2 rock" || mode == L"rock")
        return L"Rdr2Rock";
    if (mode == L"rdr2foliage" || mode == L"rdr2 foliage" || mode == L"foliage")
        return L"Rdr2Foliage";
    return L"PbrLit";
}

std::string JsonFloat3(const DirectX::XMFLOAT3& v)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(6);
    os << "[" << v.x << ", " << v.y << ", " << v.z << "]";
    return os.str();
}

bool ReadTextFile(const std::filesystem::path& path, std::string& out, std::wstring* error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        if (error) *error = L"Failed to open " + path.wstring();
        return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    out = ss.str();
    return true;
}

bool WriteTextFile(const std::filesystem::path& path, const std::string& text, std::wstring* error)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        if (error) *error = L"Failed to create " + path.parent_path().wstring();
        return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        if (error) *error = L"Failed to write " + path.wstring();
        return false;
    }
    file.write(text.data(), (std::streamsize)text.size());
    return true;
}

void EmitFloat3(std::ostream& os, const char* key, const DirectX::XMFLOAT3& v, const char* suffix = ",")
{
    os << "      \"" << key << "\": [" << v.x << ", " << v.y << ", " << v.z << "]" << suffix << "\n";
}
}

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return {};
    std::string out((size_t)bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), bytes, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& text)
{
    if (text.empty())
        return {};
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    if (chars <= 0)
        return {};
    std::wstring out((size_t)chars, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), chars);
    return out;
}

FContentLayout MakeContentLayout(const std::filesystem::path& root)
{
    FContentLayout layout{};
    layout.Root = root;
    layout.Models = root / L"Models";
    layout.Textures = root / L"Textures";
    layout.Materials = root / L"Materials";
    layout.Levels = root / L"Levels";
    return layout;
}

bool EnsureContentLayout(const FContentLayout& layout, std::wstring* error)
{
    std::error_code ec;
    std::filesystem::create_directories(layout.Models, ec);
    if (ec) { if (error) *error = L"Failed to create Models"; return false; }
    std::filesystem::create_directories(layout.Textures, ec);
    if (ec) { if (error) *error = L"Failed to create Textures"; return false; }
    std::filesystem::create_directories(layout.Materials, ec);
    if (ec) { if (error) *error = L"Failed to create Materials"; return false; }
    std::filesystem::create_directories(layout.Levels, ec);
    if (ec) { if (error) *error = L"Failed to create Levels"; return false; }
    return true;
}

std::wstring MakeRelativeContentPath(const std::filesystem::path& root, const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(path, root, ec);
    if (ec)
        rel = path.filename();
    return rel.generic_wstring();
}

std::filesystem::path ResolveContentPath(const std::filesystem::path& root, const std::wstring& relativePath)
{
    if (relativePath.empty())
        return {};
    return root / std::filesystem::path(relativePath);
}

std::vector<FAssetRecord> ScanContent(const FContentLayout& layout)
{
    std::vector<FAssetRecord> assets;
    auto scanDir = [&](const std::filesystem::path& dir, EAssetKind kind, std::initializer_list<const wchar_t*> exts)
    {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec))
            return;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec))
        {
            if (ec || !entry.is_regular_file())
                continue;
            const std::filesystem::path p = entry.path();
            if (!HasExt(p, exts))
                continue;
            FAssetRecord record{};
            record.Kind = kind;
            record.Name = FileStem(p);
            record.RelativePath = MakeRelativeContentPath(layout.Root, p);
            record.AbsolutePath = p;
            assets.push_back(record);
        }
    };

    scanDir(layout.Models, EAssetKind::Model, { L".obj" });
    scanDir(layout.Textures, EAssetKind::Texture, { L".png", L".jpg", L".jpeg", L".tga", L".dds" });
    scanDir(layout.Materials, EAssetKind::Material, { L".material.json" });
    scanDir(layout.Levels, EAssetKind::Level, { L".level.json" });

    std::sort(assets.begin(), assets.end(), [](const FAssetRecord& a, const FAssetRecord& b)
    {
        if (a.Kind != b.Kind)
            return (int)a.Kind < (int)b.Kind;
        return a.RelativePath < b.RelativePath;
    });
    return assets;
}

bool SaveMaterialFile(const std::filesystem::path& path, const FMaterialFile& material, std::wstring* error)
{
    const std::wstring mode = NormalizeShadingMode(material.ShadingMode);
    std::ostringstream os;
    os << std::fixed << std::setprecision(6);
    os << "{\n";
    os << "  \"type\": \"Material\",\n";
    os << "  \"version\": 2,\n";
    os << "  \"name\": \"" << JsonEscape(material.Name) << "\",\n";
    os << "  \"shadingMode\": \"" << JsonEscape(mode) << "\",\n";
    os << "  \"params\": {\n";
    if (mode == L"Unlit")
    {
        os << "    \"Color\": " << JsonFloat3(material.Albedo) << ",\n";
        os << "    \"Intensity\": " << material.Intensity << "\n";
    }
    else if (mode == L"Rdr2Rock" || mode == L"Rdr2Foliage")
    {
        os << "    \"BaseColor\": " << JsonFloat3(material.Albedo) << ",\n";
        os << "    \"Metallic\": " << material.Metallic << ",\n";
        os << "    \"Roughness\": " << material.Roughness << ",\n";
        os << "    \"NormalStrength\": " << material.NormalStrength << ",\n";
        os << "    \"BaseColorBoost\": " << material.BaseColorBoost << "\n";
    }
    else
    {
        os << "    \"BaseColor\": " << JsonFloat3(material.Albedo) << ",\n";
        os << "    \"Metallic\": " << material.Metallic << ",\n";
        os << "    \"Roughness\": " << material.Roughness << "\n";
    }
    os << "  },\n";
    os << "  \"textures\": {\n";
    if (mode == L"Unlit")
    {
        os << "    \"Color\": \"" << JsonEscape(material.AlbedoTexture) << "\"\n";
    }
    else if (mode == L"Rdr2Rock" || mode == L"Rdr2Foliage")
    {
        os << "    \"BaseColor\": \"" << JsonEscape(material.AlbedoTexture) << "\",\n";
        os << "    \"Normal\": \"" << JsonEscape(material.NormalTexture) << "\",\n";
        os << "    \"MaskA\": \"" << JsonEscape(material.RoughnessTexture) << "\",\n";
        os << "    \"MaskB\": \"" << JsonEscape(material.MetallicTexture) << "\",\n";
        os << "    \"Aux\": \"" << JsonEscape(material.AOTexture) << "\"\n";
    }
    else
    {
        os << "    \"BaseColor\": \"" << JsonEscape(material.AlbedoTexture) << "\",\n";
        os << "    \"Normal\": \"" << JsonEscape(material.NormalTexture) << "\",\n";
        os << "    \"Roughness\": \"" << JsonEscape(material.RoughnessTexture) << "\",\n";
        os << "    \"Metallic\": \"" << JsonEscape(material.MetallicTexture) << "\",\n";
        os << "    \"AO\": \"" << JsonEscape(material.AOTexture) << "\"\n";
    }
    os << "  }\n";
    os << "}\n";
    return WriteTextFile(path, os.str(), error);
}

bool LoadMaterialFile(const std::filesystem::path& path, FMaterialFile& material, std::wstring* error)
{
    std::string text;
    if (!ReadTextFile(path, text, error))
        return false;
    try
    {
        const rdcimport::FJsonValue root = rdcimport::ParseJson(text);
        if (!root.IsObject())
            throw std::runtime_error("root is not object");
        material.Version = GetUInt(root, "version", 1);
        material.Name = GetString(root, "name", FileStem(path));
        material.ShadingMode = NormalizeShadingMode(GetString(root, "shadingMode", L"PbrLit"));
        if (const rdcimport::FJsonValue* params = root.Find("params"); params && params->IsObject())
        {
            if (material.ShadingMode == L"Unlit")
            {
                material.Albedo = GetFloat3(*params, "Color", material.Albedo);
                material.Intensity = GetFloat(*params, "Intensity", material.Intensity);
            }
            else if (material.ShadingMode == L"Rdr2Rock" || material.ShadingMode == L"Rdr2Foliage")
            {
                material.Albedo = GetFloat3(*params, "BaseColor", material.Albedo);
                material.Metallic = GetFloat(*params, "Metallic", material.Metallic);
                material.Roughness = GetFloat(*params, "Roughness", material.Roughness);
                material.NormalStrength = GetFloat(*params, "NormalStrength", material.NormalStrength);
                material.BaseColorBoost = GetFloat(*params, "BaseColorBoost", material.BaseColorBoost);
            }
            else
            {
                material.Albedo = GetFloat3(*params, "BaseColor", material.Albedo);
                material.Metallic = GetFloat(*params, "Metallic", material.Metallic);
                material.Roughness = GetFloat(*params, "Roughness", material.Roughness);
            }
        }
        else
        {
            material.Albedo = GetFloat3(root, "albedo", material.Albedo);
            material.Metallic = GetFloat(root, "metallic", material.Metallic);
            material.Roughness = GetFloat(root, "roughness", material.Roughness);
        }
        if (const rdcimport::FJsonValue* tex = root.Find("textures"); tex && tex->IsObject())
        {
            material.AlbedoTexture = GetString(*tex, "BaseColor", GetString(*tex, "Color", GetString(*tex, "albedo")));
            material.NormalTexture = GetString(*tex, "Normal", GetString(*tex, "normal"));
            material.RoughnessTexture = GetString(*tex, "MaskA", GetString(*tex, "Roughness", GetString(*tex, "roughness")));
            material.MetallicTexture = GetString(*tex, "MaskB", GetString(*tex, "Metallic", GetString(*tex, "metallic")));
            material.AOTexture = GetString(*tex, "Aux", GetString(*tex, "AO", GetString(*tex, "ao")));
        }
    }
    catch (const std::exception& e)
    {
        if (error) *error = FromUtf8(e.what());
        return false;
    }
    return true;
}

bool SaveLevelFile(const std::filesystem::path& path, const FLevelFile& level, std::wstring* error)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(6);
    os << "{\n";
    os << "  \"type\": \"Level\",\n";
    os << "  \"name\": \"" << JsonEscape(level.Name) << "\",\n";
    os << "  \"sun\": {\n";
    os << "    \"yaw\": " << level.SunYaw << ",\n";
    os << "    \"pitch\": " << level.SunPitch << ",\n";
    os << "    \"intensity\": " << level.SunIntensity << "\n";
    os << "  },\n";
    os << "  \"objects\": [\n";
    for (size_t i = 0; i < level.Objects.size(); ++i)
    {
        const FLevelObjectFile& o = level.Objects[i];
        os << "    {\n";
        os << "      \"id\": " << o.Id << ",\n";
        os << "      \"name\": \"" << JsonEscape(o.Name) << "\",\n";
        os << "      \"type\": \"" << JsonEscape(o.Type) << "\",\n";
        os << "      \"asset\": \"" << JsonEscape(o.Asset) << "\",\n";
        os << "      \"material\": \"" << JsonEscape(o.Material) << "\",\n";
        EmitFloat3(os, "position", o.Position);
        EmitFloat3(os, "scale", o.Scale);
        EmitFloat3(os, "albedo", o.Albedo);
        os << "      \"metallic\": " << o.Metallic << ",\n";
        os << "      \"roughness\": " << o.Roughness << ",\n";
        os << "      \"lightIntensity\": " << o.LightIntensity << ",\n";
        os << "      \"skyEnabled\": " << (o.SkyEnabled ? "true" : "false") << ",\n";
        os << "      \"rayleighScale\": " << o.RayleighScale << ",\n";
        os << "      \"mieScale\": " << o.MieScale << ",\n";
        os << "      \"mieG\": " << o.MieG << ",\n";
        os << "      \"atmosphereHeight\": " << o.AtmosphereHeight << "\n";
        os << "    }" << ((i + 1 < level.Objects.size()) ? "," : "") << "\n";
    }
    os << "  ]\n";
    os << "}\n";
    return WriteTextFile(path, os.str(), error);
}

bool LoadLevelFile(const std::filesystem::path& path, FLevelFile& level, std::wstring* error)
{
    std::string text;
    if (!ReadTextFile(path, text, error))
        return false;
    try
    {
        const rdcimport::FJsonValue root = rdcimport::ParseJson(text);
        if (!root.IsObject())
            throw std::runtime_error("root is not object");

        level = {};
        level.Name = GetString(root, "name", FileStem(path));
        if (const rdcimport::FJsonValue* sun = root.Find("sun"); sun && sun->IsObject())
        {
            level.SunYaw = GetFloat(*sun, "yaw", level.SunYaw);
            level.SunPitch = GetFloat(*sun, "pitch", level.SunPitch);
            level.SunIntensity = GetFloat(*sun, "intensity", level.SunIntensity);
        }

        if (const rdcimport::FJsonValue* objects = root.Find("objects"); objects && objects->IsArray())
        {
            for (const rdcimport::FJsonValue& v : objects->AsArray())
            {
                if (!v.IsObject())
                    continue;
                FLevelObjectFile o{};
                o.Id = GetUInt(v, "id", 0);
                o.Name = GetString(v, "name");
                o.Type = GetString(v, "type", L"Sphere");
                o.Asset = GetString(v, "asset");
                o.Material = GetString(v, "material");
                o.Position = GetFloat3(v, "position", o.Position);
                o.Scale = GetFloat3(v, "scale", o.Scale);
                o.Albedo = GetFloat3(v, "albedo", o.Albedo);
                o.Metallic = GetFloat(v, "metallic", o.Metallic);
                o.Roughness = GetFloat(v, "roughness", o.Roughness);
                o.LightIntensity = GetFloat(v, "lightIntensity", o.LightIntensity);
                o.SkyEnabled = GetBool(v, "skyEnabled", o.SkyEnabled);
                o.RayleighScale = GetFloat(v, "rayleighScale", o.RayleighScale);
                o.MieScale = GetFloat(v, "mieScale", o.MieScale);
                o.MieG = GetFloat(v, "mieG", o.MieG);
                o.AtmosphereHeight = GetFloat(v, "atmosphereHeight", o.AtmosphereHeight);
                level.Objects.push_back(o);
            }
        }
    }
    catch (const std::exception& e)
    {
        if (error) *error = FromUtf8(e.what());
        return false;
    }
    return true;
}
}
