#pragma once

#include <cstdlib>
#include <vector>

namespace core {

// Must be used to release resources used by the whole app.
class AtAppExit final {
    using Callback = void(*)();
public:
    explicit AtAppExit(Callback callback) {
        if (!s_atexit_registered) {
            std::atexit(&AtAppExit::exit);
            s_atexit_registered = true;
        }
        s_callbacks.push_back(callback);
    }

    static void exit() {
        if (s_exited) {
            return;
        }
        s_exited = true;
        for (auto it = s_callbacks.rbegin(); it != s_callbacks.rend(); ++it) {
            (*it)();
        }
    }
private:
    inline static std::vector<Callback> s_callbacks{ };
    inline static bool s_atexit_registered = false;
    inline static bool s_exited = false;
};

} // namespace core
