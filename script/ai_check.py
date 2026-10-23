#!/usr/bin/env python3
"""Offline validation entry point for the repository's AI-assisted branches."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import signal
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


LOG_DIR = Path("build/ai-checks")
RECEIPT_SCHEMA_VERSION = 2
S1_OWNER_EXCEPTION_SCHEMA = "mc.s1-owner-accepted-gameplay-exception.v1"
S1_OWNER_REQUEST_PATH = "~/.codex/task-evidence/mc-s1-20261023/owner-release-acceptance/request.md"
S1_OWNER_REQUEST_SHA256 = "b55134d42a04a746ca862d4b12ba847cb8e0e73ac88ae1ca787b13daf6ebe403"
S1_OWNER_SOURCE_INPUT_SHA256 = "83da03165eea8b14d48398b8ca314b0ddcb3d74fa6b446b998f819a2538c3e9d"
S1_OWNER_SOURCE_REUSE_CURRENT_SHA256 = 'fd08d7464b1ec7c80d4a494e8e9e471baafcd7f8e85a1220923d453945b93aa1'
S1_OWNER_SOURCE_REUSE_V3_CURRENT_SHA256 = '75b2a0951e88e1ebc38a6776aaf191071ab1445a41d6c500c2cf438e12d1d641'
S1_OWNER_SOURCE_REUSE_V3_CERTIFICATE = {'application_behavior_equivalence': 'all application inputs are byte-identical to the accepted candidate '
                                     'after the two exact EOF normalizations and three pinned '
                                     'version-identity literal transforms',
 'application_current_raw_input_sha256': 'c5ead3b8ff782614e4b4ffbada4eb1471d214aa367f112092796b9fd27773b39',
 'application_input_count': 233,
 'application_source_input_sha256': 'dcc33a5b3f062aed0e67ae02d0e83d37f040ab00ed44209897a427bfda4d9bee',
 'application_unchanged_input_count': 229,
 'capture_source_input_sha256': '83da03165eea8b14d48398b8ca314b0ddcb3d74fa6b446b998f819a2538c3e9d',
 'current_source_input_sha256': '75b2a0951e88e1ebc38a6776aaf191071ab1445a41d6c500c2cf438e12d1d641',
 'normalized_files': [{'accepted_sha256': '37074292464f75c2ca6d96747202f3a3bada734541817a558218fd115fdefd99',
                       'accepted_trailing_lf_count': 3,
                       'current_sha256': 'eda9f4ef465be27b97a0cacc6f59f2b9775517a960acfd0e16f4e9800cf6aaaf',
                       'current_trailing_lf_count': 1,
                       'input_class': 'application',
                       'path': 'src/shared/corelang/Execution.cpp'},
                      {'accepted_sha256': 'dadb3a9daf5563839e3128bb25884da3b5f071c991826ac6c2307f4023fffb4c',
                       'accepted_trailing_lf_count': 2,
                       'current_sha256': '36b3f02c9ce3ff7fd57d43b6cd4f78f962e2fd1077ab08a2368a89601f9c78c0',
                       'current_trailing_lf_count': 1,
                       'input_class': 'test',
                       'path': 'tests/acceptance/column_map_replay_tests.cpp'}],
 'rebuilt_application': {'application_executable': {'path': '~/.codex/task-evidence/mc-s1-20261021/evidence-boundaries/build/mc_main',
                                                    'sha256': '0aacbd2ff732cc328e29b38509161a54c99b60706fcab0b98cf40495bc26fba7'},
                         'build_log': {'path': '~/.codex/task-evidence/mc-s1-20261023/release-freeze-01/deferred-lod/accepted-build.log',
                                       'sha256': '4ca42cfd59f341f93887df2b71d23f04558960d738e8f88f025e759eb0a282b7'},
                         'freeze_receipt': {'path': '~/.codex/task-evidence/mc-s1-20261023/release-freeze-01/deferred-lod/FINAL-FREEZE-RECEIPT.json',
                                            'sha256': '78ca7ea1d3d3f01917e754165dc89791465ea947fd939238f1d54cd9790bfa0a'}},
 'schema': 'mc.s1-source-reuse-certificate.v3',
 'source_input_count': 908,
 'test_fixture_files': [{'accepted_sha256': 'c4bafd8890b2eac5051789e0534b83ec1578be6fc76e220769abf840a1b53a2f',
                         'current_sha256': '4b96d636d353fb9c85e5b2817ee27dc861fde123ceee2af550da19ff8c9b448c',
                         'path': 'tests/acceptance/adverse_network_tests.cpp'},
                        {'accepted_sha256': '807e6e4cf308ad154e30ea139514ace679ac517d53867134f98861f8b7263aca',
                         'current_sha256': '98202508eb4a28708e76302afc644a2dfc78327065ae87ff840f72399eb40d85',
                         'path': 'tests/client/player_client_focus_tests.cpp'},
                        {'accepted_sha256': '5b23541ce5b08c565b448fd59b303513c67d06a923988fea8ff74fd360daf540',
                         'current_sha256': '2311f8044975f27a2fb516496ca527cb82185fb11683f47ca6d9d0bb7e729dc7',
                         'path': 'tests/client/renderer_smoke_tests.cpp'},
                        {'accepted_sha256': 'f8dc18c19d19e23ff5cf2a4279cd6aa86756e911ce0eaa02bcf0a8168e214a90',
                         'current_sha256': '96d578b3d79e52893ba561ee4c1e3090efb8671f2f0faffe72dedca1dd25c1ab',
                         'path': 'tests/core/game_server_preview_tests.cpp'}],
 'test_input_count': 675,
 'test_unchanged_input_count': 670,
 'unchanged_input_count': 899,
 'version_identity_rebuild': {'behavior_input_sha256_after_exact_identity_normalization': 'dcc33a5b3f062aed0e67ae02d0e83d37f040ab00ed44209897a427bfda4d9bee',
                              'build_log': {'path': '~/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/version-identity-build-01/build.log',
                                            'sha256': '6b21f90d2cfa6ee18a5cf72039392f0bff4ab970b1a7aaa8526a6bbb7bbb0dee'},
                              'formatted_version_probe': {'executable': {'path': '~/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/version-identity-build-01/version_identity_probe',
                                                                         'sha256': '1cd10ba5acd4f97121293c957fced4ecc5065847628388adeb8fc706139d81b3'},
                                                          'output': '0.1.1:1 BasicBlocks',
                                                          'run_log': {'path': '~/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/version-identity-build-01/version_identity_probe_run.log',
                                                                      'sha256': 'a65036a25541cf4ee59be2d12f521972f4aa5c701822b3ffdb5f34f86b50c16d'}},
                              'global_symbols': {'count': 3407,
                                                 'identical_to_owner_tested_application': True,
                                                 'sha256': 'df6cdfac05d588cc32b8d41dfb98b6070e4b6a0b4c0b3d43e3fc4d6d9ac7ea6d'},
                              'owner_tested_application_sha256': '0aacbd2ff732cc328e29b38509161a54c99b60706fcab0b98cf40495bc26fba7',
                              'receipt': {'path': '~/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/version-identity-build-01/version-identity-build.json',
                                          'sha256': 'bc0d803ae6e68914bc214f319c8e0951b4e26d5e23d222510f8e37e004eb3abf'},
                              'schema': 'mc.s1-version-identity-build.v1',
                              'source_input_sha256': '75b2a0951e88e1ebc38a6776aaf191071ab1445a41d6c500c2cf438e12d1d641',
                              'version_identity_application': {'path': '~/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/metadata-work/build/s1-version-identity-release/mc_main',
                                                               'sha256': '6a5b2f3174eb5715add8de61e061684f0a00ff1616f9285f350c140f569252bc'}},
 'version_only_files': [{'accepted_sha256': '317bb3826155f357af28ac04b254662fefb14dbe5282c7bed354743e77c8a596',
                         'current_sha256': '6e285b7a9f39c9b65af1fbb7442484f888d6a1eef352f7930d9767844c381297',
                         'input_class': 'application',
                         'literal_changes': [{'from': 'VERSION 0.1.0\n', 'to': 'VERSION 0.1.1\n'}],
                         'path': 'CMakeLists.txt'},
                        {'accepted_sha256': '5e5be64a6f6b8574da372911b28194ab468ce88c29ece5491a564167dceacfb1',
                         'current_sha256': '46eec31959f1b7828cfa62cfeadf3689bcd0c49030fa81e99c70b73e36031cb8',
                         'input_class': 'application',
                         'literal_changes': [{'from': 'MINOR_VERSION_NAME{ "Initiation" }',
                                              'to': 'MINOR_VERSION_NAME{ "BasicBlocks" }'},
                                             {'from': '.minor = 0,', 'to': '.minor = 1,'},
                                             {'from': '.patch = 7,', 'to': '.patch = 1,'}],
                         'path': 'src/shared/include/shared/ProjectInfo.hpp'},
                        {'accepted_sha256': 'c4199f22a2a4d7fbb128e1b861b381b091eeeb7336c5909d4ae16335b1563724',
                         'current_sha256': '00b4e1010a1f5e8a7ea196a69c295eee5240849c22312b1b09855f1d0780c945',
                         'input_class': 'application',
                         'literal_changes': [{'from': '"version-string": "0.1.0"',
                                              'to': '"version-string": "0.1.1"'}],
                         'path': 'vcpkg.json'}]}
S1_OWNER_SOURCE_REUSE_CERTIFICATE = {'acceptance_boundary': 'Fresh Release build and headless CLI scenario evidence only; no full gameplay, '
                        'rendering, performance or new owner acceptance. Independent platform, analysis and '
                        'release gates remain required.',
 'application_behavior_equivalence': 'not claimed; semantic preservation requires exact-delta review and '
                                     'current correctness evidence',
 'capture_source_input_sha256': '83da03165eea8b14d48398b8ca314b0ddcb3d74fa6b446b998f819a2538c3e9d',
 'changed_input_count': 141,
 'current_source_input_sha256': 'fd08d7464b1ec7c80d4a494e8e9e471baafcd7f8e85a1220923d453945b93aa1',
 'headless_runtime': {'path': '/Users/core-just/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/fresh-runtime-v4-01/headless-runtime-final-receipt.json',
                      'sha256': '5c0c0dd73dd3002bf5f685da9c52d033768b632268b05db6c68cafd81a16dea2',
                      'size': 1550},
 'manifest': {'path': '/Users/core-just/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/fresh-runtime-v4-01/source-provenance.v4.json',
              'sha256': 'a5665d3ba9e38aca35f815312b05bff800f203195ea6986e9dedb779237b0125',
              'size': 281090},
 'new_binary_owner_acceptance': False,
 'provenance_mode': 'review-supported-analysis-delta',
 'release_rebuild': {'path': '/Users/core-just/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/fresh-runtime-v4-01/release-rebuild-final-receipt.json',
                     'sha256': '91de8a838122f27975160b43b08188cefbc6021a09bddebb44afa3e142dcfb8f',
                     'size': 1561},
 'runtime_evidence_scope': 'historical-owner-accepted-capture',
 'schema': 'mc.s1-analysis-source-provenance.v4',
 'semantic_review': {'path': '/Users/core-just/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/review-artifacts/analysis-source-review-v8.json',
                     'sha256': '55946f17b05153a3b6edf3c04dbb4caefaf8695f354b9d95c5fe6a049e5819c7',
                     'size': 5893},
 'source_input_count': 908}
S1_OWNER_AGGREGATE_SHA256 = "abcda853c0a6af2b87526723280055ab1040c38f6c3ac265c3c538e54cdbfd06"
S1_OWNER_RAW_REPORT_SHA256 = "94140622009202bad4ee05b98952fec86d41aecd9f8cb8a732073ab0f5fd102d"
S1_OWNER_MAC_EXECUTABLE_SHA256 = "0aacbd2ff732cc328e29b38509161a54c99b60706fcab0b98cf40495bc26fba7"
S1_OWNER_WINDOWS_EXECUTABLE_SHA256 = "cba2572c3cd47b9da82a2ad2ab1f958231ea20b57b9a6ddad17107013d89402b"
S1_OWNER_CAPTURE_CANDIDATE = {
    "head": "bf5047ac52f397179e8467998f6fe67604a6e5f5",
    "tree": "6c2f5e415748a4d3803207c74b3e09cbe911a300",
}
S1_OWNER_PROOF_DISPOSITIONS = {
    "macos": {"performance": "failed", "terrain-coverage": "unqualified"},
    "windows": {"performance": "unqualified", "terrain-coverage": "unqualified"},
}
S1_OWNER_KNOWN_FAILURE = {
    "report_failure": "game benchmark did not complete the scripted full-game workload",
    "in_budget_time_ns": 14_018_509_225,
    "measured_time_ns": 30_000_000_000,
    "required_in_budget_permille": 990,
    "target_frame_duration_ns": 8_333_333,
    "movement_playable_sphere_complete": False,
    "movement_visual_lod_samples_complete": False,
    "ready_prompt_frame_offset_ns": 3_434_526_750,
    "playable_sphere_first_ready_offset_ns": 14_323_644_834,
    "initial_deadline_missed": False,
    "playable_deadline_missed": False,
}
SHARED_PYTHON_SOURCES = {
    "ai_check.py", "ai_commit.py", "ai_docs.py", "ai_history.py", "ai_plan.py",
    "ai_publish.py", "ai_run.py", "ai_setup.py", "ai_tasks.py", "ai_windows_job.py",
}
PYTHON_TEST_TIMEOUT = 420
WINDOWS_PYTHON_TEST_TIMEOUT = 420
MAX_PHASE_TIMEOUT = 600
CTEST_PHASE_TIMEOUT = 1800
TOTAL_TIMEOUT = 3900
BUILD_PHASE_TIMEOUT = 600
TERMINATION_GRACE_SECONDS = 10
GOVERNED_PREFIXES = ("src/", "tests/", "docs/", "script/", ".githooks/", ".github/", ".codex/", ".agents/", "cmake/")
PYTHON_WORKFLOW_PREFIXES = ("docs/ai/", "docs/code/", "script/", ".agents/", ".githooks/")
NATIVE_BUILD_PREFIXES = ("src/", "tests/", "cmake/", "android/")
NATIVE_BUILD_FILES = {
    "CMakeLists.txt", "CMakePresets.json", "vcpkg.json", "vcpkg-configuration.json",
}
BUILD_INPUT_PREFIXES = (
    "android/", "assets/", "cmake/", "scenarios/", "src/", "tests/",
    "script/ci/vcpkg-overlays/", "script/ci/vcpkg-triplets/",
)
BUILD_INPUT_FILES = NATIVE_BUILD_FILES | {
    "dependencies.lock.json",
    "script/ci/cppcheck_gate.py",
    "script/ci/iwyu_gate.py",
}
BUILD_PHASE_COMMANDS = {
    "build": ["cmake", "--build", "--preset", "debug"],
    "ctest": [
        "ctest", "--preset", "debug", "--output-on-failure", "--no-tests=error",
        "--timeout", "240",
    ],
}
NATIVE_RECEIPT_PHASES = {"build", "ctest", "extra-renderer-smoke"}
GOVERNED_FILES = {
    ".gitattributes", ".gitignore", "AGENTS.md", "CLAUDE.md", "CMakeLists.txt", "CMakePresets.json", "README.md",
    "publish.py", "vcpkg.json", "vcpkg-configuration.json",
}
DEFAULT_PUBLISHER_FAILURES = {
    "no unstaged or uncommitted changes",
    "version history folder exists",
    "version history markdown file exists",
    "required headings '# Overview' and '# Snapshots' present",
    "Overview contains text",
    "snapshots 1..SnapshotIndex exist with correct format",
    "snapshot sections are not empty",
    "snapshot dates are non-decreasing",
    "latest snapshot date is today",
}


@dataclass
class PhaseResult:
    name: str
    command: Sequence[str]
    returncode: int
    output: str
    timed_out: bool = False
    allowed_failure: bool = False
    reused: bool = False
    skipped: bool = False
    duration_seconds: float = 0.0


def python_test_timeout() -> int:
    return WINDOWS_PYTHON_TEST_TIMEOUT if platform.system() == "Windows" else PYTHON_TEST_TIMEOUT


def phase_timeout_cap(name: str) -> int:
    return CTEST_PHASE_TIMEOUT if name == "ctest" else MAX_PHASE_TIMEOUT


def command_output(root: Path, command: Sequence[str]) -> str:
    completed = subprocess.run(command, cwd=root, text=True, capture_output=True, check=False)
    if completed.returncode:
        raise RuntimeError(completed.stderr.strip() or " ".join(command))
    return completed.stdout


def git_output(root: Path, *arguments: str) -> str:
    return command_output(root, ["git", *arguments]).strip()


def merge_head(root: Path) -> str | None:
    location = Path(git_output(root, "rev-parse", "--git-path", "MERGE_HEAD"))
    target = location if location.is_absolute() else root / location
    try:
        heads = [line for line in target.read_text(encoding="utf-8").splitlines() if line]
    except OSError:
        return None
    if len(heads) != 1:
        raise ValueError("snapshot source binding requires exactly one pending merge parent")
    return heads[0]


def require_snapshot_source_commit(root: Path, source: str) -> str:
    if not re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", source):
        raise ValueError("snapshot source must be a full immutable commit ID")
    resolved = git_output(root, "rev-parse", "--verify", f"{source}^{{commit}}")
    if resolved != source:
        raise ValueError("snapshot source does not resolve to the requested commit")
    return source


def resolve_promotion_source(root: Path) -> str | None:
    resolver = root / "script" / "ai_publish.py"
    if not resolver.is_file():
        subject = git_output(root, "show", "-s", "--format=%s", "HEAD")
        if subject.startswith("Promote MC-AI-"):
            raise ValueError("cannot validate the HEAD promotion source without ai_publish.py")
        return None
    completed = subprocess.run(
        [sys.executable, str(resolver), "--root", str(root), "resolve-date-source"],
        cwd=root,
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise ValueError(completed.stderr.strip() or "cannot validate the HEAD promotion source")
    source = completed.stdout.strip()
    return require_snapshot_source_commit(root, source) if source else None


def snapshot_source_for_check(
    root: Path,
    explicit_source: str | None,
    *,
    candidate: bool,
    strict: bool,
    snapshot_task_id: str | None = None,
) -> str | None:
    if explicit_source is not None:
        source = require_snapshot_source_commit(root, explicit_source)
        if candidate:
            branch = git_output(root, "branch", "--show-current")
            head = git_output(root, "rev-parse", "HEAD")
            pending_source = merge_head(root)
            supported_source_branch = branch == "ai-dev" or (
                branch.startswith("codex/ai-") and len(branch) > len("codex/ai-")
            )
            if supported_source_branch and pending_source is None:
                if head == source:
                    return source
                if (
                    git_output(root, "show", "-s", "--format=%P", source) == head
                    and git_output(root, "rev-parse", f"{source}^{{tree}}")
                        == git_output(root, "write-tree")
                    and not git_output(root, "diff", "--name-only", "--")
                    and not git_output(root, "ls-files", "--others", "--exclude-standard")
                ):
                    return source
                if branch == "ai-dev" and snapshot_task_id:
                    resolver = root / "script" / "ai_publish.py"
                    completed = subprocess.run(
                        [
                            sys.executable, str(resolver), "--root", str(root),
                            "resolve-tagged-source", "--expected-head", head,
                            "--task-id", snapshot_task_id,
                        ],
                        cwd=root, text=True, capture_output=True, check=False,
                    )
                    if completed.returncode:
                        raise ValueError(
                            completed.stderr.strip()
                            or "cannot validate the tagged snapshot source"
                        )
                    if completed.stdout.strip() == source:
                        return source
            if branch == "ai-main" and pending_source == source:
                return source
            raise ValueError(
                "snapshot source does not match the clean source HEAD or pending promotion merge"
            )
        if strict:
            if resolve_promotion_source(root) != source:
                raise ValueError("snapshot source does not match the validated HEAD promotion")
        else:
            raise ValueError("snapshot source binding requires candidate or strict release checks")
        return source
    if strict:
        return resolve_promotion_source(root)
    return None


def repository_root() -> Path:
    return Path(__file__).resolve().parents[1]


def governed(path: str) -> bool:
    return path in GOVERNED_FILES or path.startswith(GOVERNED_PREFIXES)


def metadata_path(path: str) -> bool:
    return path.startswith("docs/") or path in {"README.md", "AGENTS.md"}


def native_build_path(path: str) -> bool:
    return path.startswith(NATIVE_BUILD_PREFIXES) or path in NATIVE_BUILD_FILES


def changed_paths(root: Path, cached: bool) -> set[str]:
    command = ["git", "diff"]
    if cached:
        command.append("--cached")
    command.extend(["--name-only", "-z"])
    completed = subprocess.run(command, cwd=root, capture_output=True, check=False)
    if completed.returncode:
        raise RuntimeError(completed.stderr.decode(errors="replace").strip() or " ".join(command))
    return {path.decode(errors="surrogateescape") for path in completed.stdout.split(b"\0") if path}


def untracked_paths(root: Path) -> set[str]:
    command = ["git", "ls-files", "--others", "--exclude-standard", "-z"]
    completed = subprocess.run(command, cwd=root, capture_output=True, check=False)
    if completed.returncode:
        raise RuntimeError(completed.stderr.decode(errors="replace").strip() or " ".join(command))
    return {path.decode(errors="surrogateescape") for path in completed.stdout.split(b"\0") if path}


def repository_paths(root: Path) -> list[str]:
    """Return tracked and untracked files used to fingerprint a check input."""
    tracked = subprocess.run(
        ["git", "ls-files", "-z"], cwd=root, capture_output=True, check=False
    )
    if tracked.returncode:
        raise RuntimeError(tracked.stderr.decode(errors="replace").strip() or "git ls-files")
    names = {path.decode(errors="surrogateescape") for path in tracked.stdout.split(b"\0") if path}
    names.update(
        path for path in untracked_paths(root)
        if not path.startswith("build/ai-checks/")
        and "/__pycache__/" not in f"/{path}"
        and not path.endswith(".pyc")
    )
    return sorted(names)


def phase_relevant_paths(root: Path, name: str) -> list[str]:
    paths = repository_paths(root)
    if name in {"build", "ctest"}:
        relevant = [
            path for path in paths
            if path.startswith(BUILD_INPUT_PREFIXES)
            or path in BUILD_INPUT_FILES
            or path.endswith("/CMakeLists.txt")
            or path.endswith(".cmake")
        ]
        if (root / "CMakeUserPresets.json").is_file():
            relevant.append("CMakeUserPresets.json")
        return sorted(set(relevant))
    if name == "extra-renderer-smoke":
        relevant = {
            path for path in paths
            if path.startswith(BUILD_INPUT_PREFIXES)
            or path in BUILD_INPUT_FILES
            or path.endswith("/CMakeLists.txt")
            or path.endswith(".cmake")
            or path in {"script/ai_checks.json", "script/ai_renderer_smoke.py"}
        }
        if (root / "CMakeUserPresets.json").is_file():
            relevant.add("CMakeUserPresets.json")
        return sorted(relevant)
    if name in {"python-tests", "python-tests-fast"}:
        # The suite is a repository-policy harness: tests may inspect any
        # tracked or discovered repository content, including release docs.
        # Keep a separate receipt namespace for the inventory-filtered fast run.
        tests_root = root / "script/tests"
        if tests_root.is_dir():
            paths.extend(
                target.relative_to(root).as_posix()
                for target in tests_root.rglob("test*.py")
                if target.is_file()
            )
        return sorted(set(paths))
    if name in {"backlog", "current-plan"}:
        return [
            path for path in paths
            if path.startswith("docs/ai/")
            or path in {"script/ai_tasks.py", "script/ai_plan.py", "script/ai_history.py"}
        ]
    # Documentation validation reads the complete module map and source state;
    # diff checks also observe the complete index/worktree.
    return paths


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_tree(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    count = 0
    last_progress = time.monotonic()
    visited_directories: set[Path] = set()
    for directory, child_directories, filenames in os.walk(path, followlinks=True):
        child_directories.sort()
        resolved_directory = Path(directory).resolve()
        if resolved_directory in visited_directories:
            child_directories.clear()
            continue
        visited_directories.add(resolved_directory)
        for child_directory in child_directories:
            target = Path(directory) / child_directory
            if target.is_symlink():
                relative = target.relative_to(path).as_posix()
                digest.update(relative.encode("utf-8", errors="surrogateescape"))
                digest.update(os.readlink(target).encode("utf-8", errors="surrogateescape"))
        for filename in sorted(filenames):
            target = Path(directory) / filename
            if not target.is_file():
                continue
            relative = target.relative_to(path).as_posix()
            digest.update(str(target.stat().st_mode & 0o7777).encode("ascii"))
            digest.update(relative.encode("utf-8", errors="surrogateescape"))
            digest.update(bytes.fromhex(sha256_file(target)))
            count += 1
            elapsed = time.monotonic() - last_progress
            if elapsed >= 30:
                print(
                    f"FINGERPRINT {path}: {count} dependency files hashed; "
                    f"{elapsed:.0f}s since the last progress signal",
                    flush=True,
                )
                last_progress = time.monotonic()
    return digest.hexdigest(), count


def parse_cmake_cache(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        name, value = line.split("=", 1)
        key, separator, _kind = name.partition(":")
        if separator:
            values[key] = value
    return values


def native_build_state(root: Path, name: str) -> dict[str, object]:
    preset = "renderer-smoke" if name == "extra-renderer-smoke" else "debug"
    build_root = root / "build" / preset
    cache_path = build_root / "CMakeCache.txt"
    state: dict[str, object] = {
        "preset": preset,
        "available": False,
        "reason": "CMakeCache.txt is missing",
    }
    if not cache_path.is_file():
        state["cache"] = "<missing>"
        return state

    values = parse_cmake_cache(cache_path)
    cache_digest = sha256_file(cache_path)
    generator_file = build_root / "build.ninja"
    if not generator_file.is_file():
        state["cache"] = cache_digest
        state["reason"] = "Ninja build graph is missing"
        return state

    generated_build_files = {"build.ninja": sha256_file(generator_file)}
    missing_generated_build_files = []
    for line in generator_file.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.fullmatch(r"(?:include|subninja)\s+(.+)", line)
        if match is None:
            continue
        relative = match.group(1)
        included = build_root / relative
        if not included.is_file():
            missing_generated_build_files.append(relative)
        else:
            generated_build_files[relative] = sha256_file(included)

    expected_targets = (
        (
            (
                "tests/renderer-smoke/mc_renderer_smoke",
                "tests/renderer-smoke/mc_renderer_smoke.exe",
            ),
        )
        if name == "extra-renderer-smoke"
        else (("mc_main", "mc_main.exe"), ("tests/mc_tests", "tests/mc_tests.exe"))
    )
    artifacts = {}
    missing_artifacts = []
    ambiguous_artifacts = []
    for alternatives in expected_targets:
        matching_targets = [
            build_root / relative
            for relative in alternatives
            if (build_root / relative).is_file()
        ]
        if name == "extra-renderer-smoke" and len(matching_targets) > 1:
            ambiguous_artifacts.append(" or ".join(alternatives))
            continue
        target = matching_targets[0] if matching_targets else None
        if target is None:
            missing_artifacts.append(" or ".join(alternatives))
            continue
        relative = target.relative_to(build_root).as_posix()
        artifacts[relative] = {
            "size": target.stat().st_size,
            "mode": target.stat().st_mode & 0o7777,
            "sha256": sha256_file(target),
        }

    tool_identities = {}
    missing_tools = []
    for key in ("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "CMAKE_MAKE_PROGRAM", "CMAKE_TOOLCHAIN_FILE"):
        value = values.get(key, "")
        target = Path(value).expanduser() if value else None
        if target is None or not target.is_file():
            missing_tools.append(key)
            tool_identities[key] = value or "<missing>"
            continue
        tool_identities[key] = {
            "path": str(target.resolve()),
            "size": target.stat().st_size,
            "mode": target.stat().st_mode & 0o7777,
            "sha256": sha256_file(target),
        }

    dependency_roots: list[Path] = []
    for key in ("CMAKE_PREFIX_PATH", "VCPKG_INSTALLED_DIR"):
        for raw_path in values.get(key, "").split(";"):
            if raw_path:
                dependency_roots.append(Path(raw_path).expanduser())
    for key, raw_path in values.items():
        if (
            key in {"VCPKG_MANIFEST_DIR", "_VCPKG_MANIFEST_DIR"}
            or key.endswith(("_BINARY_DIR", "_SOURCE_DIR"))
        ):
            continue
        if key.startswith("CMAKE_") or not (key.endswith("_DIR") or key.endswith("_INCLUDE_DIR")):
            continue
        if raw_path:
            dependency_roots.append(Path(raw_path).expanduser())

    unique_roots: list[Path] = []
    for path in dependency_roots:
        resolved = path.resolve()
        if not resolved.is_dir():
            missing_tools.append(f"dependency directory {path}")
            continue
        if any(resolved == parent or parent in resolved.parents for parent in unique_roots):
            continue
        unique_roots = [parent for parent in unique_roots if parent not in resolved.parents]
        unique_roots.append(resolved)

    dependency_digests = {}
    for path in sorted(unique_roots):
        digest, file_count = sha256_tree(path)
        dependency_digests[str(path)] = {"sha256": digest, "files": file_count}

    home = values.get("CMAKE_HOME_DIRECTORY", "")
    wrong_root = not home or Path(home).resolve() != root.resolve()
    if values.get("CMAKE_GENERATOR") != "Ninja":
        wrong_root = True
    if not dependency_digests:
        missing_tools.append("configured dependency prefixes")

    state.update({
        "cache_sha256": cache_digest,
        "generated_build_files": generated_build_files,
        "missing_generated_build_files": missing_generated_build_files,
        "artifacts": artifacts,
        "missing_artifacts": missing_artifacts,
        "tools": tool_identities,
        "dependency_prefixes": dependency_digests,
        "available": not (
            wrong_root
            or missing_artifacts
            or ambiguous_artifacts
            or missing_tools
            or missing_generated_build_files
        ),
        "reason": (
            "wrong or missing CMake checkout/generator identity"
            if wrong_root
            else "missing generated build files: " + ", ".join(missing_generated_build_files)
            if missing_generated_build_files
            else "ambiguous output targets: " + ", ".join(ambiguous_artifacts)
            if ambiguous_artifacts
            else "missing output targets: " + ", ".join(missing_artifacts)
            if missing_artifacts
            else "missing compiler/tool/dependency identity: " + ", ".join(missing_tools)
            if missing_tools
            else "complete"
        ),
    })
    return state


def relevant_inputs(root: Path, name: str) -> dict[str, object]:
    paths = phase_relevant_paths(root, name)
    paths = sorted(set(paths) | {"script/ai_windows_job.py"})
    files = []
    for relative in paths:
        target = root / relative
        if target.is_file():
            digest = hashlib.sha256(target.read_bytes()).hexdigest()
        else:
            digest = "<missing>"
        files.append([relative, digest])
    payload = {
        "paths": paths,
        "files": files,
    }
    if name in NATIVE_RECEIPT_PHASES:
        payload["native_build_state"] = native_build_state(root, name)
    serialized = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    payload["fingerprint"] = hashlib.sha256(serialized).hexdigest()
    return payload


def phase_environment(root: Path, name: str) -> dict[str, str]:
    if name in {"python-tests", "python-tests-fast", "build", "ctest", "extra-renderer-smoke"}:
        return python_test_environment(root)
    return os.environ.copy()


def environment_fingerprint(environment: dict[str, str]) -> str:
    # PWD, shell nesting and the command lookup placeholder vary between hook
    # invocations without changing a check's inputs. Git also injects these
    # hook-only index/worktree selectors; the index/worktree hashes below are
    # the authoritative state. Never persist values.
    ignored = {
        "PWD", "OLDPWD", "SHLVL", "_", "GIT_DIR", "GIT_COMMON_DIR",
        "GIT_INDEX_FILE", "GIT_PREFIX", "GIT_WORK_TREE",
        "GIT_AUTHOR_DATE", "GIT_AUTHOR_EMAIL", "GIT_AUTHOR_NAME", "GIT_EDITOR",
        "CODEX_TOOL_CALL_ID",
    }
    values = {key: value for key, value in environment.items() if key not in ignored}
    if "GIT_EXEC_PATH" not in values:
        resolved = subprocess.run(
            ["git", "--exec-path"], text=True, capture_output=True, check=False
        )
        if resolved.returncode == 0 and resolved.stdout.strip():
            values["GIT_EXEC_PATH"] = resolved.stdout.strip()
    serialized = json.dumps(values, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(serialized).hexdigest()


def tool_identity(command: Sequence[str]) -> dict[str, object]:
    executable = command[0] if command else ""
    resolved = shutil.which(executable) or executable
    target = Path(resolved)
    identity: dict[str, object] = {
        "requested": executable,
        "resolved": str(target.resolve()) if target.exists() else resolved,
        "python": sys.version,
        "platform": platform.platform(),
    }
    try:
        stat = target.stat()
        identity.update({"size": stat.st_size, "mtime_ns": stat.st_mtime_ns})
    except OSError:
        identity.update({"size": None, "mtime_ns": None})
    return identity


def receipt_path(log_dir: Path, name: str) -> Path:
    return log_dir / f"{name}.receipt.json"


def read_matching_receipt(
    root: Path,
    log_dir: Path,
    name: str,
    command: Sequence[str],
    environment: dict[str, str],
    inputs: dict[str, object],
) -> str | None:
    if name in NATIVE_RECEIPT_PHASES:
        native_state = inputs.get("native_build_state")
        if not isinstance(native_state, dict) or native_state.get("available") is not True:
            return None
    target = receipt_path(log_dir, name)
    log = log_dir / f"{name}.log"
    try:
        receipt = json.loads(target.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    expected = {
        "schema_version": RECEIPT_SCHEMA_VERSION,
        "phase": name,
        "status": "PASS",
        "command": list(command),
        "environment_fingerprint": environment_fingerprint(environment),
        "tool_identity": tool_identity(command),
        "relevant_inputs": inputs,
        "returncode": 0,
        "timed_out": False,
    }
    if not log.is_file():
        return None
    try:
        output = log.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return None
    expected["evidence_sha256"] = evidence_fingerprint(output)
    if receipt != expected:
        return None
    return output


def validate_phase_receipt(
    root: Path,
    log_dir: Path,
    name: str,
    command: Sequence[str],
) -> str | None:
    environment = phase_environment(root, name)
    inputs = relevant_inputs(root, name)
    return read_matching_receipt(root, log_dir, name, command, environment, inputs)


def evidence_fingerprint(output: str) -> str:
    return hashlib.sha256(output.encode("utf-8")).hexdigest()


S1_CURRENT_NORMAL_EXCEPTION_PAYLOAD = {'capture': {'aggregate_sha256': '2a437863daa4bcb334384d0d23fa4dca65a5e2230f608930983d7d51df091ac7',
             'executable_sha256': 'd8035bd0a014033f68d58a7d82f51bda81b51a1a5f280ecdb0db0ef83601f6ab',
             'raw_report_sha256': '0019d1c2c8d66e70be325dffeb022f56b5abfd95db7a0bd7332d0f52cf5b6c9b',
             'source_candidate': {'head': '168bbf38b367f9621f49286ccca85a8a42095289',
                                  'tree': '0fbf093c94770683961ca4a0653fd98f7c5690f7'}},
 'capture_source_input_sha256': '0dc600d5809fc661c3b73d86fe05dca951b661fe58ef7fc8c1f358651c3a3471',
 'current_normal_runtime': {'scope': 'fresh current-source normal Mac rendering/input/movement evidence; '
                                     'performance and LOD failed; no new owner playtest or Windows '
                                     'qualification',
                            'semantic_review': {'path': '/Users/core-just/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/review-artifacts/source13-analysis-source-review-consolidated-v3.json',
                                                'sha256': '5b50a1855bf846fb9337557a9c65535179e9ed5d7fa98ce2d92dadf732be3e82',
                                                'size': 18685}},
 'known_failure': {'counters': {'accepted_tick': 0,
                                'authoritative_tick_ms': 100,
                                'camera_relative_inputs': 0,
                                'clients_accepted': 1,
                                'clients_requested': 1,
                                'expectations_passed': 0,
                                'inputs_sent': 431,
                                'last_effective_tick': 0,
                                'server_events_processed': 9836,
                                'ticks': 300},
                   'failed_present_requests': 4,
                   'frame_budget': {'fence_confirmed_frame_count': 3467,
                                    'in_budget_time_ns': 12406228240,
                                    'measured_time_ns': 30000000000,
                                    'measurement_scope': 'fence-confirmed-render-frame-completion-observations',
                                    'measurement_window_complete': True,
                                    'measurement_window_end_offset_ns': 42096340375,
                                    'measurement_window_start_offset_ns': 12096340375,
                                    'passed': False,
                                    'required_for_benchmark_pass': True,
                                    'required_in_budget_time_permille': 990,
                                    'required_window_duration_ns': 30000000000,
                                    'sample_count': 3467,
                                    'target_frame_duration_ns': 8333333},
                   'raw_fence_confirmed_frame_count': 5004,
                   'raw_frame_count': 4984,
                   'raw_input_count': 431,
                   'report_failure': 'game benchmark did not complete the scripted full-game workload',
                   's1': {'actual_radius_blocks': 640,
                          'contract': 'mc.s1-lod-performance.v2',
                          'initial_deadline_missed': False,
                          'initial_radius_blocks': 48,
                          'movement_authoritative_update_count': 301,
                          'movement_input_during_fill': True,
                          'movement_playable_sphere_complete': False,
                          'movement_started_after_current_sphere_ready': True,
                          'movement_visual_lod_samples_complete': False,
                          'playable_deadline_missed': False,
                          'playable_sphere_first_ready_offset_ns': 14246828125,
                          'ready_prompt_frame_offset_ns': 2303056750,
                          'visual_lod_radius_blocks': 320},
                   'successful_present_requests': 718,
                   'timing': {'deadline_ms': 60000, 'elapsed_ms': 45525}},
 'owner_request_path': '~/.codex/task-evidence/mc-s1-20261023/owner-release-acceptance/request.md',
 'owner_request_sha256': 'b55134d42a04a746ca862d4b12ba847cb8e0e73ac88ae1ca787b13daf6ebe403',
 'owner_tested_windows_executable_sha256': 'cba2572c3cd47b9da82a2ad2ab1f958231ea20b57b9a6ddad17107013d89402b',
 'proof_dispositions': {'macos': {'performance': 'failed', 'terrain-coverage': 'unqualified'},
                        'windows': {'performance': 'unqualified', 'terrain-coverage': 'unqualified'}},
 'schema_version': 'mc.s1-owner-accepted-current-normal.v2',
 'source_input_sha256': '0dc600d5809fc661c3b73d86fe05dca951b661fe58ef7fc8c1f358651c3a3471',
 'source_reuse_certificate': None,
 'status': 'accepted_exception'}

def accepted_current_normal_payload(result: PhaseResult, payload: dict[str, object]) -> dict[str, object] | None:
    candidate = payload.get("candidate")
    try:
        commit = result.command[result.command.index("--candidate-commit") + 1]
    except (ValueError, IndexError):
        return None
    if (not isinstance(candidate, dict) or set(candidate) != {"head", "tree"}
            or candidate.get("head") != commit
            or any(not isinstance(candidate[key], str) or re.fullmatch(r"[0-9a-f]{40}", candidate[key]) is None
                   for key in ("head", "tree"))
            or {key: value for key, value in payload.items() if key != "candidate"}
            != S1_CURRENT_NORMAL_EXCEPTION_PAYLOAD):
        return None
    return payload


def accepted_exception_payload(result: PhaseResult) -> dict[str, object] | None:
    if (
        result.name != "full-radius-gameplay"
        or result.returncode != 0
        or result.timed_out
        or "script/ci/verify_gameplay_gate.py" not in result.command
        or "--s1-owner-accepted" not in result.command
    ):
        return None
    try:
        payload = json.loads(result.output)
    except (json.JSONDecodeError, TypeError):
        return None
    if isinstance(payload, dict) and payload.get("schema_version") == "mc.s1-owner-accepted-current-normal.v2":
        return accepted_current_normal_payload(result, payload)
    if (
        not isinstance(payload, dict)
        or set(payload) != {
            "schema_version", "status", "owner_request_path", "owner_request_sha256", "source_input_sha256",
            "capture_source_input_sha256", "source_reuse_certificate", "candidate", "capture",
            "proof_dispositions", "known_failure",
            "owner_tested_windows_executable_sha256",
        }
        or payload.get("schema_version") != S1_OWNER_EXCEPTION_SCHEMA
        or payload.get("status") != "accepted_exception"
        or payload.get("owner_request_path") != S1_OWNER_REQUEST_PATH
        or payload.get("owner_request_sha256") != S1_OWNER_REQUEST_SHA256
        or payload.get("capture_source_input_sha256") != S1_OWNER_SOURCE_INPUT_SHA256
        or (
            payload.get("source_input_sha256") == S1_OWNER_SOURCE_INPUT_SHA256
            and payload.get("source_reuse_certificate") is not None
        )
        or (
            payload.get("source_input_sha256") == S1_OWNER_SOURCE_REUSE_CURRENT_SHA256
            and payload.get("source_reuse_certificate") != S1_OWNER_SOURCE_REUSE_CERTIFICATE
        )
        or (
            payload.get("source_input_sha256") == S1_OWNER_SOURCE_REUSE_V3_CURRENT_SHA256
            and payload.get("source_reuse_certificate") != S1_OWNER_SOURCE_REUSE_V3_CERTIFICATE
        )
        or payload.get("source_input_sha256") not in {
            S1_OWNER_SOURCE_INPUT_SHA256,
            S1_OWNER_SOURCE_REUSE_V3_CURRENT_SHA256,
            S1_OWNER_SOURCE_REUSE_CURRENT_SHA256,
        }
        or payload.get("proof_dispositions") != S1_OWNER_PROOF_DISPOSITIONS
        or payload.get("known_failure") != S1_OWNER_KNOWN_FAILURE
        or payload.get("owner_tested_windows_executable_sha256")
            != S1_OWNER_WINDOWS_EXECUTABLE_SHA256
    ):
        return None
    candidate = payload.get("candidate")
    capture = payload.get("capture")
    try:
        candidate_commit = result.command[result.command.index("--candidate-commit") + 1]
    except (ValueError, IndexError):
        return None
    if (
        not isinstance(candidate, dict)
        or set(candidate) != {"head", "tree"}
        or candidate.get("head") != candidate_commit
        or any(
            not isinstance(candidate[key], str)
            or re.fullmatch(r"[0-9a-f]{40}", candidate[key]) is None
            for key in ("head", "tree")
        )
        or not isinstance(capture, dict)
        or capture != {
            "aggregate_sha256": S1_OWNER_AGGREGATE_SHA256,
            "raw_report_sha256": S1_OWNER_RAW_REPORT_SHA256,
            "executable_sha256": S1_OWNER_MAC_EXECUTABLE_SHA256,
            "source_candidate": S1_OWNER_CAPTURE_CANDIDATE,
        }
    ):
        return None
    return payload


def gameplay_output_status(result: PhaseResult) -> str:
    if result.name != "full-radius-gameplay" or result.returncode != 0 or result.timed_out:
        return "not-success"
    if (
        "script/ci/verify_gameplay_gate.py" not in result.command
        or "--candidate-commit" not in result.command
    ):
        return "invalid"
    try:
        payload = json.loads(result.output)
    except (json.JSONDecodeError, TypeError):
        return "invalid"
    if not isinstance(payload, dict):
        return "invalid"
    if payload.get("schema_version") in {
        S1_OWNER_EXCEPTION_SCHEMA, "mc.s1-owner-accepted-current-normal.v2"
    }:
        return ("accepted_exception" if payload.get("status") == "accepted_exception"
                and accepted_exception_payload(result) is not None else "invalid")
    if payload.get("status") == "passed":
        return "passed"
    if payload.get("status") == "accepted_exception":
        return "accepted_exception" if accepted_exception_payload(result) is not None else "invalid"
    return "invalid"


def write_receipt(
    log_dir: Path,
    name: str,
    command: Sequence[str],
    environment: dict[str, str],
    inputs: dict[str, object],
    result: PhaseResult,
) -> None:
    if result.returncode != 0 or result.timed_out:
        receipt = {
            "schema_version": RECEIPT_SCHEMA_VERSION,
            "phase": name,
            "status": "TIMEOUT" if result.timed_out else "FAIL",
            "command": list(command),
            "environment_fingerprint": environment_fingerprint(environment),
            "tool_identity": tool_identity(command),
            "relevant_inputs": inputs,
            "returncode": result.returncode,
            "timed_out": result.timed_out,
            "evidence_sha256": evidence_fingerprint(result.output),
        }
        receipt_path(log_dir, name).write_text(
            json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        return
    accepted_exception = accepted_exception_payload(result)
    if accepted_exception is not None:
        receipt = {
            "schema_version": RECEIPT_SCHEMA_VERSION,
            "phase": name,
            "status": "ACCEPTED_EXCEPTION",
            "command": list(command),
            "environment_fingerprint": environment_fingerprint(environment),
            "tool_identity": tool_identity(command),
            "relevant_inputs": inputs,
            "returncode": result.returncode,
            "timed_out": result.timed_out,
            "evidence_sha256": evidence_fingerprint(result.output),
            "owner_request_sha256": accepted_exception["owner_request_sha256"],
            "owner_request_path": accepted_exception["owner_request_path"],
            "source_input_sha256": accepted_exception["source_input_sha256"],
            "capture_source_input_sha256": accepted_exception["capture_source_input_sha256"],
            "source_reuse_certificate": accepted_exception["source_reuse_certificate"],
            "candidate": accepted_exception["candidate"],
        }
        receipt_path(log_dir, name).write_text(
            json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        return
    receipt = {
        "schema_version": RECEIPT_SCHEMA_VERSION,
        "phase": name,
        "status": "PASS",
        "command": list(command),
        "environment_fingerprint": environment_fingerprint(environment),
        "tool_identity": tool_identity(command),
        "relevant_inputs": inputs,
        "returncode": result.returncode,
        "timed_out": result.timed_out,
        "evidence_sha256": evidence_fingerprint(result.output),
    }
    receipt_path(log_dir, name).write_text(
        json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def partially_staged_paths(root: Path) -> list[str]:
    staged = {path for path in changed_paths(root, cached=True) if governed(path)}
    unstaged = {path for path in changed_paths(root, cached=False) if governed(path)}
    untracked = {path for path in untracked_paths(root) if governed(path)}
    return sorted(unstaged | untracked) if staged else []


def project_version_arguments(root: Path) -> tuple[str, str]:
    content = (root / "src/shared/include/shared/ProjectInfo.hpp").read_text(encoding="utf-8")

    def value(name: str) -> str:
        match = re.search(rf"{name}\s*\{{\s*\"([^\"]+)\"\s*\}}", content)
        if not match:
            raise ValueError(f"{name} was not found in ProjectInfo.hpp")
        return match.group(1)

    fields = {}
    for field in ("epoch", "major", "minor", "patch"):
        match = re.search(rf"\.{field}\s*=\s*(\d+)", content)
        if not match:
            raise ValueError(f"PROJECT_VERSION.{field} was not found in ProjectInfo.hpp")
        fields[field] = match.group(1)
    return (
        f"{value('MAJOR_VERSION_NAME')}:{value('MINOR_VERSION_NAME')}",
        f"{fields['epoch']}.{fields['major']}.{fields['minor']}:{fields['patch']}",
    )


def require_s7_candidate(root: Path) -> None:
    try:
        version = project_version_arguments(root)
        manifest = json.loads((root / "vcpkg.json").read_text(encoding="utf-8"))
        valid = version == ("EarlyDev:Initiation", "0.1.0:7") and isinstance(manifest, dict) \
            and manifest.get("version-string") == "0.1.0"
    except (OSError, ValueError) as error:
        raise ValueError(f"S7 selector cannot read actual candidate version: {error}") from error
    if not valid:
        raise ValueError("S7 selector does not match the actual candidate version")


def publisher_failure_names(output: str) -> set[str]:
    clean = re.sub(r"\x1b\[[0-9;]*m", "", output)
    names = set()
    for line in clean.splitlines():
        match = re.match(r"^\[\d+/\d+\]\s+(.+?)\s+\.\.\.\s+", line)
        if match and "FAIL" in line:
            names.add(match.group(1))
    return names


def publisher_failure_is_allowed(output: str, strict: bool, candidate: bool = False) -> bool:
    if (strict and not candidate) or "Traceback" in output or "Exception:" in output:
        return False
    clean = re.sub(r"\x1b\[[0-9;]*m", "", output)
    failures = publisher_failure_names(clean)
    rows = [line for line in clean.splitlines() if re.match(r"^\[\d+/\d+\]", line)]
    allowed_lines = {"=== Common Checks ===", "ERROR - Some checks failed. Aborting."}
    for line in clean.splitlines():
        if not line or line in allowed_lines or line in rows:
            continue
        return False
    allowed = {"no unstaged or uncommitted changes"} if candidate else DEFAULT_PUBLISHER_FAILURES
    return bool(failures) and failures <= allowed


def python_test_environment(root: Path) -> dict[str, str]:
    names = command_output(root, ["git", "rev-parse", "--local-env-vars"]).splitlines()
    if not names:
        raise RuntimeError("Git returned no repository-local environment variables")
    if any(not re.fullmatch(r"GIT_[A-Z0-9_]+", name) for name in names):
        raise RuntimeError("Git returned an invalid repository-local environment variable")
    environment = os.environ.copy()
    for name in names:
        environment.pop(name, None)
    # The selector belongs to the check caller. Letting it reach harness tests
    # makes their subprocesses depend on which task invoked the check.
    environment.pop("AI_TASK", None)
    environment.pop("AI_SNAPSHOT_SOURCE_COMMIT", None)
    return environment


def terminate_process_tree(process: subprocess.Popen) -> None:
    """Stop a timed-out phase and every subprocess it started."""
    if os.name == "nt":
        if process.poll() is None:
            process.kill()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        return

    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def run_phase(
    root: Path,
    log_dir: Path,
    name: str,
    command: Sequence[str],
    timeout: int,
    *,
    reuse: bool = False,
) -> PhaseResult:
    started = time.monotonic()
    timeout = min(timeout, phase_timeout_cap(name))
    try:
        environment = phase_environment(root, name)
        inputs = relevant_inputs(root, name)
    except (OSError, RuntimeError) as error:
        # An old PASS must not survive an inability to fingerprint the phase:
        # otherwise a later invocation could reuse evidence whose inputs were
        # never successfully checked.
        try:
            receipt_path(log_dir, name).unlink(missing_ok=True)
        except OSError:
            pass
        result = PhaseResult(name, command, 127, str(error))
        result.duration_seconds = time.monotonic() - started
        (log_dir / f"{name}.log").write_text(result.output, encoding="utf-8")
        receipt_path(log_dir, name).write_text(
            json.dumps({
                "schema_version": RECEIPT_SCHEMA_VERSION,
                "phase": name,
                "status": "FAIL",
                "command": list(command),
                "returncode": result.returncode,
                "timed_out": False,
                "error": result.output,
            }, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        return result
    if reuse:
        output = read_matching_receipt(root, log_dir, name, command, environment, inputs)
        if output is not None:
            return PhaseResult(
                name, command, 0, output, reused=True, duration_seconds=time.monotonic() - started
            )
    initial_native_state = inputs.get("native_build_state")
    if name == "ctest" and (
        not isinstance(initial_native_state, dict)
        or initial_native_state.get("available") is not True
    ):
        result = PhaseResult(
            name,
            command,
            1,
            "CTest requires a complete matching debug build receipt state: "
            + str(initial_native_state.get("reason", "native build state is unavailable"))
            if isinstance(initial_native_state, dict)
            else "CTest requires a complete matching debug build receipt state",
        )
        result.duration_seconds = time.monotonic() - started
        write_receipt(log_dir, name, command, environment, inputs, result)
        return result
    log_path = log_dir / f"{name}.log"
    relative_log = log_path.relative_to(root)
    print(
        f"RUN {name}: {' '.join(command)} (timeout {timeout}s; log {relative_log})",
        flush=True,
    )
    termination_grace = min(TERMINATION_GRACE_SECONDS, max(0.1, timeout * 0.1))
    try:
        with log_path.open("wb") as log_stream:
            launch_command = list(command)
            if os.name == "nt":
                launch_command = [sys.executable, str(Path(__file__).with_name("ai_windows_job.py")), *launch_command]
            process = subprocess.Popen(
                launch_command,
                cwd=root,
                stdout=log_stream,
                stderr=subprocess.STDOUT,
                env=environment,
                start_new_session=os.name != "nt",
                creationflags=(subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0),
            )
            cursor = 0
            timed_out = False
            while True:
                elapsed = time.monotonic() - started
                remaining = timeout - elapsed
                if remaining <= termination_grace:
                    terminate_process_tree(process)
                    timed_out = True
                    break
                try:
                    returncode = process.wait(
                        timeout=min(30.0, remaining - termination_grace)
                    )
                    break
                except subprocess.TimeoutExpired:
                    log_stream.flush()
                    with log_path.open("rb") as reader:
                        reader.seek(cursor)
                        chunk = reader.read()
                    if chunk:
                        sys.stdout.write(chunk.decode(errors="replace"))
                        sys.stdout.flush()
                        cursor += len(chunk)
                    elapsed = time.monotonic() - started
                    if timeout - elapsed <= termination_grace:
                        terminate_process_tree(process)
                        timed_out = True
                        break
                    last_evidence = "output received" if chunk else "no output received yet"
                    print(
                        f"RUNNING {name}: {elapsed:.0f}s elapsed; {last_evidence}; "
                        f"timeout {timeout}s; log {relative_log}",
                        flush=True,
                    )
            log_stream.flush()
            with log_path.open("rb") as reader:
                reader.seek(cursor)
                final_chunk = reader.read()
            if final_chunk:
                sys.stdout.write(final_chunk.decode(errors="replace"))
                sys.stdout.flush()
            output = log_path.read_text(encoding="utf-8", errors="replace")
            result = PhaseResult(
                name,
                command,
                124 if timed_out else returncode,
                output,
                timed_out=timed_out,
            )
    except (OSError, RuntimeError) as error:
        result = PhaseResult(name, command, 127, str(error))
        log_path.write_text(result.output, encoding="utf-8")
    result.duration_seconds = time.monotonic() - started
    if result.returncode == 0 and name in NATIVE_RECEIPT_PHASES:
        try:
            final_inputs = relevant_inputs(root, name)
            final_native_state = final_inputs.get("native_build_state")
            same_repository_inputs = (
                inputs.get("paths") == final_inputs.get("paths")
                and inputs.get("files") == final_inputs.get("files")
            )
            if not same_repository_inputs:
                result.returncode = 1
                result.output += "\nNative source/configuration inputs changed while the phase was running."
            elif (
                not isinstance(final_native_state, dict)
                or final_native_state.get("available") is not True
            ):
                reason = (
                    final_native_state.get("reason", "native build state is unavailable")
                    if isinstance(final_native_state, dict)
                    else "native build state is unavailable"
                )
                result.returncode = 1
                result.output += f"\nNative output/configuration evidence is incomplete: {reason}."
            elif name == "ctest" and initial_native_state != final_native_state:
                result.returncode = 1
                result.output += "\nNative build inputs or outputs changed while CTest was running."
            else:
                inputs = final_inputs
        except (OSError, RuntimeError) as error:
            result.returncode = 1
            result.output += f"\nCannot revalidate native build evidence: {error}."
    elif result.returncode == 0 and name == "ctest":
        result.returncode = 1
        result.output += "\nCTest completed without a complete pre-run native build identity."
    if result.returncode == 0 and name == "full-radius-gameplay":
        if gameplay_output_status(result) == "invalid":
            result.returncode = 1
            result.output += "\nGameplay gate emitted an invalid or unbound accepted gameplay exception."
    write_receipt(log_dir, name, command, environment, inputs, result)
    return result


def print_result(result: PhaseResult) -> None:
    command = " ".join(result.command)
    if result.skipped:
        label = "UNRUN" if result.returncode != 0 else "SKIP"
        print(f"{label} {result.name}: {result.output}")
        return
    if result.reused:
        print(f"REUSED PASS {result.name}: {command} ({result.duration_seconds:.2f}s)")
        return
    if result.returncode == 0:
        accepted_exception = accepted_exception_payload(result)
        if accepted_exception is not None:
            print(
                f"ACCEPTED_EXCEPTION {result.name}: owner request "
                f"{accepted_exception['owner_request_sha256']} accepts the retained capture source "
                f"{accepted_exception['capture_source_input_sha256']} for current source "
                f"{accepted_exception['source_input_sha256']} and candidate "
                f"{accepted_exception['candidate']['head']}"
            )
            return
        if result.name == "full-radius-gameplay" and gameplay_output_status(result) == "invalid":
            print(f"FAIL {result.name}: gameplay gate emitted an invalid or unbound success result")
            print(result.output.strip())
            return
        print(f"PASS {result.name}: {command} ({result.duration_seconds:.2f}s)")
        return
    state = "TIMEOUT" if result.timed_out else "FAIL"
    if result.allowed_failure:
        state = "ALLOWED"
    print(
        f"{state} {result.name}: exit {result.returncode} after {result.duration_seconds:.2f}s; "
        f"log build/ai-checks/{result.name}.log"
    )
    diagnostics = result.output.strip()
    if diagnostics:
        print(diagnostics)


def changed_scope(root: Path) -> tuple[str, list[str]]:
    paths = changed_paths(root, cached=True) | changed_paths(root, cached=False) | untracked_paths(root)
    paths = {
        path for path in paths
        if not path.startswith("build/ai-checks/")
        and "/__pycache__/" not in f"/{path}"
        and not path.endswith(".pyc")
    }
    if not paths:
        return "full-python", []
    if all(metadata_path(path) for path in paths):
        return "metadata-only", sorted(paths)
    python_sources = {
        path for path in paths
        if path.startswith("script/") and path.endswith(".py") and not path.startswith("script/tests/")
    }
    python_tests = {
        path for path in paths
        if path.startswith("script/tests/") and path.endswith(".py")
    }
    if python_sources:
        targets = []
        for source in sorted(python_sources):
            if Path(source).name in SHARED_PYTHON_SOURCES or "/ci/" in source:
                return "full-python", sorted(paths)
            candidate = f"script/tests/test_{Path(source).stem}.py"
            # The exact one-file mapping is intentionally conservative. A
            # missing test or unrelated mixed change falls back to the full suite.
            if not (root / candidate).is_file():
                return "full-python", sorted(paths)
            targets.append(candidate)
        metadata = paths - python_sources - python_tests
        if not all(metadata_path(path) for path in metadata):
            return "full-python", sorted(paths)
        if python_tests and python_tests != set(targets):
            return "full-python", sorted(paths)
        return "targeted-python:" + ":".join(targets), sorted(paths)
    if python_tests:
        return "full-python", sorted(paths)
    if all(
        path.startswith(PYTHON_WORKFLOW_PREFIXES) or path == "AGENTS.md"
        for path in paths
    ):
        return "full-python", sorted(paths)
    if all(native_build_path(path) or metadata_path(path) for path in paths):
        return "full-cpp", sorted(paths)
    # Unknown inputs are not safely attributable to one test file; retain the
    # broad Python policy checks rather than treating them as C++ changes.
    return "full-python", sorted(paths)


def write_summary(root: Path, scope: str, changed: Sequence[str], results: Sequence[PhaseResult]) -> None:
    try:
        head = command_output(root, ["git", "rev-parse", "HEAD"]).strip()
        index_tree = command_output(root, ["git", "write-tree"]).strip()
    except (OSError, RuntimeError):
        head = ""
        index_tree = ""
    payload = {
        "schema_version": RECEIPT_SCHEMA_VERSION,
        "invocation_id": f"{time.time_ns()}-{os.getpid()}",
        "head": head,
        "index_tree": index_tree,
        "task_id": os.environ.get("AI_TASK", ""),
        "scope": scope,
        "changed_paths": list(changed),
        "reused_phases": [result.name for result in results if result.reused],
        "executed_phases": [result.name for result in results if not result.reused and not result.skipped],
        "accepted_exception_phases": [
            result.name for result in results if accepted_exception_payload(result) is not None
        ],
        "skipped_phases": [result.name for result in results if result.skipped],
        "timed_out_phases": [result.name for result in results if result.timed_out],
        "unrun_phases": [
            result.name for result in results if result.skipped and result.returncode != 0
        ],
        "durations_seconds": {
            result.name: round(result.duration_seconds, 6) for result in results
        },
    }
    (root / LOG_DIR / "summary.json").write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    with (root / LOG_DIR / "summary.jsonl").open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(payload, sort_keys=True) + "\n")


def main(argv: Sequence[str] | None = None) -> int:
    invocation_started = time.monotonic()
    invocation_deadline = invocation_started + TOTAL_TIMEOUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fast", action="store_true", help="skip build, CTest, and publisher checks")
    parser.add_argument("--strict", action="store_true", help="do not allow development publisher failures")
    parser.add_argument("--candidate", action="store_true", help="full release checks before commit; allow only dirty Git state")
    parser.add_argument(
        "--proof-only",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--level", choices=("basic", "snapshot", "minor", "major"), default="basic")
    parser.add_argument("--require-index-match", action="store_true", help="reject partially staged governed files")
    parser.add_argument("--snapshot-source-commit", help="bind the snapshot date to an immutable AI source commit")
    parser.add_argument("--snapshot-task-id", help=argparse.SUPPRESS)
    parser.add_argument("--root", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.fast and (args.candidate or args.strict or args.level != "basic"):
        parser.error("--fast cannot be combined with release checks")
    if args.fast and args.snapshot_source_commit:
        parser.error("--fast cannot use a snapshot source binding")
    if args.proof_only and (args.fast or args.candidate == args.strict):
        parser.error("--proof-only requires exactly one of --candidate or --strict")
    if args.proof_only and args.snapshot_source_commit is None and args.candidate:
        parser.error("candidate proof validation requires an immutable snapshot source commit")
    root = (args.root or repository_root()).resolve()
    try:
        snapshot_source = snapshot_source_for_check(
            root,
            args.snapshot_source_commit,
            candidate=args.candidate,
            strict=args.strict,
            snapshot_task_id=args.snapshot_task_id,
        )
    except (OSError, RuntimeError, ValueError) as error:
        print(f"AI check date binding: {error}", file=sys.stderr)
        return 1
    try:
        proof_candidate_commit = (
            snapshot_source
            if args.candidate and snapshot_source is not None
            else git_output(root, "rev-parse", "HEAD")
        )
    except (OSError, RuntimeError) as error:
        print(f"AI check proof binding: {error}", file=sys.stderr)
        return 1
    log_dir = root / LOG_DIR
    log_dir.mkdir(parents=True, exist_ok=True)
    try:
        scope, changed = changed_scope(root)
    except (OSError, RuntimeError) as error:
        scope, changed = "full-python", []
        print(f"CHECK SCOPE unknown: {error}; using conservative full Python fallback")
    if changed:
        print(f"CHECK SCOPE {scope}: {', '.join(changed)}")

    results: list[PhaseResult] = []
    if sys.version_info < (3, 12):
        results.append(PhaseResult("python-version", (), 1, "Python 3.12 or newer is required"))
    if args.require_index_match:
        try:
            partial = partially_staged_paths(root)
            output = "" if not partial else "Partially staged governed files: " + ", ".join(partial)
            results.append(PhaseResult("index-match", (), int(bool(partial)), output))
        except (OSError, RuntimeError) as error:
            results.append(PhaseResult("index-match", (), 1, str(error)))

    phase_specs: list[tuple[str, Sequence[str], int, bool]] = [
        ("docs", [sys.executable, "script/ai_docs.py", "check"], 60, True),
        ("backlog", [sys.executable, "script/ai_tasks.py", "check"], 60, True),
        ("current-plan", [sys.executable, "script/ai_plan.py", "check"], 60, True),
        ("release-reports", [sys.executable, "script/ai_release_reports.py", "check-state"], 30, False),
        ("diff-working", ["git", "diff", "--check"], 30, False),
        ("diff-cached", ["git", "diff", "--cached", "--check"], 30, False),
    ]
    native_changes = scope == "full-cpp" or any(
        name in NATIVE_BUILD_FILES or name.startswith(NATIVE_BUILD_PREFIXES)
        for name in changed
    )
    if not args.proof_only and args.fast and native_changes:
        try:
            names, version = project_version_arguments(root)
            phase_specs.append(
                (
                    "source-checks",
                    [sys.executable, "publish.py", names, version, "--checks-only"],
                    60,
                    True,
                )
            )
        except (OSError, ValueError) as error:
            results.append(PhaseResult("source-checks", (), 1, str(error)))
    if args.proof_only:
        pass
    elif args.fast and scope in {"metadata-only", "full-cpp"}:
        results.append(PhaseResult("python-tests-fast", (), 0, f"{scope} changes", skipped=True))
    elif args.fast and scope.startswith("targeted-python:"):
        targets = scope.split(":")[1:]
        phase_specs.append(
            (
                "python-tests-fast",
                [sys.executable, "script/ai_python_tests.py", "--targets", *targets],
                python_test_timeout(),
                True,
            )
        )
    elif args.fast:
        phase_specs.append(
            (
                "python-tests-fast",
                [sys.executable, "script/ai_python_tests.py"],
                python_test_timeout(),
                True,
            )
        )
    else:
        phase_specs.append(
            (
                "python-tests",
                [sys.executable, "-m", "unittest", "discover", "-s", "script/tests", "-v"],
                python_test_timeout(),
                True,
            )
        )
    # Fast checks intentionally stop at affected tooling and whitespace checks.
    # The coordinator owns the one complete build/CTest integration gate at a
    # batch boundary; running it here would repeat the batch-wide work for
    # every basic gameplay commit.
    if not args.fast and not args.proof_only:
        phase_specs.extend([
            ("build", BUILD_PHASE_COMMANDS["build"], 600, True),
            ("ctest", BUILD_PHASE_COMMANDS["ctest"], CTEST_PHASE_TIMEOUT, True),
        ])
    required_receipts: list[tuple[str, Sequence[str]]] = []
    if args.proof_only:
        required_receipts.extend([
            (
                "python-tests",
                [sys.executable, "-m", "unittest", "discover", "-s", "script/tests", "-v"],
            ),
            ("build", BUILD_PHASE_COMMANDS["build"]),
            ("ctest", BUILD_PHASE_COMMANDS["ctest"]),
        ])
    if not args.fast:
        try:
            config = json.loads((root / "script/ai_checks.json").read_text(encoding="utf-8"))
            seen = set()
            proof_count = 0
            if not isinstance(config, list):
                raise ValueError("additional checks must be an array")
            for item in config:
                required_fields = {"name", "enabled", "levels", "command", "timeout", "reason"}
                optional_fields = {"when", "reuse", "proof", "receipt"}
                if not isinstance(item, dict) or set(item) - required_fields - optional_fields or not required_fields <= set(item):
                    raise ValueError("invalid additional check fields")
                name = item["name"]
                if not isinstance(name, str) or not re.fullmatch(r"[a-z][a-z0-9-]*", name) or name in seen:
                    raise ValueError("invalid or duplicate additional check name")
                seen.add(name)
                if type(item["enabled"]) is not bool or not isinstance(item["levels"], list) or not item["levels"] or any(level not in {"basic", "snapshot", "minor", "major"} for level in item["levels"]):
                    raise ValueError("invalid additional check enabled/levels")
                if not isinstance(item["command"], list) or any(not isinstance(arg, str) or not arg for arg in item["command"]):
                    raise ValueError("invalid additional check command")
                if (
                    type(item["timeout"]) is not int
                    or not 0 < item["timeout"] <= MAX_PHASE_TIMEOUT
                    or not isinstance(item["reason"], str)
                ):
                    raise ValueError("invalid additional check timeout/reason")
                when = item.get("when", "always")
                if when not in {"always", "candidate", "strict", "release"}:
                    raise ValueError("invalid additional check when")
                reuse = item.get("reuse", False)
                if type(reuse) is not bool:
                    raise ValueError("invalid additional check reuse flag")
                proof = item.get("proof", False)
                if type(proof) is not bool:
                    raise ValueError("invalid additional check proof flag")
                receipt = item.get("receipt", False)
                if type(receipt) is not bool or (proof and receipt):
                    raise ValueError("invalid additional check receipt flag")
                if item["enabled"] and not item["command"]:
                    raise ValueError("enabled check must have a command")
                if not item["enabled"] and not item["reason"].strip():
                    raise ValueError("disabled check must explain why")
                if args.level not in item["levels"]:
                    continue
                if when == "strict" and not args.strict:
                    print(f"DEFERRED {name}: runs in the local strict release phase")
                    continue
                if when == "candidate" and not args.candidate:
                    print(f"DEFERRED {name}: runs in the local candidate release phase")
                    continue
                if when == "release" and not (args.candidate or args.strict):
                    print(f"DEFERRED {name}: runs in candidate and strict release checks")
                    continue
                if args.proof_only and not (proof or receipt):
                    continue
                if item["enabled"]:
                    substitutions = {
                        "{python}": sys.executable,
                        "{root}": str(root),
                        "{proof-candidate-commit}": proof_candidate_commit,
                    }
                    command = [
                        substitutions.get(arg, arg).replace("{root}", str(root))
                        for arg in item["command"]
                    ]
                    if args.proof_only and proof:
                        phase_specs.append(("extra-" + name, command, item["timeout"], reuse))
                        proof_count += 1
                    elif not args.proof_only:
                        phase_specs.append(("extra-" + name, command, item["timeout"], reuse))
                    if args.proof_only and receipt:
                        required_receipts.append(("extra-" + name, command))
                else:
                    print(f"NOT ENABLED {name}: {item['reason']}")
            if args.proof_only and proof_count == 0:
                results.append(
                    PhaseResult(
                        "analysis-proof",
                        (),
                        1,
                        "no enabled local proof check applies to this release level",
                    )
                )
        except (OSError, ValueError, TypeError, KeyError) as error:
            results.append(PhaseResult("check-registry", (), 1, str(error)))
    if not args.fast and not args.proof_only:
        try:
            names, version = project_version_arguments(root)
            publisher_command = [sys.executable, "publish.py", names, version, "--checks-only"]
            if snapshot_source is not None:
                publisher_command.extend(("--snapshot-source-commit", snapshot_source))
            phase_specs.append(("publisher", publisher_command, 60, False))
        except (OSError, ValueError) as error:
            results.append(PhaseResult("publisher", (), 1, str(error)))

    for index, (name, command, timeout, cacheable) in enumerate(phase_specs):
        remaining = int(invocation_deadline - time.monotonic())
        if remaining <= TERMINATION_GRACE_SECONDS:
            reason = (
                f"aggregate ai_check deadline of {TOTAL_TIMEOUT}s reached; this phase and "
                "remaining phases were not run; resume by rerunning the same ai_check command"
            )
            for unrun_name, unrun_command, _, _ in phase_specs[index:]:
                (log_dir / f"{unrun_name}.log").write_text(reason + "\n", encoding="utf-8")
                receipt_path(log_dir, unrun_name).write_text(
                    json.dumps({
                        "schema_version": RECEIPT_SCHEMA_VERSION,
                        "phase": unrun_name,
                        "status": "UNRUN",
                        "command": list(unrun_command),
                        "returncode": 125,
                        "timed_out": False,
                        "reason": reason,
                    }, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8",
                )
                results.append(
                    PhaseResult(unrun_name, unrun_command, 125, reason, skipped=True)
                )
            break
        phase_cap = phase_timeout_cap(name)
        effective_timeout = min(timeout, phase_cap, remaining)
        result = run_phase(
            root,
            log_dir,
            name,
            command,
            effective_timeout,
            reuse=cacheable,
        )
        if result.timed_out and effective_timeout < min(timeout, phase_cap):
            result.output += (
                f"\nAggregate ai_check deadline of {TOTAL_TIMEOUT}s reached during this phase; "
                "the partial log and failure receipt were retained."
            )
            (log_dir / f"{name}.log").write_text(result.output, encoding="utf-8")
            phase_receipt_path = receipt_path(log_dir, name)
            try:
                phase_receipt = json.loads(phase_receipt_path.read_text(encoding="utf-8"))
                phase_receipt["evidence_sha256"] = evidence_fingerprint(result.output)
                phase_receipt_path.write_text(
                    json.dumps(phase_receipt, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8",
                )
            except (OSError, json.JSONDecodeError):
                pass
        if name == "source-checks":
            result.allowed_failure = (
                result.returncode not in {0, 124}
                and publisher_failure_is_allowed(result.output, strict=False)
            )
        elif name == "publisher":
            result.allowed_failure = (
                result.returncode not in {0, 124}
                and publisher_failure_is_allowed(result.output, args.strict, args.candidate)
            )
        results.append(result)
        if result.timed_out and time.monotonic() >= invocation_deadline:
            reason = (
                f"aggregate ai_check deadline of {TOTAL_TIMEOUT}s reached; remaining phases "
                "were not run; resume by rerunning the same ai_check command"
            )
            for unrun_name, unrun_command, _, _ in phase_specs[index + 1:]:
                (log_dir / f"{unrun_name}.log").write_text(reason + "\n", encoding="utf-8")
                receipt_path(log_dir, unrun_name).write_text(
                    json.dumps({
                        "schema_version": RECEIPT_SCHEMA_VERSION,
                        "phase": unrun_name,
                        "status": "UNRUN",
                        "command": list(unrun_command),
                        "returncode": 125,
                        "timed_out": False,
                        "reason": reason,
                    }, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8",
                )
                results.append(
                    PhaseResult(unrun_name, unrun_command, 125, reason, skipped=True)
                )
            break
    if args.proof_only:
        for name, command in required_receipts:
            try:
                output = validate_phase_receipt(root, log_dir, name, command)
                if output is None:
                    results.append(
                        PhaseResult(
                            "receipt-" + name,
                            command,
                            1,
                            "no matching successful qualification receipt; run the owned full "
                            "candidate gate with `python3 script/ai_check.py --candidate "
                            f"--level {args.level}`",
                        )
                    )
                else:
                    results.append(
                        PhaseResult(
                            "receipt-" + name,
                            command,
                            0,
                            output,
                            reused=True,
                        )
                    )
            except (OSError, RuntimeError) as error:
                results.append(
                    PhaseResult(
                        "receipt-" + name,
                        command,
                        127,
                        f"cannot validate qualification receipt: {error}; run the owned full "
                        f"candidate gate with `python3 script/ai_check.py --candidate --level {args.level}`",
                    )
                )
    for result in results:
        print_result(result)
    write_summary(root, scope, changed, results)
    failures = [result for result in results if result.returncode and not result.allowed_failure]
    if any(result.allowed_failure for result in results):
        print("Publisher development exceptions were recorded; use --strict to reject them.")
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
