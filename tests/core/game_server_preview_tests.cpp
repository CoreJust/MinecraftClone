#include <server/GameServer.hpp>

#include <shared/net/Message.hpp>
#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/World.hpp>

#include <core/common/Defer.hpp>
#include <core/IO/Log.hpp>
#include <core/net/Client.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <span>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

class PreviewClient final : public core::Client {
public:
    static constexpr uint32_t MAX_TRACKED_INPUTS = 512U;

    explicit PreviewClient(
        bool const acknowledges_deliveries = true,
        bool const starts_deliveries = true,
        char const tracked_character = '@',
        bool const retains_messages = true
    )
        : core::Client{2}
        , m_acknowledges_deliveries{acknowledges_deliveries}
        , m_starts_deliveries{starts_deliveries}
        , m_tracked_character{tracked_character}
        , m_retains_messages{retains_messages}
    { }

    std::vector<shared::Message> messages;
    std::vector<shared::HeightTileKey> received_height_tile_keys;
    uint32_t received_height_tiles = 0U;
    uint32_t received_delivery_batches = 0U;
    uint32_t received_height_tile_descriptors = 0U;
    uint32_t descriptor_max_height_tiles = 0U;
    uint32_t descriptor_max_height_tile_bytes = 0U;
    uint32_t received_removals = 0U;
    uint32_t invalid_height_tiles = 0U;
    uint32_t sent_delivery_credit_messages = 0U;
    uint32_t last_acknowledged_input = 0U;
    int32_t last_player_x = 0;
    uint64_t acknowledgement_latency_samples = 0U;
    uint64_t acknowledgement_latency_total_ns = 0U;
    uint64_t acknowledgement_latency_max_ns = 0U;

    void trackInputSent(uint32_t const sequence)
    {
        if (sequence < MAX_TRACKED_INPUTS) {
            m_input_sent_at[sequence] = std::chrono::steady_clock::now();
        }
    }

    void setAcknowledgesDeliveries(bool const acknowledges_deliveries) noexcept
    {
        m_acknowledges_deliveries = acknowledges_deliveries;
    }

    bool acknowledgeDelivery(uint64_t const delivery_token)
    {
        return send(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
            .world_revision = 1U,
            .delivery_token = delivery_token,
            .credits = 1U,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable});
    }

private:
    void onDisconnected(core::DisconnectEvent const) override { }

    void recordHeightTile(shared::ServerHeightTileMessage const& tile)
    {
        ++received_height_tiles;
        if (!m_retains_messages) {
            received_height_tile_keys.push_back(tile.key);
        }
        if (tile.heights.size() != shared::HEIGHT_TILE_SAMPLE_COUNT
            || tile.token == 0U
            || tile.revision != 1U) {
            ++invalid_height_tiles;
        }
    }

    void onReceived(core::ReceiveEvent event) override
    {
        auto const message = shared::decodeMessage(event.data);
        if (!message) {
            return;
        }
        if (m_retains_messages) {
            messages.push_back(*message);
        }
        if (auto const* const tile = std::get_if<shared::ServerHeightTileMessage>(&*message)) {
            recordHeightTile(*tile);
        } else if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&*message)) {
            ++received_delivery_batches;
            received_removals += static_cast<uint32_t>(batch->removals.size());
            for (shared::ServerHeightTileMessage const& tile : batch->tiles) {
                recordHeightTile(tile);
            }
        } else if (std::holds_alternative<shared::ServerRemoveHeightTileMessage>(*message)) {
            ++received_removals;
        } else if (std::holds_alternative<shared::ServerHeightTileDescriptorMessage>(*message)) {
            ++received_height_tile_descriptors;
            auto const& descriptor = std::get<shared::ServerHeightTileDescriptorMessage>(*message);
            descriptor_max_height_tiles = descriptor.max_height_tiles;
            descriptor_max_height_tile_bytes = descriptor.max_height_tile_bytes;
        }
        if (auto const* const descriptor = std::get_if<shared::ServerHeightTileDescriptorMessage>(&*message);
            descriptor != nullptr && m_starts_deliveries) {
            if (send(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
                .world_revision = descriptor->world_revision,
                .delivery_token = 0U,
                .credits = shared::HEIGHT_TILE_DELIVERY_WINDOW,
            }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable})) {
                ++sent_delivery_credit_messages;
            }
        } else if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&*message);
            batch != nullptr && m_acknowledges_deliveries) {
            if (send(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
                .world_revision = 1U,
                .delivery_token = batch->delivery_token,
                .credits = 1U,
            }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable})) {
                ++sent_delivery_credit_messages;
            }
        }
        if (auto const* position = std::get_if<shared::ServerPlayerPositionMessage>(&*message);
            position != nullptr && position->ch == m_tracked_character) {
            uint32_t const previous_acknowledged_input = last_acknowledged_input;
            last_acknowledged_input = position->acknowledged_input_sequence;
            last_player_x = position->x;
            if (position->acknowledged_input_sequence > previous_acknowledged_input
                && position->acknowledged_input_sequence < MAX_TRACKED_INPUTS) {
                auto const sent_at = m_input_sent_at[position->acknowledged_input_sequence];
                if (sent_at.time_since_epoch().count() != 0) {
                    auto const latency = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - sent_at
                    );
                    uint64_t const latency_ns = static_cast<uint64_t>(latency.count());
                    ++acknowledgement_latency_samples;
                    acknowledgement_latency_total_ns += latency_ns;
                    acknowledgement_latency_max_ns = std::max(acknowledgement_latency_max_ns, latency_ns);
                }
            }
        }
    }

    bool m_acknowledges_deliveries;
    bool m_starts_deliveries;
    char m_tracked_character;
    bool m_retains_messages;
    std::array<std::chrono::steady_clock::time_point, MAX_TRACKED_INPUTS> m_input_sent_at{};
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

std::vector<shared::ServerHeightTileBatchMessage> deliveryBatchesFrom(
    std::vector<shared::Message> const& messages,
    size_t const first_message
)
{
    std::vector<shared::ServerHeightTileBatchMessage> batches;
    for (size_t index = std::min(first_message, messages.size()); index < messages.size(); ++index) {
        if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&messages[index])) {
            batches.push_back(*batch);
        }
    }
    return batches;
}

} // namespace

