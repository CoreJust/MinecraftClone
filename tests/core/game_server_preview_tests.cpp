#include <server/GameServer.hpp>

#include <shared/net/Message.hpp>
#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/World.hpp>

#include <core/common/Defer.hpp>
#include <core/IO/Log.hpp>
#include <core/net/Address.hpp>
#include <core/net/Client.hpp>
#include <core/net/Event.hpp>
#include <core/net/SendMode.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <variant>
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
    std::optional<shared::ServerPlayerPositionMessage> last_player_position;
    std::optional<bool> join_accepted;
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

    void reportFirstDeliveryTileAsEvicted() noexcept
    {
        m_report_first_delivery_tile_as_evicted = true;
    }

    bool acknowledgeDelivery(
        uint64_t const delivery_token,
        std::vector<shared::HeightTileKey> not_resident_keys = {}
    )
    {
        return send(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
            .world_revision = 1U,
            .delivery_token = delivery_token,
            .credits = 1U,
            .not_resident_keys = std::move(not_resident_keys),
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
        if (auto const* const join_response = std::get_if<shared::JoinResponseMessage>(&*message)) {
            join_accepted = join_response->accepted;
        } else if (auto const* const tile_message = std::get_if<shared::ServerHeightTileMessage>(&*message)) {
            recordHeightTile(*tile_message);
        } else if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&*message)) {
            ++received_delivery_batches;
            received_removals += static_cast<uint32_t>(batch->removals.size());
            for (shared::ServerHeightTileMessage const& batch_tile : batch->tiles) {
                recordHeightTile(batch_tile);
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
                .not_resident_keys = {},
            }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable})) {
                ++sent_delivery_credit_messages;
            }
        } else if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&*message);
            batch != nullptr && m_acknowledges_deliveries) {
            std::vector<shared::HeightTileKey> not_resident_keys;
            if (m_report_first_delivery_tile_as_evicted && !batch->tiles.empty()) {
                not_resident_keys.push_back(batch->tiles.front().key);
                m_report_first_delivery_tile_as_evicted = false;
            }
            if (send(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
                .world_revision = 1U,
                .delivery_token = batch->delivery_token,
                .credits = 1U,
                .not_resident_keys = std::move(not_resident_keys),
            }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable})) {
                ++sent_delivery_credit_messages;
            }
        }
        if (auto const* position = std::get_if<shared::ServerPlayerPositionMessage>(&*message);
            position != nullptr && position->ch == m_tracked_character) {
            last_player_position = *position;
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
    bool m_report_first_delivery_tile_as_evicted = false;
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

bool waitForStalledDeliveryWindow(PreviewClient& client, uint32_t const first_message = 0U)
{
    static constexpr std::chrono::seconds TIMEOUT{ 2 };
    static constexpr std::chrono::milliseconds STARTUP_DURATION{ 500 };
    static constexpr std::chrono::milliseconds QUIET_DURATION{ 100 };
    static constexpr std::chrono::milliseconds POLL_INTERVAL{ 1 };
    auto const started_at = std::chrono::steady_clock::now();
    auto last_delivery_at = started_at;
    uint32_t previous_count = 0U;
    while (std::chrono::steady_clock::now() - started_at < TIMEOUT) {
        client.poll(POLL_INTERVAL);
        uint32_t const count = static_cast<uint32_t>(std::ranges::count_if(
            std::span<shared::Message const>{ client.messages }.subspan(first_message),
            [](shared::Message const& message) {
                return std::holds_alternative<shared::ServerHeightTileBatchMessage>(message);
            }
        ));
        auto const now = std::chrono::steady_clock::now();
        if (count != previous_count) {
            previous_count = count;
            last_delivery_at = now;
        }
        if (count > 0U && now - started_at >= STARTUP_DURATION && now - last_delivery_at >= QUIET_DURATION) {
            return true;
        }
    }
    return false;
}

void expectBoundedDeliveryWindow(std::vector<shared::ServerHeightTileBatchMessage> const& batches)
{
    static constexpr uint32_t MAXIMUM_BATCHES = 8U;
    static constexpr uint32_t MAXIMUM_PACKET_BYTES = 8'589U;
    static constexpr uint32_t MAXIMUM_WINDOW_BYTES = 34'356U;
    EXPECT_GE(batches.size(), 1U);
    EXPECT_LE(batches.size(), MAXIMUM_BATCHES);
    uint64_t encoded_bytes = 0U;
    for (shared::ServerHeightTileBatchMessage const& batch : batches) {
        uint64_t const packet_bytes = shared::encodeMessage(batch).size();
        EXPECT_LE(packet_bytes, MAXIMUM_PACKET_BYTES);
        encoded_bytes += packet_bytes;
    }
    EXPECT_LE(encoded_bytes, MAXIMUM_WINDOW_BYTES);
}

void expectAcknowledgedDeliveryWindow(
    std::vector<shared::ServerHeightTileBatchMessage> batches,
    std::vector<uint64_t> const& acknowledged_tokens
)
{
    static constexpr uint32_t INITIAL_CREDITS = 8U;
    EXPECT_LE(batches.size(), INITIAL_CREDITS + acknowledged_tokens.size());
    std::erase_if(batches, [&](shared::ServerHeightTileBatchMessage const& batch) {
        return std::ranges::find(acknowledged_tokens, batch.delivery_token) != acknowledged_tokens.end();
    });
    expectBoundedDeliveryWindow(batches);
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
                for (shared::ServerHeightTileMessage const& batch_tile : batch->tiles) {
                    inspect_tile(batch_tile);
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
            .direction = {.x = 0U, .y = 0U, .cycle_movement_capabilities = cycle}, .sequence = sequence++,
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

TEST(GameServerPreviewTest, StreamsCompleteCurrentNearSquareBeforeFartherTiles)
{
    static constexpr std::chrono::seconds TIMEOUT{4};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr int32_t CURRENT_NEAR_RADIUS = 7;
    static constexpr uint32_t OBSERVED_TILE_COUNT = static_cast<uint32_t>(
        (CURRENT_NEAR_RADIUS * 2 + 1) * (CURRENT_NEAR_RADIUS * 2 + 1)
    );

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
        EXPECT_LE(distance, CURRENT_NEAR_RADIUS);
        if (std::ranges::find(rows, key.y) == rows.end()) {
            rows.push_back(key.y);
        }
    }
    EXPECT_GT(rows.size(), 1U);
}

TEST(GameServerPreviewTest, FlightPrewarmsTheNextNearFootprintBeforeRadialTiles)
{
    static constexpr std::chrono::seconds TIMEOUT{20};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr std::array<uint32_t, 2U> RENDER_DISTANCES{16U, 10U};
    static constexpr int32_t NEAR_RADIUS = 7;
    static constexpr uint32_t NEAR_TILE_COUNT = static_cast<uint32_t>(
        (NEAR_RADIUS * 2 + 1) * (NEAR_RADIUS * 2 + 1)
    );
    static constexpr shared::PlayerId FORECAST_PLAYER_ID = 1U;
    static constexpr shared::Direction MOVEMENT{
        .x = 127U,
        .y = 0U,
        .accelerated = true,
        .speedup = 200U,
    };

    for (uint32_t const render_distance : RENDER_DISTANCES) {
        SCOPED_TRACE(render_distance);
        uint32_t const initial_tile_count = shared::heightTileInterestCount(render_distance);
        std::atomic_uint32_t resident_tiles{0U};
        std::atomic_uint32_t inflight_deliveries{0U};
        server::GameServer::BenchmarkHooks const hooks{
            .on_tick = {},
            .on_preview_buffered = {},
            .on_preview_metrics = [&resident_tiles, &inflight_deliveries](
                core::ClientId,
                server::GameServer::PreviewStreamMetrics const metrics
            ) {
                resident_tiles.store(metrics.resident_tiles, std::memory_order_relaxed);
                inflight_deliveries.store(metrics.inflight_deliveries, std::memory_order_relaxed);
            },
            .before_height_tile_completion = {},
        };
        server::GameServer server{
            0,
            {},
            shared::WorldMode::Flight,
            shared::World::canonicalConfiguration(),
            render_distance,
        };
        std::atomic_bool stop_requested{false};
        std::thread server_thread{[&server, &stop_requested, &hooks] {
            server.run(stop_requested, &hooks);
        }};
        defer {
            stop_requested.store(true, std::memory_order_relaxed);
            if (server_thread.joinable()) {
                server_thread.join();
            }
        };

        PreviewClient client{true, true, '@', false};
        ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
        ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
            .ch = '@',
            .mode = shared::WorldMode::Flight,
            .wants_previews = true,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
        auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
        auto const poll_until = [&client](auto const& predicate, auto const stop_at) {
            while (!predicate() && std::chrono::steady_clock::now() < stop_at) {
                client.poll(POLL_INTERVAL);
            }
            return predicate();
        };
        ASSERT_TRUE(poll_until([&] {
            return client.received_height_tile_descriptors == 1U
                && resident_tiles.load(std::memory_order_relaxed) == initial_tile_count
                && inflight_deliveries.load(std::memory_order_relaxed) == 0U;
        }, deadline));
        uint32_t const initial_received_key_count = static_cast<uint32_t>(client.received_height_tile_keys.size());

        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = MOVEMENT,
            .sequence = 1U,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
        ASSERT_TRUE(poll_until([&] {
            return client.last_acknowledged_input == 1U && client.last_player_position.has_value();
        }, deadline));
        shared::ServerPlayerPositionMessage const first_position = *client.last_player_position;
        shared::HeightTileKey const current_center = shared::normalizeHeightTileKey({
            .x = first_position.x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
            .y = first_position.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        });

        shared::World forecast_world{shared::WorldMode::Flight};
        forecast_world.spawnPlayer(FORECAST_PLAYER_ID, '@', shared::PlayerPosition{
            .x = first_position.x,
            .y = first_position.y,
            .z = first_position.z,
            .x_subcell = first_position.x_subcell,
            .y_subcell = first_position.y_subcell,
            .z_subcell = first_position.z_subcell,
        });
        ASSERT_TRUE(forecast_world.setPlayerMovementCapabilities(
            FORECAST_PLAYER_ID,
            first_position.movement_capabilities
        ));
        std::optional<shared::PlayerPosition> const forecast_position = forecast_world.forecastFlightPlayerPosition(
            FORECAST_PLAYER_ID,
            MOVEMENT
        );
        ASSERT_TRUE(forecast_position.has_value());
        shared::HeightTileKey const forecast_center = shared::normalizeHeightTileKey({
            .x = forecast_position->x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
            .y = forecast_position->y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        });

        std::vector<shared::HeightTileKey> current_near_keys;
        std::vector<shared::HeightTileKey> forecast_keys;
        current_near_keys.reserve(NEAR_TILE_COUNT);
        forecast_keys.reserve(NEAR_TILE_COUNT);
        for (int32_t y = -NEAR_RADIUS; y <= NEAR_RADIUS; ++y) {
            for (int32_t x = -NEAR_RADIUS; x <= NEAR_RADIUS; ++x) {
                shared::HeightTileKey const current_key = shared::normalizeHeightTileKey({
                    .x = current_center.x + x,
                    .y = current_center.y + y,
                });
                if (shared::heightTileWithinInterest(current_center, current_key, render_distance)) {
                    current_near_keys.push_back(current_key);
                }
                shared::HeightTileKey const forecast_key = shared::normalizeHeightTileKey({
                    .x = forecast_center.x + x,
                    .y = forecast_center.y + y,
                });
                if (shared::heightTileWithinInterest(current_center, forecast_key, render_distance)) {
                    forecast_keys.push_back(forecast_key);
                }
            }
        }
        ASSERT_GT(forecast_keys.size(), 0U);
        if (render_distance == 16U) {
            ASSERT_EQ(current_near_keys.size(), NEAR_TILE_COUNT);
            ASSERT_EQ(forecast_keys.size(), NEAR_TILE_COUNT);
        } else {
            ASSERT_EQ(current_near_keys.size(), NEAR_TILE_COUNT);
            ASSERT_LT(forecast_keys.size(), NEAR_TILE_COUNT);
        }
        std::vector<shared::HeightTileKey> forecast_missing_before_move;
        for (shared::HeightTileKey const key : forecast_keys) {
            if (std::ranges::find(
                    client.received_height_tile_keys.begin(),
                    client.received_height_tile_keys.begin() + initial_received_key_count,
                    key
                ) == client.received_height_tile_keys.begin() + initial_received_key_count) {
                forecast_missing_before_move.push_back(key);
            }
        }
        ASSERT_FALSE(forecast_missing_before_move.empty());

        auto const has_received = [&client](shared::HeightTileKey const key) {
            return std::ranges::find(client.received_height_tile_keys, key)
                != client.received_height_tile_keys.end();
        };
        auto const forecast_is_received = [&forecast_keys, &has_received] {
            return std::ranges::all_of(forecast_keys, has_received);
        };
        auto const all_missing_forecast_received_before = [&client, &forecast_missing_before_move](
            uint32_t const end
        ) {
            auto const prefix_end = client.received_height_tile_keys.begin() + end;
            return std::ranges::all_of(
                forecast_missing_before_move,
                [&client, prefix_end](shared::HeightTileKey const key) {
                    return std::ranges::find(client.received_height_tile_keys.begin(), prefix_end, key) != prefix_end;
                }
            );
        };
        uint32_t inspected_received_keys = initial_received_key_count;
        bool radial_arrived_before_forecast = false;
        auto const inspect_new_deliveries = [&] {
            while (inspected_received_keys < client.received_height_tile_keys.size()) {
                shared::HeightTileKey const key = client.received_height_tile_keys[inspected_received_keys];
                bool const protected_key = std::ranges::find(current_near_keys, key) != current_near_keys.end()
                    || std::ranges::find(forecast_keys, key) != forecast_keys.end();
                if (!protected_key) {
                    radial_arrived_before_forecast |= !all_missing_forecast_received_before(
                        inspected_received_keys
                    );
                }
                ++inspected_received_keys;
            }
        };
        while ((!forecast_is_received() || inflight_deliveries.load(std::memory_order_relaxed) != 0U)
            && std::chrono::steady_clock::now() < deadline) {
            client.poll(POLL_INTERVAL);
            inspect_new_deliveries();
        }
        inspect_new_deliveries();
        EXPECT_FALSE(radial_arrived_before_forecast);
        ASSERT_TRUE(forecast_is_received());
        ASSERT_EQ(inflight_deliveries.load(std::memory_order_relaxed), 0U);

        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = MOVEMENT,
            .sequence = 2U,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
        ASSERT_TRUE(poll_until([&] {
            return client.last_acknowledged_input == 2U
                && client.last_player_position.has_value()
                && client.last_player_position->acknowledged_input_sequence == 2U;
        }, deadline));
        shared::HeightTileKey const actual_second_center = shared::normalizeHeightTileKey({
            .x = client.last_player_position->x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
            .y = client.last_player_position->y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        });
        EXPECT_EQ(actual_second_center, forecast_center);
    }
}

TEST(GameServerPreviewTest, ProgressiveStreamStaysInsideTheNearbyRadialFrontier)
{
    static constexpr std::chrono::seconds TIMEOUT{5};
    static constexpr uint32_t OBSERVED_TILE_COUNT = 512U;
    static constexpr int32_t MAXIMUM_OBSERVED_RADIUS = 20;
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
    int64_t maximum_distance_squared = 0;
    std::vector<int32_t> rows;
    for (uint32_t index = 0U; index < OBSERVED_TILE_COUNT; ++index) {
        shared::HeightTileKey const key = keys[index];
        int64_t const delta_x = key.x - center_x;
        int64_t const delta_y = key.y - center_y;
        maximum_distance_squared = std::max(
            maximum_distance_squared,
            delta_x * delta_x + delta_y * delta_y
        );
        if (std::ranges::find(rows, key.y) == rows.end()) {
            rows.push_back(key.y);
        }
    }
    EXPECT_LE(maximum_distance_squared, MAXIMUM_OBSERVED_RADIUS * MAXIMUM_OBSERVED_RADIUS);
    EXPECT_GT(rows.size(), 20U);
}

TEST(GameServerPreviewTest, ResetsPriorityCursorAfterProgressWhenHeadingChanges)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{10};
    static constexpr auto POLL_INTERVAL = std::chrono::milliseconds{1};
    static constexpr uint32_t NORTH_SETUP_SEQUENCE = 1U;
    static constexpr uint32_t EAST_HEADING_SEQUENCE = 2U;
    static constexpr uint32_t PRIORITY_SCAN_PROGRESS_THRESHOLD = 512U;
    static constexpr uint32_t PRIORITY_SCAN_RESET_ADVANCE_LIMIT = 512U;
    static constexpr uint32_t PRIORITY_SCAN_WRAP_GUARD = 512U;
    static constexpr uint32_t PRIORITY_PREFIX_TILE_COUNT = 256U;

    std::atomic_bool north_setup_metrics_captured{false};
    std::atomic_bool north_cursor_captured{false};
    std::atomic_bool priority_prefixes_ready{false};
    std::atomic_uint32_t priority_cursor_before_rotation{0U};
    std::atomic_int32_t center_x_before_rotation{0};
    std::atomic_int32_t center_y_before_rotation{0};
    std::atomic_bool east_heading_metrics_captured{false};
    std::atomic_uint32_t priority_cursor_after_rotation{0U};
    std::atomic_int32_t center_x_after_rotation{0};
    std::atomic_int32_t center_y_after_rotation{0};
    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = {},
        .on_preview_buffered = {},
        .on_preview_metrics = [
            &north_setup_metrics_captured,
            &north_cursor_captured,
            &priority_prefixes_ready,
            &priority_cursor_before_rotation,
            &center_x_before_rotation,
            &center_y_before_rotation,
            &east_heading_metrics_captured,
            &priority_cursor_after_rotation,
            &center_x_after_rotation,
            &center_y_after_rotation
        ](core::ClientId, server::GameServer::PreviewStreamMetrics const metrics) {
            if (!metrics.terrain_center_valid) {
                return;
            }
            if (metrics.acknowledged_input_sequence == NORTH_SETUP_SEQUENCE
                && !north_setup_metrics_captured.load(std::memory_order_relaxed)) {
                center_x_before_rotation.store(metrics.terrain_center_x, std::memory_order_relaxed);
                center_y_before_rotation.store(metrics.terrain_center_y, std::memory_order_relaxed);
                north_setup_metrics_captured.store(true, std::memory_order_release);
            }
            if (metrics.acknowledged_input_sequence == NORTH_SETUP_SEQUENCE
                && priority_prefixes_ready.load(std::memory_order_acquire)
                && metrics.priority_scan_cursor > PRIORITY_SCAN_PROGRESS_THRESHOLD
                && metrics.priority_scan_cursor
                    < shared::HEIGHT_TILE_INTEREST_COUNT - PRIORITY_SCAN_WRAP_GUARD
                && !north_cursor_captured.load(std::memory_order_relaxed)) {
                priority_cursor_before_rotation.store(metrics.priority_scan_cursor, std::memory_order_relaxed);
                north_cursor_captured.store(true, std::memory_order_release);
            }
            if (metrics.acknowledged_input_sequence == EAST_HEADING_SEQUENCE
                && !east_heading_metrics_captured.load(std::memory_order_relaxed)) {
                priority_cursor_after_rotation.store(metrics.priority_scan_cursor, std::memory_order_relaxed);
                center_x_after_rotation.store(metrics.terrain_center_x, std::memory_order_relaxed);
                center_y_after_rotation.store(metrics.terrain_center_y, std::memory_order_relaxed);
                east_heading_metrics_captured.store(true, std::memory_order_release);
            }
        },
        .before_height_tile_completion = {},
    };
    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested, &hooks] { server.run(stop_requested, &hooks); }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };
    PreviewClient client;
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable}));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {.x = 0U, .y = 0U, .z = 0U, .view_x = 0, .view_y = 127},
        .sequence = NORTH_SETUP_SEQUENCE,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    auto const north_progress_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while ((!north_setup_metrics_captured.load(std::memory_order_acquire)
            || client.last_acknowledged_input < NORTH_SETUP_SEQUENCE)
        && std::chrono::steady_clock::now() < north_progress_deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_TRUE(north_setup_metrics_captured.load(std::memory_order_acquire));
    ASSERT_EQ(client.last_acknowledged_input, NORTH_SETUP_SEQUENCE);

    int32_t const center_x = center_x_before_rotation.load(std::memory_order_relaxed);
    int32_t const center_y = center_y_before_rotation.load(std::memory_order_relaxed);
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
    auto const is_current_near_key = [center_x, center_y](shared::HeightTileKey const key) {
        return shared::heightTileGenerationBand({.x = center_x, .y = center_y}, 0, 127, key)
            <= shared::HeightTileGenerationBand::Near;
    };
    auto const east_priority_prefix = std::span{east_interest.keys}.first(PRIORITY_PREFIX_TILE_COUNT);
    auto const north_priority_prefix = std::span{north_interest.keys}.first(PRIORITY_PREFIX_TILE_COUNT);
    auto const east_frontier_candidate = std::ranges::find_if(
        east_priority_prefix,
        [&north_priority_prefix](shared::HeightTileKey const key) {
            return std::ranges::find(north_priority_prefix, key) == north_priority_prefix.end();
        }
    );
    bool const has_east_frontier_candidate = east_frontier_candidate != east_priority_prefix.end();
    bool const east_frontier_is_outside_near = has_east_frontier_candidate
        && !is_current_near_key(*east_frontier_candidate);
    ASSERT_TRUE(has_east_frontier_candidate);
    EXPECT_TRUE(east_frontier_is_outside_near);
    priority_prefixes_ready.store(true, std::memory_order_release);

    while (!north_cursor_captured.load(std::memory_order_acquire)
        && std::chrono::steady_clock::now() < north_progress_deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_TRUE(north_cursor_captured.load(std::memory_order_acquire));
    uint32_t const cursor_before_rotation = priority_cursor_before_rotation.load(std::memory_order_relaxed);
    ASSERT_GT(cursor_before_rotation, PRIORITY_SCAN_PROGRESS_THRESHOLD);
    ASSERT_LT(
        cursor_before_rotation,
        shared::HEIGHT_TILE_INTEREST_COUNT - PRIORITY_SCAN_WRAP_GUARD
    );

    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {.x = 0U, .y = 0U, .z = 0U, .view_x = 127, .view_y = 0},
        .sequence = EAST_HEADING_SEQUENCE,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    auto const east_heading_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while ((!east_heading_metrics_captured.load(std::memory_order_acquire)
            || client.last_acknowledged_input < EAST_HEADING_SEQUENCE)
        && std::chrono::steady_clock::now() < east_heading_deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_TRUE(east_heading_metrics_captured.load(std::memory_order_acquire));
    EXPECT_EQ(client.last_acknowledged_input, EAST_HEADING_SEQUENCE);
    EXPECT_EQ(center_x_after_rotation.load(std::memory_order_relaxed), center_x);
    EXPECT_EQ(center_y_after_rotation.load(std::memory_order_relaxed), center_y);
    uint32_t const cursor_after_rotation = priority_cursor_after_rotation.load(std::memory_order_relaxed);
    // Interest refresh and the enclosing stream pump each inspect at most 256 priority candidates.
    EXPECT_LE(cursor_after_rotation, PRIORITY_SCAN_RESET_ADVANCE_LIMIT);
    EXPECT_LT(cursor_after_rotation, cursor_before_rotation);
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
        .direction = {.x = 0U, .y = 0U, .view_x = 127, .view_y = 0},
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
            .direction = {.x = 127U, .y = 0U, .accelerated = true, .speedup = FLIGHT_SPEEDUP, .view_x = 127},
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
    std::atomic_uint32_t near_stage_sample_count{0U};
    std::atomic_bool near_stage_partition_valid{true};
    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = {},
        .on_preview_buffered = {},
        .on_preview_metrics = [server_started_at, &next_server_progress, &near_stage_sample_count,
            &near_stage_partition_valid](
            core::ClientId,
            server::GameServer::PreviewStreamMetrics const metrics
        ) {
            if (metrics.terrain_center_valid) {
                uint32_t const classified = metrics.near_window_resident_tiles
                    + metrics.near_window_queued_tiles
                    + metrics.near_window_dispatched_tiles
                    + metrics.near_window_ready_tiles
                    + metrics.near_window_inflight_additions
                    + metrics.near_window_untracked_tiles;
                if (metrics.near_window_desired_tiles != 225U || classified != metrics.near_window_desired_tiles) {
                    near_stage_partition_valid.store(false, std::memory_order_relaxed);
                }
                near_stage_sample_count.fetch_add(1U, std::memory_order_relaxed);
            }
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
        .before_height_tile_completion = {},
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
            .direction = {.x = 127U, .y = 0U},
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
    EXPECT_GT(near_stage_sample_count.load(std::memory_order_relaxed), 0U);
    EXPECT_TRUE(near_stage_partition_valid.load(std::memory_order_relaxed));
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
            .direction = {.x = 127U, .y = 0U, .accelerated = true, .speedup = 500U},
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
            .not_resident_keys = {},
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

TEST(GameServerPreviewTest, BackgroundGenerationCooldownStaysOpenDuringCenterTravel)
{
    static constexpr std::chrono::seconds TIMEOUT{5};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t MOVEMENT_INPUT_COUNT = 8U;

    std::atomic_bool stop_requested{false};
    std::atomic_bool movement_started{false};
    std::atomic_bool initial_center_captured{false};
    std::atomic_bool initially_eligible{false};
    std::atomic_bool movement_center_change_observed{false};
    std::atomic_bool eligible_at_first_movement_center{false};
    std::atomic_int32_t initial_center_x{0};
    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = {},
        .on_preview_buffered = {},
        .on_preview_metrics = [
            &movement_started,
            &initial_center_captured,
            &initially_eligible,
            &movement_center_change_observed,
            &eligible_at_first_movement_center,
            &initial_center_x
        ](core::ClientId, server::GameServer::PreviewStreamMetrics const metrics) {
            if (!metrics.terrain_center_valid) {
                return;
            }
            if (!initial_center_captured.exchange(true, std::memory_order_relaxed)) {
                initial_center_x.store(metrics.terrain_center_x, std::memory_order_relaxed);
            }
            if (metrics.background_generation_eligible) {
                initially_eligible.store(true, std::memory_order_relaxed);
            }
            if (movement_started.load(std::memory_order_relaxed)
                && metrics.terrain_center_x != initial_center_x.load(std::memory_order_relaxed)
                && !movement_center_change_observed.exchange(true, std::memory_order_relaxed)) {
                eligible_at_first_movement_center.store(
                    metrics.background_generation_eligible,
                    std::memory_order_relaxed
                );
            }
        },
        .before_height_tile_completion = {},
    };
    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::thread server_thread{[&server, &stop_requested, &hooks] {
        server.run(stop_requested, &hooks);
    }};
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
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));

    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.received_height_tile_descriptors == 0U
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_EQ(client.received_height_tile_descriptors, 1U);
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!initially_eligible.load(std::memory_order_relaxed)
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_TRUE(initially_eligible.load(std::memory_order_relaxed));

    movement_started.store(true, std::memory_order_relaxed);
    bool sent_all_inputs = true;
    for (uint32_t sequence = 1U; sequence <= MOVEMENT_INPUT_COUNT; ++sequence) {
        sent_all_inputs = client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {.x = 127U, .y = 0U, .accelerated = true, .speedup = 500U},
            .sequence = sequence,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}) && sent_all_inputs;
    }
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (sent_all_inputs
        && (client.last_acknowledged_input < MOVEMENT_INPUT_COUNT
            || !movement_center_change_observed.load(std::memory_order_relaxed))
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_TRUE(sent_all_inputs);
    ASSERT_EQ(client.last_acknowledged_input, MOVEMENT_INPUT_COUNT);
    ASSERT_TRUE(movement_center_change_observed.load(std::memory_order_relaxed));
    EXPECT_TRUE(eligible_at_first_movement_center.load(std::memory_order_relaxed));
}

