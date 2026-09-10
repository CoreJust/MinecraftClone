#pragma once

#include <core/meta/Field.hpp>

namespace core {

enum class FieldComparisonMode {
    Eq,
    Less,
    LessEq,
    Greater,
    GreaterEq,
};

template<typename Struct, typename T, T Struct::*Field, FieldComparisonMode Mode>
struct FieldRequirement final {
    T value;

    [[nodiscard]]
    constexpr bool operator()(Struct const& s) const noexcept {
        T const field = s.*Field;
        switch (Mode) {
            case FieldComparisonMode::Eq:        return value == field;
            case FieldComparisonMode::Less:      return value <  field;
            case FieldComparisonMode::LessEq:    return value <= field;
            case FieldComparisonMode::Greater:   return value >  field;
            case FieldComparisonMode::GreaterEq: return value >= field;
        }
    }
};

template<auto Field, FieldComparisonMode Mode>
auto requireField(TypeOfField<Field> const value) {
    return FieldRequirement<StructOfField<Field>, TypeOfField<Field>, Field, Mode>{
        .value = value,
    };
}

template<auto Field>
auto requireFieldEq(TypeOfField<Field> const value) {
    return requireField<Field, FieldComparisonMode::Eq>(value);
}

template<auto Field>
auto requireFieldLess(TypeOfField<Field> const value) {
    return requireField<Field, FieldComparisonMode::Less>(value);
}

template<auto Field>
auto requireFieldLessEq(TypeOfField<Field> const value) {
    return requireField<Field, FieldComparisonMode::LessEq>(value);
}

template<auto Field>
auto requireFieldGreater(TypeOfField<Field> const value) {
    return requireField<Field, FieldComparisonMode::Greater>(value);
}

template<auto Field>
auto requireFieldGreaterEq(TypeOfField<Field> const value) {
    return requireField<Field, FieldComparisonMode::GreaterEq>(value);
}

} // namespace core
