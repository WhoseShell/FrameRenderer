#include "Engine/Engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <random>

#include <wincodec.h>
#include <shellapi.h>
#include <commdlg.h>
#include <commctrl.h>

#include "Core/Diagnostics.h"

/**
 * @brief 将浮点数限制在指定区间内。
 * @param v 输入值。
 * @param mn 最小值。
 * @param mx 最大值。
 * @return 夹取后的结果。
 * @note 阶段：通用数学辅助（参数约束）。
 */
static float Clamp(float v, float mn, float mx)
{
    // 简单分支实现，避免额外开销。
    return (v < mn) ? mn : (v > mx) ? mx : v;
}

/**
 * @brief 将整数限制在指定区间内。
 * @param v 输入值。
 * @param mn 最小值。
 * @param mx 最大值。
 * @return 夹取后的结果。
 * @note 阶段：通用数学辅助（参数约束）。
 */
static int ClampI(int v, int mn, int mx)
{
    // 简单分支实现，避免额外开销。
    return (v < mn) ? mn : (v > mx) ? mx : v;
}

/**
 * @brief 计算场景物体拾取时使用的包围半径。
 * @param obj 场景物体（包含缩放与半径）。
 * @return 拾取用的半径。
 * @note 阶段：编辑器选取/交互阶段。
 */
static float GetObjectPickRadius(const FSceneObject& obj)
{
    // 使用最大缩放分量影响拾取半径，避免过小导致难选中。
    const float s = std::max(obj.Scale.x, std::max(obj.Scale.y, obj.Scale.z));
    return obj.Radius * std::max(s, 0.01f);
}

/**
 * @brief 判断文件路径是否为支持的图像扩展名。
 * @param path 文件路径。
 * @return true 表示为支持的图像格式。
 * @note 阶段：资源导入/拖拽阶段。
 */
static bool HasImageExtension(const std::wstring& path)
{
    // 转小写后比较扩展名。
    auto ext = std::filesystem::path(path).extension().wstring();
    for (auto& c : ext) c = (wchar_t)towlower(c);
    return (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".tga");
}

/**
 * @brief 将颜色向量夹取到 0..1 范围。
 * @param c 输入颜色（RGB）。
 * @return 夹取后的颜色。
 * @note 阶段：材质/纹理参数规范化阶段。
 */
static DirectX::XMFLOAT3 Clamp01(const DirectX::XMFLOAT3& c)
{
    return { Clamp(c.x, 0.0f, 1.0f), Clamp(c.y, 0.0f, 1.0f), Clamp(c.z, 0.0f, 1.0f) };
}

/**
 * @brief 计算 RGBA8 像素数组的平均颜色。
 * @param rgba RGBA8 像素数组（按字节）。
 * @param w 宽度（像素）。
 * @param h 高度（像素）。
 * @return 平均颜色（0..1）。
 * @note 阶段：纹理预览/统计阶段。
 */
static DirectX::XMFLOAT3 ComputeAverageRGBA8(const std::vector<uint8>& rgba, uint32 w, uint32 h)
{
    // 防止空数据导致除零。
    if (rgba.empty() || w == 0 || h == 0) return { 1.0f, 1.0f, 1.0f };
    const size_t pixels = (size_t)w * (size_t)h;
    uint64 sr = 0, sg = 0, sb = 0;
    for (size_t i = 0; i < pixels; ++i)
    {
        // 逐像素累加 RGB 通道。
        sr += rgba[i * 4 + 0];
        sg += rgba[i * 4 + 1];
        sb += rgba[i * 4 + 2];
    }
    // 转换为 0..1 的平均颜色。
    const float inv = 1.0f / float(pixels) / 255.0f;
    return Clamp01({ float(sr) * inv, float(sg) * inv, float(sb) * inv });
}

/**
 * @brief 使用 WIC 加载缩略图并创建 HBITMAP 预览。
 * @param path 图片文件路径。
 * @param maxSize 缩略图最大边长（像素）。
 * @param outBmp 输出的位图句柄。
 * @param outAvgColor 输出的平均颜色（0..1）。
 * @return true 表示加载成功。
 * @note 阶段：编辑器纹理预览与导入阶段。
 */
static bool LoadImagePreviewWIC(const std::wstring& path, uint32 maxSize, HBITMAP& outBmp, DirectX::XMFLOAT3& outAvgColor)
{
    // 初始化输出，避免部分失败时返回脏数据。
    outBmp = nullptr;
    outAvgColor = { 1.0f, 1.0f, 1.0f };

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;

    // 读取首帧图像。
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)))
        return false;

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)))
        return false;

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0) return false;

    // 根据最大尺寸计算缩放目标尺寸。
    UINT tw = w, th = h;
    if (w > maxSize || h > maxSize)
    {
        const float sx = float(maxSize) / float(w);
        const float sy = float(maxSize) / float(h);
        const float s = (sx < sy) ? sx : sy;
        tw = (UINT)std::max(1.0f, std::floorf(float(w) * s));
        th = (UINT)std::max(1.0f, std::floorf(float(h) * s));
    }

    Microsoft::WRL::ComPtr<IWICBitmapSource> source = frame;
    Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
    if (tw != w || th != h)
    {
        // 需要缩放时创建 scaler。
        if (FAILED(factory->CreateBitmapScaler(&scaler))) return false;
        if (FAILED(scaler->Initialize(frame.Get(), tw, th, WICBitmapInterpolationModeFant))) return false;
        source = scaler;
    }

    // 转换为 RGBA8 便于 CPU 处理。
    Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(source.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;

    std::vector<uint8> rgba;
    rgba.resize((size_t)tw * (size_t)th * 4);
    const UINT stride = tw * 4;
    if (FAILED(conv->CopyPixels(nullptr, stride, (UINT)rgba.size(), rgba.data())))
        return false;

    // 计算平均颜色。
    outAvgColor = ComputeAverageRGBA8(rgba, tw, th);

    // 创建顶对齐位图用于 UI 预览显示。
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = (LONG)tw;
    bmi.bmiHeader.biHeight = -(LONG)th; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!bmp || !bits) return false;
    // 拷贝像素数据到位图。
    std::memcpy(bits, rgba.data(), rgba.size());
    outBmp = bmp;
    return true;
}

/**
 * @brief 使用 WIC 加载 RGBA8 像素数据（可选缩放）。
 * @param path 图片文件路径。
 * @param maxSize 最大边长（像素），为 0 表示不缩放。
 * @param outRGBA 输出的 RGBA8 像素数组。
 * @param outW 输出宽度。
 * @param outH 输出高度。
 * @return true 表示加载成功。
 * @note 阶段：纹理资源导入阶段。
 */
static bool LoadImageRGBA8WIC(const std::wstring& path, uint32 maxSize, std::vector<uint8>& outRGBA, uint32& outW, uint32& outH)
{
    // 清空输出以避免失败时保留旧数据。
    outRGBA.clear();
    outW = outH = 0;

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)))
        return false;

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)))
        return false;

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0) return false;

    // 根据最大尺寸决定是否缩放。
    UINT tw = w, th = h;
    if (maxSize > 0 && (w > maxSize || h > maxSize))
    {
        const float sx = float(maxSize) / float(w);
        const float sy = float(maxSize) / float(h);
        const float s = (sx < sy) ? sx : sy;
        tw = (UINT)std::max(1.0f, std::floorf(float(w) * s));
        th = (UINT)std::max(1.0f, std::floorf(float(h) * s));
    }

    Microsoft::WRL::ComPtr<IWICBitmapSource> source = frame;
    Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
    if (tw != w || th != h)
    {
        // 创建缩放器并缩放到目标尺寸。
        if (FAILED(factory->CreateBitmapScaler(&scaler))) return false;
        if (FAILED(scaler->Initialize(frame.Get(), tw, th, WICBitmapInterpolationModeFant))) return false;
        source = scaler;
    }

    // 转换为 RGBA8 格式。
    Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(source.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;

    outRGBA.resize((size_t)tw * (size_t)th * 4);
    const UINT stride = tw * 4;
    if (FAILED(conv->CopyPixels(nullptr, stride, (UINT)outRGBA.size(), outRGBA.data())))
        return false;

    // 返回尺寸信息。
    outW = (uint32)tw;
    outH = (uint32)th;
    return true;
}

/**
 * @brief 加载 TGA 文件为 RGBA8 像素数据。
 * @param path TGA 文件路径。
 * @param outRGBA 输出的 RGBA8 像素数组。
 * @param outW 输出宽度。
 * @param outH 输出高度。
 * @return true 表示加载成功。
 * @note 阶段：纹理资源导入阶段。
 */
static bool LoadImageRGBA8TGA(const std::wstring& path, std::vector<uint8>& outRGBA, uint32& outW, uint32& outH)
{
    // 初始化输出，失败时保持为空。
    outRGBA.clear();
    outW = outH = 0;

    // 打开 TGA 文件（仅支持 24/32 位 TrueColor）。
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (!f) return false;

    struct TGAHeader
    {
        uint8 idLength;
        uint8 colorMapType;
        uint8 imageType;
        uint16 colorMapFirstEntryIndex;
        uint16 colorMapLength;
        uint8 colorMapEntrySize;
        uint16 xOrigin;
        uint16 yOrigin;
        uint16 width;
        uint16 height;
        uint8 pixelDepth;
        uint8 imageDescriptor;
    } h{};

    if (fread(&h, sizeof(h), 1, f) != 1) { fclose(f); return false; }
    if (h.colorMapType != 0) { fclose(f); return false; }
    if (!(h.imageType == 2 || h.imageType == 10)) { fclose(f); return false; } // truecolor or RLE truecolor
    if (!(h.pixelDepth == 24 || h.pixelDepth == 32)) { fclose(f); return false; }
    if (h.width == 0 || h.height == 0) { fclose(f); return false; }

    // 跳过 ID 字段。
    if (h.idLength) fseek(f, h.idLength, SEEK_CUR);

    const uint32 w = h.width;
    const uint32 hgt = h.height;
    const uint32 bpp = h.pixelDepth / 8;
    const bool topOrigin = (h.imageDescriptor & 0x20) != 0;

    // 预分配像素数组。
    outRGBA.resize((size_t)w * (size_t)hgt * 4);

    auto writePixel = [&](uint32 x, uint32 y, const uint8* srcBGR)
    {
        // 根据原点方向写入 RGBA。
        const uint32 yy = topOrigin ? y : (hgt - 1 - y);
        uint8* dst = &outRGBA[(size_t(yy) * w + x) * 4];
        dst[0] = srcBGR[2];
        dst[1] = srcBGR[1];
        dst[2] = srcBGR[0];
        dst[3] = (bpp == 4) ? srcBGR[3] : 255;
    };

    if (h.imageType == 2)
    {
        // 非压缩 TrueColor。
        std::vector<uint8> pix(bpp);
        for (uint32 y = 0; y < hgt; ++y)
        {
            for (uint32 x = 0; x < w; ++x)
            {
                if (fread(pix.data(), bpp, 1, f) != 1) { fclose(f); return false; }
                writePixel(x, y, pix.data());
            }
        }
    }
    else
    {
        // RLE 压缩 TrueColor。
        uint32 x = 0, y = 0;
        std::vector<uint8> pix(bpp);
        while (y < hgt)
        {
            uint8 header = 0;
            if (fread(&header, 1, 1, f) != 1) { fclose(f); return false; }
            const uint32 count = (header & 0x7F) + 1;
            if (header & 0x80)
            {
                if (fread(pix.data(), bpp, 1, f) != 1) { fclose(f); return false; }
                for (uint32 i = 0; i < count; ++i)
                {
                    writePixel(x, y, pix.data());
                    if (++x >= w) { x = 0; ++y; if (y >= hgt) break; }
                }
            }
            else
            {
                for (uint32 i = 0; i < count; ++i)
                {
                    if (fread(pix.data(), bpp, 1, f) != 1) { fclose(f); return false; }
                    writePixel(x, y, pix.data());
                    if (++x >= w) { x = 0; ++y; if (y >= hgt) break; }
                }
            }
        }
    }

    fclose(f);
    outW = w;
    outH = hgt;
    return true;
}

/**
 * @brief 由屏幕坐标计算世界空间射线方向，并输出射线起点。
 * @param mouseX 屏幕 X 坐标（像素）。
 * @param mouseY 屏幕 Y 坐标（像素）。
 * @param width 视口宽度（像素）。
 * @param height 视口高度（像素）。
 * @param invViewProj 视图投影矩阵的逆矩阵。
 * @param outOrigin 输出射线起点（近裁剪面上的点）。
 * @return 归一化的射线方向。
 * @note 阶段：编辑器拾取/交互阶段。
 */
static DirectX::XMVECTOR MakeRayDirFromScreen(
    int mouseX,
    int mouseY,
    uint32 width,
    uint32 height,
    const DirectX::XMMATRIX& invViewProj,
    DirectX::XMVECTOR& outOrigin)
{
    using namespace DirectX;

    // 将屏幕坐标映射到 NDC。
    const float ndcX = (2.0f * float(mouseX) / float(width)) - 1.0f;
    const float ndcY = 1.0f - (2.0f * float(mouseY) / float(height));

    // 生成近远裁剪面上的点并反投影到世界空间。
    XMVECTOR nearClip = XMVectorSet(ndcX, ndcY, 0.0f, 1.0f);
    XMVECTOR farClip = XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);

    XMVECTOR nearWorld = XMVector4Transform(nearClip, invViewProj);
    XMVECTOR farWorld = XMVector4Transform(farClip, invViewProj);
    nearWorld = XMVectorScale(nearWorld, 1.0f / XMVectorGetW(nearWorld));
    farWorld = XMVectorScale(farWorld, 1.0f / XMVectorGetW(farWorld));

    // 输出射线起点，并计算归一化方向。
    outOrigin = nearWorld;
    return XMVector3Normalize(XMVectorSubtract(farWorld, nearWorld));
}

