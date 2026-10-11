"""Contract tests for the cppcheck compiler launcher."""
import importlib.util
import contextlib
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("cppcheck_gate", Path(__file__).parents[1] / "ci/cppcheck_gate.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


class CppcheckGateTests(unittest.TestCase):
    def run_launcher(self, report, analyzer_code=0, compiler_code=0):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            (root / "src").mkdir()
            source = root / "src/a.cpp"
            source.write_text("int main() {}")
            responses = [(subprocess.CompletedProcess([], 0, b"Cppcheck 2", b""), 1), (subprocess.CompletedProcess([], analyzer_code, b"", report), 2), (subprocess.CompletedProcess([], compiler_code, b"", b""), 3)]
            with patch.object(gate, "ROOT", root), patch.object(gate, "project_analysis_input_sha256", return_value="a" * 64), patch.object(gate, "analyzer_arguments", return_value=["cppcheck", str(source)]), patch.object(gate, "run", side_effect=responses) as runner:
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    # Buffered binary output is empty in these fake tools.
                    result = gate.main(["cppcheck", "--", "clang++", "-c", str(source)])
                return result, runner.call_count

    def report(self, identifier="uninitvar", severity="warning", file="src/a.cpp"):
        return f'<results version="2"><errors><error id="{identifier}" severity="{severity}" msg="message"><location file="{file}" line="4"/></error></errors></results>'.encode()

    def test_owned_correctness_diagnostics_block(self):
        for severity in ("error", "warning", "performance", "portability"):
            with self.subTest(severity=severity):
                self.assertTrue(gate.parse_xml(self.report(severity=severity), Path("/repo"), Path("/repo"))[0]["blocking"])

    def test_explicit_hints_and_external_primary_are_advisory(self):
        for identifier, file in (("uninitMemberVarNoCtor", "src/a.cpp"), ("passedByValue", "src/a.cpp"), ("uninitvar", "/dependency/a.hpp")):
            with self.subTest(identifier=identifier, file=file):
                self.assertFalse(gate.parse_xml(self.report(identifier=identifier, file=file), Path("/repo"), Path("/repo"))[0]["blocking"])

    def test_configuration_errors_block_even_information(self):
        for identifier in ("syntaxError", "missingInclude", "missingIncludeSystem", "unknownMacro", "internalError"):
            with self.subTest(identifier=identifier):
                self.assertTrue(gate.parse_xml(self.report(identifier, "information", "/dep/x"), Path("/repo"), Path("/repo"))[0]["blocking"])

    def test_malformed_and_unknown_reports_fail(self):
        for report in (b"", b"<results/>", self.report(severity="surprise")):
            with self.subTest(report=report):
                with self.assertRaises((ValueError, gate.ET.ParseError)):
                    gate.parse_xml(report, Path("/repo"), Path("/repo"))

    def test_only_known_standard_missing_headers_are_advisory(self):
        for header, blocking in (("vector", False), ("core/OS.hpp", True), ("gtest/gtest.h", True)):
            report = self.report("missingIncludeSystem", "information").replace(b'msg="message"', f'msg="Include file: &lt;{header}&gt; not found"'.encode())
            with self.subTest(header=header):
                self.assertEqual(gate.parse_xml(report, Path("/repo"), Path("/repo"))[0]["blocking"], blocking)

    def test_compiler_failure_is_propagated(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            (root / "src").mkdir()
            source = root / "src/a.cpp"
            source.write_text("int main() {}")
            responses = [(subprocess.CompletedProcess([], 0, b"Cppcheck 2", b""), 1), (subprocess.CompletedProcess([], 0, b"", b"<results><errors/></results>"), 2), (subprocess.CompletedProcess([], 7, b"", b""), 3)]
            with patch.object(gate, "ROOT", root), patch.object(gate, "project_analysis_input_sha256", return_value="a" * 64), patch.object(gate, "analyzer_arguments", return_value=["cppcheck", str(source)]), patch.object(gate, "run", side_effect=responses):
                self.assertEqual(gate.main(["cppcheck", "--", "clang++", "-c", str(source)]), 7)
            import json
            receipt = json.loads(next((root / "build/analysis-cppcheck/gate").glob("*.json")).read_text())
            self.assertEqual(receipt["status"], "fail")
            self.assertEqual(receipt["failure_stage"], "compiler")

    def test_configuration_probe_passes_through_return_code(self):
        source = gate.ROOT / "build/debug/CMakeFiles/CMakeScratch/TryCompile/a.cpp"
        with patch.object(gate.subprocess, "call", return_value=9) as compiler:
            self.assertEqual(gate.main(["cppcheck", "--", "clang++", "-c", str(source)]), 9)
            compiler.assert_called_once_with(["clang++", "-c", str(source)])

    def test_tool_crash_never_compiles(self):
        self.assertEqual(self.run_launcher(b"<results><errors/></results>", analyzer_code=3), (2, 2))

    def test_malformed_xml_never_compiles(self):
        self.assertEqual(self.run_launcher(b"not xml"), (2, 2))

    def test_blocking_warning_never_compiles(self):
        # Absolute primary location must belong to the temporary fixture root.
        with patch.object(gate, "parse_xml", return_value=[{"id": "uninitvar", "severity": "warning", "file": "src/a.cpp", "line": 1, "message": "uninitialized", "primary": True, "blocking": True}]):
            self.assertEqual(self.run_launcher(b"<results><errors/></results>"), (2, 2))

    def test_actual_compiler_context_is_preserved(self):
        compiler = ["clang++", "-std=c++23", "-I", "/repo/src/include", "-isystem", "/dep/include", "-D", "USER=1", "-U", "OLD", "-target", "arm64-apple-macos", "-c", "/repo/src/a.cpp"]
        macro = subprocess.CompletedProcess([], 0, b"#define __APPLE__ 1\n#define __MACH__ 1\n", b"")
        apple = subprocess.CompletedProcess([], 0, b"#define __APPLE__ 1\n#define __MACH__ 1\n#define TARGET_OS_MAC 1\n", b"")
        with patch.object(gate.subprocess, "run", side_effect=[macro, apple]) as runner:
            args = gate.analyzer_arguments(["cppcheck", "--xml"], compiler, Path("/repo/src/a.cpp"), Path("/repo"))
        for expected in ("--std=c++23", "-I/repo/src/include", "-I/dep/include", "-DUSER=1", "-UOLD", "-D__APPLE__=1", "-D__MACH__=1", "-DTARGET_OS_MAC=1"):
            self.assertIn(expected, args)
        query = runner.call_args_list[0].args[0]
        self.assertEqual(query[query.index("-D") + 1], "USER=1")
        self.assertEqual(query[query.index("-U") + 1], "OLD")

    def test_builtin_features_are_queried_not_assumed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "feature.hpp").write_text("#if __has_feature(cxx_exceptions)\n#endif\n#if __has_cpp_attribute(likely) >= 201903\n#endif")
            dump = subprocess.CompletedProcess([], 0, b"#define __clang__ 1\n", b"")
            feature = subprocess.CompletedProcess([], 0, b"MC_CPPCHECK_QUERY KEY__has_feature cxx_exceptions 1\nMC_CPPCHECK_QUERY KEY__has_cpp_attribute likely 201903L\n", b"")
            with patch.object(gate.subprocess, "run", side_effect=[dump, feature]) as runner:
                args = gate.analyzer_arguments(["cppcheck"], ["clang++", "-I" + str(root)], Path("/repo/src/a.cpp"), Path("/repo"))
            self.assertIn("-D__has_feature(x)=MC_CPPCHECK_HAS_FEATURE_##x", args)
            self.assertIn("-DMC_CPPCHECK_HAS_FEATURE_cxx_exceptions=1", args)
            self.assertIn("-DMC_CPPCHECK_HAS_CPP_ATTRIBUTE_likely=201903L", args)
            self.assertIn(b"__has_feature(cxx_exceptions)", runner.call_args_list[1].kwargs["input"])

    def test_sdk_header_advisory_requires_verified_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "sys").mkdir()
            (root / "sys/stat.h").write_text("/* SDK fixture */")
            report = self.report("missingIncludeSystem", "information").replace(b'msg="message"', b'msg="Include file: &lt;sys/stat.h&gt; not found"')
            with patch.object(gate, "PLATFORM_INCLUDE_ROOTS", [root]):
                self.assertFalse(gate.parse_xml(report, Path("/repo"), Path("/repo"))[0]["blocking"])
            with patch.object(gate, "PLATFORM_INCLUDE_ROOTS", []):
                self.assertTrue(gate.parse_xml(report, Path("/repo"), Path("/repo"))[0]["blocking"])

    def test_quoted_sdk_header_advisory_requires_verified_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "TargetConditionals.h").write_text("/* SDK fixture */")
            for header, blocking in (("TargetConditionals.h", False), ("missing-project.hpp", True)):
                report = self.report("missingInclude", "information").replace(b'msg="message"', f'msg="Include file: &quot;{header}&quot; not found"'.encode())
                with self.subTest(header=header), patch.object(gate, "PLATFORM_INCLUDE_ROOTS", [root]):
                    self.assertEqual(gate.parse_xml(report, Path("/repo"), Path("/repo"))[0]["blocking"], blocking)


if __name__ == "__main__":
    unittest.main()
