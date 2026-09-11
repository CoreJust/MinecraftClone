# Vulkan rendering

## Boundary and ownership

[`VulkanRenderer.hpp`](../../src/client/include/client/render/VulkanRenderer.hpp)
is the Client policy facade. It owns scene draw order, grid/player push
constants, shader choice, camera/world interpretation, and raw device children
(pipeline layouts and CoreCpp program/cache lifetimes). CoreCpp
`RuntimeKernel::GraphicsProgram` validates the Client's reflected vertex and
fragment entrypoints, and `RuntimeGraphicsVulkan::VulkanKernelCache` owns the
corresponding shader modules and pipelines. The Client does not own a Vulkan
instance, physical device, device, queues, command pools, synchronization
objects, swapchain, image views, or GLFW/Android surface.

Those objects belong to one CoreCpp
`RuntimeGraphicsVulkan::PresentationContext`. Desktop constructs it using
`RuntimePlatformGlfw` and `RuntimeGraphicsVulkanGlfw`; Android constructs the
same contract through `RuntimeGraphicsVulkanAndroid`. All Client device
children use the current `PresentationResourceScope::device()`, format, and
extent, and the acquired `Frame::imageView()` is the only render target. The
Client records only into `Frame::commandBuffer()` via the frame callback;
CoreCpp performs the acquire/transition/submit/present sequence.

`PresentationResourceScope` becomes stale on recreation. Pre-recreate releases
the cache, programs, layouts, and scope; post-recreate builds format-dependent
objects from a fresh scope. `waitForSubmittedFrames()` drains bounded frame
slots without `vkDeviceWaitIdle`; there is no steady per-draw allocation.

## Scene and shader policy

```text
World players -> PlayerClient / AndroidPlayerClient -> PlayerRenderData span
  -> VulkanRenderer -> PresentationContext::Frame callback -> present/readback
```

Renderer clears dark, draws the 32-by-32 ground grid first, then coloured
2-by-2-by-2 remote-player boxes. Each authoritative local-player update sets
the eye to `(x+1, y+1, 1.6)` without changing its angles and omits that local
cube. A right-handed Z-up camera supplies a zero-to-one projection with Y
flipped for its positive-height viewport. The flip reverses winding, so
grid/player/HUD triangles are reversed for the CoreCpp CCW back-face policy.
`GridPushConstants` and `PlayerPushConstants` are both 96 bytes and carry the
projection-view matrix, so their ABI must remain compatible with the GLSL push
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
ASCII bytes into each instance word. `debug_hud.vert` expands
those instances into procedural 8-by-16 quads; `debug_hud.frag` owns the
texture-free bitmap constants and performs the factor-of-eight glyph-row
addressing. The renderer submits all packed words with one instanced draw
through the same-device `GraphicsProgram` and `VulkanKernelCache` used by the
scene.

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

`renderer_golden_tests.cpp` is an opt-in true offscreen production-renderer
capture of the fixed 640-by-480 S4 scene. It creates no GLFW window, Vulkan
surface, or swapchain and remains valid with display environment variables
unset. `RuntimeGraphicsVulkan::VulkanOffscreenTarget` owns the fixed
`R8G8B8A8_UNORM` linear color target, command submission, and readback; the
Client owns its selected depth target through the recording lifetime. The
offscreen callback invokes the exact same grid/player scene-recording routine,
shader programs, camera transforms, and depth-enabled `VulkanKernelCache`
pipelines as presentation. It requires validation during strict approval and
checks both exact `flat3d-v2` bytes and a near-box-over-later-far pixel
predicate. Test-only `mc_test_support` writes bounded actual/diff diagnostics;
the versioned reference may change only after deliberate native-size review,
never automatically.

`RendererSmokeTest` remains a separate visible GLFW presentation/recreate and
readback smoke. It is useful evidence for presentation ownership and input but
is not a substitute for the display-independent offscreen golden.

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
