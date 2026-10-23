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
WORKLOAD = "turning-x30-movement-v3"
COVERAGE_WORKLOAD = "speed-200-full-radius-recovery-v1"
SOURCE_INPUTS = ("src/**", "assets/**", "cmake/**", "scenarios/**", "CMakeLists.txt", "CMakePresets.json",
                 "dependencies.lock.json", "tests/**", "vcpkg.json", "vcpkg-configuration*.json")
REQUIRED_SOURCE_INPUTS = (
    "CMakeLists.txt",
    "CMakePresets.json",
    "dependencies.lock.json",
    "scenarios/world/canonical_world.core",
    "scenarios/world/height_tile.core",
)
PHASES = ("initial-full-radius-warmup", "baseline-stationary", "full-radius-movement",
          "full-radius-recovery-warmup", "recovered-stationary")
S1_FILL_PHASE = "s1-fill-ramp"
S1_MOVEMENT_PHASE = "s1-x30-movement"
S1_PERFORMANCE_CONTRACT = "mc.s1-lod-performance.v2"
S1_OWNER_EXCEPTION_SCHEMA = "mc.s1-owner-accepted-gameplay-exception.v1"
S1_OWNER_REQUEST_PATH = "~/.codex/task-evidence/mc-s1-20261023/owner-release-acceptance/request.md"
S1_OWNER_REQUEST_SHA256 = "b55134d42a04a746ca862d4b12ba847cb8e0e73ac88ae1ca787b13daf6ebe403"
S1_OWNER_SOURCE_INPUT_SHA256 = "83da03165eea8b14d48398b8ca314b0ddcb3d74fa6b446b998f819a2538c3e9d"
S1_OWNER_SOURCE_REUSE_SCHEMA = "mc.s1-source-reuse-certificate.v3"
S1_OWNER_ACCEPTED_SOURCE_ROOT = Path.home() / ".codex/task-evidence/mc-s1-20261022/architecture-audit-01/aggregate-review-01/delta-cache-recorder-01/candidate"
S1_OWNER_SOURCE_REUSE_CURRENT_SHA256 = 'fd08d7464b1ec7c80d4a494e8e9e471baafcd7f8e85a1220923d453945b93aa1'
S1_OWNER_SOURCE_REUSE_FILE_HASHES = {
    "src/shared/corelang/Execution.cpp": {
        "accepted_sha256": "37074292464f75c2ca6d96747202f3a3bada734541817a558218fd115fdefd99",
        "current_sha256": "eda9f4ef465be27b97a0cacc6f59f2b9775517a960acfd0e16f4e9800cf6aaaf",
    },
    "tests/acceptance/column_map_replay_tests.cpp": {
        "accepted_sha256": "dadb3a9daf5563839e3128bb25884da3b5f071c991826ac6c2307f4023fffb4c",
        "current_sha256": "36b3f02c9ce3ff7fd57d43b6cd4f78f962e2fd1077ab08a2368a89601f9c78c0",
    },
}
S1_OWNER_SOURCE_REUSE_VERSION_FILES = {
    "CMakeLists.txt": {
        "accepted_sha256": "317bb3826155f357af28ac04b254662fefb14dbe5282c7bed354743e77c8a596",
        "current_sha256": "6e285b7a9f39c9b65af1fbb7442484f888d6a1eef352f7930d9767844c381297",
    },
    "vcpkg.json": {
        "accepted_sha256": "c4199f22a2a4d7fbb128e1b861b381b091eeeb7336c5909d4ae16335b1563724",
        "current_sha256": "00b4e1010a1f5e8a7ea196a69c295eee5240849c22312b1b09855f1d0780c945",
    },
    "src/shared/include/shared/ProjectInfo.hpp": {
        "accepted_sha256": "5e5be64a6f6b8574da372911b28194ab468ce88c29ece5491a564167dceacfb1",
        "current_sha256": "46eec31959f1b7828cfa62cfeadf3689bcd0c49030fa81e99c70b73e36031cb8",
    },
}
S1_OWNER_SOURCE_REUSE_VERSION_REPLACEMENTS = {
    "CMakeLists.txt": [(b"VERSION 0.1.0\n", b"VERSION 0.1.1\n")],
    "vcpkg.json": [(b'"version-string": "0.1.0"', b'"version-string": "0.1.1"')],
    "src/shared/include/shared/ProjectInfo.hpp": [
        (b'MINOR_VERSION_NAME{ "Initiation" }', b'MINOR_VERSION_NAME{ "BasicBlocks" }'),
        (b'.minor = 0,', b'.minor = 1,'),
        (b'.patch = 7,', b'.patch = 1,'),
    ],
}
S1_OWNER_SOURCE_REUSE_TEST_FIXTURE_HASHES = {
    "tests/acceptance/adverse_network_tests.cpp": {
        "accepted_sha256": "c4bafd8890b2eac5051789e0534b83ec1578be6fc76e220769abf840a1b53a2f",
        "current_sha256": "4b96d636d353fb9c85e5b2817ee27dc861fde123ceee2af550da19ff8c9b448c",
    },
    "tests/client/player_client_focus_tests.cpp": {
        "accepted_sha256": "807e6e4cf308ad154e30ea139514ace679ac517d53867134f98861f8b7263aca",
        "current_sha256": "98202508eb4a28708e76302afc644a2dfc78327065ae87ff840f72399eb40d85",
    },
    "tests/client/renderer_smoke_tests.cpp": {
        "accepted_sha256": "5b23541ce5b08c565b448fd59b303513c67d06a923988fea8ff74fd360daf540",
        "current_sha256": "2311f8044975f27a2fb516496ca527cb82185fb11683f47ca6d9d0bb7e729dc7",
    },
    "tests/core/game_server_preview_tests.cpp": {
        "accepted_sha256": "f8dc18c19d19e23ff5cf2a4279cd6aa86756e911ce0eaa02bcf0a8168e214a90",
        "current_sha256": "96d578b3d79e52893ba561ee4c1e3090efb8671f2f0faffe72dedca1dd25c1ab",
    },
}
S1_OWNER_SOURCE_INPUT_COUNT = 908
S1_OWNER_SOURCE_REUSE_UNCHANGED_INPUT_COUNT = 899
S1_OWNER_SOURCE_REUSE_APPLICATION_INPUT_COUNT = 233
S1_OWNER_SOURCE_REUSE_APPLICATION_UNCHANGED_INPUT_COUNT = 229
S1_OWNER_SOURCE_REUSE_TEST_INPUT_COUNT = 675
S1_OWNER_SOURCE_REUSE_TEST_UNCHANGED_INPUT_COUNT = 670
S1_OWNER_SOURCE_REUSE_APPLICATION_SHA256 = "dcc33a5b3f062aed0e67ae02d0e83d37f040ab00ed44209897a427bfda4d9bee"
S1_OWNER_SOURCE_REUSE_CURRENT_MAIN_PATH = Path.home() / ".codex/task-evidence/mc-s1-20261021/evidence-boundaries/build/mc_main"
S1_OWNER_SOURCE_REUSE_FREEZE_RECEIPT_PATH = Path.home() / ".codex/task-evidence/mc-s1-20261023/release-freeze-01/deferred-lod/FINAL-FREEZE-RECEIPT.json"
S1_OWNER_SOURCE_REUSE_BUILD_LOG_PATH = Path.home() / ".codex/task-evidence/mc-s1-20261023/release-freeze-01/deferred-lod/accepted-build.log"
S1_OWNER_SOURCE_REUSE_CURRENT_MAIN_PATH_LABEL = "~/.codex/task-evidence/mc-s1-20261021/evidence-boundaries/build/mc_main"
S1_OWNER_SOURCE_REUSE_FREEZE_RECEIPT_PATH_LABEL = "~/.codex/task-evidence/mc-s1-20261023/release-freeze-01/deferred-lod/FINAL-FREEZE-RECEIPT.json"
S1_OWNER_SOURCE_REUSE_BUILD_LOG_PATH_LABEL = "~/.codex/task-evidence/mc-s1-20261023/release-freeze-01/deferred-lod/accepted-build.log"
S1_OWNER_SOURCE_REUSE_CURRENT_MAIN_SHA256 = "0aacbd2ff732cc328e29b38509161a54c99b60706fcab0b98cf40495bc26fba7"
S1_OWNER_SOURCE_REUSE_FREEZE_RECEIPT_SHA256 = "78ca7ea1d3d3f01917e754165dc89791465ea947fd939238f1d54cd9790bfa0a"
S1_OWNER_SOURCE_REUSE_BUILD_LOG_SHA256 = "4ca42cfd59f341f93887df2b71d23f04558960d738e8f88f025e759eb0a282b7"
S1_OWNER_VERSION_IDENTITY_BUILD_RECEIPT_PATH = Path.home() / ".codex/task-evidence/mc-s1-20261023/release-reconciliation-01/version-identity-build-01/version-identity-build.json"
S1_OWNER_VERSION_IDENTITY_BUILD_LOG_PATH = Path.home() / ".codex/task-evidence/mc-s1-20261023/release-reconciliation-01/version-identity-build-01/build.log"
S1_OWNER_VERSION_IDENTITY_APP_PATH = Path.home() / ".codex/task-evidence/mc-s1-20261023/release-reconciliation-01/metadata-work/build/s1-version-identity-release/mc_main"
S1_OWNER_VERSION_IDENTITY_SYMBOLS_PATH = Path.home() / ".codex/task-evidence/mc-s1-20261023/release-reconciliation-01/version-identity-build-01/current-global-symbols.txt"
S1_OWNER_ACCEPTED_SYMBOLS_PATH = Path.home() / ".codex/task-evidence/mc-s1-20261023/release-reconciliation-01/version-identity-build-01/accepted-global-symbols.txt"
S1_OWNER_VERSION_IDENTITY_PROBE_PATH = Path.home() / ".codex/task-evidence/mc-s1-20261023/release-reconciliation-01/version-identity-build-01/version_identity_probe"
S1_OWNER_VERSION_IDENTITY_PROBE_RUN_LOG_PATH = Path.home() / ".codex/task-evidence/mc-s1-20261023/release-reconciliation-01/version-identity-build-01/version_identity_probe_run.log"
S1_OWNER_VERSION_IDENTITY_BUILD_RECEIPT_SHA256 = "bc0d803ae6e68914bc214f319c8e0951b4e26d5e23d222510f8e37e004eb3abf"
S1_OWNER_VERSION_IDENTITY_BUILD_LOG_SHA256 = "6b21f90d2cfa6ee18a5cf72039392f0bff4ab970b1a7aaa8526a6bbb7bbb0dee"
S1_OWNER_VERSION_IDENTITY_APP_SHA256 = "6a5b2f3174eb5715add8de61e061684f0a00ff1616f9285f350c140f569252bc"
S1_OWNER_VERSION_IDENTITY_SYMBOLS_SHA256 = "df6cdfac05d588cc32b8d41dfb98b6070e4b6a0b4c0b3d43e3fc4d6d9ac7ea6d"
S1_OWNER_VERSION_IDENTITY_PROBE_SHA256 = "1cd10ba5acd4f97121293c957fced4ecc5065847628388adeb8fc706139d81b3"
S1_OWNER_VERSION_IDENTITY_PROBE_RUN_LOG_SHA256 = "a65036a25541cf4ee59be2d12f521972f4aa5c701822b3ffdb5f34f86b50c16d"
S1_OWNER_VERSION_IDENTITY_BUILD_RECEIPT_PATH_LABEL = "~/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/version-identity-build-01/version-identity-build.json"
S1_OWNER_VERSION_IDENTITY_BUILD_LOG_PATH_LABEL = "~/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/version-identity-build-01/build.log"
S1_OWNER_VERSION_IDENTITY_APP_PATH_LABEL = "~/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/metadata-work/build/s1-version-identity-release/mc_main"
S1_OWNER_VERSION_IDENTITY_SYMBOLS_PATH_LABEL = "~/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/version-identity-build-01/current-global-symbols.txt"
S1_OWNER_ACCEPTED_SYMBOLS_PATH_LABEL = "~/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/version-identity-build-01/accepted-global-symbols.txt"
S1_OWNER_VERSION_IDENTITY_PROBE_PATH_LABEL = "~/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/version-identity-build-01/version_identity_probe"
S1_OWNER_VERSION_IDENTITY_PROBE_RUN_LOG_PATH_LABEL = "~/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/version-identity-build-01/version_identity_probe_run.log"
S1_OWNER_AGGREGATE_SHA256 = "abcda853c0a6af2b87526723280055ab1040c38f6c3ac265c3c538e54cdbfd06"
S1_OWNER_RAW_REPORT_SHA256 = "94140622009202bad4ee05b98952fec86d41aecd9f8cb8a732073ab0f5fd102d"
S1_OWNER_MAC_EXECUTABLE_SHA256 = "0aacbd2ff732cc328e29b38509161a54c99b60706fcab0b98cf40495bc26fba7"
S1_OWNER_CAPTURE_CANDIDATE = {
    "head": "bf5047ac52f397179e8467998f6fe67604a6e5f5",
    "tree": "6c2f5e415748a4d3803207c74b3e09cbe911a300",
}
S1_OWNER_WINDOWS_EXECUTABLE_SHA256 = "cba2572c3cd47b9da82a2ad2ab1f958231ea20b57b9a6ddad17107013d89402b"
ACTUAL_PRESENTATION_CONTRACT = "mc.actual-presentation-timing.v1"
ACTUAL_PRESENTATION_PROVIDER = "VK_GOOGLE_display_timing"
ACTUAL_PRESENTATION_CLOCK_DOMAIN = "vulkan-display-timing-monotonic-nanoseconds-unspecified-origin"
TARGET_HZ = {"macos": 120, "windows": 120}
FRESH_FRAME_TARGET_HZ = {"macos": 120, "windows": 300}
PROOFS = {"terrain-coverage": 256, "performance": 256}
PERFORMANCE_SAMPLE_MS = 30_000
PERFORMANCE_SAMPLE_OVERRUN_MS = 1_000
FRAME_BUDGET_SCOPE = "fence-confirmed-render-frame-completion-observations"
FRAME_COMPLETION_SOURCE = "per-slot-submitted-frame-fence-success-observed-on-reuse"
FRAME_BUDGET_TARGET_NS = {"macos": 8_333_333, "windows": 3_333_333}
FRAME_BUDGET_REQUIRED_PERMILLE = 990
PLAYABLE_SPHERE_RADIUS_BLOCKS = 320
S1_FULL_VOLUME_FIELDS = {"coverage_scope", "required_authoritative_columns",
                         "resident_authoritative_columns", "vertical_chunk_count", "authoritative_columns_center"}

ACTUAL_TERRAIN_RADIUS_BLOCKS = 640
VISUAL_LOD_RADIUS_BLOCKS = 320
PLAYABLE_SPHERE_CONTRACT = "mc.authoritative-materialized-playable-sphere.v1"
PLAYABLE_SPHERE_SAMPLE_SCOPE = "per-movement-loop-and-every-authoritative-position-update"
MOVEMENT_TRAJECTORY_CONTRACT = "mc.x30-authoritative-trajectory.v1"
MOVEMENT_TRAJECTORY_SAMPLE_SCOPE = "per-movement-loop-and-every-in-window-movement-input-ack"
MOVEMENT_SUBCELLS_PER_TICK = 5_600
SUBCELLS_PER_CELL = 10_000
FLIGHT_HORIZONTAL_EXTENT = 65_536 * SUBCELLS_PER_CELL
FLIGHT_MAX_Z = 1_023
FLIGHT_SPEEDUP_PROFILES = frozenset((2, 3, 5, 8, 15, 30, 80, 200, 500))
# Mirrors shared::TICK in src/shared/include/shared/world/World.hpp.
SERVER_TICK_NS = 100_000_000
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


@functools.lru_cache(maxsize=1)
def qualification_settings_manifest() -> dict[str, Any]:
    source = (SCRIPT_ROOT.parent / "src/acceptance/include/acceptance/GameBenchmark.hpp").read_text(encoding="utf-8")
    literal = r'"(?:\\.|[^"\\])*"'
    match = re.search(r'GAME_BENCHMARK_S1_QUALIFICATION_MANIFEST\s*=\s*('
                      + r'(?:' + literal + r'\s*)+);', source, re.S)
    if match is None:
        raise GameplayError("source qualification settings manifest is absent or not a literal initializer")
    try:
        encoded = "".join(json.loads(piece) for piece in re.findall(literal, match.group(1), re.S))
        manifest = json.loads(encoded)
    except json.JSONDecodeError:
        raise GameplayError("source qualification settings manifest literal is malformed") from None
    require(manifest.get("contract") == "mc.s1-qualification-settings.v1"
            and set(manifest.get("platforms", {})) == {"macos", "windows"},
            "source qualification settings manifest is malformed")
    return manifest


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


