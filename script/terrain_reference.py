"""Read-only verifier and byte comparator for frozen Terrain-v2 references."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
from typing import Any


REFERENCE_SCHEMA = "minecraftclone.terrain-reference.v1"
CANDIDATE_SCHEMA = "minecraftclone.terrain-candidate.v1"
TERRAIN_VERSION = "Terrain-v2"
SEEDS = (0, 1, 42, 777, 2004, 8800555)
FROZEN_MODEL_SHA256 = "5de753a4068c71d742f5ce7e75e4e8bcabbaad5fddaf6c8404dc41ba00d45704"
PALETTE_SOURCE = {
    "file": "prototype/render_maps.py",
    "sha256": "19c596dcc7b9cce5b91d7307fadc246e89e64e18a2b19729d87a3360e4df9a65",
}
PALETTE = [
    {"id": 0, "name": "Air", "rgba": [189, 217, 237, 255], "hex": "#bdd9ed"},
    {"id": 1, "name": "Stone", "rgba": [128, 128, 128, 255], "hex": "#808080"},
    {"id": 2, "name": "Grass", "rgba": [92, 147, 69, 255], "hex": "#5c9345"},
    {"id": 3, "name": "Dirt", "rgba": [136, 103, 72, 255], "hex": "#886748"},
    {"id": 4, "name": "Bedrock", "rgba": [42, 40, 49, 255], "hex": "#2a2831"},
    {"id": 5, "name": "Snow", "rgba": [244, 247, 250, 255], "hex": "#f4f7fa"},
    {"id": 6, "name": "Ice", "rgba": [128, 201, 214, 255], "hex": "#80c9d6"},
]
# Updated only when a reviewed, explicitly versioned reference contract replaces this one.
FROZEN_REFERENCE_MANIFEST_SHA256 = "d0e6ade7aed51a34a864c1dc497d3b3921d10204a8cc3602b71f7d53ab96db97"
WORLD = {
    "size": 65536,
    "depth": 1024,
    "atlas_resolution": 256,
    "block_sea_level": 384,
    "max_height": 1020,
    "sea_level_km": 0.0,
}
ATLAS_GRID_INDICES = tuple(range(0, 256, 16))
SURFACE_SAMPLE_BLOCKS = tuple(128 + 4096 * index for index in range(16))

_FLOAT_ATLAS_FIELDS = (
    "stage__geology_elevation",
    "stage__geology_uplift",
    "stage__geology_subduction",
    "stage__geology_divergence",
    "stage__geology_continental",
    "stage__coastal_elevation",
    "stage__initial_drainage_elevation",
    "stage__initial_drainage_filled",
    "stage__initial_drainage_water_level",
    "stage__initial_drainage_flow_accumulation",
    "stage__provisional_climate_temperature",
    "stage__provisional_climate_precipitation",
    "stage__provisional_climate_moisture",
    "stage__terrain_erosion_elevation",
    "stage__terrain_erosion_filled",
    "stage__terrain_erosion_water_level",
    "stage__terrain_erosion_flow_accumulation",
    "stage__final_climate_temperature",
    "stage__final_climate_precipitation",
    "stage__final_climate_moisture",
    "atlas__elevation",
    "atlas__filled",
    "atlas__water_level",
    "atlas__flow_accumulation",
    "atlas__temperature",
    "atlas__precipitation",
    "atlas__moisture",
    "atlas__wind_u",
    "atlas__wind_v",
    "atlas__current_u",
    "atlas__current_v",
    "atlas__uplift",
    "atlas__island_arc_uplift",
    "atlas__hotspot",
    "atlas__subduction",
    "atlas__divergence",
    "atlas__continental",
    "atlas__tectonic_elevation",
    "atlas__plate_vx",
    "atlas__plate_vy",
)
_INTEGER_ATLAS_FIELDS = (
    "stage__initial_drainage_downstream",
    "stage__terrain_erosion_downstream",
    "atlas__downstream",
    "atlas__lake_id",
    "atlas__outlet_id",
)
_INTEGER16_ATLAS_FIELDS = (
    "stage__geology_plate_id",
    "atlas__plate_id",
)
_MASK_ATLAS_FIELDS = (
    "stage__initial_drainage_ocean_mask",
    "stage__initial_drainage_lake_mask",
    "stage__initial_drainage_river_mask",
    "stage__terrain_erosion_ocean_mask",
    "stage__terrain_erosion_lake_mask",
    "stage__terrain_erosion_river_mask",
    "atlas__ocean_mask",
    "atlas__lake_mask",
    "atlas__river_mask",
    "atlas__fjord_candidate_mask",
    "atlas__harbour_candidate_mask",
    "atlas__snow_mask",
)
_CATEGORY_ATLAS_FIELDS = ("atlas__climate_zone",)
_SURFACE_FIELDS = ("height", "top_material", "ground_material", "pixel_rgba")
REQUIRED_FIELDS = (
    *_FLOAT_ATLAS_FIELDS,
    *_INTEGER_ATLAS_FIELDS,
    *_INTEGER16_ATLAS_FIELDS,
    *_MASK_ATLAS_FIELDS,
    *_CATEGORY_ATLAS_FIELDS,
    *_SURFACE_FIELDS,
    "atlas__vertical_scale",
)


class ReferenceError(ValueError):
    """The reference or candidate does not match the frozen contract."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _json_sha256(value: Any) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode("utf-8")).hexdigest()


