#!/usr/bin/env python3
"""Render task history from immutable Git commit trailers."""

from __future__ import annotations

import argparse
from collections import defaultdict
from pathlib import Path
import subprocess
import sys

import ai_tasks


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BACKLOG = ROOT / "docs/ai/backlog.json"
DEFAULT_MARKDOWN = ROOT / "docs/ai/BACKLOG.md"
DEFAULT_OUTPUT = ROOT / "build/ai-tasks"


class HistoryError(RuntimeError):
    pass


def git(repo: Path, *args: str, check: bool = True) -> str:
    result = subprocess.run(
        ["git", *args], cwd=repo, text=True, capture_output=True, check=False
    )
    if check and result.returncode:
        raise HistoryError(result.stderr.strip() or "git " + " ".join(args) + " failed")
    return result.stdout


def revision(repo: Path, ref: str) -> str:
    return git(repo, "rev-parse", "--verify", f"{ref}^{{commit}}").strip()


def require_common_ancestor(repo: Path, baseline: str, head: str) -> None:
    result = subprocess.run(["git", "merge-base", baseline, head], cwd=repo, capture_output=True)
    if result.returncode:
        raise HistoryError(f"baseline {baseline} has no common ancestry with {head}")


def aggregate_baseline(repo: Path, task: dict[str, object]) -> str:
    baseline = str(task["baseline_commit"])
    if not baseline:
        raise HistoryError(f"{task['id']} requires baseline_commit")
    return revision(repo, baseline)


def task_parent(task: dict[str, object]) -> str:
    return str(task["parent"])


def task_level(task: dict[str, object]) -> str:
    return str(task.get("level", "basic"))


def trailer_commits(repo: Path, tasks: list[dict[str, object]]) -> dict[str, list[str]]:
    by_id = {str(task["id"]): task for task in tasks}
    assigned: dict[str, str] = {}
    commits: dict[str, list[str]] = defaultdict(list)
    output = git(repo, "log", "--all", "--format=%H%x00%(trailers:key=Task-ID,valueonly,unfold)%x00")
    fields = output.split("\0")
    for index in range(0, len(fields) - 1, 2):
        sha = fields[index].strip()
        trailer = fields[index + 1].strip()
        if not sha or not trailer:
            continue
        task_ids = [value.strip() for value in trailer.splitlines() if value.strip()]
        if len(task_ids) != 1:
            raise HistoryError(f"commit {sha} has {len(task_ids)} Task-ID trailers")
        task_id = task_ids[0]
        if task_id not in by_id:
            raise HistoryError(f"commit {sha} has unknown Task-ID {task_id}")
        assigned[sha] = task_id

    for task in tasks:
        task_id = str(task["id"])
        for imported in task.get("commits", []):
            imported_sha = revision(repo, str(imported))
            previous = assigned.get(imported_sha)
            if previous:
                raise HistoryError(f"commit {imported_sha} belongs to both {previous} and {task_id}")
            assigned[imported_sha] = task_id

    for sha, task_id in assigned.items():
        commits[task_id].append(sha)
    return {task_id: sorted(shas) for task_id, shas in commits.items()}


def output_path(output: Path, task_id: str) -> Path:
    return output / f"{task_id}.md"


def render_records(repo: Path, tasks: list[dict[str, object]], output: Path, only: str | None = None) -> list[Path]:
    commits = trailer_commits(repo, tasks)
    output.mkdir(parents=True, exist_ok=True)
    rendered: list[Path] = []
    for task in tasks:
        task_id = str(task["id"])
        if only and task_id != only:
            continue
        path = output_path(output, task_id)
        ai_tasks.write_text_atomic(
            path, ai_tasks.render_task(task, tasks, commits=commits.get(task_id, []))
        )
        rendered.append(path)
    if only and not rendered:
        raise HistoryError(f"unknown task ID {only}")
    return rendered


def range_commits(
    repo: Path, baseline: str, head: str, additional_head: str | None = None
) -> list[str]:
    require_common_ancestor(repo, baseline, head)
    heads = [head]
    if additional_head:
        require_common_ancestor(repo, baseline, additional_head)
        heads.append(additional_head)
    return git(repo, "rev-list", "--topo-order", *heads, f"^{baseline}").split()


def mapped_range_ids(
    repo: Path, tasks: list[dict[str, object]], range_shas: list[str]
) -> set[str]:
    commits = trailer_commits(repo, tasks)
    commit_to_task = {sha: task_id for task_id, shas in commits.items() for sha in shas}
    unassigned = [sha for sha in range_shas if sha not in commit_to_task]
    if unassigned:
        raise HistoryError(f"commit {unassigned[0]} has no Task-ID or legacy import")
    return {commit_to_task[sha] for sha in range_shas}


def require_aggregate_coverage(
    tasks: list[dict[str, object]], task_id: str, linked_ids: set[str]
) -> None:
    covered = descendants(tasks, task_id) | {task_id}
    outside = linked_ids - covered
    if outside:
        raise HistoryError(f"range contains tasks outside {task_id}: {', '.join(sorted(outside))}")
    missing = descendants(tasks, task_id) - linked_ids
    if missing:
        raise HistoryError(f"range omits descendant tasks: {', '.join(sorted(missing))}")


