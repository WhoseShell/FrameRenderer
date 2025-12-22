#ifndef PBR_COMMON_HLSL
#define PBR_COMMON_HLSL

static const float PI = 3.14159265;
float Pow5(float x)
{
    float x2 = x * x;
    return x2 * x2 * x;
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * Pow5(1.0 - cosTheta);
}

float D_GGX(float NdotH, float a)
{
    float a2 = a * a;
    float denom = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

float G_SmithGGX(float NdotV, float NdotL, float k)
{
    float gv = NdotV / (NdotV * (1.0 - k) + k);
    float gl = NdotL / (NdotL * (1.0 - k) + k);
    return gv * gl;
}

float3 BRDF_UEStyle(float3 N, float3 V, float3 L, float3 albedo, float metallic, float roughness)
{
    float3 H = normalize(V + L);

    float NdotV = saturate(dot(N, V));
    float NdotL = saturate(dot(N, L));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float a = max(roughness * roughness, 0.002);
    float k = (roughness + 1.0);
    k = (k * k) / 8.0;

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 F = FresnelSchlick(VdotH, F0);
    float  D = D_GGX(NdotH, a);
    float  G = G_SmithGGX(NdotV, NdotL, k);

    float3 spec = (D * G) * F / max(4.0 * NdotV * NdotL, 1e-4);
    float3 kS = F;
    float3 kD = (float3(1.0, 1.0, 1.0) - kS) * (1.0 - metallic);
    float3 diff = kD * albedo / PI;
    return (diff + spec) * NdotL;
}

float2 EnvBRDFApprox(float roughness, float NdotV)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}

float3 EvalSkySH(float3 dir)
{
    const float x = dir.x;
    const float y = dir.y;
    const float z = dir.z;

    float3 result = g_skySH[0].xyz * 0.282095;
    result += g_skySH[1].xyz * (-0.488603 * y);
    result += g_skySH[2].xyz * (0.488603 * z);
    result += g_skySH[3].xyz * (-0.488603 * x);
    result += g_skySH[4].xyz * (1.092548 * x * y);
    result += g_skySH[5].xyz * (-1.092548 * y * z);
    result += g_skySH[6].xyz * (0.315392 * (3.0 * z * z - 1.0));
    result += g_skySH[7].xyz * (-1.092548 * x * z);
    result += g_skySH[8].xyz * (0.546274 * (x * x - y * y));
    return result;
}

float3 SampleSkyDiffuse(float3 N)
{
    return max(EvalSkySH(N), float3(0.0, 0.0, 0.0));
}

float3 SampleSkySpecular(float3 R, float roughness)
{
    float maxMip = max(g_skySH[0].w, 0.0);
    float lod = saturate(roughness) * maxMip;
    return g_skyPrefilter.SampleLevel(g_samp, R, lod).rgb;
}

float3 ApplySimpleIBL(float3 color, float3 albedo, float metallic, float roughness, float3 N, float3 V, float ao)
{
    float NdotV = saturate(dot(N, V));
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float2 envBRDF = EnvBRDFApprox(roughness, NdotV);
    float3 envSpec = (F0 * envBRDF.x + envBRDF.y);

    float3 irradiance = SampleSkyDiffuse(N);
    float3 diffuse = (1.0 - metallic) * albedo * (irradiance / PI) * ao;

    float3 R = reflect(-V, N);
    float3 prefiltered = SampleSkySpecular(R, roughness);
    float3 specular = prefiltered * envSpec * ao;

    color += diffuse + specular;
    return color;
}

#endif
