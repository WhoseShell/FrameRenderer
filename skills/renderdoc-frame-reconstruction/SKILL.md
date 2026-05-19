---
name: renderdoc-frame-reconstruction
description: "当需要把 RenderDoc 截帧数据还原到 FrameRenderer 中，并生成真实模型、纹理、材质、shader、关卡和验证截图时使用。"
---

# RenderDoc 截帧还原

使用这个 skill，把 RenderDoc 截帧、已有 RenderDoc 导出目录、或者截帧 manifest 中的 draw 还原到 FrameRenderer 中。最终结果必须是可运行的场景资源，而不是截图代理。

## 基本原则

- RenderDoc 截帧是事实来源。不要用泛用模型、截图贴片或视觉近似替代真实截帧资源，除非明确记录限制。
- 每个还原结论都应该有证据：event id、draw topology、输入 / 输出 mesh、纹理 resource id、shader id、constant buffer、render state、验证截图。
- 优先把截帧资源转成 `Content` 资产和材质 schema。只有现有材质系统无法表达截帧 shading 时，才新增 C++ 渲染路径或 HLSL 逻辑。
- 修改前先看 `git status --short`，不要回退用户或其他任务留下的无关改动。

## FrameRenderer 约定

- 项目资源目录是 `Content/Models`、`Content/Textures`、`Content/Materials`、`Content/Levels`。
- 材质文件使用 v2 JSON：

```json
{
  "type": "FrameRendererMaterial",
  "version": 2,
  "name": "Example",
  "shadingMode": "PbrLit",
  "params": {},
  "textures": {}
}
```

- Level 里的 `asset` 和 `material` 使用相对 `Content` 的路径，例如 `Models/Foo/foo.obj` 和 `Materials/Foo/foo.material.json`。
- 每个可渲染物体都必须绑定真实 `.material.json`。只有截帧没有可用材质证据时，才使用 `Materials/Default/default_pbr.material.json` 兜底。
- `ObjLoader` 导入时会翻转纹理 V 坐标；从 RenderDoc UV 导出 OBJ 时写成 `vt u 1-v`。
- 已有内置材质模式包括 `PbrLit`、`Unlit`、`Rdr2Rock`、`Rdr2Foliage`。只有截帧需要不同纹理插槽、decode 或 shading 语义时，才新增 schema。

## 工作流程

### 1. 盘点截帧输入

1. 找到 `.rdc`、导出的 manifest、buffer、shader 文件和 texture dump。优先在仓库和已知截帧目录中使用 `rg --files` 做定向搜索。
2. 识别目标 draw 或目标物体。优先使用用户给出的 event id 或 label；没有时，用 mesh bounds、顶点 / 索引数量、alpha mode、纹理名 / resource id、shader 签名判断。
3. 导入前先记录证据：
   - capture path 或 manifest path
   - event id / draw id
   - topology 和 index / vertex 数量
   - shader 实际使用的 vertex attribute
   - 绑定纹理和 resource id
   - shader id、entry point 或 disassembly 路径
   - 影响位置、光照、BRDF、alpha、wind、材质 decode 的 constant buffer / uniform

### 2. 分析目标 draw

从 RenderDoc replay 数据、结构化导出数据或已有 manifest 中提取：

- Mesh 数据：position、normal、tangent、UV、color、index、primitive topology、world bounds。
- 材质输入：base color / albedo、normal、roughness、metallic、AO、mask、opacity、transmission、subsurface、wind、terrain blend 或其他游戏专用插槽。
- Shader 行为：BRDF 分支、alpha test、双面处理、normal map decode、mask 通道用途、emissive / unlit 分支、材质常量、纹理坐标变换。
- Render state：culling、depth test / write、blend state、alpha cutoff、shadow 参与方式，以及 draw 属于 forward、deferred 还是特殊 pass。
- 光照和环境依赖：太阳方向 / 强度、sky / IBL、曝光 / tonemap，以及匹配截帧外观需要的 capture-specific 常量。

不确定的地方必须写明。如果本地 replay 失败，可以用结构化截帧数据、shader 反汇编、资源名和绑定信息继续分析。

### 3. 导出 geometry

