#!/usr/bin/env python3
"""Check that code documentation follows the module registry in docs/code."""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import re
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parent.parent
REGISTRY_PATH = Path("docs/code/modules.json")
INDEX_PATH = Path("docs/code/INDEX.md")
STATE_PATH = Path("docs/code/source_state.json")
ID_PATTERN = re.compile(r"[a-z][a-z0-9_-]*\Z")
MARKDOWN_LINK = re.compile(r"(?<!!)\[[^\]]*\]\(\s*(<[^>]+>|[^\s)]+)")
MARKDOWN_DESTINATION = re.compile(r"(?<!!)\[([^\]]*)\]\(\s*(?:<[^>]+>|[^\s)]+)(?:\s+[^)]*)?\)")
AUTOLINK = re.compile(r"<(?:[a-zA-Z][a-zA-Z0-9+.-]*:)[^>]+>")
FENCED_CODE = re.compile(r"```.*?```", re.DOTALL)
INLINE_CODE = re.compile(r"`[^`]*`")
WORD = re.compile(r"[^\W_]+(?:['’-][^\W_]+)*", re.UNICODE)


class DocsError(Exception):
    """A collection of documentation integrity failures."""

    def __init__(self, errors: list[str]) -> None:
        self.errors = errors
        super().__init__("\n".join(errors))


def relative_path(root: Path, file_path: Path) -> str:
    return file_path.relative_to(root).as_posix()


def regular_files(directory: Path) -> list[Path]:
    if not directory.is_dir():
        return []
    return sorted((path for path in directory.rglob("*") if path.is_file() and not path.is_symlink()), key=str)


def collect_source_files(root: Path) -> list[Path]:
    """Return the bounded set of files that must appear in one module."""
    files: set[Path] = set()
    files.update(regular_files(root / "src"))
    files.update(regular_files(root / "tests"))
    files.update(regular_files(root / "cmake"))
    files.update(regular_files(root / ".githooks"))
    files.update(regular_files(root / ".agents" / "skills"))
    files.update(regular_files(root / ".github" / "workflows"))

    for name in ("CMakeLists.txt", "CMakePresets.json", "publish.py", ".gitignore", ".gitattributes", "AGENTS.md", "CLAUDE.md"):
        candidate = root / name
        if candidate.is_file() and not candidate.is_symlink():
            files.add(candidate)
    files.update(
        path
        for path in (*root.glob("vcpkg*.json"), root / "dependencies.lock.json")
        if path.is_file() and not path.is_symlink()
    )
    files.update(path for path in (root / "script").rglob("*.py") if path.is_file() and not path.is_symlink())
    files.update(path for path in (root / "script").glob("*.json") if path.is_file() and not path.is_symlink())
    config = root / ".codex" / "config.toml"
    if config.is_file() and not config.is_symlink():
        files.add(config)
    return sorted(files, key=lambda path: relative_path(root, path))


