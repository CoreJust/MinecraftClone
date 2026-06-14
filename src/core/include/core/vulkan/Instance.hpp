#pragma once

#include <core/vulkan/Handles.hpp>
#include <core/vulkan/Raii.hpp>

#include <cstdint>
#include <utility>

struct VkInstance_T;
struct VkDebugUtilsMessengerEXT_T;

namespace core {

using VkInstance = VkInstance_T*;
using VkDebugUtilsMessengerEXT = VkDebugUtilsMessengerEXT_T*;

class RawInstance {
public:
    struct Destroyer final {
        void operator()(RawInstance& instance);
    };
public:
    RawInstance() noexcept = default;
    explicit RawInstance(
        VkInstance const instance,
        VkDebugUtilsMessengerEXT const debug_messenger = VK_NULL_HANDLE
    ) noexcept
        : m_instance(instance)
        , m_debug_messenger(debug_messenger)
    { }

    explicit RawInstance(
        VkInstance const instance,
        VkDebugUtilsMessengerEXT const debug_messenger,
        Destroyer
    ) noexcept : RawInstance(instance, debug_messenger) { }

    [[nodiscard]]
    VkInstance handle() const noexcept { return m_instance; };
    [[nodiscard]]
    VkDebugUtilsMessengerEXT debugMessenger() const noexcept { return m_debug_messenger; }
private:
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debug_messenger = VK_NULL_HANDLE;
};

using Instance = VulkanRaii<RawInstance>;

} // namespace core
