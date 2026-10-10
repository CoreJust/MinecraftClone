#!/usr/bin/env python3
"""Own the isolated Android HWASan emulator lifecycle on an accelerated host."""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Sequence


SERIAL = "emulator-5558"
CONSOLE_PORT = 5558
ADB_PORT = 5559
SYSTEM_IMAGE = "system-images;android-35;google_apis;arm64-v8a"
STATE_FILE = "owner.json"
EMULATOR_EXECUTABLES = {
    "emulator",
    "emulator64-arm",
    "qemu-system-aarch64",
    "qemu-system-aarch64-headless",
}
JAVA_HOME_KEYS = (
    "JAVA_HOME_21_arm64",
    "JAVA_HOME_21_ARM64",
    "JAVA_HOME_21_X64",
    "JAVA_HOME",
)


class LifecycleError(RuntimeError):
    """The requested emulator lifecycle operation could not be done safely."""


def _identity(run_id: str, run_attempt: str) -> tuple[str, str]:
    if not re.fullmatch(r"[0-9]+", run_id) or not re.fullmatch(r"[0-9]+", run_attempt):
        raise LifecycleError("run-id and run-attempt must contain only decimal digits")
    return run_id, run_attempt


def _root(runner_temp: Path, run_id: str, run_attempt: str) -> Path:
    _identity(run_id, run_attempt)
    return runner_temp.resolve() / f"mc-hwasan-{run_id}-{run_attempt}"


def _state_path(root: Path) -> Path:
    return root / STATE_FILE


def _write_state(root: Path, state: dict[str, object]) -> None:
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=root, prefix=".owner-", delete=False
    ) as output:
        temporary = Path(output.name)
        json.dump(state, output, sort_keys=True)
        output.write("\n")
    temporary.chmod(0o600)
    temporary.replace(_state_path(root))


