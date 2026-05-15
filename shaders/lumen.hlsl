// Lumen (Lite): screen-space tracing GI + rough reflections with temporal accumulation.
// This is NOT UE5's full Lumen implementation; it mimics the "screen traces + temporal history"
// part to provide a lightweight, learning-friendly approximation.

cbuffer LumenCB : register(b0)
{
    row_major float4x4 g_viewProj;
    row_major float4x4 g_prevViewProj;

    float3 g_cameraPosWs;
    float  g_temporalWeight;

    float3 g_lightDirWs;
    float  g_maxTraceDistance;

    float3 g_lightColor;
    float  g_lightIntensity;

    float2 g_invResolution;
    float2 g_viewportOrigin;

    float2 g_viewportSize;
    float  g_stepSize;
    float  g_intensity;

    float  g_frameIndex;
    float  g_prevHistoryIndex;
    float  g_historyValid;
    float  _pad0;
};

Texture2D g_gbuffer0 : register(t0); // albedo.rgb + metallic.a
Texture2D g_gbuffer1 : register(t1); // normal.xyz + (roughness+1).a (0 => empty)
Texture2D g_gbuffer2 : register(t2); // posW.xyz + ao.a
Texture2D g_history0 : register(t3);
Texture2D g_history1 : register(t4);
TextureCube g_skyPrefilter : register(t26);
StructuredBuffer<float4> g_skySH : register(t27);

SamplerState g_samp : register(s0);

float2 ViewportUVToFullUV(float2 uv)
{
    return (g_viewportOrigin + uv * g_viewportSize) * g_invResolution;
}

int2 ViewportUVToPixel(float2 uv)
{
    return int2(g_viewportOrigin + uv * g_viewportSize);
}

struct VSFullOut
{
    float4 posH : SV_Position;
    float2 uv   : TEXCOORD0;
};

/**
 * @brief 顶点着色器：生成全屏三角形。
 * @param vid 顶点索引（SV_VertexID）。
 * @return 顶点输出（位置/UV）。
 * @note 阶段：Lumen 顶点阶段。
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

/**
 * @brief 哈希函数，用于随机种子扰动。
 * @param x 输入值。
 * @return 哈希后的值。
 * @note 阶段：随机序列生成阶段。
 */
uint Hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

/**
 * @brief 生成 [0,1) 的随机数。
 * @param seed 输入/输出随机种子。
 * @return 随机浮点数。
 * @note 阶段：随机采样阶段。
 */
float Rand01(inout uint seed)
{
    seed = Hash(seed);
    return (seed & 0x00FFFFFFu) / 16777216.0;
}

/**
 * @brief 构建法线空间正交基。
 * @param N 法线方向。
 * @param T 输出切线方向。
 * @param B 输出副切线方向。
 * @note 阶段：采样方向生成阶段。
 */
void MakeBasis(float3 N, out float3 T, out float3 B)
{
    float3 up = (abs(N.y) < 0.999) ? float3(0, 1, 0) : float3(1, 0, 0);
    T = normalize(cross(up, N));
    B = normalize(cross(N, T));
}

/**
 * @brief 余弦加权半球采样方向。
 * @param N 法线方向。
 * @param seed 输入/输出随机种子。
 * @return 采样方向。
 * @note 阶段：GI 采样阶段。
 */
float3 SampleHemisphereCos(float3 N, inout uint seed)
{
    float r1 = Rand01(seed);
    float r2 = Rand01(seed);
    float phi = 2.0 * PI * r1;
    float cosTheta = sqrt(1.0 - r2);
    float sinTheta = sqrt(r2);

    float3 T, B;
    MakeBasis(N, T, B);

    float3 L = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    return normalize(T * L.x + B * L.y + N * L.z);
}


/**
 * @brief 将世界坐标投影到屏幕 UV。
 * @param posW 世界坐标位置。
 * @param uv 输出 UV。
 * @return 是否成功投影。
 * @note 阶段：屏幕空间追踪阶段。
 */
