#include <acceptance/ScenarioRunner.hpp>

#include <server/GameServer.hpp>

#include <shared/net/Message.hpp>

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
            poll(std::min(poll_interval, remainingTimeout(deadline)));
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
        std::chrono::milliseconds const poll_interval,
        std::chrono::steady_clock::time_point const deadline
    )
        : m_server(0, std::move(spawn_points))
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
                static_cast<void>(tick(std::min(m_poll_interval, remainingTimeout(m_deadline))));
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
    if (plan.profile() != shared::ScenarioProfile::Flat2dV1) {
        return std::unexpected("scenario runner does not support this profile");
    }
    static constexpr uint64_t MAX_SERVER_CLIENTS{ 4 };
    if (static_cast<uint64_t>(plan.actors().size()) > MAX_SERVER_CLIENTS) {
        return std::unexpected("scenario runner supports at most 4 actors per server");
    }

    std::vector<server::GameServer::SpawnPoint> spawn_points;
    spawn_points.reserve(plan.actors().size());
    for (shared::ScenarioActor const& actor : plan.actors()) {
        spawn_points.push_back(server::GameServer::SpawnPoint{
            .character = actor.character,
            .x = actor.x,
            .y = actor.y,
        });
    }
    auto validated_spawn_points = server::GameServer::validateSpawnPoints(std::move(spawn_points));
    if (!validated_spawn_points.has_value()) {
        return std::unexpected(validated_spawn_points.error());
    }

    core::Net::ensureInit();
    std::chrono::steady_clock::time_point const started_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point const deadline = started_at + options.deadline;
    ScenarioServerController server{
        std::move(*validated_spawn_points),
        options.network_poll_interval,
        deadline,
    };
    std::vector<std::unique_ptr<ScenarioClient>> clients;
    clients.reserve(plan.actors().size());
    std::vector<std::optional<shared::Direction>> active_inputs(plan.actors().size());
    std::optional<std::string> failure;
    uint64_t logical_tick{ 0 };
    uint64_t inputs_sent{ 0 };
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
        if (!client->sendMessage(shared::JoinRequestMessage{ .ch = actor.character })) {
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
        for (uint64_t index{ 0 }; index < static_cast<uint64_t>(clients.size()); ++index) {
            if (!active_inputs[index].has_value()) {
                continue;
            }
            if (!clients[index]->sendMessage(shared::ClientInputMessage{
                .direction = *active_inputs[index],
            })) {
                return std::unexpected("scenario client could not send active input");
            }
            clients[index]->flush();
            ++active_count;
            ++inputs_sent;
        }
        uint64_t observed_events{ 0 };
        do {
            if (std::chrono::steady_clock::now() >= deadline) {
                return std::unexpected("scenario runner exceeded its monotonic deadline at a tick barrier");
            }
            observed_events += server.tick(std::min(
                options.network_poll_interval,
                remainingTimeout(deadline)
            ));
        } while (observed_events < active_count);
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
                active_inputs[*index] = shared::Direction{
                    .x = static_cast<uint8_t>(input->x),
                    .y = static_cast<uint8_t>(input->y),
                };
                last_effective_tick = input->effective_boundary;
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
                if (!clients[*index]->waitFor(
                    [&client = clients[*index], character, expectation] {
                        auto const position = client->latestPosition(character);
                        return position.has_value()
                            && position->x == expectation->x
                            && position->y == expectation->y;
                    },
                    deadline,
                    options.network_poll_interval
                )) {
                    failure = operationFailure(operation, "did not observe the expected authoritative position");
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
            failure = "scenario client teardown was not graceful while the server was polling";
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
        .expectations_passed = expectations_passed,
        .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at
        ),
        .deadline = std::chrono::duration_cast<std::chrono::milliseconds>(options.deadline),
        .passed = true,
    };
}

} // namespace acceptance
