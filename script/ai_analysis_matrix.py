#!/usr/bin/env python3
"""Run, merge, and verify the fail-closed local S7 analysis receipt matrix."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import Any, Mapping, Sequence


TASK_ID = "MC-AI-0249"
SCHEMA_VERSION = 2
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
    "linux_lsan",
    "linux_msan",
    "clang_static_analysis",
    "clang_tidy",
    "iwyu",
    "cppcheck",
)
HOST_ROW_IDS = {
    "macos": (
        "macos_asan",
        "macos_ubsan",
        "macos_tsan",
        "clang_static_analysis",
        "clang_tidy",
        "iwyu",
        "cppcheck",
    ),
    "windows": ("windows_msvc_asan", "windows_msvc_analyze"),
    "linux": ("linux_lsan", "linux_msan"),
}
COMMON_INPUT_GLOBS = (
    "src/**",
    "tests/**",
    "testdata/**",
    "assets/**",
    "scenarios/**",
    "CMakeLists.txt",
    "cmake/**",
    "dependencies.lock.json",
    "vcpkg.json",
    "vcpkg-configuration.json",
    "vcpkg-configuration-*.json",
    "script/ci/acquire.py",
    "script/ci/build_snapshot.py",
    "script/ai_analysis_matrix.py",
)
INPUT_SCOPES = {
    "common": COMMON_INPUT_GLOBS,
    "macos_lsan": (
        "script/ci/verify_macos_lsan.py",
        "script/ci/macos_lsan.supp",
        "script/ci/macos_lsan_control.cpp",
    ),
    "windows_asan": (
        "script/ci/vcpkg-triplets/x64-windows-asan.cmake",
        "script/ci/vcpkg-overlays/gmp/**",
    ),
    "linux_lsan": (
        "script/ci/vcpkg-triplets/x64-linux-lsan.cmake",
        "script/ci/vcpkg-triplets/arm64-linux-lsan.cmake",
        "script/ci/vcpkg-clang-toolchain.cmake",
    ),
    "linux_msan": (
        "script/ci/vcpkg-triplets/x64-linux-msan.cmake",
        "script/ci/vcpkg-triplets/arm64-linux-msan.cmake",
        "script/ci/vcpkg-clang-toolchain.cmake",
        "script/ci/install_msan_libcxx.py",
        "script/ci/vcpkg-overlays/gmp/**",
    ),
    "clang_static": (),
    "clang_tidy": (),
    "iwyu": ("script/ci/iwyu_gate.py",),
    "cppcheck": ("script/ci/cppcheck_gate.py",),
}
ROW_INPUT_SCOPES = {
    "macos_asan": ("common", "macos_lsan"),
    "macos_ubsan": ("common",),
    "macos_tsan": ("common",),
    "windows_msvc_asan": ("common", "windows_asan"),
    "windows_msvc_analyze": ("common",),
    "linux_lsan": ("common", "linux_lsan"),
    "linux_msan": ("common", "linux_msan"),
    "clang_static_analysis": ("common", "clang_static"),
    "clang_tidy": ("common", "clang_tidy"),
    "iwyu": ("common", "iwyu"),
    "cppcheck": ("common", "cppcheck"),
}
REQUIRED_ROW_ENVIRONMENT = {
    "macos_asan": ("VCPKG_ROOT", "VCPKG_INSTALLED_DIR", "MC_PRIVATE_DEPENDENCIES_PREFIX"),
    "macos_ubsan": ("VCPKG_ROOT", "VCPKG_INSTALLED_DIR", "MC_PRIVATE_DEPENDENCIES_PREFIX"),
    "macos_tsan": ("VCPKG_ROOT", "VCPKG_INSTALLED_DIR", "MC_PRIVATE_DEPENDENCIES_PREFIX"),
    "windows_msvc_asan": ("VCPKG_ROOT", "VCPKG_INSTALLED_DIR", "MC_PRIVATE_DEPENDENCIES_PREFIX", "VULKAN_SDK"),
    "windows_msvc_analyze": ("VCPKG_ROOT", "VCPKG_INSTALLED_DIR", "MC_PRIVATE_DEPENDENCIES_PREFIX", "VULKAN_SDK"),
    "linux_lsan": (
        "VCPKG_ROOT",
        "VCPKG_INSTALLED_DIR",
        "MC_PRIVATE_DEPENDENCIES_PREFIX",
        "MC_VCPKG_CHAINLOAD_TOOLCHAIN_FILE",
        "MC_ANALYSIS_LINUX_LSAN_TRIPLET",
        "MC_LSAN_LIBCXX_PREFIX",
    ),
    "linux_msan": (
        "VCPKG_ROOT",
        "VCPKG_INSTALLED_DIR",
        "MC_PRIVATE_DEPENDENCIES_PREFIX",
        "MC_LLVM_PROJECT_ROOT",
        "MC_MSAN_LIBCXX_PREFIX",
        "MC_VCPKG_CHAINLOAD_TOOLCHAIN_FILE",
        "MC_ANALYSIS_LINUX_MSAN_TRIPLET",
        "LD_LIBRARY_PATH",
        "DEBUGINFOD_URLS",
    ),
    "clang_static_analysis": ("VCPKG_ROOT", "VCPKG_INSTALLED_DIR", "MC_PRIVATE_DEPENDENCIES_PREFIX"),
    "clang_tidy": ("VCPKG_ROOT", "VCPKG_INSTALLED_DIR", "MC_PRIVATE_DEPENDENCIES_PREFIX"),
    "iwyu": ("VCPKG_ROOT", "VCPKG_INSTALLED_DIR", "MC_PRIVATE_DEPENDENCIES_PREFIX"),
    "cppcheck": ("VCPKG_ROOT", "VCPKG_INSTALLED_DIR", "MC_PRIVATE_DEPENDENCIES_PREFIX"),
}
DEPENDENCY_ENVIRONMENT = {
    "macos_asan": ("VCPKG_ROOT", "VCPKG_INSTALLED_DIR", "MC_PRIVATE_DEPENDENCIES_PREFIX"),
    "macos_ubsan": ("VCPKG_ROOT", "VCPKG_INSTALLED_DIR", "MC_PRIVATE_DEPENDENCIES_PREFIX"),
    "macos_tsan": ("VCPKG_ROOT", "VCPKG_INSTALLED_DIR", "MC_PRIVATE_DEPENDENCIES_PREFIX"),
    "windows_msvc_asan": ("VCPKG_ROOT", "VCPKG_INSTALLED_DIR", "MC_PRIVATE_DEPENDENCIES_PREFIX"),
    "windows_msvc_analyze": ("VCPKG_ROOT", "VCPKG_INSTALLED_DIR", "MC_PRIVATE_DEPENDENCIES_PREFIX"),
    "linux_lsan": (
        "VCPKG_ROOT",
        "VCPKG_INSTALLED_DIR",
        "MC_PRIVATE_DEPENDENCIES_PREFIX",
        "MC_VCPKG_CHAINLOAD_TOOLCHAIN_FILE",
        "MC_LSAN_LIBCXX_PREFIX",
    ),
    "linux_msan": (
        "VCPKG_ROOT",
        "VCPKG_INSTALLED_DIR",
        "MC_PRIVATE_DEPENDENCIES_PREFIX",
        "MC_LLVM_PROJECT_ROOT",
        "MC_MSAN_LIBCXX_PREFIX",
        "MC_VCPKG_CHAINLOAD_TOOLCHAIN_FILE",
    ),
    "clang_static_analysis": ("VCPKG_ROOT", "VCPKG_INSTALLED_DIR", "MC_PRIVATE_DEPENDENCIES_PREFIX"),
    "clang_tidy": ("VCPKG_ROOT", "VCPKG_INSTALLED_DIR", "MC_PRIVATE_DEPENDENCIES_PREFIX"),
    "iwyu": ("VCPKG_ROOT", "VCPKG_INSTALLED_DIR", "MC_PRIVATE_DEPENDENCIES_PREFIX"),
    "cppcheck": ("VCPKG_ROOT", "VCPKG_INSTALLED_DIR", "MC_PRIVATE_DEPENDENCIES_PREFIX"),
}
IDENTITY_ENVIRONMENT = (
    "PATH",
    "CC",
    "CXX",
    "CFLAGS",
    "CXXFLAGS",
    "CPPFLAGS",
    "LDFLAGS",
    "CMAKE_GENERATOR",
    "CMAKE_PREFIX_PATH",
    "PKG_CONFIG_PATH",
    "SDKROOT",
    "MACOSX_DEPLOYMENT_TARGET",
    "VCPKG_ROOT",
    "VCPKG_INSTALLED_DIR",
    "VCPKG_DEFAULT_TRIPLET",
    "VCPKG_TARGET_TRIPLET",
    "VCPKG_OVERLAY_TRIPLETS",
    "LD_LIBRARY_PATH",
    "MC_PRIVATE_DEPENDENCIES_PREFIX",
    "MC_LLVM_PROJECT_ROOT",
    "MC_MSAN_LIBCXX_PREFIX",
    "MC_VCPKG_CHAINLOAD_TOOLCHAIN_FILE",
    "MC_ANALYSIS_LINUX_LSAN_TRIPLET",
    "MC_ANALYSIS_LINUX_MSAN_TRIPLET",
    "MC_LSAN_LIBCXX_PREFIX",
    "DEBUGINFOD_URLS",
    "INCLUDE",
    "LIB",
    "WindowsSdkDir",
    "VCINSTALLDIR",
    "VULKAN_SDK",
    "VisualStudioVersion",
    "WindowsSDKVersion",
)
RUNTIME_FAMILIES = {
    "macos_asan": ("asan_osx_dynamic", (".dylib",)),
    "macos_ubsan": ("ubsan_osx_dynamic", (".dylib",)),
    "macos_tsan": ("tsan_osx_dynamic", (".dylib",)),
    "linux_lsan": ("lsan", (".a",)),
    "linux_msan": ("msan", (".a",)),
}
_PLACEHOLDER_VERSIONS = {"", "unknown", "unavailable", "not-run", "stub"}
MSAN_LLVM_BASE_REVISION = "c13b7485b87909fcf739f62cfa382b55407433c0"
MSAN_HELPER_SHA256 = "92358b009a5f1effd1beb718a77ae3f9839d00595076d7bdfad4d7e19ceb994d"
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


def _host_path(value: str, platform_name: str) -> PureWindowsPath | PurePosixPath:
    return PureWindowsPath(value) if platform_name == "windows" else PurePosixPath(value)


def _same_host_path(left: str, right: str, platform_name: str) -> bool:
    return _host_path(left, platform_name) == _host_path(right, platform_name)


def _host_path_within(value: str, root: str, platform_name: str) -> bool:
    try:
        _host_path(value, platform_name).relative_to(_host_path(root, platform_name))
    except ValueError:
        return False
    return True


def _validate_selected_file(
    value: Any,
    field: str,
    platform_name: str,
    expected_path: str,
) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != {"name", "path", "size", "sha256"}:
        raise MatrixError(f"{field} is malformed")
    name = _require_string(value.get("name"), f"{field}.name")
    path = _require_string(value.get("path"), f"{field}.path")
    parsed_path = _host_path(path, platform_name)
    if not parsed_path.is_absolute() or parsed_path.name != name:
        raise MatrixError(f"{field}.path is not an absolute path for its selected file")
    if not _same_host_path(path, expected_path, platform_name):
        raise MatrixError(f"{field}.path does not match its selected CMake/environment path")
    size = value.get("size")
    digest = value.get("sha256")
    if not isinstance(size, int) or isinstance(size, bool) or size < 1:
        raise MatrixError(f"{field}.size is invalid")
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise MatrixError(f"{field}.sha256 is invalid")
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
        if name == "DEBUGINFOD_URLS" and setting == "":
            environment[name] = setting
        else:
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
        if row.get("platform") not in {"macos", "windows", "linux"}:
            raise MatrixError(f"manifest row {row_id}.platform must be macos, windows, or linux")
        if row.get("analysis_only", False) is not (row.get("platform") == "linux"):
            raise MatrixError(f"manifest row {row_id}.analysis_only must be true only for Linux analysis rows")
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
        row_environment = _require_environment(row.get("environment"), f"manifest row {row_id}.environment")
        if row_id == "linux_msan" and row_environment.get("DEBUGINFOD_URLS") != "":
            raise MatrixError("manifest row linux_msan must disable remote debuginfod lookup")
        if row_id != "linux_msan" and "DEBUGINFOD_URLS" in row_environment:
            raise MatrixError(f"manifest row {row_id} has unexpected DEBUGINFOD_URLS")
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
    declared_by_platform = {
        host: {row["id"] for row in rows if row["platform"] == host}
        for host in HOST_ROW_IDS
    }
    expected_by_platform = {host: set(row_ids) for host, row_ids in HOST_ROW_IDS.items()}
    if declared_by_platform != expected_by_platform:
        raise MatrixError("analysis manifest row platform ownership does not match the local host matrix")
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
    """Return immutable commit identity; staged and working files are row inputs."""

    head = _validate_sha(_git(root, "rev-parse", "--verify", "HEAD^{commit}"), "candidate.head")
    tree = _validate_sha(_git(root, "rev-parse", "--verify", "HEAD^{tree}"), "candidate.tree")
    return {"head": head, "tree": tree}


def commit_identity(root: Path, commit: str) -> dict[str, str]:
    """Resolve the immutable commit/tree pair independently of the current checkout HEAD."""

    head = _validate_sha(_git(root, "rev-parse", "--verify", f"{commit}^{{commit}}"), "candidate.head")
    if head != commit:
        raise MatrixError("candidate commit did not resolve to the requested immutable SHA")
    tree = _validate_sha(_git(root, "rev-parse", "--verify", f"{head}^{{tree}}"), "candidate.tree")
    return {"head": head, "tree": tree}


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
    """Require the requested commit identity while allowing scoped working-tree inputs."""

    actual = candidate_identity(root)
    if actual != dict(candidate):
        raise MatrixError("analysis proof candidate does not match the checkout HEAD commit")


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


def _input_file_paths(root: Path, patterns: Sequence[str]) -> dict[str, Path]:
    """Return the deterministic file inventory selected by relative globs."""

    root = root.resolve()
    paths: dict[str, Path] = {}
    for pattern in patterns:
        for file_path in root.glob(pattern):
            if not file_path.is_file():
                continue
            resolved = file_path.resolve()
            try:
                relative_name = file_path.relative_to(root).as_posix()
                resolved.relative_to(root)
            except ValueError as error:
                raise MatrixError(f"analysis input escapes the repository root: {file_path}") from error
            paths[relative_name] = resolved
    return paths


def _input_files_sha256(root: Path, patterns: Sequence[str]) -> str:
    paths = _input_file_paths(root, patterns)
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


def _preset_sha256(root: Path, manifest_row: Mapping[str, Any]) -> str:
    """Hash only the CMake configure/build presets used by this row and parents."""

    path = root / "CMakePresets.json"
    value = _read_json(path, "CMake presets")
    if not isinstance(value, dict):
        raise MatrixError("CMakePresets.json must be an object")
    configure_names: set[str] = set()
    build_names: set[str] = set()
    for command in manifest_row["commands"]:
        for index, part in enumerate(command[:-1]):
            if part == "--preset":
                target = build_names if "--build" in command[:index] else configure_names
                target.add(command[index + 1])

    payload: dict[str, Any] = {"version": value.get("version"), "presets": {}}
    for key in ("configurePresets", "buildPresets"):
        entries = value.get(key, [])
        if not isinstance(entries, list):
            raise MatrixError(f"CMakePresets.json {key} must be a list")
        by_name = {
            entry.get("name"): entry
            for entry in entries
            if isinstance(entry, dict) and isinstance(entry.get("name"), str)
        }

        def lineage(name: str, visiting: set[str]) -> list[dict[str, Any]]:
            if name in visiting:
                raise MatrixError(f"CMake preset inheritance cycle at {name}")
            entry = by_name.get(name)
            if entry is None:
                return []
            parents = entry.get("inherits", [])
            if isinstance(parents, str):
                parents = [parents]
            if not isinstance(parents, list) or any(not isinstance(parent, str) for parent in parents):
                raise MatrixError(f"CMake preset {name}.inherits is malformed")
            inherited: list[dict[str, Any]] = []
            for parent in parents:
                inherited.extend(lineage(parent, visiting | {name}))
            inherited.append(entry)
            return inherited

        names = configure_names if key == "configurePresets" else build_names
        selected = {name: lineage(name, set()) for name in sorted(names) if name in by_name}
        if names - selected.keys():
            kind = "configure" if key == "configurePresets" else "build"
            raise MatrixError(f"CMakePresets.json is missing a referenced {kind} preset")
        if key == "buildPresets":
            for name in build_names:
                build = by_name[name]
                configure = build.get("configurePreset")
                if not isinstance(configure, str):
                    raise MatrixError(f"CMake build preset {name} has no configurePreset")
                configure_names.add(configure)
        if key == "configurePresets" and configure_names - selected.keys():
            raise MatrixError("CMakePresets.json is missing a referenced configure preset")
        payload["presets"][key] = selected
    payload["cmakeMinimumRequired"] = value.get("cmakeMinimumRequired")
    return hashlib.sha256(_canonical_json(payload)).hexdigest()


def _row_configuration_sha256(manifest_row: Mapping[str, Any]) -> str:
    """Hash executable row settings without labels or unrelated matrix rows."""

    fields = (
        "id",
        "platform",
        "analysis_only",
        "flags",
        "compile_commands",
        "required_compile_flags",
        "environment",
        "tool_probes",
        "commands",
        "timeout_seconds",
    )
    config = {key: manifest_row[key] for key in fields if key in manifest_row}
    config["input_scopes"] = list(ROW_INPUT_SCOPES[manifest_row["id"]])
    return hashlib.sha256(_canonical_json(config)).hexdigest()


def row_source_input_sha256(root: Path, manifest_row: Mapping[str, Any]) -> str:
    """Hash this row's owned code, shared build inputs, helpers, and preset lineage."""

    row_id = manifest_row["id"]
    scopes = ROW_INPUT_SCOPES.get(row_id)
    if scopes is None:
        raise MatrixError(f"analysis row {row_id} has no input-scope definition")
    patterns = tuple(
        pattern
        for scope in scopes
        for pattern in INPUT_SCOPES[scope]
    )
    files_hash = _input_files_sha256(root, patterns)
    payload = {
        "row_configuration_sha256": _row_configuration_sha256(manifest_row),
        "input_files_sha256": files_hash,
        "cmake_presets_sha256": _preset_sha256(root, manifest_row),
    }
    return hashlib.sha256(_canonical_json(payload)).hexdigest()