TEST(GameServerPreviewTest, MaterializedLandingMatchesTheStreamedMountainHeight)
{
    static constexpr std::chrono::seconds TIMEOUT{15};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr int32_t TARGET_X = 32'880;
    static constexpr int32_t TARGET_Y = 32'768;
    static constexpr int32_t TARGET_HEIGHT = 347;
    static constexpr uint32_t STABLE_GROUNDED_INPUTS = 5U;
    static constexpr shared::HeightTileKey TARGET_TILE{.x = 2'055, .y = 2'048};

    server::GameServer server{0, {{.character = '@', .x = TARGET_X, .y = TARGET_Y, .z = TARGET_HEIGHT}},
        shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] { server.run(stop_requested); }};
    PreviewClient client;
    bool const connected = client.connect(core::Address::localhost(server.port()), TIMEOUT);
    bool const joined = connected && client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable});
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    std::optional<shared::ServerPlayerPositionMessage> position;
    std::optional<uint16_t> preview_height;
    uint32_t inspected_messages = 0U;
    uint32_t sequence = 1U;
    uint32_t stable_grounded_inputs = 0U;
    uint8_t last_cycle_source = 255U;
    bool saw_collision_enabled_flight = false;
    bool sent_all_inputs = true;
    while (joined && sent_all_inputs
        && (stable_grounded_inputs < STABLE_GROUNDED_INPUTS || !preview_height.has_value())
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
        while (inspected_messages < client.messages.size()) {
            shared::Message const& message = client.messages[inspected_messages++];
            if (auto const* const update = std::get_if<shared::ServerPlayerPositionMessage>(&message)) {
                position = *update;
                saw_collision_enabled_flight |= update->movement_capabilities.bits == 1U;
                if (update->movement_capabilities.bits == 0U && update->vertical_velocity_subcells == 0) {
                    if (stable_grounded_inputs < STABLE_GROUNDED_INPUTS) {
                        ++stable_grounded_inputs;
                    }
                } else {
                    stable_grounded_inputs = 0U;
                }
            }
            auto const inspect_tile = [&preview_height](shared::ServerHeightTileMessage const& tile) {
                if (tile.key == TARGET_TILE) {
                    preview_height = tile.heights[0U];
                }
            };
            if (auto const* const tile = std::get_if<shared::ServerHeightTileMessage>(&message)) {
                inspect_tile(*tile);
            } else if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&message)) {
                for (shared::ServerHeightTileMessage const& tile : batch->tiles) {
                    inspect_tile(tile);
                }
            }
        }
        if (!position.has_value() || position->acknowledged_input_sequence + 1U != sequence) {
            continue;
        }
        uint8_t const capability_bits = position->movement_capabilities.bits;
        bool const cycle = capability_bits != 0U && last_cycle_source != capability_bits;
        if (cycle) {
            last_cycle_source = capability_bits;
        }
        sent_all_inputs = client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {.cycle_movement_capabilities = cycle}, .sequence = sequence++,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable});
    }
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(joined);
    ASSERT_TRUE(sent_all_inputs);
    ASSERT_TRUE(saw_collision_enabled_flight);
    ASSERT_TRUE(preview_height.has_value());
    EXPECT_EQ(*preview_height, TARGET_HEIGHT);
    ASSERT_TRUE(position.has_value());
    ASSERT_EQ(stable_grounded_inputs, STABLE_GROUNDED_INPUTS);
    EXPECT_EQ(position->movement_capabilities.bits, 0U);
    EXPECT_EQ(position->x, TARGET_X);
    EXPECT_EQ(position->y, TARGET_Y);
    EXPECT_EQ(position->z, *preview_height);
    EXPECT_EQ(position->z_subcell, 0U);
    EXPECT_EQ(position->vertical_velocity_subcells, 0);
}

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
    while (joined && client.received_height_tiles < OBSERVED_TILE_COUNT
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
    static constexpr uint32_t OBSERVED_TILE_COUNT = 512U;
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
    while (joined && client.received_height_tiles < OBSERVED_TILE_COUNT
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

TEST(GameServerPreviewTest, ResetsPriorityCursorAfterProgressWhenHeadingChanges)
{
    static constexpr std::chrono::seconds TIMEOUT{10};
    static constexpr std::chrono::seconds ROTATION_TIMEOUT{3};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t INITIAL_TILE_COUNT = 256U;
    static constexpr uint32_t CURSOR_LOOKAHEAD = 512U;
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
    auto const initial_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (joined && client.received_height_tiles < INITIAL_TILE_COUNT
        && std::chrono::steady_clock::now() < initial_deadline) {
        client.poll(POLL_INTERVAL);
    }

    int32_t const center_x = shared::World::FLIGHT_SPAWN.x
        / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
    int32_t const center_y = shared::World::FLIGHT_SPAWN.y
        / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
    shared::HeightTileInterest const north_interest = shared::makeHeightTileInterest(
        {.x = center_x, .y = center_y},
        0,
        127
    );
    shared::HeightTileInterest const east_interest = shared::makeHeightTileInterest(
        {.x = center_x, .y = center_y},
        127,
        0
    );
    std::vector<shared::HeightTileKey> const initial_keys = heightTileKeys(client.messages);
    auto const east_priority_prefix = std::span{east_interest.keys}.first(INITIAL_TILE_COUNT);
    auto const north_priority_prefix = std::span{north_interest.keys}.first(
        INITIAL_TILE_COUNT + CURSOR_LOOKAHEAD
    );
    auto const east_frontier_candidate = std::ranges::find_if(
        east_priority_prefix,
        [&north_priority_prefix, &initial_keys](shared::HeightTileKey const key) {
            return std::ranges::find(north_priority_prefix, key) == north_priority_prefix.end()
                && std::ranges::find(initial_keys, key) == initial_keys.end();
        }
    );
    bool const has_east_frontier_candidate = east_frontier_candidate != east_priority_prefix.end();
    shared::HeightTileKey const east_frontier = has_east_frontier_candidate
        ? *east_frontier_candidate
        : shared::HeightTileKey{};
    auto const after_rotation = client.messages.size();
    bool const rotated = joined && client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {.view_x = 127, .view_y = 0},
        .sequence = 1U,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable});
    auto const rotation_deadline = std::chrono::steady_clock::now() + ROTATION_TIMEOUT;
    auto const received_east_frontier = [&client, after_rotation, east_frontier] {
        return std::ranges::any_of(
            std::span{client.messages}.subspan(after_rotation),
            [east_frontier](shared::Message const& message) {
                if (auto const* const tile = std::get_if<shared::ServerHeightTileMessage>(&message)) {
                    return tile->key == east_frontier;
                }
                auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&message);
                return batch != nullptr && std::ranges::any_of(
                    batch->tiles,
                    [east_frontier](shared::ServerHeightTileMessage const& tile) {
                        return tile.key == east_frontier;
                    }
                );
            }
        );
    };
    while (rotated && has_east_frontier_candidate
        && (client.last_acknowledged_input < 1U || !received_east_frontier())
        && std::chrono::steady_clock::now() < rotation_deadline) {
        client.poll(POLL_INTERVAL);
    }
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(joined);
    ASSERT_GE(initial_keys.size(), INITIAL_TILE_COUNT);
    EXPECT_TRUE(has_east_frontier_candidate);
    EXPECT_TRUE(rotated);
    EXPECT_GE(client.last_acknowledged_input, 1U);
    EXPECT_TRUE(received_east_frontier());
}

