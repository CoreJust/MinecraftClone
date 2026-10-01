#!/usr/bin/env python3
"""Require a successful hosted analysis receipt check for one immutable commit."""

from __future__ import annotations

import argparse
import importlib.util
import io
import json
import re
import subprocess
import sys
import zipfile
from pathlib import Path
from typing import Any, Sequence


CHECK_NAME = "S7 sanitizer and static-analysis matrix"
WORKFLOW_PATH = ".github/workflows/ai-checks.yml"
SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$")
GITHUB_REMOTE_PATTERN = re.compile(r"github\.com[:/]([^/]+/[^/]+?)(?:\.git)?$")


class GateError(RuntimeError):
    """The exact candidate has no successful hosted matrix check."""


def git(root: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", *arguments],
        cwd=root,
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise GateError(completed.stderr.strip() or "git " + " ".join(arguments) + " failed")
    return completed.stdout.strip()


def github_repository(root: Path, configured: str | None = None) -> str:
    if configured:
        value = configured.removesuffix(".git")
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", value):
            raise GateError("--repo must be OWNER/REPOSITORY")
        return value
    remote = git(root, "remote", "get-url", "origin")
    match = GITHUB_REMOTE_PATTERN.search(remote)
    if match is None:
        raise GateError(f"origin is not a GitHub repository: {remote}")
    return match.group(1)


def _check_runs(value: Any) -> list[dict[str, Any]]:
    pages = value if isinstance(value, list) else [value]
    runs: list[dict[str, Any]] = []
    for page in pages:
        if not isinstance(page, dict) or not isinstance(page.get("check_runs"), list):
            raise GateError("GitHub returned malformed check-run data")
        if page.get("total_count", 0) > len(page["check_runs"]):
            raise GateError("more than 100 check runs exist; increase API pagination before verifying")
        runs.extend(run for run in page["check_runs"] if isinstance(run, dict))
    return runs


def fetch_check_runs(repository: str, commit: str) -> list[dict[str, Any]]:
    command = [
        "gh",
        "api",
        "--method",
        "GET",
        "--paginate",
        "--slurp",
        f"repos/{repository}/commits/{commit}/check-runs",
        "-F",
        "per_page=100",
    ]
    completed = subprocess.run(command, text=True, capture_output=True, check=False)
    if completed.returncode:
        detail = (completed.stdout + completed.stderr).strip()
        raise GateError("GitHub check-run query failed; authenticate gh and retry: " + detail)
    try:
        return _check_runs(json.loads(completed.stdout))
    except json.JSONDecodeError as error:
        raise GateError(f"GitHub returned invalid check-run JSON: {error}") from error


def _github_json(endpoint: str) -> dict[str, Any]:
    completed = subprocess.run(["gh", "api", "--method", "GET", endpoint], text=True, capture_output=True, check=False)
    if completed.returncode:
        raise GateError(f"GitHub API query failed for {endpoint}: {(completed.stdout + completed.stderr).strip()}")
    try:
        value = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise GateError(f"GitHub returned invalid JSON for {endpoint}: {error}") from error
    if not isinstance(value, dict):
        raise GateError(f"GitHub returned malformed data for {endpoint}")
    return value


def fetch_workflow_run(repository: str, run_id: int) -> dict[str, Any]:
    return _github_json(f"repos/{repository}/actions/runs/{run_id}")


def fetch_aggregate_receipt(repository: str, run_id: int, commit: str) -> dict[str, Any]:
    page = _github_json(f"repos/{repository}/actions/runs/{run_id}/artifacts?per_page=100")
    artifacts = page.get("artifacts")
    if not isinstance(artifacts, list) or page.get("total_count") != len(artifacts):
        raise GateError("aggregate receipt artifact list is missing, malformed, or exceeds one page")
    matching = [artifact for artifact in artifacts if isinstance(artifact, dict) and artifact.get("name") == f"analysis-matrix-{commit}" and not artifact.get("expired")]
    if len(matching) != 1 or not isinstance(matching[0].get("id"), int):
        raise GateError("exact candidate aggregate receipt artifact is missing or ambiguous")
    completed = subprocess.run(
        ["gh", "api", "--method", "GET", f"repos/{repository}/actions/artifacts/{matching[0]['id']}/zip"],
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise GateError("aggregate receipt artifact download failed: " + completed.stderr.decode(errors="replace").strip())
    try:
        with zipfile.ZipFile(io.BytesIO(completed.stdout)) as archive:
            if archive.namelist() != ["analysis-matrix.json"]:
                raise GateError("aggregate receipt artifact has unexpected contents")
            value = json.loads(archive.read("analysis-matrix.json"))
    except (zipfile.BadZipFile, json.JSONDecodeError, OSError) as error:
        raise GateError(f"aggregate receipt artifact is invalid: {error}") from error
    if not isinstance(value, dict):
        raise GateError("aggregate receipt is not a JSON object")
    return value


def _validate_aggregate_receipt(receipt: Any, root: Path, commit: str, tree: str) -> None:
    if receipt is None:
        raise GateError("aggregate receipt is missing")
    spec = importlib.util.spec_from_file_location("ai_analysis_matrix", Path(__file__).resolve().parents[1] / "ai_analysis_matrix.py")
    if spec is None or spec.loader is None:
        raise GateError("aggregate receipt validator is unavailable")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    try:
        module.validate_receipt(receipt, module.load_manifest(root / "script/ai_analysis_matrix.json"), {"head": commit, "tree": tree})
    except module.MatrixError as error:
        raise GateError(f"aggregate receipt is invalid: {error}") from error


def verify_gate(root: Path, commit: str, repository: str | None = None) -> dict[str, Any]:
    if not SHA_PATTERN.fullmatch(commit):
        raise GateError("candidate commit must be a full lowercase SHA")
    resolved_commit = git(root, "rev-parse", "--verify", f"{commit}^{{commit}}")
    if resolved_commit != commit:
        raise GateError("candidate commit did not resolve to the requested immutable SHA")
    repo = github_repository(root, repository)
    runs = fetch_check_runs(repo, commit)
    matching = [
        run
        for run in runs
        if run.get("name") == CHECK_NAME and run.get("head_sha") == commit
    ]
    if not matching:
        raise GateError(f"{CHECK_NAME!r} has not reported on {commit}; wait for ai-dev CI before promotion")
    latest = max(matching, key=lambda run: (run.get("started_at", ""), run.get("id", 0)))
    if latest.get("status") != "completed" or latest.get("conclusion") != "success":
        raise GateError(
            f"{CHECK_NAME!r} for {commit} is not successful "
            f"(status={latest.get('status')!r}, conclusion={latest.get('conclusion')!r})"
        )
    app = latest.get("app")
    if not isinstance(app, dict) or app.get("slug") != "github-actions":
        raise GateError(f"{CHECK_NAME!r} was not emitted by GitHub Actions")
    details = latest.get("details_url")
    match = re.fullmatch(rf"https://github\.com/{re.escape(repo)}/actions/runs/([1-9][0-9]*)/job/([1-9][0-9]*)", details or "")
    if match is None or int(match.group(2)) != latest.get("id"):
        raise GateError("check run has no matching GitHub Actions workflow job URL")
    run_id = int(match.group(1))
    workflow = fetch_workflow_run(repo, run_id)
    if (workflow.get("id") != run_id or workflow.get("path") != WORKFLOW_PATH or workflow.get("head_sha") != commit
            or workflow.get("event") != "push" or workflow.get("status") != "completed" or workflow.get("conclusion") != "success"):
        raise GateError("check run does not belong to a successful exact-candidate ai-checks workflow")
    tree = git(root, "rev-parse", "--verify", f"{commit}^{{tree}}")
    _validate_aggregate_receipt(fetch_aggregate_receipt(repo, run_id, commit), root, commit, tree)
    return {
        "commit": commit,
        "repository": repo,
        "check_name": CHECK_NAME,
        "check_run_id": latest.get("id"),
        "workflow_run_id": run_id,
        "status": latest.get("status"),
        "conclusion": latest.get("conclusion"),
        "completed_at": latest.get("completed_at"),
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--commit", default="HEAD")
    parser.add_argument("--repo", help="GitHub OWNER/REPOSITORY; inferred from origin by default")
    args = parser.parse_args(argv)
    try:
        commit = git(args.root.resolve(), "rev-parse", "--verify", f"{args.commit}^{{commit}}")
        print(json.dumps(verify_gate(args.root.resolve(), commit, args.repo), sort_keys=True))
        return 0
    except (GateError, OSError, subprocess.SubprocessError) as error:
        print(f"FAIL sanitizer/static-analysis gate: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
