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
        EXPECT_EQ(actual.mode, expected.mode);
        EXPECT_EQ(actual.configuration, expected.configuration);
        EXPECT_EQ(actual.wants_previews, expected.wants_previews);
    } else if constexpr (std::is_same_v<MessageTy, shared::JoinResponseMessage>) {
        EXPECT_EQ(actual.accepted, expected.accepted);
    } else if constexpr (std::is_same_v<MessageTy, shared::ClientInputMessage>) {
        EXPECT_EQ(actual.direction.x, expected.direction.x);
        EXPECT_EQ(actual.direction.y, expected.direction.y);
        EXPECT_EQ(actual.direction.z, expected.direction.z);
        EXPECT_EQ(actual.sequence, expected.sequence);
    } else if constexpr (std::is_same_v<MessageTy, shared::ServerPlayerPositionMessage>) {
        EXPECT_EQ(actual.ch, expected.ch);
        EXPECT_EQ(actual.x, expected.x);
        EXPECT_EQ(actual.y, expected.y);
        EXPECT_EQ(actual.z, expected.z);
        EXPECT_EQ(actual.x_subcell, expected.x_subcell);
        EXPECT_EQ(actual.y_subcell, expected.y_subcell);
        EXPECT_EQ(actual.z_subcell, expected.z_subcell);
        EXPECT_EQ(actual.acknowledged_input_sequence, expected.acknowledged_input_sequence);
        EXPECT_EQ(actual.state_revision, expected.state_revision);
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
    expectRoundTrip(shared::ClientInputMessage{ .direction = { 129, 127, 1 }, .sequence = 0x7856'3412U });
    expectRoundTrip(shared::ServerPlayerPositionMessage{
        .ch = '#', .x = 30, .y = 2, .z = 12, .x_subcell = 9'999, .y_subcell = 500, .z_subcell = 1,
        .acknowledged_input_sequence = 0x7856'3412U, .state_revision = 0x1234'5678U,
    });
    expectRoundTrip(shared::ServerRemovePlayerMessage{ .ch = '$' });
}

TEST(MessageTest, RejectsTruncatedAndUnknownPayloads) {
    std::array<shared::Message, 5> const messages{
        shared::JoinRequestMessage{ .ch = '@' },
        shared::JoinResponseMessage{ .accepted = true },
        shared::ClientInputMessage{ .direction = { 129, 127, 1 }, .sequence = 0x7856'3412U },
        shared::ServerPlayerPositionMessage{
            .ch = '#', .x = 30, .y = 2, .z = 12, .x_subcell = 9'999, .y_subcell = 500, .z_subcell = 1,
            .acknowledged_input_sequence = 0x7856'3412U, .state_revision = 0x1234'5678U,
        },
        shared::ServerRemovePlayerMessage{ .ch = '$' },
    };
    for (auto const& message : messages) {
        auto const bytes = shared::encodeMessage(message);
        for (uint64_t size = 0; size < bytes.size(); ++size) {
            EXPECT_FALSE(shared::decodeMessage(std::span{ bytes.data(), size }).has_value());
        }
    }
    std::array<uint8_t, 4> unknown{ shared::PROTOCOL_MAGIC, shared::PROTOCOL_VERSION, 255, 0 };
    EXPECT_FALSE(shared::decodeMessage(unknown).has_value());
}

TEST(MessageTest, UsesVersionedLittleEndianFixedWidthPayloads) {
    EXPECT_EQ(
        shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' }),
        (std::vector<uint8_t>{
            0x4D, 3, 0, '@', 0,
            1, 0, 0, 0,
            42, 0, 0, 0, 0, 0, 0, 0,
            16, 16, 16,
            101, 252, 205, 108, 74, 88, 174, 176, 0,
        })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::JoinResponseMessage{ .accepted = true }),
        (std::vector<uint8_t>{ 0x4D, 3, 1, 1 })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::JoinResponseMessage{ .accepted = false }),
        (std::vector<uint8_t>{ 0x4D, 3, 1, 0 })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::ClientInputMessage{ .direction = { 129, 127, 1 }, .sequence = 0x7856'3412U }),
        (std::vector<uint8_t>{ 0x4D, 3, 2, 129, 127, 1, 0, 18, 52, 86, 120 })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::ServerPlayerPositionMessage{
            .ch = '#', .x = 30, .y = 2, .z = 12, .x_subcell = 9'999, .y_subcell = 500, .z_subcell = 1,
            .acknowledged_input_sequence = 0x7856'3412U, .state_revision = 0x1234'5678U,
        }),
        (std::vector<uint8_t>{
            0x4D, 3, 3, '#',
            30, 0, 0, 0,
            2, 0, 0, 0,
            12, 0, 0, 0,
            15, 39, 244, 1, 1, 0,
            18, 52, 86, 120, 120, 86, 52, 18,
        })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::ServerRemovePlayerMessage{ .ch = '$' }),
        (std::vector<uint8_t>{ 0x4D, 3, 4, '$' })
    );
}

