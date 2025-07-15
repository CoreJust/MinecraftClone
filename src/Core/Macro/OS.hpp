#pragma once

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
#  define WINDOWS 1
#elif __APPLE__
#  include "TargetConditionals.h"
#  if TARGET_IPHONE_SIMULATOR
#    error iPhone simulators not currently supported
#    define IOS_SIMULATOR 1
#  elif TARGET_OS_MACCATALYST
#    error Mac's Catalyst not currently supported
#    define MAC_CATALYST 1
#  elif TARGET_OS_IPHONE
#    error iOS not currently supported
#    define IOS 1
#  elif TARGET_OS_MAC
#    define OSX 1
#  else
#   error "Unknown Apple platform"
#  endif
#elif __ANDROID__
#  error Andoid OS not currently supported
#  define ANDROID 1
#elif __linux__
#  define LINUX 1
#elif __unix__
#  error General UNIX not currently supported
#  define UNIX 1
#elif defined(_POSIX_VERSION)
#  error General POSIX not currently supported
#  define POSIX 1
#else
#  error OS not supported
#endif
