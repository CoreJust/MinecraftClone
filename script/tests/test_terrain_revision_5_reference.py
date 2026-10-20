"""Tests for the independent Terrain-v2 revision-5 local-surface reference."""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import struct
import tempfile
import unittest

from script import terrain_revision_5_reference as reference


FIXTURE_ROOT = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "terrain_v2_revision_5"


def copy_validation_inputs(fixture_parent: Path) -> None:
    shutil.copytree(
        FIXTURE_ROOT.parent / "terrain_v2_revision_4",
        fixture_parent / "terrain_v2_revision_4",
    )
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


class TerrainRevision5ReferenceTests(unittest.TestCase):
    def test_manifest_binds_v4_inputs_and_six_seed_local_samples(self) -> None:
        manifest = reference.load_reference(FIXTURE_ROOT)

        self.assertEqual(manifest["model_revision"], 5)
        self.assertEqual(manifest["revision4_manifest_sha256"], reference.REVISION4_MANIFEST_SHA256)
        self.assertEqual(manifest["source_map_sha256"], reference.SOURCE_MAP_SHA256)
        self.assertEqual([item["seed"] for item in manifest["seeds"]], list(reference.SEEDS))
        self.assertEqual(len(manifest["fields"]), len(reference.SEEDS) * len(reference.FIELD_DESCRIPTORS))
        self.assertEqual(
            manifest["sample_grid"]["points_per_seed"],
            len(reference._sample_points()),
        )
        self.assertEqual(
            [item["python_native_differing_float32_count"] for item in manifest["seeds"]],
            [0, 0, 0, 18, 5, 0],
        )
        self.assertTrue(all(
            len((FIXTURE_ROOT / f"seed-{seed}" / "input_precipitation.raw").read_bytes())
            == 256 * 256 * 4
            for seed in reference.SEEDS
        ))

    def test_samples_exercise_offsets_chunk_boundaries_and_periodic_wrap(self) -> None:
        manifest = reference.load_reference(FIXTURE_ROOT)
        points_record = next(
            record for record in manifest["fields"]
            if record["seed"] == 42 and record["name"] == "point_xy"
        )
        raw = (FIXTURE_ROOT / points_record["file"]).read_bytes()
        points = set(struct.iter_unpack("<II", raw))
        self.assertIn((32_896 + 23, 32_768 + 37), points)
        self.assertIn((32_896 + 23 + 1_023, 32_768 + 37), points)
        self.assertIn((32_896 - 1, 32_768 - 1), points)
        self.assertIn((32_896 + 16, 32_768 + 16), points)
        for point in ((65_535, 65_535), (0, 0), (1, 1), (65_535, 0), (0, 65_535)):
            self.assertIn(point, points)

        for seed in reference.SEEDS:
            height_v4 = (FIXTURE_ROOT / f"seed-{seed}" / "height_v4.raw").read_bytes()
            height_v5 = (FIXTURE_ROOT / f"seed-{seed}" / "height_v5.raw").read_bytes()
            before = struct.unpack(f"<{len(height_v4) // 4}i", height_v4)
            after = struct.unpack(f"<{len(height_v5) // 4}i", height_v5)
            self.assertTrue(any(left != right for left, right in zip(before, after)), seed)

    def test_changed_sample_bytes_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture_parent = Path(temporary) / "fixtures"
            fixture_parent.mkdir()
            root = fixture_parent / "terrain_v2_revision_5"
            shutil.copytree(FIXTURE_ROOT, root)
            copy_validation_inputs(fixture_parent)
            path = root / "seed-42" / "height_v5.raw"
            changed = bytearray(path.read_bytes())
            changed[0] ^= 0x01
            path.write_bytes(changed)
            with self.assertRaisesRegex(reference.ReferenceError, "field bytes differ"):
                reference.load_reference(root)

    def test_changed_manifest_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture_parent = Path(temporary) / "fixtures"
            fixture_parent.mkdir()
            shutil.copytree(FIXTURE_ROOT, fixture_parent / "terrain_v2_revision_5")
            copy_validation_inputs(fixture_parent)
            root = fixture_parent / "terrain_v2_revision_5"
            path = root / "manifest.json"
            manifest = json.loads(path.read_text(encoding="utf-8"))
            manifest["model_revision"] = 4
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(reference.ReferenceError, "manifest SHA-256 mismatch"):
                reference.load_reference(root)


if __name__ == "__main__":
    unittest.main()
