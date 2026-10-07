from __future__ import annotations

import contextlib
import importlib.util
import io
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

    def prepare_verification_tree(self, root):
        prefix = root / "prefix"
        include_dir = prefix / "include/c++/v1"
        library_dir = prefix / "lib"
        build = root / "build"
        include_dir.mkdir(parents=True)
        library_dir.mkdir(parents=True)
        build.mkdir()
        (library_dir / "libc++.so").write_bytes(b"test libc++ runtime")
        negative_probe = build / "msan-uninitialized-read"
        negative_probe.write_bytes(b"test executable")
        symbolizer = root / "llvm-symbolizer-18"
        symbolizer.write_bytes(b"test symbolizer")
        symbolizer.chmod(0o755)
        return prefix, library_dir, build, negative_probe, symbolizer

    def diagnostic_processes(self, negative_run, *, symbolized=None):
        processes = [
            subprocess.CompletedProcess([], 0, "", ""),
            subprocess.CompletedProcess([], 0, "", ""),
            subprocess.CompletedProcess([], 0, "", ""),
            subprocess.CompletedProcess([], 0, "clang version 18.1.3\n", ""),
            subprocess.CompletedProcess(
                [], 0, "libc++.so => /tmp/prefix/lib/libc++.so (0x1234)\n", ""
            ),
            subprocess.CompletedProcess([], 0, "llvm-symbolizer\n", ""),
            negative_run,
        ]
        if symbolized is not None:
            processes.extend(symbolized if isinstance(symbolized, list) else [symbolized])
        return processes

    def assert_negative_probe_rejected(self, negative_run):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, _, build, _, symbolizer = self.prepare_verification_tree(root)
            with (
                mock.patch.object(
                    self.toolchain.subprocess,
                    "run",
                    side_effect=self.diagnostic_processes(negative_run),
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
        self.assertIn("-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi;libunwind", command)
        self.assertIn("-DLIBCXX_USE_COMPILER_RT=ON", command)

    def test_libunwind_patch_is_pinned_and_limited_to_linux_x86_64_msan(self):
        patch = self.toolchain.LIBUNWIND_MSAN_PATCH.read_text(encoding="utf-8")
        self.assertEqual(
            self.toolchain.sha256_file(self.toolchain.LIBUNWIND_MSAN_PATCH),
            self.toolchain.LIBUNWIND_MSAN_PATCH_SHA256,
        )
        self.assertIn('CMAKE_SYSTEM_NAME STREQUAL "Linux"', patch)
        self.assertIn('CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$"', patch)
        self.assertIn('LLVM_USE_SANITIZER MATCHES "^Memory(WithOrigins)?$"', patch)
        self.assertIn(
            "set_property(SOURCE UnwindRegistersSave.S APPEND PROPERTY COMPILE_DEFINITIONS",
            patch,
        )
        self.assertIn("movq  $136, %rsi", patch)
        self.assertIn("call  __msan_unpoison", patch)

    def test_libunwind_patch_missing_fails_before_git_apply(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            missing_patch = root / "missing.patch"
            with (
                mock.patch.object(self.toolchain, "LIBUNWIND_MSAN_PATCH", missing_patch),
                mock.patch.object(self.toolchain, "run") as run,
            ):
                with self.assertRaisesRegex(self.toolchain.ToolchainError, "missing or unreadable"):
                    self.toolchain.apply_libunwind_msan_patch(root / "source")
            run.assert_not_called()

    def test_libunwind_patch_modified_fails_before_git_apply(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            modified_patch = root / "modified.patch"
            modified_patch.write_text("not the pinned patch\n", encoding="utf-8")
            with (
                mock.patch.object(self.toolchain, "LIBUNWIND_MSAN_PATCH", modified_patch),
                mock.patch.object(self.toolchain, "run") as run,
            ):
                with self.assertRaisesRegex(self.toolchain.ToolchainError, "SHA-256 mismatch"):
                    self.toolchain.apply_libunwind_msan_patch(root / "source")
            run.assert_not_called()

    def test_libunwind_patch_apply_check_failure_does_not_apply(self):
        commands = []

        def reject_check(command):
            commands.append(command)
            raise self.toolchain.ToolchainError("patch does not apply")

        with mock.patch.object(self.toolchain, "run", side_effect=reject_check):
            with self.assertRaisesRegex(self.toolchain.ToolchainError, "patch does not apply"):
                self.toolchain.apply_libunwind_msan_patch(Path("source"))

        self.assertEqual(len(commands), 1)
        self.assertIn("--check", commands[0])

    def test_build_includes_every_runtime_registered_for_install(self):
        command = self.toolchain.build_command(Path("build"), jobs=4)
        target_index = command.index("--target")
        parallel_index = command.index("--parallel")
        self.assertEqual(
            command[target_index + 1 : parallel_index],
            ["cxx", "cxx_experimental", "cxxabi", "unwind"],
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
                [], 86, "", "WARNING: MemorySanitizer: use-of-uninitialized-value"
            )
            with (
                mock.patch.object(self.toolchain.subprocess, "run", side_effect=self.diagnostic_processes(negative_run)) as run,
                mock.patch.object(self.toolchain, "resolve_msan_symbolizer", return_value=str(symbolizer)),
            ):
                self.toolchain.verify_install(prefix, "clang++", build)

            commands = [call.args[0] for call in run.call_args_list]
            include_dir = prefix / "include/c++/v1"
            for command in (commands[0], commands[2]):
                self.assertIn("-fsanitize=memory", command)
                self.assertIn("-fsanitize-memory-track-origins=2", command)
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
            negative_environment = run.call_args_list[6].kwargs["env"]
            self.assertEqual(negative_environment["MSAN_SYMBOLIZER_PATH"], str(symbolizer))
            self.assertIn("symbolize=1", negative_environment["MSAN_OPTIONS"])
            self.assertIn("fast_unwind_on_fatal=1", negative_environment["MSAN_OPTIONS"])
            self.assertEqual(commands[3], ["clang++", "--version"])
            self.assertEqual(commands[4][0], "ldd")
            self.assertEqual(commands[5], [str(symbolizer), "--version"])

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

    def test_verified_toolchain_exports_fast_fatal_unwind_for_analysis_processes(self):
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
                mock.patch.dict(self.toolchain.os.environ, {"GITHUB_ENV": str(environment_file)}),
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

            self.assertEqual(result, 0)
            self.assertIn(
                f"MSAN_OPTIONS={self.toolchain.MSAN_ANALYSIS_OPTIONS}\n",
                environment_file.read_text(encoding="utf-8"),
            )
            identity_index = next(
                index for index, command in enumerate(commands) if command[-2:] == ["rev-parse", "HEAD"]
            )
            patch_check_index = next(
                index for index, command in enumerate(commands) if "--check" in command and "apply" in command
            )
            patch_apply_index = next(
                index
                for index, command in enumerate(commands)
                if "apply" in command and "--check" not in command
            )
            configure_index = next(
                index for index, command in enumerate(commands) if command[:2] == ["cmake", "-G"]
            )
            self.assertLess(identity_index, patch_check_index)
            self.assertLess(patch_check_index, patch_apply_index)
            self.assertLess(patch_apply_index, configure_index)

    def test_source_pin_mismatch_stops_before_patch_or_configure(self):
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
                [], 86, "", "==12345==WARNING: MemorySanitizer: use-of-uninitialized-value\n"
            )
            with (
                mock.patch.object(
                    self.toolchain.subprocess,
                    "run",
                    side_effect=self.diagnostic_processes(negative_run),
                ),
                mock.patch.object(
                    self.toolchain, "resolve_msan_symbolizer", return_value=str(symbolizer)
                ),
            ):
                self.toolchain.verify_install(prefix, "clang++", build)

    def test_install_rejects_a_toolchain_that_misses_the_probe(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, _, build, _, symbolizer = self.prepare_verification_tree(root)
            missed_probe = subprocess.CompletedProcess([], 0, "", "")
            with (
                mock.patch.object(
                    self.toolchain.subprocess,
                    "run",
                    side_effect=self.diagnostic_processes(missed_probe),
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
                [], 1, "", "WARNING: MemorySanitizer: use-of-uninitialized-value"
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
                *self.diagnostic_processes(subprocess.CompletedProcess([], 0, "", ""))[:6],
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
                    "    #1 0x7f0000001234 in operator new (libc++.so+0x1234)\n"
                    "MemorySanitizer:DEADLYSIGNAL",
                ),
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
            "--obj=/tmp/prefix/lib/libc++.so",
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
