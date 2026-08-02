#pragma once

#include <core/vulkan/builder/DeviceBuilder.hpp>
#include <core/vulkan/builder/InstanceBuilder.hpp>
#include <core/vulkan/builder/PhysicalDeviceSelector.hpp>
#include <core/vulkan/builder/SwapchainBuilder.hpp>

#include <optional>

namespace core::vk {

class VulkanContextBuilder {
public:
    template<typename Self>
    [[nodiscard]] auto&& project(this Self&& self, std::string name, Version const version) {
        self.m_instance_builder.project(std::move(name), version);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& engine(this Self&& self, std::string name, Version const version) {
        self.m_instance_builder.engine(std::move(name), version);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& requireVersion(this Self&& self, Version const version) {
        self.m_instance_builder.requireVersion(version);
        self.m_physical_device_selector.requireApiVersion(version);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& preferVersion(this Self&& self, Version const version) {
        self.m_instance_builder.preferVersion(version);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& requireValidation(
        this Self&& self,
        bool const enabled = true,
        DebugMessengerOptionsBuilder const& options_builder = {},
        std::function<void()> const& failure_callback = {}
    ) {
        self.m_instance_builder.requireValidation(enabled, options_builder, failure_callback);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& preferValidation(
        this Self&& self,
        bool const enabled = true,
        DebugMessengerOptionsBuilder const& options_builder = {},
        std::function<void()> const& failure_callback = {}
    ) {
        self.m_instance_builder.preferValidation(enabled, options_builder, failure_callback);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& requireExtensions(
        this Self&& self,
        InputSpan<VulkanExtension> const exts
    ) {
        for (VulkanExtension const ext : exts) {
            if (getExtensionKind(ext) == VulkanExtensionKind::Instance) {
                self.m_instance_builder.requireExtensions({ ext });
            } else {
                self.m_physical_device_selector.requireExtensions({ ext });
            }
        }
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& preferExtensions(
        this Self&& self,
        InputSpan<VulkanExtension> const exts
    ) {
        for (VulkanExtension const ext : exts) {
            if (getExtensionKind(ext) == VulkanExtensionKind::Instance) {
                self.m_instance_builder.preferExtensions({ ext });
            } else {
                self.m_physical_device_selector.preferExtensions({ ext });
            }
        }
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& requireLayers(
        this Self&& self,
        InputSpan<VulkanLayer> const layers
    ) {
        self.m_instance_builder.requireLayers(layers);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& preferLayers(
        this Self&& self,
        InputSpan<VulkanLayer> const layers
    ) {
        self.m_instance_builder.preferLayers(layers);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& portabilityEnumeration(this Self&& self, bool const enabled = true) {
        self.m_instance_builder.portabilityEnumeration(enabled);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& requireFeatures(this Self&& self, InputSpan<VulkanFeature> const features) {
        self.m_physical_device_selector.requireFeatures(features);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& preferFeatures(this Self&& self, InputSpan<VulkanFeature> const features) {
        self.m_physical_device_selector.preferFeatures(features);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& requireMemoryHeaps(this Self&& self, InputSpan<MemoryPropertyBits> const heaps) {
        self.m_physical_device_selector.requireMemoryHeaps(heaps);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& requireDeviceType(this Self&& self, InputSpan<PhysicalDeviceType> const types) {
        self.m_physical_device_selector.requireDeviceType(types);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& preferDeviceType(this Self&& self, InputSpan<PhysicalDeviceType> const types) {
        self.m_physical_device_selector.preferDeviceType(types);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& requireQueueFamilies(
        this Self&& self,
        InputSpan<TrivialPair<QueueFamily, float>> const families
    ) {
        for (auto [family, priority] : families) {
            self.m_physical_device_selector.requireQueueFamilies({ family });
        }
        self.m_device_builder.requireQueueFamilies(families);
        return std::forward<Self>(self);
    }
    
    template<typename Self>
    [[nodiscard]] auto&& requireFormats(this Self&& self, InputSpan<Format> const formats) {
        self.m_swapchain_builder.requireFormats(formats);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& preferFormats(this Self&& self, InputSpan<TrivialPair<Format, int32_t>> const formats) {
        self.m_swapchain_builder.preferFormats(formats);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& requireColorSpaces(this Self&& self, InputSpan<ColorSpace> const color_spaces) {
        self.m_swapchain_builder.requireColorSpaces(color_spaces);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& preferColorSpaces(
        this Self&& self,
        InputSpan<TrivialPair<ColorSpace, int32_t>> const color_spaces
    ) {
        self.m_swapchain_builder.preferColorSpaces(color_spaces);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& requirePresentModes(this Self&& self, InputSpan<PresentMode> const present_modes) {
        self.m_swapchain_builder.requirePresentModes(present_modes);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& preferPresentModes(
        this Self&& self,
        InputSpan<TrivialPair<PresentMode, int32_t>> const present_modes
    ) {
        self.m_swapchain_builder.preferPresentModes(present_modes);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& transform(this Self&& self, SurfaceTransformBits const transform) {
        self.m_swapchain_builder.transform(transform);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& renderTo(this Self&& self, Window const& window) {
        self.m_instance_builder.requireWindowExtensions();
        self.m_physical_device_selector.requireQueueFamilies({
            QueueFamily::Graphics,
            QueueFamily::Present,
        });
        self.m_physical_device_selector.requireExtensions({ VulkanExtension::Swapchain });
        self.m_device_builder.requireQueueFamilies({{QueueFamily::Graphics, 1.f}, {QueueFamily::Present, 1.f}});
        self.m_swapchain_builder.fallbackExtent(window.framebufferSize());
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& requireMeshShaders(this Self&& self) {
        self.m_physical_device_selector.requireExtensions({ VulkanExtension::MeshShader });
        self.m_physical_device_selector.requireFeatures({ VulkanFeature::MeshShader });
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& preferMeshShaders(this Self&& self) {
        self.m_physical_device_selector.preferExtensions({ VulkanExtension::MeshShader });
        self.m_physical_device_selector.preferFeatures({ VulkanFeature::MeshShader });
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& requireTaskShaders(this Self&& self) {
        self.m_physical_device_selector.requireFeatures({ VulkanFeature::TaskShader });
        return std::forward<Self>(self);
    }
public:
    [[nodiscard]]
    Instance buildInstance(VulkanCaps& out_caps) {
        return m_instance_builder.build(out_caps);
    }

    [[nodiscard]]
    PhysicalDevice selectPhysicalDevice(VulkanCaps& out_caps, Instance const& instance, Surface const* surface = nullptr) {
        return m_physical_device_selector.select(out_caps, instance, surface);
    }

    [[nodiscard]]
    Device buildDevice(VulkanCaps& out_caps, PhysicalDevice const& physical_device) {
        return m_device_builder.build(out_caps, physical_device);
    }

    [[nodiscard]]
    Swapchain buildSwapchain(
        VulkanCaps& out_caps,
        Device const& device,
        PhysicalDevice const& physical_device,
        Surface const& surface
    ) {
        return m_swapchain_builder.build(out_caps, device, physical_device, surface);
    }

    [[nodiscard]]
    Swapchain rebuildSwapchain(
        VulkanCaps& out_caps,
        Device const& device,
        PhysicalDevice const& physical_device,
        Surface const& surface,
        Swapchain const& old_swapchain
    ) {
        return m_swapchain_builder.build(out_caps, device, physical_device, surface, &old_swapchain);
    }
private:
    InstanceBuilder m_instance_builder;
    PhysicalDeviceSelector m_physical_device_selector;
    DeviceBuilder m_device_builder;
    SwapchainBuilder m_swapchain_builder;
};

} // namespace core::vk
