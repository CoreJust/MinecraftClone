#include <shared/net/Message.hpp>
#include <shared/world/World.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

shared::ServerHeightTileMessage heightTileMessage()
{
    shared::ServerHeightTileMessage message{
        .key = {.x = 17, .y = 23},
        .revision = 7U,
        .token = 11U,
    };
    for (uint32_t index{ 0U }; index < shared::HEIGHT_TILE_SAMPLE_COUNT; ++index) {
        message.heights[index] = static_cast<uint16_t>(index);
    }
    return message;
}

shared::ServerHeightTileBatchMessage heightTileBatchMessage()
{
    shared::ServerHeightTileMessage const first = heightTileMessage();
    shared::ServerHeightTileMessage second = heightTileMessage();
    second.key = { .x = 18, .y = 23 };
    second.token = 12U;
    return {
        .delivery_token = 19U,
        .tiles = { first, second },
        .removals = {},
    };
}

template<typename MessageTy>
void expectRoundTrip(MessageTy const& expected) {
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
        EXPECT_EQ(actual.direction.accelerated, expected.direction.accelerated);
        EXPECT_EQ(actual.direction.speedup, expected.direction.speedup);
        EXPECT_EQ(
            actual.direction.cycle_movement_capabilities,
            expected.direction.cycle_movement_capabilities
        );
        EXPECT_EQ(actual.direction.view_x, expected.direction.view_x);
        EXPECT_EQ(actual.direction.view_y, expected.direction.view_y);
        EXPECT_EQ(actual.sequence, expected.sequence);
    } else if constexpr (std::is_same_v<MessageTy, shared::ClientHeightTileCreditMessage>) {
        EXPECT_EQ(actual.world_revision, expected.world_revision);
        EXPECT_EQ(actual.delivery_token, expected.delivery_token);
        EXPECT_EQ(actual.credits, expected.credits);
        EXPECT_EQ(actual.not_resident_keys, expected.not_resident_keys);
    } else if constexpr (std::is_same_v<MessageTy, shared::ServerPlayerPositionMessage>) {
        EXPECT_EQ(actual.ch, expected.ch);
        EXPECT_EQ(actual.palette_index, expected.palette_index);
        EXPECT_EQ(actual.movement_capabilities, expected.movement_capabilities);
        EXPECT_EQ(actual.x, expected.x);
        EXPECT_EQ(actual.y, expected.y);
        EXPECT_EQ(actual.z, expected.z);
        EXPECT_EQ(actual.x_subcell, expected.x_subcell);
        EXPECT_EQ(actual.y_subcell, expected.y_subcell);
        EXPECT_EQ(actual.z_subcell, expected.z_subcell);
        EXPECT_EQ(actual.vertical_velocity_subcells, expected.vertical_velocity_subcells);
        EXPECT_EQ(actual.acknowledged_input_sequence, expected.acknowledged_input_sequence);
        EXPECT_EQ(actual.state_revision, expected.state_revision);
    } else if constexpr (std::is_same_v<MessageTy, shared::ServerRemovePlayerMessage>) {
        EXPECT_EQ(actual.ch, expected.ch);
    } else if constexpr (std::is_same_v<MessageTy, shared::ServerHeightTileDescriptorMessage>) {
        EXPECT_EQ(actual.configuration, expected.configuration);
        EXPECT_EQ(actual.world_revision, expected.world_revision);
        EXPECT_EQ(actual.max_height_tiles, expected.max_height_tiles);
        EXPECT_EQ(actual.max_height_tile_bytes, expected.max_height_tile_bytes);
    } else if constexpr (std::is_same_v<MessageTy, shared::ServerWorldRevisionMessage>) {
        EXPECT_EQ(actual.world_revision, expected.world_revision);
    } else if constexpr (std::is_same_v<MessageTy, shared::ServerHeightTileBatchMessage>) {
        EXPECT_EQ(actual.delivery_token, expected.delivery_token);
        ASSERT_EQ(actual.tiles.size(), expected.tiles.size());
        for (uint32_t index = 0U; index < actual.tiles.size(); ++index) {
            EXPECT_EQ(actual.tiles[index].key, expected.tiles[index].key);
            EXPECT_EQ(actual.tiles[index].revision, expected.tiles[index].revision);
            EXPECT_EQ(actual.tiles[index].token, expected.tiles[index].token);
            EXPECT_EQ(actual.tiles[index].heights, expected.tiles[index].heights);
        }
        ASSERT_EQ(actual.removals.size(), expected.removals.size());
        for (uint32_t index = 0U; index < actual.removals.size(); ++index) {
            EXPECT_EQ(actual.removals[index].key, expected.removals[index].key);
            EXPECT_EQ(actual.removals[index].revision, expected.removals[index].revision);
            EXPECT_EQ(actual.removals[index].token, expected.removals[index].token);
        }
    } else {
        EXPECT_EQ(actual.key, expected.key);
        EXPECT_EQ(actual.revision, expected.revision);
        EXPECT_EQ(actual.token, expected.token);
        if constexpr (std::is_same_v<MessageTy, shared::ServerHeightTileMessage>) {
            EXPECT_EQ(actual.heights, expected.heights);
        }
    }
    EXPECT_EQ(shared::encodeMessage(actual), bytes);
}

} // namespace

