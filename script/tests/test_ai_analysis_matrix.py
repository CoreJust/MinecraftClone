from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "script/ai_analysis_matrix.py"
MANIFEST_PATH = REPOSITORY / "script/ai_analysis_matrix.json"
HEAD = "a" * 40
TREE = "b" * 40
CANDIDATE = {"head": HEAD, "tree": TREE}


def load_module():
    spec = importlib.util.spec_from_file_location("ai_analysis_matrix", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class AnalysisMatrixTests(unittest.TestCase):
    def setUp(self):
        self.matrix = load_module()
        self.manifest = self.matrix.load_manifest(MANIFEST_PATH)

    def receipt(self):
        rows = []
        for manifest_row in self.manifest["rows"]:
            row_id = manifest_row["id"]
            effective_analysis = None
            if row_id in self.matrix.REQUIRED_ANALYSIS_CONFIGURATION:
                effective_analysis = {
                    "configuration_sha256": "e" * 64,
                    "configuration_key": self.matrix.REQUIRED_ANALYSIS_CONFIGURATION[row_id][1],
                    "observed_settings": self.matrix.REQUIRED_ANALYSIS_CONFIGURATION[row_id][2],
                    "invocation_count": 1,
                    "observed_tools_and_flags": self.matrix.ANALYSIS_TOOL_MARKERS[row_id],
                    "build_output_sha256": "f" * 64,
                }
                if row_id in self.matrix.PER_TRANSLATION_UNIT_ANALYSIS:
                    unit = {
                        "source": "src/main.cpp",
                        "source_sha256": "1" * 64,
                        "source_argv_sha256": "2" * 64,
                        "invoked_tool": self.matrix.PER_TRANSLATION_UNIT_ANALYSIS[row_id]["tool"],
                        "status": "passed",
                        "record_sha256": "3" * 64,
                        "report_sha256": "4" * 64,
                    }
                    effective_analysis.update(
                        {
                            "translation_unit_count": 1,
                            "translation_unit_inventory_sha256": self.matrix._translation_unit_inventory_sha256([unit]),
                            "project_input_sha256": "5" * 64,
                            "translation_units": [unit],
                        }
                    )
            rows.append(
                {
                    "id": row_id,
                    "candidate": dict(CANDIDATE),
                    "status": "passed",
                    "flags": list(manifest_row["flags"]),
                    "environment": dict(manifest_row.get("environment", {})),
                    "tool_versions": {"tool": "tool 1.0"},
                    "commands": [list(command) for command in manifest_row["commands"]],
                    "executed_commands": [
                        {
                            "declared_command": list(command),
                            "argv": list(command)
                            + (["--verbose"] if manifest_row["id"] in self.matrix.REQUIRED_ANALYSIS_CONFIGURATION and command[:2] == ["cmake", "--build"] else []),
                            "return_code": 0,
                            "output_sha256": "d" * 64,
                            "output_bytes": 12,
                        }
                        for command in manifest_row["commands"]
                    ],
                    "effective_compile_flags": (
                        {
                            "compile_commands_sha256": "c" * 64,
                            "command_count": 1,
                            "observed_flags": list(manifest_row["required_compile_flags"]),
                        }
                        if "required_compile_flags" in manifest_row
                        else None
                    ),
                    "effective_analysis": effective_analysis,
                    "diagnostics": "no diagnostics",
                    "diagnostics_sha256": hashlib.sha256(b"no diagnostics").hexdigest(),
                    "duration_seconds": 1,
                }
            )
        return {
            "schema_version": 1,
            "task_id": "MC-AI-0249",
            "manifest_sha256": self.matrix.manifest_sha256(self.manifest),
            "candidate": dict(CANDIDATE),
            "source": dict(CANDIDATE),
            "rows": rows,
        }

    def create_iwyu_fixture(self, root: Path, *, source_count: int = 1, receipt_count: int | None = None):
        for relative in (
            "CMakePresets.json",
            "script/ai_analysis_matrix.py",
            "script/ai_analysis_matrix.json",
            "script/ci/iwyu_gate.py",
            "script/ci/cppcheck_gate.py",
        ):
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("analysis fingerprint fixture\n", encoding="utf-8")

        preset, cache_key, settings = self.matrix.REQUIRED_ANALYSIS_CONFIGURATION["iwyu"]
        build_directory = root / "build" / preset
        build_directory.mkdir(parents=True, exist_ok=True)
        (build_directory / "CMakeCache.txt").write_text(
            f"{cache_key}:STRING={';'.join(settings)}\n", encoding="utf-8"
        )

        compile_commands = []
        sources = []
        for index in range(source_count):
            source = root / "src" / f"unit_{index}.cpp"
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text(f"int unit_{index}() {{ return {index}; }}\n", encoding="utf-8")
            source.with_suffix(".hpp").write_text("struct InitialHeader {};\n", encoding="utf-8")
            sources.append(source)
            compile_commands.append(
                {
                    "directory": str(root),
                    "file": str(source),
                    "arguments": [
                        "clang++",
                        "-std=c++23",
                        "-c",
                        str(source),
                        "-o",
                        str(build_directory / f"unit_{index}.o"),
                    ],
                }
            )
        (build_directory / "compile_commands.json").write_text(
            json.dumps(compile_commands), encoding="utf-8"
        )

        gate_directory = root / self.matrix.PER_TRANSLATION_UNIT_ANALYSIS["iwyu"]["receipt_directory"]
        gate_directory.mkdir(parents=True, exist_ok=True)
        input_digest = self.matrix.project_analysis_input_sha256(root)
        if receipt_count is None:
            receipt_count = source_count
        for source in sources[:receipt_count]:
            source_relative = source.relative_to(root).as_posix()
            compiler_argv = [
                "clang++",
                "-std=c++23",
                "-c",
                str(source),
                "-o",
                str(build_directory / (source.stem + ".o")),
            ]
            output_index = compiler_argv.index("-o")
            object_path = compiler_argv[output_index + 1]
            source_argv = [
                "--driver-mode=g++",
                *compiler_argv[1:output_index],
                "-MD",
                "-MT",
                object_path,
                "-MF",
                object_path + ".d",
                *compiler_argv[output_index:],
            ]
            source_argv_json = json.dumps(source_argv, separators=(",", ":")).encode("utf-8")
            stem = hashlib.sha256(source_relative.encode("utf-8") + b"\0" + source_argv_json).hexdigest()
            log_path = gate_directory / f"{stem}.log"
            record_path = gate_directory / f"{stem}.json"
            raw_report = b"IWYU complete report\n"
            log_path.write_bytes(raw_report)
            record = {
                "schema_version": 1,
                "source": source_relative,
                "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "project_input_sha256": input_digest,
                "source_argv": source_argv,
                "adapter_argv": [
                    "include-what-you-use",
                    "-Xiwyu",
                    "--no_fwd_decls",
                    "-Xiwyu",
                    "--error",
                    "--",
                    *source_argv,
                ],
                "invoked_tool": "include-what-you-use",
                "tool_argv": [
                    "include-what-you-use",
                    "-Xiwyu",
                    "--no_fwd_decls",
                    *source_argv,
                ],
                "tool_exit_code": 0,
                "report_sha256": hashlib.sha256(raw_report).hexdigest(),
                "status": "passed",
                "public_additions": [],
                "removals": [],
                "forward_declarations": [],
                "mapped_facades": [],
                "unsupported_suggestions": [],
                "log": log_path.relative_to(root).as_posix(),
                "record": record_path.relative_to(root).as_posix(),
            }
            record_path.write_text(json.dumps(record), encoding="utf-8")
        return sources

    def create_cppcheck_fixture(self, root: Path, *, source_count: int = 1, receipt_count: int | None = None):
        sources = self.create_iwyu_fixture(root, source_count=source_count, receipt_count=0)
        preset, cache_key, settings = self.matrix.REQUIRED_ANALYSIS_CONFIGURATION["cppcheck"]
        build_directory = root / "build" / preset
        build_directory.mkdir(parents=True, exist_ok=True)
        (build_directory / "CMakeCache.txt").write_text(
            f"{cache_key}:STRING={';'.join(settings)}\n"
            "CMAKE_C_CPPCHECK:STRING=\n"
            "CMAKE_CXX_CPPCHECK:STRING=\n",
            encoding="utf-8",
        )
        compile_commands = json.loads(
            (root / "build" / "analysis-iwyu" / "compile_commands.json").read_text(encoding="utf-8")
        )
        for index, entry in enumerate(compile_commands):
            entry["arguments"][-1] = str(build_directory / f"unit_{index}.o")
        (build_directory / "compile_commands.json").write_text(json.dumps(compile_commands), encoding="utf-8")

        gate_directory = root / self.matrix.PER_TRANSLATION_UNIT_ANALYSIS["cppcheck"]["receipt_directory"]
        gate_directory.mkdir(parents=True, exist_ok=True)
        input_digest = self.matrix.project_analysis_input_sha256(root)
        if receipt_count is None:
            receipt_count = source_count
        severity_counts = {
            "error": 0,
            "warning": 0,
            "performance": 0,
            "portability": 0,
            "style": 0,
            "information": 0,
            "debug": 0,
        }
        for index, source in enumerate(sources[:receipt_count]):
            source_relative = source.relative_to(root).as_posix()
            compile_argv = compile_commands[index]["arguments"]
            output_index = compile_argv.index("-o")
            object_path = compile_argv[output_index + 1]
            compiler_argv = [
                *compile_argv[:output_index],
                "-MD",
                "-MT",
                object_path,
                "-MF",
                object_path + ".d",
                *compile_argv[output_index:],
            ]
            cppcheck_argv = ["cppcheck", "--enable=all", "--inline-suppr", "--xml", str(source)]
            source_sha256 = hashlib.sha256(source.read_bytes()).hexdigest()
            identity = hashlib.sha256(
                self.matrix._canonical_json(
                    {
                        "source_absolute": str(source.resolve()),
                        "source_sha256": source_sha256,
                        "project_input_sha256": input_digest,
                        "cppcheck_argv": cppcheck_argv,
                        "compiler_argv": compiler_argv,
                        "cwd": str(root.resolve()),
                    }
                )
            ).hexdigest()
            record_path = gate_directory / f"{identity}.json"
            xml_path = gate_directory / f"{identity}.xml"
            raw_report = b"<?xml version=\"1.0\"?><results version=\"2\"/>\n"
            xml_path.write_bytes(raw_report)
            record = {
                "schema": 1,
                "gate": "cppcheck-per-tu",
                "status": "pass",
                "failure_stage": None,
                "cwd": str(root.resolve()),
                "source": {
                    "absolute": str(source.resolve()),
                    "relative": source_relative,
                    "sha256": source_sha256,
                },
                "identity_sha256": identity,
                "project_input_sha256": input_digest,
                "raw_report_sha256": hashlib.sha256(raw_report).hexdigest(),
                "cppcheck": {
                    "argv": cppcheck_argv,
                    "version": "2.17",
                    "returncode": 0,
                    "elapsed_ms": 1,
                },
                "compiler": {"argv": compiler_argv, "returncode": 0, "elapsed_ms": 1},
                "summary": {"blocking": 0, "advisory": 0, "by_severity": severity_counts},
                "diagnostics": [],
                "artifacts": {
                    "root": str(gate_directory.resolve()),
                    "xml": str(xml_path.resolve()),
                    "receipt": str(record_path.resolve()),
                },
            }
            record_path.write_text(json.dumps(record), encoding="utf-8")
        return sources

    def test_manifest_contains_exact_acceptance_rows(self):
        self.assertEqual(
            tuple(row["id"] for row in self.manifest["rows"]),
            self.matrix.REQUIRED_ROW_IDS,
        )

    def test_android_clang_tidy_is_a_required_matrix_row(self):
        self.assertIn("android_clang_tidy", self.matrix.REQUIRED_ROW_IDS)
        row = next(item for item in self.manifest["rows"] if item["id"] == "android_clang_tidy")
        self.assertEqual(row["platform"], "android-arm64")
        self.assertIn("clang-tidy", row["tool_probes"][0])
        self.assertIn("--checks=clang-analyzer-*", row["commands"][0])
        self.assertIn("--warnings-as-errors=clang-analyzer-*", row["commands"][0])

    def test_clang_static_analysis_probes_scan_build_with_supported_help_option(self):
        row = next(item for item in self.manifest["rows"] if item["id"] == "clang_static_analysis")

        self.assertEqual(row["tool_probes"], [["clang", "--version"], ["scan-build", "--help"]])
        self.assertEqual(
            row["commands"],
            [
                ["cmake", "--preset", "analysis-clang-static"],
                ["cmake", "--build", "--preset", "analysis-clang-static"],
            ],
        )

    def test_msvc_rows_use_zero_exit_version_probe_and_keep_required_analysis_commands(self):
        rows = {row["id"]: row for row in self.manifest["rows"]}
        for row_id, preset, build_directory in (
            ("windows_msvc_asan", "analysis-windows-asan", "build/analysis-windows-asan"),
            ("windows_msvc_analyze", "analysis-windows-analyze", "build/analysis-windows-analyze"),
        ):
            with self.subTest(row=row_id):
                row = rows[row_id]
                self.assertEqual(row["tool_probes"], [["cl.exe", "/Bv", "/?"], ["cmake", "--version"]])
                self.assertEqual(
                    row["commands"],
                    [
                        ["cmake", "--preset", preset],
                        ["cmake", "--build", "--preset", preset],
                        ["ctest", "--test-dir", build_directory, "--output-on-failure", "--no-tests=error"],
                    ],
                )

    def test_nonzero_msvc_version_probe_keeps_row_unavailable(self):
        row = next(item for item in self.manifest["rows"] if item["id"] == "windows_msvc_asan")
        failed_probe = self.matrix.subprocess.CompletedProcess(
            row["tool_probes"][0], 2, "", "cl.exe: error D8003: missing source filename"
        )

        with tempfile.TemporaryDirectory() as directory:
            receipt_path = Path(directory) / "row.json"
            with mock.patch.object(self.matrix, "require_clean_checkout"), mock.patch.object(
                self.matrix, "_row_supported_on_host", return_value=True
            ), mock.patch.object(self.matrix.subprocess, "run", return_value=failed_probe) as run:
                receipt = self.matrix.run_row(
                    row["id"], receipt_path, MANIFEST_PATH, root=REPOSITORY, expected_candidate=CANDIDATE
                )

        result = receipt["rows"][0]
        self.assertEqual(result["status"], "unavailable")
        self.assertIn("exited 2", result["diagnostics"])
        self.assertEqual(run.call_count, 1)

    def test_android_clang_tidy_receipt_requires_the_exact_analyzer_invocation(self):
        receipt = self.receipt()
        row = next(item for item in receipt["rows"] if item["id"] == "android_clang_tidy")
        row["executed_commands"][0]["argv"].remove("--warnings-as-errors=clang-analyzer-*")
        with self.assertRaisesRegex(self.matrix.MatrixError, "exact required analyzer command"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_complete_candidate_bound_receipt_passes(self):
        self.assertIsNotNone(self.matrix.validate_receipt(self.receipt(), self.manifest, CANDIDATE))

    def test_missing_row_fails_closed(self):
        receipt = self.receipt()
        receipt["rows"].pop()
        with self.assertRaisesRegex(self.matrix.MatrixError, "missing rows"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_unavailable_row_cannot_be_release_evidence(self):
        receipt = self.receipt()
        receipt["rows"][0]["status"] = "unavailable"
        with self.assertRaisesRegex(self.matrix.MatrixError, "not passed"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_candidate_and_manifest_bindings_are_checked(self):
        receipt = self.receipt()
        receipt["candidate"]["tree"] = "c" * 40
        with self.assertRaisesRegex(self.matrix.MatrixError, "source binding|different candidate|requested candidate"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

        receipt = self.receipt()
        receipt["manifest_sha256"] = "0" * 64
        with self.assertRaisesRegex(self.matrix.MatrixError, "manifest_sha256"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_receipt_requires_tool_commands_flags_and_diagnostics(self):
        for field in ("tool_versions", "commands", "flags", "diagnostics"):
            receipt = self.receipt()
            if field == "diagnostics":
                receipt["rows"][0].pop(field)
            else:
                receipt["rows"][0][field] = []
            with self.subTest(field=field), self.assertRaises(self.matrix.MatrixError):
                self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_receipt_requires_declared_row_environment(self):
        receipt = self.receipt()
        receipt["rows"][0]["environment"] = {"ASAN_OPTIONS": "detect_leaks=0"}
        with self.assertRaisesRegex(self.matrix.MatrixError, "environment"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_receipt_requires_observed_effective_compile_flags(self):
        receipt = self.receipt()
        row = next(row for row in receipt["rows"] if row["id"] == "macos_asan")
        row["effective_compile_flags"] = None
        with self.assertRaisesRegex(self.matrix.MatrixError, "effective compiler flags"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_receipt_requires_each_command_to_have_successful_execution_evidence(self):
        receipt = self.receipt()
        row = receipt["rows"][0]
        row["executed_commands"] = []
        with self.assertRaisesRegex(self.matrix.MatrixError, "every launched command"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

        receipt = self.receipt()
        receipt["rows"][0]["executed_commands"][0]["return_code"] = 1
        with self.assertRaisesRegex(self.matrix.MatrixError, "did not exit successfully"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_static_analysis_receipts_require_effective_tool_invocation(self):
        for row_id in self.matrix.REQUIRED_ANALYSIS_CONFIGURATION:
            receipt = self.receipt()
            row = next(item for item in receipt["rows"] if item["id"] == row_id)
            row["effective_analysis"]["invocation_count"] = 0
            with self.subTest(row_id=row_id), self.assertRaisesRegex(
                self.matrix.MatrixError, "no observed analyzer invocation"
            ):
                self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_effective_analysis_capture_requires_cache_flags_and_verbose_invocation(self):
        for row_id, (preset, cache_key, settings) in self.matrix.REQUIRED_ANALYSIS_CONFIGURATION.items():
            if row_id in self.matrix.PER_TRANSLATION_UNIT_ANALYSIS:
                continue
            with self.subTest(row_id=row_id), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                cache_path = root / "build" / preset / "CMakeCache.txt"
                cache_path.parent.mkdir(parents=True)
                cache_path.write_text(f"{cache_key}:STRING={' '.join(settings)}\n", encoding="utf-8")
                output = " ".join(self.matrix.ANALYSIS_TOOL_MARKERS[row_id])
                captured = self.matrix.capture_effective_analysis(root, {"id": row_id}, output)
                self.assertEqual(captured["invocation_count"], 1)
                self.assertEqual(captured["observed_settings"], settings)

                with self.assertRaisesRegex(self.matrix.MatrixError, "no configured analyzer invocation"):
                    self.matrix.capture_effective_analysis(root, {"id": row_id}, "build completed")

    def test_cmake_dependency_flags_must_match_the_expected_object_and_depfile(self):
        compile_argv = ["clang++", "-std=c++23", "-o", "build/unit.o", "-c", "src/unit.cpp"]
        observed = [
            *compile_argv[:2],
            "-MD",
            "-MT",
            "build/unit.o",
            "-MF",
            "build/unit.o.d",
            *compile_argv[2:],
        ]

        self.assertTrue(self.matrix._compiler_argv_matches(compile_argv, observed))
        self.assertTrue(
            self.matrix._iwyu_argv_matches(
                compile_argv[1:], ["--driver-mode=g++", *observed[1:]]
            )
        )
        self.assertTrue(
            self.matrix._iwyu_argv_matches(
                compile_argv[1:], [*observed[1:], "--driver-mode=g++"]
            )
        )

        wrong_object = list(observed)
        wrong_object[wrong_object.index("-MT") + 1] = "build/other.o"
        wrong_depfile = list(observed)
        wrong_depfile[wrong_depfile.index("-MF") + 1] = "build/other.o.d"
        extra_argument = list(observed)
        extra_argument.insert(extra_argument.index("-o"), "-fextra")
        for invalid in (wrong_object, wrong_depfile, extra_argument):
            with self.subTest(invalid=invalid):
                self.assertFalse(self.matrix._compiler_argv_matches(compile_argv, invalid))

    def test_iwyu_capture_requires_exact_owned_translation_unit_receipts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_iwyu_fixture(root)

            captured = self.matrix.capture_effective_analysis(root, {"id": "iwyu"}, "incremental build")

        self.assertEqual(captured["translation_unit_count"], 1)
        self.assertEqual(captured["invocation_count"], 1)
        self.assertEqual(captured["translation_units"][0]["invoked_tool"], "include-what-you-use")
        self.assertEqual(captured["translation_units"][0]["status"], "passed")
        self.assertEqual(len(captured["translation_unit_inventory_sha256"]), 64)

    def test_iwyu_capture_rejects_changed_owned_header_and_missing_translation_unit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = self.create_iwyu_fixture(root)
            header = sources[0].with_suffix(".hpp")
            header.write_text("struct ChangedHeader {};\n", encoding="utf-8")

            with self.assertRaisesRegex(self.matrix.MatrixError, "stale project inputs"):
                self.matrix.capture_effective_analysis(root, {"id": "iwyu"}, "incremental build")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_iwyu_fixture(root, source_count=2, receipt_count=1)

            with self.assertRaisesRegex(self.matrix.MatrixError, "missing 1 owned translation-unit"):
                self.matrix.capture_effective_analysis(root, {"id": "iwyu"}, "partial build")

    def test_iwyu_capture_rejects_an_extra_receipt_for_a_duplicate_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_iwyu_fixture(root)
            gate_directory = root / self.matrix.PER_TRANSLATION_UNIT_ANALYSIS["iwyu"]["receipt_directory"]
            original_path = next(gate_directory.glob("*.json"))
            duplicate = json.loads(original_path.read_text(encoding="utf-8"))
            duplicate["source_argv"].insert(1, "-DSECOND_PASS=1")
            source_argv_json = json.dumps(duplicate["source_argv"], separators=(",", ":")).encode("utf-8")
            stem = hashlib.sha256(duplicate["source"].encode("utf-8") + b"\0" + source_argv_json).hexdigest()
            duplicate["adapter_argv"] = [
                "include-what-you-use",
                "-Xiwyu",
                "--no_fwd_decls",
                "-Xiwyu",
                "--error",
                "--",
                *duplicate["source_argv"],
            ]
            duplicate["tool_argv"] = [
                "include-what-you-use",
                "-Xiwyu",
                "--no_fwd_decls",
                *duplicate["source_argv"],
            ]
            duplicate_log = gate_directory / f"{stem}.log"
            duplicate_record = gate_directory / f"{stem}.json"
            duplicate_log.write_bytes((root / duplicate["log"]).read_bytes())
            duplicate["log"] = duplicate_log.relative_to(root).as_posix()
            duplicate["record"] = duplicate_record.relative_to(root).as_posix()
            duplicate_record.write_text(json.dumps(duplicate), encoding="utf-8")

            with self.assertRaisesRegex(self.matrix.MatrixError, "stale or unexpected translation-unit"):
                self.matrix.capture_effective_analysis(root, {"id": "iwyu"}, "duplicate invocation")

    def test_iwyu_build_failure_propagates_to_the_analysis_row(self):
        for row_id in ("iwyu", "cppcheck"):
            with tempfile.TemporaryDirectory() as directory:
                receipt_path = Path(directory) / "row.json"
                row = next(row for row in self.manifest["rows"] if row["id"] == row_id)
                probe_results = [
                    self.matrix.subprocess.CompletedProcess(probe, 0, "tool 1.0\n", "")
                    for probe in row["tool_probes"]
                ]
                probe_results.extend(
                    [
                        self.matrix.subprocess.CompletedProcess([], 0, "configured\n", ""),
                        self.matrix.subprocess.CompletedProcess([], 1, "gate failed\n", ""),
                    ]
                )

                with mock.patch.object(self.matrix, "require_clean_checkout"), mock.patch.object(
                    self.matrix, "_row_supported_on_host", return_value=True
                ), mock.patch.object(self.matrix.subprocess, "run", side_effect=probe_results):
                    receipt = self.matrix.run_row(
                        row_id, receipt_path, MANIFEST_PATH, root=REPOSITORY, expected_candidate=CANDIDATE
                    )

            row_receipt = receipt["rows"][0]
            self.assertEqual(row_receipt["status"], "failed")
            self.assertEqual(row_receipt["executed_commands"][-1]["return_code"], 1)
            self.assertIsNone(row_receipt["effective_analysis"])

    @unittest.skipIf(sys.platform == "win32", "CMake IWYU adapter regression uses a POSIX no-op compiler")
    def test_cmake_iwyu_error_sentinel_propagates_wrapper_exit_status(self):
        cmake = shutil.which("cmake")
        no_op_compiler = shutil.which("true")
        if cmake is None or no_op_compiler is None:
            self.skipTest("cmake and POSIX true are required for the CMake adapter regression")

        failing_iwyu = f"{sys.executable};-c;raise SystemExit(7)"
        base_command = [
            cmake,
            "-E",
            "__run_co_compile",
            "--source=Chunk.cpp",
            "--",
            no_op_compiler,
            "-c",
            "Chunk.cpp",
        ]
        swallowed = subprocess.run(
            [*base_command[:3], f"--iwyu={failing_iwyu}", *base_command[3:]],
            check=False,
            capture_output=True,
        )
        propagated = subprocess.run(
            [*base_command[:3], f"--iwyu={failing_iwyu};-Xiwyu;--error", *base_command[3:]],
            check=False,
            capture_output=True,
        )

        self.assertEqual(swallowed.returncode, 0, swallowed.stderr.decode(errors="replace"))
        self.assertEqual(propagated.returncode, 7, propagated.stderr.decode(errors="replace"))

    def test_cppcheck_capture_requires_exact_receipts_and_current_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = self.create_cppcheck_fixture(root)

            captured = self.matrix.capture_effective_analysis(root, {"id": "cppcheck"}, "incremental build")
            self.assertEqual(captured["translation_unit_count"], 1)
            self.assertEqual(captured["translation_units"][0]["invoked_tool"], "cppcheck")

            sources[0].with_suffix(".hpp").write_text("struct ChangedHeader {};\n", encoding="utf-8")
            with self.assertRaisesRegex(self.matrix.MatrixError, "stale project inputs"):
                self.matrix.capture_effective_analysis(root, {"id": "cppcheck"}, "incremental build")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_cppcheck_fixture(root, source_count=2, receipt_count=1)
            with self.assertRaisesRegex(self.matrix.MatrixError, "missing 1 owned translation-unit"):
                self.matrix.capture_effective_analysis(root, {"id": "cppcheck"}, "partial build")

    def test_cppcheck_capture_rejects_changed_gate_policy_and_compile_commands(self):
        for changed_input in ("CMakePresets.json", "script/ci/cppcheck_gate.py"):
            with self.subTest(changed_input=changed_input), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self.create_cppcheck_fixture(root)
                policy_path = root / changed_input
                policy_path.write_text(policy_path.read_text(encoding="utf-8") + "changed\n", encoding="utf-8")

                with self.assertRaisesRegex(self.matrix.MatrixError, "stale project inputs"):
                    self.matrix.capture_effective_analysis(root, {"id": "cppcheck"}, "incremental build")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_cppcheck_fixture(root)
            compile_commands_path = root / "build/analysis-cppcheck/compile_commands.json"
            compile_commands = json.loads(compile_commands_path.read_text(encoding="utf-8"))
            compile_commands[0]["arguments"].insert(1, "-DCHANGED_COMMAND")
            compile_commands_path.write_text(json.dumps(compile_commands), encoding="utf-8")

            with self.assertRaisesRegex(self.matrix.MatrixError, "does not match compile_commands.json"):
                self.matrix.capture_effective_analysis(root, {"id": "cppcheck"}, "incremental build")

    def test_effective_compile_flags_are_read_from_generated_commands(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            compile_commands = root / "compile_commands.json"
            compile_commands.write_text(
                json.dumps(
                    [
                        {
                            "directory": str(root),
                            "file": str(root / "main.cpp"),
                            "arguments": ["clang++", "-fsanitize=address", "-c", "main.cpp"],
                        },
                        {
                            "directory": str(root),
                            "file": str(root / "tests.cpp"),
                            "command": 'clang++ -fsanitize=address -c "tests.cpp"',
                        },
                    ]
                ),
                encoding="utf-8",
            )
            row = {
                "id": "macos_asan",
                "compile_commands": "compile_commands.json",
                "required_compile_flags": ["-fsanitize=address"],
            }

            result = self.matrix.capture_effective_compile_flags(root, row)

        self.assertEqual(result["command_count"], 2)
        self.assertEqual(result["observed_flags"], ["-fsanitize=address"])
        self.assertEqual(len(result["compile_commands_sha256"]), 64)

    def test_effective_compile_flag_capture_rejects_uninstrumented_commands(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": str(root),
                            "file": str(root / "main.cpp"),
                            "arguments": ["clang++", "-c", "main.cpp"],
                        }
                    ]
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(self.matrix.MatrixError, "-fsanitize=address"):
                self.matrix.capture_effective_compile_flags(
                    root,
                    {
                        "id": "macos_asan",
                        "compile_commands": "compile_commands.json",
                        "required_compile_flags": ["-fsanitize=address"],
                    },
                )

    def test_row_runner_records_an_executed_passing_row(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt_path = Path(directory) / "row.json"
            row = self.manifest["rows"][0]
            row["platform"] = self.matrix._host_platform()

            successful_output = "x" * 16_385 + "successful output tail"

            def successful_command(argv, **kwargs):
                output = successful_output if argv[:2] == ["cmake", "--preset"] else "tool output"
                return self.matrix.subprocess.CompletedProcess(argv, 0, output, "")

            with mock.patch.object(self.matrix, "require_clean_checkout"), mock.patch.object(
                self.matrix,
                "capture_effective_compile_flags",
                return_value={
                    "compile_commands_sha256": "c" * 64,
                    "command_count": 1,
                    "observed_flags": list(row["required_compile_flags"]),
                },
            ), mock.patch.object(
                self.matrix,
                "_row_supported_on_host",
                return_value=True,
            ), mock.patch.object(
                self.matrix.subprocess,
                "run",
                side_effect=successful_command,
            ):
                receipt = self.matrix.run_row(
                    row["id"], receipt_path, MANIFEST_PATH, root=REPOSITORY, expected_candidate=CANDIDATE
                )
            result = receipt["rows"][0]
            configure_index = next(
                index
                for index, command in enumerate(row["commands"])
                if command[:2] == ["cmake", "--preset"]
            )
            self.assertEqual(result["status"], "passed")
            self.assertEqual(result["commands"], row["commands"])
            self.assertEqual(result["environment"], row.get("environment", {}))
            self.assertIn("tool output", result["diagnostics"])
            self.assertIn("[output truncated by analysis-matrix]", result["diagnostics"])
            self.assertNotIn("successful output tail", result["diagnostics"])
            self.assertEqual(len(result["executed_commands"]), len(row["commands"]))
            self.assertEqual(result["executed_commands"][configure_index]["return_code"], 0)
            self.assertEqual(
                result["executed_commands"][configure_index]["output_bytes"],
                len(successful_output.encode("utf-8")),
            )
            self.assertEqual(
                result["executed_commands"][configure_index]["output_sha256"],
                hashlib.sha256(successful_output.encode("utf-8")).hexdigest(),
            )

    def test_row_runner_retains_complete_failed_command_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt_path = Path(directory) / "row.json"
            row = self.manifest["rows"][0]
            row["platform"] = self.matrix._host_platform()
            stdout = "x" * 17_000 + "\nclang: error: trailing sanitizer build failure\n"
            stderr = "ninja: build stopped: subcommand failed\n"
            command_output = stdout + stderr
            probe_results = [
                self.matrix.subprocess.CompletedProcess(probe, 0, "tool 1.0\n", "")
                for probe in row["tool_probes"]
            ]
            probe_results.append(self.matrix.subprocess.CompletedProcess([], 1, stdout, stderr))

            with mock.patch.object(self.matrix, "require_clean_checkout"), mock.patch.object(
                self.matrix,
                "_row_supported_on_host",
                return_value=True,
            ), mock.patch.object(
                self.matrix.subprocess,
                "run",
                side_effect=probe_results,
            ):
                self.matrix.run_row(
                    row["id"], receipt_path, MANIFEST_PATH, root=REPOSITORY, expected_candidate=CANDIDATE
                )

            result = json.loads(receipt_path.read_text(encoding="utf-8"))["rows"][0]
            self.assertEqual(result["status"], "failed")
            self.assertIn(command_output, result["diagnostics"])
            self.assertIn("clang: error: trailing sanitizer build failure", result["diagnostics"])
            self.assertEqual(len(result["executed_commands"]), 1)
            execution = result["executed_commands"][0]
            self.assertEqual(execution["return_code"], 1)
            self.assertEqual(execution["output_bytes"], len(command_output.encode("utf-8")))
            self.assertEqual(
                execution["output_sha256"],
                hashlib.sha256(command_output.encode("utf-8")).hexdigest(),
            )

    def test_row_runner_retains_partial_timeout_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt_path = Path(directory) / "row.json"
            row = self.manifest["rows"][0]
            row["platform"] = self.matrix._host_platform()
            partial_stdout = b"x" * 17_000 + b"\nclang: error: sanitizer build timed out\n\xff"
            partial_stderr = "ninja: still waiting for a stopped job\n"
            timeout_error = self.matrix.subprocess.TimeoutExpired(
                row["commands"][0], 30, output=partial_stdout, stderr=partial_stderr
            )
            probe_results = [
                self.matrix.subprocess.CompletedProcess(probe, 0, "tool 1.0\n", "")
                for probe in row["tool_probes"]
            ]
            probe_results.append(timeout_error)

            with mock.patch.object(self.matrix, "require_clean_checkout"), mock.patch.object(
                self.matrix,
                "_row_supported_on_host",
                return_value=True,
            ), mock.patch.object(
                self.matrix.subprocess,
                "run",
                side_effect=probe_results,
            ):
                self.matrix.run_row(
                    row["id"], receipt_path, MANIFEST_PATH, root=REPOSITORY, expected_candidate=CANDIDATE
                )

            result = json.loads(receipt_path.read_text(encoding="utf-8"))["rows"][0]
            self.assertEqual(result["status"], "failed")
            self.assertIn(
                partial_stdout.decode("utf-8", errors="replace") + partial_stderr,
                result["diagnostics"],
            )
            self.assertIn("clang: error: sanitizer build timed out", result["diagnostics"])
            self.assertIn("\ufffd", result["diagnostics"])
            self.assertEqual(result["executed_commands"], [])

    def test_hosted_rows_reject_a_candidate_that_differs_from_the_checkout(self):
        with mock.patch.object(self.matrix, "candidate_identity", return_value={"head": "c" * 40, "tree": TREE}):
            with self.assertRaisesRegex(self.matrix.MatrixError, "does not match the checkout"):
                self.matrix.require_clean_checkout(REPOSITORY, CANDIDATE)

    def test_hosted_rows_reject_a_dirty_working_tree(self):
        with mock.patch.object(self.matrix, "candidate_identity", return_value=dict(CANDIDATE)), mock.patch.object(
            self.matrix,
            "_git",
            return_value=TREE,
        ), mock.patch.object(
            self.matrix.subprocess,
            "run",
            return_value=self.matrix.subprocess.CompletedProcess([], 0, " M source.cpp\n", ""),
        ):
            with self.assertRaisesRegex(self.matrix.MatrixError, "clean committed checkout"):
                self.matrix.require_clean_checkout(REPOSITORY, CANDIDATE)

    def test_row_runner_records_unsupported_host_as_unavailable(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt_path = Path(directory) / "row.json"
            host = self.matrix._host_platform()
            row_id = "windows_msvc_asan" if host != "windows" else "linux_lsan"
            with mock.patch.object(self.matrix, "require_clean_checkout"):
                receipt = self.matrix.run_row(
                    row_id, receipt_path, MANIFEST_PATH, root=REPOSITORY, expected_candidate=CANDIDATE
                )
            self.assertEqual(receipt["rows"][0]["status"], "unavailable")
            self.assertIn("requires", receipt["rows"][0]["diagnostics"])

    def test_merger_requires_every_passing_candidate_bound_row(self):
        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            complete = self.receipt()
            row_paths = []
            for index, row in enumerate(complete["rows"]):
                row_path = directory_path / f"row-{index}.json"
                row_receipt = dict(complete)
                row_receipt["rows"] = [row]
                row_path.write_text(json.dumps(row_receipt), encoding="utf-8")
                row_paths.append(row_path)
            output_path = directory_path / "complete.json"
            merged = self.matrix.merge_row_receipts(
                row_paths, output_path, MANIFEST_PATH, root=REPOSITORY, expected_candidate=CANDIDATE
            )
            self.assertEqual(len(merged["rows"]), len(self.matrix.REQUIRED_ROW_IDS))
            self.assertIsNotNone(self.matrix.validate_receipt(merged, self.manifest, CANDIDATE))

    def test_candidate_identity_hashes_the_staged_tree_without_writing_git_state(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "nested").mkdir()
            (root / "file.txt").write_text("initial\n", encoding="utf-8")
            (root / "nested/file.txt").write_text("nested\n", encoding="utf-8")
            subprocess.run(["git", "init"], cwd=root, check=True, capture_output=True)
            subprocess.run(["git", "add", "."], cwd=root, check=True, capture_output=True)
            subprocess.run(
                ["git", "-c", "user.name=Matrix", "-c", "user.email=matrix@example.test", "commit", "-m", "initial"],
                cwd=root,
                check=True,
                capture_output=True,
            )
            (root / "nested/file.txt").write_text("changed\n", encoding="utf-8")
            subprocess.run(["git", "add", "nested/file.txt"], cwd=root, check=True, capture_output=True)
            expected = subprocess.run(
                ["git", "write-tree"], cwd=root, check=True, capture_output=True, text=True
            ).stdout.strip()
            self.assertEqual(self.matrix.candidate_identity(root)["tree"], expected)

    def test_duplicate_and_extra_rows_fail_closed(self):
        receipt = self.receipt()
        receipt["rows"].append(copy.deepcopy(receipt["rows"][0]))
        with self.assertRaisesRegex(self.matrix.MatrixError, "duplicate"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

        receipt = self.receipt()
        receipt["rows"][0]["id"] = "not-a-matrix-row"
        with self.assertRaisesRegex(self.matrix.MatrixError, "unexpected"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_local_stub_is_explicitly_unavailable_and_verifier_rejects_it(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt_path = Path(directory) / "analysis.json"
            self.matrix.run_local_stub(
                receipt_path,
                MANIFEST_PATH,
                root=REPOSITORY,
                expected_candidate=CANDIDATE,
            )
            generated = json.loads(receipt_path.read_text(encoding="utf-8"))
            self.assertTrue(all(row["status"] == "unavailable" for row in generated["rows"]))
            with self.assertRaisesRegex(self.matrix.MatrixError, "not passed"):
                self.matrix.validate_receipt(generated, self.manifest, CANDIDATE)

    def test_cli_verifies_a_complete_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt_path = Path(directory) / "analysis.json"
            receipt_path.write_text(json.dumps(self.receipt()), encoding="utf-8")
            self.assertEqual(
                self.matrix.main(
                    [
                        "verify",
                        str(receipt_path),
                        "--manifest",
                        str(MANIFEST_PATH),
                        "--head",
                        HEAD,
                        "--tree",
                        TREE,
                    ]
                ),
                0,
            )

    def test_cli_fails_after_writing_a_nonpassing_row_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt_path = Path(directory) / "row.json"
            result = {
                "rows": [{"status": "unavailable"}],
            }
            with mock.patch.object(self.matrix, "run_row", return_value=result):
                self.assertEqual(
                    self.matrix.main(
                        ["run-row", str(receipt_path), "--row", "macos_asan"]
                    ),
                    1,
                )

    def test_hosted_row_commands_expand_only_declared_environment_placeholders(self):
        self.assertEqual(
            self.matrix._expand_environment(
                ["cmake", "-DROOT={env:VCPKG_ROOT}", "literal"],
                {"VCPKG_ROOT": "/tmp/vcpkg"},
            ),
            ["cmake", "-DROOT=/tmp/vcpkg", "literal"],
        )
        with self.assertRaisesRegex(self.matrix.MatrixError, "VCPKG_ROOT"):
            self.matrix._expand_environment(["{env:VCPKG_ROOT}"], {})

    def test_host_support_includes_linux_analysis_and_android_emulator_rows(self):
        with mock.patch.object(self.matrix, "_host_platform", return_value="linux"):
            self.assertTrue(self.matrix._row_supported_on_host({"platform": "linux"}))
            self.assertTrue(self.matrix._row_supported_on_host({"platform": "android-arm64"}))
            self.assertTrue(self.matrix._row_supported_on_host({"platform": "hosted"}))

    def test_manifest_requires_standalone_lsan_and_full_msan_rows(self):
        rows = {row["id"]: row for row in self.manifest["rows"]}
        self.assertEqual(rows["linux_lsan"]["platform"], "linux")
        self.assertIn("-fsanitize=leak", rows["linux_lsan"]["flags"])
        self.assertEqual(rows["linux_msan"]["platform"], "linux")
        self.assertIn("-fsanitize=memory", rows["linux_msan"]["flags"])
        self.assertIn("LLVM_USE_SANITIZER=MemoryWithOrigins", rows["linux_msan"]["flags"])
        self.assertEqual(
            rows["linux_msan"]["environment"]["MSAN_OPTIONS"],
            "halt_on_error=1:print_stats=1:fast_unwind_on_fatal=1",
        )
        self.assertIn("linux_lsan", self.matrix.REQUIRED_ROW_IDS)
        self.assertIn("linux_msan", self.matrix.REQUIRED_ROW_IDS)

    def test_macos_address_row_uses_integrated_lsan_for_the_full_ctest_run(self):
        row = next(row for row in self.manifest["rows"] if row["id"] == "macos_asan")
        self.assertEqual(row["environment"].get("ASAN_OPTIONS"), "detect_leaks=1")
        self.assertEqual(
            row["environment"].get("LSAN_OPTIONS"),
            "suppressions={env:MC_ANALYSIS_ROOT}/script/ci/macos_lsan.supp:print_suppressions=1",
        )
        controls_index = next(
            index
            for index, command in enumerate(row["commands"])
            if command[:3] == ["python", "script/ci/verify_macos_lsan.py", "controls"]
        )
        configure_index = row["commands"].index(["cmake", "--preset", "analysis-macos-asan"])
        build_index = row["commands"].index(["cmake", "--build", "--preset", "analysis-macos-asan"])
        self.assertEqual(controls_index, 0)
        self.assertLess(controls_index, configure_index)
        self.assertLess(controls_index, build_index)
        ctest_index = next(index for index, command in enumerate(row["commands"]) if command[0] == "ctest")
        summary_index = next(
            index
            for index, command in enumerate(row["commands"])
            if command[:3] == ["python", "script/ci/verify_macos_lsan.py", "summarize"]
        )
        self.assertLess(build_index, ctest_index)
        self.assertLess(ctest_index, summary_index)
        self.assertFalse(any(command[0] == "leaks" for command in row["commands"]))
        self.assertFalse(any(probe[0] == "leaks" for probe in row["tool_probes"]))
        self.assertFalse(any("leaks" in flag for flag in row["flags"]))

    def test_matrix_row_environment_resolves_checkout_root_placeholders(self):
        row = next(row for row in self.manifest["rows"] if row["id"] == "macos_asan")
        environment = self.matrix.resolve_row_environment(row, Path("/private/tmp/minecraft"), {})
        expected_root = str(Path("/private/tmp/minecraft").resolve())
        self.assertEqual(environment["MC_ANALYSIS_ROOT"], expected_root)
        self.assertEqual(
            environment["LSAN_OPTIONS"],
            f"suppressions={expected_root}/script/ci/macos_lsan.supp:print_suppressions=1",
        )

    def test_linux_presets_are_tests_only_and_use_distinct_sanitizers(self):
        presets = json.loads((REPOSITORY / "CMakePresets.json").read_text(encoding="utf-8"))
        configure = {item["name"]: item for item in presets["configurePresets"]}
        linux_base = configure["analysis-linux-base"]["cacheVariables"]
        self.assertEqual(linux_base["MC_ANALYSIS_HOST"], "ON")
        self.assertEqual(linux_base["MC_BUILD_CLIENT"], "OFF")
        self.assertEqual(configure["analysis-linux-lsan"]["cacheVariables"]["MC_SANITIZER"], "leak")
        self.assertEqual(configure["analysis-linux-msan"]["cacheVariables"]["MC_SANITIZER"], "memory")
        msan_variables = configure["analysis-linux-msan"]["cacheVariables"]
        for flag in (
            "-fsanitize=memory",
            "-fsanitize-memory-track-origins=2",
            "-fno-omit-frame-pointer",
        ):
            self.assertIn(flag, msan_variables["CMAKE_CXX_FLAGS"])
        self.assertIn("-fsanitize=memory", msan_variables["CMAKE_EXE_LINKER_FLAGS"])
        self.assertIn("-fsanitize=memory", msan_variables["CMAKE_SHARED_LINKER_FLAGS"])

    def test_sanitizer_presets_use_optimized_test_configuration(self):
        presets = json.loads((REPOSITORY / "CMakePresets.json").read_text(encoding="utf-8"))
        configure = {item["name"]: item for item in presets["configurePresets"]}
        base_variables = configure["base"]["cacheVariables"]
        release_variables = configure["release"]["cacheVariables"]
        analysis_base = configure["analysis-base"]

        self.assertEqual(analysis_base["inherits"], "release")
        self.assertEqual(release_variables["CMAKE_BUILD_TYPE"], "Release")
        self.assertEqual(base_variables["MC_ENABLE_HIGH_ASSERT"], "OFF")

        sanitizer_flags = {
            row["id"]: row["flags"]
            for row in self.manifest["rows"]
            if row["id"] in {
                "macos_asan",
                "macos_ubsan",
                "macos_tsan",
                "windows_msvc_asan",
                "linux_lsan",
                "linux_msan",
            }
        }
        self.assertEqual(len(sanitizer_flags), 6)
        for row_id, flags in sanitizer_flags.items():
            with self.subTest(row_id=row_id):
                self.assertIn("-DCMAKE_BUILD_TYPE=Release", flags)
                self.assertIn("-DMC_ENABLE_HIGH_ASSERT=OFF", flags)

    def test_msvc_analysis_preset_fails_on_code_analysis_warnings(self):
        presets = json.loads((REPOSITORY / "CMakePresets.json").read_text(encoding="utf-8"))
        configure = {item["name"]: item for item in presets["configurePresets"]}
        variables = configure["analysis-windows-analyze"]["cacheVariables"]
        row = next(row for row in self.manifest["rows"] if row["id"] == "windows_msvc_analyze")
        self.assertEqual(row["flags"], ["/analyze", "/W4", "/WX"])

        for language in ("C", "CXX"):
            with self.subTest(language=language):
                flags = variables[f"CMAKE_{language}_FLAGS"].split()
                self.assertIn("/analyze", flags)
                self.assertIn("/WX", flags)
                self.assertNotIn("/analyze:WX-", flags)

    def test_iwyu_preset_fails_on_include_violations(self):
        presets = json.loads((REPOSITORY / "CMakePresets.json").read_text(encoding="utf-8"))
        configure = {item["name"]: item for item in presets["configurePresets"]}
        command = configure["analysis-iwyu"]["cacheVariables"]["CMAKE_CXX_INCLUDE_WHAT_YOU_USE"]
        row = next(row for row in self.manifest["rows"] if row["id"] == "iwyu")

        self.assertIn("iwyu_gate.py", command)
        self.assertIn("--no_fwd_decls", command)
        self.assertIn(";-Xiwyu;--error;--", command)
        self.assertTrue(any("CMake -Xiwyu --error suffix propagates wrapper failures" in flag for flag in row["flags"]))
        self.assertTrue(any("owned public include additions fail" in flag for flag in row["flags"]))

    def test_cppcheck_manifest_records_classified_gate_policy(self):
        row = next(row for row in self.manifest["rows"] if row["id"] == "cppcheck")

        self.assertTrue(any("CMAKE_CXX_COMPILER_LAUNCHER" in flag for flag in row["flags"]))
        self.assertTrue(any("error, warning, performance, and portability" in flag for flag in row["flags"]))
        self.assertNotIn("--error-exitcode=1", ";".join(row["flags"]))

    def test_ctest_rows_fail_when_no_tests_are_registered(self):
        rows = {row["id"]: row for row in self.manifest["rows"]}
        for row_id in (
            "macos_asan",
            "macos_ubsan",
            "macos_tsan",
            "windows_msvc_asan",
            "windows_msvc_analyze",
        ):
            ctest_commands = [
                command for command in rows[row_id]["commands"] if command[0] == "ctest"
            ]
            self.assertTrue(ctest_commands, row_id)
            with self.subTest(row_id=row_id):
                self.assertTrue(all("--no-tests=error" in command for command in ctest_commands))

    def test_ubsan_row_stops_on_first_runtime_diagnostic(self):
        row = next(row for row in self.manifest["rows"] if row["id"] == "macos_ubsan")
        self.assertEqual(
            row["environment"].get("UBSAN_OPTIONS"),
            "halt_on_error=1:print_stacktrace=1",
        )

    def test_android_hwasan_release_has_an_isolated_application_id(self):
        build_gradle = (REPOSITORY / "android/app/build.gradle").read_text(encoding="utf-8")
        self.assertIn("applicationId 'com.corejust.minecraftclone'", build_gradle)
        self.assertIn("namespace 'com.corejust.minecraftclone'", build_gradle)
        sanitizer_block = build_gradle.split("if (sanitizer != null) {", 1)[1].split(
            "if (vcpkgInstalledDir != null", 1
        )[0]
        self.assertIn("if (sanitizer != 'hwasan')", sanitizer_block)
        self.assertIn("throw new GradleException", sanitizer_block)
        suffix = "android.buildTypes.release.applicationIdSuffix = '.hwasan'"
        self.assertIn(suffix, sanitizer_block)
        self.assertLess(sanitizer_block.index("throw new GradleException"), sanitizer_block.index(suffix))
        self.assertEqual(build_gradle.count("applicationIdSuffix"), 1)

    def test_android_hwasan_gradle_rejects_counterfeit_native_libraries(self):
        groovy_jars = sorted(Path.home().glob(".gradle/wrapper/dists/*/*/*/lib/groovy-[0-9]*.jar"))
        java = shutil.which("java")
        if not java or not groovy_jars:
            self.skipTest("Java and Gradle's bundled Groovy are required")
        build_gradle = (REPOSITORY / "android/app/build.gradle").read_text(encoding="utf-8")
        validation = build_gradle.split("tasks.register('verifyHwasanReleaseApk')", 1)[1].split(
            "ZipFile zip = new ZipFile(apk)", 1
        )[1].split(
            "} finally {", 1
        )[0]
        header = bytearray(64)
        header[:6] = b"\x7fELF\x02\x01"
        header[18:20] = b"\xb7\x00"
        marker = b"libclang_rt.hwasan-aarch64-android.so"
        libraries = {"valid": bytes(header) + marker, "non_elf": b"counterfeit" + marker}
        for name, offset, value in (
            ("magic", 0, 0),
            ("class", 4, 1),
            ("byte_order", 5, 2),
            ("machine", 18, 62),
            ("machine_high", 19, 1),
        ):
            invalid = bytearray(header)
            invalid[offset] = value
            libraries[name] = bytes(invalid) + marker
        libraries["short"] = bytes(header[:20]) + marker
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "validate.groovy"
            script.write_text(
                "import java.util.zip.ZipFile\n"
                "class GradleException extends RuntimeException {\n"
                "    GradleException(String message) { super(message) }\n}\n"
                "File apk = new File(args[0])\nZipFile zip = new ZipFile(apk)\n"
                + validation + "} finally { zip.close() }\n",
                encoding="utf-8",
            )
            for name, library in libraries.items():
                with self.subTest(library=name):
                    apk = root / f"{name}.apk"
                    with zipfile.ZipFile(apk, "w") as archive:
                        archive.writestr(
                            "lib/arm64-v8a/wrap.sh", '#!/system/bin/sh\nLD_HWASAN=1 exec "$@"\n'
                        )
                        archive.writestr("lib/arm64-v8a/libmc_android.so", library)
                    result = subprocess.run(
                        [java, "-cp", str(groovy_jars[0]), "groovy.ui.GroovyMain", str(script), str(apk)],
                        capture_output=True, text=True, timeout=30,
                    )
                    if name == "valid":
                        self.assertEqual(result.returncode, 0, result.stderr)
                    else:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn("not a 64-bit little-endian AArch64 ELF", result.stderr)

    def test_android_hwasan_runtime_builds_release_variant(self):
        row = next(row for row in self.manifest["rows"] if row["id"] == "android_arm64_hwasan")
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", row["flags"])
        self.assertIn("-DMC_ENABLE_HIGH_ASSERT=OFF", row["flags"])
        self.assertIn("build/hwasan/app-release.apk", row["commands"][0])
        self.assertEqual(row["commands"][0][-1], "emulator-5558")
        self.assertEqual(row["tool_probes"], [["adb", "-s", "emulator-5558", "get-state"]])

        workflow = (REPOSITORY / ".github/workflows/ai-checks.yml").read_text(encoding="utf-8")
        build_gradle = (REPOSITORY / "android/app/build.gradle").read_text(encoding="utf-8")
        self.assertIn("resources.srcDir(hwasanWrapAssets)", build_gradle)
        self.assertIn("android.buildTypes.release.debuggable = true", build_gradle)
        self.assertIn("android.packaging.jniLibs.useLegacyPackaging = true", build_gradle)
        self.assertIn("verifyHwasanReleaseApk", build_gradle)
        build_job = workflow.split("  android-hwasan-build:", 1)[1].split(
            "  android-hwasan-runtime:", 1
        )[0]
        self.assertIn(":app:assembleRelease", build_job)
        self.assertIn("outputs/apk/release/app-release.apk", build_job)
        self.assertNotIn(":app:assembleDebug", build_job)
        self.assertIn("android_clang_tidy.json", build_job)
        self.assertLess(build_job.index(":app:assembleRelease"), build_job.index("android_clang_tidy.json"))
        self.assertLess(
            build_job.index("--apk-only"),
            build_job.index("Upload HWASan APK for arm64 runtime"),
        )

        runtime_job = workflow.split("  android-hwasan-runtime:", 1)[1].split(
            "  analysis-matrix:", 1
        )[0]
        java_setup = "actions/setup-java@de7274f081f381c8f8158605e0321c36c376e2e6 # v6.0.1"
        self.assertIn(java_setup, runtime_job)
        self.assertIn("distribution: temurin", runtime_job)
        self.assertIn("java-version: '21'", runtime_job)
        self.assertIn("architecture: aarch64", runtime_job)
        self.assertLess(runtime_job.index(java_setup), runtime_job.index("Select and validate Java 21"))
        self.assertLess(
            runtime_job.index("Select and validate Java 21"),
            runtime_job.index("Create isolated Android emulator homes"),
        )
        self.assertLess(
            runtime_job.index("Download HWASan APK"),
            runtime_job.index("--apk-only"),
        )
        self.assertLess(
            runtime_job.index("--apk-only"),
            runtime_job.index("Start matching ARM64 Android emulator"),
        )

    def test_android_hwasan_runtime_routes_only_to_candidate_specific_mac_runner(self):
        workflow = (REPOSITORY / ".github/workflows/ai-checks.yml").read_text(encoding="utf-8")
        runtime_job = workflow.split("  android-hwasan-runtime:", 1)[1].split(
            "  analysis-matrix:", 1
        )[0]

        self.assertIn(
            "if: github.event_name == 'push' && github.ref == 'refs/heads/ai-main'",
            runtime_job,
        )
        label = '"mc-s7-hwasan-${{ github.sha }}-${{ github.run_id }}"'
        self.assertIn(f"runs-on: {label}", runtime_job)
        self.assertNotIn("runs-on: [self-hosted", runtime_job)
        self.assertNotIn("macOS, ARM64", runtime_job)
        self.assertIn("Start matching ARM64 Android emulator with host acceleration", runtime_job)
        self.assertIn("android_arm64_hwasan", runtime_job)

        workflow_files = list((REPOSITORY / ".github/workflows").glob("*.yml"))
        workflow_files.extend((REPOSITORY / ".github/workflows").glob("*.yaml"))
        label_uses = sum(
            label in candidate.read_text(encoding="utf-8")
            for candidate in workflow_files
        )
        self.assertEqual(1, label_uses, "only the guarded Android HWASan job may request this runner label")

    def test_linux_analysis_workflow_triplets_exist(self):
        workflow = (REPOSITORY / ".github/workflows/ai-checks.yml").read_text(encoding="utf-8")
        linux_job = workflow.split("  linux-analysis:", 1)[1].split(
            "  android-hwasan-build:", 1
        )[0]
        triplets = [
            line.split("triplet:", 1)[1].strip()
            for line in linux_job.splitlines()
            if "triplet:" in line
        ]

        self.assertEqual(triplets, ["x64-linux-lsan", "x64-linux-msan"])
        for triplet in triplets:
            with self.subTest(triplet=triplet):
                self.assertTrue(
                    (REPOSITORY / "script/ci/vcpkg-triplets" / f"{triplet}.cmake").is_file(),
                    f"Linux analysis references missing vcpkg triplet {triplet}",
                )

if __name__ == "__main__":
    unittest.main()