/**
 * @brief 计算射线与球体的相交情况。
 * @param rayOrigin 射线起点。
 * @param rayDir 射线方向（已归一化）。
 * @param sphereCenter 球心位置。
 * @param sphereRadius 球体半径。
 * @param outT 输出交点距离参数 t。
 * @return true 表示命中球体。
 * @note 阶段：编辑器拾取/碰撞检测阶段。
 */
static bool RaySphereIntersect(
    const DirectX::XMVECTOR& rayOrigin,
    const DirectX::XMVECTOR& rayDir,
    const DirectX::XMVECTOR& sphereCenter,
    float sphereRadius,
    float& outT)
{
    using namespace DirectX;

    // 解方程 ||(o + d t) - c||^2 = r^2。
    // Solve ||(o + d t) - c||^2 = r^2
    const XMVECTOR oc = XMVectorSubtract(rayOrigin, sphereCenter);
    const float b = XMVectorGetX(XMVector3Dot(oc, rayDir));
    const float c = XMVectorGetX(XMVector3Dot(oc, oc)) - sphereRadius * sphereRadius;
    const float disc = b * b - c;
    if (disc < 0.0f) return false;

    const float s = std::sqrtf(disc);
    const float t0 = -b - s;
    const float t1 = -b + s;
    const float t = (t0 >= 0.0f) ? t0 : t1;
    if (t < 0.0f) return false;
    outT = t;
    return true;
}

/**
 * @brief 计算射线与平面的相交情况。
 * @param rayOrigin 射线起点。
 * @param rayDir 射线方向（已归一化）。
 * @param planePoint 平面上一点。
 * @param planeNormalUnit 平面法线（已归一化）。
 * @param outT 输出交点距离参数 t。
 * @return true 表示命中平面。
 * @note 阶段：编辑器拾取/操控轴计算阶段。
 */
static bool RayPlaneIntersect(
    const DirectX::XMVECTOR& rayOrigin,
    const DirectX::XMVECTOR& rayDir,
    const DirectX::XMVECTOR& planePoint,
    const DirectX::XMVECTOR& planeNormalUnit,
    float& outT)
{
    const float denom = DirectX::XMVectorGetX(DirectX::XMVector3Dot(planeNormalUnit, rayDir));
    if (std::fabsf(denom) < 1e-6f) return false;

    const DirectX::XMVECTOR diff = DirectX::XMVectorSubtract(planePoint, rayOrigin);
    const float t = DirectX::XMVectorGetX(DirectX::XMVector3Dot(diff, planeNormalUnit)) / denom;
    if (t < 0.0f) return false;
    outT = t;
    return true;
}

/**
 * @brief 计算点到二维线段的最短距离。
 * @param px 点 X 坐标。
 * @param py 点 Y 坐标。
 * @param ax 线段起点 X。
 * @param ay 线段起点 Y。
 * @param bx 线段终点 X。
 * @param by 线段终点 Y。
 * @return 点到线段的最短距离。
 * @note 阶段：编辑器 Gizmo 命中测试阶段。
 */
static float DistancePointToSegment2D(float px, float py, float ax, float ay, float bx, float by)
{
    // 计算投影参数并限制在 0..1 区间。
    const float abx = bx - ax;
    const float aby = by - ay;
    const float apx = px - ax;
    const float apy = py - ay;

    const float abLen2 = abx * abx + aby * aby;
    float t = 0.0f;
    if (abLen2 > 1e-6f) t = (apx * abx + apy * aby) / abLen2;
    t = Clamp(t, 0.0f, 1.0f);

    const float cx = ax + abx * t;
    const float cy = ay + aby * t;
    const float dx = px - cx;
    const float dy = py - cy;
    return std::sqrtf(dx * dx + dy * dy);
}

/**
 * @brief 将世界坐标投影到屏幕坐标。
 * @param pWorld 世界坐标点（向量）。
 * @param viewProj 视图投影矩阵。
 * @param width 视口宽度（像素）。
 * @param height 视口高度（像素）。
 * @param outX 输出屏幕 X 坐标。
 * @param outY 输出屏幕 Y 坐标。
 * @return true 表示投影有效（w 不为 0）。
 * @note 阶段：渲染调试/编辑器 Gizmo 绘制阶段。
 */
static bool ProjectToScreen(
    const DirectX::XMVECTOR& pWorld,
    const DirectX::XMMATRIX& viewProj,
    uint32 width,
    uint32 height,
    float& outX,
    float& outY)
{
    using namespace DirectX;

    // 进行齐次坐标变换。
    XMVECTOR p = XMVector4Transform(XMVectorSetW(pWorld, 1.0f), viewProj);
    const float w = XMVectorGetW(p);
    if (std::fabsf(w) < 1e-6f) return false;
    const float invW = 1.0f / w;
    const float ndcX = XMVectorGetX(p) * invW;
    const float ndcY = XMVectorGetY(p) * invW;

    outX = (ndcX * 0.5f + 0.5f) * float(width);
    outY = (0.5f - ndcY * 0.5f) * float(height);
    return true;
}

/**
 * @brief 计算轴线与射线之间最近点在轴线上的参数值。
 * @param axisOrigin 轴线起点。
 * @param axisDirUnit 轴线方向（单位向量）。
 * @param rayOrigin 射线起点。
 * @param rayDirUnit 射线方向（单位向量）。
 * @param outAxisS 输出轴线上参数 s（沿轴线方向的距离）。
 * @return true 表示计算成功（未平行）。
 * @note 阶段：编辑器 Gizmo 拖拽阶段。
 */
static bool ClosestAxisParamToRay(
    const DirectX::XMVECTOR& axisOrigin,
    const DirectX::XMVECTOR& axisDirUnit,
    const DirectX::XMVECTOR& rayOrigin,
    const DirectX::XMVECTOR& rayDirUnit,
    float& outAxisS)
{
    using namespace DirectX;

    // 计算两方向夹角，判断是否接近平行。
    const float b = XMVectorGetX(XMVector3Dot(axisDirUnit, rayDirUnit));
    const float denom = 1.0f - b * b;
    if (std::fabsf(denom) < 1e-5f) return false;

    const XMVECTOR r = XMVectorSubtract(axisOrigin, rayOrigin); // p1 - p2
    const float d = XMVectorGetX(XMVector3Dot(axisDirUnit, r));
    const float e = XMVectorGetX(XMVector3Dot(rayDirUnit, r));
    outAxisS = (b * e - d) / denom;
    return true;
}

/**
 * @brief 静态窗口消息入口，转发到 FEngine 实例。
 * @param userPtr 用户指针（FEngine 实例）。
 * @param hwnd 窗口句柄。
 * @param msg 消息类型。
 * @param wParam 消息参数。
 * @param lParam 消息参数。
 * @return LRESULT 消息处理结果。
 * @note 阶段：运行时消息分发阶段。
 */
LRESULT FEngine::WindowMessageHandler(void* userPtr, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // 将消息转交给实例，未绑定时交由默认处理。
    auto* engine = reinterpret_cast<FEngine*>(userPtr);
    if (engine) return engine->HandleWindowMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/**
 * @brief 视口子窗口过程，转发到实例处理。
 * @param hwnd 视口窗口句柄。
 * @param msg 消息类型。
 * @param wParam 消息参数。
 * @param lParam 消息参数。
 * @return LRESULT 消息处理结果。
 * @note 阶段：运行时视口输入处理阶段。
 */
LRESULT CALLBACK FEngine::ViewportWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    FEngine* engine = reinterpret_cast<FEngine*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        // 绑定实例指针到窗口用户数据。
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        engine = reinterpret_cast<FEngine*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(engine));
        return TRUE;
    }
    if (engine) return engine->HandleViewportMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/**
 * @brief 侧边栏控件过程，处理拖拽放置与选择。
 * @param hwnd 侧边栏控件句柄。
 * @param msg 消息类型。
 * @param wParam 消息参数。
 * @param lParam 消息参数。
 * @return LRESULT 消息处理结果。
 * @note 阶段：编辑器 UI 交互阶段。
 */
LRESULT CALLBACK FEngine::SidebarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* engine = reinterpret_cast<FEngine*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!engine || !engine->SidebarOldProc)
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg)
    {
    case WM_LBUTTONDOWN:
        // 记录按下位置，用于判断拖拽意图。
        engine->SidebarMaybeDrag = true;
        engine->SidebarDownX = GET_X_LPARAM(lParam);
        engine->SidebarDownY = GET_Y_LPARAM(lParam);
        engine->SidebarDownTickMs = GetTickCount64();
        // Let the listbox handle selection normally.
        {
            // 先让 ListBox 更新选中项，再同步 Palette 类型。
            const LRESULT r = CallWindowProcW(engine->SidebarOldProc, hwnd, msg, wParam, lParam);
            const int sel = (int)SendMessageW(hwnd, LB_GETCURSEL, 0, 0);
            switch (sel)
            {
            case 0: engine->PaletteType = FSceneObject::EType::Sphere; break;
            case 1: engine->PaletteType = FSceneObject::EType::Box; break;
            case 2: engine->PaletteType = FSceneObject::EType::Cone; break;
            case 3: engine->PaletteType = FSceneObject::EType::RenderDocRock; break;
            default: break;
            }
            return r;
        }
    case WM_MOUSEMOVE:
        if (engine->SidebarMaybeDrag && (wParam & MK_LBUTTON))
        {
            // 判断是否达到拖拽阈值。
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const int dx = x - engine->SidebarDownX;
            const int dy = y - engine->SidebarDownY;

            // Start drag only after moving a bit; otherwise click should just select.
            const bool movedEnough = (dx * dx + dy * dy) >= 16;
            const bool heldLongEnough = (GetTickCount64() - engine->SidebarDownTickMs) >= 250;
            if (!engine->bPlacingFromSidebar && (movedEnough || heldLongEnough))
            {
                // 启动拖拽放置。
                engine->bPlacingFromSidebar = true;
                engine->CommitType = engine->PaletteType;
                SetCapture(hwnd);
            }

            if (engine->bPlacingFromSidebar)
            {
                // 持续刷新鼠标在视口中的位置用于预览。
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(engine->ViewportHwnd, &pt);
                engine->MouseX = pt.x;
                engine->MouseY = pt.y;
            }
        }
        break;
    case WM_LBUTTONUP:
        // 释放拖拽并在有效区域提交放置。
        engine->SidebarMaybeDrag = false;
        engine->SidebarDownTickMs = 0;
        if (engine->bPlacingFromSidebar)
        {
            engine->bPlacingFromSidebar = false;
            ReleaseCapture();

            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(engine->ViewportHwnd, &pt);
            if (pt.x >= 0 && pt.y >= 0 && pt.x < (int)engine->RHI.GetWidth() && pt.y < (int)engine->RHI.GetHeight())
            {
                engine->MouseX = pt.x;
                engine->MouseY = pt.y;
                engine->bCommitPlacement = true;
            }
            return 0;
        }
        break;
    default:
        break;
    }

    return CallWindowProcW(engine->SidebarOldProc, hwnd, msg, wParam, lParam);
}

/**
 * @brief 材质列表控件过程，处理拖拽赋材质。
 * @param hwnd 材质列表句柄。
 * @param msg 消息类型。
 * @param wParam 消息参数。
 * @param lParam 消息参数。
 * @return LRESULT 消息处理结果。
 * @note 阶段：编辑器 UI 交互阶段。
 */
LRESULT CALLBACK FEngine::MaterialWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* engine = reinterpret_cast<FEngine*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!engine || !engine->MaterialOldProc)
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg)
    {
    case WM_LBUTTONDOWN:
        // 记录按下位置，用于拖拽判断。
        engine->MaterialMaybeDrag = true;
        engine->MaterialDownX = GET_X_LPARAM(lParam);
        engine->MaterialDownY = GET_Y_LPARAM(lParam);
        engine->MaterialDownTickMs = GetTickCount64();
        return CallWindowProcW(engine->MaterialOldProc, hwnd, msg, wParam, lParam);
    case WM_MOUSEMOVE:
        if (engine->MaterialMaybeDrag && (wParam & MK_LBUTTON))
        {
            // 判断是否达到拖拽阈值。
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const int dx = x - engine->MaterialDownX;
            const int dy = y - engine->MaterialDownY;
            const bool movedEnough = (dx * dx + dy * dy) >= 16;
            const bool heldLongEnough = (GetTickCount64() - engine->MaterialDownTickMs) >= 250;
            if (!engine->bDraggingMaterial && (movedEnough || heldLongEnough))
            {
                const int sel = (int)SendMessageW(hwnd, LB_GETCURSEL, 0, 0);
                if (sel >= 0)
                {
                    // 开始拖拽材质。
                    engine->bDraggingMaterial = true;
                    engine->DragMaterialIndex = sel;
                    SetCapture(hwnd);
                }
            }
            if (engine->bDraggingMaterial && engine->ViewportHwnd)
            {
                // 更新当前鼠标位置用于预览命中。
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(engine->ViewportHwnd, &pt);
                engine->MouseX = pt.x;
                engine->MouseY = pt.y;
            }
        }
        break;
    case WM_LBUTTONUP:
        // 释放拖拽并记录待投放信息。
        engine->MaterialMaybeDrag = false;
        engine->MaterialDownTickMs = 0;
        if (engine->bDraggingMaterial)
        {
            POINT pt{};
            GetCursorPos(&pt);
            if (engine->ViewportHwnd)
            {
                ScreenToClient(engine->ViewportHwnd, &pt);
                engine->PendingDropMouseX = pt.x;
                engine->PendingDropMouseY = pt.y;
                engine->PendingMaterialIndex = engine->DragMaterialIndex;
                engine->bPendingMaterialDrop = true;
            }

            engine->bDraggingMaterial = false;
            engine->DragMaterialIndex = -1;
            ReleaseCapture();
            return 0;
        }
        break;
    default:
        break;
    }
    return CallWindowProcW(engine->MaterialOldProc, hwnd, msg, wParam, lParam);
}

