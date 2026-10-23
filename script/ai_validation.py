#!/usr/bin/env python3
"""Select and run repository validation checks from changed inputs."""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import importlib.util
import json
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence


INVENTORY = Path("script/validation_inventory.json")
RECEIPT_DIR = Path("build/ai-validation/receipts")
LOG_DIR = Path("build/ai-validation/logs")
LANES = ("routine", "small", "medium", "major", "large", "full")
LARGE_LABELS = {"gpu", "acceptance", "golden"}


class SelectionError(ValueError):
    """The requested scope cannot be mapped to a complete validation set."""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_inventory(root: Path) -> tuple[dict[str, Any], str]:
    path = root / INVENTORY
    try:
        raw = path.read_bytes()
        inventory = json.loads(raw)
    except (OSError, json.JSONDecodeError) as error:
        raise SelectionError(f"cannot read validation inventory: {error}") from error
    if not isinstance(inventory, dict):
        raise SelectionError("validation inventory must be a JSON object")
    if inventory.get("schema_version") != 1:
        raise SelectionError("unsupported validation inventory schema")
    for lane in ("small", "medium", "large"):
        if not isinstance(inventory.get("budgets_seconds", {}).get(lane), int):
            raise SelectionError(f"inventory is missing the {lane} lane budget")
    if not inventory.get("checks"):
        raise SelectionError("validation inventory has no executable checks")
    validate_release_registry(root, inventory)
    _python_test_groups(root, inventory)
    return inventory, sha256_bytes(raw)


def _python_test_groups(root: Path, inventory: dict[str, Any]) -> list[dict[str, Any]]:
    """Validate inventory-owned Python groups and normalize their inputs."""
    groups = inventory.get("python_test_groups", [])
    if not isinstance(groups, list) or not groups:
        raise SelectionError("python_test_groups must declare the mandatory medium Python group")
    allowed_modules = set(inventory.get("native", {}).get("all_test_modules", [])) | {"tooling"}
    required = inventory.get("required_python_test_groups")
    if not isinstance(required, dict) or not required or any(
        not isinstance(group_id, str) or not re.fullmatch(r"[a-z][a-z0-9-]*", group_id)
        or not isinstance(source, str) or not source.startswith("script/tests/")
        for group_id, source in required.items()
    ):
        raise SelectionError("required_python_test_groups must map ids to declared test source paths")
    identifiers: set[str] = set()
    sources: set[str] = set()
    validated = []
    for group in groups:
        if not isinstance(group, dict):
            raise SelectionError("Python test inventory contains a non-object group")
        group_id = group.get("id")
        module = group.get("module")
        size = group.get("size")
        source = group.get("source")
        if not isinstance(group_id, str) or not re.fullmatch(r"[a-z][a-z0-9-]*", group_id) or group_id in identifiers:
            raise SelectionError("Python test inventory has a missing, invalid, or duplicate group id")
        if not isinstance(module, str) or module not in allowed_modules:
            raise SelectionError(f"Python test group has an unclassified module: {group_id}")
        if size != "medium":
            raise SelectionError(f"Python test group has an unsupported size: {group_id}; classify only an implemented medium group")
        if not isinstance(source, str) or not source.startswith("script/tests/") or ".." in Path(source).parts:
            raise SelectionError(f"Python test group has an invalid source: {group_id}")
        source_path = (root / source).resolve()
        try:
            source_path.relative_to((root / "script/tests").resolve())
        except ValueError as error:
            raise SelectionError(f"Python test group source escapes the test directory: {source}") from error
        if not source_path.is_file():
            raise SelectionError(f"Python test group source is missing: {source}")
        if source in sources:
            raise SelectionError(f"Python test source is classified more than once: {source}")
        inputs = group.get("inputs")
        triggers = group.get("trigger_inputs", inputs)
        if not isinstance(inputs, list) or not inputs or any(not isinstance(path, str) or not path for path in inputs):
            raise SelectionError(f"Python test group has invalid fingerprint inputs: {group_id}")
        for pattern in inputs:
            if Path(pattern).is_absolute() or ".." in Path(pattern).parts:
                raise SelectionError(f"Python test group input escapes the repository: {group_id} ({pattern})")
            matches = [path for path in root.glob(pattern) if path.is_file()]
            if not matches:
                raise SelectionError(f"Python test group input is missing: {group_id} ({pattern})")
        if not isinstance(triggers, list) or not triggers or any(not isinstance(path, str) or not path for path in triggers):
            raise SelectionError(f"Python test group has invalid trigger inputs: {group_id}")
        if not set(triggers) <= set(inputs):
            raise SelectionError(f"Python test triggers are not fingerprinted inputs: {group_id}")
        commands = group.get("commands")
        if not isinstance(commands, list) or not commands or any(
            not isinstance(command, list) or not command
            or any(not isinstance(argument, str) for argument in command)
            for command in commands
        ):
            raise SelectionError(f"Python test group has invalid commands: {group_id}")
        if any(command[:3] != ["{python}", "-m", "unittest"] for command in commands):
            raise SelectionError(f"Python test group must invoke unittest directly: {group_id}")
        source_name = Path(source).name
        if any(source_name not in command for command in commands):
            raise SelectionError(f"Python test group command does not target its declared source: {group_id}")
        identifiers.add(group_id)
        sources.add(source)
        validated.append({**group, "trigger_inputs": triggers})
    actual_sources = {group["id"]: group["source"] for group in validated}
    mismatched_required = sorted(
        group_id for group_id, source in required.items()
        if actual_sources.get(group_id) != source
    )
    if mismatched_required:
        raise SelectionError(
            "mandatory Python test group sources are missing or mismatched: "
            + ", ".join(mismatched_required)
        )
    return validated


