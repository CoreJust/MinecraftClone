"""Contracts for the pinned desktop-CI acquisition helper."""

from __future__ import annotations

import hashlib
import io
import os
import stat
import tempfile
import unittest
import zipfile
import json
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
        self.assertEqual(acquire.VULKAN_VERSION, "1.4.357.0")
        self.assertEqual(acquire.VULKAN_DOWNLOADS["windows"]["url"], "https://sdk.lunarg.com/sdk/download/1.4.357.0/windows/vulkan_sdk.exe")
        self.assertEqual(acquire.VULKAN_DOWNLOADS["macos"]["url"], "https://sdk.lunarg.com/sdk/download/1.4.357.0/mac/vulkan_sdk.zip")
        self.assertEqual(acquire.VULKAN_DOWNLOADS["windows"]["sha256"], "81f474711e9042f4cd22b31b2f7a8870db2e428b21586fb43dd80150be97310d")
        self.assertEqual(acquire.VULKAN_DOWNLOADS["macos"]["sha256"], "539433589c83522e6f31b1c7b418a4167e21597a4a361ab119e1dc0760cf3865")
        self.assertEqual(acquire.ANDROID_COMMAND_LINE_TOOLS["url"], "https://dl.google.com/android/repository/commandlinetools-linux-15859902_latest.zip")
        self.assertEqual(acquire.ANDROID_COMMAND_LINE_TOOLS["sha256"], "4e4c464f145a7512b57d088ac6c278c03c9eea610886b35a5e0804e74eedf583")
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
            download.assert_called_once_with(f"{config['url']}?Human=true", archive)
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

    def test_download_uses_explicit_agent_request(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "payload"
            response = mock.MagicMock()
            response.__enter__.return_value = io.BytesIO(b"payload")
            with mock.patch.object(acquire.urllib.request, "urlopen", return_value=response) as urlopen:
                acquire.download("https://sdk.lunarg.com/example?Human=true", destination)
            request = urlopen.call_args.args[0]
            self.assertEqual(request.full_url, "https://sdk.lunarg.com/example?Human=true")
            self.assertEqual(request.get_header("User-agent"), acquire.DOWNLOAD_USER_AGENT)
            self.assertEqual(destination.read_bytes(), b"payload")

    def test_install_android_sdk_bootstraps_latest_layout_before_sdkmanager(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "android-sdk"
            archive = Path(directory) / acquire.ANDROID_COMMAND_LINE_TOOLS["filename"]
            with zipfile.ZipFile(archive, "w") as contents:
                contents.writestr("cmdline-tools/bin/sdkmanager", "#!/bin/sh\n")
                contents.writestr("cmdline-tools/source.properties", "Pkg.Revision=20.0\n")
            with mock.patch.object(acquire, "download") as download, mock.patch.object(acquire, "verify_sha256") as verify, mock.patch.object(acquire, "run") as run, mock.patch.object(acquire, "write_github_env") as write_env:
                def copy_archive(_url: str, destination: Path) -> None:
                    destination.write_bytes(archive.read_bytes())

                download.side_effect = copy_archive
                sdkmanager = acquire.install_android_sdk(root)
            expected_archive = root.parent / acquire.ANDROID_COMMAND_LINE_TOOLS["filename"]
            self.assertEqual(sdkmanager, root / "cmdline-tools" / "latest" / "bin" / "sdkmanager")
            self.assertTrue(sdkmanager.is_file())
            download.assert_called_once_with(acquire.ANDROID_COMMAND_LINE_TOOLS["url"], expected_archive)
            verify.assert_called_once_with(expected_archive, acquire.ANDROID_COMMAND_LINE_TOOLS["sha256"])
            command = [str(sdkmanager), f"--sdk_root={root}"]
            self.assertEqual(run.call_args_list, [
                mock.call([*command, "--licenses"], input_text="y\n" * 100),
                mock.call([*command, *acquire.ANDROID_SDK_PACKAGES]),
            ])
            self.assertEqual(write_env.call_args_list, [
                mock.call("ANDROID_HOME", str(root)),
                mock.call("ANDROID_SDK_ROOT", str(root)),
                mock.call("ANDROID_NDK_HOME", str(root / "ndk" / acquire.ANDROID_NDK_VERSION)),
                mock.call("ANDROID_NDK_ROOT", str(root / "ndk" / acquire.ANDROID_NDK_VERSION)),
            ])

    def test_install_manifest_dependencies_uses_isolated_platform_triplets(self):
        for platform_name, triplet in (
            ("macos", "arm64-osx"),
            ("windows", "x64-windows"),
            ("android", "arm64-android"),
        ):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                vcpkg_root = root / "vcpkg"
                vcpkg_root.mkdir()
                executable = vcpkg_root / ("vcpkg.exe" if os.name == "nt" else "vcpkg")
                executable.touch()
                installed_root = root / "vcpkg-installed"

                def install(command):
                    installed_root.mkdir()
                    return ""

                with mock.patch.object(acquire, "run", side_effect=install) as run, mock.patch.object(acquire, "write_github_env") as write_env:
                    result = acquire.install_manifest_dependencies(vcpkg_root, platform_name, installed_root)

                repository = Path(__file__).resolve().parents[2]
                self.assertEqual(result, installed_root)
                run.assert_called_once_with([
                    str(executable),
                    "install",
                    f"--triplet={triplet}",
                    f"--x-manifest-root={repository}",
                    f"--x-install-root={installed_root}",
                ])
                write_env.assert_called_once_with("VCPKG_INSTALLED_DIR", str(installed_root))

    def test_install_private_dependencies_passes_isolated_vcpkg_root(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(os.environ, {"VCPKG_INSTALLED_DIR": "/tmp/vcpkg-installed"}):
            root = Path(directory) / "private-dependencies"
            for name in acquire.PRIVATE_DEPENDENCIES:
                source = root / name
                source.mkdir(parents=True)
                (source / "CMakeLists.txt").touch()
            with mock.patch.object(acquire, "run") as run, mock.patch.object(acquire, "write_github_env"):
                acquire.install_private_dependencies(root, "macos", ["-DVCPKG_MANIFEST_INSTALL=OFF"])
            configure_commands = [call.args[0] for call in run.call_args_list if call.args[0][0] == "cmake" and "-S" in call.args[0]]
            self.assertEqual(len(configure_commands), len(acquire.PRIVATE_DEPENDENCIES))
            for command in configure_commands:
                self.assertIn("-DVCPKG_INSTALLED_DIR=/tmp/vcpkg-installed", command)
                self.assertIn("-DVCPKG_MANIFEST_INSTALL=OFF", command)

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

    def test_private_dependency_lock_requires_exact_allowlisted_repositories_and_pins(self):
        with tempfile.TemporaryDirectory() as directory:
            lock = Path(directory) / "dependencies.lock.json"
            lock.write_text(
                json.dumps(
                    {
                        "schema": 1,
                        "dependencies": {
                            "CoreCpp": {
                                "repository": "CoreJust/CoreCpp",
                                "revision": "a" * 40,
                            },
                            "CoreProject2026": {
                                "repository": "CoreJust/CoreProject2026",
                                "revision": "b" * 40,
                            },
                        },
                    }
                ),
                encoding="utf-8",
            )
            parsed = acquire.require_private_dependency_lock(lock)
            self.assertEqual(parsed["CoreCpp"]["revision"], "a" * 40)
            self.assertEqual(parsed["CoreProject2026"]["repository"], "CoreJust/CoreProject2026")
            lock.write_text(
                json.dumps(
                    {
                        "schema": 1,
                        "dependencies": {
                            "CoreCpp": {
                                "repository": "CoreJust/Unapproved",
                                "revision": "a" * 40,
                            },
                            "CoreProject2026": {
                                "repository": "CoreJust/CoreProject2026",
                                "revision": "main",
                            },
                        },
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(acquire.CiError, "CoreCpp repository"):
                acquire.require_private_dependency_lock(lock)

    def test_repository_private_dependency_lock_is_complete(self):
        lock = Path(__file__).resolve().parents[2] / "dependencies.lock.json"
        parsed = acquire.require_private_dependency_lock(lock)
        self.assertEqual(set(parsed), {"CoreCpp", "CoreProject2026"})

    def test_private_dependency_fetch_pins_github_host_key(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            if os.name == "nt":
                with mock.patch.object(Path, "chmod") as chmod_call:
                    known_hosts = acquire.write_github_known_host(root)
            else:
                known_hosts = acquire.write_github_known_host(root)
            self.assertEqual(known_hosts.read_text(encoding="utf-8"), acquire.GITHUB_SSH_KNOWN_HOST)
            if os.name == "nt":
                chmod_call.assert_called_once()
                self.assertEqual(chmod_call.call_args.args[1], stat.S_IRUSR | stat.S_IWUSR)
            else:
                self.assertEqual(known_hosts.stat().st_mode & 0o777, 0o600)

    def test_private_dependency_fetch_uses_two_key_files_and_exact_detached_pins(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lock = root / "dependencies.lock.json"
            lock.write_text(
                json.dumps(
                    {
                        "schema": 1,
                        "dependencies": {
                            "CoreCpp": {
                                "repository": "CoreJust/CoreCpp",
                                "revision": "a" * 40,
                            },
                            "CoreProject2026": {
                                "repository": "CoreJust/CoreProject2026",
                                "revision": "b" * 40,
                            },
                        },
                    }
                ),
                encoding="utf-8",
            )
            corecpp_key = root / "corecpp-key"
            coreproject_key = root / "coreproject-key"
            corecpp_key.touch()
            coreproject_key.touch()
            corecpp_key.chmod(0o600)
            coreproject_key.chmod(0o600)
            responses = iter(("", "", "", "", "a" * 40, "", "", "", "", "", "b" * 40, ""))
            with mock.patch.object(acquire, "run", side_effect=lambda *_args, **_kwargs: next(responses)) as run:
                sources = acquire.fetch_private_dependencies(
                    lock,
                    root / "sources",
                    {"CoreCpp": corecpp_key, "CoreProject2026": coreproject_key},
                )
            self.assertEqual(sources, {"CoreCpp": root / "sources/CoreCpp", "CoreProject2026": root / "sources/CoreProject2026"})
            commands = [call.args[0] for call in run.call_args_list]
            self.assertIn(["git", "-C", str(root / "sources/CoreCpp"), "checkout", "--detach", "FETCH_HEAD"], commands)
            self.assertIn(["git", "-C", str(root / "sources/CoreProject2026"), "checkout", "--detach", "FETCH_HEAD"], commands)
            fetches = [command for command in commands if "fetch" in command]
            self.assertEqual(len(fetches), 2)
            remotes = [command for command in commands if "remote" in command]
            self.assertEqual(
                [command[-1] for command in remotes],
                ["git@github.com:CoreJust/CoreCpp.git", "git@github.com:CoreJust/CoreProject2026.git"],
            )
            self.assertEqual(fetches[0][-1], "a" * 40)
            self.assertEqual(fetches[1][-1], "b" * 40)
            self.assertNotIn("PRIVATE KEY", "\n".join(" ".join(command) for command in commands))

    def test_private_dependency_artifact_exclusion_rejects_checkout_links(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            private_root = root / "private-dependencies"
            (private_root / "CoreCpp").mkdir(parents=True)
            artifact_root = root / "dist"
            artifact_root.mkdir()
            (artifact_root / "game.bin").write_bytes(b"artifact")
            acquire.verify_private_dependency_artifact_exclusion(artifact_root, private_root)
            (artifact_root / "private-source").symlink_to(private_root / "CoreCpp", target_is_directory=True)
            with self.assertRaisesRegex(acquire.CiError, "must not contain symlinks"):
                acquire.verify_private_dependency_artifact_exclusion(artifact_root, private_root)


if __name__ == "__main__":
    unittest.main()