bool ProjectToUV(float3 posW, out float2 uv)
{
    float4 clip = mul(float4(posW, 1.0), g_viewProj);
    if (clip.w <= 1e-4)
        return false;
    float2 ndc = clip.xy / clip.w;
    uv = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
    return true;
}

/**
 * @brief 屏幕空间射线追踪（步进采样）。
 * @param origin 起点（世界空间）。
 * @param dir 方向（归一化）。
 * @param hitUv 输出命中 UV。
 * @return 是否命中。
 * @note 阶段：Lumen 屏幕空间追踪阶段。
 */
bool TraceScreen(float3 origin, float3 dir, out float2 hitUv)
{
    float t = max(g_stepSize, 1e-3);
    const int kMaxSteps = 40;
    [loop]
    for (int s = 0; s < kMaxSteps; ++s)
    {
        if (t > g_maxTraceDistance)
            return false;

        float3 p = origin + dir * t;

        float2 uv;
        if (!ProjectToUV(p, uv))
            return false;

        if (any(uv < 0.0) || any(uv > 1.0))
            return false;

        // 通过 GBuffer 深度/位置判断是否命中。
        float2 fullUv = ViewportUVToFullUV(uv);
        float4 gb1 = g_gbuffer1.SampleLevel(g_samp, fullUv, 0);
        if (gb1.a > 0.0)
        {
            float3 posS = g_gbuffer2.SampleLevel(g_samp, fullUv, 0).xyz;
            float d = length(posS - p);
            if (d < (g_stepSize * 2.0))
            {
                hitUv = uv;
                return true;
            }
        }

        t += g_stepSize;
    }
    return false;
}

/**
 * @brief 对屏幕空间命中点做一次局部光照估计。
 * @param uv 命中点 UV。
 * @param toViewerDir 指向观察者方向。
 * @return 命中点辐射度。
 * @note 阶段：Lumen 命中着色阶段。
 */
float3 ShadeHit(float2 uv, float3 toViewerDir)
{
    float2 fullUv = ViewportUVToFullUV(uv);
    float4 hb0 = g_gbuffer0.SampleLevel(g_samp, fullUv, 0);
    float4 hb1 = g_gbuffer1.SampleLevel(g_samp, fullUv, 0);
    float4 hb2 = g_gbuffer2.SampleLevel(g_samp, fullUv, 0);

    float valid = hb1.a;
    if (valid <= 0.0)
        return float3(0.0, 0.0, 0.0);

    float3 albedo = hb0.rgb;
    float metallic = saturate(hb0.a);
    float3 N = normalize(hb1.xyz);
    float roughness = saturate(valid - 1.0);
    float ao = saturate(hb2.a);

    float3 V = normalize(toViewerDir);
    float3 L = normalize(-g_lightDirWs);
    float3 radiance = g_lightColor * g_lightIntensity;

    float3 color = BRDF_UEStyle(N, V, L, albedo, metallic, roughness) * radiance;

    // 小环境光项，避免日照弱时过暗。
    return ApplySimpleIBL(color, albedo, metallic, roughness, N, V, ao);
}

/**
 * @brief 从历史缓冲采样上一帧结果。
 * @param uv 采样 UV。
 * @param prevIndex 历史索引（0 或 1）。
 * @return 历史颜色。
 * @note 阶段：时序累积阶段。
 */
float4 SampleHistory(float2 uv, float prevIndex)
{
    if (prevIndex < 0.5)
        return g_history0.SampleLevel(g_samp, uv, 0);
    return g_history1.SampleLevel(g_samp, uv, 0);
}

struct PSLumenOut
{
    float4 AddHDR   : SV_Target0;
    float4 History  : SV_Target1;
};

/**
 * @brief Lumen 像素着色器：GI + 反射 + 时序累积。
 * @param i 插值后的顶点输出。
 * @return HDR 叠加结果与历史输出。
 * @note 阶段：Lumen 像素阶段。
 */
