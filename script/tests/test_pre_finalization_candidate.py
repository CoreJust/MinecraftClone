"""Boundary contracts for pre-finalization publisher checks."""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from script.ci import acquire


REPOSITORY = Path(__file__).resolve().parents[2]
AI_CHECKS_WORKFLOW = REPOSITORY / ".github/workflows/ai-checks.yml"
SNAPSHOT_WORKFLOW = REPOSITORY / ".github/workflows/snapshot-artifacts.yml"


class PreFinalizationCandidateTests(unittest.TestCase):
    """Exercise the publisher against an unfinalized, clean source checkout."""

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "repository"
        subprocess.run(
            ["git", "clone", "--quiet", "--no-local", str(REPOSITORY), str(self.root)],
            check=True,
            text=True,
            capture_output=True,
        )
        subprocess.run(["git", "config", "user.email", "candidate@example.invalid"], cwd=self.root, check=True)
        subprocess.run(["git", "config", "user.name", "Candidate Validation"], cwd=self.root, check=True)
        for relative in ("publish.py", "script/infrastructure_checks.py"):
            source = REPOSITORY / relative
            target = self.root / relative
            shutil.copy2(source, target)
        subprocess.run(
            ["git", "add", "publish.py", "script/infrastructure_checks.py"],
            cwd=self.root,
            check=True,
        )
        subprocess.run(
            ["git", "commit", "--quiet", "--no-gpg-sign", "-m", "candidate validation fixture"],
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
                "0.1.0:3",
                "--checks-only",
                *extra,
            ],
            cwd=self.root,
            text=True,
            capture_output=True,
        )

    def commit_history(self, content: str) -> None:
        history = self.root / "docs/version_history/EarlyDev 0.1/EarlyDev 0.1.0 Initiation.md"
        history.write_text(content, encoding="utf-8")
        subprocess.run(["git", "add", str(history.relative_to(self.root))], cwd=self.root, check=True)
        subprocess.run(
            ["git", "commit", "--quiet", "--no-gpg-sign", "-m", "candidate history fixture"],
            cwd=self.root,
            check=True,
        )

    def test_allows_only_absent_current_snapshot(self) -> None:
        ordinary = self.run_publish()
        self.assertNotEqual(ordinary.returncode, 0)
        self.assertIn("missing snapshots: [3]", ordinary.stdout)

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

    def test_hosted_workflows_limit_the_flag_to_pre_finalization_candidates(self) -> None:
        ai_checks = AI_CHECKS_WORKFLOW.read_text(encoding="utf-8")
        snapshot = SNAPSHOT_WORKFLOW.read_text(encoding="utf-8")

        self.assertIn("if: github.ref != 'refs/heads/ai-main'", ai_checks)
        self.assertIn("if: github.ref == 'refs/heads/ai-main'", ai_checks)
        self.assertIn("source-checks --pre-finalization-candidate", ai_checks)
        self.assertIn("startsWith(github.ref, 'refs/heads/codex/ai-release-')", snapshot)
        self.assertIn("source-checks --pre-finalization-candidate", snapshot)
        self.assertIn("run: python script/ci/acquire.py source-checks", snapshot)


if __name__ == "__main__":
    unittest.main()
