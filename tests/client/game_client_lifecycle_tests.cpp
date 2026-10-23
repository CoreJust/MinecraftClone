#include <client/ClientAudio.hpp>
#include <client/GameClient.hpp>
#include <client/WorldLoading.hpp>
#include <server/GameServer.hpp>
#include <shared/net/Message.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldCoverage.hpp>
#include <generation/Generation.hpp>
#include <core/net/Address.hpp>
#include <core/net/Event.hpp>
#include <core/net/Server.hpp>

#include <enet/enet.h>
#include <enet/protocol.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

// <cstring> provides std::memcpy; libc's private wrapper is not a dependency.
// IWYU pragma: no_include <_string.h>

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

class TerrainReceiptServer final : public core::Server {
public:
    using Receipt = std::variant<shared::ClientChunkDeliveryAckMessage, shared::ClientAuthoritativeTerrainAckMessage>;

    TerrainReceiptServer()
        : Server{ core::Address::localhost(0U), 1U, 2U }
    { }

    [[nodiscard]] bool waitForDelivery(uint64_t const token)
    {
        std::unique_lock lock{ m_mutex };
        return m_changed.wait_for(lock, std::chrono::seconds{ 2 }, [this, token] {
            return std::ranges::any_of(m_receipts, [token](Receipt const& receipt) {
                return std::visit([token](auto const& value) { return value.delivery_token == token; }, receipt);
            });
        });
    }

    [[nodiscard]] std::vector<Receipt> receipts() const
    {
        std::scoped_lock lock{ m_mutex };
        return m_receipts;
    }

private:
    void onConnected(core::ServerConnectEvent) override {}
    void onDisconnected(core::ServerDisconnectEvent) override {}
    void onReceived(core::ServerReceiveEvent event) override
    {
        if (event.channel_id != shared::GAME_CHANNEL) {
            return;
        }
        auto const decoded = shared::decodeMessage(event.data);
        if (!decoded) {
            return;
        }
        std::scoped_lock lock{ m_mutex };
        if (auto const* dense = std::get_if<shared::ClientChunkDeliveryAckMessage>(&*decoded)) {
            if (dense->delivery_token == 0U) {
                return;
            }
            m_receipts.emplace_back(*dense);
        } else if (auto const* profile = std::get_if<shared::ClientAuthoritativeTerrainAckMessage>(&*decoded)) {
            m_receipts.emplace_back(*profile);
        } else {
            return;
        }
        m_changed.notify_all();
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<Receipt> m_receipts;
};

class ServerPump final {
public:
    explicit ServerPump(core::Server& server)
        : m_thread{ [this, &server] {
            while (!m_stop.load(std::memory_order_relaxed)) {
                static_cast<void>(server.poll(std::chrono::milliseconds{ 1 }));
            }
        } }
    { }

    ~ServerPump()
    {
        m_stop.store(true, std::memory_order_relaxed);
        m_thread.join();
    }

private:
    std::atomic_bool m_stop{ false };
    std::thread m_thread;
};

class CoverageClient final : public client::GameClient {
public:
    using Clock = std::chrono::steady_clock;

    CoverageClient()
        : GameClient{ shared::WorldMode::Flat, shared::World::canonicalConfiguration(), false }
    { }

    [[nodiscard]] bool initialize(shared::PlayerPosition const spawn, Clock::time_point const accepted_at)
    {
        if (!beginLoadingRequest(accepted_at)) {
            return false;
        }
        m_accepted = true;

        generation::GenerationConfiguration configuration{};
        configuration.seed = m_world.configuration().seed;
        m_session = shared::TerrainSessionIdentity{
            .join_epoch = 1U,
            .terrain = shared::TerrainIdentity{
                .terrain_version = generation::TERRAIN_VERSION,
                .revision = 1U,
                .seed = configuration.seed,
                .configuration_identity = generation::configurationIdentity(configuration),
            },
            .configuration = configuration,
        };
        return applyWorldPreparation(shared::ServerWorldPreparationMessage{
            .session = m_session,
            .sequence = 1U,
            .stage = generation::GenerationStage::Complete,
            .work_completed = 1U,
            .work_total = 1U,
            .server_elapsed_microseconds = 0U,
            .latest_snapshot = nullptr,
            .final_atlas_complete = true,
            .refined_safe_spawn = spawn,
            .failure = {},
        }, accepted_at + std::chrono::milliseconds{ 1 });
    }

    [[nodiscard]] bool applyCoverage(
        shared::PlayerPosition const center,
        Clock::time_point const received_at,
        uint64_t const coverage_epoch = 1U,
        uint16_t const actual_radius_blocks = shared::ACTUAL_TERRAIN_RADIUS_BLOCKS,
        uint16_t const visual_lod_radius_blocks = shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS
    )
    {
        return applyChunkCoverage(shared::ServerChunkCoverageMessage{
            .session = m_session,
            .coverage_epoch = coverage_epoch,
            .center = center,
            .actual_radius_blocks = actual_radius_blocks,
            .visual_lod_radius_blocks = visual_lod_radius_blocks,
        }, received_at);
    }

