#include <core/vulkan/InstanceBuilder.hpp>

#include <core/common/Assert.hpp>
#include <core/common/IterEnum.hpp>
#include <core/IO/Log.hpp>
#include <core/vulkan/Check.hpp>
#include <core/vulkan/VulkanVersion.hpp>

// DONT_CHECK INCLUDE_ORDER
#include <volk.h>
// DONT_CHECK INCLUDE_ORDER
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstring>

namespace core {
namespace {

VKAPI_ATTR VkBool32 VKAPI_PTR defaultDebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    VkDebugUtilsMessengerCallbackDataEXT const* pCallbackData,
    [[maybe_unused]] void* pUserData
) {
    char const* typeStr = "-";
    switch (messageType) {
        case VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT:     typeStr = "General";     break;
        case VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT:  typeStr = "Validation";  break;
        case VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT: typeStr = "Performance"; break;
    default: break;
    }
    switch (messageSeverity) {
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
            CORE_TRACE("Vulkan validation {:11}: {}", typeStr, pCallbackData->pMessage);
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
            CORE_DEBUG("Vulkan validation {:11}: {}", typeStr, pCallbackData->pMessage);
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
            CORE_WARN("Vulkan validation {:11}: {}",  typeStr, pCallbackData->pMessage);
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
            CORE_ERROR("Vulkan validation {:11}: {}", typeStr, pCallbackData->pMessage);
            break;
    default: break;
    }
    return VK_FALSE;
}

[[nodiscard]]
constexpr VkDebugUtilsMessengerCreateInfoEXT makeDebugMessengerCreateInfo(
    DebugMessengerOptions const& options
) noexcept {
    return VkDebugUtilsMessengerCreateInfoEXT{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = options.severity_mask,
        .messageType = options.type_mask,
        .pfnUserCallback = options.callback == nullptr
            ? defaultDebugCallback
            : options.callback,
        .pUserData = options.user_data,
    };
}

[[nodiscard]]
uint32_t selectInstanceVersion(Version const required, Version const preferred) {
    if (preferred < required) {
        throw InstanceCreationError(
            InstanceCreationErrorKind::InvalidApiVersionRange,
            "{} < {}", preferred, required);
    }

    uint32_t supported_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != nullptr) {
        if (!VK_CHECK(vkEnumerateInstanceVersion(&supported_version))) {
            throw InstanceCreationError(InstanceCreationErrorKind::UnsupportedApiVersion);
        }
    }

    uint32_t const required_version = versionToVk(required);
    uint32_t const preferred_version = versionToVk(preferred);
    if (supported_version < required_version) {
        throw InstanceCreationError(
            InstanceCreationErrorKind::UnsupportedApiVersion,
            "{}; supported at most: {}", required_version, supported_version);
    }

    uint32_t const selected_version = std::min(preferred_version, supported_version);
    if (selected_version < required_version) {
        throw InstanceCreationError(
            InstanceCreationErrorKind::UnsupportedApiVersion,
            "{}; selected: {}", required_version, selected_version);
    }
    return selected_version;
}

[[nodiscard]]
VulkanLayers collectLayersToEnable(
    VulkanLayers const& supported,
    std::vector<VulkanLayer> const& required,
    std::vector<VulkanLayer> const& preferred,
    bool const wants_validation
) {
    VulkanLayers enabled{ };
    auto const addLayer = [&](VulkanLayer const layer, bool const required) {
        if (!supported.hasLayer(layer)) {
            if (required) {
                throw InstanceCreationError(
                    InstanceCreationErrorKind::MissingRequiredLayer,
                    "{}", getFullLayerName(layer)
                );
            }
        }
        if (!enabled.hasLayer(layer)) {
            enabled.versionAt(layer) = supported.getLayerVersion(layer);
        }
    };

    for (VulkanLayer const layer : required) {
        addLayer(layer, true);
    }
    for (VulkanLayer const layer : preferred) {
        addLayer(layer, false);
    }
    if (wants_validation) {
        addLayer(VulkanLayer::Validation, false);
    }

    return enabled;
}

