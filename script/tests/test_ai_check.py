import importlib.util
import contextlib
import hashlib
import io
import json
import ntpath
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "script/ai_check.py"
PRE_COMMIT = REPOSITORY / ".githooks/pre-commit"
PRE_MERGE = REPOSITORY / ".githooks/pre-merge-commit"
PRE_PUSH = REPOSITORY / ".githooks/pre-push"


def pre_push_command():
    if os.name != "nt":
        return [str(PRE_PUSH)]
    shell = shutil.which("sh")
    if shell is None:
        raise RuntimeError("POSIX shell 'sh' is required to execute the pre-push fixture on Windows")
    return [shell, "-c", 'exec "$1"', "pre-push-fixture", str(PRE_PUSH)]


def load_module():
    spec = importlib.util.spec_from_file_location("ai_check", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class AiCheckTests(unittest.TestCase):
    def test_s7_selector_requires_actual_version_owners(self):
        check = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = root / "src/shared/include/shared/ProjectInfo.hpp"
            project.parent.mkdir(parents=True)
            content = (SCRIPT.parent.parent / "src/shared/include/shared/ProjectInfo.hpp").read_text()
            project.write_text(content)
            (root / "vcpkg.json").write_text('{"version-string":"0.1.0"}')
            self.assertIsNone(check.require_s7_candidate(root))
            for wrong in (content.replace(".patch = 7", ".patch = 8"),
                          content.replace(".minor = 0", ".minor = 1"),
                          content.replace('"EarlyDev"', '"Released"'), "malformed"):
                project.write_text(wrong)
                with self.assertRaisesRegex(ValueError, "actual candidate version"):
                    check.require_s7_candidate(root)
            project.write_text(content)
            (root / "vcpkg.json").write_text('{"version-string":"0.2.0"}')
            with self.assertRaisesRegex(ValueError, "actual candidate version"):
                check.require_s7_candidate(root)
            project.unlink()
            with self.assertRaisesRegex(ValueError, "actual candidate version"):
                check.require_s7_candidate(root)

    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        for relative, content in {
            "script/ai_docs.py": "raise SystemExit(0)\n",
            "script/ai_tasks.py": "raise SystemExit(0)\n",
            "script/ai_plan.py": "raise SystemExit(0)\n",
            "script/ai_release_reports.py": "raise SystemExit(0)\n",
            "publish.py": (
                "import os\n"
                "if os.environ.get('AI_CHECK_SOURCE_FAILURE'):\n"
                "    print('[1/1] source files (.cpp/.hpp/.mesh etc.) mentioned exactly once ... FAIL bad source')\n"
                "    raise SystemExit(1)\n"
                "if os.environ.get('AI_CHECK_SOURCE_ORDINARY_FAILURE'):\n"
                "    print('[1/2] no unstaged or uncommitted changes ... FAIL dirty')\n"
                "    print('[2/2] latest snapshot date is today ... FAIL date')\n"
                "    raise SystemExit(1)\n"
            ),
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
        self.git("add", "publish.py", "script", "src")
        self.git("commit", "--no-gpg-sign", "-m", "fixture")

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_fast_gate_rejects_invalid_release_report_state(self):
        (self.root / "script/ai_release_reports.py").write_text(
            "print('retained report hash mismatch')\nraise SystemExit(1)\n",
            encoding="utf-8",
        )
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--root", str(self.root), "--fast"],
            text=True, capture_output=True,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL release-reports", result.stdout)
        self.assertIn("retained report hash mismatch", result.stdout)

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.root, text=True, capture_output=True, check=True)

    def assert_hook_path_retains_caller(self, hook_path, caller_path):
        if os.name == "nt":
            # MSYS removes empty entries and normalizes native directory spelling.
            caller_components = [
                ntpath.normcase(ntpath.normpath(component))
                for component in caller_path.split(os.pathsep) if component
            ]
            hook_components = [
                ntpath.normcase(ntpath.normpath(component))
                for component in hook_path.split(os.pathsep) if component
            ]
            self.assertGreater(len(hook_components), len(caller_components))
            self.assertEqual(hook_components[-len(caller_components):], caller_components)
        else:
            self.assertEqual(hook_path, caller_path)

    def configure_native_build_fixture(self, preset="debug"):
        build_root = self.root / "build" / preset
        dependency_prefix = self.root / "build/test-dependencies"
        (build_root / "tests").mkdir(parents=True, exist_ok=True)
        (dependency_prefix / "lib").mkdir(parents=True, exist_ok=True)
        (build_root / "CMakeCache.txt").write_text(
            "\n".join([
                f"CMAKE_C_COMPILER:FILEPATH={sys.executable}",
                f"CMAKE_CXX_COMPILER:FILEPATH={sys.executable}",
                f"CMAKE_MAKE_PROGRAM:FILEPATH={sys.executable}",
                f"CMAKE_TOOLCHAIN_FILE:FILEPATH={sys.executable}",
                f"CMAKE_PREFIX_PATH:PATH={dependency_prefix}",
                f"VCPKG_INSTALLED_DIR:PATH={dependency_prefix}",
                f"VCPKG_MANIFEST_DIR:PATH={self.root}",
                f"MinecraftClone2026_BINARY_DIR:STATIC={build_root}",
                f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root}",
                "CMAKE_GENERATOR:INTERNAL=Ninja",
                "Fixture_DIR:PATH=" + str(dependency_prefix / "lib"),
                "",
            ]),
            encoding="utf-8",
        )
        (build_root / "CMakeFiles").mkdir()
        (build_root / "CMakeFiles/rules.ninja").write_text("rule compile\n", encoding="utf-8")
        (build_root / "build.ninja").write_text(
            "include CMakeFiles/rules.ninja\nbuild mc_main: phony\n", encoding="utf-8"
        )
        (build_root / "mc_main").write_bytes(b"debug game executable")
        (build_root / "tests/mc_tests").write_bytes(b"debug test executable")
        dependency = dependency_prefix / "lib/libfixture.a"
        dependency.write_bytes(b"installed dependency")
        return build_root, dependency

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
        for phase in ("docs", "backlog", "current-plan", "release-reports", "python-tests", "diff-working", "diff-cached"):
            self.assertIn(f"PASS {phase}", result.stdout)
            self.assertTrue((self.root / "build/ai-checks" / f"{phase}.log").is_file())

    def test_candidate_snapshot_source_must_match_source_head_or_pending_merge(self):
        checker = load_module()
        self.git("branch", "-M", "ai-dev")
        promotion_base = self.git("rev-parse", "HEAD").stdout.strip()
        self.git("commit", "--allow-empty", "--no-gpg-sign", "-m", "snapshot source")
        source = self.git("rev-parse", "HEAD").stdout.strip()

        self.assertEqual(
            checker.snapshot_source_for_check(
                self.root, source, candidate=True, strict=False
            ),
            source,
        )
        with self.assertRaisesRegex(ValueError, "does not match"):
            checker.snapshot_source_for_check(
                self.root, promotion_base, candidate=True, strict=False
            )
        self.git("checkout", "-b", "ai-main", promotion_base)
        self.git("merge", "--no-ff", "--no-commit", source)
        self.assertEqual(
            checker.snapshot_source_for_check(
                self.root, source, candidate=True, strict=False
            ),
            source,
        )
        with self.assertRaisesRegex(ValueError, "does not match"):
            checker.snapshot_source_for_check(
                self.root, promotion_base, candidate=True, strict=False
            )

    def test_candidate_snapshot_source_accepts_codex_task_branch(self):
        checker = load_module()
        source_base = self.git("rev-parse", "HEAD").stdout.strip()
        self.git("switch", "-c", "codex/ai-MC-AI-0391-release-candidate")
        self.git("commit", "--allow-empty", "--no-gpg-sign", "-m", "candidate source")
        source = self.git("rev-parse", "HEAD").stdout.strip()

        self.assertEqual(
            checker.snapshot_source_for_check(
                self.root, source, candidate=True, strict=False
            ),
            source,
        )
        with self.assertRaisesRegex(ValueError, "does not match"):
            checker.snapshot_source_for_check(
                self.root, source_base, candidate=True, strict=False
            )

    def test_candidate_snapshot_source_rejects_abbreviated_or_malformed_ids(self):
        checker = load_module()
        self.git("switch", "-c", "codex/ai-MC-AI-0391-release-candidate")

        for source in ("abc123", "not-a-commit-id"):
            with self.subTest(source=source):
                with self.assertRaisesRegex(ValueError, "full immutable commit ID"):
                    checker.snapshot_source_for_check(
                        self.root, source, candidate=True, strict=False
                    )

    def test_candidate_snapshot_source_rejects_unrelated_branch(self):
        checker = load_module()
        self.git("switch", "-c", "feature/unrelated")
        source = self.git("rev-parse", "HEAD").stdout.strip()

        with self.assertRaisesRegex(ValueError, "does not match"):
            checker.snapshot_source_for_check(
                self.root, source, candidate=True, strict=False
            )

    def test_candidate_snapshot_source_rejects_pending_merge_on_codex_branch(self):
        checker = load_module()
        base = self.git("rev-parse", "HEAD").stdout.strip()
        self.git("switch", "-c", "codex/ai-MC-AI-0391-release-candidate")
        self.git("commit", "--allow-empty", "--no-gpg-sign", "-m", "candidate source")
        source = self.git("rev-parse", "HEAD").stdout.strip()
        self.git("switch", "-c", "candidate-side", base)
        self.git("commit", "--allow-empty", "--no-gpg-sign", "-m", "side source")
        self.git("switch", "codex/ai-MC-AI-0391-release-candidate")
        self.git("merge", "--no-ff", "--no-commit", "candidate-side")

        with self.assertRaisesRegex(ValueError, "does not match"):
            checker.snapshot_source_for_check(
                self.root, source, candidate=True, strict=False
            )

    def test_candidate_without_snapshot_source_keeps_today_semantics(self):
        checker = load_module()
        self.git("branch", "-M", "ai-dev")
        (self.root / "unstaged-note.txt").write_text("dirty aggregate candidate\n", encoding="utf-8")

        self.assertIsNone(
            checker.snapshot_source_for_check(
                self.root, None, candidate=True, strict=False
            )
        )

    def test_candidate_publisher_receives_the_validated_source_binding(self):
        checker = load_module()
        self.git("branch", "-M", "ai-dev")
        source = self.git("rev-parse", "HEAD").stdout.strip()
        (self.root / "script/ai_checks.json").write_text("[]\n", encoding="utf-8")
        commands = []

        def run_phase(root, log_dir, name, command, timeout, **kwargs):
            commands.append((name, command))
            return checker.PhaseResult(name, command, 0, "")

        with mock.patch.object(checker, "run_phase", side_effect=run_phase), contextlib.redirect_stdout(io.StringIO()):
            result = checker.main([
                "--root", str(self.root),
                "--candidate",
                "--level", "snapshot",
                "--snapshot-source-commit", source,
            ])

        self.assertEqual(result, 0)
        publisher = next(command for name, command in commands if name == "publisher")
        self.assertEqual(
            publisher[-2:],
            ["--snapshot-source-commit", source],
        )

    def test_strict_publisher_receives_the_reconstructed_promotion_source(self):
        checker = load_module()
        source = self.git("rev-parse", "HEAD").stdout.strip()
        (self.root / "script/ai_checks.json").write_text("[]\n", encoding="utf-8")
        commands = []

        def run_phase(root, log_dir, name, command, timeout, **kwargs):
            commands.append((name, command))
            return checker.PhaseResult(name, command, 0, "")

        with (
            mock.patch.object(checker, "run_phase", side_effect=run_phase),
            mock.patch.object(checker, "snapshot_source_for_check", return_value=source),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            result = checker.main(["--root", str(self.root), "--strict", "--level", "snapshot"])

        self.assertEqual(result, 0)
        publisher = next(command for name, command in commands if name == "publisher")
        self.assertEqual(publisher[-2:], ["--snapshot-source-commit", source])

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

        with (
            mock.patch.object(checker, "run_phase", side_effect=run_phase),
            mock.patch.object(checker.platform, "system", return_value="Darwin"),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(checker.main(["--root", str(self.root), "--fast"]), 0)
        python_tests = next(item for item in calls if item[0] == "python-tests")
        self.assertEqual(python_tests[1][-1], "-v")
        self.assertEqual(python_tests[2], 300)

    def test_python_tests_use_extended_windows_budget(self):
        checker = load_module()
        calls = []

        def run_phase(root, log_dir, name, command, timeout, **kwargs):
            calls.append((name, command, timeout))
            return checker.PhaseResult(name, command, 0, "")

        with (
            mock.patch.object(checker, "run_phase", side_effect=run_phase),
            mock.patch.object(checker.platform, "system", return_value="Windows"),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(checker.main(["--root", str(self.root), "--fast"]), 0)
        python_tests = next(item for item in calls if item[0] == "python-tests")
        self.assertEqual(python_tests[2], 420)

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
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("CMakePresets.json", result.stdout)

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

    def test_failed_phase_prints_complete_diagnostics(self):
        ai_check = load_module()
        diagnostics = "traceback-start\n" + ("diagnostic line\n" * 1_500) + "traceback-end"
        result = ai_check.PhaseResult(
            "python-tests", ["python", "-m", "unittest"], 1, diagnostics, duration_seconds=2.5
        )
        with contextlib.redirect_stdout(io.StringIO()) as output:
            ai_check.print_result(result)
        self.assertIn("traceback-start", output.getvalue())
        self.assertIn("traceback-end", output.getvalue())
        self.assertIn("after 2.50s", output.getvalue())
        self.assertEqual(output.getvalue().count("diagnostic line"), 1_500)

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
                "        for name in ('GIT_DIR', 'GIT_COMMON_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'AI_TASK'):\n"
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
                "AI_TASK": "MC-AI-0122",
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
            "  '--exec-path ') echo \"$GIT_EXEC_PATH\" ;;\n"
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
            "#!/bin/sh\n"
            "mkdir -p \"$(dirname \"$HOOK_PATH_LOG\")\"\n"
            "printf '%s\\n' \"$*\" > \"$HOOK_LOG\"\n"
            "case \"$PATH\" in\n"
            "  \"$HOOK_PATH_PREFIX\":*) echo restored > \"$HOOK_PATH_LOG\" ;;\n"
            "  *) echo retained > \"$HOOK_PATH_LOG\" ;;\n"
            "esac\n",
            encoding="utf-8",
            newline="\n",
        )
        fake_cygpath = fake_bin / "cygpath"
        fake_cygpath.write_text(
            "#!/bin/sh\n"
            "[ \"$1\" = -u ] || exit 2\n"
            "printf '%s\\n' \"$POSIX_GIT_EXEC_PATH\"\n",
            encoding="utf-8",
            newline="\n",
        )
        self.assertNotIn(b"\r", fake_git.read_bytes())
        self.assertNotIn(b"\r", fake_python.read_bytes())
        fake_git.chmod(0o755)
        fake_python.chmod(0o755)
        fake_cygpath.chmod(0o755)
        log = self.root / "hook.log"
        hook_path_log = self.root / "build/ai-checks/hook-path.log"
        fake_git_log = self.root / "fake-git.log"
        environment = os.environ | {
            "PYTHON": str(fake_python),
            "HOOK_LOG": str(log),
            "HOOK_PATH_LOG": str(hook_path_log),
            "FAKE_GIT_LOG": fake_git_log.name,
            "GIT_EXEC_PATH": "C:/fixture/git-core",
            "POSIX_GIT_EXEC_PATH": "/__fixture_git_exec_path__",
        }
        shell = shutil.which("sh")
        self.assertIsNotNone(shell)
        shell_path = 'PATH="$PWD/fake-bin:$PATH"; export PATH;'
        unprefixed_hook_path = (
            'PATH="$PWD/fake-bin:$PATH"; export PATH; '
            'HOOK_PATH_PREFIX="$PWD/fake-bin"; export HOOK_PATH_PREFIX;'
        )
        hook_path = (
            'PATH="$POSIX_GIT_EXEC_PATH:$PWD/fake-bin:$PATH"; export PATH; '
            'HOOK_PATH_PREFIX="$PWD/fake-bin"; export HOOK_PATH_PREFIX;'
        )
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
        unprefixed_result = subprocess.run(
            [shell, "-c", f'{unprefixed_hook_path} exec "$1"', "pre-push-fixture", PRE_PUSH],
            cwd=self.root,
            input="refs/tags/ai/EarlyDev/0.1 tag-object refs/tags/ai/EarlyDev/0.1 0000000000000000000000000000000000000000\n",
            text=True,
            capture_output=True,
            env=environment,
        )
        self.assertEqual(unprefixed_result.returncode, 0, unprefixed_result.stderr)
        self.assertNotIn("git:--exec-path:", fake_git_log.read_text(encoding="utf-8"))
        fake_git_log.write_text("", encoding="utf-8")
        result = subprocess.run(
            [shell, "-c", f'{hook_path} exec "$1"', "pre-push-fixture", PRE_PUSH],
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
                "git:--exec-path:",
                "git:branch:--show-current",
                "git:rev-parse:HEAD",
                "git:rev-parse:tag-object^{commit}",
                "git:status:--porcelain",
            ],
        )
        self.assertEqual(
            log.read_text(encoding="utf-8").strip(),
            "script/ai_check.py --strict --require-index-match",
        )
        self.assertEqual(hook_path_log.read_text(encoding="utf-8").strip(), "restored")

    def test_ai_dev_pre_push_entrypoint_runs_fast_gate_and_propagates_failure(self):
        self.git("branch", "-M", "ai-dev")
        head = self.git("rev-parse", "HEAD").stdout.strip()
        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            fake_python = directory_path / "python"
            fake_python.write_text(
                "#!/bin/sh\n"
                "case \"$1\" in script/ci/release_guard.py) exit 0;; esac\n"
                "printf '%s\\n' \"$*\" > \"$HOOK_LOG\"\n"
                "exit \"${HOOK_EXIT:-0}\"\n",
                encoding="utf-8",
                newline="\n",
            )
            fake_python.chmod(0o755)
            log = directory_path / "hook.log"
            environment = os.environ | {
                "PYTHON": str(fake_python),
                "HOOK_LOG": str(log),
            }
            input_line = f"refs/heads/ai-dev {head} refs/heads/ai-dev {head}\n"
            success = subprocess.run(
                pre_push_command(), cwd=self.root, input=input_line, text=True,
                capture_output=True, env=environment,
            )
            self.assertEqual(success.returncode, 0, success.stderr)
            self.assertEqual(log.read_text(encoding="utf-8").strip(), "script/ai_check.py --fast --require-index-match")

            failure = subprocess.run(
                pre_push_command(), cwd=self.root, input=input_line, text=True,
                capture_output=True, env=environment | {"HOOK_EXIT": "7"},
            )
            self.assertEqual(failure.returncode, 7, failure.stderr)
            self.assertEqual(log.read_text(encoding="utf-8").strip(), "script/ai_check.py --fast --require-index-match")

    def test_ai_main_promotion_pre_push_uses_fast_gate_to_bootstrap_local_proof(self):
        self.git("branch", "-M", "ai-main")
        head = self.git("rev-parse", "HEAD").stdout.strip()
        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            fake_python = directory_path / "python"
            fake_python.write_text(
                "#!/bin/sh\n"
                "case \"$1\" in script/ci/release_guard.py) exit 0;; esac\n"
                "if [ \"$1\" = script/ai_publish.py ]; then\n"
                "  printf '%s\\n' \"$*\" > \"$BOOTSTRAP_LOG\"\n"
                "  exit \"${BOOTSTRAP_EXIT:-0}\"\n"
                "fi\n"
                "printf '%s\\n' \"$*\" > \"$HOOK_LOG\"\n",
                encoding="utf-8",
                newline="\n",
            )
            fake_python.chmod(0o755)
            log = directory_path / "hook.log"
            environment = os.environ | {
                "PYTHON": str(fake_python),
                "HOOK_LOG": str(log),
                "BOOTSTRAP_LOG": str(directory_path / "bootstrap.log"),
            }
            input_line = f"refs/heads/ai-main {head} refs/heads/ai-main {head}\n"
            result = subprocess.run(
                pre_push_command(), cwd=self.root, input=input_line, text=True,
                capture_output=True, env=environment,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                log.read_text(encoding="utf-8").strip(),
                "script/ai_check.py --fast --require-index-match",
            )
            self.assertEqual(
                Path(environment["BOOTSTRAP_LOG"]).read_text(encoding="utf-8").strip(),
                f"script/ai_publish.py verify-promotion-bootstrap {head} {head}",
            )

    def test_nonpromotion_ai_main_push_falls_back_to_strict_gate(self):
        self.git("branch", "-M", "ai-dev")
        head = self.git("rev-parse", "HEAD").stdout.strip()
        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            fake_python = directory_path / "python"
            fake_python.write_text(
                "#!/bin/sh\n"
                "case \"$1\" in script/ci/release_guard.py) exit 0;; esac\n"
                "if [ \"$1\" = script/ai_publish.py ]; then\n"
                "  printf '%s\\n' \"$*\" > \"$BOOTSTRAP_LOG\"\n"
                "  exit 1\n"
                "fi\n"
                "printf '%s\\n' \"$*\" > \"$HOOK_LOG\"\n"
                "case \"$2\" in --strict) exit 7;; --fast) exit 0;; esac\n",
                encoding="utf-8",
                newline="\n",
            )
            fake_python.chmod(0o755)
            log = directory_path / "hook.log"
            bootstrap_log = directory_path / "bootstrap.log"
            environment = os.environ | {
                "PYTHON": str(fake_python),
                "HOOK_LOG": str(log),
                "BOOTSTRAP_LOG": str(bootstrap_log),
            }
            input_line = f"refs/heads/ai-dev {head} refs/heads/ai-main {head}\n"
            result = subprocess.run(
                pre_push_command(), cwd=self.root, input=input_line, text=True,
                capture_output=True, env=environment,
            )
            self.assertEqual(result.returncode, 7, result.stderr)
            self.assertEqual(
                bootstrap_log.read_text(encoding="utf-8").strip(),
                f"script/ai_publish.py verify-promotion-bootstrap {head} {head}",
            )
            self.assertEqual(
                log.read_text(encoding="utf-8").strip(),
                "script/ai_check.py --strict --require-index-match",
            )

    def test_pre_push_command_uses_detected_posix_shell_on_windows(self):
        with mock.patch.object(os, "name", "nt"), mock.patch.object(shutil, "which", return_value="sh.exe") as which:
            self.assertEqual(
                pre_push_command(),
                ["sh.exe", "-c", 'exec "$1"', "pre-push-fixture", str(PRE_PUSH)],
            )
        which.assert_called_once_with("sh")

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
            reuse_flags.append((name, kwargs.get("reuse", False)))
            return checker.PhaseResult(name, command, 0, "")
        with mock.patch.object(checker, "run_phase", side_effect=run_phase), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(checker.main(["--root", str(self.root), "--candidate", "--level", "snapshot"]), 0)
        self.assertIn(("extra-sample", [sys.executable, "check.py"], 7), commands)
        self.assertIn(("extra-sample", False), reuse_flags)
        config[0]["command"] = []
        config_path.write_text(json.dumps(config))
        with mock.patch.object(checker, "run_phase", side_effect=run_phase), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(checker.main(["--root", str(self.root), "--candidate", "--level", "snapshot"]), 1)

    def test_release_proof_registry_runs_for_candidate_and_strict(self):
        checker = load_module()
        config_path = self.root / "script/ai_checks.json"
        config = [{"name": "matrix", "enabled": True, "levels": ["snapshot"],
                   "command": ["{python}", "verify.py", "--root", "{root}",
                               "--candidate-commit", "{proof-candidate-commit}"],
                   "timeout": 7, "reason": "", "when": "release",
                   "proof": True, "reuse": False}]
        config_path.write_text(json.dumps(config))
        commands = []

        def run_phase(root, log_dir, name, command, timeout, **kwargs):
            commands.append((name, command, timeout, kwargs.get("reuse", False)))
            return checker.PhaseResult(name, command, 0, "")

        self.git("branch", "-M", "ai-dev")
        source = self.git("rev-parse", "HEAD").stdout.strip()
        with (
            mock.patch.object(checker, "run_phase", side_effect=run_phase),
            mock.patch.object(checker, "validate_phase_receipt", return_value="matching receipt"),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(checker.main([
                "--root", str(self.root), "--candidate", "--proof-only",
                "--snapshot-source-commit", source, "--level", "snapshot",
            ]), 0)
        expected_candidate = [
            sys.executable, "verify.py", "--root", str(self.root.resolve()),
            "--candidate-commit", source,
        ]
        self.assertIn(("extra-matrix", expected_candidate, 7, False), commands)
        self.assertEqual(
            [name for name, *_ in commands],
            ["docs", "backlog", "current-plan", "release-reports", "diff-working", "diff-cached", "extra-matrix"],
        )

        commands.clear()
        with (
            mock.patch.object(checker, "run_phase", side_effect=run_phase),
            mock.patch.object(checker, "validate_phase_receipt", return_value="matching receipt"),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(checker.main([
                "--root", str(self.root), "--strict", "--proof-only", "--level", "snapshot",
            ]), 0)
        head = self.git("rev-parse", "HEAD").stdout.strip()
        expected_strict = [
            sys.executable, "verify.py", "--root", str(self.root.resolve()),
            "--candidate-commit", head,
        ]
        self.assertIn(("extra-matrix", expected_strict, 7, False), commands)
        self.assertEqual(
            [name for name, *_ in commands],
            ["docs", "backlog", "current-plan", "release-reports", "diff-working", "diff-cached", "extra-matrix"],
        )

        config[0]["when"] = "later"
        config_path.write_text(json.dumps(config))
        with mock.patch.object(checker, "run_phase", side_effect=run_phase), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(checker.main(["--root", str(self.root), "--strict", "--level", "snapshot"]), 1)

    def test_proof_only_fails_closed_without_an_enabled_release_proof(self):
        self.git("branch", "-M", "ai-dev")
        source = self.git("rev-parse", "HEAD").stdout.strip()
        (self.root / "script/ai_checks.json").write_text("[]\n", encoding="utf-8")
        with contextlib.redirect_stdout(io.StringIO()):
            result = load_module().main([
                "--root", str(self.root), "--candidate", "--proof-only",
                "--snapshot-source-commit", source, "--level", "snapshot",
            ])
        self.assertEqual(result, 1)

    def test_proof_only_rejects_missing_integration_receipts_without_running_them(self):
        checker = load_module()
        self.git("branch", "-M", "ai-dev")
        source = self.git("rev-parse", "HEAD").stdout.strip()
        (self.root / "script/ai_checks.json").write_text(json.dumps([{
            "name": "local-proof",
            "enabled": True,
            "levels": ["snapshot"],
            "command": ["{python}", "verify.py"],
            "timeout": 7,
            "reason": "",
            "when": "release",
            "proof": True,
        }]), encoding="utf-8")
        ran = []

        def run_phase(root, log_dir, name, command, timeout, **kwargs):
            ran.append(name)
            return checker.PhaseResult(name, command, 0, "passed")

        with (
            mock.patch.object(checker, "run_phase", side_effect=run_phase),
            mock.patch.object(checker, "validate_phase_receipt", return_value=None),
            contextlib.redirect_stdout(io.StringIO()) as output,
        ):
            result = checker.main([
                "--root", str(self.root), "--candidate", "--proof-only",
                "--snapshot-source-commit", source, "--level", "snapshot",
            ])

        self.assertEqual(result, 1)
        self.assertEqual(ran, ["docs", "backlog", "current-plan", "release-reports", "diff-working", "diff-cached", "extra-local-proof"])
        self.assertIn("receipt-python-tests", output.getvalue())
        self.assertIn("receipt-build", output.getvalue())
        self.assertIn("receipt-ctest", output.getvalue())
        self.assertIn("run the owned full candidate gate", output.getvalue())

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

    def test_matching_fast_receipt_reuses_the_full_python_suite(self):
        marker = self.root / "build/ai-checks/python-command-count"
        test_file = self.root / "script/tests/test_receipt_marker.py"
        test_file.write_text(
            "import os\n"
            "from pathlib import Path\n"
            "import unittest\n"
            "\n"
            "class ReceiptMarker(unittest.TestCase):\n"
            "    def test_suite_runs_once(self):\n"
            "        marker = Path(os.environ['AI_CHECK_MARKER'])\n"
            "        value = int(marker.read_text()) if marker.exists() else 0\n"
            "        marker.parent.mkdir(parents=True, exist_ok=True)\n"
            "        marker.write_text(str(value + 1))\n",
            encoding="utf-8",
        )
        self.git("add", "script/tests/test_receipt_marker.py")
        self.git("commit", "--no-gpg-sign", "-m", "receipt marker fixture")
        environment = os.environ | {"AI_CHECK_MARKER": str(marker)}
        first = self.run_check("--fast", environment=environment)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        second = self.run_check("--fast", environment=environment)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(marker.read_text(encoding="utf-8"), "1")
        self.assertIn("REUSED PASS python-tests", second.stdout)
        summary = json.loads((self.root / "build/ai-checks/summary.json").read_text(encoding="utf-8"))
        self.assertIn("python-tests", summary["reused_phases"])

    def test_ignored_discovered_test_changes_force_python_suite_execution(self):
        ignore_file = self.root / ".gitignore"
        ignore_file.write_text("script/tests/test_ignored_receipt.py\n", encoding="utf-8")
        self.git("add", ".gitignore")
        self.git("commit", "--no-gpg-sign", "-m", "ignore fixture")
        marker = self.root / "build/ai-checks/ignored-test-count"
        test_file = self.root / "script/tests/test_ignored_receipt.py"
        test_file.write_text(
            "import unittest\n"
            "from pathlib import Path\n"
            "\n"
            "class IgnoredReceipt(unittest.TestCase):\n"
            "    def test_discovered(self):\n"
            "        marker = Path('build/ai-checks/ignored-test-count')\n"
            "        value = int(marker.read_text()) if marker.exists() else 0\n"
            "        marker.parent.mkdir(parents=True, exist_ok=True)\n"
            "        marker.write_text(str(value + 1))\n",
            encoding="utf-8",
        )
        first = self.run_check("--fast")
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        test_file.write_text(
            test_file.read_text(encoding="utf-8")
            + "        self.fail('changed ignored test')\n",
            encoding="utf-8",
        )
        second = self.run_check("--fast")
        self.assertEqual(second.returncode, 1, second.stdout + second.stderr)
        self.assertEqual(marker.read_text(encoding="utf-8"), "2")
        self.assertNotIn("REUSED PASS python-tests", second.stdout)

    def test_content_equivalent_index_and_head_identity_does_not_invalidate_receipt(self):
        source = self.root / "script/ai_docs.py"
        source.write_text("raise SystemExit(0)\n# equivalent baseline\n", encoding="utf-8")
        self.git("add", "script/ai_docs.py")
        self.git("commit", "--no-gpg-sign", "-m", "equivalent baseline")
        source.write_text("raise SystemExit(0)\n# equivalent changed\n", encoding="utf-8")
        first = self.run_check("--fast")
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)

        self.git("reset", "HEAD", "--", "script/ai_docs.py")
        second = self.run_check("--fast")
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertIn("REUSED PASS python-tests", second.stdout)

        self.git("add", "script/ai_docs.py")
        self.git("commit", "--no-gpg-sign", "-m", "equivalent content")
        third = self.run_check("--fast")
        self.assertEqual(third.returncode, 0, third.stdout + third.stderr)
        self.assertIn("REUSED PASS python-tests", third.stdout)

    def test_receipt_evidence_and_missing_receipt_force_execution(self):
        first = self.run_check("--fast")
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        log = self.root / "build/ai-checks/python-tests.log"
        log.write_text(log.read_text(encoding="utf-8") + "tampered\n", encoding="utf-8")
        second = self.run_check("--fast")
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertIn("PASS python-tests", second.stdout)
        self.assertNotIn("REUSED PASS python-tests", second.stdout)

        receipt = self.root / "build/ai-checks/python-tests.receipt.json"
        receipt.unlink()
        third = self.run_check("--fast")
        self.assertEqual(third.returncode, 0, third.stdout + third.stderr)
        self.assertNotIn("REUSED PASS python-tests", third.stdout)

        receipt.write_text("{malformed", encoding="utf-8")
        fourth = self.run_check("--fast")
        self.assertEqual(fourth.returncode, 0, fourth.stdout + fourth.stderr)
        self.assertNotIn("REUSED PASS python-tests", fourth.stdout)

    def test_receipt_command_mismatch_forces_execution(self):
        checker = load_module()
        log_dir = self.root / "build/ai-checks"
        log_dir.mkdir(parents=True)
        command = [sys.executable, "script/ai_docs.py", "check"]
        first = checker.run_phase(self.root, log_dir, "docs", command, 60)
        self.assertEqual(first.returncode, 0)
        changed_command = [sys.executable, "script/ai_docs.py", "check", "--changed"]
        second = checker.run_phase(self.root, log_dir, "docs", changed_command, 60, reuse=True)
        self.assertEqual(second.returncode, 0)
        self.assertFalse(second.reused)

    def test_receipt_reuse_ignores_tool_call_id_but_rejects_environment_changes(self):
        checker = load_module()
        log_dir = self.root / "build/ai-checks"
        log_dir.mkdir(parents=True)
        command = [
            sys.executable,
            "-c",
            "import os; print(os.environ['CODEX_TOOL_CALL_ID'])",
        ]
        environment = checker.phase_environment(self.root, "python-tests")
        environment["CODEX_TOOL_CALL_ID"] = "call-1"
        environment["ASAN_OPTIONS"] = "detect_leaks=0"

        with mock.patch.object(checker, "phase_environment", return_value=environment):
            first = checker.run_phase(
                self.root, log_dir, "python-tests", command, 60
            )
        self.assertEqual(first.returncode, 0, first.output)
        self.assertEqual(first.output, "call-1\n")

        next_call_environment = dict(environment)
        next_call_environment["CODEX_TOOL_CALL_ID"] = "call-2"
        with mock.patch.object(
            checker, "phase_environment", return_value=next_call_environment
        ):
            reused = checker.run_phase(
                self.root, log_dir, "python-tests", command, 60, reuse=True
            )
        self.assertEqual(reused.returncode, 0, reused.output)
        self.assertTrue(reused.reused)
        self.assertEqual(reused.output, "call-1\n")

        changed_environment = dict(environment)
        changed_environment["ASAN_OPTIONS"] = "detect_leaks=1"
        with mock.patch.object(
            checker, "phase_environment", return_value=changed_environment
        ):
            rerun = checker.run_phase(
                self.root, log_dir, "python-tests", command, 60, reuse=True
            )
        self.assertEqual(rerun.returncode, 0, rerun.output)
        self.assertFalse(rerun.reused)
        self.assertEqual(rerun.output, "call-1\n")

    def test_fingerprint_failure_invalidates_stale_receipt(self):
        checker = load_module()
        log_dir = self.root / "build/ai-checks"
        log_dir.mkdir(parents=True)
        command = [sys.executable, "script/ai_docs.py", "check"]

        for failing_step in ("phase_environment", "relevant_inputs"):
            with self.subTest(failing_step=failing_step):
                first = checker.run_phase(self.root, log_dir, "docs", command, 60)
                self.assertEqual(first.returncode, 0)
                receipt = log_dir / "docs.receipt.json"
                self.assertTrue(receipt.is_file())
                with mock.patch.object(checker, failing_step, side_effect=RuntimeError("fingerprint unavailable")):
                    second = checker.run_phase(
                        self.root, log_dir, "docs", command, 60, reuse=True
                    )
                self.assertEqual(second.returncode, 127)
                failure_receipt = json.loads(receipt.read_text(encoding="utf-8"))
                self.assertEqual(failure_receipt["status"], "FAIL")

    def test_build_and_ctest_receipts_reuse_only_matching_native_inputs(self):
        checker = load_module()
        log_dir = self.root / "build/ai-checks"
        log_dir.mkdir(parents=True)
        self.configure_native_build_fixture()
        metadata = self.root / "docs/ai/metadata.md"
        metadata.parent.mkdir(parents=True)
        metadata.write_text("metadata v1\n", encoding="utf-8")
        self.git("add", "docs/ai/metadata.md")
        self.git("commit", "--no-gpg-sign", "-m", "metadata baseline")
        source = self.root / "src/shared/include/shared/ProjectInfo.hpp"
        for name in ("build", "ctest"):
            with self.subTest(name=name):
                command = ["true"]
                first = checker.run_phase(self.root, log_dir, name, command, 60)
                self.assertEqual(first.returncode, 0)
                second = checker.run_phase(self.root, log_dir, name, command, 60, reuse=True)
                self.assertEqual(second.returncode, 0)
                self.assertTrue(second.reused)

                metadata.write_text(f"metadata {name} v2\n", encoding="utf-8")
                self.git("add", "docs/ai/metadata.md")
                self.git("commit", "--no-gpg-sign", "-m", f"metadata {name}")
                after_metadata = checker.run_phase(
                    self.root, log_dir, name, command, 60, reuse=True
                )
                self.assertTrue(after_metadata.reused)

                source.write_text(
                    source.read_text(encoding="utf-8") + "// changed native input\n",
                    encoding="utf-8",
                )
                stale = checker.run_phase(self.root, log_dir, name, command, 60, reuse=True)
                self.assertEqual(stale.returncode, 0)
                self.assertFalse(stale.reused)

    def test_native_receipt_rejects_deleted_build_output(self):
        checker = load_module()
        log_dir = self.root / "build/ai-checks"
        log_dir.mkdir(parents=True)
        build_root, _ = self.configure_native_build_fixture()
        command = ["true"]
        passed = checker.run_phase(self.root, log_dir, "ctest", command, 60)
        self.assertEqual(passed.returncode, 0)
        self.assertTrue(checker.receipt_path(log_dir, "ctest").is_file())

        (build_root / "tests/mc_tests").unlink()
        self.assertIsNone(
            checker.validate_phase_receipt(self.root, log_dir, "ctest", command)
        )
        rejected = checker.run_phase(self.root, log_dir, "ctest", command, 60, reuse=True)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertFalse(rejected.reused)

    def test_renderer_smoke_receipt_captures_actual_target_and_rejects_missing_target(self):
        checker = load_module()
        log_dir = self.root / "build/ai-checks"
        log_dir.mkdir(parents=True)
        build_root, _ = self.configure_native_build_fixture(preset="renderer-smoke")
        target_dir = build_root / "tests/renderer-smoke"
        target_dir.mkdir(parents=True, exist_ok=True)
        command = [sys.executable, "-c", "print('renderer smoke fixture passed')"]

        for executable_name in ("mc_renderer_smoke", "mc_renderer_smoke.exe"):
            with self.subTest(executable=executable_name):
                artifact = target_dir / executable_name
                artifact_contents = b"renderer smoke executable"
                artifact.write_bytes(artifact_contents)
                relative_artifact = f"tests/renderer-smoke/{executable_name}"

                passed = checker.run_phase(
                    self.root, log_dir, "extra-renderer-smoke", command, 60
                )
                self.assertEqual(passed.returncode, 0, passed.output)
                receipt = json.loads(
                    checker.receipt_path(log_dir, "extra-renderer-smoke").read_text(
                        encoding="utf-8"
                    )
                )
                native_state = receipt["relevant_inputs"]["native_build_state"]
                self.assertEqual(
                    native_state["artifacts"][relative_artifact],
                    {
                        "size": len(artifact_contents),
                        "mode": artifact.stat().st_mode & 0o7777,
                        "sha256": hashlib.sha256(artifact_contents).hexdigest(),
                    },
                )
                self.assertEqual(
                    checker.validate_phase_receipt(
                        self.root, log_dir, "extra-renderer-smoke", command
                    ),
                    passed.output,
                )

                artifact.unlink()
                self.assertIsNone(
                    checker.validate_phase_receipt(
                        self.root, log_dir, "extra-renderer-smoke", command
                    )
                )
                rejected = checker.run_phase(
                    self.root, log_dir, "extra-renderer-smoke", command, 60, reuse=True
                )
                self.assertNotEqual(rejected.returncode, 0)
                self.assertIn("missing output targets", rejected.output)
                self.assertIn(relative_artifact, rejected.output)

    def test_renderer_smoke_receipt_rejects_ambiguous_target_and_reuse(self):
        checker = load_module()
        log_dir = self.root / "build/ai-checks"
        log_dir.mkdir(parents=True)
        build_root, _ = self.configure_native_build_fixture(preset="renderer-smoke")
        target_dir = build_root / "tests/renderer-smoke"
        target_dir.mkdir(parents=True, exist_ok=True)
        command = [sys.executable, "-c", "print('renderer smoke fixture passed')"]
        executable = target_dir / "mc_renderer_smoke"
        windows_executable = target_dir / "mc_renderer_smoke.exe"
        executable.write_bytes(b"renderer smoke executable")

        passed = checker.run_phase(self.root, log_dir, "extra-renderer-smoke", command, 60)
        self.assertEqual(passed.returncode, 0, passed.output)
        self.assertEqual(
            checker.validate_phase_receipt(
                self.root, log_dir, "extra-renderer-smoke", command
            ),
            passed.output,
        )

        windows_executable.write_bytes(b"Windows renderer smoke executable")
        ambiguous_state = checker.native_build_state(self.root, "extra-renderer-smoke")
        self.assertFalse(ambiguous_state["available"])
        self.assertEqual(ambiguous_state["artifacts"], {})
        self.assertIn("ambiguous output targets", ambiguous_state["reason"])
        self.assertIsNone(
            checker.validate_phase_receipt(
                self.root, log_dir, "extra-renderer-smoke", command
            )
        )

        rejected = checker.run_phase(
            self.root, log_dir, "extra-renderer-smoke", command, 60, reuse=True
        )
        self.assertNotEqual(rejected.returncode, 0)
        self.assertFalse(rejected.reused)
        self.assertIn("ambiguous output targets", rejected.output)

    def test_native_receipt_rejects_changed_ignored_cmake_cache(self):
        checker = load_module()
        log_dir = self.root / "build/ai-checks"
        log_dir.mkdir(parents=True)
        build_root, _ = self.configure_native_build_fixture()
        command = ["true"]
        passed = checker.run_phase(self.root, log_dir, "build", command, 60)
        self.assertEqual(passed.returncode, 0)
        self.assertEqual(
            checker.validate_phase_receipt(self.root, log_dir, "build", command),
            passed.output,
        )

        cache = build_root / "CMakeCache.txt"
        cache.write_text(
            cache.read_text(encoding="utf-8") + "CMAKE_CXX_FLAGS:STRING=-DCHANGED\n",
            encoding="utf-8",
        )
        self.assertIsNone(
            checker.validate_phase_receipt(self.root, log_dir, "build", command)
        )

    def test_native_receipt_rejects_changed_installed_dependency(self):
        checker = load_module()
        log_dir = self.root / "build/ai-checks"
        log_dir.mkdir(parents=True)
        _, dependency = self.configure_native_build_fixture()
        command = ["true"]
        passed = checker.run_phase(self.root, log_dir, "build", command, 60)
        self.assertEqual(passed.returncode, 0)
        self.assertEqual(
            checker.validate_phase_receipt(self.root, log_dir, "build", command),
            passed.output,
        )

        dependency.write_bytes(b"replacement installed dependency")
        self.assertIsNone(
            checker.validate_phase_receipt(self.root, log_dir, "build", command)
        )

    def test_build_does_not_write_pass_without_required_output_artifacts(self):
        checker = load_module()
        log_dir = self.root / "build/ai-checks"
        log_dir.mkdir(parents=True)
        build_root, _ = self.configure_native_build_fixture()
        (build_root / "mc_main").unlink()

        result = checker.run_phase(self.root, log_dir, "build", ["true"], 60)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing output targets", result.output)
        receipt = json.loads(checker.receipt_path(log_dir, "build").read_text(encoding="utf-8"))
        self.assertEqual(receipt["status"], "FAIL")

    def test_full_python_receipt_reuses_equivalent_promotion_but_not_content_changes(self):
        checker = load_module()
        log_dir = self.root / "build/ai-checks"
        log_dir.mkdir(parents=True)
        metadata = self.root / "docs/ai/metadata.md"
        metadata.parent.mkdir(parents=True)
        metadata.write_text("promotion metadata\n", encoding="utf-8")
        self.git("add", "docs/ai/metadata.md")
        self.git("commit", "--no-gpg-sign", "-m", "metadata baseline")
        command = [sys.executable, "-m", "unittest", "discover", "-s", "script/tests", "-v"]
        environment = checker.phase_environment(self.root, "python-tests")
        inputs = checker.relevant_inputs(self.root, "python-tests")
        output = "complete Python suite passed\n"
        (log_dir / "python-tests.log").write_text(output, encoding="utf-8")
        checker.write_receipt(
            log_dir,
            "python-tests",
            command,
            environment,
            inputs,
            checker.PhaseResult("python-tests", command, 0, output),
        )
        self.assertEqual(
            checker.validate_phase_receipt(self.root, log_dir, "python-tests", command), output
        )

        self.git("commit", "--allow-empty", "--no-gpg-sign", "-m", "content-equivalent promotion")
        self.assertEqual(
            checker.validate_phase_receipt(self.root, log_dir, "python-tests", command), output
        )

        metadata.write_text("changed release documentation\n", encoding="utf-8")
        self.assertIsNone(
            checker.validate_phase_receipt(self.root, log_dir, "python-tests", command)
        )

    def test_fast_cpp_changes_skip_unrelated_python_tests(self):
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
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertIn("SKIP python-tests", second.stdout)
        summary = json.loads((self.root / "build/ai-checks/summary.json").read_text(encoding="utf-8"))
        self.assertIn("python-tests", summary["skipped_phases"])

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

    def test_tool_identity_mismatch_rejects_receipt(self):
        checker = load_module()
        log_dir = self.root / "build/ai-checks"
        log_dir.mkdir(parents=True)
        command = [sys.executable, "-c", "pass"]
        environment = checker.phase_environment(self.root, "docs")
        inputs = {"fixture": "unchanged"}
        output = "documentation check passed\n"
        (log_dir / "docs.log").write_text(output, encoding="utf-8")
        checker.write_receipt(
            log_dir,
            "docs",
            command,
            environment,
            inputs,
            checker.PhaseResult("docs", command, 0, output),
        )

        self.assertEqual(
            checker.read_matching_receipt(
                self.root, log_dir, "docs", command, environment, inputs
            ),
            output,
        )
        changed_identity = {**checker.tool_identity(command), "resolved": "changed-tool"}
        with mock.patch.object(checker, "tool_identity", return_value=changed_identity):
            self.assertIsNone(
                checker.read_matching_receipt(
                    self.root, log_dir, "docs", command, environment, inputs
                )
            )

    def test_receipt_environment_mismatch_forces_execution(self):
        first_environment = os.environ | {"AI_CHECK_RECEIPT_ENV": "one"}
        second_environment = os.environ | {"AI_CHECK_RECEIPT_ENV": "two"}
        first = self.run_check("--fast", environment=first_environment)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        second = self.run_check("--fast", environment=second_environment)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertNotIn("REUSED PASS docs", second.stdout)

    def test_changed_path_invalidates_receipt(self):
        original_path = os.environ["PATH"]
        first_environment = os.environ | {"PATH": f"/first-tool-path{os.pathsep}{original_path}"}
        second_environment = os.environ | {"PATH": f"/second-tool-path{os.pathsep}{original_path}"}
        first = self.run_check("--fast", environment=first_environment)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        second = self.run_check("--fast", environment=second_environment)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertNotIn("REUSED PASS docs", second.stdout)

    def test_real_precommit_hook_reuses_manual_receipt(self):
        marker = self.root / "build/ai-checks/docs-command-count"
        (self.root / "script/ai_docs.py").write_text(
            "import os\n"
            "from pathlib import Path\n"
            "marker = Path(os.environ['AI_CHECK_MARKER'])\n"
            "value = int(marker.read_text()) if marker.exists() else 0\n"
            "marker.parent.mkdir(parents=True, exist_ok=True)\n"
            "marker.write_text(str(value + 1))\n",
            encoding="utf-8",
        )
        checker = self.root / "script/ai_commit.py"
        checker.write_text(
            "import os\n"
            "import subprocess\n"
            "import sys\n"
            "from pathlib import Path\n"
            "Path(os.environ['AI_CHECK_PATH_LOG']).parent.mkdir(parents=True, exist_ok=True)\n"
            "Path(os.environ['AI_CHECK_PATH_LOG']).write_text(os.environ['PATH'], encoding='utf-8')\n"
            f"raise SystemExit(subprocess.run([{sys.executable!r}, 'script/ai_check.py', "
            "'--root', '.', '--fast'], check=False).returncode)\n",
            encoding="utf-8",
        )
        shutil.copy2(SCRIPT, self.root / "script/ai_check.py")
        shutil.copy2(SCRIPT.with_name("ai_windows_job.py"), self.root / "script/ai_windows_job.py")
        hook_dir = self.root / ".githooks"
        hook_dir.mkdir()
        shutil.copy2(PRE_COMMIT, hook_dir / "pre-commit")
        self.git("config", "core.hooksPath", str(hook_dir))
        self.git("branch", "-M", "codex/ai-hook-path-reuse")
        hook_path_log = self.root / "build/ai-checks/hook-path.log"
        environment = os.environ | {
            "PYTHON": sys.executable,
            "AI_CHECK_MARKER": str(marker),
            "AI_CHECK_PATH_LOG": str(hook_path_log),
        }
        first = self.run_check("--fast", environment=environment)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        self.assertEqual(marker.read_text(encoding="utf-8"), "1")
        hook_result = subprocess.run(
            ["git", "commit", "--allow-empty", "--no-gpg-sign", "-m", "hook receipt reuse"],
            cwd=self.root,
            env=environment,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(
            hook_result.returncode,
            0,
            hook_result.stdout + hook_result.stderr,
        )
        expected_runs = "2" if os.name == "nt" else "1"
        self.assertEqual(marker.read_text(encoding="utf-8"), expected_runs)
        self.assert_hook_path_retains_caller(
            hook_path_log.read_text(encoding="utf-8"), environment["PATH"]
        )

    def test_real_pre_merge_hook_restores_manual_path(self):
        marker = self.root / "build/ai-checks/docs-command-count"
        (self.root / "script/ai_docs.py").write_text(
            "import os\n"
            "from pathlib import Path\n"
            "marker = Path(os.environ['AI_CHECK_MARKER'])\n"
            "value = int(marker.read_text()) if marker.exists() else 0\n"
            "marker.parent.mkdir(parents=True, exist_ok=True)\n"
            "marker.write_text(str(value + 1))\n",
            encoding="utf-8",
        )
        hook_path_log = self.root / "build/ai-checks/hook-path.log"
        checker = self.root / "script/ai_commit.py"
        checker.write_text(
            "import os\n"
            "import subprocess\n"
            "import sys\n"
            "from pathlib import Path\n"
            "Path(os.environ['AI_CHECK_PATH_LOG']).parent.mkdir(parents=True, exist_ok=True)\n"
            "Path(os.environ['AI_CHECK_PATH_LOG']).write_text(os.environ['PATH'], encoding='utf-8')\n"
            f"raise SystemExit(subprocess.run([{sys.executable!r}, 'script/ai_check.py', "
            "'--root', '.', '--fast'], check=False).returncode)\n",
            encoding="utf-8",
        )
        shutil.copy2(SCRIPT, self.root / "script/ai_check.py")
        shutil.copy2(SCRIPT.with_name("ai_windows_job.py"), self.root / "script/ai_windows_job.py")
        hook_dir = self.root / ".githooks"
        hook_dir.mkdir()
        shutil.copy2(PRE_COMMIT, hook_dir / "pre-commit")
        shutil.copy2(PRE_MERGE, hook_dir / "pre-merge-commit")
        self.git("add", ".githooks", "script")
        self.git("commit", "--no-gpg-sign", "-m", "pre-merge receipt fixture")
        branch = "codex/ai-merge-hook-path-reuse"
        self.git("branch", "-M", branch)
        self.git("switch", "-c", "merge-side")
        self.git("commit", "--allow-empty", "--no-gpg-sign", "-m", "empty merge side")
        self.git("switch", branch)
        self.git("config", "core.hooksPath", str(hook_dir))
        merge_head = self.git("rev-parse", "merge-side").stdout.strip()
        environment = os.environ | {
            "PYTHON": sys.executable,
            "AI_CHECK_MARKER": str(marker),
            "AI_CHECK_PATH_LOG": str(hook_path_log),
            "GIT_REFLOG_ACTION": "merge merge-side",
            f"GITHEAD_{merge_head}": "merge-side",
        }
        first = self.run_check("--fast", environment=environment)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        self.assertEqual(marker.read_text(encoding="utf-8"), "1")
        merge = subprocess.run(
            ["git", "merge", "--no-ff", "--no-edit", "merge-side"],
            cwd=self.root,
            env=environment,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(merge.returncode, 0, merge.stdout + merge.stderr)
        expected_runs = "2" if os.name == "nt" else "1"
        self.assertEqual(marker.read_text(encoding="utf-8"), expected_runs)
        self.assert_hook_path_retains_caller(
            hook_path_log.read_text(encoding="utf-8"), environment["PATH"]
        )

    def test_real_pre_push_hook_reuses_manual_receipt(self):
        marker = self.root / "build/ai-checks/docs-command-count"
        (self.root / "script/ai_docs.py").write_text(
            "import os\n"
            "from pathlib import Path\n"
            "marker = Path(os.environ['AI_CHECK_MARKER'])\n"
            "value = int(marker.read_text()) if marker.exists() else 0\n"
            "marker.parent.mkdir(parents=True, exist_ok=True)\n"
            "marker.write_text(str(value + 1))\n",
            encoding="utf-8",
        )
        (self.root / ".gitignore").write_text(
            "build/\n__pycache__/\n**/__pycache__/\n*.pyc\n", encoding="utf-8"
        )
        hook_dir = self.root / ".githooks"
        hook_dir.mkdir()
        shutil.copy2(PRE_PUSH, hook_dir / "pre-push")
        shutil.copy2(SCRIPT, self.root / "script/ai_check.py")
        shutil.copy2(SCRIPT.with_name("ai_windows_job.py"), self.root / "script/ai_windows_job.py")
        release_guard = self.root / "script/ci/release_guard.py"
        release_guard.parent.mkdir(parents=True)
        release_guard.write_text(
            "import os\n"
            "from pathlib import Path\n"
            "Path(os.environ['AI_CHECK_PATH_LOG']).parent.mkdir(parents=True, exist_ok=True)\n"
            "Path(os.environ['AI_CHECK_PATH_LOG']).write_text(os.environ['PATH'], encoding='utf-8')\n",
            encoding="utf-8",
        )
        self.git("branch", "-M", "ai-dev")
        self.git("add", ".gitignore", ".githooks", "script")
        self.git("commit", "--no-gpg-sign", "-m", "pre-push receipt fixture")
        self.git("config", "core.hooksPath", str(hook_dir))
        hook_path_log = self.root / "build/ai-checks/hook-path.log"
        environment = os.environ | {
            "PYTHON": sys.executable,
            "AI_CHECK_MARKER": str(marker),
            "AI_CHECK_PATH_LOG": str(hook_path_log),
        }
        first = self.run_check("--fast", environment=environment)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        self.assertEqual(marker.read_text(encoding="utf-8"), "1")
        self.assertEqual(self.git("status", "--porcelain").stdout, "")
        with tempfile.TemporaryDirectory() as directory:
            remote = Path(directory) / "remote.git"
            subprocess.run(["git", "init", "--bare", "-q", str(remote)], check=True)
            push = subprocess.run(
                ["git", "push", str(remote), "ai-dev:ai-dev"],
                cwd=self.root,
                env=environment,
                text=True,
                capture_output=True,
                check=False,
            )
        self.assertEqual(push.returncode, 0, push.stdout + push.stderr)
        expected_runs = "2" if os.name == "nt" else "1"
        self.assertEqual(marker.read_text(encoding="utf-8"), expected_runs)
        self.assert_hook_path_retains_caller(
            hook_path_log.read_text(encoding="utf-8"), environment["PATH"]
        )

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

    def test_fast_cpp_changes_defer_build_and_ctest_to_batch_gate(self):
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
        self.assertIn("source-checks", calls)
        self.assertNotIn("python-tests", calls)
        self.assertNotIn("build", calls)
        self.assertNotIn("ctest", calls)

    def test_unknown_tooling_mixed_with_native_changes_keeps_python_and_source_checks(self):
        checker = load_module()
        changed = {"src/fixture.cpp", "publish.py"}
        calls = []

        def run_phase(root, log_dir, name, command, timeout, **kwargs):
            calls.append(name)
            return checker.PhaseResult(name, command, 0, "")

        with mock.patch.object(checker, "changed_paths", return_value=changed), mock.patch.object(checker, "untracked_paths", return_value=set()), mock.patch.object(checker, "run_phase", side_effect=run_phase), contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(checker.main(["--root", str(self.root), "--fast"]), 0)
        self.assertIn("full-python", output.getvalue())
        self.assertIn("python-tests", calls)
        self.assertIn("source-checks", calls)

    def test_mixed_native_and_tooling_changes_propagate_source_policy_failure(self):
        checker = load_module()

        def run_phase(root, log_dir, name, command, timeout, **kwargs):
            return checker.PhaseResult(name, command, 1 if name == "source-checks" else 0, "source policy failed")

        with mock.patch.object(checker, "changed_scope", return_value=("full-python", ["src/fixture.cpp", "publish.py"])), \
                mock.patch.object(checker, "run_phase", side_effect=run_phase), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(checker.main(["--root", str(self.root), "--fast"]), 1)
        self.assertIn("FAIL source-checks", output.getvalue())

    def test_fast_cpp_source_policy_failure_propagates_without_python_suite(self):
        (self.root / "CMakePresets.json").write_text("{}\n", encoding="utf-8")
        (self.root / "src/fixture.cpp").write_text("changed\n", encoding="utf-8")
        environment = os.environ | {"AI_CHECK_SOURCE_FAILURE": "1"}

        result = self.run_check("--fast", environment=environment)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("FAIL source-checks", result.stdout)
        self.assertNotIn("PASS python-tests", result.stdout)
        self.assertIn("SKIP python-tests", result.stdout)

    def test_fast_cpp_source_checks_allow_ordinary_publisher_failures(self):
        (self.root / "CMakePresets.json").write_text("{}\n", encoding="utf-8")
        (self.root / "src/fixture.cpp").write_text("changed\n", encoding="utf-8")
        environment = os.environ | {"AI_CHECK_SOURCE_ORDINARY_FAILURE": "1"}
        result = self.run_check("--fast", environment=environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ALLOWED source-checks", result.stdout)

    def test_full_and_strict_checks_still_run_and_propagate_build_failures(self):
        checker = load_module()

        for arguments in ([], ["--strict"]):
            with self.subTest(arguments=arguments):
                calls = []

                def run_phase(root, log_dir, name, command, timeout, **kwargs):
                    calls.append(name)
                    return checker.PhaseResult(name, command, 7 if name == "build" else 0, "failed build")

                with mock.patch.object(checker, "run_phase", side_effect=run_phase), contextlib.redirect_stdout(io.StringIO()):
                    result = checker.main(["--root", str(self.root), *arguments])

                self.assertEqual(result, 1)
                self.assertIn("python-tests", calls)
                self.assertIn("build", calls)
                self.assertIn("ctest", calls)

    def test_full_ctest_outer_budget_covers_nested_server_only_build(self):
        checker = load_module()
        (self.root / "script/ai_checks.json").write_text("[]\n", encoding="utf-8")
        calls = []

        def run_phase(root, log_dir, name, command, timeout, **kwargs):
            calls.append((name, command, timeout))
            return checker.PhaseResult(name, command, 0, "")

        with mock.patch.object(checker, "run_phase", side_effect=run_phase), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(checker.main(["--root", str(self.root)]), 0)

        ctest = next(call for call in calls if call[0] == "ctest")
        self.assertEqual(ctest[1][-2:], ["--timeout", "240"])
        self.assertEqual(ctest[2], checker.BUILD_PHASE_TIMEOUT)
        self.assertTrue(all(timeout <= 600 for _, _, timeout in calls))

    def test_all_configured_automated_stages_fit_ten_minutes(self):
        checker = load_module()
        self.assertEqual(checker.MAX_PHASE_TIMEOUT, 600)
        self.assertEqual(checker.TOTAL_TIMEOUT, 1500)
        self.assertEqual(checker.BUILD_PHASE_TIMEOUT, 600)
        self.assertLessEqual(checker.PYTHON_TEST_TIMEOUT, checker.MAX_PHASE_TIMEOUT)
        self.assertLessEqual(checker.WINDOWS_PYTHON_TEST_TIMEOUT, checker.MAX_PHASE_TIMEOUT)
        self.assertEqual(int(checker.BUILD_PHASE_COMMANDS["ctest"][-1]), 240)
        registry = json.loads((REPOSITORY / "script/ai_checks.json").read_text(encoding="utf-8"))
        self.assertTrue(registry)
        self.assertTrue(all(item["timeout"] <= checker.MAX_PHASE_TIMEOUT for item in registry))

    def test_aggregate_deadline_marks_remaining_phases_unrun_and_failing(self):
        checker = load_module()
        (self.root / "script/ai_checks.json").write_text("[]\n", encoding="utf-8")
        clock = [0.0]
        calls = []

        def monotonic():
            return clock[0]

        def run_phase(root, log_dir, name, command, timeout, **kwargs):
            calls.append((name, timeout))
            clock[0] += checker.TOTAL_TIMEOUT
            (log_dir / f"{name}.log").write_text("partial stage output\n", encoding="utf-8")
            return checker.PhaseResult(name, command, 124, "partial stage output\n", timed_out=True)

        with (
            mock.patch.object(checker, "time") as mocked_time,
            mock.patch.object(checker, "run_phase", side_effect=run_phase),
            contextlib.redirect_stdout(io.StringIO()) as output,
        ):
            mocked_time.monotonic.side_effect = monotonic
            mocked_time.time_ns.return_value = 1
            self.assertEqual(checker.main(["--root", str(self.root)]), 1)

        self.assertEqual([name for name, _ in calls], ["docs"])
        self.assertEqual(calls[0][1], min(60, checker.MAX_PHASE_TIMEOUT))
        self.assertIn("UNRUN backlog", output.getvalue())
        self.assertIn("UNRUN ctest", output.getvalue())
        summary = json.loads((self.root / "build/ai-checks/summary.json").read_text(encoding="utf-8"))
        self.assertIn("docs", summary["timed_out_phases"])
        self.assertIn("backlog", summary["unrun_phases"])
        self.assertIn("ctest", summary["unrun_phases"])
        self.assertIn("partial stage output", (self.root / "build/ai-checks/docs.log").read_text(encoding="utf-8"))
        self.assertIn("deadline", (self.root / "build/ai-checks/backlog.log").read_text(encoding="utf-8").lower())
        unrun_receipt = json.loads(checker.receipt_path(self.root / "build/ai-checks", "backlog").read_text(encoding="utf-8"))
        self.assertEqual(unrun_receipt["status"], "UNRUN")

    def test_phase_timeout_terminates_descendant_processes(self):
        checker = load_module()
        log_dir = self.root / "build/ai-checks"
        log_dir.mkdir(parents=True)
        marker = self.root / "descendant-finished"
        child = (
            "import pathlib,time; time.sleep(1.5); "
            f"pathlib.Path({str(marker)!r}).write_text('alive')"
        )
        parent = (
            "import subprocess,sys,time; "
            f"subprocess.Popen([sys.executable, '-c', {child!r}]); "
            "print('parent started', flush=True); time.sleep(30)"
        )
        result = checker.run_phase(
            self.root, log_dir, "tree-timeout", [sys.executable, "-c", parent], 1
        )
        self.assertEqual(result.returncode, 124)
        self.assertTrue(result.timed_out)
        self.assertIn("parent started", result.output)
        timeout_receipt = json.loads(checker.receipt_path(log_dir, "tree-timeout").read_text(encoding="utf-8"))
        self.assertEqual(timeout_receipt["status"], "TIMEOUT")
        self.assertTrue(timeout_receipt["timed_out"])
        time.sleep(1.7)
        self.assertFalse(marker.exists(), "descendant survived its timed-out phase")

    @unittest.skipUnless(os.name == "nt", "Windows owned-job cleanup contract")
    def test_phase_exit_cleans_descendants_after_command_leader_exits(self):
        checker = load_module()
        log_dir = self.root / "build/ai-checks"
        log_dir.mkdir(parents=True)
        marker = self.root / "descendant-finished"
        child = (
            "import pathlib,time; time.sleep(2); "
            f"pathlib.Path({str(marker)!r}).write_text('alive')"
        )
        parent = (
            "import subprocess,sys; "
            f"subprocess.Popen([sys.executable, '-c', {child!r}]); "
            "print('leader finished', flush=True)"
        )
        result = checker.run_phase(
            self.root, log_dir, "exited-leader", [sys.executable, "-c", parent], 10
        )
        self.assertEqual(result.returncode, 0, result.output)
        self.assertIn("leader finished", result.output)
        time.sleep(2.2)
        self.assertFalse(marker.exists())

    def test_full_and_strict_checks_run_full_python_for_focused_scopes(self):
        checker = load_module()
        (self.root / "script/ai_checks.json").write_text("[]\n", encoding="utf-8")

        for scope in ("metadata-only", "targeted-python:script/tests/test_leaf.py"):
            for arguments in ([], ["--strict"], ["--candidate"]):
                with self.subTest(scope=scope, arguments=arguments):
                    calls = []

                    def run_phase(root, log_dir, name, command, timeout, **kwargs):
                        calls.append((name, command))
                        return checker.PhaseResult(name, command, 0, "")

                    with mock.patch.object(checker, "changed_scope", return_value=(scope, [])), mock.patch.object(checker, "run_phase", side_effect=run_phase), contextlib.redirect_stdout(io.StringIO()):
                        result = checker.main(["--root", str(self.root), *arguments])

                    self.assertEqual(result, 0)
                    python_tests = next(command for name, command in calls if name == "python-tests")
                    self.assertEqual(
                        python_tests,
                        [sys.executable, "-m", "unittest", "discover", "-s", "script/tests", "-v"],
                    )

    def test_workflow_only_changes_use_full_python_scope(self):
        checker = load_module()
        changed = {
            "AGENTS.md",
            ".agents/skills/implement/SKILL.md",
            ".githooks/pre-push",
            "docs/ai/WORKFLOW.md",
            "docs/code/source_state.json",
            "script/ai_check.py",
            "script/tests/test_ai_check.py",
        }
        with mock.patch.object(
            checker,
            "changed_paths",
            side_effect=lambda root, cached: changed if cached else set(),
        ), mock.patch.object(checker, "untracked_paths", return_value=set()):
            scope, paths = checker.changed_scope(self.root)
        self.assertEqual(scope, "full-python")
        self.assertEqual(paths, sorted(changed))

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

    def test_leaf_python_mapping_targets_matching_test_with_metadata(self):
        source = self.root / "script/check_options.py"
        test_file = self.root / "script/tests/test_check_options.py"
        metadata = self.root / "docs/ai/generated.md"
        source.write_text("VALUE = 1\n", encoding="utf-8")
        test_file.write_text(
            "import unittest\n"
            "class Options(unittest.TestCase):\n"
            "    def test_ok(self): self.assertTrue(True)\n",
            encoding="utf-8",
        )
        metadata.parent.mkdir(parents=True, exist_ok=True)
        metadata.write_text("generated\n", encoding="utf-8")
        self.git("add", "script/check_options.py", "script/tests/test_check_options.py", "docs/ai/generated.md")
        self.git("commit", "--no-gpg-sign", "-m", "fixture leaf metadata")
        source.write_text("VALUE = 2\n", encoding="utf-8")
        test_file.write_text(test_file.read_text(encoding="utf-8") + "# focused\n", encoding="utf-8")
        metadata.write_text("regenerated\n", encoding="utf-8")

        result = self.run_check("--fast")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("CHECK SCOPE targeted-python:script/tests/test_check_options.py", result.stdout)
        self.assertIn(
            "PASS python-tests: " + " ".join([sys.executable, "-m", "unittest", "script/tests/test_check_options.py", "-v"]),
            result.stdout,
        )

    def test_targeted_python_failure_propagates(self):
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
        self.git("commit", "--no-gpg-sign", "-m", "fixture leaf failure")
        source.write_text("VALUE = 2\n", encoding="utf-8")
        test_file.write_text(
            test_file.read_text(encoding="utf-8").replace("self.assertTrue(True)", "self.assertTrue(False)"),
            encoding="utf-8",
        )

        result = self.run_check("--fast")
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL python-tests", result.stdout)
        self.assertIn("script/tests/test_check_options.py", result.stdout)


if __name__ == "__main__":
    unittest.main()
