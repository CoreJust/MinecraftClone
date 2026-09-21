#include <client/GameClient.hpp>
#include <server/GameServer.hpp>

#include <shared/net/Message.hpp>
#include <shared/world/World.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace {

struct FaultCounters final {
    uint32_t sent = 0U;
    uint32_t delivered = 0U;
    uint32_t lost = 0U;
    uint32_t duplicated = 0U;
    uint32_t reordered = 0U;
    uint32_t freeze_recoveries = 0U;
};

class DeterministicLink final {
public:
    void send(shared::Message const& message, uint32_t const tick)
    {
        uint32_t const ordinal = counters_.sent++;
        if (ordinal % LOSS_PERIOD == 0U) {
            ++counters_.lost;
            return;
        }
        uint32_t const jitter = (ordinal * 7U) % (MAX_JITTER_TICKS + 1U);
        uint32_t due_tick = tick + BASE_LATENCY_TICKS + jitter;
        if (tick >= FREEZE_BEGIN_TICK && tick < FREEZE_END_TICK) {
            due_tick += FREEZE_END_TICK - tick;
            ++counters_.freeze_recoveries;
        }
        queue_.push_back({
            .due_tick = due_tick,
            .ordinal = ordinal,
            .bytes = shared::encodeMessage(message),
        });
        if (ordinal % DUPLICATE_PERIOD == 0U) {
            ++counters_.duplicated;
            queue_.push_back({
                .due_tick = due_tick,
                .ordinal = ordinal + DUPLICATE_ORDINAL_OFFSET,
                .bytes = shared::encodeMessage(message),
            });
        }
    }

    template<typename Delivery>
    void deliver(uint32_t const tick, Delivery&& delivery)
    {
        std::ranges::sort(queue_, [](Packet const& first, Packet const& second) {
            return first.due_tick == second.due_tick
                ? first.ordinal > second.ordinal : first.due_tick < second.due_tick;
        });
        uint32_t delivered_this_tick = 0U;
        auto packet = queue_.begin();
        while (packet != queue_.end() && packet->due_tick <= tick) {
            std::optional<shared::Message> const decoded = shared::decodeMessage(packet->bytes);
            if (decoded.has_value()) {
                delivery(*decoded);
                ++counters_.delivered;
                ++delivered_this_tick;
            }
            packet = queue_.erase(packet);
        }
        if (delivered_this_tick > 1U) {
            ++counters_.reordered;
        }
    }

    [[nodiscard]] std::vector<shared::Message> receive(uint32_t const tick)
    {
        std::vector<shared::Message> received;
        deliver(tick, [&received](shared::Message const& message) {
            received.push_back(message);
        });
        return received;
    }

    [[nodiscard]] FaultCounters const& counters() const noexcept { return counters_; }
    [[nodiscard]] bool empty() const noexcept { return queue_.empty(); }
    [[nodiscard]] static constexpr uint32_t freezeBeginTick() noexcept { return FREEZE_BEGIN_TICK; }
    [[nodiscard]] static constexpr uint32_t freezeEndTick() noexcept { return FREEZE_END_TICK; }

private:
    static constexpr uint32_t BASE_LATENCY_TICKS = 3U;
    static constexpr uint32_t MAX_JITTER_TICKS = 4U;
    static constexpr uint32_t LOSS_PERIOD = 7U;
    static constexpr uint32_t DUPLICATE_PERIOD = 5U;
    static constexpr uint32_t DUPLICATE_ORDINAL_OFFSET = 10'000U;
    static constexpr uint32_t FREEZE_BEGIN_TICK = 9U;
    static constexpr uint32_t FREEZE_END_TICK = 15U;

    struct Packet final {
        uint32_t due_tick;
        uint32_t ordinal;
        std::vector<uint8_t> bytes;
    };

    FaultCounters counters_{};
    std::vector<Packet> queue_;
};

