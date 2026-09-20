#include <server/GameServer.hpp>

#include <shared/net/Message.hpp>
#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/World.hpp>

#include <core/net/Client.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <span>
#include <thread>
#include <vector>

namespace {

class PreviewClient final : public core::Client {
public:
    explicit PreviewClient(
        bool const acknowledges_deliveries = true,
        bool const starts_deliveries = true
    )
        : core::Client{2}
        , m_acknowledges_deliveries{acknowledges_deliveries}
        , m_starts_deliveries{starts_deliveries}
    { }

    std::vector<shared::Message> messages;
    uint32_t last_acknowledged_input = 0U;
    int32_t last_player_x = 0;

private:
    void onDisconnected(core::DisconnectEvent const) override { }

    void onReceived(core::ReceiveEvent event) override
    {
        auto const message = shared::decodeMessage(event.data);
        if (message) {
            messages.push_back(*message);
            if (auto const* const descriptor = std::get_if<shared::ServerHeightTileDescriptorMessage>(&*message);
                descriptor != nullptr && m_starts_deliveries) {
                static_cast<void>(send(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
                    .world_revision = descriptor->world_revision,
                    .delivery_token = 0U,
                    .credits = shared::HEIGHT_TILE_DELIVERY_WINDOW,
                }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
            } else if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&*message);
                batch != nullptr && m_acknowledges_deliveries) {
                static_cast<void>(send(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
                    .world_revision = 1U,
                    .delivery_token = batch->delivery_token,
                    .credits = 1U,
                }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
            }
            if (auto const* position = std::get_if<shared::ServerPlayerPositionMessage>(&*message);
                position != nullptr && position->ch == '@') {
                last_acknowledged_input = position->acknowledged_input_sequence;
                last_player_x = position->x;
            }
        }
    }

    bool m_acknowledges_deliveries;
    bool m_starts_deliveries;
};

uint32_t heightTileCount(std::vector<shared::Message> const& messages)
{
    uint32_t count = 0U;
    for (shared::Message const& message : messages) {
        if (std::holds_alternative<shared::ServerHeightTileMessage>(message)) {
            ++count;
        } else if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&message)) {
            count += static_cast<uint32_t>(batch->tiles.size());
        }
    }
    return count;
}

uint32_t removalCount(std::span<shared::Message const> const messages)
{
    uint32_t count = 0U;
    for (shared::Message const& message : messages) {
        if (std::holds_alternative<shared::ServerRemoveHeightTileMessage>(message)) {
            ++count;
        } else if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&message)) {
            count += static_cast<uint32_t>(batch->removals.size());
        }
    }
    return count;
}

uint32_t deliveryBatchCount(std::vector<shared::Message> const& messages)
{
    return static_cast<uint32_t>(std::ranges::count_if(messages, [](shared::Message const& message) {
        return std::holds_alternative<shared::ServerHeightTileBatchMessage>(message);
    }));
}

std::vector<shared::HeightTileKey> heightTileKeys(std::vector<shared::Message> const& messages)
{
    std::vector<shared::HeightTileKey> keys;
    for (shared::Message const& message : messages) {
        if (auto const* const tile = std::get_if<shared::ServerHeightTileMessage>(&message)) {
            keys.push_back(tile->key);
        } else if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&message)) {
            for (shared::ServerHeightTileMessage const& batch_tile : batch->tiles) {
                keys.push_back(batch_tile.key);
            }
        }
    }
    return keys;
}

std::vector<shared::HeightTileKey> heightTileKeysFrom(
    std::vector<shared::Message> const& messages,
    size_t const first_message
)
{
    std::vector<shared::Message> suffix;
    suffix.reserve(messages.size() - std::min(first_message, messages.size()));
    for (size_t index = std::min(first_message, messages.size()); index < messages.size(); ++index) {
        suffix.push_back(messages[index]);
    }
    return heightTileKeys(suffix);
}

} // namespace

