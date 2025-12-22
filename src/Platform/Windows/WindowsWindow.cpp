#include "Platform/Windows/WindowsWindow.h"

#include "Core/Diagnostics.h"

void FWindowsWindow::Create(
    HINSTANCE hInstance,
    uint32_t width,
    uint32_t height,
    const wchar_t* title,
    FMessageHandler handler,
    void* userPtr)
{
    HINSTANCE module = hInstance ? hInstance : GetModuleHandleW(nullptr);
    Width = width;
    Height = height;
    Handler = handler;
    UserPtr = userPtr;

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

    RECT r{ 0,0,(LONG)Width,(LONG)Height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

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

void FWindowsWindow::Show(int cmdShow)
{
    ShowWindow(Hwnd, cmdShow);
}

LRESULT CALLBACK FWindowsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Route to FWindowsWindow* stored in GWLP_USERDATA (set during WM_NCCREATE).
    FWindowsWindow* window = reinterpret_cast<FWindowsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        window = reinterpret_cast<FWindowsWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        return TRUE;
    }

    if (window && window->Handler)
    {
        return window->Handler(window->UserPtr, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
