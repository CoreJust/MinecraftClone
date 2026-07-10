#pragma once

#include <core/vulkan/enum/AccessFlag.hpp>
#include <core/vulkan/enum/ImageAspect.hpp>
#include <core/vulkan/enum/ImageLayout.hpp>
#include <core/vulkan/enum/PipelineStage.hpp>

namespace core::vk {

struct ImageMemoryBarrier final {
    PipelineStages src_stages = PipelineStages::None;
    PipelineStages dst_stages = PipelineStages::None;
    AccessFlags src_access = AccessFlags::None;
    AccessFlags dst_access = AccessFlags::None;
    ImageLayout old_layout = ImageLayout::Undefined;
    ImageLayout new_layout = ImageLayout::Undefined;
    ImageAspectBits aspect = ImageAspectBits::of(ImageAspect::Color);
    uint32_t base_mip_level = 0;
    uint32_t level_count = 1;
    uint32_t base_array_layer = 0;
    uint32_t layer_count = 1;
};

} // namespace core::vk