TEST(GameServerPreviewTest, RepeatedCenterTravelRestoresNearTerrainAheadOfQueuedBacklog)
{
    static constexpr std::chrono::seconds TIMEOUT{10};
    static constexpr std::chrono::seconds NEAR_PROGRESS_TIMEOUT{4};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t MAXIMUM_BUFFERED_TILES = 128U;
    static constexpr uint32_t MAXIMUM_BACKLOG_TILES_BEFORE_NEAR_PROGRESS =
        MAXIMUM_BUFFERED_TILES + shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY;
    static constexpr uint32_t BACKGROUND_SCAN_RESTART_THRESHOLD = 256U;
    static constexpr uint16_t FLIGHT_SPEEDUP = 500U;
    static constexpr uint32_t HEADING_SETUP_SEQUENCE = 1U;
    static constexpr uint32_t TRAVEL_SEQUENCE = HEADING_SETUP_SEQUENCE + 1U;

    std::atomic_uint32_t buffered_tiles{0U};
    std::atomic_uint32_t queued_tiles{0U};
    std::atomic_uint32_t background_scan_cursor{0U};
    std::atomic_bool final_background_scan_cursor_captured{false};
    std::atomic_uint32_t final_background_scan_cursor{0U};
    std::atomic_bool initial_background_center_captured{false};
    std::atomic_int32_t initial_background_center_x{0};
    std::atomic_bool first_travel_center_captured{false};
    std::atomic_uint32_t cursor_at_first_travel_center{0U};
    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = {},
        .on_preview_buffered = [&buffered_tiles](core::ClientId, uint32_t const count) {
            buffered_tiles.store(count, std::memory_order_relaxed);
        },
        .on_preview_metrics = [
            &queued_tiles,
            &background_scan_cursor,
            &final_background_scan_cursor_captured,
            &final_background_scan_cursor,
            &initial_background_center_captured,
            &initial_background_center_x,
            &first_travel_center_captured,
            &cursor_at_first_travel_center
        ](
            core::ClientId,
            server::GameServer::PreviewStreamMetrics const metrics
        ) {
            queued_tiles.store(metrics.queued_tiles, std::memory_order_relaxed);
            background_scan_cursor.store(metrics.background_scan_cursor, std::memory_order_relaxed);
            if (metrics.terrain_center_valid) {
                if (!initial_background_center_captured.exchange(true, std::memory_order_relaxed)) {
                    initial_background_center_x.store(metrics.terrain_center_x, std::memory_order_relaxed);
                } else if (metrics.terrain_center_x != initial_background_center_x.load(std::memory_order_relaxed)
                    && !first_travel_center_captured.exchange(true, std::memory_order_relaxed)) {
                    cursor_at_first_travel_center.store(metrics.background_scan_cursor, std::memory_order_relaxed);
                }
            }
            if (metrics.acknowledged_input_sequence == TRAVEL_SEQUENCE) {
                if (!final_background_scan_cursor_captured.load(std::memory_order_relaxed)) {
                    final_background_scan_cursor.store(metrics.background_scan_cursor, std::memory_order_relaxed);
                    final_background_scan_cursor_captured.store(true, std::memory_order_release);
                }
            }
        },
        .before_height_tile_completion = {},
    };
    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested, &hooks] {
        server.run(stop_requested, &hooks);
    }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };
    PreviewClient client{false, false};
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));

    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.received_height_tile_descriptors == 0U
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_EQ(client.received_height_tile_descriptors, 1U);
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
        .world_revision = 1U,
        .delivery_token = 0U,
        .credits = shared::HEIGHT_TILE_DELIVERY_WINDOW,
        .not_resident_keys = {},
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));

    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while ((client.received_delivery_batches == 0U
            || buffered_tiles.load(std::memory_order_relaxed) < MAXIMUM_BUFFERED_TILES
            || queued_tiles.load(std::memory_order_relaxed) == 0U)
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_TRUE(waitForStalledDeliveryWindow(client));
    expectBoundedDeliveryWindow(deliveryBatchesFrom(client.messages, 0U));
    ASSERT_GT(client.received_height_tiles, 0U);
    ASSERT_EQ(buffered_tiles.load(std::memory_order_relaxed), MAXIMUM_BUFFERED_TILES);
    ASSERT_GT(queued_tiles.load(std::memory_order_relaxed), 0U);

    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {
            .x = 127U,
            .y = 0U,
            .accelerated = true,
            .speedup = 2U,
        },
        .sequence = HEADING_SETUP_SEQUENCE,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.last_acknowledged_input < HEADING_SETUP_SEQUENCE
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_EQ(client.last_acknowledged_input, HEADING_SETUP_SEQUENCE);
    ASSERT_TRUE(initial_background_center_captured.load(std::memory_order_acquire));

    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (background_scan_cursor.load(std::memory_order_relaxed) <= BACKGROUND_SCAN_RESTART_THRESHOLD
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    uint32_t const background_scan_cursor_before_movement = background_scan_cursor.load(std::memory_order_relaxed);
    ASSERT_GT(background_scan_cursor_before_movement, BACKGROUND_SCAN_RESTART_THRESHOLD);

    bool const sent_all_inputs = client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {
            .x = 127U,
            .y = 0U,
            .accelerated = true,
            .speedup = FLIGHT_SPEEDUP,
        },
        .sequence = TRAVEL_SEQUENCE,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable});
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (sent_all_inputs && client.last_acknowledged_input < TRAVEL_SEQUENCE
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_TRUE(sent_all_inputs);
    ASSERT_EQ(client.last_acknowledged_input, TRAVEL_SEQUENCE);

    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!final_background_scan_cursor_captured.load(std::memory_order_acquire)
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_TRUE(final_background_scan_cursor_captured.load(std::memory_order_acquire));
    SCOPED_TRACE(::testing::Message()
        << "background cursor before travel=" << background_scan_cursor_before_movement
        << " at new center=" << cursor_at_first_travel_center.load(std::memory_order_relaxed)
        << " after travel=" << final_background_scan_cursor.load(std::memory_order_relaxed));
    ASSERT_TRUE(first_travel_center_captured.load(std::memory_order_acquire));
    EXPECT_LE(cursor_at_first_travel_center.load(std::memory_order_relaxed), BACKGROUND_SCAN_RESTART_THRESHOLD);

    shared::HeightTileKey const center{
        .x = client.last_player_x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        .y = shared::World::FLIGHT_SPAWN.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
    };
    uint32_t const first_post_travel_message = static_cast<uint32_t>(client.messages.size());
    std::vector<shared::ServerHeightTileBatchMessage> const stalled_batches = deliveryBatchesFrom(
        client.messages,
        0U
    );
    expectBoundedDeliveryWindow(stalled_batches);

    client.setAcknowledgesDeliveries(true);
    bool acknowledged_all = true;
    for (shared::ServerHeightTileBatchMessage const& batch : stalled_batches) {
        acknowledged_all = client.acknowledgeDelivery(batch.delivery_token) && acknowledged_all;
    }
    deadline = std::chrono::steady_clock::now() + NEAR_PROGRESS_TIMEOUT;
    std::vector<shared::HeightTileKey> streamed_after_travel;
    auto const is_near_current_key = [center](shared::HeightTileKey const key) {
        return shared::heightTileGenerationBand(center, 127, 0, key)
            <= shared::HeightTileGenerationBand::Near;
    };
    auto const near_progress_prefix_length = [&streamed_after_travel, &is_near_current_key] {
        uint32_t near_tiles = 0U;
        uint32_t const streamed_tile_count = static_cast<uint32_t>(streamed_after_travel.size());
        for (uint32_t index = 0U; index < streamed_tile_count; ++index) {
            if (is_near_current_key(streamed_after_travel[index]) && ++near_tiles == 16U) {
                return index + 1U;
            }
        }
        return uint32_t{0U};
    };
    auto const streamed_after_travel_count = [&streamed_after_travel] {
        return static_cast<uint32_t>(streamed_after_travel.size());
    };
    uint32_t near_progress_prefix = near_progress_prefix_length();
    while (acknowledged_all && near_progress_prefix == 0U
        && streamed_after_travel_count() < MAXIMUM_BACKLOG_TILES_BEFORE_NEAR_PROGRESS
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
        streamed_after_travel = heightTileKeysFrom(client.messages, first_post_travel_message);
        near_progress_prefix = near_progress_prefix_length();
    }
    if (streamed_after_travel.empty()) {
        streamed_after_travel = heightTileKeysFrom(client.messages, first_post_travel_message);
        near_progress_prefix = near_progress_prefix_length();
    }

    std::string streamed_band_trace;
    std::string streamed_near_keys;
    for (uint32_t index = 0U; index < streamed_after_travel_count(); ++index) {
        shared::HeightTileKey const key = streamed_after_travel[index];
        bool const is_near = is_near_current_key(key);
        streamed_band_trace += is_near ? 'N' : '.';
        if (is_near) {
            streamed_near_keys += std::to_string(index);
            streamed_near_keys += ":(";
            streamed_near_keys += std::to_string(key.x);
            streamed_near_keys += ',';
            streamed_near_keys += std::to_string(key.y);
            streamed_near_keys += ") ";
        }
    }
    SCOPED_TRACE(::testing::Message()
        << "center=(" << center.x << ',' << center.y << ") streamed=" << streamed_after_travel_count()
        << " near_prefix=" << near_progress_prefix << " bands=" << streamed_band_trace
        << " near_keys=" << streamed_near_keys);
    EXPECT_TRUE(acknowledged_all);
    EXPECT_GT(near_progress_prefix, 0U);
    EXPECT_LE(near_progress_prefix, MAXIMUM_BACKLOG_TILES_BEFORE_NEAR_PROGRESS);
}

