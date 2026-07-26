#include <core/vulkan/builder/SwapchainBuilder.hpp>

#include <core/IO/Log.hpp>
#include <core/meta/EnumImpl.hpp>
#include <core/vulkan/Check.hpp>

#include <volk.h>

#include <unordered_set>

CORE_ENUM_FUNCTIONS_IMPL(::core::vk::SwapchainCreationErrorKind);

namespace core::vk {
namespace {

[[nodiscard]]
TrivialPair<Format, ColorSpace> chooseSurfaceFormat(
    PhysicalDevice const& physical_device,
    Surface const& surface,
    std::vector<Format> const& required_formats,
    std::vector<TrivialPair<Format, int32_t>> const& preferred_formats,
    std::vector<ColorSpace> const& required_color_spaces,
    std::vector<TrivialPair<ColorSpace, int32_t>> const& preferred_color_spaces
) {
    uint32_t format_count = 0;
    if (!VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(
        physical_device.handle(),
        surface.handle(),
        &format_count,
        nullptr
    ))) {
        throw SwapchainCreationError(SwapchainCreationError::CapabilitiesQueryFailed, "{}", "vkGetPhysicalDeviceSurfaceFormatsKHR");
    }
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    if (!VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(
        physical_device.handle(),
        surface.handle(),
        &format_count,
        formats.data()
    ))) {
        throw SwapchainCreationError(SwapchainCreationError::CapabilitiesQueryFailed, "{}", "vkGetPhysicalDeviceSurfaceFormatsKHR");
    }

    std::unordered_set<VkFormat> found_formats;
    std::unordered_set<VkColorSpaceKHR> found_color_spaces;
    found_formats.reserve(format_count);
    found_color_spaces.reserve(format_count);

    for (VkSurfaceFormatKHR const& f : formats) {
        found_formats.insert(f.format);
        found_color_spaces.insert(f.colorSpace);
    }

    if (!required_formats.empty()) {
        bool found_some_required_format = false;
        for (Format const f : required_formats) {
            if (found_formats.contains(static_cast<VkFormat>(f))) {
                found_some_required_format = true;
                break;
            }
        }
        if (!found_some_required_format) {
            throw SwapchainCreationError(
                SwapchainCreationError::NoSuchFormat,
                "only following formats are supported: {}",
                joinFmt(found_formats, [](VkFormat const f){ return static_cast<Format>(f); }));
        }
    }

    if (!required_color_spaces.empty()) {
        bool found_some_required_color_space = false;
        for (ColorSpace const cs : required_color_spaces) {
            if (found_color_spaces.contains(static_cast<VkColorSpaceKHR>(colorSpaceToVk(cs)))) {
                found_some_required_color_space = true;
                break;
            }
        }
        if (!found_some_required_color_space) {
            throw SwapchainCreationError(
                SwapchainCreationError::NoSuchColorSpace,
                "only following color spaces are supported: {}",
                joinFmt(found_color_spaces, [](VkColorSpaceKHR const cs){ return vkToColorSpace(cs); }));
        }
    }

    TrivialPair<Format, int32_t> best_format {
        static_cast<Format>(*found_formats.begin()),
        std::numeric_limits<int32_t>::min(),
    };
    TrivialPair<ColorSpace, int32_t> best_color_space {
        static_cast<ColorSpace>(vkToColorSpace(*found_color_spaces.begin())),
        std::numeric_limits<int32_t>::min(),
    };
    for (auto const [f, value] : preferred_formats) {
        if (value > best_format.second && found_formats.contains(static_cast<VkFormat>(f))) {
            best_format.first = f;
            best_format.second = value;
        }
    }
    for (auto const [cs, value] : preferred_color_spaces) {
        if (value > best_color_space.second && found_color_spaces.contains(static_cast<VkColorSpaceKHR>(colorSpaceToVk(cs)))) {
            best_color_space.first = cs;
            best_color_space.second = value;
        }
    }

    return { best_format.first, best_color_space.first };
}

[[nodiscard]]
PresentMode choosePresentMode(
    PhysicalDevice const& physical_device,
    Surface const& surface,
    std::vector<PresentMode> const& required,
    std::vector<TrivialPair<PresentMode, int32_t>> const& preferred
) {
    uint32_t present_mode_count = 0;
    if (!VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(
        physical_device.handle(),
        surface.handle(),
        &present_mode_count,
        nullptr
    ))) {
        throw SwapchainCreationError(
            SwapchainCreationError::CapabilitiesQueryFailed,
            "{}", "vkGetPhysicalDeviceSurfacePresentModesKHR"
        );
    }
    std::vector<VkPresentModeKHR> present_modes(present_mode_count);
    if (!VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(
        physical_device.handle(),
        surface.handle(),
        &present_mode_count,
        present_modes.data()
    ))) {
        throw SwapchainCreationError(
            SwapchainCreationError::CapabilitiesQueryFailed,
            "{}", "vkGetPhysicalDeviceSurfacePresentModesKHR"
        );
    }

    std::unordered_set<PresentMode> found_modes;
    found_modes.reserve(present_mode_count);

    for (VkPresentModeKHR const mode : present_modes) {
        found_modes.insert(static_cast<PresentMode>(mode));
    }

    if (!required.empty()) {
        bool found_some_present_mode = false;
        for (PresentMode const mode : required) {
            if (found_modes.contains(mode)) {
                found_some_present_mode = true;
                break;
            }
        }
        if (!found_some_present_mode) {
            throw SwapchainCreationError(
                SwapchainCreationError::NoSuchPresentMode,
                "only following present formats are supported: {}",
                joinFmt(found_modes));
        }
    }

    TrivialPair<PresentMode, int32_t> best_present_mode {
        *found_modes.begin(),
        std::numeric_limits<int32_t>::min(),
    };
    for (auto const [mode, value] : preferred) {
        if (value > best_present_mode.second && found_modes.contains(mode)) {
            best_present_mode.first = mode;
            best_present_mode.second = value;
        }
    }

    return best_present_mode.first;
}

} // namespace

