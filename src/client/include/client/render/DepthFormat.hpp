#pragma once

#include <core/graphics/vulkan/Vulkan.hpp>

#include <optional>
#include <span>
#include <vector>

namespace client {

struct DepthFormatSupport final {
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool depth_attachment_supported = false;
};

struct PresentationPipelineDescriptors final {
    core::graphics::vulkan::PipelineDescriptor grid{};
    core::graphics::vulkan::PipelineDescriptor player{};
    core::graphics::vulkan::PipelineDescriptor debug_hud{};
};

[[nodiscard]]
constexpr PresentationPipelineDescriptors presentationPipelineDescriptors(
    VkPipelineLayout const grid_layout,
    VkPipelineLayout const player_layout,
    VkPipelineLayout const debug_hud_layout,
    VkFormat const color_format,
    VkFormat const depth_format
) noexcept
{
    auto descriptor = [color_format, depth_format](
        VkPipelineLayout const layout,
        bool const depth_test_enabled,
        bool const depth_write_enabled
    ) constexpr {
        return core::graphics::vulkan::PipelineDescriptor{
            .layout = layout,
            .color_format = color_format,
            .depth_format = depth_format,
            .depth_test_enabled = depth_test_enabled,
            .depth_write_enabled = depth_write_enabled,
            .depth_compare_op = VK_COMPARE_OP_LESS,
        };
    };
    return {
        .grid = descriptor(grid_layout, true, true),
        .player = descriptor(player_layout, true, true),
        .debug_hud = descriptor(debug_hud_layout, false, false),
    };
}

[[nodiscard]]
std::optional<VkFormat> selectDepthAttachmentFormat(
    std::span<DepthFormatSupport const> candidates
) noexcept;

class DepthTargetSelection final {
public:
    [[nodiscard]] bool select(VkImage color_image);
    void remove(VkImage color_image) noexcept;
    void clear() noexcept;
    [[nodiscard]] size_t size() const noexcept;

private:
    std::vector<VkImage> m_color_images;
};

} // namespace client
