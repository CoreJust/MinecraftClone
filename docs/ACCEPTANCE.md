# Runtime acceptance

The executable provides bounded evidence modes:

```sh
mc_main --scenario <file> --evidence <json-or-dash>
mc_main --benchmark-render [--present-immediate] [--hud] --evidence <json-or-dash>
mc_main --benchmark-game [--present-immediate] [--workload ordinary|ordinary-turning|speed-200|speed-200-full-radius|wrapped-border|permission-collision|diagnostic-stationary] [--diagnostic-full-radius-capture] [--diagnostic-presentation-stages] [--diagnostic-indexed-stone-quads 0|1|2] --evidence <json-or-dash>
mc_main --player-client-2d --address <IP:PORT> --replay stationary|ordinary-turning --seconds 1..120 --evidence <new-directory>
mc_main --capture-render --image <ppm> --evidence <json-or-dash>
```

Pass `--evidence -` for one frame after progress logs:
`\nMC_RUNTIME_EVIDENCE_V1 <byte-count>\n` plus exactly that many bytes of the
regular-file JSON, including its final newline. Windows emits binary. Reject
missing, duplicate, truncated or invalid frames; retain process exit status.
External compression leaves the workload and measurement window unchanged.

Diagnostic only; no qualification:

```sh
mc_main --benchmark-game --terrain-model v10 --workload speed-200 --benchmark-spawn-z 1600 --present-immediate --evidence <json>
mc_main --benchmark-game --benchmark-frame-capture <fresh.ppm> --evidence <json>
mc_main --benchmark-game --benchmark-frame-capture <fresh.ppm> --benchmark-frame-capture-phase generation --benchmark-frame-capture-delay-ms <0..30000> --evidence <json>
# Capture-site comparison
mc_main --benchmark-game --terrain-model v10 --workload diagnostic-stationary --benchmark-frame-capture <reference.ppm> --benchmark-capture-site <x> <y> <z> <yaw> <pitch> --evidence <reference.json>
mc_main --benchmark-game --terrain-model native-v10-fine --workload diagnostic-stationary --benchmark-frame-capture <fresh.ppm> --benchmark-capture-site <x> <y> <z> <yaw> <pitch> --evidence <json>
```

Scenarios assert local authority. Renderer modes require Vulkan/windows; watchdogs preserve failures.

Evidence records framebuffer size, device/driver, present mode, validation/HUD,
durations, frame count and CPU percentiles. Historical
`cpu_presentation_request_timings_ns` covers recording through present; acquire is
separate. Request rate is neither portable FPS nor a GPU threshold.

`--benchmark-game` exercises the production loopback server, Flight client,
simulation and Vulkan renderer. Workloads include ordinary, x30 turning, x200,
wrapped-border and permission/collision churn; compare separate receipts. Rates
require authoritative state changes. MC-AI-0247 `AdverseNetwork` tests cover
impairment, but this loopback benchmark does not measure frame timing under it.

JSON retains raw loop/tick durations, presents, traffic, resident tiles, visible faces
and payload bytes. GPU duration may lag; scanout, resident mesh bytes and power mode
are unavailable.

`diagnostic_client_after_resize_*_duration_ns` separates data/HUD, map/block,
terrain LOD and post-LOD CPU stages from renderer calls. Percentiles are not additive.

V10-fine x30 diagnostics record start, authority and disk coverage. Native x200
records fenced readiness and recovery around 30 seconds of travel.

```sh
python3 script/ci/verify_gameplay_gate.py --inspect-s2-turning macos --receipt <json>
python3 script/ci/verify_gameplay_gate.py --inspect-s2-turning windows --receipt <json>
mc_main --benchmark-game --terrain-model native-v10-fine --workload speed-200-full-radius --present-immediate --evidence <json>
python3 script/ci/verify_gameplay_gate.py --inspect-s2-recovery windows --receipt <json>
```

Inspector output is diagnostic only.

