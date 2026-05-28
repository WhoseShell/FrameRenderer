cbuffer TonemapCB : register(b0)
{
    float g_enableTonemap; // 1 = tonemap+gamma, 0 = gamma only
    float g_exposure;      // simple exposure multiplier
    float g_gamma;         // display gamma, e.g. 2.2
    float g_tonemapOperator; // 0 = none, 1 = Reinhard, 2 = AgX, 3 = ACES 1.0 RRT+ODT
    float g_targetWidth;
    float g_targetHeight;
    float g_invTargetWidth;
    float g_invTargetHeight;
};

Texture2D g_hdrTex : register(t0);
SamplerState g_samp : register(s0);

struct VSOut
{
    float4 posH : SV_Position;
    float2 uv   : TEXCOORD0;
};

/**
 * @brief 顶点着色器：生成全屏三角形。
 * @param vid 顶点索引（SV_VertexID）。
 * @return 全屏三角形的顶点位置与 UV。
 * @note 阶段：后处理顶点阶段。
 */
VSOut VSMain(uint vid : SV_VertexID)
{
    // Fullscreen triangle
    float2 pos = float2((vid == 2) ? 3.0 : -1.0, (vid == 1) ? 3.0 : -1.0);
    VSOut o;
    // 构建裁剪空间位置与 UV。
    o.posH = float4(pos, 0.0, 1.0);
    o.uv = float2((pos.x + 1.0) * 0.5, 1.0 - (pos.y + 1.0) * 0.5);
    return o;
}

/**
 * @brief Reinhard 色调映射算子。
 * @param x HDR 颜色。
 * @return 映射后的 LDR 颜色。
 * @note 阶段：后处理色调映射阶段。
 */
float3 Reinhard(float3 x)
{
    return x / (x + float3(1.0, 1.0, 1.0));
}

float3 AgXDefaultContrastApprox(float3 x)
{
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5 * x4 * x2
        - 40.14 * x4 * x
        + 31.96 * x4
        - 6.868 * x2 * x
        + 0.4298 * x2
        + 0.1191 * x
        - 0.00232;
}

float3 LinearSRGBToLinearRec2020(float3 v)
{
    return float3(
        dot(v, float3(0.6274, 0.3293, 0.0433)),
        dot(v, float3(0.0691, 0.9195, 0.0113)),
        dot(v, float3(0.0164, 0.0880, 0.8956)));
}

float3 LinearRec2020ToLinearSRGB(float3 v)
{
    return float3(
        dot(v, float3(1.6605, -0.5876, -0.0728)),
        dot(v, float3(-0.1246, 1.1329, -0.0083)),
        dot(v, float3(-0.0182, -0.1006, 1.1187)));
}

float3 AgXInsetTransform(float3 v)
{
    return float3(
        dot(v, float3(0.856627153315983, 0.0951212405381588, 0.0482516061458583)),
        dot(v, float3(0.137318972929847, 0.761241990602591, 0.101439036467562)),
        dot(v, float3(0.11189821299995, 0.0767994186031903, 0.811302368396859)));
}

float3 AgXOutsetTransform(float3 v)
{
    return float3(
        dot(v, float3(1.1271005818144368, -0.11060664309660323, -0.016493938717834573)),
        dot(v, float3(-0.1413297634984383, 1.157823702216272, -0.016493938717834257)),
        dot(v, float3(-0.14132976349843826, -0.11060664309660294, 1.2519364065950405)));
}

float3 AgXTonemap(float3 color)
{
    const float minEv = -12.47393;
    const float maxEv = 4.026069;

    color = LinearSRGBToLinearRec2020(color);
    color = AgXInsetTransform(color);
    color = log2(max(color, float3(1e-10, 1e-10, 1e-10)));
    color = saturate((color - minEv) / (maxEv - minEv));
    color = AgXDefaultContrastApprox(color);
    color = AgXOutsetTransform(color);
    color = pow(max(color, float3(0.0, 0.0, 0.0)), float3(2.2, 2.2, 2.2));
    color = LinearRec2020ToLinearSRGB(color);
    return saturate(color);
}

