#include <core/net/Address.hpp>
#include <core/net/Host.hpp>
#include <core/net/SendMode.hpp>

#include <gtest/gtest.h>

#include <chrono>

TEST(Net, CreatesValidAddress) {
    auto const address = core::Address::make("127.0.0.1", 20'040);

    ASSERT_TRUE(address.has_value());
    EXPECT_EQ(address->port(), 20'040);
    EXPECT_EQ(address->ip(), "127.0.0.1");
}

TEST(Net, RejectsInvalidAddress) {
    EXPECT_FALSE(core::Address::make("not-an-ip-address", 20'040).has_value());
}

TEST(Net, FormatsAddress) {
    auto const address = core::Address::make("127.0.0.1", 20'040);
    ASSERT_TRUE(address.has_value());
    EXPECT_EQ(fmt::format("{}", *address), "127.0.0.1:20040");
}

TEST(Net, ValidatesReliableAndUnreliableModes) {
    EXPECT_TRUE(core::SendMode{ core::SendMode::Reliable }.isValid());
    EXPECT_TRUE(core::SendMode{ }.isValid());
}

TEST(Net, RepeatedlyPollWithZeroTimeoutReturns) {
    core::Host host{ std::nullopt, 1, 1 };

    auto const start = std::chrono::steady_clock::now();
    size_t const polled = host.repeatedlyPoll([](core::NetEvent) { }, std::chrono::milliseconds{ 0 });
    auto const elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(polled, 0u);
    EXPECT_LT(elapsed, std::chrono::seconds{ 1 });
}
