#pragma once

#include <DirectXMath.h>

#include "Core/Types.h"

#include <string>

struct FSceneObject
{
    enum class EType : uint8
    {
        Sphere,
        Box,
        Cone,
        RenderDocRock,
        StaticMesh,
        SunLight,
        SkyAtmosphere,
    };

    uint32 Id = 0;
    std::wstring Name;
    EType Type = EType::Sphere;
    std::wstring AssetPath;
    std::wstring MaterialPath;
    int StaticMeshIndex = -1;
    DirectX::XMFLOAT3 Position{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 Scale{ 1.0f, 1.0f, 1.0f };
    float Radius = 0.75f; // for picking (bounding sphere)

    // Material (simple PBR parameters)
    DirectX::XMFLOAT3 Albedo{ 0.85f, 0.15f, 0.10f };
    float Metallic = 0.0f;
    float Roughness = 0.35f;
    int MaterialIndex = -1;
    int MaterialSRVBase = 0; // renderer descriptor table base (5 SRVs)

    float UseAlbedoTex = 0.0f;
    float UseNormalTex = 0.0f;
    float UseRoughnessTex = 0.0f;
    float UseMetallicTex = 0.0f;
    float UseAOTex = 0.0f;

    // Environment actor parameters.
    float LightIntensity = 3.0f;
    bool SkyEnabled = true;
    float RayleighScale = 1.0f;
    float MieScale = 1.0f;
    float MieG = 0.8f;
    float AtmosphereHeight = 12.0f;
};

inline bool IsProceduralSceneObject(FSceneObject::EType type)
{
    return type == FSceneObject::EType::Sphere
        || type == FSceneObject::EType::Box
        || type == FSceneObject::EType::Cone
        || type == FSceneObject::EType::SunLight
        || type == FSceneObject::EType::SkyAtmosphere;
}

inline bool IsStaticMeshSceneObject(FSceneObject::EType type)
{
    return type == FSceneObject::EType::StaticMesh;
}

inline bool IsMeshSceneObject(FSceneObject::EType type)
{
    return IsProceduralSceneObject(type) || IsStaticMeshSceneObject(type);
}

inline bool IsRenderDocRockObject(FSceneObject::EType type)
{
    return type == FSceneObject::EType::RenderDocRock;
}

inline bool IsEnvironmentSceneObject(FSceneObject::EType type)
{
    return type == FSceneObject::EType::SunLight
        || type == FSceneObject::EType::SkyAtmosphere;
}
