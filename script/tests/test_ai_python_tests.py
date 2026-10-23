import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "script/ai_python_tests.py"


def load_module():
    spec = importlib.util.spec_from_file_location("ai_python_tests", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class AiPythonTestsRunnerTests(unittest.TestCase):
    def test_cli_adds_explicit_repository_root_before_discovery(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            package = root / "script"
            tests = package / "tests"
            tests.mkdir(parents=True)
            (package / "__init__.py").write_text("", encoding="utf-8")
            (package / "probe.py").write_text("VALUE = 17\n", encoding="utf-8")
            (tests / "test_root_import.py").write_text(
                "import unittest\n"
                "from script.probe import VALUE\n\n"
                "class RootImportTest(unittest.TestCase):\n"
                "    def test_import_uses_selected_repository_root(self):\n"
                "        self.assertEqual(VALUE, 17)\n",
                encoding="utf-8",
            )
            (tests / "test_medium.py").write_text(
                "import unittest\n\n"
                "class MediumTest(unittest.TestCase):\n"
                "    def test_deferred(self):\n"
                "        self.fail('medium group must be deferred')\n",
                encoding="utf-8",
            )
            (package / "validation_inventory.json").write_text(
                json.dumps(
                    {
                        "required_python_test_groups": {"medium": "script/tests/test_medium.py"},
                        "python_test_groups": [
                            {
                                "id": "medium",
                                "source": "script/tests/test_medium.py",
                                "size": "medium",
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            environment = os.environ.copy()
            environment.pop("PYTHONPATH", None)
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--root",
                    str(root),
                ],
                cwd=directory,
                env=environment,
                capture_output=True,
                text=True,
                timeout=20,
                check=False,
            )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Ran 1 test", result.stderr)
        self.assertIn("DEFERRED script/tests/test_medium.py: 1 test cases", result.stdout)

    def test_only_inventory_classified_medium_source_is_deferred(self):
        runner = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            medium_source = root / "script/tests/test_verify_gameplay_gate.py"
            small_source = root / "script/tests/test_small.py"
            medium_source.parent.mkdir(parents=True)
            medium_source.write_text("", encoding="utf-8")
            small_source.write_text("", encoding="utf-8")
            medium_source = medium_source.resolve()
            small_source = small_source.resolve()
            cases = [unittest.FunctionTestCase(lambda: None), unittest.FunctionTestCase(lambda: None)]

            with mock.patch.object(
                runner,
                "_test_source",
                side_effect=[medium_source, small_source],
            ):
                selected, deferred = runner.select_tests(
                    root,
                    cases,
                    {"script/tests/test_verify_gameplay_gate.py": medium_source},
                )

        self.assertEqual(selected, [cases[1]])
        self.assertEqual(deferred, {medium_source: 1})

    def test_targeted_medium_source_is_deferred_without_running_other_tests(self):
        runner = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            medium_source = root / "script/tests/test_verify_gameplay_gate.py"
            small_source = root / "script/tests/test_small.py"
            medium_source.parent.mkdir(parents=True)
            medium_source.write_text("", encoding="utf-8")
            small_source.write_text("", encoding="utf-8")
            medium_source = medium_source.resolve()
            small_source = small_source.resolve()
            cases = [unittest.FunctionTestCase(lambda: None), unittest.FunctionTestCase(lambda: None)]

            with mock.patch.object(
                runner,
                "_test_source",
                side_effect=[medium_source, small_source],
            ):
                selected, deferred = runner.select_tests(
                    root,
                    cases,
                    {"script/tests/test_verify_gameplay_gate.py": medium_source},
                    targets=["script/tests/test_verify_gameplay_gate.py"],
                )

        self.assertEqual(selected, [])
        self.assertEqual(deferred, {medium_source: 1})

    def test_inventory_missing_a_classified_test_source_fails_closed(self):
        runner = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "script").mkdir()
            (root / "script/validation_inventory.json").write_text(
                '{"required_python_test_groups":{"missing":"script/tests/test_missing.py"},'
                '"python_test_groups":[{"id":"missing","source":'
                '"script/tests/test_missing.py","size":"medium"}]}',
                encoding="utf-8",
            )

            with self.assertRaisesRegex(runner.TestSelectionError, "source is missing"):
                runner._medium_test_sources(root)


if __name__ == "__main__":
    unittest.main()
