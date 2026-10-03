#!/usr/bin/env python3
"""Start or reuse one Android playtest client on the project's S7 AVD."""

from __future__ import annotations

import argparse
import contextlib
import errno
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Iterator, Sequence


PACKAGE = "com.corejust.minecraftclone"
ACTIVITY = "android.app.NativeActivity"
COMPONENT = f"{PACKAGE}/{ACTIVITY}"
S7_AVD = "MinecraftClone_S7_ReleaseClean_API_35"


class AndroidPlaytestError(RuntimeError):
    """The requested Android playtest could not be started safely."""


def _adb_executable() -> str:
    configured = os.environ.get("ADB_PATH") or os.environ.get("ADB")
    if configured:
        candidate = Path(configured).expanduser()
        if candidate.is_file():
            return str(candidate.resolve())
        has_path_component = candidate.is_absolute() or os.sep in configured or (
            os.altsep is not None and os.altsep in configured
        )
        found = None if has_path_component else shutil.which(configured)
        if found:
            return found
        setting = "ADB_PATH" if os.environ.get("ADB_PATH") else "ADB"
        raise AndroidPlaytestError(
            f"{setting} does not identify an Android Debug Bridge executable: {configured!r}; "
            "set it to the SDK platform-tools/adb path"
        )

    executable = "adb.exe" if os.name == "nt" else "adb"
    for setting in ("ANDROID_SDK_ROOT", "ANDROID_HOME"):
        sdk_root = os.environ.get(setting)
        if not sdk_root:
            continue
        candidate = Path(sdk_root).expanduser() / "platform-tools" / executable
        if candidate.is_file():
            return str(candidate.resolve())

    found = shutil.which(executable)
    if found:
        return found
    raise AndroidPlaytestError(
        "Android Debug Bridge was not found; set ADB_PATH or ANDROID_SDK_ROOT/ANDROID_HOME, "
        "or add Android SDK platform-tools to PATH"
    )


def _launch_lock_path() -> Path:
    user = str(os.getuid()) if hasattr(os, "getuid") else os.environ.get("USERNAME", "user")
    user = re.sub(r"[^A-Za-z0-9_.-]", "_", user)
    return Path(tempfile.gettempdir()) / f"minecraftclone-{user}-{S7_AVD}-playtest.lock"


def _lock_contention(error: OSError) -> bool:
    if os.name == "nt":
        return error.errno in {errno.EACCES, errno.EAGAIN} or getattr(error, "winerror", None) in {32, 33}
    return error.errno in {errno.EACCES, errno.EAGAIN}


