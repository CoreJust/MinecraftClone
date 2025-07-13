#pragma once
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
#  include <Windows.h>
#  define OS WINDOWS
#elif __APPLE__
#  include "TargetConditionals.h"
#  if TARGET_IPHONE_SIMULATOR
#    error iPhone simulators not currently supported
#    define OS IOS_SIMULATOR
#  elif TARGET_OS_MACCATALYST
#    error Mac's Catalyst not currently supported
#    define OS MAC_CATALYST
#  elif TARGET_OS_IPHONE
#    error iOS not currently supported
#    define OS IOS
#  elif TARGET_OS_MAC
#    define OS OSX
#  else
#   error "Unknown Apple platform"
#  endif
#elif __ANDROID__
#  error Andoid OS not currently supported
#  define OS ANDROID
#elif __linux__
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/stat.h>
#  include <sys/epoll.h>
#  include <sys/socket.h>
#  include <sys/ioctl.h>
#  include <fcntl.h>
#  include <pthread.h>
#  include <linux/input.h>
#  define OS LINUX
#elif __unix__
#  error General UNIX not currently supported
#  define OS UNIX
#elif defined(_POSIX_VERSION)
#  error General POSIX not currently supported
#  define OS POSIX
#else
#  error OS not supported
#endif
