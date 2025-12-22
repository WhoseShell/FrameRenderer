#include "sky_atmosphere.hlsl"

cbuffer SkyIblCB : register(b1)
{
    float g_roughness;
    float g_mipLevel;
    float g_maxMip;
    float g_skyIblPad0;
};

TextureCube g_skyCube : register(t0);
SamplerState g_samp : register(s0);

RWTexture2DArray<float4> g_outTex : register(u0);
RWStructuredBuffer<float4> g_outSH : register(u1);

/**
 * @brief 将立方体贴图面 + UV 转换为方向向量。
 * @param face 立方体面索引（0..5）。
 * @param uv 面内坐标（-1..1）。
 * @return 归一化方向向量。
 * @note 阶段：IBL 采样方向计算阶段。
 */
float3 CubeUVToDir(uint face, float2 uv)
{
    if (face == 0) return normalize(float3(1.0, -uv.y, -uv.x));
    if (face == 1) return normalize(float3(-1.0, -uv.y, uv.x));
    if (face == 2) return normalize(float3(uv.x, 1.0, uv.y));
    if (face == 3) return normalize(float3(uv.x, -1.0, -uv.y));
    if (face == 4) return normalize(float3(uv.x, -uv.y, 1.0));
    return normalize(float3(-uv.x, -uv.y, -1.0));
}

/**
 * @brief Van der Corput 反向基 2 序列。
 * @param bits 输入整数。
 * @return [0,1) 区间的序列值。
 * @note 阶段：重要性采样序列生成阶段。
 */
float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
    bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
    return float(bits) * 2.3283064365386963e-10;
}

/**
 * @brief Hammersley 采样序列。
 * @param i 当前样本索引。
 * @param n 总样本数。
 * @return 2D 采样坐标。
 * @note 阶段：重要性采样阶段。
 */
float2 Hammersley(uint i, uint n)
{
    return float2(float(i) / float(n), RadicalInverse_VdC(i));
}

/**
 * @brief GGX 重要性采样方向。
 * @param xi 2D 采样点。
 * @param roughness 粗糙度。
 * @param N 法线方向。
 * @return 采样方向向量。
 * @note 阶段：IBL 预滤波采样阶段。
 */
float3 ImportanceSampleGGX(float2 xi, float roughness, float3 N)
{
    float a = max(roughness * roughness, 1e-4);
    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));

    float3 H = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    float3 up = (abs(N.y) < 0.999) ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 T = normalize(cross(up, N));
    float3 B = cross(N, T);
    float3 sample = normalize(T * H.x + B * H.y + N * H.z);
    return sample;
}

/**
 * @brief 计算二阶球谐基函数。
 * @param dir 单位方向向量。
 * @param sh 输出 9 个 SH 基系数。
 * @note 阶段：天空 IBL 球谐投影阶段。
 */
void SHBasis(float3 dir, out float sh[9])
{
    const float x = dir.x;
    const float y = dir.y;
    const float z = dir.z;

    sh[0] = 0.282095;
    sh[1] = -0.488603 * y;
    sh[2] = 0.488603 * z;
    sh[3] = -0.488603 * x;
    sh[4] = 1.092548 * x * y;
    sh[5] = -1.092548 * y * z;
    sh[6] = 0.315392 * (3.0 * z * z - 1.0);
    sh[7] = -1.092548 * x * z;
    sh[8] = 0.546274 * (x * x - y * y);
}

[numthreads(8, 8, 1)]
/**
 * @brief 生成天空立方体贴图（计算着色器）。
 * @param tid Dispatch 线程 ID。
 * @note 阶段：天空 IBL 立方体生成阶段。
 */
void CSGenerateSkyCube(uint3 tid : SV_DispatchThreadID)
{
    uint w, h, layers;
    g_outTex.GetDimensions(w, h, layers);
    if (tid.x >= w || tid.y >= h || tid.z >= 6)
        return;

    // 计算每个像素对应的方向。
    float2 uv = ((float2(tid.xy) + 0.5) / float2(w, h)) * 2.0 - 1.0;
    float3 dir = CubeUVToDir(tid.z, uv);

    float3 toSun = normalize(-g_sunDirWs);
    float3 sky = ComputeSky(g_cameraPosWs, dir, toSun);

    float sunCos = dot(dir, toSun);
    float sunDisk = smoothstep(cos(0.6 * (PI / 180.0)), cos(0.2 * (PI / 180.0)), sunCos);
    sky += g_sunColor * g_sunIntensity * sunDisk * 0.02;

    // 写入天空立方体贴图。
    g_outTex[uint3(tid.xy, tid.z)] = float4(max(sky, float3(0.0, 0.0, 0.0)), 1.0);
}

