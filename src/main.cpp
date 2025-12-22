#include <stdexcept>
#include <fstream>

#include "Core/Win32.h"
#include "Engine/Engine.h"

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    try
    {
        FEngine engine;
        engine.Run(hInst);
    }
    catch (const std::exception& e)
    {
        OutputDebugStringA(e.what());
        OutputDebugStringA("\n");
        try
        {
            std::ofstream f("dx12_hello_error.txt", std::ios::out | std::ios::trunc);
            f << e.what() << "\n";
        }
        catch (...)
        {
        }
        MessageBoxA(nullptr, e.what(), "Fatal Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    return 0;
}
