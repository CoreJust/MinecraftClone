#pragma once

#include <core/common/SpanUtils.hpp>
#include <core/common/Version.hpp>
#include <core/meta/Enum.hpp>
#include <core/vulkan/Capabilities.hpp>
#include <core/vulkan/DebugMessengerOptions.hpp>
#include <core/vulkan/Error.hpp>
#include <core/vulkan/Extensions.hpp>
#include <core/vulkan/Instance.hpp>
#include <core/vulkan/Layers.hpp>

#include <array>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace core {

CORE_VK_ERROR_WITH_KINDS(InstanceCreationError,
    GlfwVulkanNotSupported,
    VolkInitializationFailed,
    UnsupportedApiVersion,
    InvalidApiVersionRange,
    MissingRequiredLayer,
    MissingRequiredExtension,
    WrongExtensionScope,
    ValidationUnavailable,
    DebugUtilsUnavailable,
    InstanceCreationFailed,
    DebugMessengerCreationFailed);

class InstanceBuilder final {
public:
    template<typename Self>
    [[nodiscard]] auto&& requireWindowExtensions(this Self&& self) {
        self.m_require_window_extensions = true;
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& requireVersion(this Self&& self, Version const version) {
        self.m_required_version = version;
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& preferVersion(this Self&& self, Version const version) {
        self.m_preferred_version = version;
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& requireValidation(
        this Self&& self,
        bool const enabled = true,
        DebugMessengerOptionsBuilder const& options_builder = {},
        std::function<void()> const& failure_callback = {}
    ) {
        self.m_require_validation = enabled;
        self.m_prefer_validation = enabled;
        self.m_debug_messenger_options = options_builder.build();
        self.m_validation_failure_callback = failure_callback;
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& preferValidation(
        this Self&& self,
        bool const enabled = true,
        DebugMessengerOptionsBuilder const& options_builder = {},
        std::function<void()> const& failure_callback = {}
    ) {
        self.m_prefer_validation = enabled;
        self.m_debug_messenger_options = options_builder.build();
        self.m_validation_failure_callback = failure_callback;
        return std::forward<Self>(self);
    }

    template<typename Self, typename First, typename... Tail>
    [[nodiscard]] auto&& requireExtensions(
        this Self&& self,
        First const first,
        Tail const... rest
    ) {
        SpanOver span_over_data{ first, rest... };
        self.m_required_extensions.insert(
            self.m_required_extensions.end(),
            span_over_data.asSpan().begin(),
            span_over_data.asSpan().end()
        );
        return std::forward<Self>(self);
    }

    template<typename Self, typename First, typename... Tail>
    [[nodiscard]] auto&& preferExtensions(
        this Self&& self,
        First const first,
        Tail const... rest
    ) {
        SpanOver span_over_data{ first, rest... };
        self.m_preferred_extensions.insert(
            self.m_preferred_extensions.end(),
            span_over_data.asSpan().begin(),
            span_over_data.asSpan().end()
        );
        return std::forward<Self>(self);
    }

    template<typename Self, typename First, typename... Tail>
    [[nodiscard]] auto&& requireLayers(
        this Self&& self,
        First const first,
        Tail const... rest
    ) {
        SpanOver span_over_data{ first, rest... };
        self.m_required_layers.insert(
            self.m_required_layers.end(),
            span_over_data.asSpan().begin(),
            span_over_data.asSpan().end()
        );
        return std::forward<Self>(self);
    }

    template<typename Self, typename First, typename... Tail>
    [[nodiscard]] auto&& preferLayers(
        this Self&& self,
        First const first,
        Tail const... rest
    ) {
        SpanOver span_over_data{ first, rest... };
        self.m_preferred_layers.insert(
            self.m_preferred_layers.end(),
            span_over_data.asSpan().begin(),
            span_over_data.asSpan().end()
        );
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& project(this Self&& self, std::string name, Version const version) {
        self.m_project_name = std::move(name);
        self.m_project_version = version;
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& engine(this Self&& self, std::string name, Version const version) {
        self.m_engine_name = std::move(name);
        self.m_engine_version = version;
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& portabilityEnumeration(this Self&& self, bool const enabled = true) {
        self.m_portability_enumeration = enabled;
        return std::forward<Self>(self);
    }

    // Throws InstanceCreationError
    // out_caps is where the capabilities of built instance will bw stored to.
    [[nodiscard]]
    Instance build(VulkanCaps* const out_caps = nullptr) const;
private:
    Version m_required_version{ 0, 1, 0, 0 };
    Version m_preferred_version{ Version::MAX() };
    DebugMessengerOptions m_debug_messenger_options{ };
    std::function<void()> m_validation_failure_callback{ };

    std::vector<VulkanExtension> m_required_extensions;
    std::vector<VulkanExtension> m_preferred_extensions;

    std::vector<VulkanLayer> m_required_layers;
    std::vector<VulkanLayer> m_preferred_layers;

    std::string m_project_name{ "Application" };
    Version m_project_version{ 0, 1, 0, 0 };

    std::string m_engine_name{ "Engine" };
    Version m_engine_version{ 0, 1, 0, 0 };

    bool m_require_validation { false };
    bool m_prefer_validation { false };
    bool m_portability_enumeration{ false };
    bool m_require_window_extensions{ false };
};

} // namespace core
