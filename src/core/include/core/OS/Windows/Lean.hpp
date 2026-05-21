#pragma once

#include <core/macro/OS.hpp>

#ifndef WINDOWS
#   error This header must only be used for Windows.
#endif

// Reduce Windows header size
#ifndef WIN32_LEAN_AND_MEAN
#   define WIN32_LEAN_AND_MEAN
#endif
#ifndef VC_EXTRALEAN
#   define VC_EXTRALEAN
#endif

#include <Windows.h>
