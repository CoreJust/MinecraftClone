#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace core::platform::glfw {
class GlfwWindow;
} // namespace core::platform::glfw

namespace acceptance {

struct PixelExtent final {
    uint32_t width = 0U;
    uint32_t height = 0U;
};

[[nodiscard]]
std::expected<PixelExtent, std::string> adjustedLogicalWindowExtent(
    PixelExtent requested_framebuffer,
    PixelExtent logical_window,
    PixelExtent actual_framebuffer
);

[[nodiscard]]
std::expected<void, std::string> ensureFramebufferExtent(
    core::platform::glfw::GlfwWindow const& window,
    PixelExtent requested_framebuffer,
    uint32_t max_resize_polls
);

} // namespace acceptance
