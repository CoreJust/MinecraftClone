#!/usr/bin/env python3
"""Verify a repair to an existing published snapshot and create its local tag."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path, PurePosixPath
from xml.etree import ElementTree
from zoneinfo import ZoneInfo

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ai_publish
from release_guard import CANONICAL_TAG, ZERO

PLATFORMS = {"windows": ".exe", "macos": ".tar.gz", "android": ".apk"}
HASH = re.compile(r"[0-9a-f]{64}")
TASK = re.compile(r"MC-AI-[0-9]{4}")
DESKTOP_CLIENT = {"src/client/PlayerClient.cpp", "src/client/include/client/PlayerClient.hpp"}
MAC_PACKAGING = {"script/package_snapshot.py", "script/ci/build_snapshot.py"}
WINDOWS_LAUNCHER = "script/windows-singleplayer-launcher/launcher.cpp"
GIT = ai_publish.git


class CorrectionError(ValueError):
    """The correction or a required qualification does not match its receipt."""


def object_fields(value: object, fields: set[str], label: str) -> dict:
    if not isinstance(value, dict) or value.keys() != fields:
        raise CorrectionError(f"{label} must have exactly these fields: {', '.join(sorted(fields))}")
    return value


def load_json(path: Path) -> dict:
    def unique_keys(pairs: list[tuple[str, object]]) -> dict:
        result = dict(pairs)
        if len(result) != len(pairs):
            raise CorrectionError(f"duplicate JSON field in {path}")
        return result

    value = json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=unique_keys)
    if not isinstance(value, dict):
        raise CorrectionError(f"{path} must contain a JSON object")
    return value


def verified_file(directory: Path, entry: object, label: str, suffix: str = "") -> Path:
    fields = object_fields(entry, {"path", "sha256"}, label)
    filename, digest = fields["path"], fields["sha256"]
    if not isinstance(filename, str) or not filename or not isinstance(digest, str) or not HASH.fullmatch(digest):
        raise CorrectionError(f"{label} requires a path and SHA-256")
    path = directory / filename
    if path.is_symlink() or not path.is_file() or not path.stat().st_size:
        raise CorrectionError(f"{label} must be a nonempty regular file: {path}")
    if not path.name.endswith(suffix):
        raise CorrectionError(f"{label} requires a {suffix} file: {path}")
    with path.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != digest:
            raise CorrectionError(f"{label} SHA-256 mismatch: {path}")
    return path


def applicable_paths(paths: set[str], platform: str) -> set[str]:
    result = {name for name in paths if name.startswith("src/") or name in {
        "CMakeLists.txt", "CMakePresets.json", "dependencies.lock.json", "vcpkg.json", "vcpkg-configuration.json",
    } or name.startswith(("assets/", "cmake/", "third_party/"))}
    if platform == "android":
        result -= DESKTOP_CLIENT
        result.update(name for name in paths if name.startswith("android/"))
    else:
        result = {name for name in result if not name.startswith("src/android/")}
    if platform == "windows":
        result.update(paths & {WINDOWS_LAUNCHER})
    if platform == "macos":
        result.update(paths & MAC_PACKAGING)
    return result


def verify_test_report(path: Path) -> None:
    if path.suffix == ".xml":
        result = ElementTree.parse(path).getroot()
        cases = list(result.iter("testcase"))
        failed = result.tag not in {"testsuite", "testsuites"} or result.get("failures") != "0"
        failed |= result.get("errors", "0") != "0"
        failed |= any(case.find("failure") is not None or case.find("error") is not None for case in cases)
        passed = sum(case.find("skipped") is None and case.get("status") != "notrun" for case in cases)
    elif path.suffix == ".json":
        totals = load_json(path).get("reconciled", {})
        if not isinstance(totals, dict) or any(
            type(totals.get(field)) is not int for field in ("passed", "failed", "errors")
        ):
            raise CorrectionError(f"invalid basic test totals: {path}")
        failed = totals["failed"] != 0 or totals["errors"] != 0
        passed = totals["passed"]
    else:
        output = path.read_text(encoding="utf-8").strip()
        result = re.search(r"^Ran (\d+) tests? in [\d.]+s\n\nOK\Z", output, re.MULTILINE)
        failed = result is None
        passed = int(result[1]) if result else 0
    if failed or type(passed) is not int or passed < 1:
        raise CorrectionError(f"basic test report must record passing tests and zero failures: {path}")


def verify_platforms(root: Path, directory: Path, receipt: dict, base: str, promoted: str) -> None:
    raw_paths = GIT(root, "diff", "--name-only", "--no-renames", "-z", base, promoted)
    paths = set(raw_paths.rstrip("\0").split("\0"))
    platforms = object_fields(receipt, set(PLATFORMS), "platforms")
    for platform, suffix in PLATFORMS.items():
        entry = object_fields(platforms[platform], {"artifact", "source_files", "tests", "runtime"}, platform)
        verified_file(directory, entry["artifact"], f"{platform} artifact", suffix)
        reports = entry["tests"]
        if not isinstance(reports, list) or not reports:
            raise CorrectionError(f"{platform} requires at least one basic test report")
        for report in reports:
            verify_test_report(verified_file(directory, report, f"{platform} basic tests"))
        verified_file(directory, entry["runtime"], f"{platform} native runtime")
        required = applicable_paths(paths, platform)
        source_files = entry["source_files"]
        if not isinstance(source_files, dict) or not source_files or not required <= source_files.keys():
            missing = required - source_files.keys() if isinstance(source_files, dict) else required
            raise CorrectionError(f"{platform} source receipt is missing changed inputs: {', '.join(sorted(missing))}")
        for name, digest in source_files.items():
            path = PurePosixPath(name)
            if path.is_absolute() or ".." in path.parts or path.as_posix() != name or "\\" in name:
                raise CorrectionError(f"invalid source receipt path: {name}")
            if not isinstance(digest, str) or not HASH.fullmatch(digest):
                raise CorrectionError(f"invalid source SHA-256 for {platform}: {name}")
            blob = subprocess.check_output(["git", "show", f"{promoted}:{name}"], cwd=root)
            if hashlib.sha256(blob).hexdigest() != digest:
                raise CorrectionError(f"{platform} source SHA-256 mismatch: {name}")


def platform_digest(platforms: object) -> str:
    payload = json.dumps(platforms, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    return hashlib.sha256(payload).hexdigest()


def snapshot_task(root: Path, revision: str) -> str:
    output = GIT(root, "show", "-s", "--format=%(trailers:key=Task-ID,valueonly,unfold)", revision)
    task_id = output.strip()
    if not TASK.fullmatch(task_id):
        raise CorrectionError(f"{revision} requires exactly one Task-ID trailer")
    return task_id


def verify_source_policy(root: Path, names: str, version: str, published_source: str) -> None:
    command = [sys.executable, "publish.py", names, version, "--checks-only",
        "--snapshot-source-commit", published_source]
    if subprocess.run(command, cwd=root, check=False).returncode:
        raise CorrectionError("correction source-policy checks failed")


def verify_prior_tag(root: Path, name: str, base: str, promoted: str) -> tuple[str, str, set[str]]:
    match = CANONICAL_TAG.fullmatch(f"refs/tags/{name}")
    if match is None or match[4] is None:
        raise CorrectionError("correction tag must use the canonical AI snapshot -rN scheme")
    major, version, snapshot, revision, date = match.groups()
    numbers: dict[int, str] = {}
    all_tags = GIT(root, "tag", "--list", f"ai/{major}/{version}/{snapshot}_*",
        f"ai/{major}/{version}/{snapshot}-r*_*").splitlines()
    for tag in all_tags:
        if tag == name:
            continue
        entry = CANONICAL_TAG.fullmatch(f"refs/tags/{tag}")
        if entry is not None:
            number = int(entry[4] or 0)
            if number in numbers:
                raise CorrectionError("duplicate prior snapshot tag identity")
            numbers[number] = tag
    if 0 not in numbers or int(revision) != max(numbers) + 1:
        raise CorrectionError("correction tag must follow the existing snapshot series")
    prior = numbers[max(numbers)]
    required_tags = {numbers[0], prior}
    for tag in required_tags:
        if GIT(root, "cat-file", "-t", f"refs/tags/{tag}").strip() != "tag":
            raise CorrectionError(f"prior snapshot tag must be annotated: {tag}")
    if ai_publish.revision(root, f"refs/tags/{prior}") != base:
        raise CorrectionError("correction base does not match the prior immutable snapshot tag")
    expected_date = datetime.fromtimestamp(int(GIT(root, "show", "-s", "--format=%ct", promoted)),
        ZoneInfo("Europe/Belgrade")).strftime("%y.%m.%d")
    if date != expected_date:
        raise CorrectionError("correction tag date differs from the promotion date")
    return major, f"{version}:{snapshot}", {f"refs/tags/{tag}" for tag in required_tags}


def verify_remote_base(root: Path, remote: str, expected_heads: set[str], prior_refs: set[str]) -> None:
    branch = "refs/heads/ai-main"
    rows = GIT(root, "ls-remote", "--refs", "--", remote, branch, *sorted(prior_refs)).splitlines()
    refs = {ref: sha for sha, ref in (row.split() for row in rows)}
    if refs.get(branch) not in expected_heads:
        raise CorrectionError("remote ai-main differs from the reviewed correction base")
    for ref in sorted(prior_refs):
        if refs.get(ref) != GIT(root, "rev-parse", "--verify", ref).strip():
            raise CorrectionError(f"local prior annotated tag differs from the remote: {ref}")


def verify(
    root: Path, receipt_path: Path, base: str, tag: str, promoted: str,
    push_ref: str | None = None, remote_sha: str | None = None, remote: str | None = None,
) -> str:
    ai_publish.require_exact_commit(base)
    ai_publish.require_exact_commit(promoted)
    ai_publish.require_branch(root, "ai-main")
    ai_publish.require_clean(root)
    if ai_publish.revision(root, "HEAD") != promoted:
        raise CorrectionError("correction promotion must be the checked HEAD")
    parents = GIT(root, "show", "-s", "--format=%P", promoted).split()
    if len(parents) != 2 or parents[0] != base:
        raise CorrectionError("correction must be a two-parent promotion from the prior ai-main tip")
    source = parents[1]
    prior_parents = GIT(root, "show", "-s", "--format=%P", base).split()
    if len(prior_parents) != 2 or source in prior_parents or subprocess.run(
        ["git", "merge-base", "--is-ancestor", prior_parents[1], source], cwd=root, check=False
    ).returncode != 0:
        raise CorrectionError("correction source must extend the prior published source")
    tree = GIT(root, "rev-parse", f"{promoted}^{{tree}}").strip()
    if tree != ai_publish.expected_merge_tree(root, base, source):
        raise CorrectionError("correction promotion differs from the deterministic merge tree")
    task_id = snapshot_task(root, source)
    if snapshot_task(root, promoted) != task_id:
        raise CorrectionError("promotion and correction source Task-ID trailers differ")
    major, version, prior_refs = verify_prior_tag(root, tag, base, promoted)
    names, actual_version = ai_publish.ai_check.project_version_arguments(root)
    if names.partition(":")[0] != major or actual_version != version:
        raise CorrectionError("correction tag does not match the embedded snapshot version")

    if push_ref == "refs/heads/ai-main":
        if remote_sha != base:
            raise CorrectionError("ai-main push must extend the exact checked remote tip")
    elif push_ref == f"refs/tags/{tag}":
        if remote_sha != ZERO:
            raise CorrectionError("correction tag must never replace an existing remote tag")
        if GIT(root, "cat-file", "-t", push_ref).strip() != "tag" or ai_publish.revision(root, push_ref) != promoted:
            raise CorrectionError("correction tag must be annotated and point to the checked promotion")
    elif push_ref is not None or remote_sha is not None:
        raise CorrectionError("unsupported correction push destination")
    if remote is not None:
        expected_heads = {base, promoted} if push_ref == f"refs/tags/{tag}" else {base}
        verify_remote_base(root, remote, expected_heads, prior_refs)

    receipt = object_fields(load_json(receipt_path), {"base", "source", "tree", "review", "platforms"}, "receipt")
    identity = {"base": base, "source": source, "tree": tree}
    if any(receipt[key] != value for key, value in identity.items()):
        raise CorrectionError("qualification receipt differs from the exact correction identity")
    review_path = verified_file(receipt_path.parent, receipt["review"], "independent review")
    review = object_fields(
        load_json(review_path), {*identity, "platforms_sha256", "verdict", "reviewer", "evidence"}, "review",
    )
    if any(review[key] != value for key, value in identity.items()) or review["verdict"] != "approved":
        raise CorrectionError("independent review does not approve the exact correction identity")
    if any(not isinstance(review[key], str) or not review[key].strip() for key in ("reviewer", "evidence")):
        raise CorrectionError("independent review requires reviewer and evidence")
    verify_platforms(root, receipt_path.parent, receipt["platforms"], base, promoted)
    if review["platforms_sha256"] != platform_digest(receipt["platforms"]):
        raise CorrectionError("independent review does not approve the exact platform receipts")
    verify_source_policy(root, names, version, prior_parents[1])
    return task_id


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("verify", "tag"))
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2], help=argparse.SUPPRESS)
    parser.add_argument("--receipt", type=Path, default=os.getenv("MC_SNAPSHOT_CORRECTION_RECEIPT"))
    parser.add_argument("--base", help="checked remote ai-main base, defaults to the receipt base")
    parser.add_argument("--tag", required=True)
    parser.add_argument("--promoted")
    parser.add_argument("--push-ref")
    parser.add_argument("--remote-sha")
    parser.add_argument("--remote", help="verify immutable prior refs against this remote")
    args = parser.parse_args()
    try:
        root = args.root.resolve()
        if args.receipt is None:
            raise CorrectionError("correction receipt is required")
        receipt_path = args.receipt.resolve()
        base = args.base or load_json(receipt_path).get("base")
        if not isinstance(base, str):
            raise CorrectionError("correction base must be an immutable commit")
        promoted = args.promoted or ai_publish.revision(root, "HEAD")
        task_id = verify(root, receipt_path, base, args.tag, promoted, args.push_ref, args.remote_sha, args.remote)
        if args.action == "tag":
            if args.push_ref is not None:
                raise CorrectionError("tag creation does not accept push arguments")
            GIT(root, "tag", "-a", args.tag, promoted, "-m", f"AI snapshot {task_id}")
        print(f"Verified correction {args.tag} at {promoted}")
    except (ValueError, ElementTree.ParseError, ai_publish.PublishError, OSError, subprocess.CalledProcessError) as error:
        print(f"snapshot correction: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
