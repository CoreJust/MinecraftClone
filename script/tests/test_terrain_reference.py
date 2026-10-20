"""Contract tests for the immutable Terrain-v2 reference comparator."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import unittest

from script import terrain_reference


FIXTURE_ROOT = Path(__file__).resolve().parents[1] / ".." / "tests" / "fixtures" / "terrain_v2"
FIXTURE_ROOT = FIXTURE_ROOT.resolve()


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _candidate_from_reference(reference_root: Path, candidate_root: Path) -> Path:
    reference = terrain_reference.validate_reference(reference_root)
    candidate_root.mkdir(parents=True)
    candidate_seeds = []
    for seed in reference["seeds"]:
        candidate_fields = []
        for field in seed["fields"]:
            relative = Path(field["file"])
            candidate_file = candidate_root / relative
            candidate_file.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(reference_root / relative, candidate_file)
            candidate_fields.append({
                key: field[key]
                for key in ("name", "dtype", "shape", "order", "coordinate_set", "unit")
            } | {"file": field["file"], "sha256": _sha256(candidate_file)})
        candidate_seeds.append({
            "seed": seed["seed"],
            "config": seed["config"],
            "config_sha256": seed["config_sha256"],
            "fields": candidate_fields,
        })

    manifest_path = reference_root / "manifest.json"
    manifest_hash = _sha256(manifest_path)
    candidate = {
        "schema": terrain_reference.CANDIDATE_SCHEMA,
        "terrain_version": reference["terrain_version"],
        "reference_manifest_sha256": manifest_hash,
        "reference_model_sha256": reference["model"]["model_sha256"],
        "reference_sources": reference["model"]["sources"],
        "world": reference["world"],
        "seeds": candidate_seeds,
    }
    output = candidate_root / "candidate.json"
    output.write_text(json.dumps(candidate, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    return output


class TerrainReferenceTests(unittest.TestCase):
    def test_reference_manifest_binds_all_seeds_and_stage_surface_fields(self) -> None:
        reference = terrain_reference.validate_reference(FIXTURE_ROOT)

        self.assertEqual(reference["schema"], terrain_reference.REFERENCE_SCHEMA)
        self.assertEqual(
            _sha256(FIXTURE_ROOT / "manifest.json"),
            terrain_reference.FROZEN_REFERENCE_MANIFEST_SHA256,
        )
        self.assertEqual(reference["model"]["model_sha256"], terrain_reference.FROZEN_MODEL_SHA256)
        self.assertEqual(
            [item["seed"] for item in reference["seeds"]],
            [0, 1, 42, 777, 2004, 8800555],
        )
        for seed in reference["seeds"]:
            self.assertEqual(
                [field["name"] for field in seed["fields"]],
                list(terrain_reference.REQUIRED_FIELDS),
            )
            self.assertEqual(seed["config"]["seed"], seed["seed"])

    def test_seed42_rng_oracle_pins_numpy_calls_and_little_endian_float_bits(self) -> None:
        oracle_path = FIXTURE_ROOT / "rng_oracle_seed42.json"
        oracle = json.loads(oracle_path.read_text(encoding="utf-8"))

        self.assertEqual(oracle["runtime"], {
            "bit_generator": "PCG64",
            "numpy": "2.5.4",
            "python": "3.12.14",
        })
        self.assertEqual(oracle["random_raw_call"]["values_hex"], [
            "c621fbcd16d92688", "705a5661a791ffc1", "dbcd12c26eda1624",
            "b286b60e1600888d", "181c01b5339381eb", "f9c262ed86c7538c",
            "c2da0d2fbc5a4471", "c93b82a3b7ac9740",
        ])
        calls = {call["name"]: call for call in oracle["calls"]}
        self.assertEqual(calls["integers(-2,3,size=2)"]["values"], [-2, 1])
        self.assertEqual(calls["uniform(0,2*pi)"]["bits_le"], ["776b74c4780f0640"])
        self.assertEqual(calls["uniform(0,65536,size=(2,2))"]["bits_le"], [
            "42db4d58a279eb40", "11c0c2c1d650e640",
            "809333b5011cb840", "ead8b05d4c38ef40",
        ])
        self.assertEqual(calls["random(4)"]["bits_le"], [
            "488bf7a5415be83f", "92f576547027e93f",
            "68b1410b0766c03f", "6ebe758a1fd3dc3f",
        ])
        self.assertEqual(calls["vonmises(0.75,1.8,size=3)"]["bits_le"], [
            "349c8d7336a2f33f", "60a860e2112ff1bf", "6c3cbf1d443af93f",
        ])

    def test_changed_manifest_is_rejected_instead_of_accepting_a_new_baseline(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            copied_reference = Path(temporary) / "reference"
            shutil.copytree(FIXTURE_ROOT, copied_reference)
            manifest_path = copied_reference / "manifest.json"
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["world"]["atlas_resolution"] = 512
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            with self.assertRaisesRegex(terrain_reference.ReferenceError, "manifest identity drift"):
                terrain_reference.validate_reference(copied_reference)

    def test_exact_candidate_matches_the_frozen_raw_fields_and_pixels(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            candidate_root = Path(temporary) / "candidate"
            _candidate_from_reference(FIXTURE_ROOT, candidate_root)

            result = terrain_reference.compare_candidate(candidate_root, FIXTURE_ROOT)

        self.assertTrue(result["passed"], result)
        self.assertEqual(result["compared_field_count"], 6 * len(terrain_reference.REQUIRED_FIELDS))
        self.assertEqual(result["mismatches"], [])

    def test_byte_differences_report_stage_indices_and_rgba_pixel_coordinates(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            candidate_root = Path(temporary) / "candidate"
            manifest_path = _candidate_from_reference(FIXTURE_ROOT, candidate_root)
            candidate = json.loads(manifest_path.read_text(encoding="utf-8"))
            seed = candidate["seeds"][2]
            stage_field = next(field for field in seed["fields"] if field["name"] == "atlas__temperature")
            pixel_field = next(field for field in seed["fields"] if field["name"] == "pixel_rgba")
            stage_path = candidate_root / stage_field["file"]
            pixel_path = candidate_root / pixel_field["file"]

            stage_data = bytearray(stage_path.read_bytes())
            stage_data[(3 * 16 + 5) * 4] ^= 0x01
            stage_path.write_bytes(stage_data)
            stage_field["sha256"] = _sha256(stage_path)

            pixel_data = bytearray(pixel_path.read_bytes())
            pixel_offset = (7 * 16 + 11) * 4 + 2
            pixel_data[pixel_offset] ^= 0x01
            pixel_path.write_bytes(pixel_data)
            pixel_field["sha256"] = _sha256(pixel_path)
            manifest_path.write_text(json.dumps(candidate, sort_keys=True, indent=2) + "\n", encoding="utf-8")

            result = terrain_reference.compare_candidate(candidate_root, FIXTURE_ROOT)

        self.assertFalse(result["passed"])
        by_name = {item["field"]: item for item in result["mismatches"]}
        self.assertEqual(by_name["atlas__temperature"]["items"][0]["index"], [3, 5])
        self.assertEqual(by_name["pixel_rgba"]["items"][0]["pixel"], [7, 11])
        self.assertEqual(by_name["pixel_rgba"]["items"][0]["channels"], [2])

    def test_candidate_rejects_reference_source_or_configuration_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            candidate_root = Path(temporary) / "candidate"
            manifest_path = _candidate_from_reference(FIXTURE_ROOT, candidate_root)
            candidate = json.loads(manifest_path.read_text(encoding="utf-8"))
            candidate["reference_model_sha256"] = "0" * 64
            manifest_path.write_text(json.dumps(candidate), encoding="utf-8")
            with self.assertRaisesRegex(terrain_reference.ReferenceError, "source identity"):
                terrain_reference.compare_candidate(candidate_root, FIXTURE_ROOT)

            candidate["reference_model_sha256"] = terrain_reference.validate_reference(FIXTURE_ROOT)["model"]["model_sha256"]
            candidate["seeds"][0]["config"]["epochs"] += 1
            candidate["seeds"][0]["config_sha256"] = hashlib.sha256(
                json.dumps(candidate["seeds"][0]["config"], sort_keys=True).encode()
            ).hexdigest()
            manifest_path.write_text(json.dumps(candidate), encoding="utf-8")
            with self.assertRaisesRegex(terrain_reference.ReferenceError, "configuration drift"):
                terrain_reference.compare_candidate(candidate_root, FIXTURE_ROOT)

    def test_reference_payload_hash_is_checked_before_comparison(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            copied_reference = Path(temporary) / "reference"
            shutil.copytree(FIXTURE_ROOT, copied_reference)
            reference = json.loads((copied_reference / "manifest.json").read_text(encoding="utf-8"))
            field_path = copied_reference / reference["seeds"][0]["fields"][0]["file"]
            data = bytearray(field_path.read_bytes())
            data[0] ^= 0x01
            field_path.write_bytes(data)

            with self.assertRaisesRegex(terrain_reference.ReferenceError, "fixture hash mismatch"):
                terrain_reference.validate_reference(copied_reference)


if __name__ == "__main__":
    unittest.main()
