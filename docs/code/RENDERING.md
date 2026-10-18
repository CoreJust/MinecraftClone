# Vulkan rendering

## Boundary and ownership

[`VulkanRenderer.hpp`](../../src/client/include/client/render/VulkanRenderer.hpp)
owns scene/shaders/camera/layouts/program lifetimes; CoreCpp's
`PresentationContext` owns validation, device/queues/commands/synchronization
and swapchain/views/surfaces.

`PresentationResourceScope` owns client device children during acquired frames.
Recreation releases cache/program/layout/scope before rebuilding.
`waitForSubmittedFrames()` drains slots without steady-state
`vkDeviceWaitIdle`. Acquired slots own indirect buffers after fence completion.
`multiDrawIndirect` + `drawIndirectFirstInstance` use
`maxDrawIndirectCount`-bounded batches; direct fallback preserves logical
counts. Allocation/mapping failures use direct draws; device loss propagates.
Indexed terrain shares `0,1,2,0,2,3`, preserving instances, winding and diagonal.
Diagnostic overrides: `MC_DIAGNOSTIC_INDEXED_STONE_QUADS=0` selects six
vertices; `MC_DIAGNOSTIC_DIRECT_STONE_DRAWS=1` forces direct submission.
`cpu_acquire_wait_duration_ns` measures total acquire time; CoreCpp optionally
reports individual stages.

## Scene and shader policy

```text
World players -> PlayerClient / AndroidPlayerClient -> PlayerRenderData span
  -> VulkanRenderer -> PresentationContext::Frame callback -> present/readback
```

Flight draws sky/terrain. Cameras separate position/look; first person hides
the body, third person shows it. Four-bit identities select 16 opaque colors.
HUD reports authoritative XYZ/angles; camera
is right-handed Z-up with zero-to-one depth.

Desktop `InstalledShaderAssets` reads installed `shaders/`; Android
`AndroidShaderAssets` reads APK assets. Both require bare `.spv` names, with no
source-tree fallback.

S7 streams server-selected camera-independent disks (R256: 205,861 tiles; R128:
51,433); frustum only culls drawing. `HeightTileDrawIndex` orders live arena
ranges in 8×8 groups. Resolved frustum planes skip exact wrapped-AABB visibility;
wrap-ambiguous groups use individual images. Three stable radix passes order
visible ranges before merging; freed spans are excluded. Installed quads and
seams form direction buckets; only provably backfacing batches drop. Coplanar/
wrap ambiguities remain; direct and indirect paths agree.
[`Visibility/mutation tests`](../../tests/client/height_tile_draw_index_tests.cpp).
Interest is a normalized player-centered disk; heading/projection affect only priority/LOD.
Mac defaults `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0`; explicit overrides win.
LOD uses wrapped player distance, actual vertical FOV/framebuffer extent and
hysteresis; every replacement face stays ≤2 px². Face-area bounds include
candidate tops/walls and seam gaps against installed neighbor tops; nearest
wrapped surface bounds drive selection. First publication uses certified detail;
jobs recheck revisions, neighbors and safety. Profile-only changes preserve valid
geometry. Failed uploads retain installed meshes; movement/FOV/profile changes
refine. Remesh batches deduplicate each tile's cardinal neighborhood.
Seam batches atomically publish center/neighbors; recoverable rejection retains state. Cached
seam spans support out-of-order uploads. Tile eviction synchronously removes
renderer state and queues four cardinal seam rebuilds; prioritized workers
compose/upload replacements outside presentation. LOD preserves terrain/collision;
far plane/fog scale with radius.

CPU retirement transfers cache/queue, renderer-slot and draw-index ownership to
CoreCpp workers; logical removal and eight-frame GPU quarantine stay synchronous.
Discarded worker sources/results follow this path. Reapers cap nested storage at
4,096 jobs/64MiB; rejection retains ownership and records fallback. Teardown drains.
Tickets/empty buffers recycle within another64MiB; overflow retires normally.
Radius-reserved scratch avoids reallocations. Priority refresh inspects256 entries
without a global barrier. Queued work, results and deferral order: near, forecast,
missing prewarm, missing far, replacement prewarm, replacement far. Distance breaks
ties; retries retain captured mesh class.