def _read_json(path: Path, description: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ReferenceError(f"cannot read {description} at {path}: {error}") from error
    if not isinstance(value, dict):
        raise ReferenceError(f"{description} must contain a JSON object")
    return value


def _safe_file(root: Path, relative: str, description: str) -> Path:
    candidate = Path(relative)
    if candidate.is_absolute():
        raise ReferenceError(f"{description} path must be relative: {relative}")
    root_resolved = root.resolve()
    path = (root / candidate).resolve()
    if not path.is_relative_to(root_resolved):
        raise ReferenceError(f"{description} path escapes its directory: {relative}")
    if not path.is_file():
        raise ReferenceError(f"{description} file is missing: {relative}")
    return path


def _item_size(dtype: str) -> int:
    sizes = {"<f8": 8, "<f4": 4, "<i4": 4, "<i8": 8, "<u8": 8, "<i2": 2, "|u1": 1, "|b1": 1}
    if dtype not in sizes:
        raise ReferenceError(f"unsupported canonical reference dtype: {dtype}")
    return sizes[dtype]


def _field_contract(name: str) -> tuple[str, tuple[int, ...], str, str]:
    if name in _FLOAT_ATLAS_FIELDS:
        if name.endswith("_temperature"):
            unit = "degrees Celsius"
        elif name.endswith("_precipitation"):
            unit = "integrated normalized precipitation flux per model run; not mm/year"
        elif name.endswith("_moisture"):
            unit = "normalized atmospheric moisture state"
        elif name.endswith("_flow_accumulation"):
            unit = "accumulated normalized runoff flux"
        elif name in ("atlas__wind_u", "atlas__wind_v", "atlas__current_u", "atlas__current_v"):
            unit = "atlas cells per model iteration"
        elif name.endswith("_continental"):
            unit = "continentality fraction"
        elif name in ("atlas__plate_vx", "atlas__plate_vy"):
            unit = "world units per tectonic epoch"
        else:
            unit = "geological kilometres"
        return "<f4", (16, 16), "atlas_grid_yx_stride16", unit
    if name in _INTEGER_ATLAS_FIELDS:
        if name.endswith("_downstream"):
            unit = "flattened C-order [y,x] receiver index; -1 means no downstream receiver"
        elif name == "atlas__lake_id":
            unit = "1-based periodic lake component ID; 0 means no lake"
        elif name == "atlas__outlet_id":
            unit = "flattened C-order [y,x] terminal root cell index"
        else:
            unit = "integer field value"
        return "<i4", (16, 16), "atlas_grid_yx_stride16", unit
    if name in _INTEGER16_ATLAS_FIELDS:
        return "<i2", (16, 16), "atlas_grid_yx_stride16", "plate index"
    if name in _MASK_ATLAS_FIELDS:
        return "|u1", (16, 16), "atlas_grid_yx_stride16", "boolean as 0 or 1"
    if name in _CATEGORY_ATLAS_FIELDS:
        return ("|u1", (16, 16), "atlas_grid_yx_stride16",
                "climate-zone code: 0 water, 1 warm arid, 2 warm humid, 3 temperate, 4 cold, 5 snow")
    if name == "height":
        return "<i4", (16, 16), "surface_blocks_yx_inset128_stride4096", "highest solid block Z"
    if name in ("top_material", "ground_material"):
        return "|u1", (16, 16), "surface_blocks_yx_inset128_stride4096", "Terrain-v2 material palette ID"
    if name == "pixel_rgba":
        return "|u1", (16, 16, 4), "surface_blocks_yx_inset128_stride4096", "top material RGBA8"
    if name == "atlas__vertical_scale":
        return "<f8", (), "atlas_scalar", "blocks per geology-km"
    raise ReferenceError(f"unknown required reference field: {name}")


def _expected_config(seed: int) -> dict[str, Any]:
    return {
        "seed": seed,
        "size": 256,
        "epochs": 12,
        "erosion_steps": 3,
        "initial_climate_iterations": 24,
        "climate_iterations": 80,
        "currents": True,
        "wind_sign": 1,
        "coastal_elaboration": True,
    }


def _validate_model_identity(model: dict[str, Any]) -> None:
    sources = model.get("sources")
    if not isinstance(sources, dict) or not sources:
        raise ReferenceError("reference model source hashes are missing")
    if model.get("model_sha256") != _json_sha256(sources):
        raise ReferenceError("reference model source identity hash mismatch")
    for name, digest in sources.items():
        if not isinstance(name, str) or not isinstance(digest, str) or len(digest) != 64:
            raise ReferenceError("reference model contains an invalid source hash")


def validate_reference(reference_root: Path) -> dict[str, Any]:
    """Validate every checked-in raw fixture and return its manifest."""
    reference_root = Path(reference_root)
    manifest_path = reference_root / "manifest.json"
    if sha256_file(manifest_path) != FROZEN_REFERENCE_MANIFEST_SHA256:
        raise ReferenceError("frozen reference manifest identity drift")
    manifest = _read_json(manifest_path, "reference manifest")
    if manifest.get("schema") != REFERENCE_SCHEMA:
        raise ReferenceError(f"unsupported reference schema: {manifest.get('schema')!r}")
    if manifest.get("terrain_version") != TERRAIN_VERSION:
        raise ReferenceError("reference terrain version drift")
    if manifest.get("world") != WORLD:
        raise ReferenceError("reference world configuration drift")
    if manifest.get("palette") != PALETTE or manifest.get("palette_source") != PALETTE_SOURCE:
        raise ReferenceError("reference material palette identity drift")
    if manifest.get("bundle", {}).get("files", {}).get(PALETTE_SOURCE["file"]) != PALETTE_SOURCE["sha256"]:
        raise ReferenceError("reference palette source hash is missing or changed")
    if manifest.get("model", {}).get("python") != "3.12.14":
        raise ReferenceError("reference Python version identity drift")
    if manifest.get("model", {}).get("packages", {}).get("numpy") != "2.5.4":
        raise ReferenceError("reference NumPy version identity drift")
    if manifest.get("model", {}).get("packages", {}).get("scipy") != "1.18.1":
        raise ReferenceError("reference SciPy version identity drift")
    _validate_model_identity(manifest["model"])
    if manifest["model"].get("model_sha256") != FROZEN_MODEL_SHA256:
        raise ReferenceError("reference model source-map identity drift")
    bundle_files = manifest.get("bundle", {}).get("files")
    if not isinstance(bundle_files, dict):
        raise ReferenceError("reference retained-bundle file hashes are missing")
    for source_name, source_hash in manifest["model"]["sources"].items():
        relative = f"results/seed-matrix-v3/source/{source_name}"
        if bundle_files.get(relative) != source_hash:
            raise ReferenceError(f"retained source snapshot identity drift: {source_name}")
    if "results/seed-matrix-v3/frozen-model.json" not in bundle_files or \
            "results/qualification-v3.json" not in bundle_files:
        raise ReferenceError("retained model or qualification hash is missing")

    sample_grid = manifest.get("samples")
    if not isinstance(sample_grid, dict):
        raise ReferenceError("reference sample coordinates are missing")
    if sample_grid.get("atlas_indices_x") != list(ATLAS_GRID_INDICES):
        raise ReferenceError("reference atlas X coordinates drifted")
    if sample_grid.get("atlas_indices_y") != list(ATLAS_GRID_INDICES):
        raise ReferenceError("reference atlas Y coordinates drifted")
    if sample_grid.get("surface_blocks_x") != list(SURFACE_SAMPLE_BLOCKS):
        raise ReferenceError("reference surface X coordinates drifted")
    if sample_grid.get("surface_blocks_y") != list(SURFACE_SAMPLE_BLOCKS):
        raise ReferenceError("reference surface Y coordinates drifted")
    if sample_grid.get("order") != "C row-major [y,x]":
        raise ReferenceError("reference array order drift")

    seeds = manifest.get("seeds")
    if not isinstance(seeds, list) or [item.get("seed") for item in seeds] != list(SEEDS):
        raise ReferenceError("reference seed order must be 0, 1, 42, 777, 2004, 8800555")
    expected_fields = list(REQUIRED_FIELDS)
    for seed_record in seeds:
        seed = seed_record["seed"]
        config = _expected_config(seed)
        if seed_record.get("config") != config or seed_record.get("config_sha256") != _json_sha256(config):
            raise ReferenceError(f"seed {seed} reference configuration drift")
        for key in ("identity_sha256", "world_npz_sha256"):
            digest = seed_record.get(key)
            if not isinstance(digest, str) or len(digest) != 64:
                raise ReferenceError(f"seed {seed} is missing {key}")
        bundle_seed_root = f"results/seed-matrix-v3/seed-{seed}-n256"
        if bundle_files.get(f"{bundle_seed_root}/identity.json") != seed_record["identity_sha256"] or \
                bundle_files.get(f"{bundle_seed_root}/world.npz") != seed_record["world_npz_sha256"]:
            raise ReferenceError(f"seed {seed} retained archive hashes are missing or inconsistent")
        fields = seed_record.get("fields")
        if not isinstance(fields, list) or [field.get("name") for field in fields] != expected_fields:
            raise ReferenceError(f"seed {seed} reference field set or order drift")
        for field in fields:
            dtype, shape, coordinate_set, unit = _field_contract(field["name"])
            expected = {
                "dtype": dtype,
                "shape": list(shape),
                "order": "C",
                "coordinate_set": coordinate_set,
                "unit": unit,
            }
            if any(field.get(key) != value for key, value in expected.items()):
                raise ReferenceError(f"seed {seed} field {field['name']} descriptor drift")
            path = _safe_file(reference_root, field.get("file", ""), f"seed {seed} reference field")
            raw = path.read_bytes()
            expected_bytes = math.prod(shape) * _item_size(dtype)
            if len(raw) != expected_bytes:
                raise ReferenceError(f"seed {seed} field {field['name']} has {len(raw)} bytes, expected {expected_bytes}")
            if sha256_file(path) != field.get("sha256"):
                raise ReferenceError(f"seed {seed} fixture hash mismatch for {field['name']}")

    rng_record = manifest.get("rng_oracle")
    if not isinstance(rng_record, dict):
        raise ReferenceError("PCG64 RNG oracle is missing")
    rng_path = _safe_file(reference_root, rng_record.get("file", ""), "PCG64 RNG oracle")
    if sha256_file(rng_path) != rng_record.get("sha256"):
        raise ReferenceError("PCG64 RNG oracle hash mismatch")
    rng_oracle = _read_json(rng_path, "PCG64 RNG oracle")
    if rng_oracle.get("schema") != "minecraftclone.terrain-v2-rng-oracle.v1" or rng_oracle.get("seed") != 42:
        raise ReferenceError("PCG64 RNG oracle identity drift")
    if manifest.get("rng_oracle", {}).get("calls") != [call["name"] for call in rng_oracle.get("calls", [])]:
        raise ReferenceError("PCG64 RNG oracle call sequence drift")
    return manifest


def _descriptor_identity(field: dict[str, Any]) -> tuple[Any, ...]:
    return tuple(field.get(key) for key in ("name", "dtype", "shape", "order", "coordinate_set", "unit"))


def _decode_value(raw: bytes, dtype: str) -> int | float | bool:
    if dtype == "<f8":
        return struct.unpack("<d", raw)[0]
    if dtype == "<f4":
        return struct.unpack("<f", raw)[0]
    if dtype == "<i4":
        return struct.unpack("<i", raw)[0]
    if dtype == "<i8":
        return struct.unpack("<q", raw)[0]
    if dtype == "<u8":
        return struct.unpack("<Q", raw)[0]
    if dtype == "<i2":
        return struct.unpack("<h", raw)[0]
    if dtype == "|b1":
        return bool(raw[0])
    return raw[0]


def _index_for(flat_index: int, shape: tuple[int, ...]) -> list[int]:
    indices = [0] * len(shape)
    remainder = flat_index
    for axis in range(len(shape) - 1, -1, -1):
        indices[axis] = remainder % shape[axis]
        remainder //= shape[axis]
    return indices


def _field_mismatches(seed: int, expected_field: dict[str, Any], actual_field: dict[str, Any],
                      expected_bytes: bytes, actual_bytes: bytes) -> dict[str, Any] | None:
    name = expected_field["name"]
    dtype = expected_field["dtype"]
    shape = tuple(expected_field["shape"])
    item_size = _item_size(dtype)
    items: list[dict[str, Any]] = []
    if name == "pixel_rgba":
        for pixel_index in range(shape[0] * shape[1]):
            offset = pixel_index * 4
            expected_pixel = expected_bytes[offset:offset + 4]
            actual_pixel = actual_bytes[offset:offset + 4]
            if expected_pixel != actual_pixel:
                row, column = divmod(pixel_index, shape[1])
                items.append({
                    "pixel": [row, column],
                    "channels": [index for index, pair in enumerate(zip(expected_pixel, actual_pixel))
                                 if pair[0] != pair[1]],
                    "expected_rgba_hex": expected_pixel.hex(),
                    "actual_rgba_hex": actual_pixel.hex(),
                })
    else:
        for flat_index in range(len(expected_bytes) // item_size):
            offset = flat_index * item_size
            expected_item = expected_bytes[offset:offset + item_size]
            actual_item = actual_bytes[offset:offset + item_size]
            if expected_item != actual_item:
                items.append({
                    "index": _index_for(flat_index, shape),
                    "expected_hex": expected_item.hex(),
                    "actual_hex": actual_item.hex(),
                    "expected_value": _decode_value(expected_item, dtype),
                    "actual_value": _decode_value(actual_item, dtype),
                })
    if not items:
        return None
    return {"seed": seed, "field": name, "mismatch_count": len(items), "items": items}


def compare_candidate(candidate_root: Path, reference_root: Path) -> dict[str, Any]:
    """Compare candidate raw files with frozen references without writing either."""
    candidate_root = Path(candidate_root)
    reference_root = Path(reference_root)
    reference = validate_reference(reference_root)
    reference_manifest_sha256 = sha256_file(reference_root / "manifest.json")
    candidate = _read_json(candidate_root / "candidate.json", "candidate manifest")
    if candidate.get("schema") != CANDIDATE_SCHEMA:
        raise ReferenceError(f"unsupported candidate schema: {candidate.get('schema')!r}")
    if candidate.get("terrain_version") != TERRAIN_VERSION:
        raise ReferenceError("candidate terrain version drift")
    if candidate.get("reference_manifest_sha256") != reference_manifest_sha256:
        raise ReferenceError("candidate reference manifest identity mismatch")
    if candidate.get("reference_model_sha256") != reference["model"]["model_sha256"] or \
            candidate.get("reference_sources") != reference["model"]["sources"]:
        raise ReferenceError("candidate source identity does not match the frozen reference")
    if candidate.get("world") != reference["world"]:
        raise ReferenceError("candidate world configuration drift")

    candidate_seeds = candidate.get("seeds")
    if not isinstance(candidate_seeds, list) or [item.get("seed") for item in candidate_seeds] != list(SEEDS):
        raise ReferenceError("candidate seed order must be 0, 1, 42, 777, 2004, 8800555")

    mismatches: list[dict[str, Any]] = []
    compared_field_count = 0
    for reference_seed, candidate_seed in zip(reference["seeds"], candidate_seeds):
        seed = reference_seed["seed"]
        if candidate_seed.get("config") != reference_seed["config"] or \
                candidate_seed.get("config_sha256") != reference_seed["config_sha256"]:
            raise ReferenceError(f"seed {seed} candidate configuration drift")
        reference_fields = reference_seed["fields"]
        candidate_fields = candidate_seed.get("fields")
        if not isinstance(candidate_fields, list) or \
                [field.get("name") for field in candidate_fields] != list(REQUIRED_FIELDS):
            raise ReferenceError(f"seed {seed} candidate field set or order drift")
        for reference_field, candidate_field in zip(reference_fields, candidate_fields):
            if _descriptor_identity(reference_field) != _descriptor_identity(candidate_field):
                raise ReferenceError(f"seed {seed} field {reference_field['name']} descriptor drift")
            candidate_file = _safe_file(candidate_root, candidate_field.get("file", ""),
                                        f"seed {seed} candidate field")
            actual_bytes = candidate_file.read_bytes()
            if len(actual_bytes) != math.prod(reference_field["shape"]) * _item_size(reference_field["dtype"]):
                raise ReferenceError(f"seed {seed} field {reference_field['name']} has an invalid byte count")
            if sha256_file(candidate_file) != candidate_field.get("sha256"):
                raise ReferenceError(f"seed {seed} candidate hash mismatch for {reference_field['name']}")
            reference_file = _safe_file(reference_root, reference_field["file"], "reference field")
            expected_bytes = reference_file.read_bytes()
            compared_field_count += 1
            mismatch = _field_mismatches(seed, reference_field, candidate_field,
                                         expected_bytes, actual_bytes)
            if mismatch:
                mismatches.append(mismatch)
    return {
        "passed": not mismatches,
        "terrain_version": TERRAIN_VERSION,
        "reference_manifest_sha256": reference_manifest_sha256,
        "reference_model_sha256": reference["model"]["model_sha256"],
        "compared_seed_order": list(SEEDS),
        "compared_field_count": compared_field_count,
        "mismatch_field_count": len(mismatches),
        "mismatches": mismatches,
    }


def verify_reference_bundle(reference_root: Path, bundle_root: Path | None = None) -> dict[str, Any]:
    """Verify that the retained external prototype bundle still matches its hashes."""
    reference_root = Path(reference_root)
    reference = validate_reference(reference_root)
    bundle = reference["bundle"]
    if bundle_root is None:
        bundle_root = Path(bundle["root"])
    else:
        bundle_root = Path(bundle_root)
    if not bundle_root.is_dir():
        raise ReferenceError(f"retained reference bundle is missing: {bundle_root}")

    for relative, digest in bundle["files"].items():
        path = _safe_file(bundle_root, relative, "retained bundle")
        if sha256_file(path) != digest:
            raise ReferenceError(f"retained reference bundle hash mismatch: {relative}")

    frozen_model = _read_json(bundle_root / "results/seed-matrix-v3/frozen-model.json", "frozen model identity")
    if frozen_model.get("model_sha256") != reference["model"]["model_sha256"] or \
            frozen_model.get("sources") != reference["model"]["sources"]:
        raise ReferenceError("retained prototype source identity drift")
    if frozen_model.get("python") != reference["model"]["python"] or \
            frozen_model.get("packages") != reference["model"]["packages"]:
        raise ReferenceError("retained prototype runtime identity drift")

    qualification = _read_json(bundle_root / "results/qualification-v3.json", "qualification identity")
    if qualification.get("status") != "passed" or \
            qualification.get("model_sha256") != reference["model"]["model_sha256"] or \
            qualification.get("mandatory_order") != list(SEEDS):
        raise ReferenceError("retained qualification identity drift")

    for seed_record in reference["seeds"]:
        seed = seed_record["seed"]
        output = bundle_root / f"results/seed-matrix-v3/seed-{seed}-n256"
        identity_path = output / "identity.json"
        arrays_path = output / "world.npz"
        if sha256_file(identity_path) != seed_record["identity_sha256"]:
            raise ReferenceError(f"seed {seed} retained identity hash mismatch")
        if sha256_file(arrays_path) != seed_record["world_npz_sha256"]:
            raise ReferenceError(f"seed {seed} retained raw array hash mismatch")
        identity = _read_json(identity_path, f"seed {seed} identity")
        if identity.get("config") != seed_record["config"] or \
                identity.get("config_sha256") != seed_record["config_sha256"] or \
                identity.get("model_sha256") != reference["model"]["model_sha256"]:
            raise ReferenceError(f"seed {seed} retained source/config identity drift")
    return {
        "passed": True,
        "bundle_root": str(bundle_root),
        "checked_bundle_file_count": len(bundle["files"]),
        "checked_seed_count": len(SEEDS),
        "model_sha256": reference["model"]["model_sha256"],
    }


def _default_reference_root() -> Path:
    return Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "terrain_v2"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    compare_parser = subparsers.add_parser("compare", help="compare a candidate raw export to frozen references")
    compare_parser.add_argument("--candidate", type=Path, required=True,
                                help="directory containing candidate.json and raw fields")
    compare_parser.add_argument("--reference", type=Path, default=_default_reference_root())
    compare_parser.add_argument("--bundle", type=Path,
                                help="also verify the retained external bundle before comparing")
    verify_parser = subparsers.add_parser("verify-bundle", help="verify retained source/config/raw bundle hashes")
    verify_parser.add_argument("--reference", type=Path, default=_default_reference_root())
    verify_parser.add_argument("--bundle", type=Path, help="retained bundle root")
    arguments = parser.parse_args(argv)
    try:
        if arguments.command == "verify-bundle":
            result = verify_reference_bundle(arguments.reference, arguments.bundle)
        else:
            if arguments.bundle is not None:
                verify_reference_bundle(arguments.reference, arguments.bundle)
            result = compare_candidate(arguments.candidate, arguments.reference)
    except ReferenceError as error:
        print(json.dumps({"passed": False, "error": str(error)}, indent=2))
        return 2
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result.get("passed") else 1


if __name__ == "__main__":
    sys.exit(main())
