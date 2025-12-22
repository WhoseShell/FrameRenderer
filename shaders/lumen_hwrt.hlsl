// Lumen (HWRT, prototype): DXR ray query + screen-probe GI with temporal reprojection and bilateral filter.
// This is a learning-friendly approximation (not UE5.7 full Lumen).

cbuffer HWRTGICB : register(b0)
{
    row_major float4x4 g_viewProj;
    row_major float4x4 g_prevViewProj;

    float3 g_cameraPosWs;
    float  g_temporalWeight;

    float3 g_lightDirWs;
    float  g_maxTraceDistance;

    float3 g_lightColor;
    float  g_lightIntensity;

    float2 g_invFullResolution;
    float2 g_invGIResolution;

    float  g_frameIndex;
    float  g_frameParity;
    float  g_prevHistoryIndex;
    float  g_historyValid;

    float  g_giIntensity;
    float  g_depthReject;
    float  g_normalReject;
    float  g_raysPerPixel;
    float  g_objectCount;
    float  _pad0;
    float  _pad1;
    float  _pad2;
};

Texture2D g_gbuffer0 : register(t0); // albedo.rgb + metallic.a
Texture2D g_gbuffer1 : register(t1); // normal.xyz + (roughness+1).a (0 => empty)
Texture2D g_gbuffer2 : register(t2); // posW.xyz + ao.a
Texture2D g_unused3  : register(t3);
Texture2D g_unused4  : register(t4);

RaytracingAccelerationStructure g_tlas0 : register(t5);
RaytracingAccelerationStructure g_tlas1 : register(t6);

struct ObjectData
{
    float3 Albedo;
    float Metallic;
    float Roughness;
    float AO;
    uint  MeshType;
    uint  _pad0;
    float3 Position;
    float Radius;
    float3 Scale;
    float _pad1;
};
StructuredBuffer<ObjectData> g_objects0 : register(t7);
StructuredBuffer<ObjectData> g_objects1 : register(t8);

struct Vertex
{
    float3 Pos;
    float3 Col;
    float3 Nrm;
    float2 UV;
};
StructuredBuffer<Vertex> g_sphereVerts : register(t9);
ByteAddressBuffer       g_sphereIB    : register(t10);
StructuredBuffer<Vertex> g_boxVerts   : register(t11);
ByteAddressBuffer       g_boxIB       : register(t12);
StructuredBuffer<Vertex> g_coneVerts  : register(t13);
ByteAddressBuffer       g_coneIB      : register(t14);

Texture2D g_giHistory0 : register(t15);
Texture2D g_giHistory1 : register(t16);
Texture2D g_giMeta0    : register(t17);
Texture2D g_giMeta1    : register(t18);
Texture2D g_giFiltered : register(t19);
TextureCube g_skyPrefilter : register(t26);
StructuredBuffer<float4> g_skySH : register(t27);

RWTexture2D<float4> g_outHistory0 : register(u0);
RWTexture2D<float4> g_outHistory1 : register(u1);
RWTexture2D<float4> g_outMeta0    : register(u2);
RWTexture2D<float4> g_outMeta1    : register(u3);
RWTexture2D<float4> g_outFiltered : register(u4);

SamplerState g_samp : register(s0);

#include "pbr_common.hlsl"

/**
 * @brief 哈希函数，用于随机种子扰动。
 * @param x 输入值。
 * @return 哈希后的值。
 * @note 阶段：随机序列生成阶段。
 */
uint Hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

/**
 * @brief 生成 [0,1) 随机数。
 * @param seed 输入/输出随机种子。
 * @return 随机浮点数。
 * @note 阶段：随机采样阶段。
 */
float Rand01(inout uint seed)
{
    seed = Hash(seed);
    return (seed & 0x00FFFFFFu) / 16777216.0;
}

/**
 * @brief 构建法线空间正交基。
 * @param N 法线方向。
 * @param T 输出切线方向。
 * @param B 输出副切线方向。
 * @note 阶段：采样方向生成阶段。
 */
void MakeBasis(float3 N, out float3 T, out float3 B)
{
    float3 up = (abs(N.y) < 0.999) ? float3(0, 1, 0) : float3(1, 0, 0);
    T = normalize(cross(up, N));
    B = normalize(cross(N, T));
}

/**
 * @brief 余弦加权半球采样方向。
 * @param N 法线方向。
 * @param seed 输入/输出随机种子。
 * @return 采样方向。
 * @note 阶段：GI 采样阶段。
 */
