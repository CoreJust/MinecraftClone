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
            "status": "done",
            "finalized": True,
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

    def write_report(self, candidate: dict[str, object], model: str = "gpt-6-luna", **overrides: object) -> Path:
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

    def record(self, candidate: dict[str, object], model: str = "gpt-6-luna", **overrides: object) -> subprocess.CompletedProcess[str]:
        return self.run_command("record-review", str(self.write_report(candidate, model, **overrides)))

    def test_missing_receipt_and_selector_are_rejected(self) -> None:
        result = self.run_command("check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("no task review", result.stderr)

    def test_current_selector_cannot_replace_a_receipt(self) -> None:
        candidate = self.candidate()
        selector = self.root / ".git/ai-reviews/current.json"
        selector.parent.mkdir(parents=True)
        selector.write_text(json.dumps(candidate), encoding="utf-8")
        result = self.run_command("check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing task review receipt", result.stderr)

    def test_staging_change_invalidates_recorded_luna_review(self) -> None:
        candidate = self.candidate()
        self.assertEqual(self.record(candidate).returncode, 0)
        source = self.root / "src/fixture.cpp"
        source.write_text("changed\n", encoding="utf-8")
        self.git("add", "src/fixture.cpp")
        result = self.run_command("check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing task review receipt", result.stderr)

    def test_report_rejects_wrong_model_task_and_head(self) -> None:
        candidate = self.candidate()
        for name, changes in (
            ("model", {"model": "gpt-5.6-luna"}),
            ("task", {"task_id": "MC-AI-9999"}),
            ("head", {"head": "0" * 40}),
            ("tree", {"tree": "0" * 40}),
            ("baseline", {"baseline_commit": "0" * 40}),
            ("merge", {"merge_head": "0" * 40}),
            ("effort", {"effort": "minimal"}),
            ("verdict", {"verdict": "rejected"}),
            ("evidence", {"evidence": " "}),
        ):
            with self.subTest(name=name):
                result = self.record(candidate, **changes)
                self.assertEqual(result.returncode, 1)

    def test_advanced_effort_reports_are_accepted(self) -> None:
        candidate = self.candidate()
        for effort in ("medium", "xhigh", "max"):
            with self.subTest(effort=effort):
                result = self.record(candidate, effort=effort)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_profile_task_receipts_preserve_actual_model_and_effort(self) -> None:
        candidate = self.candidate()
        for model, effort in (
            ("gpt-6.1-sol", "high"),
            ("gpt-6-astra", "ultra"),
            ("gpt-6-luna", "low"),
            ("gpt-6-sol", "low"),
        ):
            with self.subTest(model=model, effort=effort):
                result = self.record(candidate, model, effort=effort, review_role="task")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.run_command("check").returncode, 0)
                receipts = list((self.root / ".git/ai-reviews/MC-AI-0001").glob("*.json"))
                self.assertEqual(len(receipts), 1)
                receipt = json.loads(receipts[0].read_text(encoding="utf-8"))
                self.assertEqual((receipt["model"], receipt["effort"], receipt["review_role"]), (model, effort, "task"))

    def test_new_models_require_explicit_review_role(self) -> None:
        candidate = self.candidate()
        for model in ("gpt-6.1-sol", "gpt-6-astra"):
            with self.subTest(model=model):
                result = self.record(candidate, model)
                self.assertEqual(result.returncode, 1)
                self.assertIn("explicit role", result.stderr)

    def test_unknown_and_null_review_roles_are_rejected(self) -> None:
        candidate = self.candidate()
        for role in ("primary", "", None, ["task"]):
            with self.subTest(role=role):
                result = self.record(candidate, review_role=role)
                self.assertEqual(result.returncode, 1)
                self.assertIn("review_role", result.stderr)

    def test_model_effort_must_be_supported(self) -> None:
        candidate = self.candidate()
        for model, effort in (("gpt-6-luna", "ultra"), ("gpt-6.1-sol", "none"), ("gpt-6-astra", ["high"])):
            with self.subTest(model=model, effort=effort):
                result = self.record(candidate, model, effort=effort, review_role="task")
                self.assertEqual(result.returncode, 1)
                self.assertIn("effort", result.stderr)

    def test_aggregate_review_does_not_select_or_satisfy_task_review(self) -> None:
        candidate = self.candidate()
        self.assertEqual(self.record(candidate, "gpt-6.1-sol", review_role="aggregate").returncode, 0)
        unselected = self.run_command("check")
        self.assertEqual(unselected.returncode, 1)
        self.assertIn("no task review", unselected.stderr)
        selected = self.run_command("check", environment=os.environ | {"AI_TASK": "MC-AI-0001"})
        self.assertEqual(selected.returncode, 1)
        self.assertIn("missing task review receipt", selected.stderr)

    def test_task_review_does_not_satisfy_aggregate_review(self) -> None:
        self.stage_task(level="minor", finalized=True)
        candidate = self.candidate()
        self.assertEqual(self.record(candidate, "gpt-6.1-sol", review_role="task").returncode, 0)
        missing = self.run_command("check")
        self.assertEqual(missing.returncode, 1)
        self.assertIn("missing aggregate review receipt", missing.stderr)
        self.assertEqual(self.record(candidate, "gpt-6.1-sol", review_role="aggregate").returncode, 0)
        self.assertEqual(self.run_command("check").returncode, 0)
        receipts = list((self.root / ".git/ai-reviews/MC-AI-0001").glob("*.json"))
        self.assertEqual(len(receipts), 2)

    def test_receipt_with_mismatched_role_is_rejected(self) -> None:
        candidate = self.candidate()
        self.assertEqual(self.record(candidate, "gpt-6.1-sol", review_role="task").returncode, 0)
        receipt_path = next((self.root / ".git/ai-reviews/MC-AI-0001").glob("*.json"))
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        receipt["review_role"] = "aggregate"
        receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
        result = self.run_command("check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("wrong review_role", result.stderr)

    def test_legacy_model_paths_and_selector_still_work(self) -> None:
        self.stage_task(level="minor", finalized=True)
        candidate = self.candidate()
        directory = self.root / ".git/ai-reviews/MC-AI-0001"
        directory.mkdir(parents=True)
        for model in ("gpt-6-luna", "gpt-6-sol"):
            report = self.write_report(candidate, model)
            key = "-".join(str(candidate[field]) for field in ("head", "tree", "baseline_commit", "merge_head"))
            (directory / f"{key}-{model}.json").write_text(report.read_text(encoding="utf-8"), encoding="utf-8")
        (directory.parent / "current.json").write_text(json.dumps({"task_id": "MC-AI-0001"}), encoding="utf-8")
        self.assertEqual(self.run_command("check").returncode, 0)

    def test_legacy_path_must_match_its_model(self) -> None:
        candidate = self.candidate()
        self.assertEqual(self.record(candidate, "gpt-6-astra", review_role="task").returncode, 0)
        receipt = next((self.root / ".git/ai-reviews/MC-AI-0001").glob("*-task.json"))
        receipt.rename(receipt.with_name(receipt.name.removesuffix("task.json") + "gpt-6-luna.json"))
        result = self.run_command("check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("wrong model", result.stderr)

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

    def test_basic_requires_done_and_finalized_metadata(self) -> None:
        self.stage_task(status="active", finalized=True)
        active = self.run_command("candidate", "MC-AI-0001")
        self.assertEqual(active.returncode, 1)
        self.assertIn("status to done and finalized to true", active.stderr)

        self.stage_task(status="done", finalized=False)
        unfinalized = self.run_command("candidate", "MC-AI-0001")
        self.assertEqual(unfinalized.returncode, 1)
        self.assertIn("status to done and finalized to true", unfinalized.stderr)

        self.stage_task(status="done", finalized=True)
        candidate = self.candidate()
        self.assertEqual(candidate["level"], "basic")

    def test_minor_requires_sol_and_finalized_metadata(self) -> None:
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
        self.assertIn("missing aggregate review receipt", missing_terra.stderr)
        self.assertEqual(self.record(candidate, "gpt-6-sol").returncode, 0)
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
        self.assertIn("missing task review receipt", stale.stderr)

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

        self.git("branch", "ai-main")
        self.git("checkout", "-b", "snapshot-source")
        self.git("commit", "--allow-empty", "--no-gpg-sign", "-m", "snapshot source")
        source = self.git("rev-parse", "HEAD").stdout.strip()
        self.git("checkout", "ai-main")
        self.git("merge", "--no-ff", "--no-commit", "snapshot-source")
        self.stage_task(level="snapshot", finalized=True)
        promotion = self.candidate()
        self.assertEqual(promotion["merge_head"], source)
        self.assertEqual(self.record(promotion).returncode, 0)
        self.assertEqual(self.run_command("precommit", environment=environment).returncode, 0)
        self.assertEqual(
            log.read_text(encoding="utf-8").strip(),
            f"--candidate --level snapshot --snapshot-source-commit {source} --require-index-match",
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
                resolved_at="2026-09-09", resolution_changes="child complete", finalized=True,
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
            "status": "active",
            "owner": "Codex",
            "baseline_commit": self.baseline,
            "finalized": True,
            "product_changes": ["Snapshot result"],
            "code_changes": ["Gate result"],
            "evidence": "child reviewed",
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
