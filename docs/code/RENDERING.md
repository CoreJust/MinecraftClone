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

## Debug HUD boundary

`DebugHudState` defaults off for benchmark/capture and normal gameplay enables
it. It formats four bounded allocation-free lines and packs four sanitized
ASCII bytes into each instance word. Coordinate and angle rows use padded
groups (`XYZ: 1.25 2.50 0.00`, `YPR deg: 45.0 -10.0 3.0`).
`debug_hud.vert` expands those instances into
procedural 16-by-32 quads, twice the bitmap's native 8-by-16 glyph size, with
a 40-pixel row advance. Its DPI scale is capped by the presentation extent so
all four rows remain top-left and non-overlapping at high scale. `debug_hud.frag`
owns the texture-free bitmap constants and performs the factor-of-eight
glyph-row addressing. The renderer submits all packed words with one instanced
draw through the same-device `GraphicsProgram` and `VulkanKernelCache` used by
the scene.

Desktop and Android supply authoritative local Flight XYZ, including Z altitude,
plus camera yaw/pitch/roll as `Y/P/R(deg)`. The renderer counts only frames whose
`PresentationContext::complete` succeeds; public HUD controls support benchmark
comparisons.

## Capture, input, and validation

Capture enables transfer-source presentation and returns an owned RGBA8 vector
after submission. Ordinary frames allocate neither draw data nor readback;
recreation, resize, and Android window replacement preserve that contract.

Desktop GLFW cursor deltas control yaw/pitch; W/A/S/D lower through
`CameraController` into unchanged authoritative `Direction` packets. Escape,
R, and debounced F1 remain window input; R reloads shaders and F1 toggles HUD.
Android maps left drag to movement and right drag to yaw/pitch before the same
lowering while retaining its asset and `AndroidInput` glue.

[`renderer_smoke_tests.cpp`](../../tests/client/renderer_smoke_tests.cpp), when
enabled with `MC_ENABLE_RENDERER_SMOKE`, deterministically tests the GLFW
input adapter and runs the real presentation/context recreate/readback case on
a Vulkan-capable desktop. The latter is a hardware smoke, not a replacement for
normal CTest. Android package compilation links the exact installed Android
RuntimeGraphics components; emulator presentation remains separate runtime
acceptance.

`renderer_golden_tests.cpp` captures the fixed 640-by-480
EarlyDev 0.1.0 snapshot 4 scene without GLFW, a surface, or a swapchain. Its
oblique regression exercises yaw `37.2` and pitch `-35`, plus the opposite yaw.
`VulkanOffscreenTarget` owns its linear color target, submission, and readback;
Client owns depth and invokes the same scene recorder and pipelines as
presentation. Strict approval enables validation and checks the active
versioned reference plus independent depth/scene predicates. Diagnostics are
bounded and a reference changes only after deliberate native-size review.

`RendererSmokeTest` separately covers visible GLFW presentation, recreation,
readback, input, and the default-on HUD.

The S5 offscreen stone acceptance captures the cached chunk from above and
below, verifies textured stone and sky pixels, checks that repeated identical
meshes produce identical RGBA8 frames, and checks that replacing the mesh
changes the frame without validation errors. Its altitude case verifies that a
remote player's rendered position moves upward when authoritative Z increases.

The benchmark uses shared renderer options; `--present-immediate` fails without
immediate negotiation. Release evidence records negotiated mode, actual pixel
resolution, scene, HUD state, sustained rate, p50/p95/p99/max, and stutters;
it records CPU acquire, record, and complete timings—not GPU timestamps.
Release disables validation; `--hud` supports paired runs. Missing CoreGraphics
display metadata stays unavailable.

Visible acceptance benchmark and capture modes keep their requested resolutions
in framebuffer pixels. Their shared bounded GLFW setup converts the current
pixel-to-logical scale into a checked logical resize before creating a
`PresentationContext`; an exact framebuffer mismatch, no resize progress, or
an oscillating adjustment fails rather than changing the recorded request.
Capture additionally requires a completed transfer readback with the requested
extent and both sky and textured stone content. Benchmark additionally requires
a presented frame, a nonzero stone draw, and a stable cached mesh upload count;
it records p50/p95/p99, maximum, mean, acquire, command-record, and completion
wait timings within its monotonic deadline.
