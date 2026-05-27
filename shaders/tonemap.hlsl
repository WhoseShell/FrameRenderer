cbuffer TonemapCB : register(b0)
{
    float g_enableTonemap; // 1 = tonemap+gamma, 0 = gamma only
    float g_exposure;      // simple exposure multiplier
    float g_gamma;         // display gamma, e.g. 2.2
    float g_tonemapOperator; // 0 = none, 1 = Reinhard, 2 = AgX
    float g_targetWidth;
    float g_targetHeight;
    float g_invTargetWidth;
    float g_invTargetHeight;
};

Texture2D g_hdrTex : register(t0);
SamplerState g_samp : register(s0);

struct VSOut
{
    float4 posH : SV_Position;
    float2 uv   : TEXCOORD0;
};

/**
 * @brief 顶点着色器：生成全屏三角形。
 * @param vid 顶点索引（SV_VertexID）。
 * @return 全屏三角形的顶点位置与 UV。
 * @note 阶段：后处理顶点阶段。
 */
VSOut VSMain(uint vid : SV_VertexID)
{
    // Fullscreen triangle
    float2 pos = float2((vid == 2) ? 3.0 : -1.0, (vid == 1) ? 3.0 : -1.0);
    VSOut o;
    // 构建裁剪空间位置与 UV。
    o.posH = float4(pos, 0.0, 1.0);
    o.uv = float2((pos.x + 1.0) * 0.5, 1.0 - (pos.y + 1.0) * 0.5);
    return o;
}

/**
 * @brief Reinhard 色调映射算子。
 * @param x HDR 颜色。
 * @return 映射后的 LDR 颜色。
 * @note 阶段：后处理色调映射阶段。
 */
float3 Reinhard(float3 x)
{
    return x / (x + float3(1.0, 1.0, 1.0));
}

float3 AgXDefaultContrastApprox(float3 x)
{
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5 * x4 * x2
        - 40.14 * x4 * x
        + 31.96 * x4
        - 6.868 * x2 * x
        + 0.4298 * x2
        + 0.1191 * x
        - 0.00232;
}

float3 LinearSRGBToLinearRec2020(float3 v)
{
    return float3(
        dot(v, float3(0.6274, 0.3293, 0.0433)),
        dot(v, float3(0.0691, 0.9195, 0.0113)),
        dot(v, float3(0.0164, 0.0880, 0.8956)));
}

float3 LinearRec2020ToLinearSRGB(float3 v)
{
    return float3(
        dot(v, float3(1.6605, -0.5876, -0.0728)),
        dot(v, float3(-0.1246, 1.1329, -0.0083)),
        dot(v, float3(-0.0182, -0.1006, 1.1187)));
}

float3 AgXInsetTransform(float3 v)
{
    return float3(
        dot(v, float3(0.856627153315983, 0.0951212405381588, 0.0482516061458583)),
        dot(v, float3(0.137318972929847, 0.761241990602591, 0.101439036467562)),
        dot(v, float3(0.11189821299995, 0.0767994186031903, 0.811302368396859)));
}

float3 AgXOutsetTransform(float3 v)
{
    return float3(
        dot(v, float3(1.1271005818144368, -0.11060664309660323, -0.016493938717834573)),
        dot(v, float3(-0.1413297634984383, 1.157823702216272, -0.016493938717834257)),
        dot(v, float3(-0.14132976349843826, -0.11060664309660294, 1.2519364065950405)));
}

float3 AgXTonemap(float3 color)
{
    const float minEv = -12.47393;
    const float maxEv = 4.026069;

    color = LinearSRGBToLinearRec2020(color);
    color = AgXInsetTransform(color);
    color = log2(max(color, float3(1e-10, 1e-10, 1e-10)));
    color = saturate((color - minEv) / (maxEv - minEv));
    color = AgXDefaultContrastApprox(color);
    color = AgXOutsetTransform(color);
    color = pow(max(color, float3(0.0, 0.0, 0.0)), float3(2.2, 2.2, 2.2));
    color = LinearRec2020ToLinearSRGB(color);
    return saturate(color);
}

/**
 * @brief 像素着色器：对 HDR 颜色进行 Tonemap 与 Gamma。
 * @param i 插值后的顶点输出。
 * @return 输出 LDR 颜色。
 * @note 阶段：后处理像素阶段。
 */
float4 PSMain(VSOut i) : SV_Target
{
    // 采样 HDR 颜色并应用曝光。
    float2 sourceUv = saturate(i.posH.xy * float2(g_invTargetWidth, g_invTargetHeight));
    float3 hdr = g_hdrTex.Sample(g_samp, sourceUv).rgb;
    hdr = max(hdr, float3(0.0, 0.0, 0.0)) * max(g_exposure, 0.0);

    float3 ldr = hdr;
    if (g_enableTonemap > 0.5)
    {
        // Tonemap 分支。
        ldr = (g_tonemapOperator > 1.5) ? AgXTonemap(hdr) : Reinhard(hdr);
    }
    else
        ldr = saturate(hdr); // no tonemap: clamp only

    // Gamma 校正。
    const float invGamma = 1.0 / max(g_gamma, 1e-4);
    ldr = pow(ldr, invGamma);
    return float4(ldr, 1.0);
}

