#pragma once
#include <Core/Macro/Attributes.hpp>
#include "../Wrapper/Handles.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;

    class Semaphore final {
        VkSemaphore m_semaphore = VK_NULL_HANDLE;
        Vulkan& m_vulkan;

    public:
        Semaphore(Vulkan& vulkan);
        ~Semaphore();

        PURE VkSemaphore  get() const noexcept { return m_semaphore; }
        PURE VkSemaphore* ptr()       noexcept { return &m_semaphore; }
    };
} // namespace graphics::vulkan::internal
