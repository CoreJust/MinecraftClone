#include <client/GameClient.hpp>

#include <server/GameServer.hpp>

#include <core/net/Server.hpp>

#include <enet/enet.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

class UnjoinedServer final : public core::Server {
public:
    UnjoinedServer()
        : Server{ core::Address::localhost(0U), 1U, 2U }
    { }
private:
    void onConnected(core::ServerConnectEvent) override {}
    void onDisconnected(core::ServerDisconnectEvent) override {}
    void onReceived(core::ServerReceiveEvent) override {}
};

class LifecycleClient final : public client::GameClient {
public:
    explicit LifecycleClient(std::function<void(LifecycleClient&)> service)
        : m_service{ std::move(service) }
    { }

    void requestStop() noexcept { m_running = false; }
    [[nodiscard]] bool joined() const noexcept { return m_joined; }
    [[nodiscard]] bool accepted() const noexcept { return m_accepted; }
    [[nodiscard]] uint32_t serviceCalls() const noexcept { return m_service_calls; }
    [[nodiscard]] uint32_t inputCalls() const noexcept { return m_input_calls; }
    [[nodiscard]] uint32_t renderCalls() const noexcept { return m_render_calls; }
private:
    void servicePlatformEvents() override
    {
        ++m_service_calls;
        m_service(*this);
    }

    shared::Direction input() override
    {
        ++m_input_calls;
        return {};
    }

    void render() override
    {
        ++m_render_calls;
        if (m_world.playerByCharacter(m_local_character).has_value()) {
            m_joined = true;
            requestStop();
        }
    }
private:
    std::function<void(LifecycleClient&)> m_service;
    uint32_t m_service_calls = 0U;
    uint32_t m_input_calls = 0U;
    uint32_t m_render_calls = 0U;
    bool m_joined = false;
};

// Delay actual server datagrams, not server startup or application messages.
class HandshakeReplyRelay final {
public:
    HandshakeReplyRelay(uint16_t const server_port, std::chrono::seconds const reply_delay)
        : m_delay{ reply_delay }
    {
        enet_address_set_host_ip(&m_server, "127.0.0.1");
        m_server.port = server_port;
        m_address = m_server;
        m_address.port = 0U;
        m_socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        if (m_socket == ENET_SOCKET_NULL) {
            throw std::runtime_error{ "Cannot create handshake relay socket" };
        }
        if (enet_socket_bind(m_socket, &m_address) != 0
            || enet_socket_get_address(m_socket, &m_address) != 0
            || enet_socket_set_option(m_socket, ENET_SOCKOPT_NONBLOCK, 1) != 0) {
            enet_socket_destroy(m_socket);
            throw std::runtime_error{ "Cannot bind handshake relay socket" };
        }
    }

    ~HandshakeReplyRelay() { enet_socket_destroy(m_socket); }
    HandshakeReplyRelay(HandshakeReplyRelay const&) = delete;
    HandshakeReplyRelay& operator=(HandshakeReplyRelay const&) = delete;
    [[nodiscard]] uint16_t port() const noexcept { return m_address.port; }
    [[nodiscard]] uint32_t attemptCount() const noexcept
    {
        return static_cast<uint32_t>(m_connect_ids.size());
    }
    [[nodiscard]] bool releasedDelayedReply() const noexcept { return m_released; }

