# Vulkan rendering

## Scope and checkout state

This note describes the current working tree. Its renderer changes are staged
or unstaged local work where shown by `git status`; they are not evidence that
the behavior is released. The current source requests Vulkan 1.2; actual
capabilities and selected shader path determine driver requirements.

## Public boundary and ownership

[VulkanRenderer.hpp](../../src/client/include/client/render/VulkanRenderer.hpp)
defines a non-copyable, non-movable façade. `PlayerClient` owns it after its
window, passes a span of `PlayerRenderData` every frame, and calls `hotReload()`
when R changes from released to pressed. The façade owns `Impl` with a
`unique_ptr`; `Impl` owns the frame graph, imported swapchain pass, shader
modules, layouts, and pipelines.

Construction in [VulkanRenderer.cpp](../../src/client/render/VulkanRenderer.cpp)
builds a context for the supplied window, requests API version 1.2,
portability enumeration, dynamic rendering and synchronization2 extensions/features,
graphics and present queues, and a swapchain. Validation is required in debug
builds, with `MC_ENABLE_VULKAN_VALIDATION_LAYERS`, or when options explicitly
request it; an ordinary release build does not require validation layers.
Mesh shaders are preferred, not required. `VulkanRendererOptions` can force
vertex pipelines even when mesh support is enabled in the context.
Failure to satisfy a required context condition prevents normal renderer
construction; no alternative renderer exists.

## Per-frame data flow

```text
World players -> PlayerClient::render -> PlayerRenderData span
  -> VulkanRenderer::render -> FrameGraph pass -> swapchain
       grid pipeline first, then one player draw per record
```

The renderer imports the swapchain with a dark clear color and registers one
pass that writes it. Each `render` binds that pass, pushes constants, records
the grid then player commands, and invokes `FrameGraph::render()`. The grid
always draws 32 by 32 cells; every player produces a 2 by 2 colored quad. The
coordinate mapping uses 32 world units in each axis, so the renderer assumes
the shared world dimensions are 32. Changing `World::WIDTH` or `HEIGHT`
requires changing the C++ constants and shader constants/push data together.

`GridPushConstants` and `PlayerPushConstants` are both asserted as 32 bytes.
Their field order and alignment must remain ABI-compatible with their GLSL
push-constant blocks. The selected main stage is mesh when mesh capability is
available and vertex otherwise; the same stage is used for pipeline creation
and `pushConstants`.

## Shader sets

The source list in [src/client/CMakeLists.txt](../../src/client/CMakeLists.txt)
is compiled by [`mc_target_shaders`](../../cmake/Helpers.cmake) into `.spv`
files. `mc_copy_target_shaders` copies them beside each consuming executable
under `shaders/`; installation places `mc_main` and that directory together.
[ShaderAssets.cpp](../../src/client/render/ShaderAssets.cpp) resolves the actual
Windows/macOS executable path, independently of working directory/configuration.
It accepts only a bare `.spv` filename and reports missing files. There is no
environment override or source-tree fallback. Preserve this layout when packaging.

| Purpose | Preferred shader | Fallback | Contract |
| --- | --- | --- | --- |
| Grid | [grid.mesh](../../src/client/render/shaders/grid.mesh) | [grid.vert](../../src/client/render/shaders/grid.vert) | 32 × 32 quads with a 0.03-cell inset and the grid push block |
| Player | [player.mesh](../../src/client/render/shaders/player.mesh) | [player.vert](../../src/client/render/shaders/player.vert) | one 2-triangle quad using origin, size, and RGBA push data |
| Color | [trivial.frag](../../src/client/render/shaders/trivial.frag) | — | forwards the interpolated color |

Both geometry paths use the same four corners and two triangles. Mesh shaders
produce four vertices/two primitives per workgroup; vertex fallbacks issue six
vertices, with the grid using one instance per cell. Keep locations and
push-constant layouts synchronized across each pair and the C++ structs.

## Reload and teardown

`hotReload()` requests an instance-level frame-graph reload. The registered
reload callback destroys pipelines/layouts/modules on `Destroy` and recreates
them otherwise. Destruction first waits for the device to become idle, then
releases the same graphics objects. The renderer therefore expects pipeline
creation to be repeatable and all currently selected SPIR-V paths to exist at
reload time.

## Extension points and limits

- Add world geometry by adding a shader/pipeline and recording it in the
  existing render pass with explicit data/layout contracts.
- Add per-player attributes by extending `PlayerRenderData`, the matching C++
  push struct, both player shader variants, and their reflected layouts.
- [Shader asset tests](../../tests/client/shader_assets_tests.cpp) load all five
  compiled shaders from a temporary working directory and reject missing/invalid paths.
- Optional [renderer smoke](../../tests/client/renderer_smoke_tests.cpp), enabled
  by `MC_ENABLE_RENDERER_SMOKE`, requires a desktop, GPU and validation layers.
  It draws six frames, including empty/player data and an instance reload,
  through automatic and forced-vertex paths. CTest rejects validation/error logs.
  Automatic selection does not prove mesh execution on a non-mesh device.
- Smoke does not inspect pixels, resize, or user controls; visible scene and
  interaction acceptance still need separate evidence.
