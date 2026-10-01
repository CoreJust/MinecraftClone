from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "script/ci/verify_android_hwasan.py"


def load_module():
    spec = importlib.util.spec_from_file_location("verify_android_hwasan", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class VerifyAndroidHwasanTests(unittest.TestCase):
    def setUp(self):
        self.hwasan = load_module()

    def apk(self, root: Path, *, wrap: bytes | None = None, library: bytes | None = None) -> Path:
        apk = root / "game.apk"
        with zipfile.ZipFile(apk, "w") as archive:
            archive.writestr(self.hwasan.WRAP_PATH, wrap or self.hwasan.EXPECTED_WRAP)
            archive.writestr(
                self.hwasan.NATIVE_LIBRARY_PATH,
                library or b"native" + self.hwasan.HWASAN_RUNTIME,
            )
        return apk

    def test_apk_requires_the_hwasan_wrap_script_and_runtime_linkage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.hwasan.verify_apk(self.apk(root))
            with self.assertRaisesRegex(self.hwasan.HwasanError, "invalid"):
                self.hwasan.verify_apk(self.apk(root, wrap=b"#!/system/bin/sh\nexec \"$@\"\n"))
            with self.assertRaisesRegex(self.hwasan.HwasanError, "not linked"):
                self.hwasan.verify_apk(self.apk(root, library=b"native"))

    def test_runtime_requires_a_live_process_and_clean_logcat(self):
        with tempfile.TemporaryDirectory() as directory:
            apk = self.apk(Path(directory))
            with mock.patch.object(
                self.hwasan,
                "run_adb",
                side_effect=["", "Success", "Starting"] + ["using ns libmc_android.so"] * 6,
            ), mock.patch.object(self.hwasan, "running_process_id", return_value="123"), mock.patch.object(
                self.hwasan.time,
                "monotonic",
                side_effect=[0, 0, 1, 2, 3, 4, 5],
            ), mock.patch.object(self.hwasan.time, "sleep"):
                receipt = self.hwasan.verify_runtime("emulator-5554", apk, 10)
            self.assertEqual(receipt["pid"], "123")
            self.assertEqual(receipt["healthy_seconds"], 5)

            with mock.patch.object(
                self.hwasan,
                "run_adb",
                side_effect=["", "Success", "Starting", "FATAL EXCEPTION using ns libmc_android.so"],
            ), mock.patch.object(self.hwasan, "running_process_id", return_value="123"), mock.patch.object(
                self.hwasan.time, "monotonic", side_effect=[0, 0]
            ), mock.patch.object(self.hwasan.time, "sleep"):
                with self.assertRaisesRegex(self.hwasan.HwasanError, "FATAL EXCEPTION"):
                    self.hwasan.verify_runtime("emulator-5554", apk, 1)

    def test_runtime_rejects_a_process_that_exits_after_loading_the_library(self):
        with tempfile.TemporaryDirectory() as directory:
            apk = self.apk(Path(directory))
            with mock.patch.object(
                self.hwasan,
                "run_adb",
                side_effect=["", "Success", "Starting", "using ns libmc_android.so"],
            ), mock.patch.object(
                self.hwasan,
                "running_process_id",
                side_effect=["123", ""],
            ), mock.patch.object(
                self.hwasan.time,
                "monotonic",
                side_effect=[0, 0, 1],
            ), mock.patch.object(self.hwasan.time, "sleep"):
                with self.assertRaisesRegex(self.hwasan.HwasanError, "exited"):
                    self.hwasan.verify_runtime("emulator-5554", apk, 10)

    def test_pid_poll_treats_android_not_yet_running_as_a_retryable_state(self):
        with mock.patch.object(
            self.hwasan.subprocess,
            "run",
            return_value=self.hwasan.subprocess.CompletedProcess([], 1, "", ""),
        ):
            self.assertEqual(self.hwasan.running_process_id("emulator-5554"), "")


if __name__ == "__main__":
    unittest.main()
