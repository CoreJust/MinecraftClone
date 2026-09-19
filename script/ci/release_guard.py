#!/usr/bin/env python3
"""Fail-closed validation for the small set of publishable AI refs."""

from __future__ import annotations

import argparse
import subprocess
import sys
import re


ALLOWED_BRANCHES = frozenset(("ai-dev", "ai-main"))
CANONICAL_TAG = re.compile(
    r"^refs/tags/ai/([^/]+)/([0-9]+\.[0-9]+\.[0-9]+)/([0-9]+)(?:-r([1-9][0-9]*))?_([0-9]{2}\.[0-9]{2}\.[0-9]{2})$"
)
ZERO = "0" * 40


class ReleaseRefError(ValueError):
    """A proposed remote ref violates the repository publication contract."""


def tag_identity(ref: str) -> tuple[str, int | None] | None:
    match = CANONICAL_TAG.fullmatch(ref)
    if match is None:
        return None
    major, version, snapshot, revision, _date = match.groups()
    return f"{major}/{version}/{snapshot}", None if revision is None else int(revision)


def version_key(ref: str) -> str | None:
    identity = tag_identity(ref)
    if identity is None:
        return None
    base, revision = identity
    suffix = "" if revision is None else f"-r{revision}"
    return f"{base}{suffix}"


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
    authoritative_tags = remote_tags if remote_tags is not None else existing_tags
    existing_identities = {identity for tag in authoritative_tags if (identity := tag_identity(tag))}
    pushed_identities: set[tuple[str, int | None]] = set()
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
            identity = tag_identity(remote_ref)
            assert identity is not None
            base, revision = identity
            known = existing_identities | pushed_identities
            if identity in known:
                raise ReleaseRefError(f"one canonical tag per version is allowed: {remote_ref}")
            if revision is None:
                if any(candidate_base == base for candidate_base, _candidate_revision in known):
                    raise ReleaseRefError(f"canonical snapshot tag already exists: {remote_ref}")
            else:
                if (base, None) not in known:
                    raise ReleaseRefError(f"correction tag requires the canonical snapshot tag: {remote_ref}")
                prior = [candidate_revision for candidate_base, candidate_revision in known
                         if candidate_base == base and candidate_revision is not None]
                expected = max(prior, default=0) + 1
                if revision != expected:
                    raise ReleaseRefError(f"next correction tag must use revision {expected}: {remote_ref}")
            pushed_identities.add(identity)


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
