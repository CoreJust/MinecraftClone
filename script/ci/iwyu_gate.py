#!/usr/bin/env python3
"""Run one IWYU translation unit and fail on missing owned public includes."""

from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
RESULT_MARKER = "IWYU_GATE_RESULT "
COMMAND_MARKER = "IWYU_GATE_COMMAND "
INCLUDE_LINE = re.compile(r'^#include\s+([<"])([^>"\n]+)([>"])(?:\s*(?://.*)?)$')
ADD_REMOVE_HEADER = re.compile(r"^(.+?) should (add|remove) these lines:$")
FULL_LIST_HEADER = re.compile(r"^The full include-list for (.+):$")
CLEAN_HEADER = re.compile(r"^\((.+) has correct #includes/fwd-decls\)$")
FMT_API = re.compile(r"\bfmt::(?:format|format_to|format_to_n|print|vformat|runtime|memory_buffer)\b")


class GateError(ValueError):
    """Invalid invocation or unrecognized/incomplete IWYU output."""


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _source_argument(arguments: list[str], cwd: Path) -> Path:
    sources: list[Path] = []
    for index, argument in enumerate(arguments[:-1]):
        if argument == "-c":
            candidate = Path(arguments[index + 1])
            if not candidate.is_absolute():
                candidate = cwd / candidate
            sources.append(candidate.resolve())
    if len(sources) != 1:
        raise GateError("compiler arguments must contain exactly one '-c <source>' pair")
    return sources[0]


def _repo_relative(file_path: Path, root: Path) -> str | None:
    try:
        relative = file_path.resolve().relative_to(root.resolve())
    except ValueError:
        return None
    return relative.as_posix()


def _is_owned(relative: str | None) -> bool:
    return bool(relative and relative.split("/", 1)[0] in {"src", "tests"})


def _report_path(value: str, cwd: Path) -> Path:
    path = Path(value)
    return (path if path.is_absolute() else cwd / path).resolve()


def _include(line: str) -> tuple[str, str] | None:
    match = INCLUDE_LINE.fullmatch(line.strip())
    if not match:
        return None
    delimiter, name, closing = match.groups()
    if closing != (">" if delimiter == "<" else '"'):
        return None
    return ("angle" if delimiter == "<" else "quote", name)


def _is_forward_declaration(line: str) -> bool:
    value = line.strip()
    return value in {"{", "}", "};"} or value.startswith(
        ("namespace ", "class ", "struct ", "enum ", "union ", "template ", "typedef ", "using ")
    )


def parse_report(output: bytes, expected_source: Path, cwd: Path) -> dict[str, Any]:
    """Parse the whole documented IWYU text report, rejecting unknown output."""

    try:
        text = output.decode("utf-8")
    except UnicodeDecodeError as error:
        raise GateError("IWYU report is not UTF-8") from error

    sections: dict[Path, dict[str, Any]] = {}
    current_path: Path | None = None
    current_kind: str | None = None
    recognized = False

    def get_section(path: Path) -> dict[str, Any]:
        return sections.setdefault(
            path,
            {"add": None, "remove": None, "full": None, "clean": False},
        )

    for line_number, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line == "---":
            continue

        match = ADD_REMOVE_HEADER.fullmatch(line)
        if match:
            path = _report_path(match.group(1), cwd)
            kind = match.group(2)
            section = get_section(path)
            if section[kind] is not None or section["clean"]:
                raise GateError(f"duplicate or conflicting {kind} section at line {line_number}")
            section[kind] = []
            current_path, current_kind = path, kind
            recognized = True
            continue

        match = FULL_LIST_HEADER.fullmatch(line)
        if match:
            path = _report_path(match.group(1), cwd)
            section = get_section(path)
            if section["full"] is not None or section["clean"]:
                raise GateError(f"duplicate or conflicting full include-list at line {line_number}")
            section["full"] = []
            current_path, current_kind = path, "full"
            recognized = True
            continue

        match = CLEAN_HEADER.fullmatch(line)
        if match:
            path = _report_path(match.group(1), cwd)
            section = get_section(path)
            if section["clean"] or any(section[key] is not None for key in ("add", "remove", "full")):
                raise GateError(f"duplicate or conflicting clean marker at line {line_number}")
            section["clean"] = True
            current_path, current_kind = None, None
            recognized = True
            continue

        if current_path is None or current_kind is None:
            raise GateError(f"unrecognized IWYU output at line {line_number}: {line}")

        if current_kind == "remove" and line.startswith("- "):
            line = line[2:].strip()
        if current_kind == "full":
            if _include(line) is None and not _is_forward_declaration(line):
                raise GateError(f"malformed full include-list entry at line {line_number}: {line}")
        elif _include(line) is None and not _is_forward_declaration(line):
            raise GateError(f"malformed IWYU suggestion at line {line_number}: {line}")
        sections[current_path][current_kind].append(line)

    if not recognized or not sections:
        raise GateError("IWYU report is empty or contains no recognized report sections")

    for path, section in sections.items():
        if section["clean"]:
            continue
        if section["add"] is None or section["remove"] is None:
            raise GateError(f"incomplete IWYU report for {path}: expected add and remove sections")

    if expected_source.resolve() not in sections:
        raise GateError(f"IWYU report does not include the invoked translation unit: {expected_source}")

    return {str(path): value for path, value in sorted(sections.items(), key=lambda item: str(item[0]))}


