#!/usr/bin/env python3
"""Build and prove a pinned MemorySanitizer-instrumented libc++ toolchain."""

from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Sequence


LLVM_REPOSITORY = "https://github.com/llvm/llvm-project.git"
LLVM_TAG = "llvmorg-18.1.3"
LLVM_COMMIT = "c13b7485b87909fcf739f62cfa382b55407433c0"


class ToolchainError(RuntimeError):
    """The pinned MSan compiler or standard library could not be established."""


def run(command: Sequence[str], *, cwd: Path | None = None, env: dict[str, str] | None = None) -> str:
    rendered = " ".join(command)
    print(f"[msan-libcxx] $ {rendered}", flush=True)
    completed = subprocess.run(
        list(command),
        cwd=cwd,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )
    output = completed.stdout + completed.stderr
    if output:
        print(output, end="" if output.endswith("\n") else "\n", flush=True)
    if completed.returncode:
        raise ToolchainError(f"command exited {completed.returncode}: {rendered}")
    return output


def require_linux() -> None:
    if platform.system() != "Linux":
        raise ToolchainError("the instrumented libc++ setup is supported only on Linux")


def resolve_tool(value: str, name: str) -> str:
    resolved = shutil.which(value)
    if resolved is None:
        raise ToolchainError(f"required {name} compiler is not on PATH: {value}")
    # Clang uses argv[0] to distinguish clang from clang++; resolving symlinks erases that identity.
    return os.path.abspath(resolved)


def require_compatible_clang(output: str, compiler: str) -> None:
    source_match = re.match(r"llvmorg-(\d+)\.", LLVM_TAG)
    compiler_match = re.search(r"clang version (\d+)\.", output, re.IGNORECASE)
    if source_match is None:
        raise ToolchainError(f"cannot determine the pinned LLVM major version from {LLVM_TAG}")
    if compiler_match is None:
        raise ToolchainError(f"selected compiler did not identify itself as Clang: {compiler}")
    if compiler_match.group(1) != source_match.group(1):
        raise ToolchainError(
            f"Clang/LLVM major version mismatch for {compiler}: "
            f"Clang {compiler_match.group(1)}, LLVM {source_match.group(1)}"
        )


def configure_command(source: Path, build: Path, prefix: Path, clang: str, clangxx: str) -> list[str]:
    return [
        "cmake",
        "-G",
        "Ninja",
        "-S",
        str(source / "runtimes"),
        "-B",
        str(build),
        f"-DCMAKE_C_COMPILER={clang}",
        f"-DCMAKE_CXX_COMPILER={clangxx}",
        f"-DCMAKE_INSTALL_PREFIX={prefix}",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi;libunwind",
        "-DLLVM_USE_SANITIZER=MemoryWithOrigins",
        "-DLIBCXX_USE_COMPILER_RT=ON",
        "-DLIBCXXABI_USE_COMPILER_RT=ON",
        "-DLIBUNWIND_USE_COMPILER_RT=ON",
        "-DLIBCXX_INCLUDE_TESTS=OFF",
    ]


def build_command(build: Path, jobs: int) -> list[str]:
    return [
        "cmake",
        "--build",
        str(build),
        "--target",
        "cxx",
        "cxx_experimental",
        "cxxabi",
        "unwind",
        "--parallel",
        str(jobs),
    ]


