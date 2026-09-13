#include <acceptance/FramebufferExtent.hpp>

#include <core/platform/glfw/GlfwWindow.hpp>

#include <GLFW/glfw3.h>

#include <limits>
#include <optional>

namespace acceptance {

namespace {

[[nodiscard]]
std::expected<uint32_t, std::string> adjustedDimension(
    uint32_t requested_framebuffer,
    uint32_t logical_window,
    uint32_t actual_framebuffer
)
{
    if (requested_framebuffer == 0U || logical_window == 0U || actual_framebuffer == 0U) {
        return std::unexpected("framebuffer matching requires non-zero dimensions");
    }
    uint64_t const numerator = static_cast<uint64_t>(requested_framebuffer)
        * static_cast<uint64_t>(logical_window);
    uint64_t const adjusted = (numerator + static_cast<uint64_t>(actual_framebuffer) / 2U)
        / actual_framebuffer;
    if (adjusted == 0U || adjusted > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        return std::unexpected("requested framebuffer extent is not supported by GLFW logical dimensions");
    }
    return static_cast<uint32_t>(adjusted);
}

[[nodiscard]]
PixelExtent framebufferExtent(core::platform::glfw::GlfwWindow const& window) noexcept
{
    PixelExtent extent;
    window.framebufferSize(extent.width, extent.height);
    return extent;
}

[[nodiscard]]
PixelExtent logicalWindowExtent(GLFWwindow* const window) noexcept
{
    int width = 0;
    int height = 0;
    glfwGetWindowSize(window, &width, &height);
    return {
        .width = width > 0 ? static_cast<uint32_t>(width) : 0U,
        .height = height > 0 ? static_cast<uint32_t>(height) : 0U,
    };
}

[[nodiscard]]
bool sameExtent(PixelExtent const lhs, PixelExtent const rhs) noexcept
{
    return lhs.width == rhs.width && lhs.height == rhs.height;
}

} // namespace

std::expected<PixelExtent, std::string> adjustedLogicalWindowExtent(
    PixelExtent const requested_framebuffer,
    PixelExtent const logical_window,
    PixelExtent const actual_framebuffer
)
{
    std::expected<uint32_t, std::string> const width = adjustedDimension(
        requested_framebuffer.width,
        logical_window.width,
        actual_framebuffer.width
    );
    if (!width.has_value()) {
        return std::unexpected(width.error());
    }
    std::expected<uint32_t, std::string> const height = adjustedDimension(
        requested_framebuffer.height,
        logical_window.height,
        actual_framebuffer.height
    );
    if (!height.has_value()) {
        return std::unexpected(height.error());
    }
    return PixelExtent{ .width = *width, .height = *height };
}

std::expected<void, std::string> ensureFramebufferExtent(
    core::platform::glfw::GlfwWindow const& window,
    PixelExtent const requested_framebuffer,
    uint32_t const max_resize_polls
)
{
    if (requested_framebuffer.width == 0U || requested_framebuffer.height == 0U || max_resize_polls == 0U) {
        return std::unexpected("framebuffer matching requires non-zero extent and poll limit");
    }
    GLFWwindow* const native_window = window.nativeHandle();
    if (native_window == nullptr) {
        return std::unexpected("framebuffer matching window has no native handle");
    }
    std::optional<PixelExtent> previous_logical_extent;
    for (uint32_t poll = 0U; poll < max_resize_polls; ++poll) {
        window.pollEvents();
        PixelExtent const actual_framebuffer = framebufferExtent(window);
        if (actual_framebuffer.width == requested_framebuffer.width
            && actual_framebuffer.height == requested_framebuffer.height) {
            return { };
        }
        if (window.shouldClose()) {
            return std::unexpected("framebuffer matching window was closed");
        }
        if (poll + 1U == max_resize_polls) {
            break;
        }
        PixelExtent const logical_extent = logicalWindowExtent(native_window);
        std::expected<PixelExtent, std::string> const adjusted = adjustedLogicalWindowExtent(
            requested_framebuffer,
            logical_extent,
            actual_framebuffer
        );
        if (!adjusted.has_value()) {
            return std::unexpected(adjusted.error());
        }
        if (sameExtent(*adjusted, logical_extent)
            || (previous_logical_extent.has_value() && sameExtent(*adjusted, *previous_logical_extent))) {
            return std::unexpected("framebuffer matching cannot make bounded logical-size progress");
        }
        glfwSetWindowSize(
            native_window,
            static_cast<int>(adjusted->width),
            static_cast<int>(adjusted->height)
        );
        previous_logical_extent = logical_extent;
    }
    PixelExtent const actual_framebuffer = framebufferExtent(window);
    return std::unexpected(
        "renderer window did not retain the requested framebuffer extent after "
        + std::to_string(max_resize_polls) + " polls: requested "
        + std::to_string(requested_framebuffer.width) + "x"
        + std::to_string(requested_framebuffer.height) + ", actual "
        + std::to_string(actual_framebuffer.width) + "x"
        + std::to_string(actual_framebuffer.height)
    );
}

} // namespace acceptance
