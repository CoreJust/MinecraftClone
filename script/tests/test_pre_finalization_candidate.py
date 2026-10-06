"""Boundary contracts for pre-finalization publisher checks."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from datetime import date, datetime, timezone
from pathlib import Path
from unittest import mock

from script import infrastructure_checks
from script.ci import acquire


REPOSITORY = Path(__file__).resolve().parents[2]
AI_CHECKS_WORKFLOW = REPOSITORY / ".github/workflows/ai-checks.yml"
SNAPSHOT_WORKFLOW = REPOSITORY / ".github/workflows/snapshot-artifacts.yml"


class PreFinalizationCandidateTests(unittest.TestCase):
    """Exercise the publisher against an unfinalized, clean source checkout."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name) / "repository"
        cls.root.mkdir()
        subprocess.run(
            [
                "git",
                "checkout-index",
                "--all",
                f"--prefix={cls.root.resolve().as_posix()}/",
            ],
            cwd=REPOSITORY,
            check=True,
            text=True,
            capture_output=True,
        )
        subprocess.run(["git", "init", "--quiet"], cwd=cls.root, check=True)
        # This fixture owns its temporary repository. Keep Git's background
        # maintenance from racing TemporaryDirectory.cleanup after commits.
        for key, value in (("gc.auto", "0"), ("maintenance.auto", "false")):
            subprocess.run(["git", "config", key, value], cwd=cls.root, check=True)
        subprocess.run(["git", "config", "user.email", "candidate@example.invalid"], cwd=cls.root, check=True)
        subprocess.run(["git", "config", "user.name", "Candidate Validation"], cwd=cls.root, check=True)
        for relative in ("publish.py", "script/infrastructure_checks.py"):
            source = REPOSITORY / relative
            target = cls.root / relative
            shutil.copy2(source, target)
        project_info = cls.root / "src/shared/include/shared/ProjectInfo.hpp"
        patch_match = re.search(r"\.patch = (\d+),", project_info.read_text(encoding="utf-8"))
        if patch_match is None:
            raise AssertionError("ProjectInfo.hpp does not contain a patch version")
        cls.snapshot_index = int(patch_match.group(1))
        history_relative = "docs/version_history/EarlyDev 0.1/EarlyDev 0.1.0 Initiation.md"
        history = cls.root / history_relative
        undated, replacements = re.subn(
            rf"^## EarlyDev 0\.1\.0:{cls.snapshot_index}(?:\(\d{{2}}\.\d{{2}}\.\d{{2}}\))?$",
            f"## EarlyDev 0.1.0:{cls.snapshot_index}",
            history.read_text(encoding="utf-8"),
            count=1,
            flags=re.MULTILINE,
        )
        if replacements != 1:
            raise AssertionError("current snapshot heading was not found exactly once")
        history.write_text(undated, encoding="utf-8")
        cls.fixture_baselines = {
            history_relative: history.read_bytes(),
            "vcpkg.json": (cls.root / "vcpkg.json").read_bytes(),
        }
        subprocess.run(["git", "add", "--all"], cwd=cls.root, check=True)
        subprocess.run(
            [
                "git",
                "commit",
                "--quiet",
                "--no-gpg-sign",
                "--allow-empty",
                "-m",
                "candidate validation fixture",
            ],
            cwd=cls.root,
            check=True,
        )

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def setUp(self) -> None:
        status = subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=self.root,
            check=True,
            text=True,
            capture_output=True,
        ).stdout
        self.assertEqual(status, "", "shared fixture must start clean")

    def tearDown(self) -> None:
        changed_files = []
        for relative, baseline in self.fixture_baselines.items():
            path = self.root / relative
            if path.read_bytes() != baseline:
                path.write_bytes(baseline)
                changed_files.append(relative)

        if changed_files:
            subprocess.run(["git", "add", "--", *changed_files], cwd=self.root, check=True)
            staged = subprocess.run(
                ["git", "diff", "--cached", "--quiet"],
                cwd=self.root,
                check=False,
                text=True,
                capture_output=True,
            )
            if staged.returncode == 1:
                subprocess.run(
                    ["git", "commit", "--quiet", "--no-gpg-sign", "-m", "restore candidate fixture"],
                    cwd=self.root,
                    check=True,
                )
            else:
                self.assertEqual(staged.returncode, 0, staged.stderr)

        status = subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=self.root,
            check=True,
            text=True,
            capture_output=True,
        ).stdout
        self.assertEqual(status, "", "shared fixture must be clean after each test")

    def test_owned_temp_repo_disables_background_git_maintenance(self) -> None:
        config = subprocess.run(
            ["git", "config", "--get-regexp", r"^(gc\.auto|maintenance\.auto)$"],
            cwd=self.root,
            check=True,
            text=True,
            capture_output=True,
        ).stdout.splitlines()
        self.assertEqual(config, ["gc.auto 0", "maintenance.auto false"])

    def run_publish(self, *extra: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                "publish.py",
                "EarlyDev:Initiation",
                f"0.1.0:{self.snapshot_index}",
                "--checks-only",
                *extra,
            ],
            cwd=self.root,
            text=True,
            capture_output=True,
        )

    def commit_history(self, content: str) -> None:
        history = self.root / "docs/version_history/EarlyDev 0.1/EarlyDev 0.1.0 Initiation.md"
        preceding_snapshots = "".join(
            f"\n## EarlyDev 0.1.0:{index}(26.09.10)\nSnapshot {index}\n"
            for index in range(3, self.snapshot_index)
        )
        history.write_text(content + preceding_snapshots, encoding="utf-8")
        subprocess.run(["git", "add", str(history.relative_to(self.root))], cwd=self.root, check=True)
        subprocess.run(
            ["git", "commit", "--quiet", "--no-gpg-sign", "-m", "candidate history fixture"],
            cwd=self.root,
            check=True,
        )

    def commit_at(self, timestamp: str) -> str:
        environment = os.environ | {
            "GIT_AUTHOR_DATE": timestamp,
            "GIT_COMMITTER_DATE": timestamp,
        }
        subprocess.run(
            ["git", "commit", "--allow-empty", "--quiet", "--no-gpg-sign", "-m", "timestamp fixture"],
            cwd=self.root,
            env=environment,
            check=True,
        )
        return subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=self.root, check=True, text=True, capture_output=True
        ).stdout.strip()

    def test_allows_only_absent_current_snapshot(self) -> None:
        ordinary = self.run_publish()
        self.assertNotEqual(ordinary.returncode, 0)
        self.assertIn(f"missing snapshots: [{self.snapshot_index}]", ordinary.stdout)

        candidate = self.run_publish("--pre-finalization-candidate")
        self.assertEqual(candidate.returncode, 0, candidate.stdout + candidate.stderr)

    def test_rejects_malformed_existing_history(self) -> None:
        self.commit_history(
            "# Overview\n"
            "Fixture\n\n"
            "# Snapshots\n"
            "## EarlyDev 0.1.0:1(26.09.09)\n"
            "\n"
            "## EarlyDev 0.1.0:2(26.09.10)\n"
            "Second\n"
        )

        candidate = self.run_publish("--pre-finalization-candidate")
        self.assertNotEqual(candidate.returncode, 0)
        self.assertIn("snapshot 1 is empty", candidate.stdout)

    def test_rejects_existing_snapshot_dates_out_of_order(self) -> None:
        self.commit_history(
            "# Overview\n"
            "Fixture\n\n"
            "# Snapshots\n"
            "## EarlyDev 0.1.0:1(26.09.09)\n"
            "First\n\n"
            "## EarlyDev 0.1.0:2(26.09.08)\n"
            "Second\n"
        )

        candidate = self.run_publish("--pre-finalization-candidate")
        self.assertNotEqual(candidate.returncode, 0)
        self.assertIn("earlier than previous", candidate.stdout)

    def test_rejects_invalid_existing_snapshot_date(self) -> None:
        self.commit_history(
            "# Overview\n"
            "Fixture\n\n"
            "# Snapshots\n"
            "## EarlyDev 0.1.0:1(26.13.09)\n"
            "First\n\n"
            "## EarlyDev 0.1.0:2(26.09.10)\n"
            "Second\n"
        )

        candidate = self.run_publish("--pre-finalization-candidate")
        self.assertNotEqual(candidate.returncode, 0)
        self.assertIn("snapshot 1 has invalid date", candidate.stdout)

    def test_rejects_unrelated_source_policy_failures(self) -> None:
        vcpkg = self.root / "vcpkg.json"
        vcpkg.write_text(vcpkg.read_text(encoding="utf-8").replace('"version-string": "0.1.0"', '"version-string": "9.9.9"'), encoding="utf-8")
        subprocess.run(["git", "add", "vcpkg.json"], cwd=self.root, check=True)
        subprocess.run(
            ["git", "commit", "--quiet", "--no-gpg-sign", "-m", "invalid version fixture"],
            cwd=self.root,
            check=True,
        )

        candidate = self.run_publish("--pre-finalization-candidate")
        self.assertNotEqual(candidate.returncode, 0)
        self.assertIn("vcpkg.json version-string matches", candidate.stdout)

    def test_ci_helper_forwards_the_candidate_flag_only_when_requested(self) -> None:
        with mock.patch.object(acquire, "run") as run:
            acquire.source_checks()
            ordinary = [call.args[0] for call in run.call_args_list]
        with mock.patch.object(acquire, "run") as run:
            acquire.source_checks(pre_finalization_candidate=True)
            candidate = [call.args[0] for call in run.call_args_list]

        self.assertEqual(ordinary[0], [sys.executable, "script/ai_check.py", "--fast"])
        self.assertNotIn("--pre-finalization-candidate", ordinary[1])
        self.assertEqual(candidate[0], ordinary[0])
        self.assertEqual(candidate[1][:-1], ordinary[1])
        self.assertEqual(candidate[1][-1], "--pre-finalization-candidate")

    def test_ci_helper_binds_only_the_resolved_finalized_source(self) -> None:
        checked_out_head = "a" * 40
        resolved_source = "b" * 40
        with mock.patch.object(acquire, "run", side_effect=("", resolved_source, "")) as run:
            acquire.source_checks(source_commit=checked_out_head)

        commands = [call.args[0] for call in run.call_args_list]
        self.assertEqual(commands[0], [sys.executable, "script/ai_check.py", "--fast"])
        self.assertEqual(
            commands[1][-3:],
            ["resolve-date-source", "--expected-head", checked_out_head],
        )
        self.assertEqual(
            commands[2][-2:],
            ["--snapshot-source-commit", resolved_source],
        )
        self.assertNotIn(checked_out_head, commands[2])

    def test_ci_helper_keeps_pre_finalization_source_checks_unbound(self) -> None:
        with mock.patch.object(acquire, "run") as run:
            acquire.source_checks(pre_finalization_candidate=True)

        self.assertEqual(len(run.call_args_list), 2)
        self.assertEqual(run.call_args_list[1].args[0][-1], "--pre-finalization-candidate")

        with mock.patch.object(acquire, "run") as run:
            with self.assertRaisesRegex(acquire.CiError, "cannot bind a finalized source"):
                acquire.source_checks(pre_finalization_candidate=True, source_commit="a" * 40)
        run.assert_not_called()

    def test_project_date_uses_belgrade_midnight_on_every_runner(self) -> None:
        before_midnight = datetime(2026, 9, 11, 21, 59, tzinfo=timezone.utc)
        at_midnight = datetime(2026, 9, 11, 22, 0, tzinfo=timezone.utc)

        self.assertEqual(infrastructure_checks.project_date(before_midnight).isoformat(), "2026-09-11")
        self.assertEqual(infrastructure_checks.project_date(at_midnight).isoformat(), "2026-09-12")

    def test_project_date_rejects_runner_local_naive_time(self) -> None:
        with self.assertRaisesRegex(ValueError, "aware datetime"):
            infrastructure_checks.project_date(datetime(2026, 9, 12))

    def test_snapshot_date_still_rejects_a_project_date_mismatch(self) -> None:
        context = {"_snapshots": [(4, "26.09.12")], "snapshot_index": 4}
        with mock.patch.object(infrastructure_checks, "project_date", return_value=date(2026, 9, 13)):
            passed, message = infrastructure_checks.check_today_date(context)

        self.assertFalse(passed)
        self.assertEqual(message, "snapshot date is 26.09.12, but today is 26.09.13")

    def test_source_date_binding_survives_a_later_project_day(self) -> None:
        context = {
            "_snapshots": [(4, "26.10.06")],
            "snapshot_index": 4,
            "snapshot_source_date": date(2026, 10, 6),
        }
        with mock.patch.object(infrastructure_checks, "project_date", return_value=date(2026, 10, 7)):
            passed, message = infrastructure_checks.check_today_date(context)

        self.assertTrue(passed, message)

    def test_source_date_binding_rejects_a_heading_that_disagrees(self) -> None:
        context = {
            "_snapshots": [(4, "26.10.06")],
            "snapshot_index": 4,
            "snapshot_source_date": date(2026, 10, 7),
        }
        passed, message = infrastructure_checks.check_today_date(context)

        self.assertFalse(passed)
        self.assertEqual(message, "snapshot date is 26.10.06, but source commit date is 26.10.07")

    def test_publisher_rejects_malformed_or_missing_snapshot_sources(self) -> None:
        malformed = self.run_publish("--snapshot-source-commit", "not-a-commit")
        self.assertNotEqual(malformed.returncode, 0)
        self.assertIn("full immutable commit ID", malformed.stdout)

        missing = self.run_publish("--snapshot-source-commit", "f" * 40)
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn("snapshot source commit does not exist", missing.stdout)

    def test_commit_project_date_uses_europe_belgrade_at_utc_midnight(self) -> None:
        before_belgrade_midnight = self.commit_at("2026-10-06T21:59:00+00:00")
        at_belgrade_midnight = self.commit_at("2026-10-06T22:00:00+00:00")

        self.assertEqual(
            infrastructure_checks.commit_project_date(self.root, before_belgrade_midnight),
            date(2026, 10, 6),
        )
        self.assertEqual(
            infrastructure_checks.commit_project_date(self.root, at_belgrade_midnight),
            date(2026, 10, 7),
        )

    def test_commit_project_date_rejects_malformed_and_missing_sources(self) -> None:
        with self.assertRaisesRegex(ValueError, "full immutable commit ID"):
            infrastructure_checks.commit_project_date(self.root, "not-a-commit")
        with self.assertRaisesRegex(ValueError, "does not exist"):
            infrastructure_checks.commit_project_date(self.root, "f" * 40)

    def test_snapshot_artifact_workflow_reserves_private_dependencies_for_finalized_sources(self) -> None:
        ai_checks = AI_CHECKS_WORKFLOW.read_text(encoding="utf-8")
        snapshot = SNAPSHOT_WORKFLOW.read_text(encoding="utf-8")

        self.assertEqual(ai_checks.count("tzdata==2025.2"), 2)
        self.assertEqual(snapshot.count("tzdata==2025.2"), 2)
        self.assertIn("if: github.ref != 'refs/heads/ai-main'", ai_checks)
        self.assertIn("if: github.ref == 'refs/heads/ai-main'", ai_checks)
        self.assertIn("source-checks --pre-finalization-candidate", ai_checks)
        self.assertIn("SOURCE_COMMIT: ${{ github.sha }}", ai_checks)
        cross_platform_source_arg = 'source-checks --source-commit "${{ env.SOURCE_COMMIT }}"'
        self.assertIn(cross_platform_source_arg, ai_checks)
        fast_job = ai_checks.split("  desktop:", 1)[0]
        self.assertNotIn("fetch-depth: 0", fast_job)
        desktop_job = ai_checks.split("  desktop:", 1)[1].split("\n  linux-analysis:", 1)[0]
        self.assertIn("fetch-depth: 0", desktop_job)
        self.assertNotIn("codex/ai-release-", snapshot)
        self.assertNotIn("source-checks --pre-finalization-candidate", snapshot)
        self.assertEqual(snapshot.count("fetch-depth: 0"), 3)
        self.assertEqual(snapshot.count(cross_platform_source_arg), 2)


if __name__ == "__main__":
    unittest.main()