TEST(GameServerPreviewTest, LateFarCompletionCannotUseAWindowWhileCurrentNearWorkIsDispatched)
{
    static constexpr std::chrono::seconds TIMEOUT{15};
    static constexpr std::chrono::seconds COMPLETION_HOOK_WATCHDOG{30};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr int32_t X200_CENTER_STEP_TILES = 7;
    static constexpr int32_t CURRENT_NEAR_WINDOW_RADIUS = 7;
    static constexpr int32_t PENDING_NEAR_OFFSET = 14;
    static constexpr int32_t LATE_FAR_OFFSET = 20;
    static constexpr uint32_t MOVEMENT_SEQUENCE = 1U;

    uint32_t const hardware_concurrency = static_cast<uint32_t>(std::thread::hardware_concurrency());
    if (server::GameServer::terrainWorkerCount(hardware_concurrency) < 2U) {
        GTEST_SKIP() << "The controlled two-key completion gates require at least two terrain workers";
    }

    shared::HeightTileKey const initial_center{
        .x = shared::World::FLIGHT_SPAWN.x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        .y = shared::World::FLIGHT_SPAWN.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
    };
    shared::HeightTileKey const expected_travel_center = shared::normalizeHeightTileKey({
        .x = initial_center.x + X200_CENTER_STEP_TILES,
        .y = initial_center.y,
    });
    shared::HeightTileKey const pending_near_key = shared::normalizeHeightTileKey({
        .x = initial_center.x + PENDING_NEAR_OFFSET,
        .y = initial_center.y,
    });
    shared::HeightTileKey const late_far_key = shared::normalizeHeightTileKey({
        .x = initial_center.x + LATE_FAR_OFFSET,
        .y = initial_center.y,
    });

    std::atomic_bool stop_requested{false};
    std::atomic_bool pending_near_entered{false};
    std::atomic_bool pending_near_release{false};
    std::atomic_bool late_far_entered{false};
    std::atomic_bool late_far_release{false};
    std::atomic_bool completion_hook_timed_out{false};
    std::atomic_bool initial_center_captured{false};
    std::atomic_int32_t current_center_x{0};
    std::atomic_uint32_t current_near_dispatched{0U};
    std::atomic_uint64_t finished_jobs{0U};
    std::atomic_uint64_t collected_jobs{0U};

    auto const wait_for_release = [&](std::atomic_bool& entered, std::atomic_bool const& release) {
        entered.store(true, std::memory_order_release);
        auto const gate_deadline = std::chrono::steady_clock::now() + COMPLETION_HOOK_WATCHDOG;
        while (!release.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < gate_deadline) {
            std::this_thread::sleep_for(POLL_INTERVAL);
        }
        if (!release.load(std::memory_order_acquire)) {
            completion_hook_timed_out.store(true, std::memory_order_release);
        }
    };

    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = {},
        .on_preview_buffered = {},
        .on_preview_metrics = [
            &initial_center_captured,
            &current_center_x,
            &current_near_dispatched,
            &finished_jobs,
            &collected_jobs
        ](core::ClientId, server::GameServer::PreviewStreamMetrics const metrics) {
            if (!metrics.terrain_center_valid) {
                return;
            }
            if (!initial_center_captured.load(std::memory_order_relaxed)) {
                initial_center_captured.store(true, std::memory_order_release);
            }
            current_center_x.store(metrics.terrain_center_x, std::memory_order_relaxed);
            current_near_dispatched.store(metrics.near_window_dispatched_tiles, std::memory_order_relaxed);
            finished_jobs.store(metrics.height_tile_jobs.finished_total, std::memory_order_relaxed);
            collected_jobs.store(metrics.height_tile_jobs.collected_total, std::memory_order_relaxed);
        },
        .before_height_tile_completion = [
            &pending_near_key,
            &late_far_key,
            &pending_near_entered,
            &pending_near_release,
            &late_far_entered,
            &late_far_release,
            &wait_for_release
        ](core::ClientId, shared::HeightTileKey const key) {
            if (key == pending_near_key) {
                wait_for_release(pending_near_entered, pending_near_release);
            } else if (key == late_far_key) {
                wait_for_release(late_far_entered, late_far_release);
            }
        },
    };

    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::thread server_thread{[&server, &stop_requested, &hooks] {
        server.run(stop_requested, &hooks);
    }};
    defer {
        pending_near_release.store(true, std::memory_order_release);
        late_far_release.store(true, std::memory_order_release);
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };

    PreviewClient client;
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));

    auto const poll_until = [&](auto const& predicate, auto const stop_at) {
        while (!predicate() && std::chrono::steady_clock::now() < stop_at) {
            client.poll(POLL_INTERVAL);
        }
        return predicate();
    };
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    ASSERT_TRUE(poll_until([&] {
        return client.received_height_tile_descriptors == 1U
            && initial_center_captured.load(std::memory_order_acquire)
            && pending_near_entered.load(std::memory_order_acquire)
            && late_far_entered.load(std::memory_order_acquire);
    }, deadline));
    EXPECT_FALSE(completion_hook_timed_out.load(std::memory_order_acquire));
    EXPECT_EQ(current_center_x.load(std::memory_order_relaxed), initial_center.x);

    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {.x = 127U, .y = 0U, .accelerated = true, .speedup = 200U},
        .sequence = MOVEMENT_SEQUENCE,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    ASSERT_TRUE(poll_until([&] {
        return client.last_acknowledged_input == MOVEMENT_SEQUENCE
            && current_center_x.load(std::memory_order_relaxed) == expected_travel_center.x;
    }, deadline));
    EXPECT_EQ(current_center_x.load(std::memory_order_relaxed), expected_travel_center.x);
    ASSERT_TRUE(pending_near_entered.load(std::memory_order_acquire));
    ASSERT_FALSE(pending_near_release.load(std::memory_order_acquire));
    EXPECT_GT(current_near_dispatched.load(std::memory_order_relaxed), 0U);

    uint64_t const finished_before_far_release = finished_jobs.load(std::memory_order_relaxed);
    uint64_t const collected_before_far_release = collected_jobs.load(std::memory_order_relaxed);
    size_t const observations_begin = client.messages.size();
    late_far_release.store(true, std::memory_order_release);
    ASSERT_TRUE(poll_until([&] {
        return finished_jobs.load(std::memory_order_relaxed) > finished_before_far_release
            && collected_jobs.load(std::memory_order_relaxed) > collected_before_far_release;
    }, deadline));

    auto const observation_deadline = std::min(
        deadline,
        std::chrono::steady_clock::now() + std::chrono::milliseconds{250}
    );
    static_cast<void>(poll_until([&] {
        auto const batches = deliveryBatchesFrom(client.messages, observations_begin);
        return std::ranges::any_of(batches, [&late_far_key](auto const& batch) {
            return std::ranges::any_of(batch.tiles, [&late_far_key](auto const& tile) {
                return tile.key == late_far_key;
            });
        });
    }, observation_deadline));
    std::vector<shared::ServerHeightTileBatchMessage> const observed_batches = deliveryBatchesFrom(
        client.messages,
        observations_begin
    );
    bool outside_current_near_admitted = false;
    shared::HeightTileKey first_outside_current_near_key{};
    for (shared::ServerHeightTileBatchMessage const& batch : observed_batches) {
        for (shared::ServerHeightTileMessage const& tile : batch.tiles) {
            bool const is_current_near =
                std::abs(tile.key.x - expected_travel_center.x) <= CURRENT_NEAR_WINDOW_RADIUS
                && std::abs(tile.key.y - expected_travel_center.y) <= CURRENT_NEAR_WINDOW_RADIUS;
            if (!is_current_near && !outside_current_near_admitted) {
                outside_current_near_admitted = true;
                first_outside_current_near_key = tile.key;
            }
        }
    }
    EXPECT_FALSE(outside_current_near_admitted)
        << "tile (" << first_outside_current_near_key.x << ',' << first_outside_current_near_key.y
        << ") outside the current 225-tile window was admitted while nearer work was dispatched";
    EXPECT_FALSE(pending_near_release.load(std::memory_order_acquire));
    EXPECT_GT(current_near_dispatched.load(std::memory_order_relaxed), 0U);

    pending_near_release.store(true, std::memory_order_release);
    auto const near_delivery_deadline = std::min(
        deadline,
        std::chrono::steady_clock::now() + std::chrono::seconds{3}
    );
    ASSERT_TRUE(poll_until([&] {
        return std::ranges::any_of(deliveryBatchesFrom(client.messages, observations_begin),
            [&pending_near_key](auto const& batch) {
                return std::ranges::any_of(batch.tiles, [&pending_near_key](auto const& tile) {
                    return tile.key == pending_near_key;
                });
            });
    }, near_delivery_deadline));
}

