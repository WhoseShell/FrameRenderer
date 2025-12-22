#ifndef PBR_COMMON_HLSL
#define PBR_COMMON_HLSL

static const float PI = 3.14159265;
/**
 * @brief 计算 x^5，用于 Fresnel 近似。
 * @param x 输入值。
 * @return x 的五次方。
 * @note 阶段：PBR 基础函数。
 */
float Pow5(float x)
{
    float x2 = x * x;
    return x2 * x2 * x;
}

/**
 * @brief Schlick Fresnel 近似。
 * @param cosTheta 入射角余弦。
 * @param F0 基础反射率。
 * @return Fresnel 结果。
 * @note 阶段：PBR BRDF 计算阶段。
 */
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * Pow5(1.0 - cosTheta);
}

/**
 * @brief GGX 法线分布函数 D。
 * @param NdotH 法线与半程向量点积。
 * @param a 粗糙度参数（alpha）。
 * @return 分布函数值。
 * @note 阶段：PBR BRDF 计算阶段。
 */
float D_GGX(float NdotH, float a)
{
    float a2 = a * a;
    float denom = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

/**
 * @brief Smith GGX 几何遮蔽项。
 * @param NdotV 法线与视线点积。
 * @param NdotL 法线与光线点积。
 * @param k 几何项参数。
 * @return 遮蔽项结果。
 * @note 阶段：PBR BRDF 计算阶段。
 */
float G_SmithGGX(float NdotV, float NdotL, float k)
{
    float gv = NdotV / (NdotV * (1.0 - k) + k);
    float gl = NdotL / (NdotL * (1.0 - k) + k);
    return gv * gl;
}

/**
 * @brief UE 风格 PBR BRDF（漫反射+高光）。
 * @param N 法线。
 * @param V 视线方向。
 * @param L 光线方向。
 * @param albedo 反照率。
 * @param metallic 金属度。
 * @param roughness 粗糙度。
 * @return 该光线方向的 BRDF 结果。
 * @note 阶段：PBR 光照计算阶段。
 */
float3 BRDF_UEStyle(float3 N, float3 V, float3 L, float3 albedo, float metallic, float roughness)
{
    float3 H = normalize(V + L);

    // 计算各向量点积。
    float NdotV = saturate(dot(N, V));
    float NdotL = saturate(dot(N, L));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    // GGX 参数与几何项。
    float a = max(roughness * roughness, 0.002);
    float k = (roughness + 1.0);
    k = (k * k) / 8.0;

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 F = FresnelSchlick(VdotH, F0);
    float  D = D_GGX(NdotH, a);
    float  G = G_SmithGGX(NdotV, NdotL, k);

    // 组合漫反射与高光项。
    float3 spec = (D * G) * F / max(4.0 * NdotV * NdotL, 1e-4);
    float3 kS = F;
    float3 kD = (float3(1.0, 1.0, 1.0) - kS) * (1.0 - metallic);
    float3 diff = kD * albedo / PI;
    return (diff + spec) * NdotL;
}

/**
 * @brief 环境 BRDF 近似（用于 IBL）。
 * @param roughness 粗糙度。
 * @param NdotV 法线与视线点积。
 * @return 近似系数。
 * @note 阶段：IBL 近似计算阶段。
 */
float2 EnvBRDFApprox(float roughness, float NdotV)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}

/**
 * @brief 评估天空球谐系数得到环境光。
 * @param dir 方向向量。
 * @return 环境光颜色。
 * @note 阶段：IBL 采样阶段。
 */
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

/**
 * @brief 采样天空漫反射（SH）。
 * @param N 法线方向。
 * @return 漫反射环境光。
 * @note 阶段：IBL 采样阶段。
 */
float3 SampleSkyDiffuse(float3 N)
{
    return max(EvalSkySH(N), float3(0.0, 0.0, 0.0));
}

/**
 * @brief 采样天空镜面反射（预滤波立方体）。
 * @param R 反射方向。
 * @param roughness 粗糙度。
 * @return 镜面环境光。
 * @note 阶段：IBL 采样阶段。
 */
float3 SampleSkySpecular(float3 R, float roughness)
{
    float maxMip = max(g_skySH[0].w, 0.0);
    float lod = saturate(roughness) * maxMip;
    return g_skyPrefilter.SampleLevel(g_samp, R, lod).rgb;
}

/**
 * @brief 叠加简单 IBL 结果到当前颜色。
 * @param color 当前颜色。
 * @param albedo 反照率。
 * @param metallic 金属度。
 * @param roughness 粗糙度。
 * @param N 法线。
 * @param V 视线方向。
 * @param ao 环境遮蔽。
 * @return 叠加后的颜色。
 * @note 阶段：IBL 合成阶段。
 */
float3 ApplySimpleIBL(float3 color, float3 albedo, float metallic, float roughness, float3 N, float3 V, float ao)
{
    float NdotV = saturate(dot(N, V));
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float2 envBRDF = EnvBRDFApprox(roughness, NdotV);
    float3 envSpec = (F0 * envBRDF.x + envBRDF.y);

    // 漫反射 IBL。
    float3 irradiance = SampleSkyDiffuse(N);
    float3 diffuse = (1.0 - metallic) * albedo * (irradiance / PI) * ao;

    // 镜面 IBL。
    float3 R = reflect(-V, N);
    float3 prefiltered = SampleSkySpecular(R, roughness);
    float3 specular = prefiltered * envSpec * ao;

    color += diffuse + specular;
    return color;
}

#endif
