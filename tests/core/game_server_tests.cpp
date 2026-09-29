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
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

class ProtocolClient final : public core::Client {
public:
    std::vector<shared::Message> messages;

    ProtocolClient() : core::Client{ 2 } {}

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

bool joinManually(
    server::GameServer& server,
    ProtocolClient& client,
    char const character,
    shared::WorldMode const mode = shared::WorldMode::Flat,
    shared::WorldConfiguration const configuration = shared::World::canonicalConfiguration()
)
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
        && client.sendMessage(shared::JoinRequestMessage{
            .ch = character,
            .mode = mode,
            .configuration = configuration,
        })
        && client.waitFor([&client, character] {
            return !client.positions(character).empty();
        });
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();
    return joined;
}

shared::PolicyCapabilityRegistry movementPolicyRegistry()
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

std::expected<shared::PolicyCompilation, shared::PolicyDiagnostic> hardMovementDenyPolicy()
{
    static constexpr std::string_view SOURCE = R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policyRule("lock", "minecraft:flight", 0i64, 1u8)
    policyRule("lock", "minecraft:collision-bypass", 0i64, 1u8)
    policyAssign("all", "lock", 1u8)
}
)core";
    shared::PolicyHost compiler;
    auto compiled = compiler.compile("hard-movement-deny.core", SOURCE, {
        .jit_mode = shared::PolicyJitMode::Disabled,
        .require_interpreter_parity = true,
    });
    return compiled;
}

std::expected<shared::PolicyCompilation, shared::PolicyDiagnostic> invalidMovementPolicy()
{
    static constexpr std::string_view SOURCE = R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policyRule("invalid", "minecraft:flight", 0i64, 1u8)
    policyRule("invalid", "minecraft:collision-bypass", 1i64, 1u8)
    policyAssign("players", "invalid", 1u8)
}
)core";
    shared::PolicyHost compiler;
    auto compiled = compiler.compile("invalid-movement-permissions.core", SOURCE, {
        .jit_mode = shared::PolicyJitMode::Disabled,
        .require_interpreter_parity = true,
    });
    return compiled;
}

std::expected<shared::PolicyCompilation, shared::PolicyDiagnostic> explicitMovementPermissionPolicy(
    shared::PolicyEntityId const subject
)
{
    static constexpr std::string_view SOURCE_PREFIX = R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policyGroup("departing", 5u8, "")
    policyMember("departing", )core";
    static constexpr std::string_view SOURCE_SUFFIX = R"core(u64)
    policyRule("deny", "minecraft:flight", 0i64, 1u8)
    policyRule("deny", "minecraft:collision-bypass", 0i64, 1u8)
    policyAssign("departing", "deny", 1u8)
}
)core";
    std::string const source = std::string{SOURCE_PREFIX}
        + std::to_string(subject)
        + std::string{SOURCE_SUFFIX};
    shared::PolicyHost compiler;
    auto compiled = compiler.compile("explicit-movement-permission.core", source, {
        .jit_mode = shared::PolicyJitMode::Disabled,
        .require_interpreter_parity = true,
    });
    return compiled;
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

TEST_F(GameServerTest, MismatchedJoinConfigurationDoesNotCreateAuthoritativeState)
{
    ProtocolClient client;
    ASSERT_TRUE(connect(client));
    auto mismatched_configuration = shared::World::canonicalConfiguration();
    ++mismatched_configuration.seed;
    ASSERT_TRUE(client.sendMessage(shared::JoinRequestMessage{
        .ch = '@',
        .configuration = mismatched_configuration,
    }));
    ASSERT_TRUE(client.waitFor([&] { return client.responseCount() == 1U; }));
    EXPECT_FALSE(std::get<shared::JoinResponseMessage>(client.messages.back()).accepted);
    EXPECT_TRUE(client.positions('@').empty());

    ASSERT_TRUE(client.sendMessage(shared::JoinRequestMessage{ .ch = '@' }));
    ASSERT_TRUE(client.waitFor([&] { return !client.positions('@').empty(); }));
    EXPECT_EQ(client.responseCount(), 2U);
    EXPECT_EQ(std::ranges::count_if(client.messages, [](shared::Message const& message) {
        auto const* response = std::get_if<shared::JoinResponseMessage>(&message);
        return response != nullptr && response->accepted;
    }), 1);
}