TEST(GameServerPreviewTest, RemovalBacklogDoesNotStarveCurrentNearTileDeliveries)
{
    static constexpr std::chrono::seconds TIMEOUT{15};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t RENDER_DISTANCE = 16U;
    static constexpr uint32_t EXPECTED_TILE_COUNT = shared::heightTileInterestCount(RENDER_DISTANCE);
    static constexpr uint32_t FLIGHT_SPEEDUP = 500U;
    static constexpr uint32_t MOVEMENT_INPUT_COUNT = 3U;
    static constexpr uint32_t MAXIMUM_ADD_BATCHES = 1U;

    std::atomic_uint32_t resident_tiles{0U};
    std::atomic_uint32_t inflight_deliveries{0U};
    std::atomic_uint32_t ready_tiles{0U};
    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = {},
        .on_preview_buffered = {},
        .on_preview_metrics = [
            &resident_tiles,
            &inflight_deliveries,
            &ready_tiles
        ](core::ClientId, server::GameServer::PreviewStreamMetrics const metrics) {
            resident_tiles.store(metrics.resident_tiles, std::memory_order_relaxed);
            inflight_deliveries.store(metrics.inflight_deliveries, std::memory_order_relaxed);
            ready_tiles.store(metrics.ready_tiles, std::memory_order_relaxed);
        },
        .before_height_tile_completion = {},
    };
    server::GameServer server{
        0,
        {},
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        RENDER_DISTANCE,
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested, &hooks] {
        server.run(stop_requested, &hooks);
    }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };
    PreviewClient client;
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));

    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while ((resident_tiles.load(std::memory_order_relaxed) != EXPECTED_TILE_COUNT
            || inflight_deliveries.load(std::memory_order_relaxed) != 0U)
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_EQ(resident_tiles.load(std::memory_order_relaxed), EXPECTED_TILE_COUNT);
    ASSERT_EQ(inflight_deliveries.load(std::memory_order_relaxed), 0U);

    client.setAcknowledgesDeliveries(false);
    uint32_t const first_post_travel_message = static_cast<uint32_t>(client.messages.size());
    for (uint32_t sequence = 1U; sequence <= MOVEMENT_INPUT_COUNT; ++sequence) {
        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {
                .x = 127U,
                .y = 0U,
                .accelerated = true,
                .speedup = FLIGHT_SPEEDUP,
            },
            .sequence = sequence,
        }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    }
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.last_acknowledged_input < MOVEMENT_INPUT_COUNT
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_EQ(client.last_acknowledged_input, MOVEMENT_INPUT_COUNT);
    shared::HeightTileKey const center{
        .x = client.last_player_x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        .y = shared::World::FLIGHT_SPAWN.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
    };
    ASSERT_GT(std::abs(center.x - (shared::World::FLIGHT_SPAWN.x
        / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH))), 32);

    auto const post_travel_batch_count = [&client, first_post_travel_message] {
        return static_cast<uint32_t>(std::ranges::count_if(
            std::span<shared::Message const>{client.messages}.subspan(first_post_travel_message),
            [](shared::Message const& message) {
                return std::holds_alternative<shared::ServerHeightTileBatchMessage>(message);
            }
        ));
    };
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while ((post_travel_batch_count() == 0U
            || ready_tiles.load(std::memory_order_relaxed) == 0U)
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_TRUE(waitForStalledDeliveryWindow(client, first_post_travel_message));
    ASSERT_GT(ready_tiles.load(std::memory_order_relaxed), 0U);

    std::vector<shared::ServerHeightTileBatchMessage> const stalled_batches = deliveryBatchesFrom(
        client.messages,
        first_post_travel_message
    );
    expectBoundedDeliveryWindow(stalled_batches);
    uint32_t const maximum_pre_near_operations = (
        static_cast<uint32_t>(stalled_batches.size()) + MAXIMUM_ADD_BATCHES
    ) * shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY;
    client.setAcknowledgesDeliveries(true);
    for (shared::ServerHeightTileBatchMessage const& batch : stalled_batches) {
        ASSERT_TRUE(client.acknowledgeDelivery(batch.delivery_token));
    }

    auto const first_near_operation_count = [&client, first_post_travel_message, center] {
        uint32_t operations = 0U;
        for (uint32_t index = first_post_travel_message;
             index < static_cast<uint32_t>(client.messages.size());
             ++index) {
            shared::Message const& message = client.messages[index];
            if (auto const* const batch = std::get_if<shared::ServerHeightTileBatchMessage>(&message)) {
                operations += static_cast<uint32_t>(batch->removals.size());
                uint32_t additions = 0U;
                for (shared::ServerHeightTileMessage const& tile : batch->tiles) {
                    ++additions;
                    if (shared::heightTileGenerationBand(center, 127, 0, tile.key)
                        <= shared::HeightTileGenerationBand::Near) {
                        return std::optional<uint32_t>{operations + additions};
                    }
                }
                operations += additions;
            }
        }
        return std::optional<uint32_t>{};
    };
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    std::optional<uint32_t> near_operation_count = first_near_operation_count();
    while (!near_operation_count.has_value() && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
        near_operation_count = first_near_operation_count();
    }

    ASSERT_TRUE(near_operation_count.has_value());
    EXPECT_LE(*near_operation_count, maximum_pre_near_operations);
}

