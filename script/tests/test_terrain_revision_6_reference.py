"""Tests for the versioned Terrain-v2 revision-6 material reference."""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import struct
import tempfile
import unittest

from script import terrain_revision_6_reference as reference


FIXTURE_ROOT = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "terrain_v2_revision_6"


def copy_validation_inputs(fixture_parent: Path) -> None:
    for fixture_name in ("terrain_v2_revision_4", "terrain_v2_revision_5", "terrain_v2_revision_6"):
        shutil.copytree(FIXTURE_ROOT.parent / fixture_name, fixture_parent / fixture_name)

    original_root = FIXTURE_ROOT.parent / "terrain_v2"
    copied_original = fixture_parent / "terrain_v2"
    copied_original.mkdir()
    shutil.copy2(original_root / "manifest.json", copied_original / "manifest.json")
    for seed in reference.SEEDS:
        seed_root = copied_original / f"seed-{seed}"
        seed_root.mkdir()
        shutil.copy2(
            original_root / f"seed-{seed}" / "atlas__vertical_scale.raw",
            seed_root / "atlas__vertical_scale.raw",
        )


class TerrainRevision6ReferenceTests(unittest.TestCase):
    def test_manifest_binds_six_seed_materials_and_feedback_rays(self) -> None:
        manifest = reference.load_reference(FIXTURE_ROOT)

        self.assertEqual(manifest["model_revision"], 6)
        self.assertEqual(manifest["revision5_manifest_sha256"], reference.REVISION5_MANIFEST_SHA256)
        self.assertEqual(manifest["source_map_sha256"], reference.SOURCE_MAP_SHA256)
        self.assertEqual([item["seed"] for item in manifest["seeds"]], list(reference.SEEDS))
        self.assertEqual(
            [item["name"] for item in manifest["fields"] if item["seed"] == 42 and item["name"].startswith("owner_")],
            [
                "owner_point_xy",
                "owner_height_v5",
                "owner_temperature_v5",
                "owner_precipitation_v5",
                "owner_top_material_v5",
                "owner_ground_material_v5",
                "owner_top_material_v6",
            ],
        )
        self.assertEqual(len(manifest["owner_samples"]), len(reference.OWNER_RAY_POINTS))
        self.assertEqual(
            [item["changed_top_material_count"] for item in manifest["seeds"]],
            [
                sum(
                    before != after
                    for before, after in zip(
                        (FIXTURE_ROOT.parent / "terrain_v2_revision_5" / f"seed-{seed}" / "top_material_v5.raw").read_bytes(),
                        (FIXTURE_ROOT / f"seed-{seed}" / "top_material_v6.raw").read_bytes(),
                    )
                )
                for seed in reference.SEEDS
            ],
        )

    def test_material_rule_changes_warm_land_but_preserves_snow_and_submerged_dirt(self) -> None:
        self.assertEqual(reference._top_material_v5(500, 12.0, 0.1), reference.DIRT)
        self.assertEqual(reference._top_material_v6(500, 12.0), reference.GRASS)
        self.assertEqual(reference._top_material_v5(500, 12.0, 0.21), reference.GRASS)
        self.assertEqual(reference._top_material_v6(500, -2.0), reference.SNOW)
        self.assertEqual(reference._top_material_v6(383, 12.0), reference.DIRT)

    def test_feedback_rays_cover_changed_land_and_unchanged_submerged_top(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture_parent = Path(temporary)
            copy_validation_inputs(fixture_parent)
            manifest = reference.load_reference(fixture_parent / "terrain_v2_revision_6")
            owner_fields = {
                record["name"]: (fixture_parent / "terrain_v2_revision_6" / record["file"]).read_bytes()
                for record in manifest["fields"]
                if record["seed"] == 42 and record["name"].startswith("owner_")
            }

        heights = [value[0] for value in struct.iter_unpack("<i", owner_fields["owner_height_v5"])]
        revision5_top = owner_fields["owner_top_material_v5"]
        revision6_top = owner_fields["owner_top_material_v6"]
        temperatures = [
            value[0] for value in struct.iter_unpack("<d", owner_fields["owner_temperature_v5"])
        ]
        precipitation = [
            value[0] for value in struct.iter_unpack("<d", owner_fields["owner_precipitation_v5"])
        ]
        self.assertLess(heights[0], reference.BLOCK_SEA_LEVEL)
        self.assertEqual(revision5_top[0], reference.DIRT)
        self.assertEqual(revision6_top[0], reference.DIRT)
        self.assertGreaterEqual(heights[9], reference.BLOCK_SEA_LEVEL)
        self.assertGreater(temperatures[9], -2.0)
        self.assertLessEqual(precipitation[9], 0.20)
        self.assertEqual(revision5_top[9], reference.DIRT)
        self.assertEqual(revision6_top[9], reference.GRASS)

    def test_changed_revision6_bytes_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture_parent = Path(temporary)
            copy_validation_inputs(fixture_parent)
            root = fixture_parent / "terrain_v2_revision_6"
            path = root / "seed-42" / "top_material_v6.raw"
            changed = bytearray(path.read_bytes())
            changed[0] ^= 0x01
            path.write_bytes(changed)
            with self.assertRaisesRegex(reference.ReferenceError, "field bytes differ"):
                reference.load_reference(root)

    def test_changed_manifest_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture_parent = Path(temporary)
            copy_validation_inputs(fixture_parent)
            root = fixture_parent / "terrain_v2_revision_6"
            path = root / "manifest.json"
            manifest = json.loads(path.read_text(encoding="utf-8"))
            manifest["model_revision"] = 5
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(reference.ReferenceError, "manifest SHA-256 mismatch"):
                reference.load_reference(root)


if __name__ == "__main__":
    unittest.main()
