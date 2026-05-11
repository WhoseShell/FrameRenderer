#pragma once

#include "Renderer/MeshGeneration.h"

#include <filesystem>
#include <string>
#include <vector>

namespace editor
{
struct FObjMeshData
{
    std::vector<FVertex> Vertices;
    std::vector<uint32> Indices;
    float BoundsRadius = 1.0f;
};

bool LoadObjMesh(const std::filesystem::path& path, FObjMeshData& outMesh, std::wstring* error);
}
