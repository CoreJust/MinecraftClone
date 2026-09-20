#!/usr/bin/env python3
"""Promote one finalized AI snapshot without touching the legacy release line.

The three explicit stages intentionally leave the review boundary visible:
prepare creates a no-commit merge, finish requires the normal ai_commit receipt
for that exact index, and tag creates one new annotated AI namespace tag.
"""

from __future__ import annotations

import argparse
import copy
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Sequence

import ai_check
import ai_history
import ai_tasks


AI_DEV = "ai-dev"
AI_MAIN = "ai-main"
COMMIT_ID = re.compile(r"[0-9a-f]{40}(?:[0-9a-f]{24})?")


class PublishError(RuntimeError):
    pass


PROMOTION_STATE = "ai-publish-state.json"


def repository_root() -> Path:
    return Path(__file__).resolve().parents[1]


def git(root: Path, *arguments: str, input_text: str | None = None) -> str:
    result = subprocess.run(
        ["git", *arguments],
        cwd=root,
        text=True,
        input=input_text,
        capture_output=True,
        check=False,
    )
    if result.returncode:
        raise PublishError(result.stderr.strip() or "git " + " ".join(arguments))
    return result.stdout


def current_branch(root: Path) -> str:
    return git(root, "branch", "--show-current").strip()


def revision(root: Path, value: str) -> str:
    return git(root, "rev-parse", "--verify", f"{value}^{{commit}}").strip()


def require_branch(root: Path, expected: str) -> None:
    actual = current_branch(root)
    if actual != expected:
        raise PublishError(f"must run on {expected}, not {actual or 'a detached HEAD'}")


def require_clean(root: Path) -> None:
    dirty = git(root, "status", "--porcelain", "--untracked-files=all").strip()
    if dirty:
        raise PublishError("working tree must be clean before promotion")


def require_pending_merge_clean(root: Path) -> None:
    changed = subprocess.run(["git", "diff", "--quiet"], cwd=root, check=False)
    untracked = git(root, "ls-files", "--others", "--exclude-standard").strip()
    if changed.returncode or untracked:
        raise PublishError("pending promotion has unstaged or untracked changes")


def require_exact_commit(value: str) -> None:
    if not COMMIT_ID.fullmatch(value):
        raise PublishError("source must be a full immutable commit ID")


def load_snapshot(root: Path, task_id: str, head: str) -> dict[str, Any]:
    tasks = ai_tasks.load_backlog(root / "docs" / "ai" / "backlog.json")
    ai_tasks.validate_backlog(tasks)
    task = next((item for item in tasks if item["id"] == task_id), None)
    if task is None:
        raise PublishError(f"unknown task ID: {task_id}")
    if task["level"] != "snapshot":
        raise PublishError(
            f"{task_id} is {task['level']}; only explicit snapshot publication is supported"
        )
    if task["status"] != "active" or task["finalized"] is not True:
        raise PublishError(f"{task_id} must be active and finalized before promotion")
    try:
        ai_history.finalize(root, copy.deepcopy(tasks), task_id, head)
    except (ai_history.HistoryError, ai_tasks.BacklogError, OSError, ValueError) as error:
        raise PublishError(f"{task_id} aggregate is not ready: {error}") from error
    return task


def require_task_trailer(root: Path, commit: str, task_id: str) -> None:
    message = git(root, "log", "-1", "--format=%B", commit)
    trailers = git(root, "interpret-trailers", "--parse", input_text=message)
    values = []
    for line in trailers.splitlines():
        name, separator, value = line.partition(":")
        if separator and name.strip().lower() == "task-id":
            values.append(value.strip())
    if values != [task_id]:
        raise PublishError(f"{commit} must contain exactly one Task-ID: {task_id} trailer")


def run_check(root: Path, *arguments: str) -> None:
    command = [sys.executable, str(root / "script" / "ai_check.py"), *arguments]
    result = subprocess.run(command, cwd=root, check=False)
    if result.returncode:
        raise PublishError("AI release checks failed")


def check_ai_commit(root: Path) -> None:
    command = [sys.executable, str(root / "script" / "ai_commit.py"), "check"]
    result = subprocess.run(command, cwd=root, check=False)
    if result.returncode:
        raise PublishError("the pending promotion lacks a valid ai_commit review receipt")


def merge_head(root: Path) -> str:
    location = Path(git(root, "rev-parse", "--git-path", "MERGE_HEAD").strip())
    target = location if location.is_absolute() else root / location
    try:
        heads = [line for line in target.read_text(encoding="utf-8").splitlines() if line]
    except OSError as error:
        raise PublishError("finish requires the no-commit merge created by prepare") from error
    if len(heads) != 1:
        raise PublishError("only a single-parent snapshot merge may be promoted")
    return revision(root, heads[0])


