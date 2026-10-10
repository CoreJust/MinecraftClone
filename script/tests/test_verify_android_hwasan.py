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

    def aarch64_library(self, *, runtime: bool = True, machine: bytes = b"\xb7\x00") -> bytes:
        header = bytearray(64)
        header[:7] = b"\x7fELF\x02\x01\x01"
        header[18:20] = machine
        return bytes(header) + (self.hwasan.HWASAN_RUNTIME if runtime else b"")

    def apk(self, root: Path, *, wrap: bytes | None = None, library: bytes | None = None) -> Path:
        apk = root / "game.apk"
        with zipfile.ZipFile(apk, "w") as archive:
            archive.writestr(self.hwasan.WRAP_PATH, wrap or self.hwasan.EXPECTED_WRAP)
            archive.writestr(
                self.hwasan.NATIVE_LIBRARY_PATH,
                library or self.aarch64_library(),
            )
        return apk

    def process_maps(self, *, native_library: bool = True, hwasan_runtime: bool = True) -> str:
        mappings = []
        if native_library:
            mappings.append("00010000-00020000 r-xp 00000000 00:00 0 libmc_android.so")
        if hwasan_runtime:
            mappings.append(
                "00030000-00040000 r-xp 00000000 00:00 0 "
                "libclang_rt.hwasan-aarch64-android.so"
            )
        return "\n".join(mappings)

    def test_apk_requires_the_hwasan_wrap_script_and_runtime_linkage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.hwasan.verify_apk(self.apk(root))
            with self.assertRaisesRegex(self.hwasan.HwasanError, "invalid"):
                self.hwasan.verify_apk(self.apk(root, wrap=b"#!/system/bin/sh\nexec \"$@\"\n"))
            with self.assertRaisesRegex(self.hwasan.HwasanError, "not linked"):
                self.hwasan.verify_apk(self.apk(root, library=self.aarch64_library(runtime=False)))
            with self.assertRaisesRegex(self.hwasan.HwasanError, "AArch64 ELF"):
                self.hwasan.verify_apk(
                    self.apk(root, library=self.aarch64_library(machine=b"\x3e\x00"))
                )

    def test_apk_only_cli_validates_without_a_serial_or_adb(self):
        with tempfile.TemporaryDirectory() as directory:
            apk = self.apk(Path(directory))
            with mock.patch.object(self.hwasan.subprocess, "run") as run_adb:
                self.assertEqual(self.hwasan.main(["--apk", str(apk), "--apk-only"]), 0)
            run_adb.assert_not_called()

    def test_apk_only_cli_fails_closed_without_adb_for_invalid_apk(self):
        with tempfile.TemporaryDirectory() as directory:
            apk = self.apk(Path(directory), wrap=b"invalid")
            with mock.patch.object(self.hwasan.subprocess, "run") as run_adb:
                self.assertEqual(self.hwasan.main(["--apk", str(apk), "--apk-only"]), 1)
            run_adb.assert_not_called()

    def test_runtime_rejects_invalid_apk_before_any_device_command(self):
        with tempfile.TemporaryDirectory() as directory:
            apk = self.apk(Path(directory), wrap=b"invalid")
            with mock.patch.object(self.hwasan, "run_adb") as run_adb:
                with self.assertRaisesRegex(self.hwasan.HwasanError, "invalid"):
                    self.hwasan.verify_runtime("emulator-5554", apk, 10)
            run_adb.assert_not_called()

    def test_runtime_requires_both_libraries_and_a_live_process_for_five_seconds(self):
        with tempfile.TemporaryDirectory() as directory:
            apk = self.apk(Path(directory))
            with mock.patch.object(
                self.hwasan,
                "run_adb",
                side_effect=["Success", "Starting"] + [self.process_maps()] * 6,
            ) as run_adb, mock.patch.object(self.hwasan, "running_process_id", return_value="123"), mock.patch.object(
                self.hwasan.time,
                "monotonic",
                side_effect=[0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5],
            ), mock.patch.object(self.hwasan.time, "sleep"):
                receipt = self.hwasan.verify_runtime("emulator-5554", apk, 10)
            self.assertEqual(receipt["pid"], "123")
            self.assertEqual(receipt["healthy_seconds"], 5)
            self.assertEqual(
                receipt["mapped_libraries"],
                ["libmc_android.so", "libclang_rt.hwasan-aarch64-android.so"],
            )
            self.assertIn(
                mock.call("emulator-5554", "shell", "am", "start", "-n", "com.corejust.minecraftclone.hwasan/android.app.NativeActivity"),
                run_adb.call_args_list,
            )
            self.assertIn(
                mock.call(
                    "emulator-5554",
                    "shell",
                    "run-as",
                    "com.corejust.minecraftclone.hwasan",
                    "cat",
                    "/proc/123/maps",
                ),
                run_adb.call_args_list,
            )
            self.assertFalse(any(call.args[1:2] == ("logcat",) for call in run_adb.call_args_list))

    def test_runtime_measures_survival_from_the_first_completed_maps_observation(self):
        with tempfile.TemporaryDirectory() as directory:
            apk = self.apk(Path(directory))
            with mock.patch.object(
                self.hwasan,
                "run_adb",
                side_effect=["Success", "Starting"] + [self.process_maps()] * 7,
            ) as run_adb, mock.patch.object(self.hwasan, "running_process_id", return_value="123"), mock.patch.object(
                self.hwasan.time,
                "monotonic",
                side_effect=[0, 0, 10, 10.5, 10.5, 11.5, 11.5, 12.5, 12.5, 13.5, 13.5, 14.5, 14.5, 15.5, 15.5],
            ), mock.patch.object(self.hwasan.time, "sleep"):
                receipt = self.hwasan.verify_runtime("emulator-5554", apk, 60)
            self.assertEqual(receipt["healthy_seconds"], 5)
            maps_reads = [
                call
                for call in run_adb.call_args_list
                if call.args[1:4] == ("shell", "run-as", "com.corejust.minecraftclone.hwasan")
            ]
            self.assertEqual(len(maps_reads), 7)

    def test_runtime_rejects_a_maps_observation_completed_after_the_deadline(self):
        with tempfile.TemporaryDirectory() as directory:
            apk = self.apk(Path(directory))
            with mock.patch.object(
                self.hwasan,
                "run_adb",
                side_effect=["Success", "Starting", self.process_maps(), self.process_maps()],
            ), mock.patch.object(self.hwasan, "running_process_id", return_value="123"), mock.patch.object(
                self.hwasan.time,
                "monotonic",
                side_effect=[0, 0, 0, 0.5, 6],
            ), mock.patch.object(self.hwasan.time, "sleep"):
                with self.assertRaisesRegex(self.hwasan.HwasanError, "deadline"):
                    self.hwasan.verify_runtime("emulator-5554", apk, 1)

    def test_runtime_rejects_a_pid_change_during_the_final_maps_read(self):
        with tempfile.TemporaryDirectory() as directory:
            apk = self.apk(Path(directory))
            state = {"maps_reads": 0}

            def run_adb(*_arguments, **_kwargs):
                if state.get("installed") is None:
                    state["installed"] = True
                    return "Success"
                if state.get("launched") is None:
                    state["launched"] = True
                    return "Starting"
                state["maps_reads"] += 1
                if state["maps_reads"] == 2:
                    state["pid_after_read"] = "456"
                return self.process_maps()

            def running_process_id(_serial):
                return state.get("pid_after_read", "123")

            with mock.patch.object(self.hwasan, "run_adb", side_effect=run_adb), mock.patch.object(
                self.hwasan,
                "running_process_id",
                side_effect=running_process_id,
            ), mock.patch.object(
                self.hwasan.time,
                "monotonic",
                side_effect=[0, 0, 0, 5, 5],
            ), mock.patch.object(self.hwasan.time, "sleep"):
                with self.assertRaisesRegex(
                    self.hwasan.HwasanError,
                    "restarted during runtime verification",
                ):
                    self.hwasan.verify_runtime("emulator-5554", apk, 10)

    def test_runtime_requires_the_game_library_mapping(self):
        with tempfile.TemporaryDirectory() as directory:
            apk = self.apk(Path(directory))
            with mock.patch.object(
                self.hwasan,
                "run_adb",
                side_effect=["Success", "Starting", self.process_maps(native_library=False)],
            ), mock.patch.object(self.hwasan, "running_process_id", return_value="123"), mock.patch.object(
                self.hwasan.time, "monotonic", side_effect=[0, 0, 0, 1]
            ), mock.patch.object(self.hwasan.time, "sleep"):
                with self.assertRaisesRegex(self.hwasan.HwasanError, "did not map libmc_android.so"):
                    self.hwasan.verify_runtime("emulator-5554", apk, 1)

    def test_runtime_requires_the_hwasan_runtime_mapping(self):
        with tempfile.TemporaryDirectory() as directory:
            apk = self.apk(Path(directory))
            with mock.patch.object(
                self.hwasan,
                "run_adb",
                side_effect=["Success", "Starting", self.process_maps(hwasan_runtime=False)],
            ), mock.patch.object(self.hwasan, "running_process_id", return_value="123"), mock.patch.object(
                self.hwasan.time, "monotonic", side_effect=[0, 0, 0, 1]
            ), mock.patch.object(self.hwasan.time, "sleep"):
                with self.assertRaisesRegex(
                    self.hwasan.HwasanError,
                    "did not map libclang_rt.hwasan-aarch64-android.so",
                ):
                    self.hwasan.verify_runtime("emulator-5554", apk, 1)

    def test_runtime_restarts_health_interval_if_a_required_mapping_disappears(self):
        with tempfile.TemporaryDirectory() as directory:
            apk = self.apk(Path(directory))
            mappings = [
                self.process_maps(),
                self.process_maps(hwasan_runtime=False),
                *[self.process_maps()] * 6,
            ]
            with mock.patch.object(
                self.hwasan,
                "run_adb",
                side_effect=["Success", "Starting"] + mappings,
            ), mock.patch.object(self.hwasan, "running_process_id", return_value="123"), mock.patch.object(
                self.hwasan.time,
                "monotonic",
                side_effect=[0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7],
            ), mock.patch.object(self.hwasan.time, "sleep"):
                receipt = self.hwasan.verify_runtime("emulator-5554", apk, 10)
            self.assertEqual(receipt["healthy_seconds"], 5)

    def test_runtime_rejects_a_process_that_exits_after_loading_the_library(self):
        with tempfile.TemporaryDirectory() as directory:
            apk = self.apk(Path(directory))
            with mock.patch.object(
                self.hwasan,
                "run_adb",
                side_effect=["Success", "Starting", self.process_maps()],
            ), mock.patch.object(
                self.hwasan,
                "running_process_id",
                side_effect=["123", ""],
            ), mock.patch.object(
                self.hwasan.time,
                "monotonic",
                side_effect=[0, 0, 0, 1],
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
