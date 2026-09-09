include(GoogleTest)

add_executable(mc_tests
    client/shader_assets_tests.cpp
    core/algorithm_tests.cpp
    core/byte_io_tests.cpp
    core/defer_tests.cpp
    core/enum_bits_tests.cpp
    core/field_requirement_tests.cpp
    core/game_server_tests.cpp
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
    core/shared_world_tests.cpp
    core/span_utils_tests.cpp
    core/static_initializer_tests.cpp
    core/tagged_bool_tests.cpp
    core/trivial_pair_tests.cpp
    core/type_name_tests.cpp
    core/vector_utils_tests.cpp
    core/version_tests.cpp
    core/vulkan/capabilities_tests.cpp
    core/vulkan/extensions_tests.cpp
    core/vulkan/instance_builder_tests.cpp
    core/vulkan/physical_device_selector_tests.cpp
    core/vulkan/spirv_tests.cpp
    core/window/keyboard_tests.cpp
    core/window/mouse_tests.cpp
    main.cpp
)

target_link_libraries(mc_tests
    PRIVATE
        mc
        GTest::gtest
)
mc_apply_target_defaults(mc_tests)
mc_copy_target_shaders(mc_tests mc)

gtest_discover_tests(mc_tests)

option(MC_ENABLE_RENDERER_SMOKE "Run noninteractive GPU smoke (requires a desktop and Vulkan validation)" OFF)
if(MC_ENABLE_RENDERER_SMOKE)
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
        FAIL_REGULAR_EXPRESSION "Vulkan validation; error at ; critical at ;Failed to acquire frame"
    )
endif()
