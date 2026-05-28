#include "Engine/Engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <sstream>
#include <utility>
#include <vector>

#include <wincodec.h>
#include <shellapi.h>
#include <commdlg.h>
#include <commctrl.h>

#include "Core/Diagnostics.h"
#include "Editor/ObjLoader.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "ImGuizmo.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
constexpr int IDC_CONTENT_LIST = 5001;
constexpr int IDC_IMPORT_OBJ = 5002;
constexpr int IDC_PLACE_ASSET = 5003;
constexpr int IDC_NEW_LEVEL = 5004;
constexpr int IDC_OPEN_LEVEL = 5005;
constexpr int IDC_SAVE_LEVEL = 5006;
constexpr int IDC_TOOLBAR_NEW_LEVEL = 5007;
constexpr int IDC_TOOLBAR_OPEN_LEVEL = 5008;
constexpr int IDC_TOOLBAR_SAVE_LEVEL = 5009;
constexpr int IDC_TOOLBAR_IMPORT_OBJ = 5010;
constexpr int IDC_TOOLBAR_PLACE = 5011;
constexpr int IDC_CONTENT_FOLDERS = 5012;
constexpr int IDC_TOOLBAR_SETTINGS = 5013;
constexpr int IDC_ACTOR_SEARCH = 5014;
constexpr int IDC_CONTENT_DRAWER_TOGGLE = 5015;
constexpr int IDC_PLACE_ACTORS_TOGGLE = 5016;
constexpr int IDC_RENDER_TONEMAP_OPERATOR = 5017;
constexpr int IDC_OUTLINER_LIST = 5101;
constexpr int IDC_APPLY_DETAILS = 5201;
constexpr int IDC_MATERIAL_SHADING_MODE = 4100;
constexpr int IDC_MATERIAL_COLOR = 4101;
constexpr int IDC_MATERIAL_APPLY = 4102;
constexpr int IDC_MATERIAL_PARAM_A = 4103;
constexpr int IDC_MATERIAL_PARAM_B = 4104;
constexpr int IDC_MATERIAL_TEX0 = 4105;
constexpr int IDC_MATERIAL_TEX1 = 4106;
constexpr int IDC_MATERIAL_TEX2 = 4107;
constexpr int IDC_MATERIAL_TEX3 = 4108;
constexpr int IDC_MATERIAL_TEX4 = 4109;
constexpr int IDC_MATERIAL_LABEL_SHADING = 4110;
constexpr int IDC_MATERIAL_LABEL_COLOR = 4111;
constexpr int IDC_MATERIAL_LABEL_PARAM_A = 4113;
constexpr int IDC_MATERIAL_LABEL_PARAM_B = 4114;
constexpr int IDC_MATERIAL_LABEL_TEX0 = 4115;
constexpr int IDC_MATERIAL_LABEL_TEX1 = 4116;
constexpr int IDC_MATERIAL_LABEL_TEX2 = 4117;
constexpr int IDC_MATERIAL_LABEL_TEX3 = 4118;
constexpr int IDC_MATERIAL_LABEL_TEX4 = 4119;

constexpr COLORREF kEditorBackground = RGB(20, 20, 20);
constexpr COLORREF kEditorPanel = RGB(31, 31, 31);
constexpr COLORREF kEditorHeader = RGB(43, 43, 43);
constexpr COLORREF kEditorList = RGB(24, 24, 24);
constexpr COLORREF kEditorEdit = RGB(46, 46, 46);
constexpr COLORREF kEditorText = RGB(226, 226, 226);
constexpr wchar_t kDefaultMaterialAssetPath[] = L"Materials/Default/default_pbr.material.json";
constexpr wchar_t kDefaultStartupLevelPath[] = L"Levels/default_renderdoc_scene.level.json";

int TonemapOperatorToComboIndex(ETonemapOperator tonemapOperator)
{
    switch (tonemapOperator)
    {
    case ETonemapOperator::AgX: return 1;
    case ETonemapOperator::ACES: return 2;
    default: return 0;
    }
}

ETonemapOperator TonemapOperatorFromComboIndex(int index)
{
    switch (index)
    {
    case 1: return ETonemapOperator::AgX;
    case 2: return ETonemapOperator::ACES;
    default: return ETonemapOperator::Reinhard;
    }
}

std::wstring ReadEnvWide(const wchar_t* name)
{
    wchar_t buffer[2048]{};
    const DWORD len = GetEnvironmentVariableW(name, buffer, (DWORD)_countof(buffer));
    if (len == 0 || len >= _countof(buffer))
        return {};
    return std::wstring(buffer, len);
}

std::filesystem::path MakeUniquePath(const std::filesystem::path& directory, const std::filesystem::path& fileName)
{
    std::filesystem::path candidate = directory / fileName.filename();
    if (!std::filesystem::exists(candidate))
        return candidate;

    const std::wstring stem = fileName.stem().wstring();
    const std::wstring ext = fileName.extension().wstring();
    for (int i = 1; i < 10000; ++i)
    {
        candidate = directory / (stem + L"_" + std::to_wstring(i) + ext);
        if (!std::filesystem::exists(candidate))
            return candidate;
    }
    return directory / (stem + L"_imported" + ext);
}

bool IsPathUnder(const std::filesystem::path& root, const std::filesystem::path& path)
{
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(path, root, ec);
    if (ec || rel.empty())
        return false;
    const auto first = *rel.begin();
    return first.wstring() != L"..";
}

std::wstring DisplayContentAsset(const editor::FAssetRecord& asset)
{
    return asset.Name + L"    " + asset.RelativePath;
}

HMENU MenuHandle(int id)
{
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

FSceneObject::EType PaletteTypeFromListIndex(int index)
{
    switch (index)
    {
    case 0: return FSceneObject::EType::Sphere;
    case 1: return FSceneObject::EType::Box;
    case 2: return FSceneObject::EType::Cone;
    case 3: return FSceneObject::EType::SunLight;
    case 4: return FSceneObject::EType::SkyAtmosphere;
    case 5: return FSceneObject::EType::RenderDocRock;
    default: return FSceneObject::EType::Sphere;
    }
}

std::wstring ToLowerCopy(std::wstring text)
{
    for (wchar_t& c : text)
        c = (wchar_t)::towlower(c);
    return text;
}

struct FMaterialSchema
{
    EMaterialShadingMode Mode;
    const wchar_t* Name;
    const wchar_t* ColorLabel;
    const wchar_t* ParamALabel;
    const wchar_t* ParamBLabel;
    bool HasParamB;
    const wchar_t* TextureLabels[5];
    int TextureCount;
};

constexpr FMaterialSchema kMaterialSchemas[] = {
    { EMaterialShadingMode::PbrLit, L"PbrLit", L"Base Color:", L"Roughness:", L"Metallic:", true,
        { L"BaseColor Tex:", L"Normal Tex:", L"Rough Tex:", L"Metal Tex:", L"AO Tex:" }, 5 },
    { EMaterialShadingMode::Unlit, L"Unlit", L"Color:", L"Intensity:", L"", false,
        { L"Color Tex:", L"", L"", L"", L"" }, 1 },
    { EMaterialShadingMode::Rdr2Rock, L"Rdr2Rock", L"Base Color:", L"Roughness:", L"Normal Str:", true,
        { L"BaseColor Tex:", L"Normal Tex:", L"Mask A Tex:", L"Mask B Tex:", L"Aux Tex:" }, 5 },
    { EMaterialShadingMode::Rdr2Foliage, L"Rdr2Foliage", L"Base Color:", L"Roughness:", L"Normal Str:", true,
        { L"BaseColor+Alpha:", L"Normal Tex:", L"Mask Tex:", L"Detail Tex:", L"Aux Tex:" }, 5 },
};

const FMaterialSchema& GetMaterialSchema(EMaterialShadingMode mode)
{
    for (const FMaterialSchema& schema : kMaterialSchemas)
    {
        if (schema.Mode == mode)
            return schema;
    }
    return kMaterialSchemas[0];
}

EMaterialShadingMode ParseMaterialShadingMode(const std::wstring& text)
{
    const std::wstring lower = ToLowerCopy(text);
    for (const FMaterialSchema& schema : kMaterialSchemas)
    {
        if (lower == ToLowerCopy(schema.Name))
            return schema.Mode;
    }
    return EMaterialShadingMode::PbrLit;
}

std::wstring MaterialShadingModeName(EMaterialShadingMode mode)
{
    return GetMaterialSchema(mode).Name;
}

int MaterialShadingModeComboIndex(EMaterialShadingMode mode)
{
    for (int i = 0; i < (int)(sizeof(kMaterialSchemas) / sizeof(kMaterialSchemas[0])); ++i)
    {
        if (kMaterialSchemas[i].Mode == mode)
            return i;
    }
    return 0;
}

EMaterialShadingMode MaterialShadingModeFromComboIndex(int index)
{
    if (index >= 0 && index < (int)(sizeof(kMaterialSchemas) / sizeof(kMaterialSchemas[0])))
        return kMaterialSchemas[index].Mode;
    return EMaterialShadingMode::PbrLit;
}
}

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
    return (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".tga" || ext == L".dds");
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

static int PickEditorGizmoAxis2D(
    int mouseX,
    int mouseY,
    const DirectX::XMVECTOR& center,
    const DirectX::XMMATRIX& viewProj,
    uint32 width,
    uint32 height,
    bool& outNearGizmo)
{
    using namespace DirectX;
    outNearGizmo = false;

    float x0 = 0.0f;
    float y0 = 0.0f;
    if (!ProjectToScreen(center, viewProj, width, height, x0, y0))
        return 0;

    constexpr float kPivotPickPx = 22.0f;
    constexpr float kAxisPickPx = 16.0f;
    constexpr float kAxisScreenLenPx = 120.0f;
    constexpr float kProbeWorldLen = 1.0f;

    const float pivotDx = float(mouseX) - x0;
    const float pivotDy = float(mouseY) - y0;
    if ((pivotDx * pivotDx + pivotDy * pivotDy) <= (kPivotPickPx * kPivotPickPx))
        outNearGizmo = true;

    int bestAxis = 0;
    float bestDist = 1e9f;
    auto testAxis = [&](int axis, const XMVECTOR& worldOffset)
    {
        float xa = 0.0f;
        float ya = 0.0f;
        if (!ProjectToScreen(XMVectorAdd(center, worldOffset), viewProj, width, height, xa, ya))
            return;

        float vx = xa - x0;
        float vy = ya - y0;
        const float len = std::sqrtf(vx * vx + vy * vy);
        if (len < 1e-3f)
            return;

        vx /= len;
        vy /= len;
        const float x1 = x0 + vx * kAxisScreenLenPx;
        const float y1 = y0 + vy * kAxisScreenLenPx;
        const float dist = DistancePointToSegment2D(float(mouseX), float(mouseY), x0, y0, x1, y1);
        if (dist < bestDist)
        {
            bestDist = dist;
            bestAxis = axis;
        }
    };

    testAxis(1, XMVectorSet(kProbeWorldLen, 0.0f, 0.0f, 0.0f));
    testAxis(2, XMVectorSet(0.0f, kProbeWorldLen, 0.0f, 0.0f));
    testAxis(3, XMVectorSet(0.0f, 0.0f, kProbeWorldLen, 0.0f));
    if (bestDist <= kAxisPickPx)
    {
        outNearGizmo = true;
        return bestAxis;
    }
    return 0;
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
            if (sel >= 0 && sel < (int)engine->PaletteListTypes.size())
                engine->PaletteType = engine->PaletteListTypes[(size_t)sel];
            else
                engine->PaletteType = PaletteTypeFromListIndex(sel);
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

LRESULT CALLBACK FEngine::SidebarSearchWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* engine = reinterpret_cast<FEngine*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!engine || !engine->SidebarSearchOldProc)
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    const LRESULT result = CallWindowProcW(engine->SidebarSearchOldProc, hwnd, msg, wParam, lParam);
    if (msg == WM_SETTEXT || msg == WM_CHAR || msg == WM_KEYUP || msg == WM_PASTE || msg == WM_CUT || msg == WM_CLEAR)
    {
        wchar_t buf[128]{};
        GetWindowTextW(hwnd, buf, (int)_countof(buf));
        if (engine->ActorPaletteFilter != buf)
        {
            engine->ActorPaletteFilter = buf;
            engine->RefreshActorPalette();
        }
    }
    return result;
}

void FEngine::SyncViewportBackbufferSize()
{
    if (!ViewportHwnd || !RHI.GetDevice())
        return;

    RECT rc{};
    GetClientRect(ViewportHwnd, &rc);
    const uint32 width = (uint32)std::max(1L, rc.right - rc.left);
    const uint32 height = (uint32)std::max(1L, rc.bottom - rc.top);
    if (width == RHI.GetWidth() && height == RHI.GetHeight())
    {
        bForceViewportBackbufferResize = false;
        PendingViewportWidth = 0;
        PendingViewportHeight = 0;
        PendingViewportResizeTickMs = 0;
        return;
    }

    const uint64 nowMs = GetTickCount64();
    if (bForceViewportBackbufferResize)
    {
        bForceViewportBackbufferResize = false;
        PendingViewportWidth = width;
        PendingViewportHeight = height;
        PendingViewportResizeTickMs = nowMs;
        LastViewportResizeTickMs = nowMs;
        RHI.Resize(width, height);
        if (width == RHI.GetWidth() && height == RHI.GetHeight())
        {
            PendingViewportWidth = 0;
            PendingViewportHeight = 0;
            PendingViewportResizeTickMs = 0;
        }
        return;
    }

    if (width != PendingViewportWidth || height != PendingViewportHeight)
    {
        PendingViewportWidth = width;
        PendingViewportHeight = height;
        PendingViewportResizeTickMs = nowMs;
        return;
    }

    if (bWindowSizeMoveActive)
        return;
    if (nowMs - PendingViewportResizeTickMs < ViewportResizeDebounceMs)
        return;
    if (nowMs - LastViewportResizeTickMs < ViewportResizeRetryMs)
        return;

    LastViewportResizeTickMs = nowMs;
    RHI.Resize(width, height);
    if (width == RHI.GetWidth() && height == RHI.GetHeight())
    {
        PendingViewportWidth = 0;
        PendingViewportHeight = 0;
        PendingViewportResizeTickMs = 0;
    }
}

void FEngine::RequestImmediateViewportBackbufferResize()
{
    bForceViewportBackbufferResize = true;
    PendingViewportWidth = 0;
    PendingViewportHeight = 0;
    PendingViewportResizeTickMs = 0;
    LastViewportResizeTickMs = 0;
}

