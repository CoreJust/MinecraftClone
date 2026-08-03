#pragma once

#include <core/common/InputSpan.hpp>
#include <core/common/VectorUtils.hpp>
#include <core/vulkan/Capabilities.hpp>
#include <core/vulkan/Device.hpp>
#include <core/vulkan/Error.hpp>
#include <core/vulkan/Extent.hpp>
#include <core/vulkan/PhysicalDevice.hpp>
#include <core/vulkan/Surface.hpp>
#include <core/vulkan/Swapchain.hpp>
#include <core/vulkan/enum/ColorSpace.hpp>
#include <core/vulkan/enum/PresentMode.hpp>
#include <core/vulkan/enum/SurfaceTransform.hpp>

CORE_VK_ERROR_WITH_KINDS(SwapchainCreationError, VulkanInitializationError,
    NoSuchFormat,
    NoSuchColorSpace,
    NoSuchPresentMode,
    CapabilitiesQueryFailed,
    NoFallbackExtentProvided,
    FailedToCreateSwapchain,
    FailedToGetSwapchainImages);

namespace core::vk {

class SwapchainBuilder final {
public:
    [[nodiscard]]
    std::span<Format const> requiredFormats() const noexcept { return m_required_formats; }

    [[nodiscard]]
    std::span<TrivialPair<Format, int32_t> const> preferredFormats() const noexcept { return m_preferred_formats; }

    [[nodiscard]]
    std::span<ColorSpace const> requiredColorSpaces() const noexcept { return m_required_color_spaces; }

    [[nodiscard]]
    std::span<TrivialPair<ColorSpace, int32_t> const> preferredColorSpaces() const noexcept { return m_preferred_color_spaces; }

    [[nodiscard]]
    std::span<PresentMode const> requiredPresentModes() const noexcept { return m_required_present_modes; }

    [[nodiscard]]
    std::span<TrivialPair<PresentMode, int32_t> const> preferredPresentModes() const noexcept { return m_preferred_present_modes; }

    [[nodiscard]]
    std::optional<Extent2d> fallbackExtent() const noexcept { return m_fallback_extent; }

    [[nodiscard]]
    std::optional<SurfaceTransformBits> transform() const noexcept { return m_transform; }
public:
    // Defaults to all formats allowed
    template<typename Self>
    auto&& requireFormats(
        this Self&& self,
        InputSpan<Format> const formats
    ) {
        appendRange(self.m_required_formats, formats);
        return std::forward<Self>(self);
    }

    // Defaults to B8G8R8A8SRGB
    template<typename Self>
    auto&& preferFormats(
        this Self&& self,
        InputSpan<TrivialPair<Format, int32_t>> const formats
    ) {
        appendRange(self.m_preferred_formats, formats);
        return std::forward<Self>(self);
    }

    // Defaults to all color spaces allowed
    template<typename Self>
    auto&& requireColorSpaces(
        this Self&& self,
        InputSpan<ColorSpace> const color_spaces
    ) {
        appendRange(self.m_required_color_spaces, color_spaces);
        return std::forward<Self>(self);
    }

    // Defaults to SRGBNonlinear
    template<typename Self>
    auto&& preferColorSpaces(
        this Self&& self,
        InputSpan<TrivialPair<ColorSpace, int32_t>> const color_spaces
    ) {
        appendRange(self.m_preferred_color_spaces, color_spaces);
        return std::forward<Self>(self);
    }

    // Defaults to all present modes allowed
    template<typename Self>
    auto&& requirePresentModes(
        this Self&& self,
        InputSpan<PresentMode> const present_modes
    ) {
        appendRange(self.m_required_present_modes, present_modes);
        return std::forward<Self>(self);
    }

    // Defaults to {Mailbox=1, FIFO=0}
    template<typename Self>
    auto&& preferPresentModes(
        this Self&& self,
        InputSpan<TrivialPair<PresentMode, int32_t>> const present_modes
    ) {
        appendRange(self.m_preferred_present_modes, present_modes);
        return std::forward<Self>(self);
    }

    template<typename Self>
    auto&& fallbackExtent(
        this Self&& self,
        Extent2d const extent
    ) {
        self.m_fallback_extent = extent;
        return std::forward<Self>(self);
    }

    template<typename Self>
    auto&& fallbackExtent(
        this Self&& self,
        TrivialPair<uint32_t, uint32_t> const extent
    ) {
        return self.fallbackExtent(Extent2d{ extent.first, extent.second });
    }

    // Defaults to the one inquired from surface
    template<typename Self>
    auto&& transform(
        this Self&& self,
        SurfaceTransformBits const transform
    ) {
        self.m_transform = transform;
        return std::forward<Self>(self);
    }

    [[nodiscard]]
    Swapchain build(
        VulkanCaps& out_caps,
        Device const& device,
        PhysicalDevice const& physical_device,
        Surface const& surface,
        Swapchain const* old_swapchain = nullptr
    );
private:
    std::vector<Format> m_required_formats;
    std::vector<TrivialPair<Format, int32_t>> m_preferred_formats;
    std::vector<ColorSpace> m_required_color_spaces;
    std::vector<TrivialPair<ColorSpace, int32_t>> m_preferred_color_spaces;
    std::vector<PresentMode> m_required_present_modes;
    std::vector<TrivialPair<PresentMode, int32_t>> m_preferred_present_modes;
    std::optional<Extent2d> m_fallback_extent;
    std::optional<SurfaceTransformBits> m_transform;
};

} // namespace core::vk
