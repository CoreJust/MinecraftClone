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

Renderer clears to a light sky blue, draws a depth-tested 32-by-32 slate
platform volume from `z=-1.0` through `z=0.0` (an exact one-unit visible
thickness), then its subdued grid top and coloured 2-by-2-by-2 player boxes.
The platform draw omits its coplanar top triangles; a single non-overlapping
grid-material draw owns the complete `z=0` surface, fills every cell with slate,
and fades its thin borders toward the surface colour with distance. This keeps
strict `LESS` depth comparison without an epsilon or coplanar overlay. Player
faces use restrained per-face shading so the silhouette remains readable
against the platform and sky. Each authoritative
local-player update targets the player center `(x+1, y+1, 1)` and places a
close third-person camera six units behind and above it using the current
yaw/pitch; cursor and touch orbit recompute that position every frame. Both
the local and remote cubes are submitted. A right-handed Z-up camera supplies
a zero-to-one projection with Y flipped for its positive-height viewport. The
flip reverses winding, so grid/player/HUD triangles are reversed for the
CoreCpp CCW back-face policy.
`GridPushConstants` is 96 bytes and `BoxPushConstants` is 112 bytes; both carry
the projection-view matrix, while the box block also carries independent XYZ
origin and extent vectors. Their ABI must remain compatible with the GLSL push
blocks. The grid and boxes use a same-scope depth attachment selected
deterministically from `D32_SFLOAT`, then `D16_UNORM`, with the CC-0014
explicit `LESS` depth test/write pipeline state. Each freshly
created attachment receives an explicit `UNDEFINED` to
`DEPTH_STENCIL_ATTACHMENT_OPTIMAL` barrier before the first dynamic-rendering
pass; resize destruction releases view, image, and memory before its resource
scope. The portable `RuntimeKernel::SpirvModule` boundary
currently accepts vertex/fragment stages, so both desktop and Android use
`grid.vert`, `player.vert`, and `trivial.frag`; mesh modules remain assets but
are not a selected Client pipeline.

Shader assets are borrowed: desktop `InstalledShaderAssets` reads the installed
`shaders/` directory and Android `AndroidShaderAssets` reads APK assets. Both
accept bare `.spv` names only; no source-tree fallback is allowed.

## Debug HUD boundary

`DebugHudState` defaults off for benchmark/capture and normal gameplay enables
it. It formats four bounded allocation-free lines and packs four sanitized
ASCII bytes into each instance word. The coordinate and angle rows use padded
groups (`XYZ:  1.25   2.50   0.00` and `YPR deg: 45.0  -10.0  3.0`).
`debug_hud.vert` expands those instances into
procedural 16-by-32 quads, twice the bitmap's native 8-by-16 glyph size, with
a 40-pixel row advance. Its DPI scale is capped by the presentation extent so
all four rows remain top-left and non-overlapping at high scale. `debug_hud.frag`
owns the texture-free bitmap constants and performs the factor-of-eight
glyph-row addressing. The renderer submits all packed words with one instanced
draw through the same-device `GraphicsProgram` and `VulkanKernelCache` used by
the scene.

Desktop and Android supply the current local character's authoritative X/Y and
plane-derived Z (`0` while the world remains flat), plus camera yaw/pitch/roll
as `Y/P/R(deg):yaw,pitch,roll`. The HUD labels this `XYZ`; its plane-derived Z
is documented here, not camera-eye elevation. The renderer overwrites the presentation bit from its
own successful `PresentationContext::complete` result, so dropped or
non-presented frames do not enter the one-second FPS window. The public
`setDebugHudEnabled`/`toggleDebugHud` controls support performance measurements.

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
