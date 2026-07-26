#pragma once

#include <cstddef>
#include <type_traits>

namespace core {

template<typename E, typename RawType>
struct EnumMapping final {
    using Raw = RawType;

    [[nodiscard]]
    static constexpr E unmap(Raw const v) noexcept { return static_cast<E>(v); }
    [[nodiscard]]
    static constexpr Raw map(E const e) noexcept { return static_cast<Raw>(e); }
};

} // namespace core

/*
 * Allows to define a custom mapping between an enum and some other type (e.g. integral or an unscoped enum).
 * Must be placed in global namespace.
 * Usage:
 * CORE_DEFINE_ENUM_MAPPING(MappedEnum, MappedType,
 *     (Mapped enum anchor, Value),
 *     ...
 * )
 *
 * Example:
 * enum class Enum {
 *     A, B, C, D, E, F,
 * };
 * 
 * CORE_DEFINE_ENUM_MAPPING(Enum, uint32_t,
 *     { C, 32 },
 *     { E, 64 }
 * )
 * using Mapping = core::EnumMapping<Enum, uint32_t>;
 * 
 * Now:
 * Mapping::map(Enum::A) -> 0
 * Mapping::map(Enum::B) -> 1
 * Mapping::map(Enum::C) -> 32
 * Mapping::map(Enum::D) -> 33
 * Mapping::map(Enum::E) -> 64
 * Mapping::map(Enum::F) -> 65
 *
 * And conversely, those numbers will be unmapped to the corresponding enum values.
 */
#define CORE_DEFINE_ENUM_MAPPING(EnumType, RawType, ...)               \
namespace core {                                                       \
    template<>                                                         \
    struct EnumMapping<EnumType, RawType> final {                     \
        using Raw = RawType;                                           \
    private:                                                           \
        using enum EnumType;                                           \
        struct Anchor final {                                          \
            EnumType enum_value;                                       \
            RawType raw;                                               \
        };                                                             \
        static constexpr Anchor ANCHORS[] { __VA_ARGS__ };             \
    public:                                                            \
        [[nodiscard]]                                                  \
        static constexpr EnumType unmap(Raw const v) noexcept {        \
            Anchor const* best = nullptr;                              \
            for (auto const& a : ANCHORS) {                            \
                if (a.raw <= v) {                                      \
                    best = &a;                                         \
                }                                                      \
            }                                                          \
            if (!best) {                                               \
                return static_cast<EnumType>(v);                       \
            }                                                          \
            return static_cast<EnumType>(                              \
                static_cast<Raw>(best->enum_value) + (v - best->raw)   \
            );                                                         \
        }                                                              \
                                                                       \
        [[nodiscard]]                                                  \
        static constexpr Raw map(EnumType const e) noexcept {          \
            Anchor const* best = nullptr;                              \
            for (auto const& a : ANCHORS) {                            \
                if (a.enum_value <= e) {                               \
                    best = &a;                                         \
                }                                                      \
            }                                                          \
            if (!best) {                                               \
                return static_cast<Raw>(e);                            \
            }                                                          \
            return best->raw                                           \
                + static_cast<Raw>(e)                                  \
                - static_cast<Raw>(best->enum_value);                  \
        }                                                              \
    };                                                                 \
}