TEST(MessageTest, RoundTripsEveryMessageKind) {
    expectRoundTrip(shared::JoinRequestMessage{ .ch = '@' });
    expectRoundTrip(shared::JoinResponseMessage{ .accepted = true });
    expectRoundTrip(shared::JoinResponseMessage{ .accepted = false });
    expectRoundTrip(shared::ClientInputMessage{
        .direction = { .x = 129U, .y = 127U, .z = 1U, .cycle_movement_capabilities = true },
        .sequence = 0x7856'3412U,
    });
    expectRoundTrip(shared::ClientHeightTileCreditMessage{
        .world_revision = 7U,
        .delivery_token = 11U,
        .credits = 1U,
        .not_resident_keys = {{ .x = 17, .y = 23 }, { .x = 19, .y = 29 }},
    });
    expectRoundTrip(shared::ServerPlayerPositionMessage{
        .ch = '#', .palette_index = 7U, .movement_capabilities = { .bits = 3U },
        .x = 30, .y = 2, .z = 12, .x_subcell = 9'999, .y_subcell = 500, .z_subcell = 1,
        .vertical_velocity_subcells = 5'250,
        .acknowledged_input_sequence = 0x7856'3412U, .state_revision = 0x1234'5678U,
    });
    expectRoundTrip(shared::ServerRemovePlayerMessage{ .ch = '$' });
    expectRoundTrip(shared::ServerHeightTileDescriptorMessage{});
    expectRoundTrip(shared::ServerHeightTileDescriptorMessage{
        .max_height_tiles = shared::heightTileInterestCount(128U),
    });
    expectRoundTrip(shared::ServerHeightTileDescriptorMessage{
        .max_height_tiles = shared::heightTileInterestCount(256U),
    });
    expectRoundTrip(shared::ServerWorldRevisionMessage{});
    expectRoundTrip(heightTileMessage());
    expectRoundTrip(heightTileBatchMessage());
    expectRoundTrip(shared::ServerRemoveHeightTileMessage{
        .key = {.x = 17, .y = 23},
        .revision = 7U,
        .token = 12U,
    });
}

