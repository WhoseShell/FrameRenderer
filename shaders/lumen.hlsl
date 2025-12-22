// Lumen (Lite): screen-space tracing GI + rough reflections with temporal accumulation.
// This is NOT UE5's full Lumen implementation; it mimics the "screen traces + temporal history"
// part to provide a lightweight, learning-friendly approximation.

cbuffer LumenCB : register(b0)
{
    row_major float4x4 g_viewProj;
    row_major float4x4 g_prevViewProj;

    float3 g_cameraPosWs;
    float  g_temporalWeight;

    float3 g_lightDirWs;
    float  g_maxTraceDistance;

    float3 g_lightColor;
    float  g_lightIntensity;

    float2 g_invResolution;
    float  g_stepSize;
    float  g_intensity;

    float  g_frameIndex;
    float  g_prevHistoryIndex;
    float  g_historyValid;
    float  _pad0;
};

Texture2D g_gbuffer0 : register(t0); // albedo.rgb + metallic.a
Texture2D g_gbuffer1 : register(t1); // normal.xyz + (roughness+1).a (0 => empty)
Texture2D g_gbuffer2 : register(t2); // posW.xyz + ao.a
Texture2D g_history0 : register(t3);
Texture2D g_history1 : register(t4);
TextureCube g_skyPrefilter : register(t26);
StructuredBuffer<float4> g_skySH : register(t27);

SamplerState g_samp : register(s0);

struct VSFullOut
{
    float4 posH : SV_Position;
    float2 uv   : TEXCOORD0;
};

