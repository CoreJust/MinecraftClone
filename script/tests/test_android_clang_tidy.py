from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from script.ci import android_clang_tidy


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "script/ci/android_clang_tidy.py"


class AndroidClangTidyTests(unittest.TestCase):
    def test_uses_shell_for_windows_batch_analyzers(self):
        self.assertTrue(android_clang_tidy._requires_windows_shell("clang-tidy.cmd", platform_name="nt"))
        self.assertTrue(android_clang_tidy._requires_windows_shell("clang-tidy.bat", platform_name="nt"))

    def test_keeps_native_windows_analyzers_out_of_the_shell(self):
        self.assertFalse(android_clang_tidy._requires_windows_shell("clang-tidy.exe", platform_name="nt"))
        self.assertFalse(android_clang_tidy._requires_windows_shell("clang-tidy.cmd", platform_name="posix"))

    def create_fake_clang_tidy(self, root: Path, argument_log: Path) -> tuple[Path, dict[str, str]]:
        fake_script = root / "fake_clang_tidy.py"
        fake_script.write_text(
            "#!/usr/bin/env python3\n"
            "import json\n"
            "import os\n"
            "import sys\n"
            "with open(os.environ['ANDROID_CLANG_TIDY_ARGV'], 'a', encoding='utf-8') as log:\n"
            "    log.write(json.dumps(sys.argv[1:]) + '\\n')\n",
            encoding="utf-8",
        )
        if os.name == "nt":
            clang_tidy = root / "clang-tidy.cmd"
            clang_tidy.write_text(
                '@echo off\r\n'
                '"%PYTHON%" "%ANDROID_CLANG_TIDY_FAKE_SCRIPT%" %*\r\n'
                'exit /b %errorlevel%\r\n',
                encoding="utf-8",
            )
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
            arguments = json.loads(argument_log.read_text(encoding="utf-8").splitlines()[0])
            self.assertIn("--checks=clang-analyzer-*", arguments)
            self.assertIn("--warnings-as-errors=clang-analyzer-*", arguments)
            self.assertEqual(arguments[arguments.index("-p") + 1], str(compile_commands_dir.resolve()))
            for source in sources:
                self.assertIn(str(source.resolve()), arguments)

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
            argument_sets = [
                json.loads(line) for line in argument_log.read_text(encoding="utf-8").splitlines()
            ]
            self.assertEqual(len(argument_sets), 2)
            arguments = [argument for argument_set in argument_sets for argument in argument_set]
            for source in [*sources, second_source]:
                self.assertIn(str(source.resolve()), arguments)

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
