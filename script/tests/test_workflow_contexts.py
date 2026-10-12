"""Static contracts for valid GitHub Actions workflow contexts."""

from __future__ import annotations

import ast
import hashlib
import json
import re
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
WORKFLOWS = (
    REPOSITORY / ".github/workflows/ai-checks.yml",
    REPOSITORY / ".github/workflows/snapshot-artifacts.yml",
)


class WorkflowContextTests(unittest.TestCase):
    def test_diagnostic_dispatches_exclude_other_jobs_without_changing_defaults(self):
        workflow = WORKFLOWS[0].read_text(encoding="utf-8")
        jobs = (
            "fast",
            "desktop",
            "sustained-flight-macos-asan",
            "linux-analysis",
            "android-hwasan-build",
            "android-hwasan-runtime",
            "analysis-matrix",
        )
        job_headers = list(re.finditer(r"(?m)^  ([a-z][a-z0-9-]*):\s*$", workflow))
        job_blocks = {
            match.group(1): workflow[
                match.end() : job_headers[index + 1].start()
                if index + 1 < len(job_headers)
                else len(workflow)
            ]
            for index, match in enumerate(job_headers)
        }
        guards = {}
        for job in jobs:
            match = re.search(r"(?m)^    if: (.+)$", job_blocks[job])
            self.assertIsNotNone(match, job)
            expression = match.group(1)
            for original, replacement in (
                ("github.event.inputs.diagnostic", "diagnostic"),
                ("github.event_name", "event"),
                ("github.ref", "ref"),
                ("always()", "always"),
                ("&&", "and"),
                ("||", "or"),
            ):
                expression = expression.replace(original, replacement)
            syntax = ast.parse(expression, mode="eval")
            for node in ast.walk(syntax):
                self.assertIsInstance(node, (
                    ast.Expression, ast.BoolOp, ast.Compare, ast.Constant,
                    ast.Name, ast.Load, ast.And, ast.Or, ast.Eq, ast.NotEq,
                ))
            guards[job] = compile(syntax, "workflow guard", "eval")

        cases = (
            ("workflow_dispatch", "refs/heads/ai-dev", "sustained-flight", (False, False, True, False, False, False, False)),
            ("workflow_dispatch", "refs/heads/ai-dev", "full-context-suite", (False, False, True, False, False, False, False)),
            ("workflow_dispatch", "refs/heads/ai-dev", "none", (True, False, False, True, False, False, False)),
            ("push", "refs/heads/ai-dev", "", (True, False, False, False, False, False, False)),
            ("push", "refs/heads/ai-main", "", (True, True, False, True, True, True, True)),
            ("workflow_dispatch", "refs/heads/ai-main", "none", (True, True, False, False, False, False, False)),
            ("workflow_dispatch", "refs/heads/ai-main", "sustained-flight", (False, False, False, False, False, False, False)),
            ("workflow_dispatch", "refs/heads/ai-main", "full-context-suite", (False, False, False, False, False, False, False)),
        )
        for event, ref, diagnostic, expected in cases:
            with self.subTest(event=event, ref=ref, diagnostic=diagnostic):
                actual = tuple(eval(guard, {"__builtins__": {}}, {
                    "event": event,
                    "ref": ref,
                    "diagnostic": diagnostic,
                    "always": True,
                }) for guard in guards.values())
                self.assertEqual(actual, expected)

    def test_full_context_diagnostic_uses_original_suite_commands_and_budget(self):
        workflow = WORKFLOWS[0].read_text(encoding="utf-8")
        diagnostic = workflow.split("\n  sustained-flight-macos-asan:\n", 1)[1].split(
            "\n  linux-analysis:\n", 1
        )[0]
        self.assertIn("default: none", workflow)
        self.assertIn("          - full-context-suite\n", workflow)
        self.assertIn(
            "if: github.event_name == 'workflow_dispatch' && "
            "(github.event.inputs.diagnostic == 'sustained-flight' || "
            "github.event.inputs.diagnostic == 'full-context-suite') && "
            "github.ref == 'refs/heads/ai-dev'",
            diagnostic,
        )
        self.assertLess(
            diagnostic.index("Validate immutable diagnostic inputs"),
            diagnostic.index("secrets.MC_CI_CORECPP_DEPLOY_KEY"),
        )
        configuration = diagnostic.split(
            "      - name: Configure the hosted Mac ASan preset and inspect active tools\n", 1
        )[1].split("      - name:", 1)[0]
        self.assertNotIn("\n        if:", configuration)
        self.assertIn("verify_macos_lsan.py controls", configuration)
        self.assertIn("cmake --preset analysis-macos-asan", configuration)
        single_test = diagnostic.split(
            "      - name: Build and run only the sustained-flight test with unchanged limits\n", 1
        )[1].split("      - name:", 1)[0]
        self.assertIn("if: github.event.inputs.diagnostic == 'sustained-flight'", single_test)
        self.assertIn("--target mc_tests --parallel 4", single_test)
        self.assertIn("--no-tests=error --timeout 60 -V", single_test)
        self.assertIn("SustainedFlightKeepsInputAcknowledgementsCurrentWhileTilesStream$", single_test)

        full_suite = diagnostic.split(
            "      - name: Build and run the full hosted Mac ASan context\n", 1
        )[1].split("      - name:", 1)[0]
        self.assertIn("if: github.event.inputs.diagnostic == 'full-context-suite'", full_suite)
        self.assertIn("timeout-minutes: 15", full_suite)
        self.assertIn("cmake --build --preset analysis-macos-asan", full_suite)
        self.assertIn(
            "ctest --test-dir build/analysis-macos-asan --output-on-failure --no-tests=error",
            full_suite,
        )
        self.assertNotIn("--timeout", full_suite)
        self.assertIn("verify_macos_lsan.py summarize", full_suite)
        self.assertNotIn("verify_macos_lsan.py controls", full_suite)
        self.assertNotIn("cmake --preset analysis-macos-asan", full_suite)
        self.assertIn("set -euo pipefail", full_suite)
        self.assertIn('tee "$diagnostic_root/ctest.log"', full_suite)
        self.assertIn("LastTest.log", full_suite)
        self.assertIn("if: always()", diagnostic)
        self.assertIn("ctest.log", diagnostic)
        self.assertIn("LastTest.log", diagnostic)
        self.assertIn("ASAN_OPTIONS: detect_leaks=1", diagnostic)
        self.assertIn("if: always()", diagnostic)
        self.assertNotIn("run-row", diagnostic)
        self.assertNotIn("android_hwasan_emulator", diagnostic)

        matrix = json.loads((REPOSITORY / "script/ai_analysis_matrix.json").read_text(encoding="utf-8"))
        row = next(row for row in matrix["rows"] if row["id"] == "macos_asan")
        expected_build = " ".join(row["commands"][2])
        expected_ctest = " ".join(row["commands"][3])
        self.assertIn(expected_build, full_suite)
        self.assertIn(expected_ctest, full_suite)

    def test_flight_diagnostic_runs_lsan_controls_and_test_with_required_environment(self):
        workflow = WORKFLOWS[0].read_text(encoding="utf-8")
        expected_environment = {
            "ASAN_OPTIONS": "detect_leaks=1",
            "LSAN_OPTIONS": (
                "suppressions=${{ github.workspace }}/diagnostic-source/script/ci/macos_lsan.supp:"
                "print_suppressions=1"
            ),
        }
        steps = (
            (
                "Configure the hosted Mac ASan preset and inspect active tools",
                "verify_macos_lsan.py controls",
            ),
            (
                "Build and run only the sustained-flight test with unchanged limits",
                "ctest --test-dir",
            ),
            (
                "Build and run the full hosted Mac ASan context",
                "ctest --test-dir",
            ),
        )

        for step_name, command in steps:
            with self.subTest(step=step_name):
                step = workflow.split(f"      - name: {step_name}\n", 1)[1].split(
                    "      - name:", 1
                )[0]
                self.assertIn(command, step)
                environment_match = re.search(
                    r"(?m)^        env:\n((?:^          [^\n]*\n)+)", step
                )
                self.assertIsNotNone(environment_match, step_name)
                configured_environment = dict(
                    re.findall(
                        r"(?m)^          (ASAN_OPTIONS|LSAN_OPTIONS): ([^\n]+)$",
                        environment_match.group(1),
                    )
                )
                self.assertEqual(configured_environment, expected_environment, step_name)

    def test_flight_diagnostic_patch_bytes_match_verified_workflow_inputs(self):
        workflow = WORKFLOWS[0].read_text(encoding="utf-8")
        patch_root = REPOSITORY / "script/ci/diagnostics/sustained-flight"
        for filename in ("corecpp-host-transport.patch", "minecraft-flight-snapshot.patch"):
            with self.subTest(filename=filename):
                digest = hashlib.sha256((patch_root / filename).read_bytes()).hexdigest()
                self.assertIn(digest, workflow)
                self.assertIn(f"script/ci/diagnostics/sustained-flight/{filename}", workflow)

    def test_timed_runtime_tests_are_serial_for_full_and_server_only_registrations(self):
        timed_tests = (
            "GameServerPreviewTest.ResetsPriorityCursorAfterProgressWhenHeadingChanges",
            "GameClientLifecycleTest.StopDuringRetryIsServicedWithoutStartingAnotherAttempt",
            "GameServerPreviewTest.SustainedFlightKeepsInputAcknowledgementsCurrentWhileTilesStream",
        )
        unrelated_test = "UnrelatedRuntimeTest.KeepsDefaultScheduling"
        helper = REPOSITORY / "tests/cmake/serial_runtime_tests.cmake"
        cmake_lists = (REPOSITORY / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        discovery_start = cmake_lists.index("gtest_discover_tests(")
        discovery_end = cmake_lists.index("if(MC_BUILD_CLIENT AND NOT ANDROID)", discovery_start)
        discovery_setup = cmake_lists[discovery_start:discovery_end]
        self.assertIn("mc_tests", discovery_setup)
        self.assertIn("TEST_LIST mc_tests_TESTS", discovery_setup)
        self.assertLess(
            discovery_setup.index("gtest_discover_tests"),
            discovery_setup.index("PROPERTY TEST_INCLUDE_FILES"),
        )
        self.assertIn("cmake/serial_runtime_tests.cmake", discovery_setup)

        for registered_timed_tests in (timed_tests, (timed_tests[0], timed_tests[2])):
            with self.subTest(registered_timed_tests=registered_timed_tests):
                with tempfile.TemporaryDirectory() as temporary_directory:
                    temporary_path = Path(temporary_directory)
                    source_directory = temporary_path / "source"
                    build_directory = temporary_path / "build"
                    source_directory.mkdir()
                    registrations = (*registered_timed_tests, unrelated_test)
                    discovery_file = source_directory / "discovered_tests.cmake"
                    discovery_commands = [
                        'if(CMAKE_VERSION VERSION_LESS "4.0")',
                        '    cmake_policy(SET CMP0057 OLD)',
                        'endif()',
                    ]
                    discovery_commands.extend(
                        f'add_test("{test_name}" cmake -E true)'
                        for test_name in registrations
                    )
                    discovery_commands.append(
                        'set(mc_tests_TESTS "' + ";".join(registrations) + '")'
                    )
                    discovery_file.write_text(
                        "\n".join(discovery_commands) + "\n",
                        encoding="utf-8",
                    )
                    cmake_lists = [
                        "cmake_minimum_required(VERSION 3.25)",
                        "project(CtestSchedulingFixture NONE)",
                        "enable_testing()",
                        "set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES",
                        f'    "{discovery_file.as_posix()}"',
                        f'    "{helper.as_posix()}")',
                    ]
                    (source_directory / "CMakeLists.txt").write_text(
                        "\n".join(cmake_lists) + "\n",
                        encoding="utf-8",
                    )

                    configure = subprocess.run(
                        ["cmake", "-S", str(source_directory), "-B", str(build_directory)],
                        check=False,
                        capture_output=True,
                        text=True,
                        timeout=30,
                    )
                    self.assertEqual(configure.returncode, 0, configure.stdout + configure.stderr)

                    show_only = subprocess.run(
                        ["ctest", "--test-dir", str(build_directory), "--show-only=json-v1"],
                        check=False,
                        capture_output=True,
                        text=True,
                        timeout=30,
                    )
                    self.assertEqual(show_only.returncode, 0, show_only.stdout + show_only.stderr)
                    tests = {
                        test["name"]: {
                            prop["name"]: prop["value"] for prop in test.get("properties", [])
                        }
                        for test in json.loads(show_only.stdout)["tests"]
                    }

                    self.assertEqual(set(tests), set(registrations))
                    serial_tests = {
                        test_name
                        for test_name, properties in tests.items()
                        if properties.get("RUN_SERIAL") is True
                    }
                    self.assertEqual(
                        serial_tests,
                        set(registered_timed_tests),
                    )
                    self.assertNotIn("RUN_SERIAL", tests[unrelated_test])

    def test_windows_fast_checks_install_the_pinned_project_timezone_database(self):
        workflow = (REPOSITORY / ".github/workflows/ai-checks.yml").read_text(encoding="utf-8")
        fast_job = workflow.split("\n  desktop:", maxsplit=1)[0]

        self.assertIn("if: runner.os == 'Windows'", fast_job)
        self.assertIn("--no-cache-dir tzdata==2025.2", fast_job)

    def test_artifact_uploads_use_the_verified_immutable_revision(self):
        expected_revision = "ea165f8d65b6e75b540449e92b4886f43607fa02"
        for workflow_path in WORKFLOWS:
            workflow = workflow_path.read_text(encoding="utf-8")
            revisions = re.findall(r"uses:\s+actions/upload-artifact@([^\s#]+)", workflow)
            self.assertTrue(revisions, workflow_path)
            for revision in revisions:
                with self.subTest(workflow=workflow_path.name, revision=revision):
                    self.assertEqual(revision, expected_revision)

    def test_desktop_ctest_uses_bounded_parallelism(self):
        workflow = (REPOSITORY / ".github/workflows/ai-checks.yml").read_text(encoding="utf-8")
        desktop = workflow.split("\n  desktop:", maxsplit=1)[1].split("    steps:", maxsplit=1)[0]
        self.assertIn("CTEST_PARALLEL_LEVEL: '4'", desktop)

    def test_desktop_diagnostics_retain_python_test_phase_log_after_failure(self):
        workflow = WORKFLOWS[0].read_text(encoding="utf-8")
        diagnostics = workflow.split("      - name: Upload desktop diagnostics", maxsplit=1)[1].split(
            "      - name:", maxsplit=1
        )[0]

        self.assertIn("if: always()", diagnostics)
        self.assertIn("build/ai-checks/python-tests.log", diagnostics)

    def test_failed_analyzer_reports_are_retained_separately_from_matrix_receipts(self):
        workflow = WORKFLOWS[0].read_text(encoding="utf-8")
        diagnostics = workflow.split("      - name: Upload desktop diagnostics", maxsplit=1)[1].split(
            "      - name:", maxsplit=1
        )[0]
        receipts = workflow.split("      - name: Upload macOS analysis row receipts", maxsplit=1)[1].split(
            "      - name:", maxsplit=1
        )[0]

        self.assertIn("if: always()", diagnostics)
        for report_directory in ("build/analysis-iwyu/gate/**", "build/analysis-cppcheck/gate/**"):
            with self.subTest(reports=report_directory):
                self.assertIn(report_directory, diagnostics)
                self.assertNotIn(report_directory, receipts)
        self.assertIn("path: build/ai-analysis/*.json", receipts)

    def test_snapshot_trust_condition_has_valid_bash_then_separator(self):
        workflow = WORKFLOWS[1].read_text(encoding="utf-8")
        self.assertIn("git tag --points-at", workflow)
        self.assertIn("'+refs/tags/ai/*:refs/tags/ai/*'", workflow)
        self.assertIn("_[0-9]{2}\\.[0-9]{2}\\.[0-9]{2}$'; then", workflow)
        self.assertNotIn('refs/remotes/origin/ai-main \\\n              then', workflow)

    def test_snapshot_push_waits_for_exact_analysis_but_other_events_stay_strict(self):
        workflow = WORKFLOWS[1].read_text(encoding="utf-8")
        source_job = workflow.split("\n  source:", maxsplit=1)[1].split("\n  desktop:", maxsplit=1)[0]
        wait_step = source_job.split("      - name: Wait for successful S7 analysis matrix", maxsplit=1)[1].split(
            "      - name:", maxsplit=1
        )[0]
        strict_step = source_job.split(
            "      - name: Require a successful S7 analysis matrix for the exact source", maxsplit=1
        )[1].split("      - name:", maxsplit=1)[0]

        self.assertIn(
            "if: steps.trust.outputs.trusted_dependency_source == 'true' "
            "&& github.event_name == 'push' && github.ref == 'refs/heads/ai-main'",
            wait_step,
        )
        self.assertIn("--wait --timeout-seconds 21000 --poll-interval-seconds 30", wait_step)
        self.assertIn("timeout-minutes: 360", source_job)
        self.assertIn(
            "if: steps.trust.outputs.trusted_dependency_source == 'true' "
            "&& (github.event_name != 'push' || github.ref != 'refs/heads/ai-main')",
            strict_step,
        )
        self.assertNotIn("--wait", strict_step)

    def test_workflows_keep_runner_context_out_of_job_level_env(self):
        for workflow_path in WORKFLOWS:
            lines = workflow_path.read_text(encoding="utf-8").splitlines()
            job_env_lines: list[str] = []
            collecting = False
            for line in lines:
                if line == "    env:":
                    collecting = True
                    continue
                if collecting and line.startswith("      "):
                    job_env_lines.append(line)
                    continue
                if collecting:
                    collecting = False
            job_env = "\n".join(job_env_lines)
            self.assertNotIn("${{ runner.", job_env, workflow_path)
            workflow = workflow_path.read_text(encoding="utf-8")
            self.assertIn("Configure vcpkg binary cache (macOS)", workflow)
            self.assertIn("Configure vcpkg binary cache (Windows)", workflow)
            self.assertIn("mkdir -p \"$cache_dir\"", workflow)
            self.assertIn("VCPKG_DEFAULT_BINARY_CACHE=$cache_dir", workflow)
            self.assertIn("vcpkg-binary-cache", workflow)

    def test_snapshot_workflow_bootstraps_android_sdk_before_package_install(self):
        workflow = WORKFLOWS[1].read_text(encoding="utf-8")
        bootstrap = 'python script/ci/acquire.py install-android-sdk --root "$RUNNER_TEMP/android-sdk"'
        self.assertIn(bootstrap, workflow)
        self.assertNotIn("run: sdkmanager", workflow)

    def test_private_cmake_arguments_preserve_leading_dashes(self):
        for workflow_path in WORKFLOWS:
            workflow = workflow_path.read_text(encoding="utf-8")
            self.assertNotRegex(workflow, r"--cmake-arg\s+[\"']-D", workflow_path)
            for line in workflow.splitlines():
                if "--cmake-arg" in line:
                    self.assertRegex(line, r"--cmake-arg=-D", workflow_path)

    def test_private_dependency_secrets_are_limited_to_trusted_ai_main_desktop(self):
        workflow = WORKFLOWS[0].read_text(encoding="utf-8")
        desktop = workflow.split("\n  desktop:", maxsplit=1)[1]
        self.assertIn(
            "if: github.event_name != 'pull_request' && github.ref == 'refs/heads/ai-main'",
            desktop,
        )
        source = workflow.split("\n  desktop:", maxsplit=1)[0]
        self.assertNotIn("secrets.", source)
        self.assertIn("fetch-private-dependencies", desktop)
        self.assertIn("install-private-dependencies", desktop)

    def test_sanitizer_jobs_keep_exact_event_guards(self):
        workflow = WORKFLOWS[0].read_text(encoding="utf-8")
        job_headers = list(re.finditer(r"(?m)^  ([a-z][a-z0-9-]*):\s*$", workflow))
        jobs = {
            match.group(1): workflow[
                match.end() : job_headers[index + 1].start()
                if index + 1 < len(job_headers)
                else len(workflow)
            ]
            for index, match in enumerate(job_headers)
        }
        job_conditions = {
            "linux-analysis": (
                "((github.event_name == 'push' && github.ref == 'refs/heads/ai-main') "
                "|| (github.event_name == 'workflow_dispatch' && github.ref == 'refs/heads/ai-dev')) "
                "&& github.event.inputs.diagnostic != 'sustained-flight' "
                "&& github.event.inputs.diagnostic != 'full-context-suite'"
            ),
            "android-hwasan-build": "github.event_name == 'push' && github.ref == 'refs/heads/ai-main'",
            "android-hwasan-runtime": "github.event_name == 'push' && github.ref == 'refs/heads/ai-main'",
            "analysis-matrix": "always() && github.event_name == 'push' && github.ref == 'refs/heads/ai-main'",
        }

        for job_name, condition in job_conditions.items():
            with self.subTest(job=job_name):
                condition_match = re.search(r"(?m)^    if: (.+)$", jobs[job_name])
                self.assertIsNotNone(condition_match, job_name)
                self.assertEqual(condition_match.group(1), condition)

    def test_android_hwasan_runtime_uses_owned_host_accelerated_emulator_lifecycle(self):
        workflow = WORKFLOWS[0].read_text(encoding="utf-8")
        runtime_job = workflow.split("\n  android-hwasan-runtime:", maxsplit=1)[1].split(
            "\n  analysis-matrix:", maxsplit=1
        )[0]
        self.assertIn(
            '    runs-on: "mc-s7-hwasan-' + '$' + '{{ github.sha }}-' + '$' + '{{ github.run_id }}"',
            runtime_job,
        )
        self.assertNotIn("runs-on: [self-hosted", runtime_job)
        self.assertNotIn("ubuntu-24.04-arm", runtime_job)
        self.assertLess(
            runtime_job.index("Select and validate Java 21"),
            runtime_job.index("Bootstrap pinned Android SDK tools"),
        )
        self.assertLess(
            runtime_job.index("Create isolated Android emulator homes"),
            runtime_job.index("Bootstrap pinned Android SDK tools"),
        )
        self.assertIn("install-android-runtime-sdk", runtime_job)
        self.assertIn('--root "$ANDROID_SDK_ROOT"', runtime_job)
        self.assertIn('--seed-root "${MC_ANDROID_SDK_SEED_ROOT:-}"', runtime_job)
        self.assertNotIn("ndk;27.0.12077973", runtime_job)
        self.assertNotIn("cmake;3.30.5", runtime_job)
        self.assertLess(
            runtime_job.index("Preflight downloaded HWASan APK before emulator startup"),
            runtime_job.index("Start matching ARM64 Android emulator with host acceleration"),
        )
        self.assertIn("script/ci/android_hwasan_emulator.py start", runtime_job)
        self.assertIn("script/ci/android_hwasan_emulator.py wait", runtime_job)
        self.assertNotIn("ANDROID_I_WANT_MY_TCG", runtime_job)
        self.assertNotIn("-accel off", runtime_job)
        self.assertNotIn("avdmanager create avd --force", runtime_job)

        upload = runtime_job.index("Upload Android HWASan emulator log")
        cleanup = runtime_job.index("Stop owned Android emulator and remove private AVD state")
        self.assertLess(upload, cleanup)
        cleanup_step = runtime_job[cleanup:]
        self.assertIn("if: always()", cleanup_step)
        self.assertIn("script/ci/android_hwasan_emulator.py cleanup", cleanup_step)

    def test_linux_sanitizer_bootstrap_uses_published_ninja_wheel(self):
        workflow = WORKFLOWS[0].read_text(encoding="utf-8")
        linux_analysis = workflow.split("\n  linux-analysis:", maxsplit=1)[1].split(
            "\n  android-hwasan-build:", maxsplit=1
        )[0]
        self.assertIn(
            "python -m pip install --disable-pip-version-check --no-cache-dir "
            "cmake==3.31.6 ninja==1.13.2",
            linux_analysis,
        )

    def test_linux_sanitizer_bootstrap_pins_compatible_clang_and_glfw_headers(self):
        workflow = WORKFLOWS[0].read_text(encoding="utf-8")
        linux_analysis = workflow.split("\n  linux-analysis:", maxsplit=1)[1].split(
            "\n  android-hwasan-build:", maxsplit=1
        )[0]
        self.assertIn("CC: clang-18", linux_analysis)
        self.assertIn("CXX: clang++-18", linux_analysis)
        self.assertIn("sudo apt-get install --no-install-recommends --yes clang-18", linux_analysis)
        for package in (
            "libx11-dev",
            "libxrandr-dev",
            "libxinerama-dev",
            "libxcursor-dev",
            "libxi-dev",
            "autoconf",
            "autoconf-archive",
            "automake",
            "libtool",
        ):
            with self.subTest(package=package):
                self.assertIn(package, linux_analysis)
        self.assertIn('--clang "$CC" --clangxx "$CXX"', linux_analysis)

    def test_private_prefixes_and_manifest_mode_are_explicit(self):
        for workflow_path in WORKFLOWS:
            workflow = workflow_path.read_text(encoding="utf-8")
            self.assertIn("install-manifest-dependencies", workflow, workflow_path)
            self.assertIn("--installed-root", workflow, workflow_path)
            self.assertIn("VCPKG_INSTALLED_DIR", workflow, workflow_path)
            self.assertIn("-DVCPKG_MANIFEST_INSTALL=OFF", workflow, workflow_path)
            self.assertIn("-DCMAKE_PREFIX_PATH=", workflow, workflow_path)
            self.assertIn("-DCoreCpp_DIR=", workflow, workflow_path)
            self.assertIn("-DCoreProject2026_DIR=", workflow, workflow_path)

    def test_private_dependency_install_matches_consumer_preset(self):
        ai_workflow = WORKFLOWS[0].read_text(encoding="utf-8")
        snapshot_workflow = WORKFLOWS[1].read_text(encoding="utf-8")
        self.assertIn('--preset "${{ matrix.preset }}"', ai_workflow)
        self.assertIn("--preset release", snapshot_workflow)

    def test_windows_key_acl_is_current_user_only_and_fail_closed(self):
        for workflow_path in WORKFLOWS:
            workflow = workflow_path.read_text(encoding="utf-8")
            self.assertNotIn("$env:USERNAME:(OI)(CI)(R,W)", workflow, workflow_path)
            self.assertIn("$currentUser = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name", workflow, workflow_path)
            self.assertIn("$grant = '{0}:F' -f $currentUser", workflow, workflow_path)
            self.assertIn("/inheritance:r /grant:r $grant", workflow, workflow_path)
            self.assertIn("if ($LASTEXITCODE -ne 0)", workflow, workflow_path)

    def test_windows_keys_are_lf_utf8_without_bom_and_parse_before_fetch(self):
        for workflow_path in WORKFLOWS:
            workflow = workflow_path.read_text(encoding="utf-8")
            self.assertIn("$utf8NoBom = [System.Text.UTF8Encoding]::new($false)", workflow, workflow_path)
            self.assertIn('.Replace("`r`n", "`n").Replace("`r", "`n")', workflow, workflow_path)
            self.assertIn("$bytes[-1] -ne 10", workflow, workflow_path)
            self.assertIn("$bytes -contains 13", workflow, workflow_path)
            self.assertIn("$bytes[0] -eq 239", workflow, workflow_path)
            self.assertIn("& ssh-keygen.exe -y -f $keyPath *> $null", workflow, workflow_path)
            self.assertNotIn("[Environment]::NewLine", workflow, workflow_path)
            self.assertNotIn("WriteAllText($coreCppKey, $env:", workflow, workflow_path)
            self.assertNotIn("WriteAllText($coreProjectKey, $env:", workflow, workflow_path)
            self.assertLess(workflow.index("ssh-keygen.exe"), workflow.index("fetch-private-dependencies"))

    def test_artifact_uploads_never_include_private_sources(self):
        for workflow_path in WORKFLOWS:
            workflow = workflow_path.read_text(encoding="utf-8")
            for upload in workflow.split("path: |")[1:]:
                self.assertNotIn("private-dependencies", upload.split("if-no-files-found", maxsplit=1)[0], workflow_path)

    def test_windows_private_install_enters_msvc_before_cmake(self):
        for workflow_path in WORKFLOWS:
            workflow = workflow_path.read_text(encoding="utf-8")
            private_step = workflow.split("      - name: Install pinned private dependencies (Windows)\n", maxsplit=1)[1]
            private_step = private_step.split("      - name:", maxsplit=1)[0]
            developer_environment = 'call "%ProgramFiles%\\Microsoft Visual Studio\\2022\\Enterprise\\Common7\\Tools\\VsDevCmd.bat" -arch=amd64'
            self.assertLess(private_step.index(developer_environment), private_step.index("install-private-dependencies"), workflow_path)
            self.assertIn('set "MC_ACQUIRED_VCPKG_ROOT=%VCPKG_ROOT%"', private_step, workflow_path)
            self.assertIn('set "VCPKG_ROOT=%MC_ACQUIRED_VCPKG_ROOT%"', private_step, workflow_path)
            self.assertIn(
                '"--cmake-arg=-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\\scripts\\buildsystems\\vcpkg.cmake"',
                private_step,
                workflow_path,
            )
            self.assertIn("if errorlevel 1 exit /b %errorlevel%", private_step, workflow_path)

    def test_windows_snapshot_stages_pinned_vulkan_loader(self):
        workflow = WORKFLOWS[1].read_text(encoding="utf-8")
        runtime_step = workflow.split("      - name: Stage pinned Windows Vulkan runtime\n", maxsplit=1)[1]
        runtime_step = runtime_step.split("      - name:", maxsplit=1)[0]
        url = "https://sdk.lunarg.com/sdk/download/1.4.357.0/windows/vulkan-runtime-components.zip"
        self.assertIn(url, runtime_step)
        self.assertIn("A14672EFED15AAFC7F5A16572D35CD3A3416EADF670AEEE3CDF50EE32D5FBF83", runtime_step)
        self.assertIn("VulkanRT-X64-1.4.357.0-Components/x64/vulkan-1.dll", runtime_step)
        self.assertIn('Join-Path $env:VULKAN_SDK "Bin/vulkan-1.dll"', runtime_step)
        self.assertIn("Get-FileHash -Algorithm SHA256", runtime_step)
        self.assertIn('if ($actual -ne $expected)', runtime_step)
        self.assertIn("(Get-Item $destination).Length -le 0", runtime_step)
        self.assertLess(runtime_step.index("Get-FileHash"), runtime_step.index("Expand-Archive"))
        self.assertLess(runtime_step.index('if ($actual -ne $expected)'), runtime_step.index("Expand-Archive"))
        self.assertLess(workflow.index("Stage pinned Windows Vulkan runtime"), workflow.index("Build, test, and package Windows"))
        self.assertIn('--vulkan-runtime "build\\install\\vulkan-1.dll"', workflow)

    def test_windows_snapshot_uses_one_native_msvc_toolchain_and_bounded_server_only_plan(self):
        workflow = WORKFLOWS[1].read_text(encoding="utf-8")
        windows_build = workflow.split(
            "      - name: Build, test, and package Windows\n", maxsplit=1
        )[1].split("      - name:", maxsplit=1)[0]
        self.assertNotIn("clang-cl", windows_build)
        self.assertNotIn("CMAKE_CXX_COMPILER_LAUNCHER", windows_build)
        self.assertIn('-E "^MinecraftClone.ServerOnlyBuild$"', windows_build)
        self.assertNotIn("prepare-windows-server-only.py", workflow)
        self.assertIn("-P build\\release\\tests\\minecraftclone_server_only_test.cmake", windows_build)

    def test_nested_server_only_fixture_runs_focused_loopback_contract(self):
        fixture = (REPOSITORY / "tests/cmake/minecraftclone_server_only_test.cmake.in").read_text(encoding="utf-8")
        self.assertIn('-G "@CMAKE_GENERATOR@"', fixture)
        self.assertIn('-R "^GameServerTest\\\\.JoinRepliesArePrivateAndNewPlayersReachExistingClients$"', fixture)
        self.assertIn("--no-tests=error", fixture)
        self.assertNotIn('-E "^GameServerPreviewTest', fixture)

    def test_snapshot_private_dependencies_use_portable_compilers_without_changing_locks(self):
        workflow = WORKFLOWS[1].read_text(encoding="utf-8")
        windows_install = workflow.split(
            "      - name: Install pinned private dependencies (Windows)\n", maxsplit=1
        )[1].split("      - name:", maxsplit=1)[0]
        self.assertNotIn("clang-cl", windows_install)
        self.assertNotIn("CMAKE_CXX_COMPILER_LAUNCHER", windows_install)
        self.assertNotIn("Create bounded Windows server-only test plan", workflow)

        macos_install = workflow.split(
            "      - name: Install pinned private dependencies (macOS)\n", maxsplit=1
        )[1].split("      - name:", maxsplit=1)[0]
        self.assertIn("run: >-", macos_install)
        self.assertIn("python script/ci/acquire.py install-private-dependencies\n", macos_install)
        self.assertNotIn("install-private-dependencies \\", macos_install)

        android_install = workflow.split(
            "      - name: Install pinned private dependencies\n", maxsplit=1
        )[1].split("      - name:", maxsplit=1)[0]
        self.assertIn("python script/ci/acquire.py install-private-dependencies \\\n", android_install)
        self.assertIn("add_compile_options(-Wno-error=reorder-init-list)", android_install)
        self.assertIn("--cmake-arg=-DCMAKE_PROJECT_INCLUDE_BEFORE=", android_install)
        self.assertNotIn("dependencies.lock.json", android_install)

    def test_macos_snapshot_runs_preview_once_in_outer_suite(self):
        workflow = WORKFLOWS[1].read_text(encoding="utf-8")
        macos_phase = workflow.split(
            "      - name: Build, test, and package macOS\n", maxsplit=1
        )[1].split("      - name:", maxsplit=1)[0]
        server_test = "MinecraftClone.ServerOnlyBuild"
        flight_test = "GameServerPreviewTest.SustainedFlightKeepsInputAcknowledgementsCurrentWhileTilesStream"
        broad_ctest = next(
            line.strip()
            for line in macos_phase.splitlines()
            if "ctest --test-dir" in line and " -E " in line
        )
        self.assertIn("-E", broad_ctest)
        self.assertIn(server_test, broad_ctest)
        self.assertNotIn(flight_test, broad_ctest)
        self.assertIn("minecraftclone_server_only_test.cmake", macos_phase)
        self.assertNotIn(flight_test, (
            REPOSITORY / "tests/cmake/minecraftclone_server_only_test.cmake.in"
        ).read_text(encoding="utf-8"))
        self.assertNotIn("source.count(needle)", macos_phase)
        self.assertEqual(macos_phase.count("release-tests-macos.log"), 2)

    def test_windows_cmd_build_phases_guard_each_fallible_command(self):
        phase_names = ("Build, test, and validate shaders (Windows)", "Build, test, and package Windows")
        commands = ("cmake --preset", "cmake --build", "ctest --test-dir", "python script/ci/acquire.py validate-shaders")
        for workflow_path in WORKFLOWS:
            workflow = workflow_path.read_text(encoding="utf-8")
            for phase_name in phase_names:
                if phase_name not in workflow:
                    continue
                phase = workflow.split(f"      - name: {phase_name}\n", maxsplit=1)[1].split("      - name:", maxsplit=1)[0]
                lines = [line.strip() for line in phase.splitlines()]
                for command in commands:
                    index = next(index for index, line in enumerate(lines) if line.startswith(command))
                    if phase_name == "Build, test, and package Windows" and command == "ctest --test-dir":
                        self.assertEqual(
                            lines[index + 1:index + 6],
                            [
                                'set "ctest_result=%errorlevel%"',
                                "type build\\release-tests-windows.log",
                                'set "ctest_log_result=%errorlevel%"',
                                'if not "%ctest_result%"=="0" exit /b %ctest_result%',
                                'if not "%ctest_log_result%"=="0" exit /b %ctest_log_result%',
                            ],
                            workflow_path,
                        )
                        continue
                    self.assertEqual(lines[index + 1], "if errorlevel 1 exit /b %errorlevel%", (workflow_path, command))

    def test_windows_analysis_runs_both_abi_matched_rows_and_fails_if_either_fails(self):
        workflow = (REPOSITORY / ".github/workflows/ai-checks.yml").read_text(encoding="utf-8")
        asan_manifest_dependencies = workflow.split(
            "      - name: Install pinned Windows ASan manifest dependencies\n", maxsplit=1
        )[1].split("      - name:", maxsplit=1)[0]
        asan_dependencies = workflow.split(
            "      - name: Install pinned Windows ASan private dependencies\n", maxsplit=1
        )[1].split("      - name:", maxsplit=1)[0]
        analysis_step = workflow.split(
            "      - name: Run required Windows analysis rows\n", maxsplit=1
        )[1].split("      - name:", maxsplit=1)[0]
        lines = [line.strip() for line in analysis_step.splitlines()]
        asan_row = "python script/ai_analysis_matrix.py run-row build\\ai-analysis\\windows_msvc_asan.json --row windows_msvc_asan"
        analyze_row = "python script/ai_analysis_matrix.py run-row build\\ai-analysis\\windows_msvc_analyze.json --row windows_msvc_analyze"

        for command in (
            'call "%ProgramFiles%\\Microsoft Visual Studio\\2022\\Enterprise\\Common7\\Tools\\VsDevCmd.bat" -arch=amd64',
            "if not exist build\\ai-analysis mkdir build\\ai-analysis",
        ):
            index = lines.index(command)
            self.assertEqual(lines[index + 1], "if errorlevel 1 exit /b 1", command)

        asan_index = lines.index(asan_row)
        analyze_index = lines.index(analyze_row)
        self.assertLess(asan_index, analyze_index)
        self.assertEqual(lines[asan_index + 1], 'set "MC_ASAN_ROW_STATUS=%ERRORLEVEL%"')
        self.assertEqual(
            lines[asan_index - 3:asan_index],
            [
                'set "MC_PRIVATE_DEPENDENCIES_PREFIX=%RUNNER_TEMP%\\private-dependencies-windows-asan\\install"',
                'set "CMAKE_PREFIX_PATH=%RUNNER_TEMP%\\private-dependencies-windows-asan\\install"',
                'set "VCPKG_INSTALLED_DIR=%RUNNER_TEMP%\\vcpkg-installed-windows-asan"',
            ],
        )
        self.assertEqual(
            lines[analyze_index - 3:analyze_index],
            [
                'set "MC_PRIVATE_DEPENDENCIES_PREFIX=%RUNNER_TEMP%\\private-dependencies\\install"',
                'set "CMAKE_PREFIX_PATH=%RUNNER_TEMP%\\private-dependencies\\install"',
                'set "VCPKG_INSTALLED_DIR=%RUNNER_TEMP%\\vcpkg-installed"',
            ],
        )
        self.assertEqual(lines[analyze_index + 1], 'set "MC_ANALYZE_ROW_STATUS=%ERRORLEVEL%"')
        self.assertEqual(
            lines[-2:],
            [
                'if not "%MC_ASAN_ROW_STATUS%"=="0" exit /b %MC_ASAN_ROW_STATUS%',
                'if not "%MC_ANALYZE_ROW_STATUS%"=="0" exit /b %MC_ANALYZE_ROW_STATUS%',
            ],
        )

        self.assertIn("--platform windows-asan", asan_manifest_dependencies)
        self.assertIn("--platform windows", asan_dependencies)
        self.assertLess(
            workflow.index("Install pinned Windows ASan manifest dependencies"),
            workflow.index("Install pinned Windows ASan private dependencies"),
        )
        self.assertIn("--output-root \"%RUNNER_TEMP%\\private-dependencies-windows-asan\"", asan_dependencies)
        self.assertIn("--preset release", asan_dependencies)
        self.assertIn("-DVCPKG_TARGET_TRIPLET=x64-windows-asan", asan_dependencies)
        self.assertIn('"-DCMAKE_C_FLAGS=/fsanitize=address /Zi"', asan_dependencies)
        self.assertIn('"-DCMAKE_CXX_FLAGS=/fsanitize=address /EHsc /Zi /bigobj"', asan_dependencies)
        self.assertIn("x64-windows-asan.cmake", workflow)
        self.assertIn("Restore Windows ASan vcpkg binary cache", workflow)

    def test_nested_cmake_scripts_receive_forward_slash_path_substitutions(self):
        cmake_lists = (REPOSITORY / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        for source_name, normalized_name in (
            ("CMAKE_CURRENT_BINARY_DIR", "MC_TEST_BINARY_DIR"),
            ("CMAKE_COMMAND", "MC_CMAKE_COMMAND"),
            ("CMAKE_CTEST_COMMAND", "MC_CTEST_COMMAND"),
            ("CMAKE_C_COMPILER", "MC_CMAKE_C_COMPILER"),
            ("CMAKE_CXX_COMPILER", "MC_CMAKE_CXX_COMPILER"),
            ("PROJECT_SOURCE_DIR", "MC_PROJECT_SOURCE_DIR"),
            ("CMAKE_PREFIX_PATH", "MC_CMAKE_PREFIX_PATH"),
            ("CoreCpp_DIR", "MC_CORECPP_DIR"),
            ("CoreProject2026_DIR", "MC_COREPROJECT2026_DIR"),
        ):
            self.assertIn(f'file(TO_CMAKE_PATH "${{{source_name}}}" {normalized_name})', cmake_lists)
        for template in (REPOSITORY / "tests/cmake").glob("*.in"):
            contents = template.read_text(encoding="utf-8")
            self.assertNotRegex(contents, r"@(CMAKE_CURRENT_BINARY_DIR|CMAKE_COMMAND|CMAKE_CTEST_COMMAND|CMAKE_CXX_COMPILER|PROJECT_SOURCE_DIR|CMAKE_PREFIX_PATH|CoreCpp_DIR|CoreProject2026_DIR)@")
        for template_name in ("corecpp_server_consumer_test.cmake.in", "minecraftclone_server_only_test.cmake.in"):
            contents = (REPOSITORY / "tests/cmake" / template_name).read_text(encoding="utf-8")
            self.assertIn('set(MC_TEST_CONFIGURATION "@CMAKE_BUILD_TYPE@")', contents)
            self.assertIn('"-DCMAKE_BUILD_TYPE=${MC_TEST_CONFIGURATION}"', contents)
            self.assertIn('REGEX "^CMAKE_CONFIGURATION_TYPES:.*="', contents)
            self.assertIn("list(APPEND build_command --config \"${MC_TEST_CONFIGURATION}\")", contents)
            self.assertNotIn("-DCMAKE_BUILD_TYPE=Debug", contents)
        self.assertEqual(cmake_lists.count('"-DMC_TEST_CONFIGURATION=$<CONFIG>"'), 2)
        corecpp_template = (REPOSITORY / "tests/cmake/corecpp_server_consumer_test.cmake.in").read_text(encoding="utf-8")
        self.assertIn('-G "@MC_CMAKE_GENERATOR@"', corecpp_template)
        self.assertIn('"-DCMAKE_CXX_COMPILER=@MC_CMAKE_CXX_COMPILER@"', corecpp_template)
        self.assertIn('set(consumer_directory "${consumer_directory}/${MC_TEST_CONFIGURATION}")', corecpp_template)
        minecraftclone_template = (REPOSITORY / "tests/cmake/minecraftclone_server_only_test.cmake.in").read_text(encoding="utf-8")
        self.assertIn("list(APPEND test_command -C \"${MC_TEST_CONFIGURATION}\")", minecraftclone_template)

    def test_nested_server_build_preserves_instrumented_dependency_abi(self):
        fixture = (REPOSITORY / "tests/cmake/minecraftclone_server_only_test.cmake.in").read_text(encoding="utf-8")
        for variable, enclosing in (
            ("CMAKE_C_COMPILER", "MC_CMAKE_C_COMPILER"),
            ("CMAKE_CXX_COMPILER", "MC_CMAKE_CXX_COMPILER"),
            ("CMAKE_C_FLAGS", "CMAKE_C_FLAGS"),
            ("CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS"),
            ("CMAKE_EXE_LINKER_FLAGS", "CMAKE_EXE_LINKER_FLAGS"),
            ("CMAKE_SHARED_LINKER_FLAGS", "CMAKE_SHARED_LINKER_FLAGS"),
            ("MC_ENABLE_SANITIZERS", "MC_ENABLE_SANITIZERS"),
            ("MC_SANITIZER", "MC_SANITIZER"),
        ):
            with self.subTest(variable=variable):
                self.assertIn(f"-D{variable}=@{enclosing}@", fixture)
        self.assertIn("-DMC_REGISTER_SERVER_ONLY_BUILD_TEST=OFF", fixture)
        self.assertIn("-DVCPKG_MANIFEST_INSTALL=OFF", fixture)
        self.assertIn("--target mc_tests --parallel 4", fixture)
        self.assertIn('if(NOT "@CMAKE_MSVC_RUNTIME_LIBRARY@" STREQUAL "")', fixture)
        self.assertIn("-DCMAKE_MSVC_RUNTIME_LIBRARY=@CMAKE_MSVC_RUNTIME_LIBRARY@", fixture)

    def test_msvc_c6326_exemption_is_private_to_mc_tests(self):
        cmake_lists = (REPOSITORY / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        target_start = cmake_lists.index("add_executable(mc_tests")
        target_end = cmake_lists.index("if(WIN32)", target_start)
        test_target = cmake_lists[target_start:target_end]
        presets = (REPOSITORY / "CMakePresets.json").read_text(encoding="utf-8")

        self.assertRegex(
            test_target,
            r"(?s)mc_apply_target_defaults\(mc_tests\).*?"
            r"if\(MSVC\).*?target_compile_options\(mc_tests PRIVATE /wd6326\).*?endif\(\)",
        )
        self.assertEqual(cmake_lists.count("/wd6326"), 1)
        self.assertNotIn("/wd6326", presets)
        self.assertIn('"CMAKE_CXX_FLAGS": "/analyze /W4 /WX /EHsc /analyze:external-"', presets)

    def test_server_only_build_timeout_covers_nested_server_test(self):
        cmake_lists = (REPOSITORY / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("if(WIN32)\n        set_tests_properties(MinecraftClone.ServerOnlyBuild PROPERTIES TIMEOUT 900)", cmake_lists)
        self.assertIn("else()\n        set_tests_properties(MinecraftClone.ServerOnlyBuild PROPERTIES TIMEOUT 300)", cmake_lists)

    def test_nested_package_builds_run_serially(self):
        cmake_lists = (REPOSITORY / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        for test_name in (
            "CoreCpp.ServerOnlyPackageConsumer",
            "MinecraftClone.ServerOnlyBuild",
        ):
            with self.subTest(test_name=test_name):
                self.assertRegex(
                    cmake_lists,
                    rf"set_tests_properties\(\s*{re.escape(test_name)}\s+PROPERTIES\s+RUN_SERIAL TRUE\s*\)",
                )


if __name__ == "__main__":
    unittest.main()
