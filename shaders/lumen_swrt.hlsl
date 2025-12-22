// Lumen (SWRT): software ray tracing GI with surface cache + temporal history + bilateral filter.

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
Texture2D g_gbuffer1 : register(t1); // normal.xyz + (roughness+1).a
Texture2D g_gbuffer2 : register(t2); // posW.xyz + ao.a

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

Texture2D g_giHistory0 : register(t15);
Texture2D g_giHistory1 : register(t16);
Texture2D g_giMeta0    : register(t17);
Texture2D g_giMeta1    : register(t18);
Texture2D g_giFiltered : register(t19);
Texture2D g_surface0   : register(t20); // albedo + metallic
Texture2D g_surface1   : register(t21); // normal + (roughness+1)
TextureCube g_skyPrefilter : register(t26);
StructuredBuffer<float4> g_skySH : register(t27);

RWTexture2D<float4> g_outHistory0 : register(u0);
RWTexture2D<float4> g_outHistory1 : register(u1);
RWTexture2D<float4> g_outMeta0    : register(u2);
RWTexture2D<float4> g_outMeta1    : register(u3);
RWTexture2D<float4> g_outFiltered : register(u4);
RWTexture2D<float4> g_outSurface0 : register(u5);
RWTexture2D<float4> g_outSurface1 : register(u6);

SamplerState g_samp : register(s0);

#include "pbr_common.hlsl"

uint Hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

float Rand01(inout uint seed)
{
    seed = Hash(seed);
    return (seed & 0x00FFFFFFu) / 16777216.0;
}

void MakeBasis(float3 N, out float3 T, out float3 B)
{
    float3 up = (abs(N.y) < 0.999) ? float3(0, 1, 0) : float3(1, 0, 0);
    T = normalize(cross(up, N));
    B = normalize(cross(N, T));
}

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

ObjectData LoadObject(uint index)
{
    if (g_frameParity < 0.5)
        return g_objects0[index];
    return g_objects1[index];
}

bool RaySphere(float3 ro, float3 rd, float3 c, float r, out float t, out float3 n)
{
    float3 oc = ro - c;
    float b = dot(oc, rd);
    float c0 = dot(oc, oc) - r * r;
    float disc = b * b - c0;
    if (disc < 0.0)
        return false;
    float s = sqrt(disc);
    float t0 = -b - s;
    float t1 = -b + s;
    t = (t0 > 0.0) ? t0 : t1;
    if (t <= 0.0)
        return false;
    float3 hit = ro + rd * t;
    n = normalize(hit - c);
    return true;
}

bool RayAabb(float3 ro, float3 rd, float3 bmin, float3 bmax, out float t, out float3 n)
{
    float3 invD = 1.0 / rd;
    float3 t0 = (bmin - ro) * invD;
    float3 t1 = (bmax - ro) * invD;
    float3 tmin3 = min(t0, t1);
    float3 tmax3 = max(t0, t1);
    float tmin = max(max(tmin3.x, tmin3.y), tmin3.z);
    float tmax = min(min(tmax3.x, tmax3.y), tmax3.z);
    if (tmax < max(tmin, 0.0))
        return false;
    t = (tmin > 0.0) ? tmin : tmax;
    float3 hit = ro + rd * t;
    float3 center = (bmin + bmax) * 0.5;
    float3 local = hit - center;
    float3 a = abs(local);
    if (a.x > a.y && a.x > a.z)
        n = float3(sign(local.x), 0.0, 0.0);
    else if (a.y > a.z)
        n = float3(0.0, sign(local.y), 0.0);
    else
        n = float3(0.0, 0.0, sign(local.z));
    return true;
}

float3 ShadeHit(float3 hitPos, float3 hitN, float3 toViewer, ObjectData obj)
{
    float3 V = normalize(toViewer);
    float3 L = normalize(-g_lightDirWs);
    float3 radiance = g_lightColor * g_lightIntensity;
    float3 color = BRDF_UEStyle(hitN, V, L, obj.Albedo, obj.Metallic, obj.Roughness) * radiance;
    return ApplySimpleIBL(color, obj.Albedo, obj.Metallic, obj.Roughness, hitN, V, obj.AO);
}

[numthreads(8, 8, 1)]
void CSSWRTSurfaceCache(uint3 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_outSurface0.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h)
        return;

    float2 uv = (float2(tid.xy) + 0.5) * g_invGIResolution;
    float4 gb0 = g_gbuffer0.SampleLevel(g_samp, uv, 0);
    float4 gb1 = g_gbuffer1.SampleLevel(g_samp, uv, 0);
    g_outSurface0[tid.xy] = gb0;
    g_outSurface1[tid.xy] = gb1;
}

