#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>

namespace rdcimport
{
struct FDdsSubresource
{
    std::vector<std::byte> Data;
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t Depth = 1;
    uint32_t RowPitch = 0;
    uint32_t SlicePitch = 0;
};

struct FDdsImage
{
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t Depth = 1;
    uint32_t MipCount = 1;
    uint32_t ArraySize = 1;
    D3D12_RESOURCE_DIMENSION Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    std::vector<FDdsSubresource> Subresources;
};

FDdsImage LoadDdsImage(const std::filesystem::path& path);
uint32_t BitsPerPixel(DXGI_FORMAT format);
}
