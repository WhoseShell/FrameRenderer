# RenderDoc Plant Event 18024

Source data comes from `C:\Users\hsx54\Documents\RDC\assets\CaptureLocalSceneManifest.json`, entry `event_id=18024`, draw `1384`.

## Capture Evidence

- Mesh: 394 vertices, 588 indices, triangle list.
- World bounds: min `(2340.7515, 475.5015, -129.4420)`, max `(2343.9829, 479.8333, -126.0945)`.
- Material model: `OpaqueLit`.
- Alpha mode: `cutout`.
- Vertex shader: `capture/local_scene_precise/shaders/vertex_1000000000000001127.bin`.
- Pixel shader: `capture/local_scene_precise/shaders/pixel_1000000000000001128.bin`.

## Shader Interpretation

The pixel shader declares `SV_IsFrontFace`, samples the base color/alpha texture at `T3[13]`, and performs alpha-threshold `discard` before writing render targets. It also samples `T5[94]` as a mask/gloss texture and `T6[95]` as normal data. This is a foliage cutout material rather than an ordinary opaque rock or mesh.

## Imported Runtime Mapping

- Mesh: `Content/Models/RenderDocPlant/renderdoc_plant_event_18024.obj`.
- Base color + alpha: `Content/Textures/RenderDocPlant/102006_basecolor_alpha.dds`.
- Normal: `Content/Textures/RenderDocPlant/102010_normal.dds`.
- Mask: `Content/Textures/RenderDocPlant/102008_mask.dds`.
- Material: `Content/Materials/RenderDocPlant/renderdoc_plant_event_18024.material.json`.
- Level: `Content/Levels/renderdoc_plant_event_18024.level.json`.

Runtime shading uses `Rdr2Foliage`: two-sided rasterization, alpha cutout from base-color alpha, vertex color tint preserved from RenderDoc `COLOR0`, normal-map contribution, and roughness derived from the captured mask texture.
