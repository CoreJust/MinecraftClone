"""Record and verify the revision-6 exposed-land material contract."""
from __future__ import annotations

import argparse
import hashlib
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

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from script import terrain_revision_4_reference as revision4
from script import terrain_revision_5_reference as revision5

SCHEMA = "minecraftclone.terrain-v2-model-revision-6-surface-material-reference.v1"
REFERENCE_MANIFEST_SHA256 = "a4bb4d4a807bcb88be751705a9af743266876536a08f19067038f07876b0dd12"
ORACLE_SOURCE_SHA256 = "f355a11dd4e0a4c05d47e08fb805705997c5e1a8479ee7baf6156a52166e8057"
REVISION4_MANIFEST_SHA256 = "9a2644bbe81a88876bac4f84f334f8c0a5d3525295e83122dcce10f92f02d6eb"
REVISION4_ARCHIVE_SHA256 = "690393f062e5e2d78c3bfc495066d8150165068b486731d46354d56427d55a6d"
REVISION5_MANIFEST_SHA256 = "593da126bfebecc56f2412424aba50a160105a3471d2b5c58f2c1146108294f2"
REVISION5_SOURCE_SHA256 = "2f0fb6b653c71dd45fdd734145b9f9ad43e06722f13fdb1c97464a100f53d9cb"
SOURCE_MAP_SHA256 = revision5.SOURCE_MAP_SHA256
SEEDS = revision5.SEEDS
BLOCK_SEA_LEVEL = revision5.BLOCK_SEA_LEVEL
DIRT = 3
GRASS = 2
SNOW = 5

OWNER_RAY_POINTS = (
    ("image1", 0, 14942, 44726),
    ("image1", 8, 14937, 44731),
    ("image1", 16, 14931, 44736),
    ("image1", 32, 14920, 44745),
    ("image1", 64, 14898, 44765),
    ("image1", 128, 14853, 44804),
    ("image1", 256, 14764, 44883),
    ("image1", 512, 14585, 45040),
    ("image1", 1024, 14227, 45354),
    ("images2-3", 0, 11993, 43207),
    ("images2-3", 8, 11999, 43210),
    ("images2-3", 16, 12005, 43213),
    ("images2-3", 32, 12018, 43219),
    ("images2-3", 64, 12042, 43231),
    ("images2-3", 128, 12091, 43256),
    ("images2-3", 256, 12190, 43305),
    ("images2-3", 512, 12386, 43403),
    ("images2-3", 1024, 12780, 43599),
)

FIELD_DESCRIPTORS: dict[str, tuple[str, str, str]] = {
    "top_material_v6": ("|u1", "revision6_top_material", "Terrain-v2 material ID"),
    "owner_point_xy": ("<u4", "feedback6_owner_ray_points", "integer world block coordinates [x,y]"),
    "owner_height_v5": ("<i4", "feedback6_revision5_height_blocks", "revision-5 highest solid block Z"),
    "owner_temperature_v5": ("<f8", "feedback6_revision5_temperature", "degrees Celsius"),
    "owner_precipitation_v5": ("<f8", "feedback6_revision5_precipitation", "normalized flux"),
    "owner_top_material_v5": ("|u1", "feedback6_revision5_top_material", "Terrain-v2 material ID"),
    "owner_ground_material_v5": ("|u1", "feedback6_revision5_ground_material", "Terrain-v2 material ID"),
    "owner_top_material_v6": ("|u1", "feedback6_revision6_top_material", "Terrain-v2 material ID"),
}
ITEM_SIZES = {"<u4": 4, "<i4": 4, "<f8": 8, "|u1": 1}


