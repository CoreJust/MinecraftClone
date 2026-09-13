#include <server/GameServer.hpp>
#include <shared/net/Message.hpp>
#include <core/net/Client.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

namespace {

class ProtocolClient final : public core::Client {
public:
    std::vector<shared::Message> messages;

    ProtocolClient() : core::Client{ 1 } {}

    bool sendMessage(shared::Message const& message)
    {
        return send(shared::encodeMessage(message), 0, core::SendMode{ core::SendMode::Reliable });
    }

    bool waitFor(std::function<bool()> const& ready)
    {
        static constexpr std::chrono::seconds TIMEOUT{ 1 };
        static constexpr std::chrono::milliseconds POLL_INTERVAL{ 1 };
        auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
        while (!ready() && std::chrono::steady_clock::now() < deadline) {
            poll(POLL_INTERVAL);
        }
        return ready();
    }

    uint32_t responseCount() const
    {
        return static_cast<uint32_t>(std::ranges::count_if(messages, [](auto const& message) {
            return std::holds_alternative<shared::JoinResponseMessage>(message);
        }));
    }

    std::vector<shared::ServerPlayerPositionMessage> positions(char const ch) const
    {
        std::vector<shared::ServerPlayerPositionMessage> result;
        for (auto const& message : messages) {
            auto const* position = std::get_if<shared::ServerPlayerPositionMessage>(&message);
            if (position && position->ch == ch) {
                result.push_back(*position);
            }
        }
        return result;
    }

private:
    void onDisconnected(core::DisconnectEvent const) override {}

    void onReceived(core::ReceiveEvent event) override
    {
        auto message = shared::decodeMessage(event.data);
        ASSERT_TRUE(message.has_value());
        messages.push_back(*message);
    }
};

class GameServerTest : public testing::Test {
protected:
    server::GameServer server{ 0 };
    std::atomic_bool stop_requested{ false };
    std::thread server_thread{ [this] {
        static constexpr std::chrono::milliseconds POLL_INTERVAL{ 1 };
        while (!stop_requested.load(std::memory_order_relaxed)) {
            server.poll(POLL_INTERVAL);
        }
    } };

    ~GameServerTest() override
    {
        stop_requested.store(true, std::memory_order_relaxed);
        server_thread.join();
    }

    bool connect(ProtocolClient& client)
    {
        static constexpr std::chrono::seconds TIMEOUT{ 1 };
        return client.connect(core::Address::localhost(server.port()), TIMEOUT);
    }

    bool join(ProtocolClient& client, char const ch)
    {
        return connect(client)
            && client.sendMessage(shared::JoinRequestMessage{ .ch = ch })
            && client.waitFor([&client, ch] { return !client.positions(ch).empty(); });
    }
};

class ProductionGameServerTest : public testing::Test {
protected:
    server::GameServer server{ 0 };
    std::atomic_bool stop_requested{ false };
    std::thread server_thread{ [this] {
        server.run(stop_requested);
    } };

    ~ProductionGameServerTest() override
    {
        stop_requested.store(true, std::memory_order_relaxed);
        server_thread.join();
    }

    bool connect(ProtocolClient& client)
    {
        static constexpr std::chrono::seconds TIMEOUT{ 1 };
        return client.connect(core::Address::localhost(server.port()), TIMEOUT);
    }

    bool join(ProtocolClient& client, char const ch)
    {
        return connect(client)
            && client.sendMessage(shared::JoinRequestMessage{ .ch = ch })
            && client.waitFor([&client, ch] { return !client.positions(ch).empty(); });
    }
};

bool pumpUntil(
    server::GameServer& server,
    ProtocolClient& client,
    std::function<bool()> const& ready
)
{
    static constexpr std::chrono::milliseconds POLL_INTERVAL{ 1 };
    static constexpr std::chrono::seconds TIMEOUT{ 1 };
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(server.tick(POLL_INTERVAL));
        client.poll(POLL_INTERVAL);
    }
    return ready();
}

