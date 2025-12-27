cmake_minimum_required(VERSION 4.0)
project(Shadberry
  VERSION 0.1.0
  DESCRIPTION "The shadberry game engine"
  LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/common.cmake")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SHADBERRY_BUILD_SHARED "Build Shadberry as shared library" OFF)
option(SHADBERRY_BUILD_TESTS "Build engine tests" OFF)
option(SHADBERRY_ENABLE_IPO "Enable interprocedural optimization (LTO/IPO) in Release" ON)
option(SHADBERRY_ARCH_AUTO "Enable arch-specific optimizations (Release)" ON)

if(NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_CONFIGURATION_TYPES "Debug;RelWithDebInfo;Release" CACHE STRING "" FORCE)
endif()

sb_collect_sources(${CMAKE_CURRENT_LIST_DIR} SHAD_SRC SHAD_HDRS)

if(SHADBERRY_BUILD_SHARED)
  add_library(Shadberry SHARED ${SHAD_SRC} ${SHAD_HDRS})
else()
  add_library(Shadberry STATIC ${SHAD_SRC} ${SHAD_HDRS})
endif()

sb_generate_public_headers_mirror(Shadberry "${CMAKE_CURRENT_BINARY_DIR}/include")

get_filename_component(SHADBERRY_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)
get_filename_component(SHADBERRY_SOURCE_PARENT "${SHADBERRY_SOURCE_DIR}" DIRECTORY)
target_include_directories(Shadberry
  PRIVATE
    ${SHADBERRY_SOURCE_DIR}
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<BUILD_INTERFACE:${SHADBERRY_SOURCE_PARENT}>
    $<INSTALL_INTERFACE:include/shadberry>
)

sb_setup_target_common(Shadberry)

find_package(Eigen3 CONFIG REQUIRED)
find_package(GLEW CONFIG REQUIRED)
find_package(OpenAL CONFIG REQUIRED)

find_path(RGFW_INCLUDE_DIR
    NAMES RGFW.h
    REQUIRED
)
add_library(rgfw INTERFACE)
target_include_directories(rgfw INTERFACE ${RGFW_INCLUDE_DIR})

target_link_libraries(Shadberry PRIVATE
  GLEW::GLEW
  OpenAL::OpenAL
  Eigen3::Eigen
)

if(SHADBERRY_BUILD_TESTS)
  sb_add_tests(${CMAKE_CURRENT_LIST_DIR} Shadberry)
endif()

include(CMakePackageConfigHelpers)
set(CONFIG_INSTALL_DIR "lib/cmake/Shadberry")

install(TARGETS Shadberry
  EXPORT ShadberryTargets
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include
)

install(EXPORT ShadberryTargets
  FILE ShadberryTargets.cmake
  NAMESPACE Shadberry::
  DESTINATION ${CONFIG_INSTALL_DIR}
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/ShadberryConfigVersion.cmake"
  VERSION 0.1.0
  COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/PackageConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/ShadberryConfig.cmake"
  INSTALL_DESTINATION ${CONFIG_INSTALL_DIR}
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/ShadberryConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/ShadberryConfigVersion.cmake"
  DESTINATION ${CONFIG_INSTALL_DIR}
)

install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include/" DESTINATION include)