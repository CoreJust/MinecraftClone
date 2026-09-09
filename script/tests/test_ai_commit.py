from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "script" / "ai_commit.py"


TASKS_STUB = '''
def validate_backlog(tasks):
    if not isinstance(tasks, list):
        raise ValueError("backlog must be a list")
    for task in tasks:
        if not isinstance(task, dict) or not isinstance(task.get("id"), str):
            raise ValueError("invalid task")
'''

HISTORY_STUB = '''
class HistoryError(RuntimeError):
    pass

def finalize(repo, tasks, task_id, head, additional_head=None):
    task = next(item for item in tasks if item["id"] == task_id)
    if task.get("force_history_error"):
        raise HistoryError("range contains an unassigned task")
    if task.get("require_additional") and not additional_head:
        raise HistoryError("additional merge head was not provided")
'''


def real_task(task_id: str, level: str, parent: str = "", **overrides: object) -> dict[str, object]:
    task: dict[str, object] = {
        "id": task_id,
        "title": task_id,
        "kind": "chore",
        "status": "ready",
        "priority": "P1",
        "route": "terra",
        "milestone": "Test",
        "depends_on": [],
        "acceptance": "Acceptance",
        "evidence": "",
        "blocker": "",
        "owner": "",
        "level": level,
        "parent": parent,
        "motivation": "Motivation",
        "context": "Context",
        "complexity": "low",
        "created_at": "2026-09-09",
        "resolved_at": "",
        "resolution_changes": "",
        "plan": ["Plan"],
        "product_changes": [],
        "code_changes": [],
        "baseline_commit": "",
        "commits": [],
        "finalized": False,
        "retrospective": "",
        "docs_review": "",
        "environment_review": "",
        "backlog_review": "",
    }
    task.update(overrides)
    return task


class AiCommitTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        for relative, content in {
            "script/ai_commit.py": SCRIPT.read_text(encoding="utf-8"),
            "script/ai_tasks.py": TASKS_STUB,
            "script/ai_history.py": HISTORY_STUB,
            "script/ai_check.py": "raise SystemExit(0)\n",
            "docs/ai/backlog.json": "[]\n",
            "src/fixture.cpp": "base\n",
        }.items():
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding="utf-8")
        self.git("init")
        self.git("config", "user.email", "ai-commit@example.invalid")
        self.git("config", "user.name", "AI Commit")
        self.git("add", ".")
        self.git("commit", "--no-gpg-sign", "-m", "fixture")
        self.baseline = self.git("rev-parse", "HEAD").stdout.strip()
        self.stage_task()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def git(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(["git", *args], cwd=self.root, text=True, capture_output=True, check=True)

    def task(self, **overrides: object) -> dict[str, object]:
        task: dict[str, object] = {
            "id": "MC-AI-0001",
            "level": "basic",
            "baseline_commit": self.baseline,
            "finalized": False,
        }
        task.update(overrides)
        return task

    def stage_task(self, **overrides: object) -> None:
        backlog = self.root / "docs/ai/backlog.json"
        backlog.write_text(json.dumps([self.task(**overrides)]) + "\n", encoding="utf-8")
        self.git("add", "docs/ai/backlog.json")

    def run_command(self, *args: str, environment: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, SCRIPT, "--root", self.root, *args],
            cwd=self.root,
            text=True,
            capture_output=True,
            env=environment,
        )

    def candidate(self, task_id: str = "MC-AI-0001") -> dict[str, object]:
        result = self.run_command("candidate", task_id)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def write_report(self, candidate: dict[str, object], model: str = "gpt-5.6-luna", **overrides: object) -> Path:
        report = dict(candidate) | {
            "model": model,
            "effort": "high",
            "verdict": "approved",
            "evidence": "Reviewed the staged diff and relevant acceptance evidence.",
        }
        report.update(overrides)
        target = self.root / "report.json"
        target.write_text(json.dumps(report), encoding="utf-8")
        return target

    def record(self, candidate: dict[str, object], model: str = "gpt-5.6-luna", **overrides: object) -> subprocess.CompletedProcess[str]:
        return self.run_command("record-review", str(self.write_report(candidate, model, **overrides)))

    def test_missing_receipt_and_selector_are_rejected(self) -> None:
        result = self.run_command("check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("no Luna review", result.stderr)

    def test_current_selector_cannot_replace_a_receipt(self) -> None:
        candidate = self.candidate()
        selector = self.root / ".git/ai-reviews/current.json"
        selector.parent.mkdir(parents=True)
        selector.write_text(json.dumps(candidate), encoding="utf-8")
        result = self.run_command("check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing gpt-5.6-luna review receipt", result.stderr)

    def test_staging_change_invalidates_recorded_luna_review(self) -> None:
        candidate = self.candidate()
        self.assertEqual(self.record(candidate).returncode, 0)
        source = self.root / "src/fixture.cpp"
        source.write_text("changed\n", encoding="utf-8")
        self.git("add", "src/fixture.cpp")
        result = self.run_command("check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing gpt-5.6-luna review receipt", result.stderr)

    def test_report_rejects_wrong_model_task_and_head(self) -> None:
        candidate = self.candidate()
        for name, changes in (
            ("model", {"model": "gpt-5.6-sol"}),
            ("task", {"task_id": "MC-AI-9999"}),
            ("head", {"head": "0" * 40}),
        ):
            with self.subTest(name=name):
                result = self.record(candidate, **changes)
                self.assertEqual(result.returncode, 1)

    def test_commit_message_requires_one_matching_trailer(self) -> None:
        candidate = self.candidate()
        self.assertEqual(self.record(candidate).returncode, 0)
        messages = {
            "valid": "subject\n\nTask-ID: MC-AI-0001\n",
            "missing": "subject\n",
            "mismatch": "subject\n\nTask-ID: MC-AI-0002\n",
            "multiple": "subject\n\nTask-ID: MC-AI-0001\nTask-ID: MC-AI-0001\n",
        }
        for name, content in messages.items():
            with self.subTest(name=name):
                message = self.root / f"{name}.msg"
                message.write_text(content, encoding="utf-8")
                result = self.run_command("check", "--message", str(message))
                self.assertEqual(result.returncode, 0 if name == "valid" else 1, result.stderr)

    def test_minor_requires_terra_and_finalized_metadata(self) -> None:
        self.stage_task(level="snapshot", finalized=True, baseline_commit="")
        missing_baseline = self.run_command("candidate", "MC-AI-0001")
        self.assertEqual(missing_baseline.returncode, 1)
        self.assertIn("baseline_commit", missing_baseline.stderr)

        self.stage_task(level="minor", finalized=False)
        unfinalized = self.run_command("candidate", "MC-AI-0001")
        self.assertEqual(unfinalized.returncode, 1)
        self.assertIn("finalized", unfinalized.stderr)

        self.stage_task(level="minor", finalized=True)
        candidate = self.candidate()
        self.assertEqual(self.record(candidate).returncode, 0)
        missing_terra = self.run_command("check")
        self.assertEqual(missing_terra.returncode, 1)
        self.assertIn("gpt-5.6-terra", missing_terra.stderr)
        self.assertEqual(self.record(candidate, "gpt-5.6-terra").returncode, 0)
        self.assertEqual(self.run_command("check").returncode, 0)

    def test_aggregate_candidate_revalidates_history(self) -> None:
        self.stage_task(level="snapshot", finalized=True, force_history_error=True)
        result = self.run_command("candidate", "MC-AI-0001")
        self.assertEqual(result.returncode, 1)
        self.assertIn("staged finalization is not valid", result.stderr)

    def test_no_commit_promotion_binds_merge_head_and_invalidates_old_receipt(self) -> None:
        self.git("checkout", "-b", "promotion-one")
        self.git("commit", "--allow-empty", "--no-gpg-sign", "-m", "promotion one")
        promotion_one = self.git("rev-parse", "HEAD").stdout.strip()
        self.git("checkout", "master")
        self.git("merge", "--no-ff", "--no-commit", "promotion-one")
        self.stage_task(level="snapshot", finalized=True, require_additional=True)
        first = self.candidate()
        self.assertEqual(first["merge_head"], promotion_one)
        self.assertEqual(first["head"], self.baseline)
        self.assertEqual(self.record(first).returncode, 0)
        self.git("merge", "--abort")

        self.git("checkout", "-b", "promotion-two", self.baseline)
        self.git("commit", "--allow-empty", "--no-gpg-sign", "-m", "promotion two")
        promotion_two = self.git("rev-parse", "HEAD").stdout.strip()
        self.git("checkout", "master")
        self.git("merge", "--no-ff", "--no-commit", "promotion-two")
        self.stage_task(level="snapshot", finalized=True, require_additional=True)
        second = self.candidate()
        self.assertEqual(second["merge_head"], promotion_two)
        self.assertEqual(second["tree"], first["tree"])
        self.assertNotEqual(second["merge_head"], first["merge_head"])
        stale = self.run_command("check")
        self.assertEqual(stale.returncode, 1)
        self.assertIn("missing gpt-5.6-luna review receipt", stale.stderr)

    def test_divergent_promotion_is_accepted_and_validates_the_union(self) -> None:
        self.git("checkout", "-b", "promotion", self.baseline)
        self.git("commit", "--allow-empty", "--no-gpg-sign", "-m", "promotion")
        self.git("checkout", "master")
        self.git("commit", "--allow-empty", "--no-gpg-sign", "-m", "main work")
        main_head = self.git("rev-parse", "HEAD").stdout.strip()
        self.git("merge", "--no-ff", "--no-commit", "promotion")
        promotion_head = self.git("rev-parse", "MERGE_HEAD").stdout.strip()
        self.stage_task(level="snapshot", finalized=True, require_additional=True)
        candidate = self.candidate()
        self.assertEqual(candidate["head"], main_head)
        self.assertEqual(candidate["merge_head"], promotion_head)

    def test_promotion_rejects_unassigned_union_history(self) -> None:
        self.git("checkout", "-b", "promotion")
        self.git("commit", "--allow-empty", "--no-gpg-sign", "-m", "unassigned promotion work")
        self.git("checkout", "master")
        self.git("merge", "--no-ff", "--no-commit", "promotion")
        self.stage_task(level="snapshot", finalized=True, force_history_error=True)
        result = self.run_command("candidate", "MC-AI-0001")
        self.assertEqual(result.returncode, 1)
        self.assertIn("unassigned task", result.stderr)

    def test_empty_merge_head_is_rejected(self) -> None:
        merge_file = Path(self.git("rev-parse", "--git-path", "MERGE_HEAD").stdout.strip())
        if not merge_file.is_absolute():
            merge_file = self.root / merge_file
        merge_file.write_text("", encoding="utf-8")
        self.stage_task(level="snapshot", finalized=True)
        result = self.run_command("candidate", "MC-AI-0001")
        self.assertEqual(result.returncode, 1)
        self.assertIn("clean two-parent promotions", result.stderr)

    def test_precommit_selects_fast_or_candidate_check_by_level(self) -> None:
        log = self.root / "check.log"
        checker = self.root / "script/ai_check.py"
        checker.write_text(
            "import os\n"
            "import pathlib\n"
            "import sys\n"
            "pathlib.Path(os.environ['AI_CHECK_LOG']).write_text(' '.join(sys.argv[1:]) + '\\n')\n",
            encoding="utf-8",
        )
        environment = os.environ | {"AI_CHECK_LOG": str(log)}

        basic = self.candidate()
        self.assertEqual(self.record(basic).returncode, 0)
        self.assertEqual(self.run_command("precommit", environment=environment).returncode, 0)
        self.assertEqual(log.read_text(encoding="utf-8").strip(), "--fast --require-index-match")

        self.stage_task(level="snapshot", finalized=True)
        snapshot = self.candidate()
        self.assertEqual(self.record(snapshot).returncode, 0)
        self.assertEqual(self.run_command("precommit", environment=environment).returncode, 0)
        self.assertEqual(
            log.read_text(encoding="utf-8").strip(),
            "--candidate --level snapshot --require-index-match",
        )


class RealAiCommitIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        for name in ("ai_commit.py", "ai_tasks.py", "ai_history.py"):
            source = REPOSITORY / "script" / name
            target = self.root / "script" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(source.read_text(encoding="utf-8"), encoding="utf-8")
        self.git("init")
        self.git("config", "user.email", "ai-commit-integration@example.invalid")
        self.git("config", "user.name", "AI Commit Integration")
        tasks = [
            real_task(
                "MC-AI-0001", "basic", "MC-AI-0101", status="done", evidence="passed",
                resolved_at="2026-09-09", resolution_changes="child complete",
            ),
            real_task("MC-AI-0101", "snapshot", "MC-AI-0102"),
            real_task("MC-AI-0102", "minor", "MC-AI-0103"),
            real_task("MC-AI-0103", "major"),
        ]
        backlog = self.root / "docs/ai/backlog.json"
        backlog.parent.mkdir(parents=True, exist_ok=True)
        backlog.write_text(json.dumps(tasks) + "\n", encoding="utf-8")
        self.git("add", ".")
        self.git("commit", "--no-gpg-sign", "-m", "baseline")
        self.baseline = self.git("rev-parse", "HEAD").stdout.strip()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def git(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(["git", *args], cwd=self.root, text=True, capture_output=True, check=True)

    def test_real_history_accepts_pending_merge_union_for_snapshot_candidate(self) -> None:
        self.git("checkout", "-b", "basic-child")
        self.git("commit", "--allow-empty", "--no-gpg-sign", "-m", "child\n\nTask-ID: MC-AI-0001")
        child_head = self.git("rev-parse", "HEAD").stdout.strip()
        self.git("checkout", "master")
        self.git("merge", "--no-ff", "--no-commit", "basic-child")

        backlog = self.root / "docs/ai/backlog.json"
        tasks = json.loads(backlog.read_text(encoding="utf-8"))
        snapshot = next(task for task in tasks if task["id"] == "MC-AI-0101")
        snapshot.update({
            "baseline_commit": self.baseline,
            "finalized": True,
            "product_changes": ["Snapshot result"],
            "code_changes": ["Gate result"],
            "evidence": "child reviewed",
            "resolved_at": "2026-09-09",
            "resolution_changes": "snapshot finalization prepared",
        })
        backlog.write_text(json.dumps(tasks) + "\n", encoding="utf-8")
        self.git("add", "docs/ai/backlog.json")

        result = subprocess.run(
            [sys.executable, SCRIPT, "--root", self.root, "candidate", "MC-AI-0101"],
            cwd=self.root,
            text=True,
            capture_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        candidate = json.loads(result.stdout)
        self.assertEqual(candidate["head"], self.baseline)
        self.assertEqual(candidate["merge_head"], child_head)


if __name__ == "__main__":
    unittest.main()
