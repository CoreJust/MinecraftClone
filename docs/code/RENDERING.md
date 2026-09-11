# Vulkan rendering

## Boundary and ownership

[`VulkanRenderer.hpp`](../../src/client/include/client/render/VulkanRenderer.hpp)
is the Client policy facade. It owns scene draw order, grid/player push
constants, shader choice, camera/world interpretation, and raw device children
(shader modules, layouts, and pipelines). It does not own a Vulkan instance,
physical device, device, queues, command pools, synchronization objects,
swapchain, image views, or GLFW/Android surface.

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
pre-recreate hook destroys all raw device children and releases the scope;
its post-recreate hook obtains a fresh scope and rebuilds format-dependent
objects. This also gives `waitForSubmittedFrames()` a bounded frame-slot drain
without `vkDeviceWaitIdle`. There is no second Client device and no steady
per-draw allocation.

## Scene and shader policy

```text
World players -> PlayerClient / AndroidPlayerClient -> PlayerRenderData span
  -> VulkanRenderer -> PresentationContext::Frame callback -> present/readback
```

The renderer clears dark, draws the 32-by-32 grid first, then one coloured
2-by-2 player quad for each `PlayerRenderData`. `GridPushConstants` and
`PlayerPushConstants` are both 32 bytes and must remain ABI-compatible with
the GLSL push blocks. The portable `RuntimeKernel::SpirvModule` boundary
currently accepts vertex/fragment stages, so both desktop and Android use
`grid.vert`, `player.vert`, and `trivial.frag`; mesh modules remain assets but
are not a selected Client pipeline.

Shader assets are borrowed: desktop `InstalledShaderAssets` reads the installed
`shaders/` directory and Android `AndroidShaderAssets` reads APK assets. Both
accept bare `.spv` names only; no source-tree fallback is allowed.

## Capture, input, and validation

When capture is requested, the context enables transfer-source presentation and
the renderer consumes `takeCompletedReadback()` after submission. Captured bytes
are an owned RGBA8 vector returned only to the caller; ordinary frames allocate
neither draw data nor readback storage. Context recreation, window resize, and
Android native-window replacement preserve this contract.

Desktop input is supplied by `RuntimePlatformGlfw::GlfwWindow`; PlayerClient
maps W/A/S/D, Escape, and R without recreating a local GLFW state layer.
Android retains its asset and `AndroidInput` glue while delegating only Vulkan
surface/device/presentation ownership.

[`renderer_smoke_tests.cpp`](../../tests/client/renderer_smoke_tests.cpp), when
enabled with `MC_ENABLE_RENDERER_SMOKE`, deterministically tests the GLFW
input adapter and runs the real presentation/context recreate/readback case on
a Vulkan-capable desktop. The latter is a hardware smoke, not a replacement for
normal CTest. Android package compilation links the exact installed Android
RuntimeGraphics components; emulator presentation remains separate runtime
acceptance.