[[nodiscard]]
VulkanExtensions collectExtensionsToEnable(
    VulkanExtensions const& supported,
    std::vector<VulkanExtension> const& required,
    std::vector<VulkanExtension> const& preferred,
    bool const requires_window_extensions,
    bool const wants_validation,
    bool const portability_enumeration
) {
    VulkanExtensions enabled{ };
    auto const addExtension = [&](VulkanExtension const extension, bool const required) {
        if (getExtensionKind(extension) == VulkanExtensionKind::Device) {
            throw InstanceCreationError(
                InstanceCreationErrorKind::WrongExtensionScope,
                "{} is not an instance extension", getFullExtensionName(extension)
            );
        }
        if (!supported.hasExtension(extension)) {
            if (required) {
                throw InstanceCreationError(
                    InstanceCreationErrorKind::MissingRequiredExtension,
                    "{}", getFullExtensionName(extension)
                );
            }
        }
        if (!enabled.hasExtension(extension)) {
            enabled.versionAt(extension) = supported.getExtensionVersion(extension);
        }
    };

    for (VulkanExtension const extension : required) {
        addExtension(extension, true);
    }
    for (VulkanExtension const extension : preferred) {
        addExtension(extension, false);
    }
    if (portability_enumeration) {
        addExtension(VulkanExtension::PortabilityEnumeration, false);
    }
    if (wants_validation) {
        addExtension(VulkanExtension::DebugUtils, false);
    }
    if (requires_window_extensions) {
        if (glfwVulkanSupported() != GLFW_TRUE) {
            throw InstanceCreationError(InstanceCreationErrorKind::GlfwVulkanNotSupported);
        }

        uint32_t glfw_extension_count = 0;
        char const** const glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extension_count);
        if (glfw_extensions == nullptr || glfw_extension_count == 0) {
            throw InstanceCreationError(InstanceCreationErrorKind::GlfwVulkanNotSupported);
        }

        for (uint32_t i = 0; i < glfw_extension_count; ++i) {
            if (auto maybe_extension = extensionFromFullName(glfw_extensions[i])) {
                addExtension(*maybe_extension, true);
            } else {
                throw InstanceCreationError(
                    InstanceCreationErrorKind::MissingRequiredExtension,
                    "{} not recognized", glfw_extensions[i]
                );
            }
        }
    }

    return enabled;
}

} // namespace

[[nodiscard]]
std::string to_string(InstanceCreationErrorKind const error_kind) noexcept {
    switch (error_kind) {
        case InstanceCreationErrorKind::GlfwVulkanNotSupported:       return "GlfwVulkanNotSupported";
        case InstanceCreationErrorKind::VolkInitializationFailed:     return "VolkInitializationFailed";
        case InstanceCreationErrorKind::UnsupportedApiVersion:        return "UnsupportedApiVersion";
        case InstanceCreationErrorKind::InvalidApiVersionRange:       return "InvalidApiVersionRange";
        case InstanceCreationErrorKind::MissingRequiredLayer:         return "MissingRequiredLayer";
        case InstanceCreationErrorKind::MissingRequiredExtension:     return "MissingRequiredExtension";
        case InstanceCreationErrorKind::WrongExtensionScope:          return "WrongExtensionScope";
        case InstanceCreationErrorKind::ValidationUnavailable:        return "ValidationUnavailable";
        case InstanceCreationErrorKind::DebugUtilsUnavailable:        return "DebugUtilsUnavailable";
        case InstanceCreationErrorKind::InstanceCreationFailed:       return "InstanceCreateFailed";
        case InstanceCreationErrorKind::DebugMessengerCreationFailed: return "DebugMessengerCreationFailed";
    }
    ASSERT(false, "Unrecognized value: {}", static_cast<uint32_t>(error_kind));
    return "";
}

