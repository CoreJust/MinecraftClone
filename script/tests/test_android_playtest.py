from __future__ import annotations

import multiprocessing
import tempfile
import sys
import threading
import time
from pathlib import Path
import unittest
from unittest import mock


SCRIPT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPT_DIR))
import android_playtest


LAUNCHER_COMPONENT = "com.google.android.apps.nexuslauncher/com.google.android.apps.nexuslauncher.NexusLauncherActivity"


def activity_dump(top_component: str, state_component: str, state: str) -> str:
    top_record = f"ActivityRecord{{1 u0 {top_component} t1}}"
    state_record = f"ActivityRecord{{1 u0 {state_component} t1}}"
    return "\n".join(
        (
            f"topResumedActivity={top_record}",
            f"* Hist #1: {state_record}",
            f"  mActivityComponent={state_component}",
            f"  state={state} delayedResume=false",
            f"ResumedActivity: {top_record}",
        )
    )


def ready_outputs(playtest: android_playtest) -> list[str]:
    return [
        playtest.S7_AVD,
        "1",
        "package:/system/framework/framework-res.apk",
        "activity\npackage\nwindow\n",
    ]


def _run_simultaneous_guard(barrier, initial_scan_barrier, launch_count, process_running, result_queue, lock_path: str) -> None:
    playtest = android_playtest
    playtest._launch_lock_path = lambda: Path(lock_path)

    def run_adb(serial: str | None, *arguments: str, timeout: int = 30) -> str:
        if arguments == ("shell", "getprop", "ro.boot.qemu.avd_name"):
            return playtest.S7_AVD
        if arguments == ("shell", "getprop", "sys.boot_completed"):
            return "1"
        if arguments == ("shell", "pm", "path", "android"):
            return "package:/system/framework/framework-res.apk"
        if arguments == ("shell", "dumpsys", "-l"):
            return "activity\npackage\nwindow\n"
        if arguments == ("shell", "dumpsys", "activity", "activities"):
            return activity_dump(playtest.COMPONENT, playtest.COMPONENT, "RESUMED")
        if arguments[:3] == ("shell", "am", "start"):
            with launch_count.get_lock():
                launch_count.value += 1
            with process_running.get_lock():
                process_running.value = 1
            return "Status: ok"
        raise AssertionError(f"unexpected fake ADB command: {arguments}")

    def running_process_id(serial: str) -> str:
        with process_running.get_lock():
            running = bool(process_running.value)
        if not running:
            try:
                initial_scan_barrier.wait(timeout=1)
            except threading.BrokenBarrierError:
                pass
        return "123" if running else ""

    playtest.connected_devices = lambda: ["emulator-5556"]
    playtest.run_adb = run_adb
    playtest.running_process_id = running_process_id

    try:
        barrier.wait(timeout=10)
        result = playtest.launch_or_reuse("emulator-5556", timeout_seconds=5)
        result_queue.put({"action": result["action"]})
    except Exception as error:
        result_queue.put({"error": repr(error)})


def _hold_launch_lock(lock_path: str, acquired) -> None:
    with android_playtest._launch_lock(5, Path(lock_path)):
        acquired.set()
        while True:
            time.sleep(1)


