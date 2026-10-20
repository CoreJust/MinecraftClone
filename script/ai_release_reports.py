#!/usr/bin/env python3
"""Create, validate, consolidate, and safely retire evidence-backed PDF reports."""

from __future__ import annotations

import argparse
from datetime import datetime, timedelta, timezone
import hashlib
from io import BytesIO
import json
import math
import os
from pathlib import Path
from pathlib import PurePosixPath
import re
import shutil
import sys
import tempfile
from typing import Any
import uuid


SCHEMA_VERSION = 1
SECTION_TITLES = (
    "1. Period",
    "2. State of release",
    "3. Expected progress",
    "4. Feature progress",
    "5. Other progress",
    "6. Time spent",
    "7. Issues",
    "8. Conclusions: investigated causes and corrections",
    "9. Next goals",
    "10. Summary",
)
_SAFE_COMPONENT = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$")
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_DRAFT_KEYS = {
    "period",
    "feature_state",
    "expected_progress",
    "feature_progress",
    "other_progress",
    "time_spent",
    "issues",
    "conclusions",
    "next_goals",
    "summary",
}
_OPTIONAL_DRAFT_KEYS = {"overview"}
_MAJOR_DRAFT_KEYS = {"product_results", "screenshots", "measurements", "analysis"}
_TIME_KEYS = {"allocations", "parallel_note"}
_STATUS_ORDER = {"Done": 0, "In progress": 1, "Planned": 2}
_STATUS_COLORS = {"Done": "#188038", "In progress": "#1a73e8", "Planned": "#5f6368"}


class ReportError(ValueError):
    """Raised when report input or managed release state violates its contract."""


def _canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    try:
        temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ReportError(f"cannot read valid JSON from {path}: {error}") from error


def _safe_component(value: str, label: str) -> str:
    if not isinstance(value, str) or not _SAFE_COMPONENT.fullmatch(value) or value in {".", ".."}:
        raise ReportError(f"{label} must be a safe filename component")
    return value


def _reports_root(reports_root: str | Path) -> Path:
    root = Path(reports_root).absolute()
    if root.is_symlink():
        raise ReportError("reports root must not be a symlink")
    return root


def _release_directory(reports_root: str | Path, release_id: str) -> Path:
    release_id = _safe_component(release_id, "release ID")
    root = _reports_root(reports_root)
    release_directory = root / release_id
    if release_directory.is_symlink():
        raise ReportError("managed release directory must not be a symlink")
    return release_directory


def _validate_feature(feature: Any) -> dict[str, str]:
    if not isinstance(feature, dict) or set(feature) != {"id", "name"}:
        raise ReportError("each feature must contain exactly id and name")
    feature_id = _safe_component(feature["id"], "feature ID")
    name = _single_line(feature["name"], "feature name")
    if not name:
        raise ReportError("feature name must not be empty")
    return {"id": feature_id, "name": name}


def _single_line(value: Any, label: str) -> str:
    if not isinstance(value, str) or "\n" in value or "\r" in value:
        raise ReportError(f"{label} must be a single line of text")
    return value.strip()


def _line_list(value: Any, label: str, limit: int) -> list[str]:
    if not isinstance(value, list) or len(value) > limit:
        raise ReportError(f"{label} must contain at most {limit} lines")
    lines = [_single_line(line, f"{label} line") for line in value]
    if any(not line for line in lines):
        raise ReportError(f"{label} lines must not be empty")
    return lines


def _parse_datetime(value: Any, label: str) -> datetime:
    if not isinstance(value, str):
        raise ReportError(f"{label} must be an ISO-8601 datetime with a timezone")
    try:
        parsed = datetime.fromisoformat(value)
    except ValueError as error:
        raise ReportError(f"{label} must be an ISO-8601 datetime with a timezone") from error
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise ReportError(f"{label} must include a timezone")
    return parsed


def _features_by_id(features: list[dict[str, str]]) -> dict[str, dict[str, str]]:
    if not isinstance(features, list):
        raise ReportError("feature manifest must be an ordered list")
    normalized = [_validate_feature(feature) for feature in features]
    identifiers = [feature["id"] for feature in normalized]
    if not normalized or len(set(identifiers)) != len(identifiers):
        raise ReportError("feature manifest must be nonempty with unique IDs")
    return {feature["id"]: feature for feature in normalized}


def _validate_feature_lines(entries: Any, known_ids: set[str]) -> dict[str, list[str]]:
    if not isinstance(entries, list):
        raise ReportError("feature progress must be a list")
    result: dict[str, list[str]] = {}
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != {"feature_id", "lines"}:
            raise ReportError("each feature progress item must contain feature_id and lines")
        feature_id = _single_line(entry["feature_id"], "feature progress ID")
        if feature_id not in known_ids:
            raise ReportError(f"feature progress references unknown feature {feature_id}")
        if feature_id in result:
            raise ReportError(f"feature progress repeats {feature_id}")
        lines = _line_list(entry["lines"], f"feature progress for {feature_id}", 2)
        if not 1 <= len(lines) <= 2:
            raise ReportError(f"feature progress for {feature_id} must have 1–2 lines")
        result[feature_id] = lines
    return result