@contextlib.contextmanager
def _launch_lock(timeout_seconds: float, lock_path: Path | None = None) -> Iterator[None]:
    """Serialize check-and-start across processes; the OS releases this lock on exit."""
    if timeout_seconds <= 0:
        raise ValueError("launch lock timeout must be positive")
    selected_path = lock_path or _launch_lock_path()
    lock_file = selected_path.open("a+b")
    locked = False
    try:
        if os.name == "nt":
            import msvcrt

            lock_file.seek(0, os.SEEK_END)
            if lock_file.tell() == 0:
                lock_file.write(b"\0")
                lock_file.flush()
        else:
            import fcntl

        deadline = time.monotonic() + timeout_seconds
        while True:
            try:
                if os.name == "nt":
                    lock_file.seek(0)
                    msvcrt.locking(lock_file.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                locked = True
                break
            except OSError as error:
                if not _lock_contention(error):
                    raise
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AndroidPlaytestError(
                        "another Android playtest launch is still in progress; "
                        "wait for it to finish and retry"
                    ) from error
                time.sleep(min(0.05, remaining))

        try:
            yield
        finally:
            if locked:
                if os.name == "nt":
                    lock_file.seek(0)
                    msvcrt.locking(lock_file.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
    finally:
        lock_file.close()


def run_adb(serial: str | None, *arguments: str, timeout: int = 30) -> str:
    command = [_adb_executable()]
    if serial is not None:
        command.extend(("-s", serial))
    command.extend(arguments)
    completed = subprocess.run(command, text=True, capture_output=True, timeout=timeout, check=False)
    output = completed.stdout + completed.stderr
    if completed.returncode:
        raise AndroidPlaytestError(
            f"adb {' '.join(arguments)} on {serial or 'host'} exited {completed.returncode}: {output.strip()}"
        )
    return completed.stdout.strip()


def connected_devices() -> list[str]:
    output = run_adb(None, "devices", "-l")
    devices: list[str] = []
    unavailable: list[str] = []
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 2 or fields[0] == "List":
            continue
        if fields[1] == "device":
            devices.append(fields[0])
        else:
            unavailable.append(f"{fields[0]} ({fields[1]})")
    if unavailable:
        raise AndroidPlaytestError(
            "cannot rule out another game instance while Android devices are unavailable: "
            + ", ".join(unavailable)
        )
    return devices


def running_process_id(serial: str) -> str:
    completed = subprocess.run(
        [_adb_executable(), "-s", serial, "shell", "pidof", PACKAGE],
        text=True,
        capture_output=True,
        timeout=30,
        check=False,
    )
    if completed.returncode == 0:
        return completed.stdout.strip()
    if completed.returncode == 1 and not completed.stdout.strip() and not completed.stderr.strip():
        return ""
    output = completed.stdout + completed.stderr
    raise AndroidPlaytestError(f"adb shell pidof {PACKAGE} on {serial} failed: {output.strip()}")


def is_game_top_resumed(activity_dump: str) -> bool:
    lines = activity_dump.splitlines()
    top_activity = next(
        (line for line in lines if "topResumedActivity=" in line),
        "",
    )
    resumed_activity = next(
        (line for line in lines if line.strip().startswith("ResumedActivity:")),
        "",
    )
    record_match = re.search(r"ActivityRecord\{(\S+)", top_activity)
    if not record_match or COMPONENT not in top_activity:
        return False

    record_id = record_match.group(1)
    if f"ActivityRecord{{{record_id} " not in resumed_activity or COMPONENT not in resumed_activity:
        return False

    for index, line in enumerate(lines):
        if not line.strip().startswith("* Hist") or f"ActivityRecord{{{record_id} " not in line:
            continue
        if COMPONENT not in line:
            return False
        activity_lines = []
        for activity_line in lines[index + 1 :]:
            if activity_line.strip().startswith("* Hist"):
                break
            activity_lines.append(activity_line.strip())
        return (
            f"mActivityComponent={COMPONENT}" in activity_lines
            and any(line.startswith("state=RESUMED") for line in activity_lines)
        )
    return False


def has_game_activity_record(activity_dump: str) -> bool:
    expected = f"mActivityComponent={COMPONENT}"
    return any(line.strip() == expected for line in activity_dump.splitlines())


def require_framework_ready(serial: str) -> None:
    if run_adb(serial, "shell", "getprop", "sys.boot_completed") != "1":
        raise AndroidPlaytestError(f"Android framework has not completed boot on {serial}")
    if not run_adb(serial, "shell", "pm", "path", "android").startswith("package:"):
        raise AndroidPlaytestError(f"Android framework package is unavailable on {serial}")
    services = set(run_adb(serial, "shell", "dumpsys", "-l").split())
    if not {"activity", "package"} <= services:
        raise AndroidPlaytestError(f"Android activity/package services are unavailable on {serial}")


def launch_or_reuse(serial: str, timeout_seconds: int = 60) -> dict[str, str]:
    with _launch_lock(timeout_seconds):
        return _launch_or_reuse_locked(serial, timeout_seconds)


def _launch_or_reuse_locked(serial: str, timeout_seconds: int) -> dict[str, str]:
    devices = connected_devices()
    if serial not in devices:
        raise AndroidPlaytestError(f"selected serial {serial} is not ready; available devices: {', '.join(devices) or 'none'}")

    avd_name = run_adb(serial, "shell", "getprop", "ro.boot.qemu.avd_name")
    if avd_name != S7_AVD:
        raise AndroidPlaytestError(f"{serial} is AVD {avd_name!r}; S7 acceptance requires {S7_AVD!r}")

    require_framework_ready(serial)
    instances = {device: running_process_id(device) for device in devices}
    instances = {device: process_id for device, process_id in instances.items() if process_id}
    if len(instances) > 1:
        targets = ", ".join(f"{device} (PID {process_id})" for device, process_id in instances.items())
        raise AndroidPlaytestError(f"multiple game clients are already running: {targets}; refusing another launch")
    process_id = ""
    launch_action = "launched"
    reorder_existing_activity = False
    if instances:
        active_serial, process_id = next(iter(instances.items()))
        if active_serial != serial:
            raise AndroidPlaytestError(
                f"game client already runs on {active_serial}; refusing a second client on {serial}"
            )
        activity_dump = run_adb(serial, "shell", "dumpsys", "activity", "activities")
        if is_game_top_resumed(activity_dump):
            return {"action": "reused", "avd": avd_name, "pid": process_id, "serial": serial}
        if has_game_activity_record(activity_dump):
            launch_action = "foregrounded"
            reorder_existing_activity = True
    launch_arguments = ["shell", "am", "start", "-W"]
    if reorder_existing_activity:
        launch_arguments.append("--activity-reorder-to-front")
    launch_arguments.extend(("-n", COMPONENT))
    launch_output = run_adb(
        serial,
        *launch_arguments,
        timeout=30,
    )
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        process_id = running_process_id(serial)
        if process_id:
            activity_dump = run_adb(serial, "shell", "dumpsys", "activity", "activities")
            if is_game_top_resumed(activity_dump):
                return {
                    "action": launch_action,
                    "avd": avd_name,
                    "launch": launch_output,
                    "pid": process_id,
                    "serial": serial,
                }
        time.sleep(0.25)
    raise AndroidPlaytestError(
        f"NativeActivity on {serial} did not become top-resumed within {timeout_seconds} seconds after one launch"
    )


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True, help="explicit ADB serial for the S7 AVD")
    parser.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args(argv)
    if args.timeout < 1 or args.timeout > 120:
        parser.error("--timeout must be between 1 and 120 seconds")
    try:
        print(json.dumps(launch_or_reuse(args.serial, args.timeout), sort_keys=True))
        return 0
    except (AndroidPlaytestError, OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL Android playtest launch: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
