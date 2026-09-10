#!/usr/bin/env python3
"""Select and prepare canonical AI tasks using numeric task IDs."""

from __future__ import annotations

import argparse
from datetime import date
import json
from pathlib import Path
import re
from typing import Any, Sequence

import ai_tasks


DEFAULT_CURRENT_PATH = ai_tasks.ROOT / "docs" / "ai" / "current.json"
CURRENT_LEVELS = ("snapshot", "minor", "major")
VERSION_PATTERN = re.compile(r"(?<!\d)(\d+(?:\.\d+){1,2}(?::\d+)?)(?!\d)")


class PlanError(ValueError):
    pass


def task_id(number: str) -> str:
    if not number.isdecimal() or int(number) < 1:
        raise PlanError("task IDs must be positive decimal numbers")
    return f"{ai_tasks.ID_PREFIX}{int(number):04d}"


def task_number(task: dict[str, Any]) -> str:
    return str(int(task["id"][len(ai_tasks.ID_PREFIX):]))


def task_by_number(tasks: Sequence[dict[str, Any]], number: str) -> dict[str, Any]:
    wanted = task_id(number)
    task = next((item for item in tasks if item["id"] == wanted), None)
    if task is None:
        raise PlanError(f"unknown task {number}")
    return task


def load_current(current_path: Path) -> dict[str, str]:
    try:
        current = json.loads(current_path.read_text(encoding="utf-8"))
    except OSError as error:
        raise PlanError(f"cannot read {current_path}: {error}") from error
    except json.JSONDecodeError as error:
        raise PlanError(f"invalid JSON in {current_path}: {error}") from error
    if set(current) != set(CURRENT_LEVELS) or not all(isinstance(current[level], str) for level in CURRENT_LEVELS):
        raise PlanError("current selection must contain only snapshot, minor, and major IDs")
    return current


def current_tasks(
    tasks: Sequence[dict[str, Any]],
    current_path: Path,
    required_level: str | None = None,
    require_active: bool = False,
    selection: dict[str, str] | None = None,
) -> dict[str, dict[str, Any]]:
    ai_tasks.validate_backlog(tasks)
    current = load_current(current_path) if selection is None else selection
    task_by_id = {task["id"]: task for task in tasks}
    selected: dict[str, dict[str, Any]] = {}
    for level in CURRENT_LEVELS:
        if not current[level]:
            continue
        task = task_by_id.get(current[level])
        if task is None or task["level"] != level:
            raise PlanError(f"current {level} does not identify a {level} task")
        if task["status"] not in {"active", "done"}:
            raise PlanError(f"current {level} must be active or done")
        selected[level] = task
        active = [item for item in tasks if item["level"] == level and item["status"] == "active"]
        if task["status"] == "active" and (len(active) != 1 or active[0]["id"] != task["id"]):
            raise PlanError(f"current {level} is ambiguous")
    if "snapshot" in selected and "minor" not in selected:
        raise PlanError("current snapshot requires a current minor")
    if "minor" in selected and "major" not in selected:
        raise PlanError("current minor requires a current major")
    if "snapshot" in selected and selected["snapshot"]["parent"] != selected["minor"]["id"]:
        raise PlanError("current snapshot is not a child of current minor")
    if "minor" in selected and selected["minor"]["parent"] != selected["major"]["id"]:
        raise PlanError("current minor is not a child of current major")
    if required_level is not None:
        needed = CURRENT_LEVELS[CURRENT_LEVELS.index(required_level):]
        missing = [level for level in needed if level not in selected]
        if missing:
            raise PlanError(f"no current {missing[0]} selected")
        if require_active and selected[required_level]["status"] != "active":
            raise PlanError(f"current {required_level} must be active")
    return selected


def task_versions(task: dict[str, Any]) -> set[str]:
    return set(VERSION_PATTERN.findall(f"{task['title']} {task['milestone']}"))


def aggregate_for_identifier(tasks: Sequence[dict[str, Any]], level: str, identifier: str | None, current_path: Path) -> dict[str, Any]:
    if identifier is None:
        return current_tasks(tasks, current_path, level)[level]
    candidates = [task for task in tasks if task["level"] == level and identifier in {task["title"], task["milestone"]} | task_versions(task)]
    if not candidates:
        raise PlanError(f"no {level} matches {identifier}")
    if len(candidates) != 1:
        raise PlanError(f"{level} identifier {identifier} is ambiguous")
    return candidates[0]


def format_task(task: dict[str, Any]) -> str:
    return f"{task_number(task)}\t{task['level']}\t{task['status']}\t{task['priority']}\t{task['title']}"


def render_lookup(task: dict[str, Any], tasks: Sequence[dict[str, Any]]) -> str:
    rendered = ai_tasks.render_task(task, tasks)
    return rendered.replace(f"# {task['id']} —", f"# Task {task_number(task)} —", 1)


