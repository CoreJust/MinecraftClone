import importlib.util
import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "script/ai_check.py"
PRE_PUSH = REPOSITORY / ".githooks/pre-push"


def load_module():
    spec = importlib.util.spec_from_file_location("ai_check", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class AiCheckTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        for relative, content in {
            "script/ai_docs.py": "raise SystemExit(0)\n",
            "script/ai_tasks.py": "raise SystemExit(0)\n",
            "script/ai_plan.py": "raise SystemExit(0)\n",
            "script/tests/test_smoke.py": "import unittest\nclass Smoke(unittest.TestCase):\n    def test_ok(self): self.assertTrue(True)\n",
            "src/shared/include/shared/ProjectInfo.hpp": (
                'constexpr std::string_view MAJOR_VERSION_NAME{ "Test" };\n'
                'constexpr std::string_view MINOR_VERSION_NAME{ "Run" };\n'
                "constexpr core::Version PROJECT_VERSION{ .epoch = 1, .major = 2, .minor = 3, .patch = 4, };\n"
            ),
        }.items():
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding="utf-8")
        self.git("init")
        self.git("config", "user.email", "ai-check@example.invalid")
        self.git("config", "user.name", "AI Check")
        self.git("add", "script", "src")
        self.git("commit", "--no-gpg-sign", "-m", "fixture")

    def tearDown(self):
        self.temp_dir.cleanup()

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.root, text=True, capture_output=True, check=True)

    def run_check(self, *args, environment=None):
        return subprocess.run(
            [sys.executable, SCRIPT, "--root", self.root, *args],
            text=True,
            capture_output=True,
            env=environment,
        )

    def test_fast_runs_each_required_phase(self):
        result = self.run_check("--fast")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for phase in ("docs", "backlog", "current-plan", "python-tests", "diff-working", "diff-cached"):
            self.assertIn(f"PASS {phase}", result.stdout)
            self.assertTrue((self.root / "build/ai-checks" / f"{phase}.log").is_file())

    def test_failed_phase_does_not_skip_later_phases(self):
        (self.root / "script/ai_docs.py").write_text("raise SystemExit(7)\n", encoding="utf-8")
        result = self.run_check("--fast")
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL docs: exit 7", result.stdout)
        self.assertIn("PASS backlog", result.stdout)
        self.assertIn("PASS python-tests", result.stdout)

    def test_python_tests_use_verbose_diagnostics_and_platform_budget(self):
        checker = load_module()
        calls = []

        def run_phase(root, log_dir, name, command, timeout, **kwargs):
            calls.append((name, command, timeout))
            return checker.PhaseResult(name, command, 0, "")

        with mock.patch.object(checker, "run_phase", side_effect=run_phase), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(checker.main(["--root", str(self.root), "--fast"]), 0)
        python_tests = next(item for item in calls if item[0] == "python-tests")
        self.assertEqual(python_tests[1][-1], "-v")
        self.assertEqual(python_tests[2], 180 if os.name == "nt" else 120)

    def test_fast_rejects_partially_staged_governed_file(self):
        target = self.root / "src/changed.cpp"
        target.write_text("first\n", encoding="utf-8")
        self.git("add", "src/changed.cpp")
        target.write_text("second\n", encoding="utf-8")
        result = self.run_check("--fast", "--require-index-match")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Partially staged governed files: src/changed.cpp", result.stdout)

    def test_index_check_ignores_unstaged_non_governed_files(self):
        note = self.root / "notes.txt"
        note.write_text("base\n", encoding="utf-8")
        self.git("add", "notes.txt")
        self.git("commit", "--no-gpg-sign", "-m", "base note")
        governed = self.root / "src/changed.cpp"
        governed.write_text("staged\n", encoding="utf-8")
        self.git("add", "src/changed.cpp")
        note.write_text("unstaged\n", encoding="utf-8")
        result = self.run_check("--fast", "--require-index-match")
        self.assertEqual(result.returncode, 1)
        self.assertIn("CMakePresets.json", result.stdout)

    def test_index_check_rejects_untracked_governed_file_when_governed_content_is_staged(self):
        staged_doc = self.root / "docs/state.md"
        staged_doc.parent.mkdir(parents=True, exist_ok=True)
        staged_doc.write_text("staged\n", encoding="utf-8")
        self.git("add", "docs/state.md")
        untracked_source = self.root / "src/untracked.cpp"
        untracked_source.write_text("untracked\n", encoding="utf-8")
        result = self.run_check("--fast", "--require-index-match")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Partially staged governed files: src/untracked.cpp", result.stdout)

    def test_missing_program_is_a_recorded_phase_failure(self):
        ai_check = load_module()
        log_dir = self.root / "build/ai-checks"
        log_dir.mkdir(parents=True)
        result = ai_check.run_phase(self.root, log_dir, "missing", ["definitely-not-a-command"], 1)
        self.assertEqual(result.returncode, 127)
        self.assertTrue((log_dir / "missing.log").is_file())

    def test_python_test_environment_rejects_failed_or_malformed_git_discovery(self):
        checker = load_module()
        with mock.patch.object(checker, "command_output", side_effect=RuntimeError("git discovery failed")):
            with self.assertRaises(RuntimeError):
                checker.python_test_environment(self.root)
        for output in ("", "GIT_DIR\nNOT_GIT\n"):
            with self.subTest(output=output), mock.patch.object(checker, "command_output", return_value=output):
                with self.assertRaises(RuntimeError):
                    checker.python_test_environment(self.root)

    def test_python_tests_clear_injected_hook_repository_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            outer = Path(directory)

            def outer_git(*args):
                return subprocess.run(
                    ["git", *args], cwd=outer, text=True, capture_output=True, check=True
                )

            outer_git("init", "-q")
            outer_git("config", "user.email", "outer@example.invalid")
            outer_git("config", "user.name", "Outer Repository")
            outer_git("config", "core.hooksPath", "outer-hooks")
            (outer / "outer.txt").write_text("outer\n", encoding="utf-8")
            outer_git("add", "outer.txt")
            outer_git("commit", "--no-gpg-sign", "-q", "-m", "outer fixture")
            outer_git("config", "core.bare", "false")
            outer_git("config", "user.email", "outer@example.invalid")
            (outer / "outer.txt").write_text("outer  \n", encoding="utf-8")

            outer_git_dir = outer / ".git"
            before = {
                "config": (outer_git_dir / "config").read_bytes(),
                "head": outer_git("rev-parse", "HEAD").stdout,
                "index": (outer_git_dir / "index").read_bytes(),
                "refs": outer_git("show-ref", "--head").stdout,
                "status": outer_git("status", "--porcelain=v1").stdout,
            }
            marker = self.root / "isolated-fixture-commit"
            test_file = self.root / "script/tests/test_git_fixture.py"
            test_file.write_text(
                "import os\n"
                "from pathlib import Path\n"
                "import subprocess\n"
                "import tempfile\n"
                "import unittest\n"
                "\n"
                "class GitFixture(unittest.TestCase):\n"
                "    def test_creates_its_own_repository(self):\n"
                "        for name in ('GIT_DIR', 'GIT_COMMON_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE'):\n"
                "            self.assertNotIn(name, os.environ)\n"
                "        with tempfile.TemporaryDirectory() as directory:\n"
                "            root = Path(directory)\n"
                "            def git(*args):\n"
                "                return subprocess.run(['git', *args], cwd=root, text=True, capture_output=True, check=True)\n"
                "            git('init', '-q')\n"
                "            git('config', 'user.email', 'fixture@example.invalid')\n"
                "            git('config', 'user.name', 'Fixture Repository')\n"
                "            (root / 'fixture.txt').write_text('fixture\\n', encoding='utf-8')\n"
                "            git('add', 'fixture.txt')\n"
                "            git('commit', '--no-gpg-sign', '-q', '-m', 'fixture commit')\n"
                "            Path(os.environ['ISOLATION_MARKER']).write_text(git('rev-parse', 'HEAD').stdout, encoding='utf-8')\n",
                encoding="utf-8",
            )
            environment = os.environ | {
                "GIT_DIR": str(outer_git_dir),
                "GIT_COMMON_DIR": str(outer_git_dir),
                "GIT_WORK_TREE": str(outer),
                "GIT_INDEX_FILE": str(outer_git_dir / "index"),
                "ISOLATION_MARKER": str(marker),
            }
            result = self.run_check("--fast", environment=environment)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("PASS python-tests", result.stdout)
            self.assertIn("FAIL diff-working", result.stdout)
            self.assertRegex(marker.read_text(encoding="utf-8").strip(), r"^[0-9a-f]{40}$")
            after = {
                "config": (outer_git_dir / "config").read_bytes(),
                "head": outer_git("rev-parse", "HEAD").stdout,
                "index": (outer_git_dir / "index").read_bytes(),
                "refs": outer_git("show-ref", "--head").stdout,
                "status": outer_git("status", "--porcelain=v1").stdout,
            }
            self.assertEqual(after, before)

    def test_annotated_ai_tag_push_uses_strict_check_from_legacy_branch(self):
        fake_bin = self.root / "fake-bin"
        fake_bin.mkdir()
        fake_git = fake_bin / "git"
        fake_git.write_text(
            "#!/bin/sh\n"
            "printf 'git:%s:%s\\n' \"$1\" \"$2\" >> \"$FAKE_GIT_LOG\"\n"
            "case \"$1 $2\" in\n"
            "  'branch --show-current') echo master ;;\n"
            "  'rev-parse HEAD') echo checked-commit ;;\n"
            "  'rev-parse tag-object^{commit}') echo checked-commit ;;\n"
            "  'status --porcelain') ;;\n"
            "  *) exit 2 ;;\n"
            "esac\n",
            encoding="utf-8",
            newline="\n",
        )
        fake_python = fake_bin / "python"
        fake_python.write_text(
            "#!/bin/sh\nprintf '%s\\n' \"$*\" > \"$HOOK_LOG\"\n",
            encoding="utf-8",
            newline="\n",
        )
        self.assertNotIn(b"\r", fake_git.read_bytes())
        self.assertNotIn(b"\r", fake_python.read_bytes())
        fake_git.chmod(0o755)
        fake_python.chmod(0o755)
        log = self.root / "hook.log"
        fake_git_log = self.root / "fake-git.log"
        environment = os.environ | {
            "PYTHON": str(fake_python),
            "HOOK_LOG": str(log),
            "FAKE_GIT_LOG": fake_git_log.name,
        }
        shell = shutil.which("sh")
        self.assertIsNotNone(shell)
        shell_path = 'PATH="$PWD/fake-bin:$PATH"; export PATH;'
        shell_version = subprocess.run(
            [shell, "--version"],
            cwd=self.root,
            text=True,
            capture_output=True,
            check=False,
            env=environment,
        )
        git_probe = subprocess.run(
            [shell, "-c", f'{shell_path} command -v git; git --version'],
            cwd=self.root,
            text=True,
            capture_output=True,
            check=False,
            env=environment,
        )
        result = subprocess.run(
            [shell, "-c", f'{shell_path} exec "$1"', "pre-push-fixture", PRE_PUSH],
            cwd=self.root,
            input="refs/tags/ai/EarlyDev/0.1 tag-object refs/tags/ai/EarlyDev/0.1 0000000000000000000000000000000000000000\n",
            text=True,
            capture_output=True,
            env=environment,
        )
        fake_git_log_contents = (
            fake_git_log.read_text(encoding="utf-8") if fake_git_log.exists() else "<missing>"
        )
        diagnostics = (
            f"shell={shell!r}\n"
            f"shell_version_rc={shell_version.returncode}\n"
            f"shell_version_stdout={shell_version.stdout!r}\n"
            f"shell_version_stderr={shell_version.stderr!r}\n"
            f"fake_git_mode={oct(fake_git.stat().st_mode)}\n"
            f"fake_git_executable={os.access(fake_git, os.X_OK)}\n"
            f"git_probe_rc={git_probe.returncode}\n"
            f"git_probe_stdout={git_probe.stdout!r}\n"
            f"git_probe_stderr={git_probe.stderr!r}\n"
            f"fake_git_log={fake_git_log_contents!r}\n"
            f"hook_stdout={result.stdout!r}\n"
            f"hook_stderr={result.stderr!r}"
        )
        self.assertEqual(result.returncode, 0, diagnostics)
        self.assertEqual(
            fake_git_log_contents.splitlines(),
            [
                "git:--version:",
                "git:branch:--show-current",
                "git:rev-parse:HEAD",
                "git:rev-parse:tag-object^{commit}",
                "git:status:--porcelain",
            ],
        )
        self.assertEqual(log.read_text(encoding="utf-8").strip(), "script/ai_check.py --strict --require-index-match")

    def test_version_arguments_and_publisher_exceptions_are_precise(self):
        ai_check = load_module()
        self.assertEqual(ai_check.project_version_arguments(self.root), ("Test:Run", "1.2.3:4"))
        dirty = "[1/2] no unstaged or uncommitted changes ... FAIL bad\n[2/2] latest snapshot date is today ... FAIL bad\n"
        self.assertTrue(ai_check.publisher_failure_is_allowed(dirty, strict=False))
        self.assertFalse(ai_check.publisher_failure_is_allowed(dirty, strict=True))
        self.assertFalse(ai_check.publisher_failure_is_allowed("[1/1] README.md contains version string ... FAIL bad\n", strict=False))
        self.assertFalse(ai_check.publisher_failure_is_allowed("Traceback\n" + dirty, strict=False))
        self.assertFalse(ai_check.publisher_failure_is_allowed(dirty + "unexpected failure\n", strict=False))

    def test_candidate_allows_only_dirty_tree_not_snapshot_failures(self):
        checker = load_module()
        dirty = "[1/1] no unstaged or uncommitted changes ... FAIL dirty\n"
        snapshot = "[1/1] latest snapshot date is today ... FAIL date\n"
        self.assertTrue(checker.publisher_failure_is_allowed(dirty, strict=True, candidate=True))
        self.assertFalse(checker.publisher_failure_is_allowed(snapshot, strict=False, candidate=True))
        self.assertFalse(checker.publisher_failure_is_allowed(dirty, strict=True))
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            checker.main(["--fast", "--candidate"])

    def test_release_registry_runs_enabled_checks_and_rejects_invalid_config(self):
        checker = load_module()
        config_path = self.root / "script/ai_checks.json"
        config = [{"name": "sample", "enabled": True, "levels": ["snapshot"],
                   "command": ["{python}", "check.py"], "timeout": 7, "reason": ""}]
        config_path.write_text(json.dumps(config))
        commands = []
        reuse_flags = []
        def run_phase(root, log_dir, name, command, timeout, **kwargs):
            commands.append((name, command, timeout))
            reuse_flags.append(kwargs.get("reuse", False))
            return checker.PhaseResult(name, command, 0, "")
        with mock.patch.object(checker, "run_phase", side_effect=run_phase), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(checker.main(["--root", str(self.root), "--candidate", "--level", "snapshot"]), 0)
        self.assertIn(("extra-sample", [sys.executable, "check.py"], 7), commands)
        self.assertFalse(any(reuse_flags))
        config[0]["command"] = []
        config_path.write_text(json.dumps(config))
        with mock.patch.object(checker, "run_phase", side_effect=run_phase), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(checker.main(["--root", str(self.root), "--candidate", "--level", "snapshot"]), 1)

    def test_metadata_only_scope_skips_python_tests_and_writes_summary(self):
        metadata = self.root / "docs/ai/notes.md"
        metadata.parent.mkdir(parents=True, exist_ok=True)
        metadata.write_text("metadata\n", encoding="utf-8")
        result = self.run_check("--fast")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("CHECK SCOPE metadata-only", result.stdout)
        self.assertIn("SKIP python-tests", result.stdout)
        summary = json.loads((self.root / "build/ai-checks/summary.json").read_text(encoding="utf-8"))
        self.assertIn("python-tests", summary["skipped_phases"])

    def test_matching_fast_receipts_are_reused_and_summarized(self):
        first = self.run_check("--fast")
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        second = self.run_check("--fast")
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertIn("REUSED PASS docs", second.stdout)
        summary = json.loads((self.root / "build/ai-checks/summary.json").read_text(encoding="utf-8"))
        self.assertIn("docs", summary["reused_phases"])
        self.assertTrue(summary["durations_seconds"]["docs"] >= 0)

    def test_python_tests_execute_after_non_script_input_changes(self):
        source = self.root / "src/source_policy.cpp"
        source.write_text("int value = 1;\n", encoding="utf-8")
        test_file = self.root / "script/tests/test_source_policy.py"
        test_file.write_text(
            "from pathlib import Path\n"
            "import unittest\n"
            "\n"
            "class SourcePolicy(unittest.TestCase):\n"
            "    def test_source_lines_are_bounded(self):\n"
            "        source = Path(__file__).parents[2] / 'src/source_policy.cpp'\n"
            "        self.assertTrue(all(len(line) <= 80 for line in source.read_text().splitlines()))\n",
            encoding="utf-8",
        )
        self.git("add", "src/source_policy.cpp", "script/tests/test_source_policy.py")
        self.git("commit", "--no-gpg-sign", "-m", "source policy fixture")

        first = self.run_check("--fast")
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        source.write_text("x" * 81 + "\n", encoding="utf-8")

        second = self.run_check("--fast")
        self.assertEqual(second.returncode, 1, second.stdout + second.stderr)
        self.assertIn("FAIL python-tests", second.stdout)
        self.assertNotIn("REUSED PASS python-tests", second.stdout)
        summary = json.loads((self.root / "build/ai-checks/summary.json").read_text(encoding="utf-8"))
        self.assertIn("python-tests", summary["executed_phases"])
        self.assertNotIn("python-tests", summary["reused_phases"])

    def test_matching_receipt_keeps_hook_command_to_one_execution(self):
        marker = self.root / "build/ai-checks/docs-command-count"
        marker.parent.mkdir(parents=True, exist_ok=True)
        (self.root / "script/ai_docs.py").write_text(
            "import os\n"
            "from pathlib import Path\n"
            "marker = Path(os.environ['AI_CHECK_MARKER'])\n"
            "value = int(marker.read_text()) if marker.exists() else 0\n"
            "marker.write_text(str(value + 1))\n",
            encoding="utf-8",
        )
        environment = os.environ | {"AI_CHECK_MARKER": str(marker)}
        first = self.run_check("--fast", environment=environment)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        second = self.run_check("--fast", environment=environment)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(marker.read_text(encoding="utf-8"), "1")
        self.assertIn("REUSED PASS docs", second.stdout)

    def test_receipt_environment_mismatch_forces_execution(self):
        first_environment = os.environ | {"AI_CHECK_RECEIPT_ENV": "one"}
        second_environment = os.environ | {"AI_CHECK_RECEIPT_ENV": "two"}
        first = self.run_check("--fast", environment=first_environment)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        second = self.run_check("--fast", environment=second_environment)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertNotIn("REUSED PASS docs", second.stdout)

    def test_hook_only_git_environment_does_not_invalidate_manual_receipt(self):
        first_environment = os.environ.copy()
        for name in ("GIT_DIR", "GIT_COMMON_DIR", "GIT_INDEX_FILE", "GIT_PREFIX", "GIT_WORK_TREE"):
            first_environment.pop(name, None)
        second_environment = first_environment | {
            "GIT_DIR": str(self.root / ".git"),
            "GIT_COMMON_DIR": str(self.root / ".git"),
            "GIT_INDEX_FILE": str(self.root / ".git/index"),
            "GIT_PREFIX": "",
            "GIT_WORK_TREE": str(self.root),
            "GIT_EXEC_PATH": subprocess.run(
                ["git", "--exec-path"], text=True, capture_output=True, check=True
            ).stdout.strip(),
            "GIT_AUTHOR_DATE": "@0 +0000",
            "GIT_AUTHOR_EMAIL": "hook@example.invalid",
            "GIT_AUTHOR_NAME": "Hook",
            "GIT_EDITOR": ":",
        }
        first = self.run_check("--fast", environment=first_environment)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        second = self.run_check("--fast", environment=second_environment)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertIn("REUSED PASS docs", second.stdout)

    def test_explicit_git_exec_path_mismatch_invalidates_receipt(self):
        first = self.run_check("--fast")
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        environment = os.environ | {"GIT_EXEC_PATH": "/explicitly-different-git-core"}
        second = self.run_check("--fast", environment=environment)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertNotIn("REUSED PASS docs", second.stdout)

    def test_failed_receipt_is_never_reused(self):
        first = self.run_check("--fast")
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        receipt = self.root / "build/ai-checks/docs.receipt.json"
        payload = json.loads(receipt.read_text(encoding="utf-8"))
        payload["status"] = "FAIL"
        receipt.write_text(json.dumps(payload), encoding="utf-8")
        second = self.run_check("--fast")
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertNotIn("REUSED PASS docs", second.stdout)

    def test_cpp_changes_use_existing_build_and_ctest_fallback(self):
        checker = load_module()
        (self.root / "CMakePresets.json").write_text("{}\n", encoding="utf-8")
        (self.root / "src/fixture.cpp").write_text("changed\n", encoding="utf-8")
        calls = []

        def run_phase(root, log_dir, name, command, timeout, **kwargs):
            calls.append(name)
            return checker.PhaseResult(name, command, 0, "")

        with mock.patch.object(checker, "run_phase", side_effect=run_phase), contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(checker.main(["--root", str(self.root), "--fast"]), 0)
        self.assertIn("full-cpp", output.getvalue())
        self.assertIn("build", calls)
        self.assertIn("ctest", calls)

    def test_exact_python_mapping_targets_existing_test_and_unknown_falls_back(self):
        test_file = self.root / "script/tests/test_ai_tasks.py"
        test_file.write_text(
            "import unittest\n"
            "class Tasks(unittest.TestCase):\n"
            "    def test_ok(self): self.assertTrue(True)\n",
            encoding="utf-8",
        )
        self.git("add", "script/tests/test_ai_tasks.py")
        self.git("commit", "--no-gpg-sign", "-m", "fixture test")
        (self.root / "script/ai_tasks.py").write_text("# changed\nraise SystemExit(0)\n", encoding="utf-8")
        result = self.run_check("--fast")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("CHECK SCOPE full-python: script/ai_tasks.py", result.stdout)
        self.assertNotIn("targeted-python", result.stdout)

    def test_leaf_python_mapping_targets_proven_related_test(self):
        source = self.root / "script/check_options.py"
        test_file = self.root / "script/tests/test_check_options.py"
        source.write_text("VALUE = 1\n", encoding="utf-8")
        test_file.write_text(
            "import unittest\n"
            "class Options(unittest.TestCase):\n"
            "    def test_ok(self): self.assertTrue(True)\n",
            encoding="utf-8",
        )
        self.git("add", "script/check_options.py", "script/tests/test_check_options.py")
        self.git("commit", "--no-gpg-sign", "-m", "fixture leaf")
        source.write_text("VALUE = 2\n", encoding="utf-8")
        result = self.run_check("--fast")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("CHECK SCOPE targeted-python:script/tests/test_check_options.py", result.stdout)


if __name__ == "__main__":
    unittest.main()
