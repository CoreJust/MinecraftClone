#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace testsupport {

enum class AdverseDirection : uint8_t {
    ClientToServer,
    ServerToClient,
};

struct AdverseTransportConfig final {
    uint32_t base_latency_ticks = 0U;
    uint32_t jitter_ticks = 0U;
    uint32_t loss_per_mille = 0U;
    uint32_t duplicate_per_mille = 0U;
    uint32_t reorder_delay_ticks = 0U;
    uint32_t freeze_begin_tick = 0U;
    uint32_t freeze_duration_ticks = 0U;
    uint32_t impairment_end_tick = std::numeric_limits<uint32_t>::max();
    uint32_t recovery_burst_per_tick = 1U;
    uint32_t max_deliveries_per_tick = 1U;
    uint32_t max_queue_packets = 64U;
    uint32_t max_packet_bytes = 64U * 1'024U;
    uint32_t max_schedule_records = 256U;
};

struct AdversePacket final {
    uint64_t ordinal = 0U;
    uint32_t sent_tick = 0U;
    uint32_t due_tick = 0U;
    std::vector<uint8_t> bytes;
};

struct AdverseScheduleEntry final {
    uint64_t ordinal = 0U;
    uint32_t sent_tick = 0U;
    uint32_t due_tick = 0U;
    uint32_t bytes = 0U;
    bool duplicated = false;
    bool impaired = true;
    bool freeze_delayed = false;
    bool reorder_delayed = false;
};

struct AdverseTransportFacts final {
    uint64_t send_attempts = 0U;
    uint64_t queued_packets = 0U;
    uint64_t delivered_packets = 0U;
    uint64_t delivered_bytes = 0U;
    uint64_t lost_packets = 0U;
    uint64_t duplicated_packets = 0U;
    uint64_t reordered_packets = 0U;
    uint64_t freeze_delayed_packets = 0U;
    uint64_t queue_overflow_packets = 0U;
    uint64_t oversized_packets = 0U;
    uint64_t max_queue_packets = 0U;
    uint64_t max_delivery_burst = 0U;
    uint64_t schedule_records_dropped = 0U;
    std::vector<AdverseScheduleEntry> schedule;
};

class AdverseTransport final {
public:
    AdverseTransport(
        uint64_t seed,
        AdverseTransportConfig client_to_server,
        AdverseTransportConfig server_to_client
    );

    [[nodiscard]] bool send(
        AdverseDirection direction,
        uint32_t tick,
        std::span<uint8_t const> bytes
    );

    [[nodiscard]] std::vector<AdversePacket> receive(
        AdverseDirection direction,
        uint32_t tick
    );

    [[nodiscard]] AdverseTransportFacts const& facts(AdverseDirection direction) const noexcept;
    [[nodiscard]] AdverseTransportConfig const& configuration(AdverseDirection direction) const noexcept;
    [[nodiscard]] uint64_t seed() const noexcept { return m_seed; }
    [[nodiscard]] uint32_t pending(AdverseDirection direction) const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::string factsJson() const;

private:
    struct DirectionState final {
        AdverseTransportConfig configuration;
        AdverseTransportFacts facts;
        std::vector<AdversePacket> queue;
        uint64_t next_ordinal = 0U;
        uint64_t last_delivered_ordinal = 0U;
        bool has_last_delivered_ordinal = false;
    };

    [[nodiscard]] DirectionState& state(AdverseDirection direction) noexcept;
    [[nodiscard]] DirectionState const& state(AdverseDirection direction) const noexcept;
    [[nodiscard]] uint64_t randomValue(AdverseDirection direction, uint64_t ordinal) const noexcept;
    static void appendConfigurationJson(std::string& output, AdverseTransportConfig const& configuration);
    static void appendFactsJson(std::string& output, AdverseTransportFacts const& facts);

    uint64_t m_seed;
    DirectionState m_client_to_server;
    DirectionState m_server_to_client;
};

} // namespace testsupport
