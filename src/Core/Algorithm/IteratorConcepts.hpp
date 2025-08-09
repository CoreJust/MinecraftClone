#pragma once
#include <Core/Meta/UnRef.hpp>
#include <Core/Meta/UnConst.hpp>
#include <Core/Meta/IsSame.hpp>

namespace core {
    template<typename T>
    concept ForwardIteratorImplConcept = requires (T it) {
        typename T::Value;
        it.next();                                   // Advancing forth
        it = it;                                     // Copying
        { it == it } -> IsSame<bool>;                // Comparing for equality
        IsSame<typename T::Value, UnRef<decltype(it.get())>>; // Getting the value
    };

    template<typename T>
    concept BidirectionalIteratorImplConcept = ForwardIteratorImplConcept<T> && requires (T it) {
        it.prev();
    };

    template<typename T>
    concept RandomAccessIteratorImplConcept = BidirectionalIteratorImplConcept<T> && requires (T it) {
        typename T::Diff;
        it.advance(10);
        { it < it } -> IsSame<bool>;
    };
} // namespace core