bool joinManually(server::GameServer& server, ProtocolClient& client, char const character)
{
    static constexpr std::chrono::seconds TIMEOUT{ 1 };
    std::atomic_bool stop_requested{ false };
    std::thread server_thread{ [&server, &stop_requested] {
        static constexpr std::chrono::milliseconds POLL_INTERVAL{ 1 };
        while (!stop_requested.load(std::memory_order_relaxed)) {
            server.poll(POLL_INTERVAL);
        }
    } };
    bool const joined = client.connect(core::Address::localhost(server.port()), TIMEOUT)
        && client.sendMessage(shared::JoinRequestMessage{ .ch = character })
        && client.waitFor([&client, character] {
            return !client.positions(character).empty();
        });
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();
    return joined;
}

} // namespace

TEST_F(GameServerTest, JoinRepliesArePrivateAndNewPlayersReachExistingClients)
{
    ProtocolClient first;
    ProtocolClient rejected;
    ProtocolClient newcomer;
    ASSERT_TRUE(join(first, '@'));
    ASSERT_EQ(first.responseCount(), 1u);
    ASSERT_TRUE(std::get<shared::JoinResponseMessage>(first.messages.front()).accepted);

    ASSERT_TRUE(connect(rejected));
    ASSERT_TRUE(rejected.sendMessage(shared::JoinRequestMessage{ .ch = '@' }));
    ASSERT_TRUE(rejected.waitFor([&] { return rejected.responseCount() == 1; }));
    EXPECT_FALSE(std::get<shared::JoinResponseMessage>(rejected.messages.front()).accepted);

    ASSERT_TRUE(join(newcomer, '#'));
    ASSERT_TRUE(first.waitFor([&] { return !first.positions('#').empty(); }));
    EXPECT_EQ(first.responseCount(), 1u);
    EXPECT_EQ(newcomer.responseCount(), 1u);
    ASSERT_TRUE(newcomer.waitFor([&] { return !newcomer.positions('@').empty(); }));
    EXPECT_EQ(newcomer.positions('@').front().x, first.positions('@').front().x);
    EXPECT_EQ(newcomer.positions('@').front().y, first.positions('@').front().y);
}

TEST_F(GameServerTest, RepeatedJoinCannotCreateAnotherPlayerForAConnection)
{
    ProtocolClient first;
    ProtocolClient newcomer;
    ASSERT_TRUE(join(first, '@'));
    ASSERT_TRUE(first.sendMessage(shared::JoinRequestMessage{ .ch = '#' }));
    ASSERT_TRUE(first.waitFor([&] { return first.responseCount() == 2; }));
    auto const response = std::get_if<shared::JoinResponseMessage>(&first.messages.back());
    ASSERT_NE(response, nullptr);
    EXPECT_FALSE(response->accepted);

    ASSERT_TRUE(join(newcomer, '#'));
    EXPECT_EQ(newcomer.positions('@').size(), 1u);
    EXPECT_EQ(newcomer.positions('#').size(), 1u);
}

