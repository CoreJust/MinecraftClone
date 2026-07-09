#pragma once

#include <core/common/Assert.hpp>
#include <core/meta/Enum.hpp>
#include <core/meta/TypeName.hpp>

#include <string>
#include <unordered_map>

namespace core {

template<CountableEnum E>
struct EnumTraits final {
private:
    template<size_t... Is>
    struct ValuesHelper final {
        static constexpr EnumValues<E> VALUE { static_cast<E>(Is)... };
    };
    template<size_t... Is>
    struct EntriesHelper final {
        static constexpr EnumEntries<E> VALUE { { Is, static_cast<E>(Is) }... };
    };
    template<size_t... Is>
    struct NameesHelper final {
        static constexpr std::string_view VALUE[] { valueName<static_cast<E>(Is)>()..., "Count" };
    };

    static constexpr std::make_index_sequence<countOf<E>()> SEQ{ };

    template<template<size_t...> typename Helper, size_t... Is>
    static consteval auto const& helperValue(std::index_sequence<Is...>) {
        return Helper<Is...>::VALUE;
    }
public:
    static constexpr EnumValues<E> const& VALUES = helperValue<ValuesHelper>(SEQ);
    static constexpr EnumEntries<E> const& ENTRIES = helperValue<EntriesHelper>(SEQ);
    static constexpr auto const& NAMES = helperValue<NameesHelper>(SEQ);
    
    inline static std::unordered_map<std::string_view, E> NAME_TO_VALUE = std::invoke([] {
        std::unordered_map<std::string_view, E> result;
        result.reserve(countOf<E>() + 1);
        for (auto [i, value] : ENTRIES) {
            result.emplace(NAMES[i], value);
        }
        result.emplace("Count", E::Count);
        return result;
    });
};

// Must be in namespace core
// Must correspond to CORE_ENUM_FUNCTIONS in a header file, must be out of any namespace
#define CORE_ENUM_FUNCTIONS_IMPL(E)                                        \
namespace core {                                                           \
    template<>                                                             \
    std::string_view toStringView<E>(E const value) noexcept {             \
        ASSERT(indexOf(value) <= countOf<E>());                            \
        return EnumTraits<E>::NAMES[static_cast<size_t>(value)];           \
    }                                                                      \
    template<>                                                             \
    std::optional<E> parseEnum<E>(std::string_view const value) noexcept { \
        auto it = EnumTraits<E>::NAME_TO_VALUE.find(value);                \
        if (it != EnumTraits<E>::NAME_TO_VALUE.end()) {                    \
            return it->second;                                             \
        }                                                                  \
        return std::nullopt;                                               \
    }                                                                      \
    template<>                                                             \
    EnumValues<E> const& valuesOf<E>() noexcept {                          \
        return EnumTraits<E>::VALUES;                                      \
    }                                                                      \
    template<>                                                             \
    EnumEntries<E> const& entriesOf<E>() noexcept {                        \
        return EnumTraits<E>::ENTRIES;                                     \
    }                                                                      \
} // namespace core

} // namespace core
