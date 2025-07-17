#pragma once
#include <Core/Common/Int.hpp>

using VkPhysicalDeviceProperties = struct VkPhysicalDeviceProperties;

// All declarations as per Vulkan spec
#if defined(__LP64__) || defined(_WIN64) || (defined(__x86_64__) && !defined(__ILP32__) ) || defined(_M_X64) || defined(__ia64) || defined (_M_IA64) || defined(__aarch64__) || defined(__powerpc64__) || (defined(__riscv) && __riscv_xlen == 64)
#  define DECL_VK_HANDLE(type) using Vk##type = struct Vk##type##_T*;
#  define VK_NULL_HANDLE nullptr
#else
#  define DECL_VK_HANDLE(type) using Vk##type = u64;
#  define VK_NULL_HANDLE 0
#endif
    DECL_VK_HANDLE(Instance)
    DECL_VK_HANDLE(DebugUtilsMessengerEXT)
    DECL_VK_HANDLE(PhysicalDevice)
    DECL_VK_HANDLE(Device)
    DECL_VK_HANDLE(SurfaceKHR)
    DECL_VK_HANDLE(SwapchainKHR)
    DECL_VK_HANDLE(Image)
    DECL_VK_HANDLE(ImageView)
    DECL_VK_HANDLE(Semaphore)
    DECL_VK_HANDLE(Fence)
    DECL_VK_HANDLE(CommandPool)
    DECL_VK_HANDLE(CommandBuffer)
    DECL_VK_HANDLE(Queue)
    DECL_VK_HANDLE(Framebuffer)
    DECL_VK_HANDLE(ShaderModule)
    DECL_VK_HANDLE(RenderPass)
    DECL_VK_HANDLE(PipelineLayout)
    DECL_VK_HANDLE(Pipeline)
#undef DECL_VK_HANDLE
