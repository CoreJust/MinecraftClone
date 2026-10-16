import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "ai_windows_job.py"


@unittest.skipUnless(os.name == "nt", "Windows Job Object contract")
class WindowsJobTests(unittest.TestCase):
    def test_preserves_command_output_and_exit_code(self):
        result = subprocess.run(
            [sys.executable, str(SCRIPT), sys.executable, "-c",
             "import sys; print('command output'); sys.exit(7)"],
            capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(result.stdout.strip(), "command output")

    def test_killing_wrapper_terminates_descendants_and_releases_pipe(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "survived"
            ready = Path(directory) / "ready"
            child = (
                "import pathlib,time; print('child ready', flush=True); "
                f"pathlib.Path({str(ready)!r}).write_text('ready'); time.sleep(3); "
                f"pathlib.Path({str(marker)!r}).write_text('alive')"
            )
            command = (
                "import subprocess,sys,time; "
                f"subprocess.Popen([sys.executable, '-c', {child!r}]); time.sleep(20)"
            )
            process = subprocess.Popen(
                [sys.executable, str(SCRIPT), sys.executable, "-c", command],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            )
            try:
                deadline = time.monotonic() + 10
                while not ready.is_file():
                    if process.poll() is not None:
                        output, _ = process.communicate(timeout=5)
                        self.fail(f"wrapper exited before child readiness: {process.returncode}: {output}")
                    if time.monotonic() >= deadline:
                        self.fail("child did not report readiness within 10 seconds")
                    time.sleep(0.02)
                self.assertEqual(ready.read_text(), "ready")
            finally:
                if process.poll() is None:
                    process.kill()
                output, _ = process.communicate(timeout=5)
            self.assertIn("child ready", output)
            time.sleep(3)
            self.assertFalse(marker.exists())


if __name__ == "__main__":
    unittest.main()
