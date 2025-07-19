#pragma once

namespace core {
    template<bool... Conds>
    consteval bool Any = (Conds || ... || false);
} // namespace core