TEST(GameServerPreviewTest, CameraRotationKeepsInterestAndMovementAddsFreshFrontier)
{
    static constexpr std::chrono::seconds TIMEOUT{10};
    static constexpr uint32_t OBSERVED_TILE_COUNT = 16U;
    static constexpr uint16_t FLIGHT_SPEEDUP = 500U;
    static constexpr uint32_t MOVEMENT_INPUT_COUNT = (
        shared::HEIGHT_TILE_INTEREST_RADIUS * shared::HEIGHT_TILE_SIDE_LENGTH * shared::SUBCELLS_PER_CELL
    ) / (shared::MOVEMENT_SUBCELLS_PER_TICK * FLIGHT_SPEEDUP) + 1U;
    static constexpr uint32_t OBSERVED_NEW_FRONTIER_TILES = 8U;
    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] { server.run(stop_requested); }};
    PreviewClient client;
    bool const connected = client.connect(core::Address::localhost(server.port()), TIMEOUT);
    bool const joined = connected && client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable});
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (joined && client.received_height_tiles < OBSERVED_TILE_COUNT
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    size_t const before_rotation = client.messages.size();
    bool const rotated = client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {.view_x = 127, .view_y = 0},
        .sequence = 1U,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable});
    auto const rotation_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (rotated && client.last_acknowledged_input < 1U
        && std::chrono::steady_clock::now() < rotation_deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    size_t const after_rotation_ack = client.messages.size();

    int32_t const original_center_x = shared::World::FLIGHT_SPAWN.x
        / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
    uint32_t const final_input_sequence = 1U + MOVEMENT_INPUT_COUNT;
    bool sent_all_movement = true;
    for (uint32_t sequence = 2U; sequence <= final_input_sequence; ++sequence) {
        sent_all_movement = client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {.x = 127, .accelerated = true, .speedup = FLIGHT_SPEEDUP, .view_x = 127},
            .sequence = sequence,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}) && sent_all_movement;
    }
    auto const movement_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (sent_all_movement
        && (client.last_acknowledged_input < final_input_sequence
            || client.last_player_x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH) <= original_center_x)
        && std::chrono::steady_clock::now() < movement_deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    int32_t const moved_center_x = client.last_player_x
        / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
    shared::HeightTileInterest const original_interest = shared::makeHeightTileInterest(
        {.x = original_center_x, .y = shared::World::FLIGHT_SPAWN.y
            / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH)},
        127,
        0
    );
    shared::HeightTileInterest const moved_interest = shared::makeHeightTileInterest(
        {.x = moved_center_x, .y = shared::World::FLIGHT_SPAWN.y
            / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH)},
        127,
        0
    );
    auto const key_id = [](shared::HeightTileKey const key) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(key.x)) << 32U)
            | static_cast<uint32_t>(key.y);
    };
    std::unordered_set<uint64_t> original_interest_ids;
    original_interest_ids.reserve(original_interest.keys.size());
    for (shared::HeightTileKey const key : original_interest.keys) {
        original_interest_ids.insert(key_id(key));
    }
    std::unordered_set<uint64_t> new_frontier_ids;
    for (shared::HeightTileKey const key : moved_interest.keys) {
        uint64_t const id = key_id(key);
        if (!original_interest_ids.contains(id)) {
            new_frontier_ids.insert(id);
        }
    }
    std::vector<shared::HeightTileKey> moved_frontier_tiles;
    auto const collect_new_frontier_tiles = [&] {
        moved_frontier_tiles = heightTileKeysFrom(client.messages, after_rotation_ack);
        return static_cast<uint32_t>(std::ranges::count_if(
            moved_frontier_tiles,
            [&key_id, &new_frontier_ids](shared::HeightTileKey const key) {
                return new_frontier_ids.contains(key_id(key));
            }
        ));
    };
    while (sent_all_movement && collect_new_frontier_tiles() < OBSERVED_NEW_FRONTIER_TILES
        && std::chrono::steady_clock::now() < movement_deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    std::span<shared::Message const> const rotation_only = std::span{client.messages}.subspan(
        before_rotation,
        after_rotation_ack - before_rotation
    );
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(joined);
    ASSERT_TRUE(rotated);
    ASSERT_TRUE(sent_all_movement);
    EXPECT_GE(client.last_acknowledged_input, 1U);
    EXPECT_EQ(client.last_acknowledged_input, final_input_sequence);
    EXPECT_GT(moved_center_x, original_center_x);
    EXPECT_FALSE(new_frontier_ids.empty());
    EXPECT_GE(collect_new_frontier_tiles(), OBSERVED_NEW_FRONTIER_TILES);
    EXPECT_EQ(removalCount(rotation_only), 0U);
}

TEST(GameServerPreviewTest, StreamsPlayerCenteredHeightTilesAndRemovesDepartedTiles)
{
    static constexpr std::chrono::seconds STREAM_TIMEOUT{90};
    static constexpr std::chrono::seconds MOVEMENT_TIMEOUT{10};
    static constexpr std::chrono::seconds PROGRESS_INTERVAL{1};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t MOVEMENT_INPUT_COUNT = (
        shared::HEIGHT_TILE_SIDE_LENGTH * shared::SUBCELLS_PER_CELL
        + shared::MOVEMENT_SUBCELLS_PER_TICK - 1U
    ) / shared::MOVEMENT_SUBCELLS_PER_TICK;
    shared::HeightTileInterest const expected_interest = shared::makeHeightTileInterest(
        {
            .x = shared::World::FLIGHT_SPAWN.x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
            .y = shared::World::FLIGHT_SPAWN.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        },
        0,
        127
    );
    uint32_t const expected_height_tiles = static_cast<uint32_t>(expected_interest.keys.size());

    uint32_t const hardware_thread_count = static_cast<uint32_t>(std::thread::hardware_concurrency());
    auto const server_started_at = std::chrono::steady_clock::now();
    auto next_server_progress = server_started_at + PROGRESS_INTERVAL;
    server::GameServer::BenchmarkHooks const hooks{
        .on_preview_metrics = [server_started_at, &next_server_progress](
            core::ClientId,
            server::GameServer::PreviewStreamMetrics const metrics
        ) {
            auto const now = std::chrono::steady_clock::now();
            if (now < next_server_progress) {
                return;
            }
            auto const average_ms = [](
                std::chrono::nanoseconds const total,
                uint64_t const samples
            ) {
                return samples == 0U
                    ? int64_t{0}
                    : total.count() / static_cast<int64_t>(samples) / 1'000'000;
            };
            CORE_INFO(
                "Server preview at {} ms: queued={}, dispatched={}, worker pending={}, worker submitted={}, "
                "ready={}, in-flight batches={}, in-flight additions={}, credits={}, resident={}, "
                "tile jobs submitted/started/finished/collected={}/{}/{}/{}, "
                "tile enqueue-submit/executor/run/collect avg/max ms={}/{}/{}/{}/{}/{}/{}/{}, "
                "world jobs submitted/started/finished/collected={}/{}/{}/{}, "
                "world enqueue-submit/executor/run/collect avg/max ms={}/{}/{}/{}/{}/{}/{}/{}, "
                "loop interval/work/tick/pump/poll ms={}/{}/{}/{}/{}, input received/ack/pending/failures={}/{}/{}/{}, "
                "credit samples/avg/max ms={}/{}/{}",
                std::chrono::duration_cast<std::chrono::milliseconds>(now - server_started_at).count(),
                metrics.queued_tiles,
                metrics.dispatched_tiles,
                metrics.pending_worker_jobs,
                metrics.submitted_worker_jobs,
                metrics.ready_tiles,
                metrics.inflight_deliveries,
                metrics.inflight_additions,
                metrics.delivery_credits,
                metrics.resident_tiles,
                metrics.height_tile_jobs.submitted_total,
                metrics.height_tile_jobs.started_total,
                metrics.height_tile_jobs.finished_total,
                metrics.height_tile_jobs.collected_total,
                average_ms(
                    metrics.height_tile_jobs.enqueue_to_submit_total,
                    metrics.height_tile_jobs.submitted_total
                ),
                metrics.height_tile_jobs.enqueue_to_submit_max.count() / 1'000'000,
                average_ms(metrics.height_tile_jobs.executor_queue_total, metrics.height_tile_jobs.started_total),
                metrics.height_tile_jobs.executor_queue_max.count() / 1'000'000,
                average_ms(metrics.height_tile_jobs.execution_total, metrics.height_tile_jobs.finished_total),
                metrics.height_tile_jobs.execution_max.count() / 1'000'000,
                average_ms(
                    metrics.height_tile_jobs.completion_to_collection_total,
                    metrics.height_tile_jobs.collected_total
                ),
                metrics.height_tile_jobs.completion_to_collection_max.count() / 1'000'000,
                metrics.world_generation_jobs.submitted_total,
                metrics.world_generation_jobs.started_total,
                metrics.world_generation_jobs.finished_total,
                metrics.world_generation_jobs.collected_total,
                average_ms(
                    metrics.world_generation_jobs.enqueue_to_submit_total,
                    metrics.world_generation_jobs.submitted_total
                ),
                metrics.world_generation_jobs.enqueue_to_submit_max.count() / 1'000'000,
                average_ms(metrics.world_generation_jobs.executor_queue_total, metrics.world_generation_jobs.started_total),
                metrics.world_generation_jobs.executor_queue_max.count() / 1'000'000,
                average_ms(metrics.world_generation_jobs.execution_total, metrics.world_generation_jobs.finished_total),
                metrics.world_generation_jobs.execution_max.count() / 1'000'000,
                average_ms(
                    metrics.world_generation_jobs.completion_to_collection_total,
                    metrics.world_generation_jobs.collected_total
                ),
                metrics.world_generation_jobs.completion_to_collection_max.count() / 1'000'000,
                metrics.server_loop_interval.count() / 1'000'000,
                metrics.server_loop_work.count() / 1'000'000,
                metrics.server_tick.count() / 1'000'000,
                metrics.stream_pump.count() / 1'000'000,
                metrics.network_poll.count() / 1'000'000,
                metrics.latest_received_input_sequence,
                metrics.acknowledged_input_sequence,
                metrics.pending_input_count,
                metrics.materialization_admission_failures,
                metrics.delivery_credit_samples,
                metrics.delivery_credit_samples == 0U
                    ? 0U
                    : metrics.delivery_credit_total.count()
                        / static_cast<int64_t>(metrics.delivery_credit_samples) / 1'000'000,
                metrics.delivery_credit_max.count() / 1'000'000
            );
            next_server_progress = now + PROGRESS_INTERVAL;
        },
    };

    CORE_INFO(
        "Full-radius preview worker budget: hardware threads={}, terrain workers={}",
        hardware_thread_count,
        server::GameServer::terrainWorkerCount(hardware_thread_count)
    );
    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested, &hooks] {
        server.run(stop_requested, &hooks);
    }};
    PreviewClient client{true, true, '@', false};
    client.received_height_tile_keys.reserve(expected_height_tiles);
    if (!client.connect(core::Address::localhost(server.port()), STREAM_TIMEOUT)) {
        stop_requested.store(true, std::memory_order_relaxed);
        server_thread.join();
        ADD_FAILURE() << "Client failed to connect within " << STREAM_TIMEOUT.count() << " seconds";
        return;
    }
    if (!client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable})) {
        stop_requested.store(true, std::memory_order_relaxed);
        server_thread.join();
        ADD_FAILURE() << "Client failed to join the preview stream";
        return;
    }

    auto const started_at = std::chrono::steady_clock::now();
    auto const stream_deadline = started_at + STREAM_TIMEOUT;
    auto next_progress = started_at + PROGRESS_INTERVAL;
    uint32_t poll_iterations = 0U;
    while (client.received_height_tiles < expected_height_tiles
        && std::chrono::steady_clock::now() < stream_deadline) {
        client.poll(POLL_INTERVAL);
        ++poll_iterations;
        auto const now = std::chrono::steady_clock::now();
        if (now >= next_progress) {
            CORE_INFO(
                "Client preview: {}/{} tiles, {} batches, {} credit messages, {} poll iterations after {} ms",
                client.received_height_tiles,
                expected_height_tiles,
                client.received_delivery_batches,
                client.sent_delivery_credit_messages,
                poll_iterations,
                std::chrono::duration_cast<std::chrono::milliseconds>(now - started_at).count()
            );
            next_progress = now + PROGRESS_INTERVAL;
        }
    }

    bool const received_all_height_tiles = client.received_height_tiles == expected_height_tiles;
    std::vector<shared::HeightTileKey> received_keys = std::move(client.received_height_tile_keys);
    std::vector<shared::HeightTileKey> expected_keys = expected_interest.keys;
    auto const key_order = [](shared::HeightTileKey const first, shared::HeightTileKey const second) {
        return std::tie(first.x, first.y) < std::tie(second.x, second.y);
    };
    std::ranges::sort(received_keys, key_order);
    std::ranges::sort(expected_keys, key_order);
    bool sent_all_inputs = true;
    for (uint32_t sequence{1U}; sequence <= MOVEMENT_INPUT_COUNT; ++sequence) {
        sent_all_inputs = client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {.x = 127U},
            .sequence = sequence,
        }), 0, core::SendMode{core::SendMode::Reliable}) && sent_all_inputs;
    }
    auto const movement_deadline = std::chrono::steady_clock::now() + MOVEMENT_TIMEOUT;
    while (client.received_removals == 0U
        && std::chrono::steady_clock::now() < movement_deadline) {
        client.poll(POLL_INTERVAL);
    }
    bool const received_removal = client.received_removals > 0U;
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    CORE_INFO(
        "Full-radius preview result: {}/{} tiles, {} batches, {} credit messages, "
        "{} poll iterations; connected-to-result duration={} ms",
        client.received_height_tiles,
        expected_height_tiles,
        client.received_delivery_batches,
        client.sent_delivery_credit_messages,
        poll_iterations,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at
        ).count()
    );
    EXPECT_TRUE(received_all_height_tiles) << "received " << client.received_height_tiles
        << " of " << expected_height_tiles;
    EXPECT_TRUE(client.messages.empty());
    EXPECT_EQ(received_keys, expected_keys);
    EXPECT_EQ(client.invalid_height_tiles, 0U);
    EXPECT_EQ(client.received_height_tile_descriptors, 1U);
    EXPECT_EQ(client.descriptor_max_height_tiles, shared::HEIGHT_TILE_INTEREST_COUNT);
    EXPECT_EQ(client.descriptor_max_height_tile_bytes, shared::HEIGHT_TILE_PAYLOAD_BYTES);
    EXPECT_TRUE(sent_all_inputs);
    EXPECT_TRUE(received_removal);
    EXPECT_GT(
        client.last_player_x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        shared::World::FLIGHT_SPAWN.x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH)
    );
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