def verify_install(prefix: Path, clangxx: str, build: Path) -> None:
    include_dir = prefix / "include" / "c++" / "v1"
    if not include_dir.is_dir():
        raise ToolchainError(f"instrumented libc++ headers are missing: {include_dir}")
    shared_libraries = sorted(prefix.rglob("libc++.so"))
    if not shared_libraries:
        raise ToolchainError(f"instrumented libc++ shared library is missing under {prefix}")
    library_dir = shared_libraries[0].parent
    runtime_environment = os.environ.copy()
    existing_library_path = runtime_environment.get("LD_LIBRARY_PATH", "")
    runtime_environment["LD_LIBRARY_PATH"] = os.pathsep.join(
        item for item in (str(library_dir), existing_library_path) if item
    )
    runtime_environment["MSAN_OPTIONS"] = "halt_on_error=1:exit_code=86:print_stats=0"
    positive_probe = build / "msan-libcxx-positive"
    negative_probe = build / "msan-uninitialized-read"
    common_flags = [
        "-O1",
        "-g",
        "-fsanitize=memory",
        "-fsanitize-memory-track-origins=2",
        "-fno-omit-frame-pointer",
        "-stdlib=libc++",
        f"-I{include_dir}",
        f"-L{library_dir}",
        f"-Wl,-rpath,{library_dir}",
    ]
    positive_source = "#include <string>\nint main() { return std::string(\"msan\").empty(); }\n"
    positive_command = [clangxx, *common_flags, "-x", "c++", "-", "-o", str(positive_probe)]
    positive_build = subprocess.run(
        positive_command,
        input=positive_source,
        cwd=build,
        env=runtime_environment,
        text=True,
        capture_output=True,
        check=False,
    )
    if positive_build.returncode:
        raise ToolchainError(
            "instrumented libc++ positive-probe compilation failed: "
            + (positive_build.stdout + positive_build.stderr).strip()
        )
    positive_run = subprocess.run(
        [str(positive_probe)],
        cwd=build,
        env=runtime_environment,
        text=True,
        capture_output=True,
        timeout=30,
        check=False,
    )
    if positive_run.returncode:
        raise ToolchainError(
            "instrumented libc++ positive smoke test failed: "
            + (positive_run.stdout + positive_run.stderr).strip()
        )

    negative_source = "int main() { volatile int value; return value; }\n"
    negative_command = [clangxx, *common_flags, "-x", "c++", "-", "-o", str(negative_probe)]
    negative_build = subprocess.run(
        negative_command,
        cwd=build,
        env=runtime_environment,
        input=negative_source,
        text=True,
        capture_output=True,
        check=False,
    )
    if negative_build.returncode:
        raise ToolchainError(
            "MSan negative-probe compilation failed: "
            + (negative_build.stdout + negative_build.stderr).strip()
        )
    negative_run = subprocess.run(
        [str(negative_probe)],
        cwd=build,
        env=runtime_environment,
        text=True,
        capture_output=True,
        timeout=30,
        check=False,
    )
    diagnostics = negative_run.stdout + negative_run.stderr
    if negative_run.returncode != 86 or "use-of-uninitialized-value" not in diagnostics:
        raise ToolchainError(
            "MSan did not diagnose the deliberate uninitialized read: "
            + diagnostics.strip()
        )
    print("[msan-libcxx] PASS: libc++ executes and MSan catches an uninitialized read", flush=True)


def write_github_environment(values: dict[str, str]) -> None:
    destination = os.environ.get("GITHUB_ENV")
    if not destination:
        raise ToolchainError("GITHUB_ENV is required to export the verified MSan toolchain")
    with Path(destination).open("a", encoding="utf-8") as environment_file:
        for name, value in values.items():
            if "\n" in value or "\r" in value:
                raise ToolchainError(f"refusing a multiline GitHub environment value for {name}")
            environment_file.write(f"{name}={value}\n")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--clang", default=os.environ.get("CC", "clang"))
    parser.add_argument("--clangxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    args = parser.parse_args(argv)
    try:
        require_linux()
        if args.jobs < 1:
            raise ToolchainError("--jobs must be positive")
        for name, directory in (
            ("source", args.source_root),
            ("build", args.build_root),
            ("install prefix", args.prefix),
        ):
            if directory.exists() or directory.is_symlink():
                raise ToolchainError(f"refusing to reuse existing {name} directory: {directory}")
        clang = resolve_tool(args.clang, "Clang C")
        clangxx = resolve_tool(args.clangxx, "Clang C++")
        require_compatible_clang(run([clang, "--version"]), clang)
        require_compatible_clang(run([clangxx, "--version"]), clangxx)

        args.source_root.parent.mkdir(parents=True, exist_ok=True)
        args.build_root.parent.mkdir(parents=True, exist_ok=True)
        args.prefix.parent.mkdir(parents=True, exist_ok=True)
        print(f"[msan-libcxx] Phase 1/4: checkout LLVM {LLVM_TAG} at {LLVM_COMMIT}", flush=True)
        run(["git", "init", str(args.source_root)])
        run(["git", "-C", str(args.source_root), "remote", "add", "origin", LLVM_REPOSITORY])
        run(["git", "-C", str(args.source_root), "fetch", "--depth=1", "origin", f"refs/tags/{LLVM_TAG}"])
        run(["git", "-C", str(args.source_root), "checkout", "--detach", "FETCH_HEAD"])
        actual_commit = run(["git", "-C", str(args.source_root), "rev-parse", "HEAD"]).strip()
        if actual_commit != LLVM_COMMIT:
            raise ToolchainError(f"LLVM source pin mismatch: expected {LLVM_COMMIT}, got {actual_commit}")

        print("[msan-libcxx] Phase 2/4: configure instrumented libc++, libc++abi, and libunwind", flush=True)
        run(configure_command(args.source_root, args.build_root, args.prefix, clang, clangxx))
        print(f"[msan-libcxx] Phase 3/4: build and install runtimes using {args.jobs} jobs", flush=True)
        run(build_command(args.build_root, args.jobs))
        run(["cmake", "--install", str(args.build_root)])

        print("[msan-libcxx] Phase 4/4: verify linked libc++ and an expected MSan diagnostic", flush=True)
        verify_install(args.prefix, clangxx, args.build_root)
        write_github_environment(
            {
                "CC": clang,
                "CXX": clangxx,
                "MC_LLVM_PROJECT_ROOT": str(args.source_root),
                "MC_MSAN_LIBCXX_PREFIX": str(args.prefix),
            }
        )
        return 0
    except (OSError, subprocess.SubprocessError, ToolchainError) as error:
        print(f"FAIL MSan libc++ setup: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
