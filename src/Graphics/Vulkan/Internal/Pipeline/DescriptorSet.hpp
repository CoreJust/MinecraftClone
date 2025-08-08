#pragma once
#include <Core/Macro/Attributes.hpp>
#include "../Wrapper/Handles.hpp"
#include "../Wrapper/Resource.hpp"
#include "DescriptorType.hpp"
#include "ShaderStageBit.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;

    class DescriptorSet final : public StandaloneResource<VkDescriptorSetLayout> {
        using ParentResource = StandaloneResource<VkDescriptorSetLayout>;

        u32                   m_binding;
        u32                   m_count;
        DescriptorType        m_type;
        ShaderStageBit        m_stages;

    public:
        DescriptorSet(Vulkan& vulkan, DescriptorType type, ShaderStageBit stages, u32 binding, u32 count = 1);

        PURE DescriptorType        type  () const noexcept { return m_type; }
        PURE ShaderStageBit        stages() const noexcept { return m_stages; }
    };
} // namespace graphics::vulkan::internal
