#pragma once

#include <core/common/ByteSerializable.hpp>

#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace core {

class ByteWriter final {
public:
    explicit ByteWriter(size_t const initial_capacity = 64) {
        m_data.reserve(initial_capacity);
    }

    template<typename Self>
    auto&& reserve(this Self&& self, size_t const capacity) {
        self.m_data.reserve(capacity);
        return std::forward<Self>(self);
    }

    template<typename Self, ByteSerializable T>
    auto&& write(this Self&& self, T&& value) {
        using Value = std::remove_cvref_t<T>;
        size_t const old_size = self.m_data.size();
        self.m_data.resize(self.m_data.size() + sizeof(Value));
        Value const stored = std::forward<T>(value);
        std::memcpy(self.m_data.data() + old_size, &stored, sizeof(Value));
        return std::forward<Self>(self);
    }

    template<typename Self, ByteSerializable T, size_t Extent>
    auto&& write(this Self&& self, std::span<T const, Extent> const value) {
        uint64_t const element_count = static_cast<uint64_t>(value.size());
        size_t const value_bytes = value.size_bytes();
        size_t const old_size = self.m_data.size();
        self.m_data.resize(old_size + sizeof(element_count) + value_bytes);

        uint8_t* dst = self.m_data.data() + old_size;
        std::memcpy(dst, &element_count, sizeof(element_count));
        if (value_bytes > 0) {
            std::memcpy(dst + sizeof(element_count), value.data(), value_bytes);
        }
        return std::forward<Self>(self);
    }

    template<typename Self>
    auto&& write(this Self&& self, std::string_view const value) {
        return self.write(std::span{ reinterpret_cast<char const*>(value.data()), value.size() });
    }

    [[nodiscard]]
    std::vector<uint8_t> build() noexcept { return std::move(m_data); }
    [[nodiscard]]
    constexpr size_t size() const noexcept { return m_data.size(); }
private:
    std::vector<uint8_t> m_data;
};

} // namespace core
