#pragma once

#include <core/common/NonCopyable.hpp>
#include <core/common/NonMovable.hpp>
#include <core/common/TrivialPair.hpp>
#include <core/meta/TaggedBool.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct GLFWwindow;

namespace core {

struct WindowException{ };

using IgnoreMinimized = TaggedBool<struct IgnoreMinimizedTag>;

class Window final : NonCopyable, NonMovable {
public:
    static constexpr int FULL_SIZE = -1;
public:
    explicit Window(std::string title, int const width = FULL_SIZE, int const height = FULL_SIZE);
    ~Window();

    [[nodiscard]]
    bool nextFrame() const noexcept;
    void enableCursor(bool value);

    void onResize(IgnoreMinimized const ignore_minimized, std::function<void(int const, int const)> resize_callback) {
        m_ignore_minimized = ignore_minimized;
        m_resize_callback = std::move(resize_callback);
    }

    [[nodiscard]]
    TrivialPair<uint32_t, uint32_t> framebufferSize() const noexcept;
    [[nodiscard]]
    bool isFramebufferSizeZero() const noexcept;
    [[nodiscard]]
    GLFWwindow* nativeHandle() const noexcept { return m_window; }
    [[nodiscard]]
    std::string const& title() const noexcept { return m_title; }
private:
    static void framebuffersResized(GLFWwindow* window, int const width, int const height);
private:
    GLFWwindow* m_window = nullptr;
    std::string m_title;
    std::function<void(int const, int const)> m_resize_callback;
    IgnoreMinimized m_ignore_minimized = IgnoreMinimized::No;
    bool m_cursor_enabled = true;
};

} // namespace core
