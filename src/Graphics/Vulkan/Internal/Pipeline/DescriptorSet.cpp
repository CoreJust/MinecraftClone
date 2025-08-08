#include "DescriptorSet.hpp"
#include <vulkan/vulkan.h>
#include "../Vulkan/Vulkan.hpp"

namespace graphics::vulkan::internal {
    DescriptorSet::DescriptorSet(Vulkan& vulkan, DescriptorType type, ShaderStageBit stages, u32 binding, u32 count)
        : StandaloneResource<VkDescriptorSetLayout>(vulkan)
        , m_binding(binding)
        , m_count  (count)
        , m_type   (type)
        , m_stages (stages)
    {
        VkDescriptorSetLayoutBinding layoutBinding { };
        layoutBinding.binding            = m_binding;
        layoutBinding.descriptorType     = static_cast<VkDescriptorType>(m_type);
        layoutBinding.descriptorCount    = m_count;
        layoutBinding.stageFlags         = static_cast<VkShaderStageFlags>(m_stages);
        layoutBinding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutCreateInfo layoutInfo { };
        layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.pNext        = nullptr;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings    = &layoutBinding;

        set(m_vulkan.create<vkCreateDescriptorSetLayout>(&layoutInfo, nullptr));
    }
} // namespace graphics::vulkan::internal
