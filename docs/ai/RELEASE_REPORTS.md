# Release reports

`script/ai_release_reports.py` creates validated PDF progress reports for an
active release. The release owner supplies the ordered feature manifest,
report drafts, and delivery evidence; the tool validates the schedule and
content, embeds the validated JSON in each PDF, and manages only its marked
release folder.

## Start and update a release

Start a release once, with its ordered feature manifest and the exact gates
that must pass before the report folder can be removed:

```sh
python3 script/ai_release_reports.py --reports-root docs/ai/reports \
  start S7 MC-EarlyDev-0.1.0-S7 \
  --features /path/to/features.json \
  --gate source --gate tests --gate platforms --gate release-artifacts
```

The feature file is a JSON array of `{ "id": "...", "name": "..." }`
objects in the release's agreed order. The tool creates
`docs/ai/reports/S7/` with separate `3h/` and `12h/` PDF directories and hidden
JSON ownership/state files. Report files use
`<prefix>-<YYYY-MM-DD>-<HH>-report.pdf`, with the date/hour taken from the
period's timezone-aware start datetime. Use a release-specific prefix such as
`MC-EarlyDev-0.1.0-S7`; the prefix parameter also supports later snapshots and
other projects.

Only an explicit feature addition changes the manifest. The new feature is
appended in a new revision effective at the next not-yet-written period. Old
reports keep their original manifest revision and are never rewritten. A
12-hour summary uses the latest feature order and must mention each feature
added during its four-period span.

## Draft and cadence

Drafts are UTF-8 JSON objects with these fields:

```json
{
  "period": {"kind": "3h", "start": "2026-10-18T00:00:00+02:00", "end": "2026-10-18T03:00:00+02:00"},
  "feature_state": [{"id": "terrain", "status": "In progress", "detail": "A short current state."}],
  "expected_progress": ["One expected outcome."],
  "feature_progress": [{"feature_id": "terrain", "lines": ["An observed change."]}],
  "other_progress": [],
  "time_spent": {
    "allocations": [
      {"label": "Implementation", "hours": 1.0, "basis": "measured", "evidence": "Timer record."},
      {"label": "Diagnostics and tests", "hours": 1.0, "basis": "estimated", "evidence": "Dated work notes."},
      {"label": "Unaccounted", "hours": 1.0, "basis": "unaccounted", "evidence": "No reliable activity record."}
    ],
    "parallel_note": "Parallel spans were attributed once to their primary activity."
  },
  "issues": [],
  "conclusions": [],
  "next_goals": ["A concrete next goal."],
  "summary": ["A concise period summary."]
}
```

Every report has ten sections, in this order: Period; State of release;
Expected progress; Feature progress; Other progress; Time spent; Issues;
Conclusions (investigated causes and corrections); Next goals; Summary.
The release-state section has exactly one line per manifest feature, in order,
with rendered status `<feature> - Done`, `<feature> - in progress: <short detail>`,
or `<feature> - planned`. Expected
progress is limited to 16 lines; changed-feature progress is one or two lines
per feature; Other progress is at most 10 lines; Issues and Conclusions are
each at most 30 lines; Next goals are at most feature count plus 16 lines; and
Summary is at most 10 lines. Empty optional sections are shown as “None
recorded.”

Period start/end values must include a timezone. Three-hour reports must be
exactly three hours and contiguous with the preceding report. Time is
partitioned into at most eight nonnegative activity allocations; their hours
must sum to the period. Each allocation identifies the activity, its basis
(`measured`, `estimated`, or `unaccounted`), and evidence. The mandatory chart
shows proportional activity allocations with a legend containing each
activity's hours, period share, and basis; its adjacent table also shows
evidence. Activity labels use an ASCII hyphen separator.
Use `parallel_note` when needed to explain how overlapping work was attributed
once rather than double-counted.

Append the next report with:

```sh
python3 script/ai_release_reports.py --reports-root docs/ai/reports \
  append S7 --draft /path/to/report.json
```

The fourth report in each contiguous group of four is supplied with a separate
12-hour summary draft and is not saved as a standalone 3-hour PDF:

```sh
python3 script/ai_release_reports.py --reports-root docs/ai/reports \
  append S7 --draft /path/to/fourth-report.json \
  --summary-12h /path/to/twelve-hour-summary.json
```

The 12-hour draft uses the same fields, with `period.kind` set to `12h`, a
12-hour interval, and an additional `covered_periods` array containing the
four exact contiguous `{ "start": ..., "end": ... }` intervals. The aggregate
PDF is rendered and read back before the three standalone PDFs it covers are
removed. Validation, rendering, or readback failure preserves those files.
All 12-hour PDFs remain in the managed folder until verified full delivery.
If pruning is interrupted after the aggregate and state are saved, state retains
the exact covered paths and hashes. `check-state` reports that recovery is
needed; run `reconcile RELEASE_ID` to remove only those still-matching files.
Missing sources are treated as already pruned, while changed sources and
unrecognized files fail closed. Reconciliation is safe to retry.

Use `check RELEASE_ID --draft FILE` to validate the next report without saving
it; on every fourth report, also pass `--summary-12h FILE`. `add-feature`
appends an explicitly requested `{ "id": "...", "name": "..." }` feature revision and requires both a
user-request reference and the SHA-256 of the exact request text:

```sh
python3 script/ai_release_reports.py --reports-root docs/ai/reports \
  add-feature S7 --feature /path/to/feature.json \
  --authority-reference 'user request in the active release thread' \
  --authority-text-sha256 "$HASH_OF_EXACT_USER_REQUEST_TEXT"
```

PDF creation requires Python with ReportLab and pypdf; the bundled workspace
Python has these dependencies. Validation and lifecycle-state handling use
the standard library.

The standard-library-only state check is suitable for local `ai_check`
integration. With no release ID it checks every direct managed folder under
the reports root; passing an ID checks only that folder. It validates revision
provenance, period/aggregate coverage, retention layout, and stored PDF hashes
without rendering PDFs or changing state:

```sh
python3 script/ai_release_reports.py --reports-root docs/ai/reports check-state
```

## Full-delivery cleanup

`finish` requires a receipt whose `release_id` matches, whose
`delivery_status` is `complete`, and whose gate IDs exactly match those set at
release start. Each gate must be `passed` and include one or more evidence
objects with a relative path and SHA-256 hash, for example:

```json
{
  "release_id": "S7",
  "delivery_status": "complete",
  "gates": {
    "source": {"status": "passed", "evidence": [{"path": "source.txt", "sha256": "<64 lowercase hex characters>"}]}
  }
}
```

Before deletion, the tool checks each evidence hash, the managed-folder marker,
the retained PDF hashes, and the exact known folder contents. Unknown files,
symlinks, incomplete gates, or mismatched evidence cause refusal without
deletion. On success, only the exact marked release folder is removed; its
PDFs are not archived elsewhere.
