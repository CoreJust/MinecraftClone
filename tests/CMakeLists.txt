include(GoogleTest)

configure_file(
    cmake/corecpp_server_consumer_test.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/corecpp_server_consumer_test.cmake"
    @ONLY
)
add_test(
    NAME CoreCpp.ServerOnlyPackageConsumer
    COMMAND ${CMAKE_COMMAND}
        -P "${CMAKE_CURRENT_BINARY_DIR}/corecpp_server_consumer_test.cmake"
)

configure_file(
    cmake/minecraftclone_server_only_test.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/minecraftclone_server_only_test.cmake"
    @ONLY
)
configure_file(
    cmake/coreproject2026_provenance_test.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/coreproject2026_provenance_test.cmake"
    @ONLY
)
add_test(
    NAME CoreProject2026.Provenance
    COMMAND ${CMAKE_COMMAND}
        -P "${CMAKE_CURRENT_BINARY_DIR}/coreproject2026_provenance_test.cmake"
)
option(
    MC_REGISTER_SERVER_ONLY_BUILD_TEST
    "Register the nested MinecraftClone server-only build test"
    ON
)
if(MC_REGISTER_SERVER_ONLY_BUILD_TEST)
    add_test(
        NAME MinecraftClone.ServerOnlyBuild
        COMMAND ${CMAKE_COMMAND}
            -P "${CMAKE_CURRENT_BINARY_DIR}/minecraftclone_server_only_test.cmake"
    )
endif()

if(MC_BUILD_CLIENT AND NOT ANDROID)
    add_test(
        NAME AcceptanceCli.RuntimeModes
        COMMAND ${CMAKE_COMMAND}
            -DMC_MAIN=$<TARGET_FILE:mc_main>
            -DOUTPUT_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/acceptance-cli
            -P ${CMAKE_CURRENT_SOURCE_DIR}/acceptance/runtime_cli_tests.cmake
    )
    set_tests_properties(AcceptanceCli.RuntimeModes PROPERTIES TIMEOUT 20)
endif()

add_executable(mc_tests
    core/algorithm_tests.cpp
    core/byte_io_tests.cpp
    core/crash_handler_tests.cpp
    core/defer_tests.cpp
    core/enum_bits_tests.cpp
    core/field_requirement_tests.cpp
    core/game_server_tests.cpp
    core/game_server_lifecycle_tests.cpp
    core/hash_combine_tests.cpp
    core/input_span_tests.cpp
    core/io_tests.cpp
    core/log_tests.cpp
    core/macro_tests.cpp
    core/enum_meta_tests.cpp
    core/net_client_server_tests.cpp
    core/net_tests.cpp
    core/non_copyable_tests.cpp
    core/non_movable_tests.cpp
    core/os_tests.cpp
    core/range_span_tests.cpp
    core/shared_message_tests.cpp
    core/scenario_parser_tests.cpp
    core/shared_world_tests.cpp
    core/span_utils_tests.cpp
    core/static_initializer_tests.cpp
    core/tagged_bool_tests.cpp
    core/trivial_pair_tests.cpp
    core/type_name_tests.cpp
    core/vector_utils_tests.cpp
    core/version_tests.cpp
    main.cpp
)

target_link_libraries(mc_tests
    PRIVATE
        mc
        GTest::gtest
)
mc_apply_target_defaults(mc_tests)

if(MC_BUILD_CLIENT AND NOT ANDROID)
    target_sources(mc_tests
        PRIVATE
            android/input_state_tests.cpp
            acceptance/evidence_json_tests.cpp
            acceptance/image_evidence_tests.cpp
            acceptance/renderer_benchmark_tests.cpp
            acceptance/scenario_runner_tests.cpp
            client/debug_hud_tests.cpp
            client/shader_assets_tests.cpp
            core/corelang_scenario_tests.cpp
    )
    target_include_directories(mc_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/android)
    mc_copy_target_shaders(mc_tests mc)
endif()

gtest_discover_tests(mc_tests)

if(MC_BUILD_CLIENT AND NOT ANDROID)
    find_program(MC_SPIRV_VAL_EXECUTABLE
        NAMES spirv-val
        HINTS "$ENV{VULKAN_SDK}"
        PATH_SUFFIXES bin Bin
        REQUIRED
    )

    foreach(SHADER_TARGET_ENV IN ITEMS
        "grid.mesh.spv;vulkan1.3"
        "player.mesh.spv;vulkan1.3"
        "grid.vert.spv;vulkan1.2"
        "player.vert.spv;vulkan1.2"
        "trivial.frag.spv;vulkan1.2"
        "debug_hud.vert.spv;vulkan1.2"
        "debug_hud.frag.spv;vulkan1.2"
    )
        list(GET SHADER_TARGET_ENV 0 SHADER_FILE)
        list(GET SHADER_TARGET_ENV 1 VULKAN_TARGET_ENV)
        add_test(
            NAME ShaderSpirvTargetTest.${SHADER_FILE}
            COMMAND ${MC_SPIRV_VAL_EXECUTABLE}
                --target-env ${VULKAN_TARGET_ENV}
                "$<TARGET_FILE_DIR:mc_tests>/shaders/${SHADER_FILE}"
        )
        set_tests_properties(
            ShaderSpirvTargetTest.${SHADER_FILE}
            PROPERTIES LABELS "shader;spirv"
        )
    endforeach()
endif()

option(MC_ENABLE_RENDERER_SMOKE "Run noninteractive GPU smoke (requires a desktop and Vulkan validation)" OFF)
if(MC_BUILD_CLIENT AND NOT ANDROID AND MC_ENABLE_RENDERER_SMOKE)
    add_executable(mc_renderer_smoke client/renderer_smoke_tests.cpp main.cpp)
    set_target_properties(mc_renderer_smoke PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/renderer-smoke"
    )
    target_link_libraries(mc_renderer_smoke PRIVATE mc GTest::gtest)
    mc_apply_target_defaults(mc_renderer_smoke)
    mc_copy_target_shaders(mc_renderer_smoke mc)
    add_test(NAME RendererSmokeTest COMMAND mc_renderer_smoke)
    set_tests_properties(RendererSmokeTest PROPERTIES
        TIMEOUT 45
        RUN_SERIAL TRUE
        LABELS "renderer;gpu"
        FAIL_REGULAR_EXPRESSION "Vulkan validation; error at ; critical at "
    )
endif()
