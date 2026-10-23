from __future__ import annotations

import copy
from contextlib import ExitStack, redirect_stdout
import hashlib
import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile
from pathlib import Path
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "script/ai_analysis_matrix.py"
MANIFEST_PATH = REPOSITORY / "script/ai_analysis_matrix.json"
HEAD = "a" * 40
TREE = "b" * 40
CANDIDATE = {"head": HEAD, "tree": TREE}


def load_module():
    spec = importlib.util.spec_from_file_location("ai_analysis_matrix", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class AnalysisMatrixTests(unittest.TestCase):
    def setUp(self):
        self.matrix = load_module()
        self.manifest = self.matrix.load_manifest(MANIFEST_PATH)

    @unittest.skipIf(os.name == "nt", "POSIX session registration")
    def test_owned_session_marker_records_actual_child_start_and_owner(self):
        environment = {**os.environ, "MC_ANALYSIS_SESSION_TOKEN": "normal-session-control"}
        output = io.StringIO()
        with redirect_stdout(output):
            completed = self.matrix._run_with_liveness(
                [sys.executable, "-c", "import time; time.sleep(.15); print('normal child')"],
                cwd=REPOSITORY, environment=environment, timeout_seconds=2, row_id="normal-control",
            )
        marker = next(line for line in output.getvalue().splitlines() if line.startswith("MC_ANALYSIS_OWNED_SESSION "))
        registration = json.loads(marker.split(" ", 1)[1])
        self.assertEqual(completed.returncode, 0)
        self.assertIn("normal child", completed.stdout)
        self.assertEqual(registration["token"], environment["MC_ANALYSIS_SESSION_TOKEN"])
        self.assertEqual(registration["owner_pid"], os.getpid())
        self.assertEqual(registration["identity"]["pid"], registration["identity"]["pgid"])
        self.assertEqual(registration["identity"]["ppid"], os.getpid())
        self.assertTrue(registration["identity"]["started"])

    @unittest.skipIf(os.name == "nt", "POSIX session registration")
    def test_owned_session_timeout_preserves_marker_and_primary_failure(self):
        output = io.StringIO()
        with redirect_stdout(output), self.assertRaises(subprocess.TimeoutExpired) as raised:
            self.matrix._run_with_liveness(
                [sys.executable, "-u", "-c", "import time; print('timeout control'); time.sleep(1)"],
                cwd=REPOSITORY, environment={**os.environ, "MC_ANALYSIS_SESSION_TOKEN": "timeout-session-control"},
                timeout_seconds=.15, row_id="timeout-control",
            )
        self.assertIn("MC_ANALYSIS_OWNED_SESSION ", output.getvalue())
        self.assertIn("timeout control", raised.exception.output)
        self.assertEqual(raised.exception.timeout, .15)

    def test_timeout_kills_descendant_holding_pipe_after_command_exits(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            marker = root / "survived"
            child = (
                "import pathlib,signal,time; "
                "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                "print('descendant ready', flush=True); time.sleep(2); "
                f"pathlib.Path({str(marker)!r}).write_text('alive')"
            )
            command = [sys.executable, "-c", (
                "import subprocess,sys; "
                f"subprocess.Popen([sys.executable, '-c', {child!r}])"
            )]
            started = time.monotonic()
            try:
                # Windows closes the owned job when the command exits; POSIX
                # keeps waiting on the inherited pipe until the row deadline.
                result = self.matrix._run_with_liveness(
                    command, cwd=root, environment=os.environ.copy(),
                    timeout_seconds=0.4, row_id="exited-leader",
                )
                self.assertEqual(os.name, "nt")
                self.assertEqual(result.returncode, 0)
            except subprocess.TimeoutExpired:
                pass
            self.assertLess(time.monotonic() - started, 3)
            time.sleep(2)
            self.assertFalse(marker.exists())

    def test_probe_timeout_preserves_output_and_marks_row_failed(self):
        row = self.manifest["rows"][0]
        row["platform"] = self.matrix._host_platform()
        with tempfile.TemporaryDirectory() as directory, \
            mock.patch.object(self.matrix, "require_clean_checkout"), \
            mock.patch.object(self.matrix, "_row_supported_on_host", return_value=True), \
            mock.patch.dict(self.matrix.REQUIRED_ROW_ENVIRONMENT, {row["id"]: ()}), \
            mock.patch.object(self.matrix, "_run_with_liveness", side_effect=subprocess.TimeoutExpired(
                ["tool"], 1, output=b"partial probe\xff", stderr=" stderr retained"
            )):
            receipt = Path(directory) / "row.json"
            self.matrix.run_row(row["id"], receipt, MANIFEST_PATH, root=REPOSITORY, expected_candidate=CANDIDATE)
            result = json.loads(receipt.read_text())["rows"][0]
            self.assertEqual(result["status"], "failed")
            self.assertIn("partial probe", result["diagnostics"])
            self.assertIn("\ufffd", result["diagnostics"])
            self.assertIn("stderr retained", result["diagnostics"])

    @unittest.skipIf(os.name == "nt", "POSIX process-group permission failure")
    def test_timeout_retains_output_when_group_kill_is_denied_and_pipe_drain_times_out(self):
        process = mock.Mock(pid=42)
        process.communicate.side_effect = [
            subprocess.TimeoutExpired(["ctest"], 1, output=b"646/729 tests\npartial\xff"),
            subprocess.TimeoutExpired(["ctest"], 5, output=b"646/729 tests\npartial\xff\nlast output"),
        ]
        with mock.patch.object(self.matrix.subprocess, "Popen", return_value=process), \
                mock.patch.object(self.matrix.os, "killpg", side_effect=[None, PermissionError("SIGKILL denied")]) as killpg, \
                mock.patch.object(self.matrix.time, "monotonic", side_effect=[0, 0, 1, 1]):
            with self.assertRaises(subprocess.TimeoutExpired) as raised:
                self.matrix._run_with_liveness(["ctest"], cwd=REPOSITORY, environment={}, timeout_seconds=1, row_id="permission-test")
        self.assertEqual(raised.exception.cmd, ["ctest"])
        self.assertEqual(raised.exception.timeout, 1)
        self.assertIn("646/729 tests\npartial\ufffd\nlast output", raised.exception.output)
        self.assertIn("process-tree cleanup failed for PID 42", raised.exception.stderr)
        self.assertIn("SIGKILL denied", raised.exception.stderr)
        self.assertIn("output drain failed", raised.exception.stderr)
        self.assertIn("containment unconfirmed", raised.exception.stderr)
        self.assertEqual(killpg.call_count, 2)
        process.communicate.assert_called_with(timeout=5)
        process.stdout.close.assert_called_once_with()
        self.assertTrue(all(call.kwargs.get("timeout") == 5 for call in process.wait.call_args_list))

    @unittest.skipIf(os.name == "nt", "POSIX process-group permission failure")
    def test_row_receipt_retains_original_timeout_output_and_cleanup_permission_failure(self):
        row = self.manifest["rows"][0]
        real_run = self.matrix._run_with_liveness
        process = mock.Mock(pid=42)
        process.communicate.side_effect = [subprocess.TimeoutExpired(["ctest"], 1, output=b"646/729 completed\n"), ("646/729 completed\n", None)]

        def timed_out_command(argv, **kwargs):
            with mock.patch.object(self.matrix.subprocess, "Popen", return_value=process), \
                    mock.patch.object(self.matrix.os, "killpg", side_effect=PermissionError("group cleanup denied")), \
                    mock.patch.object(self.matrix.time, "monotonic", side_effect=[0, 0, 1, 1]):
                return real_run(argv, cwd=kwargs["cwd"], environment=kwargs["environment"], timeout_seconds=1, row_id=row["id"])

        with tempfile.TemporaryDirectory() as directory, mock.patch.object(self.matrix, "require_clean_checkout"), \
                self.patch_row_execution(row, timed_out_command):
            receipt = Path(directory) / "failed-row.json"
            result = self.matrix.run_row(row["id"], receipt, MANIFEST_PATH, root=REPOSITORY, expected_candidate=CANDIDATE)
            retained = json.loads(receipt.read_text(encoding="utf-8"))
        self.assertEqual(result, retained)
        self.assertEqual(retained["rows"][0]["status"], "failed")
        self.assertIn("timed out after 1 seconds", retained["rows"][0]["diagnostics"])
        self.assertIn("646/729 completed", retained["rows"][0]["diagnostics"])
        self.assertIn("group cleanup denied", retained["rows"][0]["diagnostics"])
        self.assertIn("containment unconfirmed", retained["rows"][0]["diagnostics"])
        self.assertEqual(retained["rows"][0]["executed_commands"], [])

    def test_timeout_retains_partial_output_when_final_pipe_read_raises_oserror(self):
        process = mock.Mock(pid=42)
        process.communicate.side_effect = OSError("pipe read denied")
        with mock.patch.object(self.matrix, "_terminate_process_tree"):
            with self.assertRaises(subprocess.TimeoutExpired) as raised:
                self.matrix._raise_timeout_after_cleanup(process, ["ctest"], 600, "646/729 completed")
        self.assertEqual(raised.exception.output, "646/729 completed")
        self.assertEqual(raised.exception.timeout, 600)
        self.assertIn("output drain failed", raised.exception.stderr)
        self.assertIn("pipe read denied", raised.exception.stderr)
        self.assertIn("containment unconfirmed", raised.exception.stderr)
        process.communicate.assert_called_once_with(timeout=5)

    @unittest.skipIf(os.name == "nt", "POSIX process-group bounded wait contract")
    def test_timeout_cleanup_never_reaps_with_an_unbounded_wait(self):
        process = mock.Mock(pid=42)
        process.wait.side_effect = subprocess.TimeoutExpired(["ctest"], 5)
        process.communicate.return_value = ("retained partial output", None)
        with mock.patch.object(self.matrix.os, "killpg"):
            with self.assertRaises(subprocess.TimeoutExpired) as raised:
                self.matrix._raise_timeout_after_cleanup(process, ["ctest"], 600, "retained partial output")
        self.assertIn("process-tree cleanup failed", raised.exception.stderr)
        self.assertIn("containment unconfirmed", raised.exception.stderr)
        self.assertEqual(process.wait.call_count, 3)
        self.assertTrue(all(call.kwargs == {"timeout": 5} for call in process.wait.call_args_list))
        process.kill.assert_called_once_with()

    def test_missing_probe_remains_unavailable(self):
        with mock.patch.object(self.matrix, "_run_with_liveness", side_effect=FileNotFoundError("missing")):
            _, _, error = self.matrix._capture_tool_probes(
                REPOSITORY, self.manifest["rows"][0], os.environ.copy()
            )
        self.assertIn("unavailable", error)

    @unittest.skipIf(os.name == "nt", "POSIX process-group contract")
    def test_timeout_kills_term_ignoring_child_after_group_leader_exits(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            marker = root / "child-finished"
            child = (
                "import pathlib,signal,time; "
                "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                "print('child ready', flush=True); time.sleep(2); "
                f"pathlib.Path({str(marker)!r}).write_text('alive')"
            )
            parent = (
                "import subprocess,sys,time; "
                f"subprocess.Popen([sys.executable, '-c', {child!r}]); "
                "time.sleep(20)"
            )
            with self.assertRaises(subprocess.TimeoutExpired):
                self.matrix._run_with_liveness(
                    [sys.executable, "-c", parent], cwd=root,
                    environment=os.environ.copy(), timeout_seconds=0.3, row_id="tree-test",
                )
            time.sleep(2)
            self.assertFalse(marker.exists(), "TERM-ignoring descendant survived timeout")

    def test_recursive_input_inventory_includes_nested_files_on_windows_glob_semantics(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nested_source = root / "src" / "deep" / "game.cpp"
            nested_source.parent.mkdir(parents=True)
            nested_source.write_text("version one\n", encoding="utf-8")

            native_glob = Path.glob

            def windows_glob(path, pattern):
                matches = native_glob(path, pattern)
                if pattern.endswith("/**"):
                    return (match for match in matches if match.is_dir())
                return matches

            with mock.patch.object(Path, "glob", windows_glob):
                first_paths = self.matrix._input_file_paths(root, ("src/**",))
                first_digest = self.matrix._input_files_sha256(root, ("src/**",))
                nested_source.write_text("version two\n", encoding="utf-8")
                second_digest = self.matrix._input_files_sha256(root, ("src/**",))

            self.assertEqual(set(first_paths), {"src/deep/game.cpp"})
            self.assertNotEqual(first_digest, second_digest)

    def receipt(self):
        rows = []
        for manifest_row in self.manifest["rows"]:
            row_id = manifest_row["id"]
            host_path_type = self.matrix.PureWindowsPath if manifest_row["platform"] == "windows" else Path
            environment = {
                name: f"/captured/{name.lower()}"
                for name in self.matrix.IDENTITY_ENVIRONMENT
            }
            environment.update({"PATH": "/captured/bin", "MC_ANALYSIS_ROOT": "/captured/root"})
            if manifest_row["platform"] == "windows":
                environment["VULKAN_SDK"] = "C:\\captured\\vulkan-sdk"
                environment["VCINSTALLDIR"] = "C:\\captured\\VC"
            elif manifest_row["platform"] == "macos":
                environment["VULKAN_SDK"] = "<unset>"
            if row_id == "linux_msan":
                environment["DEBUGINFOD_URLS"] = ""
            declared_environment = self.matrix.resolve_row_environment(
                manifest_row, Path("/captured/root"), environment
            )
            row_environment = {
                name: declared_environment[name]
                for name in manifest_row.get("environment", {})
            }
            probe_environment = declared_environment
            effective_commands = self.matrix._effective_command_argv(manifest_row, probe_environment)
            tools = [
                {
                    "probe_argv": self.matrix._expand_environment(probe, probe_environment),
                    "executable": (
                        f"C:\\captured\\bin\\{Path(probe[0]).name}"
                        if manifest_row["platform"] == "windows"
                        else f"/captured/bin/{Path(probe[0]).name}"
                    ),
                    "executable_size": 10,
                    "executable_mtime_ns": 1,
                    "executable_sha256": "a" * 64,
                    "version_sha256": "b" * 64,
                }
                for probe in manifest_row["tool_probes"]
            ]
            dependencies = {}
            for name in self.matrix.DEPENDENCY_ENVIRONMENT[row_id]:
                captured_path = environment[name]
                if name in {"VCPKG_ROOT", "MC_LLVM_PROJECT_ROOT"}:
                    dependency = {"path": captured_path, "revision": "1" * 40}
                    if name == "MC_LLVM_PROJECT_ROOT":
                        dependency["revision"] = self.matrix.MSAN_LLVM_BASE_REVISION
                        dependency["status"] = "clean"
                        dependency["helper_sha256"] = self.matrix.MSAN_HELPER_SHA256
                elif name == "MC_VCPKG_CHAINLOAD_TOOLCHAIN_FILE":
                    dependency = {"path": captured_path, "sha256": "c" * 64}
                elif name == "VCPKG_INSTALLED_DIR":
                    dependency = {
                        "path": captured_path,
                        "package_lists_sha256": "d" * 64,
                        "consumed_files_sha256": "e" * 64,
                        "consumed_file_count": 2,
                    }
                elif name in {"MC_MSAN_LIBCXX_PREFIX", "MC_LSAN_LIBCXX_PREFIX"}:
                    dependency = {"path": captured_path, "files_sha256": "f" * 64, "file_count": 2}
                else:
                    dependency = {
                        "path": captured_path,
                        "consumed_files_sha256": "9" * 64,
                        "consumed_file_count": 2,
                    }
                dependencies[name] = dependency
            family = self.matrix.RUNTIME_FAMILIES.get(row_id)
            if family is None:
                runtime_resources = {
                    "resource_directory": "not-applicable",
                    "files": [],
                    "msan_ignorelist": None,
                    "system_unwinder": None,
                }
            else:
                resource_directory = "/captured/resource"
                suffix = ".dylib" if manifest_row["platform"] == "macos" else ".a"
                names = [f"libclang_rt.{family[0]}{suffix}"]
                if row_id == "linux_lsan":
                    names = ["libclang_rt.lsan-aarch64.a"]
                if row_id == "linux_msan":
                    names = ["libclang_rt.msan-aarch64.a", "libclang_rt.msan_cxx-aarch64.a"]
                runtime_resources = {
                    "resource_directory": resource_directory,
                    "files": [
                        {
                            "name": name,
                            "path": f"{resource_directory}/lib/{'darwin' if manifest_row['platform'] == 'macos' else 'linux'}/{name}",
                            "size": 10,
                            "sha256": "8" * 64,
                        }
                        for name in names
                    ],
                    "msan_ignorelist": None,
                    "system_unwinder": None,
                }
                if row_id == "linux_msan":
                    runtime_resources["msan_ignorelist"] = {
                        "name": "msan_ignorelist.txt",
                        "path": f"{resource_directory}/share/msan_ignorelist.txt",
                        "size": 10,
                        "sha256": "7" * 64,
                    }
                    runtime_resources["system_unwinder"] = {
                        "selected_by": "ldd",
                        "probe_path": f"{environment['MC_MSAN_LIBCXX_PREFIX']}/lib/libc++abi.so.1",
                        "probe_sha256": "6" * 64,
                        "name": "libgcc_s.so.1",
                        "path": "/usr/lib/libgcc_s.so.1",
                        "size": 10,
                        "sha256": "5" * 64,
                    }
            cache_values = {}
            toolchain_resources = {"vulkan": None, "msvc_asan_runtime": None}
            if manifest_row["platform"] in {"macos", "windows"}:
                cache_values["MC_BUILD_CLIENT"] = "ON"
                if manifest_row["platform"] == "windows":
                    sdk_root = environment["VULKAN_SDK"]
                    include_directory = r"C:\captured\vcpkg\installed\x64-windows\include"
                    cache_values.update(
                        {
                            "Vulkan_INCLUDE_DIR": include_directory,
                            "Vulkan_LIBRARY": str(host_path_type(sdk_root) / "Lib" / "vulkan-1.lib"),
                            "Vulkan_GLSLC_EXECUTABLE": str(host_path_type(sdk_root) / "Bin" / "glslc.exe"),
                            "MC_SPIRV_VAL_EXECUTABLE": str(host_path_type(sdk_root) / "Bin" / "spirv-val.exe"),
                            "MC_VULKAN_RUNTIME_DLL": r"C:\Windows\System32\vulkan-1.dll",
                            "CMAKE_CXX_COMPILER": r"C:\captured\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe",
                            "CMAKE_LINKER": r"C:\captured\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\link.exe",
                        }
                    )
                    if row_id == "windows_msvc_asan":
                        cache_values["MC_SANITIZER"] = "address"
                    runtime_loader_path = cache_values["MC_VULKAN_RUNTIME_DLL"]
                else:
                    include_directory = "/captured/vcpkg/arm64-osx/include"
                    cache_values.update(
                        {
                            "Vulkan_INCLUDE_DIR": include_directory,
                            "Vulkan_LIBRARY": "/usr/local/lib/libvulkan.dylib",
                            "Vulkan_GLSLC_EXECUTABLE": "/opt/homebrew/bin/glslc",
                            "MC_SPIRV_VAL_EXECUTABLE": "/usr/local/bin/spirv-val",
                        }
                    )
                    runtime_loader_path = None
                toolchain_resources["vulkan"] = {
                    "sdk_root": environment["VULKAN_SDK"],
                    "include_directory": include_directory,
                    "headers_sha256": "1" * 64,
                    "header_file_count": 2,
                    "library": {
                        "name": host_path_type(cache_values["Vulkan_LIBRARY"]).name,
                        "path": cache_values["Vulkan_LIBRARY"],
                        "size": 10,
                        "sha256": "2" * 64,
                    },
                    "glslc": {
                        "name": host_path_type(cache_values["Vulkan_GLSLC_EXECUTABLE"]).name,
                        "path": cache_values["Vulkan_GLSLC_EXECUTABLE"],
                        "size": 10,
                        "sha256": "3" * 64,
                    },
                    "spirv_val": {
                        "name": host_path_type(cache_values["MC_SPIRV_VAL_EXECUTABLE"]).name,
                        "path": cache_values["MC_SPIRV_VAL_EXECUTABLE"],
                        "size": 10,
                        "sha256": "4" * 64,
                    },
                    "runtime_loader": (
                        {
                            "name": "vulkan-1.dll",
                            "path": runtime_loader_path,
                            "size": 10,
                            "sha256": "5" * 64,
                        }
                        if runtime_loader_path
                        else None
                    ),
                }
                if row_id == "windows_msvc_asan":
                    linker_parent = str(host_path_type(cache_values["CMAKE_LINKER"]).parent)
                    toolchain_resources["msvc_asan_runtime"] = {
                        "compiler": cache_values["CMAKE_CXX_COMPILER"],
                        "linker": cache_values["CMAKE_LINKER"],
                        "vc_install_directory": environment["VCINSTALLDIR"],
                        "files": [
                            {
                                "role": "dynamic_runtime",
                                "name": "clang_rt.asan_dynamic-x86_64.dll",
                                "path": str(host_path_type(linker_parent) / "clang_rt.asan_dynamic-x86_64.dll"),
                                "size": 10,
                                "sha256": "6" * 64,
                            }
                        ],
                    }
            if row_id == "linux_lsan":
                cache_values["VCPKG_TARGET_TRIPLET"] = environment["MC_ANALYSIS_LINUX_LSAN_TRIPLET"]
            elif row_id == "linux_msan":
                cache_values["VCPKG_TARGET_TRIPLET"] = environment["MC_ANALYSIS_LINUX_MSAN_TRIPLET"]
            input_identity = {
                "identity_schema_version": 4,
                "source_input_sha256": "6" * 64,
                "row_configuration_sha256": self.matrix._row_configuration_sha256(manifest_row),
                "platform": {
                    "name": manifest_row["platform"],
                    "system": f"{manifest_row['platform']} test host",
                    "machine": "test-machine",
                    "python": "3.test",
                },
                "environment": {
                    **{
                        name: declared_environment.get(name, "<unset>")
                        for name in self.matrix.IDENTITY_ENVIRONMENT
                    },
                    "MC_ANALYSIS_ROOT": environment["MC_ANALYSIS_ROOT"],
                },
                "row_environment": row_environment,
                "tool_identities": tools,
                "effective_commands": effective_commands,
                "external_dependencies": dependencies,
                "effective_cmake_cache": {"status": "present", "values": cache_values},
                "runtime_resources": runtime_resources,
                "toolchain_resources": toolchain_resources,
            }
            input_identity["input_sha256"] = hashlib.sha256(
                self.matrix._canonical_json(input_identity)
            ).hexdigest()
            effective_analysis = None
            if row_id in self.matrix.REQUIRED_ANALYSIS_CONFIGURATION:
                effective_analysis = {
                    "configuration_sha256": "e" * 64,
                    "configuration_key": self.matrix.REQUIRED_ANALYSIS_CONFIGURATION[row_id][1],
                    "observed_settings": self.matrix.REQUIRED_ANALYSIS_CONFIGURATION[row_id][2],
                    "invocation_count": 1,
                    "observed_tools_and_flags": self.matrix.ANALYSIS_TOOL_MARKERS[row_id],
                    "build_output_sha256": "f" * 64,
                }
                if row_id in self.matrix.PER_TRANSLATION_UNIT_ANALYSIS:
                    unit = {
                        "source": "src/main.cpp",
                        "source_sha256": "1" * 64,
                        "source_argv_sha256": "2" * 64,
                        "invoked_tool": self.matrix.PER_TRANSLATION_UNIT_ANALYSIS[row_id]["tool"],
                        "status": "passed",
                        "record_sha256": "3" * 64,
                        "report_sha256": "4" * 64,
                    }
                    effective_analysis.update(
                        {
                            "translation_unit_count": 1,
                            "translation_unit_inventory_sha256": self.matrix._translation_unit_inventory_sha256([unit]),
                            "project_input_sha256": "5" * 64,
                            "translation_units": [unit],
                        }
                    )
            rows.append(
                {
                    "id": row_id,
                    "proof_candidate": dict(CANDIDATE),
                    "proof_origin": "local",
                    "row_configuration_sha256": self.matrix._row_configuration_sha256(manifest_row),
                    "input_identity": input_identity,
                    "status": "passed",
                    "flags": list(manifest_row["flags"]),
                    "environment": dict(manifest_row.get("environment", {})),
                    "tool_versions": {f"tool_{index}": "tool 1.0" for index, _ in enumerate(tools)},
                    "commands": [list(command) for command in manifest_row["commands"]],
                    "executed_commands": [
                        {
                            "declared_command": list(command),
                            "argv": list(effective_commands[index]),
                            "return_code": 0,
                            "output_sha256": "d" * 64,
                            "output_bytes": 12,
                        }
                        for index, command in enumerate(manifest_row["commands"])
                    ],
                    "effective_compile_flags": (
                        {
                            "compile_commands_sha256": "c" * 64,
                            "command_count": 1,
                            "observed_flags": list(manifest_row["required_compile_flags"]),
                        }
                        if "required_compile_flags" in manifest_row
                        else None
                    ),
                    "effective_analysis": effective_analysis,
                    "diagnostics": "no diagnostics",
                    "diagnostics_sha256": hashlib.sha256(b"no diagnostics").hexdigest(),
                    "duration_seconds": 1,
                }
            )
        return {
            "schema_version": 2,
            "task_id": "MC-AI-0249",
            "manifest_sha256": self.matrix.manifest_sha256(self.manifest),
            "candidate": dict(CANDIDATE),
            "source": dict(CANDIDATE),
            "runner": {"name": "analysis-matrix-merge", "rows": len(rows)},
            "rows": rows,
        }

    def rehash_input_identity(self, row):
        identity = row["input_identity"]
        preimage = dict(identity)
        preimage.pop("input_sha256")
        identity["input_sha256"] = hashlib.sha256(
            self.matrix._canonical_json(preimage)
        ).hexdigest()

    def patch_row_execution(self, row, command_side_effect):
        row["platform"] = self.matrix._host_platform()
        environment = self.matrix.resolve_row_environment(
            row, REPOSITORY, {"CXXFLAGS": "-nostdinc++ -isystem /captured/sdk/usr/include/c++/v1"}
        )
        tool_identities = [
            {"probe_argv": self.matrix._expand_environment(probe, environment)}
            for probe in row["tool_probes"]
        ]
        versions = {probe[0]: "tool 1.0\n" for probe in row["tool_probes"]}
        input_identity = {
            "effective_commands": self.matrix._effective_command_argv(row, environment)
        }
        stack = ExitStack()
        stack.enter_context(mock.patch.dict(self.matrix.REQUIRED_ROW_ENVIRONMENT, {row["id"]: ()}))
        stack.enter_context(mock.patch.object(self.matrix, "_row_supported_on_host", return_value=True))
        stack.enter_context(
            mock.patch.object(
                self.matrix,
                "_capture_tool_probes",
                return_value=(versions, tool_identities, None),
            )
        )
        stack.enter_context(
            mock.patch.object(self.matrix, "_row_input_identity", return_value=input_identity)
        )
        stack.enter_context(
            mock.patch.object(self.matrix, "_run_with_liveness", side_effect=command_side_effect)
        )
        return stack

    def create_input_scope_fixture(self, root: Path) -> None:
        preset_names = set()
        configure_names = set()
        build_names = set()
        for row in self.manifest["rows"]:
            for command in row["commands"]:
                for index, value in enumerate(command[:-1]):
                    if value == "--preset":
                        (build_names if "--build" in command[:index] else configure_names).add(command[index + 1])
        configure_names.update(build_names)
        preset_names.update(configure_names)
        for name in sorted(preset_names):
            configure = root / f"preset-config-{name}"
            configure.write_text("fixture\n", encoding="utf-8")
        (root / "CMakePresets.json").write_text(
            json.dumps(
                {
                    "version": 6,
                    "configurePresets": [{"name": name} for name in sorted(configure_names)],
                    "buildPresets": [
                        {"name": name, "configurePreset": name} for name in sorted(build_names)
                    ],
                }
            ),
            encoding="utf-8",
        )
        files = (
            "CMakeLists.txt",
            "cmake/build.cmake",
            "src/owned.cpp",
            "src/shared/include/shared/public.hpp",
            "tests/owned_test.cpp",
            "testdata/fixture.json",
            "assets/asset.txt",
            "scenarios/scenario.json",
            "dependencies.lock.json",
            "vcpkg.json",
            "vcpkg-configuration.json",
            "script/ai_analysis_matrix.py",
            "script/ci/acquire.py",
            "script/ci/build_snapshot.py",
            "script/ci/verify_macos_lsan.py",
            "script/ci/macos_lsan.supp",
            "script/ci/macos_lsan_control.cpp",
            "script/ci/vcpkg-triplets/x64-windows-asan.cmake",
            "script/ci/vcpkg-triplets/arm64-linux-lsan.cmake",
            "script/ci/vcpkg-triplets/x64-linux-lsan.cmake",
            "script/ci/vcpkg-triplets/arm64-linux-msan.cmake",
            "script/ci/vcpkg-triplets/x64-linux-msan.cmake",
            "script/ci/vcpkg-clang-toolchain.cmake",
            "script/ci/install_msan_libcxx.py",
            "script/ci/vcpkg-overlays/gmp/portfile.cmake",
            "script/ci/vcpkg-overlays/gmp/vcpkg.json",
            "script/ci/iwyu_gate.py",
            "script/ci/cppcheck_gate.py",
        )
        for relative in files:
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(f"fixture {relative}\n", encoding="utf-8")

    def test_documentation_does_not_stale_row_proof_but_owned_code_tests_headers_and_lock_do(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_input_scope_fixture(root)
            rows = {row["id"]: row for row in self.manifest["rows"]}
            before = {
                row_id: self.matrix.row_source_input_sha256(root, row)
                for row_id, row in rows.items()
            }
            docs = root / "docs/notes.md"
            docs.parent.mkdir(parents=True)
            docs.write_text("documentation-only edit\n", encoding="utf-8")
            self.assertEqual(
                before["macos_asan"],
                self.matrix.row_source_input_sha256(root, rows["macos_asan"]),
            )
            for relative in (
                "src/owned.cpp",
                "tests/owned_test.cpp",
                "src/shared/include/shared/public.hpp",
                "dependencies.lock.json",
            ):
                target = root / relative
                target.write_text(target.read_text(encoding="utf-8") + "changed\n", encoding="utf-8")
                for row_id, row in rows.items():
                    with self.subTest(input=relative, row=row_id):
                        self.assertNotEqual(
                            before[row_id],
                            self.matrix.row_source_input_sha256(root, row),
                        )

    def test_platform_specific_triplet_and_overlay_changes_leave_other_rows_reusable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_input_scope_fixture(root)
            rows = {row["id"]: row for row in self.manifest["rows"]}
            tracked_ids = ("macos_asan", "windows_msvc_asan", "linux_lsan", "linux_msan")
            before = {
                row_id: self.matrix.row_source_input_sha256(root, rows[row_id])
                for row_id in tracked_ids
            }
            lsan_triplet = root / "script/ci/vcpkg-triplets/arm64-linux-lsan.cmake"
            lsan_triplet.write_text("changed native LSan triplet\n", encoding="utf-8")
            after_lsan = {
                row_id: self.matrix.row_source_input_sha256(root, rows[row_id])
                for row_id in tracked_ids
            }
            self.assertNotEqual(before["linux_lsan"], after_lsan["linux_lsan"])
            for row_id in ("macos_asan", "windows_msvc_asan", "linux_msan"):
                self.assertEqual(before[row_id], after_lsan[row_id], row_id)

            overlay = root / "script/ci/vcpkg-overlays/gmp/portfile.cmake"
            overlay.write_text("changed GMP overlay\n", encoding="utf-8")
            after_overlay = {
                row_id: self.matrix.row_source_input_sha256(root, rows[row_id])
                for row_id in tracked_ids
            }
            self.assertNotEqual(after_lsan["windows_msvc_asan"], after_overlay["windows_msvc_asan"])
            self.assertNotEqual(after_lsan["linux_lsan"], after_overlay["linux_lsan"])
            self.assertNotEqual(after_lsan["linux_msan"], after_overlay["linux_msan"])
            self.assertEqual(after_lsan["macos_asan"], after_overlay["macos_asan"])

    def create_iwyu_fixture(self, root: Path, *, source_count: int = 1, receipt_count: int | None = None):
        for relative in (
            "script/ai_analysis_matrix.py",
            "script/ai_analysis_matrix.json",
            "script/ci/iwyu_gate.py",
            "script/ci/cppcheck_gate.py",
        ):
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("analysis fingerprint fixture\n", encoding="utf-8")

        (root / "CMakePresets.json").write_text(
            json.dumps(
                {
                    "version": 6,
                    "configurePresets": [
                        {"name": "analysis-iwyu"},
                        {"name": "analysis-cppcheck"},
                    ],
                    "buildPresets": [
                        {"name": "analysis-iwyu", "configurePreset": "analysis-iwyu"},
                        {"name": "analysis-cppcheck", "configurePreset": "analysis-cppcheck"},
                    ],
                }
            ),
            encoding="utf-8",
        )

        preset, cache_key, settings = self.matrix.REQUIRED_ANALYSIS_CONFIGURATION["iwyu"]
        build_directory = root / "build" / preset
        build_directory.mkdir(parents=True, exist_ok=True)
        (build_directory / "CMakeCache.txt").write_text(
            f"{cache_key}:STRING={';'.join(settings)}\n", encoding="utf-8"
        )

        compile_commands = []
        sources = []
        for index in range(source_count):
            source = root / "src" / f"unit_{index}.cpp"
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text(f"int unit_{index}() {{ return {index}; }}\n", encoding="utf-8")
            source.with_suffix(".hpp").write_text("struct InitialHeader {};\n", encoding="utf-8")
            sources.append(source)
            compile_commands.append(
                {
                    "directory": str(root),
                    "file": str(source),
                    "arguments": [
                        "clang++",
                        "-std=c++23",
                        "-c",
                        str(source),
                        "-o",
                        str(build_directory / f"unit_{index}.o"),
                    ],
                }
            )
        (build_directory / "compile_commands.json").write_text(
            json.dumps(compile_commands), encoding="utf-8"
        )

        gate_directory = root / self.matrix.PER_TRANSLATION_UNIT_ANALYSIS["iwyu"]["receipt_directory"]
        gate_directory.mkdir(parents=True, exist_ok=True)
        input_digest = self.matrix.project_analysis_input_sha256(root)
        if receipt_count is None:
            receipt_count = source_count
        for source in sources[:receipt_count]:
            source_relative = source.relative_to(root).as_posix()
            compiler_argv = [
                "clang++",
                "-std=c++23",
                "-c",
                str(source),
                "-o",
                str(build_directory / (source.stem + ".o")),
            ]
            output_index = compiler_argv.index("-o")
            object_path = compiler_argv[output_index + 1]
            source_argv = [
                "--driver-mode=g++",
                *compiler_argv[1:output_index],
                "-MD",
                "-MT",
                object_path,
                "-MF",
                object_path + ".d",
                *compiler_argv[output_index:],
            ]
            source_argv_json = json.dumps(source_argv, separators=(",", ":")).encode("utf-8")
            stem = hashlib.sha256(source_relative.encode("utf-8") + b"\0" + source_argv_json).hexdigest()
            log_path = gate_directory / f"{stem}.log"
            record_path = gate_directory / f"{stem}.json"
            raw_report = b"IWYU complete report\n"
            log_path.write_bytes(raw_report)
            record = {
                "schema_version": 1,
                "source": source_relative,
                "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "project_input_sha256": input_digest,
                "source_argv": source_argv,
                "adapter_argv": [
                    "include-what-you-use",
                    "-Xiwyu",
                    "--no_fwd_decls",
                    "-Xiwyu",
                    "--error",
                    "--",
                    *source_argv,
                ],
                "invoked_tool": "include-what-you-use",
                "tool_argv": [
                    "include-what-you-use",
                    "-Xiwyu",
                    "--no_fwd_decls",
                    *source_argv,
                ],
                "tool_exit_code": 0,
                "report_sha256": hashlib.sha256(raw_report).hexdigest(),
                "status": "passed",
                "public_additions": [],
                "removals": [],
                "forward_declarations": [],
                "mapped_facades": [],
                "unsupported_suggestions": [],
                "log": log_path.relative_to(root).as_posix(),
                "record": record_path.relative_to(root).as_posix(),
            }
            record_path.write_text(json.dumps(record), encoding="utf-8")
        return sources

    def add_iwyu_history_record(
        self,
        root: Path,
        template_record_path: Path,
        *,
        status: str,
        source: str | None = None,
        malformed_identity: bool = False,
    ) -> tuple[Path, Path]:
        record = json.loads(template_record_path.read_text(encoding="utf-8"))
        record["source"] = source or record["source"]
        record["source_argv"] = [*record["source_argv"]]
        record["source_argv"].insert(1, "-DSTALE_IWYU_INVOCATION=1")
        record["adapter_argv"] = [
            "include-what-you-use",
            "-Xiwyu",
            "--no_fwd_decls",
            "-Xiwyu",
            "--error",
            "--",
            *record["source_argv"],
        ]
        record["tool_argv"] = [
            "include-what-you-use",
            "-Xiwyu",
            "--no_fwd_decls",
            *record["source_argv"],
        ]
        record["status"] = status
        record["tool_exit_code"] = 2 if status == "error" else 0
        record["diagnostic"] = "historical invocation result" if status == "error" else None
        raw_report = f"{status} historical IWYU output\n".encode("utf-8")
        record["report_sha256"] = hashlib.sha256(raw_report).hexdigest()
        source_argv_json = json.dumps(record["source_argv"], separators=(",", ":")).encode("utf-8")
        stem = hashlib.sha256(record["source"].encode("utf-8") + b"\0" + source_argv_json).hexdigest()
        gate_directory = root / self.matrix.PER_TRANSLATION_UNIT_ANALYSIS["iwyu"]["receipt_directory"]
        record_path = gate_directory / f"{'malformed-history' if malformed_identity else stem}.json"
        log_path = gate_directory / f"{record_path.stem}.log"
        record["log"] = log_path.relative_to(root).as_posix()
        record["record"] = record_path.relative_to(root).as_posix()
        log_path.write_bytes(raw_report)
        record_path.write_text(json.dumps(record), encoding="utf-8")
        return record_path, log_path

    def create_cppcheck_fixture(self, root: Path, *, source_count: int = 1, receipt_count: int | None = None):
        sources = self.create_iwyu_fixture(root, source_count=source_count, receipt_count=0)
        preset, cache_key, settings = self.matrix.REQUIRED_ANALYSIS_CONFIGURATION["cppcheck"]
        build_directory = root / "build" / preset
        build_directory.mkdir(parents=True, exist_ok=True)
        (build_directory / "CMakeCache.txt").write_text(
            f"{cache_key}:STRING={';'.join(settings)}\n"
            "CMAKE_C_CPPCHECK:STRING=\n"
            "CMAKE_CXX_CPPCHECK:STRING=\n",
            encoding="utf-8",
        )
        compile_commands = json.loads(
            (root / "build" / "analysis-iwyu" / "compile_commands.json").read_text(encoding="utf-8")
        )
        for index, entry in enumerate(compile_commands):
            entry["arguments"][-1] = str(build_directory / f"unit_{index}.o")
        (build_directory / "compile_commands.json").write_text(json.dumps(compile_commands), encoding="utf-8")

        gate_directory = root / self.matrix.PER_TRANSLATION_UNIT_ANALYSIS["cppcheck"]["receipt_directory"]
        gate_directory.mkdir(parents=True, exist_ok=True)
        input_digest = self.matrix.project_analysis_input_sha256(root)
        if receipt_count is None:
            receipt_count = source_count
        severity_counts = {
            "error": 0,
            "warning": 0,
            "performance": 0,
            "portability": 0,
            "style": 0,
            "information": 0,
            "debug": 0,
        }
        for index, source in enumerate(sources[:receipt_count]):
            source_relative = source.relative_to(root).as_posix()
            compile_argv = compile_commands[index]["arguments"]
            output_index = compile_argv.index("-o")
            object_path = compile_argv[output_index + 1]
            compiler_argv = [
                *compile_argv[:output_index],
                "-MD",
                "-MT",
                object_path,
                "-MF",
                object_path + ".d",
                *compile_argv[output_index:],
            ]
            cppcheck_argv = ["cppcheck", "--enable=all", "--inline-suppr", "--xml", str(source)]
            source_sha256 = hashlib.sha256(source.read_bytes()).hexdigest()
            identity = hashlib.sha256(
                self.matrix._canonical_json(
                    {
                        "source_absolute": str(source.resolve()),
                        "source_sha256": source_sha256,
                        "project_input_sha256": input_digest,
                        "cppcheck_argv": cppcheck_argv,
                        "compiler_argv": compiler_argv,
                        "cwd": str(root.resolve()),
                    }
                )
            ).hexdigest()
            record_path = gate_directory / f"{identity}.json"
            xml_path = gate_directory / f"{identity}.xml"
            raw_report = b"<?xml version=\"1.0\"?><results version=\"2\"/>\n"
            xml_path.write_bytes(raw_report)
            record = {
                "schema": 1,
                "gate": "cppcheck-per-tu",
                "status": "pass",
                "failure_stage": None,
                "cwd": str(root.resolve()),
                "source": {
                    "absolute": str(source.resolve()),
                    "relative": source_relative,
                    "sha256": source_sha256,
                },
                "identity_sha256": identity,
                "project_input_sha256": input_digest,
                "raw_report_sha256": hashlib.sha256(raw_report).hexdigest(),
                "cppcheck": {
                    "argv": cppcheck_argv,
                    "version": "2.17",
                    "returncode": 0,
                    "elapsed_ms": 1,
                },
                "compiler": {"argv": compiler_argv, "returncode": 0, "elapsed_ms": 1},
                "summary": {"blocking": 0, "advisory": 0, "by_severity": severity_counts},
                "diagnostics": [],
                "artifacts": {
                    "root": str(gate_directory.resolve()),
                    "xml": str(xml_path.resolve()),
                    "receipt": str(record_path.resolve()),
                },
            }
            record_path.write_text(json.dumps(record), encoding="utf-8")
        return sources

    def test_manifest_contains_exact_acceptance_rows(self):
        self.assertEqual(
            tuple(row["id"] for row in self.manifest["rows"]),
            self.matrix.REQUIRED_ROW_IDS,
        )
        self.assertTrue(
            all(row["timeout_seconds"] <= self.matrix.MAX_ROW_TIMEOUT_SECONDS for row in self.manifest["rows"])
        )

    def test_manifest_rejects_a_row_over_the_bounded_stage_limit(self):
        manifest = copy.deepcopy(self.manifest)
        manifest["rows"][0]["timeout_seconds"] = self.matrix.MAX_ROW_TIMEOUT_SECONDS + 1
        with self.assertRaisesRegex(self.matrix.MatrixError, "between 1 and 3600"):
            self.matrix.validate_manifest(manifest)

    def test_tool_probes_use_tree_terminating_runner_with_remaining_row_budget(self):
        row = {
            "id": "probe-budget-test",
            "tool_probes": [[sys.executable, "--version"]],
        }
        completed = subprocess.CompletedProcess([sys.executable, "--version"], 0, "tool 1.0\n", "")
        with mock.patch.object(self.matrix, "_run_with_liveness", return_value=completed) as run:
            versions, identities, error = self.matrix._capture_tool_probes(
                REPOSITORY, row, {}, timeout_seconds=42
            )

        self.assertIsNone(error)
        self.assertEqual(versions[sys.executable], "tool 1.0")
        self.assertEqual(len(identities), 1)
        run.assert_called_once()
        self.assertGreater(run.call_args.kwargs["timeout_seconds"], 0)
        self.assertLessEqual(run.call_args.kwargs["timeout_seconds"], 30)
        self.assertEqual(run.call_args.kwargs["row_id"], "probe-budget-test")

    def test_clang_static_analysis_probes_scan_build_with_supported_help_option(self):
        row = next(item for item in self.manifest["rows"] if item["id"] == "clang_static_analysis")

        self.assertEqual(row["tool_probes"], [["clang", "--version"], ["scan-build", "--help"]])
        self.assertEqual(
            row["commands"],
            [
                [
                    "cmake",
                    "--preset",
                    "analysis-clang-static",
                    "-DCMAKE_CXX_FLAGS={env:CXXFLAGS} -Xclang -analyzer-output=text",
                ],
                ["cmake", "--build", "--preset", "analysis-clang-static"],
            ],
        )
        self.assertIn("CXXFLAGS", self.matrix.REQUIRED_ROW_ENVIRONMENT["clang_static_analysis"])

        cxxflags = "-nostdinc++ -isystem /captured/sdk/usr/include/c++/v1 -stdlib=libc++"
        environment = self.matrix.resolve_row_environment(row, REPOSITORY, {"CXXFLAGS": cxxflags})
        effective_commands = self.matrix._effective_command_argv(row, environment)
        self.assertEqual(
            effective_commands[0][-1],
            f"-DCMAKE_CXX_FLAGS={cxxflags} -Xclang -analyzer-output=text",
        )
        self.assertTrue(
            any(
                effective_commands[1][index : index + 2] == ["--parallel", "2"]
                for index in range(len(effective_commands[1]) - 1)
            )
        )
        self.assertIn("--verbose", effective_commands[1])

    def test_every_matrix_build_command_respects_host_parallel_cap(self):
        for row in self.manifest["rows"]:
            inherited_environment = {
                name: f"/captured/{name.lower()}"
                for name in self.matrix.IDENTITY_ENVIRONMENT
            }
            inherited_environment["CXXFLAGS"] = "-isystem /captured/sdk/usr/include/c++/v1"
            environment = self.matrix.resolve_row_environment(
                row, REPOSITORY, inherited_environment
            )
            for command in self.matrix._effective_command_argv(row, environment):
                if command[:2] == ["cmake", "--build"]:
                    with self.subTest(row=row["id"]):
                        self.assertTrue(
                            any(
                                command[index : index + 2] == ["--parallel", "1" if row["platform"] == "windows" else "2"]
                                for index in range(len(command) - 1)
                            )
                        )

    def test_analyzer_builds_collect_all_findings_without_ignoring_failures(self):
        for row in self.manifest["rows"]:
            environment = {
                name: f"/captured/{name.lower()}"
                for name in self.matrix.IDENTITY_ENVIRONMENT
            }
            environment["CXXFLAGS"] = "-isystem /captured/sdk/usr/include/c++/v1"
            resolved = self.matrix.resolve_row_environment(row, REPOSITORY, environment)
            for command in self.matrix._effective_command_argv(row, resolved):
                if command[:2] != ["cmake", "--build"]:
                    continue
                with self.subTest(row=row["id"]):
                    if row["id"] in self.matrix.REQUIRED_ANALYSIS_CONFIGURATION:
                        self.assertEqual(command[-3:], ["--", "-k", "0"])
                        self.assertEqual(command.count("--"), 1)
                        self.assertLess(command.index("--parallel"), command.index("--"))
                        self.assertLess(command.index("--verbose"), command.index("--"))
                    else:
                        self.assertNotIn("-k", command)

    def test_tsan_runs_the_full_suite_with_bounded_test_parallelism(self):
        row = next(item for item in self.manifest["rows"] if item["id"] == "macos_tsan")
        self.assertEqual(row["timeout_seconds"], 3600)
        self.assertEqual(row["commands"][-1], [
            "ctest", "--test-dir", "build/analysis-macos-tsan",
            "--output-on-failure", "--no-tests=error", "--parallel", "2",
        ])

    def test_msvc_rows_use_zero_exit_version_probe_and_keep_required_analysis_commands(self):
        rows = {row["id"]: row for row in self.manifest["rows"]}
        for row_id, preset, build_directory in (
            ("windows_msvc_asan", "analysis-windows-asan", "build/analysis-windows-asan"),
            ("windows_msvc_analyze", "analysis-windows-analyze", "build/analysis-windows-analyze"),
        ):
            with self.subTest(row=row_id):
                row = rows[row_id]
                self.assertEqual(row["tool_probes"], [["cl.exe", "/Bv", "/?"], ["cmake", "--version"]])
                self.assertEqual(
                    row["commands"],
                    [
                        ["cmake", "--preset", preset],
                        ["cmake", "--build", "--preset", preset],
                        [
                            "ctest", "--test-dir", build_directory, "--output-on-failure",
                            "--no-tests=error", "--parallel", "1",
                        ],
                    ],
                )

    def test_nonzero_msvc_version_probe_keeps_row_unavailable(self):
        row = next(item for item in self.manifest["rows"] if item["id"] == "windows_msvc_asan")
        failed_probe = self.matrix.subprocess.CompletedProcess(
            row["tool_probes"][0], 2, "", "cl.exe: error D8003: missing source filename"
        )

        with tempfile.TemporaryDirectory() as directory:
            receipt_path = Path(directory) / "row.json"
            with mock.patch.object(self.matrix, "require_clean_checkout"), mock.patch.dict(
                self.matrix.REQUIRED_ROW_ENVIRONMENT, {row["id"]: ()}
            ), mock.patch.object(self.matrix, "_row_supported_on_host", return_value=True), mock.patch.object(
                self.matrix, "_run_with_liveness", return_value=failed_probe
            ) as run:
                receipt = self.matrix.run_row(
                    row["id"], receipt_path, MANIFEST_PATH, root=REPOSITORY, expected_candidate=CANDIDATE
                )

        result = receipt["rows"][0]
        self.assertEqual(result["status"], "unavailable")
        self.assertIn("exited 2", result["diagnostics"])
        self.assertEqual(run.call_count, 1)

    def test_complete_candidate_bound_receipt_passes(self):
        self.assertIsNotNone(self.matrix.validate_receipt(self.receipt(), self.manifest, CANDIDATE))

    def test_missing_row_fails_closed(self):
        receipt = self.receipt()
        receipt["rows"].pop()
        with self.assertRaisesRegex(self.matrix.MatrixError, "missing rows"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_unavailable_row_cannot_be_release_evidence(self):
        receipt = self.receipt()
        receipt["rows"][0]["status"] = "unavailable"
        with self.assertRaisesRegex(self.matrix.MatrixError, "not passed"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_candidate_and_manifest_bindings_are_checked(self):
        receipt = self.receipt()
        receipt["candidate"]["tree"] = "c" * 40
        with self.assertRaisesRegex(self.matrix.MatrixError, "source binding|different candidate|requested candidate"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

        receipt = self.receipt()
        receipt["manifest_sha256"] = "x" * 64
        with self.assertRaisesRegex(self.matrix.MatrixError, "manifest_sha256"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

        receipt = self.receipt()
        receipt["manifest_sha256"] = "0" * 64
        self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_legacy_hosted_receipt_schema_fails_closed(self):
        receipt = self.receipt()
        receipt["schema_version"] = 1
        with self.assertRaisesRegex(self.matrix.MatrixError, "schema_version must be 2"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_missing_or_tampered_tool_and_input_configuration_evidence_fails_closed(self):
        receipt = self.receipt()
        row = receipt["rows"][0]
        row["input_identity"]["tool_identities"].pop()
        with self.assertRaisesRegex(self.matrix.MatrixError, "tool_identities is incomplete"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

        receipt = self.receipt()
        row = receipt["rows"][0]
        row["input_identity"]["tool_identities"][0]["executable_sha256"] = "0" * 64
        with self.assertRaisesRegex(self.matrix.MatrixError, "input_identity hash does not match"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

        receipt = self.receipt()
        row = receipt["rows"][0]
        row["row_configuration_sha256"] = "0" * 64
        with self.assertRaisesRegex(self.matrix.MatrixError, "configuration differs"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_selected_vulkan_resources_hash_cache_bound_headers_library_and_tools(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            include = root / "include"
            header = include / "vulkan/vulkan.h"
            video_header = include / "vk_video/vulkan_video_codecs_common.h"
            library = root / "lib/libvulkan.dylib"
            glslc = root / "bin/glslc"
            spirv_val = root / "bin/spirv-val"
            for path, contents in (
                (header, b"vulkan header"),
                (video_header, b"video header"),
                (library, b"vulkan loader library"),
                (glslc, b"glslc tool"),
                (spirv_val, b"spirv validator"),
            ):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(contents)
            row = next(row for row in self.manifest["rows"] if row["id"] == "macos_asan")
            cache = {
                "status": "present",
                "values": {
                    "MC_BUILD_CLIENT": "ON",
                    "Vulkan_INCLUDE_DIR": str(include),
                    "Vulkan_LIBRARY": str(library),
                    "Vulkan_GLSLC_EXECUTABLE": str(glslc),
                    "MC_SPIRV_VAL_EXECUTABLE": str(spirv_val),
                },
            }
            first = self.matrix._selected_toolchain_resources(
                row, {"VULKAN_SDK": "<unset>"}, cache, require_selected=True
            )
            self.assertEqual(first["vulkan"]["header_file_count"], 2)
            self.assertEqual(first["vulkan"]["library"]["sha256"], hashlib.sha256(library.read_bytes()).hexdigest())

            header.write_bytes(b"changed Vulkan header")
            library.write_bytes(b"changed Vulkan loader library")
            glslc.write_bytes(b"changed glslc tool")
            spirv_val.write_bytes(b"changed spirv validator")
            second = self.matrix._selected_toolchain_resources(
                row, {"VULKAN_SDK": "<unset>"}, cache, require_selected=True
            )
            self.assertNotEqual(first["vulkan"]["headers_sha256"], second["vulkan"]["headers_sha256"])
            self.assertNotEqual(first["vulkan"]["library"]["sha256"], second["vulkan"]["library"]["sha256"])
            self.assertNotEqual(first["vulkan"]["glslc"]["sha256"], second["vulkan"]["glslc"]["sha256"])
            self.assertNotEqual(first["vulkan"]["spirv_val"]["sha256"], second["vulkan"]["spirv_val"]["sha256"])

            library.unlink()
            with self.assertRaisesRegex(self.matrix.MatrixError, "Vulkan loader library.*missing or unreadable"):
                self.matrix._selected_toolchain_resources(
                    row, {"VULKAN_SDK": "<unset>"}, cache, require_selected=True
                )

    def test_unconfigured_vulkan_cache_is_allowed_before_configure_but_required_afterward(self):
        row = next(row for row in self.manifest["rows"] if row["id"] == "macos_asan")
        missing_cache = {"status": "missing"}
        self.assertEqual(
            self.matrix._selected_toolchain_resources(
                row, {"VULKAN_SDK": "<unset>"}, missing_cache, require_selected=False
            ),
            {"vulkan": None, "msvc_asan_runtime": None},
        )
        with self.assertRaisesRegex(self.matrix.MatrixError, "passed without a configured CMake cache"):
            self.matrix._selected_toolchain_resources(
                row, {"VULKAN_SDK": "<unset>"}, missing_cache, require_selected=True
            )

    def test_cmake_cache_identity_retains_selected_vulkan_and_linker_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache_path = root / "build/analysis-windows-asan/CMakeCache.txt"
            cache_path.parent.mkdir(parents=True)
            cache_path.write_text(
                "\n".join(
                    (
                        r"MC_BUILD_CLIENT:BOOL=ON",
                        r"MC_SPIRV_VAL_EXECUTABLE:FILEPATH=C:/sdk/Bin/spirv-val.exe",
                        r"MC_VULKAN_RUNTIME_DLL:FILEPATH=C:/Windows/System32/vulkan-1.dll",
                        r"Vulkan_INCLUDE_DIR:PATH=C:/vcpkg/include",
                        r"Vulkan_LIBRARY:FILEPATH=C:/sdk/Lib/vulkan-1.lib",
                        r"Vulkan_GLSLC_EXECUTABLE:FILEPATH=C:/sdk/Bin/glslc.exe",
                        r"CMAKE_CXX_COMPILER:FILEPATH=C:/VS/bin/cl.exe",
                        r"CMAKE_LINKER:FILEPATH=C:/VS/bin/link.exe",
                    )
                )
                + "\n",
                encoding="utf-8",
            )
            row = next(row for row in self.manifest["rows"] if row["id"] == "windows_msvc_asan")
            identity = self.matrix._cmake_cache_identity(root, row)
            for name in (
                "MC_BUILD_CLIENT",
                "MC_SPIRV_VAL_EXECUTABLE",
                "MC_VULKAN_RUNTIME_DLL",
                "Vulkan_INCLUDE_DIR",
                "Vulkan_LIBRARY",
                "Vulkan_GLSLC_EXECUTABLE",
                "CMAKE_CXX_COMPILER",
                "CMAKE_LINKER",
            ):
                self.assertIn(name, identity["values"])

    def test_offline_windows_vulkan_identity_requires_cache_bound_files_and_sdk_root(self):
        receipt = self.receipt()
        row = next(row for row in receipt["rows"] if row["id"] == "windows_msvc_analyze")
        row["input_identity"]["toolchain_resources"]["vulkan"]["library"]["path"] = (
            r"C:\captured\other-sdk\Lib\vulkan-1.lib"
        )
        self.rehash_input_identity(row)
        with self.assertRaisesRegex(self.matrix.MatrixError, "does not match its selected CMake/environment path"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

        receipt = self.receipt()
        row = next(row for row in receipt["rows"] if row["id"] == "windows_msvc_analyze")
        row["input_identity"]["environment"]["VULKAN_SDK"] = r"D:\captured\vulkan-sdk"
        self.rehash_input_identity(row)
        with self.assertRaisesRegex(self.matrix.MatrixError, "differs from captured VULKAN_SDK"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_offline_windows_loader_must_match_explicit_cached_runtime(self):
        receipt = self.receipt()
        self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)
        row = next(row for row in receipt["rows"] if row["id"] == "windows_msvc_analyze")
        row["input_identity"]["toolchain_resources"]["vulkan"]["runtime_loader"]["path"] = (
            r"C:\captured\vulkan-sdk\Bin\vulkan-1.dll"
        )
        self.rehash_input_identity(row)
        with self.assertRaisesRegex(self.matrix.MatrixError, "does not match its selected CMake/environment path"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_offline_identity_uses_recorded_platform_not_verifier_path_flavor(self):
        receipt = self.receipt()
        with mock.patch.object(self.matrix, "Path", self.matrix.PureWindowsPath):
            for row in receipt["rows"]:
                with self.subTest(row=row["id"]):
                    manifest_row = next(item for item in self.manifest["rows"] if item["id"] == row["id"])
                    self.matrix._validate_input_identity(row, manifest_row)

    def test_offline_windows_loader_requires_selected_absolute_cache_path(self):
        for selected in (None, "vulkan-1.dll"):
            with self.subTest(selected=selected):
                receipt = self.receipt()
                row = next(row for row in receipt["rows"] if row["id"] == "windows_msvc_analyze")
                values = row["input_identity"]["effective_cmake_cache"]["values"]
                if selected is None:
                    values.pop("MC_VULKAN_RUNTIME_DLL")
                else:
                    values["MC_VULKAN_RUNTIME_DLL"] = selected
                self.rehash_input_identity(row)
                with self.assertRaisesRegex(self.matrix.MatrixError, "absolute MC_VULKAN_RUNTIME_DLL"):
                    self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_offline_windows_msvc_asan_runtime_must_match_cached_linker_directory(self):
        receipt = self.receipt()
        row = next(row for row in receipt["rows"] if row["id"] == "windows_msvc_asan")
        row["input_identity"]["toolchain_resources"]["msvc_asan_runtime"]["files"][0]["path"] = (
            r"C:\captured\elsewhere\clang_rt.asan_dynamic-x86_64.dll"
        )
        self.rehash_input_identity(row)
        with self.assertRaisesRegex(self.matrix.MatrixError, "does not match its selected CMake/environment path"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

        receipt = self.receipt()
        row = next(row for row in receipt["rows"] if row["id"] == "windows_msvc_asan")
        row["input_identity"]["toolchain_resources"]["msvc_asan_runtime"]["files"] = []
        self.rehash_input_identity(row)
        with self.assertRaisesRegex(self.matrix.MatrixError, "exactly its selected dynamic runtime DLL"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_dependency_path_must_match_captured_environment_after_receipt_rehash(self):
        receipt = self.receipt()
        row = receipt["rows"][0]
        row["input_identity"]["external_dependencies"]["VCPKG_INSTALLED_DIR"]["path"] = (
            "/captured/unobserved/vcpkg_installed"
        )
        self.rehash_input_identity(row)

        with mock.patch.object(self.matrix, "row_source_input_sha256", return_value="6" * 64):
            with self.assertRaisesRegex(self.matrix.MatrixError, "path does not match its captured environment"):
                self.matrix.validate_receipt(
                    receipt, self.manifest, CANDIDATE, root=REPOSITORY
                )

    def test_platform_system_must_match_captured_platform_name_after_receipt_rehash(self):
        receipt = self.receipt()
        row = receipt["rows"][0]
        row["input_identity"]["platform"]["system"] = "Linux-6.0-test-host"
        self.rehash_input_identity(row)

        with mock.patch.object(self.matrix, "row_source_input_sha256", return_value="6" * 64):
            with self.assertRaisesRegex(self.matrix.MatrixError, "platform.system does not match its name"):
                self.matrix.validate_receipt(
                    receipt, self.manifest, CANDIDATE, root=REPOSITORY
                )

    def test_missing_external_dependency_inventory_fails_closed(self):
        row = next(row for row in self.manifest["rows"] if row["id"] == "macos_asan")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            env = {
                "VCPKG_ROOT": str(root / "vcpkg"),
                "VCPKG_INSTALLED_DIR": str(root / "installed"),
                "MC_PRIVATE_DEPENDENCIES_PREFIX": str(root / "private"),
            }
            for name, value in env.items():
                Path(value).mkdir(parents=True, exist_ok=True)
            private_header = root / "private/include/private.hpp"
            private_header.parent.mkdir(parents=True)
            private_header.write_text("struct PrivateDependency {}\n", encoding="utf-8")
            with mock.patch.object(self.matrix, "_git_dependency_identity", return_value={"path": "x", "revision": "1" * 40}):
                with self.assertRaisesRegex(self.matrix.MatrixError, "vcpkg installed-file inventory is missing"):
                    self.matrix._external_dependency_identity(root, row, env)

    def test_external_dependency_identity_does_not_require_analysis_flags(self):
        row = next(row for row in self.manifest["rows"] if row["id"] == "macos_asan")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            vcpkg_root = root / "vcpkg"
            installed = root / "installed"
            private_prefix = root / "private"
            installed_header = installed / "arm64-osx/include/package.hpp"
            inventory = installed / "vcpkg/info/package_1.0_arm64-osx.list"
            private_header = private_prefix / "include/private.hpp"
            installed_header.parent.mkdir(parents=True)
            inventory.parent.mkdir(parents=True)
            private_header.parent.mkdir(parents=True)
            installed_header.write_text("struct InstalledPackage {};\n", encoding="utf-8")
            inventory.write_text("arm64-osx/include/package.hpp\n", encoding="utf-8")
            private_header.write_text("struct PrivateDependency {};\n", encoding="utf-8")
            environment = {
                "VCPKG_ROOT": str(vcpkg_root),
                "VCPKG_INSTALLED_DIR": str(installed),
                "MC_PRIVATE_DEPENDENCIES_PREFIX": str(private_prefix),
            }

            with mock.patch.object(
                self.matrix,
                "_git_dependency_identity",
                return_value={"path": str(vcpkg_root), "revision": "1" * 40},
            ):
                identity = self.matrix._external_dependency_identity(root, row, environment)

        self.assertEqual(
            set(identity),
            {"VCPKG_ROOT", "VCPKG_INSTALLED_DIR", "MC_PRIVATE_DEPENDENCIES_PREFIX"},
        )
        self.assertNotIn("CXXFLAGS", identity)

    def test_macos_analysis_identity_and_execution_require_cxxflags(self):
        row = next(row for row in self.manifest["rows"] if row["id"] == "macos_asan")
        environment = {
            "VCPKG_ROOT": "/captured/vcpkg",
            "VCPKG_INSTALLED_DIR": "/captured/installed",
            "MC_PRIVATE_DEPENDENCIES_PREFIX": "/captured/private",
        }
        with self.assertRaisesRegex(self.matrix.MatrixError, "requires environment variable CXXFLAGS"):
            self.matrix._row_input_identity(REPOSITORY, row, environment, [])

        with tempfile.TemporaryDirectory() as directory, \
            mock.patch.object(self.matrix, "require_clean_checkout"), \
            mock.patch.object(self.matrix, "_row_supported_on_host", return_value=True), \
            mock.patch.object(self.matrix, "_capture_tool_probes") as capture_probes, \
            mock.patch.dict(self.matrix.os.environ, environment, clear=True):
            receipt = self.matrix.run_row(
                row["id"],
                Path(directory) / "row.json",
                MANIFEST_PATH,
                root=REPOSITORY,
                expected_candidate=CANDIDATE,
            )

        self.assertEqual(receipt["rows"][0]["status"], "unavailable")
        self.assertIn("CXXFLAGS", receipt["rows"][0]["diagnostics"])
        capture_probes.assert_not_called()

    def test_vcpkg_triplet_inventory_hashes_dependency_files_and_skips_directories(self):
        with tempfile.TemporaryDirectory() as directory:
            installed = Path(directory)
            entries = (
                "arm64-osx/",
                "arm64-osx/debug/",
                "arm64-osx/debug/lib/",
                "arm64-osx/debug/lib/libfmtd.a",
                "arm64-osx/include/",
                "arm64-osx/include/fmt/args.h",
                "arm64-osx/lib/libfmt.a",
                "arm64-osx/share/fmt/cmake/fmt-config.cmake",
            )
            inventory = installed / "vcpkg/info/fmt_12.1.0_arm64-osx.list"
            inventory.parent.mkdir(parents=True)
            inventory.write_text("\n".join(entries) + "\n", encoding="utf-8")
            for relative in entries:
                if relative.endswith("/"):
                    (installed / relative).mkdir(parents=True, exist_ok=True)
                else:
                    dependency = installed / relative
                    dependency.parent.mkdir(parents=True, exist_ok=True)
                    dependency.write_bytes(relative.encode("utf-8"))

            files = self.matrix._installed_dependency_files(installed, [inventory])

        self.assertEqual(
            set(files),
            {
                "arm64-osx/debug/lib/libfmtd.a",
                "arm64-osx/include/fmt/args.h",
                "arm64-osx/lib/libfmt.a",
                "arm64-osx/share/fmt/cmake/fmt-config.cmake",
            },
        )

    def test_macos_ubsan_runtime_identity_uses_official_llvm_filename(self):
        row = next(row for row in self.manifest["rows"] if row["id"] == "macos_ubsan")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            resource_dir = root / "llvm" / "lib" / "clang" / "22"
            runtime = resource_dir / "lib/darwin/libclang_rt.ubsan_osx_dynamic.dylib"
            runtime.parent.mkdir(parents=True)
            runtime.write_bytes(b"official LLVM22 UBSan runtime fixture")
            identity = self.matrix._sanitizer_runtime_identity(
                root,
                row,
                {"MC_ANALYSIS_ROOT": str(root), "CFLAGS": f"-resource-dir {resource_dir}"},
                [{"probe_argv": ["clang", "--version"], "executable": "/usr/bin/clang"}],
                {"status": "present", "values": {}},
                [],
            )

        self.assertEqual(identity["resource_directory"], str(resource_dir.resolve()))
        self.assertEqual(
            [runtime_info["name"] for runtime_info in identity["files"]],
            ["libclang_rt.ubsan_osx_dynamic.dylib"],
        )

    def test_receipt_requires_tool_commands_flags_and_diagnostics(self):
        for field in ("tool_versions", "commands", "flags", "diagnostics"):
            receipt = self.receipt()
            if field == "diagnostics":
                receipt["rows"][0].pop(field)
            else:
                receipt["rows"][0][field] = []
            with self.subTest(field=field), self.assertRaises(self.matrix.MatrixError):
                self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_receipt_requires_declared_row_environment(self):
        receipt = self.receipt()
        receipt["rows"][0]["environment"] = {"ASAN_OPTIONS": "detect_leaks=0"}
        with self.assertRaisesRegex(self.matrix.MatrixError, "environment"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_receipt_requires_observed_effective_compile_flags(self):
        receipt = self.receipt()
        row = next(row for row in receipt["rows"] if row["id"] == "macos_asan")
        row["effective_compile_flags"] = None
        with self.assertRaisesRegex(self.matrix.MatrixError, "effective compiler flags"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_receipt_requires_each_command_to_have_successful_execution_evidence(self):
        receipt = self.receipt()
        row = receipt["rows"][0]
        row["executed_commands"] = []
        with self.assertRaisesRegex(self.matrix.MatrixError, "every launched command"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

        receipt = self.receipt()
        receipt["rows"][0]["executed_commands"][0]["return_code"] = 1
        with self.assertRaisesRegex(self.matrix.MatrixError, "did not exit successfully"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_static_analysis_receipts_require_effective_tool_invocation(self):
        for row_id in self.matrix.REQUIRED_ANALYSIS_CONFIGURATION:
            receipt = self.receipt()
            row = next(item for item in receipt["rows"] if item["id"] == row_id)
            row["effective_analysis"]["invocation_count"] = 0
            with self.subTest(row_id=row_id), self.assertRaisesRegex(
                self.matrix.MatrixError, "no observed analyzer invocation"
            ):
                self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_effective_analysis_capture_requires_cache_flags_and_verbose_invocation(self):
        for row_id, (preset, cache_key, settings) in self.matrix.REQUIRED_ANALYSIS_CONFIGURATION.items():
            if row_id in self.matrix.PER_TRANSLATION_UNIT_ANALYSIS:
                continue
            with self.subTest(row_id=row_id), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                cache_path = root / "build" / preset / "CMakeCache.txt"
                cache_path.parent.mkdir(parents=True)
                cache_path.write_text(f"{cache_key}:STRING={' '.join(settings)}\n", encoding="utf-8")
                output = " ".join(self.matrix.ANALYSIS_TOOL_MARKERS[row_id])
                captured = self.matrix.capture_effective_analysis(root, {"id": row_id}, output)
                self.assertEqual(captured["invocation_count"], 1)
                self.assertEqual(captured["observed_settings"], settings)

                with self.assertRaisesRegex(self.matrix.MatrixError, "no configured analyzer invocation"):
                    self.matrix.capture_effective_analysis(root, {"id": row_id}, "build completed")

    def test_cmake_dependency_flags_must_match_the_expected_object_and_depfile(self):
        compile_argv = ["clang++", "-std=c++23", "-o", "build/unit.o", "-c", "src/unit.cpp"]
        observed = [
            *compile_argv[:2],
            "-MD",
            "-MT",
            "build/unit.o",
            "-MF",
            "build/unit.o.d",
            *compile_argv[2:],
        ]

        self.assertTrue(self.matrix._compiler_argv_matches(compile_argv, observed))
        self.assertTrue(
            self.matrix._iwyu_argv_matches(
                compile_argv[1:], ["--driver-mode=g++", *observed[1:]]
            )
        )
        self.assertTrue(
            self.matrix._iwyu_argv_matches(
                compile_argv[1:], [*observed[1:], "--driver-mode=g++"]
            )
        )

        wrong_object = list(observed)
        wrong_object[wrong_object.index("-MT") + 1] = "build/other.o"
        wrong_depfile = list(observed)
        wrong_depfile[wrong_depfile.index("-MF") + 1] = "build/other.o.d"
        extra_argument = list(observed)
        extra_argument.insert(extra_argument.index("-o"), "-fextra")
        for invalid in (wrong_object, wrong_depfile, extra_argument):
            with self.subTest(invalid=invalid):
                self.assertFalse(self.matrix._compiler_argv_matches(compile_argv, invalid))

    def test_iwyu_capture_requires_exact_owned_translation_unit_receipts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_iwyu_fixture(root)

            captured = self.matrix.capture_effective_analysis(root, {"id": "iwyu"}, "incremental build")

        self.assertEqual(captured["translation_unit_count"], 1)
        self.assertEqual(captured["invocation_count"], 1)
        self.assertEqual(captured["translation_units"][0]["invoked_tool"], "include-what-you-use")
        self.assertEqual(captured["translation_units"][0]["status"], "passed")
        self.assertEqual(len(captured["translation_unit_inventory_sha256"]), 64)

    def test_iwyu_capture_rejects_changed_owned_header_and_missing_translation_unit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = self.create_iwyu_fixture(root)
            header = sources[0].with_suffix(".hpp")
            header.write_text("struct ChangedHeader {};\n", encoding="utf-8")

            with self.assertRaisesRegex(self.matrix.MatrixError, "stale project inputs"):
                self.matrix.capture_effective_analysis(root, {"id": "iwyu"}, "incremental build")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_iwyu_fixture(root, source_count=2, receipt_count=1)

            with self.assertRaisesRegex(self.matrix.MatrixError, "missing 1 owned translation-unit"):
                self.matrix.capture_effective_analysis(root, {"id": "iwyu"}, "partial build")

    def test_iwyu_capture_retains_old_failure_beside_current_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_iwyu_fixture(root)
            gate_directory = root / self.matrix.PER_TRANSLATION_UNIT_ANALYSIS["iwyu"]["receipt_directory"]
            current_path = next(gate_directory.glob("*.json"))
            historical_path, historical_log = self.add_iwyu_history_record(
                root, current_path, status="error"
            )

            captured = self.matrix.capture_effective_analysis(root, {"id": "iwyu"}, "retry after argv change")

            self.assertEqual(captured["translation_unit_count"], 1)
            self.assertEqual(captured["translation_units"][0]["status"], "passed")
            self.assertTrue(historical_path.is_file())
            self.assertEqual(historical_log.read_bytes(), b"error historical IWYU output\n")

    def test_iwyu_historical_pass_cannot_cover_a_missing_current_invocation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_iwyu_fixture(root)
            gate_directory = root / self.matrix.PER_TRANSLATION_UNIT_ANALYSIS["iwyu"]["receipt_directory"]
            current_path = next(gate_directory.glob("*.json"))
            current = json.loads(current_path.read_text(encoding="utf-8"))
            historical_path, _ = self.add_iwyu_history_record(root, current_path, status="passed")
            current_path.unlink()
            (root / current["log"]).unlink()

            with self.assertRaisesRegex(self.matrix.MatrixError, "missing 1 owned translation-unit"):
                self.matrix.capture_effective_analysis(root, {"id": "iwyu"}, "missing current invocation")
            self.assertTrue(historical_path.is_file())

    def test_iwyu_current_failure_remains_a_hard_error(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_iwyu_fixture(root)
            gate_directory = root / self.matrix.PER_TRANSLATION_UNIT_ANALYSIS["iwyu"]["receipt_directory"]
            current_path = next(gate_directory.glob("*.json"))
            current = json.loads(current_path.read_text(encoding="utf-8"))
            current["status"] = "error"
            current["diagnostic"] = "current invocation failed"
            current_path.write_text(json.dumps(current), encoding="utf-8")

            with self.assertRaisesRegex(self.matrix.MatrixError, "is not passed"):
                self.matrix.capture_effective_analysis(root, {"id": "iwyu"}, "current invocation failure")

    def test_iwyu_malformed_historical_identity_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_iwyu_fixture(root)
            gate_directory = root / self.matrix.PER_TRANSLATION_UNIT_ANALYSIS["iwyu"]["receipt_directory"]
            current_path = next(gate_directory.glob("*.json"))
            historical_path, _ = self.add_iwyu_history_record(
                root, current_path, status="error", malformed_identity=True
            )

            with self.assertRaisesRegex(self.matrix.MatrixError, "invalid source identity"):
                self.matrix.capture_effective_analysis(root, {"id": "iwyu"}, "malformed historical record")
            self.assertTrue(historical_path.is_file())

    def test_iwyu_historical_record_for_unknown_source_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_iwyu_fixture(root)
            gate_directory = root / self.matrix.PER_TRANSLATION_UNIT_ANALYSIS["iwyu"]["receipt_directory"]
            current_path = next(gate_directory.glob("*.json"))
            self.add_iwyu_history_record(
                root, current_path, status="error", source="src/not-in-compile-commands.cpp"
            )

            with self.assertRaisesRegex(self.matrix.MatrixError, "outside the current compile inventory"):
                self.matrix.capture_effective_analysis(root, {"id": "iwyu"}, "unknown historical source")

    def test_iwyu_build_failure_propagates_to_the_analysis_row(self):
        for row_id in ("iwyu", "cppcheck"):
            with tempfile.TemporaryDirectory() as directory:
                receipt_path = Path(directory) / "row.json"
                row = next(row for row in self.manifest["rows"] if row["id"] == row_id)
                command_results = [
                    self.matrix.subprocess.CompletedProcess([], 0, "configured\n", ""),
                    self.matrix.subprocess.CompletedProcess([], 1, "gate failed\n", ""),
                ]

                with mock.patch.object(self.matrix, "require_clean_checkout"), self.patch_row_execution(
                    row, command_results
                ):
                    receipt = self.matrix.run_row(
                        row_id, receipt_path, MANIFEST_PATH, root=REPOSITORY, expected_candidate=CANDIDATE
                    )

            row_receipt = receipt["rows"][0]
            self.assertEqual(row_receipt["status"], "failed")
            self.assertEqual(row_receipt["executed_commands"][-1]["return_code"], 1)
            self.assertIsNone(row_receipt["effective_analysis"])

    @unittest.skipIf(sys.platform == "win32", "CMake IWYU adapter regression uses a POSIX no-op compiler")
    def test_cmake_iwyu_error_sentinel_propagates_wrapper_exit_status(self):
        cmake = shutil.which("cmake")
        no_op_compiler = shutil.which("true")
        if cmake is None or no_op_compiler is None:
            self.skipTest("cmake and POSIX true are required for the CMake adapter regression")

        failing_iwyu = f"{sys.executable};-c;raise SystemExit(7)"
        base_command = [
            cmake,
            "-E",
            "__run_co_compile",
            "--source=Chunk.cpp",
            "--",
            no_op_compiler,
            "-c",
            "Chunk.cpp",
        ]
        swallowed = subprocess.run(
            [*base_command[:3], f"--iwyu={failing_iwyu}", *base_command[3:]],
            check=False,
            capture_output=True,
        )
        propagated = subprocess.run(
            [*base_command[:3], f"--iwyu={failing_iwyu};-Xiwyu;--error", *base_command[3:]],
            check=False,
            capture_output=True,
        )

        self.assertEqual(swallowed.returncode, 0, swallowed.stderr.decode(errors="replace"))
        self.assertEqual(propagated.returncode, 7, propagated.stderr.decode(errors="replace"))

    def test_cppcheck_capture_requires_exact_receipts_and_current_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = self.create_cppcheck_fixture(root)

            captured = self.matrix.capture_effective_analysis(root, {"id": "cppcheck"}, "incremental build")
            self.assertEqual(captured["translation_unit_count"], 1)
            self.assertEqual(captured["translation_units"][0]["invoked_tool"], "cppcheck")

            sources[0].with_suffix(".hpp").write_text("struct ChangedHeader {};\n", encoding="utf-8")
            with self.assertRaisesRegex(self.matrix.MatrixError, "stale project inputs"):
                self.matrix.capture_effective_analysis(root, {"id": "cppcheck"}, "incremental build")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_cppcheck_fixture(root, source_count=2, receipt_count=1)
            with self.assertRaisesRegex(self.matrix.MatrixError, "missing 1 owned translation-unit"):
                self.matrix.capture_effective_analysis(root, {"id": "cppcheck"}, "partial build")

    def test_cppcheck_effective_analysis_capture_round_trips_through_receipt_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_cppcheck_fixture(root)
            captured = self.matrix.capture_effective_analysis(root, {"id": "cppcheck"}, "incremental build")
            receipt = self.receipt()
            row = next(item for item in receipt["rows"] if item["id"] == "cppcheck")
            row["effective_analysis"] = captured

            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

        self.assertEqual(
            captured["configuration_key"],
            self.matrix.REQUIRED_ANALYSIS_CONFIGURATION["cppcheck"][1],
        )

    def test_cppcheck_capture_rejects_changed_gate_policy_and_compile_commands(self):
        for changed_input in ("CMakePresets.json", "script/ci/cppcheck_gate.py"):
            with self.subTest(changed_input=changed_input), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self.create_cppcheck_fixture(root)
                policy_path = root / changed_input
                if changed_input == "CMakePresets.json":
                    presets = json.loads(policy_path.read_text(encoding="utf-8"))
                    presets["configurePresets"][0]["description"] = "changed policy"
                    policy_path.write_text(json.dumps(presets), encoding="utf-8")
                else:
                    policy_path.write_text(
                        policy_path.read_text(encoding="utf-8") + "changed\n", encoding="utf-8"
                    )

                with self.assertRaisesRegex(self.matrix.MatrixError, "stale project inputs"):
                    self.matrix.capture_effective_analysis(root, {"id": "cppcheck"}, "incremental build")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_cppcheck_fixture(root)
            compile_commands_path = root / "build/analysis-cppcheck/compile_commands.json"
            compile_commands = json.loads(compile_commands_path.read_text(encoding="utf-8"))
            compile_commands[0]["arguments"].insert(1, "-DCHANGED_COMMAND")
            compile_commands_path.write_text(json.dumps(compile_commands), encoding="utf-8")

            with self.assertRaisesRegex(self.matrix.MatrixError, "does not match compile_commands.json"):
                self.matrix.capture_effective_analysis(root, {"id": "cppcheck"}, "incremental build")

    def test_effective_compile_flags_are_read_from_generated_commands(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            compile_commands = root / "compile_commands.json"
            compile_commands.write_text(
                json.dumps(
                    [
                        {
                            "directory": str(root),
                            "file": str(root / "main.cpp"),
                            "arguments": ["clang++", "-fsanitize=address", "-c", "main.cpp"],
                        },
                        {
                            "directory": str(root),
                            "file": str(root / "tests.cpp"),
                            "command": 'clang++ -fsanitize=address -c "tests.cpp"',
                        },
                    ]
                ),
                encoding="utf-8",
            )
            row = {
                "id": "macos_asan",
                "compile_commands": "compile_commands.json",
                "required_compile_flags": ["-fsanitize=address"],
            }

            result = self.matrix.capture_effective_compile_flags(root, row)

        self.assertEqual(result["command_count"], 2)
        self.assertEqual(result["observed_flags"], ["-fsanitize=address"])
        self.assertEqual(len(result["compile_commands_sha256"]), 64)

    def test_effective_compile_flag_capture_rejects_uninstrumented_commands(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": str(root),
                            "file": str(root / "main.cpp"),
                            "arguments": ["clang++", "-c", "main.cpp"],
                        }
                    ]
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(self.matrix.MatrixError, "-fsanitize=address"):
                self.matrix.capture_effective_compile_flags(
                    root,
                    {
                        "id": "macos_asan",
                        "compile_commands": "compile_commands.json",
                        "required_compile_flags": ["-fsanitize=address"],
                    },
                )

    def test_row_runner_records_an_executed_passing_row(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt_path = Path(directory) / "row.json"
            row = self.manifest["rows"][0]

            successful_output = "x" * 16_385 + "successful output tail"

            def successful_command(argv, **kwargs):
                output = successful_output if argv[:2] == ["cmake", "--preset"] else "command output"
                return self.matrix.subprocess.CompletedProcess(argv, 0, output, "")

            with mock.patch.object(self.matrix, "require_clean_checkout"), self.patch_row_execution(
                row, successful_command
            ), mock.patch.object(
                self.matrix,
                "capture_effective_compile_flags",
                return_value={
                    "compile_commands_sha256": "c" * 64,
                    "command_count": 1,
                    "observed_flags": list(row["required_compile_flags"]),
                },
            ):
                receipt = self.matrix.run_row(
                    row["id"], receipt_path, MANIFEST_PATH, root=REPOSITORY, expected_candidate=CANDIDATE
                )
            result = receipt["rows"][0]
            configure_index = next(
                index
                for index, command in enumerate(row["commands"])
                if command[:2] == ["cmake", "--preset"]
            )
            self.assertEqual(result["status"], "passed")
            self.assertEqual(result["commands"], row["commands"])
            self.assertEqual(result["environment"], row.get("environment", {}))
            self.assertIn("command output", result["diagnostics"])
            self.assertIn("[output truncated by analysis-matrix]", result["diagnostics"])
            self.assertNotIn("successful output tail", result["diagnostics"])
            self.assertEqual(len(result["executed_commands"]), len(row["commands"]))
            self.assertEqual(result["executed_commands"][configure_index]["return_code"], 0)
            self.assertEqual(
                result["executed_commands"][configure_index]["output_bytes"],
                len(successful_output.encode("utf-8")),
            )
            self.assertEqual(
                result["executed_commands"][configure_index]["output_sha256"],
                hashlib.sha256(successful_output.encode("utf-8")).hexdigest(),
            )

    def test_row_runner_retains_complete_failed_command_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt_path = Path(directory) / "row.json"
            row = self.manifest["rows"][0]
            stdout = "x" * 17_000 + "\nclang: error: trailing sanitizer build failure\n"
            stderr = "ninja: build stopped: subcommand failed\n"
            command_output = stdout + stderr
            def failed_command(argv, **kwargs):
                return self.matrix.subprocess.CompletedProcess(argv, 1, stdout, stderr)

            with mock.patch.object(self.matrix, "require_clean_checkout"), self.patch_row_execution(
                row, failed_command
            ):
                self.matrix.run_row(
                    row["id"], receipt_path, MANIFEST_PATH, root=REPOSITORY, expected_candidate=CANDIDATE
                )

            result = json.loads(receipt_path.read_text(encoding="utf-8"))["rows"][0]
            self.assertEqual(result["status"], "failed")
            self.assertIn(command_output, result["diagnostics"])
            self.assertIn("clang: error: trailing sanitizer build failure", result["diagnostics"])
            self.assertEqual(len(result["executed_commands"]), 1)
            execution = result["executed_commands"][0]
            self.assertEqual(execution["return_code"], 1)
            self.assertEqual(execution["output_bytes"], len(command_output.encode("utf-8")))
            self.assertEqual(
                execution["output_sha256"],
                hashlib.sha256(command_output.encode("utf-8")).hexdigest(),
            )

    def test_row_runner_retains_partial_timeout_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt_path = Path(directory) / "row.json"
            row = self.manifest["rows"][0]
            partial_stdout = b"x" * 17_000 + b"\nclang: error: sanitizer build timed out\n\xff"
            partial_stderr = "ninja: still waiting for a stopped job\n"
            def timeout_command(argv, **kwargs):
                raise self.matrix.subprocess.TimeoutExpired(
                    argv, 30, output=partial_stdout, stderr=partial_stderr
                )

            with mock.patch.object(self.matrix, "require_clean_checkout"), self.patch_row_execution(
                row, timeout_command
            ):
                self.matrix.run_row(
                    row["id"], receipt_path, MANIFEST_PATH, root=REPOSITORY, expected_candidate=CANDIDATE
                )

            result = json.loads(receipt_path.read_text(encoding="utf-8"))["rows"][0]
            self.assertEqual(result["status"], "failed")
            self.assertIn(
                partial_stdout.decode("utf-8", errors="replace") + partial_stderr,
                result["diagnostics"],
            )
            self.assertIn("clang: error: sanitizer build timed out", result["diagnostics"])
            self.assertIn("\ufffd", result["diagnostics"])
            self.assertEqual(result["executed_commands"], [])

    def test_candidate_commit_authentication_is_independent_of_working_inputs(self):
        with mock.patch.object(self.matrix, "candidate_identity", return_value={"head": "c" * 40, "tree": TREE}):
            with self.assertRaisesRegex(self.matrix.MatrixError, "does not match the checkout"):
                self.matrix.require_clean_checkout(REPOSITORY, CANDIDATE)

    def test_row_runner_records_unsupported_host_as_unavailable(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt_path = Path(directory) / "row.json"
            host = self.matrix._host_platform()
            row_id = "windows_msvc_asan" if host != "windows" else "linux_lsan"
            with mock.patch.object(self.matrix, "require_clean_checkout"):
                receipt = self.matrix.run_row(
                    row_id, receipt_path, MANIFEST_PATH, root=REPOSITORY, expected_candidate=CANDIDATE
                )
            self.assertEqual(receipt["rows"][0]["status"], "unavailable")
            self.assertIn("requires", receipt["rows"][0]["diagnostics"])

    def test_merger_requires_every_passing_candidate_bound_row(self):
        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            complete = self.receipt()
            output_path = directory_path / "complete.json"
            platform_receipts = []
            for host, row_ids in self.matrix.HOST_ROW_IDS.items():
                host_receipt = {
                    **complete,
                    "runner": {
                        "name": "analysis-matrix-local",
                        "host_platform": host,
                        "rows": len(row_ids),
                        "selected_rows": list(row_ids),
                    },
                    "rows": [row for row in complete["rows"] if row["id"] in row_ids],
                }
                host_path = directory_path / f"{host}.json"
                host_path.write_text(json.dumps(host_receipt), encoding="utf-8")
                platform_receipts.append(host_path)
            with mock.patch.object(self.matrix, "row_source_input_sha256", return_value="6" * 64):
                merged = self.matrix.merge_row_receipts(
                    platform_receipts, output_path, MANIFEST_PATH, root=REPOSITORY, expected_candidate=CANDIDATE
                )
            self.assertEqual(len(merged["rows"]), len(self.matrix.REQUIRED_ROW_IDS))
            self.assertIsNotNone(self.matrix.validate_receipt(merged, self.manifest, CANDIDATE))

    def test_candidate_identity_keeps_commit_tree_separate_from_staged_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "nested").mkdir()
            (root / "file.txt").write_text("initial\n", encoding="utf-8")
            (root / "nested/file.txt").write_text("nested\n", encoding="utf-8")
            subprocess.run(["git", "init"], cwd=root, check=True, capture_output=True)
            subprocess.run(["git", "add", "."], cwd=root, check=True, capture_output=True)
            subprocess.run(
                ["git", "-c", "user.name=Matrix", "-c", "user.email=matrix@example.test", "commit", "-m", "initial"],
                cwd=root,
                check=True,
                capture_output=True,
            )
            (root / "nested/file.txt").write_text("changed\n", encoding="utf-8")
            subprocess.run(["git", "add", "nested/file.txt"], cwd=root, check=True, capture_output=True)
            expected = subprocess.run(
                ["git", "rev-parse", "HEAD^{tree}"], cwd=root, check=True, capture_output=True, text=True
            ).stdout.strip()
            identity = self.matrix.candidate_identity(root)
            self.assertEqual(identity["tree"], expected)
            self.assertNotEqual(identity["tree"], subprocess.run(
                ["git", "write-tree"], cwd=root, check=True, capture_output=True, text=True
            ).stdout.strip())

    def test_duplicate_and_extra_rows_fail_closed(self):
        receipt = self.receipt()
        receipt["rows"].append(copy.deepcopy(receipt["rows"][0]))
        with self.assertRaisesRegex(self.matrix.MatrixError, "duplicate"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

        receipt = self.receipt()
        receipt["rows"][0]["id"] = "not-a-matrix-row"
        with self.assertRaisesRegex(self.matrix.MatrixError, "unexpected"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_run_local_selects_only_unique_host_owned_rows_and_records_partial_scope(self):
        host = self.matrix._host_platform()
        row_id = self.matrix.HOST_ROW_IDS[host][0]
        fake_row = {"id": row_id, "status": "passed"}
        with tempfile.TemporaryDirectory() as directory:
            receipt_path = Path(directory) / "analysis.json"
            with mock.patch.object(self.matrix, "_host_platform", return_value=host), mock.patch.object(
                self.matrix, "run_row", return_value={"rows": [fake_row]}
            ) as run_row:
                receipt = self.matrix.run_local(
                    receipt_path,
                    MANIFEST_PATH,
                    root=REPOSITORY,
                    expected_candidate=CANDIDATE,
                    row_ids=[row_id],
                )
            self.assertEqual(receipt["runner"]["selected_rows"], [row_id])
            self.assertEqual([row["id"] for row in receipt["rows"]], [row_id])
            run_row.assert_called_once()
            with self.assertRaisesRegex(self.matrix.MatrixError, "unique rows owned"):
                self.matrix.run_local(
                    receipt_path,
                    MANIFEST_PATH,
                    root=REPOSITORY,
                    expected_candidate=CANDIDATE,
                    row_ids=[row_id, row_id],
                )

    def test_run_local_marks_rows_unrun_at_the_aggregate_deadline(self):
        host = self.matrix._host_platform()
        selected = self.matrix.HOST_ROW_IDS[host][:2]
        clock = [0.0]

        def monotonic():
            return clock[0]

        def run_row(row_id, *args, **kwargs):
            self.assertEqual(
                kwargs["overall_timeout_seconds"],
                min(self.matrix.MAX_ROW_TIMEOUT_SECONDS, self.matrix.MAX_LOCAL_TIMEOUT_SECONDS),
            )
            clock[0] = self.matrix.MAX_LOCAL_TIMEOUT_SECONDS
            return {"rows": [{"id": row_id, "status": "passed"}]}

        with tempfile.TemporaryDirectory() as directory:
            receipt_path = Path(directory) / "analysis.json"
            with mock.patch.object(self.matrix, "_host_platform", return_value=host), mock.patch.object(
                self.matrix.time, "monotonic", side_effect=monotonic
            ), mock.patch.object(self.matrix, "run_row", side_effect=run_row) as run:
                receipt = self.matrix.run_local(
                    receipt_path,
                    MANIFEST_PATH,
                    root=REPOSITORY,
                    expected_candidate=CANDIDATE,
                    row_ids=selected,
                )

        self.assertEqual(run.call_count, 1)
        self.assertEqual([row["id"] for row in receipt["rows"]], list(selected))
        self.assertEqual(receipt["rows"][1]["status"], "failed")
        self.assertIn("UNRUN", receipt["rows"][1]["diagnostics"])
        self.assertEqual(receipt["rows"][1]["duration_seconds"], 0)

    def test_cli_verifies_a_complete_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt_path = Path(directory) / "analysis.json"
            receipt_path.write_text(json.dumps(self.receipt()), encoding="utf-8")
            with mock.patch.object(self.matrix, "row_source_input_sha256", return_value="6" * 64):
                self.assertEqual(
                    self.matrix.main(
                        [
                            "verify",
                            str(receipt_path),
                            "--manifest",
                            str(MANIFEST_PATH),
                            "--head",
                            HEAD,
                            "--tree",
                            TREE,
                        ]
                    ),
                    0,
                )

    def test_cli_fails_after_writing_a_nonpassing_row_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt_path = Path(directory) / "row.json"
            result = {
                "rows": [{"status": "unavailable"}],
            }
            with mock.patch.object(self.matrix, "run_row", return_value=result):
                self.assertEqual(
                    self.matrix.main(
                        ["run-row", str(receipt_path), "--row", "macos_asan"]
                    ),
                    1,
                )

    def test_local_row_commands_expand_only_declared_environment_placeholders(self):
        self.assertEqual(
            self.matrix._expand_environment(
                ["cmake", "-DROOT={env:VCPKG_ROOT}", "literal"],
                {"VCPKG_ROOT": "/tmp/vcpkg"},
            ),
            ["cmake", "-DROOT=/tmp/vcpkg", "literal"],
        )
        with self.assertRaisesRegex(self.matrix.MatrixError, "VCPKG_ROOT"):
            self.matrix._expand_environment(["{env:VCPKG_ROOT}"], {})

    def test_host_support_is_limited_to_native_desktop_analysis_rows(self):
        with mock.patch.object(self.matrix, "_host_platform", return_value="linux"):
            self.assertTrue(self.matrix._row_supported_on_host({"platform": "linux"}))
            self.assertFalse(self.matrix._row_supported_on_host({"platform": "android-arm64"}))
            self.assertFalse(self.matrix._row_supported_on_host({"platform": "hosted"}))

    def test_manifest_requires_standalone_lsan_and_full_msan_rows(self):
        rows = {row["id"]: row for row in self.manifest["rows"]}
        self.assertEqual(rows["linux_lsan"]["platform"], "linux")
        self.assertIn("-fsanitize=leak", rows["linux_lsan"]["flags"])
        self.assertEqual(rows["linux_msan"]["platform"], "linux")
        self.assertIn("-fsanitize=memory", rows["linux_msan"]["flags"])
        self.assertIn("LLVM_USE_SANITIZER=MemoryWithOrigins", rows["linux_msan"]["flags"])
        self.assertEqual(
            rows["linux_msan"]["environment"]["MSAN_OPTIONS"],
            "halt_on_error=1:print_stats=1:fast_unwind_on_fatal=1",
        )
        self.assertEqual(rows["linux_msan"]["environment"]["DEBUGINFOD_URLS"], "")
        self.assertIn(["ldd", "--version"], rows["linux_msan"]["tool_probes"])
        self.assertIn("linux_lsan", self.matrix.REQUIRED_ROW_IDS)
        self.assertIn("linux_msan", self.matrix.REQUIRED_ROW_IDS)
        self.assertIn("MC_LSAN_LIBCXX_PREFIX", self.matrix.REQUIRED_ROW_ENVIRONMENT["linux_lsan"])

    def test_msan_environment_disables_inherited_remote_debuginfod(self):
        row = next(row for row in self.manifest["rows"] if row["id"] == "linux_msan")
        resolved = self.matrix.resolve_row_environment(
            row,
            REPOSITORY,
            {"DEBUGINFOD_URLS": "https://debuginfod.example.invalid"},
        )
        self.assertEqual(resolved["DEBUGINFOD_URLS"], "")

        manifest = copy.deepcopy(self.manifest)
        manifest_row = next(row for row in manifest["rows"] if row["id"] == "linux_msan")
        manifest_row["environment"]["DEBUGINFOD_URLS"] = "https://debuginfod.example.invalid"
        with self.assertRaisesRegex(self.matrix.MatrixError, "remote debuginfod"):
            self.matrix.validate_manifest(manifest)

    def test_clean_pinned_msan_llvm_checkout_records_recipe_identity_and_rejects_dirty_checkout(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            llvm = root / "llvm-project"
            llvm.mkdir()
            subprocess.run(["git", "init", "-q", str(llvm)], check=True, capture_output=True, text=True)
            subprocess.run(["git", "-C", str(llvm), "config", "user.name", "Matrix Test"], check=True)
            subprocess.run(
                ["git", "-C", str(llvm), "config", "user.email", "matrix@example.invalid"],
                check=True,
            )
            (llvm / "README").write_text("pinned upstream tree\n", encoding="utf-8")
            subprocess.run(["git", "-C", str(llvm), "add", "README"], check=True)
            subprocess.run(["git", "-C", str(llvm), "commit", "-q", "-m", "fixture"], check=True)
            revision = subprocess.run(
                ["git", "-C", str(llvm), "rev-parse", "HEAD"],
                check=True,
                capture_output=True,
                text=True,
            ).stdout.strip()
            helper_root = root / "candidate"
            helper = helper_root / "script/ci/install_msan_libcxx.py"
            helper.parent.mkdir(parents=True)
            shutil.copyfile(REPOSITORY / "script/ci/install_msan_libcxx.py", helper)

            with mock.patch.object(self.matrix, "MSAN_LLVM_BASE_REVISION", revision):
                identity = self.matrix._msan_llvm_dependency_identity(helper_root, str(llvm))
                self.assertEqual(identity["status"], "clean")
                self.assertEqual(identity["revision"], revision)
                self.assertEqual(identity["helper_sha256"], self.matrix.MSAN_HELPER_SHA256)

                (llvm / "README").write_text("modified tree\n", encoding="utf-8")
                with self.assertRaisesRegex(self.matrix.MatrixError, "dirty"):
                    self.matrix._msan_llvm_dependency_identity(helper_root, str(llvm))

    def test_msan_ignorelist_is_required_and_its_bytes_are_captured(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            row = next(row for row in self.manifest["rows"] if row["id"] == "linux_msan")
            resource_dir = root / "clang-resource"
            runtime_dir = resource_dir / "lib/linux"
            runtime_dir.mkdir(parents=True)
            (runtime_dir / "libclang_rt.msan-aarch64.a").write_bytes(b"msan")
            (runtime_dir / "libclang_rt.msan_cxx-aarch64.a").write_bytes(b"msan-cxx")
            ignorelist = resource_dir / "share/msan_ignorelist.txt"
            ignorelist.parent.mkdir()
            ignorelist.write_bytes(b"stock ignorelist\n")
            prefix = root / "msan-libcxx"
            library = prefix / "lib/libc++abi.so.1"
            library.parent.mkdir(parents=True)
            library.write_bytes(b"instrumented libc++abi")
            system_unwinder = root / "system/libgcc_s.so.1"
            system_unwinder.parent.mkdir()
            system_unwinder.write_bytes(b"system libgcc_s")
            environment = {
                "MC_ANALYSIS_ROOT": str(root),
                "MC_MSAN_LIBCXX_PREFIX": str(prefix),
                "CFLAGS": f"-resource-dir {resource_dir}",
                "DEBUGINFOD_URLS": "",
                "PATH": "/captured/bin",
            }
            tools = [
                {"probe_argv": ["clang", "--version"], "executable": "/captured/bin/clang"},
                {"probe_argv": ["ldd", "--version"], "executable": "/captured/bin/ldd"},
            ]
            ldd_result = subprocess.CompletedProcess(
                ["ldd", str(library.resolve())],
                0,
                stdout=f"libgcc_s.so.1 => {system_unwinder.resolve()} (0x1234)\n",
                stderr="",
            )
            with mock.patch.object(self.matrix.subprocess, "run", return_value=ldd_result) as run:
                first = self.matrix._sanitizer_runtime_identity(
                    root,
                    row,
                    environment,
                    tools,
                    {"status": "present", "values": {}},
                    [],
                )
                self.assertEqual(first["msan_ignorelist"]["sha256"], hashlib.sha256(b"stock ignorelist\n").hexdigest())
                self.assertEqual(first["system_unwinder"]["path"], str(system_unwinder.resolve()))
                self.assertEqual(run.call_args.kwargs["env"]["DEBUGINFOD_URLS"], "")

                ignorelist.write_bytes(b"changed ignorelist\n")
                second = self.matrix._sanitizer_runtime_identity(
                    root,
                    row,
                    environment,
                    tools,
                    {"status": "present", "values": {}},
                    [],
                )
                self.assertNotEqual(first["msan_ignorelist"]["sha256"], second["msan_ignorelist"]["sha256"])
                ignorelist.unlink()
                with self.assertRaisesRegex(self.matrix.MatrixError, "stock MSan ignorelist"):
                    self.matrix._sanitizer_runtime_identity(
                        root,
                        row,
                        environment,
                        tools,
                        {"status": "present", "values": {}},
                        [],
                    )

    def test_msan_system_unwinder_probe_rejects_libunwind(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / "msan-libcxx"
            library = prefix / "lib/libc++abi.so.1"
            library.parent.mkdir(parents=True)
            library.write_bytes(b"instrumented libc++abi")
            environment = {"MC_MSAN_LIBCXX_PREFIX": str(prefix), "DEBUGINFOD_URLS": ""}
            tools = [{"probe_argv": ["ldd", "--version"], "executable": "/captured/bin/ldd"}]
            result = subprocess.CompletedProcess(
                ["ldd", str(library.resolve())],
                0,
                stdout="libunwind.so.1 => /lib/libunwind.so.1 (0x1234)\n",
                stderr="",
            )
            with mock.patch.object(self.matrix.subprocess, "run", return_value=result):
                with self.assertRaisesRegex(self.matrix.MatrixError, "libunwind"):
                    self.matrix._msan_system_unwinder_identity(Path(directory), environment, tools)

    def test_lsan_libcxx_prefix_identity_hashes_consumed_runtime_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / "llvm-18"
            header = prefix / "include/c++/v1/__config"
            libcxx = prefix / "lib/libc++.so.1"
            libcxxabi = prefix / "lib/libc++abi.so.1"
            header.parent.mkdir(parents=True)
            libcxx.parent.mkdir(parents=True)
            header.write_bytes(b"libc++ headers")
            libcxx.write_bytes(b"system libc++")
            libcxxabi.write_bytes(b"system libc++abi")
            row = next(row for row in self.manifest["rows"] if row["id"] == "linux_lsan")
            environment = {"MC_LSAN_LIBCXX_PREFIX": str(prefix)}
            with mock.patch.dict(
                self.matrix.DEPENDENCY_ENVIRONMENT,
                {"linux_lsan": ("MC_LSAN_LIBCXX_PREFIX",)},
            ):
                first = self.matrix._external_dependency_identity(root, row, environment)
                self.assertEqual(first["MC_LSAN_LIBCXX_PREFIX"]["path"], str(prefix))
                self.assertEqual(first["MC_LSAN_LIBCXX_PREFIX"]["file_count"], 3)

                libcxx.write_bytes(b"changed system libc++")
                second = self.matrix._external_dependency_identity(root, row, environment)
                self.assertNotEqual(
                    first["MC_LSAN_LIBCXX_PREFIX"]["files_sha256"],
                    second["MC_LSAN_LIBCXX_PREFIX"]["files_sha256"],
                )

    def test_offline_receipt_validation_rejects_a_changed_system_unwinder_identity(self):
        receipt = self.receipt()
        row = next(row for row in receipt["rows"] if row["id"] == "linux_msan")
        row["input_identity"]["runtime_resources"]["system_unwinder"].update(
            {"name": "libunwind.so.1", "path": "/usr/lib/libunwind.so.1"}
        )
        self.rehash_input_identity(row)
        with self.assertRaisesRegex(self.matrix.MatrixError, "system libgcc_s unwinder"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_offline_linux_msan_paths_are_not_resolved_by_the_verifier_host(self):
        receipt = self.receipt()
        row = next(row for row in receipt["rows"] if row["id"] == "linux_msan")
        identity = row["input_identity"]
        prefix = "/home/guest/msan-prefix"
        identity["environment"]["MC_MSAN_LIBCXX_PREFIX"] = prefix
        identity["external_dependencies"]["MC_MSAN_LIBCXX_PREFIX"]["path"] = prefix
        identity["runtime_resources"]["system_unwinder"]["probe_path"] = (
            f"{prefix}/lib/libc++abi.so.1"
        )
        self.rehash_input_identity(row)
        original_resolve = Path.resolve
        foreign_paths: list[str] = []

        def resolve_on_verifier_host(path: Path, *args, **kwargs):
            value = str(path)
            if value.startswith("/home/guest/"):
                foreign_paths.append(value)
                return Path("/System/Volumes/Data") / value.lstrip("/")
            return original_resolve(path, *args, **kwargs)

        with mock.patch.object(Path, "resolve", resolve_on_verifier_host):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

        self.assertEqual(foreign_paths, [])

    def test_offline_linux_msan_probe_rejects_parent_path_escape(self):
        receipt = self.receipt()
        row = next(row for row in receipt["rows"] if row["id"] == "linux_msan")
        prefix = row["input_identity"]["environment"]["MC_MSAN_LIBCXX_PREFIX"]
        row["input_identity"]["runtime_resources"]["system_unwinder"]["probe_path"] = (
            f"{prefix}/lib/../../outside/libc++abi.so.1"
        )
        self.rehash_input_identity(row)

        with self.assertRaisesRegex(self.matrix.MatrixError, r"escapes the instrumented libc\+\+ prefix"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_macos_address_row_keeps_integrated_lsan_controls_and_linux_lsan_required(self):
        row = next(row for row in self.manifest["rows"] if row["id"] == "macos_asan")
        self.assertEqual(row["tool_probes"][0], ["python3", "--version"])
        self.assertEqual(row["environment"]["ASAN_OPTIONS"], "detect_leaks=1")
        self.assertEqual(
            row["environment"]["LSAN_OPTIONS"],
            "suppressions={env:MC_ANALYSIS_ROOT}/script/ci/macos_lsan.supp:print_suppressions=1",
        )
        self.assertEqual(
            row["commands"],
            [
                ["python3", "script/ci/verify_macos_lsan.py", "controls", "--build-dir", "build/analysis-macos-asan/lsan-controls"],
                ["cmake", "--preset", "analysis-macos-asan"],
                ["cmake", "--build", "--preset", "analysis-macos-asan"],
                ["ctest", "--test-dir", "build/analysis-macos-asan", "--output-on-failure", "--no-tests=error"],
                ["python3", "script/ci/verify_macos_lsan.py", "summarize", "--last-test-log", "build/analysis-macos-asan/Testing/Temporary/LastTest.log"],
            ],
        )
        self.assertEqual(
            self.matrix.ROW_INPUT_SCOPES["macos_asan"],
            ("common", "macos_lsan"),
        )
        self.assertIn("linux_lsan", self.matrix.REQUIRED_ROW_IDS)
        linux_lsan = next(row for row in self.manifest["rows"] if row["id"] == "linux_lsan")
        self.assertEqual(linux_lsan["required_compile_flags"], ["-fsanitize=leak", "-fexperimental-library"])
        self.assertTrue(linux_lsan.get("analysis_only"))
        self.assertFalse(any(command[0] == "leaks" for command in row["commands"]))
        configure_index = row["commands"].index(["cmake", "--preset", "analysis-macos-asan"])
        build_index = row["commands"].index(["cmake", "--build", "--preset", "analysis-macos-asan"])
        ctest_index = next(index for index, command in enumerate(row["commands"]) if command[0] == "ctest")
        summary_index = next(
            index for index, command in enumerate(row["commands"]) if command[1:3] == ["script/ci/verify_macos_lsan.py", "summarize"]
        )
        controls_index = next(
            index for index, command in enumerate(row["commands"]) if command[1:3] == ["script/ci/verify_macos_lsan.py", "controls"]
        )
        self.assertLess(controls_index, configure_index)
        self.assertLess(build_index, ctest_index)
        self.assertLess(ctest_index, summary_index)

    def test_matrix_row_environment_resolves_checkout_root_placeholders(self):
        row = next(row for row in self.manifest["rows"] if row["id"] == "macos_asan")
        environment = self.matrix.resolve_row_environment(
            row,
            Path("/private/tmp/minecraft"),
            {"ASAN_OPTIONS": "detect_leaks=0", "LSAN_OPTIONS": "exitcode=0"},
        )
        expected_root = str(Path("/private/tmp/minecraft").resolve())
        self.assertEqual(environment["MC_ANALYSIS_ROOT"], expected_root)
        self.assertEqual(environment["ASAN_OPTIONS"], "detect_leaks=1")
        self.assertEqual(
            environment["LSAN_OPTIONS"],
            f"suppressions={expected_root}/script/ci/macos_lsan.supp:print_suppressions=1",
        )

    def test_macos_asan_rejects_manifest_that_disables_integrated_leak_detection(self):
        manifest = copy.deepcopy(self.manifest)
        row = next(row for row in manifest["rows"] if row["id"] == "macos_asan")
        row["environment"]["ASAN_OPTIONS"] = "detect_leaks=0"
        with self.assertRaisesRegex(self.matrix.MatrixError, "integrated LeakSanitizer policy"):
            self.matrix.validate_manifest(manifest)

    def test_macos_asan_identity_rejects_inherited_leak_disabling_override(self):
        receipt = self.receipt()
        row = next(row for row in receipt["rows"] if row["id"] == "macos_asan")
        self.assertEqual(row["input_identity"]["environment"]["ASAN_OPTIONS"], "detect_leaks=1")
        row["input_identity"]["environment"]["ASAN_OPTIONS"] = "detect_leaks=0"
        self.rehash_input_identity(row)
        with self.assertRaisesRegex(self.matrix.MatrixError, "effective row options"):
            self.matrix.validate_receipt(receipt, self.manifest, CANDIDATE)

    def test_linux_presets_are_tests_only_and_use_distinct_sanitizers(self):
        presets = json.loads((REPOSITORY / "CMakePresets.json").read_text(encoding="utf-8"))
        configure = {item["name"]: item for item in presets["configurePresets"]}
        linux_base = configure["analysis-linux-base"]["cacheVariables"]
        self.assertEqual(linux_base["MC_ANALYSIS_HOST"], "ON")
        self.assertEqual(linux_base["MC_BUILD_CLIENT"], "OFF")
        self.assertEqual(configure["analysis-linux-lsan"]["cacheVariables"]["MC_SANITIZER"], "leak")
        self.assertEqual(configure["analysis-linux-msan"]["cacheVariables"]["MC_SANITIZER"], "memory")
        msan_variables = configure["analysis-linux-msan"]["cacheVariables"]
        for flag in (
            "-fsanitize=memory",
            "-fsanitize-memory-track-origins=2",
            "-fno-omit-frame-pointer",
        ):
            self.assertIn(flag, msan_variables["CMAKE_CXX_FLAGS"])
        self.assertIn("-fsanitize=memory", msan_variables["CMAKE_EXE_LINKER_FLAGS"])
        self.assertIn("-fsanitize=memory", msan_variables["CMAKE_SHARED_LINKER_FLAGS"])

    def test_sanitizer_presets_use_optimized_test_configuration(self):
        presets = json.loads((REPOSITORY / "CMakePresets.json").read_text(encoding="utf-8"))
        configure = {item["name"]: item for item in presets["configurePresets"]}
        base_variables = configure["base"]["cacheVariables"]
        release_variables = configure["release"]["cacheVariables"]
        analysis_base = configure["analysis-base"]

        self.assertEqual(analysis_base["inherits"], "release")
        self.assertEqual(release_variables["CMAKE_BUILD_TYPE"], "Release")
        self.assertEqual(base_variables["MC_ENABLE_HIGH_ASSERT"], "OFF")

        sanitizer_flags = {
            row["id"]: row["flags"]
            for row in self.manifest["rows"]
            if row["id"] in {
                "macos_asan",
                "macos_ubsan",
                "macos_tsan",
                "windows_msvc_asan",
                "linux_lsan",
                "linux_msan",
            }
        }
        self.assertEqual(len(sanitizer_flags), 6)
        for row_id, flags in sanitizer_flags.items():
            with self.subTest(row_id=row_id):
                self.assertIn("-DCMAKE_BUILD_TYPE=Release", flags)
                self.assertIn("-DMC_ENABLE_HIGH_ASSERT=OFF", flags)

    def test_msvc_analysis_preset_fails_on_code_analysis_warnings(self):
        presets = json.loads((REPOSITORY / "CMakePresets.json").read_text(encoding="utf-8"))
        configure = {item["name"]: item for item in presets["configurePresets"]}
        variables = configure["analysis-windows-analyze"]["cacheVariables"]
        row = next(row for row in self.manifest["rows"] if row["id"] == "windows_msvc_analyze")
        self.assertEqual(row["flags"], ["/analyze", "/W4", "/WX"])
        self.assertEqual(variables["CMAKE_BUILD_TYPE"], "Release")
        self.assertEqual(variables["CMAKE_MSVC_RUNTIME_LIBRARY"], "MultiThreaded")
        self.assertEqual(variables["CMAKE_C_FLAGS"], "/analyze /W4 /WX /analyze:external-")
        self.assertEqual(variables["CMAKE_CXX_FLAGS"], "/analyze /W4 /WX /EHsc /analyze:external-")

        for language in ("C", "CXX"):
            with self.subTest(language=language):
                flags = variables[f"CMAKE_{language}_FLAGS"].split()
                self.assertIn("/analyze", flags)
                self.assertIn("/WX", flags)
                self.assertNotIn("/analyze:WX-", flags)

    def test_iwyu_preset_fails_on_include_violations(self):
        presets = json.loads((REPOSITORY / "CMakePresets.json").read_text(encoding="utf-8"))
        configure = {item["name"]: item for item in presets["configurePresets"]}
        command = configure["analysis-iwyu"]["cacheVariables"]["CMAKE_CXX_INCLUDE_WHAT_YOU_USE"]
        row = next(row for row in self.manifest["rows"] if row["id"] == "iwyu")

        self.assertIn("iwyu_gate.py", command)
        self.assertIn("--no_fwd_decls", command)
        self.assertIn(";-Xiwyu;--error;--", command)
        self.assertTrue(any("CMake -Xiwyu --error suffix propagates wrapper failures" in flag for flag in row["flags"]))
        self.assertTrue(any("owned public include additions fail" in flag for flag in row["flags"]))

    def test_cppcheck_manifest_records_classified_gate_policy(self):
        row = next(row for row in self.manifest["rows"] if row["id"] == "cppcheck")

        self.assertTrue(any("CMAKE_CXX_COMPILER_LAUNCHER" in flag for flag in row["flags"]))
        self.assertTrue(any("error, warning, performance, and portability" in flag for flag in row["flags"]))
        self.assertNotIn("--error-exitcode=1", ";".join(row["flags"]))

    def test_ctest_rows_fail_when_no_tests_are_registered(self):
        rows = {row["id"]: row for row in self.manifest["rows"]}
        for row_id in (
            "macos_asan",
            "macos_ubsan",
            "macos_tsan",
            "windows_msvc_asan",
            "windows_msvc_analyze",
        ):
            ctest_commands = [
                command for command in rows[row_id]["commands"] if command[0] == "ctest"
            ]
            self.assertTrue(ctest_commands, row_id)
            with self.subTest(row_id=row_id):
                self.assertTrue(all("--no-tests=error" in command for command in ctest_commands))

    def test_ubsan_row_stops_on_first_runtime_diagnostic(self):
        row = next(row for row in self.manifest["rows"] if row["id"] == "macos_ubsan")
        self.assertEqual(
            row["environment"].get("UBSAN_OPTIONS"),
            "halt_on_error=1:print_stacktrace=1",
        )

    def test_windows_ctest_rows_run_complete_suites_with_bounded_parallelism(self):
        for row in self.manifest["rows"]:
            if row["id"] not in ("windows_msvc_asan", "windows_msvc_analyze"):
                continue
            commands = [command for command in row["commands"] if command[0] == "ctest"]
            self.assertEqual(len(commands), 1, row["id"])
            with self.subTest(row_id=row["id"]):
                command = commands[0]
                self.assertIn("--no-tests=error", command)
                self.assertIn("--parallel", command)
                self.assertEqual(command[command.index("--parallel") + 1], "1")
                self.assertFalse(set(command) & {"-R", "-E", "-I", "--tests-from-file", "--exclude-from-file"})

    def test_android_hwasan_release_has_an_isolated_application_id(self):
        build_gradle = (REPOSITORY / "android/app/build.gradle").read_text(encoding="utf-8")
        self.assertIn("applicationId 'com.corejust.minecraftclone'", build_gradle)
        self.assertIn("namespace 'com.corejust.minecraftclone'", build_gradle)
        sanitizer_block = build_gradle.split("if (sanitizer != null) {", 1)[1].split(
            "if (vcpkgInstalledDir != null", 1
        )[0]
        self.assertIn("if (sanitizer != 'hwasan')", sanitizer_block)
        self.assertIn("throw new GradleException", sanitizer_block)
        suffix = "android.buildTypes.release.applicationIdSuffix = '.hwasan'"
        self.assertIn(suffix, sanitizer_block)
        self.assertLess(sanitizer_block.index("throw new GradleException"), sanitizer_block.index(suffix))
        self.assertEqual(build_gradle.count("applicationIdSuffix"), 1)

    def test_android_hwasan_gradle_rejects_counterfeit_native_libraries(self):
        groovy_jars = sorted(Path.home().glob(".gradle/wrapper/dists/*/*/*/lib/groovy-[0-9]*.jar"))
        java = shutil.which("java")
        if not java or not groovy_jars:
            self.skipTest("Java and Gradle's bundled Groovy are required")
        build_gradle = (REPOSITORY / "android/app/build.gradle").read_text(encoding="utf-8")
        validation = build_gradle.split("tasks.register('verifyHwasanReleaseApk')", 1)[1].split(
            "ZipFile zip = new ZipFile(apk)", 1
        )[1].split(
            "} finally {", 1
        )[0]
        header = bytearray(64)
        header[:6] = b"\x7fELF\x02\x01"
        header[18:20] = b"\xb7\x00"
        marker = b"libclang_rt.hwasan-aarch64-android.so"
        libraries = {"valid": bytes(header) + marker, "non_elf": b"counterfeit" + marker}
        for name, offset, value in (
            ("magic", 0, 0),
            ("class", 4, 1),
            ("byte_order", 5, 2),
            ("machine", 18, 62),
            ("machine_high", 19, 1),
        ):
            invalid = bytearray(header)
            invalid[offset] = value
            libraries[name] = bytes(invalid) + marker
        libraries["short"] = bytes(header[:20]) + marker
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "validate.groovy"
            script.write_text(
                "import java.util.zip.ZipFile\n"
                "class GradleException extends RuntimeException {\n"
                "    GradleException(String message) { super(message) }\n}\n"
                "File apk = new File(args[0])\nZipFile zip = new ZipFile(apk)\n"
                + validation + "} finally { zip.close() }\n",
                encoding="utf-8",
            )
            for name, library in libraries.items():
                with self.subTest(library=name):
                    apk = root / f"{name}.apk"
                    with zipfile.ZipFile(apk, "w") as archive:
                        archive.writestr(
                            "lib/arm64-v8a/wrap.sh", '#!/system/bin/sh\nLD_HWASAN=1 exec "$@"\n'
                        )
                        archive.writestr("lib/arm64-v8a/libmc_android.so", library)
                    result = subprocess.run(
                        [java, "-cp", str(groovy_jars[0]), "groovy.ui.GroovyMain", str(script), str(apk)],
                        capture_output=True, text=True, timeout=30,
                    )
                    if name == "valid":
                        self.assertEqual(result.returncode, 0, result.stderr)
                    else:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn("not a 64-bit little-endian AArch64 ELF", result.stderr)

if __name__ == "__main__":
    unittest.main()
