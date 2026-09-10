#!/usr/bin/env python3
"""Manage the canonical AI delivery backlog and its generated Markdown views."""

from __future__ import annotations

import argparse
from datetime import date
import html
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from typing import Any, Iterable, Sequence


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BACKLOG_PATH = ROOT / "docs" / "ai" / "backlog.json"
DEFAULT_MARKDOWN_PATH = ROOT / "docs" / "ai" / "BACKLOG.md"
TASK_FIELDS = (
    "id", "title", "kind", "status", "priority", "route", "milestone",
    "depends_on", "acceptance", "evidence", "blocker", "owner", "level",
    "parent", "motivation", "context", "complexity", "created_at",
    "resolved_at", "resolution_changes", "plan", "product_changes",
    "code_changes", "baseline_commit", "commits", "finalized", "retrospective",
    "docs_review", "environment_review", "backlog_review",
)
KINDS = frozenset({"feature", "bug", "chore", "decision"})
STATUSES = frozenset({"backlog", "ready", "active", "blocked", "done"})
PRIORITIES = frozenset({"P0", "P1", "P2", "P3"})
ROUTES = frozenset({"astra", "sol", "terra", "luna"})
LEVELS = frozenset({"basic", "snapshot", "minor", "major"})
COMPLEXITIES = frozenset({"low", "medium", "high"})
PARENT_LEVEL = {"basic": "snapshot", "snapshot": "minor", "minor": "major"}
PRIORITY_ORDER = {priority: index for index, priority in enumerate(("P0", "P1", "P2", "P3"))}
ID_PREFIX = "MC-AI-"
SHA_PATTERN = re.compile(r"^[0-9a-fA-F]{40}(?:[0-9a-fA-F]{24})?$")


class BacklogError(ValueError):
    pass


PACKET_TASK_FIELDS = (
    "id",
    "title",
    "status",
    "priority",
    "route",
    "level",
    "parent",
    "milestone",
    "depends_on",
    "acceptance",
    "plan",
    "blocker",
    "owner",
)
CI_SCHEMA_VERSION = 1
CI_TERMINAL_STATUS = "completed"
CI_SUCCESS_CONCLUSION = "success"
EFFICIENCY_SCHEMA_VERSION = 1
EFFICIENCY_BASELINE_TASK = "MC-AI-0089"
EFFICIENCY_TARGET_SIZE = 5


def load_backlog(backlog_path: Path) -> list[dict[str, Any]]:
    try:
        raw = json.loads(backlog_path.read_text(encoding="utf-8"))
    except OSError as error:
        raise BacklogError(f"cannot read {backlog_path}: {error}") from error
    except json.JSONDecodeError as error:
        raise BacklogError(f"invalid JSON in {backlog_path}: {error}") from error
    if not isinstance(raw, list):
        raise BacklogError("backlog root must be a JSON array")
    return raw


def validate_date(task_id: str, field: str, value: str) -> date:
    try:
        return date.fromisoformat(value)
    except ValueError as error:
        raise BacklogError(f"{task_id}.{field} must be an ISO date") from error


