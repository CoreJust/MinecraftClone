#pragma once

#include <core/common/Version.hpp>
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

enum class InstanceCreationErrorKind {
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
    DebugMessengerCreationFailed,
};

[[nodiscard]]
std::string to_string(InstanceCreationErrorKind const error_kind) noexcept;

struct InstanceCreationError : public VulkanError {
    InstanceCreationErrorKind kind;

    InstanceCreationError(InstanceCreationErrorKind const kind)
        : VulkanError(to_string(kind))
        , kind(kind)
    { }

    template<typename... Args>
    InstanceCreationError(InstanceCreationErrorKind const kind, fmt::format_string<Args...> format_string, Args&&... args)
        : VulkanError(to_string(kind) + ": " + fmt::format(format_string, std::forward<Args>(args)...))
        , kind(kind)
    { }
};

class InstanceBuilder final {
public:
    // Might throw InstanceCreationError(GlfwVulkanNotSupported)
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

    template<typename Self>
    [[nodiscard]] auto&& requireExtensions(
        this Self&& self,
        std::span<VulkanExtension const> const extensions
    ) {
        self.m_required_extensions.insert(
            self.m_required_extensions.end(),
            extensions.begin(),
            extensions.end());
        return std::forward<Self>(self);
    }

    template<typename Self, class... Tail>
    [[nodiscard]] auto&& requireExtensions(
        this Self&& self,
        VulkanExtension const first,
        Tail const... rest
    ) {
        std::array<VulkanExtension, 1U + sizeof...(rest)> values{ first, rest... };
        return self.requireExtensions(std::span<VulkanExtension const>(values.data(), values.size()));
    }

    template<typename Self>
    [[nodiscard]] auto&& preferExtensions(
        this Self&& self,
        std::span<VulkanExtension const> const extensions
    ) {
        self.m_preferred_extensions.insert(
            self.m_preferred_extensions.end(),
            extensions.begin(),
            extensions.end());
        return std::forward<Self>(self);
    }

    template<typename Self, class... Tail>
    [[nodiscard]] auto&& preferExtensions(
        this Self&& self,
        VulkanExtension const first,
        Tail const... rest
    ) {
        std::array<VulkanExtension, 1U + sizeof...(rest)> values{ first, rest... };
        return self.preferExtensions(std::span<VulkanExtension const>(values.data(), values.size()));
    }

    template<typename Self>
    [[nodiscard]] auto&& requireLayers(
        this Self&& self,
        std::span<VulkanLayer const> const layers
    ) {
        self.m_required_layers.insert(
            self.m_required_layers.end(),
            layers.begin(),
            layers.end());
        return std::forward<Self>(self);
    }

    template<typename Self, class... Tail>
    [[nodiscard]] auto&& requireLayers(
        this Self&& self,
        VulkanLayer const first,
        Tail const... rest
    ) {
        std::array<VulkanLayer, 1U + sizeof...(rest)> values{ first, rest... };
        return self.requireLayers(std::span<VulkanLayer const>(values.data(), values.size()));
    }

    template<typename Self>
    [[nodiscard]] auto&& preferLayers(
        this Self&& self,
        std::span<VulkanLayer const> const layers
    ) {
        self.m_preferred_layers.insert(
            self.m_preferred_layers.end(),
            layers.begin(),
            layers.end());
        return std::forward<Self>(self);
    }

    template<typename Self, class... Tail>
    [[nodiscard]] auto&& preferLayers(
        this Self&& self,
        VulkanLayer const first,
        Tail const... rest
    ) {
        std::array<VulkanLayer, 1U + sizeof...(rest)> values{ first, rest... };
        return self.preferLayers(std::span<VulkanLayer const>(values.data(), values.size()));
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
    [[nodiscard]]
    Instance build() const;
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
