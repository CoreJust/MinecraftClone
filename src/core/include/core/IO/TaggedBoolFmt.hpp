#pragma once

#include <core/meta/TaggedBool.hpp>
#include <core/meta/TypeName.hpp>

#include <fmt/core.h>

namespace fmt {

template<typename Tag>
struct formatter<core::TaggedBool<Tag>> : formatter<std::string_view> {
    context::iterator format(core::TaggedBool<Tag> const value, format_context& ctx) const {
        static constexpr std::string_view TYPE_NAME = core::typeName<Tag>()
            .substr(0, std::size(core::typeName<Tag>()) - 3);
        return format_to(ctx.out(), "{}({})", TYPE_NAME, value.toStringView());
    }
};

} // namespace fmt
