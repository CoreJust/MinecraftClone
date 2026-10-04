from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path
import unittest
from unittest import mock


SCRIPT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPT_DIR))
import android_pair_playtest


class FakeServer:
    def __init__(self, poll_results: list[int | None] | None = None) -> None:
        self.poll_results = iter(poll_results or [None])
        self.terminated = False
        self.killed = False

    def poll(self) -> int | None:
        return next(self.poll_results, 0 if self.terminated else None)

    def wait(self, timeout: float | None = None) -> int:
        if self.terminated:
            return 0
        raise KeyboardInterrupt

    def terminate(self) -> None:
        self.terminated = True

    def kill(self) -> None:
        self.killed = True
        self.terminated = True


class AndroidPairPlaytestTest(unittest.TestCase):
    def setUp(self) -> None:
        self.pair = android_pair_playtest

    @staticmethod
    def release_server_path() -> Path:
        return Path("/project") / "build" / "release" / "mc_main"

    def test_missing_release_server_fails_with_build_command(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            missing_server = Path(temporary_directory) / "release" / "mc_main"
            with self.assertRaisesRegex(self.pair.AndroidPairPlaytestError, "cmake --preset release"):
                self.pair._require_release_server(missing_server)

    def test_udp_probe_reports_whether_host_port_is_free(self) -> None:
        with mock.patch.object(self.pair, "_udp_port_available", return_value=False):
            with self.assertRaisesRegex(self.pair.AndroidPairPlaytestError, "already in use"):
                self.pair._require_port_free()

    def test_server_readiness_waits_for_udp_listener(self) -> None:
        server = FakeServer()
        with mock.patch.object(self.pair, "_udp_port_available", side_effect=[True, False]), mock.patch.object(
            self.pair.time, "sleep"
        ):
            self.pair._wait_for_server(server, timeout_seconds=2)

    def test_server_exit_before_udp_listener_is_reported(self) -> None:
        server = FakeServer(poll_results=[17])
        with mock.patch.object(self.pair, "_udp_port_available", return_value=True):
            with self.assertRaisesRegex(self.pair.AndroidPairPlaytestError, "exit 17"):
                self.pair._wait_for_server(server, timeout_seconds=2)

    def test_readiness_timeout_is_bounded(self) -> None:
        server = FakeServer()
        with mock.patch.object(self.pair, "_udp_port_available", return_value=True), mock.patch.object(
            self.pair.time, "monotonic", side_effect=[0.0, 0.0, 2.0]
        ), mock.patch.object(self.pair.time, "sleep"):
            with self.assertRaisesRegex(self.pair.AndroidPairPlaytestError, "did not bind UDP 20040"):
                self.pair._wait_for_server(server, timeout_seconds=1)

    def test_runner_starts_exactly_one_release_server_and_client(self) -> None:
        server = FakeServer()
        receipt = {"action": "reused", "serial": "emulator-5556"}
        server_path = self.release_server_path()
        with mock.patch.object(self.pair, "_require_release_server", return_value=server_path), mock.patch.object(
            self.pair, "_require_port_free"
        ), mock.patch.object(self.pair, "_wait_for_server") as wait_ready, mock.patch.object(
            self.pair.subprocess, "Popen", return_value=server
        ) as popen, mock.patch.object(self.pair.android_playtest, "launch_or_reuse", return_value=receipt) as launch_client:
            with self.assertRaises(KeyboardInterrupt):
                self.pair.run_playtest("emulator-5556", server_path)

        popen.assert_called_once_with(
            [
                str(server_path),
                "--server",
                "--port",
                "20040",
                "--render-distance",
                "72",
            ]
        )
        wait_ready.assert_called_once_with(server, 20)
        launch_client.assert_called_once_with("emulator-5556", timeout_seconds=60)
        self.assertTrue(server.terminated)
        self.assertFalse(server.killed)

    def test_port_conflict_does_not_start_server_or_client(self) -> None:
        server_path = self.release_server_path()
        with mock.patch.object(self.pair, "_require_release_server", return_value=server_path), mock.patch.object(
            self.pair, "_require_port_free", side_effect=self.pair.AndroidPairPlaytestError("UDP 20040 is already in use")
        ), mock.patch.object(self.pair.subprocess, "Popen") as popen, mock.patch.object(
            self.pair.android_playtest, "launch_or_reuse"
        ) as launch_client:
            with self.assertRaisesRegex(self.pair.AndroidPairPlaytestError, "already in use"):
                self.pair.run_playtest("emulator-5556", server_path)

        popen.assert_not_called()
        launch_client.assert_not_called()

    def test_client_launch_failure_stops_only_the_started_server(self) -> None:
        server = FakeServer()
        server_path = self.release_server_path()
        with mock.patch.object(self.pair, "_require_release_server", return_value=server_path), mock.patch.object(
            self.pair, "_require_port_free"
        ), mock.patch.object(self.pair, "_wait_for_server"), mock.patch.object(
            self.pair.subprocess, "Popen", return_value=server
        ), mock.patch.object(
            self.pair.android_playtest,
            "launch_or_reuse",
            side_effect=self.pair.android_playtest.AndroidPlaytestError("wrong AVD"),
        ):
            with self.assertRaisesRegex(self.pair.android_playtest.AndroidPlaytestError, "wrong AVD"):
                self.pair.run_playtest("emulator-5556", server_path)

        self.assertTrue(server.terminated)
        self.assertFalse(server.killed)

    def test_unresponsive_server_is_killed_after_terminate_timeout(self) -> None:
        class UnresponsiveServer(FakeServer):
            def wait(self, timeout: float | None = None) -> int:
                if not self.killed:
                    raise subprocess.TimeoutExpired("mc_main", timeout)
                return 0

        server = UnresponsiveServer()
        self.pair._stop_owned_server(server)
        self.assertTrue(server.terminated)
        self.assertTrue(server.killed)


if __name__ == "__main__":
    unittest.main()
