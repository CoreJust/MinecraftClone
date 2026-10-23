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

    def s7_version(self, root):
        project = root / "src/shared/include/shared/ProjectInfo.hpp"
        project.parent.mkdir(parents=True, exist_ok=True)
        project.write_text(
            'constexpr std::string_view MAJOR_VERSION_NAME{ "EarlyDev" };\n'
            'constexpr std::string_view MINOR_VERSION_NAME{ "Initiation" };\n'
            "constexpr ProjectVersion PROJECT_VERSION{\n"
            "    .epoch = 0,\n"
            "    .major = 1,\n"
            "    .minor = 0,\n"
            "    .patch = 7,\n"
            "};\n",
            encoding="utf-8",
        )
        (root / "vcpkg.json").write_text('{"version-string": "0.1.0"}\n', encoding="utf-8")

    def _paths(self, root: Path) -> tuple[Path, Path]:
        self.s7_version(root)
        receipt = root / "aggregate.json"
        receipt.write_text("{}\n", encoding="utf-8")
        return root, receipt

    def _secondary_aggregate(self) -> dict:
        manifest = self.gate._MATRIX.load_manifest(MANIFEST)
        row_ids = self.gate.secondary_row_ids(manifest)
        return {
            "schema_version": self.gate._MATRIX.SCHEMA_VERSION,
            "task_id": self.gate._MATRIX.TASK_ID,
            "manifest_sha256": self.gate._MATRIX.manifest_sha256(manifest),
            "candidate": CANDIDATE,
            "source": CANDIDATE,
            "runner": {"name": "analysis-matrix-merge", "rows": len(row_ids)},
            "rows": [{"id": row_id} for row_id in row_ids],
        }

    def test_verifier_passes_absolute_offline_aggregate_for_exact_candidate(self):
        with tempfile.TemporaryDirectory() as directory:
            root, receipt = self._paths(Path(directory))
            (root / "src/shared/include/shared/ProjectInfo.hpp").unlink()
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
            self.s7_version(root)
            with mock.patch.object(self.gate._MATRIX, "verify_receipt") as verify_receipt:
                output = io.StringIO()
                with contextlib.redirect_stderr(output):
                    result = self.gate.main(
                        ["--root", str(root), "--receipt", str(root / "missing.json")]
                    )
            self.assertEqual(result, 1)
            self.assertIn("missing.json", output.getvalue())
            verify_receipt.assert_not_called()

    def test_s7_windows_secondary_checks_mac_and_linux_rows_and_reports_windows_deferred(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.s7_version(root)
            receipt = root / "aggregate.json"
            receipt.write_text(json.dumps(self._secondary_aggregate()), encoding="utf-8")
            with mock.patch.object(self.gate._MATRIX, "candidate_identity", return_value=CANDIDATE), mock.patch.object(
                self.gate._MATRIX, "validate_receipt", side_effect=lambda value, *args, **kwargs: value
            ) as validate_row:
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    result = self.gate.main([
                        "--root", str(root), "--receipt", str(receipt), "--manifest", str(MANIFEST),
                        "--s7-windows-secondary",
                    ])

            self.assertEqual(result, 0)
            summary = json.loads(output.getvalue())
            self.assertEqual(summary["windows"]["status"], "deferred")
            self.assertTrue(any(row.startswith("macos_") for row in summary["rows"]))
            self.assertTrue(any(row.startswith("linux_") for row in summary["rows"]))
            self.assertFalse(any(row.startswith("windows_") for row in summary["rows"]))
            self.assertEqual(validate_row.call_count, len(summary["rows"]))

    def test_s7_secondary_rejects_actual_other_version_and_missing_owner_before_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.s7_version(root)
            project = root / "src/shared/include/shared/ProjectInfo.hpp"
            original = project.read_text()
            for content in (original.replace(".patch = 7", ".patch = 8"),
                            original.replace(".minor = 0", ".minor = 1"), None):
                if content is None:
                    project.unlink()
                else:
                    project.write_text(content)
                for action in (lambda: self.gate.verify_secondary_receipt(root / "missing", MANIFEST, root, CANDIDATE),
                               lambda: self.gate.merge_secondary_receipts([], root / "output", MANIFEST, root, CANDIDATE)):
                    with self.assertRaisesRegex(ValueError, "actual candidate version"):
                        action()
            self.assertFalse((root / "output").exists())

    def test_full_analysis_profile_still_rejects_snapshot_aggregate_without_windows_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.s7_version(root)
            aggregate = self._secondary_aggregate()
            aggregate["runner"]["rows"] = len(self.gate._MATRIX.REQUIRED_ROW_IDS)
            receipt = root / "aggregate.json"
            receipt.write_text(json.dumps(aggregate), encoding="utf-8")

            with mock.patch.object(self.gate._MATRIX, "_validate_row_receipt"):
                with self.assertRaisesRegex(self.gate._MATRIX.MatrixError, "missing rows: .*windows"):
                    self.gate._MATRIX.verify_receipt(
                        receipt,
                        MANIFEST,
                        root=root,
                        expected_candidate=CANDIDATE,
                    )

    def test_s7_windows_secondary_rejects_missing_mac_rows_and_windows_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.s7_version(root)
            aggregate = self._secondary_aggregate()
            aggregate["rows"] = [row for row in aggregate["rows"] if row["id"] != "macos_asan"]
            receipt = root / "aggregate.json"
            receipt.write_text(json.dumps(aggregate), encoding="utf-8")
            with mock.patch.object(self.gate._MATRIX, "validate_receipt"):
                with self.assertRaisesRegex(self.gate._MATRIX.MatrixError, "missing selected S7 rows"):
                    self.gate.verify_secondary_receipt(receipt, MANIFEST, root, CANDIDATE)

            aggregate = self._secondary_aggregate()
            aggregate["rows"] = [
                {"id": "windows_msvc_asan"} if row["id"] == "macos_asan" else row
                for row in aggregate["rows"]
            ]
            receipt.write_text(json.dumps(aggregate), encoding="utf-8")
            with mock.patch.object(self.gate._MATRIX, "validate_receipt"):
                with self.assertRaisesRegex(self.gate._MATRIX.MatrixError, "unexpected analysis row under S7"):
                    self.gate.verify_secondary_receipt(receipt, MANIFEST, root, CANDIDATE)

    def test_s7_windows_secondary_merges_only_local_mac_and_linux_row_receipts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.s7_version(root)
            manifest = self.gate._MATRIX.load_manifest(MANIFEST)
            manifest_rows = {row["id"]: row for row in manifest["rows"]}
            manifest_hash = self.gate._MATRIX.manifest_sha256(manifest)
            row_paths = []
            for row_id in self.gate.secondary_row_ids(manifest):
                row_receipt = {
                    "schema_version": self.gate._MATRIX.SCHEMA_VERSION,
                    "task_id": self.gate._MATRIX.TASK_ID,
                    "manifest_sha256": manifest_hash,
                    "candidate": CANDIDATE,
                    "source": CANDIDATE,
                    "runner": {
                        "name": "analysis-matrix-row",
                        "host_platform": manifest_rows[row_id]["platform"],
                        "row": row_id,
                    },
                    "rows": [{"id": row_id}],
                }
                row_path = root / f"{row_id}.json"
                row_path.write_text(json.dumps(row_receipt), encoding="utf-8")
                row_paths.append(row_path)

            with mock.patch.object(
                self.gate._MATRIX, "validate_local_receipt", side_effect=lambda value, *args, **kwargs: value
            ), mock.patch.object(
                self.gate._MATRIX, "validate_receipt", side_effect=lambda value, *args, **kwargs: value
            ):
                output = root / "selected-aggregate.json"
                verified = self.gate.merge_secondary_receipts(row_paths, output, MANIFEST, root, CANDIDATE)
                self.assertEqual(
                    {row["id"] for row in verified["rows"]},
                    set(self.gate.secondary_row_ids(manifest)),
                )
                self.assertTrue(output.is_file())

                windows_id = next(row_id for row_id, row in manifest_rows.items() if row["platform"] == "windows")
                windows_receipt = dict(json.loads(row_paths[0].read_text(encoding="utf-8")))
                windows_receipt["runner"] = {
                    "name": "analysis-matrix-row",
                    "host_platform": "windows",
                    "row": windows_id,
                }
                windows_receipt["rows"] = [{"id": windows_id}]
                windows_path = root / "windows.json"
                windows_path.write_text(json.dumps(windows_receipt), encoding="utf-8")
                with self.assertRaisesRegex(self.gate._MATRIX.MatrixError, "Windows or unknown"):
                    self.gate.merge_secondary_receipts(
                        [*row_paths, windows_path], root / "with-windows.json", MANIFEST, root, CANDIDATE
                    )

    def test_s7_windows_secondary_refreshes_aggregate_candidate_for_unchanged_local_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.s7_version(root)
            manifest = self.gate._MATRIX.load_manifest(MANIFEST)
            manifest_rows = {row["id"]: row for row in manifest["rows"]}
            manifest_hash = self.gate._MATRIX.manifest_sha256(manifest)
            row_paths = []
            for row_id in self.gate.secondary_row_ids(manifest):
                row_receipt = {
                    "schema_version": self.gate._MATRIX.SCHEMA_VERSION,
                    "task_id": self.gate._MATRIX.TASK_ID,
                    "manifest_sha256": manifest_hash,
                    "candidate": CANDIDATE,
                    "source": CANDIDATE,
                    "runner": {
                        "name": "analysis-matrix-row",
                        "host_platform": manifest_rows[row_id]["platform"],
                        "row": row_id,
                    },
                    "rows": [{"id": row_id}],
                }
                row_path = root / f"{row_id}.json"
                row_path.write_text(json.dumps(row_receipt), encoding="utf-8")
                row_paths.append(row_path)

            refreshed_candidate = {"head": "c" * 40, "tree": "d" * 40}
            with mock.patch.object(
                self.gate._MATRIX, "_validate_row_receipt"
            ) as validate_row:
                output = root / "refreshed-aggregate.json"
                verified = self.gate.merge_secondary_receipts(
                    row_paths, output, MANIFEST, root, refreshed_candidate
                )

            self.assertGreaterEqual(validate_row.call_count, len(row_paths))
            self.assertEqual(verified["candidate"], refreshed_candidate)
            self.assertEqual(json.loads(output.read_text(encoding="utf-8"))["source"], refreshed_candidate)

    def test_s7_windows_secondary_rejects_stale_local_inputs_before_writing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.s7_version(root)
            manifest = self.gate._MATRIX.load_manifest(MANIFEST)
            manifest_rows = {row["id"]: row for row in manifest["rows"]}
            row_id = self.gate.secondary_row_ids(manifest)[0]
            row_receipt = {
                "schema_version": self.gate._MATRIX.SCHEMA_VERSION,
                "task_id": self.gate._MATRIX.TASK_ID,
                "manifest_sha256": self.gate._MATRIX.manifest_sha256(manifest),
                "candidate": CANDIDATE,
                "source": CANDIDATE,
                "runner": {
                    "name": "analysis-matrix-row",
                    "host_platform": manifest_rows[row_id]["platform"],
                    "row": row_id,
                },
                "rows": [{"id": row_id}],
            }
            row_path = root / f"{row_id}.json"
            row_path.write_text(json.dumps(row_receipt), encoding="utf-8")
            output = root / "must-not-exist.json"

            with mock.patch.object(
                self.gate._MATRIX,
                "_validate_row_receipt",
                side_effect=self.gate._MATRIX.MatrixError(
                    f"analysis row {row_id} has stale source or configuration inputs"
                ),
            ) as validate_row:
                with self.assertRaisesRegex(self.gate._MATRIX.MatrixError, "stale source or configuration inputs"):
                    self.gate.merge_secondary_receipts([row_path], output, MANIFEST, root, CANDIDATE)

            validate_row.assert_called_once()
            self.assertFalse(output.exists())

    def test_s7_windows_secondary_rejects_aggregate_candidate_and_source_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.s7_version(root)
            receipt = root / "aggregate.json"
            for field, mismatched_candidate in (
                ("candidate", {"head": "c" * 40, "tree": "d" * 40}),
                ("source", {"head": "c" * 40, "tree": "d" * 40}),
            ):
                aggregate = self._secondary_aggregate()
                aggregate[field] = mismatched_candidate
                receipt.write_text(json.dumps(aggregate), encoding="utf-8")
                with mock.patch.object(self.gate._MATRIX, "validate_receipt"):
                    with self.assertRaisesRegex(
                        self.gate._MATRIX.MatrixError,
                        "analysis receipt does not match the requested candidate",
                    ):
                        self.gate.verify_secondary_receipt(receipt, MANIFEST, root, CANDIDATE)


if __name__ == "__main__":
    unittest.main()
