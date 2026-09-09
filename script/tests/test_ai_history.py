from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPT_DIR))
import ai_history
import ai_tasks


def task(task_id: str, level: str, parent: str = "", **overrides: object) -> dict[str, object]:
    result: dict[str, object] = {
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
    result.update(overrides)
    return result


class AiHistoryTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.repo = Path(self.temporary.name)
        self.git_run("git", "init", "-q")
        self.git_run("git", "config", "user.name", "Test")
        self.git_run("git", "config", "user.email", "test@example.com")
        self.git_run("git", "remote", "add", "origin", "https://github.com/example/history.git")
        (self.repo / ".gitignore").write_text("build/\n", encoding="utf-8")
        self.git_run("git", "add", ".gitignore")
        self.baseline = self.commit("baseline")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def git_run(self, *args: str) -> str:
        return subprocess.run(args, cwd=self.repo, check=True, text=True, capture_output=True).stdout.strip()

    def commit(self, message: str) -> str:
        self.git_run("git", "commit", "--allow-empty", "-q", "-m", message)
        return self.git_run("git", "rev-parse", "HEAD")

    def hierarchy(self, first_status: str = "done") -> list[dict[str, object]]:
        return [
            task("MC-AI-0104", "major"),
            task("MC-AI-0103", "minor", "MC-AI-0104", baseline_commit=self.baseline),
            task("MC-AI-0101", "snapshot", "MC-AI-0103", baseline_commit=self.baseline),
            task(
                "MC-AI-0001", "basic", "MC-AI-0101", status=first_status,
                evidence="passed" if first_status == "done" else "",
                resolved_at="2026-09-09" if first_status == "done" else "",
                resolution_changes="finished" if first_status == "done" else "",
            ),
            task(
                "MC-AI-0002", "basic", "MC-AI-0101", status="done", evidence="passed",
                resolved_at="2026-09-09", resolution_changes="finished",
            ),
        ]

    def test_collect_covers_merged_task_branch_and_ignores_snapshot_commit(self) -> None:
        tasks = self.hierarchy()
        self.git_run("git", "checkout", "-q", "-b", "task-branch")
        self.commit("first task\n\nTask-ID: MC-AI-0001")
        self.git_run("git", "checkout", "-q", "master")
        self.commit("second task\n\nTask-ID: MC-AI-0002")
        self.git_run("git", "merge", "--no-ff", "-q", "task-branch", "-m", "snapshot\n\nTask-ID: MC-AI-0101")

        self.assertEqual(
            ai_history.collect_snapshot(self.repo, tasks, "MC-AI-0101", "HEAD"),
            ["MC-AI-0001", "MC-AI-0002"],
        )

    def test_rejects_unknown_trailer_and_duplicate_legacy_import(self) -> None:
        tasks = self.hierarchy()
        self.commit("unknown\n\nTask-ID: MC-AI-9999")
        with self.assertRaisesRegex(ai_history.HistoryError, "unknown Task-ID"):
            ai_history.trailer_commits(self.repo, tasks)

        tasks.append(task("MC-AI-9999", "basic", "MC-AI-0101"))
        legacy = self.commit("legacy")
        tasks[3]["commits"] = [legacy]
        tasks[4]["commits"] = [legacy]
        with self.assertRaisesRegex(ai_history.HistoryError, "belongs to both"):
            ai_history.trailer_commits(self.repo, tasks)

    def test_collect_rejects_unrelated_baseline(self) -> None:
        tasks = self.hierarchy()
        self.git_run("git", "checkout", "-q", "--orphan", "unrelated")
        unrelated = self.commit("unrelated")
        self.git_run("git", "checkout", "-q", "master")
        with self.assertRaisesRegex(ai_history.HistoryError, "no common ancestry"):
            ai_history.collect_snapshot(self.repo, tasks, "MC-AI-0101", unrelated)

    def test_collect_accepts_divergent_baseline(self) -> None:
        tasks = self.hierarchy()
        self.git_run("git", "checkout", "-q", "-b", "release")
        release_tip = self.commit("release task\n\nTask-ID: MC-AI-0002")
        self.git_run("git", "checkout", "-q", "master")
        self.commit("main task\n\nTask-ID: MC-AI-0001")
        tasks[2]["baseline_commit"] = release_tip

        self.assertEqual(
            ai_history.collect_snapshot(self.repo, tasks, "MC-AI-0101", "HEAD"),
            ["MC-AI-0001"],
        )

    def test_collect_union_includes_both_promotion_heads(self) -> None:
        tasks = self.hierarchy()
        self.git_run("git", "checkout", "-q", "-b", "promotion")
        promotion_head = self.commit("promotion task\n\nTask-ID: MC-AI-0002")
        self.git_run("git", "checkout", "-q", "master")
        self.commit("main task\n\nTask-ID: MC-AI-0001")

        self.assertEqual(
            ai_history.collect_snapshot(
                self.repo, tasks, "MC-AI-0101", "HEAD", promotion_head
            ),
            ["MC-AI-0001", "MC-AI-0002"],
        )

    def test_collect_union_rejects_unassigned_promotion_commit(self) -> None:
        tasks = self.hierarchy()
        self.git_run("git", "checkout", "-q", "-b", "promotion")
        promotion_head = self.commit("unassigned promotion")
        self.git_run("git", "checkout", "-q", "master")
        self.commit("main task\n\nTask-ID: MC-AI-0001")

        with self.assertRaisesRegex(ai_history.HistoryError, "no Task-ID or legacy import"):
            ai_history.collect_snapshot(
                self.repo, tasks, "MC-AI-0101", "HEAD", promotion_head
            )

    def test_collect_aggregate_rejects_unassigned_raw_commit(self) -> None:
        tasks = self.hierarchy()
        self.commit("first task\n\nTask-ID: MC-AI-0001")
        self.commit("second task\n\nTask-ID: MC-AI-0002")
        self.commit("unassigned")

        with self.assertRaisesRegex(ai_history.HistoryError, "no Task-ID or legacy import"):
            ai_history.collect_aggregate(self.repo, tasks, "MC-AI-0103", "HEAD")

    def test_collect_minor_returns_immediate_snapshots_after_coverage_check(self) -> None:
        tasks = self.hierarchy()
        self.commit("first task\n\nTask-ID: MC-AI-0001")
        self.commit("second task\n\nTask-ID: MC-AI-0002")
        self.commit("snapshot\n\nTask-ID: MC-AI-0101")

        self.assertEqual(
            ai_history.collect_aggregate(self.repo, tasks, "MC-AI-0103", "HEAD"),
            ["MC-AI-0101"],
        )

    def test_refresh_keeps_backlog_clean_and_renders_live_backlinks(self) -> None:
        tasks = self.hierarchy()
        sha = self.commit("first task\n\nTask-ID: MC-AI-0001")
        ai_tasks.validate_backlog(tasks)

        output = self.repo / "build/ai-tasks"
        ai_history.render_records(self.repo, tasks, output)

        rendered = (output / "MC-AI-0001.md").read_text(encoding="utf-8")
        self.assertIn(f"https://github.com/CoreJust/MinecraftClone/commit/{sha}", rendered)
        self.assertEqual(self.git_run("git", "status", "--porcelain"), "")

    def test_finalize_rejects_missing_or_undone_planned_basic_task(self) -> None:
        tasks = self.hierarchy(first_status="ready")
        self.commit("first task\n\nTask-ID: MC-AI-0001")
        snapshot = tasks[2]
        snapshot.update({
            "product_changes": ["None"],
            "code_changes": ["None"],
            "evidence": "checked",
            "resolved_at": "2026-09-09",
            "resolution_changes": "prepared",
        })

        with self.assertRaisesRegex(ai_history.HistoryError, "missing"):
            ai_history.finalize(self.repo, tasks, "MC-AI-0101", "HEAD")
        self.commit("second task\n\nTask-ID: MC-AI-0002")
        with self.assertRaisesRegex(ai_history.HistoryError, "not done"):
            ai_history.finalize(self.repo, tasks, "MC-AI-0101", "HEAD")

    def test_finalize_assigns_unparented_completed_basic_tasks(self) -> None:
        tasks = self.hierarchy()
        tasks[3]["parent"] = ""
        tasks[4]["parent"] = ""
        self.commit("first task\n\nTask-ID: MC-AI-0001")
        self.commit("second task\n\nTask-ID: MC-AI-0002")
        tasks[2].update({
            "product_changes": ["None"],
            "code_changes": ["None"],
            "evidence": "checked",
            "resolved_at": "2026-09-09",
            "resolution_changes": "prepared",
        })

        ai_history.finalize(self.repo, tasks, "MC-AI-0101", "HEAD")

        self.assertTrue(tasks[2]["finalized"])
        self.assertEqual(tasks[3]["parent"], "MC-AI-0101")
        self.assertEqual(tasks[4]["parent"], "MC-AI-0101")

    def test_finalize_rejects_empty_snapshot_and_defaults_head(self) -> None:
        tasks = self.hierarchy()
        tasks[2].update({
            "product_changes": ["None"],
            "code_changes": ["None"],
            "evidence": "checked",
            "resolved_at": "2026-09-09",
            "resolution_changes": "prepared",
        })

        with self.assertRaisesRegex(ai_history.HistoryError, "has no basic task commits"):
            ai_history.finalize(self.repo, tasks, "MC-AI-0101", "HEAD")
        self.assertEqual(ai_history.parser().parse_args(["collect", "MC-AI-0101"]).head, "HEAD")
        self.assertEqual(ai_history.parser().parse_args(["finalize", "MC-AI-0101"]).head, "HEAD")


if __name__ == "__main__":
    unittest.main()