TEST(MessageTest, RejectsTruncatedAndUnknownPayloads) {
    std::array<shared::Message, 11> const messages{
        shared::JoinRequestMessage{ .ch = '@' },
        shared::JoinResponseMessage{ .accepted = true },
        shared::ClientInputMessage{ .direction = { 129, 127, 1 }, .sequence = 0x7856'3412U },
        shared::ClientHeightTileCreditMessage{
            .world_revision = 7U,
            .delivery_token = 11U,
            .credits = 1U,
            .not_resident_keys = {},
        },
        shared::ServerPlayerPositionMessage{
            .ch = '#', .palette_index = 7U, .x = 30, .y = 2, .z = 12, .x_subcell = 9'999, .y_subcell = 500, .z_subcell = 1,
            .acknowledged_input_sequence = 0x7856'3412U, .state_revision = 0x1234'5678U,
        },
        shared::ServerRemovePlayerMessage{ .ch = '$' },
        shared::ServerHeightTileDescriptorMessage{},
        shared::ServerWorldRevisionMessage{},
        heightTileMessage(),
        heightTileBatchMessage(),
        shared::ServerRemoveHeightTileMessage{
            .key = {.x = 17, .y = 23},
            .revision = 7U,
            .token = 12U,
        },
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

TEST(MessageTest, RejectsTruncatedCreditAcknowledgementToken)
{
    std::vector<uint8_t> bytes = shared::encodeMessage(shared::ClientHeightTileCreditMessage{
        .world_revision = 7U,
        .delivery_token = 0x1234'5678'9ABC'DEF0U,
        .credits = 1U,
        .not_resident_keys = {},
    });
    bytes.erase(bytes.end() - 5, bytes.end() - 1);

    EXPECT_FALSE(shared::decodeMessage(bytes).has_value());
}

TEST(MessageTest, BoundsAndValidatesTileResidencyReceipts)
{
    static constexpr uint8_t RECEIPT_CAPACITY = shared::HEIGHT_TILE_RESIDENCY_RECEIPT_KEY_CAPACITY;
    std::vector<shared::HeightTileKey> keys;
    keys.reserve(RECEIPT_CAPACITY);
    for (uint8_t index = 0U; index < RECEIPT_CAPACITY; ++index) {
        keys.push_back({ .x = index, .y = 0 });
    }
    shared::ClientHeightTileCreditMessage const maximum_receipt{
        .world_revision = 1U,
        .delivery_token = 1U,
        .credits = 1U,
        .not_resident_keys = keys,
    };
    std::vector<uint8_t> const maximum_bytes = shared::encodeMessage(maximum_receipt);
    EXPECT_EQ(maximum_bytes.size(), 3U + 18U + 8U * RECEIPT_CAPACITY);
    EXPECT_TRUE(shared::decodeMessage(maximum_bytes).has_value());

    keys.push_back({ .x = 100, .y = 0 });
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
        .world_revision = 1U,
        .delivery_token = 1U,
        .credits = 1U,
        .not_resident_keys = keys,
    })).has_value());

    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
        .world_revision = 1U,
        .delivery_token = 1U,
        .credits = 1U,
        .not_resident_keys = {{ .x = 1, .y = 2 }, { .x = 1, .y = 2 }},
    })).has_value());
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
        .world_revision = 1U,
        .delivery_token = 1U,
        .credits = 1U,
        .not_resident_keys = {{ .x = 4'096, .y = 0 }},
    })).has_value());
    std::vector<uint8_t> truncated_receipt = shared::encodeMessage(shared::ClientHeightTileCreditMessage{
        .world_revision = 1U,
        .delivery_token = 1U,
        .credits = 1U,
        .not_resident_keys = {{ .x = 1, .y = 2 }},
    });
    truncated_receipt.pop_back();
    EXPECT_FALSE(shared::decodeMessage(truncated_receipt).has_value());
}

