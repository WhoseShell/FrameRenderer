#pragma once

#include <cmath>
#include <DirectXMath.h>

struct FCamera
{
    DirectX::XMFLOAT3 Position{ 0.0f, 0.0f, -3.0f };
    float Yaw = 0.0f;
    float Pitch = 0.0f;

    DirectX::XMVECTOR GetForwardVector() const
    {
        using namespace DirectX;
        const XMMATRIX yawM = XMMatrixRotationY(Yaw); // world-up yaw
        const XMVECTOR right = XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), yawM));
        const XMMATRIX pitchM = XMMatrixRotationAxis(right, Pitch); // pitch around yawed right (UE-like)
        const XMMATRIX m = XMMatrixMultiply(yawM, pitchM);
        return XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), m));
    }

    DirectX::XMMATRIX GetViewMatrix() const
    {
        using namespace DirectX;
        const XMVECTOR dir = GetForwardVector();
        const XMVECTOR eye = XMLoadFloat3(&Position);
        return XMMatrixLookToLH(eye, dir, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    }
};
