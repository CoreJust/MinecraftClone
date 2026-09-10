#include <core/IO/Log.hpp>
#include <core/net/Net.hpp>

#include <gtest/gtest.h>

class CoreEnvironment final : public ::testing::Environment {
public:
    void SetUp() override {
        core::NetInitDestroy::init();
        core::Log::ensureInit();
    }

    void TearDown() override {
        core::NetInitDestroy::destroy();
    }
};

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new CoreEnvironment);
    return RUN_ALL_TESTS();
}