[numthreads(1, 1, 1)]
/**
 * @brief 计算天空立方体的球谐系数（计算着色器）。
 * @param tid Dispatch 线程 ID。
 * @note 阶段：天空 IBL 球谐投影阶段。
 */
void CSComputeSkySH(uint3 tid : SV_DispatchThreadID)
{
    if (any(tid.xyz != 0))
        return;

    uint w, h, mips;
    g_skyCube.GetDimensions(0, w, h, mips);

    float3 coeff[9];
    [unroll] for (int i = 0; i < 9; ++i) coeff[i] = float3(0.0, 0.0, 0.0);

    // 对立方体每个面积分。
    const float invW = 1.0 / max(1.0, (float)w);
    const float invH = 1.0 / max(1.0, (float)h);
    const float factor = 4.0 / (float(w) * float(h));

    for (uint face = 0; face < 6; ++face)
    {
        for (uint y = 0; y < h; ++y)
        {
            for (uint x = 0; x < w; ++x)
            {
                float2 uv = (float2(x, y) + 0.5) * float2(invW, invH) * 2.0 - 1.0;
                float3 dir = CubeUVToDir(face, uv);
                float d = 1.0 + uv.x * uv.x + uv.y * uv.y;
                float dOmega = factor / pow(d, 1.5);

                float3 L = g_skyCube.SampleLevel(g_samp, dir, 0).rgb;
                float sh[9];
                SHBasis(dir, sh);

                [unroll]
                for (int i = 0; i < 9; ++i)
                    coeff[i] += L * sh[i] * dOmega;
            }
        }
    }

    const float A0 = PI;
    const float A1 = 2.0 * PI / 3.0;
    const float A2 = PI / 4.0;

    coeff[0] *= A0;
    coeff[1] *= A1;
    coeff[2] *= A1;
    coeff[3] *= A1;
    coeff[4] *= A2;
    coeff[5] *= A2;
    coeff[6] *= A2;
    coeff[7] *= A2;
    coeff[8] *= A2;

    [unroll]
    for (int i = 0; i < 9; ++i)
        g_outSH[i] = float4(coeff[i], (i == 0) ? g_maxMip : 0.0);
}

[numthreads(8, 8, 1)]
/**
 * @brief 预滤波天空立方体贴图（粗糙度卷积）。
 * @param tid Dispatch 线程 ID。
 * @note 阶段：IBL 预滤波阶段。
 */
void CSPrefilterSky(uint3 tid : SV_DispatchThreadID)
{
    uint w, h, layers;
    g_outTex.GetDimensions(w, h, layers);
    if (tid.x >= w || tid.y >= h || tid.z >= 6)
        return;

    float2 uv = ((float2(tid.xy) + 0.5) / float2(w, h)) * 2.0 - 1.0;
    float3 N = CubeUVToDir(tid.z, uv);
    float3 V = N;

    float roughness = saturate(g_roughness);
    uint sampleCount = (uint)lerp(64.0, 16.0, roughness);
    sampleCount = max(1u, sampleCount);

    float3 prefiltered = float3(0.0, 0.0, 0.0);
    float totalWeight = 0.0;

    // 重要性采样积分。
    [loop]
    for (uint i = 0; i < sampleCount; ++i)
    {
        float2 xi = Hammersley(i, sampleCount);
        float3 H = ImportanceSampleGGX(xi, roughness, N);
        float3 L = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = saturate(dot(N, L));
        if (NdotL > 0.0)
        {
            float3 sampleColor = g_skyCube.SampleLevel(g_samp, L, 0).rgb;
            prefiltered += sampleColor * NdotL;
            totalWeight += NdotL;
        }
    }

    prefiltered = (totalWeight > 1e-4) ? (prefiltered / totalWeight) : float3(0.0, 0.0, 0.0);
    g_outTex[uint3(tid.xy, tid.z)] = float4(prefiltered, 1.0);
}
