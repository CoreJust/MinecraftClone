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
        self.assertEqual(self.toolchain.LLVM_TAG, "llvmorg-22.1.7")
        self.assertEqual(len(self.toolchain.LLVM_COMMIT), 40)
        self.assertIn("-DLLVM_USE_SANITIZER=MemoryWithOrigins", command)
        self.assertIn("-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi;libunwind", command)
        self.assertIn("-DLIBCXX_USE_COMPILER_RT=ON", command)

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
            self.assertTrue(all("-fsanitize=memory" in command for command in commands[:1]))
            self.assertIn(str(include_dir), " ".join(commands[0]))
            self.assertIn(str(library_dir), " ".join(commands[0]))

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


if __name__ == "__main__":
    unittest.main()