PSLumenOut PSLumen(VSFullOut i)
{
    PSLumenOut o;
    o.AddHDR = float4(0.0, 0.0, 0.0, 0.0);
    o.History = float4(0.0, 0.0, 0.0, 0.0);

    int2 pixel = int2(i.posH.xy);
    float4 gb0 = g_gbuffer0.Load(int3(pixel, 0));
    float4 gb1 = g_gbuffer1.Load(int3(pixel, 0));
    float4 gb2 = g_gbuffer2.Load(int3(pixel, 0));

    float valid = gb1.a;
    if (valid <= 0.0)
        return o;

    float3 albedo = gb0.rgb;
    float metallic = saturate(gb0.a);
    float3 N = normalize(gb1.xyz);
    float roughness = saturate(valid - 1.0);
    float3 posW = gb2.xyz;
    float ao = saturate(gb2.a);

    float3 V = normalize(g_cameraPosWs - posW);

    // RNG 种子：像素 + 帧序号。
    uint2 pix = uint2(i.posH.xy);
    uint seed = Hash(pix.x + (pix.y << 16) + (uint)(g_frameIndex * 1664525.0));

    // 漫反射 GI：两次余弦加权采样。
    float3 indirectDiffuse = float3(0.0, 0.0, 0.0);
    const int kDiffuseRays = 2;
    [unroll]
    for (int r = 0; r < kDiffuseRays; ++r)
    {
        float3 D = SampleHemisphereCos(N, seed);
        float2 hitUv;
        if (TraceScreen(posW + N * 0.02, D, hitUv))
        {
            float3 hitPos = g_gbuffer2.SampleLevel(g_samp, ViewportUVToFullUV(hitUv), 0).xyz;
            float3 toViewer = (posW - hitPos);
            float3 hitRadiance = ShadeHit(hitUv, toViewer);

            float NoD = saturate(dot(N, D));
            float3 brdfDiff = albedo / PI;
            indirectDiffuse += hitRadiance * brdfDiff * NoD;
        }
        else
        {
            // 未命中时使用天空作为回退。
            indirectDiffuse += SampleSkyDiffuse(N) * (albedo / PI);
        }
    }
    indirectDiffuse /= (float)kDiffuseRays;

    // 粗糙反射：沿反射方向采样并按粗糙度扰动。
    float3 reflection = float3(0.0, 0.0, 0.0);
    {
        float3 R = reflect(-V, N);
        float jitter = (roughness * roughness);
        float3 D = normalize(lerp(R, SampleHemisphereCos(N, seed), jitter));
        float2 hitUv;
        if (TraceScreen(posW + N * 0.02, D, hitUv))
        {
            float3 hitPos = g_gbuffer2.SampleLevel(g_samp, ViewportUVToFullUV(hitUv), 0).xyz;
            float3 toViewer = (posW - hitPos);
            reflection = ShadeHit(hitUv, toViewer);
        }
        else
        {
            reflection = SampleSkySpecular(R, roughness);
        }

        // Fresnel 权重。
        float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
        float3 F = FresnelSchlick(saturate(dot(N, V)), F0);
        reflection *= F * (1.0 - roughness);
    }

    float3 cur = (indirectDiffuse + reflection) * (g_intensity * ao);

    // 通过重投影进行时序累积。
    float w = (g_historyValid > 0.5) ? saturate(g_temporalWeight) : 0.0;
    float3 accum = cur;
    {
        float4 clipPrev = mul(float4(posW, 1.0), g_prevViewProj);
        if (clipPrev.w > 1e-4)
        {
            float2 ndcPrev = clipPrev.xy / clipPrev.w;
            float2 uvPrev = float2(ndcPrev.x * 0.5 + 0.5, -ndcPrev.y * 0.5 + 0.5);
            if (all(uvPrev >= 0.0) && all(uvPrev <= 1.0))
            {
                float4 prev = SampleHistory(ViewportUVToFullUV(uvPrev), g_prevHistoryIndex);
                accum = lerp(cur, prev.rgb, w);
            }
        }
    }

    o.AddHDR = float4(accum, 1.0);
    o.History = float4(accum, 1.0);
    return o;
}
