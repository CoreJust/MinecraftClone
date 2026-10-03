#!/usr/bin/env python3
"""Run the bounded noninteractive renderer acceptance test for release gates."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


DEFAULT_PRESET = "renderer-smoke"
DEFAULT_TIMEOUT = 840
CONFIGURE_TIMEOUT = 600
BUILD_TIMEOUT = 600
TEST_TIMEOUT = 90
RECEIPT_PATH = Path("build/ai-checks/renderer-smoke.log")
CONFIGURE_STAMP_NAME = ".ai-renderer-smoke-configured.json"


@dataclass
class CommandResult:
    command: Sequence[str]
    returncode: int
    output: str
    timed_out: bool = False


def repository_root() -> Path:
    return Path(__file__).resolve().parents[1]


def cache_path(root: Path, preset: str) -> Path:
    return root / "build" / preset / "CMakeCache.txt"


def cache_value(cache: Path, key: str) -> str | None:
    if not cache.is_file():
        return None
    prefix = f"{key}:"
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(prefix) and "=" in line:
            return line.partition("=")[2]
    return None


def _matching_debug_cache(root: Path) -> Path | None:
    cache = cache_path(root, "debug")
    source_directory = cache_value(cache, "CMAKE_HOME_DIRECTORY")
    if source_directory is None or Path(source_directory).resolve() != root:
        return None
    return cache


def _toolchain_for_root(root: Path) -> Path | None:
    toolchain = root / "scripts/buildsystems/vcpkg.cmake"
    return toolchain.resolve() if toolchain.is_file() else None


def _toolchain_from_cache(cache: Path | None) -> Path | None:
    if cache is None:
        return None
    cached_root = cache_value(cache, "Z_VCPKG_ROOT_DIR")
    toolchain = _toolchain_for_root(Path(cached_root).expanduser()) if cached_root else None
    if toolchain is not None:
        return toolchain
    cached_toolchain = cache_value(cache, "CMAKE_TOOLCHAIN_FILE")
    candidate = Path(cached_toolchain).expanduser() if cached_toolchain else None
    return candidate.resolve() if candidate is not None and candidate.is_file() else None


def renderer_configure_command(root: Path, preset: str) -> tuple[list[str] | None, str | None]:
    debug_cache = _matching_debug_cache(root)
    cached_toolchain = _toolchain_from_cache(debug_cache)
    toolchain = cached_toolchain
    configured_root = os.environ.get("VCPKG_ROOT")
    if toolchain is None and configured_root:
        toolchain = _toolchain_for_root(Path(configured_root).expanduser())
        if toolchain is None:
            return None, f"VCPKG_ROOT does not contain scripts/buildsystems/vcpkg.cmake: {configured_root}"
    if toolchain is None:
        return None, "VCPKG_ROOT is unset and no matching build/debug cache provides a valid vcpkg toolchain"

    command = ["cmake", "--preset", preset, f"-DCMAKE_TOOLCHAIN_FILE={toolchain}"]
    if debug_cache is None or cached_toolchain is None:
        return command, None

    manifest_install = cache_value(debug_cache, "VCPKG_MANIFEST_INSTALL")
    installed_dir = cache_value(debug_cache, "VCPKG_INSTALLED_DIR")
    target_triplet = cache_value(debug_cache, "VCPKG_TARGET_TRIPLET")
    prefix = cache_value(debug_cache, "CMAKE_PREFIX_PATH")
    corecpp_dir = cache_value(debug_cache, "CoreCpp_DIR")
    coreproject_dir = cache_value(debug_cache, "CoreProject2026_DIR")
    prefix_paths = [Path(item).expanduser() for item in (prefix or "").split(";") if item]
    cached_packages_are_ready = (
        manifest_install == "OFF"
        and installed_dir is not None
        and target_triplet is not None
        and (Path(installed_dir).expanduser() / target_triplet).is_dir()
        and bool(prefix_paths)
        and all(path.is_dir() for path in prefix_paths)
        and corecpp_dir is not None
        and (Path(corecpp_dir).expanduser() / "CoreCppConfig.cmake").is_file()
        and coreproject_dir is not None
        and (Path(coreproject_dir).expanduser() / "CoreProject2026Config.cmake").is_file()
    )
    if cached_packages_are_ready:
        command.extend(
            [
                "-DVCPKG_MANIFEST_INSTALL=OFF",
                f"-DVCPKG_INSTALLED_DIR={Path(installed_dir).expanduser()}",
                f"-DVCPKG_TARGET_TRIPLET={target_triplet}",
                f"-DCMAKE_PREFIX_PATH={prefix}",
                f"-DCoreCpp_DIR={Path(corecpp_dir).expanduser()}",
                f"-DCoreProject2026_DIR={Path(coreproject_dir).expanduser()}",
            ]
        )
    return command, None


def _configure_stamp_path(root: Path, preset: str) -> Path:
    return root / "build" / preset / CONFIGURE_STAMP_NAME


def _cache_digest(cache: Path) -> str:
    return hashlib.sha256(cache.read_bytes()).hexdigest()


def _configure_stamp_payload(cache: Path, command: Sequence[str]) -> dict[str, object]:
    return {"command": list(command), "cache_sha256": _cache_digest(cache)}


def _write_configure_stamp(root: Path, preset: str, command: Sequence[str]) -> None:
    cache = cache_path(root, preset)
    if not cache.is_file() or not (cache.parent / "build.ninja").is_file():
        return
    stamp = _configure_stamp_path(root, preset)
    stamp.write_text(
        json.dumps(_configure_stamp_payload(cache, command), sort_keys=True) + "\n",
        encoding="utf-8",
    )


def smoke_cache_is_usable(root: Path, preset: str, command: Sequence[str]) -> bool:
    cache = cache_path(root, preset)
    source_directory = cache_value(cache, "CMAKE_HOME_DIRECTORY")
    if source_directory is None:
        return False
    if Path(source_directory).resolve() != root:
        raise RuntimeError(
            f"{cache} belongs to {source_directory}, not this checkout; refusing to reuse it"
        )
    if cache_value(cache, "MC_ENABLE_RENDERER_SMOKE") != "ON":
        return False
    if cache_value(cache, "CMAKE_GENERATOR") != "Ninja" or not (cache.parent / "build.ninja").is_file():
        return False
    stamp = _configure_stamp_path(root, preset)
    try:
        stored = json.loads(stamp.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False
    return stored == _configure_stamp_payload(cache, command)


def run_command(root: Path, command: Sequence[str], timeout: int) -> CommandResult:
    started_at = time.monotonic()
    try:
        with tempfile.TemporaryFile(mode="w+t", encoding="utf-8") as output_file:
            process = subprocess.Popen(
                command,
                cwd=root,
                text=True,
                stdout=output_file,
                stderr=subprocess.STDOUT,
            )
            timed_out = False
            while True:
                remaining = timeout - (time.monotonic() - started_at)
                try:
                    returncode = process.wait(timeout=max(0.0, min(30.0, remaining)))
                    break
                except subprocess.TimeoutExpired:
                    elapsed = time.monotonic() - started_at
                    if elapsed >= timeout:
                        process.kill()
                        process.wait()
                        returncode = 124
                        timed_out = True
                        print(f"renderer-smoke: command timed out after {elapsed:.0f}s", flush=True)
                        break
                    print(
                        f"renderer-smoke: command still running after {elapsed:.0f}s "
                        f"(timeout {timeout}s)",
                        flush=True,
                    )
            output_file.flush()
            output_file.seek(0)
            output = output_file.read()
        return CommandResult(command, returncode, output, timed_out=timed_out)
    except OSError as error:
        return CommandResult(command, 127, str(error))


def write_receipt(receipt: Path, results: Sequence[CommandResult], failure: str | None) -> None:
    receipt.parent.mkdir(parents=True, exist_ok=True)
    lines = []
    for result in results:
        lines.append("$ " + " ".join(result.command))
        lines.append(result.output.rstrip())
        lines.append(f"exit {result.returncode}{' (timeout)' if result.timed_out else ''}")
        lines.append("")
    if failure:
        lines.append(f"FAIL: {failure}")
    receipt.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_smoke(root: Path, preset: str, timeout: int) -> tuple[int, list[CommandResult], str | None]:
    started_at = time.monotonic()
    results: list[CommandResult] = []

    def run_stage(command: Sequence[str], stage_timeout: int) -> CommandResult | None:
        remaining = timeout - int(time.monotonic() - started_at)
        if remaining <= 0:
            return None
        result = run_command(root, command, min(stage_timeout, remaining))
        results.append(result)
        return result

    command, configure_error = renderer_configure_command(root, preset)
    if configure_error:
        return 1, results, configure_error
    assert command is not None
    try:
        configured = smoke_cache_is_usable(root, preset, command)
    except RuntimeError as error:
        return 1, results, str(error)
    if not configured:
        _configure_stamp_path(root, preset).unlink(missing_ok=True)
        result = run_stage(
            command,
            CONFIGURE_TIMEOUT,
        )
        if result is None:
            return 1, results, "renderer smoke gate timed out before configuration"
        if result.returncode:
            return 1, results, "renderer smoke configuration failed"
        _write_configure_stamp(root, preset, command)

    result = run_stage(
        ["cmake", "--build", "--preset", preset, "--target", "mc_renderer_smoke"],
        BUILD_TIMEOUT,
    )
    if result is None:
        return 1, results, "renderer smoke gate timed out before target build"
    if result.returncode:
        return 1, results, "renderer smoke target build failed"

    result = run_stage(
        [
            "ctest",
            "--preset",
            preset,
            "-R",
            "^RendererSmokeTest$",
            "--output-on-failure",
            "--no-tests=error",
        ],
        TEST_TIMEOUT,
    )
    if result is None:
        return 1, results, "renderer smoke gate timed out before CTest"
    if result.returncode:
        return 1, results, "RendererSmokeTest failed, timed out, was not registered, or reported validation errors"
    return 0, results, None


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preset", default=DEFAULT_PRESET)
    parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT)
    parser.add_argument("--root", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    root = (args.root or repository_root()).resolve()
    receipt = root / RECEIPT_PATH
    result, commands, failure = run_smoke(root, args.preset, args.timeout)
    write_receipt(receipt, commands, failure)
    if failure:
        print(f"FAIL renderer-smoke: {failure}; receipt {RECEIPT_PATH}")
    else:
        print(f"PASS renderer-smoke: receipt {RECEIPT_PATH}")
    return result


if __name__ == "__main__":
    raise SystemExit(main())
