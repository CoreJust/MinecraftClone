include(GoogleTest)

add_executable(mc_tests
    main.cpp
    core/byte_io_tests.cpp
    core/defer_tests.cpp
    core/enum_bits_tests.cpp
    core/hash_combine_tests.cpp
    core/net_client_server_tests.cpp
    core/non_movable_tests.cpp
    core/non_copyable_tests.cpp
    core/range_span_tests.cpp
    core/static_initializer_tests.cpp
    core/trivial_pair_tests.cpp
    core/version_tests.cpp
)

target_link_libraries(mc_tests
    PRIVATE
        mc
        GTest::gtest
)
mc_apply_target_defaults(mc_tests)

gtest_discover_tests(mc_tests)
