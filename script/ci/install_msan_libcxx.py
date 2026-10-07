#!/usr/bin/env python3
"""Build and prove a pinned MemorySanitizer-instrumented libc++ toolchain."""

from __future__ import annotations

import argparse
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path, PurePosixPath
from typing import Sequence

try:
    import resource
except ImportError:  # The installer is Linux-only; keeping the module importable aids cross-platform checks.
    resource = None


LLVM_REPOSITORY = "https://github.com/llvm/llvm-project.git"
LLVM_TAG = "llvmorg-18.1.3"
LLVM_COMMIT = "c13b7485b87909fcf739f62cfa382b55407433c0"
LIBUNWIND_MSAN_PATCH = Path(__file__).resolve().parent / "patches/libunwind-msan-x86_64.patch"
LIBUNWIND_MSAN_PATCH_SHA256 = "ef6acdebdff1cc7bf491b453e3f62fff9e3268d7da98f37a72abc6ab843d4b14"
MSAN_EXIT_CODE = 86
MSAN_ANALYSIS_OPTIONS = "halt_on_error=1:print_stats=1:fast_unwind_on_fatal=1"
MSAN_PROBE_OPTIONS = "halt_on_error=1:exit_code=86:print_stats=0:symbolize=1:fast_unwind_on_fatal=1"
MSAN_WARNING = re.compile(
    r"(?m)^(?:==[0-9]+==)?WARNING: MemorySanitizer: use-of-uninitialized-value\r?$"
)
MSAN_DIAGNOSTIC = re.compile(
    r"(?m)^(?:==[0-9]+==)?(?:(?:WARNING|ERROR|SUMMARY): )?MemorySanitizer:"
)
MSAN_SYMBOLIZER_CANDIDATES = ("llvm-symbolizer-18", "llvm-symbolizer")
MSAN_POSITIVE_PROBE_SOURCE = (
    "#include <stdint.h>\n"
    "#include <string>\n"
    "volatile int32_t runtime_seed = 0;\n"
    "struct Cleanup {\n"
    "    int32_t* count;\n"
    "    ~Cleanup()\n"
    "    {\n"
    "        ++*count;\n"
    "    }\n"
    "};\n"
    "[[gnu::noinline]] void throw_value(int32_t value)\n"
    "{\n"
    "    throw value;\n"
    "}\n"
    "[[gnu::noinline]] void throw_with_cleanup(int32_t value, int32_t* count)\n"
    "{\n"
    "    Cleanup cleanup{count};\n"
    "    throw_value(value);\n"
    "}\n"
    "[[gnu::noinline]] void rethrow_value(int32_t value)\n"
    "{\n"
    "    try {\n"
    "        throw_value(value);\n"
    "    } catch (int32_t) {\n"
    "        throw;\n"
    "    }\n"
    "}\n"
    "int main(int argc, char**)\n"
    "{\n"
    "    const int32_t expected = static_cast<int32_t>(argc) + runtime_seed + 40;\n"
    "    const std::string marker = std::to_string(expected);\n"
    "    int32_t cleanup_count = 0;\n"
    "    int32_t caught = 0;\n"
    "    try {\n"
    "        throw_with_cleanup(expected, &cleanup_count);\n"
    "    } catch (int32_t value) {\n"
    "        caught = value;\n"
    "    }\n"
    "    if (caught != expected || cleanup_count != 1) {\n"
    "        return 1;\n"
    "    }\n"
    "    try {\n"
    "        rethrow_value(expected + 1);\n"
    "    } catch (int32_t value) {\n"
    "        caught = value;\n"
    "    }\n"
    "    return caught == expected + 1 && cleanup_count == 1 && !marker.empty() ? 0 : 2;\n"
    "}\n"
)


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


def format_probe_failure(
    stage: str, command: Sequence[str], completed: subprocess.CompletedProcess
) -> str:
    stdout = completed.stdout.strip() or "<empty>"
    stderr = completed.stderr.strip() or "<empty>"
    return (
        f"{stage} failed (return code: {completed.returncode})\n"
        f"command: {' '.join(command)}\n"
        f"stdout:\n{stdout}\n"
        f"stderr:\n{stderr}"
    )


def resolve_msan_symbolizer(clangxx: str) -> str | None:
    compiler_dir = Path(clangxx).parent
    candidates = [compiler_dir / name for name in MSAN_SYMBOLIZER_CANDIDATES]
    candidates.extend(
        Path(resolved)
        for name in MSAN_SYMBOLIZER_CANDIDATES
        if (resolved := shutil.which(name)) is not None
    )
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return os.path.abspath(candidate)
    return None


