#!/usr/bin/env python3
"""Gate AI-branch commits on task metadata and local review attestations.

Review reports are supplied by a reviewer or agent; this tool never invokes a
model.  Receipts live under Git's private directory so they cannot become part
of the source change they approve.
"""

from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any, Sequence


LEVELS = frozenset({"basic", "snapshot", "minor", "major"})
MODELS = frozenset({"gpt-5.6-luna", "gpt-5.6-terra"})
LUNA_MODEL = "gpt-5.6-luna"
TERRA_MODEL = "gpt-5.6-terra"


class CommitGateError(ValueError):
    pass


def repository_root() -> Path:
    return Path(__file__).resolve().parents[1]


def git(root: Path, *args: str, input_text: str | None = None) -> str:
    completed = subprocess.run(
        ["git", *args],
        cwd=root,
        text=True,
        input=input_text,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise CommitGateError(completed.stderr.strip() or " ".join(("git", *args)))
    return completed.stdout


def ai_tasks_module(root: Path) -> Any:
    target = root / "script" / "ai_tasks.py"
    spec = importlib.util.spec_from_file_location("ai_commit_tasks", target)
    if spec is None or spec.loader is None:
        raise CommitGateError(f"cannot load {target}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def ai_history_module(root: Path) -> Any:
    scripts = str(root / "script")
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    target = root / "script" / "ai_history.py"
    spec = importlib.util.spec_from_file_location("ai_commit_history", target)
    if spec is None or spec.loader is None:
        raise CommitGateError(f"cannot load {target}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def staged_backlog(root: Path) -> list[dict[str, Any]]:
    try:
        raw = git(root, "show", ":docs/ai/backlog.json")
        tasks = json.loads(raw)
    except json.JSONDecodeError as error:
        raise CommitGateError(f"staged docs/ai/backlog.json is invalid JSON: {error}") from error
    except CommitGateError as error:
        raise CommitGateError("docs/ai/backlog.json must be staged before an AI commit") from error
    try:
        ai_tasks_module(root).validate_backlog(tasks)
    except (OSError, ValueError) as error:
        raise CommitGateError(f"staged backlog is invalid: {error}") from error
    return tasks


def prospective_merge_head(root: Path, head: str) -> str:
    location = Path(git(root, "rev-parse", "--git-path", "MERGE_HEAD").strip())
    merge_file = location if location.is_absolute() else root / location
    if not merge_file.is_file():
        return ""
    try:
        merge_heads = [value for value in merge_file.read_text(encoding="utf-8").splitlines() if value]
    except OSError as error:
        raise CommitGateError(f"cannot read MERGE_HEAD: {error}") from error
    if len(merge_heads) != 1:
        raise CommitGateError("only clean two-parent promotions are supported")
    merge_head = git(root, "rev-parse", "--verify", f"{merge_heads[0]}^{{commit}}").strip()
    common_ancestor = subprocess.run(
        ["git", "merge-base", head, merge_head],
        cwd=root,
        capture_output=True,
        check=False,
    )
    if common_ancestor.returncode or not common_ancestor.stdout.strip():
        raise CommitGateError("unsupported unrelated promotion: HEAD and MERGE_HEAD have no common ancestor")
    return merge_head


def task_candidate(root: Path, task_id: str) -> dict[str, str]:
    tasks = staged_backlog(root)
    task = next((item for item in tasks if item["id"] == task_id), None)
    if task is None:
        raise CommitGateError(f"task {task_id} is absent from staged docs/ai/backlog.json")
    level = task.get("level")
    if level not in LEVELS:
        raise CommitGateError(f"{task_id}.level must be one of: {', '.join(sorted(LEVELS))}")
    if level != "basic" and task.get("finalized") is not True:
        raise CommitGateError(f"{task_id} is {level}; staged metadata must set finalized to true")
    try:
        head = git(root, "rev-parse", "HEAD").strip()
        configured_baseline = task.get("baseline_commit")
        if level != "basic" and (not isinstance(configured_baseline, str) or not configured_baseline):
            raise CommitGateError(f"{task_id} is {level}; baseline_commit must be a non-empty commit ID")
        baseline = configured_baseline or head
        if not isinstance(baseline, str):
            raise CommitGateError(f"{task_id}.baseline_commit must be a commit ID or blank")
        baseline = git(root, "rev-parse", "--verify", f"{baseline}^{{commit}}").strip()
        tree = git(root, "write-tree").strip()
    except CommitGateError as error:
        raise CommitGateError(f"cannot create candidate for {task_id}: {error}") from error
    merge_head = prospective_merge_head(root, head)
    candidate = {
        "task_id": task_id,
        "head": head,
        "tree": tree,
        "level": level,
        "baseline_commit": baseline,
        "merge_head": merge_head,
    }
    if level != "basic":
        try:
            ai_history_module(root).finalize(
                root,
                copy.deepcopy(tasks),
                task_id,
                head,
                additional_head=merge_head or None,
            )
        except (OSError, RuntimeError, ValueError) as error:
            raise CommitGateError(f"{task_id} staged finalization is not valid at HEAD: {error}") from error
    return candidate


def reviews_directory(root: Path) -> Path:
    location = Path(git(root, "rev-parse", "--git-path", "ai-reviews").strip())
    return location if location.is_absolute() else root / location


def receipt_path(root: Path, candidate: dict[str, str], model: str) -> Path:
    if model not in MODELS:
        raise CommitGateError(f"unsupported review model: {model}")
    key = "-".join((candidate["head"], candidate["tree"], candidate["baseline_commit"], candidate["merge_head"], model))
    return reviews_directory(root) / candidate["task_id"] / f"{key}.json"


def validate_report(report: Any, candidate: dict[str, str]) -> dict[str, Any]:
    if not isinstance(report, dict):
        raise CommitGateError("review report must be a JSON object")
    for field, value in candidate.items():
        if report.get(field) != value:
            raise CommitGateError(f"review report {field} does not match the staged candidate")
    model = report.get("model")
    if model not in MODELS:
        raise CommitGateError("review report model must be gpt-5.6-luna or gpt-5.6-terra")
    if report.get("effort") != "high":
        raise CommitGateError("review report effort must be high")
    if report.get("verdict") != "approved":
        raise CommitGateError("review report verdict must be approved")
    if not isinstance(report.get("evidence"), str) or not report["evidence"].strip():
        raise CommitGateError("review report evidence must be non-empty")
    return report


def selected_task_id(root: Path) -> str:
    selected = os.environ.get("AI_TASK")
    if selected:
        return selected
    current = reviews_directory(root) / "current.json"
    try:
        payload = json.loads(current.read_text(encoding="utf-8"))
    except OSError as error:
        raise CommitGateError("AI_TASK is not set and no Luna review has selected a task") from error
    except json.JSONDecodeError as error:
        raise CommitGateError(f"invalid local review selector {current}: {error}") from error
    task_id = payload.get("task_id") if isinstance(payload, dict) else None
    if not isinstance(task_id, str) or not task_id:
        raise CommitGateError(f"invalid local review selector {current}")
    return task_id


def record_review(root: Path, report_path: Path) -> dict[str, Any]:
    try:
        report = json.loads(report_path.read_text(encoding="utf-8"))
    except OSError as error:
        raise CommitGateError(f"cannot read review report {report_path}: {error}") from error
    except json.JSONDecodeError as error:
        raise CommitGateError(f"invalid JSON in review report {report_path}: {error}") from error
    task_id = report.get("task_id") if isinstance(report, dict) else None
    if not isinstance(task_id, str):
        raise CommitGateError("review report task_id must be a string")
    candidate = task_candidate(root, task_id)
    validated = validate_report(report, candidate)
    destination = receipt_path(root, candidate, validated["model"])
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(validated, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if validated["model"] == LUNA_MODEL:
        selector = {"task_id": task_id}
        (reviews_directory(root) / "current.json").write_text(
            json.dumps(selector, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    return candidate


def read_receipt(root: Path, candidate: dict[str, str], model: str) -> None:
    target = receipt_path(root, candidate, model)
    try:
        report = json.loads(target.read_text(encoding="utf-8"))
    except OSError as error:
        raise CommitGateError(f"missing {model} review receipt for current staged candidate") from error
    except json.JSONDecodeError as error:
        raise CommitGateError(f"invalid local receipt {target}: {error}") from error
    validated = validate_report(report, candidate)
    if validated["model"] != model:
        raise CommitGateError(f"local receipt {target} has the wrong model")


def check_message(root: Path, message_path: Path, task_id: str) -> None:
    try:
        message = message_path.read_text(encoding="utf-8")
    except OSError as error:
        raise CommitGateError(f"cannot read commit message {message_path}: {error}") from error
    completed = subprocess.run(
        ["git", "interpret-trailers", "--parse"],
        cwd=root,
        text=True,
        input=message,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise CommitGateError(completed.stderr.strip() or "cannot parse commit trailers")
    task_values = []
    for line in completed.stdout.splitlines():
        key, separator, value = line.partition(":")
        if separator and key.strip().lower() == "task-id":
            task_values.append(value.strip())
    if task_values != [task_id]:
        raise CommitGateError(f"commit message must contain exactly one Task-ID: {task_id} trailer")


def check(root: Path, message_path: Path | None = None) -> dict[str, str]:
    task_id = selected_task_id(root)
    candidate = task_candidate(root, task_id)
    read_receipt(root, candidate, LUNA_MODEL)
    if candidate["level"] in {"minor", "major"}:
        read_receipt(root, candidate, TERRA_MODEL)
    if message_path is not None:
        check_message(root, message_path, task_id)
    return candidate


def precommit(root: Path) -> int:
    candidate = check(root)
    check_args = [sys.executable, str(root / "script" / "ai_check.py")]
    if candidate["level"] == "basic":
        check_args.append("--fast")
    else:
        check_args.extend(("--candidate", "--level", candidate["level"]))
    check_args.append("--require-index-match")
    completed = subprocess.run(check_args, cwd=root, check=False)
    return completed.returncode


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, help=argparse.SUPPRESS)
    subcommands = parser.add_subparsers(dest="command", required=True)
    candidate_parser = subcommands.add_parser("candidate", help="print the staged task candidate")
    candidate_parser.add_argument("task_id")
    record_parser = subcommands.add_parser("record-review", help="store a supplied local review receipt")
    record_parser.add_argument("report", type=Path)
    check_parser = subcommands.add_parser("check", help="validate current receipts and an optional commit message")
    check_parser.add_argument("--message", type=Path)
    subcommands.add_parser("precommit", help="run receipt and level-appropriate local checks")
    args = parser.parse_args(argv)
    root = (args.root or repository_root()).resolve()
    try:
        if args.command == "candidate":
            print(json.dumps(task_candidate(root, args.task_id), sort_keys=True))
        elif args.command == "record-review":
            print(json.dumps(record_review(root, args.report), sort_keys=True))
        elif args.command == "check":
            print(json.dumps(check(root, args.message), sort_keys=True))
        else:
            return precommit(root)
    except CommitGateError as error:
        print(f"AI commit gate: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
