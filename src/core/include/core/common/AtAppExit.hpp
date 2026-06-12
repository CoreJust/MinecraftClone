#pragma once

#include <vector>

namespace core {

// Must be used to release resources used by the whole app.
class AtAppExit final {
    using Callback = void(*)();
public:
    AtAppExit(Callback callback) {
        s_callbacks.push_back(callback);
    }

    static void exit() {
        for (auto it = s_callbacks.rbegin(); it != s_callbacks.rend(); ++it) {
            (*it)();
        }
    }
private:
    inline static std::vector<Callback> s_callbacks{ };
};

} // namespace core
