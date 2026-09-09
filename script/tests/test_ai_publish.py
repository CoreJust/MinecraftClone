from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT_NAMES = ("ai_publish.py", "ai_commit.py", "ai_history.py", "ai_tasks.py")


def task(task_id: str, level: str, parent: str = "", status: str = "active", **changes: object) -> dict[str, object]:
    record: dict[str, object] = {
        "id": task_id,
        "title": task_id,
        "kind": "chore",
        "status": status,
        "priority": "P1",
        "route": "terra",
        "milestone": "Test",
        "depends_on": [],
        "acceptance": "Acceptance",
        "evidence": "passed" if status == "done" else "",
        "blocker": "publication blocked" if status == "blocked" else "",
        "owner": "Codex" if status == "active" else "",
        "level": level,
        "parent": parent,
        "motivation": "Motivation",
        "context": "Context",
        "complexity": "low",
        "created_at": "2026-09-09",
        "resolved_at": "2026-09-09" if status == "done" else "",
        "resolution_changes": "completed" if status == "done" else "",
        "plan": ["Plan"],
        "product_changes": ["Product"] if level != "basic" else [],
        "code_changes": ["Code"] if level != "basic" else [],
        "baseline_commit": "",
        "commits": [],
        "finalized": level == "snapshot" and status == "done",
        "retrospective": "",
        "docs_review": "",
        "environment_review": "",
        "backlog_review": "",
    }
    record.update(changes)
    return record


class AiPublishTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        script_dir = self.root / "script"
        script_dir.mkdir()
        for name in SCRIPT_NAMES:
            shutil.copy2(REPOSITORY / "script" / name, script_dir / name)
        (script_dir / "ai_check.py").write_text(
            "import os\n"
            "import subprocess\n"
            "from pathlib import Path\n"
            "def project_version_arguments(root):\n"
            "    return ('EarlyDev:Initiation', '0.1.0:3')\n"
            "if __name__ == '__main__':\n"
            "    mutation_root = os.environ.get('MC_TEST_PUBLISH_STAGE_DURING_CHECK')\n"
            "    if mutation_root:\n"
            "        target = Path(mutation_root) / 'src' / 'check_side_effect.txt'\n"
            "        target.write_text('not reviewed\\n', encoding='utf-8')\n"
            "        subprocess.run(['git', 'add', str(target)], cwd=mutation_root, check=True)\n"
            "    raise SystemExit(0)\n",
            encoding="utf-8",
        )
        (self.root / "docs" / "ai").mkdir(parents=True)
        (self.root / ".gitignore").write_text("__pycache__/\n", encoding="utf-8")
        source = self.root / "src" / "fixture.txt"
        source.parent.mkdir(parents=True)
        source.write_text("baseline\n", encoding="utf-8")
        (self.root / "src" / "conflict.txt").write_text("base\n", encoding="utf-8")
        self.git("init", "-q")
        self.git("config", "user.name", "Test")
        self.git("config", "user.email", "test@example.com")
        self.git("add", ".")
        self.git("commit", "--no-gpg-sign", "-q", "-m", "baseline")
        self.git("branch", "ai-main")
        self.git("checkout", "-q", "ai-main")
        (self.root / "src" / "baseline_only.txt").write_text("retain\n", encoding="utf-8")
        (self.root / "src" / "conflict.txt").write_text("ai-main\n", encoding="utf-8")
        self.git("add", "src/baseline_only.txt", "src/conflict.txt")
        self.git("commit", "--no-gpg-sign", "-q", "-m", "baseline-only work")
        self.baseline = self.git_output("rev-parse", "HEAD")
        self.git("checkout", "-q", "-b", "ai-dev", "master")
        self.write_backlog()
        source.write_text("basic\n", encoding="utf-8")
        self.git("add", "docs/ai/backlog.json", "src/fixture.txt")
        self.git("commit", "--no-gpg-sign", "-q", "-m", "basic\n\nTask-ID: MC-AI-0001")
        self.git("commit", "--allow-empty", "--no-gpg-sign", "-q", "-m", "snapshot\n\nTask-ID: MC-AI-0032")
        self.source = self.git_output("rev-parse", "HEAD")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def git(self, *arguments: str) -> None:
        subprocess.run(["git", *arguments], cwd=self.root, check=True, text=True, capture_output=True)

    def git_output(self, *arguments: str) -> str:
        return subprocess.run(
            ["git", *arguments], cwd=self.root, check=True, text=True, capture_output=True
        ).stdout.strip()

    def write_backlog(
        self,
        basic_status: str = "done",
        snapshot_status: str = "active",
        **snapshot_changes: object,
    ) -> None:
        basic = task("MC-AI-0001", "basic", "MC-AI-0032", basic_status)
        if basic_status == "active":
            basic["owner"] = "Codex"
        snapshot = task(
            "MC-AI-0032",
            "snapshot",
            "MC-AI-0033",
            snapshot_status,
            baseline_commit=self.baseline,
            finalized=True,
        )
        if snapshot_status == "active":
            snapshot.update({
                "evidence": "Local snapshot gates passed.",
                "resolution_changes": "Finalized for local promotion.",
            })
        snapshot.update(snapshot_changes)
        records = [
            task("MC-AI-0034", "major"),
            task("MC-AI-0033", "minor", "MC-AI-0034"),
            snapshot,
            basic,
        ]
        (self.root / "docs" / "ai" / "backlog.json").write_text(
            json.dumps(records, indent=2) + "\n", encoding="utf-8"
        )

    def run_publish(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, self.root / "script" / "ai_publish.py", "--root", self.root, *arguments],
            cwd=self.root,
            text=True,
            capture_output=True,
        )

    def record_pending_review(self) -> None:
        candidate = subprocess.run(
            [sys.executable, self.root / "script" / "ai_commit.py", "candidate", "MC-AI-0032"],
            cwd=self.root,
            text=True,
            capture_output=True,
            check=True,
        )
        report = json.loads(candidate.stdout)
        report.update({
            "model": "gpt-5.6-luna",
            "effort": "high",
            "verdict": "approved",
            "evidence": "Reviewed the exact pending promotion index.",
        })
        report_path = self.root / ".git" / "review.json"
        report_path.write_text(json.dumps(report), encoding="utf-8")
        subprocess.run(
            [sys.executable, self.root / "script" / "ai_commit.py", "record-review", report_path],
            cwd=self.root,
            text=True,
            capture_output=True,
            check=True,
        )

    def prepare(self) -> None:
        result = self.run_publish("prepare", "MC-AI-0032", self.source)
        self.assertEqual(result.returncode, 0, result.stderr)

    def finish(self) -> None:
        self.record_pending_review()
        result = self.run_publish("finish", "MC-AI-0032", self.source)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_snapshot_flow_requires_candidate_receipt_and_creates_immutable_tag(self) -> None:
        snapshot = json.loads((self.root / "docs" / "ai" / "backlog.json").read_text())[2]
        self.assertEqual(snapshot["status"], "active")
        self.assertEqual(snapshot["resolved_at"], "")
        self.prepare()
        (self.root / "untracked.txt").write_text("dirty\n", encoding="utf-8")
        dirty = self.run_publish("finish", "MC-AI-0032", self.source)
        self.assertNotEqual(dirty.returncode, 0)
        self.assertIn("unstaged or untracked", dirty.stderr)
        (self.root / "untracked.txt").unlink()
        missing = self.run_publish("finish", "MC-AI-0032", self.source)
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn("review receipt", missing.stderr)
        self.finish()
        promoted = self.git_output("rev-parse", "HEAD")
        self.assertEqual(self.git_output("show", "-s", "--format=%P", promoted).split(), [self.baseline, self.source])
        self.assertEqual((self.root / "src" / "baseline_only.txt").read_text(encoding="utf-8"), "retain\n")
        tagged = self.run_publish("tag", "MC-AI-0032")
        self.assertEqual(tagged.returncode, 0, tagged.stderr)
        year, month, day = self.git_output("show", "-s", "--format=%cs", promoted).split("-")
        name = f"ai/EarlyDev/0.1.0/3_{year[2:]}.{month}.{day}"
        self.assertEqual(tagged.stdout.strip(), name)
        self.assertEqual(self.git_output("rev-parse", f"{name}^{{commit}}"), promoted)
        old = self.run_publish("tag", "MC-AI-0032")
        self.assertNotEqual(old.returncode, 0)
        self.assertIn("will not be rewritten", old.stderr)

    def test_finish_rejects_a_staged_post_prepare_edit_before_commit(self) -> None:
        self.prepare()
        tampered = self.root / "src" / "tampered.txt"
        tampered.write_text("not reviewed\n", encoding="utf-8")
        self.git("add", "src/tampered.txt")
        self.record_pending_review()
        original_head = self.git_output("rev-parse", "HEAD")
        rejected = self.run_publish("finish", "MC-AI-0032", self.source)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("expected immutable merge tree", rejected.stderr)
        self.assertEqual(self.git_output("rev-parse", "HEAD"), original_head)

    def test_finish_rechecks_index_after_candidate_checks(self) -> None:
        self.prepare()
        self.record_pending_review()
        original_head = self.git_output("rev-parse", "HEAD")
        with mock.patch.dict(
            os.environ,
            {"MC_TEST_PUBLISH_STAGE_DURING_CHECK": str(self.root)},
        ):
            rejected = self.run_publish("finish", "MC-AI-0032", self.source)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("expected immutable merge tree", rejected.stderr)
        self.assertEqual(self.git_output("rev-parse", "HEAD"), original_head)

    def test_prepare_rejects_dirty_symbolic_and_mismatched_ai_main(self) -> None:
        (self.root / "untracked.txt").write_text("dirty\n", encoding="utf-8")
        dirty = self.run_publish("prepare", "MC-AI-0032", self.source)
        self.assertNotEqual(dirty.returncode, 0)
        self.assertIn("clean", dirty.stderr)
        (self.root / "untracked.txt").unlink()
        symbolic = self.run_publish("prepare", "MC-AI-0032", "ai-dev")
        self.assertNotEqual(symbolic.returncode, 0)
        self.assertIn("immutable", symbolic.stderr)
        self.git("checkout", "-q", "ai-main")
        self.git("commit", "--allow-empty", "--no-gpg-sign", "-q", "-m", "advance main")
        self.git("checkout", "-q", "ai-dev")
        mismatch = self.run_publish("prepare", "MC-AI-0032", self.source)
        self.assertNotEqual(mismatch.returncode, 0)
        self.assertIn("baseline", mismatch.stderr)

    def test_prepare_rejects_unfinished_children_and_non_snapshot_tasks(self) -> None:
        minor = self.run_publish("prepare", "MC-AI-0033", self.source)
        self.assertNotEqual(minor.returncode, 0)
        self.assertIn("only explicit snapshot", minor.stderr)
        self.write_backlog("active")
        self.git("add", "docs/ai/backlog.json")
        self.git("commit", "--no-gpg-sign", "-q", "-m", "invalid snapshot\n\nTask-ID: MC-AI-0032")
        invalid_source = self.git_output("rev-parse", "HEAD")
        unfinished = self.run_publish("prepare", "MC-AI-0032", invalid_source)
        self.assertNotEqual(unfinished.returncode, 0)
        self.assertIn("missing or not done", unfinished.stderr)

    def test_prepare_rejects_unreleased_statuses_and_done_without_resolution_date(self) -> None:
        for status in ("backlog", "blocked", "done"):
            self.write_backlog(snapshot_status=status)
            self.git("add", "docs/ai/backlog.json")
            self.git("commit", "--no-gpg-sign", "-q", "-m", f"{status} snapshot\n\nTask-ID: MC-AI-0032")
            source = self.git_output("rev-parse", "HEAD")
            rejected = self.run_publish("prepare", "MC-AI-0032", source)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("active and finalized", rejected.stderr)

        self.write_backlog(snapshot_status="done", resolved_at="")
        self.git("add", "docs/ai/backlog.json")
        self.git("commit", "--no-gpg-sign", "-q", "-m", "missing date\n\nTask-ID: MC-AI-0032")
        source = self.git_output("rev-parse", "HEAD")
        rejected = self.run_publish("prepare", "MC-AI-0032", source)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("done but has no resolved_at", rejected.stderr)

    def test_finish_rejects_a_raced_ai_main_before_commit(self) -> None:
        self.prepare()
        self.git("merge", "--abort")
        self.git("commit", "--allow-empty", "--no-gpg-sign", "-q", "-m", "racing promotion")
        self.git("merge", "--no-ff", "--no-commit", self.source)
        raced = self.run_publish("finish", "MC-AI-0032", self.source)
        self.assertNotEqual(raced.returncode, 0)
        self.assertIn("changed after prepare", raced.stderr)

    def test_prepare_rejects_conflicting_immutable_inputs_before_checkout(self) -> None:
        (self.root / "src" / "conflict.txt").write_text("ai-dev\n", encoding="utf-8")
        self.git("add", "src/conflict.txt")
        self.git("commit", "--no-gpg-sign", "-q", "-m", "conflicting source\n\nTask-ID: MC-AI-0032")
        conflicting_source = self.git_output("rev-parse", "HEAD")
        rejected = self.run_publish("prepare", "MC-AI-0032", conflicting_source)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("do not merge cleanly", rejected.stderr)
        self.assertEqual(self.git_output("branch", "--show-current"), "ai-dev")


if __name__ == "__main__":
    unittest.main()