class AdverseGameClient final : public client::GameClient {
public:
    AdverseGameClient()
        : GameClient{ shared::WorldMode::Flight }
    {
        m_local_character = '@';
    }

    using GameClient::applyServerPosition;
    using GameClient::predictInput;
    using GameClient::send;
    using core::Client::connect;
    using core::Client::disconnect;
    using core::Client::isConnected;
    using core::Client::poll;

    [[nodiscard]] std::optional<shared::Player> authoritativePlayer() const noexcept
    {
        return m_world.playerByCharacter(m_local_character);
    }

    [[nodiscard]] uint32_t pendingInputCount() const noexcept
    {
        return static_cast<uint32_t>(m_pending_inputs.size());
    }

    [[nodiscard]] uint32_t localStateRevision() const noexcept
    {
        auto const found = m_state_revisions.find(m_local_character);
        return found == m_state_revisions.end() ? 0U : found->second;
    }

    bool join(core::Address const server_address, char const character)
    {
        m_local_character = character;
        m_running = true;
        m_accepted = false;
        if (!connect(server_address, std::chrono::seconds{ 1 })) {
            return false;
        }
        return send(shared::JoinRequestMessage{
            .ch = character,
            .mode = m_world.mode(),
            .configuration = m_world.configuration(),
            .wants_previews = false,
        });
    }

    [[nodiscard]] bool accepted() const noexcept { return m_accepted; }
    [[nodiscard]] uint32_t joinedCount() const noexcept
    {
        return m_joined_count.load(std::memory_order_acquire);
    }

    void requestStop() noexcept
    {
        m_stop_requested.store(true, std::memory_order_release);
    }

private:
    shared::Direction input() override { return {}; }
    void onConnectionStateReset() override
    {
        m_was_joined = false;
        GameClient::onConnectionStateReset();
    }

    void render() override
    {
        if (m_stop_requested.load(std::memory_order_acquire)) {
            m_running = false;
            return;
        }
        bool const joined = m_accepted && m_world.playerByCharacter(m_local_character).has_value();
        if (joined && !m_was_joined) {
            m_joined_count.fetch_add(1U, std::memory_order_release);
        }
        m_was_joined = joined;
    }

    std::atomic_bool m_stop_requested{ false };
    std::atomic_uint32_t m_joined_count{ 0U };
    bool m_was_joined = false;
};

TEST(AdverseNetworkTest, CoversLossLatencyJitterFreezeReorderingAndDuplicationDeterministically)
{
    static constexpr uint32_t INPUT_COUNT = 32U;
    static constexpr uint32_t SETTLE_TICK = 64U;
    DeterministicLink link;
    std::vector<uint32_t> delivered_sequences;

    for (uint32_t tick = 0U; tick < INPUT_COUNT; ++tick) {
        link.send(shared::ClientInputMessage{
            .direction = { .x = 127U, .y = 0U, .view_y = 127 },
            .sequence = tick + 1U,
        }, tick);
        for (shared::Message const& message : link.receive(tick)) {
            ASSERT_TRUE(std::holds_alternative<shared::ClientInputMessage>(message));
            delivered_sequences.push_back(std::get<shared::ClientInputMessage>(message).sequence);
        }
    }
    for (uint32_t tick = INPUT_COUNT; tick <= SETTLE_TICK; ++tick) {
        for (shared::Message const& message : link.receive(tick)) {
            ASSERT_TRUE(std::holds_alternative<shared::ClientInputMessage>(message));
            delivered_sequences.push_back(std::get<shared::ClientInputMessage>(message).sequence);
        }
    }

    FaultCounters const& counters = link.counters();
    EXPECT_EQ(counters.sent, INPUT_COUNT);
    EXPECT_GT(counters.lost, 0U);
    EXPECT_GT(counters.duplicated, 0U);
    EXPECT_GT(counters.reordered, 0U);
    EXPECT_GT(counters.freeze_recoveries, 0U);
    EXPECT_EQ(counters.delivered, delivered_sequences.size());
    EXPECT_TRUE(link.empty());
    EXPECT_FALSE(delivered_sequences.empty());
}