    void pump()
    {
        std::array<uint8_t, 65'536> bytes{};
        ENetAddress source{};
        ENetBuffer buffer{};
        buffer.data = bytes.data();
        buffer.dataLength = bytes.size();
        for (uint32_t packet = 0U; packet < 256U; ++packet) {
            int const size = enet_socket_receive(m_socket, &source, &buffer, 1U);
            if (size <= 0) {
                break;
            }
            if (source.host == m_server.host && source.port == m_server.port) {
                if (!m_first_reply.has_value()) {
                    m_first_reply = std::chrono::steady_clock::now();
                }
                m_replies.emplace_back(bytes.begin(), bytes.begin() + size);
            } else {
                m_client = source;
                recordAttempt(bytes.data(), static_cast<uint32_t>(size));
                buffer.dataLength = static_cast<size_t>(size);
                EXPECT_EQ(enet_socket_send(m_socket, &m_server, &buffer, 1U), size);
                buffer.dataLength = bytes.size();
            }
        }
        if (m_client.has_value() && m_first_reply.has_value()
            && std::chrono::steady_clock::now() - *m_first_reply >= m_delay) {
            for (auto& reply : m_replies) {
                ENetBuffer outgoing{};
                outgoing.data = reply.data();
                outgoing.dataLength = reply.size();
                EXPECT_EQ(enet_socket_send(m_socket, &*m_client, &outgoing, 1U),
                    static_cast<int>(reply.size()));
            }
            m_released = m_released || !m_replies.empty();
            m_replies.clear();
        }
    }
private:
    void recordAttempt(uint8_t const* const bytes, uint32_t const size)
    {
        if (size < sizeof(ENetProtocolHeader)) {
            return;
        }
        ENetProtocolHeader header{};
        std::memcpy(&header, bytes, sizeof(header));
        uint32_t const offset = (ENET_NET_TO_HOST_16(header.peerID) & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME)
            ? static_cast<uint32_t>(sizeof(ENetProtocolHeader))
            : static_cast<uint32_t>(sizeof(header.peerID));
        if (size < offset + sizeof(ENetProtocolConnect)
            || (bytes[offset] & ENET_PROTOCOL_COMMAND_MASK) != ENET_PROTOCOL_COMMAND_CONNECT) {
            return;
        }
        ENetProtocolConnect command{};
        std::memcpy(&command, bytes + offset, sizeof(command));
        m_connect_ids.insert(command.connectID);
    }
    ENetSocket m_socket = ENET_SOCKET_NULL;
    ENetAddress m_address{};
    ENetAddress m_server{};
    std::optional<ENetAddress> m_client;
    std::chrono::seconds m_delay;
    std::optional<std::chrono::steady_clock::time_point> m_first_reply;
    std::vector<std::vector<uint8_t>> m_replies;
    std::set<uint32_t> m_connect_ids;
    bool m_released = false;
};

uint16_t unusedPort()
{
    UnjoinedServer const reservation;
    return reservation.port();
}

} // namespace

TEST(GameClientLifecycleTest, StopBeforeConnectionDoesNotSubmitGameplay)
{
    static constexpr std::chrono::seconds DEADLINE{ 3 };
    uint16_t const port = unusedPort();
    bool stop_processed = false;
    LifecycleClient game_client{ [&](LifecycleClient& client) {
        stop_processed = true;
        client.requestStop();
    } };
    client::GameClientBenchmarkHooks const hooks{
        .deadline = std::chrono::steady_clock::now() + DEADLINE,
    };
    game_client.run(core::Address::localhost(port), '@', &hooks);

    EXPECT_TRUE(stop_processed);
    EXPECT_FALSE(game_client.isConnected());
    EXPECT_FALSE(game_client.joined());
    EXPECT_EQ(game_client.inputCalls(), 0U);
    EXPECT_EQ(game_client.renderCalls(), 0U);
}

TEST(GameClientLifecycleTest, StopDuringPendingConnectionDoesNotSubmitGameplay)
{
    static constexpr std::chrono::seconds DEADLINE{ 3 };
    static constexpr std::chrono::milliseconds STOP_DELAY{ 75 };
    static constexpr std::chrono::milliseconds STOP_BOUND{ 250 };
    uint16_t const port = unusedPort();
    bool stop_processed = false;
    auto const started = std::chrono::steady_clock::now();
    LifecycleClient game_client{ [&](LifecycleClient& client) {
        if (std::chrono::steady_clock::now() - started >= STOP_DELAY) {
            stop_processed = true;
            client.requestStop();
        }
    } };
    client::GameClientBenchmarkHooks const hooks{
        .deadline = std::chrono::steady_clock::now() + DEADLINE,
    };
    game_client.run(core::Address::localhost(port), '@', &hooks);

    EXPECT_TRUE(stop_processed);
    EXPECT_FALSE(game_client.isConnected());
    EXPECT_FALSE(game_client.accepted());
    EXPECT_EQ(game_client.inputCalls(), 0U);
    EXPECT_EQ(game_client.renderCalls(), 0U);
    EXPECT_LT(std::chrono::steady_clock::now() - started, STOP_BOUND);
}

