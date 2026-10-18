#!/usr/bin/env python3
"""Capture and verify local Release full-radius gameplay qualification."""

from __future__ import annotations

import argparse
import base64
import datetime
import functools
import hashlib
import importlib.util
import json
import math
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import Any, Sequence


SCRIPT_ROOT = Path(__file__).resolve().parents[1]
_CHECK_SPEC = importlib.util.spec_from_file_location("gameplay_ai_check", SCRIPT_ROOT / "ai_check.py")
assert _CHECK_SPEC is not None and _CHECK_SPEC.loader is not None
_CHECK = importlib.util.module_from_spec(_CHECK_SPEC)
sys.modules[_CHECK_SPEC.name] = _CHECK
_CHECK_SPEC.loader.exec_module(_CHECK)
_SPEC = importlib.util.spec_from_file_location("ai_analysis_matrix", SCRIPT_ROOT / "ai_analysis_matrix.py")
assert _SPEC is not None and _SPEC.loader is not None
_MATRIX = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_MATRIX)
_PACKAGE_SPEC = importlib.util.spec_from_file_location("gameplay_package_snapshot", SCRIPT_ROOT / "package_snapshot.py")
assert _PACKAGE_SPEC is not None and _PACKAGE_SPEC.loader is not None
_PACKAGE = importlib.util.module_from_spec(_PACKAGE_SPEC)
sys.modules[_PACKAGE_SPEC.name] = _PACKAGE
_PACKAGE_SPEC.loader.exec_module(_PACKAGE)
SCHEMA = "mc.gameplay-proof.v2"
WORKLOAD = "speed-200-full-radius-recovery-v1"
SOURCE_INPUTS = ("src/**", "assets/**", "cmake/**", "scenarios/**", "CMakeLists.txt", "CMakePresets.json",
                 "dependencies.lock.json", "tests/**", "vcpkg.json", "vcpkg-configuration*.json")
REQUIRED_SOURCE_INPUTS = (
    "CMakeLists.txt",
    "CMakePresets.json",
    "dependencies.lock.json",
    "scenarios/world/canonical_world.core",
    "scenarios/world/height_tile.core",
)
PHASES = ("initial-full-radius-warmup", "baseline-stationary", "speed-200-movement",
          "full-radius-recovery-warmup", "recovered-stationary")
ACTUAL_PRESENTATION_CONTRACT = "mc.actual-presentation-timing.v1"
ACTUAL_PRESENTATION_PROVIDER = "VK_GOOGLE_display_timing"
ACTUAL_PRESENTATION_CLOCK_DOMAIN = "vulkan-display-timing-monotonic-nanoseconds-unspecified-origin"
TARGET_HZ = {"macos": 120, "windows": 120}
PROOFS = {"terrain-coverage": 256, "performance": 128}
PERFORMANCE_SAMPLE_MS = 30_000
PERFORMANCE_SAMPLE_OVERRUN_MS = 1_000
S7_CADENCE_EXCEPTION = "EarlyDev0.1.0-snapshot7"
S7_CADENCE_APPROVAL = "docs/ai/S7-CADENCE-EXCEPTION-2026-10-18.json"
S7_CADENCE_APPROVAL_SHA256 = "27be307193623673eb73fcf82f657fe474ef31fb03726f03db3f739bdce98d4e"
WORLD_TILE_COUNT = 4_096
QUEUE_FIELDS = ("pending_delivery_batches", "pending_residency_changes", "pending_mesh_queue",
                "active_mesh_jobs", "dirty_mesh_jobs", "worker_results", "worker_outstanding", "pending_removals")
DESKTOP_ROOT = Path(r"C:\ProgramData\CodexWindowsDesktop")
DESKTOP_HELPERS = {
    "windows-interactive-client.ps1": "51e02ae830f3cc984dd6331dca51a85b9713a0dd8545740201c0381819dd31a6",
    "windows-interactive-launch.ps1": "546fe65240c14a80f4aeed4f804b54bf90bb44473f7e2a9183ef1ec3120302c1",
}
DESKTOP_TERMINAL_STATES = ("succeeded", "failed", "timed_out", "cancelled")
DESKTOP_REQUEST_FIELDS = {"schema", "job_id", "task_id", "executable", "executable_sha256", "arguments", "timeout_seconds"}