/**
 * @brief 底部面板窗口过程，处理拖拽文件导入等事件。
 * @param hwnd 面板窗口句柄。
 * @param msg 消息类型。
 * @param wParam 消息参数。
 * @param lParam 消息参数。
 * @return LRESULT 消息处理结果。
 * @note 阶段：编辑器 UI 交互阶段。
 */
LRESULT CALLBACK FEngine::BottomWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* engine = reinterpret_cast<FEngine*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!engine || !engine->BottomOldProc)
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    // Bottom panel is the parent of its child controls; forward commands to the main window handler.
    if (msg == WM_COMMAND)
    {
        // 将子控件命令转发给主窗口统一处理。
        return SendMessageW(GetParent(hwnd), WM_COMMAND, wParam, lParam);
    }

    if (msg == WM_DROPFILES)
    {
        // 拖拽导入纹理：逐文件调用导入逻辑。
        HDROP drop = (HDROP)wParam;
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i)
        {
            wchar_t path[MAX_PATH];
            if (!DragQueryFileW(drop, i, path, MAX_PATH)) continue;
            engine->AddTextureFromFile(path);
        }
        DragFinish(drop);
        return 0;
    }

    return CallWindowProcW(engine->BottomOldProc, hwnd, msg, wParam, lParam);
}

/**
 * @brief 材质编辑器窗口过程，处理控件事件与关闭逻辑。
 * @param hwnd 窗口句柄。
 * @param msg 消息类型。
 * @param wParam 消息参数。
 * @param lParam 消息参数。
 * @return LRESULT 消息处理结果。
 * @note 阶段：编辑器 UI 交互阶段。
 */
LRESULT CALLBACK FEngine::MaterialEditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* engine = reinterpret_cast<FEngine*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_NCCREATE:
    {
        // 创建时绑定 FEngine 实例。
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        engine = reinterpret_cast<FEngine*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)engine);
        return TRUE;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (engine)
        {
            // 窗口销毁时清理编辑状态。
            engine->MaterialEditorHwnd = nullptr;
            engine->EditingMaterialIndex = -1;
        }
        return 0;
    case WM_COMMAND:
        if (!engine) break;
        // ComboBox 改变时立即应用材质参数。
        if ((LOWORD(wParam) == 4105 || LOWORD(wParam) == 4106 || LOWORD(wParam) == 4107 || LOWORD(wParam) == 4108 || LOWORD(wParam) == 4109) &&
            HIWORD(wParam) == CBN_SELCHANGE)
        {
            engine->ApplyMaterialEditorChanges();
            return 0;
        }
        switch (LOWORD(wParam))
        {
        case 4101: // Color
        {
            if (engine->EditingMaterialIndex < 0 || engine->EditingMaterialIndex >= (int)engine->Materials.size()) break;
            // 弹出颜色选择器并回写材质颜色。
            CHOOSECOLORW cc{};
            COLORREF custom[16]{};
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = hwnd;
            cc.lpCustColors = custom;
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            const auto& c = engine->Materials[engine->EditingMaterialIndex].Albedo;
            cc.rgbResult = RGB((int)(c.x * 255.0f), (int)(c.y * 255.0f), (int)(c.z * 255.0f));
            if (ChooseColorW(&cc))
            {
                auto& m = engine->Materials[engine->EditingMaterialIndex];
                m.Albedo = { GetRValue(cc.rgbResult) / 255.0f, GetGValue(cc.rgbResult) / 255.0f, GetBValue(cc.rgbResult) / 255.0f };
                engine->ApplyMaterialEditorChanges();
            }
            return 0;
        }
        case 4102: // Apply
            // 手动应用按钮。
            engine->ApplyMaterialEditorChanges();
            return 0;
        default:
            break;
        }
        break;
    case WM_HSCROLL:
        // Trackbar 滑动：实时更新材质参数。
        if (engine)
            engine->ApplyMaterialEditorChanges();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/**
 * @brief 处理视口窗口消息（输入、鼠标、键盘）。
 * @param hwnd 视口窗口句柄。
 * @param msg 消息类型。
 * @param wParam 消息参数。
 * @param lParam 消息参数。
 * @return LRESULT 消息处理结果。
 * @note 阶段：运行时视口交互阶段。
 */