def descendants(tasks: Sequence[dict[str, Any]], root_id: str) -> list[dict[str, Any]]:
    by_parent: dict[str, list[dict[str, Any]]] = {}
    for task in tasks:
        if task["parent"]:
            by_parent.setdefault(task["parent"], []).append(task)
    found: list[dict[str, Any]] = []
    pending = list(by_parent.get(root_id, []))
    while pending:
        task = pending.pop()
        found.append(task)
        pending.extend(by_parent.get(task["id"], []))
    return found


def ordered_pending_basics(tasks: Sequence[dict[str, Any]], aggregate: dict[str, Any]) -> list[dict[str, Any]]:
    pending = [task for task in descendants(tasks, aggregate["id"]) if task["level"] == "basic" and task["status"] != "done"]
    pending_by_id = {task["id"]: task for task in pending}
    unresolved = {task["id"]: {dependency for dependency in task["depends_on"] if dependency in pending_by_id} for task in pending}
    ordered: list[dict[str, Any]] = []
    while unresolved:
        ready = sorted((task_id for task_id, dependencies in unresolved.items() if not dependencies), key=lambda item: int(item[len(ai_tasks.ID_PREFIX):]))
        if not ready:
            raise PlanError("pending task dependencies are cyclic")
        for task_id in ready:
            ordered.append(pending_by_id[task_id])
            del unresolved[task_id]
        for dependencies in unresolved.values():
            dependencies.difference_update(ready)
    return ordered


def format_pending(tasks: Sequence[dict[str, Any]], aggregate: dict[str, Any]) -> str:
    rows = [format_task(aggregate)]
    for task in ordered_pending_basics(tasks, aggregate):
        dependencies = ",".join(str(int(item[len(ai_tasks.ID_PREFIX):])) for item in task["depends_on"]) or "-"
        rows.append(f"{format_task(task)}\tdeps={dependencies}")
    return "\n".join(rows)


def new_basic(tasks: Sequence[dict[str, Any]], description: str, parent: str = "") -> dict[str, Any]:
    if not description.strip():
        raise PlanError("task description must be non-empty")
    return {
        "id": ai_tasks.next_task_id(tasks), "title": description, "kind": "chore",
        "status": "backlog", "priority": "P2", "route": "luna", "milestone": "Unassigned",
        "depends_on": [], "acceptance": f"Plan and deliver: {description}", "evidence": "",
        "blocker": "", "owner": "", "level": "basic", "parent": parent,
        "motivation": description, "context": "Created through the task planning interface.",
        "complexity": "medium", "created_at": date.today().isoformat(), "resolved_at": "",
        "resolution_changes": "", "plan": ["Clarify acceptance and implementation steps."],
        "product_changes": [], "code_changes": [], "baseline_commit": "", "commits": [],
        "finalized": False, "retrospective": "", "docs_review": "",
        "environment_review": "", "backlog_review": "",
    }


def write_tasks(backlog_path: Path, markdown_path: Path, tasks: Sequence[dict[str, Any]]) -> None:
    ai_tasks.write_backlog_atomic(backlog_path, tasks)
    ai_tasks.write_rendered_backlog(markdown_path, tasks)


def prepare_implementation(tasks: list[dict[str, Any]], target: str, current_path: Path, owner: str) -> dict[str, Any]:
    snapshot = current_tasks(tasks, current_path, "snapshot", require_active=True)["snapshot"]
    if target.isdecimal():
        task = task_by_number(tasks, target)
        if task["level"] != "basic":
            raise PlanError("implement accepts basic tasks only")
        if task["status"] == "done":
            raise PlanError(f"task {target} is already done")
        if task["parent"] not in {"", snapshot["id"]}:
            raise PlanError(f"task {target} belongs to another snapshot")
    else:
        task = new_basic(tasks, target, snapshot["id"])
        tasks.append(task)
    task_by_id = {item["id"]: item for item in tasks}
    unfinished = [dependency for dependency in task["depends_on"] if task_by_id[dependency]["status"] != "done"]
    if unfinished:
        rendered = ", ".join(str(int(item[len(ai_tasks.ID_PREFIX):])) for item in unfinished)
        raise PlanError(f"task {task_number(task)} has unfinished dependencies: {rendered}")
    task["parent"] = snapshot["id"]
    task["status"] = "active"
    task["owner"] = owner
    try:
        ai_tasks.validate_backlog(tasks)
    except ai_tasks.BacklogError as error:
        raise PlanError(str(error)) from error
    return task


