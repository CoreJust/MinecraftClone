#include <testsupport/AdverseTransport.hpp>

#include <algorithm>
#include <limits>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace testsupport {
namespace {

constexpr uint64_t CLIENT_TO_SERVER_SALT = 0x5a17'c9e3'11d4'8b27ULL;
constexpr uint64_t SERVER_TO_CLIENT_SALT = 0xa4e2'713b'9c05'6df1ULL;
constexpr uint64_t ORDINAL_SALT = 0x9e37'79b9'7f4a'7c15ULL;
constexpr uint32_t PER_MILLE = 1'000U;

[[nodiscard]] uint64_t splitMix64(uint64_t value) noexcept
{
    value += ORDINAL_SALT;
    value = (value ^ (value >> 30U)) * 0xbf58'476d'1ce4'e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d0'49bb'1331'11ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] uint32_t saturatingAdd(uint32_t const first, uint64_t const second) noexcept
{
    uint64_t const result = static_cast<uint64_t>(first) + second;
    return result > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(result);
}

void appendNumber(std::string& output, uint64_t const value)
{
    output += std::to_string(value);
}

void appendBoolean(std::string& output, bool const value)
{
    output += value ? "true" : "false";
}

} // namespace

AdverseTransport::AdverseTransport(
    uint64_t const seed,
    AdverseTransportConfig client_to_server,
    AdverseTransportConfig server_to_client
)
    : m_seed{ seed }
    , m_client_to_server{ .configuration = client_to_server }
    , m_server_to_client{ .configuration = server_to_client }
{
    m_client_to_server.queue.reserve(client_to_server.max_queue_packets);
    m_server_to_client.queue.reserve(server_to_client.max_queue_packets);
    m_client_to_server.facts.schedule.reserve(client_to_server.max_schedule_records);
    m_server_to_client.facts.schedule.reserve(server_to_client.max_schedule_records);
}