TEST(GameServerPreviewTest, StreamsNearestTilesBeforeRowMajorInterestOrder)
{
    static constexpr std::chrono::seconds TIMEOUT{4};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t OBSERVED_TILE_COUNT = 16U;
    static constexpr int32_t MAXIMUM_NEARBY_DISTANCE = 7;

    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] {
        server.run(stop_requested);
    }};
    PreviewClient client;
    bool const connected = client.connect(core::Address::localhost(server.port()), TIMEOUT);
    bool const joined = connected && client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable});
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (joined && heightTileCount(client.messages) < OBSERVED_TILE_COUNT
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    std::vector<shared::HeightTileKey> const keys = heightTileKeys(client.messages);
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(joined);
    ASSERT_GE(keys.size(), OBSERVED_TILE_COUNT);
    std::vector<int32_t> rows;
    int32_t const center_x = shared::World::FLIGHT_SPAWN.x
        / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
    int32_t const center_y = shared::World::FLIGHT_SPAWN.y
        / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
    for (uint32_t index{0U}; index < OBSERVED_TILE_COUNT; ++index) {
        shared::HeightTileKey const key = keys[index];
        int32_t const distance = std::max(std::abs(key.x - center_x), std::abs(key.y - center_y));
        EXPECT_LE(distance, MAXIMUM_NEARBY_DISTANCE);
        if (std::ranges::find(rows, key.y) == rows.end()) {
            rows.push_back(key.y);
        }
    }
    EXPECT_GT(rows.size(), 1U);
}

TEST(GameServerPreviewTest, ProgressiveStreamFormsAForwardBiasedArea)
{
    static constexpr std::chrono::seconds TIMEOUT{5};
    static constexpr uint32_t OBSERVED_TILE_COUNT = 1'024U;
    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] { server.run(stop_requested); }};
    PreviewClient client;
    bool const connected = client.connect(core::Address::localhost(server.port()), TIMEOUT);
    bool const joined = connected && client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable});
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (joined && heightTileCount(client.messages) < OBSERVED_TILE_COUNT
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    std::vector<shared::HeightTileKey> const keys = heightTileKeys(client.messages);
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(joined);
    ASSERT_GE(keys.size(), OBSERVED_TILE_COUNT);
    int32_t const center_x = shared::World::FLIGHT_SPAWN.x
        / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
    int32_t const center_y = shared::World::FLIGHT_SPAWN.y
        / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
    int32_t forward_extent = 0;
    int32_t rear_extent = 0;
    int32_t side_extent = 0;
    std::vector<int32_t> rows;
    for (uint32_t index = 0U; index < OBSERVED_TILE_COUNT; ++index) {
        shared::HeightTileKey const key = keys[index];
        forward_extent = std::max(forward_extent, key.y - center_y);
        rear_extent = std::max(rear_extent, center_y - key.y);
        side_extent = std::max(side_extent, std::abs(key.x - center_x));
        if (std::ranges::find(rows, key.y) == rows.end()) {
            rows.push_back(key.y);
        }
    }
    EXPECT_GT(forward_extent, side_extent);
    EXPECT_GT(forward_extent, rear_extent);
    EXPECT_GT(rows.size(), 20U);
}

TEST(GameServerPreviewTest, CameraRotationReprioritizesWithoutAnyRemoval)
{
    static constexpr std::chrono::seconds TIMEOUT{10};
    static constexpr uint32_t OBSERVED_TILE_COUNT = 3'000U;
    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] { server.run(stop_requested); }};
    PreviewClient client;
    bool const connected = client.connect(core::Address::localhost(server.port()), TIMEOUT);
    bool const joined = connected && client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable});
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (joined && heightTileCount(client.messages) < OBSERVED_TILE_COUNT
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    size_t const before_rotation = client.messages.size();
    bool const rotated = client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {.view_x = 127, .view_y = 0},
        .sequence = 1U,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable});
    auto const rotation_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (rotated && std::chrono::steady_clock::now() < rotation_deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    std::span<shared::Message const> const suffix = std::span{client.messages}.subspan(before_rotation);
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(joined);
    ASSERT_TRUE(rotated);
    ASSERT_GE(heightTileCount(client.messages), OBSERVED_TILE_COUNT);
    EXPECT_EQ(removalCount(suffix), 0U);
}