TEST(MessageTest, UsesVersionedLittleEndianFixedWidthPayloads) {
    EXPECT_EQ(
        shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' }),
        (std::vector<uint8_t>{
            0x4D, 14, 0, '@', 0,
            1, 0, 0, 0,
            42, 0, 0, 0, 0, 0, 0, 0,
            16, 16, 16,
            101, 252, 205, 108, 74, 88, 174, 176, 0,
        })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::JoinResponseMessage{ .accepted = true }),
        (std::vector<uint8_t>{ 0x4D, 14, 1, 1 })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::JoinResponseMessage{ .accepted = false }),
        (std::vector<uint8_t>{ 0x4D, 14, 1, 0 })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::ClientInputMessage{ .direction = { 129, 127, 1 }, .sequence = 0x7856'3412U }),
        (std::vector<uint8_t>{ 0x4D, 14, 2, 129, 127, 1, 0, 5, 0, 0, 0, 127, 18, 52, 86, 120 })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::ServerPlayerPositionMessage{
            .ch = '#', .palette_index = 7U, .x = 30, .y = 2, .z = 12, .x_subcell = 9'999, .y_subcell = 500, .z_subcell = 1,
            .vertical_velocity_subcells = 5'250,
            .acknowledged_input_sequence = 0x7856'3412U, .state_revision = 0x1234'5678U,
        }),
        (std::vector<uint8_t>{
            0x4D, 14, 4, '#', 7, 0,
            30, 0, 0, 0,
            2, 0, 0, 0,
            12, 0, 0, 0,
            15, 39, 244, 1, 1, 0,
            0x82, 0x14, 0, 0,
            18, 52, 86, 120, 120, 86, 52, 18,
        })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::ServerRemovePlayerMessage{ .ch = '$' }),
        (std::vector<uint8_t>{ 0x4D, 14, 5, '$' })
    );
    auto const height_tile = shared::encodeMessage(heightTileMessage());
    EXPECT_EQ(height_tile.size(), 3U + 24U + 259U);
    EXPECT_EQ(height_tile[0], shared::PROTOCOL_MAGIC);
    EXPECT_EQ(height_tile[1], shared::PROTOCOL_VERSION);
    EXPECT_EQ(height_tile[2], 8U);
    auto const height_tile_batch = shared::encodeMessage(heightTileBatchMessage());
    EXPECT_EQ(
        height_tile_batch.size(),
        3U + 10U + 2U * (24U + 259U)
    );
    EXPECT_EQ(height_tile_batch[2], 10U);
}

TEST(MessageTest, EncodesHeightTilesWithCanonicalLosslessBitPacking)
{
    shared::ServerHeightTileMessage constant{
        .key = { .x = 17, .y = 23 },
        .revision = 7U,
        .token = 11U,
    };
    constant.heights.fill(1024U);
    std::vector<uint8_t> const constant_fixture{
        0x4D, 14, 8,
        17, 0, 0, 0, 23, 0, 0, 0,
        7, 0, 0, 0, 0, 0, 0, 0,
        11, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 4,
    };
    EXPECT_EQ(shared::encodeMessage(constant), constant_fixture);
    EXPECT_EQ(shared::encodeMessage(constant).size(), 30U);
    EXPECT_EQ(shared::encodedHeightTileSize(constant), 27U);
    auto const constant_decoded = shared::decodeMessage(constant_fixture);
    ASSERT_TRUE(constant_decoded.has_value());
    ASSERT_TRUE(std::holds_alternative<shared::ServerHeightTileMessage>(*constant_decoded));
    EXPECT_EQ(std::get<shared::ServerHeightTileMessage>(*constant_decoded).heights, constant.heights);

    shared::ServerHeightTileMessage alternating{
        .key = { .x = 17, .y = 23 },
        .revision = 7U,
        .token = 11U,
    };
    for (uint32_t index = 0U; index < shared::HEIGHT_TILE_SAMPLE_COUNT; ++index) {
        alternating.heights[index] = static_cast<uint16_t>(200U + (index & 1U));
    }
    std::vector<uint8_t> alternating_fixture{
        0x4D, 14, 8,
        17, 0, 0, 0, 23, 0, 0, 0,
        7, 0, 0, 0, 0, 0, 0, 0,
        11, 0, 0, 0, 0, 0, 0, 0,
        1, 200, 0,
    };
    alternating_fixture.insert(alternating_fixture.end(), 32U, 0xAAU);
    EXPECT_EQ(shared::encodeMessage(alternating), alternating_fixture);
    EXPECT_EQ(shared::encodeMessage(alternating).size(), 62U);
    EXPECT_EQ(shared::encodedHeightTileSize(alternating), 59U);
    auto const alternating_decoded = shared::decodeMessage(alternating_fixture);
    ASSERT_TRUE(alternating_decoded.has_value());
    ASSERT_TRUE(std::holds_alternative<shared::ServerHeightTileMessage>(*alternating_decoded));
    EXPECT_EQ(std::get<shared::ServerHeightTileMessage>(*alternating_decoded).heights, alternating.heights);

    shared::ServerHeightTileMessage valid_range{
        .key = { .x = 17, .y = 23 },
        .revision = 7U,
        .token = 11U,
    };
    for (uint32_t index = 0U; index < shared::HEIGHT_TILE_SAMPLE_COUNT; ++index) {
        valid_range.heights[index] = static_cast<uint16_t>(index * 4U);
    }
    valid_range.heights.back() = 1024U;
    auto const valid_range_packet = shared::encodeMessage(valid_range);
    EXPECT_EQ(valid_range_packet.size(), 382U);
    EXPECT_EQ(shared::encodedHeightTileSize(valid_range), 379U);
    auto const valid_range_decoded = shared::decodeMessage(valid_range_packet);
    ASSERT_TRUE(valid_range_decoded.has_value());
    ASSERT_TRUE(std::holds_alternative<shared::ServerHeightTileMessage>(*valid_range_decoded));
    EXPECT_EQ(std::get<shared::ServerHeightTileMessage>(*valid_range_decoded).heights, valid_range.heights);

    expectRoundTrip(heightTileMessage());
}

