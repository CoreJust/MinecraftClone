from __future__ import annotations

import base64
import contextlib
import copy
import datetime
import importlib.util
import io
import itertools
import json
import math
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[2]
GATE = REPOSITORY / "script/ci/verify_gameplay_gate.py"
CANDIDATE = {"head": "a" * 40, "tree": "b" * 40}
DESKTOP_JOB_ID = "12345678-1234-1234-1234-123456789abc"
DESKTOP_TASK_ID = "mc-s7-0456-runtime"


def load_module():
    spec = importlib.util.spec_from_file_location("verify_gameplay_gate", GATE)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class VerifyGameplayGateTests(unittest.TestCase):
    def setUp(self):
        self.gate = load_module()
        inspector = mock.patch.object(self.gate._PACKAGE, "validate_macos_package", return_value=[])
        inspector.start()
        self.addCleanup(inspector.stop)

    def test_qualification_manifest_decodes_adjacent_cpp_literals_exactly(self):
        expected = copy.deepcopy(self.gate.qualification_settings_manifest())
        expected["decoder_probe"] = "escaped quote: \"; slash: \\; newline: \n"
        encoded = json.dumps(expected, sort_keys=True)
        literals = "\n".join(json.dumps(encoded[index:index + 37])
                             for index in range(0, len(encoded), 37))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            header = root / "src/acceptance/include/acceptance/GameBenchmark.hpp"
            header.parent.mkdir(parents=True)
            header.write_text("inline constexpr auto GAME_BENCHMARK_S1_QUALIFICATION_MANIFEST =\n"
                              + literals + ";\n", encoding="utf-8")
            with mock.patch.object(self.gate, "SCRIPT_ROOT", root / "script"):
                self.gate.qualification_settings_manifest.cache_clear()
                self.assertEqual(self.gate.qualification_settings_manifest(), expected)

    def test_qualification_manifest_rejects_nonliteral_or_unsupported_initializers(self):
        initializers = ('buildManifest()', '"{}" + runtime_value', 'std::string{"{}"}',
                        'R"json({})json"', r'"\q"')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            header = root / "src/acceptance/include/acceptance/GameBenchmark.hpp"
            header.parent.mkdir(parents=True)
            with mock.patch.object(self.gate, "SCRIPT_ROOT", root / "script"):
                for initializer in initializers:
                    with self.subTest(initializer=initializer):
                        header.write_text("inline constexpr auto GAME_BENCHMARK_S1_QUALIFICATION_MANIFEST = "
                                          + initializer + ";\n", encoding="utf-8")
                        self.gate.qualification_settings_manifest.cache_clear()
                        with self.assertRaisesRegex(self.gate.GameplayError, "source qualification settings manifest"):
                            self.gate.qualification_settings_manifest()

    def macos_package(self, root, executable):
        package = root.resolve() / (executable.parent.name + "-package")
        if not package.exists():
            files = {"mc_main": executable.read_bytes(), "MinecraftClone.command": self.gate._PACKAGE.macos_launcher(),
                     "lib/libvulkan.1.dylib": b"bundled Vulkan loader", "lib/libMoltenVK.dylib": b"bundled MoltenVK",
                     "vulkan/icd.d/MoltenVK_icd.json": self.gate._PACKAGE.canonical_json({"ICD": {"library_path": "../../lib/libMoltenVK.dylib"}})}
            for relative, data in files.items():
                target = package / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
                target.chmod(0o755)
            manifest = {"schema": 1, "platform": "macos", "runtime": {"launcher": "MinecraftClone.command",
                        "vulkan_loader": "lib/libvulkan.1.dylib", "moltenvk": "lib/libMoltenVK.dylib", "icd": "vulkan/icd.d/MoltenVK_icd.json"},
                        "files": [{"path": relative, "bytes": len(data), "sha256": self.gate.hashlib.sha256(data).hexdigest()}
                                  for relative, data in files.items()]}
            (package / "manifest.json").write_bytes(self.gate._PACKAGE.canonical_json(manifest))
        return package, package / "manifest.json"

    def record_macos(self, root, receipt, executable):
        package, manifest = self.macos_package(root, executable)
        return self.gate.record(root, receipt, "macos", executable, 180, macos_package=package, macos_manifest=manifest)

    def macos_runtime(self, package, manifest):
        identity = self.gate.macos_package_identity(package, manifest)
        return {"kind": "normal-macos-package-v1", "capture_package": str(package),
                "original_before": identity, "original_after": identity,
                "retained_before": identity, "retained_after": identity,
                "manifest": self.gate.file_identity(manifest),
                "vulkan_loader": self.gate.file_identity(package / "lib/libvulkan.1.dylib"),
                "moltenvk": self.gate.file_identity(package / "lib/libMoltenVK.dylib"),
                "loaded_libraries": "not-observed; closure and normal launcher verified, PID mappings unavailable"}

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

    def test_macos_package_binds_normal_launcher_bundled_libraries_and_source_executable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            executable = root / "mc_main"
            executable.write_bytes(b"source Release executable")
            package, manifest = self.macos_package(root, executable)
            closure = self.gate.macos_package_identity(package, manifest, executable, inspect_binaries=True)
            self.assertEqual({entry["relative"] for entry in closure}, {"mc_main", "MinecraftClone.command", "manifest.json",
                             "lib/libvulkan.1.dylib", "lib/libMoltenVK.dylib", "vulkan/icd.d/MoltenVK_icd.json"})
            binaries = self.gate._PACKAGE.validate_macos_package.call_args.args[0]
            self.assertEqual({str(location) for _, location in binaries}, {"mc_main", "lib/libvulkan.1.dylib", "lib/libMoltenVK.dylib"})

    def test_macos_package_rejects_incomplete_changed_escaping_and_linked_runtime(self):
        for mutation in ("missing-executable", "changed-executable", "library", "launcher", "icd", "unlisted", "escape", "symlink", "directory-link", "sidecar"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as directory:
                root = Path(directory).resolve()
                executable = root / "mc_main"
                executable.write_bytes(b"source Release executable")
                package, manifest = self.macos_package(root, executable)
                if mutation == "missing-executable":
                    (package / "mc_main").unlink()
                elif mutation == "changed-executable":
                    executable.write_bytes(b"different source executable")
                elif mutation == "library":
                    (package / "lib/libvulkan.1.dylib").write_bytes(b"global old loader")
                elif mutation in ("launcher", "icd"):
                    relative = "MinecraftClone.command" if mutation == "launcher" else "vulkan/icd.d/MoltenVK_icd.json"
                    data = b"#!/bin/sh\nexec /arbitrary/wrapper\n" if mutation == "launcher" else b'{"ICD":{"library_path":"/global/libMoltenVK.dylib"}}'
                    (package / relative).write_bytes(data)
                    value = self.gate.read_json(manifest)
                    entry = next(entry for entry in value["files"] if entry["path"] == relative)
                    entry.update(bytes=len(data), sha256=self.gate.hashlib.sha256(data).hexdigest())
                    manifest.write_bytes(self.gate._PACKAGE.canonical_json(value))
                elif mutation == "unlisted":
                    (package / "extra.dylib").write_bytes(b"unlisted")
                elif mutation == "escape":
                    value = self.gate.read_json(manifest)
                    value["files"][0]["path"] = "../mc_main"
                    manifest.write_bytes(self.gate._PACKAGE.canonical_json(value))
                elif mutation == "symlink":
                    (package / "mc_main").unlink()
                    (package / "mc_main").symlink_to(executable)
                elif mutation == "directory-link":
                    (package / "lib").rename(root / "external-libraries")
                    (package / "lib").symlink_to(root / "external-libraries", target_is_directory=True)
                else:
                    sidecar = root / "different.manifest.json"
                    sidecar.write_text("{}", encoding="utf-8")
                    manifest = sidecar
                with self.assertRaises((self.gate.GameplayError, OSError)):
                    self.gate.macos_package_identity(package, manifest, executable)

    def test_macos_capture_retains_failure_on_original_or_launched_library_drift(self):
        for mutated_package in ("original", "launched"):
            with self.subTest(package=mutated_package), tempfile.TemporaryDirectory() as directory:
                root = Path(directory).resolve()
                row = self.aggregate(root)["rows"][0]
                executable = Path(row["executable"]["path"])
                package, manifest = self.macos_package(root, executable)
                receipt = root / "captured.json"

                def launch(command, cwd):
                    Path(command[-1]).write_text(json.dumps(self.report()), encoding="utf-8")
                    runtime = package if mutated_package == "original" else Path(command[0]).parent
                    (runtime / "lib/libMoltenVK.dylib").write_bytes(b"changed during capture")
                    process = mock.Mock(returncode=0)
                    process.poll.return_value = 0
                    return process

                with mock.patch.object(self.gate.sys, "platform", "darwin"), mock.patch.object(self.gate.os, "environ", {}), \
                        mock.patch.object(self.gate, "build_identity", return_value=row["build"]), \
                        mock.patch.object(self.gate._MATRIX, "candidate_identity", return_value=CANDIDATE), \
                        mock.patch.object(self.gate.subprocess, "Popen", side_effect=launch), contextlib.redirect_stdout(io.StringIO()):
                    with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt"):
                        self.gate.record(root, receipt, "macos", executable, 180, macos_package=package, macos_manifest=manifest)
                captured = self.gate.read_json(receipt)["rows"][0]
                self.assertIn("macOS package input changed", captured["capture_error"])
                self.assertTrue(receipt.with_suffix(".raw.json").is_file())
                self.assertTrue(receipt.with_suffix(".macos").is_dir())

    def test_merge_relocates_retained_macos_runtime_without_rewriting_capture_command(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            aggregate = self.aggregate(root)
            moved = root / "transported"
            moved.mkdir()
            inputs = []
            for index, row in enumerate(aggregate["rows"]):
                if row["platform"] == "macos":
                    package = Path(row["macos_runtime"]["capture_package"])
                    row["executable"] = self.gate.file_identity(package / "mc_main")
                    shutil.copytree(package, moved / package.name)
                    shutil.rmtree(package)
                receipt = moved / f"proof-{index}.json"
                receipt.write_text(json.dumps({"schema_version": self.gate.SCHEMA, "rows": [row]}), encoding="utf-8")
                inputs.append(receipt)
            result = self.gate.merge_receipts(root, inputs, moved / "aggregate.json")
            self.assertEqual(set(result), {"macos", "windows"})

    def test_direct_windows_capture_keeps_its_original_executable_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            row = self.aggregate(root)["rows"][1]
            executable = Path(row["executable"]["path"])
            receipt = root / "windows-direct.json"

            def launch(command, cwd):
                self.assertEqual(command[0], str(executable))
                Path(command[-1]).write_text(json.dumps(self.report("windows")), encoding="utf-8")
                process = mock.Mock(returncode=0)
                process.poll.return_value = 0
                return process

            with mock.patch.object(self.gate.sys, "platform", "win32"), mock.patch.object(self.gate.os, "environ", {}), \
                    mock.patch.object(self.gate, "build_identity", return_value=row["build"]), \
                    mock.patch.object(self.gate._MATRIX, "candidate_identity", return_value=CANDIDATE), \
                    mock.patch.object(self.gate.subprocess, "Popen", side_effect=launch), contextlib.redirect_stdout(io.StringIO()):
                self.gate.record(root, receipt, "windows", executable, 180)
            captured = self.gate.read_json(receipt)["rows"][0]
            self.assertNotIn("macos_runtime", captured)
            self.assertEqual(captured["actual_command"][0], str(executable))

    def test_each_proof_has_fixed_radius_and_coverage_cannot_substitute_for_performance(self):
        self.assertEqual(self.gate.PROOFS["performance"], 256)
        self.assertEqual(
            self.gate.benchmark_arguments("macos", 180, "report.json", "performance"),
            ["--benchmark-game", "--workload", "ordinary-turning", "--gameplay-proof", "performance",
             "--render-distance", "256", "--evidence", "report.json"],
        )
        for platform in self.gate.TARGET_HZ:
            for proof, radius in self.gate.PROOFS.items():
                with self.subTest(platform=platform, proof=proof):
                    report = self.report(platform, interval_ns=10000000, proof=proof)
                    if proof == "terrain-coverage":
                        result = self.gate.verify_report(report, platform)
                        self.assertEqual(result["render_distance"], radius)
                        self.assertFalse(result["request_cadence_required"])
                        self.assertEqual(result["request_cadence_status"], "failed")
                    else:
                        with self.assertRaisesRegex(self.gate.GameplayError, "fresh-frame budget exceeds 1%"):
                            self.gate.verify_report(report, platform)

    def test_missing_selector_wrong_radius_and_omitted_recovery_loop_fail_closed(self):
        for mutation in ("missing-proof", "wrong-proof", "radius", "gap", "negative-rate", "nonresident"):
            for proof in ("terrain-coverage",):
                with self.subTest(mutation=mutation, proof=proof):
                    report = self.report(proof=proof)
                    game = report["game_benchmark"]
                    if mutation == "missing-proof":
                        del game["qualification_proof"]
                    elif mutation == "wrong-proof":
                        game["qualification_proof"] = "unknown"
                    elif mutation == "radius":
                        game["full_radius_terrain"]["render_distance"] = 64
                    elif mutation == "gap":
                        next(frame for frame in game["raw_frames"] if frame["phase"] == self.gate.PHASES[3])["loop_ordinal"] += 2
                    elif mutation == "negative-rate":
                        game["full_radius_terrain"]["baseline_present_requests_per_second"] = -1
                    else:
                        game["full_radius_terrain"]["initial_barrier"]["nonresident_desired"] = 1
                    with self.assertRaises(self.gate.GameplayError):
                        self.gate.verify_report(report, "macos")

    def test_aggregate_requires_four_distinct_proofs_and_one_source_candidate(self):
        for mutation in ("missing", "duplicate", "mixed-source", "legacy"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                aggregate = self.aggregate(root)
                if mutation == "missing":
                    aggregate["rows"].pop()
                elif mutation == "duplicate":
                    aggregate["rows"][3] = copy.deepcopy(aggregate["rows"][1])
                elif mutation == "mixed-source":
                    aggregate["rows"][3]["source_candidate"] = {"head": "f" * 40, "tree": "e" * 40}
                else:
                    aggregate["schema_version"] = "mc.gameplay-proof.v1"
                receipt = root / "receipt.json"
                receipt.write_text(json.dumps(aggregate), encoding="utf-8")
                with self.assertRaises(self.gate.GameplayError):
                    self.gate.verify_receipt(root, receipt)











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

    def report(self, platform="macos", interval_ns=None, movement_interval_ns=None, proof="performance",
               stationary_duration_ns=None, movement_duration_ns=None, recovery_duration_ns=None):
        interval = interval_ns or (8000000 if platform == "macos" else 2000000)
        moving_interval = movement_interval_ns or interval
        radius = self.gate.PROOFS[proof]
        stationary_duration = stationary_duration_ns or (
            30000000000 if proof == "performance" else 10000000000
        )
        movement_duration = movement_duration_ns or (
            30000000000 if proof == "performance" else 6000000000
        )
        recovery_duration = recovery_duration_ns or stationary_duration
        bands = [0] * 16
        for x in range(-radius, radius + 1):
            limit = math.isqrt(radius * radius - x * x)
            for y in range(-limit, limit + 1):
                bands[min(max(abs(x), abs(y)) // 16, 15)] += 1
        barrier = {key: 0 for key in (*self.gate.QUEUE_FIELDS, "nonresident_desired", "stale_or_missing_mesh",
                                    "unclassified_resident_missing", "visible_outside_interest", "unclassified_current_visible_mesh_detail")}
        tile_count = sum(bands)
        barrier.update({key: tile_count for key in ("desired", "resident_desired", "resident_total", "current_handle_mesh_visible",
                                               "visible_meshes", "renderer_mesh_slots")})
        barrier.update({"scan_performed": True, "potentially_ready": True, "exact_current_coverage": True,
                        "center": {"x": 0, "y": 0}, "current_visible_mesh_detail_counts": {
                            "coarse": tile_count, "coarse2": 0, "coarse4": 0, "fine": 0, "distant": 0},
                        "radial_band_tile_width": 16, "radial_bands": [{"desired": count, "resident": count,
                            "current_mesh_visible": count, "nonresident": 0, "stale_or_missing_mesh": 0} for count in bands]})
        frames, offset = [], 0
        full = {"contract": "mc.full-radius-terrain-barriers.v1", "render_distance": radius, "expected_tile_count": tile_count,
                "complete": True, "warmup_timeout_ms": 180000, "initial_warmup_elapsed_ms": 1, "recovery_warmup_elapsed_ms": 1,
                "initial_barrier_reached": True, "recovery_barrier_reached": True,
                "movement_elapsed_ms": math.ceil(movement_duration / moving_interval) * moving_interval // 1000000,
                "movement_center_change_count": 2, "movement_start_center": {"x": 0, "y": 0}, "movement_end_center": {"x": 2, "y": 0},
                "loop_timing_scope": "all-stationary-loop-attempts", "successful_present_request_interval_scope":
                "consecutive-successful-request-completions-not-physical-display-cadence"}
        for index, phase in enumerate(self.gate.PHASES):
            durations = ([1000000] if index in (0, 3) else
                         [moving_interval] * math.ceil(movement_duration / moving_interval) if index == 2 else
                         [interval] * math.ceil((recovery_duration if index == 4 else stationary_duration) / interval))
            for number, duration in enumerate(durations):
                terrain_center = {"x": min(number // 4, 2) if index == 2 else 2 if index > 2 else 0, "y": 0}
                terrain_near = {"center": dict(terrain_center), "desired": 225, "resident": 225,
                                "nonresident": 0, "current_mesh_visible": 225, "pending": 0,
                                "active": 0, "unclassified_missing": 0} if index in (2, 3) else None
                frames.append({"phase": phase, "loop_ordinal": len(frames) + 1, "presentation_succeeded": True, "started_offset_ns": offset,
                               "loop_duration_ns": duration, "terrain_center": terrain_center,
                               "terrain_near": terrain_near,
                               "diagnostic_camera": [0.0, 0.0, 64.0,
                                   float((number * 13) % 360) if index == 2 else 0.0,
                                   float((number % 30) - 15) if index == 2 else 0.0, 0.0, 60.0],
                               "diagnostic_presentation_recreation_attempted": False,
                               "diagnostic_presentation_recreation_deferred": False,
                               "diagnostic_cpu_presentation_recreation_duration_ns": 0,
                               "diagnostic_terrain_radial_bands": None,
                               "diagnostic_terrain_spatial_scan_duration_ns": 0,
                               "diagnostic_frame_recorded": True})
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
        self.reconcile_submission_identity(frames)
        near_sample_count = sum(frame["phase"] in (self.gate.PHASES[2], self.gate.PHASES[3]) for frame in frames)
        readiness = {"contract": "mc.current-near-terrain-readiness.v1",
                     "measurement_scope": "current-center-radius-7",
                     "current_near_sample_count": near_sample_count,
                     "incomplete_current_near_sample_count": 0,
                     "max_missing_current_mesh_count": 0,
                     "current_near_coverage_complete": True}
        raw_completions = self.raw_fence_confirmed_frames(frames)
        report = {"schema_version": "mc.runtime-evidence.v1", "mode": "benchmark-game", "passed": True, "failure": "",
                "timing": {"elapsed_ms": offset // 1000000, "deadline_ms": 480000}, "game_benchmark": {
                    "workload": (self.gate.WORKLOAD if proof == "performance" else self.gate.COVERAGE_WORKLOAD),
                    "qualification_proof": proof,
                    "diagnostic_overrides_present": False, "build_mode": "release", "displayed_cadence_hz": None,
                    "frame_completion_source": self.gate.FRAME_COMPLETION_SOURCE,
                    "raw_fence_confirmed_frames": raw_completions,
                    "frame_budget": (self.frame_budget(frames, proof, raw_completions, platform)
                                     if proof == "terrain-coverage" else None),
                    "present_mode": "immediate" if platform == "windows" else "fifo",
                    "framebuffer": {"width": 1280, "height": 720}, "failed_present_requests": 0,
                    "successful_present_requests": len(frames),
                    "server_events_processed": 300, "inputs_sent": 300, "authoritative_player_updates": 300,
                    "terrain_readiness": readiness, "full_radius_terrain": full, "raw_frames": frames,
                    "raw_server_tick_durations_ns": [1000000, 2000000],
                    "server_tick_timings_ns": self.gate.timing_summary([1000000, 2000000])}}
        if proof == "performance":
            self.configure_s1_performance_report(report, moving_interval)
        return report

    def configure_s1_performance_report(self, report, movement_interval):
        game = report["game_benchmark"]
        settings = self.gate.qualification_settings_manifest()
        platform = "windows" if game["present_mode"] == "immediate" else "macos"
        game["framebuffer"] = copy.deepcopy(settings["platforms"][platform]["framebuffer"])
        game["qualification_settings_manifest"] = copy.deepcopy(settings)
        game["terrain_identity"] = copy.deepcopy(settings["terrain_identity"])
        game["session_join_epoch"] = 1
        old_frames = game["raw_frames"]
        original_movement = [frame for frame in old_frames if frame["phase"] == self.gate.PHASES[2]]
        start = 1_900_000_000
        original_start = original_movement[0]["started_offset_ns"]
        for frame in original_movement:
            frame["phase"] = self.gate.S1_MOVEMENT_PHASE
            frame["started_offset_ns"] = start + frame["started_offset_ns"] - original_start
        fill_frames = [
            {**original_movement[0], "phase": self.gate.S1_FILL_PHASE, "loop_ordinal": 1,
             "started_offset_ns": 0, "loop_duration_ns": 800_000_000, "playable_sphere": None},
            {**original_movement[0], "phase": self.gate.S1_FILL_PHASE, "loop_ordinal": 2,
             "started_offset_ns": 800_000_000, "loop_duration_ns": 1_000_000_000, "playable_sphere": None},
        ]
        movement_frames = original_movement
        for index, frame in enumerate(movement_frames, 3):
            frame["loop_ordinal"] = index
        game["raw_frames"] = [*fill_frames, *movement_frames]
        for frame in game["raw_frames"]:
            frame["diagnostic_camera"] = list(frame["diagnostic_camera"])
            frame["diagnostic_camera"][6] = settings["fov_degrees"]
            frame["qualification_settings"] = {
                "framebuffer": copy.deepcopy(game["framebuffer"]),
                "debug_hud_enabled": settings["debug_hud_enabled"],
                "terrain_model": settings["terrain_model"],
                "render_distance": settings["render_distance"],
                "camera_perspective": settings["camera_perspective"],
                "world_mode": settings["world_mode"],
            }
            frame["visual_lod"] = {
                "radius_blocks": self.gate.VISUAL_LOD_RADIUS_BLOCKS,
                "scene_active": True,
                "leaf_count": 1,
                "actual_column_count": 0,
            }
        game.pop("full_radius_terrain", None)
        game.pop("terrain_readiness", None)
        game.pop("presentation_timing", None)
        game["failed_present_requests"] = sum(not frame["presentation_succeeded"] for frame in game["raw_frames"])
        game["successful_present_requests"] = len(game["raw_frames"]) - game["failed_present_requests"]
        game["s1_terrain_performance"] = {
            "contract": self.gate.S1_PERFORMANCE_CONTRACT,
            "timing_origin": "world-request-accepted",
            "initial_radius_blocks": 48,
            "actual_radius_blocks": self.gate.ACTUAL_TERRAIN_RADIUS_BLOCKS,
            "visual_lod_radius_blocks": self.gate.VISUAL_LOD_RADIUS_BLOCKS,
            "ready_prompt_frame_offset_ns": 200_000_000,
            "playable_sphere_first_ready_offset_ns": 2_150_000_000,
            "initial_deadline_missed": False,
            "playable_deadline_missed": False,
            "movement_started_after_current_sphere_ready": True,
            "movement_input_during_fill": False,
            "movement_start_offset_ns": 2_200_000_000,
            "movement_end_offset_ns": 32_200_000_000,
            "movement_authoritative_update_count": 0,
            "movement_trajectory_anchor": None,
            "movement_trajectory_anchor_sphere_sample_offset_ns": None,
            "movement_boundary_authoritative_updates_complete": False,
            "movement_boundary_authoritative_updates": [],
            "movement_authoritative_sphere_updates": [],
            "movement_playable_sphere_complete": False,
            "movement_visual_lod_samples_complete": False,
            "complete": False,
        }
        game["controlled_start"] = {
            "contract": "mc.controlled-world-start.v1",
            "input_source": "synthetic-controlled-start-key",
            "production_path": "GameClientBenchmarkHooks.start_input_override->GameClient.acceptLoadingStartInput",
            "synthetic_input_override_invoked": True,
            "eligible_prompt_rendered_offset_ns": 200_000_000,
            "synthetic_input_requested_offset_ns": 210_000_000,
            "key_accepted_offset_ns": 220_000_000,
            "server_start_acknowledged_offset_ns": 300_000_000,
            "input_ordinal": 1,
            "offset_origin": "GameBenchmark.request_accepted_at",
            "complete": True,
        }
        self.reconcile_submission_identity(game["raw_frames"])
        game["raw_fence_confirmed_frames"] = self.raw_fence_confirmed_frames(game["raw_frames"])
        game["frame_budget"] = self.frame_budget(
            game["raw_frames"], "performance", game["raw_fence_confirmed_frames"],
            "windows" if game["present_mode"] == "immediate" else "macos",
        )
        game["raw_gameplay_inputs"] = []
        self.reconcile_movement_playable_sphere(report)
        report["timing"]["elapsed_ms"] = math.ceil(
            (300_000_000 + start + self.gate.PERFORMANCE_SAMPLE_MS * 1_000_000 + 16_000_000) / 1_000_000
        )

    def reconcile_movement_playable_sphere(self, report, step_count=300, fill_count=18):
        game = report["game_benchmark"]
        full = game["s1_terrain_performance"] if game["qualification_proof"] == "performance" else game["full_radius_terrain"]
        is_s1 = game["qualification_proof"] == "performance"

        def position_for_frame(frame, movement_ordinal):
            x = 0 if frame["phase"] != self.gate.S1_MOVEMENT_PHASE else movement_ordinal
            return {"x": x, "y": 0, "z": 64, "x_subcell": 0, "y_subcell": 0, "z_subcell": 0}

        def sample(position, epoch, ready=True, active_center=None):
            return {"radius_blocks": (self.gate.ACTUAL_TERRAIN_RADIUS_BLOCKS if is_s1
                                       else self.gate.PLAYABLE_SPHERE_RADIUS_BLOCKS),
                    "active_coverage_epoch": epoch,
                    "active_coverage_center": dict(position if active_center is None else active_center),
                    "world_loading_center": dict(position),
                    "ready_chunks": (419904 if ready else 209920) if is_s1 else (1000 if ready else 500),
                    "required_chunks": 419904 if is_s1 else 1000, "ready": ready,
                    **({"coverage_scope": "horizontal_full_volume",
                        "required_authoritative_columns": 6561,
                        "resident_authoritative_columns": 6561 if ready else 3280,
                        "vertical_chunk_count": 64, "authoritative_columns_center": dict(position)} if is_s1 else {})}

        def visual_lod_sample():
            return {"radius_blocks": self.gate.VISUAL_LOD_RADIUS_BLOCKS,
                    "scene_active": True, "leaf_count": 1, "actual_column_count": 0}

        movement_ordinal = 0
        for frame in game["raw_frames"]:
            phase = frame["phase"]
            if phase != self.gate.S1_MOVEMENT_PHASE:
                frame["playable_sphere"] = None
                continue
            movement_ordinal += 1
            position = position_for_frame(frame, movement_ordinal)
            epoch = 1 if phase == self.gate.PHASES[1] else movement_ordinal + 1
            frame["playable_sphere"] = sample(position, epoch)
            if is_s1:
                frame["visual_lod"] = visual_lod_sample()

        budget = game["frame_budget"]
        start = budget["measurement_window_start_offset_ns"]
        duration = budget["required_window_duration_ns"]
        def direction_for(intent, yaw, pitch):
            radians = math.radians(yaw % 360.0)
            sine, cosine = math.sin(radians), math.cos(radians)
            world_x = intent["strafe"] * cosine + intent["forward"] * sine
            world_y = -intent["strafe"] * sine + intent["forward"] * cosine
            length = math.hypot(world_x, world_y)
            pitch_radians = math.radians(pitch)
            direction_x, direction_y = ((int(world_x / length * 127), int(world_y / length * 127))
                                        if length > 0 else (0, 0))
            return {"x": direction_x, "y": direction_y, "z": intent["vertical"] * 127,
                    "view_x": int(sine * math.cos(pitch_radians) * 127),
                    "view_y": int(cosine * math.cos(pitch_radians) * 127),
                    "accelerated": True, "speedup": 30,
                    "cycle_movement_capabilities": False}

        def add_step(position, direction):
            squared = direction["x"] ** 2 + direction["y"] ** 2 + direction["z"] ** 2
            divisor = max(127, math.isqrt(squared) + (math.isqrt(squared) ** 2 < squared))
            speed_factor = direction["speedup"] if direction["accelerated"] else 1
            deltas = [(5600 * abs(direction[axis]) // divisor * speed_factor)
                      * (-1 if direction[axis] < 0 else 1) for axis in ("x", "y", "z")]
            x = (position["x"] * 10000 + position["x_subcell"] + deltas[0]) % (65536 * 10000)
            y = (position["y"] * 10000 + position["y_subcell"] + deltas[1]) % (65536 * 10000)
            return {"x": x // 10000, "x_subcell": x % 10000,
                    "y": y // 10000, "y_subcell": y % 10000,
                    "z": (position["z"] * 10000 + position["z_subcell"] + deltas[2]) // 10000,
                    "z_subcell": (position["z"] * 10000 + position["z_subcell"] + deltas[2]) % 10000}

        updates, route_inputs = [], []
        yaws = (0.0, 90.0, 180.0, 270.0, 45.0, 135.0, 225.0, 315.0, 30.0, 210.0)
        pitches = (-20.0, -10.0, 0.0, 10.0, 20.0, 15.0, 5.0, -5.0, -15.0, 25.0)
        intents = ((0, 1), (1, 0), (0, -1), (-1, 0), (0, 1), (1, 0),
                   (0, -1), (-1, 0), (0, 1), (1, 0))
        expected_steps = duration // self.gate.SERVER_TICK_NS
        send_latency = 10_000_000
        for index in range(step_count):
            yaw = yaws[index % len(yaws)]
            pitch = pitches[index % len(pitches)]
            strafe, forward = intents[index % len(intents)]
            vertical = 1 if index == 0 else (-1 if index == 180 else 0)
            scheduled_offset = start + index * self.gate.SERVER_TICK_NS - send_latency
            sent_offset = start + index * self.gate.SERVER_TICK_NS
            offset = sent_offset + 10_000_000
            intent = {"strafe": strafe, "forward": forward, "vertical": vertical}
            direction = direction_for(intent, yaw, pitch)
            route_inputs.append({"input_ordinal": index + 1, "sequence": index + 1,
                                 "sent_offset_ns": sent_offset, "scheduled_offset_ns": scheduled_offset,
                                 "movement_intent": intent,
                                 "camera_look": {"yaw_degrees": yaw, "pitch_degrees": pitch},
                                 "direction": direction})

        fill_inputs = []
        for index in range(fill_count):
            route = route_inputs[0]
            sent_offset = (index + 1) * self.gate.SERVER_TICK_NS
            fill_direction = dict(route["direction"])
            fill_direction["accelerated"] = False
            fill_inputs.append({**route,
                                "input_ordinal": index + 1,
                                "sequence": index + 1,
                                "sent_offset_ns": sent_offset,
                                "scheduled_offset_ns": sent_offset - send_latency,
                                "direction": fill_direction})

        anchor_position = {"x": 0, "y": 0, "z": 64,
                           "x_subcell": 0, "y_subcell": 0, "z_subcell": 0}
        for raw_input in fill_inputs:
            anchor_position = add_step(anchor_position, raw_input["direction"])
        fill_coverage_center = {"x": 0, "y": 0, "z": 64,
                                "x_subcell": 0, "y_subcell": 0, "z_subcell": 0}
        fill_positions = []
        position = {"x": 0, "y": 0, "z": 64,
                    "x_subcell": 0, "y_subcell": 0, "z_subcell": 0}
        for raw_input in fill_inputs:
            position = add_step(position, raw_input["direction"])
            fill_positions.append(position)
        for frame in game["raw_frames"]:
            if frame["phase"] != self.gate.S1_FILL_PHASE:
                continue
            frame_end = frame["started_offset_ns"] + frame["loop_duration_ns"]
            observed_count = sum(raw_input["sent_offset_ns"] <= frame_end for raw_input in fill_inputs)
            if observed_count:
                frame["playable_sphere"] = sample(
                    fill_positions[observed_count - 1], frame["loop_ordinal"], ready=False,
                    active_center=fill_coverage_center)
        anchor_offset = start - 90_000_000 if fill_count else start - 1
        anchor = {"update_ordinal": fill_count + 1, "offset_ns": anchor_offset,
                  "state_revision": fill_count + 1, "acknowledged_input_sequence": fill_count,
                  "authoritative_position": dict(anchor_position),
                  "movement_capabilities": 3, "vertical_velocity_subcells": 0,
                  "playable_sphere": sample(anchor_position, fill_count + 1),
                  **({"visual_lod": visual_lod_sample()} if is_s1 else {})}
        inputs = []
        position = dict(anchor_position)
        for index, raw_input in enumerate(route_inputs):
            sequence = fill_count + index + 1
            raw_input["input_ordinal"] = sequence
            raw_input["sequence"] = sequence
            inputs.append(raw_input)
            direction = raw_input["direction"]
            position = add_step(position, direction)
            updates.append({"update_ordinal": fill_count + index + 2, "offset_ns": raw_input["sent_offset_ns"] + 10_000_000,
                            "authoritative_position": dict(position),
                            "playable_sphere": sample(position, fill_count + index + 2),
                            **({"visual_lod": visual_lod_sample()} if is_s1 else {}),
                            "state_revision": fill_count + index + 2,
                            "acknowledged_input_sequence": sequence,
                            "movement_capabilities": 3,
                            "vertical_velocity_subcells": 0})
        authority_index = 0
        for frame in game["raw_frames"]:
            if frame["phase"] != self.gate.S1_MOVEMENT_PHASE:
                continue
            frame_end = frame["started_offset_ns"] + frame["loop_duration_ns"]
            while authority_index < len(updates) and updates[authority_index]["offset_ns"] <= frame_end:
                authority_index += 1
            state = updates[authority_index - 1] if authority_index else anchor
            frame["playable_sphere"] = copy.deepcopy(state["playable_sphere"])
            pose = state["authoritative_position"]
            frame["diagnostic_camera"] = list(frame["diagnostic_camera"])
            frame["diagnostic_camera"][:3] = [
                pose["x"] + pose["x_subcell"] / 10000.0 + 0.3125,
                pose["y"] + pose["y_subcell"] / 10000.0 + 0.3125,
                pose["z"] + pose["z_subcell"] / 10000.0 + 1.625,
            ]

        def decorate_visual(visual, sphere, submission=1):
            center = sphere["world_loading_center"]
            visual.update({
                "contract": "mc.presented-terrain-coverage.v1",
                "leaf_count": 6561, "actual_column_count": 6561,
                "terrain_identity": copy.deepcopy(game["terrain_identity"]),
                "session_join_epoch": game["session_join_epoch"], "coverage_epoch": 1,
                "submitted_frame_count": submission,
                "scene_center": {"x": center["x"] % 65536, "y": center["y"] % 65536},
                "actual_radius_blocks": 640, "required_actual_columns": 6561,
                "presented_current_actual_columns": 6561,
                "coverage_center": copy.deepcopy(center), "sources_current": True,
            })
        if is_s1:
            for state in [anchor, *updates]:
                decorate_visual(state["visual_lod"], state["playable_sphere"])
            for frame in game["raw_frames"]:
                if frame.get("playable_sphere") is not None:
                    decorate_visual(frame["visual_lod"], frame["playable_sphere"], frame["loop_ordinal"])
                    camera = frame["diagnostic_camera"]
                    frame["presented_player"] = {
                        "body": [camera[0] - 0.3125, camera[1] - 0.3125, camera[2] - 1.625],
                        "submitted_frame_count": frame["loop_ordinal"],
                        "sampled_offset_ns": frame["started_offset_ns"],
                    }

        full.update({"movement_authoritative_update_count": len(updates),
                     "movement_playable_sphere_complete": True,
                     "movement_visual_lod_samples_complete": is_s1,
                     "movement_authoritative_sphere_updates": updates,
                     "movement_trajectory_anchor": anchor,
                     "movement_trajectory_anchor_sphere_sample_offset_ns": start,
                     "movement_boundary_authoritative_updates_complete": True,
                     "movement_boundary_authoritative_updates": []})
        if game["qualification_proof"] == "performance":
            full["complete"] = True
        game["movement_trajectory_contract"] = self.gate.MOVEMENT_TRAJECTORY_CONTRACT
        game["raw_gameplay_inputs"] = fill_inputs + inputs
        game["server_events_processed"] = step_count
        game["inputs_sent"] = len(fill_inputs) + len(inputs)
        game["authoritative_player_updates"] = len(fill_inputs) + step_count
        if game["qualification_proof"] == "performance":
            full["movement_input_during_fill"] = bool(fill_inputs)

    def add_delayed_boundary_ack(self, report, *, accelerated=False):
        game = report["game_benchmark"]
        full = game["s1_terrain_performance"]
        start = game["frame_budget"]["measurement_window_start_offset_ns"]
        inputs = game["raw_gameplay_inputs"]
        fill_inputs = [entry for entry in inputs if entry["sent_offset_ns"] < start]
        route_inputs = [entry for entry in inputs if entry["sent_offset_ns"] >= start]
        updates = full["movement_authoritative_sphere_updates"]
        anchor = full["movement_trajectory_anchor"]
        delayed = copy.deepcopy(fill_inputs[-1])
        delayed["input_ordinal"] = len(fill_inputs) + 1
        delayed["sequence"] = route_inputs[0]["sequence"]
        delayed["sent_offset_ns"] = start - 70_000_000
        delayed["scheduled_offset_ns"] = start - 80_000_000
        delayed["direction"]["accelerated"] = accelerated
        for raw_input in route_inputs:
            raw_input["input_ordinal"] += 1
            raw_input["sequence"] += 1

        def add_step(position, direction):
            squared = direction["x"] ** 2 + direction["y"] ** 2 + direction["z"] ** 2
            length = math.isqrt(squared)
            divisor = max(127, length + (length * length < squared))
            factor = direction["speedup"] if direction["accelerated"] else 1
            delta = [(5600 * abs(direction[axis]) // divisor * factor)
                     * (-1 if direction[axis] < 0 else 1) for axis in ("x", "y", "z")]
            x = (position["x"] * 10000 + position["x_subcell"] + delta[0]) % (65536 * 10000)
            y = (position["y"] * 10000 + position["y_subcell"] + delta[1]) % (65536 * 10000)
            z = position["z"] * 10000 + position["z_subcell"] + delta[2]
            return {"x": x // 10000, "x_subcell": x % 10000,
                    "y": y // 10000, "y_subcell": y % 10000,
                    "z": z // 10000, "z_subcell": z % 10000}

        position = add_step(anchor["authoritative_position"], delayed["direction"])
        boundary = copy.deepcopy(anchor)
        boundary.update({"update_ordinal": anchor["update_ordinal"] + 1,
                         "offset_ns": start + 5_000_000,
                         "state_revision": anchor["state_revision"] + 1,
                         "acknowledged_input_sequence": delayed["sequence"],
                         "authoritative_position": position})
        boundary["playable_sphere"].update({
            "active_coverage_epoch": anchor["playable_sphere"]["active_coverage_epoch"] + 1,
            "active_coverage_center": dict(position), "world_loading_center": dict(position),
            "authoritative_columns_center": dict(position),
            "ready_chunks": anchor["playable_sphere"]["required_chunks"],
            "ready": True,
        })
        boundary["visual_lod"]["coverage_center"] = dict(position)
        boundary["visual_lod"]["scene_center"] = {"x": position["x"], "y": position["y"]}
        full["movement_boundary_authoritative_updates"] = [boundary]
        full["movement_boundary_authoritative_updates_complete"] = True
        for raw_input, update in zip(route_inputs, updates):
            update["update_ordinal"] += 1
            update["state_revision"] += 1
            update["acknowledged_input_sequence"] += 1
            position = add_step(position, raw_input["direction"])
            update["authoritative_position"] = dict(position)
            update["playable_sphere"]["world_loading_center"] = dict(position)
            update["playable_sphere"]["authoritative_columns_center"] = dict(position)
            update["playable_sphere"]["active_coverage_center"] = dict(position)
            update["visual_lod"]["coverage_center"] = dict(position)
            update["visual_lod"]["scene_center"] = {"x": position["x"], "y": position["y"]}
        game["raw_gameplay_inputs"] = [*fill_inputs, delayed, *route_inputs]
        game["inputs_sent"] = len(game["raw_gameplay_inputs"])
        game["authoritative_player_updates"] += 1

    def reconcile_presentation_timing(self, report, spacing_ns=8333333):
        game = report["game_benchmark"]
        self.reconcile_submission_identity(game["raw_frames"])
        requests, records = [], []
        present_id = 0
        for frame in game["raw_frames"]:
            if not frame["presentation_succeeded"]:
                continue
            present_id += 1
            frame["application_present_id"] = present_id
            requests.append({"swapchain_generation": 1, "application_present_id": present_id,
                             "loop_ordinal": frame["loop_ordinal"], "phase": frame["phase"]})
            records.append({"swapchain_generation": 1, "application_present_id": present_id,
                            "actual_present_time_ns": 1_000_000_000_000 + (present_id - 1) * spacing_ns})
        game["successful_present_requests"] = len(requests)
        endpoint_request = requests[-1]
        polls = [{"scope": "loop", "status": "records" if frame["presentation_succeeded"] else "empty",
                  "vk_result": 0, "returned_count": 1 if frame["presentation_succeeded"] else 0}
                 for frame in game["raw_frames"]]
        polls.append({"scope": "drain", "status": "empty", "vk_result": 0, "returned_count": 0})
        game["presentation_timing"] = {
            "contract": self.gate.ACTUAL_PRESENTATION_CONTRACT,
            "provider": self.gate.ACTUAL_PRESENTATION_PROVIDER,
            "source": "driver-reported",
            "capability": "supported",
            "clock_domain": self.gate.ACTUAL_PRESENTATION_CLOCK_DOMAIN,
            "successful_requests": requests,
            "records": records,
            "polls": polls,
            "capture_endpoint": {key: endpoint_request[key] for key in
                                  ("swapchain_generation", "application_present_id", "loop_ordinal")},
            "drain": {"complete": True, "request_ids": []}}
        game["raw_fence_confirmed_frames"] = self.raw_fence_confirmed_frames(game["raw_frames"])
        game["frame_budget"] = self.frame_budget(
            game["raw_frames"], game["qualification_proof"], game["raw_fence_confirmed_frames"],
            "windows" if game["present_mode"] == "immediate" else "macos"
        )

    def set_actual_phase_intervals(self, report, target_phase, interval_ns, interval_indices=None):
        game = report["game_benchmark"]
        timing = game["presentation_timing"]
        frames = {frame["loop_ordinal"]: frame for frame in game["raw_frames"]}
        requests = timing["successful_requests"]
        target_phases = set(target_phase) if isinstance(target_phase, (tuple, list, set)) else {target_phase}
        event_times = {(requests[0]["swapchain_generation"], requests[0]["application_present_id"]): 1_000_000_000_000}
        last_time = next(iter(event_times.values()))
        selected = set(interval_indices) if interval_indices is not None else None
        phase_interval_index = 0
        for previous, current in zip(requests, requests[1:]):
            next_attempt = frames.get(previous["loop_ordinal"] + 1)
            phase = next_attempt["phase"] if next_attempt is not None else None
            use_interval = phase in target_phases and (selected is None or phase_interval_index in selected)
            last_time += interval_ns if use_interval else 8333333
            if phase in target_phases:
                phase_interval_index += 1
            event_times[(current["swapchain_generation"], current["application_present_id"])] = last_time
        for record in timing["records"]:
            record["actual_present_time_ns"] = event_times[(record["swapchain_generation"], record["application_present_id"])]

    def aggregate(self, root, mac_interval_ns=None):
        for name in ("CMakeLists.txt", "CMakePresets.json", "dependencies.lock.json"):
            (root / name).write_text("fixture\n", encoding="utf-8")
        (root / "src").mkdir()
        project = root / "src/shared/include/shared/ProjectInfo.hpp"
        project.parent.mkdir(parents=True)
        shutil.copyfile(REPOSITORY / project.relative_to(root), project)
        shutil.copyfile(REPOSITORY / "vcpkg.json", root / "vcpkg.json")
        (root / "src/game.cpp").write_text("source\n", encoding="utf-8")
        scenario_directory = root / "scenarios/world"
        scenario_directory.mkdir(parents=True)
        (scenario_directory / "canonical_world.core").write_text("world fixture\n", encoding="utf-8")
        (scenario_directory / "height_tile.core").write_text("height fixture\n", encoding="utf-8")
        digest, rows = self.gate.source_digest(root), []
        for platform, proof in ((platform, proof) for proof in ("performance", "terrain-coverage") for platform in self.gate.TARGET_HZ):
            directory = root / (platform if proof == "performance" else platform + "-" + proof)
            directory.mkdir()
            executable = directory / ("mc_main.exe" if platform == "windows" else "mc_main")
            executable.write_bytes(b"fixture executable")
            report = directory / "report.json"
            report_value = self.report(platform, mac_interval_ns if platform == "macos" else None, proof=proof)
            report.write_text(json.dumps(report_value), encoding="utf-8")
            executable_identity = self.gate.file_identity(executable)
            rows.append({"platform": platform, "proof": proof, "proof_origin": "local", "source_candidate": CANDIDATE,
                         "source_input_sha256": digest, "build": {"configuration": "Release", "source_input_sha256": digest,
                            "executable_sha256": executable_identity["sha256"], "configuration_sha256": "c" * 64, "dependency_sha256": "d" * 64},
                         "executable": executable_identity, "report": self.gate.file_identity(report), "warmup_seconds": 180,
                         "command": [str(executable), *self.gate.benchmark_arguments(platform, 180, str(report), proof)],
                         "exit_code": 0, "timed_out": False, "environment": {}})
            if platform == "macos":
                package, manifest = self.macos_package(root, executable)
                rows[-1].update(macos_runtime=self.macos_runtime(package, manifest), native_report_path=str(report),
                                actual_command=[str(package / "MinecraftClone.command"), *rows[-1]["command"][1:]])
        return {"schema_version": self.gate.SCHEMA, "rows": rows}

    def reconcile_submission_identity(self, frames):
        present_id = 0
        sequence_id = 0
        for frame in frames:
            if frame["presentation_succeeded"]:
                present_id += 1
                sequence_id += 1
                frame.update({"swapchain_generation": 1, "application_present_id": present_id,
                              "submitted_frame_sequence_id": sequence_id,
                              "submitted_frame_slot": (sequence_id - 1) % 3,
                              "submitted_frame_offset_ns": frame["started_offset_ns"] + frame["loop_duration_ns"],
                              "submitted_frame_phase": frame["phase"]})
            else:
                frame.update({"swapchain_generation": None, "application_present_id": None,
                              "submitted_frame_sequence_id": None, "submitted_frame_slot": None,
                              "submitted_frame_offset_ns": 0, "submitted_frame_phase": ""})

    def raw_fence_confirmed_frames(self, frames):
        completions = []
        for frame in frames:
            if not frame["presentation_succeeded"]:
                continue
            submitted_offset = frame["submitted_frame_offset_ns"]
            completion_delay = 16_000_000
            completions.append({"sequence_id": frame["submitted_frame_sequence_id"],
                                "swapchain_generation": frame["swapchain_generation"],
                                "frame_slot": frame["submitted_frame_slot"],
                                "submitted_offset_ns": submitted_offset,
                                "completion_observed_offset_ns": submitted_offset + completion_delay,
                                "application_present_id": frame["application_present_id"],
                                "submission_phase": frame["submitted_frame_phase"]})
        return completions

    def reconcile_frame_completion_evidence(self, report):
        game = report["game_benchmark"]
        sequence_id = 0
        for frame in game["raw_frames"]:
            if frame["presentation_succeeded"]:
                sequence_id += 1
                frame["submitted_frame_sequence_id"] = sequence_id
                frame["submitted_frame_slot"] = (sequence_id - 1) % 3
                frame["submitted_frame_offset_ns"] = frame["started_offset_ns"] + frame["loop_duration_ns"]
                frame["submitted_frame_phase"] = frame["phase"]
            else:
                frame["submitted_frame_sequence_id"] = None
                frame["submitted_frame_slot"] = None
                frame["submitted_frame_offset_ns"] = 0
                frame["submitted_frame_phase"] = ""
                frame["swapchain_generation"] = None
                frame["application_present_id"] = None
        game["raw_fence_confirmed_frames"] = self.raw_fence_confirmed_frames(game["raw_frames"])
        game["frame_budget"] = self.frame_budget(
            game["raw_frames"], game["qualification_proof"], game["raw_fence_confirmed_frames"],
            "windows" if game["present_mode"] == "immediate" else "macos"
        )

    def frame_budget(self, frames, proof, completions, platform="macos"):
        movement_phase = self.gate.S1_MOVEMENT_PHASE if proof == "performance" else self.gate.PHASES[2]
        movement = [frame for frame in frames if frame["phase"] == movement_phase]
        if proof == "performance":
            start = movement[0]["started_offset_ns"]
        else:
            baseline = [frame for frame in frames if frame["phase"] == self.gate.PHASES[1]]
            start = baseline[-1]["started_offset_ns"] + baseline[-1]["loop_duration_ns"]
        required_duration = self.gate.PERFORMANCE_SAMPLE_MS * 1000000
        required_end = start + required_duration
        submitted = {frame["submitted_frame_sequence_id"]: frame for frame in movement
                     if frame["presentation_succeeded"]
                     and start <= frame["submitted_frame_offset_ns"] < required_end}
        matched = [completion for completion in completions
                   if completion["submission_phase"] == movement_phase
                   and start <= completion["submitted_offset_ns"] < required_end
                   and completion["sequence_id"] in submitted]
        end = required_end
        complete = (movement[-1]["started_offset_ns"] + movement[-1]["loop_duration_ns"] >= required_end
                    and len(matched) == len(submitted) and bool(matched))
        intervals, previous = [], start
        for completion in matched:
            observed = min(completion["completion_observed_offset_ns"], required_end)
            intervals.append(observed - previous)
            previous = observed
        if end > previous:
            intervals.append(end - previous)
        target_ns = self.gate.FRAME_BUDGET_TARGET_NS[platform]
        required_permille = self.gate.FRAME_BUDGET_REQUIRED_PERMILLE
        in_budget = sum(interval for interval in intervals if interval <= target_ns)
        total = end - start
        passed = complete and total - in_budget <= total * (1000 - required_permille) // 1000
        return {"measurement_scope": self.gate.FRAME_BUDGET_SCOPE,
                "target_frame_duration_ns": target_ns,
                "required_in_budget_time_permille": required_permille,
                "measurement_window_start_offset_ns": start,
                "measurement_window_end_offset_ns": end,
                "required_window_duration_ns": required_duration,
                "measured_time_ns": total,
                "in_budget_time_ns": in_budget,
                "sample_count": len(intervals),
                "fence_confirmed_frame_count": len(matched),
                "measurement_window_complete": complete,
                "passed": passed,
                "required_for_benchmark_pass": proof == "performance"}

    @contextlib.contextmanager
    def desktop_capture(self, **options):
        with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as patches:
            root = Path(directory).resolve()
            aggregate = self.aggregate(root)
            original = Path(aggregate["rows"][1]["executable"]["path"])
            package, helper = root / "package", root / "helper"
            package.mkdir()
            (helper / "jobs").mkdir(parents=True)
            inputs = {"mc_main.exe": original, "fmt.dll": original.parent / "fmt.dll",
                      "shaders/stone.vert.spv": original.parent / "shaders/stone.vert.spv",
                      "assets/textures/stone/default_stone.png": root / "assets/textures/stone/default_stone.png"}
            inventory = []
            for relative, origin in inputs.items():
                if not origin.exists():
                    origin.parent.mkdir(parents=True, exist_ok=True)
                    origin.write_bytes(relative.encode("ascii"))
                target = package / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(origin, target)
                identity = self.gate.file_identity(origin)
                inventory.append({"relative": relative.replace("/", "\\"), "size": identity["size"], "sha256": identity["sha256"]})
            manifest, receipt = root / "package.json", root / "capture.json"
            manifest.write_text(json.dumps(inventory), encoding="utf-8-sig")
            build = copy.deepcopy(aggregate["rows"][1]["build"])
            build["source_input_sha256"] = self.gate.source_digest(root)
            native_report = self.report("windows")
            native_report["game_benchmark"]["diagnostic_overrides_present"] = False
            state = {"calls": [], "state": "succeeded", "exit_code": 0, "report": native_report,
                     "request_mutation": {}, "status_mutation": {}, "cancelled": False, **options}

            def client(operation, **parameters):
                state["calls"].append((operation, parameters))
                job, app = helper / "jobs" / DESKTOP_JOB_ID, helper / "jobs" / DESKTOP_JOB_ID / "app"
                now = lambda: datetime.datetime.now(datetime.timezone.utc).isoformat()
                if operation == "Submit":
                    state["task_id"] = parameters["task_id"]
                    shutil.copytree(package, app)
                    submitted = now()
                    request = {"schema": 1, "job_id": DESKTOP_JOB_ID, "task_id": state["task_id"], "executable": "mc_main.exe",
                               "executable_sha256": build["executable_sha256"], "arguments": parameters["arguments"], "timeout_seconds": 480}
                    request.update(state["request_mutation"])
                    (job / "request.json").write_text(json.dumps(request), encoding="utf-8")
                    scheduler = {"schema": 1, "job_id": DESKTOP_JOB_ID, "task_id": state["task_id"], "state": "submitted",
                                 "instance_guid": "87654321-4321-4321-4321-abcdef123456", "submitted_utc": submitted, "error": None}
                    (job / "scheduler.json").write_text(json.dumps(scheduler), encoding="utf-8")
                    native = {"schema": 1, "job_id": DESKTOP_JOB_ID, "task_id": state["task_id"], "state": state["state"],
                              "started_at_utc": now(), "pid": 4242, "session_id": 1, "executable": "mc_main.exe",
                              "executable_sha256": build["executable_sha256"], "timeout_seconds": 480,
                              "exit_code": state["exit_code"] if state["state"] != "running" else None, "error": None}
                    if state["report"] is not None and state["state"] != "running":
                        (app / parameters["arguments"][-1]).write_text(json.dumps(state["report"]), encoding="utf-8")
                    if state["state"] != "running":
                        native["finished_at_utc"] = now()
                    state["native"] = native
                    (job / "status.json").write_text(json.dumps(native), encoding="utf-8")
                    for name in ("stdout.log", "stderr.log"):
                        (job / name).write_text("", encoding="utf-8")
                    (job / "broker.log").write_text(
                        f"{submitted} validated job={DESKTOP_JOB_ID} task={state['task_id']} sha256={build['executable_sha256']}\n"
                        f"{native['started_at_utc']} started job={DESKTOP_JOB_ID} pid=4242 session_id=1\n"
                        f"{now()} finished job={DESKTOP_JOB_ID} state={state['state']} pid=4242 session_id=1 exit_code={state['exit_code']} error=\n",
                        encoding="utf-8")
                    if state.get("after_submit"):
                        state["after_submit"](state, root, package, app)
                    if state.get("submit_error"):
                        raise state["submit_error"]
                    return state.get("submission", {"schema": 1, "job_id": DESKTOP_JOB_ID, "task_id": state["task_id"], "state": "submitted"})
                self.assertEqual(parameters["job_id"], DESKTOP_JOB_ID)
                if operation == "Cancel":
                    if state.get("cancel_error"):
                        raise state["cancel_error"]
                    state["cancelled"] = True
                    response = "already-terminal"
                    if state["native"]["state"] == "running":
                        state["native"].update(state="cancelled", exit_code=-9, finished_at_utc=now())
                        (job / "status.json").write_text(json.dumps(state["native"]), encoding="utf-8")
                        response = "cancel-requested"
                    return {"schema": 1, "job_id": DESKTOP_JOB_ID, "state": response}
                if state.get("poll_error") and not state["cancelled"]:
                    raise state["poll_error"]
                if state.get("after_status") and not state["cancelled"]:
                    state["after_status"](state, root, package, app)
                    state["after_status"] = None
                status = copy.deepcopy(state["native"])
                if not state["cancelled"]:
                    status.update(state["status_mutation"])
                return status

            helpers = {name: {"path": str(helper / "bin" / name), "size": 1, "sha256": digest}
                       for name, digest in self.gate.DESKTOP_HELPERS.items()}
            patches.enter_context(mock.patch.object(self.gate, "DESKTOP_ROOT", helper))
            patches.enter_context(mock.patch.object(self.gate, "desktop_helper_identity", return_value=helpers))
            patches.enter_context(mock.patch.object(self.gate, "build_identity", return_value=build))
            patches.enter_context(mock.patch.object(self.gate._MATRIX, "candidate_identity", return_value=CANDIDATE))
            patches.enter_context(mock.patch.object(self.gate.sys, "platform", "win32"))
            patches.enter_context(mock.patch.object(self.gate.os, "environ", {}))
            patches.enter_context(mock.patch.object(self.gate, "desktop_client", side_effect=client))
            patches.enter_context(mock.patch.object(self.gate.subprocess, "Popen", side_effect=AssertionError("direct native launch forbidden")))
            patches.enter_context(contextlib.redirect_stdout(io.StringIO()))
            yield {"root": root, "aggregate": aggregate, "executable": original, "receipt": receipt, "package": package,
                   "manifest": manifest, "build": build, "helper": helper, "state": state, "inputs": inputs}

    def record_desktop(self, capture, task_id=DESKTOP_TASK_ID):
        self.gate.record(capture["root"], capture["receipt"], "windows", capture["executable"], 180,
                         desktop_package=capture["package"], desktop_manifest=capture["manifest"], desktop_task_id=task_id)

    def reconcile_raw_totals(self, report):
        game = report["game_benchmark"]
        frames = game["raw_frames"]
        for ordinal, frame in enumerate(frames, 1):
            frame["loop_ordinal"] = ordinal
        self.reconcile_submission_identity(frames)
        game["raw_fence_confirmed_frames"] = self.raw_fence_confirmed_frames(frames)
        game["frame_budget"] = self.frame_budget(
            frames, game["qualification_proof"], game["raw_fence_confirmed_frames"],
            "windows" if game["present_mode"] == "immediate" else "macos"
        )
        game["successful_present_requests"] = sum(frame["presentation_succeeded"] for frame in frames)
        game["failed_present_requests"] = len(frames) - game["successful_present_requests"]
        if game["qualification_proof"] == "performance":
            self.reconcile_movement_playable_sphere(report)
            return

        full = game["full_radius_terrain"]
        for prefix, phase in (("baseline", self.gate.PHASES[1]), ("recovered", self.gate.PHASES[4])):
            selected = [frame for frame in frames if frame["phase"] == phase]
            completions = [frame["started_offset_ns"] + frame["loop_duration_ns"] for frame in selected if frame["presentation_succeeded"]]
            full.update({prefix + "_loop_attempts": len(selected), prefix + "_present_requests": len(completions),
                         prefix + "_failed_present_requests": len(selected) - len(completions),
                         prefix + "_loop_timings_ns": self.gate.timing_summary([frame["loop_duration_ns"] for frame in selected]),
                         prefix + "_successful_present_request_intervals_ns": self.gate.timing_summary(
                            [right - left for left, right in zip(completions, completions[1:])]),
                         prefix + "_present_requests_per_second": len(completions) * 1000 / full[prefix + "_stationary_elapsed_ms"]})
        if "presentation_timing" in game:
            self.reconcile_presentation_timing(report)

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
            self.assertEqual(verified["macos"]["performance"]["physical_display_cadence"], "unavailable")
            for platform in verified:
                for proof in self.gate.PROOFS:
                    if proof == "performance":
                        self.assertEqual(verified[platform][proof]["request_cadence_status"], "not-required")
                        self.assertFalse(verified[platform][proof]["request_cadence_required"])
                        self.assertFalse(verified[platform][proof]["presentation_timing_required"])
                    else:
                        self.assertEqual(verified[platform][proof]["request_cadence_status"], "passed")
                        self.assertFalse(verified[platform][proof]["request_cadence_required"])
                        self.assertFalse(verified[platform][proof]["presentation_timing_required"])
                    self.assertEqual(verified[platform][proof]["request_cadence_reasons"], [])

    def test_owner_accepted_s1_report_keeps_the_known_failure_metrics_explicit(self):
        report = {
            "schema_version": "mc.runtime-evidence.v1",
            "mode": "benchmark-game",
            "passed": False,
            "failure": "game benchmark did not complete the scripted full-game workload",
            "timing": {"elapsed_ms": 45_705, "deadline_ms": 60_000},
            "game_benchmark": {
                "workload": self.gate.WORKLOAD,
                "build_mode": "release",
                "qualification_proof": "performance",
                "s1_terrain_performance": {
                    "contract": self.gate.S1_PERFORMANCE_CONTRACT,
                    "initial_radius_blocks": 48,
                    "actual_radius_blocks": 640,
                    "visual_lod_radius_blocks": 320,
                    "ready_prompt_frame_offset_ns": 3_434_526_750,
                    "playable_sphere_first_ready_offset_ns": 14_323_644_834,
                    "initial_deadline_missed": False,
                    "playable_deadline_missed": False,
                    "movement_playable_sphere_complete": False,
                    "movement_visual_lod_samples_complete": False,
                },
                "frame_budget": {
                    "target_frame_duration_ns": 8_333_333,
                    "required_in_budget_time_permille": 990,
                    "measured_time_ns": 30_000_000_000,
                    "in_budget_time_ns": 14_018_509_225,
                    "passed": False,
                },
            },
        }
        facts = self.gate.verify_s1_owner_accepted_report_failure(report)
        self.assertEqual(facts["report_failure"], report["failure"])
        self.assertEqual(facts["in_budget_time_ns"], 14_018_509_225)
        self.assertFalse(facts["movement_playable_sphere_complete"])
        self.assertFalse(facts["movement_visual_lod_samples_complete"])

        report["game_benchmark"]["s1_terrain_performance"]["actual_radius_blocks"] = 639
        with self.assertRaisesRegex(self.gate.GameplayError, "owner-accepted failure metrics"):
            self.gate.verify_s1_owner_accepted_report_failure(report)
        report["game_benchmark"]["s1_terrain_performance"]["actual_radius_blocks"] = 640

        report["game_benchmark"]["frame_budget"]["passed"] = True
        with self.assertRaisesRegex(self.gate.GameplayError, "owner-accepted failure metrics"):
            self.gate.verify_s1_owner_accepted_report_failure(report)

    def test_owner_source_reuse_certificate_pins_application_and_test_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            accepted = root / "accepted"
            current = root / "current"
            source_files = {
                "CMakeLists.txt": b"project(MinecraftClone\nVERSION 0.1.0\n)\n",
                "vcpkg.json": b'{"version-string": "0.1.0"}\n',
                "src/shared/include/shared/ProjectInfo.hpp": (
                    b'MINOR_VERSION_NAME{ "Initiation" }\n.minor = 0,\n.patch = 7,\n'
                ),
                "src/shared/corelang/Execution.cpp": b"int execute() { return 1; }\n\n\n",
                "tests/acceptance/column_map_replay_tests.cpp": b"int replay() { return 1; }\n\n",
                "src/shared/corelang/Other.cpp": b"int other() { return 1; }\n",
                "tests/client/renderer_smoke_tests.cpp": b"old fixture\n",
            }
            for relative, contents in source_files.items():
                for base in (accepted, current):
                    target = base / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(contents)
            normalized_paths = (
                "src/shared/corelang/Execution.cpp",
                "tests/acceptance/column_map_replay_tests.cpp",
            )
            for relative in normalized_paths:
                target = current / relative
                target.write_bytes(target.read_bytes().rstrip(b"\n") + b"\n")
            fixture_path = "tests/client/renderer_smoke_tests.cpp"
            (current / fixture_path).write_bytes(b"reviewed fixture update\n")
            version_paths = tuple(sorted(self.gate.S1_OWNER_SOURCE_REUSE_VERSION_REPLACEMENTS))
            for relative in version_paths:
                target = current / relative
                contents = target.read_bytes()
                for before, after in self.gate.S1_OWNER_SOURCE_REUSE_VERSION_REPLACEMENTS[relative]:
                    contents = contents.replace(before, after, 1)
                target.write_bytes(contents)
            patterns = ("src/**", "tests/**", "CMakeLists.txt", "vcpkg.json")
            accepted_digest = self.gate._MATRIX._input_files_sha256(accepted, patterns)
            current_digest = self.gate._MATRIX._input_files_sha256(current, patterns)
            file_hashes = {
                relative: {
                    "accepted_sha256": self.gate.hashlib.sha256((accepted / relative).read_bytes()).hexdigest(),
                    "current_sha256": self.gate.hashlib.sha256((current / relative).read_bytes()).hexdigest(),
                }
                for relative in normalized_paths
            }
            test_fixture_hashes = {
                fixture_path: {
                    "accepted_sha256": self.gate.hashlib.sha256(
                        (accepted / fixture_path).read_bytes()
                    ).hexdigest(),
                    "current_sha256": self.gate.hashlib.sha256(
                        (current / fixture_path).read_bytes()
                    ).hexdigest(),
                }
            }
            version_file_hashes = {
                relative: {
                    "accepted_sha256": self.gate.hashlib.sha256((accepted / relative).read_bytes()).hexdigest(),
                    "current_sha256": self.gate.hashlib.sha256((current / relative).read_bytes()).hexdigest(),
                }
                for relative in version_paths
            }
            application_digest = self.gate.hashlib.sha256()
            for relative in sorted(source_files):
                if relative.startswith("tests/"):
                    continue
                contents = (accepted / relative).read_bytes()
                application_digest.update(relative.encode("utf-8"))
                application_digest.update(b"\0")
                application_digest.update(len(contents).to_bytes(8, byteorder="big"))
                application_digest.update(contents)

            build_evidence = root / "build-evidence"
            build_evidence.mkdir()
            executable = build_evidence / "mc_main"
            executable.write_bytes(b"accepted rebuilt executable")
            build_log = build_evidence / "accepted-build.log"
            build_log.write_bytes(b"accepted build log\n")
            executable_sha256 = self.gate.hashlib.sha256(executable.read_bytes()).hexdigest()
            freeze_receipt = build_evidence / "freeze.json"
            freeze_receipt.write_text(json.dumps({
                "build_exit": 0,
                "source_scope_drift": [],
                "bit_identical_to_accepted_mac": True,
                "mc_main_sha256": executable_sha256,
            }), encoding="utf-8")
            patches = (
                mock.patch.object(self.gate, "SOURCE_INPUTS", patterns),
                mock.patch.object(self.gate, "REQUIRED_SOURCE_INPUTS", tuple(source_files)),
                mock.patch.object(self.gate, "S1_OWNER_ACCEPTED_SOURCE_ROOT", accepted),
                mock.patch.object(self.gate, "S1_OWNER_SOURCE_INPUT_SHA256", accepted_digest),
                mock.patch.object(self.gate, "S1_OWNER_SOURCE_REUSE_V3_CURRENT_SHA256", current_digest),
                mock.patch.object(self.gate, "S1_OWNER_SOURCE_REUSE_FILE_HASHES", file_hashes),
                mock.patch.object(self.gate, "S1_OWNER_SOURCE_REUSE_TEST_FIXTURE_HASHES", test_fixture_hashes),
                mock.patch.object(self.gate, "S1_OWNER_SOURCE_REUSE_VERSION_FILES", version_file_hashes),
                mock.patch.object(self.gate, "S1_OWNER_SOURCE_INPUT_COUNT", len(source_files)),
                mock.patch.object(self.gate, "S1_OWNER_SOURCE_REUSE_UNCHANGED_INPUT_COUNT", 1),
                mock.patch.object(self.gate, "S1_OWNER_SOURCE_REUSE_APPLICATION_INPUT_COUNT", 5),
                mock.patch.object(self.gate, "S1_OWNER_SOURCE_REUSE_APPLICATION_UNCHANGED_INPUT_COUNT", 1),
                mock.patch.object(self.gate, "S1_OWNER_SOURCE_REUSE_TEST_INPUT_COUNT", 2),
                mock.patch.object(self.gate, "S1_OWNER_SOURCE_REUSE_TEST_UNCHANGED_INPUT_COUNT", 0),
                mock.patch.object(self.gate, "S1_OWNER_SOURCE_REUSE_APPLICATION_SHA256", application_digest.hexdigest()),
                mock.patch.object(self.gate, "S1_OWNER_SOURCE_REUSE_CURRENT_MAIN_PATH", executable),
                mock.patch.object(self.gate, "S1_OWNER_SOURCE_REUSE_FREEZE_RECEIPT_PATH", freeze_receipt),
                mock.patch.object(self.gate, "S1_OWNER_SOURCE_REUSE_BUILD_LOG_PATH", build_log),
                mock.patch.object(self.gate, "S1_OWNER_SOURCE_REUSE_CURRENT_MAIN_SHA256", executable_sha256),
                mock.patch.object(
                    self.gate, "S1_OWNER_SOURCE_REUSE_FREEZE_RECEIPT_SHA256",
                    self.gate.hashlib.sha256(freeze_receipt.read_bytes()).hexdigest(),
                ),
                mock.patch.object(
                    self.gate, "S1_OWNER_SOURCE_REUSE_BUILD_LOG_SHA256",
                    self.gate.hashlib.sha256(build_log.read_bytes()).hexdigest(),
                ),
                mock.patch.object(
                    self.gate,
                    "s1_owner_source_reuse_version_identity_build_evidence",
                    return_value={"schema": "mc.s1-version-identity-build.v1", "fixture": True},
                ),
            )
            with contextlib.ExitStack() as stack:
                for patcher in patches:
                    stack.enter_context(patcher)
                certificate = self.gate.s1_owner_source_reuse_certificate(current, current_digest)
                self.assertEqual(certificate["capture_source_input_sha256"], accepted_digest)
                self.assertEqual(certificate["current_source_input_sha256"], current_digest)
                self.assertEqual(certificate["unchanged_input_count"], 1)
                self.assertEqual(certificate["application_input_count"], 5)
                self.assertEqual(certificate["application_unchanged_input_count"], 1)
                self.assertEqual([item["path"] for item in certificate["version_only_files"]], list(version_paths))
                self.assertEqual(certificate["application_source_input_sha256"], application_digest.hexdigest())
                self.assertNotEqual(
                    certificate["application_current_raw_input_sha256"], application_digest.hexdigest()
                )
                self.assertEqual(certificate["test_input_count"], 2)
                self.assertEqual(certificate["test_unchanged_input_count"], 0)
                self.assertEqual(certificate["application_source_input_sha256"], application_digest.hexdigest())
                self.assertEqual(certificate["rebuilt_application"]["application_executable"]["sha256"], executable_sha256)
                self.assertEqual(certificate["version_identity_rebuild"], {"schema": "mc.s1-version-identity-build.v1", "fixture": True})

                project_info = current / "src/shared/include/shared/ProjectInfo.hpp"
                accepted_identity_bytes = project_info.read_bytes()
                project_info.write_bytes(accepted_identity_bytes.replace(b".patch = 1,", b".patch = 2,"))
                changed_digest = self.gate._MATRIX._input_files_sha256(current, patterns)
                changed_version_hashes = {
                    **version_file_hashes,
                    "src/shared/include/shared/ProjectInfo.hpp": {
                        **version_file_hashes["src/shared/include/shared/ProjectInfo.hpp"],
                        "current_sha256": self.gate.hashlib.sha256(project_info.read_bytes()).hexdigest(),
                    },
                }
                with (
                    mock.patch.object(self.gate, "S1_OWNER_SOURCE_REUSE_V3_CURRENT_SHA256", changed_digest),
                    mock.patch.object(self.gate, "S1_OWNER_SOURCE_REUSE_VERSION_FILES", changed_version_hashes),
                ):
                    with self.assertRaisesRegex(self.gate.GameplayError, "contains a non-version change"):
                        self.gate.s1_owner_source_reuse_certificate(current, changed_digest)
                project_info.write_bytes(accepted_identity_bytes)

                changed_test = current / fixture_path
                changed_test.write_bytes(b"unreviewed test behavior change\n")
                changed_digest = self.gate._MATRIX._input_files_sha256(current, patterns)
                with mock.patch.object(self.gate, "S1_OWNER_SOURCE_REUSE_V3_CURRENT_SHA256", changed_digest):
                    with self.assertRaisesRegex(self.gate.GameplayError, "exact owner-reviewed update"):
                        self.gate.s1_owner_source_reuse_certificate(current, changed_digest)

                changed = current / normalized_paths[0]
                changed.write_bytes(b"int execute() { return 2; }\n")
                changed_digest = self.gate._MATRIX._input_files_sha256(current, patterns)
                with mock.patch.object(self.gate, "S1_OWNER_SOURCE_REUSE_V3_CURRENT_SHA256", changed_digest):
                    with self.assertRaisesRegex(self.gate.GameplayError, "authorized EOF-only normalization"):
                        self.gate.s1_owner_source_reuse_certificate(current, changed_digest)

    def test_owner_version_identity_build_evidence_pins_rebuild_and_symbol_surface(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            worktree = root / "metadata-work"
            app = worktree / "build/s1-version-identity-release/mc_main"
            app.parent.mkdir(parents=True)
            evidence = root / "evidence"
            evidence.mkdir()
            build_log = evidence / "build.log"
            build_log.write_text("[100/100] Linking CXX executable mc_main\n", encoding="utf-8")
            symbols = b"_main\n_shared_version\n"
            accepted_symbols = evidence / "accepted-global-symbols.txt"
            current_symbols = evidence / "current-global-symbols.txt"
            accepted_symbols.write_bytes(symbols)
            current_symbols.write_bytes(symbols)
            app.write_bytes(b"version identity app")
            probe = evidence / "version_identity_probe"
            probe.write_bytes(b"version probe")
            probe_run_log = evidence / "version_identity_probe_run.log"
            probe_run_log.write_text("0.1.1:1 BasicBlocks\n", encoding="utf-8")
            sha = lambda path: self.gate.hashlib.sha256(path.read_bytes()).hexdigest()
            symbol_hash = sha(current_symbols)
            receipt = {
                "schema": "mc.s1-version-identity-build.v1",
                "build_exit": 0,
                "source_root": str(worktree),
                "source_input_sha256": self.gate.S1_OWNER_SOURCE_REUSE_V3_CURRENT_SHA256,
                "accepted_capture_source_input_sha256": self.gate.S1_OWNER_SOURCE_INPUT_SHA256,
                "application_behavior_input_sha256_after_exact_identity_normalization":
                    self.gate.S1_OWNER_SOURCE_REUSE_APPLICATION_SHA256,
                "build_configuration": {
                    "configuration": "Release", "target": "mc_main", "arm64_triplet": "arm64-osx",
                    "sanitizers": False, "high_assertions": False,
                },
                "accepted_application": {
                    "sha256": self.gate.S1_OWNER_SOURCE_REUSE_CURRENT_MAIN_SHA256,
                    "global_symbol_sha256": symbol_hash, "symbol_list_sha256": symbol_hash,
                },
                "version_identity_application": {
                    "path": str(app), "sha256": sha(app), "global_symbol_set_identical": True,
                    "global_symbol_sha256": symbol_hash, "symbol_list_sha256": symbol_hash,
                    "symbol_count": 3407,
                },
                "version_identity_probe": {
                    "compile_exit": 0, "run_exit": 0, "formatted_output": "0.1.1:1 BasicBlocks",
                    "executable_sha256": sha(probe), "run_log_sha256": sha(probe_run_log),
                },
            }
            receipt_path = evidence / "version-identity-build.json"
            receipt_path.write_text(json.dumps(receipt, sort_keys=True) + "\n", encoding="utf-8")
            patches = (
                mock.patch.object(self.gate, "S1_OWNER_VERSION_IDENTITY_APP_PATH", app),
                mock.patch.object(self.gate, "S1_OWNER_VERSION_IDENTITY_BUILD_LOG_PATH", build_log),
                mock.patch.object(self.gate, "S1_OWNER_VERSION_IDENTITY_BUILD_RECEIPT_PATH", receipt_path),
                mock.patch.object(self.gate, "S1_OWNER_VERSION_IDENTITY_SYMBOLS_PATH", current_symbols),
                mock.patch.object(self.gate, "S1_OWNER_ACCEPTED_SYMBOLS_PATH", accepted_symbols),
                mock.patch.object(self.gate, "S1_OWNER_VERSION_IDENTITY_PROBE_PATH", probe),
                mock.patch.object(self.gate, "S1_OWNER_VERSION_IDENTITY_PROBE_RUN_LOG_PATH", probe_run_log),
                mock.patch.object(self.gate, "S1_OWNER_VERSION_IDENTITY_BUILD_RECEIPT_SHA256", sha(receipt_path)),
                mock.patch.object(self.gate, "S1_OWNER_VERSION_IDENTITY_BUILD_LOG_SHA256", sha(build_log)),
                mock.patch.object(self.gate, "S1_OWNER_VERSION_IDENTITY_APP_SHA256", sha(app)),
                mock.patch.object(self.gate, "S1_OWNER_VERSION_IDENTITY_SYMBOLS_SHA256", symbol_hash),
                mock.patch.object(self.gate, "S1_OWNER_VERSION_IDENTITY_PROBE_SHA256", sha(probe)),
                mock.patch.object(self.gate, "S1_OWNER_VERSION_IDENTITY_PROBE_RUN_LOG_SHA256", sha(probe_run_log)),
            )
            with contextlib.ExitStack() as stack:
                for patcher in patches:
                    stack.enter_context(patcher)
                proof = self.gate.s1_owner_source_reuse_version_identity_build_evidence()
                self.assertEqual(proof["version_identity_application"]["sha256"], sha(app))
                self.assertTrue(proof["global_symbols"]["identical_to_owner_tested_application"])
                self.assertEqual(proof["formatted_version_probe"]["output"], "0.1.1:1 BasicBlocks")

                original_receipt_bytes = receipt_path.read_bytes()
                receipt["source_input_sha256"] = self.gate.S1_OWNER_SOURCE_REUSE_CURRENT_SHA256
                receipt_path.write_text(json.dumps(receipt, sort_keys=True) + "\n", encoding="utf-8")
                with mock.patch.object(
                    self.gate, "S1_OWNER_VERSION_IDENTITY_BUILD_RECEIPT_SHA256", sha(receipt_path)
                ):
                    with self.assertRaisesRegex(self.gate.GameplayError, "exact version candidate"):
                        self.gate.s1_owner_source_reuse_version_identity_build_evidence()
                receipt_path.write_bytes(original_receipt_bytes)

                current_symbols.write_bytes(b"changed\n")
                with self.assertRaisesRegex(self.gate.GameplayError, "current_symbol_list differs from the pinned"):
                    self.gate.s1_owner_source_reuse_version_identity_build_evidence()

    def test_owner_accepted_aggregate_is_exact_and_leaves_three_proofs_unqualified(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            aggregate = self.aggregate(root)
            row = aggregate["rows"][0]
            report_path = Path(row["report"]["path"])
            report = {
                "schema_version": "mc.runtime-evidence.v1",
                "mode": "benchmark-game",
                "passed": False,
                "failure": "game benchmark did not complete the scripted full-game workload",
                "timing": {"elapsed_ms": 45_705, "deadline_ms": 60_000},
                "game_benchmark": {
                    "workload": self.gate.WORKLOAD,
                    "build_mode": "release",
                    "qualification_proof": "performance",
                    "s1_terrain_performance": {
                        "contract": self.gate.S1_PERFORMANCE_CONTRACT,
                        "initial_radius_blocks": 48,
                        "actual_radius_blocks": 640,
                        "visual_lod_radius_blocks": 320,
                        "ready_prompt_frame_offset_ns": 3_434_526_750,
                        "playable_sphere_first_ready_offset_ns": 14_323_644_834,
                        "initial_deadline_missed": False,
                        "playable_deadline_missed": False,
                        "movement_playable_sphere_complete": False,
                        "movement_visual_lod_samples_complete": False,
                    },
                    "frame_budget": {
                        "target_frame_duration_ns": 8_333_333,
                        "required_in_budget_time_permille": 990,
                        "measured_time_ns": 30_000_000_000,
                        "in_budget_time_ns": 14_018_509_225,
                        "passed": False,
                    },
                },
            }
            report_path.write_text(json.dumps(report), encoding="utf-8")
            row["report"] = self.gate.file_identity(report_path)
            row["exit_code"] = 1
            row["capture_error"] = ""
            aggregate["rows"] = [row]
            receipt = root / "owner-accepted-aggregate.json"
            receipt.write_text(json.dumps(aggregate, sort_keys=True), encoding="utf-8")

            executable = row["executable"]
            report_identity = row["report"]
            receipt_identity = self.gate.file_identity(receipt)
            source_sha256 = self.gate.source_digest(root)
            patches = (
                mock.patch.object(self.gate, "S1_OWNER_REQUEST_SHA256", "e" * 64),
                mock.patch.object(self.gate, "S1_OWNER_SOURCE_INPUT_SHA256", source_sha256),
                mock.patch.object(self.gate, "S1_OWNER_AGGREGATE_SHA256", receipt_identity["sha256"]),
                mock.patch.object(self.gate, "S1_OWNER_RAW_REPORT_SHA256", report_identity["sha256"]),
                mock.patch.object(self.gate, "S1_OWNER_MAC_EXECUTABLE_SHA256", executable["sha256"]),
                mock.patch.object(self.gate, "S1_OWNER_CAPTURE_CANDIDATE", CANDIDATE),
                mock.patch.object(self.gate._MATRIX, "commit_identity", return_value=CANDIDATE),
                mock.patch.object(self.gate, "verify_macos_runtime"),
            )
            with contextlib.ExitStack() as stack:
                for patcher in patches:
                    stack.enter_context(patcher)
                result = self.gate.verify_s1_owner_accepted_receipt(
                    root, receipt, candidate_commit=CANDIDATE["head"]
                )
                self.assertEqual(result["status"], "accepted_exception")
                self.assertEqual(result["candidate"], CANDIDATE)
                self.assertEqual(result["proof_dispositions"], {
                    "macos": {"performance": "failed", "terrain-coverage": "unqualified"},
                    "windows": {"performance": "unqualified", "terrain-coverage": "unqualified"},
                })
                self.assertEqual(result["known_failure"]["in_budget_time_ns"], 14_018_509_225)
                with self.assertRaisesRegex(self.gate.GameplayError, "both terrain-coverage and performance"):
                    self.gate.verify_receipt(root, receipt)

                row["exit_code"] = 0
                aggregate["rows"] = [row]
                receipt.write_text(json.dumps(aggregate, sort_keys=True), encoding="utf-8")
                with self.assertRaisesRegex(self.gate.GameplayError, "retained Mac record"):
                    self.gate.verify_s1_owner_accepted_receipt(
                        root, receipt, candidate_commit=CANDIDATE["head"]
                    )

    def test_owner_accepted_cli_routes_a_single_row_to_the_pinned_disposition(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            receipt = root / "aggregate.json"
            receipt.write_text(json.dumps({"schema_version": self.gate.SCHEMA, "rows": [{}]}),
                               encoding="utf-8")
            completed = mock.Mock(returncode=0, stdout="")
            disposition = {"schema_version": self.gate.S1_OWNER_EXCEPTION_SCHEMA,
                           "status": "accepted_exception"}
            with (
                mock.patch.object(self.gate._MATRIX, "commit_identity", return_value=CANDIDATE),
                mock.patch.object(self.gate.subprocess, "run", return_value=completed),
                mock.patch.object(self.gate, "verify_s1_owner_accepted_receipt",
                                  return_value=disposition) as exception,
                contextlib.redirect_stdout(io.StringIO()) as output,
            ):
                result = self.gate.main([
                    "--root", str(root), "--receipt", str(receipt),
                    "--candidate-commit", CANDIDATE["head"], "--s1-owner-accepted",
                ])
            self.assertEqual(result, 0)
            exception.assert_called_once_with(
                root.resolve(), receipt.resolve(), candidate_commit=CANDIDATE["head"]
            )
            self.assertEqual(json.loads(output.getvalue())["status"], "accepted_exception")

    def test_s1_performance_accepts_exact_readiness_and_30_second_window(self):
        for platform in ("macos", "windows"):
            with self.subTest(platform=platform):
                report = self.report(platform)
                fill_inputs = [entry for entry in report["game_benchmark"]["raw_gameplay_inputs"]
                               if entry["sent_offset_ns"] + report["game_benchmark"]["controlled_start"][
                                   "server_start_acknowledged_offset_ns"]
                               < report["game_benchmark"]["s1_terrain_performance"][
                                   "playable_sphere_first_ready_offset_ns"]]
                self.assertTrue(fill_inputs)
                self.assertTrue(all(not entry["direction"]["accelerated"]
                                    and entry["direction"]["speedup"] == 30 for entry in fill_inputs))
                verified = self.gate.verify_report(report, platform)
                self.assertEqual(verified["status"], "passed")
                self.assertEqual(verified["coverage"], "passed")
                self.assertEqual(report["game_benchmark"]["workload"], "turning-x30-movement-v3")
                self.assertFalse(verified["presentation_timing_required"])
                self.assertEqual(report["game_benchmark"]["frame_budget"]["required_window_duration_ns"],
                                 30_000_000_000)

    def test_s1_performance_rejects_stationary_or_unrelated_rendered_translation(self):
        for camera_xyz in ([0.0, 0.0, 64.0], [1e9, 1e9, 1e9]):
            with self.subTest(camera_xyz=camera_xyz):
                report = self.report()
                for frame in report["game_benchmark"]["raw_frames"]:
                    frame["diagnostic_camera"][:3] = camera_xyz
                with self.assertRaisesRegex(self.gate.GameplayError, "rendered camera"):
                    self.gate.verify_report(report, "macos")

    def test_s1_rendered_translation_keeps_body_eye_offsets_and_interpolation_across_wrap(self):
        report = self.report()
        game = report["game_benchmark"]
        full = game["s1_terrain_performance"]
        def move_position(position):
            position["x"] = (position["x"] + 65_520) % 65_536
        def move_sphere(sphere):
            for key in ("active_coverage_center", "world_loading_center", "authoritative_columns_center"):
                move_position(sphere[key])
        states = [full["movement_trajectory_anchor"], *full["movement_authoritative_sphere_updates"]]
        for state in states:
            move_position(state["authoritative_position"])
            move_sphere(state["playable_sphere"])
            move_position(state["visual_lod"]["coverage_center"])
            move_position(state["visual_lod"]["scene_center"])
        for frame in game["raw_frames"]:
            if frame.get("playable_sphere") is not None:
                move_sphere(frame["playable_sphere"])
                move_position(frame["visual_lod"]["coverage_center"])
                move_position(frame["visual_lod"]["scene_center"])
            frame["diagnostic_camera"][0] = (
                frame["diagnostic_camera"][0] - 0.3125 + 65_520 - 8
            ) % 65_536 + 0.3125
            if frame.get("presented_player") is not None:
                frame["presented_player"]["body"][0] = frame["diagnostic_camera"][0] - 0.3125
        self.assertEqual(self.gate.verify_report(report, "macos")["status"], "passed")

    def test_s1_performance_rejects_a_slow_rendered_view_despite_full_speed_authority(self):
        report = self.report()
        for frame in report["game_benchmark"]["raw_frames"]:
            frame["diagnostic_camera"][0] = (frame["diagnostic_camera"][0] - 0.3125) * 0.5 + 0.3125
            frame["diagnostic_camera"][1] = (frame["diagnostic_camera"][1] - 0.3125) * 0.5 + 0.3125
        with self.assertRaisesRegex(self.gate.GameplayError, "rendered camera"):
            self.gate.verify_report(report, "macos")

    def test_s1_performance_rejects_reduced_framebuffer_and_invalid_fov(self):
        for platform in ("macos", "windows"):
            with self.subTest(platform=platform, setting="framebuffer"):
                report = self.report(platform)
                report["game_benchmark"]["framebuffer"] = {"width": 1, "height": 1}
                with self.assertRaisesRegex(self.gate.GameplayError, "qualification|framebuffer"):
                    self.gate.verify_report(report, platform)
            with self.subTest(platform=platform, setting="fov"):
                report = self.report(platform)
                for frame in report["game_benchmark"]["raw_frames"]:
                    frame["diagnostic_camera"][6] = 0.0
                with self.assertRaisesRegex(self.gate.GameplayError, "qualification|FOV|projection"):
                    self.gate.verify_report(report, platform)

    def test_s1_performance_rejects_a_nonempty_scene_without_actual_terrain_coverage(self):
        report = self.report()
        game = report["game_benchmark"]
        full = game["s1_terrain_performance"]
        samples = [frame["visual_lod"] for frame in game["raw_frames"]]
        samples += [full["movement_trajectory_anchor"]["visual_lod"]]
        samples += [update["visual_lod"] for update in full["movement_authoritative_sphere_updates"]]
        for visual in samples:
            visual.update(scene_active=True, leaf_count=1, actual_column_count=0)
        with self.assertRaisesRegex(self.gate.GameplayError, "visual|coverage"):
            self.gate.verify_report(report, "macos")

    def test_s1_performance_rejects_changed_observed_settings_or_old_receipts(self):
        for setting, value in (("debug_hud_enabled", False), ("render_distance", 1),
                               ("framebuffer", {"width": 1, "height": 1}),
                               ("camera_perspective", "third-person"), ("world_mode", "flat")):
            with self.subTest(setting=setting):
                report = self.report()
                frame = next(frame for frame in report["game_benchmark"]["raw_frames"]
                             if frame["phase"] == self.gate.S1_MOVEMENT_PHASE)
                frame["qualification_settings"][setting] = value
                with self.assertRaisesRegex(self.gate.GameplayError, "qualification"):
                    self.gate.verify_report(report, "macos")
        report = self.report()
        del report["game_benchmark"]["qualification_settings_manifest"]
        with self.assertRaisesRegex(self.gate.GameplayError, "qualification"):
            self.gate.verify_report(report, "macos")
        report = self.report()
        report["game_benchmark"]["terrain_identity"]["configuration_identity"] = "cheaper-generation"
        with self.assertRaisesRegex(self.gate.GameplayError, "generation settings"):
            self.gate.verify_report(report, "macos")

    def test_s1_performance_rejects_detached_or_stale_submitted_player_receipts(self):
        for mutation in ("missing", "body", "submission", "time"):
            with self.subTest(mutation=mutation):
                report = self.report()
                frame = next(frame for frame in report["game_benchmark"]["raw_frames"]
                             if frame["phase"] == self.gate.S1_MOVEMENT_PHASE)
                if mutation == "missing":
                    del frame["presented_player"]
                elif mutation == "body":
                    frame["presented_player"]["body"][0] += 1.0
                elif mutation == "submission":
                    frame["presented_player"]["submitted_frame_count"] += 1
                else:
                    frame["presented_player"]["sampled_offset_ns"] = frame["started_offset_ns"] - 1
                with self.assertRaisesRegex(self.gate.GameplayError, "rendered camera"):
                    self.gate.verify_report(report, "macos")

    def test_s1_performance_rejects_unbound_or_incomplete_presented_scene_receipts(self):
        mutations = (("presented_current_actual_columns", 6560), ("sources_current", False),
                     ("session_join_epoch", 2), ("coverage_epoch", 1000000),
                     ("submitted_frame_count", 0), ("leaf_count", 1),
                     ("coverage_center", {"x": 10000, "y": 0, "z": 96,
                                          "x_subcell": 0, "y_subcell": 0, "z_subcell": 0}))
        for key, value in mutations:
            with self.subTest(field=key):
                report = self.report()
                visual = report["game_benchmark"]["s1_terrain_performance"]["movement_trajectory_anchor"]["visual_lod"]
                visual[key] = value
                with self.assertRaisesRegex(self.gate.GameplayError, "visual|coverage|presented|scene"):
                    self.gate.verify_report(report, "macos")
        report = self.report()
        visual = report["game_benchmark"]["s1_terrain_performance"]["movement_trajectory_anchor"]["visual_lod"]
        visual["terrain_identity"]["seed"] += 1
        with self.assertRaisesRegex(self.gate.GameplayError, "identity|session"):
            self.gate.verify_report(report, "macos")
        report = self.report()
        visual = report["game_benchmark"]["s1_terrain_performance"]["movement_trajectory_anchor"]["visual_lod"]
        del visual["contract"]
        with self.assertRaisesRegex(self.gate.GameplayError, "visual"):
            self.gate.verify_report(report, "macos")

    def test_s1_performance_rejects_inactive_or_empty_required_visual_samples(self):
        for location in ("movement", "authoritative", "anchor", "boundary"):
            for field, value in (("scene_active", False), ("leaf_count", 0)):
                with self.subTest(location=location, field=field):
                    report = self.report()
                    game = report["game_benchmark"]
                    full = game["s1_terrain_performance"]
                    if location == "movement":
                        visual = next(frame["visual_lod"] for frame in game["raw_frames"]
                                      if frame["phase"] == self.gate.S1_MOVEMENT_PHASE)
                    elif location == "authoritative":
                        visual = full["movement_authoritative_sphere_updates"][0]["visual_lod"]
                    elif location == "anchor":
                        visual = full["movement_trajectory_anchor"]["visual_lod"]
                    else:
                        self.add_delayed_boundary_ack(report)
                        visual = full["movement_boundary_authoritative_updates"][0]["visual_lod"]
                    visual[field] = value
                    with self.assertRaisesRegex(self.gate.GameplayError, "visual LOD scene must be active and nonempty"):
                        self.gate.verify_report(report, "macos")

    def scheduler_boundary_report(self):
        report = self.report()
        self.reconcile_movement_playable_sphere(report, step_count=301)
        game = report["game_benchmark"]
        full = game["s1_terrain_performance"]
        start = game["frame_budget"]["measurement_window_start_offset_ns"]
        route = [entry for entry in game["raw_gameplay_inputs"] if entry["sent_offset_ns"] >= start]
        for index, raw_input in enumerate(route):
            scheduled = start - 385_125 + index * self.gate.SERVER_TICK_NS
            raw_input["scheduled_offset_ns"] = scheduled
            raw_input["sent_offset_ns"] = start if index == 0 else scheduled
            full["movement_authoritative_sphere_updates"][index]["offset_ns"] = raw_input["sent_offset_ns"] + 10_000_000
        return report

    def test_s1_scheduler_boundary_preserves_301st_input_and_bounded_ack_tail(self):
        report = self.scheduler_boundary_report()
        full = report["game_benchmark"]["s1_terrain_performance"]
        self.assertGreater(full["movement_authoritative_sphere_updates"][-1]["offset_ns"],
                           report["game_benchmark"]["frame_budget"]["measurement_window_end_offset_ns"])
        self.assertEqual(self.gate.verify_report(report, "macos")["status"], "passed")

    def test_s1_scheduler_boundary_rejects_missing_internal_update(self):
        report = self.scheduler_boundary_report()
        full = report["game_benchmark"]["s1_terrain_performance"]
        del full["movement_authoritative_sphere_updates"][150]
        full["movement_authoritative_update_count"] -= 1
        with self.assertRaisesRegex(self.gate.GameplayError, "scheduled movement steps"):
            self.gate.verify_report(report, "macos")

    def test_s1_scheduler_boundary_keeps_last_unsafe_sphere_observation(self):
        report = self.scheduler_boundary_report()
        full = report["game_benchmark"]["s1_terrain_performance"]
        full["movement_authoritative_sphere_updates"][-1]["playable_sphere"]["ready_chunks"] -= 1
        with self.assertRaisesRegex(self.gate.GameplayError, "materialized-chunk deficit"):
            self.gate.verify_report(report, "macos")

    def test_s1_full_volume_rejects_old_radius_scope_missing_edge_and_stale_identity(self):
        mutations = (("radius_blocks", 40), ("coverage_scope", "materialized_sphere"),
                     ("resident_authoritative_columns", 6560), ("active_coverage_epoch", 0),
                     ("vertical_chunk_count", 1))
        for field, value in mutations:
            with self.subTest(field=field):
                report = self.report()
                frame = next(frame for frame in report["game_benchmark"]["raw_frames"]
                             if frame["phase"] == self.gate.S1_MOVEMENT_PHASE)
                frame["playable_sphere"][field] = value
                with self.assertRaises(self.gate.GameplayError):
                    self.gate.verify_report(report, "macos")
        wrong_center = self.report()
        sphere = next(frame["playable_sphere"] for frame in wrong_center["game_benchmark"]["raw_frames"]
                      if frame["phase"] == self.gate.S1_MOVEMENT_PHASE)
        sphere["authoritative_columns_center"] = {**sphere["world_loading_center"], "x": 1024}
        with self.assertRaises(self.gate.GameplayError):
            self.gate.verify_report(wrong_center, "macos")

    def test_s1_performance_rejects_missing_contract_instead_of_legacy_report(self):
        report = self.report()
        del report["game_benchmark"]["s1_terrain_performance"]
        report["game_benchmark"]["full_radius_terrain"] = {"complete": True}
        with self.assertRaisesRegex(self.gate.GameplayError, "S1 terrain-performance evidence is absent"):
            self.gate.verify_report(report, "macos")

        incomplete = self.report()
        incomplete["game_benchmark"]["s1_terrain_performance"]["complete"] = False
        with self.assertRaisesRegex(self.gate.GameplayError, "deadline, movement start, or complete-sphere evidence failed"):
            self.gate.verify_report(incomplete, "macos")

    def test_s1_performance_rejects_prompt_and_full_volume_deadline_misses(self):
        prompt_late = self.report()
        prompt_late["game_benchmark"]["s1_terrain_performance"]["ready_prompt_frame_offset_ns"] = 4_000_000_001
        prompt_late["game_benchmark"]["controlled_start"]["eligible_prompt_rendered_offset_ns"] = 4_000_000_001
        with self.assertRaisesRegex(self.gate.GameplayError, "Ready prompt missed 4 seconds"):
            self.gate.verify_report(prompt_late, "macos")

        sphere_late = self.report("windows")
        sphere_late["game_benchmark"]["s1_terrain_performance"]["playable_sphere_first_ready_offset_ns"] = 15_000_000_001
        with self.assertRaisesRegex(self.gate.GameplayError, "actual 640-block terrain sphere missed the 15-second deadline"):
            self.gate.verify_report(sphere_late, "windows")

    def test_s1_performance_rejects_incomplete_current_sphere_and_fill_flag_drift(self):
        sphere_missing = self.report()
        movement = next(frame for frame in sphere_missing["game_benchmark"]["raw_frames"]
                        if frame["phase"] == self.gate.S1_MOVEMENT_PHASE)
        movement["playable_sphere"]["ready_chunks"] -= 1
        with self.assertRaisesRegex(self.gate.GameplayError, "materialized-chunk deficit"):
            self.gate.verify_report(sphere_missing, "macos")

        fill_flag_lies = self.report()
        fill_flag_lies["game_benchmark"]["s1_terrain_performance"]["movement_input_during_fill"] = False
        with self.assertRaisesRegex(self.gate.GameplayError, "fill-ramp input flag disagrees"):
            self.gate.verify_report(fill_flag_lies, "macos")

        fill_suppressed = self.report()
        game = fill_suppressed["game_benchmark"]
        movement_start = game["frame_budget"]["measurement_window_start_offset_ns"]
        game["raw_gameplay_inputs"] = [entry for entry in game["raw_gameplay_inputs"]
                                        if entry["sent_offset_ns"] >= movement_start]
        for ordinal, raw_input in enumerate(game["raw_gameplay_inputs"], 1):
            raw_input["input_ordinal"] = ordinal
        game["inputs_sent"] = len(game["raw_gameplay_inputs"])
        game["s1_terrain_performance"]["movement_input_during_fill"] = False
        with self.assertRaisesRegex(self.gate.GameplayError, "omitted its first x30 movement input during the initial fill"):
            self.gate.verify_report(fill_suppressed, "macos")

        fill_not_moving = self.report()
        fill_frames = [frame for frame in fill_not_moving["game_benchmark"]["raw_frames"]
                       if frame["phase"] == self.gate.S1_FILL_PHASE]
        self.assertTrue(fill_frames)
        for frame in fill_frames:
            frame["playable_sphere"]["world_loading_center"] = {
                "x": 0, "y": 0, "z": 64, "x_subcell": 0, "y_subcell": 0, "z_subcell": 0}
            frame["playable_sphere"]["active_coverage_center"] = {
                "x": frame["loop_ordinal"], "y": 0, "z": 64,
                "x_subcell": 0, "y_subcell": 0, "z_subcell": 0}
        with self.assertRaisesRegex(self.gate.GameplayError, "lack authoritative player displacement"):
            self.gate.verify_report(fill_not_moving, "macos")

    def test_s1_actual_readiness_can_precede_prompt_with_independent_t0_deadlines(self):
        ready_before_first_input = self.report()
        game = ready_before_first_input["game_benchmark"]
        s1 = game["s1_terrain_performance"]
        old_start = game["frame_budget"]["measurement_window_start_offset_ns"]
        # The actual sphere can become Ready before both the prompt and start key.
        # The measured window must still begin at the first eligible input.
        s1["playable_sphere_first_ready_offset_ns"] = 40_000_000
        s1["movement_start_offset_ns"] = 300_000_000
        s1["movement_end_offset_ns"] = 30_300_000_000
        game["raw_frames"] = [frame for frame in game["raw_frames"]
                              if frame["phase"] == self.gate.S1_MOVEMENT_PHASE]
        for ordinal, frame in enumerate(game["raw_frames"], 1):
            frame["loop_ordinal"] = ordinal
            frame["started_offset_ns"] -= old_start
        game["failed_present_requests"] = sum(not frame["presentation_succeeded"]
                                              for frame in game["raw_frames"])
        game["successful_present_requests"] = len(game["raw_frames"]) - game["failed_present_requests"]
        self.reconcile_submission_identity(game["raw_frames"])
        game["raw_fence_confirmed_frames"] = self.raw_fence_confirmed_frames(game["raw_frames"])
        game["frame_budget"] = self.frame_budget(
            game["raw_frames"], "performance", game["raw_fence_confirmed_frames"], "macos")
        self.reconcile_movement_playable_sphere(ready_before_first_input, fill_count=0)
        self.assertEqual(self.gate.verify_report(ready_before_first_input, "macos")["status"], "passed")

    def test_s1_movement_start_is_bounded_from_later_ready_or_ack(self):
        report = self.report()
        s1 = report["game_benchmark"]["s1_terrain_performance"]
        s1["playable_sphere_first_ready_offset_ns"] = 250_000_000
        s1["movement_start_offset_ns"] = 400_000_001
        s1["movement_end_offset_ns"] = 30_400_000_001
        with self.assertRaisesRegex(self.gate.GameplayError,
                                    "measured movement did not start at the first post-Ready input"):
            self.gate.verify_report(report, "macos")

    def test_batched_fence_observations_keep_zero_intervals_without_budget_credit(self):
        report = self.report()
        game = report["game_benchmark"]
        completions = game["raw_fence_confirmed_frames"]
        movement = [entry for entry in completions
                    if entry["submission_phase"] == self.gate.S1_MOVEMENT_PHASE]
        for index in range(0, len(movement) - 1, 2):
            movement[index]["completion_observed_offset_ns"] = movement[index + 1]["completion_observed_offset_ns"]
        budget = self.frame_budget(game["raw_frames"], "performance", completions)
        self.assertFalse(budget["passed"])
        self.assertLess(budget["in_budget_time_ns"], budget["measured_time_ns"] // 100)
        self.assertEqual(budget["sample_count"], budget["fence_confirmed_frame_count"])
        self.assertFalse(self.gate.verify_fresh_frame_budget(
            budget, game["raw_frames"], completions, game["frame_completion_source"], True,
            "macos", movement_phase=self.gate.S1_MOVEMENT_PHASE,
            expected_start_ns=budget["measurement_window_start_offset_ns"]))

    def test_s1_performance_rejects_sparse_fence_completions_and_legacy_cadence_exception(self):
        report = self.report()
        game = report["game_benchmark"]
        game["raw_fence_confirmed_frames"] = game["raw_fence_confirmed_frames"][::500]
        game["frame_budget"] = self.frame_budget(
            game["raw_frames"], "performance", game["raw_fence_confirmed_frames"], "macos")
        with self.assertRaisesRegex(self.gate.GameplayError, "fence-confirmed frame window"):
            self.gate.verify_report(report, "macos")

        with self.assertRaisesRegex(self.gate.GameplayError, "does not accept legacy.*cadence exceptions"):
            self.gate.verify_report(self.report(), "macos", cadence_exception={"release": "EarlyDev0.1.0-snapshot7"})

    def test_s1_performance_rejects_incorrect_window_end_and_sparse_authoritative_steps(self):
        short_window = self.report()
        short_window["game_benchmark"]["s1_terrain_performance"]["movement_end_offset_ns"] -= 100_000_000
        with self.assertRaisesRegex(self.gate.GameplayError, "last exactly 30 seconds"):
            self.gate.verify_report(short_window, "macos")

        sparse_steps = self.report()
        self.reconcile_movement_playable_sphere(sparse_steps, step_count=10)
        with self.assertRaisesRegex(self.gate.GameplayError, "requires 300 scheduled movement steps"):
            self.gate.verify_report(sparse_steps, "macos")

    def test_slow_mac_submission_proxy_is_diagnostic_for_coverage(self):
        report = self.report(interval_ns=10000000, proof="terrain-coverage")
        verified = self.gate.verify_report(report, "macos")
        self.assertEqual(verified["coverage"], "passed")
        self.assertEqual(verified["request_cadence_status"], "failed")
        self.assertFalse(verified["request_cadence_required"])
        self.assertEqual(verified["physical_display_cadence"], "unavailable")
        for phase in (self.gate.PHASES[1], self.gate.PHASES[2], self.gate.PHASES[4]):
            self.assertEqual(verified["request_cadence_proxy"][phase]["present_requests_per_second"], 100)
            self.assertTrue(any(reason.startswith(phase + ": successful-request rate") for reason in verified["request_cadence_reasons"]))
    def test_s1_performance_does_not_require_driver_timing_or_request_cadence(self):
        report = self.report(interval_ns=10000000, movement_interval_ns=8000000)
        verified = self.gate.verify_report(report, "macos")
        self.assertFalse(verified["request_cadence_required"])
        self.assertEqual(verified["request_cadence_status"], "not-required")
        self.assertFalse(verified["presentation_timing_required"])
        self.assertEqual(verified["presentation_timing_status"], "not-required")
        self.assertNotIn("presentation_timing", report["game_benchmark"])

    def test_mac_performance_rejects_failed_fresh_frame_budget_despite_driver_cadence(self):
        report = self.report(interval_ns=8000000, movement_interval_ns=10000000)
        game = report["game_benchmark"]
        with self.assertRaisesRegex(self.gate.GameplayError, "fresh-frame budget exceeds 1%"):
            self.gate.verify_report(report, "macos")

    def test_performance_rejects_missing_movement_playable_sphere_proof(self):
        report = self.report()
        report["game_benchmark"]["s1_terrain_performance"]["movement_authoritative_sphere_updates"] = []
        with self.assertRaisesRegex(self.gate.GameplayError, "update count does not match its raw samples"):
            self.gate.verify_report(report, "macos")

    def test_performance_rejects_materialized_sphere_deficit_during_movement(self):
        report = self.report()
        movement = next(frame for frame in report["game_benchmark"]["raw_frames"]
                        if frame["phase"] == self.gate.S1_MOVEMENT_PHASE)
        movement["playable_sphere"]["ready_chunks"] -= 1
        with self.assertRaisesRegex(self.gate.GameplayError, "materialized-chunk deficit"):
            self.gate.verify_report(report, "macos")

    def test_performance_rejects_authoritative_sphere_deficit_during_movement(self):
        report = self.report()
        update = report["game_benchmark"]["s1_terrain_performance"]["movement_authoritative_sphere_updates"][0]
        update["playable_sphere"]["ready"] = False
        with self.assertRaisesRegex(self.gate.GameplayError, "materialized-chunk deficit"):
            self.gate.verify_report(report, "macos")

    def test_x30_trajectory_rejects_slow_but_nonstationary_legacy_movement(self):
        report = self.report()
        game = report["game_benchmark"]
        inputs = game["raw_gameplay_inputs"]
        updates = game["s1_terrain_performance"]["movement_authoritative_sphere_updates"]
        anchor = game["s1_terrain_performance"]["movement_trajectory_anchor"]["authoritative_position"]
        position = {axis: anchor[axis] for axis in ("x", "y", "z", "x_subcell", "y_subcell", "z_subcell")}
        by_sequence = {entry["sequence"]: entry for entry in inputs}
        for update in updates:
            sent = by_sequence[update["acknowledged_input_sequence"]]
            direction = sent["direction"]
            direction["x"] = (direction["x"] > 0) - (direction["x"] < 0)
            direction["y"] = (direction["y"] > 0) - (direction["y"] < 0)
            squared = direction["x"] ** 2 + direction["y"] ** 2
            divisor = max(127, math.isqrt(squared) + (math.isqrt(squared) ** 2 < squared))
            deltas = [(5600 * abs(direction[axis]) // divisor * 30)
                      * (-1 if direction[axis] < 0 else 1) for axis in ("x", "y")]
            x = (position["x"] * 10000 + position["x_subcell"] + deltas[0]) % (65536 * 10000)
            y = (position["y"] * 10000 + position["y_subcell"] + deltas[1]) % (65536 * 10000)
            position = {"x": x // 10000, "x_subcell": x % 10000,
                        "y": y // 10000, "y_subcell": y % 10000,
                        "z": position["z"], "z_subcell": position["z_subcell"]}
            update["authoritative_position"] = dict(position)
            update["playable_sphere"]["world_loading_center"] = dict(position)
            update["playable_sphere"]["authoritative_columns_center"] = dict(position)
            update["playable_sphere"]["active_coverage_center"] = dict(position)
            update["visual_lod"]["coverage_center"] = dict(position)
        self.assertGreater(len({tuple(update["authoritative_position"].items()) for update in updates}), 1)
        with self.assertRaisesRegex(self.gate.GameplayError, "camera-relative keyboard input"):
            self.gate.verify_report(report, "macos")

    def test_x30_trajectory_rejects_slow_authority_steps_despite_full_scale_input_labels(self):
        report = self.report()
        game = report["game_benchmark"]
        updates = game["s1_terrain_performance"]["movement_authoritative_sphere_updates"]
        anchor = game["s1_terrain_performance"]["movement_trajectory_anchor"]["authoritative_position"]
        position = {axis: anchor[axis] for axis in ("x", "y", "z", "x_subcell", "y_subcell", "z_subcell")}
        by_sequence = {entry["sequence"]: entry for entry in game["raw_gameplay_inputs"]}
        for update in updates:
            direction = by_sequence[update["acknowledged_input_sequence"]]["direction"]
            slow_x = (direction["x"] > 0) - (direction["x"] < 0)
            slow_y = (direction["y"] > 0) - (direction["y"] < 0)
            deltas = [(5600 * abs(component) // 127 * 30) * (-1 if component < 0 else 1)
                      for component in (slow_x, slow_y)]
            x = (position["x"] * 10000 + position["x_subcell"] + deltas[0]) % (65536 * 10000)
            y = (position["y"] * 10000 + position["y_subcell"] + deltas[1]) % (65536 * 10000)
            position = {"x": x // 10000, "x_subcell": x % 10000,
                        "y": y // 10000, "y_subcell": y % 10000,
                        "z": position["z"], "z_subcell": position["z_subcell"]}
            update["authoritative_position"] = dict(position)
            update["playable_sphere"]["world_loading_center"] = dict(position)
            update["playable_sphere"]["authoritative_columns_center"] = dict(position)
            update["playable_sphere"]["active_coverage_center"] = dict(position)
            update["visual_lod"]["coverage_center"] = dict(position)
        self.assertGreater(len({tuple(update["authoritative_position"].items()) for update in updates}), 1)
        with self.assertRaisesRegex(self.gate.GameplayError, "production full-scale speed-30 step"):
            self.gate.verify_report(report, "macos")

    def test_x30_trajectory_rejects_ten_valid_steps_spread_across_thirty_seconds(self):
        report = self.report()
        self.reconcile_movement_playable_sphere(report, step_count=10)
        game = report["game_benchmark"]
        full = game["s1_terrain_performance"]
        self.assertEqual(len(full["movement_authoritative_sphere_updates"]), 10)
        with self.assertRaisesRegex(self.gate.GameplayError, "300 scheduled movement steps"):
            self.gate.verify_report(report, "macos")

    def test_x30_trajectory_rejects_299_steps_at_the_window_boundary(self):
        report = self.report()
        self.reconcile_movement_playable_sphere(report, step_count=299)
        with self.assertRaisesRegex(self.gate.GameplayError, "300 scheduled movement steps"):
            self.gate.verify_report(report, "macos")

    def test_x30_trajectory_rejects_ack_after_one_server_tick(self):
        report = self.report()
        game = report["game_benchmark"]
        full = game["s1_terrain_performance"]
        updates = full["movement_authoritative_sphere_updates"]
        inputs = game["raw_gameplay_inputs"]
        route_inputs = [entry for entry in inputs
                        if entry["sent_offset_ns"] >= game["frame_budget"]["measurement_window_start_offset_ns"]]
        updates[1]["offset_ns"] = route_inputs[1]["sent_offset_ns"] + self.gate.SERVER_TICK_NS + 1
        for update in updates[2:]:
            update["offset_ns"] += 1
        with self.assertRaisesRegex(self.gate.GameplayError, "one server tick"):
            self.gate.verify_report(report, "macos")

    def test_x30_trajectory_accepts_client_send_phase_with_bounded_ack_tail(self):
        report = self.report()
        self.reconcile_movement_playable_sphere(report, step_count=301)
        game = report["game_benchmark"]
        full = game["s1_terrain_performance"]
        start = game["frame_budget"]["measurement_window_start_offset_ns"]
        inputs = game["raw_gameplay_inputs"]
        route_inputs = [entry for entry in inputs if entry["sent_offset_ns"] >= start]
        updates = full["movement_authoritative_sphere_updates"]
        anchor_position = full["movement_trajectory_anchor"]["authoritative_position"]
        heights = [anchor_position["z"] * 10_000 + anchor_position["z_subcell"]]
        heights.extend(update["authoritative_position"]["z"] * 10_000
                       + update["authoritative_position"]["z_subcell"] for update in updates)
        self.assertGreater(max(heights), heights[0])
        self.assertLess(heights[-1], max(heights))
        for index, (raw_input, update) in enumerate(zip(route_inputs, updates)):
            scheduled_offset = start - 7_000_000 + index * self.gate.SERVER_TICK_NS
            raw_input["scheduled_offset_ns"] = scheduled_offset
            raw_input["sent_offset_ns"] = start if index == 0 else scheduled_offset
            sent_offset = raw_input["sent_offset_ns"]
            update["offset_ns"] = sent_offset + 10_000_000
        self.assertEqual(updates[-1]["offset_ns"], start + 30_003_000_000)

    def test_x30_trajectory_accepts_post_window_noop_send_in_ack_tail(self):
        report = self.report()
        game = report["game_benchmark"]
        full = game["s1_terrain_performance"]
        start = game["frame_budget"]["measurement_window_start_offset_ns"]
        end = game["frame_budget"]["measurement_window_end_offset_ns"]
        # This mirrors captured seq 424: a no-op send 453,125 ns after end,
        # acknowledged 41,038,792 ns after end, inside the one-tick tail.
        for raw_input in game["raw_gameplay_inputs"]:
            if start <= raw_input["sent_offset_ns"] < end:
                raw_input["scheduled_offset_ns"] += 3_138_084
        post_window_input = copy.deepcopy(game["raw_gameplay_inputs"][-1])
        post_window_input.update({
            "input_ordinal": len(game["raw_gameplay_inputs"]) + 1,
            "sequence": game["raw_gameplay_inputs"][-1]["sequence"] + 1,
            "scheduled_offset_ns": end - 6_861_916,
            "sent_offset_ns": end + 453_125,
            "movement_intent": {"strafe": 0, "forward": 0, "vertical": 0},
        })
        post_window_input["direction"].update({
            "x": 0, "y": 0, "z": 0, "accelerated": False, "speedup": 5,
        })
        game["raw_gameplay_inputs"].append(post_window_input)
        game["inputs_sent"] = len(game["raw_gameplay_inputs"])
        game["authoritative_player_updates"] += 1

        self.assertEqual(full["movement_boundary_authoritative_updates"], [])
        self.assertIs(full["movement_boundary_authoritative_updates_complete"], True)
        self.assertEqual(self.gate.verify_report(report, "macos")["status"], "passed")

    def test_x30_trajectory_rejects_input_beyond_bounded_ack_tail(self):
        report = self.report()
        game = report["game_benchmark"]
        end = game["frame_budget"]["measurement_window_end_offset_ns"]
        post_window_input = copy.deepcopy(game["raw_gameplay_inputs"][-1])
        post_window_input.update({
            "input_ordinal": len(game["raw_gameplay_inputs"]) + 1,
            "sequence": game["raw_gameplay_inputs"][-1]["sequence"] + 1,
            "scheduled_offset_ns": end + 2,
            "sent_offset_ns": end + self.gate.SERVER_TICK_NS + 1,
            "movement_intent": {"strafe": 0, "forward": 0, "vertical": 0},
        })
        post_window_input["direction"].update({
            "x": 0, "y": 0, "z": 0, "accelerated": False, "speedup": 5,
        })
        game["raw_gameplay_inputs"].append(post_window_input)
        game["inputs_sent"] = len(game["raw_gameplay_inputs"])
        game["authoritative_player_updates"] += 1

        with self.assertRaisesRegex(self.gate.GameplayError, "bounded post-window ACK tail"):
            self.gate.verify_report(report, "macos")

    def test_x30_trajectory_rejects_non_route_input_inside_measurement_window(self):
        report = self.report()
        game = report["game_benchmark"]
        full = game["s1_terrain_performance"]
        start = game["frame_budget"]["measurement_window_start_offset_ns"]
        end = game["frame_budget"]["measurement_window_end_offset_ns"]
        route_inputs = [entry for entry in game["raw_gameplay_inputs"]
                        if start <= entry["sent_offset_ns"] < end]
        stationary_input = route_inputs[-1]
        stationary_input["movement_intent"] = {"strafe": 0, "forward": 0, "vertical": 0}
        stationary_input["direction"].update({
            "x": 0, "y": 0, "z": 0, "accelerated": False, "speedup": 5,
        })
        last_update = full["movement_authoritative_sphere_updates"][-1]
        previous_position = full["movement_authoritative_sphere_updates"][-2]["authoritative_position"]
        last_update["authoritative_position"] = copy.deepcopy(previous_position)
        last_update["playable_sphere"]["active_coverage_center"] = copy.deepcopy(previous_position)
        last_update["playable_sphere"]["world_loading_center"] = copy.deepcopy(previous_position)
        last_update["playable_sphere"]["authoritative_columns_center"] = copy.deepcopy(previous_position)
        last_update["visual_lod"]["coverage_center"] = copy.deepcopy(previous_position)

        with self.assertRaisesRegex(self.gate.GameplayError, "stationary keyboard input"):
            self.gate.verify_report(report, "macos")

    def test_x30_trajectory_reconciles_delayed_pre_window_authority_ack(self):
        report = self.report()
        self.add_delayed_boundary_ack(report)
        self.assertEqual(self.gate.verify_report(report, "macos")["status"], "passed")

    def test_x30_trajectory_accepts_delayed_pre_window_x30_stress_ack(self):
        report = self.report()
        self.add_delayed_boundary_ack(report, accelerated=True)
        self.assertEqual(self.gate.verify_report(report, "macos")["status"], "passed")

    def test_x30_trajectory_rejects_omitted_delayed_pre_window_ack(self):
        report = self.report()
        self.add_delayed_boundary_ack(report)
        report["game_benchmark"]["s1_terrain_performance"][
            "movement_boundary_authoritative_updates"] = []
        with self.assertRaisesRegex(self.gate.GameplayError, "production full-scale speed-30 step"):
            self.gate.verify_report(report, "macos")

    def test_x30_trajectory_rejects_incomplete_boundary_capture_and_stale_anchor_sample(self):
        incomplete = self.report()
        self.add_delayed_boundary_ack(incomplete)
        incomplete["game_benchmark"]["s1_terrain_performance"][
            "movement_boundary_authoritative_updates_complete"] = False
        with self.assertRaisesRegex(self.gate.GameplayError, "boundary authority capture is incomplete"):
            self.gate.verify_report(incomplete, "macos")

        stale_anchor = self.report()
        full = stale_anchor["game_benchmark"]["s1_terrain_performance"]
        full["movement_trajectory_anchor_sphere_sample_offset_ns"] = (
            full["movement_trajectory_anchor"]["offset_ns"] - 1
        )
        with self.assertRaisesRegex(self.gate.GameplayError, "not refreshed at its exact boundary state"):
            self.gate.verify_report(stale_anchor, "macos")

    def test_x30_trajectory_rejects_interleaved_boundary_ack(self):
        report = self.report()
        self.add_delayed_boundary_ack(report)
        full = report["game_benchmark"]["s1_terrain_performance"]
        full["movement_boundary_authoritative_updates"][0]["offset_ns"] = (
            full["movement_authoritative_sphere_updates"][0]["offset_ns"] + 1
        )
        with self.assertRaisesRegex(self.gate.GameplayError, "interleaved with measured movement"):
            self.gate.verify_report(report, "macos")

    def test_x30_trajectory_rejects_incomplete_boundary_authority_sphere(self):
        report = self.report()
        self.add_delayed_boundary_ack(report)
        boundary = report["game_benchmark"]["s1_terrain_performance"][
            "movement_boundary_authoritative_updates"][0]
        boundary["playable_sphere"]["ready"] = False
        with self.assertRaisesRegex(self.gate.GameplayError, "complete sphere at its authoritative position"):
            self.gate.verify_report(report, "macos")

    def test_x30_trajectory_accepts_unrelated_global_counter_gaps(self):
        report = self.report()
        game = report["game_benchmark"]
        updates = game["s1_terrain_performance"]["movement_authoritative_sphere_updates"]
        inputs = game["raw_gameplay_inputs"]
        start = game["frame_budget"]["measurement_window_start_offset_ns"]
        route_inputs = [entry for entry in inputs if entry["sent_offset_ns"] >= start]
        for raw_input, update in zip(route_inputs, updates):
            raw_input["sequence"] += 1
            update["acknowledged_input_sequence"] += 1
            update["update_ordinal"] += 1
            update["state_revision"] += 1
        self.assertEqual(self.gate.verify_report(report, "macos")["status"], "passed")

    def test_x30_trajectory_rejects_interleaved_input_inside_route_window(self):
        report = self.report()
        game = report["game_benchmark"]
        inputs = game["raw_gameplay_inputs"]
        updates = game["s1_terrain_performance"]["movement_authoritative_sphere_updates"]
        start = game["frame_budget"]["measurement_window_start_offset_ns"]
        route_inputs = [entry for entry in inputs if entry["sent_offset_ns"] >= start]
        for raw_input, update in zip(route_inputs[150:], updates[150:]):
            raw_input["sequence"] += 1
            update["acknowledged_input_sequence"] += 1
        with self.assertRaisesRegex(self.gate.GameplayError, "sent-input sequence is incomplete"):
            self.gate.verify_report(report, "macos")

    def test_x30_trajectory_accepts_signed_preorigin_scheduler_deadline(self):
        report = self.report()
        game = report["game_benchmark"]
        self.reconcile_movement_playable_sphere(report, step_count=301)
        full = game["s1_terrain_performance"]
        budget = game["frame_budget"]
        old_start = budget["measurement_window_start_offset_ns"]
        start = 5_000_000
        shift = old_start - start
        budget["measurement_window_start_offset_ns"] = start
        inputs = game["raw_gameplay_inputs"]
        route_inputs = [entry for entry in inputs if entry["sent_offset_ns"] >= old_start]
        for raw_input in inputs:
            if raw_input["sent_offset_ns"] < old_start:
                raw_input["sent_offset_ns"] = 0
                raw_input["scheduled_offset_ns"] = 0
        updates = full["movement_authoritative_sphere_updates"]
        for index, (raw_input, update) in enumerate(zip(route_inputs, updates)):
            scheduled_offset = start - 7_000_000 + index * self.gate.SERVER_TICK_NS
            raw_input["scheduled_offset_ns"] = scheduled_offset
            raw_input["sent_offset_ns"] = start if index == 0 else scheduled_offset
            update["offset_ns"] = raw_input["sent_offset_ns"] + 10_000_000
        full["movement_trajectory_anchor"]["offset_ns"] -= shift
        full["movement_trajectory_anchor_sphere_sample_offset_ns"] = start
        trajectory_frames = []
        for frame in game["raw_frames"]:
            if frame["phase"] == self.gate.S1_MOVEMENT_PHASE:
                frame = copy.deepcopy(frame)
                frame["phase"] = self.gate.PHASES[2]
                frame["started_offset_ns"] -= shift
                trajectory_frames.append(frame)
        self.assertLess(route_inputs[0]["scheduled_offset_ns"], 0)
        self.gate.verify_x30_authoritative_trajectory(
            game, full, trajectory_frames, budget
        )

    def test_x30_trajectory_rejects_missing_interior_client_tick_slot(self):
        report = self.report()
        game = report["game_benchmark"]
        inputs = game["raw_gameplay_inputs"]
        inputs[150]["scheduled_offset_ns"] = (
            inputs[149]["scheduled_offset_ns"] + 2 * self.gate.SERVER_TICK_NS
        )
        inputs[150]["sent_offset_ns"] = inputs[150]["scheduled_offset_ns"] + 10_000_000
        with self.assertRaisesRegex(self.gate.GameplayError, "non-contiguous scheduled client tick"):
            self.gate.verify_report(report, "macos")

    def test_x30_trajectory_rejects_missing_or_incomplete_raw_evidence(self):
        missing_contract = self.report()
        del missing_contract["game_benchmark"]["movement_trajectory_contract"]
        with self.assertRaisesRegex(self.gate.GameplayError, "trajectory contract is absent"):
            self.gate.verify_report(missing_contract, "macos")

        missing_inputs = self.report()
        del missing_inputs["game_benchmark"]["raw_gameplay_inputs"]
        with self.assertRaisesRegex(self.gate.GameplayError, "lacks successful gameplay input evidence"):
            self.gate.verify_report(missing_inputs, "macos")

        missing_anchor = self.report()
        del missing_anchor["game_benchmark"]["s1_terrain_performance"]["movement_trajectory_anchor"]
        with self.assertRaisesRegex(self.gate.GameplayError, "trajectory anchor is absent"):
            self.gate.verify_report(missing_anchor, "macos")

        missing_update = self.report()
        full = missing_update["game_benchmark"]["s1_terrain_performance"]
        del full["movement_authoritative_sphere_updates"][4]
        full["movement_authoritative_update_count"] -= 1
        with self.assertRaisesRegex(self.gate.GameplayError, "requires 300 scheduled movement steps"):
            self.gate.verify_report(missing_update, "macos")

    def test_x30_trajectory_rejects_superseded_v2_workload_label(self):
        report = self.report()
        report["game_benchmark"]["workload"] = "turning-x30-movement-v2"
        with self.assertRaisesRegex(self.gate.GameplayError, "ordinary-turning Release workload"):
            self.gate.verify_report(report, "macos")

    def test_performance_requires_actual_movement_in_both_raw_evidence_streams(self):
        report = self.report()
        game = report["game_benchmark"]
        stationary_position = {"x": 0, "y": 0, "z": 64,
                               "x_subcell": 0, "y_subcell": 0, "z_subcell": 0}
        for frame in game["raw_frames"]:
            if frame["phase"] == self.gate.S1_MOVEMENT_PHASE:
                frame["playable_sphere"]["active_coverage_center"] = dict(stationary_position)
                frame["playable_sphere"]["world_loading_center"] = dict(stationary_position)
                frame["playable_sphere"]["authoritative_columns_center"] = dict(stationary_position)
                frame["visual_lod"]["coverage_center"] = dict(stationary_position)
        full = game["s1_terrain_performance"]
        for update in full["movement_authoritative_sphere_updates"]:
            update["authoritative_position"] = dict(stationary_position)
            update["playable_sphere"]["active_coverage_center"] = dict(stationary_position)
            update["playable_sphere"]["world_loading_center"] = dict(stationary_position)
            update["playable_sphere"]["authoritative_columns_center"] = dict(stationary_position)
            update["visual_lod"]["coverage_center"] = dict(stationary_position)
        with self.assertRaisesRegex(self.gate.GameplayError, "do not show ordinary player movement"):
            self.gate.verify_report(report, "macos")

    def test_performance_accepts_signed_world_cell_positions(self):
        report = self.report()
        game = report["game_benchmark"]
        horizontal_extent_cells = 65_536
        for frame in game["raw_frames"]:
            if frame["phase"] != self.gate.S1_MOVEMENT_PHASE:
                continue
            center = frame["playable_sphere"]["world_loading_center"]
            center["x"] -= horizontal_extent_cells
            frame["playable_sphere"]["active_coverage_center"]["x"] = center["x"]
            frame["visual_lod"]["coverage_center"]["x"] = center["x"]
        anchor = game["s1_terrain_performance"]["movement_trajectory_anchor"]
        anchor["authoritative_position"]["x"] -= horizontal_extent_cells
        anchor["playable_sphere"]["world_loading_center"]["x"] -= horizontal_extent_cells
        anchor["playable_sphere"]["active_coverage_center"]["x"] -= horizontal_extent_cells
        anchor["visual_lod"]["coverage_center"]["x"] -= horizontal_extent_cells
        for update in game["s1_terrain_performance"]["movement_authoritative_sphere_updates"]:
            position = update["authoritative_position"]
            position["x"] -= horizontal_extent_cells
            update["playable_sphere"]["active_coverage_center"]["x"] = position["x"]
            update["playable_sphere"]["world_loading_center"]["x"] = position["x"]
            update["visual_lod"]["coverage_center"]["x"] = position["x"]
        self.assertEqual(self.gate.verify_report(report, "macos")["status"], "passed")

    def test_performance_accepts_lagging_announcement_center_with_complete_current_profiles(self):
        report = self.report()
        game = report["game_benchmark"]

        for frame in game["raw_frames"]:
            sphere = frame.get("playable_sphere")
            if frame["phase"] != self.gate.S1_MOVEMENT_PHASE or sphere is None:
                continue
            center = sphere["world_loading_center"]
            sphere["active_coverage_center"] = {
                **center,
                "x": center["x"] - 1,
            }

        for update in game["s1_terrain_performance"]["movement_authoritative_sphere_updates"]:
            sphere = update["playable_sphere"]
            center = sphere["world_loading_center"]
            sphere["active_coverage_center"] = {
                **center,
                "x": center["x"] - 1,
            }

        self.assertEqual(self.gate.verify_report(report, "macos")["status"], "passed")

    def test_performance_rejects_sparse_fence_completions_even_when_cadence_is_fast(self):
        for platform in ("macos", "windows"):
            with self.subTest(platform=platform):
                report = self.report(platform=platform)
                game = report["game_benchmark"]
                raw_completions = game["raw_fence_confirmed_frames"]
                kept_movement = 0
                sparse = []
                for completion in raw_completions:
                    if completion["submission_phase"] == self.gate.S1_MOVEMENT_PHASE:
                        if kept_movement % 500 == 0:
                            sparse.append(completion)
                        kept_movement += 1
                    else:
                        sparse.append(completion)
                game["raw_fence_confirmed_frames"] = sparse
                game["frame_budget"] = self.frame_budget(game["raw_frames"], "performance", sparse, platform)
                with self.assertRaisesRegex(self.gate.GameplayError, "fence-confirmed frame window"):
                    self.gate.verify_report(report, platform)

    def test_s1_performance_rejects_fence_observation_stall_beyond_measurement_end(self):
        report = self.report()
        game = report["game_benchmark"]
        budget_start = game["frame_budget"]["measurement_window_start_offset_ns"]
        budget_end = budget_start + self.gate.PERFORMANCE_SAMPLE_MS * 1_000_000
        last = next(completion for completion in reversed(game["raw_fence_confirmed_frames"])
                    if completion["submission_phase"] == self.gate.S1_MOVEMENT_PHASE
                    and budget_start <= completion["submitted_offset_ns"] < budget_end)
        last["completion_observed_offset_ns"] += 200_000_000
        game["frame_budget"] = self.frame_budget(
            game["raw_frames"], "performance", game["raw_fence_confirmed_frames"], "macos")
        with self.assertRaisesRegex(self.gate.GameplayError, "beyond the bounded drain tail"):
            self.gate.verify_report(report, "macos")




















    def test_short_mac_coverage_movement_cadence_sample_is_diagnostic(self):
        report = self.report(movement_interval_ns=15000000, proof="terrain-coverage")
        verified = self.gate.verify_report(report, "macos")
        self.assertEqual(verified["request_cadence_status"], "failed")
        self.assertEqual(verified["request_cadence_proxy"][self.gate.PHASES[2]]["intervals_ns"]["sample_count"], 400)
        self.assertIn("full-radius-movement: insufficient cadence samples (400 intervals, requires 500)",
                      verified["request_cadence_reasons"])
        self.gate.verify_report(report, "macos")




    def test_coverage_cadence_diagnostic_reports_insufficient_raw_completions(self):
        for phase in (self.gate.PHASES[1], self.gate.PHASES[2], self.gate.PHASES[4]):
            for successes in (0, 1):
                with self.subTest(proof_contract="required", phase=phase, successes=successes):
                    report = self.report(proof="terrain-coverage")
                    selected = [frame for frame in report["game_benchmark"]["raw_frames"] if frame["phase"] == phase]
                    for index, frame in enumerate(selected):
                        frame["presentation_succeeded"] = index < successes
                    self.reconcile_raw_totals(report)
                    verified = self.gate.verify_report(report, "macos")
                    self.assertEqual(verified["coverage"], "passed")
                    self.assertEqual(verified["request_cadence_status"], "failed")
                    self.assertFalse(verified["request_cadence_required"])
                    self.assertIn(
                        f"{phase}: request cadence is not computable from raw completions",
                        verified["request_cadence_reasons"],
                    )

    def test_nonfinite_or_incomplete_raw_timing_fails_both_policies(self):
        for proof in ("performance", "terrain-coverage"):
            for name in ("nonfinite", "missing phase"):
                with self.subTest(proof=proof, mutation=name):
                    report = self.report(proof=proof)
                    frames = report["game_benchmark"]["raw_frames"]
                    if name == "nonfinite":
                        frames[1]["loop_duration_ns"] = float("inf")
                    else:
                        phase = (self.gate.S1_MOVEMENT_PHASE if proof == "performance"
                                 else self.gate.PHASES[2])
                        frames[:] = [frame for frame in frames if frame["phase"] != phase]
                    with self.assertRaises(self.gate.GameplayError):
                        self.gate.verify_report(report, "macos")


    def test_macos_fresh_frame_boundary_uses_at_least_120_fps(self):
        self.gate.verify_report(self.report(interval_ns=8_333_333), "macos")
        with self.assertRaisesRegex(self.gate.GameplayError, "fresh-frame budget exceeds 1%"):
            self.gate.verify_report(self.report(interval_ns=8_333_334), "macos")

    def test_windows_fresh_frame_boundary_uses_at_least_300_fps(self):
        self.gate.verify_report(self.report("windows", interval_ns=3_333_333), "windows")
        with self.assertRaisesRegex(self.gate.GameplayError, "fresh-frame budget exceeds 1%"):
            self.gate.verify_report(self.report("windows", interval_ns=3_333_334), "windows")

    def test_display_cadence_does_not_substitute_for_windows_fresh_frame_target(self):
        report = self.report("windows", interval_ns=8_000_000)
        self.assertEqual(report["game_benchmark"]["frame_budget"]["target_frame_duration_ns"], 3_333_333)
        with self.assertRaisesRegex(self.gate.GameplayError, "fresh-frame budget exceeds 1%"):
            self.gate.verify_report(report, "windows")






    def test_skipped_attempt_with_steady_successful_cadence_is_allowed_and_counted(self):
        for phase in (self.gate.PHASES[1], self.gate.PHASES[2], self.gate.PHASES[4]):
            with self.subTest(phase=phase):
                report = self.report(proof="terrain-coverage")
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

    def test_coverage_long_gap_is_retained_as_a_failed_cadence_diagnostic(self):
        report = self.report("windows", interval_ns=1800000, proof="terrain-coverage")
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
        verified = self.gate.verify_report(report, "windows")
        self.assertEqual(verified["coverage"], "passed")
        self.assertEqual(verified["request_cadence_status"], "failed")
        self.assertFalse(verified["request_cadence_required"])
        self.assertTrue(any(reason.startswith(f"{self.gate.PHASES[1]}: excess-time share")
                            for reason in verified["request_cadence_reasons"]))

    def test_phase_rate_includes_initial_work_and_a_closing_skipped_attempt(self):
        report = self.report(proof="terrain-coverage", interval_ns=8333333)
        game, gap = report["game_benchmark"], 70000000
        first = next(index for index, frame in enumerate(game["raw_frames"]) if frame["phase"] == self.gate.PHASES[1])
        for frame in game["raw_frames"][first:]:
            frame["started_offset_ns"] += gap
        game["full_radius_terrain"]["baseline_stationary_elapsed_ms"] += gap // 1000000
        report["timing"]["elapsed_ms"] += gap // 1000000
        self.reconcile_raw_totals(report)
        verified = self.gate.verify_report(report, "macos")
        self.assertEqual(verified["request_cadence_status"], "failed")
        report = self.report(proof="terrain-coverage")
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
        self.assertEqual(self.gate.verify_report(report, "macos")["request_cadence_status"], "failed")

    def test_longer_diagnostic_warmup_cannot_qualify_release(self):
        report = self.report(proof="terrain-coverage")
        report["game_benchmark"]["full_radius_terrain"]["warmup_timeout_ms"] = 1800000
        with self.assertRaisesRegex(self.gate.GameplayError, "180-second"):
            self.gate.verify_report(report, "macos")

    def test_exact_warmup_deadline_is_valid_and_strictly_later_is_rejected(self):
        report = self.report(proof="terrain-coverage")
        full = report["game_benchmark"]["full_radius_terrain"]
        for prefix, phase in (("initial", self.gate.PHASES[0]), ("recovery", self.gate.PHASES[3])):
            full[prefix + "_warmup_elapsed_ms"] = 180000
            full[prefix + "_barrier"]["phase_elapsed_ms"] = 180000
            next(frame for frame in report["game_benchmark"]["raw_frames"] if frame["phase"] == phase)["loop_duration_ns"] = 180000000000
        self.reconcile_contiguous_durations(report)
        verified = self.gate.verify_report(report, "macos")
        self.assertEqual(verified["request_cadence_proxy"][self.gate.PHASES[4]]["intervals_ns"]["max"], 8000000)
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
        for name, mutate in mutations:
            with self.subTest(name=name, proof_contract="required"):
                report = self.report(proof="terrain-coverage")
                mutate(report)
                with self.assertRaises(self.gate.GameplayError):
                    self.gate.verify_report(report, "macos")

    def test_each_full_radius_barrier_requires_exact_handles_and_drained_queues(self):
        for barrier_name in ("initial_barrier", "recovery_barrier"):
            for key, value in (("current_handle_mesh_visible", 205860), ("scan_performed", False), ("worker_outstanding", 1),
                               ("pending_delivery_batches", 1), ("unclassified_current_visible_mesh_detail", 1)):
                with self.subTest(barrier=barrier_name, field=key, proof_contract="required"):
                    report = self.report(proof="terrain-coverage")
                    report["game_benchmark"]["full_radius_terrain"][barrier_name][key] = value
                    with self.assertRaises(self.gate.GameplayError):
                        self.gate.verify_report(report, "macos")

    def test_endpoint_full_radius_does_not_hide_missing_meshes_during_travel(self):
        report = self.report(proof="terrain-coverage")
        game = report["game_benchmark"]
        movement_frame = next(frame for frame in game["raw_frames"] if frame["phase"] == self.gate.PHASES[2])
        movement_frame["terrain_near"].update(resident=37, nonresident=188, current_mesh_visible=37)
        game["terrain_readiness"].update(incomplete_current_near_sample_count=1,
                                         max_missing_current_mesh_count=188, current_near_coverage_complete=False)

        with self.assertRaisesRegex(self.gate.GameplayError, "missing meshes in the current near terrain window"):
            self.gate.verify_report(report, "macos")

    def test_current_near_samples_must_match_the_movement_frame(self):
        report = self.report(proof="terrain-coverage")
        movement_frame = next(frame for frame in report["game_benchmark"]["raw_frames"]
                              if frame["phase"] == self.gate.PHASES[2])
        movement_frame["terrain_near"]["center"]["x"] += 1

        with self.assertRaisesRegex(self.gate.GameplayError, "center disagrees"):
            self.gate.verify_report(report, "macos")

    def test_current_near_visible_tiles_cannot_contradict_residency(self):
        report = self.report(proof="terrain-coverage")
        frame = next(frame for frame in report["game_benchmark"]["raw_frames"]
                     if frame["phase"] == self.gate.PHASES[2])
        frame["terrain_near"]["resident"] = 0
        with self.assertRaisesRegex(self.gate.GameplayError, "missing meshes in the current near terrain window"):
            self.gate.verify_report(report, "macos")

    def test_all_zero_radial_diagnostics_cannot_claim_complete_coverage(self):
        report = self.report(proof="terrain-coverage")
        frame = report["game_benchmark"]["raw_frames"][0]
        frame["diagnostic_terrain_radial_bands"] = [
            {"desired": 0, "resident": 0, "nonresident": 0,
             "current_mesh_visible": 0, "stale_or_missing_mesh": 0} for _ in range(16)]
        frame["diagnostic_terrain_spatial_scan_duration_ns"] = 2_345_000
        with self.assertRaisesRegex(self.gate.GameplayError, "radial desired counts disagree with render radius"):
            self.gate.verify_report(report, "macos")

    def test_consistent_underreported_radial_diagnostics_are_rejected(self):
        report = self.report(proof="terrain-coverage")
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
        report = self.report(proof="terrain-coverage")
        frame = next(frame for frame in report["game_benchmark"]["raw_frames"]
                     if frame["phase"] == self.gate.PHASES[2])
        bands = copy.deepcopy(report["game_benchmark"]["full_radius_terrain"]["initial_barrier"]["radial_bands"])
        bands[4].update(resident=3, nonresident=bands[4]["desired"] - 3,
                        current_mesh_visible=3, stale_or_missing_mesh=0)
        frame["diagnostic_terrain_radial_bands"] = bands
        frame["diagnostic_terrain_spatial_scan_duration_ns"] = 2_345_000

        with self.assertRaisesRegex(self.gate.GameplayError, "found missing terrain inside the render radius"):
            self.gate.verify_report(report, "macos")

    def test_barrier_centers_require_signed_integer_coordinates(self):
        for prefix in ("initial", "recovery"):
            for center in ({}, {"x": True, "y": 0}, {"x": 0.5, "y": 0}, {"x": 0, "y": "0"}):
                with self.subTest(prefix=prefix, center=center):
                    report = self.report(proof="terrain-coverage")
                    report["game_benchmark"]["full_radius_terrain"][prefix + "_barrier"]["center"] = center
                    with self.assertRaisesRegex(self.gate.GameplayError, "center"):
                        self.gate.verify_report(report, "macos")
        report = self.report(proof="terrain-coverage")
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
                    report = self.report(proof="terrain-coverage")
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

    def test_recovery_barrier_must_be_on_the_travel_destination_side_of_the_origin(self):
        report = self.report(proof="terrain-coverage")
        full = report["game_benchmark"]["full_radius_terrain"]
        full["recovery_barrier"]["center"] = {"x": 0, "y": 0}
        for frame in report["game_benchmark"]["raw_frames"]:
            if frame["phase"] in (self.gate.PHASES[3], self.gate.PHASES[4]):
                frame["terrain_center"] = {"x": 0, "y": 0}

        with self.assertRaisesRegex(self.gate.GameplayError, "recovery.*destination"):
            self.gate.verify_report(report, "macos")

    def test_presentation_recreation_diagnostics_must_match_the_frame_result(self):
        report = self.report(proof="terrain-coverage")
        frame = report["game_benchmark"]["raw_frames"][0]
        frame["diagnostic_cpu_presentation_recreation_duration_ns"] = 1
        with self.assertRaisesRegex(self.gate.GameplayError, "without an attempt"):
            self.gate.verify_report(report, "macos")

        report = self.report(proof="terrain-coverage")
        frame = report["game_benchmark"]["raw_frames"][0]
        frame["diagnostic_presentation_recreation_attempted"] = True
        frame["diagnostic_presentation_recreation_deferred"] = True
        with self.assertRaisesRegex(self.gate.GameplayError, "deferred presentation recreation"):
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

    def test_barrier_elapsed_requires_typed_agreement_with_warmup(self):
        for prefix in ("initial", "recovery"):
            for elapsed in (True, 1.0, -1, 180001):
                with self.subTest(prefix=prefix, elapsed=elapsed):
                    report = self.report(proof="terrain-coverage")
                    report["game_benchmark"]["full_radius_terrain"][prefix + "_barrier"]["phase_elapsed_ms"] = elapsed
                    with self.assertRaisesRegex(self.gate.GameplayError, "barrier elapsed"):
                        self.gate.verify_report(report, "macos")

    def test_overall_runtime_cannot_end_before_the_raw_timeline(self):
        report = self.report()
        report["timing"].update(elapsed_ms=1, deadline_ms=1)
        with self.assertRaisesRegex(self.gate.GameplayError, "extends beyond the runtime report"):
            self.gate.verify_report(report, "macos")

    def test_raw_attempt_timing_rate_and_stationary_span_are_recomputed(self):
        mutations = (("attempts", lambda full: full.update(baseline_loop_attempts=1)),
                     ("failed requests", lambda full: full.update(baseline_failed_present_requests=1)),
                     ("summary", lambda full: full["baseline_loop_timings_ns"].update(p99_8=1)),
                     ("rate", lambda full: full.update(recovered_present_requests_per_second=1)),
                     ("duration", lambda full: full.update(baseline_stationary_elapsed_ms=9999)),
                     ("timeout", lambda full: full.update(recovery_warmup_elapsed_ms=180001)))
        for name, mutate in mutations:
            with self.subTest(name=name, proof_contract="required"):
                report = self.report(proof="terrain-coverage")
                mutate(report["game_benchmark"]["full_radius_terrain"])
                with self.assertRaises(self.gate.GameplayError):
                    self.gate.verify_report(report, "macos")

    def test_incorrect_unsuccessful_attempt_totals_are_rejected(self):
        report = self.report()
        report["game_benchmark"]["raw_frames"][1]["presentation_succeeded"] = False
        with self.assertRaisesRegex(self.gate.GameplayError, "S1 raw attempt totals mismatch"):
            self.gate.verify_report(report, "macos")

    def test_s1_present_mode_matches_the_frozen_platform_policy(self):
        report = self.report("windows")
        report["game_benchmark"]["present_mode"] = "fifo"
        with self.assertRaisesRegex(self.gate.GameplayError, "frozen qualification settings"):
            self.gate.verify_report(report, "windows")
        missing_mode = self.report("windows")
        missing_mode["game_benchmark"]["present_mode"] = ""
        with self.assertRaisesRegex(self.gate.GameplayError, "negotiated present mode"):
            self.gate.verify_report(missing_mode, "windows")

    def test_source_changes_invalidate_proof_but_documentation_does_not(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            receipt = root / "aggregate.json"
            receipt.write_text(json.dumps(self.aggregate(root)), encoding="utf-8")
            (root / "notes.md").write_text("new documentation", encoding="utf-8")
            self.gate.verify_receipt(root, receipt)
            (root / "src/game.cpp").write_text("changed source", encoding="utf-8")
            with self.subTest(proof_contract="required"), self.assertRaisesRegex(self.gate.GameplayError, "stale.*source"):
                self.gate.verify_receipt(root, receipt)

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
                with self.subTest(proof_contract="required"), self.assertRaises((self.gate.GameplayError, self.gate._MATRIX.MatrixError)):
                    self.gate.verify_receipt(root, receipt)

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
        self.assertEqual(len(gameplay_checks), 1)
        for level in ("snapshot", "minor", "major"):
            with self.subTest(level=level):
                selected = [item for item in gameplay_checks if item["enabled"] and level in item["levels"]]
                self.assertEqual(len(selected), 1)
                check = selected[0]
                self.assertTrue(check["proof"])
                self.assertFalse(check["reuse"])
                self.assertEqual(check["when"], "release")
                self.assertIn("--s1-owner-accepted", check["command"])
                self.assertEqual(check["command"][-2:], ["--candidate-commit", "{proof-candidate-commit}"])
                self.assertNotIn("--cadence-policy", check["command"])
                self.assertNotIn("--s7-windows-secondary", check["command"])
                self.assertNotIn("--s7-cadence-exception", check["command"])

    def test_snapshot_receipt_cannot_select_policy_or_bypass_strict_verification(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            aggregate = self.aggregate(root, mac_interval_ns=10000000)
            aggregate["cadence_policy"] = "snapshot"
            aggregate["rows"][0]["request_cadence_status"] = "passed"
            aggregate["rows"][0]["request_cadence_required"] = False
            receipt = root / "aggregate.json"
            receipt.write_text(json.dumps(aggregate), encoding="utf-8")
            with self.subTest(), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(self.gate.main(["--root", str(root), "--receipt", str(receipt)]), 1)

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
                self.record_macos(root, receipt, Path(row["executable"]["path"]))
            captured = json.loads(receipt.read_text(encoding="utf-8"))["rows"][0]
            self.assertEqual(identity.call_count, 2)
            self.assertEqual(captured["build"], row["build"])
            self.assertEqual(captured["report"], self.gate.file_identity(receipt.with_suffix(".raw.json")))
            self.assertNotIn("transport", captured)
            self.assertEqual(captured["command"], [row["executable"]["path"],
                                                   *self.gate.benchmark_arguments("macos", 180, str(receipt.with_suffix(".raw.json")))])
            self.assertEqual(captured["actual_command"], [str(receipt.with_suffix(".macos").resolve() / "MinecraftClone.command"),
                                                       *captured["command"][1:-1], captured["native_report_path"]])
            self.assertNotEqual(captured["native_report_path"], captured["command"][-1])
            self.assertFalse(Path(captured["native_report_path"]).parent.exists())

    def test_capture_merge_and_verify_require_the_same_proof_contract(self):
        with self.subTest(proof_contract="required"), tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            aggregate = self.aggregate(root)
            mac_row, windows_row = aggregate["rows"][:2]
            mac_receipt, windows_receipt, merged = root / "macos.json", root / "windows.json", root / "merged.json"
            windows_receipt.write_text(json.dumps({"schema_version": self.gate.SCHEMA, "rows": [windows_row]}), encoding="utf-8")

            def launch(command, cwd):
                Path(command[-1]).write_text(json.dumps(self.report()), encoding="utf-8")
                process = mock.Mock(returncode=0)
                process.poll.return_value = 0
                return process

            expected = 0
            package, manifest = self.macos_package(root, Path(mac_row["executable"]["path"]))
            common = ["--root", str(root)]
            with mock.patch.object(self.gate.sys, "platform", "darwin"), mock.patch.object(self.gate.os, "environ", {}), \
                    mock.patch.object(self.gate, "build_identity", return_value=mac_row["build"]), \
                    mock.patch.object(self.gate._MATRIX, "candidate_identity", return_value=CANDIDATE), \
                    mock.patch.object(self.gate.subprocess, "Popen", side_effect=launch), \
                    contextlib.redirect_stdout(io.StringIO()) as capture_output, contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(self.gate.main([*common, "--record", "macos", "--proof", "performance", "--receipt", str(mac_receipt),
                                                 "--macos-package", str(package), "--macos-package-manifest", str(manifest),
                                                 "--executable", mac_row["executable"]["path"]]), expected)
            self.assertEqual(json.loads(capture_output.getvalue().splitlines()[-1])["request_cadence_status"], "not-required")
            captured = json.loads(mac_receipt.read_text(encoding="utf-8"))
            self.assertNotIn("cadence_policy", captured)
            self.assertNotIn("cadence_policy", captured["rows"][0])
            self.assertEqual(captured["rows"][0]["command"][1:], self.gate.benchmark_arguments("macos", 180, str(mac_receipt.with_suffix(".raw.json"))))
            coverage_receipts = []
            for index, row in enumerate(aggregate["rows"][2:]):
                partial = root / f"coverage-{index}.json"
                partial.write_text(json.dumps({"schema_version": self.gate.SCHEMA, "rows": [row]}), encoding="utf-8")
                coverage_receipts.append(str(partial))
            for operation in (["--merge", str(mac_receipt), str(windows_receipt), *coverage_receipts], []):
                with contextlib.redirect_stdout(io.StringIO()) as output, contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(self.gate.main([*common, *operation, "--receipt", str(merged)]), expected)
                result = json.loads(output.getvalue())
                self.assertEqual(result["platforms"]["macos"]["performance"]["request_cadence_status"], "not-required")
                self.assertFalse(result["platforms"]["macos"]["performance"]["request_cadence_required"])
                self.assertFalse(result["platforms"]["macos"]["performance"]["presentation_timing_required"])
                self.assertFalse(result["platforms"]["macos"]["performance"]["request_cadence_reasons"])
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
                    self.record_macos(root, receipt, Path(row["executable"]["path"]))
            self.assertTrue(receipt.with_suffix(".raw.json").is_file())
            self.assertFalse(receipt.exists())

    def test_direct_capture_does_not_follow_raw_link_inserted_during_build_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            row = self.aggregate(root)["rows"][0]
            receipt, target = root / "captured.json", root / "unapproved.json"
            launched_report = []
            processes = []
            def identity(*arguments):
                report = receipt.with_suffix(".raw.json")
                if not report.is_symlink():
                    report.symlink_to(target)
                return row["build"]
            def launch(command, cwd):
                native_report = Path(command[-1])
                self.assertNotEqual(native_report, receipt.with_suffix(".raw.json"))
                launched_report.append(native_report)
                native_report.write_text(json.dumps(self.report()), encoding="utf-8")
                process = mock.Mock(returncode=0)
                process.poll.return_value = 0
                processes.append(process)
                return process
            with mock.patch.object(self.gate.sys, "platform", "darwin"), mock.patch.object(self.gate.os, "environ", {}), \
                    mock.patch.object(self.gate, "build_identity", side_effect=identity), \
                    mock.patch.object(self.gate._MATRIX, "candidate_identity", return_value=CANDIDATE), \
                    mock.patch.object(self.gate.subprocess, "Popen", side_effect=launch), contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt.*transport artifact already exists"):
                    self.record_macos(root, receipt, Path(row["executable"]["path"]))
            self.assertFalse(target.exists())
            self.assertTrue(receipt.with_suffix(".raw.json").is_symlink())
            failure = self.gate.read_json(receipt)["rows"][0]
            self.assertIsNone(failure["report"])
            self.assertIn("transport artifact already exists", failure["capture_error"])
            self.assertEqual(len(launched_report), 1)
            self.assertTrue(launched_report[0].is_file())
            self.assertEqual(launched_report[0].read_bytes(), json.dumps(self.report()).encode("utf-8"))
            self.assertEqual(failure["failed_report"], self.gate.file_identity(launched_report[0]))
            self.assertEqual(launched_report[0].parent.parent, receipt.parent)
            self.assertEqual(processes[0].poll(), 0)

    def test_verify_rejects_actual_command_changes_beyond_report_relocation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            aggregate = self.aggregate(root)
            row = aggregate["rows"][0]
            row["native_report_path"] = str(root / "private/native.raw.json")
            row["actual_command"] = [row["actual_command"][0], *row["command"][1:-1], row["native_report_path"]]
            receipt = root / "receipt.json"
            for mutation in ("arguments", "missing path", "missing command"):
                changed = copy.deepcopy(aggregate)
                current = changed["rows"][0]
                if mutation == "arguments":
                    current["actual_command"][1] = "--unapproved"
                elif mutation == "missing path":
                    del current["native_report_path"]
                else:
                    del current["actual_command"]
                receipt.write_text(json.dumps(changed), encoding="utf-8")
                with self.subTest(mutation=mutation), self.assertRaisesRegex(self.gate.GameplayError, "actual capture command"):
                    self.gate.verify_receipt(root, receipt)

    def test_verify_accepts_transported_windows_actual_report_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            aggregate = self.aggregate(root)
            row = aggregate["rows"][1]
            row["native_report_path"] = r"C:\Users\capture\Temp\mc-gameplay-capture-owned\native.raw.json"
            row["actual_command"] = [*row["command"][:-1], row["native_report_path"]]
            receipt = root / "receipt.json"
            receipt.write_text(json.dumps(aggregate), encoding="utf-8")
            self.assertEqual(set(self.gate.verify_receipt(root, receipt)), {"macos", "windows"})

    def test_direct_capture_interruption_stops_owned_child_before_retaining_raw(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            row = self.aggregate(root)["rows"][0]
            receipt = root / "interrupted.json"
            process = mock.Mock(returncode=-9)
            process.poll.return_value = None
            process.wait.side_effect = [KeyboardInterrupt(), None]
            def launch(command, cwd):
                Path(command[-1]).write_bytes(b"partial native evidence")
                return process
            with mock.patch.object(self.gate.sys, "platform", "darwin"), mock.patch.object(self.gate.os, "environ", {}), \
                    mock.patch.object(self.gate, "build_identity", return_value=row["build"]), \
                    mock.patch.object(self.gate.subprocess, "Popen", side_effect=launch), contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(KeyboardInterrupt):
                    self.record_macos(root, receipt, Path(row["executable"]["path"]))
            process.kill.assert_called_once_with()
            self.assertEqual(process.wait.call_count, 2)
            retained = list(root.glob("mc-gameplay-capture-*/native.raw.json"))
            self.assertEqual(len(retained), 1)
            self.assertEqual(retained[0].read_bytes(), b"partial native evidence")

    def direct_prefix_fixture(self, root):
        lock = json.loads((REPOSITORY / "dependencies.lock.json").read_text())
        (root / "dependencies.lock.json").write_text(json.dumps(lock), encoding="utf-8")
        cache, prefixes = {}, []
        for package in ("CoreCpp", "CoreProject2026"):
            prefix = root / package
            directory = prefix / "lib/cmake" / package
            directory.mkdir(parents=True)
            (prefix / "include").mkdir()
            (prefix / "include/public.hpp").write_text("public header", encoding="utf-8")
            (prefix / "lib/library.a").write_bytes(b"consumed static library")
            config = directory / f"{package}Config.cmake"
            content = (f'set({package}_REVISION "{lock["dependencies"][package]["revision"]}")\n'
                       + f'set({package}_DIRTY "FALSE")\nset({package}_REVISION_EXACT "TRUE")\n')
            if package == "CoreProject2026":
                content += ('set(CoreProject2026_REQUIRED_CORECPP_REVISION "'
                            + lock["dependencies"]["CoreCpp"]["revision"] + '")\n')
            config.write_text(content, encoding="utf-8")
            cache[f"{package}_DIR"] = str(directory)
            prefixes.append(str(prefix))
        return cache, prefixes

    def test_direct_prefix_capture_binds_exact_provenance_and_consumed_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache, prefixes = self.direct_prefix_fixture(root)
            identity = self.gate.direct_prefix_dependency_identity(root, cache, prefixes)
            self.assertEqual(set(identity["package_provenance"]), {"CoreCpp", "CoreProject2026"})
            self.assertEqual([item["path"] for item in identity["CMAKE_PREFIX_PATH"]], prefixes)
            self.assertTrue(all(item["consumed_file_count"] == 3 for item in identity["CMAKE_PREFIX_PATH"]))
            (Path(prefixes[0]) / "lib/library.a").write_bytes(b"changed consumed library")
            changed = self.gate.direct_prefix_dependency_identity(root, cache, prefixes)
            self.assertNotEqual(identity["CMAKE_PREFIX_PATH"][0]["consumed_files_sha256"],
                                changed["CMAKE_PREFIX_PATH"][0]["consumed_files_sha256"])

    def test_direct_prefix_capture_rejects_missing_or_uncovered_prefixes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache, prefixes = self.direct_prefix_fixture(root)
            with self.assertRaisesRegex(self.gate.GameplayError, "prefix is missing"):
                self.gate.direct_prefix_dependency_identity(root, cache, [str(root / "missing"), prefixes[1]])
            wrong = root / "unrelated"
            wrong.mkdir()
            (wrong / "unused.hpp").write_text("not the consumed package", encoding="utf-8")
            with self.assertRaisesRegex(self.gate.GameplayError, "not covered by a CMake prefix"):
                self.gate.direct_prefix_dependency_identity(root, cache, [str(wrong), prefixes[1]])

    def test_direct_prefix_capture_rejects_missing_package_provenance(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache, prefixes = self.direct_prefix_fixture(root)
            (Path(cache["CoreCpp_DIR"]) / "CoreCppConfig.cmake").unlink()
            with self.assertRaisesRegex(self.gate.GameplayError, "CoreCpp package provenance is missing"):
                self.gate.direct_prefix_dependency_identity(root, cache, prefixes)

    def test_direct_prefix_capture_rejects_wrong_dirty_or_inexact_packages(self):
        cases = (("CoreCpp", "REVISION", "b" * 40), ("CoreCpp", "DIRTY", "TRUE"),
                 ("CoreCpp", "REVISION_EXACT", "FALSE"), ("CoreProject2026", "REVISION", "c" * 40),
                 ("CoreProject2026", "REQUIRED_CORECPP_REVISION", "c" * 40))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache, prefixes = self.direct_prefix_fixture(root)
            for package, field, value in cases:
                with self.subTest(package=package, field=field):
                    config = Path(cache[f"{package}_DIR"]) / f"{package}Config.cmake"
                    before = config.read_text()
                    name = f"{package}_{field}"
                    content = self.gate.re.sub(r'set\(' + name + r' "[^"\n]*"\)',
                                               f'set({name} "{value}")', before)
                    config.write_text(content, encoding="utf-8")
                    with self.assertRaisesRegex(self.gate.GameplayError, "provenance|another CoreCpp"):
                        self.gate.direct_prefix_dependency_identity(root, cache, prefixes)
                    config.write_text(before, encoding="utf-8")

    def test_build_capture_rejects_partial_vcpkg_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            build = root / "build"
            (build / "CMakeFiles").mkdir(parents=True)
            executable = build / "mc_main"
            executable.write_bytes(b"unlaunched fixture")
            for name in ("build.ninja", "CMakeFiles/rules.ninja", "compile_commands.json"):
                (build / name).write_text("fixture", encoding="utf-8")
            (build / "CMakeCache.txt").write_text(
                f"CMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_BUILD_TYPE:STRING=Release\nCMAKE_HOME_DIRECTORY:INTERNAL={root}\n"
                "MC_ENABLE_SANITIZERS:BOOL=OFF\nMC_ALLOW_INEXACT_CORECPP:BOOL=OFF\nCMAKE_MAKE_PROGRAM:FILEPATH=/tools/ninja\n"
                "Z_VCPKG_ROOT_DIR:INTERNAL=/dependencies/vcpkg\nCMAKE_PREFIX_PATH:STRING=/dependencies/private\n",
                encoding="utf-8")
            with mock.patch.object(self.gate.subprocess, "run", return_value=mock.Mock(returncode=0, stdout="ninja: no work to do.\n")):
                with self.assertRaisesRegex(self.gate.GameplayError, "vcpkg dependency configuration is incomplete"):
                    self.gate.build_identity(root, build, executable)

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
                    mock.patch.object(self.gate._MATRIX, "_external_dependency_identity", return_value={
                        "VCPKG_ROOT": {"revision": "vcpkg"},
                        "VCPKG_INSTALLED_DIR": {"digest": "installed"},
                        "MC_PRIVATE_DEPENDENCIES_PREFIX": {"path": "/dependencies/private", "digest": "e" * 64},
                    }) as dependencies:
                identity = self.gate.build_identity(root, build_root, executable)
                run.assert_called_once_with(["/tools/ninja", "-n", "-v", "mc_main"], cwd=build_root, capture_output=True, text=True, timeout=60)
                self.assertEqual(identity["executable_sha256"], row["executable"]["sha256"])
                self.assertEqual(identity["source_input_sha256"], row["source_input_sha256"])
                self.assertEqual(dependencies.call_count, 1)
            with mock.patch.object(self.gate.subprocess, "run", return_value=mock.Mock(returncode=0, stdout="[1/2] clang++ -c game.cpp\n")), \
                    mock.patch.object(self.gate._MATRIX, "_external_dependency_identity", side_effect=AssertionError("stale build cannot qualify")):
                with self.assertRaisesRegex(self.gate.GameplayError, "build is stale"):
                    self.gate.build_identity(root, build_root, executable)

    def test_build_capture_fingerprints_each_cmake_prefix_in_order(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            row = self.aggregate(root)["rows"][0]
            executable = Path(row["executable"]["path"])
            build_root = executable.parent
            (build_root / "CMakeFiles").mkdir()
            for name in ("build.ninja", "CMakeFiles/rules.ninja", "compile_commands.json"):
                (build_root / name).write_text("fixture", encoding="utf-8")
            prefixes = ["/dependencies/corecpp", "/dependencies/coreproject", "/dependencies/vcpkg"]
            (build_root / "CMakeCache.txt").write_text(
                f"CMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_BUILD_TYPE:STRING=Release\nCMAKE_HOME_DIRECTORY:INTERNAL={root}\n"
                "MC_ENABLE_SANITIZERS:BOOL=OFF\nMC_ALLOW_INEXACT_CORECPP:BOOL=OFF\nCMAKE_MAKE_PROGRAM:FILEPATH=/tools/ninja\n"
                "Z_VCPKG_ROOT_DIR:INTERNAL=/dependencies/vcpkg-root\nVCPKG_INSTALLED_DIR:PATH=/dependencies/installed\n"
                f"CMAKE_PREFIX_PATH:STRING={';'.join(prefixes)}\n", encoding="utf-8")

            def dependency_identity(_root, _row, environment):
                prefix = environment["MC_PRIVATE_DEPENDENCIES_PREFIX"]
                return {
                    "VCPKG_ROOT": {"revision": "vcpkg"},
                    "VCPKG_INSTALLED_DIR": {"digest": "installed"},
                    "MC_PRIVATE_DEPENDENCIES_PREFIX": {"path": prefix, "digest": str(len(prefix))},
                }

            with mock.patch.object(self.gate.subprocess, "run", return_value=mock.Mock(returncode=0, stdout="ninja: no work to do.\n")), \
                    mock.patch.object(self.gate._MATRIX, "_external_dependency_identity", side_effect=dependency_identity) as dependencies:
                identity = self.gate.build_identity(root, build_root, executable)

            self.assertEqual(
                [call.args[2]["MC_PRIVATE_DEPENDENCIES_PREFIX"] for call in dependencies.call_args_list], prefixes
            )
            expected_dependencies = {
                "VCPKG_ROOT": {"revision": "vcpkg"},
                "VCPKG_INSTALLED_DIR": {"digest": "installed"},
                "CMAKE_PREFIX_PATH": [
                    {"path": prefix, "digest": str(len(prefix))} for prefix in prefixes
                ],
            }
            expected_digest = self.gate.hashlib.sha256(
                self.gate._MATRIX._canonical_json(expected_dependencies)
            ).hexdigest()
            self.assertEqual(identity["dependency_sha256"], expected_digest)

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
                    self.record_macos(root, receipt, Path(row["executable"]["path"]))
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
                        self.record_macos(root, receipt, Path(row["executable"]["path"]))
                captured = json.loads(receipt.read_text(encoding="utf-8"))["rows"][0]
                self.assertEqual(captured["exit_code"], exit_code)
                self.assertFalse(captured["timed_out"])
                self.assertIsNone(captured["report"])
                self.assertEqual(captured["capture_error"], "native report is absent")
                self.assertFalse(Path(captured["native_report_path"]).parent.exists())
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
                    mock.patch.object(self.gate.time, "monotonic", side_effect=[0, 481, 481]), contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt.*report is absent"):
                    self.record_macos(root, receipt, Path(row["executable"]["path"]))
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
                partial = Path(row["report"]["path"]).parent / "capture.json"
                if row["platform"] == "windows":
                    row["executable"]["path"] = r"C:\capture\mc_main.exe"
                    row["report"]["path"] = r"C:\capture\report.json"
                partial.write_text(json.dumps({"schema_version": self.gate.SCHEMA, "rows": [row]}), encoding="utf-8")
                inputs.append(partial)
            output = root / "aggregate.json"
            self.gate.merge_receipts(root, inputs, output)
            merged = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(merged["rows"][1]["command"], aggregate["rows"][1]["command"])
            self.assertEqual(Path(merged["rows"][1]["executable"]["path"]), (root / "windows/mc_main.exe").resolve())

    def test_desktop_capture_binds_native_session_and_every_original_runtime_input(self):
        with self.desktop_capture() as capture:
            self.record_desktop(capture)
            row = json.loads(capture["receipt"].read_text(encoding="utf-8"))["rows"][0]
            transport = row["transport"]
            self.assertEqual(transport["original_executable"], self.gate.file_identity(capture["executable"]))
            self.assertNotEqual(row["command"][0], transport["original_executable"]["path"])
            self.assertEqual(row["command"][-1], "capture.raw.json")
            self.assertEqual(transport["terminal_status"]["pid"], 4242)
            self.assertEqual(transport["terminal_status"]["session_id"], 1)
            self.assertEqual(transport["build_after"], row["build"])
            self.assertEqual(transport["package_before"], transport["package_after"])
            self.assertEqual(len(transport["copied_inputs"]), 4)
            self.gate.verify_desktop_transport(row, capture["root"])
            self.assertEqual([operation for operation, _ in capture["state"]["calls"]], ["Submit", "Status"])

    def test_desktop_transport_merge_preserves_native_job_and_copied_closure_paths(self):
        with self.desktop_capture() as capture:
            self.record_desktop(capture)
            original = json.loads(capture["receipt"].read_text(encoding="utf-8"))["rows"][0]
            moved = capture["root"] / "transported"
            moved.mkdir()
            shutil.copyfile(capture["receipt"], moved / "capture.json")
            shutil.copyfile(capture["receipt"].with_suffix(".raw.json"), moved / "capture.raw.json")
            shutil.copytree(capture["receipt"].with_suffix(".desktop"), moved / "capture.desktop")
            mac = capture["aggregate"]["rows"][0]
            mac["source_input_sha256"] = capture["build"]["source_input_sha256"]
            mac["build"]["source_input_sha256"] = capture["build"]["source_input_sha256"]
            mac_receipt = moved / "mac.json"
            mac_receipt.write_text(json.dumps({"schema_version": self.gate.SCHEMA, "rows": [mac]}), encoding="utf-8")
            merged = moved / "aggregate.json"
            inputs = [mac_receipt, moved / "capture.json"]
            for index, row in enumerate(capture["aggregate"]["rows"][2:]):
                row["source_input_sha256"] = capture["build"]["source_input_sha256"]
                row["build"]["source_input_sha256"] = capture["build"]["source_input_sha256"]
                row["build"] = copy.deepcopy(mac["build"] if row["platform"] == "macos" else original["build"])
                partial = moved / f"coverage-{index}.json"
                partial.write_text(json.dumps({"schema_version": self.gate.SCHEMA, "rows": [row]}), encoding="utf-8")
                inputs.append(partial)
            self.gate.merge_receipts(capture["root"], inputs, merged)
            row = json.loads(merged.read_text(encoding="utf-8"))["rows"][1]
            self.assertEqual(row["command"], original["command"])
            self.assertEqual(row["transport"]["job_id"], DESKTOP_JOB_ID)
            self.assertEqual(Path(row["executable"]["path"]), moved / "capture.desktop/app/mc_main.exe")

    def test_desktop_manifest_rejects_drive_absolute_traversal_and_duplicate_inputs(self):
        for relative in ("../mc_main.exe", "..\\mc_main.exe", "/mc_main.exe", r"C:\private\mc_main.exe", "mc_main.exe:stream"):
            with self.subTest(relative=relative), self.desktop_capture() as capture:
                entries = self.gate.read_json(capture["manifest"])
                entries[0]["relative"] = relative
                capture["manifest"].write_text(json.dumps(entries), encoding="utf-8")
                with self.assertRaises(self.gate.GameplayError):
                    self.record_desktop(capture)
                self.assertEqual(capture["state"]["calls"], [])
        with self.desktop_capture() as capture:
            entries = self.gate.read_json(capture["manifest"])
            entries.append({**entries[0], "relative": "MC_MAIN.EXE"})
            capture["manifest"].write_text(json.dumps(entries), encoding="utf-8")
            with self.assertRaisesRegex(self.gate.GameplayError, "duplicate"):
                self.record_desktop(capture)

    def test_desktop_manifest_rejects_malformed_field_types_and_unknown_schema(self):
        for mutation in ({"size": True}, {"size": 1.0}, {"sha256": "A" * 64}, {"sha256": None}, {"unknown": 1}):
            with self.subTest(mutation=mutation), self.desktop_capture() as capture:
                entries = self.gate.read_json(capture["manifest"])
                entries[0].update(mutation)
                capture["manifest"].write_text(json.dumps(entries), encoding="utf-8")
                with self.assertRaises(self.gate.GameplayError):
                    self.record_desktop(capture)
                self.assertEqual(capture["state"]["calls"], [])

    def test_desktop_package_rejects_unlisted_private_file_and_directory(self):
        for name, directory in (("private-source.cpp", False), (".git", True)):
            with self.subTest(name=name), self.desktop_capture() as capture:
                target = capture["package"] / name
                if directory:
                    target.mkdir()
                else:
                    target.write_bytes(b"private")
                with self.assertRaisesRegex(self.gate.GameplayError, "inventory|non-runtime directory"):
                    self.record_desktop(capture)
                self.assertEqual(capture["state"]["calls"], [])

    def test_desktop_package_rejects_original_and_package_links(self):
        for role in ("original", "package"):
            with self.subTest(role=role), self.desktop_capture() as capture:
                target = capture["inputs"]["fmt.dll"] if role == "original" else capture["package"] / "fmt.dll"
                backing = target.with_suffix(".saved")
                target.rename(backing)
                target.symlink_to(backing)
                with self.assertRaisesRegex(self.gate.GameplayError, "reparse"):
                    self.record_desktop(capture)
                self.assertEqual(capture["state"]["calls"], [])

    def test_desktop_package_requires_exact_original_executable_dll_shader_and_asset(self):
        for relative in ("mc_main.exe", "fmt.dll", "shaders/stone.vert.spv", "assets/textures/stone/default_stone.png"):
            with self.subTest(relative=relative), self.desktop_capture() as capture:
                capture["inputs"][relative].write_bytes(b"altered original")
                with self.assertRaisesRegex(self.gate.GameplayError, "disagrees with original"):
                    self.record_desktop(capture)
                self.assertEqual(capture["state"]["calls"], [])

    def test_desktop_capture_keeps_failure_receipt_on_source_build_dependency_drift(self):
        for field in ("source_input_sha256", "executable_sha256", "configuration_sha256", "dependency_sha256"):
            with self.subTest(field=field), self.desktop_capture() as capture:
                changed = {**capture["build"], field: "f" * 64}
                with mock.patch.object(self.gate, "build_identity", side_effect=[capture["build"], changed]):
                    with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt.*changed during capture"):
                        self.record_desktop(capture)
                row = self.gate.read_json(capture["receipt"])["rows"][0]
                self.assertEqual(row["transport"]["build_after"], changed)
                self.assertTrue(capture["receipt"].with_suffix(".raw.json").is_file())

    def test_desktop_capture_rejects_dangling_output_links_before_submission(self):
        for output in ("raw report", "retained directory"):
            with self.subTest(output=output), self.desktop_capture() as capture:
                target = capture["root"] / "unapproved-target"
                destination = capture["receipt"].with_suffix(".raw.json" if output == "raw report" else ".desktop")
                destination.symlink_to(target, target_is_directory=output == "retained directory")
                with self.assertRaisesRegex(self.gate.GameplayError, "raw capture path already exists|desktop transport output already exists"):
                    self.record_desktop(capture)
                self.assertFalse(target.exists())
                self.assertEqual(capture["state"]["calls"], [])

    def test_transport_copy_rejects_dangling_destination_link_without_following_it(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            source, target = root / "source.log", root / "retained.log"
            unapproved = root / "unapproved.log"
            source.write_bytes(b"retained")
            target.symlink_to(unapproved)
            with self.assertRaisesRegex(self.gate.GameplayError, "transport artifact already exists"):
                self.gate.copy_transport_file(source, target, root)
            self.assertFalse(unapproved.exists())

    def test_record_cli_rejects_dangling_receipt_link_before_submission(self):
        with self.desktop_capture() as capture:
            target = capture["root"] / "unapproved.json"
            capture["receipt"].symlink_to(target)
            with contextlib.redirect_stderr(io.StringIO()) as errors:
                result = self.gate.main(["--root", str(capture["root"]), "--receipt", str(capture["receipt"]),
                                         "--record", "windows", "--proof", "performance", "--executable", str(capture["executable"]),
                                         "--desktop-package", str(capture["package"]),
                                         "--desktop-package-manifest", str(capture["manifest"]),
                                         "--desktop-task-id", DESKTOP_TASK_ID])
            self.assertEqual(result, 1)
            self.assertIn("output receipt already exists", errors.getvalue())
            self.assertTrue(capture["receipt"].is_symlink())
            for receipt in (target, capture["receipt"]):
                self.assertFalse(receipt.exists())
                self.assertFalse(receipt.with_suffix(".raw.json").exists())
                self.assertFalse(receipt.with_suffix(".desktop").exists())
            self.assertEqual(capture["state"]["calls"], [])

    def test_merge_cli_rejects_dangling_receipt_link_without_writing_target(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            aggregate = self.aggregate(root)
            inputs = [root / f"{row['platform']}.json" for row in aggregate["rows"]]
            for receipt, row in zip(inputs, aggregate["rows"]):
                receipt.write_text(json.dumps({"schema_version": self.gate.SCHEMA, "rows": [row]}), encoding="utf-8")
            output, target = root / "merged.json", root / "unapproved.json"
            output.symlink_to(target)
            with contextlib.redirect_stderr(io.StringIO()) as errors:
                result = self.gate.main(["--root", str(root), "--receipt", str(output), "--merge", *map(str, inputs)])
            self.assertEqual(result, 1)
            self.assertIn("output receipt already exists", errors.getvalue())
            self.assertTrue(output.is_symlink())
            self.assertFalse(target.exists())
            self.assertFalse(target.with_suffix(".raw.json").exists())
            self.assertFalse(target.with_suffix(".desktop").exists())
            with self.assertRaisesRegex(self.gate.GameplayError, "aggregate receipt already exists"):
                self.gate.merge_receipts(root, inputs, output)
            self.assertFalse(target.exists())

    def test_record_cli_rejects_receipt_link_inserted_after_cli_precheck(self):
        with self.desktop_capture() as capture:
            target = capture["root"] / "unapproved.json"
            exists = self.gate.path_entry_exists
            def insert_after_check(destination):
                present = exists(destination)
                if destination == capture["receipt"] and not present:
                    destination.symlink_to(target)
                return present
            with mock.patch.object(self.gate, "path_entry_exists", side_effect=insert_after_check), \
                    contextlib.redirect_stderr(io.StringIO()):
                result = self.gate.main(["--root", str(capture["root"]), "--receipt", str(capture["receipt"]),
                                         "--record", "windows", "--proof", "performance", "--executable", str(capture["executable"])])
            self.assertEqual(result, 1)
            self.assertFalse(target.exists())
            self.assertFalse(target.with_suffix(".raw.json").exists())
            self.assertEqual(capture["state"]["calls"], [])

    def test_desktop_capture_exclusive_receipt_write_rejects_late_link(self):
        def insert_after_submission(state, root, package, app):
            (root / "capture.json").symlink_to(root / "unapproved.json")
        with self.desktop_capture(after_submit=insert_after_submission) as capture:
            with self.assertRaises(FileExistsError):
                self.record_desktop(capture)
            self.assertFalse((capture["root"] / "unapproved.json").exists())
            self.assertTrue(capture["receipt"].is_symlink())

    def test_merge_rejects_receipt_link_inserted_after_precheck(self):
        for invocation in ("cli", "direct"):
            with self.subTest(invocation=invocation), tempfile.TemporaryDirectory() as directory:
                root = Path(directory).resolve()
                aggregate = self.aggregate(root)
                inputs = [root / f"row-{index}.json" for index in range(len(aggregate["rows"]))]
                for receipt, row in zip(inputs, aggregate["rows"]):
                    receipt.write_text(json.dumps({"schema_version": self.gate.SCHEMA, "rows": [row]}), encoding="utf-8")
                output, target = root / "merged.json", root / "unapproved.json"
                exists = self.gate.path_entry_exists
                def insert_after_check(destination):
                    present = exists(destination)
                    if destination == output and not present:
                        destination.symlink_to(target)
                    return present
                with mock.patch.object(self.gate, "path_entry_exists", side_effect=insert_after_check), \
                        contextlib.redirect_stderr(io.StringIO()):
                    if invocation == "cli":
                        self.assertEqual(self.gate.main(["--root", str(root), "--receipt", str(output),
                                                         "--merge", *map(str, inputs)]), 1)
                    else:
                        with self.assertRaises(FileExistsError):
                            self.gate.merge_receipts(root, inputs, output)
                self.assertTrue(output.is_symlink())
                self.assertFalse(target.exists())
                self.assertFalse(target.with_suffix(".raw.json").exists())
                self.assertFalse(target.with_suffix(".desktop").exists())

    def test_desktop_capture_keeps_native_report_on_original_package_and_copy_drift(self):
        for role in ("original", "package", "copy"):
            def mutate(state, root, package, app):
                target = root / "windows/fmt.dll" if role == "original" else (package if role == "package" else app) / "fmt.dll"
                target.write_bytes(b"altered runtime")
            with self.subTest(role=role), self.desktop_capture(after_submit=mutate) as capture:
                with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt.*runtime|retained failure receipt.*original"):
                    self.record_desktop(capture)
                row = self.gate.read_json(capture["receipt"])["rows"][0]
                self.assertEqual(row["exit_code"], 0)
                self.assertIsNotNone(row["report"])
                self.assertTrue(row["capture_error"])

    def test_desktop_capture_checks_copied_runtime_closure_before_and_after_native_run(self):
        def mutate(state, root, package, app):
            (app / "shaders/stone.vert.spv").write_bytes(b"changed during native run")
        with self.desktop_capture(after_status=mutate) as capture:
            with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt.*runtime closure changed"):
                self.record_desktop(capture)
            row = self.gate.read_json(capture["receipt"])["rows"][0]
            self.assertIsNotNone(row["report"])
            self.assertNotEqual(row["transport"]["copied_before"], row["transport"]["copied_after"])

    def test_desktop_capture_rejects_manifest_bytes_and_installed_helper_drift(self):
        def mutate(state, root, package, app):
            manifest = root / "package.json"
            manifest.write_bytes(manifest.read_bytes() + b"\n")
        with self.desktop_capture(after_status=mutate) as capture:
            with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt.*manifest changed"):
                self.record_desktop(capture)
        with self.desktop_capture() as capture:
            helpers = self.gate.desktop_helper_identity()
            changed = copy.deepcopy(helpers)
            changed["windows-interactive-client.ps1"]["sha256"] = "f" * 64
            with mock.patch.object(self.gate, "desktop_helper_identity", side_effect=[helpers, changed]):
                with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt.*helper identity drift"):
                    self.record_desktop(capture)

    def test_desktop_capture_rejects_stale_malformed_status_and_cancels_only_owned_job(self):
        mutations = ({"schema": True}, {"schema": 2}, {"job_id": "bad"}, {"job_id": "87654321-4321-4321-4321-abcdef123456"},
                     {"task_id": "old-task"}, {"executable": "recorder.exe"}, {"executable_sha256": "f" * 64},
                     {"timeout_seconds": True}, {"session_id": 0}, {"pid": 0}, {"exit_code": True}, {"state": "unknown"})
        for mutation in mutations:
            with self.subTest(mutation=mutation), self.desktop_capture(status_mutation=mutation) as capture:
                with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt"):
                    self.record_desktop(capture)
                calls = capture["state"]["calls"]
                self.assertEqual(sum(operation == "Submit" for operation, _ in calls), 1)
                self.assertEqual([parameters["job_id"] for operation, parameters in calls if operation == "Cancel"], [DESKTOP_JOB_ID])
                row = self.gate.read_json(capture["receipt"])["rows"][0]
                self.assertTrue(row["transport"]["cleanup"]["terminal_observed"])

    def test_desktop_capture_rejects_altered_native_arguments(self):
        with self.desktop_capture(request_mutation={"arguments": ["--diagnostic"]}) as capture:
            with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt.*native gameplay command"):
                self.record_desktop(capture)
            row = self.gate.read_json(capture["receipt"])["rows"][0]
            self.assertIsNotNone(row["report"])
            self.assertTrue(row["transport"]["cleanup"]["terminal_observed"])

    def test_desktop_capture_retains_missing_report_failure_with_native_exit_zero(self):
        with self.desktop_capture(report=None) as capture:
            with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt.*native report is absent"):
                self.record_desktop(capture)
            row = self.gate.read_json(capture["receipt"])["rows"][0]
            self.assertEqual(row["exit_code"], 0)
            self.assertIsNone(row["report"])
            self.assertFalse(row["timed_out"])

    def test_desktop_capture_requires_actual_native_diagnostic_environment_false(self):
        for present in ("missing", True, None, 0):
            raw = self.report("windows")
            if present == "missing":
                del raw["game_benchmark"]["diagnostic_overrides_present"]
            else:
                raw["game_benchmark"]["diagnostic_overrides_present"] = present
            with self.subTest(present=present), self.desktop_capture(report=raw) as capture:
                with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt.*diagnostic environment proof"):
                    self.record_desktop(capture)
                row = self.gate.read_json(capture["receipt"])["rows"][0]
                self.assertEqual(row["exit_code"], 0)
                self.assertIsNotNone(row["report"])
                with self.assertRaisesRegex(self.gate.GameplayError, "diagnostic environment proof"):
                    self.gate.verify_desktop_transport(row, capture["root"])

    def test_desktop_capture_retains_nonzero_native_exit_and_failed_raw_report(self):
        raw = self.report("windows")
        raw.update(passed=False, failure="terrain warmup timeout")
        with self.desktop_capture(state="failed", exit_code=1, report=raw) as capture:
            with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt"):
                self.record_desktop(capture)
            row = self.gate.read_json(capture["receipt"])["rows"][0]
            self.assertEqual(row["exit_code"], 1)
            self.assertEqual(self.gate.read_json(capture["receipt"].with_suffix(".raw.json"))["failure"], "terrain warmup timeout")

    def test_desktop_capture_timeout_cancels_and_waits_exact_owned_native_job(self):
        with self.desktop_capture(state="running", report=None) as capture:
            clock = itertools.chain([0, 0, 0, 0, 0, 0], itertools.repeat(541))
            with mock.patch.object(self.gate.time, "monotonic", side_effect=lambda: next(clock)), \
                    mock.patch.object(self.gate.time, "sleep"):
                with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt.*deadline exceeded"):
                    self.record_desktop(capture)
            row = self.gate.read_json(capture["receipt"])["rows"][0]
            self.assertTrue(row["timed_out"])
            self.assertEqual(row["exit_code"], -9)
            self.assertTrue(row["transport"]["cleanup"]["terminal_observed"])
            self.assertEqual(row["transport"]["cleanup"]["terminal_status"]["state"], "cancelled")
            self.assertEqual(sum(operation == "Submit" for operation, _ in capture["state"]["calls"]), 1)

    def test_desktop_poll_uncertainty_does_not_resubmit_and_preserves_raw_report(self):
        with self.desktop_capture(poll_error=self.gate.subprocess.TimeoutExpired("helper-status", 30)) as capture:
            with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt"):
                self.record_desktop(capture)
            row = self.gate.read_json(capture["receipt"])["rows"][0]
            self.assertIsNotNone(row["report"])
            self.assertTrue(row["transport"]["cleanup"]["terminal_observed"])
            self.assertEqual([operation for operation, _ in capture["state"]["calls"]], ["Submit", "Status", "Cancel", "Status"])

    def test_desktop_lost_submission_response_recovers_only_exact_new_request_for_cancellation(self):
        with self.desktop_capture(submit_error=self.gate.subprocess.TimeoutExpired("helper-submit", 30)) as capture:
            with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt"):
                self.record_desktop(capture)
            row = self.gate.read_json(capture["receipt"])["rows"][0]
            self.assertEqual(row["transport"]["job_id"], DESKTOP_JOB_ID)
            self.assertTrue(row["transport"]["cleanup"]["terminal_observed"])
            self.assertEqual([operation for operation, _ in capture["state"]["calls"]], ["Submit", "Cancel", "Status"])

    def test_desktop_lost_response_does_not_claim_concurrent_same_caller_job(self):
        foreign_job = "87654321-4321-4321-4321-abcdef123456"

        def concurrent_request(state, root, package, app):
            request = self.gate.read_json(app.parent / "request.json")
            request.update(job_id=foreign_job, task_id=DESKTOP_TASK_ID)
            directory = app.parent.parent / foreign_job
            directory.mkdir()
            (directory / "request.json").write_text(json.dumps(request), encoding="utf-8")

        with self.desktop_capture(submit_error=self.gate.subprocess.TimeoutExpired("helper-submit", 30),
                                  after_submit=concurrent_request) as capture:
            with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt"):
                self.record_desktop(capture)
            row = self.gate.read_json(capture["receipt"])["rows"][0]
            self.assertEqual(row["transport"].get("job_id"), DESKTOP_JOB_ID)
            self.assertEqual([parameters["job_id"] for operation, parameters in capture["state"]["calls"]
                              if operation == "Cancel"], [DESKTOP_JOB_ID])
            self.assertTrue(row["transport"]["cleanup"]["terminal_observed"])

    def test_desktop_submissions_use_unique_bounded_task_correlations(self):
        task_ids = []
        with mock.patch.object(self.gate.uuid, "uuid4", side_effect=[mock.Mock(hex="1" * 32), mock.Mock(hex="2" * 32)]):
            for _ in range(2):
                with self.desktop_capture() as capture:
                    self.record_desktop(capture, "a" * 64)
                    task_ids.append(capture["state"]["task_id"])
        self.assertEqual(task_ids, ["a" * 31 + "-" + "1" * 32, "a" * 31 + "-" + "2" * 32])

    def test_desktop_lost_response_rejects_partial_or_invalid_task_correlation(self):
        for suffix in ("f" * 31, "g" * 32):
            with self.subTest(suffix=suffix):
                def malformed_request(state, root, package, app):
                    target = app.parent / "request.json"
                    request = self.gate.read_json(target)
                    request["task_id"] = state["task_id"].rsplit("-", 1)[0] + "-" + suffix
                    target.write_text(json.dumps(request), encoding="utf-8")

                with self.desktop_capture(submit_error=self.gate.subprocess.TimeoutExpired("helper-submit", 30),
                                          after_submit=malformed_request) as capture:
                    with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt"):
                        self.record_desktop(capture)
                    row = self.gate.read_json(capture["receipt"])["rows"][0]
                    self.assertNotIn("job_id", row["transport"])
                    self.assertEqual([operation for operation, _ in capture["state"]["calls"]], ["Submit"])

    def test_desktop_cancellation_failure_is_retained_without_claiming_cleanup(self):
        with self.desktop_capture(poll_error=OSError("poll unavailable"), cancel_error=OSError("cancel unavailable")) as capture:
            with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt.*cancel unavailable"):
                self.record_desktop(capture)
            row = self.gate.read_json(capture["receipt"])["rows"][0]
            self.assertFalse(row["transport"]["cleanup"]["terminal_observed"])
            self.assertIn("cancel unavailable", row["transport"]["cleanup"]["error"])

    def test_desktop_cleanup_interruption_is_retained_as_incomplete(self):
        with self.desktop_capture(state="running", report=None, poll_error=OSError("poll unavailable"),
                                  cancel_error=KeyboardInterrupt()) as capture:
            with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt"):
                self.record_desktop(capture)
            row = self.gate.read_json(capture["receipt"])["rows"][0]
            cleanup = row["transport"]["cleanup"]
            self.assertFalse(cleanup["terminal_observed"])
            self.assertEqual(cleanup["error"], "KeyboardInterrupt")
            self.assertEqual([operation for operation, _ in capture["state"]["calls"]], ["Submit", "Status", "Cancel"])

    def test_desktop_cleanup_rejects_skeletal_terminal_status(self):
        responses = [{"schema": 1, "job_id": DESKTOP_JOB_ID, "state": "cancel-requested"},
                     {"schema": 1, "job_id": DESKTOP_JOB_ID, "task_id": DESKTOP_TASK_ID, "state": "succeeded"}]
        with mock.patch.object(self.gate, "desktop_client", side_effect=responses):
            cleanup = self.gate.cancel_desktop_job(DESKTOP_JOB_ID, DESKTOP_TASK_ID, "d" * 64, 450, True)
        self.assertFalse(cleanup["terminal_observed"])
        self.assertNotIn("terminal_status", cleanup)
        self.assertIn("malformed native desktop status", cleanup["error"])

    def test_desktop_cleanup_accepts_documented_no_process_startup_failure(self):
        status = {"schema": 1, "job_id": DESKTOP_JOB_ID, "task_id": DESKTOP_TASK_ID, "state": "failed",
                  "submitted_utc": "2026-10-13T08:00:00+00:00", "finished_utc": "2026-10-13T08:00:01+00:00",
                  "pid": None, "session_id": None, "exit_code": None, "error": "Scheduler launch failed."}
        responses = [{"schema": 1, "job_id": DESKTOP_JOB_ID, "state": "already-terminal"}, status]
        with mock.patch.object(self.gate, "desktop_client", side_effect=responses):
            cleanup = self.gate.cancel_desktop_job(DESKTOP_JOB_ID, DESKTOP_TASK_ID, "d" * 64, 450, True)
        self.assertTrue(cleanup["terminal_observed"])
        self.assertEqual(cleanup["terminal_status"], status)

    def test_desktop_cleanup_does_not_prove_startup_failure_without_verified_request(self):
        status = {"schema": 1, "job_id": DESKTOP_JOB_ID, "task_id": DESKTOP_TASK_ID, "state": "failed",
                  "submitted_utc": "2026-10-13T08:00:00+00:00", "finished_utc": "2026-10-13T08:00:01+00:00",
                  "pid": None, "session_id": None, "exit_code": None, "error": "Scheduler launch failed."}
        responses = [{"schema": 1, "job_id": DESKTOP_JOB_ID, "state": "already-terminal"}, status]
        with mock.patch.object(self.gate, "desktop_client", side_effect=responses):
            cleanup = self.gate.cancel_desktop_job(DESKTOP_JOB_ID, DESKTOP_TASK_ID, "d" * 64, 450, False)
        self.assertFalse(cleanup["terminal_observed"])
        self.assertIn("without a verified request", cleanup["error"])

    def test_desktop_cancel_wait_is_bounded_when_helper_never_reaches_terminal(self):
        replies = [{"schema": 1, "job_id": DESKTOP_JOB_ID, "state": "cancel-requested"},
                   {"schema": 1, "job_id": DESKTOP_JOB_ID, "task_id": DESKTOP_TASK_ID, "state": "running",
                    "started_at_utc": "2026-10-13T08:00:00+00:00", "pid": 4242, "session_id": 1,
                    "executable": "mc_main.exe", "executable_sha256": "d" * 64, "timeout_seconds": 450,
                    "exit_code": None, "error": None}]
        with mock.patch.object(self.gate, "desktop_client", side_effect=replies) as client, \
                mock.patch.object(self.gate.time, "monotonic", side_effect=[0, 0, 0, 31, 31]), \
                mock.patch.object(self.gate.time, "sleep"):
            cleanup = self.gate.cancel_desktop_job(DESKTOP_JOB_ID, DESKTOP_TASK_ID, "d" * 64, 450, True)
        self.assertFalse(cleanup["terminal_observed"])
        self.assertIn("within 30 seconds", cleanup["error"])
        self.assertEqual([call.kwargs["job_id"] for call in client.call_args_list], [DESKTOP_JOB_ID, DESKTOP_JOB_ID])

    def test_desktop_report_from_before_fresh_submission_is_rejected(self):
        def stale_report(state, root, package, app):
            self.gate.os.utime(app / "capture.raw.json", (1, 1))
        with self.desktop_capture(after_submit=stale_report) as capture:
            with self.assertRaisesRegex(self.gate.GameplayError, "retained failure receipt.*predates"):
                self.record_desktop(capture)
            self.assertIsNone(self.gate.read_json(capture["receipt"])["rows"][0]["report"])

    def test_desktop_transport_rejects_broker_scheduler_and_runtime_identity_tampering(self):
        for change in ("broker", "scheduler", "native-status", "build", "package", "path", "extra-copy"):
            with self.subTest(change=change), self.desktop_capture() as capture:
                self.record_desktop(capture)
                row = self.gate.read_json(capture["receipt"])["rows"][0]
                transport = row["transport"]
                if change in ("broker", "scheduler", "native-status"):
                    name = {"broker": "broker.log", "scheduler": "scheduler.json", "native-status": "status.json"}[change]
                    target = capture["root"] / transport["artifacts"][name]["path"]
                    if change == "broker":
                        target.write_text(target.read_text(encoding="utf-8").replace("pid=4242", "pid=9999"), encoding="utf-8")
                    else:
                        artifact = self.gate.read_json(target)
                        artifact.update(instance_guid="not-a-guid" if change == "scheduler" else artifact.get("instance_guid"))
                        if change == "native-status":
                            artifact.pop("instance_guid")
                            artifact["session_id"] = 2
                        target.write_text(json.dumps(artifact), encoding="utf-8")
                    transport["artifacts"][name] = self.gate.transport_file(target, capture["root"])
                elif change == "build":
                    transport["build_after"]["dependency_sha256"] = "f" * 64
                elif change == "package":
                    transport["package_after"][0]["original"]["sha256"] = "f" * 64
                elif change == "path":
                    transport["package_before"][0]["original"]["path"] = str(capture["root"] / "wrong-asset.png")
                    transport["package_after"] = copy.deepcopy(transport["package_before"])
                else:
                    (capture["receipt"].with_suffix(".desktop") / "app/foreign.dll").write_bytes(b"unlisted")
                with self.assertRaises(self.gate.GameplayError):
                    self.gate.verify_desktop_transport(row, capture["root"])

    def test_desktop_client_encodes_argument_array_without_powershell_interpolation(self):
        parameters = {"package": "C:\\approved package\\'runtime", "arguments": ["--evidence", "Пример '$(`evil`)\"\n.json"],
                      "timeout_seconds": 450, "task_id": DESKTOP_TASK_ID}
        response = {"schema": 1, "job_id": DESKTOP_JOB_ID, "task_id": DESKTOP_TASK_ID, "state": "submitted"}
        with mock.patch.object(self.gate.subprocess, "run", return_value=mock.Mock(returncode=0, stdout=json.dumps(response), stderr="")) as run:
            self.assertEqual(self.gate.desktop_client("Submit", **parameters), response)
        command = run.call_args.args[0]
        self.assertEqual(command[1:-1], ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-EncodedCommand"])
        script = base64.b64decode(command[-1]).decode("utf-16-le")
        encoded_parameters = script.split("FromBase64String('", 1)[1].split("')", 1)[0]
        self.assertEqual(json.loads(base64.b64decode(encoded_parameters).decode("utf-8")), parameters)
        self.assertIn("-Arguments ([string[]]@($p.arguments))", script)
        self.assertNotIn("evil", script)
        self.assertEqual(run.call_args.kwargs["timeout"], 30)

    def test_desktop_options_are_windows_record_only_and_require_complete_input_set(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for arguments in (["--record", "macos", "--desktop-package", str(root), "--desktop-package-manifest", str(root),
                               "--desktop-task-id", DESKTOP_TASK_ID], ["--record", "windows", "--desktop-package", str(root)],
                              ["--desktop-task-id", DESKTOP_TASK_ID]):
                with self.subTest(arguments=arguments), contextlib.redirect_stderr(io.StringIO()), \
                        mock.patch.object(self.gate, "record", side_effect=AssertionError("native capture forbidden")):
                    self.assertEqual(self.gate.main(["--root", str(root), "--receipt", str(root / "receipt.json"), *arguments]), 1)

    def current_normal_report(self):
        facts = json.loads(json.dumps(self.gate.S1_CURRENT_NORMAL["facts"]))
        facts["s1"]["movement_authoritative_sphere_updates"] = [
            {"authoritative_position": {"x": index, "y": 0, "z": 0,
                                        "x_subcell": 0, "y_subcell": 0, "z_subcell": 0}}
            for index in range(facts["s1"]["movement_authoritative_update_count"])]
        return {"schema_version": "mc.runtime-evidence.v1", "mode": "benchmark-game", "passed": False,
                "failure": self.gate.S1_CURRENT_NORMAL["failure"], "timing": facts["timing"],
                "counters": facts["counters"], "game_benchmark": {
                    "workload": self.gate.WORKLOAD, "qualification_proof": "performance", "build_mode": "release",
                    "diagnostic_overrides_present": False,
                    "qualification_settings_manifest": self.gate.qualification_settings_manifest(),
                    "framebuffer": {"width": 2560, "height": 1440}, "present_mode": "fifo",
                    "s1_terrain_performance": facts["s1"], "frame_budget": facts["frame_budget"],
                    "successful_present_requests": facts["successful_present_requests"],
                    "failed_present_requests": facts["failed_present_requests"],
                    "raw_frames": [{}] * facts["raw_frame_count"],
                    "raw_fence_confirmed_frames": [{}] * facts["raw_fence_confirmed_frame_count"],
                    "raw_gameplay_inputs": [{}] * facts["raw_input_count"],
                    "frame_completion_source": self.gate.FRAME_COMPLETION_SOURCE}}

    def test_current_normal_capture_retains_failed_performance_and_coverage(self):
        report = self.current_normal_report()
        facts = self.gate.verify_s1_current_normal_report(report)
        self.assertFalse(facts["frame_budget"]["passed"])
        self.assertFalse(facts["s1"]["movement_playable_sphere_complete"])
        self.assertFalse(report["passed"])

    def test_current_normal_capture_does_not_waive_new_startup_failure(self):
        report = self.current_normal_report()
        report["game_benchmark"]["s1_terrain_performance"]["initial_deadline_missed"] = True
        with self.assertRaisesRegex(self.gate.GameplayError, "startup deadline failure is not waived"):
            self.gate.verify_s1_current_normal_report(report)

    def test_current_normal_capture_requires_complete_frame_window(self):
        report = self.current_normal_report()
        report["game_benchmark"]["frame_budget"]["measurement_window_complete"] = False
        with self.assertRaisesRegex(self.gate.GameplayError, "measurement window incomplete"):
            self.gate.verify_s1_current_normal_report(report)

    def test_current_normal_capture_rejects_diagnostic_renderer(self):
        report = self.current_normal_report()
        report["game_benchmark"]["diagnostic_overrides_present"] = True
        with self.assertRaisesRegex(self.gate.GameplayError, "renderer/settings changed"):
            self.gate.verify_s1_current_normal_report(report)

    def test_current_normal_capture_rejects_missing_movement_positions(self):
        report = self.current_normal_report()
        del report["game_benchmark"]["s1_terrain_performance"]["movement_authoritative_sphere_updates"]
        with self.assertRaisesRegex(self.gate.GameplayError, "trajectory is absent or malformed"):
            self.gate.verify_s1_current_normal_report(report)

    def test_current_normal_capture_rejects_stationary_movement_positions(self):
        report = self.current_normal_report()
        updates = report["game_benchmark"]["s1_terrain_performance"]["movement_authoritative_sphere_updates"]
        for update in updates:
            update["authoritative_position"]["x"] = 0
        with self.assertRaisesRegex(self.gate.GameplayError, "trajectory is stationary"):
            self.gate.verify_s1_current_normal_report(report)

    def test_current_normal_capture_rejects_malformed_movement_positions(self):
        report = self.current_normal_report()
        updates = report["game_benchmark"]["s1_terrain_performance"]["movement_authoritative_sphere_updates"]
        updates[0]["authoritative_position"]["x"] = True
        with self.assertRaisesRegex(self.gate.GameplayError, "position is malformed"):
            self.gate.verify_s1_current_normal_report(report)

    def test_current_normal_capture_requires_gameplay_input(self):
        report = self.current_normal_report()
        report["counters"]["inputs_sent"] = 0
        with self.assertRaisesRegex(self.gate.GameplayError, "connection/input/movement evidence absent"):
            self.gate.verify_s1_current_normal_report(report)


if __name__ == "__main__":
    unittest.main()
