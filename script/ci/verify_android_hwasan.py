#!/usr/bin/env python3
"""Verify that an Android arm64 HWASan APK launches and remains healthy."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import zipfile
from pathlib import Path
from typing import Sequence


PACKAGE = "com.corejust.minecraftclone"
ACTIVITY = "android.app.NativeActivity"
WRAP_PATH = "lib/arm64-v8a/wrap.sh"
NATIVE_LIBRARY_PATH = "lib/arm64-v8a/libmc_android.so"
EXPECTED_WRAP = b'#!/system/bin/sh\nLD_HWASAN=1 exec "$@"\n'
HWASAN_RUNTIME = b"libclang_rt.hwasan-aarch64-android.so"
POST_LOAD_SURVIVAL_SECONDS = 5


class HwasanError(RuntimeError):
    """The requested runtime proof could not be collected."""


def run_adb(serial: str, *arguments: str, timeout: int = 30) -> str:
    completed = subprocess.run(
        ["adb", "-s", serial, *arguments],
        text=True,
        capture_output=True,
        timeout=timeout,
        check=False,
    )
    output = completed.stdout + completed.stderr
    if completed.returncode:
        raise HwasanError(f"adb {' '.join(arguments)} exited {completed.returncode}: {output.strip()}")
    return output.strip()


def running_process_id(serial: str) -> str:
    """Return the app PID, or an empty string while Android has not spawned it."""

    completed = subprocess.run(
        ["adb", "-s", serial, "shell", "pidof", PACKAGE],
        text=True,
        capture_output=True,
        timeout=30,
        check=False,
    )
    if completed.returncode == 0:
        return completed.stdout.strip()
    if completed.returncode == 1:
        return ""
    output = completed.stdout + completed.stderr
    raise HwasanError(f"adb shell pidof {PACKAGE} exited {completed.returncode}: {output.strip()}")


def verify_apk(apk: Path) -> None:
    if not apk.is_file():
        raise HwasanError(f"HWASan APK is missing: {apk}")
    try:
        with zipfile.ZipFile(apk) as archive:
            if archive.read(WRAP_PATH) != EXPECTED_WRAP:
                raise HwasanError(f"HWASan APK has an invalid {WRAP_PATH}")
            native_library = archive.read(NATIVE_LIBRARY_PATH)
    except (OSError, zipfile.BadZipFile, KeyError) as error:
        raise HwasanError(f"HWASan APK is missing required native content: {error}") from error
    if HWASAN_RUNTIME not in native_library:
        raise HwasanError("HWASan APK native library is not linked with the HWASan runtime")


def verify_runtime(serial: str, apk: Path, timeout_seconds: int) -> dict[str, str | int]:
    verify_apk(apk)
    run_adb(serial, "logcat", "-c")
    install_output = run_adb(serial, "install", "-r", str(apk), timeout=120)
    launch_output = run_adb(serial, "shell", "am", "start", "-n", f"{PACKAGE}/{ACTIVITY}")
    deadline = time.monotonic() + timeout_seconds
    process_id = ""
    failures = ("FATAL EXCEPTION", "HWAddressSanitizer", "UnsatisfiedLinkError")
    loaded_at: float | None = None
    while (now := time.monotonic()) < deadline:
        current_process_id = running_process_id(serial)
        if not current_process_id:
            if process_id:
                raise HwasanError("HWASan app exited before runtime verification completed")
            time.sleep(1)
            continue
        if not process_id:
            process_id = current_process_id
        elif current_process_id != process_id:
            raise HwasanError("HWASan app restarted during runtime verification")

        logcat = run_adb(serial, "logcat", "-d", "-v", "brief", "-t", "2000")
        for failure in failures:
            if failure in logcat:
                raise HwasanError(f"HWASan runtime log contains {failure}")
        if "libmc_android.so" in logcat and "using ns" in logcat:
            if loaded_at is None:
                loaded_at = now
            if now - loaded_at >= POST_LOAD_SURVIVAL_SECONDS:
                return {
                    "apk": str(apk),
                    "serial": serial,
                    "pid": process_id,
                    "install": install_output,
                    "launch": launch_output,
                    "healthy_seconds": int(now - loaded_at),
                }
        time.sleep(1)

    if not process_id:
        raise HwasanError(f"HWASan app did not remain running within {timeout_seconds} seconds")
    if loaded_at is None:
        raise HwasanError("HWASan runtime log does not prove libmc_android.so loaded")
    raise HwasanError(
        f"HWASan app did not remain healthy for {POST_LOAD_SURVIVAL_SECONDS} seconds after library load"
    )


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apk", type=Path, default=os.environ.get("MC_HWASAN_APK"))
    parser.add_argument("--serial", default=os.environ.get("ANDROID_SERIAL"))
    parser.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args(argv)
    if args.apk is None:
        parser.error("--apk or MC_HWASAN_APK is required")
    if not args.serial:
        parser.error("--serial or ANDROID_SERIAL is required")
    if args.timeout < 1 or args.timeout > 120:
        parser.error("--timeout must be between 1 and 120 seconds")
    try:
        print(json.dumps(verify_runtime(args.serial, args.apk, args.timeout), sort_keys=True))
        return 0
    except (HwasanError, OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL Android HWASan runtime: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
