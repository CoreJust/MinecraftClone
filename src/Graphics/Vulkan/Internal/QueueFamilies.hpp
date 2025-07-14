#pragma once
#include <optional>
#include <Core/Macro/Attributes.hpp>
#include <Core/Common/Int.hpp>
#include "QueueType.hpp"

namespace graphics::vulkan::internal {
    class Surface;
    class PhysicalDevice;

    class QueueFamilies final {
        std::optional<u32> m_indices[static_cast<usize>(QueueType::QueueTypesCount)];

    public:
        QueueFamilies() noexcept = default;
        QueueFamilies(PhysicalDevice const& device, Surface const& surface);
        QueueFamilies(QueueFamilies const&) noexcept = default;
        QueueFamilies(QueueFamilies&&) noexcept = default;
        QueueFamilies& operator=(QueueFamilies const&) noexcept = default;
        QueueFamilies& operator=(QueueFamilies&&) noexcept = default;

        PURE bool     hasFamily(QueueType type) const noexcept { return m_indices[static_cast<usize>(type)].has_value(); }
        PURE u32 getIndex (QueueType type) const noexcept { return m_indices[static_cast<usize>(type)].value(); }
    };
} // namespace graphics::vulkan::internal
