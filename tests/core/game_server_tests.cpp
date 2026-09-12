#include <server/GameServer.hpp>
#include <shared/net/Message.hpp>
#include <core/net/Client.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
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
    ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{ .direction = { direction_x, 0 } }));
    ASSERT_TRUE(client.waitFor([&client] { return client.positions('@').size() == 2; }));
    auto const positions = client.positions('@');
    EXPECT_EQ(positions.back().x, start.x == 0 ? start.x : start.x - 1);
    EXPECT_EQ(positions.back().y, start.y);
    EXPECT_EQ(positions.back().x_subcell, start.x == 0 ? 1'000U : 9'000U);
    EXPECT_EQ(positions.back().y_subcell, 0U);
}

TEST_F(GameServerTest, IdleInputDoesNotConsumeTheSingleMovementInAPollingTick)
{
    ProtocolClient client;
    ASSERT_TRUE(join(client, '@'));
    auto const start = client.positions('@').front();
    auto const direction_x = static_cast<uint8_t>(start.x == 0 ? 127 : 129);
    ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{ .direction = { 0, 0 } }));
    ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{ .direction = { direction_x, 0 } }));
    ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{ .direction = { direction_x, 0 } }));
    ASSERT_TRUE(client.sendMessage(shared::JoinRequestMessage{ .ch = '@' }));
    ASSERT_TRUE(client.waitFor([&] { return client.responseCount() == 2; }));

    auto const positions = client.positions('@');
    ASSERT_EQ(positions.size(), 2u);
    EXPECT_EQ(positions.back().x, start.x == 0 ? start.x : start.x - 1);
    EXPECT_EQ(positions.back().y, start.y);
    EXPECT_EQ(positions.back().x_subcell, start.x == 0 ? 1'000U : 9'000U);
    EXPECT_EQ(positions.back().y_subcell, 0U);
}

TEST_F(GameServerTest, UnjoinedInputAndDisconnectLeaveAcceptedPlayersIntact)
{
    static constexpr std::chrono::seconds TIMEOUT{ 1 };
    ProtocolClient first;
    ProtocolClient unjoined;
    ProtocolClient newcomer;
    ASSERT_TRUE(join(first, '@'));
    ASSERT_TRUE(connect(unjoined));
    ASSERT_TRUE(unjoined.sendMessage(shared::ClientInputMessage{ .direction = { 127, 0 } }));
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
