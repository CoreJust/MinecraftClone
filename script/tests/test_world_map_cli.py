"""Behavior tests for the standalone CPU world-map exporter."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


WORLD_MAP_EXECUTABLE: Path | None = None
SEED_LIST = [0, 1, 42, 777, 2004, 8800555]


def read_ppm(path: Path) -> tuple[list[bytes], bytes]:
    payload = path.read_bytes()
    header = payload.split(b"\n", maxsplit=3)
    if len(header) != 4 or header[0] != b"P6":
        raise AssertionError(f"invalid binary PPM header in {path}")
    return header[1:3], header[3]


class WorldMapCliTests(unittest.TestCase):
    def setUp(self) -> None:
        if WORLD_MAP_EXECUTABLE is None:
            self.skipTest("world-map executable is not configured")

    def run_cli(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        assert WORLD_MAP_EXECUTABLE is not None
        return subprocess.run(
            [str(WORLD_MAP_EXECUTABLE), *arguments],
            capture_output=True,
            check=False,
            text=True,
            timeout=300,
        )

    def test_single_seed_exports_atlas_fields_and_canonical_regional_material(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "world"
            result = self.run_cli(
                "--seed", "42",
                "--atlas-size", "256",
                "--layers", "height,temperature,water,top-material",
                "--region", "0,0,64",
                "--output", str(output),
            )

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["status"], "complete")
            self.assertEqual(manifest["seeds"], [42])
            metadata_path = output / "seed-42" / "metadata.json"
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            self.assertEqual(metadata["configuration"]["seed"], 42)
            self.assertEqual(metadata["configuration"]["resolution"], 256)
            self.assertTrue(metadata["configuration_identity"])
            self.assertEqual(len(metadata["source_identity"]["sha256"]), 64)
            self.assertEqual(metadata["region"]["extent_blocks"], 64)
            self.assertGreaterEqual(metadata["timing_ms"]["simulation"], 0.0)
            self.assertGreaterEqual(metadata["timing_ms"]["queries"], 0.0)
            self.assertGreaterEqual(metadata["timing_ms"]["rasterization"], 0.0)
            self.assertGreaterEqual(metadata["timing_ms"]["serialization"], 0.0)
            self.assertGreater(metadata["memory_bytes"]["peak_resident"], 0)
            self.assertTrue(all(metadata["checks"].values()))

            for layer in ("height", "temperature", "water"):
                ppm = output / "seed-42" / f"{layer}.ppm"
                header, pixels = read_ppm(ppm)
                self.assertEqual(header, [b"256 256", b"255"])
                self.assertEqual(len(pixels), 256 * 256 * 3)
                raw = output / "seed-42" / "fields" / f"{layer}.f32le"
                self.assertEqual(raw.stat().st_size, 256 * 256 * struct.calcsize("<f"))
            water_layer = next(layer for layer in metadata["layers"] if layer["name"] == "water")
            self.assertEqual(water_layer["raw_fields"][0]["unit"], "geological_km")

            material_ppm = output / "seed-42" / "top-material.ppm"
            material_header, material_pixels = read_ppm(material_ppm)
            self.assertEqual(material_header, [b"65 65", b"255"])
            self.assertEqual(len(material_pixels), 65 * 65 * 3)
            material_ids = output / "seed-42" / "fields" / "top-material.u8"
            self.assertEqual(material_ids.stat().st_size, 65 * 65)
            material_values = material_ids.read_bytes()
            self.assertTrue(material_values)
            self.assertTrue(all(1 <= material <= 6 for material in material_values))
            palette = {
                int(block_id): bytes(metadata["material_palette"][block_id]["rgb"])
                for block_id in metadata["material_palette"]
            }
            self.assertEqual(
                material_pixels,
                b"".join(palette[material] for material in material_values),
            )

    def test_seed_2004_height_palette_keeps_fused_boundary_pixels(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "world"
            result = self.run_cli(
                "--seed", "2004",
                "--atlas-size", "256",
                "--layers", "height",
                "--output", str(output),
            )

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            _, pixels = read_ppm(output / "seed-2004" / "height.ppm")
            for x, y, expected in (
                (168, 55, bytes((42, 104, 136))),
                (187, 121, bytes((42, 105, 137))),
            ):
                offset = (y * 256 + x) * 3
                self.assertEqual(pixels[offset:offset + 3], expected, (x, y))

    def test_seed_list_preserves_the_requested_order_and_creates_each_world(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "worlds"
            result = self.run_cli(
                "--seed-list", ",".join(str(seed) for seed in SEED_LIST),
                "--atlas-size", "256",
                "--layers", "height",
                "--output", str(output),
            )

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["seeds"], SEED_LIST)
            for seed in SEED_LIST:
                metadata_path = output / f"seed-{seed}" / "metadata.json"
                metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
                self.assertEqual(metadata["configuration"]["seed"], seed)
                self.assertTrue((output / f"seed-{seed}" / "height.ppm").is_file())

    def test_invalid_size_is_rejected_before_creating_the_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "world"
            result = self.run_cli(
                "--seed", "42",
                "--atlas-size", "999999",
                "--layers", "height",
                "--output", str(output),
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("atlas size", result.stderr.lower())
            self.assertFalse(output.exists())

    def test_existing_output_is_refused_without_changing_its_contents(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "world"
            output.mkdir()
            sentinel = output / "keep.txt"
            sentinel.write_text("keep", encoding="utf-8")
            result = self.run_cli(
                "--seed", "42",
                "--atlas-size", "256",
                "--layers", "height",
                "--output", str(output),
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("exists", result.stderr.lower())
            self.assertEqual(sentinel.read_text(encoding="utf-8"), "keep")
            self.assertEqual(list(output.iterdir()), [sentinel])

    def test_buffered_manifest_write_failure_is_reported(self) -> None:
        if sys.platform == "win32":
            self.skipTest("RLIMIT_FSIZE is unavailable on Windows")
        try:
            import resource
            import signal
        except ImportError:
            self.skipTest("resource limits are unavailable on this platform")
        if not hasattr(signal, "SIGXFSZ"):
            self.skipTest("SIGXFSZ is unavailable on this platform")

        def restrict_file_size() -> None:
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (1, 1))

        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "world"
            result = subprocess.run(
                [
                    str(WORLD_MAP_EXECUTABLE),
                    "--seed", "42",
                    "--atlas-size", "256",
                    "--layers", "height",
                    "--output", str(output),
                ],
                capture_output=True,
                check=False,
                text=True,
                timeout=300,
                preexec_fn=restrict_file_size,
            )

            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("failed while writing output file", result.stderr)


def _parse_test_arguments() -> list[str]:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--executable", type=Path, required=True)
    arguments, remaining = parser.parse_known_args()
    global WORLD_MAP_EXECUTABLE
    WORLD_MAP_EXECUTABLE = arguments.executable.resolve()
    if not WORLD_MAP_EXECUTABLE.is_file():
        parser.error(f"world-map executable does not exist: {WORLD_MAP_EXECUTABLE}")
    return [sys.argv[0], *remaining]


if __name__ == "__main__":
    sys.argv = _parse_test_arguments()
    unittest.main()
