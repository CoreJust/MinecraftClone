#pragma once

#include <core/common/ConstString.hpp>

#include <fmt/core.h>

namespace core {

template<typename T, typename Func>
concept JoinableContainer = requires (T const& c, Func const& func, fmt::context::iterator out) {
    std::begin(c);
    std::end(c);
    requires false
        || requires { fmt::format("{}", func(*std::begin(c))); }
        || requires { out = func(out, *std::begin(c)); };
    typename T::iterator;
};

template<typename Func, JoinableContainer<Func> Container, ConstString Separator, ConstString First, ConstString Last>
struct JoinContainerDummy final {
    using Iterator = Container::const_iterator;
    Iterator first;
    Iterator last;
    [[no_unique_address]] Func func;
};

template<
    ConstString Separator = ", ",
    ConstString First = "{",
    ConstString Last = "}",
    typename Func,
    JoinableContainer<Func> Container
>
[[nodiscard]] auto joinFmt(Container const& container, Func func) {
    return JoinContainerDummy<Func, Container, Separator, First, Last>{
        .first = std::begin(container),
        .last = std::end(container),
        .func = std::move(func),
    };
}

struct IdFunc final {
    template<typename T>
    static auto&& operator()(T&& t) noexcept { return std::forward<T>(t); }
};

template<
    ConstString Separator = ", ",
    ConstString First = "{",
    ConstString Last = "}",
    JoinableContainer<IdFunc> Container
>
[[nodiscard]] auto joinFmt(Container const& container) {
    return JoinContainerDummy<IdFunc, Container, Separator, First, Last>{
        .first = std::begin(container),
        .last = std::end(container),
        .func = IdFunc{ },
    };
}

} // namespace core

namespace fmt {

template<typename Func, typename T, core::ConstString Separator, core::ConstString First, core::ConstString Last>
struct formatter<core::JoinContainerDummy<Func, T, Separator, First, Last>> : formatter<std::string_view> {
    context::iterator format(
        core::JoinContainerDummy<Func, T, Separator, First, Last> const c,
        format_context& ctx
    ) const {
        auto out = ctx.out();
        out = fmt::format_to(out, "{}", std::string_view{First});
        bool first = true;
        for (auto it = c.first; it != c.last; ++it) {
            if (!first) {
                out = fmt::format_to(out, "{}", std::string_view{Separator});
            }
            first = false;
            if constexpr (requires{ c.func(*it); }) {
                out = fmt::format_to(out, "{}", c.func(*it));
            } else {
                out = c.func(out, *it);
            }
        }
        out = fmt::format_to(out, "{}", std::string_view{Last});
        return out;
    }
};

} // namespace fmt