void FEngine::ToggleMaximizedWindow()
{
    HWND mainHwnd = Window.GetHwnd();
    if (!mainHwnd)
        return;
    ShowWindow(mainHwnd, IsZoomed(mainHwnd) ? SW_RESTORE : SW_MAXIMIZE);
    RequestImmediateViewportBackbufferResize();
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

    if (msg == WM_ERASEBKGND)
    {
        RECT fillRc{};
        GetClientRect(hwnd, &fillRc);
        FillRect((HDC)wParam, &fillRc, engine->UIPanelBrush ? engine->UIPanelBrush : (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }

    if (msg == WM_CTLCOLORLISTBOX || msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORBTN || msg == WM_CTLCOLORSTATIC)
        return engine->ApplyEditorControlColors((HWND)lParam, (HDC)wParam, msg);

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
        if ((LOWORD(wParam) == IDC_MATERIAL_SHADING_MODE ||
             LOWORD(wParam) == IDC_MATERIAL_TEX0 ||
             LOWORD(wParam) == IDC_MATERIAL_TEX1 ||
             LOWORD(wParam) == IDC_MATERIAL_TEX2 ||
             LOWORD(wParam) == IDC_MATERIAL_TEX3 ||
             LOWORD(wParam) == IDC_MATERIAL_TEX4) &&
            HIWORD(wParam) == CBN_SELCHANGE)
        {
            engine->ApplyMaterialEditorChanges();
            engine->UpdateMaterialEditorControls();
            return 0;
        }
        switch (LOWORD(wParam))
        {
        case IDC_MATERIAL_COLOR:
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
        case IDC_MATERIAL_APPLY:
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

LRESULT CALLBACK FEngine::RenderSettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* engine = reinterpret_cast<FEngine*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_NCCREATE:
    {
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        engine = reinterpret_cast<FEngine*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)engine);
        return TRUE;
    }
    case WM_COMMAND:
        if (engine)
            return SendMessageW(engine->Window.GetHwnd(), WM_COMMAND, wParam, lParam);
        break;
    case WM_SIZE:
        if (engine)
            engine->LayoutRenderSettingsDialog();
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        if (engine && engine->RenderSettingsHwnd == hwnd)
            engine->RenderSettingsHwnd = nullptr;
        return 0;
    case WM_ERASEBKGND:
        if (engine)
        {
            RECT fillRc{};
            GetClientRect(hwnd, &fillRc);
            FillRect((HDC)wParam, &fillRc, engine->UIPanelBrush ? engine->UIPanelBrush : (HBRUSH)GetStockObject(BLACK_BRUSH));
            return 1;
        }
        break;
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
        if (engine)
            return engine->ApplyEditorControlColors((HWND)lParam, (HDC)wParam, msg);
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK FEngine::ToolbarSettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* engine = reinterpret_cast<FEngine*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!engine || !engine->ToolbarSettingsOldProc)
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    if (msg == BM_CLICK || msg == WM_LBUTTONUP || (msg == WM_KEYUP && (wParam == VK_SPACE || wParam == VK_RETURN)))
    {
        const LRESULT result = CallWindowProcW(engine->ToolbarSettingsOldProc, hwnd, msg, wParam, lParam);
        engine->OpenRenderSettingsDialog();
        return result;
    }

    return CallWindowProcW(engine->ToolbarSettingsOldProc, hwnd, msg, wParam, lParam);
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
    if (msg == WM_SYSKEYDOWN && wParam == VK_RETURN && (HIWORD(lParam) & KF_ALTDOWN))
    {
        ToggleMaximizedWindow();
        return 0;
    }

    if (bImGuiInitialized)
    {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
        ImGuiIO& io = ImGui::GetIO();
        const bool mouseMessage =
            msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
            msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_MOUSEWHEEL ||
            msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP;
        const bool keyMessage =
            msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR ||
            msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
        if ((mouseMessage && io.WantCaptureMouse && !bImGuiViewportHovered) ||
            (keyMessage && io.WantCaptureKeyboard && !bImGuiViewportFocused))
            return 0;
    }

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
                MarkLevelDirty();
                RefreshOutliner();
                RefreshDetailsPanel();
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
        if ((HWND)lParam == TonemapOperatorCombo && HIWORD(wParam) == CBN_SELCHANGE)
        {
            const int sel = (int)SendMessageW(TonemapOperatorCombo, CB_GETCURSEL, 0, 0);
            TonemapOperator = TonemapOperatorFromComboIndex(sel);
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
            if (sel >= 0 && sel < (int)PaletteListTypes.size())
                PaletteType = PaletteListTypes[(size_t)sel];
            else
                PaletteType = PaletteTypeFromListIndex(sel);
            return 0;
        }
        if ((HWND)lParam == SidebarSearchEdit && HIWORD(wParam) == EN_CHANGE)
        {
            wchar_t buf[128]{};
            GetWindowTextW(SidebarSearchEdit, buf, (int)_countof(buf));
            ActorPaletteFilter = buf;
            RefreshActorPalette();
            return 0;
        }
        if (HIWORD(wParam) == LBN_SELCHANGE && (HWND)lParam == ContentFoldersList)
        {
            const int sel = (int)SendMessageW(ContentFoldersList, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel <= (int)EContentFilter::Levels)
                SelectContentFilter((EContentFilter)sel);
            return 0;
        }
        if ((HWND)lParam == ContentList && HIWORD(wParam) == LBN_DBLCLK)
        {
            PreviewSelectedContentAsset();
            return 0;
        }
        if ((HWND)lParam == OutlinerList && HIWORD(wParam) == LBN_SELCHANGE && !bSuppressOutlinerEvents)
        {
            const int sel = (int)SendMessageW(OutlinerList, LB_GETCURSEL, 0, 0);
            SetSelectedIndex(sel);
            return 0;
        }
        if ((HWND)lParam == OutlinerList && HIWORD(wParam) == LBN_DBLCLK && !bSuppressOutlinerEvents)
        {
            const int sel = (int)SendMessageW(OutlinerList, LB_GETCURSEL, 0, 0);
            SetSelectedIndex(sel);
            FocusViewportOnObject(sel);
            return 0;
        }
        if ((HWND)lParam == ImportObjBtn && HIWORD(wParam) == BN_CLICKED)
        {
            ImportObjFromDialog();
            return 0;
        }
        if ((HWND)lParam == ToolbarImportObjBtn && HIWORD(wParam) == BN_CLICKED)
        {
            ImportObjFromDialog();
            return 0;
        }
        if ((HWND)lParam == PlaceAssetBtn && HIWORD(wParam) == BN_CLICKED)
        {
            PlaceSelectedContentAsset();
            return 0;
        }
        if ((HWND)lParam == ToolbarPlaceBtn && HIWORD(wParam) == BN_CLICKED)
        {
            PlaceSelectedContentAsset();
            return 0;
        }
        if ((HWND)lParam == ToolbarSettingsBtn || LOWORD(wParam) == IDC_TOOLBAR_SETTINGS)
        {
            OpenRenderSettingsDialog();
            return 0;
        }
        if ((HWND)lParam == SidebarToggleBtn && HIWORD(wParam) == BN_CLICKED)
        {
            bPlaceActorsOpen = !bPlaceActorsOpen;
            if (SidebarToggleBtn)
                SetWindowTextW(SidebarToggleBtn, bPlaceActorsOpen ? L"<" : L">");
            LayoutUI();
            return 0;
        }
        if ((HWND)lParam == ContentDrawerToggleBtn && HIWORD(wParam) == BN_CLICKED)
        {
            bContentDrawerOpen = !bContentDrawerOpen;
            if (ContentDrawerToggleBtn)
                SetWindowTextW(ContentDrawerToggleBtn, bContentDrawerOpen ? L"Collapse" : L"Content");
            LayoutUI();
            return 0;
        }
        if ((HWND)lParam == NewLevelBtn && HIWORD(wParam) == BN_CLICKED)
        {
            NewLevel();
            return 0;
        }
        if ((HWND)lParam == ToolbarNewLevelBtn && HIWORD(wParam) == BN_CLICKED)
        {
            NewLevel();
            return 0;
        }
        if ((HWND)lParam == OpenLevelBtn && HIWORD(wParam) == BN_CLICKED)
        {
            OpenLevelFromDialog();
            return 0;
        }
        if ((HWND)lParam == ToolbarOpenLevelBtn && HIWORD(wParam) == BN_CLICKED)
        {
            OpenLevelFromDialog();
            return 0;
        }
        if ((HWND)lParam == SaveLevelBtn && HIWORD(wParam) == BN_CLICKED)
        {
            SaveCurrentLevel(false);
            return 0;
        }
        if ((HWND)lParam == ToolbarSaveLevelBtn && HIWORD(wParam) == BN_CLICKED)
        {
            SaveCurrentLevel(false);
            return 0;
        }
        if ((HWND)lParam == ApplyDetailsBtn && HIWORD(wParam) == BN_CLICKED)
        {
            ApplyDetailsEdits();
            return 0;
        }
        if (HIWORD(wParam) == LBN_SELCHANGE && (HWND)lParam == TextureList)
        {
            // 纹理列表选择改变：更新预览。
            const int sel = (int)SendMessageW(TextureList, LB_GETCURSEL, 0, 0);
            SelectTextureIndex(sel);
            return 0;
        }
        if ((HWND)lParam == NewMaterialBtn && HIWORD(wParam) == BN_CLICKED)
        {
            // 创建新材质并添加到列表。
            // Create a new material, optionally from selected texture average color.
            FMaterialAsset mat{};
            mat.Name = L"Material " + std::to_wstring((int)Materials.size() + 1);
            mat.ShadingMode = EMaterialShadingMode::PbrLit;
            if (SelectedTextureIndex >= 0 && SelectedTextureIndex < (int)Textures.size())
            {
                mat.Albedo = Textures[SelectedTextureIndex].AvgColor;
                mat.AlbedoTexIndex = SelectedTextureIndex;
                mat.AlbedoTexSlot = Textures[SelectedTextureIndex].RendererSlot;
                mat.AlbedoTexPath = Textures[SelectedTextureIndex].RelativePath;
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

            UpdateMaterialRuntimeBindings(mat);
            Materials.push_back(mat);
            SaveMaterialAsset((int)Materials.size() - 1);
            SelectContentFilter(EContentFilter::Materials);
            if (MaterialList)
            {
                SendMessageW(MaterialList, LB_ADDSTRING, 0, (LPARAM)mat.Name.c_str());
                SendMessageW(MaterialList, LB_SETCURSEL, (WPARAM)(Materials.size() - 1), 0);
            }
            PreviewMaterialAsset((int)Materials.size() - 1, true);
            return 0;
        }
        if ((HWND)lParam == TonemapCheckbox && HIWORD(wParam) == BN_CLICKED)
        {
            const LRESULT checked = SendMessageW(TonemapCheckbox, BM_GETCHECK, 0, 0);
            bEnableTonemap = (checked == BST_CHECKED);
            SyncRenderSettingsControls();
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
                PreviewMaterialAsset(sel, true);
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
    case WM_ENTERSIZEMOVE:
        bWindowSizeMoveActive = true;
        return 0;
    case WM_EXITSIZEMOVE:
        bWindowSizeMoveActive = false;
        LayoutUI();
        RequestImmediateViewportBackbufferResize();
        return 0;
    case WM_SYSKEYDOWN:
        if (wParam == VK_RETURN && (HIWORD(lParam) & KF_ALTDOWN))
        {
            ToggleMaximizedWindow();
            return 0;
        }
        break;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
        {
            bForceViewportBackbufferResize = false;
            return 0;
        }
        LayoutUI();
        if (wParam == SIZE_MAXIMIZED || !bWindowSizeMoveActive)
            RequestImmediateViewportBackbufferResize();
        return 0;
    case WM_ERASEBKGND:
    {
        RECT fillRc{};
        GetClientRect(hwnd, &fillRc);
        FillRect((HDC)wParam, &fillRc, UIBackgroundBrush ? UIBackgroundBrush : (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
        return ApplyEditorControlColors((HWND)lParam, (HDC)wParam, msg);
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

    std::filesystem::path source = std::filesystem::path(path);
    auto ext = source.extension().wstring();
    for (auto& c : ext) c = (wchar_t)towlower(c);
    const bool isDds = (ext == L".dds");
    std::filesystem::path finalPath = source;
    if (!ContentRoot.empty() && !IsPathUnder(ContentRoot, source))
    {
        std::error_code ec;
        std::filesystem::create_directories(ContentLayout.Textures, ec);
        finalPath = MakeUniquePath(ContentLayout.Textures, source.filename());
        std::filesystem::copy_file(source, finalPath, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
            finalPath = source;
    }
    const std::wstring relativePath = ContentRoot.empty()
        ? finalPath.filename().wstring()
        : editor::MakeRelativeContentPath(ContentRoot, finalPath);
    if (FindTextureByRelativePath(relativePath) >= 0)
        return;

    // Preview (small) + avg color
    HBITMAP previewBmp = nullptr;
    DirectX::XMFLOAT3 avg{ 1.0f, 1.0f, 1.0f };
    if (!isDds && !LoadImagePreviewWIC(finalPath.wstring(), 128, previewBmp, avg))
    {
        // WIC 不支持 TGA 时走自定义解码并生成预览。
        // WIC might not support TGA; handle TGA preview via full decode
        std::vector<uint8> rgba;
        uint32 w = 0, h = 0;
        if (!LoadImageRGBA8TGA(finalPath.wstring(), rgba, w, h))
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
    bool ok = true;
    if (!isDds)
    {
        ok = LoadImageRGBA8WIC(finalPath.wstring(), 1024, rgba, w, h);
        if (!ok)
            ok = LoadImageRGBA8TGA(finalPath.wstring(), rgba, w, h);
        if (!ok || rgba.empty()) return;
    }

    int slot = 0;
    if (RHI.GetDevice())
        // 上传纹理到渲染器，获取槽位。
        slot = isDds ? Renderer.CreateTextureDDS(RHI, finalPath) : Renderer.CreateTextureRGBA8(RHI, w, h, rgba.data());

    // 写入纹理资产结构。
    FTextureAsset tex{};
    tex.Name = finalPath.stem().wstring();
    tex.RelativePath = relativePath;
    tex.Path = finalPath.wstring();
    tex.Preview = previewBmp;
    tex.AvgColor = avg;
    tex.RendererSlot = slot;
    Textures.push_back(tex);

    // 更新纹理列表 UI。
    if (TextureList)
    {
        const auto name = std::filesystem::path(finalPath).filename().wstring();
        SendMessageW(TextureList, LB_ADDSTRING, 0, (LPARAM)name.c_str());
    }

    // If editor is open, refresh texture combo.
    UpdateMaterialEditorControls();
    RefreshContentBrowser();
}

/**
 * @brief 打开材质编辑器窗口并加载指定材质数据。
 * @param materialIndex 材质索引。
 * @return 无返回值。
 * @note 阶段：编辑器材质编辑阶段。
 */
void FEngine::InitializeEditorContent()
{
    ContentRoot = std::filesystem::path(CONTENT_DIR);
    ContentLayout = editor::MakeContentLayout(ContentRoot);
    std::wstring error;
    EnsureContentLayout(ContentLayout, &error);
    RefreshContentBrowser();
}

void FEngine::RefreshContentBrowser()
{
    if (ContentRoot.empty())
        return;
    ContentAssets = editor::ScanContent(ContentLayout);
    if (ContentFoldersList)
        SendMessageW(ContentFoldersList, LB_SETCURSEL, (WPARAM)static_cast<int>(ContentFilter), 0);
    if (ContentPathLabel)
    {
        const std::wstring text = L"Content / " + ContentFilterDisplayName(ContentFilter);
        SetWindowTextW(ContentPathLabel, text.c_str());
    }
    if (!ContentList)
    {
        UpdateStatusText();
        return;
    }

    SendMessageW(ContentList, LB_RESETCONTENT, 0, 0);
    ContentListAssetIndices.clear();
    for (int i = 0; i < (int)ContentAssets.size(); ++i)
    {
        const editor::FAssetRecord& asset = ContentAssets[(size_t)i];
        if (!DoesAssetPassContentFilter(asset))
            continue;
        ContentListAssetIndices.push_back(i);
        const std::wstring label = DisplayContentAsset(asset);
        SendMessageW(ContentList, LB_ADDSTRING, 0, (LPARAM)label.c_str());
    }
    if (ContentHintLabel)
    {
        std::wstring action = L"Double-click previews. Place/Open runs the active command.";
        if (ContentFilter == EContentFilter::Models)
            action = L"Double-click previews model. Place adds it to the Level.";
        else if (ContentFilter == EContentFilter::Textures)
            action = L"Double-click previews texture. New Material can use the selected texture.";
        else if (ContentFilter == EContentFilter::Materials)
            action = L"Double-click previews and opens the material editor.";
        else if (ContentFilter == EContentFilter::Levels)
            action = L"Double-click opens the Level.";
        const std::wstring hint = std::to_wstring(ContentListAssetIndices.size()) + L" items. " + action;
        SetWindowTextW(ContentHintLabel, hint.c_str());
    }
    UpdateStatusText();
}

void FEngine::RefreshOutliner()
{
    if (!OutlinerList)
        return;
    bSuppressOutlinerEvents = true;
    SendMessageW(OutlinerList, LB_RESETCONTENT, 0, 0);
    for (const FSceneObject& object : Objects)
    {
        std::wstring name = object.Name.empty()
            ? (SceneObjectTypeToString(object.Type) + L" " + std::to_wstring(object.Id))
            : object.Name;
        SendMessageW(OutlinerList, LB_ADDSTRING, 0, (LPARAM)name.c_str());
    }
    if (SelectedIndex >= 0 && SelectedIndex < (int)Objects.size())
        SendMessageW(OutlinerList, LB_SETCURSEL, (WPARAM)SelectedIndex, 0);
    bSuppressOutlinerEvents = false;
    UpdateStatusText();
}

void FEngine::RefreshDetailsPanel()
{
    const bool hasSelection = SelectedIndex >= 0 && SelectedIndex < (int)Objects.size();
    HWND edits[] = {
        DetailNameEdit, DetailPosXEdit, DetailPosYEdit, DetailPosZEdit,
        DetailScaleXEdit, DetailScaleYEdit, DetailScaleZEdit,
        DetailIntensityEdit, DetailSkyEnabledCheckbox, DetailRayleighEdit,
        DetailMieEdit, DetailMieGEdit, DetailAtmosphereHeightEdit,
        ApplyDetailsBtn
    };
    for (HWND h : edits)
        if (h) EnableWindow(h, hasSelection ? TRUE : FALSE);
    HWND sunControls[] = { DetailIntensityLabel, DetailIntensityEdit };
    HWND skyControls[] = {
        DetailSkyEnabledCheckbox, DetailRayleighLabel, DetailRayleighEdit,
        DetailMieLabel, DetailMieEdit, DetailMieGLabel, DetailMieGEdit,
        DetailAtmosphereHeightLabel, DetailAtmosphereHeightEdit
    };
    auto showGroup = [](const HWND* controls, size_t count, bool show)
    {
        for (size_t i = 0; i < count; ++i)
            if (controls[i]) ShowWindow(controls[i], show ? SW_SHOW : SW_HIDE);
    };
    if (!hasSelection)
    {
        if (DetailsLabel) SetWindowTextW(DetailsLabel, L"Details");
        if (DetailNameEdit) SetWindowTextW(DetailNameEdit, L"");
        showGroup(sunControls, _countof(sunControls), false);
        showGroup(skyControls, _countof(skyControls), false);
        if (DetailTransformLabel) ShowWindow(DetailTransformLabel, SW_HIDE);
        if (DetailEnvironmentLabel) ShowWindow(DetailEnvironmentLabel, SW_HIDE);
        LayoutUI();
        return;
    }

    const FSceneObject& object = Objects[(size_t)SelectedIndex];
    const bool isSun = object.Type == FSceneObject::EType::SunLight;
    const bool isSky = object.Type == FSceneObject::EType::SkyAtmosphere;
    showGroup(sunControls, _countof(sunControls), isSun);
    showGroup(skyControls, _countof(skyControls), isSky);
    if (DetailEnvironmentLabel)
        ShowWindow(DetailEnvironmentLabel, (isSun || isSky) ? SW_SHOW : SW_HIDE);
    if (DetailTransformLabel)
        ShowWindow(DetailTransformLabel, SW_SHOW);
    if (DetailsLabel)
    {
        const std::wstring title = L"Details: " + SceneObjectTypeToString(object.Type);
        SetWindowTextW(DetailsLabel, title.c_str());
    }
    auto setFloat = [](HWND h, float v)
    {
        if (!h) return;
        wchar_t buf[64];
        swprintf_s(buf, L"%.3f", v);
        SetWindowTextW(h, buf);
    };
    if (DetailNameEdit) SetWindowTextW(DetailNameEdit, object.Name.c_str());
    setFloat(DetailPosXEdit, object.Position.x);
    setFloat(DetailPosYEdit, object.Position.y);
    setFloat(DetailPosZEdit, object.Position.z);
    setFloat(DetailScaleXEdit, object.Scale.x);
    setFloat(DetailScaleYEdit, object.Scale.y);
    setFloat(DetailScaleZEdit, object.Scale.z);
    if (isSun)
        setFloat(DetailIntensityEdit, object.LightIntensity);
    if (isSky)
    {
        if (DetailSkyEnabledCheckbox)
            SendMessageW(DetailSkyEnabledCheckbox, BM_SETCHECK, object.SkyEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        setFloat(DetailRayleighEdit, object.RayleighScale);
        setFloat(DetailMieEdit, object.MieScale);
        setFloat(DetailMieGEdit, object.MieG);
        setFloat(DetailAtmosphereHeightEdit, object.AtmosphereHeight);
    }
    LayoutUI();
    UpdateStatusText();
}

void FEngine::UpdateStatusText()
{
    if (!StatusLabel)
        return;

    std::wstring text = L"Level: " + (CurrentLevelName.empty() ? L"Untitled" : CurrentLevelName);
    if (bLevelDirty)
        text += L" *";
    text += L"    Objects: " + std::to_wstring(Objects.size());
    text += L"    Assets: " + std::to_wstring(ContentAssets.size());
    if (SelectedIndex >= 0 && SelectedIndex < (int)Objects.size())
        text += L"    Selected: " + Objects[(size_t)SelectedIndex].Name;
    else
        text += L"    Selected: none";
    if (bAssetPreviewActive && !AssetPreviewLabel.empty())
        text += L"    Preview: " + AssetPreviewLabel;
    SetWindowTextW(StatusLabel, text.c_str());
}

void FEngine::ApplyEditorFont(HWND hwnd, bool title)
{
    if (!hwnd)
        return;
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)(title ? UITitleFont : UIFont), TRUE);
}

LRESULT FEngine::ApplyEditorControlColors(HWND control, HDC hdc, UINT msg)
{
    if (!hdc)
        return (LRESULT)(UIPanelBrush ? UIPanelBrush : GetStockObject(DC_BRUSH));

    COLORREF text = kEditorText;
    COLORREF bg = kEditorPanel;
    HBRUSH brush = UIPanelBrush;

    if (msg == WM_CTLCOLOREDIT)
    {
        text = kEditorText;
        bg = kEditorEdit;
        brush = UIEditBrush;
    }
    else if (msg == WM_CTLCOLORLISTBOX)
    {
        bg = kEditorList;
        brush = UIListBrush;
    }
    else if (msg == WM_CTLCOLORBTN)
    {
        bg = kEditorPanel;
        brush = UIPanelBrush;
    }
    else if (control == ToolbarPanel || control == ContentTitleLabel || control == TextureTitleLabel ||
             control == PreviewTitleLabel || control == MaterialTitleLabel || control == RenderSettingsLabel ||
             control == RenderGILabel || control == SunSectionLabel || control == AtmosphereSectionLabel ||
             control == SidebarBasicLabel || control == SidebarRenderDocLabel ||
             control == DetailTransformLabel || control == DetailEnvironmentLabel)
    {
        bg = kEditorHeader;
        brush = UIHeaderBrush;
    }
    else if (control == BottomPanel || control == RightPanel)
    {
        bg = kEditorPanel;
        brush = UIPanelBrush;
    }

    SetTextColor(hdc, text);
    SetBkColor(hdc, bg);
    SetBkMode(hdc, OPAQUE);
    return (LRESULT)(brush ? brush : GetStockObject(DC_BRUSH));
}

HWND FEngine::CreateEditorLabel(const wchar_t* text, HWND parent, bool title)
{
    HWND h = CreateWindowExW(
        0,
        L"STATIC",
        text ? text : L"",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        0, 0, 10, 10,
        parent ? parent : Window.GetHwnd(),
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    ApplyEditorFont(h, title);
    return h;
}

HWND FEngine::CreateEditorButton(const wchar_t* text, HWND parent, int id)
{
    HWND h = CreateWindowExW(
        0,
        L"BUTTON",
        text ? text : L"",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        0, 0, 10, 10,
        parent ? parent : Window.GetHwnd(),
        MenuHandle(id),
        GetModuleHandleW(nullptr),
        nullptr);
    ApplyEditorFont(h);
    return h;
}

HWND FEngine::CreateEditorCheckbox(const wchar_t* text, HWND parent, int id)
{
    HWND h = CreateWindowExW(
        0,
        L"BUTTON",
        text ? text : L"",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_FLAT,
        0, 0, 10, 10,
        parent ? parent : Window.GetHwnd(),
        MenuHandle(id),
        GetModuleHandleW(nullptr),
        nullptr);
    ApplyEditorFont(h);
    return h;
}

HWND FEngine::CreateEditorList(HWND parent, int id)
{
    HWND h = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"LISTBOX",
        nullptr,
        WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
        0, 0, 10, 10,
        parent ? parent : Window.GetHwnd(),
        MenuHandle(id),
        GetModuleHandleW(nullptr),
        nullptr);
    ApplyEditorFont(h);
    return h;
}

void FEngine::RefreshActorPalette()
{
    if (!SidebarList)
        return;

    struct FPaletteEntry
    {
        const wchar_t* Group;
        const wchar_t* Name;
        FSceneObject::EType Type;
    };
    static constexpr FPaletteEntry entries[] = {
        { L"Basic", L"Sphere", FSceneObject::EType::Sphere },
        { L"Basic", L"Box", FSceneObject::EType::Box },
        { L"Basic", L"Cone", FSceneObject::EType::Cone },
        { L"Environment", L"Sun Light", FSceneObject::EType::SunLight },
        { L"Environment", L"Sky Atmosphere", FSceneObject::EType::SkyAtmosphere },
        { L"RenderDoc", L"Rock", FSceneObject::EType::RenderDocRock },
    };

    const std::wstring filter = ToLowerCopy(ActorPaletteFilter);
    PaletteListTypes.clear();
    SendMessageW(SidebarList, LB_RESETCONTENT, 0, 0);

    int selectIndex = -1;
    for (const FPaletteEntry& entry : entries)
    {
        const std::wstring label = std::wstring(entry.Group) + L"    " + entry.Name;
        const std::wstring searchable = ToLowerCopy(std::wstring(entry.Group) + L" " + entry.Name);
        if (!filter.empty() && searchable.find(filter) == std::wstring::npos)
            continue;

        if (entry.Type == PaletteType)
            selectIndex = (int)PaletteListTypes.size();
        PaletteListTypes.push_back(entry.Type);
        SendMessageW(SidebarList, LB_ADDSTRING, 0, (LPARAM)label.c_str());
    }

    if (!PaletteListTypes.empty())
    {
        if (selectIndex < 0)
        {
            selectIndex = 0;
            PaletteType = PaletteListTypes[0];
        }
        SendMessageW(SidebarList, LB_SETCURSEL, (WPARAM)selectIndex, 0);
    }
}

void FEngine::SelectContentFilter(EContentFilter filter)
{
    ContentFilter = filter;
    if (ContentFoldersList)
        SendMessageW(ContentFoldersList, LB_SETCURSEL, (WPARAM)static_cast<int>(ContentFilter), 0);
    RefreshContentBrowser();
}

bool FEngine::DoesAssetPassContentFilter(const editor::FAssetRecord& asset) const
{
    switch (ContentFilter)
    {
    case EContentFilter::Models:
        return asset.Kind == editor::EAssetKind::Model;
    case EContentFilter::Textures:
        return asset.Kind == editor::EAssetKind::Texture;
    case EContentFilter::Materials:
        return asset.Kind == editor::EAssetKind::Material;
    case EContentFilter::Levels:
        return asset.Kind == editor::EAssetKind::Level;
    default:
        return true;
    }
}

std::wstring FEngine::ContentFilterDisplayName(EContentFilter filter) const
{
    switch (filter)
    {
    case EContentFilter::Models: return L"Models";
    case EContentFilter::Textures: return L"Textures";
    case EContentFilter::Materials: return L"Materials";
    case EContentFilter::Levels: return L"Levels";
    default: return L"Content";
    }
}

DirectX::XMFLOAT3 FEngine::DirectionToSunPosition(float yaw, float pitch, float distance)
{
    using namespace DirectX;

    const float cy = ::cosf(yaw);
    const float sy = ::sinf(yaw);
    const float cp = ::cosf(pitch);
    const float sp = ::sinf(pitch);
    const XMVECTOR lightDir = XMVector3Normalize(XMVectorSet(sy * cp, sp, cy * cp, 0.0f));
    const XMVECTOR sunPos = XMVectorScale(lightDir, -distance);
    XMFLOAT3 out{};
    XMStoreFloat3(&out, sunPos);
    return out;
}

void FEngine::SunPositionToAngles(const DirectX::XMFLOAT3& position, float& yaw, float& pitch)
{
    using namespace DirectX;
    XMVECTOR pos = XMLoadFloat3(&position);
    if (XMVectorGetX(XMVector3LengthSq(pos)) < 1e-6f)
    {
        const XMFLOAT3 fallback = DirectionToSunPosition(0.0f, -0.6f, 10.0f);
        pos = XMLoadFloat3(&fallback);
    }
    const XMVECTOR lightDir = XMVectorNegate(XMVector3Normalize(pos));
    const float x = XMVectorGetX(lightDir);
    const float y = XMVectorGetY(lightDir);
    const float z = XMVectorGetZ(lightDir);
    yaw = ::atan2f(x, z);
    pitch = ::asinf(Clamp(y, -1.0f, 1.0f));
}

const FSceneObject* FEngine::FindActiveSunLight() const
{
    for (const FSceneObject& object : Objects)
        if (object.Type == FSceneObject::EType::SunLight)
            return &object;
    return nullptr;
}

FSceneObject* FEngine::FindActiveSunLight()
{
    for (FSceneObject& object : Objects)
        if (object.Type == FSceneObject::EType::SunLight)
            return &object;
    return nullptr;
}

const FSceneObject* FEngine::FindActiveSkyAtmosphere() const
{
    for (const FSceneObject& object : Objects)
        if (object.Type == FSceneObject::EType::SkyAtmosphere)
            return &object;
    return nullptr;
}

DirectX::XMFLOAT3 FEngine::GetActiveLightDirection() const
{
    using namespace DirectX;
    if (const FSceneObject* sun = FindActiveSunLight())
    {
        XMVECTOR pos = XMLoadFloat3(&sun->Position);
        if (XMVectorGetX(XMVector3LengthSq(pos)) > 1e-6f)
        {
            XMFLOAT3 out{};
            XMStoreFloat3(&out, XMVectorNegate(XMVector3Normalize(pos)));
            return out;
        }
    }

    const float cy = ::cosf(SunYaw);
    const float sy = ::sinf(SunYaw);
    const float cp = ::cosf(SunPitch);
    const float sp = ::sinf(SunPitch);
    XMFLOAT3 fallback{};
    XMStoreFloat3(&fallback, XMVector3Normalize(XMVectorSet(sy * cp, sp, cy * cp, 0.0f)));
    return fallback;
}

float FEngine::GetActiveSunIntensity() const
{
    if (const FSceneObject* sun = FindActiveSunLight())
        return sun->LightIntensity;
    return SunIntensity;
}

FSkyAtmosphereSettings FEngine::GetActiveSkySettings() const
{
    FSkyAtmosphereSettings out = SkySettings;
    if (const FSceneObject* sky = FindActiveSkyAtmosphere())
    {
        out.Enable = sky->SkyEnabled;
        out.RayleighScale = sky->RayleighScale;
        out.MieScale = sky->MieScale;
        out.MieG = sky->MieG;
        out.AtmosphereHeight = sky->AtmosphereHeight;
    }
    return out;
}

void FEngine::EnsureDefaultEnvironmentActors()
{
    if (!FindActiveSunLight())
    {
        FSceneObject sun = MakeSceneObject(FSceneObject::EType::SunLight, DirectionToSunPosition(SunYaw, SunPitch, 10.0f));
        sun.LightIntensity = SunIntensity;
        Objects.push_back(sun);
    }
    if (!FindActiveSkyAtmosphere())
    {
        FSceneObject sky = MakeSceneObject(FSceneObject::EType::SkyAtmosphere, { -2.5f, 1.6f, -2.5f });
        sky.SkyEnabled = SkySettings.Enable;
        sky.RayleighScale = SkySettings.RayleighScale;
        sky.MieScale = SkySettings.MieScale;
        sky.MieG = SkySettings.MieG;
        sky.AtmosphereHeight = SkySettings.AtmosphereHeight;
        Objects.push_back(sky);
    }
}

void FEngine::SetSelectedIndex(int index)
{
    SelectedIndex = (index >= 0 && index < (int)Objects.size()) ? index : -1;
    RefreshOutliner();
    RefreshDetailsPanel();
}

void FEngine::MarkLevelDirty()
{
    bLevelDirty = true;
    UpdateStatusText();
}

void FEngine::UpdateWindowTitle(const wchar_t* baseTitle, const wchar_t* pathName)
{
    const std::wstring levelName = CurrentLevelName.empty() ? L"Untitled" : CurrentLevelName;
    const wchar_t* dirty = bLevelDirty ? L"*" : L"";
    if (SelectedIndex >= 0 && SelectedIndex < (int)Objects.size())
    {
        wchar_t title[384];
        const auto& p = Objects[(size_t)SelectedIndex].Position;
        swprintf_s(title, L"%s%s  |  Level: %s  |  Path: %s  |  Selected: %s  X=%.2f Y=%.2f Z=%.2f",
                   baseTitle, dirty, levelName.c_str(), pathName, Objects[(size_t)SelectedIndex].Name.c_str(), p.x, p.y, p.z);
        SetWindowTextW(Window.GetHwnd(), title);
    }
    else
    {
        wchar_t title[256];
        swprintf_s(title, L"%s%s  |  Level: %s  |  Path: %s", baseTitle, dirty, levelName.c_str(), pathName);
        SetWindowTextW(Window.GetHwnd(), title);
    }
}

void FEngine::NewLevel()
{
    ClearAssetPreview();
    SetPreviewBitmap(nullptr, false);
    Objects.clear();
    StaticMeshByAsset.clear();
    StaticMeshRadiusByAsset.clear();
    NextObjectId = 1;
    CurrentLevelPath.clear();
    CurrentLevelName = L"Untitled";
    EnsureDefaultEnvironmentActors();
    bLevelDirty = false;
    SetSelectedIndex(-1);
}

void FEngine::SaveCurrentLevel(bool saveAs)
{
    std::filesystem::path path = CurrentLevelPath;
    if (saveAs || path.empty())
    {
        wchar_t fileName[MAX_PATH] = L"Untitled.level.json";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = Window.GetHwnd();
        ofn.lpstrFilter = L"Level JSON (*.level.json)\0*.level.json\0All Files\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        const std::wstring initialDir = ContentLayout.Levels.wstring();
        ofn.lpstrInitialDir = initialDir.c_str();
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        ofn.lpstrDefExt = L"json";
        if (!GetSaveFileNameW(&ofn))
            return;
        path = fileName;
    }

    EnsureConcreteMaterialsForRenderableObjects();
    editor::FLevelFile level = BuildLevelFile();
    std::wstring error;
    if (editor::SaveLevelFile(path, level, &error))
    {
        CurrentLevelPath = path;
        CurrentLevelName = path.stem().wstring();
        if (CurrentLevelName.size() > 6 && CurrentLevelName.ends_with(L".level"))
            CurrentLevelName.resize(CurrentLevelName.size() - 6);
        bLevelDirty = false;
        RefreshContentBrowser();
    }
}

void FEngine::OpenLevelFromDialog()
{
    wchar_t fileName[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = Window.GetHwnd();
    ofn.lpstrFilter = L"Level JSON (*.level.json)\0*.level.json\0All Files\0*.*\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    const std::wstring initialDir = ContentLayout.Levels.wstring();
    ofn.lpstrInitialDir = initialDir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn))
        LoadLevelFromPath(fileName);
}

void FEngine::LoadLevelFromPath(const std::filesystem::path& path)
{
    editor::FLevelFile level{};
    std::wstring error;
    if (!editor::LoadLevelFile(path, level, &error))
        return;
    ApplyLevelFile(level, path);
}

void FEngine::ImportObjFromDialog()
{
    wchar_t fileName[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = Window.GetHwnd();
    ofn.lpstrFilter = L"OBJ Model (*.obj)\0*.obj\0All Files\0*.*\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn))
        return;

    const std::filesystem::path source = fileName;
    std::filesystem::path finalPath = source;
    if (!IsPathUnder(ContentRoot, source))
    {
        std::error_code ec;
        std::filesystem::create_directories(ContentLayout.Models, ec);
        finalPath = MakeUniquePath(ContentLayout.Models, source.filename());
        std::filesystem::copy_file(source, finalPath, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
            return;
    }
    SelectContentFilter(EContentFilter::Models);
    const std::wstring rel = editor::MakeRelativeContentPath(ContentRoot, finalPath);
    for (int i = 0; i < (int)ContentListAssetIndices.size(); ++i)
    {
        const int assetIndex = ContentListAssetIndices[(size_t)i];
        if (assetIndex >= 0 && assetIndex < (int)ContentAssets.size() && ContentAssets[(size_t)assetIndex].RelativePath == rel)
        {
            SendMessageW(ContentList, LB_SETCURSEL, (WPARAM)i, 0);
            PlaceSelectedContentAsset();
            break;
        }
    }
}

void FEngine::PlaceSelectedContentAsset()
{
    if (!ContentList)
        return;
    const int sel = (int)SendMessageW(ContentList, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)ContentListAssetIndices.size())
        return;
    const int assetIndex = ContentListAssetIndices[(size_t)sel];
    if (assetIndex < 0 || assetIndex >= (int)ContentAssets.size())
        return;
    const editor::FAssetRecord& asset = ContentAssets[(size_t)assetIndex];
    if (asset.Kind == editor::EAssetKind::Level)
    {
        LoadLevelFromPath(asset.AbsolutePath);
        return;
    }
    if (asset.Kind == editor::EAssetKind::Texture)
    {
        EnsureTextureLoaded(asset.RelativePath);
        return;
    }
    if (asset.Kind == editor::EAssetKind::Material)
    {
        LoadContentMaterials();
        return;
    }
    if (asset.Kind != editor::EAssetKind::Model)
        return;

    const int meshIndex = EnsureStaticMeshLoaded(asset.RelativePath);
    if (meshIndex < 0)
        return;

    using namespace DirectX;
    XMVECTOR p = XMLoadFloat3(&Camera.Position);
    p = XMVectorAdd(p, XMVectorScale(Camera.GetForwardVector(), 4.0f));
    XMFLOAT3 pos{};
    XMStoreFloat3(&pos, p);
    pos.y = 0.0f;

    FSceneObject object = MakeSceneObject(FSceneObject::EType::StaticMesh, pos, asset.RelativePath);
    object.StaticMeshIndex = meshIndex;
    if (const auto found = StaticMeshRadiusByAsset.find(asset.RelativePath); found != StaticMeshRadiusByAsset.end())
        object.Radius = found->second;
    EnsureConcreteMaterialForObject(object);
    Objects.push_back(object);
    SetSelectedIndex((int)Objects.size() - 1);
    ClearAssetPreview();
    MarkLevelDirty();
    RefreshOutliner();
}

void FEngine::PreviewSelectedContentAsset()
{
    if (!ContentList)
        return;
    const int sel = (int)SendMessageW(ContentList, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)ContentListAssetIndices.size())
        return;
    const int assetIndex = ContentListAssetIndices[(size_t)sel];
    if (assetIndex < 0 || assetIndex >= (int)ContentAssets.size())
        return;
    PreviewContentAsset(ContentAssets[(size_t)assetIndex]);
}

void FEngine::PreviewContentAsset(const editor::FAssetRecord& asset)
{
    if (asset.Kind == editor::EAssetKind::Level)
    {
        ClearAssetPreview();
        SetPreviewBitmap(nullptr, false);
        LoadLevelFromPath(asset.AbsolutePath);
        return;
    }
    if (asset.Kind == editor::EAssetKind::Texture)
    {
        PreviewTextureAsset(asset);
        return;
    }
    if (asset.Kind == editor::EAssetKind::Material)
    {
        LoadContentMaterials();
        const int materialIndex = FindMaterialByAssetPath(asset.RelativePath);
        if (materialIndex >= 0)
            PreviewMaterialAsset(materialIndex, true);
        return;
    }
    if (asset.Kind == editor::EAssetKind::Model)
        PreviewModelAsset(asset);
}

void FEngine::PreviewTextureAsset(const editor::FAssetRecord& asset)
{
    ClearAssetPreview();
    const int textureIndex = EnsureTextureLoaded(asset.RelativePath);
    SelectTextureIndex(textureIndex);
    if (PreviewTitleLabel)
        SetWindowTextW(PreviewTitleLabel, L"Texture Preview");
    if (ContentHintLabel)
    {
        const std::wstring hint = L"Previewing texture: " + asset.Name;
        SetWindowTextW(ContentHintLabel, hint.c_str());
    }
    UpdateStatusText();
}

void FEngine::PreviewMaterialAsset(int materialIndex, bool openEditor)
{
    if (materialIndex < 0 || materialIndex >= (int)Materials.size())
        return;

    const FMaterialAsset& mat = Materials[(size_t)materialIndex];

    using namespace DirectX;
    XMVECTOR p = XMLoadFloat3(&Camera.Position);
    p = XMVectorAdd(p, XMVectorScale(Camera.GetForwardVector(), 4.0f));
    XMFLOAT3 pos{};
    XMStoreFloat3(&pos, p);

    FSceneObject object{};
    object.Name = L"Preview: " + mat.Name;
    object.Type = FSceneObject::EType::Sphere;
    object.Position = pos;
    object.Scale = { 1.0f, 1.0f, 1.0f };
    object.Radius = 0.75f;
    ApplyMaterialAssetToSceneObject(object, materialIndex);

    bAssetPreviewActive = true;
    AssetPreviewObject = object;
    AssetPreviewLabel = L"Material: " + mat.Name;
    AssetPreviewMaterialIndex = materialIndex;

    if (MaterialList)
        SendMessageW(MaterialList, LB_SETCURSEL, (WPARAM)materialIndex, 0);
    if (mat.AlbedoTexIndex >= 0)
        SelectTextureIndex(mat.AlbedoTexIndex);
    else
        SetPreviewBitmap(CreateMaterialPreviewBitmap(mat.Albedo), true);
    if (PreviewTitleLabel)
        SetWindowTextW(PreviewTitleLabel, L"Material Preview");
    if (ContentHintLabel)
    {
        const std::wstring hint = L"Previewing material: " + mat.Name;
        SetWindowTextW(ContentHintLabel, hint.c_str());
    }
    if (openEditor)
        OpenMaterialEditor(materialIndex);
    UpdateStatusText();
}

void FEngine::PreviewModelAsset(const editor::FAssetRecord& asset)
{
    const int meshIndex = EnsureStaticMeshLoaded(asset.RelativePath);
    if (meshIndex < 0)
        return;

    float sourceRadius = 1.0f;
    if (const auto found = StaticMeshRadiusByAsset.find(asset.RelativePath); found != StaticMeshRadiusByAsset.end())
        sourceRadius = std::max(found->second, 0.1f);
    const float previewScale = Clamp(1.35f / sourceRadius, 0.02f, 8.0f);

    using namespace DirectX;
    XMVECTOR p = XMLoadFloat3(&Camera.Position);
    p = XMVectorAdd(p, XMVectorScale(Camera.GetForwardVector(), 4.5f));
    XMFLOAT3 pos{};
    XMStoreFloat3(&pos, p);
    pos.y = std::max(pos.y, 0.0f);

    FSceneObject object{};
    object.Name = L"Preview: " + asset.Name;
    object.Type = FSceneObject::EType::StaticMesh;
    object.AssetPath = asset.RelativePath;
    object.StaticMeshIndex = meshIndex;
    object.Position = pos;
    object.Scale = { previewScale, previewScale, previewScale };
    object.Radius = sourceRadius;
    object.Albedo = { 0.74f, 0.74f, 0.70f };
    object.Metallic = 0.0f;
    object.Roughness = 0.55f;
    object.MaterialIndex = -1;
    object.MaterialSRVBase = 0;

    bAssetPreviewActive = true;
    AssetPreviewObject = object;
    AssetPreviewLabel = L"Model: " + asset.Name;
    AssetPreviewMaterialIndex = -1;
    SetPreviewBitmap(nullptr, false);
    if (PreviewTitleLabel)
        SetWindowTextW(PreviewTitleLabel, L"Model Preview");
    if (ContentHintLabel)
    {
        const std::wstring hint = L"Previewing model: " + asset.Name + L"    Use Place to add it to the Level.";
        SetWindowTextW(ContentHintLabel, hint.c_str());
    }
    UpdateStatusText();
}

void FEngine::ClearAssetPreview()
{
    bAssetPreviewActive = false;
    AssetPreviewObject = {};
    AssetPreviewLabel.clear();
    AssetPreviewMaterialIndex = -1;
}

void FEngine::FocusViewportOnObject(int objectIndex)
{
    if (objectIndex < 0 || objectIndex >= (int)Objects.size())
        return;

    using namespace DirectX;
    const FSceneObject& object = Objects[(size_t)objectIndex];
    const float radius = std::max(GetObjectPickRadius(object), 0.35f);
    const float distance = Clamp(radius * 3.25f, 2.0f, 55.0f);
    const XMVECTOR target = XMLoadFloat3(&object.Position);
    const XMVECTOR forward = Camera.GetForwardVector();
    const XMVECTOR cameraPos = XMVectorSubtract(target, XMVectorScale(forward, distance));
    XMStoreFloat3(&Camera.Position, cameraPos);

    if (ViewportHwnd)
        SetFocus(ViewportHwnd);
    if (ContentHintLabel)
    {
        const std::wstring hint = L"Focused viewport on: " + object.Name;
        SetWindowTextW(ContentHintLabel, hint.c_str());
    }
    UpdateStatusText();
}

void FEngine::SetPreviewBitmap(HBITMAP bitmap, bool takeOwnership)
{
    if (GeneratedPreviewBitmap)
    {
        DeleteObject(GeneratedPreviewBitmap);
        GeneratedPreviewBitmap = nullptr;
    }
    if (takeOwnership)
        GeneratedPreviewBitmap = bitmap;
    if (TexturePreview)
    {
        SendMessageW(TexturePreview, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)bitmap);
        InvalidateRect(TexturePreview, nullptr, TRUE);
    }
}

void FEngine::SelectTextureIndex(int textureIndex)
{
    SelectedTextureIndex = (textureIndex >= 0 && textureIndex < (int)Textures.size()) ? textureIndex : -1;
    if (TextureList)
        SendMessageW(TextureList, LB_SETCURSEL, (SelectedTextureIndex >= 0) ? (WPARAM)SelectedTextureIndex : (WPARAM)-1, 0);
    HBITMAP bmp = nullptr;
    if (SelectedTextureIndex >= 0)
        bmp = Textures[(size_t)SelectedTextureIndex].Preview;
    SetPreviewBitmap(bmp, false);
    if (PreviewTitleLabel)
        SetWindowTextW(PreviewTitleLabel, L"Texture Preview");
}

HBITMAP FEngine::CreateMaterialPreviewBitmap(const DirectX::XMFLOAT3& color) const
{
    constexpr int w = 160;
    constexpr int h = 120;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!bitmap || !bits)
        return bitmap;

    const DirectX::XMFLOAT3 c = Clamp01(color);
    unsigned char* pixels = static_cast<unsigned char*>(bits);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const float u = (float)x / (float)(w - 1);
            const float v = (float)y / (float)(h - 1);
            const float shade = 0.55f + 0.35f * u + 0.10f * (1.0f - v);
            const int offset = (y * w + x) * 4;
            pixels[offset + 0] = (unsigned char)ClampI((int)(c.z * shade * 255.0f), 0, 255);
            pixels[offset + 1] = (unsigned char)ClampI((int)(c.y * shade * 255.0f), 0, 255);
            pixels[offset + 2] = (unsigned char)ClampI((int)(c.x * shade * 255.0f), 0, 255);
            pixels[offset + 3] = 255;
        }
    }
    return bitmap;
}

int FEngine::EnsureStaticMeshLoaded(const std::wstring& relativePath)
{
    if (const auto found = StaticMeshByAsset.find(relativePath); found != StaticMeshByAsset.end())
        return found->second;

    editor::FObjMeshData mesh{};
    std::wstring error;
    const std::filesystem::path path = editor::ResolveContentPath(ContentRoot, relativePath);
    if (!editor::LoadObjMesh(path, mesh, &error))
        return -1;
    const int meshIndex = Renderer.CreateStaticMesh(RHI, mesh.Vertices, mesh.Indices);
    if (meshIndex >= 0)
    {
        StaticMeshByAsset[relativePath] = meshIndex;
        StaticMeshRadiusByAsset[relativePath] = mesh.BoundsRadius;
    }
    return meshIndex;
}

int FEngine::FindTextureByRelativePath(const std::wstring& relativePath) const
{
    if (relativePath.empty())
        return -1;
    for (int i = 0; i < (int)Textures.size(); ++i)
        if (Textures[(size_t)i].RelativePath == relativePath)
            return i;
    return -1;
}

int FEngine::EnsureTextureLoaded(const std::wstring& relativePath)
{
    const int existing = FindTextureByRelativePath(relativePath);
    if (existing >= 0 || relativePath.empty())
        return existing;
    const std::filesystem::path path = editor::ResolveContentPath(ContentRoot, relativePath);
    AddTextureFromFile(path.wstring());
    return FindTextureByRelativePath(relativePath);
}

int FEngine::FindMaterialByAssetPath(const std::wstring& relativePath) const
{
    if (relativePath.empty())
        return -1;
    for (int i = 0; i < (int)Materials.size(); ++i)
        if (Materials[(size_t)i].AssetPath == relativePath)
            return i;
    return -1;
}

void FEngine::MigrateContentMaterialsToV2()
{
    for (const editor::FAssetRecord& asset : ContentAssets)
    {
        if (asset.Kind != editor::EAssetKind::Material)
            continue;

        editor::FMaterialFile material{};
        std::wstring error;
        if (!editor::LoadMaterialFile(asset.AbsolutePath, material, &error))
            continue;

        if (material.Version >= 2)
            continue;

        material.Version = 2;
        if (material.ShadingMode.empty())
            material.ShadingMode = L"PbrLit";
        editor::SaveMaterialFile(asset.AbsolutePath, material, &error);
    }
}

void FEngine::LoadContentMaterials()
{
    const std::vector<editor::FAssetRecord> assets = ContentAssets;
    for (const editor::FAssetRecord& asset : assets)
    {
        if (asset.Kind != editor::EAssetKind::Material || FindMaterialByAssetPath(asset.RelativePath) >= 0)
            continue;
        editor::FMaterialFile src{};
        std::wstring error;
        if (!editor::LoadMaterialFile(asset.AbsolutePath, src, &error))
            continue;

        FMaterialAsset mat{};
        mat.Name = src.Name.empty() ? asset.Name : src.Name;
        mat.AssetPath = asset.RelativePath;
        mat.ShadingMode = ParseMaterialShadingMode(src.ShadingMode);
        mat.Albedo = src.Albedo;
        mat.Metallic = src.Metallic;
        mat.Roughness = src.Roughness;
        mat.UnlitIntensity = std::max(0.0f, src.Intensity);
        mat.RockNormalStrength = Clamp(src.NormalStrength, 0.0f, 1.0f);
        mat.RockBaseColorBoost = std::max(0.0f, src.BaseColorBoost);
        mat.AlbedoTexPath = src.AlbedoTexture;
        mat.NormalTexPath = src.NormalTexture;
        mat.RoughnessTexPath = src.RoughnessTexture;
        mat.MetallicTexPath = src.MetallicTexture;
        mat.AOTexPath = src.AOTexture;

        mat.AlbedoTexIndex = EnsureTextureLoaded(mat.AlbedoTexPath);
        mat.NormalTexIndex = EnsureTextureLoaded(mat.NormalTexPath);
        mat.RoughnessTexIndex = EnsureTextureLoaded(mat.RoughnessTexPath);
        mat.MetallicTexIndex = EnsureTextureLoaded(mat.MetallicTexPath);
        mat.AOTexIndex = EnsureTextureLoaded(mat.AOTexPath);
        UpdateMaterialRuntimeBindings(mat);
        Materials.push_back(mat);
        if (MaterialList)
            SendMessageW(MaterialList, LB_ADDSTRING, 0, (LPARAM)mat.Name.c_str());
    }
}

void FEngine::UpdateMaterialRuntimeBindings(FMaterialAsset& mat)
{
    mat.AlbedoTexSlot = (mat.AlbedoTexIndex >= 0 && mat.AlbedoTexIndex < (int)Textures.size()) ? Textures[(size_t)mat.AlbedoTexIndex].RendererSlot : 0;
    mat.NormalTexSlot = (mat.NormalTexIndex >= 0 && mat.NormalTexIndex < (int)Textures.size()) ? Textures[(size_t)mat.NormalTexIndex].RendererSlot : 1;
    mat.RoughnessTexSlot = (mat.RoughnessTexIndex >= 0 && mat.RoughnessTexIndex < (int)Textures.size()) ? Textures[(size_t)mat.RoughnessTexIndex].RendererSlot : 2;
    mat.MetallicTexSlot = (mat.MetallicTexIndex >= 0 && mat.MetallicTexIndex < (int)Textures.size()) ? Textures[(size_t)mat.MetallicTexIndex].RendererSlot : 3;
    mat.AOTexSlot = (mat.AOTexIndex >= 0 && mat.AOTexIndex < (int)Textures.size()) ? Textures[(size_t)mat.AOTexIndex].RendererSlot : 4;

    if (mat.SRVBase <= 0)
        mat.SRVBase = Renderer.AllocateMaterialSRVBlock();

    if (mat.ShadingMode == EMaterialShadingMode::Unlit)
        Renderer.UpdateMaterialSRVBlock(mat.SRVBase, mat.AlbedoTexSlot, 1, 2, 3, 4);
    else
        Renderer.UpdateMaterialSRVBlock(mat.SRVBase, mat.AlbedoTexSlot, mat.NormalTexSlot, mat.RoughnessTexSlot, mat.MetallicTexSlot, mat.AOTexSlot);
}

void FEngine::ApplyMaterialAssetToSceneObject(FSceneObject& object, int materialIndex) const
{
    if (materialIndex < 0 || materialIndex >= (int)Materials.size())
        return;

    const auto& mat = Materials[(size_t)materialIndex];
    object.Albedo = mat.Albedo;
    object.Metallic = mat.Metallic;
    object.Roughness = mat.Roughness;
    object.MaterialIndex = materialIndex;
    object.MaterialPath = mat.AssetPath;
    object.MaterialSRVBase = mat.SRVBase;
    object.MaterialShadingMode = mat.ShadingMode;
    object.UnlitIntensity = mat.UnlitIntensity;
    object.RockNormalStrength = mat.RockNormalStrength;
    object.RockBaseColorBoost = mat.RockBaseColorBoost;
    object.UseAlbedoTex = (mat.AlbedoTexIndex >= 0) ? 1.0f : 0.0f;
    const bool usesPbrSlots = mat.ShadingMode == EMaterialShadingMode::PbrLit
        || mat.ShadingMode == EMaterialShadingMode::Rdr2Rock
        || mat.ShadingMode == EMaterialShadingMode::Rdr2Foliage;
    object.UseNormalTex = (usesPbrSlots && mat.NormalTexIndex >= 0) ? 1.0f : 0.0f;
    object.UseRoughnessTex = (usesPbrSlots && mat.RoughnessTexIndex >= 0) ? 1.0f : 0.0f;
    object.UseMetallicTex = (usesPbrSlots && mat.MetallicTexIndex >= 0) ? 1.0f : 0.0f;
    object.UseAOTex = (usesPbrSlots && mat.AOTexIndex >= 0) ? 1.0f : 0.0f;
}

void FEngine::SaveMaterialAsset(int materialIndex)
{
    if (materialIndex < 0 || materialIndex >= (int)Materials.size())
        return;
    FMaterialAsset& mat = Materials[(size_t)materialIndex];
    if (mat.AssetPath.empty())
    {
        std::wstring fileName = mat.Name.empty() ? L"Material" : mat.Name;
        for (wchar_t& c : fileName)
            if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|')
                c = L'_';
        std::filesystem::path path = MakeUniquePath(ContentLayout.Materials, fileName + L".material.json");
        mat.AssetPath = editor::MakeRelativeContentPath(ContentRoot, path);
    }

    editor::FMaterialFile out{};
    out.Name = mat.Name;
    out.ShadingMode = MaterialShadingModeName(mat.ShadingMode);
    out.Albedo = mat.Albedo;
    out.Metallic = mat.Metallic;
    out.Roughness = mat.Roughness;
    out.Intensity = mat.UnlitIntensity;
    out.NormalStrength = mat.RockNormalStrength;
    out.BaseColorBoost = mat.RockBaseColorBoost;
    out.AlbedoTexture = (mat.AlbedoTexIndex >= 0 && mat.AlbedoTexIndex < (int)Textures.size()) ? Textures[(size_t)mat.AlbedoTexIndex].RelativePath : mat.AlbedoTexPath;
    out.NormalTexture = (mat.NormalTexIndex >= 0 && mat.NormalTexIndex < (int)Textures.size()) ? Textures[(size_t)mat.NormalTexIndex].RelativePath : mat.NormalTexPath;
    out.RoughnessTexture = (mat.RoughnessTexIndex >= 0 && mat.RoughnessTexIndex < (int)Textures.size()) ? Textures[(size_t)mat.RoughnessTexIndex].RelativePath : mat.RoughnessTexPath;
    out.MetallicTexture = (mat.MetallicTexIndex >= 0 && mat.MetallicTexIndex < (int)Textures.size()) ? Textures[(size_t)mat.MetallicTexIndex].RelativePath : mat.MetallicTexPath;
    out.AOTexture = (mat.AOTexIndex >= 0 && mat.AOTexIndex < (int)Textures.size()) ? Textures[(size_t)mat.AOTexIndex].RelativePath : mat.AOTexPath;

    std::wstring error;
    editor::SaveMaterialFile(editor::ResolveContentPath(ContentRoot, mat.AssetPath), out, &error);
    RefreshContentBrowser();
}

void FEngine::ApplyMaterialToObject(int objectIndex, int materialIndex)
{
    if (objectIndex < 0 || objectIndex >= (int)Objects.size() || materialIndex < 0 || materialIndex >= (int)Materials.size())
        return;
    if (!IsMaterialAssignableObject(Objects[(size_t)objectIndex]))
        return;
    ApplyMaterialAssetToSceneObject(Objects[(size_t)objectIndex], materialIndex);
    MarkLevelDirty();
}

bool FEngine::EnsureDefaultMaterialAsset()
{
    if (ContentRoot.empty())
        return false;

    const std::filesystem::path path = editor::ResolveContentPath(ContentRoot, kDefaultMaterialAssetPath);
    if (std::filesystem::exists(path))
    {
        editor::FMaterialFile existing{};
        std::wstring error;
        if (editor::LoadMaterialFile(path, existing, &error))
            return false;
    }

    editor::FMaterialFile material{};
    material.Version = 2;
    material.Name = L"Default PBR";
    material.ShadingMode = L"PbrLit";
    material.Albedo = { 0.75f, 0.75f, 0.75f };
    material.Metallic = 0.0f;
    material.Roughness = 0.55f;

    std::wstring error;
    return editor::SaveMaterialFile(path, material, &error);
}

int FEngine::EnsureDefaultMaterialLoaded()
{
    int materialIndex = FindMaterialByAssetPath(kDefaultMaterialAssetPath);
    if (materialIndex >= 0)
        return materialIndex;

    const bool created = EnsureDefaultMaterialAsset();
    if (created)
        RefreshContentBrowser();

    LoadContentMaterials();
    materialIndex = FindMaterialByAssetPath(kDefaultMaterialAssetPath);
    if (materialIndex >= 0)
        return materialIndex;

    editor::FMaterialFile src{};
    std::wstring error;
    const std::filesystem::path path = editor::ResolveContentPath(ContentRoot, kDefaultMaterialAssetPath);
    if (!editor::LoadMaterialFile(path, src, &error))
        return -1;

    FMaterialAsset mat{};
    mat.Name = src.Name.empty() ? L"Default PBR" : src.Name;
    mat.AssetPath = kDefaultMaterialAssetPath;
    mat.ShadingMode = ParseMaterialShadingMode(src.ShadingMode);
    mat.Albedo = src.Albedo;
    mat.Metallic = src.Metallic;
    mat.Roughness = src.Roughness;
    mat.UnlitIntensity = std::max(0.0f, src.Intensity);
    mat.RockNormalStrength = Clamp(src.NormalStrength, 0.0f, 1.0f);
    mat.RockBaseColorBoost = std::max(0.0f, src.BaseColorBoost);
    mat.AlbedoTexPath = src.AlbedoTexture;
    mat.NormalTexPath = src.NormalTexture;
    mat.RoughnessTexPath = src.RoughnessTexture;
    mat.MetallicTexPath = src.MetallicTexture;
    mat.AOTexPath = src.AOTexture;
    mat.AlbedoTexIndex = EnsureTextureLoaded(mat.AlbedoTexPath);
    mat.NormalTexIndex = EnsureTextureLoaded(mat.NormalTexPath);
    mat.RoughnessTexIndex = EnsureTextureLoaded(mat.RoughnessTexPath);
    mat.MetallicTexIndex = EnsureTextureLoaded(mat.MetallicTexPath);
    mat.AOTexIndex = EnsureTextureLoaded(mat.AOTexPath);
    UpdateMaterialRuntimeBindings(mat);
    Materials.push_back(mat);
    materialIndex = (int)Materials.size() - 1;
    if (MaterialList)
        SendMessageW(MaterialList, LB_ADDSTRING, 0, (LPARAM)Materials[(size_t)materialIndex].Name.c_str());
    return materialIndex;
}

bool FEngine::EnsureConcreteMaterialForObject(FSceneObject& object)
{
    if (!IsMaterialAssignableObject(object))
        return false;

    LoadContentMaterials();

    int materialIndex = -1;
    if (!object.MaterialPath.empty())
        materialIndex = FindMaterialByAssetPath(object.MaterialPath);
    else if (object.MaterialIndex >= 0 && object.MaterialIndex < (int)Materials.size() && !Materials[(size_t)object.MaterialIndex].AssetPath.empty())
        materialIndex = object.MaterialIndex;

    if (materialIndex < 0)
        materialIndex = EnsureDefaultMaterialLoaded();
    if (materialIndex < 0 || materialIndex >= (int)Materials.size())
        return false;

    const FMaterialAsset& mat = Materials[(size_t)materialIndex];
    const bool changed =
        object.MaterialIndex != materialIndex ||
        object.MaterialPath != mat.AssetPath ||
        object.MaterialSRVBase != mat.SRVBase ||
        object.MaterialShadingMode != mat.ShadingMode;
    ApplyMaterialAssetToSceneObject(object, materialIndex);
    return changed;
}

bool FEngine::EnsureConcreteMaterialsForRenderableObjects()
{
    LoadContentMaterials();
    EnsureDefaultMaterialLoaded();

    bool changed = false;
    for (FSceneObject& object : Objects)
        changed = EnsureConcreteMaterialForObject(object) || changed;
    return changed;
}

FSceneObject FEngine::MakeSceneObject(FSceneObject::EType type, const DirectX::XMFLOAT3& position, const std::wstring& assetPath)
{
    FSceneObject obj{};
    obj.Id = NextObjectId++;
    obj.Type = type;
    obj.AssetPath = assetPath;
    obj.Position = position;
    obj.Name = SceneObjectTypeToString(type) + L" " + std::to_wstring(obj.Id);
    if (type == FSceneObject::EType::RenderDocRock)
    {
        obj.Position.y += 0.39f;
        obj.Scale = { 0.35f, 0.35f, 0.35f };
    }
    switch (type)
    {
    case FSceneObject::EType::Sphere: obj.Radius = 0.75f; obj.Albedo = { 0.85f, 0.15f, 0.10f }; break;
    case FSceneObject::EType::Box: obj.Radius = 0.9f; obj.Albedo = { 0.18f, 0.55f, 0.95f }; break;
    case FSceneObject::EType::Cone: obj.Radius = 0.9f; obj.Albedo = { 0.95f, 0.75f, 0.20f }; break;
    case FSceneObject::EType::RenderDocRock: obj.Radius = 2.05f; obj.Albedo = { 0.45f, 0.45f, 0.45f }; break;
    case FSceneObject::EType::StaticMesh: obj.Radius = 1.0f; obj.Albedo = { 0.75f, 0.75f, 0.75f }; break;
    case FSceneObject::EType::SunLight:
        obj.Radius = 0.35f;
        obj.Scale = { 0.18f, 0.18f, 0.18f };
        obj.Albedo = { 1.0f, 0.82f, 0.18f };
        obj.LightIntensity = 3.0f;
        break;
    case FSceneObject::EType::SkyAtmosphere:
        obj.Radius = 0.45f;
        obj.Scale = { 0.28f, 0.28f, 0.28f };
        obj.Albedo = { 0.20f, 0.45f, 0.95f };
        obj.Roughness = 0.75f;
        break;
    }
    obj.Metallic = 0.0f;
    obj.Roughness = 0.35f;
    obj.MaterialIndex = -1;
    obj.MaterialSRVBase = Renderer.AllocateMaterialSRVBlock();
    Renderer.UpdateMaterialSRVBlock(obj.MaterialSRVBase, 0, 1, 2, 3, 4);
    return obj;
}

editor::FLevelFile FEngine::BuildLevelFile() const
{
    editor::FLevelFile level{};
    level.Name = CurrentLevelName;
    if (const FSceneObject* sun = FindActiveSunLight())
    {
        SunPositionToAngles(sun->Position, level.SunYaw, level.SunPitch);
        level.SunIntensity = sun->LightIntensity;
    }
    else
    {
        level.SunYaw = SunYaw;
        level.SunPitch = SunPitch;
        level.SunIntensity = SunIntensity;
    }
    for (const FSceneObject& object : Objects)
    {
        editor::FLevelObjectFile out{};
        out.Id = object.Id;
        out.Name = object.Name;
        out.Type = SceneObjectTypeToString(object.Type);
        out.Asset = object.AssetPath;
        out.Material = object.MaterialPath;
        out.Position = object.Position;
        out.Scale = object.Scale;
        out.Albedo = object.Albedo;
        out.Metallic = object.Metallic;
        out.Roughness = object.Roughness;
        out.LightIntensity = object.LightIntensity;
        out.SkyEnabled = object.SkyEnabled;
        out.RayleighScale = object.RayleighScale;
        out.MieScale = object.MieScale;
        out.MieG = object.MieG;
        out.AtmosphereHeight = object.AtmosphereHeight;
        level.Objects.push_back(out);
    }
    return level;
}

void FEngine::ApplyLevelFile(const editor::FLevelFile& level, const std::filesystem::path& path)
{
    ClearAssetPreview();
    SetPreviewBitmap(nullptr, false);
    Objects.clear();
    StaticMeshByAsset.clear();
    StaticMeshRadiusByAsset.clear();
    NextObjectId = 1;
    SunYaw = level.SunYaw;
    SunPitch = level.SunPitch;
    SunIntensity = level.SunIntensity;
    UpdateSkyUI();
    LoadContentMaterials();

    for (const editor::FLevelObjectFile& in : level.Objects)
    {
        FSceneObject::EType type = SceneObjectTypeFromString(in.Type);
        FSceneObject object = MakeSceneObject(type, in.Position, in.Asset);
        object.Id = in.Id ? in.Id : object.Id;
        object.Name = in.Name.empty() ? object.Name : in.Name;
        object.Scale = in.Scale;
        object.Albedo = in.Albedo;
        object.Metallic = in.Metallic;
        object.Roughness = in.Roughness;
        object.LightIntensity = in.LightIntensity;
        object.SkyEnabled = in.SkyEnabled;
        object.RayleighScale = in.RayleighScale;
        object.MieScale = in.MieScale;
        object.MieG = in.MieG;
        object.AtmosphereHeight = in.AtmosphereHeight;
        object.MaterialPath = in.Material;
        if (type == FSceneObject::EType::StaticMesh)
        {
            object.StaticMeshIndex = EnsureStaticMeshLoaded(in.Asset);
            if (const auto found = StaticMeshRadiusByAsset.find(in.Asset); found != StaticMeshRadiusByAsset.end())
                object.Radius = found->second;
            if (object.StaticMeshIndex < 0)
                continue;
        }
        Objects.push_back(object);
        EnsureConcreteMaterialForObject(Objects.back());
        NextObjectId = std::max(NextObjectId, object.Id + 1);
    }
    EnsureDefaultEnvironmentActors();
    CurrentLevelPath = path;
    CurrentLevelName = level.Name.empty() ? path.stem().wstring() : level.Name;
    bLevelDirty = false;
    SetSelectedIndex(Objects.empty() ? -1 : 0);
    RefreshOutliner();
}

std::wstring FEngine::SceneObjectTypeToString(FSceneObject::EType type)
{
    switch (type)
    {
    case FSceneObject::EType::Sphere: return L"Sphere";
    case FSceneObject::EType::Box: return L"Box";
    case FSceneObject::EType::Cone: return L"Cone";
    case FSceneObject::EType::RenderDocRock: return L"RenderDoc Rock";
    case FSceneObject::EType::StaticMesh: return L"Static Mesh";
    case FSceneObject::EType::SunLight: return L"Sun Light";
    case FSceneObject::EType::SkyAtmosphere: return L"Sky Atmosphere";
    default: return L"Object";
    }
}

FSceneObject::EType FEngine::SceneObjectTypeFromString(const std::wstring& type)
{
    if (type == L"Box") return FSceneObject::EType::Box;
    if (type == L"Cone") return FSceneObject::EType::Cone;
    if (type == L"RenderDoc Rock") return FSceneObject::EType::RenderDocRock;
    if (type == L"Static Mesh") return FSceneObject::EType::StaticMesh;
    if (type == L"Sun Light") return FSceneObject::EType::SunLight;
    if (type == L"Sky Atmosphere" || type == L"SkyAtmosphere") return FSceneObject::EType::SkyAtmosphere;
    return FSceneObject::EType::Sphere;
}

bool FEngine::IsMaterialAssignableObject(const FSceneObject& object)
{
    switch (object.Type)
    {
    case FSceneObject::EType::Sphere:
    case FSceneObject::EType::Box:
    case FSceneObject::EType::Cone:
    case FSceneObject::EType::StaticMesh:
    case FSceneObject::EType::RenderDocRock:
        return true;
    default:
        return false;
    }
}

void FEngine::ApplyDetailsEdits()
{
    if (SelectedIndex < 0 || SelectedIndex >= (int)Objects.size())
        return;
    auto getText = [](HWND h) -> std::wstring
    {
        wchar_t buf[256]{};
        if (h) GetWindowTextW(h, buf, (int)_countof(buf));
        return buf;
    };
    auto getFloat = [&](HWND h, float fallback) -> float
    {
        const std::wstring text = getText(h);
        wchar_t* end = nullptr;
        const float v = std::wcstof(text.c_str(), &end);
        return (end && end != text.c_str()) ? v : fallback;
    };

    FSceneObject& object = Objects[(size_t)SelectedIndex];
    object.Name = getText(DetailNameEdit);
    object.Position.x = getFloat(DetailPosXEdit, object.Position.x);
    object.Position.y = getFloat(DetailPosYEdit, object.Position.y);
    object.Position.z = getFloat(DetailPosZEdit, object.Position.z);
    object.Scale.x = std::max(0.01f, getFloat(DetailScaleXEdit, object.Scale.x));
    object.Scale.y = std::max(0.01f, getFloat(DetailScaleYEdit, object.Scale.y));
    object.Scale.z = std::max(0.01f, getFloat(DetailScaleZEdit, object.Scale.z));
    if (object.Type == FSceneObject::EType::SunLight)
    {
        object.LightIntensity = std::max(0.0f, getFloat(DetailIntensityEdit, object.LightIntensity));
    }
    else if (object.Type == FSceneObject::EType::SkyAtmosphere)
    {
        object.SkyEnabled = DetailSkyEnabledCheckbox
            ? (SendMessageW(DetailSkyEnabledCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED)
            : object.SkyEnabled;
        object.RayleighScale = std::max(0.0f, getFloat(DetailRayleighEdit, object.RayleighScale));
        object.MieScale = std::max(0.0f, getFloat(DetailMieEdit, object.MieScale));
        object.MieG = Clamp(getFloat(DetailMieGEdit, object.MieG), 0.0f, 0.99f);
        object.AtmosphereHeight = std::max(0.5f, getFloat(DetailAtmosphereHeightEdit, object.AtmosphereHeight));
    }
    MarkLevelDirty();
    RefreshOutliner();
    RefreshDetailsPanel();
}

void FEngine::OpenMaterialEditor(int materialIndex)
{
    // 校验索引有效性。
    if (materialIndex < 0 || materialIndex >= (int)Materials.size()) return;
    EditingMaterialIndex = materialIndex;
    if (bUseImGuiEditor)
    {
        bShowImGuiMaterialEditor = true;
        return;
    }

    if (!MaterialEditorHwnd)
    {
        // 创建材质编辑器窗口与子控件。
        constexpr int kSliderSteps = 10000;

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
            380,
            Window.GetHwnd(),
            nullptr,
            wc.hInstance,
            this);

        CreateWindowExW(0, L"STATIC", L"Shading Mode:", WS_CHILD | WS_VISIBLE, 16, 18, 90, 18, MaterialEditorHwnd, MenuHandle(IDC_MATERIAL_LABEL_SHADING), wc.hInstance, nullptr);
        HWND modeCombo = CreateWindowExW(0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 110, 14, 260, 200, MaterialEditorHwnd, MenuHandle(IDC_MATERIAL_SHADING_MODE), wc.hInstance, nullptr);
        for (const FMaterialSchema& schema : kMaterialSchemas)
            SendMessageW(modeCombo, CB_ADDSTRING, 0, (LPARAM)schema.Name);

        CreateWindowExW(0, L"STATIC", L"Base Color:", WS_CHILD | WS_VISIBLE, 16, 52, 88, 18, MaterialEditorHwnd, MenuHandle(IDC_MATERIAL_LABEL_COLOR), wc.hInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Pick...", WS_CHILD | WS_VISIBLE, 110, 48, 80, 24, MaterialEditorHwnd, MenuHandle(IDC_MATERIAL_COLOR), wc.hInstance, nullptr);

        CreateWindowExW(0, L"STATIC", L"Roughness:", WS_CHILD | WS_VISIBLE, 16, 92, 88, 18, MaterialEditorHwnd, MenuHandle(IDC_MATERIAL_LABEL_PARAM_A), wc.hInstance, nullptr);
        CreateWindowExW(0, TRACKBAR_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 110, 88, 260, 30, MaterialEditorHwnd, MenuHandle(IDC_MATERIAL_PARAM_A), wc.hInstance, nullptr);
        SendMessageW(GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_PARAM_A), TBM_SETRANGE, TRUE, MAKELPARAM(0, kSliderSteps));
        SendMessageW(GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_PARAM_A), TBM_SETTICFREQ, 1000, 0);
        SendMessageW(GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_PARAM_A), TBM_SETLINESIZE, 0, 10);
        SendMessageW(GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_PARAM_A), TBM_SETPAGESIZE, 0, 100);

        CreateWindowExW(0, L"STATIC", L"Metallic:", WS_CHILD | WS_VISIBLE, 16, 130, 88, 18, MaterialEditorHwnd, MenuHandle(IDC_MATERIAL_LABEL_PARAM_B), wc.hInstance, nullptr);
        CreateWindowExW(0, TRACKBAR_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 110, 126, 260, 30, MaterialEditorHwnd, MenuHandle(IDC_MATERIAL_PARAM_B), wc.hInstance, nullptr);
        SendMessageW(GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_PARAM_B), TBM_SETRANGE, TRUE, MAKELPARAM(0, kSliderSteps));
        SendMessageW(GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_PARAM_B), TBM_SETTICFREQ, 1000, 0);
        SendMessageW(GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_PARAM_B), TBM_SETLINESIZE, 0, 10);
        SendMessageW(GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_PARAM_B), TBM_SETPAGESIZE, 0, 100);

        CreateWindowExW(0, L"STATIC", L"BaseColor Tex:", WS_CHILD | WS_VISIBLE, 16, 170, 88, 18, MaterialEditorHwnd, MenuHandle(IDC_MATERIAL_LABEL_TEX0), wc.hInstance, nullptr);
        CreateWindowExW(0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 110, 166, 260, 200, MaterialEditorHwnd, MenuHandle(IDC_MATERIAL_TEX0), wc.hInstance, nullptr);

        CreateWindowExW(0, L"STATIC", L"Normal Tex:", WS_CHILD | WS_VISIBLE, 16, 196, 88, 18, MaterialEditorHwnd, MenuHandle(IDC_MATERIAL_LABEL_TEX1), wc.hInstance, nullptr);
        CreateWindowExW(0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 110, 192, 260, 200, MaterialEditorHwnd, MenuHandle(IDC_MATERIAL_TEX1), wc.hInstance, nullptr);

        CreateWindowExW(0, L"STATIC", L"Rough Tex:", WS_CHILD | WS_VISIBLE, 16, 222, 88, 18, MaterialEditorHwnd, MenuHandle(IDC_MATERIAL_LABEL_TEX2), wc.hInstance, nullptr);
        CreateWindowExW(0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 110, 218, 260, 200, MaterialEditorHwnd, MenuHandle(IDC_MATERIAL_TEX2), wc.hInstance, nullptr);

        CreateWindowExW(0, L"STATIC", L"Metal Tex:", WS_CHILD | WS_VISIBLE, 16, 248, 88, 18, MaterialEditorHwnd, MenuHandle(IDC_MATERIAL_LABEL_TEX3), wc.hInstance, nullptr);
        CreateWindowExW(0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 110, 244, 260, 200, MaterialEditorHwnd, MenuHandle(IDC_MATERIAL_TEX3), wc.hInstance, nullptr);

        CreateWindowExW(0, L"STATIC", L"AO Tex:", WS_CHILD | WS_VISIBLE, 16, 274, 88, 18, MaterialEditorHwnd, MenuHandle(IDC_MATERIAL_LABEL_TEX4), wc.hInstance, nullptr);
        CreateWindowExW(0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 110, 270, 260, 200, MaterialEditorHwnd, MenuHandle(IDC_MATERIAL_TEX4), wc.hInstance, nullptr);

        CreateWindowExW(0, L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 290, 308, 80, 26, MaterialEditorHwnd, MenuHandle(IDC_MATERIAL_APPLY), wc.hInstance, nullptr);
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
    if (HWND mode = GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_SHADING_MODE))
        SendMessageW(mode, CB_SETCURSEL, (WPARAM)MaterialShadingModeComboIndex(mat.ShadingMode), 0);

    const FMaterialSchema& schema = GetMaterialSchema(mat.ShadingMode);
    if (HWND label = GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_LABEL_COLOR))
        SetWindowTextW(label, schema.ColorLabel);
    if (HWND label = GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_LABEL_PARAM_A))
        SetWindowTextW(label, schema.ParamALabel);
    if (HWND label = GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_LABEL_PARAM_B))
        SetWindowTextW(label, schema.ParamBLabel);
    if (HWND label = GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_LABEL_TEX0))
        SetWindowTextW(label, schema.TextureLabels[0]);
    if (HWND label = GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_LABEL_TEX1))
        SetWindowTextW(label, schema.TextureLabels[1]);
    if (HWND label = GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_LABEL_TEX2))
        SetWindowTextW(label, schema.TextureLabels[2]);
    if (HWND label = GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_LABEL_TEX3))
        SetWindowTextW(label, schema.TextureLabels[3]);
    if (HWND label = GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_LABEL_TEX4))
        SetWindowTextW(label, schema.TextureLabels[4]);

    HWND paramA = GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_PARAM_A);
    HWND paramB = GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_PARAM_B);
    if (paramA)
    {
        const float v = (mat.ShadingMode == EMaterialShadingMode::Unlit) ? Clamp(mat.UnlitIntensity / 5.0f, 0.0f, 1.0f) : Clamp(mat.Roughness, 0.0f, 1.0f);
        SendMessageW(paramA, TBM_SETPOS, TRUE, (LPARAM)(int)(v * kSliderSteps));
    }
    if (paramB)
    {
        const float v = (mat.ShadingMode == EMaterialShadingMode::Rdr2Rock || mat.ShadingMode == EMaterialShadingMode::Rdr2Foliage)
            ? Clamp(mat.RockNormalStrength, 0.0f, 1.0f)
            : Clamp(mat.Metallic, 0.0f, 1.0f);
        SendMessageW(paramB, TBM_SETPOS, TRUE, (LPARAM)(int)(v * kSliderSteps));
    }

    auto setVisible = [&](int id, bool visible)
    {
        HWND h = GetDlgItem(MaterialEditorHwnd, id);
        if (h) ShowWindow(h, visible ? SW_SHOW : SW_HIDE);
    };
    setVisible(IDC_MATERIAL_LABEL_PARAM_B, schema.HasParamB);
    setVisible(IDC_MATERIAL_PARAM_B, schema.HasParamB);
    setVisible(IDC_MATERIAL_LABEL_TEX1, schema.TextureCount > 1);
    setVisible(IDC_MATERIAL_TEX1, schema.TextureCount > 1);
    setVisible(IDC_MATERIAL_LABEL_TEX2, schema.TextureCount > 2);
    setVisible(IDC_MATERIAL_TEX2, schema.TextureCount > 2);
    setVisible(IDC_MATERIAL_LABEL_TEX3, schema.TextureCount > 3);
    setVisible(IDC_MATERIAL_TEX3, schema.TextureCount > 3);
    setVisible(IDC_MATERIAL_LABEL_TEX4, schema.TextureCount > 4);
    setVisible(IDC_MATERIAL_TEX4, schema.TextureCount > 4);

    // 填充 Albedo 纹理下拉框。
    HWND combo = GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_TEX0);
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

    setupCombo(IDC_MATERIAL_TEX1, mat.NormalTexIndex);
    setupCombo(IDC_MATERIAL_TEX2, mat.RoughnessTexIndex);
    setupCombo(IDC_MATERIAL_TEX3, mat.MetallicTexIndex);
    setupCombo(IDC_MATERIAL_TEX4, mat.AOTexIndex);
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
    const EMaterialShadingMode previousMode = mat.ShadingMode;

    // 读取滑条并更新粗糙度与金属度。
    if (HWND mode = GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_SHADING_MODE))
    {
        const int sel = (int)SendMessageW(mode, CB_GETCURSEL, 0, 0);
        mat.ShadingMode = MaterialShadingModeFromComboIndex(sel);
    }
    const bool modeChanged = previousMode != mat.ShadingMode;

    HWND paramA = GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_PARAM_A);
    HWND paramB = GetDlgItem(MaterialEditorHwnd, IDC_MATERIAL_PARAM_B);
    if (modeChanged)
    {
        if (mat.ShadingMode == EMaterialShadingMode::Unlit)
        {
            mat.UnlitIntensity = 1.0f;
        }
        else if (mat.ShadingMode == EMaterialShadingMode::Rdr2Rock || mat.ShadingMode == EMaterialShadingMode::Rdr2Foliage)
        {
            mat.Roughness = (mat.ShadingMode == EMaterialShadingMode::Rdr2Foliage) ? 0.62f : 0.82f;
            mat.Metallic = 0.0f;
            mat.RockNormalStrength = (mat.ShadingMode == EMaterialShadingMode::Rdr2Foliage) ? 0.45f : 0.18f;
            mat.RockBaseColorBoost = (mat.ShadingMode == EMaterialShadingMode::Rdr2Foliage) ? 1.0f : 1.25f;
        }
        else
        {
            mat.Roughness = 0.5f;
            mat.Metallic = 0.0f;
        }
    }
    else if (paramA)
    {
        const float value = Clamp((float)SendMessageW(paramA, TBM_GETPOS, 0, 0) / kSliderSteps, 0.0f, 1.0f);
        if (mat.ShadingMode == EMaterialShadingMode::Unlit)
            mat.UnlitIntensity = value * 5.0f;
        else
            mat.Roughness = value;
    }
    if (!modeChanged && paramB)
    {
        const float value = Clamp((float)SendMessageW(paramB, TBM_GETPOS, 0, 0) / kSliderSteps, 0.0f, 1.0f);
        if (mat.ShadingMode == EMaterialShadingMode::Rdr2Rock || mat.ShadingMode == EMaterialShadingMode::Rdr2Foliage)
            mat.RockNormalStrength = value;
        else if (mat.ShadingMode == EMaterialShadingMode::PbrLit)
            mat.Metallic = value;
    }

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

    readCombo(IDC_MATERIAL_TEX0, mat.AlbedoTexIndex, mat.AlbedoTexSlot, 0);
    if (mat.ShadingMode == EMaterialShadingMode::PbrLit || mat.ShadingMode == EMaterialShadingMode::Rdr2Rock || mat.ShadingMode == EMaterialShadingMode::Rdr2Foliage)
    {
        readCombo(IDC_MATERIAL_TEX1, mat.NormalTexIndex, mat.NormalTexSlot, 1);
        readCombo(IDC_MATERIAL_TEX2, mat.RoughnessTexIndex, mat.RoughnessTexSlot, 2);
        readCombo(IDC_MATERIAL_TEX3, mat.MetallicTexIndex, mat.MetallicTexSlot, 3);
        readCombo(IDC_MATERIAL_TEX4, mat.AOTexIndex, mat.AOTexSlot, 4);
    }
    else
    {
        mat.NormalTexIndex = -1;
        mat.RoughnessTexIndex = -1;
        mat.MetallicTexIndex = -1;
        mat.AOTexIndex = -1;
        mat.NormalTexSlot = 1;
        mat.RoughnessTexSlot = 2;
        mat.MetallicTexSlot = 3;
        mat.AOTexSlot = 4;
    }
    mat.AlbedoTexPath = (mat.AlbedoTexIndex >= 0) ? Textures[(size_t)mat.AlbedoTexIndex].RelativePath : L"";
    mat.NormalTexPath = (mat.NormalTexIndex >= 0) ? Textures[(size_t)mat.NormalTexIndex].RelativePath : L"";
    mat.RoughnessTexPath = (mat.RoughnessTexIndex >= 0) ? Textures[(size_t)mat.RoughnessTexIndex].RelativePath : L"";
    mat.MetallicTexPath = (mat.MetallicTexIndex >= 0) ? Textures[(size_t)mat.MetallicTexIndex].RelativePath : L"";
    mat.AOTexPath = (mat.AOTexIndex >= 0) ? Textures[(size_t)mat.AOTexIndex].RelativePath : L"";

    UpdateMaterialRuntimeBindings(mat);

    // Hot-reload objects that reference this material index.
    for (auto& obj : Objects)
    {
        if (obj.MaterialIndex == EditingMaterialIndex)
        {
            // 同步材质参数到对象实例。
            ApplyMaterialAssetToSceneObject(obj, EditingMaterialIndex);
        }
    }
    SaveMaterialAsset(EditingMaterialIndex);
    if (bAssetPreviewActive && AssetPreviewMaterialIndex == EditingMaterialIndex)
        PreviewMaterialAsset(EditingMaterialIndex, false);
    MarkLevelDirty();
}

