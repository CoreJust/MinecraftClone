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
REQUIRED_PRODUCER_JOBS = frozenset(
    {
        "fast (ubuntu-latest)",
        "fast (windows-2022)",
        "desktop (macos, debug)",
        "desktop (macos, release)",
        "desktop (windows, debug)",
        "desktop (windows, release)",
        "Linux analysis (linux_lsan)",
        "Linux analysis (linux_msan)",
        "android-hwasan-build",
        "android-hwasan-runtime",
    }
)
NONTERMINAL_JOB_STATUSES = frozenset({"queued", "in_progress", "waiting", "pending", "requested"})
NONTERMINAL_WORKFLOW_STATUSES = frozenset({"queued", "in_progress", "waiting", "pending", "requested"})
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


def fetch_workflow_runs(repository: str, commit: str) -> list[dict[str, Any]]:
    command = [
        "gh",
        "api",
        "--method",
        "GET",
        "--paginate",
        "--slurp",
        f"repos/{repository}/actions/workflows/ai-checks.yml/runs",
        "-F",
        f"head_sha={commit}",
        "-F",
        "event=push",
        "-F",
        "per_page=100",
    ]
    completed = subprocess.run(command, text=True, capture_output=True, check=False)
    if completed.returncode:
        detail = (completed.stdout + completed.stderr).strip()
        raise GateError("GitHub workflow-run query failed; authenticate gh and retry: " + detail)
    try:
        pages = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise GateError(f"GitHub returned invalid workflow-run JSON: {error}") from error
    if not isinstance(pages, list):
        raise GateError("GitHub returned malformed workflow-run data")
    runs: list[dict[str, Any]] = []
    for page in pages:
        if not isinstance(page, dict) or not isinstance(page.get("workflow_runs"), list):
            raise GateError("GitHub returned malformed workflow-run data")
        total_count = page.get("total_count")
        if not isinstance(total_count, int) or total_count > len(page["workflow_runs"]):
            raise GateError("more than 100 workflow runs exist; increase API pagination before verifying")
        if any(not isinstance(run, dict) for run in page["workflow_runs"]):
            raise GateError("GitHub returned malformed workflow-run data")
        runs.extend(page["workflow_runs"])
    return runs


def fetch_workflow_jobs(repository: str, run_id: int, attempt: int) -> list[dict[str, Any]]:
    page = _github_json(
        f"repos/{repository}/actions/runs/{run_id}/attempts/{attempt}/jobs?per_page=100"
    )
    jobs = page.get("jobs")
    if (
        not isinstance(jobs, list)
        or not isinstance(page.get("total_count"), int)
        or page["total_count"] != len(jobs)
        or any(not isinstance(job, dict) for job in jobs)
    ):
        raise GateError("workflow job list is missing, malformed, or exceeds one page")
    return jobs


def _is_expected_workflow_path(value: Any) -> bool:
    if not isinstance(value, str):
        return False
    if value == WORKFLOW_PATH:
        return True
    ref_suffix = value.removeprefix(WORKFLOW_PATH + "@")
    return ref_suffix != value and bool(ref_suffix)


def latest_exact_workflow_run(repository: str, commit: str) -> dict[str, Any] | None:
    runs = [
        run
        for run in fetch_workflow_runs(repository, commit)
        if _is_expected_workflow_path(run.get("path"))
        and run.get("head_sha") == commit
        and run.get("event") == "push"
        and isinstance(run.get("id"), int)
        and isinstance(run.get("run_attempt"), int)
    ]
    if not runs:
        return None
    return max(
        runs,
        key=lambda run: (
            run.get("created_at", ""),
            run.get("run_number", 0),
            run["run_attempt"],
            run["id"],
        ),
    )


