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

PSIn VSMain(VSIn i)
{
    PSIn o;
    o.pos = mul(float4(i.pos, 1.0), g_mvp);
    o.col = i.col;
    return o;
}

float4 PSMain(PSIn i) : SV_Target
{
    return float4(i.col, 1.0);
}

