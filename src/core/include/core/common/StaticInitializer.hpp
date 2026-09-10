#pragma once

#include <core/common/AtAppExit.hpp>

#include <atomic>
#include <mutex>

namespace core {

template<typename T>
concept StaticallyInitializable = requires {
    T::init();
    T::destroy();
};

template<StaticallyInitializable T>
class StaticInitializer final {
public:
    static void ensureInit() {
        std::call_once(s_once, [] {
            T::init();
            AtAppExit{ T::destroy };
            s_initialized.store(true, std::memory_order_release);
        });
    }

    [[nodiscard]]
    static bool isInitialized() noexcept {
        return s_initialized.load(std::memory_order_acquire);
    }
private:
    inline static std::once_flag s_once;
    inline static std::atomic_bool s_initialized{ false };
};

} // namespace core
