#pragma once

#include <core/meta/Enum.hpp>

namespace core::vk {

enum class PresentMode {
    Immediate,
    Mailbox,
    FIFO,
    FIFORelaxed,

    Count,
};

} // namespace core::vk

CORE_ENUM_FUNCTIONS(::core::vk::PresentMode);
