#pragma once

#include <cmath>
#include <DirectXMath.h>

struct FCamera
{
    DirectX::XMFLOAT3 Position{ 0.0f, 0.0f, -3.0f };
    float Yaw = 0.0f;
    float Pitch = 0.0f;

    /**
     * @brief 获取相机前向方向向量（单位向量）。
     * @param 无。
     * @return 相机在世界空间的前向方向（归一化向量）。
     * @note 阶段：每帧视图/投影视图计算阶段。
     */
    DirectX::XMVECTOR GetForwardVector() const
    {
        using namespace DirectX;
        // 先绕世界上方向做 yaw，再围绕右方向做 pitch，得到 UE 风格的前向向量。
        const XMMATRIX yawM = XMMatrixRotationY(Yaw); // world-up yaw
        const XMVECTOR right = XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), yawM));
        const XMMATRIX pitchM = XMMatrixRotationAxis(right, Pitch); // pitch around yawed right (UE-like)
        const XMMATRIX m = XMMatrixMultiply(yawM, pitchM);
        return XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), m));
    }

    /**
     * @brief 计算相机视图矩阵（左手坐标系）。
     * @param 无。
     * @return 相机视图矩阵，用于渲染视图变换。
     * @note 阶段：每帧渲染的视图矩阵生成阶段。
     */
    DirectX::XMMATRIX GetViewMatrix() const
    {
        using namespace DirectX;
        // 由位置和前向向量构建 LookTo 视图矩阵。
        const XMVECTOR dir = GetForwardVector();
        const XMVECTOR eye = XMLoadFloat3(&Position);
        return XMMatrixLookToLH(eye, dir, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    }
};
