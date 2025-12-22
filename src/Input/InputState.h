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

    void Clear()
    {
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
