#pragma once

#include <core/common/EnumBits.hpp>
#include <core/vulkan/enum/VulkanEnum.hpp>

namespace core::vk {

enum class ImageAspect {
    Color,
    Depth,
    Stencil,
    Metadata,
    Plain0,
    Plain1,
    Plain2,
    MemoryPlain0,
    MemoryPlain1,
    MemoryPlain2,
    MemoryPlain3,

    Count,
};

using ImageAspectBits = EnumBits<ImageAspect>;

} // namespace core::vk

CORE_VK_REGISTER_ENUM(ImageAspect);
