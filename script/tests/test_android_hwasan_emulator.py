from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "script/ci/android_hwasan_emulator.py"


def load_module():
    spec = importlib.util.spec_from_file_location("android_hwasan_emulator", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class AndroidHwasanEmulatorTests(unittest.TestCase):
    def setUp(self):
        self.lifecycle = load_module()
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.runner_temp = self.root / "runner-temp"
        self.runner_temp.mkdir()
        self.github_env = self.root / "github-env"
        self.github_env.touch()
        self.run_id = "123456"
        self.attempt = "2"

    def make_java(self, java_home: Path) -> Path:
        java = java_home / "bin" / "java"
        java.parent.mkdir(parents=True)
        java.touch()
        java.chmod(0o755)
        return java

    def prepare(self) -> Path:
        return self.lifecycle.prepare(self.runner_temp, self.run_id, self.attempt, self.github_env)

    @staticmethod
    def process_command(emulator: Path, avd_name: str, extra: str = "") -> str:
        return (
            f"{emulator.as_posix()} -avd {avd_name} "
            f"-port 5558 -accel on{extra}"
        )

    def setup_started_emulator(
        self,
        *,
        host_platform: str = "Darwin",
        accel_output: str = "accel: 0\nHypervisor.Framework OS X Version 26.6\n",
    ) -> tuple[Path, Path, mock.Mock, list]:
        root = self.prepare()
        sdk_root = root / "android-sdk"
        emulator = sdk_root / "emulator" / "emulator"
        emulator.parent.mkdir(parents=True, exist_ok=True)
        emulator.touch()
        (sdk_root / "cmdline-tools" / "latest" / "bin").mkdir(parents=True, exist_ok=True)
        (sdk_root / "platform-tools").mkdir(exist_ok=True)
        process = mock.Mock()
        process.pid = 4321
        process.poll.return_value = None
        commands = []
        command = self.process_command(
            emulator,
            f"mc-hwasan-{self.run_id}-{self.attempt}",
            " -no-window -no-audio -no-boot-anim -no-snapshot",
        )

        def run(command_args, **kwargs):
            commands.append((command_args, kwargs))
            if command_args[-2:] == ["emulator-5558", "get-state"]:
                return self.lifecycle.subprocess.CompletedProcess(
                    command_args, 1, "", "error: device 'emulator-5558' not found\n"
                )
            if command_args[-1:] == ["-accel-check"]:
                return self.lifecycle.subprocess.CompletedProcess(
                    command_args, 0, accel_output, ""
                )
            return self.lifecycle.subprocess.CompletedProcess(command_args, 0, "", "")

        with mock.patch.object(self.lifecycle.subprocess, "run", side_effect=run), mock.patch.object(
            self.lifecycle.subprocess, "Popen", return_value=process
        ) as popen, mock.patch.object(self.lifecycle, "_port_available", return_value=True), mock.patch.object(
            self.lifecycle, "_process_snapshot", return_value=("Thu Oct  7 10:00:00 2026", command)
        ), mock.patch.object(self.lifecycle.platform, "system", return_value=host_platform):
            self.lifecycle.start(self.runner_temp, self.run_id, self.attempt, sdk_root)
        return root, sdk_root, popen, commands

    def test_java_selection_uses_first_valid_supported_java_21(self):
        first_home = self.root / "java-arm64-lower"
        second_home = self.root / "java-arm64-upper"
        first_java = self.make_java(first_home)
        self.make_java(second_home)
        environment = {
            "JAVA_HOME_21_arm64": str(first_home),
            "JAVA_HOME_21_ARM64": str(second_home),
            "JAVA_HOME": str(self.root / "ignored"),
        }
        with mock.patch.object(
            self.lifecycle.subprocess,
            "run",
            return_value=self.lifecycle.subprocess.CompletedProcess(
                [str(first_java)], 0, "", 'openjdk version "21.0.7"\n'
            ),
        ) as run_java:
            java_home, java = self.lifecycle.select_java(environment)
        self.assertEqual(java_home, first_home.resolve())
        self.assertEqual(java, first_java.resolve())
        run_java.assert_called_once_with(
            [str(first_java.resolve()), "-version"], capture_output=True, text=True, timeout=15, check=False
        )

    def test_java_selection_skips_invalid_candidates_and_uses_existing_java_home(self):
        old_home = self.root / "java-17"
        valid_home = self.root / "java-existing-21"
        old_java = self.make_java(old_home)
        valid_java = self.make_java(valid_home)
        outputs = [
            self.lifecycle.subprocess.CompletedProcess([str(old_java)], 0, "", 'openjdk version "17.0.1"'),
            self.lifecycle.subprocess.CompletedProcess([str(valid_java)], 0, "", 'openjdk version "21.0.1"'),
        ]
        with mock.patch.object(self.lifecycle.subprocess, "run", side_effect=outputs) as run_java:
            java_home, java = self.lifecycle.select_java(
                {"JAVA_HOME_21_arm64": str(old_home), "JAVA_HOME": str(valid_home)}
            )
        self.assertEqual(java_home, valid_home.resolve())
        self.assertEqual(java, valid_java.resolve())
        self.assertEqual(run_java.call_count, 2)

    def test_prepare_exports_private_android_homes_under_exclusive_run_directory(self):
        root = self.prepare()
        values = dict(
            line.split("=", 1) for line in self.github_env.read_text(encoding="utf-8").splitlines()
        )
        self.assertEqual(root.name, f"mc-hwasan-{self.run_id}-{self.attempt}")
        for variable, child in (
            ("ANDROID_AVD_HOME", "avd"),
            ("ANDROID_USER_HOME", "user"),
            ("ANDROID_EMULATOR_HOME", "emulator"),
            ("ANDROID_SDK_HOME", "sdk"),
        ):
            self.assertEqual(Path(values[variable]), root / child)
            self.assertTrue((root / child).is_dir())
        self.assertEqual(Path(values["ANDROID_SDK_ROOT"]), root / "android-sdk")
        self.assertEqual(Path(values["ANDROID_HOME"]), root / "android-sdk")
        self.assertEqual(values["ANDROID_SERIAL"], "emulator-5558")
        with self.assertRaisesRegex(self.lifecycle.LifecycleError, "already exists"):
            self.prepare()

    def test_start_rejects_sdk_root_outside_owned_run_subtree(self):
        self.prepare()
        shared_sdk_root = self.runner_temp / "android-sdk"
        shared_sdk_root.mkdir()
        with mock.patch.object(self.lifecycle, "_assert_device_and_ports_free") as check_device:
            with self.assertRaisesRegex(self.lifecycle.LifecycleError, "run-owned SDK directory"):
                self.lifecycle.start(self.runner_temp, self.run_id, self.attempt, shared_sdk_root)
        check_device.assert_not_called()

    def test_start_rejects_unsupported_host_before_installing_emulator_tools(self):
        root = self.prepare()
        sdk_root = root / "android-sdk"
        sdk_root.mkdir()
        with mock.patch.object(self.lifecycle.platform, "system", return_value="Windows"), mock.patch.object(
            self.lifecycle, "_assert_device_and_ports_free"
        ), mock.patch.object(self.lifecycle, "_run_stream") as install_tools, mock.patch.object(
            self.lifecycle.subprocess, "Popen"
        ) as popen:
            with self.assertRaisesRegex(self.lifecycle.LifecycleError, "unsupported Android emulator host"):
                self.lifecycle.start(self.runner_temp, self.run_id, self.attempt, sdk_root)
        install_tools.assert_not_called()
        popen.assert_not_called()

    def test_start_creates_isolated_avd_without_force_and_requires_hvf_on_macos(self):
        root, sdk_root, popen, commands = self.setup_started_emulator()
        state = json.loads((root / self.lifecycle.STATE_FILE).read_text(encoding="utf-8"))
        self.assertEqual(state["avd_name"], f"mc-hwasan-{self.run_id}-{self.attempt}")
        self.assertEqual(Path(state["avd_home"]), root / "avd")
        self.assertEqual(state["sdk_root"], str(sdk_root.resolve()))
        avd_call = next(command for command, _ in commands if "create" in command and "avd" in command)
        self.assertEqual(avd_call[avd_call.index("--name") + 1], state["avd_name"])
        self.assertNotIn("--force", avd_call)
        avd_env = next(
            kwargs["env"] for command, kwargs in commands if "create" in command and "avd" in command
        )
        self.assertEqual(avd_env["ANDROID_AVD_HOME"], str(root / "avd"))
        self.assertEqual(avd_env["ANDROID_USER_HOME"], str(root / "user"))
        accel_check = next(command for command, _ in commands if command[-1:] == ["-accel-check"])
        self.assertEqual(accel_check[-1], "-accel-check")
        launch = popen.call_args.args[0]
        self.assertIn("-port", launch)
        self.assertEqual(launch[launch.index("-port") + 1], "5558")
        self.assertIn("-accel", launch)
        self.assertEqual(launch[launch.index("-accel") + 1], "on")
        self.assertNotIn("off", launch)
        self.assertNotIn("ANDROID_I_WANT_MY_TCG", popen.call_args.kwargs["env"])
        self.assertTrue(state["process_start_identity"])
        self.assertTrue(
            all(command[1:3] == ["-s", "emulator-5558"] for command, _ in commands if command[-1:] == ["get-state"])
        )

    def test_start_accepts_kvm_and_launches_accelerated_emulator_on_linux(self):
        root, sdk_root, popen, commands = self.setup_started_emulator(
            host_platform="Linux",
            accel_output="accel: 0\nKVM (version 12) is installed and usable.\n",
        )
        self.assertTrue(root.is_dir())
        accel_check = next(command for command, _ in commands if command[-1:] == ["-accel-check"])
        self.assertEqual(accel_check[0], str((sdk_root / "emulator" / "emulator").resolve()))
        launch = popen.call_args.args[0]
        self.assertEqual(launch[launch.index("-accel") + 1], "on")

    def test_start_fails_closed_before_launch_when_expected_acceleration_is_missing(self):
        root = self.prepare()
        sdk_root = root / "android-sdk"
        emulator = sdk_root / "emulator" / "emulator"
        emulator.parent.mkdir(parents=True)
        emulator.touch()
        with mock.patch.object(self.lifecycle, "_assert_device_and_ports_free"), mock.patch.object(
            self.lifecycle, "_run_stream"
        ), mock.patch.object(
            self.lifecycle, "_run", return_value="accel: 0\nHypervisor.Framework OS X Version 26.6\n"
        ), mock.patch.object(self.lifecycle.platform, "system", return_value="Linux"), mock.patch.object(
            self.lifecycle.subprocess, "Popen"
        ) as popen:
            with self.assertRaisesRegex(self.lifecycle.LifecycleError, "usable KVM"):
                self.lifecycle.start(self.runner_temp, self.run_id, self.attempt, sdk_root)
        popen.assert_not_called()

    def test_start_fails_closed_before_launch_when_kvm_is_reported_unusable(self):
        root = self.prepare()
        sdk_root = root / "android-sdk"
        emulator = sdk_root / "emulator" / "emulator"
        emulator.parent.mkdir(parents=True)
        emulator.touch()
        with mock.patch.object(self.lifecycle, "_assert_device_and_ports_free"), mock.patch.object(
            self.lifecycle, "_run_stream"
        ), mock.patch.object(
            self.lifecycle, "_run", return_value="accel: 0\nKVM is not installed and not usable.\n"
        ), mock.patch.object(self.lifecycle.platform, "system", return_value="Linux"), mock.patch.object(
            self.lifecycle.subprocess, "Popen"
        ) as popen:
            with self.assertRaisesRegex(self.lifecycle.LifecycleError, "usable KVM"):
                self.lifecycle.start(self.runner_temp, self.run_id, self.attempt, sdk_root)
        popen.assert_not_called()

    def test_start_fails_closed_when_emulator_serial_is_already_registered(self):
        root = self.prepare()
        sdk_root = root / "android-sdk"
        adb = sdk_root / "platform-tools" / "adb"
        adb.parent.mkdir(parents=True)
        adb.touch()
        listing = self.lifecycle.subprocess.CompletedProcess(
            [str(adb), "-s", "emulator-5558", "get-state"], 0, "device\n", ""
        )
        with mock.patch.object(self.lifecycle.subprocess, "run", return_value=listing), mock.patch.object(
            self.lifecycle.subprocess, "Popen"
        ) as popen:
            with self.assertRaisesRegex(self.lifecycle.LifecycleError, "already registered"):
                self.lifecycle.start(self.runner_temp, self.run_id, self.attempt, sdk_root)
        popen.assert_not_called()

    def test_start_fails_closed_when_either_emulator_port_is_occupied(self):
        root = self.prepare()
        sdk_root = root / "android-sdk"
        adb = sdk_root / "platform-tools" / "adb"
        adb.parent.mkdir(parents=True)
        adb.touch()
        listing = self.lifecycle.subprocess.CompletedProcess(
            [str(adb), "-s", "emulator-5558", "get-state"],
            1,
            "",
            "error: device 'emulator-5558' not found\n",
        )
        for occupied_port in (5558, 5559):
            with self.subTest(port=occupied_port), mock.patch.object(
                self.lifecycle.subprocess, "run", return_value=listing
            ), mock.patch.object(
                self.lifecycle, "_port_available", side_effect=lambda port: port != occupied_port
            ), mock.patch.object(self.lifecycle.subprocess, "Popen") as popen:
                with self.assertRaisesRegex(self.lifecycle.LifecycleError, f"port {occupied_port} is occupied"):
                    self.lifecycle.start(self.runner_temp, self.run_id, self.attempt, sdk_root)
                popen.assert_not_called()

    def test_cleanup_preserves_root_for_foreign_executable_and_reused_pid(self):
        for reused_pid in (False, True):
            with self.subTest(reused_pid=reused_pid):
                run_id = self.run_id if not reused_pid else "123457"
                attempt = self.attempt
                if reused_pid:
                    self.run_id = run_id
                root, sdk_root, _, _ = self.setup_started_emulator()
                state = json.loads((root / self.lifecycle.STATE_FILE).read_text(encoding="utf-8"))
                expected_identity = state["process_start_identity"]
                emulator = Path(state["emulator_path"])
                avd_name = state["avd_name"]
                command = self.process_command(emulator, avd_name, " -no-window")
                if not reused_pid:
                    command = self.process_command(sdk_root / "other-emulator", avd_name)
                actual_identity = "Thu Oct  7 10:01:00 2026" if reused_pid else expected_identity
                with mock.patch.object(
                    self.lifecycle, "_process_snapshot", return_value=(actual_identity, command)
                ), mock.patch.object(self.lifecycle.os, "kill") as kill:
                    with self.assertRaisesRegex(self.lifecycle.LifecycleError, "unrecognized|reused"):
                        self.lifecycle.cleanup(self.runner_temp, run_id, attempt)
                kill.assert_not_called()
                self.assertTrue(root.is_dir())

    def test_cleanup_terminates_only_the_matching_emulator_and_removes_its_private_root(self):
        root, _, _, _ = self.setup_started_emulator()
        state = json.loads((root / self.lifecycle.STATE_FILE).read_text(encoding="utf-8"))
        command = self.process_command(Path(state["emulator_path"]), state["avd_name"])
        process_snapshot = (state["process_start_identity"], command)
        with mock.patch.object(
            self.lifecycle, "_process_snapshot", side_effect=[process_snapshot, None]
        ), mock.patch.object(self.lifecycle.os, "kill") as kill:
            self.lifecycle.cleanup(self.runner_temp, self.run_id, self.attempt)
        kill.assert_called_once_with(4321, self.lifecycle.signal.SIGTERM)
        self.assertFalse(root.exists())

    def test_cleanup_treats_exit_before_sigterm_as_already_stopped(self):
        root, _, _, _ = self.setup_started_emulator()
        state = json.loads((root / self.lifecycle.STATE_FILE).read_text(encoding="utf-8"))
        snapshot = (
            state["process_start_identity"],
            self.process_command(Path(state["emulator_path"]), state["avd_name"]),
        )
        with mock.patch.object(self.lifecycle, "_process_snapshot", return_value=snapshot), mock.patch.object(
            self.lifecycle.os,
            "kill",
            side_effect=ProcessLookupError,
        ) as kill:
            self.lifecycle.cleanup(self.runner_temp, self.run_id, self.attempt)
        kill.assert_called_once_with(4321, self.lifecycle.signal.SIGTERM)
        self.assertFalse(root.exists())

    def test_cleanup_treats_exit_before_sigkill_as_already_stopped(self):
        root, _, _, _ = self.setup_started_emulator()
        state = json.loads((root / self.lifecycle.STATE_FILE).read_text(encoding="utf-8"))
        snapshot = (
            state["process_start_identity"],
            self.process_command(Path(state["emulator_path"]), state["avd_name"]),
        )
        windows_signal = SimpleNamespace(SIGTERM=self.lifecycle.signal.SIGTERM)
        sigkill = 9
        with mock.patch.object(
            self.lifecycle,
            "signal",
            windows_signal,
        ), mock.patch.object(
            self.lifecycle.signal,
            "SIGKILL",
            sigkill,
            create=True,
        ), mock.patch.object(
            self.lifecycle,
            "_process_snapshot",
            side_effect=[snapshot, snapshot],
        ), mock.patch.object(
            self.lifecycle.time,
            "monotonic",
            side_effect=[0, 11],
        ), mock.patch.object(
            self.lifecycle.os,
            "kill",
            side_effect=[None, ProcessLookupError],
        ) as kill:
            self.lifecycle.cleanup(self.runner_temp, self.run_id, self.attempt)
        self.assertEqual(
            kill.call_args_list,
            [
                mock.call(4321, self.lifecycle.signal.SIGTERM),
                mock.call(4321, sigkill),
            ],
        )
        self.assertFalse(hasattr(windows_signal, "SIGKILL"))
        self.assertFalse(root.exists())

    def test_cleanup_preserves_root_when_signal_fails_for_other_reason(self):
        root, _, _, _ = self.setup_started_emulator()
        state = json.loads((root / self.lifecycle.STATE_FILE).read_text(encoding="utf-8"))
        snapshot = (
            state["process_start_identity"],
            self.process_command(Path(state["emulator_path"]), state["avd_name"]),
        )
        with mock.patch.object(self.lifecycle, "_process_snapshot", return_value=snapshot), mock.patch.object(
            self.lifecycle.os,
            "kill",
            side_effect=PermissionError("not permitted"),
        ) as kill:
            with self.assertRaises(PermissionError):
                self.lifecycle.cleanup(self.runner_temp, self.run_id, self.attempt)
        kill.assert_called_once_with(4321, self.lifecycle.signal.SIGTERM)
        self.assertTrue(root.is_dir())

    def test_wait_targets_only_the_recorded_adb_serial(self):
        root, sdk_root, _, _ = self.setup_started_emulator()
        state = json.loads((root / self.lifecycle.STATE_FILE).read_text(encoding="utf-8"))
        command = self.process_command(Path(state["emulator_path"]), state["avd_name"])
        completed = self.lifecycle.subprocess.CompletedProcess([], 0, "device\n", "")
        booted = self.lifecycle.subprocess.CompletedProcess([], 0, "1\n", "")
        with mock.patch.object(
            self.lifecycle.subprocess,
            "run",
            side_effect=[completed, booted, completed],
        ) as adb, mock.patch.object(
            self.lifecycle, "_process_snapshot", return_value=(state["process_start_identity"], command)
        ), mock.patch("builtins.print"):
            self.lifecycle.wait_for_boot(self.runner_temp, self.run_id, self.attempt, 2)
        commands = [call.args[0] for call in adb.call_args_list]
        self.assertEqual(len(commands), 3)
        self.assertTrue(all(command[1:3] == ["-s", "emulator-5558"] for command in commands))
        self.assertTrue(
            all(command[0] == str((sdk_root / "platform-tools" / "adb").resolve()) for command in commands)
        )


if __name__ == "__main__":
    unittest.main()
