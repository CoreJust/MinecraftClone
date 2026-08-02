#pragma once

#include <core/common/ByteSerializable.hpp>

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

namespace core {

class ByteReader final {
public:
    explicit ByteReader(std::span<uint8_t const> data) : m_data(data) { }

    template<ByteSerializable T, typename Self>
    [[nodiscard]]
    std::optional<T> read(this Self&& self) noexcept {
        if (self.m_pos + sizeof(T) > self.m_data.size()) {
            return std::nullopt;
        }
        T result;
        std::memcpy(&result, self.m_data.data() + self.m_pos, sizeof(T));
        self.m_pos += sizeof(T);
        return result;
    }

    /*
     * Note: It allocates no additional memory, make sure that the created span
     * does not outlive the ByteReader. Returns std::nullopt on a truncated or
     * attacker-oversized prefix.
     */
    template<typename T>
    [[nodiscard]]
    std::optional<std::span<T const>> readSpan() noexcept {
        auto const element_count = read<uint64_t>();
        if (!element_count) {
            return std::nullopt;
        }
        if (*element_count > (m_data.size() - m_pos) / sizeof(T)) {
            return std::nullopt;
        }
        std::span<T const> result{ reinterpret_cast<T const*>(m_data.data() + m_pos), static_cast<size_t>(*element_count) };
        m_pos += result.size_bytes();
        return result;
    }

    /*
     * Note: It allocates no additional memory, make sure that the created view
     * does not outlive the ByteReader. Returns std::nullopt on a truncated or
     * attacker-oversized prefix.
     */
    [[nodiscard]]
    std::optional<std::string_view> readStr() noexcept {
        auto const as_span = readSpan<char>();
        if (!as_span) {
            return std::nullopt;
        }
        return std::string_view{ as_span->data(), as_span->size() };
    }

    [[nodiscard]]
    constexpr size_t size() const noexcept { return m_data.size(); }
    [[nodiscard]]
    constexpr size_t pos() const noexcept { return m_pos; }
    [[nodiscard]]
    constexpr size_t left() const noexcept { return m_data.size() - m_pos; }
private:
    std::span<uint8_t const> m_data;
    size_t m_pos = 0;
};

} // namespace core
