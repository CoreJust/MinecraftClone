#pragma once

#include <core/meta/Enum.hpp>

#include <cstdint>

namespace core {

enum class SurfaceTransform {
    Identity,
    Rotate90,
    Rotate180,
    Rotate270,
    HorizontalMirror,
    HorizontalMirrorRotate90,
    HorizontalMirrorRotate180,
    HorizontalMirrorRotate270,
    Inherit,

    Count,
};

CORE_ENUM_FUNCTIONS(SurfaceTransform);

struct SurfaceTransformBits final {
    uint32_t value;

    template<typename... Args> [[nodiscard]]
    static constexpr SurfaceTransformBits of(SurfaceTransform const first, Args const... args) noexcept {
        if constexpr (sizeof...(Args) > 0) {
            return { 1u << indexOf(first) | of(args...).value };
        } else {
            return { 1u << indexOf(first) };
        }
    }
};

} // namespace core
