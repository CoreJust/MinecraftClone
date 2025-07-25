#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Math/Vec.hpp>
#include <Core/Container/UniquePtr.hpp>
#include <Core/Memory/Function.hpp>
#include <Core/Container/ArrayView.hpp>

typedef struct GLFWwindow GLFWwindow;

namespace graphics::window {
    class Window final {
        GLFWwindow* m_window;
        char const* const m_name;
        core::Function<void, core::Vec2<int>> m_resizeCallback;
        bool m_ignoreMinimized = false;

    public:
        Window(char const* const name, core::Vec2<int> size = {{ 0, 0 }});
        ~Window();

        bool nextFrame();

        void onResize(bool ignoreMinimized, core::Function<void, core::Vec2<int>> resizeCallback) {
            m_ignoreMinimized = ignoreMinimized;
            m_resizeCallback = core::move(resizeCallback);
        }

        PURE void* getSurfaceCreateCallback() const noexcept;
        PURE core::ArrayView<char const*> getRequiredExtensions() const;

        PURE constexpr GLFWwindow*  windowImpl()      noexcept { return m_window; }
        PURE constexpr char const*  name()      const noexcept { return m_name; }
        PURE core::Vec2u32          pixelSize() const noexcept;

    private:
        static void framebuffersResized(GLFWwindow* window, int const width, int const height);
    };
} // namespace graphics::window