1. 默认把截帧 mesh 转成 OBJ，除非项目已经需要更完整的 mesh 格式。
2. 保留 shader 实际使用的 vertex attribute。OBJ 无法保存的属性，要写入 import manifest；只有渲染路径确实不用时才允许近似。
3. 以合理 local origin 重新锚定模型，同时保持真实比例。把原始 bounds 和 transform offset 写到分析 manifest。
4. 按 FrameRenderer `ObjLoader` 的 V 坐标约定写 UV。
5. 输出到 `Content/Models/<AssetName>/`。

### 4. 导出 textures

1. 只复制运行时需要的纹理到 `Content/Textures/<AssetName>/`。
2. 按用途和 resource id 命名，例如 `102006_basecolor_alpha.dds` 或 `102010_normal.dds`。
3. FrameRenderer 能加载 DDS 时优先保留 DDS；只有 loader 不支持源格式时才转换。
4. 只有截帧缺少资源或当前渲染器尚不支持该插槽时，才绑定 fallback 纹理。
5. 辅助纹理或暂不支持的资源要写进分析 manifest，不要静默丢弃。

### 5. 构建 material

1. 根据 shader 证据选择最接近的现有 `shadingMode`。
2. 如果是游戏专用材质，需要新增专用 schema 和 runtime mode：
   - 在编辑器 / 材质模型中注册 schema
   - 读写 v2 JSON 参数和纹理插槽
   - 在 Material Editor 中暴露控件
   - 把 mode、params、textures 传给 `SceneRenderer`
   - 在共享 HLSL helper 中实现 decode 和 shading
   - 保持 Forward 和 Deferred 的视觉语义一致
3. 材质保存到 `Content/Materials/<AssetName>/<name>.material.json`。
4. 每个已证实的纹理插槽都用相对 `Content` 路径绑定。
5. shader 推导出的常量写进 `params`，命名要清楚，并给出默认值。

### 6. 创建或更新 Level

1. 在 `Content/Levels/<asset_or_capture>.level.json` 创建聚焦验证关卡。
2. 添加还原出的 mesh actor，并绑定具体材质路径。
3. 如果截帧外观依赖光照，添加或复用 `Sun Light` 和 `Sky Atmosphere`。
4. 设置适合观察的 actor transform，保证启动后模型立刻可见。
5. 只有聚焦关卡验证通过后，才根据用户要求更新 `Content/Levels/default_renderdoc_scene.level.json`。

### 7. 在 FrameRenderer 中验证

Release 编译：

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' build_vs\DX12HelloTriangle.sln /p:Configuration=Release /p:Platform=x64 /m
```

指定 Level 并截取一帧：

```powershell
$env:SHELLENGINE_START_LEVEL = 'Content/Levels/<level>.level.json'
$env:SHELLENGINE_CAPTURE_FRAME_PATH = "$env:TEMP\fr_<asset>_validation.png"
$env:SHELLENGINE_CAPTURE_FRAME_INDEX = '5'
.\build_vs\x64\Release\dx12_hello.exe
```

检查截图并确认：

- 还原对象是真实 geometry，不是代理图片
- 除非已记录，否则材质插槽不应落入 missing-texture fallback
- alpha、culling、depth 行为符合截帧意图
- Forward / Deferred 不应该改变模型比例或位置
- editor outliner、details、content drawer 仍能正常打开和交互
- 自动验证结束后没有残留 `dx12_hello.exe` 进程

## 证据产物

每个导入的截帧对象都要有紧凑的分析证据。默认把可同步的证据写到 import manifest：

- `Content/Models/<AssetName>/<event>_import_manifest.json`：记录精确 counts、bounds、resource ids、导入参数

纯分析文档可以保留在本地，但不要默认提交到 Git，除非用户明确要求同步这些分析笔记。

证据至少应该包含：

- source capture / manifest path
- event id 和 draw label
- mesh counts 和 bounds
- shader id 或 disassembly 引用
- texture resource id 到 Content path 的映射
- constant buffer / uniform 发现
- 选择的 `shadingMode` 以及原因
- 假设、fallback 和已知差距
- 验证命令和截图路径

## 完成清单

- 修改前已查看 `git status --short`
- mesh、texture、material、level 都位于 `Content`
- material 是 v2 JSON，每个可渲染 actor 都有具体 material path
- 截帧证据已记录在 import manifest
- Release 编译通过
- 已生成并检查验证截图
- 新资源影响到的 UI / editor 冒烟路径仍正常
- 最终回复列出变更文件、capture event id、shadingMode、验证命令、截图路径和剩余 fidelity 差距

