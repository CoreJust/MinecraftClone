#include "Instance.hpp"
#include <vector>
#include <vulkan/vulkan.h>
#include <Config.hpp>
#include <Core/Collection/DynArray.hpp>
#include <Core/IO/Logger.hpp>
#include "../../Exception.hpp"
#include "../Check.hpp"
#include "Functions.hpp"
#include "Layers.hpp"
#include "Extensions.hpp"

namespace graphics::vulkan::internal {
namespace {
    constexpr ProjectInfo ENGINE_INFO {
        .name = "Voxel Engine",
        .version = Version {{ 0, 0, 1, 0 }},
    };

    VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        [[maybe_unused]] void* pUserData
    ) {
        char const* typeStr = "-";
        switch (messageType) {
            case VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT:     typeStr = "General";     break;
            case VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT:  typeStr = "Validation";  break;
            case VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT: typeStr = "Performance"; break;
        default:  break;
        }
        switch (messageSeverity) {
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
                core::trace("Vulkan validation {:11}: {}", typeStr, pCallbackData->pMessage);
                break;
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
                core::debug("Vulkan validation {:11}: {}", typeStr, pCallbackData->pMessage);
                break;
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
                core::warn("Vulkan validation {:11}: {}",  typeStr, pCallbackData->pMessage);
                break;
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
                core::error("Vulkan validation {:11}: {}", typeStr, pCallbackData->pMessage);
                break;
        default: break;
        }
        return VK_FALSE;
    }

    VkDebugUtilsMessengerCreateInfoEXT makeDebugUtilsMessengerCreateInfo() {
        return {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .pNext = nullptr,
            .flags = 0,
            .messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT
                | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
                | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debugCallback,
            .pUserData = nullptr,
        };
    }

    void initDebugCallback(VkInstance& instance, VkDebugUtilsMessengerEXT& messenger) {
        VkDebugUtilsMessengerCreateInfoEXT createInfo = makeDebugUtilsMessengerCreateInfo();
        if (!pvkCreateDebugUtilsMessengerEXT || !VK_CHECK(pvkCreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &messenger)))
            core::warn("Failed to set Vulkan debug callback");
    }

    VkApplicationInfo makeApplicationInfo(ProjectInfo const& appInfo, ProjectInfo const& engineInfo, Version const& vulkanVersion) {
        return {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pNext = nullptr,
            .pApplicationName   = appInfo.name,
            .applicationVersion = appInfo.version.asVk(),
            .pEngineName        = engineInfo.name,
            .engineVersion      = engineInfo.version.asVk(),
            .apiVersion         = vulkanVersion.asVk(),
        };
    }

    VkInstance createInstance(VkApplicationInfo const& appInfo, core::ArrayView<char const*> requiredExtensions, bool& needsDebugCallback) {
        VkInstance instance = VK_NULL_HANDLE;
        std::vector<char const*> extensions(requiredExtensions.size());
        std::copy(requiredExtensions.begin(), requiredExtensions.end(), extensions.begin());

        VkInstanceCreateInfo createInfo { };
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledLayerCount = 0;
        needsDebugCallback = false;
        VkDebugUtilsMessengerCreateInfoEXT debugUtilsCreateInfo { };
        if (config::g_isDebugEnabled) {
            if (!hasLayer(VulkanLayer::Validation)) {
                core::warn("Vulkan validation layers not found: cannot enable them");
            } else {
                static char const* LAYER_NAMES[] = { getFullLayerName(VulkanLayer::Validation) };
                createInfo.enabledLayerCount = 1;
                createInfo.ppEnabledLayerNames = LAYER_NAMES;
                debugUtilsCreateInfo = makeDebugUtilsMessengerCreateInfo();
                createInfo.pNext = &debugUtilsCreateInfo;
                if (hasExtension(VulkanExtension::DebugUtils)) {
                    extensions.push_back(getFullExtensionName(VulkanExtension::DebugUtils));
                    needsDebugCallback = true;
                }
            }
        }
        createInfo.enabledExtensionCount = static_cast<u32>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        
        if (!VK_CHECK(vkCreateInstance(&createInfo, nullptr, &instance))) {
            core::fatal("Failed to create Vulkan instance");
            throw VulkanException { };
        }

        for (char const* ext : extensions) 
            core::debug("Loaded Vulkan extension: {}", ext);

        return instance;
    }
} // namespace

    Instance::Instance(ProjectInfo const& appInfo, core::ArrayView<char const*> requiredExtensions) {
        core::debug("Creating Vulkan instance...");
        VkApplicationInfo applicationInfo = makeApplicationInfo(appInfo, ENGINE_INFO, getVkVersion());
        bool needsDebugCallback;
        m_instance = createInstance(applicationInfo, requiredExtensions, needsDebugCallback);
        loadVkFunctions(m_instance);
        if (needsDebugCallback)
            initDebugCallback(m_instance, m_debugMessenger);
        core::info("Created Vulkan {}.{}.{} instance", getVkVersionMajor(), getVkVersionMinor(), getVkVersionPatch());
    }

    Instance::~Instance() {
        if (m_instance != VK_NULL_HANDLE) {
            if (m_debugMessenger != VK_NULL_HANDLE) {
                if (pvkDestroyDebugUtilsMessengerEXT)
                    pvkDestroyDebugUtilsMessengerEXT(m_instance, m_debugMessenger, nullptr);
                m_debugMessenger = VK_NULL_HANDLE;
            }
            vkDestroyInstance(m_instance, nullptr);
            m_instance = VK_NULL_HANDLE;
            core::note("Destroyed Vulkan instance");
        }
    }
} // namespace graphics::vulkan::internal
