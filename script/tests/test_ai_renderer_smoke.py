import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "script/ai_renderer_smoke.py"


def load_module():
    spec = importlib.util.spec_from_file_location("ai_renderer_smoke", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class RendererSmokeGateTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name).resolve()
        self.gate = load_module()

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_configures_then_builds_target_and_runs_exact_registered_test(self):
        calls = []

        def run_command(root, command, timeout):
            calls.append((root, command, timeout))
            return self.gate.CommandResult(command, 0, "completed\n")

        with mock.patch.object(self.gate, "run_command", side_effect=run_command):
            result, commands, failure = self.gate.run_smoke(self.root, "renderer-smoke", 840)
        self.assertEqual(result, 0)
        self.assertIsNone(failure)
        self.assertEqual(len(commands), 3)
        self.assertEqual(calls[0][1], ["cmake", "--preset", "renderer-smoke"])
        self.assertEqual(calls[1][1], ["cmake", "--build", "--preset", "renderer-smoke", "--target", "mc_renderer_smoke"])
        self.assertEqual(
            calls[2][1],
            ["ctest", "--preset", "renderer-smoke", "-R", "^RendererSmokeTest$", "--output-on-failure", "--no-tests=error"],
        )

    def test_reuses_only_matching_cache_with_smoke_enabled(self):
        cache = self.root / "build/renderer-smoke/CMakeCache.txt"
        cache.parent.mkdir(parents=True)
        cache.write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root}\nMC_ENABLE_RENDERER_SMOKE:BOOL=ON\n",
            encoding="utf-8",
        )
        with mock.patch.object(
            self.gate,
            "run_command",
            side_effect=lambda root, command, timeout: self.gate.CommandResult(command, 0, ""),
        ) as run_command:
            result, commands, failure = self.gate.run_smoke(self.root, "renderer-smoke", 840)
        self.assertEqual(result, 0)
        self.assertIsNone(failure)
        self.assertEqual(len(commands), 2)
        self.assertEqual(run_command.call_args_list[0].args[1][:3], ["cmake", "--build", "--preset"])

    def test_missing_test_or_validation_failure_fails_closed(self):
        outcomes = iter((0, 0, 8))

        def run_command(root, command, timeout):
            return self.gate.CommandResult(command, next(outcomes), "No tests were found\n")

        with mock.patch.object(self.gate, "run_command", side_effect=run_command):
            result, commands, failure = self.gate.run_smoke(self.root, "renderer-smoke", 840)
        self.assertEqual(result, 1)
        self.assertIn("RendererSmokeTest failed", failure)
        self.assertEqual(commands[-1].returncode, 8)

    def test_target_build_timeout_fails_closed(self):
        outcomes = iter((
            self.gate.CommandResult(["cmake"], 0, ""),
            self.gate.CommandResult(["cmake"], 124, "partial output", timed_out=True),
        ))
        with mock.patch.object(self.gate, "run_command", side_effect=lambda *args: next(outcomes)):
            result, commands, failure = self.gate.run_smoke(self.root, "renderer-smoke", 840)
        self.assertEqual(result, 1)
        self.assertEqual(commands[-1].returncode, 124)
        self.assertIn("target build failed", failure)

    def test_rejects_cache_from_another_checkout_without_running_commands(self):
        cache = self.root / "build/renderer-smoke/CMakeCache.txt"
        cache.parent.mkdir(parents=True)
        cache.write_text(
            "CMAKE_HOME_DIRECTORY:INTERNAL=/different/checkout\nMC_ENABLE_RENDERER_SMOKE:BOOL=ON\n",
            encoding="utf-8",
        )
        with mock.patch.object(self.gate, "run_command") as run_command:
            result, commands, failure = self.gate.run_smoke(self.root, "renderer-smoke", 840)
        self.assertEqual(result, 1)
        self.assertEqual(commands, [])
        self.assertIn("refusing to reuse", failure)
        run_command.assert_not_called()

    def test_main_records_commands_and_output_in_scoped_receipt(self):
        with mock.patch.object(
            self.gate,
            "run_smoke",
            return_value=(0, [self.gate.CommandResult(["ctest", "--preset", "renderer-smoke"], 0, "passed\n")], None),
        ):
            self.assertEqual(self.gate.main(["--root", str(self.root)]), 0)
        receipt = self.root / "build/ai-checks/renderer-smoke.log"
        self.assertEqual(receipt.read_text(encoding="utf-8"), "$ ctest --preset renderer-smoke\npassed\nexit 0\n\n")

    def test_release_registry_enables_this_gate_at_each_aggregate_level(self):
        registry = json.loads((REPOSITORY / "script/ai_checks.json").read_text(encoding="utf-8"))
        entry = next(item for item in registry if item["name"] == "renderer-smoke")
        self.assertTrue(entry["enabled"])
        self.assertEqual(entry["levels"], ["snapshot", "minor", "major"])
        self.assertEqual(entry["command"], ["{python}", "script/ai_renderer_smoke.py"])

    def test_renderer_smoke_preset_isolated_from_the_regular_debug_build(self):
        presets = json.loads((REPOSITORY / "CMakePresets.json").read_text(encoding="utf-8"))
        configure = next(item for item in presets["configurePresets"] if item["name"] == "renderer-smoke")
        self.assertEqual(configure["inherits"], "debug")
        self.assertEqual(configure["binaryDir"], "${sourceDir}/build/${presetName}")
        self.assertEqual(configure["cacheVariables"]["MC_ENABLE_RENDERER_SMOKE"], "ON")


if __name__ == "__main__":
    unittest.main()
