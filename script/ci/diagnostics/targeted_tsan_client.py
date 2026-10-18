#!/usr/bin/env python3
"""Run the pinned delayed-server client case and retain bounded process evidence."""

from __future__ import annotations

import argparse
import io
import runpy
import subprocess
import sys
import unittest
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


TEST_NAME = "test_bot_remains_alive_until_delayed_server_starts"
WAIT_TIMEOUT_SECONDS = 5
MAX_LOG_BYTES = 4096


@dataclass
class OwnedProcess:
    name: str
    process: subprocess.Popen[Any]
    log_path: Path | None
    captured: set[str] = field(default_factory=set)


def _process_kind(arguments: Any) -> str | None:
    if isinstance(arguments, (str, bytes)):
        values = [str(arguments)]
    else:
        values = [str(value) for value in arguments]
    if "--bot-client" in values:
        return "client"
    if "--server" in values:
        return "server"
    return None


def _process_log_path(output: Any) -> Path | None:
    if not hasattr(output, "name") or not isinstance(output.name, (str, bytes)):
        return None
    return Path(output.name)


def _log_tail(log_path: Path | None) -> str:
    if log_path is None:
        return "<no file-backed process log>"
    try:
        log_size = log_path.stat().st_size
        with log_path.open("rb") as log_file:
            log_file.seek(max(0, log_size - MAX_LOG_BYTES))
            contents = log_file.read(MAX_LOG_BYTES).decode(errors="replace")
    except OSError as error:
        return f"<unavailable: {error}>"
    if log_size > MAX_LOG_BYTES:
        return f"<truncated; showing last {MAX_LOG_BYTES} bytes>\n{contents}"
    return contents or "<empty>"


def _record_process_state(
    record: OwnedProcess,
    phase: str,
    output_root: Path,
    poll: Any,
    records: list[OwnedProcess],
) -> None:
    if phase in record.captured:
        return
    record.captured.add(phase)
    try:
        state = subprocess.run(
            ["ps", "-o", "pid,ppid,state,command", "-p", str(record.process.pid)],
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )
        process_state = state.stdout + state.stderr
    except (OSError, subprocess.TimeoutExpired) as error:
        process_state = f"<process-state unavailable: {error}>\n"
    safe_phase = phase.replace("/", "_")
    observation = output_root / f"{record.name}-{safe_phase}.txt"
    log_sections = []
    for owned in records:
        log_sections.extend(
            (
                f"{owned.name}-log-path={owned.log_path}",
                f"{owned.name}-log-tail:",
                _log_tail(owned.log_path),
            )
        )
    try:
        observation.write_text(
            "\n".join(
                (
                    f"phase={phase}",
                    f"pid={record.process.pid}",
                    f"returncode={poll()}",
                    f"command={record.process.args!r}",
                    "process-state:",
                    process_state.rstrip(),
                    *log_sections,
                    "",
                )
            ),
            encoding="utf-8",
        )
    except OSError as error:
        print(f"failed to retain {phase} process evidence: {error}", file=sys.stderr)


def _instrumented_popen_factory(
    output_root: Path,
    records: list[OwnedProcess],
    original_popen: Any,
) -> Any:
    def instrumented_popen(*args: Any, **kwargs: Any) -> subprocess.Popen[Any]:
        process = original_popen(*args, **kwargs)
        name = _process_kind(process.args)
        if name is None:
            return process

        log_path = _process_log_path(kwargs.get("stdout"))
        record = OwnedProcess(name, process, log_path)
        records.append(record)
        original_poll = process.poll
        original_wait = process.wait
        original_terminate = process.terminate

        def capture(phase: str) -> None:
            if name == "client":
                _record_process_state(record, phase, output_root, original_poll, records)

        def poll() -> int | None:
            returncode = original_poll()
            if name == "client" and returncode is not None:
                capture("exit-observed")
            return returncode

        def wait(timeout: float | None = None) -> int:
            try:
                returncode = original_wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                capture("wait-timeout")
                raise
            if name == "client":
                capture("after-wait")
            return returncode

        def terminate() -> None:
            if original_poll() is None:
                capture("before-sigterm")
            original_terminate()

        process.poll = poll  # type: ignore[method-assign]
        process.wait = wait  # type: ignore[method-assign]
        process.terminate = terminate  # type: ignore[method-assign]
        return process

    return instrumented_popen


def _cleanup_owned_processes(records: list[OwnedProcess], evidence_dir: Path) -> bool:
    success = True
    for record in reversed(records):
        process = record.process
        try:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=WAIT_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            if record.name == "client":
                _record_process_state(
                    record,
                    "cleanup-wait-timeout",
                    evidence_dir,
                    process.poll,
                    records,
                )
            try:
                process.kill()
                process.wait(timeout=WAIT_TIMEOUT_SECONDS)
            except (OSError, subprocess.TimeoutExpired) as error:
                print(f"failed to reap {record.name} pid {process.pid}: {error}", file=sys.stderr)
                success = False
        except OSError as error:
            print(f"failed to clean up {record.name} pid {process.pid}: {error}", file=sys.stderr)
            success = False
    return success


def run_client_launch_case(fixture: Path, binary: Path, evidence_dir: Path) -> int:
    """Run only the fixture's original delayed-server case and preserve its result."""
    fixture = fixture.resolve()
    binary = binary.resolve()
    evidence_dir.mkdir(parents=True, exist_ok=True)
    if not fixture.is_file() or not binary.is_file():
        print("pinned client fixture or mc_main binary is missing", file=sys.stderr)
        return 2

    previous_argv = sys.argv
    sys.argv = [str(fixture), str(binary)]
    records: list[OwnedProcess] = []
    original_popen = subprocess.Popen
    suite_result: unittest.TestResult | None = None
    suite_output = io.StringIO()
    cleanup_success = True
    try:
        fixture_globals = runpy.run_path(str(fixture), run_name="_pinned_client_launch_test")
        test_case = fixture_globals["ClientLaunchTest"](TEST_NAME)
        subprocess.Popen = _instrumented_popen_factory(evidence_dir, records, original_popen)  # type: ignore[assignment]
        print(f"Running original pinned case: ClientLaunchTest.{TEST_NAME}")
        suite_result = unittest.TextTestRunner(stream=suite_output, verbosity=2).run(
            unittest.TestSuite([test_case])
        )
    finally:
        try:
            cleanup_success = _cleanup_owned_processes(records, evidence_dir)
        finally:
            subprocess.Popen = original_popen  # type: ignore[assignment]
            sys.argv = previous_argv

    if suite_result is None or not suite_result.wasSuccessful() or not cleanup_success:
        result_path = evidence_dir / "fixture-result.txt"
        result_bytes = suite_output.getvalue().encode("utf-8", errors="replace")
        if len(result_bytes) > MAX_LOG_BYTES:
            marker = b"<truncated; showing last 4096 bytes>\n"
            result_bytes = marker + result_bytes[-(MAX_LOG_BYTES - len(marker)):]
        try:
            result_path.write_bytes(result_bytes)
        except OSError as error:
            print(f"failed to retain pinned fixture result: {error}", file=sys.stderr)
        print(f"pinned client case or cleanup failed; details: {result_path}", file=sys.stderr)
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    arguments = parser.parse_args(argv)
    return run_client_launch_case(arguments.fixture, arguments.binary, arguments.evidence_dir)


if __name__ == "__main__":
    raise SystemExit(main())