void FEngine::ImGuiAllocateSrv(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu)
{
    auto* engine = info ? static_cast<FEngine*>(info->UserData) : nullptr;
    if (!engine || !engine->ImGuiSrvHeap || !outCpu || !outGpu)
        return;
    const uint32 index = std::min(engine->ImGuiNextSrvDescriptor++, engine->ImGuiSrvDescriptorCapacity - 1);
    *outCpu = engine->ImGuiSrvHeap->GetCPUDescriptorHandleForHeapStart();
    *outGpu = engine->ImGuiSrvHeap->GetGPUDescriptorHandleForHeapStart();
    outCpu->ptr += SIZE_T(index) * engine->ImGuiSrvDescriptorSize;
    outGpu->ptr += SIZE_T(index) * engine->ImGuiSrvDescriptorSize;
}

void FEngine::ImGuiFreeSrv(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE)
{
}

void FEngine::InitImGuiEditor()
{
    if (bImGuiInitialized || !RHI.GetDevice())
        return;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = ImGuiSrvDescriptorCapacity;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(RHI.GetDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&ImGuiSrvHeap)), "CreateDescriptorHeap ImGui SRV failed");
    ImGuiSrvDescriptorSize = RHI.GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ImGuiNextSrvDescriptor = 0;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 3.0f;
    style.FrameRounding = 2.0f;
    style.TabRounding = 3.0f;
    style.GrabRounding = 2.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.075f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.09f, 0.10f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.10f, 0.35f, 0.62f, 0.75f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.14f, 0.45f, 0.78f, 0.85f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.07f, 0.42f, 0.78f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.15f, 0.15f, 0.17f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.32f, 0.42f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.10f, 0.40f, 0.70f, 1.0f);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.10f, 0.10f, 0.11f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.16f, 0.34f, 0.56f, 1.0f);
    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.12f, 0.25f, 0.40f, 1.0f);

    ImGui_ImplWin32_Init(ViewportHwnd ? ViewportHwnd : Window.GetHwnd());

    ImGui_ImplDX12_InitInfo initInfo{};
    initInfo.Device = RHI.GetDevice();
    initInfo.CommandQueue = RHI.GetCommandQueue();
    initInfo.NumFramesInFlight = FD3D12RHI::kFrameCount;
    initInfo.RTVFormat = RHI.GetBackBufferFormat();
    initInfo.DSVFormat = RHI.GetDepthFormat();
    initInfo.UserData = this;
    initInfo.SrvDescriptorHeap = ImGuiSrvHeap.Get();
    initInfo.SrvDescriptorAllocFn = &FEngine::ImGuiAllocateSrv;
    initInfo.SrvDescriptorFreeFn = &FEngine::ImGuiFreeSrv;
    if (!ImGui_ImplDX12_Init(&initInfo))
        throw std::runtime_error("ImGui_ImplDX12_Init failed");

    bImGuiInitialized = true;
}

