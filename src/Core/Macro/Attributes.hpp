#pragma once
#include "Compiler.hpp"

#ifdef INLINE
#  undef INLINE
#endif
#if COMPILER == MSVC
#  define INLINE __forceinline
#else
#  define INLINE inline __attribute__((always_inline))
#endif

#ifdef PURE
#  undef PURE
#endif
#if COMPILER == MSVC
#  define PURE [[nodiscard]]
#else
#  define PURE [[nodiscard]] __attribute__((const))
#endif

#ifdef NO_UNIQUE_ADDRESS
#  undef NO_UNIQUE_ADDRESS
#endif
#if COMPILER == MSVC
#  define NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#  define NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

#ifdef LIKELY
#  undef LIKELY
#endif
#ifdef UNLIKELY
#  undef UNLIKELY
#endif
#if !defined(__has_cpp_attribute)
#  define LIKELY
#  define UNLIKELY
#elif !(__has_cpp_attribute(likely) && __has_cpp_attribute(unlikely))
#  define LIKELY
#  define UNLIKELY
#else
#  define LIKELY [[likely]]
#  define UNLIKELY [[unlikely]]
#endif
