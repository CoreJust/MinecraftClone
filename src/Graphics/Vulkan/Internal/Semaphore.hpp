#pragma once
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>

namespace graphics::vulkan::internal {
    class Vulkan;

    class Semaphore final {
        VkSemaphore m_semaphore = VK_NULL_HANDLE;
        Vulkan& m_vulkan;

    public:
        Semaphore(Vulkan& vulkan);
        ~Semaphore();

        PURE VkSemaphore get() const noexcept { return m_semaphore; }
    };
} // namespace graphics::vulkan::internal