def validate_release_registry(root: Path, inventory: dict[str, Any]) -> str:
    """Require exact inventory coverage for enabled gates, levels, and lane classes."""
    registry_path = root / "script/ai_checks.json"
    try:
        raw = registry_path.read_bytes()
        registry = json.loads(raw)
    except (OSError, json.JSONDecodeError) as error:
        raise SelectionError(f"cannot read release check registry: {error}") from error
    if not isinstance(registry, list):
        raise SelectionError("release check registry must be a list")
    if any(not isinstance(row, dict) for row in registry):
        raise SelectionError("release check registry contains a non-object row")
    enabled_levels: dict[str, list[str]] = {}
    for row in registry:
        if row.get("enabled") is not True:
            continue
        name = row.get("name")
        if not isinstance(name, str) or not name:
            raise SelectionError("release check registry has a missing or invalid enabled check name")
        if name in enabled_levels:
            raise SelectionError("release check registry has a duplicate enabled check name")
        levels = row.get("levels", [])
        if not isinstance(levels, list) or not levels or any(
            not isinstance(level, str) or level not in {"snapshot", "minor", "major"}
            for level in levels
        ) or len(levels) != len(set(levels)):
            raise SelectionError(f"release check registry has invalid enabled levels: {name}")
        enabled_levels[name] = sorted(levels)
    actual = set(enabled_levels)
    declared = {check.get("id") for check in inventory.get("release_checks", [])}
    if actual != declared:
        detail = []
        missing = sorted(actual - declared)
        stale = sorted(declared - actual)
        if missing:
            detail.append("unclassified enabled checks: " + ", ".join(missing))
        if stale:
            detail.append("inventory checks absent from registry: " + ", ".join(stale))
        raise SelectionError("; ".join(detail))
    allowed_modules = set(inventory["native"]["all_test_modules"]) | {"native-all", "tooling"}
    for check in inventory["release_checks"]:
        if check.get("size") not in {"small", "medium", "large"}:
            raise SelectionError(f"release check has invalid size: {check.get('id')}")
        if check.get("module") not in allowed_modules:
            raise SelectionError(f"release check has unclassified module: {check.get('id')}")
        levels = check.get("levels")
        if not isinstance(levels, list) or not levels or any(
            not isinstance(level, str) or level not in {"snapshot", "minor", "major"}
            for level in levels
        ) or len(levels) != len(set(levels)) or sorted(levels) != enabled_levels[check["id"]]:
            raise SelectionError(f"release check inventory levels mismatch: {check.get('id')}")
    return sha256_bytes(raw)


