import contextlib
import importlib.util
import io
import json
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "script/ai_validation.py"


def load_module():
    spec = importlib.util.spec_from_file_location("ai_validation", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class AiValidationTests(unittest.TestCase):
    def test_generation_and_acceptance_sources_select_core_consumers(self):
        validation = load_module()
        native_tests = [
            {"name": "CoreTest.Basic", "source": "tests/core/chunk_tests.cpp", "module": "core", "size": "small"},
            {"name": "ClientTest.Basic", "source": "tests/client/camera_tests.cpp", "module": "client", "size": "medium"},
            {"name": "AcceptanceTest.Basic", "source": "tests/acceptance/evidence_json_tests.cpp", "module": "acceptance", "size": "medium"},
            {"name": "GenerationTest.Basic", "source": "tests/generation/generation_tests.cpp", "module": "generation", "size": "medium"},
            {"name": "IntegrationTest.Server", "source": "tests/core/game_server_tests.cpp", "module": "integration", "size": "medium"},
            {"name": "WorldMapCli", "module": "tools", "size": "medium"},
        ]

        plan = validation.select_plan(
            REPOSITORY,
            ["src/acceptance/EvidenceJson.cpp", "src/generation/Generation.cpp"],
            lane="routine",
            native_tests=native_tests,
        )

        checks = {check["id"] for check in plan["checks"]}
        self.assertTrue({
            "ctest:core:small",
            "ctest:client:medium",
            "ctest:acceptance:medium",
            "ctest:generation:medium",
        } <= checks)

    def test_core_changes_select_consumer_modules_and_report_large_work(self):
        validation = load_module()
        native_tests = [
            {"name": "CoreTest.Basic", "source": "tests/core/chunk_tests.cpp", "module": "core", "size": "small"},
            {"name": "ClientTest.Basic", "source": "tests/client/camera_tests.cpp", "module": "client", "size": "medium"},
            {
                "name": "Acceptance.Basic",
                "source": "tests/acceptance/scenario_runner_tests.cpp",
                "module": "acceptance",
                "size": "large",
            },
            {"name": "Generation.Basic", "source": "tests/generation/generation_tests.cpp", "module": "generation", "size": "medium"},
            {"name": "Tools.Basic", "module": "tools", "size": "medium"},
        ]

        plan = validation.select_plan(
            REPOSITORY,
            ["src/shared/chunk.cpp"],
            lane="routine",
            native_tests=native_tests,
        )

        checks = {check["id"] for check in plan["checks"]}
        self.assertIn("fast-code", checks)
        self.assertIn("ctest:core:small", checks)
        self.assertIn("ctest:client:medium", checks)
        self.assertNotIn("ctest:acceptance:large", checks)
        self.assertEqual(plan["deferred_large"][0]["id"], "ctest:acceptance:large")
        self.assertIn("core", plan["affected_modules"])
        self.assertIn("client", plan["affected_modules"])

    def test_unknown_changed_path_is_rejected(self):
        validation = load_module()

        with self.assertRaisesRegex(validation.SelectionError, "unclassified changed path"):
            validation.select_plan(
                REPOSITORY,
                ["firmware/new_board.bin"],
                lane="routine",
                native_tests=[],
            )

    def test_release_registry_is_fully_classified(self):
        validation = load_module()
        inventory, _ = validation.load_inventory(REPOSITORY)

        registry_hash = validation.validate_release_registry(REPOSITORY, inventory)

        self.assertRegex(registry_hash, r"^[0-9a-f]{64}$")
        checks = {item["id"]: item for item in inventory["release_checks"]}
        self.assertEqual(checks["sanitizer-static-analysis-s1-owner-profile"]["levels"], ["snapshot"])
        self.assertEqual(checks["sanitizer-static-analysis-s1-owner-profile"]["module"], "native-all")
        self.assertEqual(checks["sanitizer-static-analysis"]["levels"], ["minor", "major"])

    def test_new_enabled_release_check_fails_closed(self):
        validation = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "script").mkdir()
            (root / "script/ai_checks.json").write_text(
                json.dumps([{"name": "new-check", "enabled": True, "levels": ["major"]}]),
                encoding="utf-8",
            )
            inventory = {"release_checks": [], "native": {"all_test_modules": []}}

            with self.assertRaisesRegex(validation.SelectionError, "unclassified enabled checks: new-check"):
                validation.validate_release_registry(root, inventory)

    def test_release_check_level_changes_require_inventory_update(self):
        validation = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "script").mkdir()
            (root / "script/ai_checks.json").write_text(
                json.dumps([{"name": "environment", "enabled": True, "levels": ["minor", "major"]}]),
                encoding="utf-8",
            )
            inventory = {
                "release_checks": [{
                    "id": "environment",
                    "module": "tooling",
                    "size": "small",
                    "levels": ["major"],
                }],
                "native": {"all_test_modules": []},
            }

            with self.assertRaisesRegex(validation.SelectionError, "release check inventory levels mismatch: environment"):
                validation.validate_release_registry(root, inventory)

    def test_full_lane_requires_every_active_release_gate_for_affected_modules(self):
        validation = load_module()
        native_tests = [
            {"name": "CoreTest.Basic", "module": "core", "size": "small"},
            {"name": "ClientTest.Basic", "module": "client", "size": "medium"},
            {"name": "AndroidTest.Basic", "module": "android", "size": "medium"},
            {"name": "AcceptanceTest.Basic", "module": "acceptance", "size": "medium"},
            {"name": "IntegrationTest.Basic", "module": "integration", "size": "medium"},
            {"name": "ShaderTest.Basic", "module": "shaders", "size": "small"},
            {"name": "GenerationTest.Basic", "module": "generation", "size": "medium"},
            {"name": "WorldMapCli.Basic", "module": "tools", "size": "medium"},
        ]

        plan = validation.select_plan(
            REPOSITORY,
            ["src/shared/world/WorldCoverage.cpp"],
            lane="full",
            native_tests=native_tests,
        )

        required = {check["id"] for check in plan["required_release_checks"]}
        self.assertEqual(
            required,
            {"renderer-smoke", "sanitizer-static-analysis", "sanitizer-static-analysis-s1-owner-profile", "full-radius-gameplay"},
        )

    def test_lane_budget_timeout_preserves_pending_checks_as_unrun(self):
        validation = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            plan = {
                "checks": [
                    {
                        "id": "slow",
                        "size": "small",
                        "module": "core",
                        "commands": [[
                            str(Path(validation.sys.executable).resolve()),
                            "-c",
                            "import time; time.sleep(1)",
                        ]],
                        "inputs": [],
                        "prerequisites": [],
                    },
                    {
                        "id": "pending",
                        "size": "small",
                        "module": "core",
                        "commands": [],
                        "inputs": [],
                        "prerequisites": [],
                    },
                ],
                "preflight_seconds": 0.0,
                "aggregate_budget_seconds": 0.1,
                "budgets_seconds": {"small": 0.1, "medium": 1, "large": 1},
                "inventory_sha256": "test-inventory",
                "build_dir": str(root / "build"),
                "lane": "small",
                "required_release_checks": [],
            }

            result = validation.run_plan(root, plan)

            receipts = root / validation.RECEIPT_DIR
            first = json.loads((receipts / "slow.json").read_text(encoding="utf-8"))
            pending = json.loads((receipts / "pending.json").read_text(encoding="utf-8"))

        self.assertEqual(result, 1)
        self.assertEqual(first["status"], "TIMEOUT")
        self.assertEqual(pending["status"], "UNRUN")

    def test_medium_lane_selects_all_medium_tests(self):
        validation = load_module()
        native_tests = [
            {"name": "CoreTest.Basic", "source": "tests/core/chunk_tests.cpp", "module": "core", "size": "small"},
            {"name": "ClientTest.Basic", "source": "tests/client/camera_tests.cpp", "module": "client", "size": "medium"},
            {"name": "AndroidTest.Basic", "source": "tests/android/device_tests.cpp", "module": "android", "size": "medium"},
            {
                "name": "Acceptance.Basic",
                "source": "tests/acceptance/scenario_runner_tests.cpp",
                "module": "acceptance",
                "size": "large",
            },
        ]

        plan = validation.select_plan(
            REPOSITORY,
            ["src/client/camera_controller.cpp"],
            lane="medium",
            native_tests=native_tests,
        )

        checks = {check["id"] for check in plan["checks"]}
        self.assertIn("ctest:client:medium", checks)
        self.assertIn("ctest:android:medium", checks)
        self.assertNotIn("fast-code", checks)
        self.assertNotIn("ctest:core:small", checks)

    def test_gameplay_gate_python_tests_are_deferred_small_and_selected_medium(self):
        validation = load_module()
        native_tests = [
            {"name": "ChunkTest.Basic", "module": "core", "size": "small"},
        ]
        changed = ["script/tests/test_verify_gameplay_gate.py"]

        small = validation.select_plan(REPOSITORY, changed, lane="small", native_tests=native_tests)
        medium = validation.select_plan(REPOSITORY, changed, lane="medium", native_tests=native_tests)
        full = validation.select_plan(REPOSITORY, changed, lane="full", native_tests=native_tests)

        self.assertEqual([group["id"] for group in small["deferred_medium"]], ["python-gameplay-gate"])
        self.assertNotIn("python-gameplay-gate", {check["id"] for check in small["checks"]})
        medium_group = next(check for check in medium["checks"] if check["id"] == "python-gameplay-gate")
        self.assertEqual(medium_group["size"], "medium")
        self.assertEqual(
            medium_group["commands"],
            [["{python}", "-m", "unittest", "discover", "-s", "script/tests",
              "-p", "test_verify_gameplay_gate.py", "-v"]],
        )
        self.assertIn("python-gameplay-gate", {check["id"] for check in full["checks"]})

    def test_unrelated_tooling_path_does_not_select_gameplay_gate_medium_group(self):
        validation = load_module()
        plan = validation.select_plan(
            REPOSITORY,
            ["docs/ai/VALIDATION.md"],
            lane="routine",
            native_tests=[{"name": "ChunkTest.Basic", "module": "core", "size": "small"}],
        )

        self.assertNotIn("python-gameplay-gate", {check["id"] for check in plan["checks"]})
        self.assertEqual(plan["deferred_medium"], [])

    def test_deferred_medium_receipt_is_fingerprinted_and_never_reused_as_pass(self):
        validation = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "script/tests/test_verify_gameplay_gate.py"
            source.parent.mkdir(parents=True)
            source.write_text("test input\n", encoding="utf-8")
            source_digest = validation.sha256_file(source)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.email", "validation@example.invalid"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.name", "Validation Fixture"], cwd=root, check=True)
            subprocess.run(["git", "add", "script/tests/test_verify_gameplay_gate.py"], cwd=root, check=True)
            subprocess.run(["git", "commit", "-q", "--no-gpg-sign", "-m", "fixture"], cwd=root, check=True)
            plan = {
                "checks": [],
                "deferred_medium": [{
                    "id": "python-gameplay-gate",
                    "size": "medium",
                    "module": "tooling",
                    "commands": [["{python}", "-m", "unittest", "discover"]],
                    "inputs": ["script/tests/test_verify_gameplay_gate.py"],
                    "prerequisites": ["Python 3.12+"],
                }],
                "preflight_seconds": 0.0,
                "aggregate_budget_seconds": 900,
                "budgets_seconds": {"small": 180, "medium": 720, "large": 2700},
                "inventory_sha256": "test-inventory",
                "build_dir": str(root / "build"),
                "lane": "small",
                "required_release_checks": [],
            }

            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(validation.run_plan(root, plan), 0)
                self.assertEqual(validation.run_plan(root, plan), 0)
            receipt = json.loads(
                (root / validation.RECEIPT_DIR / "deferred-python-gameplay-gate.json").read_text()
            )

        self.assertEqual(receipt["status"], "DEFERRED")
        self.assertEqual(receipt["required_lane"], "medium")
        self.assertFalse(receipt["reusable"])
        self.assertRegex(receipt["fingerprint"], r"^[0-9a-f]{64}$")
        self.assertRegex(receipt["source_head"], r"^[0-9a-f]{40}$")
        self.assertRegex(receipt["prerequisites"]["repository"]["index_tree"], r"^[0-9a-f]{40}$")
        self.assertEqual(receipt["inputs"], [["script/tests/test_verify_gameplay_gate.py", source_digest]])

    def test_ctest_inventory_classifies_source_and_gpu_label(self):
        validation = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inventory = {
                "test_source_modules": [
                    {"prefix": "tests/core/", "module": "core", "size": "small"},
                    {"prefix": "tests/client/", "module": "client", "size": "medium"},
                    {"prefix": "tests/acceptance/", "module": "acceptance", "size": "medium"},
                    {"prefix": "tests/generation/", "module": "generation", "size": "medium"},
                ],
                "registration_tests": {},
            }
            payload = {
                "tests": [
                    {
                        "name": "ChunkTest.Basic",
                        "command": ["mc_tests", "--gtest_filter=ChunkTest.Basic"],
                        "properties": [
                            {"name": "DEF_SOURCE_LINE", "value": f"{root}/tests/core/chunk_tests.cpp:10"}
                        ],
                    },
                    {
                        "name": "CameraTest.Draws",
                        "command": ["mc_tests", "--gtest_filter=CameraTest.Draws"],
                        "properties": [
                            {"name": "DEF_SOURCE_LINE", "value": f"{root}/tests/client/camera_tests.cpp:20"},
                            {"name": "LABELS", "value": ["renderer", "gpu"]},
                        ],
                    },
                    {
                        "name": "EvidenceJson.ValidatesMetadata",
                        "command": ["mc_tests", "--gtest_filter=EvidenceJson.ValidatesMetadata"],
                        "properties": [
                            {"name": "DEF_SOURCE_LINE", "value": f"{root}/tests/acceptance/evidence_json_tests.cpp:20"}
                        ],
                    },
                    {
                        "name": "GenerationTest.Identity",
                        "command": ["mc_tests", "--gtest_filter=GenerationTest.Identity"],
                        "properties": [
                            {"name": "DEF_SOURCE_LINE", "value": f"{root}/tests/generation/generation_tests.cpp:14"}
                        ],
                    },
                ]
            }

            tests = validation.parse_ctest_inventory(payload, root, inventory)

        by_name = {test["name"]: test for test in tests}
        self.assertEqual(by_name["ChunkTest.Basic"]["module"], "core")
        self.assertEqual(by_name["ChunkTest.Basic"]["size"], "small")
        self.assertEqual(by_name["CameraTest.Draws"]["module"], "client")
        self.assertEqual(by_name["CameraTest.Draws"]["size"], "large")
        self.assertEqual(by_name["EvidenceJson.ValidatesMetadata"]["module"], "acceptance")
        self.assertEqual(by_name["EvidenceJson.ValidatesMetadata"]["size"], "medium")
        self.assertEqual(by_name["GenerationTest.Identity"]["module"], "generation")
        self.assertEqual(by_name["GenerationTest.Identity"]["size"], "medium")

    def test_source_override_classifies_six_seed_world_reference_as_large_integration(self):
        validation = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = "tests/core/terrain_world_reference_tests.cpp"
            payload = {"tests": [{
                "name": "TerrainWorldReferenceTest.Seed42",
                "command": ["mc_tests"],
                "properties": [{
                    "name": "DEF_SOURCE_LINE",
                    "value": f"{root}/{source}:69",
                }],
            }]}
            inventory = {
                "test_source_modules": [{"prefix": "tests/core/", "module": "core", "size": "small"}],
                "test_source_overrides": {source: {"module": "integration", "size": "large"}},
                "registration_tests": {},
            }

            [test] = validation.parse_ctest_inventory(payload, root, inventory)

        self.assertEqual(test["module"], "integration")
        self.assertEqual(test["size"], "large")

    def test_generation_change_selects_integration_medium_and_defers_six_seed_pipeline(self):
        validation = load_module()
        native_tests = [
            {"name": "ChunkTest.Basic", "source": "tests/core/chunk_tests.cpp", "module": "core", "size": "small"},
            {"name": "ClientTest.Basic", "source": "tests/client/camera_tests.cpp", "module": "client", "size": "medium"},
            {"name": "AcceptanceTest.Basic", "source": "tests/acceptance/evidence_json_tests.cpp", "module": "acceptance", "size": "medium"},
            {"name": "GenerationTest.Basic", "source": "tests/generation/generation_tests.cpp", "module": "generation", "size": "medium"},
            {"name": "IntegrationTest.Basic", "source": "tests/core/game_server_tests.cpp", "module": "integration", "size": "medium"},
            {"name": "TerrainWorldReferenceTest.Seed42", "source": "tests/core/terrain_world_reference_tests.cpp", "module": "integration", "size": "large"},
            {"name": "WorldMapCli", "module": "tools", "size": "medium"},
        ]

        plan = validation.select_plan(
            REPOSITORY,
            ["src/generation/Generation.cpp"],
            lane="routine",
            native_tests=native_tests,
        )

        selected = {check["id"] for check in plan["checks"]}
        deferred = {check["id"] for check in plan["deferred_large"]}
        self.assertIn("ctest:core:small", selected)
        self.assertIn("ctest:client:medium", selected)
        self.assertIn("ctest:acceptance:medium", selected)
        self.assertIn("ctest:generation:medium", selected)
        self.assertIn("ctest:integration:medium", selected)
        self.assertIn("ctest:tools:medium", selected)
        self.assertIn("ctest:integration:large", deferred)
        self.assertNotIn("ctest:integration:large", selected)

    def test_world_map_cli_registration_retains_python_input_and_build_artifact(self):
        validation = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inventory = {
                "test_source_modules": [],
                "registration_tests": {
                    "TerrainWorldMap.Cli": {
                        "module": "tools",
                        "size": "medium",
                        "inputs": ["script/tests/test_world_map_cli.py"],
                        "build_targets": ["mc_world_map"],
                        "artifacts": ["src/mc_world_map"],
                    },
                },
            }
            payload = {"tests": [{
                "name": "TerrainWorldMap.Cli",
                "command": ["python", "test_world_map_cli.py", "--executable", "build/debug/src/mc_world_map"],
                "properties": [{"name": "LABELS", "value": ["generation", "tools"]}],
            }]}

            [test] = validation.parse_ctest_inventory(payload, root, inventory)
            [group] = validation._group_tests([test])

        self.assertEqual(test["module"], "tools")
        self.assertEqual(test["size"], "medium")
        self.assertIn("script/tests/test_world_map_cli.py", group["inputs"])
        self.assertEqual(group["build_targets"], ["mc_world_map"])
        self.assertEqual(group["artifacts"], ["src/mc_world_map"])

    def test_column_map_cpu_consumer_registration_is_required_and_tracks_target(self):
        validation = load_module()
        inventory, _ = validation.load_inventory(REPOSITORY)
        native = inventory["native"]
        name = "MinecraftClone.ColumnMapCpuConsumer"
        self.assertIn(name, native["required_registration_tests"])
        payload = {"tests": [{
            "name": name,
            "command": ["cmake", "-P", "column_map_cpu_consumer_test.cmake"],
            "properties": [],
        }]}

        [test] = validation.parse_ctest_inventory(payload, REPOSITORY, native)
        [group] = validation._group_tests([test])

        self.assertEqual(test["module"], "integration")
        self.assertEqual(test["size"], "medium")
        self.assertIn("tests/cmake/column_map_cpu_consumer_test.cmake.in", group["inputs"])
        self.assertEqual(group["build_targets"], ["mc_column_map"])
        self.assertEqual(group["artifacts"], ["src/mc_column_map"])

    def test_missing_required_column_map_ctest_registration_fails_closed(self):
        validation = load_module()
        inventory, _ = validation.load_inventory(REPOSITORY)
        native = inventory["native"]
        payload = {"tests": [{
            "name": "TerrainWorldMap.Cli",
            "command": ["python", "test_world_map_cli.py"],
            "properties": [],
        }]}

        with self.assertRaises(validation.SelectionError) as error:
            validation.parse_ctest_inventory(payload, REPOSITORY, native)

        self.assertIn("MinecraftClone.ColumnMapCpuConsumer", str(error.exception))

    def test_optional_ctest_registrations_may_be_absent(self):
        validation = load_module()
        inventory, _ = validation.load_inventory(REPOSITORY)
        native = dict(inventory["native"])
        native["required_registration_tests"] = []
        payload = {"tests": [{
            "name": "TerrainWorldMap.Cli",
            "command": ["python", "test_world_map_cli.py"],
            "properties": [],
        }]}

        [test] = validation.parse_ctest_inventory(payload, REPOSITORY, native)

        self.assertEqual(test["name"], "TerrainWorldMap.Cli")

    def test_column_map_source_selects_client_replay_and_headless_consumer_checks(self):
        validation = load_module()
        inventory, _ = validation.load_inventory(REPOSITORY)
        native = inventory["native"]
        payload = {"tests": [
            {
                "name": "ColumnMapClientTest.Basic",
                "command": ["mc_tests", "--gtest_filter=ColumnMapClientTest.Basic"],
                "properties": [{
                    "name": "DEF_SOURCE_LINE",
                    "value": f"{REPOSITORY}/tests/client/column_map_client_tests.cpp:1",
                }],
            },
            {
                "name": "ColumnMapReplayTest.Basic",
                "command": ["mc_tests", "--gtest_filter=ColumnMapReplayTest.Basic"],
                "properties": [{
                    "name": "DEF_SOURCE_LINE",
                    "value": f"{REPOSITORY}/tests/acceptance/column_map_replay_tests.cpp:1",
                }],
            },
            {
                "name": "MinecraftClone.ColumnMapCpuConsumer",
                "command": ["cmake", "-P", "column_map_cpu_consumer_test.cmake"],
                "properties": [],
            },
        ]}
        native_tests = validation.parse_ctest_inventory(payload, REPOSITORY, native)

        plan = validation.select_plan(
            REPOSITORY,
            ["src/client/ColumnMapClient.cpp"],
            lane="routine",
            native_tests=native_tests,
        )

        checks = {check["id"]: check for check in plan["checks"]}
        self.assertTrue({
            "ctest:client:medium",
            "ctest:acceptance:medium",
            "ctest:integration:medium",
        } <= set(checks))
        self.assertIn("mc_column_map", checks["native-build"]["commands"][0])
        self.assertIn("src/mc_column_map", checks["ctest:integration:medium"]["artifacts"])
        source_modules = (
            ("src/client/ColumnMapClient.cpp", "column-map"),
            ("src/client/ColumnMapPresentation.cpp", "column-map"),
            ("src/client/include/client/ColumnMapClient.hpp", "column-map"),
            ("src/client/include/client/ColumnMapPresentation.hpp", "column-map"),
            ("src/acceptance/ColumnMapReplay.cpp", "column-map"),
            ("src/acceptance/ReplayInput.cpp", "column-map"),
            ("src/acceptance/include/acceptance/ColumnMapReplay.hpp", "column-map"),
            ("src/acceptance/include/acceptance/ReplayInput.hpp", "column-map"),
            ("src/tools/column_map_client.cpp", "column-map"),
            ("src/client/ClientAudio.cpp", "game-core"),
            ("src/client/FrameScheduler.cpp", "game-core"),
            ("src/client/GameClient.cpp", "game-core"),
            ("src/client/WorldLoading.cpp", "game-core"),
            ("src/client/PreviewResidency.cpp", "game-core"),
            ("src/client/PlayerPresentation.cpp", "game-core"),
            ("src/client/include/client/ClientAudio.hpp", "game-core"),
            ("src/client/include/client/Camera.hpp", "game-core"),
            ("src/client/include/client/FrameScheduler.hpp", "game-core"),
            ("src/client/include/client/GameClient.hpp", "game-core"),
            ("src/client/include/client/WorldLoading.hpp", "game-core"),
            ("src/client/include/client/PreviewResidency.hpp", "game-core"),
            ("src/client/include/client/PlayerPresentation.hpp", "game-core"),
        )
        for source, module in source_modules:
            affected, reasons, _ = validation._changed_modules([source], inventory)
            self.assertEqual(reasons[0]["module"], module)
            self.assertTrue({"client", "acceptance", "integration"} <= affected)

    def test_generation_map_presentation_source_selects_generation_consumers(self):
        validation = load_module()
        native_tests = [
            {"name": "Client.GenerationMap", "module": "client", "size": "medium"},
            {"name": "Generation.PresentationInput", "module": "generation", "size": "medium"},
            {"name": "Tools.WorldMap", "module": "tools", "size": "medium"},
        ]

        plan = validation.select_plan(
            REPOSITORY,
            ["src/client/GenerationMapPresentation.cpp"],
            lane="routine",
            native_tests=native_tests,
        )

        check_ids = {check["id"] for check in plan["checks"]}
        self.assertTrue({
            "ctest:client:medium",
            "ctest:generation:medium",
            "ctest:tools:medium",
        } <= check_ids)

    def test_height_tile_worker_pool_suite_is_integration_medium(self):
        validation = load_module()
        inventory, _ = validation.load_inventory(REPOSITORY)
        native = dict(inventory["native"])
        native["required_registration_tests"] = []
        source = "tests/core/height_tile_worker_pool_tests.cpp"
        payload = {"tests": [{
            "name": "HeightTileWorkerPoolTest.SharesPreparedWorldAcrossJobs",
            "command": ["mc_tests", "--gtest_filter=HeightTileWorkerPoolTest.SharesPreparedWorldAcrossJobs"],
            "properties": [{
                "name": "DEF_SOURCE_LINE",
                "value": f"{REPOSITORY}/{source}:1",
            }],
        }]}

        [test] = validation.parse_ctest_inventory(payload, REPOSITORY, native)
        plan = validation.select_plan(
            REPOSITORY,
            [source],
            lane="routine",
            native_tests=[test],
        )

        self.assertEqual((test["module"], test["size"]), ("integration", "medium"))
        check_ids = {check["id"] for check in plan["checks"]}
        self.assertIn("ctest:integration:medium", check_ids)
        self.assertNotIn("ctest:core:small", check_ids)

    def test_unmapped_configured_test_source_fails_closed(self):
        validation = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload = {
                "tests": [{
                    "name": "MysteryTest.Case",
                    "command": ["mc_tests"],
                    "properties": [
                        {"name": "DEF_SOURCE_LINE", "value": f"{root}/tests/mystery/mystery_tests.cpp:4"}
                    ],
                }]
            }
            with self.assertRaisesRegex(validation.SelectionError, "unclassified configured CTest"):
                validation.parse_ctest_inventory(
                    payload,
                    root,
                    {"test_source_modules": [], "registration_tests": {}},
                )

    def test_changed_test_file_missing_from_configured_inventory_fails_closed(self):
        validation = load_module()
        native_tests = [
            {"name": "CoreTest.Basic", "source": "tests/core/chunk_tests.cpp", "module": "core", "size": "small"},
        ]

        with self.assertRaisesRegex(validation.SelectionError, "changed test source is absent"):
            validation.select_plan(
                REPOSITORY,
                ["tests/core/new_tests.cpp"],
                lane="routine",
                native_tests=native_tests,
            )

    def test_generation_source_fails_until_its_configured_suite_exists(self):
        validation = load_module()
        native_tests = [
            {"name": "CoreTest.Basic", "source": "tests/core/chunk_tests.cpp", "module": "core", "size": "small"},
        ]

        with self.assertRaisesRegex(validation.SelectionError, "no tests for affected modules: generation"):
            validation.select_plan(
                REPOSITORY,
                ["src/generation/Generation.cpp"],
                lane="routine",
                native_tests=native_tests,
            )

    def test_tool_source_fails_until_its_configured_suite_exists(self):
        validation = load_module()
        native_tests = [
            {"name": "CoreTest.Basic", "source": "tests/core/chunk_tests.cpp", "module": "core", "size": "small"},
        ]

        with self.assertRaisesRegex(validation.SelectionError, "no tests for affected modules: tools"):
            validation.select_plan(
                REPOSITORY,
                ["src/tools/world_map.cpp"],
                lane="routine",
                native_tests=native_tests,
            )

    def test_world_map_source_selects_cli_test_and_builds_its_target(self):
        validation = load_module()
        native_tests = [{
            "name": "TerrainWorldMap.Cli",
            "module": "tools",
            "size": "medium",
            "inputs": ["script/tests/test_world_map_cli.py"],
            "artifacts": ["src/mc_world_map"],
            "build_targets": ["mc_world_map"],
        }]

        plan = validation.select_plan(
            REPOSITORY,
            ["src/tools/world_map.cpp"],
            lane="medium",
            native_tests=native_tests,
        )

        checks = {check["id"]: check for check in plan["checks"]}
        self.assertIn("ctest:tools:medium", checks)
        self.assertIn("script/tests/test_world_map_cli.py", checks["ctest:tools:medium"]["inputs"])
        self.assertEqual(checks["ctest:tools:medium"]["artifacts"], ["src/mc_world_map"])
        self.assertEqual(checks["native-build"]["commands"][0][-1], "mc_world_map")

    def test_registered_cli_artifact_identity_is_bound_to_receipt_prerequisites(self):
        validation = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_dir = root / "build/debug"
            artifact = build_dir / "src/mc_world_map"
            artifact.parent.mkdir(parents=True)
            artifact.write_bytes(b"cli-binary-v1")
            check = {
                "id": "ctest:tools:medium",
                "artifacts": ["src/mc_world_map"],
            }
            plan = {
                "native_config_sha256": "configured-ctest-fingerprint",
                "build_dir": str(build_dir),
                "native_build_state": {"available": True, "artifacts": {"mc_tests": "test", "mc_main": "main"}},
            }

            first = validation._prerequisites(root, check, plan)
            artifact.write_bytes(b"cli-binary-v2")
            second = validation._prerequisites(root, check, plan)

        self.assertNotEqual(
            first["test_artifacts"]["src/mc_world_map"]["sha256"],
            second["test_artifacts"]["src/mc_world_map"]["sha256"],
        )

    def test_registered_ctest_tool_identity_is_bound_to_receipt_prerequisites(self):
        validation = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tool = root / "tools/spirv-val"
            tool.parent.mkdir(parents=True)
            tool.write_bytes(b"validator-v1")
            tool.chmod(0o755)
            payload = {"tests": [{
                "name": "Spirv.ValidatesOutput",
                "command": [str(tool), "--target-env", "vulkan1.2", "map.vert.spv"],
                "properties": [{"name": "LABELS", "value": ["shader", "spirv"]}],
            }]}
            inventory = {
                "test_source_modules": [],
                "registration_tests": {"Spirv.ValidatesOutput": {"module": "shaders", "size": "small"}},
            }
            [test] = validation.parse_ctest_inventory(payload, root, inventory)
            [check] = validation._group_tests([test])
            plan = {
                "native_config_sha256": "configured-ctest-fingerprint",
                "build_dir": str(root / "build/debug"),
                "native_build_state": {"available": True, "artifacts": {"mc_tests": "test", "mc_main": "main"}},
            }

            first = validation._prerequisites(root, check, plan)
            tool.write_bytes(b"validator-v2")
            second = validation._prerequisites(root, check, plan)

        self.assertEqual(check["executables"], [str(tool)])
        self.assertNotEqual(
            first["test_executables"][str(tool)]["sha256"],
            second["test_executables"][str(tool)]["sha256"],
        )

    def test_optional_map_shader_registrations_remain_small_shader_checks(self):
        validation = load_module()
        inventory, _ = validation.load_inventory(REPOSITORY)
        native = dict(inventory["native"])
        native["required_registration_tests"] = []
        payload = {"tests": [
            {
                "name": name,
                "command": ["spirv-val", "--target-env", "vulkan1.2", f"{name}.spv"],
                "properties": [{"name": "LABELS", "value": ["shader", "spirv"]}],
            }
            for name in ("ShaderSpirvTargetTest.map.vert.spv", "ShaderSpirvTargetTest.map.frag.spv")
        ]}

        tests = validation.parse_ctest_inventory(payload, REPOSITORY, native)

        self.assertEqual({test["module"] for test in tests}, {"shaders"})
        self.assertEqual({test["size"] for test in tests}, {"small"})
        self.assertEqual(
            {test["inputs"][0] for test in tests},
            {"src/client/render/shaders/map.vert", "src/client/render/shaders/map.frag"},
        )

    def test_native_configuration_change_requires_inventory_regeneration(self):
        validation = load_module()
        native_tests = [
            {"name": "CoreTest.Basic", "source": "tests/core/chunk_tests.cpp", "module": "core", "size": "small"},
        ]

        with self.assertRaisesRegex(validation.SelectionError, "regenerate the configured test inventory"):
            validation.select_plan(
                REPOSITORY,
                ["tests/CMakeLists.txt"],
                lane="routine",
                native_tests=native_tests,
            )

    def test_nested_cmake_inputs_are_native_configuration_changes(self):
        validation = load_module()
        native_tests = [
            {"name": "CoreTest.Basic", "source": "tests/core/chunk_tests.cpp", "module": "core", "size": "small"},
        ]

        with self.assertRaisesRegex(validation.SelectionError, "native configuration changed"):
            validation.select_plan(
                REPOSITORY,
                ["tests/cmake/corecpp_provenance_test.cmake.in"],
                lane="routine",
                native_tests=native_tests,
            )

    def test_receipt_identity_changes_with_input_and_configuration(self):
        validation = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "src/core/chunk.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("before", encoding="utf-8")
            check = {
                "id": "ctest:core:small",
                "size": "small",
                "module": "core",
                "commands": [["ctest", "-R", "ChunkTest.Basic"]],
                "inputs": ["src/core/chunk.cpp"],
                "prerequisites": {"build": "debug-mc_tests"},
            }

            before = validation.check_fingerprint(root, check, "inventory-a", {"ctest": "ctest-v1"})
            source.write_text("after", encoding="utf-8")
            after_source = validation.check_fingerprint(root, check, "inventory-a", {"ctest": "ctest-v1"})
            after_config = validation.check_fingerprint(root, check, "inventory-b", {"ctest": "ctest-v1"})

        self.assertNotEqual(before, after_source)
        self.assertNotEqual(after_source, after_config)

    def test_receipt_identity_binds_selector_and_check_driver(self):
        validation = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script_dir = root / "script"
            script_dir.mkdir()
            (script_dir / "ai_validation.py").write_text("selector v1", encoding="utf-8")
            (script_dir / "ai_check.py").write_text("driver v1", encoding="utf-8")
            check = {
                "id": "ctest:core:small",
                "size": "small",
                "module": "core",
                "commands": [["ctest", "-R", "ChunkTest.Basic"]],
                "inputs": [],
            }

            before = validation.check_fingerprint(root, check, "inventory", {})
            (script_dir / "ai_validation.py").write_text("selector v2", encoding="utf-8")
            after = validation.check_fingerprint(root, check, "inventory", {})

        self.assertNotEqual(before, after)

    def test_prior_failures_run_before_pending_checks(self):
        validation = load_module()
        with tempfile.TemporaryDirectory() as directory:
            receipt_dir = Path(directory)
            (receipt_dir / "first.json").write_text(
                json.dumps({"status": "FAIL"}), encoding="utf-8"
            )
            checks = [{"id": "second"}, {"id": "first"}, {"id": "third"}]

            ordered = validation.failure_first_order(checks, receipt_dir)

        self.assertEqual([check["id"] for check in ordered], ["first", "second", "third"])

    def test_failed_test_runs_after_its_build_prerequisite_and_before_pending_checks(self):
        validation = load_module()
        with tempfile.TemporaryDirectory() as directory:
            receipt_dir = Path(directory)
            (receipt_dir / "ctest_core_small.json").write_text(
                json.dumps({"status": "FAIL"}), encoding="utf-8"
            )
            checks = [
                {"id": "ctest:core:small", "prerequisites": ["native-build"]},
                {"id": "fast-code"},
                {"id": "native-build"},
            ]

            ordered = validation.failure_first_order(checks, receipt_dir)

        self.assertEqual(
            [check["id"] for check in ordered],
            ["native-build", "ctest:core:small", "fast-code"],
        )


if __name__ == "__main__":
    unittest.main()