TEST_F(GameServerTest, MalformedPacketFromUnjoinedPeerDoesNotPreventJoinOrMovement)
{
    static constexpr std::array<uint8_t, 1> TRUNCATED_JOIN_REQUEST{ 0 };
    ProtocolClient client;
    ASSERT_TRUE(connect(client));
    ASSERT_TRUE(client.send(
        TRUNCATED_JOIN_REQUEST,
        0,
        core::SendMode{ core::SendMode::Reliable }
    ));
    ASSERT_TRUE(client.sendMessage(shared::JoinRequestMessage{ .ch = '@' }));
    ASSERT_TRUE(client.waitFor([&client] { return !client.positions('@').empty(); }));
    ASSERT_EQ(client.responseCount(), 1u);
    EXPECT_TRUE(std::get<shared::JoinResponseMessage>(client.messages.front()).accepted);
    ASSERT_EQ(client.positions('@').size(), 1u);

    auto const start = client.positions('@').front();
    uint8_t const direction_x = start.x == 0 ? 127 : 129;
    ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{ .direction = { direction_x, 0 }, .sequence = 1U }));
    ASSERT_TRUE(client.waitFor([&client] { return client.positions('@').size() == 2; }));
    auto const positions = client.positions('@');
    EXPECT_EQ(positions.back().x, start.x == 0 ? start.x : start.x - 1);
    EXPECT_EQ(positions.back().y, start.y);
    EXPECT_EQ(
        positions.back().x_subcell,
        start.x == 0
            ? shared::MOVEMENT_SUBCELLS_PER_TICK
            : shared::SUBCELLS_PER_CELL - shared::MOVEMENT_SUBCELLS_PER_TICK
    );
    EXPECT_EQ(positions.back().y_subcell, 0U);
}

TEST(GameServerPredictionTest, IdleInputConsumesTheSingleAuthoritativeActionInATick)
{
    server::GameServer server{ 0, {{ .character = '@', .x = 0U, .y = 0U }} };
    ProtocolClient client;
    ASSERT_TRUE(joinManually(server, client, '@'));
    auto const start = client.positions('@').front();
    ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{ .direction = { 0, 0 }, .sequence = 1U }));
    ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{ .direction = { 127U, 0 }, .sequence = 2U }));

    ASSERT_TRUE(pumpUntil(server, client, [&client] {
        auto const positions = client.positions('@');
        return std::ranges::any_of(positions, [](auto const& position) {
            return position.acknowledged_input_sequence == 1U;
        });
    }));
    auto const positions = client.positions('@');
    auto const after_idle_tick_position = std::ranges::find_if(positions, [](auto const& position) {
        return position.acknowledged_input_sequence == 1U;
    });
    ASSERT_NE(after_idle_tick_position, positions.end());
    auto const after_idle_tick = *after_idle_tick_position;
    EXPECT_EQ(after_idle_tick.x, start.x);
    EXPECT_EQ(after_idle_tick.y, start.y);
    EXPECT_EQ(after_idle_tick.x_subcell, 0U);
    EXPECT_EQ(after_idle_tick.y_subcell, 0U);

    ASSERT_TRUE(pumpUntil(server, client, [&client] {
        auto const positions = client.positions('@');
        return !positions.empty() && positions.back().acknowledged_input_sequence == 2U;
    }));
    auto const after_movement_tick = client.positions('@').back();
    EXPECT_EQ(after_movement_tick.x, start.x);
    EXPECT_EQ(after_movement_tick.y, start.y);
    EXPECT_EQ(after_movement_tick.x_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);
    EXPECT_EQ(after_movement_tick.y_subcell, 0U);
    EXPECT_LT(after_idle_tick.state_revision, after_movement_tick.state_revision);
}

TEST_F(GameServerTest, UnjoinedInputAndDisconnectLeaveAcceptedPlayersIntact)
{
    static constexpr std::chrono::seconds TIMEOUT{ 1 };
    ProtocolClient first;
    ProtocolClient unjoined;
    ProtocolClient newcomer;
    ASSERT_TRUE(join(first, '@'));
    ASSERT_TRUE(connect(unjoined));
    ASSERT_TRUE(unjoined.sendMessage(shared::ClientInputMessage{ .direction = { 127, 0 }, .sequence = 1U }));
    ASSERT_TRUE(unjoined.sendMessage(shared::JoinRequestMessage{ .ch = '@' }));
    ASSERT_TRUE(unjoined.waitFor([&] { return unjoined.responseCount() == 1; }));
    EXPECT_FALSE(std::get<shared::JoinResponseMessage>(unjoined.messages.front()).accepted);
    ASSERT_TRUE(unjoined.disconnect(TIMEOUT));
    ASSERT_TRUE(join(newcomer, '#'));
    ASSERT_TRUE(first.waitFor([&] { return !first.positions('#').empty(); }));
    EXPECT_EQ(newcomer.positions('@').size(), 1u);
    EXPECT_EQ(first.positions('@').size(), 1u);
    EXPECT_EQ(std::ranges::count_if(first.messages, [](auto const& message) {
        return std::holds_alternative<shared::ServerRemovePlayerMessage>(message);
    }), 0);
}

