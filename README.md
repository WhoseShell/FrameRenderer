# FrameRenderer

FrameRenderer is a DirectX 12 editor and renderer focused on reconstructing real RenderDoc frame-capture data as editable runtime scenes. The current branch turns capture evidence into project assets: meshes, textures, material shading modes, shader decode paths, levels, and validation screenshots.

The goal is not to show a baked screenshot. The goal is to import captured resources into the renderer so the object can be selected, moved, assigned materials, saved in a level, and rendered again.

## Current Branch

This branch contains the RenderDoc reconstruction baseline:

- Default startup level: `Content/Levels/default_renderdoc_scene.level.json`.
- Default scene content: one captured rock, one captured plant, `Sun Light`, and `Sky Atmosphere`.
- Captured rock material mode: `Rdr2Rock`.
- Captured plant material mode: `Rdr2Foliage`.
- Material system: v2 `.material.json` with `shadingMode`, `params`, and `textures`.
- Editor UI: ImGui-based editor panels for Place Actors, Level Outliner, Details, Content Drawer, material editing, and render settings.
- Repo-local AI skill: `skills/renderdoc-frame-reconstruction/SKILL.md`.

## RenderDoc Reconstruction Skill

The branch includes a Codex skill for future AI agents:

```text
skills/renderdoc-frame-reconstruction/
  SKILL.md
  agents/openai.yaml
```

Use this skill when the task is to take a RenderDoc `.rdc`, exported RenderDoc manifest, or captured resource dump and reconstruct it inside FrameRenderer.

The skill covers:

- selecting and documenting the target draw/event
- extracting geometry, texture bindings, shaders, uniforms, and render states
- creating `Content/Models`, `Content/Textures`, `Content/Materials`, and `Content/Levels` assets
- adding or reusing material shading modes such as `PbrLit`, `Unlit`, `Rdr2Rock`, and `Rdr2Foliage`
- validating the result with a real rendered screenshot

## Imported Capture Assets

### RenderDoc Rock

- Mesh: `Content/Models/RDR2Rock/rdr2_capture_rock.obj`
- Material: `Content/Materials/RDR2Rock/rdr2_capture_rock.material.json`
- Textures: `Content/Textures/RDR2Rock/*.dds`
- Shading mode: `Rdr2Rock`

### RenderDoc Plant

- Source evidence: `Docs/RenderDocPlant/event_18024_analysis.md`
- Import manifest: `Content/Models/RenderDocPlant/event_18024_import_manifest.json`
- Mesh: `Content/Models/RenderDocPlant/renderdoc_plant_event_18024.obj`
- Material: `Content/Materials/RenderDocPlant/renderdoc_plant_event_18024.material.json`
- Textures:
  - `Content/Textures/RenderDocPlant/102006_basecolor_alpha.dds`
  - `Content/Textures/RenderDocPlant/102008_mask.dds`
  - `Content/Textures/RenderDocPlant/102010_normal.dds`
- Shading mode: `Rdr2Foliage`
- Capture event: `event_id=18024`, draw `1384`

## Build

Recommended Release build:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' build_vs\DX12HelloTriangle.sln /p:Configuration=Release /p:Platform=x64 /m
```

Alternative CMake generation:

```powershell
cmake -S . -B build_vs -G "Visual Studio 17 2022" -A x64
cmake --build build_vs --config Release
```

The executable is:

```text
build_vs/x64/Release/dx12_hello.exe
```

## Run

Run the default RenderDoc scene:

```powershell
.\build_vs\x64\Release\dx12_hello.exe
```

Run a specific level:

```powershell
$env:SHELLENGINE_START_LEVEL = 'Content/Levels/renderdoc_plant_event_18024.level.json'
.\build_vs\x64\Release\dx12_hello.exe
```

Capture a validation screenshot:

```powershell
$env:SHELLENGINE_START_LEVEL = 'Content/Levels/default_renderdoc_scene.level.json'
$env:SHELLENGINE_CAPTURE_FRAME_PATH = "$env:TEMP\fr_default_renderdoc_scene.png"
$env:SHELLENGINE_CAPTURE_FRAME_INDEX = '5'
.\build_vs\x64\Release\dx12_hello.exe
```

## Editor Controls

- Right mouse drag: rotate/fly the camera.
- `WASDQE`: move camera while flying.
- `Shift`: faster camera movement.
- Mouse wheel while flying: adjust camera speed.
- Left click: select objects or gizmo handles.
- `W`: translate gizmo.
- `R`: scale gizmo.
- `Delete`: delete selected object.
- Place Actors: add basic meshes, environment actors, and RenderDoc actors.
- Level Outliner: select level objects.
- Details: edit transform and assign a concrete material to renderable objects.
- Content Drawer: browse models, textures, materials, and levels.
- Material Editor: edit material `shadingMode`, parameters, and texture slots.
- Render Settings: switch render path and global render options.

## Content Layout

```text
Content/
  Levels/
  Materials/
    Default/
    RDR2Rock/
    RenderDocPlant/
  Models/
    RDR2Rock/
    RenderDocPlant/
  Textures/
    RDR2Rock/
    RenderDocPlant/
Docs/
  RenderDocPlant/
skills/
  renderdoc-frame-reconstruction/
src/
shaders/
tools/
```

## Material Modes

- `PbrLit`: standard lit PBR material.
- `Unlit`: color/intensity material that bypasses lighting.
- `Rdr2Rock`: captured rock reconstruction mode.
- `Rdr2Foliage`: captured foliage mode with two-sided rendering, alpha cutout, vertex color tint, normal input, and mask-derived roughness.

All renderable scene objects should reference a concrete `.material.json`. The fallback material is `Content/Materials/Default/default_pbr.material.json`.

## Verification Checklist

Before considering a RenderDoc import finished:

- Release build passes.
- The object is imported as real mesh data.
- Required textures are copied into `Content/Textures`.
- A v2 material is created and assigned.
- A focused level is created under `Content/Levels`.
- Capture evidence is documented under `Docs` or an import manifest.
- A screenshot is captured from the running renderer.
- The editor can select, move, inspect, and save the imported object.

## Scope

FrameRenderer is still a compact reconstruction renderer, not a full UE5 replacement. The project favors readable, focused systems that make RenderDoc capture data easy to inspect, import, iterate, and validate.
