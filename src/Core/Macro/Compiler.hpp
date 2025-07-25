#pragma once

#if defined(__clang__)
#  define CLANG 1
#  define COMPILER_VERSION_MAJOR __clang_major__
#  define COMPILER_VERSION_MINOR __clang_minor__
#  define COMPILER_VERSION_PATCH __clang_patchlevel__
#elif defined(__GNUC__)
#  define GCC 1
#  define COMPILER_VERSION_MAJOR __GNUC__ 
#  define COMPILER_VERSION_MINOR __GNUC_MINOR__ 
#  define COMPILER_VERSION_PATCH 0
#elif defined(_MSC_VER)
#  define MSVC 1
#  define COMPILER_VERSION_MAJOR _MSC_VER 
#  define COMPILER_VERSION_MINOR _MSC_FULL_VER 
#  define COMPILER_VERSION_PATCH 0
#endif