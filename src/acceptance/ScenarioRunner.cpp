#include <acceptance/ScenarioRunner.hpp>

#include <server/GameServer.hpp>

#include <shared/net/Message.hpp>
#include <shared/policy/Policy.hpp>
#include <shared/world/SparseWorld.hpp>

#include <core/net/Client.hpp>
#include <core/net/Net.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace acceptance {

namespace {

[[nodiscard]]
std::chrono::milliseconds remainingTimeout(
    std::chrono::steady_clock::time_point const deadline
);

[[nodiscard]]
shared::PolicyCapabilityRegistry movementPermissionRegistry();

[[nodiscard]]
std::string movementPermissionPolicySource(bool flight, bool collision_bypass);

class ScenarioClient final : public core::Client {
public:
    ScenarioClient()
        : core::Client{ 1 }
    { }

    [[nodiscard]]
    bool sendMessage(shared::Message const& message)
    {
        return send(shared::encodeMessage(message), 0, core::SendMode{ core::SendMode::Reliable });
    }

    template<typename Predicate>
    [[nodiscard]]
    bool waitFor(
        Predicate const& predicate,
        std::chrono::steady_clock::time_point const deadline,
        std::chrono::milliseconds const poll_interval
    )
    {
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            poll(detail::boundedNetworkPollTimeout(poll_interval, remainingTimeout(deadline)));
        }
        return true;
    }

    [[nodiscard]]
    bool accepted() const
    {
        return std::ranges::any_of(m_messages, [](shared::Message const& message) {
            auto const* const response = std::get_if<shared::JoinResponseMessage>(&message);
            return response != nullptr && response->accepted;
        });
    }

    [[nodiscard]]
    std::optional<shared::ServerPlayerPositionMessage> latestPosition(char const character) const
    {
        for (auto message = m_messages.rbegin(); message != m_messages.rend(); ++message) {
            auto const* const position = std::get_if<shared::ServerPlayerPositionMessage>(&*message);
            if (position != nullptr && position->ch == character) {
                return *position;
            }
        }
        return std::nullopt;
    }

private:
    void onDisconnected(core::DisconnectEvent const) override
    { }

    void onReceived(core::ReceiveEvent event) override
    {
        std::optional<shared::Message> const message = shared::decodeMessage(event.data);
        if (message.has_value()) {
            m_messages.push_back(*message);
        }
    }

private:
    std::vector<shared::Message> m_messages;
};

class ScenarioServerController final {
public:
    ScenarioServerController(
        std::vector<server::GameServer::SpawnPoint> spawn_points,
        shared::WorldMode const world_mode,
        std::chrono::milliseconds const poll_interval,
        std::chrono::steady_clock::time_point const deadline
    )
        : m_server{
            0,
            std::move(spawn_points),
            world_mode,
            shared::World::canonicalConfiguration(),
            shared::MIN_HEIGHT_TILE_INTEREST_RADIUS
        }
        , m_poll_interval(poll_interval)
        , m_deadline(deadline)
    { }

    ~ScenarioServerController()
    {
        stop();
    }

    void start()
    {
        if (m_worker.has_value()) {
            return;
        }
        m_stop_requested.store(false, std::memory_order_relaxed);
        m_worker.emplace([this] {
            while (!m_stop_requested.load(std::memory_order_relaxed)) {
                static_cast<void>(tick(detail::boundedNetworkPollTimeout(
                    m_poll_interval,
                    remainingTimeout(m_deadline)
                )));
            }
        });
    }

    void stop()
    {
        if (!m_worker.has_value()) {
            return;
        }
        m_stop_requested.store(true, std::memory_order_relaxed);
        m_worker->join();
        m_worker.reset();
    }

    [[nodiscard]]
    uint64_t tick(std::chrono::milliseconds const timeout)
    {
        uint64_t const events = m_server.tick(timeout);
        m_server.flush();
        m_events_processed.fetch_add(events);
        m_ticks.fetch_add(1U);
        return events;
    }