def s1_owner_source_reuse_build_evidence() -> dict[str, Any]:
    """Bind app-source reuse to the accepted rebuilt Mac binary and its receipt."""
    paths = {
        "application_executable": (
            S1_OWNER_SOURCE_REUSE_CURRENT_MAIN_PATH,
            S1_OWNER_SOURCE_REUSE_CURRENT_MAIN_SHA256,
            S1_OWNER_SOURCE_REUSE_CURRENT_MAIN_PATH_LABEL,
        ),
        "freeze_receipt": (
            S1_OWNER_SOURCE_REUSE_FREEZE_RECEIPT_PATH,
            S1_OWNER_SOURCE_REUSE_FREEZE_RECEIPT_SHA256,
            S1_OWNER_SOURCE_REUSE_FREEZE_RECEIPT_PATH_LABEL,
        ),
        "build_log": (
            S1_OWNER_SOURCE_REUSE_BUILD_LOG_PATH,
            S1_OWNER_SOURCE_REUSE_BUILD_LOG_SHA256,
            S1_OWNER_SOURCE_REUSE_BUILD_LOG_PATH_LABEL,
        ),
    }
    identities = {}
    for name, (path, expected_sha256, path_label) in paths.items():
        require(path.is_file(), f"S1 source-reuse {name} is unavailable")
        actual_sha256 = hashlib.sha256(path.read_bytes()).hexdigest()
        require(actual_sha256 == expected_sha256,
                f"S1 source-reuse {name} does not match the accepted rebuild receipt")
        identities[name] = {
            "path": path_label,
            "sha256": actual_sha256,
        }

    freeze = read_json(S1_OWNER_SOURCE_REUSE_FREEZE_RECEIPT_PATH)
    require(isinstance(freeze, dict)
            and freeze.get("build_exit") == 0
            and freeze.get("source_scope_drift") == []
            and freeze.get("bit_identical_to_accepted_mac") is True
            and freeze.get("mc_main_sha256") == S1_OWNER_SOURCE_REUSE_CURRENT_MAIN_SHA256,
            "S1 source-reuse rebuild receipt is incomplete or mismatched")
    return identities


def s1_owner_source_reuse_version_identity_build_evidence() -> dict[str, Any]:
    """Bind the version-only Mac rebuild without replacing the owner-tested app."""
    pinned_files = {
        "application_executable": (S1_OWNER_VERSION_IDENTITY_APP_PATH, S1_OWNER_VERSION_IDENTITY_APP_SHA256,
                                    S1_OWNER_VERSION_IDENTITY_APP_PATH_LABEL),
        "build_log": (S1_OWNER_VERSION_IDENTITY_BUILD_LOG_PATH, S1_OWNER_VERSION_IDENTITY_BUILD_LOG_SHA256,
                      S1_OWNER_VERSION_IDENTITY_BUILD_LOG_PATH_LABEL),
        "receipt": (S1_OWNER_VERSION_IDENTITY_BUILD_RECEIPT_PATH,
                    S1_OWNER_VERSION_IDENTITY_BUILD_RECEIPT_SHA256,
                    S1_OWNER_VERSION_IDENTITY_BUILD_RECEIPT_PATH_LABEL),
        "accepted_symbol_list": (S1_OWNER_ACCEPTED_SYMBOLS_PATH,
                                  S1_OWNER_VERSION_IDENTITY_SYMBOLS_SHA256,
                                  S1_OWNER_ACCEPTED_SYMBOLS_PATH_LABEL),
        "current_symbol_list": (S1_OWNER_VERSION_IDENTITY_SYMBOLS_PATH,
                                 S1_OWNER_VERSION_IDENTITY_SYMBOLS_SHA256,
                                 S1_OWNER_VERSION_IDENTITY_SYMBOLS_PATH_LABEL),
        "version_probe": (S1_OWNER_VERSION_IDENTITY_PROBE_PATH,
                          S1_OWNER_VERSION_IDENTITY_PROBE_SHA256,
                          S1_OWNER_VERSION_IDENTITY_PROBE_PATH_LABEL),
        "version_probe_run_log": (S1_OWNER_VERSION_IDENTITY_PROBE_RUN_LOG_PATH,
                                  S1_OWNER_VERSION_IDENTITY_PROBE_RUN_LOG_SHA256,
                                  S1_OWNER_VERSION_IDENTITY_PROBE_RUN_LOG_PATH_LABEL),
    }
    identities = {}
    for name, (path, expected_sha256, path_label) in pinned_files.items():
        require(path.is_file(), f"S1 version-identity {name} is unavailable")
        actual_sha256 = hashlib.sha256(path.read_bytes()).hexdigest()
        require(actual_sha256 == expected_sha256,
                f"S1 version-identity {name} differs from the pinned local rebuild evidence")
        identities[name] = {"path": path_label, "sha256": actual_sha256}

    receipt = read_json(S1_OWNER_VERSION_IDENTITY_BUILD_RECEIPT_PATH)
    expected_source_root = S1_OWNER_VERSION_IDENTITY_APP_PATH.parents[2]
    require(isinstance(receipt, dict)
            and receipt.get("schema") == "mc.s1-version-identity-build.v1"
            and receipt.get("build_exit") == 0
            and Path(str(receipt.get("source_root", ""))) == expected_source_root
            and receipt.get("source_input_sha256") == S1_OWNER_SOURCE_REUSE_V3_CURRENT_SHA256
            and receipt.get("accepted_capture_source_input_sha256") == S1_OWNER_SOURCE_INPUT_SHA256
            and receipt.get("application_behavior_input_sha256_after_exact_identity_normalization")
                == S1_OWNER_SOURCE_REUSE_APPLICATION_SHA256
            and receipt.get("build_configuration", {}).get("configuration") == "Release"
            and receipt.get("build_configuration", {}).get("target") == "mc_main"
            and receipt.get("build_configuration", {}).get("arm64_triplet") == "arm64-osx"
            and receipt.get("build_configuration", {}).get("sanitizers") is False
            and receipt.get("build_configuration", {}).get("high_assertions") is False,
            "S1 version-identity build receipt does not bind the accepted source, exact version candidate, and Mac Release configuration")
    require(receipt.get("accepted_application", {}).get("sha256") == S1_OWNER_SOURCE_REUSE_CURRENT_MAIN_SHA256
            and receipt.get("version_identity_application", {}).get("sha256")
                == S1_OWNER_VERSION_IDENTITY_APP_SHA256
            and receipt.get("version_identity_application", {}).get("path")
                == str(S1_OWNER_VERSION_IDENTITY_APP_PATH)
            and receipt.get("version_identity_application", {}).get("global_symbol_set_identical") is True
            and receipt.get("accepted_application", {}).get("global_symbol_sha256")
                == receipt.get("version_identity_application", {}).get("global_symbol_sha256")
            and receipt.get("version_identity_application", {}).get("symbol_list_sha256")
                == S1_OWNER_VERSION_IDENTITY_SYMBOLS_SHA256
            and receipt.get("accepted_application", {}).get("symbol_list_sha256")
                == S1_OWNER_VERSION_IDENTITY_SYMBOLS_SHA256
            and receipt.get("version_identity_application", {}).get("symbol_count") == 3407,
            "S1 version-identity build changed the accepted symbol surface or application identity")
    build_log = S1_OWNER_VERSION_IDENTITY_BUILD_LOG_PATH.read_text(encoding="utf-8", errors="replace")
    require("[100/100] Linking CXX executable mc_main" in build_log,
            "S1 version-identity build log does not show a complete mc_main link")
    accepted_symbols = S1_OWNER_ACCEPTED_SYMBOLS_PATH.read_bytes()
    current_symbols = S1_OWNER_VERSION_IDENTITY_SYMBOLS_PATH.read_bytes()
    require(accepted_symbols == current_symbols,
            "S1 version-identity rebuild exported a different global symbol set")
    probe = receipt.get("version_identity_probe", {})
    require(probe.get("compile_exit") == 0
            and probe.get("run_exit") == 0
            and probe.get("formatted_output") == "0.1.1:1 BasicBlocks"
            and probe.get("executable_sha256") == S1_OWNER_VERSION_IDENTITY_PROBE_SHA256
            and probe.get("run_log_sha256") == S1_OWNER_VERSION_IDENTITY_PROBE_RUN_LOG_SHA256
            and S1_OWNER_VERSION_IDENTITY_PROBE_RUN_LOG_PATH.read_text(encoding="utf-8").strip()
                == "0.1.1:1 BasicBlocks",
            "S1 version-identity formatter probe did not produce the exact candidate label")
    return {
        "schema": receipt["schema"],
        "source_input_sha256": receipt["source_input_sha256"],
        "behavior_input_sha256_after_exact_identity_normalization":
            receipt["application_behavior_input_sha256_after_exact_identity_normalization"],
        "owner_tested_application_sha256": S1_OWNER_SOURCE_REUSE_CURRENT_MAIN_SHA256,
        "version_identity_application": identities["application_executable"],
        "build_log": identities["build_log"],
        "receipt": identities["receipt"],
        "global_symbols": {
            "count": 3407,
            "sha256": receipt["version_identity_application"]["global_symbol_sha256"],
            "identical_to_owner_tested_application": True,
        },
        "formatted_version_probe": {
            "output": "0.1.1:1 BasicBlocks",
            "executable": identities["version_probe"],
            "run_log": identities["version_probe_run_log"],
        },
    }


S1_OWNER_SOURCE_REUSE_V3_CURRENT_SHA256 = '75b2a0951e88e1ebc38a6776aaf191071ab1445a41d6c500c2cf438e12d1d641'
def s1_owner_source_reuse_certificate_v3(root: Path, current_source_sha256: str) -> dict[str, Any]:
    """Bind retained gameplay to unchanged app inputs and exact approved deltas."""
    accepted_root = S1_OWNER_ACCEPTED_SOURCE_ROOT
    require(accepted_root.is_dir(), "approved S1 source candidate is unavailable for reuse verification")
    current_inputs = _MATRIX._input_file_paths(root, SOURCE_INPUTS)
    accepted_inputs = _MATRIX._input_file_paths(accepted_root, SOURCE_INPUTS)
    require(len(current_inputs) == S1_OWNER_SOURCE_INPUT_COUNT
            and len(accepted_inputs) == S1_OWNER_SOURCE_INPUT_COUNT
            and set(current_inputs) == set(accepted_inputs),
            "S1 source-reuse inventory differs from the approved 908-input candidate")
    require(source_digest(accepted_root) == S1_OWNER_SOURCE_INPUT_SHA256,
            "approved S1 source candidate no longer matches the retained Mac capture")
    require(current_source_sha256 == S1_OWNER_SOURCE_REUSE_V3_CURRENT_SHA256,
            "current S1 source inputs do not match the exact owner-reviewed candidate")

    normalized_files = []
    test_fixture_files = []
    version_only_files = []
    accepted_application = hashlib.sha256()
    equivalent_application = hashlib.sha256()
    raw_current_application = hashlib.sha256()
    application_paths = {relative for relative in current_inputs if not relative.startswith("tests/")}
    test_paths = set(current_inputs) - application_paths
    require(len(application_paths) == S1_OWNER_SOURCE_REUSE_APPLICATION_INPUT_COUNT
            and len(test_paths) == S1_OWNER_SOURCE_REUSE_TEST_INPUT_COUNT,
            "S1 source-reuse app/test input partition changed")

    def update_digest(digest: Any, relative: str, contents: bytes) -> None:
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(len(contents).to_bytes(8, byteorder="big"))
        digest.update(contents)

    for relative in sorted(current_inputs):
        current_bytes = current_inputs[relative].read_bytes()
        accepted_bytes = accepted_inputs[relative].read_bytes()
        if relative in S1_OWNER_SOURCE_REUSE_FILE_HASHES:
            expected = S1_OWNER_SOURCE_REUSE_FILE_HASHES[relative]
            require(hashlib.sha256(accepted_bytes).hexdigest() == expected["accepted_sha256"]
                    and hashlib.sha256(current_bytes).hexdigest() == expected["current_sha256"]
                    and accepted_bytes.endswith(b"\n\n")
                    and current_bytes == accepted_bytes.rstrip(b"\n") + b"\n",
                    f"S1 source-reuse file is not the authorized EOF-only normalization: {relative}")
            trailing_lines = len(accepted_bytes) - len(accepted_bytes.rstrip(b"\n"))
            normalized_files.append({
                "path": relative,
                "input_class": "test" if relative.startswith("tests/") else "application",
                "accepted_sha256": expected["accepted_sha256"],
                "current_sha256": expected["current_sha256"],
                "accepted_trailing_lf_count": trailing_lines,
                "current_trailing_lf_count": 1,
            })
            equivalence_bytes = accepted_bytes
        elif relative in S1_OWNER_SOURCE_REUSE_VERSION_FILES:
            expected = S1_OWNER_SOURCE_REUSE_VERSION_FILES[relative]
            require(relative in application_paths
                    and hashlib.sha256(accepted_bytes).hexdigest() == expected["accepted_sha256"]
                    and hashlib.sha256(current_bytes).hexdigest() == expected["current_sha256"],
                    f"S1 version identity input differs from its pinned candidate: {relative}")
            transformed = accepted_bytes
            literal_changes = []
            for before, after in S1_OWNER_SOURCE_REUSE_VERSION_REPLACEMENTS[relative]:
                require(transformed.count(before) == 1,
                        f"S1 version identity transform is not unique: {relative}")
                transformed = transformed.replace(before, after, 1)
                literal_changes.append({"from": before.decode("utf-8"), "to": after.decode("utf-8")})
            require(current_bytes == transformed,
                    f"S1 version identity file contains a non-version change: {relative}")
            version_only_files.append({
                "path": relative,
                "input_class": "application",
                "accepted_sha256": expected["accepted_sha256"],
                "current_sha256": expected["current_sha256"],
                "literal_changes": literal_changes,
            })
            # Version identity affects display/build metadata only. For the
            # retained gameplay comparison, substitute the exact accepted bytes.
            equivalence_bytes = accepted_bytes
        elif relative in S1_OWNER_SOURCE_REUSE_TEST_FIXTURE_HASHES:
            expected = S1_OWNER_SOURCE_REUSE_TEST_FIXTURE_HASHES[relative]
            require(relative.startswith("tests/")
                    and hashlib.sha256(accepted_bytes).hexdigest() == expected["accepted_sha256"]
                    and hashlib.sha256(current_bytes).hexdigest() == expected["current_sha256"]
                    and current_bytes != accepted_bytes,
                    f"S1 source-reuse test fixture differs from its exact owner-reviewed update: {relative}")
            test_fixture_files.append({
                "path": relative,
                "accepted_sha256": expected["accepted_sha256"],
                "current_sha256": expected["current_sha256"],
            })
            equivalence_bytes = current_bytes
        else:
            require(current_bytes == accepted_bytes,
                    f"S1 source-reuse certificate rejects changed input: {relative}")
            equivalence_bytes = current_bytes
        if relative in application_paths:
            update_digest(accepted_application, relative, accepted_bytes)
            update_digest(equivalent_application, relative, equivalence_bytes)
            update_digest(raw_current_application, relative, current_bytes)

    require([entry["path"] for entry in normalized_files]
            == sorted(S1_OWNER_SOURCE_REUSE_FILE_HASHES),
            "S1 source-reuse certificate is missing an authorized EOF-only normalization")
    require([entry["path"] for entry in test_fixture_files]
            == sorted(S1_OWNER_SOURCE_REUSE_TEST_FIXTURE_HASHES),
            "S1 source-reuse certificate is missing an exact test-fixture update")
    require([entry["path"] for entry in version_only_files]
            == sorted(S1_OWNER_SOURCE_REUSE_VERSION_FILES),
            "S1 source-reuse certificate is missing an exact version identity input")
    require(len(current_inputs) - len(normalized_files) - len(test_fixture_files) - len(version_only_files)
            == S1_OWNER_SOURCE_REUSE_UNCHANGED_INPUT_COUNT,
            "S1 source-reuse certificate does not preserve the exact unchanged-input count")
    require(len(application_paths) - sum(
                entry["input_class"] == "application" for entry in normalized_files
            ) - len(version_only_files) == S1_OWNER_SOURCE_REUSE_APPLICATION_UNCHANGED_INPUT_COUNT
            and len(test_paths) - sum(
                entry["input_class"] == "test" for entry in normalized_files
            ) - len(test_fixture_files) == S1_OWNER_SOURCE_REUSE_TEST_UNCHANGED_INPUT_COUNT,
            "S1 source-reuse application/test unchanged-input counts changed")
    accepted_application_sha256 = accepted_application.hexdigest()
    equivalent_application_sha256 = equivalent_application.hexdigest()
    require(accepted_application_sha256 == equivalent_application_sha256
            == S1_OWNER_SOURCE_REUSE_APPLICATION_SHA256,
            "S1 application behavior inputs differ after exact approved normalizations")
    return {
        "schema": S1_OWNER_SOURCE_REUSE_SCHEMA,
        "capture_source_input_sha256": S1_OWNER_SOURCE_INPUT_SHA256,
        "current_source_input_sha256": current_source_sha256,
        "source_input_count": len(current_inputs),
        "unchanged_input_count": len(current_inputs) - len(normalized_files) - len(test_fixture_files) - len(version_only_files),
        "application_input_count": len(application_paths),
        "application_unchanged_input_count": S1_OWNER_SOURCE_REUSE_APPLICATION_UNCHANGED_INPUT_COUNT,
        "application_source_input_sha256": equivalent_application_sha256,
        "application_current_raw_input_sha256": raw_current_application.hexdigest(),
        "application_behavior_equivalence": "all application inputs are byte-identical to the accepted candidate after the two exact EOF normalizations and three pinned version-identity literal transforms",
        "test_input_count": len(test_paths),
        "test_unchanged_input_count": S1_OWNER_SOURCE_REUSE_TEST_UNCHANGED_INPUT_COUNT,
        "normalized_files": normalized_files,
        "test_fixture_files": test_fixture_files,
        "version_only_files": version_only_files,
        "rebuilt_application": s1_owner_source_reuse_build_evidence(),
        "version_identity_rebuild": s1_owner_source_reuse_version_identity_build_evidence(),
    }