def collect_aggregate(
    repo: Path,
    tasks: list[dict[str, object]],
    task_id: str,
    head_ref: str,
    additional_head: str | None = None,
) -> list[str]:
    by_id = {str(task["id"]): task for task in tasks}
    task = by_id.get(task_id)
    if not task:
        raise HistoryError(f"unknown task ID {task_id}")
    level = task_level(task)
    if level not in {"snapshot", "minor", "major"}:
        raise HistoryError(f"{task_id} is not an aggregate task")
    baseline = aggregate_baseline(repo, task)
    head = revision(repo, head_ref)
    resolved_additional = revision(repo, additional_head) if additional_head else None
    linked_ids = mapped_range_ids(
        repo, tasks, range_commits(repo, baseline, head, resolved_additional)
    )
    if level == "snapshot":
        return sorted(task_id for task_id in linked_ids if task_level(by_id[task_id]) == "basic")
    require_aggregate_coverage(tasks, task_id, linked_ids)
    child_level = "snapshot" if level == "minor" else "minor"
    return sorted(
        str(child["id"])
        for child in tasks
        if task_parent(child) == task_id and task_level(child) == child_level
    )


def collect_snapshot(
    repo: Path,
    tasks: list[dict[str, object]],
    task_id: str,
    head_ref: str,
    additional_head: str | None = None,
) -> list[str]:
    task = next((item for item in tasks if item["id"] == task_id), None)
    if task is None or task_level(task) != "snapshot":
        raise HistoryError(f"{task_id} is not a snapshot task")
    return collect_aggregate(repo, tasks, task_id, head_ref, additional_head)


def descendants(tasks: list[dict[str, object]], parent_id: str) -> set[str]:
    children: dict[str, list[str]] = defaultdict(list)
    for task in tasks:
        children[task_parent(task)].append(str(task["id"]))
    result: set[str] = set()
    pending = list(children[parent_id])
    while pending:
        child = pending.pop()
        result.add(child)
        pending.extend(children[child])
    return result


def require_finalization_fields(task: dict[str, object], higher: bool) -> None:
    for field in ("product_changes", "code_changes"):
        if not task.get(field):
            raise HistoryError(f"{task['id']} requires {field}")
    for field in ("evidence", "resolution_changes", "resolved_at"):
        if not str(task.get(field, "")).strip():
            raise HistoryError(f"{task['id']} requires {field}")
    if higher:
        for field in ("retrospective", "docs_review", "environment_review", "backlog_review"):
            if not str(task.get(field, "")).strip():
                raise HistoryError(f"{task['id']} requires {field}")


def finalize(
    repo: Path,
    tasks: list[dict[str, object]],
    task_id: str,
    head_ref: str,
    additional_head: str | None = None,
) -> None:
    by_id = {str(task["id"]): task for task in tasks}
    target = by_id.get(task_id)
    if not target:
        raise HistoryError(f"unknown task ID {task_id}")
    level = task_level(target)
    if level not in {"snapshot", "minor", "major"}:
        raise HistoryError(f"{task_id} is not an aggregate task")
    baseline = aggregate_baseline(repo, target)
    head = revision(repo, head_ref)
    resolved_additional = revision(repo, additional_head) if additional_head else None
    range_shas = range_commits(repo, baseline, head, resolved_additional)
    linked_ids = mapped_range_ids(repo, tasks, range_shas)
    if level == "snapshot":
        actual = sorted(
            linked_id for linked_id in linked_ids if task_level(by_id[linked_id]) == "basic"
        )
        if not actual:
            raise HistoryError(f"snapshot {task_id} has no basic task commits")
        planned = [task for task in tasks if task_parent(task) == task_id and task_level(task) == "basic"]
        for child in planned:
            child_id = str(child["id"])
            if child_id not in actual or child.get("status") != "done":
                raise HistoryError(f"planned child {child_id} is missing or not done")
        for child_id in actual:
            child = by_id[child_id]
            parent = task_parent(child)
            if parent and parent != task_id:
                raise HistoryError(f"basic task {child_id} already belongs to {parent}")
            if child.get("status") != "done":
                raise HistoryError(f"basic task {child_id} is not done")
            child["parent"] = task_id
    else:
        allowed_levels = {"snapshot"} if level == "minor" else {"minor"}
        children = [task for task in tasks if task_parent(task) == task_id and task_level(task) in allowed_levels]
        if not children:
            raise HistoryError(f"{task_id} has no planned {next(iter(allowed_levels))} children")
        for child in children:
            if child.get("status") != "done":
                raise HistoryError(f"child {child['id']} is not done")
        require_aggregate_coverage(tasks, task_id, linked_ids)
    require_finalization_fields(target, higher=level in {"minor", "major"})
    target["finalized"] = True
    ai_tasks.validate_backlog(tasks)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--repo", type=Path, default=ROOT)
    result.add_argument("--backlog", type=Path, default=DEFAULT_BACKLOG)
    result.add_argument("--markdown", type=Path, default=DEFAULT_MARKDOWN)
    result.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    subparsers = result.add_subparsers(dest="command", required=True)
    subparsers.add_parser("refresh")
    show = subparsers.add_parser("show")
    show.add_argument("task_id")
    collect = subparsers.add_parser("collect")
    collect.add_argument("task_id")
    collect.add_argument("--head", default="HEAD")
    finalize_parser = subparsers.add_parser("finalize")
    finalize_parser.add_argument("task_id")
    finalize_parser.add_argument("--head", default="HEAD")
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        tasks = ai_tasks.load_backlog(args.backlog)
        ai_tasks.validate_backlog(tasks)
        if args.command == "refresh":
            render_records(args.repo, tasks, args.output)
        elif args.command == "show":
            print(render_records(args.repo, tasks, args.output, args.task_id)[0])
        elif args.command == "collect":
            print("\n".join(collect_aggregate(args.repo, tasks, args.task_id, args.head)))
        else:
            finalize(args.repo, tasks, args.task_id, args.head)
            ai_tasks.write_backlog_atomic(args.backlog, tasks)
            ai_tasks.write_rendered_backlog(args.markdown, tasks)
            render_records(args.repo, tasks, args.output)
    except (HistoryError, ai_tasks.BacklogError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