[[nodiscard]]
Instance InstanceBuilder::build(VulkanCaps* const out_caps) const {
    if (!VK_CHECK(volkInitialize())) {
        throw InstanceCreationError(InstanceCreationErrorKind::VolkInitializationFailed);
    }

    uint32_t const selected_version = selectInstanceVersion(m_required_version, m_preferred_version);
    bool const wants_validation = m_require_validation || m_prefer_validation;

    VulkanLayers const supported_layers = loadSupportedLayers();
    VulkanLayers const enabled_layers = collectLayersToEnable(
        supported_layers,
        m_required_layers,
        m_preferred_layers,
        wants_validation
    );
    VulkanExtensions supported_extensions = VulkanExtensions::loadSupportedInstanceExtensions();
    VulkanExtensions enabled_extensions = collectExtensionsToEnable(
        supported_extensions,
        m_required_extensions,
        m_preferred_extensions,
        m_require_window_extensions,
        wants_validation,
        m_portability_enumeration
    );

    bool const was_validation_enabled = true
        && enabled_layers.hasLayer(VulkanLayer::Validation)
        && enabled_extensions.hasExtension(VulkanExtension::DebugUtils);
    if (wants_validation && !was_validation_enabled) {
        if (m_require_validation) {
            throw InstanceCreationError(InstanceCreationErrorKind::ValidationUnavailable);
        } else if (m_validation_failure_callback) {
            m_validation_failure_callback();
        }
    }

    VkApplicationInfo app_info{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = m_project_name.c_str(),
        .applicationVersion = versionToVk(m_project_version),
        .pEngineName = m_engine_name.c_str(),
        .engineVersion = versionToVk(m_engine_version),
        .apiVersion = selected_version,
    };

    std::vector<char const*> enabled_layer_names;
    for (VulkanLayer const layer : iterEnum<VulkanLayer>()) {
        if (enabled_layers.hasLayer(layer)) {
            enabled_layer_names.push_back(getFullLayerName(layer));
        }
    }

    std::vector<char const*> enabled_extension_names;
    for (VulkanExtension const extension : iterEnum<VulkanExtension>()) {
        if (enabled_extensions.hasExtension(extension)) {
            enabled_extension_names.push_back(getFullExtensionName(extension));
        }
    }

    auto const debug_create_info = makeDebugMessengerCreateInfo(m_debug_messenger_options);
    VkInstanceCreateInfo const create_info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = (wants_validation && was_validation_enabled)
            ? &debug_create_info
            : nullptr,
        .flags = (m_portability_enumeration && enabled_extensions.hasExtension(VulkanExtension::PortabilityEnumeration))
            ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
            : static_cast<VkInstanceCreateFlags>(0),
        .pApplicationInfo = &app_info,
        .enabledLayerCount = static_cast<uint32_t>(enabled_layer_names.size()),
        .ppEnabledLayerNames = enabled_layer_names.data(),
        .enabledExtensionCount = static_cast<uint32_t>(enabled_extension_names.size()),
        .ppEnabledExtensionNames = enabled_extension_names.data(),
    };

    VkInstance instance = VK_NULL_HANDLE;
    if (!VK_CHECK(vkCreateInstance(&create_info, nullptr, &instance))) {
        throw InstanceCreationError(InstanceCreationErrorKind::InstanceCreationFailed);
    }

    volkLoadInstance(instance);

    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    if (wants_validation && was_validation_enabled) {
        if (!VK_CHECK(vkCreateDebugUtilsMessengerEXT(
            instance,
            &debug_create_info,
            nullptr,
            &debug_messenger
        ))) {
            vkDestroyInstance(instance, nullptr);
            throw InstanceCreationError(InstanceCreationErrorKind::DebugMessengerCreationFailed);
        }
    }

    if (out_caps) {
        out_caps->commitInstanceCaps(
            vkToVersion(selected_version),
            wants_validation && was_validation_enabled,
            enabled_layers,
            enabled_extensions
        );
    }

    return Instance{ instance, debug_messenger };
}

} // namespace core