def _current_includes(owner: Path) -> set[tuple[str, str]]:
    try:
        contents = owner.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return set()
    return {include for line in contents.splitlines() if (include := _include(line)) is not None}


def _glm_facade(name: str) -> str | None:
    match = re.fullmatch(r"glm/detail/type_vec(\d+)\.(?:hpp|inl)", name)
    if match:
        return f"glm/vec{match.group(1)}.hpp"
    match = re.fullmatch(r"glm/ext/vector_[A-Za-z0-9_]*(\d)\.hpp", name)
    if match:
        return f"glm/vec{match.group(1)}.hpp"
    match = re.fullmatch(r"glm/detail/type_mat(\d)x(\d)\.(?:hpp|inl)", name)
    if match:
        return f"glm/mat{match.group(1)}x{match.group(2)}.hpp"
    match = re.fullmatch(r"glm/ext/matrix_[A-Za-z0-9_]*(\d)x(\d)\.hpp", name)
    if match:
        return f"glm/mat{match.group(1)}x{match.group(2)}.hpp"
    if name == "glm/detail/func_common.inl":
        return "glm/common.hpp"
    if name == "glm/detail/func_geometric.inl":
        return "glm/geometric.hpp"
    if name == "glm/ext/matrix_transform.inl":
        return "glm/gtc/matrix_transform.hpp"
    if name == "glm/ext/matrix_clip_space.inl":
        return "glm/ext/matrix_clip_space.hpp"
    return None


def _quoted_project_include(name: str, owner: Path, root: Path) -> str | None:
    source_root = root / "src"
    public_matches: dict[Path, str] = {}
    candidates = [owner.parent / name, root / name]
    include_roots = [source_root / "include", *source_root.glob("*/include")]
    candidates.extend(include_root / name for include_root in include_roots if include_root.is_dir())

    def record_public_match(candidate: Path) -> None:
        if not candidate.is_file():
            return
        resolved = candidate.resolve()
        for parent in resolved.parents:
            if parent.name == "include":
                relative = resolved.relative_to(parent).as_posix()
                public_matches[resolved] = relative
                return

    for candidate in candidates:
        record_public_match(candidate)

    # IWYU can report a short include that is found through a nested include
    # directory, while the stable public path retains its module prefix.
    for include_root in include_roots:
        if not include_root.is_dir():
            continue
        for match in include_root.rglob(Path(name).name):
            relative = match.relative_to(include_root).as_posix()
            if match.is_file() and relative.endswith("/" + name):
                public_matches[match.resolve()] = relative
    if len(public_matches) == 1:
        return f"<{next(iter(public_matches.values()))}>"
    return None


def _enet_facade(name: str, compiler_arguments: list[str], cwd: Path) -> str | None:
    if name not in {"enet/protocol.h", "enet/unix.h"}:
        return None

    include_directories: list[Path] = []
    index = 0
    while index < len(compiler_arguments):
        argument = compiler_arguments[index]
        if argument in {"-I", "-isystem", "-iquote"} and index + 1 < len(compiler_arguments):
            index += 1
            include_path = Path(compiler_arguments[index])
        elif argument.startswith(("-I", "-isystem", "-iquote")):
            option = "-isystem" if argument.startswith("-isystem") else (
                "-iquote" if argument.startswith("-iquote") else "-I"
            )
            include_path = Path(argument[len(option) :])
        else:
            index += 1
            continue
        include_directories.append(
            (include_path if include_path.is_absolute() else cwd / include_path).resolve()
        )
        index += 1

    for include_directory in include_directories:
        private_header = include_directory / name
        public_header = include_directory / "enet/enet.h"
        if private_header.is_file() and public_header.is_file():
            return "enet/enet.h"
    return None


