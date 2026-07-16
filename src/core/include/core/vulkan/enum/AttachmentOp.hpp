#pragma once

#include <core/meta/Enum.hpp>

namespace core::vk {

enum class AttachmentLoadOp {
    Load,
    Clear,
    DontCare,
    None,

    Count,
};

enum class AttachmentStoreOp {
    Store,
    DontCare,
    None,

    Count,
};

[[nodiscard]]
constexpr uint32_t attachmentLoadOpToVk(AttachmentLoadOp const op) noexcept {
    return op == AttachmentLoadOp::None
        ? 1'000'400'000
        : static_cast<uint32_t>(op);
}

[[nodiscard]]
constexpr AttachmentLoadOp attachmentLoadOpFromVk(uint32_t const op) noexcept {
    return op == 1'000'400'000
        ? AttachmentLoadOp::None
        : static_cast<AttachmentLoadOp>(op);
}

[[nodiscard]]
constexpr uint32_t attachmentStoreOpToVk(AttachmentStoreOp const op) noexcept {
    return op == AttachmentStoreOp::None
        ? 1'000'301'000
        : static_cast<uint32_t>(op);
}

[[nodiscard]]
constexpr AttachmentStoreOp attachmentStoreOpFromVk(uint32_t const op) noexcept {
    return op == 1'000'301'000
        ? AttachmentStoreOp::None
        : static_cast<AttachmentStoreOp>(op);
}

struct AttachmentOps final {
    AttachmentLoadOp load = AttachmentLoadOp::Load;
    AttachmentStoreOp store = AttachmentStoreOp::Store;
};

} // namespace core::vk

CORE_ENUM_FUNCTIONS(vk::AttachmentLoadOp);
CORE_ENUM_FUNCTIONS(vk::AttachmentStoreOp);
