#pragma once
#include <optional>
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>

namespace graphics::vulkan::internal {
    enum class QueueType {
        Graphics,
        Compute,
        Transfer,

        QueueTypesCount,
    };

    VkQueueFlagBits queueTypeToVk(QueueType type) noexcept;

    class QueueFamilies final {
        std::optional<uint32_t> m_indices[static_cast<size_t>(QueueType::QueueTypesCount)];

    public:
        QueueFamilies() noexcept = default;
        QueueFamilies(VkPhysicalDevice device);
        QueueFamilies(QueueFamilies const&) noexcept = default;
        QueueFamilies(QueueFamilies&&) noexcept = default;
        QueueFamilies& operator=(QueueFamilies const&) noexcept = default;
        QueueFamilies& operator=(QueueFamilies&&) noexcept = default;

        PURE bool hasFamily(QueueType type) const noexcept { return m_indices[static_cast<size_t>(type)].has_value(); }
        PURE uint32_t getIndex(QueueType type) const noexcept { return m_indices[static_cast<size_t>(type)].value(); }
    };
} // namespace graphics::vulkan::internal
