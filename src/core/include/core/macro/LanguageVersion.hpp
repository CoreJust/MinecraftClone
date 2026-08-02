// DONT_CHECK_ANY DIGIT_SEPARATORS
#pragma once
#include "Compiler.hpp"

#ifdef CORE_LANG_VERSION
#  undef CORE_LANG_VERSION
#endif
#if MSVC && _MSVC_LANG > __cplusplus
#  define CORE_LANG_VERSION _MSVC_LANG
#else
#  define CORE_LANG_VERSION __cplusplus
#endif

#define PRE_CPP98 1
#if CORE_LANG_VERSION == 1
#  define CPP98 0
#  define CPP11 0
#  define CPP14 0
#  define CPP17 0
#  define CPP20 0
#  define CPP23 0
#  define LANG_VERSION PRE_98
#else
#  define CPP98 1
#  if CORE_LANG_VERSION == 199711L
#    define CPP11 0
#    define CPP14 0
#    define CPP17 0
#    define CPP20 0
#    define CPP23 0
#    define LANG_VERSION 98
#  else
#    define CPP11 1
#    if CORE_LANG_VERSION == 201103L
#      define CPP14 0
#      define CPP17 0
#      define CPP20 0
#      define CPP23 0
#      define LANG_VERSION 11
#    elif CORE_LANG_VERSION == 201402L
#      define CPP14 1
#      define CPP17 0
#      define CPP20 0
#      define CPP23 0
#      define LANG_VERSION 14
#    elif CORE_LANG_VERSION == 201703L
#      define CPP14 1
#      define CPP17 1
#      define CPP20 0
#      define CPP23 0
#      define LANG_VERSION 17
#    elif CORE_LANG_VERSION == 202002L
#      define CPP14 1
#      define CPP17 1
#      define CPP20 1
#      define CPP23 0
#      define LANG_VERSION 20
#    elif CORE_LANG_VERSION == 202302L
#      define CPP14 1
#      define CPP17 1
#      define CPP20 1
#      define CPP23 1
#      define LANG_VERSION 23
#    elif CORE_LANG_VERSION > 202302L
#      define CPP14 1
#      define CPP17 1
#      define CPP20 1
#      define CPP23 1
#      define LANG_VERSION POST_23
#    else
#      error Unrecognized language version.
#    endif
#  endif
#endif
