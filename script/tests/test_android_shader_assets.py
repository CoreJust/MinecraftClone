import re
import unittest
from pathlib import Path


class AndroidShaderAssetTests(unittest.TestCase):
    ROOT = Path(__file__).resolve().parents[2]
    REQUIRED_SOURCES = (
        "debug_hud.frag",
        "debug_hud.vert",
        "grid.vert",
        "player.vert",
        "trivial.frag",
    )
    REQUIRED_ASSETS = tuple(f"{source}.spv" for source in REQUIRED_SOURCES)

    def test_gradle_stages_all_sources_and_verifies_packaged_spirv(self):
        build_gradle = (self.ROOT / "android/app/build.gradle").read_text(encoding="utf-8")
        stage_match = re.search(
            r"def stageFallbackShaders = tasks\.register\('stageFallbackShaders', Sync\) \{(?P<body>.*?)\n\s*\}",
            build_gradle,
            re.DOTALL,
        )
        self.assertIsNotNone(stage_match)
        stage_body = stage_match.group("body")
        for source in self.REQUIRED_SOURCES:
            self.assertRegex(stage_body, rf"include ['\"]{re.escape(source)}['\"]")

        self.assertIn("assets/shaders/${asset}", build_gradle)
        for asset in self.REQUIRED_ASSETS:
            self.assertIn(f"'{asset}'", build_gradle)
        self.assertIn("name == 'assembleDebug'", build_gradle)
        self.assertIn("finalizedBy(verifyDebugShaderAssets)", build_gradle)
        self.assertIn("-DVCPKG_MANIFEST_INSTALL=${vcpkgManifestInstall}", build_gradle)
        self.assertIn("orElse('ON')", build_gradle)
        self.assertIn("-DVCPKG_INSTALLED_DIR=${file(vcpkgInstalledDir).absolutePath}", build_gradle)
        self.assertIn("-DCMAKE_PREFIX_PATH=${cmakePrefixPath}", build_gradle)
        self.assertIn("-DCoreCpp_DIR=${file(coreCppDir).absolutePath}", build_gradle)
        self.assertIn("-DCoreProject2026_DIR=${file(coreProjectDir).absolutePath}", build_gradle)


if __name__ == "__main__":
    unittest.main()
