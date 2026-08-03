#pragma once

#include <core/common/TrivialPair.hpp>

#include <optional>
#include <string_view>

namespace core {

// CountableEnum requires E::Count to exist, AND the enum's values must be
// DENSE: 0, 1, 2, ..., Count-1. The reflection system (valuesOf, entriesOf)
// generates values via static_cast<E>(0..N-1) and assumes density.
template<typename E>
concept CountableEnum = requires { static_cast<size_t>(E::Count); };

template<CountableEnum E>
using EnumEntry = TrivialPair<size_t, E>;

template<CountableEnum E> [[nodiscard]]
consteval size_t countOf() noexcept { return static_cast<size_t>(E::Count); }
template<CountableEnum E> [[nodiscard]]
constexpr size_t indexOf(E const value) noexcept { return static_cast<size_t>(value); }

template<CountableEnum E>
using EnumValues = E[countOf<E>()];
template<CountableEnum E>
using EnumEntries = EnumEntry<E>[countOf<E>()];

template<CountableEnum E> [[nodiscard]]
std::string_view toStringView(E const) noexcept = delete;
template<CountableEnum E> [[nodiscard]]
std::optional<E> parseEnum(std::string_view const) noexcept = delete;
template<CountableEnum E> [[nodiscard]]
EnumValues<E> const& valuesOf() noexcept = delete;
template<CountableEnum E> [[nodiscard]]
EnumEntries<E> const& entriesOf() noexcept = delete;

/*
 * Use for enums that have a Count member and are dense.
 * Make sure to place it in namespace core in header.
 * There must be a corresponding source file where CORE_ENUM_FUNCTIONS_IMPL
 * from EnumImpl.hpp is declared.
 * Should be placed out of any namespace.
 */
#define CORE_ENUM_FUNCTIONS(E)                                      \
namespace core {                                                    \
    template<> [[nodiscard]]                                        \
    std::string_view toStringView<E>(E const) noexcept;             \
    template<> [[nodiscard]]                                        \
    std::optional<E> parseEnum<E>(std::string_view const) noexcept; \
    template<> [[nodiscard]]                                        \
    EnumValues<E> const& valuesOf<E>() noexcept;                    \
    template<> [[nodiscard]]                                        \
    EnumEntries<E> const& entriesOf<E>() noexcept;                  \
}

} // namespace core