def validate_draft(
    draft: dict[str, Any],
    features: list[dict[str, str]],
    expected_kind: str,
    *,
    minor_kind: str | None = None,
    minors_per_major: int = 3,
) -> None:
    """Validate the ten-section report content and period-specific constraints."""
    if minor_kind is None:
        minor_kind = "3h" if expected_kind == "3h" else "4h"
    if minor_kind not in {"3h", "4h"} or expected_kind not in {minor_kind, "12h"}:
        raise ReportError("period kind must match the 3h legacy or 4h minor report profile")
    if isinstance(minors_per_major, bool) or not isinstance(minors_per_major, int) or minors_per_major < 1:
        raise ReportError("a major report must cover at least one minor period")
    if not isinstance(draft, dict):
        raise ReportError("draft must be a JSON object")
    required_keys = _DRAFT_KEYS | ({"covered_periods"} if expected_kind == "12h" else set())
    if expected_kind == "12h" and minor_kind == "4h":
        missing_major_fields = _MAJOR_DRAFT_KEYS - set(draft)
        if missing_major_fields:
            raise ReportError(f"major report fields are missing: {', '.join(sorted(missing_major_fields))}")
        required_keys |= _MAJOR_DRAFT_KEYS
    allowed_keys = required_keys | _OPTIONAL_DRAFT_KEYS
    if not required_keys.issubset(draft) or not set(draft).issubset(allowed_keys):
        raise ReportError("draft fields do not match the required report format")
    if "overview" in draft:
        _line_list(draft["overview"], "overview", 6)
    manifest = _features_by_id(features)
    period = draft["period"]
    if not isinstance(period, dict) or set(period) != {"kind", "start", "end"}:
        raise ReportError("period must contain kind, start, and end")
    if period["kind"] != expected_kind:
        raise ReportError(f"period kind must be {expected_kind}")
    start = _parse_datetime(period["start"], "period start")
    end = _parse_datetime(period["end"], "period end")
    hours = (end - start).total_seconds() / 3600
    if minor_kind == "3h":
        maximum_hours = 3 if expected_kind == "3h" else 12
        if not math.isclose(hours, maximum_hours, rel_tol=0, abs_tol=1e-9):
            raise ReportError(f"period must be exactly {maximum_hours} hours")
    else:
        maximum_hours = 4 if expected_kind == "4h" else 12
        if hours <= 0:
            raise ReportError("period must have a positive duration")
        if hours > maximum_hours:
            raise ReportError(f"period must be at most {maximum_hours} hours")

    states = draft["feature_state"]
    if not isinstance(states, list) or len(states) != len(features):
        raise ReportError("feature state must include every manifest feature in feature order")
    state_ids: list[str] = []
    for state in states:
        if not isinstance(state, dict) or set(state) != {"id", "status", "detail"}:
            raise ReportError("each feature state must contain id, status, and detail")
        feature_id = _single_line(state["id"], "feature state ID")
        status = _single_line(state["status"], "feature status")
        detail = _single_line(state["detail"], f"feature detail for {feature_id}")
        if status not in {"Done", "In progress", "Planned"}:
            raise ReportError(f"invalid status for feature {feature_id}; use Done, In progress, or Planned")
        if status == "In progress" and not detail:
            raise ReportError(f"In progress status for {feature_id} needs a short detail")
        if status != "In progress" and detail:
            raise ReportError(f"{status} status for {feature_id} must not include detail")
        state_ids.append(feature_id)
    if state_ids != [feature["id"] for feature in features]:
        raise ReportError("feature state must follow feature order")

    _line_list(draft["expected_progress"], "expected progress", 16)
    _validate_feature_lines(draft["feature_progress"], set(manifest))
    _line_list(draft["other_progress"], "other progress", 10)
    spent = draft["time_spent"]
    if not isinstance(spent, dict) or "allocations" not in spent or not set(spent).issubset(_TIME_KEYS):
        raise ReportError("time spent must contain categorized allocations")
    allocations = spent["allocations"]
    if not isinstance(allocations, list) or not 1 <= len(allocations) <= 8:
        raise ReportError("time spent must contain at most 8 allocations")
    labels: set[str] = set()
    total_hours = 0.0
    for allocation in allocations:
        if not isinstance(allocation, dict) or set(allocation) != {"label", "hours", "basis", "evidence"}:
            raise ReportError("each time allocation must contain label, hours, basis, and evidence")
        label = _single_line(allocation["label"], "time allocation label")
        if not label or label in labels:
            raise ReportError("time allocation labels must be nonempty and unique")
        labels.add(label)
        amount = allocation["hours"]
        if isinstance(amount, bool) or not isinstance(amount, (int, float)) or not math.isfinite(amount) or amount < 0:
            raise ReportError(f"time allocation hours for {label} must be finite and nonnegative")
        basis = _single_line(allocation["basis"], f"time allocation basis for {label}")
        if basis not in {"measured", "estimated", "unaccounted"}:
            raise ReportError(f"time allocation basis for {label} must be measured, estimated, or unaccounted")
        evidence = _single_line(allocation["evidence"], f"time allocation evidence for {label}")
        if not evidence:
            raise ReportError(f"time allocation evidence for {label} must not be empty")
        total_hours += float(amount)
    if not math.isclose(total_hours, hours, rel_tol=0, abs_tol=1e-6):
        raise ReportError("time allocations must sum to the period")
    if "parallel_note" in spent and not _single_line(spent["parallel_note"], "parallel work note"):
        raise ReportError("parallel work note must not be empty")
    _line_list(draft["issues"], "issues", 30)
    _line_list(draft["conclusions"], "conclusions", 30)
    _line_list(draft["next_goals"], "next goals", len(features) + 16)
    summary = _line_list(draft["summary"], "summary", 10)
    if not summary:
        raise ReportError("summary must contain at least one line")

    if expected_kind == "12h":
        covered = draft["covered_periods"]
        if not isinstance(covered, list) or len(covered) != minors_per_major:
            raise ReportError(f"12-hour summary must list exactly {minors_per_major} covered periods")
        parsed: list[tuple[datetime, datetime]] = []
        for item in covered:
            if not isinstance(item, dict) or set(item) != {"start", "end"}:
                raise ReportError("covered periods must contain start and end")
            item_start = _parse_datetime(item["start"], "covered period start")
            item_end = _parse_datetime(item["end"], "covered period end")
            item_hours = (item_end - item_start).total_seconds() / 3600
            if minor_kind == "3h":
                if not math.isclose(item_hours, 3, rel_tol=0, abs_tol=1e-6):
                    raise ReportError("legacy covered periods must each be exactly three hours")
            elif item_hours <= 0 or item_hours > 4:
                raise ReportError("covered minor periods must have positive duration and be at most 4 hours")
            parsed.append((item_start, item_end))
        if any(parsed[index][1] != parsed[index + 1][0] for index in range(len(parsed) - 1)):
            raise ReportError("covered periods must be contiguous")
        if parsed[0][0] != start or parsed[-1][1] != end:
            raise ReportError("covered periods do not match the major summary period")
        if minor_kind == "4h":
            _validate_major_details(draft)


def _validate_major_details(draft: dict[str, Any]) -> None:
    product_results = _line_list(draft["product_results"], "product results", 30)
    if not product_results:
        raise ReportError("major report fields require at least one product result")

    screenshots = draft["screenshots"]
    if not isinstance(screenshots, list) or len(screenshots) > 8:
        raise ReportError("major report screenshots must contain at most 8 images")
    screenshot_paths: set[str] = set()
    for screenshot in screenshots:
        if not isinstance(screenshot, dict) or set(screenshot) != {"path", "caption"}:
            raise ReportError("each major screenshot must contain path and caption")
        screenshot_path = _single_line(screenshot["path"], "screenshot path")
        path_parts = PurePosixPath(screenshot_path).parts
        if (
            not screenshot_path
            or "\\" in screenshot_path
            or PurePosixPath(screenshot_path).is_absolute()
            or any(part in {"", ".", ".."} for part in path_parts)
        ):
            raise ReportError("screenshot paths must be project-relative and must not traverse directories")
        if screenshot_path in screenshot_paths:
            raise ReportError("major report screenshot paths must be unique")
        screenshot_paths.add(screenshot_path)
        if not _single_line(screenshot["caption"], "screenshot caption"):
            raise ReportError("screenshot caption must not be empty")

    measurements = draft["measurements"]
    if not isinstance(measurements, list) or not 1 <= len(measurements) <= 30:
        raise ReportError("major report must contain 1 to 30 measurement rows")
    labels: set[str] = set()
    for measurement in measurements:
        if not isinstance(measurement, dict) or set(measurement) != {"metric", "value", "workload", "evidence"}:
            raise ReportError("each measurement must contain metric, value, workload, and evidence")
        metric = _single_line(measurement["metric"], "measurement metric")
        if not metric or metric in labels:
            raise ReportError("measurement metrics must be nonempty and unique")
        labels.add(metric)
        for key in ("value", "workload", "evidence"):
            if not _single_line(measurement[key], f"measurement {key}"):
                raise ReportError(f"measurement {key} must not be empty")

    analysis = _line_list(draft["analysis"], "major analysis", 30)
    if not analysis:
        raise ReportError("major report fields require at least one analysis line")


def _read_screenshots(draft: dict[str, Any]) -> tuple[list[bytes], list[str]]:
    images: list[bytes] = []
    hashes: list[str] = []
    root = Path.cwd()
    for screenshot in draft.get("screenshots", []):
        source = root
        for component in PurePosixPath(screenshot["path"]).parts:
            source = source / component
            if source.is_symlink():
                raise ReportError(f"screenshot path contains a symlink: {screenshot['path']}")
        if not source.is_file():
            raise ReportError(f"screenshot file does not exist: {screenshot['path']}")
        image_data = source.read_bytes()
        try:
            from PIL import Image

            with Image.open(BytesIO(image_data)) as image:
                if image.format not in {"PNG", "JPEG"}:
                    raise ReportError("major report screenshots must be PNG or JPEG images")
                if image.width * image.height > 50_000_000:
                    raise ReportError("major report screenshot exceeds 50 megapixels")
                image.verify()
        except ImportError as error:
            raise ReportError("validating screenshots requires Pillow from the bundled Python") from error
        except ReportError:
            raise
        except Exception as error:
            raise ReportError(f"cannot read screenshot image {screenshot['path']}: {error}") from error
        images.append(image_data)
        hashes.append(hashlib.sha256(image_data).hexdigest())
    return images, hashes


def _manifest_revision(state: dict[str, Any], period_index: int) -> dict[str, Any]:
    applicable = [
        revision
        for revision in state["manifest_revisions"]
        if revision["effective_period_index"] <= period_index
    ]
    if not applicable:
        raise ReportError("no feature manifest applies to this period")
    return applicable[-1]


def _revision_hash(revision: dict[str, Any]) -> str:
    return hashlib.sha256(_canonical_json(revision).encode("utf-8")).hexdigest()


