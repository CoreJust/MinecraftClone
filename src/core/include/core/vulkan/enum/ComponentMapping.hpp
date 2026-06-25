#pragma once

#include <core/meta/Enum.hpp>

namespace core {

enum class ComponentSwizzle {
    Identity,
    Zero,
    One,
    R,
    G,
    B,
    A,

    Count,
};

CORE_ENUM_FUNCTIONS(ComponentSwizzle);

struct ComponentMapping final {
    ComponentSwizzle r;
    ComponentSwizzle g;
    ComponentSwizzle b;
    ComponentSwizzle a;
};

} // namespace core
