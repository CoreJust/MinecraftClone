if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(MinecraftTerrainCoreLangAdapter LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    include(CTest)

    find_package(CoreCpp CONFIG REQUIRED COMPONENTS Core Runtime)
    find_package(CoreProject2026 0.1.5 CONFIG REQUIRED COMPONENTS CoreLang)

    add_subdirectory(
        "${CMAKE_CURRENT_LIST_DIR}/../../generation"
        "${CMAKE_CURRENT_BINARY_DIR}/generation"
    )

    add_library(mc_world_sampling STATIC
        "${CMAKE_CURRENT_LIST_DIR}/../world/Chunk.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../world/TerrainWorld.cpp"
    )
    target_include_directories(mc_world_sampling PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
    target_link_libraries(mc_world_sampling PUBLIC mc_generation)
endif()

add_library(mc_terrain_corelang STATIC
    Backend.cpp
    Execution.cpp
    ../world/WorldGenerationPlan.cpp
)
target_include_directories(mc_terrain_corelang
    PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/../include"
    PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../../generation/include"
)
target_link_libraries(mc_terrain_corelang
    PUBLIC
        mc_generation
        mc_world_sampling
        CoreCpp::Core
        CoreCpp::Runtime
        CoreProject2026::CoreLang
)
target_compile_features(mc_terrain_corelang PUBLIC cxx_std_23)
if(COMMAND mc_apply_target_defaults)
    mc_apply_target_defaults(mc_terrain_corelang)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(mc_terrain_corelang PRIVATE -Wall -Wextra -Wpedantic -Werror)
elseif(MSVC)
    target_compile_options(mc_terrain_corelang PRIVATE /W4 /WX)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    add_executable(mc_terrain_corelang_tests
        "${CMAKE_CURRENT_LIST_DIR}/../../../tests/core/corelang_backend_tests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../tests/core/corelang_terrain_plan_tests.cpp"
    )
    target_link_libraries(mc_terrain_corelang_tests
        PRIVATE
            mc_terrain_corelang
            GTest::gtest_main
    )
    target_compile_features(mc_terrain_corelang_tests PRIVATE cxx_std_23)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(mc_terrain_corelang_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    elseif(MSVC)
        target_compile_options(mc_terrain_corelang_tests PRIVATE /W4 /WX)
    endif()
    add_test(NAME CoreLangBackend COMMAND mc_terrain_corelang_tests --gtest_filter=CoreLangBackendTest.*)
    add_test(NAME CoreLangTerrainPlan COMMAND mc_terrain_corelang_tests --gtest_filter=CoreLangTerrainPlanTest.*)
endif()
