"""Boundary contracts for pre-finalization publisher checks."""

from __future__ import annotations

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

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "repository"
        self.root.mkdir()
        subprocess.run(
            [
                "git",
                "checkout-index",
                "--all",
                f"--prefix={self.root.resolve().as_posix()}/",
            ],
            cwd=REPOSITORY,
            check=True,
            text=True,
            capture_output=True,
        )
        subprocess.run(["git", "init", "--quiet"], cwd=self.root, check=True)
        subprocess.run(["git", "config", "user.email", "candidate@example.invalid"], cwd=self.root, check=True)
        subprocess.run(["git", "config", "user.name", "Candidate Validation"], cwd=self.root, check=True)
        for relative in ("publish.py", "script/infrastructure_checks.py"):
            source = REPOSITORY / relative
            target = self.root / relative
            shutil.copy2(source, target)
        project_info = self.root / "src/shared/include/shared/ProjectInfo.hpp"
        patch_match = re.search(r"\.patch = (\d+),", project_info.read_text(encoding="utf-8"))
        self.assertIsNotNone(patch_match)
        self.snapshot_index = int(patch_match.group(1))
        history = self.root / "docs/version_history/EarlyDev 0.1/EarlyDev 0.1.0 Initiation.md"
        undated, replacements = re.subn(
            rf"^## EarlyDev 0\.1\.0:{self.snapshot_index}(?:\(\d{{2}}\.\d{{2}}\.\d{{2}}\))?$",
            f"## EarlyDev 0.1.0:{self.snapshot_index}",
            history.read_text(encoding="utf-8"),
            count=1,
            flags=re.MULTILINE,
        )
        self.assertEqual(replacements, 1)
        history.write_text(undated, encoding="utf-8")
        subprocess.run(["git", "add", "--all"], cwd=self.root, check=True)
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
            cwd=self.root,
            check=True,
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

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

    def test_snapshot_artifact_workflow_reserves_private_dependencies_for_finalized_sources(self) -> None:
        ai_checks = AI_CHECKS_WORKFLOW.read_text(encoding="utf-8")
        snapshot = SNAPSHOT_WORKFLOW.read_text(encoding="utf-8")

        self.assertEqual(ai_checks.count("tzdata==2025.2"), 1)
        self.assertEqual(snapshot.count("tzdata==2025.2"), 2)
        self.assertIn("if: github.ref != 'refs/heads/ai-main'", ai_checks)
        self.assertIn("if: github.ref == 'refs/heads/ai-main'", ai_checks)
        self.assertIn("source-checks --pre-finalization-candidate", ai_checks)
        self.assertNotIn("codex/ai-release-", snapshot)
        self.assertNotIn("source-checks --pre-finalization-candidate", snapshot)
        self.assertIn("run: python script/ci/acquire.py source-checks", snapshot)


if __name__ == "__main__":
    unittest.main()
