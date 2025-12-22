#pragma once

#include "Core/Win32.h"
#include "Core/Types.h"

class FWindowsWindow
{
public:
    using FMessageHandler = LRESULT(*)(void* userPtr, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void Create(
        HINSTANCE hInstance,
        uint32_t width,
        uint32_t height,
        const wchar_t* title,
        FMessageHandler handler,
        void* userPtr);
    void Show(int cmdShow);

    HWND GetHwnd() const { return Hwnd; }
    uint32_t GetWidth() const { return Width; }
    uint32_t GetHeight() const { return Height; }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    HWND Hwnd = nullptr;
    uint32_t Width = 1280;
    uint32_t Height = 720;
    FMessageHandler Handler = nullptr;
    void* UserPtr = nullptr;
};
