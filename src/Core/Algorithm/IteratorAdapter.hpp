#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Memory/Forward.hpp>
#include "IteratorConcepts.hpp"

// ConstIterator and Iterator make iterator creation fast and simple - you only need to define single traits and that's it.

namespace core {
    struct IteratorEndSentinel final { };

    template<ForwardIteratorImplConcept IteratorImpl, bool Const>
    class MakeIteratorImpl {
        template<typename, bool> struct OptConstImpl { };
        template<typename T> struct OptConstImpl<T, false> { using Type = T const; };
        template<typename T> struct OptConstImpl<T, true>  { using Type = T; };
        template<typename T> using OptConst = OptConstImpl<T, Const>::Type;

        template<typename> struct DiffTypeImpl { using Type = isize; };
        template<RandomAccessIteratorImplConcept T> struct DiffTypeImpl<T> { using Type = typename T::Diff; };
        template<typename T> using DiffType = DiffTypeImpl<T>::Type;

    private:
        IteratorImpl m_impl;

    public:
        using value_type        = IteratorImpl::Value;
        using difference_type   = DiffType<IteratorImpl>;
        using reference         = OptConst<decltype(m_impl.get())>;

        constexpr MakeIteratorImpl(auto&&... args) NOEXCEPT_AS(IteratorImpl { FORWARD(args)... }) 
            requires requires { IteratorImpl { FORWARD(args)... }; }
            : m_impl { FORWARD(args)... } { }
        constexpr MakeIteratorImpl& operator=(MakeIteratorImpl&& other) &NOEXCEPT_AS(m_impl = other.m_impl) = default;
        constexpr MakeIteratorImpl& operator=(MakeIteratorImpl const& other) &NOEXCEPT_AS(m_impl = other.m_impl) = default;

        constexpr MakeIteratorImpl& operator+=(difference_type diff) NOEXCEPT_AS(m_impl.advance(diff))
            requires RandomAccessIteratorImplConcept<IteratorImpl> {
            m_impl.advance(diff);
            return *this;
        }

        constexpr MakeIteratorImpl& operator-=(difference_type diff) NOEXCEPT_AS(m_impl.advance(diff))
            requires RandomAccessIteratorImplConcept<IteratorImpl> {
            m_impl.advance(-diff);
            return *this;
        }

        PURE constexpr bool operator==(MakeIteratorImpl const& other) const NOEXCEPT_AS(m_impl == other.m_impl) { return m_impl == other.m_impl; }
        PURE constexpr bool operator< (MakeIteratorImpl const& other) const NOEXCEPT_AS(m_impl < other.m_impl) 
            requires RandomAccessIteratorImplConcept<IteratorImpl> { return m_impl < other.m_impl; }
        PURE constexpr bool operator> (MakeIteratorImpl const& other) const NOEXCEPT_AS(m_impl < other.m_impl) 
            requires RandomAccessIteratorImplConcept<IteratorImpl> { return other.m_impl < m_impl; }
        PURE constexpr bool operator<=(MakeIteratorImpl const& other) const NOEXCEPT_AS(m_impl < other.m_impl) 
            requires RandomAccessIteratorImplConcept<IteratorImpl> { return !(other.m_impl < m_impl); }
        PURE constexpr bool operator>=(MakeIteratorImpl const& other) const NOEXCEPT_AS(m_impl < other.m_impl) 
            requires RandomAccessIteratorImplConcept<IteratorImpl> { return !(m_impl < other.m_impl); }
        PURE constexpr bool operator==(IteratorEndSentinel) const NOEXCEPT_AS(m_impl.isEnd())
            requires requires { m_impl.isEnd(); } { return m_impl.isEnd(); }
        PURE constexpr bool operator< (IteratorEndSentinel) const NOEXCEPT_AS(m_impl.isEnd())
            requires requires { m_impl.isEnd(); } { return !m_impl.isEnd(); }

        PURE constexpr reference operator*()       NOEXCEPT_AS(m_impl.get()) { return m_impl.get(); }
        PURE constexpr reference operator*() const NOEXCEPT_AS(m_impl.get()) { return m_impl.get(); }
        
		constexpr MakeIteratorImpl& operator++() NOEXCEPT_AS(m_impl.next()) {
			m_impl.next();
			return *this;
		}

		constexpr MakeIteratorImpl operator++(int) NOEXCEPT_AS(m_impl.next()) {
			MakeIteratorImpl result = *this;
			m_impl.next();
			return result;
		}
        
		constexpr MakeIteratorImpl& operator--() NOEXCEPT_AS(m_impl.prev()) 
            requires BidirectionalIteratorImplConcept<IteratorImpl> {
			m_impl.prev();
			return *this;
		}

		constexpr MakeIteratorImpl operator--(int) NOEXCEPT_AS(m_impl.prev())
            requires BidirectionalIteratorImplConcept<IteratorImpl> {
			MakeIteratorImpl result = *this;
			m_impl.prev();
			return result;
		}

        PURE constexpr MakeIteratorImpl operator+(difference_type diff) const
            requires RandomAccessIteratorImplConcept<IteratorImpl> {
            MakeIteratorImpl tmp = *this;
            tmp += diff;
            return tmp;
        }

        PURE constexpr MakeIteratorImpl operator-(difference_type diff) const 
            requires RandomAccessIteratorImplConcept<IteratorImpl> {
            MakeIteratorImpl tmp = *this;
            tmp -= diff;
            return tmp;
        }

        PURE constexpr reference operator[](difference_type idx) const NOEXCEPT_AS(*(*this + idx)) 
            requires RandomAccessIteratorImplConcept<IteratorImpl> {
            return *(*this + idx);
        }
    };

    template<ForwardIteratorImplConcept IteratorImpl>
    using MakeConstIterator = MakeIteratorImpl<IteratorImpl, true>;
    template<ForwardIteratorImplConcept IteratorImpl>
    using MakeIterator      = MakeIteratorImpl<IteratorImpl, false>;
} // namespace core
