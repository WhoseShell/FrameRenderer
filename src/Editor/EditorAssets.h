#pragma once

#include <DirectXMath.h>

#include "Core/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace editor
{
enum class EAssetKind
{
    Model,
    Texture,
    Material,
    Level,
};

struct FAssetRecord
{
    EAssetKind Kind = EAssetKind::Texture;
    std::wstring Name;
    std::wstring RelativePath;
    std::filesystem::path AbsolutePath;
};

struct FMaterialFile
{
    std::wstring Name;
    DirectX::XMFLOAT3 Albedo{ 0.8f, 0.8f, 0.8f };
    float Metallic = 0.0f;
    float Roughness = 0.5f;
    std::wstring AlbedoTexture;
    std::wstring NormalTexture;
    std::wstring RoughnessTexture;
    std::wstring MetallicTexture;
    std::wstring AOTexture;
};

struct FLevelObjectFile
{
    uint32 Id = 0;
    std::wstring Name;
    std::wstring Type;
    std::wstring Asset;
    std::wstring Material;
    DirectX::XMFLOAT3 Position{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 Scale{ 1.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT3 Albedo{ 0.8f, 0.8f, 0.8f };
    float Metallic = 0.0f;
    float Roughness = 0.5f;
};

struct FLevelFile
{
    std::wstring Name;
    float SunYaw = 0.0f;
    float SunPitch = -0.6f;
    float SunIntensity = 3.0f;
    std::vector<FLevelObjectFile> Objects;
};

struct FContentLayout
{
    std::filesystem::path Root;
    std::filesystem::path Models;
    std::filesystem::path Textures;
    std::filesystem::path Materials;
    std::filesystem::path Levels;
};

std::string ToUtf8(const std::wstring& text);
std::wstring FromUtf8(const std::string& text);

FContentLayout MakeContentLayout(const std::filesystem::path& root);
bool EnsureContentLayout(const FContentLayout& layout, std::wstring* error);

std::wstring MakeRelativeContentPath(const std::filesystem::path& root, const std::filesystem::path& path);
std::filesystem::path ResolveContentPath(const std::filesystem::path& root, const std::wstring& relativePath);

std::vector<FAssetRecord> ScanContent(const FContentLayout& layout);

bool SaveMaterialFile(const std::filesystem::path& path, const FMaterialFile& material, std::wstring* error);
bool LoadMaterialFile(const std::filesystem::path& path, FMaterialFile& material, std::wstring* error);

bool SaveLevelFile(const std::filesystem::path& path, const FLevelFile& level, std::wstring* error);
bool LoadLevelFile(const std::filesystem::path& path, FLevelFile& level, std::wstring* error);
}
