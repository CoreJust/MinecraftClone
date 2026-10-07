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


if __name__ == "__main__":
    unittest.main()