[numthreads(8, 8, 1)]
void CSSWRTGI(uint3 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_outHistory0.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h)
        return;

    const float2 uv = (float2(tid.xy) + 0.5) * g_invGIResolution;

    const float4 sc0 = g_surface0.SampleLevel(g_samp, uv, 0);
    const float4 sc1 = g_surface1.SampleLevel(g_samp, uv, 0);
    const float valid = sc1.a;
    const bool isValid = (valid > 0.0);

    const float3 N = isValid ? normalize(sc1.xyz) : float3(0, 1, 0);
    const float roughness = isValid ? saturate(valid - 1.0) : 0.5;
    const float3 albedo = isValid ? sc0.rgb : float3(0.0, 0.0, 0.0);
    const float3 posW = isValid ? g_gbuffer2.SampleLevel(g_samp, uv, 0).xyz : float3(0.0, 0.0, 0.0);
    const float ao = isValid ? saturate(g_gbuffer2.SampleLevel(g_samp, uv, 0).a) : 0.0;
    const float depth = isValid ? max(length(posW - g_cameraPosWs), 1e-3) : 0.0;

    if (g_historyValid > 0.5)
    {
        uint phase = (uint)g_frameIndex & 3u;
        uint2 update = uint2(phase & 1u, (phase >> 1u) & 1u);
        if ((tid.x & 1u) != update.x || (tid.y & 1u) != update.y)
        {
            const bool prev0 = (g_prevHistoryIndex < 0.5);
            float4 prevGI = prev0 ? g_giHistory0.Load(int3(tid.xy, 0)) : g_giHistory1.Load(int3(tid.xy, 0));
            float4 prevMeta = prev0 ? g_giMeta0.Load(int3(tid.xy, 0)) : g_giMeta1.Load(int3(tid.xy, 0));
            const bool write0 = (g_frameParity < 0.5);
            if (write0)
            {
                g_outHistory0[tid.xy] = prevGI;
                g_outMeta0[tid.xy] = prevMeta;
            }
            else
            {
                g_outHistory1[tid.xy] = prevGI;
                g_outMeta1[tid.xy] = prevMeta;
            }
            return;
        }
    }

    float3 indirect = float3(0.0, 0.0, 0.0);
    if (isValid)
    {
        uint seed = Hash(tid.x + (tid.y << 16) + (uint)(g_frameIndex * 1664525.0));
        const int rayCount = max(1, (int)round(g_raysPerPixel));
        [loop]
        for (int r = 0; r < rayCount; ++r)
        {
            float3 D = SampleHemisphereCos(N, seed);
            float bestT = g_maxTraceDistance;
            float3 bestN = float3(0.0, 0.0, 0.0);
            ObjectData bestObj;
            bool hit = false;

            const uint objCount = (uint)g_objectCount;
            [loop]
            for (uint i = 0; i < objCount; ++i)
            {
                ObjectData obj = LoadObject(i);
                float t = 0.0;
                float3 n = float3(0.0, 0.0, 0.0);
                if (obj.MeshType == 0)
                {
                    float s = max(obj.Scale.x, max(obj.Scale.y, obj.Scale.z));
                    float rS = obj.Radius * s;
                    if (RaySphere(posW + N * 0.02, D, obj.Position, rS, t, n) && t < bestT)
                    {
                        bestT = t;
                        bestN = n;
                        bestObj = obj;
                        hit = true;
                    }
                }
                else if (obj.MeshType == 1)
                {
                    float3 halfExt = float3(0.6, 0.6, 0.6) * obj.Scale;
                    float3 bmin = obj.Position - halfExt;
                    float3 bmax = obj.Position + halfExt;
                    if (RayAabb(posW + N * 0.02, D, bmin, bmax, t, n) && t < bestT)
                    {
                        bestT = t;
                        bestN = n;
                        bestObj = obj;
                        hit = true;
                    }
                }
                else
                {
                    float rS = 0.6 * max(obj.Scale.x, obj.Scale.z);
                    float hS = 1.4 * obj.Scale.y;
                    float r = sqrt((hS * 0.5) * (hS * 0.5) + rS * rS);
                    if (RaySphere(posW + N * 0.02, D, obj.Position, r, t, n) && t < bestT)
                    {
                        bestT = t;
                        bestN = n;
                        bestObj = obj;
                        hit = true;
                    }
                }
            }

            if (hit)
            {
                float3 hitPos = posW + D * bestT;
                float3 toViewer = (posW - hitPos);
                float3 hitRad = ShadeHit(hitPos, bestN, toViewer, bestObj);
                float NoD = saturate(dot(N, D));
                float3 brdfDiff = albedo / PI;
                indirect += hitRad * brdfDiff * NoD;
            }
            else
            {
                indirect += SampleSkyDiffuse(N) * (albedo / PI);
            }
        }
        indirect /= (float)rayCount;
        indirect *= (g_giIntensity * ao);
    }

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
void CSSWRTFilter(uint3 tid : SV_DispatchThreadID)
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