TEST(AdverseNetworkTest, ProductionClientTraversesFaultBurstAndReconnectsOverGameServerTransport)
{
    static constexpr uint32_t ACTIVE_TICKS = 48U;
    static constexpr uint32_t SETTLE_TICKS = 192U;
    static constexpr shared::Direction DIRECTION{ .x = 127U, .y = 0U, .view_y = 127 };
    server::GameServer server{ 0, {}, shared::WorldMode::Flight };
    std::atomic_bool stop_server{ false };
    std::thread server_thread{ [&server, &stop_server] {
        while (!stop_server.load(std::memory_order_relaxed)) {
            static_cast<void>(server.tick(std::chrono::milliseconds{ 1 }));
        }
    } };

    AdverseGameClient client;
    ASSERT_TRUE(client.join(core::Address::localhost(server.port()), '@'));

    auto const waitForJoin = [&client] {
        static constexpr uint32_t MAX_POLLS = 1'000U;
        for (uint32_t poll = 0U; poll < MAX_POLLS; ++poll) {
            client.poll(std::chrono::milliseconds{ 1 });
            if (client.accepted() && client.authoritativePlayer().has_value()) {
                return true;
            }
        }
        return false;
    };
    ASSERT_TRUE(waitForJoin());
    shared::Player const initial = *client.authoritativePlayer();

    DeterministicLink input_link;
    std::vector<shared::ClientInputMessage> sent_inputs;
    sent_inputs.reserve(ACTIVE_TICKS);

    for (uint32_t tick = 0U; tick < ACTIVE_TICKS; ++tick) {
        std::optional<shared::ClientInputMessage> const input = client.predictInput(DIRECTION);
        ASSERT_TRUE(input.has_value());
        sent_inputs.push_back(*input);
        input_link.send(*input, tick);
        input_link.deliver(tick, [&client](shared::Message const& message) {
            auto const* const input_message = std::get_if<shared::ClientInputMessage>(&message);
            if (input_message != nullptr) {
                EXPECT_TRUE(client.send(*input_message));
            }
        });
        if (tick < DeterministicLink::freezeBeginTick()
            || tick >= DeterministicLink::freezeEndTick()) {
            client.poll(std::chrono::milliseconds{ 1 });
        }
    }
    for (uint32_t tick = ACTIVE_TICKS; tick < SETTLE_TICKS; ++tick) {
        if (tick % 8U == 0U) {
            for (shared::ClientInputMessage const& input : sent_inputs) {
                input_link.send(input, tick);
            }
        }
        input_link.deliver(tick, [&client](shared::Message const& message) {
            auto const* const input_message = std::get_if<shared::ClientInputMessage>(&message);
            if (input_message != nullptr) {
                EXPECT_TRUE(client.send(*input_message));
            }
        });
        client.poll(std::chrono::milliseconds{ 1 });
    }
    for (uint32_t tick = SETTLE_TICKS; tick < SETTLE_TICKS + 64U; ++tick) {
        if (tick % 8U == 0U) {
            for (shared::ClientInputMessage const& input : sent_inputs) {
                input_link.send(input, tick);
            }
        }
        input_link.deliver(tick, [&client](shared::Message const& message) {
            auto const* const input_message = std::get_if<shared::ClientInputMessage>(&message);
            if (input_message != nullptr) {
                EXPECT_TRUE(client.send(*input_message));
            }
        });
        client.poll(std::chrono::milliseconds{ 1 });
    }

    ASSERT_TRUE(client.authoritativePlayer().has_value());
    EXPECT_NE(client.authoritativePlayer()->x, initial.x);
    EXPECT_EQ(client.pendingInputCount(), 0U);
    EXPECT_GT(client.localStateRevision(), 1U);
    EXPECT_GT(input_link.counters().lost, 0U);
    EXPECT_GT(input_link.counters().duplicated, 0U);
    EXPECT_GT(input_link.counters().reordered, 0U);
    EXPECT_GT(input_link.counters().freeze_recoveries, 0U);

    ASSERT_TRUE(client.disconnect(std::chrono::seconds{ 1 }));
    client.poll(std::chrono::milliseconds::zero());
    EXPECT_FALSE(client.isConnected());
    EXPECT_FALSE(client.authoritativePlayer().has_value());
    EXPECT_EQ(client.pendingInputCount(), 0U);

    ASSERT_TRUE(client.join(core::Address::localhost(server.port()), '@'));
    ASSERT_TRUE(waitForJoin());
    ASSERT_TRUE(client.authoritativePlayer().has_value());
    std::optional<shared::ClientInputMessage> const replacement_input = client.predictInput(DIRECTION);
    ASSERT_TRUE(replacement_input.has_value());
    ASSERT_TRUE(client.send(*replacement_input));
    for (uint32_t poll = 0U; poll < 1'000U && client.pendingInputCount() != 0U; ++poll) {
        client.poll(std::chrono::milliseconds{ 1 });
    }
    uint32_t const replacement_revision = client.localStateRevision();
    ASSERT_GT(replacement_revision, 1U);
    shared::ServerPlayerPositionMessage stale{
        .ch = '@',
        .x = client.authoritativePlayer()->x,
        .y = client.authoritativePlayer()->y,
        .z = client.authoritativePlayer()->z,
        .x_subcell = client.authoritativePlayer()->x_subcell,
        .y_subcell = client.authoritativePlayer()->y_subcell,
        .z_subcell = client.authoritativePlayer()->z_subcell,
        .state_revision = replacement_revision > 1U ? replacement_revision - 1U : 1U,
    };
    EXPECT_FALSE(client.applyServerPosition(stale));

    static_cast<void>(client.disconnect(std::chrono::seconds{ 1 }));
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();
}

