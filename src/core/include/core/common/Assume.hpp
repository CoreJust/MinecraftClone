#pragma once

#ifdef _CORE_DEBUG
#  include "Assert.hpp"
#  define ASSUME(...)                           \
    do {                                        \
        bool const _value = (__VA_ARGS__);      \
        ASSERT(_value, "Incorrect assumption"); \
        [[assume(__VA_ARGS__)]];                \
    } while(0);
#else
#  define ASSUME(...) [[assume(__VA_ARGS__)]]
#endif
