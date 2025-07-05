#include "Instance.hpp"
#include <vulkan/vulkan.h>
#include <Core/IO/Logger.hpp>
#include "Check.hpp"
#include "Functions.hpp"
#include "Version.hpp"
#include "Exception.hpp"

namespace graphics::vulkan {
namespace {
    void initInstance(VkInstance& instance, char const* const appName, char const** windowRequiredExtensions, uint32_t windowRequiredExtensionsCount) {
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
        createInfo.enabledExtensionCount = windowRequiredExtensionsCount;
        createInfo.ppEnabledExtensionNames = windowRequiredExtensions;
        createInfo.enabledLayerCount = 0;

        if (!VK_CHECK(vkCreateInstance(&createInfo, nullptr, &instance))) {
            core::io::fatal("Failed to create Vulkan instance");
            throw VulkanException { };
        }
    }

    void destroyInstance(VkInstance& instance) {
        vkDestroyInstance(instance, nullptr);
    }
} // namespace

    Instance::Instance(char const* const appName, char const** windowRequiredExtensions, uint32_t windowRequiredExtensionsCount) 
        : m_instance(core::memory::TypeErasedObject::make<VkInstance>()) {
        core::io::info("Creating Vulkan instance...");
        initInstance(m_instance.get<VkInstance>(), appName, windowRequiredExtensions, windowRequiredExtensionsCount);
        core::io::info("Created Vulkan {}.{}.{} instance", getVkVersionMajor(), getVkVersionMinor(), getVkVersionPatch());
    }

    Instance::~Instance() {
        if (m_instance.hasValue()) {
            destroyInstance(m_instance.get<VkInstance>());
            core::io::info("Destroyed Vulkan instance");
        }
    }
} // namespace graphics::vulkan

