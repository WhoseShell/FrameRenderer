# RenderDoc Rock Asset Pack

This pack is the minimal RenderDoc-derived data used by the runtime rock renderer.

- Source capture: `C:/Users/hsx54/Desktop/water2-high.rdc`
- Source manifest: `C:/Users/hsx54/Documents/RDC/assets/CaptureLocalSceneManifest.json`
- Draw/event: `event_id=16731`, `draw_id=1197`, material model `OpaqueLit`
- Mesh: exported IA world-space positions, normals, UV0, color streams, and R32 indices
- Textures: all eight DDS resources bound on the selected draw
- Shaders: original RenderDoc-exported VS/PS bytecode blobs are preserved for evidence

The runtime code intentionally uses a source-equivalent shader instead of replaying the full captured command stream. It keeps the capture bytes and binding metadata in `CaptureRockManifest.json` while replacing only camera and presentation state with live engine values.
