#pragma once

#include <vector>

#include "Core/Types.h"

struct FVertex
{
    float Pos[3];
    float Col[3];
    float Nrm[3];
    float UV[2];
};

void GenerateSphereMesh(uint32 slices, uint32 stacks, float radius, std::vector<FVertex>& outVerts, std::vector<uint16>& outIndices);
void GenerateBoxMesh(float halfExtent, std::vector<FVertex>& outVerts, std::vector<uint16>& outIndices);
void GenerateConeMesh(uint32 slices, float radius, float height, std::vector<FVertex>& outVerts, std::vector<uint16>& outIndices);
