from __future__ import annotations

import io
import json
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

from script.ci import clang_static_gate as gate


class ClangStaticGateTests(unittest.TestCase):
    def invoke(self, root: Path, returncode: int = 0, internal_failure: bool = False,
               report_created: bool = True, analyzed: bool = True):
        source = root / "source with spaces" / "input;literal.cpp"
        source.parent.mkdir(exist_ok=True)
        source.write_text("int value = 1;\n", encoding="utf-8")
        output = root / "build with spaces" / "input.o"
        captured = []

        def run(command, **kwargs):
            captured.append(command)
            reports = Path(command[command.index("-o") + 1])
            if analyzed:
                (reports.parent / "analysis.jsonl").write_text(json.dumps({"argv": ["clang++", "-cc1", "-analyze"], "returncode": 0}) + "\n")
            if report_created:
                report = reports / "fresh-run"
                report.mkdir(parents=True)
                (report / "index.html").write_text("no bugs", encoding="utf-8")
                if internal_failure:
                    failures = report / "failures"
                    failures.mkdir()
                    (failures / "tu.info.txt").write_text("source\nOther Error\n", encoding="utf-8")
                    (failures / "tu.stderr.txt").write_text("header error", encoding="utf-8")
            kwargs["stdout"].write(b"scan-build output\n")
            return subprocess.CompletedProcess(command, returncode)

        with patch.object(gate, "_executable", side_effect=lambda value: value), \
                patch.object(gate.subprocess, "run", side_effect=run), redirect_stdout(io.StringIO()):
            status = gate.main(["--", "scan-build", "--status-bugs", "-v", "--", "clang++",
                                "-nostdinc++", "-c", str(source), "-o", str(output)])
        receipts = list(output.parent.glob("clang-static-*/receipt.json"))
        return status, json.loads(receipts[0].read_text()), captured

    def test_clean_run_retains_hashed_reports_and_special_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            status, receipt, commands = self.invoke(Path(directory))
            self.assertEqual(status, 0)
            self.assertEqual(receipt["status"], "passed")
            self.assertEqual(receipt["internal_failures"], [])
            self.assertTrue(all(len(item["sha256"]) == 64 for item in receipt["artifacts"]))
            self.assertIn("--status-bugs", commands[0])
            self.assertIn("-v", commands[0])
            self.assertEqual(Path(commands[0][commands[0].index("-c") + 1]).resolve(), Path(receipt["source"]))

    def test_zero_tool_exit_with_internal_failure_fails_and_preserves_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            status, receipt, _ = self.invoke(Path(directory), internal_failure=True)
            self.assertEqual(status, 1)
            self.assertEqual(receipt["returncode"], 0)
            self.assertEqual(receipt["status"], "failed")
            self.assertEqual(len(receipt["internal_failures"]), 1)
            self.assertTrue(any(item["relative"].endswith("tu.stderr.txt") for item in receipt["artifacts"]))

    def test_bug_exit_propagates(self):
        with tempfile.TemporaryDirectory() as directory:
            status, receipt, _ = self.invoke(Path(directory), returncode=1)
            self.assertEqual(status, 1)
            self.assertEqual(receipt["status"], "failed")

    def test_compile_failure_exit_propagates(self):
        with tempfile.TemporaryDirectory() as directory:
            status, receipt, _ = self.invoke(Path(directory), returncode=7)
            self.assertEqual(status, 7)
            self.assertEqual(receipt["returncode"], 7)

    def test_missing_report_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            status, receipt, _ = self.invoke(Path(directory), report_created=False)
            self.assertEqual(status, 1)
            self.assertEqual(receipt["status"], "failed")

    def test_compiler_only_success_without_analysis_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            status, receipt, _ = self.invoke(Path(directory), analyzed=False)
            self.assertEqual(status, 1)
            self.assertEqual(receipt["analyses"], [])

    def test_driver_and_cc1_launcher_preserve_exact_compiler_and_arguments(self):
        with tempfile.TemporaryDirectory(prefix="static gate ") as directory:
            root = Path(directory)
            compiler = root / "compiler;literal"
            observed = root / "observed.json"
            compiler.write_text(f"#!{sys.executable}\nimport json,sys\n"
                                f"open({str(observed)!r}, 'w').write(json.dumps(sys.argv[1:]))\n")
            compiler.chmod(0o700)
            launcher = root / "clang++"
            gate._launcher(launcher, str(compiler), True)
            for arguments, expected in ((["-###", "path with spaces.cpp"], ["-nostdinc++", "-###", "path with spaces.cpp"]),
                                        (["-cc1", "-analyze"], ["-cc1", "-analyze"])):
                subprocess.run([str(launcher), *arguments], check=True)
                self.assertEqual(json.loads(observed.read_text()), expected)
            gate._launcher(launcher, str(compiler), False)
            subprocess.run([str(launcher), "-###"], check=True)
            self.assertEqual(json.loads(observed.read_text()), ["-###"])

    def test_invalid_invocation_fails_before_launch(self):
        with patch.object(gate.subprocess, "run") as run:
            self.assertEqual(gate.main([]), 2)
            run.assert_not_called()

    def test_missing_tool_fails_before_launch(self):
        with patch.object(gate.shutil, "which", return_value=None), patch.object(gate.subprocess, "run") as run:
            self.assertEqual(gate.main(["--", "missing", "--status-bugs", "-v", "--", "clang++"]), 2)
            run.assert_not_called()

    def test_missing_compiler_fails_before_launch(self):
        with patch.object(gate.shutil, "which", side_effect=["/tool/scan-build", None]), patch.object(gate.subprocess, "run") as run:
            self.assertEqual(gate.main(["--", "scan-build", "--status-bugs", "-v", "--", "missing"]), 2)
            run.assert_not_called()

    def test_missing_source_or_output_arguments_fail_before_launch(self):
        with patch.object(gate, "_executable", side_effect=lambda value: value), patch.object(gate.subprocess, "run") as run:
            for arguments in (["-o", "build/file.o"], ["-c", "missing.cpp"]):
                self.assertEqual(gate.main(["--", "scan-build", "--status-bugs", "-v", "--", "clang++", *arguments]), 2)
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
