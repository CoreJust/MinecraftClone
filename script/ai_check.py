#!/usr/bin/env python3
"""Offline validation entry point for the repository's AI-assisted branches."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


LOG_DIR = Path("build/ai-checks")
RECEIPT_SCHEMA_VERSION = 1
SHARED_PYTHON_SOURCES = {
    "ai_check.py", "ai_commit.py", "ai_docs.py", "ai_history.py", "ai_plan.py",
    "ai_publish.py", "ai_run.py", "ai_setup.py", "ai_tasks.py",
}
PYTHON_TEST_TIMEOUT = 180 if os.name == "nt" else 120
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
    reused: bool = False
    skipped: bool = False
    duration_seconds: float = 0.0


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


def repository_paths(root: Path) -> list[str]:
    """Return tracked and untracked files used to fingerprint a check input."""
    tracked = subprocess.run(
        ["git", "ls-files", "-z"], cwd=root, capture_output=True, check=False
    )
    if tracked.returncode:
        raise RuntimeError(tracked.stderr.decode(errors="replace").strip() or "git ls-files")
    names = {path.decode(errors="surrogateescape") for path in tracked.stdout.split(b"\0") if path}
    names.update(
        path for path in untracked_paths(root)
        if not path.startswith("build/ai-checks/")
        and "/__pycache__/" not in f"/{path}"
        and not path.endswith(".pyc")
    )
    return sorted(names)


def phase_relevant_paths(root: Path, name: str) -> list[str]:
    paths = repository_paths(root)
    if name == "python-tests":
        return [path for path in paths if path.startswith("script/") and path.endswith(".py")]
    if name in {"backlog", "current-plan"}:
        return [
            path for path in paths
            if path.startswith("docs/ai/")
            or path in {"script/ai_tasks.py", "script/ai_plan.py", "script/ai_history.py"}
        ]
    # Documentation validation reads the complete module map and source state;
    # diff checks also observe the complete index/worktree.
    return paths


def _git_diff_bytes(root: Path, cached: bool, paths: Sequence[str]) -> bytes:
    command = ["git", "diff", "--binary", "--no-ext-diff"]
    if cached:
        command.append("--cached")
    command.append("--")
    command.extend(paths)
    completed = subprocess.run(command, cwd=root, capture_output=True, check=False)
    if completed.returncode:
        raise RuntimeError(completed.stderr.decode(errors="replace").strip() or "git diff")
    return completed.stdout


def relevant_inputs(root: Path, name: str) -> dict[str, object]:
    paths = phase_relevant_paths(root, name)
    files = []
    for relative in paths:
        target = root / relative
        if target.is_file():
            digest = hashlib.sha256(target.read_bytes()).hexdigest()
        else:
            digest = "<missing>"
        files.append([relative, digest])
    payload = {
        "paths": paths,
        "files": files,
        "index_diff": hashlib.sha256(_git_diff_bytes(root, True, paths)).hexdigest(),
        "working_diff": hashlib.sha256(_git_diff_bytes(root, False, paths)).hexdigest(),
    }
    serialized = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    payload["fingerprint"] = hashlib.sha256(serialized).hexdigest()
    return payload


def phase_environment(root: Path, name: str) -> dict[str, str]:
    return python_test_environment(root) if name == "python-tests" else os.environ.copy()


def environment_fingerprint(environment: dict[str, str]) -> str:
    # PWD, shell nesting and the command lookup placeholder vary between hook
    # invocations without changing a check's inputs. Git also injects these
    # hook-only index/worktree selectors; the index/worktree hashes below are
    # the authoritative state. Never persist values.
    ignored = {
        "PWD", "OLDPWD", "SHLVL", "_", "GIT_DIR", "GIT_COMMON_DIR",
        "GIT_INDEX_FILE", "GIT_PREFIX", "GIT_WORK_TREE",
        "GIT_AUTHOR_DATE", "GIT_AUTHOR_EMAIL", "GIT_AUTHOR_NAME", "GIT_EDITOR",
    }
    values = {key: value for key, value in environment.items() if key not in ignored}
    if "GIT_EXEC_PATH" not in values:
        resolved = subprocess.run(
            ["git", "--exec-path"], text=True, capture_output=True, check=False
        )
        if resolved.returncode == 0 and resolved.stdout.strip():
            values["GIT_EXEC_PATH"] = resolved.stdout.strip()
    serialized = json.dumps(values, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(serialized).hexdigest()


def tool_identity(command: Sequence[str]) -> dict[str, object]:
    executable = command[0] if command else ""
    resolved = shutil.which(executable) or executable
    target = Path(resolved)
    identity: dict[str, object] = {
        "requested": executable,
        "resolved": str(target.resolve()) if target.exists() else resolved,
        "python": sys.version,
        "platform": platform.platform(),
    }
    try:
        stat = target.stat()
        identity.update({"size": stat.st_size, "mtime_ns": stat.st_mtime_ns})
    except OSError:
        identity.update({"size": None, "mtime_ns": None})
    return identity


def receipt_path(log_dir: Path, name: str) -> Path:
    return log_dir / f"{name}.receipt.json"


def read_matching_receipt(
    root: Path,
    log_dir: Path,
    name: str,
    command: Sequence[str],
    environment: dict[str, str],
    inputs: dict[str, object],
) -> str | None:
    target = receipt_path(log_dir, name)
    log = log_dir / f"{name}.log"
    try:
        receipt = json.loads(target.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    expected = {
        "schema_version": RECEIPT_SCHEMA_VERSION,
        "phase": name,
        "status": "PASS",
        "command": list(command),
        "environment_fingerprint": environment_fingerprint(environment),
        "tool_identity": tool_identity(command),
        "relevant_inputs": inputs,
        "returncode": 0,
        "timed_out": False,
    }
    if receipt != expected or not log.is_file():
        return None
    try:
        return log.read_text(encoding="utf-8")
    except OSError:
        return None


def write_receipt(
    log_dir: Path,
    name: str,
    command: Sequence[str],
    environment: dict[str, str],
    inputs: dict[str, object],
    result: PhaseResult,
) -> None:
    if result.returncode != 0 or result.timed_out:
        return
    receipt = {
        "schema_version": RECEIPT_SCHEMA_VERSION,
        "phase": name,
        "status": "PASS",
        "command": list(command),
        "environment_fingerprint": environment_fingerprint(environment),
        "tool_identity": tool_identity(command),
        "relevant_inputs": inputs,
        "returncode": result.returncode,
        "timed_out": result.timed_out,
    }
    receipt_path(log_dir, name).write_text(
        json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


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


def python_test_environment(root: Path) -> dict[str, str]:
    names = command_output(root, ["git", "rev-parse", "--local-env-vars"]).splitlines()
    if not names:
        raise RuntimeError("Git returned no repository-local environment variables")
    if any(not re.fullmatch(r"GIT_[A-Z0-9_]+", name) for name in names):
        raise RuntimeError("Git returned an invalid repository-local environment variable")
    environment = os.environ.copy()
    for name in names:
        environment.pop(name, None)
    return environment


def run_phase(
    root: Path,
    log_dir: Path,
    name: str,
    command: Sequence[str],
    timeout: int,
    *,
    reuse: bool = False,
) -> PhaseResult:
    started = time.monotonic()
    try:
        environment = phase_environment(root, name)
        inputs = relevant_inputs(root, name)
    except (OSError, RuntimeError) as error:
        result = PhaseResult(name, command, 127, str(error))
        result.duration_seconds = time.monotonic() - started
        (log_dir / f"{name}.log").write_text(result.output, encoding="utf-8")
        return result
    # The full Python suite reads repository content outside script/**/*.py,
    # so its receipt cannot be invalidated safely by the script-only input
    # fingerprint. Always execute it during fast checks; other eligible
    # phases retain receipt reuse.
    if reuse and name not in {"build", "ctest", "python-tests"}:
        output = read_matching_receipt(root, log_dir, name, command, environment, inputs)
        if output is not None:
            return PhaseResult(
                name, command, 0, output, reused=True, duration_seconds=time.monotonic() - started
            )
    try:
        completed = subprocess.run(
            command,
            cwd=root,
            text=True,
            capture_output=True,
            timeout=timeout,
            check=False,
            env=environment,
        )
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
    except (OSError, RuntimeError) as error:
        result = PhaseResult(name, command, 127, str(error))
    (log_dir / f"{name}.log").write_text(result.output, encoding="utf-8")
    result.duration_seconds = time.monotonic() - started
    write_receipt(log_dir, name, command, environment, inputs, result)
    return result


def print_result(result: PhaseResult) -> None:
    command = " ".join(result.command)
    if result.skipped:
        print(f"SKIP {result.name}: {result.output}")
        return
    if result.reused:
        print(f"REUSED PASS {result.name}: {command}")
        return
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


def changed_scope(root: Path) -> tuple[str, list[str]]:
    paths = changed_paths(root, cached=True) | changed_paths(root, cached=False) | untracked_paths(root)
    paths = {
        path for path in paths
        if not path.startswith("build/ai-checks/")
        and "/__pycache__/" not in f"/{path}"
        and not path.endswith(".pyc")
    }
    if not paths:
        return "full-python", []
    if all(path.startswith("docs/") or path in {"README.md", "AGENTS.md"} for path in paths):
        return "metadata-only", sorted(paths)
    python_sources = {
        path for path in paths
        if path.startswith("script/") and path.endswith(".py") and not path.startswith("script/tests/")
    }
    if python_sources and python_sources == paths:
        targets = []
        for source in sorted(python_sources):
            if Path(source).name in SHARED_PYTHON_SOURCES or "/ci/" in source:
                return "full-python", sorted(paths)
            candidate = f"script/tests/test_{Path(source).stem}.py"
            # The exact one-file mapping is intentionally conservative. A
            # missing test or any mixed change falls back to the full suite.
            if not (root / candidate).is_file():
                return "full-python", sorted(paths)
            targets.append(candidate)
        return "targeted-python:" + ":".join(targets), sorted(paths)
    if any(
        path.startswith(("src/", "tests/", "cmake/"))
        or path in {"CMakeLists.txt", "CMakePresets.json", "vcpkg.json", "vcpkg-configuration.json"}
        for path in paths
    ):
        return "full-cpp", sorted(paths)
    # Unknown governed inputs are not safely attributable to one test file;
    # use the broad code/build fallback when the checkout supports it.
    return "full-cpp", sorted(paths)


def write_summary(root: Path, scope: str, changed: Sequence[str], results: Sequence[PhaseResult]) -> None:
    try:
        head = command_output(root, ["git", "rev-parse", "HEAD"]).strip()
        index_tree = command_output(root, ["git", "write-tree"]).strip()
    except (OSError, RuntimeError):
        head = ""
        index_tree = ""
    payload = {
        "schema_version": RECEIPT_SCHEMA_VERSION,
        "invocation_id": f"{time.time_ns()}-{os.getpid()}",
        "head": head,
        "index_tree": index_tree,
        "task_id": os.environ.get("AI_TASK", ""),
        "scope": scope,
        "changed_paths": list(changed),
        "reused_phases": [result.name for result in results if result.reused],
        "executed_phases": [result.name for result in results if not result.reused and not result.skipped],
        "skipped_phases": [result.name for result in results if result.skipped],
        "durations_seconds": {
            result.name: round(result.duration_seconds, 6) for result in results
        },
    }
    (root / LOG_DIR / "summary.json").write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    with (root / LOG_DIR / "summary.jsonl").open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(payload, sort_keys=True) + "\n")


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
    try:
        scope, changed = changed_scope(root)
    except (OSError, RuntimeError) as error:
        scope, changed = "full-python", []
        print(f"CHECK SCOPE unknown: {error}; using conservative full Python fallback")
    if changed:
        print(f"CHECK SCOPE {scope}: {', '.join(changed)}")

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
        ("diff-working", ["git", "diff", "--check"], 30),
        ("diff-cached", ["git", "diff", "--cached", "--check"], 30),
    ]
    if scope == "metadata-only":
        results.append(PhaseResult("python-tests", (), 0, "metadata-only changes", skipped=True))
    elif scope.startswith("targeted-python:"):
        targets = scope.split(":")[1:]
        phase_specs.append(
            (
                "python-tests",
                [sys.executable, "-m", "unittest", *targets, "-v"],
                PYTHON_TEST_TIMEOUT,
            )
        )
    else:
        phase_specs.append(
            (
                "python-tests",
                [sys.executable, "-m", "unittest", "discover", "-s", "script/tests", "-v"],
                PYTHON_TEST_TIMEOUT,
            )
        )
    build_configured = (root / "CMakePresets.json").is_file()
    needs_build = not args.fast or (scope == "full-cpp" and build_configured)
    if args.fast and needs_build:
        print("CHECK SCOPE full-cpp: running the existing build and CTest fallback")
    if args.fast and scope == "full-cpp" and not build_configured:
        results.append(
            PhaseResult(
                "code-fallback",
                ("cmake", "--build", "--preset", "debug"),
                1,
                "C++ or unknown changes require CMakePresets.json for the full build/CTest fallback",
            )
        )
    if needs_build:
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
        # Only fast development checks may reuse a receipt. Candidate,
        # strict, and pre-push release checks always execute every phase.
        if args.fast:
            results.append(run_phase(root, log_dir, name, command, timeout, reuse=True))
        else:
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
    write_summary(root, scope, changed, results)
    failures = [result for result in results if result.returncode and not result.allowed_failure]
    if any(result.allowed_failure for result in results):
        print("Publisher development exceptions were recorded; use --strict to reject them.")
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
