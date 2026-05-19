# FrameRenderer

FrameRenderer 是一个基于 DirectX 12 的编辑器和渲染器，目标是把 RenderDoc 截帧里的真实资源还原成可运行、可编辑、可保存的渲染场景。

这个项目不是把截帧截图贴到窗口里，而是把截帧证据转成项目资源：模型、纹理、材质、shader 语义、关卡和验证截图。导入后的对象应该能像普通场景物体一样被选中、移动、指定材质、保存到 Level，并由渲染器重新渲染出来。

## 当前分支内容

当前分支是 RenderDoc 截帧还原的最小基础工程：

- 默认启动关卡：`Content/Levels/default_renderdoc_scene.level.json`
- 默认场景：一个 RenderDoc 岩石、一个 RenderDoc 植被、`Sun Light`、`Sky Atmosphere`
- 岩石材质模式：`Rdr2Rock`
- 植被材质模式：`Rdr2Foliage`
- 材质系统：v2 `.material.json`，包含 `shadingMode`、`params`、`textures`
- 编辑器 UI：基于 ImGui 的 Place Actors、Level Outliner、Details、Content Drawer、Material Editor、Render Settings
- 仓库内 AI Skill：`skills/renderdoc-frame-reconstruction/SKILL.md`

## RenderDoc 截帧还原 Skill

这个分支内置了一个给后续 AI 使用的 Codex skill：

```text
skills/renderdoc-frame-reconstruction/
  SKILL.md
  agents/openai.yaml
```

当任务是把 RenderDoc `.rdc`、RenderDoc 导出的 manifest、或者已导出的截帧资源还原到 FrameRenderer 时，应该先阅读并使用这个 skill。

skill 覆盖的流程包括：

- 选择并记录目标 draw / event
- 提取 geometry、texture binding、shader、uniform、render state
- 创建 `Content/Models`、`Content/Textures`、`Content/Materials`、`Content/Levels` 资源
- 复用或新增 `PbrLit`、`Unlit`、`Rdr2Rock`、`Rdr2Foliage` 等材质模式
- 用真实运行截图验证结果，而不是依赖截图代理

## 已导入的截帧资产

### RenderDoc Rock

- 模型：`Content/Models/RDR2Rock/rdr2_capture_rock.obj`
- 专用 renderer manifest：`Content/Models/RDR2Rock/CaptureRockManifest.json`
- 专用 renderer mesh buffers：`Content/Models/RDR2Rock/meshes/*.bin`
- 材质：`Content/Materials/RDR2Rock/rdr2_capture_rock.material.json`
- 纹理：`Content/Textures/RDR2Rock/*.dds`
- 材质模式：`Rdr2Rock`

### RenderDoc Plant

- 模型：`Content/Models/RenderDocPlant/renderdoc_plant_event_18024.obj`
- 材质：`Content/Materials/RenderDocPlant/renderdoc_plant_event_18024.material.json`
- 纹理：
  - `Content/Textures/RenderDocPlant/102006_basecolor_alpha.dds`
  - `Content/Textures/RenderDocPlant/102008_mask.dds`
  - `Content/Textures/RenderDocPlant/102010_normal.dds`
- 材质模式：`Rdr2Foliage`
- 截帧事件：`event_id=18024`，draw `1384`

## 编译

推荐 Release 编译命令：

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' build_vs\DX12HelloTriangle.sln /p:Configuration=Release /p:Platform=x64 /m
```

也可以重新生成 CMake 工程：

```powershell
cmake -S . -B build_vs -G "Visual Studio 17 2022" -A x64
cmake --build build_vs --config Release
```

可执行文件路径：

```text
build_vs/Release/dx12_hello.exe
```

## 运行

运行默认 RenderDoc 场景：

```powershell
.\build_vs\Release\dx12_hello.exe
```

指定启动某个 Level：

```powershell
$env:SHELLENGINE_START_LEVEL = 'Content/Levels/renderdoc_plant_event_18024.level.json'
.\build_vs\Release\dx12_hello.exe
```

运行并截取验证图：

```powershell
$env:SHELLENGINE_START_LEVEL = 'Content/Levels/default_renderdoc_scene.level.json'
$env:SHELLENGINE_CAPTURE_FRAME_PATH = "$env:TEMP\fr_default_renderdoc_scene.png"
$env:SHELLENGINE_CAPTURE_FRAME_INDEX = '5'
.\build_vs\Release\dx12_hello.exe
```

## 编辑器操作

- 鼠标右键拖动：旋转 / 飞行相机
- `WASDQE`：飞行模式下移动相机
- `Shift`：加快相机移动
- 飞行时滚轮：调整相机速度
- 鼠标左键：选择物体或 gizmo 轴
- `W`：平移 gizmo
- `R`：缩放 gizmo
- `Delete`：删除选中物体
- Place Actors：添加基础模型、环境对象和 RenderDoc 对象
- Level Outliner：查看并选择关卡中的对象
- Details：编辑 Transform，并为可渲染对象指定具体材质
- Content Drawer：浏览模型、纹理、材质和关卡
- Material Editor：编辑材质的 `shadingMode`、参数和纹理插槽
- Render Settings：切换渲染路径和全局渲染开关

## 目录结构

```text
Content/
  Levels/
  Materials/
    Default/
    RDR2Rock/
    RenderDocPlant/
  Models/
    RDR2Rock/
      meshes/
    RenderDocPlant/
  Textures/
    RDR2Rock/
    RenderDocPlant/
skills/
  renderdoc-frame-reconstruction/
src/
shaders/
tools/
```

## 材质模式

- `PbrLit`：标准光照 PBR 材质
- `Unlit`：不参与光照的颜色 / 强度材质
- `Rdr2Rock`：从 RenderDoc 截帧还原的岩石材质模式
- `Rdr2Foliage`：从 RenderDoc 截帧还原的植被材质模式，支持双面渲染、alpha cutout、顶点色 tint、法线输入和 mask 推导粗糙度

所有可渲染场景对象都应该引用一个真实的 `.material.json`。默认兜底材质是 `Content/Materials/Default/default_pbr.material.json`。

运行时真正加载的模型、纹理、材质、关卡和专用 renderer manifest 都应该放在 `Content` 下。RenderDoc 原始 shader bytecode、分析 README、本地截帧路径等证据类文件默认只保留本地，不提交到 Git。

## 验证清单

一个 RenderDoc 资源导入完成前，至少需要确认：

- Release 编译通过
- 对象以真实 mesh 数据导入，而不是截图代理
- 必要纹理已经复制到 `Content/Textures`
- 已创建并绑定 v2 材质
- 已在 `Content/Levels` 下创建可运行 Level
- 纯分析证据只保留本地，不提交到 Git
- 已从运行中的渲染器截取验证图
- 编辑器可以选择、移动、查看、保存导入对象

## 项目边界

FrameRenderer 仍然是一个紧凑的截帧还原渲染器，不是完整 UE5 替代品。项目优先保证系统清晰、资源路径可追踪、材质和 shader 语义可验证，让 RenderDoc 截帧数据能够被持续导入、迭代和还原。
