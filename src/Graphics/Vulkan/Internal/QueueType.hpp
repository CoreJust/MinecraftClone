#pragma once

namespace graphics::vulkan::internal {
    enum class QueueType {
        Graphics,
        Present,
        Compute,
        Transfer,

        QueueTypesCount,
    };
} // namespace graphics::vulkan::internal
