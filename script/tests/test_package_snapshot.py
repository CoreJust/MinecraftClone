import contextlib
import hashlib
import importlib.util
import io
import json
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path, PurePosixPath
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "script/package_snapshot.py"


def load_module():
    spec = importlib.util.spec_from_file_location("package_snapshot", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class PackageSnapshotTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name).resolve()
        self.executable = self.write("input/mc_main.exe", b"windows executable")
        self.dll = self.write("input/dependency.dll", b"dependency")
        self.shader_dir = self.root / "input/shaders"
        self.write("input/shaders/grid.vert.spv", b"shader")
        self.license = self.root / "input/licenses"
        self.write("input/licenses/LICENSE-dependency", b"license")
        self.write("input/licenses/hud/DEBUG_HUD_ATTRIBUTION.md", b"HUD attribution")
        self.write("input/licenses/hud/TAMSYN_LICENSE.txt", b"Tamsyn license")
        self.evidence = self.write("input/toolchain.json", b'{"compiler":"fixture","vulkan":"1.3"}\n')
        self.common = [
            "--version", "0.1.0:3",
            "--source-commit", "a" * 40,
            "--toolchain-evidence", str(self.evidence),
        ]

    def tearDown(self):
        self.temp_dir.cleanup()

    def write(self, relative, content):
        target = self.root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
        return target

    def elf(self, abi):
        elf_class, machine, header_size, size_offset = {
            "armeabi-v7a": (1, 40, 52, 40),
            "arm64-v8a": (2, 183, 64, 52),
            "x86": (1, 3, 52, 40),
            "x86_64": (2, 62, 64, 52),
        }[abi]
        header = bytearray(header_size)
        header[:7] = b"\x7fELF" + bytes((elf_class, 1, 1))
        header[18:20] = machine.to_bytes(2, "little")
        header[size_offset:size_offset + 2] = header_size.to_bytes(2, "little")
        return bytes(header)

    def apk(self, relative, entries):
        target = self.root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        entries = {
            "assets/licenses/hud/DEBUG_HUD_ATTRIBUTION.md": b"HUD attribution",
            "assets/licenses/hud/TAMSYN_LICENSE.txt": b"Tamsyn license",
            **entries,
        }
        with zipfile.ZipFile(target, "w") as archive:
            for name, contents in entries.items():
                archive.writestr(name, contents)
        return target

    def call(self, module, arguments):
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = module.main(arguments)
        return result, stdout.getvalue(), stderr.getvalue()

    def windows_args(self, output):
        return [
            "desktop", "--platform", "windows", "--format", "zip",
            "--executable", str(self.executable),
            "--shaders", str(self.shader_dir),
            "--runtime", str(self.dll), "--license", str(self.license),
            *self.common, "--output", str(output),
        ]

    def test_windows_archive_is_deterministic_and_contains_runtime_contract(self):
        module = load_module()
        first = self.root / "first.zip"
        second = self.root / "second.zip"
        first_result, first_stdout, first_stderr = self.call(module, self.windows_args(first))
        second_result, second_stdout, second_stderr = self.call(module, self.windows_args(second))
        self.assertEqual(first_result, 0, first_stderr)
        self.assertEqual(second_result, 0, second_stderr)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertEqual(json.loads(first_stdout)["archive_sha256"], hashlib.sha256(first.read_bytes()).hexdigest())
        self.assertEqual(json.loads(second_stdout)["archive_sha256"], hashlib.sha256(second.read_bytes()).hexdigest())
        with zipfile.ZipFile(first) as archive:
            self.assertEqual(
                archive.namelist(),
                [
                    "LAUNCH.txt",
                    "dependency.dll",
                    "licenses/LICENSE-dependency",
                    "licenses/hud/DEBUG_HUD_ATTRIBUTION.md",
                    "licenses/hud/TAMSYN_LICENSE.txt",
                    "manifest.json",
                    "mc_main.exe",
                    "shaders/grid.vert.spv",
                ],
            )
            self.assertTrue(all(item.date_time == (1980, 1, 1, 0, 0, 0) for item in archive.infolist()))
            manifest = json.loads(archive.read("manifest.json"))
        self.assertEqual(manifest["runtime"], {"launcher": "mc_main.exe", "vulkan_driver": "external-required"})
        self.assertTrue(manifest["hud_attribution_present"])
        self.assertTrue(manifest["hud_font_license_present"])
        self.assertNotIn(str(self.root), json.dumps(manifest))
        self.assertTrue((self.root / "first.zip.manifest.json").is_file())

    def test_windows_rejects_case_collisions_and_invalid_names(self):
        module = load_module()
        colliding = self.write("other/Dependency.DLL", b"other dependency")
        collision_args = self.windows_args(self.root / "collision.zip")
        collision_args[collision_args.index("--license"):collision_args.index("--license")] = [
            "--runtime", str(colliding),
        ]
        collision, _, collision_stderr = self.call(module, collision_args)
        self.assertEqual(collision, 1)
        self.assertIn("case-insensitive Windows archive path collision", collision_stderr)

        invalid_license = self.write("other/NOTICE.", b"notice")
        invalid_args = [
            *self.windows_args(self.root / "invalid.zip"),
            "--license", str(invalid_license),
        ]
        invalid, _, invalid_stderr = self.call(module, invalid_args)
        self.assertEqual(invalid, 1)
        self.assertIn("invalid Windows archive path", invalid_stderr)

        with self.assertRaisesRegex(module.PackageError, "invalid Windows archive path"):
            module.validate_windows_archive_names([
                module.ArchiveEntry("licenses/COM¹.txt", b"reserved", 0o644),
            ])

    def test_desktop_rejects_missing_hud_license_material(self):
        module = load_module()
        generic_license = self.write("input/generic-license.txt", b"license")
        arguments = self.windows_args(self.root / "missing-hud.zip")
        license_index = arguments.index("--license") + 1
        arguments[license_index] = str(generic_license)
        result, _, stderr = self.call(module, arguments)
        self.assertEqual(result, 1)
        self.assertIn("required HUD attribution/license material is missing", stderr)

    def test_desktop_rejects_empty_hud_license_material(self):
        module = load_module()
        (self.license / "hud/TAMSYN_LICENSE.txt").write_bytes(b"")
        result, _, stderr = self.call(module, self.windows_args(self.root / "empty-hud.zip"))
        self.assertEqual(result, 1)
        self.assertIn("required HUD attribution/license material is empty", stderr)
        self.assertFalse((self.root / "empty-hud.zip").exists())

    def test_rejects_symlinked_shader_directory(self):
        module = load_module()
        linked = self.root / "linked-shaders"
        linked.symlink_to(self.shader_dir, target_is_directory=True)
        arguments = self.windows_args(self.root / "bad.zip")
        arguments[arguments.index("--shaders") + 1] = str(linked)
        result, _, stderr = self.call(module, arguments)
        self.assertEqual(result, 1)
        self.assertIn("shader directory must be an existing directory", stderr)

    def test_rejects_non_standard_json_constants(self):
        module = load_module()
        self.evidence.write_bytes(b'{"compiler":NaN}\n')
        result, _, stderr = self.call(
            module,
            self.windows_args(self.root / "non-finite.zip"),
        )
        self.assertEqual(result, 1)
        self.assertIn("non-standard JSON constant: NaN", stderr)

        icd = self.write(
            "mac/non-finite.json",
            b'{"ICD":{"library_path":"relative","api_version":Infinity}}',
        )
        with self.assertRaisesRegex(module.PackageError, "non-standard JSON constant: Infinity"):
            module.sanitized_icd(icd)

    def test_archive_uses_one_snapshot_for_manifest_and_payload(self):
        module = load_module()
        arguments = module.parser().parse_args(self.windows_args(self.root / "unused.zip"))
        entries, manifest = module.desktop_entries(arguments, {"compiler": "fixture"})
        self.dll.write_bytes(b"changed after snapshot")
        output = io.BytesIO()
        module.write_zip(output, entries)
        with zipfile.ZipFile(io.BytesIO(output.getvalue())) as archive:
            payload = archive.read("dependency.dll")
            archived_manifest = json.loads(archive.read("manifest.json"))
        recorded = next(item for item in manifest["files"] if item["path"] == "dependency.dll")
        self.assertEqual(payload, b"dependency")
        self.assertEqual(recorded["sha256"], hashlib.sha256(payload).hexdigest())
        self.assertEqual(archived_manifest, manifest)

    def test_macos_bundle_uses_relative_icd_and_self_locating_launcher(self):
        module = load_module()
        executable = self.write("mac/mc_main", b"mac executable")
        loader = self.write("mac/libvulkan.1.dylib", b"loader")
        moltenvk = self.write("mac/libMoltenVK.dylib", b"moltenvk")
        runtime = self.write("mac/libsupport.dylib", b"support")
        icd = self.write(
            "mac/MoltenVK_icd.json",
            b'{"file_format_version":"1.0.0","ICD":{"library_path":"/SDK/MoltenVK.dylib","api_version":"1.3.0"}}',
        )
        output = self.root / "mac.tar.gz"
        arguments = [
            "desktop", "--platform", "macos", "--format", "tar.gz",
            "--executable", str(executable), "--shaders", str(self.shader_dir),
            "--runtime", str(runtime), "--license", str(self.license),
            "--vulkan-loader", str(loader), "--moltenvk", str(moltenvk), "--icd-json", str(icd),
            *self.common, "--output", str(output),
        ]
        snapshots = [
            (executable.resolve(), PurePosixPath("mc_main"), executable.read_bytes()),
            (loader.resolve(), PurePosixPath("lib/libvulkan.1.dylib"), loader.read_bytes()),
            (moltenvk.resolve(), PurePosixPath("lib/libMoltenVK.dylib"), moltenvk.read_bytes()),
            (runtime.resolve(), PurePosixPath("lib/libsupport.dylib"), runtime.read_bytes()),
        ]
        with mock.patch.object(
            module,
            "validate_macos_package",
            return_value=snapshots,
        ) as validate:
            result, _, stderr = self.call(module, arguments)
        self.assertEqual(result, 0, stderr)
        validate.assert_called_once_with([
            (executable.resolve(), PurePosixPath("mc_main")),
            (loader.resolve(), PurePosixPath("lib/libvulkan.1.dylib")),
            (moltenvk.resolve(), PurePosixPath("lib/libMoltenVK.dylib")),
            (runtime.resolve(), PurePosixPath("lib/libsupport.dylib")),
        ])
        with tarfile.open(output, "r:gz") as archive:
            launcher = archive.extractfile("MinecraftClone.command").read().decode("utf-8")
            manifest = json.loads(archive.extractfile("manifest.json").read())
            packaged_icd = json.loads(archive.extractfile("vulkan/icd.d/MoltenVK_icd.json").read())
            self.assertIn("lib/libvulkan.1.dylib", archive.getnames())
            self.assertIn("licenses/hud/TAMSYN_LICENSE.txt", archive.getnames())
            self.assertIn("licenses/hud/DEBUG_HUD_ATTRIBUTION.md", archive.getnames())
        self.assertIn('bundle_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)', launcher)
        self.assertIn('VK_DRIVER_FILES="$bundle_root/vulkan/icd.d/MoltenVK_icd.json"', launcher)
        self.assertEqual(packaged_icd["ICD"]["library_path"], "../../lib/libMoltenVK.dylib")
        self.assertEqual(manifest["runtime"]["vulkan_loader"], "lib/libvulkan.1.dylib")
        self.assertEqual(manifest["runtime"]["moltenvk"], "lib/libMoltenVK.dylib")
        self.assertTrue(manifest["hud_font_license_present"])
        self.assertTrue(manifest["hud_attribution_present"])

    def test_macos_allows_universal_runtime_but_rejects_missing_executable_architecture(self):
        module = load_module()
        executable = self.write("mac/mc_main", b"app")
        universal = self.write("mac/libuniversal.dylib", b"universal")
        safe_dependencies = ["@rpath/libfixture.dylib"]
        with mock.patch.object(
            module,
            "macos_architectures",
            side_effect=[{"arm64"}, {"arm64"}, {"arm64", "x86_64"}],
        ), mock.patch.object(
            module,
            "macos_install_names",
            side_effect=[set(), {"@rpath/libfixture.dylib"}],
        ), mock.patch.object(module, "macos_dependencies", return_value=safe_dependencies):
            module.validate_macos_package([
                (executable, PurePosixPath("mc_main")),
                (universal, PurePosixPath("lib/libfixture.dylib")),
            ])
        with mock.patch.object(
            module,
            "macos_architectures",
            side_effect=[{"arm64"}, {"arm64"}, {"x86_64"}],
        ), mock.patch.object(
            module,
            "macos_install_names",
            side_effect=[set(), {"@rpath/libfixture.dylib"}],
        ), mock.patch.object(module, "macos_dependencies", return_value=safe_dependencies):
            with self.assertRaisesRegex(module.PackageError, "missing executable architecture arm64"):
                module.validate_macos_package([
                    (executable, PurePosixPath("mc_main")),
                    (universal, PurePosixPath("lib/libfixture.dylib")),
                ])

    def test_macos_rejects_unpackaged_relative_dependency(self):
        module = load_module()
        executable = self.write("mac/mc_main", b"app")
        with mock.patch.object(module, "macos_architectures", return_value={"arm64"}), mock.patch.object(
            module,
            "macos_install_names",
            return_value=set(),
        ), mock.patch.object(
            module,
            "macos_dependencies",
            return_value=["@rpath/libmissing.dylib"],
        ):
            with self.assertRaisesRegex(module.PackageError, "does not resolve"):
                module.validate_macos_package([
                    (executable, PurePosixPath("mc_main")),
                    (self.write("mac/libpresent.dylib", b"present"), PurePosixPath("lib/libpresent.dylib")),
                ])

    def test_macos_rejects_relative_dependency_outside_packaged_layout(self):
        module = load_module()
        executable = self.write("mac/mc_main", b"app")
        runtime = self.write("mac/libfoo.dylib", b"runtime")
        with mock.patch.object(module, "macos_architectures", return_value={"arm64"}), mock.patch.object(
            module,
            "macos_install_names",
            return_value=set(),
        ), mock.patch.object(
            module,
            "macos_dependencies",
            return_value=["@loader_path/../Frameworks/libfoo.dylib"],
        ):
            with self.assertRaisesRegex(module.PackageError, "does not resolve"):
                module.validate_macos_package([
                    (executable, PurePosixPath("mc_main")),
                    (runtime, PurePosixPath("lib/libfoo.dylib")),
                ])

    def test_macos_rejects_runtime_collision_with_fixed_loader_alias(self):
        module = load_module()
        executable = self.write("mac/mc_main", b"mac executable")
        loader = self.write("mac/libvulkan.1.4.357.dylib", b"loader")
        moltenvk = self.write("mac/libMoltenVK.dylib", b"moltenvk")
        collision = self.write("mac/libvulkan.1.dylib", b"different loader")
        icd = self.write("mac/MoltenVK_icd.json", b'{"ICD":{"library_path":"/SDK/MoltenVK.dylib"}}')
        arguments = [
            "desktop", "--platform", "macos", "--format", "tar.gz",
            "--executable", str(executable), "--shaders", str(self.shader_dir),
            "--runtime", str(collision), "--license", str(self.license),
            "--vulkan-loader", str(loader), "--moltenvk", str(moltenvk), "--icd-json", str(icd),
            *self.common, "--output", str(self.root / "collision.tar.gz"),
        ]
        snapshots = [
            (executable.resolve(), PurePosixPath("mc_main"), executable.read_bytes()),
            (loader.resolve(), PurePosixPath("lib/libvulkan.1.dylib"), loader.read_bytes()),
            (moltenvk.resolve(), PurePosixPath("lib/libMoltenVK.dylib"), moltenvk.read_bytes()),
            (collision.resolve(), PurePosixPath("lib/libvulkan.1.dylib"), collision.read_bytes()),
        ]
        with mock.patch.object(module, "validate_macos_package", return_value=snapshots):
            result, _, stderr = self.call(module, arguments)
        self.assertEqual(result, 1)
        self.assertIn("duplicate archive paths: lib/libvulkan.1.dylib", stderr)

    def test_macos_inspector_rejects_a_developer_machine_dependency(self):
        module = load_module()
        binary = self.write("mac/checked.dylib", b"fixture")

        def inspect(command, **_):
            inspected = command[-1]
            if command[0] == "lipo":
                return subprocess.CompletedProcess(command, 0, f"{inspected} are: arm64\n", "")
            if command[1] == "-D":
                return subprocess.CompletedProcess(
                    command,
                    0,
                    f"{inspected}:\n@rpath/checked.dylib\n",
                    "",
                )
            return subprocess.CompletedProcess(
                command,
                0,
                f"{inspected}:\n"
                "\t@rpath/checked.dylib (compatibility version 1.0.0)\n"
                "\t/Users/example/vcpkg/lib/libfixture.dylib (compatibility version 1.0.0)\n",
                "",
            )

        with mock.patch.object(module.subprocess, "run", side_effect=inspect):
            with self.assertRaisesRegex(module.PackageError, "non-relocatable dependency"):
                module.validate_macos_package([(binary, PurePosixPath("lib/checked.dylib"))])

    def test_macos_otool_parsers_skip_fat_binary_headers_and_separate_install_name(self):
        module = load_module()
        binary = self.write("mac/libfixture.dylib", b"fixture")

        def inspect(command, **_):
            option = command[1]
            values = {
                "-D": "@rpath/libfixture.dylib",
                "-L": "@rpath/libfixture.dylib (compatibility version 1.0.0)",
            }
            output = (
                f"{binary} (architecture x86_64):\n"
                f"\t{values[option]}\n"
                f"{binary} (architecture arm64):\n"
                f"\t{values[option]}\n"
            )
            return subprocess.CompletedProcess(command, 0, output, "")

        with mock.patch.object(module.subprocess, "run", side_effect=inspect):
            self.assertEqual(
                module.macos_dependencies(binary),
                ["@rpath/libfixture.dylib", "@rpath/libfixture.dylib"],
            )
            self.assertEqual(
                module.macos_install_names(binary),
                frozenset({"@rpath/libfixture.dylib"}),
            )

    def test_macos_rejects_dylib_install_name_outside_packaged_location(self):
        module = load_module()
        executable = self.write("mac/mc_main", b"app")
        runtime = self.write("mac/libfixture.dylib", b"runtime")
        with mock.patch.object(
            module,
            "macos_architectures",
            side_effect=[{"arm64"}, {"arm64"}, {"arm64"}],
        ), mock.patch.object(
            module,
            "macos_install_names",
            side_effect=[set(), {"/Users/example/libfixture.dylib"}],
        ), mock.patch.object(
            module,
            "macos_dependencies",
            return_value=["/usr/lib/libSystem.B.dylib"],
        ):
            with self.assertRaisesRegex(module.PackageError, "install name does not"):
                module.validate_macos_package([
                    (executable, PurePosixPath("mc_main")),
                    (runtime, PurePosixPath("lib/libfixture.dylib")),
                ])

    def test_macos_architecture_parser_accepts_lipo_plain_single_and_multi_arch_output(self):
        module = load_module()
        binary = self.write("mac/checked.dylib", b"fixture")
        outputs = iter(("arm64\n", "arm64 x86_64\n"))
        with mock.patch.object(
            module.subprocess,
            "run",
            side_effect=lambda command, **_: subprocess.CompletedProcess(command, 0, next(outputs), ""),
        ):
            self.assertEqual(module.macos_architectures(binary), frozenset({"arm64"}))
            self.assertEqual(module.macos_architectures(binary), frozenset({"arm64", "x86_64"}))

    def test_macos_architecture_parser_rejects_malformed_or_unexpected_output(self):
        module = load_module()
        binary = self.write("mac/checked.dylib", b"fixture")
        for output in ("", "arm64 arm64\n", "arm64 i386\n", "arm64, x86_64\n"):
            with self.subTest(output=output), mock.patch.object(
                module.subprocess,
                "run",
                return_value=subprocess.CompletedProcess(["lipo"], 0, output, ""),
            ):
                with self.assertRaisesRegex(module.PackageError, "unexpected|unsupported"):
                    module.macos_architectures(binary)

    def test_output_temporary_and_sidecar_symlinks_are_not_followed(self):
        module = load_module()
        victim = self.write("victim.txt", b"preserve me")
        output = self.root / "safe.zip"
        output.with_name(output.name + ".tmp").symlink_to(victim)
        result, _, stderr = self.call(module, self.windows_args(output))
        self.assertEqual(result, 0, stderr)
        self.assertEqual(victim.read_bytes(), b"preserve me")

        blocked = self.root / "blocked.zip"
        blocked.with_name(blocked.name + ".manifest.json").symlink_to(victim)
        arguments = [*self.windows_args(blocked), "--overwrite"]
        rejected, _, rejected_stderr = self.call(module, arguments)
        self.assertEqual(rejected, 1)
        self.assertIn("symlink", rejected_stderr)
        self.assertEqual(victim.read_bytes(), b"preserve me")

    def test_output_parent_symlink_is_rejected(self):
        module = load_module()
        real_parent = self.root / "real-output"
        real_parent.mkdir()
        linked_parent = self.root / "linked-output"
        linked_parent.symlink_to(real_parent, target_is_directory=True)
        result, _, stderr = self.call(
            module,
            self.windows_args(linked_parent / "package.zip"),
        )
        self.assertEqual(result, 1)
        self.assertIn("parent ancestry must not contain a symlink", stderr)
        self.assertFalse((real_parent / "package.zip").exists())

    def test_output_nested_parent_symlink_is_rejected(self):
        module = load_module()
        real_parent = self.root / "real-output"
        nested = real_parent / "nested"
        nested.mkdir(parents=True)
        linked_parent = self.root / "linked-output"
        linked_parent.symlink_to(real_parent, target_is_directory=True)
        result, _, stderr = self.call(
            module,
            self.windows_args(linked_parent / "nested" / "package.zip"),
        )
        self.assertEqual(result, 1)
        self.assertIn("parent ancestry must not contain a symlink", stderr)
        self.assertFalse((nested / "package.zip").exists())

    def test_android_evidence_does_not_modify_apk_or_claim_signing_identity(self):
        module = load_module()
        apk = self.apk("android/game.apk", {
            "lib/arm64-v8a/libmc_android.so": self.elf("arm64-v8a"),
            "lib/arm64-v8a/libc++_shared.so": self.elf("arm64-v8a"),
            "assets/shaders/grid.vert.spv": b"shader",
        })
        before = apk.read_bytes()
        output = self.root / "android-evidence.json"
        arguments = [
            "android", "--apk", str(apk), "--api-level", "35", "--abi", "arm64-v8a",
            "--signing", "development", "--source-exactness", "exact",
            *self.common, "--output", str(output),
        ]
        result, stdout, stderr = self.call(module, arguments)
        self.assertEqual(result, 0, stderr)
        self.assertEqual(apk.read_bytes(), before)
        evidence = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(evidence["apk"]["sha256"], hashlib.sha256(before).hexdigest())
        self.assertEqual(evidence["signing"], {"classification": "development", "identity": "not asserted"})
        self.assertEqual(evidence["source_exactness"], "exact")
        self.assertEqual(evidence["abis"], ["arm64-v8a"])
        self.assertEqual(
            [item["path"] for item in evidence["licenses"]],
            [
                "assets/licenses/hud/DEBUG_HUD_ATTRIBUTION.md",
                "assets/licenses/hud/TAMSYN_LICENSE.txt",
            ],
        )
        self.assertTrue(evidence["hud_font_license_present"])
        self.assertEqual(json.loads(stdout)["evidence_sha256"], hashlib.sha256(output.read_bytes()).hexdigest())

    def test_android_evidence_rejects_declared_abi_mismatch(self):
        module = load_module()
        apk = self.apk("android/x86.apk", {
            "lib/x86_64/libmc_android.so": self.elf("x86_64"),
        })
        output = self.root / "android-evidence.json"
        arguments = [
            "android", "--apk", str(apk), "--api-level", "35", "--abi", "arm64-v8a",
            "--signing", "development", "--source-exactness", "exact",
            *self.common, "--output", str(output),
        ]
        result, _, stderr = self.call(module, arguments)
        self.assertEqual(result, 1)
        self.assertIn("native ABI set does not match", stderr)
        self.assertFalse(output.exists())

    def test_android_evidence_rejects_missing_hud_license_asset(self):
        module = load_module()
        apk = self.root / "android/missing-license.apk"
        apk.parent.mkdir(parents=True)
        with zipfile.ZipFile(apk, "w") as archive:
            archive.writestr("lib/arm64-v8a/libmc_android.so", self.elf("arm64-v8a"))
        output = self.root / "android-missing-license-evidence.json"
        arguments = [
            "android", "--apk", str(apk), "--api-level", "35", "--abi", "arm64-v8a",
            "--signing", "development", "--source-exactness", "exact",
            *self.common, "--output", str(output),
        ]
        result, _, stderr = self.call(module, arguments)
        self.assertEqual(result, 1)
        self.assertIn("missing required HUD attribution/license asset", stderr)
        self.assertFalse(output.exists())

    def test_android_evidence_rejects_directory_and_elf_architecture_mismatch(self):
        module = load_module()
        apk = self.apk("android/mislabeled.apk", {
            "lib/arm64-v8a/libmc_android.so": self.elf("x86_64"),
        })
        output = self.root / "android-evidence.json"
        arguments = [
            "android", "--apk", str(apk), "--api-level", "35", "--abi", "arm64-v8a",
            "--signing", "development", "--source-exactness", "exact",
            *self.common, "--output", str(output),
        ]
        result, _, stderr = self.call(module, arguments)
        self.assertEqual(result, 1)
        self.assertIn("does not match directory ABI", stderr)
        self.assertFalse(output.exists())

    def test_android_evidence_rejects_malformed_or_missing_native_payload(self):
        module = load_module()
        cases = {
            "malformed": (
                {"lib/arm64-v8a/libmc_android.so": b"not an ELF"},
                "malformed ELF header",
            ),
            "missing": (
                {"lib/arm64-v8a/libc++_shared.so": self.elf("arm64-v8a")},
                "missing lib/arm64-v8a/libmc_android.so",
            ),
        }
        for name, (entries, expected_error) in cases.items():
            with self.subTest(name=name):
                apk = self.apk(f"android/{name}.apk", entries)
                output = self.root / f"{name}-evidence.json"
                arguments = [
                    "android", "--apk", str(apk), "--api-level", "35", "--abi", "arm64-v8a",
                    "--signing", "development", "--source-exactness", "exact",
                    *self.common, "--output", str(output),
                ]
                result, _, stderr = self.call(module, arguments)
                self.assertEqual(result, 1)
                self.assertIn(expected_error, stderr)
                self.assertFalse(output.exists())

    def test_android_evidence_rejects_undeclared_second_abi(self):
        module = load_module()
        apk = self.apk("android/multi.apk", {
            "lib/arm64-v8a/libmc_android.so": self.elf("arm64-v8a"),
            "lib/x86_64/libmc_android.so": self.elf("x86_64"),
        })
        output = self.root / "android-evidence.json"
        arguments = [
            "android", "--apk", str(apk), "--api-level", "35", "--abi", "arm64-v8a",
            "--signing", "development", "--source-exactness", "exact",
            *self.common, "--output", str(output),
        ]
        result, _, stderr = self.call(module, arguments)
        self.assertEqual(result, 1)
        self.assertIn("native ABI set does not match", stderr)
        self.assertFalse(output.exists())

    def test_android_evidence_rejects_normalized_duplicate_native_path(self):
        module = load_module()
        apk = self.root / "android/duplicate.apk"
        apk.parent.mkdir(parents=True)
        with zipfile.ZipFile(apk, "w") as archive:
            archive.writestr("assets/licenses/hud/DEBUG_HUD_ATTRIBUTION.md", b"HUD attribution")
            archive.writestr("assets/licenses/hud/TAMSYN_LICENSE.txt", b"Tamsyn license")
            archive.writestr("lib/arm64-v8a/libmc_android.so", self.elf("arm64-v8a"))
            archive.writestr("lib//arm64-v8a/libmc_android.so", self.elf("arm64-v8a"))
        output = self.root / "android-evidence.json"
        arguments = [
            "android", "--apk", str(apk), "--api-level", "35", "--abi", "arm64-v8a",
            "--signing", "development", "--source-exactness", "exact",
            *self.common, "--output", str(output),
        ]
        result, _, stderr = self.call(module, arguments)
        self.assertEqual(result, 1)
        self.assertIn("invalid or duplicate native library path", stderr)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
