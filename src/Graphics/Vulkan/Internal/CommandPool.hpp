#pragma once
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>
#include <Core/Collection/ArrayView.hpp>
#include "CommandBuffer.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;

    class CommandPool final {
        VkCommandPool m_pool   = VK_NULL_HANDLE;
        Vulkan&       m_vulkan;

    public:
        CommandPool(Vulkan& vulkan, uint32_t queueIndex);
        ~CommandPool();

        void allocate(core::collection::ArrayView<CommandBuffer> result);

        PURE VkCommandPool get() const noexcept { return m_pool; }
    };
} // namespace graphics::vulkan::internal
