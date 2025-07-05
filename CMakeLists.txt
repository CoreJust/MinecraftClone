cmake_minimum_required(VERSION 4.0)

project(
    MinecraftClone
    VERSION 0.0 
    LANGUAGES CXX
    DESCRIPTION "A simple clone of minecraft")
    
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(FETCHCONTENT_BASE_DIR ${PROJECT_SOURCE_DIR}/third_party CACHE PATH "Missing description." FORCE)

include(FetchContent)
include(cmake/submodule.cmake)

### Dependencies  ###
# DocTest
option(MINECRAFT_CLONE_TESTING "Enable unit tests for MINECRAFT_CLONE_TESTING" ON)
if(MINECRAFT_CLONE_TESTING)
    include(CTest)
    FetchContent_Declare(
        DocTest
        GIT_REPOSITORY "https://github.com/onqtam/doctest"
        GIT_TAG        v2.4.12
    )
    FetchContent_MakeAvailable(DocTest)
    add_compile_definitions(MINECRAFT_CLONE_TESTING)
    add_compile_definitions(ENABLE_DOCTEST_IN_LIBRARY)
    set(DOCTEST_INCLUDE ${PROJECT_SOURCE_DIR}/third_party/doctest-src)
endif()

# Vulkan
find_package(Vulkan REQUIRED)

# RGFW
FetchContent_Declare(
    RGFW
    GIT_REPOSITORY "https://github.com/ColleagueRiley/RGFW"
    GIT_TAG        1.70
)
FetchContent_MakeAvailable(RGFW)
add_library(RGFW INTERFACE)
set(RGFW_INCLUDE ${PROJECT_SOURCE_DIR}/third_party/rgfw-src)


### Language Setup ###
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

add_compile_options(
    -Werror
    -Wall
    -Wextra
    -Wpedantic
    -Wcast-align
    -Wcast-qual
    -Wconversion
    -Wctor-dtor-privacy
    -Wenum-compare
    -Wfloat-equal
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Woverloaded-virtual
    -Wredundant-decls
    -Wsign-conversion
    -Wsign-promo
)

add_subdirectory(src)
