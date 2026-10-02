from __future__ import annotations

import sys
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


class AndroidPlaytestTest(unittest.TestCase):
    def setUp(self) -> None:
        self.playtest = android_playtest

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
        ), mock.patch.object(self.playtest, "time") as time_module:
            time_module.monotonic.side_effect = [0, 0]
            time_module.sleep.return_value = None
            receipt = self.playtest.launch_or_reuse("emulator-5556", timeout_seconds=5)

        self.assertEqual(receipt["action"], "launched")
        self.assertEqual(receipt["pid"], "789")
        launch_calls = [call for call in run_adb.call_args_list if call.args[1:4] == ("shell", "am", "start")]
        self.assertEqual(len(launch_calls), 1)

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
