#include <client/BlockActionPresentation.hpp>
#include <client/ClientAudio.hpp>
#include <client/GameClient.hpp>
#include <client/WorldLoading.hpp>
#include <client/render/BlockInteractionEffects.hpp>
#include <server/GameServer.hpp>
#include <server/WorldPreparation.hpp>
#include <shared/world/BlockInteraction.hpp>
#include <shared/world/World.hpp>
#include <generation/Generation.hpp>
#include <core/net/Address.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

class ImmediateAudioOutput final : public client::ClientAudioOutput {
public:
    [[nodiscard]] bool start() noexcept override { return true; }
    void stop() noexcept override {}
    [[nodiscard]] bool play(std::span<int16_t const> samples) noexcept override
    {
        return !samples.empty();
    }
};

std::shared_ptr<server::WorldPreparation> quickPreparedWorld(shared::WorldConfiguration const& world_configuration)
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

bool waitUntilPrepared(
    std::shared_ptr<server::WorldPreparation> const& preparation,
    std::chrono::seconds const timeout
)
{
    auto const deadline = Clock::now() + timeout;
    while (preparation->latest().state == server::WorldPreparationState::Preparing && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return preparation->latest().state == server::WorldPreparationState::Ready;
}

class BlockActionClient final : public client::GameClient {
public:
    BlockActionClient()
        : GameClient{
              shared::WorldMode::Flight,
              shared::World::canonicalConfiguration(),
              false,
              std::make_unique<ImmediateAudioOutput>(),
          }
    { }

    [[nodiscard]] std::optional<shared::Player> localPlayer() const noexcept
    {
        return m_world.playerByCharacter(m_local_character);
    }

    [[nodiscard]] std::optional<shared::BlockTarget> targetBelow(shared::VoxelRay& ray) const noexcept
    {
        auto const player = localPlayer();
        shared::SparseWorld const* const world = authoritativeWorld();
        if (!player || !world) {
            return std::nullopt;
        }
        ray = {
            .origin_x = shared::playerPositionX(*player) + 0.3125,
            .origin_y = shared::playerPositionY(*player) + 0.3125,
            .origin_z = shared::playerPositionZ(*player) + shared::INTERACTION_PLAYER_EYE_HEIGHT,
            .direction_z = -1.0,
        };
        shared::BlockTargetResult const result = shared::resolveInteractionTarget(
            *world,
            ray,
            shared::InteractionPerspective::FirstPerson,
            true
        );
        return result.status == shared::BlockTargetStatus::Hit ? result.target : std::nullopt;
    }

private:
    shared::Direction input() override { return {}; }

    void render() override
    {
        client::WorldLoadingStatus const status = worldLoadingStatus();
        if (status.state == client::WorldLoadingState::ReadyForStart
            && !status.ready_prompt_frame_rendered_at.has_value()) {
            markLoadingPromptRendered(Clock::now());
        }
    }
};

enum class ScenarioPhase : uint8_t {
    AwaitingInitialStatus,
    AwaitingSurvivalMode,
    AwaitingSurvivalProgress,
    AwaitingCancellation,
    AwaitingCreativeMode,
    AwaitingCreativeBreak,
    AwaitingReplicatedEdit,
    Complete,
};

} // namespace

