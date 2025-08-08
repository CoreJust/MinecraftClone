#pragma once
#include "../Internal/Pipeline/DescriptorType.hpp"
#include "../Internal/Pipeline/ShaderStageBit.hpp"

namespace graphics::vulkan::pipeline {
    using DescriptorType = internal::DescriptorType;
    using ShaderStageBit = internal::ShaderStageBit;

    struct Descriptor final {
        DescriptorType type;
        ShaderStageBit stages;
    };
} // namespace graphics::vulkan::pipeline
