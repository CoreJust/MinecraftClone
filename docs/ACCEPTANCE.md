# Runtime acceptance

The executable provides bounded evidence modes:

```sh
mc_main --scenario <file> --evidence <json>
mc_main --benchmark-render [--present-immediate] [--hud] --evidence <json>
mc_main --benchmark-game [--present-immediate] [--workload ordinary|ordinary-turning|speed-200|speed-200-full-radius|wrapped-border|permission-collision] [--diagnostic-full-radius-capture] [--diagnostic-presentation-stages] [--diagnostic-indexed-stone-quads 0|1|2] --evidence <json>
mc_main --player-client-2d --address <IP:PORT> --replay stationary|ordinary-turning --seconds 1..120 --evidence <new-directory>
mc_main --capture-render --image <ppm> --evidence <json>
```

Scenario mode drives a bounded local server and authoritative assertions. Renderer modes require Vulkan/windows; watchdogs bound execution and preserve failures.

Evidence records framebuffer size, device/driver, present mode, validation/HUD,
durations, frame count and CPU percentiles. Historical
`cpu_presentation_request_timings_ns` covers recording through present; acquire is
separate. Request rate is neither portable FPS nor a GPU threshold.

`--benchmark-game` exercises the production loopback server, Flight client,
simulation and Vulkan renderer. Workloads include ordinary, x30 turning, x200,
wrapped-border and permission/collision churn; compare separate receipts. Rates
require authoritative state changes. MC-AI-0247 `AdverseNetwork` tests cover
impairment, but this loopback benchmark does not measure frame timing under it.

The JSON retains raw loop and tick durations, present outcomes, traffic, resident
tiles, visible faces and client payload bytes. GPU duration is the latest renderer
sample and may lag; request rate is not GPU FPS. Physical scanout, resident mesh
bytes and power mode are unavailable. Raw samples and percentiles remain available.

Both gameplay proofs use render distance 256. `terrain-coverage` remains x200
recovery; `performance` uses `turning-x30-movement-v2`: a 30-second stationary
baseline, 30 seconds of accelerated turning at speedup 30, recovery, then 30
seconds stationary. S1 uses `mc.s1-lod-performance.v2`: each movement loop, authoritative update and
trajectory anchor must certify horizontal full-volume 640 coverage. Require 6,561
resident/required XY columns, 64 vertical chunks and 419,904 chunk equivalents,
positive session coverage epoch, and the same current required XY chunk center.
Announcement lag is allowed; the older announced center cannot substitute for
the current required set. Separate active nonempty visual 320 LOD evidence is
required. Dense 48 start/dense 80 cache do not prove full 640. Generic 320 benchmarks
retain exact geometric chunk sets queried through `hasAuthoritativeChunk`;
never reinterpret their counters as S1 profile counts. Prior 40/72 readiness
receipts preserve the invalid unit interpretation and are not current passes.
Loading deadlines
keep the original request-accepted T0 through world preparation and client startup.
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

Use a new output directory with an existing parent. Replay includes loading
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

Capture tests use `--player-client-capture` and four presets. They await the
server-selected disk and CPU/renderer mesh coverage, compare keys across 16
headings, then write PPMs. Radius 256 contains 205,861 tiles; origin spawns at
(0,0). Portable output is at least 1000x600; `MC_S7_REQUIRE_2560X1440=1` selects
exact Mac dimensions. Inspect every frame.

The four Mac images show central-spike flight at distance 72 (16,241 tiles),
the adjacent trench, a mountain climb, and first-person origin. Wait for each
selected disk/mesh set; inspect 2560×1440 output with a bottom-center label.
