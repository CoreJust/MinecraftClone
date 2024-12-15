#pragma once

#if defined(_MSVC_LANG) && _MSVC_LANG > __cplusplus
#define LANG_VERSION _MSVC_LANG
#else
#define LANG_VERSION __cplusplus
#endif

#if LANG_VERSION < 202002L
#error "C++ language version must be c++20 or higher"
#endif

#if defined(_MSC_VER)
#define INLINE __forceinline
#else
#define INLINE inline __attribute__((always_inline))
#endif

#if defined(_MSC_VER)
#define PURE [[nodiscard]]
#else
#define PURE [[nodiscard]] __attribute__((const))
#endif

#if defined(_MSC_VER)
#define NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

#if !defined(__has_cpp_attribute)
#define LIKELY
#define UNLIKELY
#elif !(__has_cpp_attribute(likely) && __has_cpp_attribute(unlikely))
#define LIKELY
#define UNLIKELY
#else
#define LIKELY [[likely]]
#define UNLIKELY [[unlikely]]
#endif
