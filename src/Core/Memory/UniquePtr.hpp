#pragma once
#include "Forward.hpp"
#include <Core/Common/Assert.hpp>

namespace core::memory {
    template<typename T>
    class UniquePtr final {
        T* m_data = nullptr;
        void(*m_deleter)(T*) = nullptr;

    public:
        constexpr UniquePtr() noexcept = default;
        constexpr UniquePtr(decltype(nullptr)) noexcept : m_data(nullptr) { }
        constexpr UniquePtr(T* ptr, void(*deleter)(T*)) noexcept : m_data(ptr), m_deleter(deleter) { }
        constexpr UniquePtr(UniquePtr&& other) noexcept : m_data(other.m_data), m_deleter(other.m_deleter) { other.m_data = nullptr; }
        constexpr UniquePtr(UniquePtr const&) noexcept = delete;
        constexpr UniquePtr& operator=(UniquePtr&& other) noexcept {
            T* tmp = m_data;
            m_data = other.m_data;
            other.m_data = tmp;
            m_deleter = other.m_deleter;
            return *this;
        }

        constexpr UniquePtr& operator=(UniquePtr const&) noexcept = delete;
        ~UniquePtr() {
            if (m_data != nullptr && m_deleter != nullptr) {
                m_deleter(m_data);
                m_deleter = nullptr;
                m_data = nullptr;
            }
        }

        constexpr T      & operator* ()       { ASSERT(m_data != nullptr, "Dereferencing a null pointer!"); return *m_data; }
        constexpr T const& operator* () const { ASSERT(m_data != nullptr, "Dereferencing a null pointer!"); return *m_data; }
        constexpr T      * operator->()       { ASSERT(m_data != nullptr, "Dereferencing a null pointer!"); return  m_data; }
        constexpr T const* operator->() const { ASSERT(m_data != nullptr, "Dereferencing a null pointer!"); return  m_data; }
        constexpr T* get() noexcept { return m_data; }
        constexpr T const* get() const noexcept { return m_data; }
    };

    template<typename T>
    UniquePtr<T> makeUP(auto&&... args) {
        return UniquePtr<T>(new T(FORWARD(args)...), static_cast<void(*)(T*)>([](T* x) { delete x; }));
    }
} // namespace core::memory