def load_registry(root: Path) -> list[dict[str, Any]]:
    registry_file = root / REGISTRY_PATH
    try:
        data = json.loads(registry_file.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise DocsError([f"missing registry: {REGISTRY_PATH}"])
    except json.JSONDecodeError as error:
        raise DocsError([f"invalid JSON in {REGISTRY_PATH}: {error}"])
    if not isinstance(data, dict) or not isinstance(data.get("modules"), list):
        raise DocsError([f"{REGISTRY_PATH} must contain a modules array"])
    return data["modules"]


def safe_relative_path(value: object, label: str) -> str | None:
    if not isinstance(value, str) or not value or Path(value).is_absolute():
        return f"{label} must be a non-empty relative path"
    if "\\" in value or any(part == ".." for part in Path(value).parts):
        return f"{label} must stay below the repository root: {value!r}"
    return None


def validate_registry(root: Path, modules: list[dict[str, Any]]) -> list[str]:
    errors: list[str] = []
    seen_ids: set[str] = set()
    for index, module in enumerate(modules):
        label = f"modules[{index}]"
        if not isinstance(module, dict):
            errors.append(f"{label} must be an object")
            continue
        module_id = module.get("id")
        if not isinstance(module_id, str) or not ID_PATTERN.fullmatch(module_id):
            errors.append(f"{label}.id must match {ID_PATTERN.pattern!r}")
        elif module_id in seen_ids:
            errors.append(f"duplicate module id: {module_id}")
        else:
            seen_ids.add(module_id)
        sources = module.get("sources")
        if not isinstance(sources, list) or not sources or not all(isinstance(item, str) and item for item in sources):
            errors.append(f"{label}.sources must be a non-empty string array")
        else:
            for pattern in sources:
                path_error = safe_relative_path(pattern, f"{label}.sources")
                if path_error:
                    errors.append(path_error)
        guide = module.get("guide")
        path_error = safe_relative_path(guide, f"{label}.guide")
        if path_error:
            errors.append(path_error)
        elif not (root / guide).is_file():
            errors.append(f"missing guide for {module.get('id', label)!r}: {guide}")
        else:
            try:
                (root / guide).resolve().relative_to(root.resolve())
            except ValueError:
                errors.append(f"{label}.guide resolves outside the repository: {guide}")
    if not modules:
        errors.append("module registry must not be empty")
    return errors


def matching_modules(relative: str, modules: list[dict[str, Any]]) -> list[str]:
    return [module["id"] for module in modules if any(glob_matches(relative, pattern) for pattern in module["sources"])]


def glob_matches(relative: str, pattern: str) -> bool:
    """Match POSIX path components, with ** matching zero or more directories."""
    parts = relative.split("/")
    patterns = pattern.split("/")

    def match(path_index: int, pattern_index: int) -> bool:
        if pattern_index == len(patterns):
            return path_index == len(parts)
        current = patterns[pattern_index]
        if current == "**":
            return any(match(next_index, pattern_index + 1) for next_index in range(path_index, len(parts) + 1))
        return (
            path_index < len(parts)
            and fnmatch.fnmatchcase(parts[path_index], current)
            and match(path_index + 1, pattern_index + 1)
        )

    return match(0, 0)


def validate_coverage(root: Path, modules: list[dict[str, Any]]) -> tuple[dict[str, list[str]], list[str]]:
    files_by_module = {module["id"]: [] for module in modules}
    errors: list[str] = []
    for file_path in collect_source_files(root):
        relative = relative_path(root, file_path)
        matches = matching_modules(relative, modules)
        if len(matches) != 1:
            description = "uncovered" if not matches else "covered by " + ", ".join(matches)
            errors.append(f"{relative}: {description}")
            continue
        files_by_module[matches[0]].append(relative)
    return files_by_module, errors


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def source_digest(root: Path, files: list[str]) -> str:
    digest = hashlib.sha256()
    for relative in sorted(files):
        data = (root / relative).read_bytes()
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(data)
        digest.update(b"\0")
    return digest.hexdigest()


def state_data(root: Path, modules: list[dict[str, Any]], files_by_module: dict[str, list[str]]) -> dict[str, Any]:
    entries = []
    for module in sorted(modules, key=lambda item: item["id"]):
        guide = module["guide"]
        entries.append({
            "id": module["id"],
            "guide": guide,
            "guide_sha256": sha256_bytes((root / guide).read_bytes()),
            "sources": sorted(files_by_module[module["id"]]),
            "sources_sha256": source_digest(root, files_by_module[module["id"]]),
        })
    return {"version": 1, "modules": entries}


def markdown_link(relative: str) -> str:
    destination = "../../" + relative
    if " " in destination:
        destination = f"<{destination}>"
    return f"[`{relative}`]({destination})"


def render_index(modules: list[dict[str, Any]], files_by_module: dict[str, list[str]]) -> str:
    lines = ["# Code module index", "", "Generated by `script/ai_docs.py refresh`; do not edit by hand."]
    for module in sorted(modules, key=lambda item: item["id"]):
        guide_name = Path(module["guide"]).name
        guide_link = Path(os.path.relpath(module["guide"], INDEX_PATH.parent)).as_posix()
        lines.extend(["", f"## `{module['id']}`", "", f"Guide: [{guide_name}]({guide_link})", "", "Files:"])
        lines.extend(f"- {markdown_link(relative)}" for relative in sorted(files_by_module[module["id"]]))
    return "\n".join(lines) + "\n"


def render_state(root: Path, modules: list[dict[str, Any]], files_by_module: dict[str, list[str]]) -> str:
    return json.dumps(state_data(root, modules, files_by_module), indent=2, sort_keys=True) + "\n"


def markdown_files(root: Path, include_index: bool) -> list[Path]:
    candidates = [root / name for name in ("README.md", "AGENTS.md", "CLAUDE.md")]
    candidates.extend((root / "docs").rglob("*.md") if (root / "docs").is_dir() else [])
    skills = root / ".agents" / "skills"
    candidates.extend(skills.rglob("SKILL.md") if skills.is_dir() else [])
    index = root / INDEX_PATH
    return sorted({path for path in candidates if path.is_file() and (include_index or path.resolve() != index.resolve())}, key=str)


def local_link_errors(root: Path, markdown_file: Path, pending_targets: set[Path] | None = None) -> list[str]:
    errors: list[str] = []
    text = INLINE_CODE.sub("", FENCED_CODE.sub("", markdown_file.read_text(encoding="utf-8")))
    for raw_destination in MARKDOWN_LINK.findall(text):
        destination = raw_destination[1:-1] if raw_destination.startswith("<") else raw_destination
        destination = destination.split("#", 1)[0].split("?", 1)[0]
        if not destination or re.match(r"[a-zA-Z][a-zA-Z0-9+.-]*:", destination) or destination.startswith("//"):
            continue
        target = (markdown_file.parent / destination).resolve()
        try:
            target.relative_to(root.resolve())
        except ValueError:
            errors.append(f"{relative_path(root, markdown_file)}: link escapes repository: {raw_destination}")
        else:
            if not target.exists() and target not in (pending_targets or set()):
                errors.append(f"{relative_path(root, markdown_file)}: missing link target: {raw_destination}")
    return errors


def prose_words(text: str) -> int:
    prose = FENCED_CODE.sub("", text)
    prose = AUTOLINK.sub("", MARKDOWN_DESTINATION.sub(r"\1", prose))
    prose = INLINE_CODE.sub("", prose)
    return len(WORD.findall(prose))


def document_errors(root: Path, modules: list[dict[str, Any]], include_index: bool, pending_targets: set[Path] | None = None) -> list[str]:
    errors: list[str] = []
    for markdown_file in markdown_files(root, include_index):
        errors.extend(local_link_errors(root, markdown_file, pending_targets))
        relative = relative_path(root, markdown_file)
        if relative == "AGENTS.md":
            limit = 650
        elif relative.startswith(".agents/skills/"):
            limit = 500
        elif relative == "docs/ai/ROADMAP.md":
            limit = 1400
        elif relative.startswith("docs/ai/") and relative != "docs/ai/BACKLOG.md":
            limit = 900
        else:
            limit = None
        if limit is not None and prose_words(markdown_file.read_text(encoding="utf-8")) > limit:
            errors.append(f"{relative}: prose exceeds {limit} words")
    for module in modules:
        guide = root / module["guide"]
        limit = 1400 if module["guide"] == "docs/ai/ROADMAP.md" else 900
        if prose_words(guide.read_text(encoding="utf-8")) > limit:
            errors.append(f"{module['guide']}: guide prose exceeds {limit} words")
    return errors


def prepare(root: Path) -> tuple[list[dict[str, Any]], dict[str, list[str]]]:
    modules = load_registry(root)
    errors = validate_registry(root, modules)
    if errors:
        raise DocsError(errors)
    files_by_module, errors = validate_coverage(root, modules)
    if errors:
        raise DocsError(errors)
    return modules, files_by_module


def refresh(root: Path) -> None:
    modules, files_by_module = prepare(root)
    pending_targets = {(root / INDEX_PATH).resolve(), (root / STATE_PATH).resolve()}
    errors = document_errors(root, modules, include_index=False, pending_targets=pending_targets)
    if errors:
        raise DocsError(errors)
    (root / INDEX_PATH).parent.mkdir(parents=True, exist_ok=True)
    (root / INDEX_PATH).write_text(render_index(modules, files_by_module), encoding="utf-8")
    (root / STATE_PATH).write_text(render_state(root, modules, files_by_module), encoding="utf-8")


def check(root: Path) -> None:
    modules, files_by_module = prepare(root)
    errors = document_errors(root, modules, include_index=True)
    expected_index = render_index(modules, files_by_module)
    expected_state = render_state(root, modules, files_by_module)
    for path, expected, description in (
        (root / INDEX_PATH, expected_index, "INDEX"),
        (root / STATE_PATH, expected_state, "source state"),
    ):
        if not path.is_file() or path.read_text(encoding="utf-8") != expected:
            errors.append(f"stale or missing {description}: {relative_path(root, path)}; run `python3 script/ai_docs.py refresh`")
    if errors:
        raise DocsError(errors)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("check", "refresh"))
    args = parser.parse_args(argv)
    try:
        if args.command == "refresh":
            refresh(ROOT)
            print("Refreshed code documentation inventory and source state.")
            print("This records reviewer acknowledgment of the guides; it cannot prove their semantic accuracy.")
        else:
            check(ROOT)
            print("Code documentation integrity check passed.")
    except DocsError as error:
        print("Code documentation integrity check failed:", file=sys.stderr)
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
