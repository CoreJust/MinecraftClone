#include "Instance.hpp"
#include <vector>
#include <vulkan/vulkan.h>
#include <Config.hpp>
#include <Core/IO/Logger.hpp>
#include "Check.hpp"
#include "Functions.hpp"
#include "Version.hpp"
#include "Exception.hpp"
#include "Layers.hpp"
#include "Extensions.hpp"

namespace graphics::vulkan {
namespace {
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
                core::io::trace("Vulkan validation {:11}: {}", typeStr, pCallbackData->pMessage);
                break;
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
                core::io::debug("Vulkan validation {:11}: {}",  typeStr, pCallbackData->pMessage);
                break;
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
                core::io::warn("Vulkan validation {:11}: {}",  typeStr, pCallbackData->pMessage);
                break;
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
                core::io::error("Vulkan validation {:11}: {}", typeStr, pCallbackData->pMessage);
                break;
        default: break;
        }
        return VK_FALSE;
    }

    void initDebugUtilsMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo) {
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = debugCallback;
        createInfo.pUserData = nullptr;
    }

    void initDebugCallback(VkInstance& instance, VkDebugUtilsMessengerEXT& messenger) {
        VkDebugUtilsMessengerCreateInfoEXT createInfo { };
        initDebugUtilsMessengerCreateInfo(createInfo);

        if (!VK_CHECK(vkCreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &messenger)))
            core::io::warn("Failed to set Vulkan debug callback");
    }

    void initInstance(VkInstance& instance, char const* const appName, char const** windowRequiredExtensions, uint32_t windowRequiredExtensionsCount, bool& needsDebugCallback) {
        std::vector<char const*> extensions(windowRequiredExtensionsCount);
        std::copy(windowRequiredExtensions, windowRequiredExtensions + windowRequiredExtensionsCount, extensions.begin());

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = appName;
        appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
        appInfo.pEngineName = "CoreVoxelEngine";
        appInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        appInfo.apiVersion = getVkVersion().asVk();

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledLayerCount = 0;
        needsDebugCallback = false;
        VkDebugUtilsMessengerCreateInfoEXT debugUtilsCreateInfo { };
        if (config::g_isDebugEnabled) {
            if (!hasLayer(VulkanLayer::Validation)) {
                core::io::warn("Vulkan validation layers not found: cannot enable them");
            } else {
                static char const* LAYER_NAMES[] = { getFullLayerName(VulkanLayer::Validation) };
                createInfo.enabledLayerCount = 1;
                createInfo.ppEnabledLayerNames = LAYER_NAMES;
                initDebugUtilsMessengerCreateInfo(debugUtilsCreateInfo);
                createInfo.pNext = &debugUtilsCreateInfo;
                if (hasExtension(VulkanExtension::DebugUtils)) {
                    extensions.push_back(getFullExtensionName(VulkanExtension::DebugUtils));
                    needsDebugCallback = true;
                }
            }
        }
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        if (!VK_CHECK(vkCreateInstance(&createInfo, nullptr, &instance))) {
            core::io::fatal("Failed to create Vulkan instance");
            throw VulkanException { };
        }
    }
} // namespace

    Instance::Instance(char const* const appName, char const** windowRequiredExtensions, uint32_t windowRequiredExtensionsCount) 
        : m_instance      (core::memory::TypeErasedObject::make<VkInstance>())
        , m_debugMessenger(core::memory::TypeErasedObject::make<VkDebugUtilsMessengerEXT>())
    {
        core::io::info("Creating Vulkan instance...");
        VkInstance& instance = m_instance.get<VkInstance>();
        bool needsDebugCallback;
        initInstance(instance, appName, windowRequiredExtensions, windowRequiredExtensionsCount, needsDebugCallback);
        loadVkFunctions(instance);
        if (needsDebugCallback)
            initDebugCallback(instance, m_debugMessenger.get<VkDebugUtilsMessengerEXT>());
        core::io::info("Created Vulkan {}.{}.{} instance", getVkVersionMajor(), getVkVersionMinor(), getVkVersionPatch());
    }

    Instance::~Instance() {
        if (m_instance.hasValue()) {
            VkInstance& instance = m_instance.get<VkInstance>();
            vkDestroyDebugUtilsMessengerEXT(instance, m_debugMessenger.get<VkDebugUtilsMessengerEXT>(), nullptr);
            vkDestroyInstance(instance, nullptr);
            core::io::info("Destroyed Vulkan instance");
        }
    }
} // namespace graphics::vulkan

