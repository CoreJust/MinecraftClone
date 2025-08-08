#include "Resource.hpp"
#include <vulkan/vulkan.hpp>
#include "../Vulkan/Vulkan.hpp"

namespace graphics::vulkan::internal {
#define DECL_RELEASE(resource) \
    template<>                 \
    void Resource<resource>::release(Vulkan& vulkan, core::ArrayView<resource> items, void* allocator) noexcept

    DECL_RELEASE(VkDeviceMemory) {
        for (VkDeviceMemory mem : items)
            vulkan.destroy<vkFreeMemory>(mem, reinterpret_cast<VkAllocationCallbacks*>(allocator));
    }

    DECL_RELEASE(VkBuffer) {
        for (VkBuffer buffer : items)
            vulkan.destroy<vkDestroyBuffer>(buffer, reinterpret_cast<VkAllocationCallbacks*>(allocator));
    }

    DECL_RELEASE(VkDescriptorSetLayout) {
        for (VkDescriptorSetLayout dsl : items)
            vulkan.destroy<vkDestroyDescriptorSetLayout>(dsl, reinterpret_cast<VkAllocationCallbacks*>(allocator));
    }
#undef DECL_RELEASE
} // namespace graphics::vulkan::internal
