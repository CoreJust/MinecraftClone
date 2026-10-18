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
        for platform in self.gate.TARGET_HZ:
            for proof, radius in self.gate.PROOFS.items():
                with self.subTest(platform=platform, proof=proof):
                    report = self.report(platform, interval_ns=10000000, proof=proof)
                    if proof == "terrain-coverage":
                        result = self.gate.verify_report(report, platform)
                        self.assertEqual(result["render_distance"], radius)
                        self.assertFalse(result["request_cadence_required"])
                        self.assertEqual(result["request_cadence_status"], "failed")
                    elif platform == "macos":
                        result = self.gate.verify_report(report, platform)
                        self.assertFalse(result["request_cadence_required"])
                        self.assertEqual(result["request_cadence_status"], "failed")
                        self.assertTrue(result["presentation_timing_required"])
                        self.assertEqual(result["presentation_timing_status"], "passed")
                    else:
                        with self.assertRaises(self.gate.GameplayError):
                            self.gate.verify_report(report, platform)

    def test_missing_selector_wrong_radius_and_omitted_recovery_loop_fail_closed(self):
        for mutation in ("missing-proof", "wrong-proof", "radius", "gap", "negative-rate", "nonresident"):
            for proof in self.gate.PROOFS:
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

    def test_s7_windows_secondary_requires_both_macos_proofs_and_defers_windows(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            aggregate = self.aggregate(root)
            aggregate["rows"] = [row for row in aggregate["rows"] if row["platform"] == "macos"]
            receipt = root / "receipt.json"
            receipt.write_text(json.dumps(aggregate), encoding="utf-8")

            verified = self.gate.verify_receipt(root, receipt, s7_windows_secondary=True)

            self.assertEqual(set(verified), {"macos", "windows"})
            self.assertEqual(set(verified["macos"]), set(self.gate.PROOFS))
            self.assertEqual(verified["windows"]["status"], "deferred")

            inputs = []
            for index, row in enumerate(aggregate["rows"]):
                partial = root / f"mac-proof-{index}.json"
                partial.write_text(json.dumps({"schema_version": self.gate.SCHEMA, "rows": [row]}), encoding="utf-8")
                inputs.append(partial)
            merged = self.gate.merge_receipts(
                root,
                inputs,
                root / "selected-aggregate.json",
                s7_windows_secondary=True,
            )
            self.assertEqual(merged["windows"]["status"], "deferred")

            with self.assertRaisesRegex(self.gate.GameplayError, "both terrain-coverage and performance"):
                self.gate.verify_receipt(root, receipt)

            aggregate["rows"] = [row for row in aggregate["rows"] if row["proof"] != "performance"]
            receipt.write_text(json.dumps(aggregate), encoding="utf-8")
            with self.assertRaisesRegex(self.gate.GameplayError, "Mac terrain-coverage and performance"):
                self.gate.verify_receipt(root, receipt, s7_windows_secondary=True)

    def test_s7_windows_secondary_does_not_weaken_macos_120hz_stability_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            aggregate = self.aggregate(root)
            aggregate["rows"] = [row for row in aggregate["rows"] if row["platform"] == "macos"]
            performance = next(row for row in aggregate["rows"] if row["proof"] == "performance")
            report_path = Path(performance["report"]["path"])
            report = self.gate.read_json(report_path)
            self.set_actual_phase_intervals(report, self.gate.PHASES[1], 8500000)
            report_path.write_text(json.dumps(report), encoding="utf-8")
            performance["report"] = self.gate.file_identity(report_path)
            receipt = root / "receipt.json"
            receipt.write_text(json.dumps(aggregate), encoding="utf-8")

            with self.assertRaisesRegex(self.gate.GameplayError, "driver presentation rate.*below 119.76Hz"):
                self.gate.verify_receipt(root, receipt, s7_windows_secondary=True)

    def cadence_exception_receipt(self, root):
        aggregate = self.aggregate(root)
        aggregate["rows"] = [row for row in aggregate["rows"] if row["platform"] == "macos"]
        performance = next(row for row in aggregate["rows"] if row["proof"] == "performance")
        report_path = Path(performance["report"]["path"])
        report = self.gate.read_json(report_path)
        self.set_actual_phase_intervals(report, self.gate.PHASES[1], 8500000)
        report_path.write_text(json.dumps(report), encoding="utf-8")
        performance["report"] = self.gate.file_identity(report_path)
        receipt = root / "receipt.json"
        receipt.write_text(json.dumps(aggregate), encoding="utf-8")
        approval = root / "docs/ai/S7-CADENCE-EXCEPTION-2026-10-18.json"
        approval.parent.mkdir(parents=True)
        source = REPOSITORY / "docs/ai/S7-CADENCE-EXCEPTION-2026-10-18.json"
        shutil.copyfile(source, approval)
        return receipt, aggregate, performance, report_path, report, approval

    def test_s7_cadence_exception_retains_failed_metrics_and_is_explicit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            receipt, aggregate, _, _, _, _ = self.cadence_exception_receipt(root)
            with self.assertRaisesRegex(self.gate.GameplayError, "driver presentation rate"):
                self.gate.verify_receipt(root, receipt, s7_windows_secondary=True)
            result = self.gate.verify_receipt(root, receipt, s7_windows_secondary=True,
                                              s7_cadence_exception="EarlyDev0.1.0-snapshot7")
            performance = result["macos"]["performance"]
            self.assertEqual(performance["status"], "accepted_exception")
            self.assertEqual(performance["presentation_timing_status"], "failed")
            self.assertTrue(performance["presentation_timing_metrics"]["failure_reasons"])
            baseline = performance["presentation_timing_metrics"]["phases"][self.gate.PHASES[1]]
            self.assertAlmostEqual(baseline["interval_rate_hz"], 1e9 / 8500000)
            self.assertEqual(performance["accepted_exception"]["approval"]["sha256"],
                             self.gate.file_identity(root / "docs/ai/S7-CADENCE-EXCEPTION-2026-10-18.json")["sha256"])
            inputs = []
            for index, row in enumerate(aggregate["rows"]):
                partial = root / f"proof-{index}.json"
                partial.write_text(json.dumps({"schema_version": self.gate.SCHEMA, "rows": [row]}), encoding="utf-8")
                inputs.append(partial)
            merged = self.gate.merge_receipts(root, inputs, root / "merged.json", s7_windows_secondary=True,
                                              s7_cadence_exception="EarlyDev0.1.0-snapshot7")
            self.assertEqual(merged["macos"]["performance"]["status"], "accepted_exception")

    def test_s7_cadence_exception_rejects_wrong_scope_or_changed_approval(self):
        for mutation in ("snapshot", "approval", "missing-approval"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                receipt, _, _, _, _, approval = self.cadence_exception_receipt(root)
                if mutation == "approval":
                    approval.write_text('{}', encoding="utf-8")
                elif mutation == "missing-approval":
                    approval.unlink()
                with self.assertRaises((self.gate.GameplayError, OSError)):
                    self.gate.verify_receipt(root, receipt, s7_windows_secondary=True,
                                            s7_cadence_exception="EarlyDev0.1.0-snapshot8" if mutation == "snapshot"
                                            else "EarlyDev0.1.0-snapshot7")

    def test_s7_selectors_reject_actual_other_version_and_missing_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            receipt, _, _, _, _, _ = self.cadence_exception_receipt(root)
            project = root / "src/shared/include/shared/ProjectInfo.hpp"
            original = project.read_text()
            for content in (original.replace(".patch = 7", ".patch = 8"),
                            original.replace(".minor = 0", ".minor = 1"), None):
                if content is None:
                    project.unlink()
                else:
                    project.write_text(content)
                with self.assertRaisesRegex(ValueError, "actual candidate version"):
                    self.gate.cadence_exception_approval(root, "EarlyDev0.1.0-snapshot7")
                with self.assertRaisesRegex(ValueError, "actual candidate version"):
                    self.gate.verify_receipt(root, receipt, s7_windows_secondary=True)
                with self.assertRaisesRegex(ValueError, "actual candidate version"):
                    self.gate.merge_receipts(root, [], root / "output", s7_windows_secondary=True)

    def test_s7_cadence_exception_does_not_waive_structural_or_provenance_failure(self):
        for mutation in ("driver", "radius", "coverage", "raw", "source", "invocation", "frame-budget"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                receipt, aggregate, row, report_path, report, _ = self.cadence_exception_receipt(root)
                if mutation == "driver":
                    report["game_benchmark"]["presentation_timing"]["records"].pop()
                elif mutation == "radius":
                    report["game_benchmark"]["full_radius_terrain"]["render_distance"] = 64
                elif mutation == "coverage":
                    report["game_benchmark"]["full_radius_terrain"]["initial_barrier"]["nonresident_desired"] = 1
                elif mutation == "raw":
                    report["game_benchmark"]["raw_frames"][0]["loop_duration_ns"] = -1
                elif mutation == "source":
                    row["source_input_sha256"] = "0" * 64
                elif mutation == "invocation":
                    row["environment"] = {"MC_DIAGNOSTIC_ANYTHING": "1"}
                else:
                    report["game_benchmark"]["frame_budget"]["sample_count"] += 1
                report_path.write_text(json.dumps(report), encoding="utf-8")
                row["report"] = self.gate.file_identity(report_path)
                receipt.write_text(json.dumps(aggregate), encoding="utf-8")
                with self.assertRaises(self.gate.GameplayError):
                    self.gate.verify_receipt(root, receipt, s7_windows_secondary=True,
                                            s7_cadence_exception="EarlyDev0.1.0-snapshot7")

    def test_s7_cadence_exception_cli_marks_aggregate_without_claiming_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            receipt, _, _, _, _, _ = self.cadence_exception_receipt(root)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                result = self.gate.main(["--root", str(root), "--receipt", str(receipt),
                                         "--s7-windows-secondary", "--s7-cadence-exception", "EarlyDev0.1.0-snapshot7"])
            self.assertEqual(result, 0)
            verified = json.loads(output.getvalue())
            self.assertEqual(verified["status"], "accepted_exception")
            self.assertEqual(verified["platforms"]["macos"]["performance"]["presentation_timing_status"], "failed")

    def test_s7_cadence_exception_keeps_full_platform_windows_requirement_independent(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            aggregate = self.aggregate(root)
            approval = root / "docs/ai/S7-CADENCE-EXCEPTION-2026-10-18.json"
            approval.parent.mkdir(parents=True)
            shutil.copyfile(REPOSITORY / "docs/ai/S7-CADENCE-EXCEPTION-2026-10-18.json", approval)
            receipt = root / "receipt.json"
            receipt.write_text(json.dumps(aggregate), encoding="utf-8")
            result = self.gate.verify_receipt(root, receipt, s7_cadence_exception="EarlyDev0.1.0-snapshot7")
            self.assertEqual(result["windows"]["performance"]["status"], "passed")
            row = next(row for row in aggregate["rows"] if row["platform"] == "windows" and row["proof"] == "performance")
            report_path = Path(row["report"]["path"])
            report_path.write_text(json.dumps(self.report("windows", interval_ns=10000000)), encoding="utf-8")
            row["report"] = self.gate.file_identity(report_path)
            receipt.write_text(json.dumps(aggregate), encoding="utf-8")
            with self.assertRaisesRegex(self.gate.GameplayError, "successful-request rate"):
                self.gate.verify_receipt(root, receipt, s7_cadence_exception="EarlyDev0.1.0-snapshot7")

    def test_s7_cadence_exception_does_not_waive_missing_samples_or_windows(self):
        report = self.report()
        frames = report["game_benchmark"]["raw_frames"]
        baseline = [frame for frame in frames if frame["phase"] == self.gate.PHASES[1]]
        for frame in baseline[:-499]:
            frame["presentation_succeeded"] = False
        self.reconcile_raw_totals(report)
        with self.assertRaisesRegex(self.gate.GameplayError, "insufficient driver presentation intervals"):
            self.gate.verify_report(report, "macos", cadence_exception={"release": "EarlyDev0.1.0-snapshot7"})
        with self.assertRaisesRegex(self.gate.GameplayError, "another platform or proof"):
            self.gate.verify_report(self.report("windows"), "windows",
                                    cadence_exception={"release": "EarlyDev0.1.0-snapshot7"})
        report = self.report()
        self.set_actual_phase_intervals(report, self.gate.PHASES[1], 32000000)
        with self.assertRaisesRegex(self.gate.GameplayError, "median exceeds one"):
            self.gate.verify_report(report, "macos", cadence_exception={"release": "EarlyDev0.1.0-snapshot7"})

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
        near_sample_count = sum(frame["phase"] in (self.gate.PHASES[2], self.gate.PHASES[3]) for frame in frames)
        readiness = {"contract": "mc.current-near-terrain-readiness.v1",
                     "measurement_scope": "current-center-radius-7",
                     "current_near_sample_count": near_sample_count,
                     "incomplete_current_near_sample_count": 0,
                     "max_missing_current_mesh_count": 0,
                     "current_near_coverage_complete": True}
        report = {"schema_version": "mc.runtime-evidence.v1", "mode": "benchmark-game", "passed": True, "failure": "",
                "timing": {"elapsed_ms": offset // 1000000, "deadline_ms": 480000}, "game_benchmark": {
                    "workload": self.gate.WORKLOAD, "qualification_proof": proof,
                    "diagnostic_overrides_present": False, "build_mode": "release", "displayed_cadence_hz": None,
                    "frame_budget": self.frame_budget(frames, proof, platform),
                    "present_mode": "immediate" if platform == "windows" else "fifo", "failed_present_requests": 0,
                    "successful_present_requests": len(frames),
                    "server_events_processed": 10, "inputs_sent": 10, "authoritative_player_updates": 10,
                    "terrain_readiness": readiness, "full_radius_terrain": full, "raw_frames": frames,
                    "raw_server_tick_durations_ns": [1000000, 2000000],
                    "server_tick_timings_ns": self.gate.timing_summary([1000000, 2000000])}}
        if platform == "macos" and proof == "performance":
            self.reconcile_presentation_timing(report)
        return report

    def reconcile_presentation_timing(self, report, spacing_ns=8333333):
        game = report["game_benchmark"]
        requests, records = [], []
        present_id = 0
        for frame in game["raw_frames"]:
            if not frame["presentation_succeeded"]:
                frame["swapchain_generation"] = None
                frame["application_present_id"] = None
                continue
            present_id += 1
            frame["swapchain_generation"] = 1
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
            if platform == "macos" and proof == "performance" and mac_interval_ns is not None:
                for phase in (self.gate.PHASES[1], self.gate.PHASES[2], self.gate.PHASES[4]):
                    self.set_actual_phase_intervals(report_value, phase, mac_interval_ns)
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

    def frame_budget(self, frames, proof, platform):
        measured = [frame["loop_duration_ns"] for frame in frames if frame["phase"] in
                    (self.gate.PHASES[1], self.gate.PHASES[2], self.gate.PHASES[4])]
        total = sum(measured)
        in_budget = sum(duration for duration in measured if duration <= 8333334)
        return {"target_frame_duration_ns": 8333334, "required_in_budget_time_permille": 998,
                "measured_time_ns": total, "in_budget_time_ns": in_budget, "sample_count": len(measured),
                "passed": total > 0 and total - in_budget <= total * 2 // 1000,
                "required_for_benchmark_pass": proof == "performance" and platform == "windows"}

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
        frames, full = game["raw_frames"], game["full_radius_terrain"]
        for ordinal, frame in enumerate(frames, 1):
            frame["loop_ordinal"] = ordinal
        game["frame_budget"] = self.frame_budget(frames, game["qualification_proof"],
                                                "windows" if game["present_mode"] == "immediate" else "macos")
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
                    self.assertEqual(verified[platform][proof]["request_cadence_status"], "passed")
                    self.assertEqual(verified[platform][proof]["request_cadence_required"],
                                     proof == "performance" and platform == "windows")
                    self.assertEqual(verified[platform][proof]["presentation_timing_required"],
                                     proof == "performance" and platform == "macos")
                    self.assertEqual(verified[platform][proof]["request_cadence_reasons"], [])

    def test_performance_proof_rejects_legacy_short_sampling_windows(self):
        report = self.report(
            stationary_duration_ns=10000000000,
            movement_duration_ns=6000000000,
        )

        with self.assertRaisesRegex(self.gate.GameplayError, "fixed 30-second performance sample"):
            self.gate.verify_report(report, "macos")

    def test_performance_proof_rejects_short_recovery_sampling_window(self):
        report = self.report(recovery_duration_ns=10000000000)
        full = report["game_benchmark"]["full_radius_terrain"]
        self.assertEqual(full["baseline_stationary_elapsed_ms"], 30000)
        self.assertEqual(full["movement_elapsed_ms"], 30000)
        self.assertEqual(full["recovered_stationary_elapsed_ms"], 10000)

        with self.assertRaisesRegex(self.gate.GameplayError, "fixed 30-second performance sample"):
            self.gate.verify_report(report, "macos")

    def test_slow_mac_submission_proxy_is_diagnostic_for_coverage_and_performance(self):
        report = self.report(interval_ns=10000000, proof="terrain-coverage")
        verified = self.gate.verify_report(report, "macos")
        self.assertEqual(verified["coverage"], "passed")
        self.assertEqual(verified["request_cadence_status"], "failed")
        self.assertFalse(verified["request_cadence_required"])
        self.assertEqual(verified["physical_display_cadence"], "unavailable")
        for phase in (self.gate.PHASES[1], self.gate.PHASES[2], self.gate.PHASES[4]):
            self.assertEqual(verified["request_cadence_proxy"][phase]["present_requests_per_second"], 100)
            self.assertTrue(any(reason.startswith(phase + ": successful-request rate") for reason in verified["request_cadence_reasons"]))
        performance = self.gate.verify_report(self.report(interval_ns=10000000), "macos")
        self.assertEqual(performance["request_cadence_status"], "failed")
        self.assertTrue(performance["presentation_timing_required"])
        self.assertEqual(performance["presentation_timing_status"], "passed")

    def test_mac_performance_cadence_uses_driver_records_not_cpu_completion_bursts(self):
        report = self.report(interval_ns=10000000, movement_interval_ns=10000000)
        verified = self.gate.verify_report(report, "macos")
        self.assertFalse(verified["request_cadence_required"])
        self.assertEqual(verified["request_cadence_status"], "failed")
        self.assertTrue(verified["presentation_timing_required"])
        self.assertEqual(verified["presentation_timing_status"], "passed")
        self.assertEqual(verified["presentation_timing_metrics"]["source"], "driver-reported")
        self.assertEqual(verified["presentation_timing_metrics"]["clock_domain"],
                         "vulkan-display-timing-monotonic-nanoseconds-unspecified-origin")

    def test_mac_performance_requires_actual_timing_capability_and_new_evidence(self):
        for mutation in ("missing", "unavailable"):
            with self.subTest(mutation=mutation):
                report = self.report()
                timing = report["game_benchmark"]["presentation_timing"]
                if mutation == "missing":
                    del report["game_benchmark"]["presentation_timing"]
                else:
                    timing["capability"] = "unavailable"
                with self.assertRaisesRegex(self.gate.GameplayError, "presentation timing|display_timing"):
                    self.gate.verify_report(report, "macos")

    def test_actual_timing_accepts_recovered_incomplete_poll_and_out_of_order_delivery(self):
        for vk_result in (0, 5):
            with self.subTest(vk_result=vk_result):
                report = self.report()
                timing = report["game_benchmark"]["presentation_timing"]
                first_poll = next(poll for poll in timing["polls"] if poll["scope"] == "loop")
                first_poll.update(status="incomplete", vk_result=vk_result)
                timing["records"].reverse()
                verified = self.gate.verify_report(report, "macos")
                metrics = verified["presentation_timing_metrics"]
                self.assertEqual(metrics["status"], "passed")
                self.assertEqual(metrics["incomplete_poll_count"], 1)
                self.assertEqual(metrics["expected_record_count"], len(timing["successful_requests"]))

    def test_actual_timing_rejects_an_unpaired_extra_loop_poll(self):
        report = self.report()
        timing = report["game_benchmark"]["presentation_timing"]
        timing["polls"].insert(0, {"scope": "loop", "status": "empty", "vk_result": 0, "returned_count": 0})
        with self.assertRaisesRegex(self.gate.GameplayError, "exactly one poll per benchmark loop attempt"):
            self.gate.verify_report(report, "macos")

    def test_actual_timing_accepts_zero_initial_swapchain_generation(self):
        report = self.report()
        timing = report["game_benchmark"]["presentation_timing"]
        for request in timing["successful_requests"]:
            request["swapchain_generation"] = 0
        for record in timing["records"]:
            record["swapchain_generation"] = 0
        for frame in report["game_benchmark"]["raw_frames"]:
            if frame["presentation_succeeded"]:
                frame["swapchain_generation"] = 0
        timing["capture_endpoint"]["swapchain_generation"] = 0

        endpoint_id = timing["capture_endpoint"]["application_present_id"]
        last_time = timing["records"][-1]["actual_present_time_ns"]
        timing["drain"]["request_ids"].append(
            {"swapchain_generation": 0, "application_present_id": endpoint_id + 1})
        timing["records"].append({"swapchain_generation": 0, "application_present_id": endpoint_id + 1,
                                  "actual_present_time_ns": last_time + 8333333})
        timing["polls"][-1].update(status="records", vk_result=0, returned_count=1)

        verified = self.gate.verify_report(report, "macos")
        self.assertEqual(verified["presentation_timing_status"], "passed")
        self.assertEqual(verified["presentation_timing_metrics"]["drain_record_count"], 1)

    def test_actual_timing_rejects_negative_swapchain_generation_at_every_join(self):
        mutations = ("request", "record", "raw frame", "drain")
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                report = self.report()
                game = report["game_benchmark"]
                timing = game["presentation_timing"]
                if mutation == "request":
                    timing["successful_requests"][0]["swapchain_generation"] = -1
                elif mutation == "record":
                    timing["records"][0]["swapchain_generation"] = -1
                elif mutation == "raw frame":
                    frame = next(frame for frame in game["raw_frames"] if frame["presentation_succeeded"])
                    frame["swapchain_generation"] = -1
                else:
                    endpoint = timing["capture_endpoint"]
                    timing["drain"]["request_ids"].append(
                        {"swapchain_generation": -1,
                         "application_present_id": endpoint["application_present_id"] + 1})
                with self.assertRaisesRegex(self.gate.GameplayError, "swapchain generation must be an integer >= 0"):
                    self.gate.verify_report(report, "macos")

    def test_actual_timing_accepts_repeated_ids_and_missing_history_in_initial_warmup_generations(self):
        report = self.report()
        game = report["game_benchmark"]
        timing = game["presentation_timing"]
        requests = timing["successful_requests"]
        requests[0]["swapchain_generation"] = 0
        for request in requests[1:]:
            request["loop_ordinal"] += 1
        requests.insert(1, {"swapchain_generation": 1, "application_present_id": 1,
                            "loop_ordinal": 2, "phase": self.gate.PHASES[0]})
        for frame in game["raw_frames"]:
            if frame["phase"] == self.gate.PHASES[0]:
                frame["swapchain_generation"] = 0
            else:
                frame["loop_ordinal"] += 1
        timing["capture_endpoint"]["loop_ordinal"] += 1
        timing["polls"].insert(1, {"scope": "loop", "status": "empty", "vk_result": 0, "returned_count": 0})
        game["successful_present_requests"] += 1

        verified = self.gate.verify_report(report, "macos")
        self.assertEqual(verified["presentation_timing_status"], "passed")
        self.assertEqual(verified["presentation_timing_metrics"]["expected_record_count"], len(requests) - 1)

    def test_actual_timing_requires_history_for_measurement_generation_warmup_prefix(self):
        report = self.report()
        timing = report["game_benchmark"]["presentation_timing"]
        timing["records"].pop(0)
        first_poll = next(poll for poll in timing["polls"] if poll["scope"] == "loop")
        first_poll.update(status="empty", vk_result=0, returned_count=0)
        with self.assertRaisesRegex(self.gate.GameplayError, "lost expected measurement-generation request IDs"):
            self.gate.verify_report(report, "macos")

    def test_actual_timing_retired_warmup_timestamps_do_not_define_measurement_cadence(self):
        report = self.report()
        game = report["game_benchmark"]
        timing = game["presentation_timing"]
        requests = timing["successful_requests"]
        requests[0]["swapchain_generation"] = 0
        timing["records"][0]["swapchain_generation"] = 0
        for request in requests[1:]:
            request["loop_ordinal"] += 2
        requests.insert(1, {"swapchain_generation": 0, "application_present_id": 2,
                            "loop_ordinal": 2, "phase": self.gate.PHASES[0]})
        requests.insert(2, {"swapchain_generation": 1, "application_present_id": 1,
                            "loop_ordinal": 3, "phase": self.gate.PHASES[0]})
        timing["records"].insert(1, {"swapchain_generation": 0, "application_present_id": 2,
                                    "actual_present_time_ns": timing["records"][0]["actual_present_time_ns"]})
        timing["records"].insert(2, {"swapchain_generation": 1, "application_present_id": 1,
                                    "actual_present_time_ns": timing["records"][0]["actual_present_time_ns"]})
        for frame in game["raw_frames"]:
            if frame["phase"] == self.gate.PHASES[0]:
                frame["swapchain_generation"] = 0
            else:
                frame["loop_ordinal"] += 2
        timing["capture_endpoint"]["loop_ordinal"] += 2
        timing["polls"].insert(1, {"scope": "loop", "status": "records", "vk_result": 0, "returned_count": 1})
        timing["polls"].insert(2, {"scope": "loop", "status": "records", "vk_result": 0, "returned_count": 1})
        game["successful_present_requests"] += 2

        verified = self.gate.verify_report(report, "macos")
        self.assertEqual(verified["presentation_timing_status"], "passed")
        self.assertEqual(verified["presentation_timing_metrics"]["expected_record_count"], len(requests) - 2)

    def test_actual_timing_rejects_equal_timestamps_in_measurement_generation(self):
        for record_index in (0, 30):
            with self.subTest(record_index=record_index):
                report = self.report()
                records = report["game_benchmark"]["presentation_timing"]["records"]
                records[record_index + 1]["actual_present_time_ns"] = records[record_index]["actual_present_time_ns"]
                with self.assertRaisesRegex(self.gate.GameplayError, "not strictly increasing"):
                    self.gate.verify_report(report, "macos")

    def test_actual_timing_rejects_generation_change_after_initial_warmup(self):
        changed_requests = (
            (self.gate.PHASES[1], 1),
            (self.gate.PHASES[2], 0),
            (self.gate.PHASES[3], 0),
            (self.gate.PHASES[4], 0),
        )
        for phase, occurrence in changed_requests:
            with self.subTest(phase=phase):
                report = self.report()
                timing = report["game_benchmark"]["presentation_timing"]
                phase_requests = [request for request in timing["successful_requests"] if request["phase"] == phase]
                phase_requests[occurrence]["swapchain_generation"] = 2
                with self.assertRaisesRegex(self.gate.GameplayError, "swapchain recreation"):
                    self.gate.verify_report(report, "macos")

    def test_actual_timing_rejects_decreasing_swapchain_generations(self):
        report = self.report()
        requests = report["game_benchmark"]["presentation_timing"]["successful_requests"]
        requests[0]["swapchain_generation"] = 2
        with self.assertRaisesRegex(self.gate.GameplayError, "generations are not nondecreasing"):
            self.gate.verify_report(report, "macos")

    def test_actual_timing_uses_generation_change_not_deferred_flag_as_the_failure(self):
        for change_generation in (False, True):
            with self.subTest(change_generation=change_generation):
                report = self.report()
                game = report["game_benchmark"]
                timing = game["presentation_timing"]
                frame = next(frame for frame in game["raw_frames"] if frame["phase"] == self.gate.PHASES[2])
                ordinal = frame["loop_ordinal"]
                request = next(request for request in timing["successful_requests"]
                               if request["loop_ordinal"] == ordinal)
                generation, present_id = request["swapchain_generation"], request["application_present_id"]
                timing["successful_requests"].remove(request)
                timing["records"][:] = [record for record in timing["records"]
                                         if (record["swapchain_generation"], record["application_present_id"])
                                         != (generation, present_id)]
                timing["polls"][ordinal - 1].update(status="empty", vk_result=0, returned_count=0)
                frame.update(presentation_succeeded=False, swapchain_generation=None, application_present_id=None,
                             diagnostic_presentation_recreation_attempted=True,
                             diagnostic_presentation_recreation_deferred=True,
                             diagnostic_frame_recorded=False)
                game["successful_present_requests"] -= 1
                game["failed_present_requests"] += 1
                if change_generation:
                    next_request = next(request for request in timing["successful_requests"]
                                        if request["phase"] == self.gate.PHASES[2])
                    next_request["swapchain_generation"] = generation + 1

                if change_generation:
                    with self.assertRaisesRegex(self.gate.GameplayError, "swapchain recreation"):
                        self.gate.verify_report(report, "macos")
                else:
                    verified = self.gate.verify_report(report, "macos")
                    self.assertEqual(verified["presentation_timing_status"], "passed")

    def test_actual_timing_counts_a_phase_boundary_interval_in_the_starting_attempt_phase(self):
        report = self.report()
        self.set_actual_phase_intervals(report, self.gate.PHASES[2], 16666667, [0])
        metrics = self.gate.verify_report(report, "macos")["presentation_timing_metrics"]["phases"]
        self.assertEqual(metrics[self.gate.PHASES[1]]["intervals_ns"]["sample_count"], 3750)
        self.assertEqual(metrics[self.gate.PHASES[1]]["cadence"]["lost_slots"], 0)
        self.assertEqual(metrics[self.gate.PHASES[2]]["intervals_ns"]["sample_count"], 3750)
        self.assertEqual(metrics[self.gate.PHASES[2]]["cadence"]["lost_slots"], 1)

    def test_actual_timing_drain_records_are_outside_the_frozen_qualification_endpoint(self):
        report = self.report()
        timing = report["game_benchmark"]["presentation_timing"]
        endpoint = timing["capture_endpoint"]
        pending_id = endpoint["application_present_id"] + 2
        timing["drain"]["request_ids"] = [
            {"swapchain_generation": endpoint["swapchain_generation"],
             "application_present_id": endpoint["application_present_id"] + 1},
            {"swapchain_generation": endpoint["swapchain_generation"],
             "application_present_id": pending_id},
        ]
        final_poll = timing["polls"][-1]
        final_poll.update(status="records", vk_result=0, returned_count=1)
        timing["records"].append({"swapchain_generation": endpoint["swapchain_generation"],
                                   "application_present_id": endpoint["application_present_id"] + 1,
                                   "actual_present_time_ns": timing["records"][-1]["actual_present_time_ns"] + 8333333})
        verified = self.gate.verify_report(report, "macos")
        metrics = verified["presentation_timing_metrics"]
        self.assertEqual(metrics["expected_record_count"], len(timing["successful_requests"]))
        self.assertEqual(metrics["drain_record_count"], 1)
        self.assertEqual(timing["capture_endpoint"], endpoint)

    def test_actual_timing_fails_closed_on_poll_errors_lost_ids_and_duplicate_records(self):
        for mutation in ("unavailable poll", "error poll", "lost ID", "duplicate record"):
            with self.subTest(mutation=mutation):
                report = self.report()
                timing = report["game_benchmark"]["presentation_timing"]
                poll = next(row for row in timing["polls"] if row["scope"] == "loop")
                if mutation == "unavailable poll":
                    poll.update(status="unavailable", vk_result=-7, returned_count=0)
                    with self.assertRaisesRegex(self.gate.GameplayError, "unavailable"):
                        self.gate.verify_report(report, "macos")
                elif mutation == "error poll":
                    poll.update(status="error", vk_result=-1, returned_count=0)
                    with self.assertRaisesRegex(self.gate.GameplayError, "poll failed"):
                        self.gate.verify_report(report, "macos")
                elif mutation == "lost ID":
                    timing["records"].pop()
                    final_request = timing["successful_requests"][-1]
                    final_poll = timing["polls"][final_request["loop_ordinal"] - 1]
                    final_poll.update(status="empty", vk_result=0, returned_count=0)
                    with self.assertRaisesRegex(self.gate.GameplayError, "lost expected"):
                        self.gate.verify_report(report, "macos")
                else:
                    timing["records"].append(copy.deepcopy(timing["records"][0]))
                    timing["polls"][-1].update(status="records", vk_result=0, returned_count=1)
                    with self.assertRaisesRegex(self.gate.GameplayError, "duplicate driver"):
                        self.gate.verify_report(report, "macos")

    def test_actual_timing_rejects_bad_ID_timestamp_and_swapchain_reconciliation(self):
        for mutation in ("unknown record", "duplicate request ID", "nonmonotonic timestamp", "raw ID mismatch",
                         "swapchain recreation", "incomplete drain"):
            with self.subTest(mutation=mutation):
                report = self.report()
                game = report["game_benchmark"]
                timing = game["presentation_timing"]
                if mutation == "unknown record":
                    endpoint = timing["capture_endpoint"]
                    timing["records"].append({"swapchain_generation": endpoint["swapchain_generation"],
                                               "application_present_id": endpoint["application_present_id"] + 1,
                                               "actual_present_time_ns": timing["records"][-1]["actual_present_time_ns"] + 8333333})
                    timing["polls"][-1].update(status="records", vk_result=0, returned_count=1)
                    message = "outside the captured endpoint"
                elif mutation == "duplicate request ID":
                    timing["successful_requests"][1]["application_present_id"] = \
                        timing["successful_requests"][0]["application_present_id"]
                    message = "duplicate successful presentation"
                elif mutation == "nonmonotonic timestamp":
                    timing["records"][30]["actual_present_time_ns"], timing["records"][31]["actual_present_time_ns"] = \
                        timing["records"][31]["actual_present_time_ns"], timing["records"][30]["actual_present_time_ns"]
                    message = "not strictly increasing"
                elif mutation == "raw ID mismatch":
                    frame = next(frame for frame in game["raw_frames"] if frame["phase"] == self.gate.PHASES[1])
                    frame["application_present_id"] += 10
                    message = "does not join its exact"
                elif mutation == "swapchain recreation":
                    timing["successful_requests"][1]["swapchain_generation"] = 2
                    message = "swapchain recreation"
                else:
                    timing["drain"]["complete"] = False
                    message = "drain is incomplete"
                with self.assertRaisesRegex(self.gate.GameplayError, message):
                    self.gate.verify_report(report, "macos")

    def test_actual_timing_needs_500_intervals_per_measured_phase(self):
        report = self.report()
        frames = report["game_benchmark"]["raw_frames"]
        baseline = [frame for frame in frames if frame["phase"] == self.gate.PHASES[1]]
        for frame in baseline[:-499]:
            frame["presentation_succeeded"] = False
        self.reconcile_raw_totals(report)
        with self.assertRaisesRegex(self.gate.GameplayError, "baseline-stationary: insufficient driver presentation intervals"):
            self.gate.verify_report(report, "macos")

    def test_sparse_warmup_raw_frames_may_be_reconciled_by_the_complete_request_map(self):
        report = self.report()
        game = report["game_benchmark"]
        timing = game["presentation_timing"]
        requests, records = timing["successful_requests"], timing["records"]
        first_request = copy.deepcopy(requests[0])
        hidden_request = {**first_request, "application_present_id": 2, "loop_ordinal": 2}
        for request in requests[1:]:
            request["application_present_id"] += 1
            request["loop_ordinal"] += 1
        requests.insert(1, hidden_request)
        hidden_record = {"swapchain_generation": 1, "application_present_id": 2,
                         "actual_present_time_ns": records[0]["actual_present_time_ns"] + 8333333}
        for record in records[1:]:
            record["application_present_id"] += 1
            record["actual_present_time_ns"] += 8333333
        records.insert(1, hidden_record)
        for frame in game["raw_frames"][1:]:
            frame["loop_ordinal"] += 1
            frame["application_present_id"] += 1
        timing["capture_endpoint"]["application_present_id"] += 1
        timing["capture_endpoint"]["loop_ordinal"] += 1
        game["successful_present_requests"] += 1
        timing["polls"].insert(1, {"scope": "loop", "status": "records", "vk_result": 0, "returned_count": 1})
        verified = self.gate.verify_report(report, "macos")
        self.assertEqual(verified["presentation_timing_status"], "passed")

    def test_successful_measured_raw_frame_cannot_be_missing_from_request_map(self):
        report = self.report()
        timing = report["game_benchmark"]["presentation_timing"]
        measured = next(request for request in timing["successful_requests"]
                        if request["phase"] == self.gate.PHASES[1])
        timing["successful_requests"].remove(measured)
        timing["records"][:] = [record for record in timing["records"]
                                 if record["application_present_id"] != measured["application_present_id"]]
        timing["polls"][measured["loop_ordinal"] - 1].update(status="empty", vk_result=0, returned_count=0)
        with self.assertRaisesRegex(self.gate.GameplayError, "does not join its exact presentation request ID"):
            self.gate.verify_report(report, "macos")

    def test_slow_windows_performance_cadence_is_required(self):
        with self.subTest(proof_contract="required"), self.assertRaisesRegex(self.gate.GameplayError, "successful-request rate.*below"):
            self.gate.verify_report(self.report("windows", interval_ns=10000000), "windows")
        verified = self.gate.verify_report(self.report("windows"), "windows")
        self.assertTrue(verified["request_cadence_required"])

    def test_short_mac_coverage_movement_cadence_sample_is_diagnostic(self):
        report = self.report(movement_interval_ns=15000000, proof="terrain-coverage")
        verified = self.gate.verify_report(report, "macos")
        self.assertEqual(verified["request_cadence_status"], "failed")
        self.assertEqual(verified["request_cadence_proxy"][self.gate.PHASES[2]]["intervals_ns"]["sample_count"], 400)
        self.assertIn("speed-200-movement: insufficient cadence samples (400 intervals, requires 500)",
                      verified["request_cadence_reasons"])
        self.gate.verify_report(report, "macos")

    def test_mac_actual_fifo_rejects_slow_presentation_intervals(self):
        report = self.report(proof="performance")
        self.set_actual_phase_intervals(report, self.gate.PHASES[2], 32000000)

        with self.assertRaisesRegex(self.gate.GameplayError, "driver presentation (rate|lost FIFO refresh-slot share)"):
            self.gate.verify_report(report, "macos")

    def test_mac_fifo_cross_phase_interval_is_counted_once_in_starting_phase(self):
        report = self.report(proof="terrain-coverage")
        frames = report["game_benchmark"]["raw_frames"]
        first_movement = next(frame for frame in frames if frame["phase"] == self.gate.PHASES[2])
        first_movement["loop_duration_ns"] = 8333334
        self.reconcile_contiguous_durations(report)

        verified = self.gate.verify_report(report, "macos")
        proxies = verified["request_cadence_proxy"]
        baseline_phase, movement_phase, recovered_phase = (self.gate.PHASES[index] for index in (1, 2, 4))
        successes = {phase: sum(frame["phase"] == phase and frame["presentation_succeeded"] for frame in frames)
                     for phase in (baseline_phase, movement_phase, recovered_phase)}
        baseline = proxies[baseline_phase]
        self.assertEqual(baseline["intervals_ns"]["sample_count"], successes[baseline_phase])
        self.assertEqual(baseline["intervals_ns"]["max"], 8000000)
        movement = proxies[movement_phase]
        self.assertEqual(movement["intervals_ns"]["sample_count"], successes[movement_phase])
        self.assertEqual(movement["intervals_ns"]["max"], 8333334)
        self.assertEqual(movement["cadence"]["lost_slots"], 0)
        self.assertEqual(movement["cadence"]["total_slots"], successes[movement_phase])
        self.assertEqual(proxies[recovered_phase]["intervals_ns"]["sample_count"], successes[recovered_phase])
        self.assertEqual(sum(proxies[phase]["intervals_ns"]["sample_count"]
                             for phase in (baseline_phase, movement_phase, recovered_phase)),
                         sum(successes.values()))

    def test_mac_fifo_does_not_infer_an_interval_before_first_raw_success(self):
        report = self.report(proof="terrain-coverage")
        frames = report["game_benchmark"]["raw_frames"]
        frames[0]["presentation_succeeded"] = False
        self.reconcile_raw_totals(report)

        verified = self.gate.verify_report(report, "macos")
        baseline_phase = self.gate.PHASES[1]
        baseline_successes = sum(frame["phase"] == baseline_phase and frame["presentation_succeeded"] for frame in frames)
        self.assertEqual(verified["request_cadence_proxy"][baseline_phase]["intervals_ns"]["sample_count"],
                         baseline_successes - 1)

    def test_raw_cadence_must_be_computable(self):
        for phase in (self.gate.PHASES[1], self.gate.PHASES[2], self.gate.PHASES[4]):
            for successes in (0, 1):
                with self.subTest(proof_contract="required", phase=phase, successes=successes):
                    report = self.report()
                    selected = [frame for frame in report["game_benchmark"]["raw_frames"] if frame["phase"] == phase]
                    for index, frame in enumerate(selected):
                        frame["presentation_succeeded"] = index < successes
                    self.reconcile_raw_totals(report)
                    with self.assertRaisesRegex(self.gate.GameplayError, "presentation"):
                        self.gate.verify_report(report, "macos")

    def test_nonfinite_or_incomplete_raw_timing_fails_both_policies(self):
        for name in ("nonfinite", "missing phase"):
            with self.subTest(proof_contract="required", mutation=name):
                report = self.report()
                frames = report["game_benchmark"]["raw_frames"]
                if name == "nonfinite":
                    frames[1]["loop_duration_ns"] = float("inf")
                else:
                    frames[:] = [frame for frame in frames if frame["phase"] != self.gate.PHASES[2]]
                with self.assertRaises(self.gate.GameplayError):
                    self.gate.verify_report(report, "macos")

    def test_windows_performance_request_target_remains_required(self):
        with self.assertRaisesRegex(self.gate.GameplayError, "successful-request rate.*below"):
            self.gate.verify_report(self.report("windows", 10000000), "windows")

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
        self.assertFalse(report["game_benchmark"]["frame_budget"]["passed"])
        self.assertFalse(report["game_benchmark"]["frame_budget"]["required_for_benchmark_pass"])
        self.assertFalse(verified["request_cadence_required"])
        self.assertTrue(verified["presentation_timing_required"])
        self.assertEqual(verified["request_cadence_status"], "passed")
        for phase in (self.gate.PHASES[1], self.gate.PHASES[2], self.gate.PHASES[4]):
            self.assertEqual(verified["request_cadence_proxy"][phase]["cadence"]["lost_slots"], 0)

    def test_windows_immediate_requires_the_whole_loop_budget_even_with_passing_request_cadence(self):
        report = self.report("windows", interval_ns=8000000)
        for phase in (self.gate.PHASES[1], self.gate.PHASES[2], self.gate.PHASES[4]):
            selected = [frame for frame in report["game_benchmark"]["raw_frames"] if frame["phase"] == phase]
            for frame in selected[100::700]:
                frame["loop_duration_ns"] = 14000000
        self.reconcile_contiguous_durations(report)
        self.assertFalse(report["game_benchmark"]["frame_budget"]["passed"])
        self.assertTrue(report["game_benchmark"]["frame_budget"]["required_for_benchmark_pass"])
        with self.assertRaisesRegex(self.gate.GameplayError, "game-loop time budget"):
            self.gate.verify_report(report, "windows")

    def test_mac_actual_fifo_sustained_119hz_is_below_the_derived_rate_floor(self):
        report = self.report()
        slow_interval = math.ceil(1000000000 / 119)
        self.set_actual_phase_intervals(report, (self.gate.PHASES[1], self.gate.PHASES[2], self.gate.PHASES[4]),
                                        slow_interval)
        with self.assertRaisesRegex(self.gate.GameplayError, "driver presentation rate.*below 119.76Hz"):
            self.gate.verify_report(report, "macos")

    def test_mac_fifo_refresh_slot_loss_and_long_freezes_are_bounded(self):
        for durations, accepted in (([16666667], True), ([16666667] * 8, False), ([108000000], False)):
            with self.subTest(durations=durations):
                report = self.report()
                self.set_actual_phase_intervals(report, self.gate.PHASES[1], durations[0],
                                                range(600, 600 + len(durations)))
                if accepted:
                    metrics = self.gate.verify_report(report, "macos")["presentation_timing_metrics"]["phases"][self.gate.PHASES[1]]
                    self.assertEqual(metrics["cadence"]["lost_slots"], 1)
                    self.assertLessEqual(metrics["cadence"]["lost_slot_share"], 0.002)
                else:
                    with self.assertRaisesRegex(self.gate.GameplayError, "driver presentation"):
                        self.gate.verify_report(report, "macos")

    def test_movement_cadence_blocks_release_when_stationary_cadence_passes(self):
        with self.assertRaisesRegex(self.gate.GameplayError, "speed-200-movement: successful-request rate.*below"):
            self.gate.verify_report(self.report("windows", movement_interval_ns=10000000), "windows")
        report = self.report("macos")
        self.set_actual_phase_intervals(report, self.gate.PHASES[2], 10000000)
        with self.assertRaisesRegex(self.gate.GameplayError, "speed-200-movement: driver presentation"):
            self.gate.verify_report(report, "macos")

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
        report = self.report(interval_ns=8333333)
        game, gap = report["game_benchmark"], 70000000
        first = next(index for index, frame in enumerate(game["raw_frames"]) if frame["phase"] == self.gate.PHASES[1])
        for frame in game["raw_frames"][first:]:
            frame["started_offset_ns"] += gap
        game["full_radius_terrain"]["baseline_stationary_elapsed_ms"] += gap // 1000000
        report["timing"]["elapsed_ms"] += gap // 1000000
        self.reconcile_raw_totals(report)
        verified = self.gate.verify_report(report, "macos")
        self.assertEqual(verified["request_cadence_status"], "failed")
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
        self.assertEqual(self.gate.verify_report(report, "macos")["request_cadence_status"], "failed")

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
                report = self.report()
                mutate(report)
                with self.assertRaises(self.gate.GameplayError):
                    self.gate.verify_report(report, "macos")

    def test_each_full_radius_barrier_requires_exact_handles_and_drained_queues(self):
        for barrier_name in ("initial_barrier", "recovery_barrier"):
            for key, value in (("current_handle_mesh_visible", 205860), ("scan_performed", False), ("worker_outstanding", 1),
                               ("pending_delivery_batches", 1), ("unclassified_current_visible_mesh_detail", 1)):
                with self.subTest(barrier=barrier_name, field=key, proof_contract="required"):
                    report = self.report()
                    report["game_benchmark"]["full_radius_terrain"][barrier_name][key] = value
                    with self.assertRaises(self.gate.GameplayError):
                        self.gate.verify_report(report, "macos")

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
        for proof in ("performance", "terrain-coverage"):
            with self.subTest(proof=proof):
                report = self.report(proof=proof)
                frame = next(frame for frame in report["game_benchmark"]["raw_frames"]
                             if frame["phase"] == self.gate.PHASES[2])
                frame["terrain_near"]["resident"] = 0
                with self.assertRaisesRegex(self.gate.GameplayError, "missing meshes in the current near terrain window"):
                    self.gate.verify_report(report, "macos")

    def test_all_zero_radial_diagnostics_cannot_claim_complete_coverage(self):
        for proof in ("performance", "terrain-coverage"):
            with self.subTest(proof=proof):
                report = self.report(proof=proof)
                frame = report["game_benchmark"]["raw_frames"][0]
                frame["diagnostic_terrain_radial_bands"] = [
                    {"desired": 0, "resident": 0, "nonresident": 0,
                     "current_mesh_visible": 0, "stale_or_missing_mesh": 0} for _ in range(16)]
                frame["diagnostic_terrain_spatial_scan_duration_ns"] = 2_345_000
                with self.assertRaisesRegex(self.gate.GameplayError, "radial desired counts disagree with render radius"):
                    self.gate.verify_report(report, "macos")

    def test_consistent_underreported_radial_diagnostics_are_rejected(self):
        for proof in ("performance", "terrain-coverage"):
            with self.subTest(proof=proof):
                report = self.report(proof=proof)
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

    def test_recovery_barrier_must_be_on_the_travel_destination_side_of_the_origin(self):
        report = self.report()
        full = report["game_benchmark"]["full_radius_terrain"]
        full["recovery_barrier"]["center"] = {"x": 0, "y": 0}
        for frame in report["game_benchmark"]["raw_frames"]:
            if frame["phase"] in (self.gate.PHASES[3], self.gate.PHASES[4]):
                frame["terrain_center"] = {"x": 0, "y": 0}

        with self.assertRaisesRegex(self.gate.GameplayError, "recovery.*destination"):
            self.gate.verify_report(report, "macos")

    def test_presentation_recreation_diagnostics_must_match_the_frame_result(self):
        report = self.report()
        frame = report["game_benchmark"]["raw_frames"][0]
        frame["diagnostic_cpu_presentation_recreation_duration_ns"] = 1
        with self.assertRaisesRegex(self.gate.GameplayError, "without an attempt"):
            self.gate.verify_report(report, "macos")

        report = self.report()
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
        for name, mutate in mutations:
            with self.subTest(name=name, proof_contract="required"):
                report = self.report()
                mutate(report["game_benchmark"]["full_radius_terrain"])
                with self.assertRaises(self.gate.GameplayError):
                    self.gate.verify_report(report, "macos")

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
            self.assertEqual(json.loads(capture_output.getvalue().splitlines()[-1])["request_cadence_status"], "passed")
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
                self.assertEqual(result["platforms"]["macos"]["performance"]["request_cadence_status"], "passed")
                self.assertFalse(result["platforms"]["macos"]["performance"]["request_cadence_required"])
                self.assertTrue(result["platforms"]["macos"]["performance"]["presentation_timing_required"])
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


if __name__ == "__main__":
    unittest.main()
