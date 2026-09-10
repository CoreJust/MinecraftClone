#pragma once

#include <core/vulkan/enum/VulkanEnum.hpp>

namespace core::vk {

enum class PresentMode {
    Immediate,
    Mailbox,
    FIFO,
    FIFORelaxed,

    Count,
};

} // namespace core::vk

CORE_VK_REGISTER_ENUM(PresentMode);
