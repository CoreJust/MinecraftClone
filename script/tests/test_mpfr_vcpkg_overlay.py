from __future__ import annotations

import json
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
OVERLAY = REPOSITORY / "script/ci/vcpkg-overlays/mpfr"


class MpfrVcpkgOverlayTests(unittest.TestCase):
    def test_overlay_keeps_the_pinned_mpfr_port_identity(self):
        manifest = json.loads((OVERLAY / "vcpkg.json").read_text(encoding="utf-8"))
        configuration = json.loads((REPOSITORY / "vcpkg-configuration.json").read_text(encoding="utf-8"))

        self.assertEqual(
            (manifest["name"], manifest["version"], manifest["port-version"]),
            ("mpfr", "4.2.2", 1),
        )
        self.assertEqual(
            manifest["dependencies"],
            ["gmp", {"name": "vcpkg-make", "host": True}],
        )
        self.assertEqual(
            configuration["default-registry"]["baseline"],
            "2b65c20fc66eda893aa15a15a453c3cf09500b19",
        )

    def test_pic_is_requested_only_for_android(self):
        portfile = (OVERLAY / "portfile.cmake").read_text(encoding="utf-8")

        self.assertIn(
            "if(VCPKG_TARGET_IS_ANDROID)\n"
            "    list(APPEND VCPKG_MAKE_CONFIGURE_OPTIONS --with-pic)\n"
            "endif()",
            portfile,
        )

    def test_upstream_patch_resolution_uses_and_restores_the_pinned_port_directory(self):
        portfile = (OVERLAY / "portfile.cmake").read_text(encoding="utf-8")

        self.assertIn(
            'set(_MC_MPFR_OVERLAY_PORT_DIR "${CURRENT_PORT_DIR}")\n'
            'set(CURRENT_PORT_DIR "${VCPKG_ROOT_DIR}/ports/mpfr")\n'
            'include("${VCPKG_ROOT_DIR}/ports/mpfr/portfile.cmake")\n'
            'set(CURRENT_PORT_DIR "${_MC_MPFR_OVERLAY_PORT_DIR}")\n'
            "unset(_MC_MPFR_OVERLAY_PORT_DIR)",
            portfile,
        )

    def test_hwasan_build_installs_and_uses_the_mpfr_overlay(self):
        acquire = (REPOSITORY / "script/ci/acquire.py").read_text(encoding="utf-8")
        gradle = (REPOSITORY / "android/app/build.gradle").read_text(encoding="utf-8")

        self.assertIn('"--overlay-ports={Path(__file__).resolve().parent / \'vcpkg-overlays\'}"', acquire)
        self.assertIn("def vcpkgOverlayPorts = file('../../script/ci/vcpkg-overlays')", gradle)
        self.assertIn('"-DVCPKG_OVERLAY_PORTS=${vcpkgOverlayPorts.absolutePath}"', gradle)


if __name__ == "__main__":
    unittest.main()
