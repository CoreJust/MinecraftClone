#include <shared/net/Message.hpp>

#include <gtest/gtest.h>

#include <array>
#include <type_traits>

namespace {

template<typename MessageTy>
void expectRoundTrip(MessageTy const expected) {
    shared::Message const message = expected;
    std::vector<uint8_t> const bytes = shared::encodeMessage(message);
    auto const decoded = shared::decodeMessage(std::span<uint8_t const>{ bytes.data(), bytes.size() });
    ASSERT_TRUE(decoded.has_value());
    ASSERT_TRUE(std::holds_alternative<MessageTy>(*decoded));
    MessageTy const actual = std::get<MessageTy>(*decoded);
    if constexpr (std::is_same_v<MessageTy, shared::JoinRequestMessage>) {
        EXPECT_EQ(actual.ch, expected.ch);
    } else if constexpr (std::is_same_v<MessageTy, shared::JoinResponseMessage>) {
        EXPECT_EQ(actual.accepted, expected.accepted);
    } else if constexpr (std::is_same_v<MessageTy, shared::ClientInputMessage>) {
        EXPECT_EQ(actual.direction.x, expected.direction.x);
        EXPECT_EQ(actual.direction.y, expected.direction.y);
    } else if constexpr (std::is_same_v<MessageTy, shared::ServerPlayerPositionMessage>) {
        EXPECT_EQ(actual.ch, expected.ch);
        EXPECT_EQ(actual.x, expected.x);
        EXPECT_EQ(actual.y, expected.y);
        EXPECT_EQ(actual.x_subcell, expected.x_subcell);
        EXPECT_EQ(actual.y_subcell, expected.y_subcell);
    } else {
        EXPECT_EQ(actual.ch, expected.ch);
    }
    EXPECT_EQ(shared::encodeMessage(actual), bytes);
}

} // namespace

TEST(MessageTest, RoundTripsEveryMessageKind) {
    expectRoundTrip(shared::JoinRequestMessage{ .ch = '@' });
    expectRoundTrip(shared::JoinResponseMessage{ .accepted = true });
    expectRoundTrip(shared::JoinResponseMessage{ .accepted = false });
    expectRoundTrip(shared::ClientInputMessage{ .direction = { 129, 127 } });
    expectRoundTrip(shared::ServerPlayerPositionMessage{
        .ch = '#', .x = 31, .y = 0, .x_subcell = 9'999, .y_subcell = 500,
    });
    expectRoundTrip(shared::ServerRemovePlayerMessage{ .ch = '$' });
}

TEST(MessageTest, RejectsTruncatedAndUnknownPayloads) {
    std::array<shared::Message, 5> const messages{
        shared::JoinRequestMessage{ .ch = '@' },
        shared::JoinResponseMessage{ .accepted = true },
        shared::ClientInputMessage{ .direction = { 129, 127 } },
        shared::ServerPlayerPositionMessage{
            .ch = '#', .x = 31, .y = 0, .x_subcell = 9'999, .y_subcell = 500,
        },
        shared::ServerRemovePlayerMessage{ .ch = '$' },
    };
    for (auto const& message : messages) {
        auto const bytes = shared::encodeMessage(message);
        for (uint64_t size = 0; size < bytes.size(); ++size) {
            EXPECT_FALSE(shared::decodeMessage(std::span{ bytes.data(), size }).has_value());
        }
    }
    std::array<uint8_t, 4> unknown{ 255, 0, 0, 0 };
    EXPECT_FALSE(shared::decodeMessage(unknown).has_value());
}

TEST(MessageTest, PreservesWireTagsAndPayloadBytes) {
    EXPECT_EQ(
        shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' }),
        (std::vector<uint8_t>{ 0, '@' })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::JoinResponseMessage{ .accepted = true }),
        (std::vector<uint8_t>{ 1, 1 })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::JoinResponseMessage{ .accepted = false }),
        (std::vector<uint8_t>{ 1, 0 })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::ClientInputMessage{ .direction = { 129, 127 } }),
        (std::vector<uint8_t>{ 2, 129, 127 })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::ServerPlayerPositionMessage{
            .ch = '#', .x = 31, .y = 0, .x_subcell = 9'999, .y_subcell = 500,
        }),
        (std::vector<uint8_t>{ 3, '#', 31, 0, 15, 39, 244, 1 })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::ServerRemovePlayerMessage{ .ch = '$' }),
        (std::vector<uint8_t>{ 4, '$' })
    );
}

TEST(MessageTest, RejectsNoncanonicalAcceptanceByte) {
    for (uint16_t value = 2; value <= 255; ++value) {
        std::array<uint8_t, 2> const bytes{ 1, static_cast<uint8_t>(value) };
        EXPECT_FALSE(shared::decodeMessage(bytes).has_value());
    }
}

TEST(MessageTest, RejectsTrailingBytes) {
    auto bytes = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' });
    bytes.push_back(0);
    auto const decoded = shared::decodeMessage(std::span<uint8_t const>{ bytes.data(), bytes.size() });
    EXPECT_FALSE(decoded.has_value());
}

TEST(MessageTest, RejectsInvalidPayloadValues) {
    auto input = shared::encodeMessage(shared::ClientInputMessage{ .direction = { 127, 127 } });
    input[1] = 128;
    EXPECT_FALSE(shared::decodeMessage(input).has_value());

    auto position = shared::encodeMessage(shared::ServerPlayerPositionMessage{
        .ch = '@', .x = 1, .y = 1, .x_subcell = 0, .y_subcell = 0,
    });
    position[2] = shared::World::WIDTH;
    EXPECT_FALSE(shared::decodeMessage(position).has_value());

    position = shared::encodeMessage(shared::ServerPlayerPositionMessage{
        .ch = '@', .x = 1, .y = 1, .x_subcell = 0, .y_subcell = 0,
    });
    position[4] = 16;
    position[5] = 39;
    EXPECT_FALSE(shared::decodeMessage(position).has_value());

    auto join = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '\n' });
    EXPECT_FALSE(shared::decodeMessage(join).has_value());
}
