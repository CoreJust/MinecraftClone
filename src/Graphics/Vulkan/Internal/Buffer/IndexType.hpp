#pragma once

namespace graphics::vulkan::internal {
    enum class IndexType {
        Uint16 = 0,
        Uint32 = 1,
        Uint8 = 1000265000, // Since Vulkan 1.4 or from VK_EXT_index_type_uint8 or from VK_KHR_index_type_uint8
    };
} // namespace graphics::vulkan::internal
