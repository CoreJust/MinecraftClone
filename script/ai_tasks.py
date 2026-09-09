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
    add = commands.add_parser("add", help="append a backlog task")
    add.add_argument("title")
    add.add_argument("--kind", required=True, choices=sorted(KINDS))
    add.add_argument("--priority", required=True, choices=sorted(PRIORITIES))
    add.add_argument("--route", default="terra", choices=sorted(ROUTES))
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
        task = add_task(tasks, args) if args.command == "add" else update_task(tasks, args)
        write_backlog_atomic(args.backlog, tasks)
        write_rendered_backlog(args.markdown, tasks)
        print(task["id"])
        return 0
    except BacklogError as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
