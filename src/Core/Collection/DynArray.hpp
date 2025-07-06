#pragma once
#include <Core/Common/Assert.hpp>
#include <Core/Memory/Exchange.hpp>
#include <Core/Memory/RawMemory.hpp>

namespace core::collection {
    template<typename T>
    class DynArray final {
        memory::RawMemory m_data { };

    public:
        constexpr DynArray() noexcept = default;        
        DynArray(DynArray&& other) noexcept
            : m_data(memory::exchange(other.m_data, memory::RawMemory())) { }
        DynArray& operator=(DynArray&& other) noexcept {
            m_data = memory::exchange(other.m_data, memory::RawMemory());
            return *this;
        }
        DynArray(size_t size) : m_data(memory::RawMemory::alloc(size * sizeof(T))) { }
        DynArray(size_t size, T const& defaultValue)
            : m_data(memory::RawMemory::alloc(size * sizeof(T))) {
            T* ptr = data();
            T* end = ptr + size;
            while (ptr < end)
                ::new(ptr++) T(defaultValue);
        }
        ~DynArray() {
            m_data.free();
        }

        PURE T& operator[](size_t idx) {
            ASSERT(idx * sizeof(T) + (sizeof(T) - 1) < m_data.size());
            return data()[idx];
        }

        PURE T const& operator[](size_t idx) const {
            ASSERT(idx * sizeof(T) + (sizeof(T) - 1) < m_data.size());
            return data()[idx];
        }

        void resize(size_t newSize) {
            m_data.resize(newSize * sizeof(T));
        }

        PURE constexpr T* begin() noexcept { return data(); }
        PURE constexpr T const* begin() const noexcept { return data(); }
        PURE constexpr T const* cbegin() const noexcept { return data(); }
        PURE constexpr T* end() noexcept { return data() + size(); }
        PURE constexpr T const* end() const noexcept { return data() + size(); }
        PURE constexpr T const* cend() const noexcept { return data() + size(); }

        PURE constexpr size_t size() const noexcept { return m_data.size() / sizeof(T); }
        PURE constexpr T* data() noexcept { return reinterpret_cast<T*>(m_data.data()); }
        PURE constexpr T const* data() const noexcept { return reinterpret_cast<T const*>(m_data.data()); }
    };
} // namespace core::collection
