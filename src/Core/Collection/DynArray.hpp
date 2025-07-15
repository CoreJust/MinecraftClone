#pragma once
#include <Core/Meta/IsSame.hpp>
#include <Core/Memory/Exchange.hpp>
#include "ArrayView.hpp"

namespace core {
    template<typename T>
    class DynArray final {
        ArrayView<T> m_data { };

    public:
        constexpr DynArray() noexcept = default;        
        constexpr DynArray(DynArray&& other) noexcept
            : m_data(exchange(other.m_data, ArrayView<T> { })) { }
        constexpr DynArray& operator=(DynArray&& other) noexcept {
            m_data = exchange(other.m_data, ArrayView<T> { });
            return *this;
        }
        DynArray(usize size, T const& defaultValue = T { })
            : m_data(RawMemory::alloc(size * sizeof(T))) {
            for (auto& item : m_data)
                ::new(&item) T { defaultValue };
        }
        DynArray(usize size, auto&& func)
            requires requires(usize i) { { func(i) } -> IsSame<T>; }
            : m_data(RawMemory::alloc(size * sizeof(T))) {
            usize i = 0;
            for (auto& item : m_data)
                ::new(&item) T { func(i++) };
        }
        explicit DynArray(ArrayView<T> view)
            : m_data(RawMemory::alloc(view.size())) {
            m_data.rawMemory().copyFrom(view.rawMemory());
        }
        ~DynArray() {
            for (auto& item : m_data)
                item.~T();
            m_data.rawMemory().free();
        }

        PURE constexpr operator ArrayView<T>() const noexcept { return m_data; }

        PURE constexpr auto&& operator[](this auto&& self, usize idx) { return self.m_data[idx]; }

        void resize(usize newSize) {
            m_data.rawMemory().resize(newSize * sizeof(T));
        }

        PURE constexpr auto begin(this auto&& self) noexcept { return self.m_data.begin(); }
        PURE constexpr T const* cbegin() const noexcept { return m_data.cbegin(); }
        PURE constexpr auto end(this auto&& self) noexcept { return self.m_data.end(); }
        PURE constexpr T const* cend() const noexcept { return m_data.cend(); }

        PURE constexpr usize size() const noexcept { return m_data.size(); }
        PURE constexpr auto data(this auto&& self) noexcept { return self.m_data.data(); }

        PURE constexpr auto&& arrayView(this auto&& self) noexcept { return self.m_data; }
    };
} // namespace core