TEST(GameServerPreviewTest, SustainedNetworkIngressDoesNotStarveTicksTerrainOrShutdown)
{
    static constexpr auto WARMUP_TIMEOUT = std::chrono::seconds{10};
    static constexpr auto PROGRESS_TIMEOUT = std::chrono::seconds{2};
    static constexpr auto SHUTDOWN_TIMEOUT = std::chrono::milliseconds{250};
    static constexpr auto MINIMUM_INGRESS_DURATION = std::chrono::milliseconds{500};
    static constexpr uint32_t FLOOD_CLIENT_COUNT = 3U;
    static constexpr uint32_t MAX_FLOOD_PACKETS_PER_CLIENT = 500'000U;
    static constexpr uint32_t PACKETS_PER_FLUSH = 128U;
    server::GameServer server{0, {}, shared::WorldMode::Flight, shared::World::canonicalConfiguration(), 4U};
    std::atomic_bool stop_requested{false};
    std::atomic_bool flood_stopped{false};
    std::atomic_bool server_exited{false};
    std::atomic_uint64_t tick_count{0U};
    std::atomic_uint64_t network_events_count{0U};
    std::atomic_uint64_t metrics_count{0U};
    std::atomic_uint64_t submitted_tile_jobs{0U};
    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = [&tick_count, &network_events_count](std::chrono::nanoseconds, uint64_t events) {
            tick_count.fetch_add(1U, std::memory_order_relaxed);
            network_events_count.fetch_add(events, std::memory_order_relaxed);
        },
        .on_preview_buffered = {},
        .on_preview_metrics = [&metrics_count, &submitted_tile_jobs](
            core::ClientId,
            server::GameServer::PreviewStreamMetrics const metrics
        ) {
            metrics_count.fetch_add(1U, std::memory_order_relaxed);
            submitted_tile_jobs.store(metrics.height_tile_jobs.submitted_total, std::memory_order_relaxed);
        },
        .before_height_tile_completion = {},
    };
    PreviewClient observer;
    auto const flood_clients_storage = std::make_unique<std::array<PreviewClient, FLOOD_CLIENT_COUNT>>();
    std::array<PreviewClient, FLOOD_CLIENT_COUNT>& flood_clients = *flood_clients_storage;
    std::array<std::atomic_uint64_t, FLOOD_CLIENT_COUNT> sent_packets{};
    std::array<std::atomic_uint64_t, FLOOD_CLIENT_COUNT> flushed_packets{};
    std::array<std::thread, FLOOD_CLIENT_COUNT> flood_threads;
    auto const totalSentPackets = [&sent_packets] {
        uint64_t count = 0U;
        for (std::atomic_uint64_t const& sent : sent_packets) {
            count += sent.load(std::memory_order_relaxed);
        }
        return count;
    };
    auto const totalFlushedPackets = [&flushed_packets] {
        uint64_t count = 0U;
        for (std::atomic_uint64_t const& flushed : flushed_packets) {
            count += flushed.load(std::memory_order_relaxed);
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
        .not_resident_keys = {},
    });
    auto const flood_client_loop = [&flood_stopped, &flood_packet](
        PreviewClient& client,
        std::atomic_uint64_t& sent_count,
        std::atomic_uint64_t& flushed_count
    ) {
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
                } else {
                    break;
                }
            }
            client.flush();
            flushed_count.store(sent_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    };
    for (uint32_t index = 0U; index < FLOOD_CLIENT_COUNT; ++index) {
        flood_threads[index] = std::thread{
            [client = &flood_clients[index], sent_count = &sent_packets[index],
                flushed_count = &flushed_packets[index], flood_client_loop] {
                flood_client_loop(*client, *sent_count, *flushed_count);
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
            .direction = {.x = 127U, .y = 0U, .accelerated = true, .speedup = 500U},
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
    uint64_t const flushed_packets_before_shutdown = totalFlushedPackets();
    deadline = std::chrono::steady_clock::now() + PROGRESS_TIMEOUT;
    while (totalFlushedPackets() == flushed_packets_before_shutdown
        && std::chrono::steady_clock::now() < deadline) {
        observer.poll(std::chrono::milliseconds{1});
    }
    bool const ingress_active_at_shutdown = totalFlushedPackets() > flushed_packets_before_shutdown;
    stop_requested.store(true, std::memory_order_relaxed);
    deadline = std::chrono::steady_clock::now() + SHUTDOWN_TIMEOUT;
    while (!server_exited.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        observer.poll(std::chrono::milliseconds{1});
    }
    bool const stopped_during_ingress = ingress_active_at_shutdown
        && server_exited.load(std::memory_order_acquire);
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
    EXPECT_GT(network_events_count.load(std::memory_order_relaxed), 0U);
    EXPECT_TRUE(made_progress_under_ingress);
    EXPECT_TRUE(stopped_during_ingress);
}

TEST(GameServerPreviewTest, CompactTerrainUsesAvailableCreditsWithinTheGameplayByteReserve)
{
    static constexpr std::chrono::seconds TIMEOUT{2};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t EXPECTED_BATCHES = 8U;
    static constexpr uint32_t MAXIMUM_BATCH_BYTES = 8'589U;
    static constexpr uint32_t MAXIMUM_OUTSTANDING_BYTES = 34'356U;
    static constexpr uint32_t RENDER_DISTANCE = 32U;
    static constexpr server::GameServer::SpawnPoint SPAWN{
        .character = '@', .x = 0, .y = 0, .z = 400,
    };

    server::GameServer server{
        0U, {SPAWN}, shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(), RENDER_DISTANCE,
    };
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] { server.run(stop_requested); }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };
    PreviewClient client{false};
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (deliveryBatchCount(client.messages) < EXPECTED_BATCHES
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    std::vector<shared::ServerHeightTileBatchMessage> const batches = deliveryBatchesFrom(client.messages, 0U);
    ASSERT_EQ(batches.size(), EXPECTED_BATCHES);
    uint32_t outstanding_bytes = 0U;
    for (shared::ServerHeightTileBatchMessage const& batch : batches) {
        uint32_t const bytes = static_cast<uint32_t>(shared::encodeMessage(batch).size());
        EXPECT_LE(bytes, MAXIMUM_BATCH_BYTES);
        outstanding_bytes += bytes;
    }
    EXPECT_LE(outstanding_bytes, MAXIMUM_OUTSTANDING_BYTES);

    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {}, .sequence = 1U,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.last_acknowledged_input != 1U && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    EXPECT_EQ(client.last_acknowledged_input, 1U);
    EXPECT_EQ(deliveryBatchCount(client.messages), EXPECTED_BATCHES);
}

TEST(GameServerPreviewTest, StalledClientReservesGameplayCapacityAndResumesOneAcknowledgedBatch)
{
    static constexpr auto DURATION = std::chrono::seconds{2};
    static constexpr std::chrono::milliseconds CREDIT_OBSERVATION_DURATION{100};
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
        .not_resident_keys = {},
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {.x = 127U, .y = 0U}, .sequence = 1U,
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
        .not_resident_keys = {},
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    ASSERT_TRUE(waitForStalledDeliveryWindow(client));
    std::vector<shared::ServerHeightTileBatchMessage> const stalled_batches = deliveryBatchesFrom(client.messages, 0U);
    expectBoundedDeliveryWindow(stalled_batches);
    ASSERT_FALSE(stalled_batches.empty());
    uint32_t const stalled_batch_count = static_cast<uint32_t>(stalled_batches.size());
    ASSERT_FALSE(stalled_batches.front().tiles.empty());
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
        .direction = {.x = 127U, .y = 0U}, .sequence = 2U,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    deadline = std::chrono::steady_clock::now() + DURATION;
    while (client.last_acknowledged_input < 2U && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(client.last_acknowledged_input, 2U);
    EXPECT_EQ(deliveryBatchCount(client.messages), stalled_batch_count);
    ASSERT_TRUE(client.acknowledgeDelivery(stalled_batches.front().delivery_token));
    deadline = std::chrono::steady_clock::now() + DURATION;
    while (deliveryBatchCount(client.messages) < stalled_batch_count + 1U
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(waitForStalledDeliveryWindow(client));
    EXPECT_GE(deliveryBatchCount(client.messages), stalled_batch_count + 1U);
    EXPECT_LE(deliveryBatchCount(client.messages), shared::HEIGHT_TILE_DELIVERY_WINDOW + 1U);
    std::vector<shared::ServerHeightTileBatchMessage> outstanding_batches = deliveryBatchesFrom(client.messages, 0U);
    std::erase_if(outstanding_batches, [&](shared::ServerHeightTileBatchMessage const& batch) {
        return batch.delivery_token == stalled_batches.front().delivery_token;
    });
    expectBoundedDeliveryWindow(outstanding_batches);
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    EXPECT_LE(deliveryBatchCount(client.messages), shared::HEIGHT_TILE_DELIVERY_WINDOW + 1U);
    EXPECT_LE(
        heightTileCount(client.messages),
        (shared::HEIGHT_TILE_DELIVERY_WINDOW + 1U) * shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY
    );
}

TEST(GameServerPreviewTest, StalledCreditsBoundPrefetchAndResumeTerrainProgress)
{
    static constexpr std::chrono::seconds TIMEOUT{5};
    static constexpr std::chrono::seconds STALL_DURATION{2};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t MAXIMUM_BUFFERED_TILES = 128U;
    static constexpr uint32_t RECOVERED_TILE_COUNT = 512U;

    uint32_t maximum_buffered_tiles = 0U;
    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = {},
        .on_preview_buffered = [&maximum_buffered_tiles](core::ClientId, uint32_t const buffered_tiles) {
            maximum_buffered_tiles = std::max(maximum_buffered_tiles, buffered_tiles);
        },
        .on_preview_metrics = {},
        .before_height_tile_completion = {},
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
    bool const stalled_window_settled = joined && waitForStalledDeliveryWindow(client);
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
    ASSERT_TRUE(stalled_window_settled);
    ASSERT_TRUE(acknowledged_all);
    expectBoundedDeliveryWindow(stalled_batches);
    EXPECT_LE(
        stalled_tile_count,
        shared::HEIGHT_TILE_DELIVERY_WINDOW * shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY
    );
    EXPECT_EQ(maximum_buffered_tiles, MAXIMUM_BUFFERED_TILES);
    EXPECT_GE(client.received_height_tiles, RECOVERED_TILE_COUNT);
}

void verifyRejectedResidencyReceipt(bool const replay_acknowledgement)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{8};
    static constexpr auto POLL_INTERVAL = std::chrono::milliseconds{1};
    std::atomic_uint64_t rejected_acknowledgements{0U};
    std::atomic_uint64_t accepted_acknowledgements{0U};
    std::atomic_uint32_t resident_tiles{0U};
    server::GameServer::BenchmarkHooks const hooks{
        .on_tick = {},
        .on_preview_buffered = {},
        .on_preview_metrics = [&](core::ClientId, server::GameServer::PreviewStreamMetrics const metrics) {
            resident_tiles.store(metrics.resident_tiles, std::memory_order_relaxed);
            accepted_acknowledgements.store(metrics.delivery_credit_samples, std::memory_order_relaxed);
            rejected_acknowledgements.store(metrics.rejected_delivery_acknowledgements, std::memory_order_release);
        },
        .before_height_tile_completion = {},
    };
    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&] { server.run(stop_requested, &hooks); }};
    defer {
        stop_requested.store(true, std::memory_order_relaxed);
        server_thread.join();
    };
    PreviewClient client{false};
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));
    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    std::vector<shared::ServerHeightTileBatchMessage> batches;
    while (std::ranges::none_of(batches, [](auto const& batch) { return !batch.tiles.empty(); })
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
        batches = deliveryBatchesFrom(client.messages, 0U);
    }
    auto const delivery = std::ranges::find_if(batches, [](auto const& batch) { return !batch.tiles.empty(); });
    ASSERT_NE(delivery, batches.end());
    auto const& batch = *delivery;
    if (replay_acknowledgement) {
        ASSERT_TRUE(client.acknowledgeDelivery(batch.delivery_token));
        ASSERT_TRUE(client.acknowledgeDelivery(batch.delivery_token));
    } else {
        shared::HeightTileKey const unrelated_key = shared::normalizeHeightTileKey({
            .x = 1'000'000, .y = 1'000'000,
        });
        ASSERT_TRUE(client.acknowledgeDelivery(batch.delivery_token, { unrelated_key }));
    }
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (rejected_acknowledgements.load(std::memory_order_acquire) == 0U
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_EQ(rejected_acknowledgements.load(std::memory_order_acquire), 1U);
    EXPECT_EQ(accepted_acknowledgements.load(std::memory_order_relaxed), replay_acknowledgement ? 1U : 0U);
    EXPECT_EQ(resident_tiles.load(std::memory_order_relaxed), replay_acknowledgement ? batch.tiles.size() : 0U);
    if (!replay_acknowledgement) {
        ASSERT_TRUE(client.acknowledgeDelivery(batch.delivery_token));
        deadline = std::chrono::steady_clock::now() + TIMEOUT;
        while (accepted_acknowledgements.load(std::memory_order_relaxed) == 0U
            && std::chrono::steady_clock::now() < deadline) {
            client.poll(POLL_INTERVAL);
        }
        EXPECT_EQ(accepted_acknowledgements.load(std::memory_order_relaxed), 1U);
    }
}

