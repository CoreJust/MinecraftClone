#include <core/vulkan/Attachment.hpp>

#include <core/common/Assert.hpp>

namespace core::vk {

AttachmentView AttachmentViewProvider::operator[](AttachmentViewId const id) const {
    ASSERT(static_cast<size_t>(id) < m_attachment_views.size(), "Attachment view ID out of range");
    auto [image, image_view] = m_attachment_views[static_cast<size_t>(id)];

    ASSERT(image != nullptr && image_view != nullptr, "Attachment no. {} is not bound", static_cast<uint32_t>(id));
    return { .image = *image, .image_view = *image_view };
}

void AttachmentViewProvider::bind(AttachmentViewId const id, AttachmentView const view) {
    ASSERT(static_cast<size_t>(id) < m_attachment_views.size(), "Attachment view ID out of range");
    m_attachment_views[static_cast<size_t>(id)] = { .first = &view.image, .second = &view.image_view };
}

void AttachmentViewProvider::unbindAll() noexcept {
    for (auto& [image, image_view] : m_attachment_views) {
        image = nullptr;
        image_view = nullptr;
    }
}

} // namespace core::vk
