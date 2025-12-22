#pragma once

#include <cstdio>
#include <cstddef>
#include <string>
#include <stdexcept>

#include "Core/Win32.h"

/**
 * @brief 如果 HRESULT 失败则抛出异常并附带格式化信息。
 * @param hr 需要检查的 HRESULT。
 * @param msg 发生错误时用于补充说明的消息。
 * @return 无返回值；失败时抛出 std::runtime_error。
 * @note 阶段：初始化与运行时通用的错误处理阶段。
 */
inline void ThrowIfFailed(HRESULT hr, const char* msg)
{
    if (FAILED(hr))
    {
        // 组合错误描述与十六进制错误码，便于调试定位。
        char buf[512];
        snprintf(buf, sizeof(buf), "%s (hr=0x%08X)", msg, (unsigned)hr);
        throw std::runtime_error(buf);
    }
}

/**
 * @brief 输出一行宽字符调试日志到调试器。
 * @param s 需要输出的字符串。
 * @return 无返回值。
 * @note 阶段：运行时调试输出阶段。
 */
inline void DebugOutput(const std::wstring& s)
{
    // 输出文本并追加换行，便于调试器阅读。
    OutputDebugStringW(s.c_str());
    OutputDebugStringW(L"\n");
}

/**
 * @brief 将 Win32 错误码转换为可读字符串。
 * @param err Win32 错误码（GetLastError 的返回值）。
 * @return 格式化后的错误字符串（英文系统信息）。
 * @note 阶段：初始化与运行时通用的错误处理阶段。
 */
inline std::string FormatWin32Error(DWORD err)
{
    // 调用系统格式化接口获取错误描述。
    char buf[512];
    FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buf,
        (DWORD)std::size(buf),
        nullptr);
    return buf;
}
