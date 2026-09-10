"""Contracts for immutable snapshot build and artifact evidence orchestration."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from script.ci import build_snapshot


REPOSITORY = Path(__file__).resolve().parents[2]
WORKFLOW = REPOSITORY / ".github/workflows/snapshot-artifacts.yml"


class BuildSnapshotTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def write(self, relative: str, content: bytes) -> Path:
        target = self.root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
        return target

    def write_project_version(self, version=(0, 1, 0, 3)) -> None:
        epoch, major, minor, patch = version
        self.write(
            "src/shared/include/shared/ProjectInfo.hpp",
            (
                "constexpr Version PROJECT_VERSION{\n"
                f"    .epoch = {epoch}, .major = {major},\n"
                f"    .minor = {minor}, .patch = {patch},\n"
                "};\n"
            ).encode(),
        )

    def test_source_identity_records_one_exact_commit_and_rejects_version_drift(self):
        self.write_project_version()
        output = self.root / "build/source.json"
        github_output = self.root / "github-output"
        commit = "a" * 40
        tree = "b" * 40
        with mock.patch.object(build_snapshot, "run", side_effect=(commit, tree)):
            result = build_snapshot.write_source_identity(
                self.root,
                commit,
                "ai-main",
                "0.1.0:3",
                output,
                github_output,
            )
        self.assertEqual(result["source_commit"], commit)
        self.assertEqual(result["source_tree"], tree)
        self.assertEqual(result["version_slug"], "0.1.0-3")
        self.assertEqual(
            github_output.read_text(),
            f"source_commit={commit}\nversion=0.1.0:3\nversion_slug=0.1.0-3\n",
        )
        with self.assertRaisesRegex(build_snapshot.SnapshotBuildError, "does not match source version"):
            build_snapshot.write_source_identity(self.root, None, None, "0.1.0:4", output, None)

    def test_license_material_has_unique_project_dependency_and_pinned_vendor_names(self):
        project_license = self.write("LICENSE", b"project")
        self.write("installed/arm64-osx/share/fmt/copyright", b"fmt")
        self.write("installed/duplicate/share/fmt/copyright", b"fmt")
        self.write("installed/arm64-osx/share/volk/copyright", b"volk")

        def download(_url: str, digest: str, output: Path) -> None:
            output.write_text(digest, encoding="utf-8")

        destination = self.root / "licenses"
        with mock.patch.object(build_snapshot, "download_verified", side_effect=download) as verified:
            build_snapshot.prepare_licenses(project_license, self.root / "installed", destination, True)
        self.assertEqual(verified.call_count, 2)
        self.assertEqual(
            sorted(path.relative_to(destination).as_posix() for path in destination.rglob("*") if path.is_file()),
            [
                "MinecraftClone-LICENSE.txt",
                "vcpkg/fmt-copyright.txt",
                "vcpkg/volk-copyright.txt",
                "vulkan/MoltenVK-LICENSE.txt",
                "vulkan/Vulkan-Loader-LICENSE.txt",
            ],
        )

    def test_license_archive_is_deterministic(self):
        licenses = self.root / "licenses"
        self.write("licenses/project.txt", b"project")
        self.write("licenses/dependencies/fmt.txt", b"fmt")
        first = self.root / "first.zip"
        second = self.root / "second.zip"
        build_snapshot.write_license_archive(licenses, first)
        build_snapshot.write_license_archive(licenses, second)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        with zipfile.ZipFile(first) as archive:
            self.assertEqual(archive.namelist(), ["dependencies/fmt.txt", "project.txt"])
            self.assertTrue(all(item.date_time == (1980, 1, 1, 0, 0, 0) for item in archive.infolist()))

    def test_windows_runtime_closure_packages_recursive_non_system_dependencies(self):
        executable = self.write("install/mc_main.exe", b"exe")
        first = self.write("runtime/first.dll", b"first")
        second = self.write("runtime/second.dll", b"second")

        def dependencies(binary: Path) -> list[str]:
            if binary == executable.resolve():
                return ["KERNEL32.dll", "first.dll"]
            if binary == first.resolve():
                return ["second.dll"]
            return []

        with mock.patch.object(build_snapshot, "windows_dependencies", side_effect=dependencies), mock.patch.object(
            build_snapshot,
            "windows_system_dll",
            side_effect=lambda name: name.lower() == "kernel32.dll",
        ):
            result = build_snapshot.resolve_windows_runtime(executable, [self.root / "runtime"])
        self.assertEqual(result, [first.resolve(), second.resolve()])

    def test_macos_package_call_uses_versioned_loader_and_one_license_tree(self):
        install_root = self.root / "install"
        self.write("install/mc_main", b"executable")
        self.write("install/shaders/grid.vert.spv", b"shader")
        self.write("installed/arm64-osx/share/fmt/copyright", b"fmt")
        project_license = self.write("LICENSE", b"project")
        packager = self.write("script/package_snapshot.py", b"fixture")
        evidence = self.write("build/toolchain.json", b"{}")
        sdk_root = self.root / "sdk"
        arguments = argparse.Namespace(
            platform="macos",
            install_root=install_root,
            vcpkg_installed=self.root / "installed",
            runtime_search=[],
            sdk_root=sdk_root,
            project_license=project_license,
            packager=packager,
            toolchain_evidence=evidence,
            work_root=self.root / "work",
            version="0.1.0:3",
            source_commit="d" * 40,
            output=self.root / "dist/mac.tar.gz",
        )

        def download(_url: str, _digest: str, output: Path) -> None:
            output.write_bytes(b"vendor license")

        with mock.patch.object(build_snapshot, "download_verified", side_effect=download), mock.patch.object(
            build_snapshot,
            "run",
        ) as command:
            build_snapshot.package_desktop(arguments)
        invocation = command.call_args.args[0]
        self.assertEqual(invocation.count("--license"), 1)
        loader_index = invocation.index("--vulkan-loader")
        self.assertEqual(invocation[loader_index + 1], str(sdk_root.resolve() / "lib/libvulkan.1.4.357.dylib"))
        self.assertNotIn("--runtime", invocation)

    def test_windows_system_policy_is_explicit_and_does_not_follow_runner_contents(self):
        self.assertTrue(build_snapshot.windows_system_dll("KERNEL32.dll"))
        self.assertTrue(build_snapshot.windows_system_dll("api-ms-win-core-file-l1-1-0.dll"))
        self.assertFalse(build_snapshot.windows_system_dll("vcruntime140.dll"))
        self.assertFalse(build_snapshot.windows_system_dll("vendor-runtime.dll"))

    def test_windows_toolchain_evidence_records_redist_version_policy_and_hashes(self):
        runtime = self.write(
            "VC/Redist/MSVC/14.44.35211/x64/Microsoft.VC143.CRT/vcruntime140.dll",
            b"runtime",
        )
        evidence = self.write("build/windows.json", b'{"revision":"fixture"}\n')
        build_snapshot.record_windows_runtime(evidence, [runtime])
        result = json.loads(evidence.read_text())
        self.assertEqual(result["windows_runtime"]["msvc_redist_version"], "14.44.35211")
        self.assertEqual(
            result["windows_runtime"]["files"],
            [{"name": "vcruntime140.dll", "sha256": hashlib.sha256(b"runtime").hexdigest()}],
        )
        self.assertEqual(
            result["windows_runtime"]["redistribution_policy"],
            build_snapshot.MICROSOFT_REDISTRIBUTION_POLICY,
        )

    def test_android_source_fails_before_build_when_gradle_inputs_are_incomplete(self):
        for relative in build_snapshot.ANDROID_REQUIRED_FILES:
            if relative != "app/build.gradle":
                self.write(f"android/{relative}", b"fixture")
        with self.assertRaisesRegex(build_snapshot.SnapshotBuildError, "app/build.gradle"):
            build_snapshot.verify_android_source(self.root / "android")

    def test_android_source_requires_a_hash_pinned_gradle_813_wrapper(self):
        for relative in build_snapshot.ANDROID_REQUIRED_FILES:
            self.write(f"android/{relative}", b"fixture")
        properties = self.root / "android/gradle/wrapper/gradle-wrapper.properties"
        properties.write_text(
            "distributionUrl=https\\://services.gradle.org/distributions/gradle-8.13-bin.zip\n"
            "distributionSha256Sum=not-a-digest\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(build_snapshot.SnapshotBuildError, "distribution SHA-256"):
            build_snapshot.verify_android_source(self.root / "android")

    def test_android_sdk_environment_rejects_conflicting_roots(self):
        with mock.patch.dict(
            os.environ,
            {"ANDROID_HOME": str(self.root / "old"), "ANDROID_SDK_ROOT": str(self.root / "new")},
            clear=False,
        ):
            with self.assertRaisesRegex(build_snapshot.SnapshotBuildError, "must agree"):
                build_snapshot.android_sdk_root_from_environment()

    def test_android_sdk_environment_accepts_matching_roots(self):
        with mock.patch.dict(
            os.environ,
            {"ANDROID_HOME": str(self.root), "ANDROID_SDK_ROOT": str(self.root / ".")},
            clear=False,
        ):
            self.assertEqual(build_snapshot.android_sdk_root_from_environment(), self.root.resolve())

    def test_release_evidence_never_claims_hosted_runtime_acceptance(self):
        artifact = self.write("dist/game.apk", b"apk")
        revision = "c" * 40
        metadata = self.write("build/toolchain.json", json.dumps({"revision": revision}).encode())
        output = self.root / "dist/evidence.json"
        arguments = argparse.Namespace(
            platform="android",
            configuration="development-debug",
            automated_verification="build-only",
            version="0.1.0:3",
            source_commit=revision,
            artifact=artifact,
            toolchain_evidence=metadata,
            output=output,
        )
        build_snapshot.write_release_evidence(arguments)
        evidence = json.loads(output.read_text())
        self.assertEqual(evidence["runtime"]["status"], "not-run")
        self.assertIn("development-signed APK locally", evidence["runtime"]["requirement"])
        self.assertEqual(evidence["artifact"]["sha256"], hashlib.sha256(b"apk").hexdigest())

    def test_workflow_builds_resolved_commit_without_publishing(self):
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("workflow_dispatch:", workflow)
        self.assertIn("'codex/ai-release-*'", workflow)
        self.assertIn("'ai/*/*/*'", workflow)
        self.assertIn("source_commit: ${{ steps.identity.outputs.source_commit }}", workflow)
        self.assertGreaterEqual(workflow.count("ref: ${{ needs.source.outputs.source_commit }}"), 2)
        self.assertIn("runner: macos-15", workflow)
        self.assertIn("xcode-select --switch /Applications/Xcode_26.2.app/Contents/Developer", workflow)
        self.assertIn("runner: windows-2022", workflow)
        self.assertIn("runs-on: ubuntu-24.04", workflow)
        self.assertIn("android/gradlew --no-daemon --stacktrace :app:assembleDebug", workflow)
        self.assertIn("--api-level 35 --abi arm64-v8a --signing development", workflow)
        self.assertNotIn("--abi arm64_v8a", workflow)
        self.assertEqual(workflow.count("build/toolchain-windows.json"), 3)
        self.assertNotIn(r"build\toolchain-windows.json", workflow)
        self.assertNotIn("gh release", workflow)
        self.assertNotIn("contents: write", workflow)


if __name__ == "__main__":
    unittest.main()