TEST(GameServerFlightTest, AcceptsMatchingFlightModeAndReplicatesVerticalAuthority)
{
    server::GameServer server{ 0, {}, shared::WorldMode::Flight };
    ProtocolClient client;
    ASSERT_TRUE(joinManually(server, client, '@', shared::WorldMode::Flight));
    auto const spawn = client.positions('@').front();
    EXPECT_EQ(spawn.x, shared::World::FLIGHT_SPAWN.x);
    EXPECT_EQ(spawn.y, shared::World::FLIGHT_SPAWN.y);
    EXPECT_EQ(spawn.z, shared::World::FLIGHT_SPAWN.z);

    ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{
        .direction = { .x = 0, .y = 0, .z = 127 },
        .sequence = 1U,
    }));
    ASSERT_TRUE(pumpUntil(server, client, [&client] {
        auto const positions = client.positions('@');
        return !positions.empty() && positions.back().acknowledged_input_sequence == 1U;
    }));
    auto const moved = client.positions('@').back();
    EXPECT_EQ(moved.x, spawn.x);
    EXPECT_EQ(moved.y, spawn.y);
    EXPECT_EQ(moved.z, spawn.z);
    EXPECT_EQ(moved.z_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);
}

TEST(GameServerFlightTest, DefersUnknownTerrainAndRevalidatesPermissionsBeforeApplyingInput)
{
    server::GameServer server{0, {}, shared::WorldMode::Flight};
    ProtocolClient client;
    ASSERT_TRUE(joinManually(server, client, '@', shared::WorldMode::Flight));
    shared::ServerPlayerPositionMessage const spawn = client.positions('@').back();
    ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{
        .direction = {.x = 0U, .y = 0U, .z = 127U, .cycle_movement_capabilities = true},
        .sequence = 1U,
    }));

    static_cast<void>(server.tick(std::chrono::milliseconds{5}));
    client.poll(std::chrono::milliseconds{5});
    ASSERT_FALSE(client.positions('@').empty());
    EXPECT_EQ(client.positions('@').back().acknowledged_input_sequence, 0U);
    EXPECT_EQ(client.positions('@').back().z_subcell, spawn.z_subcell);
    EXPECT_EQ(client.positions('@').back().movement_capabilities.bits, spawn.movement_capabilities.bits);

    auto compilation = hardMovementDenyPolicy();
    ASSERT_TRUE(compilation.has_value()) << compilation.error().message;
    auto const published = server.publishPermissions(std::move(*compilation), movementPolicyRegistry());
    ASSERT_TRUE(published.has_value()) << published.error().message;
    ASSERT_TRUE(pumpUntil(server, client, [&client] {
        std::vector<shared::ServerPlayerPositionMessage> const positions = client.positions('@');
        return !positions.empty() && positions.back().acknowledged_input_sequence == 1U;
    }));

    shared::ServerPlayerPositionMessage const completed = client.positions('@').back();
    EXPECT_EQ(completed.movement_capabilities.bits, 0U);
    EXPECT_NE(completed.z_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);
}

TEST(GameServerFlightTest, ResumesDeferredInputAfterTerrainMaterializes)
{
    server::GameServer server{0, {}, shared::WorldMode::Flight};
    ProtocolClient client;
    ASSERT_TRUE(joinManually(server, client, '@', shared::WorldMode::Flight));
    shared::ServerPlayerPositionMessage const spawn = client.positions('@').back();
    ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{
        .direction = {.x = 0U, .y = 0U, .z = 127U, .cycle_movement_capabilities = true},
        .sequence = 1U,
    }));

    static_cast<void>(server.tick(std::chrono::milliseconds{5}));
    client.poll(std::chrono::milliseconds{5});
    ASSERT_FALSE(client.positions('@').empty());
    EXPECT_EQ(client.positions('@').back().acknowledged_input_sequence, 0U);
    EXPECT_EQ(client.positions('@').back().z_subcell, spawn.z_subcell);

    ASSERT_TRUE(pumpUntil(server, client, [&client] {
        std::vector<shared::ServerPlayerPositionMessage> const positions = client.positions('@');
        return !positions.empty() && positions.back().acknowledged_input_sequence == 1U;
    }));
    shared::ServerPlayerPositionMessage const completed = client.positions('@').back();
    EXPECT_EQ(completed.z_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);
    EXPECT_EQ(completed.movement_capabilities.bits, 1U);
}

