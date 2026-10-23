"""Failure diagnostics for the AcceptanceCli runtime CMake script."""

from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "tests/acceptance/runtime_cli_tests.cmake"


class RuntimeCliTests(unittest.TestCase):
    def test_native_diagnostic_provenance_covers_all_source_overrides(self):
        pattern = re.compile(r'"(MC_DIAGNOSTIC_[A-Z_]+)"')
        recognized = set()
        for source in (REPOSITORY / "src").rglob("*"):
            if source.suffix in (".cpp", ".hpp"):
                recognized.update(pattern.findall(source.read_text(encoding="utf-8")))
        benchmark = (REPOSITORY / "src/acceptance/GameBenchmark.cpp").read_text(encoding="utf-8")
        helper = benchmark.split("bool gameBenchmarkDiagnosticOverridesPresent(", 1)[1].split("\n}", 1)[0]
        reported = set(pattern.findall(helper))

        self.assertTrue(recognized)
        self.assertEqual(reported, recognized)

    def test_valid_scenario_failure_reports_exact_child_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output_directory = root / "output"
            launcher = root / ("failing-main.cmd" if os.name == "nt" else "failing-main.sh")
            if os.name == "nt":
                launcher.write_text(
                    "@echo off\r\n"
                    'if "%~1"=="--server" (\r\n'
                    'if "%~2"=="--corelang-backend" if "%~3"=="invalid" '
                    "echo CoreLang backend must be interpreter, jit, or aot 1>&2\r\n"
                    'if "%~2"=="--corelang-backend" if "%~3"=="invalid" exit /b 1\r\n'
                    "echo invalid port 1>&2\r\nexit /b 1\r\n)\r\n"
                    'if "%~1"=="--local-game" (\r\n'
                    'if "%~2"=="--corelang-backend" if "%~3"=="invalid" '
                    "echo CoreLang backend must be interpreter, jit, or aot 1>&2\r\n"
                    'if "%~2"=="--corelang-backend" if "%~3"=="invalid" exit /b 1\r\n'
                    "echo invalid port 1>&2\r\nexit /b 1\r\n)\r\n"
                    'if "%~1"=="--player-client-2d" '
                    '(echo 2D replay seconds must be an integer from 1 to 120 1>&2 & exit /b 1)\r\n'
                    'if "%~1"=="--benchmark-game" (\r\n'
                    'if "%~2"=="--corelang-backend" if "%~3"=="invalid" '
                    "echo CoreLang backend must be interpreter, jit, or aot 1>&2\r\n"
                    'if "%~2"=="--corelang-backend" if "%~3"=="invalid" exit /b 1\r\n'
                    'if "%~3"=="speed-200-full-radius" echo conflicts with render distance 1>&2\r\n'
                    'if "%~3"=="ordinary-turning" if "%~7"=="128" '
                    "echo gameplay proof conflicts with render distance 1>&2\r\n"
                    'if "%~3"=="ordinary-turning" if not "%~7"=="128" '
                    "echo gameplay proof requires its fixed radius and production settings 1>&2\r\n"
                    "exit /b 1\r\n"
                    ")\r\n"
                    "echo child stdout\r\necho child stderr 1>&2\r\nexit /b 37\r\n",
                    encoding="utf-8",
                )
            else:
                launcher.write_text(
                    "#!/bin/sh\n"
                    'if [ "$1" = "--server" ] || [ "$1" = "--local-game" ]; then\n'
                    'if [ "$2" = "--corelang-backend" ] && [ "$3" = "invalid" ]; then\n'
                    "printf '%s\\n' 'CoreLang backend must be interpreter, jit, or aot' >&2\n"
                    "else\nprintf '%s\\n' 'invalid port' >&2\nfi\nexit 1\nfi\n"
                    'if [ "$1" = "--player-client-2d" ]; then\n'
                    "printf '%s\\n' '2D replay seconds must be an integer from 1 to 120' >&2\nexit 1\nfi\n"
                    'if [ "$1" = "--benchmark-game" ]; then\n'
                    'if [ "$2" = "--corelang-backend" ] && [ "$3" = "invalid" ]; then\n'
                    "printf '%s\\n' 'CoreLang backend must be interpreter, jit, or aot' >&2\nexit 1\nfi\n"
                    'if [ "$3" = "speed-200-full-radius" ]; then\n'
                    "printf '%s\\n' 'conflicts with render distance' >&2\n"
                    'elif [ "$3" = "ordinary-turning" ] && [ "$7" = "128" ]; then\n'
                    "printf '%s\\n' 'gameplay proof conflicts with render distance' >&2\n"
                    'elif [ "$3" = "ordinary-turning" ]; then\n'
                    "printf '%s\\n' 'gameplay proof requires its fixed radius and production settings' >&2\n"
                    "fi\n"
                    "exit 1\n"
                    "fi\n"
                    "printf '%s\\n' 'child stdout'\nprintf '%s\\n' 'child stderr' >&2\nexit 37\n",
                    encoding="utf-8",
                )
                launcher.chmod(0o755)

            result = subprocess.run(
                [
                    "cmake",
                    f"-DMC_MAIN={launcher}",
                    f"-DOUTPUT_DIRECTORY={output_directory}",
                    "-P",
                    str(SCRIPT),
                ],
                text=True,
                capture_output=True,
            )

            output = result.stdout + result.stderr
            self.assertNotEqual(result.returncode, 0, output)
            self.assertIn("valid scenario command failed:", output)
            diagnostic_scenario = f"{output_directory}/valid.core"
            diagnostic_evidence = f"{output_directory}/valid.json"
            self.assertRegex(
                output,
                re.compile(
                    rf'command:\s+"{re.escape(str(launcher))}"\s+"--scenario"\s+'
                    rf'"{re.escape(diagnostic_scenario)}"\s+"--evidence"\s+'
                    rf'"{re.escape(diagnostic_evidence)}"'
                ),
            )
            self.assertIn("exit code: 37", output)
            self.assertRegex(output, r"stdout:\s+child stdout")
            self.assertRegex(output, r"stderr:\s+child stderr")


if __name__ == "__main__":
    unittest.main()
