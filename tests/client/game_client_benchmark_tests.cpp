#include <client/ClientAudio.hpp>
#include <client/GameClient.hpp>
#include <client/PlayerPresentation.hpp>
#include <client/PreviewResidency.hpp>
#include <client/WorldLoading.hpp>

#include <server/GameServer.hpp>
#include <server/WorldPreparation.hpp>

#include <shared/net/Message.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldCoverage.hpp>

#include <generation/Generation.hpp>

#include <core/IO/Log.hpp>
#include <core/net/Address.hpp>
#include <core/net/Event.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

class ImmediateAudioOutput final : public client::ClientAudioOutput {
public:
    [[nodiscard]] bool start() noexcept override { return true; }
    void stop() noexcept override {}
    [[nodiscard]] bool play(std::span<int16_t const> samples) noexcept override
    {
        return !samples.empty();
    }
};

class BenchmarkClient final : public client::GameClient {
public:
    std::function<void()> on_render;

    explicit BenchmarkClient(shared::WorldMode const mode = shared::WorldMode::Flat)
        : GameClient{
            mode,
            shared::World::canonicalConfiguration(),
            true,
            std::make_unique<ImmediateAudioOutput>(),
        }
    { }

    [[nodiscard]] std::optional<shared::Player> predictedMovementPlayer() const noexcept
    {
        return predictedLocalPlayer();
    }

    [[nodiscard]] auto sampledMovementPresentation(
        std::chrono::steady_clock::time_point const now
    ) const noexcept
    {
        return predictedLocalPresentation(now);
    }

private:
    shared::Direction input() override
    {
        return shared::Direction{ .x = 0U, .y = 0U };
    }

    void render() override
    {
        client::WorldLoadingStatus const status = worldLoadingStatus();
        if (status.state == client::WorldLoadingState::ReadyForStart) {
            markLoadingPromptRendered(std::chrono::steady_clock::now());
        }
        if (on_render) {
            on_render();
        }
    }

    [[nodiscard]] bool presentationSucceeded() const override
    {
        return true;
    }
};

std::shared_ptr<server::WorldPreparation> boundedTestPreparation(
    shared::WorldConfiguration const& world_configuration
)
{
    generation::GenerationConfiguration configuration;
    configuration.seed = world_configuration.seed;
    configuration.epochs = 1U;
    configuration.erosion_steps = 0U;
    configuration.initial_climate_iterations = 1U;
    configuration.climate_iterations = 1U;
    configuration.currents = false;
    configuration.coastal_elaboration = false;
    return std::make_shared<server::WorldPreparation>(configuration, world_configuration.algorithm_version);
}

bool readyPromptPresented(BenchmarkClient const& game_client)
{
    client::WorldLoadingStatus const status = game_client.worldLoadingStatus();
    return status.state == client::WorldLoadingState::ReadyForStart
        && status.ready_prompt_frame_rendered_at.has_value();
}

std::shared_ptr<server::WorldPreparation> cachedCanonicalWorldPreparation()
{
    static std::shared_ptr<server::WorldPreparation> const preparation = [] {
        shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
        generation::GenerationConfiguration generation_configuration{};
        generation_configuration.seed = world_configuration.seed;
        return std::make_shared<server::WorldPreparation>(
            generation_configuration,
            world_configuration.algorithm_version
        );
    }();
    return preparation;
}

std::shared_ptr<shared::TerrainWorld const> waitForPreparedTerrainWorld(
    server::WorldPreparation& preparation,
    std::chrono::steady_clock::time_point const deadline
)
{
    static constexpr std::chrono::milliseconds POLL_INTERVAL{ 1 };
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::shared_ptr<shared::TerrainWorld const> const terrain_world = preparation.readyWorld()) {
            return terrain_world;
        }
        if (preparation.latest().state != server::WorldPreparationState::Preparing) {
            return nullptr;
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
    return preparation.readyWorld();
}

struct FlatSpawnCandidate final {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int32_t horizontal_sign = 1;
    uint16_t surface_height = 0U;
    uint32_t distance_from_center = 0U;
};

bool preferredFlatSpawn(FlatSpawnCandidate const& candidate, FlatSpawnCandidate const& current)
{
    return candidate.distance_from_center < current.distance_from_center
        || (candidate.distance_from_center == current.distance_from_center
            && (candidate.x < current.x
                || (candidate.x == current.x && candidate.y < current.y)
                || (candidate.x == current.x && candidate.y == current.y
                    && candidate.horizontal_sign > current.horizontal_sign)));
}

