#pragma once

namespace core {
    template<typename T>
    concept Iterable = requires (T& t) {
        t.begin();
        t.end();
    };
} // namespace core
