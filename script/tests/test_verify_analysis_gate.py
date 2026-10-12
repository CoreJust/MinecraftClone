from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[2]
GATE = REPOSITORY / "script/ci/verify_analysis_gate.py"
MANIFEST = REPOSITORY / "script/ai_analysis_matrix.json"
CANDIDATE = {"head": "a" * 40, "tree": "b" * 40}


def load_module():
    spec = importlib.util.spec_from_file_location("verify_analysis_gate", GATE)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class VerifyAnalysisGateTests(unittest.TestCase):
    def setUp(self):
        self.gate = load_module()

    def _paths(self, root: Path) -> tuple[Path, Path]:
        receipt = root / "aggregate.json"
        receipt.write_text("{}\n", encoding="utf-8")
        return root, receipt

    def test_verifier_passes_absolute_offline_aggregate_for_exact_candidate(self):
        with tempfile.TemporaryDirectory() as directory:
            root, receipt = self._paths(Path(directory))
            verified = {
                "candidate": CANDIDATE,
                "rows": [{"id": f"row-{index}"} for index in range(11)],
            }
            with mock.patch.object(self.gate._MATRIX, "candidate_identity", return_value=CANDIDATE), mock.patch.object(
                self.gate._MATRIX, "verify_receipt", return_value=verified
            ) as verify_receipt, mock.patch.object(
                self.gate._MATRIX.subprocess, "run", side_effect=AssertionError("unexpected subprocess")
            ) as process:
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    result = self.gate.main(
                        ["--root", str(root), "--receipt", str(receipt), "--manifest", str(MANIFEST)]
                    )

            self.assertEqual(result, 0)
            self.assertEqual(json.loads(output.getvalue())["rows"], [f"row-{index}" for index in range(11)])
            verify_receipt.assert_called_once_with(
                receipt.resolve(), MANIFEST.resolve(), root=root.resolve(), expected_candidate=CANDIDATE
            )
            process.assert_not_called()
            self.assertFalse(hasattr(self.gate, "fetch_check_runs"))

    def test_candidate_commit_is_authenticated_separately_from_source_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root, receipt = self._paths(Path(directory))
            with mock.patch.object(self.gate._MATRIX, "candidate_identity", side_effect=AssertionError("HEAD must not substitute")), mock.patch.object(
                self.gate._MATRIX, "commit_identity", return_value=CANDIDATE
            ) as commit_identity, mock.patch.object(
                self.gate._MATRIX, "verify_receipt", return_value={"candidate": CANDIDATE, "rows": []}
            ) as verify_receipt:
                result = self.gate.main(
                    [
                        "--root",
                        str(root),
                        "--receipt",
                        str(receipt),
                        "--manifest",
                        str(MANIFEST),
                        "--candidate-commit",
                        CANDIDATE["head"],
                    ]
                )

            self.assertEqual(result, 0)
            commit_identity.assert_called_once_with(root.resolve(), CANDIDATE["head"])
            verify_receipt.assert_called_once_with(
                receipt.resolve(), MANIFEST.resolve(), root=root.resolve(), expected_candidate=CANDIDATE
            )

    def test_relative_manifest_is_rejected_and_malformed_absolute_manifest_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root, receipt = self._paths(Path(directory))
            with mock.patch.object(self.gate._MATRIX, "verify_receipt") as verify_receipt:
                output = io.StringIO()
                with contextlib.redirect_stderr(output):
                    result = self.gate.main(
                        ["--root", str(root), "--receipt", str(receipt), "--manifest", "relative.json"]
                    )
            self.assertEqual(result, 1)
            self.assertIn("--manifest must be an absolute path", output.getvalue())
            verify_receipt.assert_not_called()

            malformed = root / "malformed.json"
            malformed.write_text('{"schema_version": 1, "rows": []}\n', encoding="utf-8")
            with mock.patch.object(self.gate._MATRIX, "candidate_identity", return_value=CANDIDATE):
                output = io.StringIO()
                with contextlib.redirect_stderr(output):
                    result = self.gate.main(
                        ["--root", str(root), "--receipt", str(receipt), "--manifest", str(malformed)]
                    )
            self.assertEqual(result, 1)
            self.assertIn("schema_version must be 2", output.getvalue())

    def test_missing_receipt_fails_without_polling_or_external_calls(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch.object(self.gate._MATRIX, "verify_receipt") as verify_receipt:
                output = io.StringIO()
                with contextlib.redirect_stderr(output):
                    result = self.gate.main(
                        ["--root", str(root), "--receipt", str(root / "missing.json")]
                    )
            self.assertEqual(result, 1)
            self.assertIn("missing.json", output.getvalue())
            verify_receipt.assert_not_called()


if __name__ == "__main__":
    unittest.main()