TEST(GameServerPreviewTest, ReplayedResidencyAcknowledgementDoesNotApplyTwice)
{
    verifyRejectedResidencyReceipt(true);
}

TEST(GameServerPreviewTest, UnrelatedResidencyReceiptKeyDoesNotConsumeDelivery)
{
    verifyRejectedResidencyReceipt(false);
}

TEST(GameServerPreviewTest, ClientResidencyReceiptRegeneratesAnEvictedDesiredTile)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{8};
    static constexpr auto POLL_INTERVAL = std::chrono::milliseconds{1};
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
    client.reportFirstDeliveryTileAsEvicted();
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@', .mode = shared::WorldMode::Flight, .wants_previews = true,
    }), shared::GAME_CHANNEL, core::SendMode{core::SendMode::Reliable}));

    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client.received_height_tile_descriptors == 0U
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    ASSERT_EQ(client.received_height_tile_descriptors, 1U);

    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    std::vector<shared::ServerHeightTileBatchMessage> batches;
    while (batches.empty() && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
        batches = deliveryBatchesFrom(client.messages, 0U);
    }
    ASSERT_FALSE(batches.empty());
    auto const first_batch = std::ranges::find_if(batches, [](auto const& batch) {
        return !batch.tiles.empty();
    });
    ASSERT_NE(first_batch, batches.end());
    shared::HeightTileKey const evicted_key = first_batch->tiles.front().key;
    uint64_t const first_delivery_token = first_batch->delivery_token;
    ASSERT_TRUE(client.acknowledgeDelivery(first_delivery_token, { evicted_key }));

    bool regenerated = false;
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!regenerated && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
        for (shared::ServerHeightTileBatchMessage const& batch : deliveryBatchesFrom(client.messages, 0U)) {
            if (batch.delivery_token != first_delivery_token
                && std::ranges::any_of(batch.tiles, [evicted_key](auto const& tile) {
                    return tile.key == evicted_key;
                })) {
                regenerated = true;
                break;
            }
        }
    }

    EXPECT_TRUE(regenerated);
}

