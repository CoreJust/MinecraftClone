#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Math/Vec.hpp>
#include <Core/Memory/UniquePtr.hpp>
#include <Core/Collection/ArrayView.hpp>

namespace graphics::window {
    class Window final {
        void* m_window;
        char const* const m_name;

    public:
        Window(char const* const name, int width = 0, int height = 0);
        ~Window();

        bool nextFrame();

        PURE void* getSurfaceCreateCallback() const noexcept;
        PURE core::ArrayView<char const*> getRequiredExtensions() const;

        PURE constexpr void*        windowImpl()       noexcept { return m_window; }
        PURE constexpr char const*  name()       const noexcept { return m_name; }
        PURE core::Vec2u32    pixelSize() const noexcept;
    };
} // namespace graphics::window
