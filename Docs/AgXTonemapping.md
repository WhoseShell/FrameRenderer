# AgX Tonemapping Implementation Notes

## Goal

Add an AgX tonemapping option to FrameRenderer's HDR post-process path while keeping the existing Reinhard operator available for comparison.

Reference target:

- YouTube: https://www.youtube.com/watch?v=-ozCZf6R2r0
- Public shader reference used for constants and structure: three.js `AgXToneMapping`, which documents its path as based on Filament and Blender's AgX implementation.

## Pipeline Placement

AgX runs in the existing fullscreen `Tonemap` pass:

1. Scene renders into the HDR color target.
2. `TonemapCB` carries exposure, display gamma, enable flag, and `TonemapOperator`.
3. `tonemap.hlsl` samples HDR scene color.
4. If tonemapping is enabled:
   - `Reinhard` keeps the previous simple curve.
   - `AgX` uses a log exposure domain and contrast approximation.
5. The output is gamma corrected for the swapchain.

## AgX Steps

The implemented AgX path follows this structure:

1. Convert linear sRGB into linear Rec.2020.
2. Apply the AgX inset matrix to move the color into the AgX working domain.
3. Convert scene-linear radiance to a normalized log2 exposure range:
   - min EV: `-12.47393`
   - max EV: `4.026069`
4. Apply the default AgX contrast polynomial.
5. Apply the AgX outset matrix.
6. Apply the AgX display power approximation.
7. Convert linear Rec.2020 back to linear sRGB.
8. Clamp to displayable range before final gamma correction.

## Why This Is Not Just Reinhard

Reinhard compresses HDR with `x / (x + 1)`, which is compact but often desaturates highlights and has little control over perceptual contrast.

AgX instead works in a bounded log exposure domain before applying a fitted contrast curve. This gives a more filmic shoulder, better highlight color retention, and a clearer separation between scene-linear lighting and display output.

## Editor Control

Render Settings now exposes:

- `Tonemap`: on/off
- `Tonemap Operator`: `Reinhard` or `AgX`

The current default is `AgX`, while `Reinhard` remains available as the legacy comparison path.