void FEngine::ShutdownImGuiEditor()
{
    if (!bImGuiInitialized)
        return;
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ImGuiSrvHeap.Reset();
    bImGuiInitialized = false;
}

void FEngine::BeginImGuiEditorFrame()
{
    if (!bImGuiInitialized)
        return;
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
}

void FEngine::DrawImGuiMainMenu()
{
    if (!ImGui::BeginMainMenuBar())
        return;
    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("New Level", "Ctrl+N")) NewLevel();
        if (ImGui::MenuItem("Open Level", "Ctrl+O")) OpenLevelFromDialog();
        if (ImGui::MenuItem("Save Level", "Ctrl+S")) SaveCurrentLevel(false);
        ImGui::Separator();
        if (ImGui::MenuItem("Import OBJ")) ImportObjFromDialog();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window"))
    {
        ImGui::MenuItem("Render Settings", nullptr, &bShowImGuiSettings);
        if (ImGui::MenuItem("Material Editor", nullptr, bShowImGuiMaterialEditor))
        {
            if ((EditingMaterialIndex < 0 || EditingMaterialIndex >= (int)Materials.size()) && !Materials.empty())
                EditingMaterialIndex = 0;
            bShowImGuiMaterialEditor = true;
        }
        ImGui::EndMenu();
    }
    const std::string level = editor::ToUtf8(CurrentLevelName.empty() ? L"Untitled" : CurrentLevelName);
    const std::string selected = (SelectedIndex >= 0 && SelectedIndex < (int)Objects.size()) ? editor::ToUtf8(Objects[(size_t)SelectedIndex].Name) : "None";
    ImGui::Separator();
    ImGui::TextUnformatted(("Level: " + level + (bLevelDirty ? "*" : "")).c_str());
    ImGui::Separator();
    ImGui::TextUnformatted(("Selected: " + selected).c_str());
    ImGui::EndMainMenuBar();
}

