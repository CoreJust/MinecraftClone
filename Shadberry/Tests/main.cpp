#include <Shadberry/Version.hpp>
#include <Shadberry/System/Console.hpp>

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <iostream>

using namespace sb;

TEST(Main, TestFail) {
    std::cout << "FAIL!" << std::endl;
    ASSERT_TRUE(false);
}

TEST(Main, TestSuccess) {
    std::cout << "SUCCESS!" << std::endl;
    ASSERT_TRUE(true);
}
