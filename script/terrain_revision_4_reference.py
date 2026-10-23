"""Verify and compare the versioned Terrain-v2 model-revision-4 reference."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
import zipfile
from typing import Any


REFERENCE_SCHEMA = "minecraftclone.terrain-v2-model-revision-reference.v1"
REFERENCE_MANIFEST_SHA256 = "9a2644bbe81a88876bac4f84f334f8c0a5d3525295e83122dcce10f92f02d6eb"
ORIGINAL_V3_MANIFEST_SHA256 = "d0e6ade7aed51a34a864c1dc497d3b3921d10204a8cc3602b71f7d53ab96db97"
SOURCE_MAP_SHA256 = "5de753a4068c71d742f5ce7e75e4e8bcabbaad5fddaf6c8404dc41ba00d45704"
KERNEL_IDENTITY = "shelf-exp=exp64-to-f32-v1"
SEEDS = (0, 1, 42, 777, 2004, 8800555)
ATLAS_FIELDS = {
    "atlas__elevation": "height.f32le",
    "atlas__temperature": "temperature.f32le",
    "atlas__water_level": "water.f32le",
    "atlas__ocean_mask": "ocean-mask.u8",
    "atlas__lake_mask": "lake-mask.u8",
    "atlas__river_mask": "river-mask.u8",
}
ITEM_SIZES = {"<f4": 4, "<i4": 4, "|u1": 1}


class ReferenceError(ValueError):
    """The versioned reference or candidate does not match its contract."""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(file_path: Path) -> str:
    digest = hashlib.sha256()
    with file_path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _read_json(file_path: Path, description: str) -> dict[str, Any]:
    try:
        value = json.loads(file_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ReferenceError(f"cannot read {description} at {file_path}: {error}") from error
    if not isinstance(value, dict):
        raise ReferenceError(f"{description} must contain a JSON object")
    return value


def _safe_path(root: Path, relative: str, description: str) -> Path:
    relative_path = Path(relative)
    if relative_path.is_absolute():
        raise ReferenceError(f"{description} path must be relative: {relative}")
    root_resolved = root.resolve()
    resolved = (root / relative_path).resolve()
    if not resolved.is_relative_to(root_resolved):
        raise ReferenceError(f"{description} path escapes its root: {relative}")
    if not resolved.is_file():
        raise ReferenceError(f"{description} file is missing: {relative}")
    return resolved


def load_reference(fixture_root: Path) -> dict[str, Any]:
    manifest_path = fixture_root / "manifest.json"
    manifest_bytes = manifest_path.read_bytes()
    actual_manifest_sha = sha256_bytes(manifest_bytes)
    if actual_manifest_sha != REFERENCE_MANIFEST_SHA256:
        raise ReferenceError(
            "revision-4 reference manifest SHA-256 mismatch: "
            f"expected {REFERENCE_MANIFEST_SHA256}, got {actual_manifest_sha}"
        )
    manifest = _read_json(manifest_path, "revision-4 reference manifest")
    if manifest.get("schema") != REFERENCE_SCHEMA:
        raise ReferenceError(f"unsupported reference schema: {manifest.get('schema')!r}")
    if manifest.get("terrain_version") != 2 or manifest.get("model_revision") != 4:
        raise ReferenceError("reference must identify Terrain-v2 model revision 4")
    if manifest.get("numerical_kernel_identity") != KERNEL_IDENTITY:
        raise ReferenceError("reference numerical kernel identity differs")
    if manifest.get("source_map_sha256") != SOURCE_MAP_SHA256:
        raise ReferenceError("reference source-map digest differs")
    if manifest.get("seeds") != list(SEEDS) or manifest.get("resolution") != 256:
        raise ReferenceError("reference seed order or atlas resolution differs")
    if manifest.get("provenance", {}).get("original_python_reference_manifest_sha256") \
            != ORIGINAL_V3_MANIFEST_SHA256:
        raise ReferenceError("reference does not bind the preserved revision-3 manifest")

    v3_manifest = fixture_root.parent / "terrain_v2" / "manifest.json"
    if sha256_file(v3_manifest) != ORIGINAL_V3_MANIFEST_SHA256:
        raise ReferenceError("preserved revision-3 manifest has changed")

    field_keys: set[tuple[int, str]] = set()
    for field in manifest.get("fields", []):
        key = (field.get("seed"), field.get("name"))
        if key in field_keys:
            raise ReferenceError(f"duplicate sampled field: {key}")
        field_keys.add(key)
        dtype = field.get("dtype")
        if dtype not in ITEM_SIZES:
            raise ReferenceError(f"unsupported sampled-field dtype: {dtype!r}")
        shape = field.get("shape")
        expected_bytes = ITEM_SIZES[dtype]
        for dimension in shape:
            expected_bytes *= dimension
        field_path = _safe_path(fixture_root, field.get("file", ""), "sampled field")
        actual_bytes = field_path.read_bytes()
        if len(actual_bytes) != expected_bytes:
            raise ReferenceError(f"sampled field has wrong byte length: {field_path}")
        actual_sha = sha256_bytes(actual_bytes)
        if actual_sha != field.get("sha256"):
            raise ReferenceError(f"sampled field SHA-256 mismatch: {field_path}")
    if len(field_keys) != 61:
        raise ReferenceError(f"expected 61 sampled fields, got {len(field_keys)}")

    full_keys: set[tuple[int, str]] = set()
    for field in manifest.get("full_atlas_fields", []):
        key = (field.get("seed"), field.get("name"))
        if key in full_keys:
            raise ReferenceError(f"duplicate full-atlas field digest: {key}")
        full_keys.add(key)
        dtype = field.get("dtype")
        if dtype not in ITEM_SIZES or field.get("shape") != [256, 256]:
            raise ReferenceError(f"invalid full-atlas field descriptor: {field}")
        if field.get("bytes") != 256 * 256 * ITEM_SIZES[dtype]:
            raise ReferenceError(f"invalid full-atlas field byte count: {field}")
        if len(field.get("sha256", "")) != 64:
            raise ReferenceError(f"invalid full-atlas field digest: {field}")
    if len(full_keys) != 36:
        raise ReferenceError(f"expected 36 full-atlas field digests, got {len(full_keys)}")
    return manifest


def _expected_configuration_identity(seed: int) -> str:
    return (
        "terrain-v2;model-revision=4;source-map-sha256=" + SOURCE_MAP_SHA256
        + f";seed={seed};resolution=256;epochs=12;erosion-steps=3"
        + ";initial-climate-iterations=24;climate-iterations=80"
        + ";currents=1;wind-sign=1;coastal-elaboration=1;" + KERNEL_IDENTITY
    )


def verify_full_archive(manifest: dict[str, Any], archive_path: Path | None = None) -> int:
    archive = archive_path
    if archive is None:
        archive = Path(manifest["approved_candidate_full_atlas_archive"]["retained_path"])
    if not archive.is_file():
        if archive_path is not None:
            raise ReferenceError(f"approved full-field archive is missing: {archive}")
        return 0
    expected_archive_sha = manifest["approved_candidate_full_atlas_archive"]["sha256"]
    actual_archive_sha = sha256_file(archive)
    if actual_archive_sha != expected_archive_sha:
        raise ReferenceError(
            "approved candidate archive SHA-256 mismatch: "
            f"expected {expected_archive_sha}, got {actual_archive_sha}"
        )

    checked = 0
    with zipfile.ZipFile(archive) as archive_reader:
        for field in manifest["full_atlas_fields"]:
            raw = archive_reader.read(field["file"])
            if len(raw) != field["bytes"] or sha256_bytes(raw) != field["sha256"]:
                raise ReferenceError(f"approved full-atlas field differs: {field['file']}")
            checked += 1
    return checked


def _describe_first_difference(expected: bytes, actual: bytes, dtype: str) -> str:
    limit = min(len(expected), len(actual))
    first = next((index for index in range(limit) if expected[index] != actual[index]), limit)
    item_size = ITEM_SIZES[dtype]
    coordinate = first // (256 * item_size), (first // item_size) % 256
    expected_value = expected[first:first + item_size].hex() if first < len(expected) else "<eof>"
    actual_value = actual[first:first + item_size].hex() if first < len(actual) else "<eof>"
    return (
        f"first byte {first}, atlas (row={coordinate[0]}, col={coordinate[1]}), "
        f"expected raw {expected_value}, actual raw {actual_value}; lengths "
        f"{len(expected)} vs {len(actual)}"
    )


def compare_candidate(
    fixture_root: Path,
    candidate_root: Path,
    full_archive: Path | None,
) -> list[str]:
    manifest = load_reference(fixture_root)
    candidate_manifest = _read_json(candidate_root / "manifest.json", "candidate export manifest")
    if candidate_manifest.get("schema") != "mc-world-map/1" \
            or candidate_manifest.get("status") != "complete":
        raise ReferenceError("candidate export is not a complete mc-world-map/1 result")
    if candidate_manifest.get("atlas_size") != 256 \
            or candidate_manifest.get("seeds") != list(SEEDS):
        raise ReferenceError("candidate seed order or atlas size differs from revision 4")

    archive = full_archive
    if archive is None:
        archive = Path(manifest["approved_candidate_full_atlas_archive"]["retained_path"])
    elif not archive.is_file():
        raise ReferenceError(f"approved full-field archive is missing: {archive}")
    archive_reader: zipfile.ZipFile | None = None
    if archive.is_file():
        archive_reader = zipfile.ZipFile(archive)
        actual_archive_sha = sha256_file(archive)
        expected_archive_sha = manifest["approved_candidate_full_atlas_archive"]["sha256"]
        if actual_archive_sha != expected_archive_sha:
            archive_reader.close()
            raise ReferenceError(
                "approved candidate archive SHA-256 mismatch: "
                f"expected {expected_archive_sha}, got {actual_archive_sha}"
            )

    mismatches: list[str] = []
    for seed in SEEDS:
        seed_root = candidate_root / f"seed-{seed}"
        metadata = _read_json(seed_root / "metadata.json", f"seed {seed} metadata")
        expected_identity = manifest["configuration_identities"][str(seed)]
        if metadata.get("configuration_identity") != expected_identity:
            raise ReferenceError(f"seed {seed} configuration identity differs")
        for record in (item for item in manifest["full_atlas_fields"] if item["seed"] == seed):
            filename = Path(record["file"]).name
            actual_path = seed_root / "fields" / filename
            actual = actual_path.read_bytes()
            actual_sha = sha256_bytes(actual)
            if archive_reader is not None:
                expected = archive_reader.read(record["file"])
                if actual != expected:
                    mismatches.append(
                        f"seed {seed} {record['name']}: "
                        + _describe_first_difference(expected, actual, record["dtype"])
                    )
            elif actual_sha != record["sha256"]:
                mismatches.append(
                    f"seed {seed} {record['name']}: expected SHA-256 {record['sha256']}, "
                    f"got {actual_sha}"
                )
    if archive_reader is not None:
        archive_reader.close()
    return mismatches


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    verify_parser = subparsers.add_parser("verify", help="verify the checked-in v4 fixture")
    verify_parser.add_argument(
        "--fixture",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "tests/fixtures/terrain_v2_revision_4",
    )
    verify_parser.add_argument("--full-archive", type=Path)
    compare_parser = subparsers.add_parser("compare", help="compare a complete native atlas export")
    compare_parser.add_argument("--candidate", type=Path, required=True)
    compare_parser.add_argument(
        "--fixture",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "tests/fixtures/terrain_v2_revision_4",
    )
    compare_parser.add_argument("--full-archive", type=Path)
    arguments = parser.parse_args()

    try:
        if arguments.command == "verify":
            manifest = load_reference(arguments.fixture)
            full_archive_fields = verify_full_archive(manifest, arguments.full_archive)
            print(
                "verified revision-4 reference: "
                f"manifest={REFERENCE_MANIFEST_SHA256}, sampled_fields={len(manifest['fields'])}, "
                f"full_field_hashes={len(manifest['full_atlas_fields'])}, "
                f"retained_archive_fields={full_archive_fields}"
            )
            return 0
        mismatches = compare_candidate(arguments.fixture, arguments.candidate, arguments.full_archive)
        if mismatches:
            for mismatch in mismatches:
                print(mismatch, file=sys.stderr)
            return 1
        print("revision-4 candidate atlas fields match the approved six-seed reference")
        return 0
    except (OSError, ReferenceError, zipfile.BadZipFile, KeyError, TypeError) as error:
        print(f"revision-4 reference error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