TEST(AdverseNetworkTest, ProductionRunRetriesAfterRemoteServerReplacement)
{
    static constexpr std::chrono::seconds DEADLINE{ 5 };
    auto first_server = std::make_unique<server::GameServer>(0, std::vector<server::GameServer::SpawnPoint>{ },
        shared::WorldMode::Flight);
    uint16_t const port = first_server->port();
    std::atomic_bool stop_first{ false };
    std::thread first_server_thread{ [&first_server, &stop_first] {
        first_server->run(stop_first);
    } };

    AdverseGameClient client;
    std::thread client_thread{ [&client, port] {
        client.run(core::Address::localhost(port), '@');
    } };
    auto const waitForJoinedCount = [&client](uint32_t const expected) {
        auto const deadline = std::chrono::steady_clock::now() + DEADLINE;
        while (std::chrono::steady_clock::now() < deadline) {
            if (client.joinedCount() >= expected) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 10 });
        }
        return client.joinedCount() >= expected;
    };

    bool const initially_joined = waitForJoinedCount(1U);
    stop_first.store(true, std::memory_order_release);
    first_server_thread.join();
    first_server.reset();
    if (!initially_joined) {
        client.requestStop();
        client_thread.join();
        ADD_FAILURE() << "production client did not join the initial server";
        return;
    }

    auto replacement_server = std::make_unique<server::GameServer>(port,
        std::vector<server::GameServer::SpawnPoint>{ }, shared::WorldMode::Flight);
    std::atomic_bool stop_replacement{ false };
    std::thread replacement_server_thread{ [&replacement_server, &stop_replacement] {
        replacement_server->run(stop_replacement);
    } };
    bool const rejoined = waitForJoinedCount(2U);
    client.requestStop();
    client_thread.join();
    stop_replacement.store(true, std::memory_order_release);
    replacement_server_thread.join();

    EXPECT_TRUE(rejoined);
    EXPECT_GE(client.joinedCount(), 2U);
}

} // namespace