float3 SampleHemisphereCos(float3 N, inout uint seed)
{
    float r1 = Rand01(seed);
    float r2 = Rand01(seed);
    float phi = 2.0 * PI * r1;
    float cosTheta = sqrt(1.0 - r2);
    float sinTheta = sqrt(r2);

    float3 T, B;
    MakeBasis(N, T, B);

    float3 L = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    return normalize(T * L.x + B * L.y + N * L.z);
}

/**
 * @brief 读取 16 位索引。
 * @param ib 索引缓冲。
 * @param index 索引位置。
 * @return 读取的索引值。
 * @note 阶段：HWRT 三角形数据读取阶段。
 */
uint LoadIndex16(ByteAddressBuffer ib, uint index)
{
    const uint byteOffset = index * 2u;
    const uint aligned = byteOffset & ~3u;
    const uint word = ib.Load(aligned);
    return ((byteOffset & 2u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
}

/**
 * @brief 按 mesh 类型读取顶点。
 * @param meshType 网格类型。
 * @param index 顶点索引。
 * @return 顶点数据。
 * @note 阶段：HWRT 三角形数据读取阶段。
 */
Vertex LoadVertex(uint meshType, uint index)
{
    if (meshType == 0) return g_sphereVerts[index];
    if (meshType == 1) return g_boxVerts[index];
    return g_coneVerts[index];
}

/**
 * @brief 根据三角形索引与重心坐标获取位置与法线。
 * @param meshType 网格类型。
 * @param primIndex 三角形索引。
 * @param bary 重心坐标（b1,b2）。
 * @param posObj 输出对象空间位置。
 * @param nrmObj 输出对象空间法线。
 * @note 阶段：HWRT 几何解算阶段。
 */
void GetTriangleData(uint meshType, uint primIndex, float2 bary, out float3 posObj, out float3 nrmObj)
{
    uint i0 = 0, i1 = 0, i2 = 0;
    if (meshType == 0)
    {
        i0 = LoadIndex16(g_sphereIB, primIndex * 3u + 0u);
        i1 = LoadIndex16(g_sphereIB, primIndex * 3u + 1u);
        i2 = LoadIndex16(g_sphereIB, primIndex * 3u + 2u);
    }
    else if (meshType == 1)
    {
        i0 = LoadIndex16(g_boxIB, primIndex * 3u + 0u);
        i1 = LoadIndex16(g_boxIB, primIndex * 3u + 1u);
        i2 = LoadIndex16(g_boxIB, primIndex * 3u + 2u);
    }
    else
    {
        i0 = LoadIndex16(g_coneIB, primIndex * 3u + 0u);
        i1 = LoadIndex16(g_coneIB, primIndex * 3u + 1u);
        i2 = LoadIndex16(g_coneIB, primIndex * 3u + 2u);
    }

    const Vertex v0 = LoadVertex(meshType, i0);
    const Vertex v1 = LoadVertex(meshType, i1);
    const Vertex v2 = LoadVertex(meshType, i2);

    const float b1 = bary.x;
    const float b2 = bary.y;
    const float b0 = 1.0 - b1 - b2;

    posObj = v0.Pos * b0 + v1.Pos * b1 + v2.Pos * b2;
    nrmObj = normalize(v0.Nrm * b0 + v1.Nrm * b1 + v2.Nrm * b2);
}

/**
 * @brief 根据实例 ID 读取对象数据。
 * @param instanceId 实例 ID。
 * @return 对象数据。
 * @note 阶段：HWRT 场景读取阶段。
 */
ObjectData LoadObject(uint instanceId)
{
    if (g_frameParity < 0.5)
        return g_objects0[instanceId];
    return g_objects1[instanceId];
}

/**
 * @brief 追踪最近命中（RayQuery）。
 * @param origin 射线起点。
 * @param dir 射线方向。
 * @param tMax 最大距离。
 * @param instanceId 输出实例 ID。
 * @param primIndex 输出三角形索引。
 * @param bary 输出重心坐标。
 * @param t 输出命中距离。
 * @param objToWorld 输出对象到世界矩阵。
 * @return 是否命中。
 * @note 阶段：HWRT 追踪阶段。
 */
bool TraceClosest(float3 origin, float3 dir, float tMax, out uint instanceId, out uint primIndex, out float2 bary, out float t, out float3x4 objToWorld)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.001;
    ray.TMax = tMax;

    RayQuery<RAY_FLAG_NONE> q;
    if (g_frameParity < 0.5)
        q.TraceRayInline(g_tlas0, RAY_FLAG_NONE, 0xFF, ray);
    else
        q.TraceRayInline(g_tlas1, RAY_FLAG_NONE, 0xFF, ray);

    while (q.Proceed()) {}

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        instanceId = q.CommittedInstanceID();
        primIndex = q.CommittedPrimitiveIndex();
        bary = q.CommittedTriangleBarycentrics();
        t = q.CommittedRayT();
        objToWorld = q.CommittedObjectToWorld3x4();
        return true;
    }

    instanceId = 0;
    primIndex = 0;
    bary = float2(0.0, 0.0);
    t = tMax;
    objToWorld = (float3x4)0;
    return false;
}

