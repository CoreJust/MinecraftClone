#pragma once
#include <Core/Common/Assert.hpp>
#include <Core/Memory/RawArray.hpp>

namespace core {
    template<typename T>
    class ArrayView {
    public:
        using Raw = RawArray<sizeof(T)>;

    private:
        Raw m_data;

    public:
        constexpr ArrayView() noexcept = default;
        constexpr ArrayView(T* ptr, usize size) noexcept : m_data({ reinterpret_cast<byte*>(ptr), size * sizeof(T) }) { }
        template<usize N>
        constexpr ArrayView(T(&arr)[N]) noexcept : m_data({ reinterpret_cast<byte*>(arr), N * sizeof(T) }) { }
        explicit constexpr ArrayView(Raw rawData) noexcept : m_data(rawData) { }

        PURE constexpr operator ArrayView<T const>() { return ArrayView<T const>(data(), size()); }

        PURE constexpr T      & operator[](usize idx)       { return *reinterpret_cast<T      *>(m_data.ptrAt(idx)); }
        PURE constexpr T const& operator[](usize idx) const { return *reinterpret_cast<T const*>(m_data.ptrAt(idx)); }

        PURE constexpr ArrayView subview(usize from = 0ull, usize count = static_cast<usize>(-1) / sizeof(T)) const noexcept {
            return ArrayView(m_data.chunk(from, count));
        }

        PURE constexpr T      * begin()        noexcept { return data(); }
        PURE constexpr T const* begin()  const noexcept { return data(); }
        PURE constexpr T const* cbegin() const noexcept { return data(); }
        PURE constexpr T      * end()          noexcept { return data() + size(); }
        PURE constexpr T const* end()    const noexcept { return data() + size(); }
        PURE constexpr T const* cend()   const noexcept { return data() + size(); }

        PURE constexpr usize    size()   const noexcept { return m_data.count(); }
        PURE constexpr T      * data()         noexcept { return reinterpret_cast<T*>(m_data.data); }
        PURE constexpr T const* data()   const noexcept { return reinterpret_cast<T const*>(m_data.data); }

        PURE constexpr Raw      & raw()        noexcept { return m_data; }
        PURE constexpr Raw const& raw()  const noexcept { return m_data; }
    };
    template<typename T>
    class ArrayView<T const> {
    public:
        using Raw = RawArray<sizeof(T)>;

    private:
        Raw const m_data;

    public:
        constexpr ArrayView() noexcept = default;
        constexpr ArrayView(T const* ptr, usize size) noexcept : m_data({ reinterpret_cast<byte*>(const_cast<T*>(ptr)), size * sizeof(T) }) { }
        template<usize N>
        constexpr ArrayView(T const(&arr)[N]) noexcept : m_data({ reinterpret_cast<byte*>(const_cast<T*>(arr)), N * sizeof(T) }) { }
        explicit constexpr ArrayView(Raw rawData) noexcept : m_data(rawData) { }

        PURE constexpr T const& operator[](usize idx) const { return *reinterpret_cast<T const*>(m_data.ptrAt(idx)); }

        PURE constexpr ArrayView subview(usize from = 0ull, usize count = static_cast<usize>(-1) / sizeof(T)) const noexcept {
            return ArrayView(m_data.chunk(from, count));
        }

        PURE constexpr T const* begin()  const noexcept { return data(); }
        PURE constexpr T const* cbegin() const noexcept { return data(); }
        PURE constexpr T const* end()    const noexcept { return data() + size(); }
        PURE constexpr T const* cend()   const noexcept { return data() + size(); }
        PURE constexpr usize    size()   const noexcept { return m_data.count(); }
        PURE constexpr T const* data()   const noexcept { return reinterpret_cast<T const*>(m_data.data); }
        PURE constexpr Raw const& raw()  const noexcept { return m_data; }
    };
} // namespace core
