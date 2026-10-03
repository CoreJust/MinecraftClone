import io
import importlib.util
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from contextlib import redirect_stdout
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
        self.vcpkg_root = self.root / "vcpkg"
        toolchain = self.vcpkg_root / "scripts/buildsystems/vcpkg.cmake"
        toolchain.parent.mkdir(parents=True)
        toolchain.touch()
        self.environment = mock.patch.dict(os.environ, {"VCPKG_ROOT": str(self.vcpkg_root)})
        self.environment.start()

    def tearDown(self):
        self.environment.stop()
        self.temp_dir.cleanup()

    def write_smoke_cache(self, command):
        cache = self.root / "build/renderer-smoke/CMakeCache.txt"
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root}\n"
            "MC_ENABLE_RENDERER_SMOKE:BOOL=ON\n"
            "CMAKE_GENERATOR:INTERNAL=Ninja\n",
            encoding="utf-8",
        )
        (cache.parent / "build.ninja").write_text("# generated\n", encoding="utf-8")
        self.gate._write_configure_stamp(self.root, "renderer-smoke", command)
        return cache

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
        self.assertEqual(calls[0][1][:4], ["cmake", "--fresh", "--preset", "renderer-smoke"])
        self.assertIn(f"-DCMAKE_TOOLCHAIN_FILE={self.vcpkg_root}/scripts/buildsystems/vcpkg.cmake", calls[0][1])
        self.assertEqual(calls[1][1], ["cmake", "--build", "--preset", "renderer-smoke", "--target", "mc_renderer_smoke"])
        self.assertEqual(
            calls[2][1],
            ["ctest", "--preset", "renderer-smoke", "-R", "^RendererSmokeTest$", "--output-on-failure", "--no-tests=error"],
        )

    def test_reuses_only_matching_cache_with_smoke_enabled(self):
        command, failure = self.gate.renderer_configure_command(self.root, "renderer-smoke")
        self.assertIsNone(failure)
        self.write_smoke_cache(command)
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

    def test_legacy_smoke_cache_with_stale_import_paths_is_regenerated_fresh(self):
        legacy_command = [
            "cmake",
            "--preset",
            "renderer-smoke",
            f"-DCMAKE_TOOLCHAIN_FILE={self.vcpkg_root}/scripts/buildsystems/vcpkg.cmake",
        ]
        cache = self.write_smoke_cache(legacy_command)
        stale_vcpkg = self.root / "old-checkout/build/vcpkg_installed/arm64-osx"
        stale_paths = (
            stale_vcpkg / "debug/lib/libmpfr.a",
            stale_vcpkg / "debug/lib/libgmp.a",
        )
        with cache.open("a", encoding="utf-8") as stream:
            for key, stale_path in zip(
                ("CoreLangNumerics_MPFR_LIBRARY", "CoreLangNumerics_GMP_LIBRARY"),
                stale_paths,
            ):
                stream.write(f"{key}:FILEPATH={stale_path}\n")
        self.gate._write_configure_stamp(self.root, "renderer-smoke", legacy_command)
        calls = []

        def run_command(root, command, timeout):
            calls.append(command)
            if command[:4] == ["cmake", "--fresh", "--preset", "renderer-smoke"]:
                cache.write_text(
                    "CMAKE_HOME_DIRECTORY:INTERNAL=" + str(self.root) + "\n"
                    "MC_ENABLE_RENDERER_SMOKE:BOOL=ON\n"
                    "CMAKE_GENERATOR:INTERNAL=Ninja\n",
                    encoding="utf-8",
                )
            return self.gate.CommandResult(command, 0, "completed\n")

        with mock.patch.object(self.gate, "run_command", side_effect=run_command):
            result, commands, failure = self.gate.run_smoke(self.root, "renderer-smoke", 840)
            self.assertEqual(result, 0)
            self.assertIsNone(failure)
            self.assertEqual(commands[0].command[:4], ["cmake", "--fresh", "--preset", "renderer-smoke"])
            self.assertNotIn(str(stale_vcpkg), cache.read_text(encoding="utf-8"))

            calls.clear()
            reused_result, reused_commands, reused_failure = self.gate.run_smoke(
                self.root, "renderer-smoke", 840
            )

        self.assertEqual(reused_result, 0)
        self.assertIsNone(reused_failure)
        self.assertEqual(len(reused_commands), 2)
        self.assertEqual(calls[0][:3], ["cmake", "--build", "--preset"])

    def test_matching_debug_cache_toolchain_takes_precedence_over_environment(self):
        other_vcpkg_root = self.root / "other-vcpkg"
        other_toolchain = other_vcpkg_root / "scripts/buildsystems/vcpkg.cmake"
        other_toolchain.parent.mkdir(parents=True)
        other_toolchain.touch()
        with mock.patch.dict(os.environ, {"VCPKG_ROOT": str(other_vcpkg_root)}):
            installed = self.root / "shared-vcpkg"
            prefix = self.root / "private-prefix"
            corecpp = prefix / "lib/cmake/CoreCpp"
            coreproject = prefix / "lib/cmake/CoreProject2026"
            (installed / "arm64-osx").mkdir(parents=True)
            (corecpp).mkdir(parents=True)
            (coreproject).mkdir(parents=True)
            (corecpp / "CoreCppConfig.cmake").touch()
            (coreproject / "CoreProject2026Config.cmake").touch()
            cache = self.root / "build/debug/CMakeCache.txt"
            cache.parent.mkdir(parents=True)
            cache.write_text(
                f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root}\n"
                "CMAKE_TOOLCHAIN_FILE:UNINITIALIZED=/scripts/buildsystems/vcpkg.cmake\n"
                f"Z_VCPKG_ROOT_DIR:INTERNAL={self.vcpkg_root}\n"
                "VCPKG_MANIFEST_INSTALL:BOOL=OFF\n"
                f"VCPKG_INSTALLED_DIR:PATH={installed}\n"
                "VCPKG_TARGET_TRIPLET:STRING=arm64-osx\n"
                f"CMAKE_PREFIX_PATH:UNINITIALIZED={prefix}\n"
                f"CoreCpp_DIR:UNINITIALIZED={corecpp}\n"
                f"CoreProject2026_DIR:UNINITIALIZED={coreproject}\n",
                encoding="utf-8",
            )

            command, failure = self.gate.renderer_configure_command(self.root, "renderer-smoke")

        self.assertIsNone(failure)
        self.assertIn(f"-DCMAKE_TOOLCHAIN_FILE={self.vcpkg_root}/scripts/buildsystems/vcpkg.cmake", command)
        self.assertIn("-DVCPKG_MANIFEST_INSTALL=OFF", command)
        self.assertIn(f"-DVCPKG_INSTALLED_DIR={installed}", command)
        self.assertIn(f"-DCMAKE_PREFIX_PATH={prefix}", command)
        self.assertIn(f"-DCoreCpp_DIR={corecpp}", command)
        self.assertIn(f"-DCoreProject2026_DIR={coreproject}", command)
        self.assertNotIn(f"-DCMAKE_TOOLCHAIN_FILE={other_toolchain}", command)

    def test_missing_toolchain_fails_early_with_actionable_diagnostic(self):
        with mock.patch.dict(os.environ, {"VCPKG_ROOT": ""}):
            command, failure = self.gate.renderer_configure_command(self.root, "renderer-smoke")
        self.assertIsNone(command)
        self.assertIn("VCPKG_ROOT is unset", failure)

    def test_failed_configure_invalidates_partial_cache_for_retry(self):
        configure_commands = []

        def run_command(root, command, timeout):
            if command[:4] == ["cmake", "--fresh", "--preset", "renderer-smoke"]:
                configure_commands.append(command)
                cache = root / "build/renderer-smoke/CMakeCache.txt"
                cache.parent.mkdir(parents=True, exist_ok=True)
                cache.write_text(
                    f"CMAKE_HOME_DIRECTORY:INTERNAL={root}\nMC_ENABLE_RENDERER_SMOKE:BOOL=ON\n",
                    encoding="utf-8",
                )
                if len(configure_commands) == 1:
                    return self.gate.CommandResult(command, 124, "partial configure\n", timed_out=True)
                (cache.parent / "build.ninja").write_text("# generated\n", encoding="utf-8")
            return self.gate.CommandResult(command, 0, "completed\n")

        with mock.patch.object(self.gate, "run_command", side_effect=run_command):
            first_result, first_commands, first_failure = self.gate.run_smoke(
                self.root, "renderer-smoke", 840
            )
            self.assertEqual(first_result, 1)
            self.assertIn("configuration failed", first_failure)
            self.assertFalse(self.gate._configure_stamp_path(self.root, "renderer-smoke").exists())
            second_result, second_commands, second_failure = self.gate.run_smoke(
                self.root, "renderer-smoke", 840
            )

        self.assertEqual(second_result, 0)
        self.assertIsNone(second_failure)
        self.assertEqual(len(configure_commands), 2)
        self.assertEqual(
            second_commands[0].command[:4],
            ["cmake", "--fresh", "--preset", "renderer-smoke"],
        )
        self.assertEqual(len(first_commands), 1)

    def test_long_running_command_emits_liveness(self):
        class FakeProcess:
            def __init__(self):
                self.wait_calls = 0

            def wait(self, timeout=None):
                self.wait_calls += 1
                if self.wait_calls == 1:
                    raise self.gate.subprocess.TimeoutExpired("cmake", timeout)
                return 0

        process = FakeProcess()
        process.gate = self.gate
        output = io.StringIO()
        with redirect_stdout(output), mock.patch.object(
            self.gate.subprocess, "Popen", return_value=process
        ), mock.patch.object(self.gate.time, "monotonic", side_effect=[0.0, 0.0, 31.0, 31.0]):
            result = self.gate.run_command(self.root, ["cmake", "--preset", "renderer-smoke"], 90)
        self.assertEqual(result.returncode, 0)
        self.assertIn("still running after 31s", output.getvalue())

    def test_subprocess_timeout_kills_process_and_returns_timeout_status(self):
        class FakeProcess:
            def __init__(self):
                self.killed = False

            def wait(self, timeout=None):
                if timeout is not None:
                    raise self.gate.subprocess.TimeoutExpired("cmake", timeout)
                return -9

            def kill(self):
                self.killed = True

        process = FakeProcess()
        process.gate = self.gate
        with mock.patch.object(
            self.gate.subprocess, "Popen", return_value=process
        ), mock.patch.object(self.gate.time, "monotonic", side_effect=[0.0, 0.0, 2.0]):
            result = self.gate.run_command(self.root, ["cmake"], 1)
        self.assertEqual(result.returncode, 124)
        self.assertTrue(result.timed_out)
        self.assertTrue(process.killed)

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