class AndroidPlaytestTest(unittest.TestCase):
    def setUp(self) -> None:
        self.playtest = android_playtest

    @staticmethod
    def adb_name() -> str:
        return "adb.exe" if sys.platform == "win32" else "adb"

    def test_documented_adb_path_precedes_legacy_override_and_sdk_roots(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            configured_adb = root / "configured-adb"
            legacy_adb = root / "legacy-adb"
            sdk_adb = root / "sdk" / "platform-tools" / self.adb_name()
            configured_adb.touch()
            legacy_adb.touch()
            sdk_adb.parent.mkdir(parents=True)
            sdk_adb.touch()
            environment = {
                "ADB_PATH": str(configured_adb),
                "ADB": str(legacy_adb),
                "ANDROID_SDK_ROOT": str(sdk_adb.parents[1]),
                "ANDROID_HOME": str(root / "other-sdk"),
                "PATH": "",
            }
            with mock.patch.dict("os.environ", environment, clear=True):
                self.assertEqual(self.playtest._adb_executable(), str(configured_adb.resolve()))

    def test_android_sdk_root_finds_adb_without_a_path_entry(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            sdk_adb = Path(temp_dir) / "platform-tools" / self.adb_name()
            sdk_adb.parent.mkdir()
            sdk_adb.touch()
            environment = {"ANDROID_SDK_ROOT": str(sdk_adb.parent.parent), "PATH": ""}
            with mock.patch.dict("os.environ", environment, clear=True):
                self.assertEqual(self.playtest._adb_executable(), str(sdk_adb.resolve()))

    def test_android_home_is_used_when_sdk_root_has_no_adb(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            sdk_adb = root / "android-home" / "platform-tools" / self.adb_name()
            sdk_adb.parent.mkdir(parents=True)
            sdk_adb.touch()
            environment = {
                "ANDROID_SDK_ROOT": str(root / "incomplete-sdk"),
                "ANDROID_HOME": str(sdk_adb.parents[1]),
                "PATH": "",
            }
            with mock.patch.dict("os.environ", environment, clear=True):
                self.assertEqual(self.playtest._adb_executable(), str(sdk_adb.resolve()))

    def test_path_lookup_is_the_fallback_after_sdk_discovery(self) -> None:
        environment = {"ANDROID_SDK_ROOT": "", "ANDROID_HOME": "", "PATH": ""}
        expected_adb = f"/host-tools/{self.adb_name()}"
        with mock.patch.dict("os.environ", environment, clear=True), mock.patch(
            "shutil.which", return_value=expected_adb
        ) as which:
            self.assertEqual(self.playtest._adb_executable(), expected_adb)

        which.assert_called_once_with(self.adb_name())

    def test_missing_explicit_adb_path_fails_with_setup_guidance(self) -> None:
        environment = {"ADB_PATH": "/missing/android/adb", "PATH": ""}
        with mock.patch.dict("os.environ", environment, clear=True), mock.patch(
            "shutil.which", return_value="/other/adb"
        ) as which:
            with self.assertRaisesRegex(self.playtest.AndroidPlaytestError, "ADB_PATH"):
                self.playtest._adb_executable()

        which.assert_not_called()

    def test_adb_subprocesses_share_the_resolved_executable(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            configured_adb = Path(temp_dir) / "adb"
            configured_adb.touch()
            with mock.patch.dict("os.environ", {"ADB_PATH": str(configured_adb)}, clear=True), mock.patch.object(
                self.playtest.subprocess,
                "run",
                return_value=mock.Mock(returncode=0, stdout="123\n", stderr=""),
            ) as run:
                self.playtest.run_adb(None, "devices", "-l")
                self.playtest.running_process_id("emulator-5556")

        self.assertEqual(run.call_args_list[0].args[0][0], str(configured_adb.resolve()))
        self.assertEqual(run.call_args_list[1].args[0][0], str(configured_adb.resolve()))

    def test_wrong_avd_is_rejected_before_inspecting_or_launching_game(self) -> None:
        with mock.patch.object(self.playtest, "connected_devices", return_value=["emulator-5554"]), mock.patch.object(
            self.playtest, "run_adb", return_value="Medium_Phone_API_35"
        ) as run_adb, mock.patch.object(self.playtest, "running_process_id") as running_process_id:
            with self.assertRaisesRegex(self.playtest.AndroidPlaytestError, "S7 acceptance requires"):
                self.playtest.launch_or_reuse("emulator-5554")

        run_adb.assert_called_once_with("emulator-5554", "shell", "getprop", "ro.boot.qemu.avd_name")
        running_process_id.assert_not_called()

    def test_client_on_another_device_prevents_a_second_launch(self) -> None:
        with mock.patch.object(
            self.playtest, "connected_devices", return_value=["emulator-5554", "emulator-5556"]
        ), mock.patch.object(self.playtest, "run_adb", side_effect=ready_outputs(self.playtest)) as run_adb, mock.patch.object(
            self.playtest,
            "running_process_id",
            side_effect=lambda serial: "123" if serial == "emulator-5554" else "",
        ):
            with self.assertRaisesRegex(self.playtest.AndroidPlaytestError, "already runs on emulator-5554"):
                self.playtest.launch_or_reuse("emulator-5556")

        self.assertEqual(run_adb.call_count, 4)

    def test_multiple_existing_clients_fail_closed(self) -> None:
        with mock.patch.object(
            self.playtest, "connected_devices", return_value=["emulator-5554", "emulator-5556"]
        ), mock.patch.object(self.playtest, "run_adb", side_effect=ready_outputs(self.playtest)), mock.patch.object(
            self.playtest, "running_process_id", side_effect=["123", "456"]
        ):
            with self.assertRaisesRegex(self.playtest.AndroidPlaytestError, "multiple game clients"):
                self.playtest.launch_or_reuse("emulator-5556")

    def test_top_resumed_client_on_selected_avd_is_reused_without_launch(self) -> None:
        dump = activity_dump(self.playtest.COMPONENT, self.playtest.COMPONENT, "RESUMED")
        with mock.patch.object(self.playtest, "connected_devices", return_value=["emulator-5556"]), mock.patch.object(
            self.playtest, "run_adb", side_effect=[*ready_outputs(self.playtest), dump]
        ) as run_adb, mock.patch.object(self.playtest, "running_process_id", return_value="123"):
            receipt = self.playtest.launch_or_reuse("emulator-5556")

        self.assertEqual(receipt, {"action": "reused", "avd": self.playtest.S7_AVD, "pid": "123", "serial": "emulator-5556"})
        self.assertEqual(run_adb.call_count, 5)
        self.assertNotIn("am", [argument for call in run_adb.call_args_list for argument in call.args])

    def test_top_resumed_detection_requires_the_game_activity_state(self) -> None:
        dump = "\n".join(
            (
                activity_dump(self.playtest.COMPONENT, self.playtest.COMPONENT, "STOPPED"),
                "* Hist #2: ActivityRecord{2 u0 " + LAUNCHER_COMPONENT + " t2}",
                "  mActivityComponent=" + LAUNCHER_COMPONENT,
                "  state=RESUMED delayedResume=false",
            )
        )

        self.assertFalse(self.playtest.is_game_top_resumed(dump))

    def test_background_game_task_is_brought_forward_without_creating_another_activity(self) -> None:
        background_dump = activity_dump(LAUNCHER_COMPONENT, self.playtest.COMPONENT, "STOPPED")
        resumed_dump = activity_dump(self.playtest.COMPONENT, self.playtest.COMPONENT, "RESUMED")
        command_outputs = iter([*ready_outputs(self.playtest), background_dump, "Status: ok", resumed_dump])
        with mock.patch.object(self.playtest, "connected_devices", return_value=["emulator-5556"]), mock.patch.object(
            self.playtest, "run_adb", side_effect=lambda *args, **kwargs: next(command_outputs)
        ) as run_adb, mock.patch.object(self.playtest, "running_process_id", side_effect=["123", "123"]):
            receipt = self.playtest.launch_or_reuse("emulator-5556", timeout_seconds=5)

        self.assertEqual(receipt["action"], "foregrounded")
        launch_calls = [call for call in run_adb.call_args_list if call.args[1:4] == ("shell", "am", "start")]
        self.assertEqual(len(launch_calls), 1)
        self.assertIn("--activity-reorder-to-front", launch_calls[0].args)

    def test_orphaned_client_process_gets_one_fresh_activity_start(self) -> None:
        launcher_dump = activity_dump(LAUNCHER_COMPONENT, LAUNCHER_COMPONENT, "RESUMED")
        resumed_dump = activity_dump(self.playtest.COMPONENT, self.playtest.COMPONENT, "RESUMED")
        command_outputs = iter([*ready_outputs(self.playtest), launcher_dump, "Status: ok", resumed_dump])
        with mock.patch.object(self.playtest, "connected_devices", return_value=["emulator-5556"]), mock.patch.object(
            self.playtest, "run_adb", side_effect=lambda *args, **kwargs: next(command_outputs)
        ) as run_adb, mock.patch.object(self.playtest, "running_process_id", side_effect=["123", "123"]):
            receipt = self.playtest.launch_or_reuse("emulator-5556", timeout_seconds=5)

        self.assertEqual(receipt["action"], "launched")
        launch_calls = [call for call in run_adb.call_args_list if call.args[1:4] == ("shell", "am", "start")]
        self.assertEqual(len(launch_calls), 1)
        self.assertNotIn("--activity-reorder-to-front", launch_calls[0].args)

    def test_clean_target_is_launched_once_and_requires_top_resumed_state(self) -> None:
        services = "activity\npackage\nwindow\n"
        resumed_dump = activity_dump(self.playtest.COMPONENT, self.playtest.COMPONENT, "RESUMED")
        command_outputs = iter(
            [
                self.playtest.S7_AVD,
                "1",
                "package:/system/framework/framework-res.apk",
                services,
                "Status: ok",
                resumed_dump,
            ]
        )
        with mock.patch.object(self.playtest, "connected_devices", return_value=["emulator-5556"]), mock.patch.object(
            self.playtest, "run_adb", side_effect=lambda *args, **kwargs: next(command_outputs)
        ) as run_adb, mock.patch.object(
            self.playtest, "running_process_id", side_effect=["", "789"]
        ):
            receipt = self.playtest.launch_or_reuse("emulator-5556", timeout_seconds=5)

        self.assertEqual(receipt["action"], "launched")
        self.assertEqual(receipt["pid"], "789")
        launch_calls = [call for call in run_adb.call_args_list if call.args[1:4] == ("shell", "am", "start")]
        self.assertEqual(len(launch_calls), 1)

    def test_concurrent_processes_start_once_and_second_reuses_activity(self) -> None:
        context = multiprocessing.get_context("spawn")
        with tempfile.TemporaryDirectory() as temp_dir:
            lock_path = str(Path(temp_dir) / "android-playtest.lock")
            barrier = context.Barrier(2)
            initial_scan_barrier = context.Barrier(2)
            launch_count = context.Value("i", 0)
            process_running = context.Value("i", 0)
            result_queue = context.Queue()
            processes = [
                context.Process(
                    target=_run_simultaneous_guard,
                    args=(barrier, initial_scan_barrier, launch_count, process_running, result_queue, lock_path),
                )
                for _ in range(2)
            ]
            try:
                for process in processes:
                    process.start()
                for process in processes:
                    process.join(timeout=15)
                self.assertTrue(all(not process.is_alive() for process in processes))
                self.assertEqual([process.exitcode for process in processes], [0, 0])
                results = [result_queue.get(timeout=3) for _ in processes]
            finally:
                for process in processes:
                    if process.is_alive():
                        process.kill()
                        process.join(timeout=5)

        self.assertEqual(launch_count.value, 1)
        self.assertCountEqual([result.get("action") for result in results], ["launched", "reused"])
        self.assertFalse(any("error" in result for result in results), results)

    def test_hard_process_exit_releases_the_launch_lock(self) -> None:
        context = multiprocessing.get_context("spawn")
        with tempfile.TemporaryDirectory() as temp_dir:
            lock_path = str(Path(temp_dir) / "android-playtest.lock")
            acquired = context.Event()
            process = context.Process(target=_hold_launch_lock, args=(lock_path, acquired))
            process.start()
            try:
                self.assertTrue(acquired.wait(timeout=10))
                process.kill()
                process.join(timeout=5)
                self.assertFalse(process.is_alive())
                with self.playtest._launch_lock(1, Path(lock_path)):
                    pass
            finally:
                if process.is_alive():
                    process.kill()
                    process.join(timeout=5)

    def test_framework_not_ready_prevents_stale_process_recovery(self) -> None:
        with mock.patch.object(self.playtest, "connected_devices", return_value=["emulator-5556"]), mock.patch.object(
            self.playtest, "running_process_id", return_value="123"
        ) as running_process_id, mock.patch.object(
            self.playtest, "run_adb", side_effect=[self.playtest.S7_AVD, "0"]
        ) as run_adb:
            with self.assertRaisesRegex(self.playtest.AndroidPlaytestError, "has not completed boot"):
                self.playtest.launch_or_reuse("emulator-5556")

        running_process_id.assert_not_called()
        self.assertEqual(run_adb.call_count, 2)
        self.assertFalse(any("am" in call.args and "start" in call.args for call in run_adb.call_args_list))

    def test_missing_framework_package_blocks_stale_process_recovery(self) -> None:
        with mock.patch.object(self.playtest, "connected_devices", return_value=["emulator-5556"]), mock.patch.object(
            self.playtest, "running_process_id", return_value="123"
        ) as running_process_id, mock.patch.object(
            self.playtest,
            "run_adb",
            side_effect=[self.playtest.S7_AVD, "1", ""],
        ) as run_adb:
            with self.assertRaisesRegex(self.playtest.AndroidPlaytestError, "framework package is unavailable"):
                self.playtest.launch_or_reuse("emulator-5556")

        running_process_id.assert_not_called()
        self.assertEqual(run_adb.call_count, 3)

    def test_missing_activity_services_block_stale_process_recovery(self) -> None:
        with mock.patch.object(self.playtest, "connected_devices", return_value=["emulator-5556"]), mock.patch.object(
            self.playtest, "running_process_id", return_value="123"
        ) as running_process_id, mock.patch.object(
            self.playtest,
            "run_adb",
            side_effect=[
                self.playtest.S7_AVD,
                "1",
                "package:/system/framework/framework-res.apk",
                "window\n",
            ],
        ) as run_adb:
            with self.assertRaisesRegex(self.playtest.AndroidPlaytestError, "services are unavailable"):
                self.playtest.launch_or_reuse("emulator-5556")

        running_process_id.assert_not_called()
        self.assertEqual(run_adb.call_count, 4)


if __name__ == "__main__":
    unittest.main()