LRESULT FEngine::HandleViewportMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SETFOCUS:
        return 0;
    case WM_KILLFOCUS:
        // 失去焦点时清理输入状态与鼠标锁定。
        Input.Clear();
        if (Input.Rotating)
        {
            Input.Rotating = false;
            ReleaseCapture();
            ClipCursor(nullptr);
            if (bRmbCursorHidden)
            {
                ShowCursor(TRUE);
                bRmbCursorHidden = false;
            }
            SetCursorPos(RmbSavedCursorPos.x, RmbSavedCursorPos.y);
        }
        Input.Keys[VK_LBUTTON] = false;
        return 0;
    case WM_KEYDOWN:
        // 键盘按下：更新按键表与功能快捷键。
        if (wParam < 256) Input.Keys[wParam] = true;
        if (wParam == VK_F1)
        {
            RenderPath = (RenderPath == FSimpleSceneRenderer::ERenderPath::Deferred) ? FSimpleSceneRenderer::ERenderPath::Forward : FSimpleSceneRenderer::ERenderPath::Deferred;
            if (RenderPathCombo)
                SendMessageW(RenderPathCombo, CB_SETCURSEL, (WPARAM)((RenderPath == FSimpleSceneRenderer::ERenderPath::Deferred) ? 1 : 0), 0);
            return 0;
        }
        if (wParam == VK_PRIOR) // PageUp
        {
            CameraMoveSpeed = Clamp(CameraMoveSpeed * 1.1f, 0.1f, 50.0f);
            return 0;
        }
        if (wParam == VK_NEXT) // PageDown
        {
            CameraMoveSpeed = Clamp(CameraMoveSpeed / 1.1f, 0.1f, 50.0f);
            return 0;
        }
        if (wParam == VK_HOME)
        {
            CameraLookSensitivity = Clamp(CameraLookSensitivity * 1.1f, 0.0002f, 0.02f);
            return 0;
        }
        if (wParam == VK_END)
        {
            CameraLookSensitivity = Clamp(CameraLookSensitivity / 1.1f, 0.0002f, 0.02f);
            return 0;
        }
        if (wParam == VK_DELETE)
        {
            if (SelectedIndex >= 0 && SelectedIndex < (int)Objects.size())
            {
                Objects.erase(Objects.begin() + SelectedIndex);
                if (Objects.empty())
                {
                    SelectedIndex = -1;
                }
                else
                {
                    SelectedIndex = std::min(SelectedIndex, (int)Objects.size() - 1);
                }
                bDragging = false;
                ActiveAxis = EGizmoAxis::None;
            }
            return 0;
        }
        if (!Input.Rotating && (wParam == 'W' || wParam == 'R'))
        {
            GizmoMode = (wParam == 'R') ? EGizmoMode::Scale : EGizmoMode::Translate;
            bDragging = false;
            ActiveAxis = EGizmoAxis::None;
            return 0;
        }
        return 0;
    case WM_KEYUP:
        if (wParam < 256) Input.Keys[wParam] = false;
        return 0;
    case WM_RBUTTONDOWN:
    {
        // 进入右键旋转模式：锁定鼠标并记录中心。
        Input.Keys[VK_RBUTTON] = true;
        SetCapture(hwnd);
        Input.Rotating = true;
        Input.RawMouseDX = 0;
        Input.RawMouseDY = 0;
        Input.RawHasAbs = false;
        GetCursorPos(&RmbSavedCursorPos);

        RECT rc{};
        GetClientRect(hwnd, &rc);
        Input.CenterMouseX = (rc.right - rc.left) / 2;
        Input.CenterMouseY = (rc.bottom - rc.top) / 2;
        Input.LastMouseX = Input.CenterMouseX;
        Input.LastMouseY = Input.CenterMouseY;

        // Hide and lock cursor to viewport center (UE-like)
        if (!bRmbCursorHidden)
        {
            ShowCursor(FALSE);
            bRmbCursorHidden = true;
        }

        POINT tl{ rc.left, rc.top };
        POINT br{ rc.right, rc.bottom };
        ClientToScreen(hwnd, &tl);
        ClientToScreen(hwnd, &br);
        RECT clip{ tl.x, tl.y, br.x, br.y };
        ClipCursor(&clip);

        POINT center{ Input.CenterMouseX, Input.CenterMouseY };
        ClientToScreen(hwnd, &center);
        SetCursorPos(center.x, center.y);

        SetFocus(hwnd);
        return 0;
    }
    case WM_RBUTTONUP:
    {
        // 退出右键旋转模式并恢复鼠标状态。
        Input.Keys[VK_RBUTTON] = false;
        if (Input.Rotating)
        {
            Input.Rotating = false;
            ReleaseCapture();
            ClipCursor(nullptr);
            if (bRmbCursorHidden)
            {
                ShowCursor(TRUE);
                bRmbCursorHidden = false;
            }
            SetCursorPos(RmbSavedCursorPos.x, RmbSavedCursorPos.y);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        MouseX = GET_X_LPARAM(lParam);
        MouseY = GET_Y_LPARAM(lParam);
        // UE-like RMB look uses per-tick cursor deltas; WM_MOUSEMOVE is only used to update
        // mouse position for picking/placement when not rotating.
        return 0;
    case WM_MOUSEWHEEL:
        // UE-like: adjust camera speed while holding RMB.
        if (Input.Rotating)
        {
            // 滚轮仅在右键视角模式下调速。
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (delta > 0) CameraMoveSpeed = Clamp(CameraMoveSpeed * 1.1f, 0.1f, 50.0f);
            else if (delta < 0) CameraMoveSpeed = Clamp(CameraMoveSpeed / 1.1f, 0.1f, 50.0f);
            return 0;
        }
        return 0;
    case WM_INPUT:
        {
            if (Input.Rotating)
            {
                // 使用 RawInput 获取稳定的相对鼠标位移。
                UINT size = 0;
                GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
                if (size)
                {
                    std::vector<uint8> bytes(size);
                    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, bytes.data(), &size, sizeof(RAWINPUTHEADER)) == size)
                    {
                        RAWINPUT* ri = reinterpret_cast<RAWINPUT*>(bytes.data());
                        if (ri->header.dwType == RIM_TYPEMOUSE)
                        {
                            const RAWMOUSE& m = ri->data.mouse;
                            const bool isAbsolute = (m.usFlags & MOUSE_MOVE_ABSOLUTE) != 0;
                            if (!isAbsolute)
                            {
                                // 相对位移直接累加。
                                Input.RawMouseDX += (int)m.lLastX;
                                Input.RawMouseDY += (int)m.lLastY;
                            }
                            else
                            {
                                // Some devices/drivers can report absolute motion. Convert to deltas.
                                const int ax = (int)m.lLastX;
                                const int ay = (int)m.lLastY;
                                if (Input.RawHasAbs)
                                {
                                    Input.RawMouseDX += (ax - Input.RawAbsX);
                                    Input.RawMouseDY += (ay - Input.RawAbsY);
                                }
                                Input.RawAbsX = ax;
                                Input.RawAbsY = ay;
                                Input.RawHasAbs = true;
                            }
                        }
                    }
                }
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_LBUTTONDOWN:
        // 左键按下：开始选取/拖拽。
        SetCapture(hwnd);
        SetFocus(hwnd);
        MouseX = GET_X_LPARAM(lParam);
        MouseY = GET_Y_LPARAM(lParam);
        Input.Keys[VK_LBUTTON] = true;
        return 0;
    case WM_LBUTTONUP:
        Input.Keys[VK_LBUTTON] = false;
        if (bDragging)
        {
            // 结束 Gizmo 拖拽。
            bDragging = false;
            ActiveAxis = EGizmoAxis::None;
        }
        ReleaseCapture();
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

/**
 * @brief 处理主窗口的消息（菜单/控件/系统事件）。
 * @param hwnd 主窗口句柄。
 * @param msg 消息类型。
 * @param wParam 消息参数。
 * @param lParam 消息参数。
 * @return LRESULT 消息处理结果。
 * @note 阶段：运行时 UI 事件处理阶段。
 */
LRESULT FEngine::HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DESTROY:
        // 退出主循环。
        PostQuitMessage(0);
        return 0;
    case WM_SETFOCUS:
        return 0;
    case WM_KILLFOCUS:
        // 失焦时清理输入与捕获状态。
        Input.Clear();
        if (Input.Rotating) ReleaseCapture();
        return 0;
    case WM_KEYDOWN:
        if (wParam < 256) Input.Keys[wParam] = true;
        return 0;
    case WM_KEYUP:
        if (wParam < 256) Input.Keys[wParam] = false;
        return 0;
    case WM_COMMAND:
        // 处理侧栏/底栏控件命令。
        if ((HWND)lParam == RenderPathCombo && HIWORD(wParam) == CBN_SELCHANGE)
        {
            const int sel = (int)SendMessageW(RenderPathCombo, CB_GETCURSEL, 0, 0);
            RenderPath = (sel == 1) ? FSimpleSceneRenderer::ERenderPath::Deferred : FSimpleSceneRenderer::ERenderPath::Forward;
            return 0;
        }
        if ((HWND)lParam == LumenCheckbox && HIWORD(wParam) == BN_CLICKED)
        {
            const LRESULT checked = SendMessageW(LumenCheckbox, BM_GETCHECK, 0, 0);
            bEnableLumen = (checked == BST_CHECKED);
            return 0;
        }
        if ((HWND)lParam == LumenSWRTCheckbox && HIWORD(wParam) == BN_CLICKED)
        {
            const LRESULT checked = SendMessageW(LumenSWRTCheckbox, BM_GETCHECK, 0, 0);
            bEnableLumenSWRT = (checked == BST_CHECKED);
            return 0;
        }
        if ((HWND)lParam == LumenHWRTCheckbox && HIWORD(wParam) == BN_CLICKED)
        {
            const LRESULT checked = SendMessageW(LumenHWRTCheckbox, BM_GETCHECK, 0, 0);
            bEnableLumenHWRT = (checked == BST_CHECKED);
            return 0;
        }
        if (HIWORD(wParam) == LBN_SELCHANGE && (HWND)lParam == SidebarList)
        {
            // 侧栏选择改变：切换放置类型。
            const int sel = (int)SendMessageW(SidebarList, LB_GETCURSEL, 0, 0);
            switch (sel)
            {
            case 0: PaletteType = FSceneObject::EType::Sphere; break;
            case 1: PaletteType = FSceneObject::EType::Box; break;
            case 2: PaletteType = FSceneObject::EType::Cone; break;
            case 3: PaletteType = FSceneObject::EType::RenderDocRock; break;
            default: break;
            }
            return 0;
        }
        if (HIWORD(wParam) == LBN_SELCHANGE && (HWND)lParam == TextureList)
        {
            // 纹理列表选择改变：更新预览。
            const int sel = (int)SendMessageW(TextureList, LB_GETCURSEL, 0, 0);
            SelectedTextureIndex = sel;
            if (TexturePreview)
            {
                HBITMAP bmp = nullptr;
                if (sel >= 0 && sel < (int)Textures.size())
                    bmp = Textures[sel].Preview;
                SendMessageW(TexturePreview, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)bmp);
                InvalidateRect(TexturePreview, nullptr, TRUE);
            }
            return 0;
        }
        if ((HWND)lParam == NewMaterialBtn && HIWORD(wParam) == BN_CLICKED)
        {
            // 创建新材质并添加到列表。
            // Create a new material, optionally from selected texture average color.
            FMaterialAsset mat{};
            mat.Name = L"Material " + std::to_wstring((int)Materials.size() + 1);
            if (SelectedTextureIndex >= 0 && SelectedTextureIndex < (int)Textures.size())
            {
                mat.Albedo = Textures[SelectedTextureIndex].AvgColor;
                mat.AlbedoTexIndex = SelectedTextureIndex;
                mat.AlbedoTexSlot = Textures[SelectedTextureIndex].RendererSlot;
            }
            else
            {
                static std::mt19937 rng{ 1337u };
                std::uniform_real_distribution<float> dist(0.1f, 0.95f);
                mat.Albedo = { dist(rng), dist(rng), dist(rng) };
            }
            mat.Metallic = 0.0f;
            mat.Roughness = 0.4f;

            // Default slots
            mat.NormalTexSlot = 1;
            mat.RoughnessTexSlot = 2;
            mat.MetallicTexSlot = 3;
            mat.AOTexSlot = 4;

            mat.SRVBase = Renderer.AllocateMaterialSRVBlock();
            Renderer.UpdateMaterialSRVBlock(mat.SRVBase, mat.AlbedoTexSlot, mat.NormalTexSlot, mat.RoughnessTexSlot, mat.MetallicTexSlot, mat.AOTexSlot);
            Materials.push_back(mat);
            if (MaterialList)
            {
                SendMessageW(MaterialList, LB_ADDSTRING, 0, (LPARAM)mat.Name.c_str());
                SendMessageW(MaterialList, LB_SETCURSEL, (WPARAM)(Materials.size() - 1), 0);
            }
            return 0;
        }
        if ((HWND)lParam == TonemapCheckbox && HIWORD(wParam) == BN_CLICKED)
        {
            const LRESULT checked = SendMessageW(TonemapCheckbox, BM_GETCHECK, 0, 0);
            bEnableTonemap = (checked == BST_CHECKED);
            return 0;
        }
        if ((HWND)lParam == SkyEnableCheckbox && HIWORD(wParam) == BN_CLICKED)
        {
            // 天空开关：同步 UI 文本。
            const LRESULT checked = SendMessageW(SkyEnableCheckbox, BM_GETCHECK, 0, 0);
            SkySettings.Enable = (checked == BST_CHECKED);
            UpdateSkyUI();
            return 0;
        }
        if (HIWORD(wParam) == LBN_DBLCLK && (HWND)lParam == MaterialList)
        {
            // 双击材质打开编辑器。
            const int sel = (int)SendMessageW(MaterialList, LB_GETCURSEL, 0, 0);
            if (sel >= 0)
                OpenMaterialEditor(sel);
            return 0;
        }
        break;
    case WM_HSCROLL:
    {
        HWND src = (HWND)lParam;
        if (!src) break;
        if (src == SunYawSlider)
        {
            // 太阳偏航角滑条。
            const int deg10 = (int)SendMessageW(SunYawSlider, TBM_GETPOS, 0, 0);
            SunYaw = (float(deg10) / 10.0f) * (DirectX::XM_PI / 180.0f);
            UpdateSkyUI();
            return 0;
        }
        if (src == SunPitchSlider)
        {
            // 太阳俯仰角滑条。
            const int deg10 = (int)SendMessageW(SunPitchSlider, TBM_GETPOS, 0, 0);
            SunPitch = Clamp((float(deg10) / 10.0f) * (DirectX::XM_PI / 180.0f),
                             -DirectX::XM_PIDIV2 + 0.05f, DirectX::XM_PIDIV2 - 0.05f);
            UpdateSkyUI();
            return 0;
        }
        if (src == SunIntensitySlider)
        {
            const int v = (int)SendMessageW(SunIntensitySlider, TBM_GETPOS, 0, 0);
            SunIntensity = float(v) / 100.0f;
            UpdateSkyUI();
            return 0;
        }
        if (src == RayleighSlider)
        {
            const int v = (int)SendMessageW(RayleighSlider, TBM_GETPOS, 0, 0);
            SkySettings.RayleighScale = float(v) / 1000.0f;
            UpdateSkyUI();
            return 0;
        }
        if (src == MieSlider)
        {
            const int v = (int)SendMessageW(MieSlider, TBM_GETPOS, 0, 0);
            SkySettings.MieScale = float(v) / 1000.0f;
            UpdateSkyUI();
            return 0;
        }
        if (src == MieGSlider)
        {
            const int v = (int)SendMessageW(MieGSlider, TBM_GETPOS, 0, 0);
            SkySettings.MieG = float(v) / 1000.0f;
            UpdateSkyUI();
            return 0;
        }
        if (src == AtmoHeightSlider)
        {
            const int v = (int)SendMessageW(AtmoHeightSlider, TBM_GETPOS, 0, 0);
            SkySettings.AtmosphereHeight = std::max(0.5f, float(v) / 100.0f);
            UpdateSkyUI();
            return 0;
        }
        break;
    }
    case WM_SIZE:
        LayoutUI();
        return 0;
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
        // Ensure sidebar uses readable colors (avoid "black on black" on some setups).
        SetTextColor((HDC)wParam, RGB(230, 230, 230));
        SetBkColor((HDC)wParam, RGB(30, 30, 30));
        SetDCBrushColor((HDC)wParam, RGB(30, 30, 30));
        return (LRESULT)GetStockObject(DC_BRUSH);
    case WM_DROPFILES:
    {
        HDROP drop = (HDROP)wParam;
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i)
        {
            wchar_t path[MAX_PATH];
            if (!DragQueryFileW(drop, i, path, MAX_PATH)) continue;
            AddTextureFromFile(path);
        }
        DragFinish(drop);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/**
 * @brief 从文件加载纹理并注册到资源列表与渲染器。
 * @param path 纹理文件路径。
 * @return 无返回值。
 * @note 阶段：编辑器资源导入阶段。
 */
void FEngine::AddTextureFromFile(const std::wstring& path)
{
    // 仅处理支持的图像扩展名。
    if (!HasImageExtension(path)) return;

    // Preview (small) + avg color
    HBITMAP previewBmp = nullptr;
    DirectX::XMFLOAT3 avg{};
    if (!LoadImagePreviewWIC(path, 128, previewBmp, avg))
    {
        // WIC 不支持 TGA 时走自定义解码并生成预览。
        // WIC might not support TGA; handle TGA preview via full decode
        std::vector<uint8> rgba;
        uint32 w = 0, h = 0;
        if (!LoadImageRGBA8TGA(path, rgba, w, h))
            return;
        avg = ComputeAverageRGBA8(rgba, w, h);

        // Create a small preview DIB (nearest)
        const uint32 maxSize = 128;
        uint32 tw = w, th = h;
        if (w > maxSize || h > maxSize)
        {
            const float sx = float(maxSize) / float(w);
            const float sy = float(maxSize) / float(h);
            const float s = (sx < sy) ? sx : sy;
            tw = (uint32)std::max(1.0f, std::floorf(float(w) * s));
            th = (uint32)std::max(1.0f, std::floorf(float(h) * s));
        }

        // 最近邻缩放生成小图像预览。
        std::vector<uint8> smallPixels;
        smallPixels.resize((size_t)tw * (size_t)th * 4);
        for (uint32 y = 0; y < th; ++y)
        {
            const uint32 sy = (uint32)((uint64)y * h / th);
            for (uint32 x = 0; x < tw; ++x)
            {
                const uint32 sx = (uint32)((uint64)x * w / tw);
                const uint8* src = &rgba[(size_t(sy) * w + sx) * 4];
                uint8* dst = &smallPixels[(size_t(y) * tw + x) * 4];
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
            }
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = (LONG)tw;
        bmi.bmiHeader.biHeight = -(LONG)th;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC hdc = GetDC(nullptr);
        previewBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, hdc);
        if (previewBmp && bits)
            std::memcpy(bits, smallPixels.data(), smallPixels.size());
    }

    // 解码用于 GPU 上传的数据（限制最长边 1024）。
    // Decode for renderer upload (limit to 1024 on the longer edge)
    std::vector<uint8> rgba;
    uint32 w = 0, h = 0;
    bool ok = LoadImageRGBA8WIC(path, 1024, rgba, w, h);
    if (!ok)
        ok = LoadImageRGBA8TGA(path, rgba, w, h);
    if (!ok || rgba.empty()) return;

    int slot = 0;
    if (RHI.GetDevice())
        // 上传纹理到渲染器，获取槽位。
        slot = Renderer.CreateTextureRGBA8(RHI, w, h, rgba.data());

    // 写入纹理资产结构。
    FTextureAsset tex{};
    tex.Path = path;
    tex.Preview = previewBmp;
    tex.AvgColor = avg;
    tex.RendererSlot = slot;
    Textures.push_back(tex);

    // 更新纹理列表 UI。
    if (TextureList)
    {
        const auto name = std::filesystem::path(path).filename().wstring();
        SendMessageW(TextureList, LB_ADDSTRING, 0, (LPARAM)name.c_str());
    }

    // If editor is open, refresh texture combo.
    UpdateMaterialEditorControls();
}

/**
 * @brief 打开材质编辑器窗口并加载指定材质数据。
 * @param materialIndex 材质索引。
 * @return 无返回值。
 * @note 阶段：编辑器材质编辑阶段。
 */
void FEngine::OpenMaterialEditor(int materialIndex)
{
    // 校验索引有效性。
    if (materialIndex < 0 || materialIndex >= (int)Materials.size()) return;
    EditingMaterialIndex = materialIndex;

    if (!MaterialEditorHwnd)
    {
        // 创建材质编辑器窗口与子控件。
        constexpr int kSliderSteps = 10000; // higher precision for roughness/metallic

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &FEngine::MaterialEditorWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"DX12MaterialEditorWindow";
        RegisterClassExW(&wc);

        MaterialEditorHwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            wc.lpszClassName,
            L"Material Editor",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            420,
            340,
            Window.GetHwnd(),
            nullptr,
            wc.hInstance,
            this);

        // Create child controls
        CreateWindowExW(0, L"STATIC", L"Color:", WS_CHILD | WS_VISIBLE, 16, 18, 60, 18, MaterialEditorHwnd, nullptr, wc.hInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Pick...", WS_CHILD | WS_VISIBLE, 90, 14, 80, 24, MaterialEditorHwnd, (HMENU)4101, wc.hInstance, nullptr);

        CreateWindowExW(0, L"STATIC", L"Roughness:", WS_CHILD | WS_VISIBLE, 16, 60, 80, 18, MaterialEditorHwnd, nullptr, wc.hInstance, nullptr);
        CreateWindowExW(0, TRACKBAR_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 110, 56, 260, 30, MaterialEditorHwnd, (HMENU)4103, wc.hInstance, nullptr);
        SendMessageW(GetDlgItem(MaterialEditorHwnd, 4103), TBM_SETRANGE, TRUE, MAKELPARAM(0, kSliderSteps));
        SendMessageW(GetDlgItem(MaterialEditorHwnd, 4103), TBM_SETTICFREQ, 1000, 0);
        SendMessageW(GetDlgItem(MaterialEditorHwnd, 4103), TBM_SETLINESIZE, 0, 10);
        SendMessageW(GetDlgItem(MaterialEditorHwnd, 4103), TBM_SETPAGESIZE, 0, 100);

        CreateWindowExW(0, L"STATIC", L"Metallic:", WS_CHILD | WS_VISIBLE, 16, 98, 80, 18, MaterialEditorHwnd, nullptr, wc.hInstance, nullptr);
        CreateWindowExW(0, TRACKBAR_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 110, 94, 260, 30, MaterialEditorHwnd, (HMENU)4104, wc.hInstance, nullptr);
        SendMessageW(GetDlgItem(MaterialEditorHwnd, 4104), TBM_SETRANGE, TRUE, MAKELPARAM(0, kSliderSteps));
        SendMessageW(GetDlgItem(MaterialEditorHwnd, 4104), TBM_SETTICFREQ, 1000, 0);
        SendMessageW(GetDlgItem(MaterialEditorHwnd, 4104), TBM_SETLINESIZE, 0, 10);
        SendMessageW(GetDlgItem(MaterialEditorHwnd, 4104), TBM_SETPAGESIZE, 0, 100);

        CreateWindowExW(0, L"STATIC", L"Albedo Tex:", WS_CHILD | WS_VISIBLE, 16, 138, 80, 18, MaterialEditorHwnd, nullptr, wc.hInstance, nullptr);
        CreateWindowExW(0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 110, 134, 260, 200, MaterialEditorHwnd, (HMENU)4105, wc.hInstance, nullptr);

        CreateWindowExW(0, L"STATIC", L"Normal Tex:", WS_CHILD | WS_VISIBLE, 16, 164, 80, 18, MaterialEditorHwnd, nullptr, wc.hInstance, nullptr);
        CreateWindowExW(0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 110, 160, 260, 200, MaterialEditorHwnd, (HMENU)4106, wc.hInstance, nullptr);

        CreateWindowExW(0, L"STATIC", L"Rough Tex:", WS_CHILD | WS_VISIBLE, 16, 190, 80, 18, MaterialEditorHwnd, nullptr, wc.hInstance, nullptr);
        CreateWindowExW(0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 110, 186, 260, 200, MaterialEditorHwnd, (HMENU)4107, wc.hInstance, nullptr);

        CreateWindowExW(0, L"STATIC", L"Metal Tex:", WS_CHILD | WS_VISIBLE, 16, 216, 80, 18, MaterialEditorHwnd, nullptr, wc.hInstance, nullptr);
        CreateWindowExW(0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 110, 212, 260, 200, MaterialEditorHwnd, (HMENU)4108, wc.hInstance, nullptr);

        CreateWindowExW(0, L"STATIC", L"AO Tex:", WS_CHILD | WS_VISIBLE, 16, 242, 80, 18, MaterialEditorHwnd, nullptr, wc.hInstance, nullptr);
        CreateWindowExW(0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 110, 238, 260, 200, MaterialEditorHwnd, (HMENU)4109, wc.hInstance, nullptr);

        CreateWindowExW(0, L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 290, 276, 80, 26, MaterialEditorHwnd, (HMENU)4102, wc.hInstance, nullptr);
    }

    // 更新窗口标题并刷新控件内容。
    // Update title and controls
    std::wstring title = L"Material Editor - " + Materials[materialIndex].Name;
    SetWindowTextW(MaterialEditorHwnd, title.c_str());
    ShowWindow(MaterialEditorHwnd, SW_SHOW);
    SetForegroundWindow(MaterialEditorHwnd);
    UpdateMaterialEditorControls();
}

/**
 * @brief 更新材质编辑器控件显示，反映当前材质数据。
 * @param 无。
 * @return 无返回值。
 * @note 阶段：UI 同步阶段。
 */
void FEngine::UpdateMaterialEditorControls()
{
    if (!MaterialEditorHwnd) return;
    if (EditingMaterialIndex < 0 || EditingMaterialIndex >= (int)Materials.size()) return;

    const auto& mat = Materials[EditingMaterialIndex];
    constexpr float kSliderSteps = 10000.0f;

    // 更新粗糙度与金属度滑条。
    HWND rough = GetDlgItem(MaterialEditorHwnd, 4103);
    HWND metal = GetDlgItem(MaterialEditorHwnd, 4104);
    if (rough) SendMessageW(rough, TBM_SETPOS, TRUE, (LPARAM)(int)(mat.Roughness * kSliderSteps));
    if (metal) SendMessageW(metal, TBM_SETPOS, TRUE, (LPARAM)(int)(mat.Metallic * kSliderSteps));

    // 填充 Albedo 纹理下拉框。
    HWND combo = GetDlgItem(MaterialEditorHwnd, 4105);
    if (combo)
    {
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"<None>");
        for (const auto& t : Textures)
        {
            const auto name = std::filesystem::path(t.Path).filename().wstring();
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)name.c_str());
        }

        int sel = 0;
        if (mat.AlbedoTexIndex >= 0) sel = 1 + mat.AlbedoTexIndex;
        SendMessageW(combo, CB_SETCURSEL, sel, 0);
    }

    // 填充其余纹理下拉框。
    auto setupCombo = [&](int id, int texIndex)
    {
        HWND cb = GetDlgItem(MaterialEditorHwnd, id);
        if (!cb) return;
        SendMessageW(cb, CB_RESETCONTENT, 0, 0);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"<None>");
        for (const auto& t : Textures)
        {
            const auto name = std::filesystem::path(t.Path).filename().wstring();
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)name.c_str());
        }
        int sel = 0;
        if (texIndex >= 0) sel = 1 + texIndex;
        SendMessageW(cb, CB_SETCURSEL, sel, 0);
    };

    setupCombo(4106, mat.NormalTexIndex);
    setupCombo(4107, mat.RoughnessTexIndex);
    setupCombo(4108, mat.MetallicTexIndex);
    setupCombo(4109, mat.AOTexIndex);
}