TEST(MessageTest, RejectsNoncanonicalAcceptanceByte) {
    for (uint16_t value = 2; value <= 255; ++value) {
        std::array<uint8_t, 4> const bytes{ 0x4D, 3, 1, static_cast<uint8_t>(value) };
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
    auto input = shared::encodeMessage(shared::ClientInputMessage{ .direction = { 127, 127 }, .sequence = 1U });
    input[3] = 128;
    EXPECT_FALSE(shared::decodeMessage(input).has_value());

    auto position = shared::encodeMessage(shared::ServerPlayerPositionMessage{
        .ch = '@', .x = 1, .y = 1, .x_subcell = 0, .y_subcell = 0,
        .acknowledged_input_sequence = 0U, .state_revision = 1U,
    });
    position[4] = 0;
    position[5] = 0;
    position[6] = 1;
    position[7] = 0;
    EXPECT_FALSE(shared::decodeMessage(position).has_value());

    position = shared::encodeMessage(shared::ServerPlayerPositionMessage{
        .ch = '@', .x = 1, .y = 1, .x_subcell = 0, .y_subcell = 0,
        .acknowledged_input_sequence = 0U, .state_revision = 1U,
    });
    position[16] = 16;
    position[17] = 39;
    EXPECT_FALSE(shared::decodeMessage(position).has_value());

    auto join = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '\n' });
    EXPECT_FALSE(shared::decodeMessage(join).has_value());
}

TEST(MessageTest, RejectsOldAndMixedProtocolVersions) {
    std::array<uint8_t, 2> const old_packet{ 0, '@' };
    EXPECT_FALSE(shared::decodeMessage(old_packet).has_value());

    auto packet = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' });
    packet[0] = 0x4C;
    EXPECT_FALSE(shared::decodeMessage(packet).has_value());

    packet = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' });
    packet[1] = 1;
    EXPECT_FALSE(shared::decodeMessage(packet).has_value());

    packet = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' });
    packet[4] = 2;
    EXPECT_FALSE(shared::decodeMessage(packet).has_value());
}

TEST(MessageTest, RejectsInvalidWorldConfiguration) {
    auto packet = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' });
    packet[5] = 0;
    packet[6] = 0;
    packet[7] = 0;
    packet[8] = 0;
    EXPECT_FALSE(shared::decodeMessage(packet).has_value());

    packet = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' });
    packet[17] = 0;
    EXPECT_FALSE(shared::decodeMessage(packet).has_value());
}

TEST(MessageTest, OrdersSequencesAcrossWrapButRejectsTheHalfRangeTie)
{
    EXPECT_TRUE(shared::isNewerSequence(1U, 0U));
    EXPECT_TRUE(shared::isNewerSequence(0U, UINT32_MAX));
    EXPECT_FALSE(shared::isNewerSequence(0U, 0U));
    EXPECT_FALSE(shared::isNewerSequence(0x8000'0000U, 0U));
    EXPECT_FALSE(shared::isNewerSequence(0U, 0x8000'0000U));
}
