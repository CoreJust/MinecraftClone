#pragma once

#include <core/meta/Enum.hpp>

#include <fmt/core.h>

namespace fmt {

template <core::CountableEnum E>
struct formatter<E> : formatter<std::string_view> {
    context::iterator format(E const& e, format_context& ctx) const {
        return format_to(ctx.out(), "{}", core::toStringView(e));
    }
};

} // namespace fmt
