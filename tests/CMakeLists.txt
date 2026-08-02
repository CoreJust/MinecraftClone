include(GoogleTest)

add_executable(mc_tests
    core/net_client_server_tests.cpp
    core/byte_io_tests.cpp
    main.cpp
)

target_link_libraries(mc_tests
    PRIVATE
        mc
        GTest::gtest
)
mc_apply_target_defaults(mc_tests)

gtest_discover_tests(mc_tests)
