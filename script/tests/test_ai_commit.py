from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import textwrap
import time
import types
import unittest
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "script" / "ai_commit.py"


def load_ai_commit_module():
    spec = importlib.util.spec_from_file_location("ai_commit_under_test", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


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
            "script/ai_check.py": "def main(argv):\n    return 0\n",
            "docs/ai/backlog.json": "[]\n",
            "src/fixture.cpp": "base\n",
        }.items():
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding="utf-8")
        self.git("init", "-b", "master")
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

    def stage_checker_candidate(self, checker_source: str) -> dict[str, object]:
        checker = self.root / "script/ai_check.py"
        checker.write_text(checker_source, encoding="utf-8")
        self.git("add", "script/ai_check.py")
        self.stage_task()
        candidate = self.candidate()
        self.assertEqual(self.record(candidate).returncode, 0)
        return candidate

    def test_missing_receipt_and_selector_are_rejected(self) -> None:
        result = self.run_command("check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("no task review", result.stderr)

    def test_git_bytes_handles_large_batch_and_binary_output(self) -> None:
        module = load_ai_commit_module()
        payload = bytes(range(256)) * 512
        blob = subprocess.run(
            ["git", "hash-object", "-w", "--stdin"], cwd=self.root,
            input=payload, capture_output=True, check=True, timeout=5,
        ).stdout.strip()
        batch = (blob + b"\n") * 4096
        expected = (blob + b" blob " + str(len(payload)).encode() + b"\n") * 4096
        real_run = subprocess.run

        def file_backed_run(*args, **kwargs):
            # Fail before invoking Git if a regression reintroduces blocking pipes.
            self.assertNotIn("input", kwargs)
            self.assertNotIn("capture_output", kwargs)
            for name in ("stdin", "stdout", "stderr"):
                stream = kwargs[name]
                if stream is not None:
                    self.assertTrue(stat.S_ISREG(os.fstat(stream.fileno()).st_mode))
            return real_run(*args, **kwargs)

        with mock.patch.object(module.subprocess, "run", side_effect=file_backed_run):
            self.assertEqual(
                module.git_bytes(self.root, "cat-file", "--batch-check", input_bytes=batch, timeout=5),
                expected,
            )
            self.assertEqual(
                module.git_bytes(self.root, "cat-file", "blob", blob.decode(), timeout=5), payload,
            )

    def test_git_bytes_propagates_git_stderr(self) -> None:
        module = load_ai_commit_module()
        with self.assertRaisesRegex(module.CommitGateError, "not a git command"):
            module.git_bytes(self.root, "not-a-real-git-command", timeout=5)

    def test_git_bytes_translates_timeout(self) -> None:
        module = load_ai_commit_module()
        with mock.patch.object(module.subprocess, "run", side_effect=subprocess.TimeoutExpired("git", 0.25)):
            with self.assertRaisesRegex(module.CommitGateError, "timed out after 0.25s") as caught:
                module.git_bytes(self.root, "cat-file", "--batch-check", input_bytes=b"", timeout=0.25)
        self.assertIsInstance(caught.exception.__cause__, subprocess.TimeoutExpired)

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
            "def main(argv=None):\n"
            "    args = argv if argv is not None else sys.argv[1:]\n"
            "    if '--root' in args:\n"
            "        args = args[args.index('--fast'):]\n"
            "    pathlib.Path(os.environ['AI_CHECK_LOG']).write_text(' '.join(args) + '\\n')\n"
            "    return 0\n"
            "if __name__ == '__main__':\n"
            "    raise SystemExit(main())\n",
            encoding="utf-8",
        )
        self.git("add", "script/ai_check.py")
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

    def test_basic_precommit_runs_reviewed_tree_and_preserves_dirty_checkout(self) -> None:
        checker_source = textwrap.dedent('''
            import json, os, stat, subprocess, sys
            from pathlib import Path
            def main(argv):
                root = Path(__file__).resolve().parents[1]
                def git(*args):
                    return subprocess.run(["git", *args], cwd=root, text=True,
                                          capture_output=True, check=True).stdout.strip()
                link = root / "src/link"
                import ai_tasks
                data = {
                    "argv": argv, "cwd": str(root),
                    "git_dir": os.environ.get("GIT_DIR"),
                    "work_tree": os.environ.get("GIT_WORK_TREE"),
                    "index_file": os.environ.get("GIT_INDEX_FILE"),
                    "tree": git("write-tree"), "working_diff": git("diff", "--name-only"),
                    "source": (root / "src/fixture.cpp").read_text(encoding="utf-8"),
                    "untracked_source": (root / "src/untracked.cpp").exists(),
                    "untracked_doc": (root / "docs/untracked.md").exists(),
                    "tool_mode": stat.S_IMODE((root / "src/tool.sh").stat().st_mode),
                    "link_is_symlink": link.is_symlink(),
                    "link_target": os.readlink(link) if link.is_symlink() else None,
                    "ai_tasks_path": ai_tasks.__file__,
                }
                Path(os.environ["AI_TEST_LOG"]).write_text(json.dumps(data), encoding="utf-8")
                return 0
        ''')
        checker = self.root / "script/ai_check.py"
        checker.write_text(checker_source, encoding="utf-8")
        source = self.root / "src/fixture.cpp"
        source.write_text("staged source\n", encoding="utf-8")
        tool = self.root / "src/tool.sh"
        tool.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        tool.chmod(0o755)
        attributes = self.root / ".gitattributes"
        attributes.write_text("src/tool.sh export-ignore\n", encoding="utf-8")
        self.git("add", ".gitattributes", "script/ai_check.py", "src/fixture.cpp", "src/tool.sh")
        link = self.root / "src/link"
        symlink_staged = False
        try:
            link.symlink_to("fixture.cpp")
            self.git("add", "src/link")
            symlink_staged = self.git("ls-files", "--stage", "src/link").stdout.startswith("120000 ")
        except (NotImplementedError, OSError):
            pass
        self.stage_task()
        candidate = self.candidate()
        self.assertEqual(self.record(candidate).returncode, 0)

        checker.write_text("raise SystemExit('unstaged checker ran')\n", encoding="utf-8")
        source.write_text("unstaged source\n", encoding="utf-8")
        (self.root / "src/untracked.cpp").write_text("untracked source\n", encoding="utf-8")
        (self.root / "docs/untracked.md").write_text("untracked doc\n", encoding="utf-8")
        index_path = self.root / ".git/index"
        original_index = index_path.read_bytes()
        original_status = self.git("status", "--porcelain=v1", "-z").stdout
        original_refs = self.git("show-ref").stdout
        original_cwd = os.getcwd()
        original_sys_path = sys.path[:]
        staged_git_names = ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE")
        original_git_environment = {name: os.environ.get(name) for name in staged_git_names}
        original_ai_tasks = sys.modules.get("ai_tasks")
        stale_ai_tasks = types.ModuleType("ai_tasks")
        stale_ai_tasks.__file__ = str(REPOSITORY / "script" / "ai_tasks.py")
        sys.modules["ai_tasks"] = stale_ai_tasks
        with tempfile.TemporaryDirectory(prefix="ai-commit-probe-") as log_dir:
            log = Path(log_dir) / "checker.json"
            with mock.patch.dict(os.environ, {"AI_TEST_LOG": str(log)}):
                implementation = load_ai_commit_module()
                result = implementation.precommit(self.root)
            self.assertEqual(result, 0)
            observed = json.loads(log.read_text(encoding="utf-8"))
        if original_ai_tasks is None:
            sys.modules.pop("ai_tasks", None)
        else:
            sys.modules["ai_tasks"] = original_ai_tasks

        self.assertEqual(observed["argv"][0], "--root")
        self.assertEqual(Path(observed["argv"][1]).resolve(), Path(observed["cwd"]).resolve())
        self.assertEqual(observed["argv"][2:], ["--fast", "--require-index-match"])
        self.assertEqual(observed["tree"], candidate["tree"])
        self.assertEqual(observed["source"], "staged source\n")
        self.assertEqual(observed["working_diff"], "")
        self.assertFalse(observed["untracked_source"])
        self.assertFalse(observed["untracked_doc"])
        self.assertEqual(Path(observed["work_tree"]).resolve(), Path(observed["cwd"]).resolve())
        self.assertNotEqual(Path(observed["cwd"]).resolve(), self.root.resolve())
        self.assertNotEqual(Path(observed["index_file"]).resolve(), index_path.resolve())
        self.assertFalse(Path(observed["cwd"]).exists())
        self.assertFalse(Path(observed["index_file"]).exists())
        self.assertEqual(
            Path(observed["git_dir"]).resolve(),
            (Path(observed["cwd"]) / ".git").resolve(),
        )
        self.assertNotEqual(
            Path(observed["git_dir"]).resolve(),
            Path(self.git("rev-parse", "--absolute-git-dir").stdout.strip()).resolve(),
        )
        if os.name != "nt":
            self.assertEqual(observed["tool_mode"], 0o755)
        self.assertEqual(observed["link_is_symlink"], symlink_staged)
        if symlink_staged:
            self.assertEqual(observed["link_target"], "fixture.cpp")
        self.assertEqual(
            Path(observed["ai_tasks_path"]).resolve(),
            (Path(observed["cwd"]) / "script/ai_tasks.py").resolve(),
        )
        self.assertEqual(os.getcwd(), original_cwd)
        self.assertEqual(sys.path, original_sys_path)
        self.assertEqual(
            {name: os.environ.get(name) for name in staged_git_names},
            original_git_environment,
        )
        self.assertIs(sys.modules.get("ai_tasks"), original_ai_tasks)
        self.assertEqual(index_path.read_bytes(), original_index)
        self.assertEqual(self.git("status", "--porcelain=v1", "-z").stdout, original_status)
        self.assertEqual(self.git("show-ref").stdout, original_refs)
        self.assertEqual(source.read_text(encoding="utf-8"), "unstaged source\n")

    def assert_staged_checkout_index(self, *, split_index: bool) -> None:
        implementation = load_ai_commit_module()
        helper = self.root / "script/ai_check_helper.py"
        helper.write_text((REPOSITORY / "script/ai_check.py").read_text(encoding="utf-8"), encoding="utf-8")
        checker_source = textwrap.dedent('''\
            import json, os, stat, subprocess
            from pathlib import Path
            from ai_check_helper import python_test_environment

            def main(argv):
                root = Path(__file__).resolve().parents[1]
                child_environment = python_test_environment(root)
                variables = subprocess.run(
                    ["git", "rev-parse", "--local-env-vars"],
                    cwd=root, text=True, capture_output=True, check=True,
                ).stdout.splitlines()
                target = Path(os.environ["AI_TEST_CHECKOUT"])
                target.mkdir(parents=True, exist_ok=True)
                child = subprocess.run(
                    ["git", "checkout-index", "--all", f"--prefix={target.as_posix()}/"],
                    cwd=root, env=child_environment, text=True, capture_output=True,
                )
                tree = subprocess.run(
                    ["git", "write-tree"], cwd=root, env=os.environ,
                    text=True, capture_output=True, check=True,
                ).stdout.strip()
                data = {
                    "returncode": child.returncode,
                    "stderr": child.stderr,
                    "git_selectors_cleared": all(name not in child_environment for name in variables),
                    "private_index_matches": (
                        Path(os.environ["GIT_DIR"], "index").read_bytes()
                        == Path(os.environ["GIT_INDEX_FILE"]).read_bytes()
                    ),
                    "tree": tree,
                    "tool_tree_entry": subprocess.run(
                        ["git", "ls-tree", "-r", tree, "--", "src/staged-tool.sh"],
                        cwd=root, env=os.environ, text=True, capture_output=True, check=True,
                    ).stdout.strip(),
                    "tool_mode": stat.S_IMODE((target / "src/staged-tool.sh").stat().st_mode),
                    "source": (target / "src/fixture.cpp").read_text(encoding="utf-8"),
                }
                Path(os.environ["AI_TEST_LOG"]).write_text(json.dumps(data), encoding="utf-8")
                return child.returncode
        ''')
        checker = self.root / "script/ai_check.py"
        checker.write_text(checker_source, encoding="utf-8")
        source = self.root / "src/fixture.cpp"
        source.write_text("staged snapshot\n", encoding="utf-8")
        tool = self.root / "src/staged-tool.sh"
        tool.write_text("#!/bin/sh\necho staged\n", encoding="utf-8")
        self.git(
            "add",
            "script/ai_check.py",
            "script/ai_check_helper.py",
            "src/fixture.cpp",
            "src/staged-tool.sh",
        )
        self.git("update-index", "--chmod=+x", "src/staged-tool.sh")
        self.stage_task()
        if split_index:
            self.git("update-index", "--split-index")
        candidate = self.candidate()
        self.assertEqual(self.record(candidate).returncode, 0)
        source.write_text("working snapshot\n", encoding="utf-8")
        tool.write_text("#!/bin/sh\necho working\n", encoding="utf-8")
        if os.name != "nt":
            tool.chmod(0o644)

        index_path = self.root / ".git/index"
        original_index = index_path.read_bytes()
        shared_indexes = {
            target.name: target.read_bytes()
            for target in (self.root / ".git").glob("sharedindex.*")
        }
        if split_index:
            self.assertTrue(shared_indexes)
        original_status = self.git("status", "--porcelain=v1", "-z").stdout
        original_refs = self.git("show-ref").stdout
        with tempfile.TemporaryDirectory(prefix="ai-commit-checkout-index-") as directory:
            target = Path(directory) / "checkout"
            log = Path(directory) / "checker.json"
            with mock.patch.dict(
                os.environ,
                {"AI_TEST_CHECKOUT": str(target), "AI_TEST_LOG": str(log)},
            ):
                self.assertEqual(implementation.precommit(self.root), 0)
            observed = json.loads(log.read_text(encoding="utf-8"))
            self.assertEqual(observed["returncode"], 0, observed["stderr"])
            self.assertTrue(observed["git_selectors_cleared"])
            self.assertTrue(observed["private_index_matches"])
            self.assertEqual(observed["tree"], candidate["tree"])
            self.assertTrue(observed["tool_tree_entry"].startswith("100755 blob "))
            if os.name != "nt":
                self.assertTrue(observed["tool_mode"] & stat.S_IXUSR)
            self.assertEqual(observed["source"], "staged snapshot\n")
            self.assertEqual((target / "src/fixture.cpp").read_text(encoding="utf-8"), "staged snapshot\n")
            self.assertEqual((target / "src/staged-tool.sh").read_text(encoding="utf-8"), "#!/bin/sh\necho staged\n")

        self.assertEqual(index_path.read_bytes(), original_index)
        self.assertEqual(
            {
                target.name: target.read_bytes()
                for target in (self.root / ".git").glob("sharedindex.*")
            },
            shared_indexes,
        )
        self.assertEqual(self.git("status", "--porcelain=v1", "-z").stdout, original_status)
        self.assertEqual(self.git("show-ref").stdout, original_refs)
        self.assertEqual(source.read_text(encoding="utf-8"), "working snapshot\n")
        self.assertEqual(tool.read_text(encoding="utf-8"), "#!/bin/sh\necho working\n")

    def test_staged_checkout_index_works_after_python_environment_clears_git_selectors(self) -> None:
        self.assert_staged_checkout_index(split_index=False)

    def test_split_index_checkout_uses_reviewed_tree_without_mutating_source_index(self) -> None:
        self.assert_staged_checkout_index(split_index=True)

    def test_basic_precommit_failure_cleans_temporary_snapshot(self) -> None:
        source = (
            "from pathlib import Path\n"
            "import os\n"
            "def main(argv):\n"
            "    Path(os.environ['AI_TEST_LOG']).write_text(str(Path.cwd()))\n"
            "    raise SystemExit(7)\n"
        )
        candidate = self.stage_checker_candidate(source)
        index_path = self.root / ".git/index"
        original_index = index_path.read_bytes()
        original_cwd = os.getcwd()
        original_sys_path = sys.path[:]
        staged_git_names = ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE")
        original_git_environment = {name: os.environ.get(name) for name in staged_git_names}
        with tempfile.TemporaryDirectory(prefix="ai-commit-probe-") as log_dir:
            log = Path(log_dir) / "cwd.txt"
            implementation = load_ai_commit_module()
            with mock.patch.dict(os.environ, {"AI_TEST_LOG": str(log)}):
                result = implementation.precommit(self.root)
            self.assertEqual(result, 7)
            staged_root = Path(log.read_text(encoding="utf-8"))
        self.assertTrue(candidate["tree"])
        self.assertFalse(staged_root.exists())
        self.assertFalse((staged_root.parent / "index").exists())
        self.assertEqual(index_path.read_bytes(), original_index)
        self.assertEqual(os.getcwd(), original_cwd)
        self.assertEqual(sys.path, original_sys_path)
        self.assertEqual(
            {name: os.environ.get(name) for name in staged_git_names},
            original_git_environment,
        )

    def test_basic_precommit_rejects_import_time_system_exit(self) -> None:
        self.stage_checker_candidate("raise SystemExit(0)\n")
        implementation = load_ai_commit_module()
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            result = implementation.precommit(self.root)
        self.assertEqual(result, 1)
        self.assertIn("staged script/ai_check.py exited during import", stderr.getvalue())

    def test_basic_precommit_rejects_external_build_symlink_and_log_copy(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ai-commit-external-") as external_dir:
            external = Path(external_dir)
            external_logs = external / "ai-checks"
            external_logs.mkdir()
            (external_logs / "secret.log").write_text("outside evidence\n", encoding="utf-8")
            try:
                (self.root / "build").symlink_to(external, target_is_directory=True)
            except (NotImplementedError, OSError) as error:
                self.skipTest(f"directory symlinks are unavailable: {error}")
            self.git("add", "-f", "build")
            checker_source = textwrap.dedent(f'''\
                from pathlib import Path
                def main(argv):
                    Path({str(external_logs / "escaped.log")!r}).write_text("checker ran")
                    return 1
            ''')
            candidate = self.stage_checker_candidate(checker_source)
            implementation = load_ai_commit_module()
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                result = implementation.precommit(self.root)
            self.assertEqual(result, 1)
            self.assertIn("symlink", stderr.getvalue().lower())
            self.assertFalse((external_logs / "escaped.log").exists())
            self.assertEqual((external_logs / "secret.log").read_text(encoding="utf-8"), "outside evidence\n")

            retained_stderr = io.StringIO()
            with contextlib.redirect_stderr(retained_stderr):
                retained = implementation.preserve_staged_check_logs(
                    Path(self.git("rev-parse", "--absolute-git-dir").stdout.strip()),
                    self.root,
                    candidate,
                )
            self.assertIsNone(retained)
            self.assertIn("not retained", retained_stderr.getvalue())
            self.assertFalse((Path(self.git("rev-parse", "--absolute-git-dir").stdout.strip())
                              / "ai-staged-fast-failures").exists())

    def make_failure_log_fixture(self, names: dict[str, bytes]) -> tuple[Path, Path, dict[str, str]]:
        staged_worktree = self.root / "staged-check"
        log_dir = staged_worktree / "build" / "ai-checks"
        log_dir.mkdir(parents=True)
        for name, content in names.items():
            (log_dir / name).write_bytes(content)
        git_directory = Path(self.git("rev-parse", "--absolute-git-dir").stdout.strip())
        candidate = {"task_id": "MC-AI-0001", "tree": "a" * 40}
        return staged_worktree, git_directory, candidate

    def test_failure_log_retention_caps_file_count_and_reports_omissions(self) -> None:
        staged_worktree, git_directory, candidate = self.make_failure_log_fixture({
            "first.log": b"first\n", "second.log": b"second\n", "third.log": b"third\n",
        })
        implementation = load_ai_commit_module()
        stderr = io.StringIO()
        with mock.patch.object(implementation, "STAGED_FAILURE_LOG_MAX_FILES", 1):
            with contextlib.redirect_stderr(stderr):
                retained = implementation.preserve_staged_check_logs(
                    git_directory, staged_worktree, candidate
                )
        self.assertIsNotNone(retained)
        retained = Path(retained)
        copied_logs = list(retained.glob("*.log"))
        self.assertEqual(len(copied_logs), 1)
        manifest = json.loads((retained / "retention-manifest.json").read_text(encoding="utf-8"))
        self.assertFalse(manifest["complete"])
        self.assertGreaterEqual(manifest["omitted_log_count"], 1)
        self.assertIn("partial", stderr.getvalue())

    def test_failure_log_retention_truncates_oversized_log_with_byte_manifest(self) -> None:
        staged_worktree, git_directory, candidate = self.make_failure_log_fixture({
            "large.log": b"0123456789abcdef",
        })
        implementation = load_ai_commit_module()
        with mock.patch.object(implementation, "STAGED_FAILURE_LOG_MAX_BYTES", 6):
            retained = implementation.preserve_staged_check_logs(
                git_directory, staged_worktree, candidate
            )
        self.assertIsNotNone(retained)
        retained = Path(retained)
        self.assertEqual((retained / "large.log").read_bytes(), b"012345")
        manifest = json.loads((retained / "retention-manifest.json").read_text(encoding="utf-8"))
        self.assertFalse(manifest["complete"])
        self.assertEqual(manifest["bytes_copied"], 6)
        self.assertEqual(manifest["truncated_files"], ["large.log"])

    def test_failure_log_retention_deadline_writes_incomplete_manifest(self) -> None:
        staged_worktree, git_directory, candidate = self.make_failure_log_fixture({
            "pending.log": b"pending\n",
        })
        implementation = load_ai_commit_module()
        with mock.patch.object(implementation, "STAGED_FAILURE_LOG_COPY_TIMEOUT_SECONDS", 0):
            retained = implementation.preserve_staged_check_logs(
                git_directory, staged_worktree, candidate
            )
        self.assertIsNotNone(retained)
        retained = Path(retained)
        manifest = json.loads((retained / "retention-manifest.json").read_text(encoding="utf-8"))
        self.assertFalse(manifest["complete"])
        self.assertIn("deadline", manifest["truncation_reasons"])
        self.assertFalse((retained / "pending.log").exists())

    def test_failure_log_retention_skips_symlink_logs(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ai-commit-log-target-") as external_dir:
            external_log = Path(external_dir) / "external.log"
            external_log.write_text("do not copy", encoding="utf-8")
            staged_worktree, git_directory, candidate = self.make_failure_log_fixture({
                "kept.log": b"kept\n",
            })
            log_dir = staged_worktree / "build" / "ai-checks"
            try:
                (log_dir / "external.log").symlink_to(external_log)
            except (NotImplementedError, OSError) as error:
                self.skipTest(f"file symlinks are unavailable: {error}")
            implementation = load_ai_commit_module()
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                retained = implementation.preserve_staged_check_logs(
                    git_directory, staged_worktree, candidate
                )
        self.assertIsNotNone(retained)
        retained = Path(retained)
        self.assertFalse((retained / "external.log").exists())
        self.assertEqual((retained / "kept.log").read_bytes(), b"kept\n")
        manifest = json.loads((retained / "retention-manifest.json").read_text(encoding="utf-8"))
        self.assertFalse(manifest["complete"])
        self.assertIn("symlink_log", manifest["truncation_reasons"])
        self.assertIn("partial", stderr.getvalue())

    def test_basic_precommit_phase_timeout_cleans_snapshot_and_descendants(self) -> None:
        (self.root / "script/ai_check_runtime.py").write_text(
            (REPOSITORY / "script/ai_check.py").read_text(encoding="utf-8"),
            encoding="utf-8",
        )
        (self.root / "script/ai_windows_job.py").write_text(
            (REPOSITORY / "script/ai_windows_job.py").read_text(encoding="utf-8"),
            encoding="utf-8",
        )
        source = textwrap.dedent('''
            import os
            from pathlib import Path
            import sys
            from ai_check_runtime import run_phase

            def main(argv):
                root = Path(__file__).resolve().parents[1]
                log_dir = root / "build/ai-checks"
                log_dir.mkdir(parents=True, exist_ok=True)
                child = (
                    "import os,pathlib,time; time.sleep(1.5); "
                    "pathlib.Path(os.environ['AI_TEST_MARKER']).write_text('alive')"
                )
                parent = (
                    "import subprocess,sys,time; "
                    f"subprocess.Popen([sys.executable, '-c', {child!r}]); "
                    "print('parent started', flush=True); time.sleep(30)"
                )
                result = run_phase(
                    root, log_dir, "staged-fast-timeout",
                    [sys.executable, "-c", parent], 1,
                )
                Path(os.environ["AI_TEST_LOG"]).write_text(str(root), encoding="utf-8")
                return result.returncode
        ''')
        checker = self.root / "script/ai_check.py"
        checker.write_text(source, encoding="utf-8")
        self.git("add", "script/ai_check.py", "script/ai_check_runtime.py", "script/ai_windows_job.py")
        self.stage_task()
        candidate = self.candidate()
        self.assertEqual(self.record(candidate).returncode, 0)
        with tempfile.TemporaryDirectory(prefix="ai-commit-timeout-") as log_dir:
            log = Path(log_dir) / "cwd.txt"
            marker = self.root / "descendant-finished"
            index_path = self.root / ".git/index"
            original_index = index_path.read_bytes()
            implementation = load_ai_commit_module()
            stderr = io.StringIO()
            with mock.patch.dict(os.environ, {"AI_TEST_LOG": str(log), "AI_TEST_MARKER": str(marker)}), contextlib.redirect_stderr(stderr):
                result = implementation.precommit(self.root)
            self.assertEqual(result, 124)
            self.assertIn("staged fast-check logs retained at", stderr.getvalue())
            staged_root = Path(log.read_text(encoding="utf-8"))
            retained_log_text = stderr.getvalue().split("staged fast-check logs retained at ", 1)[1].strip().splitlines()[0]
            retained_logs = Path(retained_log_text)
        self.assertFalse(staged_root.exists())
        self.assertFalse((staged_root.parent / "index").exists())
        self.assertEqual(index_path.read_bytes(), original_index)
        timeout_log = retained_logs / "staged-fast-timeout.log"
        self.assertTrue(timeout_log.is_file())
        self.assertIn("parent started", timeout_log.read_text(encoding="utf-8"))
        self.assertEqual(retained_logs.name.split("-")[-2], candidate["tree"][:12])
        self.assertFalse(marker.exists())
        time.sleep(1.7)
        self.assertFalse(marker.exists(), "timed-out phase descendant survived")

    def test_staged_export_preserves_symlink_and_rejects_gitlink(self) -> None:
        link = self.root / "src/link"
        try:
            link.symlink_to("fixture.cpp")
        except (NotImplementedError, OSError):
            self.skipTest("symlinks are unavailable on this host")
        self.git("add", "src/link")
        if not self.git("ls-files", "--stage", "src/link").stdout.startswith("120000 "):
            self.skipTest("Git on this host cannot stage symlinks")
        tree = self.git("write-tree").stdout.strip()
        implementation = load_ai_commit_module()
        with tempfile.TemporaryDirectory(prefix="ai-tree-export-") as target_dir:
            exported = Path(target_dir) / "tree"
            exported.mkdir()
            implementation.materialize_tree(self.root, tree, exported)
            self.assertTrue((exported / "src/link").is_symlink())
            self.assertEqual(os.readlink(exported / "src/link"), "fixture.cpp")
            implementation.validate_export_symlinks(
                exported, deadline=time.monotonic() + 30
            )
            (exported / "internal" / "ai-checks").mkdir(parents=True)
            (exported / "build").symlink_to("internal", target_is_directory=True)
            implementation.validate_export_symlinks(
                exported, deadline=time.monotonic() + 30
            )
            with self.assertRaisesRegex(implementation.CommitGateError, "traverses a symlink"):
                implementation.staged_check_log_directory(exported)
            (exported / "build").unlink()
            (exported / "src/cycle").symlink_to("cycle")
            with self.assertRaisesRegex(implementation.CommitGateError, "cyclic"):
                implementation.validate_export_symlinks(
                    exported, deadline=time.monotonic() + 30
                )

        self.git("update-index", "--add", "--cacheinfo", f"160000,{self.baseline},vendor/core")
        gitlink_tree = self.git("write-tree").stdout.strip()
        with tempfile.TemporaryDirectory(prefix="ai-tree-export-") as target_dir:
            exported = Path(target_dir) / "tree"
            exported.mkdir()
            with self.assertRaisesRegex(implementation.CommitGateError, "gitlink"):
                implementation.materialize_tree(self.root, gitlink_tree, exported)

    def test_export_symlink_validation_obeys_shared_deadline(self) -> None:
        implementation = load_ai_commit_module()
        with tempfile.TemporaryDirectory(prefix="ai-tree-deadline-") as target_dir:
            exported = Path(target_dir) / "tree"
            exported.mkdir()
            link = exported / "internal/link"
            link.parent.mkdir()
            (link.parent / "target").write_text("safe\n", encoding="utf-8")
            try:
                link.symlink_to("target")
            except (NotImplementedError, OSError):
                self.skipTest("symlinks are unavailable on this host")

            implementation.validate_export_symlinks(
                exported, deadline=time.monotonic() + 30
            )

            now = [0.0]
            is_symlink = Path.is_symlink

            def expire_after_link_detection(path: Path) -> bool:
                result = is_symlink(path)
                if path.name == "link" and result:
                    now[0] = 2.0
                return result

            with mock.patch.object(implementation.time, "monotonic", side_effect=lambda: now[0]):
                with mock.patch.object(Path, "is_symlink", new=expire_after_link_detection):
                    with self.assertRaisesRegex(
                        implementation.CommitGateError, "materialization timed out"
                    ):
                        implementation.resolve_export_path(link, deadline=1.0)

            walk = exported / "walk"
            walk.mkdir()
            (walk / "first").write_text("first\n", encoding="utf-8")
            (walk / "second").write_text("second\n", encoding="utf-8")
            first_scan_entry, _ = list(walk.rglob("*"))
            first_scan_entry = first_scan_entry.resolve()
            now[0] = 0.0
            seen: list[Path] = []
            is_symlink = Path.is_symlink

            def expire_after_first_entry(path: Path) -> bool:
                seen.append(path)
                result = is_symlink(path)
                if path == first_scan_entry:
                    now[0] = 2.0
                return result

            with mock.patch.object(implementation.time, "monotonic", side_effect=lambda: now[0]):
                with mock.patch.object(Path, "is_symlink", new=expire_after_first_entry):
                    with self.assertRaisesRegex(
                        implementation.CommitGateError, "materialization timed out"
                    ):
                        implementation.validate_export_symlinks(walk, deadline=1.0)
            self.assertEqual(seen, [first_scan_entry])


class RealAiCommitIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        for name in ("ai_commit.py", "ai_tasks.py", "ai_history.py"):
            source = REPOSITORY / "script" / name
            target = self.root / "script" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(source.read_text(encoding="utf-8"), encoding="utf-8")
        self.git("init", "-b", "master")
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