std::optional<FlatSpawnCandidate> findFlatSpawnNear(
    shared::TerrainWorld const& terrain_world,
    int32_t const center_x,
    int32_t const center_y,
    uint32_t const search_radius,
    uint32_t const track_cells
)
{
    int32_t const first_x = center_x - static_cast<int32_t>(search_radius) - static_cast<int32_t>(track_cells);
    int32_t const last_x = center_x + static_cast<int32_t>(search_radius) + static_cast<int32_t>(track_cells);
    std::vector<uint16_t> row_heights;
    row_heights.reserve(static_cast<size_t>(last_x - first_x + 1));
    std::optional<FlatSpawnCandidate> best;

    for (int32_t y = center_y - static_cast<int32_t>(search_radius);
        y <= center_y + static_cast<int32_t>(search_radius);
        ++y) {
        row_heights.clear();
        for (int32_t x = first_x; x <= last_x; ++x) {
            row_heights.push_back(terrain_world.sampleSurfaceHeight(
                static_cast<uint32_t>(x),
                static_cast<uint32_t>(y)
            ));
        }

        for (int32_t x = center_x - static_cast<int32_t>(search_radius);
            x <= center_x + static_cast<int32_t>(search_radius);
            ++x) {
            for (int32_t const horizontal_sign : { 1, -1 }) {
                uint32_t const chunk_offset = static_cast<uint32_t>(x)
                    % shared::Chunk::SIDE_LENGTH;
                if ((horizontal_sign > 0
                        && chunk_offset + track_cells > shared::Chunk::SIDE_LENGTH)
                    || (horizontal_sign < 0 && chunk_offset + 1U < track_cells)) {
                    continue;
                }
                int32_t const strip_first_x = horizontal_sign > 0
                    ? x
                    : x - static_cast<int32_t>(track_cells) + 1;
                uint16_t const surface_height = row_heights[
                    static_cast<size_t>(strip_first_x - first_x)
                ];
                if (surface_height <= generation::BLOCK_SEA_LEVEL + 1U) {
                    continue;
                }
                size_t const strip_first_index = static_cast<size_t>(strip_first_x - first_x);
                bool const flat = std::all_of(
                    row_heights.begin() + strip_first_index,
                    row_heights.begin() + strip_first_index + track_cells,
                    [surface_height](uint16_t const height) { return height == surface_height; }
                );
                if (!flat) {
                    continue;
                }
                uint32_t const distance = static_cast<uint32_t>(std::abs(x - center_x)
                    + std::abs(y - center_y));
                FlatSpawnCandidate const candidate{
                    .x = x,
                    .y = y,
                    .z = static_cast<int32_t>(surface_height) + 1,
                    .horizontal_sign = horizontal_sign,
                    .surface_height = surface_height,
                    .distance_from_center = distance,
                };
                if (!best.has_value() || preferredFlatSpawn(candidate, *best)) {
                    best = candidate;
                }
            }
        }
    }
    return best;
}

} // namespace

TEST(GameClientBenchmarkTest, SamplesConnectedProductionLoopsAndStopsAtHookBoundary)
{
    static constexpr std::chrono::seconds DEADLINE{ 15 };
    static constexpr uint64_t TARGET_LOOPS = 150U;
    shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
    auto preparation = boundedTestPreparation(world_configuration);
    server::GameServer server{
        0U,
        {},
        shared::WorldMode::Flight,
        world_configuration,
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        nullptr,
        preparation,
    };
    std::atomic_bool stop_server{ false };
    std::thread server_thread{ [&] { server.run(stop_server); } };

    BenchmarkClient game_client{ shared::WorldMode::Flight };
    uint64_t completed_loops = 0U;
    uint64_t overridden_inputs = 0U;
    uint64_t authoritative_updates = 0U;
    struct SentInputSample final {
        uint32_t sequence = 0U;
        shared::Direction direction{};
        std::chrono::steady_clock::time_point sent_at{};
        std::chrono::steady_clock::time_point scheduled_at{};
    };
    std::vector<SentInputSample> sent_inputs;
    std::vector<uint32_t> acknowledged_sequences;
    std::vector<uint32_t> authoritative_revisions;
    bool input_before_playing = false;
    bool start_requested = false;
    std::optional<std::chrono::steady_clock::time_point> start_requested_at;
    bool invalid_sample = false;
    bool invalid_sent_input = false;
    bool invalid_authoritative_state = false;
    auto const started_at = std::chrono::steady_clock::now();
    client::GameClientBenchmarkHooks const hooks{
        .deadline = std::chrono::steady_clock::now() + DEADLINE,
        .should_stop = [&] {
            return completed_loops >= TARGET_LOOPS
                && overridden_inputs > 0U
                && game_client.worldLoadingStatus().state == client::WorldLoadingState::Playing;
        },
        .is_uncapped_phase = {},
        .input_override = [&](uint64_t) -> std::optional<shared::Direction> {
            input_before_playing |= game_client.worldLoadingStatus().state != client::WorldLoadingState::Playing;
            ++overridden_inputs;
            return shared::Direction{ .x = 1U, .y = 0U };
        },
        .start_input_override = [&](uint64_t) {
            if (!start_requested && readyPromptPresented(game_client)) {
                start_requested = true;
                start_requested_at = std::chrono::steady_clock::now();
                return true;
            }
            return false;
        },
        .on_loop = [&](client::GameClientLoopSample const& sample) {
            if (overridden_inputs > 0U
                && game_client.worldLoadingStatus().state == client::WorldLoadingState::Playing) {
                ++completed_loops;
            }
            invalid_sample |= sample.completed_at < sample.started_at
                || sample.network_poll_duration < std::chrono::nanoseconds::zero()
                || sample.render_duration < std::chrono::nanoseconds::zero()
                || !sample.presentation_succeeded;
        },
        .on_authoritative_player = [&](shared::Player const&) {
            ++authoritative_updates;
        },
        .on_input_sent = [&] (
            shared::ClientInputMessage const& message,
            std::chrono::steady_clock::time_point const sent_at,
            std::chrono::steady_clock::time_point const scheduled_at
        ) {
            sent_inputs.push_back(SentInputSample{
                .sequence = message.sequence,
                .direction = message.direction,
                .sent_at = sent_at,
                .scheduled_at = scheduled_at,
            });
            invalid_sent_input |= sent_at < scheduled_at
                || message.direction.x != 1U
                || message.direction.y != 0U;
        },
        .on_authoritative_state = [&] (
            shared::ServerPlayerPositionMessage const& message,
            shared::Player const& player,
            std::chrono::steady_clock::time_point const observed_at
        ) {
            acknowledged_sequences.push_back(message.acknowledged_input_sequence);
            authoritative_revisions.push_back(message.state_revision);
            invalid_authoritative_state |= observed_at < started_at || player.ch != message.ch;
        },
    };
    game_client.run(core::Address::localhost(server.port()), '@', &hooks);
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();

    EXPECT_EQ(completed_loops, TARGET_LOOPS);
    EXPECT_GT(overridden_inputs, 0U);
    EXPECT_TRUE(start_requested);
    client::WorldLoadingStatus const loading_status = game_client.worldLoadingStatus();
    ASSERT_TRUE(start_requested_at.has_value());
    ASSERT_TRUE(loading_status.key_accepted_at.has_value());
    EXPECT_GE(*loading_status.key_accepted_at, *start_requested_at);
    EXPECT_FALSE(input_before_playing);
    EXPECT_GT(authoritative_updates, 0U);
    ASSERT_FALSE(sent_inputs.empty());
    EXPECT_FALSE(invalid_sent_input);
    EXPECT_FALSE(invalid_authoritative_state);
    for (uint64_t index = 1U; index < sent_inputs.size(); ++index) {
        EXPECT_GT(sent_inputs[index].sequence, sent_inputs[index - 1U].sequence);
    }
    EXPECT_FALSE(authoritative_revisions.empty());
    for (uint64_t index = 1U; index < authoritative_revisions.size(); ++index) {
        EXPECT_GE(authoritative_revisions[index], authoritative_revisions[index - 1U]);
    }
    for (uint32_t const acknowledged_sequence : acknowledged_sequences) {
        if (acknowledged_sequence == 0U) {
            continue;
        }
        EXPECT_NE(std::find_if(sent_inputs.begin(), sent_inputs.end(), [&](SentInputSample const& sample) {
            return sample.sequence == acknowledged_sequence;
        }), sent_inputs.end());
    }
    EXPECT_FALSE(invalid_sample);
}

