#!/usr/bin/env python3
"""Offline validation entry point for the repository's AI-assisted branches."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


LOG_DIR = Path("build/ai-checks")
GOVERNED_PREFIXES = ("src/", "tests/", "docs/", "script/", ".githooks/", ".github/", ".codex/", ".agents/", "cmake/")
GOVERNED_FILES = {
    ".gitattributes", ".gitignore", "AGENTS.md", "CLAUDE.md", "CMakeLists.txt", "CMakePresets.json", "README.md",
    "publish.py", "vcpkg.json", "vcpkg-configuration.json",
}
DEFAULT_PUBLISHER_FAILURES = {
    "no unstaged or uncommitted changes",
    "version history folder exists",
    "version history markdown file exists",
    "required headings '# Overview' and '# Snapshots' present",
    "Overview contains text",
    "snapshots 1..SnapshotIndex exist with correct format",
    "snapshot sections are not empty",
    "snapshot dates are non-decreasing",
    "latest snapshot date is today",
}


@dataclass
class PhaseResult:
    name: str
    command: Sequence[str]
    returncode: int
    output: str
    timed_out: bool = False
    allowed_failure: bool = False


def command_output(root: Path, command: Sequence[str]) -> str:
    completed = subprocess.run(command, cwd=root, text=True, capture_output=True, check=False)
    if completed.returncode:
        raise RuntimeError(completed.stderr.strip() or " ".join(command))
    return completed.stdout


def repository_root() -> Path:
    return Path(__file__).resolve().parents[1]


def governed(path: str) -> bool:
    return path in GOVERNED_FILES or path.startswith(GOVERNED_PREFIXES)


def changed_paths(root: Path, cached: bool) -> set[str]:
    command = ["git", "diff"]
    if cached:
        command.append("--cached")
    command.extend(["--name-only", "-z"])
    completed = subprocess.run(command, cwd=root, capture_output=True, check=False)
    if completed.returncode:
        raise RuntimeError(completed.stderr.decode(errors="replace").strip() or " ".join(command))
    return {path.decode(errors="surrogateescape") for path in completed.stdout.split(b"\0") if path}


def untracked_paths(root: Path) -> set[str]:
    command = ["git", "ls-files", "--others", "--exclude-standard", "-z"]
    completed = subprocess.run(command, cwd=root, capture_output=True, check=False)
    if completed.returncode:
        raise RuntimeError(completed.stderr.decode(errors="replace").strip() or " ".join(command))
    return {path.decode(errors="surrogateescape") for path in completed.stdout.split(b"\0") if path}


def partially_staged_paths(root: Path) -> list[str]:
    staged = {path for path in changed_paths(root, cached=True) if governed(path)}
    unstaged = {path for path in changed_paths(root, cached=False) if governed(path)}
    untracked = {path for path in untracked_paths(root) if governed(path)}
    return sorted(unstaged | untracked) if staged else []


def project_version_arguments(root: Path) -> tuple[str, str]:
    content = (root / "src/shared/include/shared/ProjectInfo.hpp").read_text(encoding="utf-8")

    def value(name: str) -> str:
        match = re.search(rf"{name}\s*\{{\s*\"([^\"]+)\"\s*\}}", content)
        if not match:
            raise ValueError(f"{name} was not found in ProjectInfo.hpp")
        return match.group(1)

    fields = {}
    for field in ("epoch", "major", "minor", "patch"):
        match = re.search(rf"\.{field}\s*=\s*(\d+)", content)
        if not match:
            raise ValueError(f"PROJECT_VERSION.{field} was not found in ProjectInfo.hpp")
        fields[field] = match.group(1)
    return (
        f"{value('MAJOR_VERSION_NAME')}:{value('MINOR_VERSION_NAME')}",
        f"{fields['epoch']}.{fields['major']}.{fields['minor']}:{fields['patch']}",
    )


def publisher_failure_names(output: str) -> set[str]:
    clean = re.sub(r"\x1b\[[0-9;]*m", "", output)
    names = set()
    for line in clean.splitlines():
        match = re.match(r"^\[\d+/\d+\]\s+(.+?)\s+\.\.\.\s+", line)
        if match and "FAIL" in line:
            names.add(match.group(1))
    return names


def publisher_failure_is_allowed(output: str, strict: bool, candidate: bool = False) -> bool:
    if (strict and not candidate) or "Traceback" in output or "Exception:" in output:
        return False
    clean = re.sub(r"\x1b\[[0-9;]*m", "", output)
    failures = publisher_failure_names(clean)
    rows = [line for line in clean.splitlines() if re.match(r"^\[\d+/\d+\]", line)]
    allowed_lines = {"=== Common Checks ===", "ERROR - Some checks failed. Aborting."}
    for line in clean.splitlines():
        if not line or line in allowed_lines or line in rows:
            continue
        return False
    allowed = {"no unstaged or uncommitted changes"} if candidate else DEFAULT_PUBLISHER_FAILURES
    return bool(failures) and failures <= allowed


def run_phase(root: Path, log_dir: Path, name: str, command: Sequence[str], timeout: int) -> PhaseResult:
    try:
        completed = subprocess.run(command, cwd=root, text=True, capture_output=True, timeout=timeout, check=False)
        output = completed.stdout + completed.stderr
        result = PhaseResult(name, command, completed.returncode, output)
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout or ""
        stderr = error.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode(errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode(errors="replace")
        result = PhaseResult(name, command, 124, stdout + stderr, timed_out=True)
    except OSError as error:
        result = PhaseResult(name, command, 127, str(error))
    (log_dir / f"{name}.log").write_text(result.output, encoding="utf-8")
    return result


def print_result(result: PhaseResult) -> None:
    command = " ".join(result.command)
    if result.returncode == 0:
        print(f"PASS {result.name}: {command}")
        return
    state = "TIMEOUT" if result.timed_out else "FAIL"
    if result.allowed_failure:
        state = "ALLOWED"
    print(f"{state} {result.name}: exit {result.returncode}; log build/ai-checks/{result.name}.log")
    snippet = result.output.strip()[-4_000:]
    if snippet:
        print(snippet)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fast", action="store_true", help="skip build, CTest, and publisher checks")
    parser.add_argument("--strict", action="store_true", help="do not allow development publisher failures")
    parser.add_argument("--candidate", action="store_true", help="full release checks before commit; allow only dirty Git state")
    parser.add_argument("--level", choices=("basic", "snapshot", "minor", "major"), default="basic")
    parser.add_argument("--require-index-match", action="store_true", help="reject partially staged governed files")
    parser.add_argument("--root", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.fast and (args.candidate or args.strict or args.level != "basic"):
        parser.error("--fast cannot be combined with release checks")
    root = (args.root or repository_root()).resolve()
    log_dir = root / LOG_DIR
    log_dir.mkdir(parents=True, exist_ok=True)

    results: list[PhaseResult] = []
    if sys.version_info < (3, 12):
        results.append(PhaseResult("python-version", (), 1, "Python 3.12 or newer is required"))
    if args.require_index_match:
        try:
            partial = partially_staged_paths(root)
            output = "" if not partial else "Partially staged governed files: " + ", ".join(partial)
            results.append(PhaseResult("index-match", (), int(bool(partial)), output))
        except (OSError, RuntimeError) as error:
            results.append(PhaseResult("index-match", (), 1, str(error)))

    phase_specs: list[tuple[str, Sequence[str], int]] = [
        ("docs", [sys.executable, "script/ai_docs.py", "check"], 60),
        ("backlog", [sys.executable, "script/ai_tasks.py", "check"], 60),
        ("current-plan", [sys.executable, "script/ai_plan.py", "check"], 60),
        ("python-tests", [sys.executable, "-m", "unittest", "discover", "-s", "script/tests"], 60),
        ("diff-working", ["git", "diff", "--check"], 30),
        ("diff-cached", ["git", "diff", "--cached", "--check"], 30),
    ]
    if not args.fast:
        phase_specs.extend([
            ("build", ["cmake", "--build", "--preset", "debug"], 600),
            ("ctest", ["ctest", "--preset", "debug", "--output-on-failure", "--no-tests=error", "--timeout", "60"], 600),
        ])
    if not args.fast:
        try:
            config = json.loads((root / "script/ai_checks.json").read_text(encoding="utf-8"))
            seen = set()
            if not isinstance(config, list):
                raise ValueError("additional checks must be an array")
            for item in config:
                if set(item) != {"name", "enabled", "levels", "command", "timeout", "reason"}:
                    raise ValueError("invalid additional check fields")
                name = item["name"]
                if not isinstance(name, str) or not re.fullmatch(r"[a-z][a-z0-9-]*", name) or name in seen:
                    raise ValueError("invalid or duplicate additional check name")
                seen.add(name)
                if type(item["enabled"]) is not bool or not isinstance(item["levels"], list) or not item["levels"] or any(level not in {"basic", "snapshot", "minor", "major"} for level in item["levels"]):
                    raise ValueError("invalid additional check enabled/levels")
                if not isinstance(item["command"], list) or any(not isinstance(arg, str) or not arg for arg in item["command"]):
                    raise ValueError("invalid additional check command")
                if type(item["timeout"]) is not int or item["timeout"] <= 0 or not isinstance(item["reason"], str):
                    raise ValueError("invalid additional check timeout/reason")
                if item["enabled"] and not item["command"]:
                    raise ValueError("enabled check must have a command")
                if not item["enabled"] and not item["reason"].strip():
                    raise ValueError("disabled check must explain why")
                if args.level not in item["levels"]:
                    continue
                if item["enabled"]:
                    command = [sys.executable if arg == "{python}" else arg for arg in item["command"]]
                    phase_specs.append(("extra-" + name, command, item["timeout"]))
                else:
                    print(f"NOT ENABLED {name}: {item['reason']}")
        except (OSError, ValueError, TypeError, KeyError) as error:
            results.append(PhaseResult("check-registry", (), 1, str(error)))
    for name, command, timeout in phase_specs:
        results.append(run_phase(root, log_dir, name, command, timeout))
    if not args.fast:
        try:
            names, version = project_version_arguments(root)
            publisher = run_phase(root, log_dir, "publisher", [sys.executable, "publish.py", names, version, "--checks-only"], 60)
            publisher.allowed_failure = (
                publisher.returncode not in {0, 124}
                and publisher_failure_is_allowed(publisher.output, args.strict, args.candidate)
            )
            results.append(publisher)
        except (OSError, ValueError) as error:
            results.append(PhaseResult("publisher", (), 1, str(error)))

    for result in results:
        print_result(result)
    failures = [result for result in results if result.returncode and not result.allowed_failure]
    if any(result.allowed_failure for result in results):
        print("Publisher development exceptions were recorded; use --strict to reject them.")
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
