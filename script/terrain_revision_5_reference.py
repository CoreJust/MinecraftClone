"""Record and verify the independent Terrain-v2 revision-5 surface oracle."""
from __future__ import annotations

import argparse
import ast
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import re
import struct
import sys
import tempfile
from typing import Any
import zipfile

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from script import terrain_reference
from script import terrain_revision_4_reference as revision4

SCHEMA = "minecraftclone.terrain-v2-model-revision-5-local-surface-reference.v1"
REFERENCE_MANIFEST_SHA256 = "593da126bfebecc56f2412424aba50a160105a3471d2b5c58f2c1146108294f2"
ORACLE_SOURCE_SHA256 = "b1b385c146b5be17187695bfb75fb7886e020aa4a48639b58467e5c3c821975d"
REVISION4_MANIFEST_SHA256 = "9a2644bbe81a88876bac4f84f334f8c0a5d3525295e83122dcce10f92f02d6eb"
REVISION3_MANIFEST_SHA256 = "d0e6ade7aed51a34a864c1dc497d3b3921d10204a8cc3602b71f7d53ab96db97"
SOURCE_MAP_SHA256 = "5de753a4068c71d742f5ce7e75e4e8bcabbaad5fddaf6c8404dc41ba00d45704"
REVISION4_ARCHIVE_SHA256 = "690393f062e5e2d78c3bfc495066d8150165068b486731d46354d56427d55a6d"
SEEDS = (0, 1, 42, 777, 2004, 8800555)
WORLD_SIZE = 65_536
WORLD_DEPTH = 1_024
BLOCK_SEA_LEVEL = 384
MAX_HEIGHT = 1_020
ATLAS_RESOLUTION = 256
REGIONAL_EXTENTS = (16_384, 4_096, 1_024, 256, 64)
REGIONAL_AMPLITUDES_KM = (0.18, 0.09, 0.045, 0.022, 0.011)
LOCAL_EXTENT = 16
LOCAL_AMPLITUDE_KM = 0.044
LOCAL_PARENT_EXTENT = 64
HASH_MASK = (1 << 64) - 1
HASH_X = 0x9E3779B185EBCA87
HASH_Y = 0xC2B2AE3D27D4EB4F
HASH_LEVEL = 0x165667B19E3779F9
HASH_MIX_1 = 0xBF58476D1CE4E5B9
HASH_MIX_2 = 0x94D049BB133111EB

FULL_FIELD_NAMES = (
    "atlas__elevation",
    "atlas__temperature",
    "atlas__water_level",
    "atlas__ocean_mask",
    "atlas__lake_mask",
    "atlas__river_mask",
)
FIELD_DESCRIPTORS: dict[str, tuple[str, str, str]] = {
    "point_xy": ("<u4", "world_points_xy", "integer world block coordinates [x,y]"),
    "height_v4": ("<i4", "revision4_height_blocks", "revision-4 highest solid block Z"),
    "height_v5": ("<i4", "revision5_height_blocks", "revision-5 highest solid block Z"),
    "top_material_v4": ("|u1", "revision4_top_material", "Terrain-v2 material ID"),
    "ground_material_v4": ("|u1", "revision4_ground_material", "Terrain-v2 material ID"),
    "top_material_v5": ("|u1", "revision5_top_material", "Terrain-v2 material ID"),
    "ground_material_v5": ("|u1", "revision5_ground_material", "Terrain-v2 material ID"),
    "elevation_v4": ("<f8", "revision4_surface_elevation", "geological kilometres"),
    "elevation_v5": ("<f8", "revision5_surface_elevation", "geological kilometres"),
    "local_detail": ("<f8", "revision5_local_detail", "local 16-block elevation contribution in kilometres"),
    "input_precipitation": (
        "<f4",
        "revision5_atlas_precipitation_input",
        "revision-4-approved global atlas precipitation, full-field checked by native tests",
    ),
}
ITEM_SIZES = {"<u4": 4, "<i4": 4, "<f8": 8, "<f4": 4, "|u1": 1}


class ReferenceError(ValueError):
    """The versioned local-surface reference does not match its contract."""


