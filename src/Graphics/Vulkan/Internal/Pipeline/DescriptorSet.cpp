#include "DescriptorSet.hpp"
#include <vulkan/vulkan.h>
#include <Core/Algorithm/Zip.hpp>
#include "../Vulkan/Vulkan.hpp"

namespace graphics::vulkan::internal {
    DescriptorSet::DescriptorSet(Vulkan& vulkan, core::DynArray<DescriptorOptions> options)
        : Parent   (vulkan, options.size())
        , m_options(core::move(options))
    {
        for (auto& [layout, options] : m_resources | core::zip(m_options)) {
            VkDescriptorSetLayoutBinding layoutBinding { };
            layoutBinding.binding            = options.binding;
            layoutBinding.descriptorType     = static_cast<VkDescriptorType>(options.type);
            layoutBinding.descriptorCount    = options.count;
            layoutBinding.stageFlags         = static_cast<VkShaderStageFlags>(options.stages);
            layoutBinding.pImmutableSamplers = nullptr;

            VkDescriptorSetLayoutCreateInfo layoutInfo { };
            layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layoutInfo.pNext        = nullptr;
            layoutInfo.bindingCount = 1;
            layoutInfo.pBindings    = &layoutBinding;

            layout = vulkan.create<vkCreateDescriptorSetLayout>(&layoutInfo, nullptr);
        }
    }
} // namespace graphics::vulkan::internal
