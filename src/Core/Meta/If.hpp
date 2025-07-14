#pragma once

namespace core::meta {
    template<bool Condition, typename Then, typename Else>
    struct IfImpl { };
    template<typename Then, typename Else>
    struct IfImpl<true, Then, Else> {
        using Type = Then;
    };
    template<typename Then, typename Else>
    struct IfImpl<false, Then, Else> {
        using Type = Else;
    };

    template<bool Condition, typename Then, typename Else>
    using If = IfImpl<Condition, Then, Else>::Type;
} // namespace core::meta
