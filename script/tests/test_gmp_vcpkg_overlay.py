from __future__ import annotations

import unittest
from pathlib import Path


OVERLAY = Path(__file__).resolve().parents[1] / "ci/vcpkg-overlays/gmp/portfile.cmake"


class GmpVcpkgOverlayTests(unittest.TestCase):
    def test_android_autoconf_host_is_supplied_only_for_android_targets(self):
        overlay = OVERLAY.read_text(encoding="utf-8")
        android_guard = "if(VCPKG_TARGET_IS_ANDROID)\n    list(APPEND build_triplet_options BUILD_TRIPLET \"--host=aarch64-linux-android\")\nendif()"
        self.assertIn(android_guard, overlay)
        self.assertIn("    ${build_triplet_options}\n    ADDITIONAL_MSYS_PACKAGES", overlay)


if __name__ == "__main__":
    unittest.main()