    [[nodiscard]] bool applyAirBatch(
        uint64_t const delivery_token,
        std::span<shared::ChunkCoordinate const> coordinates,
        Clock::time_point const received_at,
        uint64_t const coverage_epoch = 1U,
        bool const wrong_session = false
    )
    {
        shared::ServerMaterializedChunkBatchMessage batch{
            .session = m_session,
            .coverage_epoch = coverage_epoch,
            .delivery_token = delivery_token,
            .chunks = {},
        };
        batch.chunks.reserve(coordinates.size());
        for (shared::ChunkCoordinate const coordinate : coordinates) {
            batch.chunks.push_back(shared::MaterializedChunkPayload{
                .coordinate = coordinate,
                .content_hash = shared::Chunk{ coordinate }.contentIdentity().content_hash,
                .data = shared::UniformChunkData{ .material = shared::Block::Air },
            });
        }
        if (wrong_session) {
            ++batch.session.join_epoch;
        }
        return applyMaterializedChunkBatch(batch, received_at);
    }

    [[nodiscard]] shared::TerrainSessionIdentity const& session() const noexcept { return m_session; }

    [[nodiscard]] bool applyProfiles(
        std::vector<shared::ChunkCoordinate> const& coordinates,
        uint64_t const token, uint64_t const epoch, Clock::time_point const received_at,
        bool const wrong_session = false
    )
    {
        shared::ServerAuthoritativeTerrainBatchMessage batch{
            .session = m_session, .coverage_epoch = epoch, .delivery_token = token,
            .tiles = {},
        };
        for (auto const coordinate : coordinates) {
            shared::AuthoritativeTerrainTile tile{
                .coordinate = coordinate, .identity = m_session.terrain,
                .block_sea_level = 64U, .max_height = 1'023U, .revision = 1U,
            };
            tile.columns.fill({ 100U, 97U, 100U, shared::Block::Grass, shared::Block::Dirt });
            batch.tiles.push_back(tile);
        }
        if (wrong_session) {
            ++batch.session.join_epoch;
        }
        return applyAuthoritativeTerrainBatch(batch, received_at);
    }

    [[nodiscard]] bool evictResidentForCoverageTest(shared::ChunkCoordinate const coordinate)
    {
        auto* const world = const_cast<shared::SparseWorld*>(authoritativeWorld());
        auto const center = activeCoverageCenter();
        if (!world || !center || !world->setProtectedChunks(std::span<shared::ChunkCoordinate const>{})) {
            return false;
        }
        bool const evicted = world->evict(coordinate);
        std::vector<shared::ChunkCoordinate> const interest = shared::materializedInterestChunkKeys(*center);
        bool const protection_restored = world->setProtectedChunks(interest);
        return evicted && protection_restored;
    }

    [[nodiscard]] bool hasResidentChunk(shared::ChunkCoordinate const coordinate) const
    {
        shared::SparseWorld const* const world = authoritativeWorld();
        return world && world->residentChunk(coordinate) != nullptr;
    }

    [[nodiscard]] bool isProtectedChunk(shared::ChunkCoordinate const coordinate) const
    {
        shared::SparseWorld const* const world = authoritativeWorld();
        return world && world->isChunkProtected(coordinate);
    }

private:
    void onDisconnected(core::DisconnectEvent const) override { }
    shared::Direction input() override { return {}; }
    void render() override { }

    shared::TerrainSessionIdentity m_session{};
};

class LifecycleClient final : public client::GameClient {
public:
    explicit LifecycleClient(std::function<void(LifecycleClient&)> service)
        : GameClient{
            shared::WorldMode::Flat,
            shared::World::canonicalConfiguration(),
            true,
            std::make_unique<ImmediateAudioOutput>(),
        }
        , m_service{ std::move(service) }
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

class ServiceCadence final {
public:
    static constexpr std::chrono::milliseconds SOFT_GAP_LIMIT{ 200 };
    static constexpr std::chrono::milliseconds HARD_GAP_LIMIT{ 250 };
    static constexpr uint32_t MAX_SOFT_GAP_OVERRUNS = 1U;

    void recordGap(std::chrono::steady_clock::duration const gap)
    {
        ++m_callback_count;
        m_maximum_gap = (std::max)(m_maximum_gap, gap);
        if (gap > SOFT_GAP_LIMIT) {
            ++m_soft_gap_overruns;
        }
        if (gap >= HARD_GAP_LIMIT) {
            m_hard_gap_observed = true;
        }
    }

    [[nodiscard]] bool meetsBudget(uint32_t const minimum_callback_count) const noexcept
    {
        return m_callback_count >= minimum_callback_count
            && m_soft_gap_overruns <= MAX_SOFT_GAP_OVERRUNS
            && !m_hard_gap_observed;
    }

