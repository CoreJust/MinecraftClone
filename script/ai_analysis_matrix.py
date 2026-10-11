#!/usr/bin/env python3
"""Run, merge, and verify the fail-closed S7 analysis receipt matrix.

Each required analysis executes as a bounded, candidate-bound row receipt.
Only a merger containing every passing row is accepted as release evidence.
``run-local`` remains a capability-only diagnostic stub and never passes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Mapping, Sequence


TASK_ID = "MC-AI-0249"
SCHEMA_VERSION = 1
DEFAULT_MANIFEST = Path(__file__).with_name("ai_analysis_matrix.json")
DEFAULT_RECEIPT = Path("build/ai-checks/analysis-matrix.json")
SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$")

# Keep this list synchronized with the acceptance rows in MC-AI-0249.  The
# manifest is checked against it so an accidentally truncated manifest cannot
# turn into a green release gate.
REQUIRED_ROW_IDS = (
    "macos_asan",
    "macos_ubsan",
    "macos_tsan",
    "windows_msvc_asan",
    "windows_msvc_analyze",
    "android_arm64_hwasan",
    "android_clang_tidy",
    "linux_lsan",
    "linux_msan",
    "clang_static_analysis",
    "clang_tidy",
    "iwyu",
    "cppcheck",
)
_PLACEHOLDER_VERSIONS = {"", "unknown", "unavailable", "not-run", "stub"}
REQUIRED_EFFECTIVE_COMPILE_FLAGS = {
    "macos_asan": ["-fsanitize=address"],
    "macos_ubsan": ["-fsanitize=undefined"],
    "macos_tsan": ["-fsanitize=thread"],
    "windows_msvc_asan": ["/fsanitize=address"],
    "linux_lsan": ["-fsanitize=leak"],
    "linux_msan": [
        "-fsanitize=memory",
        "-fsanitize-memory-track-origins=2",
        "-stdlib=libc++",
    ],
}
REQUIRED_ANALYSIS_CONFIGURATION = {
    "windows_msvc_analyze": ("analysis-windows-analyze", "CMAKE_CXX_FLAGS", ["/analyze", "/W4", "/WX"]),
    "clang_static_analysis": (
        "analysis-clang-static",
        "CMAKE_CXX_FLAGS",
        ["-Xanalyzer", "-analyzer-output=text"],
    ),
    "clang_tidy": (
        "analysis-clang-tidy",
        "CMAKE_CXX_CLANG_TIDY",
        ["clang-tidy", "--checks=bugprone-*,performance-*", "--warnings-as-errors=*"],
    ),
    "iwyu": (
        "analysis-iwyu",
        "CMAKE_CXX_INCLUDE_WHAT_YOU_USE",
        ["iwyu_gate.py", "include-what-you-use", "-Xiwyu", "--no_fwd_decls", "-Xiwyu", "--error"],
    ),
    "cppcheck": (
        "analysis-cppcheck",
        "CMAKE_CXX_COMPILER_LAUNCHER",
        ["cppcheck_gate.py", "cppcheck", "--enable=all", "--inline-suppr", "--xml"],
    ),
}
REQUIRED_ANALYSIS_FLAGS = {
    "iwyu": [
        "CMAKE_CXX_INCLUDE_WHAT_YOU_USE=python3;${sourceDir}/script/ci/iwyu_gate.py;--;include-what-you-use;-Xiwyu;--no_fwd_decls;-Xiwyu;--error;--",
        "CMake -Xiwyu --error suffix propagates wrapper failures and is stripped before the actual IWYU subprocess",
        "owned public include additions fail; removals, private SDK includes, and forward-declaration suggestions are reported",
    ],
    "cppcheck": [
        "CMAKE_CXX_COMPILER_LAUNCHER=python3;${sourceDir}/script/ci/cppcheck_gate.py;--;cppcheck;--enable=all;--inline-suppr;--xml;--",
        "CMAKE_C_CPPCHECK=;CMAKE_CXX_CPPCHECK= (disable stale built-in checker cache entries)",
        "error, warning, performance, and portability diagnostics fail; style, information, and debug diagnostics are reported; uninitMemberVarNoCtor and passedByValue are excluded",
    ],
}
ANALYSIS_TOOL_MARKERS = {
    "windows_msvc_analyze": ["cl.exe", "/analyze", "/W4", "/WX"],
    "clang_static_analysis": [
        "scan-build",
        "-Xanalyzer",
        "-analyzer-output=text",
    ],
    "clang_tidy": ["clang-tidy", "--checks=bugprone-*,performance-*", "--warnings-as-errors=*"],
    "iwyu": ["include-what-you-use", "-Xiwyu", "--no_fwd_decls"],
    "cppcheck": ["cppcheck", "--enable=all", "--inline-suppr", "--xml"],
}
PER_TRANSLATION_UNIT_ANALYSIS = {
    "iwyu": {
        "tool": "include-what-you-use",
        "receipt_directory": Path("build/analysis-iwyu/gate"),
    },
    "cppcheck": {
        "tool": "cppcheck",
        "receipt_directory": Path("build/analysis-cppcheck/gate"),
    },
}
CPP_SOURCE_SUFFIXES = {".cc", ".cp", ".cpp", ".cxx", ".c++", ".C"}
ANALYSIS_INPUT_SUFFIXES = {
    ".c",
    ".cc",
    ".cp",
    ".cpp",
    ".cxx",
    ".c++",
    ".C",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".ipp",
    ".tpp",
    ".inl",
    ".H",
}
REQUIRED_DIRECT_ANALYSIS_COMMANDS = {
    "android_clang_tidy": [
        "python3",
        "script/ci/android_clang_tidy.py",
        "--compile-commands-dir",
        "android/app/.cxx/Release",
        "--repository-root",
        ".",
        "--clang-tidy",
        "clang-tidy",
        "--checks=clang-analyzer-*",
        "--warnings-as-errors=clang-analyzer-*",
    ]
}


class MatrixError(ValueError):
    """A malformed manifest, receipt, or candidate identity."""


def _canonical_json(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _read_json(path: Path, description: str) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise MatrixError(f"missing {description}: {path}") from error
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise MatrixError(f"cannot read {description} {path}: {error}") from error


def _require_string(value: Any, field: str, *, nonempty: bool = True) -> str:
    if not isinstance(value, str) or (nonempty and not value.strip()):
        raise MatrixError(f"{field} must be a non-empty string")
    return value


def _require_string_list(value: Any, field: str) -> list[str]:
    if not isinstance(value, list) or not value or any(not isinstance(item, str) or not item.strip() for item in value):
        raise MatrixError(f"{field} must be a non-empty list of strings")
    return list(value)


def _require_environment(value: Any, field: str) -> dict[str, str]:
    """Validate the small, explicit process environment for one matrix row."""

    if value is None:
        return {}
    if not isinstance(value, dict):
        raise MatrixError(f"{field} must be an object")
    environment: dict[str, str] = {}
    for name, setting in value.items():
        if not isinstance(name, str) or not re.fullmatch(r"[A-Z][A-Z0-9_]*", name):
            raise MatrixError(f"{field} has an invalid environment variable name")
        environment[name] = _require_string(setting, f"{field}[{name}]")
    return environment


def _validate_sha(value: Any, field: str) -> str:
    if not isinstance(value, str) or not SHA_PATTERN.fullmatch(value):
        raise MatrixError(f"{field} must be a full lowercase commit/tree SHA")
    return value


def validate_manifest(manifest: Any) -> dict[str, Any]:
    if not isinstance(manifest, dict):
        raise MatrixError("analysis manifest must be a JSON object")
    if manifest.get("schema_version") != SCHEMA_VERSION:
        raise MatrixError(f"analysis manifest schema_version must be {SCHEMA_VERSION}")
    if manifest.get("task_id") != TASK_ID:
        raise MatrixError(f"analysis manifest task_id must be {TASK_ID}")
    rows = manifest.get("rows")
    if not isinstance(rows, list):
        raise MatrixError("analysis manifest rows must be a list")
    expected = set(REQUIRED_ROW_IDS)
    seen: set[str] = set()
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise MatrixError(f"analysis manifest row {index} must be an object")
        row_id = _require_string(row.get("id"), f"manifest row {index}.id")
        if row_id in seen:
            raise MatrixError(f"analysis manifest has duplicate row {row_id}")
        seen.add(row_id)
        _require_string(row.get("label"), f"manifest row {row_id}.label")
        _require_string(row.get("platform"), f"manifest row {row_id}.platform")
        _require_string(row.get("capability"), f"manifest row {row_id}.capability")
        _require_string_list(row.get("flags"), f"manifest row {row_id}.flags")
        if row_id in REQUIRED_ANALYSIS_FLAGS and row["flags"] != REQUIRED_ANALYSIS_FLAGS[row_id]:
            raise MatrixError(f"manifest row {row_id}.flags do not describe the configured per-TU gate")
        effective_flags = row.get("required_compile_flags")
        compile_commands = row.get("compile_commands")
        if row_id in REQUIRED_EFFECTIVE_COMPILE_FLAGS:
            if _require_string_list(effective_flags, f"manifest row {row_id}.required_compile_flags") != REQUIRED_EFFECTIVE_COMPILE_FLAGS[row_id]:
                raise MatrixError(f"manifest row {row_id}.required_compile_flags do not match the required instrumentation")
            _require_string(compile_commands, f"manifest row {row_id}.compile_commands")
        elif effective_flags is not None or compile_commands is not None:
            raise MatrixError(f"manifest row {row_id} has unexpected effective compiler flag requirements")
        _require_environment(row.get("environment"), f"manifest row {row_id}.environment")
        commands = row.get("commands")
        if not isinstance(commands, list) or not commands:
            raise MatrixError(f"manifest row {row_id}.commands must be a non-empty list")
        for command_index, command in enumerate(commands):
            _require_string_list(command, f"manifest row {row_id}.commands[{command_index}]")
        probes = row.get("tool_probes")
        if not isinstance(probes, list) or not probes:
            raise MatrixError(f"manifest row {row_id}.tool_probes must be a non-empty list")
        for probe_index, probe in enumerate(probes):
            _require_string_list(probe, f"manifest row {row_id}.tool_probes[{probe_index}]")
        if row_id in REQUIRED_DIRECT_ANALYSIS_COMMANDS:
            if row.get("platform") != "android-arm64":
                raise MatrixError(f"manifest row {row_id} must run on the Android arm64 toolchain")
            if commands != [REQUIRED_DIRECT_ANALYSIS_COMMANDS[row_id]]:
                raise MatrixError(f"manifest row {row_id} must invoke its required analyzer command")
            if ["clang-tidy", "--version"] not in probes:
                raise MatrixError(f"manifest row {row_id} must probe the clang-tidy version")
        timeout = row.get("timeout_seconds")
        if not isinstance(timeout, int) or isinstance(timeout, bool) or timeout <= 0 or timeout > 3600:
            raise MatrixError(f"manifest row {row_id}.timeout_seconds must be between 1 and 3600")
    missing = sorted(expected - seen)
    extra = sorted(seen - expected)
    if missing or extra:
        details = []
        if missing:
            details.append("missing rows: " + ", ".join(missing))
        if extra:
            details.append("unexpected rows: " + ", ".join(extra))
        raise MatrixError("analysis manifest row set is not the MC-AI-0249 acceptance set (" + "; ".join(details) + ")")
    return manifest


def load_manifest(path: Path = DEFAULT_MANIFEST) -> dict[str, Any]:
    return validate_manifest(_read_json(path, "analysis manifest"))


def manifest_sha256(manifest: Mapping[str, Any]) -> str:
    """Hash the canonical manifest bytes stored in a receipt."""

    return hashlib.sha256(_canonical_json(dict(manifest))).hexdigest()


def _git(root: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", *arguments], cwd=root, text=True, capture_output=True, check=False
    )
    if completed.returncode:
        message = completed.stderr.strip() or "git " + " ".join(arguments) + " failed"
        raise MatrixError(message)
    value = completed.stdout.strip()
    if not value:
        raise MatrixError("git " + " ".join(arguments) + " returned no value")
    return value


def candidate_identity(root: Path) -> dict[str, str]:
    """Return the exact HEAD/index identity used by the AI candidate gates."""

    # ``git write-tree`` creates both an index lock and a new object.  The
    # checkout may be deliberately read-only, so derive Git's tree-object hash
    # directly from the staged entries without mutating its index or object DB.
    completed = subprocess.run(
        ["git", "ls-files", "--stage", "-z"],
        cwd=root,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        message = completed.stderr.decode("utf-8", "replace").strip() or "git ls-files failed"
        raise MatrixError(message)
    tree: dict[bytes, Any] = {}
    for record in completed.stdout.split(b"\0"):
        if not record:
            continue
        metadata, separator, relative_path = record.partition(b"\t")
        fields = metadata.split()
        if not separator or len(fields) != 3:
            raise MatrixError("git ls-files returned an invalid staged entry")
        mode, object_id, stage = fields
        if stage != b"0":
            raise MatrixError("candidate index contains unresolved merge stages")
        current = tree
        parts = relative_path.split(b"/")
        for part in parts[:-1]:
            child = current.setdefault(part, {})
            if not isinstance(child, dict):
                raise MatrixError("candidate index has conflicting file and directory paths")
            current = child
        current[parts[-1]] = (mode, object_id)

    def hash_tree(entries: Mapping[bytes, Any]) -> bytes:
        serialized_entries: list[tuple[bytes, bytes]] = []
        for name, value in entries.items():
            if isinstance(value, dict):
                mode = b"40000"
                object_id = hash_tree(value)
                sort_name = name + b"/"
            else:
                mode, object_hex = value
                object_id = bytes.fromhex(object_hex.decode("ascii"))
                sort_name = name
            serialized_entries.append((sort_name, mode + b" " + name + b"\0" + object_id))
        payload = b"".join(entry for _, entry in sorted(serialized_entries))
        return hashlib.sha1(b"tree " + str(len(payload)).encode("ascii") + b"\0" + payload).digest()

    return {
        "head": _validate_sha(_git(root, "rev-parse", "--verify", "HEAD^{commit}"), "candidate.head"),
        "tree": _validate_sha(hash_tree(tree).hex(), "candidate.tree"),
    }


def _validate_candidate(candidate: Any, field: str = "candidate") -> dict[str, str]:
    if not isinstance(candidate, dict):
        raise MatrixError(f"{field} must be an object")
    if set(candidate) != {"head", "tree"}:
        raise MatrixError(f"{field} must contain exactly head and tree")
    return {
        "head": _validate_sha(candidate["head"], f"{field}.head"),
        "tree": _validate_sha(candidate["tree"], f"{field}.tree"),
    }


def require_clean_checkout(root: Path, candidate: Mapping[str, str]) -> None:
    """Require a hosted row to describe the exact, unmodified checkout being analyzed."""

    actual = candidate_identity(root)
    if actual != dict(candidate):
        raise MatrixError("analysis row candidate does not match the checkout HEAD and index")
    committed_tree = _git(root, "rev-parse", "--verify", "HEAD^{tree}")
    if committed_tree != candidate["tree"]:
        raise MatrixError("analysis row candidate tree is not the committed HEAD tree")
    completed = subprocess.run(
        ["git", "status", "--porcelain=v1", "--untracked-files=all"],
        cwd=root,
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        message = completed.stderr.strip() or "git status failed"
        raise MatrixError(message)
    if completed.stdout.strip():
        raise MatrixError("analysis rows require a clean committed checkout")


def _compile_command_arguments(row_id: str, index: int, entry: Any) -> list[str]:
    """Return the ordered compiler argv stored for one compilation database entry."""

    if not isinstance(entry, dict):
        raise MatrixError(f"{row_id} compile command {index} must be an object")
    arguments = entry.get("arguments")
    if arguments is not None:
        if not isinstance(arguments, list) or not arguments or any(not isinstance(value, str) or not value for value in arguments):
            raise MatrixError(f"{row_id} compile command {index}.arguments is malformed")
        return list(arguments)
    command = entry.get("command")
    if not isinstance(command, str) or not command.strip():
        raise MatrixError(f"{row_id} compile command {index} has no arguments or command")
    try:
        tokens = shlex.split(command, posix=_host_platform() != "windows")
    except ValueError as error:
        raise MatrixError(f"{row_id} compile command {index} cannot be parsed: {error}") from error
    if not tokens:
        raise MatrixError(f"{row_id} compile command {index} has no arguments")
    return tokens


def capture_effective_compile_flags(root: Path, manifest_row: Mapping[str, Any]) -> dict[str, Any]:
    """Validate and hash the compiler commands generated by the row's CMake preset."""

    row_id = manifest_row["id"]
    compile_commands_path = root / _require_string(
        manifest_row.get("compile_commands"), f"manifest row {row_id}.compile_commands"
    )
    compile_commands = _read_json(compile_commands_path, f"{row_id} compile_commands.json")
    if not isinstance(compile_commands, list) or not compile_commands:
        raise MatrixError(f"{row_id} compile_commands.json must contain compiler commands")

    required_flags = _require_string_list(
        manifest_row.get("required_compile_flags"),
        f"manifest row {row_id}.required_compile_flags",
    )
    for index, entry in enumerate(compile_commands):
        tokens = _compile_command_arguments(row_id, index, entry)
        missing = [flag for flag in required_flags if flag not in tokens]
        if missing:
            raise MatrixError(
                f"{row_id} effective compiler flags are missing from command {index}: "
                + ", ".join(missing)
            )

    return {
        "compile_commands_sha256": hashlib.sha256(_canonical_json(compile_commands)).hexdigest(),
        "command_count": len(compile_commands),
        "observed_flags": required_flags,
    }


