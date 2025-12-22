// Deferred rendering: GBuffer (material + normal + world pos) + fullscreen lighting into HDR.

// --- GBuffer pass (runs with VS from pbr.hlsl; PS input must match VS output semantics) ---
cbuffer SceneCB : register(b0)
{
    row_major float4x4 g_mvp;
    row_major float4x4 g_world;
    row_major float4x4 g_worldInvTranspose;
    float3 g_cameraPosWs;
    float  _pad0;
    float3 g_lightDirWs;
    float  _pad1;
    float3 g_lightColor;
    float  g_lightIntensity;
    float3 g_albedo;
    float  g_metallic;
    float  g_roughness;
    float  g_useAlbedoTex;
    float  g_useNormalTex;
    float  g_useRoughnessTex;
    float  g_useMetallicTex;
    float  g_useAOTex;
    float3 _pad2;
};

Texture2D g_albedoTex : register(t0);
Texture2D g_normalTex : register(t1);
Texture2D g_roughnessTex : register(t2);
Texture2D g_metallicTex : register(t3);
Texture2D g_aoTex : register(t4);
SamplerState g_samp : register(s0);

struct PSIn
{
    float4 posH : SV_Position;
    float3 posW : TEXCOORD0;
    float3 nrmW : TEXCOORD1;
    float3 col  : COLOR;
    float2 uv   : TEXCOORD2;
};

struct GBufferOut
{
    float4 RT0 : SV_Target0; // albedo.rgb + metallic.a
    float4 RT1 : SV_Target1; // normal.xyz + (roughness+1).a (0 => empty)
    float4 RT2 : SV_Target2; // posW.xyz + ao.a
};

GBufferOut PSGBuffer(PSIn i)
{
    float3 N = normalize(i.nrmW);

    // Normal map (tangent basis approximated from world up)
    if (g_useNormalTex > 0.5)
    {
        float3 nm = g_normalTex.Sample(g_samp, i.uv).xyz * 2.0 - 1.0;
        float3 up = abs(N.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
        float3 T = normalize(cross(up, N));
        float3 B = normalize(cross(N, T));
        N = normalize(T * nm.x + B * nm.y + N * nm.z);
    }

    float3 albedoTex = g_albedoTex.Sample(g_samp, i.uv).rgb;
    float3 albedo = g_albedo * lerp(float3(1.0, 1.0, 1.0), albedoTex, g_useAlbedoTex);

    float roughTex = g_roughnessTex.Sample(g_samp, i.uv).r;
    float metalTex = g_metallicTex.Sample(g_samp, i.uv).r;
    float aoTex = g_aoTex.Sample(g_samp, i.uv).r;

    float metallic = saturate(lerp(g_metallic, metalTex, g_useMetallicTex));
    float roughness = saturate(lerp(g_roughness, roughTex, g_useRoughnessTex));
    float ao = saturate(lerp(1.0, aoTex, g_useAOTex));

    GBufferOut o;
    o.RT0 = float4(albedo, metallic);
    o.RT1 = float4(N, roughness + 1.0);
    o.RT2 = float4(i.posW, ao);
    return o;
}

// --- Deferred lighting pass (fullscreen triangle) ---
cbuffer DeferredCB : register(b0)
{
    float3 g_cameraPosWs_L;
    float  _padL0;
    float3 g_lightDirWs_L;
    float  _padL1;
    float3 g_lightColor_L;
    float  g_lightIntensity_L;
};

cbuffer ShadowCB : register(b1)
{
    row_major float4x4 g_lightViewProj;
    float2 g_shadowInvSize;
    float g_shadowBias;
    float g_shadowStrength;
};

Texture2D g_gbuffer0 : register(t0);
Texture2D g_gbuffer1 : register(t1);
Texture2D g_gbuffer2 : register(t2);
Texture2D<float> g_shadowMap : register(t5);
TextureCube g_skyPrefilter : register(t6);
StructuredBuffer<float4> g_skySH : register(t7);
SamplerComparisonState g_shadowSamp : register(s1);

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
#include "shadow_common.hlsl"

float4 PSDeferredLighting(VSFullOut i) : SV_Target
{
    float4 gb0 = g_gbuffer0.Sample(g_samp, i.uv);
    float4 gb1 = g_gbuffer1.Sample(g_samp, i.uv);
    float4 gb2 = g_gbuffer2.Sample(g_samp, i.uv);

    const float valid = gb1.a;
    if (valid <= 0.0)
        return float4(0.0, 0.0, 0.0, 0.0); // alpha=0 keeps sky/background
    const float roughness = saturate(valid - 1.0);

    const float3 albedo = gb0.rgb;
    const float metallic = saturate(gb0.a);
    const float3 N = normalize(gb1.xyz);
    const float3 posW = gb2.xyz;
    const float ao = saturate(gb2.a);

    const float3 V = normalize(g_cameraPosWs_L - posW);
    const float3 L = normalize(-g_lightDirWs_L);
    const float3 radiance = g_lightColor_L * g_lightIntensity_L;

    float shadow = ComputeShadowFactor(posW);
    float3 color = BRDF_UEStyle(N, V, L, albedo, metallic, roughness) * radiance * shadow;

    color = ApplySimpleIBL(color, albedo, metallic, roughness, N, V, ao);

    color = max(color, float3(0.0, 0.0, 0.0));
    return float4(color, 1.0);
}
