#pragma once

#include <core/common/Assert.hpp>
#include <core/meta/Enum.hpp>
#include <core/vulkan/VulkanFwd.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace core {

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

CORE_ENUM_FUNCTIONS(QueueFamily);

[[nodiscard]]
constexpr std::optional<uint32_t> queueFamilyFlagBitOf(QueueFamily const family) noexcept {
    if (family == QueueFamily::Present || family == QueueFamily::Count) {
        return std::nullopt;
    }
    return 1 << static_cast<uint32_t>(family);
}

struct QueueFamilies final {
    static constexpr uint32_t NO_INDEX = static_cast<uint32_t>(-1);

    std::array<uint32_t, static_cast<size_t>(QueueFamily::Count)> indices;

    [[nodiscard]]
    static QueueFamilies query(VkPhysicalDevice const device, VkSurfaceKHR const surface);

    constexpr QueueFamilies() noexcept { indices.fill(NO_INDEX); }

    [[nodiscard]]
    constexpr uint32_t& operator[](QueueFamily const family) noexcept {
        ASSERT(static_cast<size_t>(family) < static_cast<size_t>(QueueFamily::Count));
        return indices[static_cast<size_t>(family)];
    }

    [[nodiscard]]
    constexpr uint32_t operator[](QueueFamily const family) const noexcept {
        ASSERT(static_cast<size_t>(family) < static_cast<size_t>(QueueFamily::Count));
        return indices[static_cast<size_t>(family)];
    }
};

} // namespace core
