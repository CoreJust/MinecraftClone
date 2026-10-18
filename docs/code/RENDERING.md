# Vulkan rendering

## Boundary and ownership

[`VulkanRenderer.hpp`](../../src/client/include/client/render/VulkanRenderer.hpp)
owns scene order, push constants, shaders, camera, layouts and program/cache
lifetimes. CoreCpp owns program validation, device/queues, commands,
synchronization, swapchain/views and surfaces via `PresentationContext`.

Client device children use `PresentationResourceScope` during acquired
frames. Recreation releases cache, programs, layouts and scope before rebuilding.
`waitForSubmittedFrames()` drains slots without steady-state `vkDeviceWaitIdle`.

Keep generation/ID-keyed warmup/drain requests and polls. Missing timestamps or
later measurement generations fail; timing is not scanout
([qualification](../ai/GAMEPLAY_QUALIFICATION.md)).

Acquired slots own indirect buffers after fence completion; recreation
releases them. `multiDrawIndirect` + `drawIndirectFirstInstance` enable
`maxDrawIndirectCount`-bounded batches; direct fallback preserves logical counts.
Recoverable allocation/mapping failures use direct draws; device loss propagates.
Indexed terrain shares `0,1,2,0,2,3`, preserving instances, winding and diagonal.
Diagnostic overrides: `MC_DIAGNOSTIC_INDEXED_STONE_QUADS=0` selects six vertices;
`MC_DIAGNOSTIC_DIRECT_STONE_DRAWS=1` forces direct submission.

## Scene and shader policy

```text
World players -> PlayerClient / AndroidPlayerClient -> PlayerRenderData span
  -> VulkanRenderer -> PresentationContext::Frame callback -> present/readback
```

S4 covers platform/grid/player. Flight draws sky blue and server-streamed terrain
with the original 16² stone texture. Camera sampling is independent of input
look. First person hides the local body; third person draws it. Replicated
four-bit palettes select 16 opaque colors; uploads follow content identity.
HUD reports authoritative XYZ/camera angles. Camera: right-handed Z-up,
zero-to-one depth, depth testing.

Desktop `InstalledShaderAssets` reads installed `shaders/`; Android
`AndroidShaderAssets` reads APK assets. Both require bare `.spv` names;
no source-tree fallback.

S7 streams server-selected camera-independent disks (default 256: 205,861 tiles;
128: 51,433); frustum only culls drawing.
`HeightTileDrawIndex` packs live entries into 16²-tile groups.
Frustum classification skips resolved planes, preserving exact wrapped-AABB
visibility; ambiguous wrap cuts use individual images. Stable radix ordering
merges ranges without freed spans. Installed quads, including seams, form direction buckets:
only provably backfacing batches drop. Coplanar/wrap ambiguities remain;
direct/indirect paths agree.
[`Tests`](../../tests/client/height_tile_draw_index_tests.cpp) cover moving cameras,
mutation/failure, seams and radii 256/1,024.
LOD uses wrapped player distance, actual vertical FOV/framebuffer extent and
hysteresis, bounding replacement cell faces to 2 px². Per-detail areas cover
candidate tops/walls and installed neighbor TOP profiles; raw bounds enclose
distance. First publication uses certified detail directly. Jobs recheck raw
revisions, neighbors and current detail safety; profile-only changes need not
discard valid completed geometry. Failed uploads retain installed state; meshes refine
after movement/FOV/profile changes. Seam batches atomically publish the center and
changed neighbors; failure retains renderer/client state. Cached seam spans handle
out-of-order uploads. LOD preserves terrain/collision; far plane/fog scale with
the accepted radius.

CoreCpp workers retire caches, queues, renderer slots and draw indexes; logical
removal and eight-frame GPU quarantine stay synchronous. Reapers cap storage at
4,096 jobs/64 MiB; rejection retains ownership and records fallback;
teardown drains. Ticket/empty-buffer recycling shares another 64 MiB; overflow
retires. Priority refresh inspects 256 entries without barriers. Admission,
results and deferral rank near, forecast, missing prewarm/far, then replacement
prewarm/far; distance breaks ties and retries retain captured mesh class.

Terrain uses 8 ms (1 ms shared publication/admission after overruns). Acquisition
expires at render-entry+ceil(1s/120); recording is not deadline-bound. Capture
retains 1 s preload. Only exact recreation/present-fence timeouts before destruction
defer growth (5 s); other Vulkan errors are fatal. Diagnostics report the latest timeout.
`chunk_draw_face_count` counts quads; `chunk_draw_count` counts ranges.
[`height_tile_surface_mesher_tests.cpp`](../../tests/core/height_tile_surface_mesher_tests.cpp)
covers flat tiles, exposed height differences, neighbor seams, and generated
column tops.

`ChunkMeshLodBuilder` creates bounded `Fine`/`Coarse` variants, merging coplanar
faces of equal direction/material, retaining content identity/full-chunk bounds.
`MeshLodSelector` applies caller-supplied squared-distance thresholds, hysteresis
and available-variant fallback; never sets distance or remeshes. Invalid directions
are rejected before indexing. Per-build heap workspace bounds stacks.
[`Tests`](../../tests/core/chunk_mesh_lod_tests.cpp) cover these contracts.

## Text and GUI boundary

`TextRenderer` lays out colored glyph instances in screen or world space.
`text.vert` expands quads and `text.frag` samples the ASCII bitmap, using one
fallback glyph per unsupported UTF-8 code point. Presentation accepts 16,384
world/GUI glyphs before frame acquisition fails.

World text uses depth; `GuiRenderer` records screen text afterward without it.
`DebugHudState` presents gameplay Z as height. GUI/world labels are independent.

The renderer counts only frames whose `PresentationContext::complete` succeeds.
`VulkanRenderer::validationErrorCount()` forwards presentation validation errors;
enable validation when interpreting the count.

## Capture, input, and validation

Recreation, resize and Android replacement preserve RGBA8 readback.

GLFW controls yaw/pitch; WASD passes authoritative `Direction` through
`CameraController`. Escape, R reload, F1 HUD and F5 camera remain. Android uses
left-drag movement/right-drag look via `AndroidInput`, plus hardware F5.

[`renderer_smoke_tests.cpp`](../../tests/client/renderer_smoke_tests.cpp) covers
desktop input, presentation/readback, HUD and resident faces. Android build and
emulator presentation are separate gates.

`renderer_golden_tests.cpp` captures 640-by-480 S4 offscreen; oblique yaw is
`37.2`/opposite, pitch `-35`.
`VulkanOffscreenTarget` owns linear color/submission/readback; Client owns
depth/recording/pipelines. Approval requires validation, the active versioned
reference, independent scene/depth checks and native-size review; GUI/HUD checks
top-left pixels.

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