def project_analysis_input_sha256(root: Path) -> str:
    """Hash shared code, dependency locks, and C++ analyzer policy for per-TU gates."""

    patterns = (
        *INPUT_SCOPES["common"],
        *INPUT_SCOPES["iwyu"],
        *INPUT_SCOPES["cppcheck"],
    )
    required_files = (
        "CMakePresets.json",
        "script/ai_analysis_matrix.py",
        "script/ci/iwyu_gate.py",
        "script/ci/cppcheck_gate.py",
    )
    missing = [name for name in required_files if not (root / name).is_file()]
    if missing:
        raise MatrixError("analysis input fingerprint is missing required files: " + ", ".join(missing))
    preset_digests = {
        row_id: _preset_sha256(
            root,
            {
                "commands": [
                    ["cmake", "--preset", preset],
                    ["cmake", "--build", "--preset", preset],
                ]
            },
        )
        for row_id, preset in (
            ("iwyu", "analysis-iwyu"),
            ("cppcheck", "analysis-cppcheck"),
        )
    }
    return hashlib.sha256(
        _canonical_json(
            {
                "input_files_sha256": _input_files_sha256(root, patterns),
                "analyzer_preset_lineages": preset_digests,
            }
        )
    ).hexdigest()


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


def _validate_input_identity(row: Mapping[str, Any], manifest_row: Mapping[str, Any]) -> dict[str, Any]:
    row_id = manifest_row["id"]
    identity = row.get("input_identity")
    if not isinstance(identity, dict):
        raise MatrixError(f"receipt row {row_id}.input_identity must be an object")
    expected_fields = {
        "identity_schema_version",
        "source_input_sha256",
        "row_configuration_sha256",
        "platform",
        "environment",
        "row_environment",
        "tool_identities",
        "effective_commands",
        "external_dependencies",
        "effective_cmake_cache",
        "runtime_resources",
        "toolchain_resources",
        "input_sha256",
    }
    if set(identity) != expected_fields or identity.get("identity_schema_version") != 4:
        raise MatrixError(f"receipt row {row_id}.input_identity has an unsupported schema")
    for key in ("source_input_sha256", "row_configuration_sha256", "input_sha256"):
        if not isinstance(identity.get(key), str) or not re.fullmatch(r"[0-9a-f]{64}", identity[key]):
            raise MatrixError(f"receipt row {row_id}.input_identity.{key} is invalid")
    if identity["row_configuration_sha256"] != _row_configuration_sha256(manifest_row):
        raise MatrixError(f"receipt row {row_id} configuration differs from this manifest")
    platform_identity = identity.get("platform")
    if not isinstance(platform_identity, dict) or set(platform_identity) != {"name", "system", "machine", "python"}:
        raise MatrixError(f"receipt row {row_id}.input_identity.platform is malformed")
    if platform_identity.get("name") != manifest_row["platform"]:
        raise MatrixError(f"receipt row {row_id} was produced on the wrong platform")
    for key, value in platform_identity.items():
        _require_string(value, f"receipt row {row_id}.input_identity.platform.{key}")
    system_prefixes = {
        "macos": ("macos", "darwin"),
        "windows": ("windows",),
        "linux": ("linux",),
    }[platform_identity["name"]]
    if not any(
        re.match(rf"^{prefix}(?:[-\s]|$)", platform_identity["system"], re.IGNORECASE)
        for prefix in system_prefixes
    ):
        raise MatrixError(f"receipt row {row_id}.input_identity.platform.system does not match its name")
    environment = identity.get("environment")
    if not isinstance(environment, dict) or set(environment) != set(IDENTITY_ENVIRONMENT) | {"MC_ANALYSIS_ROOT"}:
        raise MatrixError(f"receipt row {row_id}.input_identity.environment is incomplete")
    if any(not isinstance(value, str) for value in environment.values()):
        raise MatrixError(f"receipt row {row_id}.input_identity.environment has non-string values")
    row_environment = identity.get("row_environment")
    declared_environment = _require_environment(
        manifest_row.get("environment"), f"manifest row {row_id}.environment"
    )
    if not isinstance(row_environment, dict) or set(row_environment) != set(declared_environment):
        raise MatrixError(f"receipt row {row_id}.input_identity.row_environment is incomplete")
    if any(
        not isinstance(value, str)
        or (not value and not (row_id == "linux_msan" and name == "DEBUGINFOD_URLS"))
        for name, value in row_environment.items()
    ):
        raise MatrixError(f"receipt row {row_id}.input_identity.row_environment has invalid values")
    if row_id == "linux_msan" and row_environment.get("DEBUGINFOD_URLS") != "":
        raise MatrixError("receipt row linux_msan does not disable remote debuginfod lookup")
    if row_id == "linux_msan" and environment.get("DEBUGINFOD_URLS") != "":
        raise MatrixError("receipt row linux_msan input identity includes remote debuginfod lookup")
    tools = identity.get("tool_identities")
    if not isinstance(tools, list) or len(tools) != len(manifest_row["tool_probes"]):
        raise MatrixError(f"receipt row {row_id}.input_identity.tool_identities is incomplete")
    for index, tool in enumerate(tools):
        field = f"receipt row {row_id}.input_identity.tool_identities[{index}]"
        if not isinstance(tool, dict) or set(tool) != {
            "probe_argv",
            "executable",
            "executable_size",
            "executable_mtime_ns",
            "executable_sha256",
            "version_sha256",
        }:
            raise MatrixError(f"{field} is malformed")
        probe_environment = {**environment, **row_environment}
        expected_probe = _expand_environment(manifest_row["tool_probes"][index], probe_environment)
        if tool.get("probe_argv") != expected_probe:
            raise MatrixError(f"{field}.probe_argv does not match the manifest and captured environment")
        executable = _require_string(tool.get("executable"), f"{field}.executable")
        executable_path = PureWindowsPath(executable) if platform_identity["name"] == "windows" else Path(executable)
        if not executable_path.is_absolute():
            raise MatrixError(f"{field}.executable is not an absolute host path")
        for key in ("executable_size", "executable_mtime_ns"):
            value = tool.get(key)
            if not isinstance(value, int) or isinstance(value, bool) or value < (1 if key == "executable_size" else 0):
                raise MatrixError(f"{field}.{key} is invalid")
        for key in ("executable_sha256", "version_sha256"):
            value = tool.get(key)
            if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
                raise MatrixError(f"{field}.{key} is invalid")
    commands = identity.get("effective_commands")
    if not isinstance(commands, list) or len(commands) != len(manifest_row["commands"]):
        raise MatrixError(f"receipt row {row_id}.input_identity.effective_commands is incomplete")
    for index, command in enumerate(commands):
        _require_string_list(command, f"receipt row {row_id}.input_identity.effective_commands[{index}]")
    expected_commands = _effective_command_argv(
        manifest_row, {**environment, **row_environment}
    )
    if commands != expected_commands:
        raise MatrixError(f"receipt row {row_id}.input_identity.effective_commands do not match its captured environment")
    dependencies = identity.get("external_dependencies")
    if not isinstance(dependencies, dict):
        raise MatrixError(f"receipt row {row_id}.input_identity.external_dependencies is malformed")
    expected_dependencies = set(DEPENDENCY_ENVIRONMENT[row_id])
    if set(dependencies) != expected_dependencies:
        raise MatrixError(f"receipt row {row_id}.input_identity.external_dependencies is incomplete")
    for name, dependency in dependencies.items():
        field = f"receipt row {row_id}.input_identity.external_dependencies.{name}"
        if not isinstance(dependency, dict):
            raise MatrixError(f"{field} is malformed")
        dependency_path = _require_string(dependency.get("path"), f"{field}.path")
        captured_path = environment.get(name)
        if not captured_path or captured_path == "<unset>" or dependency_path != captured_path:
            raise MatrixError(f"{field}.path does not match its captured environment value")
        if name in {"VCPKG_ROOT", "MC_LLVM_PROJECT_ROOT"}:
            _validate_sha(dependency.get("revision"), f"{field}.revision")
            if name == "MC_LLVM_PROJECT_ROOT":
                if (
                    row_id != "linux_msan"
                    or dependency["revision"] != MSAN_LLVM_BASE_REVISION
                    or set(dependency) != {"path", "revision", "status", "helper_sha256"}
                    or dependency.get("status") != "clean"
                    or dependency.get("helper_sha256") != MSAN_HELPER_SHA256
                ):
                    raise MatrixError(f"{field} does not match the clean pinned upstream MSan runtime recipe")
        elif name == "MC_VCPKG_CHAINLOAD_TOOLCHAIN_FILE":
            digest = dependency.get("sha256")
            if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
                raise MatrixError(f"{field}.sha256 is invalid")
        else:
            hash_names = {
                "MC_PRIVATE_DEPENDENCIES_PREFIX": "consumed_files_sha256",
                "VCPKG_INSTALLED_DIR": "consumed_files_sha256",
                "MC_LSAN_LIBCXX_PREFIX": "files_sha256",
                "MC_MSAN_LIBCXX_PREFIX": "files_sha256",
            }
            hash_name = hash_names[name]
            if not isinstance(dependency.get(hash_name), str) or not re.fullmatch(r"[0-9a-f]{64}", dependency[hash_name]):
                raise MatrixError(f"{field}.{hash_name} is invalid")
            count = dependency.get("consumed_file_count", dependency.get("file_count"))
            if not isinstance(count, int) or isinstance(count, bool) or count < 1:
                raise MatrixError(f"{field} has no consumed dependency files")
        if name == "VCPKG_INSTALLED_DIR":
            digest = dependency.get("package_lists_sha256")
            if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
                raise MatrixError(f"{field}.package_lists_sha256 is invalid")
    cache = identity.get("effective_cmake_cache")
    if not isinstance(cache, dict) or cache.get("status") != "present" or not isinstance(cache.get("values"), dict):
        raise MatrixError(f"receipt row {row_id}.input_identity.effective_cmake_cache is missing")
    cache_values = cache["values"]
    if row_id in {"linux_lsan", "linux_msan"}:
        triplet_variable = (
            "MC_ANALYSIS_LINUX_LSAN_TRIPLET" if row_id == "linux_lsan" else "MC_ANALYSIS_LINUX_MSAN_TRIPLET"
        )
        triplet = environment.get(triplet_variable)
        if not triplet or cache_values.get("VCPKG_TARGET_TRIPLET") != triplet:
            raise MatrixError(f"receipt row {row_id} CMake cache does not use its explicit native triplet")
    toolchain_resources = identity.get("toolchain_resources")
    if not isinstance(toolchain_resources, dict) or set(toolchain_resources) != {"vulkan", "msvc_asan_runtime"}:
        raise MatrixError(f"receipt row {row_id}.input_identity.toolchain_resources is malformed")
    platform_name = manifest_row["platform"]
    if platform_name in {"macos", "windows"} and cache_values.get("MC_BUILD_CLIENT") != "ON":
        raise MatrixError(f"receipt row {row_id} CMake cache does not enable the client Vulkan dependencies")
    uses_vulkan = platform_name in {"macos", "windows"}
    vulkan = toolchain_resources.get("vulkan")
    if uses_vulkan:
        expected_vulkan_fields = {
            "sdk_root",
            "include_directory",
            "headers_sha256",
            "header_file_count",
            "library",
            "glslc",
            "spirv_val",
            "runtime_loader",
        }
        if not isinstance(vulkan, dict) or set(vulkan) != expected_vulkan_fields:
            raise MatrixError(f"receipt row {row_id} is missing selected Vulkan dependency provenance")
        sdk_root = vulkan.get("sdk_root")
        if not isinstance(sdk_root, str) or sdk_root != environment.get("VULKAN_SDK", "<unset>"):
            raise MatrixError(f"receipt row {row_id} Vulkan SDK identity differs from captured VULKAN_SDK")
        if sdk_root and sdk_root != "<unset>" and not _host_path(sdk_root, platform_name).is_absolute():
            raise MatrixError(f"receipt row {row_id} captured VULKAN_SDK is not an absolute path")
        selected_include = _require_string(
            vulkan.get("include_directory"), f"receipt row {row_id}.toolchain_resources.vulkan.include_directory"
        )
        include_path = _host_path(selected_include, platform_name)
        if not include_path.is_absolute() or not _same_host_path(
            selected_include, cache_values.get("Vulkan_INCLUDE_DIR", ""), platform_name
        ):
            raise MatrixError(f"receipt row {row_id} Vulkan headers do not match Vulkan_INCLUDE_DIR")
        header_count = vulkan.get("header_file_count")
        if not isinstance(header_count, int) or isinstance(header_count, bool) or header_count < 1:
            raise MatrixError(f"receipt row {row_id} Vulkan header file count is invalid")
        if not isinstance(vulkan.get("headers_sha256"), str) or not re.fullmatch(
            r"[0-9a-f]{64}", vulkan["headers_sha256"]
        ):
            raise MatrixError(f"receipt row {row_id} Vulkan headers hash is invalid")
        for field, cache_key in (
            ("library", "Vulkan_LIBRARY"),
            ("glslc", "Vulkan_GLSLC_EXECUTABLE"),
            ("spirv_val", "MC_SPIRV_VAL_EXECUTABLE"),
        ):
            selected_path = cache_values.get(cache_key)
            if not isinstance(selected_path, str) or not selected_path:
                raise MatrixError(f"receipt row {row_id} CMake cache lacks {cache_key}")
            _validate_selected_file(
                vulkan.get(field),
                f"receipt row {row_id}.toolchain_resources.vulkan.{field}",
                platform_name,
                selected_path,
            )
        runtime_loader = vulkan.get("runtime_loader")
        if platform_name == "windows":
            if not sdk_root or sdk_root == "<unset>" or not _host_path(sdk_root, platform_name).is_absolute():
                raise MatrixError(f"receipt row {row_id} requires an absolute VULKAN_SDK path")
            for cache_key in ("Vulkan_LIBRARY", "Vulkan_GLSLC_EXECUTABLE", "MC_SPIRV_VAL_EXECUTABLE"):
                if not _host_path_within(cache_values[cache_key], sdk_root, platform_name):
                    raise MatrixError(f"receipt row {row_id} selected {cache_key} escapes VULKAN_SDK")
            loader_path = str(_host_path(sdk_root, platform_name) / "Bin" / "vulkan-1.dll")
            _validate_selected_file(
                runtime_loader,
                f"receipt row {row_id}.toolchain_resources.vulkan.runtime_loader",
                platform_name,
                loader_path,
            )
        elif runtime_loader is not None:
            raise MatrixError(f"receipt row {row_id} has an unexpected Vulkan runtime loader")
    elif vulkan is not None:
        raise MatrixError(f"receipt row {row_id} has unexpected Vulkan dependency provenance")

    msvc_asan = toolchain_resources.get("msvc_asan_runtime")
    if row_id == "windows_msvc_asan":
        if cache_values.get("MC_SANITIZER") != "address":
            raise MatrixError("Windows MSVC ASan row cache does not select the address sanitizer")
        expected_msvc_fields = {"compiler", "linker", "vc_install_directory", "files"}
        if not isinstance(msvc_asan, dict) or set(msvc_asan) != expected_msvc_fields:
            raise MatrixError("Windows MSVC ASan row is missing its selected runtime DLL provenance")
        compiler = _require_string(msvc_asan.get("compiler"), "Windows MSVC ASan compiler")
        linker = _require_string(msvc_asan.get("linker"), "Windows MSVC ASan linker")
        vc_install = _require_string(msvc_asan.get("vc_install_directory"), "Windows VCINSTALLDIR")
        compiler_path = _host_path(compiler, "windows")
        linker_path = _host_path(linker, "windows")
        vc_path = _host_path(vc_install, "windows")
        if (
            compiler != cache_values.get("CMAKE_CXX_COMPILER")
            or linker != cache_values.get("CMAKE_LINKER")
            or vc_install != environment.get("VCINSTALLDIR")
            or not compiler_path.is_absolute()
            or compiler_path.name.lower() != "cl.exe"
            or not linker_path.is_absolute()
            or linker_path.name.lower() != "link.exe"
            or not vc_path.is_absolute()
            or not _host_path_within(compiler, vc_install, "windows")
            or not _host_path_within(linker, vc_install, "windows")
        ):
            raise MatrixError("Windows MSVC ASan runtime is not bound to its selected compiler/linker prefix")
        runtime_files = msvc_asan.get("files")
        if not isinstance(runtime_files, list) or len(runtime_files) != 1:
            raise MatrixError("Windows MSVC ASan row must identify exactly its selected dynamic runtime DLL")
        runtime_file = runtime_files[0]
        if not isinstance(runtime_file, dict) or set(runtime_file) != {
            "role", "name", "path", "size", "sha256"
        } or runtime_file.get("role") != "dynamic_runtime":
            raise MatrixError("Windows MSVC ASan runtime DLL identity is malformed")
        expected_runtime_path = str(linker_path.parent / "clang_rt.asan_dynamic-x86_64.dll")
        _validate_selected_file(
            {key: value for key, value in runtime_file.items() if key != "role"},
            "Windows MSVC ASan runtime DLL",
            "windows",
            expected_runtime_path,
        )
    elif msvc_asan is not None:
        raise MatrixError(f"receipt row {row_id} has unexpected MSVC ASan runtime provenance")
    runtime_identity = identity.get("runtime_resources")
    if not isinstance(runtime_identity, dict) or set(runtime_identity) != {
        "resource_directory",
        "files",
        "msan_ignorelist",
        "system_unwinder",
    }:
        raise MatrixError(f"receipt row {row_id}.input_identity.runtime_resources is malformed")
    family = RUNTIME_FAMILIES.get(row_id)
    resource_directory = runtime_identity.get("resource_directory")
    runtime_files = runtime_identity.get("files")
    if not isinstance(runtime_files, list):
        raise MatrixError(f"receipt row {row_id}.input_identity.runtime_resources.files is malformed")
    if family is None:
        if resource_directory != "not-applicable" or runtime_files:
            raise MatrixError(f"receipt row {row_id} has unexpected compiler runtime evidence")
    else:
        _require_string(resource_directory, f"receipt row {row_id}.input_identity.runtime_resources.resource_directory")
        if not Path(resource_directory).is_absolute() or not runtime_files:
            raise MatrixError(f"receipt row {row_id} is missing its selected sanitizer runtime")
        expected_families = [family[0], "msan_cxx"] if row_id == "linux_msan" else [family[0]]
        observed_families: list[str] = []
        for index, runtime in enumerate(runtime_files):
            field = f"receipt row {row_id}.input_identity.runtime_resources.files[{index}]"
            if not isinstance(runtime, dict) or set(runtime) != {"name", "path", "size", "sha256"}:
                raise MatrixError(f"{field} is malformed")
            name = _require_string(runtime.get("name"), f"{field}.name")
            path = _require_string(runtime.get("path"), f"{field}.path")
            if not Path(path).is_absolute() or Path(path).name != name:
                raise MatrixError(f"{field}.path is not an absolute path for its runtime name")
            try:
                Path(path).relative_to(Path(resource_directory))
            except ValueError as error:
                raise MatrixError(f"{field}.path escapes its compiler resource directory") from error
            size = runtime.get("size")
            digest = runtime.get("sha256")
            if not isinstance(size, int) or isinstance(size, bool) or size < 1:
                raise MatrixError(f"{field}.size is invalid")
            if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
                raise MatrixError(f"{field}.sha256 is invalid")
            match = re.fullmatch(r"libclang_rt\.([a-z0-9_]+)(?:-[A-Za-z0-9_]+)?\.(?:a|dylib)", name)
            if match is None:
                raise MatrixError(f"{field}.name is not a compiler-rt library")
            observed_families.append(match.group(1))
        if sorted(observed_families) != sorted(expected_families):
            raise MatrixError(f"receipt row {row_id} has incomplete or unexpected compiler runtime files")
    ignorelist = runtime_identity.get("msan_ignorelist")
    unwinder = runtime_identity.get("system_unwinder")
    if row_id == "linux_msan":
        if not isinstance(ignorelist, dict) or set(ignorelist) != {"name", "path", "size", "sha256"}:
            raise MatrixError(f"receipt row {row_id} is missing stock MSan ignorelist provenance")
        if ignorelist.get("name") != "msan_ignorelist.txt":
            raise MatrixError(f"receipt row {row_id} has an unexpected MSan ignorelist")
        ignorelist_path = _require_string(ignorelist.get("path"), f"receipt row {row_id}.msan_ignorelist.path")
        expected_ignorelist = str(Path(str(resource_directory)) / "share" / "msan_ignorelist.txt")
        if ignorelist_path != expected_ignorelist:
            raise MatrixError(f"receipt row {row_id} MSan ignorelist is not the selected compiler resource")
        if not Path(ignorelist_path).is_absolute():
            raise MatrixError(f"receipt row {row_id} MSan ignorelist path is not absolute")
        if not isinstance(ignorelist.get("size"), int) or isinstance(ignorelist.get("size"), bool) or ignorelist["size"] < 1:
            raise MatrixError(f"receipt row {row_id} MSan ignorelist size is invalid")
        if not isinstance(ignorelist.get("sha256"), str) or not re.fullmatch(r"[0-9a-f]{64}", ignorelist["sha256"]):
            raise MatrixError(f"receipt row {row_id} MSan ignorelist hash is invalid")
        expected_unwinder_fields = {
            "selected_by",
            "probe_path",
            "probe_sha256",
            "name",
            "path",
            "size",
            "sha256",
        }
        if not isinstance(unwinder, dict) or set(unwinder) != expected_unwinder_fields:
            raise MatrixError(f"receipt row {row_id} is missing selected system unwinder provenance")
        if unwinder.get("selected_by") != "ldd" or unwinder.get("name") != "libgcc_s.so.1":
            raise MatrixError(f"receipt row {row_id} did not select the system libgcc_s unwinder")
        unwinder_path = _require_string(unwinder.get("path"), f"receipt row {row_id}.system_unwinder.path")
        if not Path(unwinder_path).is_absolute() or Path(unwinder_path).name != "libgcc_s.so.1":
            raise MatrixError(f"receipt row {row_id} system unwinder path is invalid")
        if not isinstance(unwinder.get("size"), int) or isinstance(unwinder.get("size"), bool) or unwinder["size"] < 1:
            raise MatrixError(f"receipt row {row_id} system unwinder size is invalid")
        for key in ("sha256", "probe_sha256"):
            if not isinstance(unwinder.get(key), str) or not re.fullmatch(r"[0-9a-f]{64}", unwinder[key]):
                raise MatrixError(f"receipt row {row_id} system unwinder {key} is invalid")
        probe_path = _require_string(unwinder.get("probe_path"), f"receipt row {row_id}.system_unwinder.probe_path")
        prefix = Path(environment["MC_MSAN_LIBCXX_PREFIX"]).resolve()
        if not Path(probe_path).is_absolute() or Path(probe_path).name != "libc++abi.so.1" and not Path(probe_path).name.startswith("libc++abi.so.1."):
            raise MatrixError(f"receipt row {row_id} unwinder probe is not libc++abi")
        try:
            Path(probe_path).relative_to(prefix / "lib")
        except ValueError as error:
            raise MatrixError(f"receipt row {row_id} unwinder probe escapes the instrumented libc++ prefix") from error
    elif ignorelist is not None or unwinder is not None:
        raise MatrixError(f"receipt row {row_id} has unexpected MSan runtime provenance")
    preimage = dict(identity)
    digest = preimage.pop("input_sha256")
    if hashlib.sha256(_canonical_json(preimage)).hexdigest() != digest:
        raise MatrixError(f"receipt row {row_id}.input_identity hash does not match its contents")
    return identity


