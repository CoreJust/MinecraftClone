#pragma once
#include <Core/Common/Int.hpp>
#include "DescriptorType.hpp"
#include "ShaderStageBit.hpp"

namespace graphics::vulkan::internal {
    struct DescriptorOptions final {
        u32            binding;
        u32            count;
        DescriptorType type;
        ShaderStageBit stages;
    };
} // namespace graphics::vulkan::internal
