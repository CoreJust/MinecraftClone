#pragma once

#include <core/common/Assert.hpp>
#include <core/meta/Enum.hpp>
#include <core/vulkan/internal/VulkanFwd.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace core::vk {

enum class QueueFamily {
    Graphics,
    Compute,
    Transfer,
    SparseBinding,
    Protected,
    VideoDecode,
    VideoEncode,
    
    // Note that it doesn't map to any flag, so must be handled separately
    Present,

    Count,
};

[[nodiscard]]
constexpr std::optional<uint32_t> queueFamilyFlagBitOf(QueueFamily const family) noexcept {
    if (family == QueueFamily::Present || family == QueueFamily::Count) {
        return std::nullopt;
    }
    return 1 << indexOf(family);
}

struct QueueFamilies final {
    static constexpr uint32_t NO_INDEX = static_cast<uint32_t>(-1);

    std::array<uint32_t, countOf<QueueFamily>()> indices;

    [[nodiscard]]
    static QueueFamilies query(VkPhysicalDevice const device, VkSurfaceKHR const surface);

    constexpr QueueFamilies() noexcept { indices.fill(NO_INDEX); }

    [[nodiscard]]
    constexpr uint32_t& operator[](QueueFamily const family) noexcept {
        ASSERT(indexOf(family) < countOf<QueueFamily>());
        return indices[indexOf(family)];
    }

    [[nodiscard]]
    constexpr uint32_t operator[](QueueFamily const family) const noexcept {
        ASSERT(indexOf(family) < countOf<QueueFamily>());
        return indices[indexOf(family)];
    }
};

} // namespace core::vk

CORE_ENUM_FUNCTIONS(::core::vk::QueueFamily);
