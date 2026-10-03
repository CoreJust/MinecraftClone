#!/usr/bin/env python3
"""Run one supervised Release host server and one guarded Android playtest client."""

from __future__ import annotations

import argparse
import errno
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Sequence

import android_playtest


HOST = "127.0.0.1"
PORT = 20_040
RENDER_DISTANCE = 72
DEFAULT_SERVER = Path(__file__).resolve().parents[1] / "build" / "release" / (
    "mc_main.exe" if os.name == "nt" else "mc_main"
)


class AndroidPairPlaytestError(RuntimeError):
    """The paired Android playtest cannot be started or kept running safely."""


def _require_release_server(server_binary: Path) -> Path:
    resolved = server_binary.expanduser().resolve()
    if not resolved.is_file():
        raise AndroidPairPlaytestError(
            f"Release host executable not found: {resolved}; build it with "
            "`cmake --preset release && cmake --build --preset release`"
        )
    if os.name != "nt" and not os.access(resolved, os.X_OK):
        raise AndroidPairPlaytestError(f"Release host executable is not executable: {resolved}")
    return resolved


def _udp_port_available(host: str, port: int) -> bool:
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        try:
            probe.bind((host, port))
        except OSError as error:
            if error.errno == errno.EADDRINUSE:
                return False
            raise
        return True
    finally:
        probe.close()


def _require_port_free() -> None:
    if not _udp_port_available(HOST, PORT):
        raise AndroidPairPlaytestError(
            f"UDP {PORT} is already in use; refusing to start another server or stop an unknown process. "
            "Inspect the existing playtest server, then retry."
        )


def _wait_for_server(server: subprocess.Popen[bytes], timeout_seconds: float) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        return_code = server.poll()
        if return_code is not None:
            raise AndroidPairPlaytestError(f"Release host exited before binding UDP {PORT} (exit {return_code})")
        if not _udp_port_available(HOST, PORT):
            return
        time.sleep(0.1)
    raise AndroidPairPlaytestError(f"Release host did not bind UDP {PORT} within {timeout_seconds:g} seconds")


def _stop_owned_server(server: subprocess.Popen[bytes]) -> None:
    if server.poll() is not None:
        return
    server.terminate()
    try:
        server.wait(timeout=5)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait(timeout=5)


def run_playtest(
    serial: str,
    server_binary: Path,
    server_timeout: float = 20,
    client_timeout: int = 60,
) -> dict[str, str]:
    server_path = _require_release_server(server_binary)
    _require_port_free()
    command = [
        str(server_path),
        "--server",
        "--port",
        str(PORT),
        "--render-distance",
        str(RENDER_DISTANCE),
    ]
    print(f"Starting one Release host server: {server_path}", flush=True)
    server = subprocess.Popen(command)
    try:
        _wait_for_server(server, server_timeout)
        print(f"Host is listening on {HOST}:{PORT}; checking the selected Android client.", flush=True)
        receipt = android_playtest.launch_or_reuse(serial, timeout_seconds=client_timeout)
        print(f"Android client ready: {json.dumps(receipt, sort_keys=True)}", flush=True)
        print("Playtest is live. Leave this command running; press Ctrl-C to stop only this host server.", flush=True)
        return_code = server.wait()
        raise AndroidPairPlaytestError(f"Release host exited during the playtest (exit {return_code})")
    finally:
        _stop_owned_server(server)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True, help="explicit ADB serial for the S7 ReleaseClean AVD")
    parser.add_argument("--server-timeout", type=float, default=20, help="seconds to wait for UDP 20040 to bind")
    parser.add_argument("--client-timeout", type=int, default=60, help="seconds allowed for guarded Android launch")
    args = parser.parse_args(argv)
    if not 1 <= args.server_timeout <= 120:
        parser.error("--server-timeout must be between 1 and 120 seconds")
    if not 1 <= args.client_timeout <= 120:
        parser.error("--client-timeout must be between 1 and 120 seconds")
    try:
        run_playtest(args.serial, DEFAULT_SERVER, args.server_timeout, args.client_timeout)
        return 0
    except KeyboardInterrupt:
        print("Stopping the supervised Android host server.", file=sys.stderr, flush=True)
        return 130
    except (AndroidPairPlaytestError, android_playtest.AndroidPlaytestError, OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL Android paired playtest: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