TEST(GameServerPreviewTest, SustainedNetworkIngressDoesNotStarveTicksTerrainOrShutdown)
{
    static constexpr auto WARMUP_TIMEOUT = std::chrono::seconds{10};
    static constexpr auto PROGRESS_TIMEOUT = std::chrono::seconds{2};
    static constexpr auto SHUTDOWN_TIMEOUT = std::chrono::milliseconds{250};
    static constexpr auto MINIMUM_INGRESS_DURATION = std::chrono::milliseconds{500};
    static constexpr uint32_t FLOOD_CLIENT_COUNT = 3U;
    static constexpr uint32_t MAX_FLOOD_PACKETS_PER_CLIENT = 500'000U;
    server::GameServer server{0, {}, shared::WorldMode::Flight, shared::World::canonicalConfiguration(), 4U};
    std::atomic_bool stop_requested{false};
    std::atomic_bool flood_stopped{false};
    std::atomic_bool server_exited{false};
    std::atomic_uint64_t tick_count{0U};
    std::atomic_uint64_t metrics_count{0U};
    std::atomic_uint64_t submitted_tile_jobs{0U};
    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = [&tick_count](std::chrono::nanoseconds, uint64_t) {
            tick_count.fetch_add(1U, std::memory_order_relaxed);
        },
        .on_preview_metrics = [&metrics_count, &submitted_tile_jobs](
            core::ClientId,
            server::GameServer::PreviewStreamMetrics const metrics
        ) {
            metrics_count.fetch_add(1U, std::memory_order_relaxed);
            submitted_tile_jobs.store(metrics.height_tile_jobs.submitted_total, std::memory_order_relaxed);
        },
    };
    PreviewClient observer;
    std::array<PreviewClient, FLOOD_CLIENT_COUNT> flood_clients;
    std::array<std::atomic_uint64_t, FLOOD_CLIENT_COUNT> sent_packets{};
    std::array<std::thread, FLOOD_CLIENT_COUNT> flood_threads;
    auto const totalSentPackets = [&sent_packets] {
        uint64_t count = 0U;
        for (std::atomic_uint64_t const& sent : sent_packets) {
            count += sent.load(std::memory_order_relaxed);
        }
        return count;
    };
    std::thread server_thread{[&server, &stop_requested, &hooks, &server_exited] {
        server.run(stop_requested, &hooks);
        server_exited.store(true, std::memory_order_release);
    }};
    defer {
        flood_stopped.store(true, std::memory_order_relaxed);
        for (std::thread& flood_thread : flood_threads) {
            if (flood_thread.joinable()) {
                flood_thread.join();
            }
        }
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };

    ASSERT_TRUE(observer.connect(core::Address::localhost(server.port()), WARMUP_TIMEOUT));
    ASSERT_TRUE(observer.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable}));
    auto deadline = std::chrono::steady_clock::now() + WARMUP_TIMEOUT;
    while (observer.received_height_tiles == 0U && std::chrono::steady_clock::now() < deadline) {
        observer.poll(std::chrono::milliseconds{1});
    }
    ASSERT_GT(observer.received_height_tiles, 0U);
    ASSERT_GT(metrics_count.load(std::memory_order_relaxed), 0U);

    for (PreviewClient& flood_client : flood_clients) {
        ASSERT_TRUE(flood_client.connect(core::Address::localhost(server.port()), WARMUP_TIMEOUT));
    }
    auto const flood_packet = shared::encodeMessage(shared::ClientHeightTileCreditMessage{
        .world_revision = 1U,
        .delivery_token = 0xA'11CEU,
        .credits = 1U,
    });
    auto const flood_client_loop = [&flood_stopped, &flood_packet](
        PreviewClient& client,
        std::atomic_uint64_t& sent_count
    ) {
        static constexpr uint32_t PACKETS_PER_FLUSH = 128U;
        while (!flood_stopped.load(std::memory_order_relaxed)
            && sent_count.load(std::memory_order_relaxed) < MAX_FLOOD_PACKETS_PER_CLIENT) {
            uint32_t batch_count = 0U;
            while (batch_count < PACKETS_PER_FLUSH
                && !flood_stopped.load(std::memory_order_relaxed)
                && sent_count.load(std::memory_order_relaxed) < MAX_FLOOD_PACKETS_PER_CLIENT) {
                if (client.send(flood_packet, shared::GAME_CHANNEL,
                        core::SendMode{core::SendMode::Unsequenced})) {
                    sent_count.fetch_add(1U, std::memory_order_relaxed);
                    ++batch_count;
                }
            }
            client.flush();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    };
    for (uint32_t index = 0U; index < FLOOD_CLIENT_COUNT; ++index) {
        flood_threads[index] = std::thread{
            [client = &flood_clients[index], sent_count = &sent_packets[index], flood_client_loop] {
                flood_client_loop(*client, *sent_count);
            }
        };
    }

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (totalSentPackets() < 1'000U && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    uint32_t const baseline_tiles = observer.received_height_tiles;
    uint64_t const baseline_ticks = tick_count.load(std::memory_order_relaxed);
    uint64_t const baseline_metrics = metrics_count.load(std::memory_order_relaxed);
    uint64_t const baseline_jobs = submitted_tile_jobs.load(std::memory_order_relaxed);
    auto const flood_started_at = std::chrono::steady_clock::now();
    for (uint32_t sequence = 1U; sequence <= 8U; ++sequence) {
        ASSERT_TRUE(observer.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {.x = 127U, .accelerated = true, .speedup = 500U},
            .sequence = sequence,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    }

    bool made_progress_under_ingress = false;
    deadline = std::chrono::steady_clock::now() + PROGRESS_TIMEOUT;
    while (std::chrono::steady_clock::now() < deadline) {
        observer.poll(std::chrono::milliseconds{1});
        bool const tick_progress = tick_count.load(std::memory_order_relaxed) >= baseline_ticks + 2U;
        bool const metrics_progress = metrics_count.load(std::memory_order_relaxed) > baseline_metrics;
        bool const terrain_progress = observer.received_height_tiles > baseline_tiles
            || submitted_tile_jobs.load(std::memory_order_relaxed) > baseline_jobs;
        if (tick_progress && metrics_progress && terrain_progress) {
            made_progress_under_ingress = true;
        }
        if (made_progress_under_ingress
            && std::chrono::steady_clock::now() - flood_started_at >= MINIMUM_INGRESS_DURATION) {
            break;
        }
    }
    uint64_t const packets_before_shutdown = totalSentPackets();
    stop_requested.store(true, std::memory_order_relaxed);
    deadline = std::chrono::steady_clock::now() + SHUTDOWN_TIMEOUT;
    while (!server_exited.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        observer.poll(std::chrono::milliseconds{1});
    }
    bool const stopped_during_ingress = server_exited.load(std::memory_order_acquire)
        && totalSentPackets() > packets_before_shutdown;
    flood_stopped.store(true, std::memory_order_relaxed);
    for (std::thread& flood_thread : flood_threads) {
        if (flood_thread.joinable()) {
            flood_thread.join();
        }
    }
    if (!server_exited.load(std::memory_order_acquire)) {
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!server_exited.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    ASSERT_TRUE(server_exited.load(std::memory_order_acquire));
    server_thread.join();

    EXPECT_GE(totalSentPackets(), 1'000U);
    EXPECT_TRUE(made_progress_under_ingress);
    EXPECT_TRUE(stopped_during_ingress);
}

TEST(GameServerPreviewTest, StalledClientReservesGameplayCapacityAndResumesOneAcknowledgedBatch)
{
    static constexpr auto DURATION = std::chrono::seconds{2};
    static constexpr std::chrono::milliseconds CREDIT_OBSERVATION_DURATION{100};
    static constexpr uint32_t MAXIMUM_OUTSTANDING_BATCHES = 4U;
    static constexpr uint32_t MAXIMUM_ENCODED_BATCH_BYTES = 8'589U;
    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] { server.run(stop_requested); }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };
    PreviewClient client{false, false};
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), std::chrono::seconds{2}));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable}));
    auto deadline = std::chrono::steady_clock::now() + DURATION;
    while (std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds::zero());
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(std::ranges::any_of(client.messages, [](shared::Message const& message) {
        return std::holds_alternative<shared::ServerHeightTileDescriptorMessage>(message);
    }));
    EXPECT_EQ(deliveryBatchCount(client.messages), 0U);
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
        .world_revision = 1U,
        .delivery_token = 0xA'11CEU,
        .credits = 1U,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {.x = 127U}, .sequence = 1U,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    deadline = std::chrono::steady_clock::now() + DURATION;
    while (client.last_acknowledged_input < 1U && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(client.last_acknowledged_input, 1U);
    deadline = std::chrono::steady_clock::now() + CREDIT_OBSERVATION_DURATION;
    while (std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(deliveryBatchCount(client.messages), 0U);
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
        .world_revision = 1U,
        .delivery_token = 0U,
        .credits = shared::HEIGHT_TILE_DELIVERY_WINDOW,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    deadline = std::chrono::steady_clock::now() + DURATION;
    while (deliveryBatchCount(client.messages) < MAXIMUM_OUTSTANDING_BATCHES
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    std::vector<shared::ServerHeightTileBatchMessage> const stalled_batches = deliveryBatchesFrom(client.messages, 0U);
    ASSERT_EQ(stalled_batches.size(), MAXIMUM_OUTSTANDING_BATCHES);
    for (shared::ServerHeightTileBatchMessage const& batch : stalled_batches) {
        EXPECT_LE(shared::encodeMessage(batch).size(), MAXIMUM_ENCODED_BATCH_BYTES);
    }
    ASSERT_FALSE(stalled_batches.front().tiles.empty());
    shared::ServerHeightTileBatchMessage maximum_batch = stalled_batches.front();
    maximum_batch.tiles.resize(shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY, maximum_batch.tiles.front());
    maximum_batch.removals.clear();
    EXPECT_EQ(shared::encodeMessage(maximum_batch).size(), MAXIMUM_ENCODED_BATCH_BYTES);
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {.x = 127U}, .sequence = 2U,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    deadline = std::chrono::steady_clock::now() + DURATION;
    while (client.last_acknowledged_input < 2U && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(client.last_acknowledged_input, 2U);
    EXPECT_EQ(deliveryBatchCount(client.messages), MAXIMUM_OUTSTANDING_BATCHES);
    ASSERT_TRUE(client.acknowledgeDelivery(stalled_batches.front().delivery_token));
    deadline = std::chrono::steady_clock::now() + DURATION;
    while (deliveryBatchCount(client.messages) < MAXIMUM_OUTSTANDING_BATCHES + 1U
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    deadline = std::chrono::steady_clock::now() + CREDIT_OBSERVATION_DURATION;
    while (std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(deliveryBatchCount(client.messages), MAXIMUM_OUTSTANDING_BATCHES + 1U);
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    EXPECT_LE(deliveryBatchCount(client.messages), shared::HEIGHT_TILE_DELIVERY_WINDOW);
    EXPECT_LE(
        heightTileCount(client.messages),
        shared::HEIGHT_TILE_DELIVERY_WINDOW * shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY
    );
}

TEST(GameServerPreviewTest, StalledCreditsBoundPrefetchAndResumeTerrainProgress)
{
    static constexpr std::chrono::seconds TIMEOUT{5};
    static constexpr std::chrono::seconds STALL_DURATION{2};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t MAXIMUM_BUFFERED_TILES = 128U;
    static constexpr uint32_t RECOVERED_TILE_COUNT = 512U;
    static constexpr uint32_t MAXIMUM_OUTSTANDING_BATCHES = 4U;

    uint32_t maximum_buffered_tiles = 0U;
    server::GameServer::BenchmarkHooks const hooks{
        .on_preview_buffered = [&maximum_buffered_tiles](core::ClientId, uint32_t const buffered_tiles) {
            maximum_buffered_tiles = std::max(maximum_buffered_tiles, buffered_tiles);
        },
    };
    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested, &hooks] {
        server.run(stop_requested, &hooks);
    }};
    PreviewClient client{false};
    bool const connected = client.connect(core::Address::localhost(server.port()), TIMEOUT);
    bool const joined = connected && client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable});
    auto const stalled_deadline = std::chrono::steady_clock::now() + STALL_DURATION;
    while (joined && std::chrono::steady_clock::now() < stalled_deadline) {
        client.poll(POLL_INTERVAL);
    }
    uint32_t const stalled_tile_count = client.received_height_tiles;
    std::vector<shared::ServerHeightTileBatchMessage> const stalled_batches = deliveryBatchesFrom(
        client.messages, 0U
    );
    client.setAcknowledgesDeliveries(true);
    bool acknowledged_all = true;
    for (shared::ServerHeightTileBatchMessage const& batch : stalled_batches) {
        acknowledged_all = client.acknowledgeDelivery(batch.delivery_token) && acknowledged_all;
    }
    auto const recovery_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (joined && client.received_height_tiles < RECOVERED_TILE_COUNT
        && std::chrono::steady_clock::now() < recovery_deadline) {
        client.poll(POLL_INTERVAL);
    }
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(joined);
    ASSERT_TRUE(acknowledged_all);
    EXPECT_EQ(stalled_batches.size(), MAXIMUM_OUTSTANDING_BATCHES);
    EXPECT_LE(
        stalled_tile_count,
        shared::HEIGHT_TILE_DELIVERY_WINDOW * shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY
    );
    EXPECT_EQ(maximum_buffered_tiles, MAXIMUM_BUFFERED_TILES);
    EXPECT_GE(client.received_height_tiles, RECOVERED_TILE_COUNT);
}

TEST(GameServerPreviewTest, ReversalCancelsQueuedRemovalsForRestoredInterest)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{10};
    static constexpr uint32_t INITIAL_TILES = 512U;
    static constexpr uint32_t MAXIMUM_OUTSTANDING_BATCHES = 4U;
    static constexpr uint16_t FLIGHT_SPEEDUP = 500U;
    static constexpr uint32_t MOVEMENT_INPUT_COUNT = (
        shared::HEIGHT_TILE_INTEREST_RADIUS * shared::HEIGHT_TILE_SIDE_LENGTH * shared::SUBCELLS_PER_CELL
    ) / (shared::MOVEMENT_SUBCELLS_PER_TICK * FLIGHT_SPEEDUP) + 1U;
    static constexpr uint32_t FINAL_INPUT_SEQUENCE = 2U * MOVEMENT_INPUT_COUNT;
    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] { server.run(stop_requested); }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };
    PreviewClient client;
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable}));

    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.received_height_tiles < INITIAL_TILES
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_GE(client.received_height_tiles, INITIAL_TILES);

    client.setAcknowledgesDeliveries(false);
    size_t const stalled_message = client.messages.size();
    uint32_t const stalled_batch = deliveryBatchCount(client.messages);
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (deliveryBatchCount(client.messages) < stalled_batch + MAXIMUM_OUTSTANDING_BATCHES
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    std::vector<shared::ServerHeightTileBatchMessage> const stalled_batches = deliveryBatchesFrom(
        client.messages,
        stalled_message
    );
    ASSERT_EQ(stalled_batches.size(), MAXIMUM_OUTSTANDING_BATCHES);

    for (uint32_t sequence = 1U; sequence <= MOVEMENT_INPUT_COUNT; ++sequence) {
        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {.x = 127U, .accelerated = true, .speedup = FLIGHT_SPEEDUP},
            .sequence = sequence,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    }
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.last_acknowledged_input < MOVEMENT_INPUT_COUNT && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_EQ(client.last_acknowledged_input, MOVEMENT_INPUT_COUNT);
    for (uint32_t sequence = MOVEMENT_INPUT_COUNT + 1U; sequence <= FINAL_INPUT_SEQUENCE; ++sequence) {
        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {
                .x = static_cast<uint8_t>(-127),
                .accelerated = true,
                .speedup = FLIGHT_SPEEDUP,
            },
            .sequence = sequence,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    }
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.last_acknowledged_input < FINAL_INPUT_SEQUENCE && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_EQ(client.last_acknowledged_input, FINAL_INPUT_SEQUENCE);

    shared::HeightTileInterest const restored_interest = shared::makeHeightTileInterest({
        .x = client.last_player_x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        .y = shared::World::FLIGHT_SPAWN.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
    }, -127, 0);
    size_t const resumed_message = client.messages.size();
    client.setAcknowledgesDeliveries(true);
    for (shared::ServerHeightTileBatchMessage const& batch : stalled_batches) {
        ASSERT_TRUE(client.acknowledgeDelivery(batch.delivery_token));
    }
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    std::vector<shared::ServerHeightTileBatchMessage> const resumed_batches = deliveryBatchesFrom(
        client.messages,
        resumed_message
    );
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    for (shared::ServerHeightTileBatchMessage const& batch : resumed_batches) {
        for (shared::ServerRemoveHeightTileMessage const& removal : batch.removals) {
            EXPECT_EQ(
                std::ranges::find(restored_interest.keys, removal.key),
                restored_interest.keys.end()
            );
        }
    }
}