def _validate_row_receipt(
    row: Any,
    manifest_row: Mapping[str, Any],
    *,
    root: Path | None = None,
) -> None:
    row_id = manifest_row["id"]
    if not isinstance(row, dict):
        raise MatrixError(f"receipt row {row_id} must be an object")
    if row.get("id") != row_id:
        raise MatrixError(f"receipt row identity mismatch for {row_id}")
    _validate_candidate(row.get("proof_candidate"), f"receipt row {row_id}.proof_candidate")
    if row.get("proof_origin") != "local":
        raise MatrixError(f"receipt row {row_id} is not local proof")
    row_configuration_sha256 = row.get("row_configuration_sha256")
    if row_configuration_sha256 != _row_configuration_sha256(manifest_row):
        raise MatrixError(f"receipt row {row_id} configuration differs from this manifest")
    input_identity = _validate_input_identity(row, manifest_row)
    if root is not None:
        actual_source_sha256 = row_source_input_sha256(root, manifest_row)
        if input_identity["source_input_sha256"] != actual_source_sha256:
            raise MatrixError(f"analysis row {row_id} has stale source or configuration inputs")
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
        if argv != input_identity["effective_commands"][index]:
            raise MatrixError(f"{field}.argv does not match the recorded effective command")
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
    *,
    root: Path | None = None,
    expected_runner: str = "analysis-matrix-merge",
    expected_row_ids: Sequence[str] = REQUIRED_ROW_IDS,
) -> dict[str, Any]:
    """Validate one complete passing receipt, or raise ``MatrixError``."""

    checked_manifest = validate_manifest(manifest)
    if not isinstance(receipt, dict):
        raise MatrixError("analysis receipt must be a JSON object")
    if receipt.get("schema_version") != SCHEMA_VERSION:
        raise MatrixError(f"analysis receipt schema_version must be {SCHEMA_VERSION}")
    if receipt.get("task_id") != TASK_ID:
        raise MatrixError(f"analysis receipt task_id must be {TASK_ID}")
    runner = receipt.get("runner")
    if not isinstance(runner, dict) or runner.get("name") != expected_runner:
        raise MatrixError(f"analysis receipt runner must be {expected_runner}")
    manifest_hash = receipt.get("manifest_sha256")
    if not isinstance(manifest_hash, str) or not re.fullmatch(r"[0-9a-f]{64}", manifest_hash):
        raise MatrixError("analysis receipt manifest_sha256 is invalid")
    if expected_runner == "analysis-matrix-merge":
        if runner.get("rows") != len(REQUIRED_ROW_IDS):
            raise MatrixError("merged analysis receipt has an invalid row count")
    elif expected_runner in {"analysis-matrix-local", "analysis-matrix-row"}:
        host = runner.get("host_platform")
        if host not in HOST_ROW_IDS:
            raise MatrixError("local analysis receipt has an unsupported host platform")
        if expected_runner == "analysis-matrix-local":
            selected_rows = runner.get("selected_rows")
            if (
                not isinstance(selected_rows, list)
                or any(not isinstance(row_id, str) for row_id in selected_rows)
                or len(selected_rows) != len(set(selected_rows))
                or set(selected_rows) != set(expected_row_ids)
                or not set(expected_row_ids).issubset(HOST_ROW_IDS[host])
                or runner.get("rows") != len(expected_row_ids)
            ):
                raise MatrixError("local analysis receipt row scope does not match its host")
        if expected_runner == "analysis-matrix-row" and len(expected_row_ids) != 1:
            raise MatrixError("single-row receipt must declare one row")
        if any(
            checked_manifest_row["platform"] != host
            for checked_manifest_row in checked_manifest["rows"]
            if checked_manifest_row["id"] in expected_row_ids
        ):
            raise MatrixError("local analysis receipt contains a row owned by another host")
    else:
        raise MatrixError(f"unsupported analysis receipt runner: {expected_runner}")
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
    expected_rows = {row["id"]: row for row in checked_manifest["rows"] if row["id"] in expected_row_ids}
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
        _validate_row_receipt(row, expected_rows[row_id], root=root)
    missing = sorted(set(expected_rows) - seen)
    if missing:
        raise MatrixError("analysis receipt is missing rows: " + ", ".join(missing))
    if seen != set(expected_rows):
        raise MatrixError("analysis receipt row set does not match its declared runner scope")
    return receipt


