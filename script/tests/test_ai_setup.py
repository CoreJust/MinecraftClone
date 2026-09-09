import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "script/ai_setup.py"


def load_module():
    spec = importlib.util.spec_from_file_location("ai_setup", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class AiSetupTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        self.git("init")
        hook_dir = self.root / ".githooks"
        hook_dir.mkdir()
        for name in ("pre-commit", "pre-push", "commit-msg", "post-commit", "pre-merge-commit"):
            hook = hook_dir / name
            hook.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            hook.chmod(0o755)

    def tearDown(self):
        self.temp_dir.cleanup()

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.root, text=True, capture_output=True, check=True)

    def run_setup(self, *args):
        return subprocess.run([sys.executable, SCRIPT, "--root", self.root, *args], text=True, capture_output=True)

    def test_install_is_idempotent(self):
        first = self.run_setup("--install-hooks")
        second = self.run_setup("--install-hooks")
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(self.git("config", "--local", "--get", "core.hooksPath").stdout.strip(), ".githooks")
        self.assertIn("already installed", second.stdout)

    def test_install_preserves_conflicting_config_and_legacy_hook(self):
        self.git("config", "--local", "core.hooksPath", "custom-hooks")
        configured = self.run_setup("--install-hooks")
        self.assertEqual(configured.returncode, 1)
        self.assertIn("Refusing to replace", configured.stderr)

        self.git("config", "--local", "--unset", "core.hooksPath")
        legacy = self.root / ".git/hooks/pre-commit"
        legacy.write_text("#!/bin/sh\n", encoding="utf-8")
        legacy_result = self.run_setup("--install-hooks")
        self.assertEqual(legacy_result.returncode, 1)
        self.assertIn("Refusing to hide existing hooks", legacy_result.stderr)

    def test_doctor_is_read_only(self):
        ai_setup = load_module()
        before = subprocess.run(
            ["git", "config", "--local", "--get", "core.hooksPath"],
            cwd=self.root,
            text=True,
            capture_output=True,
        )
        self.assertNotEqual(ai_setup.doctor(self.root), None)
        after = subprocess.run(
            ["git", "config", "--local", "--get", "core.hooksPath"],
            cwd=self.root,
            text=True,
            capture_output=True,
        )
        self.assertEqual(before.returncode, after.returncode)
        self.assertEqual(before.stdout, after.stdout)

    def test_install_rejects_non_executable_hook(self):
        (self.root / ".githooks/pre-merge-commit").chmod(0o644)
        result = self.run_setup("--install-hooks")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Executable .githooks", result.stderr)


if __name__ == "__main__":
    unittest.main()
