from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPT_DIR))
import ai_plan


def task(number: int, level: str = "basic", parent: str = "", status: str = "backlog", **overrides: object) -> dict[str, object]:
    record = {
        "id": f"MC-AI-{number:04d}", "title": f"Task {number}", "kind": "chore",
        "status": status, "priority": "P2", "route": "terra", "milestone": "Unassigned",
        "depends_on": [], "acceptance": "Acceptance", "evidence": "", "blocker": "",
        "owner": "Codex" if status == "active" else "", "level": level, "parent": parent,
        "motivation": "Motivation", "context": "Context", "complexity": "medium",
        "created_at": "2026-09-09", "resolved_at": "", "resolution_changes": "",
        "plan": [], "product_changes": [], "code_changes": [], "baseline_commit": "",
        "commits": [], "finalized": False, "retrospective": "", "docs_review": "",
        "environment_review": "", "backlog_review": "",
    }
    if status == "done":
        record.update(evidence="passed", resolved_at="2026-09-09", resolution_changes="completed")
    record.update(overrides)
    return record


def hierarchy(*basic_tasks: dict[str, object]) -> list[dict[str, object]]:
    return [
        task(1, "major", status="active", title="EarlyDev 0.1"),
        task(2, "minor", "MC-AI-0001", "active", title="EarlyDev 0.1.0"),
        task(3, "snapshot", "MC-AI-0002", "active", title="EarlyDev 0.1.0:3"),
        *basic_tasks,
    ]


class AiPlanTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.backlog = self.root / "backlog.json"
        self.markdown = self.root / "docs" / "BACKLOG.md"
        self.current = self.root / "docs" / "current.json"
        self.current.parent.mkdir(parents=True)
        self.current.write_text(json.dumps({"snapshot": "MC-AI-0003", "minor": "MC-AI-0002", "major": "MC-AI-0001"}), encoding="utf-8")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write(self, tasks: list[dict[str, object]]) -> None:
        self.backlog.write_text(json.dumps(tasks), encoding="utf-8")

    def run_cli(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([sys.executable, str(SCRIPT_DIR / "ai_plan.py"), "--backlog", str(self.backlog), "--markdown", str(self.markdown), "--current", str(self.current), *arguments], text=True, capture_output=True)

    def test_numeric_task_and_version_selection_hide_storage_ids(self) -> None:
        self.write(hierarchy(task(4)))
        shown = self.run_cli("task", "4")
        self.assertEqual(shown.returncode, 0, shown.stderr)
        self.assertTrue(shown.stdout.startswith("# Task 4 — Task 4"))
        self.assertIn("## Plan", shown.stdout)
        self.assertIn("## Children", shown.stdout)
        snapshot = self.run_cli("snapshot", "0.1.0:3")
        self.assertEqual(snapshot.returncode, 0, snapshot.stderr)
        self.assertTrue(snapshot.stdout.startswith("# Task 3 — EarlyDev 0.1.0:3"))
        completed = hierarchy(task(4, parent="MC-AI-0003", status="done"))
        completed[2].update(status="done", finalized=True, product_changes=["closed"], code_changes=["closed"], evidence="closed", resolved_at="2026-09-09", resolution_changes="closed")
        self.write(completed)
        historic = self.run_cli("snapshot", "0.1.0:3")
        self.assertEqual(historic.returncode, 0, historic.stderr)
        self.assertIn("| Status | done |", historic.stdout)

    def test_backlog_lists_only_unassigned_unfinished_basic_tasks(self) -> None:
        self.write(hierarchy(task(4), task(5, parent="MC-AI-0003"), task(6, status="done")))
        listed = self.run_cli("backlog")
        self.assertEqual(listed.returncode, 0, listed.stderr)
        self.assertIn("4\tbasic", listed.stdout)
        self.assertNotIn("5\tbasic", listed.stdout)
        self.assertNotIn("6\tbasic", listed.stdout)

    def test_implement_requires_prepare_and_leaves_json_unchanged_on_failure(self) -> None:
        blocked = task(5, depends_on=["MC-AI-0004"])
        self.write(hierarchy(task(4, status="backlog"), blocked))
        original = self.backlog.read_text(encoding="utf-8")
        without_prepare = self.run_cli("implement", "5")
        self.assertNotEqual(without_prepare.returncode, 0)
        self.assertIn("requires --prepare", without_prepare.stderr)
        self.assertEqual(self.backlog.read_text(encoding="utf-8"), original)
        with_prepare = self.run_cli("implement", "5", "--prepare")
        self.assertNotEqual(with_prepare.returncode, 0)
        self.assertIn("unfinished dependencies: 4", with_prepare.stderr)
        self.assertEqual(self.backlog.read_text(encoding="utf-8"), original)

    def test_implement_prepares_basic_and_aggregate_commands_order_dependencies(self) -> None:
        first = task(4, parent="MC-AI-0003")
        second = task(5, parent="MC-AI-0003", depends_on=["MC-AI-0004"])
        self.write(hierarchy(first, second))
        pending = self.run_cli("implement-snapshot")
        self.assertEqual(pending.returncode, 0, pending.stderr)
        self.assertLess(pending.stdout.index("4\tbasic"), pending.stdout.index("5\tbasic"))
        self.assertIn("deps=4", pending.stdout)
        prepared = self.run_cli("implement", "Prepare a fresh task", "--prepare")
        self.assertEqual(prepared.returncode, 0, prepared.stderr)
        records = json.loads(self.backlog.read_text(encoding="utf-8"))
        prepared_task = next(record for record in records if record["id"] == "MC-AI-0006")
        self.assertEqual(prepared_task["parent"], "MC-AI-0003")
        self.assertEqual(prepared_task["status"], "active")

    def test_missing_or_ambiguous_current_selection_is_rejected(self) -> None:
        self.write(hierarchy(task(4)))
        self.current.unlink()
        missing = self.run_cli("snapshot")
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn("cannot read", missing.stderr)
        self.current.write_text(json.dumps({"snapshot": "MC-AI-0003", "minor": "MC-AI-0002", "major": "MC-AI-0001"}), encoding="utf-8")
        second_snapshot = task(5, "snapshot", "MC-AI-0002", "active", title="Other 0.1.0:4")
        self.write(hierarchy(task(4), second_snapshot))
        ambiguous = self.run_cli("snapshot")
        self.assertNotEqual(ambiguous.returncode, 0)
        self.assertIn("current snapshot is ambiguous", ambiguous.stderr)

    def test_plan_for_and_select_are_deterministic(self) -> None:
        self.write(hierarchy(task(4)))
        created = self.run_cli("plan-for", "Document a new capability")
        self.assertEqual(created.returncode, 0, created.stderr)
        self.assertEqual(created.stdout.strip(), "5")
        records = json.loads(self.backlog.read_text(encoding="utf-8"))
        planned = next(record for record in records if record["id"] == "MC-AI-0005")
        self.assertEqual((planned["level"], planned["parent"], planned["status"]), ("basic", "", "backlog"))
        selected = self.run_cli("select", "snapshot", "3")
        self.assertEqual(selected.returncode, 0, selected.stderr)
        self.assertEqual(selected.stdout.strip(), "snapshot\t3")

    def test_successor_selection_clears_incompatible_descendants_and_check_allows_blanks(self) -> None:
        closed_major = task(1, "major", status="done", finalized=True, product_changes=["closed"], code_changes=["closed"], retrospective="review", docs_review="review", environment_review="review", backlog_review="review")
        closed_minor = task(2, "minor", "MC-AI-0001", "done", finalized=True, product_changes=["closed"], code_changes=["closed"], retrospective="review", docs_review="review", environment_review="review", backlog_review="review")
        closed_snapshot = task(3, "snapshot", "MC-AI-0002", "done", finalized=True, product_changes=["closed"], code_changes=["closed"])
        successor_major = task(4, "major", status="active", title="Next 0.2")
        successor_minor = task(5, "minor", "MC-AI-0004", "active", title="Next 0.2.0")
        self.write([closed_major, closed_minor, closed_snapshot, successor_major, successor_minor, task(6, parent="MC-AI-0003", status="done")])
        selected_major = self.run_cli("select", "major", "4")
        self.assertEqual(selected_major.returncode, 0, selected_major.stderr)
        self.assertEqual(json.loads(self.current.read_text(encoding="utf-8")), {"snapshot": "", "minor": "", "major": "MC-AI-0004"})
        selected_minor = self.run_cli("select", "minor", "5")
        self.assertEqual(selected_minor.returncode, 0, selected_minor.stderr)
        self.assertEqual(json.loads(self.current.read_text(encoding="utf-8")), {"snapshot": "", "minor": "MC-AI-0005", "major": "MC-AI-0004"})
        checked = self.run_cli("check")
        self.assertEqual(checked.returncode, 0, checked.stderr)
        self.current.write_text(json.dumps({"snapshot": "MC-AI-0003", "minor": "MC-AI-0002", "major": "MC-AI-0001"}), encoding="utf-8")
        cleared = self.run_cli("clear", "major")
        self.assertEqual(cleared.returncode, 0, cleared.stderr)
        self.assertEqual(json.loads(self.current.read_text(encoding="utf-8")), {"snapshot": "", "minor": "", "major": ""})


if __name__ == "__main__":
    unittest.main()
