#pragma once

#include <memory>

namespace client {

enum class Key {
    None,
    W,
    A,
    S,
    D,
    Escape,
    
    Count,
};

class Window {
public:
    virtual ~Window() = default;

    virtual bool shouldClose() const = 0;
    virtual void pollEvents() = 0;
    virtual void clear(char const ch = ' ') = 0;
    virtual void display() = 0;

    virtual bool isKeyPressed(Key const key) = 0;

    virtual void draw(uint8_t const x, uint8_t const y, char const ch) = 0;

    static std::unique_ptr<Window> makeConsoleWindow(uint8_t const width, uint8_t const height);
};

} // namespace client