def expected_merge_tree(root: Path, baseline: str, source: str) -> str:
    result = subprocess.run(
        ["git", "merge-tree", "--write-tree", baseline, source],
        cwd=root,
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode:
        raise PublishError("immutable baseline and source do not merge cleanly")
    values = result.stdout.split()
    if len(values) != 1 or not COMMIT_ID.fullmatch(values[0]):
        raise PublishError("cannot determine the expected immutable merge tree")
    return values[0]


def promotion_state_path(root: Path) -> Path:
    git_directory = Path(git(root, "rev-parse", "--git-dir").strip())
    if not git_directory.is_absolute():
        git_directory = root / git_directory
    return git_directory / PROMOTION_STATE


def write_promotion_state(root: Path, task_id: str, source: str, promotion_base: str) -> None:
    state = {
        "task_id": task_id,
        "source": source,
        "promotion_base": promotion_base,
    }
    promotion_state_path(root).write_text(json.dumps(state, sort_keys=True) + "\n", encoding="utf-8")


def read_promotion_state(root: Path, task_id: str, source: str) -> str:
    path = promotion_state_path(root)
    try:
        state = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PublishError("finish requires the promotion state created by prepare") from error
    if not isinstance(state, dict) or state.get("task_id") != task_id or state.get("source") != source:
        raise PublishError("pending promotion state does not match the requested task and source")
    promotion_base = state.get("promotion_base")
    if not isinstance(promotion_base, str) or not COMMIT_ID.fullmatch(promotion_base):
        raise PublishError("pending promotion state has an invalid ai-main base")
    return revision(root, promotion_base)


def require_expected_pending_tree(root: Path, baseline: str, source: str) -> str:
    unresolved = git(root, "diff", "--name-only", "--diff-filter=U").strip()
    if unresolved:
        raise PublishError("pending promotion has unresolved merge conflicts")
    expected = expected_merge_tree(root, baseline, source)
    compared = subprocess.run(
        ["git", "diff", "--cached", "--quiet", expected], cwd=root, check=False
    )
    if compared.returncode == 1:
        raise PublishError("pending promotion index differs from the expected immutable merge tree")
    if compared.returncode:
        raise PublishError("cannot compare the pending promotion index")
    return expected


def require_expected_promoted_tree(root: Path, baseline: str, source: str, promoted: str) -> None:
    expected = expected_merge_tree(root, baseline, source)
    actual = git(root, "rev-parse", f"{promoted}^{{tree}}").strip()
    if actual != expected:
        raise PublishError("promotion tree differs from the expected immutable merge tree")


def expected_tag(root: Path, promoted: str, revision_number: int | None = None) -> str:
    names, version = ai_check.project_version_arguments(root)
    major_name, separator, _minor_name = names.partition(":")
    numeric, separator2, snapshot = version.partition(":")
    if not separator or not separator2 or not major_name or not numeric or not snapshot:
        raise PublishError("ProjectInfo version is not a snapshot version")
    committed = git(root, "show", "-s", "--format=%cs", promoted).strip()
    try:
        year, month, day = committed.split("-")
        date = f"{year[2:]}.{month}.{day}"
    except ValueError as error:
        raise PublishError(f"{promoted} has no usable promotion date") from error
    revision_suffix = "" if revision_number is None else f"-r{revision_number}"
    return f"ai/{major_name}/{numeric}/{snapshot}{revision_suffix}_{date}"


def require_next_revision_tag(root: Path, promoted: str, revision_number: int) -> None:
    canonical = expected_tag(root, promoted).rsplit("_", 1)[0]
    prefix = f"{canonical}-r"
    existing = git(root, "tag", "--list", f"{canonical}_*", f"{prefix}*_*" ).splitlines()
    has_canonical = any(name.startswith(f"{canonical}_") for name in existing)
    revisions: list[int] = []
    for name in existing:
        if not name.startswith(prefix):
            continue
        number, separator, _date = name.removeprefix(prefix).partition("_")
        if separator and number.isdigit() and int(number) >= 1:
            revisions.append(int(number))
    if not has_canonical:
        raise PublishError("a correction tag requires the existing canonical snapshot tag")
    expected = max(revisions, default=0) + 1
    if revision_number != expected:
        raise PublishError(f"the next correction tag must use revision {expected}")
    promoted_source = git(root, "show", "-s", "--format=%P", promoted).split()
    if len(promoted_source) != 2:
        raise PublishError("a correction tag requires an ai-main promotion commit")
    for name in existing:
        previous_promotion = revision(root, f"{name}^{{commit}}")
        previous_parents = git(root, "show", "-s", "--format=%P", previous_promotion).split()
        if len(previous_parents) != 2:
            raise PublishError(f"existing snapshot tag is not an ai-main promotion: {name}")
        if previous_promotion == promoted or previous_parents[1] == promoted_source[1]:
            raise PublishError("a correction tag requires a newly promoted corrected source")


def prepare(root: Path, task_id: str, source_text: str) -> None:
    require_branch(root, AI_DEV)
    require_clean(root)
    require_exact_commit(source_text)
    source = revision(root, source_text)
    if source != revision(root, "HEAD"):
        raise PublishError("source must equal the current ai-dev HEAD")
    task = load_snapshot(root, task_id, source)
    revision(root, task["baseline_commit"])
    promotion_base = revision(root, AI_MAIN)
    require_task_trailer(root, source, task_id)
    run_check(root, "--strict", "--level", "snapshot", "--require-index-match")
    if current_branch(root) != AI_DEV or revision(root, "HEAD") != source:
        raise PublishError("ai-dev changed while release checks were running")
    if revision(root, AI_MAIN) != promotion_base:
        raise PublishError("ai-main changed while release checks were running")
    expected_merge_tree(root, promotion_base, source)
    git(root, "checkout", AI_MAIN)
    git(root, "merge", "--no-ff", "--no-commit", source)
    write_promotion_state(root, task_id, source, promotion_base)
    print("Pending promotion created. Review it, record its ai_commit receipt, then run finish.")


def finish(root: Path, task_id: str, source_text: str) -> None:
    require_branch(root, AI_MAIN)
    require_exact_commit(source_text)
    source = revision(root, source_text)
    if merge_head(root) != source:
        raise PublishError("pending merge does not match the requested immutable source commit")
    load_snapshot(root, task_id, source)
    promotion_base = read_promotion_state(root, task_id, source)
    if revision(root, "HEAD") != promotion_base:
        raise PublishError("ai-main changed after prepare; abort this merge and prepare again")
    require_pending_merge_clean(root)
    require_expected_pending_tree(root, promotion_base, source)
    check_ai_commit(root)
    run_check(root, "--candidate", "--level", "snapshot", "--require-index-match")
    require_pending_merge_clean(root)
    require_expected_pending_tree(root, promotion_base, source)
    check_ai_commit(root)
    git(root, "commit", "-m", f"Promote {task_id} to ai-main", "-m", f"Task-ID: {task_id}")
    promoted = revision(root, "HEAD")
    parents = git(root, "show", "-s", "--format=%P", promoted).split()
    if parents != [promotion_base, source]:
        raise PublishError("promotion commit parents changed unexpectedly")
    require_task_trailer(root, promoted, task_id)
    require_expected_promoted_tree(root, promotion_base, source, promoted)
    promotion_state_path(root).unlink(missing_ok=True)
    require_clean(root)
    run_check(root, "--strict", "--level", "snapshot", "--require-index-match")
    print(promoted)


def tag(root: Path, task_id: str, revision_number: int | None = None) -> None:
    require_branch(root, AI_MAIN)
    require_clean(root)
    task = load_snapshot(root, task_id, "HEAD")
    promoted = revision(root, "HEAD")
    revision(root, task["baseline_commit"])
    parents = git(root, "show", "-s", "--format=%P", promoted).split()
    if len(parents) != 2:
        raise PublishError("HEAD is not the expected ai-main promotion commit")
    promotion_base, source = parents
    require_task_trailer(root, promoted, task_id)
    require_task_trailer(root, source, task_id)
    require_expected_promoted_tree(root, promotion_base, source, promoted)
    if revision_number is not None:
        require_next_revision_tag(root, promoted, revision_number)
    name = expected_tag(root, promoted, revision_number)
    git(root, "check-ref-format", f"refs/tags/{name}")
    existing = subprocess.run(
        ["git", "show-ref", "--verify", "--quiet", f"refs/tags/{name}"], cwd=root, check=False
    )
    if existing.returncode == 0:
        raise PublishError(f"tag already exists and will not be rewritten: {name}")
    run_check(root, "--strict", "--level", "snapshot", "--require-index-match")
    git(root, "tag", "-a", name, promoted, "-m", f"AI snapshot {task_id}")
    if revision(root, name) != promoted:
        raise PublishError("new tag does not resolve to the promoted commit")
    print(name)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=repository_root(), help=argparse.SUPPRESS)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("prepare", "finish"):
        command = commands.add_parser(name)
        command.add_argument("task_id")
        command.add_argument("source_commit")
    tag_command = commands.add_parser("tag")
    tag_command.add_argument("task_id")
    tag_command.add_argument("--revision", type=int, choices=range(1, 1000))
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        if args.command == "prepare":
            prepare(root, args.task_id, args.source_commit)
        elif args.command == "finish":
            finish(root, args.task_id, args.source_commit)
        else:
            tag(root, args.task_id, args.revision)
    except (OSError, PublishError, ai_tasks.BacklogError, ValueError) as error:
        print(f"AI publish: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
