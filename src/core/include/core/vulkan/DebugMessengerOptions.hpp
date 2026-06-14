#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace core {

struct DebugMessengerOptions final {
    VkDebugUtilsMessageSeverityFlagsEXT severity_mask = 0
        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;

    VkDebugUtilsMessageTypeFlagsEXT type_mask = 0
        | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    PFN_vkDebugUtilsMessengerCallbackEXT callback = nullptr;
    void* user_data = nullptr;
};

class DebugMessengerOptionsBuilder final {
public:
    template<typename Self>
    [[nodiscard]] constexpr auto&& upToVerbose(this Self&& self) noexcept {
        self.m_options.severity_mask = 0
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] constexpr auto&& upToInfo(this Self&& self) noexcept {
        self.m_options.severity_mask = 0
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] constexpr auto&& upToWarn(this Self&& self) noexcept {
        self.m_options.severity_mask = 0
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] constexpr auto&& errorsOnly(this Self&& self) noexcept {
        self.m_options.severity_mask = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] constexpr auto&& captureVerbose(this Self&& self, bool const value = true) noexcept {
        if (value) {
            self.m_options.severity_mask |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
        } else {
            self.m_options.severity_mask &= ~VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
        }
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] constexpr auto&& captureInfo(this Self&& self, bool const value = true) noexcept {
        if (value) {
            self.m_options.severity_mask |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
        } else {
            self.m_options.severity_mask &= ~VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
        }
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] constexpr auto&& captureWarn(this Self&& self, bool const value = true) noexcept {
        if (value) {
            self.m_options.severity_mask |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        } else {
            self.m_options.severity_mask &= ~VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        }
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] constexpr auto&& captureErrors(this Self&& self, bool const value = true) noexcept {
        if (value) {
            self.m_options.severity_mask |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        } else {
            self.m_options.severity_mask &= ~VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        }
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] constexpr auto&& captureGeneral(this Self&& self, bool const value = true) noexcept {
        if (value) {
            self.m_options.type_mask |= VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        } else {
            self.m_options.type_mask &= ~VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        }
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] constexpr auto&& captureValidation(this Self&& self, bool const value = true) noexcept {
        if (value) {
            self.m_options.type_mask |= VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        } else {
            self.m_options.type_mask &= ~VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        }
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] constexpr auto&& capturePerformance(this Self&& self, bool const value = true) noexcept {
        if (value) {
            self.m_options.type_mask |= VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        } else {
            self.m_options.type_mask &= ~VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        }
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] constexpr auto&& withCallback(
        this Self&& self,
        PFN_vkDebugUtilsMessengerCallbackEXT const callback,
        void* const user_data = nullptr
    ) noexcept {
        self.m_options.callback = callback;
        self.m_options.user_data = user_data;
        return std::forward<Self>(self);
    }

    [[nodiscard]]
    constexpr DebugMessengerOptions build() const noexcept {
        return m_options;
    }
private:
    DebugMessengerOptions m_options{ };
};

} // namespace core
