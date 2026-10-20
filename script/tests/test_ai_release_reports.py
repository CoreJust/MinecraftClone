from __future__ import annotations

from datetime import datetime, timedelta, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPT_DIR))
import ai_release_reports


FEATURES = [
    {"id": "terrain", "name": "Terrain streaming"},
    {"id": "render", "name": "Terrain rendering"},
]
PREFIX = "MC-EarlyDev-0.1.0-S7"
BASE = datetime(2026, 10, 18, 0, 0, tzinfo=timezone(timedelta(hours=2)))
ADD_FEATURE_AUTHORITY = {
    "reference": "explicit user feature-addition request",
    "text_sha256": hashlib.sha256(b"Please add the network feature to this release.").hexdigest(),
}


def draft_for(index: int, features: list[dict[str, str]] | None = None) -> dict[str, object]:
    active_features = FEATURES if features is None else features
    start = BASE + timedelta(hours=3 * index)
    end = start + timedelta(hours=3)
    feature_state = [
        {"id": feature["id"], "status": "In progress", "detail": "Work continues."}
        for feature in active_features
    ]
    return {
        "period": {
            "kind": "3h",
            "start": start.isoformat(),
            "end": end.isoformat(),
        },
        "feature_state": feature_state,
        "expected_progress": ["Complete the scoped validation."],
        "feature_progress": [],
        "other_progress": [],
        "time_spent": {
            "allocations": [
                {"label": "Implementation", "hours": 1.0, "basis": "measured", "evidence": "Timer record."},
                {"label": "Diagnostics and tests", "hours": 1.0, "basis": "estimated", "evidence": "Dated work notes."},
                {"label": "Unaccounted", "hours": 1.0, "basis": "unaccounted", "evidence": "No reliable activity record."},
            ],
            "parallel_note": "Parallel spans were attributed once to their primary activity.",
        },
        "issues": [],
        "conclusions": [],
        "next_goals": ["Run the next scoped check."],
        "summary": ["The report period is recorded without claiming release completion."],
    }


def summary_for(
    features: list[dict[str, str]] | None = None,
    *,
    feature_progress: list[dict[str, object]] | None = None,
) -> dict[str, object]:
    active_features = FEATURES if features is None else features
    report = draft_for(0, active_features)
    report["period"] = {
        "kind": "12h",
        "start": BASE.isoformat(),
        "end": (BASE + timedelta(hours=12)).isoformat(),
    }
    report["covered_periods"] = [
        {
            "start": (BASE + timedelta(hours=3 * index)).isoformat(),
            "end": (BASE + timedelta(hours=3 * (index + 1))).isoformat(),
        }
        for index in range(4)
    ]
    report["time_spent"] = {
        "allocations": [
            {"label": "Implementation", "hours": 4.0, "basis": "measured", "evidence": "Timer records."},
            {"label": "Diagnostics and tests", "hours": 4.0, "basis": "estimated", "evidence": "Dated work notes."},
            {"label": "Unaccounted", "hours": 4.0, "basis": "unaccounted", "evidence": "No reliable activity record."},
        ],
        "parallel_note": "Parallel spans were attributed once to their primary activity.",
    }
    report["feature_progress"] = [] if feature_progress is None else feature_progress
    return report


def four_hour_draft(
    start: datetime,
    duration_hours: float = 4.0,
    features: list[dict[str, str]] | None = None,
) -> dict[str, object]:
    report = draft_for(0, features)
    end = start + timedelta(hours=duration_hours)
    report["period"] = {"kind": "4h", "start": start.isoformat(), "end": end.isoformat()}
    report["time_spent"]["allocations"] = [
        {"label": "Implementation", "hours": duration_hours / 2, "basis": "measured", "evidence": "Timer record."},
        {"label": "Diagnostics and tests", "hours": duration_hours / 4, "basis": "estimated", "evidence": "Dated work notes."},
        {"label": "Unaccounted", "hours": duration_hours / 4, "basis": "unaccounted", "evidence": "No reliable activity record."},
    ]
    return report


