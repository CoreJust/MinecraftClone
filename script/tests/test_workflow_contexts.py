"""Static contracts for valid GitHub Actions workflow contexts."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
WORKFLOWS = (
    REPOSITORY / ".github/workflows/ai-checks.yml",
    REPOSITORY / ".github/workflows/snapshot-artifacts.yml",
)


class WorkflowContextTests(unittest.TestCase):
    def test_snapshot_trust_condition_has_valid_bash_then_separator(self):
        workflow = WORKFLOWS[1].read_text(encoding="utf-8")
        self.assertIn("git tag --points-at", workflow)
        self.assertIn("'+refs/tags/ai/*:refs/tags/ai/*'", workflow)
        self.assertIn("_[0-9]{2}\\.[0-9]{2}\\.[0-9]{2}$'; then", workflow)
        self.assertNotIn('refs/remotes/origin/ai-main \\\n              then', workflow)

    def test_workflows_keep_runner_context_out_of_job_level_env(self):
        for workflow_path in WORKFLOWS:
            lines = workflow_path.read_text(encoding="utf-8").splitlines()
            job_env_lines: list[str] = []
            collecting = False
            for line in lines:
                if line == "    env:":
                    collecting = True
                    continue
                if collecting and line.startswith("      "):
                    job_env_lines.append(line)
                    continue
                if collecting:
                    collecting = False
            job_env = "\n".join(job_env_lines)
            self.assertNotIn("${{ runner.", job_env, workflow_path)
            workflow = workflow_path.read_text(encoding="utf-8")
            self.assertIn("Configure vcpkg binary cache (macOS)", workflow)
            self.assertIn("Configure vcpkg binary cache (Windows)", workflow)
            self.assertIn("mkdir -p \"$cache_dir\"", workflow)
            self.assertIn("VCPKG_DEFAULT_BINARY_CACHE=$cache_dir", workflow)
            self.assertIn("vcpkg-binary-cache", workflow)

    def test_snapshot_workflow_bootstraps_android_sdk_before_package_install(self):
        workflow = WORKFLOWS[1].read_text(encoding="utf-8")
        bootstrap = 'python script/ci/acquire.py install-android-sdk --root "$RUNNER_TEMP/android-sdk"'
        self.assertIn(bootstrap, workflow)
        self.assertNotIn("run: sdkmanager", workflow)

    def test_private_cmake_arguments_preserve_leading_dashes(self):
        for workflow_path in WORKFLOWS:
            workflow = workflow_path.read_text(encoding="utf-8")
            self.assertNotRegex(workflow, r"--cmake-arg\s+[\"']-D", workflow_path)
            for line in workflow.splitlines():
                if "--cmake-arg" in line:
                    self.assertRegex(line, r"--cmake-arg=-D", workflow_path)

    def test_private_dependency_secrets_are_limited_to_trusted_ai_main_desktop(self):
        workflow = WORKFLOWS[0].read_text(encoding="utf-8")
        desktop = workflow.split("\n  desktop:", maxsplit=1)[1]
        self.assertIn(
            "if: github.event_name != 'pull_request' && github.ref == 'refs/heads/ai-main'",
            desktop,
        )
        source = workflow.split("\n  desktop:", maxsplit=1)[0]
        self.assertNotIn("secrets.", source)
        self.assertIn("fetch-private-dependencies", desktop)
        self.assertIn("install-private-dependencies", desktop)

    def test_private_prefixes_and_manifest_mode_are_explicit(self):
        for workflow_path in WORKFLOWS:
            workflow = workflow_path.read_text(encoding="utf-8")
            self.assertIn("install-manifest-dependencies", workflow, workflow_path)
            self.assertIn("--installed-root", workflow, workflow_path)
            self.assertIn("VCPKG_INSTALLED_DIR", workflow, workflow_path)
            self.assertIn("-DVCPKG_MANIFEST_INSTALL=OFF", workflow, workflow_path)
            self.assertIn("-DCMAKE_PREFIX_PATH=", workflow, workflow_path)
            self.assertIn("-DCoreCpp_DIR=", workflow, workflow_path)
            self.assertIn("-DCoreProject2026_DIR=", workflow, workflow_path)

    def test_private_dependency_install_matches_consumer_preset(self):
        ai_workflow = WORKFLOWS[0].read_text(encoding="utf-8")
        snapshot_workflow = WORKFLOWS[1].read_text(encoding="utf-8")
        self.assertIn('--preset "${{ matrix.preset }}"', ai_workflow)
        self.assertIn("--preset release", snapshot_workflow)

    def test_windows_key_acl_is_current_user_only_and_fail_closed(self):
        for workflow_path in WORKFLOWS:
            workflow = workflow_path.read_text(encoding="utf-8")
            self.assertNotIn("$env:USERNAME:(OI)(CI)(R,W)", workflow, workflow_path)
            self.assertIn("$currentUser = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name", workflow, workflow_path)
            self.assertIn("$grant = '{0}:F' -f $currentUser", workflow, workflow_path)
            self.assertIn("/inheritance:r /grant:r $grant", workflow, workflow_path)
            self.assertIn("if ($LASTEXITCODE -ne 0)", workflow, workflow_path)

    def test_windows_keys_are_lf_utf8_without_bom_and_parse_before_fetch(self):
        for workflow_path in WORKFLOWS:
            workflow = workflow_path.read_text(encoding="utf-8")
            self.assertIn("$utf8NoBom = [System.Text.UTF8Encoding]::new($false)", workflow, workflow_path)
            self.assertIn('.Replace("`r`n", "`n").Replace("`r", "`n")', workflow, workflow_path)
            self.assertIn("$bytes[-1] -ne 10", workflow, workflow_path)
            self.assertIn("$bytes -contains 13", workflow, workflow_path)
            self.assertIn("$bytes[0] -eq 239", workflow, workflow_path)
            self.assertIn("& ssh-keygen.exe -y -f $keyPath *> $null", workflow, workflow_path)
            self.assertNotIn("[Environment]::NewLine", workflow, workflow_path)
            self.assertNotIn("WriteAllText($coreCppKey, $env:", workflow, workflow_path)
            self.assertNotIn("WriteAllText($coreProjectKey, $env:", workflow, workflow_path)
            self.assertLess(workflow.index("ssh-keygen.exe"), workflow.index("fetch-private-dependencies"))

    def test_artifact_uploads_never_include_private_sources(self):
        for workflow_path in WORKFLOWS:
            workflow = workflow_path.read_text(encoding="utf-8")
            for upload in workflow.split("path: |")[1:]:
                self.assertNotIn("private-dependencies", upload.split("if-no-files-found", maxsplit=1)[0], workflow_path)

    def test_windows_private_install_enters_msvc_before_cmake(self):
        for workflow_path in WORKFLOWS:
            workflow = workflow_path.read_text(encoding="utf-8")
            private_step = workflow.split("      - name: Install pinned private dependencies (Windows)\n", maxsplit=1)[1]
            private_step = private_step.split("      - name:", maxsplit=1)[0]
            developer_environment = 'call "%ProgramFiles%\\Microsoft Visual Studio\\2022\\Enterprise\\Common7\\Tools\\VsDevCmd.bat" -arch=amd64'
            self.assertLess(private_step.index(developer_environment), private_step.index("install-private-dependencies"), workflow_path)
            self.assertIn('set "MC_ACQUIRED_VCPKG_ROOT=%VCPKG_ROOT%"', private_step, workflow_path)
            self.assertIn('set "VCPKG_ROOT=%MC_ACQUIRED_VCPKG_ROOT%"', private_step, workflow_path)
            self.assertIn(
                '"--cmake-arg=-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\\scripts\\buildsystems\\vcpkg.cmake"',
                private_step,
                workflow_path,
            )
            self.assertIn("if errorlevel 1 exit /b %errorlevel%", private_step, workflow_path)

    def test_windows_snapshot_stages_pinned_vulkan_loader(self):
        workflow = WORKFLOWS[1].read_text(encoding="utf-8")
        runtime_step = workflow.split("      - name: Stage pinned Windows Vulkan runtime\n", maxsplit=1)[1]
        runtime_step = runtime_step.split("      - name:", maxsplit=1)[0]
        url = "https://sdk.lunarg.com/sdk/download/1.4.357.0/windows/vulkan-runtime-components.zip"
        self.assertIn(url, runtime_step)
        self.assertIn("A14672EFED15AAFC7F5A16572D35CD3A3416EADF670AEEE3CDF50EE32D5FBF83", runtime_step)
        self.assertIn("VulkanRT-X64-1.4.357.0-Components/x64/vulkan-1.dll", runtime_step)
        self.assertIn('Join-Path $env:VULKAN_SDK "Bin/vulkan-1.dll"', runtime_step)
        self.assertIn("Get-FileHash -Algorithm SHA256", runtime_step)
        self.assertIn('if ($actual -ne $expected)', runtime_step)
        self.assertIn("(Get-Item $destination).Length -le 0", runtime_step)
        self.assertLess(runtime_step.index("Get-FileHash"), runtime_step.index("Expand-Archive"))
        self.assertLess(runtime_step.index('if ($actual -ne $expected)'), runtime_step.index("Expand-Archive"))
        self.assertLess(workflow.index("Stage pinned Windows Vulkan runtime"), workflow.index("Build, test, and package Windows"))
        self.assertIn('--vulkan-runtime "build\\install\\vulkan-1.dll"', workflow)

    def test_snapshot_private_dependencies_use_portable_compilers_without_changing_locks(self):
        workflow = WORKFLOWS[1].read_text(encoding="utf-8")
        windows_install = workflow.split(
            "      - name: Install pinned private dependencies (Windows)\n", maxsplit=1
        )[1].split("      - name:", maxsplit=1)[0]
        self.assertIn("--cmake-arg=-DCMAKE_C_COMPILER=clang-cl", windows_install)
        self.assertIn("--cmake-arg=-DCMAKE_CXX_COMPILER=clang-cl", windows_install)
        self.assertIn("corelang-windows-compat.cmake", windows_install)
        self.assertIn(
            'set "_CL_=-Wno-everything /W4 /WX -Wno-error=reorder-init-list -Wno-error=unused-command-line-argument -Wno-error=unknown-attributes"',
            windows_install,
        )
        self.assertIn(
            "add_compile_options^(-Wno-error=reorder-init-list -Wno-error=unused-command-line-argument -Wno-error=unknown-attributes -Wno-c++98-compat -Wno-c++98-compat-pedantic -Wno-pre-c++17-compat^)",
            windows_install,
        )
        self.assertIn("--cmake-arg=-DCMAKE_PROJECT_INCLUDE_BEFORE=", windows_install)
        self.assertLess(windows_install.index("VsDevCmd.bat"), windows_install.index('set "_CL_='))
        self.assertLess(
            windows_install.index('set "_CL_='),
            windows_install.index("python script/ci/acquire.py install-private-dependencies"),
        )

        macos_install = workflow.split(
            "      - name: Install pinned private dependencies (macOS)\n", maxsplit=1
        )[1].split("      - name:", maxsplit=1)[0]
        self.assertIn("run: >-", macos_install)
        self.assertIn("python script/ci/acquire.py install-private-dependencies\n", macos_install)
        self.assertNotIn("install-private-dependencies \\", macos_install)

        android_install = workflow.split(
            "      - name: Install pinned private dependencies\n", maxsplit=1
        )[1].split("      - name:", maxsplit=1)[0]
        self.assertIn("python script/ci/acquire.py install-private-dependencies \\\n", android_install)
        self.assertIn("add_compile_options(-Wno-error=reorder-init-list)", android_install)
        self.assertIn("--cmake-arg=-DCMAKE_PROJECT_INCLUDE_BEFORE=", android_install)
        self.assertNotIn("dependencies.lock.json", android_install)

    def test_macos_snapshot_defers_load_sensitive_flight_to_target_hardware(self):
        workflow = WORKFLOWS[1].read_text(encoding="utf-8")
        macos_phase = workflow.split(
            "      - name: Build, test, and package macOS\n", maxsplit=1
        )[1].split("      - name:", maxsplit=1)[0]
        server_test = "MinecraftClone.ServerOnlyBuild"
        flight_test = "GameServerPreviewTest.SustainedFlightKeepsInputAcknowledgementsCurrentWhileTilesStream"
        broad_ctest = next(
            line.strip()
            for line in macos_phase.splitlines()
            if "ctest --test-dir" in line and " -E " in line
        )
        self.assertIn("-E", broad_ctest)
        self.assertIn(server_test, broad_ctest)
        self.assertIn(flight_test, broad_ctest)
        self.assertIn("minecraftclone_server_only_test.cmake", macos_phase)
        self.assertNotIn(f"-R '^{flight_test}$'", macos_phase)
        self.assertIn('replacement = \' --output-on-failure -E "^GameServerPreviewTest', macos_phase)
        self.assertIn('source.count(needle) != 1', macos_phase)
        self.assertEqual(macos_phase.count("release-tests-macos.log"), 2)

    def test_windows_cmd_build_phases_guard_each_fallible_command(self):
        phase_names = ("Build, test, and validate shaders (Windows)", "Build, test, and package Windows")
        commands = ("cmake --preset", "cmake --build", "ctest --test-dir", "python script/ci/acquire.py validate-shaders")
        for workflow_path in WORKFLOWS:
            workflow = workflow_path.read_text(encoding="utf-8")
            for phase_name in phase_names:
                if phase_name not in workflow:
                    continue
                phase = workflow.split(f"      - name: {phase_name}\n", maxsplit=1)[1].split("      - name:", maxsplit=1)[0]
                lines = [line.strip() for line in phase.splitlines()]
                for command in commands:
                    index = next(index for index, line in enumerate(lines) if line.startswith(command))
                    if phase_name == "Build, test, and package Windows" and command == "ctest --test-dir":
                        self.assertEqual(
                            lines[index + 1:index + 6],
                            [
                                'set "ctest_result=%errorlevel%"',
                                "type build\\release-tests-windows.log",
                                'set "ctest_log_result=%errorlevel%"',
                                'if not "%ctest_result%"=="0" exit /b %ctest_result%',
                                'if not "%ctest_log_result%"=="0" exit /b %ctest_log_result%',
                            ],
                            workflow_path,
                        )
                        continue
                    self.assertEqual(lines[index + 1], "if errorlevel 1 exit /b %errorlevel%", (workflow_path, command))

    def test_nested_cmake_scripts_receive_forward_slash_path_substitutions(self):
        cmake_lists = (REPOSITORY / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        for source_name, normalized_name in (
            ("CMAKE_CURRENT_BINARY_DIR", "MC_TEST_BINARY_DIR"),
            ("CMAKE_COMMAND", "MC_CMAKE_COMMAND"),
            ("CMAKE_CTEST_COMMAND", "MC_CTEST_COMMAND"),
            ("PROJECT_SOURCE_DIR", "MC_PROJECT_SOURCE_DIR"),
            ("CMAKE_PREFIX_PATH", "MC_CMAKE_PREFIX_PATH"),
            ("CoreCpp_DIR", "MC_CORECPP_DIR"),
            ("CoreProject2026_DIR", "MC_COREPROJECT2026_DIR"),
        ):
            self.assertIn(f'file(TO_CMAKE_PATH "${{{source_name}}}" {normalized_name})', cmake_lists)
        for template in (REPOSITORY / "tests/cmake").glob("*.in"):
            contents = template.read_text(encoding="utf-8")
            self.assertNotRegex(contents, r"@(CMAKE_CURRENT_BINARY_DIR|CMAKE_COMMAND|CMAKE_CTEST_COMMAND|PROJECT_SOURCE_DIR|CMAKE_PREFIX_PATH|CoreCpp_DIR|CoreProject2026_DIR)@")
        for template_name in ("corecpp_server_consumer_test.cmake.in", "minecraftclone_server_only_test.cmake.in"):
            contents = (REPOSITORY / "tests/cmake" / template_name).read_text(encoding="utf-8")
            self.assertIn('set(MC_TEST_CONFIGURATION "@CMAKE_BUILD_TYPE@")', contents)
            self.assertIn('"-DCMAKE_BUILD_TYPE=${MC_TEST_CONFIGURATION}"', contents)
            self.assertIn('REGEX "^CMAKE_CONFIGURATION_TYPES:.*="', contents)
            self.assertIn("list(APPEND build_command --config \"${MC_TEST_CONFIGURATION}\")", contents)
            self.assertNotIn("-DCMAKE_BUILD_TYPE=Debug", contents)
        self.assertEqual(cmake_lists.count('"-DMC_TEST_CONFIGURATION=$<CONFIG>"'), 2)
        corecpp_template = (REPOSITORY / "tests/cmake/corecpp_server_consumer_test.cmake.in").read_text(encoding="utf-8")
        self.assertIn('set(consumer_directory "${consumer_directory}/${MC_TEST_CONFIGURATION}")', corecpp_template)
        minecraftclone_template = (REPOSITORY / "tests/cmake/minecraftclone_server_only_test.cmake.in").read_text(encoding="utf-8")
        self.assertIn("list(APPEND test_command -C \"${MC_TEST_CONFIGURATION}\")", minecraftclone_template)

    def test_server_only_build_test_has_platform_timeout(self):
        cmake_lists = (REPOSITORY / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("set_tests_properties(MinecraftClone.ServerOnlyBuild PROPERTIES TIMEOUT 180)", cmake_lists)
        self.assertIn("set_tests_properties(MinecraftClone.ServerOnlyBuild PROPERTIES TIMEOUT 120)", cmake_lists)


if __name__ == "__main__":
    unittest.main()