Both gameplay proofs use render distance 256. `terrain-coverage` remains x200
recovery; `performance` uses `turning-x30-movement-v2`: a 30-second stationary
baseline, 30 seconds of accelerated turning at speedup 30, recovery, then 30
seconds stationary. S1 uses `mc.s1-lod-performance.v2`: each movement loop, authoritative update and
trajectory anchor must certify horizontal full-volume 640 coverage. Require 6,561
resident/required XY columns, 64 vertical chunks and 419,904 chunk equivalents,
positive session coverage epoch, and the same current required XY chunk center.
Announcements may lag, but measure the current required set. Also require active
visual 320 LOD. Dense 48/80 caches cannot prove 640 coverage. Generic 320 uses
exact `hasAuthoritativeChunk` geometry. Historical 40/72 receipts used invalid units.
Loading deadlines start at request acceptance, including world preparation and startup.
At least 99% of
the moving window must meet the fresh-frame target: 8,333,333 ns on Mac (120 Hz)
and 3,333,333 ns on Windows (300 Hz), measured from distinct submissions whose
per-slot GPU fences later succeed. Mac driver timing and Windows request cadence
remain separate platform checks. The benchmark injects controlled input through
the production start gate, but bypasses live GLFW and mouse-look. Retain failures;
10/6/10-second prefixes are diagnostic only and never replace qualification.

`speed-200-movement-v2` uses full-axis `x=127`; historical v1 used `x=1` and is
not full-speed evidence. Readiness tracks 225 current-near tiles and separates
nonresident, pending, active and visible meshes. Completion requires two center
changes, near admissions, no far admissions while near work is pending, no
classification/deadline errors, and full visibility after a four-second neutral
drain excluded from timing.

`speed-200-full-radius-recovery-v1` uses normal HUD and scheduling, with a
30-second baseline, x200 travel and recovered window. Require exact meshes and
drained queues before and after movement; each qualification warmup is bounded to
180 seconds. Diagnostic radii span 1–256.

The 2D column-map replay records logical CPU compositions, not GPU frames or FPS:

```sh
mc_main --player-client-2d --address 127.0.0.1:8765 --replay ordinary-turning \
  --seconds 30 --evidence /absolute/path/new-evidence
```

New output directories need an existing parent. Replay includes loading
from request acceptance, production server/start gate, and stationary or synthetic
x30 input. Snapshots retain pixel/material/coverage/height/update masks and
terrain/position identity; they claim no GPU FPS and bypass live input.

For diagnosis, `--diagnostic-indexed-stone-quads 0|1|2` selects a renderer path;
`--diagnostic-presentation-stages` records attempted submit, acquire, present and
swapchain waits without changing synchronization. Recorded diagnostic overrides
cannot qualify gameplay.

Snapshot promotion requires source-bound local Mac and Windows Release
[gameplay proofs](ai/GAMEPLAY_QUALIFICATION.md). The verifier recomputes raw
coverage, rates, fence-completion intervals and platform presentation checks;
physical scanout remains unmeasured. Failed captures remain failed and do not
replace analyzer or user acceptance.

`--present-immediate` requires immediate negotiation and fails rather than reporting
FIFO. Release validation defaults off. Compare a fixed-scene 1920x1080 pair with and
without `--hud`; evidence records HUD state. CoreGraphics may hide display identity
or refresh mode, which this benchmark does not infer.

Benchmark and visible-capture extents are framebuffer pixels. Before presentation
setup, the shared helper adjusts GLFW logical dimensions until that extent is
observed, bounded by `max_resize_polls`; zero, unrepresentable, stalled, or
two-step-oscillating dimensions fail with evidence.

Keep scenario/source and image/evidence paths separate. Zero exit confirms assertions,
not visual review or proof for another platform, driver or package.

S7 adverse-network tests run two production clients through a test-only ENet relay
to a production server. Seeded schedules cover latency, jitter, loss, duplication,
reordering, bounded queues, freezes and recovery. GoogleTest XML records traffic,
server authority, stale-input rejection, queue bounds and convergence; separate
cases cover a 15-second freeze and reconnect after server replacement. Disconnect
and pending-join cancellation must stop and join production hooks. Each `mc_tests`
case has a 120-second timeout; the relay changes no shipping behavior.

Four `--player-client-capture` presets use server `--spawn-surface X Y` at preset
coordinates for safe canonical Z. Before sweeping 16 headings, require a session-
matched native LOD partition of the 65,536² world, all 6,889 columns in the
configured player-centered 83×83 capture window, uploaded faces and a fence-
completed frame. This is a configured test condition, not a general runtime
residency quota. Keep scene/columns stable; write PPMs and retain ≥1000×600,
optional exact 2560×1440, terrain, color, HUD and title pixel checks. Origin starts
above prepared canonical terrain at (0,0). Inspect every image.
