#pragma once
#include <Core/Meta/IsSame.hpp>
#include <Core/Common/Assert.hpp>
#include "TypeErasedAccessor.hpp"
#include "Forward.hpp"
#include "Exchange.hpp"

namespace core {
    template<typename Return, typename... Args>
    class Function final {
		using Deleter = void(*)(void* ptr);
        using Func = Return(*)(Args...);
        using ContextFunc = Return(*)(void*&, Args...);

        void* m_function = nullptr;
        mutable void* m_context = nullptr;
        Deleter m_deleter = nullptr;

    public:
        constexpr Function() noexcept = default;
        constexpr Function(decltype(nullptr)) noexcept : Function() { }
        constexpr Function(Func func)
            : m_function(
                reinterpret_cast<void*>(static_cast<ContextFunc>(
                    [](void*& func, Args... args) -> Return { 
                        return reinterpret_cast<Func>(func)(FORWARD(args)...); 
                    })))
            , m_context(reinterpret_cast<void*>(func))
        { }
        template<typename Functor>
        Function(Functor&& func)
            requires requires (Args... args) { { func(FORWARD(args)...) } -> IsSame<Return>; }
            : m_function(
                reinterpret_cast<void*>(static_cast<ContextFunc>(
                    [](void*& self, Args... args) -> Return {
                        return TypeErasedAccessor<Functor>::get(self).operator()(FORWARD(args)...);
                    })))
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
            }
            m_context = nullptr;
        }

        Return operator()(Args... args) const {
            ASSERT(m_function != nullptr);
            return reinterpret_cast<ContextFunc>(m_function)(m_context, FORWARD(args)...);
        }

        constexpr explicit operator bool() const noexcept {
            return m_function != nullptr;
        }
    };
} // namespace core