/**
 * @brief 追踪任意命中（阴影射线）。
 * @param origin 射线起点。
 * @param dir 射线方向。
 * @param tMax 最大距离。
 * @return 是否命中。
 * @note 阶段：HWRT 阴影测试阶段。
 */
bool TraceAnyHit(float3 origin, float3 dir, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.001;
    ray.TMax = tMax;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    if (g_frameParity < 0.5)
        q.TraceRayInline(g_tlas0, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, ray);
    else
        q.TraceRayInline(g_tlas1, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, ray);

    while (q.Proceed()) {}
    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT);
}

/**
 * @brief 命中点直接光照着色（含阴影）。
 * @param hitPosWs 命中世界位置。
 * @param hitNWs 命中世界法线。
 * @param obj 对象材质数据。
 * @return 直接光照颜色。
 * @note 阶段：HWRT 命中着色阶段。
 */
float3 ShadeHitDirect(float3 hitPosWs, float3 hitNWs, ObjectData obj)
{
    const float3 L = normalize(-g_lightDirWs);
    float NdotL = saturate(dot(hitNWs, L));
    if (NdotL > 0.0)
    {
        // Shadow ray towards directional sun.
        const bool occluded = TraceAnyHit(hitPosWs + hitNWs * 0.02, L, 1000.0);
        if (occluded) NdotL = 0.0;
    }

    const float3 sunRadiance = g_lightColor * g_lightIntensity;
    const float3 diffuse = (obj.Albedo / PI) * sunRadiance * NdotL;

    const float3 sky = SampleSkyDiffuse(hitNWs);
    const float3 diffuseSky = (obj.Albedo / PI) * sky;
    return diffuse + diffuseSky * obj.AO;
}

[numthreads(8, 8, 1)]
/**
 * @brief HWRT GI 计算着色器。
 * @param tid Dispatch 线程 ID。
 * @note 阶段：HWRT GI 计算阶段。
 */
void CSHWRTGI(uint3 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_outHistory0.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h)
        return;

    const float2 uv = (float2(tid.xy) + 0.5) * g_invGIResolution;

    const float4 gb1 = g_gbuffer1.SampleLevel(g_samp, uv, 0);
    const float valid = gb1.a;
    const bool isValid = (valid > 0.0);

    const float3 N = isValid ? normalize(gb1.xyz) : float3(0, 1, 0);
    const float3 posW = isValid ? g_gbuffer2.SampleLevel(g_samp, uv, 0).xyz : float3(0.0, 0.0, 0.0);
    const float ao = isValid ? saturate(g_gbuffer2.SampleLevel(g_samp, uv, 0).a) : 0.0;
    const float3 albedo = isValid ? g_gbuffer0.SampleLevel(g_samp, uv, 0).rgb : float3(0.0, 0.0, 0.0);

    const float depth = isValid ? max(length(posW - g_cameraPosWs), 1e-3) : 0.0;

    // RNG 种子：像素 + 帧序号。
    uint seed = Hash(tid.x + (tid.y << 16) + (uint)(g_frameIndex * 1664525.0));

    // 追踪间接光。
    float3 indirect = float3(0.0, 0.0, 0.0);
    if (isValid)
    {
        const int rayCount = max(1, (int)round(g_raysPerPixel));
        // 多条光线采样。
        [loop]
        for (int r = 0; r < rayCount; ++r)
        {
            const float3 D = SampleHemisphereCos(N, seed);

            uint instId = 0, primId = 0;
            float2 bary = float2(0.0, 0.0);
            float t = 0.0;
            float3x4 objToWorld;
            if (TraceClosest(posW + N * 0.02, D, g_maxTraceDistance, instId, primId, bary, t, objToWorld))
            {
                ObjectData obj = LoadObject(instId);

                float3 posObj, nrmObj;
                GetTriangleData(obj.MeshType, primId, bary, posObj, nrmObj);

                const float3 hitPosWs = mul(objToWorld, float4(posObj, 1.0));
                const float3 hitNWs = normalize(mul((float3x3)objToWorld, nrmObj));

                indirect += ShadeHitDirect(hitPosWs, hitNWs, obj);
            }
            else
            {
                indirect += SampleSkyDiffuse(N) / PI;
            }
        }
        indirect /= (float)rayCount;

        // Diffuse response at the shading point for cosine-weighted sampling.
        indirect = albedo * indirect * ao * g_giIntensity;
    }

    // 时序重投影。
    float3 accum = indirect;
    float wHist = (g_historyValid > 0.5) ? saturate(g_temporalWeight) : 0.0;
    if (isValid && wHist > 0.0)
    {
        float4 clipPrev = mul(float4(posW, 1.0), g_prevViewProj);
        if (clipPrev.w > 1e-4)
        {
            float2 ndcPrev = clipPrev.xy / clipPrev.w;
            float2 uvPrev = float2(ndcPrev.x * 0.5 + 0.5, -ndcPrev.y * 0.5 + 0.5);
            if (all(uvPrev >= 0.0) && all(uvPrev <= 1.0))
            {
                const bool prev0 = (g_prevHistoryIndex < 0.5);
                const float4 prevGI = prev0 ? g_giHistory0.SampleLevel(g_samp, uvPrev, 0) : g_giHistory1.SampleLevel(g_samp, uvPrev, 0);
                const float4 prevMeta = prev0 ? g_giMeta0.SampleLevel(g_samp, uvPrev, 0) : g_giMeta1.SampleLevel(g_samp, uvPrev, 0);

                const float prevDepth = prevMeta.w;
                if (prevDepth <= 0.0)
                    wHist = 0.0;

                const float3 prevN = (wHist > 0.0) ? normalize(prevMeta.xyz) : N;

                const float depthRel = abs(prevDepth - depth) / max(depth, 1e-3);
                const float nDiff = 1.0 - saturate(dot(prevN, N));
                if (depthRel > g_depthReject || nDiff > g_normalReject)
                    wHist = 0.0;

                accum = lerp(indirect, prevGI.rgb, wHist);
            }
        }
    }

    // 写回历史与元数据。
    const bool write0 = (g_frameParity < 0.5);
    if (write0)
    {
        g_outHistory0[tid.xy] = float4(accum, 1.0);
        g_outMeta0[tid.xy] = float4(isValid ? N : float3(0.0, 0.0, 0.0), depth);
    }
    else
    {
        g_outHistory1[tid.xy] = float4(accum, 1.0);
        g_outMeta1[tid.xy] = float4(isValid ? N : float3(0.0, 0.0, 0.0), depth);
    }
}