TEST(GameServerPreviewTest, StreamsPlayerCenteredHeightTilesAndRemovesDepartedTiles)
{
    static constexpr std::chrono::seconds TIMEOUT{20};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t INPUT_COUNT = 32U;
    uint32_t const expected_height_tiles = static_cast<uint32_t>(shared::makeHeightTileInterest(
        {
            .x = shared::World::FLIGHT_SPAWN.x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
            .y = shared::World::FLIGHT_SPAWN.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        },
        0,
        127
    ).keys.size());

    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] {
        server.run(stop_requested);
    }};
    PreviewClient client;
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable}));

    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (heightTileCount(client.messages) < expected_height_tiles
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }

    bool const received_all_height_tiles = heightTileCount(client.messages) == expected_height_tiles;
    auto const validateHeightTile = [](shared::ServerHeightTileMessage const& height_tile) {
        EXPECT_EQ(height_tile.heights.size(), shared::HEIGHT_TILE_SAMPLE_COUNT);
        EXPECT_NE(height_tile.token, 0U);
        EXPECT_EQ(height_tile.revision, 1U);
    };
    uint32_t descriptor_count = 0U;
    for (shared::Message const& message : client.messages) {
        if (auto const* const descriptor = std::get_if<shared::ServerHeightTileDescriptorMessage>(&message)) {
            ++descriptor_count;
            EXPECT_EQ(descriptor->max_height_tiles, shared::HEIGHT_TILE_INTEREST_COUNT);
            EXPECT_EQ(descriptor->max_height_tile_bytes, shared::HEIGHT_TILE_PAYLOAD_BYTES);
            continue;
        }
        if (auto const* const height_tile = std::get_if<shared::ServerHeightTileMessage>(&message)) {
            validateHeightTile(*height_tile);
        } else if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&message)) {
            for (shared::ServerHeightTileMessage const& tile : batch->tiles) {
                validateHeightTile(tile);
            }
        }
    }
    bool const received_one_descriptor = descriptor_count == 1U;

    bool sent_all_inputs = true;
    for (uint32_t sequence{1U}; sequence <= INPUT_COUNT; ++sequence) {
        sent_all_inputs = client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {.x = 127U},
            .sequence = sequence,
        }), 0, core::SendMode{core::SendMode::Reliable}) && sent_all_inputs;
    }
    auto const movement_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (removalCount(client.messages) == 0U
        && std::chrono::steady_clock::now() < movement_deadline) {
        client.poll(POLL_INTERVAL);
    }
    bool const received_removal = removalCount(client.messages) > 0U;
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    EXPECT_TRUE(received_all_height_tiles);
    EXPECT_TRUE(received_one_descriptor);
    EXPECT_TRUE(sent_all_inputs);
    EXPECT_TRUE(received_removal);
}

TEST(GameServerPreviewTest, FastFlightGeneratesFrontierTilesWithinDeadline)
{
    static constexpr std::chrono::seconds TIMEOUT{5};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t INPUT_COUNT = 4U;
    static constexpr uint32_t OBSERVED_TILE_COUNT = 16U;
    static constexpr int32_t MAXIMUM_FRONTIER_DISTANCE = 16;

    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] {
        server.run(stop_requested);
    }};
    PreviewClient client{true, false};
    bool const connected = client.connect(core::Address::localhost(server.port()), TIMEOUT);
    bool const joined = connected && client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable});
    auto const initial_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (joined && std::ranges::none_of(client.messages, [](shared::Message const& message) {
        return std::holds_alternative<shared::ServerHeightTileDescriptorMessage>(message);
    })
        && std::chrono::steady_clock::now() < initial_deadline) {
        client.poll(POLL_INTERVAL);
    }
    bool sent_all_inputs = true;
    for (uint32_t sequence{1U}; sequence <= INPUT_COUNT; ++sequence) {
        sent_all_inputs = client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {.x = 127U, .accelerated = true, .speedup = 500U},
            .sequence = sequence,
        }), 0, core::SendMode{core::SendMode::Reliable}) && sent_all_inputs;
    }
    auto const movement_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (joined && client.last_acknowledged_input < INPUT_COUNT
        && std::chrono::steady_clock::now() < movement_deadline) {
        client.poll(POLL_INTERVAL);
    }
    size_t const post_movement_message = client.messages.size();
    bool const started_deliveries = client.send(shared::encodeMessage(
        shared::ClientHeightTileCreditMessage{
            .world_revision = 1U,
            .delivery_token = 0U,
            .credits = shared::HEIGHT_TILE_DELIVERY_WINDOW,
        }
    ), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable});
    auto const player = shared::World::FLIGHT_SPAWN;
    shared::HeightTileKey const center{
        .x = (client.last_player_x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH)),
        .y = player.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
    };
    auto const is_frontier_key = [center](shared::HeightTileKey const key) {
        int32_t const x_distance = std::abs(key.x - center.x);
        int32_t const y_distance = std::abs(key.y - center.y);
        return std::max(x_distance, y_distance) <= MAXIMUM_FRONTIER_DISTANCE;
    };
    std::vector<shared::HeightTileKey> keys;
    while (joined && std::ranges::count_if(keys, is_frontier_key) < OBSERVED_TILE_COUNT
        && std::chrono::steady_clock::now() < movement_deadline) {
        client.poll(POLL_INTERVAL);
        keys = heightTileKeysFrom(client.messages, post_movement_message);
    }
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(joined);
    ASSERT_TRUE(sent_all_inputs);
    ASSERT_TRUE(started_deliveries);
    ASSERT_EQ(client.last_acknowledged_input, INPUT_COUNT);
    EXPECT_GE(std::ranges::count_if(keys, is_frontier_key), OBSERVED_TILE_COUNT);
}

