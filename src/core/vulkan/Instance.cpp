#include <core/vulkan/Instance.hpp>

#include <volk.h>

namespace core {

void RawInstance::Destroyer::operator()(RawInstance& instance) {
    if (instance.m_debug_messenger != nullptr) {
        vkDestroyDebugUtilsMessengerEXT(instance.m_instance, instance.m_debug_messenger, nullptr);
        instance.m_debug_messenger = nullptr;
    }
    if (instance.m_instance != nullptr) {
        vkDestroyInstance(instance.m_instance, nullptr);
        instance.m_instance = nullptr;
    }
}

} // namespace core