def _reporting_profile(state: dict[str, Any]) -> dict[str, Any]:
    """Resolve the new anchored cadence or the historical unmarked 3h cadence."""
    reporting = state.get("reporting")
    if reporting is None:
        return {"minor_kind": "3h", "minor_hours": 3, "minors_per_major": 4, "anchor": None}
    if not isinstance(reporting, dict) or set(reporting) != {"anchor", "minor_hours", "minors_per_major"}:
        raise ReportError("reporting cadence state is malformed")
    anchor = _parse_datetime(reporting["anchor"], "implementation anchor")
    if (
        isinstance(reporting["minor_hours"], bool)
        or not isinstance(reporting["minor_hours"], int)
        or reporting["minor_hours"] != 4
        or isinstance(reporting["minors_per_major"], bool)
        or not isinstance(reporting["minors_per_major"], int)
        or reporting["minors_per_major"] != 3
    ):
        raise ReportError("unsupported reporting cadence")
    return {"minor_kind": "4h", "minor_hours": 4, "minors_per_major": 3, "anchor": anchor}


def _state_paths(reports_root: str | Path, release_id: str) -> tuple[Path, Path, Path]:
    release_directory = _release_directory(reports_root, release_id)
    owner_path = release_directory / ".report-owner.json"
    state_path = release_directory / ".report-state.json"
    if not release_directory.is_dir() or owner_path.is_symlink() or state_path.is_symlink():
        raise ReportError("release is not an intact managed report folder")
    owner = _read_json(owner_path)
    state = _read_json(state_path)
    if not isinstance(owner, dict) or owner.get("release_id") != release_id:
        raise ReportError("release owner marker does not match")
    if not isinstance(state, dict) or state.get("release_id") != release_id or state.get("owner_token") != owner.get("token"):
        raise ReportError("release state does not match its owner marker")
    reporting = state.get("reporting")
    if owner.get("anchor") is not None:
        if not isinstance(reporting, dict) or reporting.get("anchor") != owner["anchor"]:
            raise ReportError("implementation anchor does not match its owner marker")
    elif reporting is not None:
        raise ReportError("anchored report state is missing its owner marker")
    return release_directory, owner_path, state_path


def start_release(
    reports_root: str | Path,
    release_id: str,
    prefix: str,
    features: list[dict[str, str]],
    required_gates: list[str],
    *,
    anchor: str | None = None,
) -> Path:
    """Create a fresh anchored 4-hour minor and 12-hour major report folder."""
    release_id = _safe_component(release_id, "release ID")
    prefix = _safe_component(prefix, "filename prefix")
    if anchor is None:
        raise ReportError("new release report state requires an implementation anchor")
    anchor_value = _parse_datetime(anchor, "implementation anchor").isoformat()
    normalized_features = [_validate_feature(feature) for feature in features]
    _features_by_id(normalized_features)
    gates = [_safe_component(gate, "gate ID") for gate in required_gates]
    if not gates or len(set(gates)) != len(gates):
        raise ReportError("required delivery gates must be nonempty and unique")
    root = _reports_root(reports_root)
    root.mkdir(parents=True, exist_ok=True)
    release_directory = root / release_id
    if release_directory.exists() or release_directory.is_symlink():
        raise ReportError(f"managed release folder already exists: {release_directory}")
    release_directory.mkdir()
    (release_directory / "4h").mkdir()
    (release_directory / "12h").mkdir()
    token = uuid.uuid4().hex
    revision = {
        "revision": 1,
        "effective_period_index": 0,
        "features": normalized_features,
        "addition_authority": None,
    }
    state = {
        "schema_version": SCHEMA_VERSION,
        "release_id": release_id,
        "prefix": prefix,
        "required_gates": gates,
        "owner_token": token,
        "manifest_revisions": [revision],
        "reporting": {
            "anchor": anchor_value,
            "minor_hours": 4,
            "minors_per_major": 3,
        },
        "periods": [],
        "aggregates": [],
    }
    try:
        _atomic_json(release_directory / ".report-owner.json", {
            "release_id": release_id,
            "token": token,
            "anchor": anchor_value,
        })
        _atomic_json(release_directory / ".report-state.json", state)
    except Exception:
        shutil.rmtree(release_directory)
        raise
    return release_directory


def load_release_state(reports_root: str | Path, release_id: str) -> dict[str, Any]:
    _, _, state_path = _state_paths(reports_root, release_id)
    return _read_json(state_path)


def add_feature(
    reports_root: str | Path,
    release_id: str,
    feature: dict[str, str],
    authority: dict[str, str] | None = None,
) -> dict[str, Any]:
    """Append an explicitly requested feature revision for the next period."""
    release_directory, _, state_path = _state_paths(reports_root, release_id)
    state = _read_json(state_path)
    normalized = _validate_feature(feature)
    if not isinstance(authority, dict) or set(authority) != {"reference", "text_sha256"}:
        raise ReportError("feature addition requires a user-request reference and text SHA-256")
    reference = _single_line(authority["reference"], "feature addition authority reference")
    authority_hash = authority["text_sha256"]
    if not reference or not isinstance(authority_hash, str) or not _SHA256.fullmatch(authority_hash):
        raise ReportError("feature addition authority must include a reference and lowercase SHA-256")
    addition_authority = {"reference": reference, "text_sha256": authority_hash}
    previous = state["manifest_revisions"][-1]
    if normalized["id"] in {item["id"] for item in previous["features"]}:
        raise ReportError(f"feature {normalized['id']} already exists")
    revision = {
        "revision": previous["revision"] + 1,
        "effective_period_index": len(state["periods"]),
        "features": [*previous["features"], normalized],
        "addition_authority": addition_authority,
    }
    state["manifest_revisions"].append(revision)
    _atomic_json(state_path, state)
    return state


def _filename(prefix: str, start: datetime, *, precise: bool = False) -> str:
    timestamp = start.strftime("%Y-%m-%d-%H%M%S%f%z" if precise else "%Y-%m-%d-%H")
    return f"{prefix}-{timestamp}-report.pdf"


def _payload(
    state: dict[str, Any], draft: dict[str, Any], revision: dict[str, Any], report_kind: str
) -> dict[str, Any]:
    return {
        "schema_version": SCHEMA_VERSION,
        "release_id": state["release_id"],
        "prefix": state["prefix"],
        "report_kind": report_kind,
        "manifest_revision": revision["revision"],
        "manifest_sha256": _revision_hash(revision),
        "features": revision["features"],
        "draft": draft,
    }


