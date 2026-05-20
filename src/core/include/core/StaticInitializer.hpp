#pragma once

#include <core/AtAppExit.hpp>

#include <atomic>

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
        if (!isInitialized()) {
            T::init();
            AtAppExit{ T::destroy };
        }
    }

    [[nodiscard]]
    static bool isInitialized() noexcept { return s_initialized.load(); }
private:
    inline static std::atomic_bool s_initialized{ false };
};

} // namespace core