float Log10(float x)
{
    return log2(max(x, 1e-30)) * 0.3010299956639812;
}

float Pow10(float x)
{
    return exp2(x * 3.321928094887362);
}

float3 LinearSRGBToACESAP0(float3 v)
{
    return float3(
        dot(v, float3(0.4396329819, 0.3829886982, 0.1773783199)),
        dot(v, float3(0.0897764430, 0.8134394287, 0.0967841283)),
        dot(v, float3(0.0175411704, 0.1115465533, 0.8709122763)));
}

float3 ACESAP0ToAP1(float3 v)
{
    return float3(
        dot(v, float3(1.4514393161, -0.2365107469, -0.2149285693)),
        dot(v, float3(-0.0765537734, 1.1762296998, -0.0996759264)),
        dot(v, float3(0.0083161484, -0.0060324498, 0.9977163014)));
}

float3 ACESAP1ToAP0(float3 v)
{
    return float3(
        dot(v, float3(0.6954522414, 0.1406786965, 0.1638690622)),
        dot(v, float3(0.0447945634, 0.8596711185, 0.0955343182)),
        dot(v, float3(-0.0055258826, 0.0040252103, 1.0015006723)));
}

float3 ACESAP1ToXYZ(float3 v)
{
    return float3(
        dot(v, float3(0.6624541811, 0.1340042065, 0.1561876870)),
        dot(v, float3(0.2722287168, 0.6740817658, 0.0536895174)),
        dot(v, float3(-0.0055746495, 0.0040607335, 1.0103391003)));
}

float3 ACESXYZToAP1(float3 v)
{
    return float3(
        dot(v, float3(1.6410233797, -0.3248032942, -0.2364246952)),
        dot(v, float3(-0.6636628587, 1.6153315917, 0.0167563477)),
        dot(v, float3(0.0117218943, -0.0082844420, 0.9883948585)));
}

float3 ACESD60ToD65(float3 v)
{
    return float3(
        dot(v, float3(0.9872240087, -0.0061132286, 0.0159532883)),
        dot(v, float3(-0.0075983718, 1.0018614847, 0.0053300358)),
        dot(v, float3(0.0030725771, -0.0050959615, 1.0816806031)));
}

float3 ACESXYZToRec709(float3 v)
{
    return float3(
        dot(v, float3(3.2409699419, -1.5373831776, -0.4986107603)),
        dot(v, float3(-0.9692436363, 1.8759675015, 0.0415550574)),
        dot(v, float3(0.0556300797, -0.2039769589, 1.0569715142)));
}

float3 ACESRRTSaturation(float3 v)
{
    return float3(
        dot(v, float3(0.9708891487, 0.0269632706, 0.0021475807)),
        dot(v, float3(0.0108891487, 0.9869632706, 0.0021475807)),
        dot(v, float3(0.0108891487, 0.0269632706, 0.9621475807)));
}

float3 ACESODTSaturation(float3 v)
{
    return float3(
        dot(v, float3(0.9490560102, 0.0471857236, 0.0037582662)),
        dot(v, float3(0.0190560102, 0.9771857236, 0.0037582662)),
        dot(v, float3(0.0190560102, 0.0471857236, 0.9337582662)));
}

float ACESRgbSaturation(float3 rgb)
{
    const float tiny = 1e-10;
    const float mx = max(max(rgb.r, rgb.g), rgb.b);
    const float mn = min(min(rgb.r, rgb.g), rgb.b);
    return (max(mx, tiny) - max(mn, tiny)) / max(mx, 1e-2);
}

float ACESRgbYC(float3 rgb)
{
    const float chroma = sqrt(max(rgb.b * (rgb.b - rgb.g) + rgb.g * (rgb.g - rgb.r) + rgb.r * (rgb.r - rgb.b), 0.0));
    return (rgb.r + rgb.g + rgb.b + 1.75 * chroma) / 3.0;
}