def _private_kind(name: str) -> str | None:
    parts = name.split("/")
    basename = parts[-1]
    project_namespaces = {"acceptance", "android", "client", "core", "server", "shared", "testsupport"}
    if parts[0] not in project_namespaces and basename.startswith(("_", "__")):
        return "platform or standard-library private header"
    if parts[0] not in project_namespaces and any(
        part in {"detail", "internal", "private", "impl", "bits"} for part in parts
    ):
        return "implementation-detail header"
    return None


def _has_fmt_api(text: str) -> bool:
    without_comments = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.DOTALL)
    return bool(FMT_API.search(without_comments))


def classify_additions(
    sections: dict[str, Any], root: Path, compiler_arguments: list[str] | None = None, cwd: Path | None = None
) -> dict[str, list[str]]:
    result: dict[str, list[str]] = {
        "public_additions": [],
        "removals": [],
        "forward_declarations": [],
        "mapped_facades": [],
        "unsupported_suggestions": [],
    }

    for raw_owner, section in sections.items():
        owner = Path(raw_owner)
        relative = _repo_relative(owner, root)
        owner_label = relative or raw_owner
        owned = _is_owned(relative)
        existing = _current_includes(owner)
        owner_text = owner.read_text(encoding="utf-8", errors="replace") if owner.is_file() else ""

        for suggestion in section["remove"] or []:
            if _include(suggestion) is None:
                result["forward_declarations"].append(f"{owner_label}: {suggestion}")
            else:
                result["removals"].append(f"{owner_label}: {suggestion}")
        for suggestion in section["add"] or []:
            include = _include(suggestion)
            if include is None:
                result["forward_declarations"].append(f"{owner_label}: {suggestion}")
                continue
            delimiter, name = include
            if not owned:
                result["unsupported_suggestions"].append(
                    f"{owner_label}: external addition {suggestion}"
                )
                continue

            facade = _glm_facade(name) or _enet_facade(
                name, compiler_arguments or [], cwd or Path.cwd()
            )
            if name == "fmt/base.h":
                if _has_fmt_api(owner_text):
                    facade = "fmt/format.h"
                else:
                    result["unsupported_suggestions"].append(
                        f"{owner_label}: {suggestion} (known fmt private header; no fmt API facade proof)"
                    )
                    continue
            elif name == "base.h" and _has_fmt_api(owner_text):
                facade = "fmt/format.h"
            if facade is not None:
                public_include = ("angle", facade)
                if public_include in existing:
                    result["mapped_facades"].append(
                        f"{owner_label}: {suggestion} -> <{facade}> (already included)"
                    )
                else:
                    result["public_additions"].append(
                        f"{owner_label}: missing facade <{facade}> for {suggestion}"
                    )
                continue

            private_reason = _private_kind(name)
            if private_reason:
                result["unsupported_suggestions"].append(
                    f"{owner_label}: {suggestion} ({private_reason}; do not add directly)"
                )
                continue

            if delimiter == "quote":
                facade = _quoted_project_include(name, owner, root)
                if facade:
                    public_include = ("angle", facade[1:-1])
                    if public_include in existing:
                        result["mapped_facades"].append(
                            f"{owner_label}: {suggestion} -> {facade} (already included)"
                        )
                    else:
                        result["public_additions"].append(
                            f"{owner_label}: use public include {facade} instead of {suggestion}"
                        )
                else:
                    result["public_additions"].append(
                        f"{owner_label}: unmapped quoted addition {suggestion}"
                    )
            else:
                result["public_additions"].append(f"{owner_label}: {suggestion}")

        for suggestion in section["full"] or []:
            if _include(suggestion) is None and not _is_forward_declaration(suggestion):
                raise GateError(f"malformed full include-list entry for {owner_label}")

    for entries in result.values():
        entries.sort()
    return result


def _project_input_sha256(root: Path) -> str:
    script_directory = str(root / "script")
    if script_directory not in sys.path:
        sys.path.insert(0, script_directory)
    try:
        from ai_analysis_matrix import project_analysis_input_sha256

        return project_analysis_input_sha256(root)
    except (ImportError, OSError, ValueError) as error:
        raise GateError(f"cannot fingerprint project analysis inputs: {error}") from error


