#include <stdexcept>
#include <fstream>

#include "Core/Win32.h"
#include "Engine/Engine.h"

/**
 * @brief Windows 应用入口，创建引擎并进入主循环。
 * @param hInst 当前进程实例句柄，用于窗口与资源初始化。
 * @param hPrevInstance 未使用（历史遗留参数）。
 * @param lpCmdLine 未使用（命令行字符串）。
 * @param nCmdShow 未使用（窗口显示方式由引擎控制）。
 * @return 0 表示正常退出，非 0 表示异常退出。
 * @note 阶段：应用启动与运行阶段。
 */
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    try
    {
        // 启动引擎主循环（创建窗口、初始化渲染与UI）。
        FEngine engine;
        engine.Run(hInst);
    }
    catch (const std::exception& e)
    {
        // 异常：写调试输出与落盘日志，弹出致命错误提示。
        OutputDebugStringA(e.what());
        OutputDebugStringA("\n");
        try
        {
            std::ofstream f("dx12_hello_error.txt", std::ios::out | std::ios::trunc);
            f << e.what() << "\n";
        }
        catch (...)
        {
            // 记录日志失败时静默忽略，避免二次异常。
        }
        MessageBoxA(nullptr, e.what(), "Fatal Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    return 0;
}
