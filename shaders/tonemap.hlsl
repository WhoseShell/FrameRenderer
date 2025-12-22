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

VSOut VSMain(uint vid : SV_VertexID)
{
    // Fullscreen triangle
    float2 pos = float2((vid == 2) ? 3.0 : -1.0, (vid == 1) ? 3.0 : -1.0);
    VSOut o;
    o.posH = float4(pos, 0.0, 1.0);
    o.uv = float2((pos.x + 1.0) * 0.5, 1.0 - (pos.y + 1.0) * 0.5);
    return o;
}

float3 Reinhard(float3 x)
{
    return x / (x + float3(1.0, 1.0, 1.0));
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 hdr = g_hdrTex.Sample(g_samp, i.uv).rgb;
    hdr = max(hdr, float3(0.0, 0.0, 0.0)) * max(g_exposure, 0.0);

    float3 ldr = hdr;
    if (g_enableTonemap > 0.5)
        ldr = Reinhard(hdr);
    else
        ldr = saturate(hdr); // no tonemap: clamp only

    const float invGamma = 1.0 / max(g_gamma, 1e-4);
    ldr = pow(ldr, invGamma);
    return float4(ldr, 1.0);
}

