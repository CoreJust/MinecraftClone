#pragma once
#include <Core/Macro/Attributes.hpp>
#include "../Wrapper/Handles.hpp"
#include "../Wrapper/Resource.hpp"
#include "DescriptorType.hpp"
#include "ShaderStageBit.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;

    class DescriptorSet final : public ResourceSet<VkDescriptorSetLayout> {
        PARENT_RESOURCE_SET(VkDescriptorSetLayout);

    public:
        struct DescriptorOptions final {
            u32            binding;
            u32            count;
            DescriptorType type;
            ShaderStageBit stages;
        };

    private:
        core::DynArray<DescriptorOptions> m_options;

    public:
        DescriptorSet(Vulkan& vulkan, core::DynArray<DescriptorOptions> options);

        PURE core::ArrayView<DescriptorOptions const> options() const noexcept { return m_options.cview(); }
    };
} // namespace graphics::vulkan::internal
