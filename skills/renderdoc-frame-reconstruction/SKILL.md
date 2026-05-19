---
name: renderdoc-frame-reconstruction
description: Use this when reconstructing RenderDoc frame-capture data into FrameRenderer as real mesh, texture, material, shader, level, and validation artifacts rather than screenshot proxies.
---

# RenderDoc Frame Reconstruction

Use this skill to take a RenderDoc capture, or an existing RenderDoc export/manifest, and make the captured draw render inside FrameRenderer with faithful geometry, textures, uniforms, shader behavior, and an editor level.

## Ground Rules

- Treat the RenderDoc capture as the source of truth. Do not replace a captured object with a generic model, baked screenshot, or visual approximation unless the limitation is explicitly documented.
- Reconstruct the draw from evidence: event id, draw topology, input/output mesh data, texture resource ids, shader ids, constant buffers, render states, and visible validation screenshots.
- Keep FrameRenderer architecture cohesive. Prefer Content assets and material schema support first; add renderer or HLSL code only when the captured shading cannot be expressed by an existing shading mode.
- Preserve existing user work. Inspect `git status --short` before edits and avoid reverting unrelated changes.

## FrameRenderer Conventions

- Content lives under `Content/Models`, `Content/Textures`, `Content/Materials`, and `Content/Levels`.
- Material files use v2 JSON:

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

- Level files reference assets and materials with paths relative to `Content`, such as `Models/Foo/foo.obj` and `Materials/Foo/foo.material.json`.
- Every renderable level object must bind a concrete `.material.json`. Use `Materials/Default/default_pbr.material.json` only when the capture has no usable material evidence.
- `ObjLoader` flips texture V on import, so write OBJ texture coordinates as `vt u 1-v` when exporting from RenderDoc-style UVs.
- Known built-in material modes include `PbrLit`, `Unlit`, `Rdr2Rock`, and `Rdr2Foliage`. Add a new schema only when the capture requires different texture slots or decode/shading semantics.

## Workflow

### 1. Inventory the capture

1. Find the `.rdc`, exported manifests, extracted buffers, shader files, and texture dumps. Use targeted searches such as `rg --files` from the repo and known capture folders.
2. Identify the target draw or object. Prefer explicit event ids or labels. Otherwise use mesh bounds, vertex/index counts, alpha mode, texture names/resource ids, and shader signatures.
3. Record a short evidence trail before importing anything:
   - capture path or manifest path
   - event id / draw id
   - draw topology and index/vertex counts
   - vertex attributes used by the shader
   - bound textures and resource ids
   - shader ids, entry points, or disassembly paths
   - constant buffers/uniforms that affect placement, lighting, BRDF, alpha, wind, or material decode

### 2. Analyze the selected draw

Use RenderDoc replay data, structured export data, or existing manifests to extract:

- Mesh data: positions, normals, tangents, UV sets, colors, indices, primitive topology, and world-space bounds.
- Material inputs: base color/albedo, normal, roughness, metallic, AO, mask, opacity, transmission, subsurface, wind, terrain blend, or other game-specific slots.
- Shader behavior: BRDF branch, alpha test, two-sided handling, normal map decode, mask channel usage, emissive/unlit branch, material constants, and texture coordinate transforms.
- Render states: culling, depth test/write, blend state, alpha cutoff, shadow participation, and whether the draw is forward, deferred, or a special pass.
- Lighting/environment dependencies: sun direction/intensity, sky/IBL assumptions, exposure/tonemap, and any capture-specific constants needed to match appearance.

Document uncertainties instead of hiding them. If replay cannot run, use structured capture data, shader disassembly, resource names, and exported bind information.

### 3. Export geometry

