from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
TOOLCHAIN = REPOSITORY / "script/ci/vcpkg-clang-toolchain.cmake"


class VcpkgClangToolchainTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        self.pinned_vcpkg_root = os.environ.get("VCPKG_ROOT")
        self.cc = self.root / "clang"
        self.cxx = self.root / "clang++"
        self.cc.touch()
        self.cxx.touch()
        self.driver = self.root / "probe.cmake"
        self.environment = os.environ.copy()
        self.environment.update({"CC": str(self.cc), "CXX": str(self.cxx)})
        self.environment.pop("VCPKG_ROOT", None)

    def tearDown(self):
        self.temp_dir.cleanup()

    def prepare_vcpkg(self):
        vcpkg_root = self.root / "vcpkg"
        toolchains = vcpkg_root / "scripts/toolchains"
        toolchains.mkdir(parents=True)
        linux_toolchain = toolchains / "linux.cmake"
        linux_toolchain.write_text(
            "file(TO_CMAKE_PATH \"$ENV{CC}\" expected_c_compiler)\n"
            "file(TO_CMAKE_PATH \"$ENV{CXX}\" expected_cxx_compiler)\n"
            "if(NOT CMAKE_C_COMPILER STREQUAL expected_c_compiler)\n"
            "    message(FATAL_ERROR \"C compiler selection was lost\")\n"
            "endif()\n"
            "if(NOT CMAKE_CXX_COMPILER STREQUAL expected_cxx_compiler)\n"
            "    message(FATAL_ERROR \"C++ compiler selection was lost\")\n"
            "endif()\n"
            "string(APPEND CMAKE_C_FLAGS_INIT \" ${VCPKG_C_FLAGS}\")\n"
            "string(APPEND CMAKE_CXX_FLAGS_INIT \" ${VCPKG_CXX_FLAGS}\")\n"
            "string(APPEND CMAKE_MODULE_LINKER_FLAGS_INIT \" ${VCPKG_LINKER_FLAGS}\")\n"
            "string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT \" ${VCPKG_LINKER_FLAGS}\")\n"
            "string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT \" ${VCPKG_LINKER_FLAGS}\")\n"
            "list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES\n"
            "    VCPKG_C_FLAGS VCPKG_CXX_FLAGS VCPKG_LINKER_FLAGS)\n",
            encoding="utf-8",
        )
        self.environment["VCPKG_ROOT"] = str(vcpkg_root)
        return vcpkg_root

    def configure_probe(self, *, vcpkg_root=None):
        if vcpkg_root is not None:
            self.environment["VCPKG_ROOT"] = str(vcpkg_root)
        self.driver.write_text(
            "cmake_minimum_required(VERSION 3.25)\n"
            "set(VCPKG_C_FLAGS \"-fsanitize=memory\")\n"
            "set(VCPKG_CXX_FLAGS \"-fsanitize=memory -stdlib=libc++\")\n"
            "set(VCPKG_LINKER_FLAGS \"-fsanitize=memory -stdlib=libc++\")\n"
            f'include("{TOOLCHAIN.as_posix()}")\n'
            "file(TO_CMAKE_PATH \"$ENV{CC}\" expected_c_compiler)\n"
            "file(TO_CMAKE_PATH \"$ENV{CXX}\" expected_cxx_compiler)\n"
            "if(NOT CMAKE_C_COMPILER STREQUAL expected_c_compiler)\n"
            "    message(FATAL_ERROR \"C compiler selection was lost\")\n"
            "endif()\n"
            "if(NOT CMAKE_CXX_COMPILER STREQUAL expected_cxx_compiler)\n"
            "    message(FATAL_ERROR \"C++ compiler selection was lost\")\n"
            "endif()\n"
            "string(FIND \"${CMAKE_C_FLAGS_INIT}\" \"-fsanitize=memory\" _c_sanitize_index)\n"
            "string(FIND \"${CMAKE_CXX_FLAGS_INIT}\" \"-fsanitize=memory\" _cxx_sanitize_index)\n"
            "string(FIND \"${CMAKE_CXX_FLAGS_INIT}\" \"-stdlib=libc++\" _cxx_stdlib_index)\n"
            "string(FIND \"${CMAKE_MODULE_LINKER_FLAGS_INIT}\" \"-fsanitize=memory\" _module_link_sanitize_index)\n"
            "string(FIND \"${CMAKE_MODULE_LINKER_FLAGS_INIT}\" \"-stdlib=libc++\" _module_link_stdlib_index)\n"
            "string(FIND \"${CMAKE_SHARED_LINKER_FLAGS_INIT}\" \"-fsanitize=memory\" _shared_link_sanitize_index)\n"
            "string(FIND \"${CMAKE_SHARED_LINKER_FLAGS_INIT}\" \"-stdlib=libc++\" _shared_link_stdlib_index)\n"
            "string(FIND \"${CMAKE_EXE_LINKER_FLAGS_INIT}\" \"-fsanitize=memory\" _link_sanitize_index)\n"
            "string(FIND \"${CMAKE_EXE_LINKER_FLAGS_INIT}\" \"-stdlib=libc++\" _link_stdlib_index)\n"
            "if(_c_sanitize_index EQUAL -1 OR _cxx_sanitize_index EQUAL -1\n"
            "        OR _cxx_stdlib_index EQUAL -1 OR _module_link_sanitize_index EQUAL -1\n"
            "        OR _module_link_stdlib_index EQUAL -1 OR _shared_link_sanitize_index EQUAL -1\n"
            "        OR _shared_link_stdlib_index EQUAL -1 OR _link_sanitize_index EQUAL -1\n"
            "        OR _link_stdlib_index EQUAL -1)\n"
            "    message(FATAL_ERROR \"vcpkg Linux initialization omitted sanitizer flags\")\n"
            "endif()\n"
            "foreach(_required_variable IN ITEMS VCPKG_C_FLAGS VCPKG_CXX_FLAGS VCPKG_LINKER_FLAGS)\n"
            "    if(NOT _required_variable IN_LIST CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)\n"
            "        message(FATAL_ERROR \"try-compile context omitted ${_required_variable}\")\n"
            "    endif()\n"
            "endforeach()\n"
            "message(STATUS \"CMAKE_C_COMPILER=${CMAKE_C_COMPILER}\")\n"
            "message(STATUS \"CMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}\")\n"
            "message(STATUS \"CMAKE_C_FLAGS_INIT=${CMAKE_C_FLAGS_INIT}\")\n"
            "message(STATUS \"CMAKE_CXX_FLAGS_INIT=${CMAKE_CXX_FLAGS_INIT}\")\n"
            "message(STATUS \"CMAKE_MODULE_LINKER_FLAGS_INIT=${CMAKE_MODULE_LINKER_FLAGS_INIT}\")\n"
            "message(STATUS \"CMAKE_SHARED_LINKER_FLAGS_INIT=${CMAKE_SHARED_LINKER_FLAGS_INIT}\")\n"
            "message(STATUS \"CMAKE_EXE_LINKER_FLAGS_INIT=${CMAKE_EXE_LINKER_FLAGS_INIT}\")\n"
            "message(STATUS \"CMAKE_TRY_COMPILE_PLATFORM_VARIABLES=${CMAKE_TRY_COMPILE_PLATFORM_VARIABLES}\")\n",
            encoding="utf-8",
        )

    def run_probe(self):
        return subprocess.run(
            ["cmake", "-P", str(self.driver)],
            check=False,
            capture_output=True,
            text=True,
            env=self.environment,
        )

    def test_delegates_to_linux_toolchain_and_preserves_sanitizer_context(self):
        self.prepare_vcpkg()
        self.configure_probe()

        result = self.run_probe()

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"CMAKE_C_COMPILER={self.cc.as_posix()}", result.stdout)
        self.assertIn(f"CMAKE_CXX_COMPILER={self.cxx.as_posix()}", result.stdout)
        self.assertIn("CMAKE_C_FLAGS_INIT= -fsanitize=memory", result.stdout)
        self.assertIn("CMAKE_CXX_FLAGS_INIT= -fsanitize=memory -stdlib=libc++", result.stdout)
        self.assertIn("CMAKE_MODULE_LINKER_FLAGS_INIT= -fsanitize=memory -stdlib=libc++", result.stdout)
        self.assertIn("CMAKE_SHARED_LINKER_FLAGS_INIT= -fsanitize=memory -stdlib=libc++", result.stdout)
        self.assertIn("CMAKE_EXE_LINKER_FLAGS_INIT= -fsanitize=memory -stdlib=libc++", result.stdout)
        self.assertIn("VCPKG_C_FLAGS;VCPKG_CXX_FLAGS;VCPKG_LINKER_FLAGS", result.stdout)

    def test_requires_compiler_files(self):
        self.prepare_vcpkg()
        self.configure_probe()

        for variable in ("CC", "CXX"):
            with self.subTest(variable=variable):
                self.environment[variable] = str(self.root / f"missing-{variable}")
                result = self.run_probe()

                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Linux analysis CC and CXX must resolve to compiler files", result.stderr)
                self.environment[variable] = str(self.cc if variable == "CC" else self.cxx)

    def test_requires_both_compiler_environment_variables(self):
        self.prepare_vcpkg()
        self.configure_probe()

        for variable in ("CC", "CXX"):
            with self.subTest(variable=variable):
                self.environment.pop(variable, None)
                result = self.run_probe()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Clang CC and CXX must be set", result.stderr)
                self.environment[variable] = str(self.cc if variable == "CC" else self.cxx)

    def test_rejects_directories_as_compiler_files(self):
        self.prepare_vcpkg()
        self.configure_probe()

        for variable in ("CC", "CXX"):
            with self.subTest(variable=variable):
                self.environment[variable] = str(self.root)
                result = self.run_probe()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Linux analysis CC and CXX must resolve to compiler files", result.stderr)
                self.environment[variable] = str(self.cc if variable == "CC" else self.cxx)

    def test_requires_vcpkg_root(self):
        self.configure_probe()

        result = self.run_probe()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("VCPKG_ROOT must identify the pinned vcpkg checkout", result.stderr)

    def test_requires_pinned_linux_toolchain_file(self):
        vcpkg_root = self.root / "vcpkg"
        vcpkg_root.mkdir()
        self.configure_probe(vcpkg_root=vcpkg_root)

        result = self.run_probe()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Pinned vcpkg Linux toolchain is missing", result.stderr)

    def test_pinned_linux_toolchain_initializes_flags_and_try_compile_context(self):
        if not self.pinned_vcpkg_root:
            self.skipTest("VCPKG_ROOT is not set to an available pinned vcpkg checkout")
        vcpkg_root = Path(self.pinned_vcpkg_root)
        if not vcpkg_root.is_dir():
            self.skipTest("VCPKG_ROOT is not set to an available pinned vcpkg checkout")
        self.assertTrue((vcpkg_root / "scripts/toolchains/linux.cmake").is_file())
        self.configure_probe(vcpkg_root=vcpkg_root)

        result = self.run_probe()

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("CMAKE_C_FLAGS_INIT= -fPIC -fsanitize=memory", result.stdout)
        self.assertIn("CMAKE_CXX_FLAGS_INIT= -fPIC -fsanitize=memory -stdlib=libc++", result.stdout)
        self.assertIn("CMAKE_MODULE_LINKER_FLAGS_INIT= -fsanitize=memory -stdlib=libc++", result.stdout)
        self.assertIn("CMAKE_SHARED_LINKER_FLAGS_INIT= -fsanitize=memory -stdlib=libc++", result.stdout)
        self.assertIn("CMAKE_EXE_LINKER_FLAGS_INIT= -fsanitize=memory -stdlib=libc++", result.stdout)
        print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
