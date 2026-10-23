"""Contract tests for the versioned Terrain-v2 model-revision-4 reference."""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import tempfile
import unittest

from script import terrain_revision_4_reference


FIXTURE_ROOT = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "terrain_v2_revision_4"
V3_MANIFEST = FIXTURE_ROOT.parent / "terrain_v2" / "manifest.json"


class TerrainRevision4ReferenceTests(unittest.TestCase):
    def test_manifest_pins_identity_samples_and_full_field_hashes(self) -> None:
        manifest = terrain_revision_4_reference.load_reference(FIXTURE_ROOT)

        self.assertEqual(manifest["model_revision"], 4)
        self.assertEqual(manifest["numerical_kernel_identity"], "shelf-exp=exp64-to-f32-v1")
        self.assertEqual(len(manifest["fields"]), 61)
        self.assertEqual(len(manifest["full_atlas_fields"]), 36)
        self.assertEqual(
            manifest["provenance"]["original_python_reference_manifest_sha256"],
            terrain_revision_4_reference.ORIGINAL_V3_MANIFEST_SHA256,
        )
        archive = Path(manifest["approved_candidate_full_atlas_archive"]["retained_path"])
        if archive.is_file():
            self.assertEqual(terrain_revision_4_reference.verify_full_archive(manifest), 36)

    def test_changed_manifest_is_rejected_instead_of_accepting_a_new_baseline(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            copied_fixture = temporary_root / "terrain_v2_revision_4"
            shutil.copytree(FIXTURE_ROOT, copied_fixture)
            original_root = temporary_root / "terrain_v2"
            original_root.mkdir()
            shutil.copyfile(V3_MANIFEST, original_root / "manifest.json")

            manifest_path = copied_fixture / "manifest.json"
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["model_revision"] = 5
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            with self.assertRaisesRegex(
                terrain_revision_4_reference.ReferenceError,
                "manifest SHA-256 mismatch",
            ):
                terrain_revision_4_reference.load_reference(copied_fixture)

    def test_changed_sample_bytes_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            copied_fixture = temporary_root / "terrain_v2_revision_4"
            shutil.copytree(FIXTURE_ROOT, copied_fixture)
            original_root = temporary_root / "terrain_v2"
            original_root.mkdir()
            shutil.copyfile(V3_MANIFEST, original_root / "manifest.json")

            field_path = copied_fixture / "seed-42" / "atlas__elevation.raw"
            field_bytes = bytearray(field_path.read_bytes())
            field_bytes[0] ^= 0x01
            field_path.write_bytes(field_bytes)

            with self.assertRaisesRegex(
                terrain_revision_4_reference.ReferenceError,
                "sampled field SHA-256 mismatch",
            ):
                terrain_revision_4_reference.load_reference(copied_fixture)

    def test_explicit_missing_full_archive_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            candidate_root = temporary_root / "candidate"
            candidate_root.mkdir()
            (candidate_root / "manifest.json").write_text(
                json.dumps({
                    "schema": "mc-world-map/1",
                    "status": "complete",
                    "atlas_size": 256,
                    "seeds": list(terrain_revision_4_reference.SEEDS),
                }),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(
                terrain_revision_4_reference.ReferenceError,
                "approved full-field archive is missing",
            ):
                terrain_revision_4_reference.compare_candidate(
                    FIXTURE_ROOT,
                    candidate_root,
                    temporary_root / "missing-reference.zip",
                )


if __name__ == "__main__":
    unittest.main()