TEST(BlockActionTransportTest, TimedProgressCancellationAndAcceptedBreakReachPresentation)
{
    static constexpr std::chrono::seconds PREPARATION_TIMEOUT{30};
    static constexpr std::chrono::seconds CLIENT_TIMEOUT{60};

    shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
    auto const preparation = quickPreparedWorld(world_configuration);
    ASSERT_TRUE(waitUntilPrepared(preparation, PREPARATION_TIMEOUT));

    server::GameServer game_server{
        0U,
        {},
        shared::WorldMode::Flight,
        world_configuration,
        2U,
        nullptr,
        preparation,
        server::GameServer::BindMode::Loopback,
    };
    std::atomic_bool stop_server{false};
    std::thread server_thread{[&] { game_server.run(stop_server); }};

    BlockActionClient game_client;
    ScenarioPhase phase = ScenarioPhase::AwaitingInitialStatus;
    std::string failure;
    bool start_requested = false;
    bool pre_play_gate_checked = false;
    bool pre_play_mode_request_rejected = false;
    bool pre_play_action_rejected = false;
    bool saw_survival_progress = false;
    bool saw_cancelled_status = false;
    bool saw_accepted_break_event = false;
    bool saw_replicated_air = false;
    float crack_progress = 0.0F;
    size_t cracked_box_count = 0U;
    size_t cancelled_outline_box_count = 0U;
    size_t particle_box_count = 0U;
    uint64_t original_cell_version = 0U;
    std::optional<shared::BlockTarget> target;
    client::BlockInteractionEffects effects;

    auto fail = [&](std::string message) {
        if (failure.empty()) {
            failure = std::move(message);
        }
        phase = ScenarioPhase::Complete;
    };

    client::GameClientBenchmarkHooks hooks;
    hooks.deadline = Clock::now() + CLIENT_TIMEOUT;
    hooks.should_stop = [&] { return phase == ScenarioPhase::Complete; };
    hooks.start_input_override = [&](uint64_t) {
        client::WorldLoadingStatus const status = game_client.worldLoadingStatus();
        if (!start_requested
            && status.state == client::WorldLoadingState::ReadyForStart
            && status.ready_prompt_frame_rendered_at.has_value()) {
            if (!game_client.activeTerrainSession() || !game_client.authoritativeWorld()) {
                fail("ReadyForStart did not have an accepted terrain session for pre-play gate checks");
                return false;
            }
            pre_play_gate_checked = true;
            pre_play_mode_request_rejected = !game_client.requestGameMode(shared::GameMode::Survival);
            pre_play_action_rejected = !game_client.submitBlockAction(
                shared::BlockActionKind::BreakRelease,
                {},
                std::nullopt,
                shared::Block::Stone
            );
            start_requested = true;
            return true;
        }
        return false;
    };
    hooks.input_override = [](uint64_t) -> std::optional<shared::Direction> {
        return shared::Direction{};
    };
    hooks.on_loop = [&](client::GameClientLoopSample const&) {
        if (phase == ScenarioPhase::Complete
            || game_client.worldLoadingStatus().state != client::WorldLoadingState::Playing) {
            return;
        }
        auto const& current = game_client.blockActionStatus();
        shared::ServerBlockActionResultMessage const* const status = current ? &*current : nullptr;
        if (phase == ScenarioPhase::AwaitingInitialStatus && status && status->sequence == 0U
            && status->status == shared::BlockActionStatus::Accepted) {
            if (!game_client.requestGameMode(shared::GameMode::Survival)) {
                fail("GameClient could not submit the Survival mode request over the local transport");
                return;
            }
            phase = ScenarioPhase::AwaitingSurvivalMode;
        } else if (phase == ScenarioPhase::AwaitingSurvivalMode && status
            && status->sequence != 0U && status->status == shared::BlockActionStatus::Accepted
            && status->mode == shared::GameMode::Survival) {
            shared::VoxelRay ray;
            target = game_client.targetBelow(ray);
            shared::SparseWorld const* const world = game_client.authoritativeWorld();
            if (!target || !world || !shared::isSolidBlock(target->block)
                || target->block == shared::Block::Bedrock) {
                fail("The accepted player eye ray did not resolve a breakable resident terrain block");
                return;
            }
            if (!client::detail::interactionMaterialAllowed(status->break_materials, target->block)) {
                fail("The server did not grant Survival break rights for the resolved target material");
                return;
            }
            auto const query = world->queryBlock(target->cell);
            if (query.state != shared::GenerationState::Materialized || query.block != target->block) {
                fail("The client target does not match its current authoritative block state");
                return;
            }
            original_cell_version = world->cellVersion(target->cell);
            if (!game_client.submitBlockAction(
                    shared::BlockActionKind::BreakHold,
                    ray,
                    target->cell,
                    target->block
                )) {
                fail("GameClient could not submit the Survival break hold");
                return;
            }
            phase = ScenarioPhase::AwaitingSurvivalProgress;
        } else if (phase == ScenarioPhase::AwaitingSurvivalProgress && status && target
            && status->status == shared::BlockActionStatus::Progress && status->target == target->cell
            && status->completed_intervals > 0U) {
            if (status->required_intervals == 0U
                || status->completed_intervals >= status->required_intervals) {
                fail("The server did not report incomplete timed-break intervals");
                return;
            }
            crack_progress = client::detail::authoritativeBreakProgress(*status);
            effects.setTarget(target, crack_progress);
            cracked_box_count = effects.boxes(Clock::now()).size();
            if (crack_progress <= 0.0F || cracked_box_count <= 6U) {
                fail("Server-confirmed timed progress did not produce visible crack geometry");
                return;
            }
            saw_survival_progress = true;
            if (!game_client.submitBlockAction(
                    shared::BlockActionKind::BreakRelease,
                    {},
                    std::nullopt,
                    target->block
                )) {
                fail("GameClient could not submit the break release");
                return;
            }
            phase = ScenarioPhase::AwaitingCancellation;
        } else if (phase == ScenarioPhase::AwaitingCancellation && status
            && status->status == shared::BlockActionStatus::Cancelled
            && status->mode == shared::GameMode::Survival) {
            effects.setTarget(target, client::detail::authoritativeBreakProgress(*status));
            cancelled_outline_box_count = effects.boxes(Clock::now()).size();
            shared::SparseWorld const* const world = game_client.authoritativeWorld();
            if (!world || !target) {
                fail("The authoritative target world was unavailable when cancellation arrived");
                return;
            }
            auto const query = world->queryBlock(target->cell);
            if (client::detail::authoritativeBreakProgress(*status) != 0.0F
                || cancelled_outline_box_count != 12U
                || query.state != shared::GenerationState::Materialized
                || query.block != target->block
                || world->cellVersion(target->cell) != original_cell_version) {
                fail("Cancellation did not clear cracks while preserving the authoritative target cell");
                return;
            }
            saw_cancelled_status = true;
            if (!game_client.requestGameMode(shared::GameMode::Creative)) {
                fail("GameClient could not submit the Creative mode request");
                return;
            }
            phase = ScenarioPhase::AwaitingCreativeMode;
        } else if (phase == ScenarioPhase::AwaitingCreativeMode && status
            && status->sequence != 0U && status->status == shared::BlockActionStatus::Accepted
            && status->mode == shared::GameMode::Creative) {
            shared::VoxelRay ray;
            auto const current_target = game_client.targetBelow(ray);
            if (!current_target || !target || current_target->cell != target->cell
                || current_target->block != target->block
                || !game_client.submitBlockAction(
                    shared::BlockActionKind::BreakHold,
                    ray,
                    target->cell,
                    target->block
                )) {
                fail("The reachable block could not be submitted for a Creative quick break");
                return;
            }
            phase = ScenarioPhase::AwaitingCreativeBreak;
        } else if (phase == ScenarioPhase::AwaitingCreativeBreak && status && target
            && status->status == shared::BlockActionStatus::Accepted && status->target == target->cell
            && status->mode == shared::GameMode::Creative) {
            phase = ScenarioPhase::AwaitingReplicatedEdit;
        }

        while (auto const accepted = game_client.consumeAcceptedBlockActionResult()) {
            if (!target || accepted->status != shared::BlockActionStatus::Accepted
                || accepted->target != target->cell) {
                continue;
            }
            saw_accepted_break_event = effects.acceptBreak(
                {
                    .join_epoch = accepted->session.join_epoch,
                    .server_tick = accepted->server_tick,
                    .sequence = accepted->sequence,
                    .cell_version = accepted->cell_version,
                },
                accepted->target,
                target->block,
                Clock::now()
            );
            if (!saw_accepted_break_event || accepted->cell_version != original_cell_version + 1U) {
                fail("The accepted break event FIFO did not reach the presentation effects with its new cell version");
                return;
            }
            effects.setTarget(std::nullopt, 0.0F);
            particle_box_count = effects.boxes(Clock::now()).size();
        }

        if (phase == ScenarioPhase::AwaitingReplicatedEdit && saw_accepted_break_event && target) {
            shared::SparseWorld const* const world = game_client.authoritativeWorld();
            if (!world) {
                fail("The authoritative client world disappeared after the accepted break");
                return;
            }
            auto const query = world->queryBlock(target->cell);
            if (query.state == shared::GenerationState::Materialized
                && query.block == shared::Block::Air
                && world->cellVersion(target->cell) == original_cell_version + 1U) {
                saw_replicated_air = true;
                if (particle_box_count != client::BlockInteractionEffects::PARTICLES_PER_BREAK) {
                    fail("The accepted break event did not produce its bounded UI particle set");
                    return;
                }
                phase = ScenarioPhase::Complete;
            }
        }
    };

    std::thread client_thread{[&] {
        game_client.run(core::Address::localhost(game_server.port()), '@', &hooks);
    }};
    client_thread.join();
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();

    SCOPED_TRACE(failure);
    EXPECT_EQ(phase, ScenarioPhase::Complete);
    EXPECT_TRUE(start_requested);
    EXPECT_TRUE(pre_play_gate_checked);
    EXPECT_TRUE(pre_play_mode_request_rejected);
    EXPECT_TRUE(pre_play_action_rejected);
    EXPECT_TRUE(saw_survival_progress);
    EXPECT_TRUE(saw_cancelled_status);
    EXPECT_TRUE(saw_accepted_break_event);
    EXPECT_TRUE(saw_replicated_air);
    EXPECT_GT(crack_progress, 0.0F);
    EXPECT_GT(cracked_box_count, 6U);
    EXPECT_EQ(cancelled_outline_box_count, 12U);
    EXPECT_EQ(particle_box_count, client::BlockInteractionEffects::PARTICLES_PER_BREAK);
}
