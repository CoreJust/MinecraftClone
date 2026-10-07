from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


OVERLAY = Path(__file__).resolve().parents[1] / "ci/vcpkg-overlays/gmp/portfile.cmake"


class GmpVcpkgOverlayTests(unittest.TestCase):
    def test_download_prefers_verified_mirror_and_preserves_integrity_pin(self):
        overlay = OVERLAY.read_text(encoding="utf-8")
        mirror = '"https://mirrors.kernel.org/gnu/gmp/gmp-${VERSION}.tar.xz"'
        gnu_mirror = '"https://ftpmirror.gnu.org/gmp/gmp-${VERSION}.tar.xz"'
        ftp_mirror = '"https://ftp.gnu.org/gnu/gmp/gmp-${VERSION}.tar.xz"'
        gmplib_mirror = '"https://gmplib.org/download/gmp/gmp-${VERSION}.tar.xz"'
        sha512 = "e85a0dab5195889948a3462189f0e0598d331d3457612e2d3350799dba2e244316d256f8161df5219538eb003e4b5343f989aaa00f96321559063ed8c8f29fd2"

        self.assertIn(mirror, overlay)
        self.assertLess(overlay.index(mirror), overlay.index(gnu_mirror))
        self.assertLess(overlay.index(gnu_mirror), overlay.index(ftp_mirror))
        self.assertLess(overlay.index(ftp_mirror), overlay.index(gmplib_mirror))
        self.assertIn(f"SHA512 {sha512}", overlay)

    def test_android_autoconf_host_is_supplied_only_for_android_targets(self):
        overlay = OVERLAY.read_text(encoding="utf-8")
        android_guard = "if(VCPKG_TARGET_IS_ANDROID)\n    list(APPEND build_triplet_options BUILD_TRIPLET \"--host=aarch64-linux-android\")\nendif()"
        self.assertIn(android_guard, overlay)
        self.assertIn("    ${build_triplet_options}\n    ADDITIONAL_MSYS_PACKAGES", overlay)

    def test_delivered_msan_patch_applies_and_preserves_the_probe(self):
        source_context = '''int main (void) { return 0; }
EOF
  echo "Test compile: [$2]" >&AC_FD_CC
  gmp_cxxcompile="$1 conftest.cc >&AC_FD_CC"
  if AC_TRY_EVAL(gmp_cxxcompile); then
    if test "$cross_compiling" = no; then
      if AC_TRY_COMMAND([./a.out || ./b.out || ./a.exe || ./a_out.exe || ./conftest]); then :;
'''
        patch = OVERLAY.parent / "msan-cxx-ldflags.patch"
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "acinclude.m4"
            source.write_text(source_context, encoding="utf-8", newline="\n")
            result = subprocess.run(
                ["git", "apply", str(patch)],
                cwd=temporary,
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(
                source.read_text(encoding="utf-8"),
                source_context.replace(
                    '$1 conftest.cc >&AC_FD_CC', '$1 conftest.cc $LDFLAGS >&AC_FD_CC'
                ),
            )

    def test_msan_cxx_link_flags_patch_is_selected_only_for_its_triplet(self):
        standard_patches = [
            "asmflags.patch",
            "cross-tools.patch",
            "subdirs.patch",
            "msvc_symbol.patch",
            "arm64-coff.patch",
            "remove_compiler_info.patch",
            "c23.patch",
        ]
        expected_patches = {
            "x64-linux-msan": standard_patches + ["msan-cxx-ldflags.patch"],
            "x64-linux-lsan": standard_patches,
            "arm64-osx": standard_patches,
            "x64-windows": standard_patches,
            "arm64-android": standard_patches,
        }

        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            script = temporary / "select_gmp_patches.cmake"
            capture = temporary / "patches.txt"
            script.write_text(
                """
function(vcpkg_download_distfile output_var)
    set(${output_var} "unused-archive" PARENT_SCOPE)
endfunction()
function(vcpkg_extract_source_archive output_var)
    cmake_parse_arguments(PARSE_ARGV 1 EXTRACT "" "ARCHIVE;SOURCE_BASE" "PATCHES")
    file(WRITE "${CAPTURE_FILE}" "${EXTRACT_PATCHES}")
endfunction()
file(READ "${PORTFILE}" portfile)
string(FIND "${portfile}" "vcpkg_list(SET OPTIONS)" configure_start)
if(configure_start EQUAL -1)
    message(FATAL_ERROR "GMP portfile configure boundary is missing")
endif()
string(SUBSTRING "${portfile}" 0 "${configure_start}" portfile_prefix)
cmake_language(EVAL CODE "${portfile_prefix}")
""",
                encoding="utf-8",
            )
            for triplet, expected in expected_patches.items():
                with self.subTest(triplet=triplet):
                    result = subprocess.run(
                        [
                            "cmake",
                            f"-DPORTFILE={OVERLAY}",
                            f"-DCAPTURE_FILE={capture}",
                            f"-DCURRENT_INSTALLED_DIR={temporary / 'installed'}",
                            "-DPORT=gmp",
                            "-DVERSION=6.3.0",
                            f"-DTARGET_TRIPLET={triplet}",
                            "-P",
                            str(script),
                        ],
                        check=False,
                        capture_output=True,
                        text=True,
                    )
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(capture.read_text(encoding="utf-8").split(";"), expected)

    def test_configure_disables_assembly_only_for_msan_and_existing_windows_fallback(self):
        scenarios = {
            "x64-linux-msan": {
                "VCPKG_TARGET_IS_LINUX": "ON",
                "VCPKG_TARGET_ARCHITECTURE": "x64",
                "VCPKG_DETECTED_CMAKE_C_COMPILER_ID": "Clang",
                "VCPKG_DETECTED_CMAKE_C_COMPILER": "/toolchain/bin/clang",
                "assembly_disabled": True,
                "ccas": "CCAS=",
                "asmflags": "ASMFLAGS=-c",
            },
            "x64-linux-lsan": {
                "VCPKG_TARGET_IS_LINUX": "ON",
                "VCPKG_TARGET_ARCHITECTURE": "x64",
                "VCPKG_DETECTED_CMAKE_C_COMPILER_ID": "Clang",
                "VCPKG_DETECTED_CMAKE_C_COMPILER": "/toolchain/bin/clang",
                "assembly_disabled": False,
                "ccas": "CCAS=clang",
                "asmflags": "ASMFLAGS=-c",
            },
            "arm64-osx": {
                "VCPKG_TARGET_ARCHITECTURE": "arm64",
                "VCPKG_DETECTED_CMAKE_C_COMPILER_ID": "AppleClang",
                "VCPKG_DETECTED_CMAKE_C_COMPILER": "/toolchain/bin/clang",
                "assembly_disabled": False,
                "ccas": "CCAS=clang",
                "asmflags": "ASMFLAGS=-c",
            },
            "x64-windows": {
                "VCPKG_TARGET_IS_WINDOWS": "ON",
                "VCPKG_TARGET_ARCHITECTURE": "x64",
                "VCPKG_DETECTED_CMAKE_C_COMPILER_ID": "MSVC",
                "VCPKG_DETECTED_CMAKE_C_COMPILER": "cl.exe",
                "assembly_disabled": False,
                "ccas": "CCAS=clang",
                "asmflags": "ASMFLAGS=-c --target=x86_64-pc-windows-msvc",
            },
            "arm64-android": {
                "VCPKG_TARGET_IS_ANDROID": "ON",
                "VCPKG_TARGET_ARCHITECTURE": "arm64",
                "VCPKG_DETECTED_CMAKE_C_COMPILER_ID": "Clang",
                "VCPKG_DETECTED_CMAKE_C_COMPILER": "/toolchain/bin/clang",
                "assembly_disabled": False,
                "ccas": "CCAS=clang",
                "asmflags": "ASMFLAGS=-c",
                "host": "--host=aarch64-linux-android",
            },
            "arm-windows": {
                "VCPKG_TARGET_IS_WINDOWS": "ON",
                "VCPKG_TARGET_ARCHITECTURE": "arm",
                "VCPKG_DETECTED_CMAKE_C_COMPILER_ID": "MSVC",
                "VCPKG_DETECTED_CMAKE_C_COMPILER": "cl.exe",
                "assembly_disabled": True,
                "ccas": "CCAS=",
                "asmflags": "ASMFLAGS=-c",
            },
        }
        portfile_script = '''cmake_minimum_required(VERSION 3.25)
function(vcpkg_download_distfile output_var)
    set(${output_var} "${TEST_TEMP}/archive" PARENT_SCOPE)
endfunction()
function(vcpkg_extract_source_archive output_var)
    set(${output_var} "${TEST_TEMP}/source" PARENT_SCOPE)
endfunction()
macro(vcpkg_list operation variable)
    if("${operation}" STREQUAL "SET")
        set(${variable} "")
    elseif("${operation}" STREQUAL "APPEND")
        list(APPEND ${variable} ${ARGN})
    else()
        message(FATAL_ERROR "Unexpected vcpkg_list operation: ${operation}")
    endif()
endmacro()
function(vcpkg_cmake_get_vars output_var)
    file(WRITE "${TEST_TEMP}/cmake-vars.cmake"
        "set(VCPKG_DETECTED_CMAKE_C_COMPILER_ID \\\"${VCPKG_DETECTED_CMAKE_C_COMPILER_ID}\\\")")
    set(${output_var} "${TEST_TEMP}/cmake-vars.cmake" PARENT_SCOPE)
endfunction()
function(vcpkg_find_acquire_program program)
    set(${program} "/tools/clang/bin/clang" PARENT_SCOPE)
endfunction()
function(vcpkg_add_to_path directory)
endfunction()
function(vcpkg_configure_make)
    file(WRITE "${CAPTURE_FILE}" "${ARGN}")
endfunction()
file(READ "${PORTFILE}" portfile)
string(FIND "${portfile}" "\\nvcpkg_install_make()" configure_end)
if(configure_end EQUAL -1)
    message(FATAL_ERROR "GMP portfile configure boundary is missing")
endif()
string(SUBSTRING "${portfile}" 0 "${configure_end}" configure_portfile)
cmake_language(EVAL CODE "${configure_portfile}")
'''

        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            script = temporary / "capture_gmp_configure.cmake"
            capture = temporary / "configure-args.txt"
            script.write_text(portfile_script, encoding="utf-8")
            for triplet, scenario in scenarios.items():
                with self.subTest(triplet=triplet):
                    command = [
                        "cmake",
                        f"-DPORTFILE={OVERLAY}",
                        f"-DTEST_TEMP={temporary}",
                        f"-DCAPTURE_FILE={capture}",
                        f"-DCURRENT_INSTALLED_DIR={temporary / 'installed'}",
                        f"-DTARGET_TRIPLET={triplet}",
                        "-DPORT=gmp",
                        "-DVERSION=6.3.0",
                        f"-DVCPKG_DETECTED_CMAKE_C_COMPILER_ID={scenario['VCPKG_DETECTED_CMAKE_C_COMPILER_ID']}",
                        f"-DVCPKG_DETECTED_CMAKE_C_COMPILER={scenario['VCPKG_DETECTED_CMAKE_C_COMPILER']}",
                        f"-DVCPKG_TARGET_ARCHITECTURE={scenario['VCPKG_TARGET_ARCHITECTURE']}",
                        "-DVCPKG_TARGET_IS_WINDOWS=OFF",
                        "-DVCPKG_TARGET_IS_MINGW=OFF",
                        "-DVCPKG_TARGET_IS_LINUX=OFF",
                        "-DVCPKG_TARGET_IS_ANDROID=OFF",
                        "-DVCPKG_CROSSCOMPILING=OFF",
                        "-DVCPKG_LIBRARY_LINKAGE=static",
                        "-P",
                        str(script),
                    ]
                    for key in (
                        "VCPKG_TARGET_IS_WINDOWS",
                        "VCPKG_TARGET_IS_LINUX",
                        "VCPKG_TARGET_IS_ANDROID",
                    ):
                        if key in scenario:
                            command[command.index(f"-D{key}=OFF")] = f"-D{key}={scenario[key]}"
                    if triplet == "arm64-android":
                        command[command.index("-DVCPKG_CROSSCOMPILING=OFF")] = "-DVCPKG_CROSSCOMPILING=ON"

                    result = subprocess.run(
                        command,
                        check=False,
                        capture_output=True,
                        text=True,
                    )
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    configure_args = capture.read_text(encoding="utf-8").split(";")
                    if scenario["assembly_disabled"]:
                        self.assertIn("--enable-assembly=no", configure_args)
                    else:
                        self.assertNotIn("--enable-assembly=no", configure_args)
                    self.assertIn(scenario["ccas"], configure_args)
                    for option in (
                        "--enable-cxx",
                        "--with-pic",
                        "--with-readline=no",
                        scenario["asmflags"],
                        "gmp_cv_prog_exeext_for_build=",
                    ):
                        self.assertIn(option, configure_args)
                    if scenario.get("VCPKG_TARGET_IS_WINDOWS") == "ON":
                        for option in (
                            "ac_cv_func_memset=yes",
                            "gmp_cv_asm_w32=.word",
                            "gmp_cv_check_libm_for_build=no",
                        ):
                            self.assertIn(option, configure_args)
                    if "host" in scenario:
                        self.assertIn("BUILD_TRIPLET", configure_args)
                        self.assertIn(scenario["host"], configure_args)
                    else:
                        self.assertNotIn("--host=aarch64-linux-android", configure_args)


if __name__ == "__main__":
    unittest.main()
