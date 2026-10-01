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
