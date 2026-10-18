#!/usr/bin/env python3
"""Verify the offline local S7 analysis aggregate for an exact candidate."""

from __future__ import annotations

import argparse
import importlib.util
import json
import sys
from pathlib import Path
from typing import Sequence


SCRIPT_ROOT = Path(__file__).resolve().parents[1]
_CHECK_SPEC = importlib.util.spec_from_file_location("analysis_ai_check", SCRIPT_ROOT / "ai_check.py")
assert _CHECK_SPEC is not None and _CHECK_SPEC.loader is not None
_CHECK = importlib.util.module_from_spec(_CHECK_SPEC)
sys.modules[_CHECK_SPEC.name] = _CHECK
_CHECK_SPEC.loader.exec_module(_CHECK)
MATRIX_PATH = SCRIPT_ROOT / "ai_analysis_matrix.py"
_SPEC = importlib.util.spec_from_file_location("ai_analysis_matrix", MATRIX_PATH)
if _SPEC is None or _SPEC.loader is None:
    raise RuntimeError(f"cannot load analysis matrix module: {MATRIX_PATH}")
_MATRIX = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_MATRIX)


def secondary_row_ids(manifest: dict) -> tuple[str, ...]:
    rows = _MATRIX.validate_manifest(manifest)["rows"]
    return tuple(row["id"] for row in rows if row["platform"] != "windows")


def validate_secondary_row(
    row: dict,
    manifest: dict,
    candidate: dict,
    root: Path,
    manifest_hash: str,
) -> None:
    row_id = row.get("id")
    manifest_row = next((item for item in manifest["rows"] if item["id"] == row_id), None)
    if manifest_row is None or manifest_row["platform"] == "windows":
        raise _MATRIX.MatrixError(f"unexpected analysis row under S7 Windows-secondary policy: {row_id}")
    wrapped_receipt = {
        "schema_version": _MATRIX.SCHEMA_VERSION,
        "task_id": _MATRIX.TASK_ID,
        "manifest_sha256": manifest_hash,
        "candidate": candidate,
        "source": candidate,
        "runner": {
            "name": "analysis-matrix-row",
            "host_platform": manifest_row["platform"],
            "row": row_id,
            "rows": 1,
        },
        "rows": [row],
    }
    _MATRIX.validate_receipt(
        wrapped_receipt,
        manifest,
        candidate,
        root=root,
        expected_runner="analysis-matrix-row",
        expected_row_ids=(row_id,),
    )


def verify_secondary_receipt(
    receipt_path: Path,
    manifest_path: Path,
    root: Path,
    expected_candidate: dict,
) -> dict:
    _CHECK.require_s7_candidate(root)
    manifest = _MATRIX.load_manifest(manifest_path)
    checked_manifest = _MATRIX.validate_manifest(manifest)
    aggregate = _MATRIX._read_json(receipt_path, "analysis receipt")
    if not isinstance(aggregate, dict) or aggregate.get("schema_version") != _MATRIX.SCHEMA_VERSION:
        raise _MATRIX.MatrixError(f"analysis receipt schema_version must be {_MATRIX.SCHEMA_VERSION}")
    if aggregate.get("task_id") != _MATRIX.TASK_ID:
        raise _MATRIX.MatrixError(f"analysis receipt task_id must be {_MATRIX.TASK_ID}")
    if aggregate.get("manifest_sha256") != _MATRIX.manifest_sha256(checked_manifest):
        raise _MATRIX.MatrixError("analysis receipt manifest does not match the current matrix")
    candidate = _MATRIX._validate_candidate(aggregate.get("candidate"))
    source = _MATRIX._validate_candidate(aggregate.get("source"), "source")
    if source != candidate or candidate != _MATRIX._validate_candidate(expected_candidate, "expected_candidate"):
        raise _MATRIX.MatrixError("analysis receipt does not match the requested candidate")
    runner = aggregate.get("runner")
    if not isinstance(runner, dict) or runner != {
        "name": "analysis-matrix-merge",
        "rows": len(secondary_row_ids(checked_manifest)),
    }:
        raise _MATRIX.MatrixError("analysis receipt does not declare the selected S7 row set")
    rows = aggregate.get("rows")
    expected_ids = set(secondary_row_ids(checked_manifest))
    if not isinstance(rows, list):
        raise _MATRIX.MatrixError("analysis receipt does not contain every selected S7 row")
    seen: set[str] = set()
    for row in rows:
        if not isinstance(row, dict):
            raise _MATRIX.MatrixError("analysis receipt row must be an object")
        row_id = row.get("id")
        if row_id in seen:
            raise _MATRIX.MatrixError(f"analysis receipt has duplicate row {row_id}")
        seen.add(row_id)
        validate_secondary_row(row, checked_manifest, candidate, root, aggregate["manifest_sha256"])
    missing = sorted(expected_ids - seen)
    if missing:
        raise _MATRIX.MatrixError("analysis receipt is missing selected S7 rows: " + ", ".join(missing))
    if seen != expected_ids:
        raise _MATRIX.MatrixError("analysis receipt row set does not match the selected S7 policy")
    return {"candidate": candidate, "rows": rows}


