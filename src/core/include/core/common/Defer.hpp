#pragma once

#include <core/macro/NameGeneration.hpp>

#include <utility>

namespace core {

struct DeferNote { };

template<typename Func>
struct DeferImpl final {
    Func f;

    ~DeferImpl() {
        f();
    }
};

template<typename Func>
DeferImpl<Func> operator^(DeferNote, Func&& f) {
    return DeferImpl<Func>{ std::forward<Func>(f) };
}

} // namespace core

#define GEN_DEFER_NAME(LINE) GEN_NAME_(DeferVariable_, LINE)
#define defer auto GEN_DEFER_NAME(__LINE__) = ::core::DeferNote{} ^ [&]()
