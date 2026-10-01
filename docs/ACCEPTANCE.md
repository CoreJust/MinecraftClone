# Runtime acceptance

The desktop executable provides bounded, noninteractive modes for snapshot evidence:

```sh
mc_main --scenario <file> --evidence <json>
mc_main --benchmark-render [--present-immediate] [--hud] --evidence <json>
mc_main --benchmark-game [--present-immediate] [--workload ordinary|speed-200|wrapped-border|permission-collision] --evidence <json>
mc_main --capture-render --image <ppm> --evidence <json>
```

Scenario mode parses the versioned scripting format, starts a bounded local server, drives clients through authoritative ticks, evaluates assertions, and records structured success or failure. The renderer modes still require a supported Vulkan device and window system. Benchmark mode warms up before sampling; capture mode writes a binary PPM image. Every mode has a watchdog deadline and attempts to persist failure evidence before exiting.

Benchmark evidence records requested and actual framebuffer size, device and driver
identity, the actual negotiated present mode, validation and HUD state, warmup and
sample durations, frame count, and CPU timing percentiles. The historical total
`cpu_presentation_request_timings_ns` covers command recording plus the
complete/submit/present call; acquire is separately reported. Preserve these fields
with each result; do not treat a hardware-specific request rate as a portable FPS or
GPU-performance threshold.

`--benchmark-game` runs the production loopback server, networked Flight client,
authoritative simulation, and Vulkan player renderer. The benchmark API selects
ordinary alternating movement, accelerated movement with the actual x200 speed
modifier, a border spawn followed by wrapped traversal, or alternating server
permission policies that change collision bypass. `--workload` selects one
script; the default is `ordinary`. Run each script to compare separate receipts.
Wrapped traversal and permission publication require observed authoritative state
changes to count as complete. Policy compilation and publication times are
recorded separately from measured server tick times. Deterministic impaired transport, including loss, jitter, freezes,
reordering and duplication, is covered separately by the MC-AI-0247
`AdverseNetwork` acceptance tests; the loopback renderer benchmark does not
claim to measure frame timing under that impairment.

The benchmark records the first two seconds of connected loops as cold,
the next three as warm, and the final second without the client idle delay as
uncapped. The JSON keeps every loop and server tick duration, successful and
failed present requests, traffic event counts, and per-loop resident terrain
tiles, visible faces, and client application-message payload bytes (excluding
transport overhead). Available GPU durations are recorded as the latest
renderer sample and may lag the corresponding request. Its request rates count completed render/submit/present
requests completed inside each half-open measurement interval; a request
completed after the final interval is recorded as excluded, not counted in its
rate. `displayed_cadence_hz` is null because the client does not observe
physical scanout. Timing summaries include p50/p95/p99/p99.8/max; raw samples
remain in the JSON. Resident mesh bytes and power mode are
unavailable in this mode and are marked accordingly. GPU identity is also null
when the renderer reports only its generic presentation context. The benchmark succeeds
when every phase includes a completed request and the workload sends inputs
and processes server events; it does not gate on a device-specific FPS target.

`--present-immediate` requires immediate negotiation and fails instead of reporting
FIFO as an immediate result. The Release default leaves validation disabled. For a
controlled fixed-scene 1920-by-1080 pair, use the same command once without `--hud`
and once with it; the evidence records the selected HUD state. CoreGraphics may not
expose a physical display identifier or refresh mode, so those properties are not
inferred from this benchmark.

The benchmark and visible capture requests express their extents in framebuffer
pixels. Before either creates a presentation context, the shared acceptance
helper polls and adjusts GLFW logical window dimensions until the requested
pixel extent is observed. This is bounded by `max_resize_polls`; zero,
unrepresentable, no-progress, or two-step oscillating dimensions fail with
explicit evidence rather than silently comparing logical and framebuffer units.

Scenario source and evidence paths must be distinct, as must image and evidence paths. A zero exit status means the requested operation completed and its assertions passed; it does not replace manual review of the captured frame or prove a different platform, driver, or package.

The S7 adverse-network acceptance runs two production `GameClient` instances
through a test-only ENet relay into a production `GameServer`. Each client has
independent seeded client-to-server and server-to-client schedules for latency,
jitter, loss, duplication, reordering, bounded queues, freezes, and recovery
bursts. The impairment window ends at a recorded logical tick; normal delivery
then gives delayed authoritative state a bounded recovery period. The GoogleTest
XML property records the seed, per-direction traffic and schedule, server
authority, relay-delivered stale-input rejection, direct stale-revision snapshot
probes on both clients, queue bounds, and final convergence. Separate checks
cover a 15-second delivery freeze and reconnecting after server replacement.
The production reconnect test supplies the client's existing stop/deadline hooks,
so timeout cleanup can stop and join while disconnected or awaiting `JoinResponse`;
a short regression checks cancellation while disconnected. Each discovered
`mc_tests` case also has a 120-second CTest timeout. The relay is test-only and
does not alter shipping physics or rendering timing.

Capture tests use `--player-client-capture` and four named presets. They wait
for the full server-selected disk and CPU/renderer mesh coverage, compare keys
across 16 headings, then write PPMs. Default radius is 256 (205,861 tiles);
origin spawns at X/Y (0,0). Portable size is at least 1000x600; set
`MC_S7_REQUIRE_2560X1440=1` for exact Mac assets. Visually inspect every frame.

The four Mac images must be distinct: (1) third-person flying by the central
spike at server distance 72 (16,241 tiles), showing the whole spike, most of
the first ring and terrain behind it, with the player clear; (2) trench by the
first spike at its existing distance; (3) mountain climb; (4) first-person at
X/Z (0,0). Wait for the full selected disk and meshes. Export 2560x1440, inspect
each, and use a large bottom-center label below the HUD.