def _raise_for_failed_producer(
    jobs: list[dict[str, Any]], commit: str, workflow_status: Any
) -> None:
    producers = [job for job in jobs if job.get("name") in REQUIRED_PRODUCER_JOBS]
    failed = [
        job
        for job in producers
        if job.get("status") == "completed" and job.get("conclusion") != "success"
    ]
    if failed:
        details = ", ".join(
            f"{job.get('name')}={job.get('conclusion')}" for job in sorted(failed, key=lambda item: item.get("name", ""))
        )
        raise GateError(f"required analysis producer failed for {commit}: {details}")
    unexpected = [
        job for job in producers
        if job.get("status") not in NONTERMINAL_JOB_STATUSES | {"completed"}
    ]
    if unexpected:
        details = ", ".join(
            f"{job.get('name')} status={job.get('status')!r}"
            for job in sorted(unexpected, key=lambda item: item.get("name", ""))
        )
        raise GateError(f"required analysis producer has unexpected status for {commit}: {details}")
    missing = REQUIRED_PRODUCER_JOBS - {job.get("name") for job in producers}
    if missing:
        names = ", ".join(sorted(missing))
        if workflow_status in NONTERMINAL_WORKFLOW_STATUSES:
            raise GatePending(
                f"required analysis producers have not appeared yet for {commit} "
                f"(workflow status={workflow_status!r}): {names}"
            )
        raise GateError(
            f"required analysis producers are missing for {commit}: {names}"
        )


def _check_latest_producers(repository: str, workflow: dict[str, Any], commit: str) -> list[dict[str, Any]]:
    run_id = workflow.get("id")
    attempt = workflow.get("run_attempt")
    if not isinstance(run_id, int) or not isinstance(attempt, int) or attempt <= 0:
        raise GateError("exact-candidate ai-checks workflow has no valid run attempt")
    jobs = fetch_workflow_jobs(repository, run_id, attempt)
    _raise_for_failed_producer(jobs, commit, workflow.get("status"))
    return jobs


def _belongs_to_workflow_attempt(run: dict[str, Any], repository: str, workflow_id: int) -> bool:
    details = run.get("details_url")
    match = re.fullmatch(
        rf"https://github\.com/{re.escape(repository)}/actions/runs/([1-9][0-9]*)/job/([1-9][0-9]*)",
        details or "",
    )
    return match is not None and int(match.group(1)) == workflow_id and int(match.group(2)) == run.get("id")


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
    workflow = latest_exact_workflow_run(repo, commit)
    if workflow is None:
        raise GatePending(f"{CHECK_NAME!r} has not reported on {commit}")
    workflow_id = workflow["id"]
    jobs = _check_latest_producers(repo, workflow, commit)
    aggregate_job_ids = {
        job["id"]
        for job in jobs
        if job.get("name") == CHECK_NAME and isinstance(job.get("id"), int)
    }
    matching = [
        run
        for run in matching
        if _belongs_to_workflow_attempt(run, repo, workflow_id) and run.get("id") in aggregate_job_ids
    ]
    if not matching:
        if workflow.get("status") == "completed" and workflow.get("conclusion") != "success":
            raise GateError(
                "exact-candidate ai-checks workflow completed unsuccessfully "
                f"(conclusion={workflow.get('conclusion')!r})"
            )
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
    fetched_workflow = fetch_workflow_run(repo, run_id)
    if (fetched_workflow.get("id") != run_id or not _is_expected_workflow_path(fetched_workflow.get("path"))
            or fetched_workflow.get("head_sha") != commit or fetched_workflow.get("event") != "push"
            or fetched_workflow.get("run_attempt") != workflow.get("run_attempt")):
        raise GateError("check run does not belong to the exact-candidate ai-checks workflow")
    workflow_status = fetched_workflow.get("status")
    workflow_pending = workflow_status in NONTERMINAL_WORKFLOW_STATUSES
    if not workflow_pending and workflow_status != "completed":
        raise GateError(f"exact-candidate ai-checks workflow has unexpected status {workflow_status!r}")
    if workflow_status == "completed" and fetched_workflow.get("conclusion") != "success":
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
    pending_producers = [
        job
        for job in jobs
        if job.get("name") in REQUIRED_PRODUCER_JOBS
        and job.get("status") in NONTERMINAL_JOB_STATUSES
    ]
    if pending_producers:
        names = ", ".join(sorted(job["name"] for job in pending_producers))
        raise GatePending(f"required analysis producers for {commit} are still running: {names}")
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
