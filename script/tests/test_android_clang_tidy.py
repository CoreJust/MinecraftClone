from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "script/ci/android_clang_tidy.py"


class AndroidClangTidyTests(unittest.TestCase):
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
            clang_tidy = root / "clang-tidy"
            clang_tidy.write_text(
                "#!/bin/sh\nprintf '%s\\n' '--database--' \"$@\" >> \"$ANDROID_CLANG_TIDY_ARGV\"\n",
                encoding="utf-8",
            )
            clang_tidy.chmod(0o755)
            environment = os.environ.copy()
            environment["ANDROID_CLANG_TIDY_ARGV"] = str(argument_log)

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
            arguments = argument_log.read_text(encoding="utf-8").splitlines()
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
            clang_tidy = root / "clang-tidy"
            clang_tidy.write_text(
                "#!/bin/sh\nprintf '%s\\n' '--database--' \"$@\" >> \"$ANDROID_CLANG_TIDY_ARGV\"\n",
                encoding="utf-8",
            )
            clang_tidy.chmod(0o755)
            environment = os.environ.copy()
            environment["ANDROID_CLANG_TIDY_ARGV"] = str(argument_log)

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
            arguments = argument_log.read_text(encoding="utf-8").splitlines()
            self.assertEqual(arguments.count("--database--"), 2)
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
                    "/bin/true",
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
