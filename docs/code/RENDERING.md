# Vulkan rendering

## Boundary and ownership

[`VulkanRenderer.hpp`](../../src/client/include/client/render/VulkanRenderer.hpp)
owns scene, shaders, camera, layouts and programs. CoreCpp's `PresentationContext`
owns validation, device/queues/synchronization, swapchain, views and surfaces.

`PresentationResourceScope` owns acquired-frame device children. Recreation releases
cache/program/layout/scope before rebuilding; `waitForSubmittedFrames()` drains
without steady-state `vkDeviceWaitIdle`. Slot fences protect indirect buffers.
Batches honor `maxDrawIndirectCount`; direct fallback preserves logical counts on
allocation/mapping failure. Quads use 0,1,2,0,2,3. Direction 6 reuses 32-byte
instances for linear triangles at 1/32-block heights; corner 3 repeats vertex 0.
Flagged triangles retain corner temperatures. Fragments bilinearly sample canonical
R6 temperature/elevation from atlas SSBO, independent of geometry. Represented depth
controls deep Stone; universal depth/material parity is unproven. Materials stay
explicit. Stone/player/text pushes are 128/128/96 bytes; normals slope-light.
Text overwrites and per-image depth reuse are ordered across slots. Scene/HUD
receipts advance only on submission, excluding undrawn publication.

## Scene and shader policy

```text
World players -> PlayerClient / AndroidPlayerClient -> PlayerRenderData span
  -> VulkanRenderer -> PresentationContext::Frame callback -> present/readback
```

Z-up uses zero-to-one depth. `InstalledShaderAssets` reads installed `shaders/`; Android
`AndroidShaderAssets` reads APK assets. Both require bare `.spv` names, without source fallback.

Frustum culls drawing only. Full-world L0 remains fallback as cells refine 4×4
through L1/L2, stitched along shared edges. A 32-block band adjusts intersecting
cells and neighbor edges toward actual edges. Incomplete captures retain fallback.
Async planning reuses canonical groups through unedited churn; edits invalidate
identity; source matching avoids allocation. Ordinary bottoms use textured 16×16
Bedrock; edits retain exact fallback. Uploads preserve the old scene until candidate
completion; stale candidates preserve columns.
See [gameplay]
(GAMEPLAY.md), [full-radius qualification](../ai/GAMEPLAY_QUALIFICATION.md), and
[LOD presentation tests](../../tests/client/terrain_lod_presentation_tests.cpp).

Active LOD terrain, blocks, players and labels bend around camera XY (radius
131072); Z drops by distance squared /262144. CPU culling bounds curvature.
Fog starts in the horizon's final 4% (altitude-aware tangent, sea 384), capped
at 32768. Adaptive projection covers altitude, terrain and maximum bend;
simulation and GUI stay flat. Captures remain open.

Blocks carry material IDs; flagged smooth triangles pack temperatures. The
[pinned vanilla atlas](../../assets/textures/terrain/README.md) repeats per block:
tinted Grass tops, layered sides, Dirt bottoms. Decode sRGB before lighting; encode
once in shader for UNORM or hardware for sRGB attachments. Capture `srgb_encoded`
labels pixel encoding; offscreen data is raw linear.

Two RGBA8 map slots (≤512²) support eight nearest-sampled cropped/scissored draws;
unchanged uploads skip. Preparing/Connecting show a backdrop; generation pairs real
status/counters with a square map on dark canvas. Hide fallback until Playing. Six
captions (Tectonics, Relief, Water, Ice, Climate, Arrival) illustrate five 5-second
phases; Arrival is at 25 s. True Ready immediately selects Arrival and the exact
final default-map composite. Pause motion when hidden/iconified. Desktop/Android ship
`map.vert`/`map.frag`. `setFrameCompletionTracking(true)` exposes `frameProgress()`;
sequence IDs include swapchain generation. Failed submits do not advance; toggling
clears. Completion needs fence observation/readback. Recreation retires only after
fences complete; shared observation time prevents earlier-display credit. Re-arm on
generation change; present IDs alone do not prove completion. `capture_frame_completion`
retains ordered receipts; ordinary tracking keeps latest and disabling clears.
FPS counts unique fence-observed IDs/s over 1 s, not submissions or scanout.

CPU retirement moves cache/queue/slot/draw-index ownership to CoreCpp workers;
removal and eight-frame GPU quarantine stay synchronous. Rejected retirement
retains caller ownership; teardown drains. Reapers cap queued storage and recycled
buffers at 64 MiB each.

Terrain has an 8 ms deadline; shared admission/publication may run 1 ms after
overrun. Ordinary acquisition expires at render-entry plus ceil(1s/120); recording
is unchecked. Capture preload is 1 s. Only pre-destruction recreation or present-
fence timeouts defer growth 5 s; other Vulkan errors are fatal. Diagnostics retain
timeout status and distinguish quad/range counts. [`height_tile_surface_mesher_tests.cpp`](../../tests/core/height_tile_surface_mesher_tests.cpp)
covers tops/steps/seams. `ChunkMesher` suppresses neighbor seams; caches require
exact blocks/neighbors/revision. `ChunkMeshLodBuilder` merges coplanar equal-direction/
material faces into bounded content-identified Fine/Coarse variants; its selector
uses caller thresholds/hysteresis and rejects invalid directions. [`LOD tests`](../../tests/core/chunk_mesh_lod_tests.cpp)
cover builder/selector.

## Text and GUI boundary

`TextRenderer` expands screen/world ASCII (UTF-8 fallback); >16384 glyphs fails.
World text is depth-tested; GUI is not. HUD defaults to Fixed; F7 enables Adaptive
only during captured-look Playing with map closed and no start transition.
Non-maximized windows force compact HUD; logical width <1050 or height <520 too.
`BlockInteractionEffects` draws authoritative targets/cracks; accepted receipts
spawn ≤16 particles each, 256 live total, one-second lifetime. Count successful
presentations; validation explains errors.

## Capture, input, and validation

Completed captures survive recreation; native recreation clears pending eligibility
until a fresh frame. GLFW camera/movement; Escape/R/F1/F5 retain actions. macOS F2
copies one frame as clipboard PNG (no file), once per press, never starting Ready.
Command modifiers release capture/suppress movement; diagnostic captures are separate.
Playing rebuilds GUI text each frame. Captures omit hints/toasts; feedback appears
centered afterward. Android uses touch movement/look and hardware F5.

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