TEST(GameServerPreviewTest, ReversalCancelsQueuedRemovalsForRestoredInterest)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{10};
    static constexpr uint32_t INITIAL_TILES = 512U;
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
    ASSERT_TRUE(waitForStalledDeliveryWindow(client, static_cast<uint32_t>(stalled_message)));
    std::vector<shared::ServerHeightTileBatchMessage> const stalled_batches = deliveryBatchesFrom(
        client.messages,
        stalled_message
    );
    expectBoundedDeliveryWindow(stalled_batches);

    for (uint32_t sequence = 1U; sequence <= MOVEMENT_INPUT_COUNT; ++sequence) {
        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {.x = 127U, .y = 0U, .accelerated = true, .speedup = FLIGHT_SPEEDUP},
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
                .y = 0U,
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
    static constexpr uint32_t MINIMUM_REMOVALS_PER_MIXED_BATCH =
        shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY / 2U;
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
    ASSERT_TRUE(waitForStalledDeliveryWindow(client, static_cast<uint32_t>(stalled_message)));
    std::vector<shared::ServerHeightTileBatchMessage> const stalled_batches = deliveryBatchesFrom(
        client.messages,
        stalled_message
    );
    expectBoundedDeliveryWindow(stalled_batches);
    ASSERT_GE(stalled_batches.size(), 2U);

    for (uint32_t sequence = 1U; sequence <= MOVEMENT_INPUT_COUNT; ++sequence) {
        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {.x = 127U, .y = 0U, .accelerated = true, .speedup = FLIGHT_SPEEDUP},
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
    std::vector<uint64_t> acknowledged_tokens{ stalled_batches.front().delivery_token };
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (deliveryBatchesFrom(client.messages, outward_message).empty()
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(waitForStalledDeliveryWindow(client, static_cast<uint32_t>(outward_message)));
    std::vector<shared::ServerHeightTileBatchMessage> outward_batches = deliveryBatchesFrom(
        client.messages,
        outward_message
    );
    ASSERT_FALSE(outward_batches.empty());
    expectAcknowledgedDeliveryWindow(deliveryBatchesFrom(client.messages, stalled_message), acknowledged_tokens);
    EXPECT_LE(stalled_batches.size() + outward_batches.size(), shared::HEIGHT_TILE_DELIVERY_WINDOW + 1U);
    uint32_t const first_ack_batch_count = static_cast<uint32_t>(outward_batches.size());
    ASSERT_TRUE(client.acknowledgeDelivery(outward_batches.front().delivery_token));
    acknowledged_tokens.push_back(outward_batches.front().delivery_token);
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (deliveryBatchesFrom(client.messages, outward_message).size() < first_ack_batch_count + 1U
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(waitForStalledDeliveryWindow(client, static_cast<uint32_t>(outward_message)));
    outward_batches = deliveryBatchesFrom(client.messages, outward_message);
    ASSERT_GE(outward_batches.size(), first_ack_batch_count + 1U);
    expectAcknowledgedDeliveryWindow(deliveryBatchesFrom(client.messages, stalled_message), acknowledged_tokens);
    EXPECT_LE(stalled_batches.size() + outward_batches.size(), shared::HEIGHT_TILE_DELIVERY_WINDOW + 2U);

    shared::HeightTileKey const outward_center{
        .x = client.last_player_x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
        .y = shared::World::FLIGHT_SPAWN.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
    };
    std::vector<shared::HeightTileKey> outward_removal_keys;
    uint32_t outward_removals = 0U;
    for (shared::ServerHeightTileBatchMessage const& batch : outward_batches) {
        EXPECT_FALSE(batch.removals.empty());
        EXPECT_GE(batch.removals.size(), batch.tiles.size());
        for (shared::ServerRemoveHeightTileMessage const& removal : batch.removals) {
            EXPECT_FALSE(shared::heightTileWithinInterest(
                outward_center, removal.key, shared::HEIGHT_TILE_INTEREST_RADIUS
            ));
            EXPECT_EQ(std::ranges::find(outward_removal_keys, removal.key), outward_removal_keys.end());
            outward_removal_keys.push_back(removal.key);
            ++outward_removals;
        }
    }
    ASSERT_GE(outward_removals, 2U * MINIMUM_REMOVALS_PER_MIXED_BATCH);

    uint32_t const late_ack_message = static_cast<uint32_t>(client.messages.size());
    ASSERT_TRUE(client.acknowledgeDelivery(stalled_batches[1U].delivery_token));
    acknowledged_tokens.push_back(stalled_batches[1U].delivery_token);
    deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (deliveryBatchesFrom(client.messages, late_ack_message).empty()
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(waitForStalledDeliveryWindow(client, late_ack_message));
    std::vector<shared::ServerHeightTileBatchMessage> const late_ack_batches = deliveryBatchesFrom(
        client.messages,
        late_ack_message
    );
    ASSERT_FALSE(late_ack_batches.empty());
    expectAcknowledgedDeliveryWindow(deliveryBatchesFrom(client.messages, stalled_message), acknowledged_tokens);
    EXPECT_LE(
        stalled_batches.size() + outward_batches.size() + late_ack_batches.size(),
        shared::HEIGHT_TILE_DELIVERY_WINDOW + 3U
    );
    uint32_t late_ack_removals = 0U;
    for (shared::ServerHeightTileBatchMessage const& batch : late_ack_batches) {
        EXPECT_FALSE(batch.removals.empty());
        EXPECT_GE(batch.removals.size(), batch.tiles.size());
        for (shared::ServerRemoveHeightTileMessage const& removal : batch.removals) {
            EXPECT_FALSE(shared::heightTileWithinInterest(
                outward_center, removal.key, shared::HEIGHT_TILE_INTEREST_RADIUS
            ));
            EXPECT_EQ(std::ranges::find(outward_removal_keys, removal.key), outward_removal_keys.end());
            outward_removal_keys.push_back(removal.key);
            ++late_ack_removals;
        }
    }
    ASSERT_GE(late_ack_removals, MINIMUM_REMOVALS_PER_MIXED_BATCH);
    outward_batches.insert(outward_batches.end(), late_ack_batches.begin(), late_ack_batches.end());

    for (uint32_t sequence = MOVEMENT_INPUT_COUNT + 1U; sequence <= FINAL_INPUT_SEQUENCE; ++sequence) {
        ASSERT_TRUE(client.send(shared::encodeMessage(shared::ClientInputMessage{
            .direction = {
                .x = static_cast<uint8_t>(-127),
                .y = 0U,
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
    // Movement can admit removal-only batches between the captured stalled and outward windows.
    for (shared::ServerHeightTileBatchMessage const& batch : deliveryBatchesFrom(client.messages, stalled_message)) {
        if (std::ranges::find(acknowledged_tokens, batch.delivery_token) == acknowledged_tokens.end()) {
            ASSERT_TRUE(client.acknowledgeDelivery(batch.delivery_token));
        }
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
    static constexpr auto JOIN_TIMEOUT = std::chrono::seconds{10};
    static constexpr uint32_t MAXIMUM_ACK_LAG = 16U;
    static constexpr uint32_t MAX_CLIENT_POLL_CALLS_PER_PASS = 32U;
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
    auto const all_clients_joined = [&clients] {
        return std::ranges::all_of(clients, [](PreviewClient const* const client) {
            return client->join_accepted.value_or(false);
        });
    };
    auto const join_deadline = std::chrono::steady_clock::now() + JOIN_TIMEOUT;
    while (joined && !all_clients_joined() && std::chrono::steady_clock::now() < join_deadline) {
        for (PreviewClient* const client : clients) {
            client->poll(std::chrono::milliseconds{1});
        }
    }
    joined = joined && all_clients_joined();
    for (PreviewClient* const client : clients) {
        while (client->poll(std::chrono::milliseconds::zero()) > 0) {
        }
    }
    uint32_t sent = 0U;
    uint32_t maximum_ack_lag = 0U;
    uint32_t sent_at_maximum_ack_lag = 0U;
    std::array<size_t, 4> midpoint_tile_counts{};
    bool midpoint_recorded = false;
    bool all_sent = true;
    auto const started = std::chrono::steady_clock::now();
    auto next_client_progress = started + PROGRESS_INTERVAL;
    auto next_input = started;
    auto const poll_clients_fairly = [&clients] {
        for (PreviewClient* const client : clients) {
            for (uint32_t poll_call = 0U; poll_call < MAX_CLIENT_POLL_CALLS_PER_PASS; ++poll_call) {
                if (client->poll(std::chrono::milliseconds::zero()) == 0U) {
                    break;
                }
            }
        }
    };
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
        poll_clients_fairly();
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
        "Flight summary sent={}, maximum ack lag={} at input={}, "
        "final acknowledgements={}/{}/{}/{}, duration={} ms",
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
    EXPECT_LE(maximum_ack_lag, MAXIMUM_ACK_LAG)
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
