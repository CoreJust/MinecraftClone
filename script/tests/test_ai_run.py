import contextlib
import importlib.util
import io
import json
import subprocess
import sys
import tempfile
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

    def test_default_route_is_luna_high(self):
        with mock.patch.object(
            ai_run.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 0),
        ) as run:
            self.assertEqual(ai_run.main([]), 0)
        self.assertEqual(run.call_args.args[0][2], "gpt-5.6-luna")

    def test_token_usage_uses_final_cumulative_record_without_double_counting(self):
        with tempfile.TemporaryDirectory() as directory:
            rollout = Path(directory) / "rollout.jsonl"
            events = [
                {
                    "type": "session_meta",
                    "payload": {"base_instructions": {"provenance": {"model": "gpt-5.6-terra"}}},
                },
                {
                    "type": "token_usage_record",
                    "payload": {
                        "session_id": "session",
                        "thread_id": "thread",
                        "thread_token_usage": {
                            "input_tokens": 10,
                            "cached_input_tokens": 4,
                            "output_tokens": 2,
                        },
                    },
                },
                {
                    "type": "token_usage_record",
                    "payload": {
                        "session_id": "session",
                        "thread_id": "thread",
                        "thread_token_usage": {
                            "input_tokens": 25,
                            "cached_input_tokens": 12,
                            "output_tokens": 5,
                        },
                    },
                },
            ]
            rollout.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
            report = ai_run.token_usage_report([rollout])
        self.assertEqual(report["by_model"][0]["model"], "gpt-5.6-terra")
        self.assertEqual(report["by_model"][0]["usage"], {
            "input_tokens": 25,
            "cached_input_tokens": 12,
            "output_tokens": 5,
        })
        self.assertEqual(report["coverage"]["excluded_duplicate_rollouts"], 0)

    def test_token_usage_preserves_zero_and_marks_missing_metrics_unavailable(self):
        with tempfile.TemporaryDirectory() as directory:
            rollout = Path(directory) / "rollout.jsonl"
            rollout.write_text(json.dumps({
                "type": "token_usage_record",
                "payload": {"usage": {"input_tokens": 0, "output_tokens": 0}},
            }) + "\n", encoding="utf-8")
            report = ai_run.token_usage_report([rollout])
        usage = report["by_model"][0]["usage"]
        self.assertIsNone(usage["input_tokens"])
        self.assertIsNone(usage["output_tokens"])
        self.assertIsNone(usage["cached_input_tokens"])
        self.assertEqual(report["totals"]["input_tokens"], 0)
        self.assertEqual(report["totals"]["output_tokens"], 0)
        self.assertEqual(report["by_model"][0]["availability"], "unavailable")

    def test_token_usage_excludes_duplicate_session_thread_rollouts(self):
        with tempfile.TemporaryDirectory() as directory:
            first = Path(directory) / "first.jsonl"
            second = Path(directory) / "second.jsonl"
            event = {
                "type": "token_usage_record",
                "payload": {
                    "session_id": "session",
                    "thread_id": "thread",
                    "usage": {"input_tokens": 3, "cached_input_tokens": 1, "output_tokens": 2},
                },
            }
            content = json.dumps(event) + "\n"
            first.write_text(content, encoding="utf-8")
            second.write_text(content, encoding="utf-8")
            report = ai_run.token_usage_report([first, second])
        self.assertEqual(report["coverage"]["counted_rollouts"], 1)
        self.assertEqual(report["coverage"]["excluded_duplicate_rollouts"], 1)

    def test_token_usage_does_not_assign_mixed_models_to_the_last_model(self):
        with tempfile.TemporaryDirectory() as directory:
            rollout = Path(directory) / "mixed.jsonl"
            events = [
                {"type": "session_meta", "payload": {"model": "gpt-5.6-luna"}},
                {"type": "session_meta", "payload": {"model": "gpt-5.6-terra"}},
                {"type": "token_usage_record", "payload": {
                    "session_id": "session",
                    "thread_id": "thread",
                    "thread_token_usage": {
                        "input_tokens": 5,
                        "cached_input_tokens": 2,
                        "output_tokens": 1,
                    },
                }},
            ]
            rollout.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
            report = ai_run.token_usage_report([rollout])
        self.assertIsNone(report["by_model"][0]["model"])
        self.assertEqual(report["by_model"][0]["availability"], "unavailable")
        self.assertIsNone(report["by_model"][0]["usage"]["input_tokens"])

    def test_token_count_events_attribute_cumulative_deltas_to_turn_context_models(self):
        with tempfile.TemporaryDirectory() as directory:
            rollout = Path(directory) / "real-shaped.jsonl"
            events = [
                {"type": "turn_context", "payload": {"model": "gpt-5.6-terra"}},
                {"type": "event_msg", "payload": {"type": "token_count", "info": {
                    "total_token_usage": {
                        "input_tokens": 10,
                        "cached_input_tokens": 4,
                        "output_tokens": 2,
                    },
                }}},
                {"type": "turn_context", "payload": {"model": "gpt-6-astra"}},
                {"type": "event_msg", "payload": {"type": "token_count", "info": {
                    "total_token_usage": {
                        "input_tokens": 16,
                        "cached_input_tokens": 7,
                        "output_tokens": 5,
                    },
                }}},
            ]
            rollout.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
            report = ai_run.token_usage_report([rollout])
        usage = {entry["model"]: entry["usage"] for entry in report["by_model"]}
        self.assertEqual(usage["gpt-5.6-terra"], {
            "input_tokens": 10,
            "cached_input_tokens": 4,
            "output_tokens": 2,
        })
        self.assertEqual(usage["gpt-6-astra"], {
            "input_tokens": 6,
            "cached_input_tokens": 3,
            "output_tokens": 3,
        })
        self.assertEqual(report["totals"], {
            "input_tokens": 16,
            "cached_input_tokens": 7,
            "output_tokens": 5,
        })

    def test_token_count_nonmonotonic_cumulative_usage_disables_model_attribution(self):
        with tempfile.TemporaryDirectory() as directory:
            rollout = Path(directory) / "nonmonotonic.jsonl"
            events = [
                {"type": "turn_context", "payload": {"model": "gpt-5.6-terra"}},
                {"type": "event_msg", "payload": {"type": "token_count", "info": {
                    "total_token_usage": {"input_tokens": 10, "cached_input_tokens": 4, "output_tokens": 2},
                }}},
                {"type": "turn_context", "payload": {"model": "gpt-6-astra"}},
                {"type": "event_msg", "payload": {"type": "token_count", "info": {
                    "total_token_usage": {"input_tokens": 9, "cached_input_tokens": 3, "output_tokens": 2},
                }}},
            ]
            rollout.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
            report = ai_run.token_usage_report([rollout])
        self.assertEqual(report["rollouts"][0]["availability"], "available")
        self.assertEqual(report["rollouts"][0]["attribution"]["availability"], "unavailable")
        self.assertEqual(report["totals"]["input_tokens"], 9)
        self.assertEqual(report["coverage"]["unavailable_model_attribution_rollouts"], 1)

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
