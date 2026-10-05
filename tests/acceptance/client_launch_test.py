import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path


binary = Path(sys.argv.pop(1)).resolve()


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
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]

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
                    for client in clients:
                        if client.poll() is None:
                            client.terminate()
                    for client in clients:
                        client.wait(timeout=5)
                    for client_log in client_logs:
                        client_log.close()
                    if server.poll() is None:
                        server.terminate()
                    server.wait(timeout=5)

    def test_bot_remains_alive_until_delayed_server_starts(self):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]

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
                    if client.poll() is None:
                        client.terminate()
                    client.wait(timeout=5)
                    if server is not None and server.poll() is None:
                        server.terminate()
                    if server is not None:
                        server.wait(timeout=5)

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

    def test_normal_server_accepts_selected_render_distance(self):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
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
                    if server.poll() is None:
                        server.terminate()
                    server.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