S1_OWNER_SOURCE_REUSE_CERTIFICATE_V4 = {'acceptance_boundary': 'Fresh Release build and headless CLI scenario evidence only; no full gameplay, '
                        'rendering, performance or new owner acceptance. Independent platform, analysis and '
                        'release gates remain required.',
 'application_behavior_equivalence': 'not claimed; semantic preservation requires exact-delta review and '
                                     'current correctness evidence',
 'capture_source_input_sha256': '83da03165eea8b14d48398b8ca314b0ddcb3d74fa6b446b998f819a2538c3e9d',
 'changed_input_count': 141,
 'current_source_input_sha256': 'fd08d7464b1ec7c80d4a494e8e9e471baafcd7f8e85a1220923d453945b93aa1',
 'headless_runtime': {'path': '/Users/core-just/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/fresh-runtime-v4-01/headless-runtime-final-receipt.json',
                      'sha256': '5c0c0dd73dd3002bf5f685da9c52d033768b632268b05db6c68cafd81a16dea2',
                      'size': 1550},
 'manifest': {'path': '/Users/core-just/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/fresh-runtime-v4-01/source-provenance.v4.json',
              'sha256': 'a5665d3ba9e38aca35f815312b05bff800f203195ea6986e9dedb779237b0125',
              'size': 281090},
 'new_binary_owner_acceptance': False,
 'provenance_mode': 'review-supported-analysis-delta',
 'release_rebuild': {'path': '/Users/core-just/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/fresh-runtime-v4-01/release-rebuild-final-receipt.json',
                     'sha256': '91de8a838122f27975160b43b08188cefbc6021a09bddebb44afa3e142dcfb8f',
                     'size': 1561},
 'runtime_evidence_scope': 'historical-owner-accepted-capture',
 'schema': 'mc.s1-analysis-source-provenance.v4',
 'semantic_review': {'path': '/Users/core-just/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/review-artifacts/analysis-source-review-v8.json',
                     'sha256': '55946f17b05153a3b6edf3c04dbb4caefaf8695f354b9d95c5fe6a049e5819c7',
                     'size': 5893},
 'source_input_count': 908}

def s1_owner_source_reuse_certificate(root: Path, current_source_sha256: str) -> dict[str, Any]:
    """Bind historical accepted capture to an exactly reviewed, rebuilt analysis delta."""
    certificate = S1_OWNER_SOURCE_REUSE_CERTIFICATE_V4
    if current_source_sha256 != certificate["current_source_input_sha256"]:
        return s1_owner_source_reuse_certificate_v3(root, current_source_sha256)
    require(current_source_sha256 == certificate["current_source_input_sha256"],
            "current S1 source differs from the exact reviewed analysis candidate")
    manifest_path = checked_file(certificate["manifest"], root)
    manifest = read_json(manifest_path)
    require(manifest.get("schema") == "mc.s1-analysis-source-provenance.v4"
            and manifest.get("qualification_status") == "review-supported-source-reuse",
            "S1 analysis source provenance is not qualified")
    current = _MATRIX._input_file_paths(root, SOURCE_INPUTS)
    accepted = _MATRIX._input_file_paths(S1_OWNER_ACCEPTED_SOURCE_ROOT, SOURCE_INPUTS)
    require(source_digest(S1_OWNER_ACCEPTED_SOURCE_ROOT) == S1_OWNER_SOURCE_INPUT_SHA256,
            "historical capture source inputs changed")
    rows = []
    for relative in sorted(set(current) | set(accepted)):
        rows.append({"path": relative,
                     "accepted_sha256": (hashlib.sha256(accepted[relative].read_bytes()).hexdigest()
                                         if relative in accepted else None),
                     "current_sha256": (hashlib.sha256(current[relative].read_bytes()).hexdigest()
                                        if relative in current else None)})
    require(rows == manifest.get("inputs") and len(current) == certificate["source_input_count"]
            and [row for row in rows if row["accepted_sha256"] != row["current_sha256"]]
            == manifest.get("changed_inputs"),
            "S1 reviewed delta input inventory changed")
    checked_file(certificate["semantic_review"], root)
    rebuild = read_json(checked_file(certificate["release_rebuild"], root))
    require(rebuild.get("status") == "passed"
            and rebuild.get("source_input_sha256") == current_source_sha256
            and rebuild.get("runtime_executed") is False and rebuild.get("new_binary_owner_acceptance") is False,
            "current S1 Release rebuild provenance changed")
    executable = checked_file(rebuild["executable"], root)
    require(build_identity(executable.parent.parent / "source", executable.parent, executable) == rebuild.get("build"),
            "current S1 Release rebuild inputs changed")
    runtime = read_json(checked_file(certificate["headless_runtime"], root))
    require(runtime.get("schema") == "mc.s1-analysis-headless-runtime.v1"
            and runtime.get("passed") is True and runtime.get("returncode") == 0
            and runtime.get("source_input_sha256") == current_source_sha256
            and runtime.get("executable") == rebuild.get("executable"),
            "current S1 headless scenario provenance changed")
    raw = read_json(checked_file(runtime["raw_report"], root))
    require(raw.get("schema_version") == "mc.runtime-evidence.v1"
            and raw.get("mode") == "scenario" and raw.get("passed") is True,
            "current S1 headless scenario schema changed")
    return certificate


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


def verify_fresh_frame_budget(value: Any, frames: list[dict[str, Any]], completions: Any,
                              completion_source: Any, required_for_benchmark_pass: bool,
                              platform: str, *, movement_phase: str = PHASES[2],
                              expected_start_ns: int | None = None) -> bool:
    fields = {"measurement_scope", "target_frame_duration_ns", "required_in_budget_time_permille",
              "measurement_window_start_offset_ns", "measurement_window_end_offset_ns",
              "required_window_duration_ns", "measured_time_ns", "in_budget_time_ns", "sample_count",
              "fence_confirmed_frame_count", "measurement_window_complete", "passed",
              "required_for_benchmark_pass"}
    require(
        isinstance(value, dict) and set(value) == fields,
        "fresh-frame interval budget schema is absent or malformed",
    )
    require(value["measurement_scope"] == FRAME_BUDGET_SCOPE and completion_source == FRAME_COMPLETION_SOURCE,
            "frame budget is not based on per-slot submitted-frame fence completion observations")
    require(isinstance(completions, list), "raw fence-confirmed frame completion evidence is absent")
    target_ns = integer(value["target_frame_duration_ns"], "target_frame_duration_ns", 1)
    required_permille = integer(value["required_in_budget_time_permille"], "required_in_budget_time_permille")
    require(target_ns == FRAME_BUDGET_TARGET_NS[platform]
            and required_permille == FRAME_BUDGET_REQUIRED_PERMILLE,
            f"fresh-frame interval budget does not use the required {FRESH_FRAME_TARGET_HZ[platform]}Hz and 99% thresholds")
    start_ns = integer(value["measurement_window_start_offset_ns"], "measurement_window_start_offset_ns")
    end_ns = integer(value["measurement_window_end_offset_ns"], "measurement_window_end_offset_ns")
    required_duration_ns = integer(value["required_window_duration_ns"], "required_window_duration_ns", 1)
    require(required_duration_ns == PERFORMANCE_SAMPLE_MS * 1_000_000,
            "fresh-frame interval budget requires the fixed 30-second movement window")
    require(end_ns == start_ns + required_duration_ns,
            "fresh-frame measurement window must remain exactly 30 seconds")
    measured_ns = required_duration_ns
    movement_frames = [frame for frame in frames if frame["phase"] == movement_phase]
    require(movement_frames, "fresh-frame interval budget lacks movement samples")
    if expected_start_ns is None:
        baseline_frames = [frame for frame in frames if frame["phase"] == PHASES[1]]
        require(baseline_frames, "fresh-frame interval budget lacks a movement boundary")
        expected_start_ns = (integer(baseline_frames[-1]["started_offset_ns"], "baseline frame start")
                             + integer(baseline_frames[-1]["loop_duration_ns"], "baseline frame duration", 1))
        require(expected_start_ns <= start_ns <= expected_start_ns + SERVER_TICK_NS,
                "fresh-frame measurement window does not begin within one input tick of the movement boundary")
    else:
        require(start_ns == expected_start_ns,
                "fresh-frame measurement window disagrees with the S1 first-send boundary")
    required_end_ns = start_ns + required_duration_ns
    completion_tail_end_ns = required_end_ns + SERVER_TICK_NS
    complete_flag = value["measurement_window_complete"]
    require(type(complete_flag) is bool, "fresh-frame measurement completeness must be boolean")

    submitted_by_sequence: dict[int, dict[str, Any]] = {}
    previous_ordinal = 0
    previous_sequence = 0
    present_ids_by_generation: dict[int, set[int]] = {}
    for frame in movement_frames:
        if not frame["presentation_succeeded"]:
            continue
        submitted_offset = integer(frame.get("submitted_frame_offset_ns"), "movement submitted-frame offset")
        phase = frame.get("submitted_frame_phase")
        if submitted_offset < start_ns or submitted_offset >= required_end_ns:
            continue
        generation = integer(frame.get("swapchain_generation"), "fresh-frame swapchain generation")
        frame_slot = integer(frame.get("submitted_frame_slot"), "fresh-frame slot")
        sequence_id = integer(frame.get("submitted_frame_sequence_id"), "fresh-frame sequence ID", 1)
        ordinal = integer(frame["loop_ordinal"], "fresh-frame loop ordinal", 1)
        present_id_value = frame.get("application_present_id")
        present_id = (
            None
            if present_id_value is None
            else integer(present_id_value, "fresh-frame application present ID", 1)
        )
        require(frame_slot < 3 and phase == movement_phase
                and submitted_offset == integer(frame["started_offset_ns"], "movement frame start")
                    + integer(frame["loop_duration_ns"], "movement frame duration", 1),
                "raw movement submission identity or timestamp is malformed")
        require(ordinal > previous_ordinal and sequence_id > previous_sequence
                and sequence_id not in submitted_by_sequence,
                "fresh-frame submission IDs are duplicated or out of order")
        if present_id is not None:
            generation_ids = present_ids_by_generation.setdefault(generation, set())
            require(present_id not in generation_ids, "fresh-frame application present IDs are duplicated")
            generation_ids.add(present_id)
        submitted_by_sequence[sequence_id] = frame
        previous_ordinal = ordinal
        previous_sequence = sequence_id

    matched: list[tuple[int, int, int]] = []
    seen_completion_ids: set[int] = set()
    previous_completion_ns = start_ns
    previous_completion_sequence = 0
    for completion in completions:
        require(isinstance(completion, dict) and set(completion) == {
            "sequence_id", "swapchain_generation", "frame_slot", "submitted_offset_ns",
            "completion_observed_offset_ns", "application_present_id", "submission_phase"},
            "raw fence-confirmed frame completion is malformed")
        if completion.get("submission_phase") != movement_phase:
            continue
        submitted_offset = integer(completion.get("submitted_offset_ns"), "fence submitted offset")
        if submitted_offset < start_ns or submitted_offset >= required_end_ns:
            continue
        sequence_id = integer(completion.get("sequence_id"), "fence sequence ID", 1)
        generation = integer(completion.get("swapchain_generation"), "fence swapchain generation")
        frame_slot = integer(completion.get("frame_slot"), "fence frame slot")
        observed_offset = integer(completion.get("completion_observed_offset_ns"), "fence observation offset")
        present_id_value = completion.get("application_present_id")
        present_id = None if present_id_value is None else integer(present_id_value, "fence application present ID", 1)
        source = submitted_by_sequence.get(sequence_id)
        require(source is not None and sequence_id not in seen_completion_ids and frame_slot < 3
                and observed_offset >= submitted_offset and observed_offset <= completion_tail_end_ns
                and observed_offset >= previous_completion_ns
                and sequence_id > previous_completion_sequence,
                "fence-confirmed frame completion is missing, duplicated, out of order, or beyond the bounded drain tail")
        require(integer(source["swapchain_generation"], "submission swapchain generation") == generation
                and integer(source["submitted_frame_slot"], "submission frame slot") == frame_slot
                and integer(source["submitted_frame_offset_ns"], "submission offset") == submitted_offset
                and source.get("application_present_id") == present_id
                and source.get("submitted_frame_phase") == completion.get("submission_phase"),
                "fence-confirmed frame completion does not match its submission identity")
        matched.append((sequence_id, observed_offset, submitted_offset))
        seen_completion_ids.add(sequence_id)
        previous_completion_ns = min(observed_offset, required_end_ns)
        previous_completion_sequence = sequence_id

    complete = (complete_flag and end_ns >= required_end_ns
                and len(matched) == len(submitted_by_sequence) and bool(matched))
    require(end_ns == required_end_ns,
            "fresh-frame measurement window must not include the fence-completion drain tail")
    if required_for_benchmark_pass:
        require(complete, "performance proof lacks the complete 30-second fence-confirmed frame window")

    intervals: list[int] = []
    previous_completion_ns = start_ns
    for _, observed_offset, _ in matched:
        clipped_observed = min(observed_offset, required_end_ns)
        # Retired fences observed in one poll share a timestamp; each remains a
        # sample, while its zero interval contributes no in-budget time.
        intervals.append(clipped_observed - previous_completion_ns)
        previous_completion_ns = clipped_observed
    if end_ns > previous_completion_ns:
        intervals.append(end_ns - previous_completion_ns)
    in_budget_ns = sum(interval for interval in intervals if interval <= target_ns)
    over_budget_ns = measured_ns - in_budget_ns
    allowed_over_budget_ns = measured_ns * (1_000 - required_permille) // 1_000
    passed = complete and over_budget_ns <= allowed_over_budget_ns
    expected = {"measurement_scope": FRAME_BUDGET_SCOPE,
                "target_frame_duration_ns": target_ns,
                "required_in_budget_time_permille": required_permille,
                "measurement_window_start_offset_ns": start_ns,
                "measurement_window_end_offset_ns": end_ns,
                "required_window_duration_ns": required_duration_ns,
                "measured_time_ns": measured_ns,
                "in_budget_time_ns": in_budget_ns,
                "sample_count": len(intervals),
                "fence_confirmed_frame_count": len(matched),
                "measurement_window_complete": complete,
                "passed": passed,
                "required_for_benchmark_pass": required_for_benchmark_pass}
    require(value == expected, "raw fresh-frame interval budget mismatch")
    return passed


def verify_s1_visual_lod_sample(value: Any, context: str, sphere: dict[str, Any] | None = None) -> None:
    fields = {"contract", "radius_blocks", "scene_active", "leaf_count", "actual_column_count",
              "terrain_identity", "session_join_epoch", "coverage_epoch", "submitted_frame_count",
              "scene_center", "actual_radius_blocks", "required_actual_columns",
              "presented_current_actual_columns", "coverage_center", "sources_current"}
    require(isinstance(value, dict) and set(value) == fields,
            f"{context}: visual LOD scene sample is absent or malformed")
    require(integer(value["radius_blocks"], f"{context} visual LOD radius") == VISUAL_LOD_RADIUS_BLOCKS,
            f"{context}: visual LOD sample does not cover the configured 320-block radius")
    require(type(value["scene_active"]) is bool,
            f"{context}: visual LOD scene-active state must be boolean")
    leaf_count = integer(value["leaf_count"], f"{context} visual LOD leaf count")
    integer(value["actual_column_count"], f"{context} visual LOD actual-column count")
    require(value["scene_active"] is True and leaf_count >= value["actual_column_count"] >= 81 * 81,
            f"{context}: visual LOD scene must be active and nonempty")
    require(value["contract"] == "mc.presented-terrain-coverage.v1"
            and integer(value["actual_radius_blocks"], f"{context} actual radius") == ACTUAL_TERRAIN_RADIUS_BLOCKS
            and integer(value["required_actual_columns"], f"{context} required actual columns") == 81 * 81
            and integer(value["presented_current_actual_columns"], f"{context} presented actual columns") == 81 * 81
            and value["actual_column_count"] >= 81 * 81
            and value["sources_current"] is True,
            f"{context}: presented visual scene has incomplete or stale actual terrain coverage")
    integer(value["session_join_epoch"], f"{context} scene session", 1)
    integer(value["coverage_epoch"], f"{context} scene coverage epoch", 1)
    integer(value["submitted_frame_count"], f"{context} scene submission count", 1)
    identity = value["terrain_identity"]
    require(isinstance(identity, dict) and set(identity) == {
        "terrain_version", "revision", "seed", "configuration_identity"}
        and isinstance(identity["configuration_identity"], str) and bool(identity["configuration_identity"]),
        f"{context}: presented scene terrain identity is absent or malformed")
    for field in ("terrain_version", "revision", "seed"):
        integer(identity[field], f"{context} scene {field}")
    center = tile_center(value["scene_center"], f"{context} scene center")
    require(all(0 <= coordinate < 65_536 for coordinate in center.values()),
            f"{context}: presented scene center is outside the world")
    if sphere is not None:
        require(value["coverage_center"] == sphere["world_loading_center"]
                and value["coverage_epoch"] <= sphere["active_coverage_epoch"],
                f"{context}: presented scene coverage is sampled for another center or future epoch")


