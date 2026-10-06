from __future__ import annotations

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


if __name__ == "__main__":
    unittest.main()
