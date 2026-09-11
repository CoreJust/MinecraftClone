#pragma once

#include <vulkan/vulkan.h>

#include <optional>
#include <span>
#include <vector>

namespace client {

struct DepthFormatSupport final {
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool depth_attachment_supported = false;
};

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