def verify_s1_full_volume_sample(value: dict[str, Any], context: str, *, require_ready: bool = True) -> None:
    required = integer(value.get("required_authoritative_columns"), f"{context} required authoritative columns", 1)
    resident = integer(value.get("resident_authoritative_columns"), f"{context} resident authoritative columns")
    vertical = integer(value.get("vertical_chunk_count"), f"{context} vertical chunk count", 1)
    require(value.get("coverage_scope") == "horizontal_full_volume"
            and required == 81 * 81 and vertical == 64 and resident <= required
            and value["required_chunks"] == required * vertical
            and value["ready_chunks"] == resident * vertical,
            f"{context}: horizontal full-volume authoritative column evidence is inconsistent")
    integer(value["active_coverage_epoch"], f"{context} profile coverage epoch", 1)
    if require_ready:
        require(resident == required and value["ready"] is True,
                f"{context}: horizontal full-volume authoritative-column deficit")
        active, current = value["authoritative_columns_center"], value["world_loading_center"]
        require(isinstance(active, dict) and isinstance(current, dict)
                and all(type(active.get(axis)) is int and type(current.get(axis)) is int
                        and (active[axis] // 16) % 4096 == (current[axis] // 16) % 4096 for axis in ("x", "y")),
                f"{context}: full-volume profile coverage does not identify the current horizontal center")


def verify_movement_playable_sphere(
    full: dict[str, Any], frames: list[dict[str, Any]], budget: dict[str, Any], sample_scope: str,
    *, movement_phase: str = PHASES[2], boundary_phase: str = PHASES[1],
    require_boundary: bool = True, require_source_contract: bool = True,
    expected_radius_blocks: int = PLAYABLE_SPHERE_RADIUS_BLOCKS,
) -> None:
    is_s1_trajectory = full.get("contract") == S1_PERFORMANCE_CONTRACT
    if require_source_contract:
        require(full.get("movement_playable_sphere_contract") == PLAYABLE_SPHERE_CONTRACT
                and full.get("movement_playable_sphere_sample_scope") == sample_scope,
                "movement playable-sphere source contract is absent or unsupported")

    def position(raw: Any, context: str, label: str) -> tuple[int, int, int, int, int, int]:
        position_fields = {"x", "y", "z", "x_subcell", "y_subcell", "z_subcell"}
        require(isinstance(raw, dict) and set(raw) == position_fields,
                f"{context}: {label} is absent or malformed")
        cells = ("x", "y", "z")
        signed_cells = []
        for key in cells:
            coordinate = raw[key]
            require(type(coordinate) is int and -(1 << 31) <= coordinate < (1 << 31),
                    f"{context} {label} {key} must be a signed 32-bit world cell")
            signed_cells.append(coordinate)
        subcells = []
        for key in ("x_subcell", "y_subcell", "z_subcell"):
            subcell = integer(raw[key], f"{context} {label} {key}")
            require(subcell < 10_000, f"{context} {label} {key} is outside its world cell")
            subcells.append(subcell)
        return tuple(signed_cells + subcells)

    def validate_sphere(value: Any, context: str, authoritative_position: Any = None) -> None:
        fields = {"radius_blocks", "active_coverage_epoch", "active_coverage_center",
                  "world_loading_center", "ready_chunks", "required_chunks", "ready"}
        if is_s1_trajectory:
            fields |= S1_FULL_VOLUME_FIELDS
        require(isinstance(value, dict) and set(value) == fields,
                f"{context}: movement playable-sphere sample is absent or malformed")
        require(integer(value["radius_blocks"], f"{context} radius") == expected_radius_blocks,
                f"{context}: movement actual-terrain radius is not {expected_radius_blocks} blocks")
        integer(value["active_coverage_epoch"], f"{context} active coverage epoch", 1)
        required = integer(value["required_chunks"], f"{context} required chunks", 1)
        ready = integer(value["ready_chunks"], f"{context} ready chunks")
        require(type(value["ready"]) is bool and value["ready"] is True and ready == required,
                f"{context}: movement playable-sphere has a materialized-chunk deficit")

        if is_s1_trajectory:
            verify_s1_full_volume_sample(value, context)
        position(value["active_coverage_center"], context, "active coverage center")
        loading_center = position(value["world_loading_center"], context, "WorldLoading center")
        if authoritative_position is not None:
            require(position(authoritative_position, context, "authoritative player position") == loading_center,
                    f"{context}: sphere center does not match the authoritative player update")

    baseline_frames = [frame for frame in frames if frame.get("phase") == boundary_phase]
    start_ns = integer(budget.get("measurement_window_start_offset_ns"), "movement sphere window start")
    duration_ns = integer(budget.get("required_window_duration_ns"), "movement sphere window duration", 1)
    end_ns = start_ns + duration_ns
    movement_frames = [frame for frame in frames if frame.get("phase") == movement_phase
                       and integer(frame.get("started_offset_ns"), "movement sphere frame start") < end_ns]
    require(movement_frames, "movement playable-sphere proof lacks loop samples")
    if require_boundary:
        require(baseline_frames, "movement playable-sphere proof lacks boundary samples")
        last_baseline = baseline_frames[-1]
        baseline_end = integer(last_baseline["started_offset_ns"], "movement boundary frame start") \
            + integer(last_baseline["loop_duration_ns"], "movement boundary frame duration", 1)
        require(baseline_end <= start_ns <= baseline_end + SERVER_TICK_NS,
                "movement playable-sphere proof lacks a sampled state within one input tick of the movement boundary")
        validate_sphere(last_baseline.get("playable_sphere"), "movement boundary")
        previous_ordinal = integer(last_baseline.get("loop_ordinal"), "movement boundary loop ordinal", 1)
    else:
        first_movement = movement_frames[0]
        first_start = integer(first_movement["started_offset_ns"], "first movement loop start")
        first_end = first_start + integer(first_movement["loop_duration_ns"], "first movement loop duration", 1)
        require(first_start <= start_ns < first_end,
                "movement playable-sphere proof does not sample the first-send boundary")
        previous_ordinal = integer(first_movement.get("loop_ordinal"), "first movement loop ordinal", 1) - 1

    last_movement_end = start_ns
    movement_centers: list[tuple[int, int, int, int, int, int]] = []
    for frame in movement_frames:
        ordinal = integer(frame.get("loop_ordinal"), "movement sphere loop ordinal", 1)
        require(ordinal == previous_ordinal + 1, "movement playable-sphere loop samples have a missing interval")
        previous_ordinal = ordinal
        frame_start = integer(frame["started_offset_ns"], "movement sphere frame start")
        frame_end = frame_start + integer(frame["loop_duration_ns"], "movement sphere frame duration", 1)
        require(frame_start < end_ns and frame_start <= last_movement_end,
                "movement playable-sphere loop evidence has a missing interval")
        validate_sphere(frame.get("playable_sphere"), f"movement loop {ordinal}")
        if is_s1_trajectory:
            verify_s1_visual_lod_sample(frame.get("visual_lod"), f"movement loop {ordinal}", frame["playable_sphere"])
        movement_centers.append(position(frame["playable_sphere"]["world_loading_center"],
                                         f"movement loop {ordinal}", "WorldLoading center"))
        last_movement_end = frame_end
    require(last_movement_end >= end_ns,
            "movement playable-sphere samples do not cover the complete window tail")
    require(len(set(movement_centers)) > 1,
            "movement playable-sphere samples do not show ordinary player movement")

    updates = full.get("movement_authoritative_sphere_updates")
    require(isinstance(updates, list), "movement playable-sphere authoritative updates are absent")
    declared_count = integer(full.get("movement_authoritative_update_count"),
                             "movement authoritative sphere update count", 1)
    require(len(updates) == declared_count,
            "movement playable-sphere authoritative update count does not match its raw samples")
    previous_update_ordinal = 0
    previous_update_offset = start_ns - 1
    authoritative_centers: list[tuple[int, int, int, int, int, int]] = []
    for update in updates:
        require(isinstance(update, dict) and {
            "update_ordinal", "offset_ns", "authoritative_position", "playable_sphere",
            "state_revision", "acknowledged_input_sequence", "movement_capabilities",
            "vertical_velocity_subcells",
        } <= set(update),
            "movement playable-sphere authoritative update is malformed")
        ordinal = integer(update["update_ordinal"], "movement authoritative update ordinal", 1)
        offset = integer(update["offset_ns"], "movement authoritative update offset")
        require((previous_update_ordinal == 0 or ordinal > previous_update_ordinal)
                and start_ns <= offset < end_ns + SERVER_TICK_NS
                and offset > previous_update_offset,
                "movement playable-sphere authoritative update sequence is out of order or outside the window")
        validate_sphere(update["playable_sphere"], f"authoritative update {ordinal}",
                        update["authoritative_position"])
        if is_s1_trajectory:
            verify_s1_visual_lod_sample(update.get("visual_lod"), f"authoritative update {ordinal}", update["playable_sphere"])
        authoritative_centers.append(position(update["authoritative_position"],
                                              f"authoritative update {ordinal}",
                                              "authoritative player position"))
        previous_update_ordinal = ordinal
        previous_update_offset = offset
    require(len(set(authoritative_centers)) > 1,
            "movement playable-sphere authoritative updates do not show ordinary player movement")
    require(full.get("movement_playable_sphere_complete") is True,
            "movement playable-sphere evidence is not marked complete")


def verify_x30_authoritative_trajectory(
    game: dict[str, Any], full: dict[str, Any], frames: list[dict[str, Any]], budget: dict[str, Any],
    *, movement_phase: str = PHASES[2],
) -> None:
    require(game.get("movement_trajectory_contract") == MOVEMENT_TRAJECTORY_CONTRACT,
            "x30 authoritative movement trajectory contract is absent or unsupported")
    start = integer(budget.get("measurement_window_start_offset_ns"), "trajectory window start")
    duration = integer(budget.get("required_window_duration_ns"), "trajectory window duration", 1)
    require(duration == PERFORMANCE_SAMPLE_MS * 1_000_000,
            "x30 authoritative movement trajectory requires the fixed 30-second window")
    require(duration % SERVER_TICK_NS == 0,
            "x30 authoritative movement window is not an integral number of server ticks")
    expected_steps = duration // SERVER_TICK_NS
    end = start + duration

    position_fields = {"x", "y", "z", "x_subcell", "y_subcell", "z_subcell"}

    def world_position(value: Any, context: str) -> tuple[int, int, int]:
        require(isinstance(value, dict) and set(value) == position_fields,
                f"{context}: authoritative trajectory position is malformed")
        cells = []
        for axis in ("x", "y", "z"):
            coordinate = value[axis]
            require(type(coordinate) is int and -(1 << 31) <= coordinate < (1 << 31),
                    f"{context}: {axis} cell must be signed 32-bit")
            cells.append(coordinate)
        subcells = []
        for axis in ("x_subcell", "y_subcell", "z_subcell"):
            subcell = integer(value[axis], f"{context} {axis}")
            require(subcell < SUBCELLS_PER_CELL, f"{context}: {axis} is outside its world cell")
            subcells.append(subcell)
        return tuple(cell * SUBCELLS_PER_CELL + subcell for cell, subcell in zip(cells, subcells))

    def signed_component(value: Any, field: str) -> int:
        require(type(value) is int and -127 <= value <= 127, f"{field} must be a signed int8 movement component")
        return value

    def direction_profile_valid(direction: dict[str, Any]) -> bool:
        return (type(direction.get("accelerated")) is bool
                and integer(direction.get("speedup"), "trajectory boundary speed profile")
                    in FLIGHT_SPEEDUP_PROFILES)

    anchor = full.get("movement_trajectory_anchor")
    is_s1_trajectory = full.get("contract") == S1_PERFORMANCE_CONTRACT
    expected_anchor_fields = {
        "update_ordinal", "offset_ns", "state_revision", "acknowledged_input_sequence",
        "authoritative_position", "movement_capabilities", "vertical_velocity_subcells", "playable_sphere",
    }
    if is_s1_trajectory:
        expected_anchor_fields.add("visual_lod")
    require(isinstance(anchor, dict) and set(anchor) == expected_anchor_fields,
            "x30 authoritative movement trajectory anchor is absent or malformed")
    previous_ordinal = integer(anchor["update_ordinal"], "trajectory anchor update ordinal", 1)
    previous_offset = anchor["offset_ns"]
    require(type(previous_offset) is int and -(1 << 63) <= previous_offset < (1 << 63),
            "trajectory anchor offset must be signed int64")
    require(start - SERVER_TICK_NS <= previous_offset < start,
            "x30 authoritative movement trajectory anchor is missing the state at the fixed window boundary")
    previous_revision = integer(anchor["state_revision"], "trajectory anchor state revision")
    previous_ack = integer(anchor["acknowledged_input_sequence"], "trajectory anchor acknowledgement")
    require(previous_ack < (1 << 32) and previous_revision < (1 << 32),
            "x30 authoritative movement trajectory anchor exceeds uint32")
    previous_position = world_position(anchor["authoritative_position"], "trajectory anchor")
    anchor_height = previous_position[2]
    require(0 <= anchor_height < (FLIGHT_MAX_Z + 1) * SUBCELLS_PER_CELL,
            "x30 trajectory anchor is outside the production Flight height bounds")
    require(integer(anchor["movement_capabilities"], "trajectory anchor capabilities") == 3
            and integer(anchor["vertical_velocity_subcells"], "trajectory anchor vertical velocity") == 0,
            "x30 trajectory anchor does not have unobstructed Flight capabilities")
    anchor_sphere = anchor["playable_sphere"]
    sphere_fields = {"radius_blocks", "active_coverage_epoch", "active_coverage_center",
                     "world_loading_center", "ready_chunks", "required_chunks", "ready"}
    if is_s1_trajectory:
        sphere_fields |= S1_FULL_VOLUME_FIELDS
    require(isinstance(anchor_sphere, dict) and set(anchor_sphere) == sphere_fields,
            "x30 trajectory anchor lacks a complete playable-sphere sample")
    anchor_ready = integer(anchor_sphere["ready_chunks"], "trajectory anchor ready chunks")
    anchor_required = integer(anchor_sphere["required_chunks"], "trajectory anchor required chunks", 1)
    integer(anchor_sphere["active_coverage_epoch"], "trajectory anchor coverage epoch", 1)
    world_position(anchor_sphere["active_coverage_center"], "trajectory anchor active coverage center")
    expected_sphere_radius = ACTUAL_TERRAIN_RADIUS_BLOCKS if is_s1_trajectory else PLAYABLE_SPHERE_RADIUS_BLOCKS
    require(integer(anchor_sphere["radius_blocks"], "trajectory anchor sphere radius")
            == expected_sphere_radius and type(anchor_sphere["ready"]) is bool
            and anchor_sphere["ready"] is True and anchor_ready == anchor_required
            and world_position(anchor_sphere["world_loading_center"], "trajectory anchor sphere")
            == previous_position,
            "x30 trajectory anchor lacks a complete sphere at its authoritative position")
    if is_s1_trajectory:
        verify_s1_full_volume_sample(anchor_sphere, "trajectory anchor")
        verify_s1_visual_lod_sample(anchor.get("visual_lod"), "trajectory anchor", anchor_sphere)
    boundary_updates: list[dict[str, Any]] = []
    if is_s1_trajectory:
        anchor_sample_offset = integer(
            full.get("movement_trajectory_anchor_sphere_sample_offset_ns"),
            "trajectory anchor sphere sample offset",
        )
        require(previous_offset <= anchor_sample_offset <= start + SERVER_TICK_NS,
                "x30 trajectory anchor sphere was not refreshed at its exact boundary state")
        require(full.get("movement_boundary_authoritative_updates_complete") is True,
                "x30 trajectory boundary authority capture is incomplete")
        value = full.get("movement_boundary_authoritative_updates")
        require(isinstance(value, list), "x30 trajectory boundary authority updates are absent")
        boundary_updates = value

    all_raw_inputs = game.get("raw_gameplay_inputs")
    require(isinstance(all_raw_inputs, list) and all_raw_inputs,
            "x30 authoritative movement trajectory has no raw sent-input evidence")
    raw_inputs: list[dict[str, Any]] = []
    input_by_sequence: dict[int, dict[str, Any]] = {}
    all_input_by_sequence: dict[int, dict[str, Any]] = {}
    previous_input_ordinal = 0
    previous_input_offset = -1
    previous_input_sequence = 0
    previous_route_sequence = 0
    for raw_input in all_raw_inputs:
        require(isinstance(raw_input, dict) and set(raw_input) == {
            "input_ordinal", "sequence", "sent_offset_ns", "scheduled_offset_ns",
            "movement_intent", "camera_look", "direction",
        }, "x30 authoritative movement sent-input record is malformed")
        input_ordinal = integer(raw_input["input_ordinal"], "trajectory input ordinal", 1)
        sequence = integer(raw_input["sequence"], "trajectory input sequence", 1)
        sent_offset = integer(raw_input["sent_offset_ns"], "trajectory input send offset")
        scheduled_offset = raw_input["scheduled_offset_ns"]
        require(type(scheduled_offset) is int and -(1 << 63) <= scheduled_offset < (1 << 63),
                "trajectory scheduled input offset must be signed int64")
        require(input_ordinal == previous_input_ordinal + 1 and sent_offset >= previous_input_offset
                and scheduled_offset <= sent_offset < scheduled_offset + SERVER_TICK_NS
                and previous_input_sequence < sequence < (1 << 32),
                "x30 authoritative movement sent-input sequence is incomplete or duplicated")
        intent = raw_input["movement_intent"]
        require(isinstance(intent, dict) and set(intent) == {"strafe", "forward", "vertical"},
                "x30 authoritative movement keyboard intent is malformed")
        for axis, component in intent.items():
            require(type(component) is int and -1 <= component <= 1,
                    f"trajectory keyboard {axis} intent must be -1, 0, or 1")
        camera = raw_input["camera_look"]
        require(isinstance(camera, dict) and set(camera) == {"yaw_degrees", "pitch_degrees"}
                and type(camera["yaw_degrees"]) in (float, int)
                and type(camera["pitch_degrees"]) in (float, int)
                and math.isfinite(camera["yaw_degrees"]) and math.isfinite(camera["pitch_degrees"])
                and -89.0 <= camera["pitch_degrees"] <= 89.0,
                "x30 authoritative movement camera look is missing or outside the production camera range")
        direction = raw_input["direction"]
        direction_fields = {"x", "y", "z", "view_x", "view_y", "accelerated", "speedup",
                            "cycle_movement_capabilities"}
        require(isinstance(direction, dict) and set(direction) == direction_fields,
                "x30 authoritative movement direction is malformed")
        for axis in ("x", "y", "z", "view_x", "view_y"):
            signed_component(direction[axis], f"trajectory direction {axis}")
        require(type(direction["accelerated"]) is bool
                and type(direction["cycle_movement_capabilities"]) is bool,
                "x30 authoritative movement flags must be booleans")
        integer(direction["speedup"], "trajectory speed profile")
        require(direction["speedup"] < (1 << 16), "trajectory speed profile exceeds uint16")
        previous_input_ordinal, previous_input_offset, previous_input_sequence = input_ordinal, sent_offset, sequence
        all_input_by_sequence[sequence] = raw_input
        if start <= sent_offset < end:
            require(previous_route_sequence == 0 or sequence == previous_route_sequence + 1,
                    "x30 authoritative movement sent-input sequence is incomplete or duplicated")
            input_by_sequence[sequence] = raw_input
            raw_inputs.append(raw_input)
            previous_route_sequence = sequence

    require(len(raw_inputs) in (expected_steps, expected_steps + 1),
            f"x30 authoritative movement requires {expected_steps} scheduled movement steps")
    require(raw_inputs[0]["sent_offset_ns"] == start,
            "x30 authoritative movement window does not start at its first successful send")
    scheduled_offsets = [raw_input["scheduled_offset_ns"] for raw_input in raw_inputs]
    require(start - SERVER_TICK_NS <= scheduled_offsets[0] <= start
            and end - 2 * SERVER_TICK_NS <= scheduled_offsets[-1] < end
            and all(current == previous + SERVER_TICK_NS
                    for previous, current in zip(scheduled_offsets, scheduled_offsets[1:])),
            "x30 authoritative movement has a missing or non-contiguous scheduled client tick")

    updates = full.get("movement_authoritative_sphere_updates")
    declared_count = integer(full.get("movement_authoritative_update_count"),
                             "trajectory authoritative update count", 1)
    require(isinstance(updates, list) and len(updates) == declared_count,
            "x30 authoritative movement trajectory update count is incomplete")
    require(len(updates) == len(raw_inputs),
            f"x30 authoritative movement requires {expected_steps} scheduled movement steps")
    acknowledged_inputs: list[dict[str, Any]] = []
    acknowledged_sequences: set[int] = set()
    previous_update_offset = start - 1
    boundary_sequences: set[int] = set()
    for update in boundary_updates:
        expected_boundary_fields = {
            "update_ordinal", "offset_ns", "state_revision", "acknowledged_input_sequence",
            "authoritative_position", "movement_capabilities", "vertical_velocity_subcells",
            "playable_sphere",
        }
        if is_s1_trajectory:
            expected_boundary_fields.add("visual_lod")
        require(isinstance(update, dict) and set(update) == expected_boundary_fields,
                "x30 trajectory boundary authority update is malformed")
        ordinal = integer(update["update_ordinal"], "trajectory boundary update ordinal", 1)
        offset = integer(update["offset_ns"], "trajectory boundary update offset")
        revision = integer(update["state_revision"], "trajectory boundary state revision")
        acknowledged = integer(update["acknowledged_input_sequence"], "trajectory boundary acknowledged input")
        capabilities = integer(update["movement_capabilities"], "trajectory boundary movement capabilities")
        vertical_velocity = update["vertical_velocity_subcells"]
        require(type(vertical_velocity) is int and -(1 << 31) <= vertical_velocity < (1 << 31),
                "trajectory boundary vertical velocity must be signed int32")
        require(ordinal > previous_ordinal and offset >= start and revision > previous_revision
                and acknowledged > previous_ack and revision < (1 << 32) and acknowledged < (1 << 32),
                "x30 trajectory boundary authority updates are missing, stale, or out of order")
        require(capabilities == 3 and vertical_velocity == 0,
                "x30 trajectory boundary update lost unobstructed Flight capabilities")
        raw_input = all_input_by_sequence.get(acknowledged)
        require(raw_input is not None and raw_input["sent_offset_ns"] < start
                and raw_input["sent_offset_ns"] <= offset <= raw_input["sent_offset_ns"] + SERVER_TICK_NS,
                "x30 trajectory boundary update is not linked to a delayed pre-window sent input")
        require(acknowledged not in boundary_sequences
                and acknowledged < raw_inputs[0]["sequence"],
                "x30 trajectory boundary acknowledgement is duplicated or interleaved with measured movement")
        require(direction_profile_valid(raw_input["direction"]),
                "x30 trajectory boundary input has an unsupported movement profile")
        direction = raw_input["direction"]
        intent = raw_input["movement_intent"]
        require(direction["cycle_movement_capabilities"] is False
                and ((intent["strafe"] != 0 or intent["forward"] != 0 or intent["vertical"] != 0)
                     == (direction["x"] != 0 or direction["y"] != 0 or direction["z"] != 0)),
                "x30 trajectory boundary input does not preserve its sent movement intent")
        current_position = world_position(update["authoritative_position"], f"trajectory boundary update {ordinal}")
        squared_length = direction["x"] ** 2 + direction["y"] ** 2 + direction["z"] ** 2
        direction_length = math.isqrt(squared_length)
        if direction_length * direction_length < squared_length:
            direction_length += 1
        divisor = max(127, direction_length)
        speed_factor = direction["speedup"] if direction["accelerated"] else 1
        delta = tuple((MOVEMENT_SUBCELLS_PER_TICK * abs(component) // divisor * speed_factor)
                      * (-1 if component < 0 else 1)
                      for component in (direction["x"], direction["y"], direction["z"]))
        delta_x = (current_position[0] - previous_position[0]) % FLIGHT_HORIZONTAL_EXTENT
        if delta_x > FLIGHT_HORIZONTAL_EXTENT // 2:
            delta_x -= FLIGHT_HORIZONTAL_EXTENT
        delta_y = (current_position[1] - previous_position[1]) % FLIGHT_HORIZONTAL_EXTENT
        if delta_y > FLIGHT_HORIZONTAL_EXTENT // 2:
            delta_y -= FLIGHT_HORIZONTAL_EXTENT
        actual_delta = (delta_x, delta_y, current_position[2] - previous_position[2])
        require(0 <= current_position[2] < (FLIGHT_MAX_Z + 1) * SUBCELLS_PER_CELL
                and actual_delta == delta,
                "x30 trajectory boundary position does not match the production movement step")
        sphere = update["playable_sphere"]
        require(isinstance(sphere, dict) and set(sphere) == sphere_fields
                and integer(sphere["radius_blocks"], "trajectory boundary sphere radius")
                    == expected_sphere_radius
                and type(sphere["ready"]) is bool and sphere["ready"] is True
                and integer(sphere["ready_chunks"], "trajectory boundary ready chunks")
                    == integer(sphere["required_chunks"], "trajectory boundary required chunks", 1)
                and world_position(sphere["world_loading_center"], "trajectory boundary sphere center")
                    == current_position,
                "x30 trajectory boundary update lacks a complete sphere at its authoritative position")
        integer(sphere["active_coverage_epoch"], "trajectory boundary coverage epoch", 1)
        world_position(sphere["active_coverage_center"], "trajectory boundary active coverage center")
        if is_s1_trajectory:
            verify_s1_full_volume_sample(sphere, f"trajectory boundary update {ordinal}")
            verify_s1_visual_lod_sample(update.get("visual_lod"), f"trajectory boundary update {ordinal}", sphere)
        boundary_sequences.add(acknowledged)
        previous_position, previous_revision, previous_ack = current_position, revision, acknowledged
        previous_ordinal, previous_update_offset = ordinal, offset

    if boundary_updates:
        first_route_ack_offset = integer(updates[0].get("offset_ns"), "first route acknowledgement offset")
        require(previous_update_offset < first_route_ack_offset,
                "x30 trajectory boundary acknowledgement is interleaved with measured movement")
    for update in updates:
        ordinal = integer(update.get("update_ordinal"), "trajectory authority update ordinal", 1)
        offset = integer(update.get("offset_ns"), "trajectory authority update offset")
        revision = integer(update.get("state_revision"), "trajectory authority state revision")
        acknowledged = integer(update.get("acknowledged_input_sequence"), "trajectory acknowledged input")
        capabilities = integer(update.get("movement_capabilities"), "trajectory movement capabilities")
        vertical_velocity = update.get("vertical_velocity_subcells")
        require(type(vertical_velocity) is int and -(1 << 31) <= vertical_velocity < (1 << 31),
                "trajectory vertical velocity must be signed int32")
        require(ordinal > previous_ordinal and offset > previous_update_offset
                and start <= offset < end + SERVER_TICK_NS
                and revision > previous_revision and acknowledged > previous_ack
                and revision < (1 << 32) and acknowledged < (1 << 32),
                "x30 authoritative movement trajectory has a missing, stale, or out-of-window update")
        require(capabilities == 3 and vertical_velocity == 0,
                "x30 authoritative movement update lost unobstructed Flight capabilities")
        raw_input = input_by_sequence.get(acknowledged)
        require(raw_input is not None and raw_input["sent_offset_ns"] <= offset,
                "x30 authoritative movement update is not linked to a sent input within the measurement window")
        require(offset <= raw_input["sent_offset_ns"] + SERVER_TICK_NS,
                "x30 authoritative movement acknowledgement arrived more than one server tick after its sent input")
        acknowledged_sequences.add(acknowledged)
        intent = raw_input["movement_intent"]
        require(intent["strafe"] != 0 or intent["forward"] != 0 or intent["vertical"] != 0,
                "x30 authoritative movement contains a stationary keyboard input")
        direction = raw_input["direction"]
        require(direction["accelerated"] is True and direction["speedup"] == 30
                and direction["cycle_movement_capabilities"] is False
                and direction["z"] == intent["vertical"] * 127,
                "x30 authoritative movement ack did not consume an uncoupled speed-30 direction")

        yaw = math.fmod(float(raw_input["camera_look"]["yaw_degrees"]), 360.0)
        if yaw < 0.0:
            yaw += 360.0
        radians = yaw * 0.01745329251994329576923690768489
        sine, cosine = math.sin(radians), math.cos(radians)
        strafe, forward = intent["strafe"], intent["forward"]
        world_x = strafe * cosine + forward * sine
        world_y = -strafe * sine + forward * cosine
        length = math.hypot(world_x, world_y)
        expected_xy = ((int(world_x / length * 127.0), int(world_y / length * 127.0))
                       if length > 0.0 else (0, 0))
        pitch_radians = float(raw_input["camera_look"]["pitch_degrees"]) * 0.01745329251994329576923690768489
        expected_view = (int(math.sin(radians) * math.cos(pitch_radians) * 127.0),
                         int(math.cos(radians) * math.cos(pitch_radians) * 127.0))
        require(all(abs(actual - expected) <= 1 for actual, expected in zip(
            (direction["x"], direction["y"]), expected_xy))
            and all(abs(actual - expected) <= 1 for actual, expected in zip(
                (direction["view_x"], direction["view_y"]), expected_view)),
            "x30 authoritative movement direction disagrees with the recorded camera-relative keyboard input")
        direction_magnitude = math.sqrt(direction["x"] ** 2 + direction["y"] ** 2 + direction["z"] ** 2)
        require(direction_magnitude >= 125.0
                and (length == 0.0 or math.hypot(direction["x"], direction["y"]) >= 125.0),
                "x30 authoritative movement direction is not full-scale keyboard input")

        squared_length = direction["x"] ** 2 + direction["y"] ** 2 + direction["z"] ** 2
        direction_length = math.isqrt(squared_length)
        if direction_length * direction_length < squared_length:
            direction_length += 1
        divisor = max(127, direction_length)
        delta = tuple((MOVEMENT_SUBCELLS_PER_TICK * abs(component) // divisor * 30)
                      * (-1 if component < 0 else 1)
                      for component in (direction["x"], direction["y"], direction["z"]))
        current_position = world_position(update.get("authoritative_position"), f"trajectory update {ordinal}")
        delta_x = (current_position[0] - previous_position[0]) % FLIGHT_HORIZONTAL_EXTENT
        if delta_x > FLIGHT_HORIZONTAL_EXTENT // 2:
            delta_x -= FLIGHT_HORIZONTAL_EXTENT
        delta_y = (current_position[1] - previous_position[1]) % FLIGHT_HORIZONTAL_EXTENT
        if delta_y > FLIGHT_HORIZONTAL_EXTENT // 2:
            delta_y -= FLIGHT_HORIZONTAL_EXTENT
        actual_delta = (delta_x, delta_y, current_position[2] - previous_position[2])
        require(0 <= current_position[2] < (FLIGHT_MAX_Z + 1) * SUBCELLS_PER_CELL,
                "x30 authoritative movement position is outside the production Flight height bounds")
        require(actual_delta == delta,
                "x30 authoritative movement position does not match the production full-scale speed-30 step")
        acknowledged_inputs.append(raw_input)
        previous_position, previous_revision, previous_ack = current_position, revision, acknowledged
        previous_ordinal, previous_update_offset = ordinal, offset

    window_input_sequences = {
        sequence for sequence, raw_input in input_by_sequence.items()
        if start <= raw_input["sent_offset_ns"] < end
    }
    require(window_input_sequences == acknowledged_sequences,
            "x30 authoritative movement has a missing or unacknowledged scheduled client tick")

    z_positions = [anchor_height]
    z_positions.extend(world_position(update["authoritative_position"], "trajectory update height")[2]
                       for update in updates)
    saw_ascent = False
    saw_descent_after_ascent = False
    for previous_height, current_height in zip(z_positions, z_positions[1:]):
        saw_ascent = saw_ascent or current_height > previous_height
        saw_descent_after_ascent = saw_descent_after_ascent or (saw_ascent and current_height < previous_height)
    require(any(entry["movement_intent"]["vertical"] > 0 for entry in acknowledged_inputs)
            and any(entry["movement_intent"]["vertical"] < 0 for entry in acknowledged_inputs)
            and saw_ascent and saw_descent_after_ascent,
            "x30 authoritative movement does not prove an ascent followed by a descent")

    require(len({(entry["movement_intent"]["strafe"], entry["movement_intent"]["forward"])
                 for entry in acknowledged_inputs}) >= 2,
            "x30 authoritative movement does not vary its acknowledged keyboard direction")
    require(len({float(entry["camera_look"]["yaw_degrees"]) for entry in acknowledged_inputs}) >= 2
            and len({float(entry["camera_look"]["pitch_degrees"]) for entry in acknowledged_inputs}) >= 2,
            "x30 authoritative movement does not vary its acknowledged camera yaw and pitch")

    observed_camera = []
    rendered_positions = []
    authority_positions = []
    # A client may predict one input tick while presentation interpolates the
    # preceding tick. The camera must still follow that same wrapped player.
    maximum_attachment_lag = 2.0 * MOVEMENT_SUBCELLS_PER_TICK * 30 / SUBCELLS_PER_CELL
    body_center_offset = 0.3125
    eye_height = 1.625
    def wrapped_delta(current: float, previous: float) -> float:
        delta = math.fmod(current - previous, 65_536.0)
        if delta > 32_768.0:
            delta -= 65_536.0
        elif delta < -32_768.0:
            delta += 65_536.0
        return delta

    for frame in frames:
        if frame.get("phase") != movement_phase:
            continue
        if frame.get("presentation_succeeded") is not True:
            continue
        frame_start = integer(frame.get("started_offset_ns"), "trajectory camera frame start")
        frame_end = frame_start + integer(frame.get("loop_duration_ns"), "trajectory camera frame duration", 1)
        if frame_start < end and frame_end > start:
            camera = frame.get("diagnostic_camera")
            require(isinstance(camera, list) and len(camera) == 7
                    and all(type(value) in (float, int) and math.isfinite(value) for value in camera),
                    "x30 authoritative movement lacks rendered camera pose samples")
            sphere = frame.get("playable_sphere")
            require(isinstance(sphere, dict), "rendered camera lacks its current authoritative player")
            authority = world_position(sphere.get("world_loading_center"), "rendered camera authority")
            body = (float(camera[0]) - body_center_offset,
                    float(camera[1]) - body_center_offset, float(camera[2]) - eye_height)
            require(-6.0 <= body[0] <= 65_542.0 and -6.0 <= body[1] <= 65_542.0,
                    "rendered camera is outside the canonical wrapped world")
            expected = tuple(component / SUBCELLS_PER_CELL for component in authority)
            error = (wrapped_delta(body[0], expected[0]), wrapped_delta(body[1], expected[1]),
                     body[2] - expected[2])
            require(math.sqrt(sum(component * component for component in error))
                    <= maximum_attachment_lag + 1e-6,
                    "rendered camera translation does not follow the authoritative x30 player")
            if movement_phase == S1_MOVEMENT_PHASE:
                presented = frame.get("presented_player")
                require(isinstance(presented, dict) and set(presented) == {
                    "body", "submitted_frame_count", "sampled_offset_ns"},
                    "rendered camera has no submitted local-player presentation receipt")
                presented_body = presented["body"]
                require(isinstance(presented_body, list) and len(presented_body) == 3
                        and all(type(value) in (float, int) and math.isfinite(value) for value in presented_body),
                        "rendered camera local-player presentation is malformed")
                sample_offset = integer(presented["sampled_offset_ns"], "rendered camera sample offset")
                require(frame_start <= sample_offset <= frame_end
                        and integer(presented["submitted_frame_count"], "rendered camera submission", 1)
                            == frame["visual_lod"]["submitted_frame_count"],
                        "rendered camera local-player receipt is not bound to the sampled submission")
                body_error = (wrapped_delta(body[0], presented_body[0]),
                              wrapped_delta(body[1], presented_body[1]), body[2] - presented_body[2])
                require(math.sqrt(sum(component * component for component in body_error)) <= 1e-4,
                        "rendered camera is detached from the submitted local-player body/eye")
            rendered_positions.append(body)
            authority_positions.append(expected)
            observed_camera.append((float(camera[3]), float(camera[4])))
    require(len(observed_camera) > 1 and len({yaw for yaw, _ in observed_camera}) > 1
            and len({pitch for _, pitch in observed_camera}) > 1,
            "x30 authoritative movement lacks a varying rendered camera yaw and pitch trajectory")
    def travel(positions: list[tuple[float, float, float]]) -> float:
        return sum(math.sqrt(wrapped_delta(current[0], previous[0]) ** 2
                             + wrapped_delta(current[1], previous[1]) ** 2
                             + (current[2] - previous[2]) ** 2)
                   for previous, current in zip(positions, positions[1:]))
    rendered_travel, authoritative_travel = travel(rendered_positions), travel(authority_positions)
    require(authoritative_travel > 0.0 and rendered_travel > 0.0
            and rendered_travel >= authoritative_travel * 0.9,
            "rendered camera translation is stationary or slower than the authoritative x30 trajectory")


def verify_s1_performance_report(report: dict[str, Any], platform: str, *, proof: str | None,
                                cadence_exception: dict[str, Any] | None) -> dict[str, Any]:
    require(proof in (None, "performance") and cadence_exception is None,
            "S1 performance does not accept legacy proof selectors or cadence exceptions")
    require(report.get("schema_version") == "mc.runtime-evidence.v1"
            and report.get("mode") == "benchmark-game" and report.get("passed") is True
            and report.get("failure") == "", "runtime workload did not pass")
    timing = report.get("timing")
    require(isinstance(timing, dict), "runtime timing evidence is absent")
    elapsed_ms = integer(timing.get("elapsed_ms"), "elapsed_ms", 1)
    require(elapsed_ms <= integer(timing.get("deadline_ms"), "deadline_ms", 1), "runtime timeout")

    game = report.get("game_benchmark")
    require(isinstance(game, dict) and game.get("qualification_proof") == "performance",
            "missing or mismatched gameplay proof selector")
    require(game.get("workload") == WORKLOAD and game.get("build_mode") == "release",
            "requires the S1 ordinary-turning Release workload")
    require(game.get("diagnostic_overrides_present") is False,
            "diagnostic gameplay report is not qualification proof")
    require(isinstance(game.get("present_mode"), str) and bool(game["present_mode"]),
            "negotiated present mode was not recorded")
    framebuffer = game.get("framebuffer")
    manifest = qualification_settings_manifest()
    require(game.get("qualification_settings_manifest") == manifest,
            "S1 qualification settings manifest is missing or differs from the source contract")
    session_epoch = integer(game.get("session_join_epoch"), "S1 current session epoch", 1)
    terrain_identity = game.get("terrain_identity")
    require(terrain_identity == manifest["terrain_identity"],
            "S1 current terrain identity differs from the frozen canonical generation settings")
    require(framebuffer == manifest["platforms"][platform]["framebuffer"],
            "normal gameplay framebuffer differs from the frozen qualification settings")
    require(game["present_mode"] == manifest["platforms"][platform]["present_mode"],
            "negotiated present mode differs from the frozen qualification settings")
    for key in ("server_events_processed", "inputs_sent", "authoritative_player_updates"):
        integer(game.get(key), key, 1)

    s1 = game.get("s1_terrain_performance")
    require(isinstance(s1, dict), "S1 terrain-performance evidence is absent or malformed")
    require("movement_trajectory_anchor" in s1,
            "x30 authoritative movement trajectory anchor is absent")
    required_fields = {
        "contract", "timing_origin", "initial_radius_blocks", "actual_radius_blocks",
        "visual_lod_radius_blocks",
        "ready_prompt_frame_offset_ns", "playable_sphere_first_ready_offset_ns",
        "initial_deadline_missed", "playable_deadline_missed",
        "movement_started_after_current_sphere_ready", "movement_input_during_fill",
        "movement_start_offset_ns", "movement_end_offset_ns",
        "movement_authoritative_update_count", "movement_trajectory_anchor",
        "movement_trajectory_anchor_sphere_sample_offset_ns",
        "movement_boundary_authoritative_updates_complete", "movement_boundary_authoritative_updates",
        "movement_authoritative_sphere_updates", "movement_playable_sphere_complete",
        "movement_visual_lod_samples_complete", "complete",
    }
    require(required_fields <= set(s1),
            "S1 terrain-performance evidence is absent or malformed")
    require(s1["contract"] == S1_PERFORMANCE_CONTRACT
            and s1["timing_origin"] == "world-request-accepted"
            and integer(s1["initial_radius_blocks"], "S1 initial radius") == 48
            and integer(s1["actual_radius_blocks"], "S1 actual terrain radius")
                == ACTUAL_TERRAIN_RADIUS_BLOCKS
            and integer(s1["visual_lod_radius_blocks"], "S1 visual LOD radius")
                == VISUAL_LOD_RADIUS_BLOCKS,
            "S1 LOD-performance contract or radii are unsupported")

    start_action = game.get("controlled_start")
    start_fields = {
        "contract", "input_source", "production_path", "synthetic_input_override_invoked",
        "eligible_prompt_rendered_offset_ns", "synthetic_input_requested_offset_ns",
        "key_accepted_offset_ns", "server_start_acknowledged_offset_ns", "input_ordinal",
        "offset_origin", "complete",
    }
    require(isinstance(start_action, dict) and set(start_action) == start_fields
            and start_action.get("contract") == "mc.controlled-world-start.v1"
            and start_action.get("input_source") == "synthetic-controlled-start-key"
            and start_action.get("production_path")
                == "GameClientBenchmarkHooks.start_input_override->GameClient.acceptLoadingStartInput"
            and start_action.get("synthetic_input_override_invoked") is True
            and start_action.get("offset_origin") == "GameBenchmark.request_accepted_at"
            and start_action.get("complete") is True,
            "S1 prompt-to-production-start evidence is absent or incomplete")
    prompt_offset = integer(s1["ready_prompt_frame_offset_ns"], "S1 ready prompt offset")
    prompt_action_offset = integer(start_action["eligible_prompt_rendered_offset_ns"],
                                   "controlled start prompt offset")
    request_offset = integer(start_action["synthetic_input_requested_offset_ns"],
                              "controlled start input request offset")
    key_offset = integer(start_action["key_accepted_offset_ns"], "controlled start key accepted offset")
    start_ack_offset = integer(start_action["server_start_acknowledged_offset_ns"],
                               "controlled start acknowledgement offset")
    integer(start_action["input_ordinal"], "controlled start input ordinal", 1)
    require(prompt_offset == prompt_action_offset and prompt_offset <= 4_000_000_000
            and prompt_offset <= request_offset <= key_offset <= start_ack_offset,
            "S1 Ready prompt missed 4 seconds or the real start action was not ordered after it")

    first_ready_offset = integer(s1["playable_sphere_first_ready_offset_ns"],
                                 "S1 actual-terrain first-ready offset")
    sphere_deadline_ns = 20_000_000_000 if platform == "macos" else 15_000_000_000
    # Prompt rendering and actual terrain readiness have independent T0 deadlines.
    require(first_ready_offset <= sphere_deadline_ns,
            f"S1 actual 640-block terrain sphere missed the {sphere_deadline_ns // 1_000_000_000}-second deadline")
    require(type(s1["initial_deadline_missed"]) is bool and s1["initial_deadline_missed"] is False
            and type(s1["playable_deadline_missed"]) is bool and s1["playable_deadline_missed"] is False
            and s1["movement_started_after_current_sphere_ready"] is True
            and type(s1["movement_input_during_fill"]) is bool
            and s1["movement_playable_sphere_complete"] is True
            and s1["movement_visual_lod_samples_complete"] is True
            and s1["complete"] is True,
            "S1 loading deadline, movement start, or complete-sphere evidence failed")

    start_t0 = integer(s1["movement_start_offset_ns"], "S1 movement start offset")
    end_t0 = integer(s1["movement_end_offset_ns"], "S1 movement end offset")
    movement_eligible_offset = max(start_ack_offset, first_ready_offset)
    require(movement_eligible_offset <= start_t0
            and start_t0 - movement_eligible_offset <= SERVER_TICK_NS
            and end_t0 == start_t0 + PERFORMANCE_SAMPLE_MS * 1_000_000,
            "S1 measured movement did not start at the first post-Ready input or last exactly 30 seconds")
    first_loop_start = start_t0 - start_ack_offset
    require(first_loop_start >= 0,
            "S1 measured movement begins before the accepted start acknowledgement")

    raw_inputs = game.get("raw_gameplay_inputs")
    require(isinstance(raw_inputs, list) and raw_inputs,
            "S1 performance lacks successful gameplay input evidence")
    require(game["inputs_sent"] == len(raw_inputs),
            "S1 successful gameplay input count does not match raw sent-input evidence")
    fill_inputs = []
    post_ready_inputs = []
    sent_offsets_t0 = []
    for raw_input in raw_inputs:
        require(isinstance(raw_input, dict) and type(raw_input.get("sent_offset_ns")) is int
                and raw_input["sent_offset_ns"] >= 0,
                "S1 gameplay input has a missing or invalid send timestamp")
        sent_t0 = start_ack_offset + raw_input["sent_offset_ns"]
        sent_offsets_t0.append(sent_t0)
        if start_ack_offset <= sent_t0 < first_ready_offset:
            fill_inputs.append(raw_input)
        if sent_t0 >= first_ready_offset:
            post_ready_inputs.append(sent_t0)
    require(s1["movement_input_during_fill"] is bool(fill_inputs),
            "S1 fill-ramp input flag disagrees with successful sent-input evidence")
    if first_ready_offset - start_ack_offset > SERVER_TICK_NS:
        require(fill_inputs and min(start_ack_offset + raw_input["sent_offset_ns"]
                                     for raw_input in fill_inputs)
                <= start_ack_offset + SERVER_TICK_NS,
                "S1 ordinary route omitted its first x30 movement input during the initial fill interval")
    for raw_input in fill_inputs:
        intent = raw_input.get("movement_intent")
        direction = raw_input.get("direction")
        require(isinstance(intent, dict) and any(intent.get(axis) != 0 for axis in ("strafe", "forward", "vertical"))
                and isinstance(direction, dict) and type(direction.get("accelerated")) is bool
                and integer(direction.get("speedup"), "S1 fill-ramp speedup") in FLIGHT_SPEEDUP_PROFILES
                and direction.get("cycle_movement_capabilities") is False,
                "S1 fill-ramp evidence is not a successful movement input with a valid speed profile")
    require(post_ready_inputs and min(post_ready_inputs) == start_t0,
            "S1 movement start is not the first successful x30 input after current actual terrain became Ready")
    require(all(sent_t0 < end_t0 + SERVER_TICK_NS for sent_t0 in sent_offsets_t0),
            "S1 successful gameplay input extends beyond the bounded post-window ACK tail")

    frames = game.get("raw_frames")
    require(isinstance(frames, list) and frames, "S1 raw frame evidence is absent")
    phases: list[str] = []
    previous_ordinal = 0
    previous_end = 0
    movement_frames = []
    for frame in frames:
        require(isinstance(frame, dict), "S1 raw frame is malformed")
        phase = frame.get("phase")
        require(phase in (S1_FILL_PHASE, S1_MOVEMENT_PHASE), "S1 raw frame has an unsupported phase")
        if not phases or phases[-1] != phase:
            phases.append(phase)
        ordinal = integer(frame.get("loop_ordinal"), "S1 frame loop ordinal", 1)
        frame_start = integer(frame.get("started_offset_ns"), "S1 frame start")
        frame_end = frame_start + integer(frame.get("loop_duration_ns"), "S1 frame duration", 1)
        require(ordinal == previous_ordinal + 1 and frame_start >= previous_end
                and type(frame.get("presentation_succeeded")) is bool,
                "S1 raw frame timeline is unordered or missing a loop")
        if phase == S1_MOVEMENT_PHASE:
            settings = frame.get("qualification_settings")
            require(isinstance(settings, dict) and settings == {
                "framebuffer": manifest["platforms"][platform]["framebuffer"],
                "debug_hud_enabled": manifest["debug_hud_enabled"],
                "terrain_model": manifest["terrain_model"],
                "render_distance": manifest["render_distance"],
                "camera_perspective": manifest["camera_perspective"],
                "world_mode": manifest["world_mode"],
            }, "per-frame qualification settings changed or were not observed")
            camera = frame.get("diagnostic_camera")
            require(isinstance(camera, list) and len(camera) == 7
                    and camera[6] == manifest["fov_degrees"],
                    "rendered projection FOV differs from the frozen qualification settings")
            require(not phases or phases == [S1_FILL_PHASE, S1_MOVEMENT_PHASE]
                    or phases == [S1_MOVEMENT_PHASE],
                    "S1 movement frames precede or interrupt the fill-ramp phase")
            movement_frames.append(frame)
        else:
            require(not movement_frames, "S1 fill-ramp phase resumed during measured movement")
        previous_ordinal, previous_end = ordinal, frame_end
    require(movement_frames and phases in ([S1_MOVEMENT_PHASE], [S1_FILL_PHASE, S1_MOVEMENT_PHASE])
            and frames[0]["started_offset_ns"] == 0,
            "S1 raw timeline lacks its first-loop origin or measured movement phase")

    def fill_world_position(value: Any, context: str) -> tuple[int, int, int, int, int, int]:
        position_fields = {"x", "y", "z", "x_subcell", "y_subcell", "z_subcell"}
        require(isinstance(value, dict) and set(value) == position_fields,
                f"{context}: WorldLoading center is absent or malformed")
        cells = []
        for axis in ("x", "y", "z"):
            coordinate = value[axis]
            require(type(coordinate) is int and -(1 << 31) <= coordinate < (1 << 31),
                    f"{context}: WorldLoading center {axis} must be signed int32")
            cells.append(coordinate)
        subcells = []
        for axis in ("x_subcell", "y_subcell", "z_subcell"):
            subcell = integer(value[axis], f"{context} WorldLoading center {axis}")
            require(subcell < SUBCELLS_PER_CELL,
                    f"{context}: WorldLoading center {axis} subcell is outside its world cell")
            subcells.append(subcell)
        return tuple(cells + subcells)

    if fill_inputs:
        fill_positions = []
        for frame in frames:
            if frame["phase"] != S1_FILL_PHASE:
                continue
            frame_end_t0 = start_ack_offset + integer(frame["started_offset_ns"], "S1 fill frame start") \
                + integer(frame["loop_duration_ns"], "S1 fill frame duration", 1)
            if frame_end_t0 >= first_ready_offset:
                continue
            sphere = frame.get("playable_sphere")
            sphere_fields = {"radius_blocks", "active_coverage_epoch", "active_coverage_center",
                             "world_loading_center", "ready_chunks", "required_chunks", "ready"}
            sphere_fields |= S1_FULL_VOLUME_FIELDS
            require(isinstance(sphere, dict) and set(sphere) == sphere_fields,
                    "S1 fill-ramp frame lacks its pre-Ready playable-sphere sample")
            require(integer(sphere["radius_blocks"], "S1 fill playable-sphere radius")
                    == ACTUAL_TERRAIN_RADIUS_BLOCKS,
                    "S1 fill-ramp sample does not describe the current 640-block actual terrain sphere")
            integer(sphere["active_coverage_epoch"], "S1 fill active coverage epoch", 1)
            # The active coverage center describes streaming coverage and can lag the player.
            fill_world_position(sphere["active_coverage_center"], "S1 fill active coverage")
            fill_positions.append(fill_world_position(sphere["world_loading_center"], "S1 fill movement"))
            integer(sphere["ready_chunks"], "S1 fill ready chunks")
            integer(sphere["required_chunks"], "S1 fill required chunks", 1)
            verify_s1_full_volume_sample(sphere, "S1 fill", require_ready=False)
            require(type(sphere["ready"]) is bool,
                    "S1 fill-ramp readiness state must be boolean")
            require(sphere["ready"] is False and sphere["ready_chunks"] < sphere["required_chunks"],
                    "S1 pre-Ready fill sample contradicts the first-ready timestamp")
        require(len(fill_positions) >= 2 and len(set(fill_positions)) > 1,
                "S1 fill-ramp sends lack authoritative player displacement before 320 Ready")

    for frame in movement_frames:
        frame_start = integer(frame["started_offset_ns"], "S1 movement frame start")
        frame_end = frame_start + integer(frame["loop_duration_ns"], "S1 movement frame duration", 1)
        require(frame_start < first_loop_start + PERFORMANCE_SAMPLE_MS * 1_000_000
                and frame_end > first_loop_start,
                "S1 movement frame lies outside the measured interval")

    budget = game.get("frame_budget")
    expected_budget_start = first_loop_start
    budget_passed = verify_fresh_frame_budget(
        budget, frames, game.get("raw_fence_confirmed_frames"), game.get("frame_completion_source"),
        True, platform, movement_phase=S1_MOVEMENT_PHASE, expected_start_ns=expected_budget_start,
    )
    frame_timeline_end = max(
        integer(frame["started_offset_ns"], "S1 frame start")
        + integer(frame["loop_duration_ns"], "S1 frame duration", 1)
        for frame in frames
    )
    budget_start = integer(budget.get("measurement_window_start_offset_ns"), "S1 frame-budget start")
    budget_end = budget_start + PERFORMANCE_SAMPLE_MS * 1_000_000
    movement_fence_tail = max((
        integer(completion.get("completion_observed_offset_ns"), "S1 fence observation offset")
        for completion in game["raw_fence_confirmed_frames"]
        if isinstance(completion, dict)
        and completion.get("submission_phase") == S1_MOVEMENT_PHASE
        and type(completion.get("submitted_offset_ns")) is int
        and budget_start <= completion["submitted_offset_ns"] < budget_end
    ), default=budget_end)
    require(start_ack_offset + max(frame_timeline_end, movement_fence_tail)
            <= elapsed_ms * 1_000_000 + 1_000_000,
            "S1 raw frame and bounded fence-drain evidence extends beyond the runtime report")

    verify_movement_playable_sphere(
        s1, frames, budget, MOVEMENT_TRAJECTORY_SAMPLE_SCOPE,
        movement_phase=S1_MOVEMENT_PHASE, require_boundary=False, require_source_contract=False,
        expected_radius_blocks=ACTUAL_TERRAIN_RADIUS_BLOCKS,
    )
    require(game.get("movement_trajectory_contract") == MOVEMENT_TRAJECTORY_CONTRACT,
            "S1 x30 authoritative movement trajectory contract is absent or unsupported")
    verify_x30_authoritative_trajectory(game, s1, frames, budget, movement_phase=S1_MOVEMENT_PHASE)

    visual_samples = [frame.get("visual_lod") for frame in movement_frames]
    visual_samples.append(s1["movement_trajectory_anchor"]["visual_lod"])
    visual_samples.extend(update["visual_lod"] for update in s1["movement_authoritative_sphere_updates"])
    visual_samples.extend(update["visual_lod"] for update in s1["movement_boundary_authoritative_updates"])
    require(all(visual["terrain_identity"] == terrain_identity
                and visual["session_join_epoch"] == session_epoch for visual in visual_samples),
            "presented visual scene belongs to another terrain identity or session")
    submission_offset = None
    for frame in movement_frames:
        if not frame["presentation_succeeded"]:
            continue
        require(frame.get("diagnostic_frame_recorded") is True,
                "successful movement frame has no recorded render work")
        offset = frame["visual_lod"]["submitted_frame_count"] - integer(
            frame.get("submitted_frame_sequence_id"), "movement frame sequence", 1)
        require(offset >= 0 and (submission_offset is None or submission_offset == offset),
                "presented visual scene receipt is not bound to the sampled frame submission")
        submission_offset = offset

    ticks = [integer(value, "server tick", 1) for value in game.get("raw_server_tick_durations_ns", [])]
    require(ticks and game.get("server_tick_timings_ns") == timing_summary(ticks),
            "S1 raw server timing summary mismatch")
    raw_unsuccessful = sum(not frame["presentation_succeeded"] for frame in frames)
    integer(game.get("failed_present_requests"), "failed_present_requests")
    integer(game.get("successful_present_requests"), "successful_present_requests")
    require(game["failed_present_requests"] == raw_unsuccessful
            and game["successful_present_requests"] == len(frames) - raw_unsuccessful,
            "S1 raw attempt totals mismatch")
    require(budget_passed, f"S1 fresh-frame budget exceeds 1% of the {FRESH_FRAME_TARGET_HZ[platform]}Hz movement window")

    return {
        "status": "passed", "accepted_exception": None, "proof": "performance", "render_distance": 256,
        "coverage": "passed", "request_cadence_status": "not-required", "request_cadence_required": False,
        "request_cadence_reasons": [], "request_cadence_proxy": {},
        "total_unsuccessful_attempts": game["failed_present_requests"],
        "raw_unsuccessful_attempts": raw_unsuccessful, "presentation_timing_required": False,
        "presentation_timing_status": "not-required", "presentation_timing_metrics": {"status": "not-required"},
        "physical_display_cadence": "unavailable",
    }


def verify_report(report: dict[str, Any], platform: str, proof: str | None = None,
                  *, cadence_exception: dict[str, Any] | None = None) -> dict[str, Any]:
    if isinstance(report, dict):
        candidate_game = report.get("game_benchmark")
        if isinstance(candidate_game, dict) and candidate_game.get("qualification_proof") == "performance":
            return verify_s1_performance_report(
                report, platform, proof=proof, cadence_exception=cadence_exception,
            )
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
    frame_budget_required = cadence_required
    require(game.get("diagnostic_overrides_present") is False, "diagnostic gameplay report is not qualification proof")
    expected_workload = WORKLOAD if actual_proof == "performance" else COVERAGE_WORKLOAD
    require(game["workload"] == expected_workload and game["build_mode"] == "release",
            "requires the proof-specific Release full-radius workload")
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
    frame_budget = game.get("frame_budget")
    budget_passed = verify_fresh_frame_budget(
        frame_budget, frames, game.get("raw_fence_confirmed_frames"),
        game.get("frame_completion_source"), frame_budget_required, platform
    )
    if actual_proof == "performance":
        require(game.get("movement_trajectory_contract") == MOVEMENT_TRAJECTORY_CONTRACT,
                "x30 authoritative movement trajectory contract is absent or unsupported")
        movement_sample_scope = (
            MOVEMENT_TRAJECTORY_SAMPLE_SCOPE
            if game.get("movement_trajectory_contract") == MOVEMENT_TRAJECTORY_CONTRACT
            else PLAYABLE_SPHERE_SAMPLE_SCOPE
        )
        verify_movement_playable_sphere(full, frames, frame_budget, movement_sample_scope)
        verify_x30_authoritative_trajectory(game, full, frames, frame_budget)
    require(not request_cadence_required or not cadence_failures, "; ".join(cadence_failures))
    require(not frame_budget_required or budget_passed,
            f"{FRESH_FRAME_TARGET_HZ[platform]}Hz fresh-frame interval budget exceeds 1% of the moving window")
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
        if row["proof"] == "terrain-coverage":
            require(raw["game_benchmark"]["full_radius_terrain"]["warmup_timeout_ms"] == row["warmup_seconds"] * 1000,
                    "capture command warmup disagrees with raw report")
    if s7_windows_secondary:
        results["windows"] = {
            "status": "deferred",
            "reason": "Windows gameplay qualification was not selected for this S7 snapshot.",
        }
    return results


def verify_s1_owner_accepted_report_failure(report: Any) -> dict[str, Any]:
    """Validate the single owner-accepted S1 gameplay failure without relabeling it as a pass."""
    expected = {
        "report_failure": "game benchmark did not complete the scripted full-game workload",
        "in_budget_time_ns": 14_018_509_225,
        "measured_time_ns": 30_000_000_000,
        "required_in_budget_permille": 990,
        "target_frame_duration_ns": 8_333_333,
        "movement_playable_sphere_complete": False,
        "movement_visual_lod_samples_complete": False,
        "ready_prompt_frame_offset_ns": 3_434_526_750,
        "playable_sphere_first_ready_offset_ns": 14_323_644_834,
        "initial_deadline_missed": False,
        "playable_deadline_missed": False,
    }
    game = report.get("game_benchmark") if isinstance(report, dict) else None
    timing = report.get("timing") if isinstance(report, dict) else None
    s1 = game.get("s1_terrain_performance") if isinstance(game, dict) else None
    budget = game.get("frame_budget") if isinstance(game, dict) else None
    require(
        isinstance(report, dict)
        and report.get("schema_version") == "mc.runtime-evidence.v1"
        and report.get("mode") == "benchmark-game"
        and report.get("passed") is False
        and report.get("failure") == expected["report_failure"]
        and isinstance(timing, dict)
        and timing.get("elapsed_ms") == 45_705
        and timing.get("deadline_ms") == 60_000
        and isinstance(game, dict)
        and game.get("workload") == WORKLOAD
        and game.get("build_mode") == "release"
        and game.get("qualification_proof") == "performance"
        and isinstance(s1, dict)
        and s1.get("contract") == S1_PERFORMANCE_CONTRACT
        and s1.get("initial_radius_blocks") == 48
        and s1.get("actual_radius_blocks") == 640
        and s1.get("visual_lod_radius_blocks") == 320
        and s1.get("ready_prompt_frame_offset_ns") == expected["ready_prompt_frame_offset_ns"]
        and s1.get("playable_sphere_first_ready_offset_ns")
            == expected["playable_sphere_first_ready_offset_ns"]
        and s1.get("initial_deadline_missed") is expected["initial_deadline_missed"]
        and s1.get("playable_deadline_missed") is expected["playable_deadline_missed"]
        and s1.get("movement_playable_sphere_complete")
            is expected["movement_playable_sphere_complete"]
        and s1.get("movement_visual_lod_samples_complete")
            is expected["movement_visual_lod_samples_complete"]
        and isinstance(budget, dict)
        and budget.get("target_frame_duration_ns") == expected["target_frame_duration_ns"]
        and budget.get("required_in_budget_time_permille")
            == expected["required_in_budget_permille"]
        and budget.get("measured_time_ns") == expected["measured_time_ns"]
        and budget.get("in_budget_time_ns") == expected["in_budget_time_ns"]
        and budget.get("passed") is False,
        "owner-accepted failure metrics differ from the retained Mac capture",
    )
    return expected


S1_CURRENT_NORMAL = {'aggregate_sha256': '2a437863daa4bcb334384d0d23fa4dca65a5e2230f608930983d7d51df091ac7',
 'executable_sha256': 'd8035bd0a014033f68d58a7d82f51bda81b51a1a5f280ecdb0db0ef83601f6ab',
 'facts': {'counters': {'accepted_tick': 0,
                        'authoritative_tick_ms': 100,
                        'camera_relative_inputs': 0,
                        'clients_accepted': 1,
                        'clients_requested': 1,
                        'expectations_passed': 0,
                        'inputs_sent': 431,
                        'last_effective_tick': 0,
                        'server_events_processed': 9836,
                        'ticks': 300},
           'failed_present_requests': 4,
           'frame_budget': {'fence_confirmed_frame_count': 3467,
                            'in_budget_time_ns': 12406228240,
                            'measured_time_ns': 30000000000,
                            'measurement_scope': 'fence-confirmed-render-frame-completion-observations',
                            'measurement_window_complete': True,
                            'measurement_window_end_offset_ns': 42096340375,
                            'measurement_window_start_offset_ns': 12096340375,
                            'passed': False,
                            'required_for_benchmark_pass': True,
                            'required_in_budget_time_permille': 990,
                            'required_window_duration_ns': 30000000000,
                            'sample_count': 3467,
                            'target_frame_duration_ns': 8333333},
           'raw_fence_confirmed_frame_count': 5004,
           'raw_frame_count': 4984,
           'raw_input_count': 431,
           's1': {'actual_radius_blocks': 640,
                  'contract': 'mc.s1-lod-performance.v2',
                  'initial_deadline_missed': False,
                  'initial_radius_blocks': 48,
                  'movement_authoritative_update_count': 301,
                  'movement_input_during_fill': True,
                  'movement_playable_sphere_complete': False,
                  'movement_started_after_current_sphere_ready': True,
                  'movement_visual_lod_samples_complete': False,
                  'playable_deadline_missed': False,
                  'playable_sphere_first_ready_offset_ns': 14246828125,
                  'ready_prompt_frame_offset_ns': 2303056750,
                  'visual_lod_radius_blocks': 320},
           'successful_present_requests': 718,
           'timing': {'deadline_ms': 60000, 'elapsed_ms': 45525}},
 'failure': 'game benchmark did not complete the scripted full-game workload',
 'raw_report_sha256': '0019d1c2c8d66e70be325dffeb022f56b5abfd95db7a0bd7332d0f52cf5b6c9b',
 'schema': 'mc.s1-owner-accepted-current-normal.v2',
 'semantic_review': {'path': '/Users/core-just/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/review-artifacts/source13-analysis-source-review-consolidated-v3.json',
                     'sha256': '5b50a1855bf846fb9337557a9c65535179e9ed5d7fa98ce2d92dadf732be3e82',
                     'size': 18685},
 'source_candidate': {'head': '168bbf38b367f9621f49286ccca85a8a42095289',
                      'tree': '0fbf093c94770683961ca4a0653fd98f7c5690f7'},
 'source_input_sha256': '0dc600d5809fc661c3b73d86fe05dca951b661fe58ef7fc8c1f358651c3a3471'}

def verify_s1_current_normal_report(report: dict[str, Any]) -> dict[str, Any]:
    """Validate the pinned fresh normal capture without converting its failures into a pass."""
    require(report.get("schema_version") == "mc.runtime-evidence.v1"
            and report.get("mode") == "benchmark-game" and report.get("passed") is False
            and report.get("failure") == S1_CURRENT_NORMAL["failure"], "current normal capture failure changed")
    game = report.get("game_benchmark")
    require(isinstance(game, dict), "current normal gameplay evidence is absent")
    terrain = game.get("s1_terrain_performance")
    budget = game.get("frame_budget")
    require(isinstance(terrain, dict) and isinstance(budget, dict), "current normal terrain/timing evidence absent")
    require(game.get("workload") == WORKLOAD and game.get("qualification_proof") == "performance"
            and game.get("build_mode") == "release" and game.get("diagnostic_overrides_present") is False
            and game.get("qualification_settings_manifest") == qualification_settings_manifest()
            and game.get("framebuffer") == {"width": 2560, "height": 1440}
            and game.get("present_mode") == "fifo", "current normal renderer/settings changed")
    require(terrain.get("contract") == S1_PERFORMANCE_CONTRACT
            and terrain.get("initial_radius_blocks") == 48 and terrain.get("actual_radius_blocks") == 640
            and terrain.get("visual_lod_radius_blocks") == 320, "current normal terrain radii changed")
    require(terrain.get("initial_deadline_missed") is False
            and 0 < terrain.get("ready_prompt_frame_offset_ns", 0) <= 4_000_000_000
            and terrain.get("playable_deadline_missed") is False,
            "current startup deadline failure is not waived")
    timing, counters = report.get("timing"), report.get("counters")
    require(isinstance(timing, dict) and 0 < timing.get("elapsed_ms", 0) <= timing.get("deadline_ms", 0),
            "current normal runtime deadline failed")
    require(isinstance(counters, dict) and counters.get("clients_requested") == counters.get("clients_accepted") == 1
            and counters.get("inputs_sent", 0) > 0 and terrain.get("movement_authoritative_update_count", 0) > 0,
            "current normal connection/input/movement evidence absent")
    updates = terrain.get("movement_authoritative_sphere_updates")
    require(isinstance(updates, list) and len(updates) == terrain.get("movement_authoritative_update_count")
            and len(updates) >= 2, "current authoritative movement trajectory is absent or malformed")
    position_keys = ("x", "y", "z", "x_subcell", "y_subcell", "z_subcell")
    positions = set()
    for update in updates:
        require(isinstance(update, dict) and isinstance(update.get("authoritative_position"), dict),
                "current authoritative movement position is malformed")
        position = update["authoritative_position"]
        require(all(type(position.get(key)) is int for key in position_keys),
                "current authoritative movement position is malformed")
        positions.add(tuple(position[key] for key in position_keys))
    require(len(positions) >= 2, "current authoritative movement trajectory is stationary")
    require(game.get("successful_present_requests", 0) > 0 and len(game.get("raw_frames", [])) > 0
            and len(game.get("raw_fence_confirmed_frames", [])) > 0 and len(game.get("raw_gameplay_inputs", [])) > 0
            and game.get("frame_completion_source") == FRAME_COMPLETION_SOURCE,
            "current normal rendered/fence-completed gameplay evidence absent")
    require(budget.get("measurement_window_complete") is True
            and budget.get("measured_time_ns") == budget.get("required_window_duration_ns") == 30_000_000_000,
            "current normal 30-second measurement window incomplete")
    require(budget.get("passed") is False and terrain.get("movement_playable_sphere_complete") is False
            and terrain.get("movement_visual_lod_samples_complete") is False,
            "current failure classes differ from accepted performance/LOD classes")
    facts = {"timing": timing, "counters": counters, "frame_budget": budget,
             "s1": {key: terrain.get(key) for key in S1_CURRENT_NORMAL["facts"]["s1"]},
             "raw_frame_count": len(game["raw_frames"]),
             "raw_fence_confirmed_frame_count": len(game["raw_fence_confirmed_frames"]),
             "raw_input_count": len(game["raw_gameplay_inputs"]),
             "successful_present_requests": game["successful_present_requests"],
             "failed_present_requests": game["failed_present_requests"]}
    require(facts == S1_CURRENT_NORMAL["facts"], "current capture metrics differ from the pinned fresh evidence")
    return facts


def verify_s1_current_normal_receipt(root: Path, receipt: Path, *, candidate_commit: str) -> dict[str, Any]:
    source = source_digest(root)
    require(source == S1_CURRENT_NORMAL["source_input_sha256"], "current source differs from fresh normal capture")
    require(file_identity(receipt)["sha256"] == S1_CURRENT_NORMAL["aggregate_sha256"],
            "current normal capture aggregate changed")
    aggregate = read_json(receipt)
    require(aggregate.get("schema_version") == SCHEMA and len(aggregate.get("rows", [])) == 1,
            "current normal evidence must contain exactly one Mac performance row")
    row = aggregate["rows"][0]
    require(row.get("platform") == "macos" and row.get("proof") == "performance"
            and row.get("proof_origin") == "local" and row.get("source_candidate") == S1_CURRENT_NORMAL["source_candidate"]
            and row.get("source_input_sha256") == source and row.get("exit_code") == 1
            and row.get("timed_out") is False and row.get("capture_error") == "" and row.get("environment") == {},
            "current normal capture identity/state changed")
    build = row.get("build", {})
    require(build.get("configuration") == "Release" and build.get("source_input_sha256") == source
            and build.get("executable_sha256") == S1_CURRENT_NORMAL["executable_sha256"],
            "current normal Release build identity changed")
    executable = checked_file(row["executable"], receipt.parent)
    report = checked_file(row["report"], receipt.parent)
    require(file_identity(executable)["sha256"] == S1_CURRENT_NORMAL["executable_sha256"]
            and file_identity(report)["sha256"] == S1_CURRENT_NORMAL["raw_report_sha256"],
            "current normal executable/report identity changed")
    require(row.get("warmup_seconds") == 180 and row["command"][1:] == benchmark_arguments(
                "macos", 180, row["command"][-1], "performance"), "current normal workload command changed")
    verify_macos_runtime(row, receipt.parent)
    review = read_json(checked_file(S1_CURRENT_NORMAL["semantic_review"], root))
    require(review.get("verdict") == "APPROVED"
            and review.get("candidate", {}).get("head_commit") == row["source_candidate"]["head"]
            and review.get("candidate", {}).get("head_tree") == row["source_candidate"]["tree"],
            "current normal semantic review changed")
    facts = verify_s1_current_normal_report(read_json(report))
    return {"schema_version": S1_CURRENT_NORMAL["schema"], "status": "accepted_exception",
            "owner_request_path": S1_OWNER_REQUEST_PATH, "owner_request_sha256": S1_OWNER_REQUEST_SHA256,
            "source_input_sha256": source, "capture_source_input_sha256": source,
            "source_reuse_certificate": None, "candidate": _MATRIX.commit_identity(root, candidate_commit),
            "capture": {"aggregate_sha256": S1_CURRENT_NORMAL["aggregate_sha256"],
                        "raw_report_sha256": S1_CURRENT_NORMAL["raw_report_sha256"],
                        "executable_sha256": S1_CURRENT_NORMAL["executable_sha256"],
                        "source_candidate": row["source_candidate"]},
            "proof_dispositions": {"macos": {"performance": "failed", "terrain-coverage": "unqualified"},
                                   "windows": {"performance": "unqualified", "terrain-coverage": "unqualified"}},
            "known_failure": {"report_failure": S1_CURRENT_NORMAL["failure"], **facts},
            "owner_tested_windows_executable_sha256": S1_OWNER_WINDOWS_EXECUTABLE_SHA256,
            "current_normal_runtime": {"semantic_review": S1_CURRENT_NORMAL["semantic_review"],
                "scope": "fresh current-source normal Mac rendering/input/movement evidence; performance and LOD failed; no new owner playtest or Windows qualification"}}


def verify_s1_owner_accepted_receipt(
    root: Path,
    receipt: Path,
    *,
    candidate_commit: str,
) -> dict[str, Any]:
    """Accept only the exact owner-approved S1 Mac failure and report other proofs unqualified."""
    if file_identity(receipt)["sha256"] == S1_CURRENT_NORMAL["aggregate_sha256"]:
        return verify_s1_current_normal_receipt(root, receipt, candidate_commit=candidate_commit)
    source = source_digest(root)
    source_reuse_certificate = None
    capture_source = source
    if source != S1_OWNER_SOURCE_INPUT_SHA256:
        source_reuse_certificate = s1_owner_source_reuse_certificate(root, source)
        capture_source = S1_OWNER_SOURCE_INPUT_SHA256
    candidate = _MATRIX.commit_identity(root, candidate_commit)
    receipt_identity = file_identity(receipt)
    require(receipt_identity["sha256"] == S1_OWNER_AGGREGATE_SHA256,
            "owner-accepted aggregate receipt differs from the retained Mac record")
    aggregate = read_json(receipt)
    require(isinstance(aggregate, dict) and aggregate.get("schema_version") == SCHEMA
            and isinstance(aggregate.get("rows"), list) and len(aggregate["rows"]) == 1,
            "owner-accepted S1 evidence must contain only the retained single Mac performance row")
    row = aggregate["rows"][0]
    require(isinstance(row, dict)
            and row.get("platform") == "macos"
            and row.get("proof") == "performance"
            and row.get("proof_origin") == "local"
            and row.get("source_candidate") == S1_OWNER_CAPTURE_CANDIDATE
            and row.get("source_input_sha256") == capture_source
            and row.get("exit_code") == 1
            and row.get("timed_out") is False
            and row.get("capture_error") == ""
            and row.get("environment") == {},
            "owner-accepted Mac row identity or failed-capture state changed")
    _MATRIX._validate_candidate(row.get("source_candidate"), "owner-accepted capture candidate")
    build = row.get("build")
    require(isinstance(build, dict)
            and build.get("configuration") == "Release"
            and build.get("source_input_sha256") == capture_source
            and build.get("executable_sha256") == S1_OWNER_MAC_EXECUTABLE_SHA256,
            "owner-accepted Mac Release build identity changed")
    for key in ("configuration_sha256", "dependency_sha256"):
        require(isinstance(build.get(key), str)
                and re.fullmatch(r"[0-9a-f]{64}", build[key]) is not None,
                f"owner-accepted Mac build is missing {key}")

    executable = row.get("executable")
    report = row.get("report")
    require(isinstance(executable, dict)
            and isinstance(executable.get("path"), str)
            and Path(executable["path"]).is_absolute()
            and executable.get("sha256") == S1_OWNER_MAC_EXECUTABLE_SHA256
            and Path(executable["path"]).name == "mc_main"
            and isinstance(report, dict)
            and isinstance(report.get("path"), str)
            and Path(report["path"]).is_absolute()
            and report.get("sha256") == S1_OWNER_RAW_REPORT_SHA256,
            "owner-accepted Mac executable or raw report identity changed")
    executable_path = checked_file(executable, receipt.parent)
    report_path = checked_file(report, receipt.parent)
    require(build["executable_sha256"] == file_identity(executable_path)["sha256"],
            "owner-accepted Mac executable bytes disagree with the Release build")
    require(row.get("warmup_seconds") == 180
            and isinstance(row.get("command"), list)
            and row["command"][1:] == benchmark_arguments(
                "macos", row["warmup_seconds"], row["command"][-1], "performance"
            )
            and Path(row["command"][-1]).resolve() == report_path,
            "owner-accepted Mac capture command is not the retained production workload")
    verify_macos_runtime(row, receipt.parent)
    known_failure = verify_s1_owner_accepted_report_failure(read_json(report_path))
    return {
        "schema_version": S1_OWNER_EXCEPTION_SCHEMA,
        "status": "accepted_exception",
        "owner_request_path": S1_OWNER_REQUEST_PATH,
        "owner_request_sha256": S1_OWNER_REQUEST_SHA256,
        "source_input_sha256": source,
        "capture_source_input_sha256": capture_source,
        "source_reuse_certificate": source_reuse_certificate,
        "candidate": candidate,
        "capture": {
            "aggregate_sha256": receipt_identity["sha256"],
            "raw_report_sha256": report["sha256"],
            "executable_sha256": executable["sha256"],
            "source_candidate": row["source_candidate"],
        },
        "proof_dispositions": {
            "macos": {
                "performance": "failed",
                "terrain-coverage": "unqualified",
            },
            "windows": {
                "performance": "unqualified",
                "terrain-coverage": "unqualified",
            },
        },
        "known_failure": known_failure,
        "owner_tested_windows_executable_sha256": S1_OWNER_WINDOWS_EXECUTABLE_SHA256,
    }


def benchmark_arguments(platform: str, warmup: int, report: str, proof: str = "performance") -> list[str]:
    require(type(warmup) is int and 1 <= warmup <= 1800, "invalid warmup bound")
    require(proof in PROOFS, "invalid gameplay proof selector")
    workload = "ordinary-turning" if proof == "performance" else "speed-200-full-radius"
    return [
        "--benchmark-game",
        "--workload",
        workload,
        "--gameplay-proof",
        proof,
        "--render-distance",
        str(PROOFS[proof]),
        *( ["--full-radius-warmup-seconds", str(warmup)] if proof == "terrain-coverage" else []),
        *( ["--present-immediate"] if platform == "windows" else []),
        "--evidence",
        report,
    ]


def direct_prefix_dependency_identity(root: Path, cache: dict[str, str], prefixes: list[str]) -> dict[str, Any]:
    resolved = [Path(prefix).resolve() for prefix in prefixes]
    require(all(Path(prefix).is_absolute() and path.is_dir() for prefix, path in zip(prefixes, resolved)),
            "direct dependency prefix is missing or is not an absolute directory")
    # CMake records the packages actually selected, even without a vcpkg toolchain.
    # Every selected dependency directory must be covered by the hashed prefixes.
    for name, value in cache.items():
        if not name.endswith("_DIR") or name.startswith(("CMAKE_", "MinecraftClone2026_")):
            continue
        directory = Path(value)
        require(directory.is_absolute() and directory.is_dir()
                and any(directory.resolve().is_relative_to(prefix) for prefix in resolved),
                f"selected dependency directory {name} is not covered by a CMake prefix")
    lock = read_json(root / "dependencies.lock.json")["dependencies"]
    packages = {}
    for package in ("CoreCpp", "CoreProject2026"):
        directory = cache.get(f"{package}_DIR", "")
        config = Path(directory) / f"{package}Config.cmake"
        require(bool(directory) and config.is_file(), f"{package} package provenance is missing")
        content = config.read_text(encoding="utf-8")

        def setting(name: str) -> str:
            matches = re.findall(r'^\s*set\(' + re.escape(name) + r'\s+"([^"\r\n]*)"\s*\)\s*$',
                                 content, re.M)
            require(len(matches) == 1, f"{package} package provenance lacks an unambiguous {name}")
            return matches[0]

        revision = setting(f"{package}_REVISION")
        require(re.fullmatch(r"[0-9a-f]{40}", revision) is not None
                and revision == lock[package]["revision"]
                and setting(f"{package}_DIRTY") in ("FALSE", "OFF", "0")
                and setting(f"{package}_REVISION_EXACT") in ("TRUE", "ON", "1"),
                f"{package} package provenance is not the locked clean exact revision")
        if package == "CoreProject2026":
            require(setting("CoreProject2026_REQUIRED_CORECPP_REVISION") == lock["CoreCpp"]["revision"],
                    "CoreProject2026 package requires another CoreCpp revision")
        packages[package] = {"directory": directory, "config": file_identity(config),
                             "revision": revision, "dirty": False, "exact": True}
    identities = []
    for prefix, path in zip(prefixes, resolved):
        files = _MATRIX._input_file_paths(path, ("include/**", "lib/**", "bin/**", "share/**/*.cmake"))
        require(bool(files), f"direct dependency prefix has no consumed package files: {prefix}")
        identities.append({"path": prefix, "consumed_files_sha256": _MATRIX._paths_sha256(files),
                           "consumed_file_count": len(files)})
    return {"CMAKE_PREFIX_PATH": identities, "package_provenance": packages}


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
    vcpkg_keys = ("Z_VCPKG_ROOT_DIR", "VCPKG_INSTALLED_DIR")
    if not any(name in cache for name in vcpkg_keys):
        dependencies = direct_prefix_dependency_identity(root, cache, private_prefixes)
    else:
        require(all(cache.get(name) for name in vcpkg_keys), "vcpkg dependency configuration is incomplete")
        dependencies = vcpkg_dependency_identity(root, cache, private_prefixes)
    return {"configuration": "Release", "source_input_sha256": source_digest(root),
            "executable_sha256": file_identity(executable)["sha256"],
            "configuration_sha256": _MATRIX._input_files_sha256(build_root, ("CMakeCache.txt", "build.ninja", "CMakeFiles/rules.ninja", "compile_commands.json", "shaders/**")),
            "dependency_sha256": hashlib.sha256(_MATRIX._canonical_json(dependencies)).hexdigest()}


def vcpkg_dependency_identity(root: Path, cache: dict[str, str], private_prefixes: list[str]) -> dict[str, Any]:
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
    return dependencies


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
    parser.add_argument(
        "--s1-owner-accepted",
        action="store_true",
        help="allow only the pinned S1 owner-accepted one-row exception; otherwise require all four normal proofs",
    )
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
        require(not args.s1_owner_accepted or args.candidate_commit is not None,
                "S1 owner acceptance requires an immutable source candidate commit")
        require(not args.s1_owner_accepted or not (args.record or args.merge
                or args.s7_windows_secondary or args.s7_cadence_exception),
                "S1 owner acceptance applies only to the S1 aggregate receipt")
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
            if args.s1_owner_accepted:
                receipt_path = args.receipt.resolve(strict=True)
                aggregate = read_json(receipt_path)
                rows = aggregate.get("rows") if isinstance(aggregate, dict) else None
                if isinstance(rows, list) and len(rows) == 1:
                    result = verify_s1_owner_accepted_receipt(
                        root,
                        receipt_path,
                        candidate_commit=args.candidate_commit,
                    )
                    print(json.dumps(result, sort_keys=True))
                else:
                    results = verify_receipt(root, receipt_path)
                    accepted = any(proof.get("status") == "accepted_exception"
                                   for platform in results.values()
                                   for proof in platform.values()
                                   if isinstance(proof, dict))
                    print(json.dumps({"status": "accepted_exception" if accepted else "passed",
                                      "platforms": results}, sort_keys=True))
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