    [[nodiscard]]
    std::expected<uint64_t, std::string> publishMovementPermissions(
        bool const flight,
        bool const collision_bypass
    )
    {
        if (m_worker.has_value()) {
            return std::unexpected("scenario movement permissions require a stopped server tick loop");
        }
        if (collision_bypass && !flight) {
            return std::unexpected("collision bypass requires flight permission");
        }
        shared::PolicyHost compiler;
        std::string const source = movementPermissionPolicySource(flight, collision_bypass);
        auto compilation = compiler.compile("scenario-movement-permissions.core", source);
        if (!compilation) {
            return std::unexpected("scenario permission compilation failed: " + compilation.error().message);
        }
        auto const published = m_server.publishPermissions(
            std::move(*compilation),
            movementPermissionRegistry()
        );
        if (!published) {
            return std::unexpected("scenario permission publication failed: " + published.error().message);
        }
        m_server.flush();
        return *published;
    }

    [[nodiscard]]
    uint64_t eventsProcessed() const
    {
        return m_events_processed.load();
    }

    [[nodiscard]]
    uint64_t ticks() const
    {
        return m_ticks.load();
    }

    [[nodiscard]]
    uint16_t port() const
    {
        return m_server.port();
    }

private:
    server::GameServer m_server;
    std::chrono::milliseconds m_poll_interval;
    std::chrono::steady_clock::time_point m_deadline;
    std::optional<std::thread> m_worker;
    std::atomic_bool m_stop_requested{ false };
    std::atomic<uint64_t> m_events_processed{ 0 };
    std::atomic<uint64_t> m_ticks{ 0 };
};

[[nodiscard]]
std::optional<uint64_t> actorIndex(shared::ScenarioPlan const& plan, shared::ScenarioActorId const id)
{
    auto const actor = std::ranges::find(plan.actors(), id, &shared::ScenarioActor::id);
    if (actor == plan.actors().end()) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(actor - plan.actors().begin());
}

[[nodiscard]]
shared::WorldMode serverWorldMode(shared::ScenarioProfile const profile) noexcept
{
    return profile == shared::ScenarioProfile::Flight3dV1
        ? shared::WorldMode::Flight : shared::WorldMode::Flat;
}

[[nodiscard]]
std::chrono::milliseconds remainingTimeout(
    std::chrono::steady_clock::time_point const deadline
)
{
    std::chrono::steady_clock::duration const remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
        return std::chrono::milliseconds::zero();
    }
    std::chrono::milliseconds const rounded = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    return std::max(rounded, std::chrono::milliseconds{ 1 });
}

[[nodiscard]]
std::string operationFailure(shared::ScenarioOperation const& operation, std::string const& message)
{
    return "scenario operation at " + std::to_string(operation.location.line) + ":"
        + std::to_string(operation.location.column) + " " + message;
}

[[nodiscard]]
shared::PolicyCapabilityRegistry movementPermissionRegistry()
{
    return {
        .definitions = {
            {
                .key = "minecraft:flight",
                .default_value = 1,
                .minimum_value = 0,
                .maximum_value = 1,
                .hard_restriction = shared::PolicyRestriction::Maximum,
            },
            {
                .key = "minecraft:collision-bypass",
                .default_value = 1,
                .minimum_value = 0,
                .maximum_value = 1,
                .hard_restriction = shared::PolicyRestriction::Maximum,
            },
        },
    };
}

[[nodiscard]]
std::string movementPermissionPolicySource(bool const flight, bool const collision_bypass)
{
    std::string source =
        "@version(\"0.1.3.1\")\n"
        "@use minecraft\n"
        "pub fn policy() {\n"
        "    policyRule(\"scenario-movement\", \"minecraft:flight\", ";
    source.append(flight ? "1i64" : "0i64");
    source.append(", 0u8)\n    policyRule(\"scenario-movement\", \"minecraft:collision-bypass\", ");
    source.append(collision_bypass ? "1i64" : "0i64");
    source.append(", 0u8)\n    policyAssign(\"players\", \"scenario-movement\", 0u8)\n}\n");
    return source;
}

