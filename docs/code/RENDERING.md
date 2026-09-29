# Vulkan rendering

## Boundary and ownership

[`VulkanRenderer.hpp`](../../src/client/include/client/render/VulkanRenderer.hpp)
owns Client scene order, push constants, shaders, camera policy, pipeline
layouts, and program/cache lifetimes. CoreCpp validates programs and owns Vulkan
instance/device, queues, commands, synchronization, swapchain, views, and
GLFW/Android surface integration through one `PresentationContext`.

Client device children use the current `PresentationResourceScope`; it records
only into an acquired frame callback. Recreation releases cache, programs,
layouts, and the stale scope before rebuilding. `waitForSubmittedFrames()`
drains bounded slots without steady-state `vkDeviceWaitIdle` or draw allocation.

## Scene and shader policy

```text
World players -> PlayerClient / AndroidPlayerClient -> PlayerRenderData span
  -> VulkanRenderer -> PresentationContext::Frame callback -> present/readback
```

The platform/grid/player scene is legacy flat-world Snapshot 4 coverage. Normal
Flight clears to sky blue, submits cached exposed faces from the deterministic
seed-42 16-by-16-by-16 air-and-stone chunk, and uses the original 16-by-16 stone
texture. Flight uses a local first-person, rear third-person, or front-facing
third-person camera at the sampled player position. The display camera is
separate from the look camera that lowers input, so switching views cannot
reverse movement. First person omits the local single-parallelepiped body;
both third-person modes draw it. Bodies use the server-replicated four-bit
palette identity and the 16-color opaque presentation table. Mesh uploads
occur only when content identity changes. The HUD reports authoritative Flight
XYZ, including altitude, and camera yaw/pitch/roll in degrees. The camera
remains right-handed Z-up with zero-to-one depth and the existing depth-tested
scene policy.

Shader assets are borrowed: desktop `InstalledShaderAssets` reads the installed
`shaders/` directory and Android `AndroidShaderAssets` reads APK assets. Both
accept bare `.spv` names only; no source-tree fallback is allowed.

The arena grows to the 45-radius cap; restoration shares the frame deadline.
Gameplay bounds uploads and rendering to 8 ms, deferring only the overdue tile
for retry. Stone
uses the attributed texture with fog and gradient sky. Frustum culling affects
drawing only; residency is camera-independent. View/movement affect generation
priority only, and workers build meshes outside the presentation callback.
[`height_tile_surface_mesher_tests.cpp`](../../tests/core/height_tile_surface_mesher_tests.cpp)
covers flat tiles, exposed height differences, neighbor seams, and generated
column tops.

`ChunkMeshLodBuilder` creates bounded `Fine` and `Coarse` variants from a
revisioned mesh. Coarse quads merge adjacent coplanar faces only when direction
and material match; both variants retain content identity and full-chunk bounds.
`MeshLodSelector` accepts caller-supplied squared-distance thresholds, applies
hysteresis, and falls back to an available variant. The caller derives the
cutoffs; this API does not hardcode a chunk distance. Building is explicit, so
per-frame selection does not remesh. Invalid face directions are rejected
before grid indexing. [`chunk_mesh_lod_tests.cpp`](../../tests/core/chunk_mesh_lod_tests.cpp)
covers these contracts.

## Text and GUI boundary

`TextRenderer` lays out colored glyph instances in screen or world space.
`text.vert` expands quads and `text.frag` samples the ASCII bitmap, using one
fallback glyph per unsupported UTF-8 code point. Presentation accepts 16,384
world/GUI glyphs before frame acquisition fails.

World text is depth tested; `GuiRenderer` records screen text after the world
scene without depth. `DebugHudState` formats five lines, presenting gameplay Z
as user-facing height. `VulkanRenderer` supports independent GUI/world labels.

The renderer counts only frames whose `PresentationContext::complete` succeeds.

## Capture, input, and validation

Capture enables transfer-source presentation and returns an owned RGBA8 vector
after submission. Readback allocation occurs only on capture; recreation,
resize, and Android window replacement retain the capture contract.

Desktop GLFW cursor deltas control yaw/pitch; W/A/S/D lower through
`CameraController` into unchanged authoritative `Direction` packets. Escape,
R, debounced F1, and debounced F5 remain window input; R reloads shaders, F1
toggles HUD, and F5 cycles camera perspective. Android maps left drag to
movement and right drag to yaw/pitch before the same lowering while retaining
its asset and `AndroidInput` glue; its hardware F5 input uses the same cycle.

[`renderer_smoke_tests.cpp`](../../tests/client/renderer_smoke_tests.cpp)
tests GLFW input and presentation/recreation/readback on a Vulkan desktop.
Android package compilation and emulator presentation remain separate gates.

`renderer_golden_tests.cpp` captures the fixed 640-by-480
EarlyDev 0.1.0 snapshot 4 scene without GLFW, a surface, or a swapchain. Its
oblique regression exercises yaw `37.2` and pitch `-35`, plus the opposite yaw.
`VulkanOffscreenTarget` owns its linear color target, submission, and readback;
Client owns depth and invokes the same scene recorder and pipelines as
presentation. Strict approval enables validation and checks the active
versioned reference plus independent depth/scene predicates. Diagnostics are
bounded and a reference changes only after deliberate native-size review.
An offscreen HUD regression draws through the GUI stage and checks top-left
pixels without a window.

`RendererSmokeTest` separately covers visible GLFW presentation, recreation,
readback, input, and the default-on HUD.

S5 offscreen acceptance verifies textured stone, sky, deterministic frames,
mesh replacement, validation, and remote-player altitude.

The benchmark fails if requested immediate presentation is unavailable. Evidence
includes mode, resolution, scene, HUD, rate, p50/p95/p99/max, stutters, and CPU
phase timings, but no GPU timestamps.
Release disables validation; `--hud` supports paired runs. The S6 benchmark uses
the maximum 90-by-90 capacity as a rendering stress case, evicts and uploads
bounded edge work across all axes, and includes streaming work in frame timings
to expose stalls. Normal gameplay uses the smaller directional ellipse documented
in [GAMEPLAY.md](GAMEPLAY.md).
It requires the nominal 120 Hz presentation rate within a one-percent host-clock
measurement tolerance and p99 at or below 10 ms, including the display wait and
scheduler jitter around the 8.33 ms deadline.

Visible benchmark and capture modes preserve requested framebuffer pixels.
Bounded GLFW setup checks logical resizing before creating a `PresentationContext`;
extent mismatch or oscillation fails. Capture requires completed readback, sky,
and textured stone. Benchmark requires presentation and a stone draw, and
records frame and phase timing within its deadline. The opt-in networked
playtest capture launches the production server and player client, waits for
1,024 streamed meshes, rotates 90 degrees toward the central peak, proves that
the resident-key set is identical, then validates the real framebuffer's terrain
coverage, texture variation, HUD visibility, completion time, and process health.
