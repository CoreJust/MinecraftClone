#pragma once
#include "TypeList.hpp"

namespace core {
    template<typename F>
    struct FunctionDecompositionImpl { };

    template<typename R, typename... Args>
    struct FunctionDecompositionImpl<R(Args...)> {
        using Arguments = TypeList<Args...>;
        using ReturnType = R;
    };

    template<typename R, typename... Args>
    struct FunctionDecompositionImpl<R(*)(Args...)> {
        using Arguments = TypeList<Args...>;
        using ReturnType = R;
    };

    template<typename F>
    using ReturnTypeOf = typename FunctionDecompositionImpl<F>::ReturnType;
    template<typename F>
    using ArgumentsOf = typename FunctionDecompositionImpl<F>::Arguments;
    template<typename F, unsigned int Index>
    using NthArgumentOf = TypeAt<typename FunctionDecompositionImpl<F>::Arguments, Index>;
    template<typename F>
    using FirstArgumentOf = FirstOf<typename FunctionDecompositionImpl<F>::Arguments>;
    template<typename F>
    using LastArgumentOf = LastOf<typename FunctionDecompositionImpl<F>::Arguments>;
} // namespace core
