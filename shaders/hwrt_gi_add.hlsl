// HWRT GI upsample + additive blend into HDR (deferred-only).
// Reads half-res filtered GI and probe meta (normal+depth) and applies a small bilateral upsample.

cbuffer HWRTGICB : register(b0)
{
    row_major float4x4 g_viewProj;
    row_major float4x4 g_prevViewProj;

    float3 g_cameraPosWs;
    float  g_temporalWeight;

    float3 g_lightDirWs;
    float  g_maxTraceDistance;

    float3 g_lightColor;
    float  g_lightIntensity;

    float2 g_invFullResolution;
    float2 g_invGIResolution;

    float  g_frameIndex;
    float  g_frameParity;
    float  g_prevHistoryIndex;
    float  g_historyValid;

    float  g_giIntensity;
    float  g_depthReject;
    float  g_normalReject;
    float  g_raysPerPixel;
    float  g_objectCount;
    float  _pad0;
    float  _pad1;
    float  _pad2;
};

Texture2D g_gbuffer0 : register(t0);
Texture2D g_gbuffer1 : register(t1);
Texture2D g_gbuffer2 : register(t2);
Texture2D g_unused3  : register(t3);
Texture2D g_unused4  : register(t4);
Texture2D g_unused5  : register(t5);
Texture2D g_unused6  : register(t6);
Texture2D g_unused7  : register(t7);
Texture2D g_unused8  : register(t8);
Texture2D g_unused9  : register(t9);
Texture2D g_unused10 : register(t10);
Texture2D g_unused11 : register(t11);
Texture2D g_unused12 : register(t12);
Texture2D g_unused13 : register(t13);
Texture2D g_unused14 : register(t14);
Texture2D g_unused15 : register(t15);
Texture2D g_unused16 : register(t16);
Texture2D g_giMeta0  : register(t17);
Texture2D g_giMeta1  : register(t18);
Texture2D g_giFiltered : register(t19);

SamplerState g_samp : register(s0);

struct VSFullOut
{
    float4 posH : SV_Position;
    float2 uv   : TEXCOORD0;
};

/**
 * @brief 顶点着色器：生成全屏三角形。
 * @param vid 顶点索引（SV_VertexID）。
 * @return 顶点输出（位置/UV）。
 * @note 阶段：GI 合成顶点阶段。
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

/**
 * @brief 读取半分辨率 GI。
 * @param giUv GI 纹理 UV。
 * @return GI 颜色。
 * @note 阶段：GI 采样阶段。
 */
float3 ReadGI(float2 giUv)
{
    return g_giFiltered.SampleLevel(g_samp, giUv, 0).rgb;
}

/**
 * @brief 读取 GI 元数据（法线+深度）。
 * @param giUv GI 纹理 UV。
 * @return 元数据。
 * @note 阶段：GI 采样阶段。
 */
float4 ReadMeta(float2 giUv)
{
    return (g_frameParity < 0.5) ? g_giMeta0.SampleLevel(g_samp, giUv, 0) : g_giMeta1.SampleLevel(g_samp, giUv, 0);
}

/**
 * @brief 像素着色器：双边上采样 GI 并合成。
 * @param i 插值后的顶点输出。
 * @return 输出 GI 颜色。
 * @note 阶段：GI 合成像素阶段。
 */
float4 PSAddGI(VSFullOut i) : SV_Target
{
    int2 pixel = int2(i.posH.xy);
    float2 fullUv = (float2(pixel) + 0.5) * g_invFullResolution;
    float4 gb1 = g_gbuffer1.Load(int3(pixel, 0));
    const float valid = gb1.a;
    if (valid <= 0.0)
        return float4(0.0, 0.0, 0.0, 0.0);

    float3 N = normalize(gb1.xyz);
    float3 posW = g_gbuffer2.Load(int3(pixel, 0)).xyz;
    float depth = max(length(posW - g_cameraPosWs), 1e-3);

    float2 giDim = 1.0 / max(g_invGIResolution, float2(1e-6, 1e-6));
    float2 giPos = fullUv * giDim - 0.5;
    int2 base = int2(floor(giPos));
    float2 f = frac(giPos);

    float3 sum = float3(0.0, 0.0, 0.0);
    float wsum = 0.0;

    // 2x2 邻域双边权重融合。
    [unroll]
    for (int dy = 0; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = 0; dx <= 1; ++dx)
        {
            int2 p = base + int2(dx, dy);
            p = clamp(p, int2(0, 0), int2((int)giDim.x - 1, (int)giDim.y - 1));
            float2 giUv = (float2(p) + 0.5) / giDim;

            float3 gi = ReadGI(giUv);
            float4 meta = ReadMeta(giUv);
            float dP = meta.w;
            if (dP <= 0.0)
                continue;

            float3 nP = normalize(meta.xyz);

            float wBilinear = (dx == 0 ? (1.0 - f.x) : f.x) * (dy == 0 ? (1.0 - f.y) : f.y);
            float wNormal = pow(saturate(dot(N, nP)), 8.0);
            float wDepth = exp(-abs(dP - depth) / max(depth * 0.1, 1e-3));

            float w = wBilinear * wNormal * wDepth;
            sum += gi * w;
            wsum += w;
        }
    }

    float3 outGI = (wsum > 1e-4) ? (sum / wsum) : ReadGI(fullUv);
    return float4(outGI, 1.0);
}