def validate_local_receipt(
    receipt: Any,
    manifest: Mapping[str, Any],
    *,
    root: Path | None = None,
) -> dict[str, Any]:
    if not isinstance(receipt, dict) or not isinstance(receipt.get("runner"), dict):
        raise MatrixError("local analysis receipt is missing runner identity")
    runner = receipt["runner"]
    host = runner.get("host_platform")
    if host not in HOST_ROW_IDS:
        raise MatrixError("local analysis receipt has an unsupported host platform")
    name = runner.get("name")
    if name == "analysis-matrix-local":
        selected_rows = runner.get("selected_rows")
        if not isinstance(selected_rows, list) or not selected_rows:
            raise MatrixError("local analysis receipt has no selected rows")
        row_ids = tuple(selected_rows)
    elif name == "analysis-matrix-row":
        row_id = runner.get("row")
        if row_id not in HOST_ROW_IDS[host]:
            raise MatrixError("single-row receipt is not owned by its runner host")
        row_ids = (row_id,)
    else:
        raise MatrixError("receipt is not local analysis evidence")
    return validate_receipt(
        receipt,
        manifest,
        root=root,
        expected_runner=name,
        expected_row_ids=row_ids,
    )


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
    return validate_receipt(receipt, manifest, expected_candidate, root=root)


