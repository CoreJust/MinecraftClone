if(NOT DEFINED ENV{CC} OR NOT DEFINED ENV{CXX})
    message(FATAL_ERROR "Clang CC and CXX must be set for Linux analysis ports")
endif()

if(NOT EXISTS "$ENV{CC}" OR NOT EXISTS "$ENV{CXX}")
    message(FATAL_ERROR "Linux analysis CC and CXX must resolve to compiler files")
endif()

set(CMAKE_C_COMPILER "$ENV{CC}" CACHE FILEPATH "Linux analysis C compiler")
set(CMAKE_CXX_COMPILER "$ENV{CXX}" CACHE FILEPATH "Linux analysis C++ compiler")
