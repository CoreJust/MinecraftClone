cmake_minimum_required(VERSION 4.0)
project(MinecraftClone
  VERSION 0.1.0
  DESCRIPTION "Sample project using the Shadberry Engine - a clone of Minecraft"
  LANGUAGES CXX)

option(MCC_BUILD_TESTS "Build Minecraft Clone tests" ON)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/common.cmake)

find_package(Shadberry CONFIG REQUIRED)

sb_collect_sources(${CMAKE_CURRENT_LIST_DIR} MC_SOURCES MC_HEADERS)
add_executable(MinecraftClone ${MC_SOURCES} ${MC_HEADERS})

target_include_directories(MinecraftClone PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(MinecraftClone PRIVATE Shadberry::Shadberry)

find_package(Eigen3 CONFIG REQUIRED)
target_link_libraries(MinecraftClone PRIVATE Eigen3::Eigen)

sb_setup_target_common(MinecraftClone)

if(MCC_BUILD_TESTS)
  enable_testing()
  sb_add_tests(${CMAKE_CURRENT_LIST_DIR} MinecraftClone)
endif()