Swapchain SwapchainBuilder::build(
    VulkanCaps& out_caps,
    Device const& device,
    PhysicalDevice const& physical_device,
    Surface const& surface,
    Swapchain const* old_swapchain
) {
    if (m_preferred_formats.empty()) {
        m_preferred_formats.emplace_back(Format::B8G8R8A8SRGB, 0);
    }
    if (m_preferred_color_spaces.empty()) {
        m_preferred_color_spaces.emplace_back(ColorSpace::SRGBNonlinear, 0);
    }
    if (m_preferred_present_modes.empty()) {
        m_preferred_present_modes.emplace_back(PresentMode::Mailbox, 1);
        m_preferred_present_modes.emplace_back(PresentMode::FIFO, 0);
    }

    auto [format, color_space] = chooseSurfaceFormat(
        physical_device,
        surface,
        m_required_formats,
        m_preferred_formats,
        m_required_color_spaces,
        m_preferred_color_spaces
    );
    PresentMode mode = choosePresentMode(physical_device, surface, m_required_present_modes, m_preferred_present_modes);
    
    VkSurfaceCapabilitiesKHR capabilities{ };
    if (!VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device.handle(), surface.handle(), &capabilities))) {
        throw SwapchainCreationError(
            SwapchainCreationError::CapabilitiesQueryFailed,
            "{}", "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"
        );
    }

    Extent2d extent{ capabilities.currentExtent.width, capabilities.currentExtent.height };
    if (extent.x == std::numeric_limits<uint32_t>::max()) {
        if (!m_fallback_extent.has_value()) {
            throw SwapchainCreationError(SwapchainCreationError::NoFallbackExtentProvided);
        }
        extent.x = std::clamp(m_fallback_extent->x, capabilities.minImageExtent.width , capabilities.maxImageExtent.width );
        extent.y = std::clamp(m_fallback_extent->y, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }

    uint32_t image_count = capabilities.minImageCount + 1U;
    if (capabilities.maxImageCount > 0U) {
        image_count = std::min(image_count, capabilities.maxImageCount);
    }

    uint32_t queue_family_ndices[2]{
        *physical_device.queueFamily(QueueFamily::Graphics),
        *physical_device.queueFamily(QueueFamily::Present),
    };

    SurfaceTransformBits const surface_transform = m_transform.value_or(
        SurfaceTransformBits{ static_cast<uint32_t>(capabilities.currentTransform) }
    );
    VkSwapchainCreateInfoKHR const create_info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface.handle(),
        .minImageCount = image_count,
        .imageFormat = static_cast<VkFormat>(format),
        .imageColorSpace = static_cast<VkColorSpaceKHR>(colorSpaceToVk(color_space)),
        .imageExtent = {extent.x, extent.y},
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = (queue_family_ndices[0] != queue_family_ndices[1])
            ? VK_SHARING_MODE_CONCURRENT
            : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = (queue_family_ndices[0] != queue_family_ndices[1]) ? 2U : 0U,
        .pQueueFamilyIndices = (queue_family_ndices[0] != queue_family_ndices[1]) ? queue_family_ndices : nullptr,
        .preTransform = static_cast<VkSurfaceTransformFlagBitsKHR>(surface_transform.value),
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = static_cast<VkPresentModeKHR>(mode),
        .clipped = VK_TRUE,
        .oldSwapchain = old_swapchain != nullptr ? old_swapchain->handle() : VK_NULL_HANDLE,
    };

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    if (!VK_CHECK(vkCreateSwapchainKHR(device.handle(), &create_info, nullptr, &swapchain))) {
        throw SwapchainCreationError(SwapchainCreationError::FailedToCreateSwapchain);
    }

    if (!VK_CHECK(vkGetSwapchainImagesKHR(device.handle(), swapchain, &image_count, nullptr))) {
        throw SwapchainCreationError(SwapchainCreationError::FailedToGetSwapchainImages);
    }
    std::vector<VkImage> vk_images(image_count);
    if (!VK_CHECK(vkGetSwapchainImagesKHR(device.handle(), swapchain, &image_count, vk_images.data()))) {
        throw SwapchainCreationError(SwapchainCreationError::FailedToGetSwapchainImages);
    }

    out_caps.commitSwapchainCaps(
        format,
        color_space,
        mode,
        surface_transform,
        extent,
        image_count
    );

    CORE_DEBUG("Swapchain built{}", old_swapchain != nullptr ? " from old swapchain" : "");

    return Swapchain(
        swapchain,
        Image::Info{
            .extent = Extent3d(extent, 1),
            .format = format,
            .usage = ImageUsage::ColorAttachment,
        },
        device
    );
}

} // namespace core::vk
