#!/usr/bin/env python3
"""Import non-water local-scene RenderDoc export data into editor Content.

The source manifest is expected to come from the water RenderDoc analysis
export. It already excludes the water runtime meshes and contains opaque scene
draws with world-space positions. This tool converts those draw exports into a
small set of material-batched OBJ files and a level that the Win32 editor can
load without adding a capture-specific runtime renderer path.
"""

from __future__ import annotations

import argparse
import json
import math
import shutil
import struct
import subprocess
from collections import defaultdict
from pathlib import Path


def read_floats(path: Path, columns: int) -> list[tuple[float, ...]]:
    data = path.read_bytes()
    stride = columns * 4
    if len(data) % stride != 0:
        raise ValueError(f"{path} size {len(data)} is not divisible by {stride}")
    count = len(data) // stride
    return [struct.unpack_from("<" + "f" * columns, data, i * stride) for i in range(count)]


def read_indices(path: Path, index_count: int) -> list[int]:
    data = path.read_bytes()
    if len(data) == index_count * 2:
        return list(struct.unpack("<" + "H" * index_count, data))
    if len(data) == index_count * 4:
        return list(struct.unpack("<" + "I" * index_count, data))
    raise ValueError(f"{path} size {len(data)} does not match {index_count} indices")


def safe_name(text: str) -> str:
    keep = []
    for ch in text:
        if ch.isalnum() or ch in ("_", "-"):
            keep.append(ch)
        else:
            keep.append("_")
    return "".join(keep).strip("_") or "asset"


def rel_content(path: Path, content_root: Path) -> str:
    return path.relative_to(content_root).as_posix()


def average_color(root: Path, entry: dict) -> tuple[float, float, float]:
    color_path = entry.get("mesh", {}).get("color0_path", "")
    if not color_path:
        return (0.72, 0.72, 0.72)
    full = root / color_path
    if not full.exists():
        return (0.72, 0.72, 0.72)
    data = full.read_bytes()
    if len(data) < 16:
        return (0.72, 0.72, 0.72)
    count = len(data) // 16
    sums = [0.0, 0.0, 0.0]
    for i in range(count):
        rgba = struct.unpack_from("<4f", data, i * 16)
        for c in range(3):
            sums[c] += rgba[c]
    return tuple(max(0.04, min(1.0, sums[c] / count)) for c in range(3))


def color_bucket(color: tuple[float, float, float]) -> tuple[float, float, float]:
    return tuple(round(c * 5.0) / 5.0 for c in color)


def entry_group_key(root: Path, entry: dict) -> tuple[str, str, tuple[float, float, float]]:
    base = entry.get("base_color_texture") or ""
    if base:
        tex_id = Path(base).stem
        return ("tex", tex_id, (1.0, 1.0, 1.0))
    bucket = color_bucket(average_color(root, entry))
    return ("color", f"{bucket[0]:.1f}_{bucket[1]:.1f}_{bucket[2]:.1f}", bucket)


def transform_position(
    pos: tuple[float, float, float],
    center_x: float,
    center_z: float,
    min_y: float,
    scale: float,
    z_offset: float,
    y_offset: float,
) -> tuple[float, float, float]:
    return (
        (pos[0] - center_x) * scale,
        (pos[1] - min_y) * scale + y_offset,
        (pos[2] - center_z) * scale + z_offset,
    )


