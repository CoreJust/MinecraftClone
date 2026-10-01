from __future__ import annotations

import importlib.util
import hashlib
import json
import sys
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "ci/verify_analysis_gate.py"
COMMIT = "a" * 40


def load_module():
    spec = importlib.util.spec_from_file_location("verify_analysis_gate", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class VerifyAnalysisGateTests(unittest.TestCase):
    def setUp(self):
        self.gate = load_module()

    def check_run(self, *, commit: str = COMMIT, status: str = "completed", conclusion: str = "success"):
        return {
            "id": 123,
            "name": self.gate.CHECK_NAME,
            "head_sha": commit,
            "status": status,
            "conclusion": conclusion,
            "started_at": "2026-09-22T10:00:00Z",
            "completed_at": "2026-09-22T10:10:00Z",
            "app": {"slug": "github-actions"},
            "details_url": "https://github.com/CoreJust/MinecraftClone/actions/runs/456/job/123",
        }

    def workflow_run(self, *, path: str = ".github/workflows/ai-checks.yml"):
        return {"id": 456, "path": path, "head_sha": COMMIT, "event": "push", "status": "completed", "conclusion": "success"}

    def receipt(self):
        matrix_script = SCRIPT.parents[1] / "ai_analysis_matrix.py"
        spec = importlib.util.spec_from_file_location("ai_analysis_matrix", matrix_script)
        matrix = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(matrix)
        manifest = matrix.load_manifest()
        candidate = {"head": COMMIT, "tree": "b" * 40}
        return {
            "schema_version": 1,
            "task_id": "MC-AI-0249",
            "manifest_sha256": matrix.manifest_sha256(manifest),
            "candidate": candidate,
            "source": candidate.copy(),
            "rows": [
                {
                    "id": row["id"],
                    "candidate": candidate.copy(),
                    "status": "passed",
                    "flags": row["flags"],
                    "environment": row.get("environment", {}),
                    "tool_versions": {"tool": "tool 1.0"},
                    "commands": row["commands"],
                    "executed_commands": [
                        {
                            "declared_command": command,
                            "argv": command + (
                                ["--verbose"]
                                if row["id"] in matrix.REQUIRED_ANALYSIS_CONFIGURATION
                                and command[:2] == ["cmake", "--build"]
                                else []
                            ),
                            "return_code": 0,
                            "output_sha256": "d" * 64,
                            "output_bytes": 12,
                        }
                        for command in row["commands"]
                    ],
                    "effective_compile_flags": (
                        {
                            "compile_commands_sha256": "c" * 64,
                            "command_count": 1,
                            "observed_flags": row["required_compile_flags"],
                        }
                        if "required_compile_flags" in row
                        else None
                    ),
                    "effective_analysis": (
                        {
                            "configuration_sha256": "e" * 64,
                            "configuration_key": matrix.REQUIRED_ANALYSIS_CONFIGURATION[row["id"]][1],
                            "observed_settings": matrix.REQUIRED_ANALYSIS_CONFIGURATION[row["id"]][2],
                            "invocation_count": 1,
                            "observed_tools_and_flags": matrix.ANALYSIS_TOOL_MARKERS[row["id"]],
                            "build_output_sha256": "f" * 64,
                        }
                        if row["id"] in matrix.REQUIRED_ANALYSIS_CONFIGURATION
                        else None
                    ),
                    "diagnostics": "no diagnostics",
                    "diagnostics_sha256": hashlib.sha256(b"no diagnostics").hexdigest(),
                    "duration_seconds": 1,
                }
                for row in manifest["rows"]
            ],
        }

    def test_gate_rejects_same_named_check_from_other_workflow(self):
        with mock.patch.object(self.gate, "git", side_effect=[COMMIT, "b" * 40]), mock.patch.object(
            self.gate, "github_repository", return_value="CoreJust/MinecraftClone"
        ), mock.patch.object(self.gate, "fetch_check_runs", return_value=[self.check_run()]), mock.patch.object(
            self.gate, "fetch_workflow_run", return_value=self.workflow_run(path=".github/workflows/other.yml"), create=True
        ), mock.patch.object(self.gate, "fetch_aggregate_receipt", return_value=self.receipt(), create=True):
            with self.assertRaisesRegex(self.gate.GateError, "workflow"):
                self.gate.verify_gate(Path("."), COMMIT)

    def test_gate_rejects_missing_or_invalid_aggregate_receipt(self):
        for receipt in (None, {"candidate": {"head": COMMIT, "tree": "b" * 40}, "rows": []}):
            with self.subTest(receipt=receipt), mock.patch.object(self.gate, "git", side_effect=[COMMIT, "b" * 40]), mock.patch.object(
                self.gate, "github_repository", return_value="CoreJust/MinecraftClone"
            ), mock.patch.object(self.gate, "fetch_check_runs", return_value=[self.check_run()]), mock.patch.object(
                self.gate, "fetch_workflow_run", return_value=self.workflow_run(), create=True
            ), mock.patch.object(self.gate, "fetch_aggregate_receipt", return_value=receipt, create=True):
                with self.assertRaisesRegex(self.gate.GateError, "receipt"):
                    self.gate.verify_gate(Path("."), COMMIT)

    def test_remote_parser_supports_ssh_and_https_github_urls(self):
        for remote in (
            "git@github.com:CoreJust/MinecraftClone.git",
            "https://github.com/CoreJust/MinecraftClone.git",
        ):
            with mock.patch.object(self.gate, "git", return_value=remote):
                self.assertEqual(self.gate.github_repository(Path(".")), "CoreJust/MinecraftClone")

    def test_check_run_query_uses_get_with_pagination_fields(self):
        with mock.patch.object(
            self.gate.subprocess,
            "run",
            return_value=self.gate.subprocess.CompletedProcess([], 0, '{"check_runs":[]}', ""),
        ) as run:
            self.gate.fetch_check_runs("CoreJust/MinecraftClone", COMMIT)

        command = run.call_args.args[0]
        self.assertIn("--method", command)
        self.assertEqual(command[command.index("--method") + 1], "GET")
        self.assertIn("-F", command)

    def test_gate_requires_success_for_the_exact_candidate_commit(self):
        page = {"total_count": 1, "check_runs": [self.check_run()]}
        with mock.patch.object(self.gate, "git", side_effect=[COMMIT, "b" * 40]), mock.patch.object(
            self.gate,
            "github_repository",
            return_value="CoreJust/MinecraftClone",
        ), mock.patch.object(self.gate, "fetch_check_runs", return_value=[self.check_run()]), mock.patch.object(
            self.gate, "fetch_workflow_run", return_value=self.workflow_run()
        ), mock.patch.object(self.gate, "fetch_aggregate_receipt", return_value=self.receipt()):
            result = self.gate.verify_gate(Path("."), COMMIT)
        self.assertEqual(result["commit"], COMMIT)
        self.assertEqual(result["conclusion"], "success")
        self.assertEqual(self.gate._check_runs([page]), [page["check_runs"][0]])

    def test_gate_rejects_a_different_commit_or_failed_check(self):
        with mock.patch.object(self.gate, "git", return_value=COMMIT), mock.patch.object(
            self.gate,
            "github_repository",
            return_value="CoreJust/MinecraftClone",
        ), mock.patch.object(self.gate, "fetch_check_runs", return_value=[self.check_run(commit="b" * 40)]):
            with self.assertRaisesRegex(self.gate.GateError, "has not reported"):
                self.gate.verify_gate(Path("."), COMMIT)

        with mock.patch.object(self.gate, "git", return_value=COMMIT), mock.patch.object(
            self.gate,
            "github_repository",
            return_value="CoreJust/MinecraftClone",
        ), mock.patch.object(
            self.gate,
            "fetch_check_runs",
            return_value=[self.check_run(conclusion="failure")],
        ):
            with self.assertRaisesRegex(self.gate.GateError, "not successful"):
                self.gate.verify_gate(Path("."), COMMIT)

    def test_latest_run_wins_even_when_an_older_run_passed(self):
        older = self.check_run()
        newer = self.check_run(status="in_progress", conclusion="")
        newer["started_at"] = "2026-09-22T11:00:00Z"
        with mock.patch.object(self.gate, "git", return_value=COMMIT), mock.patch.object(
            self.gate,
            "github_repository",
            return_value="CoreJust/MinecraftClone",
        ), mock.patch.object(self.gate, "fetch_check_runs", return_value=[older, newer]):
            with self.assertRaisesRegex(self.gate.GateError, "not successful"):
                self.gate.verify_gate(Path("."), COMMIT)

    def test_check_run_pages_fail_closed_when_api_data_is_malformed(self):
        with self.assertRaisesRegex(self.gate.GateError, "malformed"):
            self.gate._check_runs(json.loads(json.dumps({"unexpected": []})))


if __name__ == "__main__":
    unittest.main()
