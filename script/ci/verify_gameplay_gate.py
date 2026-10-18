#!/usr/bin/env python3
"""Capture and verify local Release full-radius gameplay qualification."""

from __future__ import annotations

import argparse
import functools
import hashlib
import importlib.util
import json
import math
import os
import subprocess
import sys
import time
from pathlib import Path, PureWindowsPath
from typing import Any, Sequence


SCRIPT_ROOT = Path(__file__).resolve().parents[1]
_SPEC = importlib.util.spec_from_file_location("ai_analysis_matrix", SCRIPT_ROOT / "ai_analysis_matrix.py")
assert _SPEC is not None and _SPEC.loader is not None
_MATRIX = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_MATRIX)
SCHEMA = "mc.gameplay-proof.v1"
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
TARGET_HZ = {"macos": 120, "windows": 500}
CADENCE_POLICIES = ("snapshot", "strict")
WORLD_TILE_COUNT = 4_096
QUEUE_FIELDS = ("pending_delivery_batches", "pending_residency_changes", "pending_mesh_queue",
                "active_mesh_jobs", "dirty_mesh_jobs", "worker_results", "worker_outstanding", "pending_removals")


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
    return json.loads(target.read_text(encoding="utf-8"), parse_constant=lambda value: (_ for _ in ()).throw(
        GameplayError(f"non-finite JSON value: {value}")))


