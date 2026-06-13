#include <core/net/Client.hpp>
#include <core/net/Server.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <span>
#include <string>
#include <thread>

constexpr std::chrono::milliseconds DEFAULT_TIMEOUT{ 10 };
constexpr uint16_t TEST_PORT_BASE{ 30'000 };

struct NoAction final {
    void operator()(auto&&...) { }
};

class TestClient final : public core::Client {
public:
    explicit TestClient(
        std::function<void(TestClient&, core::DisconnectEvent const)> on_disconnected = NoAction{ },
        std::function<void(TestClient&, core::ReceiveEvent)> on_received = NoAction{ },
        uint32_t const max_channels = 1)
        : core::Client{ max_channels }
        , m_on_disconnected{ std::move(on_disconnected) }
        , m_on_received{ std::move(on_received) }
    { }

    bool connectAndWait(uint16_t const port, std::chrono::milliseconds timeout = DEFAULT_TIMEOUT) {
        bool const result = connect(core::Address::localhost(port), timeout);
        pollAndWait();
        return result;
    }

    bool disconnectAndWait(std::optional<std::chrono::milliseconds> timeout = DEFAULT_TIMEOUT) {
        bool const result = disconnect(timeout);
        pollAndWait();
        return result;
    }

    bool sendAndPoll(std::string_view const msg, uint8_t const channel_id, core::SendMode const mode) {
        bool const result = send(core::asByteSpan(msg), channel_id, mode);
        pollAndWait();
        return result;
    }

    // Lets the server to process the events
    void pollAndWait() {
        std::this_thread::sleep_for(std::chrono::milliseconds{ 8 });
        poll();
    }
private:
    void onDisconnected(core::DisconnectEvent const event) override {
        m_on_disconnected(*this, event);
    }
    
    void onReceived(core::ReceiveEvent event) override {
        m_on_received(*this, std::move(event));
    }
private:
    std::function<void(TestClient&, core::DisconnectEvent const)> m_on_disconnected;
    std::function<void(TestClient&, core::ReceiveEvent)> m_on_received;
};

class TestServer final : public core::Server {
public:
    explicit TestServer(
        uint16_t const port,
        size_t const max_connections,
        uint32_t const max_channels,
        std::function<void(TestServer&, core::ServerConnectEvent const)> on_connected,
        std::function<void(TestServer&, core::ServerDisconnectEvent const)> on_disconnected,
        std::function<void(TestServer&, core::ServerReceiveEvent)> on_received)
        : core::Server{ core::Address::anyhost(port), max_connections, max_channels }
        , m_on_connected{ std::move(on_connected) }
        , m_on_disconnected{ std::move(on_disconnected) }
        , m_on_received{ std::move(on_received) }
    { }

    void run() {
        while (m_running) {
            poll(DEFAULT_TIMEOUT);
        }
    }

    void stop() noexcept { m_running = false; }
private:
    void onConnected(core::ServerConnectEvent const event) override {
        m_on_connected(*this, event);
    }

    void onDisconnected(core::ServerDisconnectEvent const event) override {
        m_on_disconnected(*this, event);
    }

    void onReceived(core::ServerReceiveEvent event) override {
        m_on_received(*this, std::move(event));
    }
private:
    std::function<void(TestServer&, core::ServerConnectEvent const)> m_on_connected;
    std::function<void(TestServer&, core::ServerDisconnectEvent const)> m_on_disconnected;
    std::function<void(TestServer&, core::ServerReceiveEvent)> m_on_received;
    bool m_running = true;
};

struct TestServerService final {
    TestServer server;
    std::jthread thread;
    std::atomic_size_t clients_connected{ 0 };
    std::atomic_size_t clients_disconnected{ 0 };

    explicit TestServerService(
        std::function<void(TestServer&, core::ServerReceiveEvent)> on_received,
        uint16_t const port,
        size_t const max_connections = 1,
        uint32_t const max_channels = 1,
        std::function<void(TestServer&, core::ServerConnectEvent const)> on_connected = nullptr,
        std::function<void(TestServer&, core::ServerDisconnectEvent const)> on_disconnected = nullptr)
        : server(
            port,
            max_connections,
            max_channels,
            on_connected
                ? std::move(on_connected)
                : [this](TestServer&, core::ServerConnectEvent const) { ++clients_connected; },
            on_disconnected
                ? std::move(on_disconnected)
                : [this](TestServer&, core::ServerDisconnectEvent const) { ++clients_disconnected; },
            std::move(on_received))
        , thread([this]{ this->server.run(); })
    { }

    ~TestServerService() { server.stop(); }

    void done(size_t const expected_connections, size_t const expected_disconnections) {
        server.stop();
        thread.join();
        EXPECT_EQ(clients_connected, expected_connections);
        EXPECT_EQ(clients_disconnected, expected_disconnections);
    }
};


///  ACTUAL TESTS  ///

TEST(NetClientServer, GracefulConnectDisconnectTest) {
    bool client_disconnected{ false };

    TestServerService srv{ NoAction{ }, TEST_PORT_BASE + 0 };
    TestClient client{ [&](auto&&...) { client_disconnected = true; } };

    ASSERT_TRUE(client.connectAndWait(TEST_PORT_BASE + 0));
    EXPECT_TRUE(srv.clients_connected == 1 && srv.clients_disconnected == 0);
    EXPECT_TRUE(client.isConnected());

    client.disconnectAndWait();

    srv.done(1, 1);
    EXPECT_TRUE(client_disconnected);
    EXPECT_FALSE(client.isConnected());
}

TEST(NetClientServer, MultipleReconnectionsTest) {
    constexpr uint32_t rounds{ 3 };

    TestServerService srv{ NoAction{ }, TEST_PORT_BASE + 1 };
    TestClient client{ };

    for (uint32_t i{ 0 }; i < rounds; ++i) {
        ASSERT_TRUE(client.connectAndWait(TEST_PORT_BASE + 1));
        ASSERT_TRUE(client.isConnected());

        client.disconnectAndWait();
        ASSERT_FALSE(client.isConnected());
    }

    srv.done(rounds, rounds);
}

TEST(NetClientServer, ConnectionTimeoutTest) {
    TestClient client{ };
    EXPECT_FALSE(client.connect(core::Address::localhost(TEST_PORT_BASE + 2), DEFAULT_TIMEOUT));
}

TEST(NetClientServer, SendReceiveSingleChannelTest) {
    std::string const msg{ "Hello, Server!" };
    std::string server_received;
    std::string client_received;

    TestServerService srv{
        [&](TestServer&, core::ServerReceiveEvent e) {
            server_received = core::asStringView(e.data);
            srv.server.send(e.client, e.data, e.channel_id, core::SendMode{ });
        },
        TEST_PORT_BASE + 3,
    };
    TestClient client{
        NoAction{ },
        [&](TestClient&, core::ReceiveEvent e) {
            client_received = core::asStringView(e.data);
        },
    };

    ASSERT_TRUE(client.connectAndWait(TEST_PORT_BASE + 3));
    ASSERT_TRUE(client.isConnected());

    ASSERT_TRUE(client.send(core::asByteSpan(msg), 0, core::SendMode{ }));
    client.pollAndWait();

    srv.done(1, 0);

    EXPECT_EQ(server_received, msg);
    EXPECT_EQ(client_received, msg);
}

TEST(NetClientServer, MultipleChannelsAndModesTest) {
    static constexpr uint32_t MAX_CHANNELS{ 3 };
    std::string server_received[3];
    std::string client_received[3];

    TestServerService srv{
        [&](TestServer&, core::ServerReceiveEvent e) {
            server_received[e.channel_id] = core::asStringView(e.data);
            srv.server.send(e.client, e.data, e.channel_id, core::SendMode{ });
        },
        TEST_PORT_BASE + 4,
        1,
        MAX_CHANNELS,
    };
    TestClient client{
        NoAction{ },
        [&](TestClient&, core::ReceiveEvent e) {
            client_received[e.channel_id] = core::asStringView(e.data);
        },
        MAX_CHANNELS,
    };

    ASSERT_TRUE(client.connectAndWait(TEST_PORT_BASE + 4));

    auto send = [&](std::string_view const msg, uint8_t ch, core::SendMode::Flag mode) {
        client.send(core::asByteSpan(msg), ch, core::SendMode{ mode });
    };

    send("Reliable", 0, core::SendMode::Reliable);
    send("Unseq",    1, core::SendMode::Unsequenced);
    send("Fragment", 2, core::SendMode::UnreliableFragment);
    client.pollAndWait();

    srv.done(1, 0);

    EXPECT_EQ(server_received[0], "Reliable");
    EXPECT_EQ(server_received[1], "Unseq");
    EXPECT_EQ(server_received[2], "Fragment");
    EXPECT_EQ(client_received[0], "Reliable");
    EXPECT_EQ(client_received[1], "Unseq");
    EXPECT_EQ(client_received[2], "Fragment");
}

TEST(NetClientServer, MultipleClientsEchoTest) {
    static constexpr uint32_t NUM_CLIENTS = 12;
    std::string received[NUM_CLIENTS];

    TestServerService srv{
        [&](TestServer&, core::ServerReceiveEvent e) {
            srv.server.send(e.client, e.data, e.channel_id, core::SendMode{ });
        },
        TEST_PORT_BASE + 5,
        NUM_CLIENTS,
        1,
    };

    std::vector<TestClient> clients;
    clients.reserve(NUM_CLIENTS);
    for (uint32_t i = 0; i < NUM_CLIENTS; ++i) {
        clients.emplace_back(
            NoAction{ },
            [i, &received](TestClient&, core::ReceiveEvent e) {
                received[i] = core::asStringView(e.data);
            }
        );
    }

    for (uint32_t i = 0; i < NUM_CLIENTS; ++i) {
        ASSERT_TRUE(clients[i].connectAndWait(TEST_PORT_BASE + 5));
    }

    for (uint32_t i = 0; i < NUM_CLIENTS; ++i) {
        clients[i].sendAndPoll("Client " + std::to_string(i), 0, core::SendMode{ });
    }

    srv.done(NUM_CLIENTS, 0);

    for (uint32_t i = 0; i < NUM_CLIENTS; ++i) {
        EXPECT_EQ(received[i], "Client " + std::to_string(i));
    }
}

TEST(NetClientServer, MessageRelayTest) {
    static constexpr uint32_t NUM_CLIENTS = 12;
    std::string received[NUM_CLIENTS];

    TestServerService srv{
        [&](TestServer& self, core::ServerReceiveEvent e) {
            std::string_view const raw = core::asStringView(e.data);
            if (raw.starts_with("to:")) {
                size_t const colon = raw.find(':', 3);
                if (colon != std::string::npos) {
                    uint32_t const target = std::stoul(std::string{ raw.substr(3, colon - 3) });
                    std::string const payload = std::string{ raw.substr(colon + 1) };
                    if (std::optional client = self.client(target)) {
                        srv.server.send(client, core::asByteSpan(payload), 0, core::SendMode{ });
                    }
                }
            }
        },
        TEST_PORT_BASE + 6,
        NUM_CLIENTS,
    };

    std::vector<TestClient> clients;
    clients.reserve(NUM_CLIENTS);
    for (uint32_t i = 0; i < NUM_CLIENTS; ++i) {
        clients.emplace_back(
            NoAction{ },
            [i, &received](TestClient&, core::ReceiveEvent e) {
                received[i] = core::asStringView(e.data);
            }
        );
    }

    for (uint32_t i = 0; i < NUM_CLIENTS; ++i) {
        ASSERT_TRUE(clients[i].connectAndWait(TEST_PORT_BASE + 6));
    }

    std::string const msg = fmt::format("to:{}:Hello from 0", NUM_CLIENTS - 1);
    clients[0].send(core::asByteSpan(msg), 0, core::SendMode{ });
    for (uint32_t i = 0; i < NUM_CLIENTS; ++i) {
        clients[i].pollAndWait();
    }

    srv.done(NUM_CLIENTS, 0);

    EXPECT_EQ(received[NUM_CLIENTS - 1], "Hello from 0");
    for (uint32_t i = 0; i < NUM_CLIENTS; ++i) {
        if (i != NUM_CLIENTS - 1) {
            EXPECT_TRUE(received[i].empty()) << "Client " << i << " should not have received anything";
        }
    }
}

TEST(NetClientServer, ServerKickGracefulTest) {
    bool client_disconnected{ false };

    TestServerService srv{
        [&](TestServer& self, auto&&) { self.kick(0, DEFAULT_TIMEOUT, true); },
        TEST_PORT_BASE + 7,
    };
    TestClient client{ [&](auto&&...) { client_disconnected = true; } };

    ASSERT_TRUE(client.connectAndWait(TEST_PORT_BASE + 7));
    EXPECT_TRUE(client.isConnected());

    EXPECT_TRUE(client.sendAndPoll(" ", 0, core::SendMode{ }));

    EXPECT_TRUE(client_disconnected);
    EXPECT_FALSE(client.isConnected());
    srv.done(1, 1);
}

TEST(NetClientServer, ServerKickImmediateTest) {
    bool client_disconnected{ false };

    TestServerService srv{
        [&](TestServer& self, auto&&) { self.kick(0, std::nullopt, true); },
        TEST_PORT_BASE + 8,
    };
    TestClient client{ [&](auto&&...) { client_disconnected = true; } };

    ASSERT_TRUE(client.connectAndWait(TEST_PORT_BASE + 8));
    EXPECT_TRUE(client.isConnected());

    EXPECT_TRUE(client.sendAndPoll(" ", 0, core::SendMode{ }));

    // Client still doesn't know server already reset it
    EXPECT_FALSE(client_disconnected);
    EXPECT_TRUE(client.isConnected());
    srv.done(1, 0);
}

TEST(NetClientServer, ClientDisconnectImmediateTest) {
    bool client_disconnected = false;

    TestServerService srv{ NoAction{ }, TEST_PORT_BASE + 9 };
    TestClient client{ [&](auto&&...) { client_disconnected = true; } };

    ASSERT_TRUE(client.connectAndWait(TEST_PORT_BASE + 9));
    EXPECT_TRUE(client.isConnected());

    client.disconnectAndWait(std::nullopt);

    EXPECT_FALSE(client_disconnected);
    EXPECT_FALSE(client.isConnected());
    srv.done(1, 0); // Server still has no idea
}

TEST(NetClientServer, SendEmptyMessageTest) {
    std::string server_received;

    TestServerService srv{
        [&](TestServer&, core::ServerReceiveEvent e) {
            server_received = core::asStringView(e.data);
        },
        TEST_PORT_BASE + 10,
    };
    TestClient client{ };

    ASSERT_TRUE(client.connectAndWait(TEST_PORT_BASE + 10));

    ASSERT_TRUE(client.send(std::span<uint8_t const>{ }, 0, core::SendMode{ }));
    client.pollAndWait();

    srv.done(1, 0);
    EXPECT_EQ(server_received, "");
}

TEST(NetClientServer, SendLargeMessageTest) {
    const std::string msg(4'096, 'A');
    std::string server_received;

    TestServerService srv{
        [&](TestServer&, core::ServerReceiveEvent e) {
            server_received = core::asStringView(e.data);
        },
        TEST_PORT_BASE + 11,
    };
    TestClient client{ };

    ASSERT_TRUE(client.connectAndWait(TEST_PORT_BASE + 11));

    ASSERT_TRUE(client.sendAndPoll(msg, 0, core::SendMode{ core::SendMode::Reliable }));

    srv.done(1, 0);
    EXPECT_EQ(server_received, msg);
}

TEST(NetClientServer, MultipleMessagesInSequenceTest) {
    std::vector<std::string> server_received;

    TestServerService srv{
        [&](TestServer&, core::ServerReceiveEvent e) {
            server_received.emplace_back(core::asStringView(e.data));
        },
        TEST_PORT_BASE + 12,
    };
    TestClient client{ };
    ASSERT_TRUE(client.connectAndWait(TEST_PORT_BASE + 12));

    client.sendAndPoll("first",  0, core::SendMode{ core::SendMode::Reliable });
    client.sendAndPoll("second", 0, core::SendMode{ core::SendMode::Reliable });
    client.sendAndPoll("third",  0, core::SendMode{ core::SendMode::Reliable });

    srv.done(1, 0);

    ASSERT_EQ(server_received.size(), 3);
    EXPECT_EQ(server_received[0], "first");
    EXPECT_EQ(server_received[1], "second");
    EXPECT_EQ(server_received[2], "third");
}

TEST(NetClientServer, DisconnectDuringSendTest) {
    std::string server_received;
    bool client_disconnected{ false };

    TestServerService srv{
        [&](TestServer&, core::ServerReceiveEvent e) {
            server_received = core::asStringView(e.data);
        },
        TEST_PORT_BASE + 13,
    };
    TestClient client{ [&](auto&&...) { client_disconnected = true; } };

    ASSERT_TRUE(client.connectAndWait(TEST_PORT_BASE + 13));

    client.send(core::asByteSpan(std::string_view{ "msg" }), 0, core::SendMode{ });
    client.disconnectAndWait();

    srv.done(1, 1);
    EXPECT_TRUE(client_disconnected);
    EXPECT_FALSE(client.isConnected());
}

TEST(NetClientServer, MaxConnectionsRejectionTest) {
    TestServerService srv{ NoAction{ }, TEST_PORT_BASE + 14, 1, 1 };

    TestClient client1{ };
    TestClient client2{ };

    ASSERT_TRUE(client1.connectAndWait(TEST_PORT_BASE + 14));
    EXPECT_TRUE(client1.isConnected());

    EXPECT_FALSE(client2.connect(core::Address::localhost(TEST_PORT_BASE + 14), DEFAULT_TIMEOUT));

    srv.done(1, 0); // only first client connected, not disconnected yet
}

TEST(NetClientServer, ServerDestructorDisconnectsTest) {
    bool client_disconnected{ false };

    TestClient client{ [&](auto&&...) { client_disconnected = true; } };
    {
        TestServerService srv{ NoAction{ }, TEST_PORT_BASE + 15 };
        ASSERT_TRUE(client.connectAndWait(TEST_PORT_BASE + 15));
        EXPECT_TRUE(client.isConnected());
    }

    client.pollAndWait();
    EXPECT_TRUE(client_disconnected);
    EXPECT_FALSE(client.isConnected());
}

TEST(NetClientServer, ChannelIdBoundariesTest) {
    static constexpr uint32_t MAX_CHANNELS = 4;
    std::string server_received;

    TestServerService srv{
        [&](TestServer&, core::ServerReceiveEvent e) {
            if (e.channel_id == MAX_CHANNELS - 1) {
                server_received = core::asStringView(e.data);
            }
        },
        TEST_PORT_BASE + 16,
        1,
        MAX_CHANNELS,
    };
    TestClient client{ NoAction{ }, NoAction{ }, MAX_CHANNELS };
    ASSERT_TRUE(client.connectAndWait(TEST_PORT_BASE + 16));

    ASSERT_TRUE(client.sendAndPoll("boundary", MAX_CHANNELS - 1, core::SendMode{ }));

    srv.done(1, 0);
    EXPECT_EQ(server_received, "boundary");
}