TEST(GameClientLifecycleTest, StopDuringRetryIsServicedWithoutStartingAnotherAttempt)
{
    static constexpr std::chrono::milliseconds STOP_DELAY{ 30'100 };
    static constexpr std::chrono::seconds DEADLINE{ 32 };
    static constexpr std::chrono::milliseconds MAX_SERVICE_GAP{ 200 };
    uint16_t const port = unusedPort();
    auto const started = std::chrono::steady_clock::now();
    auto previous_service = started;
    std::chrono::steady_clock::duration maximum_gap{};
    bool stop_processed = false;
    LifecycleClient game_client{ [&](LifecycleClient& client) {
        auto const now = std::chrono::steady_clock::now();
        maximum_gap = (std::max)(maximum_gap, now - previous_service);
        previous_service = now;
        if (now - started >= STOP_DELAY) {
            stop_processed = true;
            client.requestStop();
        }
    } };
    client::GameClientBenchmarkHooks const hooks{ .deadline = started + DEADLINE };
    game_client.run(core::Address::localhost(port), '@', &hooks);

    EXPECT_TRUE(stop_processed);
    EXPECT_LT(maximum_gap, MAX_SERVICE_GAP);
    EXPECT_LT(std::chrono::steady_clock::now() - started, STOP_DELAY + MAX_SERVICE_GAP);
    EXPECT_FALSE(game_client.isConnected());
    EXPECT_FALSE(game_client.accepted());
    EXPECT_EQ(game_client.inputCalls(), 0U);
    EXPECT_EQ(game_client.renderCalls(), 0U);
}

TEST(GameClientLifecycleTest, DelayedHandshakeReplyPreservesOneAttemptAndJoins)
{
    static constexpr std::chrono::seconds REPLY_DELAY{ 15 };
    static constexpr std::chrono::seconds DEADLINE{ 28 };
    server::GameServer server{ 0U };
    HandshakeReplyRelay relay{ server.port(), REPLY_DELAY };
    auto const started = std::chrono::steady_clock::now();
    bool gameplay_before_reply = false;
    LifecycleClient game_client{ [&](LifecycleClient& client) {
        relay.pump();
        static_cast<void>(server.tick());
        relay.pump();
        if (!relay.releasedDelayedReply()) {
            gameplay_before_reply = gameplay_before_reply || client.accepted()
                || client.inputCalls() != 0U || client.renderCalls() != 0U;
        }
    } };
    client::GameClientBenchmarkHooks const hooks{ .deadline = started + DEADLINE };
    game_client.run(core::Address::localhost(relay.port()), '@', &hooks);

    EXPECT_GE(std::chrono::steady_clock::now() - started, REPLY_DELAY);
    EXPECT_FALSE(gameplay_before_reply);
    EXPECT_TRUE(relay.releasedDelayedReply());
    EXPECT_EQ(relay.attemptCount(), 1U);
    EXPECT_TRUE(game_client.joined());
    EXPECT_TRUE(game_client.accepted());
    EXPECT_GT(game_client.renderCalls(), 0U);
}

TEST(GameClientLifecycleTest, DelayedServerServicingAfterConnectionStartsStillJoins)
{
    static constexpr std::chrono::seconds DEADLINE{ 5 };
    static constexpr uint32_t START_AFTER_SERVICE = 2U;
    uint16_t const port = unusedPort();
    server::GameServer server{ port };
    std::atomic_bool stop_server{ false };
    std::thread server_thread;
    bool server_started = false;
    LifecycleClient game_client{ [&](LifecycleClient& client) {
        if (!server_started && client.serviceCalls() == START_AFTER_SERVICE) {
            server_started = true;
            server_thread = std::thread{ [&] { server.run(stop_server); } };
        }
    } };
    client::GameClientBenchmarkHooks const hooks{
        .deadline = std::chrono::steady_clock::now() + DEADLINE,
    };
    game_client.run(core::Address::localhost(port), '@', &hooks);
    stop_server.store(true, std::memory_order_relaxed);
    if (server_thread.joinable()) {
        server_thread.join();
    }

    EXPECT_TRUE(server_started);
    EXPECT_TRUE(game_client.joined());
    EXPECT_TRUE(game_client.accepted());
    EXPECT_GT(game_client.renderCalls(), 0U);
}

TEST(GameClientLifecycleTest, StopWhileAwaitingJoinDoesNotSubmitGameplay)
{
    static constexpr std::chrono::seconds DEADLINE{ 3 };
    static constexpr std::chrono::milliseconds SERVER_POLL{ 10 };
    UnjoinedServer server;
    std::atomic_bool stop_server{ false };
    std::thread server_thread{ [&] {
        while (!stop_server.load(std::memory_order_relaxed)) {
            server.poll(SERVER_POLL);
        }
    } };
    bool stop_processed = false;
    LifecycleClient game_client{ [&](LifecycleClient& client) {
        if (client.isConnected() && !client.accepted()) {
            stop_processed = true;
            client.requestStop();
        }
    } };
    client::GameClientBenchmarkHooks const hooks{
        .deadline = std::chrono::steady_clock::now() + DEADLINE,
    };
    game_client.run(core::Address::localhost(server.port()), '@', &hooks);
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();

    EXPECT_TRUE(stop_processed);
    EXPECT_TRUE(game_client.isConnected());
    EXPECT_FALSE(game_client.accepted());
    EXPECT_EQ(game_client.inputCalls(), 0U);
    EXPECT_EQ(game_client.renderCalls(), 0U);
}