def _atomic_write(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(content)
    temporary.replace(path)


def _receipt_paths(source: str, source_argv: list[str], root: Path) -> tuple[Path, Path, str, str]:
    identity = source.encode("utf-8") + b"\0" + json.dumps(
        source_argv, separators=(",", ":")
    ).encode("utf-8")
    stem = hashlib.sha256(identity).hexdigest()
    directory = root / "build/analysis-iwyu/gate"
    log_path = directory / f"{stem}.log"
    record_path = directory / f"{stem}.json"
    return log_path, record_path, log_path.relative_to(root).as_posix(), record_path.relative_to(root).as_posix()


def _print_result(record: dict[str, Any]) -> None:
    print(RESULT_MARKER + json.dumps(record, sort_keys=True, separators=(",", ":")))


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    raw_tool_argv: list[str] = []
    source_argv: list[str] = []
    source_path: Path | None = None
    source_relative: str | None = None
    root = REPOSITORY_ROOT.resolve()
    cwd = Path.cwd().resolve()
    record: dict[str, Any] = {
        "schema_version": 1,
        "source": None,
        "source_sha256": None,
        "project_input_sha256": None,
        "adapter_argv": [],
        "source_argv": [],
        "invoked_tool": "include-what-you-use",
        "tool_argv": [],
        "tool_exit_code": None,
        "report_sha256": _sha256(b""),
        "status": "error",
        "public_additions": [],
        "removals": [],
        "forward_declarations": [],
        "mapped_facades": [],
        "unsupported_suggestions": [],
        "log": None,
        "record": None,
        "diagnostic": None,
    }
    raw_output = b""
    exit_code = 2
    log_path: Path | None = None
    record_path: Path | None = None

    try:
        if not argv or argv[0] != "--":
            raise GateError("expected '-- <IWYU command> -- <compiler arguments>'")
        command_tokens = argv[1:]
        if not command_tokens:
            raise GateError("missing IWYU command")
        try:
            separator = command_tokens.index("--")
        except ValueError as error:
            raise GateError("missing IWYU/compiler argument separator '--'") from error
        if separator == 0:
            raise GateError("missing IWYU executable")
        adapter_options = command_tokens[1:separator]
        if adapter_options[-2:] != ["-Xiwyu", "--error"]:
            raise GateError("CMake IWYU adapter must end with '-Xiwyu --error' before compiler arguments")
        tool = command_tokens[0]
        source_argv = command_tokens[separator + 1 :]
        if not source_argv:
            raise GateError("missing compiler arguments after IWYU separator")
        raw_tool_argv = [tool, *adapter_options[:-2], *source_argv]
        record["adapter_argv"] = command_tokens
        record["source_argv"] = source_argv
        record["tool_argv"] = raw_tool_argv
        source_path = _source_argument(source_argv, cwd)
        source_relative = _repo_relative(source_path, root)
        if not _is_owned(source_relative):
            raise GateError(f"translation unit is outside owned src/ or tests/: {source_path}")
        if not source_path.is_file():
            raise GateError(f"translation unit does not exist: {source_path}")
        record["source"] = source_relative
        record["source_sha256"] = _sha256(source_path.read_bytes())
        record["project_input_sha256"] = _project_input_sha256(root)
        log_path, record_path, log_relative, record_relative = _receipt_paths(
            source_relative, source_argv, root
        )
        record["log"], record["record"] = log_relative, record_relative
        print(COMMAND_MARKER + json.dumps(raw_tool_argv, separators=(",", ":")))

        try:
            completed = subprocess.run(raw_tool_argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            raw_output = completed.stdout
            exit_code = completed.returncode
            record["tool_exit_code"] = exit_code
        except OSError as error:
            record["diagnostic"] = f"cannot execute IWYU: {error}"
            raise GateError(record["diagnostic"]) from error

        record["report_sha256"] = _sha256(raw_output)
        _atomic_write(log_path, raw_output)
        sections = parse_report(raw_output, source_path, cwd)
        report_details = classify_additions(sections, root, source_argv, cwd)
        for key, entries in report_details.items():
            record[key] = entries

        if exit_code != 0:
            raise GateError(f"IWYU exited with nonzero status {exit_code}")
        record["status"] = "blocked" if record["public_additions"] else "passed"
        if record["status"] == "blocked":
            record["diagnostic"] = "IWYU recommends missing owned public includes"
        result = 1 if record["status"] == "blocked" else 0
    except (GateError, OSError) as error:
        record["status"] = "error"
        record["diagnostic"] = str(error)
        result = 2
        if log_path is not None and not log_path.exists():
            try:
                _atomic_write(log_path, raw_output)
            except OSError:
                pass

    if record_path is not None:
        try:
            serialized = (json.dumps(record, sort_keys=True, indent=2) + "\n").encode("utf-8")
            _atomic_write(record_path, serialized)
        except OSError as error:
            print(f"IWYU gate could not write receipt: {error}", file=sys.stderr)
            record["status"] = "error"
            record["diagnostic"] = f"cannot write receipt: {error}"
            result = 2
    _print_result(record)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
