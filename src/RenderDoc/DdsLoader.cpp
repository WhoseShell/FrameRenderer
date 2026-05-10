#include "RenderDoc/DdsLoader.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "RenderDoc/CaptureManifest.h"

namespace rdcimport
{
namespace
{
constexpr uint32_t kDdsMagic = 0x20534444;
constexpr uint32_t kDxt10FourCc = 0x30315844;
constexpr uint32_t kDdsCaps2Volume = 0x200000;
constexpr uint32_t kDdpfAlpha = 0x2;
constexpr uint32_t kDdpfFourCc = 0x4;
constexpr uint32_t kDdpfRgb = 0x40;
constexpr uint32_t kDdpfLuminance = 0x20000;

struct FDdsPixelFormat
{
    uint32_t Size;
    uint32_t Flags;
    uint32_t FourCC;
    uint32_t RGBBitCount;
    uint32_t RMask;
    uint32_t GMask;
    uint32_t BMask;
    uint32_t AMask;
};

struct FDdsHeader
{
    uint32_t Size;
    uint32_t Flags;
    uint32_t Height;
    uint32_t Width;
    uint32_t PitchOrLinearSize;
    uint32_t Depth;
    uint32_t MipMapCount;
    uint32_t Reserved1[11];
    FDdsPixelFormat PixelFormat;
    uint32_t Caps;
    uint32_t Caps2;
    uint32_t Caps3;
    uint32_t Caps4;
    uint32_t Reserved2;
};

struct FDdsHeaderDx10
{
    uint32_t DxgiFormat;
    uint32_t ResourceDimension;
    uint32_t MiscFlag;
    uint32_t ArraySize;
    uint32_t MiscFlags2;
};

constexpr uint32_t MakeFourCC(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(a)
        | (static_cast<uint32_t>(b) << 8u)
        | (static_cast<uint32_t>(c) << 16u)
        | (static_cast<uint32_t>(d) << 24u);
}

bool IsBlockCompressed(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_BC1_UNORM
        || format == DXGI_FORMAT_BC1_UNORM_SRGB
        || format == DXGI_FORMAT_BC3_UNORM
        || format == DXGI_FORMAT_BC3_UNORM_SRGB
        || format == DXGI_FORMAT_BC4_UNORM
        || format == DXGI_FORMAT_BC5_UNORM
        || format == DXGI_FORMAT_BC7_UNORM
        || format == DXGI_FORMAT_BC7_UNORM_SRGB;
}

uint32_t BlockSizeBytes(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_UNORM:
        return 8;
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return 16;
    default:
        return 0;
    }
}

DXGI_FORMAT LegacyDxgiFormat(const FDdsPixelFormat& pixelFormat)
{
    if ((pixelFormat.Flags & kDdpfFourCc) != 0)
    {
        switch (pixelFormat.FourCC)
        {
        case MakeFourCC('D', 'X', 'T', '1'): return DXGI_FORMAT_BC1_UNORM;
        case MakeFourCC('D', 'X', 'T', '5'): return DXGI_FORMAT_BC3_UNORM;
        case MakeFourCC('A', 'T', 'I', '1'):
        case MakeFourCC('B', 'C', '4', 'U'): return DXGI_FORMAT_BC4_UNORM;
        case MakeFourCC('A', 'T', 'I', '2'):
        case MakeFourCC('B', 'C', '5', 'U'): return DXGI_FORMAT_BC5_UNORM;
        default: break;
        }
    }

    if ((pixelFormat.Flags & kDdpfRgb) != 0)
    {
        if (pixelFormat.RGBBitCount == 32
            && pixelFormat.RMask == 0x000000FF
            && pixelFormat.GMask == 0x0000FF00
            && pixelFormat.BMask == 0x00FF0000
            && pixelFormat.AMask == 0xFF000000)
        {
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        }
        if (pixelFormat.RGBBitCount == 16
            && pixelFormat.RMask == 0x000000FF
            && pixelFormat.GMask == 0x0000FF00
            && pixelFormat.BMask == 0)
        {
            return DXGI_FORMAT_R8G8_UNORM;
        }
        if (pixelFormat.RGBBitCount == 32
            && pixelFormat.RMask == 0x0000FFFF
            && pixelFormat.GMask == 0xFFFF0000
            && pixelFormat.BMask == 0)
        {
            return DXGI_FORMAT_R16G16_UNORM;
        }
        if (pixelFormat.RGBBitCount == 16
            && pixelFormat.RMask == 0x0000FFFF
            && pixelFormat.GMask == 0
            && pixelFormat.BMask == 0)
        {
            return DXGI_FORMAT_R16_UNORM;
        }
        if (pixelFormat.RGBBitCount == 8 && pixelFormat.RMask == 0x000000FF)
        {
            return DXGI_FORMAT_R8_UNORM;
        }
    }

    if ((pixelFormat.Flags & kDdpfLuminance) != 0)
    {
        if (pixelFormat.RGBBitCount == 16 && pixelFormat.RMask == 0x0000FFFF)
        {
            return DXGI_FORMAT_R16_UNORM;
        }
        if (pixelFormat.RGBBitCount == 8 && pixelFormat.RMask == 0x000000FF)
        {
            return DXGI_FORMAT_R8_UNORM;
        }
    }

    if ((pixelFormat.Flags & kDdpfAlpha) != 0 && pixelFormat.RGBBitCount == 8)
    {
        return DXGI_FORMAT_A8_UNORM;
    }

    return DXGI_FORMAT_UNKNOWN;
}
}