def merge_secondary_receipts(
    receipt_paths: Sequence[Path],
    output_path: Path,
    manifest_path: Path,
    root: Path,
    expected_candidate: dict,
) -> dict:
    _CHECK.require_s7_candidate(root)
    manifest = _MATRIX.load_manifest(manifest_path)
    checked_manifest = _MATRIX.validate_manifest(manifest)
    required_ids = secondary_row_ids(checked_manifest)
    required = set(required_ids)
    manifest_hash = _MATRIX.manifest_sha256(checked_manifest)
    aggregate_candidate = _MATRIX._validate_candidate(expected_candidate, "expected_candidate")
    if output_path.exists() or output_path.is_symlink() or not output_path.parent.is_dir():
        raise _MATRIX.MatrixError("output must be a new file in an existing directory")
    merged: dict[str, dict] = {}
    for receipt_path in receipt_paths:
        receipt = _MATRIX._read_json(receipt_path, "analysis row receipt")
        _MATRIX.validate_local_receipt(receipt, checked_manifest, root=root)
        if receipt.get("manifest_sha256") != manifest_hash:
            raise _MATRIX.MatrixError("analysis row receipt manifest does not match the current matrix")
        candidate = _MATRIX._validate_candidate(receipt.get("candidate"))
        for row in receipt["rows"]:
            row_id = row["id"]
            if row_id not in required:
                raise _MATRIX.MatrixError(f"Windows or unknown analysis row is not accepted under S7 policy: {row_id}")
            if row_id in merged:
                raise _MATRIX.MatrixError(f"duplicate analysis row receipt: {row_id}")
            validate_secondary_row(row, checked_manifest, candidate, root, manifest_hash)
            merged[row_id] = row
    missing = sorted(required - set(merged))
    if missing:
        raise _MATRIX.MatrixError("analysis row receipts are missing selected S7 rows: " + ", ".join(missing))
    aggregate = {
        "schema_version": _MATRIX.SCHEMA_VERSION,
        "task_id": _MATRIX.TASK_ID,
        "manifest_sha256": manifest_hash,
        "candidate": aggregate_candidate,
        "source": dict(aggregate_candidate),
        "runner": {"name": "analysis-matrix-merge", "rows": len(required_ids)},
        "rows": [merged[row_id] for row_id in required_ids],
    }
    with output_path.open("x", encoding="utf-8") as output:
        output.write(json.dumps(aggregate, indent=2, sort_keys=True) + "\n")
    return verify_secondary_receipt(output_path, manifest_path, root, aggregate_candidate)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd(), help="candidate source tree")
    parser.add_argument("--receipt", required=True, type=Path, help="offline merged local receipt")
    parser.add_argument("--merge", nargs="+", type=Path, help="merge selected local row receipts into a new aggregate")
    parser.add_argument(
        "--s7-windows-secondary",
        action="store_true",
        help="for Snapshot 7 only, require Mac and Linux analysis rows and report Windows as deferred",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        help="absolute manifest path (defaults to ROOT/script/ai_analysis_matrix.json)",
    )
    parser.add_argument(
        "--candidate-commit",
        help="full immutable candidate commit SHA; source inputs are checked against ROOT",
    )
    args = parser.parse_args(argv)

    try:
        root = args.root.resolve(strict=True)
        if not root.is_dir():
            raise _MATRIX.MatrixError(f"candidate root is not a directory: {root}")
        if args.manifest is not None:
            if not args.manifest.is_absolute():
                raise _MATRIX.MatrixError("--manifest must be an absolute path")
            manifest = args.manifest.resolve(strict=True)
        else:
            manifest = root / "script" / "ai_analysis_matrix.json"
        if args.merge and not args.s7_windows_secondary:
            raise _MATRIX.MatrixError("--merge requires the explicit --s7-windows-secondary profile")
        receipt_inputs = [path.resolve(strict=True) for path in args.merge] if args.merge else None
        receipt = args.receipt.absolute() if args.merge else args.receipt.resolve(strict=True)
        if args.candidate_commit is None:
            expected_candidate = _MATRIX.candidate_identity(root)
        else:
            expected_candidate = _MATRIX.commit_identity(root, args.candidate_commit)
        if args.s7_windows_secondary:
            if receipt_inputs is not None:
                verified = merge_secondary_receipts(
                    receipt_inputs,
                    receipt,
                    manifest,
                    root,
                    expected_candidate,
                )
            else:
                verified = verify_secondary_receipt(receipt, manifest, root, expected_candidate)
        else:
            verified = _MATRIX.verify_receipt(
                receipt,
                manifest,
                root=root,
                expected_candidate=expected_candidate,
            )
        result = {
            "status": "passed",
            "candidate": verified["candidate"],
            "receipt": str(receipt),
            "rows": [row["id"] for row in verified["rows"]],
        }
        if args.s7_windows_secondary:
            result["windows"] = {
                "status": "deferred",
                "reason": "Windows MSVC ASan and /analyze rows were not selected for this S7 snapshot.",
            }
        print(
            json.dumps(result, sort_keys=True)
        )
        return 0
    except (OSError, ValueError, _MATRIX.MatrixError) as error:
        print(f"FAIL analysis gate: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
