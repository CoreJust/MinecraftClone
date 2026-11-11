# Vulkan rendering

## Boundary and ownership

[`VulkanRenderer.hpp`](../../src/client/include/client/render/VulkanRenderer.hpp)
owns scene, shaders, camera, layouts and programs. CoreCpp's `PresentationContext`
owns validation, device/queues/synchronization, swapchain, views and surfaces.

`PresentationResourceScope` owns acquired-frame children. Recreation rebuilds
resources. Growth drains fences and clears per-image scene/depth and
selection/Stone descriptors. Matching settings retain device resources. Slot fences
protect indirect buffers. `waitForSubmittedFrames()` avoids steady-state `vkDeviceWaitIdle`.

Indirect limits and allocation/mapping fallback preserve counts.
Quads use 0,1,2,0,2,3. Faces are 48 bytes: low three direction bits select
face 6; bits 3–5 carry vertex height bit 16. Decoders mask direction.
Top face 2,048 is 65,536 at 1/32-block precision; corner 3 repeats vertex 0.
Climate temperatures and V21 same-LOD materials retain payload bits.
16-byte normals average shared Q5 vertices. Voxels/skirts stay flat.
Weak memberships survive edits. Expired/reordered contributors require full preparation.
Seam-owner categories use barycentric coverage without ID/albedo blending.
R6 samples atlas climate. V9 minifies albedo; near textures, IDs and underwater
cutoffs remain exact. Parity unproven.
Push sizes: Stone/player/text 128/128/96 bytes.
[Field builder](../../src/client/render/CanonicalTerrainField.cpp) and
[GPU record](../../src/client/include/client/render/CanonicalTerrainField.hpp)
define field uploads. V10 uses authoritative top materials. Slot order protects
text/depth.

## Scene and shader policy

```text
World players -> PlayerClient / AndroidPlayerClient -> PlayerRenderData span
  -> VulkanRenderer -> PresentationContext::Frame callback -> present/readback
```

Z-up uses zero-to-one depth. Desktop/APK shaders require bare `.spv` names. No source fallback.

L0 refines 4×4 through stitched L1/L2 with a 32-block neighbor band.
Beyond the distance band, fine-V10 L1 cells reuse a canonical 5×5 child probe
to refine sampled fan errors >1 block. Residency is unchanged; interiors/seams
are not globally bounded.
Matching promotions reuse ordered samples; otherwise recollect.
[RiverBedMesh](RIVER_BEDS.md) constrains V10 L0–L2.
V10 positive wet fan vertices use canonical Dirt; authority edges retain voxel materials.
Authoritative captures store 16×16 columns, one-cell cardinal halos, tile/edit
versions and edited chunks. Immutable captures share a batch identity and omit
per-cell configuration strings. Publication validates source versions. Eviction
preserves bounded canonical proofs. Dense hashes are rechecked. Edits,
requirements, republication and identity invalidate proofs. Current-first and
settle fall back to full preparation when local patching fails.

Terrain, blocks, players and labels bend around camera XY: radius 131072,
Z drop distance²/262144. CPU culling bounds curvature. Horizon fog starts in the final
4% (altitude tangent, sea 384), capped at 32768. Adaptive projection covers
altitude, terrain and max bend; simulation/GUI stay flat.

Sand=7. The [pinned
vanilla atlas](../../assets/textures/terrain/README.md) keeps tinted Grass tops,
layered sides and Dirt bottoms; six deterministic Sand faces precede the overlay.
Each sRGB layer has five linear mips (16×16–1×1). Premultiply before
filtering; magnify nearest, minify trilinear. Decode for lighting; encode for
UNORM/sRGB output. Capture `srgb_encoded` reports encoding; offscreen stays linear.

Two ≤512² RGBA8 slots serve eight draws. Hidden views pause. Matching native
fields retain history through replacement. Optional 24 Hz history retains sharp
wind/current arrows. Plate ownership, phase-zero ice and climate means differ.
Start requires scene/frame fences. Minimap: +Y up, 0° north, +90° east.
Player-centered UVs retain half-artifact overscan. Lost coverage rebuilds previews.
Desktop maps show baked rivers during cancellable viewport rasterization.

`setFrameCompletionTracking(true)` exposes `frameProgress()`. IDs include swapchain
generation. Failed submits leave IDs unchanged. Toggling clears tracking. Fences
or readback establish completion. Recreation waits/rearms. Shared timestamps
prevent early credit. `capture_frame_completion` keeps ordered receipts, ordinary
tracking the latest. FPS counts unique fence-observed IDs per second.

CoreCpp workers retire cache/queue/slot/draw-index state. Removal and 8-frame GPU
quarantine stay synchronous. Rejection preserves caller ownership. Teardown
drains reapers. Queued storage and recycled buffers each cap at 64 MiB.

Terrain has 8 ms plus 1 ms admission/publication overrun. Pending uploads
reserve 1 ms after planning. Acquisition expires at render-entry + ceil(1s/120).
Recording is unchecked. Capture preload takes 1 s. Only pre-destruction
recreation/present-fence timeouts defer growth 5 s. Other Vulkan errors are fatal.
Diagnostics count timeouts and quads/ranges. [`height_tile_surface_mesher_tests.cpp`](../../tests/core/height_tile_surface_mesher_tests.cpp)
covers tops/steps/seams. `ChunkMesher` suppresses neighbor seams; caches require
exact blocks/neighbors/revision. `ChunkMeshLodBuilder` merges coplanar equal-direction/material faces into bounded
Fine/Coarse variants; selection validates directions and caller thresholds/hysteresis.

## Text and GUI boundary

Text uses ASCII/UTF-8 fallback and a 16,384-glyph cap. Only world text is
depth-tested. HUD defaults to outlines and distinct XYZ hues. Experimental F7
contrast defaults off. Footer shows target XYZ/material/ID. Spectators lack
wireframe. Effects use terrain-relative coordinates and flat shader seeds.
Antialiased fractures use the 16×16 material scale. Held breaks emit particles.
Bursts spawn ≤16, with 256 live for one second under analytic gravity. Target
loss preserves bursts. Successful presents count. Validation explains errors.

## Capture, input, and validation

Captures survive recreation. Native eligibility needs a fresh frame. Desktop
defaults fullscreen except windowed captures/benchmarks. Escape closes maps or
asks to exit. F1 hides both HUD sections. Windows/macOS F2 copies to clipboard
without saving or starting Ready. Command modifiers release capture/suppress
movement. Diagnostics stay separate. Playing rebuilds text every frame.
Captures omit hints/toasts. Centered feedback follows. Android uses touch/hardware F5.

[`renderer_smoke_tests.cpp`](../../tests/client/renderer_smoke_tests.cpp)
covers desktop input, presentation/recreation/readback, HUD and resident faces.
Material capture checks textures and validation near a canonical tile. Gameplay and Android runtime qualify separately.

[`renderer_golden_tests.cpp`](../../tests/client/renderer_golden_tests.cpp) captures S4 at 640×480.
`VulkanOffscreenTarget` owns linear color/submission/readback; Client owns depth,
recording and pipelines.
Approval checks validation, active native-size reference and independent depth/scene
results. S5 covers scene/replacement/validation/altitude.

`--benchmark-render` excludes server/network/gameplay; `--benchmark-game` counts
completed present requests. LOD timings overlap. Stationary diagnostics freeze
input. MoltenVK can lack timestamps. Release disables validation. Android forwards
session/coverage identity. Logs show player/worker state and include collection
waits in job time. See [qualification](../ai/GAMEPLAY_QUALIFICATION.md).
