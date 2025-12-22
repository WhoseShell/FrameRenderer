#pragma once

#include <cstdio>
#include <cstddef>
#include <string>
#include <stdexcept>

#include "Core/Win32.h"

inline void ThrowIfFailed(HRESULT hr, const char* msg)
{
    if (FAILED(hr))
    {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s (hr=0x%08X)", msg, (unsigned)hr);
        throw std::runtime_error(buf);
    }
}

inline void DebugOutput(const std::wstring& s)
{
    OutputDebugStringW(s.c_str());
    OutputDebugStringW(L"\n");
}

inline std::string FormatWin32Error(DWORD err)
{
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