def _host_platform() -> str:
    system = platform.system().lower()
    return {"darwin": "macos", "windows": "windows", "linux": "linux"}.get(system, "unsupported-host")


def _capture_tool_probes(
    root: Path,
    manifest_row: Mapping[str, Any],
    environment: Mapping[str, str],
) -> tuple[dict[str, str], list[dict[str, Any]], str | None]:
    versions: dict[str, str] = {}
    identities: list[dict[str, Any]] = []
    for probe in manifest_row["tool_probes"]:
        expanded_probe = _expand_environment(probe, environment)
        try:
            completed = subprocess.run(
                expanded_probe,
                cwd=root,
                text=True,
                capture_output=True,
                timeout=30,
                check=False,
                env=environment,
            )
        except (FileNotFoundError, subprocess.TimeoutExpired) as error:
            return versions, identities, f"tool probe {' '.join(expanded_probe)} unavailable: {error}"
        output = completed.stdout + completed.stderr
        if completed.returncode:
            return (
                versions,
                identities,
                f"tool probe {' '.join(expanded_probe)} exited {completed.returncode}: {_bounded_text(output)}",
            )
        executable = shutil.which(expanded_probe[0], path=environment.get("PATH"))
        if executable is None and Path(expanded_probe[0]).is_file():
            executable = expanded_probe[0]
        if executable is None:
            return versions, identities, f"tool probe executable could not be resolved: {expanded_probe[0]}"
        executable_path = Path(executable).resolve()
        try:
            stat = executable_path.stat()
        except OSError as error:
            return versions, identities, f"tool executable is unreadable: {executable_path}: {error}"
        versions[probe[0]] = _bounded_text(output.strip()) or "reported no version text"
        identities.append(
            {
                "probe_argv": expanded_probe,
                "executable": str(executable_path),
                "executable_size": stat.st_size,
                "executable_mtime_ns": stat.st_mtime_ns,
                "executable_sha256": hashlib.sha256(executable_path.read_bytes()).hexdigest(),
                "version_sha256": hashlib.sha256(output.encode("utf-8")).hexdigest(),
            }
        )
    return versions, identities, None


def _effective_command_argv(manifest_row: Mapping[str, Any], environment: Mapping[str, str]) -> list[list[str]]:
    commands: list[list[str]] = []
    for command in manifest_row["commands"]:
        expanded = _expand_environment(command, environment)
        if (
            manifest_row["id"] in REQUIRED_ANALYSIS_CONFIGURATION
            and expanded[:2] == ["cmake", "--build"]
        ):
            expanded.append("--verbose")
        commands.append(expanded)
    return commands


def _git_dependency_identity(path_value: str, name: str) -> dict[str, str]:
    path = Path(path_value).resolve()
    if not path.is_dir():
        raise MatrixError(f"dependency root {name} is missing: {path}")
    revision = _git(path, "rev-parse", "--verify", "HEAD^{commit}")
    status = subprocess.run(
        ["git", "status", "--porcelain=v1", "--untracked-files=all"],
        cwd=path,
        text=True,
        capture_output=True,
        check=False,
    )
    if status.returncode:
        raise MatrixError(status.stderr.strip() or f"cannot inspect dependency root {name}")
    if status.stdout.strip():
        raise MatrixError(f"dependency root {name} is dirty: {path}")
    return {"path": path_value, "revision": _validate_sha(revision, f"{name}.revision")}


def _msan_llvm_dependency_identity(root: Path, path_value: str) -> dict[str, Any]:
    identity = _git_dependency_identity(path_value, "MC_LLVM_PROJECT_ROOT")
    revision = identity["revision"]
    if revision != MSAN_LLVM_BASE_REVISION:
        raise MatrixError("MemorySanitizer LLVM project root is not at the clean pinned llvmorg-18.1.3 revision")
    helper = root / "script" / "ci" / "install_msan_libcxx.py"
    if not helper.is_file() or hashlib.sha256(helper.read_bytes()).hexdigest() != MSAN_HELPER_SHA256:
        raise MatrixError("MemorySanitizer libc++ helper does not match its approved content hash")
    return {
        "path": path_value,
        "revision": revision,
        "status": "clean",
        "helper_sha256": MSAN_HELPER_SHA256,
    }


def _cmake_cache_identity(root: Path, manifest_row: Mapping[str, Any]) -> dict[str, Any]:
    preset_names = [
        command[index + 1]
        for command in manifest_row["commands"]
        for index, value in enumerate(command[:-1])
        if value == "--preset"
    ]
    configure_preset = next((name for name in preset_names if name.startswith("analysis-")), None)
    if configure_preset is None:
        return {"status": "not-applicable"}
    cache_path = root / "build" / configure_preset / "CMakeCache.txt"
    if not cache_path.is_file():
        return {"status": "missing"}
    selected: dict[str, str] = {}
    try:
        lines = cache_path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise MatrixError(f"cannot read effective CMake cache {cache_path}: {error}") from error
    for line in lines:
        if ":" not in line or "=" not in line:
            continue
        key, remainder = line.split(":", 1)
        if "=" not in remainder:
            continue
        value = remainder.split("=", 1)[1]
        if (
            key.startswith(("CoreCpp_", "CoreProject2026_", "VCPKG_", "MC_"))
            or key in {
                "CMAKE_BUILD_TYPE",
                "CMAKE_C_COMPILER",
                "CMAKE_CXX_COMPILER",
                "CMAKE_C_FLAGS",
                "CMAKE_CXX_FLAGS",
                "CMAKE_EXE_LINKER_FLAGS",
                "CMAKE_SHARED_LINKER_FLAGS",
                "CMAKE_GENERATOR",
                "CMAKE_MAKE_PROGRAM",
                "CMAKE_PREFIX_PATH",
                "CMAKE_TOOLCHAIN_FILE",
                "CMAKE_C_CLANG_TIDY",
                "CMAKE_CXX_CLANG_TIDY",
                "CMAKE_CXX_INCLUDE_WHAT_YOU_USE",
                "CMAKE_CXX_COMPILER_LAUNCHER",
                "CMAKE_CXX_CPPCHECK",
            }
            or key in {
                "Vulkan_INCLUDE_DIR",
                "Vulkan_LIBRARY",
                "Vulkan_GLSLC_EXECUTABLE",
            }
            or (manifest_row["id"] == "windows_msvc_asan" and key == "CMAKE_LINKER")
        ):
            selected[key] = value
    return {"status": "present", "values": selected}


def _selected_file_identity(path_value: str, field: str) -> dict[str, Any]:
    path = Path(path_value)
    if not path.is_absolute():
        raise MatrixError(f"selected {field} path is not absolute: {path_value}")
    try:
        resolved = path.resolve(strict=True)
        contents = resolved.read_bytes()
    except OSError as error:
        raise MatrixError(f"selected {field} is missing or unreadable: {path_value}: {error}") from error
    if not resolved.is_file():
        raise MatrixError(f"selected {field} is not a file: {path_value}")
    return {
        "name": path.name,
        "path": path_value,
        "size": len(contents),
        "sha256": hashlib.sha256(contents).hexdigest(),
    }


