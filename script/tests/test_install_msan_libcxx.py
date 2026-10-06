from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
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
            prefix = root / "prefix"
            include_dir = prefix / "include/c++/v1"
            library_dir = prefix / "lib"
            build = root / "build"
            include_dir.mkdir(parents=True)
            library_dir.mkdir(parents=True)
            build.mkdir()
            (library_dir / "libc++.so").touch()
            completed = [
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 86, "", "WARNING: MemorySanitizer: use-of-uninitialized-value"),
            ]
            with mock.patch.object(self.toolchain.subprocess, "run", side_effect=completed) as run:
                self.toolchain.verify_install(prefix, "clang++", build)

            commands = [call.args[0] for call in run.call_args_list]
            for command in (commands[0], commands[2]):
                self.assertIn("-fsanitize=memory", command)
                self.assertIn("-fsanitize-memory-track-origins=2", command)
                self.assertIn("-stdlib=libc++", command)
                self.assertIn(str(include_dir), " ".join(command))
                self.assertIn(str(library_dir), " ".join(command))
            negative_source = run.call_args_list[2].kwargs["input"]
            self.assertIn("std::malloc(sizeof(int))", negative_source)
            self.assertIn("volatile int", negative_source)
            self.assertIn(
                "if (*static_cast<volatile int*>(value) == 0)",
                negative_source,
            )
            self.assertNotIn("return result;", negative_source)

    def test_install_rejects_a_toolchain_that_misses_the_probe(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / "prefix"
            (prefix / "include/c++/v1").mkdir(parents=True)
            library_dir = prefix / "lib"
            library_dir.mkdir()
            (library_dir / "libc++.so").touch()
            build = root / "build"
            build.mkdir()
            completed = [
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 0, "", ""),
            ]
            with mock.patch.object(self.toolchain.subprocess, "run", side_effect=completed):
                with self.assertRaisesRegex(self.toolchain.ToolchainError, "did not diagnose"):
                    self.toolchain.verify_install(prefix, "clang++", build)

    def test_negative_probe_failure_reports_child_exit_and_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / "prefix"
            (prefix / "include/c++/v1").mkdir(parents=True)
            library_dir = prefix / "lib"
            library_dir.mkdir()
            (library_dir / "libc++.so").touch()
            build = root / "build"
            build.mkdir()
            completed = [
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess(
                    [],
                    -11,
                    "probe stdout",
                    "MemorySanitizer: stack-overflow\nMemorySanitizer:DEADLYSIGNAL",
                ),
            ]
            with mock.patch.object(self.toolchain.subprocess, "run", side_effect=completed):
                with self.assertRaisesRegex(self.toolchain.ToolchainError, "did not diagnose") as raised:
                    self.toolchain.verify_install(prefix, "clang++", build)

        message = str(raised.exception)
        self.assertIn("return code: -11", message)
        self.assertIn("msan-uninitialized-read", message)
        self.assertIn("stdout:\nprobe stdout", message)
        self.assertIn("stderr:\nMemorySanitizer: stack-overflow", message)


if __name__ == "__main__":
    unittest.main()