void FEngine::DrawImGuiPlaceActors()
{
    ImGui::Begin("Place Actors");
    auto placeButton = [&](const char* label, FSceneObject::EType type)
    {
        if (ImGui::Selectable(label, false))
        {
            using namespace DirectX;
            XMVECTOR p = XMLoadFloat3(&Camera.Position);
            p = XMVectorAdd(p, XMVectorScale(Camera.GetForwardVector(), 4.0f));
            XMFLOAT3 pos{};
            XMStoreFloat3(&pos, p);
            if (type != FSceneObject::EType::SunLight && type != FSceneObject::EType::SkyAtmosphere)
                pos.y = 0.0f;
            FSceneObject obj = MakeSceneObject(type, pos);
            EnsureConcreteMaterialForObject(obj);
            Objects.push_back(obj);
            SetSelectedIndex((int)Objects.size() - 1);
            MarkLevelDirty();
        }
    };
    if (ImGui::CollapsingHeader("Basic", ImGuiTreeNodeFlags_DefaultOpen))
    {
        placeButton("Sphere", FSceneObject::EType::Sphere);
        placeButton("Box", FSceneObject::EType::Box);
        placeButton("Cone", FSceneObject::EType::Cone);
    }
    if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
    {
        placeButton("Sun Light", FSceneObject::EType::SunLight);
        placeButton("Sky Atmosphere", FSceneObject::EType::SkyAtmosphere);
    }
    if (ImGui::CollapsingHeader("RenderDoc", ImGuiTreeNodeFlags_DefaultOpen))
    {
        placeButton("RenderDoc Rock", FSceneObject::EType::RenderDocRock);
    }
    ImGui::End();
}

void FEngine::DrawImGuiViewport()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    bImGuiViewportHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    bImGuiViewportFocused = ImGui::IsWindowFocused();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGuiViewportScreenX = (int)std::floor(origin.x);
    ImGuiViewportScreenY = (int)std::floor(origin.y);
    const ImVec2 displayPos = ImGui::GetMainViewport()->Pos;
    ImGuiViewportX = std::max(0, (int)std::floor(origin.x - displayPos.x));
    ImGuiViewportY = std::max(0, (int)std::floor(origin.y - displayPos.y));
    ImGuiViewportW = std::max(1, (int)std::floor(avail.x));
    ImGuiViewportH = std::max(1, (int)std::floor(avail.y));
    bImGuizmoHovered = false;
    bImGuizmoUsing = false;
    DrawImGuiGizmo();
    ImGui::Dummy(avail);
    ImGui::End();
    ImGui::PopStyleVar();
}

void FEngine::DrawImGuiOutliner()
{
    ImGui::Begin("Level Outliner");
    for (int i = 0; i < (int)Objects.size(); ++i)
    {
        const std::string label = editor::ToUtf8(Objects[(size_t)i].Name) + "##obj" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), SelectedIndex == i))
            SetSelectedIndex(i);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            FocusViewportOnObject(i);
    }
    ImGui::End();
}

