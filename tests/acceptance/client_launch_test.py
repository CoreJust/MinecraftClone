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

    def wait_for_text(self, log_path, text, process, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if text in log_path.read_text(errors="replace"):
                return
            self.assertIsNone(process.poll(), f"server exited before {text!r}")
            time.sleep(0.05)
        self.fail(f"timed out waiting for {text!r}")

    def test_two_bots_retry_automatic_tokens_without_stdin(self):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]

        with tempfile.TemporaryDirectory() as directory:
            log_path = Path(directory) / "server.log"
            with log_path.open("w+") as log:
                server = subprocess.Popen(
                    [str(self.binary), "--server", "--port", str(port)],
                    stdin=subprocess.DEVNULL,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                )
                clients = []
                try:
                    self.wait_for_text(log_path, "Created host", server)
                    time.sleep(0.25)
                    address = f"127.0.0.1:{port}"
                    clients.append(subprocess.Popen(
                        [str(self.binary), "--bot-client", "--address", address],
                        stdin=subprocess.DEVNULL,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    ))
                    self.wait_for_text(log_path, "Player '#' spawned", server)
                    clients.append(subprocess.Popen(
                        [str(self.binary), "--bot-client", "--address", address],
                        stdin=subprocess.DEVNULL,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    ))
                    self.wait_for_text(log_path, "Player '$' spawned", server)
                    self.assertIsNone(server.poll())
                    self.assertTrue(all(client.poll() is None for client in clients))
                finally:
                    for client in clients:
                        if client.poll() is None:
                            client.terminate()
                    for client in clients:
                        client.wait(timeout=5)
                    if server.poll() is None:
                        server.terminate()
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


if __name__ == "__main__":
    unittest.main()
