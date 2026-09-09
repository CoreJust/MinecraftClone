#!/usr/bin/env python3
"""Run a project Codex task with a selected model and high reasoning effort."""

from __future__ import annotations

import argparse
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Sequence


MODELS = {
    "astra": "gpt-6-astra",
    "sol": "gpt-5.6-sol",
    "terra": "gpt-5.6-terra",
    "luna": "gpt-5.6-luna",
}
REPOSITORY_ROOT = Path(__file__).resolve().parent.parent


def make_command(route: str, codex_args: Sequence[str]) -> list[str]:
    return [
        "codex",
        "--model",
        MODELS[route],
        "-c",
        'model_reasoning_effort="high"',
        *codex_args,
    ]


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true", help="print the command without launching Codex")
    parser.add_argument("route", choices=tuple(MODELS), help="model route to use")
    parser.add_argument("codex_args", nargs=argparse.REMAINDER, help="arguments forwarded to Codex")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    command = make_command(args.route, args.codex_args)
    if args.dry_run:
        print(shlex.join(command))
        return 0
    try:
        completed = subprocess.run(command, cwd=REPOSITORY_ROOT, check=False, shell=False)
    except FileNotFoundError:
        print("ai_run: codex executable not found on PATH", file=sys.stderr)
        return 127
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
