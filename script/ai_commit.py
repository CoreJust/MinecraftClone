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
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Sequence


LEVELS = frozenset({"basic", "snapshot", "minor", "major"})
MODEL_EFFORTS = {
    "gpt-6-luna": frozenset({"low", "medium", "high", "xhigh", "max"}),
    "gpt-6-sol": frozenset({"low", "medium", "high", "xhigh", "max", "ultra"}),
    "gpt-6.1-sol": frozenset({"low", "medium", "high", "xhigh", "max", "ultra"}),
    "gpt-6-astra": frozenset({"low", "medium", "high", "xhigh", "max", "ultra"}),
}
LUNA_MODEL = "gpt-6-luna"
SOL_MODEL = "gpt-6-sol"
LEGACY_REVIEW_MODELS = {"task": LUNA_MODEL, "aggregate": SOL_MODEL}
STAGED_EXPORT_TIMEOUT_SECONDS = 120
MAX_STAGED_TREE_ENTRIES = 50000
MAX_STAGED_TREE_BYTES = 256 * 1024 * 1024
STAGED_CHECK_GIT_ENV = ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE")
STAGED_FAILURE_LOG_COPY_TIMEOUT_SECONDS = 30
STAGED_FAILURE_LOG_MAX_BYTES = 4 * 1024 * 1024
STAGED_FAILURE_LOG_MAX_FILES = 128
STAGED_FAILURE_LOG_MAX_ENTRIES = 4096
STAGED_FAILURE_LOG_CHUNK_BYTES = 64 * 1024


class CommitGateError(ValueError):
    pass


def repository_root() -> Path:
    return Path(__file__).resolve().parents[1]


