#pragma once
#include <Core/Meta/IsSame.hpp>
#include <Core/Common/Assert.hpp>
#include "TypeErasedAccessor.hpp"
#include "Forward.hpp"
#include "Exchange.hpp"

namespace core::memory {
    template<typename Return, typename... Args>
    class Function final {
		using Deleter = void(*)(void* ptr);

        void* m_function = nullptr;
        void* m_context = nullptr;
        Deleter m_deleter = nullptr;

    public:
        constexpr Function() noexcept = default;
        constexpr Function(decltype(nullptr)) noexcept : Function() { }
        constexpr Function(Return(*func)(Args...)) : m_function(reinterpret_cast<void*>(func)) { }
        template<typename Functor>
        Function(Functor&& func)
            requires requires (Args... args) { { func(args...) } -> meta::IsSame<Return>; }
            : m_function(
                reinterpret_cast<void*>(
                static_cast<Return(*)(Functor*, Args...)>(
                    [](Functor* self, Args... args) -> Return { return self->operator()(FORWARD(args)...); })))
            , m_context(TypeErasedAccessor<Functor>::make(FORWARD(func)))
            , m_deleter(static_cast<Deleter>([](void* self) { TypeErasedAccessor<Functor>::destroy(self); }))
        { }
        constexpr Function(Function&& other) noexcept
            : m_function(exchange(other.m_function, nullptr))
            , m_context (exchange(other.m_context,  nullptr))
            , m_deleter (exchange(other.m_deleter,  nullptr))
        { }
        constexpr Function& operator=(Function&& other) noexcept {
            m_function = exchange(other.m_function, nullptr);
            m_context  = exchange(other.m_context,  nullptr);
            m_deleter  = exchange(other.m_deleter,  nullptr);
            return *this;
        }
        ~Function() {
            m_function = nullptr;
            if (m_deleter) {
                m_deleter(m_context);
                m_deleter = nullptr;
                m_context = nullptr;
            }
        }

        Return operator()(Args... args) {
            ASSERT(m_function != nullptr);
            if (m_context != nullptr) {
                return reinterpret_cast<Return(*)(void*, Args...)>(m_function)(m_context, FORWARD(args)...);
            } else {
                return reinterpret_cast<Return(*)(Args...)>(m_function)(FORWARD(args)...);
            }
        }

        constexpr explicit operator bool() const noexcept {
            return m_function != nullptr;
        }
    };
} // namespace core::memory
