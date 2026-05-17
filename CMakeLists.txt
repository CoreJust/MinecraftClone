cmake_minimum_required(VERSION 3.24)

project(MinecraftClone2026
    VERSION 0.1.0
    DESCRIPTION "A clone of Minecraft created for self-study and fun reasons."
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTING "Build tests" ON)
option(MC_ENABLE_WARNINGS "Enable strict warnings" ON)
option(MC_ENABLE_SANITIZERS "Enable sanitizers where supported" OFF)
option(MC_ENABLE_HIGH_ASSERT "Enable high assertions" OFF)

find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

include(CTest)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Helpers)

if(MC_ENABLE_HIGH_ASSERT)
    add_compile_definitions(MC_ENABLE_HIGH_ASSERT)
endif()

add_executable(mc_main
    src/main.cpp
)
mc_apply_target_defaults(mc_main)
add_subdirectory(src)

if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests)
endif()
