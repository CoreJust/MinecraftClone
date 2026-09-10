"""Static contracts for valid GitHub Actions workflow contexts."""

from __future__ import annotations

import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
WORKFLOWS = (
    REPOSITORY / ".github/workflows/ai-checks.yml",
    REPOSITORY / ".github/workflows/snapshot-artifacts.yml",
)


class WorkflowContextTests(unittest.TestCase):
    def test_workflows_keep_runner_context_out_of_job_level_env(self):
        for workflow_path in WORKFLOWS:
            lines = workflow_path.read_text(encoding="utf-8").splitlines()
            job_env_lines: list[str] = []
            collecting = False
            for line in lines:
                if line == "    env:":
                    collecting = True
                    continue
                if collecting and line.startswith("      "):
                    job_env_lines.append(line)
                    continue
                if collecting:
                    collecting = False
            job_env = "\n".join(job_env_lines)
            self.assertNotIn("${{ runner.", job_env, workflow_path)
            workflow = workflow_path.read_text(encoding="utf-8")
            self.assertIn("Configure vcpkg binary cache (macOS)", workflow)
            self.assertIn("Configure vcpkg binary cache (Windows)", workflow)
            self.assertIn("mkdir -p \"$cache_dir\"", workflow)
            self.assertIn("VCPKG_DEFAULT_BINARY_CACHE=$cache_dir", workflow)
            self.assertIn("vcpkg-binary-cache", workflow)


if __name__ == "__main__":
    unittest.main()