def _selected_toolchain_resources(
    manifest_row: Mapping[str, Any],
    environment: Mapping[str, str],
    cache_identity: Mapping[str, Any],
    *,
    require_selected: bool,
) -> dict[str, Any]:
    resources: dict[str, Any] = {"vulkan": None, "msvc_asan_runtime": None}
    if cache_identity.get("status") != "present":
        if require_selected and manifest_row["platform"] in {"macos", "windows"}:
            raise MatrixError(f"{manifest_row['id']} passed without a configured CMake cache")
        return resources
    cache_values = cache_identity.get("values", {})
    row_id = manifest_row["id"]
    platform_name = manifest_row["platform"]
    if platform_name in {"macos", "windows"}:
        if cache_values.get("MC_BUILD_CLIENT") != "ON":
            if require_selected:
                raise MatrixError(f"{row_id} CMake cache does not enable the client Vulkan dependencies")
            return resources
        include_directory = cache_values.get("Vulkan_INCLUDE_DIR")
        library = cache_values.get("Vulkan_LIBRARY")
        glslc = cache_values.get("Vulkan_GLSLC_EXECUTABLE")
        spirv_val = cache_values.get("MC_SPIRV_VAL_EXECUTABLE")
        selected = {
            "Vulkan_INCLUDE_DIR": include_directory,
            "Vulkan_LIBRARY": library,
            "Vulkan_GLSLC_EXECUTABLE": glslc,
            "MC_SPIRV_VAL_EXECUTABLE": spirv_val,
        }
        for key, value in selected.items():
            if not isinstance(value, str) or not value:
                if not require_selected:
                    return resources
                raise MatrixError(f"{row_id} CMake cache lacks selected Vulkan input {key}")
            if not Path(value).is_absolute():
                raise MatrixError(f"{row_id} selected Vulkan input {key} is not an absolute path")
        include_root = Path(include_directory)
        header_files = _input_file_paths(
            include_root,
            ("vulkan/*.h", "vulkan/**/*.h", "vk_video/*.h", "vk_video/**/*.h"),
        )
        if not header_files:
            raise MatrixError(f"{row_id} selected Vulkan include directory has no Vulkan headers: {include_root}")
        sdk_root = environment.get("VULKAN_SDK", "<unset>")
        if sdk_root and sdk_root != "<unset>" and not _host_path(sdk_root, platform_name).is_absolute():
            raise MatrixError(f"{row_id} captured VULKAN_SDK is not an absolute path")
        runtime_loader = None
        if platform_name == "windows":
            if not sdk_root or sdk_root == "<unset>" or not PureWindowsPath(sdk_root).is_absolute():
                raise MatrixError(f"{row_id} requires the selected VULKAN_SDK root")
            sdk_path = PureWindowsPath(sdk_root)
            for key in ("Vulkan_LIBRARY", "Vulkan_GLSLC_EXECUTABLE", "MC_SPIRV_VAL_EXECUTABLE"):
                try:
                    PureWindowsPath(selected[key]).relative_to(sdk_path)
                except ValueError as error:
                    raise MatrixError(f"{row_id} selected {key} escapes VULKAN_SDK") from error
            runtime_loader = _selected_file_identity(
                str(Path(sdk_root) / "Bin" / "vulkan-1.dll"), "Windows Vulkan runtime loader"
            )
        resources["vulkan"] = {
            "sdk_root": sdk_root,
            "include_directory": include_directory,
            "headers_sha256": _paths_sha256(header_files),
            "header_file_count": len(header_files),
            "library": _selected_file_identity(library, "Vulkan loader library"),
            "glslc": _selected_file_identity(glslc, "Vulkan glslc compiler"),
            "spirv_val": _selected_file_identity(spirv_val, "SPIR-V validator"),
            "runtime_loader": runtime_loader,
        }
    if row_id == "windows_msvc_asan":
        if cache_values.get("MC_SANITIZER") != "address":
            if not require_selected:
                return resources
            raise MatrixError("Windows MSVC ASan cache does not select the address sanitizer")
        compiler = cache_values.get("CMAKE_CXX_COMPILER")
        linker = cache_values.get("CMAKE_LINKER")
        vc_install = environment.get("VCINSTALLDIR", "<unset>")
        if not compiler or not linker or vc_install == "<unset>":
            if not require_selected:
                return resources
            raise MatrixError("Windows MSVC ASan cache lacks its selected compiler, linker, or VC prefix")
        compiler_path = PureWindowsPath(compiler)
        linker_path = PureWindowsPath(linker)
        vc_path = PureWindowsPath(vc_install)
        if (
            not compiler_path.is_absolute()
            or not linker_path.is_absolute()
            or not vc_path.is_absolute()
            or compiler_path.name.lower() != "cl.exe"
            or linker_path.name.lower() != "link.exe"
        ):
            raise MatrixError("Windows MSVC ASan compiler/linker paths are invalid")
        for selected_tool in (compiler_path, linker_path):
            try:
                selected_tool.relative_to(vc_path)
            except ValueError as error:
                raise MatrixError("Windows MSVC ASan compiler/linker escapes VCINSTALLDIR") from error
        runtime_directory = Path(linker).parent
        runtime = _selected_file_identity(
            str(runtime_directory / "clang_rt.asan_dynamic-x86_64.dll"),
            "MSVC dynamic ASan runtime",
        )
        resources["msvc_asan_runtime"] = {
            "compiler": compiler,
            "linker": linker,
            "vc_install_directory": vc_install,
            "files": [{"role": "dynamic_runtime", **runtime}],
        }
    return resources


def _dependency_path_selected(relative_path: str) -> bool:
    parts = Path(relative_path).parts
    if not parts:
        return False
    if re.fullmatch(
        r"(?:x86|x64|arm|arm64|arm64ec|wasm32)-(?:windows|linux|osx|android|uwp|mingw|emscripten)(?:-[A-Za-z0-9_]+)*",
        parts[0],
    ):
        parts = parts[1:]
    if parts and parts[0] in {"debug", "release"}:
        parts = parts[1:]
    if not parts:
        return False
    suffix = Path(parts[-1]).suffix.lower()
    if parts[0] in {"include", "lib", "bin"}:
        return True
    return parts[0] == "share" and (suffix == ".cmake" or "cmake" in parts[1:])


def _installed_dependency_files(root: Path, list_paths: Sequence[Path]) -> dict[str, Path]:
    files: dict[str, Path] = {}
    for list_path in list_paths:
        try:
            listed = list_path.read_text(encoding="utf-8").splitlines()
        except (OSError, UnicodeError) as error:
            raise MatrixError(f"cannot read vcpkg installed-file inventory {list_path}: {error}") from error
        for entry in listed:
            relative = entry.strip().removeprefix("./").replace("\\", "/")
            if not relative:
                continue
            relative_path = Path(relative)
            if relative_path.is_absolute() or ".." in relative_path.parts:
                raise MatrixError(f"vcpkg installed-file inventory escapes its prefix: {entry}")
            file_path = root / relative_path
            resolved = file_path.resolve()
            try:
                resolved.relative_to(root.resolve())
            except ValueError as error:
                raise MatrixError(f"vcpkg installed dependency file escapes its prefix: {file_path}") from error
            if relative.endswith("/") or file_path.is_dir():
                continue
            if not _dependency_path_selected(relative):
                continue
            if not file_path.is_file():
                raise MatrixError(f"vcpkg installed dependency file is missing: {file_path}")
            files[relative_path.as_posix()] = resolved
    if not files:
        raise MatrixError(f"vcpkg inventory contains no dependency headers, libraries, or CMake metadata: {root}")
    return files


