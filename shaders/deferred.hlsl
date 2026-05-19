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
    float  g_shadingMode;
    float  g_unlitIntensity;
    float  g_rockNormalStrength;
    float  g_rockBaseColorBoost;
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

#include "material_common.hlsl"

/**
 * @brief GBuffer 像素着色器：输出材质与几何信息。
 * @param i 插值后的像素输入。
 * @return GBuffer 输出（RT0/RT1/RT2）。
 * @note 阶段：延迟渲染 GBuffer 阶段。
 */
GBufferOut PSGBuffer(PSIn i, bool isFrontFace : SV_IsFrontFace)
{
    FDecodedMaterial material = DecodeSceneMaterial(i.nrmW, i.uv, i.col);
    if (material.AlphaClip > 0.5)
        clip(material.Alpha - 0.33);
    float3 N = material.Normal;
    if (material.IsTwoSided > 0.5 && !isFrontFace)
        N = -N;

    // Normal map (tangent basis approximated from world up)
    if (false)
    {
        // 根据法线贴图修正法线。
        float3 nm = g_normalTex.Sample(g_samp, i.uv).xyz * 2.0 - 1.0;
        float3 up = abs(N.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
        float3 T = normalize(cross(up, N));
        float3 B = normalize(cross(N, T));
        N = normalize(T * nm.x + B * nm.y + N * nm.z);
    }

    float3 albedo = material.Albedo;

    float metallic = material.Metallic;
    float roughness = material.Roughness;
    float ao = material.AO;

    GBufferOut o;
    if (material.IsUnlit > 0.5)
    {
        o.RT0 = float4(material.UnlitColor, -1.0);
        o.RT1 = float4(N, 1.0);
        o.RT2 = float4(i.posW, ao);
        return o;
    }
    // 打包 GBuffer 输出。
    o.RT0 = float4(albedo, metallic);
    o.RT1 = float4(N, roughness + ((material.IsTwoSided > 0.5) ? 2.0 : 1.0));
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

/**
 * @brief 顶点着色器：生成全屏三角形用于延迟光照。
 * @param vid 顶点索引（SV_VertexID）。
 * @return 顶点输出（位置/UV）。
 * @note 阶段：延迟光照顶点阶段。
 */
VSFullOut VSFullscreen(uint vid : SV_VertexID)
{
    VSFullOut o;
    float2 pos[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
    float2 uv[3] = { float2(0.0, 1.0), float2(0.0, -1.0), float2(2.0, 1.0) };
    // 输出全屏三角形位置与 UV。
    o.posH = float4(pos[vid], 0.0, 1.0);
    o.uv = uv[vid];
    return o;
}

#include "pbr_common.hlsl"
#include "shadow_common.hlsl"

/**
 * @brief 延迟光照像素着色器：从 GBuffer 计算光照。
 * @param i 插值后的顶点输出。
 * @return 输出 HDR 颜色。
 * @note 阶段：延迟光照像素阶段。
 */
float4 PSDeferredLighting(VSFullOut i) : SV_Target
{
    int2 pixel = int2(i.posH.xy);
    float4 gb0 = g_gbuffer0.Load(int3(pixel, 0));
    float4 gb1 = g_gbuffer1.Load(int3(pixel, 0));
    float4 gb2 = g_gbuffer2.Load(int3(pixel, 0));

    const float valid = gb1.a;
    if (valid <= 0.0)
        return float4(0.0, 0.0, 0.0, 0.0); // alpha=0 keeps sky/background
    if (gb0.a < 0.0)
        return float4(max(gb0.rgb, float3(0.0, 0.0, 0.0)), 1.0);
    const bool isFoliage = valid > 2.0;
    const float roughness = saturate(valid - (isFoliage ? 2.0 : 1.0));

    const float3 albedo = gb0.rgb;
    const float metallic = saturate(gb0.a);
    const float3 N = normalize(gb1.xyz);
    const float3 posW = gb2.xyz;
    const float ao = saturate(gb2.a);

    const float3 V = normalize(g_cameraPosWs_L - posW);
    const float3 L = normalize(-g_lightDirWs_L);
    const float3 radiance = g_lightColor_L * g_lightIntensity_L;

    float shadow = ComputeShadowFactor(posW);
    if (isFoliage)
        shadow = 1.0;
    float3 color = BRDF_UEStyle(N, V, L, albedo, metallic, roughness) * radiance * shadow;

    // 叠加 IBL 环境光。
    color = ApplySimpleIBL(color, albedo, metallic, roughness, N, V, ao);

    color = max(color, float3(0.0, 0.0, 0.0));
    return float4(color, 1.0);
}
