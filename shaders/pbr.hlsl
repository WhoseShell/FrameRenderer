cbuffer SceneCB : register(b0)
{
    row_major float4x4 g_mvp;
    row_major float4x4 g_world;
    row_major float4x4 g_worldInvTranspose;
    float3 g_cameraPosWs;
    float  _pad0;
    float3 g_lightDirWs;   // direction the light rays travel (world), e.g. (0,-1,0)
    float  _pad1;
    float3 g_lightColor;   // radiance-ish
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

cbuffer ShadowCB : register(b1)
{
    row_major float4x4 g_lightViewProj;
    float2 g_shadowInvSize;
    float g_shadowBias;
    float g_shadowStrength;
};

Texture2D g_albedoTex : register(t0);
Texture2D g_normalTex : register(t1);
Texture2D g_roughnessTex : register(t2);
Texture2D g_metallicTex : register(t3);
Texture2D g_aoTex : register(t4);
Texture2D<float> g_shadowMap : register(t5);
TextureCube g_skyPrefilter : register(t6);
StructuredBuffer<float4> g_skySH : register(t7);
SamplerState g_samp : register(s0);
SamplerComparisonState g_shadowSamp : register(s1);

struct VSIn
{
    float3 pos : POSITION;
    float3 col : COLOR;
    float3 nrm : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct PSIn
{
    float4 posH : SV_Position;
    float3 posW : TEXCOORD0;
    float3 nrmW : TEXCOORD1;
    float3 col  : COLOR;
    float2 uv   : TEXCOORD2;
};

PSIn VSMain(VSIn i)
{
    PSIn o;
    float4 posW = mul(float4(i.pos, 1.0), g_world);
    o.posW = posW.xyz;
    o.nrmW = normalize(mul(i.nrm, (float3x3)g_worldInvTranspose));
    o.posH = mul(float4(i.pos, 1.0), g_mvp);
    o.col = i.col;
    o.uv = i.uv;
    return o;
}

#include "pbr_common.hlsl"
#include "shadow_common.hlsl"

float4 PSMain(PSIn i) : SV_Target
{
    float3 N = normalize(i.nrmW);
    float3 V = normalize(g_cameraPosWs - i.posW);

    // Normal map (tangent basis approximated from world up)
    if (g_useNormalTex > 0.5)
    {
        float3 nm = g_normalTex.Sample(g_samp, i.uv).xyz * 2.0 - 1.0;
        float3 up = abs(N.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
        float3 T = normalize(cross(up, N));
        float3 B = normalize(cross(N, T));
        N = normalize(T * nm.x + B * nm.y + N * nm.z);
    }

    float3 L = normalize(-g_lightDirWs); // to-light
    float3 radiance = g_lightColor * g_lightIntensity;

    float3 albedoTex = g_albedoTex.Sample(g_samp, i.uv).rgb;
    float3 albedo = g_albedo * lerp(float3(1.0, 1.0, 1.0), albedoTex, g_useAlbedoTex);

    float roughTex = g_roughnessTex.Sample(g_samp, i.uv).r;
    float metalTex = g_metallicTex.Sample(g_samp, i.uv).r;
    float aoTex = g_aoTex.Sample(g_samp, i.uv).r;

    float metallic = saturate(lerp(g_metallic, metalTex, g_useMetallicTex));
    float roughness = saturate(lerp(g_roughness, roughTex, g_useRoughnessTex));
    float ao = saturate(lerp(1.0, aoTex, g_useAOTex));

    float shadow = ComputeShadowFactor(i.posW);
    float3 color = BRDF_UEStyle(N, V, L, albedo, metallic, roughness) * radiance * shadow;

    // Ambient: tiny diffuse + simple specular IBL approximation.
    color = ApplySimpleIBL(color, albedo, metallic, roughness, N, V, ao);

    // Output linear HDR; tonemapping happens in a separate post-process pass.
    color = max(color, float3(0.0, 0.0, 0.0));
    return float4(color, 1.0);
}
