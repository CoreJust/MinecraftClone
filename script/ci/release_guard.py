#!/usr/bin/env python3
"""Fail-closed validation for the small set of publishable AI refs."""

from __future__ import annotations

import argparse
import subprocess
import sys
import re


ALLOWED_BRANCHES = frozenset(("ai-dev", "ai-main"))
CANONICAL_TAG = re.compile(r"^refs/tags/ai/([^/]+)/([0-9]+\.[0-9]+\.[0-9]+)/([0-9]+)_([0-9]{2}\.[0-9]{2}\.[0-9]{2})$")
ZERO = "0" * 40


class ReleaseRefError(ValueError):
    """A proposed remote ref violates the repository publication contract."""


def version_key(ref: str) -> str | None:
    match = CANONICAL_TAG.fullmatch(ref)
    if match is None:
        return None
    major, version, snapshot, _date = match.groups()
    return f"{major}/{version}/{snapshot}"


def validate_ref(ref: str) -> None:
    if ref.startswith("refs/heads/"):
        branch = ref.removeprefix("refs/heads/")
        if branch not in ALLOWED_BRANCHES:
            raise ReleaseRefError(f"only ai-dev and ai-main may be pushed: {ref}")
        return
    if ref.startswith("refs/tags/ai/"):
        if CANONICAL_TAG.fullmatch(ref) is None:
            raise ReleaseRefError(f"AI tags must use the canonical version scheme: {ref}")
        return
    raise ReleaseRefError(f"ref is outside the repository publication contract: {ref}")


def validate_push(lines: list[str], existing_tags: list[str], remote_tags: list[str] | None = None) -> None:
    all_existing_tags = [*existing_tags, *(remote_tags or [])]
    existing_versions = {key for tag in all_existing_tags if (key := version_key(tag))}
    pushed_versions: set[str] = set()
    for line in lines:
        fields = line.split()
        if len(fields) != 4:
            raise ReleaseRefError("pre-push input must contain four ref fields")
        _local_ref, local_sha, remote_ref, remote_sha = fields
        validate_ref(remote_ref)
        if local_sha == ZERO:
            raise ReleaseRefError(f"deleting a governed ref is not allowed: {remote_ref}")
        if remote_ref.startswith("refs/tags/"):
            if remote_sha != ZERO:
                raise ReleaseRefError(f"canonical AI tags are immutable: {remote_ref}")
            key = version_key(remote_ref)
            assert key is not None
            if key in existing_versions or key in pushed_versions:
                raise ReleaseRefError(f"one canonical tag per version is allowed: {remote_ref}")
            pushed_versions.add(key)


def _existing_ai_tags() -> list[str]:
    result = subprocess.run(
        ["git", "for-each-ref", "--format=%(refname)", "refs/tags/ai/"],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode:
        raise ReleaseRefError(result.stderr.strip() or "cannot inspect existing AI tags")
    return [line for line in result.stdout.splitlines() if line]


def _remote_ai_tags(remote: str) -> list[str]:
    result = subprocess.run(
        ["git", "ls-remote", "--refs", remote, "refs/tags/ai/*"],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode:
        raise ReleaseRefError(result.stderr.strip() or "cannot inspect destination AI tags")
    return [fields[1] for line in result.stdout.splitlines() if len(fields := line.split()) == 2]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("pre-push",))
    parser.add_argument("--remote", default="origin")
    parser.add_argument("--remote-url", default="")
    args = parser.parse_args(argv)
    try:
        validate_push(sys.stdin.read().splitlines(), _existing_ai_tags(), _remote_ai_tags(args.remote))
    except ReleaseRefError as error:
        print(f"release guard: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
