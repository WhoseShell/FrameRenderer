# ShellEngineDX12 / DX12Engine

一个面向学习的 DX12 渲染器骨架 + 小型编辑器 UI。强调直白可读、便于扩展，把常见渲染特性放在一个项目里。

## 主要特性
- 渲染路径：Forward / Deferred
- PBR 材质 + IBL（Sky SH / Prefilter）+ 阴影
- SkyAtmosphere（Rayleigh / Mie / 高度）+ 太阳控制
- Lumen Lite：屏幕空间 GI + 时间累积（学习版）
- Lumen SWRT GI：软件光追 + surface cache + temporal + bilateral
- Lumen HWRT GI（原型）：DXR RayQuery + probe cache + temporal + bilateral
- HDR + Tonemap
- 简易编辑器 UI：物体放置、材质/纹理管理、Gizmo 变换

> 说明：Lumen/HWRT 部分是学习友好的近似实现，不是 UE5 的完整 Lumen。

## 运行环境
- Windows 10 1809+ 或 Windows 11（DXR 依赖）
- 64 位构建（DXR 需要 x64）
- DX12 显卡；HWRT 需要支持 DXR Tier >= 1.0（如 RTX）
- Visual Studio Build Tools 或 Visual Studio（Desktop development with C++）
- Windows 10/11 SDK
- CMake >= 3.21
- VS Code + 扩展：C/C++、CMake Tools
- 可选：Ninja（更快）

### DXC（HWRT GI 需要）
HWRT GI 的 shader 通过 `dxc.exe` 编译（cs_6_5）。请确保：
- 安装 Windows SDK（通常自带 dxc.exe），或
- 设置环境变量 `DXC_PATH` 指向 dxc.exe

## 构建与运行

### VS Code（推荐）
1. 用 VS Code 打开工程根目录  
2. `CMake: Select a Kit` 选择 x64/amd64  
3. `CMake: Configure`  
4. `CMake: Build`  
5. `F5` 运行（或 `CMake: Run Without Debugging`）

### 命令行
```powershell
# Ninja（推荐）
cmake -S . -B build_x64 -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build_x64

# 或 Visual Studio
cmake -S . -B build_vs -G "Visual Studio 17 2022" -A x64
cmake --build build_vs --config Debug
```

可执行文件为 `dx12_hello.exe`。

## 操作与交互
- 右键按住：视角旋转 + 飞行模式移动
- WASDQE：前后左右上下移动
- Shift：移动加速
- 滚轮（右键按住时）：调整移动速度
- F1：切换 Forward / Deferred
- W / R：切换平移 / 缩放 Gizmo
- Delete：删除选中物体
- 左键点击：选中物体或 Gizmo 轴
- 从侧边栏拖拽 Sphere / Box / Cone 到视口：放置物体
- 将纹理文件拖入窗口（.png / .jpg / .tga）：导入纹理
- 双击材质列表：打开 Material Editor
- 将材质从列表拖到视口中的物体：快速赋材质

## HWRT GI 开启条件
HWRT GI 只在以下条件满足时可用：
- Render Path = Deferred
- Lumen 勾选
- HWRT GI (Probes) 勾选
- x64 构建 + DXR 支持 + DXC 可用

UI 状态提示：
- `DXR Unsupported`：通常是 x86 构建 / 系统版本 / 驱动 / GPU 不支持
- `Need DXC`：找不到 `dxc.exe`
- `Init Failed`：查看调试输出

## 目录结构
- `src/RHI/`：D3D12 设备与交换链、命令队列等
- `src/Renderer/`：渲染器主体、资源与场景
- `src/Renderer/Passes/`：Deferred/Lumen/Sky/Shadow/Tonemap 等渲染通道
- `src/Engine/`：编辑器 UI + 交互逻辑
- `shaders/`：HLSL shader 集合

## 备注
- 本项目强调“可读 + 易改”，不是工业级引擎。
- Lumen/SWRT/HWRT 为学习用近似实现，性能与画质都有取舍。
