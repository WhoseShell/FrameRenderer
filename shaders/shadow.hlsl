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
    float3 _pad2;
};

cbuffer ShadowCB : register(b1)
{
    row_major float4x4 g_lightViewProj;
    float2 g_shadowInvSize;
    float g_shadowBias;
    float g_shadowStrength;
};

struct VSIn
{
    float3 pos : POSITION;
    float3 col : COLOR;
    float3 nrm : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct VSOut
{
    float4 posH : SV_Position;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    float4 posW = mul(float4(i.pos, 1.0), g_world);
    o.posH = mul(posW, g_lightViewProj);
    return o;
}

float PSMain(VSOut i) : SV_Depth
{
    return i.posH.z;
}
