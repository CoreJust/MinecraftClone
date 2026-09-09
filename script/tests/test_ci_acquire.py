"""Contracts for the pinned desktop-CI acquisition helper."""

from __future__ import annotations

import hashlib
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from script.ci import acquire


class CiAcquireTests(unittest.TestCase):
    """Verify immutable pins and the separate shader validation contracts."""

    def test_pinned_acquisitions_have_expected_immutable_values(self):
        self.assertEqual(acquire.CMAKE_VERSION, "3.31.6")
        self.assertEqual(acquire.NINJA_VERSION, "1.13.1")
        self.assertEqual(acquire.PYTHON_VERSION, "3.12.10")
        self.assertEqual(acquire.VCPKG_COMMIT, "2b65c20fc66eda893aa15a15a453c3cf09500b19")
        self.assertEqual(acquire.VULKAN_DOWNLOADS["windows"]["sha256"], "81f474711e9042f4cd22b31b2f7a8870db2e428b21586fb43dd80150be97310d")
        self.assertEqual(acquire.VULKAN_DOWNLOADS["macos"]["sha256"], "539433589c83522e6f31b1c7b418a4167e21597a4a361ab119e1dc0760cf3865")
        self.assertEqual(acquire.NINJA_DOWNLOADS["windows"]["sha256"], "26a40fa8595694dec2fad4911e62d29e10525d2133c9a4230b66397774ae25bf")
        self.assertEqual(acquire.NINJA_DOWNLOADS["macos"]["sha256"], "da7797794153629aca5570ef7c813342d0be214ba84632af886856e8f0063dd9")

    def test_install_ninja_uses_hash_verified_upstream_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "ninja"
            root.mkdir()
            executable = root / "ninja"
            executable.touch()
            with mock.patch.object(acquire, "download") as download, mock.patch.object(acquire, "verify_sha256") as verify, mock.patch.object(acquire, "safe_extract") as extract, mock.patch.object(acquire, "write_github_path") as write_path:
                result = acquire.install_ninja("macos", root)
            config = acquire.NINJA_DOWNLOADS["macos"]
            archive = root.parent / config["filename"]
            download.assert_called_once_with(config["url"], archive)
            verify.assert_called_once_with(archive, config["sha256"])
            extract.assert_called_once_with(archive, root)
            write_path.assert_called_once_with(executable.parent)
            self.assertEqual(result, executable)

    def test_install_macos_vulkan_runs_bundled_installer_after_hash_check(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "vulkan"
            installer = Path(directory) / "vulkansdk-macOS-1.4.357.0"
            installer.touch()
            compiler = root / "installed" / "1.4.357.0" / "macOS" / "bin" / "glslc"
            compiler.parent.mkdir(parents=True)
            compiler.touch()
            sdk_root = compiler.parent.parent
            with mock.patch.object(acquire, "download") as download, mock.patch.object(acquire, "verify_sha256") as verify, mock.patch.object(acquire, "safe_extract") as extract, mock.patch.object(acquire, "find_macos_vulkan_installer", return_value=installer), mock.patch.object(acquire, "find_vulkan_sdk", return_value=(sdk_root, compiler)), mock.patch.object(acquire, "run") as run, mock.patch.object(acquire, "write_github_env"), mock.patch.object(acquire, "write_github_path"):
                result = acquire.install_vulkan("macos", root)
            config = acquire.VULKAN_DOWNLOADS["macos"]
            archive = root.parent / config["filename"]
            download.assert_called_once_with(config["url"], archive)
            verify.assert_called_once_with(archive, config["sha256"])
            extract.assert_called_once_with(archive, root / "installer")
            run.assert_called_once_with([
                str(installer),
                "--root", str(root / "installed"),
                "--accept-licenses",
                "--default-answer",
                "--confirm-command", "install",
                "copy_only=1",
            ])
            self.assertEqual(result, sdk_root)

    def test_verify_sha256_rejects_tampered_download(self):
        with tempfile.TemporaryDirectory() as directory:
            payload = Path(directory) / "payload"
            payload.write_bytes(b"known payload")
            expected = hashlib.sha256(b"different payload").hexdigest()
            with self.assertRaisesRegex(acquire.CiError, "SHA-256 mismatch"):
                acquire.verify_sha256(payload, expected)

    def test_find_vulkan_sdk_requires_exactly_one_compiler(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            compiler = root / "1.4.357.0" / "macOS" / "bin" / "glslc"
            compiler.parent.mkdir(parents=True)
            compiler.touch()
            sdk_root, discovered = acquire.find_vulkan_sdk(root)
            self.assertEqual(sdk_root, compiler.parent.parent)
            self.assertEqual(discovered, compiler)
            duplicate = root / "duplicate" / "bin" / "glslc"
            duplicate.parent.mkdir(parents=True)
            duplicate.touch()
            with self.assertRaisesRegex(acquire.CiError, "found 2"):
                acquire.find_vulkan_sdk(root)

    def test_validate_shaders_uses_separate_vulkan_targets(self):
        with tempfile.TemporaryDirectory() as directory:
            build_dir = Path(directory)
            shader_directory = build_dir / "src" / "client"
            shader_directory.mkdir(parents=True)
            for shader in acquire.MESH_SHADERS + acquire.VULKAN_12_SHADERS:
                (shader_directory / shader).touch()
            with mock.patch.object(acquire, "spirv_validator", return_value="spirv-val"), mock.patch.object(acquire, "run") as run:
                acquire.validate_shaders(build_dir)
            mesh_calls = [
                mock.call(["spirv-val", "--target-env", "vulkan1.3", str(shader_directory / shader)])
                for shader in acquire.MESH_SHADERS
            ]
            fallback_calls = [
                mock.call(["spirv-val", "--target-env", "vulkan1.2", str(shader_directory / shader)])
                for shader in acquire.VULKAN_12_SHADERS
            ]
            self.assertEqual(run.call_args_list, mesh_calls + fallback_calls)

    def test_write_github_environment_is_opt_in(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "environment"
            with mock.patch.dict(os.environ, {"GITHUB_ENV": str(destination)}, clear=False):
                acquire.write_github_env("VULKAN_SDK", "/tmp/sdk")
            self.assertEqual(destination.read_text(encoding="utf-8"), "VULKAN_SDK=/tmp/sdk\n")


if __name__ == "__main__":
    unittest.main()
