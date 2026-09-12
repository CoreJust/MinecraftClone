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
            self.assertIn("-DCMAKE_BUILD_TYPE=@CMAKE_BUILD_TYPE@", contents)
            self.assertNotIn("-DCMAKE_BUILD_TYPE=Debug", contents)

    def test_server_only_build_test_has_platform_timeout(self):
        cmake_lists = (REPOSITORY / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("set_tests_properties(MinecraftClone.ServerOnlyBuild PROPERTIES TIMEOUT 180)", cmake_lists)
        self.assertIn("set_tests_properties(MinecraftClone.ServerOnlyBuild PROPERTIES TIMEOUT 120)", cmake_lists)


if __name__ == "__main__":
    unittest.main()
