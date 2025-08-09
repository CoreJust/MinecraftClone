#pragma once
#include "IteratorAdapter.hpp"
#include "Iterable.hpp"

namespace core {
    template<typename IterBegin1, typename IterEnd1, typename IterBegin2, typename IterEnd2>
    class ZipView final {
        struct ZipIteratorEnd final {
            IterEnd1 end1;
            IterEnd2 end2;
        };
        struct ZipIterator final {
            IterBegin1 it1;
            IterBegin2 it2;

            struct Value final {
                decltype(*it1) first;
                decltype(*it2) second;
            };

            PURE constexpr bool operator==(ZipIterator const& other) const NOEXCEPT_AS(it1 == other.it1 && it2 == other.it2)
                { return it1 == other.it1 && it2 == other.it2; }
            PURE constexpr bool operator==(ZipIteratorEnd const& other) const NOEXCEPT_AS(it1 == other.end1 || it2 == other.end2)
                { return it1 == other.end1 || it2 == other.end2; }
            
            constexpr void next() NOEXCEPT_AS(++it1, ++it2) {
                ++it1;
                ++it2;
            }

            PURE constexpr Value get() NOEXCEPT_AS(*it1, *it2) {
                return { *it1, *it2 };
            }

            PURE constexpr Value get() const NOEXCEPT_AS(*it1, *it2) {
                return { *it1, *it2 };
            }
        };

        using Iterator      = MakeIterator<ZipIterator>;
        using ConstIterator = MakeConstIterator<ZipIterator>;

    public:
        IterBegin1 it1;
        IterBegin2 it2;
        IterEnd1   end1;
        IterEnd2   end2;

        PURE Iterator      begin ()       noexcept { return { it1,  it2 }; }
        PURE ConstIterator begin () const noexcept { return { it1,  it2 }; }
        PURE ConstIterator cbegin() const noexcept { return { it1,  it2 }; }
        PURE Iterator      end   ()       noexcept { return { end1, end2 }; }
        PURE ConstIterator end   () const noexcept { return { end1, end2 }; }
        PURE ConstIterator cend  () const noexcept { return { end1, end2 }; }
    };

    template<typename IterBegin, typename IterEnd>
    struct ZipViewTag final { IterBegin it; IterEnd end; };

    constexpr auto zip(Iterable auto&& view) NOEXCEPT_AS(view.begin(), view.end()) {
        return ZipViewTag { view.begin(), view.end() };
    }

    template<typename IterBegin, typename IterEnd>
    constexpr auto operator|(Iterable auto&& view, ZipViewTag<IterBegin, IterEnd> tag) NOEXCEPT_AS(view.begin(), view.end()) {
        return ZipView {
            view.begin(),
            tag.it,
            view.end(),
            tag.end,
        };
    }
} // namespace core
