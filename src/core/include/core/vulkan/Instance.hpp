#pragma once

#include <core/vulkan/Resource.hpp>

namespace core::vk {

class RawInstance : public VulkanResourceBase<VkInstance> {
    CORE_VK_RESOURCE_CONTEXT(RawInstance);
    CORE_VK_RESOURCE_CONSTRUCTION_FROM(
        VkInstance const instance,
        VkDebugUtilsMessengerEXT const debug_messenger = VK_NULL_HANDLE
    ) {
        CORE_VK_CAPTURE_DESTRUCTION_CONTEXT(){ };
        self.m_handle = instance;
        self.m_debug_messenger = debug_messenger;
    }
public:
    [[nodiscard]]
    VkDebugUtilsMessengerEXT debugMessenger() const noexcept { return m_debug_messenger; }
private:
    VkDebugUtilsMessengerEXT m_debug_messenger = VK_NULL_HANDLE;
};

using Instance = VulkanRaii<RawInstance>;

} // namespace core::vk
