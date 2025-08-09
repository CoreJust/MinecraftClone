#pragma once
#include <Core/Macro/Attributes.hpp>
#include "../Wrapper/Handles.hpp"
#include "../Wrapper/Resource.hpp"
#include "DescriptorOptions.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;

    class DescriptorPool final : public StandaloneResource<VkDescriptorPool> {
        DERIVED_STANDALONE_RESOURCE(VkDescriptorPool);

        u32 m_setsCount;
        u32 m_alocatedSets = 0;

    public:
        DescriptorPool(Vulkan& vulkan, core::ArrayView<DescriptorOptions const> options, u32 setsCount = 1);



        PURE u32 allocatedSets() const noexcept { return m_alocatedSets; }
        PURE u32 setsCount    () const noexcept { return m_setsCount; }
    };
} // namespace graphics::vulkan::internal
