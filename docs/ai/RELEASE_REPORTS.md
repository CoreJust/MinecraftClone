# Release reports

The release owner keeps evidence-backed reports on the release timeline, from
the implementation-start anchor through verified delivery. The
[report tool](../../script/ai_release_reports.py) validates and renders PDFs,
stores per-release state, and controls pruning and cleanup. This page summarizes
the owner contract; the tool defines the complete schema and validation rules.
See the [release checklist](RELEASES.md) for version-closure gates.

## Start and cadence

Start a new managed release with an ordered feature manifest, exact required
gate IDs, and a timezone-aware implementation-start anchor. The first report
starts at that anchor; it is immutable. New releases use minor periods of at
most four hours and positive duration. Periods may close early, but each next
period starts at the previous actual end, without rounding or gaps. Three
contiguous minors form a major report spanning at most twelve hours. Filenames
bind each new period's exact start timestamp and timezone offset.

Managed state without a `reporting` cadence marker remains on its historical
profile: exact three-hour periods, four-report groups, and the original `3h/`
and `12h/` folders, hashes, and cleanup behavior. Four periods form its
twelve-hour summary. Do not migrate that state or rewrite its PDFs.

Only an explicit feature addition changes the manifest. Append it as a new
revision effective with the next unwritten period; earlier reports keep their
original revision. A major summary names every feature added during its
covered minors.

## Minor reports

Every report has these ten sections in order: Period; State of release;
Expected progress; Feature progress; Other progress; Time spent; Issues;
Conclusions (investigated causes and corrections); Next goals; Summary. A draft
may include an `overview` list of up to six plain-language lines. The PDF shows
those lines at the start of State of release, before the feature statuses; when
the field is absent, the existing report layout is unchanged. State contains
exactly one feature line per manifest entry, in manifest order. Lines use
`<name> [<ID>] - Done`, `<name> [<ID>] - in progress: <detail>`, or
`<name> [<ID>] - planned`. The PDF orders features by Done, In progress, then
Planned, preserving manifest order within each status and showing each ID and
name; status words are bold and colored.

Limits are: 16 expected-progress lines; one or two lines per changed feature;
10 other-progress lines; 30 each for issues and conclusions; feature count
plus 16 next-goal lines; and 10 summary lines. Empty optional sections say
“None recorded.” Time uses at most eight nonnegative allocations whose hours
sum to the actual report duration. Each records an activity, measured,
estimated, or unaccounted basis, and evidence. Explain overlapping work when
needed so it is counted once. The chart shows proportional hours and shares;
its legend lists each activity's hours, period share, and basis, and the
adjacent table gives evidence. Use an ASCII hyphen in activity labels and
`parallel_note` to explain overlapping work counted once.

## Major reports and evidence

The third contiguous four-hour-profile minor closes a major report. Its
`covered_periods` lists the three actual intervals in order; they must be
contiguous and match the summary's exact start and end. The total may be shorter
than twelve hours, and allocations sum to that actual span. The summary also
requires 1–30 product results, up to eight PNG/JPEG screenshots, 1–30
metric/value/workload/evidence rows, and 1–30 analysis lines. Measurements
include units and workload. Screenshot paths are relative to the working
directory, cannot traverse directories, and are embedded with their hashes.
Use an empty screenshot list when none is available.

Use `check` to validate the next draft; on the third new-profile minor, also
check its twelve-hour summary. `check-state` inspects managed state without
rendering or changing it; without a release ID, it checks every managed folder.
`add-feature` requires an authority reference and
SHA-256 of the exact request text. PDF rendering and readback must succeed
before source PDFs can be pruned; any validation, rendering, or readback failure
preserves them. After saving an aggregate, state records covered source paths
and hashes so interrupted pruning can resume safely.

`finish` accepts only a complete receipt matching the release ID and exact
gate set, with each gate passed and backed by evidence paths and SHA-256
hashes. Before deletion, it checks evidence, the managed-folder marker, retained
PDF hashes, and exact folder contents. Unknown files, symlinks, or mismatches
refuse cleanup. All major PDFs remain until verified full delivery. Success
removes only the marked release folder; PDFs are not archived elsewhere. A
missed deadline, failed gate, or partial publication does not reset the period
or authorize cleanup.

PDF creation uses ReportLab and pypdf, plus Pillow when validating screenshots;
the bundled workspace Python provides them.
