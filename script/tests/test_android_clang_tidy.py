from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from script.ci import android_clang_tidy


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "script/ci/android_clang_tidy.py"


class AndroidClangTidyTests(unittest.TestCase):
    def test_uses_response_files_for_windows_batch_analyzers(self):
        self.assertTrue(android_clang_tidy._uses_response_file("clang-tidy.cmd", platform_name="nt"))
        self.assertTrue(android_clang_tidy._uses_response_file("clang-tidy.bat", platform_name="nt"))
        with patch("script.ci.android_clang_tidy.shutil.which", return_value=r"C:\LLVM\clang-tidy.cmd"):
            self.assertTrue(android_clang_tidy._uses_response_file("clang-tidy", platform_name="nt"))

    def test_keeps_native_and_posix_analyzers_out_of_response_files(self):
        self.assertFalse(android_clang_tidy._uses_response_file("clang-tidy.exe", platform_name="nt"))
        self.assertFalse(android_clang_tidy._uses_response_file("clang-tidy.cmd", platform_name="posix"))

    def create_fake_clang_tidy(
        self, root: Path, argument_log: Path, *, batch: bool | None = None
    ) -> tuple[Path, dict[str, str]]:
        fake_script = root / "fake_clang_tidy.py"
        fake_script.write_text(
            "#!/usr/bin/env python3\n"
            "import json\n"
            "import os\n"
            "import sys\n"
            "from pathlib import Path\n"
            "arguments = sys.argv[1:]\n"
            "record = {'arguments': arguments}\n"
            "if len(arguments) == 1 and arguments[0].startswith('@'):\n"
            "    record = {'response_file': Path(arguments[0][1:]).read_text(encoding='utf-8')}\n"
            "with open(os.environ['ANDROID_CLANG_TIDY_ARGV'], 'a', encoding='utf-8') as log:\n"
            "    log.write(json.dumps(record) + '\\n')\n"
            "raise SystemExit(int(os.environ.get('ANDROID_CLANG_TIDY_EXIT_CODE', '0')))\n",
            encoding="utf-8",
        )
        if batch is None:
            batch = os.name == "nt"
        if batch:
            clang_tidy = root / "clang-tidy.cmd"
            if os.name == "nt":
                clang_tidy.write_text(
                    '@echo off\r\n'
                    '"%PYTHON%" "%ANDROID_CLANG_TIDY_FAKE_SCRIPT%" %*\r\n'
                    'exit /b %errorlevel%\r\n',
                    encoding="utf-8",
                )
            else:
                clang_tidy.write_text(fake_script.read_text(encoding="utf-8"), encoding="utf-8")
                clang_tidy.chmod(0o755)
            environment = {
                "ANDROID_CLANG_TIDY_FAKE_SCRIPT": str(fake_script),
                "PYTHON": sys.executable,
            }
        else:
            clang_tidy = fake_script
            clang_tidy.chmod(0o755)
            environment = {}
        environment["ANDROID_CLANG_TIDY_ARGV"] = str(argument_log)
        return clang_tidy, environment

    def record_contains(self, record: dict[str, object], argument: str) -> bool:
        if "response_file" in record:
            return subprocess.list2cmdline([argument]) in record["response_file"]
        return argument in record["arguments"]

    def assert_record_contains(self, record: dict[str, object], argument: str):
        self.assertTrue(self.record_contains(record, argument), argument)

    def create_fixture(self, root: Path, target: str) -> tuple[Path, list[Path]]:
        sources = [root / "src/android/Player.cpp", root / "src/shared/World.cpp"]
        for source in sources:
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("int value() { return 0; }\n", encoding="utf-8")

        compile_commands_dir = root / "android/app/.cxx/Release/abc123/arm64-v8a"
        compile_commands_dir.mkdir(parents=True)
        entries = [
            {
                "directory": str(root),
                "file": str(source),
                "arguments": ["clang++", f"--target={target}", "-c", str(source)],
            }
            for source in sources
        ]
        (compile_commands_dir / "compile_commands.json").write_text(
            json.dumps(entries), encoding="utf-8"
        )
        return compile_commands_dir, sources

    def test_runs_clang_tidy_for_each_android_translation_unit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            compile_commands_dir, sources = self.create_fixture(
                root, "aarch64-none-linux-android28"
            )
            argument_log = root / "clang-tidy-arguments.txt"
            environment = os.environ.copy()
            clang_tidy, fake_environment = self.create_fake_clang_tidy(root, argument_log)
            environment.update(fake_environment)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--compile-commands-dir",
                    str(compile_commands_dir),
                    "--repository-root",
                    str(root),
                    "--clang-tidy",
                    str(clang_tidy),
                ],
                cwd=root,
                env=environment,
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            record = json.loads(argument_log.read_text(encoding="utf-8").splitlines()[0])
            self.assert_record_contains(record, "--checks=clang-analyzer-*")
            self.assert_record_contains(record, "--warnings-as-errors=clang-analyzer-*")
            self.assert_record_contains(record, "-p")
            self.assert_record_contains(record, str(compile_commands_dir.resolve()))
            for source in sources:
                self.assert_record_contains(record, str(source.resolve()))

    def test_analyzes_every_release_compile_database(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            compile_commands_dir, sources = self.create_fixture(
                root, "aarch64-none-linux-android28"
            )
            second_source = root / "src/client/Third.cpp"
            second_source.parent.mkdir(parents=True)
            second_source.write_text("int third() { return 0; }\n", encoding="utf-8")
            second_database_dir = compile_commands_dir.parents[1] / "second/arm64-v8a"
            second_database_dir.mkdir(parents=True)
            (second_database_dir / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": str(root),
                            "file": str(second_source),
                            "arguments": [
                                "clang++",
                                "--target=aarch64-none-linux-android28",
                                "-c",
                                str(second_source),
                            ],
                        }
                    ]
                ),
                encoding="utf-8",
            )
            argument_log = root / "clang-tidy-arguments.txt"
            environment = os.environ.copy()
            clang_tidy, fake_environment = self.create_fake_clang_tidy(root, argument_log)
            environment.update(fake_environment)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--compile-commands-dir",
                    str(compile_commands_dir.parents[1]),
                    "--repository-root",
                    str(root),
                    "--clang-tidy",
                    str(clang_tidy),
                ],
                cwd=root,
                env=environment,
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            records = [
                json.loads(line) for line in argument_log.read_text(encoding="utf-8").splitlines()
            ]
            self.assertEqual(len(records), 2)
            for source in [*sources, second_source]:
                self.assertTrue(
                    any(self.record_contains(record, str(source.resolve())) for record in records),
                    f"no analyzer invocation contains {source}",
                )

    def test_windows_batch_uses_response_file_for_shell_metacharacters(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            argument_log = root / "clang-tidy-arguments.txt"
            clang_tidy, environment = self.create_fake_clang_tidy(root, argument_log, batch=True)
            command = [
                str(clang_tidy),
                "--checks=clang-analyzer-*",
                r"--header-filter=^C:\workspace/(?:src|tests)/",
                "-p",
                "build output",
                r"src\Player.cpp",
            ]

            with patch.dict(os.environ, environment):
                result = android_clang_tidy._run_clang_tidy(command, root, platform_name="nt")

            self.assertEqual(result.returncode, 0)
            record = json.loads(argument_log.read_text(encoding="utf-8").splitlines()[0])
            self.assertEqual(record["response_file"], subprocess.list2cmdline(command[1:]) + "\n")
            self.assertEqual(list(root.glob(".clang-tidy-args-*")), [])

    def test_windows_batch_launch_uses_shell_only_on_windows(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch("script.ci.android_clang_tidy.subprocess.run") as run:
                run.return_value = subprocess.CompletedProcess([], 0)
                android_clang_tidy._run_clang_tidy(
                    ["clang-tidy.cmd", "--checks=clang-analyzer-*"], root, platform_name="nt"
                )

            self.assertEqual(run.call_args.kwargs["shell"], os.name == "nt")

    def test_windows_batch_response_file_is_removed_after_nonzero_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            argument_log = root / "clang-tidy-arguments.txt"
            clang_tidy, environment = self.create_fake_clang_tidy(root, argument_log, batch=True)
            environment["ANDROID_CLANG_TIDY_EXIT_CODE"] = "17"

            with patch.dict(os.environ, environment):
                result = android_clang_tidy._run_clang_tidy(
                    [str(clang_tidy), "--checks=clang-analyzer-*"], root, platform_name="nt"
                )

            self.assertEqual(result.returncode, 17)
            self.assertEqual(list(root.glob(".clang-tidy-args-*")), [])

    def test_windows_batch_response_file_is_removed_when_launch_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch(
                "script.ci.android_clang_tidy.subprocess.run", side_effect=OSError("launch failed")
            ):
                with self.assertRaisesRegex(OSError, "launch failed"):
                    android_clang_tidy._run_clang_tidy(
                        ["clang-tidy.cmd", "--checks=clang-analyzer-*"], root, platform_name="nt"
                    )

            self.assertEqual(list(root.glob(".clang-tidy-args-*")), [])

    def test_rejects_a_host_compile_database(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            compile_commands_dir, _ = self.create_fixture(root, "aarch64-linux-gnu")

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--compile-commands-dir",
                    str(compile_commands_dir),
                    "--repository-root",
                    str(root),
                    "--clang-tidy",
                    "unused-clang-tidy",
                ],
                cwd=root,
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Android target", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
