#!/usr/bin/env python3
"""Run a project Codex task with a selected model and high reasoning effort."""

from __future__ import annotations

import argparse
from collections import defaultdict
import json
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Any, Sequence


MODELS = {
    "astra": "gpt-6-astra",
    "sol": "gpt-5.6-sol",
    "terra": "gpt-5.6-terra",
    "luna": "gpt-5.6-luna",
}
REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
USAGE_FIELDS = ("input_tokens", "cached_input_tokens", "output_tokens")


def make_command(route: str, codex_args: Sequence[str]) -> list[str]:
    return [
        "codex",
        "--model",
        MODELS[route],
        "-c",
        'model_reasoning_effort="high"',
        *codex_args,
    ]


def _metric_values(usage: Any) -> dict[str, int | None]:
    if not isinstance(usage, dict):
        return {field: None for field in USAGE_FIELDS}
    values: dict[str, int | None] = {}
    for field in USAGE_FIELDS:
        value = usage.get(field)
        values[field] = value if isinstance(value, int) and value >= 0 else None
    return values


def _subtract_metrics(
    current: dict[str, int], previous: dict[str, int] | None
) -> dict[str, int] | None:
    if previous is None:
        return current
    delta = {field: current[field] - previous[field] for field in USAGE_FIELDS}
    return delta if all(value >= 0 for value in delta.values()) else None


def _rollout_usage(path: Path) -> dict[str, Any]:
    session_model: str | None = None
    context_model: str | None = None
    session_models: set[str] = set()
    observed_models: set[str] = set()
    thread_ids: set[str] = set()
    token_records: list[dict[str, Any]] = []
    direct_records: list[dict[str, Any]] = []
    malformed_lines = 0
    try:
        stream = path.open(encoding="utf-8")
    except OSError as error:
        return {
            "source": str(path),
            "model": None,
            "usage": {field: None for field in USAGE_FIELDS},
            "availability": "unavailable",
            "token_records": 0,
            "malformed_lines": 0,
            "error": str(error),
        }
    with stream:
        for line in stream:
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                malformed_lines += 1
                continue
            if not isinstance(event, dict):
                continue
            payload = event.get("payload")
            if not isinstance(payload, dict):
                continue
            if event.get("type") == "turn_context" and isinstance(payload.get("model"), str):
                context_model = payload["model"]
                observed_models.add(context_model)
            if event.get("type") == "session_meta":
                provenance = payload.get("base_instructions", {}).get("provenance", {})
                if isinstance(provenance, dict) and isinstance(provenance.get("model"), str):
                    session_model = provenance["model"]
                    session_models.add(session_model)
                    observed_models.add(session_model)
                elif isinstance(payload.get("model"), str):
                    session_model = payload["model"]
                    session_models.add(session_model)
                    observed_models.add(session_model)
            if event.get("type") == "token_usage_record":
                thread_id = payload.get("thread_id")
                if isinstance(thread_id, str):
                    thread_ids.add(thread_id)
                cumulative = payload.get("thread_token_usage")
                if not isinstance(cumulative, dict) and isinstance(payload.get("usage"), dict):
                    cumulative = payload["usage"]
                if isinstance(cumulative, dict):
                    direct_records.append({
                        "usage": cumulative,
                        "model": context_model or (session_model if len(session_models) <= 1 else None),
                    })
            if event.get("type") == "event_msg" and payload.get("type") == "token_count":
                info = payload.get("info")
                cumulative = info.get("total_token_usage") if isinstance(info, dict) else None
                if isinstance(cumulative, dict):
                    token_records.append({
                        "usage": cumulative,
                        "model": context_model or (session_model if len(session_models) <= 1 else None),
                    })
    records = token_records or direct_records
    normalized = [_metric_values(record["usage"]) for record in records]
    usage = normalized[-1] if normalized else {field: None for field in USAGE_FIELDS}
    complete = bool(records) and not malformed_lines and all(
        all(value is not None for value in metrics.values()) for metrics in normalized
    )
    attribution: dict[str, dict[str, int]] = defaultdict(lambda: {field: 0 for field in USAGE_FIELDS})
    attribution_available = complete and bool(records) and len(thread_ids) <= 1
    previous: dict[str, int] | None = None
    for metrics, record in zip(normalized, records):
        if not attribution_available or not all(value is not None for value in metrics.values()):
            break
        current = {field: int(metrics[field]) for field in USAGE_FIELDS}
        delta = _subtract_metrics(current, previous)
        model = record["model"]
        if delta is None or not isinstance(model, str):
            attribution_available = False
            break
        for field, value in delta.items():
            attribution[model][field] += value
        previous = current
    if not records:
        attribution_available = False
    model = next(iter(observed_models), session_model) if len(observed_models) <= 1 else None
    return {
        "source": str(path),
        "model": model,
        "usage": usage,
        "availability": "available" if complete else "unavailable",
        "attribution": {
            "availability": "available" if attribution_available else "unavailable",
            "by_model": dict(attribution) if attribution_available else None,
        },
        "token_records": len(records),
        "token_source": "token_count" if token_records else "token_usage_record",
        "malformed_lines": malformed_lines,
    }


