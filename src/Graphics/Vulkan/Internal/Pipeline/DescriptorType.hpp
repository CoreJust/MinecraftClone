#pragma once

namespace graphics::vulkan::internal {
    enum class DescriptorType {
        Sampler,
        CombinedImageSampler,
        SampledImage,
        StorageImage,
        UniformTexelBuffer,
        StorageTexelBuffer,
        UniformBuffer,
        StorageBuffer,
        UniformBufferDynamic,
        StorageBufferDynamic,
        InputAttachment,
        
        // Since Vulkan 1.3 or with VK_EXT_inline_uniform_block
        InlineUniformBlock = 1000138000,
    };
} // namespace graphics::vulkan::internal
