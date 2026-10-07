"""Contracts for the pinned desktop-CI acquisition helper."""

from __future__ import annotations

import hashlib
import io
import os
import stat
import subprocess
import tempfile
import unittest
import zipfile
import json
from pathlib import Path
from unittest import mock

from script.ci import acquire


class CiAcquireTests(unittest.TestCase):
    """Verify immutable pins and the separate shader validation contracts."""

    @staticmethod
    def write_windows_runtime_archive(archive: Path, loader_path: str | None = None) -> None:
        """Create the versioned Windows runtime layout supplied by LunarG."""
        root = acquire.VULKAN_WINDOWS_RUNTIME["directory"]
        with zipfile.ZipFile(archive, "w") as contents:
            contents.writestr(loader_path or f"{root}/x64/vulkan-1.dll", b"vulkan loader")
            contents.writestr(f"{root}/VulkanRT-License.txt", b"runtime license")

    def test_pinned_acquisitions_have_expected_immutable_values(self):
        self.assertEqual(acquire.CMAKE_VERSION, "3.31.6")
        self.assertEqual(acquire.NINJA_VERSION, "1.13.1")
        self.assertEqual(acquire.PYTHON_VERSION, "3.12.10")
        self.assertEqual(acquire.VCPKG_COMMIT, "2b65c20fc66eda893aa15a15a453c3cf09500b19")
        self.assertEqual(acquire.GMP_AUTOCONF_OVERLAY_ID, "gmp-autoconf-2.71-4")
        self.assertEqual(acquire.VULKAN_VERSION, "1.4.357.0")
        self.assertEqual(acquire.VULKAN_DOWNLOADS["windows"]["url"], "https://sdk.lunarg.com/sdk/download/1.4.357.0/windows/vulkan_sdk.exe")
        self.assertEqual(acquire.VULKAN_DOWNLOADS["macos"]["url"], "https://sdk.lunarg.com/sdk/download/1.4.357.0/mac/vulkan_sdk.zip")
        self.assertEqual(acquire.VULKAN_DOWNLOADS["windows"]["sha256"], "81f474711e9042f4cd22b31b2f7a8870db2e428b21586fb43dd80150be97310d")
        self.assertEqual(acquire.VULKAN_DOWNLOADS["macos"]["sha256"], "539433589c83522e6f31b1c7b418a4167e21597a4a361ab119e1dc0760cf3865")
        self.assertEqual(acquire.VULKAN_WINDOWS_RUNTIME["url"], "https://sdk.lunarg.com/sdk/download/1.4.357.0/windows/vulkan-runtime-components.zip?Human=true")
        self.assertEqual(acquire.VULKAN_WINDOWS_RUNTIME["sha256"], "a14672efed15aafc7f5a16572d35cd3a3416eadf670aeee3cdf50ee32d5fbf83")
        self.assertEqual(acquire.ANDROID_COMMAND_LINE_TOOLS["url"], "https://dl.google.com/android/repository/commandlinetools-linux-15859902_latest.zip")
        self.assertEqual(acquire.ANDROID_COMMAND_LINE_TOOLS["sha256"], "4e4c464f145a7512b57d088ac6c278c03c9eea610886b35a5e0804e74eedf583")
        self.assertEqual(acquire.ANDROID_MAC_ARM64_COMMAND_LINE_TOOLS["url"], "https://dl.google.com/android/repository/commandlinetools-mac_arm64-15859902_latest.zip")
        self.assertEqual(acquire.ANDROID_MAC_ARM64_COMMAND_LINE_TOOLS["sha256"], "835b62a26162b229b441d1f6d4680383815a270809eb33522c0d480fa5002c4e")
        self.assertEqual(acquire.NINJA_DOWNLOADS["windows"]["sha256"], "26a40fa8595694dec2fad4911e62d29e10525d2133c9a4230b66397774ae25bf")
        self.assertEqual(acquire.NINJA_DOWNLOADS["macos"]["sha256"], "da7797794153629aca5570ef7c813342d0be214ba84632af886856e8f0063dd9")

    def test_project_vcpkg_configuration_selects_the_verified_gmp_overlay(self):
        repository = Path(__file__).resolve().parents[2]
        configuration = json.loads((repository / "vcpkg-configuration.json").read_text(encoding="utf-8"))
        self.assertEqual(configuration["overlay-ports"], ["script/ci/vcpkg-overlays"])
        overlay_root = repository / configuration["overlay-ports"][0]
        port = overlay_root / "gmp"
        manifest = json.loads((port / "vcpkg.json").read_text(encoding="utf-8"))
        portfile = (port / "portfile.cmake").read_text(encoding="utf-8")

        self.assertEqual(manifest["name"], "gmp")
        self.assertIn("https://repo.msys2.org/msys/x86_64/autoconf2.71-2.71-4-any.pkg.tar.zst", portfile)
        self.assertIn(
            "c93b791eb55893cbe7c425e764074837355fd165deb7b1775f652c8e25d9d1f0cdd4120ab710d56fb859b7df55c4f971eccda7c112448f60615bff8a2dc81166",
            portfile,
        )
        self.assertNotIn("autoconf2.71-2.71-3-any.pkg.tar.zst", portfile)

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

    def test_install_windows_vulkan_stages_hash_verified_runtime_loader_and_license(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "vulkan"
            runtime_archive = Path(directory) / "runtime.zip"
            self.write_windows_runtime_archive(runtime_archive)
            sdk_root = root / "1.4.357.0" / "x86_64"
            compiler = sdk_root / "Bin" / "glslc.exe"

            def download(url: str, destination: Path) -> None:
                if url == acquire.VULKAN_WINDOWS_RUNTIME["url"]:
                    destination.write_bytes(runtime_archive.read_bytes())
                else:
                    destination.touch()

            def run(_command: list[str]) -> str:
                compiler.parent.mkdir(parents=True)
                compiler.touch()
                return ""

            with mock.patch.object(acquire, "download", side_effect=download) as download_mock, mock.patch.object(
                acquire,
                "verify_sha256",
            ) as verify, mock.patch.object(acquire, "run", side_effect=run) as run_mock, mock.patch.object(
                acquire,
                "write_github_env",
            ), mock.patch.object(acquire, "write_github_path"):
                result = acquire.install_vulkan("windows", root)

            config = acquire.VULKAN_DOWNLOADS["windows"]
            self.assertEqual(result, sdk_root)
            self.assertEqual(
                download_mock.call_args_list,
                [
                    mock.call(f"{config['url']}?Human=true", root.parent / config["filename"]),
                    mock.call(acquire.VULKAN_WINDOWS_RUNTIME["url"], root.parent / acquire.VULKAN_WINDOWS_RUNTIME["filename"]),
                ],
            )
            self.assertEqual(
                verify.call_args_list,
                [
                    mock.call(root.parent / config["filename"], config["sha256"]),
                    mock.call(root.parent / acquire.VULKAN_WINDOWS_RUNTIME["filename"], acquire.VULKAN_WINDOWS_RUNTIME["sha256"]),
                ],
            )
            run_mock.assert_called_once_with([
                str(root.parent / config["filename"]),
                "--root", str(root),
                "--accept-licenses",
                "--default-answer",
                "--confirm-command", "install",
                "copy_only=1",
            ])
            self.assertEqual((sdk_root / "Bin/vulkan-1.dll").read_bytes(), b"vulkan loader")
            self.assertEqual((sdk_root / "VulkanRT-License.txt").read_bytes(), b"runtime license")

    def test_windows_vulkan_runtime_rejects_bad_checksum_and_non_x64_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "vulkan"
            sdk_root = root / "sdk"
            (sdk_root / "Bin").mkdir(parents=True)
            archive = Path(directory) / "runtime.zip"
            self.write_windows_runtime_archive(archive)

            with mock.patch.object(acquire, "download", side_effect=lambda _url, destination: destination.write_bytes(archive.read_bytes())), mock.patch.object(
                acquire,
                "verify_sha256",
                side_effect=acquire.CiError("SHA-256 mismatch"),
            ):
                with self.assertRaisesRegex(acquire.CiError, "SHA-256 mismatch"):
                    acquire.install_windows_vulkan_runtime(root, sdk_root)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "vulkan"
            sdk_root = root / "sdk"
            (sdk_root / "Bin").mkdir(parents=True)
            archive = Path(directory) / "runtime.zip"
            self.write_windows_runtime_archive(
                archive,
                f"{acquire.VULKAN_WINDOWS_RUNTIME['directory']}/x86/vulkan-1.dll",
            )

            with mock.patch.object(acquire, "download", side_effect=lambda _url, destination: destination.write_bytes(archive.read_bytes())), mock.patch.object(acquire, "verify_sha256"):
                with self.assertRaisesRegex(acquire.CiError, "missing x64 loader"):
                    acquire.install_windows_vulkan_runtime(root, sdk_root)
            self.assertFalse((sdk_root / "Bin/vulkan-1.dll").exists())

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
        self.check_android_sdk_install("Linux", "x86_64", acquire.ANDROID_COMMAND_LINE_TOOLS)

    def test_install_android_sdk_on_apple_silicon_uses_verified_native_archive(self):
        self.check_android_sdk_install("Darwin", "arm64", acquire.ANDROID_MAC_ARM64_COMMAND_LINE_TOOLS)

    def test_android_sdk_archive_selection_preserves_linux_hosts_and_arm_aliases(self):
        for system, machine, config in (
            ("Linux", "x86_64", acquire.ANDROID_COMMAND_LINE_TOOLS),
            ("Linux", "aarch64", acquire.ANDROID_COMMAND_LINE_TOOLS),
            ("Linux", "arm64", acquire.ANDROID_COMMAND_LINE_TOOLS),
            ("Darwin", "arm64", acquire.ANDROID_MAC_ARM64_COMMAND_LINE_TOOLS),
            ("Darwin", "aarch64", acquire.ANDROID_MAC_ARM64_COMMAND_LINE_TOOLS),
        ):
            with self.subTest(system=system, machine=machine), mock.patch.object(acquire.host_platform, "system", return_value=system), mock.patch.object(acquire.host_platform, "machine", return_value=machine):
                self.assertEqual(acquire.android_command_line_tools(), config)

    def test_android_sdk_rejects_unsupported_host_before_download(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(acquire.host_platform, "system", return_value="Darwin"), mock.patch.object(acquire.host_platform, "machine", return_value="x86_64"), mock.patch.object(acquire, "download") as download:
            root = Path(directory) / "android-sdk"
            with self.assertRaisesRegex(acquire.CiError, "unsupported Android SDK host: Darwin/x86_64"):
                acquire.install_android_sdk(root)
            download.assert_not_called()
            self.assertFalse(root.exists())

    def check_android_sdk_install(self, system: str, machine: str, config: dict[str, str]) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "android-sdk"
            archive = Path(directory) / config["filename"]
            chmod_requests: list[tuple[Path, int]] = []
            real_chmod = os.chmod
            with zipfile.ZipFile(archive, "w") as contents:
                contents.writestr("cmdline-tools/bin/sdkmanager", "#!/bin/sh\n")
                contents.writestr("cmdline-tools/bin/avdmanager", "#!/bin/sh\n")
                contents.writestr("cmdline-tools/source.properties", "Pkg.Revision=20.0\n")
            with mock.patch.object(acquire.host_platform, "system", return_value=system), mock.patch.object(acquire.host_platform, "machine", return_value=machine), mock.patch.object(acquire, "download") as download, mock.patch.object(acquire, "verify_sha256") as verify, mock.patch.object(acquire, "run") as run, mock.patch.object(acquire, "write_github_env") as write_env:
                def copy_archive(_url: str, destination: Path) -> None:
                    destination.write_bytes(archive.read_bytes())

                download.side_effect = copy_archive
                def record_chmod(
                    path: str | os.PathLike[str],
                    mode: int,
                    *,
                    follow_symlinks: bool = True,
                ) -> None:
                    chmod_requests.append((Path(path), mode))
                    real_chmod(path, mode, follow_symlinks=follow_symlinks)

                with mock.patch.object(acquire.os, "chmod", side_effect=record_chmod):
                    sdkmanager = acquire.install_android_sdk(root)
            expected_archive = root.parent / config["filename"]
            self.assertEqual(sdkmanager, root / "cmdline-tools" / "latest" / "bin" / "sdkmanager")
            self.assertTrue(sdkmanager.is_file())
            executables = [sdkmanager, sdkmanager.parent / "avdmanager"]
            executable_requests = [
                (path, mode)
                for path, mode in chmod_requests
                if path in executables
            ]
            self.assertCountEqual([path for path, _ in executable_requests], executables)
            for _, mode in executable_requests:
                self.assertTrue(mode & stat.S_IXUSR)
            if os.name == "posix":
                for executable in executables:
                    self.assertTrue(executable.stat().st_mode & stat.S_IXUSR)
            download.assert_called_once_with(config["url"], expected_archive)
            verify.assert_called_once_with(expected_archive, config["sha256"])
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
            ("android-hwasan", "arm64-android-hwasan"),
            ("linux-analysis", "x64-linux-lsan"),
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

                with mock.patch.dict(os.environ, {"MC_VCPKG_ANALYSIS_TRIPLET": triplet}), mock.patch.object(
                    acquire, "run", side_effect=install
                ) as run, mock.patch.object(acquire, "write_github_env") as write_env:
                    result = acquire.install_manifest_dependencies(vcpkg_root, platform_name, installed_root)

                repository = Path(__file__).resolve().parents[2]
                command = [
                    str(executable),
                    "install",
                    f"--triplet={triplet}",
                    f"--x-manifest-root={repository}",
                    f"--x-install-root={installed_root}",
                ]
                if platform_name == "linux-analysis":
                    command.append(f"--overlay-triplets={Path(acquire.__file__).resolve().parent / 'vcpkg-triplets'}")
                if platform_name == "android-hwasan":
                    command.append(f"--overlay-triplets={Path(acquire.__file__).resolve().parent / 'vcpkg-triplets'}")
                    command.append(f"--overlay-ports={Path(acquire.__file__).resolve().parent / 'vcpkg-overlays'}")
                self.assertEqual(result, installed_root)
                run.assert_called_once_with(command)
                write_env.assert_called_once_with("VCPKG_INSTALLED_DIR", str(installed_root))

    def test_install_private_dependencies_passes_exact_platform_component_closure(self):
        common_arguments = set(acquire.CORECPP_COMMON_BUILD_ARGUMENTS)
        self.assertIn("-DCORECPP_BUILD_RUNTIME_AUDIO=ON", common_arguments)
        self.assertIn("-DCORECPP_BUILD_RUNTIME_AUDIO_OUTPUT=ON", common_arguments)
        analysis_arguments = set(acquire.CORECPP_ANALYSIS_BUILD_ARGUMENTS)
        platform_arguments = {
            name: set(arguments)
            for name, arguments in acquire.CORECPP_PLATFORM_BUILD_ARGUMENTS.items()
        }
        expected_component_arguments = {
            name: common_arguments | platform_arguments[name]
            for name in platform_arguments
        }
        expected_component_arguments["android-hwasan"] = (
            common_arguments | platform_arguments["android"]
        )
        expected_component_arguments["linux-analysis"] = analysis_arguments
        all_component_arguments = (
            common_arguments
            | analysis_arguments
            | set().union(*platform_arguments.values())
        )
        for platform_name, expected_arguments in expected_component_arguments.items():
            with self.subTest(platform_name=platform_name), tempfile.TemporaryDirectory() as directory, mock.patch.dict(
                os.environ,
                {"VCPKG_INSTALLED_DIR": "/tmp/vcpkg-installed"},
            ):
                root = Path(directory) / "private-dependencies"
                for name in acquire.PRIVATE_DEPENDENCIES:
                    source = root / name
                    source.mkdir(parents=True)
                    (source / "CMakeLists.txt").touch()
                corecpp_config = root / "install/lib/cmake/CoreCpp/CoreCppConfig.cmake"

                def run_command(command):
                    if command[:2] == ["cmake", "--install"] and command[2].endswith("CoreCpp-build"):
                        corecpp_config.parent.mkdir(parents=True)
                        corecpp_config.touch()
                    return ""

                cmake_arguments = ["-DVCPKG_MANIFEST_INSTALL=OFF"]
                if platform_name == "linux-analysis":
                    cmake_arguments.append(
                        "-DCMAKE_CXX_FLAGS=-fsanitize=leak -fno-omit-frame-pointer -stdlib=libc++"
                    )
                with mock.patch.object(acquire, "run", side_effect=run_command) as run, mock.patch.object(
                    acquire,
                    "write_github_env",
                ):
                    acquire.install_private_dependencies(
                        root,
                        platform_name,
                        cmake_arguments,
                    )
                commands = [call.args[0] for call in run.call_args_list]
                configure_commands = [command for command in commands if command[0] == "cmake" and "-S" in command]
                build_commands = [command for command in commands if command[:2] == ["cmake", "--build"]]
                self.assertEqual(len(build_commands), len(acquire.PRIVATE_DEPENDENCIES))
                for command in build_commands:
                    self.assertEqual(command[-3:], ["--", "-k", "0"])
                self.assertEqual(len(configure_commands), len(acquire.PRIVATE_DEPENDENCIES))
                for command in configure_commands:
                    self.assertIn("-DCMAKE_BUILD_TYPE=Release", command)
                    self.assertIn("-DVCPKG_INSTALLED_DIR=/tmp/vcpkg-installed", command)
                    self.assertIn("-DVCPKG_MANIFEST_INSTALL=OFF", command)
                    self.assertEqual(command[-1], "-DBUILD_TESTING=OFF")
                corecpp_command, coreproject_command = configure_commands
                if platform_name == "linux-analysis":
                    self.assertFalse(set().union(*platform_arguments.values()).intersection(corecpp_command))
                else:
                    for argument in acquire.CORECPP_COMMON_BUILD_ARGUMENTS:
                        self.assertIn(argument, corecpp_command)
                configured_components = all_component_arguments.intersection(corecpp_command)
                self.assertEqual(configured_components, expected_arguments)
                self.assertIn(f"-DCoreCpp_DIR={corecpp_config.parent.as_posix()}", coreproject_command)
                self.assertIn("-DCOREPROJECT2026_BUILD_SCRIPT=OFF", coreproject_command)
                aggregate_flag = "-DCMAKE_CXX_FLAGS=-Wno-error=missing-field-initializers"
                self.assertEqual(aggregate_flag in coreproject_command, platform_name in {"android", "android-hwasan"})
                if platform_name == "linux-analysis":
                    linux_flags = (
                        "-DCMAKE_CXX_FLAGS=-fsanitize=leak -fno-omit-frame-pointer -stdlib=libc++"
                        " -Wno-error=missing-field-initializers"
                    )
                    self.assertIn(linux_flags, coreproject_command)
                    self.assertEqual(
                        sum(argument.startswith("-DCMAKE_CXX_FLAGS=") for argument in coreproject_command),
                        1,
                    )
                    self.assertIn(linux_flags.removesuffix(" -Wno-error=missing-field-initializers"), corecpp_command)
                    self.assertNotIn("-Wno-error=missing-field-initializers", corecpp_command)
                if platform_name == "android-hwasan":
                    self.assertIn("-DVCPKG_TARGET_TRIPLET=arm64-android-hwasan", corecpp_command)
                    self.assertIn("-DANDROID_PLATFORM=android-29", corecpp_command)
                    self.assertIn("-DANDROID_SANITIZE=hwaddress", corecpp_command)
                corecpp_install_index = commands.index(["cmake", "--install", str(root / "CoreCpp-build")])
                coreproject_configure_index = commands.index(coreproject_command)
                self.assertLess(corecpp_install_index, coreproject_configure_index)

    def test_private_dependency_install_requires_corecpp_package_before_dependent_configure(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(
            os.environ,
            {"VCPKG_INSTALLED_DIR": "/tmp/vcpkg-installed"},
        ):
            root = Path(directory) / "private-dependencies"
            for name in acquire.PRIVATE_DEPENDENCIES:
                source = root / name
                source.mkdir(parents=True)
                (source / "CMakeLists.txt").touch()
            with mock.patch.object(acquire, "run", return_value=""):
                with self.assertRaisesRegex(acquire.CiError, "installed CoreCpp package config is missing"):
                    acquire.install_private_dependencies(root, "android", ["-DVCPKG_MANIFEST_INSTALL=OFF"])

    def test_private_dependency_install_matches_requested_preset(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(
            os.environ,
            {"VCPKG_INSTALLED_DIR": "/tmp/vcpkg-installed"},
        ):
            root = Path(directory) / "private-dependencies"
            for name in acquire.PRIVATE_DEPENDENCIES:
                source = root / name
                source.mkdir(parents=True)
                (source / "CMakeLists.txt").touch()
            corecpp_config = root / "install/lib/cmake/CoreCpp/CoreCppConfig.cmake"

            def run_command(command):
                if command[:2] == ["cmake", "--install"] and command[2].endswith("CoreCpp-build"):
                    corecpp_config.parent.mkdir(parents=True)
                    corecpp_config.touch()
                return ""

            with mock.patch.object(acquire, "run", side_effect=run_command) as run, mock.patch.object(
                acquire,
                "write_github_env",
            ):
                acquire.install_private_dependencies(
                    root,
                    "windows",
                    ["-DVCPKG_MANIFEST_INSTALL=OFF"],
                    "debug",
                )
            configure_commands = [
                call.args[0]
                for call in run.call_args_list
                if call.args[0][0] == "cmake" and "-S" in call.args[0]
            ]
            self.assertEqual(len(configure_commands), len(acquire.PRIVATE_DEPENDENCIES))
            self.assertTrue(all("-DCMAKE_BUILD_TYPE=Debug" in command for command in configure_commands))

    def test_install_private_dependencies_rejects_test_override(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(
            os.environ,
            {"VCPKG_INSTALLED_DIR": "/tmp/vcpkg-installed"},
        ):
            with self.assertRaisesRegex(acquire.CiError, "own BUILD_TESTING=OFF"):
                acquire.install_private_dependencies(
                    Path(directory) / "private-dependencies",
                    "android",
                    ["-DBUILD_TESTING=ON"],
                )

    def test_install_private_dependencies_rejects_unknown_preset(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(
            os.environ,
            {"VCPKG_INSTALLED_DIR": "/tmp/vcpkg-installed"},
        ):
            with self.assertRaisesRegex(acquire.CiError, "unsupported private dependency preset"):
                acquire.install_private_dependencies(Path(directory) / "private-dependencies", "windows", [], "profile")

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
                        "schema": 2,
                        "dependencies": {
                            "CoreCpp": {
                                "repository": "CoreJust/CoreCpp",
                                "revision": "a" * 40,
                                "fetch_ref": "refs/heads/codex/corecpp-pin",
                            },
                            "CoreProject2026": {
                                "repository": "CoreJust/CoreProject2026",
                                "revision": "b" * 40,
                                "fetch_ref": "refs/heads/codex/coreproject-pin",
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
                        "schema": 2,
                        "dependencies": {
                            "CoreCpp": {
                                "repository": "CoreJust/Unapproved",
                                "revision": "a" * 40,
                                "fetch_ref": "refs/heads/codex/corecpp-pin",
                            },
                            "CoreProject2026": {
                                "repository": "CoreJust/CoreProject2026",
                                "revision": "main",
                                "fetch_ref": "refs/heads/codex/coreproject-pin",
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
        self.assertEqual(parsed["CoreCpp"]["revision"], "d5759fa48b7073787434121624b5f5f93cf3fa2c")
        self.assertEqual(
            parsed["CoreCpp"]["fetch_ref"],
            "refs/heads/codex/ai-cc-0042-inputspan-public-header",
        )
        self.assertEqual(
            parsed["CoreProject2026"]["revision"],
            "10200a380a883ffd70806627710c3c0c25971cbb",
        )
        self.assertEqual(
            parsed["CoreProject2026"]["fetch_ref"],
            "refs/heads/codex/ai-s7-coreproject-pin-reviewed",
        )

    def test_private_dependency_lock_rejects_non_branch_fetch_ref(self):
        with tempfile.TemporaryDirectory() as directory:
            lock = Path(directory) / "dependencies.lock.json"
            lock.write_text(
                json.dumps(
                    {
                        "schema": 2,
                        "dependencies": {
                            "CoreCpp": {
                                "repository": "CoreJust/CoreCpp",
                                "revision": "a" * 40,
                                "fetch_ref": "refs/tags/v1.0",
                            },
                            "CoreProject2026": {
                                "repository": "CoreJust/CoreProject2026",
                                "revision": "b" * 40,
                                "fetch_ref": "refs/heads/codex/coreproject-pin",
                            },
                        },
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(acquire.CiError, "safe full refs/heads branch name"):
                acquire.require_private_dependency_lock(lock)

    def test_private_dependency_lock_rejects_unsafe_branch_components(self):
        with tempfile.TemporaryDirectory() as directory:
            lock = Path(directory) / "dependencies.lock.json"
            lock.write_text(
                json.dumps(
                    {
                        "schema": 2,
                        "dependencies": {
                            "CoreCpp": {
                                "repository": "CoreJust/CoreCpp",
                                "revision": "a" * 40,
                                "fetch_ref": "refs/heads/codex/../main",
                            },
                            "CoreProject2026": {
                                "repository": "CoreJust/CoreProject2026",
                                "revision": "b" * 40,
                                "fetch_ref": "refs/heads/codex/coreproject-pin",
                            },
                        },
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(acquire.CiError, "safe full refs/heads branch name"):
                acquire.require_private_dependency_lock(lock)

    def test_android_hwasan_private_dependencies_share_the_instrumented_ndk_triplet(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(
            os.environ,
            {"VCPKG_INSTALLED_DIR": "/tmp/vcpkg-installed"},
        ):
            root = Path(directory) / "private-dependencies"
            for name in acquire.PRIVATE_DEPENDENCIES:
                source = root / name
                source.mkdir(parents=True)
                (source / "CMakeLists.txt").touch()
            corecpp_config = root / "install/lib/cmake/CoreCpp/CoreCppConfig.cmake"
            chainload = "-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=/ndk/build/cmake/android.toolchain.cmake"

            def run_command(command):
                if command[:2] == ["cmake", "--install"] and command[2].endswith("CoreCpp-build"):
                    corecpp_config.parent.mkdir(parents=True)
                    corecpp_config.touch()
                return ""

            with mock.patch.object(acquire, "run", side_effect=run_command) as run, mock.patch.object(
                acquire, "write_github_env"
            ):
                acquire.install_private_dependencies(root, "android-hwasan", [chainload])

            configure_commands = [
                call.args[0]
                for call in run.call_args_list
                if call.args[0][:2] == ["cmake", "-S"]
            ]
            self.assertEqual(len(configure_commands), 2)
            for command in configure_commands:
                self.assertIn(chainload, command)
                self.assertIn("-DVCPKG_TARGET_TRIPLET=arm64-android-hwasan", command)
                self.assertIn("-DANDROID_PLATFORM=android-29", command)
                self.assertIn("-DANDROID_STL=c++_shared", command)
                self.assertIn("-DANDROID_SANITIZE=hwaddress", command)
            triplet = Path(acquire.__file__).resolve().parent / "vcpkg-triplets/arm64-android-hwasan.cmake"
            triplet_contents = triplet.read_text(encoding="utf-8")
            self.assertIn("set(VCPKG_CMAKE_SYSTEM_VERSION 29)", triplet_contents)
            self.assertIn("set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE", triplet_contents)
            self.assertIn(
                'set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-linux-android")',
                triplet_contents,
            )

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
                chmod_call.assert_called_once_with(stat.S_IRUSR | stat.S_IWUSR)
            else:
                self.assertEqual(known_hosts.stat().st_mode & 0o777, 0o600)

    def test_private_dependency_fetch_uses_two_key_files_and_exact_detached_pins(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lock = root / "dependencies.lock.json"
            lock.write_text(
                json.dumps(
                    {
                        "schema": 2,
                        "dependencies": {
                            "CoreCpp": {
                                "repository": "CoreJust/CoreCpp",
                                "revision": "a" * 40,
                                "fetch_ref": "refs/heads/codex/corecpp-pin",
                            },
                            "CoreProject2026": {
                                "repository": "CoreJust/CoreProject2026",
                                "revision": "b" * 40,
                                "fetch_ref": "refs/heads/codex/coreproject-pin",
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
            responses = iter(("", "", "", "", "", "a" * 40, "", "", "", "", "", "", "b" * 40, ""))
            with mock.patch.object(acquire, "run", side_effect=lambda *_args, **_kwargs: next(responses)) as run:
                sources = acquire.fetch_private_dependencies(
                    lock,
                    root / "sources",
                    {"CoreCpp": corecpp_key, "CoreProject2026": coreproject_key},
                )
            self.assertEqual(sources, {"CoreCpp": root / "sources/CoreCpp", "CoreProject2026": root / "sources/CoreProject2026"})
            commands = [call.args[0] for call in run.call_args_list]
            self.assertIn(["git", "-C", str(root / "sources/CoreCpp"), "cat-file", "-e", f"{'a' * 40}^{{commit}}"], commands)
            self.assertIn(["git", "-C", str(root / "sources/CoreCpp"), "checkout", "--detach", "a" * 40], commands)
            self.assertIn(["git", "-C", str(root / "sources/CoreProject2026"), "cat-file", "-e", f"{'b' * 40}^{{commit}}"], commands)
            self.assertIn(["git", "-C", str(root / "sources/CoreProject2026"), "checkout", "--detach", "b" * 40], commands)
            fetches = [command for command in commands if "fetch" in command]
            self.assertEqual(len(fetches), 2)
            remotes = [command for command in commands if "remote" in command]
            self.assertEqual(
                [command[-1] for command in remotes],
                ["git@github.com:CoreJust/CoreCpp.git", "git@github.com:CoreJust/CoreProject2026.git"],
            )
            self.assertEqual(fetches[0][-3:], ["--no-tags", "origin", "refs/heads/codex/corecpp-pin"])
            self.assertEqual(fetches[1][-3:], ["--no-tags", "origin", "refs/heads/codex/coreproject-pin"])
            self.assertNotIn("PRIVATE KEY", "\n".join(" ".join(command) for command in commands))

    def test_private_dependency_fetch_fails_clearly_when_pin_is_not_in_fetched_ref(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lock = root / "dependencies.lock.json"
            lock.write_text(
                json.dumps(
                    {
                        "schema": 2,
                        "dependencies": {
                            "CoreCpp": {
                                "repository": "CoreJust/CoreCpp",
                                "revision": "a" * 40,
                                "fetch_ref": "refs/heads/codex/corecpp-pin",
                            },
                            "CoreProject2026": {
                                "repository": "CoreJust/CoreProject2026",
                                "revision": "b" * 40,
                                "fetch_ref": "refs/heads/codex/coreproject-pin",
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

            def fail_on_missing_pin(command):
                if "cat-file" in command:
                    raise acquire.CiError("missing object")
                return ""

            with mock.patch.object(acquire, "run", side_effect=fail_on_missing_pin) as run:
                with self.assertRaisesRegex(
                    acquire.CiError,
                    f"CoreCpp locked revision {'a' * 40} is not reachable from refs/heads/codex/corecpp-pin",
                ):
                    acquire.fetch_private_dependencies(
                        lock,
                        root / "sources",
                        {"CoreCpp": corecpp_key, "CoreProject2026": coreproject_key},
                    )
            self.assertFalse(any("checkout" in call.args[0] for call in run.call_args_list))

    def test_private_dependency_fetches_branch_history_but_checks_out_exact_ancestor_pin(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            key_files = {}
            remotes = {}
            dependencies = {}
            for name, repository in (
                ("CoreCpp", "CoreJust/CoreCpp"),
                ("CoreProject2026", "CoreJust/CoreProject2026"),
            ):
                remote = root / f"{name}.git"
                seed = root / f"{name}-seed"
                subprocess.run(["git", "init", "--bare", str(remote)], capture_output=True, check=True)
                subprocess.run(["git", "init", str(seed)], capture_output=True, check=True)
                subprocess.run(["git", "-C", str(seed), "config", "user.name", "CI fixture"], capture_output=True, check=True)
                subprocess.run(["git", "-C", str(seed), "config", "user.email", "ci-fixture@example.invalid"], capture_output=True, check=True)
                branch = f"codex/{name.lower()}-pin"
                subprocess.run(["git", "-C", str(seed), "checkout", "-b", branch], capture_output=True, check=True)
                fixture_file = seed / "pin.txt"
                fixture_file.write_text("pinned ancestor\n", encoding="utf-8")
                subprocess.run(["git", "-C", str(seed), "add", "pin.txt"], capture_output=True, check=True)
                subprocess.run(["git", "-C", str(seed), "commit", "-m", "pinned commit"], capture_output=True, check=True)
                revision = subprocess.run(
                    ["git", "-C", str(seed), "rev-parse", "HEAD"],
                    capture_output=True,
                    check=True,
                    text=True,
                ).stdout.strip()
                fixture_file.write_text("later branch tip\n", encoding="utf-8")
                subprocess.run(["git", "-C", str(seed), "commit", "-am", "later branch tip"], capture_output=True, check=True)
                subprocess.run(["git", "-C", str(seed), "push", str(remote), f"HEAD:refs/heads/{branch}"], capture_output=True, check=True)
                key_file = root / f"{name}-key"
                key_file.touch()
                key_file.chmod(0o600)
                key_files[name] = key_file
                remotes[repository] = remote
                dependencies[name] = {
                    "repository": repository,
                    "revision": revision,
                    "fetch_ref": f"refs/heads/{branch}",
                }

            lock = root / "dependencies.lock.json"
            lock.write_text(json.dumps({"schema": 2, "dependencies": dependencies}), encoding="utf-8")
            real_run = acquire.run

            def run_against_local_remotes(command):
                if len(command) >= 7 and command[:2] == ["git", "-C"] and command[3:6] == ["remote", "add", "origin"]:
                    repository = command[-1].removeprefix("git@github.com:").removesuffix(".git")
                    command = [*command[:-1], str(remotes[repository])]
                return real_run(command)

            with mock.patch.object(acquire, "git_with_key", side_effect=lambda _key, _hosts, command: ["git", *command]), mock.patch.object(
                acquire,
                "run",
                side_effect=run_against_local_remotes,
            ):
                sources = acquire.fetch_private_dependencies(lock, root / "sources", key_files)

            for name, dependency in dependencies.items():
                actual_revision = subprocess.run(
                    ["git", "-C", str(sources[name]), "rev-parse", "HEAD"],
                    capture_output=True,
                    check=True,
                    text=True,
                ).stdout.strip()
                self.assertEqual(actual_revision, dependency["revision"])
                self.assertEqual((sources[name] / "pin.txt").read_text(encoding="utf-8"), "pinned ancestor\n")

    def test_git_ssh_paths_preserve_windows_drives_spaces_and_option_boundaries(self):
        self.assertEqual(
            acquire.quote_git_ssh_path(r"D:\a\runner temp\private-dependency-keys\corecpp"),
            "'D:/a/runner temp/private-dependency-keys/corecpp'",
        )
        self.assertEqual(
            acquire.quote_git_ssh_path(r"D:\a\runner's temp\github-known-hosts"),
            "'D:/a/runner'\"'\"'s temp/github-known-hosts'",
        )
        with self.assertRaisesRegex(acquire.CiError, "forbidden control character"):
            acquire.quote_git_ssh_path("D:\\a\\key\n-o StrictHostKeyChecking=no")

    def test_cmake_paths_normalize_windows_separators_without_changing_flags(self):
        self.assertEqual(
            acquire.normalize_cmake_path(r"D:\a\runner temp\vcpkg-installed"),
            "D:/a/runner temp/vcpkg-installed",
        )
        self.assertEqual(
            acquire.normalize_cmake_argument(r"-DCMAKE_TOOLCHAIN_FILE=D:\a\vcpkg\scripts\buildsystems\vcpkg.cmake"),
            "-DCMAKE_TOOLCHAIN_FILE=D:/a/vcpkg/scripts/buildsystems/vcpkg.cmake",
        )
        self.assertEqual(acquire.normalize_cmake_argument("-DVCPKG_MANIFEST_INSTALL=OFF"), "-DVCPKG_MANIFEST_INSTALL=OFF")

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