TEST(GameClientBenchmarkTest, ConnectedProductionLoopAdoptsTheServerWideRadius)
{
    static constexpr uint32_t RADIUS = 128U;
    static constexpr std::chrono::seconds DEADLINE{15};
    shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
    auto preparation = boundedTestPreparation(world_configuration);
    server::GameServer server{
        0U,
        {},
        shared::WorldMode::Flight,
        world_configuration,
        RADIUS,
        nullptr,
        preparation,
    };
    std::atomic_bool stop_server{false};
    std::thread server_thread{[&] { server.run(stop_server); }};
    BenchmarkClient game_client{shared::WorldMode::Flight};
    bool start_requested = false;
    client::GameClientBenchmarkHooks const hooks{
        .deadline = std::chrono::steady_clock::now() + DEADLINE,
        .should_stop = [&] {
            return game_client.worldLoadingStatus().state == client::WorldLoadingState::Playing
                && game_client.heightTileResidency().stats().resident_tiles > 0U;
        },
        .is_uncapped_phase = {},
        .input_override = {},
        .start_input_override = [&](uint64_t) {
            if (!start_requested && readyPromptPresented(game_client)) {
                start_requested = true;
                return true;
            }
            return false;
        },
        .on_loop = {},
        .on_authoritative_player = {},
        .on_input_sent = {},
        .on_authoritative_state = {},
    };
    game_client.run(core::Address::localhost(server.port()), '@', &hooks);
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();
    EXPECT_EQ(game_client.renderDistance(), RADIUS);
    EXPECT_EQ(game_client.requiredHeightTileCount(), 51'433U);
    EXPECT_GT(game_client.heightTileResidency().stats().resident_tiles, 0U);
}

TEST(GameClientBenchmarkTest, AppliedTerrainDeliveryReachesServerBeforeRenderingCompletes)
{
    static constexpr uint32_t RADIUS = 8U;
    static constexpr std::chrono::seconds DEADLINE{ 15 };
    static constexpr std::chrono::milliseconds ACK_DEADLINE{ 500 };
    static constexpr std::chrono::milliseconds ACK_POLL_INTERVAL{ 1 };
    shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
    auto preparation = boundedTestPreparation(world_configuration);
    server::GameServer server{
        0U,
        {},
        shared::WorldMode::Flight,
        world_configuration,
        RADIUS,
        nullptr,
        preparation,
    };
    std::atomic_bool stop_server{ false };
    std::atomic_uint64_t acknowledged_deliveries{ 0U };
    server::GameServer::BenchmarkHooks const server_hooks{
        .on_tick = {},
        .on_preview_buffered = {},
        .on_preview_metrics = [&](core::ClientId, server::GameServer::PreviewStreamMetrics const metrics) {
            acknowledged_deliveries.store(metrics.delivery_credit_samples, std::memory_order_release);
        },
        .before_height_tile_completion = {},
    };
    std::thread server_thread{ [&] { server.run(stop_server, &server_hooks); } };
    BenchmarkClient game_client{ shared::WorldMode::Flight };
    bool blocked_after_delivery = false;
    bool acknowledged_during_render = false;
    bool start_requested = false;
    game_client.on_render = [&] {
        if (game_client.heightTileResidency().stats().resident_tiles == 0U) {
            return;
        }
        blocked_after_delivery = true;
        auto const acknowledgement_deadline = std::chrono::steady_clock::now() + ACK_DEADLINE;
        while (acknowledged_deliveries.load(std::memory_order_acquire) == 0U
            && std::chrono::steady_clock::now() < acknowledgement_deadline) {
            std::this_thread::sleep_for(ACK_POLL_INTERVAL);
        }
        acknowledged_during_render = acknowledged_deliveries.load(std::memory_order_acquire) > 0U;
    };
    client::GameClientBenchmarkHooks const hooks{
        .deadline = std::chrono::steady_clock::now() + DEADLINE,
        .should_stop = [&] {
            return blocked_after_delivery && acknowledged_during_render
                && game_client.worldLoadingStatus().state == client::WorldLoadingState::Playing;
        },
        .is_uncapped_phase = {},
        .input_override = {},
        .start_input_override = [&](uint64_t) {
            if (!start_requested && readyPromptPresented(game_client)) {
                start_requested = true;
                return true;
            }
            return false;
        },
        .on_loop = {},
        .on_authoritative_player = {},
        .on_input_sent = {},
        .on_authoritative_state = {},
    };
    game_client.run(core::Address::localhost(server.port()), '@', &hooks);
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();

    EXPECT_TRUE(blocked_after_delivery);
    EXPECT_TRUE(acknowledged_during_render);
}

TEST(GameClientBenchmarkTest, QueuedGameplayInputReachesServerWhilePresentationIsBlocked)
{
    static constexpr uint32_t RADIUS = 8U;
    static constexpr std::chrono::seconds DEADLINE{ 15 };
    static constexpr std::chrono::milliseconds SERVER_RECEIVE_DEADLINE{ 500 };
    struct ServerInputObservation final {
        uint32_t sequence;
        std::chrono::steady_clock::time_point received_at;
    };
    shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
    auto preparation = boundedTestPreparation(world_configuration);
    server::GameServer server{
        0U,
        {},
        shared::WorldMode::Flight,
        world_configuration,
        RADIUS,
        nullptr,
        preparation,
    };
    std::atomic_bool stop_server{ false };
    std::atomic_bool server_input_observed{ false };
    std::promise<ServerInputObservation> first_server_input;
    std::future<ServerInputObservation> first_server_input_future = first_server_input.get_future();
    server::GameServer::BenchmarkHooks const server_hooks{
        .on_tick = {},
        .on_preview_buffered = {},
        .on_preview_metrics = [&](core::ClientId, server::GameServer::PreviewStreamMetrics const metrics) {
            if (metrics.latest_received_input_sequence > 0U && !server_input_observed.load(
                std::memory_order_relaxed
            )) {
                first_server_input.set_value(ServerInputObservation{
                    .sequence = metrics.latest_received_input_sequence,
                    .received_at = std::chrono::steady_clock::now(),
                });
                server_input_observed.store(true, std::memory_order_release);
            }
        },
        .before_height_tile_completion = {},
    };
    std::thread server_thread{ [&] { server.run(stop_server, &server_hooks); } };
    BenchmarkClient game_client{ shared::WorldMode::Flight };
    uint32_t first_sent_input_sequence = 0U;
    bool start_requested = false;
    bool presentation_blocked_after_input = false;
    bool server_received_input_during_render = false;
    std::optional<uint32_t> received_input_sequence_during_render;
    std::optional<std::chrono::steady_clock::time_point> server_received_at;
    std::optional<std::chrono::steady_clock::time_point> render_finished_at;
    client::GameClientBenchmarkHooks const hooks{
        .deadline = std::chrono::steady_clock::now() + DEADLINE,
        .should_stop = [&] {
            return presentation_blocked_after_input
                && server_input_observed.load(std::memory_order_acquire);
        },
        .is_uncapped_phase = {},
        .input_override = [](uint64_t) -> std::optional<shared::Direction> {
            return shared::Direction{ .x = 1U, .y = 0U };
        },
        .start_input_override = [&](uint64_t) {
            if (!start_requested && readyPromptPresented(game_client)) {
                start_requested = true;
                return true;
            }
            return false;
        },
        .on_loop = {},
        .on_authoritative_player = {},
        .on_input_sent = [&](
            shared::ClientInputMessage const& message,
            std::chrono::steady_clock::time_point,
            std::chrono::steady_clock::time_point
        ) {
            if (first_sent_input_sequence == 0U) {
                first_sent_input_sequence = message.sequence;
            }
        },
        .on_authoritative_state = {},
    };
    game_client.on_render = [&] {
        if (first_sent_input_sequence == 0U || presentation_blocked_after_input) {
            return;
        }
        presentation_blocked_after_input = true;
        server_received_input_during_render = first_server_input_future.wait_for(SERVER_RECEIVE_DEADLINE)
            == std::future_status::ready;
        if (server_received_input_during_render) {
            ServerInputObservation const observation = first_server_input_future.get();
            received_input_sequence_during_render = observation.sequence;
            server_received_at = observation.received_at;
        }
        render_finished_at = std::chrono::steady_clock::now();
    };
    game_client.run(core::Address::localhost(server.port()), '@', &hooks);
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();

    EXPECT_TRUE(start_requested);
    EXPECT_GT(first_sent_input_sequence, 0U);
    EXPECT_TRUE(presentation_blocked_after_input);
    EXPECT_TRUE(server_received_input_during_render);
    ASSERT_TRUE(received_input_sequence_during_render.has_value());
    EXPECT_EQ(*received_input_sequence_during_render, first_sent_input_sequence);
    ASSERT_TRUE(server_received_at.has_value());
    ASSERT_TRUE(render_finished_at.has_value());
    EXPECT_LT(*server_received_at, *render_finished_at);
}

TEST(GameClientBenchmarkTest, ReplaysRecordedCanonicalGroundStallAndFlatControl)
{
    static constexpr shared::PlayerPosition RECORDED_POSITION{
        .x = 32'921,
        .y = 32'864,
        .z = 471,
        .x_subcell = 2'265U,
        .y_subcell = 2'689U,
        .z_subcell = 3'049U,
    };
    static constexpr uint32_t FLAT_CONTROL_INPUTS = 3U;
    static constexpr uint32_t FLAT_CONTROL_TRACK_CELLS = (
        FLAT_CONTROL_INPUTS * shared::MOVEMENT_SUBCELLS_PER_TICK
        + shared::World::PLAYER_WIDTH_SUBCELLS
        + shared::SUBCELLS_PER_CELL - 1U
    ) / shared::SUBCELLS_PER_CELL + 1U;
    static constexpr uint32_t FLAT_CONTROL_SEARCH_RADIUS =
        shared::NEAR_TERRAIN_RADIUS_BLOCKS - FLAT_CONTROL_TRACK_CELLS;
    static constexpr std::chrono::seconds PREPARATION_DEADLINE{ 120 };
    static constexpr std::chrono::seconds DEADLINE{ 60 };

    enum class MovementPhase : uint8_t {
        Waiting,
        RecordedPose,
        FlatControl,
    };
    using Pose = std::array<double, 3U>;
    auto const makeRecordedDirection = [](uint8_t const x, uint8_t const y, uint8_t const z) {
        return shared::Direction{
            .x = x,
            .y = y,
            .z = z,
            .accelerated = true,
            .speedup = 5U,
        };
    };
    std::array<shared::Direction, 8U> const recorded_inputs{
        makeRecordedDirection(31U, 122U, 0U),
        makeRecordedDirection(31U, 123U, 0U),
        makeRecordedDirection(31U, 123U, 0U),
        makeRecordedDirection(31U, 123U, 127U),
        makeRecordedDirection(11U, 126U, 127U),
        makeRecordedDirection(0U, 127U, 127U),
        makeRecordedDirection(0U, 127U, 127U),
        makeRecordedDirection(0U, 127U, 127U),
    };

    shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
    std::shared_ptr<server::WorldPreparation> const preparation = cachedCanonicalWorldPreparation();
    std::shared_ptr<shared::TerrainWorld const> const terrain_world = waitForPreparedTerrainWorld(
        *preparation,
        std::chrono::steady_clock::now() + PREPARATION_DEADLINE
    );
    if (terrain_world == nullptr) {
        CORE_ERROR("PF02 canonical movement trace could not prepare the default terrain world");
        FAIL() << "Canonical terrain preparation did not complete";
        return;
    }
    ASSERT_EQ(world_configuration.seed, shared::WorldConfiguration::SEED);
    ASSERT_EQ(terrain_world->identity().seed, shared::WorldConfiguration::SEED);

    std::optional<FlatSpawnCandidate> const flat_control = findFlatSpawnNear(
        *terrain_world,
        RECORDED_POSITION.x,
        RECORDED_POSITION.y,
        FLAT_CONTROL_SEARCH_RADIUS,
        FLAT_CONTROL_TRACK_CELLS
    );
    ASSERT_TRUE(flat_control.has_value()) << "No short canonical flat control was found near the recorded pose";
    shared::PlayerPosition const flat_control_position{
        .x = flat_control->x,
        .y = flat_control->y,
        .z = flat_control->z,
    };
    CORE_INFO("PF02 canonical seed={} terrain_revision={} recorded_pose=({}.{};{}.{};{}.{}) "
        "flat_control=({}, {}, {}) surface_z={} sign={} track_cells={} distance={}",
        terrain_world->identity().seed,
        terrain_world->identity().revision,
        RECORDED_POSITION.x,
        RECORDED_POSITION.x_subcell,
        RECORDED_POSITION.y,
        RECORDED_POSITION.y_subcell,
        RECORDED_POSITION.z,
        RECORDED_POSITION.z_subcell,
        flat_control->x,
        flat_control->y,
        flat_control->z,
        flat_control->surface_height,
        flat_control->horizontal_sign,
        FLAT_CONTROL_TRACK_CELLS,
        flat_control->distance_from_center);

    server::GameServer server{
        0U,
        {{'@', RECORDED_POSITION.x, RECORDED_POSITION.y, RECORDED_POSITION.z}},
        shared::WorldMode::Flight,
        world_configuration,
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        nullptr,
        preparation,
    };
    std::atomic_bool stop_server{ false };
    std::atomic_bool near_required_chunks_all_resident{ false };
    std::atomic_bool exact_rerooted{ false };
    std::atomic_bool exact_replay_acknowledged{ false };
    std::atomic_bool control_rerooted{ false };
    std::atomic_bool has_server_player_id{ false };
    std::atomic_uint32_t server_player_id{ 0U };
    std::atomic_uint32_t server_tick_count{ 0U };
    auto const started_at = std::chrono::steady_clock::now();
    uint32_t last_reported_server_ack = 0U;
    bool have_reported_residency = false;
    bool last_reported_residency = false;
    bool server_exact_rerooted = false;
    bool server_control_rerooted = false;
    server::GameServer::BenchmarkHooks const server_hooks{
        .on_tick = [&](std::chrono::nanoseconds, uint64_t) {
            if (!has_server_player_id.load(std::memory_order_acquire)) {
                return;
            }
            uint32_t const player_id = server_player_id.load(std::memory_order_acquire);
            if (!server_exact_rerooted
                && server.rerootPlayerCoverage(player_id, RECORDED_POSITION)) {
                server_exact_rerooted = true;
                exact_rerooted.store(true, std::memory_order_release);
                CORE_INFO("PF02 exact reroot accepted player_id={} x={} y={} z={} subcells={},{},{}",
                    player_id,
                    RECORDED_POSITION.x,
                    RECORDED_POSITION.y,
                    RECORDED_POSITION.z,
                    RECORDED_POSITION.x_subcell,
                    RECORDED_POSITION.y_subcell,
                    RECORDED_POSITION.z_subcell);
            }
            if (server_exact_rerooted
                && exact_replay_acknowledged.load(std::memory_order_acquire)
                && !server_control_rerooted
                && server.rerootPlayerCoverage(player_id, flat_control_position)) {
                server_control_rerooted = true;
                control_rerooted.store(true, std::memory_order_release);
                CORE_INFO("PF02 flat control reroot accepted player_id={} x={} y={} z={} surface_z={}",
                    player_id,
                    flat_control_position.x,
                    flat_control_position.y,
                    flat_control_position.z,
                    flat_control->surface_height);
            }
        },
        .on_preview_buffered = {},
        .on_preview_metrics = [&](core::ClientId, server::GameServer::PreviewStreamMetrics const metrics) {
            near_required_chunks_all_resident.store(
                metrics.near_required_chunks_all_resident,
                std::memory_order_release
            );
            if (metrics.server_tick == std::chrono::nanoseconds::zero()) {
                return;
            }
            uint32_t const tick = server_tick_count.fetch_add(1U, std::memory_order_relaxed) + 1U;
            bool const residency_changed = !have_reported_residency
                || metrics.near_required_chunks_all_resident != last_reported_residency;
            if (!residency_changed && tick % 10U != 0U
                && metrics.acknowledged_input_sequence == last_reported_server_ack) {
                return;
            }
            have_reported_residency = true;
            last_reported_residency = metrics.near_required_chunks_all_resident;
            last_reported_server_ack = metrics.acknowledged_input_sequence;
            CORE_INFO("PF02 server tick={} elapsed_ms={} received={} acknowledged={} pending={} "
                "materialization_failures={} near_resident={} loop_interval_ns={} loop_work_ns={} tick_ns={}",
                tick,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started_at).count(),
                metrics.latest_received_input_sequence,
                metrics.acknowledged_input_sequence,
                metrics.pending_input_count,
                metrics.materialization_admission_failures,
                metrics.near_required_chunks_all_resident,
                metrics.server_loop_interval.count(),
                metrics.server_loop_work.count(),
                metrics.server_tick.count());
        },
        .before_height_tile_completion = {},
    };
    std::thread server_thread{ [&] { server.run(stop_server, &server_hooks); } };

    BenchmarkClient game_client{ shared::WorldMode::Flight };
    uint32_t recorded_inputs_sent = 0U;
    uint32_t flat_control_inputs_sent = 0U;
    uint32_t last_recorded_sequence = 0U;
    uint32_t last_flat_control_sequence = 0U;
    uint32_t latest_acknowledged_sequence = 0U;
    uint32_t authority_sample_count = 0U;
    uint32_t invalid_capability_count = 0U;
    MovementPhase current_phase = MovementPhase::Waiting;
    bool start_requested = false;
    bool collision_window_queried = false;
    std::optional<Pose> exact_pose;
    std::optional<Pose> exact_replay_end;
    std::optional<Pose> flat_control_start;
    std::optional<Pose> flat_control_end;
    auto poseFromPlayer = [](shared::Player const& player) -> Pose {
        return {
            shared::playerPositionX(player),
            shared::playerPositionY(player),
            shared::playerPositionZ(player),
        };
    };
    auto poseFromPresentation = [](std::optional<client::PlayerPresentationPosition> const& position) -> Pose {
        return position.has_value()
            ? Pose{ position->x, position->y, position->z }
            : Pose{ -1.0, -1.0, -1.0 };
    };
    auto samePosition = [](shared::Player const& player, shared::PlayerPosition const position) {
        return player.x == position.x
            && player.y == position.y
            && player.z == position.z
            && player.x_subcell == position.x_subcell
            && player.y_subcell == position.y_subcell
            && player.z_subcell == position.z_subcell;
    };
    auto logRecordedCollisionWindow = [&] {
        shared::SparseWorld const* const authority_world = game_client.authoritativeWorld();
        if (authority_world == nullptr) {
            CORE_INFO("PF02 recorded collision window unavailable: authoritative world not attached");
            return;
        }

        shared::Direction const first_direction = recorded_inputs.front();
        int32_t const direction_x = first_direction.x;
        int32_t const direction_y = first_direction.y;
        uint32_t const squared_direction_length = static_cast<uint32_t>(
            direction_x * direction_x + direction_y * direction_y
        );
        uint32_t direction_length = 0U;
        while (direction_length * direction_length < squared_direction_length) {
            ++direction_length;
        }
        uint32_t const divisor = std::max<uint32_t>(127U, direction_length);
        uint32_t const base_step = static_cast<uint32_t>(shared::TICK.count())
            * shared::MOVEMENT_SUBCELLS_PER_TICK / static_cast<uint32_t>(shared::TICK.count());
        int64_t const start_x = static_cast<int64_t>(RECORDED_POSITION.x) * shared::SUBCELLS_PER_CELL
            + RECORDED_POSITION.x_subcell;
        int64_t const start_y = static_cast<int64_t>(RECORDED_POSITION.y) * shared::SUBCELLS_PER_CELL
            + RECORDED_POSITION.y_subcell;
        int64_t const delta_x = static_cast<int64_t>(base_step * static_cast<uint32_t>(direction_x) / divisor)
            * first_direction.speedup;
        int64_t const delta_y = static_cast<int64_t>(base_step * static_cast<uint32_t>(direction_y) / divisor)
            * first_direction.speedup;
        int64_t const end_x = start_x + delta_x;
        int64_t const end_y = start_y + delta_y;
        int64_t const first_x = std::min(start_x, end_x) / shared::SUBCELLS_PER_CELL;
        int64_t const last_x = (std::max(start_x, end_x) + shared::World::PLAYER_WIDTH_SUBCELLS - 1)
            / shared::SUBCELLS_PER_CELL;
        int64_t const first_y = std::min(start_y, end_y) / shared::SUBCELLS_PER_CELL;
        int64_t const last_y = (std::max(start_y, end_y) + shared::World::PLAYER_WIDTH_SUBCELLS - 1)
            / shared::SUBCELLS_PER_CELL;
        int64_t const start_z = static_cast<int64_t>(RECORDED_POSITION.z) * shared::SUBCELLS_PER_CELL
            + RECORDED_POSITION.z_subcell;
        int64_t const first_z = start_z / shared::SUBCELLS_PER_CELL;
        int64_t const last_z = (start_z + shared::World::PLAYER_HEIGHT_SUBCELLS - 1)
            / shared::SUBCELLS_PER_CELL;
        uint32_t queried_cells = 0U;
        uint32_t non_air_cells = 0U;
        uint32_t unknown_cells = 0U;
        auto const logColumn = [&](
            int64_t const x,
            int64_t const y,
            char const* const region,
            bool const count_for_swept_window
        ) {
            uint8_t solid_mask = 0U;
            uint8_t unknown_mask = 0U;
            for (int64_t z = first_z; z <= last_z; ++z) {
                shared::SparseBlockQuery const query = authority_world->queryBlock({
                    .x = x,
                    .y = y,
                    .z = z,
                });
                uint8_t const mask = static_cast<uint8_t>(1U << (z - first_z));
                if (count_for_swept_window) {
                    ++queried_cells;
                }
                if (query.state != shared::GenerationState::Materialized || !query.block.has_value()) {
                    unknown_mask |= mask;
                    if (count_for_swept_window) {
                        ++unknown_cells;
                    }
                } else if (*query.block != shared::Block::Air) {
                    solid_mask |= mask;
                    if (count_for_swept_window) {
                        ++non_air_cells;
                    }
                }
            }
            uint16_t const canonical_top = terrain_world->sampleSurfaceHeight(
                static_cast<uint32_t>(x),
                static_cast<uint32_t>(y)
            );
            CORE_INFO("PF02 recorded {} profile x={} y={} tile_key=({}, {}) tile_sample=({}, {}) "
                "canonical_top={} canonical_height_tile={} z_first={} solid_mask={} unknown_mask={}",
                region,
                x,
                y,
                x / shared::Chunk::SIDE_LENGTH,
                y / shared::Chunk::SIDE_LENGTH,
                x % shared::Chunk::SIDE_LENGTH,
                y % shared::Chunk::SIDE_LENGTH,
                canonical_top,
                static_cast<uint32_t>(canonical_top) + 1U,
                first_z,
                solid_mask,
                unknown_mask);
        };
        for (int64_t y = first_y; y <= last_y; ++y) {
            for (int64_t x = first_x; x <= last_x; ++x) {
                logColumn(x, y, "swept", true);
            }
        }
        for (int64_t y = static_cast<int64_t>(RECORDED_POSITION.y) - 1;
            y <= static_cast<int64_t>(RECORDED_POSITION.y) + 1;
            ++y) {
            for (int64_t x = static_cast<int64_t>(RECORDED_POSITION.x) - 1;
                x <= static_cast<int64_t>(RECORDED_POSITION.x) + 1;
                ++x) {
                logColumn(x, y, "height_tile_boundary", false);
            }
        }
        CORE_INFO("PF02 recorded first-input body window direction=({},{},{}) accelerated={} speedup={} "
            "x_cells=[{}..{}] y_cells=[{}..{}] z_cells=[{}..{}] queried={} non_air={} unknown={}",
            direction_x,
            direction_y,
            static_cast<int8_t>(first_direction.z),
            first_direction.accelerated,
            first_direction.speedup,
            first_x,
            last_x,
            first_y,
            last_y,
            first_z,
            last_z,
            queried_cells,
            non_air_cells,
            unknown_cells);
        collision_window_queried = true;
    };

    client::GameClientBenchmarkHooks const hooks{
        .deadline = std::chrono::steady_clock::now() + DEADLINE,
        .should_stop = [&] {
            return flat_control_inputs_sent == FLAT_CONTROL_INPUTS
                && last_flat_control_sequence != 0U
                && latest_acknowledged_sequence >= last_flat_control_sequence;
        },
        .is_uncapped_phase = {},
        .input_override = [&](uint64_t) -> std::optional<shared::Direction> {
            if (!start_requested
                || !exact_rerooted.load(std::memory_order_acquire)
                || !near_required_chunks_all_resident.load(std::memory_order_acquire)) {
                current_phase = MovementPhase::Waiting;
                return shared::Direction{};
            }
            if (recorded_inputs_sent < recorded_inputs.size()) {
                current_phase = MovementPhase::RecordedPose;
                return recorded_inputs[recorded_inputs_sent];
            }
            if (!exact_replay_acknowledged.load(std::memory_order_acquire)
                || !control_rerooted.load(std::memory_order_acquire)
                || !flat_control_start.has_value()) {
                current_phase = MovementPhase::Waiting;
                return shared::Direction{};
            }
            if (flat_control_inputs_sent < FLAT_CONTROL_INPUTS) {
                current_phase = MovementPhase::FlatControl;
                return shared::Direction{
                    .x = static_cast<uint8_t>(flat_control->horizontal_sign * 127),
                    .y = 0U,
                    .z = 0U,
                    .accelerated = false,
                    .speedup = 5U, // Wire decoding validates the profile even when acceleration is disabled.
                };
            }
            current_phase = MovementPhase::Waiting;
            return shared::Direction{};
        },
        .start_input_override = [&](uint64_t) {
            if (!start_requested
                && readyPromptPresented(game_client)
                && exact_pose.has_value()
                && exact_rerooted.load(std::memory_order_acquire)) {
                start_requested = true;
                return true;
            }
            return false;
        },
        .on_loop = {},
        .on_authoritative_player = [&](shared::Player const& player) {
            server_player_id.store(player.id, std::memory_order_release);
            has_server_player_id.store(true, std::memory_order_release);
        },
        .on_input_sent = [&](shared::ClientInputMessage const& message,
            std::chrono::steady_clock::time_point const sent_at,
            std::chrono::steady_clock::time_point const scheduled_at) {
            if (current_phase == MovementPhase::RecordedPose) {
                if (recorded_inputs_sent == 0U) {
                    logRecordedCollisionWindow();
                }
                ++recorded_inputs_sent;
                last_recorded_sequence = message.sequence;
            } else if (current_phase == MovementPhase::FlatControl) {
                ++flat_control_inputs_sent;
                last_flat_control_sequence = message.sequence;
            }
            if (current_phase != MovementPhase::Waiting) {
                CORE_INFO("PF02 input phase={} seq={} direction=({},{},{}) acked={} sent_ms={} scheduled_ms={}",
                    current_phase == MovementPhase::RecordedPose ? "recorded_pose" : "flat_control",
                    message.sequence,
                    static_cast<int8_t>(message.direction.x),
                    static_cast<int8_t>(message.direction.y),
                    static_cast<int8_t>(message.direction.z),
                    latest_acknowledged_sequence,
                    std::chrono::duration_cast<std::chrono::milliseconds>(sent_at - started_at).count(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(scheduled_at - started_at).count());
            }
        },
        .on_authoritative_state = [&](shared::ServerPlayerPositionMessage const& message,
            shared::Player const& player,
            std::chrono::steady_clock::time_point const observed_at) {
            latest_acknowledged_sequence = message.acknowledged_input_sequence;
            ++authority_sample_count;
            invalid_capability_count += player.movement_capabilities.bits != 1U ? 1U : 0U;
            if (samePosition(player, RECORDED_POSITION)) {
                exact_pose = poseFromPlayer(player);
            }
            if (samePosition(player, flat_control_position) && !flat_control_start.has_value()) {
                flat_control_start = poseFromPlayer(player);
            }
            if (last_recorded_sequence != 0U
                && recorded_inputs_sent == recorded_inputs.size()
                && latest_acknowledged_sequence >= last_recorded_sequence
                && !exact_replay_end.has_value()) {
                exact_replay_acknowledged.store(true, std::memory_order_release);
                exact_replay_end = poseFromPlayer(player);
            }
            if (last_flat_control_sequence != 0U
                && latest_acknowledged_sequence >= last_flat_control_sequence) {
                flat_control_end = poseFromPlayer(player);
            }
            Pose const authoritative = poseFromPlayer(player);
            std::optional<shared::Player> const predicted = game_client.predictedMovementPlayer();
            Pose const predicted_pose = predicted.has_value()
                ? poseFromPlayer(*predicted)
                : Pose{ -1.0, -1.0, -1.0 };
            Pose const presentation = poseFromPresentation(game_client.sampledMovementPresentation(observed_at));
            CORE_INFO("PF02 state ack={} revision={} capabilities={} observed_ms={} authority=({:.4f},{:.4f},{:.4f}) "
                "predicted=({:.4f},{:.4f},{:.4f}) presentation=({:.4f},{:.4f},{:.4f})",
                latest_acknowledged_sequence,
                message.state_revision,
                player.movement_capabilities.bits,
                std::chrono::duration_cast<std::chrono::milliseconds>(observed_at - started_at).count(),
                authoritative[0], authoritative[1], authoritative[2],
                predicted_pose[0], predicted_pose[1], predicted_pose[2],
                presentation[0], presentation[1], presentation[2]);
        },
    };

    game_client.run(core::Address::localhost(server.port()), '@', &hooks);
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();

    std::optional<double> flat_control_delta;
    if (flat_control_start.has_value() && flat_control_end.has_value()) {
        flat_control_delta = static_cast<double>(flat_control->horizontal_sign)
            * ((*flat_control_end)[0] - (*flat_control_start)[0]);
    }
    double const expected_flat_control_delta = static_cast<double>(FLAT_CONTROL_INPUTS)
        * shared::MOVEMENT_SUBCELLS_PER_TICK / shared::SUBCELLS_PER_CELL;
    CORE_INFO("PF02 movement summary started={} exact_rerooted={} exact_pose_observed={} "
        "exact_inputs={} exact_last_sequence={} exact_acknowledged={} exact_end=({},{},{}) "
        "control_rerooted={} control_inputs={} control_last_sequence={} latest_ack={} control_delta={} "
        "expected_control_delta={} authority_samples={} server_ticks={} collision_window_queried={} "
        "invalid_capability_samples={}",
        start_requested,
        exact_rerooted.load(std::memory_order_acquire),
        exact_pose.has_value(),
        recorded_inputs_sent,
        last_recorded_sequence,
        exact_replay_acknowledged.load(std::memory_order_acquire),
        exact_replay_end.has_value() ? (*exact_replay_end)[0] : -1.0,
        exact_replay_end.has_value() ? (*exact_replay_end)[1] : -1.0,
        exact_replay_end.has_value() ? (*exact_replay_end)[2] : -1.0,
        control_rerooted.load(std::memory_order_acquire),
        flat_control_inputs_sent,
        last_flat_control_sequence,
        latest_acknowledged_sequence,
        flat_control_delta.value_or(-1.0),
        expected_flat_control_delta,
        authority_sample_count,
        server_tick_count.load(std::memory_order_relaxed),
        collision_window_queried,
        invalid_capability_count);

    EXPECT_TRUE(start_requested);
    EXPECT_TRUE(exact_rerooted.load(std::memory_order_acquire));
    EXPECT_TRUE(exact_pose.has_value());
    EXPECT_TRUE(exact_replay_acknowledged.load(std::memory_order_acquire));
    EXPECT_EQ(recorded_inputs_sent, recorded_inputs.size());
    EXPECT_TRUE(control_rerooted.load(std::memory_order_acquire));
    EXPECT_EQ(flat_control_inputs_sent, FLAT_CONTROL_INPUTS);
    EXPECT_GE(latest_acknowledged_sequence, last_flat_control_sequence);
    EXPECT_TRUE(collision_window_queried);
    EXPECT_GT(authority_sample_count, 0U);
    EXPECT_GT(server_tick_count.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(invalid_capability_count, 0U);
    ASSERT_TRUE(flat_control_delta.has_value());
    EXPECT_NEAR(*flat_control_delta, expected_flat_control_delta, 0.02);
}