def git(
    root: Path,
    *args: str,
    input_text: str | None = None,
    env: dict[str, str] | None = None,
    timeout: float | None = None,
) -> str:
    try:
        completed = subprocess.run(
            ["git", *args],
            cwd=root,
            text=True,
            input=input_text,
            capture_output=True,
            check=False,
            env=env,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        raise CommitGateError(f"git {' '.join(args)} timed out") from error
    if completed.returncode:
        raise CommitGateError(completed.stderr.strip() or " ".join(("git", *args)))
    return completed.stdout


def git_bytes(
    root: Path,
    *args: str,
    input_bytes: bytes | None = None,
    timeout: float,
) -> bytes:
    try:
        # Batch commands can fill stdout while Python is still writing stdin.
        with tempfile.TemporaryFile() as source, tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as errors:
            if input_bytes is not None:
                source.write(input_bytes)
                source.seek(0)
            completed = subprocess.run(
                ["git", *args],
                cwd=root,
                stdin=source if input_bytes is not None else None,
                stdout=output,
                stderr=errors,
                check=False,
                timeout=timeout,
            )
            output.seek(0)
            stdout = output.read()
            errors.seek(0)
            stderr = errors.read()
    except subprocess.TimeoutExpired as error:
        raise CommitGateError(f"git {' '.join(args)} timed out after {timeout:g}s") from error
    if completed.returncode:
        detail = stderr.decode(errors="replace").strip()
        raise CommitGateError(detail or " ".join(("git", *args)))
    return stdout


def remaining_export_time(deadline: float) -> float:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise CommitGateError("staged tree materialization timed out")
    return remaining


def repository_local_git_environment_names(
    root: Path,
    *,
    env: dict[str, str] | None = None,
    deadline: float | None = None,
) -> tuple[str, ...]:
    timeout = remaining_export_time(deadline) if deadline is not None else None
    names = git(
        root, "rev-parse", "--local-env-vars", env=env, timeout=timeout
    ).splitlines()
    if not names or any(not name.startswith("GIT_") or not name.isidentifier() for name in names):
        raise CommitGateError("Git returned invalid repository-local environment variable names")
    return tuple(names)


def create_staged_git_metadata(
    root: Path,
    staged_worktree: Path,
    staged_index: Path,
    staged_tree: str,
    deadline: float,
) -> tuple[Path, tuple[str, ...]]:
    local_environment_names = repository_local_git_environment_names(root, deadline=deadline)
    source_environment = os.environ.copy()
    for name in local_environment_names:
        source_environment.pop(name, None)
    git(
        root,
        "clone",
        "--shared",
        "--no-checkout",
        str(root),
        str(staged_worktree),
        env=source_environment,
        timeout=remaining_export_time(deadline),
    )
    private_git_directory = staged_worktree / ".git"
    if not private_git_directory.is_dir() or private_git_directory.is_symlink():
        raise CommitGateError("could not create private staged Git metadata")
    try:
        private_environment = os.environ.copy()
        for name in local_environment_names:
            private_environment.pop(name, None)
        private_environment.update({
            "GIT_DIR": str(private_git_directory),
            "GIT_WORK_TREE": str(staged_worktree),
            "GIT_INDEX_FILE": str(private_git_directory / "index"),
        })
        git(
            staged_worktree,
            "read-tree",
            staged_tree,
            env=private_environment,
            timeout=remaining_export_time(deadline),
        )
        shutil.copyfile(private_git_directory / "index", staged_index)
    except OSError as error:
        raise CommitGateError(f"cannot install the reviewed staged tree index: {error}") from error
    remaining_export_time(deadline)
    return private_git_directory, local_environment_names


def resolve_export_path(
    path: Path,
    active_links: frozenset[str] = frozenset(),
    *,
    deadline: float,
) -> Path:
    remaining_export_time(deadline)
    if not path.is_absolute():
        raise CommitGateError("staged symlink target is not absolute after anchoring")
    current = Path(path.anchor)
    components = path.parts[1:]
    for index, component in enumerate(components):
        remaining_export_time(deadline)
        if component == "..":
            current = current.parent
            continue
        if component == ".":
            continue
        candidate = current / component
        if not candidate.is_symlink():
            current = candidate
            continue
        link_key = os.path.normcase(os.path.abspath(candidate))
        if link_key in active_links:
            raise CommitGateError(f"staged tree contains a cyclic symlink: {candidate}")
        target = candidate.readlink()
        target_path = target if target.is_absolute() else candidate.parent / target
        resolved_target = resolve_export_path(
            target_path, active_links | {link_key}, deadline=deadline
        )
        remainder = components[index + 1:]
        return resolve_export_path(
            resolved_target.joinpath(*remainder), active_links, deadline=deadline
        )
    remaining_export_time(deadline)
    return current


def validate_export_symlinks(destination: Path, *, deadline: float) -> None:
    remaining_export_time(deadline)
    root = destination.resolve()
    remaining_export_time(deadline)
    for path in root.rglob("*"):
        remaining_export_time(deadline)
        if not path.is_symlink():
            continue
        target = path.readlink()
        target_path = target if target.is_absolute() else path.parent / target
        resolved = resolve_export_path(
            target_path,
            frozenset({os.path.normcase(os.path.abspath(path))}),
            deadline=deadline,
        )
        try:
            resolved.relative_to(root)
        except ValueError as error:
            raise CommitGateError(f"staged tree contains an escaping symlink: {path.relative_to(root)}") from error
    remaining_export_time(deadline)


def staged_check_log_directory(staged_worktree: Path) -> Path:
    root = staged_worktree.resolve()
    log_directory = staged_worktree / "build" / "ai-checks"
    for path in (staged_worktree / "build", log_directory):
        if path.is_symlink():
            raise CommitGateError("staged fast-check log path traverses a symlink")
    try:
        resolved = log_directory.resolve()
        resolved.relative_to(root)
    except (OSError, RuntimeError, ValueError) as error:
        raise CommitGateError("staged fast-check log path escapes or aliases the staged tree") from error
    if log_directory.exists() and not log_directory.is_dir():
        raise CommitGateError("staged fast-check log path is not a directory")
    return log_directory


def materialize_tree(
    root: Path,
    tree: str,
    destination: Path,
    *,
    deadline: float | None = None,
) -> None:
    if deadline is None:
        deadline = time.monotonic() + STAGED_EXPORT_TIMEOUT_SECONDS
    entries = []
    listing = git_bytes(
        root, "ls-tree", "-rz", "--full-tree", tree, timeout=remaining_export_time(deadline)
    )
    for record in listing.split(b"\0"):
        remaining_export_time(deadline)
        if not record:
            continue
        metadata, separator, relative = record.partition(b"\t")
        fields = metadata.split(b" ")
        if not separator or len(fields) != 3:
            raise CommitGateError("staged tree contains an invalid entry")
        mode, kind, object_id = fields
        if mode == b"160000" or kind != b"blob":
            raise CommitGateError("staged tree contains a gitlink or non-blob entry")
        if mode not in {b"100644", b"100755", b"120000"}:
            raise CommitGateError(f"staged tree contains unsupported mode {mode.decode('ascii', errors='replace')}")
        components = relative.split(b"/")
        if (
            not relative
            or relative.startswith(b"/")
            or b"\\" in relative
            or any(component in {b"", b".", b".."} for component in components)
            or (os.name == "nt" and any(b":" in component for component in components))
        ):
            raise CommitGateError("staged tree contains an unsafe path")
        entries.append((mode, object_id, components))
        if len(entries) > MAX_STAGED_TREE_ENTRIES:
            raise CommitGateError("staged tree exceeds the file-count limit")

    object_ids = [object_id for _, object_id, _ in entries]
    if object_ids:
        sizes = git_bytes(
            root,
            "cat-file",
            "--batch-check",
            input_bytes=b"".join(object_id + b"\n" for object_id in object_ids),
            timeout=remaining_export_time(deadline),
        ).splitlines()
        if len(sizes) != len(entries):
            raise CommitGateError("could not size all staged blobs")
        total_bytes = 0
        for entry, line in zip(entries, sizes):
            remaining_export_time(deadline)
            fields = line.split()
            if len(fields) != 3 or fields[0] != entry[1] or fields[1] != b"blob":
                raise CommitGateError("staged tree references an unavailable or unsupported blob")
            try:
                blob_size = int(fields[2])
            except ValueError as error:
                raise CommitGateError("Git returned an invalid staged blob size") from error
            if blob_size < 0:
                raise CommitGateError("Git returned an invalid staged blob size")
            total_bytes += blob_size
            if total_bytes > MAX_STAGED_TREE_BYTES:
                raise CommitGateError("staged tree exceeds the byte limit")

        packed = git_bytes(
            root,
            "cat-file",
            "--batch",
            input_bytes=b"".join(object_id + b"\n" for object_id in object_ids),
            timeout=remaining_export_time(deadline),
        )
    else:
        packed = b""

    offset = 0
    for (mode, object_id, components), size_line in zip(entries, sizes if object_ids else []):
        remaining_export_time(deadline)
        header_end = packed.find(b"\n", offset)
        if header_end < 0:
            raise CommitGateError("Git returned a truncated staged blob header")
        header = packed[offset:header_end].split()
        offset = header_end + 1
        if len(header) != 3 or header[0] != object_id or header[1] != b"blob":
            raise CommitGateError("Git returned an invalid staged blob")
        try:
            size = int(header[2])
        except ValueError as error:
            raise CommitGateError("Git returned an invalid staged blob size") from error
        if size != int(size_line.split()[2]) or offset + size >= len(packed):
            raise CommitGateError("Git returned a truncated staged blob")
        content = packed[offset:offset + size]
        offset += size
        if packed[offset:offset + 1] != b"\n":
            raise CommitGateError("Git returned an invalid staged blob delimiter")
        offset += 1

        target = destination
        for component in components[:-1]:
            target = target / os.fsdecode(component)
            if target.exists() or target.is_symlink():
                if target.is_symlink() or not target.is_dir():
                    raise CommitGateError("staged tree has colliding paths")
            else:
                target.mkdir()
        target = target / os.fsdecode(components[-1])
        if target.exists() or target.is_symlink():
            raise CommitGateError("staged tree has colliding paths")
        if mode == b"120000":
            if len(content) > 4096:
                raise CommitGateError("staged symlink target exceeds the supported limit")
            try:
                os.symlink(os.fsdecode(content), target)
            except (NotImplementedError, OSError, ValueError) as error:
                raise CommitGateError(f"cannot materialize staged symlink {target.name}: {error}") from error
        else:
            try:
                target.write_bytes(content)
                target.chmod(0o755 if mode == b"100755" else 0o644)
            except OSError as error:
                raise CommitGateError(f"cannot materialize staged file {target.name}: {error}") from error
    if offset != len(packed):
        raise CommitGateError("Git returned unexpected bytes after staged blobs")
    validate_export_symlinks(destination, deadline=deadline)


def staged_script_module_names(script_directory: Path) -> set[str]:
    return {path.stem for path in script_directory.glob("*.py")}


def module_is_under(module: Any, directory: Path) -> bool:
    filename = getattr(module, "__file__", None)
    if not isinstance(filename, str):
        return False
    try:
        Path(filename).resolve().relative_to(directory.resolve())
    except (OSError, ValueError):
        return False
    return True


def run_staged_ai_check(
    staged_worktree: Path,
    environment: dict[str, str],
    local_environment_names: Sequence[str],
) -> int:
    staged_script = staged_worktree / "script"
    checker_path = staged_script / "ai_check.py"
    original_script = repository_root() / "script"
    original_cwd = Path.cwd()
    original_environment = {name: os.environ.get(name) for name in local_environment_names}
    original_sys_path = sys.path[:]
    displaced_modules: dict[str, Any] = {}
    module_name = f"_ai_commit_staged_check_{os.getpid()}_{time.time_ns()}"
    staged_names = staged_script_module_names(staged_script)
    try:
        for name in staged_names:
            existing = sys.modules.get(name)
            if existing is not None and module_is_under(existing, original_script):
                displaced_modules[name] = sys.modules.pop(name)
        for name in local_environment_names:
            if name in environment:
                os.environ[name] = environment[name]
            else:
                os.environ.pop(name, None)
        os.chdir(staged_worktree)
        sys.path.insert(0, str(staged_script))

        spec = importlib.util.spec_from_file_location(module_name, checker_path)
        if spec is None or spec.loader is None:
            raise CommitGateError("cannot load staged script/ai_check.py")
        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module
        try:
            spec.loader.exec_module(module)
        except SystemExit as error:
            raise CommitGateError(
                "staged script/ai_check.py exited during import before main(argv)"
            ) from error
        entrypoint = getattr(module, "main", None)
        if not callable(entrypoint):
            raise CommitGateError("staged script/ai_check.py must define main(argv)")
        try:
            result = entrypoint([
                "--root", str(staged_worktree), "--fast", "--require-index-match"
            ])
        except SystemExit as error:
            result = error.code
        if result is None:
            return 0
        if isinstance(result, int):
            return result
        if result:
            print(str(result), file=sys.stderr)
        return 1
    finally:
        try:
            os.chdir(original_cwd)
        finally:
            try:
                for name, value in original_environment.items():
                    if value is None:
                        os.environ.pop(name, None)
                    else:
                        os.environ[name] = value
            finally:
                sys.path[:] = original_sys_path
                for name, module in tuple(sys.modules.items()):
                    if name == module_name or (name in staged_names and module_is_under(module, staged_script)):
                        sys.modules.pop(name, None)
                sys.modules.update(displaced_modules)


def run_staged_fast_check(root: Path, candidate: dict[str, str]) -> int:
    try:
        git_directory = Path(git(root, "rev-parse", "--absolute-git-dir").strip())
        with tempfile.TemporaryDirectory(prefix="ai-staged-fast-") as temporary:
            temporary_root = Path(temporary)
            staged_worktree = temporary_root / "worktree"
            staged_index = temporary_root / "index"
            export_deadline = time.monotonic() + STAGED_EXPORT_TIMEOUT_SECONDS
            private_git_directory, local_environment_names = create_staged_git_metadata(
                root, staged_worktree, staged_index, candidate["tree"], export_deadline
            )
            environment = os.environ.copy()
            for name in local_environment_names:
                environment.pop(name, None)
            environment.update({
                "GIT_DIR": str(private_git_directory),
                "GIT_WORK_TREE": str(staged_worktree),
                "GIT_INDEX_FILE": str(staged_index),
            })
            materialize_tree(root, candidate["tree"], staged_worktree, deadline=export_deadline)
            staged_check_log_directory(staged_worktree)
            exported_tree = git(
                staged_worktree,
                "write-tree",
                env=environment,
                timeout=remaining_export_time(export_deadline),
            ).strip()
            if exported_tree != candidate["tree"]:
                raise CommitGateError("temporary index does not match the reviewed staged tree")
            try:
                returncode = run_staged_ai_check(
                    staged_worktree, environment, local_environment_names
                )
            except BaseException:
                preserve_staged_check_logs(git_directory, staged_worktree, candidate)
                raise
            if returncode:
                preserve_staged_check_logs(git_directory, staged_worktree, candidate)
            return returncode
    except Exception as error:
        print(f"cannot validate exact staged source: {error}", file=sys.stderr)
        return 1


def preserve_staged_check_logs(
    git_directory: Path,
    staged_worktree: Path,
    candidate: dict[str, str],
) -> Path | None:
    try:
        source = staged_check_log_directory(staged_worktree)
    except (CommitGateError, OSError, RuntimeError) as error:
        print(f"staged fast-check logs not retained: {error}", file=sys.stderr)
        return None
    if not source.is_dir():
        return None
    destination_parent = git_directory / "ai-staged-fast-failures"
    try:
        if destination_parent.is_symlink():
            raise OSError("failure-log destination is a symlink")
        destination_parent.mkdir(parents=True, exist_ok=True)
        if destination_parent.is_symlink():
            raise OSError("failure-log destination is a symlink")
        destination_parent.resolve().relative_to(git_directory.resolve())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"could not prepare staged fast-check log retention: {error}", file=sys.stderr)
        return None
    destination = destination_parent / f"{candidate['task_id']}-{candidate['tree'][:12]}-{time.time_ns()}"
    try:
        destination.mkdir()
    except OSError as error:
        print(f"could not prepare staged fast-check log retention: {error}", file=sys.stderr)
        return None

    deadline = time.monotonic() + STAGED_FAILURE_LOG_COPY_TIMEOUT_SECONDS
    copied_files: list[dict[str, object]] = []
    copied_bytes = 0
    omitted_log_count = 0
    scanned_entries = 0
    incomplete = False
    unscanned_entries = False
    reasons: set[str] = set()
    try:
        with os.scandir(source) as entries:
            for entry in entries:
                if time.monotonic() >= deadline:
                    reasons.add("deadline")
                    incomplete = True
                    unscanned_entries = True
                    break
                scanned_entries += 1
                if scanned_entries > STAGED_FAILURE_LOG_MAX_ENTRIES:
                    reasons.add("entry_count_limit")
                    incomplete = True
                    unscanned_entries = True
                    break
                if entry.is_symlink():
                    if entry.name.endswith(".log"):
                        omitted_log_count += 1
                        reasons.add("symlink_log")
                        incomplete = True
                    continue
                if not entry.name.endswith(".log") or not entry.is_file(follow_symlinks=False):
                    continue
                if len(copied_files) >= STAGED_FAILURE_LOG_MAX_FILES:
                    omitted_log_count += 1
                    reasons.add("file_count_limit")
                    incomplete = True
                    unscanned_entries = True
                    break
                remaining_bytes = STAGED_FAILURE_LOG_MAX_BYTES - copied_bytes
                if remaining_bytes <= 0:
                    omitted_log_count += 1
                    reasons.add("byte_limit")
                    incomplete = True
                    unscanned_entries = True
                    break
                target = destination / entry.name
                file_bytes = 0
                truncated = False
                try:
                    source_size = entry.stat(follow_symlinks=False).st_size
                    with open(entry.path, "rb") as input_stream, target.open("wb") as output_stream:
                        while file_bytes < remaining_bytes:
                            if time.monotonic() >= deadline:
                                reasons.add("deadline")
                                incomplete = True
                                truncated = True
                                unscanned_entries = True
                                break
                            chunk = input_stream.read(min(
                                STAGED_FAILURE_LOG_CHUNK_BYTES,
                                remaining_bytes - file_bytes,
                            ))
                            if not chunk:
                                break
                            output_stream.write(chunk)
                            file_bytes += len(chunk)
                            copied_bytes += len(chunk)
                    truncated = truncated or file_bytes < source_size
                    if truncated:
                        reasons.add("byte_limit" if time.monotonic() < deadline else "deadline")
                        incomplete = True
                        unscanned_entries = True
                    copied_files.append({
                        "path": entry.name,
                        "bytes": file_bytes,
                        "truncated": truncated,
                    })
                except OSError:
                    omitted_log_count += 1
                    reasons.add("read_error")
                    incomplete = True
                if unscanned_entries:
                    break
    except OSError as error:
        reasons.add("scan_error")
        incomplete = True
        print(f"could not scan staged fast-check logs completely: {error}", file=sys.stderr)

    manifest = {
        "schema_version": 1,
        "complete": not incomplete,
        "limits": {
            "copy_timeout_seconds": STAGED_FAILURE_LOG_COPY_TIMEOUT_SECONDS,
            "max_bytes": STAGED_FAILURE_LOG_MAX_BYTES,
            "max_files": STAGED_FAILURE_LOG_MAX_FILES,
            "max_entries_scanned": STAGED_FAILURE_LOG_MAX_ENTRIES,
        },
        "files": copied_files,
        "truncated_files": [entry["path"] for entry in copied_files if entry["truncated"]],
        "bytes_copied": copied_bytes,
        "omitted_log_count": omitted_log_count,
        "unscanned_entries": unscanned_entries,
        "truncation_reasons": sorted(reasons),
        "non_log_files": "omitted by design; only regular .log files are retained",
    }
    try:
        (destination / "retention-manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    except OSError as error:
        print(f"could not write staged fast-check retention manifest: {error}", file=sys.stderr)
        return destination
    print(f"staged fast-check logs retained at {destination}", file=sys.stderr)
    if incomplete:
        print(
            f"staged fast-check log retention partial: reasons={','.join(sorted(reasons)) or 'unknown'}; "
            f"see {destination / 'retention-manifest.json'}",
            file=sys.stderr,
        )
    return destination


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
    if level == "basic" and (task.get("status") != "done" or task.get("finalized") is not True):
        raise CommitGateError(f"{task_id} is basic; staged metadata must set status to done and finalized to true")
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


def receipt_path(root: Path, candidate: dict[str, str], identity: str) -> Path:
    if identity not in LEGACY_REVIEW_MODELS and identity not in {LUNA_MODEL, SOL_MODEL}:
        raise CommitGateError(f"unsupported review receipt identity: {identity}")
    key = "-".join((candidate["head"], candidate["tree"], candidate["baseline_commit"], candidate["merge_head"], identity))
    return reviews_directory(root) / candidate["task_id"] / f"{key}.json"


def validate_report(report: Any, candidate: dict[str, str]) -> dict[str, Any]:
    if not isinstance(report, dict):
        raise CommitGateError("review report must be a JSON object")
    for field, value in candidate.items():
        if report.get(field) != value:
            raise CommitGateError(f"review report {field} does not match the staged candidate")
    model = report.get("model")
    if not isinstance(model, str) or model not in MODEL_EFFORTS:
        raise CommitGateError(f"review report model must be one of: {', '.join(MODEL_EFFORTS)}")
    role = report.get("review_role")
    if "review_role" not in report:
        role = {LUNA_MODEL: "task", SOL_MODEL: "aggregate"}.get(model)
    if not isinstance(role, str) or role not in LEGACY_REVIEW_MODELS:
        raise CommitGateError("review report review_role must be task or aggregate; new models require an explicit role")
    effort = report.get("effort")
    if not isinstance(effort, str) or effort not in MODEL_EFFORTS[model]:
        raise CommitGateError(f"review report effort for {model} must be one of: {', '.join(sorted(MODEL_EFFORTS[model]))}")
    if report.get("verdict") != "approved":
        raise CommitGateError("review report verdict must be approved")
    if not isinstance(report.get("evidence"), str) or not report["evidence"].strip():
        raise CommitGateError("review report evidence must be non-empty")
    return report | {"review_role": role}


def selected_task_id(root: Path) -> str:
    selected = os.environ.get("AI_TASK")
    if selected:
        return selected
    current = reviews_directory(root) / "current.json"
    try:
        payload = json.loads(current.read_text(encoding="utf-8"))
    except OSError as error:
        raise CommitGateError("AI_TASK is not set and no task review has selected a task") from error
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
    destination = receipt_path(root, candidate, validated["review_role"])
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(validated, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if validated["review_role"] == "task":
        selector = {"task_id": task_id}
        (reviews_directory(root) / "current.json").write_text(
            json.dumps(selector, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    return candidate


def read_receipt(root: Path, candidate: dict[str, str], role: str) -> None:
    target = receipt_path(root, candidate, role)
    legacy_model = LEGACY_REVIEW_MODELS[role]
    legacy = not target.exists()
    if legacy:
        target = receipt_path(root, candidate, legacy_model)
    try:
        report = json.loads(target.read_text(encoding="utf-8"))
    except OSError as error:
        raise CommitGateError(f"missing {role} review receipt for current staged candidate") from error
    except json.JSONDecodeError as error:
        raise CommitGateError(f"invalid local receipt {target}: {error}") from error
    validated = validate_report(report, candidate)
    if validated["review_role"] != role:
        raise CommitGateError(f"local receipt {target} has the wrong review_role")
    if legacy and validated["model"] != legacy_model:
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
    read_receipt(root, candidate, "task")
    if candidate["level"] in {"minor", "major"}:
        read_receipt(root, candidate, "aggregate")
    if message_path is not None:
        check_message(root, message_path, task_id)
    return candidate


def precommit(root: Path) -> int:
    candidate = check(root)
    if candidate["level"] == "basic":
        return run_staged_fast_check(root, candidate)
    check_args = [sys.executable, str(root / "script" / "ai_check.py")]
    check_args.extend(("--candidate", "--level", candidate["level"]))
    branch = git(root, "branch", "--show-current").strip()
    if candidate["merge_head"] and branch == "ai-main":
        check_args.extend(("--snapshot-source-commit", candidate["merge_head"]))
    elif candidate["level"] == "snapshot" and branch == "ai-dev":
        resolved = subprocess.run(
            [
                sys.executable, str(root / "script" / "ai_publish.py"),
                "--root", str(root), "resolve-tagged-source",
                "--expected-head", candidate["head"], "--task-id", candidate["task_id"],
            ],
            cwd=root, text=True, capture_output=True, check=False,
        )
        if resolved.returncode:
            print(resolved.stderr.strip() or "cannot resolve the tagged snapshot source", file=sys.stderr)
            return resolved.returncode
        source = resolved.stdout.strip()
        if source:
            check_args.extend((
                "--snapshot-source-commit", source,
                "--snapshot-task-id", candidate["task_id"],
            ))
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
