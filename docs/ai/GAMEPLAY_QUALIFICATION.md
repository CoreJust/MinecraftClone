# Full-radius gameplay qualification

S1 `performance` uses render distance 256, `turning-x30-movement-v2`: 30 s baseline,
30 s accelerated turning at speed 30, recovery, then 30 s stationary. First successful
movement send starts forward, forward-looking-left, strafe-looking-back and
backward-turning-right phases. Yaw/pitch/ascent/descent use production paths.
`terrain-coverage` remains x200 recovery at radius 256; both require exact
CPU/renderer barriers and ≤180 s warmups. Recovery must be nearer the movement
destination than the start by wrapped Chebyshev distance.

The v1 replay's ±1 axes invalidated its x30 receipts. The replacement records
keyboard intent, full-scale production directions, rendered camera poses and
authoritative positions under `mc.x30-authoritative-trajectory.v1`. The verifier
recomputes every integer 3D Flight step, XY wrapping and Z bounds at speed 30.
It requires contiguous scheduled 100 ms deadlines, sends within their serviced
tick, and matching ACKs within one server tick. The ACK tail may extend one tick
beyond movement; the frame window stays fixed. Missing ticks, slow motion,
unchanged view, missing vertical motion or incomplete ACKs fail. See the
[owner contract](decisions/earlydev_0_1_1_s1_performance.md#representative-movement-and-actual-speed).

First public full Ready requires once-primed exact 768 reserve, then current 640;
counters remain 640. S1 uses `mc.s1-lod-performance.v2`: each movement loop, authoritative update and
trajectory anchor must certify horizontal full-volume 640 coverage. Require 6,561
resident/required XY columns, 64 vertical chunks and 419,904 chunk equivalents,
positive session coverage epoch, and the same current required XY chunk center.
Announcement lag is allowed; the older announced center cannot substitute for
the current required set. Separate active nonempty visual 320 LOD evidence is
required. Dense 48 start/dense 80 cache do not prove full 640. Generic 320 benchmarks
retain exact geometric chunk sets queried through `hasAuthoritativeChunk`;
never reinterpret their counters as S1 profile counts. Prior 40/72 readiness
receipts preserve the invalid unit interpretation and are not current passes.
Before the first x30 send, build a fresh anchor from matching authoritative
position and the current full-profile/visual sample; prior readiness alone is
insufficient. Loading deadlines retain
the original request-accepted T0 through world preparation and client startup.
At least 99% of the fixed moving window must meet the fresh-frame target: 120 Hz
on Mac (8,333,333 ns) and 300 Hz on Windows (3,333,333 ns), from distinct frame
submissions whose per-slot GPU fences later succeed. The harness uses synthetic
input through the production start gate; it bypasses live GLFW and mouse-look.

Historical S7 secondary/exception flags report deferred or
[accepted exception](S7-CADENCE-EXCEPTION-2026-10-18.md), never performance PASS.
The active registry waives neither platform.

Preregister one capture per candidate; retain failures and never retry to
cherry-pick. Keep the historical v25 normal 10/6/10-second recovery failure
(3/1,201 lost slots at 119.699493 Hz) visible in comparisons.

Diagnostic prefixes retain raw timing/generation attribution, never qualification.

Require four Mac/Windows Release receipts for both proofs, one candidate with
matching build/dependency identities; no S7 waivers/reuse. Native `--gameplay-proof`
fixes workload/radius under `mc.gameplay-proof.v2`. Reject stale, altered,
incomplete, timed-out or inconsistent evidence. Recompute loops, server/stationary
summaries, rates, fence intervals and coupled movement coverage.

Fresh-frame and platform-presentation gates remain independent. Mac requires normal FIFO and
`VK_GOOGLE_display_timing`; Windows requires immediate mode. Neither replaces the
fence-confirmed 120/300 Hz thresholds or proves physical scanout. Mac timing binds
request IDs, loop/phase, timestamps and ordered poll results through a bounded
drain to a frozen endpoint. Lock generation at the first baseline request; never
bridge generations or invent timestamps.
Unsupported timing, generation changes, query errors or unresolved endpoint
timestamps fail. Require 500 driver intervals per Mac performance phase.
Mac FIFO timing requires at least 119.76 Hz, a one-refresh median and at most
0.2% lost slots. Windows separately checks 120 Hz request cadence with its 99.8%
interval rules.

Every measurement-generation success through the endpoint, including warmup,
needs one positive increasing timestamp. Drain IDs are declared and ordered; not
all must arrive.

Acquire deferral under the production deadline counts as an unsuccessful attempt;
raw attempt/success/failure totals must reconcile. Renderer, validation or device
errors fail. Frame inspection and analyzer qualification remain required.

Production Release commands:

```sh
python3 script/ci/verify_gameplay_gate.py --root . --record macos --proof terrain-coverage \
  --executable build/release/mc_main --receipt /absolute/path/macos-terrain.json \
  --macos-package /absolute/path/normal-package \
  --macos-package-manifest /absolute/path/normal-package/manifest.json
```

Use `script/package_snapshot.py`; verify inventory, source executable, launcher,
relative ICD and Mach-O closure. Reject inherited `DYLD_*`/`VK_*`; bind loader
hashes/invocation. Retain `.macos`, launch normally and recheck closures.
Bare source captures fail; package hashes do not prove runtime library mapping.

With separate Windows build/desktop accounts, the recorder sends only the
runtime package to the desktop helper:

```powershell
python script/ci/verify_gameplay_gate.py --root C:\task\source --record windows `
  --executable C:\task\build\mc_main.exe --receipt C:\task\proof\windows-gameplay.json `
  --desktop-package C:\task\runtime --desktop-package-manifest C:\task\runtime-manifest.json `
  --desktop-task-id <task-id> --proof terrain-coverage
```

The UTF-8/BOM manifest binds executable/DLL/SPIR-V/assets by relative path,
size and hash; reject unlisted, duplicate, escaping or reparse inputs. Keep source
private; the broker binds task, arguments, hashes, timeout and process identity.

Reject build-account `MC_DIAGNOSTIC_*`; require the native selector and false
`diagnostic_overrides_present` marker. Recognized diagnostic overrides invalidate proof
even at `0`; registry values are not proof. Diagnostic options/evidence:
[runtime acceptance](../ACCEPTANCE.md) and [rendering guide](../code/RENDERING.md).
Diagnostic captures cannot qualify.

Native timeout is two warmups plus 120 seconds (480 seconds with the 180-second
default); Windows polling adds 60 seconds for startup. Polling never resubmits;
cancellation waits up to 30 seconds. Retain failed receipts, `.raw.json` and
`.desktop` runtime/helper evidence. Cleanup failure or nonzero exit fails.

Hash source/scenarios, Release graph, executable, configuration, dependencies and
raw bytes before/after; retain failures, sidecars and promotion artifacts.

```sh
python3 script/ci/verify_gameplay_gate.py --root . \
  --merge /absolute/path/macos-terrain.json /absolute/path/macos-performance.json \
          /absolute/path/windows-terrain.json /absolute/path/windows-performance.json \
  --receipt build/ai-checks/gameplay-aggregate.json
python3 script/ci/verify_gameplay_gate.py --root . \
  --receipt build/ai-checks/gameplay-aggregate.json --candidate-commit <full-source-SHA>
```

Merge hashes before resolving paths; verify offline (`proof: true`, `reuse: false`).
Smoke/analysis never replaces gameplay evidence.
