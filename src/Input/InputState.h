#pragma once

#include <cstring>

#include "Core/Types.h"

struct FInputState
{
    bool Keys[256]{};
    bool Rotating = false;
    bool IgnoreNextMouseMove = false;
    int LastMouseX = 0;
    int LastMouseY = 0;
    int CenterMouseX = 0;
    int CenterMouseY = 0;
    int RawMouseDX = 0;
    int RawMouseDY = 0;
    bool RawHasAbs = false;
    int RawAbsX = 0;
    int RawAbsY = 0;

    /**
     * @brief 清空输入状态缓存（按键、鼠标与原始输入）。
     * @param 无。
     * @return 无返回值；重置结构体内部输入状态。
     * @note 阶段：每帧输入刷新/初始化阶段。
     */
    void Clear()
    {
        // 清空所有键位状态与鼠标跟踪数据。
        std::memset(Keys, 0, sizeof(Keys));
        Rotating = false;
        IgnoreNextMouseMove = false;
        LastMouseX = 0;
        LastMouseY = 0;
        CenterMouseX = 0;
        CenterMouseY = 0;
        RawMouseDX = 0;
        RawMouseDY = 0;
        RawHasAbs = false;
        RawAbsX = 0;
        RawAbsY = 0;
    }
};
