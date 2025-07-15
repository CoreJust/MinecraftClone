#pragma once

namespace core {
    template<typename... Ts>
    struct TypeList {
        inline constexpr static auto Size = sizeof...(Ts);
    };

    template<typename List, unsigned int Index>
    struct TypeAtImpl { };

    template<typename T, typename... Ts>
    struct TypeAtImpl<TypeList<T, Ts...>, 0> {
        using Type = T;
    };
    
    template<unsigned int Index, typename T, typename... Ts>
    struct TypeAtImpl<TypeList<T, Ts...>, Index> {
        using Type = TypeAtImpl<TypeList<Ts...>, Index - 1>::Type;
    };

    template<typename TypeList, unsigned int Index>
    using TypeAt = TypeAtImpl<TypeList, Index>::Type;

    template<typename TypeList>
    using FirstOf = TypeAtImpl<TypeList, 0>::Type;

    template<typename TypeList>
    using LastOf = TypeAtImpl<TypeList, TypeList::Size - 1>::Type;
} // namespace core