def token_usage_report(paths: Sequence[Path]) -> dict[str, Any]:
    """Read only token metadata from explicitly selected rollout JSONL files."""

    if not paths:
        raise ValueError("at least one rollout path is required")
    rollouts = [_rollout_usage(path) for path in paths]
    seen: set[tuple[str, str]] = set()
    unique: list[dict[str, Any]] = []
    excluded_duplicates = 0
    for rollout in rollouts:
        try:
            with Path(rollout["source"]).open(encoding="utf-8") as stream:
                for line in stream:
                    try:
                        event = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if event.get("type") == "token_usage_record":
                        payload = event.get("payload", {})
                        key = (str(payload.get("session_id", "")), str(payload.get("thread_id", "")))
                        break
                else:
                    key = (rollout["source"], "")
        except OSError:
            key = (rollout["source"], "")
        if key[0] and key[1] and key in seen:
            excluded_duplicates += 1
            continue
        seen.add(key)
        unique.append(rollout)
    grouped: dict[str, list[dict[str, int]]] = defaultdict(list)
    total_entries: list[dict[str, int | None]] = []
    unavailable_attribution = 0
    for rollout in unique:
        total_entries.append({
            field: int(rollout["usage"][field]) if rollout["usage"][field] is not None else None
            for field in USAGE_FIELDS
        })
        if rollout["attribution"]["availability"] != "available":
            unavailable_attribution += 1
            continue
        for model, metrics in rollout["attribution"]["by_model"].items():
            grouped[model].append(metrics)
    by_model = []
    for model, entries in sorted(grouped.items()):
        metrics = {}
        for field in USAGE_FIELDS:
            metrics[field] = sum(entry[field] for entry in entries) if entries else None
        by_model.append({
            "model": model,
            "rollouts": len(entries),
            "usage": metrics,
            "availability": "available" if all(value is not None for value in metrics.values()) else "unavailable",
        })
    if unavailable_attribution:
        by_model.append({
            "model": None,
            "rollouts": unavailable_attribution,
            "usage": {field: None for field in USAGE_FIELDS},
            "availability": "unavailable",
        })
    totals = {
        field: (
            sum(int(entry[field]) for entry in total_entries)
            if total_entries and all(entry[field] is not None for entry in total_entries)
            else None
        )
        for field in USAGE_FIELDS
    }
    return {
        "schema_version": 1,
        "scope": "explicit_rollout_paths",
        "rollouts": unique,
        "by_model": by_model,
        "totals": totals,
        "coverage": {
            "requested_rollouts": len(paths),
            "counted_rollouts": len(unique),
            "excluded_duplicate_rollouts": excluded_duplicates,
            "unavailable_model_attribution_rollouts": unavailable_attribution,
            "metrics": list(USAGE_FIELDS),
        },
    }


def write_immutable_json(target_path: Path, payload: dict[str, Any]) -> None:
    target_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with target_path.open("x", encoding="utf-8", newline="\n") as output:
            json.dump(payload, output, indent=2, ensure_ascii=False, sort_keys=True)
            output.write("\n")
    except FileExistsError as error:
        raise ValueError(f"refusing to overwrite immutable record {target_path}") from error


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true", help="print the command without launching Codex")
    parser.add_argument("route", choices=tuple(MODELS), nargs="?", default="luna", help="model route to use")
    parser.add_argument("codex_args", nargs=argparse.REMAINDER, help="arguments forwarded to Codex")
    return parser


def usage_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Report token usage from selected Codex rollout JSONL files.")
    parser.add_argument("--rollout", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if arguments and arguments[0] == "usage":
        try:
            usage_args = usage_parser().parse_args(arguments[1:])
            report = token_usage_report(usage_args.rollout)
            if usage_args.output:
                write_immutable_json(usage_args.output, report)
            print(json.dumps(report, indent=2, ensure_ascii=False, sort_keys=True))
            return 0
        except ValueError as error:
            print(f"ai_run: {error}", file=sys.stderr)
            return 1
    args = make_parser().parse_args(argv)
    command = make_command(args.route, args.codex_args)
    if args.dry_run:
        print(shlex.join(command))
        return 0
    try:
        completed = subprocess.run(command, cwd=REPOSITORY_ROOT, check=False, shell=False)
    except FileNotFoundError:
        print("ai_run: codex executable not found on PATH", file=sys.stderr)
        return 127
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
