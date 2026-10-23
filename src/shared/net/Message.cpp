#include <shared/net/Message.hpp>

#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldCoverage.hpp>

#include <generation/Generation.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace shared {

namespace {

enum class MessageType : uint8_t {
    JoinRequest,
    JoinResponse,
    ClientInput,
    ClientHeightTileCredit,
    ServerPlayerPosition,
    ServerRemovePlayer,
    ServerHeightTileDescriptor,
    ServerWorldRevision,
    ServerHeightTile,
    ServerRemoveHeightTile,
    ServerHeightTileBatch,
    ServerWorldPreparation = 11,
    ServerMaterializedChunkBatch = 12,
    ClientChunkDeliveryAck = 13,
    ClientStartWorld = 14,
    ServerStartWorldAck = 15,
    ServerChunkCoverage = 16,
    ServerAuthoritativeTerrainBatch = 17,
    ClientAuthoritativeTerrainAck = 18,
};

[[nodiscard]]
constexpr bool isValidCharacter(char const ch) noexcept
{
    return ch == '@' || ch == '#' || ch == '$' || ch == '%' || ch == '&';
}

[[nodiscard]]
constexpr bool isValidDirection(uint8_t const value) noexcept
{
    return value != 128;
}

[[nodiscard]]
constexpr bool isValidMode(WorldMode const mode) noexcept
{
    return mode == WorldMode::Flat || mode == WorldMode::Flight;
}

void appendUint16(std::vector<uint8_t>& bytes, uint16_t const value)
{
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8U));
}

void appendUint32(std::vector<uint8_t>& bytes, uint32_t const value)
{
    for (uint32_t byte{ 0 }; byte < 4; ++byte) {
        bytes.push_back(static_cast<uint8_t>(value >> (byte * 8U)));
    }
}

void appendUint64(std::vector<uint8_t>& bytes, uint64_t const value)
{
    for (uint32_t byte{ 0 }; byte < 8; ++byte) {
        bytes.push_back(static_cast<uint8_t>(value >> (byte * 8U)));
    }
}

void appendInt32(std::vector<uint8_t>& bytes, int32_t const value)
{
    appendUint32(bytes, static_cast<uint32_t>(value));
}

void appendChunkCoordinate(std::vector<uint8_t>& bytes, ChunkCoordinate const coordinate)
{
    appendInt32(bytes, coordinate.x);
    appendInt32(bytes, coordinate.y);
    appendInt32(bytes, coordinate.z);
}

void appendPlayerPosition(std::vector<uint8_t>& bytes, PlayerPosition const& position)
{
    appendInt32(bytes, position.x);
    appendInt32(bytes, position.y);
    appendInt32(bytes, position.z);
    appendUint16(bytes, position.x_subcell);
    appendUint16(bytes, position.y_subcell);
    appendUint16(bytes, position.z_subcell);
}

void appendGenerationConfiguration(
    std::vector<uint8_t>& bytes,
    generation::GenerationConfiguration const& configuration
)
{
    appendUint64(bytes, configuration.seed);
    appendUint32(bytes, configuration.resolution);
    appendUint32(bytes, configuration.epochs);
    appendUint32(bytes, configuration.erosion_steps);
    appendUint32(bytes, configuration.initial_climate_iterations);
    appendUint32(bytes, configuration.climate_iterations);
    bytes.push_back(static_cast<uint8_t>(configuration.currents));
    bytes.push_back(static_cast<uint8_t>(configuration.wind_sign));
    bytes.push_back(static_cast<uint8_t>(configuration.coastal_elaboration));
}

[[nodiscard]]
uint64_t terrainSessionEncodedSize(TerrainSessionIdentity const& session) noexcept
{
    return 8U + 22U + session.terrain.configuration_identity.size() + 31U;
}

[[nodiscard]]
uint64_t generationSnapshotEncodedSize(generation::GenerationSnapshot const& snapshot) noexcept
{
    uint64_t size = 5U;
    auto const add_field_size = [&size](uint64_t const field_size) {
        size += 4U + field_size;
    };
    add_field_size(snapshot.elevation.size() * sizeof(float));
    add_field_size(snapshot.temperature.size() * sizeof(float));
    add_field_size(snapshot.precipitation.size() * sizeof(float));
    add_field_size(snapshot.water_level.size() * sizeof(float));
    add_field_size(snapshot.ocean_mask.size());
    add_field_size(snapshot.lake_mask.size());
    add_field_size(snapshot.river_mask.size());
    return size;
}

[[nodiscard]]
uint64_t worldPreparationEncodedSize(ServerWorldPreparationMessage const& message) noexcept
{
    uint64_t size = terrainSessionEncodedSize(message.session) + 8U + 1U + 4U + 4U + 8U + 1U;
    if (message.latest_snapshot) {
        size += generationSnapshotEncodedSize(*message.latest_snapshot);
    }
    size += 1U;
    if (message.refined_safe_spawn) {
        size += 18U;
    }
    size += 2U + message.failure.size();
    return size;
}

void appendFloatVector(std::vector<uint8_t>& bytes, std::vector<float> const& values)
{
    appendUint32(bytes, static_cast<uint32_t>(values.size()));
    for (float const value : values) {
        appendUint32(bytes, std::bit_cast<uint32_t>(value));
    }
}

void appendByteVector(std::vector<uint8_t>& bytes, std::vector<uint8_t> const& values)
{
    appendUint32(bytes, static_cast<uint32_t>(values.size()));
    bytes.insert(bytes.end(), values.begin(), values.end());
}

void appendGenerationSnapshot(std::vector<uint8_t>& bytes, generation::GenerationSnapshot const& snapshot)
{
    bytes.push_back(static_cast<uint8_t>(snapshot.stage));
    appendUint32(bytes, snapshot.resolution);
    appendFloatVector(bytes, snapshot.elevation);
    appendFloatVector(bytes, snapshot.temperature);
    appendFloatVector(bytes, snapshot.precipitation);
    appendFloatVector(bytes, snapshot.water_level);
    appendByteVector(bytes, snapshot.ocean_mask);
    appendByteVector(bytes, snapshot.lake_mask);
    appendByteVector(bytes, snapshot.river_mask);
}

void appendBoundedString(std::vector<uint8_t>& bytes, std::string const& value)
{
    appendUint16(bytes, static_cast<uint16_t>(value.size()));
    for (char const character : value) {
        bytes.push_back(static_cast<uint8_t>(static_cast<unsigned char>(character)));
    }
}

[[nodiscard]]
uint16_t heightTileBase(ServerHeightTileMessage const& message) noexcept
{
    return *std::ranges::min_element(message.heights);
}

[[nodiscard]]
uint8_t heightTileDeltaBitWidth(ServerHeightTileMessage const& message) noexcept
{
    uint32_t const height_range = static_cast<uint32_t>(*std::ranges::max_element(message.heights))
        - static_cast<uint32_t>(heightTileBase(message));
    uint8_t bit_width = 0U;
    for (uint32_t range = height_range; range != 0U; range >>= 1U) {
        ++bit_width;
    }
    return bit_width;
}

[[nodiscard]]
uint32_t surfaceTileEncodedSize(SurfaceTile const& tile) noexcept
{
    return 8U + 22U + static_cast<uint32_t>(tile.identity.configuration_identity.size())
        + HEIGHT_TILE_SAMPLE_COUNT * HEIGHT_TILE_SURFACE_COLUMN_BYTES;
}

[[nodiscard]]
uint32_t heightTileEncodedSize(ServerHeightTileMessage const& message) noexcept
{
    uint32_t const packed_bits = HEIGHT_TILE_SAMPLE_COUNT
        * static_cast<uint32_t>(heightTileDeltaBitWidth(message));
    return 27U + (packed_bits + 7U) / 8U + surfaceTileEncodedSize(message.surface);
}

void appendTerrainIdentity(std::vector<uint8_t>& bytes, TerrainIdentity const& identity)
{
    appendUint32(bytes, identity.terrain_version);
    appendUint64(bytes, identity.revision);
    appendUint64(bytes, identity.seed);
    appendUint16(bytes, static_cast<uint16_t>(identity.configuration_identity.size()));
    for (char const character : identity.configuration_identity) {
        bytes.push_back(static_cast<uint8_t>(static_cast<unsigned char>(character)));
    }
}

void appendTerrainSession(std::vector<uint8_t>& bytes, TerrainSessionIdentity const& session)
{
    appendUint64(bytes, session.join_epoch);
    appendTerrainIdentity(bytes, session.terrain);
    appendGenerationConfiguration(bytes, session.configuration);
}

