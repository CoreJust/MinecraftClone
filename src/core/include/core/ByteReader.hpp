#pragma once

#include <core/Assert.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace core {

class ByteReader final {
public:
    explicit ByteReader(std::span<uint8_t> data) : m_data(data) { }

    template<typename T, typename Self>
        requires std::is_pod_v<T>
    [[nodiscard]]
    auto read(this Self&& self) {
        ASSERT(
            self.m_pos + sizeof(T) <= self.m_data.size(),
            "Tried to read {} bytes at position {} with packet size {}",
            sizeof(T), self.m_pos, self.m_data.size());
        T result = std::move(*reinterpret_cast<T*>(self.m_data.data() + self.m_pos));
        self.m_pos += sizeof(T);
        return result;
    }

    // Note: It allocates no additional memory, make sure that the created span
    // does not outlive the PacketReader.
    template<typename T>
    [[nodiscard]]
    std::span<T> readSpan() {
        size_t const size = read<size_t>();
        std::span<T> result { reinterpret_cast<T*>(m_data.data() + m_pos), size };
        ASSERT(
            m_pos + result.size_bytes() <= m_data.size(),
            "Tried to read {} bytes at position {} with packet size {}",
            result.size_bytes(), m_pos, m_data.size());
        m_pos += result.size_bytes();
        return result;
    }

    // Note: It allocates no additional memory, make sure that the created span
    // does not outlive the PacketReader.
    [[nodiscard]]
    std::string_view readStr() {
        std::span as_span = readSpan<char>();
        return std::string_view{ as_span.data(), as_span.size() };
    }

    [[nodiscard]]
    constexpr size_t size() const noexcept { return m_data.size(); }
    [[nodiscard]]
    constexpr size_t pos() const noexcept { return m_pos; }
    [[nodiscard]]
    constexpr size_t left() const noexcept { return m_data.size() - m_pos; }
private:
    std::span<uint8_t> m_data;
    size_t m_pos = 0;
};

} // namespace core
