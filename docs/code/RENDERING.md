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
texture. Flight uses a first-person camera at the authoritative local player
position and draws remote players only. Mesh uploads occur only when content
identity changes. The HUD reports authoritative Flight XYZ, including altitude,
and camera yaw/pitch/roll in degrees. The camera remains right-handed Z-up with
zero-to-one depth and the existing depth-tested scene policy.

Shader assets are borrowed: desktop `InstalledShaderAssets` reads the installed
`shaders/` directory and Android `AndroidShaderAssets` reads APK assets. Both
accept bare `.spv` names only; no source-tree fallback is allowed.

S6 arena has 65,536 faces, grows to the 45-radius cap, and restores meshes
out-of-frame. Stone samples the attributed 16-by-16
texture from a buffer, with distance fog blending into a gradient sky. Frustum
culling affects drawing only; tile residency follows a camera-independent
45-tile-radius circle around the player. View and movement direction affect
generation and meshing priority only. Mesh construction runs on
worker threads, outside the presentation callback.
[`height_tile_surface_mesher_tests.cpp`](../../tests/core/height_tile_surface_mesher_tests.cpp)
covers flat tiles, exposed height differences, neighbor seams, and generated
column tops.

## Text and GUI boundary

`TextRenderer` lays out dynamically sized strings and RGBA color spans as glyph
instances. Placement supports screen coordinates or a world origin with right
and up basis vectors, so text is not tied to the HUD's five-line format.
`text.vert` expands glyph instances into quads; `text.frag` samples the
attributed bitmap. The current bitmap covers printable ASCII and substitutes a
fallback glyph for unsupported characters.
UTF-8 input consumes one fallback glyph per unsupported code point. Presentation
accepts up to 16,384 glyphs across world and GUI labels in a frame and reports
an unsuccessful render before frame acquisition when that budget is exceeded.

World text is submitted with depth testing in the world scene. `GuiRenderer`
owns screen-space text and records it in a separate color-load GUI stage after
the world scene, without depth testing. `DebugHudState` formats five lines and
submits them through that GUI API. The HUD presents gameplay Z as user-facing Y
(height), with separated coordinate and angle values.
`VulkanRenderer` exposes set/add/clear calls for GUI and world labels; add calls
allow multiple labels with independent colors and placements in one frame.

The renderer counts only frames whose `PresentationContext::complete` succeeds.

## Capture, input, and validation

Capture enables transfer-source presentation and returns an owned RGBA8 vector
after submission. Readback allocation occurs only on capture; recreation,
resize, and Android window replacement retain the capture contract.

Desktop GLFW cursor deltas control yaw/pitch; W/A/S/D lower through
`CameraController` into unchanged authoritative `Direction` packets. Escape,
R, and debounced F1 remain window input; R reloads shaders and F1 toggles HUD.
Android maps left drag to movement and right drag to yaw/pitch before the same
lowering while retaining its asset and `AndroidInput` glue.

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
