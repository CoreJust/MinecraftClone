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
import time
import zipfile
from pathlib import Path
from typing import Any, Sequence


CHECK_NAME = "S7 sanitizer and static-analysis matrix"
WORKFLOW_PATH = ".github/workflows/ai-checks.yml"
SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$")
GITHUB_REMOTE_PATTERN = re.compile(r"github\.com[:/]([^/]+/[^/]+?)(?:\.git)?$")


class GateError(RuntimeError):
    """The exact candidate has no successful hosted matrix check."""


class GatePending(GateError):
    """The exact candidate's matrix has not reported a terminal result yet."""


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
        raise GatePending(f"{CHECK_NAME!r} has not reported on {commit}")
    latest = max(matching, key=lambda run: (run.get("started_at", ""), run.get("id", 0)))
    app = latest.get("app")
    if not isinstance(app, dict) or app.get("slug") != "github-actions":
        raise GateError(f"{CHECK_NAME!r} was not emitted by GitHub Actions")
    details = latest.get("details_url")
    match = re.fullmatch(rf"https://github\.com/{re.escape(repo)}/actions/runs/([1-9][0-9]*)/job/([1-9][0-9]*)", details or "")
    if match is None or int(match.group(2)) != latest.get("id"):
        raise GateError("check run has no matching GitHub Actions workflow job URL")
    run_id = int(match.group(1))
    workflow = fetch_workflow_run(repo, run_id)
    if (workflow.get("id") != run_id or workflow.get("path") != WORKFLOW_PATH
            or workflow.get("head_sha") != commit or workflow.get("event") != "push"):
        raise GateError("check run does not belong to the exact-candidate ai-checks workflow")
    workflow_status = workflow.get("status")
    workflow_pending = workflow_status in {"queued", "in_progress", "waiting", "pending", "requested"}
    if not workflow_pending and workflow_status != "completed":
        raise GateError(f"exact-candidate ai-checks workflow has unexpected status {workflow_status!r}")
    if workflow_status == "completed" and workflow.get("conclusion") != "success":
        raise GateError("check run does not belong to a successful exact-candidate ai-checks workflow")

    status = latest.get("status")
    conclusion = latest.get("conclusion")
    if status in {"queued", "in_progress", "waiting", "pending", "requested"}:
        raise GatePending(
            f"{CHECK_NAME!r} for {commit} is still running (status={status!r})"
        )
    if status != "completed":
        raise GateError(f"{CHECK_NAME!r} for {commit} has unexpected status {status!r}")
    if conclusion != "success":
        raise GateError(
            f"{CHECK_NAME!r} for {commit} is not successful "
            f"(status={status!r}, conclusion={conclusion!r})"
        )
    if workflow_pending:
        raise GatePending(
            f"exact-candidate ai-checks workflow for {commit} is still running "
            f"(status={workflow_status!r})"
        )
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


def wait_for_gate(
    root: Path,
    commit: str,
    repository: str | None = None,
    *,
    timeout_seconds: int = 6 * 60 * 60,
    poll_interval_seconds: int = 30,
) -> dict[str, Any]:
    if timeout_seconds <= 0 or poll_interval_seconds <= 0:
        raise GateError("wait timeout and poll interval must be positive")

    deadline = time.monotonic() + timeout_seconds
    while True:
        try:
            return verify_gate(root, commit, repository)
        except GatePending as error:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise GateError(
                    f"timed out after {timeout_seconds}s waiting for the exact analysis matrix "
                    f"for {commit}: {error}"
                ) from error
            elapsed = timeout_seconds - remaining
            delay = min(poll_interval_seconds, remaining)
            print(
                f"Waiting for the exact analysis matrix for {commit}: {error}; "
                f"elapsed {elapsed:.0f}s, retrying in {delay:.0f}s",
                flush=True,
            )
            time.sleep(delay)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--commit", default="HEAD")
    parser.add_argument("--repo", help="GitHub OWNER/REPOSITORY; inferred from origin by default")
    parser.add_argument("--wait", action="store_true", help="poll until the exact matrix succeeds or fails")
    parser.add_argument("--timeout-seconds", type=int, default=6 * 60 * 60)
    parser.add_argument("--poll-interval-seconds", type=int, default=30)
    args = parser.parse_args(argv)
    try:
        commit = git(args.root.resolve(), "rev-parse", "--verify", f"{args.commit}^{{commit}}")
        result = (
            wait_for_gate(
                args.root.resolve(),
                commit,
                args.repo,
                timeout_seconds=args.timeout_seconds,
                poll_interval_seconds=args.poll_interval_seconds,
            )
            if args.wait
            else verify_gate(args.root.resolve(), commit, args.repo)
        )
        print(json.dumps(result, sort_keys=True))
        return 0
    except (GateError, OSError, subprocess.SubprocessError) as error:
        print(f"FAIL sanitizer/static-analysis gate: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