def sha256_file(file_path: Path) -> str:
    digest = hashlib.sha256()
    with file_path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def apply_libunwind_msan_patch(source: Path) -> None:
    try:
        actual_hash = sha256_file(LIBUNWIND_MSAN_PATCH)
    except OSError as error:
        raise ToolchainError(f"pinned libunwind MSan patch is missing or unreadable: {error}") from error
    if actual_hash != LIBUNWIND_MSAN_PATCH_SHA256:
        raise ToolchainError(
            "pinned libunwind MSan patch SHA-256 mismatch: "
            f"expected {LIBUNWIND_MSAN_PATCH_SHA256}, got {actual_hash}"
        )
    patch_path = str(LIBUNWIND_MSAN_PATCH)
    run(
        [
            "git",
            "-C",
            str(source),
            "apply",
            "--check",
            "--whitespace=error-all",
            patch_path,
        ]
    )
    run(
        [
            "git",
            "-C",
            str(source),
            "apply",
            "--whitespace=error-all",
            patch_path,
        ]
    )


def read_linux_setting(setting_path: str) -> str:
    try:
        return Path(setting_path).read_text(encoding="utf-8").strip()
    except OSError as error:
        return f"<unavailable: {error}>"


def format_limit(limit: int) -> str:
    if resource is None:
        return "<unavailable>"
    return "unlimited" if limit == resource.RLIM_INFINITY else str(limit)


def collect_probe_context(
    clangxx: str,
    probe: Path,
    library_dir: Path,
    runtime_environment: dict[str, str],
    symbolizer: str,
) -> tuple[str, dict[str, PurePosixPath]]:
    compiler = subprocess.run(
        [clangxx, "--version"], text=True, capture_output=True, check=False, timeout=10
    )
    dependencies = subprocess.run(
        ["ldd", str(probe)],
        env=runtime_environment,
        text=True,
        capture_output=True,
        check=False,
        timeout=10,
    )
    symbolizer_version = subprocess.run(
        [symbolizer, "--version"], text=True, capture_output=True, check=False, timeout=10
    )
    if resource is None:
        stack_limit = "soft=<unavailable> hard=<unavailable>"
    else:
        stack_soft, stack_hard = resource.getrlimit(resource.RLIMIT_STACK)
        stack_limit = f"soft={format_limit(stack_soft)} hard={format_limit(stack_hard)}"
    probe_hash = sha256_file(probe)
    resolved_compiler = Path(shutil.which(clangxx) or clangxx)
    compiler_hash = sha256_file(resolved_compiler) if resolved_compiler.is_file() else "<unavailable>"
    runtime_libraries = sorted(library_dir.glob("libc++*.so*"))
    runtime_identity = [
        f"{runtime_library} sha256={sha256_file(runtime_library)}"
        for runtime_library in runtime_libraries
        if runtime_library.is_file()
    ]
    symbolizer_hash = sha256_file(Path(symbolizer))
    dependencies_by_name: dict[str, PurePosixPath] = {}
    for line in dependencies.stdout.splitlines():
        match = re.search(r"(?:=>\s+)?(/\S+)\s+\(", line)
        if match is not None:
            dependency = PurePosixPath(match.group(1))
            dependencies_by_name[dependency.name] = dependency
    lines = [
        f"kernel: {platform.uname()}",
        "ASLR settings: "
        + ", ".join(
            f"{setting}={read_linux_setting('/proc/sys/' + setting.replace('.', '/'))}"
            for setting in (
                "vm.mmap_rnd_bits",
                "vm.mmap_rnd_compat_bits",
                "kernel.randomize_va_space",
            )
        ),
        f"stack limit: {stack_limit}",
        f"compiler: {clangxx} sha256={compiler_hash} return_code={compiler.returncode}\n"
        f"{compiler.stdout}{compiler.stderr}".rstrip(),
        f"probe: {probe} size={probe.stat().st_size} sha256={probe_hash}",
        f"instrumented libc++ directory: {library_dir}",
        "instrumented libc++ libraries: " + ("; ".join(runtime_identity) or "<none found>"),
        f"ldd return_code={dependencies.returncode}:\n{dependencies.stdout}{dependencies.stderr}".rstrip(),
        f"MSAN_SYMBOLIZER_PATH={symbolizer} sha256={symbolizer_hash}",
        f"symbolizer return_code={symbolizer_version.returncode}:\n"
        f"{symbolizer_version.stdout}{symbolizer_version.stderr}".rstrip(),
        f"MSAN_OPTIONS={runtime_environment['MSAN_OPTIONS']}",
    ]
    return "\n".join(lines), dependencies_by_name


def symbolize_reported_pcs(
    diagnostics: str,
    probe: Path,
    symbolizer: str,
    dependencies_by_name: dict[str, PurePosixPath],
) -> str:
    relative_addresses: dict[Path | PurePosixPath, set[str]] = {}
    for line in diagnostics.splitlines():
        frame = re.search(r"#\d+\s+0x[0-9a-fA-F]+.*?\(([^()]+)\+0x([0-9a-fA-F]+)\)", line)
        if frame is None:
            continue
        module = PurePosixPath(frame.group(1))
        if module.name == probe.name:
            object_path = probe
        elif module.is_absolute():
            object_path = module
        else:
            object_path = dependencies_by_name.get(module.name)
            if object_path is None:
                continue
        relative_addresses.setdefault(object_path, set()).add(f"0x{frame.group(2)}")

    symbolized: list[str] = []
    if relative_addresses:
        for object_path, addresses in relative_addresses.items():
            command = [
                symbolizer,
                "--inlining",
                f"--obj={object_path}",
                *sorted(addresses),
            ]
            completed = subprocess.run(
                command, text=True, capture_output=True, check=False, timeout=10
            )
            symbolized.append(
                f"$ {' '.join(command)} (return code {completed.returncode})\n"
                f"{completed.stdout}{completed.stderr}".rstrip()
            )
    else:
        symbolized.append("<no module-relative MSan frame addresses found>")
    return "\n".join(symbolized)


