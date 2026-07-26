#pragma once

#include <core/vulkan/enum/VulkanEnum.hpp>

namespace core::vk {

enum class AttachmentLoadOp {
    Load,
    Clear,
    DontCare,
    None,

    Count,
};

enum class AttachmentStoreOp {
    Store,
    DontCare,
    None,

    Count,
};

struct AttachmentOps final {
    AttachmentLoadOp load = AttachmentLoadOp::Load;
    AttachmentStoreOp store = AttachmentStoreOp::Store;
};

} // namespace core::vk

CORE_VK_REGISTER_ENUM(AttachmentLoadOp, { None, 1'000'400'000 });
CORE_VK_REGISTER_ENUM(AttachmentStoreOp, { None, 1'000'301'000 });
