import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import call, Mock


PROCESS_CLEANUP_TIMEOUT_SECONDS = 5
MAX_PROCESS_LOG_BYTES = 4096

binary = Path(sys.argv.pop(1)).resolve()


def free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def read_log_tail(log_path):
    try:
        log_size = log_path.stat().st_size
        with log_path.open("rb") as log_file:
            log_file.seek(max(0, log_size - MAX_PROCESS_LOG_BYTES))
            contents = log_file.read(MAX_PROCESS_LOG_BYTES).decode(errors="replace")
    except OSError as error:
        return f"<unavailable: {error}>"

    if log_size > MAX_PROCESS_LOG_BYTES:
        return f"<truncated; showing last {MAX_PROCESS_LOG_BYTES} bytes>\n{contents}"
    return contents or "<empty>"


def terminate_and_reap_processes(named_processes):
    named_processes = list(named_processes)
    failures = []
    for name, process, _ in named_processes:
        if process is None:
            continue
        try:
            if process.poll() is None:
                process.terminate()
        except ProcessLookupError:
            pass

    for name, process, _ in named_processes:
        if process is None:
            continue
        try:
            process.wait(timeout=PROCESS_CLEANUP_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired as error:
            failures.append((name, error))
            try:
                if process.poll() is None:
                    process.kill()
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=PROCESS_CLEANUP_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired as kill_error:
                failures.append((f"{name} after kill", kill_error))

    if failures:
        first_name, first_error = failures[0]
        details = [f"{first_name} did not exit within {PROCESS_CLEANUP_TIMEOUT_SECONDS} seconds"]
        for name, error in failures[1:]:
            details.append(f"{name} cleanup also failed: {error}")
        for name, process, log_path in named_processes:
            if process is None:
                continue
            details.append(f"{name} pid={process.pid} returncode={process.returncode}")
            details.append(f"{name} output:\n{read_log_tail(log_path)}")
        first_error.add_note("\n".join(details))
        raise first_error


class ClientLaunchTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = binary

    def wait_for_text(self, log_path, text, process, timeout=8, diagnostic_path=None):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if text in log_path.read_text(errors="replace"):
                return
            self.assertIsNone(process.poll(), f"server exited before {text!r}")
            time.sleep(0.05)
        diagnostics = f"\nServer output:\n{log_path.read_text(errors='replace')}"
        if diagnostic_path is not None:
            diagnostics += f"\nClient output:\n{diagnostic_path.read_text(errors='replace')}"
        self.fail(f"timed out waiting for {text!r}{diagnostics}")

    def test_two_bots_retry_automatic_tokens_without_stdin(self):
        port = free_udp_port()

        with tempfile.TemporaryDirectory() as directory:
            log_path = Path(directory) / "server.log"
            client_log_paths = [
                Path(directory) / "client-one.log",
                Path(directory) / "client-two.log",
            ]
            with log_path.open("w+") as log:
                server = subprocess.Popen(
                    [str(self.binary), "--server", "--port", str(port)],
                    stdin=subprocess.DEVNULL,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                )
                clients = []
                client_logs = []
                try:
                    address = f"127.0.0.1:{port}"
                    client_logs.append(client_log_paths[0].open("w+"))
                    clients.append(subprocess.Popen(
                        [str(self.binary), "--bot-client", "--address", address],
                        stdin=subprocess.DEVNULL,
                        stdout=client_logs[0],
                        stderr=subprocess.STDOUT,
                    ))
                    self.wait_for_text(
                        log_path,
                        "Player '#' spawned",
                        server,
                        timeout=35,
                        diagnostic_path=client_log_paths[0],
                    )
                    client_logs.append(client_log_paths[1].open("w+"))
                    clients.append(subprocess.Popen(
                        [str(self.binary), "--bot-client", "--address", address],
                        stdin=subprocess.DEVNULL,
                        stdout=client_logs[1],
                        stderr=subprocess.STDOUT,
                    ))
                    self.wait_for_text(
                        log_path,
                        "Player '$' spawned",
                        server,
                        timeout=35,
                        diagnostic_path=client_log_paths[1],
                    )
                    self.assertIsNone(server.poll())
                    self.assertTrue(all(client.poll() is None for client in clients))
                finally:
                    try:
                        terminate_and_reap_processes([
                            (f"client {index + 1}", client, client_log_paths[index])
                            for index, client in enumerate(clients)
                        ] + [("server", server, log_path)])
                    finally:
                        for client_log in client_logs:
                            client_log.close()

    def test_bot_remains_alive_until_delayed_server_starts(self):
        port = free_udp_port()

        with tempfile.TemporaryDirectory() as directory:
            log_path = Path(directory) / "server.log"
            client_log_path = Path(directory) / "client.log"
            address = f"127.0.0.1:{port}"
            with client_log_path.open("w+") as client_log:
                client = subprocess.Popen(
                    [str(self.binary), "--bot-client", "--address", address],
                    stdin=subprocess.DEVNULL,
                    stdout=client_log,
                    stderr=subprocess.STDOUT,
                )
                server = None
                try:
                    time.sleep(1.5)
                    self.assertIsNone(client.poll(), "client exited before delayed server startup")
                    with log_path.open("w+") as log:
                        server = subprocess.Popen(
                            [str(self.binary), "--server", "--port", str(port)],
                            stdin=subprocess.DEVNULL,
                            stdout=log,
                            stderr=subprocess.STDOUT,
                        )
                        self.wait_for_text(
                            log_path,
                            "Player '#' spawned",
                            server,
                            timeout=35,
                            diagnostic_path=client_log_path,
                        )
                        self.assertIsNone(client.poll())
                finally:
                    terminate_and_reap_processes([
                        ("client", client, client_log_path),
                        ("server", server, log_path),
                    ])

    def test_cleanup_kills_timed_out_client_and_reaps_server(self):
        with tempfile.TemporaryDirectory() as directory:
            client_log_path = Path(directory) / "client.log"
            server_log_path = Path(directory) / "server.log"
            client_log_path.write_bytes(
                b"CLIENT_PREFIX\n" + b"x" * MAX_PROCESS_LOG_BYTES + b"\nCLIENT_TAIL\n"
            )
            server_log_path.write_text("SERVER_TAIL\n")

            client = Mock()
            client.args = ["mc_main", "--bot-client"]
            client.pid = 123
            client.returncode = -9
            client.poll.side_effect = [None, None]
            client.wait.side_effect = [
                subprocess.TimeoutExpired(client.args, PROCESS_CLEANUP_TIMEOUT_SECONDS),
                -9,
            ]
            server = Mock()
            server.args = ["mc_main", "--server"]
            server.pid = 456
            server.returncode = -15
            server.poll.return_value = None
            server.wait.return_value = -15

            with self.assertRaises(subprocess.TimeoutExpired) as raised:
                terminate_and_reap_processes([
                    ("client", client, client_log_path),
                    ("server", server, server_log_path),
                ])

            self.assertEqual(raised.exception.timeout, PROCESS_CLEANUP_TIMEOUT_SECONDS)
            client.terminate.assert_called_once_with()
            client.kill.assert_called_once_with()
            self.assertEqual(client.wait.call_args_list, [
                call(timeout=PROCESS_CLEANUP_TIMEOUT_SECONDS),
                call(timeout=PROCESS_CLEANUP_TIMEOUT_SECONDS),
            ])
            server.terminate.assert_called_once_with()
            server.wait.assert_called_once_with(timeout=PROCESS_CLEANUP_TIMEOUT_SECONDS)
            diagnostics = "\n".join(raised.exception.__notes__)
            self.assertIn("CLIENT_TAIL", diagnostics)
            self.assertIn("SERVER_TAIL", diagnostics)
            self.assertNotIn("CLIENT_PREFIX", diagnostics)
            self.assertIn("<truncated; showing last", diagnostics)

    def test_invalid_launch_arguments_fail(self):
        cases = [
            ("--server", "--port", "0"),
            ("--server", "--port", "65536"),
            ("--server", "--port", "12x"),
            ("--server", "--address", "127.0.0.1:20040"),
            ("--bot-client", "trailing"),
        ]
        for arguments in cases:
            with self.subTest(arguments=arguments):
                result = subprocess.run(
                    [str(self.binary), *arguments],
                    stdin=subprocess.DEVNULL,
                    capture_output=True,
                    text=True,
                    timeout=5,
                )
                self.assertNotEqual(result.returncode, 0)

    def test_render_distance_is_validated_by_server_and_game_benchmark(self):
        for value in ("0", "257", "1024", "-1", "128x", "128.5", "4294967296"):
            for mode in ("--server", "--benchmark-game"):
                arguments = [mode, "--render-distance", value]
                if mode == "--benchmark-game":
                    arguments.extend(("--evidence", "unused-radius-evidence.json"))
                with self.subTest(mode=mode, radius=value):
                    result = subprocess.run(
                        [str(self.binary), *arguments], capture_output=True, text=True, timeout=5,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("render distance must be an integer chunk radius from 1 to 256",
                                  result.stderr)
        for value in ("128", "256"):
            with self.subTest(radius=value):
                result = subprocess.run(
                    [str(self.binary), "--benchmark-game", "--render-distance", value,
                     "--workload", "invalid", "--evidence", "unused-radius-evidence.json"],
                    capture_output=True, text=True, timeout=5,
                )
                self.assertIn("invalid game benchmark workload", result.stderr)

    def test_full_radius_warmup_timeout_is_bounded_and_workload_specific(self):
        for value in ("-1", "0", "1801", "1x"):
            with self.subTest(timeout=value):
                result = subprocess.run(
                    [str(self.binary), "--benchmark-game", "--workload", "speed-200-full-radius",
                     "--full-radius-warmup-seconds", value, "--evidence", "unused-full-radius.json"],
                    capture_output=True, text=True, timeout=5,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("full-radius warmup timeout must be", result.stderr)

        result = subprocess.run(
            [str(self.binary), "--benchmark-game", "--workload", "speed-200",
             "--full-radius-warmup-seconds", "1", "--evidence", "unused-full-radius.json"],
            capture_output=True, text=True, timeout=5,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires --workload speed-200-full-radius", result.stderr)

        result = subprocess.run(
            [str(self.binary), "--benchmark-game", "--diagnostic-full-radius-capture",
             "--workload", "speed-200", "--evidence", "unused-full-radius.json"],
            capture_output=True, text=True, timeout=5,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--diagnostic-full-radius-capture requires --workload speed-200-full-radius", result.stderr)


        with tempfile.TemporaryDirectory() as directory:
            evidence_path = Path(directory) / "full-radius.json"
            result = subprocess.run(
                [str(self.binary), "--benchmark-game", "--workload", "speed-200-full-radius",
                 "--full-radius-warmup-seconds", "1", "--render-distance", "255",
                 "--evidence", str(evidence_path)],
                capture_output=True, text=True, timeout=5,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(evidence_path.exists())
            self.assertIn("requires render distance 256", evidence_path.read_text())
    def test_diagnostic_indexed_stone_quad_mode_is_bounded_and_workload_specific(self):
        for value in ("0", "1", "2"):
            with self.subTest(mode=value):
                result = subprocess.run(
                    [str(self.binary), "--benchmark-game", "--diagnostic-indexed-stone-quads", value,
                     "--workload", "invalid", "--evidence", "unused-renderer-mode.json"],
                    capture_output=True, text=True, timeout=5,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("invalid game benchmark workload", result.stderr)

        for value in ("-1", "3", "1x"):
            with self.subTest(mode=value):
                result = subprocess.run(
                    [str(self.binary), "--benchmark-game", "--diagnostic-indexed-stone-quads", value,
                     "--workload", "speed-200-full-radius", "--evidence", "unused-renderer-mode.json"],
                    capture_output=True, text=True, timeout=5,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("diagnostic indexed stone quad mode must be 0, 1, or 2", result.stderr)

        result = subprocess.run(
            [str(self.binary), "--benchmark-game",
             "--diagnostic-indexed-stone-quads", "1",
             "--workload", "speed-200-full-radius",
             "--full-radius-warmup-seconds", "180",
             "--present-immediate",
             "--diagnostic-full-radius-capture",
             "--diagnostic-presentation-stages",
             "--render-distance", "invalid",
             "--evidence", "unused-renderer-mode.json"],
            capture_output=True, text=True, timeout=5,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("render distance must be an integer chunk radius from 1 to 256", result.stderr)

        result = subprocess.run(
            [str(self.binary), "--benchmark-game", "--diagnostic-indexed-stone-quads", "1",
             "--workload", "speed-200", "--evidence", "unused-renderer-mode.json"],
            capture_output=True, text=True, timeout=5,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires --workload speed-200-full-radius", result.stderr)


    def test_diagnostic_presentation_stages_are_full_radius_specific(self):
        result = subprocess.run(
            [str(self.binary), "--benchmark-game", "--diagnostic-presentation-stages",
             "--workload", "invalid", "--evidence", "unused-presentation-stages.json"],
            capture_output=True, text=True, timeout=5,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid game benchmark workload", result.stderr)

        result = subprocess.run(
            [str(self.binary), "--benchmark-game", "--diagnostic-presentation-stages",
             "--workload", "speed-200", "--evidence", "unused-presentation-stages.json"],
            capture_output=True, text=True, timeout=5,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--diagnostic-presentation-stages requires --workload speed-200-full-radius",
            result.stderr)


    def test_normal_server_accepts_selected_render_distance(self):
        port = free_udp_port()
        with tempfile.TemporaryDirectory() as directory:
            log_path = Path(directory) / "server.log"
            with log_path.open("w+") as log:
                server = subprocess.Popen(
                    [str(self.binary), "--server", "--render-distance", "128", "--port", str(port)],
                    stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                )
                try:
                    self.wait_for_text(log_path, "Created host", server)
                    self.assertIsNone(server.poll())
                finally:
                    terminate_and_reap_processes([("server", server, log_path)])


if __name__ == "__main__":
    unittest.main()