TEST(GameServerPreviewTest, SaturatedAdditionsDoNotStarveRemovalsAndInflightReclaimsRecover)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{12};
    static constexpr uint32_t INITIAL_TILES = 1'024U;
    static constexpr uint32_t MAXIMUM_OUTSTANDING_BATCHES = 4U;
    static constexpr uint16_t FLIGHT_SPEEDUP = 500U;
    static constexpr uint32_t MOVEMENT_INPUT_COUNT = (
        shared::HEIGHT_TILE_INTEREST_RADIUS * shared::HEIGHT_TILE_SIDE_LENGTH * shared::SUBCELLS_PER_CELL
    ) / (shared::MOVEMENT_SUBCELLS_PER_TICK * FLIGHT_SPEEDUP) + 1U;
    static constexpr uint32_t FINAL_INPUT_SEQUENCE = 2U * MOVEMENT_INPUT_COUNT;
    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] { server.run(stop_requested); }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };
    PreviewClient client;
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable}));

    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.received_height_tiles < INITIAL_TILES
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_GE(client.received_height_tiles, INITIAL_TILES);

    client.setAcknowledgesDeliveries(false);
    size_t const stalled_message = client.messages.size();
    uint32_t const stalled_batch = deliveryBatchCount(client.messages);
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (deliveryBatchCount(client.messages) < stalled_batch + MAXIMUM_OUTSTANDING_BATCHES
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    std::vector<shared::ServerHeightTileBatchMessage> const stalled_batches = deliveryBatchesFrom(
        client.messages,
        stalled_message
    );
    ASSERT_EQ(stalled_batches.size(), MAXIMUM_OUTSTANDING_BATCHES);

    for (uint32_t sequence = 1U; sequence <= MOVEMENT_INPUT_COUNT; ++sequence) {
        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {.x = 127U, .accelerated = true, .speedup = FLIGHT_SPEEDUP},
            .sequence = sequence,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    }
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.last_acknowledged_input < MOVEMENT_INPUT_COUNT && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_EQ(client.last_acknowledged_input, MOVEMENT_INPUT_COUNT);
    size_t const outward_message = client.messages.size();
    ASSERT_TRUE(client.acknowledgeDelivery(stalled_batches.front().delivery_token));
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (deliveryBatchesFrom(client.messages, outward_message).empty()
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    std::vector<shared::ServerHeightTileBatchMessage> const outward_batches = deliveryBatchesFrom(
        client.messages,
        outward_message
    );
    ASSERT_EQ(outward_batches.size(), 1U);
    uint32_t outward_removals = 0U;
    for (shared::ServerHeightTileBatchMessage const& batch : outward_batches) {
        outward_removals += static_cast<uint32_t>(batch.removals.size());
    }
    ASSERT_GT(outward_removals, 0U);

    for (uint32_t sequence = MOVEMENT_INPUT_COUNT + 1U; sequence <= FINAL_INPUT_SEQUENCE; ++sequence) {
        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {
                .x = static_cast<uint8_t>(-127),
                .accelerated = true,
                .speedup = FLIGHT_SPEEDUP,
            },
            .sequence = sequence,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    }
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.last_acknowledged_input < FINAL_INPUT_SEQUENCE && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_EQ(client.last_acknowledged_input, FINAL_INPUT_SEQUENCE);
    shared::HeightTileInterest const restored_interest = shared::makeHeightTileInterest({
        .x = client.last_player_x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        .y = shared::World::FLIGHT_SPAWN.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
    }, -127, 0);
    std::vector<shared::HeightTileKey> reclaimed_keys;
    for (shared::ServerHeightTileBatchMessage const& batch : outward_batches) {
        for (shared::ServerRemoveHeightTileMessage const& removal : batch.removals) {
            if (std::ranges::find(restored_interest.keys, removal.key) != restored_interest.keys.end()) {
                reclaimed_keys.push_back(removal.key);
            }
        }
    }
    ASSERT_FALSE(reclaimed_keys.empty());

    // Let the restored-interest cursor pass resident keys before the already-sent removal is acknowledged.
    auto const reentry_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{250};
    while (std::chrono::steady_clock::now() < reentry_deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    size_t const recovery_message = client.messages.size();
    client.setAcknowledgesDeliveries(true);
    for (auto batch = std::next(stalled_batches.begin()); batch != stalled_batches.end(); ++batch) {
        ASSERT_TRUE(client.acknowledgeDelivery(batch->delivery_token));
    }
    for (shared::ServerHeightTileBatchMessage const& batch : outward_batches) {
        ASSERT_TRUE(client.acknowledgeDelivery(batch.delivery_token));
    }
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    std::vector<shared::HeightTileKey> recovered;
    while (std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
        recovered = heightTileKeysFrom(client.messages, recovery_message);
        if (std::ranges::all_of(reclaimed_keys, [&recovered](shared::HeightTileKey const key) {
                return std::ranges::find(recovered, key) != recovered.end();
            })) {
            break;
        }
    }
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    for (shared::HeightTileKey const key : reclaimed_keys) {
        EXPECT_NE(std::ranges::find(recovered, key), recovered.end());
    }
}

TEST(GameServerPreviewTest, SustainedFlightKeepsInputAcknowledgementsCurrentWhileTilesStream)
{
#ifndef NDEBUG
    GTEST_SKIP() << "Four-peer full-radius latency acceptance requires an optimized Release build";
#else
    static constexpr auto DURATION = std::chrono::seconds{35};
    static constexpr auto INPUT_PERIOD = std::chrono::milliseconds{100};
    static constexpr auto PROGRESS_INTERVAL = std::chrono::seconds{1};
    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    struct ClientProgress final {
        core::ClientId client_id = 0U;
        std::chrono::steady_clock::time_point next_log{};
        bool active = false;
    };
    std::array<ClientProgress, 4> server_progress{};
    auto const server_started = std::chrono::steady_clock::now();
    server::GameServer::BenchmarkHooks const hooks{
        .on_preview_metrics = [server_started, &server_progress](
            core::ClientId const client_id,
            server::GameServer::PreviewStreamMetrics const metrics
        ) {
            auto const progress = std::ranges::find(server_progress, client_id, &ClientProgress::client_id);
            auto const slot = progress != server_progress.end()
                ? progress
                : std::ranges::find(server_progress, false, &ClientProgress::active);
            if (slot == server_progress.end()) {
                return;
            }
            if (!slot->active) {
                slot->active = true;
                slot->client_id = client_id;
            }
            auto const now = std::chrono::steady_clock::now();
            if (now < slot->next_log) {
                return;
            }
            slot->next_log = now + PROGRESS_INTERVAL;
            auto const average_ms = [](
                std::chrono::nanoseconds const total,
                uint64_t const samples
            ) {
                return samples == 0U
                    ? int64_t{0}
                    : total.count() / static_cast<int64_t>(samples) / 1'000'000;
            };
            CORE_INFO(
                "Flight server client {} at {} ms: input received/ack/unacked/pending/materialization-failures="
                "{}/{}/{}/{}/{}, jobs pending/queued/running/finished-uncollected={}/{}/{}/{}, "
                "tile jobs submitted/started/finished/collected={}/{}/{}/{}, "
                "enqueue-submit/executor/run/collect avg/max ms={}/{}/{}/{}/{}/{}/{}/{}, "
                "world jobs submitted/started/finished/collected={}/{}/{}/{}, "
                "enqueue-submit/executor/run/collect avg/max ms={}/{}/{}/{}/{}/{}/{}/{}, "
                "ready/inflight/credits={}/{}/{}, "
                "credit samples/avg/max ms={}/{}/{}, loop interval/work/tick/pump/poll ms={}/{}/{}/{}/{}",
                client_id,
                std::chrono::duration_cast<std::chrono::milliseconds>(now - server_started).count(),
                metrics.latest_received_input_sequence,
                metrics.acknowledged_input_sequence,
                metrics.unacknowledged_input_count,
                metrics.pending_input_count,
                metrics.materialization_admission_failures,
                metrics.pending_worker_jobs,
                metrics.height_tile_jobs.executor_queued_jobs + metrics.world_generation_jobs.executor_queued_jobs,
                metrics.height_tile_jobs.running_jobs + metrics.world_generation_jobs.running_jobs,
                metrics.height_tile_jobs.completed_uncollected_jobs
                    + metrics.world_generation_jobs.completed_uncollected_jobs,
                metrics.height_tile_jobs.submitted_total,
                metrics.height_tile_jobs.started_total,
                metrics.height_tile_jobs.finished_total,
                metrics.height_tile_jobs.collected_total,
                average_ms(
                    metrics.height_tile_jobs.enqueue_to_submit_total,
                    metrics.height_tile_jobs.submitted_total
                ),
                metrics.height_tile_jobs.enqueue_to_submit_max.count() / 1'000'000,
                average_ms(metrics.height_tile_jobs.executor_queue_total, metrics.height_tile_jobs.started_total),
                metrics.height_tile_jobs.executor_queue_max.count() / 1'000'000,
                average_ms(metrics.height_tile_jobs.execution_total, metrics.height_tile_jobs.finished_total),
                metrics.height_tile_jobs.execution_max.count() / 1'000'000,
                average_ms(
                    metrics.height_tile_jobs.completion_to_collection_total,
                    metrics.height_tile_jobs.collected_total
                ),
                metrics.height_tile_jobs.completion_to_collection_max.count() / 1'000'000,
                metrics.world_generation_jobs.submitted_total,
                metrics.world_generation_jobs.started_total,
                metrics.world_generation_jobs.finished_total,
                metrics.world_generation_jobs.collected_total,
                average_ms(
                    metrics.world_generation_jobs.enqueue_to_submit_total,
                    metrics.world_generation_jobs.submitted_total
                ),
                metrics.world_generation_jobs.enqueue_to_submit_max.count() / 1'000'000,
                average_ms(
                    metrics.world_generation_jobs.executor_queue_total,
                    metrics.world_generation_jobs.started_total
                ),
                metrics.world_generation_jobs.executor_queue_max.count() / 1'000'000,
                average_ms(metrics.world_generation_jobs.execution_total, metrics.world_generation_jobs.finished_total),
                metrics.world_generation_jobs.execution_max.count() / 1'000'000,
                average_ms(
                    metrics.world_generation_jobs.completion_to_collection_total,
                    metrics.world_generation_jobs.collected_total
                ),
                metrics.world_generation_jobs.completion_to_collection_max.count() / 1'000'000,
                metrics.ready_tiles,
                metrics.inflight_deliveries,
                metrics.delivery_credits,
                metrics.delivery_credit_samples,
                average_ms(metrics.delivery_credit_total, metrics.delivery_credit_samples),
                metrics.delivery_credit_max.count() / 1'000'000,
                metrics.server_loop_interval.count() / 1'000'000,
                metrics.server_loop_work.count() / 1'000'000,
                metrics.server_tick.count() / 1'000'000,
                metrics.stream_pump.count() / 1'000'000,
                metrics.network_poll.count() / 1'000'000
            );
        },
    };
    std::thread server_thread{[&server, &stop_requested, &hooks] {
        server.run(stop_requested, &hooks);
    }};
    PreviewClient first_client{true, true, '@', false};
    PreviewClient second_client{true, true, '#', false};
    PreviewClient third_client{true, true, '$', false};
    PreviewClient fourth_client{true, true, '%', false};
    std::array<PreviewClient*, 4> const clients{
        &first_client, &second_client, &third_client, &fourth_client,
    };
    std::array<char, 4> const characters{'@', '#', '$', '%'};
    bool connected = true;
    bool joined = true;
    for (size_t index = 0U; index < clients.size(); ++index) {
        connected = clients[index]->connect(
            core::Address::localhost(server.port()), std::chrono::seconds{2}
        ) && connected;
        joined = clients[index]->send(shared::encodeMessage(shared::JoinRequestMessage{
            .ch = characters[index], .mode = shared::WorldMode::Flight, .wants_previews = true,
        }), 0, core::SendMode{core::SendMode::Reliable}) && joined;
    }
    joined = connected && joined;
    uint32_t sent = 0U;
    uint32_t maximum_ack_lag = 0U;
    uint32_t sent_at_maximum_ack_lag = 0U;
    std::array<size_t, 4> midpoint_tile_counts{};
    bool midpoint_recorded = false;
    bool all_sent = true;
    auto const started = std::chrono::steady_clock::now();
    auto next_client_progress = started + PROGRESS_INTERVAL;
    auto next_input = started;
    while (joined && std::chrono::steady_clock::now() - started < DURATION) {
        auto const now = std::chrono::steady_clock::now();
        if (now >= next_input) {
            ++sent;
            std::vector<uint8_t> const input = shared::encodeMessage(shared::ClientInputMessage{
                .direction = {.x = 127U, .accelerated = true, .speedup = 500U},
                .sequence = sent,
            });
            for (PreviewClient* const client : clients) {
                client->trackInputSent(sent);
                all_sent = client->send(input, 0, core::SendMode{core::SendMode::Reliable}) && all_sent;
            }
            next_input += INPUT_PERIOD;
        }
        for (PreviewClient* const client : clients) {
            while (client->poll(std::chrono::milliseconds::zero()) > 0) {
            }
        }
        if (now >= next_client_progress) {
            for (size_t index = 0U; index < clients.size(); ++index) {
                PreviewClient const* const client = clients[index];
                uint64_t const latency_samples = client->acknowledgement_latency_samples;
                CORE_INFO(
                    "Flight client {} sent/ack/lag={}/{}/{}, ack latency samples/avg/max ms={}/{}/{}, "
                    "received tiles={}",
                    index,
                    sent,
                    client->last_acknowledged_input,
                    sent - client->last_acknowledged_input,
                    latency_samples,
                    latency_samples == 0U
                        ? 0U
                        : client->acknowledgement_latency_total_ns / latency_samples / 1'000'000U,
                    client->acknowledgement_latency_max_ns / 1'000'000U,
                    client->received_height_tiles
                );
            }
            next_client_progress = now + PROGRESS_INTERVAL;
        }
        if (!midpoint_recorded && now - started >= DURATION / 2) {
            for (size_t index = 0U; index < clients.size(); ++index) {
                midpoint_tile_counts[index] = clients[index]->received_height_tiles;
            }
            midpoint_recorded = true;
        }
        uint32_t lag = 0U;
        for (PreviewClient const* const client : clients) {
            lag = std::max(lag, sent - client->last_acknowledged_input);
        }
        if (lag > maximum_ack_lag) {
            maximum_ack_lag = lag;
            sent_at_maximum_ack_lag = sent;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{16});
    }
    auto const catch_up_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    auto const all_caught_up = [&clients, sent] {
        return std::ranges::all_of(clients, [sent](PreviewClient const* const client) {
            return sent - client->last_acknowledged_input <= 2U;
        });
    };
    while (!all_caught_up() && std::chrono::steady_clock::now() < catch_up_deadline) {
        for (PreviewClient* const client : clients) {
            client->poll(std::chrono::milliseconds{1});
        }
    }
    CORE_INFO(
        "Flight summary sent={}, maximum ack lag={} at input={}, final acknowledgements={}/{}/{}/{}, duration={} ms",
        sent,
        maximum_ack_lag,
        sent_at_maximum_ack_lag,
        first_client.last_acknowledged_input,
        second_client.last_acknowledged_input,
        third_client.last_acknowledged_input,
        fourth_client.last_acknowledged_input,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started
        ).count()
    );
    for (size_t index = 0U; index < clients.size(); ++index) {
        PreviewClient const* const client = clients[index];
        uint64_t const latency_samples = client->acknowledgement_latency_samples;
        CORE_INFO(
            "Flight result client {} ack latency samples/avg/max ms={}/{}/{}, received tiles={}",
            index,
            latency_samples,
            latency_samples == 0U
                ? 0U
                : client->acknowledgement_latency_total_ns / latency_samples / 1'000'000U,
            client->acknowledgement_latency_max_ns / 1'000'000U,
            client->received_height_tiles
        );
    }
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(joined);
    EXPECT_TRUE(all_sent);
    ASSERT_TRUE(midpoint_recorded);
    EXPECT_GE(sent, 340U);
    // Prediction remains continuous while acknowledgements trail. Keep the worst
    // terrain-loaded lag far below the 64-input safety bound and require catch-up.
    EXPECT_LE(maximum_ack_lag, 16U)
        << "maximum lag occurred after input " << sent_at_maximum_ack_lag
        << ", last acknowledgements " << first_client.last_acknowledged_input << ", "
        << second_client.last_acknowledged_input << ", " << third_client.last_acknowledged_input
        << ", and " << fourth_client.last_acknowledged_input;
    for (size_t index = 0U; index < clients.size(); ++index) {
        PreviewClient const* const client = clients[index];
        EXPECT_LE(sent - client->last_acknowledged_input, 2U);
        EXPECT_GT(client->acknowledgement_latency_samples, 0U)
            << "client " << index << " produced no input acknowledgement latency samples";
        EXPECT_TRUE(client->messages.empty());
        EXPECT_GT(client->received_delivery_batches, 0U);
        EXPECT_GT(client->received_height_tiles, midpoint_tile_counts[index])
            << "client " << index << " made no terrain progress during the second half";
    }
#endif
}
