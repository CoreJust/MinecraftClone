#include <client/GameClient.hpp>
#include <server/GameServer.hpp>
#include <shared/net/Message.hpp>
#include <shared/world/World.hpp>

#include <core/net/Client.hpp>
#include <core/net/Server.hpp>

#include <gtest/gtest.h>
#include <testsupport/AdverseTransport.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {

using testsupport::AdverseDirection;
using testsupport::AdversePacket;
using testsupport::AdverseTransport;
using testsupport::AdverseTransportConfig;

[[nodiscard]] std::optional<shared::Message> decode(testsupport::AdversePacket const& packet)
{
    return shared::decodeMessage(packet.bytes);
}

class AdverseGameClient final : public client::GameClient {
public:
    AdverseGameClient()
        : GameClient{ shared::WorldMode::Flight, shared::World::canonicalConfiguration(), false }
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

    [[nodiscard]] std::optional<shared::Player> authoritativePlayer(char const character) const noexcept
    {
        return m_world.playerByCharacter(character);
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

    [[nodiscard]] uint32_t stateRevision(char const character) const noexcept
    {
        auto const found = m_state_revisions.find(character);
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

private:
    shared::Direction input() override { return {}; }
    void onConnectionStateReset() override
    {
        m_was_joined = false;
        GameClient::onConnectionStateReset();
    }

    void render() override
    {
        bool const joined = m_accepted && m_world.playerByCharacter(m_local_character).has_value();
        if (joined && !m_was_joined) {
            m_joined_count.fetch_add(1U, std::memory_order_release);
        }
        m_was_joined = joined;
    }

    std::atomic_uint32_t m_joined_count{ 0U };
    bool m_was_joined = false;
};

struct ClientNetworkFacts final {
    std::atomic_uint64_t applied_positions{ 0U };
};

struct AdverseRelayReceipt final {
    char character = 0;
    uint64_t seed = 0U;
    std::string transport_json;
    testsupport::AdverseTransportConfig client_to_server_configuration;
    testsupport::AdverseTransportConfig server_to_client_configuration;
    testsupport::AdverseTransportFacts client_to_server_facts;
    testsupport::AdverseTransportFacts server_to_client_facts;
    std::map<char, shared::ServerPlayerPositionMessage> latest_server_positions;
    uint64_t forwarded_inputs = 0U;
    uint64_t stale_inputs_forwarded = 0U;
    uint64_t stale_probe_attempts = 0U;
    uint64_t stale_probe_deliveries = 0U;
    uint32_t stale_probe_sequence = 0U;
    uint32_t stale_probe_baseline_revision = 0U;
    bool stale_probe_queued = false;
    uint64_t stale_probe_authority_acceptances = 0U;
    bool queues_empty_at_stop = false;
    uint64_t relay_send_failures = 0U;
};

class AdverseNetworkRelay final : public core::Server {
public:
    static constexpr uint64_t BASE_SEED = 0x0247'2026'0000ULL;
    static constexpr std::chrono::milliseconds LOGICAL_TICK{ 10 };
    static constexpr uint32_t STALE_PROBE_TICK = 350U;

    explicit AdverseNetworkRelay(uint16_t const game_server_port, uint32_t const initial_tick)
        : core::Server{ core::Address::localhost(0U), 4U, 2U }
        , m_game_server_port{ game_server_port }
        , m_tick{ initial_tick }
    { }

    void run(std::atomic_bool const& stop_requested)
    {
        auto next_tick = std::chrono::steady_clock::now();
        uint32_t tick = m_tick;
        while (!stop_requested.load(std::memory_order_acquire)) {
            static_cast<void>(poll(std::chrono::milliseconds{ 1 }));
            for (auto& [client_id, link] : m_links) {
                static_cast<void>(client_id);
                if (link->upstream->isConnected()) {
                    static_cast<void>(link->upstream->poll(std::chrono::milliseconds::zero()));
                }
            }
            auto const now = std::chrono::steady_clock::now();
            while (now >= next_tick) {
                processTick(tick++);
                next_tick += LOGICAL_TICK;
            }
        }
    }

    [[nodiscard]] uint16_t port() const noexcept { return core::Server::port(); }

    [[nodiscard]] uint64_t upstreamConnectionFailures() const noexcept
    {
        return m_upstream_connection_failures;
    }

    [[nodiscard]] std::optional<AdverseRelayReceipt> receipt(char const character) const
    {
        for (auto const& [client_id, link] : m_links) {
            static_cast<void>(client_id);
            if (link->character != character || !link->transport) {
                continue;
            }
            AdverseRelayReceipt receipt{
                .character = link->character,
                .seed = link->transport->seed(),
                .transport_json = link->transport->factsJson(),
                .client_to_server_configuration = link->transport->configuration(
                    AdverseDirection::ClientToServer
                ),
                .server_to_client_configuration = link->transport->configuration(
                    AdverseDirection::ServerToClient
                ),
                .client_to_server_facts = link->transport->facts(AdverseDirection::ClientToServer),
                .server_to_client_facts = link->transport->facts(AdverseDirection::ServerToClient),
                .latest_server_positions = link->latest_server_positions,
                .forwarded_inputs = link->forwarded_inputs,
                .stale_inputs_forwarded = link->stale_inputs_forwarded,
                .stale_probe_attempts = link->stale_probe.attempts,
                .stale_probe_deliveries = link->stale_probe.deliveries,
                .stale_probe_sequence = link->stale_probe.sequence,
                .stale_probe_baseline_revision = link->stale_probe.baseline_revision,
                .stale_probe_queued = link->stale_probe.queued,
                .stale_probe_authority_acceptances = link->stale_probe.authority_acceptances,
                .queues_empty_at_stop = link->transport->empty(),
                .relay_send_failures = link->relay_send_failures,
            };
            return receipt;
        }
        return std::nullopt;
    }

private:
    class UpstreamClient final : public core::Client {
    public:
        UpstreamClient(AdverseNetworkRelay& relay, core::ClientId const downstream_id)
            : core::Client{ 2U }
            , m_relay{ relay }
            , m_downstream_id{ downstream_id }
        { }

    private:
        void onDisconnected(core::DisconnectEvent const) override
        {
            m_relay.onUpstreamDisconnected(m_downstream_id);
        }

        void onReceived(core::ReceiveEvent event) override
        {
            m_relay.onUpstreamReceived(m_downstream_id, std::move(event));
        }

        AdverseNetworkRelay& m_relay;
        core::ClientId m_downstream_id;
    };

    struct StaleProbe final {
        bool requested = false;
        bool queued = false;
        uint32_t sequence = 0U;
        uint32_t baseline_revision = 0U;
        uint64_t attempts = 0U;
        uint64_t deliveries = 0U;
        uint64_t authority_acceptances = 0U;
    };

    struct Link final {
        explicit Link(core::ClientId const downstream_id)
            : id{ downstream_id }
        { }

        core::ClientId id;
        uint32_t connection_start_tick = 0U;
        char character = 0;
        std::unique_ptr<UpstreamClient> upstream;
        std::unique_ptr<AdverseTransport> transport;
        std::map<char, shared::ServerPlayerPositionMessage> latest_server_positions;
        uint32_t latest_forwarded_input_sequence = 0U;
        bool has_forwarded_input = false;
        uint64_t forwarded_inputs = 0U;
        uint64_t stale_inputs_forwarded = 0U;
        StaleProbe stale_probe;
        uint64_t relay_send_failures = 0U;
    };

    void onConnected(core::ServerConnectEvent const event) override
    {
        auto link = std::make_unique<Link>(event.client_id);
        link->upstream = std::make_unique<UpstreamClient>(*this, event.client_id);
        if (!link->upstream->connect(
                core::Address::localhost(m_game_server_port),
                std::chrono::seconds{ 2 }
            )) {
            ++m_upstream_connection_failures;
            static_cast<void>(kick(event.client_id));
            return;
        }
        m_links.emplace(event.client_id, std::move(link));
    }

    void onDisconnected(core::ServerDisconnectEvent const event) override
    {
        m_links.erase(event.client_id);
    }

    void onReceived(core::ServerReceiveEvent event) override
    {
        auto const found = m_links.find(event.client_id);
        if (found == m_links.end()) {
            return;
        }
        Link& link = *found->second;
        if (event.channel_id == shared::GAME_CHANNEL) {
            std::optional<shared::Message> const decoded = shared::decodeMessage(event.data);
            if (!decoded.has_value()) {
                ++link.relay_send_failures;
                return;
            }
            if (auto const* const join = std::get_if<shared::JoinRequestMessage>(&*decoded)) {
                link.character = join->ch;
                link.connection_start_tick = m_tick;
                link.transport = std::make_unique<AdverseTransport>(
                    seedForCharacter(join->ch),
                    CLIENT_TO_SERVER,
                    SERVER_TO_CLIENT
                );
            } else if (link.transport) {
                if (auto const* const input = std::get_if<shared::ClientInputMessage>(&*decoded)) {
                    static_cast<void>(link.transport->send(
                        AdverseDirection::ClientToServer,
                        tickSinceJoin(link, m_tick),
                        event.data
                    ));
                    static_cast<void>(input);
                    return;
                }
            }
        }
        if (!link.upstream->send(event.data, event.channel_id, RELIABLE)) {
            ++link.relay_send_failures;
        }
    }

    void onUpstreamDisconnected(core::ClientId const downstream_id)
    {
        auto const found = m_links.find(downstream_id);
        if (found != m_links.end()) {
            ++found->second->relay_send_failures;
        }
    }

    void onUpstreamReceived(core::ClientId const downstream_id, core::ReceiveEvent event)
    {
        auto const found = m_links.find(downstream_id);
        if (found == m_links.end()) {
            return;
        }
        Link& link = *found->second;
        if (event.channel_id == shared::GAME_CHANNEL && link.transport) {
            std::optional<shared::Message> const decoded = shared::decodeMessage(event.data);
            if (decoded.has_value()) {
                if (auto const* const position = std::get_if<shared::ServerPlayerPositionMessage>(&*decoded)) {
                    auto const previous = link.latest_server_positions.find(position->ch);
                    if (previous == link.latest_server_positions.end()
                        || shared::isNewerSequence(position->state_revision, previous->second.state_revision)) {
                        link.latest_server_positions.insert_or_assign(position->ch, *position);
                    }
                    if (link.stale_probe.queued
                        && position->ch == link.character
                        && position->acknowledged_input_sequence == link.stale_probe.sequence
                        && shared::isNewerSequence(
                            position->state_revision,
                            link.stale_probe.baseline_revision
                        )) {
                        ++link.stale_probe.authority_acceptances;
                    }
                    static_cast<void>(link.transport->send(
                        AdverseDirection::ServerToClient,
                        tickSinceJoin(link, m_tick),
                        event.data
                    ));
                    return;
                }
            }
        }
        if (!sendToDownstream(link, event.data, event.channel_id)) {
            ++link.relay_send_failures;
        }
    }

    [[nodiscard]] bool sendToDownstream(
        Link const& link,
        std::span<uint8_t const> const bytes,
        uint8_t const channel_id
    )
    {
        std::optional<core::Peer> const peer = client(link.id);
        return peer.has_value() && core::Server::send(peer, bytes, channel_id, RELIABLE);
    }

    void processTick(uint32_t const tick)
    {
        m_tick = tick;
        for (auto& [client_id, link_pointer] : m_links) {
            static_cast<void>(client_id);
            Link& link = *link_pointer;
            if (!link.transport) {
                continue;
            }
            uint32_t const link_tick = tickSinceJoin(link, tick);
            requestStaleProbe(link, link_tick);
            for (AdversePacket const& packet : link.transport->receive(
                AdverseDirection::ClientToServer,
                link_tick
            )) {
                std::optional<shared::Message> const decoded = shared::decodeMessage(packet.bytes);
                auto const* const input = decoded.has_value()
                    ? std::get_if<shared::ClientInputMessage>(&*decoded)
                    : nullptr;
                if (input == nullptr) {
                    ++link.relay_send_failures;
                    continue;
                }
                ++link.forwarded_inputs;
                if (link.has_forwarded_input
                    && !shared::isNewerSequence(input->sequence, link.latest_forwarded_input_sequence)) {
                    ++link.stale_inputs_forwarded;
                } else {
                    link.latest_forwarded_input_sequence = input->sequence;
                    link.has_forwarded_input = true;
                }
                if (link.stale_probe.queued && input->sequence == link.stale_probe.sequence) {
                    ++link.stale_probe.deliveries;
                }
                if (!link.upstream->send(packet.bytes, shared::GAME_CHANNEL, RELIABLE)) {
                    ++link.relay_send_failures;
                }
            }
            for (AdversePacket const& packet : link.transport->receive(
                AdverseDirection::ServerToClient,
                link_tick
            )) {
                if (!sendToDownstream(link, packet.bytes, shared::GAME_CHANNEL)) {
                    ++link.relay_send_failures;
                }
            }
        }
    }

    static void requestStaleProbe(Link& link, uint32_t const tick)
    {
        if (!link.stale_probe.requested && tick >= STALE_PROBE_TICK) {
            auto const authority = link.latest_server_positions.find(link.character);
            if (authority != link.latest_server_positions.end()
                && authority->second.acknowledged_input_sequence != 0U) {
                link.stale_probe.requested = true;
                link.stale_probe.sequence = authority->second.acknowledged_input_sequence;
                link.stale_probe.baseline_revision = authority->second.state_revision;
            }
        }
        if (!link.stale_probe.requested || link.stale_probe.queued) {
            return;
        }
        ++link.stale_probe.attempts;
        shared::Message const stale_input = shared::ClientInputMessage{
            .direction = {},
            .sequence = link.stale_probe.sequence,
        };
        if (link.transport->send(
                AdverseDirection::ClientToServer,
                tick,
                shared::encodeMessage(stale_input)
            )) {
            link.stale_probe.queued = true;
        }
    }

    [[nodiscard]] static uint32_t tickSinceJoin(Link const& link, uint32_t const tick) noexcept
    {
        return tick - link.connection_start_tick;
    }

    [[nodiscard]] static uint64_t seedForCharacter(char const character) noexcept
    {
        return BASE_SEED ^ (
            static_cast<uint64_t>(static_cast<uint8_t>(character)) * 0x9e37'79b9'7f4a'7c15ULL
        );
    }

    static constexpr core::SendMode RELIABLE{ core::SendMode::Reliable };
    static constexpr AdverseTransportConfig CLIENT_TO_SERVER{
        .base_latency_ticks = 1U,
        .jitter_ticks = 5U,
        .loss_per_mille = 120U,
        .duplicate_per_mille = 360U,
        .reorder_delay_ticks = 14U,
        .freeze_begin_tick = 0U,
        .freeze_duration_ticks = 100U,
        .impairment_end_tick = 450U,
        .recovery_burst_per_tick = 4U,
        .max_deliveries_per_tick = 8U,
        .max_queue_packets = 64U,
        .max_packet_bytes = 1'024U,
        .max_schedule_records = 2'048U,
    };
    static constexpr AdverseTransportConfig SERVER_TO_CLIENT{
        .base_latency_ticks = 2U,
        .jitter_ticks = 6U,
        .loss_per_mille = 160U,
        .duplicate_per_mille = 360U,
        .reorder_delay_ticks = 16U,
        // Leave time for client-to-server freeze recovery and its maximum delivery delay.
        .freeze_begin_tick = 150U,
        .freeze_duration_ticks = 100U,
        .impairment_end_tick = 450U,
        .recovery_burst_per_tick = 4U,
        .max_deliveries_per_tick = 8U,
        .max_queue_packets = 64U,
        .max_packet_bytes = 1'024U,
        .max_schedule_records = 2'048U,
    };

    uint16_t m_game_server_port;
    uint32_t m_tick = 0U;
    uint64_t m_upstream_connection_failures = 0U;
    std::unordered_map<core::ClientId, std::unique_ptr<Link>> m_links;
};

TEST(AdverseNetworkTest, SyntheticTransportCoversLossLatencyJitterFreezeReorderingAndDuplicationDeterministically)
{
    static constexpr uint32_t INPUT_COUNT = 32U;
    static constexpr uint32_t SETTLE_TICK = 64U;
    static constexpr uint64_t SEED = 0x0247'2026ULL;
    static constexpr AdverseTransportConfig CONFIG{
        .base_latency_ticks = 3U,
        .jitter_ticks = 4U,
        .loss_per_mille = 180U,
        .duplicate_per_mille = 280U,
        .reorder_delay_ticks = 2U,
        .freeze_begin_tick = 9U,
        .freeze_duration_ticks = 6U,
        .recovery_burst_per_tick = 3U,
        .max_deliveries_per_tick = 8U,
        .max_queue_packets = 64U,
        .max_packet_bytes = 256U,
        .max_schedule_records = 128U,
    };
    AdverseTransport link{ SEED, CONFIG, CONFIG };
    std::vector<uint32_t> delivered_sequences;

    for (uint32_t tick = 0U; tick < INPUT_COUNT; ++tick) {
        shared::Message const message = shared::ClientInputMessage{
            .direction = { .x = 127U, .y = 0U, .view_y = 127 },
            .sequence = tick + 1U,
        };
        static_cast<void>(link.send(
            AdverseDirection::ClientToServer,
            tick,
            shared::encodeMessage(message)
        ));
        std::vector<testsupport::AdversePacket> const received = link.receive(
            AdverseDirection::ClientToServer,
            tick
        );
        if (tick >= CONFIG.freeze_begin_tick
            && tick < CONFIG.freeze_begin_tick + CONFIG.freeze_duration_ticks) {
            EXPECT_TRUE(received.empty());
        }
        for (testsupport::AdversePacket const& packet : received) {
            std::optional<shared::Message> const received = decode(packet);
            ASSERT_TRUE(received.has_value());
            ASSERT_TRUE(std::holds_alternative<shared::ClientInputMessage>(*received));
            delivered_sequences.push_back(std::get<shared::ClientInputMessage>(*received).sequence);
        }
    }
    for (uint32_t tick = INPUT_COUNT; tick <= SETTLE_TICK; ++tick) {
        for (testsupport::AdversePacket const& packet : link.receive(AdverseDirection::ClientToServer, tick)) {
            std::optional<shared::Message> const received = decode(packet);
            ASSERT_TRUE(received.has_value());
            ASSERT_TRUE(std::holds_alternative<shared::ClientInputMessage>(*received));
            delivered_sequences.push_back(std::get<shared::ClientInputMessage>(*received).sequence);
        }
    }

    testsupport::AdverseTransportFacts const& counters = link.facts(AdverseDirection::ClientToServer);
    EXPECT_EQ(counters.send_attempts, INPUT_COUNT);
    EXPECT_GT(counters.lost_packets, 0U);
    EXPECT_GT(counters.duplicated_packets, 0U);
    EXPECT_GT(counters.reordered_packets, 0U);
    EXPECT_GT(counters.freeze_delayed_packets, 0U);
    EXPECT_EQ(counters.delivered_packets, delivered_sequences.size());
    EXPECT_LE(counters.max_queue_packets, CONFIG.max_queue_packets);
    EXPECT_LE(counters.max_delivery_burst, CONFIG.max_deliveries_per_tick);
    EXPECT_TRUE(link.empty());
    EXPECT_FALSE(delivered_sequences.empty());

    AdverseTransport replay{ SEED, CONFIG, CONFIG };
    for (uint32_t tick = 0U; tick < INPUT_COUNT; ++tick) {
        shared::Message const message = shared::ClientInputMessage{
            .direction = { .x = 127U, .y = 0U, .view_y = 127 },
            .sequence = tick + 1U,
        };
        static_cast<void>(replay.send(
            AdverseDirection::ClientToServer,
            tick,
            shared::encodeMessage(message)
        ));
        static_cast<void>(replay.receive(AdverseDirection::ClientToServer, tick));
    }
    for (uint32_t tick = INPUT_COUNT; tick <= SETTLE_TICK; ++tick) {
        static_cast<void>(replay.receive(AdverseDirection::ClientToServer, tick));
    }
    EXPECT_EQ(link.factsJson(), replay.factsJson());

    std::string const facts = link.factsJson();
    EXPECT_NE(facts.find("\"schema_version\": \"mc.adverse-network.v1\""), std::string::npos);
    EXPECT_NE(facts.find("\"seed\": " + std::to_string(SEED)), std::string::npos);
    EXPECT_NE(facts.find("\"client_to_server\""), std::string::npos);
    EXPECT_NE(facts.find("\"schedule\""), std::string::npos);
}

TEST(AdverseNetworkTest, DeliversPacketsAfterFiveSecondsOfLatency)
{
    static constexpr uint64_t SEED = 0x0247'0500ULL;
    static constexpr AdverseTransportConfig LATENCY_CONFIG{
        .base_latency_ticks = 5'000U,
        .max_deliveries_per_tick = 1U,
        .max_queue_packets = 1U,
        .max_packet_bytes = 256U,
        .max_schedule_records = 1U,
    };
    std::vector<uint8_t> const bytes = shared::encodeMessage(shared::ClientInputMessage{
        .direction = { .x = 127U },
        .sequence = 1U,
    });
    AdverseTransport latency{ SEED, LATENCY_CONFIG, AdverseTransportConfig{} };

    ASSERT_TRUE(latency.send(AdverseDirection::ClientToServer, 0U, bytes));
    EXPECT_TRUE(latency.receive(AdverseDirection::ClientToServer, 4'999U).empty());
    std::vector<testsupport::AdversePacket> const delayed = latency.receive(
        AdverseDirection::ClientToServer,
        5'000U
    );
    ASSERT_EQ(delayed.size(), 1U);
    EXPECT_EQ(delayed.front().bytes, bytes);
}

TEST(AdverseNetworkTest, RecoversPacketsAfterFifteenSecondDeliveryFreeze)
{
    static constexpr uint64_t SEED = 0x0247'1500ULL;
    static constexpr AdverseTransportConfig FREEZE_CONFIG{
        .freeze_begin_tick = 0U,
        .freeze_duration_ticks = 15'000U,
        .max_deliveries_per_tick = 1U,
        .max_queue_packets = 1U,
        .max_packet_bytes = 256U,
        .max_schedule_records = 1U,
    };
    std::vector<uint8_t> const bytes = shared::encodeMessage(shared::ClientInputMessage{
        .direction = { .x = 127U },
        .sequence = 1U,
    });
    AdverseTransport freeze{ SEED, FREEZE_CONFIG, AdverseTransportConfig{} };

    ASSERT_TRUE(freeze.send(AdverseDirection::ClientToServer, 0U, bytes));
    EXPECT_TRUE(freeze.receive(AdverseDirection::ClientToServer, 14'999U).empty());
    std::vector<testsupport::AdversePacket> const recovered = freeze.receive(
        AdverseDirection::ClientToServer,
        15'000U
    );
    ASSERT_EQ(recovered.size(), 1U);
    EXPECT_EQ(recovered.front().bytes, bytes);
}

TEST(AdverseNetworkTest, ProductionTwoClientsConvergeAcrossSeededBidirectionalImpairment)
{
    static constexpr uint32_t INITIAL_RELAY_TICK = 500U;
    static constexpr std::chrono::seconds MOVEMENT_DURATION{ 5 };
    static constexpr std::chrono::seconds RUN_DURATION{ 7 };
    static constexpr shared::Direction MOVEMENT{ .x = 127U, .y = 0U, .view_y = 127 };

    server::GameServer game_server{ 0U, {}, shared::WorldMode::Flight };
    AdverseNetworkRelay relay{ game_server.port(), INITIAL_RELAY_TICK };
    std::atomic_bool stop_server{ false };
    std::atomic_bool stop_relay{ false };
    std::thread server_thread{ [&] {
        game_server.run(stop_server);
    } };
    std::thread relay_thread{ [&] {
        relay.run(stop_relay);
    } };

    AdverseGameClient first_client;
    AdverseGameClient second_client;
    auto const scenario_start = std::chrono::steady_clock::now();
    auto const movement_end = scenario_start + MOVEMENT_DURATION;
    auto const deadline = scenario_start + RUN_DURATION;
    ClientNetworkFacts first_facts;
    ClientNetworkFacts second_facts;
    auto const make_hooks = [movement_end, deadline](ClientNetworkFacts& facts) {
        client::GameClientBenchmarkHooks hooks;
        hooks.deadline = deadline;
        hooks.input_override = [movement_end](uint64_t) -> std::optional<shared::Direction> {
            if (std::chrono::steady_clock::now() < movement_end) {
                return MOVEMENT;
            }
            return shared::Direction{};
        };
        hooks.on_authoritative_player = [&facts](shared::Player const&) {
            facts.applied_positions.fetch_add(1U, std::memory_order_relaxed);
        };
        return hooks;
    };
    client::GameClientBenchmarkHooks first_hooks = make_hooks(first_facts);
    client::GameClientBenchmarkHooks second_hooks = make_hooks(second_facts);

    std::thread first_client_thread{ [&] {
        first_client.run(core::Address::localhost(relay.port()), '@', &first_hooks);
    } };
    std::thread second_client_thread{ [&] {
        second_client.run(core::Address::localhost(relay.port()), '#', &second_hooks);
    } };

    first_client_thread.join();
    second_client_thread.join();
    auto const drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 2 };
    while (std::chrono::steady_clock::now() < drain_deadline) {
        static_cast<void>(first_client.poll(std::chrono::milliseconds{ 1 }));
        static_cast<void>(second_client.poll(std::chrono::milliseconds{ 1 }));
    }
    stop_relay.store(true, std::memory_order_release);
    relay_thread.join();
    stop_server.store(true, std::memory_order_release);
    server_thread.join();

    std::optional<AdverseRelayReceipt> const first_receipt = relay.receipt('@');
    std::optional<AdverseRelayReceipt> const second_receipt = relay.receipt('#');
    ASSERT_TRUE(first_receipt.has_value());
    ASSERT_TRUE(second_receipt.has_value());
    EXPECT_EQ(relay.upstreamConnectionFailures(), 0U);

    auto const checkTransport = [](
        AdverseTransportConfig const& configuration,
        testsupport::AdverseTransportFacts const& facts
    ) {
        EXPECT_GT(facts.send_attempts, 0U);
        EXPECT_GT(facts.delivered_packets, 0U);
        EXPECT_GT(facts.lost_packets, 0U);
        EXPECT_GT(facts.duplicated_packets, 0U);
        EXPECT_GT(facts.reordered_packets, 0U);
        EXPECT_EQ(facts.queue_overflow_packets, 0U);
        EXPECT_EQ(facts.oversized_packets, 0U);
        EXPECT_EQ(facts.schedule_records_dropped, 0U);
        EXPECT_LE(facts.max_queue_packets, configuration.max_queue_packets);
        EXPECT_LE(facts.max_delivery_burst, configuration.max_deliveries_per_tick);
        EXPECT_FALSE(facts.schedule.empty());
        bool saw_recovery_traffic = false;
        for (testsupport::AdverseScheduleEntry const& entry : facts.schedule) {
            saw_recovery_traffic = saw_recovery_traffic || !entry.impaired;
        }
        EXPECT_TRUE(saw_recovery_traffic);
    };
    auto const checkClient = [&checkTransport](
        AdverseRelayReceipt const& receipt,
        AdverseGameClient& game_client,
        ClientNetworkFacts const& network_facts,
        char const remote_character,
        uint64_t& stale_snapshot_rejections
    ) {
        checkTransport(
            receipt.client_to_server_configuration,
            receipt.client_to_server_facts
        );
        checkTransport(
            receipt.server_to_client_configuration,
            receipt.server_to_client_facts
        );
        EXPECT_GT(receipt.forwarded_inputs, 10U);
        EXPECT_GT(receipt.stale_inputs_forwarded, 0U);
        EXPECT_TRUE(receipt.stale_probe_queued);
        EXPECT_GT(receipt.stale_probe_attempts, 0U);
        EXPECT_GT(receipt.stale_probe_deliveries, 0U);
        EXPECT_EQ(receipt.stale_probe_authority_acceptances, 0U);
        EXPECT_TRUE(receipt.queues_empty_at_stop);
        EXPECT_EQ(receipt.relay_send_failures, 0U);

        uint64_t const applied = network_facts.applied_positions.load(std::memory_order_relaxed);
        EXPECT_GT(applied, 0U);

        bool positions_match_authority = true;
        for (char const character : std::array{ '@', '#' }) {
            auto const server_position = receipt.latest_server_positions.find(character);
            std::optional<shared::Player> const client_player = game_client.authoritativePlayer(character);
            if (server_position == receipt.latest_server_positions.end() || !client_player.has_value()) {
                ADD_FAILURE() << "Missing authoritative or client position for " << character;
                positions_match_authority = false;
                continue;
            }
            EXPECT_EQ(client_player->x, server_position->second.x) << character;
            EXPECT_EQ(client_player->y, server_position->second.y) << character;
            EXPECT_EQ(client_player->z, server_position->second.z) << character;
            EXPECT_EQ(client_player->x_subcell, server_position->second.x_subcell) << character;
            EXPECT_EQ(client_player->y_subcell, server_position->second.y_subcell) << character;
            EXPECT_EQ(client_player->z_subcell, server_position->second.z_subcell) << character;
            positions_match_authority = positions_match_authority
                && client_player->x == server_position->second.x
                && client_player->y == server_position->second.y
                && client_player->z == server_position->second.z
                && client_player->x_subcell == server_position->second.x_subcell
                && client_player->y_subcell == server_position->second.y_subcell
                && client_player->z_subcell == server_position->second.z_subcell;
        }

        stale_snapshot_rejections = 0U;
        for (char const character : std::array{ '@', '#' }) {
            auto const server_position = receipt.latest_server_positions.find(character);
            std::optional<shared::Player> const before = game_client.authoritativePlayer(character);
            uint32_t const revision = game_client.stateRevision(character);
            if (server_position == receipt.latest_server_positions.end()
                || !before.has_value()
                || revision <= 1U) {
                ADD_FAILURE() << "Cannot probe stale snapshot for " << character;
                continue;
            }

            shared::ServerPlayerPositionMessage stale = server_position->second;
            stale.state_revision = revision - 1U;
            bool const accepted = game_client.applyServerPosition(stale);
            EXPECT_FALSE(accepted) << character;
            EXPECT_EQ(game_client.stateRevision(character), revision) << character;
            if (!accepted) {
                ++stale_snapshot_rejections;
            }

            std::optional<shared::Player> const after = game_client.authoritativePlayer(character);
            if (!after.has_value()) {
                ADD_FAILURE() << "Missing authoritative player after stale snapshot probe " << character;
                positions_match_authority = false;
                continue;
            }
            EXPECT_EQ(after->x, before->x) << character;
            EXPECT_EQ(after->y, before->y) << character;
            EXPECT_EQ(after->z, before->z) << character;
            EXPECT_EQ(after->x_subcell, before->x_subcell) << character;
            EXPECT_EQ(after->y_subcell, before->y_subcell) << character;
            EXPECT_EQ(after->z_subcell, before->z_subcell) << character;
        }
        EXPECT_EQ(stale_snapshot_rejections, 2U);

        std::optional<shared::Player> const own_player = game_client.authoritativePlayer();
        if (!own_player.has_value()) {
            ADD_FAILURE() << "Missing own authoritative player " << receipt.character;
            return false;
        }
        EXPECT_NE(own_player->x, 0);
        EXPECT_NE(remote_character, receipt.character);
        return positions_match_authority;
    };

    uint64_t first_stale_snapshot_rejections = 0U;
    uint64_t second_stale_snapshot_rejections = 0U;
    bool const first_converged = checkClient(
        *first_receipt,
        first_client,
        first_facts,
        '#',
        first_stale_snapshot_rejections
    );
    bool const second_converged = checkClient(
        *second_receipt,
        second_client,
        second_facts,
        '@',
        second_stale_snapshot_rejections
    );
    uint64_t const client_to_server_freeze_delayed_packets =
        first_receipt->client_to_server_facts.freeze_delayed_packets
        + second_receipt->client_to_server_facts.freeze_delayed_packets;
    uint64_t const server_to_client_freeze_delayed_packets =
        first_receipt->server_to_client_facts.freeze_delayed_packets
        + second_receipt->server_to_client_facts.freeze_delayed_packets;
    EXPECT_GT(client_to_server_freeze_delayed_packets, 0U)
        << "Neither client exercised the client-to-server freeze";
    EXPECT_GT(server_to_client_freeze_delayed_packets, 0U)
        << "Neither client exercised the server-to-client freeze";
    EXPECT_TRUE(first_converged);
    EXPECT_TRUE(second_converged);
    std::optional<shared::Player> const first_authoritative_at = first_client.authoritativePlayer('@');
    std::optional<shared::Player> const second_authoritative_at = second_client.authoritativePlayer('@');
    std::optional<shared::Player> const first_authoritative_hash = first_client.authoritativePlayer('#');
    std::optional<shared::Player> const second_authoritative_hash = second_client.authoritativePlayer('#');
    ASSERT_TRUE(first_authoritative_at.has_value());
    ASSERT_TRUE(second_authoritative_at.has_value());
    ASSERT_TRUE(first_authoritative_hash.has_value());
    ASSERT_TRUE(second_authoritative_hash.has_value());
    EXPECT_EQ(
        first_authoritative_at->x,
        second_authoritative_at->x
    );
    EXPECT_EQ(
        first_authoritative_hash->x,
        second_authoritative_hash->x
    );

    auto appendNumber = [](std::string& output, uint64_t const value) {
        output += std::to_string(value);
    };
    auto appendClientFacts = [&appendNumber](
        std::string& output,
        AdverseRelayReceipt const& receipt,
        ClientNetworkFacts const& client_facts,
        AdverseGameClient const& game_client,
        uint64_t const stale_snapshot_rejections,
        bool const positions_match_authority
    ) {
        uint64_t const applied = client_facts.applied_positions.load(std::memory_order_relaxed);
        output += "{\"character\":\"";
        output.push_back(receipt.character);
        output += "\",\"seed\":";
        appendNumber(output, receipt.seed);
        output += ",\"transport\":";
        output += receipt.transport_json;
        output += ",\"authority\":{\"latest_positions\":[";
        bool first_position = true;
        for (auto const& [character, position] : receipt.latest_server_positions) {
            if (!first_position) {
                output += ",";
            }
            first_position = false;
            output += "{\"character\":\"";
            output.push_back(character);
            output += "\",\"x\":";
            output += std::to_string(position.x);
            output += ",\"y\":";
            output += std::to_string(position.y);
            output += ",\"z\":";
            output += std::to_string(position.z);
            output += ",\"state_revision\":";
            appendNumber(output, position.state_revision);
            output += ",\"acknowledged_input_sequence\":";
            appendNumber(output, position.acknowledged_input_sequence);
            output += "}";
        }
        output += "],\"stale_input_packets_forwarded\":";
        appendNumber(output, receipt.stale_inputs_forwarded);
        output += ",\"stale_probe\":{\"queued\":";
        output += receipt.stale_probe_queued ? "true" : "false";
        output += ",\"attempts\":";
        appendNumber(output, receipt.stale_probe_attempts);
        output += ",\"sequence\":";
        appendNumber(output, receipt.stale_probe_sequence);
        output += ",\"baseline_revision\":";
        appendNumber(output, receipt.stale_probe_baseline_revision);
        output += ",\"delivered_copies\":";
        appendNumber(output, receipt.stale_probe_deliveries);
        output += ",\"transport_queues_empty_after_drain\":";
        output += receipt.queues_empty_at_stop ? "true" : "false";
        output += ",\"authority_acceptances\":";
        appendNumber(output, receipt.stale_probe_authority_acceptances);
        output += ",\"rejected_by_authority\":";
        output += receipt.stale_probe_queued
                && receipt.stale_probe_deliveries > 0U
                && receipt.stale_probe_authority_acceptances == 0U
            ? "true" : "false";
        output += "}},\"client\":{\"applied_position_messages\":";
        appendNumber(output, applied);
        output += ",\"stale_snapshot_rejections\":";
        appendNumber(output, stale_snapshot_rejections);
        output += ",\"stale_snapshot_probe\":\"direct_stale_revision\"";
        output += ",\"final_state_revisions\":{\"@\":";
        appendNumber(output, game_client.stateRevision('@'));
        output += ",\"#\":";
        appendNumber(output, game_client.stateRevision('#'));
        output += "}},\"convergence\":{\"positions_match_authority\":";
        output += positions_match_authority ? "true" : "false";
        output += "}}";
    };

    std::string facts_json = "{\"schema_version\":\"mc.acceptance.adverse-network.v1\",\"base_seed\":";
    appendNumber(facts_json, AdverseNetworkRelay::BASE_SEED);
    facts_json += ",\"logical_tick_ms\":";
    appendNumber(facts_json, AdverseNetworkRelay::LOGICAL_TICK.count());
    facts_json += ",\"clients\":[";
    appendClientFacts(
        facts_json,
        *first_receipt,
        first_facts,
        first_client,
        first_stale_snapshot_rejections,
        first_converged
    );
    facts_json += ",";
    appendClientFacts(
        facts_json,
        *second_receipt,
        second_facts,
        second_client,
        second_stale_snapshot_rejections,
        second_converged
    );
    facts_json += "]}";
    testing::Test::RecordProperty("adverse_network_facts", facts_json);
}

TEST(AdverseNetworkTest, ProductionRunRetriesAfterRemoteServerReplacement)
{
    static constexpr std::chrono::seconds INITIAL_JOIN_DEADLINE{ 5 };
    static constexpr std::chrono::seconds RECONNECT_DEADLINE{ 25 };
    auto first_server = std::make_unique<server::GameServer>(0, std::vector<server::GameServer::SpawnPoint>{ },
        shared::WorldMode::Flight);
    uint16_t const port = first_server->port();
    std::atomic_bool stop_first{ false };
    std::thread first_server_thread{ [&first_server, &stop_first] {
        first_server->run(stop_first);
    } };

    AdverseGameClient client;
    std::atomic_bool stop_client{ false };
    client::GameClientBenchmarkHooks client_hooks;
    client_hooks.deadline = std::chrono::steady_clock::now()
        + INITIAL_JOIN_DEADLINE + RECONNECT_DEADLINE + std::chrono::seconds{ 2 };
    client_hooks.should_stop = [&stop_client] {
        return stop_client.load(std::memory_order_acquire);
    };
    std::thread client_thread{ [&client, port, &client_hooks] {
        client.run(core::Address::localhost(port), '@', &client_hooks);
    } };
    auto const waitForJoinedCount = [&client](
        uint32_t const expected,
        std::chrono::seconds const timeout
    ) {
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (client.joinedCount() >= expected) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 10 });
        }
        return client.joinedCount() >= expected;
    };

    bool const initially_joined = waitForJoinedCount(1U, INITIAL_JOIN_DEADLINE);
    stop_first.store(true, std::memory_order_release);
    first_server_thread.join();
    first_server.reset();
    if (!initially_joined) {
        stop_client.store(true, std::memory_order_release);
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
    bool const rejoined = waitForJoinedCount(2U, RECONNECT_DEADLINE);
    stop_client.store(true, std::memory_order_release);
    client_thread.join();
    stop_replacement.store(true, std::memory_order_release);
    replacement_server_thread.join();

    EXPECT_TRUE(rejoined);
    EXPECT_GE(client.joinedCount(), 2U);
}

TEST(AdverseNetworkTest, ProductionRunStopsAtDeadlineWhileDisconnected)
{
    AdverseGameClient client;
    client::GameClientBenchmarkHooks hooks;
    hooks.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{ 100 };
    auto const started_at = std::chrono::steady_clock::now();

    client.run(core::Address::localhost(0), '@', &hooks);

    auto const elapsed = std::chrono::steady_clock::now() - started_at;
    EXPECT_FALSE(client.isConnected());
    EXPECT_LT(elapsed, std::chrono::seconds{ 2 });
}

} // namespace