float ACESRgbHue(float3 rgb)
{
    if (abs(rgb.r - rgb.g) < 1e-8 && abs(rgb.g - rgb.b) < 1e-8)
        return 0.0;

    float hue = degrees(atan2(sqrt(3.0) * (rgb.g - rgb.b), 2.0 * rgb.r - rgb.g - rgb.b));
    return (hue < 0.0) ? (hue + 360.0) : hue;
}

float ACESSigmoidShaper(float x)
{
    float t = max(1.0 - abs(x * 0.5), 0.0);
    return (1.0 + sign(x) * (1.0 - t * t)) * 0.5;
}

float ACESGlow(float ycIn, float glowGainIn, float glowMid)
{
    if (ycIn <= (2.0 / 3.0) * glowMid)
        return glowGainIn;
    if (ycIn >= 2.0 * glowMid)
        return 0.0;
    return glowGainIn * (glowMid / ycIn - 0.5);
}

float ACESCenterHue(float hue, float centerH)
{
    float hueCentered = hue - centerH;
    if (hueCentered < -180.0)
        hueCentered += 360.0;
    else if (hueCentered > 180.0)
        hueCentered -= 360.0;
    return hueCentered;
}

float ACESCubicBasisShaper(float x, float width)
{
    float y = 0.0;
    if (x > -0.5 * width && x < 0.5 * width)
    {
        const float knotCoord = (x + 0.5 * width) * 4.0 / width;
        const int j = min(max((int)floor(knotCoord), 0), 3);
        const float t = knotCoord - (float)j;
        const float t2 = t * t;
        const float t3 = t2 * t;

        if (j == 0)
            y = t3 * (1.0 / 6.0);
        else if (j == 1)
            y = t3 * (-3.0 / 6.0) + t2 * (3.0 / 6.0) + t * (3.0 / 6.0) + (1.0 / 6.0);
        else if (j == 2)
            y = t3 * (3.0 / 6.0) + t2 * (-6.0 / 6.0) + (4.0 / 6.0);
        else
            y = t3 * (-1.0 / 6.0) + t2 * (3.0 / 6.0) + t * (-3.0 / 6.0) + (1.0 / 6.0);
    }
    return y * 1.5;
}

float ACESBSpline(float c0, float c1, float c2, float t)
{
    return (0.5 * c0 - c1 + 0.5 * c2) * t * t + (-c0 + c1) * t + 0.5 * (c0 + c1);
}

void ACESGetC5Low(int j, out float c0, out float c1, out float c2)
{
    if (j <= 0) { c0 = -4.0000000000; c1 = -4.0000000000; c2 = -3.1573765773; }
    else if (j == 1) { c0 = -4.0000000000; c1 = -3.1573765773; c2 = -0.4852499958; }
    else { c0 = -3.1573765773; c1 = -0.4852499958; c2 = 1.8477324706; }
}

void ACESGetC5High(int j, out float c0, out float c1, out float c2)
{
    if (j <= 0) { c0 = -0.7185482425; c1 = 2.0810307172; c2 = 3.6681241237; }
    else if (j == 1) { c0 = 2.0810307172; c1 = 3.6681241237; c2 = 4.0000000000; }
    else { c0 = 3.6681241237; c1 = 4.0000000000; c2 = 4.0000000000; }
}

float ACESSegmentedSplineC5(float x)
{
    const float minX = 0.18 * 0.000030517578125;
    const float minY = 0.0001;
    const float midX = 0.18;
    const float maxX = 0.18 * 262144.0;
    const float maxY = 10000.0;

    const float logx = Log10(max(x, 1e-30));
    float logy = 0.0;
    if (logx <= Log10(minX))
    {
        logy = Log10(minY);
    }
    else if (logx < Log10(midX))
    {
        const float knotCoord = 3.0 * (logx - Log10(minX)) / (Log10(midX) - Log10(minX));
        const int j = min(max((int)floor(knotCoord), 0), 2);
        float c0, c1, c2;
        ACESGetC5Low(j, c0, c1, c2);
        logy = ACESBSpline(c0, c1, c2, knotCoord - (float)j);
    }
    else if (logx < Log10(maxX))
    {
        const float knotCoord = 3.0 * (logx - Log10(midX)) / (Log10(maxX) - Log10(midX));
        const int j = min(max((int)floor(knotCoord), 0), 2);
        float c0, c1, c2;
        ACESGetC5High(j, c0, c1, c2);
        logy = ACESBSpline(c0, c1, c2, knotCoord - (float)j);
    }
    else
    {
        logy = Log10(maxY);
    }
    return Pow10(logy);
}

