#include "Extensions.hpp"

#include <core/IO/Log.hpp>
#include <core/meta/EnumImpl.hpp>
#include <core/vulkan/Check.hpp>
#include <core/vulkan/PhysicalDevice.hpp>
#include <core/vulkan/VulkanVersion.hpp>

#include <volk.h>

#include <string>
#include <unordered_map>
#include <vector>
    
CORE_ENUM_FUNCTIONS_IMPL(::core::vk::VulkanExtension);
CORE_ENUM_FUNCTIONS_IMPL(::core::vk::VulkanExtensionKind);

namespace core::vk {

VulkanExtensions::VulkanExtensions() noexcept {
    memset(versions, 255, std::size(versions) * sizeof(Version));
}

bool VulkanExtensions::hasExtension(VulkanExtension const ext) const noexcept {
    return getExtensionVersion(ext).epoch != UINT32_MAX;
}

std::string VulkanExtensions::toString(std::string_view const indent) const {
    std::string extensions_message;
    for (VulkanExtension const ext : valuesOf<VulkanExtension>()) {
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
        "VK_KHR_portability_enumeration",
        "VK_KHR_portability_subset",

        "VK_KHR_surface",
        "VK_KHR_android_surface",
        "VK_EXT_directfb_surface",
        "VK_FUCHSIA_imagepipe_surface",
        "VK_EXT_headless_surface",
        "VK_EXT_metal_surface",
        "VK_OHOS_surface",
        "VK_QNX_screen_surface",
        "VK_SEC_ubm_surface",
        "VK_KHR_win32_surface",
        "VK_KHR_wayland_surface",
        "VK_KHR_xcb_surface",
        "VK_KHR_xlib_surface",

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
    return EXTENSION_NAMES[indexOf(ext)];
}

std::optional<VulkanExtension> extensionFromFullName(std::string_view const name) {
    static std::unordered_map<std::string_view, VulkanExtension> FULL_NAME_TO_EXTENSION = std::invoke(
        []{
            std::unordered_map<std::string_view, VulkanExtension> result;
            result.reserve(countOf<VulkanExtension>());
            for (VulkanExtension const ext : valuesOf<VulkanExtension>()) {
                result[getFullExtensionName(ext)] = ext;
            }
            return result;
        }
    );
    if (auto it = FULL_NAME_TO_EXTENSION.find(name); it != FULL_NAME_TO_EXTENSION.end()) {
        return it->second;
    }
    return std::nullopt;
}

VulkanExtensionKind getExtensionKind(VulkanExtension const ext) noexcept {
    switch (ext) {
        case VulkanExtension::Surface:                 [[fallthrough]];
        case VulkanExtension::AndroidSurface:          [[fallthrough]];
        case VulkanExtension::DirectfbSurface:         [[fallthrough]];
        case VulkanExtension::FuchsiaImagepipeSurface: [[fallthrough]];
        case VulkanExtension::HeadlessSurface:         [[fallthrough]];
        case VulkanExtension::MetalSurface:            [[fallthrough]];
        case VulkanExtension::OhosSurface:             [[fallthrough]];
        case VulkanExtension::QnxSurface:              [[fallthrough]];
        case VulkanExtension::UbmSurface:              [[fallthrough]];
        case VulkanExtension::Win32Surface:            [[fallthrough]];
        case VulkanExtension::WaylandSurface:          [[fallthrough]];
        case VulkanExtension::XcbSurface:              [[fallthrough]];
        case VulkanExtension::XlibSurface:             [[fallthrough]];
        case VulkanExtension::PortabilityEnumeration:  [[fallthrough]];
        case VulkanExtension::DebugUtils:
            return VulkanExtensionKind::Instance;
    default: return VulkanExtensionKind::Device;
    }
}

Version getExtensionPromotionVersion(VulkanExtension const ext) noexcept {
    switch (ext) {
        case VulkanExtension::Maintenance1:               [[fallthrough]];
        case VulkanExtension::Maintenance2:               [[fallthrough]];
        case VulkanExtension::BindMemory2:                [[fallthrough]];
        case VulkanExtension::DedicatedAllocation:        [[fallthrough]];
        case VulkanExtension::ExternalMemory:             [[fallthrough]];
        case VulkanExtension::ExternalSemaphore:          [[fallthrough]];
        case VulkanExtension::GetMemoryRequirements2:     [[fallthrough]];
        case VulkanExtension::Maintenance3:
            return vkToVersion(VK_API_VERSION_1_1);
        case VulkanExtension::BufferDeviceAddress:        [[fallthrough]];
        case VulkanExtension::CreateRenderPass2:          [[fallthrough]];
        case VulkanExtension::DescriptorIndexing:
            return vkToVersion(VK_API_VERSION_1_2);
        case VulkanExtension::Maintenance4:
            return vkToVersion(VK_API_VERSION_1_3);
        case VulkanExtension::Maintenance5:               [[fallthrough]];
        case VulkanExtension::HostImageCopy:              [[fallthrough]];
        case VulkanExtension::ToolingInfo:                [[fallthrough]];
        case VulkanExtension::Maintenance6:
            return vkToVersion(VK_API_VERSION_1_4);
        case VulkanExtension::Surface:                    [[fallthrough]];
        case VulkanExtension::AndroidSurface:             [[fallthrough]];
        case VulkanExtension::DirectfbSurface:            [[fallthrough]];
        case VulkanExtension::FuchsiaImagepipeSurface:    [[fallthrough]];
        case VulkanExtension::HeadlessSurface:            [[fallthrough]];
        case VulkanExtension::MetalSurface:               [[fallthrough]];
        case VulkanExtension::OhosSurface:                [[fallthrough]];
        case VulkanExtension::QnxSurface:                 [[fallthrough]];
        case VulkanExtension::UbmSurface:                 [[fallthrough]];
        case VulkanExtension::Win32Surface:               [[fallthrough]];
        case VulkanExtension::WaylandSurface:             [[fallthrough]];
        case VulkanExtension::XcbSurface:                 [[fallthrough]];
        case VulkanExtension::XlibSurface:                [[fallthrough]];
        case VulkanExtension::Maintenance7:               [[fallthrough]];
        case VulkanExtension::Maintenance8:               [[fallthrough]];
        case VulkanExtension::Maintenance9:               [[fallthrough]];
        case VulkanExtension::Maintenance10:              [[fallthrough]];
        case VulkanExtension::Maintenance11:              [[fallthrough]];
        case VulkanExtension::DeviceCoherentMemory:       [[fallthrough]];
        case VulkanExtension::DeviceMemoryReport:         [[fallthrough]];
        case VulkanExtension::DeviceAddressBindingReport: [[fallthrough]];
        case VulkanExtension::Swapchain:                  [[fallthrough]];
        case VulkanExtension::DeviceFault:                [[fallthrough]];
        case VulkanExtension::DeviceGeneratedCommands:    [[fallthrough]];
        case VulkanExtension::GraphicsPipelineLibrary:    [[fallthrough]];
        case VulkanExtension::PipelineLibrary:            [[fallthrough]];
        case VulkanExtension::MemoryBudget:               [[fallthrough]];
        case VulkanExtension::MemoryPriority:             [[fallthrough]];
        case VulkanExtension::MeshShader:                 [[fallthrough]];
        case VulkanExtension::MultiDraw:                  [[fallthrough]];
        case VulkanExtension::PortabilityEnumeration:     [[fallthrough]];
        case VulkanExtension::PortabilitySubset:          [[fallthrough]];
        case VulkanExtension::ConditionalRendering:       [[fallthrough]];
        case VulkanExtension::DebugUtils:
            return Version::MAX();
    default:
        UNREACHABLE("Unknown vulkan extension: {}", indexOf(ext));
    }
}

VulkanExtensions VulkanExtensions::loadSupportedInstanceExtensions() {
    VulkanExtensions supported_extensions { };

    CORE_DEBUG("Loading supported Vulkan instance extensions list...");
    uint32_t extension_count = 0;
    CORE_VK_ASSERT(vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr));

    std::vector<VkExtensionProperties> extensions(extension_count);
    if (extension_count > 0) {
        CORE_VK_ASSERT(vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, extensions.data()));
    }