class GameplayError(ValueError):
    """Gameplay evidence is missing, stale, or does not satisfy its contract."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GameplayError(message)


def integer(value: Any, field: str, minimum: int = 0) -> int:
    require(type(value) is int and value >= minimum, f"{field} must be an integer >= {minimum}")
    return value


def tile_center(value: Any, field: str) -> dict[str, int]:
    require(isinstance(value, dict) and set(value) == {"x", "y"}
            and all(type(coordinate) is int for coordinate in value.values()), f"{field}: center requires signed integer x/y")
    return value


def wrapped_tile_distance(first: dict[str, int], second: dict[str, int]) -> int:
    axis_distances = []
    for axis in ("x", "y"):
        difference = abs(first[axis] - second[axis]) % WORLD_TILE_COUNT
        axis_distances.append(min(difference, WORLD_TILE_COUNT - difference))
    return max(axis_distances)


@functools.lru_cache(maxsize=256)
def radial_band_counts(radius: int) -> tuple[int, ...]:
    integer(radius, "render radius", 1)
    require(radius <= 256, "render radius exceeds 256")
    counts = [0] * 16
    for x in range(-radius, radius + 1):
        limit = math.isqrt(radius * radius - x * x)
        for y in range(-limit, limit + 1):
            counts[min(max(abs(x), abs(y)) // 16, 15)] += 1
    return tuple(counts)


def verify_terrain_radial_scan(bands: Any, duration_ns: Any) -> None:
    duration = integer(duration_ns, "terrain spatial scan duration")
    require(isinstance(bands, list) and len(bands) == 16 and duration > 0,
            "terrain spatial scan must contain 16 timed radial bands")
    for band in bands:
        require(isinstance(band, dict), "terrain spatial radial band must be an object")
        desired = integer(band.get("desired"), "radial desired count")
        resident = integer(band.get("resident"), "radial resident count")
        nonresident = integer(band.get("nonresident"), "radial nonresident count")
        visible = integer(band.get("current_mesh_visible"), "radial current visible count")
        stale = integer(band.get("stale_or_missing_mesh"), "radial stale/missing count")
        require(desired == resident + nonresident and resident == visible + stale,
                "terrain spatial radial counts do not reconcile")


def read_json(target: Path) -> Any:
    return json.loads(target.read_text(encoding="utf-8-sig"), parse_constant=lambda value: (_ for _ in ()).throw(
        GameplayError(f"non-finite JSON value: {value}")))


def file_identity(target: Path) -> dict[str, Any]:
    require(target.is_file() and not target.is_symlink(), f"missing regular evidence file: {target}")
    data = target.read_bytes()
    return {"path": str(target.resolve()), "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def cadence_exception_approval(root: Path, selection: str | None) -> dict[str, Any] | None:
    if selection is None:
        return None
    require(selection == S7_CADENCE_EXCEPTION,
            "cadence exception is only authorized for EarlyDev0.1.0 snapshot7 macOS performance")
    _CHECK.require_s7_candidate(root)
    target = root / S7_CADENCE_APPROVAL
    identity = file_identity(target)
    require(identity["sha256"] == S7_CADENCE_APPROVAL_SHA256,
            "S7 cadence approval record is missing or altered")
    approval = read_json(target)
    require(approval["release"] == S7_CADENCE_EXCEPTION and approval["platform"] == "macos"
            and approval["proof"] == "performance" and approval["follow_up"] == "MC-AI-0472",
            "S7 cadence approval scope disagrees with the selected exception")
    return {"release": selection, "approval": identity, "follow_up": approval["follow_up"]}


def checked_file(descriptor: Any, base: Path) -> Path:
    require(isinstance(descriptor, dict), "file identity must be an object")
    target = base / descriptor["path"]
    identity = file_identity(target)
    require(all(identity[key] == descriptor.get(key) for key in ("size", "sha256")), f"altered evidence: {target}")
    return target


def regular_path(target: Path) -> None:
    for component in (target, *target.parents):
        metadata = component.lstat()
        require(not stat.S_ISLNK(metadata.st_mode) and not getattr(metadata, "st_file_attributes", 0)
                & stat.FILE_ATTRIBUTE_REPARSE_POINT, f"reparse point in gameplay transport path: {component}")


def path_entry_exists(target: Path) -> bool:
    try:
        target.lstat()
    except FileNotFoundError:
        return False
    return True


def relative_runtime_path(value: Any) -> str:
    require(isinstance(value, str) and value and not re.search(r"[:*?\x00]", value), "invalid relative runtime path")
    parts = value.replace("\\", "/").split("/")
    require(all(part not in ("", ".", "..") for part in parts), "runtime path contains traversal or empty segments")
    return "/".join(parts)


def desktop_package_identity(root: Path, executable: Path, package: Path, manifest: Path) -> list[dict[str, Any]]:
    regular_path(package)
    regular_path(manifest)
    entries = read_json(manifest)
    require(isinstance(entries, list) and entries, "desktop package manifest must list its runtime inputs")
    expected, identities = set(), []
    for entry in entries:
        require(isinstance(entry, dict) and set(entry) == {"relative", "size", "sha256"}, "invalid desktop package manifest entry")
        integer(entry["size"], "runtime input size")
        require(isinstance(entry["sha256"], str) and re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]) is not None,
                "runtime input SHA256 must be lowercase hexadecimal")
        relative = relative_runtime_path(entry["relative"])
        require(relative.casefold() not in expected, "duplicate desktop package input")
        expected.add(relative.casefold())
        parts = relative.split("/")
        require(relative == "mc_main.exe" or len(parts) == 1 and relative.endswith(".dll") or
                len(parts) == 2 and parts[0] == "shaders" and relative.endswith(".spv") or
                len(parts) > 1 and parts[0] == "assets", "desktop package contains a non-runtime input")
        original = root / relative if parts[0] == "assets" else executable.parent / relative
        regular_path(original)
        regular_path(package / relative)
        original_identity, package_identity = file_identity(original), file_identity(package / relative)
        require(all(original_identity[key] == package_identity[key] == entry[key] for key in ("size", "sha256")),
                f"desktop package input disagrees with original build/source runtime: {relative}")
        identities.append({"relative": relative, "original": original_identity, "package": package_identity})
    require("mc_main.exe" in expected, "desktop package is missing mc_main.exe")
    directories = {str(parent).replace("\\", "/").casefold() for entry in identities
                   for parent in Path(entry["relative"]).parents if str(parent) != "."}
    actual = set()
    for target in package.rglob("*"):
        regular_path(target)
        if target.is_file():
            actual.add(target.relative_to(package).as_posix().casefold())
        else:
            require(target.is_dir() and target.relative_to(package).as_posix().casefold() in directories,
                    "desktop package contains a non-runtime directory")
    require(actual == expected, "desktop package inventory differs from its manifest")
    return sorted(identities, key=lambda identity: identity["relative"])


def desktop_helper_identity() -> dict[str, Any]:
    identities = {}
    for name, digest in DESKTOP_HELPERS.items():
        target = DESKTOP_ROOT / "bin" / name
        regular_path(target)
        identities[name] = file_identity(target)
        require(identities[name]["sha256"] == digest, "installed Windows desktop helper differs from inspected contract")
    return identities


def macos_package_identity(package: Path, manifest: Path, executable: Path | None = None,
                           inspect_binaries: bool = False) -> list[dict[str, Any]]:
    regular_path(package)
    regular_path(manifest)
    value = _PACKAGE.strict_json(manifest.read_bytes(), "macOS package manifest")
    require(isinstance(value, dict) and value.get("schema") == 1 and value.get("platform") == "macos", "invalid macOS package manifest")
    require(value.get("runtime") == {"launcher": "MinecraftClone.command", "vulkan_loader": "lib/libvulkan.1.dylib",
            "moltenvk": "lib/libMoltenVK.dylib", "icd": "vulkan/icd.d/MoltenVK_icd.json"}, "incorrect macOS runtime manifest")
    entries = value.get("files")
    require(isinstance(entries, list) and entries, "missing macOS package inventory")
    identities, expected = [], set()
    for entry in entries:
        require(isinstance(entry, dict) and set(entry) == {"path", "bytes", "sha256"}, "invalid macOS inventory entry")
        relative = relative_runtime_path(entry["path"])
        require(relative == entry["path"] and relative not in expected and relative != "manifest.json", "duplicate or invalid macOS package path")
        expected.add(relative)
        target = package / relative
        regular_path(target)
        identity = file_identity(target)
        require(identity["size"] == integer(entry["bytes"], "macOS runtime bytes")
                and identity["sha256"] == entry["sha256"], f"macOS package input changed: {relative}")
        identities.append({"relative": relative, "file": identity})
    required = {"mc_main", "MinecraftClone.command", "lib/libvulkan.1.dylib", "lib/libMoltenVK.dylib", "vulkan/icd.d/MoltenVK_icd.json"}
    require(required <= expected, "macOS package is missing its normal runtime")
    regular_path(package / "manifest.json")
    require((package / "manifest.json").read_bytes() == manifest.read_bytes(), "macOS package manifest differs from supplied manifest")
    expected.add("manifest.json")
    directories = {str(parent) for relative in expected for parent in PurePosixPath(relative).parents if str(parent) != "."}
    actual = set()
    for target in package.rglob("*"):
        regular_path(target)
        relative = target.relative_to(package).as_posix()
        if target.is_file():
            actual.add(relative)
        else:
            require(target.is_dir() and relative in directories, "unlisted macOS package directory")
    require(actual == expected, "unlisted or missing macOS package input")
    launcher = package / "MinecraftClone.command"
    require(launcher.read_bytes() == _PACKAGE.macos_launcher() and os.access(launcher, os.X_OK), "macOS launcher differs from normal production launcher")
    icd = package / "vulkan/icd.d/MoltenVK_icd.json"
    require(icd.read_bytes() == _PACKAGE.sanitized_icd(icd), "macOS ICD must reference exactly the bundled MoltenVK")
    if executable is not None:
        require(all(file_identity(package / "mc_main")[key] == file_identity(executable)[key] for key in ("sha256", "size")),
                "macOS package executable differs from the source Release executable")
    if inspect_binaries:
        binaries = [(package / "mc_main", PurePosixPath("mc_main"))]
        binaries.extend((package / relative, PurePosixPath(relative)) for relative in sorted(expected) if relative.startswith("lib/") and relative.endswith(".dylib"))
        _PACKAGE.validate_macos_package(binaries)
    identities.append({"relative": "manifest.json", "file": file_identity(package / "manifest.json")})
    return sorted(identities, key=lambda entry: entry["relative"])


def verify_macos_runtime(row: dict[str, Any], base: Path) -> None:
    runtime = row.get("macos_runtime")
    require(isinstance(runtime, dict) and runtime.get("kind") == "normal-macos-package-v1", "Mac gameplay proof requires its normal packaged runtime")
    require(isinstance(row.get("native_report_path"), str) and Path(row["native_report_path"]).is_absolute(), "actual capture command requires an absolute Mac native report path")
    require(runtime["original_before"] == runtime["original_after"] and runtime["retained_before"] == runtime["retained_after"],
            "macOS runtime inputs changed during capture")
    require(runtime["loaded_libraries"] == "not-observed; closure and normal launcher verified, PID mappings unavailable",
            "unsupported macOS loaded-library evidence claim")
    retained = runtime["retained_before"]
    require(isinstance(retained, list) and retained, "missing retained macOS runtime")
    package = checked_file(next(entry["file"] for entry in retained if entry["relative"] == "mc_main"), base).parent
    manifest = checked_file(runtime["manifest"], base)
    current = macos_package_identity(package, manifest)
    require(current == retained, "retained macOS runtime identity mismatch")
    original = {entry["relative"]: entry["file"] for entry in runtime["original_before"]}
    require(set(original) == {entry["relative"] for entry in current} and all(
        all(original[entry["relative"]][key] == entry["file"][key] for key in ("size", "sha256")) for entry in current),
        "macOS retained closure differs from original runtime")
    require(all(file_identity(package / "mc_main")[key] == row["executable"][key] for key in ("size", "sha256")), "macOS runtime executable identity mismatch")
    require(runtime["vulkan_loader"] == file_identity(package / "lib/libvulkan.1.dylib")
            and runtime["moltenvk"] == file_identity(package / "lib/libMoltenVK.dylib"), "macOS Vulkan library identities disagree")
    require(Path(runtime["capture_package"]).is_absolute() and row.get("actual_command") == [str(Path(runtime["capture_package"]) / "MinecraftClone.command"), *row["command"][1:-1], row["native_report_path"]],
            "actual capture command did not use the normal packaged Mac launcher")


def desktop_copy_identity(app: Path, inputs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    copied = []
    for entry in inputs:
        target = app / entry["relative"]
        regular_path(target)
        copied.append({"relative": entry["relative"], "file": file_identity(target)})
    return copied


def desktop_client(operation: str, client_timeout: float = 30, **parameters: Any) -> dict[str, Any]:
    require(operation in ("Submit", "Status", "Cancel"), "invalid desktop operation")
    payload = base64.b64encode(json.dumps(parameters, ensure_ascii=False).encode("utf-8")).decode("ascii")
    command = ("$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; "
               "[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); "
               f"$p=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('{payload}')) | ConvertFrom-Json; "
               f"& '{str(DESKTOP_ROOT / 'bin/windows-interactive-client.ps1').replace(chr(39), chr(39) * 2)}' -Operation {operation} ")
    command += ("-PackageDirectory $p.package -Executable mc_main.exe -Arguments ([string[]]@($p.arguments)) "
                "-TimeoutSeconds $p.timeout_seconds -TaskId $p.task_id" if operation == "Submit" else "-JobId $p.job_id")
    encoded = base64.b64encode(command.encode("utf-16-le")).decode("ascii")
    powershell = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    result = subprocess.run([str(powershell), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-EncodedCommand", encoded],
                            capture_output=True, text=True, encoding="utf-8", timeout=client_timeout)
    require(result.returncode == 0 or operation == "Submit", f"desktop {operation} failed: {result.stderr.strip()}")
    response = json.loads(result.stdout)
    require(isinstance(response, dict), f"desktop {operation} returned a non-object status")
    return response


def desktop_job_id(response: Any) -> str:
    require(isinstance(response, dict) and type(response.get("schema")) is int and response["schema"] == 1,
            "invalid desktop helper schema")
    job_id = response.get("job_id")
    require(isinstance(job_id, str) and re.fullmatch(r"[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}", job_id) is not None,
            "desktop helper requires a canonical job UUID")
    return job_id


def desktop_utc_timestamp(value: Any, field: str) -> datetime.datetime:
    require(isinstance(value, str), f"native desktop {field} must be an ISO timestamp")
    try:
        parsed = datetime.datetime.fromisoformat(value)
    except ValueError as error:
        raise GameplayError(f"native desktop {field} must be an ISO timestamp") from error
    require(parsed.utcoffset() == datetime.timedelta(0), f"native desktop {field} must be UTC")
    return parsed


def desktop_status(response: Any, job_id: str, task_id: str, digest: str, timeout: int) -> bool:
    require(desktop_job_id(response) == job_id and response.get("task_id") == task_id, "desktop status job/task identity mismatch")
    state = response.get("state")
    require(state in ("submitted", "running", *DESKTOP_TERMINAL_STATES), "unknown desktop status state")
    if state == "submitted":
        require(set(response) == {"schema", "job_id", "task_id", "state"}, "malformed submitted desktop status")
        return False
    fields = {"schema", "job_id", "task_id", "state", "started_at_utc", "pid", "session_id", "executable",
              "executable_sha256", "timeout_seconds", "exit_code", "error"}
    require(set(response) == fields | ({"finished_at_utc"} if state in DESKTOP_TERMINAL_STATES else set()),
            "malformed native desktop status")
    require(response.get("executable") == "mc_main.exe" and response.get("executable_sha256") == digest
            and type(response.get("timeout_seconds")) is int and response["timeout_seconds"] == timeout,
            "desktop status executable/hash/timeout mismatch")
    started = desktop_utc_timestamp(response.get("started_at_utc"), "start time")
    if state != "running" or response.get("pid") is not None or response.get("session_id") is not None:
        integer(response.get("pid"), "native desktop PID", 1)
        integer(response.get("session_id"), "native desktop session", 1)
    else:
        require(response.get("pid") is None and response.get("session_id") is None,
                "malformed running desktop process identity")
    require(response.get("error") is None or isinstance(response.get("error"), str), "malformed native desktop error")
    if state in DESKTOP_TERMINAL_STATES:
        finished = desktop_utc_timestamp(response.get("finished_at_utc"), "finish time")
        require(started <= finished, "native desktop finish time precedes start time")
        require(type(response.get("exit_code")) is int, "malformed native desktop exit code")
        require(state != "succeeded" or response["exit_code"] == 0 and response.get("error") is None,
                "desktop success disagrees with native exit/error")
    else:
        require(response.get("exit_code") is None and response.get("error") is None,
                "running desktop status contains terminal result fields")
    return state in DESKTOP_TERMINAL_STATES


def desktop_startup_failure(response: Any, job_id: str, task_id: str) -> bool:
    fields = {"schema", "job_id", "task_id", "state", "submitted_utc", "finished_utc", "pid", "session_id", "exit_code", "error"}
    if not isinstance(response, dict) or set(response) != fields or response.get("state") != "failed":
        return False
    require(desktop_job_id(response) == job_id and response.get("task_id") == task_id,
            "desktop startup failure job/task identity mismatch")
    require(response["pid"] is None and response["session_id"] is None and response["exit_code"] is None
            and isinstance(response["error"], str) and bool(response["error"].strip()),
            "malformed desktop startup failure")
    submitted = desktop_utc_timestamp(response["submitted_utc"], "submission time")
    finished = desktop_utc_timestamp(response["finished_utc"], "finish time")
    require(submitted <= finished, "desktop startup failure finish time precedes submission")
    return True


def transport_file(target: Path, base: Path) -> dict[str, Any]:
    identity = file_identity(target)
    identity["path"] = target.relative_to(base).as_posix()
    return identity


def copy_transport_file(source: Path, target: Path, base: Path) -> dict[str, Any]:
    regular_path(source)
    before = file_identity(source)
    require(not path_entry_exists(target), "transport artifact already exists")
    target.parent.mkdir(parents=True, exist_ok=True)
    regular_path(target.parent)
    try:
        with source.open("rb") as source_stream, target.open("xb") as target_stream:
            shutil.copyfileobj(source_stream, target_stream)
    except FileExistsError as error:
        raise GameplayError("transport artifact already exists") from error
    regular_path(source)
    regular_path(target)
    require(file_identity(source) == before and all(file_identity(target)[key] == before[key] for key in ("size", "sha256")),
            "transport artifact changed while copying")
    return transport_file(target, base)


def verify_desktop_transport(row: dict[str, Any], base: Path) -> None:
    transport = row["transport"]
    require(row["platform"] == "windows" and transport["kind"] == "windows-desktop-helper-v1", "invalid desktop gameplay transport")
    require(transport["containment"] == "direct-native-process-only", "incorrect desktop native process ownership contract")
    job_id, task_id = transport["job_id"], transport["task_id"]
    require(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", task_id) is not None, "invalid desktop task ID")
    require(transport["build_after"] == row["build"] and transport["package_before"] == transport["package_after"],
            "source/build/dependency/runtime inputs changed during desktop capture")
    require(transport["helpers_before"] == transport["helpers_after"] and all(
        transport["helpers_before"][name]["sha256"] == digest for name, digest in DESKTOP_HELPERS.items()), "desktop helper identity drift")
    original = transport["original_executable"]
    require(original["sha256"] == row["build"]["executable_sha256"] == row["executable"]["sha256"]
            and original["size"] == row["executable"]["size"], "original build and native copied executable differ")
    require(transport["application_directory"] == str(PureWindowsPath(transport["job_directory"]) / "app")
            and PureWindowsPath(transport["job_directory"]) == PureWindowsPath(str(DESKTOP_ROOT)) / "jobs" / job_id,
            "desktop job/application path mismatch")
    require(PureWindowsPath(original["path"]) != PureWindowsPath(transport["application_directory"]) / "mc_main.exe",
            "original build executable must be distinguished from the native helper copy")
    require(row["command"] == [str(PureWindowsPath(transport["application_directory"]) / "mc_main.exe"),
                               *benchmark_arguments("windows", row["warmup_seconds"], transport["report_name"], row["proof"])],
            "native desktop command identity mismatch")
    report_name = relative_runtime_path(transport["report_name"])
    require("/" not in report_name and report_name == PureWindowsPath(row["report"]["path"]).name,
            "native report must be a relative filename in the fresh helper app")
    artifacts = {}
    require(set(transport["artifacts"]) == {"request.json", "status.json", "scheduler.json", "broker.log", "stdout.log", "stderr.log"},
            "missing native desktop request/status/process artifacts")
    for name, descriptor in transport["artifacts"].items():
        target = checked_file(descriptor, base)
        regular_path(target)
        artifacts[name] = target
    request, status, scheduler = (read_json(artifacts[name]) for name in ("request.json", "status.json", "scheduler.json"))
    require(status == transport["terminal_status"], "copied status differs from observed native terminal status")
    timeout = row["warmup_seconds"] * 2 + 120
    require(set(request) == DESKTOP_REQUEST_FIELDS and desktop_job_id(request) == job_id and request["task_id"] == task_id
            and request["executable"] == "mc_main.exe" and request["executable_sha256"] == original["sha256"]
            and request["arguments"] == row["command"][1:] and type(request["timeout_seconds"]) is int
            and request["timeout_seconds"] == timeout, "desktop request differs from native gameplay command")
    require(desktop_status(status, job_id, task_id, original["sha256"], timeout) and status["state"] == "succeeded"
            and status["exit_code"] == row["exit_code"], "native desktop terminal result failed")
    require(desktop_job_id(scheduler) == job_id and scheduler["task_id"] == task_id and scheduler["state"] == "submitted"
            and re.fullmatch(r"[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}", scheduler["instance_guid"]) is not None,
            "desktop scheduler instance identity mismatch")
    times = [datetime.datetime.fromisoformat(value) for value in (transport["started_at_utc"], scheduler["submitted_utc"],
             status["started_at_utc"], status["finished_at_utc"], transport["finished_at_utc"])]
    require(all(value.utcoffset() == datetime.timedelta(0) for value in times) and times == sorted(times), "stale desktop capture timestamps")
    lines = artifacts["broker.log"].read_text(encoding="utf-8-sig").splitlines()
    for suffix in (f"validated job={job_id} task={task_id} sha256={original['sha256']}",
                   f"started job={job_id} pid={status['pid']} session_id={status['session_id']}",
                   f"finished job={job_id} state=succeeded pid={status['pid']} session_id={status['session_id']} exit_code=0 error="):
        require(sum(line.endswith(" " + suffix) for line in lines) == 1, "broker log disagrees with native job/result identity")
    inputs, copied = transport["package_before"], transport["copied_inputs"]
    require(isinstance(inputs, list) and inputs and len(inputs) == len(copied), "missing copied runtime closure")
    require(transport["copied_before"] == transport["copied_after"] and len(transport["copied_before"]) == len(inputs),
            "native helper runtime closure changed during capture")
    require(transport["manifest_before"] == transport["manifest_after"], "desktop package manifest changed during capture")
    manifest = checked_file(transport["manifest"], base)
    regular_path(manifest)
    require(all(transport["manifest"][key] == transport["manifest_before"][key] for key in ("size", "sha256")),
            "copied runtime manifest identity mismatch")
    manifest_inputs = read_json(manifest)
    require(isinstance(manifest_inputs, list) and len(manifest_inputs) == len(inputs) and
            sorted((relative_runtime_path(entry["relative"]), entry["size"], entry["sha256"]) for entry in manifest_inputs) ==
            sorted((entry["relative"], entry["package"]["size"], entry["package"]["sha256"]) for entry in inputs),
            "runtime closure disagrees with its prelisted manifest")
    app = artifacts["request.json"].parent / "app"
    require(all(target.name == name and target.parent == app.parent for name, target in artifacts.items()),
            "retained helper artifact paths disagree")
    seen = set()
    for entry, staged, launched in zip(inputs, copied, transport["copied_before"]):
        relative = relative_runtime_path(entry["relative"])
        require(relative not in seen and staged["relative"] == launched["relative"] == relative,
                "copied runtime closure inventory mismatch")
        seen.add(relative)
        origin = (PureWindowsPath(transport["source_directory"]) / relative if relative.startswith("assets/") else
                  PureWindowsPath(original["path"]).parent / relative)
        require(PureWindowsPath(entry["original"]["path"]) == origin and
                PureWindowsPath(entry["package"]["path"]) == PureWindowsPath(transport["package_directory"]) / relative and
                PureWindowsPath(launched["file"]["path"]) == PureWindowsPath(transport["application_directory"]) / relative,
                "runtime closure original/package path mismatch")
        target = checked_file(staged["file"], base)
        regular_path(target)
        require(target == app / relative, "retained runtime input path disagrees with helper app closure")
        require(all(entry["original"][key] == entry["package"][key] == staged["file"][key] == launched["file"][key] for key in ("size", "sha256")),
                "copied runtime closure disagrees with original source/build inputs")
        if relative == "mc_main.exe":
            require(entry["original"] == original and staged["file"]["size"] == row["executable"]["size"] and
                    staged["file"]["sha256"] == row["executable"]["sha256"], "runtime executable identity mismatch")
    require("mc_main.exe" in seen, "copied runtime closure is missing the native executable")
    require({target.relative_to(app).as_posix() for target in app.rglob("*") if target.is_file()} == seen,
            "retained runtime closure contains unlisted inputs")
    raw = read_json(checked_file(row["report"], base))
    require(raw["game_benchmark"].get("diagnostic_overrides_present") is False,
            "desktop native diagnostic environment proof is missing or contains overrides")


def source_digest(root: Path) -> str:
    require(all((root / name).is_file() for name in REQUIRED_SOURCE_INPUTS),
            "source input inventory is incomplete")
    return _MATRIX._input_files_sha256(root, SOURCE_INPUTS)


def timing_summary(samples: list[int]) -> dict[str, int]:
    ordered = sorted(samples)
    count = len(ordered)
    return {"sample_count": count, **{name: ordered[(count * percentile + 999) // 1000 - 1] if count else 0
            for name, percentile in (("p50", 500), ("p95", 950), ("p99", 990), ("p99_8", 998), ("max", 1000))},
            "mean": sum(ordered) // count if count else 0}


def verify_actual_presentation_timing(value: Any, game: dict[str, Any], frames: list[dict[str, Any]],
                                      full: dict[str, Any], *, accept_cadence_exception: bool = False) -> dict[str, Any]:
    fields = {"contract", "provider", "source", "capability", "clock_domain", "successful_requests", "records",
              "polls", "capture_endpoint", "drain"}
    require(isinstance(value, dict) and set(value) == fields,
            "driver-reported presentation timing schema is absent or malformed")
    require(value["contract"] == ACTUAL_PRESENTATION_CONTRACT
            and value["provider"] == ACTUAL_PRESENTATION_PROVIDER
            and value["source"] == "driver-reported"
            and value["clock_domain"] == ACTUAL_PRESENTATION_CLOCK_DOMAIN,
            "driver-reported presentation timing identity is incorrect")
    require(value["capability"] == "supported", "VK_GOOGLE_display_timing is unavailable")

    requests = value["successful_requests"]
    require(isinstance(requests, list) and requests, "successful presentation request map is empty")
    request_fields = {"swapchain_generation", "application_present_id", "loop_ordinal", "phase"}
    keys: list[tuple[int, int]] = []
    request_by_key: dict[tuple[int, int], dict[str, Any]] = {}
    request_by_ordinal: dict[int, dict[str, Any]] = {}
    measurement_generation: int | None = None
    previous_generation: int | None = None
    previous_ordinal = 0
    previous_phase = -1
    last_id_by_generation: dict[int, int] = {}
    phase_index = {phase: index for index, phase in enumerate(PHASES)}
    for request in requests:
        require(isinstance(request, dict) and set(request) == request_fields,
                "successful presentation request map entry is malformed")
        request_generation = integer(request["swapchain_generation"], "presentation swapchain generation")
        present_id = integer(request["application_present_id"], "application present ID", 1)
        ordinal = integer(request["loop_ordinal"], "presentation loop ordinal", 1)
        phase = request["phase"]
        require(request_generation <= (1 << 64) - 1 and present_id <= (1 << 32) - 1,
                "presentation request key is outside its Vulkan integer range")
        require(isinstance(phase, str) and phase in phase_index,
                "successful presentation request has an unknown phase")
        key = (request_generation, present_id)
        require(key not in request_by_key and ordinal not in request_by_ordinal,
                "duplicate successful presentation request key or loop ordinal")
        require(ordinal > previous_ordinal and phase_index[phase] >= previous_phase,
                "successful presentation request map is not ordered")
        require(previous_generation is None or request_generation >= previous_generation,
                "successful presentation request generations are not nondecreasing")
        if previous_generation is not None and request_generation != previous_generation:
            require(phase == PHASES[0] and measurement_generation is None,
                    "swapchain recreation is only valid during initial warmup")
        if phase == PHASES[1] and measurement_generation is None:
            measurement_generation = request_generation
        elif measurement_generation is not None:
            require(request_generation == measurement_generation,
                    "swapchain recreation after the first baseline request makes presentation timing inconclusive")
        require(present_id > last_id_by_generation.get(request_generation, 0),
                "application present IDs are not strictly increasing within a swapchain generation")
        last_id_by_generation[request_generation] = present_id
        request_by_key[key] = request
        request_by_ordinal[ordinal] = request
        keys.append(key)
        previous_generation, previous_ordinal, previous_phase = request_generation, ordinal, phase_index[phase]
    require(measurement_generation is not None,
            "presentation request map has no successful baseline request to lock the measurement generation")
    measurement_keys = [key for key in keys if key[0] == measurement_generation]
    require(measurement_keys, "measurement swapchain generation has no successful requests")

    endpoint_fields = {"swapchain_generation", "application_present_id", "loop_ordinal"}
    endpoint = value["capture_endpoint"]
    last_request = requests[-1]
    require(isinstance(endpoint, dict) and set(endpoint) == endpoint_fields
            and endpoint == {field: last_request[field] for field in endpoint_fields}
            and last_request["phase"] == PHASES[4]
            and last_request["swapchain_generation"] == measurement_generation,
            "presentation capture endpoint is not the final recovered-stationary success")

    drain = value["drain"]
    require(isinstance(drain, dict) and set(drain) == {"complete", "request_ids"}
            and type(drain["complete"]) is bool and drain["complete"] is True
            and isinstance(drain["request_ids"], list),
            "bounded presentation timing drain is incomplete")
    drain_fields = {"swapchain_generation", "application_present_id"}
    drain_keys: list[tuple[int, int]] = []
    previous_drain_id = last_request["application_present_id"]
    for request_id in drain["request_ids"]:
        require(isinstance(request_id, dict) and set(request_id) == drain_fields,
                "presentation drain request ID is malformed")
        request_generation = integer(request_id["swapchain_generation"], "drain swapchain generation")
        present_id = integer(request_id["application_present_id"], "drain application present ID", 1)
        require(request_generation == measurement_generation and request_generation <= (1 << 64) - 1
                and present_id <= (1 << 32) - 1 and present_id > previous_drain_id,
                "presentation drain IDs are duplicate, unordered, or cross a swapchain generation")
        drain_keys.append((request_generation, present_id))
        previous_drain_id = present_id

    polls = value["polls"]
    require(isinstance(polls, list) and polls, "presentation timing poll history is empty")
    poll_fields = {"scope", "status", "vk_result", "returned_count"}
    record_count_from_polls, draining, loop_polls = 0, False, 0
    for poll in polls:
        require(isinstance(poll, dict) and set(poll) == poll_fields,
                "presentation timing poll entry is malformed")
        scope, status = poll["scope"], poll["status"]
        vk_result = poll["vk_result"]
        returned_count = integer(poll["returned_count"], "presentation poll returned count")
        require(scope in ("loop", "drain") and status in ("unavailable", "empty", "records", "incomplete", "error")
                and type(vk_result) is int, "presentation timing poll result is malformed")
        require(not draining or scope == "drain", "loop-scope poll follows presentation shutdown drain")
        if scope == "drain":
            draining = True
        else:
            loop_polls += 1
        record_count_from_polls += returned_count
        if status == "unavailable":
            require(False, "presentation timing provider became unavailable while polling")
        if status == "error":
            require(False, f"presentation timing poll failed with VkResult {vk_result}")
        if status == "empty":
            require(vk_result == 0 and returned_count == 0, "empty presentation poll has an inconsistent VkResult/count")
        elif status == "records":
            require(vk_result == 0 and returned_count > 0, "records presentation poll has an inconsistent VkResult/count")
        elif status == "incomplete":
            require(vk_result in (0, 5), "incomplete presentation poll has an unexpected VkResult")
    records = value["records"]
    require(isinstance(records, list), "flat driver presentation record history is malformed")
    expected_attempts = (integer(game["successful_present_requests"], "successful present requests")
                         + integer(game["failed_present_requests"], "failed present requests"))
    require(loop_polls == expected_attempts,
            "presentation poll history must contain exactly one poll per benchmark loop attempt")
    require(record_count_from_polls == len(records),
            "presentation poll counts do not reconcile with the flat record history")

    record_fields = {"swapchain_generation", "application_present_id", "actual_present_time_ns"}
    record_by_key: dict[tuple[int, int], int] = {}
    expected_keys = set(measurement_keys)
    all_request_keys = set(keys)
    drain_key_set = set(drain_keys)
    for record in records:
        require(isinstance(record, dict) and set(record) == record_fields,
                "driver presentation timing record is malformed")
        record_generation = integer(record["swapchain_generation"], "record swapchain generation")
        present_id = integer(record["application_present_id"], "record application present ID", 1)
        present_time = integer(record["actual_present_time_ns"], "actual presentation time", 1)
        require(present_time <= (1 << 64) - 1, "actual presentation time exceeds its Vulkan integer range")
        key = (record_generation, present_id)
        require(key not in record_by_key, "duplicate driver presentation timing record")
        require(key in all_request_keys or key in drain_key_set,
                "driver returned a presentation record outside the captured endpoint and drain IDs")
        record_by_key[key] = present_time
    missing = expected_keys - record_by_key.keys()
    require(not missing, "driver presentation history lost expected measurement-generation request IDs")
    ordered_record_keys = measurement_keys + [key for key in drain_keys if key in record_by_key]
    ordered_times = [record_by_key[key] for key in ordered_record_keys]
    require(all(right > left for left, right in zip(ordered_times, ordered_times[1:])),
            "driver presentation times are not strictly increasing within a swapchain generation")

    raw_by_ordinal: dict[int, dict[str, Any]] = {}
    for frame in frames:
        ordinal = integer(frame["loop_ordinal"], "raw frame loop ordinal", 1)
        require(ordinal not in raw_by_ordinal, "raw frame loop ordinal is duplicated")
        raw_by_ordinal[ordinal] = frame
        key_in_map = request_by_ordinal.get(ordinal)
        if frame["presentation_succeeded"]:
            require(key_in_map is not None
                    and key_in_map["phase"] == frame["phase"]
                    and key_in_map["swapchain_generation"] == integer(
                        frame.get("swapchain_generation"), "raw frame swapchain generation")
                    and key_in_map["application_present_id"] == integer(
                        frame.get("application_present_id"), "raw frame application present ID", 1),
                    "successful raw frame does not join its exact presentation request ID")
        else:
            require(key_in_map is None and frame.get("swapchain_generation") is None
                    and frame.get("application_present_id") is None,
                    "unsuccessful raw frame unexpectedly has a presentation request ID")
    for request in requests:
        if request["loop_ordinal"] not in raw_by_ordinal:
            require(request["phase"] in (PHASES[0], PHASES[3]),
                    "successful measured-phase request is missing from dense raw frame evidence")
    require(integer(game["successful_present_requests"], "successful present requests") == len(requests),
            "presentation request map does not cover all successful benchmark requests")
    for phase, count_field in ((PHASES[1], "baseline_present_requests"),
                               (PHASES[4], "recovered_present_requests")):
        require(sum(request["phase"] == phase for request in requests)
                == integer(full[count_field], count_field),
                f"{phase}: presentation request map does not match benchmark totals")
    for phase in (PHASES[1], PHASES[2], PHASES[4]):
        raw_successes = sum(frame["phase"] == phase and frame["presentation_succeeded"] for frame in frames)
        mapped_successes = sum(request["phase"] == phase for request in requests)
        require(mapped_successes == raw_successes,
                f"{phase}: driver request map does not reconcile with measured raw frames")

    interval_values: dict[str, list[int]] = {phase: [] for phase in (PHASES[1], PHASES[2], PHASES[4])}
    measurement_requests = [request for request in requests
                            if request["swapchain_generation"] == measurement_generation]
    for previous_request, current_request in zip(measurement_requests, measurement_requests[1:]):
        previous_key = (measurement_generation, previous_request["application_present_id"])
        current_key = (measurement_generation, current_request["application_present_id"])
        first_attempt = raw_by_ordinal.get(previous_request["loop_ordinal"] + 1)
        if first_attempt is None:
            continue
        phase = first_attempt["phase"]
        if phase in interval_values:
            interval_values[phase].append(record_by_key[current_key] - record_by_key[previous_key])

    metrics, failures = {}, []
    for phase, elapsed_field in ((PHASES[1], "baseline_stationary_elapsed_ms"),
                                 (PHASES[2], "movement_elapsed_ms"),
                                 (PHASES[4], "recovered_stationary_elapsed_ms")):
        intervals = interval_values[phase]
        summary = timing_summary(intervals)
        require(len(intervals) >= 500,
                f"{phase}: insufficient driver presentation intervals ({len(intervals)}, requires 500)")
        require(bool(intervals) and all(interval > 0 for interval in intervals),
                f"{phase}: driver presentation cadence is not computable")
        interval_ns = sum(intervals)
        rate = len(intervals) * 1_000_000_000 / interval_ns
        slots = [max(1, round(interval * 120 / 1_000_000_000)) for interval in intervals]
        lost, total = sum(slot - 1 for slot in slots), sum(slots)
        if interval_ns * 120 * 998 > len(intervals) * 1_000_000_000 * 1000:
            failures.append(f"{phase}: driver presentation rate {rate:.6f}Hz is below 119.76Hz")
        require(max(1, round(summary["p50"] * 120 / 1_000_000_000)) == 1,
                "; ".join([*failures, f"{phase}: driver presentation median exceeds one 120Hz refresh slot"]))
        if lost * 1000 > total * 2:
            failures.append(f"{phase}: driver presentation lost FIFO refresh-slot share exceeds 0.2% ({lost}/{total})")
        metrics[phase] = {"target_hz": 120, "minimum_rate_hz": 119.76, "intervals_ns": summary,
                          "phase_elapsed_ms": integer(full[elapsed_field], elapsed_field, 1),
                          "successful_requests": sum(request["phase"] == phase for request in requests),
                          "interval_rate_hz": rate,
                          "cadence": {"contract": "fifo-nearest-120hz-driver-presentation-slots",
                                      "lost_slots": lost, "total_slots": total,
                                      "lost_slot_share": lost / total}}
    require(not failures or accept_cadence_exception, "; ".join(failures))
    return {"status": "failed" if failures else "passed", "failure_reasons": failures,
            "source": "driver-reported", "provider": ACTUAL_PRESENTATION_PROVIDER,
            "clock_domain": ACTUAL_PRESENTATION_CLOCK_DOMAIN, "expected_record_count": len(measurement_keys),
            "drain_record_count": sum(key in record_by_key for key in drain_keys), "poll_count": len(polls),
            "incomplete_poll_count": sum(poll["status"] == "incomplete" for poll in polls), "phases": metrics}


def verify_barrier(barrier: dict[str, Any], phase: str, elapsed: int, radius: int) -> None:
    tile_count = sum(2 * math.isqrt(radius * radius - x * x) + 1 for x in range(-radius, radius + 1))
    require(barrier["phase"] == phase and all(barrier[key] is True for key in
            ("scan_performed", "potentially_ready", "exact_current_coverage")), f"{phase}: missing exact coverage scan")
    tile_center(barrier["center"], phase)
    require(integer(barrier["phase_elapsed_ms"], f"{phase}: barrier elapsed") == elapsed, f"{phase}: barrier elapsed disagrees with warmup")
    for key in ("desired", "resident_desired", "current_handle_mesh_visible", "visible_meshes", "renderer_mesh_slots"):
        require(integer(barrier[key], key) == tile_count, f"{phase}: {key} must be {tile_count}")
    require(integer(barrier["resident_total"], "resident_total") >= tile_count, f"{phase}: incomplete residency")
    for key in (*QUEUE_FIELDS, "nonresident_desired", "stale_or_missing_mesh", "unclassified_resident_missing",
                "visible_outside_interest", "unclassified_current_visible_mesh_detail"):
        require(integer(barrier[key], key) == 0, f"{phase}: {key} is not drained")
    details = barrier["current_visible_mesh_detail_counts"]
    require(isinstance(details, dict) and set(details) == {"coarse", "coarse2", "coarse4", "fine", "distant"}
            and sum(integer(value, "mesh detail") for value in details.values()) == tile_count, f"{phase}: mesh details mismatch")
    expected = radial_band_counts(radius)
    bands = barrier["radial_bands"]
    require(barrier["radial_band_tile_width"] == 16 and len(bands) == 16, f"{phase}: incomplete radial bands")
    for band, count in zip(bands, expected):
        require(all(integer(band[key], key) == count for key in ("desired", "resident", "current_mesh_visible"))
                and band["nonresident"] == 0 and band["stale_or_missing_mesh"] == 0, f"{phase}: radial coverage mismatch")


def verify_report(report: dict[str, Any], platform: str, proof: str | None = None,
                  *, cadence_exception: dict[str, Any] | None = None) -> dict[str, Any]:
    require(report["schema_version"] == "mc.runtime-evidence.v1" and report["mode"] == "benchmark-game"
            and report["passed"] is True and report["failure"] == "", "runtime workload did not pass")
    timing = report["timing"]
    require(integer(timing["elapsed_ms"], "elapsed_ms", 1) <= integer(timing["deadline_ms"], "deadline_ms", 1), "runtime timeout")
    game = report["game_benchmark"]
    actual_proof = game.get("qualification_proof")
    require(actual_proof in PROOFS and (proof is None or proof == actual_proof), "missing or mismatched gameplay proof selector")
    require(cadence_exception is None or platform == "macos" and actual_proof == "performance",
            "cadence exception cannot apply to another platform or proof")
    radius = PROOFS[actual_proof]
    cadence_required = actual_proof == "performance"
    request_cadence_required = cadence_required and platform == "windows"
    presentation_timing_required = cadence_required and platform == "macos"
    frame_budget_required = request_cadence_required
    require(game.get("diagnostic_overrides_present") is False, "diagnostic gameplay report is not qualification proof")
    require(game["workload"] == WORKLOAD and game["build_mode"] == "release", "requires Release full-radius workload")
    require(game["displayed_cadence_hz"] is None, "physical display cadence is unavailable in this contract")
    require(game["present_mode"] == ("immediate" if platform == "windows" else "fifo"), "incorrect negotiated present mode")
    for key in ("server_events_processed", "inputs_sent", "authoritative_player_updates"):
        integer(game[key], key, 1)
    full = game["full_radius_terrain"]
    expected_count = sum(2 * math.isqrt(radius * radius - x * x) + 1 for x in range(-radius, radius + 1))
    require(full["contract"] == "mc.full-radius-terrain-barriers.v1" and full["render_distance"] == radius
            and full["expected_tile_count"] == expected_count and full["complete"] is True, "incomplete proof radius terrain contract")
    require(full["loop_timing_scope"] == "all-stationary-loop-attempts" and full["successful_present_request_interval_scope"]
            == "consecutive-successful-request-completions-not-physical-display-cadence", "incorrect timing scope")
    timeout = integer(full["warmup_timeout_ms"], "warmup_timeout_ms", 1000)
    require(timeout == 180000, "qualification requires the 180-second terrain warmup bound")
    for prefix, phase in (("initial", PHASES[0]), ("recovery", PHASES[3])):
        elapsed = integer(full[prefix + "_warmup_elapsed_ms"], prefix + " warmup elapsed")
        require(full[prefix + "_barrier_reached"] is True and elapsed <= timeout, f"{prefix}: warmup failed")
        verify_barrier(full[prefix + "_barrier"], phase, elapsed, radius)
    movement_start_center = tile_center(full["movement_start_center"], "movement start")
    movement_end_center = tile_center(full["movement_end_center"], "movement end")
    recovery_center = tile_center(full["recovery_barrier"]["center"], "recovery")
    movement_elapsed_ms = integer(full["movement_elapsed_ms"], "movement_elapsed_ms")
    if cadence_required:
        require(PERFORMANCE_SAMPLE_MS <= movement_elapsed_ms
                <= PERFORMANCE_SAMPLE_MS + PERFORMANCE_SAMPLE_OVERRUN_MS,
                "performance proof requires a fixed 30-second performance sample")
    else:
        require(movement_elapsed_ms >= 6000, "full-axis movement did not occur")
    require(
            integer(full["movement_center_change_count"], "movement_center_change_count", 1) > 0 and
            movement_start_center != movement_end_center, "full-axis movement did not occur")
    require(wrapped_tile_distance(recovery_center, movement_end_center)
            < wrapped_tile_distance(recovery_center, movement_start_center),
            "recovery barrier is not on the travel destination side of the origin")
    frames = game["raw_frames"]
    require(isinstance(frames, list) and frames, "raw frame evidence is absent")
    readiness = game.get("terrain_readiness")
    require(isinstance(readiness, dict)
            and readiness.get("contract") == "mc.current-near-terrain-readiness.v1"
            and readiness.get("measurement_scope") == "current-center-radius-7",
            "full-radius current-near terrain readiness evidence is absent")
    near_frames = [frame for frame in frames if frame.get("phase") in (PHASES[2], PHASES[3])]
    require(near_frames, "full-radius travel has no current-near samples")
    movement_frames = [frame for frame in frames if frame.get("phase") == PHASES[2]]
    require(movement_frames, "full-radius movement has no raw frames")
    for frame in near_frames:
        near = frame.get("terrain_near")
        require(isinstance(near, dict), "full-radius travel frame lacks current-near terrain evidence")
        frame_center = tile_center(frame.get("terrain_center"), "travel frame")
        near_center = tile_center(near.get("center"), "current-near sample")
        require(near_center == frame_center, "current-near terrain center disagrees with its travel frame")
        require(integer(near.get("desired"), "current-near desired") == 225
                and integer(near.get("resident"), "current-near resident") == 225
                and integer(near.get("current_mesh_visible"), "current-near visible") == 225
                and integer(near.get("nonresident"), "current-near nonresident") == 0,
                "full-radius travel left missing meshes in the current near terrain window")
    require(integer(readiness.get("current_near_sample_count"), "current-near sample count") >= len(movement_frames)
            and integer(readiness.get("incomplete_current_near_sample_count"),
                        "incomplete current-near sample count") == 0
            and integer(readiness.get("max_missing_current_mesh_count"), "maximum current-near mesh gap") == 0
            and readiness.get("current_near_coverage_complete") is True,
            "full-radius current-near terrain coverage is incomplete")
    observed_phases, previous_completion = [], -1
    for frame in frames:
        require(frame["phase"] in PHASES, "unexpected raw frame phase")
        if not observed_phases or observed_phases[-1] != frame["phase"]:
            observed_phases.append(frame["phase"])
        start = integer(frame["started_offset_ns"], "started_offset_ns")
        duration = integer(frame["loop_duration_ns"], "loop_duration_ns", 1)
        require(start >= previous_completion and type(frame["presentation_succeeded"]) is bool, "unordered or malformed raw frames")
        recreation_attempted = frame["diagnostic_presentation_recreation_attempted"]
        recreation_deferred = frame["diagnostic_presentation_recreation_deferred"]
        recreation_duration = integer(
            frame["diagnostic_cpu_presentation_recreation_duration_ns"],
            "presentation recreation duration",
        )
        require(type(recreation_attempted) is bool and type(recreation_deferred) is bool,
                "presentation recreation diagnostics must be boolean")
        require(recreation_attempted or (not recreation_deferred and recreation_duration == 0),
                "presentation recreation timing exists without an attempt")
        require(not recreation_deferred or (recreation_attempted and not frame["presentation_succeeded"]
                and frame["diagnostic_frame_recorded"] is False),
                "deferred presentation recreation recorded or presented a frame")
        radial_bands = frame["diagnostic_terrain_radial_bands"]
        if radial_bands is None:
            require(integer(frame["diagnostic_terrain_spatial_scan_duration_ns"],
                            "terrain spatial scan duration") == 0,
                    "terrain spatial scan duration exists without radial bands")
        else:
            verify_terrain_radial_scan(radial_bands, frame["diagnostic_terrain_spatial_scan_duration_ns"])
            require(all(band["desired"] == count
                        for band, count in zip(radial_bands, radial_band_counts(radius))),
                    "terrain radial desired counts disagree with render radius")
            require(all(integer(band["desired"], "radial desired")
                        == integer(band["current_mesh_visible"], "radial current visible")
                        and integer(band["nonresident"], "radial nonresident") == 0
                        and integer(band["stale_or_missing_mesh"], "radial stale/missing") == 0
                        for band in radial_bands),
                    "full-radius diagnostic scan found missing terrain inside the render radius")
        previous_completion = start + duration
    require(tuple(observed_phases) == PHASES, "raw phase sequence is incomplete")
    for left, right in zip(frames, frames[1:]):
        if left["phase"] in (PHASES[1], PHASES[2], PHASES[3], PHASES[4]):
            require(integer(right.get("loop_ordinal"), "loop ordinal", 1)
                    == integer(left.get("loop_ordinal"), "loop ordinal", 1) + 1,
                    "missing loop in measured or recovery raw timeline")
    require(frames[0]["started_offset_ns"] == 0 and previous_completion // 1000000 <= timing["elapsed_ms"],
            "raw timeline exceeds overall runtime elapsed or has an incorrect origin")
    for prefix, warmup, stationary in (("initial", PHASES[0], PHASES[1]), ("recovery", PHASES[3], PHASES[4])):
        last_warmup = next(frame for frame in reversed(frames) if frame["phase"] == warmup)
        first_stationary = next(frame for frame in frames if frame["phase"] == stationary)
        require(all(tile_center(frame["terrain_center"], prefix) == full[prefix + "_barrier"]["center"]
                    for frame in (last_warmup, first_stationary)), f"{prefix}: barrier center disagrees with raw transition")
    raw_unsuccessful = sum(not frame["presentation_succeeded"] for frame in frames)
    require(integer(game["failed_present_requests"], "failed_present_requests") >= raw_unsuccessful and
            integer(game["successful_present_requests"], "successful_present_requests") >= len(frames) - raw_unsuccessful,
            "aggregate presentation totals omit raw attempts")
    movement = [frame["terrain_center"] for frame in frames if frame["phase"] == PHASES[2]]
    require(movement[0] == full["movement_start_center"] and movement[-1] == full["movement_end_center"] and
            sum(left != right for left, right in zip(movement, movement[1:])) == full["movement_center_change_count"],
            "raw movement centers mismatch")
    movement_frames = [frame for frame in frames if frame["phase"] == PHASES[2]]
    movement_end = movement_frames[-1]["started_offset_ns"] + movement_frames[-1]["loop_duration_ns"]
    movement_before_ms = full["initial_warmup_elapsed_ms"] + full["baseline_stationary_elapsed_ms"]
    require(0 <= movement_end - (movement_before_ms + full["movement_elapsed_ms"]) * 1000000 < 3000000,
            "raw movement duration mismatch")
    for prefix, phase in (("baseline", PHASES[1]), ("recovered", PHASES[4])):
        selected = [frame for frame in frames if frame["phase"] == phase]
        durations = [frame["loop_duration_ns"] for frame in selected]
        completions = [frame["started_offset_ns"] + frame["loop_duration_ns"] for frame in selected if frame["presentation_succeeded"]]
        intervals = [right - left for left, right in zip(completions, completions[1:])]
        attempts, successful = len(selected), len(completions)
        elapsed = integer(full[prefix + "_stationary_elapsed_ms"], "stationary_elapsed_ms")
        if cadence_required:
            require(PERFORMANCE_SAMPLE_MS <= elapsed
                    <= PERFORMANCE_SAMPLE_MS + PERFORMANCE_SAMPLE_OVERRUN_MS,
                    "performance proof requires a fixed 30-second performance sample")
        else:
            require(elapsed >= 10000, f"{prefix}: incomplete stationary raw duration")
        require(attempts == full[prefix + "_loop_attempts"] and successful == full[prefix + "_present_requests"]
                and attempts - successful == full[prefix + "_failed_present_requests"], f"{prefix}: raw attempt totals mismatch")
        require(full[prefix + "_loop_timings_ns"] == timing_summary(durations)
                and full[prefix + "_successful_present_request_intervals_ns"] == timing_summary(intervals), f"{prefix}: raw timing summary mismatch")
        require(math.isclose(full[prefix + "_present_requests_per_second"], successful * 1000 / elapsed, rel_tol=0, abs_tol=0.00000051),
                f"{prefix}: raw request rate mismatch")
        span = selected[-1]["started_offset_ns"] + selected[-1]["loop_duration_ns"] - selected[0]["started_offset_ns"]
        preceding_ms = full["initial_warmup_elapsed_ms"] + (full["baseline_stationary_elapsed_ms"] +
                       full["movement_elapsed_ms"] + full["recovery_warmup_elapsed_ms"] if prefix == "recovered" else 0)
        end = selected[-1]["started_offset_ns"] + selected[-1]["loop_duration_ns"]
        require(0 <= end - (preceding_ms + elapsed) * 1000000 < (5 if prefix == "recovered" else 2) * 1000000
                and 0 <= selected[0]["started_offset_ns"] - preceding_ms * 1000000 < elapsed * 1000000
                and span <= (elapsed + 1) * 1000000 and sum(durations) <= (elapsed + 1) * 1000000,
                f"{prefix}: incomplete stationary raw duration")
    presentation_metrics = {"status": "not-required"}
    if presentation_timing_required:
        presentation_metrics = verify_actual_presentation_timing(
            game.get("presentation_timing"), game, frames, full,
            accept_cadence_exception=cadence_exception is not None)

    proxy, cadence_failures = {}, []

    def check_cadence(condition: bool, message: str) -> None:
        if not condition:
            cadence_failures.append(message)

    cadence_phases = (PHASES[1], PHASES[2], PHASES[4])
    intervals_by_phase: dict[str, list[int]] = {phase: [] for phase in cadence_phases}
    successful_positions = [index for index, frame in enumerate(frames) if frame["presentation_succeeded"]]
    # The first attempt after a completion starts the next half-open interval.
    for previous_index, current_index in zip(successful_positions, successful_positions[1:]):
        attempt_chain = frames[previous_index:current_index + 1]
        ordinals = [integer(frame["loop_ordinal"], "loop ordinal", 1) for frame in attempt_chain]
        if any(right != left + 1 for left, right in zip(ordinals, ordinals[1:])):
            continue
        phase = frames[previous_index + 1]["phase"]
        if phase in intervals_by_phase:
            previous = frames[previous_index]
            current = frames[current_index]
            previous_completion = previous["started_offset_ns"] + previous["loop_duration_ns"]
            current_completion = current["started_offset_ns"] + current["loop_duration_ns"]
            intervals_by_phase[phase].append(current_completion - previous_completion)

    for phase, elapsed_key in ((PHASES[1], "baseline_stationary_elapsed_ms"), (PHASES[2], "movement_elapsed_ms"),
                               (PHASES[4], "recovered_stationary_elapsed_ms")):
        selected = [frame for frame in frames if frame["phase"] == phase]
        completions = [frame["started_offset_ns"] + frame["loop_duration_ns"] for frame in selected if frame["presentation_succeeded"]]
        intervals = intervals_by_phase[phase]
        computable = len(completions) >= 2 and bool(intervals) and all(interval > 0 for interval in intervals)
        check_cadence(computable, f"{phase}: request cadence is not computable from raw completions")
        rate = len(completions) * 1000 / full[elapsed_key]
        require(math.isfinite(rate), f"{phase}: request cadence rate is not finite")
        minimum_rate = TARGET_HZ[platform] * (0.998 if platform == "macos" else 1)
        check_cadence(rate >= minimum_rate, f"{phase}: successful-request rate {rate:.6f}Hz is below {minimum_rate:g}Hz")
        check_cadence(len(intervals) >= 500, f"{phase}: insufficient cadence samples ({len(intervals)} intervals, requires 500)")
        summary, limit = timing_summary(intervals), math.ceil(1000000000 / TARGET_HZ[platform])
        interval_ns = sum(intervals)
        if platform == "macos":
            slots = [max(1, round(interval * 120 / 1000000000)) for interval in intervals]
            lost, total = sum(slot - 1 for slot in slots), sum(slots)
            check_cadence(bool(intervals) and lost * 1000 <= total * 2,
                          f"{phase}: lost FIFO refresh-slot share exceeds 0.2% ({lost}/{total})")
            check_cadence(max(1, round(summary["p50"] * 120 / 1000000000)) == 1
                    and interval_ns * 120 * 998 <= len(intervals) * 1000000000 * 1000,
                    f"{phase}: FIFO median/mean cadence is below the 99.8% rate floor")
            cadence = {"contract": "fifo-nearest-120hz-request-slots", "lost_slots": lost, "total_slots": total,
                       "lost_slot_share": lost / total if total else 0.0}
        else:
            check_cadence(bool(intervals) and summary["p99_8"] <= limit,
                          f"{phase}: request-cadence proxy fails 120Hz at 99.8% (p99.8={summary['p99_8']}ns, limit={limit}ns)")
            excess_ns = sum(max(0, interval - limit) for interval in intervals)
            check_cadence(bool(intervals) and excess_ns * 1000 <= interval_ns * 2,
                          f"{phase}: excess-time share exceeds 0.2% ({excess_ns}/{interval_ns}ns)")
            cadence = {"contract": "immediate-raw-request-completions",
                       "excess_time_share": excess_ns / interval_ns if interval_ns else 0.0}
        proxy[phase] = {"target_hz": TARGET_HZ[platform], "minimum_rate_hz": minimum_rate, "cadence": cadence,
                        "intervals_ns": summary, "attempts": len(selected),
                        "successful_requests": len(completions), "failed_requests": len(selected) - len(completions),
                        "first_loop_started_offset_ns": selected[0]["started_offset_ns"],
                        "last_loop_completed_offset_ns": selected[-1]["started_offset_ns"] + selected[-1]["loop_duration_ns"],
                        "phase_elapsed_ms": full[elapsed_key], "present_requests_per_second": rate,
                        "interval_scope": "half-open [previous_successful_completion, current_successful_completion)"}
    ticks = [integer(value, "server tick", 1) for value in game["raw_server_tick_durations_ns"]]
    require(ticks and game["server_tick_timings_ns"] == timing_summary(ticks), "raw server timing summary mismatch")
    measured = [frame["loop_duration_ns"] for frame in frames if frame["phase"] in (PHASES[1], PHASES[2], PHASES[4])]
    measured_ns, in_budget_ns = sum(measured), sum(value for value in measured if value <= 8333334)
    budget_passed = measured_ns > 0 and measured_ns - in_budget_ns <= measured_ns * 2 // 1000
    require(game.get("frame_budget") == {"target_frame_duration_ns": 8333334,
            "required_in_budget_time_permille": 998, "measured_time_ns": measured_ns,
            "in_budget_time_ns": in_budget_ns, "sample_count": len(measured), "passed": budget_passed,
            "required_for_benchmark_pass": frame_budget_required}, "raw game-loop frame budget mismatch")
    require(not request_cadence_required or not cadence_failures, "; ".join(cadence_failures))
    require(not frame_budget_required or budget_passed, "120Hz game-loop time budget exceeds 0.2%")
    accepted = cadence_exception if presentation_metrics["status"] == "failed" else None
    return {"status": "accepted_exception" if accepted else "passed", "accepted_exception": accepted,
            "proof": actual_proof, "render_distance": radius, "coverage": "passed", "request_cadence_status": "failed" if cadence_failures else "passed",
            "request_cadence_required": request_cadence_required, "request_cadence_reasons": cadence_failures,
            "request_cadence_proxy": proxy, "total_unsuccessful_attempts": game["failed_present_requests"],
            "raw_unsuccessful_attempts": raw_unsuccessful, "presentation_timing_required": presentation_timing_required,
            "presentation_timing_status": presentation_metrics["status"],
            "presentation_timing_metrics": presentation_metrics,
            "physical_display_cadence": "unavailable"}


def verify_receipt(root: Path, receipt: Path, *, s7_windows_secondary: bool = False,
                   s7_cadence_exception: str | None = None) -> dict[str, Any]:
    if s7_windows_secondary:
        _CHECK.require_s7_candidate(root)
    approval = cadence_exception_approval(root, s7_cadence_exception)
    aggregate = read_json(receipt)
    require(aggregate["schema_version"] == SCHEMA and isinstance(aggregate["rows"], list), "invalid gameplay proof schema")
    rows = aggregate["rows"]
    required_platforms = ("macos",) if s7_windows_secondary else tuple(TARGET_HZ)
    required = {(platform, proof) for platform in required_platforms for proof in PROOFS}
    requirement = (
        "S7 Windows-secondary profile requires Mac terrain-coverage and performance proofs only"
        if s7_windows_secondary else
        "both terrain-coverage and performance proof are required on macos and windows"
    )
    require(len(rows) == len(required) and {(row["platform"], row.get("proof")) for row in rows} == required,
            requirement)
    expected_source = source_digest(root)
    require(all(row["source_candidate"] == rows[0]["source_candidate"] for row in rows), "mixed gameplay source candidates")
    results = {}
    for row in rows:
        require(row["proof_origin"] == "local" and row["source_input_sha256"] == expected_source, "stale or nonlocal gameplay source")
        _MATRIX._validate_candidate(row["source_candidate"], "gameplay source candidate")
        build = row["build"]
        require(build["configuration"] == "Release" and build["source_input_sha256"] == expected_source
                and build["executable_sha256"] == row["executable"]["sha256"], "stale gameplay build identity")
        for key in ("configuration_sha256", "dependency_sha256"):
            require(isinstance(build[key], str) and len(build[key]) == 64 and all(char in "0123456789abcdef" for char in build[key]), "missing build/dependency identity")
        same_platform = next(other for other in rows if other["platform"] == row["platform"])
        require(build == same_platform["build"], "mixed gameplay builds or dependencies across proof types")
        executable = checked_file(row["executable"], receipt.parent)
        report = checked_file(row["report"], receipt.parent)
        require(row["command"][1:] == benchmark_arguments(row["platform"], row["warmup_seconds"], row["command"][-1], row["proof"]), "nonproduction gameplay command")
        require(PureWindowsPath(row["command"][0]).name == executable.name and PureWindowsPath(row["command"][-1]).name == report.name,
                "capture command disagrees with executable or report identity")
        if row["platform"] == "macos":
            verify_macos_runtime(row, receipt.parent)
        elif "actual_command" in row or "native_report_path" in row:
            native_report_path = row.get("native_report_path")
            native_path_type = PureWindowsPath if row["platform"] == "windows" else Path
            require(isinstance(native_report_path, str) and native_path_type(native_report_path).is_absolute()
                    and row.get("actual_command") == [*row["command"][:-1], native_report_path],
                    "actual capture command differs beyond native report relocation")
        require(integer(row["exit_code"], "exit_code") == 0 and row["timed_out"] is False and row["environment"] == {}, "failed or diagnostic gameplay invocation")
        require(not row.get("capture_error"), "gameplay recorder retained a capture failure")
        require(executable.name == ("mc_main.exe" if row["platform"] == "windows" else "mc_main"), "wrong gameplay executable")
        if "transport" in row:
            verify_desktop_transport(row, receipt.parent)
        raw = read_json(report)
        results.setdefault(row["platform"], {})[row["proof"]] = verify_report(
            raw, row["platform"], row["proof"],
            cadence_exception=approval if row["platform"] == "macos" and row["proof"] == "performance" else None)
        require(raw["game_benchmark"]["full_radius_terrain"]["warmup_timeout_ms"] == row["warmup_seconds"] * 1000,
                "capture command warmup disagrees with raw report")
    if s7_windows_secondary:
        results["windows"] = {
            "status": "deferred",
            "reason": "Windows gameplay qualification was not selected for this S7 snapshot.",
        }
    return results


def benchmark_arguments(platform: str, warmup: int, report: str, proof: str = "performance") -> list[str]:
    require(type(warmup) is int and 1 <= warmup <= 1800, "invalid warmup bound")
    require(proof in PROOFS, "invalid gameplay proof selector")
    return ["--benchmark-game", "--workload", "speed-200-full-radius", "--gameplay-proof", proof, "--render-distance", str(PROOFS[proof]),
            "--full-radius-warmup-seconds", str(warmup), *( ["--present-immediate"] if platform == "windows" else []), "--evidence", report]


def build_identity(root: Path, build_root: Path, executable: Path) -> dict[str, Any]:
    require(all((build_root / name).is_file() for name in ("CMakeCache.txt", "build.ninja", "CMakeFiles/rules.ninja", "compile_commands.json")),
            "Release build configuration inventory is incomplete")
    cache = {}
    for line in (build_root / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if not line.startswith(("#", "//")) and ":" in line and "=" in line:
            cache[line.split(":", 1)[0]] = line.split("=", 1)[1]
    require(cache.get("CMAKE_GENERATOR") == "Ninja" and cache.get("CMAKE_BUILD_TYPE") == "Release" and Path(cache["CMAKE_HOME_DIRECTORY"]).resolve() == root,
            "requires a Release build of this source tree")
    require(cache.get("MC_ENABLE_SANITIZERS") == "OFF" and cache.get("MC_ALLOW_INEXACT_CORECPP") == "OFF", "requires production exact dependencies")
    probe = subprocess.run([cache["CMAKE_MAKE_PROGRAM"], "-n", "-v", executable.name], cwd=build_root, capture_output=True, text=True, timeout=60)
    actions = [line for line in probe.stdout.splitlines() if line.startswith("[")]
    require(probe.returncode == 0 and all(" -E copy_if_different " in line and " -E make_directory " in line for line in actions),
            "Release build is stale; build it before capturing gameplay evidence")
    private_prefixes = cache.get("CMAKE_PREFIX_PATH", "").split(";")
    require(private_prefixes and all(private_prefixes), "Release CMake prefix path is missing or contains an empty entry")
    environment = {"VCPKG_ROOT": cache["Z_VCPKG_ROOT_DIR"], "VCPKG_INSTALLED_DIR": cache["VCPKG_INSTALLED_DIR"]}
    dependency_row = {"id": "macos_ubsan"}
    shared_dependencies = None
    prefix_identities = []
    for prefix in private_prefixes:
        identity = _MATRIX._external_dependency_identity(
            root, dependency_row, {**environment, "MC_PRIVATE_DEPENDENCIES_PREFIX": prefix}
        )
        prefix_identity = identity.pop("MC_PRIVATE_DEPENDENCIES_PREFIX", None)
        require(prefix_identity is not None, "dependency identity omitted a CMake prefix")
        if shared_dependencies is None:
            shared_dependencies = identity
        else:
            require(identity == shared_dependencies, "shared dependency identity changed while capturing CMake prefixes")
        prefix_identities.append(prefix_identity)
    dependencies = shared_dependencies or {}
    dependencies["CMAKE_PREFIX_PATH"] = prefix_identities
    return {"configuration": "Release", "source_input_sha256": source_digest(root),
            "executable_sha256": file_identity(executable)["sha256"],
            "configuration_sha256": _MATRIX._input_files_sha256(build_root, ("CMakeCache.txt", "build.ninja", "CMakeFiles/rules.ninja", "compile_commands.json", "shaders/**")),
            "dependency_sha256": hashlib.sha256(_MATRIX._canonical_json(dependencies)).hexdigest()}


def cancel_desktop_job(job_id: str, task_id: str, digest: str, timeout: int,
                       request_identity_verified: bool) -> dict[str, Any]:
    cleanup: dict[str, Any] = {"job_id": job_id, "terminal_observed": False}
    try:
        cancelled = desktop_client("Cancel", job_id=job_id)
        require(desktop_job_id(cancelled) == job_id and cancelled.get("state") in ("cancel-requested", "already-terminal"),
                "desktop cancellation identity mismatch")
        cleanup["cancel_response"] = cancelled
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            status = desktop_client("Status", job_id=job_id, client_timeout=max(0.1, min(30, deadline - time.monotonic())))
            if desktop_startup_failure(status, job_id, task_id):
                require(request_identity_verified, "desktop cleanup cannot prove startup failure without a verified request")
                terminal = True
            else:
                terminal = desktop_status(status, job_id, task_id, digest, timeout)
            if terminal:
                cleanup.update(terminal_observed=True, terminal_status=status)
                break
            time.sleep(min(1, max(0, deadline - time.monotonic())))
        require(cleanup["terminal_observed"], "desktop cancellation did not reach terminal status within 30 seconds")
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        cleanup["error"] = str(error) or type(error).__name__
    return cleanup


def record_desktop(root: Path, receipt: Path, executable: Path, warmup: int,
                   before: dict[str, Any], package: Path, manifest: Path, task_id: str, proof: str = "performance") -> None:
    require(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", task_id) is not None, "invalid desktop task ID")
    task_id = f"{task_id[:31]}-{uuid.uuid4().hex}"
    report = receipt.with_suffix(".raw.json")
    report_name = relative_runtime_path(report.name)
    require("/" not in report_name, "native report must be a relative filename")
    retained = receipt.with_suffix(".desktop")
    require(not path_entry_exists(report), "raw capture path already exists")
    require(not path_entry_exists(retained), "desktop transport output already exists; select a fresh receipt")
    timeout, arguments = warmup * 2 + 120, benchmark_arguments("windows", warmup, report_name, proof)
    package_before = desktop_package_identity(root, executable, package, manifest)
    helpers_before = desktop_helper_identity()
    candidate = _MATRIX.candidate_identity(root)
    jobs = DESKTOP_ROOT / "jobs"
    regular_path(jobs)
    preexisting_jobs = {target.name for target in jobs.iterdir()}
    transport: dict[str, Any] = {
        "kind": "windows-desktop-helper-v1", "containment": "direct-native-process-only", "original_executable": file_identity(executable),
        "source_directory": str(root), "package_directory": str(package),
        "task_id": task_id, "report_name": report_name, "helpers_before": helpers_before, "helpers_after": None,
        "manifest_before": file_identity(manifest), "manifest_after": None,
        "package_before": package_before, "package_after": None, "build_after": None, "artifacts": {}, "copied_inputs": [],
        "copied_before": None, "copied_after": None,
        "started_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    }
    job_id, terminal, exit_code, timed_out = None, False, None, False
    request_identity_verified = False
    command, captured_report, captured_executable = [str(executable), *arguments], None, file_identity(executable)
    errors = []
    started, heartbeat = time.monotonic(), time.monotonic()
    print(f"CAPTURE windows: submit direct native desktop workload once; native timeout={timeout}s, startup allowance=60s", flush=True)
    try:
        submission = desktop_client("Submit", package=str(package), arguments=arguments, timeout_seconds=timeout, task_id=task_id)
        returned_id = desktop_job_id(submission)
        require(returned_id not in preexisting_jobs, "desktop helper returned a pre-existing job")
        regular_path(jobs / returned_id / "request.json")
        request = read_json(jobs / returned_id / "request.json")
        require(desktop_job_id(request) == returned_id and request.get("task_id") == task_id,
                "desktop submission does not identify the newly owned request")
        job_id = returned_id
        transport["job_id"] = job_id
        require(submission.get("task_id") == task_id and submission.get("state") == "submitted", "desktop submission failed or has stale task identity")
        require(set(request) == DESKTOP_REQUEST_FIELDS and request["arguments"] == arguments and
                request["executable"] == "mc_main.exe" and request["executable_sha256"] == before["executable_sha256"] and
                type(request["timeout_seconds"]) is int and request["timeout_seconds"] == timeout,
                "desktop request differs from native gameplay command")
        request_identity_verified = True
        transport["copied_before"] = desktop_copy_identity(jobs / job_id / "app", package_before)
        require(all(all(copied["file"][key] == original["package"][key] for key in ("size", "sha256"))
                    for copied, original in zip(transport["copied_before"], package_before)),
                "initial copied runtime closure differs from approved package")
        deadline = started + timeout + 60
        while True:
            require(time.monotonic() < deadline, "desktop capture polling deadline exceeded")
            status = desktop_client("Status", job_id=job_id, client_timeout=max(0.1, min(30, deadline - time.monotonic())))
            terminal = desktop_status(status, job_id, task_id, before["executable_sha256"], timeout)
            if terminal:
                transport["terminal_status"] = status
                exit_code, timed_out = status["exit_code"], status["state"] == "timed_out"
                break
            if time.monotonic() - heartbeat >= 30:
                print(f"CAPTURE windows: elapsed={time.monotonic() - started:.0f}s; job={job_id}, state={status['state']}", flush=True)
                heartbeat = time.monotonic()
            time.sleep(min(2, max(0, deadline - time.monotonic())))
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        errors.append(str(error) or type(error).__name__)
        timed_out = isinstance(error, subprocess.TimeoutExpired) or "deadline exceeded" in str(error)
        if job_id in preexisting_jobs:
            job_id = None
        if job_id is None:
            owned = []
            for target in jobs.iterdir():
                if target.name in preexisting_jobs:
                    continue
                try:
                    regular_path(target / "request.json")
                    request = read_json(target / "request.json")
                    if (desktop_job_id(request) == target.name and set(request) == DESKTOP_REQUEST_FIELDS and
                        request["task_id"] == task_id and request["arguments"] == arguments and request["executable"] == "mc_main.exe" and
                        request["executable_sha256"] == before["executable_sha256"] and request["timeout_seconds"] == timeout):
                        owned.append(target.name)
                except (OSError, ValueError, KeyError, TypeError):
                    continue
            if len(owned) == 1:
                job_id = owned[0]
                transport["job_id"] = job_id
                request_identity_verified = True
        if job_id is not None:
            transport["cleanup"] = cancel_desktop_job(job_id, task_id, before["executable_sha256"], timeout,
                                                       request_identity_verified)
            if transport["cleanup"].get("error"):
                errors.append(transport["cleanup"]["error"])
            cleanup_status = transport["cleanup"].get("terminal_status", {})
            exit_code = cleanup_status.get("exit_code")
        else:
            errors.append("submission did not establish one exact owned helper UUID; no retry or unrelated cancellation")
    if job_id is not None:
        job, app = jobs / job_id, jobs / job_id / "app"
        transport.update(job_directory=str(PureWindowsPath(str(job))), application_directory=str(PureWindowsPath(str(app))))
        command = [str(PureWindowsPath(str(app)) / "mc_main.exe"), *arguments]
        try:
            regular_path(job)
            for target in job.rglob("*"):
                regular_path(target)
            for name in ("request.json", "status.json", "scheduler.json", "broker.log", "stdout.log", "stderr.log"):
                if (job / name).exists():
                    transport["artifacts"][name] = copy_transport_file(job / name, retained / name, receipt.parent)
            if (app / report_name).is_file():
                regular_path(app / report_name)
                capture_started = datetime.datetime.fromisoformat(transport["started_at_utc"]).timestamp()
                require((app / report_name).stat().st_mtime >= capture_started, "native report predates fresh desktop capture")
                captured_report = copy_transport_file(app / report_name, report, receipt.parent)
            else:
                errors.append("native report is absent")
            transport["copied_after"] = desktop_copy_identity(app, package_before)
            require(transport["copied_before"] == transport["copied_after"], "native helper runtime closure changed during capture")
            for entry in package_before:
                relative = entry["relative"]
                descriptor = copy_transport_file(app / relative, retained / "app" / relative, receipt.parent)
                require(all(descriptor[key] == entry["original"][key] for key in ("size", "sha256")), "copied runtime input drift")
                transport["copied_inputs"].append({"relative": relative, "file": descriptor})
                if relative == "mc_main.exe":
                    captured_executable = descriptor
            actual = {target.relative_to(app).as_posix() for target in app.rglob("*") if target.is_file()}
            expected = {entry["relative"] for entry in package_before}
            require(actual in (expected, expected | {report_name}), "helper app contains unexpected runtime inputs")
        except (OSError, ValueError, KeyError, TypeError) as error:
            errors.append(str(error))
    try:
        transport["manifest"] = copy_transport_file(manifest, retained / "package-manifest.json", receipt.parent)
        transport["manifest_after"] = file_identity(manifest)
    except (OSError, ValueError, KeyError, TypeError) as error:
        errors.append(str(error))
    try:
        transport["build_after"] = build_identity(root, executable.parent, executable)
        transport["package_after"] = desktop_package_identity(root, executable, package, manifest)
        transport["helpers_after"] = desktop_helper_identity()
        require(before == transport["build_after"] and candidate == _MATRIX.candidate_identity(root),
                "source, executable, build, or dependency inputs changed during capture")
        require(package_before == transport["package_after"], "original package/runtime closure changed during capture")
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError, _MATRIX.MatrixError) as error:
        errors.append(str(error))
    transport["finished_at_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    row = {"platform": "windows", "proof": proof, "proof_origin": "local", "source_candidate": candidate,
           "source_input_sha256": before["source_input_sha256"], "build": before, "executable": captured_executable,
           "report": captured_report, "command": command, "warmup_seconds": warmup, "environment": {}, "exit_code": exit_code,
           "timed_out": timed_out, "capture_error": "", "transport": transport}
    if not errors:
        try:
            verify_desktop_transport(row, receipt.parent)
        except (OSError, ValueError, KeyError, TypeError, IndexError) as error:
            errors.append(str(error))
    row["capture_error"] = "; ".join(errors)
    with receipt.open("x", encoding="utf-8") as output:
        output.write(json.dumps({"schema_version": SCHEMA, "rows": [row]}, indent=2, sort_keys=True) + "\n")
    require(exit_code == 0 and not timed_out and not errors, "gameplay capture failed; retained failure receipt: " + row["capture_error"])
    print(json.dumps(verify_report(read_json(report), "windows", proof), sort_keys=True))


def record(root: Path, receipt: Path, platform: str, executable: Path, warmup: int,
           desktop_package: Path | None = None, desktop_manifest: Path | None = None, desktop_task_id: str | None = None,
           proof: str = "performance", macos_package: Path | None = None, macos_manifest: Path | None = None) -> None:
    require(proof in PROOFS, "invalid gameplay proof selector")
    require(warmup == 180, "qualification capture requires --warmup-seconds 180; other bounds are diagnostic only")
    require(platform == ("windows" if sys.platform == "win32" else "macos" if sys.platform == "darwin" else "unsupported"), "capture must run on the named local host")
    diagnostics = {key: value for key, value in os.environ.items() if key.startswith("MC_DIAGNOSTIC_")}
    require(not diagnostics, "remove diagnostic renderer overrides before qualification")
    require(not path_entry_exists(receipt), "capture receipt already exists; select a fresh output path")
    report = receipt.with_suffix(".raw.json")
    require(not path_entry_exists(report), "raw capture path already exists")
    print(f"CAPTURE {platform}: fingerprinting relevant source, Release build and consumed dependencies", flush=True)
    before = build_identity(root, executable.parent, executable)
    if desktop_package is not None:
        require(platform == "windows" and desktop_manifest is not None and desktop_task_id is not None,
                "desktop capture requires Windows, a runtime manifest and task ID")
        record_desktop(root, receipt, executable, warmup, before, desktop_package, desktop_manifest, desktop_task_id, proof)
        return
    macos_runtime = None
    if platform == "macos":
        require(macos_package is not None and macos_manifest is not None,
                "Mac capture requires --macos-package and --macos-package-manifest")
        require(not any(name.startswith(("DYLD_", "VK_")) for name in os.environ), "remove inherited macOS loader/driver overrides")
        original_runtime = macos_package_identity(macos_package, macos_manifest, executable, inspect_binaries=True)
        retained_package = receipt.parent.resolve() / receipt.with_suffix(".macos").name
        require(not path_entry_exists(retained_package), "retained macOS runtime already exists")
        for entry in original_runtime:
            source = macos_package / entry["relative"]
            target = retained_package / entry["relative"]
            copy_transport_file(source, target, receipt.parent.resolve())
            target.chmod(source.stat().st_mode & 0o777)
        retained_manifest = retained_package / "manifest.json"
        retained_runtime = macos_package_identity(retained_package, retained_manifest, executable)
        macos_runtime = {"kind": "normal-macos-package-v1", "capture_package": str(retained_package),
                         "original_before": original_runtime, "original_after": None,
                         "retained_before": retained_runtime, "retained_after": None,
                         "manifest": file_identity(retained_manifest),
                         "vulkan_loader": file_identity(retained_package / "lib/libvulkan.1.dylib"),
                         "moltenvk": file_identity(retained_package / "lib/libMoltenVK.dylib"),
                         "loaded_libraries": "not-observed; closure and normal launcher verified, PID mappings unavailable"}
    command = [str(executable), *benchmark_arguments(platform, warmup, str(report), proof)]
    started = time.monotonic()
    print(f"CAPTURE {platform}: bounded full-radius gameplay, timeout={warmup * 2 + 120}s", flush=True)
    captured_report = None
    failed_report = None
    capture_error = ""
    capture_directory = Path(tempfile.mkdtemp(prefix="mc-gameplay-capture-", dir=receipt.parent.resolve()))
    preserve_capture = False
    process = None
    process_finished = False
    try:
        native_report = capture_directory / "native.raw.json"
        launched_executable = str(retained_package / "MinecraftClone.command") if macos_runtime else command[0]
        actual_command = [launched_executable, *command[1:-1], str(native_report)]
        process = subprocess.Popen(actual_command, cwd=root)
        timed_out = False
        while process.poll() is None:
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                print(f"CAPTURE {platform}: elapsed={time.monotonic() - started:.0f}s; waiting for full-radius phases", flush=True)
                if time.monotonic() - started >= warmup * 2 + 120:
                    timed_out = True
                    process.kill()
                    process.wait(timeout=30)
        process_finished = True
        if native_report.is_file():
            try:
                retained_report = report.parent.resolve() / report.name
                copy_transport_file(native_report, retained_report, retained_report.parent)
                captured_report = file_identity(report)
            except (OSError, ValueError) as error:
                capture_error = str(error)
                preserve_capture = True
                failed_report = file_identity(native_report)
        else:
            capture_error = "native report is absent"
    finally:
        if process is not None and not process_finished:
            preserve_capture = True
            if process.poll() is None:
                process.kill()
                process.wait(timeout=30)
        if native_report.is_file() and captured_report is None:
            preserve_capture = True
        if not preserve_capture:
            shutil.rmtree(capture_directory)
    after = build_identity(root, executable.parent, executable)
    require(before == after, "source, executable, build, or dependency inputs changed during capture")
    if macos_runtime:
        try:
            macos_runtime["original_after"] = macos_package_identity(macos_package, macos_manifest, executable)
            macos_runtime["retained_after"] = macos_package_identity(retained_package, retained_manifest, executable)
            require(original_runtime == macos_runtime["original_after"] and retained_runtime == macos_runtime["retained_after"],
                    "macOS runtime inputs changed during capture")
        except (OSError, ValueError, KeyError, TypeError) as error:
            capture_error = "; ".join(filter(None, (capture_error, str(error))))
    row = {"platform": platform, "proof": proof, "proof_origin": "local", "source_candidate": _MATRIX.candidate_identity(root),
           "source_input_sha256": before["source_input_sha256"], "build": before,
           "executable": (next(entry["file"] for entry in retained_runtime if entry["relative"] == "mc_main")
                          if macos_runtime else file_identity(executable)),
           "report": captured_report, "command": command, "actual_command": actual_command,
           "native_report_path": str(native_report), "warmup_seconds": warmup, "environment": diagnostics,
           "exit_code": process.returncode, "timed_out": timed_out, "capture_error": capture_error}
    if macos_runtime:
        row["macos_runtime"] = macos_runtime
    if failed_report is not None:
        row["failed_report"] = failed_report
    with receipt.open("x", encoding="utf-8") as output:
        output.write(json.dumps({"schema_version": SCHEMA, "rows": [row]}, indent=2, sort_keys=True) + "\n")
    require(process.returncode == 0 and not timed_out and not capture_error,
            "gameplay capture failed; retained failure receipt" + (": " + capture_error if capture_error else ""))
    print(json.dumps(verify_report(read_json(report), platform, proof), sort_keys=True))


def merge_receipts(
    root: Path,
    inputs: list[Path],
    output: Path,
    *,
    s7_windows_secondary: bool = False,
    s7_cadence_exception: str | None = None,
) -> dict[str, Any]:
    require(not path_entry_exists(output), "aggregate receipt already exists; select a fresh output path")
    if s7_windows_secondary or s7_cadence_exception is not None:
        _CHECK.require_s7_candidate(root)
    selected_platforms = ("macos",) if s7_windows_secondary else tuple(TARGET_HZ)
    required = {(platform, proof) for platform in selected_platforms for proof in PROOFS}
    require(len(inputs) == len(required), "merge requires exactly one input receipt for every selected gameplay proof")
    rows = []
    for target in inputs:
        partial = read_json(target)
        require(partial["schema_version"] == SCHEMA and len(partial["rows"]) == 1, "merge requires one captured row per input")
        row = partial["rows"][0]
        for key in ("executable", "report"):
            descriptor = row[key]
            if not (target.parent / descriptor["path"]).is_file():
                basename = PureWindowsPath(descriptor["path"]).name if row["platform"] == "windows" else Path(descriptor["path"]).name
                location = target.parent / basename
                if key == "executable" and "macos_runtime" in row:
                    location = target.parent / Path(row["macos_runtime"]["capture_package"]).name / "mc_main"
                descriptor["path"] = str(location)
            descriptor["path"] = str(checked_file(descriptor, target.parent).resolve())
        if "transport" in row:
            descriptors = [*row["transport"]["artifacts"].values(),
                           row["transport"]["manifest"],
                           *(entry["file"] for entry in row["transport"]["copied_inputs"])]
            for descriptor in descriptors:
                descriptor["path"] = str(checked_file(descriptor, target.parent).resolve())
        if "macos_runtime" in row:
            runtime = row["macos_runtime"]
            for phase in ("retained_before", "retained_after"):
                for entry in runtime[phase]:
                    descriptor = entry["file"]
                    if not (target.parent / descriptor["path"]).is_file():
                        descriptor["path"] = str(target.parent / Path(runtime["capture_package"]).name / entry["relative"])
                    descriptor["path"] = str(checked_file(descriptor, target.parent).resolve())
            files = {entry["relative"]: entry["file"] for entry in runtime["retained_before"]}
            for key, relative in (("manifest", "manifest.json"), ("vulkan_loader", "lib/libvulkan.1.dylib"), ("moltenvk", "lib/libMoltenVK.dylib")):
                runtime[key] = dict(files[relative])
        rows.append(row)
    require(
        len(rows) == len(required)
        and {(row["platform"], row.get("proof")) for row in rows} == required,
        "merge inputs do not match the selected gameplay proof policy",
    )
    with output.open("x", encoding="utf-8") as receipt_file:
        receipt_file.write(json.dumps({"schema_version": SCHEMA, "rows": rows}, indent=2, sort_keys=True) + "\n")
    return verify_receipt(root, output, s7_windows_secondary=s7_windows_secondary,
                          s7_cadence_exception=s7_cadence_exception)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--candidate-commit")
    parser.add_argument("--record", choices=tuple(TARGET_HZ))
    parser.add_argument("--merge", nargs="+", type=Path, metavar="PROOF_RECEIPT")
    parser.add_argument(
        "--s7-windows-secondary",
        action="store_true",
        help="for the selected S7 snapshot only, require Mac proofs and report Windows as deferred",
    )
    parser.add_argument("--proof", choices=tuple(PROOFS), help="one explicitly selected capture proof")
    parser.add_argument("--s7-cadence-exception", choices=(S7_CADENCE_EXCEPTION,),
                        help="explicitly accept only the documented S7 Mac cadence threshold exception, never a performance PASS")
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--desktop-package", type=Path, help="Windows only: approved native runtime package for the installed desktop helper")
    parser.add_argument("--desktop-package-manifest", type=Path, help="exact relative/size/SHA256 inventory of the approved package")
    parser.add_argument("--desktop-task-id", help="safe task identity for this one native desktop helper submission")
    parser.add_argument("--macos-package", type=Path, help="normal extracted macOS package root")
    parser.add_argument("--macos-package-manifest", type=Path, help="generated macOS package manifest")
    parser.add_argument("--warmup-seconds", type=int, default=180)
    args = parser.parse_args(argv)
    try:
        root = args.root.resolve(strict=True)
        if args.candidate_commit:
            _MATRIX.commit_identity(root, args.candidate_commit)
            source_check = subprocess.run(["git", "diff", "--quiet", args.candidate_commit, "--", *SOURCE_INPUTS], cwd=root, timeout=30)
            require(source_check.returncode == 0, "candidate commit has different gameplay source inputs")
            untracked = subprocess.run(["git", "ls-files", "--others", "--exclude-standard", "--", *SOURCE_INPUTS], cwd=root,
                                      capture_output=True, text=True, timeout=30)
            require(untracked.returncode == 0 and not untracked.stdout.strip(), "candidate has uncommitted gameplay source files")
        require(not (args.record and args.merge), "--record and --merge are mutually exclusive")
        require(not (args.s7_windows_secondary and args.record), "S7 Windows-secondary applies only to aggregate verification")
        require(not args.s7_cadence_exception or not args.record,
                "S7 cadence exception applies only to aggregate verification, not capture")
        desktop_options = (args.desktop_package, args.desktop_package_manifest, args.desktop_task_id)
        require(not any(desktop_options) or args.record == "windows" and all(desktop_options),
                "desktop options require --record windows, --desktop-package, --desktop-package-manifest and --desktop-task-id")
        require(not (args.macos_package or args.macos_package_manifest) or args.record == "macos"
                and args.macos_package is not None and args.macos_package_manifest is not None,
                "macOS package options require --record macos and both package paths")
        if args.record or args.merge:
            require(not path_entry_exists(args.receipt), "output receipt already exists; select a fresh output path")
        if args.record:
            require(args.executable is not None, "--record requires --executable")
            require(args.proof is not None, "--record requires --proof")
            record(root, args.receipt.absolute(), args.record, args.executable.resolve(strict=True), args.warmup_seconds,
                   args.desktop_package.absolute() if args.desktop_package else None,
                   args.desktop_package_manifest.absolute() if args.desktop_package_manifest else None, args.desktop_task_id, args.proof,
                   args.macos_package.absolute() if args.macos_package else None,
                   args.macos_package_manifest.absolute() if args.macos_package_manifest else None)
        else:
            results = (
                merge_receipts(
                    root,
                    [target.resolve(strict=True) for target in args.merge],
                    args.receipt.absolute(),
                    s7_windows_secondary=args.s7_windows_secondary,
                    s7_cadence_exception=args.s7_cadence_exception,
                )
                if args.merge else verify_receipt(
                    root,
                    args.receipt.resolve(strict=True),
                    s7_windows_secondary=args.s7_windows_secondary,
                    s7_cadence_exception=args.s7_cadence_exception,
                )
            )
            accepted = any(proof.get("status") == "accepted_exception"
                           for platform in results.values() for proof in platform.values() if isinstance(proof, dict))
            print(json.dumps({"status": "accepted_exception" if accepted else "passed",
                              "platforms": results}, sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError, TypeError, IndexError, subprocess.SubprocessError, _MATRIX.MatrixError) as error:
        print(f"FAIL gameplay gate: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
