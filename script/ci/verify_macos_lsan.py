#!/usr/bin/env python3
"""Verify macOS LSan suppressions against application leaks and report CTest diagnostics."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any, Mapping


ROOT = Path(__file__).resolve().parents[2]
SUPPRESSION_FILE = ROOT / "script/ci/macos_lsan.supp"
FIXTURE_SOURCE = ROOT / "script/ci/macos_lsan_control.cpp"
EXPECTED_SUPPRESSIONS = (
    "leak:AMCP::Utility::Dispatch_Queue::install_mig_server",
    "leak:AutoreleasePoolPage::autoreleaseNoPage",
    "leak:__CFTSDGetTable",
)
EXPECTED_LEAK = re.compile(
    r"Direct leak of 4096 byte\(s\) in 1 object\(s\) allocated from:\n(?P<stack>.*?)(?=\n\s*\n|\nSUMMARY:)",
    re.DOTALL,
)
TEST_HEADER = re.compile(r"^\d+/\d+ Test: (.+)$", re.MULTILINE)
SUPPRESSION_ENTRY = re.compile(r"^\s*(\d+)\s+(\d+)\s+(.+?)\s*$", re.MULTILINE)
LEAK_SUMMARY = re.compile(r"^SUMMARY: AddressSanitizer: (.+)$", re.MULTILINE)


class VerificationError(RuntimeError):
    """Raised when the macOS LSan row does not prove its required contracts."""


def read_suppressions(path: Path) -> tuple[str, ...]:
    try:
        entries = tuple(line.strip() for line in path.read_text(encoding="utf-8").splitlines() if line.strip())
    except OSError as error:
        raise VerificationError(f"cannot read LSan suppressions at {path}: {error}") from error
    if entries != EXPECTED_SUPPRESSIONS:
        raise VerificationError(
            "macOS LSan suppression file must contain exactly the three reviewed allocation sites"
        )
    return entries


def validate_sanitizer_environment(environment: Mapping[str, str], suppression_path: Path) -> None:
    asan_options = environment.get("ASAN_OPTIONS", "").split(":")
    if "detect_leaks=1" not in asan_options:
        raise VerificationError("ASAN_OPTIONS must retain detect_leaks=1")

    lsan_options = {}
    for option in environment.get("LSAN_OPTIONS", "").split(":"):
        key, separator, value = option.partition("=")
        if separator:
            lsan_options[key] = value
    configured_suppressions = Path(lsan_options.get("suppressions", "")).resolve()
    if configured_suppressions != suppression_path.resolve():
        raise VerificationError("LSAN_OPTIONS must point to the exact row suppression file")
    if lsan_options.get("print_suppressions") != "1":
        raise VerificationError("LSAN_OPTIONS must retain print_suppressions=1")


def suppression_hits(output: str) -> dict[str, dict[str, int]]:
    totals: dict[str, dict[str, int]] = defaultdict(lambda: {"count": 0, "bytes": 0})
    for table in re.findall(
        r"Suppressions used:\s*\n\s*count\s+bytes\s+template\s*\n(.*?)(?=\n[-]{5,}|\Z)",
        output,
        re.DOTALL,
    ):
        for count, size, template in SUPPRESSION_ENTRY.findall(table):
            totals[template]["count"] += int(count)
            totals[template]["bytes"] += int(size)
    return dict(sorted(totals.items()))


def application_leak_block(output: str, required_frames: tuple[str, ...]) -> str:
    for match in EXPECTED_LEAK.finditer(output):
        block = match.group(0)
        if all(frame in block for frame in required_frames):
            return block
    frames = ", ".join(required_frames)
    raise VerificationError(f"no 4096-byte application leak report contained the required frames: {frames}")


def verify_clean_control(return_code: int, output: str) -> dict[str, Any]:
    if return_code != 0:
        raise VerificationError(f"clean control exited {return_code}: {output}")
    if "ERROR: LeakSanitizer:" in output or LEAK_SUMMARY.search(output):
        raise VerificationError(f"clean control reported a leak: {output}")
    return {"return_code": return_code, "output_sha256": hashlib.sha256(output.encode()).hexdigest()}


def verify_leak_control(
    return_code: int,
    output: str,
    required_frames: tuple[str, ...],
) -> dict[str, Any]:
    if return_code == 0:
        raise VerificationError("intentional application leak unexpectedly exited successfully")
    block = application_leak_block(output, required_frames)
    return {
        "return_code": return_code,
        "bytes": 4096,
        "required_frames": list(required_frames),
        "application_leak_report": block,
        "suppression_hits": suppression_hits(output),
        "unsuppressed_summary_lines": LEAK_SUMMARY.findall(output),
        "output_sha256": hashlib.sha256(output.encode()).hexdigest(),
    }


def verify_audio_callback_control(return_code: int, output: str) -> dict[str, Any]:
    if "audio callback observed: yes" not in output:
        raise VerificationError(f"real AudioUnit callback was not observed: {output}")
    if "audio callback succeeded: yes" not in output:
        raise VerificationError(f"Audio callback did not succeed: {output}")

    setup_operations = (
        "AudioComponentInstanceNew",
        "AudioUnitSetProperty",
        "AudioUnitInitialize",
        "AudioOutputUnitStart",
    )
    teardown_operations = (
        "AudioOutputUnitStop",
        "AudioUnitUninitialize",
        "AudioComponentInstanceDispose",
    )
    for operation in setup_operations + teardown_operations:
        if not re.search(
            rf"^{re.escape(operation)}: OSStatus=0 \(success\)$",
            output,
            re.MULTILINE,
        ):
            raise VerificationError(f"AudioUnit control did not report successful {operation}")
    result = verify_leak_control(return_code, output, ("createApplicationLeak", "renderAndLeak"))
    result["callback_observed"] = True
    result["callback_succeeded"] = True
    result["setup_statuses"] = {operation: "OSStatus=0 (success)" for operation in setup_operations}
    result["teardown_statuses"] = {
        operation: "OSStatus=0 (success)" for operation in teardown_operations
    }
    return result


def _run(
    command: list[str],
    *,
    cwd: Path,
    environment: Mapping[str, str],
    timeout_seconds: int,
) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            command,
            cwd=cwd,
            env=dict(environment),
            text=True,
            capture_output=True,
            timeout=timeout_seconds,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise VerificationError(f"command failed or exceeded {timeout_seconds}s: {command!r}: {error}") from error


def run_controls(build_dir: Path) -> dict[str, Any]:
    if sys.platform != "darwin":
        raise VerificationError("macOS LSan controls require a Darwin host")
    suppressions = read_suppressions(SUPPRESSION_FILE)
    environment = os.environ.copy()
    validate_sanitizer_environment(environment, SUPPRESSION_FILE)

    compiler = shutil.which("clang++")
    if compiler is None:
        raise VerificationError("clang++ is not available on PATH")
    build_dir.mkdir(parents=True, exist_ok=True)
    binary = build_dir / "macos_lsan_control"
    compile_command = [
        compiler,
        "-std=c++23",
        "-g",
        "-O0",
        "-fsanitize=address",
        "-fno-omit-frame-pointer",
        "-fno-optimize-sibling-calls",
        str(FIXTURE_SOURCE),
        "-framework",
        "AudioToolbox",
        "-framework",
        "CoreAudio",
        "-framework",
        "CoreFoundation",
        "-o",
        str(binary),
    ]
    compiled = _run(compile_command, cwd=ROOT, environment=environment, timeout_seconds=120)
    if compiled.returncode:
        raise VerificationError(
            f"control fixture compilation exited {compiled.returncode}: {compiled.stdout}{compiled.stderr}"
        )

    controls: dict[str, Any] = {}
    for mode, timeout in (("clean", 15), ("ordinary-leak", 15), ("audio-callback-leak", 20)):
        completed = _run([str(binary), mode], cwd=ROOT, environment=environment, timeout_seconds=timeout)
        output = completed.stdout + completed.stderr
        log_path = build_dir / f"{mode}.log"
        log_path.write_text(output, encoding="utf-8")
        if mode == "clean":
            result = verify_clean_control(completed.returncode, output)
        elif mode == "ordinary-leak":
            result = verify_leak_control(completed.returncode, output, ("createApplicationLeak", "main"))
        else:
            result = verify_audio_callback_control(completed.returncode, output)
        result["log"] = str(log_path.relative_to(ROOT))
        controls[mode] = result

    receipt = {
        "schema_version": 1,
        "compiler": compiler,
        "suppression_file": str(SUPPRESSION_FILE.relative_to(ROOT)),
        "suppression_file_sha256": hashlib.sha256(SUPPRESSION_FILE.read_bytes()).hexdigest(),
        "suppression_rules": list(suppressions),
        "controls": controls,
    }
    receipt_path = build_dir / "control-receipt.json"
    receipt_path.write_text(json.dumps(receipt, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(receipt, sort_keys=True, separators=(",", ":")))
    return receipt


def summarize_last_test_log(path: Path) -> dict[str, Any]:
    try:
        raw = path.read_bytes()
        text = raw.decode("utf-8", errors="replace").replace("\r\n", "\n")
    except OSError as error:
        raise VerificationError(f"cannot read CTest LastTest.log at {path}: {error}") from error
    headers = list(TEST_HEADER.finditer(text))
    if not headers:
        raise VerificationError("CTest LastTest.log contains no test sections")

    totals: dict[str, dict[str, int]] = defaultdict(lambda: {"count": 0, "bytes": 0})
    unsuppressed: list[dict[str, str]] = []
    for index, header in enumerate(headers):
        end = headers[index + 1].start() if index + 1 < len(headers) else len(text)
        section = text[header.start() : end]
        for count, size, template in SUPPRESSION_ENTRY.findall(
            "\n".join(
                match.group(1)
                for match in re.finditer(
                    r"Suppressions used:\s*\n\s*count\s+bytes\s+template\s*\n(.*?)(?=\n[-]{5,}|\Z)",
                    section,
                    re.DOTALL,
                )
            )
        ):
            totals[template]["count"] += int(count)
            totals[template]["bytes"] += int(size)
        summary = LEAK_SUMMARY.search(section)
        if summary and not re.search(r"\b0 bytes leaked in 0 allocations?", summary.group(1)):
            diagnostics_start = section.find("ERROR: LeakSanitizer:")
            diagnostics = section[diagnostics_start : summary.end()] if diagnostics_start >= 0 else summary.group(0)
            unsuppressed.append(
                {
                    "test": header.group(1),
                    "summary": summary.group(0),
                    "diagnostics": diagnostics.strip(),
                }
            )

    for pattern in EXPECTED_SUPPRESSIONS:
        template = pattern.removeprefix("leak:")
        totals.setdefault(template, {"count": 0, "bytes": 0})
    report = {
        "schema_version": 1,
        "last_test_log": str(path),
        "last_test_log_sha256": hashlib.sha256(raw).hexdigest(),
        "last_test_log_bytes": len(raw),
        "tests": len(headers),
        "suppression_hits": dict(sorted(totals.items())),
        "unsuppressed_leak_reports": unsuppressed,
    }
    print(json.dumps(report, sort_keys=True, separators=(",", ":")))
    if unsuppressed:
        return {**report, "status": "failed"}
    return {**report, "status": "passed"}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    controls = commands.add_parser("controls")
    controls.add_argument("--build-dir", type=Path, required=True)
    summarize = commands.add_parser("summarize")
    summarize.add_argument("--last-test-log", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "controls":
            run_controls((ROOT / args.build_dir).resolve() if not args.build_dir.is_absolute() else args.build_dir)
            return 0
        result = summarize_last_test_log(
            (ROOT / args.last_test_log).resolve()
            if not args.last_test_log.is_absolute()
            else args.last_test_log
        )
        return 1 if result["status"] == "failed" else 0
    except VerificationError as error:
        print(f"macOS LSan verification failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
