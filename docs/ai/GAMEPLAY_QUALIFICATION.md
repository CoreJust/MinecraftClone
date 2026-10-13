# Full-radius gameplay qualification

`speed-200-full-radius-recovery-v1` adds the release workload: radius 256,
205,861 desired tiles, exact current-handle CPU/renderer coverage and drained
delivery, residency, meshing, worker and removal queues. It waits for this
initial barrier, measures 10 seconds stationary with the normal HUD and frame
scheduler, drives full-axis `x=127` at x200 speed for 6 seconds, waits for the
same exact barrier at the new center, then measures 10 seconds stationary.
Each terrain warmup has a configurable 1–1,800 second diagnostic deadline
(`--full-radius-warmup-seconds`, default 180); qualification requires exactly
180 seconds. Exact coverage at the deadline may succeed; completion strictly
after it fails. Neither outcome is a performance result.
Radial bands retain the complete disk counts. Raw
stationary frames include every loop attempt and failed presentation request;
warmup frames are sampled once per second and stay outside the cadence sample.

Snapshot, minor and major promotion require the `full-radius-gameplay` local
proof in [the check registry](../../script/ai_checks.json), including proof-only
publisher checks. Both Mac and Windows must complete the Release workload.
The verifier recomputes loop and server timing summaries, stationary attempt
totals, actual-duration request rates, and intervals between consecutive
successful request completions from the raw JSON. Altered bytes, missing or
stale source/build/dependency identity, reduced radius, missing barriers,
pending queues, timeout, fatal benchmark errors or inconsistent summaries block release.

Coverage completion and the request-cadence proxy are separate predicates.
Mac uses normal FIFO and a nearest-refresh-slot proxy for its 120 Hz target:
`slots = max(1, round(interval_ns * 120 / 1e9))`. In each stationary window and
the x200 movement window, `sum(slots - 1) / sum(slots)` must be at most 0.002.
The median interval must quantize to one slot and the mean interval must be no
slower than the same derived 99.8% rate floor. Successful request count divided
by actual phase elapsed time must reach `120 * 0.998 = 119.76 Hz`.
These assistant-derived proxy rules distinguish small CPU completion jitter
from a missed refresh slot; they do not measure physical refresh or prove an
exact 120 Hz scanout target.

Windows requests immediate presentation and must actually negotiate it.
At least 99.8% of its successful-request intervals must be at most 2,000,000 ns,
and its actual-phase request rate must reach 500 Hz in all three windows.
To guard against a few long freezes hidden by a count percentile, the total
interval time exceeding 2,000,000 ns divided by total interval time must be at
most 0.002: `sum(max(0, interval_ns - 2000000)) / sum(interval_ns)`.

Every successful-completion interval is half-open,
`[previous_successful_completion, current_successful_completion)`. Attempts
retain the native report's attribution to the phase in which the loop starts,
including its closing transition attempt; reported offsets show these bounds.
Each window must contain at least 500 intervals. Intervals across skipped
attempts stay in the calculation; initial phase work and closing unsuccessful
attempts remain in the actual-phase rate denominator. Long freezes cannot be
discarded as percentile outliers.

A false presentation outcome can be a normal acquire deferral before frame
recording under the production 8 ms budget. It is an unsuccessful attempt,
not evidence of a fatal renderer error. Stationary raw attempts, successful
requests and unsuccessful totals must reconcile exactly; aggregate counters
must include all raw events, while warmup raw sampling prevents full aggregate
recomputation. The native benchmark's failure result still blocks fatal
renderer, validation or device errors.

The cadence thresholds reflect the owner's gameplay targets through request
completions. Physical display cadence remains unavailable; neither `passed: true`
nor this proxy
claims measured scanout, replaces frame inspection, or replaces the user's
separate Windows runtime acceptance. Analyzer qualification remains independent.

Capture fresh evidence after an up-to-date production Release build:

```sh
python3 script/ci/verify_gameplay_gate.py --root . --record macos \
  --executable build/release/mc_main --receipt /absolute/path/macos-gameplay.json
# On Windows, use --record windows and build/release/mc_main.exe.
```

The capture command checks the Release Ninja graph before and after the bounded
run, and binds unchanged relevant source, executable, build configuration,
configured dependency inventory and raw report bytes. It retains the raw report
and a one-platform receipt even when the workload or cadence fails. It does not
retroactively qualify a bare report. Documentation and backlog edits do not
invalidate the native source fingerprint. Keep the recorded executable and raw
report until promotion consumes them. To move Windows evidence to the verifying
host, copy its receipt, raw report and exact `mc_main.exe` bytes together.

```sh
python3 script/ci/verify_gameplay_gate.py --root . \
  --merge /absolute/path/macos-gameplay.json /absolute/path/windows-gameplay.json \
  --receipt build/ai-checks/gameplay-aggregate.json
python3 script/ci/verify_gameplay_gate.py --root . \
  --receipt build/ai-checks/gameplay-aggregate.json --candidate-commit <full-source-SHA>
```

Merge checks the preserved file hashes before resolving transported paths.
Release checks always execute the offline verifier (`proof: true`, `reuse: false`);
an earlier analysis or renderer-smoke pass cannot supply gameplay evidence.
