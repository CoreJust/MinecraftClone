if(NOT DEFINED ENV{CC} OR NOT DEFINED ENV{CXX})
    message(FATAL_ERROR "Clang CC and CXX must be set for Linux analysis ports")
endif()

if(NOT EXISTS "$ENV{CC}" OR IS_DIRECTORY "$ENV{CC}"
        OR NOT EXISTS "$ENV{CXX}" OR IS_DIRECTORY "$ENV{CXX}")
    message(FATAL_ERROR "Linux analysis CC and CXX must resolve to compiler files")
endif()

if(NOT DEFINED ENV{VCPKG_ROOT} OR "$ENV{VCPKG_ROOT}" STREQUAL "")
    message(FATAL_ERROR "VCPKG_ROOT must identify the pinned vcpkg checkout for Linux analysis ports")
endif()

set(_vcpkg_linux_toolchain "$ENV{VCPKG_ROOT}/scripts/toolchains/linux.cmake")
if(NOT EXISTS "${_vcpkg_linux_toolchain}" OR IS_DIRECTORY "${_vcpkg_linux_toolchain}")
    message(FATAL_ERROR "Pinned vcpkg Linux toolchain is missing: ${_vcpkg_linux_toolchain}")
endif()

set(CMAKE_C_COMPILER "$ENV{CC}" CACHE FILEPATH "Linux analysis C compiler")
set(CMAKE_CXX_COMPILER "$ENV{CXX}" CACHE FILEPATH "Linux analysis C++ compiler")

include("${_vcpkg_linux_toolchain}")
unset(_vcpkg_linux_toolchain)