void appendSurfaceTile(std::vector<uint8_t>& bytes, SurfaceTile const& tile)
{
    appendUint32(bytes, tile.block_sea_level);
    appendUint32(bytes, tile.max_height);
    appendTerrainIdentity(bytes, tile.identity);
    for (TerrainColumn const& column : tile.columns) {
        appendUint16(bytes, column.highest_solid_z);
        appendUint16(bytes, column.water_height_z);
        bytes.push_back(static_cast<uint8_t>(column.water_present));
        bytes.push_back(static_cast<uint8_t>(column.submerged));
        appendUint64(bytes, std::bit_cast<uint64_t>(column.geological_elevation_km));
        appendUint64(bytes, std::bit_cast<uint64_t>(column.temperature_celsius));
        appendUint64(bytes, std::bit_cast<uint64_t>(column.precipitation));
        bytes.push_back(static_cast<uint8_t>(column.top_material));
        bytes.push_back(static_cast<uint8_t>(column.ground_material));
        appendUint16(bytes, column.ground_first_z);
        appendUint16(bytes, column.ground_end_z);
    }
}

void appendHeightTile(std::vector<uint8_t>& bytes, ServerHeightTileMessage const& message)
{
    appendInt32(bytes, message.key.x);
    appendInt32(bytes, message.key.y);
    appendUint64(bytes, message.revision);
    appendUint64(bytes, message.token);

    uint16_t const base = heightTileBase(message);
    uint8_t const bit_width = heightTileDeltaBitWidth(message);
    bytes.push_back(bit_width);
    appendUint16(bytes, base);

    uint64_t bit_buffer = 0U;
    uint32_t buffered_bits = 0U;
    for (uint16_t const height : message.heights) {
        uint32_t const delta = static_cast<uint32_t>(height) - static_cast<uint32_t>(base);
        bit_buffer |= static_cast<uint64_t>(delta) << buffered_bits;
        buffered_bits += bit_width;
        while (buffered_bits >= 8U) {
            bytes.push_back(static_cast<uint8_t>(bit_buffer));
            bit_buffer >>= 8U;
            buffered_bits -= 8U;
        }
    }
    appendSurfaceTile(bytes, message.surface);
}

void appendHeightTileRemoval(std::vector<uint8_t>& bytes, ServerRemoveHeightTileMessage const& message)
{
    appendInt32(bytes, message.key.x);
    appendInt32(bytes, message.key.y);
    appendUint64(bytes, message.revision);
    appendUint64(bytes, message.token);
}

struct MessageEncoder final {
    std::vector<uint8_t> bytes;

    void begin(MessageType const type, uint64_t const payload_size)
    {
        if (payload_size > MAX_MESSAGE_SIZE_BYTES - 3U) {
            throw std::length_error{ "Message payload exceeds the protocol frame limit" };
        }
        bytes.reserve(3U + payload_size);
        bytes.push_back(PROTOCOL_MAGIC);
        bytes.push_back(PROTOCOL_VERSION);
        bytes.push_back(static_cast<uint8_t>(type));
    }

    std::vector<uint8_t> operator()(JoinRequestMessage const& message)
    {
        begin(MessageType::JoinRequest, 34U);
        bytes.push_back(static_cast<uint8_t>(message.ch));
        bytes.push_back(static_cast<uint8_t>(message.mode));
        appendUint32(bytes, message.configuration.algorithm_version);
        appendUint64(bytes, message.configuration.seed);
        bytes.push_back(message.configuration.chunk_width);
        bytes.push_back(message.configuration.chunk_height);
        bytes.push_back(message.configuration.chunk_depth);
        appendUint64(bytes, message.configuration.chunk_content_digest);
        bytes.push_back(static_cast<uint8_t>(message.wants_previews));
        appendUint64(bytes, message.join_epoch);
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(JoinResponseMessage const& message)
    {
        begin(MessageType::JoinResponse, 9U);
        bytes.push_back(static_cast<uint8_t>(message.accepted));
        appendUint64(bytes, message.join_epoch);
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ClientInputMessage const& message)
    {
        begin(MessageType::ClientInput, 13U);
        bytes.push_back(message.direction.x);
        bytes.push_back(message.direction.y);
        bytes.push_back(message.direction.z);
        bytes.push_back(static_cast<uint8_t>(message.direction.accelerated));
        appendUint16(bytes, message.direction.speedup);
        bytes.push_back(static_cast<uint8_t>(message.direction.cycle_movement_capabilities));
        bytes.push_back(static_cast<uint8_t>(message.direction.view_x));
        bytes.push_back(static_cast<uint8_t>(message.direction.view_y));
        appendUint32(bytes, message.sequence);
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ClientHeightTileCreditMessage const& message)
    {
        begin(MessageType::ClientHeightTileCredit,
            18U + message.not_resident_keys.size() * 2U * sizeof(int32_t));
        appendUint64(bytes, message.world_revision);
        appendUint64(bytes, message.delivery_token);
        bytes.push_back(message.credits);
        bytes.push_back(static_cast<uint8_t>(message.not_resident_keys.size()));
        for (HeightTileKey const key : message.not_resident_keys) {
            appendInt32(bytes, key.x);
            appendInt32(bytes, key.y);
        }
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ServerPlayerPositionMessage const& message)
    {
        begin(MessageType::ServerPlayerPosition, 33U);
        bytes.push_back(static_cast<uint8_t>(message.ch));
        bytes.push_back(message.palette_index);
        bytes.push_back(message.movement_capabilities.bits);
        appendInt32(bytes, message.x);
        appendInt32(bytes, message.y);
        appendInt32(bytes, message.z);
        appendUint16(bytes, message.x_subcell);
        appendUint16(bytes, message.y_subcell);
        appendUint16(bytes, message.z_subcell);
        appendInt32(bytes, message.vertical_velocity_subcells);
        appendUint32(bytes, message.acknowledged_input_sequence);
        appendUint32(bytes, message.state_revision);
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ServerRemovePlayerMessage const& message)
    {
        begin(MessageType::ServerRemovePlayer, 1U);
        bytes.push_back(static_cast<uint8_t>(message.ch));
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ServerHeightTileDescriptorMessage const& message)
    {
        begin(
            MessageType::ServerHeightTileDescriptor,
            39U + 22U + message.world_identity.configuration_identity.size()
        );
        appendUint32(bytes, message.configuration.algorithm_version);
        appendUint64(bytes, message.configuration.seed);
        bytes.push_back(message.configuration.chunk_width);
        bytes.push_back(message.configuration.chunk_height);
        bytes.push_back(message.configuration.chunk_depth);
        appendUint64(bytes, message.configuration.chunk_content_digest);
        appendTerrainIdentity(bytes, message.world_identity);
        appendUint64(bytes, message.world_revision);
        appendUint32(bytes, message.max_height_tiles);
        appendUint32(bytes, message.max_height_tile_bytes);
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ServerWorldRevisionMessage const& message)
    {
        begin(MessageType::ServerWorldRevision, 8U);
        appendUint64(bytes, message.world_revision);
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ServerHeightTileMessage const& message)
    {
        begin(MessageType::ServerHeightTile, heightTileEncodedSize(message));
        appendHeightTile(bytes, message);
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ServerHeightTileBatchMessage const& message)
    {
        uint64_t payload_size = 10U + 24U * message.removals.size();
        for (ServerHeightTileMessage const& tile : message.tiles) {
            payload_size += heightTileEncodedSize(tile);
        }
        begin(MessageType::ServerHeightTileBatch, payload_size);
        appendUint64(bytes, message.delivery_token);
        bytes.push_back(static_cast<uint8_t>(message.tiles.size()));
        bytes.push_back(static_cast<uint8_t>(message.removals.size()));
        for (ServerHeightTileMessage const& tile : message.tiles) {
            appendHeightTile(bytes, tile);
        }
        for (ServerRemoveHeightTileMessage const& removal : message.removals) {
            appendHeightTileRemoval(bytes, removal);
        }
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ServerRemoveHeightTileMessage const& message)
    {
        begin(MessageType::ServerRemoveHeightTile, 24U);
        appendHeightTileRemoval(bytes, message);
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ServerWorldPreparationMessage const& message)
    {
        begin(MessageType::ServerWorldPreparation, worldPreparationEncodedSize(message));
        appendTerrainSession(bytes, message.session);
        appendUint64(bytes, message.sequence);
        bytes.push_back(static_cast<uint8_t>(message.stage));
        appendUint32(bytes, message.work_completed);
        appendUint32(bytes, message.work_total);
        appendUint64(bytes, message.server_elapsed_microseconds);
        bytes.push_back(static_cast<uint8_t>(message.latest_snapshot != nullptr));
        if (message.latest_snapshot) {
            appendGenerationSnapshot(bytes, *message.latest_snapshot);
        }
        bytes.push_back(static_cast<uint8_t>(message.final_atlas_complete));
        bytes.push_back(static_cast<uint8_t>(message.refined_safe_spawn.has_value()));
        if (message.refined_safe_spawn) {
            appendPlayerPosition(bytes, *message.refined_safe_spawn);
        }
        appendBoundedString(bytes, message.failure);
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ServerMaterializedChunkBatchMessage const& message)
    {
        begin(MessageType::ServerMaterializedChunkBatch, encodedMaterializedChunkBatchSize(message) - 3U);
        appendTerrainSession(bytes, message.session);
        appendUint64(bytes, message.coverage_epoch);
        appendUint64(bytes, message.delivery_token);
        bytes.push_back(static_cast<uint8_t>(message.chunks.size()));
        for (MaterializedChunkPayload const& payload : message.chunks) {
            appendChunkCoordinate(bytes, payload.coordinate);
            appendUint64(bytes, payload.content_hash);
            if (auto const* const uniform = std::get_if<UniformChunkData>(&payload.data)) {
                bytes.push_back(0U);
                bytes.push_back(static_cast<uint8_t>(uniform->material));
            } else {
                bytes.push_back(1U);
                for (Block const block : std::get<DenseChunkData>(payload.data).blocks) {
                    bytes.push_back(static_cast<uint8_t>(block));
                }
            }
        }
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ClientChunkDeliveryAckMessage const& message)
    {
        begin(
            MessageType::ClientChunkDeliveryAck,
            terrainSessionEncodedSize(message.session) + 18U + 12U * message.not_resident_keys.size()
        );
        appendTerrainSession(bytes, message.session);
        appendUint64(bytes, message.coverage_epoch);
        appendUint64(bytes, message.delivery_token);
        bytes.push_back(message.credits);
        bytes.push_back(static_cast<uint8_t>(message.not_resident_keys.size()));
        for (ChunkCoordinate const coordinate : message.not_resident_keys) {
            appendChunkCoordinate(bytes, coordinate);
        }
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ServerAuthoritativeTerrainBatchMessage const& message)
    {
        begin(MessageType::ServerAuthoritativeTerrainBatch, encodedAuthoritativeTerrainBatchSize(message) - 3U);
        appendTerrainSession(bytes, message.session);
        appendUint64(bytes, message.coverage_epoch);
        appendUint64(bytes, message.delivery_token);
        bytes.push_back(static_cast<uint8_t>(message.tiles.size()));
        for (AuthoritativeTerrainTile const& tile : message.tiles) {
            appendChunkCoordinate(bytes, tile.coordinate);
            appendUint32(bytes, tile.block_sea_level);
            appendUint32(bytes, tile.max_height);
            appendUint64(bytes, tile.revision);
            for (AuthoritativeMaterialProfile const& column : tile.columns) {
                appendUint16(bytes, column.highest_solid_z);
                appendUint16(bytes, column.ground_first_z);
                appendUint16(bytes, column.ground_end_z);
                bytes.push_back(static_cast<uint8_t>(column.top_material));
                bytes.push_back(static_cast<uint8_t>(column.ground_material));
            }
        }
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ClientAuthoritativeTerrainAckMessage const& message)
    {
        begin(MessageType::ClientAuthoritativeTerrainAck,
            terrainSessionEncodedSize(message.session) + 17U + message.not_resident_keys.size() * 12U);
        appendTerrainSession(bytes, message.session);
        appendUint64(bytes, message.coverage_epoch);
        appendUint64(bytes, message.delivery_token);
        bytes.push_back(static_cast<uint8_t>(message.not_resident_keys.size()));
        for (ChunkCoordinate const coordinate : message.not_resident_keys) {
            appendChunkCoordinate(bytes, coordinate);
        }
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ClientStartWorldMessage const& message)
    {
        begin(MessageType::ClientStartWorld, terrainSessionEncodedSize(message.session));
        appendTerrainSession(bytes, message.session);
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ServerStartWorldAckMessage const& message)
    {
        begin(
            MessageType::ServerStartWorldAck,
            terrainSessionEncodedSize(message.session) + 3U + message.failure.size()
        );
        appendTerrainSession(bytes, message.session);
        bytes.push_back(static_cast<uint8_t>(message.accepted));
        appendBoundedString(bytes, message.failure);
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ServerChunkCoverageMessage const& message)
    {
        begin(MessageType::ServerChunkCoverage, terrainSessionEncodedSize(message.session) + 30U);
        appendTerrainSession(bytes, message.session);
        appendUint64(bytes, message.coverage_epoch);
        appendPlayerPosition(bytes, message.center);
        appendUint16(bytes, message.actual_radius_blocks);
        appendUint16(bytes, message.visual_lod_radius_blocks);
        return std::move(bytes);
    }
};

class MessageReader final {
public:
    explicit MessageReader(std::span<uint8_t const> const data) noexcept
        : m_data(data)
    {
    }

