from __future__ import annotations

import contextlib
import importlib.util
import io
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path, PureWindowsPath
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "ci/install_msan_libcxx.py"


def load_module():
    spec = importlib.util.spec_from_file_location("install_msan_libcxx", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class InstallMsanLibcxxTests(unittest.TestCase):
    def setUp(self):
        self.toolchain = load_module()

    def compile_msan_ir(self, source, *extra_flags):
        clangxx = shutil.which("clang++")
        if clangxx is None:
            self.skipTest("Clang C++ is required for MSan IR controls")
        completed = subprocess.run(
            [
                clangxx,
                "--target=x86_64-pc-linux-gnu",
                "-std=c++17",
                "-O1",
                "-fsanitize=memory",
                "-fsanitize-memory-track-origins=2",
                *extra_flags,
                "-S",
                "-emit-llvm",
                "-x",
                "c++",
                "-o",
                "-",
                "-",
            ],
            input=source,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        return completed.stdout

    def function_ir(self, ir, name):
        marker = f"@{name}("
        self.assertIn(marker, ir)
        start = ir.rfind("define ", 0, ir.index(marker))
        end = ir.index("\n}", start) + 2
        return ir[start:end]

    def prepare_verification_tree(self, root):
        prefix = root / "prefix"
        include_dir = prefix / "include/c++/v1"
        library_dir = prefix / "lib"
        build = root / "build"
        include_dir.mkdir(parents=True)
        library_dir.mkdir(parents=True)
        build.mkdir()
        (library_dir / "libc++.so").write_bytes(b"test libc++ linker name")
        (library_dir / "libc++.so.1").write_bytes(b"test libc++ runtime")
        (library_dir / "libc++abi.so.1").write_bytes(b"test libc++abi runtime")
        negative_probe = build / "msan-uninitialized-read"
        negative_probe.write_bytes(b"test executable")
        symbolizer = root / "llvm-symbolizer-18"
        symbolizer.write_bytes(b"test symbolizer")
        symbolizer.chmod(0o755)
        return prefix, library_dir, build, negative_probe, symbolizer

    def diagnostic_processes(self, negative_run, prefix, *, symbolized=None):
        library_dir = prefix / "lib"
        processes = [
            subprocess.CompletedProcess([], 0, "", ""),
            subprocess.CompletedProcess([], 0, "", ""),
            subprocess.CompletedProcess([], 0, "", ""),
            subprocess.CompletedProcess([], 0, "clang version 18.1.3\n", ""),
            subprocess.CompletedProcess(
                [],
                0,
                f"libc++.so.1 => {library_dir / 'libc++.so.1'} (0x1234)\n"
                f"libc++abi.so.1 => {library_dir / 'libc++abi.so.1'} (0x1235)\n"
                "libgcc_s.so.1 => /usr/lib/libgcc_s.so.1 (0x1236)\n",
                "",
            ),
            subprocess.CompletedProcess([], 0, "llvm-symbolizer\n", ""),
            negative_run,
        ]
        if symbolized is not None:
            processes.extend(symbolized if isinstance(symbolized, list) else [symbolized])
        return processes

    def negative_report(self, *, pid="", read_site="msanNegativeHeapRead", allocation_site="msanNegativeHeapRead"):
        return (
            f"{pid}WARNING: MemorySanitizer: use-of-uninitialized-value\n"
            f"    #0 0x1234 in {read_site} /tmp/probe.cpp:8:9\n"
            "    #1 0x2345 in main /tmp/probe.cpp:17:21\n\n"
            "  Uninitialized value was created by a heap allocation\n"
            "    #0 0x3456 in malloc /tmp/msan_interceptors.cpp:123:3\n"
            f"    #1 0x4567 in {allocation_site} /tmp/probe.cpp:3:37\n\n"
            "SUMMARY: MemorySanitizer: use-of-uninitialized-value /tmp/probe.cpp:8:9 in msanNegativeHeapRead\n"
        )

    def assert_negative_probe_rejected(self, negative_run):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, _, build, _, symbolizer = self.prepare_verification_tree(root)
            with (
                mock.patch.object(
                    self.toolchain.subprocess,
                    "run",
                    side_effect=self.diagnostic_processes(negative_run, prefix),
                ),
                mock.patch.object(self.toolchain, "resolve_msan_symbolizer", return_value=str(symbolizer)),
            ):
                with self.assertRaisesRegex(self.toolchain.ToolchainError, "did not diagnose") as raised:
                    self.toolchain.verify_install(prefix, "clang++", build)
        return str(raised.exception)

    def assert_positive_probe_rejected(self, positive_run):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, _, build, _, _ = self.prepare_verification_tree(root)
            with mock.patch.object(
                self.toolchain.subprocess,
                "run",
                side_effect=[subprocess.CompletedProcess([], 0, "", ""), positive_run],
            ) as run:
                with self.assertRaises(self.toolchain.ToolchainError) as raised:
                    self.toolchain.verify_install(prefix, "clang++", build)

        self.assertEqual(run.call_count, 2)
        return str(raised.exception)

    def test_resolve_tool_preserves_clang_driver_aliases(self):
        with (
            mock.patch.object(
                self.toolchain.shutil,
                "which",
                side_effect=["/usr/bin/clang-18", "/usr/bin/clang++-18"],
            ),
            mock.patch.object(
                self.toolchain.Path,
                "resolve",
                return_value=Path("/usr/lib/llvm-18/bin/clang"),
            ) as resolve,
        ):
            clang = self.toolchain.resolve_tool("clang-18", "Clang C")
            clangxx = self.toolchain.resolve_tool("clang++-18", "Clang C++")

        self.assertEqual(clang, self.toolchain.os.path.abspath("/usr/bin/clang-18"))
        self.assertEqual(clangxx, self.toolchain.os.path.abspath("/usr/bin/clang++-18"))
        resolve.assert_not_called()

    def test_runtime_build_is_pinned_and_instrumented(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            command = self.toolchain.configure_command(
                root / "llvm-project",
                root / "build",
                root / "prefix",
                "/usr/bin/clang",
                "/usr/bin/clang++",
            )
        self.assertEqual(self.toolchain.LLVM_TAG, "llvmorg-18.1.3")
        self.assertEqual(self.toolchain.LLVM_COMMIT, "c13b7485b87909fcf739f62cfa382b55407433c0")
        self.assertIn("-DLLVM_USE_SANITIZER=MemoryWithOrigins", command)
        self.assertIn("-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi", command)
        self.assertIn("-DLIBCXX_USE_COMPILER_RT=ON", command)
        self.assertIn("-DLIBCXXABI_USE_LLVM_UNWINDER=OFF", command)
        self.assertFalse(any("libunwind" in arg.lower() for arg in command))
        self.assertFalse(any("ignorelist" in arg.lower() for arg in command))
        self.assertFalse(any(argument.startswith("-DCMAKE_CXX_FLAGS=") for argument in command))

    def test_default_msan_application_keeps_branch_and_return_checks(self):
        source = (
            "extern \"C\" void side_effect();\n"
            "extern \"C\" void application_call() {\n"
            "  volatile int value;\n"
            "  if (value) side_effect();\n"
            "}\n"
            "extern \"C\" int application_return() {\n"
            "  volatile int value;\n"
            "  return value;\n"
            "}\n"
        )
        ir = self.compile_msan_ir(source)
        application_call = self.function_ir(ir, "application_call")
        application_return = self.function_ir(ir, "application_return")
        self.assertIn("sanitize_memory", ir)
        self.assertIn("call void @__msan_warning_with_origin_noreturn", application_call)
        self.assertIn("call void @side_effect()", application_call)
        self.assertIn("load volatile i32, ptr", application_return)
        self.assertIn("call void @__msan_warning_with_origin_noreturn", application_return)

    def test_build_contains_only_instrumented_libcxx_and_libcxxabi_targets(self):
        command = self.toolchain.build_command(Path("build"), jobs=4)
        target_index = command.index("--target")
        parallel_index = command.index("--parallel")
        self.assertEqual(
            command[target_index + 1 : parallel_index],
            ["cxx", "cxx_experimental", "cxxabi"],
        )

    def test_dependency_closure_uses_instrumented_runtimes_and_system_libgcc_s(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / "prefix"
            library_dir = prefix / "lib"
            library_dir.mkdir(parents=True)
            output = (
                f"libc++.so.1 => {library_dir / 'libc++.so.1.0.0'} (0x1234)\n"
                f"libc++abi.so.1 => {library_dir / 'libc++abi.so.1.0.0'} (0x1235)\n"
                "libgcc_s.so.1 => /usr/lib/libgcc_s.so.1 (0x1236)\n"
            )
            dependencies = self.toolchain.require_system_unwinder(output, 0, prefix)

        self.assertEqual(set(dependencies), {"libc++.so.1", "libc++abi.so.1", "libgcc_s.so.1"})

    def test_dependency_closure_rejects_bundled_or_missing_unwinder(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / "prefix"
            library_dir = prefix / "lib"
            library_dir.mkdir(parents=True)
            runtimes = (
                f"libc++.so.1 => {library_dir / 'libc++.so.1'} (0x1234)\n"
                f"libc++abi.so.1 => {library_dir / 'libc++abi.so.1'} (0x1235)\n"
            )
            with self.assertRaisesRegex(self.toolchain.ToolchainError, "missing system unwinder"):
                self.toolchain.require_system_unwinder(runtimes, 0, prefix)
            with self.assertRaisesRegex(self.toolchain.ToolchainError, "ldd failed"):
                self.toolchain.require_system_unwinder(runtimes, 1, prefix)
            with self.assertRaisesRegex(self.toolchain.ToolchainError, "unexpectedly references libunwind"):
                self.toolchain.require_system_unwinder(
                    runtimes
                    + "libgcc_s.so.1 => /usr/lib/libgcc_s.so.1 (0x1236)\n"
                    + f"libunwind.so.1 => {library_dir / 'libunwind.so.1'} (0x1237)\n",
                    0,
                    prefix,
                )
            with self.assertRaisesRegex(self.toolchain.ToolchainError, "unexpectedly references libunwind"):
                self.toolchain.require_system_unwinder(
                    runtimes
                    + "libgcc_s.so.1 => /usr/lib/libgcc_s.so.1 (0x1236)\n"
                    + "libunwind.so.1 => not found\n",
                    0,
                    prefix,
                )

    def test_compiler_major_must_match_pinned_llvm_sources(self):
        self.toolchain.require_compatible_clang("Ubuntu clang version 18.1.3", "clang-18")
        with self.assertRaisesRegex(self.toolchain.ToolchainError, "Clang 22, LLVM 18"):
            self.toolchain.require_compatible_clang("clang version 22.1.7", "clang-22")
        with self.assertRaisesRegex(self.toolchain.ToolchainError, "did not identify itself as Clang"):
            self.toolchain.require_compatible_clang("gcc version 13.2.0", "gcc")

    def test_setup_refuses_non_linux_hosts(self):
        with mock.patch.object(self.toolchain.platform, "system", return_value="Darwin"):
            with self.assertRaisesRegex(self.toolchain.ToolchainError, "only on Linux"):
                self.toolchain.require_linux()

    def test_install_requires_runtime_to_catch_an_uninitialized_read(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, library_dir, build, _, symbolizer = self.prepare_verification_tree(root)
            negative_run = subprocess.CompletedProcess(
                [], 86, "", self.negative_report()
            )
            output = io.StringIO()
            with (
                mock.patch.object(
                    self.toolchain.subprocess,
                    "run",
                    side_effect=self.diagnostic_processes(negative_run, prefix),
                ) as run,
                mock.patch.object(self.toolchain, "resolve_msan_symbolizer", return_value=str(symbolizer)),
                contextlib.redirect_stdout(output),
            ):
                self.toolchain.verify_install(prefix, "clang++", build)

            self.assertIn("Accepted negative-probe report:", output.getvalue())
            self.assertIn(self.negative_report(), output.getvalue())

            commands = [call.args[0] for call in run.call_args_list]
            include_dir = prefix / "include/c++/v1"
            for command in (commands[0], commands[2]):
                self.assertIn("-fsanitize=memory", command)
                self.assertIn("-fsanitize-memory-track-origins=2", command)
                self.assertNotIn("-fno-sanitize-memory-param-retval", command)
                self.assertIn("-stdlib=libc++", command)
                self.assertIn(str(include_dir), " ".join(command))
                self.assertIn(str(library_dir), " ".join(command))
            positive_source = run.call_args_list[0].kwargs["input"]
            self.assertIn("[[gnu::noinline]] void throw_value(int32_t value)", positive_source)
            self.assertIn("volatile int32_t runtime_seed = 0;", positive_source)
            self.assertIn("#include <string>", positive_source)
            self.assertIn("std::to_string(expected)", positive_source)
            self.assertIn("!marker.empty()", positive_source)
            self.assertIn("Cleanup cleanup{count};", positive_source)
            self.assertIn("throw;", positive_source)
            self.assertIn("cleanup_count != 1", positive_source)
            negative_source = run.call_args_list[2].kwargs["input"]
            self.assertIn("std::malloc(sizeof(int))", negative_source)
            self.assertIn("volatile int", negative_source)
            self.assertIn(
                "if (*static_cast<volatile int*>(value) == 0)",
                negative_source,
            )
            self.assertNotIn("return result;", negative_source)
            self.assertIn('extern "C" [[gnu::noinline]] int msanNegativeHeapRead()', negative_source)
            self.assertNotIn("__msan_", negative_source)
            negative_environment = run.call_args_list[6].kwargs["env"]
            self.assertEqual(negative_environment["MSAN_SYMBOLIZER_PATH"], str(symbolizer))
            self.assertIn("symbolize=1", negative_environment["MSAN_OPTIONS"])
            self.assertIn("fast_unwind_on_fatal=1", negative_environment["MSAN_OPTIONS"])
            self.assertEqual(negative_environment["DEBUGINFOD_URLS"], "")
            self.assertEqual(commands[3], ["clang++", "--version"])
            self.assertEqual(commands[4][0], "ldd")
            self.assertEqual(commands[5], [str(symbolizer), "--version"])

    def test_controls_and_fallback_symbolization_use_offline_environment_only(self):
        inherited_urls = "https://debuginfod.invalid"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, _, build, _, symbolizer = self.prepare_verification_tree(root)
            failed_probe = subprocess.CompletedProcess(
                [], -11, "", "MemorySanitizer: stack-overflow\n"
                "    #0 0x1234 in main (/tmp/build/msan-uninitialized-read+0x1234)\n"
                "    #1 0x2345 in operator new (libc++.so.1+0x2345)\n"
                "MemorySanitizer:DEADLYSIGNAL",
            )
            processes = self.diagnostic_processes(
                failed_probe,
                prefix,
                symbolized=[
                    subprocess.CompletedProcess([], 0, "main\nprobe.cpp:5:7\n", ""),
                    subprocess.CompletedProcess([], 0, "operator new\nlibc++/new.cpp:9:1\n", ""),
                ],
            )
            with (
                mock.patch.dict(self.toolchain.os.environ, {"DEBUGINFOD_URLS": inherited_urls}),
                mock.patch.object(
                    self.toolchain.subprocess, "run", side_effect=processes
                ) as run,
                mock.patch.object(self.toolchain, "resolve_msan_symbolizer", return_value=str(symbolizer)),
            ):
                with self.assertRaisesRegex(self.toolchain.ToolchainError, "did not diagnose"):
                    self.toolchain.verify_install(prefix, "clang++", build)

                self.assertEqual(self.toolchain.os.environ["DEBUGINFOD_URLS"], inherited_urls)

        self.assertEqual(run.call_count, 9)
        for call in run.call_args_list:
            with self.subTest(command=call.args[0]):
                self.assertEqual(call.kwargs["env"]["DEBUGINFOD_URLS"], "")

    def test_positive_probe_compile_failure_stops_before_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, _, build, _, _ = self.prepare_verification_tree(root)
            compile_failure = subprocess.CompletedProcess([], 1, "compiler stdout", "compiler stderr")
            with mock.patch.object(
                self.toolchain.subprocess,
                "run",
                side_effect=[compile_failure],
            ) as run:
                with self.assertRaisesRegex(
                    self.toolchain.ToolchainError, "positive-probe compilation"
                ) as raised:
                    self.toolchain.verify_install(prefix, "clang++", build)

        self.assertEqual(run.call_count, 1)
        self.assertIn("compiler stdout", str(raised.exception))
        self.assertIn("compiler stderr", str(raised.exception))

    def test_positive_probe_nonzero_exit_fails_before_negative_control(self):
        message = self.assert_positive_probe_rejected(
            subprocess.CompletedProcess([], -11, "positive stdout", "positive stderr")
        )
        self.assertIn("return code: -11", message)
        self.assertIn("positive stdout", message)
        self.assertIn("positive stderr", message)

    def test_positive_probe_timeout_preserves_partial_output(self):
        message = self.assert_positive_probe_rejected(
            subprocess.TimeoutExpired(
                ["msan-libcxx-positive"],
                30,
                output="partial stdout",
                stderr="partial stderr",
            )
        )
        self.assertIn("return code: 124", message)
        self.assertIn("partial stdout", message)
        self.assertIn("partial stderr", message)
        self.assertIn("timed out after 30 seconds", message)

    def test_positive_probe_sanitizer_diagnostic_fails_even_on_zero_exit(self):
        message = self.assert_positive_probe_rejected(
            subprocess.CompletedProcess(
                [], 0, "", "WARNING: MemorySanitizer: use-of-uninitialized-value\n"
            )
        )
        self.assertIn("emitted a sanitizer diagnostic", message)
        self.assertIn("use-of-uninitialized-value", message)

    def test_verified_toolchain_exports_offline_environment_and_upstream_recipe(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            environment_file = root / "github-env"
            commands = []

            def fake_run(command, **_kwargs):
                commands.append(command)
                if command[-1] == "--version":
                    return "Ubuntu clang version 18.1.3\n"
                if command[-2:] == ["rev-parse", "HEAD"]:
                    return self.toolchain.LLVM_COMMIT
                return ""

            with (
                mock.patch.object(self.toolchain, "require_linux"),
                mock.patch.object(
                    self.toolchain,
                    "resolve_tool",
                    side_effect=["/usr/bin/clang-18", "/usr/bin/clang++-18"],
                ),
                mock.patch.object(self.toolchain, "run", side_effect=fake_run),
                mock.patch.object(self.toolchain, "verify_install"),
                mock.patch.dict(
                    self.toolchain.os.environ,
                    {
                        "GITHUB_ENV": str(environment_file),
                        "DEBUGINFOD_URLS": "https://debuginfod.invalid",
                    },
                ),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                result = self.toolchain.main(
                    [
                        "--source-root",
                        str(root / "llvm" / "source"),
                        "--build-root",
                        str(root / "llvm" / "build"),
                        "--prefix",
                        str(root / "msan-libcxx"),
                        "--clang",
                        "clang-18",
                        "--clangxx",
                        "clang++-18",
                        "--jobs",
                        "1",
                    ]
                )
                self.assertEqual(
                    self.toolchain.os.environ["DEBUGINFOD_URLS"], "https://debuginfod.invalid"
                )

            self.assertEqual(result, 0)
            self.assertIn(
                f"MSAN_OPTIONS={self.toolchain.MSAN_ANALYSIS_OPTIONS}\n",
                environment_file.read_text(encoding="utf-8"),
            )
            self.assertIn("DEBUGINFOD_URLS=\n", environment_file.read_text(encoding="utf-8"))
            identity_index = next(
                index for index, command in enumerate(commands) if command[-2:] == ["rev-parse", "HEAD"]
            )
            configure_index = next(
                index for index, command in enumerate(commands) if command[:2] == ["cmake", "-G"]
            )
            self.assertLess(identity_index, configure_index)
            self.assertFalse(any("apply" in command for command in commands))
            self.assertIn("-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi", commands[configure_index])
            self.assertIn("-DLIBCXXABI_USE_LLVM_UNWINDER=OFF", commands[configure_index])

    def test_local_setup_prints_verified_environment_without_github_env(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def fake_run(command, **_kwargs):
                if command[-1] == "--version":
                    return "Ubuntu clang version 18.1.3\n"
                if command[-2:] == ["rev-parse", "HEAD"]:
                    return self.toolchain.LLVM_COMMIT
                return ""

            output = io.StringIO()
            with (
                mock.patch.object(self.toolchain, "require_linux"),
                mock.patch.object(
                    self.toolchain,
                    "resolve_tool",
                    side_effect=["/usr/bin/clang-18", "/usr/bin/clang++-18"],
                ),
                mock.patch.object(self.toolchain, "run", side_effect=fake_run),
                mock.patch.object(self.toolchain, "verify_install"),
                mock.patch.dict(
                    self.toolchain.os.environ,
                    {"DEBUGINFOD_URLS": "https://debuginfod.invalid"},
                    clear=True,
                ),
                contextlib.redirect_stdout(output),
            ):
                result = self.toolchain.main(
                    [
                        "--source-root",
                        str(root / "llvm" / "source"),
                        "--build-root",
                        str(root / "llvm" / "build"),
                        "--prefix",
                        str(root / "msan-libcxx"),
                        "--clang",
                        "clang-18",
                        "--clangxx",
                        "clang++-18",
                        "--jobs",
                        "1",
                    ]
                )

                self.assertNotIn("GITHUB_ENV", self.toolchain.os.environ)
                self.assertEqual(
                    self.toolchain.os.environ["DEBUGINFOD_URLS"], "https://debuginfod.invalid"
                )

        self.assertEqual(result, 0)
        self.assertIn("Add these verified exports to the shell running analysis", output.getvalue())
        self.assertIn(
            f"export MSAN_OPTIONS={self.toolchain.shlex.quote(self.toolchain.MSAN_ANALYSIS_OPTIONS)}",
            output.getvalue(),
        )
        self.assertIn("export DEBUGINFOD_URLS=''", output.getvalue())

    def test_environment_export_rejects_multiline_values_without_partial_output(self):
        with tempfile.TemporaryDirectory() as directory:
            environment_file = Path(directory) / "github-env"
            with mock.patch.dict(self.toolchain.os.environ, {"GITHUB_ENV": str(environment_file)}):
                with self.assertRaisesRegex(self.toolchain.ToolchainError, "multiline environment value"):
                    self.toolchain.write_github_environment({"CC": "clang\nCXX=clang++"})
            self.assertFalse(environment_file.exists())

    def test_source_pin_mismatch_stops_before_configure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            environment_file = root / "github-env"
            commands = []

            def fake_run(command, **_kwargs):
                commands.append(command)
                if command[-1] == "--version":
                    return "Ubuntu clang version 18.1.3\n"
                if command[-2:] == ["rev-parse", "HEAD"]:
                    return "unexpected-source-commit"
                return ""

            with (
                mock.patch.object(self.toolchain, "require_linux"),
                mock.patch.object(
                    self.toolchain,
                    "resolve_tool",
                    side_effect=["/usr/bin/clang-18", "/usr/bin/clang++-18"],
                ),
                mock.patch.object(self.toolchain, "run", side_effect=fake_run),
                mock.patch.dict(self.toolchain.os.environ, {"GITHUB_ENV": str(environment_file)}),
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                result = self.toolchain.main(
                    [
                        "--source-root",
                        str(root / "llvm" / "source"),
                        "--build-root",
                        str(root / "llvm" / "build"),
                        "--prefix",
                        str(root / "msan-libcxx"),
                        "--clang",
                        "clang-18",
                        "--clangxx",
                        "clang++-18",
                        "--jobs",
                        "1",
                    ]
                )

            self.assertEqual(result, 1)
            self.assertFalse(environment_file.exists())
            self.assertFalse(any("apply" in command for command in commands))
            self.assertFalse(any(command[:2] == ["cmake", "-G"] for command in commands))

    def test_install_accepts_standard_pid_prefix_on_msan_warning(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, _, build, _, symbolizer = self.prepare_verification_tree(root)
            negative_run = subprocess.CompletedProcess(
                [], 86, "", self.negative_report(pid="==12345==")
            )
            with (
                mock.patch.object(
                    self.toolchain.subprocess,
                    "run",
                    side_effect=self.diagnostic_processes(negative_run, prefix),
                ),
                mock.patch.object(
                    self.toolchain, "resolve_msan_symbolizer", return_value=str(symbolizer)
                ),
            ):
                self.toolchain.verify_install(prefix, "clang++", build)

    def test_negative_probe_has_an_ordinary_instrumented_noinline_heap_read(self):
        source = self.toolchain.MSAN_NEGATIVE_PROBE_SOURCE.replace(
            "#include <cstdlib>",
            'extern "C" void* malloc(__SIZE_TYPE__); extern "C" void free(void*);\n'
            "namespace std { using ::malloc; using ::free; }",
        )
        self.assertNotIn("__msan_", source)
        ir = self.compile_msan_ir(source)
        body = self.function_ir(ir, "msanNegativeHeapRead")
        self.assertIn("@malloc(", body)
        self.assertIn("load volatile i32", body)
        self.assertIn("@__msan_warning_with_origin_noreturn(", body)
        attributes = re.search(r"define[^\n]+@msanNegativeHeapRead\([^\n]+#([0-9]+)", ir)
        self.assertIsNotNone(attributes)
        self.assertRegex(ir, rf"attributes #{attributes.group(1)} = \{{[^\n]*\bnoinline\b")

    def test_negative_report_accepts_symbolized_format_variants_and_chained_origin(self):
        report = self.negative_report().replace(
            "  Uninitialized value was created",
            "  Uninitialized value was stored to memory at\n"
            "    #0 0x5678 in otherFunction /tmp/probe.cpp:5:3\n\n"
            "  Uninitialized value was created",
        )
        for variant in (report, report.replace("\n", "\r\n"), "\x1b[1m" + report + "\x1b[0m"):
            with self.subTest(variant=repr(variant[:30])):
                self.assertIsNotNone(self.toolchain.negative_report_evidence(variant))

    def test_negative_report_rejects_unrelated_primary_even_with_named_caller(self):
        report = self.negative_report(read_site="libcStartup").replace("in main", "in msanNegativeHeapRead")
        self.assertIsNone(self.toolchain.negative_report_evidence(report))
        self.assert_negative_probe_rejected(subprocess.CompletedProcess([], 86, "", report))

    def test_negative_report_requires_matching_heap_allocation_provenance(self):
        report = self.negative_report(allocation_site="otherAllocation")
        self.assertIsNone(self.toolchain.negative_report_evidence(report))
        self.assert_negative_probe_rejected(subprocess.CompletedProcess([], 86, "", report))
        for origin in ("a stack frame", "a heap deallocation", "destroyed member fields"):
            with self.subTest(origin=origin):
                self.assertIsNone(self.toolchain.negative_report_evidence(
                    self.negative_report().replace("a heap allocation", origin)))

    def test_negative_report_rejects_missing_origin_and_warning_only(self):
        report = self.negative_report().split("  Uninitialized value", 1)[0]
        self.assertIsNone(self.toolchain.negative_report_evidence(report))
        self.assert_negative_probe_rejected(subprocess.CompletedProcess([], 86, "", report))
        self.assertIsNone(self.toolchain.negative_report_evidence(
            "WARNING: MemorySanitizer: use-of-uninitialized-value\n"))

    def test_negative_report_rejects_named_allocation_caller_at_frame_two(self):
        report = self.negative_report(allocation_site="helperAllocation").replace(
            "    #1 0x4567 in helperAllocation /tmp/probe.cpp:3:37",
            "    #1 0x4567 in helperAllocation /tmp/probe.cpp:3:37\n"
            "    #2 0x5678 in msanNegativeHeapRead /tmp/probe.cpp:4:2",
        )
        self.assertIsNone(self.toolchain.negative_report_evidence(report))
        self.assert_negative_probe_rejected(subprocess.CompletedProcess([], 86, "", report))

    def test_negative_report_cannot_combine_separate_reports_or_text_only_names(self):
        report = self.negative_report(allocation_site="otherAllocation") + self.negative_report(read_site="libcStartup")
        self.assertIsNone(self.toolchain.negative_report_evidence(report))
        report = self.negative_report(allocation_site="otherAllocation") + "msanNegativeHeapRead\n"
        self.assertIsNone(self.toolchain.negative_report_evidence(report))
        report = self.negative_report(allocation_site="otherAllocation").replace(
            "SUMMARY:", "    #0 0x6789 in msanNegativeHeapRead /tmp/unrelated.cpp:1:1\nSUMMARY:")
        self.assertIsNone(self.toolchain.negative_report_evidence(report))

    def test_negative_report_rejects_an_additional_fatal_diagnostic(self):
        self.assertIsNone(self.toolchain.negative_report_evidence(
            self.negative_report() + "MemorySanitizer:DEADLYSIGNAL\n"))
        self.assertIsNone(self.toolchain.negative_report_evidence(
            self.negative_report() + "ERROR: MemorySanitizer: unexpected failure\n"))
        self.assertIsNone(self.toolchain.negative_report_evidence(
            self.negative_report() + "WARNING: MemorySanitizer: unexpected failure\n"))

    def test_negative_report_retention_is_bounded_and_keeps_accepted_provenance(self):
        report = self.negative_report().replace(
            "  Uninitialized value was created", "x" * 20000 + "\n  Uninitialized value was created")
        evidence = self.toolchain.negative_report_evidence(report)
        self.assertIsNotNone(evidence)
        self.assertLess(len(evidence), self.toolchain.MSAN_NEGATIVE_REPORT_LIMIT + 4200)
        self.assertIn("[report truncated; accepted provenance follows]", evidence)
        self.assertIn("#0 0x1234 in msanNegativeHeapRead /tmp/probe.cpp:8:9", evidence)
        self.assertIn("Uninitialized value was created by a heap allocation", evidence)
        self.assertIn("#1 0x4567 in msanNegativeHeapRead /tmp/probe.cpp:3:37", evidence)
        self.assertIsNone(self.toolchain.negative_report_evidence(
            self.negative_report().replace("/tmp/probe.cpp:8:9", "/" + "a" * 2000)))

    def test_install_rejects_a_toolchain_that_misses_the_probe(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, _, build, _, symbolizer = self.prepare_verification_tree(root)
            missed_probe = subprocess.CompletedProcess([], 0, "", "")
            with (
                mock.patch.object(
                    self.toolchain.subprocess,
                    "run",
                    side_effect=self.diagnostic_processes(missed_probe, prefix),
                ),
                mock.patch.object(self.toolchain, "resolve_msan_symbolizer", return_value=str(symbolizer)),
            ):
                with self.assertRaisesRegex(self.toolchain.ToolchainError, "did not diagnose"):
                    self.toolchain.verify_install(prefix, "clang++", build)

    def test_exit_code_86_without_uninitialized_read_diagnostic_is_failure(self):
        message = self.assert_negative_probe_rejected(
            subprocess.CompletedProcess(
                [], 86, "", "MemorySanitizer: stack-overflow\nMemorySanitizer:DEADLYSIGNAL"
            )
        )
        self.assertIn("return code: 86", message)
        self.assertIn("MemorySanitizer: stack-overflow", message)

    def test_exit_code_86_with_unrelated_text_containing_error_kind_is_failure(self):
        message = self.assert_negative_probe_rejected(
            subprocess.CompletedProcess(
                [], 86, "", "not an MSan warning: use-of-uninitialized-value"
            )
        )
        self.assertIn("return code: 86", message)
        self.assertIn("not an MSan warning: use-of-uninitialized-value", message)

    def test_exit_code_86_with_unicode_pid_prefix_is_failure(self):
        message = self.assert_negative_probe_rejected(
            subprocess.CompletedProcess(
                [], 86, "", "==١٢٣==WARNING: MemorySanitizer: use-of-uninitialized-value\n"
            )
        )
        self.assertIn("return code: 86", message)
        self.assertIn("==١٢٣==WARNING: MemorySanitizer: use-of-uninitialized-value", message)

    def test_uninitialized_read_diagnostic_with_wrong_exit_code_is_failure(self):
        message = self.assert_negative_probe_rejected(
            subprocess.CompletedProcess(
                [], 1, "", self.negative_report()
            )
        )
        self.assertIn("return code: 1", message)
        self.assertIn("use-of-uninitialized-value", message)

    def test_negative_probe_timeout_preserves_diagnostics_and_context(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, _, build, negative_probe, symbolizer = self.prepare_verification_tree(root)
            timeout = subprocess.TimeoutExpired(
                [str(negative_probe)], 30, output="partial stdout", stderr="partial stderr"
            )
            process_results = [
                *self.diagnostic_processes(
                    subprocess.CompletedProcess([], 0, "", ""), prefix
                )[:6],
                timeout,
            ]
            with (
                mock.patch.object(self.toolchain.subprocess, "run", side_effect=process_results),
                mock.patch.object(self.toolchain, "resolve_msan_symbolizer", return_value=str(symbolizer)),
            ):
                with self.assertRaisesRegex(self.toolchain.ToolchainError, "did not diagnose") as raised:
                    self.toolchain.verify_install(prefix, "clang++", build)

        message = str(raised.exception)
        self.assertIn("return code: 124", message)
        self.assertIn("partial stdout", message)
        self.assertIn("partial stderr", message)
        self.assertIn("timed out after 30 seconds", message)
        self.assertIn(f"probe: {negative_probe}", message)

    def test_negative_probe_failure_reports_child_exit_and_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, _, build, negative_probe, symbolizer = self.prepare_verification_tree(root)
            completed = [
                *self.diagnostic_processes(
                subprocess.CompletedProcess(
                    [],
                    -11,
                    "probe stdout",
                    "MemorySanitizer: stack-overflow\n"
                    "    #0 0x5615fe3a250b in main (/tmp/build/msan-uninitialized-read+0x1250b)\n"
                    "    #1 0x7f0000001234 in operator new (libc++.so.1+0x1234)\n"
                    "MemorySanitizer:DEADLYSIGNAL",
                ),
                prefix,
                symbolized=[
                    subprocess.CompletedProcess([], 0, "main\n/tmp/probe.cpp:5:7\n", ""),
                    subprocess.CompletedProcess([], 0, "operator new\nlibc++/new.cpp:9:1\n", ""),
                ],
            ),
            ]
            with (
                mock.patch.object(self.toolchain.subprocess, "run", side_effect=completed) as run,
                mock.patch.object(self.toolchain, "resolve_msan_symbolizer", return_value=str(symbolizer)),
            ):
                with self.assertRaisesRegex(self.toolchain.ToolchainError, "did not diagnose") as raised:
                    self.toolchain.verify_install(prefix, "clang++", build)

        message = str(raised.exception)
        self.assertIn("return code: -11", message)
        self.assertIn("stdout:\nprobe stdout", message)
        self.assertIn("stderr:\nMemorySanitizer: stack-overflow", message)
        self.assertIn("kernel:", message)
        self.assertIn("vm.mmap_rnd_bits=", message)
        self.assertIn("stack limit:", message)
        self.assertIn("compiler: clang++ sha256=", message)
        self.assertIn("clang version 18.1.3", message)
        self.assertIn(f"probe: {negative_probe}", message)
        self.assertRegex(message, r"probe: .* sha256=[0-9a-f]{64}")
        self.assertIn("libc++.so", message)
        self.assertIn(f"MSAN_SYMBOLIZER_PATH={symbolizer}", message)
        self.assertIn("symbolized module-relative PCs:\n$ ", message)
        self.assertIn("/tmp/probe.cpp:5:7", message)
        self.assertIn("libc++/new.cpp:9:1", message)
        self.assertIn(f"--obj={negative_probe}", " ".join(run.call_args_list[7].args[0]))
        self.assertIn(
            f"--obj={prefix / 'lib/libc++.so.1'}",
            " ".join(run.call_args_list[8].args[0]),
        )

    def test_symbolization_keeps_linux_module_paths_posix_on_windows_hosts(self):
        diagnostics = (
            "    #0 0x1 in main (/tmp/build/msan-uninitialized-read+0x1250b)\n"
            "    #1 0x2 in operator new (/tmp/prefix/lib/libc++.so+0x1234)\n"
        )
        symbolized_frames = [
            subprocess.CompletedProcess([], 0, "main\nprobe.cpp:5:7\n", ""),
            subprocess.CompletedProcess([], 0, "operator new\nlibc++/new.cpp:9:1\n", ""),
        ]
        with (
            mock.patch.object(self.toolchain, "Path", PureWindowsPath),
            mock.patch.object(
                self.toolchain.subprocess, "run", side_effect=symbolized_frames
            ) as run,
        ):
            symbolized = self.toolchain.symbolize_reported_pcs(
                diagnostics,
                PureWindowsPath("C:/build/msan-uninitialized-read"),
                "llvm-symbolizer",
                {},
                {"DEBUGINFOD_URLS": ""},
            )

        self.assertEqual(run.call_count, 2)
        self.assertIn(
            "--obj=C:\\build\\msan-uninitialized-read",
            " ".join(run.call_args_list[0].args[0]),
        )
        self.assertIn(
            "--obj=/tmp/prefix/lib/libc++.so",
            " ".join(run.call_args_list[1].args[0]),
        )
        self.assertIn("probe.cpp:5:7", symbolized)
        self.assertIn("libc++/new.cpp:9:1", symbolized)


if __name__ == "__main__":
    unittest.main()
