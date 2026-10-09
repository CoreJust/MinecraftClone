import os
import re
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path


binary = Path(sys.argv.pop(1)).resolve()


def free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def run_client(arguments, timeout=1200):
    command = [str(binary), *arguments]
    started = time.monotonic()
    client = subprocess.Popen(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    reported_stdout = 0
    reported_stderr = 0
    while True:
        remaining = timeout - (time.monotonic() - started)
        if remaining <= 0.0:
            client.kill()
            stdout, stderr = client.communicate()
            raise subprocess.TimeoutExpired(command, timeout, output=stdout, stderr=stderr)
        try:
            stdout, stderr = client.communicate(timeout=min(30.0, remaining))
        except subprocess.TimeoutExpired as pending:
            for output, reported in ((pending.output, reported_stdout), (pending.stderr, reported_stderr)):
                if output:
                    new_output = output[reported:]
                    if isinstance(new_output, bytes):
                        new_output = new_output.decode(errors="replace")
                    print(new_output, end="", flush=True)
            if pending.output:
                reported_stdout = len(pending.output)
            if pending.stderr:
                reported_stderr = len(pending.stderr)
            elapsed = time.monotonic() - started
            print(f"capture client still running after {elapsed:.0f}s", flush=True)
        else:
            return subprocess.CompletedProcess(command, client.returncode, stdout, stderr)


def read_ppm(path):
    with path.open("rb") as stream:
        if stream.readline() != b"P6\n":
            raise AssertionError("capture is not a binary PPM")
        width, height = map(int, stream.readline().split())
        if stream.readline() != b"255\n":
            raise AssertionError("capture has an unsupported color range")
        pixels = stream.read()
    if len(pixels) != width * height * 3:
        raise AssertionError("capture pixel payload is truncated")
    return width, height, pixels


class PlayerPlaytestCaptureTest(unittest.TestCase):
    def assert_full_radius_sweep(self, client, expected_tiles=205861):
        output = client.stdout + client.stderr
        started = re.search(
            rf"Capture sweep started: readiness_elapsed=(\d+)s step=1/16 required={expected_tiles}",
            output,
        )
        self.assertIsNotNone(started, output)
        self.assertLessEqual(int(started.group(1)), 900)
        self.assertIn("Capture sweep complete: headings=16", output)

    def assert_labeled_terrain_capture(self, image, require_background_terrain=False):
        width, height, pixels = read_ppm(image)
        if os.environ.get("MC_S7_REQUIRE_2560X1440") == "1":
            self.assertEqual((width, height), (2560, 1440))
        else:
            self.assertGreaterEqual(width, 1_000)
            self.assertGreaterEqual(height, 600)
        colors = set()
        terrain_pixels = 0
        white_hud_pixels = 0
        title_pixels = 0
        beyond_ring_terrain_pixels = 0
        for pixel_index, offset in enumerate(range(0, len(pixels), 3)):
            red, green, blue = pixels[offset:offset + 3]
            x = pixel_index % width
            y = pixel_index // width
            if red < 150 and green < 150 and blue < 150:
                terrain_pixels += 1
                if len(colors) < 512:
                    colors.add((red, green, blue))
                if (
                    require_background_terrain
                    and x >= 3 * width // 4
                    and 78 * height // 100 <= y < 94 * height // 100
                ):
                    beyond_ring_terrain_pixels += 1
            if x < width // 3 and y < height // 3 and red > 220 and green > 220 and blue > 220:
                white_hud_pixels += 1
            if red > 200 and green > 150 and blue < 180:
                if y >= 2 * height // 3 and width // 5 <= x < 4 * width // 5:
                    title_pixels += 1
        terrain_fraction = terrain_pixels / (width * height)
        self.assertGreater(terrain_fraction, 0.05)
        self.assertLess(terrain_fraction, 0.95)
        self.assertGreater(len(colors), 24)
        self.assertGreater(white_hud_pixels, 100)
        self.assertGreater(title_pixels, 100)
        if require_background_terrain:
            background_region_area = (width - 3 * width // 4) * (
                94 * height // 100 - 78 * height // 100
            )
            self.assertGreater(beyond_ring_terrain_pixels, background_region_area // 10)

    def test_real_networked_client_renders_terrain_texture_and_hud(self):
        port = free_udp_port()
        with tempfile.TemporaryDirectory() as directory:
            capture_output = os.environ.get("MC_S7_CENTRAL_CAPTURE_PPM")
            image = (
                Path(capture_output).resolve()
                if capture_output
                else Path(directory) / "playtest.ppm"
            )
            image.parent.mkdir(parents=True, exist_ok=True)
            server = subprocess.Popen(
                [str(binary), "--server", "--port", str(port), "--render-distance", "72"],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                time.sleep(0.2)
                started = time.monotonic()
                client = run_client(
                    [
                        "--player-client-capture",
                        "--address",
                        f"127.0.0.1:{port}",
                        "--preset",
                        "central-spike",
                        "--image",
                        str(image),
                    ],
                )
                self.assertEqual(client.returncode, 0, client.stdout + client.stderr)
                self.assertLess(time.monotonic() - started, 1200.0)
                self.assert_full_radius_sweep(client, expected_tiles=16241)
                self.assert_labeled_terrain_capture(image, require_background_terrain=True)
            finally:
                if server.poll() is None:
                    server.terminate()
                server.wait(timeout=5)

    def test_origin_preset_captures_labeled_first_person_terrain(self):
        port = free_udp_port()
        with tempfile.TemporaryDirectory() as directory:
            capture_output = os.environ.get("MC_S7_ORIGIN_CAPTURE_PPM")
            image = (
                Path(capture_output).resolve()
                if capture_output
                else Path(directory) / "origin.ppm"
            )
            image.parent.mkdir(parents=True, exist_ok=True)
            server = subprocess.Popen(
                [
                    str(binary),
                    "--server",
                    "--port",
                    str(port),
                    "--spawn",
                    "0",
                    "0",
                    "8",
                ],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                time.sleep(0.2)
                client = run_client(
                    [
                        "--player-client-capture",
                        "--address",
                        f"127.0.0.1:{port}",
                        "--preset",
                        "first-person-origin",
                        "--image",
                        str(image),
                    ],
                )
                self.assertEqual(client.returncode, 0, client.stdout + client.stderr)
                self.assert_full_radius_sweep(client)
                self.assert_labeled_terrain_capture(image)
            finally:
                if server.poll() is None:
                    server.terminate()
                server.wait(timeout=5)

    def test_trench_preset_waits_for_full_terrain_before_capture(self):
        port = free_udp_port()
        with tempfile.TemporaryDirectory() as directory:
            capture_output = os.environ.get("MC_S7_TRENCH_CAPTURE_PPM")
            image = (
                Path(capture_output).resolve()
                if capture_output
                else Path(directory) / "trench.ppm"
            )
            image.parent.mkdir(parents=True, exist_ok=True)
            server = subprocess.Popen(
                [str(binary), "--server", "--port", str(port)],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                time.sleep(0.2)
                client = run_client(
                    [
                        "--player-client-capture",
                        "--address",
                        f"127.0.0.1:{port}",
                        "--preset",
                        "trench-first-spike",
                        "--image",
                        str(image),
                    ],
                )
                self.assertEqual(client.returncode, 0, client.stdout + client.stderr)
                self.assert_full_radius_sweep(client)
                self.assert_labeled_terrain_capture(image)
            finally:
                if server.poll() is None:
                    server.terminate()
                server.wait(timeout=5)

    def test_mountain_climb_preset_captures_labeled_climb(self):
        port = free_udp_port()
        with tempfile.TemporaryDirectory() as directory:
            capture_output = os.environ.get("MC_S7_MOUNTAIN_CAPTURE_PPM")
            image = (
                Path(capture_output).resolve()
                if capture_output
                else Path(directory) / "mountain.ppm"
            )
            image.parent.mkdir(parents=True, exist_ok=True)
            server = subprocess.Popen(
                [str(binary), "--server", "--port", str(port)],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                time.sleep(0.2)
                client = run_client(
                    [
                        "--player-client-capture",
                        "--address",
                        f"127.0.0.1:{port}",
                        "--preset",
                        "mountain-climb",
                        "--image",
                        str(image),
                    ],
                )
                self.assertEqual(client.returncode, 0, client.stdout + client.stderr)
                self.assert_full_radius_sweep(client)
                self.assert_labeled_terrain_capture(image)
            finally:
                if server.poll() is None:
                    server.terminate()
                server.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
