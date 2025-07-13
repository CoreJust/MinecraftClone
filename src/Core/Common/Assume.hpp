#pragma once

#ifdef _MC_DEBUG
#  include "Assert.hpp"
#  define ASSUME(...) { bool const _value = (__VA_ARGS__); ASSERT(_value); [[assume(__VA_ARGS__)]]; }
#else
#  define ASSUME(...) [[assume(__VA_ARGS__)]]
#endif
