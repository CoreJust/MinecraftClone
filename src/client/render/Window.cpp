#include <client/render/Window.hpp>

#include <core/Assert.hpp>
#include <core/IO/Log.hpp>
#include <core/macro/OS.hpp>

#ifdef WINDOWS
#  include <core/OS/Windows/Lean.hpp>
#  include <conio.h>
#else
#  include <termios.h>
#  include <unistd.h>
#  include <sys/select.h>
#endif

#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

namespace client {

namespace {

class ConsoleWindow final : public Window {
public:
    explicit ConsoleWindow(uint8_t const width, uint8_t const height)
        : m_width(width)
        , m_height(height)
        , m_grid(height, std::string(width, ' '))
        , m_shouldClose(false)
    {
        std::memset(m_keyStates, 0, sizeof(m_keyStates));
#ifdef WINDOWS
        HANDLE const hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD dwMode = 0;
        GetConsoleMode(hOut, &dwMode);
        dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hOut, dwMode);
#else
        tcgetattr(STDIN_FILENO, &m_originalTermios);
        struct termios raw = m_originalTermios;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        std::cout << "\033[?25l" << std::flush;
#endif
        MC_INFO("ConsoleWindow created {}x{}", width, height);
    }

    ~ConsoleWindow() override {
#ifndef WINDOWS
        tcsetattr(STDIN_FILENO, TCSANOW, &m_originalTermios);
        std::cout << "\033[?25h" << std::flush;
#endif
        MC_INFO("ConsoleWindow destroyed");
    }

    bool shouldClose() const override {
        return m_shouldClose;
    }

    void pollEvents() override {
        std::memset(m_keyStates, 0, sizeof(m_keyStates));
#ifdef _WIN32
        while (_kbhit()) {
            int ch = _getch();
            processKey(ch);
        }
#else
        int ch;
        while (true) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            struct timeval tv = {0, 0};
            if (select(STDIN_FILENO+1, &fds, NULL, NULL, &tv) <= 0) {
                break;
            }
            ch = getchar();
            if (ch == EOF) {
                break;
            }
            processKey(ch);
        }
#endif
    }
    
    void clear(char const ch) override {
        for (auto& row : m_grid) {
            std::fill(row.begin(), row.end(), ch);
        }
    }
    
    void display() override {
        std::cout << "\033[H";
        for (auto it = m_grid.rbegin(); it != m_grid.rend(); ++it) {
            std::cout.write(it->data(), it->size());
            if (it != m_grid.rend() - 1) {
                std::cout << '\n';
            }
        }
        std::cout << "\033[H" << std::flush;
    }

    bool isKeyPressed(Key const key) override {
        uint8_t idx = static_cast<uint8_t>(key);
        if (idx >= static_cast<uint8_t>(Key::Count)) {
            return false;
        }
        return m_keyStates[idx];
    }

    void draw(uint8_t const x, uint8_t const y, char const ch) override {
        ASSERT(x < m_width && y < m_height);
        m_grid[y][x] = ch;
    }
private:
    void processKey(int const ch) {
        switch (ch) {
            case 'w': case 'W': m_keyStates[static_cast<uint8_t>(Key::W)] = true; break;
            case 'a': case 'A': m_keyStates[static_cast<uint8_t>(Key::A)] = true; break;
            case 's': case 'S': m_keyStates[static_cast<uint8_t>(Key::S)] = true; break;
            case 'd': case 'D': m_keyStates[static_cast<uint8_t>(Key::D)] = true; break;
            case 27:
                m_keyStates[static_cast<uint8_t>(Key::Escape)] = true;
                m_shouldClose = true;
                break;
            default: break;
        }
    }
private:
    uint8_t m_width;
    uint8_t m_height;
    std::vector<std::string> m_grid;
    bool m_shouldClose;
    bool m_keyStates[static_cast<uint8_t>(Key::Count)];

#ifndef WINDOWS
    struct termios m_originalTermios;
#endif
};

} // namespace

std::unique_ptr<Window> Window::makeConsoleWindow(uint8_t const width, uint8_t const height) {
    return std::make_unique<ConsoleWindow>(width, height);
}

} // namespace client
