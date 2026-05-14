cmake_minimum_required(VERSION 3.21)
project(DX12HelloTriangle LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(renderdoc_capture STATIC
    src/RenderDoc/Json.cpp
    src/RenderDoc/CaptureManifest.cpp
    src/RenderDoc/DdsLoader.cpp
    src/RenderDoc/CaptureRockRenderer.cpp
)

add_library(editor_imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_demo.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx12.cpp
    third_party/ImGuizmo/src/ImGuizmo.cpp
)

target_include_directories(editor_imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ImGuizmo/src
)

target_compile_definitions(editor_imgui PUBLIC
    IMGUI_DEFINE_MATH_OPERATORS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_include_directories(renderdoc_capture PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(renderdoc_capture PUBLIC
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

add_executable(dx12_hello
    WIN32
    src/main.cpp
    src/Editor/EditorAssets.cpp
    src/Editor/ObjLoader.cpp
    src/Engine/Engine.cpp
    src/Platform/Windows/WindowsWindow.cpp
    src/RHI/D3D12RHI.cpp
    src/Renderer/MeshGeneration.cpp
    src/Renderer/SceneRenderer.cpp
    src/Renderer/SimpleSceneRenderer.cpp
    src/Renderer/Passes/DeferredPass.cpp
    src/Renderer/Passes/HWRTGIPass.cpp
    src/Renderer/Passes/LumenSwrtPass.cpp
    src/Renderer/Passes/LumenPass.cpp
    src/Renderer/Passes/ShadowPass.cpp
    src/Renderer/Passes/SkyAtmospherePass.cpp
    src/Renderer/Passes/SkyIBLPass.cpp
    src/Renderer/Passes/TonemapPass.cpp
)

target_include_directories(dx12_hello PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(dx12_hello PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    SHADER_DIR=L"${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    ASSET_DIR=L"${CMAKE_CURRENT_SOURCE_DIR}/assets"
    CONTENT_DIR=L"${CMAKE_CURRENT_SOURCE_DIR}/Content"
)

# D3D12 + DXGI + Shader compiler (D3DCompileFromFile uses D3DCompiler_47.dll)
target_link_libraries(dx12_hello PRIVATE
    renderdoc_capture
    editor_imgui
    d3d12
    dxgi
    d3dcompiler
    dxguid
    comctl32
)

# Better warnings; keep it simple for learning
if (MSVC)
    target_compile_options(dx12_hello PRIVATE /W4 /permissive- /utf-8 /FS)
    target_compile_options(renderdoc_capture PRIVATE /W4 /permissive- /utf-8 /FS)
    target_compile_options(editor_imgui PRIVATE /W3 /permissive- /utf-8 /FS)
else()
    target_compile_options(dx12_hello PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(renderdoc_capture PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(editor_imgui PRIVATE -Wall -Wextra)
endif()