class ReferenceError(ValueError):
    """The versioned revision-6 material reference does not match its contract."""


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
    for name in ("REFERENCE_MANIFEST_SHA256", "ORACLE_SOURCE_SHA256"):
        source = re.sub(
            rf'^{name} = ".*"$',
            f'{name} = "<pinned>"',
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
    resolved_root = root.resolve()
    resolved_file = (root / candidate).resolve()
    if not resolved_file.is_relative_to(resolved_root) or not resolved_file.is_file():
        raise ReferenceError(f"{description} is missing or escapes its root: {relative}")
    return resolved_file


def _revision5_field(
    fixture_root: Path,
    manifest: dict[str, Any],
    seed: int,
    field_name: str,
) -> bytes:
    record = next(
        field for field in manifest["fields"]
        if field["seed"] == seed and field["name"] == field_name
    )
    raw = _safe_file(fixture_root, record["file"], "revision-5 input field").read_bytes()
    if len(raw) != record["bytes"] or sha256_bytes(raw) != record["sha256"]:
        raise ReferenceError(f"revision-5 input field changed for seed {seed}: {field_name}")
    return raw


def _top_material_v5(height: int, temperature: float, precipitation: float) -> int:
    if height < BLOCK_SEA_LEVEL:
        return DIRT
    if temperature <= -2.0:
        return SNOW
    return GRASS if precipitation > 0.20 else DIRT


def _top_material_v6(height: int, temperature: float) -> int:
    if height < BLOCK_SEA_LEVEL:
        return DIRT
    return SNOW if temperature <= -2.0 else GRASS


def _top_material_from_revision5(height: int, revision5_top: int) -> int:
    if height < BLOCK_SEA_LEVEL:
        return DIRT
    if revision5_top == SNOW:
        return SNOW
    if revision5_top in (DIRT, GRASS):
        return GRASS
    raise ReferenceError(f"unsupported revision-5 top material ID: {revision5_top}")


def _top_material_field_v6(height_raw: bytes, top_raw: bytes) -> bytes:
    heights = [value[0] for value in struct.iter_unpack("<i", height_raw)]
    if len(heights) != len(top_raw):
        raise ReferenceError("revision-5 height and top-material sample counts differ")
    return bytes(
        _top_material_from_revision5(height, top)
        for height, top in zip(heights, top_raw)
    )


def _expected_identity(seed: int) -> str:
    return (
        f"terrain-v2;model-revision=6;source-map-sha256={SOURCE_MAP_SHA256};seed={seed}"
        ";resolution=256;epochs=12;erosion-steps=3;initial-climate-iterations=24"
        ";climate-iterations=80;currents=1;wind-sign=1;coastal-elaboration=1"
        ";shelf-exp=exp64-to-f32-v1"
    )


def _write_field(root: Path, seed: int, name: str, raw: bytes, count: int) -> dict[str, Any]:
    dtype, coordinate_set, unit = FIELD_DESCRIPTORS[name]
    path = root / f"seed-{seed}" / f"{name}.raw"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(raw)
    shape = [count, 2] if name == "owner_point_xy" else [count]
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
    revision5_fixture_root: Path,
    revision4_fixture_root: Path,
    revision4_archive_path: Path,
) -> dict[str, Any]:
    if output_root.exists():
        raise ReferenceError(f"refusing to overwrite existing revision-6 fixture: {output_root}")
    revision5_manifest = revision5.load_reference(revision5_fixture_root)
    if sha256_file(revision5_fixture_root / "manifest.json") != REVISION5_MANIFEST_SHA256:
        raise ReferenceError("revision-5 reference manifest identity differs")
    if sha256_file(REPOSITORY_ROOT / "script" / "terrain_revision_5_reference.py") \
            != REVISION5_SOURCE_SHA256:
        raise ReferenceError("revision-5 oracle source identity differs")
    revision4_manifest = revision4.load_reference(revision4_fixture_root)
    if sha256_file(revision4_archive_path) != REVISION4_ARCHIVE_SHA256:
        raise ReferenceError("approved revision-4 full-atlas archive identity differs")
    revision4.verify_full_archive(revision4_manifest, revision4_archive_path)

    output_root.parent.mkdir(parents=True, exist_ok=True)
    temporary_root = Path(tempfile.mkdtemp(prefix="terrain-v6-reference-", dir=output_root.parent))
    try:
        seed_records: list[dict[str, Any]] = []
        fields_manifest: list[dict[str, Any]] = []
        for seed in SEEDS:
            points_raw = _revision5_field(revision5_fixture_root, revision5_manifest, seed, "point_xy")
            height_raw = _revision5_field(revision5_fixture_root, revision5_manifest, seed, "height_v5")
            top_v5_raw = _revision5_field(
                revision5_fixture_root, revision5_manifest, seed, "top_material_v5"
            )
            count = len(top_v5_raw)
            top_v6_raw = _top_material_field_v6(height_raw, top_v5_raw)
            fields_manifest.append(_write_field(temporary_root, seed, "top_material_v6", top_v6_raw, count))
            seed_record = {
                "seed": seed,
                "configuration_identity": _expected_identity(seed),
                "revision5_point_xy_sha256": sha256_bytes(points_raw),
                "revision5_height_v5_sha256": sha256_bytes(height_raw),
                "revision5_top_material_v5_sha256": sha256_bytes(top_v5_raw),
                "revision6_top_material_sha256": sha256_bytes(top_v6_raw),
                "point_count": count,
                "changed_top_material_count": sum(
                    old != new for old, new in zip(top_v5_raw, top_v6_raw)
                ),
            }
            if seed == 42:
                full_fields = revision5._read_full_fields(revision4_archive_path, revision4_manifest, seed)
                precipitation_raw = _revision5_field(
                    revision5_fixture_root, revision5_manifest, seed, "input_precipitation"
                )
                full_fields["atlas__precipitation"] = revision5._as_float32(precipitation_raw)
                scale_path = REPOSITORY_ROOT / "tests" / "fixtures" / "terrain_v2" \
                    / f"seed-{seed}" / "atlas__vertical_scale.raw"
                scale_raw = scale_path.read_bytes()
                seed_input = next(record for record in revision5_manifest["seeds"] if record["seed"] == seed)
                if sha256_bytes(scale_raw) != seed_input["vertical_scale_f64le_sha256"]:
                    raise ReferenceError("revision-5 verified vertical-scale input changed")
                vertical_scale = struct.unpack("<d", scale_raw)[0]
                if vertical_scale != seed_input["vertical_scale_f64"]:
                    raise ReferenceError("revision-5 vertical-scale scalar differs from its manifest")
                owner_outputs = [
                    revision5._sample_surface(full_fields, seed, vertical_scale, x, y)
                    for _, _, x, y in OWNER_RAY_POINTS
                ]
                owner_xy_raw = b"".join(struct.pack("<II", x, y) for _, _, x, y in OWNER_RAY_POINTS)
                owner_height_raw = b"".join(
                    struct.pack("<i", int(output["height_v5"])) for output in owner_outputs
                )
                owner_temperature_raw = b"".join(
                    struct.pack(
                        "<d",
                        revision5._field(full_fields["atlas__temperature"], x, y),
                    )
                    for _, _, x, y in OWNER_RAY_POINTS
                )
                owner_precipitation_raw = b"".join(
                    struct.pack(
                        "<d",
                        revision5._field(full_fields["atlas__precipitation"], x, y),
                    )
                    for _, _, x, y in OWNER_RAY_POINTS
                )
                owner_top_v5_raw = bytes(int(output["top_material_v5"]) for output in owner_outputs)
                owner_ground_v5_raw = bytes(int(output["ground_material_v5"]) for output in owner_outputs)
                owner_top_v6_raw = bytes(
                    _top_material_v6(int(output["height_v5"]), temperature)
                    for output, temperature in zip(
                        owner_outputs,
                        struct.unpack(f"<{len(OWNER_RAY_POINTS)}d", owner_temperature_raw),
                    )
                )
                owner_fields = {
                    "owner_point_xy": owner_xy_raw,
                    "owner_height_v5": owner_height_raw,
                    "owner_temperature_v5": owner_temperature_raw,
                    "owner_precipitation_v5": owner_precipitation_raw,
                    "owner_top_material_v5": owner_top_v5_raw,
                    "owner_ground_material_v5": owner_ground_v5_raw,
                    "owner_top_material_v6": owner_top_v6_raw,
                }
                for name, raw in owner_fields.items():
                    fields_manifest.append(_write_field(
                        temporary_root, seed, name, raw, len(OWNER_RAY_POINTS)
                    ))
                seed_record["owner_point_count"] = len(OWNER_RAY_POINTS)
                seed_record["owner_top_material_changed_count"] = sum(
                    old != new for old, new in zip(owner_top_v5_raw, owner_top_v6_raw)
                )
            seed_records.append(seed_record)

        owner_samples = [
            {"view": view, "ray_distance_blocks": distance, "x": x, "y": y}
            for view, distance, x, y in OWNER_RAY_POINTS
        ]
        manifest = {
            "schema": SCHEMA,
            "terrain_version": 2,
            "model_revision": 6,
            "configuration_identity_schema": "terrain-v2 canonical identity with model-revision=6",
            "configuration_identities": {str(seed): _expected_identity(seed) for seed in SEEDS},
            "source_map_sha256": SOURCE_MAP_SHA256,
            "revision4_manifest_sha256": REVISION4_MANIFEST_SHA256,
            "revision4_full_atlas_archive_sha256": REVISION4_ARCHIVE_SHA256,
            "revision5_manifest_sha256": REVISION5_MANIFEST_SHA256,
            "revision5_oracle_source_sha256": REVISION5_SOURCE_SHA256,
            "revision5_fields_are_material_inputs_only": True,
            "material_rule": {
                "exposed_land": "Snow when temperature <= -2 C; otherwise Grass independent of precipitation",
                "submerged": "unchanged revision-5 Dirt/soil-band/Stone rule",
                "ground_material": "unchanged revision-5 Dirt or Ice rule",
                "height_climate_and_atlas": "unchanged revision-5 values",
            },
            "oracle": {
                "source": "script/terrain_revision_6_reference.py",
                "source_sha256": _oracle_source_sha256(),
                "revision5_surface_oracle": "script/terrain_revision_5_reference.py",
                "revision5_surface_oracle_sha256": REVISION5_SOURCE_SHA256,
                "owner_ray_source": "feedback-6 probe raw coordinates, replayed with pinned revision-5 Python sampler and approved revision-4 atlas fields",
                "runtime": {
                    "python": platform.python_version(),
                    "implementation": platform.python_implementation(),
                    "platform": platform.platform(),
                },
                "scope": "The revision-6 change is material classification only; these references do not claim universal host-libm equivalence.",
            },
            "owner_samples": owner_samples,
            "seeds": seed_records,
            "fields": fields_manifest,
        }
        manifest_path = temporary_root / "manifest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        os.replace(temporary_root, output_root)
    except BaseException:
        if temporary_root.exists():
            import shutil
            shutil.rmtree(temporary_root)
        raise
    return manifest