    [[nodiscard]]
    std::optional<uint8_t> readUint8() noexcept
    {
        if (m_position >= static_cast<uint64_t>(m_data.size())) {
            return std::nullopt;
        }
        return m_data.data()[m_position++];
    }

    [[nodiscard]]
    std::optional<uint16_t> readUint16() noexcept
    {
        auto const low = readUint8();
        auto const high = readUint8();
        if (!low || !high) {
            return std::nullopt;
        }
        uint32_t const combined = static_cast<uint32_t>(*low)
            | (static_cast<uint32_t>(*high) << 8U);
        return static_cast<uint16_t>(combined);
    }

    [[nodiscard]]
    std::optional<uint32_t> readUint32() noexcept
    {
        uint32_t result = 0;
        for (uint32_t byte{ 0 }; byte < 4; ++byte) {
            auto const value = readUint8();
            if (!value) {
                return std::nullopt;
            }
            result |= static_cast<uint32_t>(*value) << (byte * 8U);
        }
        return result;
    }

    [[nodiscard]]
    std::optional<uint64_t> readUint64() noexcept
    {
        uint64_t result = 0;
        for (uint32_t byte{ 0 }; byte < 8; ++byte) {
            auto const value = readUint8();
            if (!value) {
                return std::nullopt;
            }
            result |= static_cast<uint64_t>(*value) << (byte * 8U);
        }
        return result;
    }

    [[nodiscard]]
    std::optional<double> readDouble() noexcept
    {
        auto const value = readUint64();
        if (!value) {
            return std::nullopt;
        }
        return std::bit_cast<double>(*value);
    }

    [[nodiscard]]
    std::optional<float> readFloat() noexcept
    {
        auto const value = readUint32();
        if (!value) {
            return std::nullopt;
        }
        return std::bit_cast<float>(*value);
    }

    [[nodiscard]]
    std::optional<std::string> readString(
        uint16_t const length,
        uint16_t const maximum_length = TERRAIN_CONFIGURATION_IDENTITY_MAX_BYTES
    )
    {
        if (length > maximum_length || length > m_data.size() - m_position) {
            return std::nullopt;
        }
        std::string result;
        result.reserve(length);
        for (uint16_t index = 0U; index < length; ++index) {
            auto const value = readUint8();
            if (!value) {
                return std::nullopt;
            }
            result.push_back(static_cast<char>(*value));
        }
        return result;
    }

    [[nodiscard]]
    std::optional<int32_t> readInt32() noexcept
    {
        auto const value = readUint32();
        if (!value) {
            return std::nullopt;
        }
        return static_cast<int32_t>(*value);
    }

    [[nodiscard]]
    uint64_t remaining() const noexcept
    {
        return static_cast<uint64_t>(m_data.size()) - m_position;
    }

