#!/usr/bin/env python3
"""Run clang-tidy against Android arm64 translation units from Gradle's database."""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


ANDROID_ARM64_TARGET = re.compile(r"^aarch64(?:-[a-z0-9]+)?-linux-android(?:[0-9]+)?$")
CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}


class AndroidClangTidyError(ValueError):
    """A missing, malformed, or non-Android compile database."""


def _uses_response_file(executable: str, *, platform_name: str = os.name) -> bool:
    if platform_name != "nt":
        return False
    resolved = shutil.which(executable) or executable
    return Path(resolved).suffix.lower() in {".bat", ".cmd"}


def _run_clang_tidy(
    command: list[str], root: Path, *, platform_name: str = os.name
) -> subprocess.CompletedProcess[bytes]:
    if not _uses_response_file(command[0], platform_name=platform_name):
        return subprocess.run(command, cwd=root, check=False)

    with tempfile.TemporaryDirectory(prefix=".clang-tidy-args-", dir=root) as directory:
        response_file = Path(directory) / "arguments.rsp"
        response_file.write_text(
            subprocess.list2cmdline(command[1:]) + "\n",
            encoding="utf-8",
            newline="\n",
        )
        relative_response_file = response_file.relative_to(root)
        return subprocess.run(
            [command[0], f"@{relative_response_file}"],
            cwd=root,
            check=False,
            shell=os.name == "nt",
        )


def _compile_databases(directory: Path) -> list[Path]:
    if not directory.is_dir():
        raise AndroidClangTidyError(f"compile database directory does not exist: {directory}")
    candidates = sorted(directory.rglob("compile_commands.json"))
    if not candidates:
        raise AndroidClangTidyError(f"no compile_commands.json found under {directory}")
    return [candidate.resolve() for candidate in candidates]


def _tokens(entry: Any, index: int) -> tuple[Path, list[str]]:
    if not isinstance(entry, dict):
        raise AndroidClangTidyError(f"compile command {index} must be an object")
    directory = entry.get("directory")
    source = entry.get("file")
    if not isinstance(directory, str) or not directory or not isinstance(source, str) or not source:
        raise AndroidClangTidyError(f"compile command {index} is missing its directory or source file")
    source_path = Path(source)
    if not source_path.is_absolute():
        source_path = Path(directory) / source_path

    arguments = entry.get("arguments")
    if arguments is not None:
        if not isinstance(arguments, list) or not arguments or any(not isinstance(item, str) for item in arguments):
            raise AndroidClangTidyError(f"compile command {index}.arguments is malformed")
        tokens = arguments
    else:
        command = entry.get("command")
        if not isinstance(command, str) or not command.strip():
            raise AndroidClangTidyError(f"compile command {index} has no arguments or command")
        try:
            tokens = shlex.split(command, posix=os.name != "nt")
        except ValueError as error:
            raise AndroidClangTidyError(f"compile command {index} cannot be parsed: {error}") from error
    return source_path.resolve(), tokens


def _has_android_arm64_target(tokens: list[str]) -> bool:
    for index, token in enumerate(tokens):
        target: str | None = None
        if token in {"--target", "-target"} and index + 1 < len(tokens):
            target = tokens[index + 1]
        elif token.startswith("--target=") or token.startswith("-target="):
            target = token.split("=", 1)[1]
        if target and ANDROID_ARM64_TARGET.fullmatch(target):
            return True
    return False


def _project_translation_units(database: Path, repository_root: Path) -> list[Path]:
    try:
        entries = json.loads(database.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise AndroidClangTidyError(f"cannot read compile database {database}: {error}") from error
    if not isinstance(entries, list):
        raise AndroidClangTidyError("compile_commands.json must contain a list")

    root = repository_root.resolve()
    sources: set[Path] = set()
    for index, entry in enumerate(entries):
        source, tokens = _tokens(entry, index)
        try:
            relative = source.relative_to(root)
        except ValueError:
            continue
        if relative.parts[0] not in {"src", "tests"} or source.suffix.lower() not in CPP_SUFFIXES:
            continue
        if not _has_android_arm64_target(tokens):
            raise AndroidClangTidyError(
                f"project translation unit is missing an Android target (arm64 required): {source}"
            )
        sources.add(source)
    if not sources:
        raise AndroidClangTidyError("Android compile database has no project translation units under src/ or tests/")
    return sorted(sources)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compile-commands-dir", type=Path, required=True)
    parser.add_argument("--repository-root", type=Path, default=Path.cwd())
    parser.add_argument("--clang-tidy", default="clang-tidy")
    parser.add_argument("--checks", default="clang-analyzer-*")
    parser.add_argument("--warnings-as-errors", default="clang-analyzer-*")
    args = parser.parse_args(argv)

    try:
        databases = _compile_databases(args.compile_commands_dir.resolve())
        analyses = [
            (database, _project_translation_units(database, args.repository_root))
            for database in databases
        ]
    except AndroidClangTidyError as error:
        print(f"Android clang-tidy configuration error: {error}", file=sys.stderr)
        return 2

    root = args.repository_root.resolve()
    header_filter = f"^{re.escape(str(root))}/(?:src|tests)/"
    analyzed_translation_units = 0
    for database, sources in analyses:
        command = [
            args.clang_tidy,
            f"--checks={args.checks}",
            f"--warnings-as-errors={args.warnings_as_errors}",
            f"--header-filter={header_filter}",
            "-p",
            str(database.parent),
            *(str(source) for source in sources),
        ]
        try:
            completed = _run_clang_tidy(command, root)
        except OSError as error:
            print(f"Android clang-tidy invocation failed: {error}", file=sys.stderr)
            return 2
        if completed.returncode:
            return completed.returncode
        analyzed_translation_units += len(sources)
    print(
        f"Android clang-tidy analyzed {analyzed_translation_units} arm64 translation units "
        f"across {len(analyses)} compile databases."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
