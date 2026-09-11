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

`PresentationResourceScope` retains the context device but becomes stale after
every recreation, even where extent and format happen to match. The renderer's
pre-recreate hook invalidates its cache using that scope's device identity,
releases the programs, layouts, and scope; its post-recreate hook obtains a
fresh scope and rebuilds format-dependent objects. This also gives
`waitForSubmittedFrames()` a bounded frame-slot drain without
`vkDeviceWaitIdle`. There is no second Client device and no steady per-draw
allocation.

## Scene and shader policy

```text
World players -> PlayerClient / AndroidPlayerClient -> PlayerRenderData span
  -> VulkanRenderer -> PresentationContext::Frame callback -> present/readback
```

The renderer clears dark, draws the 32-by-32 ground grid first, then one
coloured 2-by-2-by-2 player box for each `PlayerRenderData`. A client-local
right-handed Z-up camera supplies a resize-safe Vulkan zero-to-one perspective
projection; the initial eye is `(16, -20, 22)` with pitch `-35` degrees.
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

`DebugHudState` formats a bounded, allocation-free diagnostic line and packs
four sanitized ASCII bytes into each instance word. `debug_hud.vert` expands
those instances into procedural 8-by-16 quads; `debug_hud.frag` owns the
texture-free bitmap constants and performs the factor-of-eight glyph-row
addressing. The renderer submits all packed words with one instanced draw
through the same-device `GraphicsProgram` and `VulkanKernelCache` used by the
scene.

Desktop and Android supply the current local character's authoritative X/Y and
plane-derived Z (`0` while the world remains flat), plus camera yaw/pitch/roll
in degrees. The HUD labels that coordinate as `XYZ(plane)` so it cannot be
mistaken for camera-eye elevation. The renderer overwrites the presentation bit from its
own successful `PresentationContext::complete` result, so dropped or
non-presented frames do not enter the one-second FPS window. The public
`setDebugHudEnabled`/`toggleDebugHud` controls support performance measurements.

## Capture, input, and validation

When capture is requested, the context enables transfer-source presentation and
the renderer consumes `takeCompletedReadback()` after submission. Captured bytes
are an owned RGBA8 vector returned only to the caller; ordinary frames allocate
neither draw data nor readback storage. Context recreation, window resize, and
Android native-window replacement preserve this contract.

Desktop input is supplied by `RuntimePlatformGlfw::GlfwWindow`; PlayerClient
uses GLFW cursor deltas for yaw/pitch and maps W/A/S/D camera-relative intent
through `CameraController` into the unchanged cardinal authoritative
`Direction` packet. Escape, R, and F1 remain window input without recreating a
local GLFW state layer. A
debounced F1 key-down toggles the HUD; holding F1 does not retrigger it. R
retains its existing debounced shader-hot-reload action. Android uses the same
F1 edge contract through `AndroidInput`.
Android keeps left-half drag movement intent and uses right-half drag deltas
for yaw/pitch before the same controller lowering; it retains its asset and
`AndroidInput` glue while delegating only Vulkan surface/device/presentation
ownership.

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
immediate negotiation. Evidence records mode, HUD state, and CPU acquire, record, and
complete timings—not GPU timestamps. Release disables validation;
`--hud` supports paired runs. Missing CoreGraphics display metadata stays
unavailable.

Visible acceptance benchmark and capture modes keep their requested resolutions
in framebuffer pixels. Their shared bounded GLFW setup converts the current
pixel-to-logical scale into a checked logical resize before creating a
`PresentationContext`; an exact framebuffer mismatch, no resize progress, or
an oscillating adjustment fails rather than changing the recorded request.
