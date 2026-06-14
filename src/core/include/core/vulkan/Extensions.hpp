#pragma once

#include <core/common/Version.hpp>

#include <optional>
#include <string>

namespace core {

enum class VulkanExtension {
    // General
    Swapchain,               // VK_KHR_swapchain
    Maintenance1,            // VK_KHR_maintenance1
    Maintenance2,            // VK_KHR_maintenance2
    Maintenance3,            // VK_KHR_maintenance3
    Maintenance4,            // VK_KHR_maintenance4
    Maintenance5,            // VK_KHR_maintenance5
    Maintenance6,            // VK_KHR_maintenance6
    Maintenance7,            // VK_KHR_maintenance7
    Maintenance8,            // VK_KHR_maintenance8
    Maintenance9,            // VK_KHR_maintenance9
    Maintenance10,           // VK_KHR_maintenance10
    Maintenance11,           // VK_KHR_maintenance11
    ExternalMemory,          // VK_KHR_external_memory
    ExternalSemaphore,       // VK_KHR_external_semaphore
    GetMemoryRequirements2,  // VK_KHR_get_memory_requirements2
    CreateRenderPass2,       // VK_KHR_create_renderpass2
    MeshShader,              // VK_EXT_mesh_shader
    PortabilityEnumeration,  // VK_KHR_portability_enumeration

    // Surface
    Surface,                 // VK_KHR_surface
    AndroidSurface,          // VK_KHR_adnroid_surface
    DirectfbSurface,         // VK_EXT_directfb_surface
    FuchsiaImagepipeSurface, // VK_FUCHSIA_imagepipe_surface 
    HeadlessSurface,         // VK_EXT_headless_surface
    MetalSurface,            // VK_EXT_metal_surface
    OhosSurface,             // VK_OHOS_surface
    QnxSurface,              // VK_QNX_screen_surface
    UbmSurface,              // VK_SEC_ubm_surface
    Win32Surface,            // VK_KHR_win32_surface
    WaylandSurface,          // VK_KHR_wayland_surface
    XcbSurface,              // VK_KHR_xcb_surface
    XlibSurface,             // VK_KHR_xlib_surface

    // Efficiency
    DedicatedAllocation,     // VK_KHR_dedicated_allocation
    BufferDeviceAddress,     // VK_KHR_buffer_device_address
    BindMemory2,             // VK_KHR_bind_memory2
    MemoryPriority,          // VK_EXT_memory_priority
    MultiDraw,               // VK_EXT_multi_draw
    PipelineLibrary,         // VK_KHR_pipeline_library
    GraphicsPipelineLibrary, // VK_EXT_graphics_pipeline_library
    ConditionalRendering,    // VK_EXT_conditional_rendering
    DescriptorIndexing,      // VK_EXT_descriptor_indexing
    DeviceGeneratedCommands, // VK_EXT_device_generated_commands
    HostImageCopy,           // VK_EXT_host_image_copy 

    // Debug
    DebugUtils,              // VK_EXT_debug_utils
    MemoryBudget,            // VK_EXT_memory_budget
    DeviceAddressBindingReport, // VK_EXT_device_address_binding_report
    DeviceCoherentMemory,    // VK_AMD_device_coherent_memory
    DeviceMemoryReport,      // VK_EXT_device_memory_report
    DeviceFault,             // VK_KHR_device_fault
    ToolingInfo,             // VK_EXT_tooling_info

    Count,
};

enum class VulkanExtensionKind {
    Instance,
    Device,
};

struct VulkanExtensions final {
    Version versions[static_cast<size_t>(VulkanExtension::Count)];

    VulkanExtensions() noexcept;

    [[nodiscard]]
    bool hasExtension(VulkanExtension const ext) const noexcept;
    [[nodiscard]]
    Version getExtensionVersion(VulkanExtension const ext) const noexcept {
        return versions[static_cast<size_t>(ext)];
    }

    [[nodiscard]]
    Version& versionAt(VulkanExtension const ext) & noexcept {
        return versions[static_cast<size_t>(ext)];
    }

    [[nodiscard]]
    std::string toString(std::string_view const indent = "") const;
};

[[nodiscard]]
char const* getFullExtensionName(VulkanExtension const ext) noexcept;
[[nodiscard]]
std::optional<VulkanExtension> extensionFromFullName(std::string_view const name);
[[nodiscard]]
VulkanExtensionKind getExtensionKind(VulkanExtension const ext) noexcept;
[[nodiscard]]
Version getExtensionPromotionVersion(VulkanExtension const ext) noexcept;

VulkanExtensions loadSupportedInstanceExtensions();

} // namespace core