Canonical-coordinate histograms and mandatory bucket indexes update incrementally.
Certificate epochs invalidate lazily. Each refresh inspects ≤64 optional Fine-bucket
IDs; dirty/coarse refinement remains immediate. A persistent cursor drains
stationary work without whole-residency scans.

Terrain uses 8 ms (1 ms shared publication/admission after overruns).
Acquisition expires at render-entry+ceil(1s/120); recording is not deadline-bound.
Capture retains 1 s preload. Only pre-destruction recreation/present-fence
timeouts defer growth 5 s; other Vulkan errors are fatal. Diagnostics retain
the timeout; `chunk_draw_face_count`/`chunk_draw_count` count quads/ranges.
[`height_tile_surface_mesher_tests.cpp`](../../tests/core/height_tile_surface_mesher_tests.cpp)
covers tops, steps and seams.

`ChunkMeshLodBuilder` merges coplanar equal-direction/material faces into
bounded `Fine`/`Coarse` variants with content identity and full-chunk bounds.
`MeshLodSelector` uses caller thresholds, hysteresis and fallback, not distance
or remesh policy. Invalid directions fail before indexing; heap workspace bounds
stacks.
[`LOD tests`](../../tests/core/chunk_mesh_lod_tests.cpp).

## Text and GUI boundary

`TextRenderer` lays out colored screen/world glyphs; `text.vert` expands quads;
`text.frag` samples an ASCII bitmap with one fallback per unsupported UTF-8
code point. More than 16,384 world/GUI glyphs fails acquisition.
World text is depth-tested; GUI is not. HUD maps Z to height; GUI/world labels
are independent. Count only successful `PresentationContext::complete` frames.
`VulkanRenderer::validationErrorCount()` forwards presentation errors; enable
validation to interpret them.

## Capture, input, and validation

Recreation, resize and Android window replacement preserve requested RGBA8 readback.

GLFW controls yaw/pitch; WASD passes authoritative `Direction` through
`CameraController`. Escape, R reload, F1 HUD and F5 camera remain. Android uses
left-drag movement/right-drag look via `AndroidInput`, plus hardware F5.

[`renderer_smoke_tests.cpp`](../../tests/client/renderer_smoke_tests.cpp) covers
desktop input, presentation/recreation/readback, HUD and resident faces. Android
package compilation and emulator presentation are separate gates.

`renderer_golden_tests.cpp` captures 640-by-480 S4 offscreen; oblique yaw is
`37.2`/opposite, pitch `-35`.
`VulkanOffscreenTarget` owns linear color/submission/readback; Client owns
depth and shared recording/pipelines. Approval requires validation, the active
versioned reference and independent depth/scene checks. Reference changes require
native-size review; GUI/HUD checks top-left pixels.

S5 offscreen checks scene/replacement/validation/remote-player altitude.

`--benchmark-render` excludes server/network/gameplay. `--benchmark-game` counts
completed player-client present requests with all three; neither measures
scanout, and missing immediate mode fails. Evidence records resolution/GPU/HUD/
rate, p50/p95/p99/max, CPU phases and submitted work. `DiagnosticStationary`
freezes movement/look for diagnostic A/B only. Terrain timestamps are opt-in,
not whole-frame; MoltenVK reports unsupported/null when render-pass markers defer
to encoder end. Release disables validation; `--hud` supports paired runs.
`speed-200-full-radius` keeps normal HUD/scheduling and requires 225 current
radius-7 meshes each iteration; endpoint refill cannot mask gaps
([`qualification`](../ai/GAMEPLAY_QUALIFICATION.md)). Barriers report faces/
capacity; samples retain CPU, not GPU or physical-cadence, timing. S6 stress uses
90-by-90 capacity and bounded edge streaming; its gate is 120 Hz within 1% and
p99 ≤10 ms including display wait/jitter.

GLFW resizing preserves framebuffer pixels. Capture checks readback/sky/stone;
benchmark checks presentation/stone draws/timing. Networked
S7 capture waits for exact-radius production meshes, compares keys at 16
headings and validates terrain/HUD/label. FPS includes full-disk scans, not
normal throughput. Radius1,024 coverage is mathematical; runtime capacity/performance
is unmeasured.