    [[nodiscard]] uint32_t callbackCount() const noexcept { return m_callback_count; }
    [[nodiscard]] uint32_t softGapOverruns() const noexcept { return m_soft_gap_overruns; }
    [[nodiscard]] std::chrono::steady_clock::duration maximumGap() const noexcept
    {
        return m_maximum_gap;
    }
private:
    uint32_t m_callback_count = 0U;
    uint32_t m_soft_gap_overruns = 0U;
    bool m_hard_gap_observed = false;
    std::chrono::steady_clock::duration m_maximum_gap{};
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
        std::vector<uint8_t> bytes(65'536U);
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
        .should_stop = {},
        .is_uncapped_phase = {},
        .input_override = {},
        .start_input_override = {},
        .on_loop = {},
        .on_authoritative_player = {},
        .on_input_sent = {},
        .on_authoritative_state = {},
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
        .should_stop = {},
        .is_uncapped_phase = {},
        .input_override = {},
        .start_input_override = {},
        .on_loop = {},
        .on_authoritative_player = {},
        .on_input_sent = {},
        .on_authoritative_state = {},
    };
    game_client.run(core::Address::localhost(port), '@', &hooks);

    EXPECT_TRUE(stop_processed);
    EXPECT_FALSE(game_client.isConnected());
    EXPECT_FALSE(game_client.accepted());
    EXPECT_EQ(game_client.inputCalls(), 0U);
    EXPECT_GT(game_client.renderCalls(), 0U);
    EXPECT_LT(std::chrono::steady_clock::now() - started, STOP_BOUND);
}

TEST(GameClientLifecycleTest, StopDuringRetryIsServicedWithoutStartingAnotherAttempt)
{
    static constexpr std::chrono::milliseconds STOP_DELAY{ 30'100 };
    static constexpr std::chrono::seconds DEADLINE{ 32 };
    static constexpr std::chrono::seconds REPLY_DELAY{ 60 };
    static constexpr uint32_t MINIMUM_SERVICE_CALLS = static_cast<uint32_t>(
        STOP_DELAY / ServiceCadence::SOFT_GAP_LIMIT + 1
    );
    server::GameServer server{ 0U };
    HandshakeReplyRelay relay{ server.port(), REPLY_DELAY };
    auto const started = std::chrono::steady_clock::now();
    auto previous_service = started;
    ServiceCadence service_cadence;
    bool stop_processed = false;
    LifecycleClient game_client{ [&](LifecycleClient& client) {
        auto const now = std::chrono::steady_clock::now();
        service_cadence.recordGap(now - previous_service);
        previous_service = now;
        relay.pump();
        static_cast<void>(server.tick());
        relay.pump();
        if (now - started >= STOP_DELAY) {
            stop_processed = true;
            client.requestStop();
        }
    } };
    client::GameClientBenchmarkHooks const hooks{
        .deadline = started + DEADLINE,
        .should_stop = {},
        .is_uncapped_phase = {},
        .input_override = {},
        .start_input_override = {},
        .on_loop = {},
        .on_authoritative_player = {},
        .on_input_sent = {},
        .on_authoritative_state = {},
    };
    game_client.run(core::Address::localhost(relay.port()), '@', &hooks);

    EXPECT_TRUE(stop_processed);
    EXPECT_TRUE(service_cadence.meetsBudget(MINIMUM_SERVICE_CALLS))
        << "callbacks=" << service_cadence.callbackCount()
        << ", gaps>200ms=" << service_cadence.softGapOverruns()
        << ", max gap (us)="
        << std::chrono::duration_cast<std::chrono::microseconds>(service_cadence.maximumGap()).count();
    EXPECT_LT(
        std::chrono::steady_clock::now() - started,
        STOP_DELAY + ServiceCadence::SOFT_GAP_LIMIT
    );
    EXPECT_FALSE(game_client.isConnected());
    EXPECT_FALSE(game_client.accepted());
    EXPECT_EQ(game_client.inputCalls(), 0U);
    auto const elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_GT(game_client.renderCalls(), 0U);
    EXPECT_LE(
        game_client.renderCalls(),
        static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 10 + 1)
    );
    EXPECT_EQ(relay.attemptCount(), 1U);
    EXPECT_FALSE(relay.releasedDelayedReply());
}

TEST(GameClientLifecycleTest, ServiceCadenceBudgetAllowsOneBoundedOutlier)
{
    static constexpr uint32_t MINIMUM_SERVICE_CALLS = 151U;
    ServiceCadence service_cadence;
    for (uint32_t callback = 0U; callback < MINIMUM_SERVICE_CALLS; ++callback) {
        service_cadence.recordGap(
            callback == 0U ? std::chrono::milliseconds{ 201 } : ServiceCadence::SOFT_GAP_LIMIT
        );
    }

    EXPECT_TRUE(service_cadence.meetsBudget(MINIMUM_SERVICE_CALLS));
}

TEST(GameClientLifecycleTest, ServiceCadenceBudgetRejectsSparseCallbacks)
{
    static constexpr uint32_t MINIMUM_SERVICE_CALLS = 151U;
    ServiceCadence service_cadence;
    for (uint32_t callback = 0U; callback + 1U < MINIMUM_SERVICE_CALLS; ++callback) {
        service_cadence.recordGap(ServiceCadence::SOFT_GAP_LIMIT);
    }

    EXPECT_FALSE(service_cadence.meetsBudget(MINIMUM_SERVICE_CALLS));
}

TEST(GameClientLifecycleTest, ServiceCadenceBudgetRejectsRepeatedSlowGaps)
{
    static constexpr uint32_t MINIMUM_SERVICE_CALLS = 151U;
    ServiceCadence service_cadence;
    for (uint32_t callback = 0U; callback < MINIMUM_SERVICE_CALLS; ++callback) {
        service_cadence.recordGap(
            callback < 2U ? ServiceCadence::SOFT_GAP_LIMIT + std::chrono::milliseconds{ 1 }
                : ServiceCadence::SOFT_GAP_LIMIT
        );
    }

    EXPECT_FALSE(service_cadence.meetsBudget(MINIMUM_SERVICE_CALLS));
}

TEST(GameClientLifecycleTest, ServiceCadenceBudgetRejectsOneLongGap)
{
    static constexpr uint32_t MINIMUM_SERVICE_CALLS = 151U;
    ServiceCadence service_cadence;
    for (uint32_t callback = 0U; callback < MINIMUM_SERVICE_CALLS; ++callback) {
        service_cadence.recordGap(
            callback == 0U ? ServiceCadence::HARD_GAP_LIMIT : ServiceCadence::SOFT_GAP_LIMIT
        );
    }

    EXPECT_FALSE(service_cadence.meetsBudget(MINIMUM_SERVICE_CALLS));
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
                || client.inputCalls() != 0U;
        }
    } };
    client::GameClientBenchmarkHooks const hooks{
        .deadline = started + DEADLINE,
        .should_stop = {},
        .is_uncapped_phase = {},
        .input_override = {},
        .start_input_override = {},
        .on_loop = {},
        .on_authoritative_player = {},
        .on_input_sent = {},
        .on_authoritative_state = {},
    };
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
        .should_stop = {},
        .is_uncapped_phase = {},
        .input_override = {},
        .start_input_override = {},
        .on_loop = {},
        .on_authoritative_player = {},
        .on_input_sent = {},
        .on_authoritative_state = {},
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
        .should_stop = {},
        .is_uncapped_phase = {},
        .input_override = {},
        .start_input_override = {},
        .on_loop = {},
        .on_authoritative_player = {},
        .on_input_sent = {},
        .on_authoritative_state = {},
    };
    game_client.run(core::Address::localhost(server.port()), '@', &hooks);
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();

    EXPECT_TRUE(stop_processed);
    EXPECT_TRUE(game_client.isConnected());
    EXPECT_FALSE(game_client.accepted());
    EXPECT_EQ(game_client.inputCalls(), 0U);
}

