"""Contracts for the artifact-only GitHub Actions workflow."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
ARTIFACT_WORKFLOW = REPOSITORY / ".github/workflows/snapshot-artifacts.yml"
ORDINARY_CHECKS_WORKFLOW = REPOSITORY / ".github/workflows/ai-checks.yml"
UPLOAD_ARTIFACT_REVISION = "ea165f8d65b6e75b540449e92b4886f43607fa02"


def extract_jobs(workflow: str) -> dict[str, str]:
    jobs_section = workflow.split("\njobs:\n", maxsplit=1)[1]
    headers = list(re.finditer(r"(?m)^  ([a-z][a-z0-9_-]*):\s*$", jobs_section))
    return {
        match.group(1): jobs_section[
            match.end() : headers[index + 1].start()
            if index + 1 < len(headers)
            else len(jobs_section)
        ]
        for index, match in enumerate(headers)
    }


class WorkflowContextTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflow = ARTIFACT_WORKFLOW.read_text(encoding="utf-8")
        cls.jobs = extract_jobs(cls.workflow)

    def test_only_source_and_platform_artifact_jobs_exist(self):
        self.assertFalse(ORDINARY_CHECKS_WORKFLOW.exists())
        self.assertEqual(set(self.jobs), {"source", "desktop", "android"})
        self.assertIn("platform: macos", self.jobs["desktop"])
        self.assertIn("platform: windows", self.jobs["desktop"])
        self.assertNotIn("platform: android", self.jobs["desktop"])

    def test_artifact_builds_require_explicit_manual_dispatch(self):
        triggers = self.workflow.split("\npermissions:\n", 1)[0]
        self.assertNotIn("  push:", triggers)
        self.assertNotIn("  pull_request:", triggers)
        self.assertNotIn("  schedule:", triggers)
        self.assertEqual(re.findall(r"(?m)^  ([a-z_]+):", triggers), ["workflow_dispatch"])
        manual = triggers.split("  workflow_dispatch:\n", 1)[1]
        for field in ("source_ref", "version"):
            field_block = re.split(r"(?m)^      \w+:", manual.split(f"      {field}:\n", 1)[1], maxsplit=1)[0]
            self.assertIn("required: true", field_block)
        manual_trust = self.jobs["source"].split("workflow_dispatch)", 1)[1].split(";;", 1)[0]
        self.assertIn('git merge-base --is-ancestor "$source_commit" refs/remotes/origin/ai-main', manual_trust)

    def test_artifact_workflow_contains_no_automated_test_or_analysis_gates(self):
        forbidden = re.compile(
            r"(?i)\b(?:ctest|pytest|unittest|ai_check|ai_analysis_matrix|"
            r"clang_tidy|clang-tidy|cppcheck|include-what-you-use|iwyu|"
            r"analysis|analyzer|hwasan|asan|tsan|lsan|sanitizer|emulator|adb)\b|"
            r"verify_analysis_gate|--wait\b|poll-interval|timeout-seconds"
        )
        self.assertIsNone(forbidden.search(self.workflow))
        self.assertNotIn("GH_TOKEN", self.workflow)
        self.assertNotIn("checks:", self.workflow)
        self.assertNotIn("actions:", self.workflow)

    def test_source_job_keeps_exact_identity_and_trust_with_bounded_runtime(self):
        source = self.jobs["source"]
        permissions = self.workflow.split("permissions:\n", maxsplit=1)[1].split(
            "\njobs:\n", maxsplit=1
        )[0]
        self.assertEqual(permissions, "  contents: read\n")
        self.assertIn("timeout-minutes: 15", source)
        self.assertIn('source_commit="$(git rev-parse HEAD)"', source)
        self.assertIn("git merge-base --is-ancestor", source)
        self.assertIn("refs/remotes/origin/ai-dev", source)
        self.assertIn("refs/remotes/origin/ai-main", source)
        self.assertIn("refs/tags/ai/*", source)
        self.assertIn("test \"$trusted\" = true", source)
        self.assertIn("ref: ${{ inputs.source_ref || github.sha }}", source)
        self.assertIn("python script/ci/build_snapshot.py source-identity", source)
        self.assertIn('--expected "${{ steps.trust.outputs.source_commit }}"', source)
        self.assertNotIn("secrets.", source)
        self.assertNotIn("verify_analysis_gate", source)

    def test_platform_jobs_are_bound_to_trusted_immutable_source(self):
        expected_guard = "needs.source.outputs.trusted_dependency_source == 'true'"
        self.assertIn(f"if: {expected_guard}", self.jobs["desktop"])
        self.assertIn(f"if: {expected_guard}", self.jobs["android"])
        self.assertEqual(
            self.workflow.count("ref: ${{ needs.source.outputs.source_commit }}"),
            2,
        )
        for job_name in ("desktop", "android"):
            with self.subTest(job=job_name):
                job = self.jobs[job_name]
                self.assertIn("fetch-private-dependencies", job)
                self.assertIn("install-private-dependencies", job)
                self.assertLess(
                    job.index("Materialize private dependency deploy keys"),
                    job.index("Fetch exact private dependency commits"),
                )

        windows_keys = self.jobs["desktop"].split(
            "      - name: Materialize private dependency deploy keys (Windows)\n", 1
        )[1].split("      - name:", 1)[0]
        self.assertIn("/inheritance:r /grant:r $grant", windows_keys)
        self.assertIn("ssh-keygen.exe -y -f $keyPath", windows_keys)

    def test_desktop_jobs_only_build_package_and_integrity_check_release_files(self):
        desktop = self.jobs["desktop"]
        for expected in (
            "-DBUILD_TESTING=OFF",
            "cmake --build --preset release",
            "cmake --install",
            "python script/ci/build_snapshot.py package-desktop",
            "--automated-verification build-only",
            "python script/ci/build_snapshot.py write-checksum",
            "verify-private-dependency-artifact-exclusion",
            "minecraftclone-${VERSION_SLUG}-macos-arm64.tar.gz",
            "minecraftclone-%VERSION_SLUG%-windows-x86_64.zip",
        ):
            with self.subTest(contract=expected):
                self.assertIn(expected, desktop)
        self.assertNotIn("--automated-verification passed", desktop)
        self.assertIn(
            "snapshot-${{ matrix.platform }}-${{ needs.source.outputs.version_slug }}-"
            "${{ needs.source.outputs.source_commit }}",
            desktop,
        )
        self.assertIn("if-no-files-found: error", desktop)
        self.assertNotIn("release-tests-", desktop)

    def test_android_job_builds_and_uploads_only_the_development_apk_bundle(self):
        android = self.jobs["android"]
        for expected in (
            ":app:assembleDebug",
            "minecraftclone-${VERSION_SLUG}-android-arm64-v8a-ci-development.apk",
            "python script/package_snapshot.py android",
            "--automated-verification build-only",
            "license-archive",
            "verify-private-dependency-artifact-exclusion",
            "minecraftclone-${VERSION_SLUG}-android-arm64-v8a-ci-development.apk.sha256",
            "minecraftclone-${VERSION_SLUG}-android-licenses.zip.sha256",
            "snapshot-android-${{ needs.source.outputs.version_slug }}-${{ needs.source.outputs.source_commit }}",
            "if-no-files-found: error",
        ):
            with self.subTest(contract=expected):
                self.assertIn(expected, android)
        self.assertNotIn(":app:test", android)
        self.assertNotIn("--automated-verification passed", android)

    def test_artifact_uploads_are_pinned_and_exclude_private_sources(self):
        revisions = re.findall(
            r"uses:\s+actions/upload-artifact@([^\s#]+)", self.workflow
        )
        self.assertEqual(revisions, [UPLOAD_ARTIFACT_REVISION, UPLOAD_ARTIFACT_REVISION])
        for job_name in ("desktop", "android"):
            with self.subTest(job=job_name):
                uploads = self.jobs[job_name].split("      - name: Upload ", maxsplit=1)[1:]
                self.assertTrue(uploads)
                for upload in uploads:
                    paths = upload.split("          path: |\n", maxsplit=1)[1].split(
                        "          if-no-files-found:", maxsplit=1
                    )[0]
                    self.assertIn("dist/*", paths)
                    self.assertNotIn("private-dependencies", paths)

    def test_normal_build_tooling_and_binary_cache_remain_configured(self):
        for expected in (
            "Install pinned CMake",
            "Acquire pinned Ninja",
            "Acquire pinned vcpkg",
            "Install pinned vcpkg manifest dependencies",
            "Restore vcpkg binary cache",
            "Save vcpkg binary cache",
            "Acquire pinned Vulkan SDK",
            "Bootstrap and acquire pinned Android SDK components",
        ):
            with self.subTest(setup=expected):
                self.assertIn(expected, self.workflow)
        self.assertIn("VCPKG_DEFAULT_BINARY_CACHE", self.workflow)


if __name__ == "__main__":
    unittest.main()