TEST(MessageTest, RejectsMalformedHeightTileEncodings)
{
    shared::ServerHeightTileMessage alternating{
        .key = { .x = 17, .y = 23 },
        .revision = 7U,
        .token = 11U,
    };
    for (uint32_t index = 0U; index < shared::HEIGHT_TILE_SAMPLE_COUNT; ++index) {
        alternating.heights[index] = static_cast<uint16_t>(200U + (index & 1U));
    }
    std::vector<uint8_t> const valid_packet = shared::encodeMessage(alternating);

    auto invalid_width = valid_packet;
    invalid_width[27] = 17U;
    EXPECT_FALSE(shared::decodeMessage(invalid_width).has_value());

    auto truncated = valid_packet;
    truncated.pop_back();
    EXPECT_FALSE(shared::decodeMessage(truncated).has_value());

    auto overflowing_base = valid_packet;
    overflowing_base[28] = 0xFFU;
    overflowing_base[29] = 0xFFU;
    EXPECT_FALSE(shared::decodeMessage(overflowing_base).has_value());

    auto trailing = valid_packet;
    trailing.push_back(0U);
    EXPECT_FALSE(shared::decodeMessage(trailing).has_value());
}

TEST(MessageTest, RejectsNoncanonicalAcceptanceByte) {
    for (uint16_t value = 2; value <= 255; ++value) {
        std::array<uint8_t, 4> const bytes{ 0x4D, 6, 1, static_cast<uint8_t>(value) };
        EXPECT_FALSE(shared::decodeMessage(bytes).has_value());
    }
}

TEST(MessageTest, RejectsTrailingBytes) {
    auto bytes = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' });
    bytes.push_back(0);
    auto const decoded = shared::decodeMessage(std::span<uint8_t const>{ bytes.data(), bytes.size() });
    EXPECT_FALSE(decoded.has_value());
}

TEST(MessageTest, RejectsEmptyOversizedMalformedAndTrailingHeightTileBatches)
{
    shared::ServerHeightTileBatchMessage empty;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(empty)).has_value());

    shared::ServerHeightTileBatchMessage oversized;
    oversized.tiles.resize(shared::HEIGHT_TILE_BATCH_CAPACITY + 1U, heightTileMessage());
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(oversized)).has_value());

    auto malformed = heightTileBatchMessage();
    malformed.tiles[1].heights[0] = std::numeric_limits<uint16_t>::max();
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(malformed)).has_value());

    auto trailing = shared::encodeMessage(heightTileBatchMessage());
    trailing.push_back(0U);
    EXPECT_FALSE(shared::decodeMessage(trailing).has_value());
}

TEST(MessageTest, RejectsInvalidPayloadValues) {
    auto input = shared::encodeMessage(shared::ClientInputMessage{ .direction = { 127, 127 }, .sequence = 1U });
    input[3] = 128;
    EXPECT_FALSE(shared::decodeMessage(input).has_value());

    input = shared::encodeMessage(shared::ClientInputMessage{
        .direction = { .x = 127, .y = 127, .accelerated = true, .speedup = 5U },
        .sequence = 1U,
    });
    input[7] = 4U;
    EXPECT_FALSE(shared::decodeMessage(input).has_value());

    auto position = shared::encodeMessage(shared::ServerPlayerPositionMessage{
        .ch = '@', .x = 1, .y = 1, .x_subcell = 0, .y_subcell = 0,
        .acknowledged_input_sequence = 0U, .state_revision = 1U,
    });
    position[4] = shared::PLAYER_PALETTE_COUNT;
    EXPECT_FALSE(shared::decodeMessage(position).has_value());

    position = shared::encodeMessage(shared::ServerPlayerPositionMessage{
        .ch = '@', .x = 1, .y = 1, .x_subcell = 0, .y_subcell = 0,
        .acknowledged_input_sequence = 0U, .state_revision = 1U,
    });
    position[17] = 16;
    position[18] = 39;
    EXPECT_FALSE(shared::decodeMessage(position).has_value());

    auto join = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '\n' });
    EXPECT_FALSE(shared::decodeMessage(join).has_value());

    auto invalid_tile = heightTileMessage();
    invalid_tile.heights.fill(1025U);
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(invalid_tile)).has_value());
}

