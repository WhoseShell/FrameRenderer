#include "Platform/Windows/WindowsWindow.h"

#include "Core/Diagnostics.h"

/**
 * @brief 创建 Win32 窗口并设置回调与尺寸。
 * @param hInstance 应用实例句柄。
 * @param width 目标窗口宽度（像素）。
 * @param height 目标窗口高度（像素）。
 * @param title 窗口标题文本。
 * @param handler 消息处理回调。
 * @param userPtr 回调的用户指针。
 * @return 无返回值；失败时抛出异常。
 * @note 阶段：窗口初始化阶段。
 */
void FWindowsWindow::Create(
    HINSTANCE hInstance,
    uint32_t width,
    uint32_t height,
    const wchar_t* title,
    FMessageHandler handler,
    void* userPtr)
{
    // 记录窗口参数与回调，为后续消息转发准备。
    HINSTANCE module = hInstance ? hInstance : GetModuleHandleW(nullptr);
    Width = width;
    Height = height;
    Handler = handler;
    UserPtr = userPtr;

    // 注册窗口类（使用静态 WndProc）。
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = FWindowsWindow::WndProc;
    wc.hInstance = module;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DX12HelloWindowClass";

    if (!RegisterClassExW(&wc))
    {
        DWORD err = GetLastError();
        throw std::runtime_error("RegisterClassExW failed: " + FormatWin32Error(err));
    }

    // 计算包含窗口装饰的实际窗口大小。
    RECT r{ 0,0,(LONG)Width,(LONG)Height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    // 创建窗口并将 this 作为创建参数传入（用于 WM_NCCREATE 绑定）。
    Hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        r.right - r.left,
        r.bottom - r.top,
        nullptr,
        nullptr,
        module,
        this);

    if (!Hwnd)
    {
        DWORD err = GetLastError();
        throw std::runtime_error("CreateWindowExW failed: " + FormatWin32Error(err));
    }
}

/**
 * @brief 显示窗口。
 * @param cmdShow ShowWindow 参数。
 * @return 无返回值。
 * @note 阶段：初始化完成后的显示阶段。
 */
void FWindowsWindow::Show(int cmdShow)
{
    ShowWindow(Hwnd, cmdShow);
}

/**
 * @brief 静态窗口过程，将消息路由到实例回调。
 * @param hwnd 窗口句柄。
 * @param msg 消息类型。
 * @param wParam 消息参数。
 * @param lParam 消息参数。
 * @return LRESULT 消息处理结果。
 * @note 阶段：运行时消息泵阶段。
 */
LRESULT CALLBACK FWindowsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Route to FWindowsWindow* stored in GWLP_USERDATA (set during WM_NCCREATE).
    FWindowsWindow* window = reinterpret_cast<FWindowsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        // 首次创建时，把 this 指针写入窗口用户数据区。
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        window = reinterpret_cast<FWindowsWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        return TRUE;
    }

    if (window && window->Handler)
    {
        // 将消息交给实例回调处理。
        return window->Handler(window->UserPtr, hwnd, msg, wParam, lParam);
    }
    // 默认处理未被消费的消息。
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