TEST(GameServerFlightTest, DefersMixedJumpUntilAdjacentHorizontalChunkMaterializes)
{
    server::GameServer server{
        0,
        {{.character = '@', .x = 14, .y = 100, .z = 801}},
        shared::WorldMode::Flight,
    };
    ProtocolClient client;
    ASSERT_TRUE(joinManually(server, client, '@', shared::WorldMode::Flight));

    for (uint32_t sequence = 1U; sequence <= 2U; ++sequence) {
        ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{
            .direction = {
                .x = static_cast<uint8_t>(sequence == 1U ? 127U : 80U),
                .cycle_movement_capabilities = sequence == 1U,
            },
            .sequence = sequence,
        }));
        ASSERT_TRUE(pumpUntil(server, client, [&client, sequence] {
            return client.positions('@').back().acknowledged_input_sequence == sequence;
        }));
    }
    ASSERT_EQ(client.positions('@').back().x, 14);
    ASSERT_EQ(client.positions('@').back().x_subcell, 9'127U);

    ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{
        .direction = {.cycle_movement_capabilities = true},
        .sequence = 3U,
    }));
    ASSERT_TRUE(pumpUntil(server, client, [&client] {
        return client.positions('@').back().acknowledged_input_sequence == 3U;
    }));
    ASSERT_EQ(client.positions('@').back().movement_capabilities.bits, 0U);

    ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{
        .direction = {.x = 127U, .z = 127U},
        .sequence = 4U,
    }));
    static_cast<void>(server.tick(std::chrono::milliseconds{5}));
    client.poll(std::chrono::milliseconds{5});
    EXPECT_EQ(client.positions('@').back().acknowledged_input_sequence, 3U);
    ASSERT_TRUE(pumpUntil(server, client, [&client] {
        return client.positions('@').back().acknowledged_input_sequence == 4U;
    }));
    EXPECT_EQ(client.positions('@').back().x, 15);
    EXPECT_EQ(client.positions('@').back().x_subcell, 4'727U);
}

TEST(GameServerFlightTest, CyclesOnlyTheThreeServerValidatedMovementCapabilityStates)
{
    server::GameServer server{ 0, {}, shared::WorldMode::Flight };
    ProtocolClient client;
    ASSERT_TRUE(joinManually(server, client, '@', shared::WorldMode::Flight));
    ASSERT_EQ(client.positions('@').back().movement_capabilities.bits, 3U);

    for (uint32_t sequence = 1U; sequence <= 3U; ++sequence) {
        ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{
            .direction = { .cycle_movement_capabilities = true },
            .sequence = sequence,
        }));
        ASSERT_TRUE(pumpUntil(server, client, [&client, sequence] {
            auto const positions = client.positions('@');
            return !positions.empty() && positions.back().acknowledged_input_sequence == sequence;
        }));
    }

    std::vector<shared::ServerPlayerPositionMessage> const positions = client.positions('@');
    ASSERT_GE(positions.size(), 4U);
    EXPECT_EQ(positions[positions.size() - 3U].movement_capabilities.bits, 1U);
    EXPECT_EQ(positions[positions.size() - 2U].movement_capabilities.bits, 0U);
    EXPECT_EQ(positions.back().movement_capabilities.bits, 3U);
}

TEST(GameServerFlightTest, PublishedHardPermissionsOverrideAndReplicateMovementCapabilities)
{
    server::GameServer server{ 0, {}, shared::WorldMode::Flight };
    ProtocolClient client;
    ASSERT_TRUE(joinManually(server, client, '@', shared::WorldMode::Flight));
    ASSERT_EQ(client.positions('@').back().movement_capabilities.bits, 3U);

    auto compilation = hardMovementDenyPolicy();
    ASSERT_TRUE(compilation.has_value()) << compilation.error().message;
    auto const published = server.publishPermissions(std::move(*compilation), movementPolicyRegistry());
    ASSERT_TRUE(published.has_value()) << published.error().message;
    ASSERT_TRUE(pumpUntil(server, client, [&client] {
        std::vector<shared::ServerPlayerPositionMessage> const positions = client.positions('@');
        return !positions.empty() && positions.back().movement_capabilities.bits == 0U;
    }));
    EXPECT_EQ(client.positions('@').back().movement_capabilities.bits, 0U);

    ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{
        .direction = { .cycle_movement_capabilities = true },
        .sequence = 1U,
    }));
    ASSERT_TRUE(pumpUntil(server, client, [&client] {
        std::vector<shared::ServerPlayerPositionMessage> const positions = client.positions('@');
        return !positions.empty() && positions.back().acknowledged_input_sequence == 1U;
    }));
    EXPECT_EQ(client.positions('@').back().movement_capabilities.bits, 0U);
}

