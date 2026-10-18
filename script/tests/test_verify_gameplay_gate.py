from __future__ import annotations

import contextlib
import copy
import importlib.util
import io
import json
import math
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[2]
GATE = REPOSITORY / "script/ci/verify_gameplay_gate.py"
CANDIDATE = {"head": "a" * 40, "tree": "b" * 40}


def load_module():
    spec = importlib.util.spec_from_file_location("verify_gameplay_gate", GATE)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class VerifyGameplayGateTests(unittest.TestCase):
    def setUp(self):
        self.gate = load_module()

    def report(self, platform="macos", interval_ns=None, movement_interval_ns=None):
        interval = interval_ns or (8000000 if platform == "macos" else 2000000)
        moving_interval = movement_interval_ns or interval
        bands = [0] * 16
        for x in range(-256, 257):
            limit = math.isqrt(256 * 256 - x * x)
            for y in range(-limit, limit + 1):
                bands[min(max(abs(x), abs(y)) // 16, 15)] += 1
        barrier = {key: 0 for key in (*self.gate.QUEUE_FIELDS, "nonresident_desired", "stale_or_missing_mesh",
                                    "unclassified_resident_missing", "visible_outside_interest", "unclassified_current_visible_mesh_detail")}
        barrier.update({key: 205861 for key in ("desired", "resident_desired", "resident_total", "current_handle_mesh_visible",
                                               "visible_meshes", "renderer_mesh_slots")})
        barrier.update({"scan_performed": True, "potentially_ready": True, "exact_current_coverage": True,
                        "center": {"x": 0, "y": 0}, "current_visible_mesh_detail_counts": {
                            "coarse": 205861, "coarse2": 0, "coarse4": 0, "fine": 0, "distant": 0},
                        "radial_band_tile_width": 16, "radial_bands": [{"desired": count, "resident": count,
                            "current_mesh_visible": count, "nonresident": 0, "stale_or_missing_mesh": 0} for count in bands]})
        frames, offset = [], 0
        full = {"contract": "mc.full-radius-terrain-barriers.v1", "render_distance": 256, "expected_tile_count": 205861,
                "complete": True, "warmup_timeout_ms": 180000, "initial_warmup_elapsed_ms": 1, "recovery_warmup_elapsed_ms": 1,
                "initial_barrier_reached": True, "recovery_barrier_reached": True,
                "movement_elapsed_ms": math.ceil(6000000000 / moving_interval) * moving_interval // 1000000,
                "movement_center_change_count": 2, "movement_start_center": {"x": 0, "y": 0}, "movement_end_center": {"x": 2, "y": 0},
                "loop_timing_scope": "all-stationary-loop-attempts", "successful_present_request_interval_scope":
                "consecutive-successful-request-completions-not-physical-display-cadence"}
        for index, phase in enumerate(self.gate.PHASES):
            durations = ([1000000] if index in (0, 3) else [moving_interval] * math.ceil(6000000000 / moving_interval) if index == 2 else
                         [interval] * math.ceil(10000000000 / interval))
            for number, duration in enumerate(durations):
                terrain_center = {"x": min(number // 4, 2) if index == 2 else 2 if index > 2 else 0, "y": 0}
                terrain_near = {"center": dict(terrain_center), "desired": 225, "resident": 225,
                                "nonresident": 0, "current_mesh_visible": 225}
                frames.append({"phase": phase, "presentation_succeeded": True, "started_offset_ns": offset,
                               "loop_duration_ns": duration, "terrain_center": terrain_center,
                               "terrain_near": terrain_near, "diagnostic_terrain_radial_bands": None,
                               "diagnostic_terrain_spatial_scan_duration_ns": 0})
                offset += duration
            if index in (0, 3):
                prefix = "initial" if index == 0 else "recovery"
                snapshot = copy.deepcopy(barrier)
                snapshot.update({"phase": phase, "phase_elapsed_ms": 1, "center": {"x": 0 if index == 0 else 2, "y": 0}})
                full[prefix + "_barrier"] = snapshot
            if index in (1, 4):
                prefix = "baseline" if index == 1 else "recovered"
                elapsed = sum(durations) // 1000000
                full.update({prefix + "_stationary_elapsed_ms": elapsed, prefix + "_loop_attempts": len(durations),
                             prefix + "_present_requests": len(durations), prefix + "_failed_present_requests": 0,
                             prefix + "_present_requests_per_second": len(durations) * 1000 / elapsed,
                             prefix + "_loop_timings_ns": self.gate.timing_summary(durations),
                             prefix + "_successful_present_request_intervals_ns": self.gate.timing_summary(durations[1:])})
        near_sample_count = sum(frame["phase"] in (self.gate.PHASES[2], self.gate.PHASES[3]) for frame in frames)
        readiness = {"contract": "mc.current-near-terrain-readiness.v1",
                     "measurement_scope": "current-center-radius-7",
                     "current_near_sample_count": near_sample_count,
                     "incomplete_current_near_sample_count": 0,
                     "max_missing_current_mesh_count": 0,
                     "current_near_coverage_complete": True}
        return {"schema_version": "mc.runtime-evidence.v1", "mode": "benchmark-game", "passed": True, "failure": "",
                "timing": {"elapsed_ms": offset // 1000000, "deadline_ms": 450000}, "game_benchmark": {
                    "workload": self.gate.WORKLOAD, "build_mode": "release", "displayed_cadence_hz": None,
                    "present_mode": "immediate" if platform == "windows" else "fifo", "failed_present_requests": 0,
                    "successful_present_requests": len(frames),
                    "server_events_processed": 10, "inputs_sent": 10, "authoritative_player_updates": 10,
                    "terrain_readiness": readiness, "full_radius_terrain": full, "raw_frames": frames, "raw_server_tick_durations_ns": [1000000, 2000000],
                    "server_tick_timings_ns": self.gate.timing_summary([1000000, 2000000])}}

    def aggregate(self, root, mac_interval_ns=None):
        for name in self.gate.REQUIRED_SOURCE_INPUTS:
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_text("fixture\n", encoding="utf-8")
        (root / "src").mkdir()
        (root / "src/game.cpp").write_text("source\n", encoding="utf-8")
        digest, rows = self.gate.source_digest(root), []
        for platform in self.gate.TARGET_HZ:
            directory = root / platform
            directory.mkdir()
            executable = directory / ("mc_main.exe" if platform == "windows" else "mc_main")
            executable.write_bytes(b"fixture executable")
            report = directory / "report.json"
            report.write_text(json.dumps(self.report(platform, mac_interval_ns if platform == "macos" else None)), encoding="utf-8")
            executable_identity = self.gate.file_identity(executable)
            rows.append({"platform": platform, "proof_origin": "local", "source_candidate": CANDIDATE,
                         "source_input_sha256": digest, "build": {"configuration": "Release", "source_input_sha256": digest,
                            "executable_sha256": executable_identity["sha256"], "configuration_sha256": "c" * 64, "dependency_sha256": "d" * 64},
                         "executable": executable_identity, "report": self.gate.file_identity(report), "warmup_seconds": 180,
                         "command": [str(executable), *self.gate.benchmark_arguments(platform, 180, str(report))],
                         "exit_code": 0, "timed_out": False, "environment": {}})
        return {"schema_version": self.gate.SCHEMA, "rows": rows}

    def reconcile_raw_totals(self, report):
        game = report["game_benchmark"]
        frames, full = game["raw_frames"], game["full_radius_terrain"]
        game["successful_present_requests"] = sum(frame["presentation_succeeded"] for frame in frames)
        game["failed_present_requests"] = len(frames) - game["successful_present_requests"]
        for prefix, phase in (("baseline", self.gate.PHASES[1]), ("recovered", self.gate.PHASES[4])):
            selected = [frame for frame in frames if frame["phase"] == phase]
            completions = [frame["started_offset_ns"] + frame["loop_duration_ns"] for frame in selected if frame["presentation_succeeded"]]
            full.update({prefix + "_loop_attempts": len(selected), prefix + "_present_requests": len(completions),
                         prefix + "_failed_present_requests": len(selected) - len(completions),
                         prefix + "_loop_timings_ns": self.gate.timing_summary([frame["loop_duration_ns"] for frame in selected]),
                         prefix + "_successful_present_request_intervals_ns": self.gate.timing_summary(
                            [right - left for left, right in zip(completions, completions[1:])]),
                         prefix + "_present_requests_per_second": len(completions) * 1000 / full[prefix + "_stationary_elapsed_ms"]})

    def reconcile_contiguous_durations(self, report):
        offset, elapsed = 0, {phase: 0 for phase in self.gate.PHASES}
        for frame in report["game_benchmark"]["raw_frames"]:
            frame["started_offset_ns"] = offset
            offset += frame["loop_duration_ns"]
            elapsed[frame["phase"]] += frame["loop_duration_ns"]
        full = report["game_benchmark"]["full_radius_terrain"]
        for phase, key in ((self.gate.PHASES[1], "baseline_stationary_elapsed_ms"),
                           (self.gate.PHASES[2], "movement_elapsed_ms"), (self.gate.PHASES[4], "recovered_stationary_elapsed_ms")):
            full[key] = elapsed[phase] // 1000000
        report["timing"]["elapsed_ms"] = offset // 1000000
        self.reconcile_raw_totals(report)

    def test_complete_mac_and_windows_proxy_proof_passes_without_display_cadence_claim(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            receipt = root / "aggregate.json"
            receipt.write_text(json.dumps(self.aggregate(root)), encoding="utf-8")
            verified = self.gate.verify_receipt(root, receipt)
            self.assertEqual(set(verified), {"macos", "windows"})
            self.assertEqual(verified["macos"]["physical_display_cadence"], "unavailable")
            for platform in verified:
                self.assertEqual(verified[platform]["request_cadence_status"], "passed")
                self.assertTrue(verified[platform]["request_cadence_required"])
                self.assertEqual(verified[platform]["request_cadence_reasons"], [])

    def test_slow_mac_snapshot_retains_failed_cadence_and_strict_rejects_it(self):
        report = self.report(interval_ns=10000000)
        verified = self.gate.verify_report(report, "macos", "snapshot")
        self.assertEqual(verified["coverage"], "passed")
        self.assertEqual(verified["request_cadence_status"], "failed")
        self.assertFalse(verified["request_cadence_required"])
        self.assertEqual(verified["physical_display_cadence"], "unavailable")
        for phase in (self.gate.PHASES[1], self.gate.PHASES[2], self.gate.PHASES[4]):
            self.assertEqual(verified["request_cadence_proxy"][phase]["present_requests_per_second"], 100)
            self.assertTrue(any(reason.startswith(phase + ": successful-request rate") for reason in verified["request_cadence_reasons"]))
        with self.assertRaisesRegex(self.gate.GameplayError, "successful-request rate.*below"):
            self.gate.verify_report(report, "macos", "strict")

    def test_slow_windows_cadence_is_required_under_both_policies(self):
        for policy in self.gate.CADENCE_POLICIES:
            with self.subTest(policy=policy), self.assertRaisesRegex(self.gate.GameplayError, "successful-request rate.*below"):
                self.gate.verify_report(self.report("windows", interval_ns=2500000), "windows", policy)
            verified = self.gate.verify_report(self.report("windows"), "windows", policy)
            self.assertTrue(verified["request_cadence_required"])

    def test_short_mac_movement_cadence_sample_is_diagnostic_only_at_snapshot(self):
        report = self.report(movement_interval_ns=15000000)
        verified = self.gate.verify_report(report, "macos", "snapshot")
        self.assertEqual(verified["request_cadence_status"], "failed")
        self.assertEqual(verified["request_cadence_proxy"][self.gate.PHASES[2]]["intervals_ns"]["sample_count"], 399)
        self.assertIn("speed-200-movement: insufficient cadence samples (399 intervals, requires 500)",
                      verified["request_cadence_reasons"])
        with self.assertRaisesRegex(self.gate.GameplayError, "insufficient cadence samples"):
            self.gate.verify_report(report, "macos", "strict")

    def test_raw_cadence_must_be_computable_under_both_policies(self):
        for policy in self.gate.CADENCE_POLICIES:
            for phase in (self.gate.PHASES[1], self.gate.PHASES[2], self.gate.PHASES[4]):
                for successes in (0, 1):
                    with self.subTest(policy=policy, phase=phase, successes=successes):
                        report = self.report()
                        selected = [frame for frame in report["game_benchmark"]["raw_frames"] if frame["phase"] == phase]
                        for index, frame in enumerate(selected):
                            frame["presentation_succeeded"] = index < successes
                        self.reconcile_raw_totals(report)
                        with self.assertRaisesRegex(self.gate.GameplayError, "not computable from raw completions"):
                            self.gate.verify_report(report, "macos", policy)

    def test_nonfinite_or_incomplete_raw_timing_fails_both_policies(self):
        for policy in self.gate.CADENCE_POLICIES:
            for name in ("nonfinite", "missing phase"):
                with self.subTest(policy=policy, mutation=name):
                    report = self.report()
                    frames = report["game_benchmark"]["raw_frames"]
                    if name == "nonfinite":
                        frames[1]["loop_duration_ns"] = float("inf")
                    else:
                        frames[:] = [frame for frame in frames if frame["phase"] != self.gate.PHASES[2]]
                    with self.assertRaises(self.gate.GameplayError):
                        self.gate.verify_report(report, "macos", policy)

    def test_coverage_completion_does_not_bypass_request_cadence_target(self):
        for platform, interval in (("macos", 10000000), ("windows", 2500000)):
            with self.subTest(platform=platform), self.assertRaisesRegex(self.gate.GameplayError, "successful-request rate.*below"):
                self.gate.verify_report(self.report(platform, interval), platform)

    def test_mac_nanosecond_rounding_at_120hz_is_accepted(self):
        self.gate.verify_report(self.report(interval_ns=8333334), "macos")

    def test_mac_fifo_submillisecond_completion_jitter_is_not_a_missed_refresh_slot(self):
        report = self.report(interval_ns=8333333)
        for phase in (self.gate.PHASES[1], self.gate.PHASES[2], self.gate.PHASES[4]):
            selected = [frame for frame in report["game_benchmark"]["raw_frames"] if frame["phase"] == phase]
            for index, frame in enumerate(selected):
                frame["loop_duration_ns"] += 400000 if index % 2 else -400000
        self.reconcile_contiguous_durations(report)
        verified = self.gate.verify_report(report, "macos")
        for phase in (self.gate.PHASES[1], self.gate.PHASES[2], self.gate.PHASES[4]):
            proxy = verified["request_cadence_proxy"][phase]
            self.assertGreater(proxy["intervals_ns"]["p99_8"], 8333334)
            self.assertEqual(proxy["cadence"]["lost_slots"], 0)

    def test_mac_fifo_sustained_119hz_is_below_the_derived_rate_floor(self):
        with self.assertRaisesRegex(self.gate.GameplayError, "successful-request rate.*below 119.76Hz"):
            self.gate.verify_report(self.report(interval_ns=math.ceil(1000000000 / 119)), "macos")

    def test_mac_fifo_refresh_slot_loss_and_long_freezes_are_bounded(self):
        for durations, accepted in (([16666667], True), ([16666667] * 3, False), ([108000000], False)):
            with self.subTest(durations=durations):
                report = self.report()
                selected = [frame for frame in report["game_benchmark"]["raw_frames"] if frame["phase"] == self.gate.PHASES[1]]
                for frame, duration in zip(selected[600:], durations):
                    frame["loop_duration_ns"] = duration
                self.reconcile_contiguous_durations(report)
                if accepted:
                    proxy = self.gate.verify_report(report, "macos")["request_cadence_proxy"][self.gate.PHASES[1]]
                    self.assertEqual(proxy["cadence"]["lost_slots"], 1)
                    self.assertLessEqual(proxy["cadence"]["lost_slot_share"], 0.002)
                else:
                    with self.assertRaisesRegex(self.gate.GameplayError, "lost FIFO refresh-slot share exceeds 0.2%"):
                        self.gate.verify_report(report, "macos")

    def test_movement_cadence_blocks_release_when_stationary_cadence_passes(self):
        for platform, interval in (("macos", 10000000), ("windows", 2500000)):
            with self.subTest(platform=platform), self.assertRaisesRegex(self.gate.GameplayError, "speed-200-movement: successful-request rate.*below"):
                self.gate.verify_report(self.report(platform, movement_interval_ns=interval), platform)

    def test_skipped_attempt_with_steady_successful_cadence_is_allowed_and_counted(self):
        for phase in (self.gate.PHASES[1], self.gate.PHASES[2], self.gate.PHASES[4]):
            with self.subTest(phase=phase):
                report = self.report()
                frames = report["game_benchmark"]["raw_frames"]
                index = next(index for index, frame in enumerate(frames) if frame["phase"] == phase) + 10
                original = frames[index]
                failed = {**original, "presentation_succeeded": False, "loop_duration_ns": 4000000}
                successful = {**original, "started_offset_ns": original["started_offset_ns"] + 4000000, "loop_duration_ns": 4000000}
                frames[index:index + 1] = [failed, successful]
                self.reconcile_raw_totals(report)
                verified = self.gate.verify_report(report, "macos")
                self.assertEqual(verified["request_cadence_proxy"][phase]["failed_requests"], 1)
                self.assertEqual(verified["total_unsuccessful_attempts"], 1)

    def test_a_long_gap_fails_time_guard_even_when_count_percentile_and_rate_pass(self):
        report = self.report("windows", interval_ns=1800000)
        game, gap = report["game_benchmark"], 100000000
        frames = game["raw_frames"]
        first = next(index for index, frame in enumerate(frames) if frame["phase"] == self.gate.PHASES[1])
        for frame in frames[first + 600:]:
            frame["started_offset_ns"] += gap
        game["full_radius_terrain"]["baseline_stationary_elapsed_ms"] += gap // 1000000
        report["timing"]["elapsed_ms"] += gap // 1000000
        self.reconcile_raw_totals(report)
        full = game["full_radius_terrain"]
        self.assertLessEqual(full["baseline_successful_present_request_intervals_ns"]["p99_8"], 2000000)
        self.assertGreaterEqual(full["baseline_present_requests_per_second"], 500)
        with self.assertRaisesRegex(self.gate.GameplayError, "excess-time share exceeds 0.2%"):
            self.gate.verify_report(report, "windows")

    def test_phase_rate_includes_initial_work_and_a_closing_skipped_attempt(self):
        report = self.report()
        game, gap = report["game_benchmark"], 60000000
        first = next(index for index, frame in enumerate(game["raw_frames"]) if frame["phase"] == self.gate.PHASES[1])
        for frame in game["raw_frames"][first:]:
            frame["started_offset_ns"] += gap
        game["full_radius_terrain"]["baseline_stationary_elapsed_ms"] += gap // 1000000
        report["timing"]["elapsed_ms"] += gap // 1000000
        self.reconcile_raw_totals(report)
        self.gate.verify_report(report, "macos")
        report = self.report()
        game, gap = report["game_benchmark"], 500000000
        frames = game["raw_frames"]
        index = max(index for index, frame in enumerate(frames) if frame["phase"] == self.gate.PHASES[1])
        previous = frames[index]
        frames.insert(index + 1, {**previous, "presentation_succeeded": False,
                                 "started_offset_ns": previous["started_offset_ns"] + previous["loop_duration_ns"], "loop_duration_ns": gap})
        for frame in frames[index + 2:]:
            frame["started_offset_ns"] += gap
        game["full_radius_terrain"]["baseline_stationary_elapsed_ms"] += gap // 1000000
        report["timing"]["elapsed_ms"] += gap // 1000000
        self.reconcile_raw_totals(report)
        self.assertLessEqual(game["full_radius_terrain"]["baseline_successful_present_request_intervals_ns"]["p99_8"], 8333334)
        with self.assertRaisesRegex(self.gate.GameplayError, "successful-request rate.*below"):
            self.gate.verify_report(report, "macos")

    def test_longer_diagnostic_warmup_cannot_qualify_release(self):
        report = self.report()
        report["game_benchmark"]["full_radius_terrain"]["warmup_timeout_ms"] = 1800000
        with self.assertRaisesRegex(self.gate.GameplayError, "180-second"):
            self.gate.verify_report(report, "macos")

    def test_exact_warmup_deadline_is_valid_and_strictly_later_is_rejected(self):
        report = self.report()
        full = report["game_benchmark"]["full_radius_terrain"]
        for prefix, phase in (("initial", self.gate.PHASES[0]), ("recovery", self.gate.PHASES[3])):
            full[prefix + "_warmup_elapsed_ms"] = 180000
            full[prefix + "_barrier"]["phase_elapsed_ms"] = 180000
            next(frame for frame in report["game_benchmark"]["raw_frames"] if frame["phase"] == phase)["loop_duration_ns"] = 180000000000
        self.reconcile_contiguous_durations(report)
        self.gate.verify_report(report, "macos")
        for prefix, phase in (("initial", self.gate.PHASES[0]), ("recovery", self.gate.PHASES[3])):
            with self.subTest(prefix=prefix):
                late = copy.deepcopy(report)
                late_full = late["game_benchmark"]["full_radius_terrain"]
                late_full[prefix + "_warmup_elapsed_ms"] += 1
                late_full[prefix + "_barrier"]["phase_elapsed_ms"] += 1
                next(frame for frame in late["game_benchmark"]["raw_frames"] if frame["phase"] == phase)["loop_duration_ns"] += 1000000
                self.reconcile_contiguous_durations(late)
                with self.assertRaisesRegex(self.gate.GameplayError, prefix + ": warmup failed"):
                    self.gate.verify_report(late, "macos")

    def test_near_only_reduced_radius_and_failed_runtime_are_rejected(self):
        mutations = (("near-only", lambda report: report["game_benchmark"].update(workload="speed-200-movement-v2")),
                     ("radius72", lambda report: report["game_benchmark"]["full_radius_terrain"].update(render_distance=72)),
                     ("failed", lambda report: report.update(passed=False)),
                     ("fatal", lambda report: report.update(failure="fatal renderer failure")),
                     ("debug", lambda report: report["game_benchmark"].update(build_mode="debug")))
        for policy in self.gate.CADENCE_POLICIES:
            for name, mutate in mutations:
                with self.subTest(name=name, policy=policy):
                    report = self.report()
                    mutate(report)
                    with self.assertRaises(self.gate.GameplayError):
                        self.gate.verify_report(report, "macos", policy)

    def test_each_full_radius_barrier_requires_exact_handles_and_drained_queues(self):
        for barrier_name in ("initial_barrier", "recovery_barrier"):
            for key, value in (("current_handle_mesh_visible", 205860), ("scan_performed", False), ("worker_outstanding", 1),
                               ("pending_delivery_batches", 1), ("unclassified_current_visible_mesh_detail", 1)):
                for policy in self.gate.CADENCE_POLICIES:
                    with self.subTest(barrier=barrier_name, field=key, policy=policy):
                        report = self.report()
                        report["game_benchmark"]["full_radius_terrain"][barrier_name][key] = value
                        with self.assertRaises(self.gate.GameplayError):
                            self.gate.verify_report(report, "macos", policy)

    def test_barrier_centers_require_signed_integer_coordinates(self):
        for prefix in ("initial", "recovery"):
            for center in ({}, {"x": True, "y": 0}, {"x": 0.5, "y": 0}, {"x": 0, "y": "0"}):
                with self.subTest(prefix=prefix, center=center):
                    report = self.report()
                    report["game_benchmark"]["full_radius_terrain"][prefix + "_barrier"]["center"] = center
                    with self.assertRaisesRegex(self.gate.GameplayError, "center"):
                        self.gate.verify_report(report, "macos")
        report = self.report()
        full = report["game_benchmark"]["full_radius_terrain"]
        centers = [frame["terrain_center"] for frame in report["game_benchmark"]["raw_frames"]]
        centers += [frame["terrain_near"]["center"] for frame in report["game_benchmark"]["raw_frames"]
                    if frame.get("terrain_near") is not None]
        centers += [full[key] for key in ("movement_start_center", "movement_end_center")]
        centers += [full[prefix + "_barrier"]["center"] for prefix in ("initial", "recovery")]
        for center in centers:
            center.update(x=center["x"] - 100, y=-20)
        self.gate.verify_report(report, "macos")

    def test_barrier_center_matches_raw_warmup_and_stationary_transition(self):
        for prefix, warmup, stationary in (("initial", self.gate.PHASES[0], self.gate.PHASES[1]),
                                          ("recovery", self.gate.PHASES[3], self.gate.PHASES[4])):
            for observation in ("barrier", "warmup", "stationary"):
                with self.subTest(prefix=prefix, observation=observation):
                    report = self.report()
                    frames = report["game_benchmark"]["raw_frames"]
                    if observation == "barrier":
                        center = report["game_benchmark"]["full_radius_terrain"][prefix + "_barrier"]["center"]
                    elif observation == "warmup":
                        center = next(frame["terrain_center"] for frame in reversed(frames) if frame["phase"] == warmup)
                    else:
                        center = next(frame["terrain_center"] for frame in frames if frame["phase"] == stationary)
                    center["x"] = 999
                    with self.assertRaisesRegex(self.gate.GameplayError, "center"):
                        self.gate.verify_report(report, "macos")

    def test_barrier_elapsed_requires_typed_agreement_with_warmup(self):
        for prefix in ("initial", "recovery"):
            for elapsed in (True, 1.0, -1, 180001):
                with self.subTest(prefix=prefix, elapsed=elapsed):
                    report = self.report()
                    report["game_benchmark"]["full_radius_terrain"][prefix + "_barrier"]["phase_elapsed_ms"] = elapsed
                    with self.assertRaisesRegex(self.gate.GameplayError, "barrier elapsed"):
                        self.gate.verify_report(report, "macos")

    def test_overall_runtime_cannot_end_before_the_raw_timeline(self):
        report = self.report()
        report["timing"].update(elapsed_ms=1, deadline_ms=1)
        with self.assertRaisesRegex(self.gate.GameplayError, "raw timeline"):
            self.gate.verify_report(report, "macos")

    def test_raw_attempt_timing_rate_and_stationary_span_are_recomputed(self):
        mutations = (("attempts", lambda full: full.update(baseline_loop_attempts=1)),
                     ("failed requests", lambda full: full.update(baseline_failed_present_requests=1)),
                     ("summary", lambda full: full["baseline_loop_timings_ns"].update(p99_8=1)),
                     ("rate", lambda full: full.update(recovered_present_requests_per_second=1)),
                     ("duration", lambda full: full.update(baseline_stationary_elapsed_ms=9999)),
                     ("timeout", lambda full: full.update(recovery_warmup_elapsed_ms=180001)))
        for policy in self.gate.CADENCE_POLICIES:
            for name, mutate in mutations:
                with self.subTest(name=name, policy=policy):
                    report = self.report()
                    mutate(report["game_benchmark"]["full_radius_terrain"])
                    with self.assertRaises(self.gate.GameplayError):
                        self.gate.verify_report(report, "macos", policy)

    def test_incorrect_unsuccessful_attempt_totals_are_rejected(self):
        report = self.report()
        report["game_benchmark"]["raw_frames"][1]["presentation_succeeded"] = False
        report["game_benchmark"]["failed_present_requests"] = 1
        report["game_benchmark"]["successful_present_requests"] -= 1
        with self.assertRaisesRegex(self.gate.GameplayError, "raw attempt totals mismatch"):
            self.gate.verify_report(report, "macos")

    def test_windows_fifo_cannot_supply_500hz_qualification(self):
        report = self.report("windows")
        report["game_benchmark"]["present_mode"] = "fifo"
        with self.assertRaisesRegex(self.gate.GameplayError, "negotiated present mode"):
            self.gate.verify_report(report, "windows")

    def test_source_changes_invalidate_proof_but_documentation_does_not(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            receipt = root / "aggregate.json"
            receipt.write_text(json.dumps(self.aggregate(root)), encoding="utf-8")
            (root / "notes.md").write_text("new documentation", encoding="utf-8")
            for policy in self.gate.CADENCE_POLICIES:
                self.gate.verify_receipt(root, receipt, policy)
            (root / "src/game.cpp").write_text("changed source", encoding="utf-8")
            for policy in self.gate.CADENCE_POLICIES:
                with self.subTest(policy=policy), self.assertRaisesRegex(self.gate.GameplayError, "stale.*source"):
                    self.gate.verify_receipt(root, receipt, policy)

    def test_missing_platform_changed_raw_bytes_executable_and_build_binding_fail_closed(self):
        for name in ("missing-platform", "raw-bytes", "executable", "build", "command-executable", "command-report",
                     "diagnostic-environment", "nonlocal-origin", "candidate", "dependency"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                aggregate = self.aggregate(root)
                if name == "missing-platform":
                    aggregate["rows"].pop()
                elif name == "raw-bytes":
                    Path(aggregate["rows"][0]["report"]["path"]).write_text("{}", encoding="utf-8")
                elif name == "executable":
                    Path(aggregate["rows"][0]["executable"]["path"]).write_bytes(b"changed executable")
                elif name == "command-executable":
                    aggregate["rows"][0]["command"][0] = "/different/executable"
                elif name == "command-report":
                    aggregate["rows"][0]["command"][-1] = "/different/other-report.json"
                elif name == "diagnostic-environment":
                    aggregate["rows"][0]["environment"] = {"MC_DIAGNOSTIC_DISABLE_STONE": "1"}
                elif name == "nonlocal-origin":
                    aggregate["rows"][0]["proof_origin"] = "hosted"
                elif name == "candidate":
                    aggregate["rows"][0]["source_candidate"] = {"head": "invalid", "tree": "b" * 40}
                elif name == "dependency":
                    aggregate["rows"][0]["build"]["dependency_sha256"] = "invalid"
                else:
                    aggregate["rows"][0]["build"]["executable_sha256"] = "0" * 64
                receipt = root / "aggregate.json"
                receipt.write_text(json.dumps(aggregate), encoding="utf-8")
                for policy in self.gate.CADENCE_POLICIES:
                    with self.subTest(policy=policy), self.assertRaises((self.gate.GameplayError, self.gate._MATRIX.MatrixError)):
                        self.gate.verify_receipt(root, receipt, policy)

    def test_absent_or_corrupt_receipt_returns_failure_without_native_run(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for contents in (None, "{invalid", "{\"schema_version\":NaN}"):
                receipt = root / "receipt.json"
                if contents is not None:
                    receipt.write_text(contents, encoding="utf-8")
                with mock.patch.object(self.gate.subprocess, "Popen", side_effect=AssertionError("native run forbidden")), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(self.gate.main(["--root", str(root), "--receipt", str(receipt)]), 1)

    def test_release_registry_executes_gameplay_proof_without_receipt_reuse(self):
        checks = json.loads((REPOSITORY / "script/ai_checks.json").read_text(encoding="utf-8"))
        gameplay_checks = [item for item in checks if "script/ci/verify_gameplay_gate.py" in item["command"]]
        self.assertEqual(len(gameplay_checks), 2)
        for level in ("snapshot", "minor", "major"):
            with self.subTest(level=level):
                selected = [item for item in gameplay_checks if item["enabled"] and level in item["levels"]]
                self.assertEqual(len(selected), 1)
                check = selected[0]
                self.assertTrue(check["proof"])
                self.assertFalse(check["reuse"])
                self.assertEqual(check["when"], "release")
                self.assertEqual(check["command"][-2:], ["--candidate-commit", "{proof-candidate-commit}"])
                if level == "snapshot":
                    index = check["command"].index("--cadence-policy")
                    self.assertEqual(check["command"][index + 1], "snapshot")
                else:
                    self.assertNotIn("--cadence-policy", check["command"])

    def test_snapshot_receipt_cannot_select_policy_or_bypass_strict_verification(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            aggregate = self.aggregate(root, mac_interval_ns=10000000)
            aggregate["cadence_policy"] = "snapshot"
            aggregate["rows"][0]["request_cadence_status"] = "passed"
            aggregate["rows"][0]["request_cadence_required"] = False
            receipt = root / "aggregate.json"
            receipt.write_text(json.dumps(aggregate), encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(self.gate.main(["--root", str(root), "--receipt", str(receipt), "--cadence-policy", "snapshot"]), 0)
            verified = json.loads(output.getvalue())
            self.assertEqual(verified["status"], "passed")
            self.assertEqual(verified["platforms"]["macos"]["request_cadence_status"], "failed")
            for policy_arguments in ([], ["--cadence-policy", "strict"]):
                with self.subTest(arguments=policy_arguments), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(self.gate.main(["--root", str(root), "--receipt", str(receipt), *policy_arguments]), 1)

    def test_candidate_commit_checks_relevant_tracked_and_untracked_source_without_native_run(self):
        for name, diff_result, untracked in (("same inputs", 0, ""), ("tracked change", 1, ""),
                                              ("uncommitted source", 0, "src/new.cpp\n")):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                receipt = root / "aggregate.json"
                receipt.write_text(json.dumps(self.aggregate(root)), encoding="utf-8")
                with mock.patch.object(self.gate._MATRIX, "commit_identity", return_value=CANDIDATE) as candidate, \
                        mock.patch.object(self.gate.subprocess, "run", side_effect=[mock.Mock(returncode=diff_result),
                            mock.Mock(returncode=0, stdout=untracked)]) as run, \
                        contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    result = self.gate.main(["--root", str(root), "--receipt", str(receipt), "--candidate-commit", CANDIDATE["head"]])
                self.assertEqual(result, 0 if name == "same inputs" else 1)
                candidate.assert_called_once_with(root.resolve(), CANDIDATE["head"])
                self.assertEqual(run.call_args_list[0].args[0][0:4], ["git", "diff", "--quiet", CANDIDATE["head"]])

    def test_capture_binds_before_and_after_inputs_and_preserves_raw_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            row = self.aggregate(root)["rows"][0]
            receipt = root / "captured.json"

            def launch(command, cwd):
                self.assertEqual(cwd, root)
                Path(command[-1]).write_text(json.dumps(self.report()), encoding="utf-8")
                process = mock.Mock(returncode=0)
                process.poll.return_value = 0
                return process

            with mock.patch.object(self.gate.sys, "platform", "darwin"), mock.patch.object(self.gate.os, "environ", {}), \
                    mock.patch.object(self.gate, "build_identity", return_value=row["build"]) as identity, \
                    mock.patch.object(self.gate._MATRIX, "candidate_identity", return_value=CANDIDATE), \
                    mock.patch.object(self.gate.subprocess, "Popen", side_effect=launch), contextlib.redirect_stdout(io.StringIO()):
                self.gate.record(root, receipt, "macos", Path(row["executable"]["path"]), 180)
            captured = json.loads(receipt.read_text(encoding="utf-8"))["rows"][0]
            self.assertEqual(identity.call_count, 2)
            self.assertEqual(captured["build"], row["build"])
            self.assertEqual(captured["report"], self.gate.file_identity(receipt.with_suffix(".raw.json")))

    def test_capture_merge_and_verify_apply_the_same_invocation_policy(self):
        for policy in self.gate.CADENCE_POLICIES:
            with self.subTest(policy=policy), tempfile.TemporaryDirectory() as directory:
                root = Path(directory).resolve()
                aggregate = self.aggregate(root)
                mac_row, windows_row = aggregate["rows"]
                mac_receipt, windows_receipt, merged = root / "macos.json", root / "windows.json", root / "merged.json"
                windows_receipt.write_text(json.dumps({"schema_version": self.gate.SCHEMA, "rows": [windows_row]}), encoding="utf-8")

                def launch(command, cwd):
                    Path(command[-1]).write_text(json.dumps(self.report(interval_ns=10000000)), encoding="utf-8")
                    process = mock.Mock(returncode=0)
                    process.poll.return_value = 0
                    return process

                expected = 0 if policy == "snapshot" else 1
                common = ["--root", str(root), "--cadence-policy", policy]
                with mock.patch.object(self.gate.sys, "platform", "darwin"), mock.patch.object(self.gate.os, "environ", {}), \
                        mock.patch.object(self.gate, "build_identity", return_value=mac_row["build"]), \
                        mock.patch.object(self.gate._MATRIX, "candidate_identity", return_value=CANDIDATE), \
                        mock.patch.object(self.gate.subprocess, "Popen", side_effect=launch), \
                        contextlib.redirect_stdout(io.StringIO()) as capture_output, contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(self.gate.main([*common, "--record", "macos", "--receipt", str(mac_receipt),
                                                     "--executable", mac_row["executable"]["path"]]), expected)
                if policy == "snapshot":
                    self.assertEqual(json.loads(capture_output.getvalue().splitlines()[-1])["request_cadence_status"], "failed")
                captured = json.loads(mac_receipt.read_text(encoding="utf-8"))
                self.assertNotIn("cadence_policy", captured)
                self.assertNotIn("cadence_policy", captured["rows"][0])
                self.assertEqual(captured["rows"][0]["command"][1:], self.gate.benchmark_arguments("macos", 180, str(mac_receipt.with_suffix(".raw.json"))))
                for operation in (["--merge", str(mac_receipt), str(windows_receipt)], []):
                    with contextlib.redirect_stdout(io.StringIO()) as output, contextlib.redirect_stderr(io.StringIO()):
                        self.assertEqual(self.gate.main([*common, *operation, "--receipt", str(merged)]), expected)
                    if policy == "snapshot":
                        result = json.loads(output.getvalue())
                        self.assertEqual(result["platforms"]["macos"]["request_cadence_status"], "failed")
                        self.assertFalse(result["platforms"]["macos"]["request_cadence_required"])
                        self.assertTrue(result["platforms"]["macos"]["request_cadence_reasons"])
                self.assertNotIn("cadence_policy", json.loads(merged.read_text(encoding="utf-8")))

    def test_capture_rejects_mutation_and_preserves_failed_report(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            row = self.aggregate(root)["rows"][0]
            receipt = root / "captured.json"

            def launch(command, cwd):
                Path(command[-1]).write_text(json.dumps(self.report()), encoding="utf-8")
                process = mock.Mock(returncode=0)
                process.poll.return_value = 0
                return process

            changed = {**row["build"], "dependency_sha256": "f" * 64}
            with mock.patch.object(self.gate.sys, "platform", "darwin"), mock.patch.object(self.gate.os, "environ", {}), \
                    mock.patch.object(self.gate, "build_identity", side_effect=[row["build"], changed]), \
                    mock.patch.object(self.gate.subprocess, "Popen", side_effect=launch), contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(self.gate.GameplayError, "changed during capture"):
                    self.gate.record(root, receipt, "macos", Path(row["executable"]["path"]), 180)
            self.assertTrue(receipt.with_suffix(".raw.json").is_file())
            self.assertFalse(receipt.exists())

    def test_build_capture_requires_current_release_ninja_graph_and_binds_dependencies(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            row = self.aggregate(root)["rows"][0]
            executable = Path(row["executable"]["path"])
            build_root = executable.parent
            (build_root / "CMakeFiles").mkdir()
            for name in ("build.ninja", "CMakeFiles/rules.ninja", "compile_commands.json"):
                (build_root / name).write_text("fixture", encoding="utf-8")
            (build_root / "CMakeCache.txt").write_text(
                f"CMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_BUILD_TYPE:STRING=Release\nCMAKE_HOME_DIRECTORY:INTERNAL={root}\n"
                "MC_ENABLE_SANITIZERS:BOOL=OFF\nMC_ALLOW_INEXACT_CORECPP:BOOL=OFF\nCMAKE_MAKE_PROGRAM:FILEPATH=/tools/ninja\n"
                "Z_VCPKG_ROOT_DIR:INTERNAL=/dependencies/vcpkg\nVCPKG_INSTALLED_DIR:PATH=/dependencies/installed\n"
                "CMAKE_PREFIX_PATH:STRING=/dependencies/private\n", encoding="utf-8")
            with mock.patch.object(self.gate.subprocess, "run", return_value=mock.Mock(returncode=0, stdout="ninja: no work to do.\n")) as run, \
                    mock.patch.object(self.gate._MATRIX, "_external_dependency_identity", return_value={"consumed_files_sha256": "e" * 64}) as dependencies:
                identity = self.gate.build_identity(root, build_root, executable)
                run.assert_called_once_with(["/tools/ninja", "-n", "-v", "mc_main"], cwd=build_root, capture_output=True, text=True, timeout=60)
                self.assertEqual(identity["executable_sha256"], row["executable"]["sha256"])
                self.assertEqual(identity["source_input_sha256"], row["source_input_sha256"])
                self.assertEqual(dependencies.call_count, 1)
            with mock.patch.object(self.gate.subprocess, "run", return_value=mock.Mock(returncode=0, stdout="[1/2] clang++ -c game.cpp\n")), \
                    mock.patch.object(self.gate._MATRIX, "_external_dependency_identity", side_effect=AssertionError("stale build cannot qualify")):
                with self.assertRaisesRegex(self.gate.GameplayError, "build is stale"):
                    self.gate.build_identity(root, build_root, executable)

    def test_failed_capture_keeps_failure_receipt_and_raw_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            row = self.aggregate(root)["rows"][0]
            receipt = root / "failed.json"

            def launch(command, cwd):
                raw = self.report()
                raw["passed"] = False
                raw["failure"] = "terrain warmup timeout"
                Path(command[-1]).write_text(json.dumps(raw), encoding="utf-8")
                process = mock.Mock(returncode=1)
                process.poll.return_value = 1
                return process

            with mock.patch.object(self.gate.sys, "platform", "darwin"), mock.patch.object(self.gate.os, "environ", {}), \
                    mock.patch.object(self.gate, "build_identity", return_value=row["build"]), \
                    mock.patch.object(self.gate._MATRIX, "candidate_identity", return_value=CANDIDATE), \
                    mock.patch.object(self.gate.subprocess, "Popen", side_effect=launch), contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt"):
                    self.gate.record(root, receipt, "macos", Path(row["executable"]["path"]), 180)
            captured = json.loads(receipt.read_text(encoding="utf-8"))["rows"][0]
            self.assertEqual(captured["exit_code"], 1)
            self.assertEqual(json.loads(receipt.with_suffix(".raw.json").read_text(encoding="utf-8"))["failure"], "terrain warmup timeout")

    def test_early_exit_without_report_keeps_an_explicit_failure_receipt(self):
        for exit_code in (0, 1):
            with self.subTest(exit_code=exit_code), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                row = self.aggregate(root)["rows"][0]
                receipt = root / "failed.json"
                process = mock.Mock(returncode=exit_code)
                process.poll.return_value = exit_code
                with mock.patch.object(self.gate.sys, "platform", "darwin"), mock.patch.object(self.gate.os, "environ", {}), \
                        mock.patch.object(self.gate, "build_identity", return_value=row["build"]), \
                        mock.patch.object(self.gate._MATRIX, "candidate_identity", return_value=CANDIDATE), \
                        mock.patch.object(self.gate.subprocess, "Popen", return_value=process), contextlib.redirect_stdout(io.StringIO()):
                    with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt.*report is absent"):
                        self.gate.record(root, receipt, "macos", Path(row["executable"]["path"]), 180)
                captured = json.loads(receipt.read_text(encoding="utf-8"))["rows"][0]
                self.assertEqual(captured["exit_code"], exit_code)
                self.assertFalse(captured["timed_out"])
                self.assertIsNone(captured["report"])
                self.assertEqual(captured["capture_error"], "native report is absent")
                self.assertEqual(captured["build"], row["build"])
                self.assertEqual(captured["command"][-1], str(receipt.with_suffix(".raw.json")))

    def test_timeout_without_report_keeps_an_explicit_failure_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            row = self.aggregate(root)["rows"][0]
            receipt = root / "timeout.json"
            process = mock.Mock(returncode=-9)
            process.poll.side_effect = [None, -9]
            process.wait.side_effect = [self.gate.subprocess.TimeoutExpired("mc_main", 30), None]
            with mock.patch.object(self.gate.sys, "platform", "darwin"), mock.patch.object(self.gate.os, "environ", {}), \
                    mock.patch.object(self.gate, "build_identity", return_value=row["build"]), \
                    mock.patch.object(self.gate._MATRIX, "candidate_identity", return_value=CANDIDATE), \
                    mock.patch.object(self.gate.subprocess, "Popen", return_value=process), \
                    mock.patch.object(self.gate.time, "monotonic", side_effect=[0, 451, 451]), contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt.*report is absent"):
                    self.gate.record(root, receipt, "macos", Path(row["executable"]["path"]), 180)
            captured = json.loads(receipt.read_text(encoding="utf-8"))["rows"][0]
            self.assertTrue(captured["timed_out"])
            self.assertEqual(captured["exit_code"], -9)
            self.assertIsNone(captured["report"])
            self.assertEqual(captured["capture_error"], "native report is absent")
            process.kill.assert_called_once_with()

    def test_merge_resolves_transported_windows_bytes_and_keeps_command_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            aggregate = self.aggregate(root)
            inputs = []
            for row in aggregate["rows"]:
                if row["platform"] == "windows":
                    row["executable"]["path"] = r"C:\capture\mc_main.exe"
                    row["report"]["path"] = r"C:\capture\report.json"
                partial = root / row["platform"] / "capture.json"
                partial.write_text(json.dumps({"schema_version": self.gate.SCHEMA, "rows": [row]}), encoding="utf-8")
                inputs.append(partial)
            output = root / "aggregate.json"
            self.gate.merge_receipts(root, inputs, output)
            merged = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(merged["rows"][1]["command"], aggregate["rows"][1]["command"])
            self.assertEqual(Path(merged["rows"][1]["executable"]["path"]), (root / "windows/mc_main.exe").resolve())


    def test_source_digest_tracks_test_and_build_time_scenario_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("CMakeLists.txt", "CMakePresets.json", "dependencies.lock.json"):
                (root / name).write_text("fixture\n", encoding="utf-8")
            source = root / "src"
            tests = root / "tests"
            scenarios = root / "scenarios/world"
            source.mkdir()
            tests.mkdir()
            scenarios.mkdir(parents=True)
            (source / "game.cpp").write_text("production\n", encoding="utf-8")
            test_source = tests / "game_tests.cpp"
            test_source.write_text("test version one\n", encoding="utf-8")
            scenario_script = scenarios / "canonical_world.core"
            scenario_script.write_text("world version one\n", encoding="utf-8")
            (scenarios / "height_tile.core").write_text("height version one\n", encoding="utf-8")

            first_digest = self.gate.source_digest(root)
            test_source.write_text("test version two\n", encoding="utf-8")
            after_test_change = self.gate.source_digest(root)
            scenario_script.write_text("world version two\n", encoding="utf-8")

            self.assertNotEqual(after_test_change, first_digest)
            self.assertNotEqual(self.gate.source_digest(root), after_test_change)

    def test_source_digest_rejects_missing_embedded_scenario_script(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("CMakeLists.txt", "CMakePresets.json", "dependencies.lock.json"):
                (root / name).write_text("fixture\n", encoding="utf-8")
            scenario_directory = root / "scenarios/world"
            scenario_directory.mkdir(parents=True)
            (scenario_directory / "canonical_world.core").write_text("world\n", encoding="utf-8")

            with self.assertRaisesRegex(self.gate.GameplayError, "source input inventory is incomplete"):
                self.gate.source_digest(root)

    def test_endpoint_full_radius_does_not_hide_missing_meshes_during_travel(self):
        report = self.report()
        game = report["game_benchmark"]
        movement_frame = next(frame for frame in game["raw_frames"] if frame["phase"] == self.gate.PHASES[2])
        movement_frame["terrain_near"].update(resident=37, nonresident=188, current_mesh_visible=37)
        game["terrain_readiness"].update(incomplete_current_near_sample_count=1,
                                         max_missing_current_mesh_count=188, current_near_coverage_complete=False)

        with self.assertRaisesRegex(self.gate.GameplayError, "missing meshes in the current near terrain window"):
            self.gate.verify_report(report, "macos")

    def test_current_near_samples_must_match_the_movement_frame(self):
        report = self.report()
        movement_frame = next(frame for frame in report["game_benchmark"]["raw_frames"]
                              if frame["phase"] == self.gate.PHASES[2])
        movement_frame["terrain_near"]["center"]["x"] += 1

        with self.assertRaisesRegex(self.gate.GameplayError, "center disagrees"):
            self.gate.verify_report(report, "macos")

    def test_current_near_visible_tiles_cannot_contradict_residency(self):
        report = self.report()
        frame = next(frame for frame in report["game_benchmark"]["raw_frames"]
                     if frame["phase"] == self.gate.PHASES[2])
        frame["terrain_near"]["resident"] = 0

        with self.assertRaisesRegex(self.gate.GameplayError, "missing meshes in the current near terrain window"):
            self.gate.verify_report(report, "macos")

    def test_all_zero_radial_diagnostics_cannot_claim_complete_coverage(self):
        report = self.report()
        frame = report["game_benchmark"]["raw_frames"][0]
        frame["diagnostic_terrain_radial_bands"] = [
            {"desired": 0, "resident": 0, "nonresident": 0,
             "current_mesh_visible": 0, "stale_or_missing_mesh": 0} for _ in range(16)]
        frame["diagnostic_terrain_spatial_scan_duration_ns"] = 2_345_000

        with self.assertRaisesRegex(self.gate.GameplayError, "radial desired counts disagree with render radius"):
            self.gate.verify_report(report, "macos")

    def test_consistent_underreported_radial_diagnostics_are_rejected(self):
        report = self.report()
        frame = report["game_benchmark"]["raw_frames"][0]
        bands = copy.deepcopy(report["game_benchmark"]["full_radius_terrain"]["initial_barrier"]["radial_bands"])
        for key in ("desired", "resident", "current_mesh_visible"):
            bands[4][key] -= 1
        frame["diagnostic_terrain_radial_bands"] = bands
        frame["diagnostic_terrain_spatial_scan_duration_ns"] = 2_345_000

        with self.assertRaisesRegex(self.gate.GameplayError, "radial desired counts disagree with render radius"):
            self.gate.verify_report(report, "macos")

    def test_radial_cardinalities_follow_the_verified_radius(self):
        for radius, total, last_occupied_band in ((128, 51433, 8), (256, 205861, 15)):
            with self.subTest(radius=radius):
                counts = self.gate.radial_band_counts(radius)
                self.assertEqual(sum(counts), total)
                self.assertEqual(counts[0], 961)
                self.assertGreater(counts[last_occupied_band], 0)
                self.assertEqual(sum(counts[last_occupied_band + 1:]), 0)

    def test_included_radial_diagnostics_must_show_complete_render_radius(self):
        report = self.report()
        frame = next(frame for frame in report["game_benchmark"]["raw_frames"]
                     if frame["phase"] == self.gate.PHASES[2])
        bands = copy.deepcopy(report["game_benchmark"]["full_radius_terrain"]["initial_barrier"]["radial_bands"])
        bands[4].update(resident=3, nonresident=bands[4]["desired"] - 3,
                        current_mesh_visible=3, stale_or_missing_mesh=0)
        frame["diagnostic_terrain_radial_bands"] = bands
        frame["diagnostic_terrain_spatial_scan_duration_ns"] = 2_345_000

        with self.assertRaisesRegex(self.gate.GameplayError, "found missing terrain inside the render radius"):
            self.gate.verify_report(report, "macos")

    def test_recovery_barrier_must_be_on_the_travel_destination_side_of_the_origin(self):
        report = self.report()
        full = report["game_benchmark"]["full_radius_terrain"]
        full["recovery_barrier"]["center"] = {"x": 0, "y": 0}
        for frame in report["game_benchmark"]["raw_frames"]:
            if frame["phase"] in (self.gate.PHASES[3], self.gate.PHASES[4]):
                frame["terrain_center"] = {"x": 0, "y": 0}

        with self.assertRaisesRegex(self.gate.GameplayError, "recovery.*destination"):
            self.gate.verify_report(report, "macos")

    def test_terrain_spatial_radial_scan_retains_missing_ring_counts(self):
        bands = [{"desired": 1, "resident": 1, "nonresident": 0,
                  "current_mesh_visible": 1, "stale_or_missing_mesh": 0} for _ in range(16)]
        bands[4] = {"desired": 20, "resident": 3, "nonresident": 17,
                    "current_mesh_visible": 3, "stale_or_missing_mesh": 0}
        self.gate.verify_terrain_radial_scan(bands, 2_345_000)

        bands[4]["resident"] = 4
        with self.assertRaisesRegex(self.gate.GameplayError, "do not reconcile"):
            self.gate.verify_terrain_radial_scan(bands, 2_345_000)

if __name__ == "__main__":
    unittest.main()
