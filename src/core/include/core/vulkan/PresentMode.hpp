#pragma once

#include <core/meta/Enum.hpp>

namespace core {

enum class PresentMode {
    Immediate,
    Mailbox,
    FIFO,
    FIFORelaxed,

    Count,
};

CORE_ENUM_FUNCTIONS(PresentMode);

} // namespace core
