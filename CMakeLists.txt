cmake_minimum_required(VERSION 3.25)

project(MinecraftClone2026
    VERSION 0.1.0
    DESCRIPTION "A clone of Minecraft created for self-study and fun reasons."
    LANGUAGES C CXX
)

if(NOT WIN32 AND NOT APPLE AND NOT ANDROID)
    message(FATAL_ERROR "MinecraftClone currently supports only Windows, macOS, and Android")
endif()


###  C++ OPTIONS  ###
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


###  BUILD OPTIONS  ###
option(BUILD_TESTING "Build tests" ON)
option(MC_ENABLE_WARNINGS "Enable strict warnings" ON)
option(MC_ENABLE_SANITIZERS "Enable sanitizers where supported" OFF)
option(MC_ENABLE_HIGH_ASSERT "Enable high assertions" OFF)
option(MC_ENABLE_VULKAN_VALIDATION_LAYERS "Enable Vulkan validation layers" OFF)
option(
    MC_ALLOW_INEXACT_CORECPP
    "Allow a dirty, unknown, or mismatched CoreCpp package for local development"
    OFF
)
if(ANDROID)
    # Tests target desktop-only window and process contracts. Android builds are
    # produced by the Gradle application module and never include CTest.
    set(BUILD_TESTING OFF CACHE BOOL "Build tests" FORCE)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR MC_ENABLE_HIGH_ASSERT)
    add_compile_definitions(_MC_DEBUG)
    add_compile_definitions(_CORE_DEBUG)
endif()
if(MC_ENABLE_HIGH_ASSERT)
    add_compile_definitions(_MC_ENABLE_HIGH_ASSERT)
    add_compile_definitions(_CORE_ENABLE_HIGH_ASSERT)
endif()
if(MC_ENABLE_VULKAN_VALIDATION_LAYERS)
    add_compile_definitions(_MC_VK_VALIDATION_LAYERS)
endif()


###  DEPENDENCIES SETUP  ###
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CoreCppProvenance.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CoreProject2026Provenance.cmake")
find_package(CoreCpp CONFIG REQUIRED COMPONENTS Core Runtime RuntimeNetwork)
mc_check_corecpp_provenance()
find_package(CoreProject2026 CONFIG REQUIRED COMPONENTS CoreLang)
mc_check_coreproject2026_provenance()

find_package(fmt CONFIG REQUIRED)
if(NOT ANDROID)
    find_package(glfw3 CONFIG REQUIRED)
endif()
find_package(glm CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-spirv-reflect CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
if(ANDROID)
    find_package(Vulkan REQUIRED)
else()
    find_package(Vulkan REQUIRED COMPONENTS glslc)
endif()
find_package(VulkanMemoryAllocator CONFIG REQUIRED)


###  CMAKE HELPERS  ###
include(CTest)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Helpers)


###  BUILD  ###
if(NOT ANDROID)
    add_executable(mc_main
        src/main.cpp
    )
    mc_apply_target_defaults(mc_main)
endif()
add_subdirectory(src)

if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests)
endif()
