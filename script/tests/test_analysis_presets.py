from __future__ import annotations

import json
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
PRESETS_PATH = REPOSITORY / "CMakePresets.json"


def effective_cache_variables(preset_name: str) -> dict[str, object]:
    presets = json.loads(PRESETS_PATH.read_text(encoding="utf-8"))["configurePresets"]
    presets_by_name = {preset["name"]: preset for preset in presets}

    def resolve(name: str) -> dict[str, object]:
        preset = presets_by_name[name]
        inherited = preset.get("inherits", [])
        parents = [inherited] if isinstance(inherited, str) else inherited
        cache_variables: dict[str, object] = {}
        for parent in parents:
            cache_variables.update(resolve(parent))
        cache_variables.update(preset.get("cacheVariables", {}))
        return cache_variables

    return resolve(preset_name)


class AnalysisPresetTests(unittest.TestCase):
    def test_standard_presets_install_vcpkg_packages_inside_their_build_tree(self):
        expected = "${sourceDir}/build/${presetName}/vcpkg_installed"

        for preset_name in ("debug", "release", "renderer-smoke"):
            with self.subTest(preset=preset_name):
                self.assertEqual(
                    effective_cache_variables(preset_name).get("VCPKG_INSTALLED_DIR"),
                    expected,
                )

    def test_macos_sanitizer_presets_inherit_upstream_clang_selection(self):
        expected_sanitizers = {
            "analysis-macos-asan": "address",
            "analysis-macos-ubsan": "undefined",
            "analysis-macos-tsan": "thread",
        }

        for preset_name, sanitizer in expected_sanitizers.items():
            with self.subTest(preset=preset_name):
                cache_variables = effective_cache_variables(preset_name)

                self.assertEqual(cache_variables.get("CMAKE_C_COMPILER"), "clang")
                self.assertEqual(cache_variables.get("CMAKE_CXX_COMPILER"), "clang++")
                self.assertEqual(cache_variables.get("MC_ENABLE_SANITIZERS"), "ON")
                self.assertEqual(cache_variables.get("MC_SANITIZER"), sanitizer)

    def test_standard_and_windows_presets_keep_their_compiler_defaults(self):
        preset_names = (
            "debug",
            "release",
            "renderer-smoke",
            "analysis-windows-asan",
            "analysis-windows-analyze",
        )

        for preset_name in preset_names:
            with self.subTest(preset=preset_name):
                cache_variables = effective_cache_variables(preset_name)

                self.assertNotIn("CMAKE_C_COMPILER", cache_variables)
                self.assertNotIn("CMAKE_CXX_COMPILER", cache_variables)

    def test_linux_sanitizer_presets_keep_explicit_clang_selection(self):
        for preset_name in ("analysis-linux-lsan", "analysis-linux-msan"):
            with self.subTest(preset=preset_name):
                cache_variables = effective_cache_variables(preset_name)

                self.assertEqual(cache_variables.get("CMAKE_C_COMPILER"), "clang")
                self.assertEqual(cache_variables.get("CMAKE_CXX_COMPILER"), "clang++")

    def test_windows_analyzer_resolves_pinned_private_package_paths(self):
        cache_variables = effective_cache_variables("analysis-windows-analyze")

        self.assertEqual(cache_variables.get("VCPKG_INSTALLED_DIR"), "$env{VCPKG_INSTALLED_DIR}")
        self.assertEqual(cache_variables.get("CMAKE_PREFIX_PATH"), "$env{MC_PRIVATE_DEPENDENCIES_PREFIX}")
        self.assertEqual(
            cache_variables.get("CoreCpp_DIR"),
            "$env{MC_PRIVATE_DEPENDENCIES_PREFIX}/lib/cmake/CoreCpp",
        )
        self.assertEqual(
            cache_variables.get("CoreProject2026_DIR"),
            "$env{MC_PRIVATE_DEPENDENCIES_PREFIX}/lib/cmake/CoreProject2026",
        )


if __name__ == "__main__":
    unittest.main()
