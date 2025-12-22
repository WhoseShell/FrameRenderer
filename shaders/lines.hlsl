cbuffer SceneCB : register(b0)
{
    row_major float4x4 g_mvp;
};

struct VSIn
{
    float3 pos : POSITION;
    float3 col : COLOR;
};

struct PSIn
{
    float4 pos : SV_Position;
    float3 col : COLOR;
};

/**
 * @brief 顶点着色器：线段顶点变换并传递颜色。
 * @param i 顶点输入（位置/颜色）。
 * @return 顶点输出（裁剪空间位置/颜色）。
 * @note 阶段：几何管线顶点阶段。
 */
PSIn VSMain(VSIn i)
{
    PSIn o;
    // 计算裁剪空间位置并传递颜色。
    o.pos = mul(float4(i.pos, 1.0), g_mvp);
    o.col = i.col;
    return o;
}

/**
 * @brief 像素着色器：输出线段颜色。
 * @param i 插值后的像素输入。
 * @return 输出颜色（RGBA）。
 * @note 阶段：像素着色阶段。
 */
float4 PSMain(PSIn i) : SV_Target
{
    // 直接输出颜色，Alpha=1。
    return float4(i.col, 1.0);
}