/**
 * @brief 将材质编辑器控件的值应用到材质与场景对象。
 * @param 无。
 * @return 无返回值。
 * @note 阶段：编辑器材质编辑提交阶段。
 */
void FEngine::ApplyMaterialEditorChanges()
{
    if (!MaterialEditorHwnd) return;
    if (EditingMaterialIndex < 0 || EditingMaterialIndex >= (int)Materials.size()) return;

    auto& mat = Materials[EditingMaterialIndex];
    constexpr float kSliderSteps = 10000.0f;

    // 读取滑条并更新粗糙度与金属度。
    HWND rough = GetDlgItem(MaterialEditorHwnd, 4103);
    HWND metal = GetDlgItem(MaterialEditorHwnd, 4104);

    if (rough) mat.Roughness = Clamp((float)SendMessageW(rough, TBM_GETPOS, 0, 0) / kSliderSteps, 0.0f, 1.0f);
    if (metal) mat.Metallic = Clamp((float)SendMessageW(metal, TBM_GETPOS, 0, 0) / kSliderSteps, 0.0f, 1.0f);

    // 读取下拉框选择，更新纹理索引与槽位。
    auto readCombo = [&](int id, int& outIndex, int& outSlot, int defaultSlot)
    {
        outIndex = -1;
        outSlot = defaultSlot;
        HWND cb = GetDlgItem(MaterialEditorHwnd, id);
        if (!cb) return;
        const int sel = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
        if (sel > 0)
        {
            const int ti = sel - 1;
            if (ti >= 0 && ti < (int)Textures.size())
            {
                outIndex = ti;
                outSlot = Textures[ti].RendererSlot;
            }
        }
    };

    readCombo(4105, mat.AlbedoTexIndex, mat.AlbedoTexSlot, 0);
    readCombo(4106, mat.NormalTexIndex, mat.NormalTexSlot, 1);
    readCombo(4107, mat.RoughnessTexIndex, mat.RoughnessTexSlot, 2);
    readCombo(4108, mat.MetallicTexIndex, mat.MetallicTexSlot, 3);
    readCombo(4109, mat.AOTexIndex, mat.AOTexSlot, 4);

    // Update renderer descriptor block.
    Renderer.UpdateMaterialSRVBlock(mat.SRVBase, mat.AlbedoTexSlot, mat.NormalTexSlot, mat.RoughnessTexSlot, mat.MetallicTexSlot, mat.AOTexSlot);

    // Hot-reload objects that reference this material index.
    for (auto& obj : Objects)
    {
        if (obj.MaterialIndex == EditingMaterialIndex)
        {
            // 同步材质参数到对象实例。
            obj.Albedo = mat.Albedo;
            obj.Metallic = mat.Metallic;
            obj.Roughness = mat.Roughness;
            obj.MaterialSRVBase = mat.SRVBase;
            obj.UseAlbedoTex = (mat.AlbedoTexIndex >= 0) ? 1.0f : 0.0f;
            obj.UseNormalTex = (mat.NormalTexIndex >= 0) ? 1.0f : 0.0f;
            obj.UseRoughnessTex = (mat.RoughnessTexIndex >= 0) ? 1.0f : 0.0f;
            obj.UseMetallicTex = (mat.MetallicTexIndex >= 0) ? 1.0f : 0.0f;
            obj.UseAOTex = (mat.AOTexIndex >= 0) ? 1.0f : 0.0f;
        }
    }
}

/**
 * @brief 重新布局侧栏、视口与底部面板的控件。
 * @param 无。
 * @return 无返回值。
 * @note 阶段：窗口尺寸变化/初始化布局阶段。
 */