def major_summary(
    periods: list[dict[str, object]],
    features: list[dict[str, str]] | None = None,
    *,
    feature_progress: list[dict[str, object]] | None = None,
) -> dict[str, object]:
    active_features = FEATURES if features is None else features
    first = periods[0]["period"]
    last = periods[-1]["period"]
    report = draft_for(0, active_features)
    total_hours = sum(
        (datetime.fromisoformat(item["period"]["end"]) - datetime.fromisoformat(item["period"]["start"])).total_seconds() / 3600
        for item in periods
    )
    report["period"] = {
        "kind": "12h",
        "start": first["start"],
        "end": last["end"],
    }
    report["covered_periods"] = [
        {"start": item["period"]["start"], "end": item["period"]["end"]}
        for item in periods
    ]
    report["time_spent"] = {
        "allocations": [
            {"label": "Implementation", "hours": total_hours / 2, "basis": "measured", "evidence": "Timer records."},
            {"label": "Diagnostics and tests", "hours": total_hours / 4, "basis": "estimated", "evidence": "Dated work notes."},
            {"label": "Unaccounted", "hours": total_hours / 4, "basis": "unaccounted", "evidence": "No reliable activity record."},
        ],
        "parallel_note": "Parallel spans were attributed once to their primary activity.",
    }
    report["feature_progress"] = [] if feature_progress is None else feature_progress
    report["product_results"] = ["Terrain loads in the measured ordinary-travel scenario."]
    report["screenshots"] = []
    report["measurements"] = [{
        "metric": "fresh frame rate",
        "value": "120 FPS",
        "workload": "Sustained forward movement",
        "evidence": "Frame-time capture 17.",
    }]
    report["analysis"] = ["The moving workload meets the target; reverse travel remains unmeasured."]
    return report


class AiReleaseReportsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.root = Path(self.temporary_directory.name) / "reports"

    def start_release(self) -> Path:
        release_directory = ai_release_reports.start_release(
            self.root,
            "S7",
            PREFIX,
            FEATURES,
            ["source", "tests", "platforms", "release-artifacts"],
            anchor=BASE.isoformat(),
        )
        (release_directory / "4h").rename(release_directory / "3h")
        state_path = release_directory / ".report-state.json"
        state = json.loads(state_path.read_text(encoding="utf-8"))
        state.pop("reporting")
        state_path.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
        owner_path = release_directory / ".report-owner.json"
        owner = json.loads(owner_path.read_text(encoding="utf-8"))
        owner.pop("anchor")
        owner_path.write_text(json.dumps(owner, indent=2) + "\n", encoding="utf-8")
        return release_directory

    def start_new_release(self) -> Path:
        return ai_release_reports.start_release(
            self.root,
            "S7",
            PREFIX,
            FEATURES,
            ["source", "tests", "platforms", "release-artifacts"],
            anchor=BASE.isoformat(),
        )

    def mock_pdf(self):
        def render(_payload: dict[str, object], output_path: Path) -> None:
            output_path.write_bytes(b"%PDF-1.4 test fixture")

        return (
            mock.patch.object(ai_release_reports, "render_pdf", side_effect=render),
            mock.patch.object(ai_release_reports, "verify_pdf", return_value=None),
        )

    def test_new_minor_accepts_early_closure_and_allocates_only_actual_hours(self) -> None:
        report = four_hour_draft(BASE, 2.5)
        ai_release_reports.validate_draft(report, FEATURES, "4h")

        too_long = four_hour_draft(BASE, 4.01)
        with self.assertRaisesRegex(ai_release_reports.ReportError, "at most 4 hours"):
            ai_release_reports.validate_draft(too_long, FEATURES, "4h")

        zero_length = four_hour_draft(BASE, 0)
        with self.assertRaisesRegex(ai_release_reports.ReportError, "positive duration"):
            ai_release_reports.validate_draft(zero_length, FEATURES, "4h")

    def test_new_release_is_anchored_and_periods_are_contiguous_actual_intervals(self) -> None:
        with self.assertRaisesRegex(ai_release_reports.ReportError, "anchor"):
            ai_release_reports.start_release(
                self.root,
                "unanchored",
                PREFIX,
                FEATURES,
                ["source"],
            )

        release_directory = self.start_new_release()
        self.assertTrue((release_directory / "4h").is_dir())
        self.assertTrue((release_directory / "12h").is_dir())
        state = ai_release_reports.load_release_state(self.root, "S7")
        self.assertEqual(state["reporting"], {
            "anchor": BASE.isoformat(),
            "minor_hours": 4,
            "minors_per_major": 3,
        })

        render, verify = self.mock_pdf()
        with render, verify:
            wrong_start = four_hour_draft(BASE + timedelta(minutes=1), 2)
            with self.assertRaisesRegex(ai_release_reports.ReportError, "implementation anchor"):
                ai_release_reports.append_report(self.root, "S7", wrong_start)

            first = four_hour_draft(BASE, 0.25)
            ai_release_reports.append_report(self.root, "S7", first)
            gap = four_hour_draft(BASE + timedelta(minutes=15, seconds=1), 1)
            with self.assertRaisesRegex(ai_release_reports.ReportError, "contiguous"):
                ai_release_reports.append_report(self.root, "S7", gap)

            second = four_hour_draft(BASE + timedelta(minutes=15), 0.5)
            ai_release_reports.append_report(self.root, "S7", second)

        checked = ai_release_reports.check_release_state(self.root, "S7")
        self.assertEqual(len(checked["periods"]), 2)
        self.assertEqual(len(list((release_directory / "4h").glob("*.pdf"))), 2)

    def test_three_minor_major_validates_readback_before_pruning_covered_reports(self) -> None:
        release_directory = self.start_new_release()
        durations = [2.0, 3.0, 1.5]
        periods = []
        start = BASE
        render, verify = self.mock_pdf()
        with render, verify:
            for duration in durations[:2]:
                report = four_hour_draft(start, duration)
                ai_release_reports.append_report(self.root, "S7", report)
                periods.append(report)
                start = datetime.fromisoformat(report["period"]["end"])

            third = four_hour_draft(start, durations[2])
            summary = major_summary([*periods, third])
            standalone_directory = release_directory / "4h"
            source_reports = sorted(standalone_directory.glob("*.pdf"))
            self.assertEqual(len(source_reports), 2)

            with self.assertRaisesRegex(ai_release_reports.ReportError, "12-hour summary"):
                ai_release_reports.append_report(self.root, "S7", third)

            wrong_coverage = major_summary([*periods, third])
            wrong_coverage["covered_periods"][0]["start"] = (BASE + timedelta(minutes=1)).isoformat()
            with self.assertRaisesRegex(ai_release_reports.ReportError, "covered periods"):
                ai_release_reports.append_report(self.root, "S7", third, wrong_coverage)
            self.assertEqual(len(list(standalone_directory.glob("*.pdf"))), 2)

            missing_analysis = major_summary([*periods, third])
            del missing_analysis["analysis"]
            with self.assertRaisesRegex(ai_release_reports.ReportError, "major report fields"):
                ai_release_reports.append_report(self.root, "S7", third, missing_analysis)
            self.assertEqual(len(list(standalone_directory.glob("*.pdf"))), 2)

            original_verify = ai_release_reports.verify_pdf

            def fail_major_readback(path: Path, payload: dict[str, object]) -> None:
                if path.parent.name == "12h":
                    raise ai_release_reports.ReportError("simulated aggregate readback failure")
                original_verify(path, payload)

            with mock.patch.object(ai_release_reports, "verify_pdf", side_effect=fail_major_readback):
                with self.assertRaisesRegex(ai_release_reports.ReportError, "readback failure"):
                    ai_release_reports.append_report(self.root, "S7", third, summary)
            self.assertEqual(len(list(standalone_directory.glob("*.pdf"))), 2)
            self.assertEqual(list((release_directory / "12h").glob("*.pdf")), [])

            ai_release_reports.append_report(self.root, "S7", third, summary)

        checked = ai_release_reports.check_release_state(self.root, "S7")
        self.assertEqual(len(checked["periods"]), 3)
        self.assertEqual(len(checked["aggregates"]), 1)
        self.assertEqual(list((release_directory / "4h").glob("*.pdf")), [])
        self.assertEqual(len(list((release_directory / "12h").glob("*.pdf"))), 1)
        self.assertEqual(checked["aggregates"][0]["covered_period_indices"], [0, 1, 2])

    @unittest.skipUnless(
        all(importlib.util.find_spec(name) for name in ("reportlab", "pypdf", "PIL")),
        "bundled PDF and image libraries are required",
    )
    def test_major_pdf_readback_keeps_status_identity_and_product_evidence(self) -> None:
        from PIL import Image
        from pypdf import PdfReader, PdfWriter

        release_directory = self.start_new_release()
        old_cwd = Path.cwd()
        try:
            os.chdir(self.temporary_directory.name)
            Image.new("RGB", (32, 20), (20, 120, 80)).save("terrain.png")
            periods = [
                four_hour_draft(BASE, 2),
                four_hour_draft(BASE + timedelta(hours=2), 3),
                four_hour_draft(BASE + timedelta(hours=5), 1),
            ]
            summary = major_summary(periods)
            summary["feature_state"] = [
                {"id": "terrain", "status": "Planned", "detail": ""},
                {"id": "render", "status": "Done", "detail": ""},
            ]
            summary["screenshots"] = [{"path": "terrain.png", "caption": "Terrain after movement."}]
            long_evidence = "/Users/terrain-evidence/" + "abcdef0123456789" * 10 + ".json"
            summary["measurements"][0]["evidence"] = long_evidence
            for index, period in enumerate(periods):
                ai_release_reports.append_report(self.root, "S7", period, summary if index == 2 else None)

            aggregate_path = next((release_directory / "12h").glob("*.pdf"))
            reader = PdfReader(str(aggregate_path))
            text = " ".join(page.extract_text() or "" for page in reader.pages)
            embedded_data = json.loads(reader.metadata["/ReleaseReportData"])
            done_text = "Terrain rendering [render] - Done"
            planned_text = "Terrain streaming [terrain] - planned"
            self.assertLess(text.index(done_text), text.index(planned_text))
            self.assertIn("Terrain after movement.", text)
            self.assertIn("fresh frame rate", text)
            self.assertIn("120 FPS", text)
            self.assertIn("The moving workload meets the target", text)
            self.assertIn(long_evidence, "".join(text.split()))
            self.assertEqual(sum(len(page.images) for page in reader.pages), 1)
            self.assertEqual(
                embedded_data["screenshot_sha256"],
                [hashlib.sha256(Path("terrain.png").read_bytes()).hexdigest()],
            )
            checked = ai_release_reports.check_release_state(self.root, "S7")
            self.assertEqual(len(checked["aggregates"]), 1)

            missing_payload = json.loads(reader.metadata["/ReleaseReportData"])
            missing_payload["draft"]["measurements"][0]["value"] = "missing-visible-evidence-guard"
            missing_path = Path(self.temporary_directory.name) / "missing-visible-evidence.pdf"
            writer = PdfWriter()
            writer.append(reader)
            writer.add_metadata({"/ReleaseReportData": ai_release_reports._canonical_json(missing_payload)})
            with missing_path.open("wb") as stream:
                writer.write(stream)
            with self.assertRaisesRegex(ai_release_reports.ReportError, "missing product evidence"):
                ai_release_reports.verify_pdf(missing_path, missing_payload)
        finally:
            os.chdir(old_cwd)

    def test_manifest_additions_are_append_only_and_apply_to_future_periods(self) -> None:
        release_directory = self.start_release()
        added = {"id": "network", "name": "Network delivery"}
        with self.assertRaisesRegex(ai_release_reports.ReportError, "user-request reference"):
            ai_release_reports.add_feature(self.root, "S7", added)
        ai_release_reports.add_feature(self.root, "S7", added, ADD_FEATURE_AUTHORITY)

        state = ai_release_reports.load_release_state(self.root, "S7")
        self.assertEqual([feature["id"] for feature in state["manifest_revisions"][0]["features"]], ["terrain", "render"])
        self.assertEqual([feature["id"] for feature in state["manifest_revisions"][1]["features"]], ["terrain", "render", "network"])
        self.assertEqual(state["manifest_revisions"][1]["effective_period_index"], 0)
        self.assertEqual(state["manifest_revisions"][1]["addition_authority"], ADD_FEATURE_AUTHORITY)
        self.assertTrue((release_directory / ".report-state.json").is_file())

        with self.assertRaisesRegex(ai_release_reports.ReportError, "already exists"):
            ai_release_reports.add_feature(self.root, "S7", added, ADD_FEATURE_AUTHORITY)

    def test_report_validation_rejects_manifest_order_status_line_and_time_errors(self) -> None:
        report = draft_for(0)
        ai_release_reports.validate_draft(report, FEATURES, "3h")

        reordered = draft_for(0)
        reordered["feature_state"] = list(reversed(reordered["feature_state"]))
        with self.assertRaisesRegex(ai_release_reports.ReportError, "feature order"):
            ai_release_reports.validate_draft(reordered, FEATURES, "3h")

        malformed_status = draft_for(0)
        malformed_status["feature_state"][0] = {"id": "terrain", "status": "done"}
        with self.assertRaisesRegex(ai_release_reports.ReportError, "status"):
            ai_release_reports.validate_draft(malformed_status, FEATURES, "3h")

        too_many_lines = draft_for(0)
        too_many_lines["expected_progress"] = ["progress"] * 17
        with self.assertRaisesRegex(ai_release_reports.ReportError, "16 lines"):
            ai_release_reports.validate_draft(too_many_lines, FEATURES, "3h")

        invented_time = draft_for(0)
        invented_time["time_spent"]["allocations"][2]["hours"] = 0.5
        with self.assertRaisesRegex(ai_release_reports.ReportError, "sum to the period"):
            ai_release_reports.validate_draft(invented_time, FEATURES, "3h")

        too_many_time_categories = draft_for(0)
        too_many_time_categories["time_spent"]["allocations"] *= 3
        with self.assertRaisesRegex(ai_release_reports.ReportError, "at most 8"):
            ai_release_reports.validate_draft(too_many_time_categories, FEATURES, "3h")

    def test_rendered_status_and_time_share_labels_match_report_contract(self) -> None:
        self.assertEqual(ai_release_reports._feature_status_line("Terrain streaming", {"status": "Done"}), "Terrain streaming - Done")
        self.assertEqual(
            ai_release_reports._feature_status_line("Terrain rendering", {"status": "In progress", "detail": "Work continues."}),
            "Terrain rendering - in progress: Work continues.",
        )
        self.assertEqual(ai_release_reports._feature_status_line("Network delivery", {"status": "Planned"}), "Network delivery - planned")
        labels = ai_release_reports._time_share_labels(draft_for(0)["time_spent"]["allocations"], 3)
        self.assertEqual(labels, [
            "Implementation - 1 h (33.3%, measured)",
            "Diagnostics and tests - 1 h (33.3%, estimated)",
            "Unaccounted - 1 h (33.3%, unaccounted)",
        ])

    def test_periods_must_be_timezone_aware_contiguous_and_exactly_three_hours(self) -> None:
        report = draft_for(0)
        report["period"]["start"] = "2026-10-18T00:00:00"
        with self.assertRaisesRegex(ai_release_reports.ReportError, "timezone"):
            ai_release_reports.validate_draft(report, FEATURES, "3h")

        report = draft_for(0)
        report["period"]["end"] = (BASE + timedelta(hours=4)).isoformat()
        with self.assertRaisesRegex(ai_release_reports.ReportError, "exactly 3 hours"):
            ai_release_reports.validate_draft(report, FEATURES, "3h")

        release_directory = self.start_release()
        render, verify = self.mock_pdf()
        with render, verify:
            ai_release_reports.append_report(self.root, "S7", draft_for(0))
            gap = draft_for(2)
            with self.assertRaisesRegex(ai_release_reports.ReportError, "contiguous"):
                ai_release_reports.append_report(self.root, "S7", gap)
        self.assertEqual(len(list((release_directory / "3h").glob("*.pdf"))), 1)

    def test_check_state_validates_active_reports_without_pdf_dependencies(self) -> None:
        release_directory = self.start_release()
        render, verify = self.mock_pdf()
        with render, verify:
            ai_release_reports.append_report(self.root, "S7", draft_for(0))

        checked = ai_release_reports.check_release_state(self.root, "S7")
        self.assertEqual(checked["release_id"], "S7")
        all_checked = ai_release_reports.check_all_release_states(self.root)
        self.assertEqual([item["release_id"] for item in all_checked], ["S7"])

        report_path = next((release_directory / "3h").glob("*.pdf"))
        report_path.write_bytes(b"changed after report verification")
        with self.assertRaisesRegex(ai_release_reports.ReportError, "changed after verification"):
            ai_release_reports.check_release_state(self.root, "S7")

    def test_fourth_period_requires_valid_aggregate_before_pruning_three_pdfs(self) -> None:
        release_directory = self.start_release()
        render, verify = self.mock_pdf()
        with render, verify:
            for index in range(3):
                ai_release_reports.append_report(self.root, "S7", draft_for(index))

            standalone_directory = release_directory / "3h"
            previous_reports = sorted(standalone_directory.glob("*.pdf"))
            self.assertEqual(len(previous_reports), 3)
            previous_bytes = {path: path.read_bytes() for path in previous_reports}

            with self.assertRaisesRegex(ai_release_reports.ReportError, "12-hour summary"):
                ai_release_reports.append_report(self.root, "S7", draft_for(3))
            self.assertEqual({path: path.read_bytes() for path in previous_reports}, previous_bytes)

            wrong_coverage = summary_for()
            wrong_coverage["covered_periods"][0]["start"] = (BASE + timedelta(minutes=1)).isoformat()
            with self.assertRaisesRegex(ai_release_reports.ReportError, "covered periods"):
                ai_release_reports.append_report(self.root, "S7", draft_for(3), wrong_coverage)
            self.assertEqual({path: path.read_bytes() for path in previous_reports}, previous_bytes)
            self.assertEqual(list((release_directory / "12h").glob("*.pdf")), [])

            tampered_path = previous_reports[0]
            tampered_path.write_bytes(b"tampered standalone report")
            with self.assertRaisesRegex(ai_release_reports.ReportError, "changed after verification"):
                ai_release_reports.append_report(self.root, "S7", draft_for(3), summary_for())
            self.assertEqual(len(list(standalone_directory.glob("*.pdf"))), 3)
            self.assertEqual(list((release_directory / "12h").glob("*.pdf")), [])
            tampered_path.write_bytes(previous_bytes[tampered_path])

            def verify_aggregate_first(path: Path, _payload: dict[str, object]) -> None:
                if path.parent.name == "12h":
                    self.assertEqual(len(list(standalone_directory.glob("*.pdf"))), 3)

            with mock.patch.object(ai_release_reports, "verify_pdf", side_effect=verify_aggregate_first):
                outputs = ai_release_reports.append_report(self.root, "S7", draft_for(3), summary_for())

        aggregate = release_directory / "12h" / f"{PREFIX}-2026-10-18-00-report.pdf"
        self.assertIn(aggregate, outputs)
        self.assertTrue(aggregate.is_file())
        self.assertEqual(list((release_directory / "3h").glob("*.pdf")), [])
        checked_state = ai_release_reports.check_release_state(self.root, "S7")
        self.assertEqual(len(checked_state["aggregates"]), 1)
        state = ai_release_reports.load_release_state(self.root, "S7")
        self.assertEqual(len(state["periods"]), 4)
        self.assertEqual(len(state["aggregates"]), 1)
        self.assertNotIn("pdf_path", state["periods"][3])

    def test_interrupted_pruning_is_hash_checked_and_safely_recoverable(self) -> None:
        release_directory = self.start_release()
        render, verify = self.mock_pdf()
        with render, verify:
            for index in range(3):
                ai_release_reports.append_report(self.root, "S7", draft_for(index))
            standalone_directory = release_directory / "3h"
            source_reports = sorted(standalone_directory.glob("*.pdf"))
            source_bytes = {path: path.read_bytes() for path in source_reports}
            failed_path = source_reports[1]
            original_unlink = Path.unlink
            injected = False

            def fail_second_prune(path: Path, *args, **kwargs):
                nonlocal injected
                if path == failed_path and not injected:
                    injected = True
                    raise OSError("simulated unlink failure")
                return original_unlink(path, *args, **kwargs)

            with mock.patch.object(Path, "unlink", new=fail_second_prune):
                with self.assertRaisesRegex(OSError, "simulated unlink failure"):
                    ai_release_reports.append_report(self.root, "S7", draft_for(3), summary_for())

            self.assertFalse(source_reports[0].exists())
            self.assertTrue(failed_path.is_file())
            self.assertTrue(source_reports[2].is_file())
            self.assertTrue(list((release_directory / "12h").glob("*.pdf")))
            self.assertIn("pending_pruning", ai_release_reports.load_release_state(self.root, "S7"))
            ai_release_reports.check_release_state(self.root, "S7")

            failed_path.write_bytes(b"tampered pending source")
            with self.assertRaisesRegex(ai_release_reports.ReportError, "pending standalone report changed"):
                ai_release_reports.reconcile_pruning(self.root, "S7")
            self.assertTrue(source_reports[2].is_file())

            failed_path.write_bytes(source_bytes[failed_path])
            recovered = ai_release_reports.reconcile_pruning(self.root, "S7")
            self.assertNotIn("pending_pruning", recovered)
            self.assertEqual(list(standalone_directory.glob("*.pdf")), [])
            self.assertEqual(len(list((release_directory / "12h").glob("*.pdf"))), 1)
            self.assertEqual(len(ai_release_reports.check_release_state(self.root, "S7")["aggregates"]), 1)

    def test_later_feature_addition_keeps_history_and_must_be_noted_in_twelve_hour_summary(self) -> None:
        release_directory = self.start_release()
        render, verify = self.mock_pdf()
        added_feature = {"id": "network", "name": "Network delivery"}
        latest_features = [*FEATURES, added_feature]
        with render, verify:
            ai_release_reports.append_report(self.root, "S7", draft_for(0))
            first_pdf = next((release_directory / "3h").glob("*.pdf"))
            first_pdf_hash = hashlib.sha256(first_pdf.read_bytes()).hexdigest()

            ai_release_reports.add_feature(self.root, "S7", added_feature, ADD_FEATURE_AUTHORITY)
            ai_release_reports.append_report(self.root, "S7", draft_for(1, latest_features))
            self.assertEqual(hashlib.sha256(first_pdf.read_bytes()).hexdigest(), first_pdf_hash)

            ai_release_reports.append_report(self.root, "S7", draft_for(2, latest_features))
            missing_addition_note = summary_for(latest_features)
            with self.assertRaisesRegex(ai_release_reports.ReportError, "addition.*network"):
                ai_release_reports.append_report(self.root, "S7", draft_for(3, latest_features), missing_addition_note)

            noted_addition = summary_for(
                latest_features,
                feature_progress=[{
                    "feature_id": "network",
                    "lines": ["Added to the release manifest by explicit request."],
                }],
            )
            ai_release_reports.append_report(self.root, "S7", draft_for(3, latest_features), noted_addition)

        state = ai_release_reports.load_release_state(self.root, "S7")
        self.assertEqual(state["periods"][0]["manifest_revision"], 1)
        self.assertEqual(state["periods"][1]["manifest_revision"], 2)
        self.assertIn("network", [feature["id"] for feature in state["aggregates"][0]["features"]])

    def test_finish_requires_verified_gate_receipts_and_refuses_unknown_files(self) -> None:
        release_directory = self.start_release()
        render, verify = self.mock_pdf()
        with render, verify:
            ai_release_reports.append_report(self.root, "S7", draft_for(0))

        evidence = self.root.parent / "gate-evidence.json"
        evidence.write_text("passed\n", encoding="utf-8")
        evidence_hash = hashlib.sha256(evidence.read_bytes()).hexdigest()
        gates = {
            gate: {
                "status": "passed",
                "evidence": [{"path": evidence.name, "sha256": evidence_hash}],
            }
            for gate in ("source", "tests", "platforms", "release-artifacts")
        }
        receipt = {"release_id": "S7", "delivery_status": "complete", "gates": gates}
        ai_release_reports.finish_release(self.root, "S7", receipt, self.root.parent)
        self.assertFalse(release_directory.exists())

        preserved = self.start_release()
        (preserved / "unowned.txt").write_text("keep", encoding="utf-8")
        with self.assertRaisesRegex(ai_release_reports.ReportError, "unrecognized file"):
            ai_release_reports.finish_release(self.root, "S7", receipt, self.root.parent)
        self.assertTrue((preserved / "unowned.txt").is_file())


if __name__ == "__main__":
    unittest.main()