TEST(GameServerPreviewTest, StalledClientCapsTerrainDeliveryAtTheAdvertisedWindow)
{
    static constexpr auto DURATION = std::chrono::seconds{2};
    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] { server.run(stop_requested); }};
    PreviewClient client{false};
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), std::chrono::seconds{2}));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable}));
    auto const deadline = std::chrono::steady_clock::now() + DURATION;
    while (std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds::zero());
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    static_cast<void>(client.send(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
        .world_revision = 1U,
        .delivery_token = 0xA'11CEU,
        .credits = 1U,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    client.poll(std::chrono::milliseconds::zero());
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    EXPECT_LE(deliveryBatchCount(client.messages), shared::HEIGHT_TILE_DELIVERY_WINDOW);
    EXPECT_LE(
        heightTileCount(client.messages),
        shared::HEIGHT_TILE_DELIVERY_WINDOW * shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY
    );
}

TEST(GameServerPreviewTest, SustainedFlightKeepsInputAcknowledgementsCurrentWhileTilesStream)
{
    static constexpr auto DURATION = std::chrono::seconds{35};
    static constexpr auto INPUT_PERIOD = std::chrono::milliseconds{100};
    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] { server.run(stop_requested); }};
    PreviewClient client;
    bool const connected = client.connect(core::Address::localhost(server.port()), std::chrono::seconds{2});
    bool joined = connected && client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable});
    uint32_t sent = 0U;
    uint32_t maximum_ack_lag = 0U;
    uint32_t sent_at_maximum_ack_lag = 0U;
    bool all_sent = true;
    auto const started = std::chrono::steady_clock::now();
    auto next_input = started;
    while (joined && std::chrono::steady_clock::now() - started < DURATION) {
        auto const now = std::chrono::steady_clock::now();
        if (now >= next_input) {
            ++sent;
            all_sent = client.send(shared::encodeMessage(shared::ClientInputMessage{
                .direction = {.x = 127U, .accelerated = true, .speedup = 500U},
                .sequence = sent,
            }), 0, core::SendMode{core::SendMode::Reliable}) && all_sent;
            next_input += INPUT_PERIOD;
        }
        while (client.poll(std::chrono::milliseconds::zero()) > 0) {
        }
        if (sent > 30U) {
            uint32_t const lag = sent - client.last_acknowledged_input;
            if (lag > maximum_ack_lag) {
                maximum_ack_lag = lag;
                sent_at_maximum_ack_lag = sent;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{16});
    }
    auto const catch_up_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (sent - client.last_acknowledged_input > 2U
        && std::chrono::steady_clock::now() < catch_up_deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(joined);
    EXPECT_TRUE(all_sent);
    EXPECT_GE(sent, 340U);
    // Prediction remains continuous while acknowledgements trail. Keep the worst
    // terrain-loaded lag far below the 64-input safety bound and require catch-up.
    EXPECT_LE(maximum_ack_lag, 16U)
        << "maximum lag occurred after input " << sent_at_maximum_ack_lag
        << ", last acknowledgement " << client.last_acknowledged_input;
    EXPECT_LE(sent - client.last_acknowledged_input, 2U);
}