1. Convert the captured mesh into OBJ unless a richer project mesh path is already required.
2. Preserve the actual vertex attributes used by the shader. If OBJ cannot store an attribute, document it in an import manifest and only approximate it when the renderer path does not use it.
3. Reanchor the mesh around a sensible local origin while preserving scale. Store the source bounds and any transform offset in the analysis manifest.
4. Write UVs with the V convention expected by FrameRenderer's `ObjLoader`.
5. Place the result in `Content/Models/<AssetName>/`.

### 4. Export textures

1. Copy only runtime-needed textures to `Content/Textures/<AssetName>/`.
2. Name textures by role and resource id, for example `102006_basecolor_alpha.dds` or `102010_normal.dds`.
3. Preserve DDS format when FrameRenderer can load it. Convert only when the loader cannot support the source format.
4. Bind fallback textures only when the capture lacks a resource or the renderer does not yet support that slot.
5. Keep auxiliary or unsupported resources in the analysis manifest instead of silently ignoring them.

### 5. Build the material

1. Choose the closest existing shading mode based on the shader evidence.
2. For a game-specific material, add a dedicated schema and runtime mode:
   - register the schema in the editor/material model
   - load/save v2 JSON params and texture slots
   - expose controls in the material editor
   - pass mode/params/textures to `SceneRenderer`
   - implement decode and shading in shared HLSL helpers
   - keep forward and deferred visual semantics consistent
3. Store the material at `Content/Materials/<AssetName>/<name>.material.json`.
4. Bind every proven texture slot by relative Content path.
5. Put shader-derived constants in `params` with clear names and defaults.

### 6. Create or update a level

1. Create a focused level at `Content/Levels/<asset_or_capture>.level.json`.
2. Add the reconstructed mesh actor with its concrete material path.
3. Add or reuse `Sun Light` and `Sky Atmosphere` actors when the captured appearance depends on lighting.
4. Set camera-friendly actor transforms so the object is visible immediately on startup.
5. If the user asks for default startup content, update `Content/Levels/default_renderdoc_scene.level.json` only after the focused level is validated.

### 7. Validate in FrameRenderer

Build Release:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' build_vs\DX12HelloTriangle.sln /p:Configuration=Release /p:Platform=x64 /m
```

Launch a specific level and capture a frame:

```powershell
$env:SHELLENGINE_START_LEVEL = 'Content/Levels/<level>.level.json'
$env:SHELLENGINE_CAPTURE_FRAME_PATH = "$env:TEMP\fr_<asset>_validation.png"
$env:SHELLENGINE_CAPTURE_FRAME_INDEX = '5'
.\build_vs\x64\Release\dx12_hello.exe
```

Inspect the screenshot and confirm:

- the reconstructed object is real geometry, not a proxy image
- material slots resolve without missing-texture fallbacks unless documented
- alpha/culling/depth behavior matches the capture intent
- forward/deferred paths do not visibly change scale or object placement
- the editor outliner/details/content drawer still open and interact normally
- no `dx12_hello.exe` process remains after automated validation

## Evidence Artifacts

Add a compact analysis artifact for every imported capture object. Prefer:

- `Docs/<AssetName>/<event>_analysis.md` for human-readable shader/material reasoning
- `Content/Models/<AssetName>/<event>_import_manifest.json` for exact counts, bounds, resource ids, and import parameters

The evidence should include:

- source capture/manifest path
- event id and draw label
- mesh counts and bounds
- shader ids or disassembly references
- texture resource id to Content path mapping
- constant buffer/uniform findings
- chosen shading mode and why
- assumptions, fallbacks, and known gaps
- validation command and screenshot path

## Completion Checklist

- `git status --short` reviewed before edits.
- Mesh, textures, material, and level are all under `Content`.
- Material is v2 JSON and every renderable actor has a concrete material path.
- Capture evidence is documented in `Docs` or an import manifest.
- Release build passes.
- A validation screenshot was produced and inspected.
- UI/editor smoke paths affected by the new asset still work.
- Final response lists changed files, capture event ids, shading mode, validation command, screenshot path, and any remaining fidelity gaps.

