set(LOCAL_SOURCES
    Check.cpp
    Framebuffers.cpp
    Wrapper/Resource.cpp
    Vulkan/Functions.cpp
    Vulkan/ErrorCallbacks.cpp
    Vulkan/Layers.cpp
    Vulkan/Extensions.cpp
    Vulkan/Instance.cpp
    Vulkan/Surface.cpp
    Vulkan/PhysicalDevice.cpp
    Vulkan/Device.cpp
    Vulkan/Frame.cpp
    Vulkan/Swapchain.cpp
    Vulkan/SwapchainSupport.cpp
    Vulkan/Vulkan.cpp
    Sync/Semaphore.cpp
    Sync/Fence.cpp
    Pipeline/AttributeFormat.cpp
    Pipeline/ShaderModule.cpp
    Pipeline/RenderPass.cpp
    Pipeline/Pipeline.cpp
    Command/QueueFamilies.cpp
    Command/Queue.cpp
    Command/CommandPool.cpp
    Command/CommandBuffer.cpp
    Memory/VMA.cpp
    Memory/Allocator.cpp
    Memory/MappedMemory.cpp
    Buffer/Buffer.cpp
    Buffer/CopyBuffer.cpp)
set(LOCAL_TESTS)

SUBMODULE(Graphics/Vulkan/Internal)