TEST(GameClientLifecycleTest, SameEpochCoverageDropsChunksDisconnectedByAnEvictedBridge)
{
    UnjoinedServer server;
    ServerPump server_pump{ server };
    CoverageClient game_client;
    ASSERT_TRUE(game_client.connect(core::Address::localhost(server.port()), std::chrono::seconds{ 2 }));

    using Clock = CoverageClient::Clock;
    shared::PlayerPosition center{ .x = 32'768, .y = 32'768, .z = 128 };
    Clock::time_point event_time = Clock::now();
    ASSERT_TRUE(game_client.initialize(center, event_time));
    event_time += std::chrono::milliseconds{ 1 };
    ASSERT_TRUE(game_client.applyCoverage(center, event_time));

    std::vector<shared::ChunkCoordinate> coordinates;
    coordinates.reserve(6U);
    int32_t const center_chunk_x = center.x / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH);
    int32_t const center_chunk_y = center.y / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH);
    int32_t const center_chunk_z = center.z / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH);
    for (int32_t offset = -3; offset < 3; ++offset) {
        coordinates.push_back({
            .x = center_chunk_x + offset,
            .y = center_chunk_y,
            .z = center_chunk_z,
        });
    }
    event_time += std::chrono::milliseconds{ 1 };
    ASSERT_TRUE(game_client.applyAirBatch(
        1U,
        std::span<shared::ChunkCoordinate const>{ coordinates.data(), 3U },
        event_time
    ));
    event_time += std::chrono::milliseconds{ 1 };
    ASSERT_TRUE(game_client.applyAirBatch(
        2U,
        std::span<shared::ChunkCoordinate const>{ coordinates.data() + 3U, 3U },
        event_time
    ));
    for (shared::ChunkCoordinate const coordinate : coordinates) {
        EXPECT_TRUE(game_client.activeCoverageContains(coordinate));
    }
    ASSERT_EQ(game_client.worldLoadingStatus().resident_authoritative_columns, 0U);

    ASSERT_TRUE(game_client.evictResidentForCoverageTest(coordinates[3U]));
    center.x += 80;
    event_time += std::chrono::milliseconds{ 1 };
    ASSERT_TRUE(game_client.applyCoverage(center, event_time));

    for (uint32_t index = 0U; index < coordinates.size(); ++index) {
        EXPECT_EQ(game_client.activeCoverageContains(coordinates[index]), index >= 4U)
            << "chunk line offset " << index;
        EXPECT_EQ(game_client.hasResidentChunk(coordinates[index]), index != 3U)
            << "resident chunk line offset " << index;
    }
    EXPECT_EQ(game_client.worldLoadingStatus().resident_authoritative_columns, 0U);
}