TEST_F(GameServerTest, JoinedDisconnectRemovesOnlyTheDepartingPlayer)
{
    static constexpr std::chrono::seconds TIMEOUT{ 1 };
    ProtocolClient first;
    ProtocolClient departing;
    ASSERT_TRUE(join(first, '@'));
    ASSERT_TRUE(join(departing, '#'));
    ASSERT_TRUE(departing.disconnect(TIMEOUT));
    ASSERT_TRUE(first.waitFor([&] {
        return std::ranges::any_of(first.messages, [](auto const& message) {
            return std::holds_alternative<shared::ServerRemovePlayerMessage>(message);
        });
    }));
    auto const* removal = std::get_if<shared::ServerRemovePlayerMessage>(&first.messages.back());
    ASSERT_NE(removal, nullptr);
    EXPECT_EQ(removal->ch, '#');

    ProtocolClient replacement;
    ASSERT_TRUE(join(replacement, '#'));
    EXPECT_EQ(replacement.positions('@').size(), 1u);
    EXPECT_EQ(replacement.positions('#').size(), 1u);
}

TEST_F(ProductionGameServerTest, ConcurrentNormalCadenceInputsDoNotStarveNewJoin)
{
    static constexpr uint32_t INPUTS_PER_SENDER_BEFORE_JOIN{ 12 };
    static constexpr std::chrono::seconds JOIN_TIMEOUT{ 1 };
    static constexpr std::chrono::seconds NORMAL_CADENCE_TIMEOUT{ 5 };
    static constexpr shared::Direction DIRECTION{ .x = 127, .y = 0 };
    ProtocolClient first;
    ProtocolClient second;
    ProtocolClient newcomer;
    ASSERT_TRUE(join(first, '@'));
    ASSERT_TRUE(join(second, '#'));

    std::atomic_bool stop_sending{ false };
    std::atomic_bool send_failed{ false };
    std::atomic<uint32_t> first_inputs_sent{ 0 };
    std::atomic<uint32_t> second_inputs_sent{ 0 };
    std::barrier start_senders{ 3 };
    auto const send_inputs = [&](ProtocolClient& client, std::atomic<uint32_t>& inputs_sent) {
        uint32_t sequence = 1U;
        start_senders.arrive_and_wait();
        while (!stop_sending.load(std::memory_order_relaxed)) {
            if (!client.sendMessage(shared::ClientInputMessage{ .direction = DIRECTION, .sequence = sequence++ })) {
                send_failed.store(true, std::memory_order_relaxed);
                return;
            }
            client.flush();
            inputs_sent.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(shared::TICK);
        }
    };
    std::thread first_sender{ send_inputs, std::ref(first), std::ref(first_inputs_sent) };
    std::thread second_sender{ send_inputs, std::ref(second), std::ref(second_inputs_sent) };
    start_senders.arrive_and_wait();

    auto const normal_cadence_deadline = std::chrono::steady_clock::now() + NORMAL_CADENCE_TIMEOUT;
    while (
        (
            first_inputs_sent.load(std::memory_order_relaxed) < INPUTS_PER_SENDER_BEFORE_JOIN
            || second_inputs_sent.load(std::memory_order_relaxed) < INPUTS_PER_SENDER_BEFORE_JOIN
        )
        && std::chrono::steady_clock::now() < normal_cadence_deadline
    ) {
        std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
    }
    bool const normal_cadence_inputs_prepared =
        first_inputs_sent.load(std::memory_order_relaxed) >= INPUTS_PER_SENDER_BEFORE_JOIN
        && second_inputs_sent.load(std::memory_order_relaxed) >= INPUTS_PER_SENDER_BEFORE_JOIN;

    auto const join_started = std::chrono::steady_clock::now();
    bool const joined = join(newcomer, '$');
    bool const received_authoritative_state = joined && newcomer.waitFor([&] {
        return !newcomer.positions('@').empty() && !newcomer.positions('#').empty();
    });
    auto const join_elapsed = std::chrono::steady_clock::now() - join_started;

    stop_sending.store(true, std::memory_order_relaxed);
    first_sender.join();
    second_sender.join();

    EXPECT_FALSE(send_failed.load(std::memory_order_relaxed));
    EXPECT_TRUE(normal_cadence_inputs_prepared);
    EXPECT_TRUE(joined);
    EXPECT_TRUE(received_authoritative_state);
    EXPECT_LT(join_elapsed, JOIN_TIMEOUT);
}

