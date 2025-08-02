#pragma once
#include <Core/Math/Color.hpp>
#include "DynArray.hpp"

namespace core {
    using Pixel = Color4u;

    class Image final {
        DynArray<Pixel> m_pixels;
        Vec2<usize> m_size;

    public:
        Image() noexcept = default;
        explicit Image(Vec2<usize> size)
            : m_pixels(DynArray<Pixel>::uninitialized(size.width() * size.height()))
            , m_size(size)
        { }
        explicit Image(Vec2<usize> size, DynArray<Pixel> data)
            : m_pixels(core::move(data))
            , m_size(size)
        { }

        PURE ArrayView<Pixel> operator[](usize rowIdx) const { return m_pixels.subview(rowIdx * m_size.width(), m_size.width()); }

        PURE DynArray<Pixel>      & data  ()       noexcept { return m_pixels; }
        PURE DynArray<Pixel> const& data  () const noexcept { return m_pixels; }
        PURE Vec2<usize>            size  () const noexcept { return m_size; }
        PURE usize                  width () const noexcept { return m_size.width(); }
        PURE usize                  height() const noexcept { return m_size.height(); }
    };
} // namespace core