def timeout_output(value: str | bytes | None) -> str:
    if isinstance(value, bytes):
        return value.decode(errors="replace")
    return value or ""


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
    runtime_environment["MSAN_OPTIONS"] = MSAN_PROBE_OPTIONS
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
    positive_command = [clangxx, *common_flags, "-x", "c++", "-", "-o", str(positive_probe)]
    positive_build = subprocess.run(
        positive_command,
        input=MSAN_POSITIVE_PROBE_SOURCE,
        cwd=build,
        env=runtime_environment,
        text=True,
        capture_output=True,
        check=False,
    )
    if positive_build.returncode:
        raise ToolchainError(
            format_probe_failure(
                "instrumented libc++ positive-probe compilation", positive_command, positive_build
            )
        )
    try:
        positive_run = subprocess.run(
            [str(positive_probe)],
            cwd=build,
            env=runtime_environment,
            text=True,
            capture_output=True,
            timeout=30,
            check=False,
        )
    except subprocess.TimeoutExpired as timeout:
        positive_run = subprocess.CompletedProcess(
            [str(positive_probe)],
            124,
            timeout_output(timeout.stdout),
            timeout_output(timeout.stderr) + "\nMSan exception positive probe timed out after 30 seconds",
        )
    positive_diagnostics = positive_run.stdout + positive_run.stderr
    has_positive_diagnostic = MSAN_DIAGNOSTIC.search(positive_diagnostics) is not None
    if positive_run.returncode or has_positive_diagnostic:
        reason = "emitted a sanitizer diagnostic" if has_positive_diagnostic else "failed"
        raise ToolchainError(
            f"instrumented libc++ exception positive probe {reason}\n"
            + format_probe_failure(
                "instrumented libc++ exception positive-probe execution",
                [str(positive_probe)],
                positive_run,
            )
        )
    print("[msan-libcxx] PASS: typed exception, rethrow, and cleanup execute", flush=True)

    negative_source = (
        "#include <cstdlib>\n"
        "int main() {\n"
        "    auto* value = static_cast<int*>(std::malloc(sizeof(int)));\n"
        "    if (value == nullptr) {\n"
        "        return 2;\n"
        "    }\n"
        "    if (*static_cast<volatile int*>(value) == 0) {\n"
        "        std::free(value);\n"
        "        return 0;\n"
        "    }\n"
        "    std::free(value);\n"
        "    return 1;\n"
        "}\n"
    )
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
            format_probe_failure(
                "MSan negative-probe compilation", negative_command, negative_build
            )
        )
    symbolizer = resolve_msan_symbolizer(clangxx)
    if symbolizer is None:
        raise ToolchainError(
            "required llvm-symbolizer was not found beside the selected Clang or on PATH"
        )
    runtime_environment["MSAN_SYMBOLIZER_PATH"] = symbolizer
    context, dependencies_by_name = collect_probe_context(
        clangxx, negative_probe, library_dir, runtime_environment, symbolizer
    )
    print(f"[msan-libcxx] Negative-probe environment:\n{context}", flush=True)
    negative_command = [str(negative_probe)]
    try:
        negative_run = subprocess.run(
            negative_command,
            cwd=build,
            env=runtime_environment,
            text=True,
            capture_output=True,
            timeout=30,
            check=False,
        )
    except subprocess.TimeoutExpired as timeout:
        negative_run = subprocess.CompletedProcess(
            negative_command,
            124,
            timeout_output(timeout.stdout),
            timeout_output(timeout.stderr) + "\nMSan negative probe timed out after 30 seconds",
        )
    diagnostics = negative_run.stdout + negative_run.stderr
    has_msan_warning = any(
        MSAN_WARNING.search(output) is not None
        for output in (negative_run.stdout, negative_run.stderr)
    )
    if negative_run.returncode != MSAN_EXIT_CODE or not has_msan_warning:
        symbolized = symbolize_reported_pcs(
            diagnostics, negative_probe, symbolizer, dependencies_by_name
        )
        raise ToolchainError(
            "MSan did not diagnose the deliberate uninitialized read\n"
            + format_probe_failure(
                "MSan negative-probe execution", negative_command, negative_run
            )
            + f"\nnegative-probe environment:\n{context}"
            + f"\nsymbolized module-relative PCs:\n{symbolized}"
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

        print("[msan-libcxx] Apply pinned Linux x86_64 MSan libunwind patch", flush=True)
        apply_libunwind_msan_patch(args.source_root)
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
                "MSAN_OPTIONS": MSAN_ANALYSIS_OPTIONS,
            }
        )
        return 0
    except (OSError, subprocess.SubprocessError, ToolchainError) as error:
        print(f"FAIL MSan libc++ setup: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