TEST(GameServerPredictionTest, QueuedInputsApplyAtMostOncePerTickAndAcknowledgeInOrder)
{
    static constexpr shared::Direction RIGHT{ .x = 127U, .y = 0U };
    server::GameServer server{ 0, {{ .character = '@', .x = 0U, .y = 0U }} };
    ProtocolClient client;
    ASSERT_TRUE(joinManually(server, client, '@'));
    ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{ .direction = RIGHT, .sequence = 1U }));
    ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{ .direction = RIGHT, .sequence = 2U }));

    ASSERT_TRUE(pumpUntil(server, client, [&client] {
        auto const positions = client.positions('@');
        return !positions.empty() && positions.back().acknowledged_input_sequence == 1U;
    }));
    auto const after_first_tick = client.positions('@').back();
    EXPECT_EQ(after_first_tick.x_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);

    ASSERT_TRUE(pumpUntil(server, client, [&client] {
        auto const positions = client.positions('@');
        return !positions.empty() && positions.back().acknowledged_input_sequence == 2U;
    }));
    auto const after_second_tick = client.positions('@').back();
    uint32_t const expected_subcells = 2U * shared::MOVEMENT_SUBCELLS_PER_TICK;
    EXPECT_EQ(after_second_tick.x, expected_subcells / shared::SUBCELLS_PER_CELL);
    EXPECT_EQ(after_second_tick.x_subcell, expected_subcells % shared::SUBCELLS_PER_CELL);
    EXPECT_LT(after_first_tick.state_revision, after_second_tick.state_revision);
}

TEST(GameServerPredictionTest, ZeroAndCollisionInputsReceiveAuthoritativeOwnerAcknowledgements)
{
    static constexpr shared::Direction RIGHT{ .x = 127U, .y = 0U };
    server::GameServer server{ 0, {{ .character = '@', .x = 30U, .y = 0U }} };
    ProtocolClient client;
    ASSERT_TRUE(joinManually(server, client, '@'));
    ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{ .direction = { }, .sequence = 1U }));
    ASSERT_TRUE(pumpUntil(server, client, [&client] {
        auto const positions = client.positions('@');
        return !positions.empty() && positions.back().acknowledged_input_sequence == 1U;
    }));
    auto const zero_acknowledgement = client.positions('@').back();
    EXPECT_EQ(zero_acknowledgement.x, 30U);
    EXPECT_EQ(zero_acknowledgement.x_subcell, 0U);

    ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{ .direction = RIGHT, .sequence = 2U }));
    ASSERT_TRUE(pumpUntil(server, client, [&client] {
        auto const positions = client.positions('@');
        return !positions.empty() && positions.back().acknowledged_input_sequence == 2U;
    }));
    auto const collision_acknowledgement = client.positions('@').back();
    EXPECT_EQ(collision_acknowledgement.x, 30U);
    EXPECT_EQ(collision_acknowledgement.x_subcell, 0U);
    EXPECT_LT(zero_acknowledgement.state_revision, collision_acknowledgement.state_revision);
}