void FEngine::LayoutUI()
{
    if (!Window.GetHwnd()) return;

    // 计算主窗口与视口尺寸。
    RECT rc{};
    GetClientRect(Window.GetHwnd(), &rc);
    const int clientW = (int)(rc.right - rc.left);
    const int clientH = (int)(rc.bottom - rc.top);
    const int viewportW = std::max(1, clientW - SidebarWidthPx);
    const int viewportH = std::max(1, clientH - BottomPanelHeightPx);

    const int sidebarH = viewportH;
    const int titleH = 22;
    const int paletteH = std::min(120, sidebarH);

    // 顶部标题与物体列表区域。
    if (EngineNameLabel)
        MoveWindow(EngineNameLabel, 0, 0, SidebarWidthPx, titleH, TRUE);
    if (SidebarList)
        MoveWindow(SidebarList, 0, titleH, SidebarWidthPx, std::max(1, paletteH - titleH), TRUE);

    // Sidebar controls live below the palette list (in main window coordinates).
    const int sidebarX = 0;
    const int baseY = paletteH + 8;
    const int innerW = SidebarWidthPx - 16;

    // 统一摆放侧栏控件。
    auto place = [&](HWND h, int y, int hgt)
    {
        if (!h) return;
        MoveWindow(h, sidebarX + 8, baseY + y, innerW, hgt, TRUE);
    };

    place(RenderPathLabel, 0, 16);
    place(RenderPathCombo, 18, 24);
    place(LumenCheckbox, 44, 20);
    place(LumenSWRTCheckbox, 66, 20);
    place(LumenHWRTCheckbox, 88, 20);

    const int yOff = 114;
    place(SkyEnableCheckbox, yOff + 0, 20);
    place(SunYawLabel, yOff + 24, 16);
    place(SunYawSlider, yOff + 40, 26);
    place(SunPitchLabel, yOff + 66, 16);
    place(SunPitchSlider, yOff + 82, 26);
    place(SunIntensityLabel, yOff + 108, 16);
    place(SunIntensitySlider, yOff + 124, 26);
    place(RayleighLabel, yOff + 150, 16);
    place(RayleighSlider, yOff + 166, 26);
    place(MieLabel, yOff + 192, 16);
    place(MieSlider, yOff + 208, 26);
    place(MieGLabel, yOff + 234, 16);
    place(MieGSlider, yOff + 250, 26);
    place(AtmoHeightLabel, yOff + 276, 16);
    place(AtmoHeightSlider, yOff + 292, 26);
    place(SkyLabel, yOff + 326, 62);

    // 视口与底部面板布局。
    if (ViewportHwnd)
        MoveWindow(ViewportHwnd, SidebarWidthPx, 0, viewportW, viewportH, TRUE);

    if (BottomPanel)
        MoveWindow(BottomPanel, 0, viewportH, clientW, BottomPanelHeightPx, TRUE);

    if (BottomPanel)
    {
        // 计算底栏三列布局并更新控件位置。
        const int padding = 8;
        const int colW = std::max(80, (clientW - padding * 4) / 3);
        const int listH = std::max(40, BottomPanelHeightPx - padding * 2 - 26);

        if (TextureList)
            MoveWindow(TextureList, padding, padding, colW, listH, TRUE);
        if (TexturePreview)
            MoveWindow(TexturePreview, padding + colW + padding, padding, colW, listH, TRUE);
        if (MaterialList)
            MoveWindow(MaterialList, padding + (colW + padding) * 2, padding, colW, listH, TRUE);
        if (NewMaterialBtn)
            MoveWindow(NewMaterialBtn, padding + (colW + padding) * 2, padding + listH + 4, colW, 22, TRUE);
        if (TonemapCheckbox)
            MoveWindow(TonemapCheckbox, padding + colW + padding, padding + listH + 4, colW, 22, TRUE);
    }

    // 同步 RHI 尺寸。
    if (ViewportHwnd && RHI.GetDevice())
        RHI.Resize((uint32)viewportW, (uint32)viewportH);

    // Ensure Win32 panels are visible above the swapchain host window.
    // (Flip-model swapchains are sensitive to z-order / overlapping child windows.)
    if (ViewportHwnd)
        SetWindowPos(ViewportHwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // 将侧栏与底栏控件保持在顶层。
    auto bringTop = [](HWND h)
    {
        if (!h) return;
        SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    };

    bringTop(SidebarList);
    bringTop(EngineNameLabel);
    bringTop(RenderPathLabel);
    bringTop(RenderPathCombo);
    bringTop(LumenCheckbox);
    bringTop(LumenSWRTCheckbox);
    bringTop(LumenHWRTCheckbox);
    bringTop(SkyEnableCheckbox);
    bringTop(SunYawLabel);
    bringTop(SunYawSlider);
    bringTop(SunPitchLabel);
    bringTop(SunPitchSlider);
    bringTop(SunIntensityLabel);
    bringTop(SunIntensitySlider);
    bringTop(RayleighLabel);
    bringTop(RayleighSlider);
    bringTop(MieLabel);
    bringTop(MieSlider);
    bringTop(MieGLabel);
    bringTop(MieGSlider);
    bringTop(AtmoHeightLabel);
    bringTop(AtmoHeightSlider);
    bringTop(SkyLabel);
    bringTop(BottomPanel);
}

/**
 * @brief 同步天空参数到 UI 控件与文本。
 * @param 无。
 * @return 无返回值。
 * @note 阶段：天空参数 UI 更新阶段。
 */
void FEngine::UpdateSkyUI()
{
    // 刷新滑条数值。
    if (SunYawSlider)
    {
        const int deg10 = (int)std::lround((SunYaw * 180.0f / DirectX::XM_PI) * 10.0f);
        SendMessageW(SunYawSlider, TBM_SETPOS, TRUE, (LPARAM)((deg10 % 3600 + 3600) % 3600));
    }
    if (SunPitchSlider)
    {
        const int deg10 = (int)std::lround((SunPitch * 180.0f / DirectX::XM_PI) * 10.0f);
        SendMessageW(SunPitchSlider, TBM_SETPOS, TRUE, (LPARAM)ClampI(deg10, -890, 890));
    }
    if (SunIntensitySlider)
    {
        SendMessageW(SunIntensitySlider, TBM_SETPOS, TRUE, (LPARAM)ClampI((int)std::lround(SunIntensity * 100.0f), 0, 2000));
    }
    if (RayleighSlider)
    {
        SendMessageW(RayleighSlider, TBM_SETPOS, TRUE, (LPARAM)ClampI((int)std::lround(SkySettings.RayleighScale * 1000.0f), 0, 3000));
    }
    if (MieSlider)
    {
        SendMessageW(MieSlider, TBM_SETPOS, TRUE, (LPARAM)ClampI((int)std::lround(SkySettings.MieScale * 1000.0f), 0, 3000));
    }
    if (MieGSlider)
    {
        SendMessageW(MieGSlider, TBM_SETPOS, TRUE, (LPARAM)ClampI((int)std::lround(SkySettings.MieG * 1000.0f), 0, 990));
    }
    if (AtmoHeightSlider)
    {
        SendMessageW(AtmoHeightSlider, TBM_SETPOS, TRUE, (LPARAM)ClampI((int)std::lround(SkySettings.AtmosphereHeight * 100.0f), 50, 3000));
    }
    if (SkyEnableCheckbox)
        SendMessageW(SkyEnableCheckbox, BM_SETCHECK, SkySettings.Enable ? BST_CHECKED : BST_UNCHECKED, 0);

    // 更新状态文本。
    if (SkyLabel)
    {
        wchar_t buf[256];
        const float yawDeg = SunYaw * 180.0f / DirectX::XM_PI;
        const float pitchDeg = SunPitch * 180.0f / DirectX::XM_PI;
        swprintf_s(buf, L"Yaw=%.1f  Pitch=%.1f\nI=%.2f  R=%.2f  M=%.2f  g=%.2f\nH=%.2f",
                   yawDeg, pitchDeg, SunIntensity, SkySettings.RayleighScale, SkySettings.MieScale, SkySettings.MieG, SkySettings.AtmosphereHeight);
        SetWindowTextW(SkyLabel, buf);
    }
}

/**
 * @brief 每帧更新输入、交互、摄像机与物体操作。
 * @param dtSeconds 帧间隔时间（秒）。
 * @return 无返回值。
 * @note 阶段：每帧更新阶段（渲染前）。
 */
void FEngine::Tick(float dtSeconds)
{
    using namespace DirectX;

    // 侧栏长按：无移动也允许拖拽放置。
    // 侧栏长按：即使没有鼠标移动也触发拖拽放置。
    // Sidebar long-press: start drag even without mouse move events.
    if (SidebarMaybeDrag && !bPlacingFromSidebar && SidebarList && (GetKeyState(VK_LBUTTON) & 0x8000))
    {
        const uint64 nowMs = GetTickCount64();
        if (SidebarDownTickMs != 0 && (nowMs - SidebarDownTickMs) >= 250)
        {
            bPlacingFromSidebar = true;
            CommitType = PaletteType;
            SetCapture(SidebarList);
        }
    }

    // UE 风格右键视角：使用 RawInput 相对位移。
    // UE-like RMB look: use raw mouse deltas (stable, unbounded).
    if (Input.Rotating)
    {
        const int dx = Input.RawMouseDX;
        const int dy = Input.RawMouseDY;
        Input.RawMouseDX = 0;
        Input.RawMouseDY = 0;
        if (dx != 0 || dy != 0)
        {
            const float sensitivity = CameraLookSensitivity;
            Camera.Yaw += float(dx) * sensitivity;
            Camera.Pitch += float(dy) * sensitivity;

            const float limit = DirectX::XM_PIDIV2 - 0.01f;
            Camera.Pitch = Clamp(Camera.Pitch, -limit, limit);
        }
    }

    // UE 风格自由飞行：仅在右键按下时生效。
    // UE-like "fly" navigation: move only while holding RMB.
    if (Input.Rotating)
    {
        const float baseSpeed = CameraMoveSpeed * (Input.Keys[VK_SHIFT] ? 2.0f : 1.0f);
        const float speed = baseSpeed * dtSeconds;

        XMVECTOR forward = Camera.GetForwardVector();
        const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, forward));

        XMVECTOR pos = XMLoadFloat3(&Camera.Position);
        if (Input.Keys['W']) pos = XMVectorAdd(pos, XMVectorScale(forward, speed));
        if (Input.Keys['S']) pos = XMVectorSubtract(pos, XMVectorScale(forward, speed));
        if (Input.Keys['D']) pos = XMVectorAdd(pos, XMVectorScale(right, speed));
        if (Input.Keys['A']) pos = XMVectorSubtract(pos, XMVectorScale(right, speed));
        if (Input.Keys['E']) pos = XMVectorAdd(pos, XMVectorScale(up, speed));
        if (Input.Keys['Q']) pos = XMVectorSubtract(pos, XMVectorScale(up, speed));
        XMStoreFloat3(&Camera.Position, pos);
    }

    // 太阳方向快捷键控制（J/L 偏航，I/K 俯仰）。
    // Sun direction controls (J/L yaw, I/K pitch)
    const float sunSpeed = 1.2f * dtSeconds;
    if (Input.Keys['J']) SunYaw -= sunSpeed;
    if (Input.Keys['L']) SunYaw += sunSpeed;
    if (Input.Keys['I']) SunPitch += sunSpeed;
    if (Input.Keys['K']) SunPitch -= sunSpeed;
    SunPitch = Clamp(SunPitch, -DirectX::XM_PIDIV2 + 0.05f, DirectX::XM_PIDIV2 - 0.05f);
    if (Input.Keys['J'] || Input.Keys['L'] || Input.Keys['I'] || Input.Keys['K'])
        UpdateSkyUI();

    // 视口内的选取/放置/Gizmo 拖拽（左键）。
    // Selection + placement + gizmo drag (left mouse) in viewport
    const uint32 w = RHI.GetWidth();
    const uint32 h = RHI.GetHeight();
    const float aspect = (h == 0) ? 1.0f : (float)w / (float)h;
    const XMMATRIX view = Camera.GetViewMatrix();
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, 0.1f, 100.0f);
    const XMMATRIX viewProj = view * proj;
    const XMMATRIX invViewProj = XMMatrixInverse(nullptr, viewProj);

    static bool prevLDown = false;
    const bool lDown = Input.Keys[VK_LBUTTON];

    // 拖拽放置时每帧更新鼠标（非消息驱动）。
    // While dragging from sidebar, follow the mouse every frame (not message-driven).
    if (bPlacingFromSidebar && ViewportHwnd)
    {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(ViewportHwnd, &pt);
        MouseX = pt.x;
        MouseY = pt.y;
    }

    // 放置预览（MouseX/Y 已是视口坐标）。
    // Placement preview (MouseX/Y already in viewport coords)
    if (bPlacingFromSidebar || bCommitPlacement)
    {
        // 鼠标在视口外则跳过，保留最后有效预览。
        // Ignore if cursor is outside viewport; keep last valid preview.
        if (MouseX < 0 || MouseY < 0 || MouseX >= (int)w || MouseY >= (int)h)
            goto PlacementDone;

        const XMVECTOR camForward = Camera.GetForwardVector();

        XMVECTOR rayOrigin{};
        const XMVECTOR rayDir = MakeRayDirFromScreen(MouseX, MouseY, w, h, invViewProj, rayOrigin);
        float tPlane = 0.0f;
        // 优先与地面 y=0 求交；若平行则退化为面向相机的平面。
        // Prefer ground plane y=0; if ray is parallel, fallback to a view-facing plane.
        const XMVECTOR groundPoint = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
        const XMVECTOR groundNormal = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        bool hit = RayPlaneIntersect(rayOrigin, rayDir, groundPoint, groundNormal, tPlane);
        if (!hit)
        {
            const XMVECTOR camPos = XMLoadFloat3(&Camera.Position);
            const XMVECTOR planePoint = XMVectorAdd(camPos, XMVectorScale(camForward, 5.0f));
            hit = RayPlaneIntersect(rayOrigin, rayDir, planePoint, camForward, tPlane);
        }
        if (hit)
        {
            const XMVECTOR p = XMVectorAdd(rayOrigin, XMVectorScale(rayDir, tPlane));
            XMStoreFloat3(&PreviewPos, p);
        }
    }
