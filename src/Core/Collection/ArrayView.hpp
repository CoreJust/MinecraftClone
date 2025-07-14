#pragma once
#include <Core/Common/Assert.hpp>
#include <Core/Memory/RawMemory.hpp>

namespace core::collection {
    template<typename T>
    class ArrayView {
        memory::RawMemory m_data;

    public:
        constexpr ArrayView() noexcept = default;        
        constexpr ArrayView(ArrayView&&) noexcept = default;
        constexpr ArrayView(ArrayView const&) noexcept = default;
        constexpr ArrayView& operator=(ArrayView&&) noexcept = default;
        constexpr ArrayView& operator=(ArrayView const&) noexcept = default;
        constexpr ArrayView(T* ptr, usize size) noexcept : m_data(reinterpret_cast<unsigned char*>(ptr), size * sizeof(T)) { }
        explicit constexpr ArrayView(memory::RawMemory rawData) noexcept : m_data(rawData) {
            ASSERT(rawData.size() % sizeof(T) == 0);
        }

        PURE constexpr operator ArrayView<T const>() { return ArrayView<T const>(data(), size()); }

        PURE constexpr auto&& operator[](this auto&& self, usize idx) {
            ASSERT(idx * sizeof(T) + (sizeof(T) - 1) < self.m_data.size());
            return self.data()[idx];
        }

        PURE constexpr ArrayView subview(usize from = 0ull, usize count = static_cast<usize>(-1) / sizeof(T)) const noexcept {
            return ArrayView(m_data.chunk(from * sizeof(T), count * sizeof(T)));
        }

        PURE constexpr auto begin(this auto&& self) noexcept { return self.data(); }
        PURE constexpr T const* cbegin() const noexcept { return data(); }
        PURE constexpr auto end(this auto&& self) noexcept { return self.data() + self.size(); }
        PURE constexpr T const* cend() const noexcept { return data() + size(); }

        PURE constexpr usize size() const noexcept { return m_data.size() / sizeof(T); }
        PURE constexpr T* data() noexcept { return reinterpret_cast<T*>(m_data.data()); }
        PURE constexpr T const* data() const noexcept { return reinterpret_cast<T const*>(m_data.data()); }

        PURE constexpr auto&& rawMemory(this auto&& self) noexcept { return self.m_data; }
    };
} // namespace core::collection
