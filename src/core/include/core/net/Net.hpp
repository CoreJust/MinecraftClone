#pragma once

#include <atomic>

namespace core {

class Net final {
public:
    static void init();
    static void destroy();
    
    [[nodiscard]]
    static bool isInitialized() noexcept { return s_initialized.load(); }

private:
    inline static std::atomic_bool s_initialized{ true };
};

} // namespace core