PlacementDone:

    // 在下一帧提交放置（基于当前鼠标位置）。
    // Commit placement on next tick (computed at current mouse)
    if (bCommitPlacement)
    {
        bCommitPlacement = false;
        FSceneObject obj{};
        obj.Type = CommitType;
        obj.Position = PreviewPos;
        if (obj.Type == FSceneObject::EType::RenderDocRock)
        {
            obj.Position.y += 0.39f;
            obj.Scale = { 0.35f, 0.35f, 0.35f };
        }
        switch (obj.Type)
        {
        case FSceneObject::EType::Sphere: obj.Radius = 0.75f; break;
        case FSceneObject::EType::Box: obj.Radius = 0.9f; break;
        case FSceneObject::EType::Cone: obj.Radius = 0.9f; break;
        case FSceneObject::EType::RenderDocRock: obj.Radius = 2.05f; break;
        }
        // 按类型设置默认材质颜色。
        // Default material per type
        switch (obj.Type)
        {
        case FSceneObject::EType::Sphere: obj.Albedo = { 0.85f, 0.15f, 0.10f }; break;
        case FSceneObject::EType::Box: obj.Albedo = { 0.18f, 0.55f, 0.95f }; break;
        case FSceneObject::EType::Cone: obj.Albedo = { 0.95f, 0.75f, 0.20f }; break;
        case FSceneObject::EType::RenderDocRock: obj.Albedo = { 0.45f, 0.45f, 0.45f }; break;
        }
        obj.Metallic = 0.0f;
        obj.Roughness = 0.35f;
        obj.MaterialIndex = -1;
        obj.MaterialSRVBase = Renderer.AllocateMaterialSRVBlock();
        Renderer.UpdateMaterialSRVBlock(obj.MaterialSRVBase, 0, 1, 2, 3, 4);
        Objects.push_back(obj);
    }

    // 点击：优先拾取 Gizmo 轴，否则拾取物体。
    // On click: pick either gizmo axis (if selected) or the sphere.
    if (lDown && !prevLDown)
    {
        // 若处于放置拖拽，不进行场景拾取。
        // Skip scene picking when starting a placement drag.
        if (bPlacingFromSidebar)
        {
            prevLDown = lDown;
            return;
        }

        ActiveAxis = EGizmoAxis::None;
        bDragging = false;

        XMVECTOR rayOrigin{};
        const XMVECTOR rayDir = MakeRayDirFromScreen(MouseX, MouseY, w, h, invViewProj, rayOrigin);

        const bool hasSelection = (SelectedIndex >= 0 && SelectedIndex < (int)Objects.size());
        const XMVECTOR selectedCenter = hasSelection ? XMLoadFloat3(&Objects[SelectedIndex].Position) : XMVectorZero();

        // 已选中物体时，先在屏幕空间测试 Gizmo 轴线命中。
        // If sphere is selected, try axis hit-test in screen-space (like UE).
        if (hasSelection)
        {
            constexpr float gizmoLen = 1.5f;
            const XMVECTOR p0 = selectedCenter;
            const XMVECTOR px = XMVectorAdd(p0, XMVectorSet(gizmoLen, 0.0f, 0.0f, 0.0f));
            const XMVECTOR py = XMVectorAdd(p0, XMVectorSet(0.0f, gizmoLen, 0.0f, 0.0f));
            const XMVECTOR pz = XMVectorAdd(p0, XMVectorSet(0.0f, 0.0f, gizmoLen, 0.0f));

            float x0, y0, x1, y1;
            float bestDist = 1e9f;
            EGizmoAxis bestAxis = EGizmoAxis::None;
            constexpr float kPickThresholdPx = 12.0f;

            if (ProjectToScreen(p0, viewProj, w, h, x0, y0))
            {
                if (ProjectToScreen(px, viewProj, w, h, x1, y1))
                {
                    const float d = DistancePointToSegment2D((float)MouseX, (float)MouseY, x0, y0, x1, y1);
                    if (d < bestDist) { bestDist = d; bestAxis = EGizmoAxis::X; }
                }
                if (ProjectToScreen(py, viewProj, w, h, x1, y1))
                {
                    const float d = DistancePointToSegment2D((float)MouseX, (float)MouseY, x0, y0, x1, y1);
                    if (d < bestDist) { bestDist = d; bestAxis = EGizmoAxis::Y; }
                }
                if (ProjectToScreen(pz, viewProj, w, h, x1, y1))
                {
                    const float d = DistancePointToSegment2D((float)MouseX, (float)MouseY, x0, y0, x1, y1);
                    if (d < bestDist) { bestDist = d; bestAxis = EGizmoAxis::Z; }
                }
            }

            if (bestDist <= kPickThresholdPx)
            {
                ActiveAxis = bestAxis;

                XMVECTOR axisDir{};
                switch (ActiveAxis)
                {
                case EGizmoAxis::X: axisDir = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f); break;
                case EGizmoAxis::Y: axisDir = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f); break;
                case EGizmoAxis::Z: axisDir = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f); break;
                default: break;
                }

                float s0 = 0.0f;
                if (ClosestAxisParamToRay(selectedCenter, axisDir, rayOrigin, rayDir, s0))
                {
                    bDragging = true;
                    DragAxisS0 = s0;
                    DragStartPos = Objects[SelectedIndex].Position;
                    DragStartScale = Objects[SelectedIndex].Scale;
                }
            }
        }

        // 若未命中 Gizmo，则进行物体拾取。
        // If we didn't start dragging an axis, pick an object.
        if (!bDragging)
        {
            float bestT = 1e30f;
            int bestIdx = -1;
            for (int i = 0; i < (int)Objects.size(); ++i)
            {
                float tHit = 0.0f;
                const XMVECTOR c = XMLoadFloat3(&Objects[i].Position);
                if (RaySphereIntersect(rayOrigin, rayDir, c, GetObjectPickRadius(Objects[i]), tHit))
                {
                    if (tHit < bestT)
                    {
                        bestT = tHit;
                        bestIdx = i;
                    }
                }
            }

            SelectedIndex = bestIdx;
        }
    }

    // 拖拽：沿选中轴进行平移或缩放。
    // Dragging: translate or scale along selected axis.
    if (lDown && bDragging && SelectedIndex >= 0 && SelectedIndex < (int)Objects.size() && ActiveAxis != EGizmoAxis::None)
    {
        XMVECTOR rayOrigin{};
        const XMVECTOR rayDir = MakeRayDirFromScreen(MouseX, MouseY, w, h, invViewProj, rayOrigin);

        const XMVECTOR axisOrigin = XMLoadFloat3(&DragStartPos);
        XMVECTOR axisDir{};
        switch (ActiveAxis)
        {
        case EGizmoAxis::X: axisDir = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f); break;
        case EGizmoAxis::Y: axisDir = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f); break;
        case EGizmoAxis::Z: axisDir = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f); break;
        default: axisDir = XMVectorZero(); break;
        }

        float s = 0.0f;
        if (ClosestAxisParamToRay(axisOrigin, axisDir, rayOrigin, rayDir, s))
        {
            const float delta = s - DragAxisS0;
            if (GizmoMode == EGizmoMode::Scale)
            {
                constexpr float kScaleSpeed = 0.5f;
                DirectX::XMFLOAT3 newScale = DragStartScale;
                switch (ActiveAxis)
                {
                case EGizmoAxis::X: newScale.x = std::max(0.1f, DragStartScale.x + delta * kScaleSpeed); break;
                case EGizmoAxis::Y: newScale.y = std::max(0.1f, DragStartScale.y + delta * kScaleSpeed); break;
                case EGizmoAxis::Z: newScale.z = std::max(0.1f, DragStartScale.z + delta * kScaleSpeed); break;
                default: break;
                }
                Objects[SelectedIndex].Scale = newScale;
            }
            else
            {
                XMVECTOR newPos = XMVectorAdd(axisOrigin, XMVectorScale(axisDir, delta));
                XMStoreFloat3(&Objects[SelectedIndex].Position, newPos);
            }
        }
    }

    prevLDown = lDown;

    // 若拖拽材质，持续更新鼠标在视口内的坐标。
    // If dragging a material, keep mouse in viewport coords updated every frame.
    if (bDraggingMaterial && ViewportHwnd)
    {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(ViewportHwnd, &pt);
        MouseX = pt.x;
        MouseY = pt.y;
    }
}

/**
 * @brief 引擎主循环入口：初始化系统、创建窗口、初始化渲染器并开始帧循环。
 * @param hInstance 应用实例句柄。
 * @return 无返回值；内部维护消息泵并在退出时释放资源。
 * @note 阶段：应用启动与运行阶段。
 */
