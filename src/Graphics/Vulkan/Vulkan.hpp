#pragma once
#include <cstdint>
#include <Core/Memory/UniquePtr.hpp>
#include "RenderPipeline.hpp"

namespace graphics::vulkan {
    namespace internal {
        class Instance;
        class Surface;
        class PhysicalDevice;
        class Device;
        class Queue;
        class Swapchain;
        class CommandPool;
    }

    class Vulkan final {
        core::memory::UniquePtr<internal::Instance> m_instance;
        core::memory::UniquePtr<internal::Surface> m_surface;
        core::memory::UniquePtr<internal::PhysicalDevice> m_physicalDevice;
        core::memory::UniquePtr<internal::Device> m_device;
        core::memory::UniquePtr<internal::Queue> m_graphicsQueue;
        core::memory::UniquePtr<internal::Queue> m_presentQueue;
        core::memory::UniquePtr<internal::Swapchain> m_swapchain;
        core::memory::UniquePtr<internal::CommandPool> m_commandPool;

    public:
        Vulkan() noexcept = default;
        Vulkan(void* window, void* surfaceCreator, char const* const appName, char const** windowRequiredExtensions, uint32_t windowRequiredExtensionsCount);
        Vulkan(Vulkan&&) noexcept = default;
        Vulkan& operator=(Vulkan&&) noexcept = default;

        PURE RenderPipeline createPipeline(char const* const vertexShaderPath, char const* const fragmentShaderPath);
    };
} // namespace graphics::vulkan
