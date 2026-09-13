#include <client/render/DepthFormat.hpp>

#include <algorithm>

namespace client {

std::optional<VkFormat> selectDepthAttachmentFormat(
    std::span<DepthFormatSupport const> const candidates
) noexcept
{
    for (DepthFormatSupport const candidate : candidates) {
        if (candidate.depth_attachment_supported) {
            return candidate.format;
        }
    }
    return std::nullopt;
}

bool DepthTargetSelection::select(VkImage const color_image)
{
    if (std::ranges::find(m_color_images, color_image) != m_color_images.end()) {
        return false;
    }
    m_color_images.push_back(color_image);
    return true;
}

void DepthTargetSelection::remove(VkImage const color_image) noexcept
{
    std::erase(m_color_images, color_image);
}

void DepthTargetSelection::clear() noexcept
{
    m_color_images.clear();
}

size_t DepthTargetSelection::size() const noexcept
{
    return m_color_images.size();
}

} // namespace client
