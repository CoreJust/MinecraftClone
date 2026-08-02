#pragma once

#include <functional>

namespace core {

template<typename T>
concept Hashable = requires(T const& t) { std::hash<std::remove_cvref_t<T>>{}(t); };

class HashCombiner final {
public:
    constexpr HashCombiner(size_t const initial_hash = 0xcbf2'9ce4'8422'2325ull) noexcept
        : m_hash(initial_hash)
    { }

    constexpr void consume(size_t const value) noexcept {
        m_hash ^= value + 0x9e37'79b9 + (m_hash << 6) + (m_hash >> 2);
    }

    constexpr void consume(Hashable auto const&... args) requires (sizeof...(args) > 0) {
        (consume(static_cast<size_t>(std::hash<std::remove_cvref_t<decltype(args)>>{}( args ))), ...);
    }

    [[nodiscard]]
    constexpr size_t hash() const noexcept { return m_hash; }
private:
    size_t m_hash = 0xcbf2'9ce4'8422'2325ull;
};

} // namespace core
