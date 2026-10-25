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
Triangles pack temperatures. R6 bilinearly samples atlas temperature/elevation;
V9 nearest-samples the discrete source-cell material from biome, water and elevation.
Depth controls Stone; depth/material parity remains unproven. Stone/player/text pushes
are 128/128/96 bytes; normals slope-light. The [field builder](../../src/client/render/CanonicalTerrainField.cpp)
and [GPU record](../../src/client/include/client/render/CanonicalTerrainField.hpp) define this V9 upload.
Text overwrites and per-image depth reuse are ordered across slots. Scene/HUD
receipts advance only on submission, excluding undrawn publication.

## Scene and shader policy

```text
World players -> PlayerClient / AndroidPlayerClient -> PlayerRenderData span
  -> VulkanRenderer -> PresentationContext::Frame callback -> present/readback
```

Z-up depth is zero-to-one. Desktop/Android load bare installed/APK `.spv` assets;
no source fallback.

Conservative order; L0 refines 4×4 through stitched L1/L2. Incomplete 32-block
neighbor captures retain fallback. Async plans reuse groups; sealed captures share
identity and check tile availability, revisions and dense content. Bounded changes
use exact checks; resident diffs catch borrowed edits. Textured 16×16 Bedrock keeps
edit fallback. Uploads retain old scenes/stale columns until complete.
See [gameplay]
(GAMEPLAY.md), [full-radius qualification](../ai/GAMEPLAY_QUALIFICATION.md), and
[LOD presentation tests](../../tests/client/terrain_lod_presentation_tests.cpp).

Terrain, blocks, players and labels bend around camera XY (radius 131,072); Z
drops by distance²/262,144. CPU culling bounds curvature. Final-4% horizon fog
(altitude-aware tangent, sea 384), capped at 32,768. Adaptive projection covers
altitude/terrain/bend; simulation and GUI stay flat. Captures remain open.

Material IDs fixed; Sand=7. Smooth triangles pack temperatures. The [pinned
vanilla atlas](../../assets/textures/terrain/README.md) keeps tinted Grass tops,
layered sides and Dirt bottoms; six deterministic Sand faces precede overlay. Each
sRGB layer has five linear-light mips (16×16–1×1). Premultiply before filtering;
magnify nearest/minify trilinear. Decode for lighting; encode for UNORM output.
`srgb_encoded` reports pixel encoding; offscreen remains linear.

Two `512²` RGBA8 slots; eight draws. Prep/Connect backdrops hide fallback. Five 2s phases overlap computation; hidden views pause; latest-only 24 Hz. Molten cooling, crust/ocean formation; continuous 2× drift, scaled/uplifted land-only shade. V9 wind/current/rain and temperature ice lead to exact sharp atlas; no RGB blur. Late fields: 1 s. True Ready; prompt waits for formation/scene/frame fences. Desktop/Android. Map fit; +Y up; heading 0 north/+90 east; raster/marker/pointer align.

`setFrameCompletionTracking(true)` exposes `frameProgress()`;
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

Text: ASCII/UTF-8 fallback, `16,384`-glyph cap; depth-tested world, GUI not. HUD: colored adaptive pixels (`4.5:1`), luminance-aware backing, distinct XYZ hues; co-row XYZ/speed when fitting; hide unavailable humidity, redundant Break/Place. F7 styles only captured-look Playing, map closed, transition-free. Resident/capacity chunks; ready/required columns. Compact windowed <`1050` wide or <`520` high.
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
completed player-client present requests, not scanout. Frame evidence adds nested LOD
timings/counts; spans overlap; diagnostics sample them.
Stationary diagnostics freeze input. Optional terrain timestamps may be unsupported
on MoltenVK; Release disables validation. See [gameplay qualification](../ai/GAMEPLAY_QUALIFICATION.md).

Capture checks exact-radius meshes and 16 headings.
