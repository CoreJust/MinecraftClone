#pragma once

#include <core/common/NonCopyable.hpp>
#include <core/common/NonMovable.hpp>

#include <cstdint>
#include <memory>
#include <string>

struct GLFWwindow;

namespace core {

enum class Key : int {
    A = 65,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
};

class Window final : NonCopyable, NonMovable {
public:
    explicit Window(int const width, int const height, std::string const& title);
    ~Window();

    [[nodiscard]]
    GLFWwindow* nativeHandle() const noexcept { return m_window; }

    [[nodiscard]]
    bool shouldClose() const noexcept;

    void pollEvents() const noexcept;

    [[nodiscard]]
    bool isKeyPressed(Key const key) const noexcept;

    [[nodiscard]]
    std::pair<std::uint32_t, std::uint32_t> framebufferSize() const noexcept;

    [[nodiscard]]
    bool isFramebufferSizeZero() const noexcept;
private:
    GLFWwindow* m_window = nullptr;
};

} // namespace core