def sha256_bytes(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def sha256_file(file_path: Path) -> str:
    digest = hashlib.sha256()
    with file_path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _oracle_source_sha256() -> str:
    source = Path(__file__).read_text(encoding="utf-8")
    source = re.sub(
        r'^(REFERENCE_MANIFEST_SHA256|ORACLE_SOURCE_SHA256) = ".*"$',
        r'\1 = "<pinned>"',
        source,
        flags=re.MULTILINE,
    )
    return sha256_bytes(source.encode("utf-8"))


def _read_json(file_path: Path, description: str) -> dict[str, Any]:
    try:
        value = json.loads(file_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ReferenceError(f"cannot read {description} at {file_path}: {error}") from error
    if not isinstance(value, dict):
        raise ReferenceError(f"{description} must contain a JSON object")
    return value


def _safe_file(root: Path, relative: str, description: str) -> Path:
    candidate = Path(relative)
    if candidate.is_absolute():
        raise ReferenceError(f"{description} path must be relative: {relative}")
    root_resolved = root.resolve()
    resolved = (root / candidate).resolve()
    if not resolved.is_relative_to(root_resolved) or not resolved.is_file():
        raise ReferenceError(f"{description} is missing or escapes its root: {relative}")
    return resolved


def _sample_points() -> list[tuple[int, int]]:
    points: list[tuple[int, int]] = []
    seen: set[tuple[int, int]] = set()

    def add(x: int, y: int) -> None:
        point = (x % WORLD_SIZE, y % WORLD_SIZE)
        if point not in seen:
            seen.add(point)
            points.append(point)

    origin_x, origin_y = 32_896, 32_768
    add(origin_x, origin_y)
    add(origin_x + 23, origin_y + 37)
    for dy, dx in itertools.product(range(-1, 18), repeat=2):
        add(origin_x + dx, origin_y + dy)
    for offset in range(1_024):
        add(origin_x + 23 + offset, origin_y + 37)
    for dy, dx in itertools.product((63, 64, 65), repeat=2):
        add(origin_x + dx, origin_y + dy)
    for y, x in itertools.product((WORLD_SIZE - 1, 0, 1), repeat=2):
        add(x, y)
    return points


def _npy_payload(raw: bytes, expected_name: str) -> bytes:
    if raw[:6] != b"\x93NUMPY" or raw[6:8] != b"\x01\x00":
        raise ReferenceError(f"unsupported NumPy array header for {expected_name}")
    header_length = struct.unpack_from("<H", raw, 8)[0]
    header_end = 10 + header_length
    try:
        header = ast.literal_eval(raw[10:header_end].decode("latin1").strip())
    except (SyntaxError, ValueError, UnicodeDecodeError) as error:
        raise ReferenceError(f"invalid NumPy array header for {expected_name}: {error}") from error
    if header.get("descr") != "<f4" or header.get("fortran_order") is not False \
            or header.get("shape") != (ATLAS_RESOLUTION, ATLAS_RESOLUTION):
        raise ReferenceError(f"unexpected NumPy field contract for {expected_name}: {header}")
    payload = raw[header_end:]
    if len(payload) != ATLAS_RESOLUTION * ATLAS_RESOLUTION * 4:
        raise ReferenceError(f"unexpected NumPy payload size for {expected_name}")
    return payload


def _as_float32(raw: bytes) -> tuple[float, ...]:
    if len(raw) % 4:
        raise ReferenceError("float32 field has a partial item")
    return tuple(item[0] for item in struct.iter_unpack("<f", raw))


def _wrap(value: float) -> float:
    wrapped = math.fmod(value, WORLD_SIZE)
    return wrapped + WORLD_SIZE if wrapped < 0.0 else wrapped


def _field(field: tuple[float, ...] | bytes, x: float, y: float) -> float:
    sx = _wrap(x) * ATLAS_RESOLUTION / WORLD_SIZE
    sy = _wrap(y) * ATLAS_RESOLUTION / WORLD_SIZE
    floor_x = math.floor(sx)
    floor_y = math.floor(sy)
    x0 = floor_x % ATLAS_RESOLUTION
    y0 = floor_y % ATLAS_RESOLUTION
    x1 = (x0 + 1) % ATLAS_RESOLUTION
    y1 = (y0 + 1) % ATLAS_RESOLUTION
    fx = sx - floor_x
    fy = sy - floor_y

    def at(ix: int, iy: int) -> float:
        value = field[iy * ATLAS_RESOLUTION + ix]
        return float(value)

    one_minus_x = 1.0 - fx
    one_minus_y = 1.0 - fy
    value_00 = (one_minus_x * one_minus_y) * at(x0, y0)
    value_10 = (fx * one_minus_y) * at(x1, y0)
    value_01 = (one_minus_x * fy) * at(x0, y1)
    value_11 = (fx * fy) * at(x1, y1)
    return ((value_00 + value_10) + value_01) + value_11


def _hash_noise(seed: int, ix: int, iy: int, level: int) -> int:
    period = WORLD_SIZE // level
    ix %= period
    iy %= period
    bits = (seed & HASH_MASK) ^ ((ix * HASH_X) & HASH_MASK) \
        ^ ((iy * HASH_Y) & HASH_MASK) ^ ((level * HASH_LEVEL) & HASH_MASK)
    bits ^= bits >> 30
    bits = (bits * HASH_MIX_1) & HASH_MASK
    bits ^= bits >> 27
    bits = (bits * HASH_MIX_2) & HASH_MASK
    bits ^= bits >> 31
    return bits


def _lattice_noise(seed: int, ix: int, iy: int, level: int) -> float:
    return (_hash_noise(seed, ix, iy, level) >> 11) * (2.0 / (1 << 53)) - 1.0


def _relaxed_node(
    fields: dict[str, tuple[float, ...] | bytes], seed: int, ix: int, iy: int, level: int
) -> float:
    x = float(ix * level)
    y = float(iy * level)
    cell = WORLD_SIZE / ATLAS_RESOLUTION
    dx = (_field(fields["atlas__elevation"], x + cell, y)
          - _field(fields["atlas__elevation"], x - cell, y)) / (2.0 * cell)
    dy = (_field(fields["atlas__elevation"], x, y + cell)
          - _field(fields["atlas__elevation"], x, y - cell)) / (2.0 * cell)
    rain = max(_field(fields["atlas__precipitation"], x, y), 0.0)
    diffusivity = 0.15 + 0.18 * math.tanh(rain)
    east = math.tanh(-dx * 2_000.0)
    south = math.tanh(-dy * 2_000.0)
    center = _lattice_noise(seed, ix, iy, level)
    east_node = _lattice_noise(seed, ix + 1, iy, level)
    west_node = _lattice_noise(seed, ix - 1, iy, level)
    south_node = _lattice_noise(seed, ix, iy + 1, level)
    north_node = _lattice_noise(seed, ix, iy - 1, level)
    weight_east = 1.0 + east
    weight_west = 1.0 - east
    weight_south = 1.0 + south
    weight_north = 1.0 - south
    neighbor = (
        weight_east * east_node
        + weight_west * west_node
        + weight_south * south_node
        + weight_north * north_node
    ) * 0.25
    return center + diffusivity * (neighbor - center)


def _detail_influence(
    fields: dict[str, tuple[float, ...] | bytes],
    x: float,
    y: float,
    base: float,
    temperature: float,
    precipitation: float,
) -> float:
    cell = WORLD_SIZE / ATLAS_RESOLUTION
    dx = (_field(fields["atlas__elevation"], x + cell, y)
          - _field(fields["atlas__elevation"], x - cell, y)) / (2.0 * cell)
    dy = (_field(fields["atlas__elevation"], x, y + cell)
          - _field(fields["atlas__elevation"], x, y - cell)) / (2.0 * cell)
    slope = math.hypot(dx, dy)
    rainfall = max(precipitation, 0.0)
    roughness = 0.35 + 0.65 * math.tanh(2_500.0 * slope)
    coldness = min(max((-temperature - 5.0) / 30.0, 0.0), 1.0)
    climate_factor = 1.0 - 0.18 * math.tanh(rainfall) + 0.08 * coldness
    lake = min(max(_field(fields["atlas__lake_mask"], x, y), 0.0), 1.0)
    river = min(max(_field(fields["atlas__river_mask"], x, y), 0.0), 1.0)
    corridor = (1.0 - lake) * (1.0 - river) * (1.0 - river)
    ocean_factor = 0.4 if base <= 0.0 else 1.0
    return roughness * climate_factor * corridor * ocean_factor


def _contribution(
    fields: dict[str, tuple[float, ...] | bytes],
    seed: int,
    x: float,
    y: float,
    base: float,
    temperature: float,
    precipitation: float,
    previous: float,
    level: int,
    amplitude: float,
    parent_level: int | None,
) -> float:
    ux = x / level
    uy = y / level
    ix = math.floor(ux)
    iy = math.floor(uy)
    fx = ux - ix
    fy = uy - iy
    fx = fx * fx * (3.0 - 2.0 * fx)
    fy = fy * fy * (3.0 - 2.0 * fy)
    a = _relaxed_node(fields, seed, ix, iy, level)
    b = _relaxed_node(fields, seed, ix + 1, iy, level)
    c = _relaxed_node(fields, seed, ix, iy + 1, level)
    d = _relaxed_node(fields, seed, ix + 1, iy + 1, level)
    node_value = (
        (1.0 - fx) * (1.0 - fy) * a
        + fx * (1.0 - fy) * b
        + (1.0 - fx) * fy * c
        + fx * fy * d
    )
    ancestry = 0.75 + 0.25 * math.tanh(abs(previous) / max(amplitude, 1e-12))
    if parent_level is not None:
        parent_x = math.fmod(x / parent_level, 1.0)
        parent_y = math.fmod(y / parent_level, 1.0)
        cosine_product = math.cos(math.pi * parent_x) * math.cos(math.pi * parent_y)
        ancestry *= 1.0 - cosine_product * cosine_product
    influence = _detail_influence(fields, x, y, base, temperature, precipitation)
    return amplitude * influence * ancestry * node_value


def _clamp_elevation(base: float, total: float) -> float:
    refined = base + total
    if base > 0.0:
        refined = max(refined, 1e-6)
    elif base < 0.0:
        refined = min(refined, -1e-6)
    else:
        refined = 0.0
    return refined


def _sample_surface(
    fields: dict[str, tuple[float, ...] | bytes],
    seed: int,
    scale: float,
    x: int,
    y: int,
) -> dict[str, float | int]:
    base = _field(fields["atlas__elevation"], x, y)
    temperature = _field(fields["atlas__temperature"], x, y)
    precipitation = _field(fields["atlas__precipitation"], x, y)
    lake = _field(fields["atlas__lake_mask"], x, y)
    river = _field(fields["atlas__river_mask"], x, y)
    total = 0.0
    previous = 0.0
    for index, (level, amplitude) in enumerate(zip(REGIONAL_EXTENTS, REGIONAL_AMPLITUDES_KM)):
        parent = REGIONAL_EXTENTS[index - 1] if index else None
        previous = _contribution(
            fields, seed, x, y, base, temperature, precipitation, previous,
            level, amplitude, parent,
        )
        total += previous
    elevation_v4 = _clamp_elevation(base, total)
    local_detail = _contribution(
        fields, seed, x, y, base, temperature, precipitation, previous,
        LOCAL_EXTENT, LOCAL_AMPLITUDE_KM, LOCAL_PARENT_EXTENT,
    )
    elevation_v5 = _clamp_elevation(base, total + local_detail)
    if lake >= 1.0 or river >= 1.0:
        elevation_v4 = base
        elevation_v5 = base

    def height(elevation: float) -> int:
        raw = BLOCK_SEA_LEVEL + scale * elevation
        return min(max(math.floor(raw), 1), MAX_HEIGHT)

    height_v4 = height(elevation_v4)
    height_v5 = height(elevation_v5)
    top_v4 = top_v5 = 3 if height_v4 < BLOCK_SEA_LEVEL else (
        5 if temperature <= -2.0 else (2 if precipitation > 0.20 else 3)
    )
    top_v5 = 3 if height_v5 < BLOCK_SEA_LEVEL else (
        5 if temperature <= -2.0 else (2 if precipitation > 0.20 else 3)
    )
    ground = 6 if temperature <= -2.0 else 3
    return {
        "height_v4": height_v4,
        "height_v5": height_v5,
        "top_material_v4": top_v4,
        "ground_material_v4": ground,
        "top_material_v5": top_v5,
        "ground_material_v5": ground,
        "elevation_v4": elevation_v4,
        "elevation_v5": elevation_v5,
        "local_detail": local_detail,
    }


def _expected_identity(seed: int) -> str:
    return (
        f"terrain-v2;model-revision=5;source-map-sha256={SOURCE_MAP_SHA256};seed={seed}"
        ";resolution=256;epochs=12;erosion-steps=3;initial-climate-iterations=24"
        ";climate-iterations=80;currents=1;wind-sign=1;coastal-elaboration=1"
        ";shelf-exp=exp64-to-f32-v1"
    )


def _read_full_fields(archive_path: Path, manifest: dict[str, Any], seed: int) \
        -> dict[str, tuple[float, ...] | bytes]:
    result: dict[str, tuple[float, ...] | bytes] = {}
    with zipfile.ZipFile(archive_path) as archive:
        for name in FULL_FIELD_NAMES:
            field_record = next(
                record for record in manifest["full_atlas_fields"]
                if record["seed"] == seed and record["name"] == name
            )
            raw = archive.read(field_record["file"])
            if sha256_bytes(raw) != field_record["sha256"]:
                raise ReferenceError(f"revision-4 atlas field digest mismatch for seed {seed} {name}")
            result[name] = _as_float32(raw) if field_record["dtype"] == "<f4" else raw
    return result


def _load_precipitation(npz_path: Path) -> tuple[bytes, str]:
    with zipfile.ZipFile(npz_path) as npz:
        raw = _npy_payload(npz.read("atlas__precipitation.npy"), "atlas__precipitation")
    return raw, sha256_file(npz_path)


def _write_field(root: Path, seed: int, name: str, raw: bytes, count: int) -> dict[str, Any]:
    dtype, coordinate_set, unit = FIELD_DESCRIPTORS[name]
    path = root / f"seed-{seed}" / f"{name}.raw"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(raw)
    shape = [count, 2] if name == "point_xy" else [count]
    if name == "input_precipitation":
        shape = [ATLAS_RESOLUTION * ATLAS_RESOLUTION]
    return {
        "seed": seed,
        "name": name,
        "dtype": dtype,
        "shape": shape,
        "order": "C",
        "coordinate_set": coordinate_set,
        "unit": unit,
        "file": path.relative_to(root).as_posix(),
        "bytes": len(raw),
        "sha256": sha256_bytes(raw),
    }


def record_reference(
    output_root: Path,
    v4_fixture_root: Path,
    v4_archive_path: Path,
    bundle_root: Path,
    precipitation_inputs_root: Path,
    native_library_path: Path,
) -> dict[str, Any]:
    if output_root.exists():
        raise ReferenceError(f"refusing to overwrite existing revision-5 fixture: {output_root}")
    v4_manifest = revision4.load_reference(v4_fixture_root)
    if sha256_file(v4_fixture_root / "manifest.json") != REVISION4_MANIFEST_SHA256:
        raise ReferenceError("revision-4 reference manifest identity differs")
    revision4.verify_full_archive(v4_manifest, v4_archive_path)
    if sha256_file(v4_archive_path) != REVISION4_ARCHIVE_SHA256:
        raise ReferenceError("approved revision-4 full-atlas archive identity differs")
    bundle_receipt = terrain_reference.verify_reference_bundle(
        REPOSITORY_ROOT / "tests" / "fixtures" / "terrain_v2",
        bundle_root,
    )
    if bundle_receipt["model_sha256"] != SOURCE_MAP_SHA256:
        raise ReferenceError("retained source bundle model identity differs")
    if not native_library_path.is_file():
        raise ReferenceError(f"native generation library is missing: {native_library_path}")

    output_root.parent.mkdir(parents=True, exist_ok=True)
    temporary_root = Path(tempfile.mkdtemp(prefix="terrain-v5-reference-", dir=output_root.parent))
    try:
        archive_hashes = [
            {
                "seed": field["seed"],
                "name": field["name"],
                "dtype": field["dtype"],
                "shape": field["shape"],
                "sha256": field["sha256"],
            }
            for field in v4_manifest["full_atlas_fields"]
        ]
        if len(archive_hashes) != len(SEEDS) * len(FULL_FIELD_NAMES):
            raise ReferenceError("approved revision-4 full-atlas field set is incomplete")
        approved_records = {
            (record["seed"], record["name"]): record
            for record in v4_manifest["full_atlas_fields"]
        }
        if len(approved_records) != len(archive_hashes) or any(
            (record["seed"], record["name"]) not in approved_records
            or any(record[key] != approved_records[(record["seed"], record["name"])][key]
                   for key in ("dtype", "shape", "sha256"))
            for record in archive_hashes
        ):
            raise ReferenceError("approved revision-4 archive field records changed")
        seed_records: list[dict[str, Any]] = []
        fields_manifest: list[dict[str, Any]] = []
        points = _sample_points()
        v3_reference = terrain_reference.validate_reference(
            REPOSITORY_ROOT / "tests" / "fixtures" / "terrain_v2"
        )
        for seed in SEEDS:
            source_relative = f"results/seed-matrix-v3/seed-{seed}-n256/world.npz"
            npz_path = bundle_root / source_relative
            python_precipitation_raw, npz_digest = _load_precipitation(npz_path)
            if sha256_file(npz_path) != npz_digest:
                raise ReferenceError(f"retained seed {seed} NumPy archive changed while reading")
            native_precipitation_path = precipitation_inputs_root / f"seed-{seed}.f32le"
            native_precipitation_raw = native_precipitation_path.read_bytes()
            if len(native_precipitation_raw) != ATLAS_RESOLUTION * ATLAS_RESOLUTION * 4:
                raise ReferenceError(f"native seed {seed} precipitation field has the wrong byte count")
            difference_offsets = [
                offset for offset in range(0, len(python_precipitation_raw), 4)
                if python_precipitation_raw[offset:offset + 4]
                != native_precipitation_raw[offset:offset + 4]
            ]
            precipitation = _as_float32(native_precipitation_raw)
            input_record = _write_field(
                temporary_root, seed, "input_precipitation", native_precipitation_raw,
                ATLAS_RESOLUTION * ATLAS_RESOLUTION,
            )
            input_record["order"] = "C row-major [y,x]"
            fields_manifest.append(input_record)

            scale_path = REPOSITORY_ROOT / "tests" / "fixtures" / "terrain_v2" \
                / f"seed-{seed}" / "atlas__vertical_scale.raw"
            scale_raw = scale_path.read_bytes()
            if len(scale_raw) != 8:
                raise ReferenceError(f"revision-4 vertical scale has the wrong size for seed {seed}")
            vertical_scale = struct.unpack("<d", scale_raw)[0]
            full_fields = _read_full_fields(v4_archive_path, v4_manifest, seed)
            full_fields["atlas__precipitation"] = precipitation

            outputs = [_sample_surface(full_fields, seed, vertical_scale, x, y) for x, y in points]
            raw_fields: dict[str, bytes] = {
                "point_xy": b"".join(struct.pack("<II", x, y) for x, y in points),
            }
            for name in FIELD_DESCRIPTORS:
                if name == "point_xy" or name == "input_precipitation":
                    continue
                dtype = FIELD_DESCRIPTORS[name][0]
                fmt = {"<i4": "<i", "|u1": "<B", "<f8": "<d"}[dtype]
                raw_fields[name] = b"".join(struct.pack(fmt, int(output[name]) if dtype in ("<i4", "|u1")
                                                         else float(output[name]))
                                               for output in outputs)
            for name, raw in raw_fields.items():
                fields_manifest.append(_write_field(temporary_root, seed, name, raw, len(points)))

            source_record = next(record for record in v3_reference["seeds"] if record["seed"] == seed)
            seed_records.append({
                "seed": seed,
                "configuration_identity": _expected_identity(seed),
                "python_bundle_npz": source_relative,
                "python_bundle_npz_sha256": npz_digest,
                "python_bundle_identity_sha256": source_record["identity_sha256"],
                "precipitation_member": "atlas__precipitation.npy",
                "python_precipitation_sha256": sha256_bytes(python_precipitation_raw),
                "native_precipitation_sha256": sha256_bytes(native_precipitation_raw),
                "python_native_differing_float32_count": len(difference_offsets),
                "first_python_native_difference": (
                    {
                        "element": difference_offsets[0] // 4,
                        "python_little_endian_hex": python_precipitation_raw[
                            difference_offsets[0]:difference_offsets[0] + 4
                        ].hex(),
                        "native_little_endian_hex": native_precipitation_raw[
                            difference_offsets[0]:difference_offsets[0] + 4
                        ].hex(),
                    }
                    if difference_offsets else None
                ),
                "vertical_scale_f64": vertical_scale,
                "vertical_scale_f64le_sha256": sha256_bytes(scale_raw),
                "point_count": len(points),
            })
            print(f"recorded revision-5 Python oracle points for seed {seed}: {len(points)}")

        manifest = {
            "schema": SCHEMA,
            "terrain_version": 2,
            "model_revision": 5,
            "configuration_identity_schema": "terrain-v2 canonical identity with model-revision=5",
            "configuration_identities": {str(seed): _expected_identity(seed) for seed in SEEDS},
            "source_map_sha256": SOURCE_MAP_SHA256,
            "revision4_manifest_sha256": REVISION4_MANIFEST_SHA256,
            "revision3_manifest_sha256": REVISION3_MANIFEST_SHA256,
            "revision4_full_atlas_archive": {
                "sha256": REVISION4_ARCHIVE_SHA256,
                "retained_path": str(v4_archive_path),
                "field_sha256_records": archive_hashes,
            },
            "native_precipitation_input": {
                "provider": "generation::prepareAtlas(...).precipitation from the revision-4-approved global atlas",
                "global_atlas_is_unchanged_by_revision5": True,
                "native_generation_library_sha256": sha256_file(native_library_path),
                "seed_inputs_verified_against_native_by": "tests/core/terrain_world_reference_tests.cpp",
                "retained_python_comparison": "The retained v3 Python precipitation field is compared in full; any differences are recorded per seed, and the native revision-4 atlas field is the oracle input.",
            },
            "oracle": {
                "source": "script/terrain_revision_5_reference.py",
                "source_sha256": ORACLE_SOURCE_SHA256,
                "algorithm": "five frozen regional levels followed by one 16-block final-surface octave",
                "regional_extents": list(REGIONAL_EXTENTS),
                "regional_amplitudes_km": list(REGIONAL_AMPLITUDES_KM),
                "local_extent": LOCAL_EXTENT,
                "local_amplitude_km": LOCAL_AMPLITUDE_KM,
                "local_parent_extent": LOCAL_PARENT_EXTENT,
                "input_interpolation": "revision-4 approved float32 fields plus precipitation verified against each native v4 atlas in C++ tests",
                "runtime": {
                    "python": platform.python_version(),
                    "implementation": platform.python_implementation(),
                    "platform": platform.platform(),
                    "math_library": "Python standard-library math on the recording host",
                },
                "scope": "The fixture asserts these six seeds and listed integer coordinates; it makes no universal libm claim.",
            },
            "seeds": seed_records,
            "sample_grid": {
                "coordinate_dtype": "<u4",
                "coordinate_order": "C row-major point list [x,y]",
                "sampling": "integer block positions; dense 19x19 neighborhood around 16-block chunk boundaries; 1,024-block route from offset (23,37); 64-block boundaries; periodic XY wrap",
                "points_per_seed": len(points),
                "seeds": list(SEEDS),
            },
            "fields": fields_manifest,
        }
        manifest_path = temporary_root / "manifest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        if output_root.exists():
            raise ReferenceError(f"refusing to overwrite existing revision-5 fixture: {output_root}")
        os.replace(temporary_root, output_root)
    except BaseException:
        if temporary_root.exists():
            import shutil
            shutil.rmtree(temporary_root)
        raise
    return manifest


def load_reference(fixture_root: Path) -> dict[str, Any]:
    manifest_path = fixture_root / "manifest.json"
    manifest_bytes = manifest_path.read_bytes()
    actual_digest = sha256_bytes(manifest_bytes)
    if actual_digest != REFERENCE_MANIFEST_SHA256:
        raise ReferenceError(
            f"revision-5 manifest SHA-256 mismatch: expected {REFERENCE_MANIFEST_SHA256}, got {actual_digest}"
        )
    manifest = _read_json(manifest_path, "revision-5 reference manifest")
    if manifest.get("schema") != SCHEMA or manifest.get("model_revision") != 5:
        raise ReferenceError("unsupported revision-5 local-surface reference schema")
    if manifest.get("source_map_sha256") != SOURCE_MAP_SHA256 \
            or manifest.get("revision4_manifest_sha256") != REVISION4_MANIFEST_SHA256 \
            or manifest.get("revision3_manifest_sha256") != REVISION3_MANIFEST_SHA256:
        raise ReferenceError("revision-5 reference source identity differs")
    if manifest.get("seeds") is None or [item.get("seed") for item in manifest["seeds"]] != list(SEEDS):
        raise ReferenceError("revision-5 seed order differs")
    if manifest.get("configuration_identities") != {
        str(seed): _expected_identity(seed) for seed in SEEDS
    }:
        raise ReferenceError("revision-5 configuration identity differs")
    expected_points = _sample_points()
    if manifest.get("sample_grid", {}).get("points_per_seed") != len(expected_points):
        raise ReferenceError("revision-5 sample coordinate count differs")
    oracle = manifest.get("oracle", {})
    if oracle.get("source_sha256") != ORACLE_SOURCE_SHA256 \
            or ORACLE_SOURCE_SHA256 != _oracle_source_sha256():
        raise ReferenceError("revision-5 oracle source digest differs")
    if oracle.get("regional_extents") != list(REGIONAL_EXTENTS) \
            or oracle.get("regional_amplitudes_km") != list(REGIONAL_AMPLITUDES_KM) \
            or oracle.get("local_extent") != LOCAL_EXTENT \
            or oracle.get("local_amplitude_km") != LOCAL_AMPLITUDE_KM \
            or oracle.get("local_parent_extent") != LOCAL_PARENT_EXTENT:
        raise ReferenceError("revision-5 surface detail constants differ")
    v4_manifest = fixture_root.parent / "terrain_v2_revision_4" / "manifest.json"
    if sha256_file(v4_manifest) != REVISION4_MANIFEST_SHA256:
        raise ReferenceError("preserved revision-4 manifest has changed")
    verified_v4 = revision4.load_reference(v4_manifest.parent)
    expected_full_fields = [
        {
            "seed": field["seed"],
            "name": field["name"],
            "dtype": field["dtype"],
            "shape": field["shape"],
            "sha256": field["sha256"],
        }
        for field in verified_v4["full_atlas_fields"]
    ]
    archive_record = manifest.get("revision4_full_atlas_archive", {})
    if archive_record.get("sha256") != REVISION4_ARCHIVE_SHA256 \
            or archive_record.get("field_sha256_records") != expected_full_fields:
        raise ReferenceError("revision-5 approved revision-4 atlas provenance differs")
    records = manifest.get("fields")
    if not isinstance(records, list) or len(records) != len(SEEDS) * len(FIELD_DESCRIPTORS):
        raise ReferenceError("revision-5 field record count differs")
    seen: set[tuple[int, str]] = set()
    counts: dict[int, int] = {}
    for record in records:
        seed, name = record.get("seed"), record.get("name")
        if seed not in SEEDS or name not in FIELD_DESCRIPTORS or (seed, name) in seen:
            raise ReferenceError(f"duplicate or unsupported revision-5 field: {(seed, name)}")
        seen.add((seed, name))
        dtype, coordinate_set, _ = FIELD_DESCRIPTORS[name]
        if record.get("dtype") != dtype:
            raise ReferenceError(f"revision-5 field dtype differs for {(seed, name)}")
        count = manifest["sample_grid"]["points_per_seed"]
        shape = [ATLAS_RESOLUTION * ATLAS_RESOLUTION] if name == "input_precipitation" \
            else ([count, 2] if name == "point_xy" else [count])
        expected_bytes = math.prod(shape) * ITEM_SIZES[dtype]
        if record.get("shape") != shape or record.get("bytes") != expected_bytes \
                or record.get("coordinate_set") != coordinate_set:
            raise ReferenceError(f"revision-5 field descriptor differs for {(seed, name)}")
        raw = _safe_file(fixture_root, record.get("file", ""), "revision-5 field").read_bytes()
        if len(raw) != expected_bytes or sha256_bytes(raw) != record.get("sha256"):
            raise ReferenceError(f"revision-5 field bytes differ for {(seed, name)}")
        counts[seed] = counts.get(seed, 0) + 1
    if any(counts.get(seed) != len(FIELD_DESCRIPTORS) for seed in SEEDS):
        raise ReferenceError("one or more revision-5 seeds have incomplete fields")
    for seed_record in manifest["seeds"]:
        seed = seed_record["seed"]
        if seed_record.get("configuration_identity") != _expected_identity(seed):
            raise ReferenceError(f"revision-5 seed {seed} configuration identity differs")
        input_record = next(
            record for record in records
            if record["seed"] == seed and record["name"] == "input_precipitation"
        )
        if input_record["sha256"] != seed_record.get("native_precipitation_sha256"):
            raise ReferenceError(f"revision-5 seed {seed} precipitation input binding differs")
        scale_path = fixture_root.parent / "terrain_v2" \
            / f"seed-{seed}" / "atlas__vertical_scale.raw"
        if sha256_file(scale_path) != seed_record.get("vertical_scale_f64le_sha256"):
            raise ReferenceError(f"revision-5 seed {seed} vertical-scale provenance differs")
    return manifest


def verify_input_bundle(manifest: dict[str, Any], fixture_root: Path, bundle_root: Path) -> int:
    reference = terrain_reference.validate_reference(REPOSITORY_ROOT / "tests" / "fixtures" / "terrain_v2")
    terrain_reference.verify_reference_bundle(
        REPOSITORY_ROOT / "tests" / "fixtures" / "terrain_v2",
        bundle_root,
    )
    checked = 0
    for seed_record in manifest["seeds"]:
        npz_path = _safe_file(bundle_root, seed_record["python_bundle_npz"], "retained source NumPy archive")
        if sha256_file(npz_path) != seed_record["python_bundle_npz_sha256"]:
            raise ReferenceError(f"retained seed {seed_record['seed']} NumPy archive digest differs")
        raw, _ = _load_precipitation(npz_path)
        if sha256_bytes(raw) != seed_record["python_precipitation_sha256"]:
            raise ReferenceError(f"retained seed {seed_record['seed']} Python precipitation digest differs")
        fixture_path = fixture_root / f"seed-{seed_record['seed']}" / "input_precipitation.raw"
        native = fixture_path.read_bytes()
        if sha256_bytes(native) != seed_record["native_precipitation_sha256"]:
            raise ReferenceError(f"retained seed {seed_record['seed']} native precipitation digest differs")
        difference_count = sum(
            raw[offset:offset + 4] != native[offset:offset + 4]
            for offset in range(0, len(raw), 4)
        )
        if difference_count != seed_record["python_native_differing_float32_count"]:
            raise ReferenceError(f"retained seed {seed_record['seed']} precipitation comparison differs")
        checked += 1
    if reference["model"]["model_sha256"] != SOURCE_MAP_SHA256:
        raise ReferenceError("retained reference bundle source map differs")
    return checked


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    verify_parser = subparsers.add_parser("verify", help="verify checked-in revision-5 sample fields")
    verify_parser.add_argument(
        "--fixture",
        type=Path,
        default=REPOSITORY_ROOT / "tests" / "fixtures" / "terrain_v2_revision_5",
    )
    verify_parser.add_argument("--bundle", type=Path)
    record_parser = subparsers.add_parser("record", help="record a new revision-5 oracle fixture without overwrite")
    record_parser.add_argument("--output", type=Path, required=True)
    record_parser.add_argument(
        "--v4-fixture",
        type=Path,
        default=REPOSITORY_ROOT / "tests" / "fixtures" / "terrain_v2_revision_4",
    )
    record_parser.add_argument("--v4-archive", type=Path)
    record_parser.add_argument(
        "--bundle",
        type=Path,
        default=Path("/Users/core-just/.codex/reports/minecraft-worldgen-2026-10-19/bundle"),
    )
    record_parser.add_argument("--precipitation-inputs", type=Path, required=True)
    record_parser.add_argument(
        "--native-library",
        type=Path,
        default=REPOSITORY_ROOT / "build" / "generation-standalone" / "libmc_generation.a",
    )
    args = parser.parse_args()
    try:
        if args.command == "verify":
            manifest = load_reference(args.fixture)
            checked = verify_input_bundle(manifest, args.fixture, args.bundle) if args.bundle else 0
            print(
                "verified revision-5 local-surface reference: "
                f"manifest={REFERENCE_MANIFEST_SHA256}, fields={len(manifest['fields'])}, "
                f"precipitation_inputs={checked}"
            )
            return 0
        archive = args.v4_archive or Path(
            revision4.load_reference(args.v4_fixture)["approved_candidate_full_atlas_archive"]["retained_path"]
        )
        record_reference(
            args.output,
            args.v4_fixture,
            archive,
            args.bundle,
            args.precipitation_inputs,
            args.native_library,
        )
        print(f"recorded new revision-5 fixture at {args.output}")
        return 0
    except (OSError, ReferenceError, KeyError, TypeError, zipfile.BadZipFile) as error:
        print(f"revision-5 reference error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
