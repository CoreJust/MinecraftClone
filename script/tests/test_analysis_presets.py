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

    def test_windows_asan_preset_selects_release_instrumented_dependency_closure(self):
        cache_variables = effective_cache_variables("analysis-windows-asan")

        self.assertEqual(cache_variables.get("CMAKE_BUILD_TYPE"), "Release")
        self.assertEqual(cache_variables.get("MC_ENABLE_SANITIZERS"), "ON")
        self.assertEqual(cache_variables.get("MC_SANITIZER"), "address")
        self.assertEqual(cache_variables.get("CMAKE_C_FLAGS"), "/Zi")
        self.assertEqual(cache_variables.get("CMAKE_CXX_FLAGS"), "/EHsc /Zi /bigobj")
        self.assertEqual(cache_variables.get("CMAKE_EXE_LINKER_FLAGS"), "/DEBUG")
        self.assertEqual(cache_variables.get("CMAKE_SHARED_LINKER_FLAGS"), "/DEBUG")
        self.assertEqual(cache_variables.get("VCPKG_TARGET_TRIPLET"), "x64-windows-asan")
        self.assertEqual(
            cache_variables.get("VCPKG_OVERLAY_TRIPLETS"),
            "${sourceDir}/script/ci/vcpkg-triplets",
        )

    def test_windows_analyze_preset_keeps_required_warning_gate(self):
        cache_variables = effective_cache_variables("analysis-windows-analyze")

        c_flags = cache_variables.get("CMAKE_C_FLAGS")
        cxx_flags = cache_variables.get("CMAKE_CXX_FLAGS")
        self.assertEqual(c_flags, "/analyze /W4 /WX /analyze:external-")
        self.assertEqual(cxx_flags, "/analyze /W4 /WX /EHsc /analyze:external-")
        for flags in (c_flags, cxx_flags):
            self.assertIn("/analyze", flags)
            self.assertIn("/W4", flags)
            self.assertIn("/WX", flags)
            self.assertNotIn("/analyze-", flags)
            self.assertNotIn("/WX-", flags)
        self.assertNotEqual(cache_variables.get("VCPKG_TARGET_TRIPLET"), "x64-windows-asan")
        self.assertNotIn("VCPKG_OVERLAY_TRIPLETS", cache_variables)

    def test_linux_sanitizer_presets_keep_explicit_clang_selection(self):
        for preset_name in ("analysis-linux-lsan", "analysis-linux-msan"):
            with self.subTest(preset=preset_name):
                cache_variables = effective_cache_variables(preset_name)

                self.assertEqual(cache_variables.get("CMAKE_C_COMPILER"), "clang")
                self.assertEqual(cache_variables.get("CMAKE_CXX_COMPILER"), "clang++")
                self.assertEqual(
                    cache_variables.get("VCPKG_OVERLAY_TRIPLETS"),
                    "${sourceDir}/script/ci/vcpkg-triplets",
                )

    def test_linux_arm64_triplets_keep_native_architecture_and_sanitizer_runtime(self):
        lsan = (REPOSITORY / "script/ci/vcpkg-triplets/arm64-linux-lsan.cmake").read_text(encoding="utf-8")
        msan = (REPOSITORY / "script/ci/vcpkg-triplets/arm64-linux-msan.cmake").read_text(encoding="utf-8")

        self.assertIn("set(VCPKG_TARGET_ARCHITECTURE arm64)", lsan)
        self.assertIn('"-fsanitize=leak', lsan)
        self.assertIn("set(VCPKG_TARGET_ARCHITECTURE arm64)", msan)
        self.assertIn('"-fsanitize=memory -fsanitize-memory-track-origins=2', msan)
        self.assertIn("MC_MSAN_LIBCXX_PREFIX", msan)
        self.assertNotIn("set(VCPKG_TARGET_ARCHITECTURE x64)", lsan + msan)

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

    def test_iwyu_preset_uses_fail_closed_public_include_gate(self):
        cache_variables = effective_cache_variables("analysis-iwyu")

        self.assertEqual(
            cache_variables["CMAKE_CXX_INCLUDE_WHAT_YOU_USE"],
            "python3;${sourceDir}/script/ci/iwyu_gate.py;--;include-what-you-use;-Xiwyu;--no_fwd_decls;-Xiwyu;--error;--",
        )

    def test_cppcheck_preset_uses_fail_closed_per_translation_unit_launcher(self):
        cache_variables = effective_cache_variables("analysis-cppcheck")

        self.assertEqual(
            cache_variables["CMAKE_CXX_COMPILER_LAUNCHER"],
            "python3;${sourceDir}/script/ci/cppcheck_gate.py;--;cppcheck;--enable=all;--inline-suppr;--xml;--",
        )
        self.assertEqual(cache_variables["CMAKE_CXX_CPPCHECK"], "")
        self.assertEqual(cache_variables["CMAKE_C_CPPCHECK"], "")


if __name__ == "__main__":
    unittest.main()
