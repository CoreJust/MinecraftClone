#pragma once
#include <optional>
#include <Core/Macro/Attributes.hpp>
#include "QueueType.hpp"

namespace graphics::vulkan::internal {
    class Surface;
    class PhysicalDevice;

    class QueueFamilies final {
        std::optional<uint32_t> m_indices[static_cast<size_t>(QueueType::QueueTypesCount)];

    public:
        QueueFamilies() noexcept = default;
        QueueFamilies(PhysicalDevice const& device, Surface const& surface);
        QueueFamilies(QueueFamilies const&) noexcept = default;
        QueueFamilies(QueueFamilies&&) noexcept = default;
        QueueFamilies& operator=(QueueFamilies const&) noexcept = default;
        QueueFamilies& operator=(QueueFamilies&&) noexcept = default;

        PURE bool     hasFamily(QueueType type) const noexcept { return m_indices[static_cast<size_t>(type)].has_value(); }
        PURE uint32_t getIndex (QueueType type) const noexcept { return m_indices[static_cast<size_t>(type)].value(); }
    };
} // namespace graphics::vulkan::internal
