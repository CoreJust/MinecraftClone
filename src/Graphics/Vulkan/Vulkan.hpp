#pragma once
#include <cstdint>

namespace graphics::vulkan {
    namespace internal {
        class Instance;
        class PhysicalDevice;
        class Device;
        class Queue;
    }

    class Vulkan final {
        internal::Instance* m_instance = nullptr;
        internal::PhysicalDevice* m_physicalDevice = nullptr;
        internal::Device* m_device = nullptr;
        internal::Queue* m_graphicsQueue = nullptr;

    public:
        Vulkan() noexcept = default;
        Vulkan(char const* const appName, char const** windowRequiredExtensions, uint32_t windowRequiredExtensionsCount);
        Vulkan(Vulkan&&) noexcept = default;
        Vulkan& operator=(Vulkan&&) noexcept = default;
        ~Vulkan();
    };
} // namespace graphics::vulkan
