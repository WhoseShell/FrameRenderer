#include "Renderer/MeshGeneration.h"

#include <DirectXMath.h>
#include <cmath>

static FVertex MakeVertex(const DirectX::XMFLOAT3& p, const DirectX::XMFLOAT3& n, float u = 0.0f, float vCoord = 0.0f)
{
    FVertex vert{};
    vert.Pos[0] = p.x; vert.Pos[1] = p.y; vert.Pos[2] = p.z;
    vert.Col[0] = 0.5f + 0.5f * n.x;
    vert.Col[1] = 0.5f + 0.5f * n.y;
    vert.Col[2] = 0.5f + 0.5f * n.z;
    vert.Nrm[0] = n.x; vert.Nrm[1] = n.y; vert.Nrm[2] = n.z;
    vert.UV[0] = u;
    vert.UV[1] = vCoord;
    return vert;
}

void GenerateSphereMesh(uint32 slices, uint32 stacks, float radius, std::vector<FVertex>& outVerts, std::vector<uint16>& outIndices)
{
    using namespace DirectX;

    outVerts.clear();
    outIndices.clear();

    slices = (slices < 3) ? 3 : slices;
    stacks = (stacks < 2) ? 2 : stacks;

    const float invSlices = 1.0f / float(slices);
    const float invStacks = 1.0f / float(stacks);

    for (uint32 stack = 0; stack <= stacks; ++stack)
    {
        const float v = float(stack) * invStacks;
        const float phi = v * XM_PI;
        const float y = std::cosf(phi);
        const float r = std::sinf(phi);

        for (uint32 slice = 0; slice <= slices; ++slice)
        {
            const float u = float(slice) * invSlices;
            const float theta = u * XM_2PI;

            const float x = r * std::cosf(theta);
            const float z = r * std::sinf(theta);

            const XMFLOAT3 n{ x, y, z };
            const XMFLOAT3 p{ x * radius, y * radius, z * radius };

            outVerts.push_back(MakeVertex(p, n, u, v));
        }
    }

    const uint32 stride = slices + 1;
    for (uint32 stack = 0; stack < stacks; ++stack)
    {
        for (uint32 slice = 0; slice < slices; ++slice)
        {
            const uint16 i0 = uint16(stack * stride + slice);
            const uint16 i1 = uint16((stack + 1) * stride + slice);
            const uint16 i2 = uint16((stack + 1) * stride + (slice + 1));
            const uint16 i3 = uint16(stack * stride + (slice + 1));

            // Match winding with the rest of the engine (box/cull mode),
            // so vertex normals (outward) agree with the visible surface.
            outIndices.push_back(i0);
            outIndices.push_back(i2);
            outIndices.push_back(i1);

            outIndices.push_back(i0);
            outIndices.push_back(i3);
            outIndices.push_back(i2);
        }
    }
}

void GenerateBoxMesh(float halfExtent, std::vector<FVertex>& outVerts, std::vector<uint16>& outIndices)
{
    using namespace DirectX;

    outVerts.clear();
    outIndices.clear();

    const float h = halfExtent;

    // 6 faces * 4 verts = 24 verts
    const XMFLOAT3 p[8] = {
        { -h, -h, -h }, { +h, -h, -h }, { +h, +h, -h }, { -h, +h, -h },
        { -h, -h, +h }, { +h, -h, +h }, { +h, +h, +h }, { -h, +h, +h },
    };

    auto addFace = [&](int i0, int i1, int i2, int i3, const XMFLOAT3& n)
    {
        const uint16 base = (uint16)outVerts.size();
        outVerts.push_back(MakeVertex(p[i0], n, 0.0f, 1.0f));
        outVerts.push_back(MakeVertex(p[i1], n, 1.0f, 1.0f));
        outVerts.push_back(MakeVertex(p[i2], n, 1.0f, 0.0f));
        outVerts.push_back(MakeVertex(p[i3], n, 0.0f, 0.0f));

        outIndices.push_back(base + 0); outIndices.push_back(base + 1); outIndices.push_back(base + 2);
        outIndices.push_back(base + 0); outIndices.push_back(base + 2); outIndices.push_back(base + 3);
    };

    addFace(4, 5, 6, 7, { 0, 0, +1 }); // +Z
    addFace(1, 0, 3, 2, { 0, 0, -1 }); // -Z
    addFace(0, 4, 7, 3, { -1, 0, 0 }); // -X
    addFace(5, 1, 2, 6, { +1, 0, 0 }); // +X
    addFace(3, 7, 6, 2, { 0, +1, 0 }); // +Y
    addFace(0, 1, 5, 4, { 0, -1, 0 }); // -Y
}

void GenerateConeMesh(uint32 slices, float radius, float height, std::vector<FVertex>& outVerts, std::vector<uint16>& outIndices)
{
    using namespace DirectX;

    outVerts.clear();
    outIndices.clear();

    slices = (slices < 3) ? 3 : slices;

    const float halfH = height * 0.5f;
    const XMFLOAT3 tip{ 0.0f, +halfH, 0.0f };
    const XMFLOAT3 baseCenter{ 0.0f, -halfH, 0.0f };

    // Side vertices: duplicate per-triangle strip for decent normals
    for (uint32 i = 0; i <= slices; ++i)
    {
        const float t = (float)i / (float)slices;
        const float a = t * XM_2PI;
        const float x = std::cosf(a) * radius;
        const float z = std::sinf(a) * radius;

        // Approx normal for cone side
        const float ny = radius / height;
        XMVECTOR n = XMVector3Normalize(XMVectorSet(x, ny * radius, z, 0.0f));
        XMFLOAT3 n3{};
        XMStoreFloat3(&n3, n);

        outVerts.push_back(MakeVertex({ x, -halfH, z }, n3, t, 1.0f));
    }

    const uint16 tipIndex = (uint16)outVerts.size();
    outVerts.push_back(MakeVertex(tip, { 0.0f, 1.0f, 0.0f }, 0.5f, 0.0f));

    // Side indices (fan from tip)
    for (uint32 i = 0; i < slices; ++i)
    {
        const uint16 i0 = (uint16)i;
        const uint16 i1 = (uint16)(i + 1);
        outIndices.push_back(tipIndex);
        outIndices.push_back(i1);
        outIndices.push_back(i0);
    }

    // Base cap
    const uint16 baseCenterIndex = (uint16)outVerts.size();
    outVerts.push_back(MakeVertex(baseCenter, { 0.0f, -1.0f, 0.0f }, 0.5f, 0.5f));

    const uint16 baseRingStart = (uint16)outVerts.size();
    for (uint32 i = 0; i <= slices; ++i)
    {
        const float t = (float)i / (float)slices;
        const float a = t * XM_2PI;
        const float x = std::cosf(a) * radius;
        const float z = std::sinf(a) * radius;
        outVerts.push_back(MakeVertex({ x, -halfH, z }, { 0.0f, -1.0f, 0.0f }, 0.5f + x / (radius * 2.0f), 0.5f + z / (radius * 2.0f)));
    }

    for (uint32 i = 0; i < slices; ++i)
    {
        const uint16 i0 = baseRingStart + (uint16)i;
        const uint16 i1 = baseRingStart + (uint16)(i + 1);
        outIndices.push_back(baseCenterIndex);
        outIndices.push_back(i0);
        outIndices.push_back(i1);
    }
}