def file_identity(target: Path) -> dict[str, Any]:
    require(target.is_file() and not target.is_symlink(), f"missing regular evidence file: {target}")
    data = target.read_bytes()
    return {"path": str(target.resolve()), "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def checked_file(descriptor: Any, base: Path) -> Path:
    require(isinstance(descriptor, dict), "file identity must be an object")
    target = base / descriptor["path"]
    identity = file_identity(target)
    require(all(identity[key] == descriptor.get(key) for key in ("size", "sha256")), f"altered evidence: {target}")
    return target


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


def verify_barrier(barrier: dict[str, Any], phase: str, elapsed: int) -> None:
    require(barrier["phase"] == phase and all(barrier[key] is True for key in
            ("scan_performed", "potentially_ready", "exact_current_coverage")), f"{phase}: missing exact coverage scan")
    tile_center(barrier["center"], phase)
    require(integer(barrier["phase_elapsed_ms"], f"{phase}: barrier elapsed") == elapsed, f"{phase}: barrier elapsed disagrees with warmup")
    for key in ("desired", "resident_desired", "current_handle_mesh_visible", "visible_meshes", "renderer_mesh_slots"):
        require(integer(barrier[key], key) == 205861, f"{phase}: {key} must be 205861")
    require(integer(barrier["resident_total"], "resident_total") >= 205861, f"{phase}: incomplete residency")
    for key in (*QUEUE_FIELDS, "nonresident_desired", "stale_or_missing_mesh", "unclassified_resident_missing",
                "visible_outside_interest", "unclassified_current_visible_mesh_detail"):
        require(integer(barrier[key], key) == 0, f"{phase}: {key} is not drained")
    details = barrier["current_visible_mesh_detail_counts"]
    require(isinstance(details, dict) and set(details) == {"coarse", "coarse2", "coarse4", "fine", "distant"}
            and sum(integer(value, "mesh detail") for value in details.values()) == 205861, f"{phase}: mesh details mismatch")
    expected = radial_band_counts(256)
    bands = barrier["radial_bands"]
    require(barrier["radial_band_tile_width"] == 16 and len(bands) == 16, f"{phase}: incomplete radial bands")
    for band, count in zip(bands, expected):
        require(all(integer(band[key], key) == count for key in ("desired", "resident", "current_mesh_visible"))
                and band["nonresident"] == 0 and band["stale_or_missing_mesh"] == 0, f"{phase}: radial coverage mismatch")


def verify_report(report: dict[str, Any], platform: str, cadence_policy: str = "strict") -> dict[str, Any]:
    require(cadence_policy in CADENCE_POLICIES, "invalid cadence policy")
    cadence_required = platform == "windows" or cadence_policy == "strict"
    require(report["schema_version"] == "mc.runtime-evidence.v1" and report["mode"] == "benchmark-game"
            and report["passed"] is True and report["failure"] == "", "runtime workload did not pass")
    timing = report["timing"]
    require(integer(timing["elapsed_ms"], "elapsed_ms", 1) <= integer(timing["deadline_ms"], "deadline_ms", 1), "runtime timeout")
    game = report["game_benchmark"]
    require(game.get("diagnostic_overrides_present") is False, "diagnostic gameplay report is not qualification proof")
    require(game["workload"] == WORKLOAD and game["build_mode"] == "release", "requires Release full-radius workload")
    require(game["displayed_cadence_hz"] is None, "physical display cadence is unavailable in this contract")
    require(game["present_mode"] == ("immediate" if platform == "windows" else "fifo"), "incorrect negotiated present mode")
    for key in ("server_events_processed", "inputs_sent", "authoritative_player_updates"):
        integer(game[key], key, 1)
    full = game["full_radius_terrain"]
    require(full["contract"] == "mc.full-radius-terrain-barriers.v1" and full["render_distance"] == 256
            and full["expected_tile_count"] == 205861 and full["complete"] is True, "incomplete radius256 terrain contract")
    require(full["loop_timing_scope"] == "all-stationary-loop-attempts" and full["successful_present_request_interval_scope"]
            == "consecutive-successful-request-completions-not-physical-display-cadence", "incorrect timing scope")
    timeout = integer(full["warmup_timeout_ms"], "warmup_timeout_ms", 1000)
    require(timeout == 180000, "qualification requires the 180-second terrain warmup bound")
    for prefix, phase in (("initial", PHASES[0]), ("recovery", PHASES[3])):
        elapsed = integer(full[prefix + "_warmup_elapsed_ms"], prefix + " warmup elapsed")
        require(full[prefix + "_barrier_reached"] is True and elapsed <= timeout, f"{prefix}: warmup failed")
        verify_barrier(full[prefix + "_barrier"], phase, elapsed)
    movement_start_center = tile_center(full["movement_start_center"], "movement start")
    movement_end_center = tile_center(full["movement_end_center"], "movement end")
    recovery_center = tile_center(full["recovery_barrier"]["center"], "recovery")
    require(integer(full["movement_elapsed_ms"], "movement_elapsed_ms") >= 6000 and
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
        radial_bands = frame["diagnostic_terrain_radial_bands"]
        if radial_bands is None:
            require(integer(frame["diagnostic_terrain_spatial_scan_duration_ns"],
                            "terrain spatial scan duration") == 0,
                    "terrain spatial scan duration exists without radial bands")
        else:
            verify_terrain_radial_scan(radial_bands, frame["diagnostic_terrain_spatial_scan_duration_ns"])
            require(all(band["desired"] == count
                        for band, count in zip(radial_bands, radial_band_counts(full["render_distance"]))),
                    "terrain radial desired counts disagree with render radius")
            require(all(integer(band["desired"], "radial desired")
                        == integer(band["current_mesh_visible"], "radial current visible")
                        and integer(band["nonresident"], "radial nonresident") == 0
                        and integer(band["stale_or_missing_mesh"], "radial stale/missing") == 0
                        for band in radial_bands),
                    "full-radius diagnostic scan found missing terrain inside the render radius")
        previous_completion = start + duration
    require(tuple(observed_phases) == PHASES, "raw phase sequence is incomplete")
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
        elapsed = integer(full[prefix + "_stationary_elapsed_ms"], "stationary_elapsed_ms", 10000)
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
    proxy, cadence_failures = {}, []

    def check_cadence(condition: bool, message: str) -> None:
        if not condition:
            cadence_failures.append(message)

    for phase, elapsed_key in ((PHASES[1], "baseline_stationary_elapsed_ms"), (PHASES[2], "movement_elapsed_ms"),
                               (PHASES[4], "recovered_stationary_elapsed_ms")):
        selected = [frame for frame in frames if frame["phase"] == phase]
        completions = [frame["started_offset_ns"] + frame["loop_duration_ns"] for frame in selected if frame["presentation_succeeded"]]
        intervals = [right - left for left, right in zip(completions, completions[1:])]
        require(intervals and all(interval > 0 for interval in intervals), f"{phase}: request cadence is not computable from raw completions")
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
            check_cadence(lost * 1000 <= total * 2, f"{phase}: lost FIFO refresh-slot share exceeds 0.2% ({lost}/{total})")
            check_cadence(max(1, round(summary["p50"] * 120 / 1000000000)) == 1
                    and interval_ns * 120 * 998 <= len(intervals) * 1000000000 * 1000,
                    f"{phase}: FIFO median/mean cadence is below the 99.8% rate floor")
            cadence = {"contract": "fifo-nearest-120hz-request-slots", "lost_slots": lost, "total_slots": total,
                       "lost_slot_share": lost / total}
        else:
            check_cadence(summary["p99_8"] <= limit, f"{phase}: request-cadence proxy fails 500Hz at 99.8% (p99.8={summary['p99_8']}ns, limit={limit}ns)")
            excess_ns = sum(max(0, interval - limit) for interval in intervals)
            check_cadence(excess_ns * 1000 <= interval_ns * 2, f"{phase}: excess-time share exceeds 0.2% ({excess_ns}/{interval_ns}ns)")
            cadence = {"contract": "immediate-raw-request-completions", "excess_time_share": excess_ns / interval_ns}
        proxy[phase] = {"target_hz": TARGET_HZ[platform], "minimum_rate_hz": minimum_rate, "cadence": cadence,
                        "intervals_ns": summary, "attempts": len(selected),
                        "successful_requests": len(completions), "failed_requests": len(selected) - len(completions),
                        "first_loop_started_offset_ns": selected[0]["started_offset_ns"],
                        "last_loop_completed_offset_ns": selected[-1]["started_offset_ns"] + selected[-1]["loop_duration_ns"],
                        "phase_elapsed_ms": full[elapsed_key], "present_requests_per_second": rate,
                        "interval_scope": "half-open [previous_successful_completion, current_successful_completion)"}
    ticks = [integer(value, "server tick", 1) for value in game["raw_server_tick_durations_ns"]]
    require(ticks and game["server_tick_timings_ns"] == timing_summary(ticks), "raw server timing summary mismatch")
    require(not cadence_required or not cadence_failures, "; ".join(cadence_failures))
    return {"coverage": "passed", "request_cadence_status": "failed" if cadence_failures else "passed",
            "request_cadence_required": cadence_required, "request_cadence_reasons": cadence_failures,
            "request_cadence_proxy": proxy, "total_unsuccessful_attempts": game["failed_present_requests"],
            "raw_unsuccessful_attempts": raw_unsuccessful, "physical_display_cadence": "unavailable"}


def verify_receipt(root: Path, receipt: Path, cadence_policy: str = "strict") -> dict[str, Any]:
    aggregate = read_json(receipt)
    require(aggregate["schema_version"] == SCHEMA and isinstance(aggregate["rows"], list), "invalid gameplay proof schema")
    rows = aggregate["rows"]
    require(len(rows) == 2 and {row["platform"] for row in rows} == set(TARGET_HZ), "both macos and windows gameplay proof are required")
    expected_source = source_digest(root)
    results = {}
    for row in rows:
        require(row["proof_origin"] == "local" and row["source_input_sha256"] == expected_source, "stale or nonlocal gameplay source")
        _MATRIX._validate_candidate(row["source_candidate"], "gameplay source candidate")
        build = row["build"]
        require(build["configuration"] == "Release" and build["source_input_sha256"] == expected_source
                and build["executable_sha256"] == row["executable"]["sha256"], "stale gameplay build identity")
        for key in ("configuration_sha256", "dependency_sha256"):
            require(isinstance(build[key], str) and len(build[key]) == 64 and all(char in "0123456789abcdef" for char in build[key]), "missing build/dependency identity")
        executable = checked_file(row["executable"], receipt.parent)
        report = checked_file(row["report"], receipt.parent)
        require(row["command"][1:] == benchmark_arguments(row["platform"], row["warmup_seconds"], row["command"][-1]), "nonproduction gameplay command")
        require(PureWindowsPath(row["command"][0]).name == executable.name and PureWindowsPath(row["command"][-1]).name == report.name,
                "capture command disagrees with executable or report identity")
        require(integer(row["exit_code"], "exit_code") == 0 and row["timed_out"] is False and row["environment"] == {}, "failed or diagnostic gameplay invocation")
        require(executable.name == ("mc_main.exe" if row["platform"] == "windows" else "mc_main"), "wrong gameplay executable")
        raw = read_json(report)
        results[row["platform"]] = verify_report(raw, row["platform"], cadence_policy)
        require(raw["game_benchmark"]["full_radius_terrain"]["warmup_timeout_ms"] == row["warmup_seconds"] * 1000,
                "capture command warmup disagrees with raw report")
    return results


def benchmark_arguments(platform: str, warmup: int, report: str) -> list[str]:
    require(type(warmup) is int and 1 <= warmup <= 1800, "invalid warmup bound")
    return ["--benchmark-game", "--workload", "speed-200-full-radius", "--render-distance", "256",
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
    environment = {"VCPKG_ROOT": cache["Z_VCPKG_ROOT_DIR"], "VCPKG_INSTALLED_DIR": cache["VCPKG_INSTALLED_DIR"],
                   "MC_PRIVATE_DEPENDENCIES_PREFIX": cache["CMAKE_PREFIX_PATH"]}
    dependencies = _MATRIX._external_dependency_identity(root, {"id": "macos_ubsan"}, environment)
    return {"configuration": "Release", "source_input_sha256": source_digest(root),
            "executable_sha256": file_identity(executable)["sha256"],
            "configuration_sha256": _MATRIX._input_files_sha256(build_root, ("CMakeCache.txt", "build.ninja", "CMakeFiles/rules.ninja", "compile_commands.json", "shaders/**")),
            "dependency_sha256": hashlib.sha256(_MATRIX._canonical_json(dependencies)).hexdigest()}


def record(root: Path, receipt: Path, platform: str, executable: Path, warmup: int, cadence_policy: str = "strict") -> None:
    require(warmup == 180, "qualification capture requires --warmup-seconds 180; other bounds are diagnostic only")
    require(platform == ("windows" if sys.platform == "win32" else "macos" if sys.platform == "darwin" else "unsupported"), "capture must run on the named local host")
    diagnostics = {key: value for key, value in os.environ.items() if key.startswith("MC_DIAGNOSTIC_")}
    require(not diagnostics, "remove diagnostic renderer overrides before qualification")
    require(not receipt.exists(), "capture receipt already exists; select a fresh output path")
    report = receipt.with_suffix(".raw.json")
    require(not report.exists(), "raw capture path already exists")
    print(f"CAPTURE {platform}: fingerprinting relevant source, Release build and consumed dependencies", flush=True)
    before = build_identity(root, executable.parent, executable)
    command = [str(executable), *benchmark_arguments(platform, warmup, str(report))]
    started = time.monotonic()
    print(f"CAPTURE {platform}: bounded full-radius gameplay, timeout={warmup * 2 + 90}s", flush=True)
    process = subprocess.Popen(command, cwd=root)
    timed_out = False
    while process.poll() is None:
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            print(f"CAPTURE {platform}: elapsed={time.monotonic() - started:.0f}s; waiting for full-radius phases", flush=True)
            if time.monotonic() - started >= warmup * 2 + 90:
                timed_out = True
                process.kill()
                process.wait(timeout=30)
    after = build_identity(root, executable.parent, executable)
    require(before == after, "source, executable, build, or dependency inputs changed during capture")
    capture_error = "" if report.is_file() else "native report is absent"
    row = {"platform": platform, "proof_origin": "local", "source_candidate": _MATRIX.candidate_identity(root),
           "source_input_sha256": before["source_input_sha256"], "build": before, "executable": file_identity(executable),
           "report": None if capture_error else file_identity(report), "command": command, "warmup_seconds": warmup, "environment": diagnostics,
           "exit_code": process.returncode, "timed_out": timed_out, "capture_error": capture_error}
    receipt.write_text(json.dumps({"schema_version": SCHEMA, "rows": [row]}, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    require(process.returncode == 0 and not timed_out and not capture_error,
            "gameplay capture failed; retained failure receipt" + (": " + capture_error if capture_error else ""))
    print(json.dumps(verify_report(read_json(report), platform, cadence_policy), sort_keys=True))


def merge_receipts(root: Path, inputs: list[Path], output: Path, cadence_policy: str = "strict") -> dict[str, Any]:
    require(not output.exists(), "aggregate receipt already exists; select a fresh output path")
    rows = []
    for target in inputs:
        partial = read_json(target)
        require(partial["schema_version"] == SCHEMA and len(partial["rows"]) == 1, "merge requires one captured row per input")
        row = partial["rows"][0]
        for key in ("executable", "report"):
            descriptor = row[key]
            if not (target.parent / descriptor["path"]).is_file():
                basename = PureWindowsPath(descriptor["path"]).name if row["platform"] == "windows" else Path(descriptor["path"]).name
                descriptor["path"] = str(target.parent / basename)
            descriptor["path"] = str(checked_file(descriptor, target.parent).resolve())
        rows.append(row)
    output.write_text(json.dumps({"schema_version": SCHEMA, "rows": rows}, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return verify_receipt(root, output, cadence_policy)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--candidate-commit")
    parser.add_argument("--record", choices=tuple(TARGET_HZ))
    parser.add_argument("--merge", nargs=2, type=Path, metavar=("MACOS_RECEIPT", "WINDOWS_RECEIPT"))
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--warmup-seconds", type=int, default=180)
    parser.add_argument("--cadence-policy", choices=CADENCE_POLICIES, default="strict",
                        help="snapshot records Mac cadence diagnostically; strict requires both platform targets (default)")
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
        if args.record:
            require(args.executable is not None, "--record requires --executable")
            record(root, args.receipt.resolve(), args.record, args.executable.resolve(strict=True), args.warmup_seconds, args.cadence_policy)
        else:
            results = (merge_receipts(root, [target.resolve(strict=True) for target in args.merge], args.receipt.resolve(), args.cadence_policy)
                       if args.merge else verify_receipt(root, args.receipt.resolve(strict=True), args.cadence_policy))
            print(json.dumps({"status": "passed", "cadence_policy": args.cadence_policy,
                              "platforms": results}, sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError, TypeError, IndexError, subprocess.SubprocessError, _MATRIX.MatrixError) as error:
        print(f"FAIL gameplay gate: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