[numthreads(8, 8, 1)]
/**
 * @brief 双边滤波 HWRT GI 结果。
 * @param tid Dispatch 线程 ID。
 * @note 阶段：HWRT GI 滤波阶段。
 */
void CSFilter(uint3 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_outFiltered.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h)
        return;

    const float2 uv = (float2(tid.xy) + 0.5) * g_invGIResolution;
    const bool read0 = (g_frameParity < 0.5);

    const float4 metaC = read0 ? g_giMeta0.SampleLevel(g_samp, uv, 0) : g_giMeta1.SampleLevel(g_samp, uv, 0);
    const float depthC = metaC.w;
    if (depthC <= 0.0)
    {
        g_outFiltered[tid.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    const float3 nC = normalize(metaC.xyz);

    float3 sum = float3(0.0, 0.0, 0.0);
    float wsum = 0.0;

    // 5x5 邻域双边权重累积。
    [unroll]
    for (int oy = -2; oy <= 2; ++oy)
    {
        [unroll]
        for (int ox = -2; ox <= 2; ++ox)
        {
            const int2 p = int2(tid.xy) + int2(ox, oy);
            const int2 pc = clamp(p, int2(0, 0), int2((int)w - 1, (int)h - 1));
            const float2 u = (float2(pc) + 0.5) * g_invGIResolution;

            const float4 meta = read0 ? g_giMeta0.SampleLevel(g_samp, u, 0) : g_giMeta1.SampleLevel(g_samp, u, 0);
            const float4 gi = read0 ? g_giHistory0.SampleLevel(g_samp, u, 0) : g_giHistory1.SampleLevel(g_samp, u, 0);

            const float depth = meta.w;
            if (depth <= 0.0)
                continue;
            const float3 n = normalize(meta.xyz);
            const float nd = saturate(dot(nC, n));

            const float depthRel = abs(depth - depthC) / max(depthC, 1e-3);
            const float wDepth = exp(-depthRel * 25.0);
            const float wNormal = pow(nd, 8.0);
            const float wSpatial = exp(-(ox * ox + oy * oy) * 0.25);
            const float ww = wSpatial * wDepth * wNormal;

            sum += gi.rgb * ww;
            wsum += ww;
        }
    }

    const float3 filtered = (wsum > 1e-4) ? (sum / wsum) : float3(0.0, 0.0, 0.0);
    g_outFiltered[tid.xy] = float4(filtered, 1.0);
}
