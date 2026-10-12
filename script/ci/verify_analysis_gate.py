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
MATRIX_PATH = SCRIPT_ROOT / "ai_analysis_matrix.py"
_SPEC = importlib.util.spec_from_file_location("ai_analysis_matrix", MATRIX_PATH)
if _SPEC is None or _SPEC.loader is None:
    raise RuntimeError(f"cannot load analysis matrix module: {MATRIX_PATH}")
_MATRIX = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_MATRIX)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd(), help="candidate source tree")
    parser.add_argument("--receipt", required=True, type=Path, help="offline merged local receipt")
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
        receipt = args.receipt.resolve(strict=True)
        if args.candidate_commit is None:
            expected_candidate = _MATRIX.candidate_identity(root)
        else:
            expected_candidate = _MATRIX.commit_identity(root, args.candidate_commit)
        verified = _MATRIX.verify_receipt(
            receipt,
            manifest,
            root=root,
            expected_candidate=expected_candidate,
        )
        print(
            json.dumps(
                {
                    "status": "passed",
                    "candidate": verified["candidate"],
                    "receipt": str(receipt),
                    "rows": [row["id"] for row in verified["rows"]],
                },
                sort_keys=True,
            )
        )
        return 0
    except (OSError, ValueError, _MATRIX.MatrixError) as error:
        print(f"FAIL analysis gate: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
