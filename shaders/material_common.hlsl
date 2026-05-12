#ifndef MATERIAL_COMMON_HLSL
#define MATERIAL_COMMON_HLSL

struct FDecodedMaterial
{
    float3 Normal;
    float3 Albedo;
    float Metallic;
    float Roughness;
    float AO;
    float IsUnlit;
    float3 UnlitColor;
};

float3 DecodeMaterialNormal(float3 normalW, float2 uv)
{
    float3 N = normalize(normalW);
    if (g_useNormalTex > 0.5)
    {
        float3 nm = g_normalTex.Sample(g_samp, uv).xyz * 2.0 - 1.0;
        float3 up = abs(N.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
        float3 T = normalize(cross(up, N));
        float3 B = normalize(cross(N, T));
        N = normalize(T * nm.x + B * nm.y + N * nm.z);
    }
    return N;
}

FDecodedMaterial DecodeSceneMaterial(float3 normalW, float2 uv)
{
    FDecodedMaterial m;
    m.Normal = DecodeMaterialNormal(normalW, uv);

    float3 albedoTex = g_albedoTex.Sample(g_samp, uv).rgb;
    float roughTex = g_roughnessTex.Sample(g_samp, uv).r;
    float metalTex = g_metallicTex.Sample(g_samp, uv).r;
    float aoTex = g_aoTex.Sample(g_samp, uv).r;

    m.Albedo = g_albedo * lerp(float3(1.0, 1.0, 1.0), albedoTex, g_useAlbedoTex);
    m.Metallic = saturate(lerp(g_metallic, metalTex, g_useMetallicTex));
    m.Roughness = saturate(lerp(g_roughness, roughTex, g_useRoughnessTex));
    m.AO = saturate(lerp(1.0, aoTex, g_useAOTex));
    m.IsUnlit = g_shadingMode;
    m.UnlitColor = max(m.Albedo * max(g_unlitIntensity, 0.0), float3(0.0, 0.0, 0.0));
    return m;
}

#endif