void FEngine::Run(HINSTANCE hInstance)
{
    StartTickMs = GetTickCount64();
    PrevTickMs = StartTickMs;

    // 初始化 COM 与通用控件。
    ThrowIfFailed(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx failed");
    {
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_BAR_CLASSES;
        InitCommonControlsEx(&icc);
    }

    // 创建主窗口与侧栏、视口基础。
    const wchar_t* baseTitle = L"ShellEngine";
    Window.Create(hInstance, 1280, 720, baseTitle, &FEngine::WindowMessageHandler, this);
    Window.Show(SW_SHOW);
    DragAcceptFiles(Window.GetHwnd(), TRUE);

    // Sidebar listbox (native Win32 UI)
    EngineNameLabel = CreateWindowExW(
        0,
        L"STATIC",
        baseTitle,
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        0,
        0,
        SidebarWidthPx,
        22,
        Window.GetHwnd(),
        (HMENU)1000,
        GetModuleHandleW(nullptr),
        nullptr);

    SidebarList = CreateWindowExW(
        0,
        L"LISTBOX",
        nullptr,
        WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
        0,
        0,
        SidebarWidthPx,
        (int)Window.GetHeight(),
        Window.GetHwnd(),
        (HMENU)1001,
        GetModuleHandleW(nullptr),
        nullptr);
    SendMessageW(SidebarList, LB_ADDSTRING, 0, (LPARAM)L"Sphere");
    SendMessageW(SidebarList, LB_ADDSTRING, 0, (LPARAM)L"Box");
    SendMessageW(SidebarList, LB_ADDSTRING, 0, (LPARAM)L"Cone");
    SendMessageW(SidebarList, LB_ADDSTRING, 0, (LPARAM)L"RenderDoc Rock");
    SendMessageW(SidebarList, LB_SETCURSEL, 0, 0);

    // Subclass sidebar to support drag-to-place
    SetWindowLongPtrW(SidebarList, GWLP_USERDATA, (LONG_PTR)this);
    SidebarOldProc = (WNDPROC)SetWindowLongPtrW(SidebarList, GWLP_WNDPROC, (LONG_PTR)&FEngine::SidebarWndProc);

    // Sidebar: render path selector
    {
        RenderPathLabel = CreateWindowExW(
            0, L"STATIC", L"Render Path",
            WS_CHILD | WS_VISIBLE,
            8, 130, SidebarWidthPx - 16, 16,
            Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);

        RenderPathCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            8, 148, SidebarWidthPx - 16, 200,
            Window.GetHwnd(), (HMENU)3000, GetModuleHandleW(nullptr), nullptr);

        SendMessageW(RenderPathCombo, CB_ADDSTRING, 0, (LPARAM)L"Forward");
        SendMessageW(RenderPathCombo, CB_ADDSTRING, 0, (LPARAM)L"Deferred");
        SendMessageW(RenderPathCombo, CB_SETCURSEL, (WPARAM)((RenderPath == FSimpleSceneRenderer::ERenderPath::Deferred) ? 1 : 0), 0);

        LumenCheckbox = CreateWindowExW(
            0, L"BUTTON", L"Lumen",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            8, 176, SidebarWidthPx - 16, 20,
            Window.GetHwnd(), (HMENU)3009, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(LumenCheckbox, BM_SETCHECK, bEnableLumen ? BST_CHECKED : BST_UNCHECKED, 0);

        LumenSWRTCheckbox = CreateWindowExW(
            0, L"BUTTON", L"SWRT GI (Caches)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            8, 198, SidebarWidthPx - 16, 20,
            Window.GetHwnd(), (HMENU)3011, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(LumenSWRTCheckbox, BM_SETCHECK, bEnableLumenSWRT ? BST_CHECKED : BST_UNCHECKED, 0);

        LumenHWRTCheckbox = CreateWindowExW(
            0, L"BUTTON", L"HWRT GI (Probes)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            8, 220, SidebarWidthPx - 16, 20,
            Window.GetHwnd(), (HMENU)3010, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(LumenHWRTCheckbox, BM_SETCHECK, bEnableLumenHWRT ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    // Sidebar: SkyAtmosphere + Sun controls
    {
        SkySettings.Enable = true;
        SkySettings.AtmosphereHeight = 12.0f;
        SkySettings.RayleighScale = 1.0f;
        SkySettings.MieScale = 1.0f;
        SkySettings.MieG = 0.8f;
        SkySettings.GroundAlbedo = 0.2f;

        SkyEnableCheckbox = CreateWindowExW(
            0, L"BUTTON", L"SkyAtmosphere",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            8, 130, SidebarWidthPx - 16, 20,
            Window.GetHwnd(), (HMENU)3001, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(SkyEnableCheckbox, BM_SETCHECK, SkySettings.Enable ? BST_CHECKED : BST_UNCHECKED, 0);

        auto makeSlider = [&](int y, int id)
        {
            return CreateWindowExW(0, TRACKBAR_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 8, y, SidebarWidthPx - 16, 26, Window.GetHwnd(), (HMENU)id, GetModuleHandleW(nullptr), nullptr);
        };

        SunYawLabel = CreateWindowExW(0, L"STATIC", L"Sun Yaw", WS_CHILD | WS_VISIBLE, 8, 154, SidebarWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        SunYawSlider = makeSlider(170, 3002);
        SendMessageW(SunYawSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 3600));
        SendMessageW(SunYawSlider, TBM_SETTICFREQ, 300, 0);

        SunPitchLabel = CreateWindowExW(0, L"STATIC", L"Sun Pitch", WS_CHILD | WS_VISIBLE, 8, 196, SidebarWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        SunPitchSlider = makeSlider(212, 3003);
        SendMessageW(SunPitchSlider, TBM_SETRANGE, TRUE, MAKELPARAM(-890, 890));
        SendMessageW(SunPitchSlider, TBM_SETTICFREQ, 200, 0);

        SunIntensityLabel = CreateWindowExW(0, L"STATIC", L"Sun Intensity", WS_CHILD | WS_VISIBLE, 8, 238, SidebarWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        SunIntensitySlider = makeSlider(254, 3004);
        SendMessageW(SunIntensitySlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 2000)); // 0..20
        SendMessageW(SunIntensitySlider, TBM_SETTICFREQ, 200, 0);

        RayleighLabel = CreateWindowExW(0, L"STATIC", L"Rayleigh", WS_CHILD | WS_VISIBLE, 8, 280, SidebarWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        RayleighSlider = makeSlider(296, 3005);
        SendMessageW(RayleighSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 3000)); // 0..3
        SendMessageW(RayleighSlider, TBM_SETTICFREQ, 500, 0);

        MieLabel = CreateWindowExW(0, L"STATIC", L"Mie", WS_CHILD | WS_VISIBLE, 8, 322, SidebarWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        MieSlider = makeSlider(338, 3006);
        SendMessageW(MieSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 3000)); // 0..3
        SendMessageW(MieSlider, TBM_SETTICFREQ, 500, 0);

        MieGLabel = CreateWindowExW(0, L"STATIC", L"Mie G", WS_CHILD | WS_VISIBLE, 8, 364, SidebarWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        MieGSlider = makeSlider(380, 3007);
        SendMessageW(MieGSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 990)); // 0..0.99
        SendMessageW(MieGSlider, TBM_SETTICFREQ, 165, 0);

        AtmoHeightLabel = CreateWindowExW(0, L"STATIC", L"Atmosphere Height", WS_CHILD | WS_VISIBLE, 8, 406, SidebarWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        AtmoHeightSlider = makeSlider(422, 3008);
        SendMessageW(AtmoHeightSlider, TBM_SETRANGE, TRUE, MAKELPARAM(50, 3000)); // 0.5..30.0
        SendMessageW(AtmoHeightSlider, TBM_SETTICFREQ, 500, 0);

        SkyLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 8, 452, SidebarWidthPx - 16, 54, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);

        UpdateSkyUI();
    }

    // Viewport child window (swapchain host)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &FEngine::ViewportWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"DX12ViewportWindowClass";
        RegisterClassExW(&wc);

        RECT rc{};
        GetClientRect(Window.GetHwnd(), &rc);
        const int vw = std::max(1, (int)(rc.right - rc.left) - SidebarWidthPx);
        const int vh = std::max(1, (int)(rc.bottom - rc.top));

        ViewportHwnd = CreateWindowExW(
            0,
            wc.lpszClassName,
            nullptr,
            WS_CHILD | WS_VISIBLE,
            SidebarWidthPx,
            0,
            vw,
            vh,
            Window.GetHwnd(),
            (HMENU)1002,
            wc.hInstance,
            this);
        SetFocus(ViewportHwnd);

        // Raw mouse input for UE-like RMB look (stable relative deltas, independent of cursor position).
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01;
        rid.usUsage = 0x02; // mouse
        rid.dwFlags = RIDEV_INPUTSINK;
        rid.hwndTarget = ViewportHwnd;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));

        // Match brush color used by WM_CTLCOLOR*.
        SetClassLongPtrW(Window.GetHwnd(), GCLP_HBRBACKGROUND, (LONG_PTR)GetStockObject(DC_BRUSH));
    }

    // Bottom panel UI (textures + materials)
    {
        RECT rc{};
        GetClientRect(Window.GetHwnd(), &rc);
        const int clientW = (int)(rc.right - rc.left);
        const int clientH = (int)(rc.bottom - rc.top);

        BottomPanel = CreateWindowExW(
            0, L"STATIC", nullptr,
            WS_CHILD | WS_VISIBLE,
            0, clientH - BottomPanelHeightPx,
            clientW, BottomPanelHeightPx,
            Window.GetHwnd(), (HMENU)2000, GetModuleHandleW(nullptr), nullptr);

        const int padding = 8;
        const int colW = (clientW - padding * 4) / 3;
        const int listH = BottomPanelHeightPx - padding * 2 - 26;

        TextureList = CreateWindowExW(
            0, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
            padding, padding,
            colW, listH,
            BottomPanel, (HMENU)2001, GetModuleHandleW(nullptr), nullptr);

        TexturePreview = CreateWindowExW(
            0, L"STATIC", nullptr,
            WS_CHILD | WS_VISIBLE | SS_BITMAP | SS_CENTERIMAGE,
            padding + colW + padding, padding,
            colW, listH,
            BottomPanel, (HMENU)2002, GetModuleHandleW(nullptr), nullptr);

        MaterialList = CreateWindowExW(
            0, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
            padding + (colW + padding) * 2, padding,
            colW, listH,
            BottomPanel, (HMENU)2003, GetModuleHandleW(nullptr), nullptr);

        NewMaterialBtn = CreateWindowExW(
            0, L"BUTTON", L"New Material",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            padding + (colW + padding) * 2, padding + listH + 4,
            colW, 22,
            BottomPanel, (HMENU)2004, GetModuleHandleW(nullptr), nullptr);

        TonemapCheckbox = CreateWindowExW(
            0, L"BUTTON", L"Tonemap",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            padding + colW + padding, padding + listH + 4,
            colW, 22,
            BottomPanel, (HMENU)2005, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(TonemapCheckbox, BM_SETCHECK, bEnableTonemap ? BST_CHECKED : BST_UNCHECKED, 0);

        SetWindowLongPtrW(MaterialList, GWLP_USERDATA, (LONG_PTR)this);
        MaterialOldProc = (WNDPROC)SetWindowLongPtrW(MaterialList, GWLP_WNDPROC, (LONG_PTR)&FEngine::MaterialWndProc);

        DragAcceptFiles(BottomPanel, TRUE);

        SetWindowLongPtrW(BottomPanel, GWLP_USERDATA, (LONG_PTR)this);
        BottomOldProc = (WNDPROC)SetWindowLongPtrW(BottomPanel, GWLP_WNDPROC, (LONG_PTR)&FEngine::BottomWndProc);
    }

    // 初始布局，确保视口不与底栏重叠。
    // Make sure viewport does not overlap bottom panel at startup.
    LayoutUI();

    // 创建一个默认球体对象以便演示。
    // Create one default sphere in scene
    {
        FSceneObject obj{};
        obj.Type = FSceneObject::EType::Sphere;
        obj.Position = { 0.0f, 0.0f, 0.0f };
        obj.Radius = 0.75f;
        obj.Albedo = { 0.85f, 0.15f, 0.10f };
        obj.Metallic = 0.0f;
        obj.Roughness = 0.35f;
        obj.MaterialIndex = -1;
        obj.MaterialSRVBase = Renderer.AllocateMaterialSRVBlock();
        Renderer.UpdateMaterialSRVBlock(obj.MaterialSRVBase, 0, 1, 2, 3, 4);
        Objects.push_back(obj);
        SelectedIndex = 0;
    }

    {
        FSceneObject obj{};
        obj.Type = FSceneObject::EType::RenderDocRock;
        obj.Position = { 1.4f, 0.39f, 0.0f };
        obj.Scale = { 0.35f, 0.35f, 0.35f };
        obj.Radius = 2.05f;
        obj.Albedo = { 0.45f, 0.45f, 0.45f };
        obj.Metallic = 0.0f;
        obj.Roughness = 0.82f;
        obj.MaterialIndex = -1;
        obj.MaterialSRVBase = 0;
        Objects.push_back(obj);
    }

    {
        RECT rc{};
        GetClientRect(ViewportHwnd, &rc);
        const uint32 vw = (uint32)std::max(1L, rc.right - rc.left);
        const uint32 vh = (uint32)std::max(1L, rc.bottom - rc.top);
        RHI.Init(ViewportHwnd, vw, vh);
    }
    Renderer.Init(RHI);

    // 初始化后同步 HWRT 可用性与 UI。
    // Update HWRT UI availability after renderer init.
    if (LumenHWRTCheckbox)
    {
        const bool dxrSupported = Renderer.IsRaytracingSupported();
        const bool hwrtReady = Renderer.IsHWRTGIReady();

        std::wstring label = L"HWRT GI (Probes)";
        bool enable = dxrSupported && hwrtReady;

        if (!dxrSupported)
        {
            label = L"HWRT GI (DXR Unsupported)";
        }
        else if (!hwrtReady)
        {
            label = L"HWRT GI (Init Failed)";
            const std::wstring& err = Renderer.GetHWRTGIInitError();
            if (err.find(L"DXC not found") != std::wstring::npos)
                label = L"HWRT GI (Need DXC)";
        }

        EnableWindow(LumenHWRTCheckbox, enable ? TRUE : FALSE);
        SetWindowTextW(LumenHWRTCheckbox, label.c_str());

        if (!enable)
        {
            bEnableLumenHWRT = false;
            SendMessageW(LumenHWRTCheckbox, BM_SETCHECK, BST_UNCHECKED, 0);
        }
    }

    // 主循环：消息泵 + 帧更新与渲染。
    MSG msg{};
    while (bRunning)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) bRunning = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!bRunning) break;

        // 帧时间与更新逻辑。
        const uint64 nowMs = GetTickCount64();
        const float dt = float(nowMs - PrevTickMs) / 1000.0f;
        PrevTickMs = nowMs;
        Tick(dt);

        // 计算光源方向并传入渲染器。
        const float t = float(nowMs - StartTickMs) / 1000.0f;
        DirectX::XMFLOAT3 lightDirWs{};
        {
            using namespace DirectX;
            const float cy = ::cosf(SunYaw);
            const float sy = ::sinf(SunYaw);
            const float cp = ::cosf(SunPitch);
            const float sp = ::sinf(SunPitch);
            const XMVECTOR dir = XMVector3Normalize(XMVectorSet(sy * cp, sp, cy * cp, 0.0f));
            XMStoreFloat3(&lightDirWs, dir);
        }

        // 处理材质拖拽投放到场景。
        // Handle material drop onto scene (from material list).
        if (bPendingMaterialDrop && PendingMaterialIndex >= 0 && PendingMaterialIndex < (int)Materials.size())
        {
            const uint32 w = RHI.GetWidth();
            const uint32 h = RHI.GetHeight();
            const int mx = PendingDropMouseX;
            const int my = PendingDropMouseY;
            if (mx >= 0 && my >= 0 && mx < (int)w && my < (int)h)
            {
                using namespace DirectX;
                const float aspect = (h == 0) ? 1.0f : (float)w / (float)h;
                const XMMATRIX view = Camera.GetViewMatrix();
                const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, 0.1f, 100.0f);
                const XMMATRIX invVP = XMMatrixInverse(nullptr, view * proj);
                XMVECTOR rayOrigin{};
                const XMVECTOR rayDir = MakeRayDirFromScreen(mx, my, w, h, invVP, rayOrigin);

                float bestT = 1e30f;
                int bestIdx = -1;
                for (int i = 0; i < (int)Objects.size(); ++i)
                {
                    float tHit = 0.0f;
                    const XMVECTOR c = XMLoadFloat3(&Objects[i].Position);
                    if (RaySphereIntersect(rayOrigin, rayDir, c, GetObjectPickRadius(Objects[i]), tHit))
                    {
                        if (tHit < bestT) { bestT = tHit; bestIdx = i; }
                    }
                }

                if (bestIdx >= 0)
                {
                    // 将材质参数应用到命中的物体。
                    const auto& mat = Materials[PendingMaterialIndex];
                    Objects[bestIdx].Albedo = mat.Albedo;
                    Objects[bestIdx].Metallic = mat.Metallic;
                    Objects[bestIdx].Roughness = mat.Roughness;
                    Objects[bestIdx].MaterialIndex = PendingMaterialIndex;
                    Objects[bestIdx].MaterialSRVBase = mat.SRVBase;
                    Objects[bestIdx].UseAlbedoTex = (mat.AlbedoTexIndex >= 0) ? 1.0f : 0.0f;
                    Objects[bestIdx].UseNormalTex = (mat.NormalTexIndex >= 0) ? 1.0f : 0.0f;
                    Objects[bestIdx].UseRoughnessTex = (mat.RoughnessTexIndex >= 0) ? 1.0f : 0.0f;
                    Objects[bestIdx].UseMetallicTex = (mat.MetallicTexIndex >= 0) ? 1.0f : 0.0f;
                    Objects[bestIdx].UseAOTex = (mat.AOTexIndex >= 0) ? 1.0f : 0.0f;
                }
            }
            bPendingMaterialDrop = false;
            PendingMaterialIndex = -1;
        }

        // 调用渲染器绘制一帧。
        Renderer.Render(
            RHI,
            Camera,
            t,
            RenderPath,
            bEnableLumen,
            bEnableLumenSWRT,
            bEnableLumenHWRT,
            Objects,
            SelectedIndex,
            GizmoMode == EGizmoMode::Scale,
            lightDirWs,
            bEnableTonemap,
            SunIntensity,
            SkySettings,
            0,
            bPlacingFromSidebar ? &PreviewPos : nullptr,
            bPlacingFromSidebar ? PaletteType : FSceneObject::EType::Sphere);

        // 更新窗口标题以展示选中信息。
        // Window title shows selected object position (minimal UE-like feedback)
        const wchar_t* pathName = (RenderPath == FSimpleSceneRenderer::ERenderPath::Deferred) ? L"Deferred" : L"Forward";
        if (SelectedIndex >= 0 && SelectedIndex < (int)Objects.size())
        {
            wchar_t title[256];
            const auto& p = Objects[SelectedIndex].Position;
            const float r = Objects[SelectedIndex].Roughness;
            const float m = Objects[SelectedIndex].Metallic;
            swprintf_s(title, L"%s  |  Path: %s  |  Pos: X=%.2f Y=%.2f Z=%.2f  |  Rough=%.3f Metal=%.3f  |  CamSpeed=%.2f Look=%.4f",
                       baseTitle, pathName, p.x, p.y, p.z, r, m, CameraMoveSpeed, CameraLookSensitivity);
            SetWindowTextW(Window.GetHwnd(), title);
        }
        else
        {
            wchar_t title[256];
            swprintf_s(title, L"%s  |  Path: %s", baseTitle, pathName);
            SetWindowTextW(Window.GetHwnd(), title);
        }
    }

    // 退出前等待 GPU 完成并释放资源。
    RHI.WaitForGPU();
    Renderer.Shutdown();
    RHI.Shutdown();

    // 清理预览位图与 COM。
    if (TexturePreview)
        SendMessageW(TexturePreview, STM_SETIMAGE, IMAGE_BITMAP, 0);
    for (auto& t : Textures)
    {
        if (t.Preview) DeleteObject(t.Preview);
        t.Preview = nullptr;
    }
    CoUninitialize();
}
