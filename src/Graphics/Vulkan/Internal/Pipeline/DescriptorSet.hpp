#pragma once
#include <Core/Macro/Attributes.hpp>
#include "../Wrapper/Handles.hpp"
#include "../Wrapper/Resource.hpp"
#include "DescriptorOptions.hpp"
#include "DescriptorPool.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;

    class DescriptorSetLayout final : public ResourceSet<VkDescriptorSetLayout> {
        DERIVED_RESOURCE_SET(VkDescriptorSetLayout);

    private:
        core::DynArray<DescriptorOptions> m_options;
        DescriptorPool                    m_pool;

    public:
        DescriptorSetLayout(Vulkan& vulkan, core::DynArray<DescriptorOptions> options, u32 maxSets);

        PURE core::ArrayView<DescriptorOptions const> options() const noexcept { return m_options.cview(); }
    };
} // namespace graphics::vulkan::internal