[[nodiscard]]
std::expected<RuntimeEvidence, std::string> runSparseWorldScenario(
    shared::ScenarioPlan const& plan,
    ScenarioRunOptions const& options,
    std::chrono::steady_clock::time_point const started_at
)
{
    if (!plan.actors().empty() || plan.totalTicks() != 0U || plan.operations().empty()) {
        return std::unexpected("sparse-world scenarios cannot contain players or tick operations");
    }
    auto const* const configuration = std::get_if<shared::ScenarioSparseWorldOptionsOperation>(
        &plan.operations().front().data
    );
    if (configuration == nullptr || configuration->generator_version != 1U
        || configuration->max_resident_chunks == 0U) {
        return std::unexpected("sparse-world scenario requires supported generator options first");
    }

    shared::SparseWorld world{
        shared::SparseWorldOptions{
            .seed = plan.seed(),
            .max_resident_chunks = configuration->max_resident_chunks,
        },
    };
    std::chrono::steady_clock::time_point const deadline = started_at + options.deadline;
    uint64_t expectations_passed{0U};
    for (uint64_t index = 1U; index < static_cast<uint64_t>(plan.operations().size()); ++index) {
        shared::ScenarioOperation const& operation = plan.operations()[index];
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(operationFailure(operation, "exceeded the monotonic deadline"));
        }
        if (operation.boundary != 0U) {
            return std::unexpected(operationFailure(operation, "has an inconsistent sparse-world boundary"));
        }
        if (auto const* const expected = std::get_if<shared::ScenarioExpectBlockOperation>(&operation.data)) {
            std::optional<shared::Block> const observed = world.blockAt({
                .x = expected->x,
                .y = expected->y,
                .z = expected->z,
            });
            if (!observed.has_value() || *observed != expected->block) {
                return std::unexpected(operationFailure(operation, "did not observe the expected generated block"));
            }
        } else if (auto const* const expected = std::get_if<
                       shared::ScenarioExpectResidentChunksOperation
                   >(&operation.data)) {
            if (world.residentChunkCount() != expected->count) {
                return std::unexpected(operationFailure(operation, "observed an unexpected resident chunk count"));
            }
        } else {
            return std::unexpected(operationFailure(operation, "contains an unsupported sparse-world operation"));
        }
        if (world.residentChunkCount() > world.maxResidentChunks()) {
            return std::unexpected(operationFailure(operation, "exceeded its configured residency bound"));
        }
        ++expectations_passed;
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(operationFailure(operation, "exceeded the monotonic deadline"));
        }
    }
    if (expectations_passed != plan.evidenceCount()) {
        return std::unexpected("sparse-world evidence count did not match executed observations");
    }

    return RuntimeEvidence{
        .mode = "scenario",
        .scenario_version = std::to_string(plan.version()),
        .profile = std::string{shared::scenarioProfileName(plan.profile())},
        .seed = plan.seed(),
        .ticks = 0U,
        .clients_requested = 0U,
        .clients_accepted = 0U,
        .server_events_processed = 0U,
        .accepted_tick = 0U,
        .last_effective_tick = 0U,
        .inputs_sent = 0U,
        .camera_relative_inputs = 0U,
        .expectations_passed = expectations_passed,
        .authoritative_tick_ms = 0U,
        .replay_id = shared::scenarioReplayId(plan),
        .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at
        ),
        .deadline = std::chrono::duration_cast<std::chrono::milliseconds>(options.deadline),
        .passed = true,
    };
}

} // namespace