def _cache_values(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key, value = line.split("=", 1)
        name, separator, _kind = key.partition(":")
        if separator:
            values[name] = value
    return values


def parse_ctest_inventory(payload: dict[str, Any], root: Path, native: dict[str, Any]) -> list[dict[str, Any]]:
    """Classify generated CTest rows by their CMake source or exact registration."""
    if not isinstance(payload.get("tests"), list) or not payload["tests"]:
        raise SelectionError("configured CTest inventory is empty or malformed")
    prefix_rules = native.get("test_source_modules", [])
    source_overrides = native.get("test_source_overrides", {})
    registration_rules = native.get("registration_tests", {})
    required_registrations = native.get("required_registration_tests", [])
    if not isinstance(required_registrations, list) or any(
        not isinstance(name, str) or not name or name not in registration_rules
        for name in required_registrations
    ):
        raise SelectionError("required CTest registrations must name declared registration tests")
    tests = []
    seen = set()
    for row in payload["tests"]:
        name = row.get("name")
        if not isinstance(name, str) or not name or name in seen:
            raise SelectionError("configured CTest inventory has a missing or duplicate test name")
        seen.add(name)
        properties = {
            item.get("name"): item.get("value")
            for item in row.get("properties", [])
            if isinstance(item, dict)
        }
        command = row.get("command")
        if not isinstance(command, list) or not command or not isinstance(command[0], str):
            raise SelectionError(f"configured CTest command is missing or malformed: {name}")
        labels_value = properties.get("LABELS", [])
        labels = set(labels_value if isinstance(labels_value, list) else [labels_value])
        source_value = properties.get("DEF_SOURCE_LINE")
        source = None
        if source_value:
            source_text = source_value[0] if isinstance(source_value, list) else source_value
            source_text = str(source_text).rsplit(":", 1)[0]
            try:
                source_path = Path(source_text).resolve().relative_to(root.resolve()).as_posix()
            except (OSError, ValueError) as error:
                raise SelectionError(f"CTest source for {name} is outside this checkout") from error
            match = source_overrides.get(source_path) or next(
                (rule for rule in prefix_rules if source_path.startswith(rule["prefix"])),
                None,
            )
            if match is None:
                raise SelectionError(f"unclassified configured CTest source: {name} ({source_path})")
            module, size = match["module"], match["size"]
            source = source_path
        else:
            classification = registration_rules.get(name)
            if classification is None:
                raise SelectionError(f"unclassified configured CTest registration: {name}")
            module, size = classification["module"], classification["size"]
            source_inputs = list(classification.get("inputs", []))
            artifacts = list(classification.get("artifacts", []))
            build_targets = list(classification.get("build_targets", []))
        if source is not None:
            source_inputs = [source]
            artifacts = list(match.get("artifacts", []))
            build_targets = list(match.get("build_targets", []))
        if labels & LARGE_LABELS:
            size = "large"
        if size not in {"small", "medium", "large"}:
            raise SelectionError(f"invalid size classification for configured CTest: {name}")
        tests.append({
            "name": name,
            "source": source,
            "inputs": source_inputs,
            "artifacts": artifacts,
            "build_targets": build_targets,
            "executables": [command[0]],
            "module": module,
            "size": size,
            "labels": sorted(str(label) for label in labels if label),
            "command": command,
        })
    missing_registrations = sorted(set(required_registrations) - seen)
    if missing_registrations:
        raise SelectionError("configured CTest inventory is missing required registrations: "
                             + ", ".join(missing_registrations))
    return sorted(tests, key=lambda test: test["name"])


def load_native_inventory(root: Path, native: dict[str, Any]) -> dict[str, Any]:
    """Read the current configured CTest graph; never guess at an absent graph."""
    build_dir = root / native["build_dir"]
    cache_path = build_dir / "CMakeCache.txt"
    ctest_file = build_dir / "CTestTestfile.cmake"
    if not cache_path.is_file() or not ctest_file.is_file():
        raise SelectionError(
            f"native selection requires a configured {native['preset']} CMake/CTest tree; "
            "configure it before selecting native checks"
        )
    values = _cache_values(cache_path)
    configured_root = values.get("CMAKE_HOME_DIRECTORY")
    if not configured_root or Path(configured_root).resolve() != root.resolve():
        raise SelectionError("configured CTest tree belongs to a different checkout")
    if values.get("CMAKE_GENERATOR") != "Ninja":
        raise SelectionError("configured CTest tree is not using the inventoried Ninja preset")
    make_program = Path(values.get("CMAKE_MAKE_PROGRAM", ""))
    if not make_program.is_file():
        raise SelectionError("configured CTest tree is missing its recorded build tool")
    ctest = shutil.which(native.get("ctest_executable", "ctest"))
    if ctest is None:
        raise SelectionError("ctest is unavailable; native selection cannot be classified")
    command = [ctest, "--test-dir", str(build_dir), "--show-only=json-v1"]
    try:
        completed = subprocess.run(
            command,
            cwd=root,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise SelectionError(f"cannot capture configured CTest inventory: {error}") from error
    if completed.returncode:
        raise SelectionError(completed.stderr.strip() or "ctest inventory capture failed")
    try:
        payload = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise SelectionError(f"ctest returned malformed inventory JSON: {error}") from error
    tests = parse_ctest_inventory(payload, root, native)
    generated = sorted(build_dir.rglob("CTestTestfile.cmake"))
    config_files = [cache_path, build_dir / "build.ninja", *generated]
    config_hashes = []
    for path in config_files:
        if path.is_file():
            config_hashes.append([path.relative_to(root).as_posix(), sha256_file(path)])
    config_hashes.append(["ctest-inventory", sha256_bytes(completed.stdout.encode())])
    return {
        "tests": tests,
        "build_dir": str(build_dir),
        "fingerprint": sha256_bytes(json.dumps(config_hashes, sort_keys=True).encode()),
        "make_program": str(make_program.resolve()),
        "ctest": str(Path(ctest).resolve()),
    }


def _path_rule(path: str, rules: list[dict[str, str]]) -> dict[str, str] | None:
    matches = [rule for rule in rules if fnmatch.fnmatchcase(path, rule["pattern"])]
    if not matches:
        return None
    return max(matches, key=lambda rule: sum(character not in "*?[]" for character in rule["pattern"]))


def _changed_modules(paths: Sequence[str], inventory: dict[str, Any]) -> tuple[set[str], list[dict[str, str]], bool]:
    modules: set[str] = set()
    reasons = []
    native_global = False
    for raw_path in sorted(set(paths)):
        path = raw_path.replace("\\", "/").removeprefix("./")
        rule = _path_rule(path, inventory.get("path_rules", []))
        if rule is None:
            raise SelectionError(f"unclassified changed path: {path}")
        module = rule["module"]
        if rule["kind"] == "native-global":
            native_global = True
            impacted = set(inventory["native"]["all_test_modules"])
        elif rule["kind"] == "source":
            impacted = set(inventory["native"].get("source_dependencies", {}).get(module, [module]))
        else:
            impacted = {module}
        modules.update(impacted)
        reasons.append({
            "path": path,
            "module": module,
            "impact": rule["kind"],
            "selects": ", ".join(sorted(impacted)),
        })
    return modules, reasons, native_global


def _group_tests(tests: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    groups: dict[tuple[str, str], list[dict[str, Any]]] = {}
    for test in tests:
        groups.setdefault((test["module"], test["size"]), []).append(test)
    result = []
    for (module, size), rows in sorted(groups.items()):
        names = [row["name"] for row in rows]
        regex = "^(" + "|".join(re.escape(name) for name in names) + ")$"
        result.append({
            "id": f"ctest:{module}:{size}",
            "module": module,
            "size": size,
            "test_names": names,
            "test_count": len(names),
            "test_names_sample": names[:5],
            "reason": "Configured CTest rows mapped from their source paths or exact registration names.",
            "inputs": sorted({item for row in rows for item in row.get("inputs", [])}),
            "artifacts": sorted({item for row in rows for item in row.get("artifacts", [])}),
            "build_targets": sorted({item for row in rows for item in row.get("build_targets", [])}),
            "executables": sorted({item for row in rows for item in row.get("executables", [])}),
            "prerequisites": ["native-build"],
            "commands": [[
                "ctest", "--test-dir", "{build_dir}", "--output-on-failure",
                "--no-tests=error", "-R", regex,
            ]],
        })
    return result


def select_plan(
    root: Path,
    changed_paths: Sequence[str],
    lane: str = "routine",
    native_tests: Sequence[dict[str, Any]] | None = None,
    native_config_fingerprint: str = "injected-test-inventory",
) -> dict[str, Any]:
    """Select checks, refusing any path or configured CTest row without a class."""
    started = time.monotonic()
    if lane not in LANES:
        raise SelectionError(f"unknown lane: {lane}")
    inventory, inventory_digest = load_inventory(root)
    affected, reasons, native_global = _changed_modules(changed_paths, inventory)
    if native_tests is None:
        native = load_native_inventory(root, inventory["native"])
        native_tests = native["tests"]
        native_config_fingerprint = native["fingerprint"]
        build_dir = native["build_dir"]
    else:
        build_dir = str(root / inventory["native"]["build_dir"])
    if not native_tests:
        raise SelectionError("configured CTest inventory has no classifiable tests")
    if native_global:
        configuration_patterns = inventory["native"].get("configuration_inputs", [])
        changed_configuration = [
            reason["path"] for reason in reasons
            if reason["impact"] == "native-global"
            and any(fnmatch.fnmatchcase(reason["path"], pattern) for pattern in configuration_patterns)
        ]
        if changed_configuration:
            raise SelectionError(
                "native configuration changed; regenerate the configured test inventory first: "
                + ", ".join(changed_configuration)
                + "; run cmake --preset debug before selecting checks"
            )
    allowed_modules = set(inventory["native"]["all_test_modules"])
    unknown_modules = {test.get("module") for test in native_tests} - allowed_modules
    if unknown_modules:
        raise SelectionError("unclassified native test modules: " + ", ".join(sorted(unknown_modules)))
    configured_test_inputs = {
        input_path
        for test in native_tests
        for input_path in test.get("inputs", [])
    }
    configured_test_sources = {
        test.get("source") for test in native_tests if test.get("source")
    }
    configured_modules = {test.get("module") for test in native_tests}
    missing_required_modules = (
        set(inventory["native"].get("modules_requiring_tests", [])) & affected
    ) - configured_modules
    if missing_required_modules:
        raise SelectionError(
            "configured CTest inventory has no tests for affected modules: "
            + ", ".join(sorted(missing_required_modules))
            + "; register and configure those module tests before selecting checks"
        )
    for raw_path in changed_paths:
        path = raw_path.replace("\\", "/").removeprefix("./")
        is_test_source = path.startswith("tests/") and (
            path.endswith("_tests.cpp") or path.endswith("_test.py") or path.endswith(".cmake.in")
        )
        if is_test_source and path not in configured_test_sources and not any(
            fnmatch.fnmatchcase(path, pattern) for pattern in configured_test_inputs
        ):
            raise SelectionError(
                f"changed test source is absent from configured CTest inventory: {path}; "
                "register it and reconfigure before selecting checks"
            )

    all_groups = _group_tests(native_tests)
    python_groups = _python_test_groups(root, inventory)
    if native_global:
        affected.update(allowed_modules)

    selected: list[dict[str, Any]] = []
    if lane in {"routine", "small", "major", "full"}:
        selected.extend(dict(check) for check in inventory["checks"] if check.get("always"))
    if lane in {"routine", "small", "major", "full"}:
        selected.extend(group for group in all_groups if group["size"] == "small")
    if lane == "routine":
        selected.extend(
            group for group in all_groups
            if group["size"] == "medium" and group["module"] in affected
        )
    elif lane in {"medium", "major", "full"}:
        selected.extend(group for group in all_groups if group["size"] == "medium")
    if lane in {"large", "full"}:
        selected.extend(group for group in all_groups if group["size"] == "large")

    normalized_changed = {
        path.replace("\\", "/").removeprefix("./") for path in changed_paths
    }
    affected_python_groups = [
        group for group in python_groups
        if any(
            fnmatch.fnmatchcase(path, pattern)
            for path in normalized_changed
            for pattern in group["trigger_inputs"]
        )
    ]
    if lane == "routine":
        selected.extend(
            group for group in affected_python_groups if group["size"] == "medium"
        )
    elif lane in {"medium", "major", "full"}:
        selected.extend(
            group for group in python_groups if group["size"] == "medium"
        )
    deferred_medium = (
        [group for group in affected_python_groups if group["size"] == "medium"]
        if lane == "small" else []
    )

    deferred_large = []
    if lane in {"routine", "small", "medium"}:
        deferred_large = [
            group for group in all_groups
            if group["size"] == "large" and group["module"] in affected
        ]
    if lane == "routine" and not affected:
        deferred_large = []

    native_selected = any(check["id"].startswith("ctest:") for check in selected)
    if native_selected:
        native_size = min(
            (check["size"] for check in selected if check["id"].startswith("ctest:")),
            key=("small", "medium", "large").index,
        )
        build_command = list(inventory["native"]["build_command"])
        additional_targets = sorted({
            target
            for check in selected
            if check["id"].startswith("ctest:")
            for target in check.get("build_targets", [])
        })
        build_command.extend(additional_targets)
        selected.append({
            "id": "native-build",
            "module": "native-build",
            "size": native_size,
            "reason": "Refreshes configured native test/game targets before CTest; build time counts in this lane.",
            "commands": [build_command],
            "inputs": list(inventory["native"]["build_inputs"]),
            "prerequisites": [native_config_fingerprint],
            "reuse": False,
        })
        for check in selected:
            if check["id"].startswith("ctest:"):
                check["inputs"] = sorted(
                    set(inventory["native"]["build_inputs"]) | set(check.get("inputs", []))
                )

    applicable_release_checks = []
    native_affected = native_global or bool(affected & allowed_modules)
    for check in inventory.get("release_checks", []):
        if check["module"] == "native-all" and native_affected:
            applicable_release_checks.append(check)
        elif check["module"] in affected:
            applicable_release_checks.append(check)
    deferred_release_checks = applicable_release_checks if lane not in {"large", "full"} else []
    required_release_checks = applicable_release_checks if lane in {"large", "full"} else []

    def check_order(check: dict[str, Any]) -> tuple[int, int, str]:
        if check["id"] == "fast-code":
            phase = 0
        elif check["id"] == "native-build":
            phase = 1
        else:
            phase = 2
        return ( ("small", "medium", "large").index(check["size"]), phase, check["id"] )

    selected.sort(key=check_order)
    for check in selected:
        check.setdefault("inputs", [])
        check.setdefault("prerequisites", [])
        check.setdefault("reason", "Selected by the validation inventory.")
    budgets = inventory["budgets_seconds"]
    aggregate_budget = (
        budgets["full_aggregate"] if lane == "full"
        else budgets["large"] if lane == "large"
        else budgets["small"] if lane == "small"
        else budgets["medium"] if lane == "medium"
        else budgets["routine_aggregate"]
    )
    return {
        "schema_version": 1,
        "lane": lane,
        "changed_paths": sorted(set(changed_paths)),
        "affected_modules": sorted(affected),
        "reasons": reasons,
        "checks": selected,
        "deferred_medium": deferred_medium,
        "deferred_large": deferred_large,
        "deferred_release_checks": deferred_release_checks,
        "required_release_checks": required_release_checks,
        "budgets_seconds": budgets,
        "aggregate_budget_seconds": aggregate_budget,
        "inventory_sha256": inventory_digest,
        "native_config_sha256": native_config_fingerprint,
        "release_registry_sha256": validate_release_registry(root, inventory),
        "preflight_seconds": time.monotonic() - started,
        "build_dir": build_dir,
    }


def _input_hashes(root: Path, patterns: Sequence[str]) -> list[list[str]]:
    hashed = []
    started = time.monotonic()
    last_progress = started
    file_count = 0
    for pattern in sorted(set(patterns)):
        matches = sorted(path for path in root.glob(pattern) if path.is_file())
        if not any(character in pattern for character in "*?[]") and not matches:
            hashed.append([pattern, "<missing>"])
        for path in matches:
            hashed.append([path.relative_to(root).as_posix(), sha256_file(path)])
            file_count += 1
            now = time.monotonic()
            if now - last_progress >= 30:
                print(
                    f"FINGERPRINT inputs: {file_count} files hashed; "
                    f"{now - started:.0f}s elapsed",
                    flush=True,
                )
                last_progress = now
    return hashed


def check_fingerprint(
    root: Path,
    check: dict[str, Any],
    inventory_sha256: str,
    prerequisites: dict[str, Any],
    input_hashes: list[list[str]] | None = None,
) -> str:
    """Bind a receipt to exact inputs, command, inventory, and prerequisites."""
    validation_sources = {}
    for source in ("script/ai_validation.py", "script/ai_check.py"):
        path = root / source
        validation_sources[source] = sha256_file(path) if path.is_file() else "<missing>"
    payload = {
        "id": check["id"],
        "size": check["size"],
        "module": check["module"],
        "commands": check.get("commands", []),
        "inputs": input_hashes if input_hashes is not None else _input_hashes(root, check.get("inputs", [])),
        "inventory_sha256": inventory_sha256,
        "prerequisites": prerequisites,
        "validation_sources": validation_sources,
    }
    return sha256_bytes(json.dumps(payload, sort_keys=True, separators=(",", ":")).encode())


def _receipt_path(receipt_dir: Path, check_id: str) -> Path:
    safe_name = re.sub(r"[^A-Za-z0-9_.-]", "_", check_id)
    return receipt_dir / f"{safe_name}.json"


def failure_first_order(checks: Sequence[dict[str, Any]], receipt_dir: Path) -> list[dict[str, Any]]:
    """Retry prior FAIL/UNRUN checks before pending checks in the same lane."""
    failed = set()
    for check in checks:
        path = _receipt_path(receipt_dir, check["id"])
        try:
            receipt = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        if receipt.get("status") in {"FAIL", "UNRUN", "TIMEOUT"}:
            failed.add(check["id"])
    by_id = {check["id"]: check for check in checks}
    ordered: list[dict[str, Any]] = []
    visited = set()

    def append_with_prerequisites(check: dict[str, Any]) -> None:
        if check["id"] in visited:
            return
        for prerequisite in check.get("prerequisites", []):
            if prerequisite in by_id:
                append_with_prerequisites(by_id[prerequisite])
        visited.add(check["id"])
        ordered.append(check)

    for check in sorted(checks, key=lambda item: item["id"] not in failed):
        append_with_prerequisites(check)
    return ordered


def _git_identity(root: Path) -> dict[str, str]:
    values = {}
    for key, args in {
        "head": ["rev-parse", "HEAD"],
        "index_tree": ["write-tree"],
    }.items():
        result = subprocess.run(
            ["git", *args], cwd=root, capture_output=True, text=True, check=False
        )
        values[key] = result.stdout.strip() if result.returncode == 0 else f"unavailable:{result.returncode}"
    return values


def _executable_identity(command: str) -> dict[str, str]:
    resolved = shutil.which(command) or command
    path = Path(resolved)
    return {
        "path": str(path.resolve()) if path.exists() else resolved,
        "sha256": sha256_file(path) if path.is_file() else "missing",
    }


def _prerequisites(root: Path, check: dict[str, Any], plan: dict[str, Any]) -> dict[str, Any]:
    environment = {
        key: value for key, value in os.environ.items()
        if key not in {"PWD", "OLDPWD", "SHLVL", "_", "CODEX_TOOL_CALL_ID"}
    }
    values: dict[str, Any] = {
        "repository": _git_identity(root),
        "environment_sha256": sha256_bytes(
            json.dumps(environment, sort_keys=True, separators=(",", ":")).encode()
        ),
        "platform": sys.platform,
        "python": {
            "executable": str(Path(sys.executable).resolve()),
            "version": sys.version,
            "sha256": sha256_file(Path(sys.executable)),
        },
    }
    if check["id"] == "native-build" or check["id"].startswith("ctest:"):
        native = plan["native_config_sha256"]
        values["configured_ctest"] = native
        values["cmake"] = _executable_identity("cmake")
        values["ninja"] = _executable_identity("ninja")
        build_state = plan.get("native_build_state")
        if build_state is None:
            ai_check_path = root / "script/ai_check.py"
            spec = importlib.util.spec_from_file_location("_validation_ai_check", ai_check_path)
            if spec is None or spec.loader is None:
                raise SelectionError("cannot load native build prerequisite checker")
            module = importlib.util.module_from_spec(spec)
            sys.modules[spec.name] = module
            spec.loader.exec_module(module)
            build_state = module.native_build_state(root, "ctest")
            if not build_state.get("available"):
                missing_outputs = build_state.get("missing_artifacts")
                only_missing_outputs = str(build_state.get("reason", "")).startswith(
                    "missing output targets: "
                )
                if check["id"] == "native-build" and missing_outputs and only_missing_outputs:
                    build_state = {**build_state, "available": True, "reason": "build outputs will be created by this check"}
                else:
                    raise SelectionError(
                        "native build prerequisites are incomplete: " + str(build_state.get("reason"))
                    )
            plan["native_build_state"] = build_state
        values["native_build_state"] = build_state
        if check["id"].startswith("ctest:"):
            values["ctest"] = _executable_identity("ctest")
            values["test_executables"] = {
                executable: _executable_identity(executable)
                for executable in check.get("executables", [])
            }
            build_dir = Path(plan["build_dir"]).resolve()
            artifacts = {}
            for relative in check.get("artifacts", []):
                artifact = (build_dir / relative).resolve()
                if not artifact.is_relative_to(build_dir):
                    raise SelectionError(f"CTest artifact escapes the configured build tree: {relative}")
                if not artifact.is_file():
                    raise SelectionError(f"CTest prerequisite artifact is missing: {artifact}")
                artifacts[relative] = {
                    "sha256": sha256_file(artifact),
                    "size": artifact.stat().st_size,
                }
            if artifacts:
                values["test_artifacts"] = artifacts
    return values


def _terminate_process(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    try:
        if os.name == "nt":
            subprocess.run(
                ["taskkill", "/T", "/F", "/PID", str(process.pid)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )
        else:
            os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        try:
            if os.name == "nt":
                process.kill()
            else:
                os.killpg(process.pid, signal.SIGKILL)
        except OSError:
            process.kill()
        process.wait()


def _run_command(
    root: Path,
    command: Sequence[str],
    timeout: float,
    log_path: Path,
    check_id: str,
) -> tuple[int, float, bool]:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    print(
        f"START {check_id}: {' '.join(command)}; budget {timeout:.1f}s; "
        "next liveness update within 30s",
        flush=True,
    )
    creationflags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    try:
        process = subprocess.Popen(
            list(command),
            cwd=root,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
            start_new_session=os.name != "nt",
            creationflags=creationflags,
        )
    except OSError as error:
        log_path.write_text(str(error) + "\n", encoding="utf-8")
        return 127, time.monotonic() - started, False
    output_queue: queue.Queue[str | None] = queue.Queue()

    def read_output() -> None:
        assert process.stdout is not None
        for line in process.stdout:
            output_queue.put(line)
        output_queue.put(None)

    reader = threading.Thread(target=read_output, daemon=True)
    reader.start()
    timed_out = False
    done_reading = False
    last_progress = started
    with log_path.open("w", encoding="utf-8") as log:
        while process.poll() is None or not done_reading:
            remaining = timeout - (time.monotonic() - started)
            if remaining <= 0 and process.poll() is None:
                timed_out = True
                _terminate_process(process)
            try:
                line = output_queue.get(timeout=min(1.0, max(0.05, remaining)))
            except queue.Empty:
                line = ""
            if line is None:
                done_reading = True
            elif line:
                log.write(line)
                log.flush()
                print(line, end="", flush=True)
            now = time.monotonic()
            if now - last_progress >= 30 and process.poll() is None:
                print(
                    f"RUNNING {check_id}: {now - started:.0f}s elapsed; "
                    f"{max(0, timeout - (now - started)):.1f}s remain in this check",
                    flush=True,
                )
                last_progress = now
        reader.join(timeout=1)
    if process.stdout is not None:
        process.stdout.close()
    return (124 if timed_out else process.returncode or 0), time.monotonic() - started, timed_out


def _write_receipt(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def _record_deferred_checks(root: Path, plan: dict[str, Any]) -> float:
    """Write truthful, non-reusable receipts for checks moved to a later lane."""
    all_started = time.monotonic()
    receipt_dir = root / RECEIPT_DIR
    for check in plan.get("deferred_medium", []):
        check_started = time.monotonic()
        prerequisites = _prerequisites(root, check, plan)
        repository = prerequisites.get("repository", {})
        if any(
            not repository.get(key) or repository[key].startswith("unavailable:")
            for key in ("head", "index_tree")
        ):
            raise SelectionError(f"cannot record deferred check without immutable Git identity: {check['id']}")
        input_hashes = _input_hashes(root, check.get("inputs", []))
        fingerprint = check_fingerprint(
            root, check, plan["inventory_sha256"], prerequisites, input_hashes
        )
        deferred_id = f"deferred-{check['id']}"
        elapsed = time.monotonic() - check_started
        _write_receipt(_receipt_path(receipt_dir, deferred_id), {
            "schema_version": 1,
            "id": deferred_id,
            "status": "DEFERRED",
            "lane": "small",
            "module": check["module"],
            "required_lane": "medium",
            "reason": "This medium Python group is preserved for the medium lane and was not run here.",
            "source_head": prerequisites.get("repository", {}).get("head"),
            "inventory_sha256": plan["inventory_sha256"],
            "fingerprint": fingerprint,
            "prerequisites": prerequisites,
            "commands": check.get("commands", []),
            "inputs": input_hashes,
            "duration_seconds": round(elapsed, 3),
            "reusable": False,
            "deferred_at_utc": datetime.now(timezone.utc).isoformat(),
        })
        print(
            f"DEFERRED {check['id']}: medium lane required; "
            f"source/configuration fingerprint {fingerprint[:12]}",
            flush=True,
        )
    return time.monotonic() - all_started


def run_plan(root: Path, plan: dict[str, Any]) -> int:
    """Run selected checks under per-lane and aggregate wall-clock limits."""
    receipt_dir = root / RECEIPT_DIR
    log_dir = root / LOG_DIR
    checks = plan["checks"]
    started = time.monotonic()
    preflight_size = min(
        (check["size"] for check in checks),
        key=("small", "medium", "large").index,
        default="small",
    )
    lane_elapsed = {"small": 0.0, "medium": 0.0, "large": 0.0}
    lane_elapsed[preflight_size] = plan["preflight_seconds"]
    total_budget = plan["aggregate_budget_seconds"]
    total_used = plan["preflight_seconds"]
    statuses: dict[str, str] = {}
    stop = False
    deferred_duration = _record_deferred_checks(root, plan)
    lane_elapsed["small"] += deferred_duration
    total_used += deferred_duration
    stop_reason = None
    if (
        lane_elapsed["small"] > plan["budgets_seconds"]["small"]
        or total_used > total_budget
    ):
        stop = True
        stop_reason = "small-lane or aggregate budget exhausted while recording deferred input identity"
    for check in failure_first_order(checks, receipt_dir):
        if stop:
            break
        size = check["size"]
        lane_cap = plan["budgets_seconds"][size]
        prerequisite_ids = check.get("prerequisites", [])
        if "native-build" in prerequisite_ids and statuses.get("native-build") not in {"PASS", "REUSED"}:
            reason = "native build prerequisite did not pass in this invocation"
            _write_receipt(_receipt_path(receipt_dir, check["id"]), {
                "schema_version": 1, "id": check["id"], "status": "UNRUN", "reason": reason,
            })
            statuses[check["id"]] = "UNRUN"
            stop = True
            print(f"UNRUN {check['id']}: {reason}", flush=True)
            continue
        check_started = time.monotonic()
        prerequisites = _prerequisites(root, check, plan)
        input_hashes = _input_hashes(root, check.get("inputs", []))
        fingerprint = check_fingerprint(
            root, check, plan["inventory_sha256"], prerequisites, input_hashes
        )
        identity_duration = time.monotonic() - check_started
        lane_elapsed[size] += identity_duration
        total_used += identity_duration
        receipt_file = _receipt_path(receipt_dir, check["id"])
        try:
            previous = json.loads(receipt_file.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            previous = {}
        if (
            check.get("reuse", True)
            and previous.get("status") == "PASS"
            and previous.get("fingerprint") == fingerprint
        ):
            statuses[check["id"]] = "REUSED"
            print(f"REUSE {check['id']}: source, configuration, command, and prerequisites match", flush=True)
            continue
        timeout = min(lane_cap - lane_elapsed[size], total_budget - total_used)
        if timeout <= 0:
            reason = f"{size} or aggregate validation budget exhausted"
            _write_receipt(receipt_file, {
                "schema_version": 1, "id": check["id"], "status": "TIMEOUT",
                "reason": reason, "fingerprint": fingerprint,
            })
            statuses[check["id"]] = "TIMEOUT"
            stop = True
            print(f"TIMEOUT {check['id']}: {reason}", flush=True)
            continue
        overall_check_started = time.monotonic()
        return_code = 0
        timed_out = False
        output_logs = []
        for index, raw_command in enumerate(check.get("commands", [])):
            command = [
                str(argument)
                .replace("{python}", sys.executable)
                .replace("{build_dir}", plan["build_dir"])
                for argument in raw_command
            ]
            remaining = timeout - (time.monotonic() - overall_check_started)
            if remaining <= 0:
                return_code, timed_out = 124, True
                break
            log_path = log_dir / f"{_receipt_path(Path(), check['id']).stem}-{index + 1}.log"
            return_code, _, timed_out = _run_command(
                root, command, remaining, log_path, check["id"]
            )
            output_logs.append(str(log_path.relative_to(root)))
            if return_code:
                break
        command_duration = time.monotonic() - overall_check_started
        duration = identity_duration + command_duration
        lane_elapsed[size] += command_duration
        total_used += command_duration
        status_name = "TIMEOUT" if timed_out else "PASS" if return_code == 0 else "FAIL"
        receipt = {
            "schema_version": 1,
            "id": check["id"],
            "status": status_name,
            "lane": size,
            "module": check["module"],
            "source_head": prerequisites.get("repository", {}).get("head"),
            "inventory_sha256": plan["inventory_sha256"],
            "fingerprint": fingerprint,
            "prerequisites": prerequisites,
            "commands": check.get("commands", []),
            "inputs": input_hashes,
            "log_files": output_logs,
            "duration_seconds": round(duration, 3),
            "return_code": return_code,
            "started_utc": datetime.fromtimestamp(time.time() - duration, timezone.utc).isoformat(),
            "finished_utc": datetime.now(timezone.utc).isoformat(),
        }
        _write_receipt(receipt_file, receipt)
        statuses[check["id"]] = status_name
        print(f"{status_name} {check['id']}: {duration:.1f}s", flush=True)
        if check["id"] == "native-build" and return_code == 0:
            plan.pop("native_build_state", None)
        if return_code:
            stop = True
    if stop:
        for check in checks:
            if check["id"] in statuses:
                continue
            receipt_file = _receipt_path(receipt_dir, check["id"])
            _write_receipt(receipt_file, {
                "schema_version": 1,
                "id": check["id"],
                "status": "UNRUN",
                "reason": stop_reason or "A prior selected check failed or exhausted a lane budget; retry failures first.",
            })
            statuses[check["id"]] = "UNRUN"
    elapsed = time.monotonic() - started + plan["preflight_seconds"]
    print(
        f"LANE RESULT {plan['lane']}: {elapsed:.1f}s elapsed, "
        f"{total_budget}s aggregate limit; "
        + ("failed or incomplete" if stop else "all selected checks passed or reused")
        + ("; medium Python checks remain deferred" if plan.get("deferred_medium") else ""),
        flush=True,
    )
    if plan.get("required_release_checks"):
        print(
            "INCOMPLETE delegated release gates: "
            + ", ".join(check["id"] for check in plan["required_release_checks"])
            + "; run the matching ai_check.py candidate gate",
            flush=True,
        )
    return int(stop or bool(plan.get("required_release_checks")))


def changed_paths_from_git(root: Path) -> list[str]:
    diff = subprocess.run(
        ["git", "diff", "HEAD", "--name-only", "-z"],
        cwd=root,
        capture_output=True,
        check=False,
    )
    untracked = subprocess.run(
        ["git", "ls-files", "--others", "--exclude-standard", "-z"],
        cwd=root,
        capture_output=True,
        check=False,
    )
    if diff.returncode or untracked.returncode:
        raise SelectionError("cannot enumerate changed paths from Git")
    paths = diff.stdout.split(b"\0") + untracked.stdout.split(b"\0")
    return sorted({path.decode(errors="surrogateescape") for path in paths if path})


def display_plan(plan: dict[str, Any]) -> dict[str, Any]:
    """Keep plan output useful when a module contains hundreds of CTest rows."""
    checks = []
    for check in plan["checks"]:
        displayed = {
            key: value for key, value in check.items()
            if key not in {"test_names", "commands"}
        }
        if check["id"].startswith("ctest:"):
            displayed["runner"] = "ctest: exact configured test names"
        else:
            displayed["commands"] = check.get("commands", [])
        checks.append(displayed)
    return {**{key: value for key, value in plan.items() if key != "checks"}, "checks": checks}


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--lane", choices=LANES, default="routine")
    parser.add_argument("--paths", nargs="*", help="changed paths; defaults to staged, unstaged, and untracked paths")
    parser.add_argument("--run", action="store_true", help="execute the selected checks")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        selection_started = time.monotonic()
        paths = args.paths if args.paths is not None else changed_paths_from_git(root)
        plan = select_plan(root, paths, lane=args.lane)
        plan["preflight_seconds"] = time.monotonic() - selection_started
        print(json.dumps(display_plan(plan), indent=2, sort_keys=True))
        if plan["deferred_medium"] and not args.run:
            names = ", ".join(check["id"] for check in plan["deferred_medium"])
            print(f"DEFERRED medium Python checks: {names}; run --lane medium", file=sys.stderr)
        if plan["deferred_large"]:
            print("DEFERRED large checks: use --lane full at the release-candidate boundary", file=sys.stderr)
        if plan["deferred_release_checks"]:
            names = ", ".join(check["id"] for check in plan["deferred_release_checks"])
            print(f"DEFERRED release gates owned by ai_check.py: {names}", file=sys.stderr)
        return run_plan(root, plan) if args.run else 0
    except (OSError, SelectionError, ValueError, KeyError, TypeError) as error:
        print(f"VALIDATION BLOCKED: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
