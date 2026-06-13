#include "Extensions.hpp"

#include <core/IO/Log.hpp>
#include <core/vulkan/Check.hpp>
#include <core/vulkan/VulkanVersion.hpp>

#include <fmt/core.h>
#include <vulkan/vulkan.h>

#include <string>
#include <vector>
#include <unordered_map>

namespace core {
    
VulkanExtensions::VulkanExtensions() noexcept {
    memset(versions, 255, std::size(versions));
}

bool VulkanExtensions::hasExtension(VulkanExtension const ext) const noexcept {
    return getExtensionVersion(ext).epoch != UINT32_MAX;
}

Version VulkanExtensions::getExtensionVersion(VulkanExtension const ext) const noexcept {
    return versions[static_cast<size_t>(ext)];
}

std::string VulkanExtensions::toString(std::string_view const indent) const {
    std::string extensions_message;
    for (uint32_t i = 0; i < static_cast<uint32_t>(VulkanExtension::VulkanExtensionsCount); ++i) {
        VulkanExtension ext = static_cast<VulkanExtension>(i);
        if (hasExtension(ext)) {
            Version const v = getExtensionVersion(ext);
            extensions_message += fmt::format("{}{:40} v{}.{}.{}\n", indent, getFullExtensionName(ext), v.major, v.minor, v.patch);
        }
    }
    return extensions_message;
}

char const* getFullExtensionName(VulkanExtension const ext) noexcept {
    char const* EXTENSION_NAMES[] = {
        "VK_KHR_swapchain",
        "VK_KHR_maintenance1",
        "VK_KHR_maintenance2",
        "VK_KHR_maintenance3",
        "VK_KHR_maintenance4",
        "VK_KHR_maintenance5",
        "VK_KHR_maintenance6",
        "VK_KHR_maintenance7",
        "VK_KHR_maintenance8",
        "VK_KHR_maintenance9",
        "VK_KHR_maintenance10",
        "VK_KHR_maintenance11",
        "VK_KHR_external_memory",
        "VK_KHR_external_semaphore",
        "VK_KHR_get_memory_requirements2",
        "VK_KHR_create_renderpass2",
        "VK_EXT_mesh_shader",

        "VK_KHR_dedicated_allocation",
        "VK_KHR_buffer_device_address",
        "VK_KHR_bind_memory2",
        "VK_EXT_memory_priority",
        "VK_EXT_multi_draw",
        "VK_KHR_pipeline_library",
        "VK_EXT_graphics_pipeline_library",
        "VK_EXT_conditional_rendering",
        "VK_EXT_descriptor_indexing",
        "VK_EXT_device_generated_commands",
        "VK_EXT_host_image_copy",

        "VK_EXT_debug_utils",
        "VK_EXT_memory_budget",
        "VK_EXT_device_address_binding_report",
        "VK_AMD_device_coherent_memory",
        "VK_EXT_device_memory_report",
        "VK_KHR_device_fault",
        "VK_EXT_tooling_info",
    };
    return EXTENSION_NAMES[static_cast<size_t>(ext)];
}

VulkanExtensionKind getExtensionKind(VulkanExtension const ext) noexcept {
    switch (ext) {
        case VulkanExtension::DebugUtils:
            return VulkanExtensionKind::Instance;
    default: return VulkanExtensionKind::Device;
    }
}

Version getExtensionPromotionVersion(VulkanExtension const ext) noexcept {
    switch (ext) {
    case VulkanExtension::Maintenance1: [[fallthrough]];
    case VulkanExtension::Maintenance2: [[fallthrough]];
    case VulkanExtension::BindMemory2: [[fallthrough]];
    case VulkanExtension::DedicatedAllocation: [[fallthrough]];
    case VulkanExtension::ExternalMemory: [[fallthrough]];
    case VulkanExtension::ExternalSemaphore: [[fallthrough]];
    case VulkanExtension::GetMemoryRequirements2: [[fallthrough]];
    case VulkanExtension::Maintenance3:
        return vkToVersion(VK_API_VERSION_1_1);
    case VulkanExtension::BufferDeviceAddress: [[fallthrough]];
    case VulkanExtension::CreateRenderPass2: [[fallthrough]];
    case VulkanExtension::DescriptorIndexing:
        return vkToVersion(VK_API_VERSION_1_2);
    case VulkanExtension::Maintenance4:
        return vkToVersion(VK_API_VERSION_1_3);
    case VulkanExtension::Maintenance5: [[fallthrough]];
    case VulkanExtension::HostImageCopy: [[fallthrough]];
    case VulkanExtension::ToolingInfo: [[fallthrough]];
    case VulkanExtension::Maintenance6:
        return vkToVersion(VK_API_VERSION_1_4);
    case VulkanExtension::Maintenance7: [[fallthrough]];
    case VulkanExtension::Maintenance8: [[fallthrough]];
    case VulkanExtension::Maintenance9: [[fallthrough]];
    case VulkanExtension::Maintenance10: [[fallthrough]];
    case VulkanExtension::Maintenance11: [[fallthrough]];
    case VulkanExtension::DeviceCoherentMemory: [[fallthrough]];
    case VulkanExtension::DeviceMemoryReport: [[fallthrough]];
    case VulkanExtension::DeviceAddressBindingReport: [[fallthrough]];
    case VulkanExtension::Swapchain: [[fallthrough]];
    case VulkanExtension::DeviceFault: [[fallthrough]];
    case VulkanExtension::DeviceGeneratedCommands: [[fallthrough]];
    case VulkanExtension::GraphicsPipelineLibrary: [[fallthrough]];
    case VulkanExtension::PipelineLibrary: [[fallthrough]];
    case VulkanExtension::MemoryBudget: [[fallthrough]];
    case VulkanExtension::MemoryPriority: [[fallthrough]];
    case VulkanExtension::MeshShader: [[fallthrough]];
    case VulkanExtension::MultiDraw: [[fallthrough]];
    case VulkanExtension::ConditionalRendering:
        return Version::max();
    default:
        ASSERT(false, "Unknown vulkan extension: {}", static_cast<uint32_t>(ext));
    }
}

VulkanExtensions loadSupportedInstanceExtensions() {
    VulkanExtensions supported_extensions { };

    MC_INFO("Loading supported Vulkan extensions list...");
    uint32_t extensionCount = 0;
    VK_ASSERT(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr));

    std::vector<VkExtensionProperties> extensions(extensionCount);
    VK_ASSERT(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data()));

    std::unordered_map<std::string, uint32_t> extensionVersions;
    for (VkExtensionProperties const& ext : extensions) {
        extensionVersions[ext.extensionName] = ext.specVersion;
    }

    for (uint32_t i = 0; i < static_cast<uint32_t>(VulkanExtension::VulkanExtensionsCount); ++i) {
        VulkanExtension ext = static_cast<VulkanExtension>(i);
        if (auto it = extensionVersions.find(getFullExtensionName(ext)); it != extensionVersions.end()) {
            supported_extensions.versions[static_cast<size_t>(ext)] = vkToVersion(it->second);
        }
    }

    return supported_extensions;
}

} // namespace core
