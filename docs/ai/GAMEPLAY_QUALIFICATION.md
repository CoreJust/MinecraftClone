# Full-radius gameplay qualification

`speed-200-full-radius-recovery-v1` requires exact CPU/renderer coverage and
empty delivery/residency/mesh/worker/removal queues. `terrain-coverage` fixes
radius256 (205,861 tiles); `performance` fixes128. Both use30s baseline,30s
full-axis `x=127` travel at x200, complete recovery closer to the movement end
than start by wrapped Chebyshev distance, then30s stationary. Returning to the
origin fails. Qualification warmups are180s;1–1,800s overrides are diagnostic.
Count every attempt; movement/recovery require all225 center-radius7 meshes
and complete raw radial bands. Reject short performance windows; quality and
120Hz/99.8% thresholds stay fixed. Longer exposure is not an engine fix or
representativeness guarantee.

The [S7-only Mac exception](S7-CADENCE-EXCEPTION-2026-10-18.md) reports
`accepted_exception`, never performance PASS; defaults and other gates stay strict.

Preregister one performance capture per candidate; retain failures, never retry
to cherry-pick. The v25 normal10/6/10s recovery failure remains3/1,201 lost slots
at119.699493Hz and must remain visible in comparisons.

Derive diagnostic10/6/10s prefixes from the full raw capture: select raw attempts
starting before each phase's first attempt plus its cutoff. Keep whole intervals
under unchanged generation/ID/first-subsequent-attempt attribution; recompute
attempt/success/failure, loop, request-rate and cadence summaries. Never truncate
or replace qualification windows with prefixes.

The default verifier requires both local Mac and Windows Release proofs at all
levels; minor/major keep this full-platform policy. Only the Snapshot 7 registry
selects `--s7-windows-secondary`: both Mac proofs remain mandatory, while
Windows is reported `deferred`, never passed. That profile rejects Windows
rows and Windows-labelled release assets; it does not reuse or relabel old
receipts. Capture one proof per invocation: native `--gameplay-proof` fixes the
radius and records `qualification_proof`; schema `mc.gameplay-proof.v2` binds
rows. Merge four receipts for the full profile or two Mac receipts for selected
S7, all from one candidate and identical per-platform build/dependency
identities. Legacy, stale, altered, incomplete, timed-out or inconsistent
evidence fails. Verification recomputes raw loops, server and stationary
summaries, rates and intervals; each Mac proof needs complete measured phases,
at least two successful completions per phase and finite positive intervals.

Coverage cadence is diagnostic; 500 FPS is later work. Mac performance uses
normal FIFO and driver-reported `VK_GOOGLE_display_timing`, not CPU completion
spacing. Require 120 Hz with ≥99.8% stability in each window: missed-slot ratio
≤0.002, median one refresh slot and mean rate ≥119.76 Hz. Actual-timing evidence
binds request IDs, loop/phase, timestamps, ordered poll results and a bounded
drain through a frozen endpoint. Lock the presentation generation at the first
baseline request; never bridge generations or invent timestamps. Unsupported or
incomplete timing, late generation changes, query errors and unresolved endpoint
timestamps fail; evidence does not prove physical scanout. Require ≥500 measured
intervals per performance phase, attributed to the first subsequent raw
attempt, including skipped, startup and closing attempts. Mac uses adjacent
actual-present timestamps; monotonic loop ordinals expose gaps.

Every measurement-generation successful request through that endpoint, including
warmup predecessors, needs exactly one positive increasing timestamp.
Post-endpoint drain IDs are declared; returned timestamps are order-checked,
but need not all arrive.

When full-platform Windows qualification is selected, immediate presentation
must be negotiated; each window needs 120 Hz and ≥99.8% of successful intervals
≤8,333,334 ns, with interval-time overage share ≤0.002. Its performance proof
also needs ≥99.8% of measured baseline/movement/recovered loop time within that
8,333,334 ns budget. Mac loop budget and CPU rates remain diagnostic. Acquire
deferral under the production acquisition deadline counts as an unsuccessful attempt,
not a fatal error; raw attempt/success/failure totals must reconcile. Renderer,
validation or device errors fail.

