# Vulkan rendering

## Boundary and ownership

[`VulkanRenderer.hpp`](../../src/client/include/client/render/VulkanRenderer.hpp)
owns scene order, push constants, shaders, camera, layouts and program/cache
lifetimes. CoreCpp owns program validation, device/queues, commands,
synchronization, swapchain/views and surfaces via `PresentationContext`.

Client device children use `PresentationResourceScope` during acquired
frames. Recreation releases cache, programs, layouts and scope before rebuilding.
`waitForSubmittedFrames()` drains slots without steady-state `vkDeviceWaitIdle`.

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

The platform/grid/player scene is legacy flat-world Snapshot 4 coverage. Normal
Flight clears sky blue and draws server-streamed height-tile terrain with the
original 16² stone texture. Local cameras sample player position separately
from input look. First person hides the
local body; third-person modes draw it. Four-bit replicated palette identities
map to 16 opaque colors; mesh uploads follow content identity. The HUD reports
authoritative Flight XYZ and camera angles. Camera: right-handed Z-up,
zero-to-one depth, depth testing.

Desktop `InstalledShaderAssets` reads installed `shaders/`; Android
`AndroidShaderAssets` reads APK assets. Both require bare `.spv` names;
no source-tree fallback.

S7 streams server-selected camera-independent disks (default 256: 205,861 tiles;
128: 51,433); frustum only culls drawing.
`HeightTileDrawIndex` incrementally orders live arena ranges and
refreshes affected 16²-tile groups. Per-frame `VulkanFrustum` classification skips
resolved planes with exact individual wrapped-AABB visibility; ambiguous wrap
cuts use individual images. Visible ranges merge without sorting or freed spans.
[`Tests`](../../tests/client/height_tile_draw_index_tests.cpp) cover moving cameras,
mutation/failure, seams and radii 256/1,024.
LOD uses wrapped player distance and actual vertical FOV/framebuffer extent,
hysteresis, and replacement cell faces bounded to 2 px². Bounds enclose complete
raw neighbors and installed edge profiles, including retained revisions/detail.
First publication uses certified detail directly without duplicate coarse/final
work. Jobs recheck revisions, neighbors, detail and epoch. Failed uploads retain
installed geometry/certificate inputs; displayed meshes refine asynchronously
after movement/FOV/profile changes. Seam batches atomically publish the center and
changed neighbors; failure retains renderer/client state. Cached seam spans handle
out-of-order uploads. LOD preserves terrain/collision; far plane/fog scale with
the accepted radius,
and workers mesh outside presentation.

The arena grows with streamed residency; restoration shares the frame deadline.
Gameplay bounds uploads and rendering to 8 ms, deferring overdue tiles.
Runtime face counts track residency; `chunk_draw_face_count` sums recorded quads
and `chunk_draw_count` counts logical ranges.
[`height_tile_surface_mesher_tests.cpp`](../../tests/core/height_tile_surface_mesher_tests.cpp)
covers flat tiles, exposed height differences, neighbor seams, and generated
column tops.

`ChunkMeshLodBuilder` creates bounded `Fine`/`Coarse` variants by merging
same-direction/material coplanar faces; both retain content identity and full-chunk
bounds. `MeshLodSelector` uses caller-supplied squared distance thresholds,
hysteresis and available-variant fallback; it neither sets distance nor remeshes.
Invalid directions are rejected before grid indexing.
[`Tests`](../../tests/core/chunk_mesh_lod_tests.cpp) cover these contracts.

## Text and GUI boundary

`TextRenderer` lays out colored glyph instances in screen or world space.
`text.vert` expands quads and `text.frag` samples the ASCII bitmap, using one
fallback glyph per unsupported UTF-8 code point. Presentation accepts 16,384
world/GUI glyphs before frame acquisition fails.

World text is depth tested; `GuiRenderer` records screen text after the world
scene without depth. `DebugHudState` formats five lines, presenting gameplay Z
as user-facing height. `VulkanRenderer` supports independent GUI/world labels.

The renderer counts only frames whose `PresentationContext::complete` succeeds.
`VulkanRenderer::validationErrorCount()` forwards presentation validation errors;
enable validation when interpreting the count.

## Capture, input, and validation

Capture enables transfer-source presentation and returns an owned RGBA8 vector
after submission. Readback allocation occurs only on capture; recreation,
resize, and Android window replacement retain the capture contract.

Desktop GLFW cursor deltas control yaw/pitch; W/A/S/D lower through
`CameraController` into unchanged authoritative `Direction` packets. Window input
retains Escape, shader-reload R, debounced F1 HUD-toggle and debounced F5 camera-cycle.
Android lowers left-drag movement/right-drag yaw/pitch identically, retaining assets
and `AndroidInput`; hardware F5 uses the same cycle.

[`renderer_smoke_tests.cpp`](../../tests/client/renderer_smoke_tests.cpp)
tests desktop GLFW input, presentation/recreation/readback, HUD and resident face counts.
Android package compilation and emulator presentation remain separate gates.

`renderer_golden_tests.cpp` captures the fixed 640-by-480 S4 scene without
GLFW/surface/swapchain; oblique cases cover yaw `37.2`, its opposite and pitch `-35`.
`VulkanOffscreenTarget` owns linear color, submission/readback; Client owns depth
and shared presentation recording/pipelines. Strict approval requires validation,
the active versioned reference and independent depth/scene checks. References
change only after bounded native-size review. Offscreen GUI/HUD checks top-left pixels.

S5 offscreen acceptance verifies textured stone, sky, deterministic frames,
mesh replacement, validation, and remote-player altitude.

The fixed-scene `--benchmark-render` excludes server/network/gameplay;
`--benchmark-game` counts completed player-client present requests with all three.
Neither observes physical scanout; unavailable immediate mode fails. Evidence
includes resolution, GPU, HUD, rate, p50/p95/p99/max, CPU phases and submitted work.
`DiagnosticStationary` freezes movement/look for diagnostic A/B, not gameplay
acceptance. Opt-in terrain timestamps are not whole-frame time; MoltenVK reports
unsupported/null because in-render-pass markers may be deferred to encoder end.
Release disables validation; `--hud` supports paired runs.
S6 renderer stress uses 90-by-90 maximum capacity and bounded edge streaming; S6 gameplay uses
the smaller directional ellipse in [GAMEPLAY.md](GAMEPLAY.md). Its renderer-only
gate requires nominal 120 Hz within one percent and p99 at most 10 ms,
including display wait and scheduler jitter.

Benchmark and renderer capture preserve requested framebuffer pixels through
bounded GLFW resizing. Capture checks readback, sky, and stone; benchmark checks
presentation, a stone draw, and phase timing. Networked S7 capture runs the
production server/client, waits for the accepted radius's exact resident tiles and uploaded
meshes, compares keys across 16 headings, and validates terrain, HUD, and label.
Capture HUD FPS includes full-disk readiness scans and heading-key comparisons,
so is not normal-game throughput. Radius 1,024 coverage is mathematical;
runtime capacity/performance is unmeasured.
