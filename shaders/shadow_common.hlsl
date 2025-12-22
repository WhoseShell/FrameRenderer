#ifndef SHADOW_COMMON_HLSL
#define SHADOW_COMMON_HLSL

float ComputeShadowFactor(float3 posW)
{
    float4 lightClip = mul(float4(posW, 1.0), g_lightViewProj);
    float3 ndc = lightClip.xyz / lightClip.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
    float depth = ndc.z - g_shadowBias;

    if (any(uv < 0.0) || any(uv > 1.0) || depth <= 0.0 || depth >= 1.0)
        return 1.0;

    float shadow = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * g_shadowInvSize;
            shadow += g_shadowMap.SampleCmpLevelZero(g_shadowSamp, uv + offset, depth);
        }
    }
    shadow *= (1.0 / 9.0);
    return lerp(1.0, shadow, g_shadowStrength);
}

#endif