bool AdverseTransport::send(
    AdverseDirection const direction,
    uint32_t const tick,
    std::span<uint8_t const> const bytes
)
{
    DirectionState& direction_state = state(direction);
    AdverseTransportConfig const& configuration = direction_state.configuration;
    AdverseTransportFacts& facts = direction_state.facts;
    ++facts.send_attempts;

    if (bytes.size() > configuration.max_packet_bytes) {
        ++facts.oversized_packets;
        return false;
    }

    uint64_t const ordinal = direction_state.next_ordinal++;
    auto const record_schedule = [&configuration, &facts](AdverseScheduleEntry entry) {
        if (facts.schedule.size() < configuration.max_schedule_records) {
            facts.schedule.push_back(entry);
        } else {
            ++facts.schedule_records_dropped;
        }
    };
    uint64_t const random = randomValue(direction, ordinal);
    if (tick >= configuration.impairment_end_tick) {
        if (direction_state.queue.size() >= configuration.max_queue_packets) {
            ++facts.queue_overflow_packets;
            return false;
        }
        direction_state.queue.push_back(AdversePacket{
            .ordinal = ordinal,
            .sent_tick = tick,
            .due_tick = tick,
            .bytes = { bytes.begin(), bytes.end() },
        });
        ++facts.queued_packets;
        facts.max_queue_packets = std::max<uint64_t>(facts.max_queue_packets, direction_state.queue.size());
        record_schedule({
            .ordinal = ordinal,
            .sent_tick = tick,
            .due_tick = tick,
            .bytes = static_cast<uint32_t>(bytes.size()),
            .duplicated = false,
            .impaired = false,
            .freeze_delayed = false,
            .reorder_delayed = false,
        });
        return true;
    }

    uint32_t const loss_per_mille = std::min(configuration.loss_per_mille, PER_MILLE);
    if (random % PER_MILLE < loss_per_mille) {
        ++facts.lost_packets;
        return false;
    }
    if (direction_state.queue.size() >= configuration.max_queue_packets) {
        ++facts.queue_overflow_packets;
        return false;
    }

    uint32_t jitter = 0U;
    if (configuration.jitter_ticks != 0U) {
        jitter = static_cast<uint32_t>((random >> 16U) % (static_cast<uint64_t>(configuration.jitter_ticks) + 1U));
    }
    bool const reorder_delayed = configuration.reorder_delay_ticks != 0U && (ordinal % 2U == 0U);
    uint32_t due_tick = saturatingAdd(tick, configuration.base_latency_ticks);
    due_tick = saturatingAdd(due_tick, jitter);
    if (reorder_delayed) {
        due_tick = saturatingAdd(due_tick, configuration.reorder_delay_ticks);
    }

    uint32_t const freeze_end_tick = saturatingAdd(
        configuration.freeze_begin_tick,
        configuration.freeze_duration_ticks
    );
    bool const freeze_delayed = configuration.freeze_duration_ticks != 0U
        && tick >= configuration.freeze_begin_tick
        && tick < freeze_end_tick;
    if (freeze_delayed) {
        due_tick = std::max(due_tick, freeze_end_tick);
        ++facts.freeze_delayed_packets;
    }

    AdversePacket packet{
        .ordinal = ordinal,
        .sent_tick = tick,
        .due_tick = due_tick,
        .bytes = { bytes.begin(), bytes.end() },
    };
    direction_state.queue.push_back(packet);
    ++facts.queued_packets;
    facts.max_queue_packets = std::max<uint64_t>(facts.max_queue_packets, direction_state.queue.size());
    record_schedule({
        .ordinal = ordinal,
        .sent_tick = tick,
        .due_tick = due_tick,
        .bytes = static_cast<uint32_t>(bytes.size()),
        .duplicated = false,
        .impaired = true,
        .freeze_delayed = freeze_delayed,
        .reorder_delayed = reorder_delayed,
    });

    uint32_t const duplicate_per_mille = std::min(configuration.duplicate_per_mille, PER_MILLE);
    if (randomValue(direction, ordinal ^ 0x7f4a'7c15ULL) % PER_MILLE >= duplicate_per_mille) {
        return true;
    }
    ++facts.duplicated_packets;
    if (direction_state.queue.size() >= configuration.max_queue_packets) {
        ++facts.queue_overflow_packets;
        return true;
    }
    direction_state.queue.push_back(packet);
    ++facts.queued_packets;
    facts.max_queue_packets = std::max<uint64_t>(facts.max_queue_packets, direction_state.queue.size());
    record_schedule({
        .ordinal = ordinal,
        .sent_tick = tick,
        .due_tick = due_tick,
        .bytes = static_cast<uint32_t>(bytes.size()),
        .duplicated = true,
        .impaired = true,
        .freeze_delayed = freeze_delayed,
        .reorder_delayed = reorder_delayed,
    });
    return true;
}

std::vector<AdversePacket> AdverseTransport::receive(
    AdverseDirection const direction,
    uint32_t const tick
)
{
    DirectionState& direction_state = state(direction);
    AdverseTransportConfig const& configuration = direction_state.configuration;
    AdverseTransportFacts& facts = direction_state.facts;
    uint32_t const freeze_end_tick = saturatingAdd(
        configuration.freeze_begin_tick,
        configuration.freeze_duration_ticks
    );
    bool const frozen = configuration.freeze_duration_ticks != 0U
        && tick < configuration.impairment_end_tick
        && tick >= configuration.freeze_begin_tick
        && tick < freeze_end_tick;
    if (frozen) {
        return {};
    }
    std::ranges::sort(direction_state.queue, [](AdversePacket const& first, AdversePacket const& second) {
        if (first.due_tick != second.due_tick) {
            return first.due_tick < second.due_tick;
        }
        return first.ordinal < second.ordinal;
    });

    uint32_t delivery_limit = std::max(configuration.max_deliveries_per_tick, 1U);
    if (configuration.freeze_duration_ticks != 0U) {
        if (tick >= freeze_end_tick) {
            delivery_limit = std::min(
                delivery_limit,
                std::max(configuration.recovery_burst_per_tick, 1U)
            );
        }
    }

    std::vector<AdversePacket> delivered;
    uint32_t const delivery_capacity = std::min(
        static_cast<uint32_t>(direction_state.queue.size()),
        delivery_limit
    );
    delivered.reserve(delivery_capacity);
    while (!direction_state.queue.empty()
        && direction_state.queue.front().due_tick <= tick
        && delivered.size() < delivery_limit) {
        AdversePacket packet = std::move(direction_state.queue.front());
        direction_state.queue.erase(direction_state.queue.begin());
        if (direction_state.has_last_delivered_ordinal
            && packet.ordinal < direction_state.last_delivered_ordinal) {
            ++facts.reordered_packets;
        }
        direction_state.last_delivered_ordinal = packet.ordinal;
        direction_state.has_last_delivered_ordinal = true;
        facts.delivered_bytes += packet.bytes.size();
        delivered.push_back(std::move(packet));
    }
    facts.delivered_packets += delivered.size();
    facts.max_delivery_burst = std::max<uint64_t>(
        facts.max_delivery_burst,
        static_cast<uint64_t>(delivered.size())
    );
    return delivered;
}

AdverseTransportFacts const& AdverseTransport::facts(AdverseDirection const direction) const noexcept
{
    return state(direction).facts;
}

AdverseTransportConfig const& AdverseTransport::configuration(AdverseDirection const direction) const noexcept
{
    return state(direction).configuration;
}

uint32_t AdverseTransport::pending(AdverseDirection const direction) const noexcept
{
    return static_cast<uint32_t>(state(direction).queue.size());
}

bool AdverseTransport::empty() const noexcept
{
    return m_client_to_server.queue.empty() && m_server_to_client.queue.empty();
}

std::string AdverseTransport::factsJson() const
{
    std::string output;
    output.reserve(2'048U);
    output += "{\n  \"schema_version\": \"mc.adverse-network.v1\",\n  \"model\": "
        "\"bounded_application_datagram\",\n  \"seed\": ";
    appendNumber(output, m_seed);
    output += ",\n  \"directions\": {\n    \"client_to_server\": {\n      \"configuration\": ";
    appendConfigurationJson(output, m_client_to_server.configuration);
    output += ",\n      \"facts\": ";
    appendFactsJson(output, m_client_to_server.facts);
    output += "\n    },\n    \"server_to_client\": {\n      \"configuration\": ";
    appendConfigurationJson(output, m_server_to_client.configuration);
    output += ",\n      \"facts\": ";
    appendFactsJson(output, m_server_to_client.facts);
    output += "\n    }\n  }\n}\n";
    return output;
}

AdverseTransport::DirectionState& AdverseTransport::state(AdverseDirection const direction) noexcept
{
    return direction == AdverseDirection::ClientToServer ? m_client_to_server : m_server_to_client;
}

AdverseTransport::DirectionState const& AdverseTransport::state(AdverseDirection const direction) const noexcept
{
    return direction == AdverseDirection::ClientToServer ? m_client_to_server : m_server_to_client;
}

uint64_t AdverseTransport::randomValue(
    AdverseDirection const direction,
    uint64_t const ordinal
) const noexcept
{
    uint64_t const salt = direction == AdverseDirection::ClientToServer
        ? CLIENT_TO_SERVER_SALT
        : SERVER_TO_CLIENT_SALT;
    return splitMix64(m_seed ^ salt ^ (ordinal * ORDINAL_SALT));
}

void AdverseTransport::appendConfigurationJson(
    std::string& output,
    AdverseTransportConfig const& configuration
)
{
    output += "{\"base_latency_ticks\": ";
    appendNumber(output, configuration.base_latency_ticks);
    output += ", \"jitter_ticks\": ";
    appendNumber(output, configuration.jitter_ticks);
    output += ", \"loss_per_mille\": ";
    appendNumber(output, configuration.loss_per_mille);
    output += ", \"duplicate_per_mille\": ";
    appendNumber(output, configuration.duplicate_per_mille);
    output += ", \"reorder_delay_ticks\": ";
    appendNumber(output, configuration.reorder_delay_ticks);
    output += ", \"freeze_begin_tick\": ";
    appendNumber(output, configuration.freeze_begin_tick);
    output += ", \"freeze_duration_ticks\": ";
    appendNumber(output, configuration.freeze_duration_ticks);
    output += ", \"impairment_end_tick\": ";
    appendNumber(output, configuration.impairment_end_tick);
    output += ", \"recovery_burst_per_tick\": ";
    appendNumber(output, configuration.recovery_burst_per_tick);
    output += ", \"max_deliveries_per_tick\": ";
    appendNumber(output, configuration.max_deliveries_per_tick);
    output += ", \"max_queue_packets\": ";
    appendNumber(output, configuration.max_queue_packets);
    output += ", \"max_packet_bytes\": ";
    appendNumber(output, configuration.max_packet_bytes);
    output += ", \"max_schedule_records\": ";
    appendNumber(output, configuration.max_schedule_records);
    output += "}";
}

void AdverseTransport::appendFactsJson(std::string& output, AdverseTransportFacts const& facts)
{
    output += "{\"send_attempts\": ";
    appendNumber(output, facts.send_attempts);
    output += ", \"queued_packets\": ";
    appendNumber(output, facts.queued_packets);
    output += ", \"delivered_packets\": ";
    appendNumber(output, facts.delivered_packets);
    output += ", \"delivered_bytes\": ";
    appendNumber(output, facts.delivered_bytes);
    output += ", \"lost_packets\": ";
    appendNumber(output, facts.lost_packets);
    output += ", \"duplicated_packets\": ";
    appendNumber(output, facts.duplicated_packets);
    output += ", \"reordered_packets\": ";
    appendNumber(output, facts.reordered_packets);
    output += ", \"freeze_delayed_packets\": ";
    appendNumber(output, facts.freeze_delayed_packets);
    output += ", \"queue_overflow_packets\": ";
    appendNumber(output, facts.queue_overflow_packets);
    output += ", \"oversized_packets\": ";
    appendNumber(output, facts.oversized_packets);
    output += ", \"max_queue_packets\": ";
    appendNumber(output, facts.max_queue_packets);
    output += ", \"max_delivery_burst\": ";
    appendNumber(output, facts.max_delivery_burst);
    output += ", \"schedule_records_dropped\": ";
    appendNumber(output, facts.schedule_records_dropped);
    output += ", \"schedule\": [";
    uint32_t const schedule_size = static_cast<uint32_t>(facts.schedule.size());
    for (uint32_t index = 0U; index < schedule_size; ++index) {
        if (index != 0U) {
            output += ", ";
        }
        AdverseScheduleEntry const& entry = facts.schedule[index];
        output += "{\"ordinal\": ";
        appendNumber(output, entry.ordinal);
        output += ", \"sent_tick\": ";
        appendNumber(output, entry.sent_tick);
        output += ", \"due_tick\": ";
        appendNumber(output, entry.due_tick);
        output += ", \"bytes\": ";
        appendNumber(output, entry.bytes);
        output += ", \"duplicated\": ";
        appendBoolean(output, entry.duplicated);
        output += ", \"impaired\": ";
        appendBoolean(output, entry.impaired);
        output += ", \"freeze_delayed\": ";
        appendBoolean(output, entry.freeze_delayed);
        output += ", \"reorder_delayed\": ";
        appendBoolean(output, entry.reorder_delayed);
        output += "}";
    }
    output += "]}";
}

} // namespace testsupport
