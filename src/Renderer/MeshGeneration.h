#pragma once

#include <vector>

#include "Core/Types.h"

struct FVertex
{
    float Pos[3];
    float Col[3];
    float Nrm[3];
    float UV[2];
};

/**
 * @brief 生成球体网格（顶点与索引）。
 * @param slices 水平方向分段数（经度）。
 * @param stacks 垂直方向分段数（纬度）。
 * @param radius 球体半径。
 * @param outVerts 输出顶点数组。
 * @param outIndices 输出索引数组（uint16）。
 * @return 无返回值。
 * @note 阶段：几何体生成/资源准备阶段。
 */
void GenerateSphereMesh(uint32 slices, uint32 stacks, float radius, std::vector<FVertex>& outVerts, std::vector<uint16>& outIndices);
/**
 * @brief 生成立方体网格（顶点与索引）。
 * @param halfExtent 半边长（立方体中心到面距离）。
 * @param outVerts 输出顶点数组。
 * @param outIndices 输出索引数组（uint16）。
 * @return 无返回值。
 * @note 阶段：几何体生成/资源准备阶段。
 */
void GenerateBoxMesh(float halfExtent, std::vector<FVertex>& outVerts, std::vector<uint16>& outIndices);
/**
 * @brief 生成圆锥体网格（顶点与索引）。
 * @param slices 圆周分段数。
 * @param radius 圆锥底面半径。
 * @param height 圆锥高度。
 * @param outVerts 输出顶点数组。
 * @param outIndices 输出索引数组（uint16）。
 * @return 无返回值。
 * @note 阶段：几何体生成/资源准备阶段。
 */
void GenerateConeMesh(uint32 slices, float radius, float height, std::vector<FVertex>& outVerts, std::vector<uint16>& outIndices);