TEST(GameServerFlightTest, RejectsInvalidMovementPolicyWithoutChangingPublishedState)
{
    server::GameServer server{ 0, {}, shared::WorldMode::Flight };
    ProtocolClient client;
    ASSERT_TRUE(joinManually(server, client, '@', shared::WorldMode::Flight));
    ASSERT_EQ(client.positions('@').back().movement_capabilities.bits, 3U);

    auto compilation = invalidMovementPolicy();
    ASSERT_TRUE(compilation.has_value()) << compilation.error().message;
    auto const published = server.publishPermissions(std::move(*compilation), movementPolicyRegistry());
    EXPECT_FALSE(published.has_value());
    EXPECT_EQ(client.positions('@').back().movement_capabilities.bits, 3U);

    ASSERT_TRUE(client.sendMessage(shared::ClientInputMessage{
        .direction = { .x = 0U, .y = 0U, .z = 127U },
        .sequence = 1U,
    }));
    ASSERT_TRUE(pumpUntil(server, client, [&client] {
        std::vector<shared::ServerPlayerPositionMessage> const positions = client.positions('@');
        return !positions.empty() && positions.back().acknowledged_input_sequence == 1U;
    }));
    EXPECT_EQ(client.positions('@').back().z_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);
}

TEST(GameServerFlightTest, AppliesPublishedPermissionsBeforeAcceptingNewPlayers)
{
    server::GameServer server{ 0, {}, shared::WorldMode::Flight };
    ProtocolClient first;
    ProtocolClient newcomer;
    ProtocolClient observer;
    ASSERT_TRUE(joinManually(server, first, '@', shared::WorldMode::Flight));
    ASSERT_TRUE(joinManually(server, observer, '$', shared::WorldMode::Flight));

    auto compilation = hardMovementDenyPolicy();
    ASSERT_TRUE(compilation.has_value()) << compilation.error().message;
    auto const published = server.publishPermissions(std::move(*compilation), movementPolicyRegistry());
    ASSERT_TRUE(published.has_value()) << published.error().message;
    auto const firstHasRestriction = [&first] {
        std::vector<shared::ServerPlayerPositionMessage> const positions = first.positions('@');
        return !positions.empty() && positions.back().movement_capabilities.bits == 0U;
    };
    ASSERT_TRUE(pumpUntil(server, first, firstHasRestriction));
    ASSERT_TRUE(pumpUntil(server, observer, [&observer] {
        std::vector<shared::ServerPlayerPositionMessage> const positions = observer.positions('@');
        return !positions.empty() && positions.back().movement_capabilities.bits == 0U;
    }));

    ASSERT_TRUE(joinManually(server, newcomer, '#', shared::WorldMode::Flight));
    ASSERT_TRUE(pumpUntil(server, newcomer, [&newcomer] {
        std::vector<shared::ServerPlayerPositionMessage> const own = newcomer.positions('#');
        std::vector<shared::ServerPlayerPositionMessage> const first_player = newcomer.positions('@');
        return !own.empty() && !first_player.empty()
            && own.back().movement_capabilities.bits == 0U
            && first_player.back().movement_capabilities.bits == 0U;
    }));
    ASSERT_TRUE(pumpUntil(server, observer, [&observer] {
        std::vector<shared::ServerPlayerPositionMessage> const newcomer_positions = observer.positions('#');
        return !newcomer_positions.empty() && newcomer_positions.back().movement_capabilities.bits == 0U;
    }));
}