uint32_t BitsPerPixel(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return 128;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return 64;
    case DXGI_FORMAT_R32G32B32_FLOAT:
        return 96;
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
        return 32;
    case DXGI_FORMAT_R32G32_FLOAT:
        return 64;
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R8G8_UNORM:
        return 16;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_A8_UNORM:
        return 8;
    default:
        return 0;
    }
}

FDdsImage LoadDdsImage(const std::filesystem::path& path)
{
    const std::vector<std::byte> bytes = ReadBinaryFile(path);
    if (bytes.size() < sizeof(uint32_t) + sizeof(FDdsHeader))
    {
        throw std::runtime_error("DDS file too small: " + WideToUtf8(path.wstring()));
    }

    const auto* raw = reinterpret_cast<const uint8_t*>(bytes.data());
    if (*reinterpret_cast<const uint32_t*>(raw) != kDdsMagic)
    {
        throw std::runtime_error("invalid DDS magic: " + WideToUtf8(path.wstring()));
    }

    const auto* header = reinterpret_cast<const FDdsHeader*>(raw + sizeof(uint32_t));
    size_t offset = sizeof(uint32_t) + sizeof(FDdsHeader);

    FDdsImage image;
    image.Width = header->Width;
    image.Height = header->Height;
    image.Depth = (std::max)(header->Depth, 1u);
    image.MipCount = (std::max)(header->MipMapCount, 1u);

    if ((header->PixelFormat.Flags & kDdpfFourCc) != 0 && header->PixelFormat.FourCC == kDxt10FourCc)
    {
        if (bytes.size() < offset + sizeof(FDdsHeaderDx10))
        {
            throw std::runtime_error("DDS missing DX10 header: " + WideToUtf8(path.wstring()));
        }
        const auto* header10 = reinterpret_cast<const FDdsHeaderDx10*>(raw + offset);
        offset += sizeof(FDdsHeaderDx10);
        image.Format = static_cast<DXGI_FORMAT>(header10->DxgiFormat);
        image.Dimension = static_cast<D3D12_RESOURCE_DIMENSION>(header10->ResourceDimension);
        image.ArraySize = (std::max)(header10->ArraySize, 1u);
    }
    else
    {
        image.Format = LegacyDxgiFormat(header->PixelFormat);
        image.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        image.ArraySize = 1;
    }

    if (image.Format == DXGI_FORMAT_UNKNOWN)
    {
        throw std::runtime_error("unsupported DDS format: " + WideToUtf8(path.wstring()));
    }
    if ((header->Caps2 & kDdsCaps2Volume) != 0)
    {
        image.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        image.ArraySize = 1;
    }

    const uint32_t arraySize = image.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : image.ArraySize;
    for (uint32_t arrayIndex = 0; arrayIndex < arraySize; ++arrayIndex)
    {
        (void)arrayIndex;
        uint32_t width = image.Width;
        uint32_t height = image.Height;
        uint32_t depth = image.Depth;
        for (uint32_t mip = 0; mip < image.MipCount; ++mip)
        {
            FDdsSubresource subresource;
            subresource.Width = width;
            subresource.Height = height;
            subresource.Depth = depth;
            if (IsBlockCompressed(image.Format))
            {
                const uint32_t blocksX = (std::max)(1u, (width + 3u) / 4u);
                const uint32_t blocksY = (std::max)(1u, (height + 3u) / 4u);
                subresource.RowPitch = blocksX * BlockSizeBytes(image.Format);
                subresource.SlicePitch = subresource.RowPitch * blocksY;
            }
            else
            {
                const uint32_t bytesPerPixel = BitsPerPixel(image.Format) / 8u;
                if (bytesPerPixel == 0)
                {
                    throw std::runtime_error("unsupported DDS bits per pixel: " + WideToUtf8(path.wstring()));
                }
                subresource.RowPitch = width * bytesPerPixel;
                subresource.SlicePitch = subresource.RowPitch * height;
            }

            const size_t totalBytes = static_cast<size_t>(subresource.SlicePitch) * depth;
            if (offset + totalBytes > bytes.size())
            {
                throw std::runtime_error("DDS data truncated: " + WideToUtf8(path.wstring()));
            }

            subresource.Data.resize(totalBytes);
            std::memcpy(subresource.Data.data(), raw + offset, totalBytes);
            offset += totalBytes;
            image.Subresources.push_back(std::move(subresource));

            width = (std::max)(width / 2u, 1u);
            height = (std::max)(height / 2u, 1u);
            if (image.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
            {
                depth = (std::max)(depth / 2u, 1u);
            }
        }
    }

    return image;
}
}
