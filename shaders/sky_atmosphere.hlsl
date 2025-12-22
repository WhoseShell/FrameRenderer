cbuffer SkyCB : register(b0)
{
    row_major float4x4 g_invViewProj;
    float3 g_cameraPosWs;
    float  g_atmosphereHeight; // world units (Y up), ground at y=0, top at y=height

    float3 g_sunDirWs;         // direction the sun points (world)
    float  g_sunIntensity;

    float3 g_sunColor;
    float  g_rayleighScale;

    float  g_mieScale;
    float  g_mieG;
    float  g_groundAlbedo;
    float  _pad0;
};

struct VSOut
{
    float4 posH : SV_Position;
    float2 ndc  : TEXCOORD0; // [-1,1] NDC
};

/**
 * @brief 顶点着色器：生成全屏三角形并输出 NDC。
 * @param vid 顶点索引（SV_VertexID）。
 * @return 顶点输出（裁剪空间位置与 NDC）。
 * @note 阶段：天空渲染顶点阶段。
 */
VSOut VSMain(uint vid : SV_VertexID)
{
    // Fullscreen triangle
    float2 pos = float2((vid == 2) ? 3.0 : -1.0, (vid == 1) ? 3.0 : -1.0);
    VSOut o;
    // 构建裁剪空间位置与 NDC。
    o.posH = float4(pos, 0.0, 1.0);
    o.ndc = pos;
    return o;
}

static const float PI = 3.14159265;

/**
 * @brief Rayleigh 相位函数。
 * @param cosTheta 入射角余弦。
 * @return 相位函数值。
 * @note 阶段：天空散射计算阶段。
 */
float RayleighPhase(float cosTheta)
{
    return 3.0 / (16.0 * PI) * (1.0 + cosTheta * cosTheta);
}

/**
 * @brief Henyey-Greenstein 相位函数（Mie）。
 * @param cosTheta 入射角余弦。
 * @param g 各向异性系数。
 * @return 相位函数值。
 * @note 阶段：天空散射计算阶段。
 */
float HenyeyGreenstein(float cosTheta, float g)
{
    float g2 = g * g;
    float denom = pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5);
    return 1.0 / (4.0 * PI) * (1.0 - g2) / denom;
}

// Very small, fast single-scattering approximation (flat atmosphere, exponential densities).
/**
 * @brief 计算简化单次散射天空颜色。
 * @param rayOrigin 视线起点（世界空间）。
 * @param rayDir 视线方向（归一化）。
 * @param sunDir 太阳方向（归一化）。
 * @return 估算的天空颜色。
 * @note 阶段：天空散射计算阶段。
 */
float3 ComputeSky(float3 rayOrigin, float3 rayDir, float3 sunDir)
{
    // If looking below the horizon, return a simple ground tint.
    if (rayDir.y <= 0.0)
    {
        const float t = saturate(-rayDir.y * 4.0);
        return lerp(float3(0.08, 0.08, 0.08), float3(0.10, 0.10, 0.10) * g_groundAlbedo, t);
    }

    const float topY = g_atmosphereHeight;
    const float tMax = max((topY - rayOrigin.y) / max(rayDir.y, 1e-4), 0.0);
    if (tMax <= 0.0)
        return float3(0.0, 0.0, 0.0);

    // 散射系数（艺术化缩放）。
    // Scattering coefficients (artist-friendly scaling)
    const float3 betaR = float3(0.0058, 0.0135, 0.0331) * max(g_rayleighScale, 0.0);
    const float3 betaM = float3(0.003996, 0.003996, 0.003996) * max(g_mieScale, 0.0);

    // 大气尺度高度（世界单位）。
    // Scale heights (world units)
    const float HR = max(topY * 0.22, 0.5);
    const float HM = max(topY * 0.06, 0.2);

    const int VIEW_STEPS = 16;
    const int SUN_STEPS = 8;
    const float dt = tMax / VIEW_STEPS;

    float3 sum = float3(0.0, 0.0, 0.0);
    float odR = 0.0;
    float odM = 0.0;

    const float cosTheta = saturate(dot(rayDir, sunDir));
    const float phaseR = RayleighPhase(cosTheta);
    const float phaseM = HenyeyGreenstein(cosTheta, clamp(g_mieG, 0.0, 0.99));

    for (int s = 0; s < VIEW_STEPS; ++s)
    {
        float t = (s + 0.5) * dt;
        float3 p = rayOrigin + rayDir * t;
        float h = saturate(p.y / topY) * topY;

        float dR = exp(-h / HR);
        float dM = exp(-h / HM);
        odR += dR * dt;
        odM += dM * dt;

        // 从采样点到太阳方向的光学厚度。
        // Optical depth to sun from sample
        float tSunMax = max((topY - p.y) / max(sunDir.y, 1e-4), 0.0);
        float dtSun = tSunMax / SUN_STEPS;
        float odSunR = 0.0;
        float odSunM = 0.0;
        for (int j = 0; j < SUN_STEPS; ++j)
        {
            float ts = (j + 0.5) * dtSun;
            float3 ps = p + sunDir * ts;
            float hs = saturate(ps.y / topY) * topY;
            odSunR += exp(-hs / HR) * dtSun;
            odSunM += exp(-hs / HM) * dtSun;
        }

        float3 tau = betaR * (odR + odSunR) + betaM * (odM + odSunM);
        float3 attn = exp(-tau);

        float3 inscatter = (dR * betaR * phaseR + dM * betaM * phaseM);
        sum += inscatter * attn * dt;
    }

    // 太阳辐射缩放。
    // Sun radiance; keep a moderate scale.
    return sum * g_sunColor * g_sunIntensity;
}

/**
 * @brief 像素着色器：重建视线并计算天空颜色。
 * @param i 插值后的顶点输出。
 * @return 输出颜色（HDR）。
 * @note 阶段：天空渲染像素阶段。
 */
float4 PSMain(VSOut i) : SV_Target
{
    // 反投影生成世界空间视线。
    // Reconstruct a world-space view ray via inverse view-projection.
    float2 ndc = i.ndc;
    float4 nearH = mul(float4(ndc, 0.0, 1.0), g_invViewProj);
    float4 farH  = mul(float4(ndc, 1.0, 1.0), g_invViewProj);
    float3 nearW = nearH.xyz / max(nearH.w, 1e-6);
    float3 farW  = farH.xyz / max(farH.w, 1e-6);
    float3 dir = normalize(farW - nearW);

    // UE 使用“光线方向”，天空需要“指向太阳”的方向。
    // UE uses "direction the light points"; for sky we want "to sun".
    // g_sunDirWs is the direction the light rays travel; sun is in the opposite direction.
    float3 toSun = normalize(-g_sunDirWs);
    float3 sky = ComputeSky(g_cameraPosWs, dir, toSun);

    // 叠加一个小太阳盘便于观察。
    // Add a small sun disk for visual feedback
    float sunCos = dot(dir, toSun);
    float sunDisk = smoothstep(cos(radians(0.6)), cos(radians(0.2)), sunCos);
    sky += g_sunColor * g_sunIntensity * sunDisk * 0.02;

    return float4(max(sky, float3(0.0, 0.0, 0.0)), 1.0);
}
