# Runtime acceptance

The desktop executable provides bounded, noninteractive modes for snapshot evidence:

```sh
mc_main --scenario <file> --evidence <json>
mc_main --benchmark-render [--present-immediate] [--hud] --evidence <json>
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
