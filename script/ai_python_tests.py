#!/usr/bin/env python3
"""Run the fast Python test group, deferring inventory-classified larger groups."""

from __future__ import annotations

import argparse
import json
import sys
import unittest
from pathlib import Path
from types import ModuleType
from typing import Iterable, Sequence


INVENTORY = Path("script/validation_inventory.json")
TEST_ROOT = Path("script/tests")


class TestSelectionError(ValueError):
    """The requested Python test selection is incomplete or unclassified."""


def _medium_test_sources(root: Path) -> dict[str, Path]:
    try:
        inventory = json.loads((root / INVENTORY).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise TestSelectionError(f"cannot read Python test inventory: {error}") from error
    if not isinstance(inventory, dict):
        raise TestSelectionError("Python test inventory must be an object")
    groups = inventory.get("python_test_groups", [])
    if not isinstance(groups, list) or not groups:
        raise TestSelectionError("python_test_groups must declare the mandatory medium Python group")
    required = inventory.get("required_python_test_groups")
    if not isinstance(required, dict) or not required or any(
        not isinstance(group_id, str) or not group_id
        or not isinstance(source, str) or not source.startswith(f"{TEST_ROOT.as_posix()}/")
        for group_id, source in required.items()
    ):
        raise TestSelectionError("required_python_test_groups must map ids to declared test source paths")

    sources: dict[str, Path] = {}
    group_ids: set[str] = set()
    classified_sources: dict[str, str] = {}
    for group in groups:
        if not isinstance(group, dict):
            raise TestSelectionError("Python test inventory contains a non-object group")
        group_id = group.get("id")
        source = group.get("source")
        size = group.get("size")
        if not isinstance(group_id, str) or not group_id or group_id in group_ids:
            raise TestSelectionError("Python test group has a missing or invalid id")
        if (
            not isinstance(source, str)
            or not source.startswith(f"{TEST_ROOT.as_posix()}/")
            or ".." in Path(source).parts
        ):
            raise TestSelectionError(f"Python test group has an invalid source: {group_id}")
        if size != "medium":
            raise TestSelectionError(f"Python test group has an unsupported size: {group_id}")
        source_path = (root / source).resolve()
        try:
            source_path.relative_to((root / TEST_ROOT).resolve())
        except ValueError as error:
            raise TestSelectionError(f"Python test group source escapes the test directory: {source}") from error
        if not source_path.is_file():
            raise TestSelectionError(f"Python test group source is missing: {source}")
        if source in classified_sources.values():
            raise TestSelectionError(f"Python test source is classified more than once: {source}")
        group_ids.add(group_id)
        classified_sources[group_id] = source
        sources[source] = source_path
    mismatched_required = sorted(
        group_id for group_id, source in required.items()
        if classified_sources.get(group_id) != source or source not in sources
    )
    if mismatched_required:
        raise TestSelectionError(
            "mandatory Python test groups are missing or mismatched: "
            + ", ".join(mismatched_required)
        )
    return sources


def _flatten(suite: unittest.TestSuite) -> Iterable[unittest.TestCase]:
    for test in suite:
        if isinstance(test, unittest.TestSuite):
            yield from _flatten(test)
        else:
            yield test


def _test_source(root: Path, test: unittest.TestCase) -> Path:
    module: ModuleType | None = sys.modules.get(test.__class__.__module__)
    source = getattr(module, "__file__", None)
    if not isinstance(source, str):
        raise TestSelectionError(f"cannot identify source file for test {test.id()}")
    resolved = Path(source).resolve()
    try:
        resolved.relative_to(root.resolve())
    except ValueError as error:
        raise TestSelectionError(f"test was loaded from outside the repository: {test.id()}") from error
    return resolved


def select_tests(
    root: Path,
    tests: Sequence[unittest.TestCase],
    medium_sources: dict[str, Path],
    targets: Sequence[str] | None = None,
) -> tuple[list[unittest.TestCase], dict[str, int]]:
    """Select requested discovered tests and return inventory-deferred counts."""
    root = root.resolve()
    medium_paths = {path.resolve() for path in medium_sources.values()}
    requested = None
    if targets is not None:
        requested = set()
        for target in targets:
            source = (root / target).resolve()
            try:
                source.relative_to(root.resolve())
            except ValueError as error:
                raise TestSelectionError(f"test target escapes the repository: {target}") from error
            requested.add(source)

    by_source: dict[Path, list[unittest.TestCase]] = {}
    for test in tests:
        by_source.setdefault(_test_source(root, test), []).append(test)

    missing_groups = [source for source in medium_paths if source not in by_source]
    if missing_groups:
        names = ", ".join(str(source.relative_to(root)) for source in missing_groups)
        raise TestSelectionError(f"inventory Python test groups were not discovered: {names}")
    if requested is not None:
        missing_targets = requested - set(by_source)
        if missing_targets:
            names = ", ".join(str(source.relative_to(root)) for source in sorted(missing_targets))
            raise TestSelectionError(f"requested Python test sources were not discovered: {names}")

    deferred = {
        path: len(by_source[path])
        for path in medium_paths
        if path in by_source and (requested is None or path in requested)
    }
    selected = [
        test
        for source, cases in by_source.items()
        if (requested is None or source in requested) and source not in medium_paths
        for test in cases
    ]
    if not selected and not deferred:
        raise TestSelectionError("Python test selection is empty")
    return selected, deferred


def run(root: Path, targets: Sequence[str] | None = None) -> int:
    root = root.resolve()
    root_import_path = str(root)
    if root_import_path not in sys.path:
        sys.path.insert(0, root_import_path)
    medium_sources = _medium_test_sources(root)
    loader = unittest.TestLoader()
    suite = loader.discover(str(root / TEST_ROOT), pattern="test*.py")
    if loader.errors:
        raise TestSelectionError("Python test discovery failed:\n" + "\n".join(loader.errors))
    tests = list(_flatten(suite))
    selected, deferred = select_tests(root, tests, medium_sources, targets)
    for source, count in sorted(deferred.items()):
        print(
            f"DEFERRED {source.relative_to(root)}: {count} test cases; "
            "run the matching medium validation lane",
            flush=True,
        )
    if not selected:
        print("No small Python tests matched this change scope.", flush=True)
        return 0
    return int(not unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite(selected)).wasSuccessful())


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--targets", nargs="*", help="run only these discovered test source files")
    args = parser.parse_args(argv)
    try:
        return run(args.root.resolve(), args.targets)
    except (OSError, TestSelectionError) as error:
        print(f"Python test selection blocked: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