TEST(GameClientLifecycleTest, SameEpochCoverageRetainsConnectedChunksAcrossHorizontalWrap)
{
    UnjoinedServer server;
    ServerPump server_pump{ server };
    CoverageClient game_client;
    ASSERT_TRUE(game_client.connect(core::Address::localhost(server.port()), std::chrono::seconds{ 2 }));

    using Clock = CoverageClient::Clock;
    shared::PlayerPosition center{ .x = 0, .y = 32'768, .z = 128 };
    Clock::time_point event_time = Clock::now();
    ASSERT_TRUE(game_client.initialize(center, event_time));
    event_time += std::chrono::milliseconds{ 1 };
    ASSERT_TRUE(game_client.applyCoverage(center, event_time));

    std::array<shared::ChunkCoordinate, 2U> const seam_pair{
        shared::ChunkCoordinate{ .x = 0, .y = 2'048, .z = 8 },
        shared::ChunkCoordinate{ .x = 4'095, .y = 2'048, .z = 8 },
    };
    event_time += std::chrono::milliseconds{ 1 };
    ASSERT_TRUE(game_client.applyAirBatch(1U, seam_pair, event_time));
    for (shared::ChunkCoordinate const coordinate : seam_pair) {
        EXPECT_TRUE(game_client.activeCoverageContains(coordinate));
    }

    center.x = 16;
    event_time += std::chrono::milliseconds{ 1 };
    ASSERT_TRUE(game_client.applyCoverage(center, event_time));
    EXPECT_TRUE(game_client.activeCoverageContains(seam_pair[0]));
    EXPECT_TRUE(game_client.activeCoverageContains(seam_pair[1]));

    center.x = 0;
    event_time += std::chrono::milliseconds{ 1 };
    ASSERT_TRUE(game_client.applyCoverage(center, event_time));
    EXPECT_TRUE(game_client.activeCoverageContains(seam_pair[0]));
    EXPECT_TRUE(game_client.activeCoverageContains(seam_pair[1]));
    EXPECT_EQ(game_client.worldLoadingStatus().resident_authoritative_columns, 0U);
}

TEST(GameClientLifecycleTest, EmptyHighAltitudeCoverageCanBeEnteredAndLeftWithoutResettingTheSession)
{
    UnjoinedServer server;
    ServerPump server_pump{ server };
    CoverageClient game_client;
    ASSERT_TRUE(game_client.connect(core::Address::localhost(server.port()), std::chrono::seconds{ 2 }));

    using Clock = CoverageClient::Clock;
    static constexpr shared::PlayerPosition TERRAIN_EDGE{ .x = 32'768, .y = 32'768, .z = 1'023 };
    static constexpr shared::PlayerPosition ABOVE_TERRAIN{ .x = 32'768, .y = 32'768, .z = 1'400 };
    Clock::time_point event_time = Clock::now();
    ASSERT_TRUE(game_client.initialize(TERRAIN_EDGE, event_time));

    event_time += std::chrono::milliseconds{ 1 };
    ASSERT_TRUE(game_client.applyCoverage(TERRAIN_EDGE, event_time, 1U));
    ASSERT_TRUE(game_client.activeCoverageEpoch().has_value());
    ASSERT_TRUE(game_client.activeCoverageCenter().has_value());
    EXPECT_EQ(*game_client.activeCoverageEpoch(), 1U);
    EXPECT_EQ(game_client.activeCoverageCenter()->z, TERRAIN_EDGE.z);

    event_time += std::chrono::milliseconds{ 1 };
    ASSERT_TRUE(game_client.applyCoverage(ABOVE_TERRAIN, event_time, 2U));
    ASSERT_TRUE(game_client.activeCoverageEpoch().has_value());
    ASSERT_TRUE(game_client.activeCoverageCenter().has_value());
    EXPECT_EQ(*game_client.activeCoverageEpoch(), 2U);
    EXPECT_EQ(game_client.activeCoverageCenter()->z, ABOVE_TERRAIN.z);

    event_time += std::chrono::milliseconds{ 1 };
    shared::PlayerPosition const HIGHER_EMPTY{ .x = 32'768, .y = 32'768, .z = 1'410 };
    ASSERT_TRUE(game_client.applyCoverage(HIGHER_EMPTY, event_time, 2U));
    auto const higher_empty_epoch = game_client.activeCoverageEpoch();
    ASSERT_TRUE(higher_empty_epoch.has_value());
    auto const higher_empty_center = game_client.activeCoverageCenter();
    ASSERT_TRUE(higher_empty_center.has_value());
    EXPECT_EQ(higher_empty_epoch.value(), 2U);
    EXPECT_EQ(higher_empty_center->z, HIGHER_EMPTY.z);

    event_time += std::chrono::milliseconds{ 1 };
    ASSERT_TRUE(game_client.applyCoverage(TERRAIN_EDGE, event_time, 3U));
    ASSERT_TRUE(game_client.activeCoverageEpoch().has_value());
    ASSERT_TRUE(game_client.activeCoverageCenter().has_value());
    EXPECT_EQ(*game_client.activeCoverageEpoch(), 3U);
    EXPECT_EQ(game_client.activeCoverageCenter()->z, TERRAIN_EDGE.z);
}

TEST(GameClientLifecycleTest, FullDetailUsesFortyChunksWhileDensePhysicsKeepsBoundedNearLead)
{
    UnjoinedServer server;
    ServerPump server_pump{ server };
    CoverageClient game_client;
    ASSERT_TRUE(game_client.connect(core::Address::localhost(server.port()), std::chrono::seconds{ 2 }));

    using Clock = CoverageClient::Clock;
    static constexpr shared::PlayerPosition CENTER{ .x = 32'768, .y = 32'768, .z = 512 };
    Clock::time_point event_time = Clock::now();
    ASSERT_TRUE(game_client.initialize(CENTER, event_time));
    event_time += std::chrono::milliseconds{ 1 };
    ASSERT_FALSE(game_client.applyCoverage(
        CENTER,
        event_time,
        1U,
        shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS,
        shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS
    ));
    ASSERT_TRUE(game_client.applyCoverage(CENTER, event_time));

    client::WorldLoadingStatus const status = game_client.worldLoadingStatus();
    EXPECT_EQ(status.actual_chunk_radius_blocks, 640U);
    EXPECT_EQ(status.visual_lod_radius_blocks, 320U);
    EXPECT_EQ(
        status.playable_sphere_required_chunks,
        shared::requiredTerrainColumns(CENTER).size() * 64U
    );

    std::vector<shared::ChunkCoordinate> const actual_keys = shared::requiredChunkKeys(
        CENTER,
        shared::NEAR_TERRAIN_RADIUS_BLOCKS
    );
    std::vector<shared::ChunkCoordinate> const interest_keys = shared::materializedInterestChunkKeys(CENTER);
    auto const lead_key = std::ranges::find_if(interest_keys, [&actual_keys](shared::ChunkCoordinate const key) {
        return std::ranges::find(actual_keys, key) == actual_keys.end();
    });
    ASSERT_NE(lead_key, interest_keys.end());
    EXPECT_TRUE(game_client.isProtectedChunk(*lead_key));

    shared::ChunkCoordinate const visual_only_key{
        .x = 2'068,
        .y = 2'048,
        .z = 32,
    };
    EXPECT_FALSE(game_client.isProtectedChunk(visual_only_key));

    shared::PlayerPosition const moved_center{ .x = 33'024, .y = 32'768, .z = 512 };
    event_time += std::chrono::milliseconds{ 1 };
    ASSERT_TRUE(game_client.applyCoverage(moved_center, event_time, 2U));
    EXPECT_FALSE(game_client.isProtectedChunk(*lead_key));
}

TEST(GameClientLifecycleTest, FullProfilesProvideAllZOnlyAfterCurrentSessionEpochAndConnectedReceipt)
{
    UnjoinedServer server;
    ServerPump server_pump{ server };
    CoverageClient game_client;
    ASSERT_TRUE(game_client.connect(core::Address::localhost(server.port()), std::chrono::seconds{ 2 }));
    auto event_time = CoverageClient::Clock::now();
    shared::PlayerPosition const center{ .x = 32'768, .y = 32'768, .z = 128 };
    shared::ChunkCoordinate const root{ .x = 2'048, .y = 2'048, .z = 0 };
    shared::ChunkCoordinate const next{ .x = 2'049, .y = 2'048, .z = 0 };
    shared::ChunkCoordinate const disconnected{ .x = 2'050, .y = 2'048, .z = 0 };
    ASSERT_TRUE(game_client.initialize(center, event_time));
    event_time += std::chrono::milliseconds{ 1 };
    ASSERT_TRUE(game_client.applyCoverage(center, event_time));
    EXPECT_EQ(game_client.requiredAuthoritativeColumnCount(), 81U * 81U);
    EXPECT_EQ(game_client.residentAuthoritativeColumnCount(), 0U);
    event_time += std::chrono::milliseconds{ 1 };
    EXPECT_FALSE(game_client.applyProfiles({ root }, 1U, 2U, event_time));
    EXPECT_FALSE(game_client.applyProfiles({ root }, 1U, 1U, event_time, true));
    EXPECT_EQ(game_client.residentAuthoritativeColumnCount(), 0U);
    ASSERT_TRUE(game_client.applyProfiles({ root, disconnected }, 1U, 1U, event_time));
    EXPECT_EQ(game_client.residentAuthoritativeColumnCount(), 1U);
    EXPECT_FALSE(game_client.authoritativeWorld()->hasAuthoritativeSurface(disconnected));
    EXPECT_FALSE(game_client.applyProfiles({ next }, 1U, 1U, event_time));
    event_time += std::chrono::milliseconds{ 1 };
    ASSERT_TRUE(game_client.applyProfiles({ next, disconnected }, 2U, 1U, event_time));
    EXPECT_EQ(game_client.residentAuthoritativeColumnCount(), 3U);
    auto const* const world = game_client.authoritativeWorld();
    ASSERT_NE(world, nullptr);
    EXPECT_EQ(world->residentChunk({ root.x, root.y, 0 }), nullptr);
    EXPECT_EQ(world->blockAt({ center.x, center.y, 0 }), shared::Block::Bedrock);
    EXPECT_EQ(world->blockAt({ center.x, center.y, 98 }), shared::Block::Dirt);
    EXPECT_EQ(world->blockAt({ center.x, center.y, 100 }), shared::Block::Grass);
    EXPECT_EQ(world->blockAt({ center.x, center.y, 1'023 }), shared::Block::Air);
    EXPECT_EQ(game_client.worldLoadingStatus().resident_authoritative_columns, 3U);
    EXPECT_FALSE(game_client.worldLoadingStatus().playable_sphere_ready);
}

TEST(GameClientLifecycleTest, StaleEpochBulkReleasesDeliveryDebtWithoutPublishingTerrain)
{
    TerrainReceiptServer server;
    ServerPump server_pump{ server };
    CoverageClient game_client;
    ASSERT_TRUE(game_client.connect(core::Address::localhost(server.port()), std::chrono::seconds{ 2 }));
    auto event_time = CoverageClient::Clock::now();
    shared::PlayerPosition const center{ .x = 32'768, .y = 32'768, .z = 128 };
    std::vector<shared::ChunkCoordinate> const profiles{ { 2'048, 2'048, 0 }, { 2'049, 2'048, 0 } };
    std::array const chunks{ shared::ChunkCoordinate{ 2'048, 2'048, 8 }, shared::ChunkCoordinate{ 2'049, 2'048, 8 } };
    ASSERT_TRUE(game_client.initialize(center, event_time));
    event_time += std::chrono::milliseconds{ 2 };
    ASSERT_TRUE(game_client.applyCoverage(center, event_time, 1U));
    ASSERT_TRUE(game_client.applyCoverage(center, event_time, 2U));

    EXPECT_FALSE(game_client.applyProfiles(profiles, 10U, 1U, event_time));
    EXPECT_FALSE(game_client.applyAirBatch(11U, chunks, event_time, 1U));
    for (uint64_t const invalid_epoch : std::array<uint64_t, 2>{ 0U, 3U }) {
        EXPECT_FALSE(game_client.applyProfiles(profiles, 20U + invalid_epoch, invalid_epoch, event_time));
        EXPECT_FALSE(game_client.applyAirBatch(30U + invalid_epoch, chunks, event_time, invalid_epoch));
    }
    EXPECT_FALSE(game_client.applyProfiles(profiles, 40U, 1U, event_time, true));
    EXPECT_FALSE(game_client.applyAirBatch(41U, chunks, event_time, 1U, true));
    EXPECT_FALSE(game_client.applyProfiles(profiles, 0U, 1U, event_time));
    EXPECT_FALSE(game_client.applyAirBatch(0U, chunks, event_time, 1U));
    EXPECT_FALSE(game_client.applyProfiles({}, 50U, 1U, event_time));
    EXPECT_FALSE(game_client.applyAirBatch(51U, {}, event_time, 1U));
    EXPECT_FALSE(game_client.applyProfiles(profiles, 60U, 1U, event_time));
    game_client.flush();

    ASSERT_TRUE(server.waitForDelivery(60U));
    auto const receipts = server.receipts();
    ASSERT_EQ(receipts.size(), 3U);
    auto const* first = std::get_if<shared::ClientAuthoritativeTerrainAckMessage>(&receipts[0]);
    auto const* dense = std::get_if<shared::ClientChunkDeliveryAckMessage>(&receipts[1]);
    auto const* last = std::get_if<shared::ClientAuthoritativeTerrainAckMessage>(&receipts[2]);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(dense, nullptr);
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(first->session, game_client.session());
    EXPECT_EQ(first->coverage_epoch, 1U);
    EXPECT_EQ(first->delivery_token, 10U);
    EXPECT_EQ(first->not_resident_keys, profiles);
    EXPECT_EQ(dense->session, game_client.session());
    EXPECT_EQ(dense->coverage_epoch, 1U);
    EXPECT_EQ(dense->delivery_token, 11U);
    EXPECT_EQ(dense->credits, 1U);
    EXPECT_EQ(dense->not_resident_keys, (std::vector<shared::ChunkCoordinate>{ chunks.begin(), chunks.end() }));
    EXPECT_EQ(last->coverage_epoch, 1U);
    EXPECT_EQ(last->delivery_token, 60U);
    EXPECT_EQ(game_client.residentAuthoritativeColumnCount(), 0U);
    for (auto const coordinate : profiles) {
        EXPECT_FALSE(game_client.authoritativeWorld()->hasAuthoritativeSurface(coordinate));
    }
    for (auto const coordinate : chunks) {
        EXPECT_FALSE(game_client.hasResidentChunk(coordinate));
        EXPECT_FALSE(game_client.activeCoverageContains(coordinate));
    }
    ASSERT_TRUE(game_client.applyProfiles(profiles, 61U, 2U, event_time));
    EXPECT_EQ(game_client.residentAuthoritativeColumnCount(), 2U);
}

TEST(GameClientLifecycleTest, NewCoverageEpochRetiresEveryPendingDenseBatchBeforeDiscardingIt)
{
    TerrainReceiptServer server;
    ServerPump server_pump{ server };
    CoverageClient game_client;
    ASSERT_TRUE(game_client.connect(core::Address::localhost(server.port()), std::chrono::seconds{ 2 }));
    auto event_time = CoverageClient::Clock::now();
    shared::PlayerPosition const center{ .x = 32'768, .y = 32'768, .z = 128 };
    shared::ChunkCoordinate const root{ 2'048, 2'048, 8 };
    shared::ChunkCoordinate const disconnected{ 2'050, 2'048, 8 };
    shared::ChunkCoordinate const outside{ 2'148, 2'048, 8 };
    shared::ChunkCoordinate const other_disconnected{ 2'048, 2'050, 8 };
    ASSERT_TRUE(game_client.initialize(center, event_time));
    event_time += std::chrono::milliseconds{ 2 };
    ASSERT_TRUE(game_client.applyCoverage(center, event_time));
    ASSERT_TRUE(game_client.applyAirBatch(71U, std::array{ root, disconnected, outside }, event_time));
    ASSERT_TRUE(game_client.applyAirBatch(72U, std::array{ other_disconnected }, event_time));
    ASSERT_TRUE(game_client.hasResidentChunk(root));
    ASSERT_FALSE(game_client.hasResidentChunk(disconnected));
    ASSERT_FALSE(game_client.hasResidentChunk(other_disconnected));

    ASSERT_TRUE(game_client.applyCoverage(center, event_time, 2U));
    game_client.flush();

    ASSERT_TRUE(server.waitForDelivery(71U));
    ASSERT_TRUE(server.waitForDelivery(72U));
    auto const receipts = server.receipts();
    ASSERT_EQ(receipts.size(), 2U);
    for (auto const& receipt : receipts) {
        auto const* dense = std::get_if<shared::ClientChunkDeliveryAckMessage>(&receipt);
        ASSERT_NE(dense, nullptr);
        EXPECT_EQ(dense->session, game_client.session());
        EXPECT_EQ(dense->coverage_epoch, 1U);
        EXPECT_EQ(dense->credits, 1U);
        if (dense->delivery_token == 71U) {
            EXPECT_EQ(dense->not_resident_keys.size(), 2U);
            EXPECT_EQ(std::ranges::count(dense->not_resident_keys, disconnected), 1);
            EXPECT_EQ(std::ranges::count(dense->not_resident_keys, outside), 1);
        } else {
            EXPECT_EQ(dense->delivery_token, 72U);
            EXPECT_EQ(dense->not_resident_keys, (std::vector<shared::ChunkCoordinate>{ other_disconnected }));
        }
    }
    EXPECT_EQ(game_client.activeCoverageEpoch(), 2U);
    EXPECT_FALSE(game_client.hasResidentChunk(disconnected));
    EXPECT_FALSE(game_client.hasResidentChunk(other_disconnected));
    ASSERT_TRUE(game_client.applyAirBatch(73U, std::array{ root }, event_time, 2U));
    game_client.flush();
    ASSERT_TRUE(server.waitForDelivery(73U));
    auto const final_receipts = server.receipts();
    ASSERT_EQ(final_receipts.size(), 3U);
    auto const& current = std::get<shared::ClientChunkDeliveryAckMessage>(final_receipts.back());
    EXPECT_EQ(current.coverage_epoch, 2U);
    EXPECT_EQ(current.delivery_token, 73U);
    EXPECT_TRUE(current.not_resident_keys.empty());
}