void ACESGetC9Low(int j, out float c0, out float c1, out float c2)
{
    if (j <= 0) { c0 = -1.6989700043; c1 = -1.6989700043; c2 = -1.4779000000; }
    else if (j == 1) { c0 = -1.6989700043; c1 = -1.4779000000; c2 = -1.2291000000; }
    else if (j == 2) { c0 = -1.4779000000; c1 = -1.2291000000; c2 = -0.8648000000; }
    else if (j == 3) { c0 = -1.2291000000; c1 = -0.8648000000; c2 = -0.4480000000; }
    else if (j == 4) { c0 = -0.8648000000; c1 = -0.4480000000; c2 = 0.0051800000; }
    else if (j == 5) { c0 = -0.4480000000; c1 = 0.0051800000; c2 = 0.4511080334; }
    else { c0 = 0.0051800000; c1 = 0.4511080334; c2 = 0.9113744414; }
}

void ACESGetC9High(int j, out float c0, out float c1, out float c2)
{
    if (j <= 0) { c0 = 0.5154386965; c1 = 0.8470437783; c2 = 1.1358000000; }
    else if (j == 1) { c0 = 0.8470437783; c1 = 1.1358000000; c2 = 1.3802000000; }
    else if (j == 2) { c0 = 1.1358000000; c1 = 1.3802000000; c2 = 1.5197000000; }
    else if (j == 3) { c0 = 1.3802000000; c1 = 1.5197000000; c2 = 1.5985000000; }
    else if (j == 4) { c0 = 1.5197000000; c1 = 1.5985000000; c2 = 1.6467000000; }
    else if (j == 5) { c0 = 1.5985000000; c1 = 1.6467000000; c2 = 1.6746091357; }
    else { c0 = 1.6467000000; c1 = 1.6746091357; c2 = 1.6878733390; }
}

float ACESSegmentedSplineC9(float x)
{
    const float minX = 0.0028798932;
    const float minY = 0.02;
    const float midX = 4.8;
    const float maxX = 1005.7193595080;
    const float maxY = 48.0;
    const float slopeHigh = 0.04;

    const float logx = Log10(max(x, 1e-30));
    float logy = 0.0;
    if (logx <= Log10(minX))
    {
        logy = Log10(minY);
    }
    else if (logx < Log10(midX))
    {
        const float knotCoord = 7.0 * (logx - Log10(minX)) / (Log10(midX) - Log10(minX));
        const int j = min(max((int)floor(knotCoord), 0), 6);
        float c0, c1, c2;
        ACESGetC9Low(j, c0, c1, c2);
        logy = ACESBSpline(c0, c1, c2, knotCoord - (float)j);
    }
    else if (logx < Log10(maxX))
    {
        const float knotCoord = 7.0 * (logx - Log10(midX)) / (Log10(maxX) - Log10(midX));
        const int j = min(max((int)floor(knotCoord), 0), 6);
        float c0, c1, c2;
        ACESGetC9High(j, c0, c1, c2);
        logy = ACESBSpline(c0, c1, c2, knotCoord - (float)j);
    }
    else
    {
        logy = logx * slopeHigh + (Log10(maxY) - slopeHigh * Log10(maxX));
    }
    return Pow10(logy);
}

float3 ACESXYYToXYZ(float3 xyY)
{
    const float y = max(xyY.y, 1e-10);
    return float3(xyY.x * xyY.z / y, xyY.z, (1.0 - xyY.x - xyY.y) * xyY.z / y);
}

float3 ACESXYZToXYY(float3 xyz)
{
    const float d = max(xyz.x + xyz.y + xyz.z, 1e-10);
    return float3(xyz.x / d, xyz.y / d, xyz.y);
}

