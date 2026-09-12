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
    def test_valid_scenario_failure_reports_exact_child_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output_directory = root / "output"
            launcher = root / ("failing-main.cmd" if os.name == "nt" else "failing-main.sh")
            if os.name == "nt":
                launcher.write_text(
                    "@echo off\r\necho child stdout\r\necho child stderr 1>&2\r\nexit /b 37\r\n",
                    encoding="utf-8",
                )
            else:
                launcher.write_text(
                    "#!/bin/sh\nprintf '%s\\n' 'child stdout'\nprintf '%s\\n' 'child stderr' >&2\nexit 37\n",
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
            self.assertRegex(
                output,
                re.compile(
                    rf'command:\s+"{re.escape(str(launcher))}"\s+"--scenario"\s+'
                    rf'"{re.escape(str(output_directory / "valid.mcscenario"))}"\s+"--evidence"\s+'
                    rf'"{re.escape(str(output_directory / "valid.json"))}"'
                ),
            )
            self.assertIn("exit code: 37", output)
            self.assertRegex(output, r"stdout:\s+child stdout")
            self.assertRegex(output, r"stderr:\s+child stderr")


if __name__ == "__main__":
    unittest.main()
