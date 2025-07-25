#pragma once
#include <Core/Meta/IsSame.hpp>
#include <Core/Common/NonCopyable.hpp>
#include <Core/Memory/Exchange.hpp>
#include <Core/Memory/Move.hpp>
#include <Core/Memory/Function.hpp>
#include "ArrayView.hpp"

namespace core {
    template<typename T>
    class DynArray final : public ArrayView<T>, NonCopyable {
        using Parent = ArrayView<T>;

    public:
        using Raw = Parent::Raw;
        using Parent::raw;

    private:
        constexpr DynArray(Raw mem) noexcept : Parent(move(mem)) { }
    public:
        constexpr DynArray() noexcept = default;
        constexpr DynArray(DynArray&& other) noexcept : Parent(exchange(other.raw(), Raw { })) { }
        constexpr DynArray& operator=(DynArray&& other) noexcept {
            Parent::operator=(Parent { exchange(other.raw(), Raw { }) });
            return *this;
        }
        DynArray(usize size, T const& defaultValue = T { })
            : Parent(Raw::alloc(size)) {
            for (auto& item : *this)
                ::new(&item) T { defaultValue };
        }
        DynArray(usize size, Function<T, usize> const& func)
            : Parent(Raw::alloc(size)) {
            usize i = 0;
            for (auto& item : *this)
                ::new(&item) T { func(i++) };
        }
        DynArray(usize size, Function<void, T*, usize> const& func)
            : Parent(Raw::alloc(size)) {
            usize i = 0;
            for (auto& item : *this)
                func(&item, i++);
        }
        explicit DynArray(Parent view)
            : Parent(Raw::alloc(view.size())) {
            raw().copyFrom(view.raw());
        }
        ~DynArray() {
            for (auto& item : *this)
                item.~T();
            raw().free();
        }

        PURE static DynArray uninitialized(usize size) {
            return DynArray(Raw::alloc(size));
        }

        // Note: requires T to be trivially movable
        void resize(usize newSize) {
            raw().resize(newSize);
        }
    };
} // namespace core