def write_group_obj(
    root: Path,
    entries: list[dict],
    output: Path,
    center_x: float,
    center_z: float,
    min_y: float,
    scale: float,
    z_offset: float,
    y_offset: float,
    double_sided: bool,
) -> dict:
    output.parent.mkdir(parents=True, exist_ok=True)
    vertex_total = 0
    index_total = 0
    with output.open("w", encoding="utf-8", newline="\n") as f:
        f.write("# Generated from RenderDoc water local-scene export; water draws excluded.\n")
        f.write("# Positions are recentered and uniformly scaled for this editor level.\n")
        vertex_offset = 0
        for entry in entries:
            mesh = entry["mesh"]
            positions = read_floats(root / mesh["positions_path"], 3)
            normals_path = root / mesh.get("normals_path", "")
            uv_path = root / mesh.get("uv_path", "")
            normals = read_floats(normals_path, 3) if normals_path.exists() else []
            uvs = read_floats(uv_path, 2) if uv_path.exists() else []
            indices = read_indices(root / mesh["indices_path"], int(mesh["index_count"]))

            f.write(f"o {safe_name(entry.get('name', 'draw'))}_{entry.get('event_id', 0)}\n")
            f.write(f"# event_id={entry.get('event_id', 0)} draw_id={entry.get('draw_id', 0)} mesh_mode={entry.get('mesh_mode', '')}\n")
            for p in positions:
                x, y, z = transform_position(p, center_x, center_z, min_y, scale, z_offset, y_offset)
                f.write(f"v {x:.7f} {y:.7f} {z:.7f}\n")
            for uv in uvs if uvs else [(0.0, 0.0)] * len(positions):
                f.write(f"vt {uv[0]:.7f} {1.0 - uv[1]:.7f}\n")
            for n in normals if normals else [(0.0, 1.0, 0.0)] * len(positions):
                length = math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) or 1.0
                f.write(f"vn {n[0] / length:.7f} {n[1] / length:.7f} {n[2] / length:.7f}\n")
            triangle_index_count = (len(indices) // 3) * 3
            for i in range(0, triangle_index_count, 3):
                a, b, c = indices[i], indices[i + 1], indices[i + 2]
                f.write(
                    f"f {a + 1 + vertex_offset}/{a + 1 + vertex_offset}/{a + 1 + vertex_offset} "
                    f"{b + 1 + vertex_offset}/{b + 1 + vertex_offset}/{b + 1 + vertex_offset} "
                    f"{c + 1 + vertex_offset}/{c + 1 + vertex_offset}/{c + 1 + vertex_offset}\n"
                )
                if double_sided:
                    f.write(
                        f"f {c + 1 + vertex_offset}/{c + 1 + vertex_offset}/{c + 1 + vertex_offset} "
                        f"{b + 1 + vertex_offset}/{b + 1 + vertex_offset}/{b + 1 + vertex_offset} "
                        f"{a + 1 + vertex_offset}/{a + 1 + vertex_offset}/{a + 1 + vertex_offset}\n"
                    )
            vertex_offset += len(positions)
            vertex_total += len(positions)
            index_total += triangle_index_count * (2 if double_sided else 1)
    return {"vertices": vertex_total, "indices": index_total}


def convert_texture(ffmpeg: str, source: Path, output: Path) -> bool:
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists() and output.stat().st_size > 0:
        return True
    try:
        subprocess.run(
            [ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-i", str(source), str(output)],
            check=True,
        )
        return output.exists() and output.stat().st_size > 0
    except (OSError, subprocess.CalledProcessError):
        return False


def write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    default_source = Path(r"C:\Users\hsx54\Documents\RDC\build_v143\Release\assets")
    parser.add_argument("--source-root", type=Path, default=default_source)
    parser.add_argument("--manifest", type=Path, default=None)
    parser.add_argument("--project-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--level-name", default="RenderDocWaterScene_NoWater")
    parser.add_argument("--scale", type=float, default=0.008)
    parser.add_argument("--z-offset", type=float, default=8.5)
    parser.add_argument("--y-offset", type=float, default=-2.15)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--single-sided", action="store_true", help="Do not duplicate reversed faces in OBJ output.")
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    manifest_path = args.manifest.resolve() if args.manifest else source_root / "CaptureLocalSceneManifest.json"
    project_root = args.project_root.resolve()
    content_root = project_root / "Content"

    data = json.loads(manifest_path.read_text(encoding="utf-8"))
    entries = [e for e in data.get("entries", []) if "water" not in json.dumps(e).lower()]
    if not entries:
        raise RuntimeError(f"No non-water entries found in {manifest_path}")

    bounds = data.get("movement_bounds") or {}
    bmin = bounds.get("min")
    bmax = bounds.get("max")
    if not bmin or not bmax:
        mins = [float("inf"), float("inf"), float("inf")]
        maxs = [float("-inf"), float("-inf"), float("-inf")]
        for entry in entries:
            wb = entry.get("world_bounds") or {}
            for i, v in enumerate(wb.get("min", [])):
                mins[i] = min(mins[i], float(v))
            for i, v in enumerate(wb.get("max", [])):
                maxs[i] = max(maxs[i], float(v))
        bmin, bmax = mins, maxs

    center_x = (float(bmin[0]) + float(bmax[0])) * 0.5
    center_z = (float(bmin[2]) + float(bmax[2])) * 0.5
    min_y = float(bmin[1])

    groups: dict[tuple[str, str, tuple[float, float, float]], list[dict]] = defaultdict(list)
    for entry in entries:
        groups[entry_group_key(source_root, entry)].append(entry)

    model_dir = content_root / "Models" / "RenderDocWaterScene"
    tex_dir = content_root / "Textures" / "RenderDocWaterScene"
    mat_dir = content_root / "Materials" / "RenderDocWaterScene"
    level_path = content_root / "Levels" / f"{args.level_name}.level.json"
    report_path = content_root / "Levels" / f"{args.level_name}.import_report.json"

    objects = []
    report_groups = []
    object_id = 1

    for key, group_entries in sorted(groups.items(), key=lambda item: item[0]):
        kind, label, fallback_color = key
        asset_base = safe_name(f"rdc_scene_{kind}_{label}")
        obj_path = model_dir / f"{asset_base}.obj"
        mat_path = mat_dir / f"{asset_base}.material.json"
        mesh_stats = write_group_obj(
            source_root,
            group_entries,
            obj_path,
            center_x,
            center_z,
            min_y,
            args.scale,
            args.z_offset,
            args.y_offset,
            not args.single_sided,
        )

        albedo_texture = ""
        albedo = list(fallback_color)
        if kind == "tex":
            first = next((e for e in group_entries if e.get("base_color_texture")), None)
            if first:
                src = source_root / first["base_color_texture"]
                png_path = tex_dir / f"{asset_base}_albedo.png"
                if src.exists() and convert_texture(args.ffmpeg, src, png_path):
                    albedo_texture = rel_content(png_path, content_root)
                else:
                    albedo = [0.78, 0.78, 0.78]
        else:
            # Use the average of all capture vertex colors in this no-texture group.
            colors = [average_color(source_root, e) for e in group_entries]
            albedo = [sum(c[i] for c in colors) / len(colors) for i in range(3)]

        write_json(
            mat_path,
            {
                "type": "Material",
                "name": asset_base,
                "albedo": albedo,
                "metallic": 0.0,
                "roughness": 0.72,
                "textures": {
                    "albedo": albedo_texture,
                    "normal": "",
                    "roughness": "",
                    "metallic": "",
                    "ao": "",
                },
            },
        )

        objects.append(
            {
                "id": object_id,
                "name": asset_base,
                "type": "Static Mesh",
                "asset": rel_content(obj_path, content_root),
                "material": rel_content(mat_path, content_root),
                "position": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
                "albedo": albedo,
                "metallic": 0.0,
                "roughness": 0.72,
                "lightIntensity": 3.0,
                "skyEnabled": True,
                "rayleighScale": 1.0,
                "mieScale": 1.0,
                "mieG": 0.8,
                "atmosphereHeight": 12.0,
            }
        )
        report_groups.append(
            {
                "name": asset_base,
                "draw_count": len(group_entries),
                "event_ids": [e.get("event_id") for e in group_entries],
                "mesh": mesh_stats,
                "material": rel_content(mat_path, content_root),
                "albedo_texture": albedo_texture,
            }
        )
        object_id += 1

    objects.append(
        {
            "id": object_id,
            "name": "Sun Light",
            "type": "Sun Light",
            "asset": "",
            "material": "",
            "position": [6.0, 8.0, -6.0],
            "scale": [0.18, 0.18, 0.18],
            "albedo": [1.0, 0.82, 0.18],
            "metallic": 0.0,
            "roughness": 0.35,
            "lightIntensity": 3.5,
            "skyEnabled": True,
            "rayleighScale": 1.0,
            "mieScale": 1.0,
            "mieG": 0.8,
            "atmosphereHeight": 12.0,
        }
    )
    object_id += 1
    objects.append(
        {
            "id": object_id,
            "name": "Sky Atmosphere",
            "type": "Sky Atmosphere",
            "asset": "",
            "material": "",
            "position": [-2.5, 1.6, -2.5],
            "scale": [0.28, 0.28, 0.28],
            "albedo": [0.2, 0.45, 0.95],
            "metallic": 0.0,
            "roughness": 0.75,
            "lightIntensity": 3.0,
            "skyEnabled": True,
            "rayleighScale": 1.0,
            "mieScale": 1.0,
            "mieG": 0.8,
            "atmosphereHeight": 12.0,
        }
    )

    write_json(
        level_path,
        {
            "type": "Level",
            "name": args.level_name,
            "sun": {"yaw": 0.785398, "pitch": -0.65, "intensity": 3.5},
            "objects": objects,
        },
    )
    write_json(
        report_path,
        {
            "source_manifest": str(manifest_path),
            "source_entry_count": data.get("entry_count", len(entries)),
            "imported_entry_count": len(entries),
            "source_skipped_count": data.get("skipped_count", 0),
            "group_count": len(report_groups),
            "level": rel_content(level_path, content_root),
            "transform": {
                "center_x": center_x,
                "center_z": center_z,
                "min_y": min_y,
                "scale": args.scale,
                "z_offset": args.z_offset,
                "y_offset": args.y_offset,
                "double_sided": not args.single_sided,
            },
            "groups": report_groups,
        },
    )

    print(f"Imported {len(entries)} non-water draws into {len(report_groups)} static mesh objects.")
    print(f"Level: {level_path}")
    print(f"Report: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
