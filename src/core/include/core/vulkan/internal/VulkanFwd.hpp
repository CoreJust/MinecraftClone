#pragma once

#include <cstdint>

using VkBool32 = uint32_t;
using VkDeviceAddress = uint64_t;
using VkDeviceSize = uint64_t;
using VkFlags = uint32_t;
using VkSampleMask = uint32_t;

using VkMemoryPropertyFlags = VkFlags;
using VkSampleCountFlags = VkFlags;
using VkMemoryPropertyFlags = VkFlags;
using VkMemoryHeapFlags = VkFlags;

#ifndef VK_FALSE
#  define VK_FALSE 0u
#endif
#ifndef VK_TRUE
#  define VK_TRUE 1u
#endif
#ifndef VK_MAX_MEMORY_TYPES
#  define VK_MAX_MEMORY_TYPES 32u
#endif
#ifndef VK_MAX_MEMORY_HEAPS
#  define VK_MAX_MEMORY_HEAPS 16u
#endif
#ifndef VK_MAX_PHYSICAL_DEVICE_NAME_SIZE
#  define VK_MAX_PHYSICAL_DEVICE_NAME_SIZE 256u
#endif
#ifndef VK_UUID_SIZE
#  define VK_UUID_SIZE 16uc
#endif

// All declarations as per Vulkan spec
#if defined(__LP64__)                                \
    || defined(_WIN64)                               \
    || (defined(__x86_64__) && !defined(__ILP32__) ) \
    || defined(_M_X64)                               \
    || defined(__ia64)                               \
    || defined(_M_IA64)                              \
    || defined(__aarch64__)                          \
    || defined(__powerpc64__)                        \
    || (defined(__riscv) && __riscv_xlen == 64)
#  define DECL_VK_HANDLE(type) using Vk##type = struct Vk##type##_T*;
#  define DECL_VMA_HANDLE(type) using Vma##type = struct Vma##type##_T*;
#  define VK_NULL_HANDLE nullptr
    using VkAnyHandle = void*;
#else
#  define DECL_VK_HANDLE(type) using Vk##type = uint64_t;
#  define DECL_VMA_HANDLE(type) using Vma##type = uint64_t;
#  define VK_NULL_HANDLE 0
    using VkAnyHandle = uint64_t;
#endif
#ifndef VMA_NULL
#   define VMA_NULL nullptr
#endif
    DECL_VMA_HANDLE(Allocation)
    DECL_VMA_HANDLE(Allocator)
    DECL_VK_HANDLE(Buffer)
    DECL_VK_HANDLE(BufferView)
    DECL_VK_HANDLE(CommandBuffer)
    DECL_VK_HANDLE(CommandPool)
    DECL_VK_HANDLE(DebugUtilsMessengerEXT)
    DECL_VK_HANDLE(DescriptorPool)
    DECL_VK_HANDLE(DescriptorSet)
    DECL_VK_HANDLE(DescriptorSetLayout)
    DECL_VK_HANDLE(Device)
    DECL_VK_HANDLE(DeviceMemory)
    DECL_VK_HANDLE(Event)
    DECL_VK_HANDLE(Fence)
    DECL_VK_HANDLE(Framebuffer)
    DECL_VK_HANDLE(Image)
    DECL_VK_HANDLE(ImageView)
    DECL_VK_HANDLE(Instance)
    DECL_VK_HANDLE(PhysicalDevice)
    DECL_VK_HANDLE(Pipeline)
    DECL_VK_HANDLE(PipelineCache)
    DECL_VK_HANDLE(PipelineLayout)
    DECL_VK_HANDLE(Queue)
    DECL_VK_HANDLE(RenderPass)
    DECL_VK_HANDLE(Sampler)
    DECL_VK_HANDLE(Semaphore)
    DECL_VK_HANDLE(ShaderModule)
    DECL_VK_HANDLE(SurfaceKHR)
    DECL_VK_HANDLE(SwapchainKHR)
#undef DECL_VK_HANDLE