TEST(GameServerFlightTest, ExpiresExplicitPermissionMembershipAfterDisconnect)
{
    static constexpr std::chrono::seconds DISCONNECT_TIMEOUT{ 1 };
    static constexpr std::chrono::milliseconds SERVER_POLL_INTERVAL{ 1 };
    server::GameServer server{ 0, {}, shared::WorldMode::Flight };
    ProtocolClient departing;
    ProtocolClient observer;
    ProtocolClient replacement;
    ASSERT_TRUE(joinManually(server, departing, '@', shared::WorldMode::Flight));
    std::vector<core::ClientId> const connected_clients = server.collectConnectedClients();
    ASSERT_EQ(connected_clients.size(), 1U);
    ASSERT_TRUE(joinManually(server, observer, '$', shared::WorldMode::Flight));

    auto compilation = explicitMovementPermissionPolicy(connected_clients.front());
    ASSERT_TRUE(compilation.has_value()) << compilation.error().message;
    auto const published = server.publishPermissions(std::move(*compilation), movementPolicyRegistry());
    ASSERT_TRUE(published.has_value()) << published.error().message;
    ASSERT_TRUE(pumpUntil(server, departing, [&departing] {
        std::vector<shared::ServerPlayerPositionMessage> const positions = departing.positions('@');
        return !positions.empty() && positions.back().movement_capabilities.bits == 0U;
    }));

    std::atomic_bool stop_requested{ false };
    std::thread server_thread{ [&server, &stop_requested] {
        while (!stop_requested.load(std::memory_order_relaxed)) {
            static_cast<void>(server.poll(SERVER_POLL_INTERVAL));
        }
    } };
    bool const disconnected = departing.disconnect(DISCONNECT_TIMEOUT);
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();
    ASSERT_TRUE(disconnected);
    ASSERT_TRUE(observer.waitFor([&observer] {
        return std::ranges::any_of(observer.messages, [](shared::Message const& message) {
            auto const* removal = std::get_if<shared::ServerRemovePlayerMessage>(&message);
            return removal != nullptr && removal->ch == '@';
        });
    }));

    ASSERT_TRUE(joinManually(server, replacement, '#', shared::WorldMode::Flight));
    EXPECT_EQ(replacement.positions('#').back().movement_capabilities.bits, 3U);
}

TEST(GameServerFlightTest, HardPermissionsPreserveCollisionAcrossTheWrappedSeam)
{
    static constexpr uint32_t WORLD_EDGE_X = static_cast<uint32_t>(shared::World::FLIGHT_MAX_CELL);
    static constexpr uint32_t Y = 100U;
    shared::TerrainGenerator const terrain;
    int32_t const spawn_z = std::max(
        static_cast<int32_t>(terrain.heightAt(WORLD_EDGE_X, Y)),
        static_cast<int32_t>(terrain.heightAt(0U, Y))
    );
    server::GameServer server{ 0, {
        { .character = '@', .x = shared::World::FLIGHT_MAX_CELL, .y = static_cast<int32_t>(Y), .z = spawn_z },
        { .character = '#', .x = 0, .y = static_cast<int32_t>(Y), .z = spawn_z },
    }, shared::WorldMode::Flight };
    ProtocolClient first;
    ProtocolClient second;
    ASSERT_TRUE(joinManually(server, first, '@', shared::WorldMode::Flight));
    ASSERT_TRUE(joinManually(server, second, '#', shared::WorldMode::Flight));

    auto compilation = hardMovementDenyPolicy();
    ASSERT_TRUE(compilation.has_value()) << compilation.error().message;
    auto const published = server.publishPermissions(std::move(*compilation), movementPolicyRegistry());
    ASSERT_TRUE(published.has_value()) << published.error().message;
    auto const hasHardRestriction = [](ProtocolClient const& client, char const character) {
        std::vector<shared::ServerPlayerPositionMessage> const positions = client.positions(character);
        return !positions.empty() && positions.back().movement_capabilities.bits == 0U;
    };
    ASSERT_TRUE(pumpUntil(server, first, [&] {
        return hasHardRestriction(first, '@') && hasHardRestriction(first, '#');
    }));
    ASSERT_TRUE(pumpUntil(server, second, [&] {
        return hasHardRestriction(second, '@') && hasHardRestriction(second, '#');
    }));

    shared::ServerPlayerPositionMessage const before = first.positions('@').back();
    ASSERT_TRUE(first.sendMessage(shared::ClientInputMessage{
        .direction = { .x = 127U },
        .sequence = 1U,
    }));
    ASSERT_TRUE(pumpUntil(server, first, [&] {
        std::vector<shared::ServerPlayerPositionMessage> const positions = first.positions('@');
        return !positions.empty() && positions.back().acknowledged_input_sequence == 1U;
    }));

    shared::ServerPlayerPositionMessage const after = first.positions('@').back();
    EXPECT_EQ(after.x, shared::World::FLIGHT_MAX_CELL);
    EXPECT_EQ(after.x_subcell, before.x_subcell);
    EXPECT_EQ(after.movement_capabilities.bits, 0U);
}

