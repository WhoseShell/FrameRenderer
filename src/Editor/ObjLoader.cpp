#include "Editor/ObjLoader.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace editor
{
namespace
{
struct FObjIndex
{
    int Pos = 0;
    int Tex = 0;
    int Nrm = 0;

    bool operator==(const FObjIndex& rhs) const
    {
        return Pos == rhs.Pos && Tex == rhs.Tex && Nrm == rhs.Nrm;
    }
};

struct FObjIndexHash
{
    size_t operator()(const FObjIndex& v) const
    {
        const uint64 a = (uint32)v.Pos;
        const uint64 b = (uint32)v.Tex;
        const uint64 c = (uint32)v.Nrm;
        return (size_t)(a * 73856093ull ^ b * 19349663ull ^ c * 83492791ull);
    }
};

int ResolveIndex(int objIndex, size_t count)
{
    if (objIndex > 0)
        return objIndex - 1;
    if (objIndex < 0)
        return (int)count + objIndex;
    return -1;
}

FObjIndex ParseFaceToken(const std::string& token)
{
    FObjIndex out{};
    size_t first = token.find('/');
    if (first == std::string::npos)
    {
        out.Pos = std::stoi(token);
        return out;
    }

    const std::string p = token.substr(0, first);
    if (!p.empty())
        out.Pos = std::stoi(p);

    size_t second = token.find('/', first + 1);
    if (second == std::string::npos)
    {
        const std::string t = token.substr(first + 1);
        if (!t.empty())
            out.Tex = std::stoi(t);
        return out;
    }

    const std::string t = token.substr(first + 1, second - first - 1);
    const std::string n = token.substr(second + 1);
    if (!t.empty())
        out.Tex = std::stoi(t);
    if (!n.empty())
        out.Nrm = std::stoi(n);
    return out;
}

DirectX::XMFLOAT3 ComputeFaceNormal(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, const DirectX::XMFLOAT3& c)
{
    using namespace DirectX;
    const XMVECTOR va = XMLoadFloat3(&a);
    const XMVECTOR vb = XMLoadFloat3(&b);
    const XMVECTOR vc = XMLoadFloat3(&c);
    XMFLOAT3 out{};
    XMStoreFloat3(&out, XMVector3Normalize(XMVector3Cross(XMVectorSubtract(vb, va), XMVectorSubtract(vc, va))));
    return out;
}

FVertex MakeVertex(const DirectX::XMFLOAT3& p, const DirectX::XMFLOAT3& n, const DirectX::XMFLOAT2& uv)
{
    FVertex v{};
    v.Pos[0] = p.x;
    v.Pos[1] = p.y;
    v.Pos[2] = p.z;
    v.Col[0] = 0.5f + 0.5f * n.x;
    v.Col[1] = 0.5f + 0.5f * n.y;
    v.Col[2] = 0.5f + 0.5f * n.z;
    v.Nrm[0] = n.x;
    v.Nrm[1] = n.y;
    v.Nrm[2] = n.z;
    v.UV[0] = uv.x;
    v.UV[1] = 1.0f - uv.y;
    return v;
}
}

bool LoadObjMesh(const std::filesystem::path& path, FObjMeshData& outMesh, std::wstring* error)
{
    std::ifstream file(path);
    if (!file)
    {
        if (error) *error = L"Failed to open OBJ: " + path.wstring();
        return false;
    }

    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<DirectX::XMFLOAT2> uvs;
    std::vector<DirectX::XMFLOAT3> normals;
    std::unordered_map<FObjIndex, uint32, FObjIndexHash> dedup;

    outMesh = {};

    auto appendVertex = [&](const FObjIndex& raw, const DirectX::XMFLOAT3& fallbackNormal) -> uint32
    {
        FObjIndex idx{};
        idx.Pos = ResolveIndex(raw.Pos, positions.size());
        idx.Tex = ResolveIndex(raw.Tex, uvs.size());
        idx.Nrm = ResolveIndex(raw.Nrm, normals.size());

        const auto found = dedup.find(idx);
        if (found != dedup.end())
            return found->second;

        if (idx.Pos < 0 || idx.Pos >= (int)positions.size())
            return UINT32_MAX;

        const DirectX::XMFLOAT3 p = positions[(size_t)idx.Pos];
        const DirectX::XMFLOAT2 uv = (idx.Tex >= 0 && idx.Tex < (int)uvs.size()) ? uvs[(size_t)idx.Tex] : DirectX::XMFLOAT2{ 0.0f, 0.0f };
        const DirectX::XMFLOAT3 n = (idx.Nrm >= 0 && idx.Nrm < (int)normals.size()) ? normals[(size_t)idx.Nrm] : fallbackNormal;

        const uint32 newIndex = (uint32)outMesh.Vertices.size();
        outMesh.Vertices.push_back(MakeVertex(p, n, uv));
        dedup.emplace(idx, newIndex);
        return newIndex;
    };

    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
            continue;

        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "v")
        {
            DirectX::XMFLOAT3 p{};
            ss >> p.x >> p.y >> p.z;
            positions.push_back(p);
        }
        else if (tag == "vt")
        {
            DirectX::XMFLOAT2 uv{};
            ss >> uv.x >> uv.y;
            uvs.push_back(uv);
        }
        else if (tag == "vn")
        {
            DirectX::XMFLOAT3 n{};
            ss >> n.x >> n.y >> n.z;
            normals.push_back(n);
        }
        else if (tag == "f")
        {
            std::vector<FObjIndex> face;
            std::string token;
            while (ss >> token)
                face.push_back(ParseFaceToken(token));
            if (face.size() < 3)
                continue;

            for (size_t tri = 1; tri + 1 < face.size(); ++tri)
            {
                const FObjIndex raw[3] = { face[0], face[tri], face[tri + 1] };
                DirectX::XMFLOAT3 fallbackNormal{ 0.0f, 1.0f, 0.0f };
                const int p0 = ResolveIndex(raw[0].Pos, positions.size());
                const int p1 = ResolveIndex(raw[1].Pos, positions.size());
                const int p2 = ResolveIndex(raw[2].Pos, positions.size());
                if (p0 >= 0 && p1 >= 0 && p2 >= 0 && p0 < (int)positions.size() && p1 < (int)positions.size() && p2 < (int)positions.size())
                    fallbackNormal = ComputeFaceNormal(positions[(size_t)p0], positions[(size_t)p1], positions[(size_t)p2]);

                const uint32 i0 = appendVertex(raw[0], fallbackNormal);
                const uint32 i1 = appendVertex(raw[1], fallbackNormal);
                const uint32 i2 = appendVertex(raw[2], fallbackNormal);
                if (i0 == UINT32_MAX || i1 == UINT32_MAX || i2 == UINT32_MAX)
                    continue;

                outMesh.Indices.push_back(i0);
                outMesh.Indices.push_back(i1);
                outMesh.Indices.push_back(i2);
            }
        }
    }

    if (outMesh.Vertices.empty() || outMesh.Indices.empty())
    {
        if (error) *error = L"OBJ contains no triangles: " + path.wstring();
        return false;
    }

    float radiusSq = 0.0f;
    for (const FVertex& v : outMesh.Vertices)
    {
        const float d = v.Pos[0] * v.Pos[0] + v.Pos[1] * v.Pos[1] + v.Pos[2] * v.Pos[2];
        radiusSq = std::max(radiusSq, d);
    }
    outMesh.BoundsRadius = std::max(0.1f, std::sqrt(radiusSq));
    return true;
}
}
