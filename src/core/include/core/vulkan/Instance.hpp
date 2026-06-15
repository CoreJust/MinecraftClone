#pragma once

#include <core/vulkan/Resource.hpp>

#include <cstdint>
#include <utility>

namespace core {

class RawInstance : public VulkanResourceBase<VkInstance> {
    VKC_RESOURCE_CONTEXT(RawInstance);
    VKC_RESOURCE_CONSTRUCTION_FROM(
        VkInstance const instance,
        VkDebugUtilsMessengerEXT const debug_messenger = VK_NULL_HANDLE
    ) {
        VKC_CAPTURE_DESTRUCTION_CONTEXT(){ };
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

} // namespace core
