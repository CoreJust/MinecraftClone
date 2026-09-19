import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path


binary = Path(sys.argv.pop(1)).resolve()


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
    def test_real_networked_client_renders_terrain_texture_and_hud(self):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "playtest.ppm"
            server = subprocess.Popen(
                [str(binary), "--server", "--port", str(port)],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                time.sleep(0.2)
                started = time.monotonic()
                client = subprocess.run(
                    [
                        str(binary),
                        "--player-client-capture",
                        "--address",
                        f"127.0.0.1:{port}",
                        "--image",
                        str(image),
                    ],
                    stdin=subprocess.DEVNULL,
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                self.assertEqual(client.returncode, 0, client.stdout + client.stderr)
                self.assertLess(time.monotonic() - started, 8.0)
                width, height, pixels = read_ppm(image)
                self.assertGreaterEqual(width, 1_000)
                self.assertGreaterEqual(height, 600)
                colors = set()
                terrain_pixels = 0
                white_hud_pixels = 0
                for offset in range(0, len(pixels), 3):
                    red, green, blue = pixels[offset:offset + 3]
                    if red < 150 and green < 150 and blue < 150:
                        terrain_pixels += 1
                        if len(colors) < 512:
                            colors.add((red, green, blue))
                hud_width = width // 3
                hud_height = height // 3
                for y in range(hud_height):
                    for x in range(hud_width):
                        offset = (y * width + x) * 3
                        red, green, blue = pixels[offset:offset + 3]
                        if red > 220 and green > 220 and blue > 220:
                            white_hud_pixels += 1
                terrain_fraction = terrain_pixels / (width * height)
                self.assertGreater(terrain_fraction, 0.05)
                self.assertLess(terrain_fraction, 0.80)
                self.assertGreater(len(colors), 24)
                self.assertGreater(white_hud_pixels, 100)
            finally:
                if server.poll() is None:
                    server.terminate()
                server.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