float3 ACESDarkSurroundToDimSurround(float3 linearCV)
{
    float3 xyz = ACESAP1ToXYZ(linearCV);
    float3 xyY = ACESXYZToXYY(xyz);
    xyY.z = pow(max(xyY.z, 0.0), 0.9811);
    return ACESXYZToAP1(ACESXYYToXYZ(xyY));
}

float3 ACESRRT(float3 color)
{
    float3 aces = LinearSRGBToACESAP0(max(color, float3(0.0, 0.0, 0.0)));

    const float saturation = ACESRgbSaturation(aces);
    const float ycIn = ACESRgbYC(aces);
    const float glowWeight = ACESSigmoidShaper((saturation - 0.4) / 0.2);
    aces *= 1.0 + ACESGlow(ycIn, 0.05 * glowWeight, 0.08);

    const float hue = ACESRgbHue(aces);
    const float hueWeight = ACESCubicBasisShaper(ACESCenterHue(hue, 0.0), 135.0);
    aces.r += hueWeight * saturation * (0.03 - aces.r) * (1.0 - 0.82);

    float3 rgbPre = ACESAP0ToAP1(max(aces, float3(0.0, 0.0, 0.0)));
    rgbPre = ACESRRTSaturation(max(rgbPre, float3(0.0, 0.0, 0.0)));

    float3 rgbPost = float3(
        ACESSegmentedSplineC5(rgbPre.r),
        ACESSegmentedSplineC5(rgbPre.g),
        ACESSegmentedSplineC5(rgbPre.b));

    return ACESAP1ToAP0(rgbPost);
}

float3 ACESODTRec709(float3 oces)
{
    float3 rgbPre = ACESAP0ToAP1(oces);
    float3 rgbPost = float3(
        ACESSegmentedSplineC9(rgbPre.r),
        ACESSegmentedSplineC9(rgbPre.g),
        ACESSegmentedSplineC9(rgbPre.b));

    float3 linearCV = (rgbPost - 0.02) / (48.0 - 0.02);
    linearCV = ACESDarkSurroundToDimSurround(linearCV);
    linearCV = ACESODTSaturation(linearCV);

    float3 xyz = ACESAP1ToXYZ(linearCV);
    xyz = ACESD60ToD65(xyz);
    linearCV = saturate(ACESXYZToRec709(xyz));

    return pow(linearCV, float3(1.0 / 2.4, 1.0 / 2.4, 1.0 / 2.4));
}

float3 ACESOfficialTonemap(float3 color)
{
    return saturate(ACESODTRec709(ACESRRT(color)));
}

/**
 * @brief 像素着色器：对 HDR 颜色进行 Tonemap 与 Gamma。
 * @param i 插值后的顶点输出。
 * @return 输出 LDR 颜色。
 * @note 阶段：后处理像素阶段。
 */
float4 PSMain(VSOut i) : SV_Target
{
    // 采样 HDR 颜色并应用曝光。
    float2 sourceUv = saturate(i.posH.xy * float2(g_invTargetWidth, g_invTargetHeight));
    float3 hdr = g_hdrTex.Sample(g_samp, sourceUv).rgb;
    hdr = max(hdr, float3(0.0, 0.0, 0.0)) * max(g_exposure, 0.0);

    float3 ldr = hdr;
    if (g_enableTonemap > 0.5)
    {
        // Tonemap 分支。
        if (g_tonemapOperator > 2.5)
            ldr = ACESOfficialTonemap(hdr);
        else
            ldr = (g_tonemapOperator > 1.5) ? AgXTonemap(hdr) : Reinhard(hdr);
    }
    else
        ldr = saturate(hdr); // no tonemap: clamp only

    // Gamma 校正。
    const float invGamma = 1.0 / max(g_gamma, 1e-4);
    if (!(g_enableTonemap > 0.5 && g_tonemapOperator > 2.5))
        ldr = pow(ldr, invGamma);
    return float4(ldr, 1.0);
}

