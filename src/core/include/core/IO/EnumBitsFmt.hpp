#pragma once

#include <core/common/ConstString.hpp>
#include <core/common/EnumBits.hpp>

#include <fmt/core.h>

namespace core {

template<CountableEnum E, ConstString Separator, ConstString First, ConstString Last>
struct JoinEnumBitsDummy final {
    EnumBits<E> value;
};

template<ConstString Separator = " | ", ConstString First = "(", ConstString Last = ")", CountableEnum E>
[[nodiscard]] auto joinFmt(EnumBits<E> const& bits) {
    return JoinEnumBitsDummy<E, Separator, First, Last>{
        .value = bits,
    };
}

} // namespace core

namespace fmt {

template<core::CountableEnum E, core::ConstString Separator, core::ConstString First, core::ConstString Last>
struct formatter<core::JoinEnumBitsDummy<E, Separator, First, Last>> : formatter<std::string_view> {
    context::iterator format(core::JoinEnumBitsDummy<E, Separator, First, Last> const bits, format_context& ctx) const {
        auto out = ctx.out();
        out = fmt::format_to(out, "{}", std::string_view{First});
        bool first = true;
        for (E const bit : core::valuesOf<E>()) {
            if (!bits.value[bit]) {
                continue;
            }
            if (!first) {
                out = fmt::format_to(out, "{}", std::string_view{Separator});
            }
            first = false;
            out = fmt::format_to(out, "{}", core::toStringView(bit));
        }
        out = fmt::format_to(out, "{}", std::string_view{Last});
        return out;
    }
};

} // namespace fmt
