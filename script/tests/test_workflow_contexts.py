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

    def test_windows_key_acl_is_current_user_only_and_fail_closed(self):
        for workflow_path in WORKFLOWS:
            workflow = workflow_path.read_text(encoding="utf-8")
            self.assertNotIn("$env:USERNAME:(OI)(CI)(R,W)", workflow, workflow_path)
            self.assertIn("$currentUser = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name", workflow, workflow_path)
            self.assertIn("$grant = '{0}:F' -f $currentUser", workflow, workflow_path)
            self.assertIn("/inheritance:r /grant:r $grant", workflow, workflow_path)
            self.assertIn("if ($LASTEXITCODE -ne 0)", workflow, workflow_path)

    def test_artifact_uploads_never_include_private_sources(self):
        for workflow_path in WORKFLOWS:
            workflow = workflow_path.read_text(encoding="utf-8")
            for upload in workflow.split("path: |")[1:]:
                self.assertNotIn("private-dependencies", upload.split("if-no-files-found", maxsplit=1)[0], workflow_path)


if __name__ == "__main__":
    unittest.main()
