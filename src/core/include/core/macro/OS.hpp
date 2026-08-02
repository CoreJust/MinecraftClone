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
#    error "Unknown Apple platform"
#  endif
#elif __ANDROID__
#  error Android OS not currently supported
#elif __linux__
#  error Linux not currently supported
#elif __unix__
#  error General UNIX not currently supported
#elif defined(_POSIX_VERSION)
#  error General POSIX not currently supported
#else
#  error OS not supported
#endif
