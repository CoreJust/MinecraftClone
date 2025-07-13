#pragma once
#include <iterator>
#include <Core/Macro/Attributes.hpp>
#include <Core/Meta/UnRef.hpp>
#include <Core/Meta/UnConst.hpp>
#include <Core/Meta/IsSame.hpp>
#include <Core/Memory/Forward.hpp>

// ConstIterator and Iterator make iterator creation fast and simple - you only need to define single traits and that's it.

namespace core::algorithm {
    template<typename T>
    concept ForwardIteratorImplConcept = requires (T it) {
        typename T::Value;
        it.next();                          // Advancing forth
        it = it;                            // Copying
        { it == it } -> meta::IsSame<bool>; // Comparing for equality
        meta::IsSame<T::Value, meta::UnRef<decltype(it.get())>>; // Getting the value
        meta::IsSame<T::Value, meta::UnConst<meta::UnRef<decltype(*it.ptr())>>>; // Getting the value as a pointer (might be a temporary one)
    };

    template<typename T>
    concept BidirectionalIteratorImplConcept = ForwardIteratorImplConcept<T> && requires (T it) {
        it.prev();
    };

    template<typename T>
    concept RandomAccessIteratorImplConcept = BidirectionalIteratorImplConcept<T> && requires (T it) {
        it.advance(10);
        { it < it } -> meta::SameAs<bool>;
    };

    template<ForwardIteratorImplConcept IteratorImpl, bool Const>
    class MakeIteratorImpl {
        template<typename, bool>
        struct OptConstImpl { };
        
        template<typename T>
        struct OptConstImpl<T, false> { using Type = T const; };

        template<typename T>
        struct OptConstImpl<T, true> { using Type = T; };

        template<typename T>
        using OptConst = OptConstImpl<T, Const>::Type;

        template<ForwardIteratorImplConcept T>
        struct IteratorCategoryImpl { using Category = std::forward_iterator_tag; };
        template<BidirectionalIteratorImplConcept T>
        struct IteratorCategoryImpl { using Category = std::bidirectional_iterator_tag; };
        template<RandomAccessIteratorImplConcept T>
        struct IteratorCategoryImpl { using Category = std::random_access_iterator_tag; };

    private:
        IteratorImpl m_impl;

    public:
        using iterator_category = IteratorCategoryImpl<IteratorImpl>::Category;
        using value_type        = IteratorImpl::Value;
        using pointer           = OptConst<decltype(m_impl.ptr())>;
        using reference         = OptConst<decltype(m_impl.get())>;
        using difference_type   = decltype(m_impl.ptr() - m_impl.ptr());

        constexpr MakeIteratorImpl(auto&&... args) noexcept(noexcept(IteratorImpl(FORWARD(args)...))) : m_impl(FORWARD(args)...) { }
        constexpr MakeIteratorImpl& operator=(MakeIteratorImpl&& other) noexcept(noexcept(m_impl = other.m_impl)) = default;
        constexpr MakeIteratorImpl& operator=(MakeIteratorImpl const& other) noexcept(noexcept(m_impl = other.m_impl)) = default;

        constexpr ConstRandomAccessIterator& operator+=(difference_type diff) noexcept(IS_NOEXCEPT_ADVANCE) {
            m_impl.advance(diff);
            return *this;
        }

        constexpr ConstRandomAccessIterator& operator-=(difference_type diff) noexcept(IS_NOEXCEPT_ADVANCE) {
            m_impl.advance(-diff);
            return *this;
        }

        PURE constexpr bool operator==(MakeIteratorImpl const& other) const noexcept(m_impl < other.m_impl) { return m_impl == other.m_impl; }
        PURE constexpr bool operator< (MakeIteratorImpl const& other) const noexcept(m_impl < other.m_impl) 
            requires RandomAccessIteratorImplConcept<IteratorImpl> { return m_impl < other.m_impl; }
        PURE constexpr bool operator> (MakeIteratorImpl const& other) const noexcept(m_impl < other.m_impl) 
            requires RandomAccessIteratorImplConcept<IteratorImpl> { return other.m_impl < m_impl; }
        PURE constexpr bool operator<=(MakeIteratorImpl const& other) const noexcept(m_impl < other.m_impl) 
            requires RandomAccessIteratorImplConcept<IteratorImpl> { return !(other.m_impl < m_impl); }
        PURE constexpr bool operator>=(MakeIteratorImpl const& other) const noexcept(m_impl < other.m_impl) 
            requires RandomAccessIteratorImplConcept<IteratorImpl> { return !(m_impl < other.m_impl); }

        PURE constexpr reference operator* () const noexcept(m_impl.get()) { return m_impl.get(); }
        PURE constexpr pointer   operator->() const noexcept(m_impl.ptr()) { return m_impl.ptr(); }
        
		constexpr MakeIteratorImpl& operator++() noexcept(IS_NOEXCEPT_NEXT) {
			m_impl.next();
			return *this;
		}

		constexpr MakeIteratorImpl operator++(int) noexcept(IS_NOEXCEPT_NEXT) {
			ConstIterator result = *this;
			m_impl.next();
			return result;
		}
        
		constexpr MakeIteratorImpl& operator--() noexcept(IS_NOEXCEPT_PREV) 
            requires BidirectionalIteratorImplConcept<IteratorImpl> {
			m_impl.prev();
			return *this;
		}

		constexpr MakeIteratorImpl operator--(int) noexcept(IS_NOEXCEPT_PREV)
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

        PURE constexpr difference_type operator-(MakeIteratorImpl const& other) const 
            requires RandomAccessIteratorImplConcept<IteratorImpl> {
            return ptr() - other.ptr();
        }

        PURE constexpr reference operator[](difference_type idx) const noexcept(*(*this + idx)) 
            requires RandomAccessIteratorImplConcept<IteratorImpl> {
            return *(*this + idx);
        }
    };

    template<ForwardIteratorImplConcept IteratorImpl>
    using MakeConstIterator = MakeIteratorImpl<IteratorImpl, true>;
    template<ForwardIteratorImplConcept IteratorImpl>
    using MakeIterator = MakeIteratorImpl<IteratorImpl, false>;
} // namespace core::algorithm
