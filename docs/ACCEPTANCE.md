# Runtime acceptance

The desktop executable provides bounded, noninteractive modes for snapshot evidence:

```sh
mc_main --scenario <file> --evidence <json>
mc_main --benchmark-render [--present-immediate] [--hud] --evidence <json>
mc_main --benchmark-game [--present-immediate] [--workload ordinary|speed-200|speed-200-full-radius|wrapped-border|permission-collision] --evidence <json>
mc_main --capture-render --image <ppm> --evidence <json>
```

Scenario mode parses the versioned scripting format, starts a bounded local server, drives clients through authoritative ticks, evaluates assertions, and records structured success or failure. The renderer modes still require a supported Vulkan device and window system. Benchmark mode warms up before sampling; capture mode writes a binary PPM image. Every mode has a watchdog deadline and attempts to persist failure evidence before exiting.

Benchmark evidence records requested/actual framebuffer size, device and driver,
negotiated present mode, validation/HUD state, durations, frame count, and CPU
percentiles. Historical `cpu_presentation_request_timings_ns` covers command
recording plus complete/submit/present; acquire is separate. Preserve these fields,
but do not treat device request rate as portable FPS or a GPU threshold.

`--benchmark-game` runs the production loopback server, networked Flight client,
authoritative simulation, and Vulkan player renderer. The benchmark API selects
ordinary alternating movement, sustained x200 travel, wrapped-border traversal,
or alternating collision-bypass policies. `--workload` selects one script (default
`ordinary`); compare separate receipts. Wrapped traversal and policy publication
count only observed authoritative state changes; compile/publication timing is
separate from server ticks. MC-AI-0247 `AdverseNetwork` tests cover loss, jitter,
freezes, reordering, and duplication; the loopback benchmark does not measure frame
timing under impairment.

The JSON separates 2 s cold, 3 s warm, and 1 s uncapped loops, retaining every
loop/tick duration, present success/failure, traffic counts, resident tiles,
visible faces, and client payload bytes (not transport overhead). GPU duration is
the latest renderer sample and may lag. Rates count render/submit/present requests
completed within each half-open interval; a post-final completion is excluded.
Physical `displayed_cadence_hz`, resident mesh bytes, and power mode are unavailable;
GPU identity may also be null for a generic presentation context. Raw samples and
p50/p95/p99/p99.8/max remain recorded. Success requires a completed request in each
phase, sent inputs, and processed server events—not a device-specific FPS target.

`speed-200-movement-v2` uses full-axis `x=127`; historical v1 used `x=1`, not
full-speed evidence. Production readiness samples 225 current-near tiles,
separating nonresident, pending, active and current-visible meshes. Resident-to-visible
episodes have a four-second diagnostic bound, not an FPS target; departed keys
are censored. Completion requires two center changes, near admissions, no far
admissions while near work is pending, no ordering/classification errors or
deadline misses, and full current-near visibility after a four-second neutral-input
drain excluded from timing/rates.

`speed-200-full-radius-recovery-v1` separates radius256 terrain coverage from
radius128 performance. Both use30 s baseline,30 s x200 travel and30 s recovery
stationary, with normal HUD/scheduling, exact meshes and drained queues.
Warmups stay bounded to180 s. Preregister one performance capture per candidate;
retain failures without pass-seeking retries or cherry-picking. Reject legacy
short performance samples; radius, quality and120Hz/99.8% thresholds stay fixed.
Longer exposure/granularity is not an engine fix or representativeness guarantee.
The v25 short-window recovery failure remains3/1,201 slots,119.699493Hz. Derive
diagnostic10/6/10 s prefixes from complete raw capture under the
[preregistered attribution](ai/GAMEPLAY_QUALIFICATION.md); they cannot replace
full-window qualification.

Snapshot promotion separately requires source-bound local Mac/Windows Release
[gameplay proof](ai/GAMEPLAY_QUALIFICATION.md). Its verifier recomputes raw
coverage, rates and request-cadence proxies; physical scanout remains unmeasured.
Failed captures remain failed and do not replace analyzer or user acceptance.

`--present-immediate` requires immediate negotiation and fails rather than reporting
FIFO. Release validation defaults off. Compare a fixed-scene 1920x1080 pair with and
without `--hud`; evidence records HUD state. CoreGraphics may hide display identity
or refresh mode, which this benchmark does not infer.

Benchmark and visible-capture extents are framebuffer pixels. Before presentation
setup, the shared helper adjusts GLFW logical dimensions until that extent is
observed, bounded by `max_resize_polls`; zero, unrepresentable, stalled, or
two-step-oscillating dimensions fail with evidence.

Scenario source/evidence paths and image/evidence paths must differ. Zero exit means
the operation and assertions passed; it does not replace frame review or prove a
different platform, driver, or package.

S7 adverse-network acceptance runs two production `GameClient`s through a test-only
ENet relay to a production `GameServer`. Independent seeded schedules in both
directions cover latency, jitter, loss, duplication, reordering, bounded queues,
freezes, and recovery bursts. After the impairment window ends at a recorded tick,
normal delivery has bounded time to converge. GoogleTest XML records seed,
per-direction traffic/schedule, server authority, relay stale-input rejection,
direct stale-revision probes on both clients, queue bounds, and convergence.
Separate checks cover a 15-second freeze and reconnect after server replacement.
Production stop/deadline hooks must stop/join during disconnect or pending
`JoinResponse`; cancellation while disconnected has a regression. Every discovered
`mc_tests` case has a 120-second timeout. The relay changes no shipping physics or
render timing.

Capture tests use `--player-client-capture` and four named presets. They await the
full server-selected disk and CPU/renderer mesh coverage, compare keys across 16
headings, then write PPMs. Default radius 256 contains 205,861 tiles; origin spawns
at X/Y (0,0). Portable size is at least 1000x600; set
`MC_S7_REQUIRE_2560X1440=1` for exact Mac assets. Inspect every frame.

The four Mac images must be distinct: (1) third-person flying by the central
spike at server distance 72 (16,241 tiles), showing the whole spike, most of
the first ring and terrain behind it, with the player clear; (2) trench by the
first spike at its existing distance; (3) mountain climb; (4) first-person at
X/Z (0,0). Wait for the full selected disk and meshes. Export 2560x1440, inspect
each, and use a large bottom-center label below the HUD.