def load_reference(fixture_root: Path) -> dict[str, Any]:
    manifest_path = fixture_root / "manifest.json"
    actual_manifest_sha = sha256_file(manifest_path)
    if actual_manifest_sha != REFERENCE_MANIFEST_SHA256:
        raise ReferenceError(
            f"revision-6 manifest SHA-256 mismatch: expected {REFERENCE_MANIFEST_SHA256}, got {actual_manifest_sha}"
        )
    manifest = _read_json(manifest_path, "revision-6 reference manifest")
    if manifest.get("schema") != SCHEMA or manifest.get("model_revision") != 6:
        raise ReferenceError("unsupported revision-6 surface-material reference")
    if manifest.get("source_map_sha256") != SOURCE_MAP_SHA256 \
            or manifest.get("revision4_manifest_sha256") != REVISION4_MANIFEST_SHA256 \
            or manifest.get("revision4_full_atlas_archive_sha256") != REVISION4_ARCHIVE_SHA256 \
            or manifest.get("revision5_manifest_sha256") != REVISION5_MANIFEST_SHA256 \
            or manifest.get("revision5_oracle_source_sha256") != REVISION5_SOURCE_SHA256:
        raise ReferenceError("revision-6 source provenance differs")
    if manifest.get("seeds") is None or [record.get("seed") for record in manifest["seeds"]] != list(SEEDS):
        raise ReferenceError("revision-6 seed order differs")
    if manifest.get("configuration_identities") != {
        str(seed): _expected_identity(seed) for seed in SEEDS
    }:
        raise ReferenceError("revision-6 configuration identities differ")
    if sha256_file(REPOSITORY_ROOT / "script" / "terrain_revision_5_reference.py") \
            != REVISION5_SOURCE_SHA256:
        raise ReferenceError("revision-5 oracle source identity differs")
    oracle = manifest.get("oracle", {})
    if oracle.get("source_sha256") != ORACLE_SOURCE_SHA256 \
            or ORACLE_SOURCE_SHA256 != _oracle_source_sha256():
        raise ReferenceError("revision-6 oracle source digest differs")

    revision5_root = fixture_root.parent / "terrain_v2_revision_5"
    revision5_manifest = revision5.load_reference(revision5_root)
    expected_counts = {
        (seed, "top_material_v6"): revision5_manifest["sample_grid"]["points_per_seed"]
        for seed in SEEDS
    }
    expected_counts.update({(42, name): len(OWNER_RAY_POINTS) for name in FIELD_DESCRIPTORS if name.startswith("owner_")})
    records = manifest.get("fields")
    if not isinstance(records, list) or len(records) != len(expected_counts):
        raise ReferenceError("revision-6 field record count differs")
    seen: set[tuple[int, str]] = set()
    field_bytes: dict[tuple[int, str], bytes] = {}
    for record in records:
        seed = record.get("seed")
        name = record.get("name")
        key = (seed, name)
        if key not in expected_counts or key in seen:
            raise ReferenceError(f"duplicate or unsupported revision-6 field: {key}")
        seen.add(key)
        dtype, coordinate_set, _ = FIELD_DESCRIPTORS[name]
        count = expected_counts[key]
        shape = [count, 2] if name == "owner_point_xy" else [count]
        expected_bytes = math.prod(shape) * ITEM_SIZES[dtype]
        if record.get("dtype") != dtype or record.get("shape") != shape \
                or record.get("coordinate_set") != coordinate_set or record.get("bytes") != expected_bytes:
            raise ReferenceError(f"revision-6 field descriptor differs for {key}")
        raw = _safe_file(fixture_root, record.get("file", ""), "revision-6 field").read_bytes()
        if len(raw) != expected_bytes or sha256_bytes(raw) != record.get("sha256"):
            raise ReferenceError(f"revision-6 field bytes differ for {key}")
        field_bytes[key] = raw
    if seen != set(expected_counts):
        raise ReferenceError("revision-6 field set is incomplete")

    for seed in SEEDS:
        height_raw = _revision5_field(revision5_root, revision5_manifest, seed, "height_v5")
        top_v5_raw = _revision5_field(revision5_root, revision5_manifest, seed, "top_material_v5")
        top_v6_raw = field_bytes[(seed, "top_material_v6")]
        if top_v6_raw != _top_material_field_v6(height_raw, top_v5_raw):
            raise ReferenceError(f"revision-6 material transform differs for seed {seed}")
    owner_coords = b"".join(struct.pack("<II", x, y) for _, _, x, y in OWNER_RAY_POINTS)
    if field_bytes[(42, "owner_point_xy")] != owner_coords:
        raise ReferenceError("revision-6 feedback ray coordinates differ")
    owner_heights = [
        value[0] for value in struct.iter_unpack("<i", field_bytes[(42, "owner_height_v5")])
    ]
    owner_temperatures = [
        value[0] for value in struct.iter_unpack("<d", field_bytes[(42, "owner_temperature_v5")])
    ]
    owner_precipitations = [
        value[0] for value in struct.iter_unpack("<d", field_bytes[(42, "owner_precipitation_v5")])
    ]
    owner_top_v5 = field_bytes[(42, "owner_top_material_v5")]
    expected_v5 = bytes(
        _top_material_v5(height, temperature, precipitation)
        for height, temperature, precipitation in zip(
            owner_heights,
            owner_temperatures,
            owner_precipitations,
        )
    )
    if owner_top_v5 != expected_v5:
        raise ReferenceError("revision-5 owner material inputs differ from its climate rule")
    owner_expected = bytes(
        _top_material_v6(height, temperature)
        for height, temperature in zip(owner_heights, owner_temperatures)
    )
    if field_bytes[(42, "owner_top_material_v6")] != owner_expected:
        raise ReferenceError("revision-6 feedback owner material transform differs")
    if manifest.get("owner_samples") != [
        {"view": view, "ray_distance_blocks": distance, "x": x, "y": y}
        for view, distance, x, y in OWNER_RAY_POINTS
    ]:
        raise ReferenceError("revision-6 owner sample descriptions differ")
    return manifest


