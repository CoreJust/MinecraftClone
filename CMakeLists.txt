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

### Options ###
option(MINECRAFT_CLONE_TESTING "Enable unit tests" OFF)
if(MINECRAFT_CLONE_TESTING)
    add_compile_definitions(_TEST)
endif()
option(MINECRAFT_CLONE_DEBUG "Enable debug" OFF)
if(MINECRAFT_CLONE_DEBUG)
    add_compile_definitions(_MC_DEBUG)
endif()


### Dependencies  ###
# Vulkan
find_package(Vulkan REQUIRED)

# GLFW
FetchContent_Declare(
    GLFW
    GIT_REPOSITORY "https://github.com/glfw/glfw"
    GIT_TAG        3.4
)
FetchContent_MakeAvailable(GLFW)
set(GLFW_INCLUDE ${PROJECT_SOURCE_DIR}/third_party/glfw-src/include)


### Language Setup ###
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

add_compile_options(
    -march=native
    -Werror
    -Wall
    -Wextra
    -Wpedantic
    -Wcast-align
    -Wcast-qual
    -Wconversion
    -Wctor-dtor-privacy
    -Wenum-compare
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Woverloaded-virtual
    -Wredundant-decls
    -Wsign-conversion
    -Wsign-promo
)

add_subdirectory(src)