def _read_state(root: Path, run_id: str, run_attempt: str) -> dict[str, object]:
    expected_root = _root(root.parent, run_id, run_attempt)
    if root.resolve() != expected_root or root.is_symlink() or not root.is_dir():
        raise LifecycleError(f"refusing unexpected lifecycle directory: {root}")
    try:
        state = json.loads(_state_path(root).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise LifecycleError(f"missing or invalid ownership record in {root}") from error
    if not isinstance(state, dict):
        raise LifecycleError(f"invalid ownership record in {root}")
    expected_name = f"mc-hwasan-{run_id}-{run_attempt}"
    expected_paths = {
        "root": str(root),
        "avd_home": str(root / "avd"),
        "user_home": str(root / "user"),
        "emulator_home": str(root / "emulator"),
        "sdk_root": str(root / "android-sdk"),
        "avd_name": expected_name,
        "run_id": run_id,
        "run_attempt": run_attempt,
        "serial": SERIAL,
        "ports": [CONSOLE_PORT, ADB_PORT],
    }
    if any(state.get(key) != value for key, value in expected_paths.items()):
        raise LifecycleError(f"ownership record does not match this run in {root}")
    return state


def _environment(state: dict[str, object]) -> dict[str, str]:
    user_home = Path(str(state["user_home"]))
    return {
        "ANDROID_AVD_HOME": str(state["avd_home"]),
        "ANDROID_USER_HOME": str(user_home / ".android"),
        "ANDROID_EMULATOR_HOME": str(state["emulator_home"]),
        "ANDROID_SDK_HOME": str(user_home),
        "ANDROID_HOME": str(state["sdk_root"]),
        "ANDROID_SDK_ROOT": str(state["sdk_root"]),
        "ANDROID_SERIAL": SERIAL,
    }


def _append_github_env(destination: Path, values: dict[str, str]) -> None:
    with destination.open("a", encoding="utf-8") as output:
        for name, value in values.items():
            if "\n" in value or "\r" in value:
                raise LifecycleError(f"cannot export a multiline value for {name}")
            output.write(f"{name}={value}\n")


def prepare(runner_temp: Path, run_id: str, run_attempt: str, github_env: Path) -> Path:
    _identity(run_id, run_attempt)
    runner_temp = runner_temp.resolve(strict=True)
    if not runner_temp.is_dir():
        raise LifecycleError(f"RUNNER_TEMP is not a directory: {runner_temp}")
    root = _root(runner_temp, run_id, run_attempt)
    try:
        root.mkdir(mode=0o700)
    except FileExistsError as error:
        raise LifecycleError(f"run-owned lifecycle directory already exists: {root}") from error

    state: dict[str, object] = {
        "root": str(root),
        "run_id": run_id,
        "run_attempt": run_attempt,
        "avd_name": f"mc-hwasan-{run_id}-{run_attempt}",
        "avd_home": str(root / "avd"),
        "user_home": str(root / "user"),
        "emulator_home": str(root / "emulator"),
        "sdk_root": str(root / "android-sdk"),
        "serial": SERIAL,
        "ports": [CONSOLE_PORT, ADB_PORT],
        "emulator_path": None,
        "pid": None,
        "process_start_identity": None,
    }
    _write_state(root, state)
    for key in ("avd_home", "user_home", "emulator_home"):
        Path(str(state[key])).mkdir(mode=0o700)
    (Path(str(state["user_home"])) / ".android").mkdir(mode=0o700)
    _append_github_env(github_env, _environment(state))
    return root


def select_java(environment: dict[str, str]) -> tuple[Path, Path]:
    for key in JAVA_HOME_KEYS:
        candidate = environment.get(key)
        if not candidate:
            continue
        java_home = Path(candidate).expanduser().resolve()
        java = java_home / "bin" / "java"
        if not java.is_file() or not os.access(java, os.X_OK):
            continue
        completed = subprocess.run(
            [str(java), "-version"], capture_output=True, text=True, timeout=15, check=False
        )
        version = completed.stdout + completed.stderr
        match = re.search(r"(?:openjdk|java)(?: version)?\s+\"?(\d+)(?:[._+\"\s]|$)", version, re.I)
        if completed.returncode == 0 and match and int(match.group(1)) == 21:
            return java_home, java
    raise LifecycleError("no configured Java 21 installation passed validation")


def select_java_cli(github_env: Path, github_path: Path) -> None:
    java_home, java = select_java(dict(os.environ))
    _append_github_env(github_env, {"JAVA_HOME": str(java_home)})
    with github_path.open("a", encoding="utf-8") as output:
        output.write(f"{java.parent}\n")
    print(f"Selected Java 21 from {java_home}.", flush=True)


def _run(command: Sequence[str], *, env: dict[str, str], timeout: int = 600) -> str:
    completed = subprocess.run(
        list(command), text=True, capture_output=True, timeout=timeout, check=False, env=env
    )
    output = completed.stdout + completed.stderr
    if completed.returncode:
        raise LifecycleError(f"{' '.join(command)} exited {completed.returncode}: {output.strip()}")
    return output.strip()


def _run_stream(command: Sequence[str], *, env: dict[str, str], timeout: int = 1800) -> None:
    completed = subprocess.run(list(command), timeout=timeout, check=False, env=env)
    if completed.returncode:
        raise LifecycleError(f"{' '.join(command)} exited {completed.returncode}")


def _port_available(port: int) -> bool:
    listeners: list[socket.socket] = []
    try:
        for family, address in (
            (socket.AF_INET, ("0.0.0.0", port)),
            (socket.AF_INET6, ("::", port, 0, 0)),
        ):
            listener = socket.socket(family, socket.SOCK_STREAM)
            listeners.append(listener)
            if family == socket.AF_INET6:
                listener.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
            listener.bind(address)
    except OSError:
        return False
    finally:
        for listener in listeners:
            listener.close()
    return True


def _required_hypervisor(system_name: str | None = None) -> tuple[str, str]:
    host = system_name or platform.system()
    if host == "Linux":
        return "KVM", "kvm"
    if host == "Darwin":
        return "Hypervisor.Framework", "hypervisor.framework"
    raise LifecycleError(f"unsupported Android emulator host platform: {host}")


def _inside(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def _assert_device_and_ports_free(sdk_root: Path, env: dict[str, str]) -> None:
    adb = sdk_root / "platform-tools" / "adb"
    probe = subprocess.run(
        [str(adb), "-s", SERIAL, "get-state"],
        text=True,
        capture_output=True,
        timeout=30,
        check=False,
        env=env,
    )
    output = (probe.stdout + probe.stderr).strip()
    if probe.returncode == 0:
        raise LifecycleError(f"{SERIAL} is already registered with adb: {output or 'unknown state'}")
    not_found = re.search(rf"device ['\"]?{re.escape(SERIAL)}['\"]? not found", output, re.I)
    if probe.returncode != 1 or not not_found:
        raise LifecycleError(f"could not establish that {SERIAL} is free: {output or 'unexpected adb response'}")
    for port in (CONSOLE_PORT, ADB_PORT):
        if not _port_available(port):
            raise LifecycleError(f"emulator port {port} is occupied")


def start(runner_temp: Path, run_id: str, run_attempt: str, sdk_root: Path) -> int:
    root = _root(runner_temp, run_id, run_attempt)
    state = _read_state(root, run_id, run_attempt)
    if state.get("pid") is not None:
        raise LifecycleError("this lifecycle record already owns an emulator PID")
    sdk_root = sdk_root.resolve(strict=True)
    expected_sdk_root = Path(str(state["sdk_root"])).resolve()
    if not sdk_root.is_dir() or sdk_root != expected_sdk_root or not _inside(sdk_root, root.resolve()):
        raise LifecycleError(f"ANDROID_SDK_ROOT must be the run-owned SDK directory: {expected_sdk_root}")
    env = dict(os.environ)
    env.update(_environment(state))
    env["ANDROID_SDK_ROOT"] = str(sdk_root)
    env.pop("ANDROID_I_WANT_MY_TCG", None)
    hypervisor_name, hypervisor_marker = _required_hypervisor()
    _assert_device_and_ports_free(sdk_root, env)

    sdkmanager = sdk_root / "cmdline-tools" / "latest" / "bin" / "sdkmanager"
    avdmanager = sdk_root / "cmdline-tools" / "latest" / "bin" / "avdmanager"
    emulator = sdk_root / "emulator" / "emulator"
    print(f"Installing Android emulator and API 35 ARM64 system image under {sdk_root}.", flush=True)
    _run_stream(
        [str(sdkmanager), f"--sdk_root={sdk_root}", "--install", "emulator", SYSTEM_IMAGE],
        env=env,
    )
    avd_name = str(state["avd_name"])
    print(f"Creating isolated Android AVD {avd_name}.", flush=True)
    _run_stream(
        [
            str(avdmanager),
            "create",
            "avd",
            "--name",
            avd_name,
            "--package",
            SYSTEM_IMAGE,
            "--device",
            "pixel_2",
        ],
        env=env,
    )
    accel = _run([str(emulator), "-accel-check"], env=env, timeout=30)
    accel_lower = accel.lower()
    unavailable_markers = ("not installed", "not usable", "cannot be used", "can not be used")
    if hypervisor_marker not in accel_lower or any(marker in accel_lower for marker in unavailable_markers):
        raise LifecycleError(
            f"emulator -accel-check did not confirm usable {hypervisor_name}: {accel}"
        )
    _assert_device_and_ports_free(sdk_root, env)

    emulator = emulator.resolve(strict=True)
    if not _inside(emulator, sdk_root):
        raise LifecycleError(f"emulator executable is outside ANDROID_SDK_ROOT: {emulator}")
    state["sdk_root"] = str(sdk_root)
    state["emulator_path"] = str(emulator)
    _write_state(root, state)
    log = (root / "emulator.log").open("ab")
    try:
        process = subprocess.Popen(
            [str(emulator), "-avd", avd_name, "-port", str(CONSOLE_PORT), "-accel", "on", "-no-window", "-no-audio", "-no-boot-anim", "-no-snapshot"],
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
    finally:
        log.close()
    state["pid"] = process.pid
    _write_state(root, state)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise LifecycleError(f"emulator exited during startup; see {root / 'emulator.log'}")
        snapshot = _process_snapshot(process.pid)
        if snapshot is not None:
            identity, command = snapshot
            _verify_process(state, identity, command)
            state["process_start_identity"] = identity
            _write_state(root, state)
            return process.pid
        time.sleep(0.1)
    raise LifecycleError("could not establish emulator process identity; preserving its ownership directory")


def _process_snapshot(pid: int) -> tuple[str, str] | None:
    start = subprocess.run(
        ["ps", "-ww", "-p", str(pid), "-o", "lstart="],
        text=True,
        capture_output=True,
        timeout=10,
        check=False,
    )
    command = subprocess.run(
        ["ps", "-ww", "-p", str(pid), "-o", "command="],
        text=True,
        capture_output=True,
        timeout=10,
        check=False,
    )
    if start.returncode or command.returncode or not start.stdout.strip() or not command.stdout.strip():
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return None
        except PermissionError as error:
            raise LifecycleError(f"cannot verify recorded emulator PID {pid}") from error
        raise LifecycleError(f"could not verify recorded emulator PID {pid}; preserving lifecycle directory")
    return start.stdout.strip(), command.stdout.strip()


def _verify_process(state: dict[str, object], start_identity: str, command: str) -> None:
    sdk_root = Path(str(state.get("sdk_root", ""))).resolve(strict=True)
    emulator = Path(str(state.get("emulator_path", ""))).resolve(strict=True)
    if not _inside(emulator, sdk_root):
        raise LifecycleError("recorded emulator executable is outside the private SDK root")
    try:
        arguments = shlex.split(command)
    except ValueError as error:
        raise LifecycleError("cannot parse emulator process command line") from error
    expected = ["-avd", str(state["avd_name"]), "-port", str(CONSOLE_PORT)]
    if not arguments:
        raise LifecycleError("recorded PID has no executable command line")
    executable = Path(arguments[0]).resolve()
    if not _inside(executable, sdk_root):
        raise LifecycleError("recorded PID is running an executable outside the private SDK root")
    if executable.name not in EMULATOR_EXECUTABLES:
        raise LifecycleError("recorded PID is running an unrecognized emulator executable")
    for offset in range(0, len(expected), 2):
        flag, value = expected[offset : offset + 2]
        try:
            index = arguments.index(flag)
        except ValueError as error:
            raise LifecycleError(f"recorded PID is missing {flag} {value}") from error
        if arguments[index + 1 : index + 2] != [value]:
            raise LifecycleError(f"recorded PID has a different {flag} value")
    recorded_identity = state.get("process_start_identity")
    if recorded_identity is not None and recorded_identity != start_identity:
        raise LifecycleError("recorded PID has been reused by a different process")


def _signal_process_if_running(pid: int, signal_number: int) -> bool:
    try:
        os.kill(pid, signal_number)
    except ProcessLookupError:
        return False
    return True


def wait_for_boot(runner_temp: Path, run_id: str, run_attempt: str, timeout_seconds: int) -> None:
    root = _root(runner_temp, run_id, run_attempt)
    state = _read_state(root, run_id, run_attempt)
    if not isinstance(state.get("pid"), int) or not state.get("process_start_identity"):
        raise LifecycleError("no verified emulator process is recorded")
    sdk_root = Path(str(state["sdk_root"]))
    adb = sdk_root / "platform-tools" / "adb"
    env = dict(os.environ)
    env.update(_environment(state))
    deadline = time.monotonic() + timeout_seconds
    started_at = time.monotonic()
    next_status = started_at + 30
    latest_state = "ADB has not reported a device yet"
    print(
        f"Waiting up to {timeout_seconds}s for {SERIAL} to finish booting; status will be reported every 30s.",
        flush=True,
    )
    while time.monotonic() < deadline:
        snapshot = _process_snapshot(int(state["pid"]))
        if snapshot is None:
            raise LifecycleError(f"emulator process exited before boot; see {root / 'emulator.log'}")
        _verify_process(state, *snapshot)
        state_output = subprocess.run(
            [str(adb), "-s", SERIAL, "get-state"],
            text=True,
            capture_output=True,
            timeout=30,
            check=False,
            env=env,
        )
        latest_state = (state_output.stdout + state_output.stderr).strip().replace("\n", " ")
        if not latest_state:
            latest_state = f"adb get-state exited {state_output.returncode} without output"
        if state_output.returncode == 0 and state_output.stdout.strip() == "device":
            boot = subprocess.run(
                [str(adb), "-s", SERIAL, "shell", "getprop", "sys.boot_completed"],
                text=True,
                capture_output=True,
                timeout=30,
                check=False,
                env=env,
            )
            if boot.returncode == 0 and boot.stdout.strip() == "1":
                _run([str(adb), "-s", SERIAL, "shell", "input", "keyevent", "82"], env=env, timeout=30)
                elapsed = int(time.monotonic() - started_at)
                print(f"{SERIAL} finished booting after {elapsed}s.", flush=True)
                return
            latest_state = (boot.stdout + boot.stderr).strip().replace("\n", " ") or "Android is booting"
        now = time.monotonic()
        if now >= next_status:
            elapsed = int(now - started_at)
            print(f"Still waiting for {SERIAL}: elapsed {elapsed}s; latest adb state: {latest_state[:200]}.", flush=True)
            next_status = now + 30
        time.sleep(2)
    raise LifecycleError(f"{SERIAL} did not finish booting within {timeout_seconds} seconds")


def cleanup(runner_temp: Path, run_id: str, run_attempt: str) -> None:
    root = _root(runner_temp, run_id, run_attempt)
    if not root.exists():
        return
    state = _read_state(root, run_id, run_attempt)
    pid = state.get("pid")
    if pid is not None:
        if not isinstance(pid, int) or pid < 1:
            raise LifecycleError("invalid emulator PID in ownership record; preserving lifecycle directory")
        snapshot = _process_snapshot(pid)
        if snapshot is not None:
            _verify_process(state, *snapshot)
            if not state.get("process_start_identity"):
                raise LifecycleError("emulator process has no recorded start identity; preserving lifecycle directory")
            if _signal_process_if_running(pid, signal.SIGTERM):
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    snapshot = _process_snapshot(pid)
                    if snapshot is None:
                        break
                    _verify_process(state, *snapshot)
                    time.sleep(0.2)
                else:
                    snapshot = _process_snapshot(pid)
                    if snapshot is not None:
                        _verify_process(state, *snapshot)
                        if _signal_process_if_running(pid, signal.SIGKILL):
                            deadline = time.monotonic() + 5
                            while time.monotonic() < deadline:
                                snapshot = _process_snapshot(pid)
                                if snapshot is None:
                                    break
                                _verify_process(state, *snapshot)
                                time.sleep(0.2)
                            else:
                                snapshot = _process_snapshot(pid)
                                if snapshot is not None:
                                    _verify_process(state, *snapshot)
                                    raise LifecycleError(
                                        "verified emulator did not exit after SIGKILL; preserving lifecycle directory"
                                    )
    shutil.rmtree(root)


def _cli() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    select = commands.add_parser("select-java")
    select.add_argument("--github-env", type=Path, required=True)
    select.add_argument("--github-path", type=Path, required=True)
    prepare_parser = commands.add_parser("prepare")
    prepare_parser.add_argument("--runner-temp", type=Path, required=True)
    prepare_parser.add_argument("--run-id", required=True)
    prepare_parser.add_argument("--run-attempt", required=True)
    prepare_parser.add_argument("--github-env", type=Path, required=True)
    for name in ("start", "wait", "cleanup"):
        command = commands.add_parser(name)
        command.add_argument("--runner-temp", type=Path, required=True)
        command.add_argument("--run-id", required=True)
        command.add_argument("--run-attempt", required=True)
        if name == "start":
            command.add_argument("--sdk-root", type=Path, required=True)
        if name == "wait":
            command.add_argument("--timeout-seconds", type=int, default=1200)
    args = parser.parse_args()
    try:
        if args.command == "select-java":
            select_java_cli(args.github_env, args.github_path)
        elif args.command == "prepare":
            print(prepare(args.runner_temp, args.run_id, args.run_attempt, args.github_env))
        elif args.command == "start":
            print(start(args.runner_temp, args.run_id, args.run_attempt, args.sdk_root))
        elif args.command == "wait":
            wait_for_boot(args.runner_temp, args.run_id, args.run_attempt, args.timeout_seconds)
        else:
            cleanup(args.runner_temp, args.run_id, args.run_attempt)
        return 0
    except (LifecycleError, OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL Android HWASan emulator lifecycle: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(_cli())