    for (VkExtensionProperties const& vk_ext : extensions) {
        if (auto maybe_ext = extensionFromFullName(vk_ext.extensionName)) {
            supported_extensions.versionAt(*maybe_ext) = vkToVersion(vk_ext.specVersion);
        } else {
            CORE_TRACE("Supported extension not known: {}", vk_ext.extensionName);
        }
    }

    return supported_extensions;
}

VulkanExtensions VulkanExtensions::loadSupportedDeviceExtensions(PhysicalDevice const& device) {
    VulkanExtensions supported_extensions { };

    CORE_DEBUG("Loading supported Vulkan extensions list...");
    uint32_t extension_count = 0;
    CORE_VK_ASSERT(vkEnumerateDeviceExtensionProperties(device.handle(), nullptr, &extension_count, nullptr));

    std::vector<VkExtensionProperties> extensions(extension_count);
    if (extension_count > 0) {
        CORE_VK_ASSERT(vkEnumerateDeviceExtensionProperties(device.handle(), nullptr, &extension_count, extensions.data()));
    }

    for (VkExtensionProperties const& vk_ext : extensions) {
        if (auto maybe_ext = extensionFromFullName(vk_ext.extensionName)) {
            supported_extensions.versionAt(*maybe_ext) = vkToVersion(vk_ext.specVersion);
        } else {
            CORE_TRACE("Supported extension not known: {}", vk_ext.extensionName);
        }
    }

    return supported_extensions;
}

} // namespace core::vk
