#pragma once
#include "ArrayView.hpp"

namespace core {
    class StringView : public ArrayView<char const> {
        using Parent = ArrayView<char const>;

    public:
        constexpr static inline usize NoPos = static_cast<usize>(-1);

    public:
        constexpr StringView() noexcept = default;
        constexpr StringView(StringView&&) noexcept = default;
        constexpr StringView(StringView const&) noexcept = default;
        constexpr StringView(char const* ptr, usize size) noexcept : Parent(ptr, size) { }
        constexpr StringView(char const* ptr) noexcept 
            : Parent(
                ptr,
                ([ptr] {
                    char const* p = ptr;
                    while (*p)
                        ++p;
                    return static_cast<usize>(p - ptr);
                })()) 
        { }
        explicit constexpr StringView(Raw rawData) noexcept : Parent(rawData) { }
        explicit constexpr StringView(Parent view) noexcept : Parent(view) { }
        constexpr StringView& operator=(StringView&&) noexcept = default;
        constexpr StringView& operator=(StringView const&) noexcept = default;

        PURE constexpr bool operator==(StringView rhs) const noexcept {
            if (size() != rhs.size())
                return false;
            char const* it = begin();
            for (char const b : rhs) {
                if (*(it++) != b)
                    return false;
            }
            return true;
        }

        PURE constexpr char operator[](usize idx) const { return Parent::operator[](idx); }

        PURE constexpr StringView substr(usize from = 0ull, usize count = NoPos) const noexcept {
            return StringView(Parent::subview(from, count));
        }

        PURE constexpr int compare(StringView rhs) const noexcept {
            if (rhs.empty())
                return empty();
            if (size() < rhs.size())
                return -rhs.compare(*this);
            char const* it = begin();
            for (char const b : rhs) {
                char const a = *(it++);
                if (a < b)
                    return -1;
                if (a > b)
                    return 1;
            }
            return size() > rhs.size();
        }

        PURE constexpr usize find(char const ch) const noexcept {
            char const* endIt = end();
            for (char const* it = begin(); it < endIt; ++it) {
                if (*it == ch)
                    return static_cast<usize>(it - begin());
            }
            return NoPos;
        }

        PURE constexpr bool startsWith(StringView prefix) const noexcept {
            if (prefix.size() > size())
                return false;
            return substr(0ull, prefix.size()) == prefix;
        }

        PURE constexpr bool endsWith(StringView postfix) const noexcept {
            if (postfix.size() > size())
                return false;
            return substr(size() - postfix.size()) == postfix;
        }
    };
} // namespace core
