#pragma once
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>

namespace graphics::vulkan::internal {
    class Device;
    class QueueFamilies;

    class CommandPool final {
        VkCommandPool m_pool = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE; // Device used for pool creation

    public:
        CommandPool(Device& device, QueueFamilies const& queueFamilies);
        ~CommandPool();

        PURE VkCommandPool get() const noexcept { return m_pool; }
    };
} // namespace graphics::vulkan::internal