std::expected<RuntimeEvidence, std::string> runScenario(
    shared::ScenarioPlan const& plan,
    ScenarioRunOptions const& options
)
{
    if (options.deadline <= std::chrono::seconds::zero()
        || options.network_poll_interval <= std::chrono::milliseconds::zero()
    ) {
        return std::unexpected("scenario runner requires positive monotonic limits");
    }
    if (plan.profile() == shared::ScenarioProfile::SparseWorldV1) {
        return runSparseWorldScenario(plan, options, std::chrono::steady_clock::now());
    }
    if (plan.profile() != shared::ScenarioProfile::Flat2dV1
        && plan.profile() != shared::ScenarioProfile::Flat3dV1
        && plan.profile() != shared::ScenarioProfile::Flight3dV1) {
        return std::unexpected("scenario runner does not support this profile");
    }
    static constexpr uint64_t MAX_SERVER_CLIENTS{ 4 };
    if (static_cast<uint64_t>(plan.actors().size()) > MAX_SERVER_CLIENTS) {
        return std::unexpected("scenario runner supports at most 4 actors per server");
    }

    std::vector<server::GameServer::SpawnPoint> spawn_points;
    shared::WorldMode const world_mode = serverWorldMode(plan.profile());
    spawn_points.reserve(plan.actors().size());
    for (shared::ScenarioActor const& actor : plan.actors()) {
        spawn_points.push_back(server::GameServer::SpawnPoint{
            .character = actor.character,
            .x = actor.x,
            .y = actor.y,
            .z = actor.z,
        });
    }
    auto validated_spawn_points = server::GameServer::validateSpawnPoints(std::move(spawn_points), world_mode);
    if (!validated_spawn_points.has_value()) {
        return std::unexpected(validated_spawn_points.error());
    }

    core::Net::ensureInit();
    std::chrono::steady_clock::time_point const started_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point const deadline = started_at + options.deadline;
    ScenarioServerController server{
        std::move(*validated_spawn_points),
        world_mode,
        options.network_poll_interval,
        deadline,
    };
    std::vector<std::unique_ptr<ScenarioClient>> clients;
    clients.reserve(plan.actors().size());
    std::vector<std::optional<shared::Direction>> active_inputs(plan.actors().size());
    std::vector<uint8_t> pending_jumps(plan.actors().size(), 0U);
    std::vector<uint32_t> input_sequences(plan.actors().size(), 1U);
    std::optional<std::string> failure;
    uint64_t logical_tick{ 0 };
    uint64_t inputs_sent{ 0 };
    uint64_t camera_relative_inputs{ 0 };
    uint64_t expectations_passed{ 0 };
    uint64_t last_effective_tick{ 0 };
    uint64_t clients_accepted{ 0 };
    uint64_t accepted_tick{ 0 };

    server.start();
    for (shared::ScenarioActor const& actor : plan.actors()) {
        if (remainingTimeout(deadline) == std::chrono::milliseconds::zero()) {
            failure = "scenario runner exceeded its monotonic deadline while connecting clients";
            break;
        }
        auto client = std::make_unique<ScenarioClient>();
        if (!client->connect(core::Address::localhost(server.port()), remainingTimeout(deadline))) {
            failure = "scenario client could not connect";
            break;
        }
        if (!client->sendMessage(shared::JoinRequestMessage{
            .ch = actor.character,
            .mode = world_mode,
            .configuration = shared::World::canonicalConfiguration(),
        })) {
            failure = "scenario client could not send its join request";
            break;
        }
        client->flush();
        char const character = actor.character;
        if (!client->waitFor(
            [&client, character] {
                return client->accepted() && client->latestPosition(character).has_value();
            },
            deadline,
            options.network_poll_interval
        )) {
            failure = "scenario client was not accepted before the monotonic deadline";
            break;
        }
        clients.push_back(std::move(client));
        ++clients_accepted;
    }
    if (!failure.has_value()) {
        accepted_tick = server.ticks();
        server.stop();
    }

    auto advanceOneTick = [&]() -> std::expected<void, std::string> {
        uint64_t active_count{ 0 };
        std::vector<std::optional<uint32_t>> submitted_sequences(clients.size());
        for (uint64_t index{ 0 }; index < static_cast<uint64_t>(clients.size()); ++index) {
            bool const jump = pending_jumps[index] != 0U;
            if (!active_inputs[index].has_value() && !jump) {
                continue;
            }
            shared::Direction direction = active_inputs[index].value_or(shared::Direction{});
            if (jump) {
                direction.z = 127U;
                pending_jumps[index] = 0U;
                if (active_inputs[index].has_value()) {
                    active_inputs[index]->z = 0U;
                }
            }
            uint32_t const sequence = input_sequences[index]++;
            if (!clients[index]->sendMessage(shared::ClientInputMessage{
                .direction = direction,
                .sequence = sequence,
            })) {
                return std::unexpected("scenario client could not send active input");
            }
            clients[index]->flush();
            submitted_sequences[index] = sequence;
            ++active_count;
            ++inputs_sent;
        }
        bool acknowledged = active_count == 0U;
        do {
            if (std::chrono::steady_clock::now() >= deadline) {
                return std::unexpected("scenario runner exceeded its monotonic deadline at a tick barrier");
            }
            static_cast<void>(server.tick(detail::boundedNetworkPollTimeout(
                options.network_poll_interval,
                remainingTimeout(deadline)
            )));
            // Unknown collision chunks defer input until materialization; packet receipt alone is not completion.
            acknowledged = true;
            for (uint64_t index{ 0 }; index < static_cast<uint64_t>(clients.size()); ++index) {
                if (!submitted_sequences[index].has_value()) {
                    continue;
                }
                static_cast<void>(clients[index]->poll(std::chrono::milliseconds::zero()));
                std::optional<shared::ServerPlayerPositionMessage> const position =
                    clients[index]->latestPosition(plan.actors()[index].character);
                if (!position.has_value()
                    || position->acknowledged_input_sequence != *submitted_sequences[index]) {
                    acknowledged = false;
                }
            }
        } while (!acknowledged);
        ++logical_tick;
        return { };
    };

    if (!failure.has_value()) {
        for (shared::ScenarioOperation const& operation : plan.operations()) {
            if (operation.boundary != logical_tick) {
                failure = operationFailure(operation, "has an inconsistent plan boundary");
                break;
            }
            if (auto const* const input = std::get_if<shared::ScenarioInputOperation>(&operation.data)) {
                if (input->effective_boundary != logical_tick + 1U) {
                    failure = operationFailure(operation, "has an inconsistent effective input boundary");
                    break;
                }
                auto const index = actorIndex(plan, input->actor);
                if (!index.has_value()) {
                    failure = operationFailure(operation, "references an unknown actor");
                    break;
                }
                if (input->intent != shared::ScenarioInputOperation::Intent::Direct) {
                    char const character = plan.actors()[*index].character;
                    bool const requires_phase = input->intent == shared::ScenarioInputOperation::Intent::Phase;
                    bool const permission_observed = clients[*index]->waitFor(
                        [&client = clients[*index], character, requires_phase] {
                            auto const position = client->latestPosition(character);
                            auto const flight = shared::MovementCapability::Flight;
                            auto const phase = shared::MovementCapability::CollisionBypass;
                            bool const has_flight = position.has_value() && position->movement_capabilities.allows(flight);
                            bool const has_phase = position.has_value() && position->movement_capabilities.allows(phase);
                            return has_flight && (!requires_phase || has_phase);
                        },
                        deadline,
                        options.network_poll_interval
                    );
                    if (!permission_observed) {
                        char const* const reason = input->intent == shared::ScenarioInputOperation::Intent::Flight
                            ? "flightXYZ requires server-granted flight permission"
                            : "phaseXYZ requires server-granted collision-bypass permission";
                        failure = operationFailure(operation, reason);
                        break;
                    }
                }
                active_inputs[*index] = shared::Direction{
                    .x = static_cast<uint8_t>(input->x * 127),
                    .y = static_cast<uint8_t>(input->y * 127),
                    .z = static_cast<uint8_t>(input->z * 127),
                };
                last_effective_tick = input->effective_boundary;
            } else if (auto const* const input = std::get_if<shared::ScenarioCameraInputOperation>(&operation.data)) {
                if ((plan.profile() != shared::ScenarioProfile::Flat3dV1
                        && plan.profile() != shared::ScenarioProfile::Flight3dV1)
                    || input->effective_boundary != logical_tick + 1U) {
                    failure = operationFailure(operation, "has an inconsistent camera input boundary");
                    break;
                }
                auto const index = actorIndex(plan, input->actor);
                if (!index.has_value()) {
                    failure = operationFailure(operation, "references an unknown actor");
                    break;
                }
                shared::ScenarioActor const& actor = plan.actors()[*index];
                active_inputs[*index] = shared::scenarioCameraRelativeDirection(
                    {
                        .yaw_degrees = actor.yaw_degrees,
                        .strafe = input->strafe,
                        .forward = input->forward,
                        .vertical = input->vertical,
                    }
                );
                ++camera_relative_inputs;
                last_effective_tick = input->effective_boundary;
            } else if (auto const* const permissions = std::get_if<shared::ScenarioMovementPermissionsOperation>(
                &operation.data
            )) {
                auto const published = server.publishMovementPermissions(
                    permissions->flight,
                    permissions->collision_bypass
                );
                if (!published.has_value()) {
                    failure = operationFailure(operation, published.error());
                    break;
                }
            } else if (auto const* const jump = std::get_if<shared::ScenarioJumpOperation>(&operation.data)) {
                auto const index = actorIndex(plan, jump->actor);
                if (!index.has_value()) {
                    failure = operationFailure(operation, "references an unknown actor");
                    break;
                }
                char const character = plan.actors()[*index].character;
                bool const flight_disabled = clients[*index]->waitFor(
                    [&client = clients[*index], character] {
                        auto const position = client->latestPosition(character);
                        return position.has_value()
                            && !position->movement_capabilities.allows(shared::MovementCapability::Flight);
                    },
                    deadline,
                    options.network_poll_interval
                );
                if (!flight_disabled) {
                    failure = operationFailure(operation, "jump requires flight permission to be disabled");
                    break;
                }
                pending_jumps[*index] = 1U;
                last_effective_tick = logical_tick + 1U;
                auto const advanced = advanceOneTick();
                if (!advanced.has_value()) {
                    failure = operationFailure(operation, advanced.error());
                    break;
                }
            } else if (auto const* const wait = std::get_if<shared::ScenarioWaitOperation>(&operation.data)) {
                for (uint64_t tick{ 0 }; tick < wait->ticks; ++tick) {
                    auto const advanced = advanceOneTick();
                    if (!advanced.has_value()) {
                        failure = operationFailure(operation, advanced.error());
                        break;
                    }
                }
                if (failure.has_value()) {
                    break;
                }
            } else if (auto const* const expectation = std::get_if<shared::ScenarioExpectPositionOperation>(
                &operation.data
            )) {
                auto const index = actorIndex(plan, expectation->actor);
                if (!index.has_value()) {
                    failure = operationFailure(operation, "references an unknown actor");
                    break;
                }
                char const character = plan.actors()[*index].character;
                auto const wrap = [](int32_t value) noexcept {
                    constexpr int32_t EXTENT = shared::World::FLIGHT_MAX_CELL + 1;
                    return (value % EXTENT + EXTENT) % EXTENT;
                };
                int32_t const expected_x = plan.profile() == shared::ScenarioProfile::Flight3dV1
                    ? wrap(expectation->x) : expectation->x;
                int32_t const expected_y = plan.profile() == shared::ScenarioProfile::Flight3dV1
                    ? wrap(expectation->y) : expectation->y;
                if (!clients[*index]->waitFor(
                    [&client = clients[*index], character, expectation, expected_x, expected_y] {
                        auto const position = client->latestPosition(character);
                        return position.has_value()
                            && position->x == expected_x
                            && position->y == expected_y
                            && position->z == expectation->z;
                    },
                    deadline,
                    options.network_poll_interval
                )) {
                    std::optional<shared::ServerPlayerPositionMessage> const observed =
                        clients[*index]->latestPosition(character);
                    std::string reason = "did not observe expected authoritative position ("
                        + std::to_string(expected_x) + ", " + std::to_string(expected_y) + ", "
                        + std::to_string(expectation->z) + ")";
                    if (observed.has_value()) {
                        reason += "; last observed (" + std::to_string(observed->x) + ", "
                            + std::to_string(observed->y) + ", " + std::to_string(observed->z)
                            + "), ack=" + std::to_string(observed->acknowledged_input_sequence)
                            + ", velocity=" + std::to_string(observed->vertical_velocity_subcells)
                            + ", capabilities=" + std::to_string(observed->movement_capabilities.bits);
                    } else {
                        reason += "; no authoritative position was received";
                    }
                    failure = operationFailure(operation, std::move(reason));
                    break;
                }
                ++expectations_passed;
            } else if (auto const* const expectation = std::get_if<
                           shared::ScenarioExpectMovementPermissionsOperation
                       >(&operation.data)) {
                auto const index = actorIndex(plan, expectation->actor);
                if (!index.has_value()) {
                    failure = operationFailure(operation, "references an unknown actor");
                    break;
                }
                char const character = plan.actors()[*index].character;
                uint8_t const expected_bits = static_cast<uint8_t>(
                    (expectation->flight ? static_cast<uint8_t>(shared::MovementCapability::Flight) : 0U)
                    | (expectation->collision_bypass
                        ? static_cast<uint8_t>(shared::MovementCapability::CollisionBypass) : 0U)
                );
                if (!clients[*index]->waitFor(
                    [&client = clients[*index], character, expected_bits] {
                        auto const position = client->latestPosition(character);
                        return position.has_value() && position->movement_capabilities.bits == expected_bits;
                    },
                    deadline,
                    options.network_poll_interval
                )) {
                    std::optional<shared::ServerPlayerPositionMessage> const observed =
                        clients[*index]->latestPosition(character);
                    std::string reason = "did not observe an authoritative movement permission state";
                    if (observed.has_value()) {
                        reason = "expected movement capability bits " + std::to_string(expected_bits)
                            + " but observed " + std::to_string(observed->movement_capabilities.bits);
                    }
                    failure = operationFailure(operation, reason);
                    break;
                }
                ++expectations_passed;
            } else if (auto const* const expectation = std::get_if<
                           shared::ScenarioExpectVerticalVelocityOperation
                       >(&operation.data)) {
                auto const index = actorIndex(plan, expectation->actor);
                if (!index.has_value()) {
                    failure = operationFailure(operation, "references an unknown actor");
                    break;
                }
                char const character = plan.actors()[*index].character;
                if (!clients[*index]->waitFor(
                    [&client = clients[*index], character, expectation] {
                        auto const position = client->latestPosition(character);
                        return position.has_value()
                            && position->vertical_velocity_subcells == expectation->velocity_subcells;
                    },
                    deadline,
                    options.network_poll_interval
                )) {
                    failure = operationFailure(operation, "did not observe the expected authoritative vertical velocity");
                    break;
                }
                ++expectations_passed;
            }
        }
    }

    if (!failure.has_value() && logical_tick != plan.totalTicks()) {
        failure = "scenario plan total tick count did not match executed tick barriers";
    }
    if (!failure.has_value() && expectations_passed != plan.evidenceCount()) {
        failure = "scenario plan evidence count did not match executed expectations";
    }

    server.start();
    for (std::unique_ptr<ScenarioClient> const& client : clients) {
        if (client->isConnected() && !client->disconnect(remainingTimeout(deadline))) {
            if (!failure.has_value()) {
                failure = "scenario client teardown was not graceful while the server was polling";
            }
        }
    }
    server.stop();
    if (failure.has_value()) {
        return std::unexpected(*failure);
    }

    return RuntimeEvidence{
        .mode = "scenario",
        .scenario_version = std::to_string(plan.version()),
        .profile = std::string{ shared::scenarioProfileName(plan.profile()) },
        .seed = plan.seed(),
        .ticks = logical_tick,
        .clients_requested = static_cast<uint64_t>(plan.actors().size()),
        .clients_accepted = clients_accepted,
        .server_events_processed = server.eventsProcessed(),
        .accepted_tick = accepted_tick,
        .last_effective_tick = last_effective_tick,
        .inputs_sent = inputs_sent,
        .camera_relative_inputs = camera_relative_inputs,
        .expectations_passed = expectations_passed,
        .authoritative_tick_ms = static_cast<uint64_t>(shared::TICK.count()),
        .replay_id = shared::scenarioReplayId(plan),
        .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at
        ),
        .deadline = std::chrono::duration_cast<std::chrono::milliseconds>(options.deadline),
        .passed = true,
    };
}

} // namespace acceptance