def verify_reference(fixture_root: Path) -> dict[str, int]:
    manifest = load_reference(fixture_root)
    return {
        "seeds": len(SEEDS),
        "sampled_columns": sum(record["point_count"] for record in manifest["seeds"]),
        "owner_ray_samples": len(OWNER_RAY_POINTS),
        "changed_sampled_top_materials": sum(
            record["changed_top_material_count"] for record in manifest["seeds"]
        ),
        "changed_owner_ray_materials": manifest["seeds"][SEEDS.index(42)]["owner_top_material_changed_count"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    verify_parser = subparsers.add_parser("verify", help="verify the immutable revision-6 material fixture")
    verify_parser.add_argument(
        "--fixtures",
        type=Path,
        default=REPOSITORY_ROOT / "tests" / "fixtures" / "terrain_v2_revision_6",
    )
    record_parser = subparsers.add_parser("record", help="record a revision-6 material fixture without overwrite")
    record_parser.add_argument(
        "--output",
        type=Path,
        default=REPOSITORY_ROOT / "tests" / "fixtures" / "terrain_v2_revision_6",
    )
    record_parser.add_argument(
        "--revision5-fixtures",
        type=Path,
        default=REPOSITORY_ROOT / "tests" / "fixtures" / "terrain_v2_revision_5",
    )
    record_parser.add_argument(
        "--revision4-fixtures",
        type=Path,
        default=REPOSITORY_ROOT / "tests" / "fixtures" / "terrain_v2_revision_4",
    )
    record_parser.add_argument("--revision4-archive", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "verify":
            result = verify_reference(args.fixtures)
            print(json.dumps({"fixture": str(args.fixtures), **result}, sort_keys=True))
            return 0
        manifest = record_reference(
            args.output,
            args.revision5_fixtures,
            args.revision4_fixtures,
            args.revision4_archive,
        )
        digest = sha256_file(args.output / "manifest.json")
        print(json.dumps({"fixture": str(args.output), "manifest_sha256": digest,
                          "fields": len(manifest["fields"])}, sort_keys=True))
        return 0
    except (OSError, ReferenceError, revision4.ReferenceError, revision5.ReferenceError) as error:
        parser.exit(2, f"revision-6 reference error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
