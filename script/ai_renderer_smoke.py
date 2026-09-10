#!/usr/bin/env python3
"""Run the bounded noninteractive renderer acceptance test for release gates."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


DEFAULT_PRESET = "renderer-smoke"
DEFAULT_TIMEOUT = 840
CONFIGURE_TIMEOUT = 180
BUILD_TIMEOUT = 600
TEST_TIMEOUT = 90
RECEIPT_PATH = Path("build/ai-checks/renderer-smoke.log")


@dataclass
class CommandResult:
    command: Sequence[str]
    returncode: int
    output: str
    timed_out: bool = False


def repository_root() -> Path:
    return Path(__file__).resolve().parents[1]


def cache_path(root: Path, preset: str) -> Path:
    return root / "build" / preset / "CMakeCache.txt"


def cache_value(cache: Path, key: str) -> str | None:
    if not cache.is_file():
        return None
    prefix = f"{key}:"
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(prefix) and "=" in line:
            return line.partition("=")[2]
    return None


def smoke_cache_is_usable(root: Path, preset: str) -> bool:
    cache = cache_path(root, preset)
    source_directory = cache_value(cache, "CMAKE_HOME_DIRECTORY")
    if source_directory is None:
        return False
    if Path(source_directory).resolve() != root:
        raise RuntimeError(
            f"{cache} belongs to {source_directory}, not this checkout; refusing to reuse it"
        )
    return cache_value(cache, "MC_ENABLE_RENDERER_SMOKE") == "ON"


def run_command(root: Path, command: Sequence[str], timeout: int) -> CommandResult:
    try:
        completed = subprocess.run(
            command,
            cwd=root,
            text=True,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
        return CommandResult(command, completed.returncode, completed.stdout + completed.stderr)
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout or ""
        stderr = error.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode(errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode(errors="replace")
        return CommandResult(command, 124, stdout + stderr, timed_out=True)
    except OSError as error:
        return CommandResult(command, 127, str(error))


def write_receipt(receipt: Path, results: Sequence[CommandResult], failure: str | None) -> None:
    receipt.parent.mkdir(parents=True, exist_ok=True)
    lines = []
    for result in results:
        lines.append("$ " + " ".join(result.command))
        lines.append(result.output.rstrip())
        lines.append(f"exit {result.returncode}{' (timeout)' if result.timed_out else ''}")
        lines.append("")
    if failure:
        lines.append(f"FAIL: {failure}")
    receipt.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_smoke(root: Path, preset: str, timeout: int) -> tuple[int, list[CommandResult], str | None]:
    started_at = time.monotonic()
    results: list[CommandResult] = []

    def run_stage(command: Sequence[str], stage_timeout: int) -> CommandResult | None:
        remaining = timeout - int(time.monotonic() - started_at)
        if remaining <= 0:
            return None
        result = run_command(root, command, min(stage_timeout, remaining))
        results.append(result)
        return result

    try:
        configured = smoke_cache_is_usable(root, preset)
    except RuntimeError as error:
        return 1, results, str(error)
    if not configured:
        result = run_stage(
            ["cmake", "--preset", preset],
            CONFIGURE_TIMEOUT,
        )
        if result is None:
            return 1, results, "renderer smoke gate timed out before configuration"
        if result.returncode:
            return 1, results, "renderer smoke configuration failed"

    result = run_stage(
        ["cmake", "--build", "--preset", preset, "--target", "mc_renderer_smoke"],
        BUILD_TIMEOUT,
    )
    if result is None:
        return 1, results, "renderer smoke gate timed out before target build"
    if result.returncode:
        return 1, results, "renderer smoke target build failed"

    result = run_stage(
        [
            "ctest",
            "--preset",
            preset,
            "-R",
            "^RendererSmokeTest$",
            "--output-on-failure",
            "--no-tests=error",
        ],
        TEST_TIMEOUT,
    )
    if result is None:
        return 1, results, "renderer smoke gate timed out before CTest"
    if result.returncode:
        return 1, results, "RendererSmokeTest failed, timed out, was not registered, or reported validation errors"
    return 0, results, None


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preset", default=DEFAULT_PRESET)
    parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT)
    parser.add_argument("--root", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    root = (args.root or repository_root()).resolve()
    receipt = root / RECEIPT_PATH
    result, commands, failure = run_smoke(root, args.preset, args.timeout)
    write_receipt(receipt, commands, failure)
    if failure:
        print(f"FAIL renderer-smoke: {failure}; receipt {RECEIPT_PATH}")
    else:
        print(f"PASS renderer-smoke: receipt {RECEIPT_PATH}")
    return result


if __name__ == "__main__":
    raise SystemExit(main())
