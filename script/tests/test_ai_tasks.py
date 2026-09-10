from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPT_DIR))
import ai_tasks


def make_task(task_id: str = "MC-AI-0001", **overrides: object) -> dict[str, object]:
    task = {
        "id": task_id, "title": "Task", "kind": "chore", "status": "backlog",
        "priority": "P2", "route": "terra", "milestone": "Milestone", "depends_on": [],
        "acceptance": "Acceptance", "evidence": "", "blocker": "", "owner": "",
        "level": "basic", "parent": "", "motivation": "Why this task matters.",
        "context": "What existed before this task.", "complexity": "medium",
        "created_at": "2026-09-09", "resolved_at": "", "resolution_changes": "",
        "plan": [], "product_changes": [], "code_changes": [], "baseline_commit": "",
        "commits": [], "finalized": False, "retrospective": "", "docs_review": "",
        "environment_review": "", "backlog_review": "",
    }
    task.update(overrides)
    return task


def done_task(task_id: str, **overrides: object) -> dict[str, object]:
    return make_task(task_id, status="done", evidence="Focused checks passed.", resolved_at="2026-09-09", resolution_changes="Implemented the requested slice.", **overrides)


class AiTasksTest(unittest.TestCase):
    def test_validation_rejects_invalid_metadata_dates_and_done_state(self) -> None:
        malformed = make_task()
        del malformed["motivation"]
        with self.assertRaisesRegex(ai_tasks.BacklogError, "missing fields: motivation"):
            ai_tasks.validate_backlog([malformed])
        unknown = make_task(extra="value")
        with self.assertRaisesRegex(ai_tasks.BacklogError, "unknown fields: extra"):
            ai_tasks.validate_backlog([unknown])
        for field in ("title", "milestone", "acceptance", "motivation", "context"):
            with self.subTest(field=field):
                with self.assertRaisesRegex(ai_tasks.BacklogError, f"{field} must be non-empty"):
                    ai_tasks.validate_backlog([make_task(**{field: "  "})])
        for field, value in (("complexity", "other"), ("created_at", "09-09-2026"), ("baseline_commit", "deadbeef")):
            with self.subTest(field=field):
                with self.assertRaises(ai_tasks.BacklogError):
                    ai_tasks.validate_backlog([make_task(**{field: value})])
        with self.assertRaisesRegex(ai_tasks.BacklogError, "commits must contain"):
            ai_tasks.validate_backlog([make_task(commits=["abc"])])
        with self.assertRaisesRegex(ai_tasks.BacklogError, "cannot precede"):
            ai_tasks.validate_backlog([make_task(resolved_at="2026-09-08")])
        with self.assertRaisesRegex(ai_tasks.BacklogError, "no resolved_at date"):
            ai_tasks.validate_backlog([make_task(status="done", evidence="recorded", resolution_changes="done")])
        with self.assertRaisesRegex(ai_tasks.BacklogError, "no resolution_changes"):
            ai_tasks.validate_backlog([make_task(status="done", evidence="recorded", resolved_at="2026-09-09")])
        for status, extra in (
            ("active", {"owner": "Codex"}),
            ("ready", {}),
            ("blocked", {"blocker": "Awaiting evidence."}),
        ):
            with self.subTest(status=status):
                with self.assertRaisesRegex(ai_tasks.BacklogError, "not a done snapshot"):
                    ai_tasks.validate_backlog([
                        make_task(
                            status=status,
                            level="snapshot",
                            resolved_at="2026-09-09",
                            **extra,
                        )
                    ])

    def test_hierarchy_and_aggregate_finalization_contracts(self) -> None:
        major = make_task("MC-AI-0001", level="major")
        minor = make_task("MC-AI-0002", level="minor", parent="MC-AI-0001")
        snapshot = make_task("MC-AI-0003", level="snapshot", parent="MC-AI-0002")
        basic = make_task("MC-AI-0004", level="basic", parent="MC-AI-0003", status="active", owner="owner")
        ai_tasks.validate_backlog([major, minor, snapshot, basic])
        with self.assertRaisesRegex(ai_tasks.BacklogError, "parent is required"):
            ai_tasks.validate_backlog([make_task(level="snapshot")])
        with self.assertRaisesRegex(ai_tasks.BacklogError, "must have level snapshot"):
            ai_tasks.validate_backlog([make_task("MC-AI-0001"), make_task("MC-AI-0002", level="basic", parent="MC-AI-0001")])
        unfinished_parent = done_task("MC-AI-0003", level="snapshot", parent="MC-AI-0002", finalized=True, product_changes=["None—aggregate only."], code_changes=["None—aggregate only."])
        with self.assertRaisesRegex(ai_tasks.BacklogError, "unfinished children"):
            ai_tasks.validate_backlog([major, minor, unfinished_parent, basic])
        complete_child = done_task("MC-AI-0004", level="basic", parent="MC-AI-0003")
        with self.assertRaisesRegex(ai_tasks.BacklogError, "not finalized"):
            ai_tasks.validate_backlog([major, minor, done_task("MC-AI-0003", level="snapshot", parent="MC-AI-0002"), complete_child])
        finalized_parent = done_task("MC-AI-0003", level="snapshot", parent="MC-AI-0002", finalized=True, product_changes=["Delivered the snapshot."], code_changes=["Updated task records."])
        ai_tasks.validate_backlog([major, minor, finalized_parent, complete_child])
        finalized_minor = make_task("MC-AI-0002", level="minor", parent="MC-AI-0001", finalized=True)
        with self.assertRaisesRegex(ai_tasks.BacklogError, "no retrospective"):
            ai_tasks.validate_backlog([major, finalized_minor])

    def test_done_aggregate_requires_finalization_children_and_change_lists(self) -> None:
        major = make_task("MC-AI-0001", level="major")
        minor = make_task("MC-AI-0002", level="minor", parent=major["id"])
        snapshot = done_task("MC-AI-0003", level="snapshot", parent=minor["id"])
        records = [major, minor, snapshot]
        with self.assertRaisesRegex(ai_tasks.BacklogError, "not finalized"):
            ai_tasks.validate_backlog(records)
        snapshot["finalized"] = True
        with self.assertRaisesRegex(ai_tasks.BacklogError, "no children"):
            ai_tasks.validate_backlog(records)
        records.append(done_task("MC-AI-0004", parent=snapshot["id"]))
        with self.assertRaisesRegex(ai_tasks.BacklogError, "no product_changes"):
            ai_tasks.validate_backlog(records)
        snapshot["product_changes"] = ["Product result"]
        snapshot["code_changes"] = ["Code result"]
        ai_tasks.validate_backlog(records)

    def test_ready_keeps_dependency_semantics(self) -> None:
        complete = done_task("MC-AI-0001")
        actionable = make_task("MC-AI-0002", status="ready", priority="P1", depends_on=["MC-AI-0001"])
        backlog = make_task("MC-AI-0003", depends_on=["MC-AI-0002"])
        self.assertEqual([task["id"] for task in ai_tasks.ready_tasks([complete, actionable, backlog])], ["MC-AI-0002"])
        invalid = make_task("MC-AI-0004", status="ready", depends_on=["MC-AI-0002"])
        with self.assertRaisesRegex(ai_tasks.BacklogError, "unfinished dependency MC-AI-0002"):
            ai_tasks.validate_backlog([complete, actionable, invalid])

    def test_render_task_and_check_include_generated_task_documents(self) -> None:
        sha = "a" * 40
        parent = make_task("MC-AI-0001", level="major", plan=["Plan the work."], product_changes=["Product outcome."], code_changes=["Code outcome."], commits=[sha])
        child = make_task("MC-AI-0002", level="minor", parent="MC-AI-0001")
        rendered = ai_tasks.render_task(parent, [parent, child])
        self.assertIn("| Motivation | Why this task matters. |", rendered)
        self.assertIn("- MC-AI-0002: Task", rendered)
        self.assertLess(rendered.index("## Product changes"), rendered.index("## Code changes"))
        self.assertIn(f"https://github.com/CoreJust/MinecraftClone/commit/{sha}", rendered)
        self.assertIn("Commit lookup: python3 script/ai_history.py show MC-AI-0001", rendered)
        override = ai_tasks.render_task(parent, [parent, child], commits=["b" * 40])
        self.assertIn("b" * 40, override)
        self.assertNotIn(f"[{sha}]", override)
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            backlog_path = temporary / "backlog.json"
            markdown_path = temporary / "portable" / "BACKLOG.md"
            backlog_path.write_text(json.dumps([parent, child]), encoding="utf-8")
            ai_tasks.write_rendered_backlog(markdown_path, [parent, child])
            self.assertTrue((markdown_path.parent / "tasks" / "MC-AI-0001.md").exists())
            ai_tasks.check(backlog_path, markdown_path)
            (markdown_path.parent / "tasks" / "MC-AI-0001.md").write_text("stale\n", encoding="utf-8")
            with self.assertRaisesRegex(ai_tasks.BacklogError, "generated task Markdown is stale"):
                ai_tasks.check(backlog_path, markdown_path)

    def test_cli_add_and_update_append_lists_without_clearing(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            backlog_path = temporary / "backlog.json"
            markdown_path = temporary / "BACKLOG.md"
            backlog_path.write_text(json.dumps([make_task()]), encoding="utf-8")
            command = [sys.executable, str(SCRIPT_DIR / "ai_tasks.py"), "--backlog", str(backlog_path), "--markdown", str(markdown_path)]
            add = command + ["add", "New task", "--kind", "decision", "--priority", "P0", "--milestone", "Now", "--acceptance", "Record it", "--level", "basic", "--motivation", "Need a decision.", "--context", "No prior decision.", "--complexity", "low", "--created-at", "2026-09-09", "--plan", "first", "--plan", "second"]
            added = subprocess.run(add, check=True, text=True, capture_output=True)
            self.assertEqual(added.stdout.strip(), "MC-AI-0002")
            subprocess.run(command + ["update", "MC-AI-0002", "--plan", "third", "--product-change", "product", "--code-change", "code"], check=True, text=True, capture_output=True)
            tasks = ai_tasks.load_backlog(backlog_path)
            self.assertEqual(tasks[1]["plan"], ["first", "second", "third"])
            self.assertEqual(tasks[1]["route"], "luna")
            self.assertEqual(tasks[1]["product_changes"], ["product"])
            self.assertEqual(tasks[1]["code_changes"], ["code"])
            ai_tasks.check(backlog_path, markdown_path)

    def test_compact_packet_contains_only_current_task_and_candidate_state(self) -> None:
        task = make_task("MC-AI-0007", baseline_commit="b" * 40, plan=["Run focused checks."])
        with mock.patch.object(
            ai_tasks,
            "_git_output",
            side_effect=["a" * 40, "c" * 40],
        ), mock.patch.object(ai_tasks, "changed_paths", return_value=["script/ai_tasks.py"]):
            packet = ai_tasks.compact_task_packet(
                [task],
                task["id"],
                root=Path("/tmp/repository"),
                next_commands=["python3 script/ai_check.py --fast"],
            )
        self.assertEqual(packet["schema_version"], 1)
        self.assertEqual(packet["task"]["id"], "MC-AI-0007")
        self.assertEqual(packet["candidate"], {
            "task_id": "MC-AI-0007",
            "head_sha": "a" * 40,
            "index_tree": "c" * 40,
            "baseline_commit": "b" * 40,
        })
        self.assertEqual(packet["changed_paths"], ["script/ai_tasks.py"])
        self.assertEqual(packet["next_commands"], ["python3 script/ai_check.py --fast"])
        self.assertNotIn("context", packet["task"])

    def test_ci_record_rejects_mismatched_or_nonterminal_runs(self) -> None:
        successful = {
            "databaseId": 42,
            "headSha": "a" * 40,
            "status": "completed",
            "conclusion": "success",
            "workflowName": "checks",
            "url": "https://example.invalid/run/42",
            "artifacts": [{"id": 7, "name": "diagnostics"}],
        }
        record = ai_tasks.build_ci_record([successful], "a" * 40, "checks")
        self.assertEqual(record["schema_version"], 1)
        self.assertEqual(record["runs"][0]["artifacts"][0]["id"], 7)
        for override, message in (
            ({"headSha": "b" * 40}, "head does not match"),
            ({"status": "in_progress"}, "completed success"),
            ({"artifacts": [{"name": "missing-id"}]}, "no ID"),
        ):
            with self.subTest(message=message):
                candidate = {**successful, **override}
                with self.assertRaisesRegex(ai_tasks.BacklogError, message):
                    ai_tasks.build_ci_record([candidate], "a" * 40, "checks")

    def test_collect_ci_record_queries_read_only_exact_commit_and_artifacts(self) -> None:
        summary = {
            "databaseId": 42,
            "headSha": "a" * 40,
        }
        details = {
            **summary,
            "status": "completed",
            "conclusion": "success",
            "workflowName": "checks",
            "artifacts": [{"id": 9, "name": "diagnostics"}],
        }
        with mock.patch.object(ai_tasks, "gh_json", side_effect=[ [summary], details]) as gh:
            record = ai_tasks.collect_ci_record(Path("/tmp/repository"), "a" * 40, "checks")
        self.assertEqual(record["runs"][0]["artifacts"][0]["id"], 9)
        list_call = gh.call_args_list[0].args[1]
        self.assertEqual(list_call[list_call.index("--commit") + 1], "a" * 40)
        self.assertIn("--workflow", list_call)

        with mock.patch.object(ai_tasks, "gh_json", return_value=[{**summary, "headSha": "b" * 40}]):
            with self.assertRaisesRegex(ai_tasks.BacklogError, "different commit SHA"):
                ai_tasks.collect_ci_record(Path("/tmp/repository"), "a" * 40)

    def test_immutable_record_does_not_overwrite(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "record.json"
            payload = {"schema_version": 1}
            ai_tasks.write_immutable_json(target, payload)
            with self.assertRaisesRegex(ai_tasks.BacklogError, "overwrite immutable"):
                ai_tasks.write_immutable_json(target, payload)

    def test_efficiency_cohort_selects_only_next_basic_tasks_and_reports_missing_metrics(self) -> None:
        baseline = make_task("MC-AI-0089", status="active", owner="Codex")
        first = make_task("MC-AI-0090", status="ready")
        second = make_task("MC-AI-0091")
        aggregate = make_task("MC-AI-0092", level="snapshot", parent="MC-AI-0093")
        parent = make_task("MC-AI-0093", level="minor", parent="MC-AI-0094")
        major = make_task("MC-AI-0094", level="major")
        records = [baseline, first, second, aggregate, parent, major]
        with tempfile.TemporaryDirectory() as directory:
            receipt_dir = Path(directory)
            (receipt_dir / "summary.json").write_text(json.dumps({
                "schema_version": 1,
                "scope": "local",
                "root": {"head_sha": "a" * 40, "index_tree": "b" * 40},
                "executed_phases": ["python-tests"],
            }), encoding="utf-8")
            with mock.patch.object(
                ai_tasks,
                "committed_task_ids",
                return_value=(["MC-AI-0090", "MC-AI-0091", "MC-AI-0092"], []),
            ):
                report = ai_tasks.efficiency_cohort(
                    records,
                    "c" * 40,
                    known_invocations=["python3 script/ai_check.py --fast"],
                    test_commands=["python3 -m unittest script.tests.test_ai_tasks"],
                    receipt_dir=receipt_dir,
                )
        self.assertEqual([record["task_id"] for record in report["tasks"]], ["MC-AI-0090", "MC-AI-0091"])
        self.assertEqual(report["baseline"], {"task_id": "MC-AI-0089", "commit": "c" * 40})
        self.assertTrue(report["coverage"]["partial"])
        self.assertNotIn("task_join", report["coverage"]["missing_metrics"])
        self.assertEqual(report["coverage"]["unmatched_check_summaries"], 0)

    def test_efficiency_cohort_joins_taskless_summary_by_commit_identity(self) -> None:
        records = [
            make_task("MC-AI-0089", status="active", owner="Codex"),
            make_task("MC-AI-0090", status="ready"),
        ]
        parent_head = "a" * 40
        commit_tree = "b" * 40
        with tempfile.TemporaryDirectory() as directory:
            receipt_dir = Path(directory)
            (receipt_dir / "summary.json").write_text(json.dumps({
                "schema_version": 1,
                "status": "PASS",
                "elapsed_ms": 123,
                "model_usage": {"input": 10, "output": 2},
                "root": {"head_sha": parent_head, "index_tree": commit_tree},
            }), encoding="utf-8")
            with mock.patch.object(
                ai_tasks,
                "committed_task_ids",
                return_value=(["MC-AI-0090"], []),
            ), mock.patch.object(
                ai_tasks,
                "committed_task_identities",
                return_value={"MC-AI-0090": {"parent_head": parent_head, "commit_tree": commit_tree}},
            ):
                report = ai_tasks.efficiency_cohort(records, "c" * 40, receipt_dir=receipt_dir)
        self.assertEqual(len(report["tasks"][0]["check_summaries"]), 1)
        self.assertEqual(report["coverage"]["unmatched_check_summaries"], 0)
        self.assertNotIn("task_join", report["coverage"]["missing_metrics"])

    def test_efficiency_cohort_ignores_out_of_cohort_taskless_summary_identity(self) -> None:
        records = [
            make_task("MC-AI-0089", status="active", owner="Codex"),
            make_task("MC-AI-0090", status="ready"),
        ]
        with tempfile.TemporaryDirectory() as directory:
            receipt_dir = Path(directory)
            (receipt_dir / "summary.json").write_text(json.dumps({
                "schema_version": 1,
                "status": "PASS",
                "root": {"head_sha": "a" * 40, "index_tree": "c" * 40},
            }), encoding="utf-8")
            with mock.patch.object(
                ai_tasks,
                "committed_task_ids",
                return_value=(["MC-AI-0090"], []),
            ), mock.patch.object(
                ai_tasks,
                "committed_task_identities",
                return_value={"MC-AI-0090": {"parent_head": "a" * 40, "commit_tree": "b" * 40}},
            ):
                report = ai_tasks.efficiency_cohort(records, "c" * 40, receipt_dir=receipt_dir)
        self.assertEqual(report["tasks"][0]["check_summaries"], [])
        self.assertEqual(report["coverage"]["unmatched_check_summaries"], 0)
        self.assertNotIn("task_join", report["coverage"]["missing_metrics"])

    def test_efficiency_cohort_reads_summary_history_and_deduplicates_latest_copy(self) -> None:
        records = [
            make_task("MC-AI-0089", status="active", owner="Codex"),
            make_task("MC-AI-0090", status="ready"),
            make_task("MC-AI-0091", status="ready"),
        ]
        first = {
            "invocation_id": "first",
            "scope": "metadata-only",
            "head": "a" * 40,
            "index_tree": "b" * 40,
            "executed_phases": ["docs"],
        }
        second = {
            "invocation_id": "second",
            "scope": "metadata-only",
            "head": "c" * 40,
            "index_tree": "d" * 40,
            "executed_phases": ["docs"],
        }
        latest_second = {**second, "reused_phases": ["docs"]}
        with tempfile.TemporaryDirectory() as directory:
            receipt_dir = Path(directory)
            (receipt_dir / "summary.jsonl").write_text(
                json.dumps(first) + "\nnot-json\n" + json.dumps(second) + "\n",
                encoding="utf-8",
            )
            (receipt_dir / "summary.json").write_text(json.dumps(latest_second), encoding="utf-8")
            with mock.patch.object(
                ai_tasks,
                "committed_task_ids",
                return_value=(["MC-AI-0090", "MC-AI-0091"], []),
            ), mock.patch.object(
                ai_tasks,
                "committed_task_identities",
                return_value={
                    "MC-AI-0090": {"parent_head": "a" * 40, "commit_tree": "b" * 40},
                    "MC-AI-0091": {"parent_head": "c" * 40, "commit_tree": "d" * 40},
                },
            ):
                report = ai_tasks.efficiency_cohort(records, "e" * 40, receipt_dir=receipt_dir)
        self.assertEqual([len(record["check_summaries"]) for record in report["tasks"]], [1, 1])
        self.assertEqual(report["tasks"][1]["check_summaries"][0]["reused_phases"], ["docs"])
        self.assertEqual(report["coverage"]["unmatched_check_summaries"], 0)
        self.assertNotIn("task_join", report["coverage"]["missing_metrics"])

    def test_committed_task_ids_preserve_commit_order_and_reject_ambiguous_trailers(self) -> None:
        output = "\0".join((
            "a" * 40,
            "MC-AI-0091",
            "b" * 40,
            "MC-AI-0092\nMC-AI-0093",
        ))
        with mock.patch.object(ai_tasks, "_git_output", return_value=output):
            task_ids, unavailable = ai_tasks.committed_task_ids(Path("/tmp/repository"), "c" * 40)
        self.assertEqual(task_ids, ["MC-AI-0091"])
        self.assertEqual(unavailable, ["b" * 40])

    def test_efficiency_cohort_marks_missing_baseline_history_unavailable(self) -> None:
        records = [make_task("MC-AI-0089", status="active", owner="Codex")]
        with mock.patch.object(ai_tasks, "committed_task_ids", side_effect=ai_tasks.BacklogError("missing")):
            report = ai_tasks.efficiency_cohort(records, "c" * 40)
        self.assertEqual(report["tasks"], [])
        self.assertIn("task_mapping", report["coverage"]["missing_metrics"])
        self.assertEqual(report["coverage"]["unavailable_commits"], ["c" * 40])


if __name__ == "__main__":
    unittest.main()
