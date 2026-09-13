"""Static and configure-time contracts for Windows runtime staging."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
HELPERS = REPOSITORY / "cmake/Helpers.cmake"


class WindowsRuntimeStagingTests(unittest.TestCase):
    def test_helper_gates_target_types_and_required_files(self):
        helpers = HELPERS.read_text(encoding="utf-8")
        self.assertIn("function(mc_stage_windows_runtime TARGET)", helpers)
        self.assertIn("set(one_value_arguments RUNTIME_ROOT)", helpers)
        self.assertIn("set(multi_value_arguments DEPENDENCIES REQUIRED_FILES)", helpers)
        self.assertIn('if(NOT TARGET "${TARGET}")', helpers)
        self.assertIn("target_type STREQUAL \"EXECUTABLE\"", helpers)
        self.assertIn("target_type STREQUAL \"SHARED_LIBRARY\"", helpers)
        self.assertIn("dependency_type STREQUAL \"UNKNOWN_LIBRARY\"", helpers)
        self.assertIn('if(NOT EXISTS "${required_file}")', helpers)
        self.assertIn('copy_if_different "${runtime_file}" "${MC_STAGE_RUNTIME_ROOT}"', helpers)
        self.assertNotIn("TARGET_RUNTIME_DLLS", helpers)

    def test_game_and_fixture_targets_use_explicit_runtime_roots(self):
        client = (REPOSITORY / "src/client/CMakeLists.txt").read_text(encoding="utf-8")
        tests = (REPOSITORY / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        fixture = (
            REPOSITORY / "cmake/fixtures/corecpp_server_consumer/CMakeLists.txt"
        ).read_text(encoding="utf-8")
        self.assertIn("mc_stage_windows_runtime(", client)
        self.assertIn('RUNTIME_ROOT "$<TARGET_FILE_DIR:mc_main>"', client)
        self.assertIn('REQUIRED_FILES "${MC_VULKAN_RUNTIME_DLL}"', client)
        self.assertIn('install(FILES "${MC_VULKAN_RUNTIME_DLL}" DESTINATION .)', client)
        for target in ("mc_tests", "mc_renderer_smoke", "mc_renderer_golden"):
            self.assertIn(f"{target}\n        RUNTIME_ROOT", tests)
            self.assertIn(f'RUNTIME_ROOT "$<TARGET_FILE_DIR:{target}>"', tests)
        self.assertIn("mc_stage_windows_runtime(", fixture)
        self.assertIn(
            'RUNTIME_ROOT "$<TARGET_FILE_DIR:mc_corecpp_server_consumer>"',
            fixture,
        )
        self.assertIn("unofficial::enet::enet", fixture)

    def _configure_fixture(self, dependency="runtime", required_file=True):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            build = root / "build"
            source.mkdir()
            (source / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
            runtime = root / "runtime.dll"
            runtime.write_bytes(b"fixture")
            required = root / "required.dll"
            if required_file:
                required.write_bytes(b"required")
            cmake_lists = (
                "cmake_minimum_required(VERSION 3.25)\n"
                "project(RuntimeStagingFixture LANGUAGES CXX)\n"
                "set(WIN32 TRUE)\n"
                f'list(APPEND CMAKE_MODULE_PATH "{(REPOSITORY / "cmake").as_posix()}")\n'
                f'include("{HELPERS.as_posix()}")\n'
                "add_library(runtime SHARED IMPORTED)\n"
                f'set_target_properties(runtime PROPERTIES IMPORTED_LOCATION "{runtime.as_posix()}")\n'
                "add_executable(app main.cpp)\n"
                f'mc_stage_windows_runtime(app RUNTIME_ROOT "$<TARGET_FILE_DIR:app>" '
                f'DEPENDENCIES {dependency} REQUIRED_FILES "{required.as_posix()}")\n'
            )
            (source / "CMakeLists.txt").write_text(cmake_lists, encoding="utf-8")
            return subprocess.run(
                ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja"],
                text=True,
                capture_output=True,
                check=False,
            )

    def test_configure_accepts_supported_imported_runtime_target(self):
        result = self._configure_fixture()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_configure_rejects_missing_runtime_dependency(self):
        result = self._configure_fixture(dependency="missing_runtime")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("runtime staging dependency is missing", result.stdout + result.stderr)

    def test_configure_rejects_missing_required_file(self):
        result = self._configure_fixture(required_file=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("runtime staging file is missing", result.stdout + result.stderr)

    def test_corecpp_server_fixture_configures_with_local_package_stub(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / "prefix/lib/cmake/CoreCpp"
            build = root / "build"
            prefix.mkdir(parents=True)
            for name in (
                "core.a",
                "runtime.a",
                "runtime-network.a",
                "fmt.a",
                "spdlog.a",
                "enet.a",
            ):
                (root / name).write_bytes(b"fixture")
            config = (
                "set(CoreCpp_AVAILABLE_COMPONENTS Core Runtime RuntimeNetwork)\n"
                "set(CoreCpp_Core_FOUND TRUE)\n"
                "set(CoreCpp_Runtime_FOUND TRUE)\n"
                "set(CoreCpp_RuntimeNetwork_FOUND TRUE)\n"
                "set(CoreCpp_FOUND TRUE)\n"
                "add_library(CoreCpp::Core STATIC IMPORTED)\n"
                "add_library(CoreCpp::Runtime STATIC IMPORTED)\n"
                "add_library(CoreCpp::RuntimeNetwork STATIC IMPORTED)\n"
                f'set_target_properties(CoreCpp::Core PROPERTIES IMPORTED_LOCATION "{(root / "core.a").as_posix()}")\n'
                f'set_target_properties(CoreCpp::Runtime PROPERTIES IMPORTED_LOCATION "{(root / "runtime.a").as_posix()}")\n'
                f'set_target_properties(CoreCpp::RuntimeNetwork PROPERTIES IMPORTED_LOCATION "{(root / "runtime-network.a").as_posix()}")\n'
            )
            for target, archive in (
                ("fmt::fmt", "fmt.a"),
                ("spdlog::spdlog", "spdlog.a"),
                ("unofficial::enet::enet", "enet.a"),
            ):
                config += (
                    f"add_library({target} STATIC IMPORTED)\n"
                    f'set_target_properties({target} PROPERTIES IMPORTED_LOCATION "{(root / archive).as_posix()}")\n'
                )
            (prefix / "CoreCppConfig.cmake").write_text(config, encoding="utf-8")
            result = subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(REPOSITORY / "cmake/fixtures/corecpp_server_consumer"),
                    "-B",
                    str(build),
                    "-G",
                    "Ninja",
                    f"-DCMAKE_PREFIX_PATH={root / 'prefix'}",
                    "-DCMAKE_BUILD_TYPE=Debug",
                    "-DWIN32=TRUE",
                ],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
