"""Regression coverage for the bounded targeted TSan client diagnostic."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "script/ci/diagnostics/targeted_tsan_client.py"
SPEC = importlib.util.spec_from_file_location("targeted_tsan_client", SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("could not load targeted TSan client diagnostic")
targeted_tsan_client = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = targeted_tsan_client
SPEC.loader.exec_module(targeted_tsan_client)


class TargetedTsanClientTests(unittest.TestCase):
    def test_original_failure_is_preserved_and_child_evidence_is_bounded(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary_root = Path(temporary_directory)
            fixture = temporary_root / "client_launch_test.py"
            binary = temporary_root / "mc_main"
            binary.write_text("diagnostic fixture binary placeholder\n", encoding="utf-8")
            evidence_dir = temporary_root / "evidence"
            fixture.write_text(
                textwrap.dedent(
                    """
                    import subprocess
                    import sys
                    import tempfile
                    import time
                    import unittest
                    from pathlib import Path

                    binary = Path(sys.argv.pop(1)).resolve()

                    class ClientLaunchTest(unittest.TestCase):
                        def test_bot_remains_alive_until_delayed_server_starts(self):
                            with tempfile.TemporaryDirectory() as directory:
                                root = Path(directory)
                                client_log = (root / "client.log").open("w+")
                                server_log = (root / "server.log").open("w+")
                                child = (
                                    "import sys,time; "
                                    "sys.stdout.write(sys.argv[-2] + chr(10) + 'x'*8192 + "
                                    "sys.argv[-2] + '_TAIL' + chr(10)); "
                                    "sys.stdout.flush(); time.sleep(30)"
                                )
                                client = subprocess.Popen(
                                    [sys.executable, "-u", "-c", child, "CLIENT", "--bot-client"],
                                    stdin=subprocess.DEVNULL,
                                    stdout=client_log,
                                    stderr=subprocess.STDOUT,
                                )
                                server = subprocess.Popen(
                                    [sys.executable, "-u", "-c", child, "SERVER", "--server"],
                                    stdin=subprocess.DEVNULL,
                                    stdout=server_log,
                                    stderr=subprocess.STDOUT,
                                )
                                try:
                                    time.sleep(0.1)
                                    self.fail("original delayed-server assertion failed")
                                finally:
                                    for process in (client, server):
                                        if process.poll() is None:
                                            process.terminate()
                                    for process in (client, server):
                                        process.wait(timeout=5)
                                    client_log.close()
                                    server_log.close()
                    """
                ),
                encoding="utf-8",
            )

            real_popen = subprocess.Popen
            child_processes = []

            def record_process(*args, **kwargs):
                process = real_popen(*args, **kwargs)
                child_processes.append(process)
                return process

            previous_argv = sys.argv.copy()
            subprocess.Popen = record_process
            try:
                result = targeted_tsan_client.run_client_launch_case(
                    fixture,
                    binary,
                    evidence_dir,
                )
            finally:
                subprocess.Popen = real_popen

            self.assertEqual(result, 1)
            self.assertEqual(sys.argv, previous_argv)
            self.assertTrue(child_processes)
            self.assertTrue(all(process.poll() is not None for process in child_processes))

            observation = evidence_dir / "client-before-sigterm.txt"
            self.assertTrue(
                observation.is_file(),
                f"evidence={list(evidence_dir.iterdir())}, "
                f"children={[process.args for process in child_processes]}",
            )
            contents = observation.read_text(encoding="utf-8")
            self.assertIn("process-state:", contents)
            self.assertIn("client-log-path=", contents)
            self.assertIn("server-log-path=", contents)
            self.assertIn("CLIENT_TAIL", contents)
            self.assertIn("SERVER_TAIL", contents)
            self.assertIn("truncated; showing last 4096 bytes", contents)
            self.assertLessEqual(observation.stat().st_size, 2 * 4096 + 4096)
            for line in contents.splitlines():
                if line.startswith(("client-log-path=", "server-log-path=")):
                    self.assertFalse(Path(line.split("=", 1)[1]).exists())

    def test_cleanup_timeout_writes_only_to_explicit_evidence_directory(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            fixture_logs = root / "fixture-temporary"
            fixture_logs.mkdir()
            log_path = fixture_logs / "client.log"
            log_path.write_text("bounded tail\n", encoding="utf-8")
            evidence_dir = root / "evidence"
            evidence_dir.mkdir()

            class TimedOutProcess:
                pid = 2**30
                args = ["mc_main", "--bot-client"]
                returncode = None

                def poll(self):
                    return self.returncode

                def terminate(self):
                    pass

                def wait(self, timeout=None):
                    if self.returncode is None:
                        raise subprocess.TimeoutExpired(self.args, timeout)
                    return self.returncode

                def kill(self):
                    self.returncode = -9

            process = TimedOutProcess()
            record = targeted_tsan_client.OwnedProcess("client", process, log_path)
            fixture_logs_path = fixture_logs
            fixture_logs_path.rename(root / "fixture-temporary-removed")

            self.assertTrue(
                targeted_tsan_client._cleanup_owned_processes([record], evidence_dir)
            )
            self.assertEqual(process.returncode, -9)
            self.assertTrue((evidence_dir / "client-cleanup-wait-timeout.txt").is_file())
            self.assertFalse(fixture_logs_path.exists())
            self.assertFalse((REPOSITORY / "client-cleanup-wait-timeout.txt").exists())


if __name__ == "__main__":
    unittest.main()
