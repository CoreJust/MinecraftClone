# Full-radius gameplay qualification

`speed-200-full-radius-recovery-v1` checks exact CPU/renderer terrain coverage
and empty delivery, residency, mesh, worker and removal queues. The
`terrain-coverage` proof requires radius 256 (205,861 tiles); `performance`
requires radius 128. Each uses complete-terrain barriers, 30 s stationary, 30 s
of full-axis `x=127` travel at x200, recovery closer to the movement end than
start by wrapped Chebyshev distance, then 30 s stationary. Returning to the
origin fails. Release warmups are exactly 180 s; 1–1,800 s overrides are
diagnostic only. Movement and recovery require all 225 current radius-7 meshes
and complete raw radial bands. Desired band counts must match the verified
radius, not merely agree with underreported resident/visible counters.

Preregister one performance capture per candidate; retain failures, with no
pass-seeking retry or cherry-picked substitute. Performance verification rejects
legacy short windows and accepts only 30–31 s per measured phase. Radius128,
production quality and120Hz/99.8% thresholds are unchanged. Longer exposure and
finer percentage granularity do not guarantee representativeness or fix engines.
The valid v25 short proof remains FAIL: recovery lost3/1,201 slots at119.699493Hz.

Report original10/6/10 s prefixes diagnostically from the full raw capture.
Start each at its first raw phase attempt; include a whole display interval when
its first subsequent attempt starts inside that prefix. Retain concentrated
early losses. This is a later capture-report obligation, not a new output field
or a replacement for full-window qualification.

The shared verifier requires both local Mac and Windows Release proofs. Capture
one proof per invocation: native `--gameplay-proof` fixes the radius and records
`qualification_proof`; schema `mc.gameplay-proof.v2` binds rows. Merge all four
receipts from one candidate with identical per-platform build/dependency
identities. Missing rows cannot become passes. Parent snapshot policy owns
authorized platform deferrals and final execution, not this implementation
contract. Legacy, stale, altered, incomplete, timed-out or inconsistent evidence
fails. Verification recomputes raw loops, server and stationary summaries, rates
and intervals; each proof needs complete measured phases and at least two
successful completions per phase with finite positive intervals.

Coverage cadence is diagnostic; 500 FPS remains later work. Mac performance
uses normal FIFO and driver-reported `VK_GOOGLE_display_timing`, not CPU
completion spacing. Require 120 Hz with ≥99.8% stability in each window:
missed-slot ratio ≤0.002, median one refresh slot and mean rate ≥119.76 Hz.
Actual-timing evidence binds request IDs, loop/phase, timestamps, ordered poll
results and a bounded drain through a frozen endpoint. Lock the presentation
generation at baseline; never bridge generations or invent timestamps.

Requests and raw polls survive initial warmup generations and are keyed by
generation/present ID. Older warmup generations may have incomplete histories;
every successful request in the measurement generation through the frozen
endpoint, including its warmup predecessors, needs exactly one positive,
strictly increasing driver timestamp. Post-endpoint drain IDs are declared;
returned timestamps are order-checked, but need not all arrive. Measured IDs
stay dense. Late generation changes, unavailable timing, query errors and
unresolved endpoint timestamps fail in every phase. Evidence does
not prove physical scanout. Require ≥500 measured intervals per performance
phase, attributed to the first subsequent raw attempt, including skipped,
startup and closing attempts. Monotonic loop ordinals expose missing attempts.

Windows performance must negotiate immediate presentation. Each window needs
120 Hz and ≥99.8% of successful intervals ≤8,333,334 ns, with interval-time
overage share ≤0.002. Its performance proof also needs ≥99.8% of measured
baseline/movement/recovered loop time within that budget. Mac loop budget and
CPU rates remain diagnostic. Acquire deferral under the production 8 ms budget
counts as an unsuccessful attempt, not a fatal error; raw attempt/success/failure
totals must reconcile. Renderer, validation and device errors fail. Neither
proof replaces frame inspection, analyzer qualification or separate runtime
acceptance.

Production Release capture:

```sh
python3 script/ci/verify_gameplay_gate.py --root . --record macos --proof terrain-coverage \
  --executable build/release/mc_main --receipt /absolute/path/macos-terrain.json \
  --macos-package /absolute/path/normal-package \
  --macos-package-manifest /absolute/path/normal-package/manifest.json
# Repeat with --proof performance and a fresh receipt, then capture both Windows proofs.
```

Use `script/package_snapshot.py`; verify manifest/inventory, source Release
executable, `MinecraftClone.command` bytes, relative ICD and Mach-O closure.
Reject inherited `DYLD_*`/`VK_*`; bind receipts to loader/MoltenVK hashes and
invocation. Retain `.macos` beside the receipt and in transit; launch its normal
command and recheck closures. Bare source captures fail. Package hashes do not
prove library mapping; PID maps are unobserved.

With separate Windows build/desktop accounts, send only the runtime package:

```powershell
python script/ci/verify_gameplay_gate.py --root C:\task\source --record windows `
  --executable C:\task\build\mc_main.exe --receipt C:\task\proof\windows-terrain.json `
  --desktop-package C:\task\runtime --desktop-package-manifest C:\task\runtime-manifest.json `
  --desktop-task-id mc-s7-0465-terrain --proof terrain-coverage
```

The UTF-8/BOM manifest lists relative paths, sizes and SHA-256 for executable,
DLLs, SPIR-V and assets; reject unlisted, duplicate, escaping or reparse inputs.
Keep source/dependencies private. Each submission adds a fresh bounded UUID
correlation to the caller label in the existing TaskId. Broker checks bind that
invocation, arguments, hashes, timeout and process identity; recheck runtime
inputs around polling. Keep build/launch paths separate and reports
helper-relative. Reject build-account `MC_DIAGNOSTIC_*`; require the native
selector and false `diagnostic_overrides_present` marker. Recognized overrides
invalidate proof even at `0`; registry values are not evidence.

Native timeout is 480 s; Windows polling retains its separate 60 s allowance.
Poll uncertainty never
resubmits; exceptions cancel/wait only the owned UUID up to 30 s. A lost response
recovers only one exactly correlated request. The helper owns the direct
process; this benchmark uses threads. Retain failed receipts, `.raw.json` and
sibling `.desktop` runtime/manifest/helper evidence. Cleanup failure, stale
reports or nonzero exit fail; bare reports lack provenance.

Hash production/tests/embedded `scenarios/**`, Release graph, executable,
configuration, dependencies and raw bytes before/after with stable globs.
Retain failures, raw files and sidecars/relocation. Docs/backlog do not change
native source identity; keep executable/report through promotion.

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
gameplay evidence. Diagnostic options: [runtime acceptance](../ACCEPTANCE.md)
and [rendering guide](../code/RENDERING.md). Diagnostic captures cannot qualify.
