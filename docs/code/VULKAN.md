# Vulkan subsystem guide

Checkout guide, not release evidence. Headers live in
[`src/core/include/core/vulkan`](../../src/core/include/core/vulkan), implementations
in [`src/core/vulkan`](../../src/core/vulkan). Use
[`VulkanContext`](../../src/core/include/core/vulkan/Context.hpp) or
[`FrameGraph`](../../src/core/include/core/vulkan/FrameGraph.hpp) from gameplay.

## Resource model and lifetime

[`Resource.hpp`](../../src/core/include/core/vulkan/Resource.hpp) is the core
ownership contract. `Raw*` types are trivially copyable handles plus destruction
context; `VulkanRaii<Raw*>` owns destruction. `grabRaw()` transfers ownership;
`raw()` copies only the handle/context. Destroyers capture required parent
handles, so destroy children before parents. Do not retain a raw handle after its
owner dies. Batch resources use `VulkanRaiiVector` and retain that wrapper.

- [`Instance`](../../src/core/include/core/vulkan/Instance.hpp),
  [`Surface`](../../src/core/include/core/vulkan/Surface.hpp),
  [`Device`](../../src/core/include/core/vulkan/Device.hpp), and selected
  [`PhysicalDevice`](../../src/core/include/core/vulkan/PhysicalDevice.hpp)
  establish the parent chain. A physical device is selected, not destroyed.
  `Queue` is a borrowed device queue with its semantic family.
- [`Allocator`](../../src/core/include/core/vulkan/Allocator.hpp) owns VMA;
  allocated [`Image`](../../src/core/include/core/vulkan/Image.hpp) owns both
  `VkImage` and allocation, while swapchain images are borrowed and must not be
  VMA-destroyed. Image views own their Vulkan view and retain device destruction
  context. Preserve image format, extent, aspect, and usage in `Image::Info`.
- Synchronization, command, shader and pipeline wrappers own device children;
  retain them until GPU use ends. Reset/begin/end command buffers only in their
  legal lifecycle; resettable buffers require the matching pool flag.

## Construction and capabilities

[`VulkanContextBuilder`](../../src/core/include/core/vulkan/builder/ContextBuilder.hpp)
coordinates instance, selection, device, and swapchain builders.
Configure before construction or `VulkanContext::rebuild`. Builders validate
requirements and throw typed errors; query enabled
[`VulkanCaps`](../../src/core/include/core/vulkan/Capabilities.hpp) before use.
Repeated required/preferred extension calls append, including empty calls;
they do not clear earlier window/swapchain requirements. ContextBuilder routes
instance/device extensions through explicit `InputSpan`s. This accumulation
contract does not imply every other builder setter accumulates.

- [`InstanceBuilder`](../../src/core/include/core/vulkan/builder/InstanceBuilder.hpp)
  configures API/validation/extensions/layers/debug messenger.
  [`PhysicalDeviceSelector`](../../src/core/include/core/vulkan/builder/PhysicalDeviceSelector.hpp)
  filters properties, extensions, features, and queue requirements.
- [`DeviceBuilder`](../../src/core/include/core/vulkan/builder/DeviceBuilder.hpp)
  enables selected queues/extensions/features. [`AllocatorBuilder`](../../src/core/include/core/vulkan/builder/AllocatorBuilder.hpp)
  creates VMA after device selection. [`SwapchainBuilder`](../../src/core/include/core/vulkan/builder/SwapchainBuilder.hpp)
  chooses format, present mode, extent, image count, sharing, transform, and usage.
- [`Extensions`](../../src/core/include/core/vulkan/Extensions.hpp),
  [`Features`](../../src/core/include/core/vulkan/Features.hpp),
  [`Layers`](../../src/core/include/core/vulkan/Layers.hpp), and
  [`Capabilities`](../../src/core/include/core/vulkan/Capabilities.hpp) are the
  supported/enabled record. `has` checks enabled state;
  `hasExtensionOrPromoted` also accepts the recorded API promotion version.
  Dynamic rendering/synchronization2 promote in Vulkan 1.3. Feature enablement
  remains distinct from extension availability.

## Context, swapchain, and reload

`VulkanContext` owns instance → surface → physical device → device → swapchain
and three frame slots. It creates graphics command objects, semaphores, and
initially signaled fences for a windowed surface. Call `waitIdle()` before
destroying resources that could be in flight.

`reload(type)` waits idle, announces `Destroy`, tears down from that scope,
rebuilds, then announces `Recreate`. `rebuild(fn, type)` mutates its stored
builder first. `type` must be the highest mutated scope: using `Swapchain` for
an instance/device edit silently leaves that edit unapplied. Out-of-date/surface
loss during acquire invokes error-driven reload and returns no frame.

