#pragma once

#include "Core/Win32.h"
#include "Core/Types.h"

class FWindowsWindow
{
public:
    using FMessageHandler = LRESULT(*)(void* userPtr, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    /**
     * @brief 创建 Win32 窗口并保存尺寸与消息回调。
     * @param hInstance 应用实例句柄。
     * @param width 窗口宽度（像素）。
     * @param height 窗口高度（像素）。
     * @param title 窗口标题（宽字符）。
     * @param handler 消息处理回调（转发给引擎层）。
     * @param userPtr 回调用户指针（通常为引擎对象）。
     * @return 无返回值；失败时抛出异常。
     * @note 阶段：窗口初始化阶段。
     */
    void Create(
        HINSTANCE hInstance,
        uint32_t width,
        uint32_t height,
        const wchar_t* title,
        FMessageHandler handler,
        void* userPtr);
    /**
     * @brief 显示窗口。
     * @param cmdShow Win32 ShowWindow 参数。
     * @return 无返回值。
     * @note 阶段：初始化完成后进入可视阶段。
     */
    void Show(int cmdShow);

    /**
     * @brief 获取窗口句柄。
     * @param 无。
     * @return Win32 HWND，用于创建设备或查询窗口信息。
     * @note 阶段：运行时/渲染阶段。
     */
    HWND GetHwnd() const { return Hwnd; }
    /**
     * @brief 获取窗口宽度（像素）。
     * @param 无。
     * @return 窗口宽度。
     * @note 阶段：运行时/渲染阶段。
     */
    uint32_t GetWidth() const { return Width; }
    /**
     * @brief 获取窗口高度（像素）。
     * @param 无。
     * @return 窗口高度。
     * @note 阶段：运行时/渲染阶段。
     */
    uint32_t GetHeight() const { return Height; }

    /**
     * @brief 窗口过程函数，转发消息到实例回调。
     * @param hwnd 窗口句柄。
     * @param msg 消息类型。
     * @param wParam 消息参数。
     * @param lParam 消息参数。
     * @return LRESULT 消息处理结果。
     * @note 阶段：运行时消息泵处理阶段。
     */
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    HWND Hwnd = nullptr;
    uint32_t Width = 1280;
    uint32_t Height = 720;
    FMessageHandler Handler = nullptr;
    void* UserPtr = nullptr;
};
