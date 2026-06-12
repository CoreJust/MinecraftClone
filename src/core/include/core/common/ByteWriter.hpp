#pragma once

#include <cstdint>
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

    template<typename Self, typename T>
        requires std::is_pod_v<std::remove_cvref_t<T>>
    auto&& write(this Self&& self, T&& value) {
        using Value = std::remove_cvref_t<T>;
        size_t const old_size = self.m_data.size();
        self.m_data.resize(self.m_data.size() + sizeof(Value));
        ::new(self.m_data.data() + old_size) Value(std::forward<T>(value));
        return std::forward<Self>(self);
    }

    template<typename Self, typename T>
        requires std::is_pod_v<T>
    auto&& write(this Self&& self, std::span<T const> const value) {
        size_t const old_size = self.m_data.size();
        size_t const value_size = value.size_bytes();
        self.m_data.resize(old_size + value_size + sizeof(value_size));

        uint8_t* dst = self.m_data.data() + old_size;
        memcpy(dst, &value_size, sizeof(value_size));
        memcpy(dst + sizeof(value_size), value.data(), value_size);

        return std::forward<Self>(self);
    }

    template<typename Self>
    auto&& write(this Self&& self, std::string_view const value) {
        return self.write(std::span{ value.data(), value,size() });
    }

    [[nodiscard]]
    std::vector<uint8_t> build() noexcept { return std::move(m_data); }
    [[nodiscard]]
    constexpr size_t size() const noexcept { return m_data.size(); }
private:
    std::vector<uint8_t> m_data;
};

} // namespace core
