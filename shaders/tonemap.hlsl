cbuffer TonemapCB : register(b0)
{
    float g_enableTonemap; // 1 = tonemap+gamma, 0 = gamma only
    float g_exposure;      // simple exposure multiplier
    float g_gamma;         // display gamma, e.g. 2.2
    float _pad0;
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

/**
 * @brief 像素着色器：对 HDR 颜色进行 Tonemap 与 Gamma。
 * @param i 插值后的顶点输出。
 * @return 输出 LDR 颜色。
 * @note 阶段：后处理像素阶段。
 */
float4 PSMain(VSOut i) : SV_Target
{
    // 采样 HDR 颜色并应用曝光。
    float3 hdr = g_hdrTex.Sample(g_samp, i.uv).rgb;
    hdr = max(hdr, float3(0.0, 0.0, 0.0)) * max(g_exposure, 0.0);

    float3 ldr = hdr;
    if (g_enableTonemap > 0.5)
        // Tonemap 分支。
        ldr = Reinhard(hdr);
    else
        ldr = saturate(hdr); // no tonemap: clamp only

    // Gamma 校正。
    const float invGamma = 1.0 / max(g_gamma, 1e-4);
    ldr = pow(ldr, invGamma);
    return float4(ldr, 1.0);
}

