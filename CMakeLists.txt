cmake_minimum_required(VERSION 3.24)

project(MinecraftClone2026
    VERSION 0.0.0
    DESCRIPTION "A clone of Minecraft created for self-study and fun reasons."
    LANGUAGES C CXX
)


###  C++ OPTIONS  ###
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


###  BUILD OPTIONS  ###
option(BUILD_TESTING "Build tests" ON)
option(MC_ENABLE_WARNINGS "Enable strict warnings" ON)
option(MC_ENABLE_SANITIZERS "Enable sanitizers where supported" OFF)
option(MC_ENABLE_HIGH_ASSERT "Enable high assertions" OFF)
if(MC_ENABLE_HIGH_ASSERT)
    add_compile_definitions(_MC_ENABLE_HIGH_ASSERT)
    add_compile_definitions(_MC_DEBUG)
endif()


###  DEPENDENCIES SETUP  ###
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(unofficial-enet CONFIG REQUIRED)


###  CMAKE HELPERS  ###
include(CTest)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Helpers)


###  BUILD  ###
add_executable(mc_main
    src/main.cpp
)
mc_apply_target_defaults(mc_main)
add_subdirectory(src)

if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests)
endif()