VSFullOut VSFullscreen(uint vid : SV_VertexID)
{
    VSFullOut o;
    float2 pos[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
    float2 uv[3] = { float2(0.0, 1.0), float2(0.0, -1.0), float2(2.0, 1.0) };
    o.posH = float4(pos[vid], 0.0, 1.0);
    o.uv = uv[vid];
    return o;
}

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


bool ProjectToUV(float3 posW, out float2 uv)
{
    float4 clip = mul(float4(posW, 1.0), g_viewProj);
    if (clip.w <= 1e-4)
        return false;
    float2 ndc = clip.xy / clip.w;
    uv = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
    return true;
}

bool TraceScreen(float3 origin, float3 dir, out float2 hitUv)
{
    float t = max(g_stepSize, 1e-3);
    const int kMaxSteps = 40;
    [loop]
    for (int s = 0; s < kMaxSteps; ++s)
    {
        if (t > g_maxTraceDistance)
            return false;

        float3 p = origin + dir * t;

        float2 uv;
        if (!ProjectToUV(p, uv))
            return false;

        if (any(uv < 0.0) || any(uv > 1.0))
            return false;

        float4 gb1 = g_gbuffer1.SampleLevel(g_samp, uv, 0);
        if (gb1.a > 0.0)
        {
            float3 posS = g_gbuffer2.SampleLevel(g_samp, uv, 0).xyz;
            float d = length(posS - p);
            if (d < (g_stepSize * 2.0))
            {
                hitUv = uv;
                return true;
            }
        }

        t += g_stepSize;
    }
    return false;
}

float3 ShadeHit(float2 uv, float3 toViewerDir)
{
    float4 hb0 = g_gbuffer0.SampleLevel(g_samp, uv, 0);
    float4 hb1 = g_gbuffer1.SampleLevel(g_samp, uv, 0);
    float4 hb2 = g_gbuffer2.SampleLevel(g_samp, uv, 0);

    float valid = hb1.a;
    if (valid <= 0.0)
        return float3(0.0, 0.0, 0.0);

    float3 albedo = hb0.rgb;
    float metallic = saturate(hb0.a);
    float3 N = normalize(hb1.xyz);
    float roughness = saturate(valid - 1.0);
    float ao = saturate(hb2.a);

    float3 V = normalize(toViewerDir);
    float3 L = normalize(-g_lightDirWs);
    float3 radiance = g_lightColor * g_lightIntensity;

    float3 color = BRDF_UEStyle(N, V, L, albedo, metallic, roughness) * radiance;

    // small ambient term (keeps GI stable when the sun is weak)
    return ApplySimpleIBL(color, albedo, metallic, roughness, N, V, ao);
}

float4 SampleHistory(float2 uv, float prevIndex)
{
    if (prevIndex < 0.5)
        return g_history0.SampleLevel(g_samp, uv, 0);
    return g_history1.SampleLevel(g_samp, uv, 0);
}

struct PSLumenOut
{
    float4 AddHDR   : SV_Target0;
    float4 History  : SV_Target1;
};

PSLumenOut PSLumen(VSFullOut i)
{
    PSLumenOut o;
    o.AddHDR = float4(0.0, 0.0, 0.0, 0.0);
    o.History = float4(0.0, 0.0, 0.0, 0.0);

    float4 gb0 = g_gbuffer0.SampleLevel(g_samp, i.uv, 0);
    float4 gb1 = g_gbuffer1.SampleLevel(g_samp, i.uv, 0);
    float4 gb2 = g_gbuffer2.SampleLevel(g_samp, i.uv, 0);

    float valid = gb1.a;
    if (valid <= 0.0)
        return o;

    float3 albedo = gb0.rgb;
    float metallic = saturate(gb0.a);
    float3 N = normalize(gb1.xyz);
    float roughness = saturate(valid - 1.0);
    float3 posW = gb2.xyz;
    float ao = saturate(gb2.a);

    float3 V = normalize(g_cameraPosWs - posW);

    // RNG seed: pixel + frame
    uint2 pix = uint2(i.posH.xy);
    uint seed = Hash(pix.x + (pix.y << 16) + (uint)(g_frameIndex * 1664525.0));

    // Diffuse GI: 2 cosine-weighted hemisphere traces
    float3 indirectDiffuse = float3(0.0, 0.0, 0.0);
    const int kDiffuseRays = 2;
    [unroll]
    for (int r = 0; r < kDiffuseRays; ++r)
    {
        float3 D = SampleHemisphereCos(N, seed);
        float2 hitUv;
        if (TraceScreen(posW + N * 0.02, D, hitUv))
        {
            float3 hitPos = g_gbuffer2.SampleLevel(g_samp, hitUv, 0).xyz;
            float3 toViewer = (posW - hitPos);
            float3 hitRadiance = ShadeHit(hitUv, toViewer);

            float NoD = saturate(dot(N, D));
            float3 brdfDiff = albedo / PI;
            indirectDiffuse += hitRadiance * brdfDiff * NoD;
        }
        else
        {
            // Sky fallback
            indirectDiffuse += SampleSkyDiffuse(N) * (albedo / PI);
        }
    }
    indirectDiffuse /= (float)kDiffuseRays;

    // Rough reflections: 1 trace along reflection direction (jittered by roughness)
    float3 reflection = float3(0.0, 0.0, 0.0);
    {
        float3 R = reflect(-V, N);
        float jitter = (roughness * roughness);
        float3 D = normalize(lerp(R, SampleHemisphereCos(N, seed), jitter));
        float2 hitUv;
        if (TraceScreen(posW + N * 0.02, D, hitUv))
        {
            float3 hitPos = g_gbuffer2.SampleLevel(g_samp, hitUv, 0).xyz;
            float3 toViewer = (posW - hitPos);
            reflection = ShadeHit(hitUv, toViewer);
        }
        else
        {
            reflection = SampleSkySpecular(R, roughness);
        }

        float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
        float3 F = FresnelSchlick(saturate(dot(N, V)), F0);
        reflection *= F * (1.0 - roughness);
    }

    float3 cur = (indirectDiffuse + reflection) * (g_intensity * ao);

    // Temporal accumulation via reprojection
    float w = (g_historyValid > 0.5) ? saturate(g_temporalWeight) : 0.0;
    float3 accum = cur;
    {
        float4 clipPrev = mul(float4(posW, 1.0), g_prevViewProj);
        if (clipPrev.w > 1e-4)
        {
            float2 ndcPrev = clipPrev.xy / clipPrev.w;
            float2 uvPrev = float2(ndcPrev.x * 0.5 + 0.5, -ndcPrev.y * 0.5 + 0.5);
            if (all(uvPrev >= 0.0) && all(uvPrev <= 1.0))
            {
                float4 prev = SampleHistory(uvPrev, g_prevHistoryIndex);
                accum = lerp(cur, prev.rgb, w);
            }
        }
    }

    o.AddHDR = float4(accum, 1.0);
    o.History = float4(accum, 1.0);
    return o;
}