TEST_F(GameServerTest, CapabilityChangesReachStationaryObservers)
{
    ProtocolClient first;
    ProtocolClient observer;
    ASSERT_TRUE(join(first, '@'));
    ASSERT_TRUE(join(observer, '#'));
    size_t const observer_messages_before = observer.positions('@').size();

    ASSERT_TRUE(first.sendMessage(shared::ClientInputMessage{
        .direction = { .cycle_movement_capabilities = true },
        .sequence = 1U,
    }));
    ASSERT_TRUE(first.waitFor([&first] {
        std::vector<shared::ServerPlayerPositionMessage> const positions = first.positions('@');
        return !positions.empty() && positions.back().acknowledged_input_sequence == 1U;
    }));
    ASSERT_TRUE(observer.waitFor([&observer, observer_messages_before] {
        return observer.positions('@').size() > observer_messages_before;
    }));

    EXPECT_EQ(first.positions('@').back().movement_capabilities.bits, 3U);
    EXPECT_EQ(observer.positions('@').back().movement_capabilities.bits, 3U);
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
        return !positions.empty() && positions.back().acknowledged_input_sequence == 1U;
    }));
    auto const after_idle_tick = client.positions('@').back();
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
    uint32_t const first_inputs_before_join = first_inputs_sent.load(std::memory_order_relaxed);
    uint32_t const second_inputs_before_join = second_inputs_sent.load(std::memory_order_relaxed);
    bool const normal_cadence_backlog_created = first_inputs_before_join >= INPUTS_PER_SENDER_BEFORE_JOIN
        && second_inputs_before_join >= INPUTS_PER_SENDER_BEFORE_JOIN;

    auto const join_started = std::chrono::steady_clock::now();
    bool const joined = normal_cadence_backlog_created && join(newcomer, '$');
    bool const received_authoritative_state = joined && newcomer.waitFor([&] {
        return !newcomer.positions('@').empty() && !newcomer.positions('#').empty();
    });
    auto const join_elapsed = std::chrono::steady_clock::now() - join_started;

    stop_sending.store(true, std::memory_order_relaxed);
    first_sender.join();
    second_sender.join();

    EXPECT_FALSE(send_failed.load(std::memory_order_relaxed));
    EXPECT_GE(first_inputs_before_join, INPUTS_PER_SENDER_BEFORE_JOIN);
    EXPECT_GE(second_inputs_before_join, INPUTS_PER_SENDER_BEFORE_JOIN);
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
        return std::ranges::any_of(positions, [](auto const& position) {
            return position.acknowledged_input_sequence == 1U;
        });
    }));
    auto const positions_after_first_tick = client.positions('@');
    auto const first_tick = std::ranges::find(
        positions_after_first_tick,
        1U,
        &shared::ServerPlayerPositionMessage::acknowledged_input_sequence
    );
    ASSERT_NE(first_tick, positions_after_first_tick.end());
    auto const after_first_tick = *first_tick;
    EXPECT_EQ(after_first_tick.x_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);
    EXPECT_EQ(
        std::ranges::find(
            positions_after_first_tick,
            2U,
            &shared::ServerPlayerPositionMessage::acknowledged_input_sequence
        ),
        positions_after_first_tick.end()
    );

    ASSERT_TRUE(pumpUntil(server, client, [&client] {
        auto const positions = client.positions('@');
        return std::ranges::any_of(positions, [](auto const& position) {
            return position.acknowledged_input_sequence == 2U;
        });
    }));
    auto const positions_after_second_tick = client.positions('@');
    auto const second_tick = std::ranges::find(
        positions_after_second_tick,
        2U,
        &shared::ServerPlayerPositionMessage::acknowledged_input_sequence
    );
    ASSERT_NE(second_tick, positions_after_second_tick.end());
    auto const after_second_tick = *second_tick;
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