def validate_backlog(tasks: Sequence[dict[str, Any]]) -> None:
    ids: set[str] = set()
    list_fields = {"depends_on", "plan", "product_changes", "code_changes", "commits"}
    for index, task in enumerate(tasks):
        location = f"task {index}"
        if not isinstance(task, dict):
            raise BacklogError(f"{location} must be an object")
        fields = set(task)
        missing = sorted(set(TASK_FIELDS) - fields)
        unknown = sorted(fields - set(TASK_FIELDS))
        if missing:
            raise BacklogError(f"{location} missing fields: {', '.join(missing)}")
        if unknown:
            raise BacklogError(f"{location} has unknown fields: {', '.join(unknown)}")
        for field in TASK_FIELDS:
            value = task[field]
            if field in list_fields:
                if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
                    raise BacklogError(f"{location}.{field} must be a list of strings")
            elif field == "finalized":
                if not isinstance(value, bool):
                    raise BacklogError(f"{location}.finalized must be a boolean")
            elif not isinstance(value, str):
                raise BacklogError(f"{location}.{field} must be a string")
        task_id = task["id"]
        if not task_id.startswith(ID_PREFIX) or not task_id[len(ID_PREFIX):].isdigit() or len(task_id) != 10:
            raise BacklogError(f"{location}.id must use {ID_PREFIX}#### format")
        if task_id in ids:
            raise BacklogError(f"duplicate task ID: {task_id}")
        ids.add(task_id)
        for field, allowed in (("kind", KINDS), ("status", STATUSES), ("priority", PRIORITIES), ("route", ROUTES), ("level", LEVELS), ("complexity", COMPLEXITIES)):
            if task[field] not in allowed:
                raise BacklogError(f"{task_id}.{field} must be one of: {', '.join(sorted(allowed))}")
        for field in ("title", "milestone", "acceptance", "motivation", "context"):
            if not task[field].strip():
                raise BacklogError(f"{task_id}.{field} must be non-empty")
        if len(set(task["depends_on"])) != len(task["depends_on"]):
            raise BacklogError(f"{task_id}.depends_on contains duplicates")
        if task_id in task["depends_on"]:
            raise BacklogError(f"{task_id} cannot depend on itself")
        created_at = validate_date(task_id, "created_at", task["created_at"])
        if task["resolved_at"]:
            if validate_date(task_id, "resolved_at", task["resolved_at"]) < created_at:
                raise BacklogError(f"{task_id}.resolved_at cannot precede created_at")
        if task["level"] == "snapshot" and task["status"] != "done" and task["resolved_at"]:
            raise BacklogError(f"{task_id} is not a done snapshot but has resolved_at")
        if task["baseline_commit"] and not SHA_PATTERN.fullmatch(task["baseline_commit"]):
            raise BacklogError(f"{task_id}.baseline_commit must be blank or a full 40/64-character SHA")
        if not all(SHA_PATTERN.fullmatch(commit) for commit in task["commits"]):
            raise BacklogError(f"{task_id}.commits must contain full 40/64-character SHAs")
        if task["status"] == "done":
            if not task["evidence"].strip():
                raise BacklogError(f"{task_id} is done but has no evidence")
            if not task["resolved_at"]:
                raise BacklogError(f"{task_id} is done but has no resolved_at date")
            if not task["resolution_changes"].strip():
                raise BacklogError(f"{task_id} is done but has no resolution_changes")
        if task["status"] == "blocked" and not task["blocker"].strip():
            raise BacklogError(f"{task_id} is blocked but has no blocker")
        if task["status"] == "active" and not task["owner"].strip():
            raise BacklogError(f"{task_id} is active but has no owner")
        if task["finalized"] and task["level"] in {"minor", "major"}:
            for field in ("retrospective", "docs_review", "environment_review", "backlog_review"):
                if not task[field].strip():
                    raise BacklogError(f"{task_id} is finalized but has no {field}")

    task_by_id = {task["id"]: task for task in tasks}
    for task in tasks:
        parent = task["parent"]
        if parent:
            if parent not in task_by_id:
                raise BacklogError(f"{task['id']}.parent references missing task {parent}")
            if parent == task["id"]:
                raise BacklogError(f"{task['id']} cannot be its own parent")
            expected = PARENT_LEVEL.get(task["level"])
            if expected is None:
                raise BacklogError(f"{task['id']}.parent must be blank for level {task['level']}")
            if task_by_id[parent]["level"] != expected:
                raise BacklogError(f"{task['id']}.parent must have level {expected or 'none'}")
        elif task["level"] not in {"basic", "major"}:
            raise BacklogError(f"{task['id']}.parent is required for level {task['level']}")
        for dependency in task["depends_on"]:
            if dependency not in task_by_id:
                raise BacklogError(f"{task['id']} depends on missing task {dependency}")
            if task["status"] in {"ready", "active", "done"} and task_by_id[dependency]["status"] != "done":
                raise BacklogError(f"{task['id']} has unfinished dependency {dependency}")

    visiting: set[str] = set()
    visited: set[str] = set()
    def visit(task_id: str) -> None:
        if task_id in visiting:
            raise BacklogError(f"hierarchy cycle includes {task_id}")
        if task_id in visited:
            return
        visiting.add(task_id)
        parent = task_by_id[task_id]["parent"]
        if parent:
            visit(parent)
        visiting.remove(task_id)
        visited.add(task_id)
    for task_id in sorted(task_by_id):
        visit(task_id)

    children: dict[str, list[dict[str, Any]]] = {}
    for task in tasks:
        if task["parent"]:
            children.setdefault(task["parent"], []).append(task)
    for parent in tasks:
        if parent["level"] == "basic" or parent["status"] != "done":
            continue
        parent_id = parent["id"]
        if not parent["finalized"]:
            raise BacklogError(f"{parent_id} is done but is not finalized")
        child_tasks = children.get(parent_id, [])
        if not child_tasks:
            raise BacklogError(f"{parent_id} is done but has no children")
        if any(child["status"] != "done" for child in child_tasks):
            raise BacklogError(f"{parent_id} is done but has unfinished children")
        for field in ("product_changes", "code_changes"):
            if not parent[field]:
                raise BacklogError(f"{parent_id} is done but has no {field}")