Neither proof replaces frame inspection or analyzer qualification. The S7
Windows-deferred record makes no Windows runtime claim; the later full-platform
profile still requires separate Windows runtime acceptance.

Production Release commands:

```sh
python3 script/ci/verify_gameplay_gate.py --root . --record macos --proof terrain-coverage \
  --executable build/release/mc_main --receipt /absolute/path/macos-terrain.json \
  --macos-package /absolute/path/normal-package \
  --macos-package-manifest /absolute/path/normal-package/manifest.json
# For the Snapshot 7 selection, capture both Mac proofs and merge them with:
# python3 script/ci/verify_gameplay_gate.py --root . --receipt /absolute/path/gameplay-aggregate.json \
#   --s7-windows-secondary --merge /absolute/path/macos-terrain.json /absolute/path/macos-performance.json
# Full-platform minor/major qualification also captures both Windows proofs and merges all four receipts.
```

Use `script/package_snapshot.py`; verify manifest/inventory, source Release
executable, `MinecraftClone.command` bytes, relative ICD and Mach-O closure.
Reject inherited `DYLD_*`/`VK_*`; bind receipts to loader/MoltenVK hashes and
invocation. Retain `.macos` beside the receipt and in transit; launch its normal
command and recheck closures. Bare source captures fail. Package hashes do not
prove library mapping; PID maps are unobserved.

With separate Windows build/desktop accounts, the recorder sends only the
runtime package to the desktop helper:

```powershell
python script/ci/verify_gameplay_gate.py --root C:\task\source --record windows `
  --executable C:\task\build\mc_main.exe --receipt C:\task\proof\windows-gameplay.json `
  --desktop-package C:\task\runtime --desktop-package-manifest C:\task\runtime-manifest.json `
  --desktop-task-id mc-s7-0465-terrain --proof terrain-coverage
```

The UTF-8/BOM manifest lists relative paths, sizes and SHA-256 for executable,
DLLs, SPIR-V and assets; reject unlisted, duplicate, escaping or reparse inputs.
Keep source/dependencies private. Broker binds task, arguments, hashes, timeout
and process identity; recheck runtime inputs around polling. Keep build/launch
paths separate and reports helper-relative.

Reject build-account `MC_DIAGNOSTIC_*`; require the native selector and false
`diagnostic_overrides_present` marker. Recognized diagnostic overrides invalidate proof
even at `0`; registry values are not proof. Diagnostic options/evidence:
[runtime acceptance](../ACCEPTANCE.md) and [rendering guide](../code/RENDERING.md).
Diagnostic captures cannot qualify.

Native process timeout is two warmups plus 120 s for the fixed sample and
loop-boundary allowance (480 s with the 180 s default warmups); the Windows
desktop polling deadline adds a separate 60 s startup allowance. Poll uncertainty never
resubmits; exceptions cancel/wait the owned UUID up to 30 s; lost responses
recover one request. The helper owns only the direct process; this benchmark
uses threads. Retain failed receipts, `.raw.json` and sibling `.desktop`
runtime/manifest/helper evidence. Cleanup failure, stale reports or nonzero
exit fail; bare reports lack provenance.

Hash production/tests/embedded `scenarios/**`, Release graph, executable,
configuration, dependencies and raw bytes before/after with stable globs.
Retain failures, raw files and sidecars/relocation; refused publication keeps
raw bytes. Docs/backlog do not change source identity; keep executable/report
through promotion.

```sh
python3 script/ci/verify_gameplay_gate.py --root . \
  --merge /absolute/path/macos-terrain.json /absolute/path/macos-performance.json \
          /absolute/path/windows-terrain.json /absolute/path/windows-performance.json \
  --receipt build/ai-checks/gameplay-aggregate.json
python3 script/ci/verify_gameplay_gate.py --root . \
  --receipt build/ai-checks/gameplay-aggregate.json --candidate-commit <full-source-SHA>
```

Merge hashes before path resolution; release verifies offline
(`proof: true`, `reuse: false`). Analysis or renderer smoke cannot replace
gameplay evidence.