TEST(MessageTest, RejectsOldAndMixedProtocolVersions) {
    std::array<uint8_t, 2> const old_packet{ 0, '@' };
    EXPECT_FALSE(shared::decodeMessage(old_packet).has_value());

    auto packet = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' });
    packet[0] = 0x4C;
    EXPECT_FALSE(shared::decodeMessage(packet).has_value());

    packet = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' });
    packet[1] = 11U;
    EXPECT_FALSE(shared::decodeMessage(packet).has_value());

    packet[1] = 10U;
    EXPECT_FALSE(shared::decodeMessage(packet).has_value());

    packet = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' });
    packet[4] = 2;
    EXPECT_FALSE(shared::decodeMessage(packet).has_value());
}

TEST(MessageTest, MapsExactDiskCardinalitiesToEverySupportedRadius)
{
    static constexpr uint32_t MINIMUM_RADIUS = 1U;
    static constexpr uint32_t MAXIMUM_RADIUS = 256U;
    static constexpr uint32_t DEFAULT_COUNT = shared::HEIGHT_TILE_INTEREST_COUNT;
    static constexpr std::array<uint32_t, 4> INVALID_COUNTS{
        0U,
        DEFAULT_COUNT - 1U,
        DEFAULT_COUNT + 1U,
        shared::heightTileInterestCount(257U),
    };

    EXPECT_EQ(shared::PROTOCOL_VERSION, 14U);
    EXPECT_EQ(shared::HEIGHT_TILE_BATCH_CAPACITY, 64U);
    EXPECT_EQ(shared::HEIGHT_TILE_RESIDENCY_RECEIPT_KEY_CAPACITY, 128U);
    EXPECT_EQ(shared::encodedHeightTileSize(heightTileMessage()), 283U);
    EXPECT_FALSE(shared::isValidHeightTileInterestRadius(0U));
    EXPECT_FALSE(shared::isValidHeightTileInterestRadius(257U));
    for (uint32_t radius = MINIMUM_RADIUS; radius <= MAXIMUM_RADIUS; ++radius) {
        ASSERT_TRUE(shared::isValidHeightTileInterestRadius(radius));
        auto const count = shared::heightTileInterestCount(radius);
        ASSERT_EQ(shared::heightTileInterestRadiusForCount(count), std::optional{radius});
        if (radius > MINIMUM_RADIUS) {
            EXPECT_GT(count, shared::heightTileInterestCount(radius - 1U));
        }
    }
    for (uint32_t const count : INVALID_COUNTS) {
        EXPECT_EQ(shared::heightTileInterestRadiusForCount(count), std::nullopt);
    }
}

TEST(MessageTest, RejectsMalformedHeightTileDescriptorCardinalities)
{
    static constexpr uint32_t DEFAULT_COUNT = shared::HEIGHT_TILE_INTEREST_COUNT;
    static constexpr std::array<uint32_t, 4> INVALID_COUNTS{
        0U,
        DEFAULT_COUNT - 1U,
        DEFAULT_COUNT + 1U,
        shared::heightTileInterestCount(257U),
    };

    for (uint32_t const count : INVALID_COUNTS) {
        auto const packet = shared::encodeMessage(shared::ServerHeightTileDescriptorMessage{
            .max_height_tiles = count,
        });
        EXPECT_FALSE(shared::decodeMessage(packet).has_value()) << "count " << count;
    }
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
