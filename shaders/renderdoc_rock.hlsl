cbuffer RockFrame : register(b0)
{
    row_major float4x4 gViewProj;
    row_major float4x4 gRockWorld;
    row_major float4x4 gRockWorldInvTranspose;
    float4 gCameraPosition;
    float4 gSunDirection;
    float4 gSunColorIntensity;
    float4 gMaterialParams;
};

Texture2D gBaseColor : register(t0);
Texture2D gNormal : register(t1);
Texture2D gMaskA : register(t2);
Texture2D gMaskB : register(t3);
Texture2D gAux15308 : register(t4);
Texture2D gAux20264 : register(t5);
Texture2D gAux2597 : register(t6);
Texture2D gAux2592 : register(t7);
SamplerState gLinearWrap : register(s0);

static const float PI = 3.14159265;

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
    float4 Color : COLOR0;
};

struct VSOutput
{
    float4 Position : SV_POSITION;
    float3 WorldPosition : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float2 UV : TEXCOORD2;
    float4 Color : COLOR0;
};

float Pow5(float x)
{
    float x2 = x * x;
    return x2 * x2 * x;
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * Pow5(1.0 - cosTheta);
}

float DistributionGGX(float nDotH, float roughness)
{
    float a = max(roughness * roughness, 0.002);
    float a2 = a * a;
    float denom = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1.0e-5);
}

float GeometrySchlickGGX(float nDotV, float roughness)
{
    float k = roughness + 1.0;
    k = (k * k) * 0.125;
    return nDotV / max(nDotV * (1.0 - k) + k, 1.0e-5);
}

float GeometrySmith(float nDotV, float nDotL, float roughness)
{
    return GeometrySchlickGGX(nDotV, roughness) * GeometrySchlickGGX(nDotL, roughness);
}

float3 EvaluateBRDF(float3 n, float3 v, float3 l, float3 albedo, float metallic, float roughness)
{
    float3 h = normalize(v + l);
    float nDotV = saturate(dot(n, v));
    float nDotL = saturate(dot(n, l));
    float nDotH = saturate(dot(n, h));
    float vDotH = saturate(dot(v, h));

    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 f = FresnelSchlick(vDotH, f0);
    float d = DistributionGGX(nDotH, roughness);
    float g = GeometrySmith(nDotV, nDotL, roughness);

    float3 specular = (d * g) * f / max(4.0 * nDotV * nDotL, 1.0e-4);
    float3 diffuse = (1.0 - f) * (1.0 - metallic) * albedo / PI;
    return (diffuse + specular) * nDotL;
}

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    float3 worldPosition = mul(float4(input.Position, 1.0), gRockWorld).xyz;
    output.WorldPosition = worldPosition;
    output.Normal = normalize(mul(input.Normal, (float3x3)gRockWorldInvTranspose));
    output.UV = input.UV;
    output.Color = input.Color;
    output.Position = mul(float4(worldPosition, 1.0), gViewProj);
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float roughness = saturate(gMaterialParams.x);
    float metallic = saturate(gMaterialParams.y);
    float normalStrength = saturate(gMaterialParams.z);
    float baseBoost = max(gMaterialParams.w, 0.0);

    float4 baseSample = gBaseColor.Sample(gLinearWrap, input.UV);
    float4 maskA = gMaskA.Sample(gLinearWrap, input.UV);
    float4 maskB = gMaskB.Sample(gLinearWrap, input.UV);

    float3 albedo = pow(max(baseSample.rgb, 0.0), 2.2) * baseBoost;
    albedo *= lerp(float3(1.0, 1.0, 1.0), max(input.Color.rgb, 0.0), 0.35);
    roughness = saturate(lerp(roughness, 0.35 + 0.6 * maskA.g, 0.25));
    metallic = saturate(lerp(metallic, maskB.b, 0.10));
    float ao = saturate(lerp(1.0, maskA.r, 0.30));

    float3 n = normalize(input.Normal);
    float3 normalSample = gNormal.Sample(gLinearWrap, input.UV).xyz * 2.0 - 1.0;
    n = normalize(n + float3(normalSample.x, normalSample.y, normalSample.z) * normalStrength);

    float3 v = normalize(gCameraPosition.xyz - input.WorldPosition);
    float3 l = normalize(gSunDirection.xyz);
    float3 sun = gSunColorIntensity.rgb * gSunColorIntensity.a;
    float3 direct = EvaluateBRDF(n, v, l, albedo, metallic, roughness) * sun;

    float ambientIntensity = max(gSunDirection.w, 0.0);
    float3 ambient = albedo * ambientIntensity * ao;
    float3 color = direct * ao + ambient;
    return float4(color, 1.0);
}