    [[nodiscard]]
    bool atEnd() const noexcept
    {
        return m_position == static_cast<uint64_t>(m_data.size());
    }
private:
    std::span<uint8_t const> m_data;
    uint64_t m_position = 0;
};

[[nodiscard]]
std::optional<TerrainIdentity> readTerrainIdentity(MessageReader& reader)
{
    auto const terrain_version = reader.readUint32();
    auto const revision = reader.readUint64();
    auto const seed = reader.readUint64();
    auto const configuration_identity_size = reader.readUint16();
    if (!terrain_version || !revision || !seed || !configuration_identity_size) {
        return std::nullopt;
    }
    auto const configuration_identity = reader.readString(*configuration_identity_size);
    if (!configuration_identity) {
        return std::nullopt;
    }
    return TerrainIdentity{
        .terrain_version = *terrain_version,
        .revision = *revision,
        .seed = *seed,
        .configuration_identity = *configuration_identity,
    };
}

[[nodiscard]]
std::optional<SurfaceTile> readSurfaceTile(MessageReader& reader, HeightTileKey key)
{
    auto const block_sea_level = reader.readUint32();
    auto const max_height = reader.readUint32();
    auto const identity = readTerrainIdentity(reader);
    if (!block_sea_level || !max_height || !identity) {
        return std::nullopt;
    }
    SurfaceTile surface{
        .coordinate = { .x = key.x, .y = key.y, .z = 0 },
        .block_sea_level = *block_sea_level,
        .max_height = *max_height,
        .identity = *identity,
    };
    for (TerrainColumn& column : surface.columns) {
        auto const highest_solid_z = reader.readUint16();
        auto const water_height_z = reader.readUint16();
        auto const water_present = reader.readUint8();
        auto const submerged = reader.readUint8();
        auto const geological_elevation_km = reader.readDouble();
        auto const temperature_celsius = reader.readDouble();
        auto const precipitation = reader.readDouble();
        auto const top_material = reader.readUint8();
        auto const ground_material = reader.readUint8();
        auto const ground_first_z = reader.readUint16();
        auto const ground_end_z = reader.readUint16();
        if (!highest_solid_z || !water_height_z || !water_present || *water_present > 1U
            || !submerged || *submerged > 1U || !geological_elevation_km || !temperature_celsius
            || !precipitation || !top_material || !ground_material || !ground_first_z || !ground_end_z) {
            return std::nullopt;
        }
        column = TerrainColumn{
            .highest_solid_z = *highest_solid_z,
            .water_height_z = *water_height_z,
            .water_present = *water_present == 1U,
            .submerged = *submerged == 1U,
            .geological_elevation_km = *geological_elevation_km,
            .temperature_celsius = *temperature_celsius,
            .precipitation = *precipitation,
            .top_material = static_cast<Block>(*top_material),
            .ground_material = static_cast<Block>(*ground_material),
            .ground_first_z = *ground_first_z,
            .ground_end_z = *ground_end_z,
        };
    }
    return surface;
}

[[nodiscard]]
std::optional<ServerHeightTileMessage> readHeightTile(MessageReader& reader)
{
    auto const x = reader.readInt32();
    auto const y = reader.readInt32();
    auto const revision = reader.readUint64();
    auto const token = reader.readUint64();
    if (!x || !y || !revision || !token) {
        return std::nullopt;
    }
    auto const bit_width = reader.readUint8();
    auto const base = reader.readUint16();
    if (!bit_width || !base || *bit_width > 16U) {
        return std::nullopt;
    }

    uint64_t bit_buffer = 0U;
    uint32_t buffered_bits = 0U;
    uint64_t const delta_mask = *bit_width == 0U
        ? 0U
        : (uint64_t{ 1U } << *bit_width) - 1U;
    std::array<uint16_t, HEIGHT_TILE_SAMPLE_COUNT> heights{};
    for (uint32_t index{ 0U }; index < HEIGHT_TILE_SAMPLE_COUNT; ++index) {
        uint32_t delta = 0U;
        while (buffered_bits < *bit_width) {
            auto const byte = reader.readUint8();
            if (!byte) {
                return std::nullopt;
            }
            bit_buffer |= static_cast<uint64_t>(*byte) << buffered_bits;
            buffered_bits += 8U;
        }
        delta = static_cast<uint32_t>(bit_buffer & delta_mask);
        if (*bit_width > 0U) {
            bit_buffer >>= *bit_width;
            buffered_bits -= *bit_width;
        }
        uint32_t const height = static_cast<uint32_t>(*base) + delta;
        if (height > std::numeric_limits<uint16_t>::max()) {
            return std::nullopt;
        }
        heights[index] = static_cast<uint16_t>(height);
    }
    HeightTileKey const key{ .x = *x, .y = *y };
    auto const surface = readSurfaceTile(reader, key);
    if (!surface) {
        return std::nullopt;
    }
    return ServerHeightTileMessage{
        .key = key,
        .revision = *revision,
        .token = *token,
        .heights = heights,
        .surface = *surface,
    };
}

[[nodiscard]]
std::optional<WorldConfiguration> readConfiguration(MessageReader& reader) noexcept
{
    auto const algorithm_version = reader.readUint32();
    auto const seed = reader.readUint64();
    auto const chunk_width = reader.readUint8();
    auto const chunk_height = reader.readUint8();
    auto const chunk_depth = reader.readUint8();
    auto const chunk_content_digest = reader.readUint64();
    if (!algorithm_version || !seed || !chunk_width || !chunk_height || !chunk_depth || !chunk_content_digest) {
        return std::nullopt;
    }
    return WorldConfiguration{
        .algorithm_version = *algorithm_version,
        .seed = *seed,
        .chunk_width = *chunk_width,
        .chunk_height = *chunk_height,
        .chunk_depth = *chunk_depth,
        .chunk_content_digest = *chunk_content_digest,
    };
}

[[nodiscard]]
std::optional<generation::GenerationConfiguration> readGenerationConfiguration(MessageReader& reader) noexcept
{
    auto const seed = reader.readUint64();
    auto const resolution = reader.readUint32();
    auto const epochs = reader.readUint32();
    auto const erosion_steps = reader.readUint32();
    auto const initial_climate_iterations = reader.readUint32();
    auto const climate_iterations = reader.readUint32();
    auto const currents = reader.readUint8();
    auto const wind_sign = reader.readUint8();
    auto const coastal_elaboration = reader.readUint8();
    if (!seed || !resolution || !epochs || !erosion_steps || !initial_climate_iterations
        || !climate_iterations || !currents || *currents > 1U || !wind_sign || !coastal_elaboration
        || *coastal_elaboration > 1U) {
        return std::nullopt;
    }
    return generation::GenerationConfiguration{
        .seed = *seed,
        .resolution = *resolution,
        .epochs = *epochs,
        .erosion_steps = *erosion_steps,
        .initial_climate_iterations = *initial_climate_iterations,
        .climate_iterations = *climate_iterations,
        .currents = *currents == 1U,
        .wind_sign = std::bit_cast<int8_t>(*wind_sign),
        .coastal_elaboration = *coastal_elaboration == 1U,
    };
}

[[nodiscard]]
std::optional<TerrainSessionIdentity> readTerrainSession(MessageReader& reader)
{
    auto const join_epoch = reader.readUint64();
    auto const terrain = readTerrainIdentity(reader);
    auto const configuration = readGenerationConfiguration(reader);
    if (!join_epoch || !terrain || !configuration) {
        return std::nullopt;
    }
    return TerrainSessionIdentity{
        .join_epoch = *join_epoch,
        .terrain = *terrain,
        .configuration = *configuration,
    };
}

[[nodiscard]]
std::optional<ChunkCoordinate> readChunkCoordinate(MessageReader& reader) noexcept
{
    auto const x = reader.readInt32();
    auto const y = reader.readInt32();
    auto const z = reader.readInt32();
    if (!x || !y || !z) {
        return std::nullopt;
    }
    return ChunkCoordinate{ .x = *x, .y = *y, .z = *z };
}

[[nodiscard]]
std::optional<PlayerPosition> readPlayerPosition(MessageReader& reader) noexcept
{
    auto const x = reader.readInt32();
    auto const y = reader.readInt32();
    auto const z = reader.readInt32();
    auto const x_subcell = reader.readUint16();
    auto const y_subcell = reader.readUint16();
    auto const z_subcell = reader.readUint16();
    if (!x || !y || !z || !x_subcell || !y_subcell || !z_subcell) {
        return std::nullopt;
    }
    return PlayerPosition{
        .x = *x,
        .y = *y,
        .z = *z,
        .x_subcell = *x_subcell,
        .y_subcell = *y_subcell,
        .z_subcell = *z_subcell,
    };
}

[[nodiscard]]
std::optional<std::vector<float>> readFloatVector(
    MessageReader& reader,
    uint32_t const sample_count
)
{
    auto const count = reader.readUint32();
    if (!count || (*count != 0U && *count != sample_count)
        || static_cast<uint64_t>(*count) * sizeof(float) > reader.remaining()) {
        return std::nullopt;
    }
    std::vector<float> values;
    values.reserve(*count);
    for (uint32_t index = 0U; index < *count; ++index) {
        auto const value = reader.readFloat();
        if (!value) {
            return std::nullopt;
        }
        values.push_back(*value);
    }
    return values;
}

[[nodiscard]]
std::optional<std::vector<uint8_t>> readByteVector(
    MessageReader& reader,
    uint32_t const sample_count
)
{
    auto const count = reader.readUint32();
    if (!count || (*count != 0U && *count != sample_count) || *count > reader.remaining()) {
        return std::nullopt;
    }
    std::vector<uint8_t> values;
    values.reserve(*count);
    for (uint32_t index = 0U; index < *count; ++index) {
        auto const value = reader.readUint8();
        if (!value) {
            return std::nullopt;
        }
        values.push_back(*value);
    }
    return values;
}

[[nodiscard]]
std::optional<generation::GenerationSnapshot> readGenerationSnapshot(MessageReader& reader)
{
    auto const stage = reader.readUint8();
    auto const resolution = reader.readUint32();
    if (!stage || !resolution
        || (*resolution != 256U && *resolution != MAX_GENERATION_SNAPSHOT_RESOLUTION)) {
        return std::nullopt;
    }
    uint32_t const sample_count = *resolution * *resolution;
    auto const elevation = readFloatVector(reader, sample_count);
    auto const temperature = readFloatVector(reader, sample_count);
    auto const precipitation = readFloatVector(reader, sample_count);
    auto const water_level = readFloatVector(reader, sample_count);
    auto const ocean_mask = readByteVector(reader, sample_count);
    auto const lake_mask = readByteVector(reader, sample_count);
    auto const river_mask = readByteVector(reader, sample_count);
    if (!elevation || !temperature || !precipitation || !water_level
        || !ocean_mask || !lake_mask || !river_mask) {
        return std::nullopt;
    }
    return generation::GenerationSnapshot{
        .stage = static_cast<generation::GenerationStage>(*stage),
        .resolution = *resolution,
        .elevation = *elevation,
        .temperature = *temperature,
        .precipitation = *precipitation,
        .water_level = *water_level,
        .ocean_mask = *ocean_mask,
        .lake_mask = *lake_mask,
        .river_mask = *river_mask,
    };
}

[[nodiscard]]
constexpr bool isValidGenerationStage(generation::GenerationStage const stage) noexcept
{
    return static_cast<uint8_t>(stage)
        <= static_cast<uint8_t>(generation::GenerationStage::Complete);
}

[[nodiscard]]
bool isValidUtf8(std::string const& value) noexcept
{
    uint32_t index = 0U;
    while (index < value.size()) {
        uint8_t const first = static_cast<uint8_t>(value[index]);
        if (first <= 0x7FU) {
            if (first < 0x20U && first != '\n' && first != '\r' && first != '\t') {
                return false;
            }
            ++index;
            continue;
        }

        uint32_t continuation_count = 0U;
        uint32_t code_point = 0U;
        uint32_t minimum_code_point = 0U;
        if ((first & 0xE0U) == 0xC0U) {
            continuation_count = 1U;
            code_point = first & 0x1FU;
            minimum_code_point = 0x80U;
        } else if ((first & 0xF0U) == 0xE0U) {
            continuation_count = 2U;
            code_point = first & 0x0FU;
            minimum_code_point = 0x800U;
        } else if ((first & 0xF8U) == 0xF0U) {
            continuation_count = 3U;
            code_point = first & 0x07U;
            minimum_code_point = 0x1'0000U;
        } else {
            return false;
        }
        if (continuation_count > value.size() - index - 1U) {
            return false;
        }
        for (uint32_t offset = 1U; offset <= continuation_count; ++offset) {
            uint8_t const continuation = static_cast<uint8_t>(value[index + offset]);
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3FU);
        }
        if (code_point < minimum_code_point || code_point > 0x10'FFFFU
            || (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

[[nodiscard]]
bool isValidTerrainSessionIdentity(TerrainSessionIdentity const& session) noexcept
{
    if (session.join_epoch == 0U || !isValidTerrainIdentity(session.terrain)
        || session.terrain.seed != session.configuration.seed) {
        return false;
    }
    try {
        return generation::configurationIdentity(session.configuration)
            == session.terrain.configuration_identity;
    } catch (...) {
        return false;
    }
}

[[nodiscard]]
bool isValidGenerationSnapshot(generation::GenerationSnapshot const& snapshot) noexcept
{
    if (!isValidGenerationStage(snapshot.stage)
        || (snapshot.resolution != 256U && snapshot.resolution != MAX_GENERATION_SNAPSHOT_RESOLUTION)) {
        return false;
    }
    uint32_t const sample_count = snapshot.resolution * snapshot.resolution;
    bool has_field = false;
    auto const valid_float_field = [&has_field, sample_count](std::vector<float> const& values) {
        if (values.empty()) {
            return true;
        }
        has_field = true;
        return values.size() == sample_count
            && std::ranges::all_of(values, [](float const value) { return std::isfinite(value); });
    };
    auto const valid_mask = [&has_field, sample_count](std::vector<uint8_t> const& values) {
        if (values.empty()) {
            return true;
        }
        has_field = true;
        return values.size() == sample_count
            && std::ranges::all_of(values, [](uint8_t const value) { return value <= 1U; });
    };
    return valid_float_field(snapshot.elevation)
        && valid_float_field(snapshot.temperature)
        && valid_float_field(snapshot.precipitation)
        && valid_float_field(snapshot.water_level)
        && valid_mask(snapshot.ocean_mask)
        && valid_mask(snapshot.lake_mask)
        && valid_mask(snapshot.river_mask)
        && has_field;
}

[[nodiscard]]
bool isValidWorldPreparation(ServerWorldPreparationMessage const& message) noexcept
{
    if (!isValidTerrainSessionIdentity(message.session) || message.sequence == 0U
        || !isValidGenerationStage(message.stage) || message.work_completed > message.work_total
        || (message.work_total == 0U && message.work_completed != 0U)
        || message.failure.size() > MAX_WORLD_PREPARATION_FAILURE_BYTES
        || !isValidUtf8(message.failure)) {
        return false;
    }
    if (message.latest_snapshot
        && (!isValidGenerationSnapshot(*message.latest_snapshot)
            || static_cast<uint8_t>(message.latest_snapshot->stage) > static_cast<uint8_t>(message.stage))) {
        return false;
    }
    if (message.final_atlas_complete
        && (message.stage != generation::GenerationStage::Complete || !message.failure.empty()
            || message.work_total == 0U || message.work_completed != message.work_total)) {
        return false;
    }
    if (message.stage == generation::GenerationStage::Complete
        && !message.final_atlas_complete && message.failure.empty()) {
        return false;
    }
    return !message.refined_safe_spawn
        || (message.final_atlas_complete && World::isFlightPositionInBounds(*message.refined_safe_spawn));
}

[[nodiscard]]
bool isValidMaterializedChunkBatch(ServerMaterializedChunkBatchMessage const& message)
{
    if (!isValidTerrainSessionIdentity(message.session) || message.coverage_epoch == 0U
        || message.delivery_token == 0U
        || message.chunks.empty() || message.chunks.size() > MATERIALIZED_CHUNK_BATCH_CAPACITY) {
        return false;
    }
    for (auto current = message.chunks.cbegin(); current != message.chunks.cend(); ++current) {
        if (!materializeChunkPayload(*current)) {
            return false;
        }
        if (std::ranges::find(message.chunks.cbegin(), current, current->coordinate,
                &MaterializedChunkPayload::coordinate) != current) {
            return false;
        }
    }
    return true;
}

[[nodiscard]]
bool isValidChunkDeliveryAck(ClientChunkDeliveryAckMessage const& message) noexcept
{
    if (!isValidTerrainSessionIdentity(message.session) || message.coverage_epoch == 0U
        || message.credits == 0U
        || message.credits > MATERIALIZED_CHUNK_DELIVERY_WINDOW
        || message.not_resident_keys.size() > MATERIALIZED_CHUNK_RETRY_KEY_CAPACITY
        || (message.delivery_token == 0U
            && (message.credits != MATERIALIZED_CHUNK_DELIVERY_WINDOW || !message.not_resident_keys.empty()))
        || (message.delivery_token != 0U && message.credits != 1U)) {
        return false;
    }
    for (auto current = message.not_resident_keys.cbegin();
         current != message.not_resident_keys.cend();
         ++current) {
        if (!WorldBounds::isValidChunk(*current)
            || std::ranges::find(message.not_resident_keys.cbegin(), current, *current) != current) {
            return false;
        }
    }
    return true;
}

[[nodiscard]]
bool isValidChunkCoverage(ServerChunkCoverageMessage const& message) noexcept
{
    return isValidTerrainSessionIdentity(message.session) && message.coverage_epoch != 0U
        && World::isFlightPositionInBounds(message.center)
        && message.actual_radius_blocks == ACTUAL_TERRAIN_RADIUS_BLOCKS
        && message.visual_lod_radius_blocks == FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS;
}

[[nodiscard]]
bool isValidHeightTile(ServerHeightTileMessage const& message) noexcept
{
    if (message.key != normalizeHeightTileKey(message.key)
        || message.revision == 0U || message.token == 0U
        || message.surface.coordinate != ChunkCoordinate{
            .x = message.key.x,
            .y = message.key.y,
            .z = 0,
        }
        || message.surface.identity.revision != message.revision
        || !isValidSurfaceTile(message.surface)) {
        return false;
    }
    for (uint32_t index = 0U; index < message.heights.size(); ++index) {
        uint32_t const expected_height = static_cast<uint32_t>(message.surface.columns[index].highest_solid_z) + 1U;
        if (message.heights[index] != expected_height || message.heights[index] > WorldExtent::DEPTH) {
            return false;
        }
    }
    return true;
}

[[nodiscard]]
bool isValidMessage(Message const& message) noexcept
{
    if (message.valueless_by_exception()) {
        return false;
    }
    auto const validator = [](auto const& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, JoinRequestMessage>) {
            return isValidCharacter(value.ch) && isValidMode(value.mode)
                && isValidWorldConfiguration(value.configuration) && value.join_epoch != 0U;
        } else if constexpr (std::is_same_v<Value, JoinResponseMessage>) {
            return value.join_epoch != 0U;
        } else if constexpr (std::is_same_v<Value, ClientInputMessage>) {
            return isValidDirection(value.direction.x) && isValidDirection(value.direction.y)
                && isValidDirection(value.direction.z)
                && isValidDirection(static_cast<uint8_t>(value.direction.view_x))
                && isValidDirection(static_cast<uint8_t>(value.direction.view_y));
        } else if constexpr (std::is_same_v<Value, ClientHeightTileCreditMessage>) {
            if (value.world_revision == 0U || value.credits == 0U
                || value.credits > HEIGHT_TILE_DELIVERY_WINDOW
                || value.not_resident_keys.size() > HEIGHT_TILE_RESIDENCY_RECEIPT_KEY_CAPACITY
                || (value.delivery_token == 0U && !value.not_resident_keys.empty())) {
                return false;
            }
            for (auto current = value.not_resident_keys.cbegin();
                 current != value.not_resident_keys.cend();
                 ++current) {
                if (*current != normalizeHeightTileKey(*current)
                    || std::ranges::find(value.not_resident_keys.cbegin(), current, *current)
                        != current) {
                    return false;
                }
            }
            return true;
        } else if constexpr (std::is_same_v<Value, ServerPlayerPositionMessage>) {
            return isValidCharacter(value.ch) && isValidPlayerPaletteIndex(value.palette_index)
                && isValidMovementCapabilities(value.movement_capabilities)
                && World::isFlightPositionInBounds(PlayerPosition{
                .x = value.x,
                .y = value.y,
                .z = value.z,
                .x_subcell = value.x_subcell,
                .y_subcell = value.y_subcell,
                .z_subcell = value.z_subcell,
            });
        } else if constexpr (std::is_same_v<Value, ServerRemovePlayerMessage>) {
            return isValidCharacter(value.ch);
        } else if constexpr (std::is_same_v<Value, ServerHeightTileDescriptorMessage>) {
            return isValidWorldConfiguration(value.configuration)
                && value.world_revision != 0U
                && value.world_identity.revision == value.world_revision
                && value.world_identity.seed == value.configuration.seed
                && isValidTerrainIdentity(value.world_identity)
                && heightTileInterestRadiusForCount(value.max_height_tiles).has_value()
                && value.max_height_tile_bytes == HEIGHT_TILE_PAYLOAD_BYTES;
        } else if constexpr (std::is_same_v<Value, ServerWorldRevisionMessage>) {
            return value.world_revision != 0U;
        } else if constexpr (std::is_same_v<Value, ServerHeightTileMessage>) {
            return isValidHeightTile(value);
        } else if constexpr (std::is_same_v<Value, ServerHeightTileBatchMessage>) {
            return value.delivery_token != 0U
                && (!value.tiles.empty() || !value.removals.empty())
                && value.tiles.size() + value.removals.size() <= HEIGHT_TILE_BATCH_CAPACITY
                && std::ranges::all_of(value.tiles, [](ServerHeightTileMessage const& tile) {
                    return isValidHeightTile(tile);
                })
                && std::ranges::all_of(value.removals, [](ServerRemoveHeightTileMessage const& removal) {
                    return removal.key == normalizeHeightTileKey(removal.key)
                        && removal.revision != 0U && removal.token != 0U;
                });
        } else if constexpr (std::is_same_v<Value, ServerWorldPreparationMessage>) {
            return isValidWorldPreparation(value);
        } else if constexpr (std::is_same_v<Value, ServerMaterializedChunkBatchMessage>) {
            return isValidMaterializedChunkBatch(value);
        } else if constexpr (std::is_same_v<Value, ClientChunkDeliveryAckMessage>) {
            return isValidChunkDeliveryAck(value);
        } else if constexpr (std::is_same_v<Value, ServerAuthoritativeTerrainBatchMessage>) {
            if (!isValidTerrainSessionIdentity(value.session) || value.coverage_epoch == 0U
                || value.delivery_token == 0U || value.tiles.empty()
                || value.tiles.size() > AUTHORITATIVE_TERRAIN_BATCH_CAPACITY) {
                return false;
            }
            for (auto tile = value.tiles.begin(); tile != value.tiles.end(); ++tile) {
                if (!WorldBounds::isValidChunk(tile->coordinate) || tile->coordinate.z != 0
                    || tile->identity != value.session.terrain || tile->revision == 0U
                    || tile->block_sea_level > tile->max_height || tile->max_height >= WorldExtent::DEPTH
                    || std::ranges::find(value.tiles.begin(), tile, tile->coordinate,
                        &AuthoritativeTerrainTile::coordinate) != tile) {
                    return false;
                }
                for (auto const& column : tile->columns) {
                    if (column.highest_solid_z > tile->max_height
                        || column.ground_first_z > column.ground_end_z
                        || column.ground_end_z > WorldExtent::DEPTH
                        || (column.ground_first_z != column.ground_end_z
                            && column.ground_end_z > column.highest_solid_z + 1U)
                        || !isSolidBlock(column.top_material) || !isSolidBlock(column.ground_material)) {
                        return false;
                    }
                }
            }
            return true;
        } else if constexpr (std::is_same_v<Value, ClientAuthoritativeTerrainAckMessage>) {
            if (!isValidTerrainSessionIdentity(value.session) || value.coverage_epoch == 0U
                || value.delivery_token == 0U
                || value.not_resident_keys.size() > AUTHORITATIVE_TERRAIN_BATCH_CAPACITY) {
                return false;
            }
            for (auto key = value.not_resident_keys.begin(); key != value.not_resident_keys.end(); ++key) {
                if (!WorldBounds::isValidChunk(*key) || key->z != 0
                    || std::find(value.not_resident_keys.begin(), key, *key) != key) {
                    return false;
                }
            }
            return true;
        } else if constexpr (std::is_same_v<Value, ClientStartWorldMessage>) {
            return isValidTerrainSessionIdentity(value.session);
        } else if constexpr (std::is_same_v<Value, ServerStartWorldAckMessage>) {
            return isValidTerrainSessionIdentity(value.session)
                && value.failure.size() <= MAX_WORLD_PREPARATION_FAILURE_BYTES
                && isValidUtf8(value.failure)
                && (value.accepted ? value.failure.empty() : !value.failure.empty());
        } else if constexpr (std::is_same_v<Value, ServerChunkCoverageMessage>) {
            return isValidChunkCoverage(value);
        } else {
            return value.key == normalizeHeightTileKey(value.key)
                && value.revision != 0U && value.token != 0U;
        }
    };
    try {
        return std::visit(validator, message);
    } catch (std::bad_variant_access const&) {
        return false;
    }
}

void decodeServerHeightTileMessage(MessageReader& reader, std::optional<Message>& message)
{
    auto tile = readHeightTile(reader);
    if (tile) {
        message = std::move(*tile);
    }
}

void decodeServerHeightTileBatchMessage(MessageReader& reader, std::optional<Message>& message)
{
    auto const delivery_token = reader.readUint64();
    auto const tile_count = reader.readUint8();
    auto const removal_count = reader.readUint8();
    if (!delivery_token || !tile_count || !removal_count
        || (*tile_count == 0U && *removal_count == 0U)
        || *tile_count + *removal_count > HEIGHT_TILE_BATCH_CAPACITY) {
        return;
    }
    std::vector<ServerHeightTileMessage> tiles;
    tiles.reserve(*tile_count);
    for (uint8_t index = 0U; index < *tile_count; ++index) {
        auto tile = readHeightTile(reader);
        if (!tile) {
            break;
        }
        tiles.push_back(std::move(*tile));
    }
    std::vector<ServerRemoveHeightTileMessage> removals;
    removals.reserve(*removal_count);
    for (uint8_t index = 0U; tiles.size() == *tile_count && index < *removal_count; ++index) {
        auto const x = reader.readInt32();
        auto const y = reader.readInt32();
        auto const revision = reader.readUint64();
        auto const token = reader.readUint64();
        if (!x || !y || !revision || !token) {
            break;
        }
        removals.push_back({
            .key = {.x = *x, .y = *y},
            .revision = *revision,
            .token = *token,
        });
    }
    if (tiles.size() == *tile_count && removals.size() == *removal_count) {
        message = ServerHeightTileBatchMessage{
            .delivery_token = *delivery_token,
            .tiles = std::move(tiles),
            .removals = std::move(removals),
        };
    }
}

void decodeServerMaterializedChunkBatchMessage(MessageReader& reader, std::optional<Message>& message)
{
    auto const session = readTerrainSession(reader);
    auto const coverage_epoch = reader.readUint64();
    auto const delivery_token = reader.readUint64();
    auto const chunk_count = reader.readUint8();
    if (!session || !coverage_epoch || !delivery_token || !chunk_count || *chunk_count == 0U
        || *chunk_count > MATERIALIZED_CHUNK_BATCH_CAPACITY) {
        return;
    }
    std::vector<MaterializedChunkPayload> chunks;
    chunks.reserve(*chunk_count);
    for (uint8_t index = 0U; index < *chunk_count; ++index) {
        auto const coordinate = readChunkCoordinate(reader);
        auto const content_hash = reader.readUint64();
        auto const encoding = reader.readUint8();
        if (!coordinate || !content_hash || !encoding) {
            break;
        }
        MaterializedChunkPayload payload{
            .coordinate = *coordinate,
            .content_hash = *content_hash,
        };
        if (*encoding == 0U) {
            auto const material = reader.readUint8();
            if (!material) {
                break;
            }
            payload.data = UniformChunkData{ .material = static_cast<Block>(*material) };
        } else if (*encoding == 1U) {
            auto& dense = payload.data.emplace<DenseChunkData>();
            bool complete = true;
            for (Block& block : dense.blocks) {
                auto const material = reader.readUint8();
                if (!material) {
                    complete = false;
                    break;
                }
                block = static_cast<Block>(*material);
            }
            if (!complete) {
                break;
            }
        } else {
            break;
        }
        chunks.push_back(payload);
    }
    if (chunks.size() == *chunk_count) {
        message = ServerMaterializedChunkBatchMessage{
            .session = *session,
            .coverage_epoch = *coverage_epoch,
            .delivery_token = *delivery_token,
            .chunks = std::move(chunks),
        };
    }
}

void decodeServerAuthoritativeTerrainBatchMessage(MessageReader& reader, std::optional<Message>& message)
{
    auto const session = readTerrainSession(reader);
    auto const epoch = reader.readUint64();
    auto const token = reader.readUint64();
    auto const count = reader.readUint8();
    if (!session || !epoch || !token || !count || *count == 0U
        || *count > AUTHORITATIVE_TERRAIN_BATCH_CAPACITY) {
        return;
    }
    std::vector<AuthoritativeTerrainTile> tiles;
    for (uint32_t index = 0U; index < *count; ++index) {
        auto const coordinate = readChunkCoordinate(reader);
        auto const sea = reader.readUint32();
        auto const height = reader.readUint32();
        auto const revision = reader.readUint64();
        if (!coordinate || !sea || !height || !revision) {
            break;
        }
        AuthoritativeTerrainTile tile{
            .coordinate = *coordinate,
            .identity = session->terrain,
            .block_sea_level = *sea,
            .max_height = *height,
            .revision = *revision,
        };
        bool complete = true;
        for (auto& column : tile.columns) {
            auto const highest = reader.readUint16();
            auto const first = reader.readUint16();
            auto const end = reader.readUint16();
            auto const top = reader.readUint8();
            auto const ground = reader.readUint8();
            if (!highest || !first || !end || !top || !ground) {
                complete = false;
                break;
            }
            column = { *highest, *first, *end, static_cast<Block>(*top), static_cast<Block>(*ground) };
        }
        if (!complete) {
            break;
        }
        tiles.push_back(std::move(tile));
    }
    if (tiles.size() == *count) {
        message = ServerAuthoritativeTerrainBatchMessage{
            .session = *session,
            .coverage_epoch = *epoch,
            .delivery_token = *token,
            .tiles = std::move(tiles),
        };
    }
}

} // namespace

std::optional<Chunk> materializeChunkPayload(MaterializedChunkPayload const& payload)
{
    if (!WorldBounds::isValidChunk(payload.coordinate) || payload.data.valueless_by_exception()) {
        return std::nullopt;
    }
    Chunk::Blocks blocks{};
    if (auto const* const uniform = std::get_if<UniformChunkData>(&payload.data)) {
        if (!isValidBlock(uniform->material)) {
            return std::nullopt;
        }
        blocks.fill(uniform->material);
    } else {
        blocks = std::get<DenseChunkData>(payload.data).blocks;
        if (!std::ranges::all_of(blocks, [](Block const block) { return isValidBlock(block); })) {
            return std::nullopt;
        }
    }
    try {
        Chunk const chunk{ payload.coordinate, blocks };
        if (chunk.contentIdentity().content_hash != payload.content_hash) {
            return std::nullopt;
        }
        return chunk;
    } catch (std::exception const&) {
        return std::nullopt;
    }
}

uint32_t encodedHeightTileSize(ServerHeightTileMessage const& message) noexcept
{
    return heightTileEncodedSize(message);
}

uint64_t encodedMaterializedChunkBatchSize(ServerMaterializedChunkBatchMessage const& message) noexcept
{
    uint64_t size = 3U + terrainSessionEncodedSize(message.session) + 17U;
    for (MaterializedChunkPayload const& payload : message.chunks) {
        size += 21U + (std::holds_alternative<DenseChunkData>(payload.data) ? Chunk::BLOCK_COUNT : 1U);
    }
    return size;
}

uint64_t encodedAuthoritativeTerrainBatchSize(ServerAuthoritativeTerrainBatchMessage const& message) noexcept
{
    return 3U + terrainSessionEncodedSize(message.session) + 17U + message.tiles.size() * (28U + 2'048U);
}

std::vector<uint8_t> encodeMessage(Message message)
{
    return std::visit(MessageEncoder{}, std::move(message));
}

std::optional<Message> decodeMessage(std::span<uint8_t const> const data)
{
    if (data.size() > MAX_MESSAGE_SIZE_BYTES) {
        return std::nullopt;
    }
    MessageReader reader{ data };
    auto const magic = reader.readUint8();
    auto const version = reader.readUint8();
    auto const type_value = reader.readUint8();
    if (!magic || !version || !type_value || *magic != PROTOCOL_MAGIC || *version != PROTOCOL_VERSION) {
        return std::nullopt;
    }

    std::optional<Message> message;
    switch (static_cast<MessageType>(*type_value)) {
        case MessageType::JoinRequest: {
            auto const character = reader.readUint8();
            auto const mode = reader.readUint8();
            auto const configuration = readConfiguration(reader);
            auto const wants_previews = reader.readUint8();
            auto const join_epoch = reader.readUint64();
            if (character && mode && configuration && wants_previews && *wants_previews <= 1U && join_epoch) {
                message = JoinRequestMessage{
                    .ch = static_cast<char>(*character),
                    .mode = static_cast<WorldMode>(*mode),
                    .configuration = *configuration,
                    .wants_previews = *wants_previews == 1U,
                    .join_epoch = *join_epoch,
                };
            }
            break;
        }
        case MessageType::JoinResponse: {
            auto const accepted = reader.readUint8();
            auto const join_epoch = reader.readUint64();
            if (!accepted || *accepted > 1 || !join_epoch) {
                return std::nullopt;
            }
            message = JoinResponseMessage{ .accepted = *accepted == 1, .join_epoch = *join_epoch };
            break;
        }
        case MessageType::ClientInput: {
            auto const x = reader.readUint8();
            auto const y = reader.readUint8();
            auto const z = reader.readUint8();
            auto const accelerated = reader.readUint8();
            auto const speedup = reader.readUint16();
            auto const cycle_movement_capabilities = reader.readUint8();
            auto const view_x = reader.readUint8();
            auto const view_y = reader.readUint8();
            auto const sequence = reader.readUint32();
            if (x && y && z && accelerated && *accelerated <= 1U && speedup
                && cycle_movement_capabilities && *cycle_movement_capabilities <= 1U
                && isFlightSpeedupProfile(*speedup) && view_x && view_y && sequence) {
                message = ClientInputMessage{
                    .direction = {
                        .x = *x,
                        .y = *y,
                        .z = *z,
                        .accelerated = *accelerated == 1U,
                        .speedup = *speedup,
                        .cycle_movement_capabilities = *cycle_movement_capabilities == 1U,
                        .view_x = static_cast<int8_t>(*view_x),
                        .view_y = static_cast<int8_t>(*view_y),
                    },
                    .sequence = *sequence,
                };
            }
            break;
        }
        case MessageType::ClientHeightTileCredit: {
            auto const world_revision = reader.readUint64();
            auto const delivery_token = reader.readUint64();
            auto const credits = reader.readUint8();
            auto const not_resident_count = reader.readUint8();
            if (world_revision && delivery_token && credits && not_resident_count
                && *not_resident_count <= HEIGHT_TILE_RESIDENCY_RECEIPT_KEY_CAPACITY) {
                std::vector<HeightTileKey> not_resident_keys;
                not_resident_keys.reserve(*not_resident_count);
                bool complete = true;
                for (uint8_t index = 0U; index < *not_resident_count; ++index) {
                    auto const x = reader.readInt32();
                    auto const y = reader.readInt32();
                    if (!x || !y) {
                        complete = false;
                        break;
                    }
                    not_resident_keys.push_back({ .x = *x, .y = *y });
                }
                if (complete) {
                    message = ClientHeightTileCreditMessage{
                        .world_revision = *world_revision,
                        .delivery_token = *delivery_token,
                        .credits = *credits,
                        .not_resident_keys = std::move(not_resident_keys),
                    };
                }
            }
            break;
        }
        case MessageType::ServerPlayerPosition: {
            auto const character = reader.readUint8();
            auto const palette_index = reader.readUint8();
            auto const movement_capabilities = reader.readUint8();
            auto const x = reader.readInt32();
            auto const y = reader.readInt32();
            auto const z = reader.readInt32();
            auto const x_subcell = reader.readUint16();
            auto const y_subcell = reader.readUint16();
            auto const z_subcell = reader.readUint16();
            auto const vertical_velocity_subcells = reader.readInt32();
            auto const acknowledged_input_sequence = reader.readUint32();
            auto const state_revision = reader.readUint32();
            if (character && palette_index && movement_capabilities && x && y && z && x_subcell && y_subcell && z_subcell
                && vertical_velocity_subcells && acknowledged_input_sequence && state_revision) {
                message = ServerPlayerPositionMessage{
                    .ch = static_cast<char>(*character),
                    .palette_index = *palette_index,
                    .movement_capabilities = { .bits = *movement_capabilities },
                    .x = *x,
                    .y = *y,
                    .z = *z,
                    .x_subcell = *x_subcell,
                    .y_subcell = *y_subcell,
                    .z_subcell = *z_subcell,
                    .vertical_velocity_subcells = *vertical_velocity_subcells,
                    .acknowledged_input_sequence = *acknowledged_input_sequence,
                    .state_revision = *state_revision,
                };
            }
            break;
        }
        case MessageType::ServerRemovePlayer: {
            auto const character = reader.readUint8();
            if (character) {
                message = ServerRemovePlayerMessage{ .ch = static_cast<char>(*character) };
            }
            break;
        }
        case MessageType::ServerHeightTileDescriptor: {
            auto const configuration = readConfiguration(reader);
            auto const world_identity = readTerrainIdentity(reader);
            auto const world_revision = reader.readUint64();
            auto const max_height_tiles = reader.readUint32();
            auto const max_height_tile_bytes = reader.readUint32();
            if (configuration && world_identity && world_revision && max_height_tiles && max_height_tile_bytes) {
                message = ServerHeightTileDescriptorMessage{
                    .configuration = *configuration,
                    .world_identity = *world_identity,
                    .world_revision = *world_revision,
                    .max_height_tiles = *max_height_tiles,
                    .max_height_tile_bytes = *max_height_tile_bytes,
                };
            }
            break;
        }
        case MessageType::ServerWorldRevision: {
            if (auto const world_revision = reader.readUint64()) {
                message = ServerWorldRevisionMessage{ .world_revision = *world_revision };
            }
            break;
        }
        case MessageType::ServerHeightTile: {
            decodeServerHeightTileMessage(reader, message);
            break;
        }
        case MessageType::ServerHeightTileBatch: {
            decodeServerHeightTileBatchMessage(reader, message);
            break;
        }
        case MessageType::ServerRemoveHeightTile: {
            auto const x = reader.readInt32();
            auto const y = reader.readInt32();
            auto const revision = reader.readUint64();
            auto const token = reader.readUint64();
            if (x && y && revision && token) {
                message = ServerRemoveHeightTileMessage{
                    .key = { .x = *x, .y = *y },
                    .revision = *revision,
                    .token = *token,
                };
            }
            break;
        }
        case MessageType::ServerWorldPreparation: {
            auto const session = readTerrainSession(reader);
            auto const sequence = reader.readUint64();
            auto const stage = reader.readUint8();
            auto const work_completed = reader.readUint32();
            auto const work_total = reader.readUint32();
            auto const server_elapsed_microseconds = reader.readUint64();
            auto const has_snapshot = reader.readUint8();
            if (!session || !sequence || !stage || !work_completed || !work_total
                || !server_elapsed_microseconds || !has_snapshot || *has_snapshot > 1U) {
                break;
            }
            std::shared_ptr<generation::GenerationSnapshot const> latest_snapshot;
            if (*has_snapshot == 1U) {
                auto snapshot = readGenerationSnapshot(reader);
                if (!snapshot) {
                    break;
                }
                latest_snapshot = std::make_shared<generation::GenerationSnapshot const>(std::move(*snapshot));
            }
            auto const final_atlas_complete = reader.readUint8();
            auto const has_safe_spawn = reader.readUint8();
            std::optional<PlayerPosition> refined_safe_spawn;
            if (!final_atlas_complete || *final_atlas_complete > 1U || !has_safe_spawn || *has_safe_spawn > 1U) {
                break;
            }
            if (*has_safe_spawn == 1U) {
                refined_safe_spawn = readPlayerPosition(reader);
                if (!refined_safe_spawn) {
                    break;
                }
            }
            auto const failure_size = reader.readUint16();
            if (!failure_size) {
                break;
            }
            auto const failure = reader.readString(*failure_size, MAX_WORLD_PREPARATION_FAILURE_BYTES);
            if (!failure) {
                break;
            }
            message = ServerWorldPreparationMessage{
                .session = *session,
                .sequence = *sequence,
                .stage = static_cast<generation::GenerationStage>(*stage),
                .work_completed = *work_completed,
                .work_total = *work_total,
                .server_elapsed_microseconds = *server_elapsed_microseconds,
                .latest_snapshot = std::move(latest_snapshot),
                .final_atlas_complete = *final_atlas_complete == 1U,
                .refined_safe_spawn = refined_safe_spawn,
                .failure = *failure,
            };
            break;
        }
        case MessageType::ServerMaterializedChunkBatch: {
            decodeServerMaterializedChunkBatchMessage(reader, message);
            break;
        }
        case MessageType::ClientChunkDeliveryAck: {
            auto const session = readTerrainSession(reader);
            auto const coverage_epoch = reader.readUint64();
            auto const delivery_token = reader.readUint64();
            auto const credits = reader.readUint8();
            auto const key_count = reader.readUint8();
            if (!session || !coverage_epoch || !delivery_token || !credits || !key_count
                || *key_count > MATERIALIZED_CHUNK_RETRY_KEY_CAPACITY) {
                break;
            }
            std::vector<ChunkCoordinate> not_resident_keys;
            not_resident_keys.reserve(*key_count);
            bool complete = true;
            for (uint8_t index = 0U; index < *key_count; ++index) {
                auto const coordinate = readChunkCoordinate(reader);
                if (!coordinate) {
                    complete = false;
                    break;
                }
                not_resident_keys.push_back(*coordinate);
            }
            if (complete) {
                message = ClientChunkDeliveryAckMessage{
                    .session = *session,
                    .coverage_epoch = *coverage_epoch,
                    .delivery_token = *delivery_token,
                    .credits = *credits,
                    .not_resident_keys = std::move(not_resident_keys),
                };
            }
            break;
        }
        case MessageType::ServerAuthoritativeTerrainBatch: {
            decodeServerAuthoritativeTerrainBatchMessage(reader, message);
            break;
        }
        case MessageType::ClientAuthoritativeTerrainAck: {
            auto const session = readTerrainSession(reader);
            auto const epoch = reader.readUint64();
            auto const token = reader.readUint64();
            auto const count = reader.readUint8();
            if (!session || !epoch || !token || !count || *count > AUTHORITATIVE_TERRAIN_BATCH_CAPACITY) {
                break;
            }
            std::vector<ChunkCoordinate> keys;
            for (uint32_t index = 0U; index < *count; ++index) {
                auto const key = readChunkCoordinate(reader);
                if (!key) {
                    break;
                }
                keys.push_back(*key);
            }
            if (keys.size() == *count) {
                message = ClientAuthoritativeTerrainAckMessage{
                    .session = *session, .coverage_epoch = *epoch,
                    .delivery_token = *token, .not_resident_keys = std::move(keys),
                };
            }
            break;
        }
        case MessageType::ClientStartWorld: {
            if (auto const session = readTerrainSession(reader)) {
                message = ClientStartWorldMessage{ .session = *session };
            }
            break;
        }
        case MessageType::ServerStartWorldAck: {
            auto const session = readTerrainSession(reader);
            auto const accepted = reader.readUint8();
            auto const failure_size = reader.readUint16();
            if (!session || !accepted || *accepted > 1U || !failure_size) {
                break;
            }
            auto const failure = reader.readString(*failure_size, MAX_WORLD_PREPARATION_FAILURE_BYTES);
            if (failure) {
                message = ServerStartWorldAckMessage{
                    .session = *session,
                    .accepted = *accepted == 1U,
                    .failure = *failure,
                };
            }
            break;
        }
        case MessageType::ServerChunkCoverage: {
            auto const session = readTerrainSession(reader);
            auto const coverage_epoch = reader.readUint64();
            auto const center = readPlayerPosition(reader);
            auto const actual_radius_blocks = reader.readUint16();
            auto const visual_lod_radius_blocks = reader.readUint16();
            if (session && coverage_epoch && center && actual_radius_blocks && visual_lod_radius_blocks) {
                message = ServerChunkCoverageMessage{
                    .session = *session,
                    .coverage_epoch = *coverage_epoch,
                    .center = *center,
                    .actual_radius_blocks = *actual_radius_blocks,
                    .visual_lod_radius_blocks = *visual_lod_radius_blocks,
                };
            }
            break;
        }
        default: return std::nullopt;
    }
    if (!message || !reader.atEnd() || !isValidMessage(*message)) {
        return std::nullopt;
    }
    return message;
}

} // namespace shared