def _paths_sha256(files: Mapping[str, Path]) -> str:
    digest = hashlib.sha256()
    for relative_name, file_path in sorted(files.items()):
        try:
            contents = file_path.read_bytes()
        except OSError as error:
            raise MatrixError(f"cannot read dependency input {file_path}: {error}") from error
        digest.update(relative_name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(len(contents).to_bytes(8, byteorder="big"))
        digest.update(contents)
    return digest.hexdigest()


def _resource_dir_override(
    manifest_row: Mapping[str, Any],
    environment: Mapping[str, str],
    cache_identity: Mapping[str, Any],
    effective_commands: Sequence[Sequence[str]],
) -> Path | None:
    values: list[str] = []
    for key in ("CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS"):
        value = environment.get(key, "")
        try:
            tokens = shlex.split(value, posix=_host_platform() != "windows")
        except ValueError as error:
            raise MatrixError(f"cannot parse {key} while identifying sanitizer runtime: {error}") from error
        for index, token in enumerate(tokens):
            if token == "-resource-dir" and index + 1 < len(tokens):
                values.append(tokens[index + 1])
            elif token.startswith("-resource-dir="):
                values.append(token.split("=", 1)[1])
    cache_values = cache_identity.get("values")
    if isinstance(cache_values, dict):
        for key, value in cache_values.items():
            if key not in {"CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS", "CMAKE_EXE_LINKER_FLAGS", "CMAKE_SHARED_LINKER_FLAGS"}:
                continue
            try:
                tokens = shlex.split(value, posix=_host_platform() != "windows")
            except ValueError as error:
                raise MatrixError(f"cannot parse effective {key} while identifying sanitizer runtime: {error}") from error
            for index, token in enumerate(tokens):
                if token == "-resource-dir" and index + 1 < len(tokens):
                    values.append(tokens[index + 1])
                elif token.startswith("-resource-dir="):
                    values.append(token.split("=", 1)[1])
    for command in effective_commands:
        for argument in command:
            if argument.startswith(("-DCMAKE_C_FLAGS=", "-DCMAKE_CXX_FLAGS=", "-DCMAKE_EXE_LINKER_FLAGS=", "-DCMAKE_SHARED_LINKER_FLAGS=")):
                try:
                    tokens = shlex.split(argument.split("=", 1)[1], posix=True)
                except ValueError as error:
                    raise MatrixError(f"cannot parse CMake resource-directory override: {error}") from error
                for index, token in enumerate(tokens):
                    if token == "-resource-dir" and index + 1 < len(tokens):
                        values.append(tokens[index + 1])
                    elif token.startswith("-resource-dir="):
                        values.append(token.split("=", 1)[1])
    distinct = {value for value in values if value}
    if len(distinct) > 1:
        raise MatrixError("analysis row has conflicting Clang -resource-dir overrides")
    if not distinct:
        return None
    selected = Path(next(iter(distinct)))
    if not selected.is_absolute():
        selected = Path(environment.get("MC_ANALYSIS_ROOT", ".")) / selected
    selected = selected.resolve()
    if not selected.is_dir():
        raise MatrixError(f"selected Clang resource directory is missing: {selected}")
    return selected


def _sanitizer_runtime_identity(
    root: Path,
    manifest_row: Mapping[str, Any],
    environment: Mapping[str, str],
    tool_identities: Sequence[Mapping[str, Any]],
    cache_identity: Mapping[str, Any],
    effective_commands: Sequence[Sequence[str]],
) -> dict[str, Any]:
    row_id = manifest_row["id"]
    family = RUNTIME_FAMILIES.get(row_id)
    if family is None:
        return {
            "resource_directory": "not-applicable",
            "files": [],
            "msan_ignorelist": None,
            "system_unwinder": None,
        }
    compiler = next(
        (
            str(tool["executable"])
            for tool in tool_identities
            if Path(str(tool.get("probe_argv", [""])[0])).name in {"clang", "clang++"}
        ),
        None,
    )
    if compiler is None:
        raise MatrixError(f"analysis row {row_id} has no Clang tool identity for its sanitizer runtime")
    resource_dir = _resource_dir_override(manifest_row, environment, cache_identity, effective_commands)
    if resource_dir is None:
        try:
            completed = subprocess.run(
                [compiler, "-print-resource-dir"],
                cwd=root,
                text=True,
                capture_output=True,
                timeout=30,
                check=False,
                env=environment,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise MatrixError(f"cannot locate selected Clang resource directory: {error}") from error
        if completed.returncode:
            raise MatrixError("Clang -print-resource-dir failed: " + _bounded_text(completed.stdout + completed.stderr))
        resource_dir = Path(completed.stdout.strip()).resolve()
    if not resource_dir.is_dir():
        raise MatrixError(f"selected Clang resource directory is missing: {resource_dir}")
    resource_subdir = "darwin" if manifest_row["platform"] == "macos" else "linux"
    runtime_dir = resource_dir / "lib" / resource_subdir
    family_name, suffixes = family
    candidates = sorted(
        path for path in runtime_dir.glob(f"libclang_rt.{family_name}*{suffixes[0]}") if path.is_file()
    )
    if row_id == "linux_msan":
        cxx_candidates = sorted(
            path for path in runtime_dir.glob("libclang_rt.msan_cxx*.a") if path.is_file()
        )
        selected = [candidates[0]] if candidates else []
        if cxx_candidates:
            selected.append(cxx_candidates[0])
        if not candidates or not cxx_candidates:
            raise MatrixError(f"selected Clang resource directory lacks the MSan runtime pair: {runtime_dir}")
    elif not candidates:
        raise MatrixError(f"selected Clang resource directory lacks libclang_rt.{family_name}: {runtime_dir}")
    else:
        selected = [candidates[0]]
    files = []
    for file_path in selected:
        resolved = file_path.resolve()
        try:
            resolved.relative_to(resource_dir.resolve())
            contents = resolved.read_bytes()
        except (ValueError, OSError) as error:
            raise MatrixError(f"cannot read selected sanitizer runtime {file_path}: {error}") from error
        files.append(
            {
                "name": resolved.name,
                "path": str(resolved),
                "size": len(contents),
                "sha256": hashlib.sha256(contents).hexdigest(),
            }
        )
    ignorelist_identity = None
    unwinder_identity = None
    if row_id == "linux_msan":
        ignorelist = resource_dir / "share" / "msan_ignorelist.txt"
        try:
            ignorelist_contents = ignorelist.read_bytes()
        except OSError as error:
            raise MatrixError(f"selected Clang resource directory lacks stock MSan ignorelist: {ignorelist}: {error}") from error
        ignorelist_identity = {
            "name": ignorelist.name,
            "path": str(ignorelist),
            "size": len(ignorelist_contents),
            "sha256": hashlib.sha256(ignorelist_contents).hexdigest(),
        }
        unwinder_identity = _msan_system_unwinder_identity(root, environment, tool_identities)
    return {
        "resource_directory": str(resource_dir.resolve()),
        "files": files,
        "msan_ignorelist": ignorelist_identity,
        "system_unwinder": unwinder_identity,
    }


def _msan_system_unwinder_identity(
    root: Path,
    environment: Mapping[str, str],
    tool_identities: Sequence[Mapping[str, Any]],
) -> dict[str, Any]:
    prefix = Path(environment["MC_MSAN_LIBCXX_PREFIX"]).resolve()
    probe = prefix / "lib" / "libc++abi.so.1"
    try:
        probe = probe.resolve(strict=True)
        probe_contents = probe.read_bytes()
    except OSError as error:
        raise MatrixError(f"instrumented libc++abi unwinder probe is missing or unreadable: {error}") from error
    if not probe.is_file():
        raise MatrixError(f"instrumented libc++abi unwinder probe is not a file: {probe}")
    try:
        probe.relative_to(prefix / "lib")
    except ValueError as error:
        raise MatrixError(f"instrumented libc++abi unwinder probe escapes its prefix: {probe}") from error
    ldd = next(
        (
            str(tool["executable"])
            for tool in tool_identities
            if Path(str(tool.get("probe_argv", [""])[0])).name == "ldd"
        ),
        None,
    )
    if ldd is None:
        raise MatrixError("MemorySanitizer row has no captured ldd tool identity")
    try:
        completed = subprocess.run(
            [ldd, str(probe)],
            cwd=root,
            text=True,
            capture_output=True,
            timeout=30,
            check=False,
            env=environment,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise MatrixError(f"cannot inspect selected libc++abi unwinder binding: {error}") from error
    output = completed.stdout + completed.stderr
    if completed.returncode:
        raise MatrixError("ldd failed for instrumented libc++abi: " + _bounded_text(output))
    if re.search(r"\blibunwind[^\s]*\.so(?:\.[0-9]+)*\b", output):
        raise MatrixError("instrumented libc++abi selected libunwind instead of the system unwinder")
    matches = re.findall(
        r"(?m)^\s*libgcc_s\.so\.1\s+=>\s+(/[^\s]+)(?:\s+\(0x[0-9a-fA-F]+\))?\s*$",
        output,
    )
    if len(matches) != 1:
        raise MatrixError("ldd did not resolve exactly one system libgcc_s.so.1 unwinder")
    unwinder = Path(matches[0]).resolve()
    try:
        contents = unwinder.read_bytes()
    except OSError as error:
        raise MatrixError(f"selected system libgcc_s unwinder is missing or unreadable: {error}") from error
    if not unwinder.is_file():
        raise MatrixError(f"selected system libgcc_s unwinder is not a file: {unwinder}")
    return {
        "selected_by": "ldd",
        "probe_path": str(probe),
        "probe_sha256": hashlib.sha256(probe_contents).hexdigest(),
        "name": "libgcc_s.so.1",
        "path": str(unwinder),
        "size": len(contents),
        "sha256": hashlib.sha256(contents).hexdigest(),
    }


def _external_dependency_identity(
    root: Path,
    manifest_row: Mapping[str, Any],
    environment: Mapping[str, str],
) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for name in REQUIRED_ROW_ENVIRONMENT[manifest_row["id"]]:
        if name not in environment or (not environment[name] and name != "DEBUGINFOD_URLS"):
            raise MatrixError(f"analysis row {manifest_row['id']} requires environment variable {name}")
    for name in ("VCPKG_ROOT", "MC_LLVM_PROJECT_ROOT"):
        value = environment.get(name)
        if value:
            if name == "MC_LLVM_PROJECT_ROOT" and manifest_row["id"] == "linux_msan":
                result[name] = _msan_llvm_dependency_identity(root, value)
            else:
                result[name] = _git_dependency_identity(value, name)
    private_prefix = environment.get("MC_PRIVATE_DEPENDENCIES_PREFIX")
    if private_prefix:
        prefix = Path(private_prefix).resolve()
        package_files = _input_file_paths(prefix, ("include/**", "lib/**", "bin/**", "share/**/*.cmake"))
        if not package_files:
            raise MatrixError(f"private dependency prefix has no headers, libraries, or CMake metadata: {prefix}")
        result["MC_PRIVATE_DEPENDENCIES_PREFIX"] = {
            "path": private_prefix,
            "consumed_files_sha256": _paths_sha256(package_files),
            "consumed_file_count": len(package_files),
        }
    installed = environment.get("VCPKG_INSTALLED_DIR")
    if installed:
        installed_root = Path(installed).resolve()
        inventories = sorted((installed_root / "vcpkg" / "info").glob("*.list"))
        if not inventories:
            raise MatrixError(f"vcpkg installed-file inventory is missing under {installed_root}")
        consumed_files = _installed_dependency_files(installed_root, inventories)
        result["VCPKG_INSTALLED_DIR"] = {
            "path": installed,
            "package_lists_sha256": _paths_sha256(
                {path.relative_to(installed_root).as_posix(): path for path in inventories}
            ),
            "consumed_files_sha256": _paths_sha256(consumed_files),
            "consumed_file_count": len(consumed_files),
        }
    for name, label in (
        ("MC_LSAN_LIBCXX_PREFIX", "LSan"),
        ("MC_MSAN_LIBCXX_PREFIX", "MSan"),
    ):
        prefix_value = environment.get(name)
        if not prefix_value:
            continue
        prefix = Path(prefix_value).resolve()
        patterns = (
            "include/c++/v1/**",
            "lib/libc++*",
            "lib/libc++abi*",
        )
        libcxx_files = _input_file_paths(prefix, patterns)
        if not libcxx_files:
            raise MatrixError(f"instrumented {label} libc++ prefix has no headers or runtime libraries: {prefix}")
        result[name] = {
            "path": prefix_value,
            "files_sha256": _paths_sha256(libcxx_files),
            "file_count": len(libcxx_files),
        }
    chainload = environment.get("MC_VCPKG_CHAINLOAD_TOOLCHAIN_FILE")
    if chainload:
        chainload_path = Path(chainload).resolve()
        if not chainload_path.is_file():
            raise MatrixError(f"vcpkg chainload toolchain file is missing: {chainload_path}")
        try:
            chainload_contents = chainload_path.read_bytes()
        except OSError as error:
            raise MatrixError(f"cannot read vcpkg chainload toolchain file {chainload_path}: {error}") from error
        result["MC_VCPKG_CHAINLOAD_TOOLCHAIN_FILE"] = {
            "path": chainload,
            "sha256": hashlib.sha256(chainload_contents).hexdigest(),
        }
    return result


def _row_input_identity(
    root: Path,
    manifest_row: Mapping[str, Any],
    environment: Mapping[str, str],
    tool_identities: Sequence[Mapping[str, Any]],
    *,
    require_toolchain_resources: bool = False,
) -> dict[str, Any]:
    effective_environment = {
        name: environment.get(name, "<unset>")
        for name in IDENTITY_ENVIRONMENT
    }
    effective_environment["MC_ANALYSIS_ROOT"] = environment.get("MC_ANALYSIS_ROOT", "<unset>")
    row_environment = _require_environment(
        manifest_row.get("environment"), f"manifest row {manifest_row['id']}.environment"
    )
    resolved_row_environment = {
        name: _expand_environment([value], environment)[0]
        for name, value in row_environment.items()
    }
    identity = {
        "identity_schema_version": 4,
        "source_input_sha256": row_source_input_sha256(root, manifest_row),
        "row_configuration_sha256": _row_configuration_sha256(manifest_row),
        "platform": {
            "name": _host_platform(),
            "system": platform.platform(),
            "machine": platform.machine(),
            "python": platform.python_version(),
        },
        "environment": effective_environment,
        "row_environment": resolved_row_environment,
        "tool_identities": [dict(item) for item in tool_identities],
        "effective_commands": _effective_command_argv(manifest_row, environment),
        "external_dependencies": _external_dependency_identity(root, manifest_row, environment),
        "effective_cmake_cache": _cmake_cache_identity(root, manifest_row),
    }
    if require_toolchain_resources:
        identity["toolchain_resources"] = _selected_toolchain_resources(
            manifest_row,
            environment,
            identity["effective_cmake_cache"],
            require_selected=True,
        )
    else:
        try:
            identity["toolchain_resources"] = _selected_toolchain_resources(
                manifest_row,
                environment,
                identity["effective_cmake_cache"],
                require_selected=False,
            )
        except MatrixError:
            identity["toolchain_resources"] = {"vulkan": None, "msvc_asan_runtime": None}
    identity["runtime_resources"] = _sanitizer_runtime_identity(
        root,
        manifest_row,
        environment,
        tool_identities,
        identity["effective_cmake_cache"],
        identity["effective_commands"],
    )
    identity["input_sha256"] = hashlib.sha256(_canonical_json(identity)).hexdigest()
    return identity


def _row_supported_on_host(row: Mapping[str, Any]) -> bool:
    """Return whether the runner can execute this row on the current host."""

    return row["platform"] == _host_platform()


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


def _terminate_process_tree(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        taskkill = shutil.which("taskkill")
        if taskkill is not None:
            subprocess.run(
                [taskkill, "/PID", str(process.pid), "/T", "/F"],
                text=True,
                capture_output=True,
                timeout=10,
                check=False,
            )
        else:
            process.kill()
    else:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            return
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def _run_with_liveness(
    command: Sequence[str],
    *,
    cwd: Path,
    environment: Mapping[str, str],
    timeout_seconds: float,
    row_id: str,
) -> subprocess.CompletedProcess[str]:
    """Run one proof command with bounded waits and useful output heartbeats."""

    started = time.monotonic()
    process = subprocess.Popen(
        list(command),
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=environment,
        start_new_session=os.name != "nt",
    )
    while True:
        remaining = timeout_seconds - (time.monotonic() - started)
        if remaining <= 0:
            _terminate_process_tree(process)
            output, _ = process.communicate()
            raise subprocess.TimeoutExpired(command, timeout_seconds, output=output)
        try:
            output, _ = process.communicate(timeout=min(30.0, remaining))
            return subprocess.CompletedProcess(list(command), process.returncode, output, "")
        except subprocess.TimeoutExpired as error:
            partial_output = error.output or ""
            if isinstance(partial_output, bytes):
                partial_output = partial_output.decode("utf-8", errors="replace")
            latest = next((line.strip() for line in reversed(partial_output.splitlines()) if line.strip()), "no output line observed yet")
            latest = latest[-240:]
            elapsed = int(time.monotonic() - started)
            print(
                f"STILL RUNNING {row_id}: {elapsed}s elapsed; latest output: {latest}",
                flush=True,
            )
            if time.monotonic() - started >= timeout_seconds:
                _terminate_process_tree(process)
                output, _ = process.communicate()
                raise subprocess.TimeoutExpired(command, timeout_seconds, output=output)


def _row_receipt(
    manifest_row: Mapping[str, Any],
    proof_candidate: Mapping[str, str],
    status: str,
    tool_versions: Mapping[str, str],
    diagnostics: str,
    duration_seconds: float,
    input_identity: Mapping[str, Any] | None = None,
    effective_compile_flags: Mapping[str, Any] | None = None,
    executed_commands: Sequence[Mapping[str, Any]] = (),
    effective_analysis: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    return {
        "id": manifest_row["id"],
        "proof_candidate": dict(proof_candidate),
        "proof_origin": "local",
        "row_configuration_sha256": _row_configuration_sha256(manifest_row),
        "input_identity": dict(input_identity) if input_identity is not None else None,
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
    reuse_row: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Execute one bounded local row, reusing only exact matching local proof."""

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
    input_identity: dict[str, Any] | None = None
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
        command_environment = resolve_row_environment(row, root, os.environ)
        missing_environment = sorted(
            name
            for name in REQUIRED_ROW_ENVIRONMENT[row_id]
            if name not in command_environment or (not command_environment[name] and name != "DEBUGINFOD_URLS")
        )
        versions, tool_identities, unavailable = ({}, [], None)
        if missing_environment:
            unavailable = "required analysis environment is unavailable: " + ", ".join(missing_environment)
        else:
            versions, tool_identities, unavailable = _capture_tool_probes(
                root, row, command_environment
            )
        probe_output = [
            f"$ {' '.join(identity['probe_argv'])}\n"
            f"{versions.get(identity['probe_argv'][0], 'tool version output recorded')}"
            for identity in tool_identities
        ]
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
            try:
                current_identity = _row_input_identity(root, row, command_environment, tool_identities)
            except (MatrixError, OSError) as error:
                current_identity = None
                unavailable = str(error)
            if unavailable is not None:
                result = _row_receipt(
                    row,
                    candidate,
                    "unavailable",
                    versions,
                    unavailable,
                    time.monotonic() - started,
                )
            reuse_matches = False
            if reuse_row is not None and unavailable is None:
                try:
                    _validate_row_receipt(reuse_row, row, root=root)
                    if reuse_row["input_identity"] == current_identity:
                        reuse_matches = True
                except MatrixError:
                    pass
            if reuse_matches:
                result = dict(reuse_row)
            elif unavailable is None:
                input_identity = current_identity
                output_parts = list(probe_output)
                status = "passed"
                executed_commands: list[dict[str, Any]] = []
                command_outputs: list[str] = []
                for expanded_command in current_identity["effective_commands"]:
                    command = row["commands"][len(executed_commands)]
                    remaining = row["timeout_seconds"] - (time.monotonic() - started)
                    if remaining <= 0:
                        status = "failed"
                        output_parts.append("analysis row timeout elapsed before command execution")
                        break
                    print(
                        f"RUN {row_id} command {len(executed_commands) + 1}/{len(row['commands'])}: "
                        f"{' '.join(expanded_command)} (timeout {int(remaining)}s)",
                        flush=True,
                    )
                    try:
                        completed = _run_with_liveness(
                            expanded_command,
                            cwd=root,
                            environment=command_environment,
                            timeout_seconds=remaining,
                            row_id=row_id,
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
                try:
                    input_identity = _row_input_identity(
                        root,
                        row,
                        command_environment,
                        tool_identities,
                        require_toolchain_resources=status == "passed",
                    )
                except (MatrixError, OSError) as error:
                    status = "failed"
                    output_parts.append(str(error))
                    input_identity = None
                result = _row_receipt(
                    row,
                    candidate,
                    status,
                    versions,
                    "\n\n".join(output_parts),
                    time.monotonic() - started,
                    input_identity,
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
        "runner": {"name": "analysis-matrix-row", "host_platform": host, "row": row_id},
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
    """Merge offline local receipts, refreshing the aggregate candidate binding."""

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
        runner = receipt.get("runner")
        if not isinstance(runner, dict):
            raise MatrixError(f"analysis row receipt has no runner identity: {receipt_path}")
        if runner.get("name") == "analysis-matrix-local":
            validate_local_receipt(receipt, manifest, root=root)
        elif runner.get("name") == "analysis-matrix-row":
            validate_local_receipt(receipt, manifest, root=root)
        else:
            raise MatrixError(f"analysis row receipt is not local evidence: {receipt_path}")
        rows = receipt.get("rows")
        if not isinstance(rows, list) or not rows:
            raise MatrixError(f"analysis row receipt has no rows: {receipt_path}")
        for row in rows:
            if not isinstance(row, dict) or row.get("id") not in expected_rows:
                raise MatrixError(f"analysis row receipt contains an unknown row: {receipt_path}")
            row_id = row["id"]
            if row_id in merged_rows:
                raise MatrixError(f"analysis row receipts duplicate {row_id}")
            _validate_row_receipt(row, expected_rows[row_id], root=root)
            merged_rows[row_id] = row
    missing = sorted(set(expected_rows) - set(merged_rows))
    if missing:
        raise MatrixError("analysis row receipts are missing rows: " + ", ".join(missing))
    receipt = {
        "schema_version": SCHEMA_VERSION,
        "task_id": TASK_ID,
        "manifest_sha256": manifest_sha256(manifest),
        "candidate": candidate,
        "source": dict(candidate),
        "runner": {"name": "analysis-matrix-merge", "rows": len(merged_rows)},
        "rows": [merged_rows[row["id"]] for row in manifest["rows"]],
    }
    validate_receipt(receipt, manifest, candidate, root=root)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return receipt


def run_local(
    receipt_path: Path,
    manifest_path: Path = DEFAULT_MANIFEST,
    *,
    root: Path | None = None,
    expected_candidate: Mapping[str, str] | None = None,
    reuse_receipt_path: Path | None = None,
    row_ids: Sequence[str] | None = None,
) -> dict[str, Any]:
    """Execute the rows owned by this host and write their local proof receipt."""

    manifest = load_manifest(manifest_path)
    if root is None:
        root = manifest_path.resolve().parents[1]
    if expected_candidate is None:
        expected_candidate = candidate_identity(root)
    candidate = _validate_candidate(expected_candidate, "expected_candidate")
    host = _host_platform()
    if host not in HOST_ROW_IDS:
        raise MatrixError(f"analysis matrix has no rows for host platform {host}")
    selected_rows = tuple(row_ids or HOST_ROW_IDS[host])
    if (
        not selected_rows
        or len(selected_rows) != len(set(selected_rows))
        or not set(selected_rows).issubset(HOST_ROW_IDS[host])
    ):
        raise MatrixError(f"run-local rows must be unique rows owned by {host}")
    reusable_rows: dict[str, Mapping[str, Any]] = {}
    if reuse_receipt_path is not None and reuse_receipt_path.is_file():
        previous = _read_json(reuse_receipt_path, "previous local analysis receipt")
        try:
            validate_local_receipt(previous, manifest, root=root)
            if previous["runner"].get("host_platform") == host:
                reusable_rows = {row["id"]: row for row in previous["rows"]}
        except (KeyError, MatrixError, TypeError):
            # Schema-1 hosted receipts and stale local receipts are never reused.
            reusable_rows = {}
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    rows: list[dict[str, Any]] = []
    with tempfile.TemporaryDirectory(prefix=".analysis-matrix-", dir=receipt_path.parent) as temporary_directory:
        for row_id in selected_rows:
            print(f"RUN analysis row {row_id} on {host}", flush=True)
            temporary_receipt = Path(temporary_directory) / f"{row_id}.json"
            result = run_row(
                row_id,
                temporary_receipt,
                manifest_path,
                root=root,
                expected_candidate=candidate,
                reuse_row=reusable_rows.get(row_id),
            )
            row = result["rows"][0]
            rows.append(row)
            print(f"{row_id}: {row['status']}", flush=True)
    receipt = {
        "schema_version": SCHEMA_VERSION,
        "task_id": TASK_ID,
        "manifest_sha256": manifest_sha256(manifest),
        "candidate": candidate,
        "source": dict(candidate),
        "runner": {
            "name": "analysis-matrix-local",
            "host_platform": host,
            "rows": len(rows),
            "selected_rows": list(selected_rows),
        },
        "rows": rows,
    }
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
    parser.add_argument("--reuse", type=Path, help="reuse rows from a prior local receipt only when all inputs match")
    parser.add_argument(
        "--row",
        choices=REQUIRED_ROW_IDS,
        action="append",
        help="row to execute (repeat for host-owned run-local rows; exactly one for run-row)",
    )
    parser.add_argument("--row-receipt", action="append", type=Path, default=[], help="row receipt to merge")
    parser.add_argument("--head", help="expected candidate HEAD SHA")
    parser.add_argument("--tree", help="expected immutable candidate commit tree SHA")
    args = parser.parse_args(argv)
    root = (args.root or args.manifest.resolve().parents[1]).resolve()
    receipt_path = args.receipt or args.receipt_positional
    try:
        candidate = _candidate_from_args(root, args.head, args.tree)
        if args.command == "run-local":
            target = receipt_path or (root / DEFAULT_RECEIPT)
            result = run_local(
                target,
                args.manifest,
                root=root,
                expected_candidate=candidate,
                reuse_receipt_path=args.reuse,
                row_ids=args.row,
            )
            print(f"WROTE local analysis-matrix receipt: {target}")
            failed = [row["id"] for row in result["rows"] if row.get("status") != "passed"]
            if failed:
                raise MatrixError("local analysis rows did not pass: " + ", ".join(failed))
            return 0
        if args.command == "run-row":
            if receipt_path is None:
                parser.error("run-row requires a receipt path (positional or --receipt)")
            if args.row is None or len(args.row) != 1:
                parser.error("run-row requires --row")
            row_id = args.row[0]
            result = run_row(row_id, receipt_path, args.manifest, root=root, expected_candidate=candidate)
            print(f"WROTE analysis-matrix row receipt: {receipt_path}")
            status = result["rows"][0]["status"]
            if status != "passed":
                raise MatrixError(f"analysis row {row_id} did not pass (status={status!r})")
            return 0
        if args.command == "merge":
            if receipt_path is None:
                parser.error("merge requires an output receipt path (positional or --receipt)")
            if not args.row_receipt:
                parser.error("merge requires at least one --row-receipt")
            merge_row_receipts(
                args.row_receipt, receipt_path, args.manifest, root=root, expected_candidate=candidate
            )
            print(f"WROTE complete local analysis-matrix receipt: {receipt_path}")
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
