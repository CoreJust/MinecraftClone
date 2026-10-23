# Vulkan rendering

## Boundary and ownership

[`VulkanRenderer.hpp`](../../src/client/include/client/render/VulkanRenderer.hpp)
owns scene, shaders, camera, layouts and programs. CoreCpp's `PresentationContext`
owns validation, device/queues/synchronization, swapchain, views and surfaces.

`PresentationResourceScope` owns client device children used by an acquired
frame. Recreation releases cache/program/layout/scope before rebuilding.
`waitForSubmittedFrames()` drains slots without steady-state
`vkDeviceWaitIdle`; acquired slots own indirect buffers after fence completion.
Indirect batches respect `maxDrawIndirectCount`; direct fallback preserves
logical counts. Allocation/mapping failures use direct draws. Quads share indices `0,1,2,0,2,3`. Direction 6 reuses the 32-byte
instance for linear triangles with 1/32-block vertex heights; indexed corner 3
repeats vertex 0. Flagged triangles retain corner temperatures. Fragments sample canonical R6
temperature/elevation bilinearly from an atlas SSBO, independent of geometry.
Represented depth still controls deep Stone; universal depth/material parity is unproven.
Actual materials remain explicit. Stone/player/text pushes are 128/128/96
bytes; normals use slope lighting.
Text overwrites and per-image depth reuse are ordered across frame slots.
Scene/HUD receipts advance after submission, excluding undrawn publication.

## Scene and shader policy

```text
World players -> PlayerClient / AndroidPlayerClient -> PlayerRenderData span
  -> VulkanRenderer -> PresentationContext::Frame callback -> present/readback
```

Z-up uses zero-to-one depth. `InstalledShaderAssets` reads installed `shaders/`; Android
`AndroidShaderAssets` reads APK assets. Both require bare `.spv` names, without source fallback.

Frustum tests cull drawing only. LOD keeps full-world L0 while cells refine 4×4 through L1/L2. Shared chains stitch triangles; a 32-block band
adjusts whole intersecting cells and neighbor edge segments toward actual edges. Incomplete capture retains fallback. Async upload planning reuses canonical groups across unedited cache churn;
edits invalidate identity. Source matching avoids allocation. Ordinary bottoms use a textured
16×16 Bedrock rectangle; edits retain exact fallback. Uploads preserve the old scene until candidate completion. Stale candidates preserve columns.
See [gameplay]
(GAMEPLAY.md), [full-radius qualification](../ai/GAMEPLAY_QUALIFICATION.md), and
[LOD presentation tests](../../tests/client/terrain_lod_presentation_tests.cpp).

Active LOD terrain, blocks, players and world labels bend visually around camera
XY with radius 131072: Z drops by horizontal distance squared /262144.
CPU culling conservatively bounds curvature. Fragment fog starts at
the last 4% of an altitude-aware tangent horizon (sea 384), capped at 32768.
Adaptive projection covers altitude, terrain height and maximum bend; simulation
and GUI stay flat. Captures remain open.

Block instances carry material IDs; flagged smooth triangles pack temperatures.
[pinned vanilla atlas](../../assets/textures/terrain/README.md) repeats per
block: Grass tops are tinted, sides layered, bottoms Dirt. Decode sRGB PNGs to
linear before lighting; encode once, in shader for UNORM or hardware for sRGB
attachments. Capture `srgb_encoded` labels pixel encoding; offscreen is raw linear.

Loading copies RGBA8 images up to 512×512, skips unchanged uploads and stages
nearest-sampled updates with aspect-fitted letterboxing beneath GUI text. Desktop loading
hides the fallback grid/platform until Playing, including before map arrival. `map.vert`/`map.frag` ship for
desktop and Android. Loading callers may
enable `VulkanRenderer::setFrameCompletionTracking(true)` and read `frameProgress()`.
Submitted/completed IDs pair renderer sequences with swapchain generations.
While enabled, failures leave submission unchanged; toggling clears progress.
Completion needs a successful submitted-frame fence observation or completed
readback fence wait.
`beforeRecreate` records retired submission/slot receipts only after CoreCpp
completes every submitted fence. All receipts from that observation share its
time; prompt readiness cannot credit earlier display time, including a zero
remaining budget. Re-arm after generation changes; present IDs alone prove no
GPU completion.
Benchmarks opt in; ordinary play keeps tracking off.

CPU retirement transfers cache/queue, slot and draw-index ownership
to CoreCpp workers; removal and eight-frame GPU quarantine remain
synchronous. Rejected retirement retains ownership; teardown drains. Reapers
bound queued storage and recycled buffers separately at 64 MiB.

Terrain uses an 8 ms deadline, with 1 ms shared publication/admission after
overruns. Acquisition expires at render-entry plus ceil(1s/120); recording is
not deadline-bound. Capture retains a 1 s preload. Only pre-destruction
recreation and present-fence timeouts defer growth for 5 s; other Vulkan errors
are fatal. Diagnostics retain timeout status and distinguish quad from range counts.
[`height_tile_surface_mesher_tests.cpp`](../../tests/core/height_tile_surface_mesher_tests.cpp)
covers tops, steps and seams. `ChunkMesher` suppresses neighbor-plane seams;
caches require exact blocks, neighbors and revision. `ChunkMeshLodBuilder` merges
coplanar equal-direction/material faces into bounds-preserving, content-identified
Fine/Coarse variants. The selector uses caller thresholds and hysteresis; invalid
directions fail before indexing.
[`LOD tests`](../../tests/core/chunk_mesh_lod_tests.cpp) cover builder/selector
contracts.

## Text and GUI boundary

`TextRenderer` expands colored screen/world ASCII glyphs with UTF-8 fallback.
More than 16384 glyphs fails acquisition. World text has depth; GUI does not.
Count successful presentation completions; enable validation to interpret errors.

## Capture, input, and validation

Recreation/Android window replacement preserve readback. GLFW controls camera
and authoritative movement; Escape, R, F1 and F5 retain their actions.
F2 copies one rendered frame as an in-memory PNG to the macOS clipboard;
no screenshot file is saved. Held F2 neither repeats nor starts a ready world.
Command-modified input releases pointer capture and suppresses movement.
Explicit diagnostic file captures remain separate. Playing rebuilds GUI text
each frame to bound glyph count. Clipboard feedback uses a centered animated toast excluded from the copied image.
Android uses touch movement/look and hardware F5.

[`renderer_smoke_tests.cpp`](../../tests/client/renderer_smoke_tests.cpp)
covers desktop input, presentation/recreation/readback, HUD and resident faces.
Material capture checks textures and validation beside a canonical tile; normal
gameplay and Android runtime remain separate gates.

[`renderer_golden_tests.cpp`](../../tests/client/renderer_golden_tests.cpp)
captures S4 offscreen at 640×480. `VulkanOffscreenTarget` owns linear
color/submission/readback; Client owns depth and shared recording/pipelines.
Approval requires validation, the active versioned reference and independent
depth/scene checks. Review references at native size. S5 checks scene, replacement, validation and remote altitude.

`--benchmark-render` excludes server/network/gameplay; `--benchmark-game` counts
completed player-client present requests. Neither measures scanout. Evidence records GPU, extent, HUD, frame percentiles, CPU phases and submissions.
Stationary diagnostics freeze input. Optional terrain timestamps may be unsupported
on MoltenVK; Release disables validation. See [gameplay qualification](../ai/GAMEPLAY_QUALIFICATION.md).

Capture checks exact-radius meshes and 16 headings.
