#!/usr/bin/env python3
"""Verify that a published GitHub Release contains the accepted snapshot bytes."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
import urllib.parse
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Protocol, Sequence


PLATFORMS = ("macos", "windows", "android")
REVISION_RE = re.compile(r"[0-9a-f]{40}")
SHA256_RE = re.compile(r"[0-9a-f]{64}")
VERSION_RE = re.compile(r"\d+\.\d+\.\d+:\d+")
REPOSITORY_RE = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
TAG_RE = re.compile(r"[A-Za-z0-9._/-]+")
ASSET_NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")


class ReleaseVerificationError(RuntimeError):
    """Raised when published release identity or bytes do not match acceptance evidence."""


class GitHubClient(Protocol):
    def json(self, arguments: Sequence[str]) -> Any: ...

    def download(self, repository: str, tag: str, name: str, destination: Path) -> None: ...


class GhClient:
    """Small read-only adapter around the GitHub CLI."""

    def __init__(self, executable: str = "gh") -> None:
        self.executable = executable

    def _run(self, arguments: Sequence[str]) -> str:
        command = [self.executable, *arguments]
        try:
            completed = subprocess.run(command, text=True, capture_output=True, check=False)
        except OSError as error:
            raise ReleaseVerificationError(f"could not start {self.executable}: {error}") from error
        if completed.returncode:
            raise ReleaseVerificationError(
                f"read-only GitHub command failed with exit code {completed.returncode}"
            )
        return completed.stdout

    def json(self, arguments: Sequence[str]) -> Any:
        try:
            return json.loads(self._run(arguments))
        except json.JSONDecodeError as error:
            raise ReleaseVerificationError("GitHub returned invalid JSON") from error

    def download(self, repository: str, tag: str, name: str, destination: Path) -> None:
        self._run([
            "release",
            "download",
            tag,
            "--repo",
            repository,
            "--pattern",
            name,
            "--dir",
            str(destination),
        ])


@dataclass(frozen=True)
class ExpectedAsset:
    platform: str
    version: str
    source_commit: str
    name: str
    size: int
    sha256: str
    evidence_name: str
    evidence_sha256: str


def canonical_json(value: Any) -> str:
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(source: Path) -> str:
    digest = hashlib.sha256()
    with source.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def require_revision(value: str) -> str:
    if not REVISION_RE.fullmatch(value):
        raise ReleaseVerificationError(
            "source commit must be a 40-character lowercase hexadecimal revision"
        )
    return value


def require_repository(value: str) -> str:
    if not REPOSITORY_RE.fullmatch(value) or ".." in value:
        raise ReleaseVerificationError("repository must use the exact OWNER/REPOSITORY form")
    return value


def require_tag(value: str) -> str:
    parts = value.split("/")
    if (
        not TAG_RE.fullmatch(value)
        or value.startswith(("-", "/"))
        or value.endswith("/")
        or any(part in {"", ".", ".."} for part in parts)
    ):
        raise ReleaseVerificationError("tag contains unsafe or unsupported characters")
    return value


def require_asset_name(value: Any) -> str:
    if not isinstance(value, str) or not ASSET_NAME_RE.fullmatch(value) or ".." in value:
        raise ReleaseVerificationError("release asset name contains unsafe or unsupported characters")
    return value


def require_regular_file(source: Path, label: str) -> Path:
    if source.is_symlink() or not source.is_file():
        raise ReleaseVerificationError(f"{label} must be an existing regular file: {source}")
    return source.resolve()


def load_expected_asset(evidence_path: Path) -> ExpectedAsset:
    source = require_regular_file(evidence_path, "release evidence")
    try:
        evidence = json.loads(source.read_text(encoding="utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ReleaseVerificationError(f"release evidence is not valid UTF-8 JSON: {source.name}") from error
    if not isinstance(evidence, dict) or evidence.get("schema") != 1:
        raise ReleaseVerificationError(f"release evidence must use schema 1: {source.name}")
    platform = evidence.get("platform")
    if platform not in PLATFORMS:
        raise ReleaseVerificationError(f"release evidence has an unsupported platform: {source.name}")
    artifact = evidence.get("artifact")
    if platform == "android":
        signing = evidence.get("signing")
        if (
            evidence.get("kind") != "android-apk-evidence"
            or evidence.get("source_exactness") != "exact"
            or not isinstance(signing, dict)
            or signing.get("classification") != "development"
        ):
            raise ReleaseVerificationError(
                "Android release evidence must describe an exact development-signed package"
            )
        artifact = evidence.get("apk")
    if not isinstance(artifact, dict):
        raise ReleaseVerificationError(f"release evidence has no artifact identity: {source.name}")
    name = require_asset_name(artifact.get("name"))
    if platform == "android" and "ci-development" in name:
        raise ReleaseVerificationError("the CI-only development APK is not a publishable Android asset")
    size = artifact.get("bytes")
    digest = artifact.get("sha256")
    version = evidence.get("version")
    revision = evidence.get("source_commit")
    if not isinstance(size, int) or isinstance(size, bool) or size < 1:
        raise ReleaseVerificationError(f"release evidence has an invalid artifact byte count: {source.name}")
    if not isinstance(digest, str) or not SHA256_RE.fullmatch(digest):
        raise ReleaseVerificationError(f"release evidence has an invalid artifact SHA-256: {source.name}")
    if not isinstance(version, str) or not VERSION_RE.fullmatch(version):
        raise ReleaseVerificationError(f"release evidence has an invalid version: {source.name}")
    if not isinstance(revision, str):
        raise ReleaseVerificationError(f"release evidence has no source commit: {source.name}")
    return ExpectedAsset(
        platform=platform,
        version=version,
        source_commit=require_revision(revision),
        name=name,
        size=size,
        sha256=digest,
        evidence_name=source.name,
        evidence_sha256=sha256_file(source),
    )


def load_expected_assets(evidence_paths: Sequence[Path], expected_commit: str) -> dict[str, ExpectedAsset]:
    if len(evidence_paths) != len(PLATFORMS):
        raise ReleaseVerificationError("exactly one macOS, Windows, and Android evidence file is required")
    result: dict[str, ExpectedAsset] = {}
    versions: set[str] = set()
    names: set[str] = set()
    for evidence_path in evidence_paths:
        asset = load_expected_asset(evidence_path)
        if asset.platform in result:
            raise ReleaseVerificationError(f"duplicate release evidence for {asset.platform}")
        if asset.source_commit != expected_commit:
            raise ReleaseVerificationError(
                f"{asset.platform} evidence source commit does not match the expected commit"
            )
        if asset.name in names or f"{asset.name}.sha256" in names:
            raise ReleaseVerificationError("release evidence selects colliding asset names")
        names.update((asset.name, f"{asset.name}.sha256"))
        versions.add(asset.version)
        result[asset.platform] = asset
    if set(result) != set(PLATFORMS):
        missing = sorted(set(PLATFORMS) - set(result))
        raise ReleaseVerificationError("missing release evidence for: " + ", ".join(missing))
    if len(versions) != 1:
        raise ReleaseVerificationError("release evidence does not describe one exact version")
    return result


def object_identity(value: Any, label: str) -> tuple[str, str]:
    if not isinstance(value, dict):
        raise ReleaseVerificationError(f"GitHub {label} response has no object identity")
    kind = value.get("type")
    revision = value.get("sha")
    if kind not in {"commit", "tag"} or not isinstance(revision, str):
        raise ReleaseVerificationError(f"GitHub {label} response has an invalid object identity")
    return kind, require_revision(revision)


def resolve_tag(repository: str, tag: str, github: GitHubClient) -> dict[str, str]:
    encoded_tag = urllib.parse.quote(tag, safe="")
    reference = github.json(["api", f"repos/{repository}/git/ref/tags/{encoded_tag}"])
    if not isinstance(reference, dict) or reference.get("ref") != f"refs/tags/{tag}":
        raise ReleaseVerificationError("GitHub tag reference does not match the requested tag")
    initial_type, initial_sha = object_identity(reference.get("object"), "tag reference")
    current_type = initial_type
    current_sha = initial_sha
    visited: set[str] = set()
    while current_type == "tag":
        if current_sha in visited or len(visited) >= 8:
            raise ReleaseVerificationError("annotated tag chain is cyclic or too deep")
        visited.add(current_sha)
        tag_object = github.json(["api", f"repos/{repository}/git/tags/{current_sha}"])
        if not isinstance(tag_object, dict) or tag_object.get("sha") not in {None, current_sha}:
            raise ReleaseVerificationError("GitHub annotated tag object identity does not match its request")
        current_type, current_sha = object_identity(
            tag_object.get("object") if isinstance(tag_object, dict) else None,
            "annotated tag",
        )
    return {
        "ref": f"refs/tags/{tag}",
        "object_type": initial_type,
        "object_sha": initial_sha,
        "commit": current_sha,
    }


def release_assets(release: Any, tag: str) -> tuple[dict[str, dict[str, Any]], dict[str, Any]]:
    if not isinstance(release, dict) or release.get("tagName") != tag:
        raise ReleaseVerificationError("GitHub Release tag does not match the requested tag")
    if release.get("isDraft") is not False:
        raise ReleaseVerificationError("GitHub Release must be published, not a draft")
    body = release.get("body")
    if not isinstance(body, str) or not body.strip():
        raise ReleaseVerificationError("GitHub Release must contain release notes")
    raw_assets = release.get("assets")
    if not isinstance(raw_assets, list):
        raise ReleaseVerificationError("GitHub Release returned no asset list")
    assets: dict[str, dict[str, Any]] = {}
    for item in raw_assets:
        if not isinstance(item, dict):
            raise ReleaseVerificationError("GitHub Release returned an invalid asset identity")
        name = item.get("name")
        if not isinstance(name, str):
            raise ReleaseVerificationError("GitHub Release returned an invalid asset identity")
        if name in assets:
            raise ReleaseVerificationError(f"GitHub Release returned duplicate asset name: {name}")
        assets[name] = item
    identity = {
        "database_id": release.get("databaseId"),
        "id": release.get("id"),
        "url": release.get("url"),
        "is_prerelease": release.get("isPrerelease"),
        "notes_bytes": len(body.encode("utf-8")),
        "notes_sha256": sha256_bytes(body.encode("utf-8")),
    }
    return assets, identity


def checked_github_asset(
    item: dict[str, Any],
    expected_size: int,
    expected_sha256: str,
    name: str,
) -> dict[str, Any]:
    size = item.get("size")
    if not isinstance(size, int) or isinstance(size, bool) or size != expected_size:
        raise ReleaseVerificationError(f"GitHub asset byte count does not match accepted evidence: {name}")
    github_digest = item.get("digest")
    if github_digest not in (None, "") and github_digest != f"sha256:{expected_sha256}":
        raise ReleaseVerificationError(f"GitHub asset digest does not match accepted evidence: {name}")
    result: dict[str, Any] = {"name": name, "bytes": size}
    for source_name, output_name in (
        ("apiUrl", "api_url"),
        ("databaseId", "database_id"),
        ("digest", "github_digest"),
        ("id", "id"),
        ("url", "url"),
    ):
        if item.get(source_name) not in (None, ""):
            result[output_name] = item[source_name]
    return result


def download_and_hash(
    github: GitHubClient,
    repository: str,
    tag: str,
    name: str,
    expected_size: int,
    expected_sha256: str,
    destination: Path,
) -> None:
    target = destination / name
    if target.exists() or target.is_symlink():
        raise ReleaseVerificationError(f"download target already exists: {name}")
    github.download(repository, tag, name, destination)
    if target.is_symlink() or not target.is_file():
        raise ReleaseVerificationError(f"GitHub did not download one regular asset: {name}")
    if target.stat().st_size != expected_size:
        raise ReleaseVerificationError(f"downloaded asset byte count mismatch: {name}")
    actual_sha256 = sha256_file(target)
    if actual_sha256 != expected_sha256:
        raise ReleaseVerificationError(
            f"downloaded asset SHA-256 mismatch: {name}; expected {expected_sha256}, got {actual_sha256}"
        )


def verify_release(
    repository: str,
    tag: str,
    expected_commit: str,
    evidence_paths: Sequence[Path],
    output: Path,
    github: GitHubClient,
) -> dict[str, Any]:
    repository = require_repository(repository)
    tag = require_tag(tag)
    expected_commit = require_revision(expected_commit)
    if output.exists() or output.is_symlink() or not output.parent.is_dir():
        raise ReleaseVerificationError("output must be a new file in an existing directory")
    expected_assets = load_expected_assets(evidence_paths, expected_commit)
    tag_identity = resolve_tag(repository, tag, github)
    if tag_identity["commit"] != expected_commit:
        raise ReleaseVerificationError("GitHub tag does not resolve to the expected source commit")
    release = github.json([
        "release",
        "view",
        tag,
        "--repo",
        repository,
        "--json",
        "tagName,isDraft,isPrerelease,body,databaseId,id,url,assets",
    ])
    published_assets, release_identity = release_assets(release, tag)

    checks: list[dict[str, Any]] = []
    expected_downloads: set[str] = set()
    with tempfile.TemporaryDirectory(prefix="minecraftclone-release-verify-") as temporary:
        download_root = Path(temporary)
        for platform in PLATFORMS:
            expected = expected_assets[platform]
            checksum_name = f"{expected.name}.sha256"
            checksum_bytes = f"{expected.sha256}  {expected.name}\n".encode("utf-8")
            selected = (
                (expected.name, expected.size, expected.sha256),
                (checksum_name, len(checksum_bytes), sha256_bytes(checksum_bytes)),
            )
            github_identities: dict[str, Any] = {}
            for name, size, digest in selected:
                item = published_assets.get(name)
                if item is None:
                    raise ReleaseVerificationError(f"GitHub Release is missing required asset: {name}")
                github_identities[name] = checked_github_asset(item, size, digest, name)
                download_and_hash(github, repository, tag, name, size, digest, download_root)
                expected_downloads.add(name)
            checks.append({
                "platform": platform,
                "version": expected.version,
                "source_commit": expected.source_commit,
                "artifact": {
                    "name": expected.name,
                    "bytes": expected.size,
                    "sha256": expected.sha256,
                },
                "checksum_asset": {
                    "name": checksum_name,
                    "bytes": len(checksum_bytes),
                    "sha256": sha256_bytes(checksum_bytes),
                },
                "expected_evidence": {
                    "name": expected.evidence_name,
                    "sha256": expected.evidence_sha256,
                },
                "github_assets": github_identities,
            })
        downloaded = {item.name for item in download_root.iterdir()}
        if downloaded != expected_downloads or any(item.is_symlink() for item in download_root.iterdir()):
            raise ReleaseVerificationError("GitHub download produced unexpected files")

    result = {
        "schema": 1,
        "kind": "github-release-verification",
        "repository": repository,
        "tag": tag_identity,
        "release": release_identity,
        "version": next(iter(expected_assets.values())).version,
        "source_commit": expected_commit,
        "assets": checks,
        "verification": {
            "status": "passed",
            "scope": "exact published asset bytes; downloaded files were not executed or extracted",
        },
    }
    with output.open("x", encoding="utf-8") as stream:
        stream.write(canonical_json(result))
    return result


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--repository", required=True, help="exact OWNER/REPOSITORY")
    result.add_argument("--tag", required=True, help="exact published release tag")
    result.add_argument("--source-commit", required=True, help="expected 40-character commit")
    result.add_argument(
        "--evidence",
        type=Path,
        action="append",
        required=True,
        help="schema-1 release evidence; pass once for macOS, Windows, and Android",
    )
    result.add_argument("--output", type=Path, required=True)
    return result


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        result = verify_release(
            args.repository,
            args.tag,
            args.source_commit,
            args.evidence,
            args.output,
            GhClient(),
        )
    except (OSError, ReleaseVerificationError) as error:
        print(f"release verification failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps({
        "output": str(args.output),
        "repository": result["repository"],
        "source_commit": result["source_commit"],
        "status": result["verification"]["status"],
        "tag": result["tag"]["ref"],
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
