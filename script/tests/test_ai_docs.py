"""Contracts for the generated code-documentation inventory."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "ai_docs.py"
SPEC = importlib.util.spec_from_file_location("ai_docs", SCRIPT)
assert SPEC and SPEC.loader
ai_docs = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ai_docs)


class AiDocsTest(unittest.TestCase):
    def make_repo(self) -> tempfile.TemporaryDirectory[str]:
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
        self.write(root, "src/core/one.cpp", "int one = 1;\n")
        self.write(root, "script/tool.py", "print('tool')\n")
        self.write(root, ".codex/config.toml", "model = 'test'\n")
        self.write(root, "docs/code/CORE.md", "# Core\n")
        self.write(root, "docs/code/TOOLING.md", "# Tooling\n")
        self.write_json(root, "docs/code/modules.json", {
            "modules": [
                {"id": "core", "sources": ["src/**"], "guide": "docs/code/CORE.md"},
                {"id": "tooling", "sources": ["script/**/*.py", ".codex/config.toml"], "guide": "docs/code/TOOLING.md"},
            ],
        })
        return temporary

    def write(self, root: Path, relative: str, contents: str) -> None:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")

    def write_json(self, root: Path, relative: str, data: object) -> None:
        self.write(root, relative, json.dumps(data, indent=2) + "\n")

    def assert_docs_error(self, operation: object) -> ai_docs.DocsError:
        with self.assertRaises(ai_docs.DocsError) as context:
            operation()
        return context.exception

    def test_source_state_detects_added_removed_and_changed_files(self) -> None:
        with self.make_repo() as temporary:
            root = Path(temporary)
            ai_docs.refresh(root)
            ai_docs.check(root)

            self.write(root, "src/core/two.cpp", "int two = 2;\n")
            self.assertIn("stale or missing source state", str(self.assert_docs_error(lambda: ai_docs.check(root))))
            ai_docs.refresh(root)

            (root / "src/core/two.cpp").unlink()
            self.assert_docs_error(lambda: ai_docs.check(root))
            ai_docs.refresh(root)
            self.assertNotIn("two.cpp", (root / ai_docs.INDEX_PATH).read_text(encoding="utf-8"))

            self.write(root, "src/core/one.cpp", "int one = 42;\n")
            self.assert_docs_error(lambda: ai_docs.check(root))
            ai_docs.refresh(root)
            ai_docs.check(root)

    def test_guide_edits_require_refresh(self) -> None:
        with self.make_repo() as temporary:
            root = Path(temporary)
            ai_docs.refresh(root)
            self.write(root, "docs/code/CORE.md", "# Core\n\nReviewed description.\n")
            error = self.assert_docs_error(lambda: ai_docs.check(root))
            self.assertIn("stale or missing source state", str(error))
            ai_docs.refresh(root)
            ai_docs.check(root)

    def test_coverage_rejects_overlap_and_uncovered_files(self) -> None:
        with self.make_repo() as temporary:
            root = Path(temporary)
            registry = root / "docs/code/modules.json"
            data = json.loads(registry.read_text(encoding="utf-8"))
            data["modules"][1]["sources"] = ["src/**", "script/**/*.py", ".codex/config.toml"]
            registry.write_text(json.dumps(data), encoding="utf-8")
            overlap = self.assert_docs_error(lambda: ai_docs.refresh(root))
            self.assertIn("covered by core, tooling", str(overlap))

            data["modules"][1]["sources"] = ["script/tool.py", ".codex/config.toml"]
            registry.write_text(json.dumps(data), encoding="utf-8")
            self.write(root, "script/other.py", "pass\n")
            uncovered = self.assert_docs_error(lambda: ai_docs.refresh(root))
            self.assertIn("script/other.py: uncovered", str(uncovered))

    def test_refresh_is_deterministic_and_idempotent(self) -> None:
        with self.make_repo() as temporary:
            root = Path(temporary)
            ai_docs.refresh(root)
            first_index = (root / ai_docs.INDEX_PATH).read_bytes()
            first_state = (root / ai_docs.STATE_PATH).read_bytes()
            ai_docs.refresh(root)
            self.assertEqual(first_index, (root / ai_docs.INDEX_PATH).read_bytes())
            self.assertEqual(first_state, (root / ai_docs.STATE_PATH).read_bytes())
            ai_docs.check(root)

    def test_first_refresh_accepts_pending_generated_link_targets(self) -> None:
        with self.make_repo() as temporary:
            root = Path(temporary)
            self.write(
                root,
                "docs/ai/MAINTENANCE.md",
                "[index](../code/INDEX.md) [state](../code/source_state.json)\n",
            )
            ai_docs.refresh(root)
            self.assertTrue((root / ai_docs.INDEX_PATH).is_file())
            self.assertTrue((root / ai_docs.STATE_PATH).is_file())
            ai_docs.check(root)

    def test_rejects_missing_links_and_path_escapes(self) -> None:
        with self.make_repo() as temporary:
            root = Path(temporary)
            self.write(root, "README.md", "[missing](docs/absent.md) [escape](../../outside.md)\n")
            errors = self.assert_docs_error(lambda: ai_docs.refresh(root))
            self.assertIn("missing link target", str(errors))
            self.assertIn("link escapes repository", str(errors))

            self.write_json(root, "docs/code/modules.json", {
                "modules": [{"id": "core", "sources": ["../src/**"], "guide": "../CORE.md"}],
            })
            escaped = self.assert_docs_error(lambda: ai_docs.refresh(root))
            self.assertIn("must stay below the repository root", str(escaped))

    def test_rejects_guide_symlink_outside_repository(self) -> None:
        with self.make_repo() as temporary:
            root = Path(temporary)
            guide = root / "docs/code/CORE.md"
            guide.unlink()
            guide.symlink_to(SCRIPT)
            error = self.assert_docs_error(lambda: ai_docs.refresh(root))
            self.assertIn("guide resolves outside the repository", str(error))


if __name__ == "__main__":
    unittest.main()