def _pdf_payload(payload: dict[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in payload.items() if key != "_screenshot_data"}


def _atomic_render(payload: dict[str, Any], destination: Path) -> str:
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(f".{destination.name}.{uuid.uuid4().hex}.tmp.pdf")
    try:
        render_pdf(payload, temporary)
        verify_pdf(temporary, payload)
        os.replace(temporary, destination)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise
    return hashlib.sha256(destination.read_bytes()).hexdigest()


def _validate_append_inputs(
    state: dict[str, Any],
    draft: dict[str, Any],
    twelve_hour_summary: dict[str, Any] | None,
    *,
    check_screenshots: bool = True,
) -> tuple[dict[str, Any], datetime, datetime, bool]:
    if "pending_pruning" in state:
        raise ReportError("a previous aggregate needs pruning reconciliation before another report")
    profile = _reporting_profile(state)
    periods = state["periods"]
    period_index = len(periods)
    revision = _manifest_revision(state, period_index)
    validate_draft(
        draft,
        revision["features"],
        profile["minor_kind"],
        minor_kind=profile["minor_kind"],
        minors_per_major=profile["minors_per_major"],
    )
    period_start = _parse_datetime(draft["period"]["start"], "period start")
    period_end = _parse_datetime(draft["period"]["end"], "period end")
    if period_index == 0 and profile["anchor"] is not None and period_start != profile["anchor"]:
        raise ReportError("first minor report must begin at the implementation anchor")
    if periods and period_start != _parse_datetime(periods[-1]["end"], "previous period end"):
        raise ReportError("report periods must be contiguous")
    aggregate_required = period_index % profile["minors_per_major"] == profile["minors_per_major"] - 1
    aggregate_ordinal = "third" if profile["minors_per_major"] == 3 else "fourth"
    if aggregate_required and twelve_hour_summary is None:
        raise ReportError(f"the {aggregate_ordinal} contiguous report requires a 12-hour summary")
    if not aggregate_required and twelve_hour_summary is not None:
        raise ReportError(f"12-hour summary is only accepted with every {aggregate_ordinal} report")
    if aggregate_required:
        assert twelve_hour_summary is not None
        validate_draft(
            twelve_hour_summary,
            revision["features"],
            "12h",
            minor_kind=profile["minor_kind"],
            minors_per_major=profile["minors_per_major"],
        )
        first_index = period_index - profile["minors_per_major"] + 1
        covered_records = [
            *periods[first_index:period_index],
            {"start": draft["period"]["start"], "end": draft["period"]["end"]},
        ]
        expected = [
            (_parse_datetime(item["start"], "expected covered start"), _parse_datetime(item["end"], "expected covered end"))
            for item in covered_records
        ]
        supplied = [
            (_parse_datetime(item["start"], "covered start"), _parse_datetime(item["end"], "covered end"))
            for item in twelve_hour_summary["covered_periods"]
        ]
        if supplied != expected:
            raise ReportError(f"12-hour summary covered periods do not match the {profile['minors_per_major']} reports")
        if (
            _parse_datetime(twelve_hour_summary["period"]["start"], "summary start") != expected[0][0]
            or _parse_datetime(twelve_hour_summary["period"]["end"], "summary end") != period_end
        ):
            raise ReportError(f"12-hour summary must cover the exact {profile['minors_per_major']}-report interval")
        initial_revision = _manifest_revision(state, first_index)
        initial_ids = {feature["id"] for feature in initial_revision["features"]}
        summary_progress = _validate_feature_lines(
            twelve_hour_summary["feature_progress"],
            {item["id"] for item in revision["features"]},
        )
        for feature in revision["features"]:
            if feature["id"] not in initial_ids and feature["id"] not in summary_progress:
                raise ReportError(f"12-hour summary must note addition of feature {feature['id']}")
        if check_screenshots and profile["minor_kind"] == "4h":
            _read_screenshots(twelve_hour_summary)
    return revision, period_start, period_end, aggregate_required


def append_report(
    reports_root: str | Path,
    release_id: str,
    draft: dict[str, Any],
    twelve_hour_summary: dict[str, Any] | None = None,
) -> list[Path]:
    """Append one contiguous minor report and consolidate each complete major."""
    release_directory, _, state_path = _state_paths(reports_root, release_id)
    state = _read_json(state_path)
    if "pending_pruning" in state:
        raise ReportError("a previous aggregate needs pruning reconciliation before another report")
    periods = state["periods"]
    period_index = len(periods)
    current_revision, period_start, period_end, expected_aggregate = _validate_append_inputs(
        state, draft, twelve_hour_summary
    )
    features = current_revision["features"]
    profile = _reporting_profile(state)
    precise_names = profile["minor_kind"] == "4h"

    standalone_path: Path | None = None
    if not expected_aggregate:
        standalone_path = release_directory / profile["minor_kind"] / _filename(state["prefix"], period_start, precise=precise_names)
        if standalone_path.exists():
            raise ReportError("report filename already exists")
        payload = _payload(state, draft, current_revision, profile["minor_kind"])
        digest = _atomic_render(payload, standalone_path)
        period_record = {
            "start": draft["period"]["start"],
            "end": draft["period"]["end"],
            "draft": draft,
            "manifest_revision": current_revision["revision"],
            "manifest_sha256": _revision_hash(current_revision),
            "pdf_path": str(standalone_path.relative_to(release_directory)),
            "pdf_sha256": digest,
        }
        periods.append(period_record)
        try:
            _atomic_json(state_path, state)
        except Exception:
            standalone_path.unlink(missing_ok=True)
            raise
        return [standalone_path]

    assert twelve_hour_summary is not None
    first_index = period_index - profile["minors_per_major"] + 1
    covered_records = periods[first_index:period_index]
    summary_start = _parse_datetime(twelve_hour_summary["period"]["start"], "summary start")

    aggregate_path = release_directory / "12h" / _filename(state["prefix"], summary_start, precise=precise_names)
    if aggregate_path.exists():
        raise ReportError("12-hour report filename already exists")
    standalone_paths = [record.get("pdf_path") for record in covered_records]
    pending_sources = [
        {"path": relative, "sha256": record["pdf_sha256"]}
        for record, relative in zip(covered_records, standalone_paths)
        if relative is not None
    ]
    for record, relative in zip(covered_records, standalone_paths):
        candidate = release_directory / relative if relative else release_directory
        if candidate.parent != release_directory / profile["minor_kind"] or candidate.is_symlink() or not candidate.is_file():
            raise ReportError("a covered standalone report is missing or unsafe")
        if hashlib.sha256(candidate.read_bytes()).hexdigest() != record.get("pdf_sha256"):
            raise ReportError(f"standalone report changed after verification: {relative}")
    payload = _payload(state, twelve_hour_summary, current_revision, "12h")
    if precise_names:
        screenshot_data, screenshot_hashes = _read_screenshots(twelve_hour_summary)
        payload["screenshot_sha256"] = screenshot_hashes
        payload["_screenshot_data"] = screenshot_data
    digest = _atomic_render(payload, aggregate_path)
    aggregate = {
        "start": twelve_hour_summary["period"]["start"],
        "end": twelve_hour_summary["period"]["end"],
        "covered_period_indices": list(range(first_index, period_index + 1)),
        "draft": twelve_hour_summary,
        "manifest_revision": current_revision["revision"],
        "manifest_sha256": _revision_hash(current_revision),
        "features": features,
        "pdf_path": str(aggregate_path.relative_to(release_directory)),
        "pdf_sha256": digest,
    }
    for record in covered_records:
        record.pop("pdf_path", None)
        record.pop("pdf_sha256", None)
    periods.append({
        "start": draft["period"]["start"],
        "end": draft["period"]["end"],
        "draft": draft,
        "manifest_revision": current_revision["revision"],
        "manifest_sha256": _revision_hash(current_revision),
    })
    state["aggregates"].append(aggregate)
    state["pending_pruning"] = {"sources": pending_sources}
    try:
        _atomic_json(state_path, state)
    except Exception:
        aggregate_path.unlink(missing_ok=True)
        raise
    reconcile_pruning(reports_root, release_id)
    return [aggregate_path]


def _feature_status_text(feature_state: dict[str, Any]) -> str:
    status = feature_state["status"]
    if status == "Done":
        return "Done"
    if status == "In progress":
        return f"in progress: {feature_state['detail']}"
    if status == "Planned":
        return "planned"
    raise ReportError(f"unsupported feature status: {status}")


def _feature_status_line(feature_name: str, feature_state: dict[str, Any]) -> str:
    identity = f" [{feature_state['id']}]" if "id" in feature_state else ""
    return f"{feature_name}{identity} - {_feature_status_text(feature_state)}"


def _time_share_labels(allocations: list[dict[str, Any]], period_hours: float) -> list[str]:
    return [
        f"{allocation['label']} - {allocation['hours']:g} h "
        f"({allocation['hours'] / period_hours * 100:.1f}%, {allocation['basis']})"
        for allocation in allocations
    ]


def _period_duration_hours(period: dict[str, Any]) -> float:
    start = _parse_datetime(period["start"], "period start")
    end = _parse_datetime(period["end"], "period end")
    return (end - start).total_seconds() / 3600


def _report_story(payload: dict[str, Any]):
    from reportlab.lib import colors
    from reportlab.lib.enums import TA_LEFT
    from reportlab.lib.pagesizes import A4
    from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
    from reportlab.lib.units import mm
    from reportlab.platypus import Flowable, Image, Paragraph, Spacer, Table, TableStyle
    from reportlab.lib.utils import ImageReader

    class TimeShareChart(Flowable):
        def __init__(self, allocations: list[dict[str, Any]], period_hours: float):
            super().__init__()
            self.allocations = allocations
            self.period_hours = period_hours
            self.labels = _time_share_labels(allocations, period_hours)
            self.legend_rows = []

        def wrap(self, available_width, available_height):
            self.width = available_width
            self.legend_rows = [paragraph_text(label) for label in self.labels]
            self.legend_heights = [row.wrap(available_width - 13, available_height)[1] for row in self.legend_rows]
            self.height = 29 + sum(height + 2 for height in self.legend_heights)
            return available_width, self.height

        def draw(self):
            canvas = self.canv
            palette = ("#3977b8", "#e69f24", "#4f9b72", "#a765a5", "#d46f46", "#4d9da5", "#7584bd", "#9299a3")
            width = self.width
            x = 0
            bar_y = self.height - 12
            for index, allocation in enumerate(self.allocations):
                segment = width * float(allocation["hours"]) / self.period_hours
                if segment > 0:
                    canvas.setFillColor(colors.HexColor(palette[index]))
                    canvas.rect(x, bar_y, segment, 8, stroke=0, fill=1)
                    if segment > 14:
                        canvas.setFillColor(colors.white)
                        canvas.setFont("Helvetica-Bold", 7)
                        canvas.drawCentredString(x + segment / 2, bar_y + 1, str(index + 1))
                    x += segment
            top = bar_y - 5
            for index, (row, row_height) in enumerate(zip(self.legend_rows, self.legend_heights)):
                row_bottom = top - row_height
                canvas.setFillColor(colors.HexColor(palette[index]))
                canvas.rect(1, row_bottom + max(0, (row_height - 7) / 2), 7, 7, stroke=0, fill=1)
                row.drawOn(canvas, 13, row_bottom)
                top = row_bottom - 2

    report = payload["draft"]
    features = payload["features"]
    styles = getSampleStyleSheet()
    styles.add(ParagraphStyle(name="ReportSection", parent=styles["Heading2"], fontSize=11, leading=14, spaceBefore=7, spaceAfter=4))
    styles.add(ParagraphStyle(name="ReportBody", parent=styles["BodyText"], fontSize=9, leading=12, alignment=TA_LEFT, spaceAfter=2))
    story: list[Any] = [Paragraph(f"{payload['prefix']} release report", styles["Title"])]

    def paragraph_text(text: str):
        escaped = text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        return Paragraph(escaped, styles["ReportBody"])

    def status_paragraph(feature_name: str, state: dict[str, Any]):
        identity = f" [{state['id']}]" if "id" in state else ""
        status = _feature_status_text(state)
        color = _STATUS_COLORS[state["status"]]
        escaped_name = feature_name.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        escaped_status = status.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        return Paragraph(
            f"{escaped_name}{identity} - <font color=\"{color}\"><b>{escaped_status}</b></font>",
            styles["ReportBody"],
        )

    def add_section(title: str, lines: list[str]):
        story.append(Paragraph(title, styles["ReportSection"]))
        if not lines:
            story.append(paragraph_text("None recorded."))
        for line in lines:
            story.append(paragraph_text(line))

    period = report["period"]
    period_lines = [f"{period['kind']}: {period['start']} to {period['end']}"]
    if period["kind"] == "12h":
        minor_label = "4-hour minor interval" if "product_results" in report else "3-hour interval"
        period_lines.extend(f"Covered {minor_label}: {item['start']} to {item['end']}" for item in report["covered_periods"])
    add_section(SECTION_TITLES[0], period_lines)
    feature_names = {feature["id"]: feature["name"] for feature in features}
    display_states = sorted(
        enumerate(report["feature_state"]),
        key=lambda item: (_STATUS_ORDER[item[1]["status"]], item[0]),
    )
    story.append(Paragraph(SECTION_TITLES[1], styles["ReportSection"]))
    for line in report.get("overview", []):
        story.append(paragraph_text(line))
    if not display_states:
        story.append(paragraph_text("None recorded."))
    for _, state in display_states:
        story.append(status_paragraph(feature_names[state["id"]], state))
    add_section(SECTION_TITLES[2], report["expected_progress"])
    progress_lines = []
    for item in report["feature_progress"]:
        feature_name = next(feature["name"] for feature in features if feature["id"] == item["feature_id"])
        progress_lines.extend(f"{feature_name}: {line}" for line in item["lines"])
    add_section(SECTION_TITLES[3], progress_lines)
    add_section(SECTION_TITLES[4], report["other_progress"])
    story.append(Paragraph(SECTION_TITLES[5], styles["ReportSection"]))
    spent = report["time_spent"]
    hours = _period_duration_hours(period)
    story.append(Spacer(1, 3))
    allocations = spent["allocations"]
    story.append(paragraph_text("Time-share chart: proportional activity hours; labels show hours, share, and basis."))
    story.append(TimeShareChart(allocations, hours))
    palette = ("#3977b8", "#e69f24", "#4f9b72", "#a765a5", "#d46f46", "#4d9da5", "#7584bd", "#9299a3")
    allocation_rows = [["#", "Activity", "Hours", "Basis", "Evidence"]]
    for index, allocation in enumerate(allocations):
        allocation_rows.append([
            str(index + 1),
            paragraph_text(allocation["label"]),
            f"{allocation['hours']:g}",
            allocation["basis"],
            paragraph_text(allocation["evidence"]),
        ])
    allocation_table = Table(allocation_rows, colWidths=[8 * mm, 42 * mm, 15 * mm, 24 * mm, None], repeatRows=1)
    allocation_commands = [
        ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#e8edf3")),
        ("GRID", (0, 0), (-1, -1), 0.3, colors.HexColor("#b8c0c9")),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("FONTSIZE", (0, 0), (-1, -1), 8),
        ("ALIGN", (0, 0), (0, -1), "CENTER"),
    ]
    for row, color in enumerate(palette[: len(allocations)], start=1):
        allocation_commands.append(("BACKGROUND", (0, row), (0, row), colors.HexColor(color)))
        allocation_commands.append(("TEXTCOLOR", (0, row), (0, row), colors.white))
    allocation_table.setStyle(TableStyle(allocation_commands))
    story.append(allocation_table)
    if "parallel_note" in spent:
        story.append(paragraph_text(f"Parallel-work accounting: {spent['parallel_note']}"))
    add_section(SECTION_TITLES[6], report["issues"])
    add_section(SECTION_TITLES[7], report["conclusions"])
    add_section(SECTION_TITLES[8], report["next_goals"])
    story.append(Paragraph(SECTION_TITLES[9], styles["ReportSection"]))
    if "product_results" in report:
        story.append(Paragraph("Product-level results", styles["Heading3"]))
        for line in report["product_results"]:
            story.append(paragraph_text(line))

        story.append(Paragraph("Screenshots", styles["Heading3"]))
        screenshot_data = payload.get("_screenshot_data", [])
        if not report["screenshots"]:
            story.append(paragraph_text("None available."))
        for screenshot, image_data in zip(report["screenshots"], screenshot_data):
            image_width, image_height = ImageReader(BytesIO(image_data)).getSize()
            scale = min((A4[0] - 36 * mm) / image_width, 120 * mm / image_height, 1)
            story.append(Image(BytesIO(image_data), width=image_width * scale, height=image_height * scale))
            story.append(paragraph_text(screenshot["caption"]))

        story.append(Paragraph("Measurements", styles["Heading3"]))
        measurement_rows = [["Metric", "Value", "Workload", "Evidence"]]
        measurement_rows.extend([
            paragraph_text(measurement[key])
            for key in ("metric", "value", "workload", "evidence")
        ] for measurement in report["measurements"])
        measurement_table = Table(measurement_rows, colWidths=[31 * mm, 25 * mm, 48 * mm, None], repeatRows=1)
        measurement_table.setStyle(TableStyle([
            ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#e8edf3")),
            ("GRID", (0, 0), (-1, -1), 0.3, colors.HexColor("#b8c0c9")),
            ("VALIGN", (0, 0), (-1, -1), "TOP"),
            ("FONTSIZE", (0, 0), (-1, -1), 8),
        ]))
        story.append(measurement_table)

        story.append(Paragraph("Analysis of results and problems", styles["Heading3"]))
        for line in report["analysis"]:
            story.append(paragraph_text(line))
    if not report["summary"]:
        story.append(paragraph_text("None recorded."))
    for line in report["summary"]:
        story.append(paragraph_text(line))

    return story


def render_pdf(payload: dict[str, Any], output_path: Path) -> None:
    """Render a PDF with a machine-readable canonical payload in its metadata."""
    try:
        from pypdf import PdfReader, PdfWriter
        from reportlab.lib.pagesizes import A4
        from reportlab.lib.units import mm
        from reportlab.platypus import SimpleDocTemplate
    except ImportError as error:
        raise ReportError("PDF rendering requires bundled Python with reportlab and pypdf") from error

    output_path.parent.mkdir(parents=True, exist_ok=True)
    staging_path = output_path.with_name(f".{output_path.name}.{uuid.uuid4().hex}.render.pdf")
    try:
        story = _report_story(payload)
        document = SimpleDocTemplate(
            str(staging_path),
            pagesize=A4,
            rightMargin=18 * mm,
            leftMargin=18 * mm,
            topMargin=16 * mm,
            bottomMargin=16 * mm,
        )

        def draw_footer(canvas, active_document):
            canvas.saveState()
            canvas.setFont("Helvetica", 8)
            canvas.setFillColorRGB(0.35, 0.35, 0.35)
            canvas.drawRightString(A4[0] - 18 * mm, 8 * mm, f"Page {active_document.page}")
            canvas.restoreState()

        document.build(story, onFirstPage=draw_footer, onLaterPages=draw_footer)
        reader = PdfReader(str(staging_path))
        writer = PdfWriter()
        writer.append(reader)
        writer.add_metadata({
            "/Title": f"{payload['prefix']} release report",
            "/ReleaseReportData": _canonical_json(_pdf_payload(payload)),
        })
        with output_path.open("wb") as stream:
            writer.write(stream)
    finally:
        staging_path.unlink(missing_ok=True)


def verify_pdf(path: Path, payload: dict[str, Any]) -> None:
    """Read back the PDF, embedded payload, sections, and period before retention."""
    try:
        from pypdf import PdfReader
    except ImportError as error:
        raise ReportError("PDF verification requires bundled Python with pypdf") from error
    try:
        reader = PdfReader(str(path))
        if not reader.pages:
            raise ReportError("rendered PDF has no pages")
        metadata = reader.metadata
        if metadata is None or metadata.get("/ReleaseReportData") != _canonical_json(_pdf_payload(payload)):
            raise ReportError("rendered PDF payload metadata does not match the validated draft")
        text = " ".join(" ".join((page.extract_text() or "").split()) for page in reader.pages)
        missing = [title for title in SECTION_TITLES if title not in text]
        if missing:
            raise ReportError(f"rendered PDF is missing report sections: {', '.join(missing)}")
        if "Time-share chart" not in text:
            raise ReportError("rendered PDF is missing the time-share chart")
        for index, page in enumerate(reader.pages, start=1):
            if f"Page {index}" not in (page.extract_text() or ""):
                raise ReportError("rendered PDF is missing a page number")
        period = payload["draft"]["period"]
        if period["start"] not in text or period["end"] not in text:
            raise ReportError("rendered PDF period is not visible")
        for allocation in payload["draft"]["time_spent"]["allocations"]:
            if any(allocation[key] not in text for key in ("label", "basis", "evidence")):
                raise ReportError("rendered PDF is missing time allocation details")
        for label in _time_share_labels(payload["draft"]["time_spent"]["allocations"], _period_duration_hours(period)):
            if label not in text:
                raise ReportError("rendered PDF is missing a time-share chart label")
        feature_names = {feature["id"]: feature["name"] for feature in payload["features"]}
        display_states = sorted(
            enumerate(payload["draft"]["feature_state"]),
            key=lambda item: (_STATUS_ORDER[item[1]["status"]], item[0]),
        )
        previous_position = -1
        for _, state in display_states:
            status_line = _feature_status_line(feature_names[state["id"]], state)
            position = text.find(status_line)
            if position < 0:
                raise ReportError("rendered PDF has an incorrect feature status")
            if position <= previous_position:
                raise ReportError("rendered PDF feature statuses are not in stable status order")
            previous_position = position
        report = payload["draft"]
        overview_positions = [text.find(" ".join(line.split())) for line in report.get("overview", [])]
        if any(position < 0 for position in overview_positions):
            raise ReportError("rendered PDF is missing release overview text")
        if overview_positions and display_states and max(overview_positions) >= text.find(
            _feature_status_line(feature_names[display_states[0][1]["id"]], display_states[0][1])
        ):
            raise ReportError("rendered PDF overview must appear before feature statuses")
        if "product_results" in report:
            major_headings = (
                "Product-level results",
                "Screenshots",
                "Measurements",
                "Analysis of results and problems",
            )
            if any(heading not in text for heading in major_headings):
                raise ReportError("rendered major report is missing a product evidence section")
            required_text = [
                *report["product_results"],
                *[screenshot["caption"] for screenshot in report["screenshots"]],
                *[item[key] for item in report["measurements"] for key in ("metric", "value", "workload", "evidence")],
                *report["analysis"],
            ]
            # Table cells can wrap inside paths and hashes during text extraction.
            compact_text = "".join(text.split())
            if any("".join(line.split()) not in compact_text for line in required_text):
                raise ReportError("rendered major report is missing product evidence")
            screenshot_hashes = payload.get("screenshot_sha256", [])
            screenshot_data = payload.get("_screenshot_data", [])
            if not isinstance(screenshot_hashes, list) or not isinstance(screenshot_data, list):
                raise ReportError("rendered major report screenshot evidence is malformed")
            expected_images = len(screenshot_hashes)
            actual_images = sum(len(page.images) for page in reader.pages)
            if expected_images != actual_images or expected_images != len(report["screenshots"]):
                raise ReportError("rendered major report screenshot images do not match its evidence")
            if (
                any(not isinstance(digest, str) or not _SHA256.fullmatch(digest) for digest in screenshot_hashes)
                or [hashlib.sha256(image).hexdigest() for image in screenshot_data] != screenshot_hashes
            ):
                raise ReportError("rendered major report screenshot hash is malformed")
    except OSError as error:
        raise ReportError(f"cannot read rendered PDF: {error}") from error


def _expected_report_paths(release_directory: Path, state: dict[str, Any]) -> dict[str, str]:
    expected: dict[str, str] = {}
    for period in state["periods"]:
        if "pdf_path" in period:
            expected[period["pdf_path"]] = period["pdf_sha256"]
    for aggregate in state["aggregates"]:
        expected[aggregate["pdf_path"]] = aggregate["pdf_sha256"]
    return expected


def _pending_prune_hashes(state: dict[str, Any]) -> dict[str, str]:
    pending = state.get("pending_pruning")
    if pending is None:
        return {}
    if not isinstance(pending, dict) or set(pending) != {"sources"} or not isinstance(pending["sources"], list):
        raise ReportError("pending report-pruning record is malformed")
    aggregates = state.get("aggregates", [])
    periods = state.get("periods", [])
    if not aggregates:
        raise ReportError("pending report-pruning record has no aggregate")
    profile = _reporting_profile(state)
    group_size = profile["minors_per_major"]
    precise_names = profile["minor_kind"] == "4h"
    covered = aggregates[-1].get("covered_period_indices")
    if not isinstance(covered, list) or len(covered) != group_size or covered != list(range(len(periods) - group_size, len(periods))):
        raise ReportError("pending report-pruning record is not for the latest completed aggregate")
    expected_paths = [
        f"{profile['minor_kind']}/{_filename(state['prefix'], _parse_datetime(periods[index]['start'], 'period start'), precise=precise_names)}"
        for index in covered[:-1]
    ]
    sources = pending["sources"]
    if len(sources) != len(expected_paths):
        raise ReportError("pending report-pruning record has an invalid source count")
    result: dict[str, str] = {}
    for source, expected_path in zip(sources, expected_paths):
        if (
            not isinstance(source, dict)
            or set(source) != {"path", "sha256"}
            or source["path"] != expected_path
            or not isinstance(source["sha256"], str)
            or not _SHA256.fullmatch(source["sha256"])
        ):
            raise ReportError("pending report-pruning source is malformed or unexpected")
        result[expected_path] = source["sha256"]
    return result


def _verify_managed_tree(release_directory: Path, state: dict[str, Any]) -> None:
    profile = _reporting_profile(state)
    report_directories = (profile["minor_kind"], "12h")
    expected_root_entries = {".report-owner.json", ".report-state.json", *report_directories}
    actual_root_entries = {entry.name for entry in release_directory.iterdir()}
    if actual_root_entries != expected_root_entries:
        raise ReportError("managed release folder contains an unrecognized file or directory")
    expected = _expected_report_paths(release_directory, state)
    pending = _pending_prune_hashes(state)
    actual: set[str] = set()
    for directory_name in report_directories:
        directory = release_directory / directory_name
        if directory.is_symlink():
            raise ReportError("managed report directories must not be symlinks")
        for entry in directory.iterdir():
            if entry.is_symlink() or not entry.is_file():
                raise ReportError("managed release folder contains an unrecognized file")
            relative = str(entry.relative_to(release_directory))
            actual.add(relative)
            if not relative.startswith(f"{directory_name}/") or (relative not in expected and relative not in pending):
                raise ReportError("managed release folder contains an unrecognized file")
            digest = hashlib.sha256(entry.read_bytes()).hexdigest()
            expected_digest = expected.get(relative, pending.get(relative))
            if digest != expected_digest:
                label = "pending standalone report" if relative in pending else "retained report"
                raise ReportError(f"{label} changed after verification: {relative}")
    if not set(expected).issubset(actual):
        raise ReportError("a retained report is missing from the managed release folder")


def check_release_state(reports_root: str | Path, release_id: str) -> dict[str, Any]:
    """Read-only, standard-library validation of one active managed release."""
    release_directory, _, state_path = _state_paths(reports_root, release_id)
    state = _read_json(state_path)
    state_keys = {
        "schema_version", "release_id", "prefix", "required_gates", "owner_token",
        "manifest_revisions", "periods", "aggregates",
    }
    allowed_state_keys = {
        frozenset(state_keys),
        frozenset(state_keys | {"pending_pruning"}),
        frozenset(state_keys | {"reporting"}),
        frozenset(state_keys | {"reporting", "pending_pruning"}),
    }
    if not isinstance(state, dict) or frozenset(state) not in allowed_state_keys:
        raise ReportError("unsupported or malformed report state")
    if state.get("schema_version") != SCHEMA_VERSION or not isinstance(state.get("periods"), list):
        raise ReportError("unsupported or malformed report state")
    profile = _reporting_profile(state)
    precise_names = profile["minor_kind"] == "4h"
    group_size = profile["minors_per_major"]
    revisions = state.get("manifest_revisions")
    gates = state.get("required_gates")
    if not isinstance(revisions, list) or not revisions or not isinstance(gates, list) or not gates:
        raise ReportError("report state is missing its manifest or delivery gates")
    gate_ids = [_safe_component(gate, "gate ID") for gate in gates]
    if len(set(gate_ids)) != len(gate_ids):
        raise ReportError("report state has duplicate delivery gate IDs")
    _safe_component(state.get("prefix"), "filename prefix")
    if not isinstance(revisions[0], dict):
        raise ReportError("initial feature manifest revision is malformed")
    _features_by_id(revisions[0].get("features", []))
    prior_features: list[dict[str, str]] = []
    period_count = len(state["periods"])
    for index, revision in enumerate(revisions):
        if not isinstance(revision, dict) or set(revision) != {"revision", "effective_period_index", "features", "addition_authority"}:
            raise ReportError("malformed feature manifest revision")
        if (
            revision["revision"] != index + 1
            or isinstance(revision["effective_period_index"], bool)
            or not isinstance(revision["effective_period_index"], int)
        ):
            raise ReportError("feature manifest revisions are not ordered")
        effective_index = revision["effective_period_index"]
        if not 0 <= effective_index <= period_count or (index and effective_index < revisions[index - 1]["effective_period_index"]):
            raise ReportError("feature manifest revision has an invalid effective period")
        features = revision["features"]
        _features_by_id(features)
        if index == 0:
            if effective_index != 0 or revision["addition_authority"] is not None:
                raise ReportError("initial feature manifest revision is malformed")
        else:
            if features[:-1] != prior_features or len(features) != len(prior_features) + 1:
                raise ReportError("feature manifest revisions must append exactly one feature")
            authority = revision["addition_authority"]
            if not isinstance(authority, dict) or set(authority) != {"reference", "text_sha256"}:
                raise ReportError("feature addition lacks request provenance")
            if (
                not _single_line(authority["reference"], "feature addition authority reference")
                or not isinstance(authority["text_sha256"], str)
                or not _SHA256.fullmatch(authority["text_sha256"])
            ):
                raise ReportError("feature addition provenance is malformed")
        prior_features = features

    completed_period_count = (period_count // group_size) * group_size
    aggregates = state.get("aggregates")
    if not isinstance(aggregates, list) or len(aggregates) != period_count // group_size:
        raise ReportError("report state does not contain one aggregate for each complete minor group")
    previous_end: datetime | None = None
    for index, record in enumerate(state["periods"]):
        has_pdf = index >= completed_period_count
        expected_keys = {"start", "end", "draft", "manifest_revision", "manifest_sha256"}
        if has_pdf:
            expected_keys |= {"pdf_path", "pdf_sha256"}
        if not isinstance(record, dict) or set(record) != expected_keys:
            raise ReportError("3-hour report history does not match the retention schedule")
        start = _parse_datetime(record["start"], "period start")
        end = _parse_datetime(record["end"], "period end")
        if index == 0 and profile["anchor"] is not None and start != profile["anchor"]:
            raise ReportError("first minor report does not begin at the implementation anchor")
        if previous_end is not None and start != previous_end:
            raise ReportError("report periods must be contiguous")
        draft_period = record["draft"].get("period") if isinstance(record["draft"], dict) else None
        if not isinstance(draft_period, dict) or draft_period.get("start") != record["start"] or draft_period.get("end") != record["end"]:
            raise ReportError("report history period does not match its draft")
        revision = _manifest_revision(state, index)
        if record["manifest_revision"] != revision["revision"] or record["manifest_sha256"] != _revision_hash(revision):
            raise ReportError("report history references the wrong feature manifest")
        validate_draft(
            record["draft"],
            revision["features"],
            profile["minor_kind"],
            minor_kind=profile["minor_kind"],
            minors_per_major=group_size,
        )
        if has_pdf:
            expected_relative = f"{profile['minor_kind']}/{_filename(state['prefix'], start, precise=precise_names)}"
            if record["pdf_path"] != expected_relative or not _SHA256.fullmatch(str(record["pdf_sha256"])):
                raise ReportError("standalone report path or hash is malformed")
        previous_end = end

    for group_index, aggregate in enumerate(aggregates):
        first_index = group_index * group_size
        last_index = first_index + group_size - 1
        expected_indices = list(range(first_index, last_index + 1))
        aggregate_keys = {
            "start", "end", "covered_period_indices", "draft", "manifest_revision",
            "manifest_sha256", "features", "pdf_path", "pdf_sha256",
        }
        if not isinstance(aggregate, dict) or set(aggregate) != aggregate_keys or aggregate.get("covered_period_indices") != expected_indices:
            raise ReportError("12-hour aggregate has an invalid coverage index")
        final_period = state["periods"][last_index]
        summary = aggregate.get("draft")
        if not isinstance(summary, dict):
            raise ReportError("12-hour aggregate summary is malformed")
        prior_state = {key: value for key, value in state.items() if key != "pending_pruning"}
        prior_state["periods"] = state["periods"][:last_index]
        revision, _, _, aggregate_required = _validate_append_inputs(
            prior_state,
            final_period["draft"],
            summary,
            check_screenshots=False,
        )
        if not aggregate_required:
            raise ReportError("12-hour aggregate is not aligned to a complete minor group")
        if (
            aggregate.get("start") != summary["period"]["start"]
            or aggregate.get("end") != summary["period"]["end"]
            or aggregate.get("manifest_revision") != revision["revision"]
            or aggregate.get("manifest_sha256") != _revision_hash(revision)
            or aggregate.get("features") != revision["features"]
        ):
            raise ReportError("12-hour aggregate metadata does not match its validated summary")
        summary_start = _parse_datetime(summary["period"]["start"], "summary start")
        expected_relative = f"12h/{_filename(state['prefix'], summary_start, precise=precise_names)}"
        if aggregate.get("pdf_path") != expected_relative or not _SHA256.fullmatch(str(aggregate.get("pdf_sha256", ""))):
            raise ReportError("12-hour aggregate path or hash is malformed")

    _verify_managed_tree(release_directory, state)
    return state


def check_all_release_states(reports_root: str | Path) -> list[dict[str, Any]]:
    """Read-only validation of every direct managed release folder."""
    root = _reports_root(reports_root)
    if not root.exists():
        return []
    if not root.is_dir():
        raise ReportError("reports root must be a directory")
    entries = sorted(root.iterdir(), key=lambda entry: entry.name)
    if any(entry.is_symlink() or not entry.is_dir() for entry in entries):
        raise ReportError("reports root contains an unrecognized file or symlink")
    return [check_release_state(root, entry.name) for entry in entries]


def finish_release(
    reports_root: str | Path,
    release_id: str,
    receipt: dict[str, Any],
    evidence_root: str | Path,
) -> None:
    """Delete only a complete, verified managed release report folder."""
    release_directory = _release_directory(reports_root, release_id)
    state = check_release_state(reports_root, release_id)
    if "pending_pruning" in state:
        raise ReportError("report pruning must be reconciled before full delivery")
    if not isinstance(receipt, dict) or receipt.get("release_id") != release_id or receipt.get("delivery_status") != "complete":
        raise ReportError("full-delivery receipt does not match this release")
    gates = receipt.get("gates")
    if not isinstance(gates, dict) or set(gates) != set(state["required_gates"]):
        raise ReportError("full-delivery receipt must cover every required gate")
    evidence_root_path = Path(evidence_root).resolve(strict=True)
    for gate_id in state["required_gates"]:
        gate = gates[gate_id]
        if not isinstance(gate, dict) or gate.get("status") != "passed" or not isinstance(gate.get("evidence"), list) or not gate["evidence"]:
            raise ReportError(f"required delivery gate {gate_id} is not verified")
        for evidence in gate["evidence"]:
            if not isinstance(evidence, dict) or set(evidence) != {"path", "sha256"}:
                raise ReportError(f"gate {gate_id} has malformed evidence")
            evidence_path_value = evidence["path"]
            evidence_hash = evidence["sha256"]
            if not isinstance(evidence_path_value, str) or not isinstance(evidence_hash, str):
                raise ReportError(f"gate {gate_id} evidence path and hash must be strings")
            relative_path = Path(evidence_path_value)
            if relative_path.is_absolute() or ".." in relative_path.parts or not _SHA256.fullmatch(evidence_hash):
                raise ReportError(f"gate {gate_id} evidence path or hash is unsafe")
            evidence_path = evidence_root_path / relative_path
            if evidence_path.is_symlink() or not evidence_path.is_file():
                raise ReportError(f"gate {gate_id} evidence file is missing or unsafe")
            try:
                evidence_path.resolve(strict=True).relative_to(evidence_root_path)
            except ValueError as error:
                raise ReportError(f"gate {gate_id} evidence escapes its evidence root") from error
            digest = hashlib.sha256(evidence_path.read_bytes()).hexdigest()
            if digest != evidence_hash:
                raise ReportError(f"gate {gate_id} evidence hash does not match")
    shutil.rmtree(release_directory)


def reconcile_pruning(reports_root: str | Path, release_id: str) -> dict[str, Any]:
    """Resume only the exact hash-verified standalone deletions recorded by an aggregate."""
    state = check_release_state(reports_root, release_id)
    pending = _pending_prune_hashes(state)
    if not pending:
        return state
    release_directory, _, state_path = _state_paths(reports_root, release_id)
    for relative, expected_hash in pending.items():
        candidate = release_directory / relative
        if candidate.is_symlink():
            raise ReportError(f"pending standalone report is unsafe: {relative}")
        if not candidate.exists():
            continue
        if not candidate.is_file() or hashlib.sha256(candidate.read_bytes()).hexdigest() != expected_hash:
            raise ReportError(f"pending standalone report changed after verification: {relative}")
        candidate.unlink()
    state.pop("pending_pruning")
    _atomic_json(state_path, state)
    return check_release_state(reports_root, release_id)


def _read_object(path: str | Path, label: str) -> dict[str, Any]:
    value = _read_json(Path(path))
    if not isinstance(value, dict):
        raise ReportError(f"{label} must contain a JSON object")
    return value


def _default_reports_root() -> Path:
    return Path(__file__).resolve().parent.parent / "docs" / "ai" / "reports"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reports-root", type=Path, default=_default_reports_root())
    subparsers = parser.add_subparsers(dest="command", required=True)
    start = subparsers.add_parser("start", help="create a managed release report folder")
    start.add_argument("release_id")
    start.add_argument("prefix")
    start.add_argument("--features", type=Path, required=True)
    start.add_argument("--gate", action="append", dest="gates", required=True)
    start.add_argument("--anchor", required=True, help="timezone-aware implementation-start datetime")
    add = subparsers.add_parser("add-feature", help="append an explicitly requested feature revision")
    add.add_argument("release_id")
    add.add_argument("--feature", type=Path, required=True)
    add.add_argument("--authority-reference", required=True)
    add.add_argument("--authority-text-sha256", required=True)
    check = subparsers.add_parser("check", help="validate a report draft without saving a PDF")
    check.add_argument("release_id")
    check.add_argument("--draft", type=Path, required=True)
    check.add_argument("--summary-12h", type=Path)
    check_state = subparsers.add_parser("check-state", help="validate active managed report folders")
    check_state.add_argument("release_id", nargs="?")
    append = subparsers.add_parser("append", help="validate and save the next report")
    append.add_argument("release_id")
    append.add_argument("--draft", type=Path, required=True)
    append.add_argument("--summary-12h", type=Path)
    reconcile = subparsers.add_parser("reconcile", help="resume verified standalone pruning after an interrupted aggregate")
    reconcile.add_argument("release_id")
    finish = subparsers.add_parser("finish", help="verify delivery and remove the managed report folder")
    finish.add_argument("release_id")
    finish.add_argument("--receipt", type=Path, required=True)
    finish.add_argument("--evidence-root", type=Path, default=Path.cwd())
    args = parser.parse_args(argv)
    try:
        if args.command == "start":
            features = _read_json(args.features)
            directory = start_release(
                args.reports_root,
                args.release_id,
                args.prefix,
                features,
                args.gates,
                anchor=args.anchor,
            )
            print(directory)
        elif args.command == "add-feature":
            add_feature(
                args.reports_root,
                args.release_id,
                _read_object(args.feature, "feature"),
                {"reference": args.authority_reference, "text_sha256": args.authority_text_sha256},
            )
            print("feature revision appended")
        elif args.command == "check":
            state = load_release_state(args.reports_root, args.release_id)
            draft = _read_object(args.draft, "draft")
            summary = _read_object(args.summary_12h, "12-hour summary") if args.summary_12h else None
            _validate_append_inputs(state, draft, summary)
            print("draft valid")
        elif args.command == "check-state":
            states = (
                [check_release_state(args.reports_root, args.release_id)]
                if args.release_id
                else check_all_release_states(args.reports_root)
            )
            if not states:
                print("no managed release report folders")
            for state in states:
                suffix = " (pruning recovery needed)" if "pending_pruning" in state else ""
                print(f"{state['release_id']}: valid{suffix}")
        elif args.command == "append":
            draft = _read_object(args.draft, "draft")
            summary = _read_object(args.summary_12h, "12-hour summary") if args.summary_12h else None
            for output in append_report(args.reports_root, args.release_id, draft, summary):
                print(output)
        elif args.command == "reconcile":
            state = reconcile_pruning(args.reports_root, args.release_id)
            print(f"{state['release_id']}: report pruning reconciled")
        elif args.command == "finish":
            finish_release(args.reports_root, args.release_id, _read_object(args.receipt, "receipt"), args.evidence_root)
            print("managed release report folder removed after verified delivery")
    except (OSError, ReportError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
