include(GoogleTest)

add_executable(mc_tests
    test_app.cpp
)

target_link_libraries(mc_tests
    PRIVATE
        mc
        GTest::gtest_main
)
mc_apply_target_defaults(mc_tests)

gtest_discover_tests(mc_tests)
