# Runtime acceptance

The desktop executable provides bounded, noninteractive modes for snapshot evidence:

```sh
mc_main --scenario <file> --evidence <json>
mc_main --benchmark-render [--present-immediate] --evidence <json>
mc_main --capture-render --image <ppm> --evidence <json>
```

Scenario mode parses the versioned scripting format, starts a bounded local server, drives clients through authoritative ticks, evaluates assertions, and records structured success or failure. The renderer modes still require a supported Vulkan device and window system. Benchmark mode warms up before sampling; capture mode writes a binary PPM image. Every mode has a watchdog deadline and attempts to persist failure evidence before exiting.

Benchmark evidence records requested and actual framebuffer size, device and driver identity, present mode, validation state, warmup and sample durations, frame count, and frame-time percentiles. Preserve these fields with each result; do not treat a hardware-specific FPS value as a portable pass threshold.

Evidence and image paths must be distinct. A zero exit status means the requested operation completed and its assertions passed; it does not replace manual review of the captured frame or prove a different platform, driver, or package.