def select_current(tasks: Sequence[dict[str, Any]], level: str, number: str, current_path: Path) -> dict[str, str]:
    task = task_by_number(tasks, number)
    if task["level"] != level or task["status"] != "active":
        raise PlanError(f"task {number} is not an active {level}")
    active = [item for item in tasks if item["level"] == level and item["status"] == "active"]
    if len(active) != 1:
        raise PlanError(f"current {level} is ambiguous")
    current = load_current(current_path)
    task_by_id = {item["id"]: item for item in tasks}
    if level == "major":
        current["major"] = task["id"]
        if not current["minor"] or task_by_id.get(current["minor"], {}).get("parent") != task["id"]:
            current["minor"] = ""
            current["snapshot"] = ""
    elif level == "minor":
        if not current["major"] or task["parent"] != current["major"]:
            raise PlanError("selected minor is not a child of current major")
        current["minor"] = task["id"]
        if not current["snapshot"] or task_by_id.get(current["snapshot"], {}).get("parent") != task["id"]:
            current["snapshot"] = ""
    else:
        if not current["minor"] or task["parent"] != current["minor"]:
            raise PlanError("selected snapshot is not a child of current minor")
        current["snapshot"] = task["id"]
    current_tasks(tasks, current_path, selection=current)
    ai_tasks.write_text_atomic(current_path, json.dumps(current, indent=2) + "\n")
    return current


def clear_current(tasks: Sequence[dict[str, Any]], level: str, current_path: Path) -> dict[str, str]:
    current_tasks(tasks, current_path, level)
    current = load_current(current_path)
    selected_id = current[level]
    if not selected_id:
        raise PlanError(f"no current {level} selected")
    task = next((item for item in tasks if item["id"] == selected_id), None)
    if task is None or task["level"] != level:
        raise PlanError(f"current {level} does not identify a {level} task")
    if task["status"] != "done":
        raise PlanError(f"current {level} must be done before clearing")
    for child_level in CURRENT_LEVELS[: CURRENT_LEVELS.index(level) + 1]:
        current[child_level] = ""
    ai_tasks.write_text_atomic(current_path, json.dumps(current, indent=2) + "\n")
    return current


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backlog", type=Path, default=ai_tasks.DEFAULT_BACKLOG_PATH)
    parser.add_argument("--markdown", type=Path, default=ai_tasks.DEFAULT_MARKDOWN_PATH)
    parser.add_argument("--current", type=Path, default=DEFAULT_CURRENT_PATH)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("check", help="validate the current aggregate registry")
    task = commands.add_parser("task", help="show a task by numeric ID")
    task.add_argument("number")
    for level in CURRENT_LEVELS:
        aggregate = commands.add_parser(level, help=f"show the selected {level}")
        aggregate.add_argument("identifier", nargs="?")
        pending = commands.add_parser(f"implement-{level}", help=f"list pending basic tasks under the current {level}")
    commands.add_parser("backlog", help="list unassigned unfinished basic tasks")
    plan_for = commands.add_parser("plan-for", help="create an unassigned basic task")
    plan_for.add_argument("description")
    implement = commands.add_parser("implement", help="prepare one basic task; does not implement code")
    implement.add_argument("target")
    implement.add_argument("--prepare", action="store_true")
    implement.add_argument("--owner", default="Codex")
    select = commands.add_parser("select", help="set one coherent current aggregate")
    select.add_argument("level", choices=CURRENT_LEVELS)
    select.add_argument("number")
    clear = commands.add_parser("clear", help="clear a completed current aggregate and descendants")
    clear.add_argument("level", choices=CURRENT_LEVELS)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = make_parser()
    args = parser.parse_args(argv)
    try:
        tasks = ai_tasks.load_backlog(args.backlog)
        ai_tasks.validate_backlog(tasks)
        if args.command == "check":
            current_tasks(tasks, args.current)
            print("current selection is valid")
            return 0
        if args.command == "task":
            print(render_lookup(task_by_number(tasks, args.number), tasks), end="")
            return 0
        if args.command in CURRENT_LEVELS:
            print(render_lookup(aggregate_for_identifier(tasks, args.command, args.identifier, args.current), tasks), end="")
            return 0
        if args.command.startswith("implement-"):
            level = args.command.removeprefix("implement-")
            print(format_pending(tasks, current_tasks(tasks, args.current, level)[level]))
            return 0
        if args.command == "backlog":
            for task in sorted((item for item in tasks if item["level"] == "basic" and not item["parent"] and item["status"] != "done"), key=lambda item: int(item["id"][len(ai_tasks.ID_PREFIX):])):
                print(format_task(task))
            return 0
        if args.command == "plan-for":
            task = new_basic(tasks, args.description)
            candidate = [*tasks, task]
            ai_tasks.validate_backlog(candidate)
            write_tasks(args.backlog, args.markdown, candidate)
            print(task_number(task))
            return 0
        if args.command == "implement":
            if not args.prepare:
                raise PlanError("implement requires --prepare; it only prepares task metadata")
            candidate = [dict(task) for task in tasks]
            task = prepare_implementation(candidate, args.target, args.current, args.owner)
            write_tasks(args.backlog, args.markdown, candidate)
            print(format_task(task))
            return 0
        if args.command == "clear":
            clear_current(tasks, args.level, args.current)
            print(f"cleared {args.level}")
            return 0
        current = select_current(tasks, args.level, args.number, args.current)
        print(f"{args.level}\t{task_number({'id': current[args.level]})}")
        return 0
    except (PlanError, ai_tasks.BacklogError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
