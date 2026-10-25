get_filename_component(MC_WORLD_MAP_SOURCE ../tools/world_map.cpp ABSOLUTE)
set(MC_WORLD_MAP_SOURCE "${MC_WORLD_MAP_SOURCE}" PARENT_SCOPE)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(MinecraftGeneration LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    include(CTest)
    option(MC_GENERATION_BUILD_TESTS "Build optional generation tests" OFF)
endif()

add_library(mc_generation STATIC
    Coastal.cpp
    Climate.cpp
    Generation.cpp
    Geology.cpp
    Hydrology.cpp
    Numerics.cpp
    NumpyRandom.cpp
    NumpyTanh.cpp
    Sha256.cpp
    V9PlatecSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src/bounds.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src/geometry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src/heightmap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src/lithosphere.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src/mass.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src/movement.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src/noise.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src/plate.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src/plate_functions.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src/rectangle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src/segment_creator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src/segment_data.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src/segments.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src/simplerandom.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src/simplexnoise.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src/sqrdmd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src/utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src/world_point.cpp
)

target_sources(mc_generation
    PUBLIC
        FILE_SET public_headers TYPE HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
        FILES include/generation/Generation.hpp
    PRIVATE
        FILE_SET private_headers TYPE HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            GenerationStages.hpp
            NumpyRandom.hpp
            Numerics.hpp
            Sha256.hpp
            V9PlatecSource.hpp
)

target_include_directories(mc_generation
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src
)
target_compile_features(mc_generation PUBLIC cxx_std_23)
if(ANDROID)
    # NDK 27 libc++ gates C++20 cancellation APIs behind its experimental-library option.
    target_compile_options(mc_generation PUBLIC -fexperimental-library)
endif()
if(MSVC)
    target_compile_options(mc_generation PRIVATE /fp:strict)
else()
    target_compile_options(mc_generation PRIVATE -ffp-contract=off)
endif()

# The frozen build contracts specific Platec expressions. Spell those out with
# std::fma so the output does not depend on compiler or CPU feature flags.
get_target_property(MC_GENERATION_SOURCES mc_generation SOURCES)
set(MC_PLATEC_SOURCE_FILES)
foreach(MC_GENERATION_SOURCE IN LISTS MC_GENERATION_SOURCES)
    if(MC_GENERATION_SOURCE MATCHES "/third_party/platec/cpp_src/.*\\.cpp$")
        list(APPEND MC_PLATEC_SOURCE_FILES "${MC_GENERATION_SOURCE}")
    endif()
endforeach()
if(MSVC)
    set_source_files_properties(${MC_PLATEC_SOURCE_FILES} PROPERTIES COMPILE_OPTIONS "/fp:precise")
else()
    set_source_files_properties(${MC_PLATEC_SOURCE_FILES} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-fno-strict-overflow")
endif()

if(COMMAND mc_apply_target_defaults)
    mc_apply_target_defaults(mc_generation)
endif()

if(TARGET mc)
    target_link_libraries(mc PUBLIC mc_generation)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_library(mc_world_sampling STATIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../shared/world/Chunk.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../shared/world/TerrainWorld.cpp"
    )
    target_link_libraries(mc_world_sampling PUBLIC mc_generation)
    target_include_directories(mc_world_sampling
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/../shared/include"
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/.."
    )
    target_compile_features(mc_world_sampling PUBLIC cxx_std_23)
    if(MSVC)
        target_compile_options(mc_world_sampling PRIVATE /fp:strict)
    else()
        target_compile_options(mc_world_sampling PRIVATE -ffp-contract=off)
    endif()

    add_executable(mc_world_map "${MC_WORLD_MAP_SOURCE}")
    target_link_libraries(mc_world_map PRIVATE mc_world_sampling)
    target_compile_features(mc_world_map PRIVATE cxx_std_23)
    if(WIN32)
        target_link_libraries(mc_world_map PRIVATE Psapi)
    endif()

    if(BUILD_TESTING)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(
                NAME TerrainWorldMap.Cli
                COMMAND ${Python3_EXECUTABLE}
                    "${CMAKE_CURRENT_SOURCE_DIR}/../../script/tests/test_world_map_cli.py"
                    --executable $<TARGET_FILE:mc_world_map>
            )
            set_tests_properties(TerrainWorldMap.Cli PROPERTIES
                TIMEOUT 180
                LABELS "generation;tools"
            )
        endif()
    endif()
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND MC_GENERATION_BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)

    set(MC_REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    add_executable(mc_generation_tests
        "${MC_REPOSITORY_ROOT}/tests/generation/coastal_tests.cpp"
        "${MC_REPOSITORY_ROOT}/tests/generation/climate_tests.cpp"
        "${MC_REPOSITORY_ROOT}/tests/generation/generation_tests.cpp"
        "${MC_REPOSITORY_ROOT}/tests/generation/geology_tests.cpp"
        "${MC_REPOSITORY_ROOT}/tests/generation/hydrology_tests.cpp"
        "${MC_REPOSITORY_ROOT}/tests/generation/numpy_random_tests.cpp"
        "${MC_REPOSITORY_ROOT}/tests/generation/platec_compat_tests.cpp"
        "${MC_REPOSITORY_ROOT}/tests/generation/reference_tests.cpp"
        "${MC_REPOSITORY_ROOT}/tests/generation/v9_platec_source_tests.cpp"
    )
    target_link_libraries(mc_generation_tests PRIVATE mc_generation GTest::gtest_main)
    target_include_directories(mc_generation_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/platec/cpp_src
    )
    target_compile_definitions(mc_generation_tests PRIVATE
        MC_TERRAIN_REFERENCE_FIXTURE_DIR="${MC_REPOSITORY_ROOT}/tests/fixtures/terrain_v2"
        MC_V9_PLATEC_FIXTURE_DIR="${MC_REPOSITORY_ROOT}/tests/fixtures/terrain_v9"
    )
    target_compile_features(mc_generation_tests PRIVATE cxx_std_23)
    include(GoogleTest)
    gtest_discover_tests(mc_generation_tests)
endif()
