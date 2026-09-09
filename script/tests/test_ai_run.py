import contextlib
import importlib.util
import io
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "script/ai_run.py"


def load_module():
    spec = importlib.util.spec_from_file_location("ai_run", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


ai_run = load_module()


class AiRunTests(unittest.TestCase):
    def test_all_routes_select_expected_model(self):
        expected_models = {
            "astra": "gpt-6-astra",
            "sol": "gpt-5.6-sol",
            "terra": "gpt-5.6-terra",
            "luna": "gpt-5.6-luna",
        }
        for route, model in expected_models.items():
            with self.subTest(route=route), mock.patch.object(
                ai_run.subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 0),
            ) as run:
                self.assertEqual(ai_run.main([route, "--", "--version"]), 0)
                run.assert_called_once_with(
                    ["codex", "--model", model, "-c", 'model_reasoning_effort="high"', "--version"],
                    cwd=REPOSITORY,
                    check=False,
                    shell=False,
                )

    def test_separator_is_optional_and_arguments_are_not_interpreted(self):
        forwarded = ["value with spaces", "$(touch /tmp/should-not-exist)", "semi;colon", "'quoted'"]
        for separator in ([], ["--"]):
            with self.subTest(separator=separator), mock.patch.object(
                ai_run.subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 0),
            ) as run:
                self.assertEqual(ai_run.main(["terra", *separator, *forwarded]), 0)
                self.assertEqual(run.call_args.args[0][-len(forwarded):], forwarded)

    def test_dry_run_prints_shell_escaped_command_without_launching(self):
        with mock.patch.object(ai_run.subprocess, "run") as run:
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                result = ai_run.main(["--dry-run", "luna", "--", "arg with spaces"])

        self.assertEqual(result, 0)
        self.assertEqual(
            output.getvalue(),
            'codex --model gpt-5.6-luna -c \'model_reasoning_effort="high"\' \'arg with spaces\'\n',
        )
        run.assert_not_called()

    def test_unknown_route_is_rejected(self):
        with self.assertRaises(SystemExit) as error:
            ai_run.main(["unknown"])
        self.assertEqual(error.exception.code, 2)

    def test_missing_codex_returns_127_with_clear_error(self):
        with mock.patch.object(ai_run.subprocess, "run", side_effect=FileNotFoundError), contextlib.redirect_stderr(
            io.StringIO()
        ) as error:
            result = ai_run.main(["astra"])

        self.assertEqual(result, 127)
        self.assertIn("codex executable not found", error.getvalue())


if __name__ == "__main__":
    unittest.main()