def _git_output(root: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", *arguments],
        cwd=root,
        text=True,
        encoding="utf-8",
        errors="strict",
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise BacklogError(completed.stderr.strip() or "git " + " ".join(arguments) + " failed")
    return completed.stdout.strip()


def candidate_identity(root: Path, task: dict[str, Any]) -> dict[str, str]:
    """Return the immutable local identity used by a compact handoff packet."""

    head = _git_output(root, "rev-parse", "--verify", "HEAD^{commit}")
    index_tree = _git_output(root, "write-tree")
    baseline = task["baseline_commit"]
    if baseline and not SHA_PATTERN.fullmatch(baseline):
        raise BacklogError(f"{task['id']}.baseline_commit must be a full commit SHA")
    return {
        "task_id": task["id"],
        "head_sha": head,
        "index_tree": index_tree,
        "baseline_commit": baseline,
    }


def changed_paths(root: Path) -> list[str]:
    """List changed paths without reading file contents or commit history."""

    completed = subprocess.run(
        ["git", "status", "--porcelain=v1", "-z", "--untracked-files=all"],
        cwd=root,
        text=False,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        error = completed.stderr.decode("utf-8", errors="replace").strip()
        raise BacklogError(error or "git status failed")
    fields = completed.stdout.split(b"\0")
    paths: set[str] = set()
    index = 0
    while index < len(fields):
        field = fields[index]
        index += 1
        if not field:
            continue
        if len(field) < 4:
            raise BacklogError("git status returned a malformed porcelain record")
        status = field[:2].decode("ascii", errors="replace")
        current = field[3:].decode("utf-8", errors="strict")
        paths.add(current)
        if status[0] in "RC" or status[1] in "RC":
            if index >= len(fields) or not fields[index]:
                raise BacklogError("git status returned an incomplete rename record")
            paths.add(fields[index].decode("utf-8", errors="strict"))
            index += 1
    return sorted(paths)


def compact_task_packet(
    tasks: Sequence[dict[str, Any]],
    task_id: str,
    root: Path = ROOT,
    next_commands: Sequence[str] = (),
) -> dict[str, Any]:
    """Build a short, reproducible handoff packet from current local state."""

    validate_backlog(tasks)
    task = next((item for item in tasks if item["id"] == task_id), None)
    if task is None:
        raise BacklogError(f"unknown task ID: {task_id}")
    if not all(isinstance(command, str) and command.strip() for command in next_commands):
        raise BacklogError("next_commands must contain non-empty strings")
    return {
        "schema_version": 1,
        "task": {field: task[field] for field in PACKET_TASK_FIELDS},
        "candidate": candidate_identity(root, task),
        "changed_paths": changed_paths(root),
        "next_commands": list(next_commands),
    }


def _ci_artifacts(run: dict[str, Any]) -> list[dict[str, Any]]:
    artifacts = run.get("artifacts")
    if not isinstance(artifacts, list):
        raise BacklogError("CI run has no artifact list")
    result = []
    seen: set[str] = set()
    for artifact in artifacts:
        if not isinstance(artifact, dict):
            raise BacklogError("CI artifact must be an object")
        artifact_id = artifact.get("id")
        if not isinstance(artifact_id, (str, int)) or not str(artifact_id):
            raise BacklogError("CI artifact has no ID")
        key = str(artifact_id)
        if key in seen:
            raise BacklogError(f"duplicate CI artifact ID: {key}")
        seen.add(key)
        result.append({
            "id": artifact_id,
            "name": artifact.get("name", ""),
        })
    return result


def build_ci_record(
    runs: Sequence[dict[str, Any]],
    expected_head: str,
    workflow: str = "",
) -> dict[str, Any]:
    """Validate exact-SHA terminal CI runs and return a versioned local record."""

    if not SHA_PATTERN.fullmatch(expected_head):
        raise BacklogError("CI head must be a full commit SHA")
    if not runs:
        raise BacklogError(f"no CI runs found for {expected_head}")
    records = []
    for run in runs:
        if not isinstance(run, dict):
            raise BacklogError("CI run must be an object")
        head = run.get("headSha", run.get("head_sha"))
        if head != expected_head:
            raise BacklogError("CI run head does not match the requested commit SHA")
        status = run.get("status")
        conclusion = run.get("conclusion")
        if status != CI_TERMINAL_STATUS or conclusion != CI_SUCCESS_CONCLUSION:
            raise BacklogError("CI run is not a completed success")
        run_id = run.get("databaseId", run.get("run_id"))
        if not isinstance(run_id, (str, int)) or not str(run_id):
            raise BacklogError("CI run has no immutable run ID")
        records.append({
            "run_id": run_id,
            "head_sha": head,
            "status": status,
            "conclusion": conclusion,
            "workflow": run.get("workflowName", workflow),
            "url": run.get("url", ""),
            "artifacts": _ci_artifacts(run),
        })
    return {
        "schema_version": CI_SCHEMA_VERSION,
        "head_sha": expected_head,
        "workflow": workflow,
        "runs": records,
    }


def gh_json(root: Path, arguments: Sequence[str]) -> Any:
    completed = subprocess.run(
        ["gh", *arguments],
        cwd=root,
        text=True,
        encoding="utf-8",
        errors="strict",
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise BacklogError(completed.stderr.strip() or "gh command failed")
    try:
        return json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise BacklogError(f"gh returned invalid JSON: {error}") from error


def collect_ci_record(root: Path, expected_head: str, workflow: str = "") -> dict[str, Any]:
    arguments = [
        "run",
        "list",
        "--commit",
        expected_head,
        "--limit",
        "100",
        "--json",
        "databaseId,status,conclusion,headSha,workflowName,url",
    ]
    if workflow:
        arguments[4:4] = ["--workflow", workflow]
    summaries = gh_json(root, arguments)
    if not isinstance(summaries, list):
        raise BacklogError("gh run list must return an array")
    runs = []
    for summary in summaries:
        if not isinstance(summary, dict):
            raise BacklogError("gh run list returned a malformed run")
        run_id = summary.get("databaseId")
        if not isinstance(run_id, (str, int)) or not str(run_id):
            raise BacklogError("gh run list returned a run without an ID")
        if summary.get("headSha") != expected_head:
            raise BacklogError("gh run list returned a run for a different commit SHA")
        details = gh_json(root, [
            "run",
            "view",
            str(run_id),
            "--json",
            "databaseId,status,conclusion,headSha,workflowName,url,artifacts",
        ])
        if not isinstance(details, dict):
            raise BacklogError("gh run view must return an object")
        if details.get("databaseId", run_id) != run_id:
            raise BacklogError("gh run view returned a different run ID")
        runs.append(details)
    return build_ci_record(runs, expected_head, workflow)


def write_immutable_json(target_path: Path, payload: dict[str, Any]) -> None:
    target_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with target_path.open("x", encoding="utf-8", newline="\n") as output:
            json.dump(payload, output, indent=2, ensure_ascii=False, sort_keys=True)
            output.write("\n")
    except FileExistsError as error:
        raise BacklogError(f"refusing to overwrite immutable record {target_path}") from error


def _receipt_summary(receipt_path: Path) -> dict[str, Any] | None:
    try:
        payload = json.loads(receipt_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(payload, dict):
        return None
    return _receipt_summary_payload(payload, receipt_path.stem)


def _receipt_summary_payload(payload: dict[str, Any], source: str) -> dict[str, Any]:
    task_id = payload.get("task_id")
    if not isinstance(task_id, str) or not task_id:
        task_id = None
    root_identity = payload.get("root", payload.get("root_identity", {}))
    if not isinstance(root_identity, dict):
        root_identity = {}
    elapsed_ms = payload.get("elapsed_ms")
    durations = payload.get("durations")
    if elapsed_ms is None and isinstance(durations, dict):
        values = [value for value in durations.values() if isinstance(value, (int, float)) and value >= 0]
        if values and len(values) == len(durations):
            elapsed_ms = sum(values)
    return {
        "invocation_id": payload.get("invocation_id"),
        "task_id": task_id,
        "phase": payload.get("phase", source),
        "status": payload.get("status"),
        "scope": payload.get("scope"),
        "changed_paths": payload.get("changed_paths"),
        "head_sha": root_identity.get(
            "HEAD", root_identity.get(
                "head", root_identity.get("head_sha", payload.get("head_sha", payload.get("head")))
            )
        ),
        "index_tree": root_identity.get(
            "INDEX", root_identity.get(
                "index", root_identity.get("index_tree", payload.get("index_tree"))
            )
        ),
        "reused_phases": payload.get("reused_phases"),
        "executed_phases": payload.get("executed_phases"),
        "skipped_phases": payload.get("skipped_phases"),
        "durations": payload.get("durations"),
        "elapsed_ms": elapsed_ms,
        "model_usage": payload.get("model_usage"),
    }


def _summary_history(receipt_dir: Path) -> list[dict[str, Any]]:
    """Read check summaries, preferring the latest copy for each invocation."""

    payloads: list[dict[str, Any]] = []
    history_path = receipt_dir / "summary.jsonl"
    if history_path.is_file():
        try:
            lines = history_path.read_text(encoding="utf-8").splitlines()
        except OSError:
            lines = []
        for line in lines:
            try:
                payload = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(payload, dict):
                payloads.append(payload)
    latest_path = receipt_dir / "summary.json"
    if latest_path.is_file():
        try:
            payload = json.loads(latest_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            payload = None
        if isinstance(payload, dict):
            payloads.append(payload)
    by_invocation: dict[str, int] = {}
    summaries: list[dict[str, Any]] = []
    for payload in payloads:
        summary = _receipt_summary_payload(payload, "summary")
        invocation_id = summary["invocation_id"]
        if isinstance(invocation_id, str) and invocation_id:
            previous = by_invocation.get(invocation_id)
            if previous is not None:
                summaries[previous] = summary
                continue
            by_invocation[invocation_id] = len(summaries)
        summaries.append(summary)
    return summaries


def committed_task_ids(root: Path, baseline_commit: str) -> tuple[list[str], list[str]]:
    """Resolve distinct task trailers after an immutable baseline in commit order."""

    output = _git_output(
        root,
        "log",
        "--reverse",
        "--format=%H%x00%(trailers:key=Task-ID,valueonly,unfold)%x00",
        f"{baseline_commit}..HEAD",
    )
    parts = output.split("\0")
    task_ids: list[str] = []
    unavailable: list[str] = []
    for index in range(0, len(parts) - 1, 2):
        commit = parts[index].strip()
        trailer = parts[index + 1].strip()
        if not commit or not trailer:
            continue
        values = [value.strip() for value in trailer.splitlines() if value.strip()]
        if len(values) != 1:
            unavailable.append(commit)
            continue
        if values[0] not in task_ids:
            task_ids.append(values[0])
    return task_ids, unavailable


def committed_task_identities(root: Path, baseline_commit: str) -> dict[str, dict[str, str]]:
    """Resolve task commits to the parent HEAD and resulting commit tree."""

    output = _git_output(
        root,
        "log",
        "--reverse",
        "--format=%H%x00%(trailers:key=Task-ID,valueonly,unfold)%x00",
        f"{baseline_commit}..HEAD",
    )
    parts = output.split("\0")
    identities: dict[str, dict[str, str]] = {}
    for index in range(0, len(parts) - 1, 2):
        commit = parts[index].strip()
        trailer = parts[index + 1].strip()
        values = [value.strip() for value in trailer.splitlines() if value.strip()]
        if not commit or len(values) != 1 or values[0] in identities:
            continue
        parents = _git_output(root, "show", "-s", "--format=%P", commit).split()
        if len(parents) != 1:
            continue
        tree = _git_output(root, "rev-parse", f"{commit}^{{tree}}").strip()
        if SHA_PATTERN.fullmatch(parents[0]) and SHA_PATTERN.fullmatch(tree):
            identities[values[0]] = {
                "parent_head": parents[0],
                "commit_tree": tree,
            }
    return identities


def efficiency_cohort(
    tasks: Sequence[dict[str, Any]],
    baseline_commit: str,
    known_invocations: Sequence[str] = (),
    test_commands: Sequence[str] = (),
    receipt_dir: Path | None = None,
    root: Path = ROOT,
) -> dict[str, Any]:
    """Create a small honest measurement ledger for the next five basic tasks."""

    validate_backlog(tasks)
    if not SHA_PATTERN.fullmatch(baseline_commit):
        raise BacklogError("efficiency baseline_commit must be a full commit SHA")
    baseline = next((task for task in tasks if task["id"] == EFFICIENCY_BASELINE_TASK), None)
    if baseline is None or baseline["level"] != "basic":
        raise BacklogError(f"{EFFICIENCY_BASELINE_TASK} must exist as a basic baseline task")
    if not all(isinstance(value, str) and value.strip() for value in (*known_invocations, *test_commands)):
        raise BacklogError("known invocations and test commands must be non-empty strings")
    try:
        committed_ids, unavailable_commits = committed_task_ids(root, baseline_commit)
    except BacklogError:
        committed_ids, unavailable_commits = [], [baseline_commit]
    task_by_id = {task["id"]: task for task in tasks}
    unavailable_task_ids = [task_id for task_id in committed_ids if task_id not in task_by_id]
    candidates = [
        task_by_id[task_id]
        for task_id in committed_ids
        if task_id in task_by_id
        and task_id != EFFICIENCY_BASELINE_TASK
        and task_by_id[task_id]["level"] == "basic"
    ][:EFFICIENCY_TARGET_SIZE]
    receipts: list[dict[str, Any]] = []
    if receipt_dir is not None and receipt_dir.is_dir():
        receipt_paths = sorted(receipt_dir.glob("*.receipt.json"))
        for receipt_path in receipt_paths:
            summary = _receipt_summary(receipt_path)
            if summary is not None:
                receipts.append(summary)
        receipts.extend(_summary_history(receipt_dir))
        receipts = [
            receipt
            for receipt in receipts
            if receipt["task_id"] or (receipt["head_sha"] and receipt["index_tree"])
        ]
    by_task = {task["id"]: [] for task in candidates}
    unmatched_receipts = []
    try:
        task_identities = committed_task_identities(root, baseline_commit)
    except BacklogError:
        task_identities = {}
    for receipt in receipts:
        if receipt["task_id"] in by_task:
            by_task[receipt["task_id"]].append(receipt)
            continue
        matches = [
            task_id
            for task_id in by_task
            if receipt["task_id"] is None
            and receipt["head_sha"]
            and receipt["index_tree"]
            and task_identities.get(task_id, {}).get("parent_head") == receipt["head_sha"]
            and task_identities.get(task_id, {}).get("commit_tree") == receipt["index_tree"]
        ]
        if len(matches) == 1:
            by_task[matches[0]].append(receipt)
        elif receipt["task_id"] is not None:
            unmatched_receipts.append(receipt)
    task_records = []
    for task in candidates:
        task_receipts = by_task[task["id"]]
        elapsed_values = {
            receipt["elapsed_ms"]
            for receipt in task_receipts
            if receipt["elapsed_ms"] is not None
        }
        usage_values = {
            json.dumps(receipt["model_usage"], sort_keys=True)
            for receipt in task_receipts
            if receipt["model_usage"] is not None
        }
        elapsed_ms = next(iter(elapsed_values)) if len(elapsed_values) == 1 else None
        model_usage = json.loads(next(iter(usage_values))) if len(usage_values) == 1 else None
        receipt_available = any(
            receipt["status"] == "PASS"
            and receipt["head_sha"]
            and receipt["index_tree"]
            for receipt in task_receipts
        )
        task_records.append({
            "task_id": task["id"],
            "status": task["status"],
            "check_summaries": task_receipts,
            "elapsed_ms": elapsed_ms,
            "model_usage": model_usage,
            "availability": "available" if receipt_available else "unavailable",
        })
    missing_metrics = []
    if not receipts:
        missing_metrics.extend(("checks", "elapsed_ms", "model_usage"))
    else:
        if unmatched_receipts:
            missing_metrics.append("task_join")
        if not task_records or all(not record["check_summaries"] for record in task_records):
            missing_metrics.append("checks")
        if any(record["elapsed_ms"] is None for record in task_records):
            missing_metrics.append("elapsed_ms")
        if any(record["model_usage"] is None for record in task_records):
            missing_metrics.append("model_usage")
    if unavailable_commits or unavailable_task_ids:
        missing_metrics.append("task_mapping")
    return {
        "schema_version": EFFICIENCY_SCHEMA_VERSION,
        "baseline": {"task_id": EFFICIENCY_BASELINE_TASK, "commit": baseline_commit},
        "target_tasks": EFFICIENCY_TARGET_SIZE,
        "comparison": {
            "baseline": {
                "checks": None,
                "elapsed_ms": None,
                "model_usage": None,
                "availability": "unavailable",
            },
            "next_tasks": task_records,
        },
        "tasks": task_records,
        "preworkflow": {
            "known_invocations": list(known_invocations),
            "test_commands": list(test_commands),
            "availability": "available" if known_invocations or test_commands else "unavailable",
        },
        "coverage": {
            "selected_tasks": len(task_records),
            "completed_tasks": sum(record["status"] == "done" for record in task_records),
            "partial": len(task_records) < EFFICIENCY_TARGET_SIZE or bool(missing_metrics),
            "missing_metrics": sorted(set(missing_metrics)),
            "receipt_count": len(receipts),
            "unmatched_check_summaries": len(unmatched_receipts),
            "unavailable_commits": unavailable_commits,
            "unavailable_task_ids": unavailable_task_ids,
        },
    }


def markdown_escape(value: str) -> str:
    escaped = html.escape(value, quote=False)
    for marker in "\\|*_[]()`~#!>":
        escaped = escaped.replace(marker, f"\\{marker}")
    return escaped.replace("\r\n", "\n").replace("\n", "<br>")


def render_backlog(tasks: Sequence[dict[str, Any]]) -> str:
    validate_backlog(tasks)
    lines = ["# AI task backlog", "", "Generated by `python3 script/ai_tasks.py render`; edit [backlog.json](backlog.json), then rerender. Product sequencing lives in [ROADMAP.md](ROADMAP.md).", "", "|ID|Title|Level|Parent|Kind|Status|Priority|Complexity|Created|Route|Milestone|Depends on|", "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for task in sorted(tasks, key=lambda item: item["id"]):
        values = [task["id"], task["title"], task["level"], task["parent"] or "—", task["kind"], task["status"], task["priority"], task["complexity"], task["created_at"], task["route"], task["milestone"], ", ".join(task["depends_on"]) or "—"]
        lines.append("|" + "|".join(markdown_escape(value) for value in values) + "|")
    return "\n".join(lines) + "\n"


def render_list(values: Sequence[str]) -> list[str]:
    return [f"- {html.escape(value, quote=False)}" for value in values] or ["- None"]


def render_task(task: dict[str, Any], tasks: Sequence[dict[str, Any]], commits: Sequence[str] | None = None) -> str:
    validate_backlog(tasks)
    task_by_id = {item["id"]: item for item in tasks}
    if task["id"] not in task_by_id:
        raise BacklogError(f"unknown task ID: {task['id']}")
    commit_list = list(task["commits"] if commits is None else commits)
    if not all(isinstance(commit, str) and SHA_PATTERN.fullmatch(commit) for commit in commit_list):
        raise BacklogError("rendered commits must be full 40/64-character SHAs")
    children = sorted((item for item in tasks if item["parent"] == task["id"]), key=lambda item: item["id"])
    metadata = (("ID", task["id"]), ("Level", task["level"]), ("Parent", task["parent"] or "—"), ("Status", task["status"]), ("Finalized", str(task["finalized"]).lower()), ("Priority", task["priority"]), ("Complexity", task["complexity"]), ("Created", task["created_at"]), ("Resolved", task["resolved_at"] or "—"), ("Route", task["route"]), ("Milestone", task["milestone"]), ("Kind", task["kind"]), ("Baseline commit", task["baseline_commit"] or "—"), ("Motivation", task["motivation"]), ("Pre-existing context", task["context"]), ("Acceptance", task["acceptance"]), ("Evidence", task["evidence"] or "—"), ("Blocker", task["blocker"] or "—"), ("Owner", task["owner"] or "—"))
    lines = [f"# {task['id']} — {html.escape(task['title'], quote=False)}", "", "Generated by `python3 script/ai_tasks.py render`; edit `backlog.json`, then rerender.", "", "| Field | Value |", "|---|---|", *(f"| {markdown_escape(key)} | {markdown_escape(value)} |" for key, value in metadata), "", "## Plan", "", *render_list(task["plan"]), "", "## Children", ""]
    lines.extend((f"- {child['id']}: {html.escape(child['title'], quote=False)}" for child in children),) if children else lines.append("- None")
    lines.extend(["", "## Product changes", "", *render_list(task["product_changes"]), "", "## Code changes", "", *render_list(task["code_changes"]), "", "## Resolution", "", f"- Changes: {html.escape(task['resolution_changes'], quote=False) or '—'}"])
    if task["level"] != "basic":
        lines.extend(["", "## Retrospective", "", task["retrospective"] or "None", "", "## Documentation review", "", task["docs_review"] or "None", "", "## Environment review", "", task["environment_review"] or "None", "", "## Backlog review", "", task["backlog_review"] or "None"])
    lines.extend(["", "## Commits", ""])
    lines.extend((f"- [{commit}](https://github.com/CoreJust/MinecraftClone/commit/{commit})" for commit in commit_list),) if commit_list else lines.append("- No imported commits")
    lines.extend(["", f"Commit lookup: python3 script/ai_history.py show {task['id']}", ""])
    return "\n".join(lines)


def ready_tasks(tasks: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    validate_backlog(tasks)
    task_by_id = {task["id"]: task for task in tasks}
    return sorted((task for task in tasks if task["status"] == "ready" and all(task_by_id[dependency]["status"] == "done" for dependency in task["depends_on"])), key=lambda item: (PRIORITY_ORDER[item["priority"]], item["id"]))


def next_task_id(tasks: Iterable[dict[str, Any]]) -> str:
    highest = max((int(task["id"][len(ID_PREFIX):]) for task in tasks), default=0)
    if highest >= 9999:
        raise BacklogError("no MC-AI-#### IDs remain")
    return f"{ID_PREFIX}{highest + 1:04d}"


def write_text_atomic(target_path: Path, content: str) -> None:
    target_path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{target_path.name}.", dir=target_path.parent, text=True)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as output:
            output.write(content)
        os.replace(temporary_name, target_path)
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def write_backlog_atomic(backlog_path: Path, tasks: Sequence[dict[str, Any]]) -> None:
    validate_backlog(tasks)
    write_text_atomic(backlog_path, json.dumps(tasks, indent=2, ensure_ascii=False) + "\n")


def task_docs_dir(markdown_path: Path) -> Path:
    return markdown_path.parent / "tasks"


def write_rendered_backlog(markdown_path: Path, tasks: Sequence[dict[str, Any]]) -> None:
    validate_backlog(tasks)
    write_text_atomic(markdown_path, render_backlog(tasks))
    for task in tasks:
        write_text_atomic(task_docs_dir(markdown_path) / f"{task['id']}.md", render_task(task, tasks))


def check(backlog_path: Path, markdown_path: Path) -> None:
    tasks = load_backlog(backlog_path)
    validate_backlog(tasks)
    try:
        actual = markdown_path.read_text(encoding="utf-8")
    except OSError as error:
        raise BacklogError(f"cannot read {markdown_path}: {error}") from error
    if actual != render_backlog(tasks):
        raise BacklogError(f"generated Markdown is stale: run {ROOT / 'script' / 'ai_tasks.py'} render")
    for task in tasks:
        task_path = task_docs_dir(markdown_path) / f"{task['id']}.md"
        try:
            actual = task_path.read_text(encoding="utf-8")
        except OSError as error:
            raise BacklogError(f"cannot read {task_path}: {error}") from error
        if actual != render_task(task, tasks):
            raise BacklogError(f"generated task Markdown is stale: run {ROOT / 'script' / 'ai_tasks.py'} render")


def parse_dependencies(values: Sequence[str] | None) -> list[str] | None:
    if values is None:
        return None
    return [dependency for value in values for dependency in value.split(",") if dependency]


def add_task(tasks: list[dict[str, Any]], args: argparse.Namespace) -> dict[str, Any]:
    task = {"id": next_task_id(tasks), "title": args.title, "kind": args.kind, "status": "backlog", "priority": args.priority, "route": args.route, "milestone": args.milestone, "depends_on": parse_dependencies(args.depends_on) or [], "acceptance": args.acceptance, "evidence": "", "blocker": "", "owner": "", "level": args.level, "parent": args.parent, "motivation": args.motivation, "context": args.context, "complexity": args.complexity, "created_at": args.created_at, "resolved_at": args.resolved_at, "resolution_changes": args.resolution_changes, "plan": args.plan, "product_changes": args.product_change, "code_changes": args.code_change, "baseline_commit": args.baseline_commit, "commits": args.commit, "finalized": args.finalized, "retrospective": args.retrospective, "docs_review": args.docs_review, "environment_review": args.environment_review, "backlog_review": args.backlog_review}
    validate_backlog([*tasks, task])
    tasks.append(task)
    return task


def update_task(tasks: list[dict[str, Any]], args: argparse.Namespace) -> dict[str, Any]:
    task_id = args.id_option or args.id
    if not task_id:
        raise BacklogError("update requires an ID")
    if args.id_option and args.id and args.id_option != args.id:
        raise BacklogError("update ID and --id disagree")
    task = next((item for item in tasks if item["id"] == task_id), None)
    if task is None:
        raise BacklogError(f"unknown task ID: {task_id}")
    fields = ("title", "kind", "status", "priority", "route", "milestone", "acceptance", "evidence", "blocker", "owner", "level", "parent", "motivation", "context", "complexity", "created_at", "resolved_at", "resolution_changes", "baseline_commit", "finalized", "retrospective", "docs_review", "environment_review", "backlog_review")
    updates = {field: getattr(args, field) for field in fields if getattr(args, field) is not None}
    dependencies = parse_dependencies(args.depends_on)
    if dependencies is not None:
        updates["depends_on"] = dependencies
    for argument, field in (("plan", "plan"), ("product_change", "product_changes"), ("code_change", "code_changes"), ("commit", "commits")):
        if values := getattr(args, argument):
            updates[field] = [*task[field], *values]
    if not updates:
        raise BacklogError("update requires at least one field")
    candidate = [dict(item) for item in tasks]
    next(item for item in candidate if item["id"] == task_id).update(updates)
    validate_backlog(candidate)
    task.update(updates)
    return task


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backlog", type=Path, default=DEFAULT_BACKLOG_PATH)
    parser.add_argument("--markdown", type=Path, default=DEFAULT_MARKDOWN_PATH)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("check", help="validate JSON and generated Markdown")
    commands.add_parser("render", help="regenerate Markdown from JSON")
    commands.add_parser("ready", help="list actionable ready tasks")
    show = commands.add_parser("show", help="print one task as JSON")
    show.add_argument("id")
    packet = commands.add_parser("packet", help="print a compact task handoff packet")
    packet.add_argument("id")
    packet.add_argument("--root", type=Path, default=ROOT)
    packet.add_argument("--next-command", action="append", default=[])
    ci_record = commands.add_parser("ci-record", help="record terminal successful GitHub runs for one commit")
    ci_record.add_argument("--head", required=True)
    ci_record.add_argument("--workflow", default="")
    ci_record.add_argument("--root", type=Path, default=ROOT)
    ci_record.add_argument("--output", type=Path)
    cohort = commands.add_parser("efficiency-cohort", help="write the bounded next-five measurement ledger")
    cohort.add_argument("--baseline-commit", required=True)
    cohort.add_argument("--root", type=Path, default=ROOT)
    cohort.add_argument("--receipt-dir", type=Path)
    cohort.add_argument("--known-invocation", action="append", default=[])
    cohort.add_argument("--test-command", action="append", default=[])
    cohort.add_argument(
        "--output",
        type=Path,
        default=ROOT / "build" / "ai-checks" / "efficiency-cohort-v1.json",
    )
    add = commands.add_parser("add", help="append a backlog task")
    add.add_argument("title")
    add.add_argument("--kind", required=True, choices=sorted(KINDS))
    add.add_argument("--priority", required=True, choices=sorted(PRIORITIES))
    add.add_argument("--route", default="luna", choices=sorted(ROUTES))
    add.add_argument("--milestone", required=True)
    add.add_argument("--acceptance", required=True)
    add.add_argument("--level", required=True, choices=sorted(LEVELS))
    add.add_argument("--parent", default="")
    add.add_argument("--motivation", required=True)
    add.add_argument("--context", required=True)
    add.add_argument("--complexity", required=True, choices=sorted(COMPLEXITIES))
    add.add_argument("--created-at", required=True)
    add.add_argument("--resolved-at", default="")
    add.add_argument("--resolution-changes", default="")
    add.add_argument("--baseline-commit", default="")
    add.add_argument("--finalized", action="store_true")
    for field in ("retrospective", "docs_review", "environment_review", "backlog_review"):
        add.add_argument(f"--{field.replace('_', '-')}", default="")
    add.add_argument("--depends-on", nargs="*", default=None)
    for flag, destination in (("--plan", "plan"), ("--product-change", "product_change"), ("--code-change", "code_change"), ("--commit", "commit")):
        add.add_argument(flag, dest=destination, action="append", default=[])
    update = commands.add_parser("update", help="update one existing task")
    update.add_argument("id", nargs="?")
    update.add_argument("--id", dest="id_option")
    for field in ("title", "kind", "status", "priority", "route", "milestone", "acceptance", "evidence", "blocker", "owner", "level", "parent", "motivation", "context", "complexity", "created_at", "resolved_at", "resolution_changes", "baseline_commit", "retrospective", "docs_review", "environment_review", "backlog_review"):
        update.add_argument(f"--{field.replace('_', '-')}")
    update.add_argument("--finalized", action="store_const", const=True, default=None)
    update.add_argument("--not-finalized", dest="finalized", action="store_const", const=False)
    update.add_argument("--depends-on", nargs="*", default=None)
    for flag, destination in (("--plan", "plan"), ("--product-change", "product_change"), ("--code-change", "code_change"), ("--commit", "commit")):
        update.add_argument(flag, dest=destination, action="append", default=[])
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = make_parser()
    args = parser.parse_args(argv)
    try:
        if args.command == "check":
            check(args.backlog, args.markdown)
            print("backlog and generated Markdown are valid")
            return 0
        if args.command == "ci-record":
            record = collect_ci_record(args.root.resolve(), args.head, args.workflow)
            if args.output:
                write_immutable_json(args.output, record)
            print(json.dumps(record, indent=2, ensure_ascii=False, sort_keys=True))
            return 0
        tasks = load_backlog(args.backlog)
        validate_backlog(tasks)
        if args.command == "render":
            write_rendered_backlog(args.markdown, tasks)
            print(f"rendered {args.markdown}")
            return 0
        if args.command == "ready":
            for task in ready_tasks(tasks):
                print(f"{task['id']}\t{task['priority']}\t{task['route']}\t{task['title']}")
            return 0
        if args.command == "show":
            task = next((item for item in tasks if item["id"] == args.id), None)
            if task is None:
                raise BacklogError(f"unknown task ID: {args.id}")
            print(json.dumps(task, indent=2, ensure_ascii=False))
            return 0
        if args.command == "packet":
            packet = compact_task_packet(
                tasks,
                args.id,
                root=args.root.resolve(),
                next_commands=args.next_command,
            )
            print(json.dumps(packet, indent=2, ensure_ascii=False, sort_keys=True))
            return 0
        if args.command == "efficiency-cohort":
            receipt_dir = args.receipt_dir or args.root / "build" / "ai-checks"
            cohort = efficiency_cohort(
                tasks,
                args.baseline_commit,
                known_invocations=args.known_invocation,
                test_commands=args.test_command,
                receipt_dir=receipt_dir,
                root=args.root.resolve(),
            )
            write_immutable_json(args.output, cohort)
            print(json.dumps(cohort, indent=2, ensure_ascii=False, sort_keys=True))
            return 0
        task = add_task(tasks, args) if args.command == "add" else update_task(tasks, args)
        write_backlog_atomic(args.backlog, tasks)
        write_rendered_backlog(args.markdown, tasks)
        print(task["id"])
        return 0
    except BacklogError as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
