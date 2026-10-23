"""Portable bundle-format and runtime-closure contracts; no Windows/GPU required."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build_windows_singleplayer as builder


class SinglePlayerBundleTest(unittest.TestCase):
    def fixture(self, root):
        runtime = root / "runtime"
        (runtime / "shaders").mkdir(parents=True)
        (runtime / "mc_main.exe").write_bytes(b"MZ-reviewed-game")
        (runtime / "runtime.dll").write_bytes(b"MZ-reviewed-runtime")
        for name in (
            "grid.mesh", "player.mesh", "grid.vert", "player.vert", "player.frag", "stone.vert",
            "stone_indexed_block.vert", "stone.frag", "stone_solid.frag", "sky.vert", "sky.frag",
            "trivial.frag", "text.vert", "text.frag", "map.vert", "map.frag",
        ):
            (runtime / "shaders" / f"{name}.spv").write_bytes(struct.pack("<I", 0x07230203))
        assets = root / "assets"
        assets.mkdir()
        (assets / "texture.png").write_bytes(b"texture")
        scenarios = root / "scenarios"
        scenarios.mkdir()
        (scenarios / "game.core").write_text("reviewed scenario\n")
        return runtime, assets, scenarios

    def prepare(self, inputs, output):
        game_hash = hashlib.sha256((inputs[0] / "mc_main.exe").read_bytes()).hexdigest()
        arguments = [
            "build_windows_singleplayer.py", "--runtime", str(inputs[0]),
            "--assets", str(inputs[1]), "--scenarios", str(inputs[2]), "--output", str(output),
            "--max-extra-bytes", "10000000", "--expected-game-sha256", game_hash,
            "--source-identity", "portable-reviewed-candidate", "--prepare-only",
        ]
        with (
            mock.patch.object(sys, "argv", arguments),
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            return builder.main()

    def test_bundle_is_deterministic_and_retains_exact_paths_bytes_and_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            entries = builder.collect_entries(*self.fixture(root))
            first = builder.write_bundle(entries, root / "first.bin")
            second = builder.write_bundle(list(reversed(entries)), root / "second.bin")
            data = (root / "first.bin").read_bytes()
            self.assertEqual(data, (root / "second.bin").read_bytes())
            self.assertEqual(first, second)
            self.assertEqual(hashlib.sha256(data).hexdigest(), first["bundle_sha256"])
            self.assertEqual(data[:8], builder.MAGIC)
            schema, count = struct.unpack_from("<II", data, 8)
            self.assertEqual(schema, 1)
            self.assertEqual(count, len(entries))
            offset = 16
            decoded = {}
            for _ in range(count):
                length, size = struct.unpack_from("<IQ", data, offset)
                digest = data[offset + 12:offset + 44]
                offset += 44
                name = data[offset:offset + length].decode("utf-8")
                offset += length
                contents = data[offset:offset + size]
                offset += size
                self.assertEqual(hashlib.sha256(contents).digest(), digest)
                decoded[name] = contents
            self.assertEqual(offset, len(data))
            self.assertEqual(decoded, {entry.name: entry.data for entry in entries})
            self.assertIn("assets/texture.png", decoded)
            self.assertIn("scenarios/game.core", decoded)
            self.assertEqual(first["maximum_build_extra_bytes"], len(data) * 3 + 4_194_304)

    def test_missing_game_or_runtime_or_shader_cannot_be_silently_packaged(self):
        for removed in ["mc_main.exe", "runtime.dll", "shaders/map.vert.spv"]:
            with self.subTest(removed=removed), tempfile.TemporaryDirectory() as directory:
                inputs = self.fixture(Path(directory))
                (inputs[0] / removed).unlink()
                with self.assertRaises(ValueError):
                    builder.collect_entries(*inputs)

    def test_renamed_required_shader_is_rejected_with_sixteen_valid_magic_files(self):
        with tempfile.TemporaryDirectory() as directory:
            inputs = self.fixture(Path(directory))
            shader = inputs[0] / "shaders/map.vert.spv"
            shader.rename(shader.with_name("unexpected.vert.spv"))
            with self.assertRaises(builder.packages.PackageError):
                builder.collect_entries(*inputs)

    def test_required_shader_in_a_nested_directory_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            inputs = self.fixture(Path(directory))
            shader = inputs[0] / "shaders/map.vert.spv"
            nested = shader.parent / "nested"
            nested.mkdir()
            shader.rename(nested / shader.name)
            with self.assertRaises(builder.packages.PackageError):
                builder.collect_entries(*inputs)

    def test_invalid_shader_magic_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            inputs = self.fixture(Path(directory))
            (inputs[0] / "shaders/map.vert.spv").write_bytes(b"not SPIR-V")
            with self.assertRaises(builder.packages.PackageError):
                builder.collect_entries(*inputs)

    def test_prepare_creates_only_a_complete_manifest_in_a_new_output_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            inputs = self.fixture(root)
            output = root / "new-output" / "MinecraftClone.exe"
            self.assertEqual(self.prepare(inputs, output), 0)
            sidecar = output.with_suffix(".manifest.json")
            manifest = json.loads(sidecar.read_text())
            self.assertTrue(manifest["prepared_only"])
            self.assertEqual(manifest["source_identity"], "portable-reviewed-candidate")
            self.assertEqual([item.name for item in output.parent.iterdir()], [sidecar.name])

    def test_existing_sidecar_is_preserved_before_creating_any_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            inputs = self.fixture(root)
            output = root / "MinecraftClone.exe"
            sidecar = output.with_suffix(".manifest.json")
            retained = b"previous package evidence\n"
            sidecar.write_bytes(retained)
            before = sidecar.stat()
            before_names = set(root.iterdir())
            self.assertEqual(self.prepare(inputs, output), 1)
            self.assertEqual(sidecar.read_bytes(), retained)
            self.assertEqual(sidecar.stat().st_ino, before.st_ino)
            self.assertEqual(sidecar.stat().st_mtime_ns, before.st_mtime_ns)
            self.assertEqual(set(root.iterdir()), before_names)

    def test_sidecar_symlinks_preserve_existing_and_absent_targets(self):
        for existing in (True, False):
            with self.subTest(existing=existing), tempfile.TemporaryDirectory() as directory:
                root = Path(directory).resolve()
                inputs = self.fixture(root)
                output = root / "MinecraftClone.exe"
                sidecar = output.with_suffix(".manifest.json")
                target = root / "retained-evidence.json"
                if existing:
                    target.write_bytes(b"retained evidence\n")
                sidecar.symlink_to(target)
                before_names = set(root.iterdir())
                self.assertEqual(self.prepare(inputs, output), 1)
                self.assertTrue(sidecar.is_symlink())
                self.assertEqual(set(root.iterdir()), before_names)
                if existing:
                    self.assertEqual(target.read_bytes(), b"retained evidence\n")
                else:
                    self.assertFalse(target.exists())

    def test_executable_symlinks_preserve_existing_and_absent_targets(self):
        for existing in (True, False):
            with self.subTest(existing=existing), tempfile.TemporaryDirectory() as directory:
                root = Path(directory).resolve()
                inputs = self.fixture(root)
                output = root / "MinecraftClone.exe"
                target = root / "retained-executable.exe"
                if existing:
                    target.write_bytes(b"retained executable\n")
                output.symlink_to(target)
                before_names = set(root.iterdir())
                self.assertEqual(self.prepare(inputs, output), 1)
                self.assertTrue(output.is_symlink())
                self.assertEqual(set(root.iterdir()), before_names)
                if existing:
                    self.assertEqual(target.read_bytes(), b"retained executable\n")
                else:
                    self.assertFalse(target.exists())

    def test_existing_bundle_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            output = root / "retained.bin"
            output.write_bytes(b"retained bundle\n")
            entries = [builder.packages.ArchiveEntry("mc_main.exe", b"game", 0o644)]
            with self.assertRaises(builder.packages.PackageError):
                builder.write_bundle(entries, output)
            self.assertEqual(output.read_bytes(), b"retained bundle\n")

    def test_failed_bundle_write_does_not_publish_partial_bytes(self):
        real_open = io.open

        @contextlib.contextmanager
        def failing_open(*arguments, **keywords):
            with real_open(*arguments, **keywords) as stream:
                proxy = mock.Mock(wraps=stream)

                def fail_write(data):
                    stream.write(data[:4])
                    raise OSError("disk write failed")

                proxy.write.side_effect = fail_write
                yield proxy

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            output = root / "bundle.bin"
            entries = [builder.packages.ArchiveEntry("mc_main.exe", b"game", 0o644)]
            with mock.patch.object(io, "open", side_effect=failing_open):
                with self.assertRaises(OSError):
                    builder.write_bundle(entries, output)
            self.assertEqual(list(root.iterdir()), [])

    def test_bundle_symlink_preserves_its_target(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            output = root / "bundle.bin"
            target = root / "retained.bin"
            target.write_bytes(b"retained bundle\n")
            output.symlink_to(target)
            entries = [builder.packages.ArchiveEntry("mc_main.exe", b"game", 0o644)]
            with self.assertRaises(builder.packages.PackageError):
                builder.write_bundle(entries, output)
            self.assertTrue(output.is_symlink())
            self.assertEqual(target.read_bytes(), b"retained bundle\n")

    def test_missing_assets_and_scenarios_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            inputs = self.fixture(Path(directory))
            for index in [1, 2]:
                changed = list(inputs)
                changed[index] = Path(directory) / "missing"
                with self.assertRaises(ValueError):
                    builder.collect_entries(*changed)

    def test_unsafe_windows_paths_case_collisions_and_links_are_rejected(self):
        for names in [["../escape"], ["CON.txt"], ["file", "FILE"], ["name."]]:
            with self.subTest(names=names), tempfile.TemporaryDirectory() as directory:
                entries = [builder.packages.ArchiveEntry(name, b"data", 0o644) for name in names]
                with self.assertRaises(ValueError):
                    builder.write_bundle(entries, Path(directory) / "bad.bin")
        with tempfile.TemporaryDirectory() as directory:
            inputs = self.fixture(Path(directory))
            (inputs[1] / "link").symlink_to(inputs[0] / "mc_main.exe")
            with self.assertRaises(ValueError):
                builder.collect_entries(*inputs)

    def test_console_or_wrong_architecture_launcher_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "launcher.exe"
            data = bytearray(224)
            data[:2] = b"MZ"
            struct.pack_into("<I", data, 60, 128)
            data[128:132] = b"PE\0\0"
            struct.pack_into("<H", data, 132, 0x8664)
            struct.pack_into("<H", data, 152, 0x20b)
            struct.pack_into("<H", data, 220, 2)
            executable.write_bytes(data)
            builder.verify_gui_executable(executable)
            for offset, value in [(220, 3), (132, 0x14c)]:
                changed = bytearray(data)
                struct.pack_into("<H", changed, offset, value)
                executable.write_bytes(changed)
                with self.assertRaises(ValueError):
                    builder.verify_gui_executable(executable)


if __name__ == "__main__":
    unittest.main()
