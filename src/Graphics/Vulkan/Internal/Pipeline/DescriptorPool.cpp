#include "DescriptorPool.hpp"
#include <vulkan/vulkan.h>
#include "../Vulkan/Vulkan.hpp"

namespace graphics::vulkan::internal {
    DescriptorPool::DescriptorPool(Vulkan& vulkan, core::ArrayView<DescriptorOptions const> options, u32 setsCount) 
        : Parent     (vulkan)
        , m_setsCount(setsCount)
    {
        core::DynArray<VkDescriptorPoolSize> poolSizes {
            options.size(),
            [&](usize i) {
                return VkDescriptorPoolSize {
                    .type            = static_cast<VkDescriptorType>(options[i].type),
                    .descriptorCount = options[i].count * m_setsCount,
                };
            },
        };
        
        VkDescriptorPoolCreateInfo pool_info { };
        pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags         = 0;
        pool_info.maxSets       = setsCount;
        pool_info.poolSizeCount = static_cast<u32>(poolSizes.size());
        pool_info.pPoolSizes    = poolSizes.data();

        set(m_vulkan.create<vkCreateDescriptorPool>(&pool_info, nullptr));
    }
} // namespace graphics::vulkan::internal