`Swapchain` owns its handle/views and borrows images. Current image/view validity
starts at successful acquire and ends at the next frame/reload.

## Per-frame recording and synchronization

The canonical sequence is:

1. `auto frame = ctx.acquireFrame();` and skip rendering when it is empty.
   It waits the slot fence, acquires an image, resets/begins that slot's command
   buffer, then returns a noncopyable/nonmovable `FrameContext`.
2. Record barriers and rendering through that `FrameContext`. Its destructor
   ends the command buffer, submits graphics work waiting at color-output on
   `imageAvailable`, signals `renderFinished`, presents, and advances the slot.
   It must end before the context; only one live frame context is supported.
3. Use `RenderScope scope = frame->acquireRenderScope(...)` for each rendering
   region. Its destructor ends dynamic rendering or the render pass. Do not
   manually end it, nest it, or destroy the frame while a scope remains alive.

[`ImageMemoryBarrier`](../../src/core/include/core/vulkan/ImageMemoryBarrier.hpp)
describes layout/stage/access/subresource transitions. `FrameContext` chooses
Synchronization2 when enabled; otherwise flags must fit legacy 32-bit masks or
it throws `FrameContextError`. Callers maintain old layout, access, and queue
ownership.

Dynamic rendering begin/end and Synchronization2 barriers dispatch to KHR
commands when their extensions are enabled, otherwise to core commands on the
promoted path. A feature bit alone does not select the correct function name.

## Attachments, render paths, and frame graph

[`Attachment`](../../src/core/include/core/vulkan/Attachment.hpp) expresses
color/depth/stencil/input/resolve/preserve use through stable
`AttachmentViewId`s. Bind every referenced ID in `AttachmentViewProvider`; views
  need compatible format/extent/sample count. The context chooses dynamic
  rendering only when supported and there are
no input/preserve/depth-stencil-resolve attachments; otherwise it caches Vulkan
render passes/framebuffers internally. More color resolves than colors is an
explicit context error. Relative viewport/scissor are scaled to attachment
extent.

[`FrameGraph`](../../src/core/include/core/vulkan/FrameGraph.hpp) owns a moved
context and declarative passes. Add/import changes discard the built graph;
`render()` lazily builds dependencies, views, barriers, and pass artifacts. Each
pass needs a callback. Default bindings are one-frame; use
`PersistentFramePassBindCallback::Yes` across renders. `discard()` frees cached
artifacts. The only current resource kind is swapchain import; a new kind also
needs internal lifetime/barrier rules.

## Pipelines, shader input, and errors

[`SpirV`](../../src/core/include/core/vulkan/SpirV.hpp) loads binary words from
a file; malformed size/I/O failures throw. `GraphicsPipelineOptions` owns the
declarative shader stages, vertex input, rasterization, depth/stencil, blending,
dynamic state, and render-target compatibility used to create a graphics
pipeline. Supply a mesh or vertex stage, ensure stage/module counts agree, and
match attachment formats/sample counts to the actual render path. Build layouts
from SPIR-V reflection with `PipelineLayout::Info::fromSpirVs` when applicable;
descriptor/push-constant declarations remain pipeline ABI.

`VK_CHECK` dispatches expected recoverable results (out-of-date, suboptimal,
device/surface/memory loss) through callbacks and returns whether handled.
Typed `VulkanError` subclasses cover construction, frame, graph, allocation,
and pipeline failures. An empty acquire is a reload boundary; propagate other
typed errors rather than using partially created handles.

## Threading, extension, and tests

No wrapper locking. Serialize reload, frame acquisition, recording,
and destruction; respect Vulkan external synchronization for each
device/queue/command pool. Change error/reload callbacks outside frame work.

For new wrappers, define destruction context, add CMake sources and prove
parent-before-child teardown. For
a capability, add enum/mapping/query, builder validation/enablement, and a caps
test. Extension accumulation has dedicated
[`instance builder`](../../tests/core/vulkan/instance_builder_tests.cpp) and
[`physical device selector`](../../tests/core/vulkan/physical_device_selector_tests.cpp)
regressions. Other coverage includes
[`capabilities_tests.cpp`](../../tests/core/vulkan/capabilities_tests.cpp),
[`extensions_tests.cpp`](../../tests/core/vulkan/extensions_tests.cpp), and
[`spirv_tests.cpp`](../../tests/core/vulkan/spirv_tests.cpp). Frame/context,
lifetime, and presentation still need Vulkan-host validation; optional
[renderer smoke](RENDERING.md) exercises draw/reload and rejects validation logs,
but does not establish every frame-graph or resource-lifetime contract.
