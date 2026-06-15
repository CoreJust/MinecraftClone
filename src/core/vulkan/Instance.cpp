#include <core/vulkan/Instance.hpp>

#include <volk.h>

namespace core {

VKC_RESOURCE_DESTROY_IMPL(RawInstance) {
    if (self.m_debug_messenger != nullptr) {
        vkDestroyDebugUtilsMessengerEXT(self.m_handle, self.m_debug_messenger, nullptr);
    }
    vkDestroyInstance(self.m_handle, nullptr);
}

} // namespace core