void FEngine::DrawImGuiDetails()
{
    ImGui::Begin("Details");
    if (SelectedIndex < 0 || SelectedIndex >= (int)Objects.size())
    {
        ImGui::TextDisabled("No actor selected");
        ImGui::End();
        return;
    }

    FSceneObject& obj = Objects[(size_t)SelectedIndex];
    char nameBuf[256]{};
    const std::string name = editor::ToUtf8(obj.Name);
    strncpy_s(nameBuf, name.c_str(), _TRUNCATE);
    if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
    {
        obj.Name = editor::FromUtf8(nameBuf);
        MarkLevelDirty();
    }

    float pos[3] = { obj.Position.x, obj.Position.y, obj.Position.z };
    if (ImGui::DragFloat3("Location", pos, 0.05f))
    {
        obj.Position = { pos[0], pos[1], pos[2] };
        MarkLevelDirty();
    }
    float scale[3] = { obj.Scale.x, obj.Scale.y, obj.Scale.z };
    if (ImGui::DragFloat3("Scale", scale, 0.02f, 0.01f, 100.0f))
    {
        obj.Scale = { scale[0], scale[1], scale[2] };
        MarkLevelDirty();
    }
    if (IsMaterialAssignableObject(obj))
    {
        LoadContentMaterials();
        if (EnsureConcreteMaterialForObject(obj))
            MarkLevelDirty();

        ImGui::SeparatorText("Material");

        int activeMaterialIndex = obj.MaterialIndex;
        if ((activeMaterialIndex < 0 || activeMaterialIndex >= (int)Materials.size()) && !obj.MaterialPath.empty())
            activeMaterialIndex = FindMaterialByAssetPath(obj.MaterialPath);

        const bool hasBoundMaterial = activeMaterialIndex >= 0 && activeMaterialIndex < (int)Materials.size();
        std::string preview = "Missing Material";
        if (hasBoundMaterial)
            preview = editor::ToUtf8(Materials[(size_t)activeMaterialIndex].Name);
        else if (!obj.MaterialPath.empty())
            preview = "Missing: " + editor::ToUtf8(obj.MaterialPath);

        if (ImGui::BeginCombo("Material", preview.c_str()))
        {
            for (int i = 0; i < (int)Materials.size(); ++i)
            {
                const FMaterialAsset& mat = Materials[(size_t)i];
                std::string label = editor::ToUtf8(mat.Name);
                if (!mat.AssetPath.empty())
                    label += "    " + editor::ToUtf8(mat.AssetPath);
                label += "##details_material_" + std::to_string(i);

                const bool selected = activeMaterialIndex == i || (!obj.MaterialPath.empty() && obj.MaterialPath == mat.AssetPath);
                if (ImGui::Selectable(label.c_str(), selected))
                    ApplyMaterialToObject(SelectedIndex, i);
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (hasBoundMaterial)
        {
            const FMaterialAsset& mat = Materials[(size_t)activeMaterialIndex];
            if (!mat.AssetPath.empty())
                ImGui::TextDisabled("%s", editor::ToUtf8(mat.AssetPath).c_str());
        }
        else if (!obj.MaterialPath.empty())
        {
            ImGui::TextColored(ImVec4(0.95f, 0.48f, 0.32f, 1.0f), "%s", editor::ToUtf8(obj.MaterialPath).c_str());
        }
        else
        {
            ImGui::TextColored(ImVec4(0.95f, 0.48f, 0.32f, 1.0f), "No concrete material loaded");
        }
    }
    if (obj.Type == FSceneObject::EType::SunLight)
    {
        if (ImGui::DragFloat("Intensity", &obj.LightIntensity, 0.05f, 0.0f, 100.0f))
            MarkLevelDirty();
    }
    if (obj.Type == FSceneObject::EType::SkyAtmosphere)
    {
        if (ImGui::Checkbox("Enable", &obj.SkyEnabled))
            MarkLevelDirty();
        if (ImGui::DragFloat("Rayleigh", &obj.RayleighScale, 0.01f, 0.0f, 10.0f))
            MarkLevelDirty();
        if (ImGui::DragFloat("Mie", &obj.MieScale, 0.01f, 0.0f, 10.0f))
            MarkLevelDirty();
        if (ImGui::DragFloat("Mie G", &obj.MieG, 0.01f, -0.99f, 0.99f))
            MarkLevelDirty();
        if (ImGui::DragFloat("Atmosphere Height", &obj.AtmosphereHeight, 0.1f, 0.5f, 100.0f))
            MarkLevelDirty();
    }
    ImGui::End();
}

void FEngine::DrawImGuiContentDrawer()
{
    ImGui::Begin("Content Drawer");
    if (ImGui::Button("Import OBJ")) ImportObjFromDialog();
    ImGui::SameLine();
    if (ImGui::Button("Place/Open")) PlaceSelectedContentAsset();
    ImGui::SameLine();
    if (ImGui::Button("New Material"))
    {
        FMaterialAsset mat{};
        mat.Name = L"Material " + std::to_wstring((int)Materials.size() + 1);
        mat.AssetPath = L"Materials/" + mat.Name + L".material.json";
        Materials.push_back(mat);
        SaveMaterialAsset((int)Materials.size() - 1);
        RefreshContentBrowser();
    }
    ImGui::SameLine();
    if (ImGui::Button("New Level")) NewLevel();
    ImGui::SameLine();
    if (ImGui::Button("Open")) OpenLevelFromDialog();
    ImGui::SameLine();
    if (ImGui::Button("Save")) SaveCurrentLevel(false);
    ImGui::Separator();

    const char* folders[] = { "Models", "Textures", "Materials", "Levels" };
    ImGui::BeginChild("Folders", ImVec2(150, 0), true);
    for (int i = 0; i < 4; ++i)
    {
        if (ImGui::Selectable(folders[i], (int)ContentFilter == i))
        {
            SelectContentFilter((EContentFilter)i);
            ImGuiSelectedContentListIndex = -1;
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    const float inspectorWidth = 260.0f;
    const float assetWidth = std::max(220.0f, ImGui::GetContentRegionAvail().x - inspectorWidth - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginChild("Assets", ImVec2(assetWidth, 0), true);
    for (int local = 0; local < (int)ContentListAssetIndices.size(); ++local)
    {
        const int assetIndex = ContentListAssetIndices[(size_t)local];
        if (assetIndex < 0 || assetIndex >= (int)ContentAssets.size())
            continue;
        const auto& asset = ContentAssets[(size_t)assetIndex];
        const std::string label = editor::ToUtf8(asset.Name + L"    " + asset.RelativePath) + "##asset" + std::to_string(local);
        if (ImGui::Selectable(label.c_str(), ImGuiSelectedContentListIndex == local))
        {
            ImGuiSelectedContentListIndex = local;
            if (ContentList)
                SendMessageW(ContentList, LB_SETCURSEL, (WPARAM)local, 0);
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            ImGuiSelectedContentListIndex = local;
            if (ContentList)
                SendMessageW(ContentList, LB_SETCURSEL, (WPARAM)local, 0);
            PreviewContentAsset(asset);
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("Inspector", ImVec2(0, 0), true);
    ImGui::TextUnformatted("Inspector");
    ImGui::Separator();
    if (ImGuiSelectedContentListIndex >= 0 && ImGuiSelectedContentListIndex < (int)ContentListAssetIndices.size())
    {
        const int assetIndex = ContentListAssetIndices[(size_t)ImGuiSelectedContentListIndex];
        if (assetIndex >= 0 && assetIndex < (int)ContentAssets.size())
        {
            const auto& asset = ContentAssets[(size_t)assetIndex];
            ImGui::TextWrapped("%s", editor::ToUtf8(asset.Name).c_str());
            ImGui::TextDisabled("%s", editor::ToUtf8(asset.RelativePath).c_str());
            ImGui::Spacing();
            ImGui::Text("Type: %s", folders[(int)asset.Kind]);
        }
    }
    else
    {
        ImGui::TextDisabled("Select an asset to preview details.");
    }
    ImGui::EndChild();
    ImGui::End();
}

void FEngine::DrawImGuiRenderSettings()
{
    if (!bShowImGuiSettings)
        return;
    ImGui::Begin("Render Settings", &bShowImGuiSettings);
    int path = (RenderPath == FSimpleSceneRenderer::ERenderPath::Deferred) ? 1 : 0;
    if (ImGui::Combo("Render Path", &path, "Forward\0Deferred\0"))
        RenderPath = (path == 1) ? FSimpleSceneRenderer::ERenderPath::Deferred : FSimpleSceneRenderer::ERenderPath::Forward;
    ImGui::Checkbox("Lumen", &bEnableLumen);
    ImGui::Checkbox("SWRT GI", &bEnableLumenSWRT);
    ImGui::Checkbox("HWRT GI", &bEnableLumenHWRT);
    ImGui::Checkbox("Tonemap", &bEnableTonemap);
    int tonemapIndex = TonemapOperatorToComboIndex(TonemapOperator);
    if (!bEnableTonemap)
        ImGui::BeginDisabled();
    if (ImGui::Combo("Tonemap Operator", &tonemapIndex, "Reinhard\0AgX\0ACES 1.0 RRT+ODT\0"))
        TonemapOperator = TonemapOperatorFromComboIndex(tonemapIndex);
    if (!bEnableTonemap)
        ImGui::EndDisabled();
    ImGui::End();
}

void FEngine::DrawImGuiMaterialEditor()
{
    if (!bShowImGuiMaterialEditor)
        return;
    if (EditingMaterialIndex < 0 || EditingMaterialIndex >= (int)Materials.size())
    {
        ImGui::Begin("Material Editor", &bShowImGuiMaterialEditor);
        ImGui::TextDisabled("Double-click or preview a material first.");
        ImGui::End();
        return;
    }

    FMaterialAsset& mat = Materials[(size_t)EditingMaterialIndex];
    ImGui::Begin("Material Editor", &bShowImGuiMaterialEditor);
    ImGui::TextUnformatted(editor::ToUtf8(mat.Name).c_str());
    ImGui::TextDisabled("Changes update the preview live. Save writes the material JSON.");
    ImGui::Separator();

    bool materialChanged = false;
    bool saveMaterial = false;

    auto clearInactiveTextureSlots = [&]()
    {
        if (mat.ShadingMode == EMaterialShadingMode::Unlit)
        {
            mat.NormalTexIndex = -1;
            mat.RoughnessTexIndex = -1;
            mat.MetallicTexIndex = -1;
            mat.AOTexIndex = -1;
            mat.NormalTexPath.clear();
            mat.RoughnessTexPath.clear();
            mat.MetallicTexPath.clear();
            mat.AOTexPath.clear();
            mat.NormalTexSlot = 1;
            mat.RoughnessTexSlot = 2;
            mat.MetallicTexSlot = 3;
            mat.AOTexSlot = 4;
        }
    };

    std::vector<std::pair<std::wstring, std::wstring>> textureOptions;
    auto hasTextureOption = [&](const std::wstring& relPath)
    {
        for (const auto& option : textureOptions)
            if (option.second == relPath)
                return true;
        return false;
    };
    for (const auto& asset : ContentAssets)
    {
        if (asset.Kind != editor::EAssetKind::Texture || asset.RelativePath.empty() || hasTextureOption(asset.RelativePath))
            continue;
        textureOptions.emplace_back(asset.Name + L"    " + asset.RelativePath, asset.RelativePath);
    }
    for (const auto& texture : Textures)
    {
        if (texture.RelativePath.empty() || hasTextureOption(texture.RelativePath))
            continue;
        textureOptions.emplace_back(texture.Name + L"    " + texture.RelativePath, texture.RelativePath);
    }

    auto currentTexturePath = [&](int textureIndex, const std::wstring& fallbackPath)
    {
        if (textureIndex >= 0 && textureIndex < (int)Textures.size())
            return Textures[(size_t)textureIndex].RelativePath;
        return fallbackPath;
    };

    auto setTextureSlot = [&](const std::wstring& relPath, int& textureIndex, int& rendererSlot, std::wstring& storedPath, int defaultSlot)
    {
        if (relPath.empty())
        {
            textureIndex = -1;
            rendererSlot = defaultSlot;
            storedPath.clear();
            return true;
        }

        const int loadedIndex = EnsureTextureLoaded(relPath);
        if (loadedIndex < 0 || loadedIndex >= (int)Textures.size())
            return false;

        textureIndex = loadedIndex;
        rendererSlot = Textures[(size_t)loadedIndex].RendererSlot;
        storedPath = Textures[(size_t)loadedIndex].RelativePath;
        return true;
    };

    int mode = MaterialShadingModeComboIndex(mat.ShadingMode);
    if (ImGui::BeginCombo("Shading Mode", editor::ToUtf8(MaterialShadingModeName(mat.ShadingMode)).c_str()))
    {
        for (int i = 0; i < (int)(sizeof(kMaterialSchemas) / sizeof(kMaterialSchemas[0])); ++i)
        {
            const bool selected = (mode == i);
            if (ImGui::Selectable(editor::ToUtf8(kMaterialSchemas[i].Name).c_str(), selected))
            {
                mode = i;
                const EMaterialShadingMode newMode = MaterialShadingModeFromComboIndex(i);
                if (mat.ShadingMode != newMode)
                {
                    mat.ShadingMode = newMode;
                    if (mat.ShadingMode == EMaterialShadingMode::Unlit)
                        mat.UnlitIntensity = 1.0f;
                    else if (mat.ShadingMode == EMaterialShadingMode::Rdr2Rock || mat.ShadingMode == EMaterialShadingMode::Rdr2Foliage)
                    {
                        mat.Roughness = (mat.ShadingMode == EMaterialShadingMode::Rdr2Foliage) ? 0.62f : 0.82f;
                        mat.Metallic = 0.0f;
                        mat.RockNormalStrength = (mat.ShadingMode == EMaterialShadingMode::Rdr2Foliage) ? 0.45f : 0.18f;
                        mat.RockBaseColorBoost = (mat.ShadingMode == EMaterialShadingMode::Rdr2Foliage) ? 1.0f : 1.25f;
                    }
                    else
                    {
                        mat.Roughness = 0.5f;
                        mat.Metallic = 0.0f;
                    }
                    clearInactiveTextureSlots();
                    materialChanged = true;
                }
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    const FMaterialSchema& schema = GetMaterialSchema(mat.ShadingMode);
    float color[3] = { mat.Albedo.x, mat.Albedo.y, mat.Albedo.z };
    if (ImGui::ColorEdit3("Color", color))
    {
        mat.Albedo = { color[0], color[1], color[2] };
        materialChanged = true;
    }
    if (mat.ShadingMode == EMaterialShadingMode::Unlit)
    {
        ImGui::DragFloat("Intensity", &mat.UnlitIntensity, 0.02f, 0.0f, 20.0f);
        if (ImGui::IsItemEdited())
            materialChanged = true;
    }
    else
    {
        if (ImGui::SliderFloat("Roughness", &mat.Roughness, 0.0f, 1.0f))
            materialChanged = true;
        if (mat.ShadingMode == EMaterialShadingMode::PbrLit)
            if (ImGui::SliderFloat("Metallic", &mat.Metallic, 0.0f, 1.0f))
                materialChanged = true;
        if (mat.ShadingMode == EMaterialShadingMode::Rdr2Rock || mat.ShadingMode == EMaterialShadingMode::Rdr2Foliage)
        {
            if (ImGui::SliderFloat("Normal Strength", &mat.RockNormalStrength, 0.0f, 1.0f))
                materialChanged = true;
            if (mat.ShadingMode == EMaterialShadingMode::Rdr2Rock
                && ImGui::DragFloat("Base Color Boost", &mat.RockBaseColorBoost, 0.01f, 0.0f, 8.0f))
                materialChanged = true;
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Texture Slots");
    if (textureOptions.empty())
        ImGui::TextDisabled("No texture assets found under Content/Textures.");

    auto drawTextureSlot = [&](int slotIndex, const wchar_t* slotLabel, int& textureIndex, int& rendererSlot, std::wstring& storedPath, int defaultSlot)
    {
        const std::wstring selectedPath = currentTexturePath(textureIndex, storedPath);
        std::string preview = "<None>";
        if (!selectedPath.empty())
            preview = editor::ToUtf8(std::filesystem::path(selectedPath).filename().wstring());

        const std::string label = editor::ToUtf8(slotLabel) + "##mat_tex_slot_" + std::to_string(slotIndex);
        if (ImGui::BeginCombo(label.c_str(), preview.c_str()))
        {
            const bool noneSelected = selectedPath.empty();
            if (ImGui::Selectable("<None>", noneSelected))
            {
                if (setTextureSlot(L"", textureIndex, rendererSlot, storedPath, defaultSlot))
                    materialChanged = true;
            }
            if (noneSelected)
                ImGui::SetItemDefaultFocus();
            ImGui::Separator();

            for (const auto& option : textureOptions)
            {
                const bool selected = (option.second == selectedPath);
                if (ImGui::Selectable(editor::ToUtf8(option.first).c_str(), selected))
                {
                    if (setTextureSlot(option.second, textureIndex, rendererSlot, storedPath, defaultSlot))
                        materialChanged = true;
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        const std::wstring resolvedPath = currentTexturePath(textureIndex, storedPath);
        if (!resolvedPath.empty())
        {
            ImGui::TextDisabled("%s", editor::ToUtf8(resolvedPath).c_str());
            if (textureIndex >= 0 && textureIndex < (int)Textures.size())
            {
                const auto& texture = Textures[(size_t)textureIndex];
                const ImVec4 avg(texture.AvgColor.x, texture.AvgColor.y, texture.AvgColor.z, 1.0f);
                ImGui::ColorButton(("Avg##slot_avg_" + std::to_string(slotIndex)).c_str(), avg, ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18));
                ImGui::SameLine();
                ImGui::TextDisabled("Renderer slot %d", rendererSlot);
            }
            else
            {
                ImGui::TextColored(ImVec4(0.95f, 0.48f, 0.32f, 1.0f), "Missing or not loaded");
            }
        }
    };

    drawTextureSlot(0, schema.TextureLabels[0], mat.AlbedoTexIndex, mat.AlbedoTexSlot, mat.AlbedoTexPath, 0);
    if (schema.TextureCount > 1)
        drawTextureSlot(1, schema.TextureLabels[1], mat.NormalTexIndex, mat.NormalTexSlot, mat.NormalTexPath, 1);
    if (schema.TextureCount > 2)
        drawTextureSlot(2, schema.TextureLabels[2], mat.RoughnessTexIndex, mat.RoughnessTexSlot, mat.RoughnessTexPath, 2);
    if (schema.TextureCount > 3)
        drawTextureSlot(3, schema.TextureLabels[3], mat.MetallicTexIndex, mat.MetallicTexSlot, mat.MetallicTexPath, 3);
    if (schema.TextureCount > 4)
        drawTextureSlot(4, schema.TextureLabels[4], mat.AOTexIndex, mat.AOTexSlot, mat.AOTexPath, 4);

    ImGui::Spacing();
    if (ImGui::Button("Apply"))
        materialChanged = true;
    ImGui::SameLine();
    if (ImGui::Button("Save Material"))
        saveMaterial = true;
    ImGui::SameLine();
    if (ImGui::Button("Refresh Textures"))
    {
        RefreshContentBrowser();
        materialChanged = true;
    }

    if (materialChanged || saveMaterial)
    {
        clearInactiveTextureSlots();
        UpdateMaterialRuntimeBindings(mat);
        for (auto& obj : Objects)
        {
            if (obj.MaterialIndex == EditingMaterialIndex)
                ApplyMaterialAssetToSceneObject(obj, EditingMaterialIndex);
        }
        if (bAssetPreviewActive && AssetPreviewMaterialIndex == EditingMaterialIndex)
            PreviewMaterialAsset(EditingMaterialIndex, false);
        MarkLevelDirty();
    }
    if (saveMaterial)
        SaveMaterialAsset(EditingMaterialIndex);
    ImGui::End();
}

void FEngine::DrawImGuiGizmo()
{
    if (SelectedIndex < 0 || SelectedIndex >= (int)Objects.size())
    {
        bImGuizmoHovered = false;
        bImGuizmoUsing = false;
        return;
    }
    if (ImGuiViewportW <= 1 || ImGuiViewportH <= 1)
    {
        bImGuizmoHovered = false;
        bImGuizmoUsing = false;
        return;
    }

    using namespace DirectX;
    FSceneObject& obj = Objects[(size_t)SelectedIndex];
    const XMMATRIX view = Camera.GetViewMatrix();
    const float aspect = (float)ImGuiViewportW / (float)std::max(1, ImGuiViewportH);
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, 0.1f, 100.0f);
    const XMMATRIX world = XMMatrixScaling(obj.Scale.x, obj.Scale.y, obj.Scale.z) * XMMatrixTranslation(obj.Position.x, obj.Position.y, obj.Position.z);
    XMFLOAT4X4 viewM{}, projM{}, worldM{};
    XMStoreFloat4x4(&viewM, view);
    XMStoreFloat4x4(&projM, proj);
    XMStoreFloat4x4(&worldM, world);
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect((float)ImGuiViewportScreenX, (float)ImGuiViewportScreenY, (float)ImGuiViewportW, (float)ImGuiViewportH);
    const ImGuizmo::OPERATION op = (GizmoMode == EGizmoMode::Scale) ? ImGuizmo::SCALE : ImGuizmo::TRANSLATE;
    if (ImGuizmo::Manipulate(&viewM._11, &projM._11, op, ImGuizmo::WORLD, &worldM._11) && !bDragging)
    {
        float tr[3]{}, rot[3]{}, sc[3]{};
        ImGuizmo::DecomposeMatrixToComponents(&worldM._11, tr, rot, sc);
        obj.Position = { tr[0], tr[1], tr[2] };
        obj.Scale = { std::max(0.01f, sc[0]), std::max(0.01f, sc[1]), std::max(0.01f, sc[2]) };
        MarkLevelDirty();
    }
    bImGuizmoHovered = ImGuizmo::IsOver(op);
    bImGuizmoUsing = ImGuizmo::IsUsing();
}

void FEngine::DrawImGuiEditor()
{
    if (!bImGuiInitialized)
        return;

    DrawImGuiMainMenu();

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_MenuBar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("ShellEngineDockspace", nullptr, hostFlags);
    ImGui::PopStyleVar(2);
    const ImGuiID dockspaceId = ImGui::GetID("ShellEngineDockspaceID");
    if (!ImGui::DockBuilderGetNode(dockspaceId))
    {
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);
        ImGuiID dockMain = dockspaceId;
        ImGuiID dockLeft = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.18f, nullptr, &dockMain);
        ImGuiID dockRight = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.22f, nullptr, &dockMain);
        ImGuiID dockBottom = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, 0.28f, nullptr, &dockMain);
        ImGui::DockBuilderDockWindow("Place Actors", dockLeft);
        ImGui::DockBuilderDockWindow("Viewport", dockMain);
        ImGui::DockBuilderDockWindow("Level Outliner", dockRight);
        ImGui::DockBuilderDockWindow("Details", dockRight);
        ImGui::DockBuilderDockWindow("Content Drawer", dockBottom);
        ImGui::DockBuilderFinish(dockspaceId);
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_DockingEmptyBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::PopStyleColor(2);
    ImGui::End();

    DrawImGuiPlaceActors();
    DrawImGuiViewport();
    DrawImGuiOutliner();
    DrawImGuiDetails();
    DrawImGuiContentDrawer();
    DrawImGuiRenderSettings();
    DrawImGuiMaterialEditor();
}

void FEngine::RenderImGuiDrawData(ID3D12GraphicsCommandList* cmd)
{
    if (!bImGuiInitialized || !cmd)
        return;
    ID3D12DescriptorHeap* heaps[] = { ImGuiSrvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd);
}

void FEngine::HideLegacyWin32EditorControls()
{
    HWND controls[] = {
        ToolbarPanel, ToolbarTitle, ToolbarNewLevelBtn, ToolbarOpenLevelBtn, ToolbarSaveLevelBtn, ToolbarImportObjBtn, ToolbarPlaceBtn, ToolbarSettingsBtn, StatusLabel,
        EngineNameLabel, SidebarToggleBtn, SidebarSearchEdit, SidebarList, SidebarBasicLabel, SidebarRenderDocLabel,
        RenderPathLabel, RenderSettingsLabel, RenderGILabel, SunSectionLabel, AtmosphereSectionLabel, RenderPathCombo, LumenCheckbox, LumenSWRTCheckbox, LumenHWRTCheckbox,
        TonemapOperatorLabel, TonemapOperatorCombo,
        SkyEnableCheckbox, SunYawLabel, SunYawValueLabel, SunYawSlider, SunPitchLabel, SunPitchValueLabel, SunPitchSlider, SunIntensityLabel, SunIntensityValueLabel, SunIntensitySlider,
        RayleighLabel, RayleighValueLabel, RayleighSlider, MieLabel, MieValueLabel, MieSlider, MieGLabel, MieGValueLabel, MieGSlider, AtmoHeightLabel, AtmoHeightValueLabel, AtmoHeightSlider, SkyLabel,
        BottomPanel, ContentTitleLabel, ContentDrawerToggleBtn, TextureTitleLabel, PreviewTitleLabel, MaterialTitleLabel, ContentPathLabel, ContentActionsLabel, ContentFoldersList, ContentHintLabel,
        ContentList, ImportObjBtn, PlaceAssetBtn, NewLevelBtn, OpenLevelBtn, SaveLevelBtn, TextureList, MaterialList, TexturePreview, NewMaterialBtn, TonemapCheckbox,
        RightPanel, OutlinerLabel, OutlinerList, DetailsLabel, DetailTransformLabel, DetailEnvironmentLabel, DetailNameLabel, DetailPositionLabel, DetailScaleLabel,
        DetailNameEdit, DetailPosXEdit, DetailPosYEdit, DetailPosZEdit, DetailScaleXEdit, DetailScaleYEdit, DetailScaleZEdit, DetailIntensityLabel, DetailIntensityEdit,
        DetailSkyEnabledCheckbox, DetailRayleighLabel, DetailRayleighEdit, DetailMieLabel, DetailMieEdit, DetailMieGLabel, DetailMieGEdit, DetailAtmosphereHeightLabel, DetailAtmosphereHeightEdit, ApplyDetailsBtn
    };
    for (HWND h : controls)
        if (h) ShowWindow(h, SW_HIDE);
}

void FEngine::OpenRenderSettingsDialog()
{
    if (bUseImGuiEditor)
    {
        bShowImGuiSettings = true;
        return;
    }
    if (!RenderSettingsHwnd)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &FEngine::RenderSettingsWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"DX12RenderSettingsWindow";
        RegisterClassExW(&wc);

        RenderSettingsHwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            wc.lpszClassName,
            L"Render Settings",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            390,
            320,
            Window.GetHwnd(),
            nullptr,
            wc.hInstance,
            this);
    }
    if (!RenderSettingsHwnd)
        return;
    SetWindowTextW(RenderSettingsHwnd, L"Render Settings");

    HWND controls[] = {
        RenderSettingsLabel, RenderPathLabel, RenderPathCombo, RenderGILabel,
        LumenCheckbox, LumenSWRTCheckbox, LumenHWRTCheckbox, TonemapCheckbox,
        TonemapOperatorLabel, TonemapOperatorCombo
    };
    for (HWND h : controls)
    {
        if (!h) continue;
        SetParent(h, RenderSettingsHwnd);
        ApplyEditorFont(h, h == RenderSettingsLabel || h == RenderGILabel);
        ShowWindow(h, SW_SHOW);
    }

    if (RenderSettingsLabel)
        SetWindowTextW(RenderSettingsLabel, L"Render Settings");
    SyncRenderSettingsControls();
    LayoutRenderSettingsDialog();
    ShowWindow(RenderSettingsHwnd, SW_SHOW);
    SetForegroundWindow(RenderSettingsHwnd);
}

void FEngine::LayoutRenderSettingsDialog()
{
    if (!RenderSettingsHwnd)
        return;

    RECT rc{};
    GetClientRect(RenderSettingsHwnd, &rc);
    const int w = (int)(rc.right - rc.left);
    const int pad = 14;
    const int innerW = std::max(120, w - pad * 2);
    int y = 12;

    if (RenderSettingsLabel)
        MoveWindow(RenderSettingsLabel, pad, y, innerW, 22, TRUE);
    y += 34;

    if (RenderPathLabel)
        MoveWindow(RenderPathLabel, pad, y + 4, 88, 18, TRUE);
    if (RenderPathCombo)
        MoveWindow(RenderPathCombo, pad + 96, y, std::max(120, innerW - 96), 180, TRUE);
    y += 34;

    if (RenderGILabel)
        MoveWindow(RenderGILabel, pad, y, innerW, 22, TRUE);
    y += 30;

    if (LumenCheckbox)
        MoveWindow(LumenCheckbox, pad, y, innerW, 22, TRUE);
    y += 26;
    if (LumenSWRTCheckbox)
        MoveWindow(LumenSWRTCheckbox, pad, y, innerW, 22, TRUE);
    y += 26;
    if (LumenHWRTCheckbox)
        MoveWindow(LumenHWRTCheckbox, pad, y, innerW, 22, TRUE);
    y += 32;

    if (TonemapCheckbox)
        MoveWindow(TonemapCheckbox, pad, y, innerW, 22, TRUE);
    y += 30;

    if (TonemapOperatorLabel)
        MoveWindow(TonemapOperatorLabel, pad, y + 4, 122, 18, TRUE);
    if (TonemapOperatorCombo)
        MoveWindow(TonemapOperatorCombo, pad + 130, y, std::max(120, innerW - 130), 120, TRUE);
}

void FEngine::SyncRenderSettingsControls()
{
    if (RenderPathCombo)
        SendMessageW(RenderPathCombo, CB_SETCURSEL, (WPARAM)((RenderPath == FSimpleSceneRenderer::ERenderPath::Deferred) ? 1 : 0), 0);
    if (LumenCheckbox)
        SendMessageW(LumenCheckbox, BM_SETCHECK, bEnableLumen ? BST_CHECKED : BST_UNCHECKED, 0);
    if (LumenSWRTCheckbox)
        SendMessageW(LumenSWRTCheckbox, BM_SETCHECK, bEnableLumenSWRT ? BST_CHECKED : BST_UNCHECKED, 0);
    if (LumenHWRTCheckbox)
        SendMessageW(LumenHWRTCheckbox, BM_SETCHECK, bEnableLumenHWRT ? BST_CHECKED : BST_UNCHECKED, 0);
    if (TonemapCheckbox)
        SendMessageW(TonemapCheckbox, BM_SETCHECK, bEnableTonemap ? BST_CHECKED : BST_UNCHECKED, 0);
    if (TonemapOperatorCombo)
    {
        SendMessageW(TonemapOperatorCombo, CB_SETCURSEL, (WPARAM)TonemapOperatorToComboIndex(TonemapOperator), 0);
        EnableWindow(TonemapOperatorCombo, bEnableTonemap ? TRUE : FALSE);
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
    if (bUseImGuiEditor)
    {
        HideLegacyWin32EditorControls();
        if (ViewportHwnd)
        {
            SetWindowPos(
                ViewportHwnd,
                nullptr,
                0,
                0,
                std::max(1, clientW),
                std::max(1, clientH),
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
            ShowWindow(ViewportHwnd, SW_SHOW);
        }
        return;
    }
    const int bottomPanelH = bContentDrawerOpen ? ((clientH < 850) ? 160 : BottomPanelHeightPx) : 38;
    const int collapsedSidebarW = 42;
    const int leftPanelW = bPlaceActorsOpen ? SidebarWidthPx : collapsedSidebarW;
    const int viewportW = std::max(1, clientW - leftPanelW - RightPanelWidthPx);
    const int viewportH = std::max(1, clientH - TopToolbarHeightPx - bottomPanelH);

    // 顶部标题与物体列表区域。
    if (ToolbarPanel)
        MoveWindow(ToolbarPanel, 0, 0, clientW, TopToolbarHeightPx, TRUE);
    if (ToolbarTitle)
        MoveWindow(ToolbarTitle, 10, 7, 190, 22, TRUE);
    int tbX = 210;
    auto toolbarButton = [&](HWND h, int width)
    {
        if (!h) return;
        MoveWindow(h, tbX, 6, width, 24, TRUE);
        tbX += width + 6;
    };
    toolbarButton(ToolbarNewLevelBtn, 72);
    toolbarButton(ToolbarOpenLevelBtn, 76);
    toolbarButton(ToolbarSaveLevelBtn, 76);
    toolbarButton(ToolbarImportObjBtn, 92);
    toolbarButton(ToolbarPlaceBtn, 72);
    toolbarButton(ToolbarSettingsBtn, 82);
    if (StatusLabel)
        MoveWindow(StatusLabel, tbX + 10, 8, std::max(120, clientW - tbX - 18), 20, TRUE);

    const int contentTop = TopToolbarHeightPx;

    const int sidePad = 10;
    const int innerX = sidePad;
    const int innerW = std::max(1, leftPanelW - sidePad * 2);
    const int headerH = 20;
    const int rowH = 24;
    int sy = 8;

    auto placeSide = [&](HWND h, int y, int w, int hgt)
    {
        if (!h) return;
        MoveWindow(h, innerX, contentTop + y, w, hgt, TRUE);
    };
    auto placeSliderRow = [&](HWND label, HWND value, HWND slider, int y)
    {
        const int labelW = 110;
        const int valueW = 48;
        const int sliderW = std::max(60, innerW - labelW - valueW - 8);
        placeSide(label, y + 4, labelW, 16);
        if (value)
            MoveWindow(value, innerX + innerW - valueW, contentTop + y + 4, valueW, 16, TRUE);
        if (slider)
            MoveWindow(slider, innerX + labelW, contentTop + y, sliderW, 24, TRUE);
    };

    if (EngineNameLabel)
    {
        SetWindowTextW(EngineNameLabel, bPlaceActorsOpen ? L"Place Actors" : L"");
        placeSide(EngineNameLabel, sy, bPlaceActorsOpen ? std::max(1, innerW - 36) : 1, headerH);
    }
    if (SidebarToggleBtn)
    {
        SetWindowTextW(SidebarToggleBtn, bPlaceActorsOpen ? L"<" : L">");
        const int toggleX = bPlaceActorsOpen ? std::max(8, leftPanelW - sidePad - 28) : 8;
        MoveWindow(SidebarToggleBtn, toggleX, contentTop + sy - 1, 28, 22, TRUE);
        ShowWindow(SidebarToggleBtn, SW_SHOW);
    }
    sy += headerH + 6;

    HWND sidebarBody[] = { SidebarSearchEdit, SidebarBasicLabel, SidebarList };
    for (HWND h : sidebarBody)
        if (h) ShowWindow(h, bPlaceActorsOpen ? SW_SHOW : SW_HIDE);
    if (SidebarRenderDocLabel)
        ShowWindow(SidebarRenderDocLabel, SW_HIDE);
    if (bPlaceActorsOpen)
    {
        placeSide(SidebarSearchEdit, sy, innerW, 22);
        sy += 28;
        placeSide(SidebarBasicLabel, sy, innerW, 18);
        sy += 20;
        placeSide(SidebarList, sy, innerW, std::max(156, viewportH - sy - 12));
    }

    // Sidebar controls live below the palette list (in main window coordinates).
    if (!RenderSettingsHwnd && bPlaceActorsOpen)
    {
    const int baseY = sy + 86;

    // 统一摆放侧栏控件。
    int cy = baseY;
    placeSide(RenderSettingsLabel, cy, innerW, headerH);
    cy += headerH + 2;
    placeSide(RenderPathLabel, cy, 86, 16);
    if (RenderPathCombo)
        MoveWindow(RenderPathCombo, innerX + 92, contentTop + cy - 2, innerW - 92, 210, TRUE);
    cy += 22;
    placeSide(RenderGILabel, cy, innerW, 16);
    cy += 16;
    placeSide(LumenCheckbox, cy, innerW, 16);
    cy += 16;
    placeSide(LumenSWRTCheckbox, cy, innerW, 16);
    cy += 16;
    placeSide(LumenHWRTCheckbox, cy, innerW, 16);
    cy += 18;

    placeSide(SunSectionLabel, cy, innerW, headerH);
    cy += headerH + 2;
    placeSide(SkyEnableCheckbox, cy, innerW, 16);
    cy += 18;
    placeSliderRow(SunYawLabel, SunYawValueLabel, SunYawSlider, cy);
    cy += rowH;
    placeSliderRow(SunPitchLabel, SunPitchValueLabel, SunPitchSlider, cy);
    cy += rowH;
    placeSliderRow(SunIntensityLabel, SunIntensityValueLabel, SunIntensitySlider, cy);
    cy += rowH + 2;

    placeSide(AtmosphereSectionLabel, cy, innerW, headerH);
    cy += headerH + 2;
    placeSliderRow(RayleighLabel, RayleighValueLabel, RayleighSlider, cy);
    cy += rowH;
    placeSliderRow(MieLabel, MieValueLabel, MieSlider, cy);
    cy += rowH;
    placeSliderRow(MieGLabel, MieGValueLabel, MieGSlider, cy);
    cy += rowH;
    placeSliderRow(AtmoHeightLabel, AtmoHeightValueLabel, AtmoHeightSlider, cy);
    cy += rowH + 2;
    placeSide(SkyLabel, cy, innerW, 14);
    }

    // 视口与底部面板布局。
    if (ViewportHwnd)
    {
        SetWindowPos(
            ViewportHwnd,
            nullptr,
            leftPanelW,
            contentTop,
            viewportW,
            viewportH,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    }

    const int rightX = leftPanelW + viewportW;
    if (RightPanel)
    {
        MoveWindow(RightPanel, rightX, contentTop, RightPanelWidthPx, viewportH, TRUE);
        // The right panel controls are siblings of the D3D viewport. Keeping a
        // separate STATIC background visible here can cover the outliner/details
        // controls after large-window relayouts, so the main window background is
        // used as the panel surface instead.
        ShowWindow(RightPanel, SW_HIDE);
    }
    if (OutlinerLabel)
        MoveWindow(OutlinerLabel, rightX + 8, contentTop + 8, RightPanelWidthPx - 16, 18, TRUE);
    const int outlinerH = std::clamp(viewportH / 4, 108, 190);
    if (OutlinerList)
        MoveWindow(OutlinerList, rightX + 8, contentTop + 30, RightPanelWidthPx - 16, outlinerH, TRUE);
    const int detailsY = 30 + outlinerH + 12;
    if (DetailsLabel)
        MoveWindow(DetailsLabel, rightX + 8, contentTop + detailsY, RightPanelWidthPx - 16, 18, TRUE);
    if (DetailNameLabel)
        MoveWindow(DetailNameLabel, rightX + 8, contentTop + detailsY + 24, RightPanelWidthPx - 16, 16, TRUE);
    if (DetailNameEdit)
        MoveWindow(DetailNameEdit, rightX + 8, contentTop + detailsY + 42, RightPanelWidthPx - 16, 22, TRUE);
    if (DetailTransformLabel)
        MoveWindow(DetailTransformLabel, rightX + 8, contentTop + detailsY + 70, RightPanelWidthPx - 16, 18, TRUE);
    if (DetailPositionLabel)
        MoveWindow(DetailPositionLabel, rightX + 8, contentTop + detailsY + 94, RightPanelWidthPx - 16, 16, TRUE);
    const int editW = (RightPanelWidthPx - 32) / 3;
    if (DetailPosXEdit)
        MoveWindow(DetailPosXEdit, rightX + 8, contentTop + detailsY + 112, editW, 22, TRUE);
    if (DetailPosYEdit)
        MoveWindow(DetailPosYEdit, rightX + 12 + editW, contentTop + detailsY + 112, editW, 22, TRUE);
    if (DetailPosZEdit)
        MoveWindow(DetailPosZEdit, rightX + 16 + editW * 2, contentTop + detailsY + 112, editW, 22, TRUE);
    if (DetailScaleLabel)
        MoveWindow(DetailScaleLabel, rightX + 8, contentTop + detailsY + 138, RightPanelWidthPx - 16, 16, TRUE);
    if (DetailScaleXEdit)
        MoveWindow(DetailScaleXEdit, rightX + 8, contentTop + detailsY + 156, editW, 22, TRUE);
    if (DetailScaleYEdit)
        MoveWindow(DetailScaleYEdit, rightX + 12 + editW, contentTop + detailsY + 156, editW, 22, TRUE);
    if (DetailScaleZEdit)
        MoveWindow(DetailScaleZEdit, rightX + 16 + editW * 2, contentTop + detailsY + 156, editW, 22, TRUE);
    int applyY = detailsY + 186;
    const int fullW = RightPanelWidthPx - 16;
    const int fieldX = rightX + 8;
    const int extraW = fullW;
    const bool showSunDetails = SelectedIndex >= 0 && SelectedIndex < (int)Objects.size() && Objects[(size_t)SelectedIndex].Type == FSceneObject::EType::SunLight;
    const bool showSkyDetails = SelectedIndex >= 0 && SelectedIndex < (int)Objects.size() && Objects[(size_t)SelectedIndex].Type == FSceneObject::EType::SkyAtmosphere;
    if (showSunDetails)
    {
        if (DetailEnvironmentLabel)
            MoveWindow(DetailEnvironmentLabel, fieldX, contentTop + applyY, extraW, 18, TRUE);
        applyY += 24;
        if (DetailIntensityLabel)
            MoveWindow(DetailIntensityLabel, fieldX, contentTop + applyY, extraW, 16, TRUE);
        if (DetailIntensityEdit)
            MoveWindow(DetailIntensityEdit, fieldX, contentTop + applyY + 18, extraW, 22, TRUE);
        applyY += 48;
    }
    if (showSkyDetails)
    {
        if (DetailEnvironmentLabel)
            MoveWindow(DetailEnvironmentLabel, fieldX, contentTop + applyY, extraW, 18, TRUE);
        applyY += 24;
        if (DetailSkyEnabledCheckbox)
            MoveWindow(DetailSkyEnabledCheckbox, fieldX, contentTop + applyY, extraW, 22, TRUE);
        applyY += 28;
        const int colGap = 8;
        const int colW = (extraW - colGap) / 2;
        auto placeDetailGridRow = [&](HWND leftLabel, HWND leftEdit, HWND rightLabel, HWND rightEdit)
        {
            if (leftLabel)
                MoveWindow(leftLabel, fieldX, contentTop + applyY, colW, 16, TRUE);
            if (rightLabel)
                MoveWindow(rightLabel, fieldX + colW + colGap, contentTop + applyY, colW, 16, TRUE);
            if (leftEdit)
                MoveWindow(leftEdit, fieldX, contentTop + applyY + 18, colW, 22, TRUE);
            if (rightEdit)
                MoveWindow(rightEdit, fieldX + colW + colGap, contentTop + applyY + 18, colW, 22, TRUE);
            applyY += 46;
        };
        placeDetailGridRow(DetailRayleighLabel, DetailRayleighEdit, DetailMieLabel, DetailMieEdit);
        placeDetailGridRow(DetailMieGLabel, DetailMieGEdit, DetailAtmosphereHeightLabel, DetailAtmosphereHeightEdit);
    }
    if (ApplyDetailsBtn)
        MoveWindow(ApplyDetailsBtn, rightX + 8, contentTop + applyY, RightPanelWidthPx - 16, 24, TRUE);

    if (BottomPanel)
        MoveWindow(BottomPanel, 0, contentTop + viewportH, clientW, bottomPanelH, TRUE);

    if (BottomPanel)
    {
        // 计算底栏三列布局并更新控件位置。
        const int padding = 10;
        const int headerH2 = 34;
        const int buttonH = 26;
        const int bodyY = headerH2 + 8;
        const int bodyH = std::max(80, bottomPanelH - bodyY - padding);
        const int bodyW = std::max(320, clientW - padding * 2);
        const int foldersW = 180;
        const int previewW = std::clamp(bodyW / 4, 280, 380);
        const int gap = 10;
        const int assetsW = std::max(220, bodyW - foldersW - previewW - gap * 2);
        const int foldersX = padding;
        const int assetsX = foldersX + foldersW + gap;
        const int previewX = assetsX + assetsW + gap;

        if (ContentTitleLabel)
            MoveWindow(ContentTitleLabel, padding, 8, 148, 22, TRUE);
        if (ContentDrawerToggleBtn)
        {
            SetWindowTextW(ContentDrawerToggleBtn, bContentDrawerOpen ? L"Collapse" : L"Content");
            MoveWindow(ContentDrawerToggleBtn, padding + 156, 7, 86, buttonH, TRUE);
        }

        HWND contentDrawerBody[] = {
            TextureTitleLabel, PreviewTitleLabel, MaterialTitleLabel,
            ContentActionsLabel, ContentFoldersList, ContentHintLabel, ContentList,
            ImportObjBtn, PlaceAssetBtn, NewLevelBtn, OpenLevelBtn, SaveLevelBtn,
            TextureList, MaterialList, TexturePreview, NewMaterialBtn
        };
        for (HWND h : contentDrawerBody)
            if (h) ShowWindow(h, bContentDrawerOpen ? SW_SHOW : SW_HIDE);
        if (TonemapCheckbox && GetParent(TonemapCheckbox) == BottomPanel)
            ShowWindow(TonemapCheckbox, SW_HIDE);

        if (!bContentDrawerOpen)
        {
            if (ContentPathLabel)
                MoveWindow(ContentPathLabel, padding + 252, 10, std::max(80, clientW - padding - 252), 18, TRUE);
        }
        else
        {

        const int actionsW = 58 + 58 + 74 + 112 + 72 + 104 + 6 * 5;
        const int actionLeft = clientW - padding - actionsW;
        const int pathX = padding + 252;
        const int pathW = std::max(80, actionLeft - pathX - 10);
        if (ContentPathLabel)
            MoveWindow(ContentPathLabel, pathX, 10, pathW, 18, TRUE);
        if (TonemapCheckbox && GetParent(TonemapCheckbox) == BottomPanel)
            MoveWindow(TonemapCheckbox, pathX + pathW + 10, 8, 96, buttonH, TRUE);

        int actionX = clientW - padding;
        auto rightButton = [&](HWND h, int width)
        {
            if (!h) return;
            actionX -= width;
            MoveWindow(h, actionX, 7, width, buttonH, TRUE);
            actionX -= 6;
        };
        rightButton(SaveLevelBtn, 58);
        rightButton(OpenLevelBtn, 58);
        rightButton(NewLevelBtn, 74);
        rightButton(NewMaterialBtn, 112);
        rightButton(PlaceAssetBtn, 72);
        rightButton(ImportObjBtn, 104);

        if (TextureTitleLabel)
            MoveWindow(TextureTitleLabel, foldersX, bodyY, foldersW, 20, TRUE);
        if (ContentFoldersList)
            MoveWindow(ContentFoldersList, foldersX, bodyY + 24, foldersW, bodyH - 24, TRUE);
        if (MaterialTitleLabel)
            MoveWindow(MaterialTitleLabel, assetsX, bodyY, assetsW, 20, TRUE);
        if (ContentList)
            MoveWindow(ContentList, assetsX, bodyY + 24, assetsW, bodyH - 46, TRUE);
        if (ContentHintLabel)
            MoveWindow(ContentHintLabel, assetsX, bodyY + bodyH - 18, assetsW, 18, TRUE);
        if (PreviewTitleLabel)
            MoveWindow(PreviewTitleLabel, previewX, bodyY, previewW, 20, TRUE);
        const int previewH = std::max(82, bodyH / 2);
        if (TexturePreview)
            MoveWindow(TexturePreview, previewX, bodyY + 24, previewW, previewH - 24, TRUE);
        if (ContentActionsLabel)
            MoveWindow(ContentActionsLabel, previewX, bodyY + previewH + 6, previewW, 18, TRUE);
        const int loadedY = bodyY + previewH + 28;
        const int loadedH = std::max(40, bodyH - previewH - 28);
        if (TextureList)
            MoveWindow(TextureList, previewX, loadedY, previewW / 2 - 4, loadedH, TRUE);
        if (MaterialList)
            MoveWindow(MaterialList, previewX + previewW / 2 + 4, loadedY, previewW / 2 - 4, loadedH, TRUE);
        }
    }

    // 同步 RHI 尺寸。
    // Do not synchronously resize the D3D12 swapchain from Win32 UI handlers.
    // Some editor-only layout actions can otherwise block the message pump in DXGI.

    // Keep resize handling cheap. The editor panels and swapchain child are laid
    // out into non-overlapping rectangles, so WM_SIZE should not churn child
    // z-order. Reordering many Win32 controls during resize can block with a
    // flip-model D3D12 child window on some drivers.

    // 将侧栏与底栏控件保持在顶层。
    auto bringTop = [](HWND h)
    {
        (void)h;
    };

    bringTop(ToolbarPanel);
    bringTop(ToolbarTitle);
    bringTop(ToolbarNewLevelBtn);
    bringTop(ToolbarOpenLevelBtn);
    bringTop(ToolbarSaveLevelBtn);
    bringTop(ToolbarImportObjBtn);
    bringTop(ToolbarPlaceBtn);
    bringTop(ToolbarSettingsBtn);
    bringTop(StatusLabel);
    bringTop(SidebarList);
    bringTop(EngineNameLabel);
    bringTop(SidebarToggleBtn);
    bringTop(SidebarSearchEdit);
    bringTop(SidebarBasicLabel);
    bringTop(SidebarRenderDocLabel);
    bringTop(RenderSettingsLabel);
    bringTop(RenderPathLabel);
    bringTop(RenderPathCombo);
    bringTop(RenderGILabel);
    bringTop(LumenCheckbox);
    bringTop(LumenSWRTCheckbox);
    bringTop(LumenHWRTCheckbox);
    bringTop(TonemapOperatorLabel);
    bringTop(TonemapOperatorCombo);
    bringTop(SunSectionLabel);
    bringTop(AtmosphereSectionLabel);
    bringTop(SkyEnableCheckbox);
    bringTop(SunYawLabel);
    bringTop(SunYawValueLabel);
    bringTop(SunYawSlider);
    bringTop(SunPitchLabel);
    bringTop(SunPitchValueLabel);
    bringTop(SunPitchSlider);
    bringTop(SunIntensityLabel);
    bringTop(SunIntensityValueLabel);
    bringTop(SunIntensitySlider);
    bringTop(RayleighLabel);
    bringTop(RayleighValueLabel);
    bringTop(RayleighSlider);
    bringTop(MieLabel);
    bringTop(MieValueLabel);
    bringTop(MieSlider);
    bringTop(MieGLabel);
    bringTop(MieGValueLabel);
    bringTop(MieGSlider);
    bringTop(AtmoHeightLabel);
    bringTop(AtmoHeightValueLabel);
    bringTop(AtmoHeightSlider);
    bringTop(SkyLabel);
    bringTop(OutlinerLabel);
    bringTop(OutlinerList);
    bringTop(DetailsLabel);
    bringTop(DetailTransformLabel);
    bringTop(DetailEnvironmentLabel);
    bringTop(DetailNameLabel);
    bringTop(DetailPositionLabel);
    bringTop(DetailScaleLabel);
    bringTop(DetailNameEdit);
    bringTop(DetailPosXEdit);
    bringTop(DetailPosYEdit);
    bringTop(DetailPosZEdit);
    bringTop(DetailScaleXEdit);
    bringTop(DetailScaleYEdit);
    bringTop(DetailScaleZEdit);
    bringTop(DetailIntensityLabel);
    bringTop(DetailIntensityEdit);
    bringTop(DetailSkyEnabledCheckbox);
    bringTop(DetailRayleighLabel);
    bringTop(DetailRayleighEdit);
    bringTop(DetailMieLabel);
    bringTop(DetailMieEdit);
    bringTop(DetailMieGLabel);
    bringTop(DetailMieGEdit);
    bringTop(DetailAtmosphereHeightLabel);
    bringTop(DetailAtmosphereHeightEdit);
    bringTop(ApplyDetailsBtn);
    bringTop(BottomPanel);
    bringTop(ContentTitleLabel);
    bringTop(ContentDrawerToggleBtn);
    bringTop(TextureTitleLabel);
    bringTop(PreviewTitleLabel);
    bringTop(MaterialTitleLabel);
    bringTop(ContentPathLabel);
    bringTop(ContentActionsLabel);
    bringTop(ContentFoldersList);
    bringTop(ContentList);
    bringTop(TextureList);
    bringTop(TexturePreview);
    bringTop(MaterialList);
    bringTop(ContentHintLabel);
    bringTop(ImportObjBtn);
    bringTop(PlaceAssetBtn);
    bringTop(NewMaterialBtn);
    bringTop(TonemapCheckbox);
    bringTop(NewLevelBtn);
    bringTop(OpenLevelBtn);
    bringTop(SaveLevelBtn);
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

    auto setValue = [](HWND h, const wchar_t* fmt, float v)
    {
        if (!h) return;
        wchar_t value[64];
        swprintf_s(value, fmt, v);
        SetWindowTextW(h, value);
    };

    const float yawDeg = SunYaw * 180.0f / DirectX::XM_PI;
    const float pitchDeg = SunPitch * 180.0f / DirectX::XM_PI;
    setValue(SunYawValueLabel, L"%.1f", yawDeg);
    setValue(SunPitchValueLabel, L"%.1f", pitchDeg);
    setValue(SunIntensityValueLabel, L"%.2f", SunIntensity);
    setValue(RayleighValueLabel, L"%.2f", SkySettings.RayleighScale);
    setValue(MieValueLabel, L"%.2f", SkySettings.MieScale);
    setValue(MieGValueLabel, L"%.2f", SkySettings.MieG);
    setValue(AtmoHeightValueLabel, L"%.2f", SkySettings.AtmosphereHeight);

    // 更新状态文本。
    if (SkyLabel)
    {
        wchar_t buf[256];
        swprintf_s(buf, L"Sky %s    Direction %.1f / %.1f    Intensity %.2f",
                   SkySettings.Enable ? L"On" : L"Off", yawDeg, pitchDeg, SunIntensity);
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
    SyncViewportBackbufferSize();

    if (SidebarSearchEdit)
    {
        wchar_t buf[128]{};
        GetWindowTextW(SidebarSearchEdit, buf, (int)_countof(buf));
        if (ActorPaletteFilter != buf)
        {
            ActorPaletteFilter = buf;
            RefreshActorPalette();
        }
    }

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
    if (Input.Keys['J'] || Input.Keys['L'] || Input.Keys['I'] || Input.Keys['K'])
    {
        float yaw = SunYaw;
        float pitch = SunPitch;
        float distance = 10.0f;
        if (FSceneObject* sun = FindActiveSunLight())
        {
            SunPositionToAngles(sun->Position, yaw, pitch);
            const XMVECTOR p = XMLoadFloat3(&sun->Position);
            distance = std::max(0.5f, XMVectorGetX(XMVector3Length(p)));
            if (Input.Keys['J']) yaw -= sunSpeed;
            if (Input.Keys['L']) yaw += sunSpeed;
            if (Input.Keys['I']) pitch += sunSpeed;
            if (Input.Keys['K']) pitch -= sunSpeed;
            pitch = Clamp(pitch, -DirectX::XM_PIDIV2 + 0.05f, DirectX::XM_PIDIV2 - 0.05f);
            sun->Position = DirectionToSunPosition(yaw, pitch, distance);
            MarkLevelDirty();
            if (SelectedIndex >= 0 && SelectedIndex < (int)Objects.size() && &Objects[(size_t)SelectedIndex] == sun)
                RefreshDetailsPanel();
        }
        else
        {
            if (Input.Keys['J']) SunYaw -= sunSpeed;
            if (Input.Keys['L']) SunYaw += sunSpeed;
            if (Input.Keys['I']) SunPitch += sunSpeed;
            if (Input.Keys['K']) SunPitch -= sunSpeed;
            SunPitch = Clamp(SunPitch, -DirectX::XM_PIDIV2 + 0.05f, DirectX::XM_PIDIV2 - 0.05f);
            UpdateSkyUI();
        }
    }

    // 视口内的选取/放置/Gizmo 拖拽（左键）。
    // Selection + placement + gizmo drag (left mouse) in viewport
    const uint32 w = bUseImGuiEditor ? (uint32)std::max(1, ImGuiViewportW) : RHI.GetWidth();
    const uint32 h = bUseImGuiEditor ? (uint32)std::max(1, ImGuiViewportH) : RHI.GetHeight();
    if (bUseImGuiEditor && ViewportHwnd)
    {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(ViewportHwnd, &pt);
        MouseX = pt.x - ImGuiViewportX;
        MouseY = pt.y - ImGuiViewportY;
    }
    const float aspect = (h == 0) ? 1.0f : (float)w / (float)h;
    const XMMATRIX view = Camera.GetViewMatrix();
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, 0.1f, 100.0f);
    const XMMATRIX viewProj = view * proj;
    const XMMATRIX invViewProj = XMMatrixInverse(nullptr, viewProj);

    static bool prevLDown = false;
    const bool mouseInsideImGuiViewport = bUseImGuiEditor &&
        MouseX >= 0 && MouseY >= 0 && MouseX < (int)w && MouseY < (int)h;
    const bool sceneMouseAvailable = !bUseImGuiEditor || bImGuiViewportHovered || mouseInsideImGuiViewport ||
        bDragging || bPlacingFromSidebar || bDraggingMaterial;
    const bool physicalLeftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool leftMouseDown = (bUseImGuiEditor && bImGuiInitialized)
        ? (ImGui::GetIO().MouseDown[0] || Input.Keys[VK_LBUTTON] || physicalLeftDown)
        : Input.Keys[VK_LBUTTON];
    const bool lDown = leftMouseDown && sceneMouseAvailable;
    const bool imGuiGizmoCapturing = bUseImGuiEditor && (bImGuizmoHovered || bImGuizmoUsing);

    // 拖拽放置时每帧更新鼠标（非消息驱动）。
    // While dragging from sidebar, follow the mouse every frame (not message-driven).
    if (bPlacingFromSidebar && ViewportHwnd)
    {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(ViewportHwnd, &pt);
        MouseX = bUseImGuiEditor ? (pt.x - ImGuiViewportX) : pt.x;
        MouseY = bUseImGuiEditor ? (pt.y - ImGuiViewportY) : pt.y;
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
        // 按类型设置默认材质颜色。
        FSceneObject obj = MakeSceneObject(CommitType, PreviewPos);
        EnsureConcreteMaterialForObject(obj);
        Objects.push_back(obj);
        SetSelectedIndex((int)Objects.size() - 1);
        MarkLevelDirty();
    }

    // 点击：优先拾取 Gizmo 轴，否则拾取物体。
    // On click: pick either gizmo axis (if selected) or the sphere.
    if (lDown && !prevLDown)
    {
        const bool hasSelection = (SelectedIndex >= 0 && SelectedIndex < (int)Objects.size());
        const XMVECTOR selectedCenter = hasSelection ? XMLoadFloat3(&Objects[SelectedIndex].Position) : XMVectorZero();
        bool imGuiNearGizmo = false;
        const int imGuiPickedAxis = (bUseImGuiEditor && hasSelection)
            ? PickEditorGizmoAxis2D(MouseX, MouseY, selectedCenter, viewProj, w, h, imGuiNearGizmo)
            : 0;

        // 若处于放置拖拽，不进行场景拾取。
        // Skip legacy scene picking while placement or ImGuizmo owns the click.
        if (bPlacingFromSidebar || (bUseImGuiEditor && imGuiPickedAxis == 0 && (imGuiGizmoCapturing || imGuiNearGizmo)))
        {
            prevLDown = lDown;
            goto SceneClickDone;
        }

        ActiveAxis = EGizmoAxis::None;
        bDragging = false;

        XMVECTOR rayOrigin{};
        const XMVECTOR rayDir = MakeRayDirFromScreen(MouseX, MouseY, w, h, invViewProj, rayOrigin);

        // 已选中物体时，先在屏幕空间测试 Gizmo 轴线命中。
        // If sphere is selected, try axis hit-test in screen-space (like UE).
        if (hasSelection)
        {
            float bestDist = 1e9f;
            EGizmoAxis bestAxis = EGizmoAxis::None;
            constexpr float kPickThresholdPx = 12.0f;

            if (bUseImGuiEditor)
            {
                switch (imGuiPickedAxis)
                {
                case 1: bestDist = 0.0f; bestAxis = EGizmoAxis::X; break;
                case 2: bestDist = 0.0f; bestAxis = EGizmoAxis::Y; break;
                case 3: bestDist = 0.0f; bestAxis = EGizmoAxis::Z; break;
                default: break;
                }
            }
            else
            {
                constexpr float gizmoLen = 1.5f;
                const XMVECTOR p0 = selectedCenter;
                const XMVECTOR px = XMVectorAdd(p0, XMVectorSet(gizmoLen, 0.0f, 0.0f, 0.0f));
                const XMVECTOR py = XMVectorAdd(p0, XMVectorSet(0.0f, gizmoLen, 0.0f, 0.0f));
                const XMVECTOR pz = XMVectorAdd(p0, XMVectorSet(0.0f, 0.0f, gizmoLen, 0.0f));

                float x0, y0, x1, y1;
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

            SetSelectedIndex(bestIdx);
        }
    }

    // 拖拽：沿选中轴进行平移或缩放。
SceneClickDone:
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
                MarkLevelDirty();
            }
            else
            {
                XMVECTOR newPos = XMVectorAdd(axisOrigin, XMVectorScale(axisDir, delta));
                XMStoreFloat3(&Objects[SelectedIndex].Position, newPos);
                MarkLevelDirty();
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
        MouseX = bUseImGuiEditor ? (pt.x - ImGuiViewportX) : pt.x;
        MouseY = bUseImGuiEditor ? (pt.y - ImGuiViewportY) : pt.y;
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
    InitializeEditorContent();
    UIFont = CreateFontW(
        -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    UITitleFont = CreateFontW(
        -16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    UIBackgroundBrush = CreateSolidBrush(kEditorBackground);
    UIPanelBrush = CreateSolidBrush(kEditorPanel);
    UIHeaderBrush = CreateSolidBrush(kEditorHeader);
    UIListBrush = CreateSolidBrush(kEditorList);
    UIEditBrush = CreateSolidBrush(kEditorEdit);

    // 创建主窗口与侧栏、视口基础。
    const wchar_t* baseTitle = L"ShellEngine";
    Window.Create(hInstance, 1280, 720, baseTitle, &FEngine::WindowMessageHandler, this);
    DragAcceptFiles(Window.GetHwnd(), TRUE);

    ToolbarPanel = CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 1280, TopToolbarHeightPx, Window.GetHwnd(), MenuHandle(5300), GetModuleHandleW(nullptr), nullptr);
    ToolbarTitle = CreateWindowExW(0, L"STATIC", L"ShellEngine Editor", WS_CHILD | WS_VISIBLE, 10, 7, 190, 22, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
    ToolbarNewLevelBtn = CreateWindowExW(0, L"BUTTON", L"New", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 210, 6, 72, 24, Window.GetHwnd(), MenuHandle(IDC_TOOLBAR_NEW_LEVEL), GetModuleHandleW(nullptr), nullptr);
    ToolbarOpenLevelBtn = CreateWindowExW(0, L"BUTTON", L"Open", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 288, 6, 76, 24, Window.GetHwnd(), MenuHandle(IDC_TOOLBAR_OPEN_LEVEL), GetModuleHandleW(nullptr), nullptr);
    ToolbarSaveLevelBtn = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 370, 6, 76, 24, Window.GetHwnd(), MenuHandle(IDC_TOOLBAR_SAVE_LEVEL), GetModuleHandleW(nullptr), nullptr);
    ToolbarImportObjBtn = CreateWindowExW(0, L"BUTTON", L"Import OBJ", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 452, 6, 92, 24, Window.GetHwnd(), MenuHandle(IDC_TOOLBAR_IMPORT_OBJ), GetModuleHandleW(nullptr), nullptr);
    ToolbarPlaceBtn = CreateWindowExW(0, L"BUTTON", L"Place", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 550, 6, 72, 24, Window.GetHwnd(), MenuHandle(IDC_TOOLBAR_PLACE), GetModuleHandleW(nullptr), nullptr);
    ToolbarSettingsBtn = CreateWindowExW(0, L"BUTTON", L"Settings", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 628, 6, 82, 24, Window.GetHwnd(), MenuHandle(IDC_TOOLBAR_SETTINGS), GetModuleHandleW(nullptr), nullptr);
    StatusLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 724, 8, 520, 20, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
    ApplyEditorFont(ToolbarTitle, true);
    ApplyEditorFont(ToolbarNewLevelBtn);
    ApplyEditorFont(ToolbarOpenLevelBtn);
    ApplyEditorFont(ToolbarSaveLevelBtn);
    ApplyEditorFont(ToolbarImportObjBtn);
    ApplyEditorFont(ToolbarPlaceBtn);
    ApplyEditorFont(ToolbarSettingsBtn);
    ApplyEditorFont(StatusLabel);
    SetWindowLongPtrW(ToolbarSettingsBtn, GWLP_USERDATA, (LONG_PTR)this);
    ToolbarSettingsOldProc = (WNDPROC)SetWindowLongPtrW(ToolbarSettingsBtn, GWLP_WNDPROC, (LONG_PTR)&FEngine::ToolbarSettingsWndProc);

    // Sidebar listbox (native Win32 UI)
    EngineNameLabel = CreateWindowExW(
        0,
        L"STATIC",
        L"Place Actors",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        0,
        0,
        SidebarWidthPx,
        22,
        Window.GetHwnd(),
        MenuHandle(1000),
        GetModuleHandleW(nullptr),
        nullptr);
    ApplyEditorFont(EngineNameLabel, true);
    SidebarToggleBtn = CreateEditorButton(L"<", Window.GetHwnd(), IDC_PLACE_ACTORS_TOGGLE);
    SidebarBasicLabel = CreateEditorLabel(L"Actor Palette", Window.GetHwnd(), true);
    SidebarRenderDocLabel = CreateEditorLabel(L"RenderDoc Captures", Window.GetHwnd(), true);
    ShowWindow(SidebarRenderDocLabel, SW_HIDE);
    SidebarSearchEdit = CreateWindowExW(
        0,
        L"EDIT",
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        0,
        0,
        SidebarWidthPx,
        22,
        Window.GetHwnd(),
        MenuHandle(IDC_ACTOR_SEARCH),
        GetModuleHandleW(nullptr),
        nullptr);
    ApplyEditorFont(SidebarSearchEdit);
    SendMessageW(SidebarSearchEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search actors");
    SetWindowLongPtrW(SidebarSearchEdit, GWLP_USERDATA, (LONG_PTR)this);
    SidebarSearchOldProc = (WNDPROC)SetWindowLongPtrW(SidebarSearchEdit, GWLP_WNDPROC, (LONG_PTR)&FEngine::SidebarSearchWndProc);

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
        MenuHandle(1001),
        GetModuleHandleW(nullptr),
        nullptr);
    ApplyEditorFont(SidebarList);
    RefreshActorPalette();

    // Subclass sidebar to support drag-to-place
    SetWindowLongPtrW(SidebarList, GWLP_USERDATA, (LONG_PTR)this);
    SidebarOldProc = (WNDPROC)SetWindowLongPtrW(SidebarList, GWLP_WNDPROC, (LONG_PTR)&FEngine::SidebarWndProc);

    // Sidebar: render path selector
    {
        RenderSettingsLabel = CreateEditorLabel(L"Render & Environment", Window.GetHwnd(), true);
        RenderGILabel = CreateEditorLabel(L"Global Illumination", Window.GetHwnd(), true);
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

        TonemapOperatorLabel = CreateWindowExW(
            0, L"STATIC", L"Tonemap Operator",
            WS_CHILD | WS_VISIBLE,
            8, 244, SidebarWidthPx - 16, 16,
            Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);

        TonemapOperatorCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            8, 262, SidebarWidthPx - 16, 120,
            Window.GetHwnd(), MenuHandle(IDC_RENDER_TONEMAP_OPERATOR), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(TonemapOperatorCombo, CB_ADDSTRING, 0, (LPARAM)L"Reinhard");
        SendMessageW(TonemapOperatorCombo, CB_ADDSTRING, 0, (LPARAM)L"AgX");
        SendMessageW(TonemapOperatorCombo, CB_ADDSTRING, 0, (LPARAM)L"ACES 1.0 RRT+ODT");
        SendMessageW(TonemapOperatorCombo, CB_SETCURSEL, (WPARAM)TonemapOperatorToComboIndex(TonemapOperator), 0);
    }

    // Sidebar: SkyAtmosphere + Sun controls
    {
        SkySettings.Enable = true;
        SkySettings.AtmosphereHeight = 12.0f;
        SkySettings.RayleighScale = 1.0f;
        SkySettings.MieScale = 1.0f;
        SkySettings.MieG = 0.8f;
        SkySettings.GroundAlbedo = 0.2f;

        SunSectionLabel = CreateEditorLabel(L"Sun", Window.GetHwnd(), true);
        AtmosphereSectionLabel = CreateEditorLabel(L"Atmosphere", Window.GetHwnd(), true);

        SkyEnableCheckbox = CreateWindowExW(
            0, L"BUTTON", L"SkyAtmosphere",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            8, 130, SidebarWidthPx - 16, 20,
            Window.GetHwnd(), (HMENU)3001, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(SkyEnableCheckbox, BM_SETCHECK, SkySettings.Enable ? BST_CHECKED : BST_UNCHECKED, 0);

        auto makeSlider = [&](int y, int id)
        {
            return CreateWindowExW(0, TRACKBAR_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 8, y, SidebarWidthPx - 16, 26, Window.GetHwnd(), MenuHandle(id), GetModuleHandleW(nullptr), nullptr);
        };

        SunYawLabel = CreateWindowExW(0, L"STATIC", L"Sun Yaw", WS_CHILD | WS_VISIBLE, 8, 154, SidebarWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        SunYawValueLabel = CreateEditorLabel(L"", Window.GetHwnd());
        SunYawSlider = makeSlider(170, 3002);
        SendMessageW(SunYawSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 3600));
        SendMessageW(SunYawSlider, TBM_SETTICFREQ, 300, 0);

        SunPitchLabel = CreateWindowExW(0, L"STATIC", L"Sun Pitch", WS_CHILD | WS_VISIBLE, 8, 196, SidebarWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        SunPitchValueLabel = CreateEditorLabel(L"", Window.GetHwnd());
        SunPitchSlider = makeSlider(212, 3003);
        SendMessageW(SunPitchSlider, TBM_SETRANGE, TRUE, MAKELPARAM(-890, 890));
        SendMessageW(SunPitchSlider, TBM_SETTICFREQ, 200, 0);

        SunIntensityLabel = CreateWindowExW(0, L"STATIC", L"Sun Intensity", WS_CHILD | WS_VISIBLE, 8, 238, SidebarWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        SunIntensityValueLabel = CreateEditorLabel(L"", Window.GetHwnd());
        SunIntensitySlider = makeSlider(254, 3004);
        SendMessageW(SunIntensitySlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 2000)); // 0..20
        SendMessageW(SunIntensitySlider, TBM_SETTICFREQ, 200, 0);

        RayleighLabel = CreateWindowExW(0, L"STATIC", L"Rayleigh", WS_CHILD | WS_VISIBLE, 8, 280, SidebarWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        RayleighValueLabel = CreateEditorLabel(L"", Window.GetHwnd());
        RayleighSlider = makeSlider(296, 3005);
        SendMessageW(RayleighSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 3000)); // 0..3
        SendMessageW(RayleighSlider, TBM_SETTICFREQ, 500, 0);

        MieLabel = CreateWindowExW(0, L"STATIC", L"Mie", WS_CHILD | WS_VISIBLE, 8, 322, SidebarWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        MieValueLabel = CreateEditorLabel(L"", Window.GetHwnd());
        MieSlider = makeSlider(338, 3006);
        SendMessageW(MieSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 3000)); // 0..3
        SendMessageW(MieSlider, TBM_SETTICFREQ, 500, 0);

        MieGLabel = CreateWindowExW(0, L"STATIC", L"Mie G", WS_CHILD | WS_VISIBLE, 8, 364, SidebarWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        MieGValueLabel = CreateEditorLabel(L"", Window.GetHwnd());
        MieGSlider = makeSlider(380, 3007);
        SendMessageW(MieGSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 990)); // 0..0.99
        SendMessageW(MieGSlider, TBM_SETTICFREQ, 165, 0);

        AtmoHeightLabel = CreateWindowExW(0, L"STATIC", L"Atmosphere Height", WS_CHILD | WS_VISIBLE, 8, 406, SidebarWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        AtmoHeightValueLabel = CreateEditorLabel(L"", Window.GetHwnd());
        AtmoHeightSlider = makeSlider(422, 3008);
        SendMessageW(AtmoHeightSlider, TBM_SETRANGE, TRUE, MAKELPARAM(50, 3000)); // 0.5..30.0
        SendMessageW(AtmoHeightSlider, TBM_SETTICFREQ, 500, 0);

        SkyLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 8, 452, SidebarWidthPx - 16, 54, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);

        UpdateSkyUI();
    }
    {
        HWND fontTargets[] = {
            RenderPathLabel, RenderPathCombo, LumenCheckbox, LumenSWRTCheckbox, LumenHWRTCheckbox, TonemapOperatorLabel, TonemapOperatorCombo,
            SkyEnableCheckbox, SunYawLabel, SunPitchLabel, SunIntensityLabel, RayleighLabel,
            MieLabel, MieGLabel, AtmoHeightLabel, SkyLabel
        };
        for (HWND h : fontTargets)
            ApplyEditorFont(h);
        HWND hiddenLeftControls[] = {
            RenderSettingsLabel, RenderGILabel, RenderPathLabel, RenderPathCombo,
            LumenCheckbox, LumenSWRTCheckbox, LumenHWRTCheckbox, TonemapOperatorLabel, TonemapOperatorCombo,
            SkyEnableCheckbox, SunYawLabel, SunYawValueLabel, SunYawSlider,
            SunPitchLabel, SunPitchValueLabel, SunPitchSlider,
            SunIntensityLabel, SunIntensityValueLabel, SunIntensitySlider,
            RayleighLabel, RayleighValueLabel, RayleighSlider,
            MieLabel, MieValueLabel, MieSlider,
            MieGLabel, MieGValueLabel, MieGSlider,
            AtmoHeightLabel, AtmoHeightValueLabel, AtmoHeightSlider,
            SunSectionLabel, AtmosphereSectionLabel, SkyLabel
        };
        for (HWND h : hiddenLeftControls)
            if (h) ShowWindow(h, SW_HIDE);
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
        const int clientW = std::max(1, (int)(rc.right - rc.left));
        const int clientH = std::max(1, (int)(rc.bottom - rc.top));
        const int vx = bUseImGuiEditor ? 0 : SidebarWidthPx;
        const int vy = bUseImGuiEditor ? 0 : TopToolbarHeightPx;
        const int vw = bUseImGuiEditor ? clientW : std::max(1, clientW - SidebarWidthPx - RightPanelWidthPx);
        const int vh = bUseImGuiEditor ? clientH : std::max(1, clientH - TopToolbarHeightPx - BottomPanelHeightPx);

        ViewportHwnd = CreateWindowExW(
            0,
            wc.lpszClassName,
            nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            vx,
            vy,
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
        const int viewportW = std::max(1, clientW - SidebarWidthPx - RightPanelWidthPx);
        const int viewportH = std::max(1, clientH - TopToolbarHeightPx - BottomPanelHeightPx);
        const int rightX = SidebarWidthPx + viewportW;

        RightPanel = CreateWindowExW(
            0, L"STATIC", nullptr,
            WS_CHILD | WS_VISIBLE,
            rightX, TopToolbarHeightPx, RightPanelWidthPx, viewportH,
            Window.GetHwnd(), MenuHandle(5100), GetModuleHandleW(nullptr), nullptr);
        OutlinerLabel = CreateWindowExW(0, L"STATIC", L"Level Outliner", WS_CHILD | WS_VISIBLE, rightX + 8, 8, RightPanelWidthPx - 16, 18, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        OutlinerList = CreateWindowExW(0, L"LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, rightX + 8, 30, RightPanelWidthPx - 16, 220, Window.GetHwnd(), MenuHandle(IDC_OUTLINER_LIST), GetModuleHandleW(nullptr), nullptr);
        DetailsLabel = CreateWindowExW(0, L"STATIC", L"Details", WS_CHILD | WS_VISIBLE, rightX + 8, 270, RightPanelWidthPx - 16, 18, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailTransformLabel = CreateWindowExW(0, L"STATIC", L"Transform", WS_CHILD | WS_VISIBLE, rightX + 8, 320, RightPanelWidthPx - 16, 18, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailEnvironmentLabel = CreateWindowExW(0, L"STATIC", L"Environment", WS_CHILD | WS_VISIBLE, rightX + 8, 382, RightPanelWidthPx - 16, 18, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailNameLabel = CreateWindowExW(0, L"STATIC", L"Name", WS_CHILD | WS_VISIBLE, rightX + 8, 294, RightPanelWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailNameEdit = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, rightX + 8, 294, RightPanelWidthPx - 16, 22, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailPositionLabel = CreateWindowExW(0, L"STATIC", L"Location  X / Y / Z", WS_CHILD | WS_VISIBLE, rightX + 8, 320, RightPanelWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailPosXEdit = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, rightX + 8, 326, 72, 22, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailPosYEdit = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, rightX + 88, 326, 72, 22, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailPosZEdit = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, rightX + 168, 326, 72, 22, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailScaleLabel = CreateWindowExW(0, L"STATIC", L"Scale  X / Y / Z", WS_CHILD | WS_VISIBLE, rightX + 8, 350, RightPanelWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailScaleXEdit = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, rightX + 8, 356, 72, 22, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailScaleYEdit = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, rightX + 88, 356, 72, 22, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailScaleZEdit = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, rightX + 168, 356, 72, 22, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailIntensityLabel = CreateWindowExW(0, L"STATIC", L"Intensity", WS_CHILD | WS_VISIBLE, rightX + 8, 382, RightPanelWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailIntensityEdit = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, rightX + 8, 400, RightPanelWidthPx - 16, 22, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailSkyEnabledCheckbox = CreateWindowExW(0, L"BUTTON", L"Enable Sky Atmosphere", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, rightX + 8, 382, RightPanelWidthPx - 16, 22, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailRayleighLabel = CreateWindowExW(0, L"STATIC", L"Rayleigh Scale", WS_CHILD | WS_VISIBLE, rightX + 8, 408, RightPanelWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailRayleighEdit = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, rightX + 8, 426, RightPanelWidthPx - 16, 22, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailMieLabel = CreateWindowExW(0, L"STATIC", L"Mie Scale", WS_CHILD | WS_VISIBLE, rightX + 8, 452, RightPanelWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailMieEdit = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, rightX + 8, 470, RightPanelWidthPx - 16, 22, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailMieGLabel = CreateWindowExW(0, L"STATIC", L"Mie G", WS_CHILD | WS_VISIBLE, rightX + 8, 496, RightPanelWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailMieGEdit = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, rightX + 8, 514, RightPanelWidthPx - 16, 22, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailAtmosphereHeightLabel = CreateWindowExW(0, L"STATIC", L"Atmosphere Height", WS_CHILD | WS_VISIBLE, rightX + 8, 540, RightPanelWidthPx - 16, 16, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        DetailAtmosphereHeightEdit = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, rightX + 8, 558, RightPanelWidthPx - 16, 22, Window.GetHwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        ApplyDetailsBtn = CreateWindowExW(0, L"BUTTON", L"Apply Details", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, rightX + 8, 386, RightPanelWidthPx - 16, 24, Window.GetHwnd(), MenuHandle(IDC_APPLY_DETAILS), GetModuleHandleW(nullptr), nullptr);
        ApplyEditorFont(OutlinerLabel, true);
        ApplyEditorFont(OutlinerList);
        ApplyEditorFont(DetailsLabel, true);
        ApplyEditorFont(DetailTransformLabel, true);
        ApplyEditorFont(DetailEnvironmentLabel, true);
        ApplyEditorFont(DetailNameLabel);
        ApplyEditorFont(DetailPositionLabel);
        ApplyEditorFont(DetailScaleLabel);
        ApplyEditorFont(DetailNameEdit);
        ApplyEditorFont(DetailPosXEdit);
        ApplyEditorFont(DetailPosYEdit);
        ApplyEditorFont(DetailPosZEdit);
        ApplyEditorFont(DetailScaleXEdit);
        ApplyEditorFont(DetailScaleYEdit);
        ApplyEditorFont(DetailScaleZEdit);
        ApplyEditorFont(DetailIntensityLabel);
        ApplyEditorFont(DetailIntensityEdit);
        ApplyEditorFont(DetailSkyEnabledCheckbox);
        ApplyEditorFont(DetailRayleighLabel);
        ApplyEditorFont(DetailRayleighEdit);
        ApplyEditorFont(DetailMieLabel);
        ApplyEditorFont(DetailMieEdit);
        ApplyEditorFont(DetailMieGLabel);
        ApplyEditorFont(DetailMieGEdit);
        ApplyEditorFont(DetailAtmosphereHeightLabel);
        ApplyEditorFont(DetailAtmosphereHeightEdit);
        ApplyEditorFont(ApplyDetailsBtn);
    }

    // Bottom panel UI (content + textures + materials + level commands)
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
        const int colW = (clientW - padding * 5) / 4;
        const int listH = BottomPanelHeightPx - padding * 2 - 26;

        ContentTitleLabel = CreateWindowExW(0, L"STATIC", L"Content Drawer", WS_CHILD | WS_VISIBLE, padding, padding, colW, 18, BottomPanel, nullptr, GetModuleHandleW(nullptr), nullptr);
        ContentDrawerToggleBtn = CreateWindowExW(0, L"BUTTON", L"Collapse", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, padding, padding, 78, 22, BottomPanel, MenuHandle(IDC_CONTENT_DRAWER_TOGGLE), GetModuleHandleW(nullptr), nullptr);
        TextureTitleLabel = CreateWindowExW(0, L"STATIC", L"Folders", WS_CHILD | WS_VISIBLE, padding + colW + padding, padding, colW, 18, BottomPanel, nullptr, GetModuleHandleW(nullptr), nullptr);
        PreviewTitleLabel = CreateWindowExW(0, L"STATIC", L"Inspector", WS_CHILD | WS_VISIBLE, padding + (colW + padding) * 2, padding, colW, 18, BottomPanel, nullptr, GetModuleHandleW(nullptr), nullptr);
        MaterialTitleLabel = CreateWindowExW(0, L"STATIC", L"Assets", WS_CHILD | WS_VISIBLE, padding + (colW + padding) * 3, padding, colW, 18, BottomPanel, nullptr, GetModuleHandleW(nullptr), nullptr);
        ContentPathLabel = CreateWindowExW(0, L"STATIC", L"Content / Models", WS_CHILD | WS_VISIBLE, padding, padding, colW, 18, BottomPanel, nullptr, GetModuleHandleW(nullptr), nullptr);
        ContentActionsLabel = CreateWindowExW(0, L"STATIC", L"Loaded Textures / Materials", WS_CHILD | WS_VISIBLE, padding, padding, colW, 18, BottomPanel, nullptr, GetModuleHandleW(nullptr), nullptr);

        ContentFoldersList = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            padding, padding,
            colW, listH,
            BottomPanel, MenuHandle(IDC_CONTENT_FOLDERS), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(ContentFoldersList, LB_ADDSTRING, 0, (LPARAM)L"Models");
        SendMessageW(ContentFoldersList, LB_ADDSTRING, 0, (LPARAM)L"Textures");
        SendMessageW(ContentFoldersList, LB_ADDSTRING, 0, (LPARAM)L"Materials");
        SendMessageW(ContentFoldersList, LB_ADDSTRING, 0, (LPARAM)L"Levels");
        SendMessageW(ContentFoldersList, LB_SETCURSEL, (WPARAM)static_cast<int>(ContentFilter), 0);

        ContentList = CreateWindowExW(
            0, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
            padding, padding,
            colW, listH,
            BottomPanel, MenuHandle(IDC_CONTENT_LIST), GetModuleHandleW(nullptr), nullptr);
        ContentHintLabel = CreateWindowExW(0, L"STATIC", L"Double-click asset or use Place", WS_CHILD | WS_VISIBLE, padding, padding + listH + 2, colW, 16, BottomPanel, nullptr, GetModuleHandleW(nullptr), nullptr);

        ImportObjBtn = CreateWindowExW(
            0, L"BUTTON", L"Import OBJ",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            padding, padding + listH + 4,
            colW / 2 - 2, 22,
            BottomPanel, MenuHandle(IDC_IMPORT_OBJ), GetModuleHandleW(nullptr), nullptr);

        PlaceAssetBtn = CreateWindowExW(
            0, L"BUTTON", L"Place",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            padding + colW / 2 + 2, padding + listH + 4,
            colW / 2 - 2, 22,
            BottomPanel, MenuHandle(IDC_PLACE_ASSET), GetModuleHandleW(nullptr), nullptr);

        TextureList = CreateWindowExW(
            0, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
            padding + colW + padding, padding,
            colW, listH,
            BottomPanel, (HMENU)2001, GetModuleHandleW(nullptr), nullptr);

        TexturePreview = CreateWindowExW(
            0, L"STATIC", nullptr,
            WS_CHILD | WS_VISIBLE | SS_BITMAP | SS_CENTERIMAGE,
            padding + (colW + padding) * 2, padding,
            colW, listH,
            BottomPanel, (HMENU)2002, GetModuleHandleW(nullptr), nullptr);

        MaterialList = CreateWindowExW(
            0, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
            padding + (colW + padding) * 3, padding,
            colW, listH,
            BottomPanel, (HMENU)2003, GetModuleHandleW(nullptr), nullptr);

        NewMaterialBtn = CreateWindowExW(
            0, L"BUTTON", L"New Material",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            padding + (colW + padding) * 3, padding + listH + 4,
            colW / 2 - 2, 22,
            BottomPanel, (HMENU)2004, GetModuleHandleW(nullptr), nullptr);

        TonemapCheckbox = CreateWindowExW(
            0, L"BUTTON", L"Tonemap",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            padding + (colW + padding) * 2, padding + listH + 4,
            colW, 22,
            BottomPanel, (HMENU)2005, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(TonemapCheckbox, BM_SETCHECK, bEnableTonemap ? BST_CHECKED : BST_UNCHECKED, 0);
        ShowWindow(TonemapCheckbox, SW_HIDE);

        const int levelButtonW = std::max(24, (colW / 2 - 8) / 3);
        NewLevelBtn = CreateWindowExW(0, L"BUTTON", L"New", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            padding + (colW + padding) * 3 + colW / 2 + 2, padding + listH + 4, levelButtonW, 22,
            BottomPanel, MenuHandle(IDC_NEW_LEVEL), GetModuleHandleW(nullptr), nullptr);
        OpenLevelBtn = CreateWindowExW(0, L"BUTTON", L"Open", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            padding + (colW + padding) * 3 + colW / 2 + 2 + levelButtonW + 2, padding + listH + 4, levelButtonW, 22,
            BottomPanel, MenuHandle(IDC_OPEN_LEVEL), GetModuleHandleW(nullptr), nullptr);
        SaveLevelBtn = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            padding + (colW + padding) * 3 + colW / 2 + 2 + (levelButtonW + 2) * 2, padding + listH + 4, levelButtonW, 22,
            BottomPanel, MenuHandle(IDC_SAVE_LEVEL), GetModuleHandleW(nullptr), nullptr);

        HWND fontTargets[] = {
            ContentTitleLabel, ContentDrawerToggleBtn, TextureTitleLabel, PreviewTitleLabel, MaterialTitleLabel, ContentPathLabel, ContentActionsLabel, ContentHintLabel,
            ContentFoldersList, ContentList, ImportObjBtn, PlaceAssetBtn, TextureList, TexturePreview, MaterialList,
            NewMaterialBtn, TonemapCheckbox, NewLevelBtn, OpenLevelBtn, SaveLevelBtn
        };
        for (HWND h : fontTargets)
            ApplyEditorFont(h, h == ContentTitleLabel || h == TextureTitleLabel || h == PreviewTitleLabel || h == MaterialTitleLabel || h == ContentActionsLabel);

        SetWindowLongPtrW(MaterialList, GWLP_USERDATA, (LONG_PTR)this);
        MaterialOldProc = (WNDPROC)SetWindowLongPtrW(MaterialList, GWLP_WNDPROC, (LONG_PTR)&FEngine::MaterialWndProc);

        DragAcceptFiles(BottomPanel, TRUE);

        SetWindowLongPtrW(BottomPanel, GWLP_USERDATA, (LONG_PTR)this);
        BottomOldProc = (WNDPROC)SetWindowLongPtrW(BottomPanel, GWLP_WNDPROC, (LONG_PTR)&FEngine::BottomWndProc);
    }
    RefreshContentBrowser();

    // 初始布局，确保视口不与底栏重叠。
    // Make sure viewport does not overlap bottom panel at startup.
    LayoutUI();

    // 创建一个默认球体对象以便演示。
    // Fallback scene shown only if the startup level cannot be loaded.
    {
        FSceneObject obj{};
        obj.Id = NextObjectId++;
        obj.Name = L"RenderDoc Rock " + std::to_wstring(obj.Id);
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
        SelectedIndex = 0;
    }
    EnsureDefaultEnvironmentActors();
    RefreshOutliner();
    RefreshDetailsPanel();

    {
        RECT rc{};
        GetClientRect(ViewportHwnd, &rc);
        const uint32 vw = (uint32)std::max(1L, rc.right - rc.left);
        const uint32 vh = (uint32)std::max(1L, rc.bottom - rc.top);
        RHI.Init(ViewportHwnd, vw, vh);
    }
    Renderer.Init(RHI);
    if (bUseImGuiEditor)
        InitImGuiEditor();
    RefreshContentBrowser();
    MigrateContentMaterialsToV2();
    LoadContentMaterials();
    EnsureConcreteMaterialsForRenderableObjects();
    const std::wstring startupLevel = ReadEnvWide(L"SHELLENGINE_START_LEVEL");
    std::filesystem::path startupPath = startupLevel.empty()
        ? (ContentRoot / kDefaultStartupLevelPath)
        : std::filesystem::path(startupLevel);
    if (!startupPath.empty())
    {
        if (startupPath.is_relative() && !std::filesystem::exists(startupPath))
            startupPath = ContentRoot / startupPath;
        if (std::filesystem::exists(startupPath))
            LoadLevelFromPath(startupPath);
    }

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
    Window.Show(SW_SHOW);
    UpdateWindow(Window.GetHwnd());

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
        if (bUseImGuiEditor && bImGuiInitialized)
        {
            BeginImGuiEditorFrame();
            DrawImGuiEditor();
            ImGui::Render();
        }

        // 计算光源方向并传入渲染器。
        const float t = float(nowMs - StartTickMs) / 1000.0f;
        const DirectX::XMFLOAT3 lightDirWs = GetActiveLightDirection();
        const float activeSunIntensity = GetActiveSunIntensity();
        const FSkyAtmosphereSettings activeSky = GetActiveSkySettings();

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
                    ApplyMaterialToObject(bestIdx, PendingMaterialIndex);
                    RefreshDetailsPanel();
                }
            }
            bPendingMaterialDrop = false;
            PendingMaterialIndex = -1;
        }

        // 调用渲染器绘制一帧。
        std::vector<FSceneObject> previewObjects;
        const std::vector<FSceneObject>* renderObjects = &Objects;
        if (bAssetPreviewActive)
        {
            previewObjects = Objects;
            previewObjects.push_back(AssetPreviewObject);
            renderObjects = &previewObjects;
        }

        Renderer.Render(
            RHI,
            Camera,
            t,
            RenderPath,
            bEnableLumen,
            bEnableLumenSWRT,
            bEnableLumenHWRT,
            *renderObjects,
            bUseImGuiEditor ? -1 : SelectedIndex,
            GizmoMode == EGizmoMode::Scale,
            lightDirWs,
            bEnableTonemap,
            TonemapOperator,
            activeSunIntensity,
            activeSky,
            0,
            bPlacingFromSidebar ? &PreviewPos : nullptr,
            bPlacingFromSidebar ? PaletteType : FSceneObject::EType::Sphere,
            bUseImGuiEditor ? ImGuiViewportX : 0,
            bUseImGuiEditor ? ImGuiViewportY : 0,
            bUseImGuiEditor ? ImGuiViewportW : 0,
            bUseImGuiEditor ? ImGuiViewportH : 0,
            bUseImGuiEditor ? std::function<void(ID3D12GraphicsCommandList*)>([this](ID3D12GraphicsCommandList* cmd) { RenderImGuiDrawData(cmd); }) : std::function<void(ID3D12GraphicsCommandList*)>{});

        // 更新窗口标题以展示选中信息。
        // Window title shows selected object position (minimal UE-like feedback)
        const wchar_t* pathName = (RenderPath == FSimpleSceneRenderer::ERenderPath::Deferred) ? L"Deferred" : L"Forward";
        UpdateWindowTitle(baseTitle, pathName);
    }

    // 退出前等待 GPU 完成并释放资源。
    RHI.WaitForGPU();
    ShutdownImGuiEditor();
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
    if (GeneratedPreviewBitmap)
    {
        DeleteObject(GeneratedPreviewBitmap);
        GeneratedPreviewBitmap = nullptr;
    }
    if (UIFont)
    {
        DeleteObject(UIFont);
        UIFont = nullptr;
    }
    if (UITitleFont)
    {
        DeleteObject(UITitleFont);
        UITitleFont = nullptr;
    }
    if (UIBackgroundBrush)
    {
        DeleteObject(UIBackgroundBrush);
        UIBackgroundBrush = nullptr;
    }
    if (UIPanelBrush)
    {
        DeleteObject(UIPanelBrush);
        UIPanelBrush = nullptr;
    }
    if (UIHeaderBrush)
    {
        DeleteObject(UIHeaderBrush);
        UIHeaderBrush = nullptr;
    }
    if (UIListBrush)
    {
        DeleteObject(UIListBrush);
        UIListBrush = nullptr;
    }
    if (UIEditBrush)
    {
        DeleteObject(UIEditBrush);
        UIEditBrush = nullptr;
    }
    CoUninitialize();
}
