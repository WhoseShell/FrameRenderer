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

/**
 * @brief 顶点着色器：输出光源裁剪空间位置。
 * @param i 顶点输入。
 * @return 顶点输出（光源裁剪空间位置）。
 * @note 阶段：阴影深度渲染顶点阶段。
 */
VSOut VSMain(VSIn i)
{
    VSOut o;
    // 转换到世界空间并再到光源裁剪空间。
    float4 posW = mul(float4(i.pos, 1.0), g_world);
    o.posH = mul(posW, g_lightViewProj);
    return o;
}

/**
 * @brief 像素着色器：输出深度值。
 * @param i 插值后的顶点输出。
 * @return 深度值。
 * @note 阶段：阴影深度渲染像素阶段。
 */
float PSMain(VSOut i) : SV_Depth
{
    // 直接输出裁剪空间 Z 作为深度。
    return i.posH.z;
}