def project_analysis_input_sha256(root: Path) -> str:
    """Hash owned C++ inputs and analyzer/build policy shared by per-TU gates."""

    root = root.resolve()
    excluded_directories = {".git", ".worktrees", "build", "node_modules", "dist", "out", "vcpkg_installed"}
    paths: dict[str, Path] = {}
    for current, directories, filenames in os.walk(root):
        directories[:] = [name for name in directories if name not in excluded_directories]
        current_path = Path(current)
        for filename in filenames:
            file_path = current_path / filename
            relative = file_path.relative_to(root)
            relative_name = relative.as_posix()
            if (
                relative_name == "CMakePresets.json"
                or relative_name in {
                    "script/ai_analysis_matrix.py",
                    "script/ai_analysis_matrix.json",
                    "script/ci/iwyu_gate.py",
                    "script/ci/cppcheck_gate.py",
                }
                or filename == "CMakeLists.txt"
                or file_path.suffix == ".cmake"
                or (relative.parts[0] in {"src", "tests"} and file_path.suffix in ANALYSIS_INPUT_SUFFIXES)
            ):
                paths[relative_name] = file_path

    required_files = {
        "CMakePresets.json",
        "script/ai_analysis_matrix.py",
        "script/ai_analysis_matrix.json",
        "script/ci/iwyu_gate.py",
        "script/ci/cppcheck_gate.py",
    }
    missing = sorted(required_files - paths.keys())
    if missing:
        raise MatrixError("analysis input fingerprint is missing required files: " + ", ".join(missing))

    digest = hashlib.sha256()
    for relative_name, file_path in sorted(paths.items()):
        try:
            contents = file_path.read_bytes()
        except OSError as error:
            raise MatrixError(f"cannot read analysis input {relative_name}: {error}") from error
        digest.update(relative_name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(len(contents).to_bytes(8, byteorder="big"))
        digest.update(contents)
    return digest.hexdigest()


def _analysis_translation_units(root: Path, row_id: str, preset: str) -> list[dict[str, Any]]:
    compile_commands_path = root / "build" / preset / "compile_commands.json"
    compile_commands = _read_json(compile_commands_path, f"{row_id} compile_commands.json")
    if not isinstance(compile_commands, list) or not compile_commands:
        raise MatrixError(f"{row_id} compile_commands.json must contain compiler commands")

    root = root.resolve()
    inventory: list[dict[str, Any]] = []
    seen: set[tuple[str, tuple[str, ...]]] = set()
    for index, entry in enumerate(compile_commands):
        if not isinstance(entry, dict):
            raise MatrixError(f"{row_id} compile command {index} must be an object")
        file_value = _require_string(entry.get("file"), f"{row_id} compile command {index}.file")
        directory_value = _require_string(
            entry.get("directory"), f"{row_id} compile command {index}.directory"
        )
        if Path(file_value).suffix not in CPP_SOURCE_SUFFIXES:
            continue
        directory = Path(directory_value)
        if not directory.is_absolute():
            directory = root / directory
        source_path = Path(file_value)
        if not source_path.is_absolute():
            source_path = directory / source_path
        source_path = source_path.resolve()
        try:
            relative = source_path.relative_to(root).as_posix()
        except ValueError:
            continue
        if not (relative.startswith("src/") or relative.startswith("tests/")):
            continue
        if not source_path.is_file():
            raise MatrixError(f"{row_id} owned translation unit is missing: {relative}")
        working_directory = directory.resolve()
        compiler_argv = _compile_command_arguments(row_id, index, entry)
        if len(compiler_argv) < 2:
            raise MatrixError(f"{row_id} compile command {index} has no compiler arguments")
        comparison_argv = compiler_argv[1:] if row_id == "iwyu" else compiler_argv
        key = (relative, tuple(comparison_argv))
        if key in seen:
            raise MatrixError(f"{row_id} compile_commands.json has a duplicate owned command for {relative}")
        seen.add(key)
        source_sha256 = hashlib.sha256(source_path.read_bytes()).hexdigest()
        inventory.append(
            {
                "source": relative,
                "source_path": source_path,
                "source_sha256": source_sha256,
                "cwd": str(working_directory),
                "compiler_argv": compiler_argv,
                "source_argv": comparison_argv,
            }
        )
    if not inventory:
        raise MatrixError(f"{row_id} compile_commands.json has no owned C++ translation units under src/ or tests/")
    return sorted(inventory, key=lambda item: (item["source"], _canonical_json(item["source_argv"])))


def _translation_unit_inventory_sha256(records: Sequence[Mapping[str, Any]]) -> str:
    inventory = [
        {
            "source": record["source"],
            "source_sha256": record["source_sha256"],
            "source_argv_sha256": record.get("source_argv_sha256")
            or hashlib.sha256(_canonical_json(record["source_argv"])).hexdigest(),
        }
        for record in records
    ]
    return hashlib.sha256(_canonical_json(inventory)).hexdigest()


def _compiler_argv_matches(compile_argv: Sequence[str], observed_argv: Sequence[str]) -> bool:
    """Accept the exact dependency flags CMake adds before one compile output."""

    expected = list(compile_argv)
    observed = list(observed_argv)
    if observed == expected:
        return True
    output_options = [index for index, value in enumerate(expected[:-1]) if value == "-o"]
    if len(output_options) != 1:
        return False
    output_index = output_options[0]
    object_path = expected[output_index + 1]
    dependency_flags = ["-MD", "-MT", object_path, "-MF", object_path + ".d"]
    with_dependencies = [*expected[:output_index], *dependency_flags, *expected[output_index:]]
    return observed == with_dependencies


def _iwyu_argv_matches(compile_argv: Sequence[str], observed_argv: Sequence[str]) -> bool:
    """Accept CMake dependency flags plus its single driver-mode adapter token."""

    observed = list(observed_argv)
    if _compiler_argv_matches(compile_argv, observed):
        return True
    driver_mode = "--driver-mode=g++"
    if observed and observed[0] == driver_mode:
        return _compiler_argv_matches(compile_argv, observed[1:])
    if observed and observed[-1] == driver_mode:
        return _compiler_argv_matches(compile_argv, observed[:-1])
    return False


def _read_translation_unit_records(
    root: Path,
    row_id: str,
    inventory: Sequence[Mapping[str, Any]],
    project_input_sha256: str,
) -> list[dict[str, str]]:
    root = root.resolve()
    gate_directory = root / PER_TRANSLATION_UNIT_ANALYSIS[row_id]["receipt_directory"]
    if not gate_directory.is_dir():
        raise MatrixError(f"{row_id} per-translation-unit receipt directory is missing: {gate_directory}")
    record_paths = sorted(gate_directory.glob("*.json"))
    if not record_paths:
        raise MatrixError(f"{row_id} produced no per-translation-unit receipts")

    expected = {(item["source"], tuple(item["source_argv"])): item for item in inventory}
    observed: dict[tuple[str, tuple[str, ...]], tuple[dict[str, Any], Path, bytes]] = {}
    stale_project_records = 0
    for record_path in record_paths:
        try:
            raw = record_path.read_bytes()
            record = json.loads(raw.decode("utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise MatrixError(f"{row_id} has an unreadable per-translation-unit receipt {record_path}: {error}") from error
        if not isinstance(record, dict):
            raise MatrixError(f"{row_id} per-translation-unit receipt must be a JSON object: {record_path}")
        if row_id == "cppcheck" and record.get("project_input_sha256") != project_input_sha256:
            # Identity-addressed reports from older source/policy snapshots are never evidence
            # for this run; current-fingerprint records still need exact TU coverage below.
            stale_project_records += 1
            continue

        if row_id == "iwyu":
            if record.get("schema_version") != 1 or isinstance(record.get("schema_version"), bool):
                raise MatrixError(f"{row_id} receipt {record_path.name} has an unsupported schema")
            source = _require_string(record.get("source"), f"{row_id} receipt source")
            source_argv = _require_string_list(record.get("source_argv"), f"{row_id} receipt source_argv")
            tool = record.get("invoked_tool")
            tool_argv = _require_string_list(record.get("tool_argv"), f"{row_id} receipt tool_argv")
            expected_tool_argv = ["-Xiwyu", "--no_fwd_decls"]
            if not tool_argv or Path(tool_argv[0]).name != "include-what-you-use" or tool_argv[1:3] != expected_tool_argv:
                raise MatrixError(f"{row_id} receipt {record_path.name} has an unexpected IWYU command")
            adapter_argv = _require_string_list(record.get("adapter_argv"), f"{row_id} receipt adapter_argv")
            expected_adapter_argv = [
                tool_argv[0],
                "-Xiwyu",
                "--no_fwd_decls",
                "-Xiwyu",
                "--error",
                "--",
                *source_argv,
            ]
            if adapter_argv != expected_adapter_argv:
                raise MatrixError(f"{row_id} receipt {record_path.name} has unexpected CMake adapter arguments")
            if tool_argv[3:] != source_argv:
                raise MatrixError(f"{row_id} receipt {record_path.name} does not bind the full IWYU argv")
            tool_exit_code = record.get("tool_exit_code")
            if tool != "include-what-you-use" or not isinstance(tool_exit_code, int) or isinstance(tool_exit_code, bool) or tool_exit_code != 0:
                raise MatrixError(f"{row_id} receipt {record_path.name} did not run IWYU successfully")
            if record.get("status") != "passed":
                raise MatrixError(f"{row_id} receipt {record_path.name} is not passed")
            if record.get("public_additions") != []:
                raise MatrixError(f"{row_id} receipt {record_path.name} reports owned public include additions")
            for key in ("removals", "forward_declarations", "mapped_facades", "unsupported_suggestions"):
                if not isinstance(record.get(key), list) or any(not isinstance(value, str) for value in record[key]):
                    raise MatrixError(f"{row_id} receipt {record_path.name} has malformed {key}")
            raw_report_sha256 = record.get("report_sha256")
            report_path_value = record.get("log")
            receipt_path_value = record.get("record")
            source_argv_json = json.dumps(source_argv, separators=(",", ":")).encode("utf-8")
            expected_stem = hashlib.sha256(source.encode("utf-8") + b"\0" + source_argv_json).hexdigest()
            if record_path.stem != expected_stem:
                raise MatrixError(f"{row_id} receipt {record_path.name} has an invalid source identity")
        else:
            if record.get("schema") != 1 or isinstance(record.get("schema"), bool) or record.get("gate") != "cppcheck-per-tu":
                raise MatrixError(f"{row_id} receipt {record_path.name} has an unsupported schema or gate")
            source_record = record.get("source")
            compiler_record = record.get("compiler")
            cppcheck_record = record.get("cppcheck")
            if not isinstance(source_record, dict) or not isinstance(compiler_record, dict) or not isinstance(cppcheck_record, dict):
                raise MatrixError(f"{row_id} receipt {record_path.name} is missing source or tool details")
            source = _require_string(source_record.get("relative"), f"{row_id} receipt source.relative")
            source_argv = _require_string_list(compiler_record.get("argv"), f"{row_id} receipt compiler.argv")
            tool = "cppcheck"
            tool_argv = _require_string_list(cppcheck_record.get("argv"), f"{row_id} receipt cppcheck.argv")
            if not tool_argv or Path(tool_argv[0]).name != "cppcheck" or any(
                flag not in tool_argv
                for flag in ("--enable=all", "--inline-suppr", "--xml")
            ):
                raise MatrixError(f"{row_id} receipt {record_path.name} has an unexpected cppcheck command")
            cppcheck_returncode = cppcheck_record.get("returncode")
            compiler_returncode = compiler_record.get("returncode")
            if (
                record.get("status") != "pass"
                or not isinstance(cppcheck_returncode, int)
                or isinstance(cppcheck_returncode, bool)
                or cppcheck_returncode != 0
                or not isinstance(compiler_returncode, int)
                or isinstance(compiler_returncode, bool)
                or compiler_returncode != 0
            ):
                raise MatrixError(f"{row_id} receipt {record_path.name} did not pass cppcheck and compiler")
            if record.get("failure_stage") is not None:
                raise MatrixError(f"{row_id} receipt {record_path.name} has a failure stage despite passing")
            summary = record.get("summary")
            if (
                not isinstance(summary, dict)
                or not isinstance(summary.get("blocking"), int)
                or isinstance(summary.get("blocking"), bool)
                or summary.get("blocking") != 0
            ):
                raise MatrixError(f"{row_id} receipt {record_path.name} reports blocking cppcheck diagnostics")
            diagnostics = record.get("diagnostics")
            if not isinstance(diagnostics, list) or any(
                not isinstance(item, dict) or item.get("blocking") is not False for item in diagnostics
            ):
                raise MatrixError(f"{row_id} receipt {record_path.name} contains malformed or blocking diagnostics")
            severity_counts = summary.get("by_severity")
            if not isinstance(severity_counts, dict) or any(
                not isinstance(value, int) or isinstance(value, bool) or value < 0
                for value in severity_counts.values()
            ):
                raise MatrixError(f"{row_id} receipt {record_path.name} has malformed severity counts")
            if not isinstance(cppcheck_record.get("version"), str) or not cppcheck_record["version"]:
                raise MatrixError(f"{row_id} receipt {record_path.name} has no cppcheck version")
            if not isinstance(record.get("cwd"), str) or not Path(record["cwd"]).is_absolute():
                raise MatrixError(f"{row_id} receipt {record_path.name} has an invalid compiler working directory")
            absolute_source = _require_string(source_record.get("absolute"), f"{row_id} receipt source.absolute")
            if not Path(absolute_source).is_absolute():
                raise MatrixError(f"{row_id} receipt {record_path.name} has a non-absolute source path")
            identity = _require_string(record.get("identity_sha256"), f"{row_id} receipt identity_sha256")
            if not re.fullmatch(r"[0-9a-f]{64}", identity):
                raise MatrixError(f"{row_id} receipt {record_path.name} has an invalid identity hash")
            identity_preimage = {
                "source_absolute": absolute_source,
                "source_sha256": source_record.get("sha256"),
                "project_input_sha256": record.get("project_input_sha256"),
                "cppcheck_argv": tool_argv,
                "compiler_argv": source_argv,
                "cwd": record["cwd"],
            }
            expected_identity = hashlib.sha256(_canonical_json(identity_preimage)).hexdigest()
            if identity != expected_identity or record_path.stem != identity:
                raise MatrixError(f"{row_id} receipt {record_path.name} has an invalid command identity")
            raw_report_sha256 = record.get("raw_report_sha256")
            artifacts = record.get("artifacts")
            if not isinstance(artifacts, dict):
                raise MatrixError(f"{row_id} receipt {record_path.name} is missing report artifacts")
            report_path_value = artifacts.get("xml")
            receipt_path_value = artifacts.get("receipt")
            if artifacts.get("root") != str(gate_directory.resolve()):
                raise MatrixError(f"{row_id} receipt {record_path.name} names the wrong artifact directory")
            if record.get("project_input_sha256") != project_input_sha256:
                raise MatrixError(f"{row_id} receipt {record_path.name} has stale project inputs")

        key = (source, tuple(source_argv))
        if key not in expected:
            matching_keys = [
                expected_key
                for expected_key in expected
                if expected_key[0] == source
                and (
                    _iwyu_argv_matches(expected_key[1], source_argv)
                    if row_id == "iwyu"
                    else _compiler_argv_matches(expected_key[1], source_argv)
                )
            ]
            if len(matching_keys) == 1:
                key = matching_keys[0]
        if key in observed:
            raise MatrixError(f"{row_id} has duplicate per-translation-unit receipts for {source}")
        observed[key] = (record, record_path, raw)

        if key not in expected:
            continue
        current = expected[key]
        if row_id == "cppcheck":
            if source_record.get("absolute") != str(current["source_path"]):
                raise MatrixError(f"{row_id} receipt {record_path.name} has a mismatched absolute source path")
            if record.get("cwd") != current["cwd"]:
                raise MatrixError(f"{row_id} receipt {record_path.name} has a stale compiler working directory")
            if not _compiler_argv_matches(current["compiler_argv"], source_argv):
                raise MatrixError(f"{row_id} receipt {record_path.name} has a mismatched compiler command")
        source_sha256 = record.get("source_sha256") if row_id == "iwyu" else source_record.get("sha256")
        if source_sha256 != current["source_sha256"]:
            raise MatrixError(f"{row_id} receipt {record_path.name} has a stale source hash")
        record_project_hash = record.get("project_input_sha256")
        if record_project_hash != project_input_sha256:
            raise MatrixError(f"{row_id} receipt {record_path.name} has stale project inputs")
        report_sha256 = _require_string(raw_report_sha256, f"{row_id} receipt report SHA256")
        if not re.fullmatch(r"[0-9a-f]{64}", report_sha256):
            raise MatrixError(f"{row_id} receipt {record_path.name} has an invalid report hash")
        if not isinstance(report_path_value, str) or not isinstance(receipt_path_value, str):
            raise MatrixError(f"{row_id} receipt {record_path.name} has invalid artifact paths")
        report_path_value_path = Path(report_path_value)
        if row_id == "cppcheck" and not report_path_value_path.is_absolute():
            raise MatrixError(f"{row_id} receipt {record_path.name} has a non-absolute XML path")
        if row_id == "iwyu" and report_path_value_path.is_absolute():
            raise MatrixError(f"{row_id} receipt {record_path.name} has an unexpected absolute log path")
        report_path = (report_path_value_path if report_path_value_path.is_absolute() else root / report_path_value_path).resolve()
        if report_path != (gate_directory / f"{record_path.stem}.{'log' if row_id == 'iwyu' else 'xml'}").resolve():
            raise MatrixError(f"{row_id} receipt {record_path.name} does not name its paired report")
        try:
            report_path.relative_to(gate_directory.resolve())
        except ValueError as error:
            raise MatrixError(f"{row_id} report path escapes its gate directory") from error
        if not report_path.is_file() or hashlib.sha256(report_path.read_bytes()).hexdigest() != report_sha256:
            raise MatrixError(f"{row_id} receipt {record_path.name} has a missing or stale raw report")
        expected_receipt_path = str(record_path.resolve()) if row_id == "cppcheck" else record_path.relative_to(root).as_posix()
        if receipt_path_value != expected_receipt_path:
            raise MatrixError(f"{row_id} receipt {record_path.name} does not name itself as its artifact")

    if not observed and stale_project_records:
        raise MatrixError(f"{row_id} receipts have stale project inputs")
    missing = sorted(set(expected) - set(observed))
    extra = sorted(set(observed) - set(expected))
    if missing or extra:
        details = []
        if missing:
            details.append(f"missing {len(missing)} owned translation-unit receipt(s)")
        if extra:
            details.append(f"found {len(extra)} stale or unexpected translation-unit receipt(s)")
        raise MatrixError(f"{row_id} receipt set does not match compile_commands.json: " + "; ".join(details))

    summaries = []
    for key in sorted(expected, key=lambda value: (value[0], _canonical_json(list(value[1])))):
        current = expected[key]
        record, record_path, raw = observed[key]
        observed_argv = record["source_argv"] if row_id == "iwyu" else record["compiler"]["argv"]
        summaries.append(
            {
                "source": current["source"],
                "source_sha256": current["source_sha256"],
                "source_argv_sha256": hashlib.sha256(_canonical_json(observed_argv)).hexdigest(),
                "invoked_tool": PER_TRANSLATION_UNIT_ANALYSIS[row_id]["tool"],
                "status": "passed",
                "record_sha256": hashlib.sha256(raw).hexdigest(),
                "report_sha256": record.get("report_sha256") if row_id == "iwyu" else record.get("raw_report_sha256"),
            }
        )
    return summaries


def capture_effective_analysis(
    root: Path, manifest_row: Mapping[str, Any], build_output: str
) -> dict[str, Any]:
    """Prove configured analyzer settings and a tool invocation in verbose build output."""

    row_id = manifest_row["id"]
    preset, cache_key, required_settings = REQUIRED_ANALYSIS_CONFIGURATION[row_id]
    cache_path = root / "build" / preset / "CMakeCache.txt"
    cache_text = cache_path.read_text(encoding="utf-8")
    cache_value = None
    for line in cache_text.splitlines():
        if line.startswith(cache_key + ":") and "=" in line:
            cache_value = line.split("=", 1)[1]
            break
    if cache_value is None:
        raise MatrixError(f"{row_id} CMakeCache.txt is missing {cache_key}")
    for setting in required_settings:
        if setting not in cache_value:
            raise MatrixError(f"{row_id} effective analysis configuration is missing {setting}")
    if row_id == "cppcheck":
        cache_values = {
            line.split(":", 1)[0]: line.split("=", 1)[1]
            for line in cache_text.splitlines()
            if ":" in line and "=" in line
        }
        for cache_key in ("CMAKE_C_CPPCHECK", "CMAKE_CXX_CPPCHECK"):
            if cache_key not in cache_values or cache_values[cache_key]:
                raise MatrixError(f"{row_id} effective analysis configuration must clear {cache_key}")

    markers = ANALYSIS_TOOL_MARKERS[row_id]
    if row_id in PER_TRANSLATION_UNIT_ANALYSIS:
        inventory = _analysis_translation_units(root, row_id, preset)
        project_input_digest = project_analysis_input_sha256(root)
        translation_units = _read_translation_unit_records(
            root, row_id, inventory, project_input_digest
        )
        return {
            "configuration_sha256": hashlib.sha256(cache_text.encode("utf-8")).hexdigest(),
            "configuration_key": cache_key,
            "observed_settings": required_settings,
            "invocation_count": len(translation_units),
            "observed_tools_and_flags": markers,
            "build_output_sha256": hashlib.sha256(build_output.encode("utf-8")).hexdigest(),
            "translation_unit_count": len(translation_units),
            "translation_unit_inventory_sha256": _translation_unit_inventory_sha256(translation_units),
            "project_input_sha256": project_input_digest,
            "translation_units": translation_units,
        }
    invocation_count = sum(
        1 for line in build_output.splitlines() if all(marker in line for marker in markers)
    )
    if invocation_count == 0:
        raise MatrixError(f"{row_id} verbose build output contains no configured analyzer invocation")
    return {
        "configuration_sha256": hashlib.sha256(cache_text.encode("utf-8")).hexdigest(),
        "configuration_key": cache_key,
        "observed_settings": required_settings,
        "invocation_count": invocation_count,
        "observed_tools_and_flags": markers,
        "build_output_sha256": hashlib.sha256(build_output.encode("utf-8")).hexdigest(),
    }


def _validate_row_receipt(row: Any, manifest_row: Mapping[str, Any], candidate: Mapping[str, str]) -> None:
    row_id = manifest_row["id"]
    if not isinstance(row, dict):
        raise MatrixError(f"receipt row {row_id} must be an object")
    if row.get("id") != row_id:
        raise MatrixError(f"receipt row identity mismatch for {row_id}")
    row_candidate = _validate_candidate(row.get("candidate"), f"receipt row {row_id}.candidate")
    if row_candidate != candidate:
        raise MatrixError(f"receipt row {row_id} is bound to a different candidate")
    status = row.get("status")
    if status != "passed":
        raise MatrixError(f"analysis row {row_id} is not passed (status={status!r})")
    flags = _require_string_list(row.get("flags"), f"receipt row {row_id}.flags")
    if flags != manifest_row["flags"]:
        raise MatrixError(f"receipt row {row_id}.flags do not match the manifest")
    versions = row.get("tool_versions")
    if not isinstance(versions, dict) or not versions:
        raise MatrixError(f"receipt row {row_id}.tool_versions must be a non-empty object")
    for tool, version in versions.items():
        _require_string(tool, f"receipt row {row_id}.tool_versions key")
        value = _require_string(version, f"receipt row {row_id}.tool_versions[{tool!r}]")
        if value.strip().lower() in _PLACEHOLDER_VERSIONS:
            raise MatrixError(f"receipt row {row_id}.tool_versions[{tool!r}] is unavailable")
    commands = row.get("commands")
    if commands != manifest_row["commands"]:
        raise MatrixError(f"receipt row {row_id}.commands do not match the manifest")
    executions = row.get("executed_commands")
    if not isinstance(executions, list) or len(executions) != len(commands):
        raise MatrixError(f"receipt row {row_id}.executed_commands must record every launched command")
    for index, (execution, declared) in enumerate(zip(executions, commands)):
        field = f"receipt row {row_id}.executed_commands[{index}]"
        if not isinstance(execution, dict) or execution.get("declared_command") != declared:
            raise MatrixError(f"{field} does not match the manifest command")
        argv = _require_string_list(execution.get("argv"), f"{field}.argv")
        if argv[0] != declared[0]:
            raise MatrixError(f"{field}.argv does not invoke the declared executable")
        if row_id in REQUIRED_DIRECT_ANALYSIS_COMMANDS and argv != declared:
            raise MatrixError(f"{field}.argv must execute the exact required analyzer command")
        if row_id in REQUIRED_ANALYSIS_CONFIGURATION and declared[:2] == ["cmake", "--build"] and "--verbose" not in argv:
            raise MatrixError(f"{field}.argv must enable verbose analyzer command capture")
        if execution.get("return_code") != 0:
            raise MatrixError(f"{field} did not exit successfully")
        digest = execution.get("output_sha256")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise MatrixError(f"{field}.output_sha256 is invalid")
        output_bytes = execution.get("output_bytes")
        if not isinstance(output_bytes, int) or isinstance(output_bytes, bool) or output_bytes < 0:
            raise MatrixError(f"{field}.output_bytes is invalid")
    environment = _require_environment(row.get("environment"), f"receipt row {row_id}.environment")
    if environment != _require_environment(manifest_row.get("environment"), f"manifest row {row_id}.environment"):
        raise MatrixError(f"receipt row {row_id}.environment does not match the manifest")
    effective_flags = row.get("effective_compile_flags")
    required_effective_flags = manifest_row.get("required_compile_flags")
    if required_effective_flags is not None:
        if not isinstance(effective_flags, dict):
            raise MatrixError(f"receipt row {row_id} is missing effective compiler flags")
        if set(effective_flags) != {"compile_commands_sha256", "command_count", "observed_flags"}:
            raise MatrixError(f"receipt row {row_id}.effective_compile_flags has invalid fields")
        digest = effective_flags.get("compile_commands_sha256")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise MatrixError(f"receipt row {row_id}.effective_compile_flags has an invalid compile-commands hash")
        command_count = effective_flags.get("command_count")
        if not isinstance(command_count, int) or isinstance(command_count, bool) or command_count < 1:
            raise MatrixError(f"receipt row {row_id}.effective_compile_flags has an invalid command count")
        observed_flags = _require_string_list(
            effective_flags.get("observed_flags"), f"receipt row {row_id}.effective_compile_flags.observed_flags"
        )
        if observed_flags != required_effective_flags:
            raise MatrixError(f"receipt row {row_id}.effective_compile_flags do not match observed instrumentation")
    elif effective_flags is not None:
        raise MatrixError(f"receipt row {row_id} has unexpected effective compiler flags")
    effective_analysis = row.get("effective_analysis")
    if row_id in REQUIRED_ANALYSIS_CONFIGURATION:
        if not isinstance(effective_analysis, dict):
            raise MatrixError(f"receipt row {row_id} is missing effective analysis evidence")
        expected_fields = {
            "configuration_sha256",
            "configuration_key",
            "observed_settings",
            "invocation_count",
            "observed_tools_and_flags",
            "build_output_sha256",
        }
        if row_id in PER_TRANSLATION_UNIT_ANALYSIS:
            expected_fields.update(
                {
                    "translation_unit_count",
                    "translation_unit_inventory_sha256",
                    "project_input_sha256",
                    "translation_units",
                }
            )
        if set(effective_analysis) != expected_fields:
            raise MatrixError(f"receipt row {row_id}.effective_analysis has invalid fields")
        for key in ("configuration_sha256", "build_output_sha256"):
            value = effective_analysis.get(key)
            if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
                raise MatrixError(f"receipt row {row_id}.effective_analysis.{key} is invalid")
        preset, cache_key, settings = REQUIRED_ANALYSIS_CONFIGURATION[row_id]
        del preset
        if effective_analysis.get("configuration_key") != cache_key:
            raise MatrixError(f"receipt row {row_id}.effective_analysis configuration key is incorrect")
        if effective_analysis.get("observed_settings") != settings:
            raise MatrixError(f"receipt row {row_id}.effective_analysis settings are incomplete")
        count = effective_analysis.get("invocation_count")
        if not isinstance(count, int) or isinstance(count, bool) or count < 1:
            raise MatrixError(f"receipt row {row_id}.effective_analysis has no observed analyzer invocation")
        if effective_analysis.get("observed_tools_and_flags") != ANALYSIS_TOOL_MARKERS[row_id]:
            raise MatrixError(f"receipt row {row_id}.effective_analysis tool flags are incomplete")
        if row_id in PER_TRANSLATION_UNIT_ANALYSIS:
            input_digest = effective_analysis.get("project_input_sha256")
            if not isinstance(input_digest, str) or not re.fullmatch(r"[0-9a-f]{64}", input_digest):
                raise MatrixError(f"receipt row {row_id}.effective_analysis project input hash is invalid")
            inventory_digest = effective_analysis.get("translation_unit_inventory_sha256")
            if not isinstance(inventory_digest, str) or not re.fullmatch(r"[0-9a-f]{64}", inventory_digest):
                raise MatrixError(f"receipt row {row_id}.effective_analysis inventory hash is invalid")
            translation_unit_count = effective_analysis.get("translation_unit_count")
            translation_units = effective_analysis.get("translation_units")
            if (
                not isinstance(translation_unit_count, int)
                or isinstance(translation_unit_count, bool)
                or translation_unit_count < 1
                or not isinstance(translation_units, list)
                or len(translation_units) != translation_unit_count
                or count != translation_unit_count
            ):
                raise MatrixError(f"receipt row {row_id}.effective_analysis translation-unit coverage is invalid")
            seen_units: set[tuple[str, str]] = set()
            for index, unit in enumerate(translation_units):
                field = f"receipt row {row_id}.effective_analysis.translation_units[{index}]"
                if not isinstance(unit, dict) or set(unit) != {
                    "source",
                    "source_sha256",
                    "source_argv_sha256",
                    "invoked_tool",
                    "status",
                    "record_sha256",
                    "report_sha256",
                }:
                    raise MatrixError(f"{field} has invalid fields")
                source_path = _require_string(unit.get("source"), f"{field}.source")
                source_parts = Path(source_path).parts
                if Path(source_path).is_absolute() or ".." in source_parts or not (
                    source_path.startswith("src/") or source_path.startswith("tests/")
                ):
                    raise MatrixError(f"{field}.source is outside owned src/tests inputs")
                for hash_key in ("source_sha256", "source_argv_sha256", "record_sha256", "report_sha256"):
                    value = unit.get(hash_key)
                    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
                        raise MatrixError(f"{field}.{hash_key} is invalid")
                tool = unit.get("invoked_tool")
                if tool != PER_TRANSLATION_UNIT_ANALYSIS[row_id]["tool"] or unit.get("status") != "passed":
                    raise MatrixError(f"{field} did not record a passing analyzer invocation")
                identity = (source_path, unit["source_argv_sha256"])
                if identity in seen_units:
                    raise MatrixError(f"receipt row {row_id} has duplicate translation-unit evidence")
                seen_units.add(identity)
            if _translation_unit_inventory_sha256(translation_units) != inventory_digest:
                raise MatrixError(f"receipt row {row_id}.effective_analysis inventory hash does not match its records")
    elif effective_analysis is not None:
        raise MatrixError(f"receipt row {row_id} has unexpected effective analysis evidence")
    diagnostics = row.get("diagnostics")
    if not isinstance(diagnostics, str):
        raise MatrixError(f"receipt row {row_id}.diagnostics must be a string")
    diagnostics_sha256 = row.get("diagnostics_sha256")
    if not isinstance(diagnostics_sha256, str) or diagnostics_sha256 != hashlib.sha256(diagnostics.encode("utf-8")).hexdigest():
        raise MatrixError(f"receipt row {row_id}.diagnostics_sha256 does not match captured diagnostics")
    duration = row.get("duration_seconds")
    if not isinstance(duration, (int, float)) or isinstance(duration, bool) or duration < 0:
        raise MatrixError(f"receipt row {row_id}.duration_seconds must be a non-negative number")
    if duration > manifest_row["timeout_seconds"]:
        raise MatrixError(f"receipt row {row_id} exceeded its bounded timeout")


def validate_receipt(
    receipt: Any,
    manifest: Mapping[str, Any],
    expected_candidate: Mapping[str, str] | None = None,
) -> dict[str, Any]:
    """Validate one complete passing receipt, or raise ``MatrixError``."""

    checked_manifest = validate_manifest(manifest)
    if not isinstance(receipt, dict):
        raise MatrixError("analysis receipt must be a JSON object")
    if receipt.get("schema_version") != SCHEMA_VERSION:
        raise MatrixError(f"analysis receipt schema_version must be {SCHEMA_VERSION}")
    if receipt.get("task_id") != TASK_ID:
        raise MatrixError(f"analysis receipt task_id must be {TASK_ID}")
    if receipt.get("manifest_sha256") != manifest_sha256(checked_manifest):
        raise MatrixError("analysis receipt manifest_sha256 does not match the manifest")
    candidate = _validate_candidate(receipt.get("candidate"))
    source = receipt.get("source")
    if source is None:
        raise MatrixError("analysis receipt is missing source binding")
    if _validate_candidate(source, "source") != candidate:
        raise MatrixError("analysis receipt source binding does not match candidate")
    if expected_candidate is not None and candidate != _validate_candidate(expected_candidate, "expected_candidate"):
        raise MatrixError("analysis receipt does not match the requested candidate")
    rows = receipt.get("rows")
    if not isinstance(rows, list):
        raise MatrixError("analysis receipt rows must be a list")
    expected_rows = {row["id"]: row for row in checked_manifest["rows"]}
    seen: set[str] = set()
    for row in rows:
        if not isinstance(row, dict):
            raise MatrixError("analysis receipt row must be an object")
        row_id = row.get("id")
        if row_id in seen:
            raise MatrixError(f"analysis receipt has duplicate row {row_id}")
        if row_id not in expected_rows:
            raise MatrixError(f"analysis receipt has unexpected row {row_id}")
        seen.add(row_id)
        _validate_row_receipt(row, expected_rows[row_id], candidate)
    missing = sorted(set(expected_rows) - seen)
    if missing:
        raise MatrixError("analysis receipt is missing rows: " + ", ".join(missing))
    return receipt


def verify_receipt(
    receipt_path: Path,
    manifest_path: Path = DEFAULT_MANIFEST,
    *,
    root: Path | None = None,
    expected_candidate: Mapping[str, str] | None = None,
) -> dict[str, Any]:
    """Read and verify a receipt against the manifest and exact candidate."""

    manifest = load_manifest(manifest_path)
    receipt = _read_json(receipt_path, "analysis receipt")
    if expected_candidate is None:
        if root is None:
            root = manifest_path.resolve().parents[1]
        expected_candidate = candidate_identity(root)
    return validate_receipt(receipt, manifest, expected_candidate)


def _host_platform() -> str:
    system = platform.system().lower()
    return {"darwin": "macos", "windows": "windows", "linux": "linux"}.get(system, "unsupported-host")


def _row_supported_on_host(row: Mapping[str, Any]) -> bool:
    """Return whether the runner can execute this row on the current host."""

    row_platform = row["platform"]
    host = _host_platform()
    if row_platform == "hosted":
        return host in {"macos", "windows", "linux"}
    if row_platform == "android-arm64":
        return host in {"macos", "windows", "linux"}
    return row_platform == host


def _expand_environment(command: Sequence[str], environment: Mapping[str, str]) -> list[str]:
    """Expand explicit environment placeholders used by hosted matrix rows."""

    expanded: list[str] = []
    for argument in command:
        def replace(match: re.Match[str]) -> str:
            name = match.group(1)
            value = environment.get(name)
            if not value:
                raise MatrixError(f"analysis command requires environment variable {name}")
            return value

        expanded.append(re.sub(r"\{env:([A-Z][A-Z0-9_]*)\}", replace, argument))
    return expanded


def resolve_row_environment(
    manifest_row: Mapping[str, Any],
    root: Path,
    inherited: Mapping[str, str],
) -> dict[str, str]:
    """Resolve manifest environment values against the checkout root and host environment."""

    environment = dict(inherited)
    environment["MC_ANALYSIS_ROOT"] = str(root.resolve())
    declared = _require_environment(
        manifest_row.get("environment"), f"manifest row {manifest_row['id']}.environment"
    )
    for name, value in declared.items():
        environment[name] = _expand_environment([value], environment)[0]
    return environment


def _bounded_text(value: str, limit: int = 16_384) -> str:
    if len(value) <= limit:
        return value
    return value[:limit] + "\n[output truncated by analysis-matrix]\n"


def _row_receipt(
    manifest_row: Mapping[str, Any],
    candidate: Mapping[str, str],
    status: str,
    tool_versions: Mapping[str, str],
    diagnostics: str,
    duration_seconds: float,
    effective_compile_flags: Mapping[str, Any] | None = None,
    executed_commands: Sequence[Mapping[str, Any]] = (),
    effective_analysis: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    return {
        "id": manifest_row["id"],
        "candidate": dict(candidate),
        "status": status,
        "flags": list(manifest_row["flags"]),
        "environment": _require_environment(manifest_row.get("environment"), f"manifest row {manifest_row['id']}.environment"),
        "tool_versions": dict(tool_versions),
        "commands": [list(command) for command in manifest_row["commands"]],
        "executed_commands": [dict(execution) for execution in executed_commands],
        "effective_compile_flags": dict(effective_compile_flags) if effective_compile_flags is not None else None,
        "effective_analysis": dict(effective_analysis) if effective_analysis is not None else None,
        "diagnostics": diagnostics,
        "diagnostics_sha256": hashlib.sha256(diagnostics.encode("utf-8")).hexdigest(),
        "duration_seconds": duration_seconds,
    }


def run_row(
    row_id: str,
    receipt_path: Path,
    manifest_path: Path = DEFAULT_MANIFEST,
    *,
    root: Path | None = None,
    expected_candidate: Mapping[str, str] | None = None,
) -> dict[str, Any]:
    """Execute one bounded matrix row and write its candidate-bound receipt."""

    manifest = load_manifest(manifest_path)
    if root is None:
        root = manifest_path.resolve().parents[1]
    if expected_candidate is None:
        expected_candidate = candidate_identity(root)
    candidate = _validate_candidate(expected_candidate, "expected_candidate")
    require_clean_checkout(root, candidate)
    rows = {row["id"]: row for row in manifest["rows"]}
    if row_id not in rows:
        raise MatrixError(f"unknown analysis matrix row: {row_id}")
    row = rows[row_id]
    started = time.monotonic()
    host = _host_platform()
    if not _row_supported_on_host(row):
        result = _row_receipt(
            row,
            candidate,
            "unavailable",
            {"runner": f"host={host}"},
            f"row requires {row['platform']}, but runner host is {host}",
            time.monotonic() - started,
        )
    else:
        versions: dict[str, str] = {}
        probe_output: list[str] = []
        unavailable: str | None = None
        for probe in row["tool_probes"]:
            expanded_probe = _expand_environment(probe, os.environ)
            try:
                completed = subprocess.run(
                    expanded_probe,
                    cwd=root,
                    text=True,
                    capture_output=True,
                    timeout=30,
                    check=False,
                )
            except (FileNotFoundError, subprocess.TimeoutExpired) as error:
                unavailable = f"tool probe {' '.join(expanded_probe)} unavailable: {error}"
                break
            output = (completed.stdout + completed.stderr).strip()
            if completed.returncode:
                unavailable = f"tool probe {' '.join(expanded_probe)} exited {completed.returncode}: {output}"
                break
            versions[probe[0]] = _bounded_text(output) or "reported no version text"
            probe_output.append(f"$ {' '.join(expanded_probe)}\n{_bounded_text(output)}")
        if unavailable is not None:
            result = _row_receipt(
                row,
                candidate,
                "unavailable",
                versions or {"runner": f"host={host}"},
                unavailable,
                time.monotonic() - started,
            )
        else:
            output_parts = list(probe_output)
            status = "passed"
            executed_commands: list[dict[str, Any]] = []
            command_outputs: list[str] = []
            command_environment = resolve_row_environment(row, root, os.environ)
            for command in row["commands"]:
                expanded_command = _expand_environment(command, command_environment)
                if row_id in REQUIRED_ANALYSIS_CONFIGURATION and expanded_command[:2] == ["cmake", "--build"]:
                    expanded_command.append("--verbose")
                remaining = row["timeout_seconds"] - (time.monotonic() - started)
                if remaining <= 0:
                    status = "failed"
                    output_parts.append("analysis row timeout elapsed before command execution")
                    break
                try:
                    completed = subprocess.run(
                        expanded_command,
                        cwd=root,
                        text=True,
                        capture_output=True,
                        timeout=remaining,
                        check=False,
                        env=command_environment,
                    )
                except subprocess.TimeoutExpired as error:
                    status = "failed"
                    partial_stdout = error.stdout
                    if isinstance(partial_stdout, bytes):
                        partial_stdout = partial_stdout.decode("utf-8", errors="replace")
                    elif partial_stdout is None:
                        partial_stdout = ""
                    partial_stderr = error.stderr
                    if isinstance(partial_stderr, bytes):
                        partial_stderr = partial_stderr.decode("utf-8", errors="replace")
                    elif partial_stderr is None:
                        partial_stderr = ""
                    output_parts.append(
                        f"$ {' '.join(expanded_command)}\n{error}\n{partial_stdout}{partial_stderr}"
                    )
                    break
                except FileNotFoundError as error:
                    status = "failed"
                    output_parts.append(f"$ {' '.join(expanded_command)}\n{error}")
                    break
                command_output = completed.stdout + completed.stderr
                diagnostic_output = command_output if completed.returncode else _bounded_text(command_output)
                output_parts.append(f"$ {' '.join(expanded_command)}\n{diagnostic_output}")
                command_outputs.append(command_output)
                executed_commands.append(
                    {
                        "declared_command": list(command),
                        "argv": list(expanded_command),
                        "return_code": completed.returncode,
                        "output_sha256": hashlib.sha256(command_output.encode("utf-8")).hexdigest(),
                        "output_bytes": len(command_output.encode("utf-8")),
                    }
                )
                if completed.returncode:
                    status = "failed"
                    break
            effective_compile_flags = None
            effective_analysis = None
            if status == "passed" and row.get("required_compile_flags") is not None:
                try:
                    effective_compile_flags = capture_effective_compile_flags(root, row)
                except (MatrixError, OSError) as error:
                    status = "failed"
                    output_parts.append(str(error))
            if status == "passed" and row_id in REQUIRED_ANALYSIS_CONFIGURATION:
                try:
                    effective_analysis = capture_effective_analysis(root, row, "\n".join(command_outputs))
                except (MatrixError, OSError) as error:
                    status = "failed"
                    output_parts.append(str(error))
            result = _row_receipt(
                row,
                candidate,
                status,
                versions,
                "\n\n".join(output_parts),
                time.monotonic() - started,
                effective_compile_flags,
                executed_commands,
                effective_analysis,
            )
    receipt = {
        "schema_version": SCHEMA_VERSION,
        "task_id": TASK_ID,
        "manifest_sha256": manifest_sha256(manifest),
        "candidate": candidate,
        "source": dict(candidate),
        "runner": {"name": "analysis-matrix", "host_platform": host, "row": row_id},
        "rows": [result],
    }
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    receipt_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return receipt


def merge_row_receipts(
    receipt_paths: Sequence[Path],
    output_path: Path,
    manifest_path: Path = DEFAULT_MANIFEST,
    *,
    root: Path | None = None,
    expected_candidate: Mapping[str, str] | None = None,
) -> dict[str, Any]:
    """Merge one passing candidate-bound receipt for every required row."""

    manifest = load_manifest(manifest_path)
    if root is None:
        root = manifest_path.resolve().parents[1]
    if expected_candidate is None:
        expected_candidate = candidate_identity(root)
    candidate = _validate_candidate(expected_candidate, "expected_candidate")
    expected_rows = {row["id"]: row for row in manifest["rows"]}
    merged_rows: dict[str, dict[str, Any]] = {}
    for receipt_path in receipt_paths:
        receipt = _read_json(receipt_path, "analysis row receipt")
        if not isinstance(receipt, dict):
            raise MatrixError(f"analysis row receipt must be an object: {receipt_path}")
        if receipt.get("schema_version") != SCHEMA_VERSION or receipt.get("task_id") != TASK_ID:
            raise MatrixError(f"analysis row receipt has incompatible schema or task: {receipt_path}")
        if receipt.get("manifest_sha256") != manifest_sha256(manifest):
            raise MatrixError(f"analysis row receipt has a different manifest: {receipt_path}")
        if _validate_candidate(receipt.get("candidate"), "row receipt candidate") != candidate:
            raise MatrixError(f"analysis row receipt is bound to a different candidate: {receipt_path}")
        if _validate_candidate(receipt.get("source"), "row receipt source") != candidate:
            raise MatrixError(f"analysis row receipt source differs from its candidate: {receipt_path}")
        rows = receipt.get("rows")
        if not isinstance(rows, list) or not rows:
            raise MatrixError(f"analysis row receipt has no rows: {receipt_path}")
        for row in rows:
            if not isinstance(row, dict) or row.get("id") not in expected_rows:
                raise MatrixError(f"analysis row receipt contains an unknown row: {receipt_path}")
            row_id = row["id"]
            if row_id in merged_rows:
                raise MatrixError(f"analysis row receipts duplicate {row_id}")
            _validate_row_receipt(row, expected_rows[row_id], candidate)
            merged_rows[row_id] = row
    receipt = {
        "schema_version": SCHEMA_VERSION,
        "task_id": TASK_ID,
        "manifest_sha256": manifest_sha256(manifest),
        "candidate": candidate,
        "source": dict(candidate),
        "runner": {"name": "analysis-matrix-merge", "rows": len(merged_rows)},
        "rows": [merged_rows[row["id"]] for row in manifest["rows"] if row["id"] in merged_rows],
    }
    validate_receipt(receipt, manifest, candidate)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return receipt


def run_local_stub(
    receipt_path: Path,
    manifest_path: Path = DEFAULT_MANIFEST,
    *,
    root: Path | None = None,
    expected_candidate: Mapping[str, str] | None = None,
) -> dict[str, Any]:
    """Write an explicit non-evidence receipt without executing analyses."""

    manifest = load_manifest(manifest_path)
    if root is None:
        root = manifest_path.resolve().parents[1]
    if expected_candidate is None:
        expected_candidate = candidate_identity(root)
    candidate = _validate_candidate(expected_candidate, "expected_candidate")
    host = _host_platform()
    rows = []
    for manifest_row in manifest["rows"]:
        reason = (
            f"local runner stub did not execute {manifest_row['id']}; "
            f"{host} capability probing is not release evidence"
        )
        rows.append(
            {
                "id": manifest_row["id"],
                "candidate": candidate,
                "status": "unavailable",
                "capability": {"host_platform": host, "available": False},
                "flags": list(manifest_row["flags"]),
                "environment": _require_environment(manifest_row.get("environment"), f"manifest row {manifest_row['id']}.environment"),
                "tool_versions": {"runner": "local-stub"},
                "commands": [list(command) for command in manifest_row["commands"]],
                "diagnostics": reason,
                "duration_seconds": 0,
            }
        )
    receipt = {
        "schema_version": SCHEMA_VERSION,
        "task_id": TASK_ID,
        "manifest_sha256": manifest_sha256(manifest),
        "candidate": candidate,
        "source": dict(candidate),
        "runner": {"name": "local-stub", "bounded": True, "host_platform": host},
        "rows": rows,
    }
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    receipt_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return receipt


def _candidate_from_args(root: Path, head: str | None, tree: str | None) -> dict[str, str] | None:
    if head is None and tree is None:
        return None
    if head is None or tree is None:
        raise MatrixError("--head and --tree must be supplied together")
    return _validate_candidate({"head": head, "tree": tree}, "candidate")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", nargs="?", choices=("verify", "run-local", "run-row", "merge"), default="verify")
    parser.add_argument("receipt_positional", nargs="?", type=Path)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--receipt", type=Path)
    parser.add_argument("--root", type=Path)
    parser.add_argument("--row", choices=REQUIRED_ROW_IDS, help="row to execute with run-row")
    parser.add_argument("--row-receipt", action="append", type=Path, default=[], help="row receipt to merge")
    parser.add_argument("--head", help="expected candidate HEAD SHA")
    parser.add_argument("--tree", help="expected candidate index-tree SHA")
    args = parser.parse_args(argv)
    root = (args.root or args.manifest.resolve().parents[1]).resolve()
    receipt_path = args.receipt or args.receipt_positional
    try:
        candidate = _candidate_from_args(root, args.head, args.tree)
        if args.command == "run-local":
            target = receipt_path or (root / DEFAULT_RECEIPT)
            run_local_stub(target, args.manifest, root=root, expected_candidate=candidate)
            print(f"WROTE analysis-matrix stub receipt: {target}")
            return 0
        if args.command == "run-row":
            if receipt_path is None:
                parser.error("run-row requires a receipt path (positional or --receipt)")
            if args.row is None:
                parser.error("run-row requires --row")
            result = run_row(args.row, receipt_path, args.manifest, root=root, expected_candidate=candidate)
            print(f"WROTE analysis-matrix row receipt: {receipt_path}")
            status = result["rows"][0]["status"]
            if status != "passed":
                raise MatrixError(f"analysis row {args.row} did not pass (status={status!r})")
            return 0
        if args.command == "merge":
            if receipt_path is None:
                parser.error("merge requires an output receipt path (positional or --receipt)")
            if not args.row_receipt:
                parser.error("merge requires at least one --row-receipt")
            merge_row_receipts(
                args.row_receipt, receipt_path, args.manifest, root=root, expected_candidate=candidate
            )
            print(f"WROTE complete analysis-matrix receipt: {receipt_path}")
            return 0
        if receipt_path is None:
            parser.error("verify requires a receipt path (positional or --receipt)")
        verify_receipt(receipt_path, args.manifest, root=root, expected_candidate=candidate)
        print(f"PASS analysis-matrix: {receipt_path}")
        return 0
    except MatrixError as error:
        print(f"FAIL analysis-matrix: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
